# mnemos

An in-memory key-value server that speaks the Redis protocol, written from
scratch in C++23. Any Redis client works with it, including `redis-cli`.

It also ships an [MCP](https://modelcontextprotocol.io/) server, so an AI
assistant can inspect a running instance directly — read keys, watch encodings
change, check what the event loop is doing.

```console
$ mnemos-server --port 6380
mnemos 0.1.0 ready to accept connections on 127.0.0.1:6380

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

## What it's for

**A zero-dependency local cache for development.** One static binary, no
install, starts instantly. Point your app's Redis client at it and go. Useful
in test suites and CI where spinning up a real Redis container is more
ceremony than the test deserves.

**Testing client libraries.** If you're writing or debugging a Redis client,
having a second independent implementation of RESP to test against is genuinely
useful — bugs that look like your code's fault are sometimes assumptions about
the server. mnemos returns byte-identical errors, so it exercises the same
paths.

**A codebase you can actually read.** Redis is ~200k lines of C with two
decades of history. This is a few thousand lines of commented C++ covering the
same core ideas: the event loop, the protocol, the hash table, the encodings,
expiry. If you want to understand how a key-value store works, or teach it,
reading this is a much shorter path.

**Something to fork and modify.** Adding a command is one line in the command
table plus a handler function. If you want a key-value server with custom
commands, a different eviction policy, or your own data type, this is a
reasonable starting point that already handles the networking and protocol.

**Letting an agent inspect your cache.** The MCP layer means you can ask Claude
"what's in this keyspace and why is it using so much memory" and it can actually
look, rather than you pasting `INFO` output back and forth.

## Install

```bash
git clone https://github.com/aryaman-boop/mnemos.git
cd mnemos
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j

./build/mnemos-server --port 6380
```

Needs a C++23 compiler (Clang 16+ / GCC 13+) and CMake 3.20+. Nothing else —
no third-party libraries for the server, the tests, or CI.

```
--port <n>            Port to listen on (default 6380)
--bind <addr>         Address to bind (default 127.0.0.1)
--databases <n>       Number of keyspaces (default 16)
--requirepass <pw>    Require AUTH before any command
--maxclients <n>      Connection limit (default 10000)
--notify-keyspace-events <flags>
                      Keyspace notification classes (default off)
```

## Features

- **RESP2 and RESP3**, negotiated with `HELLO`. Pipelining, inline commands,
  and the full RESP3 type set — maps, sets, doubles, booleans, verbatim
  strings, push frames.
- **Single-threaded event loop** — kqueue on macOS/BSD, epoll on Linux. Every
  command is atomic without locking.
- **All five types** — strings, lists, hashes, sets and sorted sets.
- **Real encodings.** Strings are `int`, `embstr`, or `raw`. Collections use
  the genuine `listpack`, `intset`, `quicklist`, `hashtable` and `skiplist`
  representations and convert between them on the same thresholds Redis uses.
  `OBJECT ENCODING` reports what's actually in memory, not a guess.
- **Incremental rehashing.** The hash table grows a bucket at a time across
  operations instead of stalling on a large resize, and `SCAN` uses the
  reverse-binary cursor so iteration stays correct across a resize.
- **TTLs** with both lazy and active expiry, including `EXPIRE`'s
  `NX`/`XX`/`GT`/`LT` options.
- **~134 commands** — the string, list, hash, set and sorted-set families, the
  full key/TTL surface, `SCAN` with `MATCH`/`COUNT`/`TYPE`, plus `OBJECT`,
  `INFO`, `CONFIG`, `COMMAND`, `CLIENT` and `DEBUG`.
- **Pub/sub** — channel, pattern and shard subscriptions
  (`SSUBSCRIBE`/`SPUBLISH`), with messages framed as plain arrays for RESP2
  subscribers and as push frames for RESP3 ones, and the RESP2
  subscriber-mode command restriction that goes with it.
- **Keyspace notifications** — `notify-keyspace-events` with the whole class
  bitmask, publishing to `__keyspace@<db>__:<key>` and `__keyevent@<db>__:<event>`.
  The event names, the order two of them arrive in, which commands stay silent,
  and the way `CONFIG GET` normalises the flag string are all matched against
  Redis rather than approximated.

### Not there yet

- [ ] `MULTI` / `EXEC` / `WATCH`
- [ ] RDB persistence — in the real format, so files are portable
- [ ] AOF with rewrite
- [ ] Replication over `PSYNC`
- [ ] The MCP server

Until then, any command that isn't implemented returns an unknown-command
error rather than doing something surprising.

## Performance

`redis-benchmark`, 100k requests, 50 connections, on an Apple M-series laptop
over loopback:

| | throughput |
|---|---:|
| SET | 218,818 rps |
| GET | 219,780 rps |
| INCR | 221,729 rps |
| GET, pipelined ×16 | 3,030,303 rps |

Fast enough that it won't be the bottleneck in local development, which is the
bar that matters here.

## Testing

```bash
./scripts/check.sh          # build + every suite; quiet unless something fails
./scripts/check.sh --unit   # unit tests only, for the inner loop
```

`check.sh` is the one command that answers "is the tree good?". The suites also
run standalone:

```bash
ctest --test-dir build --output-on-failure   # unit tests
./scripts/interop_test.sh                    # drives the real redis-cli
./scripts/differential_test.sh               # compares against a real redis-server
```

The **differential test** is the one that matters most: it starts mnemos and a
real `redis-server` side by side, runs identical command sequences against
both, and compares every reply. Unit tests can only show that mnemos agrees
with itself; this shows it agrees with the thing it's imitating, including on
edge cases nobody thinks to write a test for — negative `LREM` counts,
`ZADD GT/LT`, exclusive score ranges, `WRONGTYPE` on every command, and the
rule that a collection is deleted once its last element is removed. Pub/sub is
covered the same way, over two connections per server, so a delivered message
has to arrive on the right one with the right framing. All 46 suites match byte
for byte against Redis 8.10.1; a suite that depends on behaviour newer than the
reference server is skipped and counted, never quietly passed.

The interop suite adds 51 assertions driven through the genuine `redis-cli`.
Unit tests cover protocol parsing under byte-by-byte fragmentation, `SCAN`
during a rehash, skiplist ranks cross-checked against a sorted reference, and
the listpack and intset formats against bytes extracted from a real Redis via
`DUMP`.

CI runs everything on Linux and macOS, plus a pass under AddressSanitizer and
UndefinedBehaviorSanitizer.

## Layout

```
src/net/       event loop, RESP parsing and serialisation
src/core/      value objects, encodings, listpack, intset, skiplist, dict
src/server/    server, connections, databases, commands
src/persist/   RDB and AOF                          (in progress)
src/repl/      replication                          (in progress)
src/mcp/       MCP server                           (in progress)
tests/         unit tests
scripts/       check.sh, interop and differential suites
```

## License

MIT
