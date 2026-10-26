#!/bin/sh
# Smoke test for the tools that cannot be exercised from inside an assembly
# test: --monitor (HTTP API and dashboard), `mlwsvm top`, --profile and --trace.
# Usage: tests/tools/check_tools.sh [path/to/mlwsvm]      (run by `make check-tools`)
set -eu
BIN=${1:-./mlwsvm}
PORT=${MLWSVM_TEST_PORT:-7397}
fail() { echo "check-tools: FAIL: $*" >&2; exit 1; }

# --profile / --flame
FLAME=$(mktemp)
REPORT=$($BIN run --profile --flame "$FLAME" examples/fib.masm 2>&1)  # (no early-exit pipe: let it finish)
echo "$REPORT" | grep -q "fib.fib" || fail "profile report"
grep -q "fib.main;fib.fib" "$FLAME" || fail "flame stacks"
rm -f "$FLAME"

# --trace with a limit
N=$($BIN run --trace - --trace-limit 5 examples/hello.masm 2>&1 >/dev/null | grep -c '^\[1\]')
[ "$N" = 5 ] || fail "trace limit (got $N lines)"

# --monitor + --hold, then query it
$BIN run --monitor 127.0.0.1:$PORT --hold examples/workers.masm >/dev/null 2>&1 &
VMPID=$!
trap 'kill $VMPID 2>/dev/null || true' EXIT
i=0
until $BIN top --once 127.0.0.1:$PORT >/dev/null 2>&1; do
    i=$((i + 1)); [ $i -lt 50 ] || fail "monitor did not start"; sleep 0.1
done
TOP=$($BIN top --once 127.0.0.1:$PORT)
echo "$TOP" | grep -q "recent exits" || fail "top output"
echo "$TOP" | grep -q "failed E_BOUNDS" || fail "top shows the crashed worker"
if command -v curl >/dev/null 2>&1; then
    curl -sf http://127.0.0.1:$PORT/api/vm | grep -q '"main_pid":1' || fail "/api/vm"
    curl -sf http://127.0.0.1:$PORT/api/exits | grep -q '"code":"E_BOUNDS"' || fail "/api/exits"
    curl -sf http://127.0.0.1:$PORT/api/procs | grep -q '^\[' || fail "/api/procs"
    curl -sf http://127.0.0.1:$PORT/ | grep -q "MLWSVM monitor" || fail "dashboard"
    [ "$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:$PORT/nope)" = 404 ] || fail "404"
fi
echo "check-tools: profile, trace, monitor, top OK"
