# syntax=docker/dockerfile:1

# ------------------------------------------------------------------- build
# C++23 needs GCC 13 or newer; trixie ships 14. This stage carries the whole
# toolchain and is thrown away.
FROM debian:trixie-slim AS build

RUN apt-get update \
 && apt-get install -y --no-install-recommends g++ make cmake \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY src src

# RelWithDebInfo, not Release: it is the optimisation level CI tests and the
# benchmarks were taken at. The debug info is stripped afterwards, so it costs
# nothing in the image.
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=OFF \
 && cmake --build build -j"$(nproc)" \
 && strip build/mnemos-server build/mnemos-mcp build/mnemos-kafka

# ----------------------------------------------------------------- runtime
# Same base as the build, so the libstdc++ the binaries were linked against is
# the one they find. Nothing else is installed: zero dependencies holds here too.
FROM debian:trixie-slim

RUN useradd --system --uid 10001 --home-dir /data --shell /usr/sbin/nologin mnemos \
 && mkdir /data \
 && chown mnemos:mnemos /data

COPY --from=build /src/build/mnemos-server /src/build/mnemos-mcp /src/build/mnemos-kafka /usr/local/bin/

USER mnemos
WORKDIR /data
VOLUME /data
EXPOSE 6380 9092

# An inline PING over bash's /dev/tcp, since the image has no redis-cli.
# NOAUTH still proves the server is up and answering when --requirepass is set.
# Assumes the default port; a container started with --port needs its own check.
HEALTHCHECK --interval=5s --timeout=3s --start-period=2s --retries=5 \
    CMD bash -c 'exec 3<>/dev/tcp/127.0.0.1/6380 && printf "PING\r\n" >&3 && read -r -t 2 reply <&3 && [[ $reply == +PONG* || $reply == -NOAUTH* ]]'

# The default bind is 127.0.0.1, which inside a container is unreachable from
# anywhere else. Publish the port to the host's loopback (-p 127.0.0.1:6380:6380)
# to keep the same exposure the bare binary has.
CMD ["mnemos-server", "--bind", "0.0.0.0", "--dir", "/data"]
