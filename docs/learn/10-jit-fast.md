# 10. JIT 3: making it fast

Translating each instruction into machine code removes decoding and
dispatch (chapter 8). That alone helps less than you might hope, because
the translated code still loads every operand from the register window and
stores every result back, exactly as the interpreter did. Most of what makes
MLWSVM's machine code fast is about *not* doing that: keeping VM registers in
machine registers, remembering what has been checked, and making calls
cheap. This chapter goes through those techniques in the order they matter,
each with the measurement that justified it.

## The register cache

Inside a block, the JIT keeps the VM's I and F registers in machine
registers. The first time an instruction reads `I4`, it is loaded from the
window into a free machine register; after that, instructions use the
machine register. A write marks it *dirty*. At the block's end, the dirty
ones are stored back to the window, so that the next block, or the
interpreter, finds them there (chapter 9's rule).

This is a *register cache*, the simplest kind of register allocation:
local to one block, decided as the code is emitted, with no analysis
beforehand. In [src/vm/jit.inc](../../src/vm/jit.inc):

```c
/* The machine register holding register r, loaded if it is not yet. */
static int jc_get(JComp *j, int bank, int r) {
    JCache *c = &j->jc;
    ...
    int s = c->of[bank][r];
    ...
    if (s < 0) {
        s = jc_alloc(j, bank);
        if (bank) jm_fld(&j->b, jc_freg[s], JR_WIN, jwoff(j, BANK_F, (unsigned)r));
        else jm_ld(&j->b, jc_ireg[s], JR_WIN, jwoff(j, BANK_I, (unsigned)r));
        c->of[bank][r] = (int8_t)s;
        c->vm[bank][s] = (int8_t)r;
    }
    c->pin[bank] |= 1u << s;
    return jc_mreg(bank, s);
}
```

`of[bank][r]` says which slot holds VM register `r`, and `vm[bank][s]` which
VM register slot `s` holds. `jc_def` is the same for a register about to be
written, and marks it dirty. When all slots are taken, `jc_alloc` evicts one
that the current instruction does not use (`pin`), storing it first if it is
dirty. At the end of a block, `jc_flush` stores the dirty ones and forgets
them all:

```c
static void jc_flush(JComp *j) {
    JCache *c = &j->jc;
    if (j->loop || c->record) return;
    for (int bank = 0; bank < 2; bank++)
        for (int s = 0; s < jc_nslots(bank); s++)
            if (c->vm[bank][s] >= 0 && c->dirty[bank][s]) jc_store(j, bank, s, c->vm[bank][s]), c->dirty[bank][s] = false;
    if (!j->keep) jc_reset(c);
}
```

How many slots there are depends on the processor. arm64 has 31 general
registers, so the cache has 16 for integers and 16 for floats. x86-64 has
16, and after the ones the JIT needs for itself (the `Ctx`, the window, the
slice's count, the frame, scratch registers for the forms) only 5 are left
for integers, with 13 for floats in the `xmm` registers. A sixth was tried
on the i7 and the Ryzen by moving the slice's count to memory to free its
register: most programs got 3–40% slower, because every block's header then
went through memory.

R registers (references to heap objects) are a special case. The garbage
collector finds live objects by scanning the register windows (chapter 5),
so a reference must be in the window whenever a collection could happen. The
cache therefore *writes through* R registers: it stores them to the window
as soon as they are set, and may also keep a copy in a free machine register
for reading. On x86-64 even that is off: with so few slots, keeping R
registers cost the i7 (whose register-to-register moves are not free) up to
7%.

## Remembering what has been checked

A compiled field read checks that the object is not null and is a struct of
the right type, then loads the field. A second field of the same object, a
few instructions later, would check the same things again. The cache also
keeps *facts* about R registers: what the block has checked of each one's
object since the register was last set.

```c
/* ... rk[r]:
 * what the block has checked of R r's object since r was last set (kind 0:
 * nothing; OBJ_STRUCT with its def; OBJ_LIST), so that the checks are not
 * repeated. Both are forgotten at every block's start. */
typedef struct {
    uint8_t kind;
    bool young; /* made in this block (new, lnew): no collection runs before it is left */
    const void *def;
} JRKnown;
```

A struct's type cannot change while the register holds the same reference,
so the fact stays true until the register is written. (`young` is used by the
write barrier, chapter 5: an object made in this block cannot be in the old
generation yet, so storing into it needs no barrier.) Facts gained little on
the M5, up to 5%, since the repeated checks' branches were always predicted
correctly, but they cost nothing to keep, and they matter more where checks
are expensive, as the next section shows.

## Blocks that only fall through

Both the cache and the facts are forgotten at every block's start, because
a block can be reached from anywhere, including the interpreter. But many
blocks cannot. A conditional branch that ends one block either jumps or
falls into the next one. If nothing else jumps to that next block, it is
reached only from the block before, and everything that block knew still
holds.

MLWSVM calls these *fallin* blocks and compiles them as continuations of the
block before: the branch writes back the dirty registers (they must be in
the window if the branch is taken), but marks them clean instead of
forgetting them, and the block after goes on using the machine registers and
the facts. Such a block gets no entry from the interpreter, since its code
assumes the cache. From the main loop of `jit_compile`:

```c
fallin[pc] = lead[pc] && !target[pc] && jit_handles(jx[pc - 1]) && !jit_steps(&f->code[pc - 1], jx[pc - 1]) &&
             jit_branches(jx[pc - 1]) && jx[pc - 1] != Q_JMP && jit_handles(jx[pc]) &&
             !jit_steps(&f->code[pc], jx[pc]) && loop_of[pc] < 0 && loop_of[pc - 1] < 0;
```

This came from a profile. Compiled Lua checks the type of every value before
using it, so its blocks are one to three instructions long, each ending in a
check's branch. In compiled Lua's `nbody`, 46% of the machine code's time was
moving values to and from the window, and 1% was the arithmetic. Keeping the
registers across fallin blocks, together with cheaper reads of globals, made
`nbody` 6–7% faster and Lox's `zoo` 4–13% faster on the three machines.

The obvious refinement was measured and not taken. Writing the registers
back only on the branch's taken path, in a stub, saved more where the branch
falls through, but where the branch is usually *taken* it added two jumps:
compiled Lua's `loop` got 7% slower on the Ryzen.

## Loops

A loop is where a program spends its time, and a loop's blocks are entered
from the block before *and* from the branch back, so fallin does not apply.
For loops, the JIT goes further: if all the I and F registers the loop uses
fit in the cache, they are loaded once on the way in and stay in machine
registers for every iteration. They are written back on every way out: a
branch out of the loop, a side exit, the slice's end.

That was the loop in chapter 8:

```text
  78: ldr   x0, [x20]               ; on the way in: n, I1 and I2
  7c: ldr   x1, [x20, #0x8]
  80: ldr   x2, [x20, #0x10]
  84: subs  w23, w23, #0x3          ; each iteration: no load, no store
  88: b.lo  ...
  8c: add   x1, x1, x2
  90: add   x2, x2, #0x1
  94: cmp   x2, x0
  98: b.lt  0x84
  9c: str   x1, [x20, #0x8]         ; on the way out
  a0: str   x2, [x20, #0x10]
```

The loop is found from its branch back (`jit_loops`): the instructions from
its head that reach the branch back. A slow path inside its range that
leaves for good (an error's call, a return) is not part of it. An
instruction inside that cannot keep the registers (a call, a stepped
instruction) is a *hole*: the registers are written back before it and
loaded again after. Each hole costs a store and a load of every loop
register, so a loop with more than two holes on arm64, or any on x86-64,
keeps its registers in the window instead. Without that limit, compiled Lox
got 7–9% slower.

Loop registers also fixed a surprising effect. Before them, the `loop`
benchmark took either 39 or 23 ms depending on 32 bytes of padding in front
of the code: the value stored to the window at the end of one iteration was
loaded at the start of the next, and whether the processor could forward
the store to the load depended on alignment. With the value in a register
all along, there is nothing to forward.

## Two words at once

The Lox and Lua compilers keep a dynamically typed value in two words of a
list: Lox as a number word and a reference word, Lua as a reference (its
type) and a number. Reading a value is two element loads at `i` and `i+1`,
and each checked that the object was a list and the index was in bounds.

The JIT recognises the pair (two loads of different banks from the same list
and index register, at `i` and `i+1`) and compiles it as one form: one check
that the object is a list, then both loads (`jit_pair`). Anything that is not
a list stops before the first load, which is exact, because an array's
elements all have one type and could not give both words. Afterwards the
block knows the object is a list. A field read of compiled Lua went from
about 100 arm64 instructions to 69.

## Calls and returns

A call in machine code does what the interpreter's call does: it pushes a
frame, clears the parts of the callee's window that must start empty, copies
the arguments, and goes to the callee's machine code (chapter 6 describes
frames and windows). Three choices make it cheap.

* **Real calls and returns.** The machine code uses the processor's own call
  and return instructions (`bl` and `ret` on arm64, `call` and `ret` on
  x86-64), paired one to one. Processors predict where a `ret` goes with a
  small stack of return addresses, which only works if calls and returns
  match. The code after a call, its *continuation*, takes the caller's window
  back and stores the results. A return goes to the continuation of the call
  that made its frame, whose address is kept in the call instruction itself
  (`Insn.target`).
* **Results in registers.** A return can only reach a continuation from
  compiled code, so the two can agree on where results go. The first four
  are left in machine registers rather than stored in memory and loaded
  again (`JRES_N` in `jit.inc`):

  ```c
  /* A return to a continuation leaves its first JRES_N values in the cache's
   * first integer registers (free at a return, whose block has written its
   * registers back), the rest in Ctx.retv. Only a compiled return (jit_ret)
   * goes to a continuation. */
  #define JRES_N (JC_NI < 4 ? JC_NI : 4)
  ```

* **Small frames.** The frame record's pc and two flags are written as one
  64-bit store, the callee's window is cleared two words at a time (`stp xzr,
  xzr` on arm64, 16-byte stores on x86-64), and a function that never uses
  the data stack does not save or restore its pointers at all.

Counted with callgrind (chapter 11), a call was 64 x86-64 instructions and a
return 25–27, about half of compiled Erlang's `binarytrees`. The changes
above made call-heavy programs 3–19% faster with the JIT. Against the
interpreter, `bench/fib` now takes 61–66% less time.

## A closure call, compiled for its callee

A direct call names its callee, so the JIT knows exactly what it calls. A
*closure call*, `calli R0 (...)`, calls whatever function value is in a
register: a method, a callback, a function passed in. In general the call
must check, at run time, that the value is a closure and that its function's
parameters match the arguments, and only then push the frame. That generic
path is a C helper, and it was 25–48% of compiled Lua's `methods` and Lox's
`properties`.

In practice, most closure call sites call the same function every time,
even when the closure values differ (many objects, one method). This is the
observation behind *inline caches* (chapter 4), and HotSpot and V8 compile
calls for the target they have seen. MLWSVM does the same. The interpreter
records, in the call site's plan, the function it called last
(`QPlan.seen`). When the caller is compiled, a call site that has seen a
function, and whose arguments and results suit it, is compiled for it. From
`jit_calli`:

```c
if (fn) {
    int clo = jc_ireg[2];
    jm_ld(b, clo, JR_WIN, (int64_t)in->ops[0].pad * 8);
    other[0] = jm_fwd_cbz(b, clo, false);
    jm_ld8(b, JR_S0, clo, (int64_t)offsetof(Obj, kind));
    jm_cmpi32(b, JR_S0, OBJ_CLOSURE);
    other[1] = jm_fwd(b, JC_NE);
    jm_ld(b, JR_S0, clo, (int64_t)offsetof(ObjClosure, fn));
    jm_movi(b, JR_S1, (uint64_t)(uintptr_t)fn);
    jm_cmp(b, JR_S0, JR_S1);
    other[2] = jm_fwd(b, JC_NE);
    jit_push_frame(j, in, pc, fn, pl->argc, pl->slot, dst, kw, clo);
    ...
}
```

Three checks, each a forward branch to the general path (`other[0..2]`):
the value is not null, it is a closure, and its function is the one seen.
Then the frame is pushed exactly as for a direct call. If any check fails, the general path
follows, and both reach the same machine call, so the return finds one
continuation either way. This is *speculation*: compiling for what has
happened so far, with a check that falls back if it stops being true. It is
the only speculation MLWSVM's JIT does. Chapter 11 measures a broader kind
and explains why it was not done.

## Worked example: a closure in a loop

```asm
.include "std/io.inc"

; A closure: adds its captured value.
.func adder(i64 x) -> i64
    mov I1, UP[0]
    add I1, x
    ret I1
.endf

.func main
    closure adder (3) -> (R0)
    mov I1, 0
    mov I2, 0
1:  calli R0 (I1) -> (I1)     ; ten million closure calls
    inc I2
    blt I2, 10000000, 1b
    PRINTLN_I I1
    ret 0
.endf
```

Three ways to run it: interpreted; with `--jit all`, which compiles every
function before anything has run, so no call site has seen a callee; and
with `--jit on`, which compiles `main` once its loop is hot, after the call
site has seen `adder`.

| | M5 | i7 |
|---|---|---|
| `--jit off` (interpreted) | 85 ms | 174 ms |
| `--jit all` (compiled, the general closure call) | 80 ms | 147 ms |
| `--jit on` (compiled for `adder`) | 28 ms | 53 ms |

```sh
$ ./mlwsvm run --jit on --jit-report - clo.masm
30000000
mlwsvm jit (arm64): a function compiled after 1000 calls or loop iterations
  functions compiled  2
  ...
  closure calls       1 of 1 compiled for the function they called last
```

Compiling the loop and the closure's body, but not the call, gains 6–16%:
the general call's checks and its trip through C are most of the time. Once
the call is compiled for `adder`, the loop runs three times as fast. On the
compilers' benchmarks, which call methods everywhere, compiling closure calls
for their callee is what most of the gain of `bench/closures` (84–86% less
time than interpreted), Lua's `methods` and the Lox programs is.

## What it does not do

MLWSVM's JIT stops short of the big techniques of optimizing JITs, each for a
measured reason:

* **Inlining**, replacing a call with the callee's body. Few calls in the
  compilers' benchmarks go to small functions that call nothing, and an
  inlined body would need a frame built at every way out of it (a side exit,
  a trap), since the collector and the unwinder find registers through
  frames. The optimizer's own inliner, run before the JIT, gained 1–2% on the
  programs where it applied.
* **Type speculation**, compiling arithmetic for the types a value has had,
  as LuaJIT and V8 do. Chapter 11 shows how it was measured: removing the
  checks it could remove would save 1–8% of the machine instructions.
* **Register allocation across blocks**, beyond fallin blocks and loops.
  Measured on the places where slow paths rejoin, it would save 0.1–1.1% of
  the instructions in loads.

[ROADMAP.md](../ROADMAP.md#4-performance-when-measurements-call-for-it) has
these and the other ideas measured and set aside, with their numbers.

## Where to look

| What | Where |
|---|---|
| The register cache | `JCache`, `jc_get`, `jc_def`, `jc_alloc`, `jc_flush`, `jld_r`, `jst_r` in [src/vm/jit.inc](../../src/vm/jit.inc) |
| Facts about R registers | `JRKnown`, `c->rk` |
| Fallin blocks | `fallin[]` in `jit_compile`; `j->keep` |
| Loops | `jit_loops`, `jit_loop_load`, `jit_loop_load_as`, `JM_LOOP_HOLES` |
| Two-word values | `jit_is_pair`, `jit_pair` |
| Calls and returns | `jit_call`, `jit_push_frame`, `jit_cont`, `jit_ret`, `jit_tail_frame`, `JRES_N` |
| Closure calls | `jit_spec_callee`, `jit_calli`, `QPlan.seen` |
| How many registers | `JC_NI`, `JC_NF`, `jc_ireg`, `jc_freg` in [src/vm/jit_a64.inc](../../src/vm/jit_a64.inc) and [src/vm/jit_x64.inc](../../src/vm/jit_x64.inc) |
| Reference | [JIT.md: how it runs with the interpreter](../JIT.md#how-it-runs-with-the-interpreter), [PERFORMANCE.md: the JIT](../PERFORMANCE.md#the-jit) |

Previous: [9. JIT 2: running with the interpreter](09-jit-interpreter.md) · Next: [11. JIT 4: measuring and testing](11-jit-measuring.md)
