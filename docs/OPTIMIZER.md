# The Optimizer

MLWSVM's optimizer rewrites a module's code to do the same thing in fewer
instructions. It removes what compilers commonly leave behind: moves nothing
reads, constants computed at run time, jumps to jumps, copies of copies, and
code nothing reaches. It can also inline small functions and turn calls into
tail calls. Every pass can be turned on or off by name, and the optimizer
reports what each one did.

It works on a module before it is linked: the form the assembler produces and
a `.mlwso` file holds. So it can run in two places:

* **When a module loads.** `mlwsvm run --opt 2 prog.masm` (also `mlwsvm test`,
  `MLWSVM_OPT=2`, or `; option: --opt 2` in a test) optimizes every module as
  it is loaded: the program's, the standard library's, and those in library
  archives. The optimized module is kept in the module cache
  ([TOOLING.md](TOOLING.md#module-cache)), so it is optimized once, not at
  every run.
* **Ahead of time.** `mlwsvm opt -O2 prog.masm -o prog.mlwso` writes the
  optimized module, and prints the report.

The optimizer is off unless asked for. The interpreter's quickener
([PERFORMANCE.md](PERFORMANCE.md#how-the-interpreter-is-made-fast)) is a
different thing: it makes each instruction faster to run, but never removes
or changes one. The two work together. The optimizer leaves shapes the
quickener has fast forms for (it puts a constant second, for one).

## Levels and passes

| Level | Passes | What can tell the difference |
|---|---|---|
| `-O0` | none | |
| `-O1` | fold, simplify, constprop, copyprop, dce, branches, unreachable, peephole | nothing but the number of instructions run |
| `-O2` | also deadfuncs, compact | also the tools: the debugger shows fewer registers and functions |
| `-O3` | also inline, tailcall | also the program, through backtraces: frames disappear from `Error.trace` |

`-O2` is the level to use for compiled programs in general. `-O3` is for
languages that never read a backtrace. A language that uses frames for its
own semantics should stop at `-O2`. Lua reports `error(msg, 2)` at the line
of the function two frames up, for example, and `-O3` changes which function
that is.

| Pass | What it does |
|---|---|
| `fold` | An instruction whose operands are all constants is replaced by a `mov` of its result: arithmetic, bitwise, shifts, compares, `sel`, `cvt` and `bitcast`. |
| `simplify` | Identities true for any value of the other operand: `x + 0`, `x * 1`, `x / 1`, `x - x`, `x ^ x`, `x & -1`, `x \| 0`, `x * 0` become a move. A multiply by a power of two becomes a shift, and `beq x, 0` becomes `bz x`. A compare or commutative operation with its constant first has it put second: `blt 1, x` becomes `bgt x, 1`. |
| `constprop` | A register known to hold a constant at an instruction is read as that constant. A store of the value the register already holds is removed. Constants don't go into a store to an element, a global or a closure capture, whose fast forms take a register. A call's arguments and a ret's values take a constant only where the function computed it, so that computing it can be removed: their register-only forms are faster than the ones with constants. |
| `copyprop` | Within a block, a register copied from another by a full-width move is read from the original instead, so the copy usually dies. |
| `dce` | A pure instruction whose result nothing reads is removed. |
| `branches` | A branch to a `jmp` goes straight to its target. A `jmp` to the next instruction is removed. A branch on constants becomes a `jmp`, or is removed. `bcc L1; jmp L2; L1:` becomes `bncc L2`. A `jmp` to a `ret` becomes the `ret`, and a `jtab` on a constant becomes a `jmp`. A branch is also decided by what earlier code established (see below). |
| `unreachable` | Instructions nothing reaches, from the entry or a live exception handler, are removed. |
| `peephole` | `nop`s, a full-width move of a register to itself, and the move back in `mov a, b; mov b, a` are removed. |
| `deadfuncs` | A function nothing can call is removed. Callable means exported, `main`, or named by an instruction of a callable function (a call, a spawn, a closure). |
| `compact` | A function's registers are renumbered to the lowest of each bank, and its window shrinks to them. Parameters keep their places. Registers a call must clear are placed next to the R bank, so the single run it clears is as short as it can be. |
| `inline` | A call to a small function of the same module becomes its body, on registers of the caller's that nothing else uses. The function must have at most 40 instructions, and no handlers, stack or frame operands, `UP`, tail calls or coroutine switches. Its own calls are then made from the caller's frame. A caller grows by at most its own size. Only direct calls are inlined: a call through a closure (`calli`) has no known target. |
| `tailcall` | `call g (...) -> (r...); ret r...` becomes `tcall g (...)`, for a `g` of the same module that declares the same results. A call inside a `.try` region is never converted. |

**What a branch establishes.** Each edge of a branch knows what the branch
tested:
- after `bnz R0, L`, R0 holds an object at `L` and is null on the
  fall-through (`bz` the other way round);
- after `beq I0, 5, L`, I0 is 5 at `L`;
- an R register whose field or element was used without a trap, or that a
  taken `bisa` tested, holds an object.

These facts decide later branches. Dynamically typed code tests the same
value's type again and again on one path (compiled Lua's `fib` has five such
tests on its path for boxed values), and the tests after the first are
decided. A branch decided this way sends nothing along the edge it never
takes, so what is known on its other edge stays known.

Nothing is inferred where it would be wrong. `bz.i32 x` says nothing about
x's upper 32 bits, so only 64-bit integer and reference tests count, and
`beq F0, 0.0` is taken for `-0.0` too, so float equality counts for nothing.

The `-O1` passes run in rounds until a round changes nothing. Each pass's
result is often another pass's opportunity. A folded constant makes a branch
decidable, which makes a block unreachable, which leaves a move dead.

### Choosing passes

`--opt` (and `mlwsvm opt --passes`) takes a comma-separated list, applied
left to right:

| Item | Meaning |
|---|---|
| `0`, `1`, `2`, `3` | exactly the passes of that level |
| `all`, `none` | level 3, and no passes |
| a pass name | that pass too |
| `-name` | not that pass |

So `--opt 3,-tailcall` is `-O3` without tail calls, and
`--opt fold,constprop,dce` is those three alone. `mlwsvm opt --list` lists the
passes with their levels. `--opt 0` (the default) turns the optimizer off.

## What it keeps the same

Every pass is written against what the interpreter does, and the parts of
that it relies on are these:

* **Only registers and constants are analysed.** An operand in memory, an
  element, a field, a global or a closure capture (`UP`) is never propagated
  through or assumed. The registers used to address it count as read.
* **Calls change only their results.** Every call gets a fresh register
  window, so nothing else in the caller's registers changes across a call,
  and what is known about them survives it. Nothing is assumed about memory
  across anything.
* **Pure is a fixed list.** `mov`, the arithmetic, bitwise, shift and
  floating-point instructions, compares, `sel`, `cvt`, `bitcast`, `tagof` and
  `typeof` are pure, when every operand is a register or a constant. Integer
  division counts only when the divisor is a constant that is not 0 (a
  division by zero traps). Arithmetic on BigInts and decimals is never pure
  (it allocates), and nor is pointer arithmetic (it can trap). Only a pure
  instruction is ever removed or computed early. Anything that can trap,
  allocate, call or touch memory stays where it is.
* **Values are computed by the interpreter's own code.** Folding uses the
  functions the interpreter executes with (`src/core/arith.h`), on the 64-bit
  words registers hold. So a folded value is the one the instruction would
  produce: narrow types wrap, `f32` rounds, shift counts are taken modulo the
  width, NaN is unequal to itself, conversions saturate, and
  `INT64_MIN / -1` is `INT64_MIN`.
* **Types are kept.** A narrower move (`mov.i32 I0, I1`) wraps the value, so
  it is not a copy. Where an instruction stores a value with its own type (an
  untyped `push`, `lpush`, `send`, `ret` without declared results), a
  register is replaced only by a constant of the same natural type. A `null`
  never replaces one there, since its natural type is an integer. A `ret`
  with declared results, and a call to a function of the same module, read
  their values as the declared types, so any constant of that type will do.
* **New windows read as zero.** A function's registers other than its
  parameters start as 0 (`null`, `0.0`), as the ABI guarantees, and the
  analyses use that.
* **Exception handlers see what they would have seen.** An instruction in a
  `.try` region may trap before it writes anything, so whatever the handler
  reads is kept alive throughout the region. A handler starts with nothing
  known. A region whose instructions are all removed protects nothing, and
  its handler goes too.
* **Releasing an object is kept.** An `R` register is a root of the garbage
  collector, so `mov R3, null` can be what lets an object be collected (and
  its finalizer run), even if nothing reads `R3` afterwards. A write to an
  `R` register is removed as dead only where the register already holds
  `null` or a constant, which live as long as the VM.
* **Positions are kept.** The instructions left keep their source lines and
  their `.loc` positions, so backtraces and the debugger still name the right
  lines. An inlined body has the callee's lines and the call's `.loc`
  position.

What a program can still observe at `-O1`:
- the instruction counts, as `--stats`, the profiler and the `insns` quota
  count them;
- where its time slices end;
- how long objects in `R` registers it no longer reads stay alive, which can
  only get shorter.

At `-O2`:
- the debugger and the profiler see the renumbered registers and the
  remaining functions.

At `-O3`:
- an inlined call, and the caller of a tail call, have no frame of their own;
- backtraces and `Error.trace` lack those frames, and the profiler counts
  their time in the caller;
- a recursion through tail calls no longer reaches the call depth limit.

## The report

```
$ mlwsvm opt -O2 fib.masm -o fib.mlwso          # compiled Lua's fib
mlwsvm opt: 1 module, 6 functions, 0.2 ms
                   before    after
  instructions        174      159   -8.6%
  registers            68       62
  pass          changes
  fold                2   constant expressions folded
  simplify            1   instructions simplified
  constprop          21   register reads replaced by constants
  copyprop            1   register reads replaced by the register copied
  dce                 4   dead instructions removed
  branches            7   jumps threaded, removed, inverted or decided
  peephole            1   redundant moves and nops removed
  compact             2   register windows made smaller
  functions changed (most instructions removed first):
    fib.L100$fib  75 -> 63
    fib.L99$main  74 -> 71  (registers 34 -> 30)
    fib.main  15 -> 15  (registers 7 -> 5)
```

* **The totals:** the instructions and the registers in the windows, before
  and after.
* **The passes:** how many changes each made.
* **The functions changed:** up to 20, those that lost the most first.

`-v` adds a log of every change, with its source line, function and
instruction, and the instruction as it was:

```
  changes:
    fib.masm:44 L100$fib+4 constprop: the register already holds that value: mov.r R2, 0
    fib.masm:93 L100$fib+38 fold: computed now: cvt.f64.i64 F6, 1
```

`mlwsvm opt` prints the report on stderr (`--report FILE` to put it in a file,
`-q` for none). For a run, `--opt-report FILE` (or `-` for stderr) writes the
report of every module the run loaded when it ends. A run asked for a report
optimizes its modules afresh, so that the report says what the passes did,
rather than reading the module cache.

### `mlwsvm opt`

```
mlwsvm opt [-L dir] [-O0|-O1|-O2|-O3] [--passes LIST] [-o out.mlwso] [-S] [--report FILE|-] [-v] [-q] file.(masm|mlwso)
mlwsvm opt --list
```

* **The default level** is `-O1`, and `--passes` changes the level's set of
  passes (`-O3 --passes -tailcall`).
* **Output:**
  - the optimized module goes to `-o FILE`, or to `prog.mlwso` for
    `prog.masm`;
  - `-S` writes assembly instead, to `-o FILE` or to stdout, which shows what
    the optimizer made.
* **`--passes encode`** decodes and re-encodes every function without
  changing anything. `make check-tools` uses it to check that the optimizer
  gives every module back byte for byte.

## Tests

Three test-file headers check the optimizer
([ASSEMBLY.md §11](ASSEMBLY.md#11-tools)):

| Header | Meaning |
|---|---|
| `; expect-opt: dce 5` | the optimizer made exactly 5 changes of that pass to this file's module |
| `; expect-insns: main 7` | after optimizing, `main` has 7 instructions |
| `; no-opt: reason` | not optimized by `MLWSVM_OPT` or `--opt` on the command line (the test's own `; option: --opt` still applies) |

* **`tests/opt/`** has a test per pass. Each sets its passes with
  `; option: --opt`, checks the counts, and asserts its results at run time.
  Each pass's edge cases are tested:
  - divisions by zero kept;
  - stores a handler reads kept;
  - releasing objects kept;
  - narrow and NaN folding;
  - types of pushed values;
  - window zeroing for inlined bodies;
  - no tail calls in `.try` regions;
  - facts from branches, and the narrow and float cases where none hold.
* **`make check-opt`** runs the whole suite and the examples with every
  module at `--opt 3`, plainly and with GC stress. Two tests say `no-opt`:
  the debugger's transcript shows registers that `compact` renumbers, and
  `tests/asm/loc.masm` reads frames that `inline` removes.

## What it does not do

* **One module at a time.** It sees no further than the module it optimizes.
  It cannot inline a function of another module, or know what an imported
  function returns.
* **No memory.** Values in memory, globals, elements and fields are never
  tracked, so loads are never removed or reused, and stores are never
  removed.
* **No loop optimizations.** Nothing is hoisted out of loops, and no loop is
  unrolled or strength-reduced.
* **No `P` registers.** Pointer values are not tracked (pointer arithmetic
  can trap).

## Performance

What `--opt` gains depends on the code. Hand-written assembly, like the
benchmarks in `bench/`, has little to remove. Compiled code has more. Compiled
Lua's benchmarks, on an Apple M5 (wall time, median of five runs, with the
optimized modules in the cache):

| Benchmark | `-O2` | `-O3` |
|---|---|---|
| loop | -13% | -15% |
| binarytrees | -4% | -6% |
| iterate | -3% | -4% |
| methods | -2% | -2% |
| spectralnorm | +1% | -30% |
| fib, strings, nbody, fannkuch | within 1.5% | within 1.5% |

`spectralnorm` gains from inlining its small helper functions, at `-O3` only.

Optimizing costs about 1.5 µs per instruction: the Lua runtime library
(27,000 instructions) takes about 40 ms. The module cache keeps the result,
so a later run reads the optimized module instead of optimizing again.
