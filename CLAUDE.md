# mnemos

A Redis-compatible in-memory key-value server in C++23. Zero third-party
dependencies — server, tests and CI use only the standard library and CMake.

## Build and verify

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo   # once
./scripts/check.sh                                      # build + all three suites
```

**`./scripts/check.sh` is the verification command.** It configures, builds, and
runs unit tests, the interop suite and the differential suite in one pass, and
prints only failures plus a summary. Do not run the steps separately — one
command means one round trip.

```bash
./scripts/check.sh --unit      # unit tests only (fast inner loop)
./scripts/check.sh --verbose   # per-assertion output, for debugging a failure
./scripts/check.sh --sanitize  # Debug + asan/ubsan build in build-asan/
```

Needs `redis-server` and `redis-cli` on PATH for the interop and differential
suites (`brew install redis`). Without them those suites are skipped, not failed.

## The correctness bar

The **differential test** is what the project is judged on: it runs identical
command sequences against mnemos and a real `redis-server` and compares every
reply byte for byte. Unit tests only prove mnemos agrees with itself.

A change is not done until `./scripts/check.sh` is green. New behaviour needs a
new differential suite in `scripts/differential_test.py` (`SUITES`), not just a
unit test. Error strings must be byte-identical to Redis's — use the helpers in
`namespace replies` (`command_table.h`), never a hand-written string.

## Layout

```
src/net/       event loop (kqueue/epoll), RESP2+RESP3 parse and serialise
src/core/      value objects, encodings: listpack, intset, skiplist, dict
src/server/    server, connections, databases, command dispatch
src/server/commands/   one file per command family
tests/         unit tests (ctest, harness in tests/test_harness.h)
scripts/       interop + differential suites
```

`CMakeLists.txt` globs `src/**/*.cpp` with `CONFIGURE_DEPENDS` — new source
files need no build-file edit. `src/persist/`, `src/repl/`, `src/mcp/` are
already wired in the glob and will build as soon as they contain sources.

## Adding a command

1. Handler in the matching `src/server/commands/*_commands.cpp`, signature
   `void name(CommandContext&)`.
2. Declare it in `src/server/commands/commands.h`.
3. One row in the table in `src/server/command_table.cpp`: name, handler, arity
   (negative = "at least |n|", Redis's convention), flags, first/last key, step.
4. A differential suite covering it, including its WRONGTYPE and arity errors.

Collection commands must go through `type_helpers.h` — `lookupTyped`,
`lookupOrCreate`, `deleteIfEmpty`. Those encode three rules that are easy to get
wrong: WRONGTYPE stops the command; a write to a missing key creates the
collection; a collection that becomes empty is *deleted* (`EXISTS` → 0).

## Conventions

- C++23, `-Wall -Wextra -Wpedantic`, no warnings in new code.
- Comments explain *why*, not what. Match the density of the surrounding file.
- `OBJECT ENCODING` must report what is actually in memory. Encoding conversion
  thresholds match Redis's exactly — do not approximate them.
- Single-threaded by design: every command is atomic without locking. Do not
  introduce threads or locks into the command path.

## Not yet implemented

Sharded pub/sub (`SSUBSCRIBE`/`SPUBLISH`), keyspace notifications,
`MULTI`/`EXEC`/`WATCH`, RDB persistence
(real format), AOF with rewrite, replication over `PSYNC`, the MCP server.
Unimplemented commands return an unknown-command error rather than a stub.

## Working agreement

- One feature per session. Commit, then `/clear` before starting the next.
- Batch shell work into one command; each tool call re-reads the whole context.
- Read the specific function or struct, not the whole file, when that will do.
- Reach for `/effort xhigh` for byte-format work (RDB, PSYNC framing) and drop
  back to the project default for command handlers that follow an existing
  pattern.
