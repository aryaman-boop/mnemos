#!/usr/bin/env bash
# Interoperability suite: drives mnemos-server with the *real* redis-cli and
# asserts exact reply values. This is the test that actually proves protocol
# compatibility -- unit tests can only prove we agree with ourselves.
set -uo pipefail

PORT="${MNEMOS_TEST_PORT:-7399}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERVER="${ROOT}/build/mnemos-server"
LOG="$(mktemp)"

pass=0
fail=0

cleanup() {
    if [[ -n "${SERVER_PID:-}" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    rm -f "$LOG"
}
trap cleanup EXIT

if [[ ! -x "$SERVER" ]]; then
    echo "error: $SERVER not found. Run: cmake -S . -B build && cmake --build build -j" >&2
    exit 1
fi
if ! command -v redis-cli >/dev/null 2>&1; then
    echo "error: redis-cli not found. Install redis-tools / brew install redis." >&2
    exit 1
fi

r() { redis-cli -p "$PORT" "$@"; }

# MNEMOS_QUIET=1 suppresses the per-assertion and section chatter, leaving only
# failures and the final tally. Set by scripts/check.sh.
QUIET="${MNEMOS_QUIET:-0}"
say() { [[ "$QUIET" == "1" ]] || echo "$@"; }

# check <description> <expected> <command...>
check() {
    local description="$1" expected="$2"; shift 2
    local actual
    actual="$(r "$@" 2>&1)"
    if [[ "$actual" == "$expected" ]]; then
        [[ "$QUIET" == "1" ]] || printf '  ok   %s\n' "$description"
        pass=$((pass + 1))
    else
        printf '  FAIL %s\n       expected: %q\n         actual: %q\n' \
            "$description" "$expected" "$actual"
        fail=$((fail + 1))
    fi
}

"$SERVER" --port "$PORT" >"$LOG" 2>&1 &
SERVER_PID=$!

# Wait for the listener rather than sleeping a fixed amount, so the suite is
# not flaky on a loaded CI runner.
for _ in $(seq 1 50); do
    if redis-cli -p "$PORT" PING >/dev/null 2>&1; then break; fi
    sleep 0.1
done
if ! redis-cli -p "$PORT" PING >/dev/null 2>&1; then
    echo "error: server did not start. Log:" >&2
    cat "$LOG" >&2
    exit 1
fi

say "== connection =="
check "PING"                  "PONG"           PING
check "PING with message"     "hello"          PING hello
check "ECHO"                  "hi there"       ECHO "hi there"
check "SELECT valid db"       "OK"             SELECT 3
check "SELECT out of range"   "ERR DB index is out of range" SELECT 99
r SELECT 0 >/dev/null

say "== strings =="
check "SET"                   "OK"             SET k1 v1
check "GET"                   "v1"             GET k1
check "GET missing"           ""               GET nosuchkey
check "SET NX on existing"    ""               SET k1 other NX
check "SET XX on missing"     ""               SET brandnew v XX
check "SET GET returns old"   "v1"             SET k1 v2 GET
check "APPEND returns length" "4"              APPEND app abcd
check "STRLEN"                "4"              STRLEN app
check "GETRANGE negative"     "bcd"            GETRANGE app -3 -1
check "SETRANGE"              "4"              SETRANGE app 0 Z
check "GETRANGE after patch"  "Zbcd"           GETRANGE app 0 -1

say "== integers =="
r SET counter 10 >/dev/null
check "INCR"                  "11"             INCR counter
check "INCRBY"                "16"             INCRBY counter 5
check "DECRBY"                "6"              DECRBY counter 10
check "INCR on non-integer"   "ERR value is not an integer or out of range" INCR k1
r SET maxint 9223372036854775807 >/dev/null
check "INCR overflow"         "ERR increment or decrement would overflow" INCR maxint
check "INCRBYFLOAT trims"     "10.5"           INCRBYFLOAT f1 10.5
check "INCRBYFLOAT integral"  "11"             INCRBYFLOAT f1 0.5

say "== encodings =="
r SET enc_int 12345 >/dev/null
check "int encoding"          "int"            OBJECT ENCODING enc_int
r SET enc_emb short >/dev/null
check "embstr encoding"       "embstr"         OBJECT ENCODING enc_emb
r SET enc_lead 007 >/dev/null
check "leading zeros not int" "embstr"         OBJECT ENCODING enc_lead
r SET enc_raw "$(printf 'x%.0s' $(seq 1 45))" >/dev/null
check "raw over 44 bytes"     "raw"            OBJECT ENCODING enc_raw
r SET enc_promote 100 >/dev/null
r APPEND enc_promote abc >/dev/null
check "APPEND promotes to raw" "raw"           OBJECT ENCODING enc_promote

say "== expiry =="
r SET ttlkey v EX 100 >/dev/null
check "TTL set"               "100"            TTL ttlkey
check "PERSIST"               "1"              PERSIST ttlkey
check "TTL after persist"     "-1"             TTL ttlkey
check "TTL missing key"       "-2"             TTL nosuchkey
check "EXPIRE nonexistent"    "0"              EXPIRE nosuchkey 10
r SET gt v EX 100 >/dev/null
check "EXPIRE GT lower fails" "0"              EXPIRE gt 50 GT
check "EXPIRE GT higher ok"   "1"              EXPIRE gt 200 GT
check "EXPIRE NX with TTL"    "0"              EXPIRE gt 300 NX
r SET shortlived v PX 50 >/dev/null
sleep 0.3
check "key expired lazily"    ""               GET shortlived
check "EXISTS after expiry"   "0"              EXISTS shortlived

say "== keyspace =="
r FLUSHDB >/dev/null
r MSET a 1 b 2 c 3 >/dev/null
check "DBSIZE"                "3"              DBSIZE
check "EXISTS counts dupes"   "2"              EXISTS a a
check "TYPE"                  "string"         TYPE a
check "TYPE missing"          "none"           TYPE zzz
check "DEL multiple"          "2"              DEL a b
check "RENAME missing"        "ERR no such key" RENAME nosuchkey dst
r SET src v >/dev/null
check "RENAME"                "OK"             RENAME src dst
check "renamed value"         "v"              GET dst
check "COPY"                  "1"              COPY dst dst2
check "COPY no replace"       "0"              COPY dst dst2

say "== errors =="
check "unknown command"       "ERR unknown command 'NOPE', with args beginning with: " NOPE
check "wrong arity"           "ERR wrong number of arguments for 'get' command" GET
check "SET bad option"        "ERR syntax error" SET k v BOGUS

echo
echo "passed: $pass   failed: $fail"
[[ $fail -eq 0 ]]
