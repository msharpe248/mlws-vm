# 3. The interpreter loop

An interpreter spends its life in one loop: take the next instruction, find
the code that implements it, run that code, repeat. Everything except the
work of the instruction itself is overhead, paid on every instruction, so how
the loop is written decides much of how fast the VM is. This chapter follows
one instruction, an `add`, from the form the loader leaves it in, through the
dispatch to its handler and on to the next. It shows the three ways MLWSVM
can compile its dispatch (a `switch`, computed `goto`, and tail calls between
handlers), how a build picks one, and what each measures; then the pieces
around the loop: its state, the time slice, the general path for
instructions without a fast form, and safepoints.

## The decoded instruction

An interpreter that reads its instructions as bytes must decode them every
time it runs them: CPython reads 2-byte code units (an opcode and its
argument), Lua extracts register fields from a 32-bit word with shifts and
masks. MLWSVM's instructions have variable numbers of operands, each with one
of 27 addressing modes (chapter 1), and decoding them on every execution
would cost more than most instructions do. So the loader decodes each
function once, at load time, into an array of `Insn` (`Insn` in
[src/core/insn.h](../../src/core/insn.h)):

```c
typedef struct {
    uint16_t op;  /* dense Opcode */
    uint16_t xop; /* what the interpreter executes: op, or a quickened form (interp.c) */
    uint8_t type, type2;
    uint8_t nops;
    Operand *ops;
    /* A compact copy of a quickened form's operands, filled by interp_quicken ... */
    uint8_t r[3];
    uint32_t target;
    union {
        int64_t i;
        double f;
        const void *p;
    } k;
} Insn;
```

The struct is 32 bytes. `op` is the instruction as written, and `ops` points
at its `nops` decoded operands, each a 72-byte `Operand` with room for every
addressing mode and for what the loader resolved it to (a constant, a
function, a global's address). The general path and the tools read those.

The other fields exist for speed. `xop` is what the interpreter *executes*:
the same as `op`, or, for an instruction of one of a couple of hundred common
shapes, a specialised form such as `Q_ADD_III` (an `i64` add of three `I`
registers). The step that picks it is *quickening*, the subject of chapter 4.
A quickened instruction also carries what its handler needs inside the
`Insn`: register numbers in `r[]`, a branch target in `target`, an immediate
in `k`. So `add I0, I0, I1` becomes, after loading:

```text
op = OP_ADD   type = i64   nops = 3   ops -> [ reg I0 | reg I0 | reg I1 ]
xop = Q_ADD_III   r = {0, 0, 1}
```

[DESIGN.md §8.1](../DESIGN.md#81-decoded-instructions) has the byte layout of
both structs.

## A handler, written once

The code that runs one kind of instruction is its *handler*. MLWSVM writes
each quickened handler once, in [src/vm/qforms.inc](../../src/vm/qforms.inc),
in a small vocabulary of macros listed at the top of the file: `QCASE(x)`
starts the handler for form `x`, `NEXT` goes on to the next instruction,
`JUMP()` makes the branch target the next instruction, `GENERIC()` hands the
instruction to the general path, and `SPILL()` and `RELOAD()` write the fast
path's state back and read it again. The integer add, subtract, multiply and
bitwise forms are one X-macro, expanded once per operator:

```c
#define X(N, op_)                                                             \
        QCASE(Q_##N##_III) IR(0) = (int64_t)(U(IR(1)) op_ U(IR(2))); NEXT;     \
        QCASE(Q_##N##_IIK) IR(0) = (int64_t)(U(IR(1)) op_ U(in->k.i)); NEXT; \
        QCASE(Q_##N##_II) IR(0) = (int64_t)(U(IR(0)) op_ U(IR(1))); NEXT;      \
        QCASE(Q_##N##_IK) IR(0) = (int64_t)(U(IR(0)) op_ U(in->k.i)); NEXT;
        QARITH_OPS(X)
#undef X
```

`IR(k)` is `I[in->r[k]]`, the `I` register named by compact operand `k`.
`U(x)` casts to `uint64_t`: signed overflow is undefined behaviour in C, and
MLWSVM's `add` wraps, so the sum is computed unsigned. `interp.c` includes
this file in one of three ways, each defining `QCASE` and `NEXT` differently.

## Dispatch 1: a `switch` in a loop

The plainest interpreter is a `for` loop around a `switch` on the opcode, as
in the first VM of *Crafting Interpreters*
([A Virtual Machine](https://craftinginterpreters.com/a-virtual-machine.html)).
MLWSVM's fallback is exactly that. In `run_loop`
([src/vm/interp.c](../../src/vm/interp.c)), `QCASE(x)` becomes `case x:` and
`NEXT` becomes `continue`:

```c
    for (;;) {
        ...
        if (left == 0) { /* time slice used up */
            SPILL();
            return SLICE_YIELD;
        }
        ...
        left--;
        in = &code[pc++];
        ...
        switch (in->xop) {
        ...
#include "qforms.inc"
        ...
        default:
            break; /* not quickened: the generic forms below */
        }
```

The program counter `pc`, the code pointer and the slice counter `left` are
locals, which the compiler keeps in machine registers. The cost is the
`switch`: one indirect jump through a table, shared by every instruction,
so the processor must predict every handler's successor at that one jump. And
every handler is part of one large function, whose registers the compiler
allocates once for all of them, so a line added to one handler can change
where the hot variables live in every other.

## Dispatch 2: computed `goto`

GCC and Clang support *labels as values*, an extension to C: `&&label` is the
address of a label, and `goto *p` jumps to it. With a table of handler
addresses, each handler can end by jumping straight to the next instruction's
handler, without going back to the top of the loop. This is *threaded code*.
CPython and Lua use it when built with GCC or Clang. In MLWSVM's threaded
build the table is filled from [src/vm/qlist.inc](../../src/vm/qlist.inc),
which lists every quickened form once, and `NEXT` dispatches in place:

```c
    static const void *const qtab[Q__COUNT] = {
        [0 ... Q__COUNT - 1] = &&generic,
#define QL(x) [x] = &&L_##x,
#include "qlist.inc"
#undef QL
    };
#define QCASE(x) L_##x:
#undef NEXT
#define NEXT                                                                   \
    {                                                                          \
        if (__builtin_expect(left == 0 || hooked, 0)) goto top;                \
        left--;                                                                \
        in = &code[pc++];                                                      \
        goto *qtab[in->xop];                                                   \
    }
```

Every opcode without a quickened form maps to `generic`. Each handler now has
an indirect jump of its own, and the predictor can learn per handler what
usually follows it: after the `inc` in a loop, the compare-and-branch. The
handlers are still one function, with one register allocation.

## Dispatch 3: tail calls between handlers

The third way makes each handler a C function of its own that ends by calling
the next instruction's handler. Done naively, every instruction would push a
C stack frame. But a call in *tail position* (the last thing a function does,
its result returned unchanged) can be compiled as a jump, and
`__attribute__((musttail))`, in Clang and GCC 15, *guarantees* that it is, or
the build fails. The interpreter's hot state travels as the handlers'
arguments, which the calling convention passes in machine registers, so it
stays in registers from one handler to the next. CPython 3.14 can be built
this way too. MLWSVM's version (the tail-call section of
[src/vm/interp.c](../../src/vm/interp.c)):

```c
#define QPARAMS Ctx *c, const Insn *in, uint32_t left, int64_t *I, double *F, Word *R, Frame *qfr, const Insn *qcode
...
#define NEXT                                                                   \
    do {                                                                       \
        if (__builtin_expect(left == 0, 0)) MUSTTAIL return h_slice(QARGSN(nx)); \
        left--;                                                                \
        MUSTTAIL return qhandlers[nx->xop](QARGSN(nx));                        \
    } while (0)
...
#define QCASE(x)                                                               \
    }                                                                          \
    static QCC int h_##x(QPARAMS) {                                            \
        QLOCALS
```

`QCASE` closes the previous handler's function and opens the next, so the
same `qforms.inc` becomes some two hundred functions: `h_Q_ADD_III`,
`h_Q_INC_I`, `h_Q_BLE_IK` and the rest. `QLOCALS` declares `nx = in + 1`;
a branch's `JUMP()` sets `nx = in + target` instead, since in this build
branch targets are stored relative to the instruction. `qhandlers` is the
table of function pointers, filled from `qlist.inc`, with `h_generic` in every
other slot.

`QCC` is `__attribute__((preserve_none))` where Clang has it: a calling
convention that preserves no register for the caller. No handler returns to
its caller, so nothing is lost, a handler that needs many registers never
saves and restores them, and there are argument registers to spare for the
current frame and its code (`qfr`, `qcode`). Elsewhere, and in sanitizer
builds, the parameters stop at the six that x86-64 passes in registers.

The result shows in the machine code. This is `h_Q_ADD_III` in `./mlwsvm` on
an Apple M5 (arm64), from `objdump --disassemble-symbols=_h_Q_ADD_III mlwsvm`,
with comments added:

```text
ldrb  w8, [x21, #0x11]        ; r[1]                   x21 = in
ldr   x8, [x23, x8, lsl #3]   ; I[r[1]]                x23 = the I window
ldrb  w9, [x21, #0x12]        ; r[2]
ldr   x9, [x23, x9, lsl #3]   ; I[r[2]]
add   x8, x9, x8
ldrb  w9, [x21, #0x10]        ; r[0]
str   x8, [x23, x9, lsl #3]   ; I[r[0]] = the sum
cbz   w22, <+0x3c>            ; left == 0: on to h_slice   w22 = left
sub   w22, w22, #0x1          ; left--
ldrh  w8, [x21, #0x22]        ; the next Insn's xop (32 bytes on, +2)
adrp  x9, ...                 ; &qhandlers
add   x9, x9, #0xf08
ldr   x0, [x9, x8, lsl #3]    ; qhandlers[xop]
add   x21, x21, #0x20         ; in = in + 1
br    x0                      ; the tail call: a jump
```

Fifteen instructions, the last of them the dispatch, and not one load or
store of interpreter state: the instruction pointer, the slice counter and
the `I` window arrive in registers and leave in the same registers.

This is the fastest of the three for two reasons. Each handler has its own
indirect branch, as with computed `goto`. And each handler is a function the
compiler allocates registers for on its own, so the code of one cannot make
another slower. Adding an opcode and its handler moves the benchmarks by
about 2% on the M5, where in the one-function loop, code added to it moved
`fib` and `loop` by 7–52%
([PERFORMANCE.md](../PERFORMANCE.md#how-the-interpreter-is-made-fast), items
3 and 9).

## Choosing a dispatch at build time

The preprocessor picks the dispatch in `interp.c`:

```c
#if defined(__has_attribute) && !defined(MLWS_NO_TAILCALL) && !defined(MLWS_NO_THREADED)
#if __has_attribute(musttail)
#define MLWS_TAILCALL 1
#endif
#endif
...
/* Threaded dispatch needs GNU C's labels as values; other compilers use the switch. */
#if (defined(__GNUC__) || defined(__clang__)) && !defined(MLWS_NO_THREADED)
#define MLWS_THREADED 1
```

| Build | Selected by | Built and tested by |
|---|---|---|
| Tail calls | a compiler with `musttail` (Clang, GCC 15) | `make` (`./mlwsvm`) |
| Computed `goto` | GCC before 15, or `-DMLWS_NO_TAILCALL` | `make check-loop` (`./mlwsvm-loop`) |
| `switch` | any other C11 compiler, or `-DMLWS_NO_THREADED` | `make check-switch` (`./mlwsvm-switch`) |

ThreadSanitizer builds take the loop too (its runtime failed an internal
check with the tail-call chain). CI runs the tests in all three. Measured on
the Apple M5 Pro, the three builds made from the same source (best of seven
runs, milliseconds of wall time from `--stats`):

| Benchmark | Tail calls | Computed `goto` | `switch` |
|---|---|---|---|
| `loop` (a tight integer loop) | 83 | 86 | 128 |
| `fib` (recursive calls) | 47 | 51 | 62 |
| `sieve` (byte loads and stores) | 601 | 629 | 678 |
| `closures` | 97 | 97 | 103 |
| `nbody` (mostly generic instructions) | 98 | 98 | 99 |

On the M5 the step from `switch` to threaded code is the large one; tail
calls then take another 3–9% off `loop`, `fib` and `sieve`. On the x86
machines the second step is larger: against the dispatch loop, PERFORMANCE.md
reports `loop` taking 18–19% less time with tail calls, compiled Lua's `loop`
22–37% less, and `fib` 4–9% less
([item 3](../PERFORMANCE.md#how-the-interpreter-is-made-fast)). `nbody`
barely moves in any build: it spends its time on the general path, which is
the same code in all three.

## The time slice and counting instructions

MLWSVM runs many processes on a few threads (chapter 7), so no process may
keep a thread for long. Each turn is a *time slice* of 2,000 instructions
(`--slice N` changes it). `left` counts down what is left of it, and `NEXT`
tests it before every dispatch. At zero, `h_slice` ends the slice:

```c
static MLWS_NOINLINE QCC int h_slice(QPARAMS) {
    Process *p = c->p;
    uint32_t pc = (uint32_t)(in - QCODE);
    QFRAME->pc = pc;
    p->insns = c->ibase - left;
    if (!c->hooked || c->hleft == 0) return SLICE_YIELD; /* time slice used up */
    if (tool_hook(c->vm, p, pc)) return SLICE_BREAK; /* debugger stop: pc not advanced */
    c->hleft--, c->ibase++;
    left = 0;
    MUSTTAIL return qhandlers[in->xop](QARGS);
}
```

Since every handler tail-called the next, there is no chain of C frames to
unwind: the value returned goes straight back to `run_loop`'s caller,
`interp_run`, and from there to the scheduler, which runs another process.

What the handlers do not carry in arguments is in a `Ctx`
(`Ctx` in [src/vm/interp.c](../../src/vm/interp.c)): the process, the frame,
pointers to the current frame's register window in each bank (`I`, `F`, `R`,
`P`; chapter 6), and in the tail-call build the slice's bookkeeping:

```c
    const Insn *code;  /* the current frame's function's code */
    uint64_t ibase;    /* Process.insns = ibase - left */
    uint32_t budget;   /* instructions the slice may run */
    uint32_t hleft;    /* with a tool's hook: instructions left (the handlers' `left` stays 0) */
```

No handler increments the process's instruction count (`Process.insns`,
which `--stats` prints and quotas limit). It is computed when needed, as
`ibase - left`, and the frame's program counter from `in`. `SPILL()` writes
both back, and handlers call it only where something else could look: before
a generic instruction, at calls and returns, before an allocation, and at the
end of the slice.

The tools use the same counter. With the profiler, tracer or debugger
attached, `run_loop` sets `left` to 0 and counts in `hleft` instead, so every
instruction passes through `h_slice`, which calls the tool's hook. Without a
tool, the dispatch pays nothing for them.

## The general path

Of about 130 opcodes and their many operand shapes, only the common
combinations have quickened forms. Everything else, and any quickened
instruction that meets something unexpected, runs through the *generic*
code: one large `switch` on `in->op` in
[src/vm/generic.inc](../../src/vm/generic.inc), which in the tail-call build
is the body of one function, `h_generic`. It writes the state back, runs the
instruction, and dispatches the next like any handler (trimmed):

```c
static QCC QNOSP int h_generic(QPARAMS) {
    ...
    fr->pc = (uint32_t)(in - QCODE) + 1;
    p->insns = c->ibase - left;
#define CX c
#include "generic.inc"
#undef CX
    /* Safepoint: between instructions every live reference is in a root. */
    if (p->gc_pending | p->kill_pending) {
        ...
    MUSTTAIL return qhandlers[nx->xop](QARGSN(nx));
}
```

The generic `add` handles every type and every operand mode
([DESIGN.md §8.4](../DESIGN.md#84-the-generic-path)). It locates the
destination (`loc_of`), reads each source with `rd`, which switches on the
operand's mode and then on the register's bank and canonicalises the value to
the instruction's type (so that an `add.i32` wraps at 32 bits), applies
`arith_binop` from [src/core/arith.h](../../src/core/arith.h), which switches
on the opcode, and writes the result with a check that it fits the
destination's bank. Each step is cheap, but together they cost far more than
the fifteen machine instructions above, as the example below measures.

## Safepoints

A *safepoint* is a point where the VM may stop a process for work that needs
it still: a garbage collection, which must see every live reference, or a
kill. In MLWSVM an allocation that wants a collection only sets
`gc_pending`, and a kill only sets `kill_pending`. Both are checked at the
start of each slice (in `run_loop`) and after each generic instruction (in
`h_generic`, above). Quickened handlers do not trap, and the few that
allocate (`new`, `lnew`, and the string builders such as `strcat` and
`tostr`) call `SPILL()` first. So a collection never runs in the middle of an
instruction, and a run of quickened instructions delays one by at most a
slice ([DESIGN.md §8.5](../DESIGN.md#85-safepoints)).

An error, such as a division by zero, is raised by `vm_trap`, which
`longjmp`s to the `setjmp` that `proc_run_slice`
([src/vm/process.c](../../src/vm/process.c)) sets once per slice; the frame's
`pc`, written before any instruction that can trap, says where it happened.

## Worked example: one loop, three ways

Save this as `sum.masm`:

```asm
; sum.masm - add up the numbers 1 to 1,000,000
.func main
    mov I0, 0                  ; the sum
    mov I1, 1                  ; i
1:  add I0, I0, I1
    inc I1
    ble I1, 1000000, 1b
    ncall "io.print_i" (I0)
    ncall "io.nl" ()
    ret 0
.endf
```

The tracer shows the dispatch going round, one line per instruction, with the
types the assembler filled in:

```text
$ ./mlwsvm run --trace - --trace-limit 8 sum.masm
[1] sum.main+0 (sum.masm:3)  mov.i64 I0, 0
[1] sum.main+1 (sum.masm:4)  mov.i64 I1, 1
[1] sum.main+2 (sum.masm:5)  add.i64 I0, I0, I1
[1] sum.main+3 (sum.masm:6)  inc.i64 I1
[1] sum.main+4 (sum.masm:7)  ble.i64 I1, 1000000, .L2
[1] sum.main+2 (sum.masm:5)  add.i64 I0, I0, I1
...
```

Each of the loop's three instructions is quickened (`Q_ADD_III`, `Q_INC_I`,
`Q_BLE_IK`), so the loop is three handlers tail-calling each other round and
round. `--stats` counts them, and the time slices:

```text
$ ./mlwsvm run --stats sum.masm
500000500000
--- mlwsvm stats ---
instructions:     3000005
...
processes:        1 spawned, 1 at most at once, 1501 time slices
```

Three million instructions in 2,000-instruction slices is 1,500 slices, plus
one for the end. With `--slice 100` the same run takes 30,001.

Now make the add 32-bit, `add.i32 I0, I0, I1`, and save it as `sum32.masm`.
Its result must wrap at 32 bits, so it has no quickened form, and the
profiler says so:

```text
$ ./mlwsvm run --profile sum32.masm
1784293664
...
opcodes (fast = the share run by a specialised handler, chosen at load time):
       %        insns   fast  opcode
   33.3%      1000000   0.0%  add
   33.3%      1000000 100.0%  inc
   33.3%      1000000 100.0%  ble
...
```

On the M5 the 64-bit version runs in 2.0 ms and the 32-bit one in 10.3 ms
(best of nine): one instruction in three on the general path makes the loop
five times slower. Chapter 4 is about keeping instructions off it.

To compare the dispatches, `make check-loop` and `make check-switch` build
`./mlwsvm-loop` and `./mlwsvm-switch` (and run the tests with them); then run
`bench/loop.masm` with `--stats` in each of the three builds.

## Where to look

| What | Where |
|---|---|
| The decoded instruction | `Insn`, `Operand` in [src/core/insn.h](../../src/core/insn.h); `decode_insn` in [src/core/insn.c](../../src/core/insn.c) |
| The quickened handlers, written once | [src/vm/qforms.inc](../../src/vm/qforms.inc); the table's entries in [src/vm/qlist.inc](../../src/vm/qlist.inc) |
| The three dispatches | `MLWS_TAILCALL`, `MLWS_THREADED`, `NEXT`, `QCASE`, `run_loop` in [src/vm/interp.c](../../src/vm/interp.c) |
| The slice, the hook, the general path | `h_slice`, `h_generic` in [src/vm/interp.c](../../src/vm/interp.c); [src/vm/generic.inc](../../src/vm/generic.inc) |
| Around the dispatch | `interp_run` in [src/vm/interp.c](../../src/vm/interp.c); `proc_run_slice` in [src/vm/process.c](../../src/vm/process.c) |
| Reference | [DESIGN.md §8](../DESIGN.md#8-the-interpreter) (§8.1 decoding, §8.3 dispatch, §8.4 the general path, §8.5 safepoints); [PERFORMANCE.md](../PERFORMANCE.md#how-the-interpreter-is-made-fast), items 3, 4 and 9; [REGISTER_VS_STACK.md](../REGISTER_VS_STACK.md#dispatch-where-an-interpreters-time-goes) on dispatch in other VMs |

Previous: [2. From text to instructions](02-assembler.md) · Next: [4. Making an interpreter fast](04-fast-interpreter.md)
