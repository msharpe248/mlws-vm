# 11. JIT 4: measuring and testing

A JIT is easy to get wrong in two ways. It can be slower than it should be,
in ways no one notices because the program still gives the right answer. And
it can give the wrong answer, but only for some programs, in some modes, on
some processors. This chapter is about catching both: how MLWSVM's JIT is
measured (timing, profiling machine code that has no symbols, counting
instructions), how to read the code it makes, and how it is tested against
the interpreter. It ends with two ideas that were measured this way and not
built.

## Timing: A/B, on three machines

The basic measurement is an A/B comparison: the same program, timed with two
builds or two settings, alternately, several times, keeping the best of each.
Alternating matters because a machine's speed drifts (temperature, other
work), and drift would otherwise land on one side. Taking the best rather
than the mean removes interruptions, which only ever make a run slower.

For the JIT itself, the two sides are one binary, with and without the JIT,
so nothing else differs. `MLWSVM_JIT=on` in the environment does what
`--jit on` does. [PERFORMANCE.md](../PERFORMANCE.md#the-jit) has the table
this gives for 31 programs, medians of five interleaved runs, on an Apple
M5, an Intel i7 (GCC) and an AMD Ryzen (Clang).

Three machines, because they disagree. Changes to the JIT have helped one
machine and hurt another often enough that every change is measured on all
three. The Ryzen showed a further effect: moving the machine code by a few
bytes, with no other change, moved some programs' times by up to 10%, since
alignment decides how the processor fetches and caches the code. A
regression seen only there is checked by padding the code by 0 to 56 bytes
and averaging over the placements. One change looked 8% slower on Lox's
`zoo` on the Ryzen; averaged over eight placements it was 0.1%.

## Profiling machine code

A sampling profiler interrupts the program many times a second and records
where it was. For C code it names the function from the binary's symbol
table. JIT code has no symbols: its addresses are in memory the JIT
allocated, and a profiler shows them as `???`.

`MLWSVM_JIT_MAP=FILE` fills that gap. As each function is compiled, it
writes where each compiled instruction's machine code starts:

```text
109790060 sum 0 mov
10979006c sum 1 mov
109790078 sum 2 add
109790090 sum 3 inc
109790094 sum 4 blt
1097900a4 sum 5 ret
1097900e4 sum stubs -
10979017c sum end -
```

That is address, function, VM instruction index and mnemonic, then where the
function's stubs start and where it ends. A sample at any address can be put
back on its VM instruction by finding the last line at or below it. Linux's
`perf` reads files of this kind for JIT code (`/tmp/perf-PID.map`), and
V8 and the JVM can write them.

On macOS, `sample PID` records call stacks; a short script sorts the map and
looks up each sampled address. That is how the work in chapter 10 was chosen.
In compiled Lua's `nbody`, 46% of the machine code's samples were loads and
stores of the register window, 25% tag tests and about 1% arithmetic. The
fallin blocks of chapter 10 were the answer to the first number.

## Counting instructions

Samples say where the time goes. They do not say how many instructions were
executed, or which ones. For that, a simulator that counts every instruction
is better, and the Linux machines here allow no hardware profiling (`perf`)
at all. Valgrind's callgrind runs the program on a simulated processor and
counts each machine instruction by address, JIT code included:

```sh
$ MLWSVM_JIT_MAP=nbody.map valgrind --tool=callgrind --dump-instr=yes \
    --compress-pos=no --compress-strings=no --smc-check=all \
    --callgrind-out-file=nbody.cg ./mlwsvm run --jit on lua_nbody.masm 100000
```

`--smc-check=all` tells Valgrind that code may be written while the program
runs; `--dump-instr=yes` keeps one count per instruction address. The output
lists `ADDRESS LINE COUNT` lines, and the JIT's code is in an object named
`???`. Joining them with the map:

```python
import re, sys, bisect, collections
cg, mapfile = sys.argv[1], sys.argv[2]
counts, ob, skip = collections.Counter(), None, False
for l in open(cg):
    if l.startswith('ob='): ob = l[3:].strip(); continue
    if l.startswith('calls='): skip = True; continue      # the next line is a call's cost
    m = re.match(r'^(0x[0-9a-f]+) \S+ (\d+)', l)
    if not m: continue
    if skip: skip = False; continue
    if ob and '???' in ob: counts[int(m.group(1), 16)] += int(m.group(2))
mp = sorted((int(a, 16), f, pc, op) for a, f, pc, op in (l.split() for l in open(mapfile)))
keys = [x[0] for x in mp]
per = collections.Counter()
for a, n in counts.items():
    k = bisect.bisect_right(keys, a) - 1
    if k >= 0: per[mp[k][1:]] += n
for (f, pc, op), n in per.most_common(20): print(f"{n:>14,}  {f} {pc} {op}")
```

The count at the first address of an instruction's code is how many times
that instruction ran, so the ratio gives machine instructions per execution.
Two results from it, both used in chapter 10:

* **A call cost 64 x86-64 instructions and a return 25–27.** In compiled
  Erlang's `binarytrees`, calls and returns were about half of all the
  machine instructions. That is what the cheaper calls of chapter 10
  addressed.
* **The x86-64 code is as long as the arm64 code.** The JIT gains less on
  x86-64 (compiled Lua's `nbody` takes 39% less time on the i7 and 59% less
  on the M5), and the obvious suspect was the x86-64 encoder. Counting
  showed the same number of machine instructions for each VM instruction on
  both. The i7 simply runs fewer of them per cycle: 11–17 billion a second
  against 28–33 billion on the M5, on the same programs. That is the
  processors, not the JIT.

## Reading the machine code

`MLWSVM_JIT_DUMP=FILE` writes every compiled function as `.byte` lines that
an assembler turns back into an object file a disassembler can read (chapter
8). Combined with the map, it shows the code for any instruction. This is
compiled Lua reading one field of a table (arm64, comments added): a check
of the table's shape against the call site's inline cache, the slot's index
from the cache, then the field's two words, as one pair (chapter 10).

```text
; bne.r R30, @ic$249s, ...   the table's shape against the cached shape (a global)
  ldr   w10, [x25, #0x68]     ; the process has this module's globals?
  cmp   w10, #0x0
  b.ls  ...
  ldr   x9, [x25, #0x60]      ; the segment, its tags...
  ldr   x10, [x9, #0x8]
  ldrb  w11, [x10, #0x22e]    ; ...the global's tag: a reference?
  cmp   w11, #0xd
  b.ne  ...
  ldr   x9, [x9]
  ldr   x10, [x9, #0x1170]    ; the global
  mov   x9, x2
  cmp   x9, x10
  b.ne  ...                   ; another shape: the slow path
; mov.i64 I31, @ic$249w      the slot's index (another global: the same checks)
  ...
; mov.r R19, R18[I31] + ldv I19, F19, R18[I31+1]   the field's two words
  subs  w23, w23, #0x3        ; a new block: the cache miss's path rejoins here
  ldr   x0, [x20, #0xf8]      ; I31 again, just stored
  ldr   x9, [x20, #0x288]     ; R18
  cbz   x9, ...               ; null?
  ldrb  w10, [x9]
  cmp   w10, #0x5             ; a list?
  ...                         ; bounds, the element's type tag, the load, then the same for i+1
```

Reading code like this is how you find what to change: here, each global
read rechecks the segment, and a label where the slow path rejoins starts a
new block, so a register stored one instruction earlier is loaded again.
It is also how you check a change did what you meant: after the cheaper
calls of chapter 10, the dumps showed the jump after each call gone.

## Testing: the same answer, every way

The JIT's correctness has one definition: a program gives the same result,
output, errors and instruction counts with the JIT as without it. Every test
of the JIT checks that, in four ways.

### Every test, in every JIT mode

`make check-jit` runs the whole test suite (chapter 12) with the JIT, several
ways (from the [Makefile](../../Makefile)):

```make
check-jit: $(BIN) test-ext
	MLWSVM_JIT=all ./$(BIN) test tests examples
	MLWSVM_TEST_TIMEOUT=$(STRESS_TIMEOUT) MLWSVM_JIT=all MLWSVM_GC_STRESS=1 ./$(BIN) test tests examples
	MLWSVM_JIT=all MLWSVM_OPT=all ./$(BIN) test tests examples
	MLWSVM_JIT=on MLWSVM_JIT_HOT=2 ./$(BIN) test tests examples
	MLWSVM_JIT=on MLWSVM_JIT_HOT=2 MLWSVM_THREADS=1 ./$(BIN) test tests examples
```

`all` compiles every function before it runs and enters the machine code at
every block, so every form is compiled and run, and every hand-over point is
exercised. `on` with a threshold of 2 compiles functions in the middle of
running them, which is what real programs do. Garbage-collection stress (a
collection at every allocation) checks that the machine code leaves every
reference where the collector can see it.

### Tests aimed at the JIT

[tests/jit/](../../tests/jit/) has a test for each part of the JIT, written to
reach its edge cases. Each lists what it covers. From `tests/jit/results.masm`,
which tests results returned in registers:

```asm
; ...
;   * one to seven results, of every bank, constants among them, from direct
;     calls, a closure call and a tail call;
;   * a window cleared after a call that left values in it: R registers, and
;     I and F registers read before they are written, near the window's start
;     and far enough into it that a pair store cannot reach them;
;   * a hot caller calling a function once, which has no machine code then.
; option: --jit on
; option: --jit-hot 20
```

Seven results, because only the first four go in registers. The far
registers, because the paired stores that clear a window on arm64 only reach
504 bytes. Each case is there because the code has a branch for it.

### Differential fuzzing

Hand-written tests only find the bugs someone thought of. `mlwsvm fuzz`
generates random programs, built to always end, that use every kind of
instruction, and runs each in the interpreter and in each other mode,
comparing everything they print and how they end:

```sh
$ ./mlwsvm fuzz --list
interp                               the interpreter, the reference
jit         --jit all                every function compiled as it loads, entered at every block
hot         --jit on --jit-hot 2     functions compiled once hot, in the middle of a run
first       --jit on --jit-hot 1     functions compiled at their first call
slice       --jit all --slice 7      the JIT with a time slice of 7 instructions
...
jit-stress  --jit all --gc-stress    the JIT with a collection at every allocation
jit-verify  --jit all --gc-verify    the JIT, each minor collection checked
$ ./mlwsvm fuzz --time 3
mlwsvm fuzz: seeds from 826531522, size 40, 18 jobs; against the interpreter: jit,hot,first,slice,opt,jit-opt,stress,jit-stress,verify,jit-verify
mlwsvm fuzz: 881 programs (seeds 826531522-826532402), 9691 runs, 0 skipped, 0 failed, 3.1 s
```

The `slice` mode, with a time slice of 7 instructions, makes slices end
inside almost every block, so every block's stop and every way back in is
exercised. When a program disagrees, `mlwsvm fuzz --reduce FILE` shrinks it,
removing instructions while it still disagrees, to the few lines that show
the bug. That is how the fuzzer found that a `jmp`'s target was not being
counted as a block start, which made compiled Lua's `nbody` loop forever: the
reduced program was a handful of instructions.

### Breaking the code on purpose

A test that passes proves less than it seems: it might pass whatever the code
does. *Mutation testing* checks the test. Make the code deliberately wrong
in the way the test is meant to catch (use the wrong register, skip a word,
take the wrong branch), rebuild, and run the test. It must fail. If it
passes, the test does not test that, and it is improved until it fails.

For the register results above, six such mutants were built: results mapped
to the wrong registers, one register too few, a window cleared one word
short, a pair store at the wrong offset, a frame's flag wrong, and a call to
a function without machine code skipped. One passed at first, which showed
that it had broken a branch the test never took (a function never compiled
has no entry table at all, so an earlier branch is the one taken). Mutating
that branch made it fail too, as the other five did.

### What these have found

Each of these caught bugs that the others missed:

* The fuzzer and its reducer: the block start above, and an optimizer
  rewrite that turned `mul.i8 x, 2^32` into a shift by 32, using the
  immediate without truncating it to the instruction's type.
* The compilers' suites, run in every JIT mode: a crash in compiling large
  functions, where a branch was patched into a buffer that had moved as it
  grew. It showed only with every function compiled and the optimizer on, on
  one Lua test, and a release went out with it. The fuzzer was written
  after; run against that release, it finds the crash within 300 programs.
  Since then the release check runs every compiler's suite in every mode,
  and the fuzzer, before each release (chapter 12).
* CI on arm64 Linux, with AddressSanitizer and UBSan together: pointer
  arithmetic on a null pointer that no other machine reported.

## Ideas measured and not done

The same tools decide what not to build. Two ideas, measured on compiled
Lua's `nbody` and `methods`, Lox's `trees` and `zoo`, and Erlang's `nbody`
and `binarytrees`:

**Type speculation.** Optimizing JITs compile code for the types values have
had, and remove the checks. To see what that could gain here, a script
followed what each check of an R register (null, a constant tag, a type)
established, through each function's control flow, and asked how many later
checks were already answered. As the code stands, none: the compilers' slow
paths rejoin the fast path, and might change a value's tag. Treating code
that never ran as a side exit (speculation) made 1–8% of the machine
instructions redundant checks, and 0–6% checks a loop could hoist.

**Keeping registers across slow paths' merges.** 57–98% of the labels that
ran had only one predecessor that ran: the others were slow paths that never
ran, and they force a new block, with its write-back and reloads. Classifying
every executed x86-64 instruction from the dump (window loads and stores,
compares, branches) showed what keeping the registers across those labels
would save: 0.1–1.1% in loads, and at most 0.8–4.4% in stores.

Both are real and both are small, and both would add complexity where bugs
are hardest to find. The machine code's time is spread over checks, branches
and memory access, with no one overhead left to remove. The numbers are in
[ROADMAP.md](../ROADMAP.md#4-performance-when-measurements-call-for-it).

## Where to look

| What | Where |
|---|---|
| The address map and the dump | `jit_map`, `jit_dump` in [src/vm/jit.inc](../../src/vm/jit.inc) |
| The report | `interp_jit_report` in `jit.inc`; [JIT.md: the report](../JIT.md#the-report) |
| The JIT's tests | [tests/jit/](../../tests/jit/), `check-jit` in the [Makefile](../../Makefile) |
| The fuzzer and reducer | [src/tools/fuzz.c](../../src/tools/fuzz.c); [TOOLING.md](../TOOLING.md) |
| How the JIT is tested | [JIT.md: testing](../JIT.md#testing) |
| The measurements | [PERFORMANCE.md: the JIT](../PERFORMANCE.md#the-jit), [ROADMAP.md §4](../ROADMAP.md#4-performance-when-measurements-call-for-it) |

Previous: [10. JIT 3: making it fast](10-jit-fast.md) · Next: [12. Testing a virtual machine](12-testing.md)
