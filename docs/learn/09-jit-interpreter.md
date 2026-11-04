# 9. JIT 2: running with the interpreter

A JIT that compiled everything, perfectly, would never need the interpreter
again. Real ones do not: some instructions are rare, or awkward to compile,
or have a slow case that is not worth writing in machine code. MLWSVM's JIT
runs in *mixed mode*. The machine code and the interpreter run the same
program on the same data, and control passes between them in both
directions, at any block, many thousands of times a second. This chapter
explains how that can work and stay exact: blocks, the instruction count,
entering and leaving the machine code, side exits, and stepping.

## One state, two ways to run it

Everything about a running process lives in memory the interpreter defines
(chapters 3 and 6): the register windows, the frames, the heap, the slice's
count. The machine code reads and writes exactly that state. When it stops,
it has put everything back where the interpreter expects it, and when it
starts, it reads it from there.

This is the one rule that makes mixed mode possible: at every instruction
where control can change hands, the state is the interpreter's state. In
between, the machine code may keep values in machine registers, but it must
write them back before it lets the interpreter go on. The rest of this
chapter is about where those hand-over points are and what happens at each.

V8, HotSpot and LuaJIT all have the same problem in a harder form, since
their optimized code keeps state the interpreter would not recognise (values
in registers, objects never allocated). Going back to the interpreter from
optimized code is called *deoptimization* there, and it rebuilds the
interpreter's frames from a map the compiler wrote. MLWSVM's JIT keeps its
state close to the interpreter's, so going back is much simpler.

## Blocks

The JIT splits each function into *basic blocks*: runs of instructions that
are entered only at the top and left only at the bottom. The header comment
of [src/vm/jit.inc](../../src/vm/jit.inc) defines them:

```c
 * Blocks. A function's compiled instructions fall into basic blocks: one
 * starts at the first instruction, at a compiled branch's target, and after a
 * block's end (a branch, call or return) or an instruction the interpreter
 * runs.
```

A block is the unit for everything that follows. Values stay in machine
registers inside it, and are written back at its end. It is counted against
the time slice at its start. And it is where the interpreter may go into the
machine code.

In the loop of chapter 8, the blocks were the two `mov`s, then the loop
body (`add`, `inc`, `blt`), then the `ret`:

```asm
    mov I1, 0                 ; block 1
    mov I2, 0
1:  add I1, I1, I2            ; block 2: a branch's target
    inc I2
    blt I2, n, 1b             ;   a branch ends it
    ret I1                    ; block 3
```

## Counting instructions exactly

The interpreter counts every instruction. A process runs for a *time slice*
of 2,000 instructions and then gives way to the others (chapter 7). Programs
can set quotas on instructions, and `--stats` reports how many ran. For all
of that to mean the same thing with the JIT on, the machine code must count
exactly as the interpreter does, and stop exactly where the interpreter
would have stopped.

Counting each instruction would cost an instruction each. Instead, each
block's code begins with a *header* that subtracts the whole block's length
at once. From the dump in chapter 8:

```text
  84: subs  w23, w23, #0x3          ; the loop's block: 3 instructions
  88: b.lo  ...                     ; fewer than 3 left: stop
```

`w23` holds what is left of the slice. If the subtraction goes below zero,
the block does not fit, and the branch goes to a stub that adds the count
back and stops *before* the block's first instruction, for the interpreter.
The interpreter then runs those instructions one at a time, counting each,
and the slice ends where it would have ended without the JIT. So
`Process.insns`, quotas and preemption are exactly the interpreter's. Tests
depend on that: the same program reports the same counts in every mode
(chapter 12).

## Going in

The interpreter enters the machine code through a special opcode. When a
function is compiled, the first instruction of each block where entering
pays gets `Q_JIT`. Its handler, also in `jit.inc`:

```c
static QCC int h_Q_JIT(QPARAMS) {
    uint32_t pc = (uint32_t)(in - QCODE);
    const Function *fn = QFRAME->fn;
    if (c->hooked) MUSTTAIL return qhandlers[fn->jit_xop[pc]](QARGS); /* a tool sees each instruction */
    Jit *jt = c->vm->jit;
    void **ent = __atomic_load_n(&fn->jit_entry, __ATOMIC_ACQUIRE);
    JitExit r = jt->enter(c, left + 1, ent[pc]);
    ...
    left = (uint32_t)r.left;
    const Insn *nx = r.in;
    if (!nx) return c->jstatus; /* a stepped instruction ended the slice */
    ctx_load(c); /* the machine code kept the frame and windows to itself */
    ...
    MUSTTAIL return qhandlers[x](QARGSN(nx));
}
```

It calls `jt->enter` with the slice's count and the machine code's address
for this instruction (`jit_entry[pc]`). That comes back with the
instruction to go on at and the count left, and the handler goes on there
as `NEXT` would. While a debugger, tracer or profiler is attached
(`c->hooked`), it never enters: those tools need to see every instruction.

`jt->enter` is a small piece of machine code made once per VM, a
*trampoline* between C and the compiled code. From the dump of chapter 8
(arm64, comments added):

```text
   0: stp   x29, x30, [sp, #-0x60]!   ; save the registers C expects kept
   8: stp   x19, x20, [sp, #0x10]
   ...
  1c: mov   x19, x0                   ; the Ctx
  20: ldr   x20, [x19, #0x10]         ; the register window
  24: ldr   x24, [x19, #0x8]          ; the current frame
  28: ldr   x25, [x19]                ; the process
  2c: mov   x26, ...                  ; the code memory's start
  38: mov   w23, w1                   ; the slice's count
  3c: br    x2                        ; into the block
  40: mov   w1, w23                   ; the exit: the count back,
  44: ldp   x27, x28, [sp, #0x50]     ;   the saved registers back,
  ...
  5c: ret                             ;   and back to C (x0: where to go on)
```

On the way in it saves the registers the C calling convention requires a
function to preserve, and loads the VM's state into fixed machine registers.
Every exit comes back through the address 0x40, with the instruction to go
on at in `x0`. The same idea exists in every mixed-mode VM, often called
the entry and exit stubs.

### Where entering pays

An entry and exit cost about as much as a few interpreted instructions. If
the machine code could only run two instructions before it had to stop
again, entering would be slower than not. So not every block gets `Q_JIT`.
`jit_worth_entering` follows the code from the block and asks whether it
reaches a loop or a call, or runs long, before the next instruction the
interpreter must run:

```c
/* Whether entering the machine code at leader L pays: the code it can run
 * from there before it must stop for the interpreter has a loop or a call,
 * or is long, and steps few instructions. Each entry and exit, and each step,
 * costs about as much as a few instructions in the interpreter, so a short
 * run between two instructions the interpreter runs would be slower than the
 * interpreter alone. */
#define JIT_RUN_MIN 16
#define JIT_STEP_COST 32
```

This was measured, not guessed: entering a one-instruction `ret` function
made compiled Lox's `instantiation` 13% slower than interpreting it.

A block with no `Q_JIT` is still compiled. The machine code reaches it from
the block before, and calls from machine code go straight to it. Only the
interpreter does not go in there.

## Coming out: side exits

Most instructions have a common case and rare ones. An element read
`mov I1, xs[i]` usually finds a list and an index inside it, but `xs` might
be null, or not a list, or `i` might be past the end. The interpreter's
handler checks each and hands anything unusual to the generic
instruction, which raises the right error or handles the unusual type
(chapter 4).

The machine code makes the same checks, and on any failure it takes a *side
exit*: it stops before the instruction and lets the interpreter run it. The
interpreter then does whatever it would have done: raise `E_BOUNDS`, or
handle the case the machine code does not. The machine code never has to
know how to raise an error.

Here is `get`, compiled (arm64, from `MLWSVM_JIT_DUMP`, comments added):

```asm
.func get(ref xs, i64 i) -> i64
    mov I1, xs[i]             ; checked: an index past the end is a side exit
    ret I1
.endf
```

```text
   0: subs  w23, w23, #0x2          ; header
   4: b.lo  0x1a4
   8: ldr   x0, [x20]               ; i
   c: ldr   x9, [x20, #0x10]        ; xs
  10: cbz   x9, 0xf4                ; null: side exit
  14: ldrb  w10, [x9]               ; the object's kind
  18: cmp   w10, #0x5               ; a list?
  1c: b.ne  0x48                    ;   no: an array, a string... (code for each kind follows)
  20: ldr   w10, [x9, #0x10]        ; its length
  24: cmp   x0, x10
  28: b.hs  0x108                   ; i past the end (or negative): side exit
  2c: ldr   x10, [x9, #0x20]        ; the element's type tag
  30: ldrb  w11, [x10, x0]
  34: cmp   w11, #0x6               ; an i64?
  38: b.ne  0x11c                   ;   no: another stub
  3c: ldr   x10, [x9, #0x18]
  40: ldr   x17, [x10, x0, lsl #3]  ; the element
  ...
```

and the stubs after the function:

```text
  f4: add   w23, w23, #0x2          ; the block's count back
  f8: mov   x0, #0x7ef0             ; x0: the instruction to stop at (the mov)
  fc: movk  x0, #0xbb, lsl #16
 100: movk  x0, #0x1, lsl #32
 104: b     <exit>                  ; out through the trampoline
 108: add   w23, w23, #0x2          ; (the same, for the bounds check)
 ...
 11c: cmp   w11, #0x0               ; an element never written:
 120: b.ne  0x12c
 124: mov   x17, #0x0               ;   it reads as zero; back to the code
 128: b     0x44
 12c: add   w23, w23, #0x2          ; any other type: side exit
 ...
```

Three things to notice. The checks are a compare and a branch each, and the
branches are almost never taken, which processors predict well. The stubs
are placed after the function's code, so the common path runs straight
through without jumping over them. And each stub gives back the block's
count before stopping, since the block did not finish: the interpreter will
count the instructions as it runs them.

If the block had changed registers that were still only in machine
registers, the stub would first store them to the window (`jside_stores`).
The rule from the start of the chapter holds: before the interpreter runs,
its state is complete.

## Stepping

Some instructions are not compiled at all: printing (`ncall` of a native),
`spawn`, rare conversions, anything the JIT has no form for. Ending the
block at each of them, and re-entering after, would cost an exit and an
entry every time. Instead, the machine code *steps* them: it calls the
interpreter's own handler for that one instruction, from the machine code,
and goes on in machine code afterwards.

The stepping code, emitted in the main loop of `jit_compile`:

```c
if (jit_steps(in, jx[pc])) { /* a block of its own: counted, then run by its handler */
    jm_subs32i(b, JR_LEFT, JR_LEFT, 1);
    jbranch(j, FX_BCOND, JC_LO, 0, false, TO_UNDO, pc);
    jm_mov(b, JR_A0, JR_CTX);
    jm_movi(b, JR_A1, (uint64_t)(uintptr_t)in);
    jm_mov32(b, JR_A2, JR_LEFT);
    jm_ccall(b, (const void *)jit_step);
    ...
}
```

`jit_step` (also in `jit.inc`) runs the handler with no slice left, so the
handler does its instruction and stops, as it would at a slice's end. Then
it looks at where the frame is now (the next instruction, a branch's target,
or a callee) and returns that place's machine code. A stepped instruction is
a block of its own, so the registers are in the window when its handler
reads them. A step costs only about 5 ns more than the handler alone.

## Worked example

This program has all three: a loop the JIT compiles and stays in, a native
call it steps, and an element read whose check fails in machine code.

```asm
.include "std/io.inc"

.func mean(ref xs) -> f64
    mov I1, 0
    mov I2, 0
    len I3, xs
1:  mov I4, xs[I2]
    add I1, I4
    inc I2
    blt I2, I3, 1b
    cvt.f64.i64 F1, I1
    cvt.f64.i64 F2, I3
    div.f64 F1, F1, F2
    ret F1
.endf

.func get(ref xs, i64 i) -> i64
    mov I1, xs[i]
    ret I1
.endf

.func main
    lnew R0
    mov I0, 0
1:  lpush R0, I0              ; the list 0..99999
    inc I0
    blt I0, 100000, 1b
    mov I5, 0
2:  call mean (R0) -> (F0)    ; 20 times
    inc I5
    blt I5, 20, 2b
    PRINTLN_F F0
    mov I5, 0
3:  call get (R0, I5) -> (I2) ; 5000 good reads: get is compiled
    inc I5
    blt I5, 5000, 3b
.try 9f
    call get (R0, 100000) -> (I2)   ; one past the end
.endtry
    ret 0
9:  catch R5
    PRINTLN_S R5->Error.code
    ret 0
.endf
```

```sh
$ ./mlwsvm run --jit on --jit-report - mixed.masm
49999.5
E_BOUNDS
mlwsvm jit (arm64): a function compiled after 1000 calls or loop iterations
  functions compiled  3
  instructions        27 of 34 compiled (79.4%)
  loops               2 with their registers in machine registers
  machine code        3472 bytes, in 0.04 ms
  entries             5162, 8298895 instructions run in machine code (1607.7 per entry)
  stepped             2 instructions
  most stepped (forms the JIT does not compile):
               2  ncall (form 66)
  most common stops (where the interpreter took over, a time slice's end included):
            4000  mov (form 212)
            1001  call (form 202)
             149  lpush (form 279)
              12  inc (form 197)
```

The output is the interpreter's, `E_BOUNDS` included: the failed bounds
check in `get`'s machine code was a side exit, and the interpreter raised
the error. Reading the report:

* **Stepped.** The two `ncall`s are the `PRINTLN` macros' calls to the
  print natives, run by their handlers from `main`'s machine code.
* **The 4,000 stops at `mov`.** These are time slices. `mean`'s loop runs 8
  million instructions, and every 2,000 of them the slice ends at a block's
  header. The interpreter takes over at the loop's first instruction, the
  `mov`, counts out what is left, lets the other processes run, and enters
  the machine code again.
* **The 1,001 stops at `call`.** `main` was compiled during its first loop,
  long before `get` had been called 1,000 times. A call from machine code to
  a function that has no machine code yet stops before the call, and the
  interpreter makes it. Once `get` is compiled, its calls stay in machine
  code.

Run it with `--jit off`, and with `--jit all` (every block entered, nothing
left to the interpreter but what must be), and the output and `--stats`'
instruction counts are the same:

```sh
$ for m in off on all; do ./mlwsvm run --jit $m --stats mixed.masm 2>&1 | grep instructions:; done
instructions:     8325212
instructions:     8325212
instructions:     8325212
```

That sameness is what chapter 11's fuzzer checks on random programs.

## Where to look

| What | Where |
|---|---|
| Blocks, and which start where | the main loop of `jit_compile`, `lead[]`, in [src/vm/jit.inc](../../src/vm/jit.inc) |
| The header and its stop | `jm_subs32i(b, JR_LEFT, ...)` in that loop; the `TO_UNDO` stubs |
| Going in and out | `h_Q_JIT`; the trampoline (`jit_enter`, made once per VM) in [src/vm/jit_a64.inc](../../src/vm/jit_a64.inc) and [src/vm/jit_x64.inc](../../src/vm/jit_x64.inc) |
| Where entering pays | `jit_worth_entering` |
| Side exits | `jside`, `SIDE_IF`, `jside_record`, `jside_stores`, and the stubs at the end of `jit_compile` |
| Stepping | `jit_steps`, `jit_step` |
| Reference | [JIT.md: how it runs with the interpreter](../JIT.md#how-it-runs-with-the-interpreter), [DESIGN.md §8.8](../DESIGN.md#88-the-jit) |

Previous: [8. JIT 1: what a JIT is](08-jit-what.md) · Next: [10. JIT 3: making it fast](10-jit-fast.md)
