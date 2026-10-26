# MLWSVM Assembly Language

This is the reference for MLWSVM assembly (`.masm`), the language that every MLWSVM
program is written in or compiled to. It is intended both for writing code by hand
and as the target of a compiler.

| Document | Contents |
|---|---|
| **ASSEMBLY.md** (this file) | Syntax, directives, functions, data, type inference, preprocessor, modules |
| [ADDRESSING.md](ADDRESSING.md) | Every operand form (addressing mode), with semantics and encodings |
| [ISA.md](ISA.md) | Every instruction (generated from the opcode table) |
| [ABI.md](ABI.md) | Execution model: registers, calls, the stack, memory, processes |
| [NATIVES.md](NATIVES.md) | Host functions callable with `ncall` (generated) |
| [ERRORS.md](ERRORS.md) | Every error code the assembler, loader and VM can report |
| [BINARY_FORMAT.md](BINARY_FORMAT.md) | The `.mlwso` module file and instruction encoding |
| [TARGETING.md](TARGETING.md) | A guide for compiler writers |

---

## 1. A first program

```asm
; hello.masm
.import std.io as io

.func main
    call io.println ("Hello, MLWSVM!")
    ret 0
.endf
```

```
$ mlwsvm run hello.masm
Hello, MLWSVM!
```

Every program has a function called `main` that takes no parameters. If its first
result is an integer, that value becomes the exit code.

## 2. Lexical structure

The language is line-oriented: one statement per line.

| Element | Syntax |
|---|---|
| Comment | `;` to the end of the line (outside string and character literals) |
| Identifier | `[A-Za-z_.$][A-Za-z0-9_.$]*`, case-sensitive. Dots are allowed, so `std.io.print` and `add.i32` are single identifiers |
| Integer | `42`, `-7`, `0x2A`, `0b101010`, `0o52`, `1_000_000` (underscores are ignored) |
| Float | `3.5`, `1e9`, `2.5e-3`, `inf`, `nan` |
| BigInt | `123456789012345678901234567890n` (decimal digits, any length; `-5n`) |
| Decimal | `19.99d`, `0.1d`, `1.5e-3d` (exact, base 10; `-2.50d`) |
| Character | `'a'`, `'\n'`, `'\x41'`, `'\u{1F600}'`. The value is the Unicode scalar value |
| String | `"text"` with the same escapes. `\xHH` inserts a raw byte; `\u{...}` inserts UTF-8 |
| Booleans / null | `true` (1), `false` (0), `null` (0, valid for `r` and `p` types) |
| Registers | `I0`–`I31`, `F0`–`F31`, `R0`–`R31`, `P0`–`P31`, `SP`, `FP`, `PID`. Uppercase only |

Escapes: `\n \t \r \0 \\ \" \' \xHH \u{HEX}`.

A statement has this shape:

```
[label:]... [directive | instruction | macro-invocation]
```

```asm
loop:   add I0, 1           ; label + instruction
        .i64 1, 2, 3        ; directive
1: 2:   nop                 ; several labels on one line
```

## 3. Registers and types

There are four banks of 32 registers, plus three special registers. Every function
call gets a **fresh window** of registers, so callers never need to save registers
around a call (see [ABI.md](ABI.md)).

| Bank | Holds | Natural type |
|---|---|---|
| `I0`–`I31` | Integers of any width, bools, chars | `i64` |
| `F0`–`F31` | Floats | `f64` |
| `R0`–`R31` | Garbage-collected references: strings, byte buffers, arrays, structs, lists, maps | `r` |
| `P0`–`P31` | Raw, bounds-checked pointers | `p` |
| `SP`, `FP` | Data stack pointer and frame pointer | `p` |
| `PID` | The current process id (read-only) | `i64` |

**Types** are written as instruction suffixes and in signatures:

| Suffix | Alias | Meaning | Size in memory |
|---|---|---|---|
| `b` | `bool` | 0 or 1 | 1 |
| `c` | `char` | Unicode scalar value | 4 |
| `i8 i16 i32 i64` | | Signed integers | 1, 2, 4, 8 |
| `u8 u16 u32 u64` | | Unsigned integers | 1, 2, 4, 8 |
| `f32 f64` | | IEEE 754 floats | 4, 8 |
| `r` | `ref` | GC reference | 8 (word slot) |
| `p` | `ptr` | Raw pointer | 8 |
| `big` | | Arbitrary-precision integer (a reference to an immutable heap object) | 8 (reference) |
| `dec` | | Arbitrary-precision decimal: coefficient × 10^exponent | 8 (reference) |

Integer registers always hold a canonical 64-bit value: signed types are
sign-extended, unsigned types and bools/chars are zero-extended. `add.i8 I0, 127, 1`
leaves `-128` in `I0`. f32 results are rounded to single precision.

## 4. Instructions

```
mnemonic[.type[.type2]] operand, operand, ...
```

```asm
add.i32  I0, I1, 5          ; explicit type
add      I0, I1, 5          ; inferred: i64 from I0
cvt.f64.i32 F0, I1          ; two types: to.from
call fib (I1) -> (I2)       ; call block syntax
```

* **Operands** are any of the addressing modes in [ADDRESSING.md](ADDRESSING.md):
  registers, literals, strings, stack slots `SP[3]`, globals `@x`, pointer arithmetic
  `P0[I1*2+1]`, and so on. *Any* operand may be a memory operand, including the
  destination and all the sources at once.
* **Up to 32 operands.** The arithmetic and bitwise instructions are variadic:
  `add I0, I1, I2, I3` computes `I1 + I2 + I3`.
* **Two-operand shorthand.** `add I0, 5` means `I0 = I0 + 5`. With three or more
  operands the first is the destination only.
* **Evaluation order.** Operands are evaluated left to right, starting with the
  destination's address. This only matters for auto-increment modes:
  `add I0, [SP]+, [SP]+` adds the top two stack words and pops both.

### 4.1 Type inference

An instruction with no type suffix gets its type as follows:

1. `push`, `pop` and `ret`: each operand uses its own **natural type** (see below).
2. `cvt`/`bitcast`: the target type comes from the destination register and the
   source type from the source operand (`cvt F0, I1` is `cvt.f64.i64`).
3. Otherwise the **first register** among the destination and source operands decides:
   `I`→`i64`, `F`→`f64`, `R`→`r`, `P`/`SP`/`FP`→`p`.
4. `mov`/`swap` with no register operand use natural types, so a memory-to-memory
   `mov` copies a word together with its type tag.
5. Otherwise the first literal source decides (integer→`i64`, float→`f64`, string→`r`),
   falling back to `i64` (or the first type the instruction supports).

The **natural type** of an operand: a register's bank type; `i64` for an integer
literal; `f64` for a float literal; `r` for a string; and for memory, the type tag
of the word (see [ABI.md §5](ABI.md#5-memory)), or `i64` if it has none.

Mismatches are assembly errors: `add.f64 I0, I1, I2` fails with
`register I0 cannot hold a value of type f64`. To move between banks, convert
with `cvt` or reinterpret with `bitcast`.

### 4.2 Calls

```asm
call  target (arg, arg, ...) -> (dst, dst, ...)
tcall target (arg, arg, ...)              ; tail call: no results
ncall "native.name" (arg, ...) -> (dst, ...)
```

The argument list and the `-> (...)` result list are both optional. With a single
result the parentheses can be dropped: `call f (1) -> I0`. See [ABI.md](ABI.md) for
how arguments are bound.

## 5. Functions

```asm
.func name [(type [param_name], ...)] [-> type | -> (type, ...)]
    ...instructions...
.endf
```

```asm
.func divmod(i64 a, i64 b) -> (i64, i64)
    div I2, a, b            ; a is I0, b is I1
    rem I3, a, b
    ret I2, I3
.endf
```

* Parameters are bound per bank, in order. Here `i64 a, f64 x, i64 b` puts `a` in
  `I0`, `x` in `F0` and `b` in `I1`. Parameter names are register aliases.
* A declared result list (`-> ...`) is checked by the verifier: every `ret` must
  return exactly those types. Without one, `ret` returns its operands with their
  natural types.
* The assembler computes how many registers of each bank the function uses, and
  `.regs I=8, F=2` can raise those numbers. A call allocates exactly that window.
* The last instruction must be `ret`, `jmp`, `tcall` or `halt`. Execution cannot
  run off the end.
* Instructions are only allowed inside a function.

### 5.1 Labels

* A label defined inside a function is a branch target local to that function.
  Labels with the same name in different functions don't conflict. A leading dot
  (`.Lloop`) is conventional for macro-generated labels but not required.
* **Numeric labels** (`1:`, `2:`) can be redefined. `1b` refers to the nearest
  earlier `1:` and `1f` to the nearest later one. They are ideal inside macros and
  short loops.
* A label defined outside any function names **data** at the current global offset
  (§6).
* Labels cannot be named like registers or `ARG`, `LOC`, `null`, `true`, `false`.

```asm
    mov I1, 0
1:  inc I1
    blt I1, 10, 1b          ; back to the nearest earlier 1:
```

### 5.2 Register aliases

```asm
.alias counter, I5          ; outside a function: module-wide
.func f
    .alias tmp, I9          ; inside: this function only
    mov tmp, counter
    ret tmp
.endf
```

### 5.3 Exceptions

```asm
.func parse_or_zero(ref s) -> i64
.try bad                     ; block form: protect the code up to .endtry
    parse.i64 I0, s          ; traps with E_PARSE on bad input
.endtry
    ret I0
bad:
    catch R0                 ; R0 = the exception (an Error struct for faults)
    ret 0
.endf
```

* `.try handler` ... `.endtry` protects the instructions between the two directives.
  If anything inside them, including any function they call, raises an exception,
  control jumps to `handler`. Blocks can nest.
* `.try start, end, handler` names the region with labels instead. `end` is
  exclusive. This form suits compilers that lay out code themselves. Numeric local
  labels (`1f`) work in both forms.
* `throw R0` raises any non-null reference. `catch dst`, at the start of a handler,
  receives it. Rethrow by throwing the caught value again; a "finally" block is a
  catch-all handler that cleans up and rethrows.
* **Every runtime fault is catchable.** It arrives as a built-in `Error` struct with
  three string fields: `R0->Error.code` (for example `"E_DIV_ZERO"`), `R0->Error.message`
  and `R0->Error.trace` (the backtrace). `Error` is available in every module
  without a declaration, and programs can create and throw their own:
  `new R0, Error`.
* Handlers are tables beside the code, so a `.try` costs nothing until something
  is thrown. Unwinding rules are in [ABI.md §7](ABI.md#7-exceptions-and-traps).

### 5.4 Processes

```asm
.func worker(i64 parent, i64 n)
    mul I2, n, n
    send parent, I2          ; reply to whoever spawned us
    ret
.endf

.func main
    spawnm worker (PID, 7) -> (I0)  ; a new process, monitored from the start; I0 = its pid
    recv I1                  ; 49
    recv R0                  ; the Down struct: R0->Down.exit, R0->Down.code
    ret 0
.endf
```

Processes are isolated: they share no memory and talk only by messages, which
are deep-copied. They run in parallel on several threads, so a new process may
finish before `spawn` returns: use `spawnm` (spawn and monitor) or `spawnl`
(spawn and link) rather than `spawn` followed by `monitor` or `link`. Scheduling, links, monitors, timeouts and quotas are described in
[ABI.md §1](ABI.md#1-processes), and the instructions in the "Processes" section of
[ISA.md](ISA.md). `PID` is the current process's id.

## 6. Data (module globals)

Data directives outside functions append to the module's **global segment**. Each
process gets its own copy, initialised from the module image. A preceding label
names the data, and operands refer to it as `@name`.

```asm
counter:  .i64 0
table:    .i32 10, 20, 30, 40
message:  .asciz "hello"
          .align 8
buffer:   .zero 64

.func main
    add  @counter, 1               ; read-modify-write a global
    mov  I1, 2
    mov.i32 I0, @table{I1*4}       ; byte-indexed: table[2] = 30
    ret
.endf
```

| Directive | Effect |
|---|---|
| `.i8 .i16 .i32 .i64 .u8 .u16 .u32 .u64` *values...* | Integers, aligned to their size |
| `.f32 .f64` *values...* | Floats (integer expressions are converted) |
| `.b` *values...* / `.c` *values...* | Bools (1 byte) / chars (4 bytes) |
| `.ascii "s"` / `.asciz "s"` | String bytes, without / with a terminating NUL |
| `.zero n` / `.space n` | n zero bytes |
| `.align n` | Pad to a multiple of n (a power of two) |

Each 8-byte word of the image carries a type tag (`i64`, `f64`, ...) that untyped
moves respect. The tags record the declared type of the data.

## 7. Heap objects and structs

`R` registers hold references to objects on the process's garbage-collected heap.
Allocate objects with instructions, access their contents with the element and
field operand forms, and never free them: the collector reclaims anything that is
no longer reachable ([ABI.md §6](ABI.md#6-the-garbage-collected-heap)).

| Kind (`typeof`) | Create | Access | Notes |
|---|---|---|---|
| string (1) | `"literal"`, `strcat`, `substr`, `tostr` | `R0[I1]` reads a byte | Immutable bytes, by convention UTF-8 |
| byte buffer (2) | `bufnew R0, n` | `R0[I1]` reads/writes a byte | Mutable and growable: `bufcat`, `bufpush`, `resize` |
| array (3) | `anew.f64 R0, n` | `R0[I1]`, `R0[3]` | Fixed length; every element has the instruction's type |
| struct (4) | `new R0, Point` | `R0->Point.x` | Declared with `.struct`; fields have declared types |
| list (5) | `lnew R0` | `R0[I1]` | Growable; every element carries its own type tag |
| map (6) | `mnew R0` | `mset`, `mget`, `mhas`, `mdel`, `mkeys` | Insertion-ordered; strings and numbers compare by value |
| closure (7) | `closure f (captures) -> (R0)` | `calli R0 (args) -> (results)`; inside f, `UP[n]` | A function plus copies of captured values |
| bigint (8) | `123n`, `cvt.big.i64`, `parse.big` | `add.big`, `blt.big`, ... | Immutable, arbitrary precision |
| decimal (9) | `19.99d`, `cvt.dec.f64`, `parse.dec` | `add.dec`, `div.dec`, ... | Immutable; exact except inexact division |

```asm
.struct Point                 ; a struct type, usable as `Point` in this module
    x: f64
    y: f64
    label: ref
.ends

.func main
    new   R0, Point           ; fields start as 0 / 0.0 / null
    mov   R0->Point.x, 1.5    ; field operands work with every instruction
    add   R0->Point.y, R0->Point.x, 2.0
    anew.i32 R1, 10           ; array of 10 i32 elements
    mov   R1[3], 42
    lnew  R2                  ; a list of mixed values
    lpush R2, 1, 2.5, "three"
    mnew  R3                  ; a map
    mset  R3, "pi", 3.14
    mget  F0, R3, "pi"
    tostr R4, R1[3]           ; "42"
    strcat R5, "answer=", R4
    ret
.endf
```

* **Element operands** `R[Ireg]` and `R[n]` index strings, byte buffers, arrays and
  lists. They are bounds-checked (`E_BOUNDS`), and the index counts elements, not
  bytes. **Field operands** `R->Type.field` check at run time that the object really
  is a `Type` (`E_TYPE`). Both are ordinary operands: you can `add R0[I1], 1`,
  `push R2->Point.label` or pass `R1[0]` to a call.
* **Types are checked.** Array elements and struct fields have fixed types: an
  integer read or write is converted to the element's integer width, and storing a
  float into an `i64` slot traps with `E_TYPE`. A list element or map value
  remembers the type it was stored with. It can be read back as any type of the same
  register bank, but a reference never mixes with a non-reference.
* **Struct types** can be exported (`.export Point`) and used from other modules by
  qualified name: `new R0, geo.Point`, `R0->geo.Point.x`. Fields are matched by name
  when the module is linked.
* **Handles** (`hnew`, `hget`, `hfree`) give an object an `i64` id, so raw memory or
  external code can refer to it and keep it alive.
* There are no pointers into objects: `lea` and the `P`-register addressing forms
  don't apply to them. That is what lets the collector stay precise and safe.

See [ISA.md](ISA.md) (sections "Objects", "Strings", "Lists and maps", "Handles") for
every instruction.

### 7.1 Closures

```asm
.func adder(i64 x) -> i64        ; runs as a closure: UP[0] is the captured amount
    add I1, x, UP[0]
    ret I1
.endf

.func counter -> i64             ; UP[0] is state that belongs to this closure object
    inc UP[0]
    ret UP[0]
.endf

.func main
    closure adder (10) -> (R0)   ; capture 10
    calli R0 (5) -> (I0)         ; 15
    closure counter (0) -> (R1)
    calli R1 -> (I1)             ; 1, then 2, 3, ... on later calls
    ret 0
.endf
```

* `closure f (v...) -> (R0)` copies each value into the new closure, keeping its
  type, as a list element does. Inside `f`, `UP[n]` reads and writes capture n. A
  write changes that closure's copy, which is how `counter` keeps state.
* Captures are **values**. To share a mutable variable between closures (or with
  the creator), capture a struct, list or map and mutate that.
* `calli R0 (args) -> (results)` calls through a closure. Because the callee
  isn't known until run time, the argument count and register banks are checked
  then (`E_TYPE`). `tcalli` is the tail-call form, and `spawni` starts a closure as
  a new process (`spawnim` / `spawnil` also monitor or link it).
* A function that uses `UP[n]` must be called through a closure; called directly
  it traps with `E_TYPE`. Closures can be stored anywhere a reference can, and sent
  in messages, where they are deep-copied along with their captures.

### 7.2 BigInt and decimal

```asm
    mul R0, 123456789012345678901234567890n, 987654321098765432109876543210n
    add R1, 1.10d, 2.205d        ; 3.305 (scale is preserved)
    div.dec R2, 2, 3             ; 0.6666666666666666666666666667
    blt.big R0, 10n, small       ; comparisons and branches take .big / .dec
    cvt.i64.big I0, R0           ; traps E_OVERFLOW if it does not fit
    tostr R3, R1                 ; "3.305"
```

* BigInt and decimal values are immutable heap objects held in `R` registers. The
  type suffix chooses the arithmetic. The assembler infers `.big` or `.dec` when an
  operand is a `123n` or `1.5d` literal; otherwise write the suffix, as plain `R`
  registers would mean `.r`.
* Supported: `add sub mul div rem min max neg abs`, `seq sne slt sle sgt sge`,
  `beq bne blt ble bgt bge`, `mov`, `cvt` to and from every numeric type, `tostr`,
  `parse`. Integer literals mix in freely, and so do float literals for `.dec`.
* **BigInt division** truncates toward zero, and `rem` takes the dividend's sign,
  as for `i64`.
* **Decimal**: `+ - * %` are exact, with scale preserved as in Java's BigDecimal
  (`1.10 + 2.205 = 3.305`, `0.50 * 5 = 2.50`). `div` is exact when the quotient
  terminates within 28 fractional digits (or the operands' scale, if larger);
  otherwise it is rounded half-to-even to that many digits. Trailing zeros of a
  quotient are removed. Converting from a float uses its shortest form, so `0.1`
  becomes exactly `0.1d`.
* Values compare by value: `1.0d == 1.00d`, and maps treat them as the same key.
  A BigInt and a decimal are never equal to each other without a `cvt`.

## 8. Modules and libraries

```asm
.module geometry.vec          ; optional; default is the file name without extension
.import std.io                ; refer to it as std.io.println
.import std.math as math      ; or by alias: math.gcd
.export length, origin        ; functions or data symbols visible to importers
```

* A module named `a.b.c` lives in `a/b/c.masm` (or `a/b/c.mlwso`) under some library
  directory. A library must declare (or default to) exactly the name it is imported
  as.
* **Library path**, searched in order: the program's own directory, each `-L dir`,
  each entry of `$MLWSVM_PATH`, and the `lib/` directory next to the `mlwsvm` executable
  (which holds `std`). If both a `.masm` and a `.mlwso` exist, the newer one is used.
* Imported functions are called by qualified name, `call math.gcd (a, b) -> (g)`.
  Exported data is accessed as `@math.some_table[3]`.
* The **loader** assembles `.masm` sources in memory, or reads `.mlwso` binaries. It
  then loads imports recursively, rejects import cycles (`E_LINK_CYCLE`), resolves
  every symbol (`E_LINK_UNDEF`) and runs the verifier before any code runs.

### 8.1 The standard library (`lib/std`)

| File | Use with | Contents |
|---|---|---|
| `std/io.masm` | `.import std.io` | `print`, `println`, `print_i`, `println_i`, `print_u`, `print_f`, `println_f`, `print_c`, `print_b`, `print_hex`, `nl` |
| `std/math.masm` | `.import std.math` | `gcd`, `ipow`, `isqrt`, `fact`, `clamp`, `fclamp`, `sin`, `cos`, `sqrt`, `pow` |
| `std/io.inc` | `.include "std/io.inc"` | Macros `PRINT_S`, `PRINTLN_S`, `PRINT_I`, `PRINTLN_I`, `PRINT_F`, `PRINTLN_F`, `PRINT_C`, `PRINT_B`, `PRINT_HEX`, `NL`, `PRINTLN s, v...` |
| `std/test.inc` | `.include "std/test.inc"` | Macros `ASSERT_EQ/NE/LT/LE/GT/GE a, b [, type]`, `ASSERT_TRUE/FALSE v [, type]`, `FAIL "msg"` |

## 9. Preprocessor

The preprocessor runs before assembly and works on lines of text. `mlwsvm asm -E
file.masm` prints the fully expanded source, annotated with the original line
numbers and the macros that produced each line.

### 9.1 Constants

```asm
.const WORDS = 16             ; immutable (.equ is a synonym)
.const BYTES = WORDS * 8
.set   i = 0                  ; reassignable
.set   i = i + 1
```

Constant expressions use C precedence: `|| && | ^ & == != < <= > >= << >> + - * / %`
and unary `- + ~ !`. They can use integer, float and char literals, named constants,
`sizeof(type)` (e.g. `sizeof(f64)` is 8) and `defined(NAME)`. Integer arithmetic is
64-bit and wraps. An expression is float if any operand is. Expressions can appear
anywhere a number can, including displacements: `SP[BYTES/8 - 1]`.

### 9.2 Macros

```asm
.macro NAME param1, param2=default, rest...
    ...body using \param1, \param2, \rest...
.endm
```

| In a macro body | Expands to |
|---|---|
| `\name` | The argument for parameter `name` (the longest matching parameter name wins) |
| `\@` | A number unique to this expansion. Use it for labels: `.Lloop\@:` |
| `\()` | Nothing. Use it to separate a parameter from following text: `\op\().i32` |

* Arguments are split at top-level commas; commas inside `()`, `[]`, `{}` and
  quotes don't split. Missing arguments take their default, or the empty string.
* A final `name...` parameter collects all remaining arguments, joined by `, `.
* Macros can invoke other macros, including a macro named by a parameter
  (`\m \arg`). Expansion depth is limited to 64.
* A label before an invocation (`top: MYMACRO x`) labels the first line of the
  expansion.
* A macro cannot have the same name as an instruction.
* Errors inside an expansion report both the macro body line and the invocation
  site.

```asm
.macro SUM dst, vals...
    mov \dst, 0
    .irp v, \vals
    add \dst, \v
    .endr
.endm

.macro COUNT_TO dst, n
    mov \dst, 0
.Lcount\@:
    inc \dst
    blt \dst, \n, .Lcount\@
.endm
```

### 9.3 Repetition

```asm
.rept 4, k                    ; \k = 0, 1, 2, 3 (the variable is optional)
    .i64 \k * \k
.endr

.irp reg, I1, I2, I3          ; iterate over a list
    mov \reg, 0
.endr
```

### 9.4 Conditional assembly

```asm
.if MODE == 2 && defined(FAST)
    ...
.elif MODE == 3
    ...
.else
    .error "unsupported MODE"
.endif

.ifdef NAME / .ifndef NAME    ; constant or macro defined?
.ifb \arg / .ifnb \arg        ; macro argument blank / not blank?
```

Lines in an inactive branch are not even parsed.

### 9.5 Includes

```asm
.include "std/test.inc"
```

The path is searched relative to the including file, then along the library path.
Each file is included at most once, so include files need no guards. By convention,
macro-only files use the `.inc` extension and are not modules.

## 10. Directive summary

| Directive | Where | Purpose |
|---|---|---|
| `.module name` | top level | Module name |
| `.import name [as alias]` | top level | Load and link another module |
| `.extension "name"` | top level | Load an installed native extension ([EXTENDING.md](EXTENDING.md)) |
| `.extern name, "lib", "sym" (types) -> (type) [, blocking] [, serial]` | top level | Make a C function a native (the FFI; [EXTENDING.md](EXTENDING.md)) |
| `.cstruct Name` … `.ends` | top level | A C struct's layout (`field: type` lines), and a struct of the same name, for `.extern` `struct Name` / `struct* Name` / `out struct Name` ([EXTENDING.md](EXTENDING.md#c-structs)) |
| `.callback Name (types) -> (type) [, async]` | top level | A C function-pointer type, for `.extern` parameters `callback Name`: a closure that C calls during the call, or (async) a handle whose calls become messages ([EXTENDING.md](EXTENDING.md#callbacks)) |
| `.ffi_free tag, "lib", "sym"` | top level | Give `ptr<tag>` handles a finalizer: `sym(ptr)` when collected or released ([EXTENDING.md](EXTENDING.md#handles-that-free-themselves)) |
| `.export a, b, ...` | anywhere | Export functions, data symbols or struct types |
| `.func ... / .endf` | top level | Define a function |
| `.struct Name` ... `field: type` ... `.ends` | top level | Define a struct type |
| `.try handler` ... `.endtry`, `.try start, end, handler` | in function | Exception handler region |
| `.regs I=n, F=n, R=n, P=n` | in function | Minimum register window |
| `.alias name, REG` | anywhere | Register alias (module-wide or function-local) |
| `.i8` ... `.f64`, `.b`, `.c`, `.ascii`, `.asciz`, `.zero`, `.space`, `.align` | top level | Global data |
| `.const`/`.equ`, `.set` | anywhere | Named constants |
| `.macro/.endm`, `.rept/.endr`, `.irp/.endr` | anywhere | Macros and repetition |
| `.if/.elif/.else/.endif`, `.ifdef`, `.ifndef`, `.ifb`, `.ifnb` | anywhere | Conditional assembly |
| `.include "file"` | anywhere | Textual include (once per file) |
| `.error "msg"` | anywhere | Stop with `E_ASM_USER` |

## 11. Tools

```
mlwsvm run  [-L dir] [--stats] [--profile] [--trace FILE] [--monitor PORT] [VM options] prog.masm|prog.mlwso
mlwsvm debug [-L dir] prog.masm|prog.mlwso
mlwsvm top  [HOST:PORT] [--once]
mlwsvm asm  [-L dir] [-o out.mlwso] [-E] prog.masm
mlwsvm dis  [-L dir] prog.masm|prog.mlwso
mlwsvm test [-L dir] [-v] [VM options] [paths...]
mlwsvm doc  isa|natives [-o file]
```

`mlwsvm test` runs every `.masm` file under the given paths (default `tests/`),
skipping directories whose names start with `_` (use those for helper modules).
Expectations are written as comments in the test file:

```asm
; expect-stdout: first line of output     (repeatable; compared exactly)
; expect-exit: 3                          (default 0)
; expect-error: E_DIV_ZERO                (an assembler, loader or runtime error code)
; skip: reason
; threads: 4                              (run on this many worker threads, whatever --threads says)
; option: --max-frames 500               (repeatable: a VM option, as on the command line)
; requires: ffi                           (skip unless built with libffi)
; debug-script: break main               (repeatable: run under the debugger; stdout = transcript)
```

Tests run on one worker thread per core unless `--threads N`, `MLWSVM_THREADS` or
a `; threads:` line says otherwise, so a test should not depend on the order in
which independent processes run (see [ABI.md](ABI.md) §1.1).

A test still running after `MLWSVM_TEST_TIMEOUT` seconds (default 300) is
reported as `TIMEOUT path`, and the run ends with exit code 3. A hung VM cannot
be stopped from outside, so the run cannot go on.

See [TOOLING.md](TOOLING.md) for the debugger, profiler, tracer and monitor.
