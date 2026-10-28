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

# .loc: the profile's program lines, and `dis` printing the runs back
REPORT=$($BIN run --profile tests/asm/loc.masm 2>&1)
echo "$REPORT" | grep -q "hot program lines" || fail "profile: no program lines"
echo "$REPORT" | grep -q "shapes.lua:12" || fail "profile: shapes.lua:12"
TMPD=$(mktemp -d)
$BIN asm tests/asm/loc.masm -o "$TMPD/loc.mlwso" >/dev/null 2>&1 || fail "asm loc.masm"
$BIN dis "$TMPD/loc.mlwso" | grep -q '\.loc "shapes.lua", 12' || fail "dis: .loc"
rm -f "$TMPD/loc.mlwso"
rmdir "$TMPD"

# The module cache: an entry is made and used; an included file that changes,
# or an entry that is damaged, means assembling again; --no-cache makes none
CDIR=$(mktemp -d)
mkdir "$CDIR/src" "$CDIR/cache"
printf '.include "val.inc"\n.include "std/io.inc"\n.func main\n    PRINTLN_I VAL\n    ret 0\n.endf\n' > "$CDIR/src/prog.masm"
printf '.const VAL = 1\n' > "$CDIR/src/val.inc"
OUT=$(MLWSVM_CACHE="$CDIR/cache" $BIN run "$CDIR/src/prog.masm")
[ "$OUT" = 1 ] || fail "cache: first run printed '$OUT'"
[ "$(ls "$CDIR/cache" | grep -c '\.mlwsc$')" = 1 ] || fail "cache: no entry made"
OUT=$(MLWSVM_CACHE="$CDIR/cache" $BIN run "$CDIR/src/prog.masm")
[ "$OUT" = 1 ] || fail "cache: second run printed '$OUT'"
printf '.const VAL = 2\n' > "$CDIR/src/val.inc"
OUT=$(MLWSVM_CACHE="$CDIR/cache" $BIN run "$CDIR/src/prog.masm")
[ "$OUT" = 2 ] || fail "cache: a changed include was not seen ('$OUT')"
for f in "$CDIR"/cache/*.mlwsc; do printf 'MLWCxx' > "$f"; done
OUT=$(MLWSVM_CACHE="$CDIR/cache" $BIN run "$CDIR/src/prog.masm")
[ "$OUT" = 2 ] || fail "cache: a damaged entry was used ('$OUT')"
rm -f "$CDIR"/cache/*.mlwsc
MLWSVM_CACHE="$CDIR/cache" $BIN run --no-cache "$CDIR/src/prog.masm" >/dev/null
[ "$(ls "$CDIR/cache" | grep -c '\.mlwsc$')" = 0 ] || fail "cache: --no-cache made an entry"
rm -rf "${CDIR:?}"

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
echo "check-tools: profile, .loc, cache, trace, monitor, top OK"
