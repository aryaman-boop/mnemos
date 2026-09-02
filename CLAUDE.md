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

A suite is `(name, commands, flags)`. The flags that exist:

- `two_clients` — each step is `(index, *command)` and runs on connection 0 or
  1. Needed for anything one connection observes another doing.
- `min_redis: (7, 2)` — skip below that reference version. For behaviour newer
  than the redis a CI image ships, *not* for papering over a real difference.
  Skips are printed and counted in the verdict.
- `unordered` — compare replies as multisets, for genuinely unordered ones.

Two pseudo-commands: `@recv` reads a frame nobody asked for (a delivered
message; it blocks on the socket timeout, so one that never arrives fails), and
`@hello3` switches that connection to RESP3. One step must consume exactly one
frame — `UNSUBSCRIBE a b` sends two and desyncs the connection.

## Layout

```
src/net/       event loop (kqueue/epoll), RESP2+RESP3 parse and serialise
src/core/      value objects, encodings: listpack, intset, skiplist, dict
src/persist/   the RDB codec: CRC64, LZF, lengths, objects, whole files
src/server/    server, connections, databases, command dispatch
src/server/commands/   one file per command family
tests/         unit tests (ctest, harness in tests/test_harness.h)
scripts/       interop + differential suites
```

`CMakeLists.txt` globs `src/**/*.cpp` with `CONFIGURE_DEPENDS` — new source
files need no build-file edit. `src/repl/` and `src/mcp/` are already wired in
the glob and will build as soon as they contain sources.

## Adding a command

1. Handler in the matching `src/server/commands/*_commands.cpp`, signature
   `void name(CommandContext&)`.
2. Declare it in `src/server/commands/commands.h`.
3. One row in the table in `src/server/command_table.cpp`: name, handler, arity
   (negative = "at least |n|", Redis's convention), flags, first/last key, step.
4. A differential suite covering it, including its WRONGTYPE and arity errors.

Commands whose first argument selects a subcommand need the `kContainer` flag:
Redis names them `parent|sub` when it quotes them back, and their two error
shapes come from `replies::unknownSubcommand` and
`replies::subcommandSyntaxError`.

Collection commands must go through `type_helpers.h` — `lookupTyped`,
`lookupTypedRead`, `lookupOrCreate`, `deleteIfEmpty`. Those encode three rules
that are easy to get wrong: WRONGTYPE stops the command; a write to a missing
key creates the collection; a collection that becomes empty is *deleted*
(`EXISTS` → 0). Use the `Read` flavour on read paths only — it is what raises
`keymiss`.

## Keyspace notifications

Every mutating command raises its event through `notifyKeyspaceEvent`
(`src/server/notify.h`); nothing publishes to `__keyspace@`/`__keyevent@`
directly. Four rules, all of them checked by the differential suites:

- The name is Redis's, not the command's: `INCR` → `incrby`, `ZINCRBY` and
  `ZADD INCR` → `zincr`, `HMSET` → `hset`, `SINTERSTORE` → `sinterstore`.
- A command that changes nothing says nothing — `SADD` of a member already
  present, `ZADD NX` on an existing member, `EXPIRE ... XX` with no TTL.
- Order is fixed: `new` first, then the command's own event, then the `del`
  that `deleteIfEmpty` raises. Commands that move an element (`LMOVE`, `SMOVE`)
  announce the destination before the source.
- `n` (new keys) and `m` (key misses) are outside the `A` alias, yet `A`
  swallows a set `n` in `CONFIG GET`'s output. Both halves of that are Redis's.

## Conventions

- C++23, `-Wall -Wextra -Wpedantic`, no warnings in new code.
- Comments explain *why*, not what. Match the density of the surrounding file.
- `OBJECT ENCODING` must report what is actually in memory. Encoding conversion
  thresholds match Redis's exactly — do not approximate them.
- Single-threaded by design: every command is atomic without locking. Do not
  introduce threads or locks into the command path.
- Anything written to a client other than the one running the command is built
  against *that* client's protocol version (`Server::queueWrite`, and pub/sub
  delivery for the worked example). RESP2 and RESP3 frame pushes differently.

## Transactions

`MULTI` queues; `EXEC` runs the queue back to back on the one thread, which is
the whole of the atomicity. There is no rollback, so a command that fails inside
`EXEC` fails alone and the rest still run.

- Validation happens at *queue* time, not at `EXEC` time. `Server::dispatch`
  checks arity, auth and subscriber mode before deciding to queue, and its
  `reject()` lambda flags the transaction so `EXEC` refuses the lot with
  `EXECABORT`. `EXEC` then calls handlers directly through `Server::callCommand`
  — no second round of checks.
- Rejecting `EXEC` *itself* is an abort, not an error: `EXEC x` reports
  `EXECABORT Transaction discarded because of: <bare message>`. That is what the
  `…Text` helpers in `namespace replies` exist for.
- A handler's own error (a nested `MULTI`, `WATCH` inside `MULTI`) does *not*
  abort the transaction. Only a command that failed to queue does.
- `WATCH` is invalidated from `Server::notifyKeyspaceEvent`, before its enabled
  check — every mutation already funnels through there, so the set of things
  worth announcing and the set a transaction can lose a race to are one set. The
  two paths that bypass it announce themselves: `FLUSHDB`/`FLUSHALL` call
  `touchWatchedKeysOnFlush` (only for keys actually present), and disconnect and
  `RESET` call `unwatchAllKeys`.
- A key already past its TTL when it was watched records `expired`, and the lazy
  delete that follows is not counted as a change — the watcher had already seen
  it as gone.

## RDB

`src/persist/rdb.{h,cpp}` is the real format, not an approximation: files
mnemos writes are loaded by a live `redis-server` and vice versa (the last two
sections of `scripts/interop_test.sh`), and `DUMP` payloads are compared byte
for byte (the `rdb:` suites). Version 15, CRC-64/Jones, liblzf ported.

- A value is written **in the encoding it is actually in**, so `OBJECT
  ENCODING` still tells the truth after `DEBUG RELOAD`. The pre-listpack types
  (ziplist, zipmap, `RDB_TYPE_LIST`) are rejected, not half-read — redis 8
  never writes them.
- Quicklist nodes: an element over 8192 bytes (`isLargeElement` at
  `list-max-listpack-size -2`) gets a node of its own, written with container
  byte 1 and no listpack around it. Otherwise a node takes elements while
  `node->sz + raw_len + 11 <= 8192`. mnemos has no separate plain node — a node
  holding one oversized element *is* one, and `saveObjectBody` is where the two
  representations meet.
- LZF is offered any string over 20 bytes and kept only if it saved at least
  four, which is expressed by handing it an output buffer four bytes short.
- **Doubles go through `core::d2string`** (`src/core/dtoa.h`), which is Redis's:
  `double2ll`'s integer fast path (exact integers within ±2^62 print in full,
  and `-0.0` prints `0`), else grisu2 shortest digits with a *signed* exponent
  (`1e+100`). `net::formatDouble` is d2string with the one case taken back —
  a reply says `-0`. The difference is visible: a listpack-encoded score has
  already lost the sign, a skiplist-encoded one has not.
- Hashtable-encoded `SET` and `HASH` are **not** byte-comparable against real
  redis — their iteration order is unspecified. Everything else is, intsets and
  listpacks and quicklists and `ZSET_2` included; keep new differential cases
  under the encoding thresholds so they stay so.

## Not yet implemented

AOF with rewrite, replication over `PSYNC`, the MCP server.
Unimplemented commands return an unknown-command error rather than a stub.

## Working agreement

- One feature per session. Commit, then `/clear` before starting the next.
- Batch shell work into one command; each tool call re-reads the whole context.
- Read the specific function or struct, not the whole file, when that will do.
- Reach for `/effort xhigh` for byte-format work (RDB, PSYNC framing) and drop
  back to the project default for command handlers that follow an existing
  pattern.
