# TODO

The roadmap in [`AGENT.md`](AGENT.md) is the reasoning; this is the checklist.
Ordering is not preference — three pieces of infrastructure are shared, and
building a consumer before the thing it consumes means building that thing
twice.

## Ordering constraints, in force

- **Propagation before AOF, and AOF before replication.** The layer that
  rewrites a command into its deterministic effect (`SPOP` → `SREM`, `EXPIRE` →
  `PEXPIREAT`, `INCRBYFLOAT` → `SET`) does not exist yet. AOF and `PSYNC` both
  need exactly it, and `MONITOR` falls out of it nearly free.
- **Blocking infrastructure before any blocking command.** One blocked-client
  registry, ready-key signal and timeout wheel serve `BLPOP`, `BZPOPMIN`,
  `BLMPOP`, `XREAD BLOCK` and `WAIT`.
- **Hash-field TTL before its RDB type** — the `listpackex` encoding is what
  that new RDB type stores.

## Next

- [x] ZSET completion — lex ranges, modern `ZRANGE`, set ops, `ZMPOP`, `ZINTERCARD`
- [x] The MCP server — `mnemos-mcp`, six tools, `scripts/mcp_test.py`
- [ ] **Bitmaps** — `SETBIT` through `BITFIELD`. The type/overflow grammar is
      the real work.
- [ ] **Cursor scans** — `HSCAN`, `SSCAN`, `ZSCAN`. Needs a reverse-binary
      cursor over `Dict` to keep Redis's guarantee across a rehash; trivial
      while the collection is still a listpack.
- [ ] `SORT`/`SORT_RO`, `LCS`, `LINSERT`, `LMPOP`, `MOVE`, `SWAPDB`
- [ ] **Hash-field TTL** — `HEXPIRE` and family, a `listpackex` encoding, field
      expiry. Not testable on the ubuntu runner; gate at `min_redis: (7, 4)`.
- [ ] **HyperLogLog** — sparse and dense encodings, byte-exact. `/effort xhigh`.
- [ ] **Geo** — 52-bit geohash over a zset, neighbour-cell search. `/effort xhigh`.
- [ ] **Blocking infrastructure**, then the blocking list and zset pops
- [ ] **Streams** — the type with `XADD`/`XRANGE`/`XTRIM` and its RDB encoding,
      then consumer groups. Large enough to split in two.
- [ ] **The propagation layer**, then AOF with rewrite, then replication over
      `PSYNC` — itself split, full sync before backlog and partial resync.
- [ ] ACL, `CLIENT KILL`/`UNBLOCK`/`PAUSE`/`TRACKING`, `SLOWLOG`, `LATENCY`,
      `MONITOR`, and a single-node `CLUSTER` shim

## Loose ends

- [ ] `mnemos-mcp` declares only the `tools` capability. `resources/` (a key as
      a resource) and `prompts/` are unimplemented, and nothing needs them yet.
- [ ] The MCP differential suite never *compares* `server_info` (versions differ
      by construction) though it does read the reference's version through it,
      and it normalises TTLs away. Keep new cases under the encoding thresholds
      so the encodings stay comparable — and note that comparable there means
      comparable against redis 7.0.15, which is what the ubuntu runner ships:
      a call reporting a list or non-integer set encoding needs the fourth
      `DIFF_CALLS` element, `(7, 2)`.
- [ ] Renaming `CLAUDE.md` to `AGENT.md` means Claude Code no longer loads it
      automatically. Symlink it back, or point at it explicitly, if that
      auto-loading is wanted.

## Explicit non-goals

`EVAL`/`EVALSHA`/`FUNCTION`/`FCALL` need a Lua interpreter, and the
zero-dependency rule means writing one — more work than everything above put
together. Also out: modules, vector sets, the cluster bus, and the commands
proprietary to the 8.10 reference (`hotkeys`, `backup`, `himport`, the `ar*`
and `V*` families). Unimplemented commands return an unknown-command error
rather than a stub.
