;; loop.wat - bench/loop.masm in WebAssembly text, for listings only (see fib.wat).
(module
  (func $loop (export "loop") (param $n i64) (result i64)
    (local $acc i64) (local $i i64)
    (block $done
      (loop $top
        (br_if $done (i64.ge_s (local.get $i) (local.get $n)))
        (local.set $acc (i64.xor
          (i64.add (local.get $acc) (i64.rem_s (i64.mul (local.get $i) (local.get $i)) (i64.const 7)))
          (i64.const 0x55)))
        (local.set $i (i64.add (local.get $i) (i64.const 1)))
        (br $top)))
    (i64.rem_s (local.get $acc) (i64.const 100000000))))
