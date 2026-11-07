# 6. Calls, frames and exceptions

A function call looks simple in source code, but the VM has to find room
for the callee's registers, pass the arguments, remember where to come back
to, and on return hand back the results and put everything as it was.
Exceptions run the same machinery backwards: a `throw` has to find a
handler, perhaps several calls up, and discard the frames in between. This
chapter shows how MLWSVM keeps its frames, what a call and a return do step
by step, how the data stack, tail calls and closures fit in, and how an
exception or a runtime error unwinds to a handler and prints a backtrace.
In a program written in small functions, calls are a large share of all
instructions, so how fast they are matters a great deal.

## What a call has to do

A C program keeps everything about a call on one stack: the return
address, saved registers, arguments and locals, side by side. That is
compact, and it is why a buffer overflow in C can overwrite a return
address. A VM can choose its own layout. CPython and the JVM give each call
a frame with its local variables and an operand stack. Lua 5 keeps one big
stack of values, and each call's registers are a *window* onto it: the
caller puts the arguments at the top of its own window, and the callee's
window starts right there, so they are already in place. *Crafting
Interpreters* builds Lox's calls that way in
[Calls and Functions](https://craftinginterpreters.com/calls-and-functions.html).

Whatever the layout, a call does five things: read the arguments in the
caller; make a frame for the callee whose registers hold nothing of the
caller's; put the arguments where the callee expects them; remember where
to continue and where the results go; and on return, copy the results
back, drop the frame and continue.

## The register stack and its windows

MLWSVM has four register banks: `I` (integers), `F` (floats), `R`
(references to heap objects) and `P` (checked pointers), up to 32 registers
each. A process keeps all its registers in one array of 8-byte words,
`Process.regs`, the *register stack*. Each frame owns a window of it,
`regs[base .. base + rsize)`, holding its `I`, `F`, `R` and `P` registers
one after another at offsets (`roff`) fixed for each function when it is
loaded ([DESIGN.md §9.2](../DESIGN.md#92-the-register-stack)). On entering
a frame, the interpreter sets one base pointer per bank, so that `I3` is
just `c->I[3]` (`ctx_set` in [src/vm/interp.c](../../src/vm/interp.c)):

```c
static inline void ctx_set(Ctx *c, Frame *fr, const Function *fn, uint64_t *w) {
    c->fr = fr;
    c->I = (int64_t *)w; /* roff[BANK_I] is 0 */
    c->F = (double *)(w + fn->roff[BANK_F]);
    c->R = (Word *)(w + fn->roff[BANK_R]);
    c->P = w + fn->roff[BANK_P];
}
```

Unlike Lua's, MLWSVM's windows do not overlap. A callee gets a fresh
window, and no register is shared between caller and callee, so there are
no caller-saved or callee-saved registers: when a call returns, the
caller's registers are exactly as it left them. The price is that a call
copies its arguments, a word each. Keeping a window's four banks together
on one stack is what makes that cheap. With a separate stack per bank,
every call and return did its bookkeeping four times and `fib` took 37%
longer ([PERFORMANCE.md](../PERFORMANCE.md#how-the-interpreter-is-made-fast));
as it is, a call costs about as much as in Lua 5.5
([REGISTER_VS_STACK.md](../REGISTER_VS_STACK.md#calls-and-frames)).

## A frame

The rest of a call's state is a `Frame`, kept in an array of its own
(`struct Frame` in [src/vm/vm.h](../../src/vm/vm.h), 56 bytes):

```c
typedef struct {
    Function *fn;
    uint32_t pc;
    ...
    uint8_t qret;
    uint8_t stackless; /* fn->stackless: entry_sp and saved_fp are not kept */
    uint32_t base; /* its register window: regs[base .. base + fn->rsize) */
    uint64_t entry_sp; /* SP at the call; restored on return */
    uint64_t saved_fp;
    const Insn *ret_insn; /* caller's call instruction that receives results */
    Obj *closure;         /* the closure being run (UP[n] reads it), or NULL */
} Frame;
```

`pc` is the index after the instruction the frame is running, so in a
caller it is the return address. `ret_insn` is the caller's `call`
instruction, whose operands say where the results go. Both live in the
frame array, which no program can address: however a program misuses its
data stack, it cannot overwrite a return address.

## A call, step by step

The generic `call` in [src/vm/generic.inc](../../src/vm/generic.inc)
handles every shape of call:

```c
        case OP_CALL:
        case OP_TCALL: {
            Function *fn = ops[0].rv.ptr;
            int argc = (int)ops[1].imm;
            Word args[MAX_OPERANDS];
            const MFunc *mf = fn->mf;
            for (int k = 0; k < argc; k++) args[k] = rd(CX, &ops[2 + k], (MlwsType)mf->params[k]);
            const Insn *ret_insn = in;
            if (in->op == OP_TCALL) {
                ret_insn = fr->ret_insn;
                proc_pop_frame(p);
            }
            proc_push_frame(p, fn, ret_insn);
            ctx_load(CX);
            bind_args(CX, NULL, mf->params, argc, args);
            break;
        }
```

It reads each argument in the caller with the type of the callee's
parameter (an argument may be a register, a constant, a stack slot, a
field), pushes the frame, and binds the arguments. Pushing is
`proc_push_frame_fitting` in
[src/vm/proc_internal.h](../../src/vm/proc_internal.h):

```c
PROC_INLINE void proc_push_frame_fitting(Process *p, Function *fn, const Insn *ret_insn) {
    Frame *fr = &p->frames[p->nframes++];
    fr->fn = fn;
    fr->pc = 0;
    fr->ret_insn = ret_insn;
    fr->closure = NULL;
    fr->qret = 0;
    fr->stackless = fn->stackless;
    if (!fn->stackless) {
        fr->entry_sp = p->sp;
        fr->saved_fp = p->fp;
        p->fp = p->sp;
    }
    fr->base = p->rtop;
    p->rtop += fn->rsize;
    uint64_t *w = p->regs + fr->base;
    ...
    ZERO_REGS(w + fn->zoff, fn->zlen);
    if (p->nframes > p->max_depth) p->max_depth = p->nframes;
}
```

The window starts at the top of the register stack, `rtop`, which moves up
by the window's size. Every register of a new window must read as zero
until written, but clearing whole windows costs a `memset` per call. So the
verifier works out once per function which registers need it
([DESIGN.md §7.1](../DESIGN.md#71-planning-the-register-window)): the `R`
registers, because the collector scans the whole `R` window (chapter 5),
and any register the function might read before writing it, found by a
dataflow analysis (`plan_window` in [src/vm/verify.c](../../src/vm/verify.c)).
A call clears that one run, `zlen` words from `zoff`. The profiler shows
it per function; `sum`, below, writes each of its three registers before
reading it and has no `R` registers, so its calls clear nothing:

```text
$ ./mlwsvm run --profile sum.masm
...
       %        insns    entries   cleared  function
  100.0%        50002      10001       0/3  sum.sum
```

`bind_args` then puts the arguments in the callee's banks in order: the
first integer parameter in `I0`, the first float in `F0`, the first
reference in `R0`, so `.func f(i64 a, f64 x, ref s, i64 b)` receives `a`
in `I0`, `b` in `I1`, `x` in `F0` and `s` in `R0`. The way back needs no
work: the caller's `pc` already points after the call, and the new frame's
`ret_insn` is the call.

`ret` reads its values in the callee, pops the frame, and writes them to
the result operands of `ret_insn`. Popping is at most three stores
(`proc_pop_frame`):

```c
PROC_INLINE void proc_pop_frame(Process *p) {
    Frame *fr = &p->frames[--p->nframes];
    if (!fr->stackless) { /* (a stackless function's leaves them as they were) */
        p->sp = fr->entry_sp;
        p->fp = fr->saved_fp;
    }
    p->rtop = fr->base;
}
```

That is the general path. Most calls pass registers of the right banks and
receive results in registers, and at load time they are quickened
(chapter 4) into `Q_CALL_R` in [src/vm/qforms.inc](../../src/vm/qforms.inc).
Its plan, made once, holds each argument's slot in the caller's window and
in the callee's, so the handler copies words straight across with no test
of their types, and marks the frame `qret` so that the matching quickened
`ret` copies the results back the same way.

**Growing.** The frame array starts with 8 frames and the register stack
with 64 words, so a process that never calls deeply stays small.
`proc_push_frame` checks with one comparison per array that the frame
fits, and otherwise calls `proc_frame_slow` in
[src/vm/process.c](../../src/vm/process.c), which doubles them (they may
move, so the call handlers reload their pointers) or, at the depth limit,
traps `E_CALL_DEPTH`. Frames are ordinary memory from `malloc`, not the C stack, so
the limit (100,000 frames, `--max-frames`) is a choice rather than a crash,
and `E_CALL_DEPTH` is an exception a program can catch.

## The data stack

Registers are not addressable: nothing can point at one. For values that
need an address, and for more arguments than a call instruction holds
(30), each process also has a *data stack*, 1 MiB by default, allocated the
first time something uses it
([DESIGN.md §9.5](../DESIGN.md#95-the-data-stack)). It grows down. A call
sets `FP` to `SP`, so `ARG[n]` (`FP[n]`) is the `n`th word the caller
pushed and `LOC[n]` (`FP[-1-n]`) the `n`th local reserved by `enter`.
There is no `leave`: `ret` puts `SP` back from the frame.

```asm
; stack.masm - arguments on the data stack, and a local reserved with enter
.func sum3 -> i64                   ; three arguments, passed on the stack
    enter 1                         ; LOC[0], a zeroed word below FP
    add LOC[0], ARG[0], ARG[1]
    add I0, LOC[0], ARG[2]
    ret I0                          ; SP goes back to what it was at the call
.endf

.func main
    push 3, 2, 1                    ; pushed right to left: ARG[0] = 1
    call sum3 -> (I0)
    drop 3                          ; the caller removes its arguments
    ncall "io.print_i" (I0)
    ncall "io.nl" ()
    ret 0
.endf
```

It prints 6. `./mlwsvm dis stack.masm` shows the names as what they are,
`add.i64 FP[-1], FP[0], FP[1]`, and `--stats` reports `stack peak: 32
bytes`: three arguments and one local.

Most functions never touch the data stack. When it loads a function
(`decode_function` in [src/vm/loader.c](../../src/vm/loader.c)), the
loader marks it *stackless* if it has no `push`, `pop`, `drop` or `enter`
and no operand naming `SP` or `FP` (which `ARG[n]` and `LOC[n]` are). Such
a function leaves `SP` and `FP` as it found them, so its frame need not
save them and its pop need not restore them: the `if (!fn->stackless)` in
the push and pop above. With the JIT that removes five of the twenty or so
stores a call and its return make, and the `fib` programs take 4–22% less
time for it ([DESIGN.md §9.4](../DESIGN.md#94-pushing-and-popping-a-frame)).

## Tail calls

When the last thing a function does is call another and return its
results, its own frame is no longer needed. `tcall f (args)` pops the
current frame and pushes the callee in its place with the same
`ret_insn`, so the callee's results go straight to the original caller:
the `OP_TCALL` branch of the generic call above. Recursion written this way
runs in constant space, which Erlang and Lua programs depend on (Lua's
`OP_TAILCALL` reuses the frame too). Because the frame is gone before the
callee starts, a tail call takes register arguments only, never stack
arguments ([ABI.md §2.5](../ABI.md#25-tail-calls)). The worked example
below measures the difference.

## Closures

A closure is a function together with values captured when it was made.
`closure f (v1, v2) -> (R0)` makes an `ObjClosure` holding the function
and copies of the values, each with its type tag; inside `f` they are
`UP[0]` and `UP[1]`. `calli R0 (args) -> (results)` binds arguments like
`call`, then records the closure in the new frame's `closure` field, which
is how `UP[n]` finds it and how the collector keeps the captures alive
while it runs (`OP_CALLI` in generic.inc).

```asm
; closure2.masm - a function value with a captured value
.func adder(i64 x) -> i64           ; UP[0]: the amount captured when it was made
    add I1, x, UP[0]
    ret I1
.endf

.func main
    closure adder (10) -> (R0)      ; capture 10
    calli R0 (5) -> (I0)
    ncall "io.print_i" (I0)
    ncall "io.nl" ()
    call adder (1) -> (I2)          ; called directly: no closure, so UP[0] traps
    ret 0
.endf
```

```text
$ ./mlwsvm run closure2.masm
15
runtime error[E_TYPE]: UP[0] used in adder, which was not called as a closure
  at closure2.adder (closure2.masm:3)
  at closure2.main (closure2.masm:12)
```

Captures are values, not variables. *Crafting Interpreters*'
[Closures](https://craftinginterpreters.com/closures.html) chapter builds
upvalues that share a variable with the function that made them; in MLWSVM
a compiler that needs that captures a struct or a list and mutates it
([ASSEMBLY.md §7.1](../ASSEMBLY.md#71-closures)).

## Exceptions

An exception transfers control to a handler that may be several calls up,
popping the frames in between. Two costs matter: what a `try` costs when
nothing is thrown, and what a throw costs. The JVM and C++'s "zero-cost"
exceptions answer the first with *tables*: beside a function's code is a
list of protected ranges and their handlers, consulted only when something
is thrown. MLWSVM does the same. A `.try` region becomes an `MHandler` of
the function ([src/core/module.h](../../src/core/module.h)):

```c
/* An exception handler region: instructions [start, end) are protected by the
 * handler at `target`. Indices are instruction numbers within the function. */
typedef struct {
    uint32_t start, end, target;
} MHandler;
```

The disassembler prints one as `.try .L1, .L6, .L7` (start, end, handler).
No instruction runs to enter or leave a region.

The throw uses C's `setjmp` and `longjmp`, but `setjmp` runs once per time
slice, not per call or per `.try`. `proc_run_slice` in
[src/vm/process.c](../../src/vm/process.c) sets the jump buffer and runs
the interpreter; every throw and every runtime error jumps back to it (on
macOS and the BSDs `VM_SETJMP` is `_setjmp`, which skips saving the signal
mask with a system call):

```c
    p->jb = &jb;
    if (VM_SETJMP(jb)) { /* every throw and trap lands here */
        p->jb = NULL;
        ...
        if (unwind(p)) return SLICE_YIELD; /* continue at the handler next turn */
        describe_uncaught(p);
        return SLICE_FAILED;
    }
    int r = interp_run(p, slice);
```

`unwind_frames` then looks for a handler, innermost frame first:

```c
static bool unwind_frames(Process *p) {
    while (p->nframes > p->cb_base) { /* a callback's exceptions never unwind the code that called C */
        Frame *fr = &p->frames[p->nframes - 1];
        const MFunc *mf = fr->fn->mf;
        uint32_t pc = fr->pc ? fr->pc - 1 : 0;
        const MHandler *best = NULL;
        for (uint32_t i = 0; i < mf->nhandlers; i++) {
            const MHandler *h = &mf->handlers[i];
            if (pc >= h->start && pc < h->end && (!best || h->end - h->start < best->end - best->start)) best = h;
        }
        if (best) {
            fr->pc = best->target;
            return true;
        }
        proc_pop_frame(p);
    }
    return false;
}
```

In the frame that threw, `pc - 1` is the throwing instruction; in every
frame below it, the `call` still in progress. The smallest region
containing it wins, so nested regions work whatever order they are listed
in. A frame with no handler is popped exactly as `ret` pops it, which
restores `SP`. If a handler is found, its frame continues there, in the
next slice, with its registers as they were, and `catch R0` fetches the
exception. If none is, the process fails.

**Traps.** `throw R0` throws any non-null reference. A runtime error is
thrown the same way: `vm_trap` builds the backtrace, makes a built-in
`Error` struct with three strings, and throws it:

```c
    ObjStruct *e = struct_new(p, &VM_ERROR_DEF);
    e->fields[ERRF_CODE].r = str_new(p, code, strlen(code));
    e->fields[ERRF_MESSAGE].r = str_new(p, msg, strlen(msg));
    e->fields[ERRF_TRACE].r = str_new(p, p->backtrace.s ? p->backtrace.s : "", p->backtrace.n);
    ...
    proc_throw(p, &e->h);
```

The codes are stable, part of the VM's interface, and listed in
[ERRORS.md](../ERRORS.md#runtime-traps): `E_DIV_ZERO`, `E_NULL`,
`E_BOUNDS`, `E_TYPE`, `E_KEY`, `E_CALL_DEPTH` and the rest. Every one is
catchable, so a compiler can turn them into its own language's errors, and
tests check a program by the code it fails with.

**Backtraces** are built from the frames before any are popped
(`build_backtrace` and `bt_frames` in process.c). Each line names a
function and the file and line of the instruction its frame is at. A run of
identical lines (direct recursion) is collapsed, and a very deep stack
keeps its innermost 40 and outermost 10 frames. A compiler records its own
source's positions with `.loc`
([ASSEMBLY.md §5.4](../ASSEMBLY.md#54-source-positions-of-a-translated-program)),
and the backtrace shows them first.

## Worked examples

### Recursion, a tail call and the depth limit

`sum` adds 1 to n by plain recursion: the `add` waits for the call, so
every level keeps its frame. `sum_acc` passes the total along and ends in a
tail call.

```asm
; sum.masm - 1 + 2 + ... + n, by plain recursion and by a tail call
.func sum(i64 n) -> i64             ; n + sum(n - 1): the add waits for the call
    bz n, 1f
    sub I1, n, 1
    call sum (I1) -> (I2)
    add I0, n, I2
    ret I0
1:  ret 0
.endf

.func sum_acc(i64 n, i64 acc) -> i64  ; nothing left to do after the call
    bz n, 1f
    add I1, acc, n
    sub I0, n, 1
    tcall sum_acc (I0, I1)
1:  ret acc
.endf

.func main
    call sum (10000) -> (I0)
    ncall "io.print_i" (I0)
    ncall "io.nl" ()
    ret 0
.endf
```

```text
$ ./mlwsvm run --stats sum.masm
50005000
--- mlwsvm stats ---
instructions:     50006
...
max call depth:   10002
```

That is `main`, then `sum` for each n from 10,000 down to 0. In
`sum_tail.masm`, the same file with `main` calling
`sum_acc (10000, 0)`, the depth stays at 2, `main` and whichever `sum_acc`
is current:

```text
$ ./mlwsvm run --stats sum_tail.masm
50005000
--- mlwsvm stats ---
instructions:     40006
...
max call depth:   2
```

With `sum (1000000)` the plain recursion reaches the limit, and the
backtrace collapses the 99,999 identical `sum` frames into two lines.
`mlwsvm run` exits with status 70 for an uncaught runtime error.

```text
$ ./mlwsvm run sum_deep.masm
runtime error[E_CALL_DEPTH]: call depth limit of 100000 frames exceeded (unbounded recursion?)
  at sum_deep.sum (sum_deep.masm:5)
  ... repeated 99998 more times
  at sum_deep.main (sum_deep.masm:20)
```

### A throw caught two frames up

`check` throws an `Error` of its own, `middle` has no handler, and `main`
catches it:

```asm
; catch2.masm - a throw in check, caught two frames up in main
.func check(i64 n)                  ; throws for a negative n
    bge n, 0, 1f
    new R0, Error
    mov R0->Error.code, "E_NEGATIVE"
    throw R0
1:  ret
.endf

.func middle(i64 n) -> i64          ; no handler: unwinding pops this frame
    call check (n)
    mul I1, n, 2
    ret I1
.endf

.func main
    mov I5, 7                       ; set before the call
.try failed
    call middle (-1) -> (I0)
.endtry
    ret 0
failed:
    catch R1                        ; R1 = the Error that check threw
    ncall "io.print_s" (R1->Error.code)
    ncall "io.nl" ()
    ncall "io.print_i" (I5)         ; main's registers are as main left them
    ncall "io.nl" ()
    ret 1
.endf
```

```text
$ ./mlwsvm run catch2.masm; echo "exit=$?"
E_NEGATIVE
7
exit=1
```

The `throw` is in no region of `check`, so its frame is popped. In
`middle` the faulting instruction is `call check`, in no region, so that
frame goes too. In `main` it is `call middle`, inside the `.try`, so `main`
continues at `failed` with its own registers: `I5` is still 7.

### A runtime error and its backtrace

```asm
; trap.masm - a division by zero two calls deep, not caught
.func average(i64 total, i64 count) -> i64
    div I2, total, count            ; count = 0: E_DIV_ZERO
    ret I2
.endf

.func report(i64 total, i64 count)
    call average (total, count) -> (I2)
    ncall "io.print_i" (I2)
    ncall "io.nl" ()
    ret
.endf

.func main
    call report (10, 2)
    call report (10, 0)
    ret 0
.endf
```

```text
$ ./mlwsvm run trap.masm; echo "exit=$?"
5
runtime error[E_DIV_ZERO]: integer division by zero
  at trap.average (trap.masm:3)
  at trap.report (trap.masm:8)
  at trap.main (trap.masm:16)
exit=70
```

Each line is a frame, innermost first, at the instruction it was running:
the `div`, then the two calls in progress. Caught in `main` instead (a
`.try 1f` around the call, `1: catch R0`), the same error gives the program
`R0->Error.code`, `"E_DIV_ZERO"`, and the same lines in `R0->Error.trace`.
And when a compiler has marked `average`'s `div` with
`.loc "stats.lox", 2` and `main`'s call with `.loc "stats.lox", 6`, the
backtrace leads with the Lox program's lines:

```text
runtime error[E_DIV_ZERO]: integer division by zero
  at trap_loc.average (stats.lox:2; trap_loc.masm:4)
  at trap_loc.main (stats.lox:6; trap_loc.masm:10)
```

## Where to look

| What | Where |
|---|---|
| Frames and windows | `struct Frame`, `frame_reg` in [src/vm/vm.h](../../src/vm/vm.h) |
| Pushing and popping | `proc_frame_fits`, `proc_push_frame_fitting`, `proc_pop_frame` in [src/vm/proc_internal.h](../../src/vm/proc_internal.h); `proc_frame_slow` in [src/vm/process.c](../../src/vm/process.c) |
| Generic calls and returns | `OP_CALL`, `OP_TCALL`, `OP_RET`, `OP_CALLI` in [src/vm/generic.inc](../../src/vm/generic.inc); `bind_args`, `ctx_set`, `ctx_load` in [src/vm/interp.c](../../src/vm/interp.c) |
| Quickened calls and returns | `Q_CALL_R`, `Q_RET_R`, `Q_TCALL_R` in [src/vm/qforms.inc](../../src/vm/qforms.inc) |
| Which registers a call clears | `plan_window` in [src/vm/verify.c](../../src/vm/verify.c) |
| Stackless functions | `decode_function`, `uses_stack_reg` in [src/vm/loader.c](../../src/vm/loader.c) |
| Closures | `ObjClosure` in [src/heap/object.h](../../src/heap/object.h); `OP_CLOSURE`, `OP_CALLI` in generic.inc |
| Throwing and unwinding | `proc_throw`, `vm_trap`, `unwind`, `unwind_frames`, `proc_run_slice`, `describe_uncaught` in process.c; `MHandler` in [src/core/module.h](../../src/core/module.h) |
| Backtraces | `build_backtrace`, `bt_frames`, `frame_line`, `fn_where` in process.c |
| Reference | [DESIGN.md §9](../DESIGN.md#9-a-process-in-memory) and [§12](../DESIGN.md#12-exceptions-and-traps), [ABI.md §2](../ABI.md#2-registers-and-frames) and [§7](../ABI.md#7-exceptions-and-traps), [ASSEMBLY.md §5.3](../ASSEMBLY.md#53-exceptions), [ERRORS.md](../ERRORS.md#runtime-traps), [REGISTER_VS_STACK.md](../REGISTER_VS_STACK.md#calls-and-frames) |

Previous: [5. Memory and garbage collection](05-memory-and-gc.md) · Next: [7. Processes and the scheduler](07-processes.md)
