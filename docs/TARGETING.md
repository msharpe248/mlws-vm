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
* Division by zero traps (`E_DIV_ZERO`). Exceptions (milestone M5) will make it
  catchable.

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

## 7. Records and arrays (until milestone M4)

Until the GC-managed `.struct` and array types land, lay out records by hand in
raw memory with `.const` field offsets (see `examples/records.masm`):

```asm
.const NODE_VALUE = 0             ; word offsets
.const NODE_NEXT  = 1
.const NODE_SIZE  = 16            ; bytes
    alloc P1, NODE_SIZE
    mov   P1[NODE_VALUE], I0
    mov.p P1[NODE_NEXT], P0
```

Raw memory is bounds-checked and generation-checked, but it is manually managed
(`free`) and cannot hold GC references.

## 8. A complete translation

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

## 9. Debugging generated code

| Tool | Use |
|---|---|
| `mlwsvm asm -E out.masm` | See the source after macro expansion, with line numbers |
| `mlwsvm dis out.masm` | See exactly what was encoded: types, operand forms, register windows |
| `mlwsvm run --stats out.masm` | Instruction count, speed, call depth, stack peak, raw heap use |
| `mlwsvm test dir/` | Golden tests: put `; expect-stdout:` lines in the generated files |

Every error has a stable code ([ERRORS.md](ERRORS.md)), so compiler tests can assert
on them.

## 10. Stability and what is coming

These are stable and safe to target now: the assembly syntax, the operand forms,
the opcode numbers, the calling convention and the `.mlwso` layout. These are
planned, and will only be *additions*:

| Milestone | Adds |
|---|---|
| M4 | GC heap: strings, byte buffers, arrays, structs (`.struct`, `R2->Point.x`, `R2[I1]`), lists, maps, handles |
| M5 | Exceptions: `.try`, `throw`, `catch`. Traps become catchable |
| M6 | Processes: `spawn`, `send`, `recv`, `link`, `monitor`, quotas |
| M7 | Tooling: `mlwsvm top`, `mlwsvm debug`, `--trace`, `--profile`, web dashboard |
| M8 | Closures, BigInt (`.big`), decimal (`.dec`) |
| M9 | Performance: computed-goto dispatch, multi-threaded scheduler, generational GC |
