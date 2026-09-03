# mnemos-mcp

An MCP server over the mnemos keyspace. Roadmap item 11, pulled ahead of items
2-10 because nothing in the engine is waiting on it.

This file is the plan, written before the code. Delete it or fold it into a
`## MCP` section of `CLAUDE.md` once the thing exists and the design has stopped
moving.

## Shape

```
  mnemos-mcp  --(TCP, RESP3)-->  mnemos-server
   ^                             (or a real redis-server)
   |
  stdio, newline-delimited JSON-RPC 2.0
   |
  an MCP client
```

A **client of the server, not an embedding of it.** Two reasons, both concrete:

- `src/client/` is empty and already in the core glob. Replication (item 10)
  needs a RESP client to talk to a master. Building one here means item 10
  inherits it instead of writing a second one.
- Pointing `mnemos-mcp` at a real `redis-server` costs nothing and hands this
  feature the diff oracle it otherwise would not have. See *Testing*.

The cost is that two processes must be running. Accepted.

## What already exists

`src/net/resp.h` carries the client half of the protocol, written in
anticipation of exactly this — its section comment says *"used by the replica
link and by mnemos-mcp"*:

- `struct Reply` — a parsed reply tree, `Type` covering all thirteen RESP2+RESP3
  forms.
- `bool parseReply(std::string_view buf, std::size_t& pos, Reply& out, std::string& error)`
  — returns false and leaves `pos` alone when the buffer holds a partial reply,
  which is exactly the resumable behaviour a socket read loop needs.
- `std::string encodeCommand(const std::vector<std::string>& argv)`.

All three are covered by `tests/test_resp.cpp` and have no production consumer
yet. **Do not write a second RESP parser.**

`CMakeLists.txt:43` already declares the `mnemos-mcp` target, gated on
`src/mcp/*.cpp` being non-empty. No build-file edit is needed.

## New files

```
src/client/resp_client.{h,cpp}   connect, send argv, read one Reply. Sockets only.
src/mcp/json.{h,cpp}             JSON value, parser, serialiser.
src/mcp/protocol.{h,cpp}         JSON-RPC 2.0 framing + the MCP method handlers.
src/mcp/tools.{h,cpp}            the six tools: schemas, dispatch, RESP->JSON.
src/mcp/main.cpp                 flags, connect, stdio loop.
scripts/mcp_test.py              the harness (see Testing).
```

`src/client/` is inside the `mnemos_core` glob, so `resp_client` is linked into
the server library too — which is what makes it free for replication later.
`src/mcp/` is *not* in that glob; it builds only into the `mnemos-mcp` binary.

## The JSON layer

There is no JSON anywhere in this repo today. It has to be written, and it is
the part most likely to be quietly wrong. The places to be careful:

- **String escapes on output**: `"` `\` and `\b \f \n \r \t`, plus `\u00XX` for
  everything below 0x20. Nothing else gets escaped — do not escape `/`.
- **UTF-8**: Redis values are arbitrary bytes and JSON strings are not. A value
  that is not valid UTF-8 cannot go in a JSON string. Decide once and document
  it in the code: the plan is to emit invalid bytes as `�` and set a
  `binary: true` flag alongside, rather than emitting invalid JSON.
- **Numbers**: use `core::d2string` (`src/core/dtoa.h`). It is already Redis's
  shortest-round-trip formatter. Note that JSON has no `inf`/`nan` — those must
  become strings, which is the same decision RESP2 makes for doubles.
- **Parsing depth**: bound it. A malicious or broken client sending 10k nested
  arrays must not blow the stack.

Keep it minimal: a `Json` variant over null/bool/double/string/array/object is
enough. No pretty-printing, no comments, no trailing commas.

## The MCP layer

Transport is **stdio, newline-delimited JSON-RPC 2.0** — one JSON message per
line. Not LSP's `Content-Length` framing.

> **stdout is the wire.** Every log line, warning and error message goes to
> stderr. A single stray `printf` corrupts the session. `main.cpp` in the server
> prints to stdout freely; do not copy that pattern here.

Lifecycle:

1. `initialize` request -> result `{protocolVersion, capabilities, serverInfo}`.
   Declare `capabilities: {"tools": {"listChanged": false}}`.
2. `notifications/initialized` notification -> no reply (notifications have no
   `id`; never answer one).
3. `tools/list` -> `{tools: [{name, description, inputSchema}]}` where
   `inputSchema` is a JSON Schema object.
4. `tools/call` with `{name, arguments}` -> `{content: [{type: "text", text}], isError}`.

**Protocol version:** echo back the version the client asked for when it is one
we support, otherwise reply with our own latest. Do not hardcode a single string
and reject everything else — the spec is revision-dated and moves. Verify the
current revision against the live spec when implementing rather than trusting
this file.

**Two distinct error channels, and they are not interchangeable:**

- A malformed request, unknown method, or bad params is a JSON-RPC *error
  object* (`-32700` parse, `-32600` invalid request, `-32601` method not found,
  `-32602` invalid params).
- A tool that ran and failed — `WRONGTYPE`, a connection drop, a Redis error
  reply — is a *successful* JSON-RPC result carrying `isError: true`. The model
  is meant to see the error text and adapt.

## Tools

Six. A generic escape hatch plus five curated ones, chosen so the surface does
not have to grow as the command table goes from 156 rows to ~240.

| tool | arguments | notes |
|---|---|---|
| `redis_command` | `argv: string[]` | Runs any command. The escape hatch. |
| `get` | `key` | |
| `set` | `key`, `value`, `ttl_seconds?` | |
| `scan_keys` | `pattern?`, `count?` | Wraps a full `SCAN` cursor walk, not one page. Cap the total returned. |
| `describe_key` | `key` | `TYPE` + `OBJECT ENCODING` + `TTL` + a type-appropriate size (`STRLEN`/`LLEN`/`HLEN`/`SCARD`/`ZCARD`). |
| `server_info` | `section?` | `INFO`, parsed into an object rather than returned as one blob. |

`describe_key` is worth the effort: `OBJECT ENCODING` reporting what is actually
in memory is one of this project's real invariants, and this surfaces it.

**Blast radius of the escape hatch.** `redis_command` can run `FLUSHALL`,
`SHUTDOWN` and `DEBUG`. Add a `--read-only` flag that refuses any command
carrying `kWrite`; the command table is linked in, so classification is a
`lookupCommand` away and costs nearly nothing. Default it off, but document it.

## RESP -> JSON

Connect with `HELLO 3`. RESP3 carries real types, so the mapping is nearly free
and far better than RESP2's flattening. Fall back to RESP2 if `HELLO` errors.

| `Reply::Type` | JSON |
|---|---|
| `SimpleString`, `Bulk`, `Verbatim`, `BigNumber` | string |
| `Error` | tool result with `isError: true`, text = the error |
| `Integer` | number |
| `Double` | number, or a string for `inf`/`-inf`/`nan` |
| `Boolean` | bool |
| `Null` | null |
| `Array`, `Set`, `Push` | array |
| `Map` | object when every key is a string, else an array of pairs |

## Testing

This is the one roadmap item with no byte-for-byte oracle, and that gap needs
covering rather than accepting.

1. **`scripts/mcp_test.py`** — spawns `mnemos-mcp` wired to a running
   `mnemos-server`, speaks JSON-RPC over its stdio, and asserts the handshake,
   `tools/list` schema validity, each tool's happy path, and the two error
   channels. Same shape as `interop_test.sh` and `differential_test.py`.
2. **Point it at a real `redis-server`.** Because the MCP server is a RESP
   client, the identical tool calls can run against real redis and the JSON
   compared. That recovers a differential oracle for everything except the
   MCP framing itself.
3. **Unit tests** for the JSON parser/serialiser in `tests/` — round-trip,
   escapes, invalid UTF-8, depth limit, `inf`/`nan`. The JSON layer is the part
   with the most edge cases and the least protocol help.
4. Add a fourth stage to `scripts/check.sh`, skipped when the binary is absent,
   the way the redis-dependent suites already skip.

## Order of work

1. `src/client/resp_client.{h,cpp}` + unit tests. Smallest piece, and the only
   one another roadmap item depends on.
2. `src/mcp/json.{h,cpp}` + unit tests. Independent of everything above.
3. `src/mcp/protocol.{h,cpp}` — handshake and dispatch, one tool (`redis_command`)
   to prove the loop end to end.
4. `scripts/mcp_test.py` and the `check.sh` stage, at this point rather than last
   — everything after it is then verified as it lands.
5. The five curated tools.
6. `--read-only`, then the `CLAUDE.md` section.

Steps 1 and 2 have no dependency on each other and no MCP knowledge in them;
they are the natural place to start.
