#!/usr/bin/env bash
# The single verification entry point: configure, build, and run every suite.
#
# Quiet by design. A green run prints one line per suite and nothing else; a
# failing run prints the failing output and only that. The point is that one
# command answers "is the tree good?", so you never have to run four and read
# four screens of passing assertions to find the one line that matters.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${ROOT}/build"
BUILD_TYPE="RelWithDebInfo"
CMAKE_ARGS=()
UNIT_ONLY=0
VERBOSE=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --unit)     UNIT_ONLY=1 ;;
        --verbose)  VERBOSE=1 ;;
        --sanitize) BUILD="${ROOT}/build-asan"; BUILD_TYPE="Debug"
                    CMAKE_ARGS+=(-DMNEMOS_SANITIZE=ON) ;;
        -h|--help)
            sed -n '2,9p' "${BASH_SOURCE[0]}" | sed 's/^# \?//'
            echo
            echo "usage: check.sh [--unit] [--verbose] [--sanitize]"
            exit 0 ;;
        *) echo "check.sh: unknown option $1" >&2; exit 2 ;;
    esac
    shift
done

# Quiet unless --verbose: the sub-suites print per-assertion lines otherwise.
export MNEMOS_QUIET=$((1 - VERBOSE))

LOG="$(mktemp)"
trap 'rm -f "$LOG"' EXIT

failures=0
declare -a results=()

# run <label> <command...> -- captures output, shows it only on failure (or
# under --verbose), and records the outcome for the summary.
run() {
    local label="$1"; shift
    if "$@" >"$LOG" 2>&1; then
        results+=("  ok    ${label}")
        [[ $VERBOSE -eq 1 ]] && cat "$LOG"
        return 0
    fi
    local status=$?
    results+=("  FAIL  ${label}")
    echo "───── ${label} ─────"
    cat "$LOG"
    echo
    failures=$((failures + 1))
    return $status
}

skip() { results+=("  skip  $1 ($2)"); }

# ---------------------------------------------------------------- build
if [[ ! -f "${BUILD}/CMakeCache.txt" ]]; then
    run "configure" cmake -S "$ROOT" -B "$BUILD" \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" "${CMAKE_ARGS[@]}" || { printf '%s\n' "${results[@]}"; exit 1; }
fi

# A build failure makes every downstream result meaningless, so stop here.
if ! run "build" cmake --build "$BUILD" -j; then
    printf '%s\n' "${results[@]}"
    exit 1
fi

# ---------------------------------------------------------------- suites
run "unit tests" ctest --test-dir "$BUILD" --output-on-failure

if [[ $UNIT_ONLY -eq 0 ]]; then
    if ! command -v redis-cli >/dev/null 2>&1; then
        skip "interop" "redis-cli not found"
        skip "differential" "redis-cli not found"
    else
        run "interop" "${ROOT}/scripts/interop_test.sh"
        if ! command -v redis-server >/dev/null 2>&1; then
            skip "differential" "redis-server not found"
        else
            run "differential" "${ROOT}/scripts/differential_test.sh"
        fi
    fi
fi

# ---------------------------------------------------------------- summary
printf '%s\n' "${results[@]}"
if [[ $failures -gt 0 ]]; then
    echo
    echo "${failures} stage$([[ $failures -eq 1 ]] || echo s) failed"
    exit 1
fi
exit 0
