# The Design of MLWSVM

This document describes how MLWSVM is put together and why: the goals behind
it, the path a program takes from source text to running instructions, and
the design of each part. It gives the shape and the reasons. The reference
documents it links to give the details: every instruction, operand, option
and error code.

## Contents

1. [Goals](#1-goals)
2. [The whole system at a glance](#2-the-whole-system-at-a-glance)
3. [The instruction set](#3-the-instruction-set)
4. [Modules](#4-modules)
5. [The assembler](#5-the-assembler)
6. [Loading, linking and verifying](#6-loading-linking-and-verifying)
7. [The interpreter](#7-the-interpreter)
8. [Registers, frames and calls](#8-registers-frames-and-calls)
9. [Memory and safety](#9-memory-and-safety)
10. [The heap and the collector](#10-the-heap-and-the-collector)
11. [Exceptions and traps](#11-exceptions-and-traps)
12. [Processes and the scheduler](#12-processes-and-the-scheduler)
13. [Input and output](#13-input-and-output)
14. [Coroutines](#14-coroutines)
15. [Natives, extensions and the FFI](#15-natives-extensions-and-the-ffi)
16. [Embedding](#16-embedding)
17. [The optimizer](#17-the-optimizer)
18. [The tools](#18-the-tools)
19. [The platform layer](#19-the-platform-layer)
20. [Testing](#20-testing)
21. [What is left out, and why](#21-what-is-left-out-and-why)

## 1. Goals

These goals decide the trade-offs everywhere else.

* **A target for compilers that people can also write by hand.** The
  assembly language is the interface. A compiler emits text
  ([TARGETING.md](TARGETING.md)), and the VM assembles, links and checks it.
  A rich instruction set (typed, variadic, any operand in memory) keeps
  generated code short and readable. A preprocessor, register aliases and
  local labels make hand-written code bearable.
* **A register machine.** Fewer, larger instructions than a stack machine's,
  so less dispatch per unit of work.
  [REGISTER_VS_STACK.md](REGISTER_VS_STACK.md) measures the trade-off.
* **Safe by construction.** No program, however wrong or hostile, can crash
  the VM, read memory that is not its own, or forge a reference. Every check
  either happens once, at load time (the verifier), or is built into the
  representation (checked pointers, tagged words), so the interpreter never
  has to trust the code it runs.
* **Isolation through processes.** Concurrency means many small processes
  that share nothing mutable and talk by copying messages, as in Erlang. That
  makes the collector, the scheduler and the safety story simple: no heap is
  ever touched by two threads.
* **Portable C11, nothing required.** The VM builds with any C11 compiler.
  libffi is optional: without it, only `.extern` is missing. All OS-specific
  code is behind one header (§19).
* **Measured, not guessed.** Every performance technique was chosen from a
  profile and kept only if it measured faster on three machines. Ideas that
  measured badly are written down with their numbers
  ([PERFORMANCE.md](PERFORMANCE.md#what-is-not-done-and-why)), so they are not
  tried again blind.
* **One source of truth.** The instruction set is defined once
  (`src/core/opcodes.def`). The assembler, disassembler, checker, verifier
  and `docs/ISA.md` all derive from it. The native registry generates
  `docs/NATIVES.md` the same way.
* **Tested in its own language.** The test suite is written in MLWSVM
  assembly, and every test mode runs all of it (§20).

## 2. The whole system at a glance

```
  prog.masm ─► preprocessor ─► assembler ─┐                    (src/asm)
                                          ▼
  prog.mlwso ─────► module_read ─────►  Module  ◄──── library archive (.mlwsl)
                                          │           (src/loader)
                         module cache ◄───┤  assembled once; optimized once
                                          ▼
                                optimizer (--opt)                (src/opt)
                                          │
                                          ▼
                    loader: find imports, choose library versions,
                    link names to code, load native code         (src/vm/loader.c)
                                          │
                                          ▼
                    decode every function into Insns, then verify
                    (check.c per instruction, verify.c per function)
                                          │
                                          ▼
                    quicken: pick a fast handler for each common shape
                                          │
                                          ▼
       processes ──► scheduler: N worker threads, run queues, stealing
                     │   └─ the interpreter runs a time slice of a process
                     ├─ poller thread: processes waiting for sockets and pipes
                     └─ I/O thread pool: blocking native calls
```

A **module** is the unit of everything. It is what the assembler produces,
what a `.mlwso` file holds, what the cache stores, what the optimizer
rewrites and what the loader links. Text and binary are two spellings of
one thing: `mlwsvm dis` turns a module back into assembly that assembles to
the same bytes.

Loaded code is **immutable and shared** by every process and thread.
Everything that changes belongs to exactly one process.

The source tree follows the same structure:

| Directory | What |
|---|---|
| `src/core/` | Types, the opcode table, the instruction codec, the per-instruction checker, register effects, shared arithmetic, the disassembler, Unicode, versions, SHA-256 |
| `src/asm/` | Lexer, constant expressions, preprocessor, assembler |
| `src/loader/` | The `.mlwso` and `.mlwsl` readers and writers |
| `src/vm/` | Loader and linker, verifier, module cache, processes, coroutines, scheduler, interpreter, extensions, options, the run helper |
| `src/heap/` | Heap objects, the collector, BigInt and decimal, message copying |
| `src/opt/` | The optimizer |
| `src/native/` | Built-in and standard-library natives, the extension API, the FFI |
| `src/embed/` | The embedding API |
| `src/tools/` | Debugger, profiler and tracer, monitor, `top`, test runner, doc generator, `lib`, `ext`, `opt`, `bindgen` |
| `src/platform/` | The only OS-specific code |
| `lib/std/` | The parts of the standard library written in MLWSVM assembly |

## 3. The instruction set

**One table defines it.** Each line of `src/core/opcodes.def` is an X-macro
entry: `OP(ENUM, CODE, "mnemonic", "category", "pattern", typeset, flags, "doc")`.

* `CODE` is the stable 16-bit number written to `.mlwso` files. Opcodes are
  never renumbered.
* The *pattern* gives each operand position a letter: a destination, a typed
  source, a label, a function, a call block, and so on, with `?`, `*` and `+`
  for optional and repeated operands. Everything that needs to know an
  instruction's shape derives it from the pattern: operand counts, which
  addressing modes each position allows, which positions are written, and the
  register effects the verifier and optimizer use.
* `typeset` lists the type suffixes the instruction accepts.

**Typed instructions, typed registers.** An instruction's type is a suffix:
`b c i8 i16 i32 i64 u8 u16 u32 u64 f32 f64 r p big dec`. There are four
register banks of 32: `I` (integers), `F` (floats), `R` (references the
collector traces) and `P` (checked pointers). Each type belongs to one bank.

* Integers are kept *canonical*: a register holds the 64-bit value of the
  type last written, sign- or zero-extended. Narrow arithmetic wraps
  correctly, and an `i64` handler never has to look at the type.
* Keeping references in their own bank is what makes the collector precise
  without a tag on every register (§10).

**Variable arity.** An instruction has 0 to 32 operands. `add I0, I1, I2, I3`
folds left to right, and `call f (a, b) -> (q, r)` carries its arguments and
results in one instruction. Variable arity costs one byte per instruction and
a loop in the generic handler. A fixed three-operand format measured no
faster, and its calls were 13–17% slower
([REGISTER_VS_STACK.md, part 4](REGISTER_VS_STACK.md#part-4-up-to-32-operands-against-a-fixed-3)).

**Any operand can be in memory**, including the destination: registers,
immediates, constants, `SP[n]`, `P1[I2*8+4]`, `[SP]+`, `@global[I3]`,
`[FP[2]][3]`, `ARG[n]`, `LOC[n]`, object elements `R1[I2]` and fields
`R1->Point.x`, and closure captures `UP[n]` ([ADDRESSING.md](ADDRESSING.md)).
This VAX-like generality lets a compiler emit one instruction where a
load-store machine needs three. Every memory access still goes through the
checks of §9.

**Encoding.** `opcode:u16 type:u8 [type2:u8] nops:u8 operand*`. Each operand
is a mode byte and a payload. Immediates and displacements are LEB128, so
code is compact and has no alignment ([BINARY_FORMAT.md](BINARY_FORMAT.md)).
Nothing in the encoding is a machine address: functions, globals, constants,
natives, struct types and fields are indices into the module's own tables.

## 4. Modules

A `Module` (`src/core/module.h`) holds tables, and code that refers to those
tables by index:

* **Constants**: strings, BigInts and decimals.
* **Functions**: name, parameter and result types, register counts per bank,
  code range, exception handler regions, and the source line of every
  instruction. Optionally, `.loc` runs that say which line of a translated
  program each stretch came from.
* **Exported symbols, imports, global references, native names, struct types
  and field references.** All of these are *by name*. A call to `math.gcd` is
  an import index plus a name, resolved at link time.
* **The global segment's initial image**, with one type tag per 8-byte word.
* **Native-code declarations**: `.extension`, `.extern`, `.cstruct`,
  `.callback` and `.ffi_free`.
* **`.requires` constraints**, and for a library's manifest, its name,
  version and public modules.

Because a module refers to everything outside itself by name, a module is
self-contained:

* It can be assembled, cached, optimized or archived on its own.
* It can be moved on disk: a module's name never says where its file is.
* It can be loaded into any VM, where its imports might resolve to
  different modules.

## 5. The assembler

`src/asm/` is a pipeline:

1. **The lexer** (`lexer.c`) turns a line into tokens.
2. **The preprocessor** (`preproc.c`) expands constants, macros (with
   defaults, varargs and unique labels), `.rept` and `.irp`, conditional
   assembly and `.include`. `mlwsvm asm -E` shows its output. Constant
   expressions are evaluated by a precedence-climbing evaluator (`expr.c`)
   with 64-bit wrapping integers and floats.
3. **The assembler** (`assembler.c`) parses each line into labels,
   directives and instructions with operands. It infers types where the
   operands decide them, and resolves labels and register counts at
   `.endf`. At the end of the module it resolves functions and globals, and
   encodes the module.

The assembler checks each instruction with the same code as the verifier
(`src/core/check.c`): operand counts, the addressing modes each position
allows, register banks against types, and writability. Its errors therefore
point at the source line and operand, and nothing the assembler accepts can
fail verification for a reason it could have reported.

## 6. Loading, linking and verifying

`src/vm/loader.c` turns modules into runnable code.

**Finding modules.** An import is looked for in:

1. the program's own directory;
2. each directory of the library path;
3. the library archives in those directories.

The first match wins. Within one directory the newer of `.masm` and `.mlwso`
wins ([ASSEMBLY.md §8](ASSEMBLY.md#8-modules-and-libraries)).

* A module found in a program's directory is known by that directory and its
  name (`LModule.key`). So several programs in one VM can each have their own
  `main` and `util`.
* A module from the library path is known by its name alone and shared by
  every program.
* A library-path module looks for its imports only on the library path, so a
  program's files can never stand in for a library's.

**Libraries.** A library is a named, versioned archive (`.mlwsl`) with a
manifest:

* **One version per run.** Each library is chosen the first time it is
  needed: the newest version that meets every `.requires` stated so far. When
  several programs start together, all their first modules' constraints are
  stated before any library is chosen, so the order of the programs does not
  matter.
* **Private modules.** Modules the manifest does not list as public can be
  imported only by the library's own modules.
* **Lazy decoding.** An archive's module table is read when the library path
  is scanned. A module is decoded only when it is imported.

**Registering a module** (`register_module`) runs these steps in order:

1. Reject a second module with the same key.
2. Optimize the module, if `--opt` asks for it (§17).
3. State the module's `.requires`.
4. Load its imports recursively. A module that is still loading when it is
   imported again is a cycle, reported with its chain (`E_LINK_CYCLE`).
5. Link it (`link_module`):
   * intern its constants into the VM's pool, one immortal object per kind
     and text for all modules;
   * resolve imported functions, globals, structs and fields by name;
   * load the native code it declares, with the trust checks of §15;
   * decode every function into `Insn`s;
   * verify the module;
   * quicken it (§7).

Any failure stops the load before any code runs.

**The verifier** (`src/vm/verify.c`) is what lets the interpreter trust the
code it runs. Everything the interpreter relies on without checking again is
established here:

* each instruction's shape (the shared checker);
* registers within the function's window;
* branch targets and handler regions within the function;
* `ret` arity against the declared results;
* every call against the callee's signature, also across modules: argument
  count, argument and result operand kinds, and banks.

A `.mlwso` file from anywhere gets the same checks as freshly assembled
source.

The verifier also plans each function's **register window**
(`plan_window`). A forward dataflow pass finds the registers that may be read
before they are written. The window's `R` registers are always cleared. A
call clears one run of words, from the first register that needs clearing to
the last (§8).

**Loading at run time.** `module.load` loads a module by name while the
program runs, under `mods_lock`.

* Processes that started earlier get the new module's global segment when
  they first touch it (`proc_grow_globals`).
* A load that fails leaves the modules it had not linked unusable, and their
  names free.
* Modules are never unloaded, so a closure can never outlive its code.

**The module cache** (`src/vm/modcache.c`) keeps assembled modules, so a
large generated program is assembled once.

* An entry's key is a SHA-256 of the executable's identity, the default
  module name and the source text. A hit also requires the same SHA-256 for
  every file the source included, and the same inputs to their resolution.
* Optimized modules are cached too, keyed by the passes and the module's
  bytes.
* The cache directory must pass the same trust check as native code, because
  a cached module can declare foreign functions.
* Entries are written to a temporary file and renamed into place, so a
  reader never sees half of one.

## 7. The interpreter

**Decode once.** Each function is decoded at load time into an array of
`Insn` (`src/core/insn.h`):

* the opcode and types;
* a pointer to its decoded `Operand`s, 72 bytes each, with room for every
  addressing mode and for what the linker resolved (object pointers, global
  segment bases, byte displacements, branch targets);
* an *execution opcode* (`xop`);
* a compact copy of a quickened form's operands: three register numbers, a
  target and an immediate.

**Quicken.** `interp_quicken` gives each common instruction shape a
specialised handler as its `xop`. Examples are 64-bit integer and `f64`
arithmetic on registers and immediates, compare-and-branch, calls and
returns, field and element moves, `mget` by a constant key, `lpush` and
`bisa`. The real opcode stays in `op`, so the disassembler, tracer, profiler
and debugger see the instruction as written.

A shape qualifies only when its generic meaning is trivially the same: no
width canonicalisation, and no trap that cannot be checked cheaply. When a
handler meets anything unusual, it hands the instruction to the generic form,
which then raises the usual error: a null object, another struct type, a
list element of another type, or an index out of range. The shapes were
chosen from profiles of compiled Lox and Lua. The profiler lists the
instructions that still run generically, which is how new ones are picked
([PERFORMANCE.md](PERFORMANCE.md#how-the-interpreter-is-made-fast)).

**Dispatch** is built one of three ways from the same handler source
(`qforms.inc` for the quickened forms, `generic.inc` for the rest):

* **Tail calls.** Each handler is a C function that ends by tail-calling the
  next instruction's handler through a table (`musttail`, and with Clang
  `preserve_none`). The hot state travels in argument registers: the
  instruction, the slice counter, the register windows and the frame. Each
  handler's registers are allocated on their own, so adding one does not
  slow the others, and each jump is predicted separately.
* **A dispatch loop** with labels as values, for compilers that cannot
  guarantee tail calls.
* **A `switch`**, for compilers without labels as values.

`make check-loop` and `make check-switch` test the two fallbacks, and CI runs
them.

**Keep the hot state in registers.** The current instruction and the slice
counter live in CPU registers. The frame's `pc` and the process's instruction
count are written back only where something can look at them: before a
generic instruction (which may trap, call a native or switch frames), at
calls and returns, before a tool's hook, and at the end of the slice. None of
this is observable: a trap reports the right instruction, and `--stats`
counts every instruction.

**Safepoints.** An allocation only marks a collection as due. Pending
collections and kills run at a safepoint after each generic instruction and
at the start of each slice. So a collection never happens inside an
instruction, and a run of quickened instructions delays one by at most a
slice.

**Shared arithmetic.** The pure arithmetic is `static inline` code in
`src/core/arith.h`: binary and unary operations, shifts, compares,
conversions, bitcasts and width masks. The interpreter and the optimizer's
constant folding both use it, so a folded value is always the one the
instruction would have computed.

**Rare work outside the dispatch.** Coroutine switches and a coroutine's
final return are handled by a wrapper around the dispatch, which is handed
the instruction instead of running it. This keeps the dispatch's register
allocation, and therefore every handler's speed, untouched by rare features.

## 8. Registers, frames and calls

**A fresh window per call.** Every call gets its own registers, which read as
zero until written. A function's window size per bank is fixed when it is
assembled and checked by the verifier. The caller's registers are exactly as
it left them when a call returns, so there is no caller-saved or callee-saved
convention for a compiler to manage ([ABI.md §2](ABI.md#2-registers-and-frames)).

**One register stack.** Each process has one stack of 8-byte words. A
frame's window is one block of it, holding its `I`, `F`, `R` and `P`
registers one after another, at per-function offsets (`Function.roff`).

* Pushing a frame checks one limit, moves one top and clears one run of
  words. Popping a frame restores one top.
* Both the register stack and the frame stack start small (64 words and 8
  frames) and double as needed, so a process that never calls deeply stays
  small.
* A window per bank was measured instead: every call did its bookkeeping
  four times, and `fib` took 37% longer.

**Clearing less.** A new window must read as zero, but only two kinds of
register need clearing: its `R` registers, which the collector scans, and
those the function may read before writing them. The verifier's plan (§6)
gives one run of words to clear. Compiled Lua's typed `fib` clears nothing.
`make check-poison` fills each new window with garbage before clearing the
run, so a register that is read before it is written, but was not cleared,
makes a test fail.

**Call plans.** Each quickened `call` and `ret` gets a plan at load time: the
callee, and for each argument and result, its word offset in each window.
Arguments are copied straight across with no test of their bank. A frame
pushed this way is marked, so its quickened `ret` copies the results back
the same way. Closure calls (`calli`) work the same way, with the closure's
type checked per call.

**Calling convention.** Arguments bind per bank, in order. The first
integer-class parameter goes to `I0`, the first float to `F0`, and so on.
Results go to any writable operands of the caller. More arguments, or
variadic ones, go on the per-process data stack and are read as `ARG[n]`.
`SP`, `FP` and `PID` are per process, not per frame. Tail calls, closures
(captures read as `UP[n]`) and natives use the same call block
([ABI.md §2](ABI.md#2-registers-and-frames)).

## 9. Memory and safety

A program may do low-level work: raw memory, pointer arithmetic, a data
stack, byte-level access. All of it stays inside the process and is checked.

**Checked pointers.** A `p` value is `segment:20 | generation:12 |
offset:32` ([ABI.md §4](ABI.md#4-pointers)). Each process has its own
segment table:

* segment 1 is the running data stack;
* segments 2–4095 are the module global segments;
* segments from 4096 up are raw heap blocks.

Every dereference finds the segment, compares its generation and checks the
offset against its size. That catches:

* **out of bounds**: the offset is checked against the segment's size;
* **use after free**: `free` bumps the generation, so stale pointers trap
  `E_UAF`;
* **another process's memory**: a pointer means nothing outside its own
  process's table.

No instruction turns an integer into a pointer. Fabricating a pointer's bits
in raw memory is possible but harmless: the checks still apply.

**Tagged words.** The data stack and the global segments keep a type tag for
every 8-byte word, recording what it was last written as. Raw heap blocks
have none.

* A word can be read as a reference only if its tag says reference (or it
  was never written, when it reads as null).
* A reference word can be read as nothing else, and `memcpy` refuses to copy
  one.

So references are unforgeable, and the collector knows exactly which words
hold them. Raw memory cannot hold references: a handle (`hnew`) refers to an
object from there.

**What the representation guarantees.** The verifier proves register
indices, branch targets and call shapes once. Pointers and tags make every
memory access safe at run time. Together they mean the interpreter never
executes an unchecked access, and every runtime fault is a catchable `Error`
with a stable code ([ERRORS.md](ERRORS.md)). It is never a crash.

## 10. The heap and the collector

**Objects** (`src/heap/object.h`) are strings, byte buffers, typed arrays,
structs declared in assembly, dynamically typed lists, insertion-ordered
maps, closures, BigInts, decimals, foreign handles and coroutines. Each
starts with an `Obj` header: a kind, flags, and the size class it came from.

* Constant-pool strings, BigInts and decimals are **immortal**. They are
  owned by the VM, shared read-only by every process, and never collected or
  copied.
* Struct types are `StructDef`s owned by their module. A field reference
  `R1->Point.x` is resolved at link time to a type and an index, and checked
  against the object's type at run time.
* Lists and maps carry a type tag for each value, so they hold any mix of
  types and stay precise.

**One heap per process, and a precise, non-moving mark-sweep collector**
(`src/heap/gc.c`).

* **Roots** are:
  * every live frame's `R` window;
  * the data stack's reference-tagged words, scanned from the lowest word
    ever tagged as a reference (code may store references below `SP`);
  * every global segment's reference-tagged words;
  * the handle table, the mailbox, the exception being raised, pinned
    objects, and the running coroutine and its resumers.
* **No locks.** A collection only ever happens between instructions, and
  only on the process's own thread, so it needs no barrier, no locking and
  no stack maps. A heap is never shared, so it is never collected while
  another thread uses it.
* **When.** A collection runs when the heap has doubled since the last one
  (at least `--gc-min`, 1 MiB by default), or earlier as a heap quota
  approaches. The heap may always grow by an eighth of its live data before
  the next collection, so a process near its quota collects a few times and
  then gets `E_QUOTA`, instead of collecting at every allocation.

**The small-object allocator.** Objects of up to 256 bytes come from the
process's own chunks, in 16-byte size classes with a free list each.

* Allocation pops a free list or carves from the current chunk, with no lock
  and no `malloc`.
* The sweep walks chunks in address order and rebuilds the free lists in
  address order, so later allocation runs through memory in order too.
* Chunks grow from 1 KiB to 64 KiB, so ten thousand small processes stay
  small.
* A short list keeps its values and tags inside its own allocation.
* Because heaps are never shared, a process's chunks are simply freed when
  it ends.
* AddressSanitizer builds and `MLWSVM_NO_SLABS=1` use `malloc` for every
  object, so each object's lifetime is visible.

**Messages are copied, not shared.** `msg_copy` (`src/heap/copy.c`)
deep-copies a value into a detached list of objects, keeping sharing and
cycles (an old-to-new map and a worklist, with no recursion). The receiver
links that list into its own heap on its own thread. Immortal constants are
shared. Raw pointers, foreign handles and coroutines belong to their
process, and sending one traps.

**BigInt and decimal** (`src/heap/numeric.c`) are immutable heap objects held
in `R` registers. The arithmetic works on 32-bit limbs in temporary buffers,
and division uses Knuth's Algorithm D. Temporaries are freed before the
result is charged to the heap, so a quota trap leaks nothing.

## 11. Exceptions and traps

**Handlers are tables, not code.** A `.try` region becomes a
`[start, end) -> handler` entry beside the function's code. A region costs
nothing until something is thrown.

**Throwing.** `throw` raises any non-null reference. A runtime fault raises a
new built-in `Error` struct, with fields `code`, `message` and `trace`.
Either way, the VM builds a backtrace where the exception is raised, and
`longjmp`s to the jump buffer that `proc_run_slice` set at the start of the
time slice. `setjmp` therefore runs once per slice, not per instruction or
per call. On macOS and the BSDs the VM uses `_setjmp`, which does not save
the signal mask with a system call.

**Unwinding** (`src/vm/process.c`) looks for a handler, frame by frame:

1. In each frame, the faulting instruction is the one that was executing (in
   outer frames, the call in progress).
2. The smallest handler region containing that instruction wins, so nesting
   works in any declaration order.
3. A frame with no matching region is popped, exactly as a `ret` would pop
   it, and the search goes on in the caller.
4. Past a coroutine's first frame, the coroutine dies and the search goes on
   at the `coresume` that ran it.
5. Past a C callback's frames, the exception is held. It is raised again
   after the native returns, because foreign code is never unwound (§15).

An uncaught exception ends the process with its code, its message and the
backtrace from where it was thrown, naming source lines (also through `.loc`
for translated programs).

## 12. Processes and the scheduler

**A process owns everything mutable** ([ABI.md §1](ABI.md#1-processes)):

* its register stack and frames;
* a data stack (1 MiB by default, allocated on first use);
* its own copy of every module's global segment;
* raw heap blocks;
* its GC heap and handle table;
* its mailbox, quotas and statistics.

A waiting process takes about 4 KB. Spawning one, waking it and ending it
each take a microsecond or two.

**The scheduler** (`src/vm/sched.c`):

* **Preemption by instruction count.** N worker threads (one per core by
  default) each run one process at a time, for a slice of 2,000 instructions
  or until it blocks. An endless loop never starves the others.
* **Per-worker run queues with work stealing.** An idle worker takes half of
  another's queue. A worker with nothing to take sleeps.
* **A woken process stays where it was woken.** A receiver goes on the
  sender's queue, so a pair of processes talking to each other stays on one
  thread. Another worker is woken only when a queue has a backlog, and only
  one at a time while a woken worker is on its way.
* **A shared timer heap** for `sleep` and `recvt`. A worker reads it only
  when the earliest timer is due, which costs one atomic load per slice.
* **Determinism.** With one worker, scheduling is deterministic round robin.
  The debugger, tracer and profiler use one worker.

**Locking.** Processes share nothing, so a running process touches only its
own state and the interpreter takes no locks. Locks guard only what other
threads touch: a process's inbox, links, monitors and scheduling fields
(marked `[L]` in `vm.h`), the run queues, the timers and the VM's counters.

* The process table is an array of fixed-size chunks that never move, so a
  lookup takes only a stripe lock.
* A lookup pins a process by taking its lock. A process is freed only after
  it has left the table and its lock has been taken once more.
* Locks are always taken in a fixed order (documented at the top of
  `sched.c`), so they cannot deadlock.
* Short critical sections use `os_unfair_lock` on macOS.

**Messages.** `send` copies the value into a detached block (§10) and appends
it to the receiver's inbox under the receiver's lock. The receiver moves its
inbox into its mailbox on its own thread. Messages from one process to
another arrive in order, and before the `Down` that is sent when the sender
ends.

**Failure** follows Erlang:

* `link` makes two processes fail together.
* `monitor` delivers a `Down` struct when the watched process ends.
* A process that fails with nobody watching prints a crash report, so no
  failure is silent.
* By the time a `Down` arrives, the process's files, sockets and registered
  names are released, so a supervisor (`std.supervisor`) can restart it at
  once.

**Quotas** limit a process's heap, raw memory, mailbox, instructions, foreign
calls and OS calls. A process can lower its own limits but never raise them,
and children inherit them. A sandbox is therefore a process that sets quotas
and then calls the untrusted code.

**Deadlock.** If `main` waits in `recv` while every other process is waiting
too, with no I/O, timer or blocking call pending, no message can ever arrive,
so `main` gets a catchable `E_DEADLOCK`. An embedding host that may send
messages (`mlws_start`) turns this check off.

**Several programs in one VM.** `mlwsvm run a.masm --also b.masm` starts each
program's `main` as its own process.

* Each program has its own home directory, `os.args` and processes.
* They share the scheduler, the library-path modules, one version of each
  library and the name registry.
* When one program's `main` ends, that program's other processes stop. The
  run ends with the last program.

**Output** is written a whole line at a time, so lines from processes
running at once never mix. A line waits in the process until its newline,
and is written early when the process stops running for any reason other
than its slice ending.

## 13. Input and output

I/O never holds a worker thread ([STDLIB.md](STDLIB.md#how-io-waits)).
Natives that can wait are *checked* natives. Instead of blocking, they tell
the scheduler what they need (`Process.native_req`), and are run again
later, with `Process.resumed` set:

* **Wait for a descriptor.** Pipes and sockets the VM created are
  non-blocking. When one is not ready, the process waits in the **poller**, a
  thread of its own (kqueue on macOS and the BSDs, epoll on Linux), exactly
  as it would wait in `recv`. 10,000 open connections are 10,000 small
  waiting processes, not 10,000 threads.
* **Run on the I/O pool.** Regular files and inherited descriptors cannot be
  polled, so those calls run on a pool of I/O threads while the process
  waits. Natives declared blocking (`NF_BLOCKING`) go there too.

A wait for I/O is never mistaken for a deadlock. A process killed while it
waits for I/O stops at once. One in a call on the pool stops when that call
returns. Descriptors can be handed to another process (`fs.give` and
`fs.take`), through an unguessable ticket.

## 14. Coroutines

A coroutine is a second line of execution inside a process. It has its own
register stack, frames and data stack, and shares the process's heap,
globals and mailbox ([ABI.md §8](ABI.md#8-coroutines)).

* **The process always holds the running context.** A switch (`coro_swap`)
  exchanges about fifteen fields between the `Process` and the coroutine
  object. The rest of the VM (the collector, traps, the tools) never needs
  to know a coroutine is running.
* **Resumers form a chain** through `ObjCoro.prev`. Each one holds the
  context below it, so coroutines can resume coroutines.
* **Each data stack gets its own generation**, so a pointer into a
  suspended coroutine's stack, or into the resumer's stack from inside a
  coroutine, fails the usual pointer check.
* **Coroutines are heap objects.** One that nothing refers to is collected
  with everything it holds, even while it is suspended.
* **The switches run outside the fast dispatch** (§7). They cost about as
  much as a few generic instructions: a resume and a yield take 35 ns.

## 15. Natives, extensions and the FFI

**Natives** are C functions with a declared signature (`NativeDef`): up to 8
typed arguments, up to 4 results, and flags. Built-in natives are registered
by name in the VM (`io.*`, `text.*`, `json.*`, `fs.*`, `os.*`, `net.*`,
`proc.*`, `module.*` and more), and code calls them with `ncall`. Most take
the fast path: they are called directly with argument and result words. The
flags mark the others:

* *foreign*: from an extension or the FFI, counted by quota 4;
* *blocking*: run on the I/O pool;
* *serial*: one call at a time per library;
* *checked*: reports errors with `native_fail` instead of trapping;
* *OS*: reaches the operating system, counted by quota 5.

**Foreign code is never unwound.** C code may hold locks or resources, so
nothing traps while it runs (`src/native/extapi.c`). API misuse is recorded,
heap quotas are suspended (`in_foreign`), and the error is raised once the
foreign code returns. Values need no rooting, because the collector only
runs between instructions.

**Extensions** (`.mlwsx`) are shared libraries written against a small,
versioned C API (`include/mlwsvm/mlwsvm.h`). **The declarative FFI**
(`.extern`, `src/native/ffi.c`) calls C functions directly through libffi:

* integers, floats and strings;
* byte buffers the callee may change;
* foreign handles with finalizers (`.ffi_free`);
* C structs by value and by pointer (`.cstruct`);
* variadic functions;
* closures passed as C function pointers (`.callback`).

A callback runs on the calling process (`proc_callback`), on top of its
frames, and cannot wait. A call from another C thread is delivered as a
message instead (`ffi.async_callback`). `mlwsvm bindgen` writes `.extern`
declarations from a C header, using Clang.

**Trust** ([EXTENDING.md](EXTENDING.md#security)). Native code is the only
way around isolation, so loading it is a deliberate act
(`src/vm/extensions.c`):

* Native code loads only from trusted directories, never from a program's
  own directory.
* Each extension is pinned by its SHA-256 in a `TRUSTED` manifest.
* The directory, the manifest and each extension must not be writable by
  other users.
* The FFI may load only the libraries a manifest allows.
* Quota 4 shuts native code out of a sandbox entirely.

## 16. Embedding

`libmlwsvm` (`include/mlwsvm/embed.h`, `src/embed/embed.c`) is a thin layer
over the same VM, loader and run helper the command line uses
([EMBEDDING.md](EMBEDDING.md)). A host:

* loads modules from files or from source in memory;
* defines host functions as natives;
* calls exported functions (`mlws_call`);
* starts long-lived services it feeds with messages (`mlws_start`,
  `mlws_send`, `mlws_wait`).

Each call runs as a fresh main process with fresh module globals, so calls do
not interfere with each other. Values cross the boundary as numbers and
strings. Errors come back as codes, messages and backtraces. MLWSVM code
cannot crash the host, because the same checks apply. A message from another
host thread is built on a scratch heap and delivered like any other
(`sched_post`).

## 17. The optimizer

The optimizer (`src/opt/`, [OPTIMIZER.md](OPTIMIZER.md)) rewrites a
`Module` before it is linked: when it loads (`--opt`) or ahead of time
(`mlwsvm opt`). Working on modules means it applies equally to `.masm`,
`.mlwso` and archive modules, and its output is an ordinary module that the
verifier checks like any other.

* **A working form per function** (`ir.c`, `OFunc`). Instructions are
  decoded into `Insn`s, keeping their source lines and `.loc` runs. Each
  instruction's register effects (`src/core/regflow.c`, the same code the
  verifier uses) give a control-flow graph. The result is re-encoded,
  remapping labels, handlers, lines and register counts. Decoding and
  encoding with no passes reproduces a module byte for byte.
* **Passes**:
  * constant folding, using `arith.h`;
  * constant and copy propagation, including what each branch's test
    establishes on each of its edges;
  * algebraic simplification;
  * reuse of repeated computations and loads within a block and along
    single-predecessor chains (`cse`);
  * dead code and unreachable code removal;
  * branch threading, inversion and deciding;
  * unused functions;
  * register compaction;
  * inlining;
  * tail calls.
* **Levels are defined by who can tell the difference.**
  * `-O1` changes only the number of instructions run.
  * `-O2` also changes what only the tools see: unused functions removed,
    smaller register windows.
  * `-O3` adds inlining, so an inlined call has no frame in a backtrace.
  * `tailcall` is in no level, because it removes the caller's frame.
* **What keeps it sound**:
  * anything that can trap, allocate, call or touch memory stays in place;
  * a store that may release an object stays;
  * values an exception handler can read stay;
  * stored values keep their type tags;
  * constants go into an operand only where the instruction's own checks
    accept them.
* **It reports what it did**: per pass and per function, and with `-v`,
  every change with its source line.

## 18. The tools

The tools are built into the one `mlwsvm` binary and reach the VM through two
hooks ([TOOLING.md](TOOLING.md)):

* **The instruction hook** (`VM.hook`) is called before every instruction
  while a tool is active, and can stop the scheduler. The **debugger**
  (gdb-style: breakpoints, stepping, `bt`, `print`), the **tracer** and the
  exact **profiler** (counts per instruction, flame graphs, and the list of
  instructions that ran generically) use it. They run the program on one
  worker. Whether a hook is installed is read once per slice, so without one
  the dispatch pays nothing.
* **The poll hook** (`VM.poll`) is called by worker 0 between slices. The
  **monitor** uses it: a small HTTP server with no thread of its own. It
  serves a JSON API, a web dashboard (`web/`, compiled into the binary) and
  the table that `mlwsvm top` shows. For each request it stops the other
  workers at a slice boundary (`sched_stop_world`), so it can read every
  process safely.

**Source positions.** Every instruction knows its assembly line. A compiler
can also mark which line of its own source each stretch of code came from
(`.loc "fib.lua", 3`). Backtraces, the debugger and the profiler show both.

**The rest of the toolchain:**

* `asm` writes `.mlwso`, and `-E` shows the preprocessed source;
* `dis` turns a module back into assembly that assembles to the same module;
* `opt` optimizes ahead of time, with a report;
* `lib` builds and lists library archives;
* `ext` manages trusted native code;
* `bindgen` writes FFI declarations from C headers;
* `doc` generates `docs/ISA.md` and `docs/NATIVES.md`;
* `test` runs assembly test files.

## 19. The platform layer

`src/platform/platform.h` is the only OS-specific interface, implemented by
`posix.c`, `posix_io.c` and `win32.c`. It covers:

* threads, mutexes, condition variables and the short-section locks;
* clocks and sleeping;
* files, directories, modification times and real paths;
* processes and pipes;
* sockets and the poller (kqueue or epoll);
* dynamic loading;
* the permission checks behind trusted directories.

Everything else is plain C11 with `<stdatomic.h>`. Byte order is fixed:
memory is little-endian on every host, and the binary formats spell out
their byte order. The Windows implementation is written but untested
([ROADMAP.md](ROADMAP.md#3-platform)).

## 20. Testing

**Tests are MLWSVM programs.** `tests/` holds the suite (293 programs) and
`examples/` holds self-checking examples, all in documented assembly.

The test runner (`src/tools/testrunner.c`) reads expectations from comments
in each test file:

* `; expect-stdout:` for each expected line of output;
* `; expect-exit:` for the exit code;
* `; expect-error:` for the error code from the assembler, loader or a trap;
* `; option:` for VM options;
* `; debug-script:` to run the test under the debugger;
* `; expect-opt:` and `; expect-insns:` for what the optimizer did;
* `; no-opt:`, with a reason, for a test that must not be optimized;
* `; skip:`, with a reason.

Directories whose names start with `_` hold helper modules.

**The same suite runs in many modes.** Each mode targets a class of bug:

| Mode | What it catches |
|---|---|
| `make test` | Wrong results |
| `make check-gc` | A collection at every allocation: missing roots, and objects used after they are freed (freed memory is filled with a pattern) |
| `make check-opt` | Every module optimized with every pass, also under GC stress: optimizer bugs, on all the suite's code |
| `make check-asan` | AddressSanitizer and UBSan: memory errors and undefined behaviour in the VM itself |
| `make check-tsan` | ThreadSanitizer, on several worker threads: data races |
| `make check-poison` | New register windows filled with garbage: registers read before they are written but not cleared |
| `make check-switch`, `check-loop` | The fallback dispatch builds |
| a build without libffi | The FFI's tests are skipped cleanly |
| `make check-tools` | The tools: the monitor, `top`, the profiler, the tracer, `.loc`, the module cache, archives, the optimizer's round trips and reports |
| `make check-embed`, `check-bindgen` | The embedding library and its example host, and generated bindings |

Unicode support is checked against the Unicode Consortium's own
normalization and segmentation test files. CI runs the modes on Linux (GCC
and Clang) and macOS (Clang). The compilers project (`mlws-compilers`)
generates large amounts of real code, and its suites are a second,
independent test of the VM and the optimizer.

## 21. What is left out, and why

Each of these was considered, and most were measured
([PERFORMANCE.md](PERFORMANCE.md#what-is-not-done-and-why),
[ROADMAP.md](ROADMAP.md)).

* **Shared mutable state between processes.** Isolation is the design. It
  keeps the collector lock-free and failure contained. Constants and loaded
  code are the only shared things.
* **Unloading modules.** Closures may refer to any loaded function, so
  loaded code lives as long as the VM.
* **Turning integers into pointers.** That would defeat the checks of §9.
  Raw memory refers to objects through handles.
* **A generational collector.** One was built and measured. On the
  allocation-heavy benchmark where collection matters most, it saved at most
  a quarter of the collection time, and it was much slower on large
  containers written now and then. It would need card marking.
* **Calls without copying.** Overlapping windows, as in Lua, would save a
  word copy per argument, but need an ABI change. That waits until
  call-heavy programs show the copies.
* **A fixed three-operand format.** Measured no faster, with slower calls.
* **A compact decoded form for every instruction.** Quickened forms already
  carry one. The 72-byte generic `Operand` is the fix to make if large
  programs show cache misses.
* **Removing type checks in the optimizer.** That needs the source
  language's types, so it stays in the compilers.
* **Lock-free run queues and spinning before sleeping.** The queue locks
  are almost always uncontended, and waking one thread at a time avoided the
  latency problem in every benchmark so far.
* **Windows.** Written, untested: it needs CI, MSVC or clang-cl builds, and
  an IOCP poller.
