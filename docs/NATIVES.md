# MLWSVM Built-in Natives

> Generated from `src/native/natives.c` by `mlwsvm doc natives`. Do not edit by hand.

Natives are host functions called with `ncall "name" (args...) -> (results...)`.
Arguments are read with the parameter types listed below, and the verifier checks
argument counts and register banks at load time. Most programs call them through the
wrappers in `lib/std/*.masm` instead of directly.

| Name | Parameters | Results | Description |
|---|---|---|---|
| `io.print_i` | i64 | - | Print a signed integer. |
| `io.print_u` | u64 | - | Print an unsigned integer. |
| `io.print_hex` | u64 | - | Print an integer in hexadecimal with a 0x prefix. |
| `io.print_f` | f64 | - | Print a float using the shortest representation that round-trips. |
| `io.print_s` | r | - | Print a string or byte buffer (null prints `null`). |
| `io.print_c` | c | - | Print a character (UTF-8 encoded). |
| `io.print_b` | b | - | Print `true` or `false`. |
| `io.print_p` | p | - | Print a pointer for debugging. |
| `io.nl` | - | - | Print a newline. |
| `time.now_ns` | - | i64 | Monotonic clock in nanoseconds. |
| `str.len` | r | i64 | Length of a string or byte buffer in bytes (same as the len instruction). |
| `str.byte` | r, i64 | u8 | Byte at an index of a string (bounds checked). |
| `test.fail` | r | - | Trap with E_ASSERT and the given message. |
| `gc.collect` | - | - | Request a garbage collection; it runs before the next instruction. |
| `gc.heap_bytes` | - | i64 | Bytes currently allocated on this process's heap (live and not yet collected). |
| `gc.collections` | - | i64 | Number of garbage collections so far in this process. |
| `proc.count` | - | i64 | Number of processes currently alive in the VM (including this one). |
| `math.sin` | f64 | f64 | Sine (radians). |
| `math.cos` | f64 | f64 | Cosine (radians). |
| `math.tan` | f64 | f64 | Tangent (radians). |
| `math.asin` | f64 | f64 | Arc sine. |
| `math.acos` | f64 | f64 | Arc cosine. |
| `math.atan` | f64 | f64 | Arc tangent. |
| `math.atan2` | f64, f64 | f64 | atan2(y, x). |
| `math.exp` | f64 | f64 | e raised to x. |
| `math.log` | f64 | f64 | Natural logarithm. |
| `math.log2` | f64 | f64 | Base-2 logarithm. |
| `math.log10` | f64 | f64 | Base-10 logarithm. |
| `math.pow` | f64, f64 | f64 | x raised to y. |
| `math.hypot` | f64, f64 | f64 | sqrt(x*x + y*y) without undue overflow. |
