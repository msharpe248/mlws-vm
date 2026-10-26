#!/bin/sh
# Run the benchmarks and print wall time and instruction rate for each.
# Usage: bench/run.sh [path/to/mlwsvm]          (make bench)
BIN=${1:-./mlwsvm}
printf "%-14s %10s %12s %12s  %s\n" benchmark "time (ms)" "insns" "M insn/s" check
for f in bench/*.masm; do
    name=$(basename "$f" .masm)
    want=$(grep '^; expect-stdout:' "$f" | sed 's/^; expect-stdout: //')
    out=$($BIN run --stats "$f" 2>"${TMPDIR:-/tmp}/mlwsvm-bench.$$")
    stats=$(cat "${TMPDIR:-/tmp}/mlwsvm-bench.$$")
    ms=$(echo "$stats" | sed -n 's/^wall time: *\([0-9.]*\) ms/\1/p')
    insns=$(echo "$stats" | sed -n 's/^all instructions: *\([0-9]*\).*/\1/p')
    rate=$(awk -v i="$insns" -v t="$ms" 'BEGIN { if (t > 0) printf "%.1f", i / t / 1000; }')
    [ "$out" = "$want" ] && ok=ok || ok="WRONG: $out"
    printf "%-14s %10s %12s %12s  %s\n" "$name" "$ms" "$insns" "$rate" "$ok"
done
rm -f "${TMPDIR:-/tmp}/mlwsvm-bench.$$"
