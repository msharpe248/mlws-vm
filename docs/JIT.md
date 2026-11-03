# The JIT

MLWSVM's JIT turns a function's quickened instructions into machine code,
for arm64 and x86-64, and runs it in mixed mode with the interpreter: the
machine code works on the same register windows, frames and heap as the
interpreter, and hands back to it at any instruction it does not compile.
A program's results, errors, instruction counts and preemption are the same
with it as without it. It removes the interpreter's dispatch, and keeps a
block's registers, and a loop's, in machine registers: programs whose time
goes to dispatch (calls, loops, field and element access) take 20–78% less
time.

```
mlwsvm run --jit on prog.masm              # compile each function once it is hot
mlwsvm run --jit on --jit-report - prog.masm   # and say what it did
```

The JIT is off unless asked for. It is also `MLWSVM_JIT=on`, `; option:
--jit on` in a test, and `mlws_set_option(vm, "jit", "on")` for a host
([EMBEDDING.md](EMBEDDING.md)).

## Options

| Option | Default | Meaning |
|---|---|---|
| `--jit off\|on\|all` | `off` | `on`: compile each function once it is hot. `all`: compile every function as it is loaded, and enter its machine code at every block, which is slower but runs every compiled instruction (for testing). `0` and `1` mean off and on. |
| `--jit-hot N` | 1000 | with `--jit on`, how many calls and loop iterations make a function hot |
| `--jit-report FILE\|-` | (none) | when the run ends, write what the JIT did to FILE (`-`: stderr) |

`MLWSVM_JIT_DUMP=FILE` writes each piece of machine code as `.byte` lines,
to check with an assembler and a disassembler (`cc -c FILE.s && objdump -d
FILE.o`).

## When a function is compiled

With `--jit on`, a function's first instruction and the head of each of its
loops count each time they run. When the count reaches `--jit-hot`, the
function is compiled, and its next block start goes into the machine code:
a loop that made its function hot goes on in the machine code at its next
iteration, since the interpreter's frame and registers are the machine
code's too. A function that never gets hot is never compiled, so a large
program starts as fast as without the JIT: compiled Lua's `nbody` (a 1.1 MB
file of 335 functions, most of them the Lua runtime's) compiles 5 of them,
in 2 ms on the M5. Compiling every function (`--jit all`) takes 6 ms and 4 MB
of machine code.

Compiling takes a lock of the VM's; other worker threads go on running the
function in the interpreter meanwhile, and in the machine code once it is
installed.

## What it compiles

The JIT compiles the quickened forms ([PERFORMANCE.md](PERFORMANCE.md#how-the-interpreter-is-made-fast)):

* moves, integer and float arithmetic, shifts, division and remainder by a
  constant, sqrt, neg and abs, conversions and bit casts;
* compares into a register and compare-and-branch, signed, unsigned and
  float, `jmp`, `jtab`;
* calls, closure calls, returns and tail calls;
* struct fields, list and array elements, bytes of strings and byte
  buffers, globals, closure captures, `len`,
  constants stored to fields, elements and globals,
  `new` and `lnew` (for a struct, and for a list whose storage is inside
  it, the allocator's pointer bump in machine code: DESIGN.md §11.2),
  `lpush` into spare capacity, `mget` of a constant key or a key in a
  register,
  `ldv`, `typeof`, `isa`, `bisa`, `bnisa`, and a tuple's tag test.

It also compiles a few generic instructions that have no quickened form,
from their operands: `i64` and `u64` add, sub, mul, and, or, xor and
compares into a register, of I registers, constants and closure captures.

Each form does what its handler's fast path does, with the same checks.
Whatever the handler hands to the generic instruction (a struct of another
type, an index out of range, an element or capture of another type, a list
without room, a map's `len`) is a *side exit*: the machine code writes back
the registers it changed, and stops before the instruction for the
interpreter to run it.

Any other instruction (a native call, a string operation, a coroutine
instruction, a throw) is *stepped*: the machine code calls the
instruction's own handler and goes on after it, in the machine code of
wherever the instruction went (the next instruction, a branch's target, a
callee). `yield`, `sleep` and `break` stop for the interpreter instead.

The report lists the forms stepped most and the instructions the
interpreter took over at most, which is how a form worth compiling shows
up.

## How it runs with the interpreter

**Blocks.** A function's compiled instructions fall into basic blocks. The
first instruction of a block where entering pays gets the execution opcode
`Q_JIT` (the instruction's own form is kept aside), whose handler enters the
machine code there. Entering and leaving costs about as much as 20
interpreted instructions, and so does a step, so a block is entered only if
what runs from it before the interpreter must take over has a loop or a
call, or is long, and steps rarely. (`--jit all` enters at every block.)

**The slice.** Each block's code starts by taking its length from the
slice's count, or, if the slice has less left, by stopping before its first
instruction for the interpreter. So `Process.insns`, instruction quotas and
preemption are exactly the interpreter's, and a slice can end in the middle
of a loop as it would without the JIT.

**Calls.** A call from machine code pushes the frame as the interpreter's
call does and goes to the callee's machine code, or to the interpreter at
its first instruction if it has none. Its continuation follows it in the
machine code. A return to a call made from machine code goes to the
continuation; any other return is the interpreter's. Calls and returns are
paired machine calls and returns, so the CPU's return predictor follows
them. On arm64 nothing is pushed; on x86-64 a call pushes its return
address, and a return checks it against the frame's continuation. A call
stops for the interpreter when the machine stack is near 256 KB deep, and
the interpreter grows the frame and register stacks when they are full.

**Closure calls.** A closure call (`calli`) or closure tail call (`tcalli`)
records, in its call site, the function it called last. Compiled once hot,
it is compiled for that function, if the call suits it (its arguments and
results fit the function's parameters and results): the machine code checks
that the value called is a closure of that function, and then calls it as a
direct call does, with the closure in the frame; anything else (a closure of
another function, something that is not a closure) takes the general
closure call, which checks the callee's parameters at each call. Both go on
at the same machine call, so the continuation is the same either way. A
call site that calls one function from many closures (a method, a callback)
is the common case: on the M5, compiled Lox's `invocation` and `properties`
take 56% and 36% less time than with the general call, and Lua's `methods`
31% less. With
`--jit all` every function is compiled before any call has run, so nothing
is compiled for a callee.

**Registers.** Inside a block, I and F registers live in machine registers
once read or written, and those changed are written back at the block's
end: 16 general and 16 float registers on arm64, 5 and 13 on x86-64. On
arm64, an R register read twice in a block also stays in a general register
that is free: it is written to the window as soon as it is set, where the
collector sees it, so it is never written back. What a block has checked of
an R register's object (a struct of a type, or a list) holds until the
register is set again, so a second field or element of the same object
skips the checks.

**Loops.** A loop keeps its I and F registers in machine registers across
iterations when they fit: they are loaded on the way in and written back on
every way out (a branch out, a side exit, the slice's end). The loop is the
code from its head that reaches its branch back, so a cold path in its
range that leaves it (a type check's slow path to a return or an error) is
not part of it. A call or an instruction the interpreter runs inside it is
a *hole*: the registers are written back before it and loaded after it, as
they are on any way back in past the head (a slow path that rejoins the
loop, or the interpreter). A loop with more than 2 holes on arm64, or any on
x86-64, does not keep its registers: each hole costs a write-back and a
load of all of them.

**Tools.** The debugger, profiler and tracer see every instruction, so a run
under them compiles nothing.

## Memory

Each VM has its own code memory (64 MB of address space, reserved when it
first compiles something, and freed with the VM). Code is written, then
made executable, and never written again: on macOS on Apple silicon, the
memory is `MAP_JIT`, writable or executable for each thread in turn; on
Linux each piece of code has pages of its own, made executable once
written. If the memory runs out, later functions stay interpreted. A host
built with the hardened runtime needs the
`com.apple.security.cs.allow-jit` entitlement for `MAP_JIT`; without it the
JIT compiles nothing.

## Platforms

The JIT needs the tail-call interpreter (Clang, or GCC 15 and later) on
arm64 or x86-64: macOS and Linux on arm64, Linux and Windows on x86-64. On
Windows, the machine code is the same as on Linux: the C functions it calls
use System V's calling convention there too (Clang's `sysv_abi`). macOS on
x86-64 should work but is not tested. Elsewhere (other processors, Windows
on arm64, older GCC, ThreadSanitizer builds, which use the dispatch loop)
`--jit` is accepted and the program runs in the interpreter.

## The report

```
$ mlwsvm run --jit on --jit-report - nbody.masm 500000
...
mlwsvm jit (arm64): a function compiled after 1000 calls or loop iterations
  functions compiled  5
  instructions        6571 of 6745 compiled (97.4%)
  loops               0 with their registers in machine registers
  closure calls       0 of 0 compiled for the function they called last
  machine code        601076 bytes, in 1.89 ms
  entries             567441, 1130332729 instructions run in machine code (1992.0 per entry)
  stepped             2 instructions
  most stepped (forms the JIT does not compile):
               2  mget (form 95)
  most common stops (where the interpreter took over, a time slice's end included):
          108499  mov (form 214)
           90500  bne (form 226)
           ...
```

*Entries* counts the times the interpreter entered the machine code;
*stepped* the instructions it ran by their handlers. A stop is where the
machine code handed over: a side exit, an instruction it does not compile,
a return to the interpreter, or the end of a time slice (2,000 instructions
by default: about one per entry above). Entries, steps and stops are counted
only for a report.

## Testing

* `tests/jit/` checks the machine code against the interpreter: every form,
  with its side exits; calls, tail calls, closures, unwinding through
  machine-code frames and coroutines; loops with their registers in machine
  registers, entered from the interpreter in the middle; hot compiling;
  closure calls compiled for their callees, given closures of other
  functions and values that are not closures; a tail call made for callers
  that take its results differently;
  processes on four threads compiling the same functions; and an
  instruction quota stopping a process at the same instruction. Their
  expected output is the interpreter's.
* `; expect-jit: compiled FUNCTION N`, `; expect-jit: loops FUNCTION N`,
  `; expect-jit: speculated FUNCTION N` (closure calls compiled for a callee)
  and `; expect-jit: steps N` in a test check what the JIT did (where the
  build has one). A line ending with `arm64` or `x86-64` is checked only for that
  JIT.
* `make check-jit` runs every test and example with `--jit all` (also under
  GC stress and with every module optimized) and with `--jit on
  --jit-hot 2` (also on one worker thread). `make check-asan` runs the tests
  with `--jit all` and `--jit on --jit-hot 2` too. CI runs both on Linux (x86-64 and arm64) and
  macOS, and `check-jit` on Windows.
* The Brainfuck, Lox, Lua and Erlang compilers' suites pass with the JIT,
  plainly, under GC stress and at `-O3`.

## Performance

[PERFORMANCE.md](PERFORMANCE.md#the-jit) has the results on the three
machines, for the benchmarks and the compilers' programs. Calls (`fib`) take
52–66% less time, closure calls (`closures`, compiled Lox's `invocation`)
56–85% less, loops (`loop`, `fannkuch`, `spectralnorm`) 48–78% less, and
field and element access with method calls (`nbody`, `methods`, compiled
Lox's `trees`) 30–58% less. Where the time goes to natives (`maps`), raw memory (`sieve`)
or the scheduler (Erlang's `ring`), the JIT changes little.

## Design

The machine code is written directly, one encoder per processor
(`src/vm/jit_a64.inc`, `src/vm/jit_x64.inc`), behind one set of
operations (`jm_*`) that `src/vm/jit.inc` writes every form in, once. The
alternative was copy-and-patch (compiling the interpreter's handlers into
templates at build time), which keeps the machine code and the handlers
from drifting apart but needs Clang and an extractor of object-file
relocations in the build, and gives up the register cache, which is most of
the gain on loops. The tests guard against drift instead: every form runs
against the interpreter's results under `--jit all`, and so do the whole
suite and the compilers' suites.

What it does not do: it does not reorder or remove instructions (that is
the optimizer's job, [OPTIMIZER.md](OPTIMIZER.md)), speculate on types
(only on a closure call's callee), inline, or keep values in machine
registers across calls (a call in a loop is a hole). A function's
machine code is kept for the life of the VM.
