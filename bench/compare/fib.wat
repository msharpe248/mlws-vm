;; fib.wat - bench/fib.masm in WebAssembly text (a stack machine), for listings only:
;; wat2wasm fib.wat -o fib.wasm && wasm-objdump -d fib.wasm. See docs/REGISTER_VS_STACK.md.
(module
  (func $fib (export "fib") (param $n i64) (result i64)
    (if (result i64) (i64.lt_s (local.get $n) (i64.const 2))
      (then (local.get $n))
      (else (i64.add
        (call $fib (i64.sub (local.get $n) (i64.const 1)))
        (call $fib (i64.sub (local.get $n) (i64.const 2))))))))
