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
| string, object, array, map, closure (GC), FFI handle | `r` | `R` |
| raw buffer | `p` | `P` |

* **Every function has 32 registers per bank, and they are private to its call.**
  You never save or restore registers around calls. The simplest correct allocator
  gives each local and temporary its own register, and spills to stack locals
  (`enter n` and `LOC[k]`) only past 32 per bank.
* **Use the lowest registers.** A function's window runs to the highest
  register it names in each bank. Scratch registers just above a function's
  own (rather than fixed ones at 28–31) keep windows small and calls cheap.
* **Write a register before reading it.** A new window reads as zero, but a
  call clears only the function's `R` registers and the `I`, `F` and `P`
  registers it may read before writing them (on any path, found when it is
  loaded). A function whose every path writes those first clears just its `R`
  registers, or nothing. The usual way to read one first is to pass on the
  half of a value that its type leaves unused: pass a constant instead
  (`call f (I2, 0.0, null)`, not `call f (I2, F2, R2)` where `F2` was never
  written). `mlwsvm run --profile` shows how much of each hot function's
  window a call clears, and which registers it reads first.
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
* Integer overflow wraps. There are no checked-arithmetic variants, so generate
  explicit checks if the language requires them.
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
* Every function must end with `ret`, `jmp`, `tcall`, `tcalli`, `throw` or `halt`.
  If the source function can fall off its end, emit a final `ret`.

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
* String literals become `"..."` operands. They are immutable, and each value
  is one object in the whole VM, in every module, so equal literals compare
  equal by identity (`beq.r`). The same holds for BigInt and decimal literals
  (`3n`, `1.50d`), which makes them usable as markers across modules (but
  `1.5d` and `1.50d`, written differently, are two objects).
* Lookup tables become data directives with indexed operands:
  `table: .i32 ...` and then `mov.i32 I0, @table{I1*4}`.

### 6.1 Setting up a module: `init`

Loading a module runs none of its code. Its globals start as its data
directives say, so a starting value can only be a number or bytes: a
reference global (`names: .zero 8`) starts null. A global that must hold an
object (a table, a map, a runtime's state) has to be set by code, and in every
process, since each process gets its own copy of every module's globals,
starting from the module's image. Nothing in the VM does this: it is the
compiler's job, and the convention below lets modules from different compilers,
and C hosts, set each other up.

A module that needs setting up exports a function `init`, with no arguments
and no results, that:

1. returns at once if it has already run in this process (a flag global), so
   it may be called any number of times;
2. calls the `init` of each module it imports that has one, first;
3. then sets its own globals, and its flag last.

```asm
.module json5
.import json5.tables as tables
.export init, parse

inited: .i64 0              ; 1 once init has run in this process
names:  .zero 8             ; a map, made by init

.func init
    bnz @inited, 1f         ; already set up here
    call tables.init ()     ; imports first
    mnew R0
    mset R0, "null", 0
    mov @names, R0
    mov @inited, 1
1:  ret
.endf
```

A module imported along several paths (a diamond) is set up once: its second
caller finds the flag set. Imports cannot form a cycle, so an `init` never
re-enters itself and never sees a module half set up.

Every way into the module's code must have called `init` in that process:

* **`main`** calls the `init` of the program's own imports first.
* **Spawned processes** start from the module images too, flags 0 and
  references null. Start every process through an entry function of the
  program's that sets up and then runs the body, and spawn that instead of the
  body: `closure body (args) -> (R0)`, then `spawn start (R0)`, where `start`
  calls the inits and then `calli`s the closure.
* **`module.load`**: after loading a module, call its `init` if
  `module.exports` lists one (`module.function` gives a closure for it).
* **C hosts**: each `mlws_call` starts with fresh module globals
  ([EMBEDDING.md](EMBEDDING.md)), so a function that a host calls must set up
  in the same call.

For a function that may be reached without any of these, such as one handed
to other code as a closure, or called by a host, use the **lazy form**: the
function itself starts with `bnz @inited, 1f; call init (); 1:`. It costs a
test and branch per call, where the entry function costs every `init` once
per process. Keep `init` cheap either way, since processes are meant to be
started freely; a library with costly set-up that most processes never use is
better set up lazily.

`tests/loader/init_convention.masm` shows all of this: a diamond set up once,
a spawned process with and without the entry function, a module loaded at run
time, and the lazy form.

## 7. Objects, records and collections

The heap object types map directly onto scripting-language values
([ASSEMBLY.md §7](ASSEMBLY.md#7-heap-objects-and-structs)):

| Source construct | MLWSVM |
|---|---|
| `"a" + b` | `strcat R0, "a", R1` (up to 31 parts in one instruction) |
| `str(x)`, string interpolation | `tostr R0, x`, then `strcat` |
| `int("42")`, `float(s)` | `parse.i64 I0, R1` / `parse.f64 F0, R1` (`E_PARSE` on failure) |
| `s[i]`, `len(s)`, slices | `R1[I2]` (a byte), `len I0, R1`, `substr R0, R1, start, n` |
| Characters, case, Unicode | `text.chars`/`text.next` (code points), `text.graphemes` (what a reader calls characters), `text.upper`/`lower`/`fold`, `text.normalize`, `text.equal_fold`; `text.is_id_start`/`is_id_continue` for a lexer's identifiers ([STDLIB.md](STDLIB.md#unicode)) |
| A class or record type | `.struct Name` with typed fields; `new R0, Name`; `R0->Name.field` |
| A field or method dispatch check | `bnisa R0, Name, L` (branch unless R0 is a `Name` struct), `isa I0, R0, Name`, or `typeof I0, R0` for the kind |
| The type of a dynamically typed slot | `tagof I0, R1[I2]` (also `UP[n]`, stack and global words) |
| A number of either kind from one slot | `ldv I0, F0, R1[I2]`: the slot's tag picks `I0` or `F0` |
| A fixed-size typed array | `anew.f64 R0, n`; `R0[I1]` |
| A dynamic list / array | `lnew`, `lpush`, `lpop`, `lins`, `ldel`, `R0[I1]`, `len` |
| A dictionary / object literal | `mnew`, `mset`, `mget` (with a default), `mhas`, `mdel`, `mkeys` |
| Sorting and searching a list | `ncall "list.sort" (R0, cmp)` with a comparator closure (or `null` for natural order); `list.bsearch` |
| Set operations | maps as sets: `map.union`, `map.intersect`, `map.difference` |
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
* **One slot per dynamic value.** `tagof I0, R1[I2]` reads the type code an
  element holds (6 `i64`, 12 `f64`, 13 `r`, 0 never written; all of them in
  [ISA.md](ISA.md#types)), so a list element, closure capture or stack word can
  hold an integer, a float or a reference, and code branches on the tag before
  reading it with that type:

  ```asm
      tagof I0, R1[I2]
      beq   I0, 12, Lfloat           ; f64
      bne   I0, 6, Lref              ; otherwise i64, or a reference
      mov   I3, R1[I2]               ; the integer
  ```
* **Two words per dynamic value.** A language that keeps a value in registers
  as a reference (saying what the value is) plus an integer or a float can
  store it in a list as two words, the reference and then the number. `ldv`
  reads the number word into whichever register its tag picks, without a
  branch, and sets the other to zero; a word never written reads as the
  integer 0:

  ```asm
      mov   R3, R1[I2]               ; what the value is
      ldv   I3, F3, R1[I2+1]         ; its number: I3 if an integer, F3 if a float
  ```
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
| `spawn f(x)` / `go f(x)` / a new actor | `spawn f (x) -> (pid)` (runs in parallel) |
| actor `send` / channel write to a known receiver | `send pid, value` (deep copy) |
| actor `receive` / channel read | `recv dst`, or `recvt dst, ms, Ltimeout` |
| `await task` | `spawnm f (x) -> (pid)`, then `recv` the result message, then the `Down` |
| a supervisor | `std.supervisor` (restart strategies and limits), or by hand: `spawnm` / `monitor` (to observe), `spawnl` / `link` (to fail together) |
| a server's API / an actor's methods | `proc.call` (with a timeout) and `proc.reply`; find services by name with `proc.register` / `proc.whereis` |
| async I/O / `await socket.read()` | just call `fs.read` / `net.accept`: the process waits in the poller, holding no thread |
| a sandbox | spawn a wrapper that executes `quota` and then calls the real code |

* Messages are values, not shared objects. For a request/reply protocol, send a
  small struct or list that includes the reply-to pid (`PID`).
* Processes run in parallel, so don't rely on the order in which independent
  processes run: only messages order events. In particular, to observe a child,
  start it with `spawnm` or `spawnl`; a child started with `spawn` can finish
  before a following `monitor` (which then reports `E_NOPROC`).
* For selective receive (Erlang `receive` with patterns), loop on `recv`, inspecting
  each message with `isa` / `typeof`, and keep the ones you don't want yet in a list.
* `examples/workers.masm` shows a supervised worker pool.

### Coroutines and generators

Generators, iterators written with `yield`, Lua coroutines and `async` code
that runs within one process map onto coroutines ([ABI.md](ABI.md#8-coroutines)):
`conew` makes one from a closure, `coresume` runs it until it yields or returns,
and `coyield` passes values out and receives the next resume's. They share the
process's heap, so unlike processes they suit fine-grained control flow, not
parallelism.

| Source construct | MLWSVM |
|---|---|
| a generator function `def g(): ... yield x` | a function whose body executes `coyield (x)`; calling `g()` is `closure g (captures) -> (R0)` + `conew R1, R0` |
| `for x in g()` / `next(it)` | a loop around `coresume R1 -> (x)`, ending when the coroutine is dead (`costatus`) or returns a sentinel |
| `coroutine.create(f)` | `conew R1, R0` (`R0` is the function value, a closure) |
| `coroutine.resume(co, ...)` → `true, ...` or `false, err` | `coresume R1 (args) -> (results)` inside a `.try`; the handler's `catch` gives `err` |
| `coroutine.yield(...)` | `coyield (values) -> (results)`: the results are the next resume's arguments |
| `coroutine.status(co)` | `costatus I0, R1`: 0 suspended, 1 running, 2 normal, 3 dead |
| `coroutine.running()`, `coroutine.isyieldable()` | `cocur R0`: null outside any coroutine (where Lua returns the main thread) |
| `coroutine.wrap(f)` | a closure that holds the coroutine (`UP[0]`) and does `coresume`, letting exceptions propagate |
| `coroutine.close(co)` | `coclose R1` (for a dead coroutine Lua also returns its error, which the compiler must keep) |

* A coroutine is stackful: its function and anything it calls can `coyield`,
  so no transformation of the source is needed, however deep the yield.
* A `coresume` or `coyield` has a fixed number of operands. When the count is
  only known at run time (Lua's `...`), pass one list each way.
* A coroutine's data stack is 8 KiB unless `conew`'s third operand asks for more
  (in words). Code that keeps locals in registers needs little; size it up for
  generated code that spills a lot inside deep recursion.
* Don't yield from code called back by C (`list.sort`'s comparator, an FFI
  callback): that traps with `E_CALLBACK`, as Lua's "attempt to yield across a
  C-call boundary" does.
* A resume and yield together cost about as much as a few calls, and making a
  coroutine about as much as allocating its 8 KiB stack
  ([PERFORMANCE.md](PERFORMANCE.md#coroutines)).

### Calling C

A language's "foreign function" or "native" declarations map to `.extern`:

```asm
.extern m.pow, "libm.so.6|libm", "pow" (f64, f64) -> (f64)
    ncall "m.pow" (F0, F1) -> (F2)
```

The FFI also covers C structs (`.cstruct`, passed as `struct NAME`,
`struct* NAME` or `out struct NAME`), variadic functions (`...`), function
pointers (`.callback`, which C can call back into, or `async` to receive the
calls as messages) and handles that free themselves (`.ffi_free`).
`mlwsvm bindgen header.h` writes these declarations from a C header. For
anything the FFI can't express, wrap the library in a native extension and emit
`.extension "name"` plus `ncall`s. Mark calls that can block as `blocking`, so
they run off the worker threads. A library must be allowed before it can be
loaded (`mlwsvm ext allow-ffi`). See [EXTENDING.md](EXTENDING.md).

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

Emit `.loc "file", line` before the code of each statement (or each line), so
that backtraces, `Error.trace`, the profiler, the tracer and the debugger name
the line of your source program as well as the assembly line
([ASSEMBLY.md §5.4](ASSEMBLY.md#54-source-positions-of-a-translated-program)):

```asm
.func L7$fib(i64 n) -> i64
    .loc "fib.lua", 2
    bge n, 2, 1f
    ret n
1:  .loc "fib.lua", 3
    sub I1, n, 1
    ...
```

| Tool | Use |
|---|---|
| `mlwsvm asm -E out.masm` | See the source after macro expansion, with line numbers |
| `mlwsvm dis out.masm` | See exactly what was encoded: types, operand forms, register windows |
| `mlwsvm run --stats out.masm` | Instruction count, speed, call depth, stack peak, heap and GC use, processes |
| `mlwsvm run --profile out.masm` | Which generated functions, lines and opcodes dominate (exact counts) |
| `mlwsvm run --trace - --trace-limit N out.masm` | Every executed instruction, to compare against what the compiler meant |
| `mlwsvm debug out.masm` | Breakpoints on generated functions or lines (or `fib.lua:3`, with `.loc`), stepping, registers, objects, processes |
| `mlwsvm test dir/` | Golden tests: put `; expect-stdout:` lines in the generated files |

Every error has a stable code ([ERRORS.md](ERRORS.md)), so compiler tests can assert
on them.

## 12. What runs fastest

**Leave the clean-up to the optimizer.** `--opt 2` ([OPTIMIZER.md](OPTIMIZER.md))
removes most of what simple code generation leaves behind:
- moves nothing reads;
- conversions and arithmetic on constants;
- copies of copies;
- jumps to jumps;
- branches on constants;
- code nothing reaches;
- unused functions;
- unused registers.

So a compiler can emit the straightforward translation of each construct and
let the optimizer remove what the context makes redundant. `--opt 3` also
inlines small functions, whose frames are then not in backtraces. The
`tailcall` pass, which makes calls whose results are returned into tail
calls, runs only when named (`--opt 3,tailcall`): it takes the calling
function's frame away, so a language whose semantics read frames (an error's
position in the caller, a level argument) should leave it off. Optimized
modules are kept in the module cache, so the cost is paid
once per module. The rest of this section is about the shapes each
instruction runs fastest in, which the optimizer does not change, except to
put a constant second where a compare or commutative operation has it first.

Every instruction works with every operand form, but the interpreter has fast
paths for the common shapes. They are chosen at load time, so a compiler gets
them by emitting these shapes; nothing else changes.

* **64-bit registers.** Arithmetic and moves on `i64`/`u64` in `I` registers and
  `f64` in `F` registers, with register or constant operands (`add I0, I1, I2`,
  `add I0, 1`, `mul F0, F1, 2.0`, `div F0, 1.0, F1`), shifts (`shl I0, I1, 3`,
  `sar I0, I1`), `sqrt`, `neg` and `abs` of `f64` registers (`sqrt F0` in place,
  or `sqrt F0, F1`), `cvt.f64.i64 F0, I0`, and compare-and-branch on them
  (`blt I0, I1, L`, `bge F0, 0.5, L`), signed or unsigned (`bge.u64 I0, I1, L`:
  one bounds check for `0 <= i < n`). Narrow types (`i32`, `u8`, ...) take the
  general path, because they must wrap.
* **Set-compares** of `f64` registers and constants (`slt.f64 I0, F0, F1`,
  `seq.f64 I0, F0, 0.0`, all six), and reference identity (`seq.r I0, R0, R1`,
  `sne.r I0, R0, null`).
* **Calls with registers and constants as arguments**, and registers as results,
  of the declared 64-bit types: `call f (I1, F0, R2) -> (I3)`,
  `call f (F0, null) -> (F1, R1)`. A constant can be an integer or float for a
  numeric parameter, a string for a reference, or `null`. Memory operands as
  arguments work, but take the general path; move them into registers first in
  hot code. Closure calls and tail calls (`calli R0 (I1, 0.0) -> (I2)`,
  `tcalli R0 (I1, F1)`) are fast when the closure's parameter and result types
  fit, checked per call. `ret` is fast with registers and constants of the
  declared result types (`ret F1, null`). `jtab` on a register is fast.
* **References:** `mov.r R0, null`, `mov.r R0, "text"`, and `beq.r`/`bne.r` of
  a register against a register, a constant or a global
  (`bne.r R0, @cache, L`, the shape of an inline cache).
* **Globals and closure captures moved to or from a register** of the
  matching bank, for 64-bit types: `mov I0, @count`, `mov @count, I0`,
  `mov.r R0, @cache`, `mov F0, UP[1]`, `mov UP[0], I1`. A store sets the
  word's or capture's type; a read of another type takes the general path.
* **Fields and elements moved to or from a register:** `mov I0, R1->Point.x`,
  `mov R1->Point.x, I0`, `mov F0, R1[I2]`, `mov R1[I2], F0`, `mov I0, R1[3]`,
  `mov I0, R1[I2+1]`, for
  64-bit fields, and `i64`/`f64`/`r` arrays and list elements indexed by a
  register or a constant; constants stored to fields
  (`mov.i64 R0->Point.x, 0`, `mov.r R0->Node.next, null`); and `ldv I0, F0, R1[I2]`
  (or `R1[I2+1]`, `R1[3]`) of an `i64` or `f64` list or array element. Arithmetic
  directly on a field or element works, but takes the general path. Loading
  into registers, computing there and storing back is much faster in a loop:
  `bench/nbody.masm` runs in 96 ms with element operands and in 24 ms rewritten
  that way, although it executes more instructions.
* **Lists:** `lpush R0, I1, F2, R3` of untyped register values and constants
  (`lpush R0, "x", 0.0, R1, I1`), while the list has room: each value is
  tagged with its register's or constant's type. Growing the list takes the
  general path, so create lists with their final capacity (`lnew R0, n`) when
  it is known.
* **Objects:** `new R0, Point`; `lnew R0` and `lnew R0, 8`; `isa I0, R0, Point`,
  and `bisa`/`bnisa R0, Point, L`, which branch on it in one instruction;
  `len I0, R0`; `tagof I0, R1[I2]`; null tests `bz R0, L` and `bnz R0, L`;
  `mget.r R0, R1, "name", null`, a reference looked up by a constant string key
  with a null default (an object whose fields are a map); and `mget` of a key
  in any register into a register of each bank, with or without a constant
  default (`mget I0, R1, R2, -1`), when the value has the bank's 64-bit type.

Other allocating instructions (`mnew`, `mset`, `strcat`, an `lpush` that grows
its list, ...) take the general path. To see which of a
program's instructions do, run it with `mlwsvm run --profile`: the report lists
the hottest instructions on the general path, with their lines
([TOOLING.md](TOOLING.md#profiler---profile)).

Calls cost less with smaller windows, and with fewer registers read before
they are written, since each call clears those of its callee (section 2).

Where it matters, prefer one instruction to several (`blt a, b, L` over `slt` +
`bnz`), and keep hot values in registers rather than in `SP[...]` or globals. See
[PERFORMANCE.md](PERFORMANCE.md) for measurements.

## 13. What is coming

The remaining planned work is performance, done when measurements call for it
([ROADMAP.md](ROADMAP.md#4-performance-when-measurements-call-for-it)): more
specialised forms for arithmetic on fields, elements and raw memory, a smaller
decoded form, and possibly a generational collector. These need no changes to
generated code, but which forms get specialised should follow what compilers
actually emit, so the compilers' output will decide them.

A language's `require`, `import()` or plugin loading maps onto `module.load`
and `module.function`, which load a module by name while the program runs and
give closures for its functions ([ABI.md §9](ABI.md#9-loading-modules-at-run-time)).

A language's runtime or standard library can ship as a library archive
(`mlwsvm lib build`, [ASSEMBLY.md §8.1](ASSEMBLY.md#81-libraries)): one
versioned file whose internal modules stay private, which compiled programs
import and pin with `.requires`.

Ship `.masm`: the VM assembles it once and keeps the result in its module cache
([TOOLING.md](TOOLING.md#module-cache)), so a large generated program costs the
assembler only on its first run, even when it is written to a new temporary
file each time. Or assemble `.mlwso` files with the VM that will run them: the
loader accepts only its own format version ([BINARY_FORMAT.md](BINARY_FORMAT.md)).
