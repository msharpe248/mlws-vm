# MLWSVM Performance

How fast the interpreter is, what made it fast, and what has been left alone
deliberately. Run `make bench` to reproduce: it runs `bench/*.masm` and checks
each one's output against its expected checksum.

## Benchmarks

| Benchmark | Exercises |
|---|---|
| `fib` | recursive calls: `fib(32)` |
| `loop` | a tight integer loop in registers, 30M iterations |
| `sieve` | byte loads and stores in raw memory, 20M bytes |
| `nbody` | `f64` arithmetic on typed array elements |
| `binarytrees` | struct allocation and GC: a depth-18 tree plus 60 short-lived depth-14 trees |
| `maps` | `tostr`, `strcat`, and map insertion and lookup with 300k string keys |
| `pingpong` | message round trips between two processes, 300k |
| `closures` | 5M calls through a closure |

## Results

Wall time in milliseconds, on an Apple M5 Pro laptop, single thread:

| Benchmark | M8 baseline | After M9 | Speed-up | CPython 3.14 (same program) |
|---|---|---|---|---|
| fib | 323 | 108 | 3.0× | 126 |
| loop | 1,845 | 237 | 7.8× | 2,390 |
| sieve | 1,717 | 696 | 2.5× | |
| nbody | 242 | 130 | 1.9× | |
| binarytrees | 276 | 204 | 1.4× | |
| maps | 71 | 60 | 1.2× | 84 |
| pingpong | 168 | 38 | 4.4× | |
| closures | 318 | 149 | 2.1× | |

The tight register loop now runs at about 800M instructions per second, roughly
5 cycles per instruction. That is the lower bound with the current design.

## What was done, and why

Each step was chosen from a profile (`sample` on macOS), not guessed.

1. **Inline the type helpers.** `canon_int`, `type_bank`, `type_size` and similar
   took about a third of all time. They are tiny, but they lived in another
   translation unit, so the compiler could not inline them. They are now
   `static inline`, with the common 64-bit case first. This gave 20–35%
   everywhere.
2. **Quickening.** After a module is verified, each instruction gets an
   *execution opcode* (`Insn.xop`) next to its real opcode (`Insn.op`). Common
   shapes get a specialised handler that skips the generic operand machinery:
   64-bit integer and `f64` arithmetic on registers or immediates; moves;
   compare-and-branch; `inc`/`dec`/`bz`/`bnz`/`jmp`; `div`/`rem` by non-zero
   constants; and calls and returns whose operands are registers of the right
   banks. A shape qualifies only when the generic semantics are trivially the
   same: no width canonicalisation, no signedness differences (unsigned ordering
   is left generic), and no possible trap (`div` by a register, or by `-1`, stays
   generic). The disassembler, tracer, profiler and debugger keep seeing the real
   opcode.
3. **Cheaper call frames.** Frame push and pop are inline, and only the register
   banks a function actually uses are zeroed.
4. **`_setjmp` instead of `setjmp`.** On macOS `setjmp` saves the signal mask with
   a system call. The scheduler sets a jump buffer once per time slice, so
   message-heavy code paid a syscall per message. The VM never touches signal
   masks, so the cheaper variant is safe.

`tests/perf/quickened.masm` checks the quickened forms against the generic
semantics at the edges: overflow, negative divisors, unsigned compares,
`INT64_MIN / -1`, mixed-bank calls, and results written to memory.

## What was not done, and why

* **Computed-goto dispatch.** Not attempted. After quickening, the hot loops spend
  about 5 cycles per instruction including dispatch, and the remaining time is
  spread across the generic operand paths rather than in dispatch itself.
  Revisit if profiles show dispatch dominating.
* **A generational GC.** Deferred. Collection is about 9% of `binarytrees` and 5%
  of `maps`, the most allocation-heavy benchmarks. Halving it would save under
  5% overall, while a generational collector needs a write barrier on every
  reference store. The better next target for allocation-heavy code is `malloc`
  itself (about 10%); a size-class allocator would help there.
* **More specialised memory and array operand forms.** `sieve` and `nbody` now
  spend most of their time in the generic memory and element paths. There are
  many possible shapes, so the right ones to specialise are the ones the
  scripting-language compiler actually emits. Measure its output first.
* **A multi-threaded scheduler.** Still to come (see the roadmap). Processes share
  nothing, so programs will not need to change.
