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
| `nodes/` | calls and messages between two VMs over TCP (`bench/nodes/run.sh`; [below](#between-nodes)) |

## Results

Wall time in milliseconds, best of five, with the default of one worker thread
per core:

| Benchmark | Apple M5 Pro, Clang | Intel i7-11700KF, GCC | AMD Ryzen 9 5900X, Clang |
|---|---|---|---|
| fib | 42 | 80 | 74 |
| loop | 82 | 145 | 128 |
| sieve | 184 | 297 | 311 |
| nbody | 104 | 161 | 173 |
| binarytrees | 79 | 137 | 142 |
| maps | 26 | 54 | 63 |
| pingpong | 36 | 93 | 76 |
| closures | 87 | 155 | 158 |
| parallel | 17 | 40 | 29 |
| connections | 989 | 457 | 765 |

On Windows (GitHub's runners, in CI), `connections` takes 2 to 5 s on x86-64
and arm64 (the runners vary). Sockets are waited for on an I/O completion port,
so the 20,000 waiting ends cost nothing until one is ready; asking `WSAPoll`
about all of them at each wait takes 175 to 320 s.

The Mac is a laptop with 6 performance and 12 efficiency cores, running macOS.
The two desktops run Ubuntu 26.04: the Intel one has 8 cores (16 threads), the
AMD one 12 (24 threads). All builds use `-O2`.

With tail-call dispatch, a change to some handlers leaves the others' code
alone, and builds differ by a few percent at most (item 3 below); the
dispatch loop that other compilers get is more sensitive, by 5% or more.

The tight loop runs at about 2.2G instructions per second on the Mac, about two
clock cycles per instruction. `maps` and `pingpong` spend their time in natives
(strings, hashing, message copying) and in the scheduler rather than in the
interpreter loop.

### Compiled Lox

The benchmarks of *Crafting Interpreters*, compiled by the Lox compiler in the
`mlws-compilers` project, which keeps each dynamically typed value in an F and
an R register and caches method lookups at each call site. Milliseconds of wall
time, best of five:

| Benchmark | Apple M5 Pro | Intel i7, GCC | AMD Ryzen, Clang |
|---|---|---|---|
| fib | 277 | 453 | 429 |
| binary_trees | 696 | 1,171 | 1,136 |
| trees | 1,741 | 3,095 | 2,986 |
| instantiation | 289 | 473 | 435 |
| invocation | 242 | 457 | 435 |
| method_call | 145 | 246 | 235 |
| properties | 341 | 638 | 583 |
| zoo | 265 | 447 | 425 |

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
benchmarks, as whole runs (start-up included, with the module cache holding the
assembled program: [TOOLING.md](TOOLING.md#module-cache)), in milliseconds, the
median of five, interpreted and with the JIT, against the reference
interpreter running the same source:

| Benchmark | MLWSVM, M5 | JIT, M5 | Lua 5.5, M5 | MLWSVM, i7 | JIT, i7 | Lua 5.5, i7 |
|---|---|---|---|---|---|---|
| loop | 187 | 43 | 221 | 332 | 75 | 289 |
| iterate | 72 | 31 | 60 | 109 | 47 | 79 |
| fib | 55 | 27 | 57 | 93 | 42 | 101 |
| methods | 514 | 207 | 351 | 880 | 339 | 576 |
| strings | 189 | 103 | 137 | 332 | 193 | 212 |
| binarytrees (15) | 321 | 162 | 399 | 537 | 260 | 582 |
| nbody (500,000) | 853 | 301 | 404 | 1,301 | 714 | 595 |
| spectralnorm (500) | 206 | 70 | 184 | 376 | 110 | 267 |
| fannkuch (10) | 1,750 | 551 | 824 | 2,604 | 1,146 | 1,164 |

Interpreted, `loop`, `fib` and `binarytrees` take less time than in Lua on
the M5, and `fib` and `binarytrees` on the i7; with the JIT, all nine take
less time than in Lua on the M5, and all but `nbody` on the i7. Without
start-up, interpreted `fib` runs in 44 ms on the M5, faster than Lua: its
typed copy executes fewer instructions than Lua's bytecode does. Counted with Lua's
own instruction hook, the others execute 1.7 to 5.2 times as many, and each
costs less: 0.5–0.9 ns on the M5 in the loop-heavy benchmarks, against Lua's
1.0–1.5 ns. What the
generated code still spends is instruction count (type guards on values of
unknown type, two-word table values, inline-cache guards), allocation, and,
in `strings` and `iterate`, library functions that run as VM code where
Lua's are in C.

With the optimizer ([OPTIMIZER.md](OPTIMIZER.md#performance)), the same
benchmarks take up to 14% less time at `--opt 2` (`loop`), and at
`--opt 3`, where small functions are inlined, `spectralnorm` takes 33% less
and `iterate` 12% less. The rest gain up to 6%. Code that spends its time in
calls through closures, allocation and table checks has less for it to
remove.

### Compiled Erlang

The Erlang compiler in `mlws-compilers`, also in development, makes each
Erlang process an MLWSVM process and keeps each value in an `I` and an `R`
register: a small integer in `I` with a null `R`, a float as its bits in `I`
with a marker in `R`, or an object in `R`. Tuples are lists. Its benchmarks
(`make bench` there), as whole runs in seconds, the median of five on the M5,
against Erlang/OTP 29 with its JIT running the same source (start-up
included on both sides; Erlang's is about 0.08 s):

| Benchmark | MLWSVM | MLWSVM with its JIT | Erlang/OTP 29 |
|---|---|---|---|
| fib | 0.04 | 0.02 | 0.09 |
| ring | 0.14 | 0.13 | 0.27 |
| binarytrees | 0.52 | 0.18 | 0.13 |
| nbody | 0.47 | 0.17 | 0.15 |
| sorting | 0.36 | 0.21 | 0.12 |

BEAM compiles to machine code here, so its loops are not a fair match for an
interpreter. `ring`, which passes a message round a ring of a thousand
processes a thousand times, measures the scheduler and mailboxes more than
code; most of what MLWSVM runs there is the compiler's selective receive,
written in VM code around `recv`. In `nbody` every float operation unpacks
and repacks a float's bits.

### Against other virtual machines

The same `fib` and `loop` programs on Lua 5.5, the JVM and CPython
(`bench/compare/`, milliseconds, best of five, each VM's start-up time
subtracted):

| | `fib` (Mac) | `loop` (Mac) | `fib` (Intel) | `loop` (Intel) |
|---|---|---|---|---|
| MLWSVM | **42** | **83** | **76** | **143** |
| Lua 5.5 | 51 | 213 | 100 | 288 |
| MLWSVM with its JIT (`--jit on`) | 15 | 16 | 26 | 28 |
| JVM, interpreter only (`-Xint`) | 92 | 187 | 104 | 396 |
| CPython | 130 | 1,195 | 186 | 2,379 |
| JVM with its JIT compiler | 3 | 16 | 8 | 32 |

MLWSVM is the fastest of these interpreters on both programs. It runs the
loop 2.6× faster than Lua and 2.3× faster than the JVM's interpreter on the
Mac, and `fib` 1.2× faster than Lua there and 1.3× faster on the Intel
machine: a call costs about 6 ns on the Mac, against Lua's 7. With its JIT
(the medians of the next section), MLWSVM takes about a third of the time
on `fib` and a fifth on the loop, where it is as fast as the JVM's JIT on
the Mac. The JVM's JIT row is there for scale: an optimizing compiler removes
the loop's work and inlines `fib`'s calls.
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

### Between nodes

`bench/nodes/run.sh` runs two VMs on one machine, connected over TCP on the
loopback interface, and times calls (`proc.call`, one after another) and
messages (`send`, all of them, then a call to see that they arrived), of a
few shapes: `[1]`, a list of 100 integers, a map of 20 string keys, and a
list of 100 cells `[head, tail]`, as a compiled language's cons list. Each
message is encoded where it is sent and written by the connection's process,
with those waiting behind it, in one write; it is decoded where it arrives
straight into the message the receiver gets. Nanoseconds a call or a message,
the better of two runs; `nodes.erl` is the same for Erlang/OTP 29's
distribution. Messages, in 0.21.0 and 0.22.0:

| | M5: 0.21.0 | M5: 0.22.0 | M5: Erlang | i7: 0.21.0 | i7: 0.22.0 | Ryzen: 0.21.0 | Ryzen: 0.22.0 |
|---|---|---|---|---|---|---|---|
| send `[1]` | 1,740 | 241 | 1,820 | 2,440 | 657 | 4,460 | 677 |
| send 100 integers | 4,380 | 948 | 1,820 | 7,460 | 2,010 | 9,760 | 1,890 |
| send a map of 20 | 2,970 | 763 | 1,800 | 6,330 | 1,930 | 8,550 | 2,350 |
| send 100 cells | 8,500 | 2,640 | 1,820 | 20,200 | 6,320 | 20,500 | 7,930 |

Messages are 3 to 7 times faster than in 0.21.0, in which each was copied as
objects to the connection's process, encoded there and written alone, and
copied again where it arrived.

A call is a round trip over TCP: on the M5, a bare ping-pong over the
loopback interface, in C, takes 13,100 ns, and Erlang's call 20,200. In
0.22.0 MLWSVM's took about 23,000, since its poller was a thread of its own,
which woke a worker to run the connection's reader at each end. Now an idle
worker waits in the poller itself, as Erlang's schedulers do, and runs the
reader it wakes. Calls, against 0.22.0, the better of two runs, both in one
session:

| | M5: 0.22.0 | M5: now | i7: 0.22.0 | i7: now | Ryzen: 0.22.0 | Ryzen: now |
|---|---|---|---|---|---|---|
| call `[1]` | 22,800 | 18,800 | 22,300 | 12,900 | 42,800 | 19,700 |
| call 100 integers | 28,900 | 22,000 | 33,200 | 18,000 | 50,100 | 25,200 |

On the M5 a call now adds 5,700 ns to the round trip, where Erlang adds
7,100. Messages are as they were, within the runs' noise (on the i7, small
sends' medians of four runs were 5% slower, within a spread of ±15%).
`bench/connections` is 6% faster on the i7 and 13% on the Ryzen, and
`pingpong` and `parallel`, which wait for no descriptor, are unchanged.

A message to a name on the other node (`node.name_pid`, resolved where it
arrives) costs what one to a pid does: on the M5, 259 ns against 245.

## The JIT

With `--jit on` ([JIT.md](JIT.md)), hot functions run as machine code. The
table gives each program's time on the M5 without and with it, and the
change it makes on each machine: the same build both ways, medians of five
interleaved runs. The Erlang, Lua and Lox programs are the compilers'
benchmarks of the sections above.

| Program | M5, ms (interpreter → JIT) | M5 | i7, GCC | Ryzen, Clang |
|---|---|---|---|---|
| bench/fib | 42 → 15 | −64% | −69% | −62% |
| bench/loop | 83 → 16 | −81% | −81% | −75% |
| bench/nbody | 104 → 10 | −91% | −91% | −92% |
| bench/sieve | 186 → 73 | −61% | −53% | −63% |
| bench/closures | 88 → 13 | −85% | −84% | −85% |
| bench/maps | 27 → 26 | −2% | −4% | −4% |
| bench/binarytrees | 79 → 35 | −55% | −53% | −46% |
| bench/parallel | 17 → 3.4 | −80% | −81% | −70% |
| Erlang fib | 31 → 9.5 | −69% | −69% | −67% |
| Erlang ring | 127 → 117 | −8% | −4% | −5% |
| Erlang binarytrees | 511 → 172 | −66% | −65% | −62% |
| Erlang nbody | 462 → 165 | −64% | −61% | −65% |
| Erlang sorting | 347 → 197 | −43% | −41% | −38% |
| Lua fib | 44 → 16 | −64% | −66% | −62% |
| Lua loop | 175 → 31 | −82% | −81% | −79% |
| Lua methods | 501 → 195 | −61% | −62% | −55% |
| Lua strings | 176 → 89 | −49% | −44% | −44% |
| Lua iterate | 60 → 19 | −68% | −67% | −72% |
| Lua binarytrees | 308 → 149 | −52% | −54% | −50% |
| Lua nbody | 835 → 285 | −66% | −46% | −66% |
| Lua spectralnorm | 193 → 57 | −70% | −73% | −75% |
| Lua fannkuch | 1737 → 538 | −69% | −56% | −73% |
| Lox binary_trees | 696 → 273 | −61% | −62% | −56% |
| Lox equality | 129 → 63 | −51% | −63% | −59% |
| Lox fib | 278 → 125 | −55% | −68% | −63% |
| Lox instantiation | 290 → 189 | −35% | −34% | −20% |
| Lox invocation | 245 → 61 | −75% | −68% | −52% |
| Lox method_call | 146 → 62 | −58% | −56% | −38% |
| Lox properties | 343 → 109 | −68% | −64% | −51% |
| Lox string_equality | 1.4 → 0.8 | −41% | −52% | −40% |
| Lox trees | 1746 → 576 | −67% | −65% | −55% |
| Lox zoo | 267 → 80 | −70% | −68% | −56% |

Calls and loops gain the most: the machine code has no dispatch, keeps a
block's registers in machine registers, and a loop's across iterations (with
up to two calls inside it on arm64), and pairs its calls and returns for the
CPU's return predictor. A closure call is compiled for the function it
called last ([JIT.md](JIT.md#how-it-runs-with-the-interpreter)), so method
calls and callbacks cost a direct call: that is most of what `closures`,
Lua's `methods` and the Lox programs gain. Field, element and capture access gain less, since
each still checks its object's type and bounds, as the interpreter does,
though not again for an object the block has checked.

`bench/nbody` computes on elements in place, which the interpreter has no
fast form for and the JIT compiles, and `sieve`'s bytes of raw memory are
compiled too. Where the time is not the interpreter's, the JIT has nothing
to remove: `maps` is natives, and `pingpong` and Erlang's `ring` are
messages and the scheduler.
On x86-64 the machine code is about as long as on arm64 (compiled Lua's
`nbody` reads a field in 35 and 39 instructions where arm64 takes 35 and
38), but the i7 runs about half as many instructions a cycle as the M5:
counted with Valgrind's callgrind, 11–17 billion a second in compiled Lua's
`nbody`, `fannkuch` and `methods`, and the same counts in the M5's times
make 28–33 billion. That is
most of why the Lox and Lua programs gain less there.

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
   `methods`. A seventh, from the Erlang compiler's profiles: `bitcast`
   between an `I` and an `F` register (the compiler keeps a float's bits in
   an `I` register), `typeof` of a register, `beq.r`/`bne.r` of an element at
   a constant index against a constant (a tuple's tag), and direct tail calls
   with registers and constants as arguments. These took 29–39% off its
   `binarytrees` and `nbody` and 15–21% off `sorting`. An eighth, for text:
   `strcat` and `bufcat` of registers and string constants, `tostr` of an
   `I` or an `F` register, `substr` of registers, `bufnew` of a constant and
   `bufpush` of a register, each a check of its operands and a call of C. A
   ninth: `mnew`, and `mset` and `mhas` of a key in a register or a string
   constant, and bytes of raw memory at a register index, loaded, stored and
   branched on (`mov.u8 I0, P0{I1}`, `bnz.u8 P0{I1}, L`), which took
   70% off `sieve` on all three machines.
   The profiler lists the
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
   except those that allocate (`new`, `lnew` and the string builders' forms:
   `strcat`, `bufcat`, `tostr`, `substr`, `bufnew`, `bufpush`), which save the
   `pc` first, since an allocation can reach the heap quota. An
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
   (`calli`, `tcalli`) work alike. The first function a call site calls has
   its parameter and result types checked once, and is kept in the plan with
   its argument slots, so a later call to it checks only that the closure is
   one of it; a call to any other function checks the types each time.
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

10. **Functions aligned to 64 bytes** (`-falign-functions=64`). Each handler
   is a function of its own, and without alignment a change to one moves the
   others across cache lines: unrelated programs took up to 5% more or less
   time from one build to the next, most on the Ryzen. Aligned, they stay
   within about 2%, and some run up to 10% faster.

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

Every object up to 256 bytes comes from its process's own chunks, by bumping
a pointer:

* **Allocation** takes the next bytes of the current hole, a run of 128-byte
  lines that no object surviving the last collection is on. The hole is
  zeroed 2 KiB at a time ahead of the allocator, so an object needs no
  `memset` of its own. There is no lock, no `malloc` call and no free list.
  The fast path is inline wherever an object is made, and the JIT emits it
  for `new` and for `lnew` of a list whose storage is inside it, keeping its
  registers in machine registers (a stub out of the way writes them back
  and calls C when the hole is used up): one compare, the bump, the header
  and two counts. The compare is against `bump_lim`, which the slow path
  keeps at the nearer of the end of the hole's zeroed part and the point
  where the heap reaches one limit, both the next collection's threshold and
  the heap quota. Allocating 50 million two-field structs, nothing kept,
  takes 146 ms with the JIT on the M5, against 369 ms with the free lists
  this replaced.
* **The dead are never visited.** A collection marks what survives and the
  lines it lies on; every other line is free for the next cycle. Only the
  objects that own memory besides themselves (byte buffers, maps, lists
  whose storage is a buffer, coroutines, foreign handles) are on lists of
  their own, which a collection walks to free what the dead ones own. With
  nothing kept, a program that allocates 1.6 GB spends 1.4 ms in 20,833
  collections, against 81 ms when each dead object went back on a free list.
* **List storage.** A list keeps its values and their type tags in one block.
  A list made with a capacity of up to 24 has it inside its own allocation, so
  `lnew R0, 4` is one allocation; any other list, or one that outgrows that,
  has a buffer, carved from separate chunks with a free list per size class
  while it is small (up to 28 values), so making or growing a short list
  calls no `malloc` either.
* **Chunks** start at 1 KiB and double up to 64 KiB, so ten thousand small
  processes stay small. Lines are 128 bytes: 64-byte lines made more, smaller
  holes and more refills, and 256-byte lines gained no more than noise. The
  price of not moving objects is that a line stays in use while any object
  on it lives (until the next collection of the whole heap, for an old one);
  the peak memory footprint of compiled Erlang's `binarytrees` and `sorting`
  is no larger than with the free lists. `--stats` shows the chunks' size
  and how many of their lines were in use at the last collection.
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

`MLWSVM_NO_SLABS=1` uses `malloc` for every object instead, for comparisons:

| Benchmark | Mac | Intel | AMD |
|---|---|---|---|
| binarytrees, with the allocator / with `malloc` | 79 / 103 | 144 / 222 | 144 / 219 |
| maps, with the allocator / with `malloc` | 34 / 42 | 68 / 111 | 78 / 116 |

macOS gains less because its own allocator's small-object zone is already very
fast. AddressSanitizer builds always use `malloc`, so that it sees each
object's lifetime. Under `MLWSVM_GC_STRESS=1` (`make check-gc`), freed objects
are filled with a garbage pattern, so a use after a free fails loudly there too.

### The collector

The collector is a non-moving, generational mark-region collector, one per
process ([DESIGN.md §11](DESIGN.md#11-the-heap-and-the-collector)). An object
that survives a collection is old. A minor collection marks only the young
objects, from the roots and from the old objects written since the last
collection (the remembered set); it walks neither the old objects nor the
dead young ones. A collection of the whole heap runs when the old objects
have doubled.

Most of a collection's time used to be re-marking what long-lived data a
program keeps, at every collection, and then visiting every dead object to
free it. Generations removed the first: against the collector that collected
the whole heap each time, with the JIT, on the M5 / i7 / Ryzen:

| Program | M5 | i7, GCC | Ryzen, Clang |
|---|---|---|---|
| Lua binarytrees | −25% | −39% | −28% |
| Erlang sorting | −9% | −21% | −19% |
| Erlang binarytrees | −12% | −24% | −13% |
| Lox binary_trees | −14% | −20% | −10% |
| bench/binarytrees | +1% | −13% | −12% |

Lines and bump allocation removed the second, and most of allocation's own
cost. Against the free lists and the sweep of the young they replaced, with
the JIT:

| Program | M5 | i7, GCC | Ryzen, Clang |
|---|---|---|---|
| Lua binarytrees | −25% | −36% | −34% |
| Lox instantiation | −28% | −29% | −37% |
| Lox binary_trees | −13% | −19% | −17% |
| Erlang sorting | −9% | −15% | −13% |
| Erlang binarytrees | −13% | −14% | −12% |
| Erlang nbody | −11% | −10% | −13% |
| Lua methods | −6% | −12% | −15% |
| bench/binarytrees | −11% | −15% | −14% |
| bench/maps | −12% | −12% | −7% |

Interpreted, they gain about two-thirds as much (Lua `binarytrees` −14% /
−24% / −19%, Lox `instantiation` −18% / −30% / −33%). Programs that allocate
little do not change, beyond the code-layout effects every change to the
interpreter has on the i7 (compiled Lua's `loop`, which allocates nothing,
runs 20% slower interpreted there, and as fast as before, or faster, with
the `Process` record 64, 192 or 512 bytes larger). Erlang's `ring` pays for the barrier on its
mailbox, up to 4% against no generations.

* **The young generation** starts at `--gc-min` (64 KB) per process, so a
  process that keeps little stays small and in the caches (Erlang's `ring`,
  1,000 processes, takes 10% less time than with 1 MB), and doubles, up to
  16 MB, while a
  minor collection promotes more than an eighth of it: a program that builds
  structures larger than the young generation (`binarytrees`' trees) would
  otherwise promote them, and they would die old. It also grows to the size
  of the old objects a minor collection had to rescan (a large map written
  now and then is rescanned whole), up to the old generation's size, so such
  a program is rescanned no more often than the whole heap was once.
* **The write barrier.** A store of a reference into an existing object
  checks the object first (most stores are into young ones) and then the
  value: a young value into an old object not yet remembered remembers it.
  The interpreter's store forms do the check inline and go to a handler of
  their own to remember; the JIT's machine code skips it for an object made
  in the same block, and otherwise leaves for the interpreter, once per
  object between collections. Stores of null, numbers and constants never
  remember anything.
* **Leaves are not scanned.** An object with no references in it (a
  string, a number, an array of numbers, an empty list or map, a struct
  whose type has no reference field) is marked without going on the mark
  stack, and a struct's fields are scanned by its type's bit mask of
  reference fields.
* **The order of the chunks.** After a minor collection the allocator goes
  first through the chunks it used last, the most recent first, whose memory
  is the likeliest to be in the caches.
* **A collection that is due** runs at the next safepoint, up to a time slice
  later. Until then the fast path checks only the heap quota: otherwise every
  allocation in between would take the slow path.

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
  collector needs no locks. A string of 4 KiB or more is the exception: it leaves
  every heap and goes by reference, so sending one costs the same at any size, with
  a count touched only as a heap gains or loses it: about 80 ns a send on the M5,
  whether the string is 5 KB, 1 MB or 20 MB.
* **Cheap slices.** A time slice is 2,000 instructions, or fewer if the process
  blocks. The scheduler reads the clock once per slice, and the short critical
  sections use `os_unfair_lock` on macOS rather than `pthread_mutex`. Both
  matter in message-heavy code, where a slice may be only a few instructions
  long: with three clock reads per slice and plain mutexes, `pingpong` took
  twice as long. A slice that is used up, when nothing else wants its
  worker (no other process in its queue, no timer, no process waiting for
  I/O, no stop of the world, nothing due in the process itself), goes
  straight on into the next, up to 64 in a row, after a handful of relaxed
  atomic loads. Each worker counts its slices itself, so they share no
  counter.

`parallel` on the Mac with different thread counts (best of six):

| Threads | 1 | 2 | 4 | 6 | 8 | 12 | 16 | 18 |
|---|---|---|---|---|---|---|---|---|
| ms | 190 | 97 | 50 | 34 | 30 | 23 | 17 | 17 |
| Speed-up | 1.0× | 2.0× | 3.8× | 5.6× | 6.4× | 8.4× | 11.5× | 11.3× |

Scaling is close to linear up to the six performance cores. The efficiency cores
add less after that, and nothing past sixteen threads, one for each of the
sixteen processes.

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
* **Card marking.** A remembered object is rescanned whole at the next
  minor collection; remembering the slots written instead would make a large
  container written now and then cheaper still. The young generation's
  growth bounds what it costs today.
* **More specialised shapes.** Arithmetic directly on elements and fields
  (`nbody`) takes the generic path in the interpreter, though the JIT
  compiles it for `f64` elements (`nbody` takes 10 ms with the JIT on the M5). Rewriting `nbody` to load into
  registers, compute there and store back takes the interpreter from 96 ms
  to 24 ms, so compilers can already get most of that speed. The shapes
  worth a form of their own are the ones compiled programs run most: the
  profiler's list of instructions on the general path shows them, as it
  showed the ones added for compiled Lox.
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
