# MLWSVM Performance

How fast the interpreter is, how it is made fast, and what has been left alone
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
| `parallel` | 16 independent CPU-bound processes (4M loop iterations each) |
| `connections` | 10,000 TCP connections open at once, each end in its own process, then a line echoed on each |

## Results

Wall time in milliseconds, best of five, with the default of one worker thread
per core:

| Benchmark | Apple M5 Pro, Clang | Intel i7-11700KF, GCC | AMD Ryzen 9 5900X, Clang |
|---|---|---|---|
| fib | 71 | 181 | 137 |
| loop | 86 | 185 | 164 |
| sieve | 633 | 1,024 | 1,155 |
| nbody | 97 | 164 | 166 |
| binarytrees | 118 | 239 | 230 |
| maps | 64 | 102 | 107 |
| pingpong | 32 | 95 | 71 |
| closures | 109 | 211 | 193 |
| parallel | 23 | 49 | 47 |
| connections | 1,024 | 687 | 1,200 |

The Mac is a laptop with 6 performance and 12 efficiency cores, running macOS.
The two desktops run Ubuntu 26.04: the Intel one has 8 cores (16 threads), the
AMD one 12 (24 threads). All builds use `-O2`.

The interpreter is sensitive to code layout: a change that does not touch the
hot handlers at all (adding one opcode, say) can move `fib` on GCC or `loop`
and `nbody` on the Ryzen by 10% in either direction, because it changes how the
compiler allocates registers in the interpreter's loop (see item 9 below).
Differences that size between builds are not, by themselves, regressions.

The tight loop runs at about 2.1G instructions per second on the Mac, about two
clock cycles per instruction. `maps` and `pingpong` spend their time in natives
(strings, hashing, message copying) and in the scheduler rather than in the
interpreter loop.

### Compiled Lox

The benchmarks of *Crafting Interpreters*, compiled by the Lox compiler in the
`mlws-compilers` project, which keeps each dynamically typed value in an F and
an R register and caches method lookups at each call site. Milliseconds of wall
time, best of three:

| Benchmark | Apple M5 Pro | Intel i7, GCC | AMD Ryzen, Clang |
|---|---|---|---|
| fib | 595 | 1,091 | 1,073 |
| binary_trees | 2,076 | 4,090 | 4,189 |
| trees | 2,111 | 5,436 | 6,643 |
| instantiation | 529 | 861 | 924 |
| invocation | 297 | 664 | 580 |
| method_call | 193 | 364 | 369 |
| properties | 413 | 843 | 814 |
| zoo | 327 | 628 | 628 |

These are 1.3–3.0× faster than before the forms for compiled dynamic code
existed ([item 1](#how-the-interpreter-is-made-fast) below): `invocation` most, since every call there passed a
constant, then the property and method benchmarks, whose inline-cache checks
and field reads took the generic path. What they spend their time on now is
mostly field maps and allocation.

### Compiled Lua

The Lua 5.5 compiler in `mlws-compilers`, still in development, keeps each Lua
value in three registers (an integer, a float and a reference whose value says
which is live) and gives tables shapes and per-site caches. Its benchmarks, as
whole runs (start-up and assembling included), in milliseconds, best of three,
against the reference interpreter running the same source:

| Benchmark | MLWSVM, M5 | Lua 5.5, M5 | MLWSVM, i7 | Lua 5.5, i7 |
|---|---|---|---|---|
| loop | 300 | 220 | 484 | 289 |
| iterate | 165 | 58 | 275 | 78 |
| fib | 193 | 52 | 325 | 101 |
| methods | 2,183 | 353 | 3,941 | 564 |
| strings | 2,302 | 136 | 3,865 | 213 |

The profiler shows nearly all of these instructions on fast paths; what the
generated code still spends is instruction count (type guards on values of
unknown type, two-word table values, hash lookups), allocation, and, for
`strings`, string operations, which are natives.

### Against other virtual machines

The same `fib` and `loop` programs on Lua 5.5, the JVM and CPython
(`bench/compare/`, milliseconds, best of five, each VM's start-up time
subtracted):

| | `fib` (Mac) | `loop` (Mac) | `fib` (Intel) | `loop` (Intel) |
|---|---|---|---|---|
| MLWSVM | 72 | **87** | 149 | **189** |
| Lua 5.5 | **52** | 215 | **101** | 288 |
| JVM, interpreter only (`-Xint`) | 92 | 186 | 104 | 396 |
| CPython | 133 | 1,216 | 186 | 2,375 |
| JVM with its JIT compiler | 3 | 16 | 8 | 31 |

MLWSVM runs the loop 2.5× faster than Lua and 2.1× faster than the JVM's
interpreter on the Mac. Lua runs `fib` 1.4× faster on the Mac and 1.5× faster on
the Intel machine, where the JVM's interpreter is faster too: an MLWSVM call
costs about 10 ns on the Mac, against 7 ns in Lua (see
[What is not done](#what-is-not-done-and-why)). The JIT row is there for scale.
[REGISTER_VS_STACK.md](REGISTER_VS_STACK.md) explains these numbers, with the
instruction counts and listings of each VM's code.

### HTTP

`examples/http_server.masm` (a process per connection, an access-log line per
request), loaded by `examples/http_load.masm` (a process per client connection)
running on the other machine over gigabit Ethernet:

| Connections | Requests | Requests/s | Slowest request |
|---|---|---|---|
| 50 | 200,000 | 156,000 | 9 ms |
| 200 | 200,000 | 171,000 | 13 ms |
| 1,000 | 200,000 | 168,000 | 20 ms |

The server ran on the AMD machine (24 worker threads), and the load generator
on the Intel one (best of two runs). During a 200-connection run of 600,000
requests, 40 handlers were made to fail (20 `GET /crash`, 20 `GET /hog`, which
exhausts its heap quota): every other request was still answered, at 173,000
requests per second.

## How the interpreter is made fast

Each technique was chosen from a profile (`sample` on macOS), and kept only if
it measured faster.

1. **Decode once, then quicken.** The loader decodes each function once into
   `Insn`s with resolved operands, and verifies them. Each instruction then gets
   an *execution opcode* (`Insn.xop`) next to its real one (`Insn.op`). Common
   shapes get a specialised handler that skips the generic operand machinery:
   64-bit integer and `f64` arithmetic on registers and immediates, moves,
   compare-and-branch, `inc`/`dec`/`bz`/`bnz`/`jmp`, `div`/`rem` by constants
   other than 0 and -1, calls and returns, field and element moves, null tests,
   and closure calls. A second set covers the shapes compiled dynamically typed
   code is made of (taken from the Lox compiler's output, below): constants
   among call arguments and `ret` values, reference constants and globals moved
   into registers, reference and `f64` set-compares and branches, `isa`, and
   `mget` by a constant key. A third covers what the Lua compiler measured:
   64-bit globals, elements at a constant index and closure captures (`UP[n]`)
   moved to or from registers, `len`, `tagof` of an element, `lpush` of
   registers into a list's spare capacity, and `f64` `sqrt`, `neg` and `abs`. The profiler lists the instructions that still run
   generically, which is how these were chosen. A shape qualifies only when its generic semantics are
   trivially the same: no width canonicalisation, no signedness differences
   (unsigned ordering stays generic), and no trap that cannot be checked
   cheaply. The disassembler, tracer, profiler and debugger see the real opcode.
2. **Compact operands.** Each decoded operand is a 72-byte `Operand` with room
   for every addressing mode. A quickened instruction also carries a compact
   copy inside its 32-byte `Insn`: three register numbers, a branch target and
   an immediate. Its handler reads only that copy, with no pointer to follow.
   Measured with threaded dispatch, this saved 18% on `loop`.
3. **Threaded dispatch.** Each quickened handler ends by fetching the next
   instruction and jumping straight to its handler through a table (GCC's and
   Clang's "labels as values"), so each handler's jump is predicted separately.
   Instructions that are not quickened go to the generic `switch`. Without
   threaded dispatch, `loop` takes 43% longer and `fib` 17% longer. Compilers
   without the extension use a `switch` for the quickened forms too;
   `make check-switch` tests that build, and CI runs it.
4. **A lean loop head.** The program counter and the count of instructions
   left in the time slice live in local variables, so in CPU registers. The
   frame's `pc` and the process's instruction count are written back only where
   something can look at them: before a generic instruction (which may trap,
   call a native or switch frames), at calls and returns, before the tools'
   hook, and at the end of the slice. Quickened instructions never allocate or
   trap, so the safepoint that checks for a pending collection or kill comes
   after each generic instruction and at the start of each slice. After a
   generic instruction the count left is recomputed from the one written back,
   rather than kept: it is then not live across the generic code's calls, and
   the compiler can keep it in a register in the quickened handlers. (Kept,
   GCC put it on the stack once enough code surrounded the loop, and `loop`
   took 52% longer.) Whether a debugger hook is installed is read once per
   slice. None of this is observable: a process that kills itself stops at the
   next instruction, and `--stats` counts every instruction.
5. **Direct calls.** The loader records where each argument of a quickened
   `call` goes in the callee's register windows, so the arguments are copied
   straight from the caller's registers to the callee's. Frame push and pop are
   always inlined, and only the register banks a function uses are zeroed. A
   frame pushed this way is marked, and its quickened `ret` copies the results
   straight back. Closure calls (`calli`) work the same way, with the closure's
   type and banks checked per call.
6. **Fields and elements.** `mov` between a register and a struct field, or an
   array or list element indexed by a register, for 64-bit types, has its own
   handlers. Each checks the object at run time and hands anything unexpected
   (null, another struct type, a list element of another type, an index out of
   range) to the generic `mov`, which raises the usual error. The generic operand
   reader and writer also have shortcuts for array elements and struct fields of
   exactly the instruction's type, which helps arithmetic directly on elements
   (`nbody`).
7. **Inlined type helpers.** `canon_int`, `type_bank`, `type_size` and similar
   are tiny but were a third of all time when they lived in another translation
   unit. They are `static inline`, with the common 64-bit case first.
8. **`_setjmp` instead of `setjmp`.** On macOS `setjmp` saves the signal mask
   with a system call. The scheduler sets a jump buffer once per time slice, so
   message-heavy code would pay a system call per message. The VM never touches
   signal masks, so the cheaper variant is safe.
9. **Rare and large work outside the loop.** The quickened handlers and the
   generic `switch` are one function, and how a compiler allocates its registers
   decides the speed of every handler. Any code added to that function can
   change it: two never-taken branches and one call, for coroutines, made `fib`
   7–14% and `loop` up to 43% slower, differently with each compiler. So the
   coroutine instructions, and a coroutine's last return, are handled by a
   wrapper around the loop: the loop returns to it for an instruction it does
   not run, and a coroutine's first frame returns as a process's entry function
   does. The larger fast forms (calls and returns with constants, `mget`) do
   their work in functions of their own and keep only a call in the loop:
   written inline, they made GCC's `fib` 14% slower. On x86 the generic forms
   also re-read their instruction and frame from memory, so the fast path's
   copies are not live across the generic code's calls; without that, GCC kept
   the instruction pointer on the stack and Lua's `iterate` ran 11% slower. On
   arm64, with twice the registers, the same change made `loop` 46% slower, so
   it is x86 only. Moving the whole generic
   `switch` out of the loop instead cost the Apple M5 build 11–15% on the
   generic-heavy benchmarks (`binarytrees`, `sieve`, `nbody`), for the call per
   generic instruction.

`tests/perf/quickened.masm` checks the arithmetic, branch and call forms
against the generic semantics at the edges: overflow, negative divisors,
unsigned compares, `INT64_MIN / -1`, mixed-bank calls, and results written to
memory. `tests/perf/quickened_slots.masm` checks every fallback of the field,
element, null-test and closure-call forms, and `tests/perf/quickened_dynamic.masm`
those of the forms for compiled dynamically typed code.

[TARGETING.md §12](TARGETING.md#12-what-runs-fastest) tells compilers which
shapes are fast.

## Coroutines

Measured on the Apple M5 Pro (best of 3; Lua 5.5 for comparison):

| Operation | MLWSVM | Lua 5.5 |
|---|---|---|
| A resume and a yield, in a generator loop (5 million) | 177 ms (35 ns each) | 272 ms (`coroutine.wrap`) |
| Make a coroutine, resume it twice, let it end (1 million) | 230 ms (230 ns each) | 196 ms |

A switch swaps about fifteen fields between the process and the coroutine object
and reloads the frame. It is not on the interpreter's fast path (see item 9
above), but costs no more than a few generic instructions. Making a coroutine
is mostly allocating and zeroing its data stack (8 KiB by default), so a larger
stack costs proportionally more.

## Memory

### The small-object allocator

Every object up to 256 bytes comes from its process's own chunks, in 16-byte
size classes with a free list each:

* **Allocation** pops the class's free list, or carves from the current chunk.
  There is no lock and no `malloc` call. The collector's sweep frees an object
  by pushing it on its list.
* **Chunks** start at 1 KiB and double up to 64 KiB, so ten thousand small
  processes stay small.
* **No sharing.** A process's objects never move to another heap: messages are
  copied, and only the copies (built in a scratch heap that uses `malloc`) are
  adopted by the receiver. So a process's chunks are simply freed with it. The
  object header records which allocator an object came from.
* **Zeroing** a new object uses one fixed-size `memset` per size class, which
  the compiler inlines into stores. The small register windows of new call
  frames are zeroed with inline stores too.

`MLWSVM_NO_SLABS=1` uses `malloc` for every object instead, for comparisons:

| Benchmark | Mac | Intel | AMD |
|---|---|---|---|
| binarytrees, with the allocator / with `malloc` | 116 / 126 | 237 / 301 | 229 / 294 |
| maps, with the allocator / with `malloc` | 61 / 61 | 102 / 135 | 108 / 145 |

macOS gains less because its own allocator's small-object zone is already very
fast. AddressSanitizer builds always use `malloc`, so that it sees each
object's lifetime. Under `MLWSVM_GC_STRESS=1` (`make check-gc`), freed objects
are filled with a garbage pattern, so a use after a free fails loudly there too.

### The collector

The collector is a non-moving mark-and-sweep, one per process. It takes about
11% of `binarytrees` and 4% of `maps`, the most allocation-heavy benchmarks.

* **Near a heap quota.** The collection threshold is capped at three-quarters
  of the process's heap quota, so the collector runs before the quota is hit.
  The heap may always grow by an eighth of its live data before the next
  collection, so a process whose live data is close to its quota collects a
  handful of times and then gets its `E_QUOTA`, instead of collecting at every
  allocation.
* **The data stack.** Code may store references below `SP`, so `SP` cannot
  bound the scan of a process's 1 MB data stack. Instead each process records
  the lowest stack word ever tagged as a reference, and the scan starts there
  (`tests/objects/gc_roots.masm` checks a reference stored far below `SP`).

## The multi-threaded scheduler

Processes run on N worker threads (`--threads N`; default one per core). The
design is in [ABI.md](ABI.md) §1.1 and at the top of `src/vm/sched.c`. In short:

* **Nothing is locked on the interpreter's fast path.** Processes share nothing,
  so a running process touches only its own state. Locks guard only what other
  threads touch: a process's inbox, links, monitors and scheduling state, the run
  queues, and the timer heap.
* **Per-thread run queues with work stealing.** An idle thread takes half of
  another thread's queue, and sleeps when there is nothing to take.
* **A woken process stays where it was woken.** A receiver woken by a message
  goes on the sender's queue, and another thread is woken only when that queue
  has a backlog. Waking a sleeping thread costs microseconds, far more than a
  message, so a ping-pong pair stays on one thread instead of bouncing between
  two.
* **Heaps are never shared.** A sender copies a message into a detached block of
  objects; the receiver links the block into its own heap on its own thread. The
  collector needs no locks.
* **Cheap slices.** A time slice is 2,000 instructions, or fewer if the process
  blocks. The scheduler reads the clock once per slice, and the short critical
  sections use `os_unfair_lock` on macOS rather than `pthread_mutex`. Both
  matter in message-heavy code, where a slice may be only a few instructions
  long: with three clock reads per slice and plain mutexes, `pingpong` took
  twice as long.

`parallel` on the Mac with different thread counts (best of six):

| Threads | 1 | 2 | 4 | 6 | 8 | 12 | 16 | 18 |
|---|---|---|---|---|---|---|---|---|
| ms | 191 | 102 | 56 | 37 | 36 | 30 | 26 | 25 |
| Speed-up | 1.0× | 1.9× | 3.4× | 5.1× | 5.4× | 6.3× | 7.4× | 7.8× |

Scaling is close to linear up to the six performance cores. The efficiency cores
add less after that.

The tools: the debugger, tracer and profiler run the program on one thread (they
see every instruction). The monitor pauses the other threads at a slice boundary
for each request it serves.

## What is not done, and why

* **Cheaper calls, further.** A `fib` call costs about 10 ns; Lua 5.5 takes 7.
  The rest is the frame itself: a separate window in each register bank the
  callee uses, zeroed, and four bank tops to restore on return, where Lua slides
  one window over a single stack. A shared window would need a different
  register model, so this waits until real programs show calls dominating.
* **Fewer operands per instruction.** Measured, and rejected. Storing the
  operands inside each instruction, as a fixed 3-operand format would, was no
  faster, and fixed-format calls make `fib` 13–17% slower. See
  [REGISTER_VS_STACK.md](REGISTER_VS_STACK.md#part-4-up-to-32-operands-against-a-fixed-3).
* **A compact decoded form for everything.** Quickened instructions carry a
  compact copy of their operands, but every instruction still keeps its 72-byte
  `Operand`s for the generic path, the debugger and the tools. The benchmarks fit
  in cache, so this does not show yet. Shrinking `Operand` is the fix if large
  programs show cache misses.
* **A generational GC.** Collection is about 11% of `binarytrees`, so halving
  it would save about 5% at most, while a generational collector needs a write
  barrier on every reference store.
* **More specialised shapes.** Arithmetic directly on elements and fields
  (`nbody`) and raw memory (`sieve`) take the generic path. Rewriting `nbody` to
  load into registers, compute there and store back takes it from 97 ms to
  24 ms, so compilers can already get most of that speed. The shapes worth a
  form of their own are the ones compiled programs run most: the profiler's
  list of instructions on the general path shows them, as it showed the ones
  added for compiled Lox. What remains there for Lox is allocation (`new`,
  `mnew`, `mset`), which a fast form must not do.
* **Lock-free run queues** (Chase–Lev deques). Each queue's lock is almost
  always uncontended, because a thread mostly uses its own queue. Revisit if
  profiles of many-thread, message-heavy programs show queue locks.
* **Spinning before sleeping.** Go's scheduler spins briefly before parking a
  thread, to cut wake-up latency. Here a thread is woken only when there is a
  backlog, which avoided the problem in every benchmark so far.
* **Per-thread timers.** The timer heap is shared, but threads read it only when
  the earliest timer is due (one atomic load per slice).
