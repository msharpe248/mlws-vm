#!/bin/sh
# Messages between two VMs on this machine, over TCP, then within one VM:
#     bench/nodes/run.sh [path/to/mlwsvm]
# nodes.erl is the same for Erlang/OTP's distribution (see its top).
BIN=${1:-./mlwsvm}
D=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PORT=${TMPDIR:-/tmp}/mlwsvm-bench-nodes.$$
$BIN run "$D/node2.masm" > "$PORT" &
NODE2=$!
i=0
while ! grep -q '^[0-9][0-9]*$' "$PORT" && [ $i -lt 200 ]; do sleep 0.05; i=$((i + 1)); done
echo "== two VMs"
$BIN run "$D/client.masm" "$(tr -d '\r\n' < "$PORT")"
kill $NODE2
wait $NODE2 2>/dev/null
rm -f "$PORT"
echo "== one VM"
$BIN run "$D/client.masm" 0 local
