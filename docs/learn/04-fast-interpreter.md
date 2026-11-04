# 4. Making an interpreter fast

Chapter 3 made the dispatch cheap: on arm64, fifteen machine instructions
take an `add` from one handler to the next. What is left is the work inside the
handlers and the number of instructions a program needs. This chapter covers
the techniques that cut both: *quickening* (rewriting an instruction into a
form specialised for its operands), *superinstructions* (one instruction
where there were two), *inline caches* (remembering the answer to a lookup
at the place that asked), and cheaper calls, from argument plans made at load
time to register windows that are only partly cleared. Each comes with what
it measured, because each was kept only if it measured faster. The worked
example uses the profiler to find an instruction on the slow path, and moves
it off.

## Why a general instruction is slow

MLWSVM's `add` takes any number type (`i8` to `u64`, `f32`, `f64`, big
integers and decimals), a pointer plus offsets, two or more sources, and any
operand: a register, a constant, a word in memory, an array element, a
struct field. The general path (chapter 3) asks, every time it runs, which
case it is in: a `switch` on the opcode, one on each operand's mode, one on
each register's bank, a canonicalisation to the type's width, a `switch` in
`arith_binop`, a bank check on the write. But for most instructions every
answer is the same each time, and known at load time: `add I0, I0, I1` is
always an `i64` add of three `I` registers. An interpreter can ask once and
keep the answer. That is quickening.

## Quickening

The idea is old. Early Java VMs rewrote a `getfield` into a `getfield_quick`
once the field had been resolved, so later executions skipped the lookup.
CPython 3.11's *specialising adaptive interpreter* (PEP 659) rewrites an
instruction as it runs: a `BINARY_OP` that keeps seeing two integers becomes
`BINARY_OP_ADD_INT`, and is rewritten back if its guess stops holding.
Brunthaler's *Inline Caching Meets Quickening* (ECOOP 2010) is the standard
account.

MLWSVM quickens once, at load time, after the verifier has checked the code,
and from static facts only: the opcode, the type suffix, and each operand's
mode and register bank. It never looks at a value. A form chosen this way is
never wrong, so nothing has to be undone; a form that can meet something
unexpected at run time (a null object, another struct type) checks for it and
hands that one execution to the general path. `interp_quicken_function`
([src/vm/interp.c](../../src/vm/interp.c)) runs `interp_quicken` on each
instruction as the loader finishes a function. For the integer arithmetic it
reads:

```c
#define X(N, op_)                                                              \
    case OP_##N:                                                               \
        if (i64 && n == 3 && IREG(0) && IREG(1) && IREG(2)) in->xop = Q_##N##_III; \
        else if (i64 && n == 3 && IREG(0) && IREG(1) && KINT(2)) in->xop = Q_##N##_IIK; \
        else if (i64 && n == 2 && IREG(0) && IREG(1)) in->xop = Q_##N##_II;   \
        else if (i64 && n == 2 && IREG(0) && KINT(1)) in->xop = Q_##N##_IK;   \
        else goto float_arith;                                                 \
        break;
        QARITH_OPS(X)
#undef X
```

`IREG(k)` tests that operand `k` is an `I` register, `KINT(k)` that it is an
integer immediate, and `i64` that the type is `i64` or `u64`. Anything else
falls through to the `f64` forms, and failing those keeps `xop = op`, the
general path. Once a form is chosen, the operands its handler needs are
copied into the 32-byte `Insn` itself:

```c
    if (in->xop >= Q__FIRST) /* the compact operand copy the quickened handlers read */
        for (int k = 0; k < n && k < 3; k++) {
            switch (o[k].mode) {
            case OM_REG: in->r[k] = (uint8_t)REG_IDX(o[k].reg); break;
            ...
            case OM_FIMM: in->k.f = o[k].f; break;
            ...
            case OM_LABEL: in->target = (uint32_t)((int64_t)o[k].rv.target - (int64_t)quicken_idx); break;
```

The forms are numbered after the real opcodes (`Q__FIRST = OP__COUNT`), so
one table indexed by `xop` holds both. There are about 210 of them for some
130 opcodes, named by their operands: `I`, `F` and `R` for a register of
that bank, `K` for a constant, `E` for an element.

| Form | Shape | Example |
|---|---|---|
| `Q_ADD_III` | `i64` add of three `I` registers | `add I0, I1, I2` |
| `Q_BLT_IK` | compare with a constant and branch | `blt I0, 100, loop` |
| `Q_DIV_IIK` | division by a constant other than 0 and -1 | `div I0, I1, 10` |
| `Q_LDF_F` | `f64` struct field into a register | `mov F0, R1->Point.x` |
| `Q_LDE_I` | array or list element by a register index | `mov I0, R1[I2]` |
| `Q_CALL_R` | direct call with register arguments | `call fib (I1) -> (I2)` |
| `Q_BNE_RG` | reference compared with a global | `bne.r R0, @cache, miss` |

A shape qualifies only when its general meaning is trivially the same: no
canonicalisation to a narrower width, and no trap that cannot be checked
cheaply ([DESIGN.md §8.2](../DESIGN.md#82-quickening)). That is why
`add.i32` has no form (its result must wrap at 32 bits), and why division
qualifies only by a constant that is neither 0 (a trap) nor -1
(`INT64_MIN / -1` overflows, which the general path defines).

Where a check is cheap, the form makes it and gives up when it fails. The
struct field load is the type check and one load (`Q_LDF_*` in
[src/vm/qforms.inc](../../src/vm/qforms.inc); `in->k.p` is the struct
definition the operand named):

```c
#define STRUCT_OF(reg) ((ObjStruct *)QR[reg])
#define IS_STRUCT(o) ((o) && (o)->h.kind == OBJ_STRUCT && (o)->def == in->k.p)
...
        QCASE(Q_LDF_##B) {                                                    \
            ObjStruct *so = STRUCT_OF(in->r[1]);                              \
            if (!IS_STRUCT(so)) GENERIC();                                 \
            QB(bank)[in->r[0]] = so->fields[in->target].w;                      \
            NEXT;                                                             \
        }                                                                     \
```

On a null, another kind of object or another struct type, `GENERIC()` runs
the general `mov`, which raises the error with its usual message. The real
opcode stays in `Insn.op`, so the disassembler, tracer, profiler and debugger
never see a quickened form. `tests/perf/quickened.masm` checks the
arithmetic, branch and call forms against the general path at their edges
(overflow, `INT64_MIN / -1`, unsigned compares), and
`tests/perf/quickened_slots.masm` every fallback of the field, element,
null-test and closure-call forms.

**Compact operands.** The copy into `r[]`, `target` and `k` is a technique of
its own. Without it a handler would follow `ops` to a 72-byte `Operand` per
operand. With `switch` dispatch the copy made no difference; with threaded
dispatch it saved 18% on `loop`, since once dispatch is cheap, the pointer
chase shows ([REGISTER_VS_STACK.md](../REGISTER_VS_STACK.md#what-variable-arity-costs)).

## Choosing the forms: the profiler

Which shapes deserve a form? Not the ones that look common, but the ones
programs run. `mlwsvm run --profile` counts every instruction exactly, and
two of its tables exist for this question
([TOOLING.md](../TOOLING.md#profiler---profile)). *opcodes* gives the
instruction mix and, for each opcode, the share of its executions that ran a
specialised handler ("fast"). *hottest instructions on the general path*
lists, disassembled, the instructions that ran most often without one.

The forms for compiled dynamically typed code were chosen from that list, run
on the output of the Lox, Lua and Erlang compilers in the `mlws-compilers`
project. Their effects, from
[PERFORMANCE.md](../PERFORMANCE.md#how-the-interpreter-is-made-fast) and
[ROADMAP.md](../ROADMAP.md):

| Forms added for | What they cover | Measured |
|---|---|---|
| Compiled Lox | constants as call arguments and `ret` values, reference constants and globals, reference compares, `isa`, `mget` by a constant key | the Lox benchmarks 1.3–3.0× faster |
| Compiled Lua | calls and returns with constants, closure tail calls, `new`, `lnew`, `jtab`, shifts, and more | compiled Lua's `methods` 26% less time |
| A Lua table constructor | `lpush` with constants among its values | `methods` 16% less |
| Compiled Erlang | `bitcast` between `I` and `F`, `typeof`, a tuple's tag test, direct tail calls | `binarytrees` and `nbody` 29–39% less, `sorting` 15–21% less |

A profile of real compiled code shows what a finished set looks like. This
Lox program calls a method 100,000 times:

```text
class Counter {
  init() { this.n = 0; }
  bump() { this.n = this.n + 1; }
}
var c = Counter();
for (var i = 0; i < 100000; i = i + 1) c.bump();
print c.n;
```

Compiled by the Lox compiler (0.5.0, `mlws-vm-lox -S`) and run with
`--profile`:

```text
opcodes (fast = the share run by a specialised handler, chosen at load time):
       %        insns   fast  opcode
   53.6%      1500082 100.0%  mov
   10.7%       300008 100.0%  bne
   10.7%       300001 100.0%  bnz
    7.1%       200001 100.0%  add
    3.6%       100008 100.0%  ret
...
    3.6%       100000 100.0%  calli
...
    0.0%            6  66.7%  mget
    0.0%            5   0.0%  mnew
```

Every hot instruction is fast. The `mget` line shows the share is per
execution: of its six runs, four were of a shape with a form. The general
path is left with the program's set-up, a handful of instructions.

## Superinstructions

Every instruction dispatched costs the dispatch, so two instructions that
always appear together can be merged into one: a *superinstruction*
(Proebsting's *superoperators*, 1995; Ertl and Gregg's work on Gforth). The
merged form saves a dispatch and often a register write and read between the
halves. *Crafting Interpreters* adds one to clox: `OP_INVOKE`, a property
lookup and a call in one instruction
([Methods and Initializers](https://craftinginterpreters.com/methods-and-initializers.html)).
The cost is more handlers, and pairs that the compiler must find.

MLWSVM puts its superinstructions in the instruction set, where a compiler
emits them, rather than combining instructions as it loads them:

* **Compare-and-branch.** `blt I0, I1, L` is a compare and a jump in one;
  [TARGETING.md](../TARGETING.md#4-control-flow) tells compilers to prefer it
  to `slt` and `bnz`.
* **Elements at an index plus a constant.** `R1[I2+1]` is an operand, so a
  dynamically typed value's second word is read without an `inc` first.
* **`bisa` and `bnisa`**: a struct type test and a branch.
* **A tuple's tag test.** `bne.r R0[0], "nil", L` compares a list element at
  a constant index with a constant and branches, as one quickened form
  (`Q_BNE_EKK`).

The middle two were chosen from counts of adjacent instruction pairs in
compiled Lua; rewriting its output to use them ran 3–10% fewer instructions
and took up to 8% less time on the three machines
([ROADMAP.md](../ROADMAP.md)). (The JIT fuses more, such as a value's two
loads: chapter 10.)

What one dispatch is worth can be measured directly, with
`bench/loop.masm`'s back branch written three ways. On the Apple M5 Pro,
best of seven:

| Back branch | Instructions run | Time |
|---|---|---|
| `bne I0, 30000000, 1b` (fused, quickened) | 180,000,006 | 84 ms |
| `sub I3, I0, 30000000` then `bnz I3, 1b` (both quickened) | 210,000,006 | 97 ms |
| `slt I3, I0, 30000000` then `bnz I3, 1b` (`slt` on the general path) | 210,000,006 | 286 ms |

One extra quickened instruction per iteration costs 16%. The third row is a
warning: an integer `slt` has no fast form (only the `f64` and reference
set-compares do), and one general-path instruction in seven makes the loop
3.4 times slower.

## Inline caches

A dynamically typed language cannot know, when compiling `obj.m()`, where `m`
is: it depends on `obj`'s class at run time, and a full lookup is a hash
table probe. But at any one call site the class is usually the same each
time. An *inline cache* (Deutsch and Schiffman's Smalltalk-80, 1984) stores
the last class seen and what the lookup found *at the call site*; the next
execution compares the class, and on a match skips the lookup. A
*polymorphic* inline cache keeps several. V8 and HotSpot both use them.

MLWSVM has no classes of its own, so its inline caches are of three kinds.

**Caches the compilers build.** The Lox and Lua compilers keep their caches
in the module's globals, and the VM's job is to make each step a fast form.
Here is Lox's `bump() { this.n = this.n + 1; }` reading `this.n`, as the Lox
compiler emits it (in `L1$bump`): the instance's shape, compared with the
shape the cache saw last (`@fs$0`), then the field's word from the cache
(`@fw$0`), then the value's two words:

```asm
    mov.r R25, R0->Inst.shape
    bne.r R25, @fs$0, L3          ; a miss: the second entry, then a lookup
    mov I1, @fw$0
L2:
    mov.r R25, R0->Inst.slots
    mov.f64 F2, R25[I1]
    mov.r R2, R25[I1+1]
```

Six instructions, every one a quickened form: `Q_LDF_R`, `Q_BNE_RG`,
`Q_LDG_I`, `Q_LDF_R`, `Q_LDE_F`, `Q_LDED_R`. A method call site works the
same way, with the class and the method's closure in two globals and a
`calli` of the closure. Before these shapes had forms, the cache checks and
field reads took the general path, which is much of why the Lox property and
method benchmarks gained most from the Lox forms
([PERFORMANCE.md](../PERFORMANCE.md#compiled-lox)).

**Call plans: a cache made at load time.** A quickened call still has to
copy each argument from the caller's register window to the callee's. The
operands say where, but reading them means a 72-byte `Operand` each. So
`quicken_plan` gives each quickened call and return a *plan*, stored just
after its operands, which says the same thing in bytes (`QPlan` in
[src/vm/interp.c](../../src/vm/interp.c)):

```c
typedef struct {
    Function *fn;   /* a direct call's callee */
    Function *seen; /* a closure call's last callee: the JIT's guess at the next */
    uint8_t argc, nres;
    uint8_t slot[];
} QPlan;
```

For `fib`'s `call fib (I1) -> (I2)` the plan is the callee, one argument and
one result, and `slot = {1, 0, 2}`: the argument is word 1 of the caller's
window, the parameter word 0 of the callee's, the result word 2 of the
caller's. `Q_CALL_R` then copies each argument with one load and one store:

```c
            for (int k = 0; k < argc; k++) memcpy(&nw[pl->slot[argc + k]], &ow[pl->slot[k]], 8);
```

This is CPython's idea of a cache stored next to the instruction, but since
it is computed from verified, static facts, it never needs invalidating.

**The callee seen last.** A closure call (`calli`) cannot know its callee at
load time. Its quickened form checks the closure's function against the call
(the arity, each argument's bank), and records it with `plan_saw`:

```c
static inline void plan_saw(const Insn *in, Function *fn) {
    QPlan *pl = (QPlan *)in->k.p;
    if (__atomic_load_n(&pl->seen, __ATOMIC_RELAXED) != fn) __atomic_store_n(&pl->seen, fn, __ATOMIC_RELAXED);
}
```

It writes only when the callee changes, so a call site that always calls the
same function writes once. The interpreter does not use `seen` itself; the
JIT does, compiling the call site as a direct call to that function behind a
check of the closure. With that, the JIT takes 55–66% off the Lox
`invocation` benchmark, where it took 15–24% before, and 53–55% off compiled
Lua's `methods`, where it took 28–36% ([ROADMAP.md](../ROADMAP.md)). That is
a monomorphic inline cache, and chapter 10 tells the rest.

## Clearing register windows

Every register of a new frame must read as zero until written, so a naive
call clears the whole window. Most of that is wasted: a register the function
always writes before reading never shows what was there. Only two kinds need
clearing: the `R` registers, which the collector scans at any instruction,
and the registers the function may read before writing them. The verifier
finds the second kind (`plan_window` in
[src/vm/verify.c](../../src/vm/verify.c)) by a forward dataflow analysis: the
set of registers definitely written before each instruction, starting from
the parameters, growing along each path and intersected where paths join. A
call then clears one run of words, from the first register that needs it to
the last (`proc_push_frame_fitting` in
[src/vm/proc_internal.h](../../src/vm/proc_internal.h)):

```c
    ZERO_REGS(w + fn->zoff, fn->zlen);
```

The profiler shows the plan in its *functions* table: `cleared` is the words
each call clears, of the window's size, and a further list names the
registers read first. Here `tri` sums `n` down to 1 in `I1`, which it reads
before writing; `tri0` is the same with `mov I1, 0` first:

```asm
.func tri(i64 n) -> i64        ; 1 + 2 + ... + n
1:  add I1, n                  ; I1 is read before it is written: a call clears it
    dec n
    bnz n, 1b
    ret I1
.endf
```

Called once each from `main`, with `--profile`:

```text
       %        insns    entries   cleared  function
   45.7%           32          1       0/2  clear.tri0
   44.3%           31         10       1/2  clear.tri
...
registers read before they are written (each call clears them):
  clear.tri: I1
```

`make check-poison` builds a VM that fills every new window with garbage
before clearing the run, so a register read first but not cleared makes a
test fail. The clearing was measured together with the call plans, the `lpush` form above and
changes to the Lua compiler's output: with all of them, compiled Lua's
`methods` took 16–21% less time on the M5, i7 and Ryzen, and its `fib` up to
12% less ([ROADMAP.md](../ROADMAP.md)). Chapter 6 covers the register stack
these windows live on.

## Worked example: an element off the general path

This program fills an array with squares, then sums it 10,000 times
(`sumarr.masm`):

```asm
; sumarr.masm - sum an array of 1,000 squares, 10,000 times
.func main
    anew.i64 R0, 1000
    mov I1, 0
1:  mul I2, I1, I1             ; fill: R0[i] = i * i
    mov R0[I1], I2
    inc I1
    blt I1, 1000, 1b
    mov I0, 0                  ; the sum
    mov I3, 0                  ; rounds
2:  mov I1, 0
3:  mov I2, R0[I1]             ; load the element into a register...
    add I0, I2                 ; ...and add the register
    inc I1
    blt I1, 1000, 3b
    inc I3
    blt I3, 10000, 2b
    ncall "io.print_i" (I0)
    ncall "io.nl" ()
    ret 0
.endf
```

The inner loop is four instructions, each with a form:

```text
$ ./mlwsvm run --profile sumarr.masm
...
       %        insns   fast  opcode
   25.0%     10011003 100.0%  mov
   25.0%     10011000 100.0%  blt
   25.0%     10011000 100.0%  inc
   25.0%     10000000 100.0%  add
...
```

MLWSVM's `add` can take the element as an operand, so the load looks
redundant. Save a copy as `sumarr_elem.masm` with the two lines at `3:`
replaced by one:

```asm
3:  add I0, R0[I1]             ; add the element directly
```

The profile now shows a quarter fewer instructions, and the `add`, a third of
them, on the general path:

```text
$ ./mlwsvm run --profile sumarr_elem.masm
...
   33.3%     10011000 100.0%  inc
   33.3%     10011000 100.0%  blt
   33.3%     10000000   0.0%  add
...
hottest instructions on the general path:
       %        insns  instruction
   33.3%     10000000  add.i64 I0, R0[I1]                       ; sumarr_elem.main line 12
...
```

And the time, from `--stats` on the M5:

| Program | Instructions | Wall time |
|---|---|---|
| `sumarr.masm` (load, then add registers) | 40,034,007 | 26 ms |
| `sumarr_elem.masm` (add the element) | 30,034,007 | 96 ms |

Fewer instructions, nearly four times the time. The general `add` reads the
element through `rdn`, which has a shortcut for an array element of exactly
the instruction's type, and by these timings it still costs about as much as
a dozen quickened instructions. Arithmetic directly on elements has no form
(ROADMAP.md leaves it until compiled programs show it in the profiler). The
profiler's last table points straight at it, with its line; this is the loop
compiler writers follow: find the hot general-path shape, then emit one that
has a form, or ask for a new form.
[TARGETING.md §12](../TARGETING.md#12-what-runs-fastest) lists every shape
that has one; it reports `bench/nbody.masm` running in 96 ms with element
operands and in 24 ms with its values loaded into registers first.

## Where to look

| What | Where |
|---|---|
| Choosing a form | `interp_quicken`, `interp_quicken_function`, the `Q_*` enum in [src/vm/interp.c](../../src/vm/interp.c) |
| The forms' handlers | [src/vm/qforms.inc](../../src/vm/qforms.inc); the table's entries in [src/vm/qlist.inc](../../src/vm/qlist.inc) |
| Call plans and the callee seen | `QPlan`, `quicken_plan`, `plan_saw` in [src/vm/interp.c](../../src/vm/interp.c); `Q_CALL_R`, `Q_CALLI_R` in [src/vm/qforms.inc](../../src/vm/qforms.inc) |
| What a call clears | `plan_window` in [src/vm/verify.c](../../src/vm/verify.c); `proc_push_frame_fitting` in [src/vm/proc_internal.h](../../src/vm/proc_internal.h) |
| The profiler's tables | `src/tools/hooks.c` ([TOOLING.md](../TOOLING.md#profiler---profile)) |
| Tests of the forms | `tests/perf/quickened.masm`, `quickened_slots.masm`, `quickened_dynamic.masm` |
| Reference | [DESIGN.md §8.2](../DESIGN.md#82-quickening); [PERFORMANCE.md](../PERFORMANCE.md#how-the-interpreter-is-made-fast), items 1, 2, 5 and 6; [TARGETING.md §12](../TARGETING.md#12-what-runs-fastest); [PRIOR_ART.md](../PRIOR_ART.md) for where each idea came from |

Previous: [3. The interpreter loop](03-interpreter-loop.md) · Next: [5. Memory and garbage collection](05-memory-and-gc.md)
