#!/usr/bin/env bash
# Interoperability suite: drives mnemos-server with the *real* redis-cli and
# asserts exact reply values. This is the test that actually proves protocol
# compatibility -- unit tests can only prove we agree with ourselves.
set -uo pipefail

PORT="${MNEMOS_TEST_PORT:-7399}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERVER="${ROOT}/build/mnemos-server"
LOG="$(mktemp)"
WORKDIR="$(mktemp -d)"

pass=0
fail=0

cleanup() {
    if [[ -n "${SERVER_PID:-}" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    if [[ -n "${RDB_PID:-}" ]] && kill -0 "$RDB_PID" 2>/dev/null; then
        kill "$RDB_PID" 2>/dev/null || true
        wait "$RDB_PID" 2>/dev/null || true
    fi
    [[ -n "${REF_PORT:-}" ]] && redis-cli -p "$REF_PORT" shutdown nosave 2>/dev/null
    rm -f "$LOG"
    rm -rf "$WORKDIR"
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

mkdir -p "${WORKDIR}/mnemos" "${WORKDIR}/redis"
"$SERVER" --port "$PORT" --dir "${WORKDIR}/mnemos" >"$LOG" 2>&1 &
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

# --- RDB files, in both directions -------------------------------------------
# The differential suite compares DUMP payloads, which is the same codec; this
# is the other half of the claim, and the only test that hands a whole file to
# the other implementation. Without redis-server there is nothing to hand it
# to, so the section is skipped rather than failed.
if command -v redis-server >/dev/null 2>&1; then
    # Same assertion as `check`, against whichever server holds the file.
    at() {
        local port="$1" description="$2" expected="$3"; shift 3
        local actual; actual="$(redis-cli -p "$port" "$@" 2>&1)"
        if [[ "$actual" == "$expected" ]]; then
            [[ "$QUIET" == "1" ]] || printf '  ok   %s\n' "$description"
            pass=$((pass + 1))
        else
            printf '  FAIL %s\n       expected: %q\n         actual: %q\n' \
                "$description" "$expected" "$actual"
            fail=$((fail + 1))
        fi
    }
    await() {
        local port="$1"
        for _ in $(seq 1 50); do
            redis-cli -p "$port" PING >/dev/null 2>&1 && return 0
            sleep 0.1
        done
        return 1
    }

    # Enough elements to force a quicklist of several nodes, which is the part
    # of the format with the most room to be wrong.
    BIG=()
    for i in $(seq 1 400); do BIG+=("element-$i-padding-padding-padding-padding"); done
    LZF="$(printf 'abcabcabc-%.0s' $(seq 1 40))"
    # redis-cli prints an array one element per line when it is not a tty.
    LIST="$(printf 'a\nb\nc\n1\n2\n3')"
    BACKLIST="$(printf 'x\ny\nz')"
    REF_PORT=$((PORT + 1))
    RDB_PORT=$((PORT + 2))

    # mnemos writes RDB version 15, which is redis 8.10's. An older redis
    # refuses to load a file from the future and exits, so that direction is
    # gated on the version rather than left to look like a crash. The other
    # direction has no such limit: mnemos loads any version up to its own.
    REF_MAJOR=0
    REF_MINOR=0
    read -r REF_MAJOR REF_MINOR <<<"$(redis-server --version |
        sed -n 's/.*v=\([0-9][0-9]*\)\.\([0-9][0-9]*\).*/\1 \2/p')"

    say
    say "== rdb: a file mnemos wrote, opened by redis =="
    if (( REF_MAJOR > 8 || (REF_MAJOR == 8 && REF_MINOR >= 10) )); then
        r FLUSHALL >/dev/null
        r SET rdb_str hello >/dev/null
        r SET rdb_int 12345 >/dev/null
        r SET rdb_lzf "$LZF" >/dev/null
        r RPUSH rdb_list a b c 1 2 3 >/dev/null
        r RPUSH rdb_big "${BIG[@]}" >/dev/null
        r SADD rdb_intset 1 2 3 >/dev/null
        r SADD rdb_set a b c >/dev/null
        r HSET rdb_hash f1 v1 f2 v2 >/dev/null
        r ZADD rdb_zset 1 a 2.5 b >/dev/null
        r SET rdb_ttl v >/dev/null
        r EXPIRE rdb_ttl 10000 >/dev/null
        r SAVE >/dev/null

        redis-server --port "$REF_PORT" --dir "${WORKDIR}/mnemos" --appendonly no \
            --save '' --daemonize yes >/dev/null 2>&1
        if await "$REF_PORT"; then
            at "$REF_PORT" "redis loaded the file"  "10"        DBSIZE
            at "$REF_PORT" "string survived"        "hello"     GET rdb_str
            at "$REF_PORT" "int encoding survived"  "int"       OBJECT ENCODING rdb_int
            at "$REF_PORT" "lzf string survived"    "$LZF"      GET rdb_lzf
            at "$REF_PORT" "list survived"          "$LIST"     LRANGE rdb_list 0 -1
            at "$REF_PORT" "quicklist survived"     "400"       LLEN rdb_big
            at "$REF_PORT" "quicklist encoding"     "quicklist" OBJECT ENCODING rdb_big
            at "$REF_PORT" "last element intact"    "${BIG[399]}" LINDEX rdb_big -1
            at "$REF_PORT" "intset encoding"        "intset"    OBJECT ENCODING rdb_intset
            at "$REF_PORT" "listpack set encoding"  "listpack"  OBJECT ENCODING rdb_set
            at "$REF_PORT" "hash survived"          "v2"        HGET rdb_hash f2
            at "$REF_PORT" "zset survived"          "2.5"       ZSCORE rdb_zset b
            at "$REF_PORT" "ttl survived"           "1"         PERSIST rdb_ttl
        else
            printf '  FAIL redis-server would not start on the file mnemos wrote\n'
            fail=$((fail + 1))
        fi
        redis-cli -p "$REF_PORT" shutdown nosave 2>/dev/null
    else
        say "  (skipped: redis ${REF_MAJOR}.${REF_MINOR} predates RDB version 15)"
    fi

    say
    say "== rdb: a file redis wrote, opened by mnemos =="
    redis-server --port "$REF_PORT" --dir "${WORKDIR}/redis" --appendonly no \
        --save '' --daemonize yes >/dev/null 2>&1
    if await "$REF_PORT"; then
        redis-cli -p "$REF_PORT" FLUSHALL >/dev/null
        redis-cli -p "$REF_PORT" SET back_str world >/dev/null
        redis-cli -p "$REF_PORT" SET back_lzf "$LZF" >/dev/null
        redis-cli -p "$REF_PORT" RPUSH back_list x y z >/dev/null
        redis-cli -p "$REF_PORT" RPUSH back_big "${BIG[@]}" >/dev/null
        redis-cli -p "$REF_PORT" SADD back_intset 7 8 9 >/dev/null
        redis-cli -p "$REF_PORT" HSET back_hash f v >/dev/null
        redis-cli -p "$REF_PORT" ZADD back_zset 1.5 a >/dev/null
        redis-cli -p "$REF_PORT" SET back_ttl v >/dev/null
        redis-cli -p "$REF_PORT" EXPIRE back_ttl 10000 >/dev/null
        redis-cli -p "$REF_PORT" SAVE >/dev/null
        redis-cli -p "$REF_PORT" shutdown nosave 2>/dev/null
        REF_PORT=""

        # A second mnemos, pointed at the directory redis just wrote into.
        # Loading at startup is the only path that reads a file nothing in this
        # process produced.
        "$SERVER" --port "$RDB_PORT" --dir "${WORKDIR}/redis" >>"$LOG" 2>&1 &
        RDB_PID=$!
        if await "$RDB_PORT"; then
            at "$RDB_PORT" "mnemos loaded the file" "8"        DBSIZE
            at "$RDB_PORT" "string survived"        "world"    GET back_str
            at "$RDB_PORT" "lzf string survived"    "$LZF"     GET back_lzf
            at "$RDB_PORT" "list survived"          "$BACKLIST" LRANGE back_list 0 -1
            at "$RDB_PORT" "quicklist survived"     "400"      LLEN back_big
            at "$RDB_PORT" "quicklist encoding"     "quicklist" OBJECT ENCODING back_big
            at "$RDB_PORT" "last element intact"    "${BIG[399]}" LINDEX back_big -1
            at "$RDB_PORT" "intset encoding"        "intset"   OBJECT ENCODING back_intset
            at "$RDB_PORT" "hash survived"          "v"        HGET back_hash f
            at "$RDB_PORT" "zset survived"          "1.5"      ZSCORE back_zset a
            at "$RDB_PORT" "ttl survived"           "1"        PERSIST back_ttl
        else
            printf '  FAIL mnemos would not start on the file redis wrote\n'
            fail=$((fail + 1))
        fi
    else
        printf '  FAIL redis-server would not start\n'
        fail=$((fail + 1))
    fi
fi

echo
echo "passed: $pass   failed: $fail"
[[ $fail -eq 0 ]]
