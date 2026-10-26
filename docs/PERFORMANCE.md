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
| loop | 1,845 | 237 | 7.8× | 2,390 |
| sieve | 1,717 | 670 | 2.6× | |
| nbody | 242 | 129 | 1.9× | |
| binarytrees | 276 | 208 | 1.3× | |
| maps | 71 | 59 | 1.2× | 84 |
| pingpong | 168 | 32 | 5.2× | |
| closures | 318 | 150 | 2.1× | |
| parallel | 507 (one thread) | 61 | 8.2× | |

The tight register loop now runs at about 800M instructions per second, roughly
5 cycles per instruction. That is the lower bound with the current design.

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
* **Lock-free run queues** (Chase–Lev deques). Each queue's lock is almost
  always uncontended, because a thread mostly uses its own queue. Revisit if
  profiles of many-thread, message-heavy programs show queue locks.
* **Spinning before sleeping.** Go's scheduler spins briefly before parking a
  thread, to cut wake-up latency. Here a thread is woken only when there is a
  backlog, which avoided the problem in every benchmark so far.
* **Per-thread timers.** The timer heap is shared, but threads read it only when
  the earliest timer is due (one atomic load per slice).
