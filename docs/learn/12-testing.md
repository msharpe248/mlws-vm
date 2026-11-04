# 12. Testing a virtual machine

A bug in a VM rarely announces itself. A missing garbage-collection root
gives a wrong answer only when a collection happens at exactly the wrong
instruction. A JIT bug shows only in code hot enough to be compiled. A data
race may show once in many runs, and on one machine only. And MLWSVM can run one
program in many ways (the interpreter, quickened forms, three dispatch
builds, the optimizer, the JIT, one thread or many), all of which must
agree. This chapter shows how the project tests itself: a suite written in
the VM's own assembly, run again in a series of *modes* that each make one
class of bug visible, sanitizers, a differential fuzzer that writes random
programs, mutation tests that check the tests, and the release check that
runs all of it on three machines. [Chapter 11](11-jit-measuring.md) covers
testing and measuring the JIT in particular; this chapter is about the
whole VM.

## Tests written in the VM's own language

A VM could be tested with unit tests in C, calling the assembler, the
loader and the interpreter's functions one by one. MLWSVM's tests are
instead MLWSVM programs, `.masm` files in [tests/](../../tests/) (318 of
them, in directories by subject) and [examples/](../../examples/). A test
program goes through everything a real program does: the preprocessor,
assembler, verifier, loader, interpreter, collector and scheduler. It
needs no knowledge of the VM's internals, so it stays valid when they
change, and the same file can be run in every mode below. LLVM's `lit`
tests, with expectations written in comments, are the model
([PRIOR_ART.md](../PRIOR_ART.md)).

The test runner, `mlwsvm test`, reads what a test expects from its
comments. The list is at the top of
[src/tools/testrunner.c](../../src/tools/testrunner.c):

```c
/* testrunner.c - `mlwsvm test`: runs every .masm test file and checks the
 * expectations declared in its comments:
 *
 *   ; expect-stdout: <one line of expected output>   (repeatable, in order)
 *   ; expect-exit: <n>                               (default 0)
 *   ; expect-error: <E_CODE>                         (assembler, loader or trap)
 *   ; skip: <reason>
 *   ; threads: <n>                                   (run on n worker threads)
 *   ; option: --<name> <value>                       (repeatable: a VM option, as on the command line)
 *   ; requires: ffi                                  (skipped in a build without libffi)
 *   ; requires: posix | windows                      (skipped on other systems)
 *   ; debug-script: <debugger command>                (repeatable: run under
 *                                                       `mlwsvm debug`; stdout is the transcript)
 *   ; expect-opt: <pass> <n>                         (repeatable: the optimizer made exactly n
 *                                                       changes of that pass to this file's module)
 *   ...
 *   ; expect-jit: compiled <function> <n>            (repeatable: the JIT gave n of its instructions
 *                                                       machine code; checked where there is a JIT)
```

In `tests/`, 190 files use the assertion macros below, 103 expect an
error and 45 check their output. `expect-error` is how the suite tests
failures, which matter as much as successes in a VM whose goal is that no
program can crash it. The whole of
[tests/processes/err_deadlock.masm](../../tests/processes/err_deadlock.masm):

```asm
; expect-error: E_DEADLOCK
.func waiter
    recv I0
    ret
.endf
.func main
    spawn waiter
    recv I0                            ; both wait forever
    ret 0
.endf
```

`tool_test` collects the `.masm` files under the paths it is given
(skipping directories whose names start with `_`, which hold helper
modules), and runs each in a fresh VM inside the runner's own process, with
the program's output captured. Then it compares, in this order:

```c
        if (why.n) {
        } else if (e.error_want[0]) {
            if (strcmp(got_code, e.error_want) != 0) {
                if (r.status == RUN_OK) sb_printf(&why, "expected error %s, but the program succeeded", e.error_want);
                else sb_printf(&why, "expected error %s, got %s: %s", e.error_want, got_code, r.err.msg);
            }
        } else if (r.status != RUN_OK) {
            sb_printf(&why, "%s: %s", got_code, r.err.msg);
            if (r.backtrace && *r.backtrace) sb_printf(&why, "\n%s", r.backtrace);
        } else if (r.exit_code != e.exit_want) {
            sb_printf(&why, "exit code %lld, expected %lld", (long long)r.exit_code, (long long)e.exit_want);
        }
        ...
        if (!why.n && e.has_stdout && strcmp(out.s ? out.s : "", e.stdout_want.s) != 0) {
            sb_printf(&why, "stdout mismatch\n--- expected ---\n%s--- got ---\n%s", e.stdout_want.s, out.s ? out.s : "");
        }
```

A watchdog thread ends the whole run, naming the test, if one runs longer
than `MLWSVM_TEST_TIMEOUT` seconds (300 by default): a VM that hangs
cannot be stopped from inside, and a CI job that hangs silently is the
worst way to fail.

### Assertions

Printing values and comparing output works, but an assertion says what
was wrong and where. [lib/std/test.inc](../../lib/std/test.inc) defines
them as macros. Each expands to one conditional branch around a call to a
native that traps:

```asm
.macro _ASSERT_CMP op, a, b, type, name
.ifb \type
    \op \a, \b, .Lassert_ok\@
.else
    \op\().\type \a, \b, .Lassert_ok\@
.endif
    ncall "test.fail" ("\name(\a, \b) failed")
.Lassert_ok\@:
.endm

.macro ASSERT_EQ a, b, type
    _ASSERT_CMP beq, \a, \b, \type, ASSERT_EQ
.endm
```

`\@` makes each expansion's label unique. `test.fail`
([src/native/natives.c](../../src/native/natives.c), `n_test_fail`) traps
with `E_ASSERT` and the message, so a failed assertion is an ordinary
runtime error, with a backtrace that gives the source line. The optional
`type` picks the comparison: `ASSERT_EQ R1, R2, r` compares references
for identity, and without it the type comes from the first register, as
for any instruction. There are `ASSERT_NE`, `_LT`, `_LE`, `_GT`, `_GE`,
`ASSERT_TRUE`, `ASSERT_FALSE` and `FAIL`.

### Worked example: a test

This test checks a function both ways: the printed results against
`expect-stdout`, and an internal fact with an assertion.

```asm
; sum_squares(n) = 1*1 + 2*2 + ... + n*n, built up in a list first so that
; the collector has something to do.
; expect-stdout: 385
; expect-stdout: 338350
.include "std/io.inc"
.include "std/test.inc"

.func sum_squares(i64 n) -> i64
    lnew R0
    mov I1, 1
1:  bgt I1, n, 2f
    mul I2, I1, I1
    lpush R0, I2
    inc I1
    jmp 1b
2:  len I3, R0
    ASSERT_EQ I3, n                    ; one square per number
    mov I4, 0                          ; the sum
    mov I5, 0
3:  bge I5, I3, 4f
    add I4, R0[I5]
    inc I5
    jmp 3b
4:  ret I4
.endf

.func main
    call sum_squares (10) -> (I0)
    PRINTLN_I I0
    call sum_squares (100) -> (I0)
    PRINTLN_I I0
    call sum_squares (0) -> (I0)
    ASSERT_EQ I0, 0                    ; the empty sum
    ret 0
.endf
```

```sh
$ ./mlwsvm test -v sum_squares.masm
PASS sum_squares.masm
1 passed, 0 failed, 0 skipped (1 files)
```

Now break it: change the loop's `bgt I1, n, 2f` to `bge`, so the last
number is left out. The list is one short, and the assertion says so,
with the line:

```sh
$ ./mlwsvm test sum_squares.masm
FAIL sum_squares.masm
     E_ASSERT: ASSERT_EQ(I3, n) failed
  at sum_squares.sum_squares (sum_squares.masm:17)
  at sum_squares.main (sum_squares.masm:28)

0 passed, 1 failed, 0 skipped (1 files)
```

A wrong `expect-stdout` line (384 for 385) fails with both outputs:

```text
FAIL wrong_expect.masm
     stdout mismatch
--- expected ---
384
338350
--- got ---
385
338350
```

## One suite, many modes

A *mode* changes how the VM runs a program but not what the program
means, so every test must give the same result in every mode. That turns
one suite into many test suites for the price of writing it once. Each
mode is chosen to make one kind of bug fail loudly, where in a normal run
it would stay hidden or show up rarely. The [Makefile](../../Makefile)
runs them ([DESIGN.md §22](../DESIGN.md#22-testing)):

| Target | How | What it makes visible |
|---|---|---|
| `make test` | `./mlwsvm test tests examples` | wrong results |
| one thread | `MLWSVM_THREADS=1` | the same suite under deterministic round-robin scheduling ([chapter 7](07-processes.md)) |
| `check-gc` | `MLWSVM_GC_STRESS=1`; `MLWSVM_GC_VERIFY=1`, also with the JIT | missing roots, use after free; missing write barriers |
| `check-opt` | `MLWSVM_OPT=all`, also with GC stress | optimizer bugs, on all the suite's code |
| `check-jit` | `MLWSVM_JIT=all` (plain, GC stress, optimized), `MLWSVM_JIT=on MLWSVM_JIT_HOT=2` (also on one thread) | JIT bugs ([chapter 11](11-jit-measuring.md)) |
| `check-switch`, `check-loop` | builds with `-DMLWS_NO_THREADED`, `-DMLWS_NO_TAILCALL` | bugs in the dispatch other compilers get ([chapter 3](03-interpreter-loop.md)) |
| `check-poison` | a build with `-DMLWS_POISON_WINDOWS` | registers read before they are written, that a call does not clear |
| `check-asan` | AddressSanitizer and UBSan, plain, GC stress, the JIT; 100 fuzzed programs | memory errors and undefined behaviour in the VM's C |
| `check-tsan` | ThreadSanitizer, `MLWSVM_THREADS=4` and 8 | data races |
| without libffi | `make FFI=0` | the FFI's tests skipped cleanly (`; requires: ffi`) |

**GC stress** asks for a collection after every allocation, run at the next
safepoint (after the next generic instruction, or at the next slice). Normally a collection happens when the young generation fills,
so a missing root (a reference the collector does not know about) bites
only if a collection falls in the short window where that reference is
live. Under stress every such window gets a collection. In
[src/heap/gc.c](../../src/heap/gc.c) it is one more condition where the
collector decides it is time:

```c
void gc_account(Process *p, int64_t delta) {
    note_peak(p);
    p->heap_bytes = (uint64_t)((int64_t)p->heap_bytes + delta);
    note_peak(p);
    if (p->heap_bytes >= p->next_gc || p->vm->gc_stress) p->gc_pending = true;
```

Every eighth stressed collection is of the whole heap, so both kinds get
exercised, and freed object buffers are filled with the byte `0xDB`, so
that code reading freed memory sees garbage instead of the old values.

**GC verify** checks the generational collector's one hard invariant
([chapter 5](05-memory-and-gc.md)): after a minor collection, no object
the program can reach may still be young, because a young object held only
by an old one is found only if a write barrier remembered the old one.
After each minor collection, `gc_verify` walks everything reachable, and
`verify_reach` stops the VM at the first young object it meets:

```c
static void verify_reach(Process *p, Obj *o) {
    if (o->flags & (OBJF_VERIFY | OBJF_IMMORTAL)) return;
    if (!(o->flags & OBJF_OLD)) {
        const Obj *by = p->gc_parent;
        fprintf(stderr, "mlwsvm: gc verify: a young %s is held by %s %s that was not remembered (a write barrier is missing)\n",
                obj_kind_name(o->kind), by ? (by->flags & OBJF_OLD ? "an old" : "a young") : "a root, not",
                by ? obj_kind_name(by->kind) : "marked");
        abort();
    }
```

**Poisoned windows.** A call clears only the registers of the new frame
that the verifier's `plan_window` found may be read before they are
written ([DESIGN.md §7.1](../DESIGN.md#71-planning-the-register-window)).
If that analysis were wrong, a function would read a register left over
from an earlier call, which is usually zero or a plausible value. The
poison build fills each new window with a pattern first, in
`proc_push_frame_fitting` ([src/vm/proc_internal.h](../../src/vm/proc_internal.h)):

```c
#ifdef MLWS_POISON_WINDOWS /* make check-poison: a register read before it is written, but not cleared, shows */
    memset(w, 0xA5, (size_t)fn->rsize * sizeof *w);
#endif
    ZERO_REGS(w + fn->zoff, fn->zlen);
```

**Sanitizers** are compiler instrumentation. AddressSanitizer checks every
memory access in the VM's C against shadow memory and reports overflows
and use after free at the access, not later; UBSan reports undefined
behaviour such as signed overflow or a null pointer passed to `memcpy`;
LeakSanitizer (on Linux) reports memory never freed. ThreadSanitizer
records which thread touched which memory under which lock, and reports
two accesses with no ordering between them, even when the run happened to
give the right answer. The `check-tsan` target runs the whole suite on
four threads and the process, thread, native, OS, network and library
tests on eight.

You can run a mode on one file through the same environment variables:

```sh
$ MLWSVM_GC_STRESS=1 ./mlwsvm test sum_squares.masm
1 passed, 0 failed, 0 skipped (1 files)
$ MLWSVM_JIT=all ./mlwsvm test sum_squares.masm
1 passed, 0 failed, 0 skipped (1 files)
```

### A bug only one mode sees

To see why the modes exist, take a real bug: a missing write barrier.
`list_push` in [src/heap/objects.c](../../src/heap/objects.c) ends with
`gc_write_word(p, &l->h, v, tag);`, which remembers an old list that is
given a young object. Delete that line, build the VM, and run the suite
with the result (a copy of the tree in a scratch directory was built for
this; the paths are shortened):

```sh
$ mut/mlwsvm-barrier test tests examples
324 passed, 0 failed, 4 skipped (328 files)
$ MLWSVM_GC_VERIFY=1 mut/mlwsvm-barrier test tests examples
mlwsvm: gc verify: a young string is held by an old list that was not remembered (a write barrier is missing)
```

Every test passes in a normal run, and a run of `tests/` with GC stress
passes too. The minor collection never sees the young string through the
old list, so it treats the string as garbage while the list still points
to it; but a dangling reference can go on reading correct-looking bytes until
something reuses the memory, so whether a test notices is luck. GC verify
does not depend on luck: it stops at
[tests/objects/gc_generations.masm](../../tests/objects/gc_generations.masm),
which stores young objects into old ones in every way the instruction set
allows, and it names the kind of object and its holder. (The four skipped
tests are Windows-only.)

## Differential fuzzing

Hand-written tests check what their authors thought of. *Fuzzing*
generates inputs nobody thought of, and runs them by the thousand. For a
compiler or VM the input is a program, and the hard part is the *oracle*:
how do you know what a random program should print? *Differential
testing* answers it by running the program two ways and comparing
(Csmith does this for C compilers). MLWSVM has a natural reference: its
interpreter. Every other mode must print exactly what the interpreter
prints.

`mlwsvm fuzz` ([src/tools/fuzz.c](../../src/tools/fuzz.c),
[TOOLING.md](../TOOLING.md#differential-fuzzing-mlwsvm-fuzz)) generates
programs with three properties a random program needs to be useful here:

* **Valid.** Every instruction must find the types it expects, or the
  verifier rejects the program. The generator gives registers fixed roles,
  so it always knows what each holds:

  ```c
   *   R0  a list of 16 integers      R5  a string (the function's own)
   *   R1  a list of 16 floats        R6  a Box: R2, or a new one
   *   R2  a Box (struct)             R7  a list of integers (the function's own)
   *   R3  a map: keys integers 0..31 and strings, values integers
   *   R4  a list of 8 strings        R8  a closure; R9 anything, briefly
   *   R10 a byte buffer of 32 or more  R11, R12 arrays of 16 i64 and f64
   *   I0..I11, F0..F7 values (I0, I1, F0 the arguments); I12..I14 loop
   *   counters; I15 the checksum; P0 a raw block of 128 bytes.
  ```

* **Always ends.** Each function calls only functions after it, loops count
  to constants, and a budget of instructions and allocations is counted
  through loops and calls, so a program runs in a few milliseconds.
* **Deterministic.** The generator is a seeded splitmix64, part of the
  binary, so a seed names the same program on every machine
  (`mlwsvm fuzz --emit SEED`). Floats are printed as their bit patterns,
  with every NaN printed as one value.

Each program runs as a child process, `mlwsvm run --no-cache --threads 1`
plus the mode's options, with every `MLWSVM_` variable cleared, and a
watchdog kills a run that hangs. The modes:

```sh
$ ./mlwsvm fuzz --list
interp                               the interpreter, the reference
jit         --jit all                every function compiled as it loads, entered at every block
hot         --jit on --jit-hot 2     functions compiled once hot, in the middle of a run
first       --jit on --jit-hot 1     functions compiled at their first call
slice       --jit all --slice 7      the JIT with a time slice of 7 instructions
opt         --opt all                every optimizer pass
jit-opt     --jit all --opt all      the JIT on optimized code
stress      --gc-stress              a collection at every allocation
jit-stress  --jit all --gc-stress    the JIT with a collection at every allocation
verify      --gc-verify              each minor collection checked against the whole heap
jit-verify  --jit all --gc-verify    the JIT, each minor collection checked
```

`verdict` compares each run with the interpreter's:

```c
static const char *verdict(const RunOut *base, const RunOut *r, char *buf, size_t n) {
    if (r->timeout) return "hangs";
    if (r->code >= 128 && r->code < 255) {
        snprintf(buf, n, "crashes (signal %d)", r->code - 128);
        return buf;
    }
    if (r->code != base->code) {
        snprintf(buf, n, "exits with %d, not %d", r->code, base->code);
        return buf;
    }
    if (r->out.n != base->out.n || memcmp(r->out.s, base->out.s, r->out.n)) return "prints something else";
    return NULL;
}
```

Five seconds of it on the M5 Pro, 18 programs at a time:

```sh
$ ./mlwsvm fuzz --time 5 --seed 1000
mlwsvm fuzz: seeds from 1000, size 40, 18 jobs; against the interpreter: jit,hot,first,slice,opt,jit-opt,stress,jit-stress,verify,jit-verify
mlwsvm fuzz: 1493 programs (seeds 1000-2492), 16423 runs, 0 skipped, 0 failed, 5.1 s
```

A small program (`--emit 1000 --size 6`) shows the shape. `main` builds the
shared objects, then calls each function inside a `.try`, so an exception
is compared too:

```asm
.func main
    lnew R0, 16
    resize R0, 16
    ...
    mnew R3
    lnew R4, 8
    lpush R4, "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7"
    mov I10, 0
.L37:
.try .L38
    call f0 (R0, R1, R2, R3, R4, -9223372036854775807, I10, 1e-310) -> (I1, F1)
.endtry
    ncall "io.print_s" ("call 0 f0: ")
    ncall "io.print_i" (I1)
    ...
```

```text
call 0 f0: 3018282391252991213 -4503599627370496
call 1 f1: -32768302 -4503599627370496
call 2 f2: 3410461528447320780 9218868437227405312
```

A failing program is saved with the outputs of every mode. It is usually
hundreds of lines, most of which have nothing to do with the bug, so
`--reduce` shrinks it in the manner of delta debugging: remove half the
lines, then quarters, and so on, then runs of up to four lines, keeping a
removal whenever the program still assembles, still exits as before in the
interpreter, and still fails the same way in the failing mode. The tools
check ([tests/tools/check_tools.sh](../../tests/tools/check_tools.sh))
tests the reducer with a fake VM, a shell script that prints an extra line
whenever it is given `--jit` and the program contains `MARK`. The same
thing, by hand. The fake VM, `vm`:

```sh
#!/bin/sh
for a; do f=$a; done
case " $* " in *" --jit "*) grep -q MARK "$f" && echo miscompiled;; esac
exec "/path/to/mlwsvm" "$@"
```

```sh
$ ./mlwsvm fuzz --emit 7 > a.masm
$ awk '{ print } /^\.func f0/ { print "    nop                                ; MARK" }' a.masm > c.masm
$ ./mlwsvm fuzz --vm ./vm --reduce c.masm --modes jit --out out
c.masm: jit differs
...
c.masm: 7 lines, from 751 (313 tests): c.reduced.masm
$ cat c.reduced.masm
.func f0(ref l, ref fl, ref b, ref m, ref ss, i64 x, i64 y, f64 z) -> (i64, f64)
    nop                                ; MARK
    ret I15, F8
.endf
.func main
    ret 0
.endf
```

`make check-fuzz` runs seeds 1 to 300 in every mode, and CI runs it on
every push. Fuzzing found real bugs, such as an optimizer identity that did
not truncate a constant to the instruction's width
([ROADMAP.md](../ROADMAP.md)). It is not a replacement for the suite: the
fuzzer's 300 seeds pass on the missing-barrier VM above, which GC verify
on the hand-written suite caught at once. [Chapter 11](11-jit-measuring.md)
shows the fuzzer at work on the JIT.

## Mutation testing: testing the tests

A test that passes tells you nothing unless it would fail if the code
were wrong. *Mutation testing* checks exactly that: make a small deliberate
change to the code under test (a *mutant*: a `<` for a `<=`, a deleted
line, a constant off by one) and run the tests. If some test fails, the
mutant is *killed*; if all pass, it *survived*, and the tests have a gap.
Tools such as PIT (for Java) and Stryker generate mutants by the thousand.
MLWSVM does it by hand and on purpose. When a test is added for a fix or a
feature, the code under test is broken the way the test is meant to
catch, the VM is rebuilt, and the test must fail; then the change is put
back. [ROADMAP.md](../ROADMAP.md) records tests written to that standard:
"a cache ignoring the caller fails it", "a JIT whose tail call skips the
save fails it".

The `bgt` changed to `bge` in the worked example above was a mutant of the
program under test, and the assertion killed it. Here is one of the VM,
in the message copy of [chapter 7](07-processes.md). In `shallow` in
[src/heap/copy.c](../../src/heap/copy.c), a struct's fields that are not
references are copied by this line:

```c
            if (!type_is_ref(s->def->types[i])) ns->fields[i] = s->fields[i];
```

Delete it, so a struct sent to another process arrives with its numbers
zeroed, and build the mutant from a copy of the tree. The test runner runs
the repository's tests with any binary, and the module cache is keyed by
the executable, so the mutant cannot disturb the real build's entries:

```sh
$ mut/mlwsvm test tests examples
FAIL tests/loader/module_processes.masm
...
FAIL examples/workers.masm
     stdout mismatch
--- expected ---
worker 1: 25 primes in [0, 100)
worker 2: 21 primes in [100, 200)
worker 3: crashed with E_BOUNDS
worker 4: 16 primes in [300, 400)
total: 62
--- got ---
null
null
worker 3: crashed with E_BOUNDS
null
total: 0

322 passed, 2 failed, 4 skipped (328 files)
```

The mutant is killed, but look at who killed it: a loader test and an
example. All 30 files in `tests/processes/` and `tests/threads/` pass with
it. [tests/processes/messages.masm](../../tests/processes/messages.masm)
even runs this very line, since it sends a struct, but its struct's fields
are both references, so there is no number to lose. Line coverage would
count the line as tested; the mutant shows that nothing in the message
tests checks what it does. [Chapter 11](11-jit-measuring.md#breaking-the-code-on-purpose)
describes six mutants built to check the tests of one JIT feature, one of
which survived at first.

## The release check

Before a release, `make release-check` runs
[tests/release-check.sh](../../tests/release-check.sh)
([TOOLING.md](../TOOLING.md#before-a-release-make-release-check)). It
starts from a clean build, goes on after a failure, logs each step, and
prints one line per step:

```sh
step build make CC="$CC" -j"$JOBS"
step build-warnings no_warnings
step test make CC="$CC" test
step one-thread env MLWSVM_THREADS=1 ./mlwsvm test tests examples
step check-gc make CC="$CC" check-gc
step check-opt make CC="$CC" check-opt
step check-jit make CC="$CC" check-jit
...
step check-asan make CC="$CC" check-asan
step asan-opt env MLWSVM_OPT=3 ./mlwsvm-asan test tests examples
step asan-jit-opt env MLWSVM_JIT=all MLWSVM_OPT=3 ./mlwsvm-asan test tests examples
step check-tsan check_tsan
...
step fuzz ./mlwsvm fuzz --time "$FUZZ" --out build/release-check/fuzz
step fuzz-asan ./mlwsvm-asan fuzz --time "$((FUZZ / 10 + 1))" --out build/release-check/fuzz-asan
```

A build must have no warnings. The fuzzer runs for five minutes by
default, and a tenth of that under AddressSanitizer. Then, if the
compilers' repository (`mlws-compilers`) is next to this one, it runs the
Brainfuck, Lox, Lua and Erlang compilers' own test suites against this
build in every mode: the JIT off, on, at first call and `all`; GC stress
and `MLWSVM_OPT=3`, also with `all`; GC verify, also with `all`; and
under AddressSanitizer. Those suites are large amounts of real generated code
that nobody wrote with the VM's corners in mind, which makes them a
second, independent test of the VM and its optimizer. The whole check
takes about half an hour on each machine, and it is run on each of the three machines the
project measures on, an Apple M5 Pro, an Intel i7 and an AMD Ryzen, which
differ in architecture (arm64 and x86-64), compiler (Clang and GCC) and
operating system (macOS and Linux).

## Continuous integration

Every push runs CI on GitHub Actions
([.github/workflows/ci.yml](../../.github/workflows/ci.yml)): Linux with
GCC and with Clang, and macOS with Clang, each running the suite, one
thread, `check-gc`, `check-opt`, `check-jit`, `check-fuzz`, the tools,
both sanitizers, both fallback dispatches, the poison build, a build
without libffi and the benchmarks' output checks. A fourth job runs the
suite, `check-jit`, the fuzzer and AddressSanitizer on arm64 Linux, since the
first three cover x86-64 Linux and arm64 macOS only.
[windows.yml](../../.github/workflows/windows.yml) builds with Clang in
MSYS2 and runs everything but the sanitizers (MinGW has no
ThreadSanitizer, and its AddressSanitizer is incomplete), and checks that
the JIT is built in. Because the tests are programs in the VM's own
language, the same files run unchanged on all of them.

## Where to look

| What | Where |
|---|---|
| The test runner | `tool_test`, `parse_expect`, `check_opt`, `check_jit`, `watchdog` in [src/tools/testrunner.c](../../src/tools/testrunner.c) |
| Assertions | [lib/std/test.inc](../../lib/std/test.inc); `n_test_fail` in [src/native/natives.c](../../src/native/natives.c) |
| The modes | the `check-*` targets in the [Makefile](../../Makefile); `gc_account`, `set_alloc_limit`, `gc_verify`, `verify_reach` in [src/heap/gc.c](../../src/heap/gc.c); `MLWS_POISON_WINDOWS` in [src/vm/proc_internal.h](../../src/vm/proc_internal.h) |
| The fuzzer | `gen_program`, `stmt`, `MODES`, `run_mode`, `verdict`, `reduce` in [src/tools/fuzz.c](../../src/tools/fuzz.c); its own test in [tests/tools/check_tools.sh](../../tests/tools/check_tools.sh) |
| The release check and CI | [tests/release-check.sh](../../tests/release-check.sh), [.github/workflows/](../../.github/workflows/) |
| Reference | [DESIGN.md §22](../DESIGN.md#22-testing); [TOOLING.md](../TOOLING.md#differential-fuzzing-mlwsvm-fuzz) on the fuzzer, [the release check](../TOOLING.md#before-a-release-make-release-check) and [VM options](../TOOLING.md#vm-options); [ASSEMBLY.md §11](../ASSEMBLY.md#11-tools); [JIT.md](../JIT.md#testing) |

Previous: [11. JIT 4: measuring and testing](11-jit-measuring.md)
