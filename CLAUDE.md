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

## Roadmap

Roughly 110 in-scope commands remain of ~250. The order below is not
preference: three pieces of infrastructure are shared, and building a consumer
before the thing it consumes means building that thing twice.

**Ordering constraints, in force:**

- **Propagation before AOF, and AOF before replication.** The layer that
  rewrites a command into its deterministic effect (`SPOP` -> `SREM`, `EXPIRE`
  -> `PEXPIREAT`, `INCRBYFLOAT` -> `SET`) does not exist yet. AOF and `PSYNC`
  both need exactly it, and `MONITOR` falls out of it nearly free.
- **Blocking infrastructure before any blocking command.** One blocked-client
  registry, ready-key signal and timeout wheel serve `BLPOP`, `BZPOPMIN`,
  `BLMPOP`, `XREAD BLOCK` and `WAIT`. Reaching streams without it means
  writing it a second time.
- **Hash-field TTL before its RDB type**, not after: the `listpackex` encoding
  is what that new RDB type stores.

In order:

1. ZSET completion -- lex ranges and the modern `ZRANGE` argument forms
   first, then the set ops (`ZUNIONSTORE` and family), `ZMPOP`, `ZINTERCARD`.
   No new infrastructure; every command follows a pattern already in the file.
2. Bitmaps -- `SETBIT` through `BITFIELD`, whose type/overflow grammar is the
   real work.
3. Cursor scans -- `HSCAN`, `SSCAN`, `ZSCAN`. Needs a reverse-binary cursor
   over `Dict` to keep Redis's guarantee across a rehash; trivial while the
   collection is still a listpack.
4. `SORT`/`SORT_RO`, `LCS`, `LINSERT`, `LMPOP`, `MOVE`, `SWAPDB`.
5. Hash-field TTL -- `HEXPIRE` and family, a `listpackex` encoding, field
   expiry. Not testable on the ubuntu runner; gate at `min_redis: (7, 4)`.
6. HyperLogLog -- sparse and dense encodings, byte-exact. `/effort xhigh`.
7. Geo -- 52-bit geohash over a zset, neighbour-cell search. `/effort xhigh`.
8. Blocking infrastructure, then the blocking list and zset pops.
9. Streams -- a whole new type, and large enough to split in two: the type
   with `XADD`/`XRANGE`/`XTRIM` and its RDB encoding, then consumer groups.
10. The propagation layer, then AOF with rewrite, then replication over
    `PSYNC` -- itself split, full sync before backlog and partial resync.
11. The MCP server. Nothing to diff against, so no differential suite.
12. ACL, `CLIENT KILL`/`UNBLOCK`/`PAUSE`/`TRACKING`, `SLOWLOG`, `LATENCY`,
    `MONITOR`, and a single-node `CLUSTER` shim.

**Explicit non-goals.** `EVAL`/`EVALSHA`/`FUNCTION`/`FCALL` need a Lua
interpreter, and the zero-dependency rule means writing one -- more work than
everything above put together. Also out: modules, vector sets, the cluster bus,
and the commands proprietary to the 8.10 reference (`hotkeys`, `backup`,
`himport`, the `ar*` and `V*` families). Unimplemented commands return an
unknown-command error rather than a stub.

## Working agreement

- One feature per session. Commit, then `/clear` before starting the next.
- Batch shell work into one command; each tool call re-reads the whole context.
- Read the specific function or struct, not the whole file, when that will do.
- Reach for `/effort xhigh` for byte-format work (RDB, PSYNC framing) and drop
  back to the project default for command handlers that follow an existing
  pattern.
- Measured across nine sessions of this repo: **~80k tokens per turn, 93-96% of
  it cache reads** of accumulated context. Cost is `turns x mean context`.
- Of those two factors, **turn count is the one worth optimising**, and it was
  tested rather than assumed. The ZSET-lex session read every file the feature
  touched up front in batched parallel calls and then wrote, and came in at
  4.3k tokens per line changed against 8.8-12.7k for the three feature sessions
  before it. Its mean context per turn was *higher* than the repo average (94k
  vs 81k) -- the wide up-front reads cost exactly what residency predicts --
  and it still won, on 25 turns against 91-409. Do not chase the cache-read
  share: it fell to 79% there only because output rose, which is what progress
  looks like. Caveats worth keeping: n=1, lines changed is a poor proxy for
  difficulty, and one compaction inside that session reset residency in its
  favour.
