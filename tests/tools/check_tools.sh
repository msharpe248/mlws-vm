#!/bin/sh
# Smoke test for the tools that cannot be exercised from inside an assembly
# test: --monitor (HTTP API and dashboard), `mlwsvm top`, --profile, --trace,
# `mlwsvm opt`, `mlwsvm bundle`, `mlwsvm dap`, std.node and `mlwsvm fuzz`.
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
# A recursion 100,000 deep: each stack keeps its innermost 128 frames, under
# one for the rest, so the samples there share one short key.
DEEP=$(mktemp)
printf '.func down(i64 n) -> i64\n    bz n, 1f\n    dec n\n    call down (n) -> (n)\n1:  ret n\n.endf\n.func main\n    call down (100000) -> (I0)\n    ret 0\n.endf\n' > "$DEEP.masm"
$BIN run --profile --flame "$FLAME" --max-frames 200000 "$DEEP.masm" >/dev/null 2>&1 || fail "deep recursion profile"
grep -q '^\[deeper frames\];.*down;.*down [0-9]*$' "$FLAME" || fail "flame: deep stacks not cut"
[ "$(awk '{ if (length($0) > m) m = length($0) } END { print m }' "$FLAME")" -lt 4000 ] || fail "flame: a stack longer than 128 frames"
rm -f "$FLAME" "$DEEP" "$DEEP.masm"

# .loc: the profile's program lines, and `dis` printing the runs back
REPORT=$($BIN run --profile tests/asm/loc.masm 2>&1)
echo "$REPORT" | grep -q "hot program lines" || fail "profile: no program lines"
echo "$REPORT" | grep -q "shapes.lua:12" || fail "profile: shapes.lua:12"
TMPD=$(mktemp -d)
$BIN asm tests/asm/loc.masm -o "$TMPD/loc.mlwso" >/dev/null 2>&1 || fail "asm loc.masm"
$BIN dis "$TMPD/loc.mlwso" | grep -q '\.loc "shapes.lua", 12' || fail "dis: .loc"
# .local of several registers and .debug_format: printed back, and the
# formatters kept, by name, through every pass (nothing calls them)
$BIN asm tests/tools/debugger_format.masm -o "$TMPD/fmt.mlwso" >/dev/null 2>&1 || fail "asm debugger_format.masm"
$BIN dis "$TMPD/fmt.mlwso" | grep -q '^    \.local n, I3, F3, R3$' || fail "dis: .local of several registers"
$BIN opt -q --passes all -o "$TMPD/fmt2.mlwso" tests/tools/debugger_format.masm || fail "opt debugger_format.masm"
$BIN dis "$TMPD/fmt2.mlwso" > "$TMPD/fmt2.masm"
for f in show_triple show_pair show_thrown show_forever show_sending show_spawning show_waiting; do
    grep -q "^\.debug_format $f\$" "$TMPD/fmt2.masm" || fail "opt: formatter $f lost"
done
rm -f "$TMPD/fmt.mlwso" "$TMPD/fmt2.mlwso" "$TMPD/fmt2.masm"
# A .local's registers are in the window though no instruction uses them (a
# Lua integer never touches its F register): the variable is kept
cat > "$TMPD/unused.masm" <<'EOF'
.func main
    mov R3, 1n
    .local a, I3, F3, R3
    .local b, I4, F4, R4
    mov I0, 0
    ret 0
.endf
EOF
$BIN asm "$TMPD/unused.masm" -o "$TMPD/unused.mlwso" >/dev/null 2>&1 || fail "asm unused.masm"
$BIN dis "$TMPD/unused.mlwso" > "$TMPD/unused.dis"
grep -q '^    \.local a, I3, F3, R3$' "$TMPD/unused.dis" || fail "dis: .local of registers no instruction uses"
grep -q '^    \.local b, I4, F4, R4$' "$TMPD/unused.dis" || fail "dis: .local of registers no instruction uses"
grep -q '^    \.regs I=5, F=5, R=5, P=0$' "$TMPD/unused.dis" || fail "dis: a .local's registers in the window"
# ...and through the optimizer: kept while the registers keep their numbers,
# gone with them when compacting renames one (then the module still loads)
for reg in F1 F2; do
    cat > "$TMPD/unused.masm" <<EOF
.func f(i64 x, f64 y) -> i64
    .local a, I0, $reg
    ret I0
.endf
.func main
    ncall "os.args" () -> (R0)
    len I0, R0
    closure f -> (R1)
    calli R1 (I0, 0.0) -> (I1)
    ret 0
.endf
EOF
    $BIN opt -q --passes all -o "$TMPD/unused.mlwso" "$TMPD/unused.masm" || fail "opt unused.masm ($reg)"
    $BIN run "$TMPD/unused.mlwso" || fail "run optimized unused.masm ($reg)"
    $BIN dis "$TMPD/unused.mlwso" > "$TMPD/unused.dis"
    if [ $reg = F1 ]; then
        grep -q '^    \.local a, I0, F1$' "$TMPD/unused.dis" || fail "opt: a .local of an unused register lost"
    else
        ! grep -q '\.local a' "$TMPD/unused.dis" || fail "opt: a .local kept after its registers were renamed"
    fi
done
rm -f "$TMPD/unused.masm" "$TMPD/unused.mlwso" "$TMPD/unused.dis"
rm -f "$TMPD/loc.mlwso"

# mlwsvm bundle: one executable that runs with no lib/ beside it, whose command
# line is the program's. It keeps VM options (the environment still overrides
# them), takes modules only module.load asks for with --with, and a damaged
# payload is refused.
case $BIN in *.exe) X=.exe ;; *) X= ;; esac
BD=$(mktemp -d)
cat > "$BD/app.masm" <<'EOF'
.import std.math
.func down(i64 n) -> i64
    bz n, 1f
    dec n
    call down (n) -> (n)
1:  ret n
.endf
.func main
    ncall "os.args" () -> (R0)
    len I0, R0
    call std.math.ipow (2, I0) -> (I1)
    call down (500) -> (I2)
    ret I1
.endf
EOF
cat > "$BD/dyn.masm" <<'EOF'
.func main
    ncall "module.load" ("std.math")
    ret 5
.endf
EOF
$BIN bundle -o "$BD/app$X" --max-frames 100 "$BD/app.masm" >/dev/null || fail "bundle"
$BIN bundle -o "$BD/plain$X" --opt 3 --jit on "$BD/app.masm" >/dev/null || fail "bundle --opt"
$BIN bundle -o "$BD/dyn$X" "$BD/dyn.masm" >/dev/null || fail "bundle dyn"
$BIN bundle -o "$BD/dynw$X" --with std.math "$BD/dyn.masm" >/dev/null || fail "bundle --with"
# a name ending in .app, which macOS kills as it starts, is warned of; others are not
$BIN bundle -o "$BD/named.app" "$BD/dyn.masm" 2>&1 >/dev/null | grep -q 'warning: macOS kills' || fail "bundle: no warning for .app"
$BIN bundle -o "$BD/named$X" "$BD/dyn.masm" 2>&1 >/dev/null | grep -q warning && fail "bundle: a warning for a plain name"
mkdir "$BD/alone"
mv "$BD/app$X" "$BD/plain$X" "$BD/dyn$X" "$BD/dynw$X" "$BD/alone/"
(
    cd "$BD/alone" || exit 1
    rm -f ../app.masm ../dyn.masm
    unset MLWSVM_PATH MLWSVM_JIT MLWSVM_OPT MLWSVM_GC_STRESS MLWSVM_MAX_FRAMES
    set +e
    ./plain$X a --also b; [ $? = 8 ] || fail "bundle: arguments, the import or the status"
    ./app$X >/dev/null 2>&1; [ $? = 70 ] || fail "bundle: kept option (--max-frames)"
    MLWSVM_MAX_FRAMES=1000 ./app$X; [ $? = 1 ] || fail "bundle: the environment over a kept option"
    ./dyn$X >/dev/null 2>&1; [ $? != 5 ] || fail "bundle: a module it does not hold"
    ./dynw$X; [ $? = 5 ] || fail "bundle --with"
    cp "plain$X" "bad$X" # (a file just run can be busy for an instant on Linux)
    perl -e 'open F, "+<", $ARGV[0] or die; binmode F; seek F, -16, 2; read F, $l, 8; $l = unpack("Q<", $l); seek F, -16 - $l, 2; print F "XXXX"; close F' "bad$X"
    OUT=$(./bad$X 2>&1); [ $? = 65 ] || fail "bundle: a damaged payload ran"
    echo "$OUT" | grep -q E_LOAD_FORMAT || fail "bundle: damaged payload message"
) || exit 1
rm -rf "${BD:?}"

# mlwsvm dap: a session through the Debug Adapter Protocol, each request taken
# in turn: a breakpoint (one on a line without code moves to the next that has
# some), the stack, registers and an object's fields, stepping in and out,
# the program's output and exit status.
DAPD=$(mktemp -d)
dap() { # session file [VM options...] -> the adapter's output
    f=$1
    shift
    perl tests/tools/_dap/session.pl "$f" | perl -e 'alarm 30; exec @ARGV' $BIN dap "$@" > "$f.out" || fail "dap: did not end"
}
cat > "$DAPD/a" <<'EOF'
initialize {"adapterID":"test"}
launch {"program":"tests/tools/_dap/program.masm"}
setBreakpoints {"source":{"path":"tests/tools/_dap/program.masm"},"breakpoints":[{"line":20},{"line":12}]}
configurationDone
continue
threads
stackTrace {"threadId":1}
scopes {"frameId":65537}
variables {"variablesReference":1}
variables {"variablesReference":3}
evaluate {"expression":"I0","frameId":65537}
stepIn
stackTrace {"threadId":1}
scopes {"frameId":65538}
variables {"variablesReference":1}
evaluate {"expression":"b","frameId":65538}
stepOut
stackTrace {"threadId":1}
next
continue
disconnect
EOF
dap "$DAPD/a"
grep -q '"verified":true,"line":20' "$DAPD/a.out" || fail "dap: breakpoint"
grep -q '"verified":true,"line":14' "$DAPD/a.out" || fail "dap: a breakpoint on a line without code"
grep -q '"reason":"breakpoint"' "$DAPD/a.out" || fail "dap: stopped"
grep -q '"name":"program.main"' "$DAPD/a.out" || fail "dap: stack"
grep -q '"name":"R0","value":"Pair' "$DAPD/a.out" || fail "dap: registers"
grep -q '"name":"left","value":"1"' "$DAPD/a.out" || fail "dap: an object's fields"
grep -q '"result":"40"' "$DAPD/a.out" || fail "dap: evaluate"
grep -q '"name":"program.add"' "$DAPD/a.out" || fail "dap: step in"
grep -q '"name":"Locals","presentationHint":"locals","variablesReference":1' "$DAPD/a.out" || fail "dap: a Locals scope"
grep -q '"name":"a","value":"40"' "$DAPD/a.out" || fail "dap: a parameter by name"
grep -q '"result":"2"' "$DAPD/a.out" || fail "dap: evaluate a name"
grep -q '"output":"42"' "$DAPD/a.out" || fail "dap: the program's output"
grep -q '"exitCode":3' "$DAPD/a.out" || fail "dap: exit status"
grep -q '"event":"terminated"' "$DAPD/a.out" || fail "dap: terminated"

# Source lines through .loc, a bad program, and pausing a program that runs on.
cat > "$DAPD/b" <<'EOF'
initialize {"adapterID":"test"}
launch {"program":"tests/asm/loc.masm"}
setBreakpoints {"source":{"path":"/somewhere/shapes.lua"},"breakpoints":[{"line":12}]}
configurationDone
stackTrace {"threadId":1}
disconnect
EOF
dap "$DAPD/b"
grep -q '"verified":true,"line":12' "$DAPD/b.out" || fail "dap: a breakpoint in a translated file"
grep -q '"name":"shapes.lua"' "$DAPD/b.out" || fail "dap: a frame in a translated file"
cat > "$DAPD/c" <<'EOF'
initialize {"adapterID":"test"}
launch {"program":"tests/tools/_dap/no_such_program.masm"}
disconnect
EOF
dap "$DAPD/c"
grep -q '"success":false,"command":"launch"' "$DAPD/c.out" || fail "dap: a program that does not load"
cat > "$DAPD/d" <<'EOF'
initialize {"adapterID":"test"}
launch {"program":"tests/tools/_dap/spin.masm"}
configurationDone
pause
stackTrace {"threadId":1}
disconnect
EOF
dap "$DAPD/d"
grep -q '"reason":"pause"' "$DAPD/d.out" || fail "dap: pause"
grep -q '"name":"spin.main"' "$DAPD/d.out" || fail "dap: stack after a pause"
# A variable of two registers, shown by the program's formatter, expanded into
# its registers, and evaluated by name.
cat > "$DAPD/e" <<'EOF'
initialize {"adapterID":"test"}
launch {"program":"tests/tools/_dap/format.masm"}
setBreakpoints {"source":{"path":"tests/tools/_dap/format.masm"},"breakpoints":[{"line":18}]}
configurationDone
scopes {"frameId":65537}
variables {"variablesReference":1}
variables {"variablesReference":4}
evaluate {"expression":"x","frameId":65537}
disconnect
EOF
dap "$DAPD/e"
grep -q '"name":"x","value":"int 7","variablesReference":4' "$DAPD/e.out" || fail "dap: a variable shown by its formatter"
grep -q '"name":"I3","value":"7".*"name":"R3","value":"null"' "$DAPD/e.out" || fail "dap: a variable's registers"
grep -q '"result":"int 7"' "$DAPD/e.out" || fail "dap: evaluate a formatted variable"
# VM options: a recursion 150,000 deep runs past the default limit of 100,000
# frames with --max-frames from the adapter's command line, or from the
# launch's vmOptions; a bad one fails the launch. (The program's path is
# relative: on Windows, MSYS translates a path in an argument, not in JSON.)
DEEP_PROG=tests/tools/_dap/deep.masm
k=0
for v in '' ',"vmOptions":["--max-frames","200000"]' ',"vmOptions":["--max-frames"]' ',"vmOptions":["--frames","1"]'; do
    k=$((k + 1))
    printf 'initialize {"adapterID":"test"}\nlaunch {"program":"%s"%s}\nconfigurationDone\ndisconnect\n' "$DEEP_PROG" "$v" > "$DAPD/f$k"
    dap "$DAPD/f$k"
done
grep -q '"exitCode":70' "$DAPD/f1.out" || fail "dap: the default call depth limit"
grep -q '"exitCode":7}' "$DAPD/f2.out" || fail "dap: --max-frames in vmOptions"
grep -q '"success":false,"command":"launch","message":"vmOptions: --max-frames needs a value"' "$DAPD/f3.out" || fail "dap: a VM option without a value"
grep -q '"success":false,"command":"launch","message":"vmOptions: --frames is not a VM option"' "$DAPD/f4.out" || fail "dap: an unknown VM option"
printf 'initialize {"adapterID":"test"}\nlaunch {"program":"%s"}\nconfigurationDone\ndisconnect\n' "$DEEP_PROG" > "$DAPD/g"
dap "$DAPD/g" --max-frames 200000
grep -q '"exitCode":7}' "$DAPD/g.out" || fail "dap --max-frames"

# mlwsvm debug takes the same VM options, anywhere on its command line, and,
# like dap, does not optimize for MLWSVM_OPT: the names would go.
echo run | $BIN debug "$DEEP_PROG" | grep -q E_CALL_DEPTH || fail "debug: the default call depth limit"
echo run | $BIN debug --max-frames 200000 "$DEEP_PROG" | grep -q 'Program exited with code 7' || fail "debug --max-frames"
echo run | $BIN debug "$DEEP_PROG" --max-frames 200000 | grep -q 'Program exited with code 7' || fail "debug: a VM option after the program"
set +e
$BIN debug --max-frames x "$DEEP_PROG" </dev/null 2>/dev/null; RC_BAD=$?
$BIN debug "$DEEP_PROG" --max-frames </dev/null 2>/dev/null; RC_NOVAL=$?
set -e
[ $RC_BAD = 2 ] || fail "debug: a bad VM option (exit $RC_BAD)"
[ $RC_NOVAL = 2 ] || fail "debug: a VM option without a value (exit $RC_NOVAL)"
printf 'break area\nrun\nlocals\nquit\n' > "$DAPD/script"
MLWSVM_OPT=3 $BIN debug tests/tools/debugger_locals.masm < "$DAPD/script" | grep -q '^width (I0) = 3$' || fail "debug: optimized for MLWSVM_OPT"
$BIN debug --opt 3 tests/tools/debugger_locals.masm < "$DAPD/script" | grep -q "No function named 'area'" || fail "debug --opt 3: not optimized"
rm -rf "${DAPD:?}"

# A connect with no local port free fails with E_RESOURCES on every system.
# Only on Linux, which runs out of ports per destination (elsewhere every
# program on the machine would lose its ports), with descriptors enough.
if [ "$(uname -s)" = Linux ] && [ "$(ulimit -Hn)" = unlimited -o "$(ulimit -Hn)" -ge 70000 ] 2>/dev/null; then
    OUT=$($BIN run tests/tools/_net/no_port.masm 2>&1) || fail "no_port: $OUT"
    echo "$OUT" | grep -q 'E_RESOURCES net.connect: 127.0.0.1:[0-9]*: no local port is free' || fail "no_port: $OUT"
fi

# Crash reports: a process nobody watches that is killed ends quietly; its own
# failure, or an E_LINKED, is reported.
OUT=$($BIN run tests/tools/_reports/kills.masm 2>&1) || fail "reports: $OUT"
[ "$(echo "$OUT" | grep -c ' failed: ')" = 2 ] || fail "reports: $OUT"
echo "$OUT" | grep -q '(divider) failed: \[E_DIV_ZERO\]' || fail "reports: $OUT"
echo "$OUT" | grep -q '(partner) failed: \[E_LINKED\]' || fail "reports: $OUT"
! echo "$OUT" | grep -q E_KILLED || fail "reports: $OUT"

# std.node: two VMs on this machine, plainly and, if there is a TLS library,
# over TLS. Node 1 checks all of it against node 2, and ends it.
NODED=$(mktemp -d)
node_pair() { # [tls]
    $BIN run tests/tools/_node/two.masm ${1:-} > "$NODED/port" 2>/dev/null &
    TWO=$!
    for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
        [ -s "$NODED/port" ] && break
        perl -e 'select(undef, undef, undef, 0.25)'
    done
    [ -s "$NODED/port" ] || { kill $TWO 2>/dev/null || true; fail "node ${1:-}: the second VM did not start"; }
    if [ -z "${1:-}" ]; then # another node with node 2's name: refused, and node 2 unharmed
        for clash in "3 two@test" "2 other@test"; do # node 2's name; node 2's number
            # shellcheck disable=SC2086
            perl -e 'alarm 30; exec @ARGV' $BIN run tests/tools/_node/clash.masm "$(tr -d '\r\n' < "$NODED/port")" $clash >/dev/null 2>"$NODED/err" || { kill $TWO 2>/dev/null || true; cat "$NODED/err" >&2; fail "node names: clash ($clash)"; }
        done
    fi
    perl -e 'alarm 30; exec @ARGV' $BIN run tests/tools/_node/one.masm "$(tr -d '\r\n' < "$NODED/port")" ${1:-} >/dev/null 2>"$NODED/err" || { kill $TWO 2>/dev/null || true; cat "$NODED/err" >&2; fail "node ${1:-}"; }
    wait $TWO
    rm -f "$NODED/port"
}
node_pair
printf '.func main\n    ncall "tls.available" () -> (I0)\n    ret I0\n.endf\n' > "$NODED/tls.masm"
set +e
$BIN run "$NODED/tls.masm"; HAVE_TLS=$?
set -e
[ $HAVE_TLS = 1 ] && node_pair tls
rm -rf "${NODED:?}"

# Back-pressure between nodes: near.masm sends 125 MiB to far.masm's sink
# while far's VM is stopped, so that the connection cannot write. The sender
# must wait after a few MiB, and everything arrives once far goes on. Near's
# monitor shows it meanwhile: the connection's backlog, over 1 MiB, and one
# sender waiting, in /api/nodes and /top. (No SIGSTOP on Windows.)
case "$(uname -s)" in
MINGW* | MSYS* | CYGWIN*) ;;
*)
    BUSYD=$(mktemp -d)
    mkfifo "$BUSYD/in"
    $BIN run tests/tools/_busy/far.masm > "$BUSYD/port" 2>/dev/null &
    FAR=$!
    for _ in $(seq 1 80); do grep -q '^[0-9][0-9]*$' "$BUSYD/port" && break; perl -e 'select(undef, undef, undef, 0.25)'; done
    perl -e 'alarm 120; exec @ARGV' $BIN run --monitor 127.0.0.1:$PORT tests/tools/_busy/near.masm "$(cat "$BUSYD/port")" < "$BUSYD/in" > "$BUSYD/out" 2>&1 &
    NEAR=$!
    exec 4> "$BUSYD/in"
    for _ in $(seq 1 80); do grep -q ready "$BUSYD/out" && break; perl -e 'select(undef, undef, undef, 0.25)'; done
    kill -STOP $FAR
    echo stopped >&4
    for _ in $(seq 1 80); do grep -q 'sent while' "$BUSYD/out" && break; perl -e 'select(undef, undef, undef, 0.25)'; done
    if command -v curl >/dev/null 2>&1; then
        NODES=$(curl -sf http://127.0.0.1:$PORT/api/nodes) || NODES="(no answer)"
        TOPN=$(curl -sf http://127.0.0.1:$PORT/top) || TOPN="(no answer)"
        AWAY=$(curl -sf http://127.0.0.1:$PORT/api/proc/1) || AWAY="(no answer)"
    fi
    kill -CONT $FAR
    echo going >&4
    exec 4>&-
    wait $NEAR || { kill $FAR 2>/dev/null; cat "$BUSYD/out" >&2; fail "node back-pressure: near failed"; }
    kill $FAR 2>/dev/null; wait $FAR 2>/dev/null || true
    SENT=$(sed -n 's/^sent while stopped: //p' "$BUSYD/out")
    grep -q '^arrived: 2000$' "$BUSYD/out" || fail "node back-pressure: not all arrived: $(cat "$BUSYD/out")"
    [ -n "$SENT" ] && [ "$SENT" -lt 1000 ] || fail "node back-pressure: the sender did not wait: $(cat "$BUSYD/out")"
    if [ -n "${NODES:-}" ]; then
        QUEUED=$(echo "$NODES" | sed -n 's/.*"queued":\([0-9]*\).*/\1/p')
        echo "$NODES" | grep -q '"node":2,"name":"","carrier":[0-9]*,"up_ms":[0-9]*,"sent":[0-9]*' || fail "/api/nodes: $NODES"
        echo "$NODES" | grep -q '"waiting":1,' || fail "/api/nodes: no sender waiting: $NODES"
        [ -n "$QUEUED" ] && [ "$QUEUED" -gt 1048576 ] || fail "/api/nodes: the backlog: $NODES"
        echo "$TOPN" | grep -q '^nodes (this VM: node 1):' || fail "/top: no nodes: $TOPN"
        echo "$TOPN" | grep -Eq '^ +2 - .* 1 +[0-9]+$' || fail "/top: node 2's line: $TOPN"
        # Its main is reading a line meanwhile, a blocking call an I/O thread
        # makes: it is shown as it was when the call began, where it made it.
        echo "$AWAY" | grep -q '^{"pid":1,"state":"native","away":true,' || fail "/api/proc: a process in a blocking call: $AWAY"
        echo "$AWAY" | grep -q '"messages":\[\],"backtrace":\["near.main+[0-9]* ([^"]*near.masm:50)"\]}$' || fail "/api/proc: where its call was made: $AWAY"
        echo "$TOPN" | grep -Eq '^ +1 native .* near\.main\+[0-9]+ \(near\.masm:50\)$' || fail "/top: a process in a blocking call: $TOPN"
    fi
    rm -rf "${BUSYD:?}"
    ;;
esac

# Connecting when a message needs it: node a drops its connection to b, and a
# send, then a call by name, connect again; with auto_connect off they do
# not; once b has ended, they fail without hanging.
AUTOD=$(mktemp -d)
$BIN run tests/tools/_auto/b.masm > "$AUTOD/port" 2>/dev/null &
NODEB=$!
for _ in $(seq 1 80); do grep -q '^[0-9][0-9]*$' "$AUTOD/port" && break; perl -e 'select(undef, undef, undef, 0.25)'; done
perl -e 'alarm 60; exec @ARGV' $BIN run tests/tools/_auto/a.masm "$(cat "$AUTOD/port")" > "$AUTOD/out" 2>&1 || { kill $NODEB 2>/dev/null; fail "node auto-connect: $(cat "$AUTOD/out")"; }
wait $NODEB 2>/dev/null || true
rm -rf "${AUTOD:?}"

# Environments from a file (--env): a variable's text within a JSON string is
# escaped (a backslash, a quote), and outside one is the JSON itself; a file
# that is not right stops the run, saying where.
OUT=$(MLWSVM_ENV_TEST='C:\Windows "q"' MLWSVM_ENV_NUM=42 $BIN run --env tests/tools/_env/esc.mlws.env tests/tools/_env/esc.masm 2>&1) || fail "--env: $OUT"
[ "$OUT" = "$(printf 'C:\\Windows "q"\\x\n42\na"C:\\Windows "q"')" ] || fail "--env: the values ($OUT)"
printf 'oops\n' > "$TMPD/bad.env"
set +e
OUT=$($BIN run --env "$TMPD/bad.env" tests/tools/_env/esc.masm 2>&1); RC=$?
set -e
[ $RC = 2 ] && echo "$OUT" | grep -q 'bad.env:1: expected name = value' || fail "--env: a bad file ($RC: $OUT)"
rm -f "$TMPD/bad.env"

# Names across the cluster: registered at one node, seen and taken at the
# other; gone everywhere when the process ends or is unregistered; dropped
# when the nodes part; and settled when both registered one name apart.
GLOBD=$(mktemp -d)
$BIN run tests/tools/_global/b.masm > "$GLOBD/port" 2>"$GLOBD/b.err" &
NODEB=$!
for _ in $(seq 1 80); do grep -q '^[0-9][0-9]*$' "$GLOBD/port" && break; perl -e 'select(undef, undef, undef, 0.25)'; done
perl -e 'alarm 60; exec @ARGV' $BIN run tests/tools/_global/a.masm "$(cat "$GLOBD/port")" > "$GLOBD/out" 2>&1 || { kill $NODEB 2>/dev/null; fail "names across the cluster: $(cat "$GLOBD/out") $(cat "$GLOBD/b.err")"; }
kill $NODEB 2>/dev/null || true
wait $NODEB 2>/dev/null || true
[ ! -s "$GLOBD/b.err" ] || fail "names across the cluster: node b: $(cat "$GLOBD/b.err")"
rm -rf "${GLOBD:?}"

# Ticks: two nodes with a ticktime of a second stay connected while idle,
# and once the far one is stopped, the near one takes it as down in about a
# second. (No SIGSTOP on Windows.)
case "$(uname -s)" in
MINGW* | MSYS* | CYGWIN*) ;;
*)
    TICKD=$(mktemp -d)
    $BIN run tests/tools/_tick/far.masm > "$TICKD/port" 2>/dev/null &
    FAR=$!
    for _ in $(seq 1 80); do grep -q '^[0-9][0-9]*$' "$TICKD/port" && break; perl -e 'select(undef, undef, undef, 0.25)'; done
    perl -e 'alarm 60; exec @ARGV' $BIN run tests/tools/_tick/near.masm "$(cat "$TICKD/port")" > "$TICKD/out" 2>&1 &
    NEAR=$!
    for _ in $(seq 1 80); do grep -q 'idle ok\|dropped' "$TICKD/out" && break; perl -e 'select(undef, undef, undef, 0.25)'; done
    kill -STOP $FAR
    wait $NEAR || { kill -CONT $FAR; kill $FAR 2>/dev/null; fail "node ticks: $(cat "$TICKD/out")"; }
    kill -CONT $FAR
    kill $FAR 2>/dev/null; wait $FAR 2>/dev/null || true
    DOWN=$(sed -n 's/^down after \([0-9]*\) ms$/\1/p' "$TICKD/out")
    [ -n "$DOWN" ] && [ "$DOWN" -lt 5000 ] || fail "node ticks: $(cat "$TICKD/out")"
    rm -rf "${TICKD:?}"
    ;;
esac

# mlwsvm names, and nodes that register with it and connect by name: node b
# registers and waits, node a registers too, connects to b by name, checks the
# service's answers, and tells b to end. The service takes any free port (0),
# so as not to meet a service already running here, and says which.
NAMESD=$(mktemp -d)
$BIN names --port 0 > "$NAMESD/names.out" 2>&1 &
NAMES_PID=$!
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
    grep -q "^mlwsvm names: on port [0-9]" "$NAMESD/names.out" 2>/dev/null && break
    perl -e 'select(undef, undef, undef, 0.25)'
done
grep -q "^mlwsvm names: on port [0-9]" "$NAMESD/names.out" || { kill $NAMES_PID 2>/dev/null || true; fail "names did not start ($(cat "$NAMESD/names.out"))"; }
MLWSVM_NAMES_PORT=$(sed -n 's/^mlwsvm names: on port \([0-9]*\).*/\1/p' "$NAMESD/names.out")
export MLWSVM_NAMES_PORT
$BIN run tests/tools/_names/b.masm > "$NAMESD/b.port" 2>/dev/null &
B_PID=$!
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
    [ -s "$NAMESD/b.port" ] && break
    perl -e 'select(undef, undef, undef, 0.25)'
done
perl -e 'alarm 30; exec @ARGV' $BIN run tests/tools/_names/a.masm >/dev/null 2>"$NAMESD/err" || { kill $B_PID $NAMES_PID 2>/dev/null || true; cat "$NAMESD/err" >&2; fail "names: connecting by name"; }
wait $B_PID 2>/dev/null || true
# Two nodes connecting to each other at once, for messages: one connection
# stays at both ends, and both messages arrive.
$BIN run tests/tools/_race/rb.masm > "$NAMESD/rb.port" 2>/dev/null &
RB_PID=$!
for _ in $(seq 1 40); do [ -s "$NAMESD/rb.port" ] && break; perl -e 'select(undef, undef, undef, 0.25)'; done
OUT=$(perl -e 'alarm 30; exec @ARGV' $BIN run tests/tools/_race/ra.masm 2>&1) || { kill $RB_PID $NAMES_PID 2>/dev/null || true; fail "nodes connecting at once: $OUT"; }
echo "$OUT" | grep -q '^ok, nodes connected: 1$' || { kill $RB_PID $NAMES_PID 2>/dev/null || true; fail "nodes connecting at once: $OUT"; }
kill $RB_PID 2>/dev/null || true
wait $RB_PID 2>/dev/null || true
kill $NAMES_PID 2>/dev/null || true
wait $NAMES_PID 2>/dev/null || true
unset MLWSVM_NAMES_PORT
set +e
$BIN names --bogus >/dev/null 2>&1; RC=$?
set -e
[ $RC = 2 ] || fail "names: a bad option (exit $RC)"
rm -rf "${NAMESD:?}"

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
mkdir "$CDIR/src" # (the cache directory is the VM's to make: private, so trusted)
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

# An uncaught Error is reported with its own backtrace, from where it was
# made, though a handler caught and rethrew it.
set +e
OUT=$($BIN run tests/exceptions/err_uncaught_trap.masm 2>&1)
set -e
echo "$OUT" | grep -q "^  at err_uncaught_trap.store (tests/exceptions/err_uncaught_trap.masm:8)$" ||
    fail "uncaught: not the backtrace of the fault ($OUT)"

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
    ncall "proc.register" ("d_main", PID)
    spawn child ()
    recv I0                              ; e has seen the child: end
    ret 0
.endf
MASM
cat > "$PDIR/e.masm" <<'MASM'
.include "std/io.inc"
; Waits (at most 5 s each) for d's child to run, tells d's main to end, and
; waits for the child to be stopped: events, not times, so a slow machine
; does not change the answer.
.func main
    mov I1, 0
1:  ncall "proc.whereis" ("d_child") -> (I0)
    bnz I0, 2f
    sleep 5
    inc I1
    blt I1, 1000, 1b
    PRINTLN_S "d's child never ran"
    ret 1
2:  ncall "proc.whereis" ("d_main") -> (I2)
    send I2, 1
    mov I1, 0
3:  sleep 5
    ncall "proc.whereis" ("d_child") -> (I0)
    bz I0, 4f
    inc I1
    blt I1, 1000, 3b
    PRINTLN_S "d's child still runs"
    ret 1
4:  PRINTLN_S "d's child was stopped"
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
echo "$OUT" | grep -q "util.who \[.*/y/util.masm\]" || fail "--also: profile without util's file ($OUT)"
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

# mlwsvm fuzz: the program for a seed is the same each time, and runs; a
# few seeds agree with the interpreter in every mode; and --reduce, given a VM
# whose JIT "miscompiles" any program with a marked line (a script that adds a
# line of output when --jit is given), keeps that line and little else.
FDIR=$(mktemp -d)
$BIN fuzz --emit 7 > "$FDIR/a.masm" && $BIN fuzz --emit 7 > "$FDIR/b.masm" || fail "fuzz --emit"
cmp -s "$FDIR/a.masm" "$FDIR/b.masm" || fail "fuzz --emit: not the same program twice"
$BIN run --no-cache "$FDIR/a.masm" >/dev/null || fail "fuzz --emit: the program fails"
$BIN fuzz --list | grep -q "^jit-verify " || fail "fuzz --list"
$BIN fuzz -n 20 --seed 1 -j 4 --out "$FDIR/out" 2>"$FDIR/log" || fail "fuzz: $(tail -3 "$FDIR/log")"
grep -q "20 programs (seeds 1-20), 220 runs, 0 skipped, 0 failed" "$FDIR/log" || fail "fuzz: $(tail -1 "$FDIR/log")"
# (Not on Windows, which cannot start the shell script that stands in for a
# VM here as a program.)
case "$(uname -s)" in
MINGW* | MSYS* | CYGWIN*) ;;
*)
    REAL="$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")"
    {
        echo '#!/bin/sh'
        echo 'for a; do f=$a; done'
        echo 'case " $* " in *" --jit "*) grep -q MARK "$f" && echo miscompiled;; esac'
        echo "exec \"$REAL\" \"\$@\""
    } > "$FDIR/vm"
    chmod +x "$FDIR/vm"
    awk '{ print } /^\.func f0/ { print "    nop                                ; MARK" }' "$FDIR/a.masm" > "$FDIR/c.masm"
    $BIN fuzz --vm "$FDIR/vm" --reduce "$FDIR/c.masm" --modes jit --out "$FDIR/out" 2>"$FDIR/log" ||
        fail "fuzz --reduce: $(tail -2 "$FDIR/log")"
    grep -q MARK "$FDIR/c.reduced.masm" || fail "fuzz --reduce lost the marked line"
    [ "$(wc -l < "$FDIR/c.reduced.masm")" -lt 10 ] || fail "fuzz --reduce left $(wc -l < "$FDIR/c.reduced.masm") lines"
    ;;
esac
rm -rf "${FDIR:?}"

# --trace with a limit
N=$($BIN run --trace - --trace-limit 5 examples/hello.masm 2>&1 >/dev/null | grep -c '^\[1\]')
[ "$N" = 5 ] || fail "trace limit (got $N lines)"

# --jit-report: what the JIT compiled (fib, after 2 calls; main runs once),
# or that this build has no JIT; under the profiler's hook it compiles
# nothing, so the profile is the interpreter's.
OUT=$($BIN run --jit on --jit-hot 2 --jit-report - examples/fib.masm 2>&1 >/dev/null)
if ! echo "$OUT" | grep -q "not in this build"; then
    echo "$OUT" | grep -q "^mlwsvm jit (.*): a function compiled after 2 calls" || fail "--jit-report ($OUT)"
    echo "$OUT" | grep -q "functions compiled  1$" || fail "--jit-report: fib alone ($OUT)"
    OUT=$($BIN run --jit on --jit-hot 2 --jit-report - --profile examples/fib.masm 2>&1 >/dev/null)
    echo "$OUT" | grep -q "fib.fib" || fail "--profile with --jit on ($OUT)"
    echo "$OUT" | grep -q "^  nothing compiled" || fail "--jit under the profiler compiled something ($OUT)"
fi

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
    curl -sf http://127.0.0.1:$PORT/api/modules | grep -q '"name":"workers","path":"[^"]*","version":1,"current":true,"state":"ready"' || fail "/api/modules"
    curl -sf http://127.0.0.1:$PORT/api/nodes | grep -q '^{"self":0,"name":"","connected":\[\]}$' || fail "/api/nodes"
fi
kill $VMPID 2>/dev/null || true
wait $VMPID 2>/dev/null || true

# The log channel, the registry, links and monitors, and a mailbox, while a
# program runs: tests/tools/_monitor/watched.masm.
if command -v curl >/dev/null 2>&1; then
    $BIN run --monitor 127.0.0.1:$PORT tests/tools/_monitor/watched.masm >/dev/null 2>&1 &
    VMPID=$!
    i=0
    until curl -sf "http://127.0.0.1:$PORT/api/log" 2>/dev/null | grep -q '"level":"warn","text":"E_JSON"'; do
        i=$((i + 1)); [ $i -lt 50 ] || fail "monitor log did not start"; sleep 0.1
    done
    LOG=$(curl -sf "http://127.0.0.1:$PORT/api/log")
    echo "$LOG" | grep -q '"level":"info","text":"watched: started"' || fail "/api/log: a line ($LOG)"
    echo "$LOG" | grep -q '"level":"error","text":"an \\"error\\"\\u000ain two lines"' || fail "/api/log: escaped ($LOG)"
    echo "$LOG" | grep -q '"event":"ready","value":\[42,"x"\]' || fail "/api/log: an event ($LOG)"
    curl -sf "http://127.0.0.1:$PORT/api/log?since=3" | grep -q '^{"next":4,"lost":0,"entries":\[{"seq":4,' || fail "/api/log?since"
    # mlwsvm top --log: the same lines as text (time, pid, program, level,
    # text), a line with a newline in it on two, and none of /log's first line
    TLOG=$($BIN top --log --once 127.0.0.1:$PORT)
    echo "$TLOG" | grep -Eq '^ +[0-9]+\.[0-9]{3} s +1  watched\.masm  info   watched: started' || fail "top --log: a line ($TLOG)"
    echo "$TLOG" | grep -Eq '  watched\.masm  error  an "error"' || fail "top --log: an error ($TLOG)"
    echo "$TLOG" | grep -q '^in two lines' || fail "top --log: a line's second line ($TLOG)"
    echo "$TLOG" | grep -Eq '  watched\.masm  event  ready \[42,"x"\]' || fail "top --log: an event ($TLOG)"
    echo "$TLOG" | grep -Eq '  watched\.masm  warn   E_JSON' || fail "top --log: a warning ($TLOG)"
    [ "$(echo "$TLOG" | wc -l | tr -d ' ')" = 5 ] || fail "top --log: lines ($TLOG)"
    curl -sf "http://127.0.0.1:$PORT/log?since=3" | head -1 | grep -q '^4' || fail "/log?since: the number to ask from next"
    [ "$(curl -sf "http://127.0.0.1:$PORT/log?since=3" | wc -l | tr -d ' ')" = 2 ] || fail "/log?since: one line after 3"
    curl -sf http://127.0.0.1:$PORT/api/names | grep -q '{"name":"sleeper","pid":2}' || fail "/api/names"
    curl -sf http://127.0.0.1:$PORT/api/graph | grep -q '"links":\[\[1,2\]\],"monitors":\[\[1,2\]\]' || fail "/api/graph"
    curl -sf http://127.0.0.1:$PORT/api/proc/2 | grep -q '"messages":\[{"type":"string","size":5,"preview":"first"},{"type":"i64","preview":7}\]' || fail "/api/proc: messages"
    # without --monitor-actions, no action
    [ "$(curl -s -o /dev/null -w '%{http_code}' -X POST http://127.0.0.1:$PORT/api/proc/2/kill)" = 403 ] || fail "an action without --monitor-actions"
    kill $VMPID 2>/dev/null || true
    wait $VMPID 2>/dev/null || true

    MOND=$(mktemp -d)
    # Actions (--monitor-actions): the token, the Host, the Origin; then send,
    # reload, purge and kill, each in the log.
    MLWSVM_MONITOR_TOKEN=s3cret $BIN run --monitor 127.0.0.1:$PORT --monitor-actions tests/tools/_monitor/acted.masm >/dev/null 2>"$MOND/acted.err" &
    VMPID=$!
    i=0
    until curl -sf http://127.0.0.1:$PORT/api/names 2>/dev/null | grep -q '"receiver"'; do
        i=$((i + 1)); [ $i -lt 50 ] || fail "monitor actions did not start"; sleep 0.1
    done
    grep -q '^mlwsvm: monitor at http://127.0.0.1:[0-9]*/#token=s3cret ' "$MOND/acted.err" || fail "the token printed"
    U=http://127.0.0.1:$PORT
    code() { curl -s -o "$MOND/r" -w '%{http_code}' -X POST "$@"; }
    [ "$(code $U/api/proc/2/kill)" = 401 ] || fail "an action without the token"
    [ "$(code -H 'Authorization: Bearer nope' $U/api/proc/2/kill)" = 401 ] || fail "an action with the wrong token"
    [ "$(code -H 'Authorization: Bearer s3cret' -H 'Origin: http://evil.example' $U/api/proc/2/kill)" = 403 ] || fail "an action from another site"
    [ "$(code -H 'Authorization: Bearer s3cret' -H 'Host: evil.example' $U/api/proc/2/kill)" = 403 ] || fail "an action for another Host"
    A='Authorization: Bearer s3cret'
    [ "$(code -H "$A" -H "Origin: $U" --data '{"n":[1,2.5,"x"]}' $U/api/proc/2/send)" = 200 ] || fail "send"
    [ "$(code -H "$A" --data '{"n":' $U/api/proc/2/send)" = 400 ] || fail "send: not JSON"
    [ "$(code -H "$A" --data '1' $U/api/proc/99/send)" = 404 ] || fail "send: no such process"
    i=0
    until curl -sf "$U/api/log" | grep -q '"level":"info","text":"got {\\"n\\":\[1,2.5,\\"x\\"\]}"'; do
        i=$((i + 1)); [ $i -lt 50 ] || fail "send: the message did not arrive ($(curl -sf $U/api/log))"; sleep 0.1
    done
    [ "$(code -H "$A" $U/api/modules/acted/reload)" = 200 ] || fail "reload"
    curl -sf $U/api/modules | grep -q '"name":"acted","path":"[^"]*","version":2,"current":true' || fail "reload: version 2"
    [ "$(code -H "$A" $U/api/modules/nope/reload)" = 404 ] || fail "reload: no such module"
    [ "$(code -H "$A" $U/api/modules/acted/purge)" = 200 ] && grep -q '"kept":1' "$MOND/r" || fail "purge ($(cat "$MOND/r"))"
    [ "$(code -H "$A" $U/api/proc/2/kill)" = 200 ] || fail "kill"
    curl -sf $U/api/exits | grep -q '"pid":2,"entry":"receiver","program":0,"program_name":"acted.masm","main":false,"failed":true,"code":"E_KILLED"' || fail "kill: E_KILLED"
    LOG=$(curl -sf "$U/api/log")
    for e in send reload purge kill; do
        echo "$LOG" | grep -q "\"event\":\"monitor.$e\"" || fail "monitor.$e not in the log"
    done
    kill $VMPID 2>/dev/null || true
    wait $VMPID 2>/dev/null || true

    # Over TLS (--monitor-cert, --monitor-key), where there is a TLS library.
    printf '.func main\n    ncall "tls.available" () -> (I0)\n    ret I0\n.endf\n' > "$MOND/tls.masm"
    set +e
    $BIN run "$MOND/tls.masm"; TLS_HERE=$?
    set -e
    if [ $TLS_HERE = 1 ]; then
        MLWSVM_MONITOR_TOKEN=s3cret $BIN run --monitor 127.0.0.1:$PORT --monitor-actions --monitor-cert tests/net/_tls/cert.pem \
            --monitor-key tests/net/_tls/key.pem tests/tools/_monitor/acted.masm >/dev/null 2>"$MOND/tls.err" &
        VMPID=$!
        i=0
        until curl -sf --cacert tests/net/_tls/cert.pem https://localhost:$PORT/api/names 2>/dev/null | grep -q '"receiver"'; do
            i=$((i + 1)); [ $i -lt 50 ] || fail "monitor over TLS did not start"; sleep 0.1
        done
        grep -q "^mlwsvm: monitor at https://" "$MOND/tls.err" || fail "TLS: https printed"
        [ "$(curl -s -o /dev/null -w '%{http_code}' --cacert tests/net/_tls/cert.pem -X POST -H "$A" https://localhost:$PORT/api/proc/2/kill)" = 200 ] || fail "TLS: an action"
        [ "$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:$PORT/api/vm)" != 200 ] || fail "TLS: plain HTTP answered"
        # mlwsvm top over HTTPS: with the authority, from the address the run
        # printed too; without it, the certificate is refused.
        $BIN top --once --ca tests/net/_tls/cert.pem https://localhost:$PORT | grep -q "recent exits" || fail "TLS: top"
        $BIN top --log --once --ca tests/net/_tls/cert.pem https://localhost:$PORT | grep -q '  monitor  event  monitor\.kill {"by":"(start)","pid":2}' || fail "TLS: top --log"
        $BIN top --once --ca tests/net/_tls/cert.pem "https://localhost:$PORT/#token=s3cret" | grep -q "PID" || fail "TLS: top from the printed address"
        set +e
        TOPERR=$($BIN top --once https://localhost:$PORT 2>&1 >/dev/null); RC=$?
        set -e
        [ $RC = 1 ] && echo "$TOPERR" | grep -q "^mlwsvm top: https://localhost:$PORT: " || fail "TLS: top without the authority (exit $RC): $TOPERR"
        kill $VMPID 2>/dev/null || true
        wait $VMPID 2>/dev/null || true
    fi

    # Keys (--monitor-tokens, mlwsvm token): a tokens file holds each key's
    # hash; with one, every request but the page's needs a key; a read key
    # reads, an admin's acts and manages keys, each action in the log by its
    # key's label; the file is read again when it changes.
    KF="$MOND/tokens"
    RK=$($BIN token new --label ops --file "$KF" 2>/dev/null)
    grep -q '^read ops [0-9a-f]\{64\}$' "$KF" || fail "token new: the file ($(cat "$KF"))"
    grep -q "$RK" "$KF" && fail "token new: the token itself in the file"
    [ "$($BIN token list --file "$KF")" = "read   ops" ] || fail "token list"
    set +e
    $BIN token new --label ops --file "$KF" >/dev/null 2>&1; RC=$?
    set -e
    [ $RC = 1 ] || fail "token new: a label used twice (exit $RC)"
    printf 'read bad\n' > "$MOND/badtokens"
    set +e
    BADERR=$($BIN run --monitor 127.0.0.1:$PORT --monitor-tokens "$MOND/badtokens" examples/hello.masm 2>&1 >/dev/null); RC=$?
    set -e
    [ $RC = 1 ] && echo "$BADERR" | grep -q 'badtokens:1: expected SCOPE LABEL SHA256' || fail "a tokens file that is not one (exit $RC): $BADERR"
    MLWSVM_MONITOR_TOKEN=s3cret $BIN run --monitor 127.0.0.1:$PORT --monitor-actions --monitor-tokens "$KF" tests/tools/_monitor/acted.masm >/dev/null 2>"$MOND/keys.err" &
    VMPID=$!
    i=0
    until curl -sf -H "$A" $U/api/names 2>/dev/null | grep -q '"receiver"'; do
        i=$((i + 1)); [ $i -lt 50 ] || fail "keys: the monitor did not start"; sleep 0.1
    done
    get() { curl -s -o "$MOND/r" -w '%{http_code}' "$@"; }
    [ "$(get $U/api/procs)" = 401 ] || fail "keys: a read without a key"
    [ "$(get $U/)" = 200 ] || fail "keys: the page without a key"
    [ "$(get $U/debug)" = 200 ] || fail "keys: the debugger's page without a key"
    [ "$(get -H 'Authorization: Bearer nope' $U/api/procs)" = 401 ] || fail "keys: a read with no such key"
    R="Authorization: Bearer $RK"
    [ "$(get -H "$R" $U/api/procs)" = 200 ] || fail "keys: a read with a read key"
    [ "$(get -H "$R" $U/api/vm)" = 200 ] && grep -q '"keys":true,"you":{"label":"ops","scope":"read"}' "$MOND/r" || fail "keys: /api/vm says whose ($(cat "$MOND/r"))"
    [ "$(get -H "$R" $U/api/keys)" = 403 ] || fail "keys: the keys to a read key"
    [ "$(get -H "$R" $U/api/debug)" = 403 ] || fail "keys: the debugger to a read key"
    [ "$(get -H "$R" $U/api/debug/2/vars)" = 403 ] || fail "keys: a process's variables to a read key"
    [ "$(code -H "$R" $U/api/proc/2/kill)" = 403 ] || fail "keys: an action with a read key"
    [ "$(get -H "$A" $U/api/keys)" = 200 ] && grep -q '"label":"ops","scope":"read","from":"file","used_ago_ms":[0-9]' "$MOND/r" \
        && grep -q '"label":"(start)","scope":"admin","from":"start"' "$MOND/r" || fail "keys: the list ($(cat "$MOND/r"))"
    [ "$(code -H "$A" --data '{"label":"ci","scope":"admin"}' $U/api/keys)" = 200 ] || fail "keys: make one ($(cat "$MOND/r"))"
    CK=$(sed 's/.*"token":"\([0-9a-f]*\)".*/\1/' "$MOND/r")
    grep -q '^admin ci [0-9a-f]\{64\}$' "$KF" || fail "keys: a key made, into the file"
    [ "$(code -H "$A" --data '{"label":"ci"}' $U/api/keys)" = 409 ] || fail "keys: a label used twice"
    [ "$(code -H "$A" --data '{"label":"no spaces"}' $U/api/keys)" = 400 ] || fail "keys: a bad label"
    [ "$(code -H "Authorization: Bearer $CK" $U/api/keys/ops/revoke)" = 200 ] || fail "keys: revoke ($(cat "$MOND/r"))"
    [ "$(get -H "$R" $U/api/procs)" = 401 ] || fail "keys: a revoked key"
    grep -q ' ops ' "$KF" && fail "keys: a revoked key still in the file"
    [ "$(code -H "Authorization: Bearer $CK" $U/api/keys/ci/revoke)" = 409 ] || fail "keys: a key revoking itself"
    LK=$($BIN token new --label late --file "$KF" 2>/dev/null)
    [ "$(get -H "Authorization: Bearer $LK" $U/api/procs)" = 200 ] || fail "keys: one added to the file while it runs"
    $BIN top --once --token "$LK" $U | grep -q "PID" || fail "keys: top --token"
    $BIN top --once "$U/#token=$LK" | grep -q "PID" || fail "keys: top from an address with the key"
    set +e
    TOPERR=$($BIN top --once $U 2>&1 >/dev/null); RC=$?
    set -e
    [ $RC = 1 ] && echo "$TOPERR" | grep -q 'needs a key' || fail "keys: top without a key (exit $RC): $TOPERR"
    $BIN top --log --once --token "$LK" $U | grep -Eq ' s +-  monitor  event  monitor\.key_new \{"by":"\(start\)","label":"ci","scope":"admin"\}' || fail "keys: top --log --token"
    set +e
    TOPERR=$($BIN top --log --once $U 2>&1 >/dev/null); RC=$?
    set -e
    [ $RC = 1 ] && echo "$TOPERR" | grep -q 'needs a key' || fail "keys: top --log without a key (exit $RC): $TOPERR"
    [ "$(code -H "Authorization: Bearer $CK" $U/api/proc/2/kill)" = 200 ] || fail "keys: an action with a key made"
    LOG=$(curl -sf -H "$A" "$U/api/log")
    echo "$LOG" | grep -q '"event":"monitor.key_new","value":{"by":"(start)","label":"ci","scope":"admin"}' || fail "keys: key_new in the log ($LOG)"
    echo "$LOG" | grep -q '"event":"monitor.key_revoke","value":{"by":"ci","label":"ops"}' || fail "keys: key_revoke in the log"
    echo "$LOG" | grep -q '"event":"monitor.kill","value":{"by":"ci","pid":2}' || fail "keys: an action's key in the log"
    $BIN token revoke --label late --file "$KF" || fail "token revoke"
    [ "$($BIN token list --file "$KF")" = "admin  ci" ] || fail "token revoke: the file ($(cat "$KF"))"
    kill $VMPID 2>/dev/null || true
    wait $VMPID 2>/dev/null || true

    # The monitor where others can reach it. No client holds it up: beside one
    # that sends half a request, and one that reads a big answer slowly, others
    # are answered at once and the program goes on. Headers too big are refused.
    # The page says it is for no frame and runs only its own script. Wrong keys
    # from an address are counted, the same one again once: past ten, a key from
    # there is refused unread for a few seconds, which the log says.
    MLWSVM_MONITOR_TOKEN=s3cret $BIN run --monitor 127.0.0.1:$PORT --monitor-actions --monitor-tokens "$KF" tests/tools/_monitor/many.masm >/dev/null 2>"$MOND/net.err" &
    VMPID=$!
    i=0
    until curl -sf -H "$A" $U/api/names 2>/dev/null | grep -q '"ticker"'; do
        i=$((i + 1)); [ $i -lt 100 ] || fail "net: the monitor did not start"; sleep 0.1
    done
    curl -s --max-time 10 -o /dev/null -H "$A" -H 'Content-Length: 5000' --data 'x' $U/api/proc/1/send &
    SLOW1=$!
    curl -s --max-time 10 --limit-rate 2000 -o /dev/null -H "$A" $U/api/procs &
    SLOW2=$!
    sleep 0.5 # (each asked with a second to answer in: a monitor held up fails here, not later)
    [ "$(curl -s --max-time 1 -o "$MOND/r" -w '%{http_code}' -H "$A" $U/api/vm)" = 200 ] || fail "net: a request beside a slow sender and a slow reader"
    T0=$(curl -s --max-time 1 -H "$A" "$U/api/log" | sed 's/.*"next":\([0-9]*\).*/\1/')
    sleep 1
    T1=$(curl -s --max-time 1 -H "$A" "$U/api/log" | sed 's/.*"next":\([0-9]*\).*/\1/')
    [ $((T1 - T0)) -ge 5 ] || fail "net: the program held up by slow clients ($T0 to $T1 log lines in a second)"
    kill $SLOW1 $SLOW2 2>/dev/null || true
    wait $SLOW1 $SLOW2 2>/dev/null || true
    BIG=$(printf 'a%.0s' $(seq 1 900))
    GOT=$(get -H "$A" -H "X-A: $BIG$BIG$BIG$BIG$BIG" -H "X-B: $BIG$BIG$BIG$BIG$BIG" $U/api/vm)
    [ "$GOT" = 431 ] || fail "net: headers over 8 KiB ($GOT)"
    GOT=$(head -c 70000 /dev/zero | tr '\0' 'a' | code -H "$A" --data-binary @- $U/api/proc/1/send)
    [ "$GOT" = 413 ] && grep -q 'over 64 KiB' "$MOND/r" || fail "net: a body over 64 KiB ($GOT)"
    HEAD=$(curl -s -D - -o /dev/null $U/ | tr -d '\r')
    echo "$HEAD" | grep -qi '^X-Frame-Options: DENY$' || fail "net: the page's frame header ($HEAD)"
    echo "$HEAD" | grep -qi "^Content-Security-Policy: default-src 'none'; .*frame-ancestors 'none'$" || fail "net: the page's policy ($HEAD)"
    curl -s -D - -o /dev/null -H "$A" $U/api/vm | grep -qi '^X-Content-Type-Options: nosniff' || fail "net: nosniff"
    for i in 1 2 3 4 5 6 7 8 9 10 11 12; do # the same wrong key: counted once
        [ "$(get -H 'Authorization: Bearer wrong0' $U/api/vm)" = 401 ] || fail "net: the same wrong key again ($i)"
    done
    i=1
    until [ "$(get -H "Authorization: Bearer wrong$i" $U/api/vm)" = 429 ]; do
        i=$((i + 1)); [ $i -lt 40 ] || fail "net: wrong keys never refused"
    done
    [ $i -ge 10 ] || fail "net: refused after $i wrong keys"
    [ "$(get -H "$A" $U/api/vm)" = 429 ] && grep -q 'too many wrong keys' "$MOND/r" || fail "net: a key from there while refused ($(cat "$MOND/r"))"
    [ "$(get $U/)" = 200 ] || fail "net: the page while keys are refused"
    [ "$(get $U/api/vm)" = 401 ] || fail "net: no key while keys are refused"
    set +e
    TOPERR=$($BIN top --once --token s3cret $U 2>&1 >/dev/null); RC=$?
    set -e
    [ $RC = 1 ] && echo "$TOPERR" | grep -q 'too many wrong keys' || fail "net: top while refused (exit $RC): $TOPERR"
    i=0
    until [ "$(get -H "$A" $U/api/vm)" = 200 ]; do # one more every three seconds
        i=$((i + 1)); [ $i -lt 20 ] || fail "net: keys still refused after ten seconds"; sleep 0.5
    done
    [ $i -ge 1 ] || fail "net: keys refused for no time"
    curl -sf -H "$A" "$U/api/log" | grep -q '"event":"monitor.keys_refused","value":{"from":"127.0.0.1"}' || fail "net: keys_refused in the log"
    # The tokens file is the monitor's to trust: one that other users can
    # write gives no keys (those it had stay), and no monitor starts on one.
    # mlwsvm token writes one that only its owner can, whatever the umask.
    case "$(uname -s)" in
    MINGW* | MSYS* | CYGWIN*) ;;
    *)
        chmod 666 "$KF"
        [ "$(get -H "$A" $U/api/vm)" = 200 ] || fail "net: the keys it had, with the file open to others"
        curl -sf -H "$A" "$U/api/log" | grep -q '"event":"monitor.keys_unread","value":{"error":"[^"]*tokens is writable by other users (mode 666)"}' || fail "net: keys_unread in the log"
        set +e
        BADERR=$($BIN run --monitor 127.0.0.1:$((PORT + 1)) --monitor-tokens "$KF" examples/hello.masm 2>&1 >/dev/null); RC=$?
        set -e
        [ $RC = 1 ] && echo "$BADERR" | grep -q 'tokens is writable by other users' || fail "a tokens file others can write (exit $RC): $BADERR"
        chmod 600 "$KF"
        (umask 000; $BIN token new --label open --file "$MOND/tokens2" >/dev/null 2>&1; $BIN token new --label two --file "$MOND/tokens2" >/dev/null 2>&1)
        case "$(ls -l "$MOND/tokens2")" in
        -rw-r--r--*) ;;
        *) fail "token new: the file's mode ($(ls -l "$MOND/tokens2"))" ;;
        esac
        ;;
    esac
    kill $VMPID 2>/dev/null || true
    wait $VMPID 2>/dev/null || true

    # A process debugged alone (/api/debug), the rest going on: an admin's to
    # do. Attached and asked to pause, a process stops before its next
    # instruction; its frames, its variables (one shown by its formatter) and
    # what they open to show, its code and its source are read; it steps by
    # lines and by instructions, into a call, out of one and over one; a
    # breakpoint at a function or a line stops it, one on a line without code
    # at the next that has some, and one for code not loaded waits; a `break`
    # instruction stops it; a breakpoint on a recv stops it once, though the
    # recv runs again when a message comes; detached, it runs as before; killed
    # while paused, it ends.
    DF=tests/tools/_monitor/debugged.masm
    MLWSVM_MONITOR_TOKEN=s3cret $BIN run --monitor 127.0.0.1:$PORT --monitor-actions $DF >/dev/null 2>"$MOND/debug.err" &
    VMPID=$!
    i=0
    until curl -sf -H "$A" $U/api/names 2>/dev/null | grep -q '"breaker"'; do
        i=$((i + 1)); [ $i -lt 50 ] || fail "debug: the monitor did not start"; sleep 0.1
    done
    D=$U/api/debug
    pid_of() { curl -sf -H "$A" $U/api/names | sed "s/.*{\"name\":\"$1\",\"pid\":\([0-9]*\)}.*/\1/"; }
    W=$(pid_of worker); O=$(pid_of other); B=$(pid_of breaker)
    line_of() { grep -n -- "$1" $DF | head -1 | cut -d: -f1; }
    # break_at FILE LINE: a breakpoint asked for; the status, with the answer in $MOND/r
    break_at() {
        BODY="{\"file\":\"$1\",\"line\":$2}"
        code -H "$A" --data "$BODY" $D/breakpoints
    }
    L_SLEEP=$(line_of 'sleep 5'); L_NOTE=$(line_of '^; (I, R)'); L_RECV=$(line_of 'recv R0')
    # paused PID N: wait until it is paused for the Nth time; its status is in $MOND/st
    paused() {
        TRIES=0
        until curl -s -H "$A" $D/$1 > "$MOND/st" && grep -q "\"state\":\"paused\".*\"seq\":$2," "$MOND/st"; do
            TRIES=$((TRIES + 1)); [ $TRIES -lt 50 ] || fail "debug: process $1 not paused for the ${2}th time ($(cat "$MOND/st"))"; sleep 0.1
        done
    }
    [ "$(get $D)" = 401 ] || fail "debug: without a key"
    [ "$(get -H "$A" $D)" = 200 ] && grep -q '^{"available":true,"why":null,"breakpoints":\[\],"processes":\[\]}$' "$MOND/r" || fail "debug: nothing debugged yet ($(cat "$MOND/r"))"
    [ "$(code -H "$A" --data '{"kind":"over"}' $D/$W/step)" = 409 ] && grep -q 'attach first' "$MOND/r" || fail "debug: a step of a process not debugged"
    [ "$(code -H "$A" $D/9999/attach)" = 404 ] || fail "debug: no such process"
    [ "$(code -H "$A" --data '{"pause":true}' $D/$W/attach)" = 200 ] || fail "debug: attach ($(cat "$MOND/r"))"
    paused $W 1
    grep -q '"debugged":true,"state":"paused","away":false,"pausing":false,"stepping":false,"reason":"pause","error":null,"seq":1,"children":false,"entry":"worker"' "$MOND/st" || fail "debug: paused ($(cat "$MOND/st"))"
    grep -q '"frames":\[{"function":"debugged.worker","pc":[0-9]*,"file":"[^"]*debugged.masm","line":[0-9]*}\]}$' "$MOND/st" || fail "debug: its frames ($(cat "$MOND/st"))"
    curl -sf -H "$A" $U/api/proc/$W | grep -q "^{\"pid\":$W,\"state\":\"paused\"," || fail "debug: /api/proc says paused"
    $BIN top --once 127.0.0.1:$PORT | grep -Eq "^ +$W paused " || fail "debug: top says paused"
    T0=$(curl -s -H "$A" "$U/api/log" | sed 's/.*"next":\([0-9]*\).*/\1/'); sleep 0.5
    T1=$(curl -s -H "$A" "$U/api/log" | sed 's/.*"next":\([0-9]*\).*/\1/')
    [ $((T1 - T0)) -ge 5 ] || fail "debug: another process held up while one is paused ($T0 to $T1 log lines)"
    curl -sf -H "$A" $D | grep -q "\"processes\":\[{\"pid\":$W,\"debugged\":true,\"state\":\"paused\"" || fail "debug: the processes debugged"
    VARS=$(curl -s -H "$A" "$D/$W/vars")
    echo "$VARS" | grep -q '^{"locals":\[{"name":"count","value":"[0-9]*","kind":"i64","children":0,"path":"L0"},{"name":"seen","value":"Seen {count: [0-9]*, last: null}","kind":"Seen","children":2,"path":"L1"},{"name":"all","value":"list([0-9]) \[[0-9, ]*\]","kind":"list","children":[0-9],"path":"L2"},{"name":"tagged","value":"int 7","kind":"registers","children":2,"path":"L3"}\],"registers":{"I":\[{"name":"I0"' \
        || fail "debug: variables ($VARS)"
    curl -s -H "$A" "$D/$W/vars?path=L1" | grep -q '^{"path":"L1","count":2,"from":0,"children":\[{"name":"count","value":"[0-9]*","kind":"i64","children":0,"path":"L1/0"},{"name":"last","value":"null","kind":"null","children":0,"path":"L1/1"}\]}$' || fail "debug: a struct opened"
    curl -s -H "$A" "$D/$W/vars?path=L3" | grep -q '"children":\[{"name":"I3","value":"7","kind":"i64","children":0,"path":"L3/0"},{"name":"R3","value":"null","kind":"null","children":0,"path":"L3/1"}\]' || fail "debug: a variable's registers"
    [ "$(get -H "$A" "$D/$W/vars?path=R2/99")" = 404 ] || fail "debug: a value that is not there"
    [ "$(get -H "$A" "$D/$W/vars?frame=7")" = 404 ] || fail "debug: a frame that is not there"
    curl -s -H "$A" "$D/$W/code" | grep -q '^{"function":"debugged.worker","pc":[0-9]*,"file":"[^"]*","insns":\[{"pc":0,"line":[0-9]*,"text":"ncall' || fail "debug: its code"
    curl -s -H "$A" "$D/$W/code" | grep -q "{\"pc\":[0-9]*,\"line\":$(line_of 'sleep 5'),\"text\":\"sleep 5\",\"breakpoint\":false}" || fail "debug: an instruction and its line"
    [ "$(get -H "$A" "$D/source?file=$DF")" = 200 ] && grep -q '^{"file":"[^"]*","text":"; Debugged by check_tools.sh' "$MOND/r" || fail "debug: its source"
    [ "$(get -H "$A" "$D/source?file=/etc/passwd")" = 404 ] || fail "debug: a file no code came from"
    # a breakpoint at a function; out of it; over the next lines, the sleep among them; into it again
    [ "$(code -H "$A" --data '{"function":"bump"}' $D/breakpoints)" = 200 ] && grep -q "^{\"id\":1,\"file\":null,\"line\":0,\"line_at\":0,\"function\":\"bump\",\"at\":\[{\"function\":\"debugged.bump\",\"pc\":0,\"file\":\"[^\"]*\",\"line\":$(line_of 'add I1, n, 1')}\]}$" "$MOND/r" \
        || fail "debug: a breakpoint at a function ($(cat "$MOND/r"))"
    [ "$(code -H "$A" $D/$W/continue)" = 200 ] || fail "debug: continue"
    paused $W 2
    grep -q '"reason":"breakpoint".*"frames":\[{"function":"debugged.bump","pc":0,[^}]*},{"function":"debugged.worker",' "$MOND/st" || fail "debug: at the breakpoint ($(cat "$MOND/st"))"
    curl -s -H "$A" "$D/$W/vars?frame=1" | grep -q '"name":"count"' || fail "debug: the caller's variables"
    curl -s -H "$A" "$D/$W/code" | grep -q '"insns":\[{"pc":0,"line":[0-9]*,"text":"add.i64 I1, I0, 1","breakpoint":true}' || fail "debug: the breakpoint in the code"
    [ "$(code -H "$A" $D/breakpoints/1/delete)" = 200 ] || fail "debug: delete a breakpoint"
    [ "$(code -H "$A" $D/breakpoints/1/delete)" = 404 ] || fail "debug: delete it again"
    [ "$(code -H "$A" --data '{"kind":"out"}' $D/$W/step)" = 200 ] || fail "debug: step out"
    paused $W 3
    grep -q "\"reason\":\"step\".*\"frames\":\[{\"function\":\"debugged.worker\",\"pc\":[0-9]*,\"file\":\"[^\"]*\",\"line\":$(line_of 'mov R1->Seen')}\]" "$MOND/st" || fail "debug: out of the call ($(cat "$MOND/st"))"
    N=3
    for L in 'lpush R2' 'len I6' 'blt I6' ; do
        [ "$(code -H "$A" --data '{"kind":"over"}' $D/$W/step)" = 200 ] || fail "debug: step over"
        N=$((N + 1)); paused $W $N
        grep -q "\"frames\":\[{\"function\":\"debugged.worker\",\"pc\":[0-9]*,\"file\":\"[^\"]*\",\"line\":$(line_of "$L")}\]" "$MOND/st" || fail "debug: over a line, to '$L' ($(cat "$MOND/st"))"
    done
    i=0 # on, over the sleep and round to the call, then into it by an instruction
    until grep -q "\"line\":$(line_of 'call bump')}\]" "$MOND/st"; do
        i=$((i + 1)); [ $i -lt 8 ] || fail "debug: never back at the call ($(cat "$MOND/st"))"
        [ "$(code -H "$A" --data '{"kind":"over","by":"line"}' $D/$W/step)" = 200 ] || fail "debug: step over"
        N=$((N + 1)); paused $W $N
    done
    [ "$(code -H "$A" --data '{"kind":"into","by":"insn"}' $D/$W/step)" = 200 ] || fail "debug: step into"
    N=$((N + 1)); paused $W $N
    grep -q '"frames":\[{"function":"debugged.bump","pc":0,' "$MOND/st" || fail "debug: into the call ($(cat "$MOND/st"))"
    [ "$(code -H "$A" --data '{"kind":"sideways"}' $D/$W/step)" = 400 ] || fail "debug: a step of no kind"
    # breakpoints at lines: one with code, one without (the next that has), one in no file loaded
    [ "$(break_at debugged.masm $L_SLEEP)" = 200 ] && grep -q "\"line_at\":$(line_of 'sleep 5'),\"function\":null,\"at\":\[{\"function\":\"debugged.worker\"" "$MOND/r" || fail "debug: a breakpoint at a line ($(cat "$MOND/r"))"
    [ "$(break_at debugged.masm $L_NOTE)" = 200 ] && grep -q "\"line_at\":$(line_of 'bnz.r r, 1f'),\"function\":null,\"at\":\[{\"function\":\"debugged.show\"" "$MOND/r" || fail "debug: a breakpoint at a line without code ($(cat "$MOND/r"))"
    [ "$(code -H "$A" --data '{"file":"later.masm","line":3}' $D/breakpoints)" = 200 ] && grep -q '"file":"later.masm","line":3,"line_at":0,"function":null,"at":\[\]}$' "$MOND/r" || fail "debug: a breakpoint for code not loaded ($(cat "$MOND/r"))"
    [ "$(code -H "$A" --data '{"line":3}' $D/breakpoints)" = 400 ] || fail "debug: a breakpoint nowhere"
    [ "$(get -H "$A" $D/breakpoints)" = 200 ] && [ "$(grep -o '"id":' "$MOND/r" | wc -l | tr -d ' ')" = 3 ] || fail "debug: the breakpoints ($(cat "$MOND/r"))"
    for i in 1 2; do # at the sleep once a pass: it is one instruction, though the process waits in it
        [ "$(code -H "$A" $D/$W/continue)" = 200 ] || fail "debug: continue"
        N=$((N + 1)); paused $W $N
        grep -q "\"reason\":\"breakpoint\".*\"line\":$(line_of 'sleep 5')}\]" "$MOND/st" || fail "debug: at the line's breakpoint ($(cat "$MOND/st"))"
        C=$(curl -s -H "$A" "$D/$W/vars" | sed 's/.*"name":"count","value":"\([0-9]*\)".*/\1/')
        [ $i = 1 ] && C1=$C
    done
    [ "$C" = $((C1 + 1)) ] || fail "debug: a pass between two stops at a line ($C1, then $C)"
    # a `break` instruction, and a breakpoint on a recv: once for each message
    [ "$(code -H "$A" $D/$B/attach)" = 200 ] && grep -q '"debugged":true,"state":"waiting"' "$MOND/r" || fail "debug: attach to one that waits ($(cat "$MOND/r"))"
    [ "$(code -H "$A" --data '"go"' $U/api/proc/$B/send)" = 200 ] || fail "debug: a message"
    paused $B 1
    grep -q "\"reason\":\"break\"" "$MOND/st" || fail "debug: at a break instruction ($(cat "$MOND/st"))"
    [ "$(break_at debugged.masm $L_RECV)" = 200 ] || fail "debug: a breakpoint at a recv"
    [ "$(code -H "$A" $D/$B/continue)" = 200 ] || fail "debug: continue after a break"
    paused $B 2
    grep -q "\"reason\":\"breakpoint\".*\"line\":$(line_of 'recv R0')}\]" "$MOND/st" || fail "debug: at the recv ($(cat "$MOND/st"))"
    curl -sf -H "$A" "$U/api/log" | grep -q '"text":"after the break"' || fail "debug: on, after the break"
    [ "$(code -H "$A" $D/$B/continue)" = 200 ] || fail "debug: continue into the recv"
    sleep 0.3
    curl -s -H "$A" $D/$B | grep -q '"state":"waiting".*"seq":2,' || fail "debug: waiting in the recv ($(curl -s -H "$A" $D/$B))"
    [ "$(code -H "$A" --data '"again"' $U/api/proc/$B/send)" = 200 ] || fail "debug: another message"
    paused $B 3
    grep -q "\"reason\":\"break\"" "$MOND/st" || fail "debug: the recv done without stopping there again ($(cat "$MOND/st"))"
    # pause one that runs; detach: both go on; killed while paused
    [ "$(code -H "$A" $D/$B/detach)" = 200 ] && grep -q '"debugged":false' "$MOND/r" || fail "debug: detach ($(cat "$MOND/r"))"
    [ "$(code -H "$A" $D/$B/continue)" = 409 ] || fail "debug: continue one not debugged"
    for i in 2 3 4; do [ "$(code -H "$A" $D/breakpoints/$i/delete)" = 200 ] || fail "debug: delete breakpoint $i"; done
    [ "$(code -H "$A" $D/breakpoints/5/delete)" = 200 ] || fail "debug: delete the recv's breakpoint"
    [ "$(code -H "$A" $D/$W/continue)" = 200 ] || fail "debug: continue with no breakpoints"
    sleep 0.3
    curl -s -H "$A" $D/$W | grep -q "\"debugged\":true,\"state\":\"\(sleeping\|ready\|running\)\".*\"seq\":$N," || fail "debug: running on ($(curl -s -H "$A" $D/$W))"
    [ "$(code -H "$A" $D/$W/pause)" = 200 ] || fail "debug: pause"
    N=$((N + 1)); paused $W $N
    grep -q '"reason":"pause"' "$MOND/st" || fail "debug: paused when asked ($(cat "$MOND/st"))"
    [ "$(code -H "$A" $U/api/proc/$W/kill)" = 200 ] || fail "debug: kill one paused"
    sleep 0.2
    [ "$(get -H "$A" $D/$W)" = 404 ] || fail "debug: a killed process still there"
    curl -sf -H "$A" $U/api/exits | grep -q "{\"pid\":$W,\"entry\":\"worker\",[^}]*\"failed\":true,\"code\":\"E_KILLED\"" || fail "debug: the killed process's exit"
    LOG=$(curl -sf -H "$A" "$U/api/log")
    echo "$LOG" | grep -q "\"event\":\"monitor.debug_attach\",\"value\":{\"by\":\"(start)\",\"pid\":$W}" || fail "debug: attach in the log"
    echo "$LOG" | grep -q "\"event\":\"monitor.debug_detach\",\"value\":{\"by\":\"(start)\",\"pid\":$B}" || fail "debug: detach in the log"
    echo "$LOG" | grep -q '"event":"monitor.debug_break","value":{"by":"(start)","function":"bump"}' || fail "debug: a breakpoint in the log"
    kill $VMPID 2>/dev/null || true
    wait $VMPID 2>/dev/null || true
    # With one paused and every other waiting, the VM goes on (no deadlock);
    # and the program ends, with one still paused, as any other.
    MLWSVM_MONITOR_TOKEN=s3cret $BIN run --monitor 127.0.0.1:$PORT --monitor-actions tests/tools/_monitor/waits.masm >/dev/null 2>"$MOND/waits.err" &
    VMPID=$!
    i=0
    until curl -sf -H "$A" $U/api/names 2>/dev/null | grep -q '"waiter"'; do
        i=$((i + 1)); [ $i -lt 50 ] || fail "debug: waits.masm did not start"; sleep 0.1
    done
    W=$(pid_of waiter)
    [ "$(code -H "$A" --data '{"pause":true}' $D/$W/attach)" = 200 ] && grep -q '"state":"waiting","away":false,"pausing":true' "$MOND/r" || fail "debug: pausing one that waits ($(cat "$MOND/r"))"
    [ "$(code -H "$A" --data '1' $U/api/proc/$W/send)" = 200 ] || fail "debug: a message to it"
    paused $W 1
    curl -sf -H "$A" $U/api/proc/$W | grep -q '"messages":\[{"type":"i64","preview":1}\]' || fail "debug: paused before the message was taken"
    i=0
    until curl -sf -H "$A" "$U/api/log" | grep -q '"text":"main waits"'; do
        i=$((i + 1)); [ $i -lt 50 ] || fail "debug: main never waited"; sleep 0.1
    done
    sleep 1
    curl -sf -H "$A" $U/api/procs | grep -q '"state":"waiting".*"state":"paused"' || fail "debug: the VM with one paused and the rest waiting ($(cat "$MOND/waits.err"))"
    [ "$(code -H "$A" --data '"end"' $U/api/proc/1/send)" = 200 ] || fail "debug: a message to main"
    set +e
    wait $VMPID; RC=$?
    set -e
    [ $RC = 7 ] || fail "debug: the program's end with a process paused (exit $RC): $(cat "$MOND/waits.err")"
    # A process running as machine code: attached to, it is interpreted from
    # where it was, its registers as the machine code left them; detached
    # from, it runs on.
    MLWSVM_MONITOR_TOKEN=s3cret $BIN run --monitor 127.0.0.1:$PORT --monitor-actions tests/tools/_monitor/hot.masm >/dev/null 2>"$MOND/hot.err" &
    VMPID=$!
    i=0
    until curl -sf -H "$A" $U/api/names 2>/dev/null | grep -q '"spinner"'; do
        i=$((i + 1)); [ $i -lt 50 ] || fail "debug: hot.masm did not start"; sleep 0.1
    done
    sleep 0.5
    W=$(pid_of spinner)
    count() { curl -s -H "$A" "$D/$W/vars?frame=$1" | sed 's/.*"name":"count","value":"\([0-9]*\)".*/\1/'; }
    [ "$(code -H "$A" --data '{"pause":true}' $D/$W/attach)" = 200 ] || fail "debug: attach to machine code"
    paused $W 1
    FR=$(grep -o '"function":"hot.inc"' "$MOND/st" | wc -l | tr -d ' ') # (in inc: the caller is the next frame)
    C1=$(count $FR); I6=$(curl -s -H "$A" "$D/$W/vars?frame=$FR" | sed 's/.*"name":"I6","value":"\([0-9]*\)".*/\1/')
    [ "$C1" -gt 1000 ] 2>/dev/null && [ $((I6 / 2)) -ge $((C1 - 1)) ] && [ $((I6 / 2)) -le $((C1 + 1)) ] || fail "debug: machine code's registers (count $C1, I6 $I6)"
    for i in 2 3 4 5 6 7; do
        [ "$(code -H "$A" --data '{"kind":"into","by":"insn"}' $D/$W/step)" = 200 ] || fail "debug: a step from machine code"
        paused $W $i
    done
    FR=$(grep -o '"function":"hot.inc"' "$MOND/st" | wc -l | tr -d ' ')
    C2=$(count $FR)
    [ "$C2" -ge $((C1 + 1)) ] && [ "$C2" -le $((C1 + 3)) ] || fail "debug: six instructions on ($C1, then $C2)"
    [ "$(code -H "$A" $D/$W/detach)" = 200 ] || fail "debug: detach from it"
    I0=$(curl -sf -H "$A" $U/api/proc/$W | sed 's/.*"insns":\([0-9]*\),"cpu_ns".*/\1/'); sleep 0.3
    I1=$(curl -sf -H "$A" $U/api/proc/$W | sed 's/.*"insns":\([0-9]*\),"cpu_ns".*/\1/')
    [ $((I1 - I0)) -gt 100000 ] || fail "debug: running on after a detach ($I0 to $I1 instructions)"
    kill $VMPID 2>/dev/null || true
    wait $VMPID 2>/dev/null || true

    # What a debugger follows, and where it stops for an error. Following
    # what a process spawns, its children are debugged too. One that fails
    # with nothing to catch its error is paused where it failed, its frames
    # and variables still there; it cannot step, and let go on it ends with
    # the same report. One that catches its error is not stopped. A program's
    # processes can all be paused, and all let go on; one asked to pause while
    # it waits can be let off.
    DF=tests/tools/_monitor/family.masm
    MLWSVM_MONITOR_TOKEN=s3cret $BIN run --monitor 127.0.0.1:$PORT --monitor-actions $DF >/dev/null 2>"$MOND/family.err" &
    VMPID=$!
    i=0
    until curl -sf -H "$A" $U/api/names 2>/dev/null | grep -q '"parent"'; do
        i=$((i + 1)); [ $i -lt 50 ] || fail "family: the monitor did not start"; sleep 0.1
    done
    W=$(pid_of parent)
    [ "$(code -H "$A" --data '{"children":true}' $D/$W/attach)" = 200 ] && grep -q '"debugged":true,"state":"waiting".*"children":true' "$MOND/r" || fail "family: attach, following what it spawns ($(cat "$MOND/r"))"
    [ "$(code -H "$A" --data '2' $U/api/proc/$W/send)" = 200 ] || fail "family: a message"
    i=0
    until curl -sf -H "$A" "$U/api/log" | grep -q '"text":"careful: caught"'; do
        i=$((i + 1)); [ $i -lt 50 ] || fail "family: an error caught ($(curl -sf -H "$A" $U/api/log))"; sleep 0.1
    done
    curl -s -H "$A" $D | grep -q '"state":"paused"' && fail "family: stopped for an error that was caught ($(curl -s -H "$A" $D))"
    [ "$(code -H "$A" --data '1' $U/api/proc/$W/send)" = 200 ] || fail "family: a message"
    i=0
    until curl -s -H "$A" $D | grep -q '"state":"paused".*"entry":"crasher"'; do
        i=$((i + 1)); [ $i -lt 50 ] || fail "family: a child not debugged ($(curl -s -H "$A" $D))"; sleep 0.1
    done
    C=$(curl -s -H "$A" $D | grep -o '{"pid":[0-9]*,"debugged":true,"state":"paused"' | head -1 | sed 's/{"pid":\([0-9]*\).*/\1/')
    paused $C 1
    grep -q "\"reason\":\"error\",\"error\":{\"code\":\"E_DIV_ZERO\",\"message\":\"integer division by zero\"},\"seq\":1,\"children\":true,\"entry\":\"crasher\"" "$MOND/st" || fail "family: paused where it failed ($(cat "$MOND/st"))"
    grep -q "\"frames\":\[{\"function\":\"family.crasher\",\"pc\":1,\"file\":\"[^\"]*\",\"line\":$(line_of 'div I2, I1, d')}\]" "$MOND/st" || fail "family: at the instruction that failed ($(cat "$MOND/st"))"
    curl -s -H "$A" "$D/$C/vars" | grep -q '^{"locals":\[{"name":"d","value":"0","kind":"i64"' || fail "family: its variables where it failed"
    [ "$(code -H "$A" --data '{"kind":"over"}' $D/$C/step)" = 409 ] && grep -q 'has failed' "$MOND/r" || fail "family: a step of one that has failed"
    [ "$(code -H "$A" $D/$C/continue)" = 200 ] || fail "family: let go on to its end"
    i=0
    until curl -sf -H "$A" $U/api/exits | grep -q "{\"pid\":$C,\"entry\":\"crasher\",[^}]*\"failed\":true,\"code\":\"E_DIV_ZERO\""; do
        i=$((i + 1)); [ $i -lt 50 ] || fail "family: its end ($(curl -sf -H "$A" $U/api/exits))"; sleep 0.1
    done
    grep -q "^process $C (crasher) failed: \[E_DIV_ZERO\] integer division by zero" "$MOND/family.err" && grep -q "at family.crasher ([^)]*family.masm:$(line_of 'div I2, I1, d'))" "$MOND/family.err" || fail "family: its report ($(cat "$MOND/family.err"))"
    # all of its program paused; one asked to pause while it waits let off; all let go on
    [ "$(code -H "$A" --data '{"program":true}' $D/$W/pause)" = 200 ] || fail "family: pause its program"
    [ "$(curl -s -H "$A" $D | grep -o '"debugged":true,"state":"[a-z]*","away":false,"pausing":true' | wc -l | tr -d ' ')" = 2 ] || fail "family: its program's processes asked to pause ($(curl -s -H "$A" $D))"
    [ "$(code -H "$A" $D/1/continue)" = 200 ] && grep -q '"pausing":false' "$MOND/r" || fail "family: one asked to pause, let off ($(cat "$MOND/r"))"
    [ "$(code -H "$A" $D/1/continue)" = 409 ] || fail "family: let off twice"
    [ "$(code -H "$A" --data '3' $U/api/proc/$W/send)" = 200 ] || fail "family: a message"
    paused $W 1
    [ "$(code -H "$A" --data '{"program":true}' $D/$W/continue)" = 200 ] || fail "family: its program let go on"
    i=0
    until curl -sf -H "$A" "$U/api/log" | grep -q '"text":"crasher: done"'; do
        i=$((i + 1)); [ $i -lt 50 ] || fail "family: on, after its program was let go on"; sleep 0.1
    done
    # detached from, what it spawns is no longer followed: one that fails ends at once
    [ "$(code -H "$A" $D/$W/detach)" = 200 ] && grep -q '"debugged":false.*"children":false' "$MOND/r" || fail "family: detach ($(cat "$MOND/r"))"
    [ "$(code -H "$A" --data '1' $U/api/proc/$W/send)" = 200 ] || fail "family: a message"
    i=0
    until [ "$(curl -sf -H "$A" $U/api/exits | grep -o '"code":"E_DIV_ZERO"' | wc -l | tr -d ' ')" = 2 ]; do
        i=$((i + 1)); [ $i -lt 50 ] || fail "family: one not debugged, failing ($(curl -sf -H "$A" $U/api/exits))"; sleep 0.1
    done
    curl -s -H "$A" $D | grep -q '"state":"paused"' && fail "family: one not debugged, stopped for its error"
    curl -sf -H "$A" "$U/api/log" | grep -q '"event":"monitor.debug_pause","value":{"by":"(start)","program":0}' || fail "family: a program's pause in the log"
    kill $VMPID 2>/dev/null || true
    wait $VMPID 2>/dev/null || true

    # Environments in the monitor: each one's names to a read key, values to
    # an admin's (never a secret's); an admin sets and deletes one, which its
    # watcher hears of, each in the log by name and never by value.
    MLWSVM_MONITOR_TOKEN=s3cret $BIN run --monitor 127.0.0.1:$PORT --monitor-actions --monitor-tokens "$KF" \
        --env tests/tools/_monitor/envs.env tests/tools/_monitor/envs.masm >/dev/null 2>"$MOND/envs.err" &
    VMPID=$!
    RK=$($BIN token new --label viewer --file "$KF" 2>/dev/null)
    R="Authorization: Bearer $RK"
    i=0
    until curl -sf -H "$A" $U/api/names 2>/dev/null | grep -q '"watcher"'; do
        i=$((i + 1)); [ $i -lt 50 ] || fail "envs: the monitor did not start"; sleep 0.1
    done
    [ "$(get $U/api/envs)" = 401 ] || fail "envs: without a key"
    [ "$(get -H "$R" $U/api/envs)" = 200 ] || fail "envs: a read key"
    grep -q '^\[{"env":null,"program":0,"program_name":"envs.masm","processes":1,"watchers":0,"values":\[{"key":"db_password","secret":true},{"key":"port","secret":false},{"key":"servers","secret":false}\]},{"env":"billing","program":null,"program_name":null,"processes":1,"watchers":1,"values":\[{"key":"odd","secret":false},{"key":"rate","secret":false}\]}\]$' "$MOND/r" \
        || fail "envs: to a read key ($(cat "$MOND/r"))"
    [ "$(get -H "$A" $U/api/envs)" = 200 ] || fail "envs: an admin's key"
    grep -q '{"key":"db_password","secret":true,"value":null},{"key":"port","secret":false,"value":8080},{"key":"servers","secret":false,"value":\["a","b"\]}' "$MOND/r" \
        && grep -q '"values":\[{"key":"odd","secret":false,"value":null,"note":"json.stringify: map keys must be strings or integers"},{"key":"rate","secret":false,"value":3}\]' "$MOND/r" \
        || fail "envs: to an admin ($(cat "$MOND/r"))"
    grep -q hunter2 "$MOND/r" && fail "envs: a secret shown"
    curl -sf -H "$R" $U/api/proc/2 | grep -q '"entry":"watcher","program":0,"program_name":"envs.masm","env":"billing"' || fail "envs: a process's environment"
    [ "$(code -H "$R" --data '{"env":"billing","key":"rate","value":4}' $U/api/envs/set)" = 403 ] || fail "envs: a set with a read key"
    [ "$(code -H "$A" --data '{"env":"billing","key":"rate","value":4.5}' $U/api/envs/set)" = 200 ] || fail "envs: set ($(cat "$MOND/r"))"
    [ "$(code -H "$A" --data '{"program":0,"key":"db_password","value":"swordfish"}' $U/api/envs/set)" = 200 ] || fail "envs: set a secret"
    [ "$(code -H "$A" --data '{"program":0,"key":"api_token","value":"t0k","secret":true}' $U/api/envs/set)" = 200 ] || fail "envs: set one secret"
    [ "$(code -H "$A" --data '{"env":"nobody","key":"x","value":1}' $U/api/envs/set)" = 404 ] || fail "envs: no such environment"
    [ "$(code -H "$A" --data '{"env":"billing","key":"bad key","value":1}' $U/api/envs/set)" = 400 ] || fail "envs: a bad name"
    [ "$(code -H "$A" --data '{"env":"billing","key":"x"}' $U/api/envs/set)" = 400 ] || fail "envs: a set with no value"
    [ "$(code -H "$A" --data '{"env":"billing","key":"x"' $U/api/envs/set)" = 400 ] || fail "envs: not JSON"
    [ "$(code -H "$A" --data '{"env":"billing","key":"nope"}' $U/api/envs/delete)" = 404 ] || fail "envs: delete no such value"
    i=0
    until curl -sf -H "$A" "$U/api/log" | grep -q '"text":"changed rate \[4.5\]"'; do
        i=$((i + 1)); [ $i -lt 50 ] || fail "envs: the watcher did not hear ($(curl -sf -H "$A" $U/api/log))"; sleep 0.1
    done
    [ "$(code -H "$A" --data '{"env":"billing","key":"rate"}' $U/api/envs/delete)" = 200 ] || fail "envs: delete"
    i=0
    until curl -sf -H "$A" "$U/api/log" | grep -q '"text":"changed rate \[\]"'; do
        i=$((i + 1)); [ $i -lt 50 ] || fail "envs: the watcher did not hear of the delete"; sleep 0.1
    done
    curl -sf -H "$A" $U/api/envs | grep -q '{"key":"api_token","secret":true,"value":null},{"key":"db_password","secret":true,"value":null}' || fail "envs: secrets after a set"
    LOG=$(curl -sf -H "$A" "$U/api/log")
    echo "$LOG" | grep -q '"event":"monitor.env_set","value":{"by":"(start)","env":"billing","program":null,"key":"rate"}' || fail "envs: env_set in the log ($LOG)"
    echo "$LOG" | grep -q '"event":"monitor.env_delete","value":{"by":"(start)","env":"billing","program":null,"key":"rate"}' || fail "envs: env_delete in the log"
    echo "$LOG" | grep -q 'swordfish\|t0k' && fail "envs: a value in the log"
    kill $VMPID 2>/dev/null || true
    wait $VMPID 2>/dev/null || true

    # mlwsvm serve: programs started, listed, stopped, kept within their
    # capabilities, only from their directory; then shut down.
    set +e
    $BIN serve --monitor 127.0.0.1:$PORT >/dev/null 2>&1; RC=$?
    set -e
    [ $RC = 2 ] || fail "serve without --programs (exit $RC)"
    MLWSVM_MONITOR_TOKEN=s3cret $BIN serve --programs tests/tools/_serve --monitor 127.0.0.1:$PORT >/dev/null 2>"$MOND/serve.err" &
    VMPID=$!
    i=0
    until curl -sf $U/api/vm 2>/dev/null | grep -q '"actions":true,"serve":true'; do
        i=$((i + 1)); [ $i -lt 50 ] || fail "serve did not start"; sleep 0.1
    done
    grep -q '^mlwsvm: serving at http://127.0.0.1:[0-9]*/#token=s3cret ' "$MOND/serve.err" || fail "serve: the address printed"
    [ "$(code -H "$A" --data '{"path":"hello.masm","args":["a","b"]}' $U/api/programs)" = 200 ] || fail "serve: start ($(cat "$MOND/r"))"
    [ "$(code -H "$A" --data '{"path":"hello.masm","args":["c"]}' $U/api/programs)" = 200 ] || fail "serve: start the same again"
    [ "$(code -H "$A" --data '{"path":"long.masm"}' $U/api/programs)" = 200 ] || fail "serve: start long"
    [ "$(code -H "$A" --data '{"path":"reader.masm","cap":"none"}' $U/api/programs)" = 200 ] || fail "serve: start reader"
    [ "$(code -H "$A" --data '{"path":"../_monitor/acted.masm"}' $U/api/programs)" = 403 ] || fail "serve: a program outside its directory"
    [ "$(code -H "$A" --data '{"path":"../_serve_too/beside.masm"}' $U/api/programs)" = 403 ] || fail "serve: a program in a directory named like its own"
    [ "$(code -H "$A" --data '{"path":"nope.masm"}' $U/api/programs)" = 403 ] || fail "serve: no such program"
    [ "$(code -H "$A" --data '{"pth":"hello.masm"}' $U/api/programs)" = 400 ] || fail "serve: not a start"
    [ "$(code -H "$A" --data '{"path":"hello.masm","quota":"bogus=1"}' $U/api/programs)" = 400 ] || fail "serve: a bad quota"
    [ "$(code --data '{"path":"hello.masm"}' $U/api/programs)" = 401 ] || fail "serve: a start without the token"
    i=0
    until curl -sf $U/api/programs | grep -q '"id":4,"path":"reader.masm","cap":"none","quota":null,"args":\[\],"pid":[0-9]*,"state":"failed","code":"E_PERMISSION"'; do
        i=$((i + 1)); [ $i -lt 50 ] || fail "serve: the programs ($(curl -sf $U/api/programs))"; sleep 0.1
    done
    P=$(curl -sf $U/api/programs)
    echo "$P" | grep -q '"id":1,"path":"hello.masm","cap":null,"quota":null,"args":\["a","b"\],"pid":[0-9]*,"state":"done","exit":2' || fail "serve: hello ($P)"
    echo "$P" | grep -q '"id":2,"path":"hello.masm","cap":null,"quota":null,"args":\["c"\],"pid":[0-9]*,"state":"done","exit":1' || fail "serve: hello again ($P)"
    echo "$P" | grep -q '"id":3,"path":"long.masm","cap":null,"quota":null,"args":\[\],"pid":[0-9]*,"state":"running"' || fail "serve: long ($P)"
    # A program's main is freed when it ends (its result kept): gone from the
    # processes, its heap with it.
    curl -sf $U/api/procs | grep -q '"program_name":"hello.masm"' && fail "serve: an ended program's main still a process ($(curl -sf $U/api/procs))"
    [ "$(get $U/api/proc/$(echo "$P" | sed 's/.*"id":1,[^}]*"pid":\([0-9]*\).*/\1/'))" = 404 ] || fail "serve: an ended program's main still found"
    curl -sf $U/api/procs | grep -q '"entry":"main","program":3,"program_name":"long.masm"' || fail "serve: a process's program"
    [ "$(code -H "$A" $U/api/programs/3/stop)" = 200 ] || fail "serve: stop"
    curl -sf $U/api/programs | grep -q '"id":3,"path":"long.masm","cap":null,"quota":null,"args":\[\],"pid":[0-9]*,"state":"failed","code":"E_KILLED"' || fail "serve: stopped"
    [ "$(code -H "$A" $U/api/programs/3/stop)" = 404 ] || fail "serve: stop a program that has ended"
    curl -sf "$U/api/log" | grep -q '"pid":[0-9]*,"program":3,"program_name":"long.masm","level":"info","text":"long started"' || fail "serve: a program's log, with its program"
    F=$(curl -sf $U/api/programs/files)
    for f in hello.masm long.masm reader.masm; do echo "$F" | grep -q "\"$f\"" || fail "serve: $f not listed ($F)"; done
    curl -sf $U/api/exits | grep -q '"entry":"main","program":1,"program_name":"hello.masm","main":true' || fail "serve: an exit's program"
    [ "$(code -H "$A" $U/api/programs/1/rerun)" = 200 ] || fail "serve: rerun ($(cat "$MOND/r"))"
    grep -q '"id":5' "$MOND/r" || fail "serve: rerun's id ($(cat "$MOND/r"))"
    [ "$(code -H "$A" $U/api/programs/4/rerun)" = 200 ] || fail "serve: rerun of one with no capabilities"
    [ "$(code -H "$A" $U/api/programs/99/rerun)" = 404 ] || fail "serve: rerun of no such program"
    i=0
    until curl -sf $U/api/programs | grep -q '"id":6,"path":"reader.masm","cap":"none","quota":null,"args":\[\],"pid":[0-9]*,"state":"failed","code":"E_PERMISSION"'; do
        i=$((i + 1)); [ $i -lt 50 ] || fail "serve: rerun, the same again ($(curl -sf $U/api/programs))"; sleep 0.1
    done
    curl -sf $U/api/programs | grep -q '"id":5,"path":"hello.masm","cap":null,"quota":null,"args":\["a","b"\],"pid":[0-9]*,"state":"done","exit":2' || fail "serve: rerun with its arguments"
    # A program's environment, from the NAME.mlws.env beside it.
    [ "$(code -H "$A" --data '{"path":"envy.masm"}' $U/api/programs)" = 200 ] || fail "serve: start envy"
    i=0
    until curl -sf "$U/api/log" | grep -q 'envy: from its file'; do
        i=$((i + 1)); [ $i -lt 50 ] || fail "serve: a program's environment ($(curl -sf $U/api/log))"; sleep 0.1
    done
    # A program started paused, in its debugger: its main stops before its
    # first instruction; it steps by the lines of the file it says it was made
    # from, which is read from beside it; let go on, it ends as it would have.
    # A program run again likewise. mlwsvm serve's own process is not debugged.
    [ "$(get $U/debug)" = 200 ] && grep -q '<title>MLWSVM Debugger</title>' "$MOND/r" || fail "serve: the debugger's page"
    [ "$(code -H "$A" --data '{"path":"located.masm","debug":true}' $U/api/programs)" = 200 ] && grep -q '^{"ok":true,"id":8,"pid":[0-9]*,"debug":true}$' "$MOND/r" || fail "serve: start paused ($(cat "$MOND/r"))"
    LP=$(sed 's/.*"pid":\([0-9]*\).*/\1/' "$MOND/r")
    D=$U/api/debug
    ST=$(curl -s -H "$A" $D/$LP)
    echo "$ST" | grep -q '"debugged":true,"state":"paused","away":false,"pausing":false,"stepping":false,"reason":"entry","error":null,"seq":1,"children":true,"entry":"main"' || fail "serve: paused at its entry, what it spawns to be debugged too ($ST)"
    echo "$ST" | grep -q '"frames":\[{"function":"located.main","pc":0,"file":"[^"]*located.masm","line":8,"source":"located.src","source_line":1}\]' || fail "serve: where it is paused ($ST)"
    [ "$(get -H "$A" "$D/source?file=located.src")" = 200 ] && grep -Eq '^\{"file":"located.src","text":"let n = 1 \+ 1(\\u000d)?\\u000an = n \+ 2(\\u000d)?\\u000areturn n(\\u000d)?\\u000a"\}$' "$MOND/r" || fail "serve: the source beside the program ($(cat "$MOND/r"))" # (lines end as the checkout made them)
    [ "$(code -H "$A" --data '{"kind":"over"}' $D/$LP/step)" = 200 ] || fail "serve: a step by a line of its source"
    sleep 0.2
    curl -s -H "$A" $D/$LP | grep -q '"reason":"step","error":null,"seq":2,.*"pc":2,"file":"[^"]*","line":11,"source":"located.src","source_line":2}\]' || fail "serve: a line on, two instructions ($(curl -s -H "$A" $D/$LP))"
    [ "$(code -H "$A" --data '{"file":"located.src","line":3}' $D/breakpoints)" = 200 ] && grep -q '"line_at":3,"function":null,"at":\[{"function":"located.main","pc":3,' "$MOND/r" || fail "serve: a breakpoint at a line of its source ($(cat "$MOND/r"))"
    [ "$(code -H "$A" $D/$LP/continue)" = 200 ] || fail "serve: continue"
    sleep 0.2
    curl -s -H "$A" $D/$LP | grep -q '"reason":"breakpoint","error":null,"seq":3,' || fail "serve: at its source's breakpoint ($(curl -s -H "$A" $D/$LP))"
    [ "$(code -H "$A" $D/$LP/continue)" = 200 ] || fail "serve: continue to its end"
    i=0
    until curl -sf $U/api/programs | grep -q '"id":8,"path":"located.masm",[^}]*"state":"done","exit":4'; do
        i=$((i + 1)); [ $i -lt 50 ] || fail "serve: a debugged program's end ($(curl -sf $U/api/programs))"; sleep 0.1
    done
    [ "$(code -H "$A" --data '{"debug":true}' $U/api/programs/8/rerun)" = 200 ] && grep -q '"id":9,"pid":[0-9]*,"debug":true}$' "$MOND/r" || fail "serve: run again, paused ($(cat "$MOND/r"))"
    LP=$(sed 's/.*"pid":\([0-9]*\).*/\1/' "$MOND/r")
    curl -s -H "$A" $D/$LP | grep -q '"state":"paused".*"reason":"entry"' || fail "serve: run again, at its entry"
    [ "$(code -H "$A" $U/api/programs/9/stop)" = 200 ] || fail "serve: stop one paused"
    curl -sf $U/api/programs | grep -q '"id":9,"path":"located.masm",[^}]*"state":"failed","code":"E_KILLED"' || fail "serve: one stopped while paused"
    [ "$(code -H "$A" $D/1/attach)" = 403 ] && grep -q "serve's own" "$MOND/r" || fail "serve: its own process debugged ($(cat "$MOND/r"))"
    curl -sf "$U/api/log" | grep -q '"event":"monitor.start","value":{"by":"(start)","path":"located.masm","pid":[0-9]*,"debug":true}' || fail "serve: a paused start in the log"
    [ "$(code -H "$A" $U/api/shutdown)" = 200 ] || fail "serve: shutdown"
    set +e
    wait $VMPID; RC=$?
    set -e
    [ $RC = 0 ] || fail "serve: exit after shutdown ($RC)"

    # examples/serve/echo.masm, as its comment says to run it: two messages
    # sent, each in the log, then goodbye.
    MLWSVM_MONITOR_TOKEN=s3cret $BIN serve --programs examples/serve --monitor 127.0.0.1:$PORT >/dev/null 2>"$MOND/echo.err" &
    VMPID=$!
    i=0
    until curl -sf $U/api/vm 2>/dev/null | grep -q '"serve":true'; do
        i=$((i + 1)); [ $i -lt 50 ] || fail "echo: serve did not start"; sleep 0.1
    done
    [ "$(code -H "$A" --data '{"path":"echo.masm","args":["2"]}' $U/api/programs)" = 200 ] || fail "echo: start ($(cat "$MOND/r"))"
    i=0
    until curl -sf "$U/api/log" | grep -q 'hello: echo is listening for 2 messages'; do
        i=$((i + 1)); [ $i -lt 50 ] || fail "echo: hello ($(curl -sf $U/api/log))"; sleep 0.1
    done
    EP=$(curl -sf $U/api/programs | sed 's/.*"path":"echo.masm"[^}]*"pid":\([0-9]*\).*/\1/')
    [ "$(code -H "$A" --data '7' $U/api/proc/$EP/send)" = 200 ] || fail "echo: send ($(cat "$MOND/r"))"
    [ "$(code -H "$A" --data '{"n":[1,"x"]}' $U/api/proc/$EP/send)" = 200 ] || fail "echo: send a map"
    i=0
    until curl -sf "$U/api/log" | grep -q 'echo: that was the last one; goodbye'; do
        i=$((i + 1)); [ $i -lt 50 ] || fail "echo: goodbye ($(curl -sf $U/api/log))"; sleep 0.1
    done
    L=$(curl -sf "$U/api/log")
    echo "$L" | grep -q '"text":"echo 1/2: 7"' || fail "echo: the first message ($L)"
    echo "$L" | grep -q '"text":"echo 2/2: {\\"n\\":\[1,\\"x\\"\]}"' || fail "echo: the second message ($L)"
    curl -sf $U/api/programs | grep -q '"path":"echo.masm","cap":null,"quota":null,"args":\["2"\],"pid":[0-9]*,"state":"done","exit":0' || fail "echo: done ($(curl -sf $U/api/programs))"
    [ "$(code -H "$A" $U/api/shutdown)" = 200 ] || fail "echo: shutdown"
    wait $VMPID || fail "echo: serve's exit"
    # A VM run in the background here wrote its errors to a file of its own: a
    # sanitizer's report in one would otherwise go unseen.
    if grep -l "Sanitizer" "$MOND"/*.err >/dev/null 2>&1; then
        fail "a sanitizer's report from a VM in the background: $(grep -h -A14 "Sanitizer" "$MOND"/*.err | head -45)"
    fi
    rm -rf "${MOND:?}"
fi
echo "check-tools: profile, .loc, bundle, dap, node, names, cache, lib, --also, opt, fuzz, trace, jit, monitor, top OK"
