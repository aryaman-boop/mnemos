#!/usr/bin/env python3
"""MCP test: drive mnemos-mcp over its stdio JSON-RPC transport.

This is the one roadmap item with no byte-for-byte oracle, so the coverage is
built in two layers:

1. A functional suite -- both handshake eras, the two error channels, tools/list
   schema validity, every tool's happy path, and the --read-only gate.
2. A differential suite. Because mnemos-mcp is a RESP *client*, the identical
   tool calls can be pointed at a real redis-server and the resulting JSON
   compared. That recovers an oracle for everything except the MCP framing
   itself, which layer 1 covers directly.

Both servers are started and stopped by this script; nothing is left running.

Usage:  mcp_test.py [--quiet] [--build DIR]
"""
import argparse
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

MODERN = "2026-07-28"
LEGACY = "2025-11-25"

results = []
failures = []
skipped = []


def check(name, condition, detail=""):
    if condition:
        results.append(("ok", name))
    else:
        results.append(("FAIL", name))
        failures.append((name, detail))
    return bool(condition)


def check_eq(name, got, want):
    return check(name, got == want, f"got {got!r}, want {want!r}")


def skip(name, want):
    """A check the reference redis is too old to be asked -- recorded, not run,
    and kept out of the totals."""
    results.append(("skip", name))
    skipped.append(name)


# --------------------------------------------------------------------- client
class Mcp:
    """One mnemos-mcp process, spoken to over its stdin/stdout."""

    def __init__(self, binary, port, extra=()):
        self.errfile = tempfile.TemporaryFile()
        self.proc = subprocess.Popen(
            [binary, "--host", "127.0.0.1", "--port", str(port), *extra],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.errfile,
            text=True, bufsize=1)
        self.next_id = 1

    def send_raw(self, line):
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()

    def read_raw(self):
        line = self.proc.stdout.readline()
        if not line:
            raise RuntimeError("mnemos-mcp closed stdout: " + self.stderr())
        return line.rstrip("\n")

    def raw(self, line):
        """Sends an arbitrary line and reads one response."""
        self.send_raw(line)
        return json.loads(self.read_raw())

    def request(self, method, params=None, meta=None, request_id=None):
        if request_id is None:
            request_id = self.next_id
            self.next_id += 1
        message = {"jsonrpc": "2.0", "id": request_id, "method": method}
        params = dict(params or {})
        if meta is not None:
            params["_meta"] = meta
        if params or meta is not None:
            message["params"] = params
        return self.raw(json.dumps(message))

    def modern(self, method, params=None, version=MODERN):
        """A modern-era request: the version and capabilities ride in _meta on
        every call rather than in a one-time handshake."""
        return self.request(method, params, meta={
            "io.modelcontextprotocol/protocolVersion": version,
            "io.modelcontextprotocol/clientCapabilities": {},
        })

    def notify(self, method, params=None):
        message = {"jsonrpc": "2.0", "method": method}
        if params:
            message["params"] = params
        self.send_raw(json.dumps(message))

    def call(self, tool, arguments=None, modern=False):
        """A tools/call, with the text block parsed back out of the envelope."""
        params = {"name": tool, "arguments": arguments or {}}
        reply = self.modern("tools/call", params) if modern else self.request("tools/call", params)
        return reply

    def tool_json(self, tool, arguments=None):
        """The parsed payload of a successful tool call, or (None, error text)."""
        reply = self.call(tool, arguments)
        result = reply.get("result", {})
        text = result.get("content", [{}])[0].get("text", "")
        if result.get("isError"):
            return None, text
        return json.loads(text), None

    def stderr(self):
        self.errfile.seek(0)
        return self.errfile.read().decode("utf-8", "replace").strip()

    def close(self):
        try:
            self.proc.stdin.close()
            self.proc.wait(timeout=5)
        except Exception:
            self.proc.kill()
        self.errfile.close()


# ------------------------------------------------------------- server control
def wait_for_port(port, seconds=5.0):
    deadline = time.time() + seconds
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return True
        except OSError:
            time.sleep(0.05)
    return False


def port_is_free(port):
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=0.2):
            return False
    except OSError:
        return True


# ---------------------------------------------------------- functional suites
def test_legacy_handshake(mcp):
    reply = mcp.request("initialize", {
        "protocolVersion": "2025-06-18",
        "capabilities": {},
        "clientInfo": {"name": "mcp_test", "version": "0"},
    })
    result = reply.get("result", {})
    check_eq("initialize echoes a version the client asked for",
             result.get("protocolVersion"), "2025-06-18")
    check_eq("initialize declares the tools capability",
             result.get("capabilities", {}).get("tools"), {"listChanged": False})
    check_eq("initialize identifies the server",
             result.get("serverInfo", {}).get("name"), "mnemos-mcp")
    # resultType is a modern-era field. A legacy client must not see it.
    check("legacy result carries no resultType", "resultType" not in result,
          f"result keys: {sorted(result)}")

    # An unknown version is answered with one we do speak rather than refused,
    # which is what lets an older or newer client still connect.
    reply = mcp.request("initialize", {"protocolVersion": "1999-01-01", "capabilities": {}})
    check_eq("initialize falls back to our latest legacy version",
             reply.get("result", {}).get("protocolVersion"), LEGACY)


def test_notifications(mcp):
    # A notification has no id and must produce no line at all. Proving that
    # means sending something answerable afterwards and checking whose answer
    # comes back first.
    mcp.notify("notifications/initialized")
    reply = mcp.request("ping", request_id="probe-1")
    check_eq("a notification is never answered", reply.get("id"), "probe-1")
    check_eq("ping returns an empty result", reply.get("result"), {})

    # A null id means the same thing as no id, and MCP forbids it on a request.
    mcp.send_raw(json.dumps({"jsonrpc": "2.0", "id": None, "method": "ping"}))
    reply = mcp.request("ping", request_id="probe-2")
    check_eq("a null id is treated as a notification", reply.get("id"), "probe-2")


def test_modern_handshake(mcp):
    # server/discover is the modern probe and carries no _meta of its own, so
    # it must answer in modern shape regardless.
    reply = mcp.request("server/discover")
    result = reply.get("result", {})
    check_eq("server/discover answers in modern shape", result.get("resultType"), "complete")
    check("server/discover advertises the modern revision",
          MODERN in result.get("supportedVersions", []),
          str(result.get("supportedVersions")))
    check("server/discover still advertises the legacy revisions",
          LEGACY in result.get("supportedVersions", []),
          str(result.get("supportedVersions")))
    check_eq("a modern result identifies the server in _meta",
             result.get("_meta", {}).get("io.modelcontextprotocol/serverInfo", {}).get("name"),
             "mnemos-mcp")

    reply = mcp.modern("ping")
    check_eq("a modern request gets a modern result",
             reply.get("result", {}).get("resultType"), "complete")

    # An id is matched on identity, so it comes back exactly as sent -- number
    # or string, never normalised to one of them.
    reply = mcp.request("ping", request_id=17)
    check_eq("a numeric id is echoed as a number", reply.get("id"), 17)


def test_modern_version_errors(mcp):
    reply = mcp.request("ping", meta={
        "io.modelcontextprotocol/protocolVersion": "1999-01-01",
        "io.modelcontextprotocol/clientCapabilities": {},
    })
    error = reply.get("error", {})
    check_eq("an unsupported modern version is -32022", error.get("code"), -32022)
    check("the error names what we support and what was asked",
          MODERN in error.get("data", {}).get("supported", [])
          and error.get("data", {}).get("requested") == "1999-01-01",
          json.dumps(error))

    reply = mcp.request("ping", meta={"io.modelcontextprotocol/protocolVersion": MODERN})
    check_eq("a modern request without clientCapabilities is -32602",
             reply.get("error", {}).get("code"), -32602)


def test_protocol_errors(mcp):
    check_eq("malformed JSON is -32700",
             mcp.raw("{not json").get("error", {}).get("code"), -32700)
    check_eq("a non-object message is -32600",
             mcp.raw("[1,2,3]").get("error", {}).get("code"), -32600)
    check_eq("a request with no method is -32600",
             mcp.raw('{"jsonrpc":"2.0","id":1}').get("error", {}).get("code"), -32600)
    check_eq("an unknown method is -32601",
             mcp.request("no/such/method").get("error", {}).get("code"), -32601)
    check_eq("tools/call with no name is -32602",
             mcp.request("tools/call", {"arguments": {}}).get("error", {}).get("code"), -32602)
    check_eq("tools/call on an unknown tool is -32602",
             mcp.call("no_such_tool").get("error", {}).get("code"), -32602)

    # A blank line is not a message. It must be swallowed rather than answered
    # with a parse error nobody asked about.
    mcp.send_raw("")
    check_eq("a blank line produces no response",
             mcp.request("ping", request_id="blank").get("id"), "blank")


def test_tool_schemas(mcp, expect_set=True):
    tools = mcp.request("tools/list").get("result", {}).get("tools", [])
    names = [t.get("name") for t in tools]
    expected = ["redis_command", "get", "scan_keys", "describe_key", "server_info"]
    if expect_set:
        expected.append("set")
    check("tools/list offers every tool", sorted(names) == sorted(expected), str(names))

    for tool in tools:
        name = tool.get("name", "?")
        schema = tool.get("inputSchema", {})
        ok = (isinstance(tool.get("description"), str) and tool["description"]
              and schema.get("type") == "object"
              and isinstance(schema.get("properties"), dict)
              and isinstance(schema.get("required"), list)
              # A required argument that is not in `properties` is a schema a
              # client cannot satisfy.
              and all(r in schema["properties"] for r in schema["required"])
              and all(isinstance(p, dict) and "type" in p
                      for p in schema["properties"].values()))
        check(f"{name} has a usable inputSchema", ok, json.dumps(tool))


def test_tools(mcp):
    payload, error = mcp.tool_json("redis_command", {"argv": ["FLUSHALL"]})
    check("FLUSHALL runs through redis_command", error is None, str(error))

    payload, error = mcp.tool_json("set", {"key": "mcp:s", "value": "hello"})
    check_eq("set reports the key it wrote", (payload or {}).get("key"), "mcp:s")

    payload, _ = mcp.tool_json("get", {"key": "mcp:s"})
    check_eq("get returns the value", (payload or {}).get("value"), "hello")
    check_eq("get reports existence", (payload or {}).get("exists"), True)

    payload, _ = mcp.tool_json("get", {"key": "mcp:missing"})
    check_eq("a missing key is null, not an error", (payload or {}).get("value"), None)
    check_eq("a missing key reports exists=false", (payload or {}).get("exists"), False)

    payload, _ = mcp.tool_json("set", {"key": "mcp:ttl", "value": "v", "ttl_seconds": 100})
    check_eq("set applies a TTL", (payload or {}).get("ttl_seconds"), 100)
    payload, _ = mcp.tool_json("describe_key", {"key": "mcp:ttl"})
    check_eq("describe_key sees the TTL", (payload or {}).get("volatile"), True)
    check("describe_key reports ttl_ms", isinstance((payload or {}).get("ttl_ms"), (int, float)),
          json.dumps(payload))

    mcp.tool_json("redis_command", {"argv": ["RPUSH", "mcp:list", "a", "b", "c"]})
    payload, _ = mcp.tool_json("describe_key", {"key": "mcp:list"})
    check_eq("describe_key reports the type", (payload or {}).get("type"), "list")
    check_eq("describe_key reports the real encoding", (payload or {}).get("encoding"), "listpack")
    check_eq("describe_key picks the right size command",
             (payload or {}).get("size_command"), "LLEN")
    check_eq("describe_key reports the size", (payload or {}).get("size"), 3)

    payload, _ = mcp.tool_json("describe_key", {"key": "mcp:missing"})
    check_eq("describe_key on a missing key says so", (payload or {}).get("exists"), False)

    for i in range(20):
        mcp.tool_json("redis_command", {"argv": ["SET", f"mcp:scan:{i}", str(i)]})
    payload, _ = mcp.tool_json("scan_keys", {"pattern": "mcp:scan:*"})
    check_eq("scan_keys finds every matching key", (payload or {}).get("count"), 20)
    check_eq("scan_keys reports a complete walk", (payload or {}).get("complete"), True)
    payload, _ = mcp.tool_json("scan_keys", {"pattern": "mcp:scan:*", "count": 5})
    check_eq("scan_keys honours count", (payload or {}).get("count"), 5)
    check_eq("a truncated walk is not complete", (payload or {}).get("complete"), False)

    payload, _ = mcp.tool_json("server_info", {"section": "server"})
    sections = (payload or {}).get("sections", {})
    check("server_info parses INFO into sections", "server" in sections, json.dumps(payload))
    check("the server section has a version",
          any(k.endswith("_version") for k in sections.get("server", {})),
          json.dumps(sections.get("server")))

    # RESP3 types survive the mapping: an integer is a JSON number, a map is a
    # JSON object, a nil is null.
    payload, _ = mcp.tool_json("redis_command", {"argv": ["LLEN", "mcp:list"]})
    check_eq("an integer reply maps to a number", (payload or {}).get("result"), 3)
    payload, _ = mcp.tool_json("redis_command", {"argv": ["LRANGE", "mcp:list", "0", "-1"]})
    check_eq("an array reply maps to an array", (payload or {}).get("result"), ["a", "b", "c"])
    mcp.tool_json("redis_command", {"argv": ["HSET", "mcp:hash", "f", "v"]})
    payload, _ = mcp.tool_json("redis_command", {"argv": ["HGETALL", "mcp:hash"]})
    check_eq("a RESP3 map reply maps to an object", (payload or {}).get("result"), {"f": "v"})
    payload, _ = mcp.tool_json("redis_command", {"argv": ["GET", "mcp:missing"]})
    check_eq("a nil reply maps to null", (payload or {}).get("result"), None)

    # A DUMP payload is arbitrary bytes. It cannot go in a JSON string as-is,
    # so it is substituted and flagged rather than emitted as invalid JSON.
    payload, _ = mcp.tool_json("redis_command", {"argv": ["DUMP", "mcp:list"]})
    check_eq("a binary reply is flagged", (payload or {}).get("binary"), True)


def test_tool_error_channel(mcp):
    # A tool that ran and failed is a *successful* JSON-RPC result carrying
    # isError, not an error object. The model is meant to read the text.
    reply = mcp.call("redis_command", {"argv": ["LPUSH", "mcp:s", "x"]})
    result = reply.get("result", {})
    check("a WRONGTYPE is not a JSON-RPC error", "error" not in reply, json.dumps(reply))
    check_eq("a failed tool sets isError", result.get("isError"), True)
    text = result.get("content", [{}])[0].get("text", "")
    check("the error text reaches the model", text.startswith("WRONGTYPE"), text)

    reply = mcp.call("redis_command", {"argv": []})
    check_eq("an empty argv is a tool error", reply.get("result", {}).get("isError"), True)
    reply = mcp.call("redis_command", {"argv": [{"nested": 1}]})
    check_eq("a non-scalar argv element is a tool error",
             reply.get("result", {}).get("isError"), True)
    reply = mcp.call("get", {})
    check_eq("a missing required argument is a tool error",
             reply.get("result", {}).get("isError"), True)
    reply = mcp.call("redis_command", {"argv": ["NOSUCHCOMMAND"]})
    check_eq("an unknown redis command is a tool error",
             reply.get("result", {}).get("isError"), True)


def test_read_only(mcp):
    tools = [t["name"] for t in mcp.request("tools/list").get("result", {}).get("tools", [])]
    check("read-only hides the set tool", "set" not in tools, str(tools))
    check_eq("read-only refuses tools/call on set",
             mcp.call("set", {"key": "k", "value": "v"}).get("error", {}).get("code"), -32602)

    payload, error = mcp.tool_json("get", {"key": "mcp:s"})
    check("read-only still reads", error is None, str(error))

    for argv in (["SET", "k", "v"], ["DEL", "k"], ["FLUSHALL"], ["CONFIG", "SET", "x", "y"]):
        reply = mcp.call("redis_command", {"argv": argv})
        check(f"read-only refuses {argv[0]}", reply.get("result", {}).get("isError") is True,
              json.dumps(reply))

    # An unclassifiable command is refused rather than passed through: it cannot
    # be shown to be safe, so the gate fails closed.
    reply = mcp.call("redis_command", {"argv": ["NOSUCHCOMMAND"]})
    check("read-only refuses an unknown command",
          "cannot be classified" in reply.get("result", {}).get("content", [{}])[0].get("text", ""),
          json.dumps(reply))

    payload, error = mcp.tool_json("redis_command", {"argv": ["TYPE", "mcp:s"]})
    check_eq("read-only allows a read command", (payload or {}).get("result"), "string")


# ------------------------------------------------------------ differential
# Identical tool calls run against mnemos and against a real redis-server; the
# JSON is compared. Everything here has to be deterministic on both sides, so
# keep collections under the encoding thresholds and avoid anything timing- or
# version-dependent.
DIFF_SETUP = [
    ["FLUSHALL"],
    ["SET", "d:str", "hello"],
    ["SET", "d:int", "12345"],
    ["SET", "d:long", "x" * 64],
    ["RPUSH", "d:list", "a", "b", "c"],
    ["HSET", "d:hash", "f1", "v1", "f2", "v2"],
    ["SADD", "d:intset", "1", "2", "3"],
    ["SADD", "d:set", "a", "b", "c"],
    ["ZADD", "d:zset", "1", "a", "2.5", "b"],
    ["EXPIRE", "d:str", "1000"],
]

# Entries are (name, tool, arguments) with an optional fourth element: the
# earliest reference redis whose answer this can be compared against.
DIFF_CALLS = [
    ("get a string", "get", {"key": "d:str"}),
    ("get a missing key", "get", {"key": "d:nope"}),
    ("describe a string", "describe_key", {"key": "d:str"}),
    ("describe an int-encoded string", "describe_key", {"key": "d:int"}),
    ("describe a raw string", "describe_key", {"key": "d:long"}),
    # Redis 7.2 is where small lists and small non-integer sets gained their own
    # listpack encoding; before it they report quicklist and hashtable. mnemos
    # targets current redis, so an older reference is out of date here rather
    # than in disagreement -- the same gate the differential suite uses.
    ("describe a list", "describe_key", {"key": "d:list"}, (7, 2)),
    ("describe a hash", "describe_key", {"key": "d:hash"}),
    ("describe an intset", "describe_key", {"key": "d:intset"}),
    ("describe a set", "describe_key", {"key": "d:set"}, (7, 2)),
    ("describe a zset", "describe_key", {"key": "d:zset"}),
    ("describe a missing key", "describe_key", {"key": "d:nope"}),
    ("scan the keyspace", "scan_keys", {"pattern": "d:*"}),
    ("scan for nothing", "scan_keys", {"pattern": "no-such-prefix:*"}),
    ("LRANGE", "redis_command", {"argv": ["LRANGE", "d:list", "0", "-1"]}),
    ("HGET", "redis_command", {"argv": ["HGET", "d:hash", "f1"]}),
    ("HGETALL", "redis_command", {"argv": ["HGETALL", "d:hash"]}),
    ("SMEMBERS of an intset", "redis_command", {"argv": ["SMEMBERS", "d:intset"]}),
    ("ZRANGE WITHSCORES", "redis_command",
     {"argv": ["ZRANGE", "d:zset", "0", "-1", "WITHSCORES"]}),
    ("ZSCORE", "redis_command", {"argv": ["ZSCORE", "d:zset", "b"]}),
    ("ZINCRBY", "redis_command", {"argv": ["ZINCRBY", "d:zset", "1.5", "a"]}),
    ("TYPE", "redis_command", {"argv": ["TYPE", "d:zset"]}),
    ("OBJECT ENCODING", "redis_command", {"argv": ["OBJECT", "ENCODING", "d:zset"]}),
    ("EXISTS", "redis_command", {"argv": ["EXISTS", "d:str", "d:nope"]}),
    ("INCR", "redis_command", {"argv": ["INCR", "d:int"]}),
    ("INCRBYFLOAT", "redis_command", {"argv": ["INCRBYFLOAT", "d:int", "0.5"]}),
    ("a WRONGTYPE error", "redis_command", {"argv": ["LPUSH", "d:str", "x"]}),
    ("an arity error", "redis_command", {"argv": ["GET"]}),
    ("a syntax error", "redis_command", {"argv": ["SET", "k", "v", "BOGUS"]}),
    ("a missing key", "redis_command", {"argv": ["GET", "d:nope"]}),
]


def normalise(value):
    """Strips the fields that cannot agree between two independent servers: a
    TTL read microseconds apart, and a set whose iteration order is its own."""
    if isinstance(value, dict):
        return {k: normalise(v) for k, v in value.items()
                if k not in ("ttl_ms", "ttl_seconds")}
    if isinstance(value, list):
        return [normalise(v) for v in value]
    return value


def sorted_if_unordered(name, payload):
    # SCAN and SMEMBERS have no defined order, so compare them as multisets.
    if isinstance(payload, dict) and isinstance(payload.get("keys"), list):
        payload = dict(payload)
        payload["keys"] = sorted(payload["keys"])
    if "SMEMBERS" in name and isinstance(payload, dict) \
            and isinstance(payload.get("result"), list):
        payload = dict(payload)
        payload["result"] = sorted(payload["result"])
    return payload


def reference_version(client):
    """The reference server's version, as a comparable tuple. Calls tagged with
    a minimum are skipped below it."""
    payload, _ = client.tool_json("server_info", {"section": "server"})
    text = (payload or {}).get("sections", {}).get("server", {}).get("redis_version", "")
    parts = str(text).strip().split(".")
    return tuple(int(p) for p in parts[:3] if p.isdigit())


def run_differential(a, b, quiet):
    reference = reference_version(b)
    for argv in DIFF_SETUP:
        a.tool_json("redis_command", {"argv": argv})
        b.tool_json("redis_command", {"argv": argv})

    for entry in DIFF_CALLS:
        name, tool, arguments = entry[:3]
        want = entry[3] if len(entry) > 3 else None
        if want and reference < want:
            skip(f"diff: {name} (needs redis >= {'.'.join(map(str, want))})", want)
            continue
        ra = a.call(tool, arguments).get("result", {})
        rb = b.call(tool, arguments).get("result", {})
        ta = ra.get("content", [{}])[0].get("text", "")
        tb = rb.get("content", [{}])[0].get("text", "")
        try:
            pa = sorted_if_unordered(name, normalise(json.loads(ta)))
            pb = sorted_if_unordered(name, normalise(json.loads(tb)))
        except json.JSONDecodeError:
            pa, pb = ta, tb  # an isError result carries plain text, not JSON
        same = pa == pb and ra.get("isError") == rb.get("isError")
        check(f"diff: {name}", same,
              f"mnemos: {json.dumps(pa)}\n           redis : {json.dumps(pb)}")


# --------------------------------------------------------------------- driver
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default=None, help="build directory (default: ../build)")
    ap.add_argument("--mnemos-port", type=int, default=7403)
    ap.add_argument("--redis-port", type=int, default=7404)
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    build = args.build or os.path.join(root, "build")
    mcp_binary = os.path.join(build, "mnemos-mcp")
    server_binary = os.path.join(build, "mnemos-server")

    for path in (mcp_binary, server_binary):
        if not os.access(path, os.X_OK):
            print(f"error: {path} not built", file=sys.stderr)
            return 1
    for port in (args.mnemos_port, args.redis_port):
        if not port_is_free(port):
            print(f"error: something is already listening on port {port}", file=sys.stderr)
            return 1

    # A hang is a failure, not something to wait out: every exchange here is a
    # local round trip and finishes in milliseconds.
    signal.alarm(120)

    workdir = tempfile.mkdtemp()
    processes, clients = [], []
    have_redis = shutil.which("redis-server") is not None
    try:
        os.makedirs(os.path.join(workdir, "mnemos"), exist_ok=True)
        processes.append(subprocess.Popen(
            [server_binary, "--port", str(args.mnemos_port),
             "--dir", os.path.join(workdir, "mnemos")],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        if not wait_for_port(args.mnemos_port):
            print("error: mnemos-server did not start", file=sys.stderr)
            return 1

        mcp = Mcp(mcp_binary, args.mnemos_port)
        clients.append(mcp)
        test_legacy_handshake(mcp)
        test_notifications(mcp)
        test_modern_handshake(mcp)
        test_modern_version_errors(mcp)
        test_protocol_errors(mcp)
        test_tool_schemas(mcp)
        test_tools(mcp)
        test_tool_error_channel(mcp)

        ro = Mcp(mcp_binary, args.mnemos_port, extra=["--read-only"])
        clients.append(ro)
        test_tool_schemas(ro, expect_set=False)
        test_read_only(ro)

        if have_redis:
            os.makedirs(os.path.join(workdir, "redis"), exist_ok=True)
            subprocess.run(
                ["redis-server", "--port", str(args.redis_port), "--save", "",
                 "--appendonly", "no", "--daemonize", "yes",
                 "--dir", os.path.join(workdir, "redis")],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
            if wait_for_port(args.redis_port):
                against_redis = Mcp(mcp_binary, args.redis_port)
                clients.append(against_redis)
                run_differential(mcp, against_redis, args.quiet)
            else:
                have_redis = False
    finally:
        signal.alarm(0)
        for client in clients:
            client.close()
        for proc in processes:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
        if have_redis:
            subprocess.run(["redis-cli", "-p", str(args.redis_port), "shutdown", "nosave"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
        shutil.rmtree(workdir, ignore_errors=True)

    if not args.quiet:
        for status, name in results:
            print(f"  {status.ljust(4)} {name}")
        print()
    else:
        for name, detail in failures:
            print(f"  FAIL {name}")
            for line in detail.splitlines():
                print(f"           {line}")

    total = len(results) - len(skipped)
    if failures:
        if not args.quiet:
            for name, detail in failures:
                print(f"  {name}")
                for line in detail.splitlines():
                    print(f"      {line}")
        print(f"{len(failures)} of {total} checks failed")
        return 1
    if not have_redis:
        note = " (differential skipped: redis-server not found)"
    elif skipped:
        note = (f" ({len(skipped)} skipped: reference redis is older than the "
                "behaviour tested)")
    else:
        note = ""
    print(f"all {total} checks passed{note}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
