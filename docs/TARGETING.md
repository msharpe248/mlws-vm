# Targeting MLWSVM from a Compiler

This guide is for anyone writing a compiler (for example, the scripting-language
project) that emits MLWSVM code. It covers the recommended mappings from high-level
constructs to MLWSVM, and the conventions that keep generated code simple and
correct. Read [ASSEMBLY.md](ASSEMBLY.md) and [ABI.md](ABI.md) first.

## 1. What to emit

**Emit `.masm` text.** It is stable, readable and diffable, and the loader assembles
it in memory, so there is no separate build step: `mlwsvm run out.masm` just works.
Emitting `.mlwso` directly ([BINARY_FORMAT.md](BINARY_FORMAT.md)) is an optimisation
for later. Even then, `mlwsvm dis` turns any module back into assembly for debugging.

Map **one source file to one module**, give the module a dotted name with
`.module`, and lay the output files out to match: `app/util.masm` for `app.util`.
Use `.import` for dependencies and `.export` for public functions and globals.

## 2. Values and registers

| Source-language value | MLWSVM type | Bank |
|---|---|---|
| int, bool, char, enum tag | `i64` / `b` / `c` (or narrower) | `I` |
| float | `f64` (or `f32`) | `F` |
| string, object, array, map, closure (GC) | `r` | `R` |
| raw buffer / FFI pointer | `p` | `P` |

* **Every function has 32 registers per bank, and they are private to its call.**
  You never save or restore registers around calls. The simplest correct allocator
  gives each local and temporary its own register, and spills to stack locals
  (`enter n` and `LOC[k]`) only past 32 per bank.
* **Name registers with `.alias`** in generated code (`.alias count, I4`). It costs
  nothing at runtime and makes the output readable.
* **Use explicit type suffixes for narrow types** (`add.i32`, `mov.u8`). Without
  one, an `I` register means `i64` and an `F` register means `f64`.

## 3. Expressions

MLWSVM is a three-address machine with memory operands everywhere, so expression
trees map onto it almost directly:

```
a = b + c * d            mul  I9, c, d
                         add  a, b, I9

x = p.y + q[i] * 2       mul  I9, P1[I2], 2          ; q is a word array
                         add  x, P0[Y_OFFSET], I9

total = a + b + c + d    add  total, a, b, c, d       ; variadic: one instruction

counter += 1             add  @counter, 1             ; globals are operands too
```

* Comparisons that produce a value use `seq`/`slt`/... (written as a bool).
  Conditional values use `sel`.
* Integer/float conversion is `cvt.f64.i64` / `cvt.i64.f64`, which truncates and
  saturates.
* Integer overflow wraps. Checked-arithmetic variants are planned. Until then,
  generate explicit checks if the language requires them.
* Division by zero traps (`E_DIV_ZERO`). Like every fault, it can be caught with a
  `.try` handler (§8).

## 4. Control flow

```
if c { A } else { B }        bz   c, .Lelse3
                               ...A...
                               jmp  .Lend3
                           .Lelse3:
                               ...B...
                           .Lend3:

while c { A }              .Ltop4:
                               bz   c, .Lend4        ; or a fused compare: bge i, n, .Lend4
                               ...A...
                               jmp  .Ltop4
                           .Lend4:

a && b   (short circuit)       bz   a, .Lfalse5
                               bz   b, .Lfalse5
                               mov  r, 1
                               jmp  .Ldone5
                           .Lfalse5:
                               mov  r, 0
                           .Ldone5:

switch x { 0: .. 1: .. }       jtab x, .Lcase0, .Lcase1, .Lcase2
                               jmp  .Ldefault        ; out of range falls through
```

* Prefer the fused compare-and-branch forms (`blt a, b, L`) over `slt` + `bnz`.
* Labels are local to the function. Generate unique names with a per-function
  counter (`.Lelse3`), or use numeric labels (`1:` / `1b` / `1f`).
* Every function must end with `ret`, `jmp`, `tcall` or `halt`. If the source
  function can fall off its end, emit a final `ret`.

## 5. Functions

```asm
.func score(ref word, f64 bonus) -> (i64, f64)
    ...
    ret s, F1
.endf
```

* **Always declare signatures**, results included. The verifier then checks every
  call site at load time, including calls across modules, and the declared types
  drive argument conversion.
* Parameters are bound **per bank in order**: `(ref w, f64 b, i64 n)` gives
  `w=R0 b=F0 n=I0`.
* **Multiple return values** are native: `call f (x) -> (I0, F0, R3)`. Results can
  go straight into memory: `-> (@cache[I1])`.
* Use **`tcall`** for calls in tail position. It makes tail recursion and
  state-machine style code run in constant space. Tail calls take register
  arguments only.
* Up to 30 register arguments are allowed. For more, or for varargs, push the
  extras in reverse and read them as `ARG[k]` in the callee. The caller must
  `drop` them after the call.
* Recursion is limited to 100,000 frames by default (`E_CALL_DEPTH`).

## 6. Globals, constants and data

* A module-level variable becomes a data directive: `total: .i64 0`. Reference it
  as `@total`. Each process has its own copy.
* Compile-time constants become `.const NAME = value` or literals.
* String literals become `"..."` operands. They are deduplicated per module,
  immutable, and compare equal by identity (`beq.r`).
* Lookup tables become data directives with indexed operands:
  `table: .i32 ...` and then `mov.i32 I0, @table{I1*4}`.

## 7. Objects, records and collections

The heap object types map directly onto scripting-language values
([ASSEMBLY.md §7](ASSEMBLY.md#7-heap-objects-and-structs)):

| Source construct | MLWSVM |
|---|---|
| `"a" + b` | `strcat R0, "a", R1` (up to 31 parts in one instruction) |
| `str(x)`, string interpolation | `tostr R0, x`, then `strcat` |
| `int("42")`, `float(s)` | `parse.i64 I0, R1` / `parse.f64 F0, R1` (`E_PARSE` on failure) |
| `s[i]`, `len(s)`, slices | `R1[I2]` (a byte), `len I0, R1`, `substr R0, R1, start, n` |
| A class or record type | `.struct Name` with typed fields; `new R0, Name`; `R0->Name.field` |
| A field or method dispatch check | `isa I0, R0, Name`, or `typeof I0, R0` for the kind |
| A fixed-size typed array | `anew.f64 R0, n`; `R0[I1]` |
| A dynamic list / array | `lnew`, `lpush`, `lpop`, `lins`, `ldel`, `R0[I1]`, `len` |
| A dictionary / object literal | `mnew`, `mset`, `mget` (with a default), `mhas`, `mdel`, `mkeys` |
| A string builder | `bufnew`, `bufcat`, `bufpush`, then `strcat R0, buf` |
| A lambda / nested function | `closure f (captured...) -> (R0)`; the body reads `UP[n]` |
| Calling a function value | `calli R0 (args) -> (results)` (`tcalli` in tail position) |
| A captured *mutable* variable | box it: capture a one-field struct (or list) and mutate its field |
| Arbitrary-size integers (Python `int`) | `.big` arithmetic on `123n` values; promote from `i64` with `cvt.big.i64` |
| Money and exact decimals | `.dec` arithmetic on `19.99d` values |

* Lists and maps are **dynamically typed**: every element carries its type tag, so
  a dynamically typed language can store anything in them, and `tostr` / `typeof`
  can inspect values. Arrays and struct fields are **statically typed**, which
  suits typed languages and is more compact.
* Map keys compare by value. Integers of any width are compared by value and strings
  by content, so `"apple"` built at run time finds the key `"apple"`. `mkeys` returns
  keys in insertion order, which gives deterministic iteration.
* Never free objects; the collector does it. Keep live objects reachable from an `R`
  register, a stack slot, a global or another object. To refer to one from raw
  memory or host code, use `hnew`.
* Raw memory (`alloc`, `P` registers) is still useful for byte-level buffers, FFI
  and manual layouts. See `examples/records.masm`.

## 8. Exceptions

```
try { A } catch (e) { B }        .try .Lcatch7
                                     ...A...
                                 .endtry
                                     jmp .Lend7
                                 .Lcatch7:
                                     catch R5            ; e
                                     ...B...
                                 .Lend7:

try { A } finally { F }          .try .Lfin8
                                     ...A...
                                 .endtry
                                     ...F...             ; normal path
                                     jmp .Ldone8
                                 .Lfin8:
                                     catch R5
                                     ...F...             ; exceptional path
                                     throw R5            ; rethrow
                                 .Ldone8:

throw MyError(...)                   new R0, MyError     ; any struct or value
                                     throw R0
```

* Typed `catch (e: T)` clauses are an `isa` test (or `typeof`, or a check of
  `R5->Error.code`) in the handler, followed by `throw R5` if nothing matches.
* `return` inside a `try` with a `finally`: emit the finally code before the `ret`.
* The handler runs in the same frame with the same registers. Values that the
  handler needs are just registers.
* Faults are `Error` structs with string fields `code`, `message` and `trace`.
  User exceptions can be anything, and uncaught non-`Error` values report
  `E_UNCAUGHT`.

## 9. Concurrency

Processes give a source language actors, tasks or goroutine-style concurrency
directly:

| Source construct | MLWSVM |
|---|---|
| `spawn f(x)` / `go f(x)` / a new actor | `spawn f (x) -> (pid)` |
| actor `send` / channel write to a known receiver | `send pid, value` (deep copy) |
| actor `receive` / channel read | `recv dst`, or `recvt dst, ms, Ltimeout` |
| `await task` | `monitor pid` then `recv` the result message, then the `Down` |
| a supervisor | `monitor` (to observe) or `link` (to fail together) |
| a sandbox | spawn a wrapper that executes `quota` and then calls the real code |

* Messages are values, not shared objects. For a request/reply protocol, send a
  small struct or list that includes the reply-to pid (`PID`).
* For selective receive (Erlang `receive` with patterns), loop on `recv`, inspecting
  each message with `isa` / `typeof`, and keep the ones you don't want yet in a list.
* `examples/workers.masm` shows a supervised worker pool.

## 10. A complete translation

`examples/translation.masm` (run by `make test`) translates this program:

```
var total = 0
fn score(word: str, bonus: f64) -> (i64, f64) {
    var n = len(word)
    var s = 0
    for i in 0..n { if word[i] == 'a' { s += 10 } else { s += 1 } }
    return (s, s * bonus)
}
fn main() {
    for w in ["banana", "kiwi"] {
        let (pts, weighted) = score(w, 1.5)
        total += pts
        print(w, pts, weighted)
    }
    print("total", total)
}
```

into:

```asm
.include "std/io.inc"

total:  .i64 0

.func score(ref word, f64 bonus) -> (i64, f64)
    .alias n, I0
    .alias s, I1
    .alias i, I2
    .alias ch, I3
    ncall "str.len" (word) -> (n)
    mov s, 0
    mov i, 0
loop:
    bge i, n, end
    ncall "str.byte" (word, i) -> (ch)
    bne ch, 'a', else
    add s, 10
    jmp next
else:
    add s, 1
next:
    inc i
    jmp loop
end:
    cvt.f64.i64 F1, s
    mul F1, bonus
    ret s, F1
.endf
```

## 11. Debugging generated code

| Tool | Use |
|---|---|
| `mlwsvm asm -E out.masm` | See the source after macro expansion, with line numbers |
| `mlwsvm dis out.masm` | See exactly what was encoded: types, operand forms, register windows |
| `mlwsvm run --stats out.masm` | Instruction count, speed, call depth, stack peak, heap and GC use, processes |
| `mlwsvm run --profile out.masm` | Which generated functions, lines and opcodes dominate (exact counts) |
| `mlwsvm run --trace - --trace-limit N out.masm` | Every executed instruction, to compare against what the compiler meant |
| `mlwsvm debug out.masm` | Breakpoints on generated functions or lines, stepping, registers, objects, processes |
| `mlwsvm test dir/` | Golden tests: put `; expect-stdout:` lines in the generated files |

Every error has a stable code ([ERRORS.md](ERRORS.md)), so compiler tests can assert
on them.

## 12. Stability and what is coming

These are stable and safe to target now: the assembly syntax, the operand forms,
the opcode numbers, the calling convention, the heap object model (M4),
exceptions (M5), processes (M6), closures and arbitrary-precision numbers (M8), and
the `.mlwso` layout. These are
planned, and will only be *additions*:

| Milestone | Adds |
|---|---|
| M9 | Performance: computed-goto dispatch, multi-threaded scheduler, generational GC |
