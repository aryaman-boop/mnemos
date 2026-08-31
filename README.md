# mnemos

A Redis-compatible server written from scratch in modern C++, with a native
[Model Context Protocol](https://modelcontextprotocol.io/) layer that exposes
its internals to AI agents.

Real `redis-cli` connects to it. Real Redis client libraries work against it.
Its error messages are byte-identical to the originals.

```console
$ redis-cli -p 6380
127.0.0.1:6380> SET user:1 12345
OK
127.0.0.1:6380> OBJECT ENCODING user:1
"int"
127.0.0.1:6380> APPEND user:1 abc
(integer) 8
127.0.0.1:6380> OBJECT ENCODING user:1
"raw"
```

---

## Why this exists

Most "build your own Redis" projects stop once `GET` and `SET` work over a
hand-rolled parser. That demonstrates socket programming, not Redis.

The parts of Redis that are actually interesting are the ones you only meet if
you keep going: that a small hash is not a hash table, that expiry is a
probabilistic sampling loop rather than a scan, that `SCAN`'s guarantees come
from iterating buckets in reverse-binary order so a mid-iteration resize can't
lose elements, and that a "null" is a different thing on the wire depending on
which protocol version the client negotiated.

mnemos implements those. Where a shortcut would have been invisible from the
outside, the harder and more faithful path was taken instead — and the test
suite drives the real `redis-cli` to prove it.

## Quick start

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j

./build/mnemos-server --port 6380
redis-cli -p 6380 PING     # PONG
```

Requires a C++23 compiler (Clang 16+ / GCC 13+) and CMake 3.20+. No third-party
dependencies — not for the server, not for the tests, not for CI.

```
--port <n>            Port to listen on (default 6380)
--bind <addr>         Address to bind (default 127.0.0.1)
--databases <n>       Number of keyspaces (default 16)
--requirepass <pw>    Require AUTH before any command
--maxclients <n>      Connection limit (default 10000)
```

## What's implemented

**Protocol** — RESP2 and RESP3, negotiated with `HELLO`. Incremental parsing
that holds state across partial reads, so a command split across ten TCP
segments costs no re-scanning. Inline commands, quote-and-escape argument
splitting, pipelining, and the full RESP3 type set (maps, sets, doubles,
booleans, verbatim strings, big numbers, push frames).

**Event loop** — kqueue on macOS/BSD, epoll on Linux. Single-threaded, so every
command is atomic without a single lock, exactly as in Redis. Timers drive
background work; the clock is cached once per iteration so every key touched in
one tick agrees on "now".

**Storage** — an open-hashed dict with *incremental* rehashing: two live tables
with one bucket migrated per operation, so growing a large keyspace never stalls
the loop. `SCAN` uses the reverse-binary cursor, which is what lets it promise
that elements present throughout an iteration are returned even if the table
resizes underneath it.

**String encodings** — `int`, `embstr`, and `raw`, chosen by the same rules
Redis uses (strict integer parsing, 44-byte embstr boundary), with the same
one-way promotion to `raw` on mutation. `OBJECT ENCODING` tells the truth.

**Expiry** — lazy deletion on lookup plus the adaptive active cycle: sample 20
random volatile keys 10×/second, delete the dead ones, and repeat immediately
while more than 25% of a sample comes back expired. Replicas deliberately do
*not* expire keys themselves.

**Commands** — 60 of them: the full string family, the complete key/TTL surface
(including `EXPIRE`'s `NX`/`XX`/`GT`/`LT`), `SCAN` with `MATCH`/`COUNT`/`TYPE`,
`OBJECT`, `INFO`, `CONFIG`, `COMMAND`, `CLIENT`, `DEBUG`, and connection
management.

### Roadmap

The remaining work, in the order it's being built:

- [ ] Real `listpack` / `intset` / `quicklist` / `skiplist` encodings
- [ ] Lists, hashes, sets, sorted sets
- [ ] Pub/sub and keyspace notifications
- [ ] `MULTI`/`EXEC`/`WATCH`
- [ ] RDB persistence in the genuine binary format — files real `redis-server` can load
- [ ] AOF with rewrite
- [ ] `PSYNC` replication, with partial resync from a backlog ring buffer
- [ ] The MCP introspection server

## Protocol compatibility

`scripts/interop_test.sh` starts mnemos and drives it with the genuine
`redis-cli`, asserting exact reply values across 51 cases — encodings, TTL edge
cases, arity errors, type errors, and expiry semantics. It runs in CI on Linux
and macOS.

Compatibility is pursued down to the byte. Both of these are real Redis
behaviours that mnemos reproduces:

```
# The command name is echoed as the client spelled it...
> NoPe arg1 arg2
ERR unknown command 'NoPe', with args beginning with: 'arg1' 'arg2'

# ...but an arity error reports the canonical lower-case name.
> GeT
ERR wrong number of arguments for 'get' command
```

## Benchmarks

`redis-benchmark`, 100k requests, 50 connections, Apple M-series, loopback.
Redis 8.10.1 as the reference.

| | Redis 8.10.1 | mnemos |
|---|---:|---:|
| SET | 177,936 rps | 218,818 rps |
| GET | 192,678 rps | 219,780 rps |
| INCR | 190,840 rps | 221,729 rps |
| SET, pipelined ×16 | 1,470,588 rps | 2,941,176 rps |
| GET, pipelined ×16 | 1,834,862 rps | 3,030,303 rps |

**These numbers do not mean mnemos is faster than Redis, and reading them that
way would be a mistake.** They mean mnemos does *less work per command*. Real
Redis is evaluating ACLs, firing keyspace notifications, maintaining replication
backlogs and AOF buffers, tracking LRU/LFU metadata, checking cluster slot
ownership, and dispatching module hooks on every single call. mnemos currently
does none of that.

The honest claim is narrower and still worth making: the event loop, the parser,
and the dispatch path are not leaving performance on the table relative to the
reference implementation. As the roadmap above lands, this gap should *close* —
and if it doesn't, that would indicate the new features aren't being implemented
faithfully.

## Testing

```bash
ctest --test-dir build --output-on-failure   # unit tests
./scripts/interop_test.sh                    # against real redis-cli
```

The unit suite covers protocol parsing under byte-by-byte fragmentation,
RESP2-vs-RESP3 encoding divergence, dict growth and rehashing, `SCAN` coverage
*during* a resize, and every encoding-transition boundary. CI additionally runs
everything under AddressSanitizer and UndefinedBehaviorSanitizer.

## Layout

```
src/net/       event loop (kqueue/epoll), RESP parsing and serialisation
src/core/      value objects, encodings, dict, string utilities
src/server/    server, connection state, databases, command implementations
src/persist/   RDB and AOF                          (in progress)
src/repl/      replication                          (in progress)
src/mcp/       Model Context Protocol server        (in progress)
tests/         unit tests
scripts/       interoperability suite
```

## License

MIT
