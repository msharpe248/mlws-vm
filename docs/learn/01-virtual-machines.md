# 1. What a virtual machine is

A virtual machine (VM) is a program that runs programs written for a
computer that does not exist. This chapter explains why that is useful, what
the instructions of such a machine look like, and the loop at the heart of
every interpreter. It then compares the two main designs, stack machines
and register machines, with the same expression compiled both ways and
measured. It ends with MLWSVM's own machine: its four banks of typed
registers, a fresh set of registers for every call, and instructions that
carry their type and any number of operands. Every later chapter builds on
these words.

## Why virtual machines exist

A real CPU runs machine code: bytes that mean something to one family of
processors. A program compiled for an arm64 Mac does not run on an x86-64
Linux server. A virtual machine defines an imaginary processor instead,
with its own instructions, and a program (the VM) that carries them out on
whatever real machine it runs on. The VM's instructions, encoded as bytes,
are called *bytecode*, after VMs whose opcodes are one byte each, such as
the JVM (the Java virtual machine) and CPython.

Three reasons keep coming back:

* **Portability.** Compile once to bytecode, and the program runs wherever
  the VM has been built. Java's "write once, run anywhere" is this idea.
* **Safety.** The VM sees every instruction before the CPU does, so it can
  refuse to do anything the program has no right to: read memory outside
  its own, call the operating system without permission, forge a pointer.
  WebAssembly runs untrusted code in browsers this way.
* **A target for compilers.** A language implementer writes one compiler to
  the VM's instructions and gets every platform the VM runs on, a garbage
  collector, a debugger and a profiler. The JVM hosts Java, Kotlin, Scala and
  Clojure; Erlang's VM, BEAM, hosts Erlang and Elixir.

MLWSVM is built for all three ([DESIGN.md §1](../DESIGN.md#1-goals)). It is
portable C11, and its binary modules hold no machine addresses, only indices
into tables, so a module built on one machine loads on any other. Its safety
goal is that "no program, however wrong or hostile, may crash the VM,
read memory that is not its own, or forge a reference". And it is a
compiler target: its assembly language is the interface, and compilers for
Lox, Lua, Erlang and Brainfuck, in a separate repository, emit it as text.

*Crafting Interpreters* introduces bytecode in
[Chunks of Bytecode](https://craftinginterpreters.com/chunks-of-bytecode.html).

## The fetch-decode-execute loop

A CPU runs a cycle: fetch the instruction the program counter points at,
decode it (which operation, which operands), execute it, move the program
counter on, repeat. An interpreter does the same thing in software. Its
program counter is a variable, its instructions are bytes in an array, and
"execute" is a few lines of C per instruction.

Here is a complete one. It is an illustration, not MLWSVM code: eight
instructions, written to show the loop and nothing else.

```c
/* toy.c - an illustration, not MLWSVM code */
enum { LOAD, STORE, ADD, SUB, MUL,      /* stack: operands are implicit  */
       RADD, RSUB, RMUL,                /* register: dst, src1, src2     */
       HALT };

static long run(const uint8_t *code, int64_t *var) {
    int64_t stack[16];
    int sp = 0;                  /* stack[sp - 1] is the top */
    const uint8_t *pc = code;
    long count = 0;
    for (;;) {
        uint8_t op = *pc++;      /* fetch */
        count++;
        switch (op) {            /* decode */
        case LOAD:  stack[sp++] = var[*pc++]; break;     /* execute */
        case STORE: var[*pc++] = stack[--sp]; break;
        case ADD:   sp--; stack[sp - 1] += stack[sp]; break;
        case SUB:   sp--; stack[sp - 1] -= stack[sp]; break;
        case MUL:   sp--; stack[sp - 1] *= stack[sp]; break;
        case RADD:  var[pc[0]] = var[pc[1]] + var[pc[2]]; pc += 3; break;
        case RSUB:  var[pc[0]] = var[pc[1]] - var[pc[2]]; pc += 3; break;
        case RMUL:  var[pc[0]] = var[pc[1]] * var[pc[2]]; pc += 3; break;
        case HALT:  return count;
        }
    }
}
```

Everything in that loop except the one line of arithmetic in each case is
overhead: reading the opcode, the `switch` (an indirect jump through a
table, which the CPU has to predict), the operand bytes, the loop back. That
overhead is paid once per instruction, whatever the instruction does. Two
consequences follow, and the rest of this series keeps returning to them:
an interpreter is faster if it executes fewer instructions for the same
work, and faster if each instruction costs less to get to.

MLWSVM runs the same cycle, with two differences. Decoding happens once,
when a module is loaded, not every time an instruction runs: each
instruction becomes an `Insn`, a 32-byte structure the interpreter reads
directly (from [src/core/insn.h](../../src/core/insn.h)):

```c
typedef struct {
    uint16_t op;  /* dense Opcode */
    uint16_t xop; /* what the interpreter executes: op, or a quickened form (interp.c) */
    uint8_t type, type2;
    uint8_t nops;
    Operand *ops;
    ...
    uint8_t r[3];
    uint32_t target;
    union { int64_t i; double f; const void *p; } k;
} Insn;
```

And there is no central `switch`. Each instruction's handler ends by
jumping straight to the next one's. The handler for `add` of three integer
registers is one line in [src/vm/qforms.inc](../../src/vm/qforms.inc),
where `IR(k)` is the integer register named by operand `k`:

```c
        QCASE(Q_##N##_III) IR(0) = (int64_t)(U(IR(1)) op_ U(IR(2))); NEXT;     \
```

Compare it with the toy's `RADD` case: the same work, with the operands
already unpacked into `in->r[]`. `NEXT`, in the tail-call build of
[src/vm/interp.c](../../src/vm/interp.c), is the fetch and the dispatch:

```c
#define NEXT                                                                   \
    do {                                                                       \
        if (__builtin_expect(left == 0, 0)) MUSTTAIL return h_slice(QARGSN(nx)); \
        left--;                                                                \
        MUSTTAIL return qhandlers[nx->xop](QARGSN(nx));                        \
    } while (0)
```

`left` counts down the instructions left in this process's time slice
(chapter 7), and `qhandlers[nx->xop]` is the next instruction's handler.
Chapter 3 follows this path in detail, and measures it against a `switch`.

*Crafting Interpreters* builds this loop in
[A Virtual Machine](https://craftinginterpreters.com/a-virtual-machine.html).

## Stack machines and register machines

Every VM must decide where an instruction finds its inputs and puts its
result. There are two main answers.

A **stack machine** keeps them on an operand stack. Instructions take their
inputs from the top and push the result back, so most instructions name no
operands at all. The JVM, the .NET runtime, CPython, Ruby's VM and
WebAssembly work this way.

A **register machine** names them. Each instruction says which numbered
slots to read and which to write. In a VM these "registers" are not CPU
registers: they are slots in an array that belongs to the current call. Lua
(since 5.0), Dalvik (Android's original VM), BEAM and MLWSVM work this way.

Take `a = (b + c) * (b - d)`. A stack machine evaluates it as a Reverse
Polish calculator would:

```text
load b        ; stack: b
load c        ; stack: b c
add           ; stack: (b+c)
load b        ; stack: (b+c) b
load d        ; stack: (b+c) b d
sub           ; stack: (b+c) (b-d)
mul           ; stack: (b+c)*(b-d)
store a       ; stack: empty
```

A register machine names every input and output, with temporaries `t1` and
`t2` for the intermediate results:

```text
add t1, b, c
sub t2, b, d
mul a, t1, t2
```

The toy interpreter above has both kinds of instruction, so it can run both
versions of the expression. With `#include <stdint.h>` and
`#include <stdio.h>` at the top, this `main` completes `toy.c`:

```c
int main(void) {
    enum { A, B, C, D, T1, T2 };          /* variable slots */
    /* a = (b + c) * (b - d) */
    const uint8_t stack_code[] = {
        LOAD, B, LOAD, C, ADD, LOAD, B, LOAD, D, SUB, MUL, STORE, A, HALT };
    const uint8_t reg_code[] = {
        RADD, T1, B, C,  RSUB, T2, B, D,  RMUL, A, T1, T2,  HALT };
    int64_t v1[6] = {0, 7, 5, 3}, v2[6] = {0, 7, 5, 3};
    long n1 = run(stack_code, v1), n2 = run(reg_code, v2);
    printf("stack:    a = %lld, %ld instructions, %zu bytes\n", (long long)v1[A], n1, sizeof stack_code);
    printf("register: a = %lld, %ld instructions, %zu bytes\n", (long long)v2[A], n2, sizeof reg_code);
    return 0;
}
```

```sh
$ cc -std=c11 -O2 -o toy toy.c && ./toy
stack:    a = 48, 9 instructions, 14 bytes
register: a = 48, 4 instructions, 13 bytes
```

(With `b = 7`, `c = 5`, `d = 3`; each count includes the `HALT`.) That is
the whole trade. A stack machine spends instructions moving values to and
from the stack; a register machine spends bits naming where values are.
Here the two programs happen to be the same size, because the toy's
operands are one byte; in real VMs register code is larger.

[REGISTER_VS_STACK.md](../REGISTER_VS_STACK.md) measures the trade on real
VMs, with naive recursive Fibonacci (`fib(32)`, which makes 7 million calls)
and an arithmetic loop of 30 million iterations
([Part 2](../REGISTER_VS_STACK.md#part-2-measured)):

| | MLWSVM | Lua 5.5 | JVM | CPython 3.14 |
|---|---|---|---|---|
| Kind | register | register | stack | stack |
| Instructions executed per `fib` call | 4.5 | 5.5 | 10 | 11.5 |
| Instructions executed per loop iteration | 6 | 7 | 19 | 18 |
| `fib` code size | 76 bytes | 60 bytes | 26 bytes | 40 bytes + caches |
| `fib` time on an Apple M5 Pro (ms) | 50 | 51 | 90 (interpreter only) | 128 |
| loop time on an Apple M5 Pro (ms) | 86 | 213 | 185 (interpreter only) | 1,177 |

The register machines execute about half as many instructions, and their
code is larger, up to three times the JVM's. But fewer instructions is not
the same as faster: the JVM's interpreter, which HotSpot generates as machine code when
it starts, runs the loop faster than Lua while executing almost three times
as many instructions, because each one costs about a third of a nanosecond.
How many instructions a design needs and how much each one costs are two
separate questions. A register machine wins when each instruction is cheap,
which is why MLWSVM pays for decoding once, at load time, instead of on
every instruction.

The JVM and WebAssembly chose stacks for compact code and simple compilers:
emitting stack code is a post-order walk of the expression tree, with no
register allocation. Lua, Dalvik and MLWSVM chose registers to dispatch
fewer instructions. With many registers (Lua offers up to 255 per function,
MLWSVM 32 per bank), register allocation in a compiler can be simple: give
each local variable its own register.

## MLWSVM's machine

### Four typed register banks

Most register VMs have one register file whose slots hold any value, with a
tag saying what each one is. MLWSVM has four banks of 32 registers, and each
bank holds one kind of value
([ASSEMBLY.md §3](../ASSEMBLY.md#3-registers-and-types)):

| Bank | Holds | Natural type |
|---|---|---|
| `I0`–`I31` | integers of any width, booleans, characters | `i64` |
| `F0`–`F31` | floating-point numbers | `f64` |
| `R0`–`R31` | references to garbage-collected objects: strings, lists, maps, structs | `r` |
| `P0`–`P31` | raw pointers, checked against the memory they point into | `p` |

There are also `SP` and `FP`, the data stack and frame pointers, and `PID`,
the running process's id.

The banks are a design decision with consequences in every later chapter.
The garbage collector knows that only `R` registers hold references, so it
finds every live object without a tag on every value (chapter 5). The
verifier can check each instruction on its own, because a register's bank
fixes what it may hold (chapter 2). A float in a register never needs
boxing, and an integer handler never checks a type. Parrot, the VM once planned for
Perl 6, had four register sets the same way
([PRIOR_ART.md](../PRIOR_ART.md)).

Integers are kept *canonical*: an `I` register holds the 64-bit value of
the type last written, sign- or zero-extended. So 8-bit arithmetic wraps as
8-bit arithmetic should, and a 64-bit handler never looks at the type. This
program uses all four banks:

```asm
; banks.masm - one function using all four banks
.include "std/io.inc"

cell:   .i64 0                  ; a module global, one 8-byte word

.func main
    add.i8  I0, 127, 1          ; I: 8-bit arithmetic wraps to -128
    cvt     F0, I0              ; F: cvt.f64.i64 gives -128.0
    mul     F0, F0, 0.5         ; -64.0
    tostr   R0, F0              ; R: a new heap string, "-64"
    lea     P0, @cell           ; P: a checked pointer to the global
    mov     [P0], I0            ; store -128 through it
    PRINT_I @cell
    PRINT_S " "
    PRINTLN_S R0
    ret 0
.endf
```

```sh
$ ./mlwsvm run banks.masm
-128 -64
```

Moving a value between banks always takes an instruction (`cvt` converts,
`bitcast` reinterprets the bits). Writing `add.f64 I0, I1, I2` is an
assembly error: `register I0 cannot hold a value of type f64`.

### A fresh register window per call

Each call gets its own registers, a *window*, sized for the function being
called. The assembler counts the highest register of each bank a function
uses; a call allocates exactly that many, and the registers read as zero
until written. Arguments are bound per bank, in order: the first integer
parameter arrives in `I0`, the first float in `F0`, the first reference in
`R0`. Because every call has its own window, a caller never saves its
registers around a call, and there are no caller-saved or callee-saved
conventions to follow ([ABI.md §2](../ABI.md#2-registers-and-frames)).

All of a process's windows are slices of one array of 8-byte words, with a
window's four banks one after another. A call moves the top of that array
up by the callee's window size and copies the arguments in; a return moves
it back. Lua's frames are windows on one stack too, though Lua's callee
window starts at the caller's argument registers, so a call copies nothing.
REGISTER_VS_STACK.md measures the two at about 7 ns per `fib` call each on
the Mac. Chapter 6 takes calls apart.

### Typed instructions with many operands

An MLWSVM instruction is a mnemonic, an optional type suffix, and its
operands:

```asm
add.i32  I0, I1, 5          ; explicit type
add      I0, I1, 5          ; no suffix: i64, inferred from I0
cvt.f64.i32 F0, I1          ; two types: to.from
add      I0, I1, I2, I3     ; any number of sources: I0 = I1 + I2 + I3
add      @counter, 1        ; a memory operand: a global, updated in place
call fib (I1) -> (I2)       ; a call names its arguments and its results
```

One opcode serves every type: there is one `add`, and its type suffix
(`b c i8 i16 i32 i64 u8 u16 u32 u64 f32 f64 r p big dec`) is a separate
byte, so the instruction set stays at 130 instructions. Without a suffix
the assembler infers the type, usually from the first register operand.
An instruction has 0 to 32 operands, and any operand can be in memory,
including the destination, in the manner of CISC (complex instruction set)
machines such as the VAX. A call is a single instruction carrying all its
arguments and results. A fixed three-operand format, as Lua uses, was
measured: it was no faster, and made `fib`'s calls 13–17% slower
([REGISTER_VS_STACK.md, part 4](../REGISTER_VS_STACK.md#part-4-up-to-32-operands-against-a-fixed-3)).

[ISA.md](../ISA.md) lists every instruction, and
[ADDRESSING.md](../ADDRESSING.md) every operand form.

## A worked example: `fib`

[examples/fib.masm](../../examples/fib.masm) is naive recursive Fibonacci,
the call benchmark from the table above:

```asm
.include "std/io.inc"

.func fib(i64 n) -> i64
    blt n, 2, base
    sub I1, n, 1
    call fib (I1) -> (I2)
    sub I1, n, 2
    call fib (I1) -> (I3)
    add I0, I2, I3
    ret I0
base:
    ret n
.endf

.func main
    call fib (27) -> (I0)
    PRINT_S "fib(27) = "
    PRINTLN_I I0
    ret 0
.endf
```

`.func fib(i64 n) -> i64` declares one integer parameter, which arrives in
`I0` and is called `n` here, and one integer result. `blt n, 2, base`
branches to `base` if `n < 2`: one instruction where the JVM's `fib` needs
four (two loads, `lcmp` and `ifge`). The two calls each pass one register and
receive one result. `PRINT_S` and `PRINTLN_I` are macros from
[lib/std/io.inc](../../lib/std/io.inc).

The disassembler shows what the assembler made of it:

```sh
$ ./mlwsvm dis examples/fib.masm
```

```asm
.func fib(i64) -> (i64)
    .regs I=4, F=0, R=0, P=0
    blt.i64 I0, 2, .L7                      ; line 9
    sub.i64 I1, I0, 1                       ; line 10
    call fib (I1) -> (I2)                   ; line 11
    sub.i64 I1, I0, 2                       ; line 12
    call fib (I1) -> (I3)                   ; line 13
    add.i64 I0, I2, I3                      ; line 14
    ret I0                                  ; line 15
.L7:
    ret I0                                  ; line 17
.endf
...
    ncall "io.print_s" ("fib(27) = ")       ; line 22
    ncall "io.print_i" (I0)                 ; line 23
    ncall "io.nl" ()                        ; line 23
```

Every instruction now has its type (`i64`, inferred from `I0`). The name
`n` has become `I0`. The label `base` has become `.L7`: labels are stored
as instruction indices, and `ret n` is instruction 7. `.regs I=4` is the
window: `fib` uses `I0` to `I3` and nothing else. In `main`, the macros
have become `ncall`, a call to a function built into the VM (a *native*).

Running it with `--stats` prints what the run cost:

```sh
$ ./mlwsvm run --stats examples/fib.masm
fib(27) = 196418
--- mlwsvm stats ---
instructions:     2860297
wall time:        4.739 ms
speed:            603.6 M insn/s
max call depth:   28
stack peak:       0 bytes
...
heap peak:        0 bytes
heap at exit:     0 bytes
objects:          0 allocated, 0 collected
gc:               0 collections (0 of the whole heap), 0.000 ms
...
processes:        1 spawned, 1 at most at once, 1431 time slices
```

Read the numbers against the code. `fib(27)` makes 635,621 calls, and the
run executes 2,860,297 instructions: 4.5 per call, as in the table above
(half the calls take the short path of 2 instructions, half the long one of
7). On an Apple M5 Pro that took about 4.7 ms, 540–650 million instructions
a second over five runs, all of it in the interpreter (the JIT, chapter 8,
is off unless asked for with `--jit on`). The call depth reaches 28. Nothing
used the data stack, and nothing allocated, so the collector never ran: all
of `fib`'s values live in `I` registers. The 1,431 time slices are the
scheduler taking the processor back every 2,000 instructions, which matters
when there is more than one process (chapter 7).

`--profile` breaks the same run down:

```sh
$ ./mlwsvm run --profile examples/fib.masm
...
opcodes (fast = the share run by a specialised handler, chosen at load time):
       %        insns   fast  opcode
   22.2%       635622 100.0%  ret
   22.2%       635621 100.0%  blt
   22.2%       635621 100.0%  call
   22.2%       635620 100.0%  sub
   11.1%       317810 100.0%  add
    0.0%            3   0.0%  ncall
```

Every `ret`, `blt`, `call`, `sub` and `add` ran in a specialised handler,
like the one-line `add` above, chosen when the module was loaded. Chapter 4
explains how.

To watch the loop itself, `--trace -` prints each instruction as it runs.
Here it is on a function `expr(i64 b, i64 c, i64 d) -> i64` holding the
register version of the expression from earlier (`add I3, b, c`,
`sub I4, b, d`, `mul I3, I3, I4`, `ret I3`), called from `main` as
`call expr (7, 5, 3) -> (I0)`:

```text
[1] expr.main+0 (expr.masm:12)  call expr (7, 5, 3) -> (I0)
[1] expr.expr+0 (expr.masm:5)  add.i64 I3, I0, I1
[1] expr.expr+1 (expr.masm:6)  sub.i64 I4, I0, I2
[1] expr.expr+2 (expr.masm:7)  mul.i64 I3, I3, I4
[1] expr.expr+3 (expr.masm:8)  ret I3
```

`[1]` is the process id, then the module, function and instruction index,
the source line, and the instruction.

Try changing `fib` to keep `n - 1` in a new register `I4` instead of
reusing `I1`, and look at `.regs` in the disassembly; or run
`./mlwsvm run --stats --jit on examples/fib.masm` and compare the speed.

## Where to look

| What | Where |
|---|---|
| Why the VM is built as it is | [DESIGN.md §1](../DESIGN.md#1-goals), [§2](../DESIGN.md#2-the-whole-system) |
| Stack and register machines, measured | [REGISTER_VS_STACK.md](../REGISTER_VS_STACK.md) |
| Registers, types, instructions, functions | [ASSEMBLY.md §3–5](../ASSEMBLY.md#3-registers-and-types) |
| Banks, types and canonical values | [DESIGN.md §3.2](../DESIGN.md#32-types-banks-and-canonical-values); `REG_MAKE`, `Bank` in [src/core/types.h](../../src/core/types.h) |
| Every instruction | [ISA.md](../ISA.md), generated from [src/core/opcodes.def](../../src/core/opcodes.def) |
| A decoded instruction | `Insn`, `Operand` in [src/core/insn.h](../../src/core/insn.h) |
| The quickened handlers and dispatch | [src/vm/qforms.inc](../../src/vm/qforms.inc); `NEXT` in [src/vm/interp.c](../../src/vm/interp.c) |
| Calls and register windows | [ABI.md §2](../ABI.md#2-registers-and-frames), [DESIGN.md §9.2](../DESIGN.md#92-the-register-stack) |
| `--stats`, `--profile`, `--trace` | [TOOLING.md](../TOOLING.md#statistics---stats) |
| Examples to run | [examples/](../../examples/) |

Next: [2. From text to instructions](02-assembler.md)
