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
| fib | 50 | 93 | 92 |
| loop | 86 | 152 | 135 |
| sieve | 582 | 1,023 | 1,039 |
| nbody | 98 | 167 | 158 |
| binarytrees | 103 | 196 | 190 |
| maps | 62 | 104 | 109 |
| pingpong | 33 | 96 | 74 |
| closures | 99 | 176 | 166 |
| parallel | 24 | 44 | 33 |
| connections | 1,061 | 680 | 1,212 |

The Mac is a laptop with 6 performance and 12 efficiency cores, running macOS.
The two desktops run Ubuntu 26.04: the Intel one has 8 cores (16 threads), the
AMD one 12 (24 threads). All builds use `-O2`.

With tail-call dispatch, a change to some handlers leaves the others' code
alone, and builds differ by a few percent at most (item 3 below); the
dispatch loop that other compilers get is more sensitive, by 5% or more.

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
| fib | 513 | 932 | 921 |
| binary_trees | 2,002 | 4,011 | 4,036 |
| trees | 1,864 | 5,012 | 6,135 |
| instantiation | 490 | 835 | 837 |
| invocation | 272 | 597 | 515 |
| method_call | 175 | 329 | 313 |
| properties | 383 | 752 | 688 |
| zoo | 307 | 600 | 493 |

These are 1.3–3.0× faster than before the forms for compiled dynamic code
existed ([item 1](#how-the-interpreter-is-made-fast) below): `invocation` most, since every call there passed a
constant, then the property and method benchmarks, whose inline-cache checks
and field reads took the generic path. What they spend their time on now is
mostly field maps and allocation.

### Compiled Lua

The Lua 5.5 compiler in `mlws-compilers`, still in development, keeps each Lua
value in three registers (an integer, a float and a reference whose value says
which is live), gives tables shapes and per-site caches, and makes typed
copies of local functions for the argument types they are called with. Its
benchmarks, as whole runs (start-up and assembling its runtime, about 40 ms on
the M5, included), in milliseconds, best of three, against the reference
interpreter running the same source:

| Benchmark | MLWSVM, M5 | Lua 5.5, M5 | MLWSVM, i7 | Lua 5.5, i7 |
|---|---|---|---|---|
| loop | 225 | 221 | 382 | 289 |
| iterate | 125 | 57 | 193 | 79 |
| fib | 91 | 54 | 150 | 102 |
| methods | 615 | 353 | 1,086 | 563 |
| strings | 406 | 144 | 648 | 211 |
| binarytrees (15) | 623 | 388 | 1,243 | 579 |
| nbody (500,000) | 1,044 | 432 | 1,741 | 585 |
| spectralnorm (500) | 263 | 181 | 416 | 269 |
| fannkuch (10) | 2,026 | 814 | 3,063 | 1,160 |

Without start-up, `fib` runs in 50 ms on the M5, as fast as Lua: its typed
copy executes fewer instructions than Lua's bytecode does. Counted with Lua's
own instruction hook, the others execute 1.7 to 5.2 times as many, and each
costs less: 0.5–0.9 ns on the M5 in the loop-heavy benchmarks, against Lua's
1.0–1.5 ns. What the
generated code still spends is instruction count (type guards on values of
unknown type, two-word table values, inline-cache guards), allocation, and,
in `strings` and `iterate`, library functions that run as VM code where
Lua's are in C.

### Against other virtual machines

The same `fib` and `loop` programs on Lua 5.5, the JVM and CPython
(`bench/compare/`, milliseconds, best of five, each VM's start-up time
subtracted):

| | `fib` (Mac) | `loop` (Mac) | `fib` (Intel) | `loop` (Intel) |
|---|---|---|---|---|
| MLWSVM | **50** | **86** | **92** | **149** |
| Lua 5.5 | 51 | 213 | 101 | 289 |
| JVM, interpreter only (`-Xint`) | 90 | 185 | 104 | 396 |
| CPython | 128 | 1,177 | 186 | 2,374 |
| JVM with its JIT compiler | 3 | 16 | 8 | 31 |

MLWSVM is the fastest of these interpreters on both programs. It runs the
loop 2.5× faster than Lua and 2.2× faster than the JVM's interpreter on the
Mac, and `fib` as fast as Lua there and 1.1× faster on the Intel machine: a
call costs about 7 ns on the Mac, as in Lua. The JIT row is there for scale.
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
   registers into a list's spare capacity, and `f64` `sqrt`, `neg` and `abs`. A
   fourth, from the Lua compiler's later profiles: calls and returns with
   constants copied inline (each constant converted at load time), closure tail
   calls, `new` and `lnew`, constants stored to fields, `mget` of a key in any
   register, `jtab`, `cvt.f64.i64`, shifts, unsigned compare-and-branch,
   `f64` arithmetic with a constant first operand, and `ldv` of a list or
   array element. A fifth, also asked for by the Lua compiler: elements at a
   register index plus a constant (`R1[I2+1]`, in forms of their own, so that
   a plain index costs nothing more) and `bisa`/`bnisa`, a struct-type test
   and branch in one. A sixth: `lpush` with constants among its values, the
   shape of a table constructor's keys and values (each constant's word and
   type worked out at load time), which took 16% off compiled Lua's
   `methods`. The profiler lists the
   instructions that still run generically, which is how these were chosen. A
   shape qualifies only when its generic semantics are trivially the same: no
   width canonicalisation, and no trap that cannot be checked cheaply. The disassembler, tracer, profiler and debugger see the real opcode.
2. **Compact operands.** Each decoded operand is a 72-byte `Operand` with room
   for every addressing mode. A quickened instruction also carries a compact
   copy inside its 32-byte `Insn`: three register numbers, a branch target and
   an immediate. Its handler reads only that copy, with no pointer to follow.
   Measured with threaded dispatch, this saved 18% on `loop`.
3. **Tail-call dispatch.** Each quickened handler is a function of its own
   that ends by tail-calling the next instruction's handler through a table
   (`musttail`, which Clang and GCC 15 guarantee), so each handler's jump is
   predicted separately and nothing grows the C stack. The hot state travels in
   argument registers: the instruction, the slice counter, the `I`, `F` and `R`
   windows and a pointer to the rest; with Clang's `preserve_none` calling
   convention, also the current frame and its code, and no handler saves
   registers for another. Each handler is compiled, and its registers
   allocated, on its own, so a change to one does not change the others. Adding
   an opcode and a handler moves the benchmarks by about 2% on the M5 and 1% on
   the i7 (up to 4% on the Ryzen, whose run-to-run variation is about 3%).
   It is faster as well: against the dispatch loop, `loop` takes 18–19% less
   time on the x86 machines, compiled Lua's `loop` 22–37% less and its
   `methods` 5–7% less, and `fib` 4–9% less. `nbody`, which runs mostly
   generic instructions, is the exception, at up to 8% slower on the Ryzen.
   The instructions that are not quickened run in one function, `h_generic`,
   with the same calling convention and without a stack-protector check, so a
   generic instruction costs no more than in a loop. Compilers that cannot
   guarantee tail calls (GCC before 15, MSVC) build the same handlers into one
   dispatch loop instead, jumping through a table of labels (GCC's and Clang's
   "labels as values") or, failing that, a `switch`; `make check-loop` and
   `make check-switch` test those builds, and CI runs both.
4. **A lean dispatch.** The current instruction and the count of instructions
   left in the time slice live in CPU registers (handler arguments, or the
   loop's locals). The
   frame's `pc` and the process's instruction count are written back only where
   something can look at them: before a generic instruction (which may trap,
   call a native or switch frames), at calls and returns, before the tools'
   hook, and at the end of the slice. Quickened instructions do not trap,
   except `new` and `lnew`, which save the `pc` before allocating. An
   allocation only marks a collection as due: the safepoint that runs a pending
   collection or kill comes after each generic instruction and at the start of
   each slice, so a run of quickened instructions delays a collection by at most
   one slice (2,000 instructions). Whether a
   debugger hook is installed is read once per slice; with one, every
   instruction goes through the slice check, which calls it. None of this is observable: a process that kills itself stops at the
   next instruction, and `--stats` counts every instruction.
5. **One register stack, and direct calls.** A frame's registers are one window
   on a single register stack: its `I`, `F`, `R` and `P` registers one after
   another, at offsets fixed for each function when it is loaded. Pushing a
   frame checks one limit, moves one stack top and clears one run of words;
   popping restores one top. Each quickened `call` and `ret` gets a plan when it
   is loaded: the callee, and as bytes each argument's slot in the caller's
   window and in the callee's, and each result's, kept just after the
   instruction's operands. Arguments are copied word by word straight across,
   with no test of their bank and with one cache line read instead of a
   72-byte operand per value; a frame pushed this way is marked, and its
   quickened `ret` copies the results back the same way. Closure calls
   (`calli`) work alike, with the closure's type and banks checked per call.
   Measured with a window per bank instead, every call and return does its
   bookkeeping four times, and `fib` takes 37% longer.

   A new window must read as zero, but only its `R` registers (which the
   collector scans) and those the function may read before writing them need
   clearing. A dataflow pass at load time (`plan_window` in
   `src/vm/verify.c`) finds the second kind, from the registers definitely
   written before each instruction, and a call clears one run, from the first
   register that needs it to the last. `ldv` zeroes the destination its value
   does not pick, so that both count as written. Compiled Lua's typed copy of
   `fib` (6 words, no references) clears nothing at all. `make check-poison`
   fills every new window with garbage before the run is cleared, so that a
   register read before it is written, but not cleared, makes a test fail.
   The run's length is a 16-bit field like the window's size: with a byte,
   GCC knew the clear was under 2 KiB and expanded it inline as `rep stos`,
   whose start-up made calls on x86 up to a third slower than a call to
   `memset`.
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
9. **Rare work outside the dispatch.** The coroutine instructions, and a
   coroutine's last return, are handled by a wrapper around the dispatch: it
   is handed an instruction it does not run, and a coroutine's first frame
   returns as a process's entry function does. In the dispatch loop (the
   fallback build), this matters most: its handlers and the generic forms are
   one function, whose register allocation decides the speed of every handler,
   and code added to it moved `fib` and `loop` by 7–52%, differently with each
   compiler. The loop build therefore also does the larger fast forms (calls
   and returns with constants, `mget`) in functions of their own, recomputes the
   slice counter after a generic instruction instead of keeping it live, and on
   x86 has the generic forms re-read their instruction and frame.

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
  There is no lock and no `malloc` call.
* **The sweep walks the chunks in address order.** A chunk holds only objects,
  one after another, each starting with its header, which gives its size; a
  free one keeps its header and is marked free. So the sweep reads memory in
  order instead of following a list from object to object, and rebuilds each
  class's free list in address order as it goes, so that the program then
  allocates through memory in order too. Objects from `malloc` are on a list
  of their own. This cut collection time by 31% in compiled Lua's `strings`
  and by 21% in a program with 200,000 long-lived lists; `binarytrees`, whose
  sweep frees most of what it visits, did not change.
* **List storage.** A list keeps its values and their type tags in one block.
  A list made with a capacity of up to 24 has it inside its own allocation, so
  `lnew R0, 4` is one allocation; any other list, or one that outgrows that,
  has a buffer, carved from separate chunks of the same kind while it is small
  (up to 28 values), so making or growing a short list calls no `malloc`
  either. Storage inside the list took 2–4% off compiled Lua's `binarytrees`,
  whose inner nodes are a table and a list of two. With a
  buffer each for values and tags from `malloc`, and the sweep following a
  list, compiled Lua's `methods` spent two to three times as long collecting
  and took 6–14% longer, and a program that makes 20 million short lists
  alongside 200,000 long-lived ones took 1.8–2.4 times as long.
* **Chunks** start at 1 KiB and double up to 64 KiB, so ten thousand small
  processes stay small.
* **Small processes.** A process that is waiting in `recv` takes about 4.3 KB
  (100,000 of them, measured on the M5): its `Process` record (3 KB), room for
  8 frames and 64 register words, which double when a call needs more, and no
  data stack until something uses one, when its 1 MiB (by default) is
  allocated. Coroutines' data stacks are allocated the same way, at first use.
  With the stacks allocated up front, the same process took 23 KB.
* **No sharing.** A process's objects never move to another heap: messages are
  copied, and only the copies (built in a scratch heap that uses `malloc`) are
  adopted by the receiver. So a process's chunks are simply freed with it. The
  object header records which allocator an object came from.
* **Zeroing** a new object uses one fixed-size `memset` per size class, which
  the compiler inlines into stores.

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
12% of `binarytrees` and 4% of `maps`, the most allocation-heavy benchmarks,
and 20–29% of compiled Lua's `binarytrees`, whose inner nodes are two
allocations each (a table, and a list that holds its storage).

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
* **One thread woken at a time.** While a woken thread is on its way, more
  ready processes wake no one else; that thread takes them, and if it steals
  more than it can run it wakes the next (as Go's scheduler limits its
  spinning threads). A process spawning a million processes that each wait
  for a message, then waking them all, takes 0.9 s on one thread and 1.7 s on
  18; with a signal for every ready process it took 6.5 s on 18.
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

* **Calls without copying.** A call costs about as much as in Lua 5.5, but
  still copies its arguments (a word each): Lua's callee window starts at the
  caller's argument registers, so a call copies nothing. Overlapping windows
  would need compilers to put arguments at the end of the caller's window, and
  a register model in which banks overlap too, so this waits until call-heavy
  programs show the copies.
* **Fewer operands per instruction.** Measured, and rejected. Storing the
  operands inside each instruction, as a fixed 3-operand format would, was no
  faster, and fixed-format calls make `fib` 13–17% slower. See
  [REGISTER_VS_STACK.md](REGISTER_VS_STACK.md#part-4-up-to-32-operands-against-a-fixed-3).
* **A compact decoded form for everything.** Quickened instructions carry a
  compact copy of their operands, but every instruction still keeps its 72-byte
  `Operand`s for the generic path, the debugger and the tools. The benchmarks fit
  in cache, so this does not show yet. Shrinking `Operand` is the fix if large
  programs show cache misses.
* **A generational GC.** Built and measured, and not kept: a non-moving
  collector with an old generation, a write barrier on every reference store
  into an object, and a remembered set of old objects written since the last
  collection. On compiled Lua's `binarytrees`, where collection is 20% of the
  time, it saved at most 27% of that, with an 8 MB young generation: the trees
  that program builds outgrow any small one, so they are promoted and die old,
  and most of a collection's cost is freeing the dead, which a generational
  collector does too. With a 1 MB young generation it was 2.4 times slower on
  a large map written now and then, which it rescanned whole at every young
  collection; remembering written slots instead of objects (card marking)
  would fix that at more cost again.
* **More specialised shapes.** Arithmetic directly on elements and fields
  (`nbody`) and raw memory (`sieve`) take the generic path. Rewriting `nbody` to
  load into registers, compute there and store back takes it from 96 ms to
  24 ms, so compilers can already get most of that speed. The shapes worth a
  form of their own are the ones compiled programs run most: the profiler's
  list of instructions on the general path shows them, as it showed the ones
  added for compiled Lox. What remains there for Lox is allocation (`new`,
  `mnew`, `mset`), which a fast form must not do.
* **Lock-free run queues** (Chase–Lev deques). Each queue's lock is almost
  always uncontended, because a thread mostly uses its own queue. Spawning a
  million processes from one is still twice as slow on 18 threads as on one,
  partly from threads stealing from the spawner's queue while it pushes;
  revisit if real programs show queue locks.
* **Spinning before sleeping.** Go's scheduler spins briefly before parking a
  thread, to cut wake-up latency. Here a thread is woken only when there is a
  backlog, which avoided the problem in every benchmark so far.
* **Per-thread timers.** The timer heap is shared, but threads read it only when
  the earliest timer is due (one atomic load per slice).
