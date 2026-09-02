#!/usr/bin/env bash
# Starts mnemos and a real redis-server side by side, then compares their
# replies to identical command sequences.
set -uo pipefail

MNEMOS_PORT="${MNEMOS_DIFF_PORT:-7401}"
REDIS_PORT="${REDIS_DIFF_PORT:-7402}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERVER="${ROOT}/build/mnemos-server"

# Both servers get their own scratch directory: SAVE, BGSAVE and DEBUG RELOAD
# all write a real file, and neither the repository nor the caller's cwd is the
# place for it.
WORKDIR="$(mktemp -d)"
mkdir -p "${WORKDIR}/mnemos" "${WORKDIR}/redis"

cleanup() {
    [[ -n "${MNEMOS_PID:-}" ]] && kill "$MNEMOS_PID" 2>/dev/null
    redis-cli -p "$REDIS_PORT" shutdown nosave 2>/dev/null
    wait 2>/dev/null
    rm -rf "$WORKDIR"
    return 0
}
trap cleanup EXIT

for tool in redis-server redis-cli python3; do
    command -v "$tool" >/dev/null 2>&1 || { echo "error: $tool not found" >&2; exit 1; }
done
[[ -x "$SERVER" ]] || { echo "error: $SERVER not built" >&2; exit 1; }

# A server already on the port would quietly absorb the whole run: the freshly
# built binary fails to bind, and every reply comes from whatever is squatting
# there instead. Refusing to start is the only safe answer.
if redis-cli -p "$MNEMOS_PORT" PING >/dev/null 2>&1; then
    echo "error: something is already listening on port $MNEMOS_PORT" >&2
    exit 1
fi

"$SERVER" --port "$MNEMOS_PORT" --dir "${WORKDIR}/mnemos" >/dev/null 2>&1 &
MNEMOS_PID=$!
# DEBUG is refused by default from redis 7 on, and the RDB suites are built
# around DEBUG RELOAD. `enable-debug-command` predates that refusal, but not by
# much, so a server too old to know the option gets a second, plainer attempt.
redis-server --port "$REDIS_PORT" --save '' --appendonly no --daemonize yes \
    --dir "${WORKDIR}/redis" --enable-debug-command yes >/dev/null 2>&1
sleep 0.2
if ! redis-cli -p "$REDIS_PORT" PING >/dev/null 2>&1; then
    redis-server --port "$REDIS_PORT" --save '' --appendonly no --daemonize yes \
        --dir "${WORKDIR}/redis" >/dev/null 2>&1
fi

# Wait for both to accept connections rather than sleeping a fixed amount.
for _ in $(seq 1 50); do
    if redis-cli -p "$MNEMOS_PORT" PING >/dev/null 2>&1 &&
       redis-cli -p "$REDIS_PORT" PING >/dev/null 2>&1; then
        break
    fi
    sleep 0.1
done

QUIET="${MNEMOS_QUIET:-0}"
if [[ "$QUIET" == "1" ]]; then
    python3 "${ROOT}/scripts/differential_test.py" --quiet \
        --mnemos-port "$MNEMOS_PORT" --redis-port "$REDIS_PORT"
    exit $?
fi

echo "mnemos:       $(redis-cli -p "$MNEMOS_PORT" INFO server | grep mnemos_version | tr -d '\r')"
echo "reference:    redis $(redis-cli -p "$REDIS_PORT" INFO server | grep '^redis_version' | cut -d: -f2 | tr -d '\r')"
echo
python3 "${ROOT}/scripts/differential_test.py" \
    --mnemos-port "$MNEMOS_PORT" --redis-port "$REDIS_PORT"
