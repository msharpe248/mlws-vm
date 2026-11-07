#!/bin/sh
# Every check a release needs, on this machine (docs/TOOLING.md, "Before a
# release"): a clean build without warnings, every `make check-*`, the fuzzer
# (plainly and under AddressSanitizer), and, if the compilers' repository is
# found, their suites in every mode, with this build and the ASan one. Run it
# on each platform a release supports before tagging.
#
# Usage: sh tests/release-check.sh [-c CC] [-C COMPILERS_DIR] [-f FUZZ_SECONDS]
#   CC              the compiler (default: cc)
#   COMPILERS_DIR   mlws-compilers (default: ../mlws-compilers, if there)
#   FUZZ_SECONDS    how long to fuzz (default 300; the ASan run is a tenth)
# Each step's output is in build/release-check/STEP.log. It goes on after a
# failure, and exits with 1 if any step failed.
set -u
CC=cc
COMPILERS=../mlws-compilers
FUZZ=300
while getopts c:C:f: opt; do
    case $opt in
    c) CC=$OPTARG ;;
    C) COMPILERS=$OPTARG ;;
    f) FUZZ=$OPTARG ;;
    *) echo "usage: sh tests/release-check.sh [-c CC] [-C COMPILERS_DIR] [-f FUZZ_SECONDS]" >&2; exit 2 ;;
    esac
done
CDPATH= cd "$(dirname "$0")/.." || exit 2
ROOT=$(pwd)
LOGS=$ROOT/build/release-check
FAILED=""
JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)

# step NAME COMMAND...: run it, log its output, and say how it went (with the
# test runner's last summary line, if it printed one).
step() {
    name=$1
    shift
    start=$(date +%s)
    if "$@" > "$LOGS/$name.log" 2>&1; then status=ok; else status=FAILED; FAILED="$FAILED $name"; fi
    summary=$(grep -E '[0-9]+ passed|programs \(seeds|check-[a-z]+:|OK$' "$LOGS/$name.log" | tail -1)
    printf '%-24s %-6s %4ss  %s\n' "$name" "$status" "$(($(date +%s) - start))" "$summary"
}

no_warnings() { ! grep -E 'warning:|error:' "$LOGS/build.log"; }

# ThreadSanitizer cannot map its shadow memory with the mmap randomisation of
# recent Linux kernels: run it without (CI lowers vm.mmap_rnd_bits instead).
tsan() {
    if [ "$(uname)" = Linux ] && command -v setarch >/dev/null 2>&1; then setarch "$(uname -m)" -R "$@"; else "$@"; fi
}
check_tsan() {
    make CC="$CC" BUILD=build-tsan BIN=mlwsvm-tsan CFLAGS="-O1 -g -fsanitize=thread -fno-omit-frame-pointer" \
        LDFLAGS="-fsanitize=thread" &&
    TSAN_OPTIONS=halt_on_error=1 MLWSVM_THREADS=4 tsan ./mlwsvm-tsan test tests examples &&
    TSAN_OPTIONS=halt_on_error=1 MLWSVM_THREADS=8 tsan ./mlwsvm-tsan test tests/processes tests/threads \
        tests/native tests/os tests/net tests/libs
}

no_ffi() { make CC="$CC" BUILD=build-noffi BIN=mlwsvm-noffi FFI=0 && ./mlwsvm-noffi test tests examples; }

# The compilers' suites with a VM, in each mode the VM can run them in.
compilers() {
    vm=$1
    shift
    for mode in "$@"; do
        echo "== $mode"
        (cd "$COMPILERS" && env MLWSVM="$vm" $mode make -s test) || return 1
    done
}
MODES="MLWSVM_JIT=off MLWSVM_JIT=on MLWSVM_JIT=on,MLWSVM_JIT_HOT=1 MLWSVM_JIT=all MLWSVM_GC_STRESS=1 MLWSVM_OPT=3
MLWSVM_JIT=all,MLWSVM_GC_STRESS=1 MLWSVM_JIT=all,MLWSVM_OPT=3 MLWSVM_GC_VERIFY=1 MLWSVM_JIT=all,MLWSVM_GC_VERIFY=1"
ASAN_MODES="MLWSVM_JIT=all MLWSVM_JIT=all,MLWSVM_OPT=3 MLWSVM_JIT=on,MLWSVM_JIT_HOT=2"
modes() { echo "$1" | tr ',' ' '; }
compilers_all() {
    set --
    for m in $MODES; do set -- "$@" "$(modes "$m")"; done
    compilers "$ROOT/mlwsvm" "$@"
}
compilers_asan() {
    set --
    for m in $ASAN_MODES; do set -- "$@" "$(modes "$m")"; done
    compilers "$ROOT/mlwsvm-asan" "$@"
}

make clean > /dev/null 2>&1 # (build/ too, with any earlier logs)
mkdir -p "$LOGS"
echo "release check: on $(uname -sm), CC=$CC ($("$CC" --version 2>/dev/null | head -1))"
step build make CC="$CC" -j"$JOBS"
step build-warnings no_warnings
step test make CC="$CC" test
step one-thread env MLWSVM_THREADS=1 ./mlwsvm test tests examples
step check-gc make CC="$CC" check-gc
step check-opt make CC="$CC" check-opt
step check-jit make CC="$CC" check-jit
step check-embed make CC="$CC" lib check-embed
step check-tools make CC="$CC" check-tools
step check-bindgen make CC="$CC" check-bindgen
step check-asan make CC="$CC" check-asan
step asan-opt env MLWSVM_OPT=3 ./mlwsvm-asan test tests examples
step asan-jit-opt env MLWSVM_JIT=all MLWSVM_OPT=3 ./mlwsvm-asan test tests examples
step check-tsan check_tsan
step check-switch make CC="$CC" check-switch
step check-loop make CC="$CC" check-loop
step check-poison make CC="$CC" check-poison
step check-regex make CC="$CC" check-regex
step no-ffi no_ffi
step bench sh bench/run.sh ./mlwsvm
step version ./mlwsvm version
step fuzz ./mlwsvm fuzz --time "$FUZZ" --out build/release-check/fuzz
step fuzz-asan ./mlwsvm-asan fuzz --time "$((FUZZ / 10 + 1))" --out build/release-check/fuzz-asan
if [ -f "$COMPILERS/Makefile" ]; then
    COMPILERS=$(CDPATH= cd "$COMPILERS" && pwd)
    step compilers-build make -s -C "$COMPILERS"
    step compilers compilers_all
    step compilers-asan compilers_asan
else
    echo "compilers                skipped: no $COMPILERS (give it with -C)"
fi
if [ -n "$FAILED" ]; then
    echo "release check: FAILED:$FAILED (logs in $LOGS)"
    exit 1
fi
echo "release check: every step passed"
