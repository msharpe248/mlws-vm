#!/bin/sh
# Smoke test for the tools that cannot be exercised from inside an assembly
# test: --monitor (HTTP API and dashboard), `mlwsvm top`, --profile, --trace
# and `mlwsvm opt`.
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

# A .mlwso whose global word is tagged as a reference (the assembler never
# writes one) is refused at load, not used: the word would be a forged pointer.
printf 'g:  .i64 0x4141414141414141\n.func main\n    mov.r R0, @g\n    typeof I0, R0\n    ret 0\n.endf\n' > "$TMPD/gtag.masm"
$BIN asm "$TMPD/gtag.masm" -o "$TMPD/gtag.mlwso" >/dev/null 2>&1 || fail "asm gtag.masm"
perl -0777 -pi -e 's/AAAAAAAA\x06/AAAAAAAA\x0d/ or die' "$TMPD/gtag.mlwso" || fail "gtag: no i64 tag to patch"
set +e
OUT=$($BIN run "$TMPD/gtag.mlwso" 2>&1); rc=$?
set -e
[ $rc = 65 ] && echo "$OUT" | grep -q "E_LOAD_FORMAT.*bad word tag" || fail "forged global tag accepted ($rc: $OUT)"
rm -f "$TMPD/gtag.masm" "$TMPD/gtag.mlwso"
rmdir "$TMPD"

# The module cache: an entry is made and used; an included file that changes,
# or an entry that is damaged, means assembling again; --no-cache makes none;
# with --opt, optimized modules are kept and found by content
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
# --opt: each module's optimized form is kept too, and found again when the
# same program is written to another directory (as a compiler writes a fresh
# temporary one each time)
rm -f "$CDIR"/cache/*.mlwsc
OUT=$(MLWSVM_CACHE="$CDIR/cache" $BIN run --opt 2 "$CDIR/src/prog.masm")
[ "$OUT" = 2 ] || fail "cache: --opt run printed '$OUT'"
N=$(ls "$CDIR/cache" | grep -c '\.mlwsc$')
[ "$N" -ge 2 ] || fail "cache: --opt kept no optimized module ($N entries)"
mkdir "$CDIR/src2"
cp "$CDIR/src/prog.masm" "$CDIR/src/val.inc" "$CDIR/src2/"
OUT=$(MLWSVM_CACHE="$CDIR/cache" $BIN run --opt 2 "$CDIR/src2/prog.masm")
[ "$OUT" = 2 ] || fail "cache: --opt from another file printed '$OUT'"
[ "$(ls "$CDIR/cache" | grep -c '\.mlwsc$')" = "$N" ] || fail "cache: the same program in another directory was optimized again"
rm -rf "${CDIR:?}"

# mlwsvm lib: build, info, and what it refuses
LDIR=$(mktemp -d)
mkdir -p "$LDIR/ok/kit" "$LDIR/nomanifest" "$LDIR/outside" "$LDIR/nopublic" "$LDIR/out"
printf '.library kit, "0.1.0"\n.public kit\n' > "$LDIR/ok/library.masm"
printf '.import kit.part\n.export two\n.func two -> i64\n    call kit.part.one () -> (I0)\n    add I0, 1\n    ret I0\n.endf\n' > "$LDIR/ok/kit.masm"
printf '.export one\n.func one -> i64\n    ret 1\n.endf\n' > "$LDIR/ok/kit/part.masm"
$BIN lib build "$LDIR/ok" -o "$LDIR/out/kit-0.1.0.mlwsl" >/dev/null || fail "lib build"
$BIN lib info "$LDIR/out/kit-0.1.0.mlwsl" | grep -q '^module kit.part ' || fail "lib info: kit.part"
$BIN lib info "$LDIR/out/kit-0.1.0.mlwsl" | grep -q '^module kit (public)' || fail "lib info: public kit"
printf '.import kit\n.func main\n    call kit.two () -> (I0)\n    ret I0\n.endf\n' > "$LDIR/use.masm"
set +e
MLWSVM_NO_CACHE=1 $BIN run --lib-path "$LDIR/out" "$LDIR/use.masm"; rc=$?
set -e
[ $rc = 2 ] || fail "a program using the built library exited with $rc"
printf '.export x\n.func x\n    ret\n.endf\n' > "$LDIR/nomanifest/x.masm"
! $BIN lib build "$LDIR/nomanifest" -o "$LDIR/out/x.mlwsl" 2>/dev/null || fail "lib build without a manifest"
printf '.library kit, "0.1.0"\n' > "$LDIR/outside/library.masm"
cp "$LDIR/nomanifest/x.masm" "$LDIR/outside/other.masm"
! $BIN lib build "$LDIR/outside" -o "$LDIR/out/y.mlwsl" 2>/dev/null || fail "lib build with a module outside the library"
printf '.library kit, "0.1.0"\n.public kit.missing\n' > "$LDIR/nopublic/library.masm"
! $BIN lib build "$LDIR/nopublic" -o "$LDIR/out/z.mlwsl" 2>/dev/null || fail "lib build with a missing public module"
printf 'not an archive' > "$LDIR/out/broken-1.0.0.mlwsl"
MLWSVM_NO_CACHE=1 $BIN run --lib-path "$LDIR/out" "$LDIR/use.masm" 2>&1 >/dev/null | grep -q 'ignoring .*broken-1.0.0.mlwsl' ||
    fail "a damaged archive is not reported"
rm -rf "${LDIR:?}"

# The library path: -L directories before $MLWSVM_PATH's (each dir holds its
# own `which` module); and a -L after the program file is the program's.
WDIR=$(mktemp -d)
mkdir -p "$WDIR/l" "$WDIR/e" "$WDIR/p"
for d in l e; do printf '.export name\n.func name -> ref\n    ret "%s"\n.endf\n' $d > "$WDIR/$d/which.masm"; done
cat > "$WDIR/p/prog.masm" <<'MASM'
.include "std/io.inc"
.import which
.func main
    call which.name () -> (R0)
    ncall "os.args" () -> (R1)
    ncall "text.join" (R1, ",") -> (R2)
    strcat R0, R0, " ", R2
    PRINTLN_S R0
    ret 0
.endf
MASM
OUT=$(MLWSVM_NO_CACHE=1 MLWSVM_PATH="$WDIR/e" $BIN run -L "$WDIR/l" "$WDIR/p/prog.masm" -L x 2>&1) || fail "-L: run ($OUT)"
[ "$OUT" = "l -L,x" ] || fail "-L: before \$MLWSVM_PATH, and not taken from the program's arguments ($OUT)"
OUT=$(MLWSVM_NO_CACHE=1 MLWSVM_PATH="$WDIR/e" $BIN run "$WDIR/p/prog.masm" 2>&1) || fail "MLWSVM_PATH: run ($OUT)"
[ "$OUT" = "e " ] || fail "MLWSVM_PATH: not searched ($OUT)"
rm -rf "${WDIR:?}"

# mlwsvm run a.masm --also b.masm ...: several programs in one VM, to the end of the last
PDIR=$(mktemp -d)
cat > "$PDIR/a.masm" <<'MASM'
.include "std/io.inc"
.func main
    ncall "os.args" () -> (R0)
    ncall "text.join" (R0, ",") -> (R1)
    strcat R1, "a args: ", R1
    PRINTLN_S R1
    ncall "proc.register" ("prog_a", PID)
    recv R2                              ; from program b
    PRINTLN_S R2
    ret 0
.endf
MASM
cat > "$PDIR/b.masm" <<'MASM'
.include "std/io.inc"
.func main
    ncall "os.args" () -> (R0)
    ncall "text.join" (R0, ",") -> (R1)
    strcat R1, "b args: ", R1
    PRINTLN_S R1
1:  ncall "proc.whereis" ("prog_a") -> (I0)
    bnz I0, 2f
    sleep 1
    jmp 1b
2:  send I0, "a got b's message"
    sleep 100                            ; the run waits for the last program
    PRINTLN_S "b done"
    ret 0
.endf
MASM
cat > "$PDIR/c.masm" <<'MASM'
.func main
    ret 3
.endf
MASM
cat > "$PDIR/d.masm" <<'MASM'
.func child
    ncall "proc.register" ("d_child", PID)
    recv I0                              ; forever: stopped when d's main ends
    ret
.endf
.func main
    spawn child ()
    sleep 20
    ret 0
.endf
MASM
cat > "$PDIR/e.masm" <<'MASM'
.include "std/io.inc"
.func main
    sleep 60                             ; d has ended by now
    ncall "proc.whereis" ("d_child") -> (I0)
    bz I0, 1f
    PRINTLN_S "d's child still runs"
    ret 1
1:  PRINTLN_S "d's child was stopped"
    ret 0
.endf
MASM
set +e
OUT=$(MLWSVM_NO_CACHE=1 $BIN run "$PDIR/a.masm" x y --also "$PDIR/b.masm" z --also "$PDIR/c.masm" \
      --also "$PDIR/d.masm" --also "$PDIR/e.masm" 2>&1); rc=$?
set -e
[ $rc = 3 ] || fail "--also: exit status $rc, not c's 3 ($OUT)"
for want in "a args: x,y" "b args: z" "a got b's message" "b done" "d's child was stopped"; do
    echo "$OUT" | grep -q "$want" || fail "--also: no '$want' in: $OUT"
done
cat > "$PDIR/echo.masm" <<'MASM'
.include "std/io.inc"
.func main
    ncall "os.args" () -> (R0)
    ncall "text.join" (R0, "") -> (R1)
    strcat R1, "[", R1, "]"
    PRINTLN_S R1
    ret 0
.endf
MASM
OUT=$(MLWSVM_NO_CACHE=1 $BIN run "$PDIR/echo.masm" one --also "$PDIR/echo.masm" two)
echo "$OUT" | grep -q '\[one\]' && echo "$OUT" | grep -q '\[two\]' || fail "--also: the same program twice ($OUT)"

# Each program's own modules: x/main.masm and y/main.masm (both module main)
# each import util and tools.fmt from their own directory, and get their own;
# a third program in x shares x's. module.load from y looks in y first.
mkdir -p "$PDIR/x/tools" "$PDIR/y/tools"
for d in x y; do
    printf '.export who\n.func who -> ref\n    ret "util of %s"\n.endf\n' $d > "$PDIR/$d/util.masm"
    printf '.export tag\n.func tag -> ref\n    ret "fmt of %s"\n.endf\n' $d > "$PDIR/$d/tools/fmt.masm"
done
cat > "$PDIR/x/main.masm" <<'MASM'
.include "std/io.inc"
.import util
.import tools.fmt
.func main
    call util.who () -> (R0)
    call tools.fmt.tag () -> (R1)
    strcat R0, "x: ", R0, ", ", R1
    PRINTLN_S R0
    ret 0
.endf
MASM
sed 's/"x: "/"y: "/' "$PDIR/x/main.masm" > "$PDIR/y/main.masm"
cat > "$PDIR/x/other.masm" <<'MASM'
.include "std/io.inc"
.import util
.func main
    call util.who () -> (R0)
    strcat R0, "x/other: ", R0
    PRINTLN_S R0
    ret 0
.endf
MASM
cat > "$PDIR/y/late.masm" <<'MASM'
.include "std/io.inc"
.func main
    ncall "module.load" ("util")
    ncall "module.function" ("util", "who") -> (R0)
    calli R0 () -> (R1)
    strcat R1, "y/late: ", R1
    PRINTLN_S R1
    ret 0
.endf
MASM
OUT=$(MLWSVM_NO_CACHE=1 $BIN run "$PDIR/x/main.masm" --also "$PDIR/y/main.masm" --also "$PDIR/x/other.masm" \
      --also "$PDIR/y/late.masm" 2>&1) || fail "--also: programs with same-named modules ($OUT)"
for want in "x: util of x, fmt of x" "y: util of y, fmt of y" "x/other: util of x" "y/late: util of y"; do
    echo "$OUT" | grep -q "^$want\$" || fail "--also: no '$want' in: $OUT"
done
# Tools tell same-named modules apart by their files.
OUT=$(MLWSVM_NO_CACHE=1 $BIN run --profile "$PDIR/x/main.masm" --also "$PDIR/y/main.masm" 2>&1)
echo "$OUT" | grep -q "util.who \[$PDIR/y/util.masm\]" || fail "--also: profile without util's file ($OUT)"
# Two programs of one directory naming the same module: an error that says how to fix it.
printf '.module main\n.func main\n    ret 0\n.endf\n' > "$PDIR/x/alt.masm"
set +e
OUT=$(MLWSVM_NO_CACHE=1 $BIN run "$PDIR/x/main.masm" --also "$PDIR/x/alt.masm" 2>&1); rc=$?
set -e
[ $rc = 65 ] && echo "$OUT" | grep -q "E_LOAD_NAME.*two modules named 'main'.*\.module names a module" ||
    fail "--also: same-named roots in one directory ($rc: $OUT)"

# Library versions are chosen for every program at once: one program allows
# ^1 (newest 1.2.0), the other <1.2; 1.0.0 suits both, in either order.
mkdir -p "$PDIR/libs" "$PDIR/src/v10" "$PDIR/src/v12"
for v in 1.0 1.2; do
    printf '.library ver, "%s.0"\n.public ver\n' $v > "$PDIR/src/v${v%.*}${v#*.}/library.masm"
    printf '.export get\n.func get -> ref\n    ret "%s.0"\n.endf\n' $v > "$PDIR/src/v${v%.*}${v#*.}/ver.masm"
    $BIN lib build "$PDIR/src/v${v%.*}${v#*.}" -o "$PDIR/libs/ver-$v.0.mlwsl" >/dev/null || fail "lib build ver $v"
done
for c in '^1' '<1.2'; do
    n=$(printf '%s' "$c" | tr -dc '0-9')
    printf '.include "std/io.inc"\n.requires ver, "%s"\n.import ver\n.func main\n    call ver.get () -> (R0)\n    PRINTLN_S R0\n    ret 0\n.endf\n' \
        "$c" > "$PDIR/v$n.masm"
done
for order in "v1 v12" "v12 v1"; do
    set -- $order
    OUT=$(MLWSVM_NO_CACHE=1 $BIN run --lib-path "$PDIR/libs" "$PDIR/$1.masm" --also "$PDIR/$2.masm" 2>&1) ||
        fail "--also: versions in the order $order ($OUT)"
    [ "$(echo "$OUT" | grep -c '^1\.0\.0$')" = 2 ] || fail "--also: not 1.0.0 for both, order $order ($OUT)"
done
rm -rf "${PDIR:?}"

# mlwsvm opt: its encoder gives back every module byte for byte (decoding and
# re-encoding each function, with no pass on), and every module optimized at
# -O1 and with every pass reads back (the reader checks lines, .loc runs and handlers); -S, the
# report, --list, the change log, and --opt-report at the end of a run.
# Also `dis`: the disassembly of every module assembles again.
ODIR=$(mktemp -d)
n=0
for f in $(find tests examples lib -name '*.masm' -not -path '*/_bindgen/*'); do
    $BIN asm "$f" -o "$ODIR/a.mlwso" >/dev/null 2>&1 || continue
    $BIN opt -q -O0 --passes encode -o "$ODIR/b.mlwso" "$f" || fail "opt --passes encode: $f"
    cmp -s "$ODIR/a.mlwso" "$ODIR/b.mlwso" || fail "opt: re-encoding changed $f"
    $BIN dis "$ODIR/a.mlwso" > "$ODIR/d.masm" || fail "dis: $f"
    $BIN asm "$ODIR/d.masm" -o "$ODIR/d.mlwso" >/dev/null || fail "dis: the disassembly of $f does not assemble"
    for passes in 1 all; do
        $BIN opt -q -O0 --passes $passes -o "$ODIR/c.mlwso" "$f" || fail "opt --passes $passes: $f"
        $BIN dis "$ODIR/c.mlwso" >/dev/null || fail "opt --passes $passes: $f does not load back"
    done
    n=$((n + 1))
done
[ $n -gt 250 ] || fail "opt: only $n modules re-encoded"
$BIN opt --list | grep -q "^constprop .*-O1" || fail "opt --list"
OUT=$($BIN opt -O3 -S tests/control/branches.masm 2>"$ODIR/report") || fail "opt -S"
echo "$OUT" | grep -q "^.func main" || fail "opt -S: no assembly"
AFTER=$(awk '/^  instructions/ { print $2 "," $3 }' "$ODIR/report")
case "$AFTER" in 39,[1-6]) ;; *) fail "opt: the report ($(cat "$ODIR/report"))" ;; esac
grep -q "branches .*jumps threaded" "$ODIR/report" || fail "opt: the report's passes"
$BIN opt -O1 -v -o "$ODIR/c.mlwso" tests/opt/dead.masm 2>&1 | grep -q "dead.masm:21 unused+1 dce: its result is never read: mul.i64 I2, I0, I0" ||
    fail "opt -v: the change log"
$BIN run "$ODIR/c.mlwso" || fail "opt: the optimized module does not run"
$BIN opt -O9 x.masm 2>/dev/null && fail "opt -O9 accepted"
$BIN run --opt 3,-nosuch examples/hello.masm 2>/dev/null && fail "--opt with an unknown pass accepted"
MLWSVM_NO_CACHE=1 $BIN run --opt 2 --opt-report "$ODIR/r.txt" examples/fib.masm >/dev/null || fail "--opt-report run"
grep -q "^mlwsvm opt: 1 module, 2 functions" "$ODIR/r.txt" || fail "--opt-report: $(cat "$ODIR/r.txt")"
# mlwsvm dis: a string with bytes that are not UTF-8 is written with escapes,
# so the assembly is valid UTF-8 and assembles to the same module
printf '.func main\n    mov.r R0, "a\\xc2b\\xe2\\x82\\xacc"\n    len I0, R0\n    ret I0\n.endf\n' > "$ODIR/bytes.masm"
$BIN asm "$ODIR/bytes.masm" -o "$ODIR/bytes.mlwso" || fail "dis: assembling bytes.masm"
$BIN dis "$ODIR/bytes.mlwso" > "$ODIR/bytes2.masm" || fail "dis: bytes.mlwso"
grep -q 'a\\xc2b' "$ODIR/bytes2.masm" || fail "dis: the stray byte is not escaped ($(cat "$ODIR/bytes2.masm"))"
if command -v iconv >/dev/null 2>&1; then
    iconv -f UTF-8 -t UTF-8 "$ODIR/bytes2.masm" >/dev/null 2>&1 || fail "dis: the output is not UTF-8"
fi
$BIN asm "$ODIR/bytes2.masm" -o "$ODIR/bytes2.mlwso" || fail "dis: reassembling"
$BIN dis "$ODIR/bytes2.mlwso" | grep -q 'a\\xc2b€c' || fail "dis: the string changed on the way round"
rm -rf "${ODIR:?}"

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
echo "check-tools: profile, .loc, cache, lib, --also, opt, trace, monitor, top OK"
