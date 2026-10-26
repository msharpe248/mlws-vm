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
| `parallel` | 16 independent CPU-bound processes (4M loop iterations each) |

## Results

Wall time in milliseconds, on an Apple M5 Pro laptop (6 performance and 12
efficiency cores). "After M9" uses the default of one worker thread per core;
only `parallel` has the work to use more than one.

| Benchmark | M8 baseline | After M9 | Speed-up | CPython 3.14 (same program) |
|---|---|---|---|---|
| fib | 323 | 112 | 2.9× | 126 |
| loop | 1,845 | 237 | 7.8× | 1,167 |
| sieve | 1,717 | 670 | 2.6× | |
| nbody | 242 | 129 | 1.9× | |
| binarytrees | 276 | 208 | 1.3× | |
| maps | 71 | 59 | 1.2× | 84 |
| pingpong | 168 | 32 | 5.2× | |
| closures | 318 | 150 | 2.1× | |
| parallel | 507 (one thread) | 61 | 8.3× | |

The tight register loop now runs at about 800M instructions per second, roughly
5 cycles per instruction. That is the lower bound with the current design.

The CPython programs are written as functions (`bench/compare/`), so their
variables are fast locals. Until October 2026 this table gave 2,390 ms for
CPython's `loop`. That figure was measured with module-level variables, which
CPython keeps in a dictionary, and it overstated MLWSVM's lead. For the same
programs on Lua 5.5 and the JVM, see
[REGISTER_VS_STACK.md](REGISTER_VS_STACK.md#part-2-measured). Lua's interpreter
runs `fib` 2.3× faster than MLWSVM and the loop 12% faster.

### On Linux

The same benchmarks on two Ubuntu 26.04 desktops (GCC, `-O2`, default threads):

| Benchmark | Intel i7-11700KF (8 cores, 16 threads) | AMD Ryzen 9 5900X (12 cores, 24 threads) |
|---|---|---|
| fib | 205 | 208 |
| loop | 473 | 617 |
| sieve | 1,271 | 1,264 |
| nbody | 222 | 216 |
| binarytrees | 442 | 404 |
| maps | 131 | 139 |
| pingpong | 95 | 71 |
| closures | 286 | 252 |
| parallel | 146 | 121 |
| connections (10,000 at once) | 814 | 1,528 |

### HTTP

`examples/http_server.masm` (a process per connection, an access-log line per
request), loaded by `examples/http_load.masm` (a process per client connection)
running on the other machine over gigabit Ethernet:

| Connections | Requests | Requests/s | Slowest request |
|---|---|---|---|
| 50 | 200,000 | 140,000 | 18 ms |
| 200 | 200,000 | 169,000 | 26 ms |
| 1,000 | 200,000 | 168,000 | 29 ms |

The server ran on the Ryzen machine (24 worker threads), and the load
generator on the Intel one. During a 200-connection run, 40 handlers were made
to fail (20 `GET /crash`, 20 `GET /hog`, which exhausts its heap quota):
all 200,000 other requests were still answered, at the same rate.

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

5. **No collection storm near a heap quota.** The collection threshold is
   capped at three-quarters of a process's heap quota, so the collector runs
   before the quota is hit. Once the live data was above that cap, every
   allocation triggered a full collection, which is quadratic: a process
   filling its last few megabytes took about a second normally, and 20 minutes
   under ThreadSanitizer. Now the heap may always grow by an eighth of its live
   data before the next collection, so a process near its quota collects a
   handful of times and then gets its `E_QUOTA`.

## The small-object allocator (M21)

Profiles of the allocation-heavy benchmarks showed `malloc` and `free` at about
10% on Linux. Since M21, every object up to 256 bytes comes from its process's
own chunks, in 16-byte size classes with a free list each:

* **Allocation** pops the class's free list, or carves from the current chunk.
  There is no lock and no `malloc` call. The collector's sweep frees an object
  by pushing it on its list.
* **Chunks** start at 1 KiB and double up to 64 KiB, so ten thousand small
  processes stay small. `bench/connections` peaks 3% higher than with `malloc`.
* **No sharing.** A process's objects never move to another heap: messages are
  copied, and only the copies (built in a scratch heap that still uses `malloc`)
  are adopted by the receiver. So a process's chunks are simply freed with it.
  The object header records which allocator an object came from.
* **Zeroing** a new object uses one fixed-size `memset` per size class, which
  the compiler inlines into stores. The same profile showed `memset` at about 9%,
  much of it zeroing the register windows of new call frames. Those small
  windows are now zeroed with inline stores too.

AddressSanitizer builds use `malloc` for every object, so that it still sees
each object's lifetime. `MLWSVM_NO_SLABS=1` does the same in any build, for
comparisons. Under `MLWSVM_GC_STRESS=1` (`make check-gc`), freed objects are
filled with a garbage pattern, so a use after a free fails loudly there too.

| Benchmark | Linux, Ryzen 9 5900X: M20 → M21 | macOS, M5 Pro: M20 → M21 |
|---|---|---|
| binarytrees | 420 → 348 ms (−17%) | 210 → 191 ms (−9%) |
| maps | 143 → 107 ms (−25%) | 60 → 60 ms |
| closures | 267 → 260 ms (−2%) | 150 → 145 ms (−3%) |
| fib | 232 → 234 ms | 121 → 116 ms (−3%) |
| others | unchanged | unchanged |

macOS gains less because its allocator's small-object zone is already very
fast.

## A faster interpreter loop (M22)

[REGISTER_VS_STACK.md](REGISTER_VS_STACK.md) compared MLWSVM with Lua 5.5 and
the JVM's interpreter. MLWSVM executed the fewest instructions but was slower
than both: each instruction cost too much, and calls most of all. M22 worked
through the costs one at a time, measuring each step on the Mac and keeping
only what paid (milliseconds, best of five):

| Step | fib | loop | Other |
|---|---|---|---|
| M21 | 115 | 236 | |
| 1. Fast path keeps `pc` and the slice count in locals | 95 | 124 | `parallel` 69 → 29 |
| 2. Calls copy arguments and results directly | 79 | 126 | `binarytrees` 188 → 181 |
| 3. Compact operands inside each instruction | 80 | 124 | |
| 4. Threaded dispatch | 67 | 93 | `closures` 146 → 137 |
| 5. More quickened shapes; shortcuts in the generic operand path | 70 | 87 | `binarytrees` 180 → 119, `closures` 137 → 105, `nbody` 128 → 94 |

1. **The loop head.** Before every instruction, the interpreter counted the
   time slice down, checked for a pending collection or kill, checked for a
   debugger hook, loaded the program counter from the frame, stored it back,
   and counted the instruction in the process. Now the program counter and the
   slice count live in local variables (CPU registers), and the frame's
   `pc` and the process's instruction count are written back only where
   something can look at them: before a generic instruction (which may trap,
   call a native or switch frames), at calls and returns, before the tools'
   hook, and at the end of the slice. Quickened instructions never allocate or
   trap, so the safepoint check for a collection or a kill moved to after each
   generic instruction (and the start of the slice). Whether a debugger hook
   is installed is read once per slice. Nothing observable changed: a process
   that kills itself still stops at the next instruction, and `--stats` counts
   the same instructions.
2. **Calls.** A quickened `call` copied its arguments into an array, pushed the
   frame (a function the compiler chose not to inline), then looked up each
   parameter's type to place it. The loader now records each argument's
   destination register, the frame push is always inlined, and the arguments go
   straight from the caller's registers to the callee's. A frame pushed this way
   is marked, and its quickened `ret` copies the results straight back. Closure
   calls (`calli`) got the same treatment, with the bank checks made per call.
3. **Compact operands.** Quickened handlers read register numbers and
   immediates from the 72-byte decoded `Operand`s, through a pointer. Each
   instruction now also carries a compact copy (three register numbers, a branch
   target and an immediate), which grows `Insn` from 16 to 32 bytes. On its own
   this was within noise. After step 4 it was worth 18% on the loop (113 ms
   without it, 93 with it): once dispatch was cheap, the pointer chase showed.
4. **Threaded dispatch.** Each quickened handler ends by fetching the next
   instruction and jumping straight to its handler through a table (GCC's and
   Clang's "labels as values"), instead of returning to one shared `switch`.
   Each handler's jump is predicted separately. Instructions that are not
   quickened go to the generic `switch` as before. Compilers without the
   extension use a `switch` for the quickened forms too; `make check-switch`
   tests that build, and CI runs it.
5. **More shapes.** `mov` between a register and a struct field, or an array or
   list element indexed by a register, for 64-bit types; `bz`/`bnz` on
   references; and closure calls. Each checks the object at run time and hands
   anything unexpected (null, another struct type, a list element of another
   type, an index out of range) to the generic `mov`, which raises the usual
   error. The generic operand reader and writer also gained shortcuts for array
   elements and struct fields of exactly the instruction's type, which helps
   arithmetic directly on elements (`nbody`). `tests/perf/quickened_slots.masm`
   checks every fallback.

The collector also stopped scanning the whole 1 MB data stack of a process at
every collection. Code may store references below `SP`, so `SP` cannot bound
the scan; instead each process records the lowest stack word ever tagged as a
reference, and the scan starts there. Collection time in `binarytrees` fell by
about 7%, and a GC-stress run of the HTTP example by 13%
(`tests/objects/gc_roots.masm` checks a reference stored far below `SP`).

One experiment was rejected: caching `min(frame capacity, call-depth limit)`
in the process saved 1.5% on `fib` but would go stale if an embedder changed
the limit while processes exist.

Results, before and after (milliseconds, best of five; both versions built and
run on each machine in turn):

| Benchmark | Mac (clang) M21 | M22 | Intel i7 (gcc) M21 | M22 | AMD Ryzen (clang) M21 | M22 |
|---|---|---|---|---|---|---|
| fib | 116 | 70 | 206 | 153 | 236 | 139 |
| loop | 237 | 87 | 475 | 186 | 633 | 285 |
| binarytrees | 192 | 119 | 352 | 236 | 348 | 226 |
| closures | 148 | 105 | 261 | 204 | 261 | 188 |
| nbody | 131 | 94 | 219 | 163 | 237 | 170 |
| sieve | 699 | 612 | 1,269 | 1,067 | 1,342 | 1,165 |
| parallel | 69 | 23 | 133 | 47 | 128 | 60 |
| maps | 61 | 60 | 104 | 103 | 108 | 108 |
| pingpong | 34 | 33 | 94 | 94 | 77 | 76 |

`maps` and `pingpong` are spent in natives (strings, hashing, messages) and in
the scheduler, which M22 did not touch. The tight loop now runs at about 2.1G
instructions per second on the Mac, about 2 cycles per instruction.

Against the other VMs of [REGISTER_VS_STACK.md](REGISTER_VS_STACK.md) (Mac,
milliseconds):

| | fib | loop |
|---|---|---|
| MLWSVM M21 | 115 | 234 |
| MLWSVM M22 | 69 | 87 |
| Lua 5.5 | 49 | 211 |
| JVM, interpreter only | 87 | 183 |
| CPython 3.14 | 128 | 1,150 |

MLWSVM now runs the loop 2.4× faster than Lua and 2.1× faster than the JVM's
interpreter, and `fib` faster than the JVM's interpreter. On the Intel machine
with GCC the picture is similar for the loop (MLWSVM 183 ms, Lua 289, the JVM's
interpreter 396), but GCC gains less on calls: `fib` takes 152 ms there, against
100 for Lua and 104 for the JVM's interpreter. Lua is still 1.4× faster on `fib`
on the Mac: a call still sets up a separate window in each register bank
the callee uses, zeroes it and restores four bank tops on return, where Lua
slides one window.

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

`parallel` with different thread counts (best of three):

| Threads | 1 | 2 | 4 | 6 | 8 | 12 | 16 | 18 |
|---|---|---|---|---|---|---|---|---|
| ms | 508 | 264 | 136 | 99 | 89 | 75 | 62 | 61 |
| Speed-up | 1.0× | 1.9× | 3.7× | 5.1× | 5.7× | 6.8× | 8.2× | 8.3× |

Scaling is close to linear up to the six performance cores. The efficiency cores
add less after that.

Single-process benchmarks run at the same speed as before: each 2,000-instruction
time slice adds a few uncontended lock operations and one clock read. Messages got
*faster* (`pingpong` 38 → 32 ms). Locking added cost, but two other changes
saved more: the scheduler reads the clock once per slice instead of three times
(a slice in a ping-pong is only a few instructions long), and the short critical
sections use `os_unfair_lock` on macOS instead of `pthread_mutex`. A first
version with plain mutexes and three clock reads took 63 ms.

The tools: the debugger, tracer and profiler run the program on one thread (they
see every instruction). The monitor pauses the other threads at a slice boundary
for each request it serves.

## What was not done, and why

* **Cheaper calls, further.** M22 took a `fib` call from about 16 ns to 10 ns;
  Lua 5.5 takes 7. The rest is the frame itself: a window per register bank,
  zeroed, and four bank tops to restore. A single shared window would need a
  different register model, so this waits until real programs show calls
  dominating.
* **Fewer operands per instruction.** Measured, and rejected. Storing up to three
  operands inside each instruction, as a fixed 3-operand format would, was no
  faster, and fixed-format calls would make `fib` 22% slower. See
  [REGISTER_VS_STACK.md](REGISTER_VS_STACK.md), which also compares MLWSVM with Lua,
  the JVM and CPython.
* **A compact decoded form for everything.** Quickened instructions carry a
  compact copy of their operands since M22, but every instruction still keeps
  its 72-byte `Operand`s for the generic path, the debugger and the tools. The
  benchmarks fit in cache, so this does not show yet. Shrinking `Operand` is the
  fix if large programs show cache misses.
* **A generational GC.** Deferred. Collection is about 12% of `binarytrees` and
  4% of `maps`, the most allocation-heavy benchmarks (it was 9% of `binarytrees`
  before M22 made the rest faster). Halving it would save about 6% at most, while a generational collector needs a write barrier on every
  reference store. (The other cost of allocation, `malloc` itself, is gone since
  M21's size-class allocator.)
* **More specialised shapes.** M22 quickened the field and element moves any
  compiler emits. Arithmetic directly on elements and fields (`nbody`) and raw
  memory (`sieve`) still take the generic path. There are many possible shapes,
  so the right ones to add are the ones the scripting-language compiler actually
  emits. Measure its output first.
* **Lock-free run queues** (Chase–Lev deques). Each queue's lock is almost
  always uncontended, because a thread mostly uses its own queue. Revisit if
  profiles of many-thread, message-heavy programs show queue locks.
* **Spinning before sleeping.** Go's scheduler spins briefly before parking a
  thread, to cut wake-up latency. Here a thread is woken only when there is a
  backlog, which avoided the problem in every benchmark so far.
* **Per-thread timers.** The timer heap is shared, but threads read it only when
  the earliest timer is due (one atomic load per slice).
