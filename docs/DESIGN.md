# The Design of MLWSVM

This document describes how MLWSVM is built: its goals, the path a program
takes from source text to running instructions, and, for each part, the main
data structures, the algorithms that work on them, and how the parts hand
data to each other. The reference documents it links to cover what a user of
each part needs: every instruction, operand, option and error code. Source
files are named as `file.c:function` so each statement can be checked
against the code.

## Contents

1. [Goals](#1-goals)
2. [The whole system](#2-the-whole-system)
3. [The instruction set](#3-the-instruction-set)
4. [Modules and the binary format](#4-modules-and-the-binary-format)
5. [The assembler](#5-the-assembler)
6. [Loading and linking](#6-loading-and-linking)
7. [The verifier](#7-the-verifier)
8. [The interpreter](#8-the-interpreter)
9. [A process in memory](#9-a-process-in-memory)
10. [Pointers, segments and tags](#10-pointers-segments-and-tags)
11. [The heap and the collector](#11-the-heap-and-the-collector)
12. [Exceptions and traps](#12-exceptions-and-traps)
13. [Processes and the scheduler](#13-processes-and-the-scheduler)
14. [Messages](#14-messages)
15. [Input and output](#15-input-and-output)
16. [Coroutines](#16-coroutines)
17. [Natives, extensions and the FFI](#17-natives-extensions-and-the-ffi)
18. [Embedding](#18-embedding)
19. [The optimizer](#19-the-optimizer)
20. [The tools](#20-the-tools)
21. [The platform layer](#21-the-platform-layer)
22. [Testing](#22-testing)
23. [What is left out, and why](#23-what-is-left-out-and-why)

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
* **Safe by construction.** No program, however wrong or hostile, may crash
  the VM, read memory that is not its own, or forge a reference. Each check
  happens either once, at load time (the verifier), or is built into the
  representation (checked pointers, tagged words), so the interpreter does
  not have to trust the code it runs.
* **Isolation through processes.** Concurrency means many small processes
  that share nothing mutable and talk by copying messages, as in Erlang. That
  keeps the collector, the scheduler and the safety argument simple: no heap
  is ever touched by two threads.
* **Portable C11, nothing required.** The VM builds with any C11 compiler.
  libffi is optional; without it, only `.extern` is missing. All OS-specific
  code is behind one header (§21).
* **Measured, not guessed.** Every performance technique was chosen from a
  profile and kept only if it measured faster on three machines. Ideas that
  measured badly are written down with their numbers
  ([PERFORMANCE.md](PERFORMANCE.md#what-is-not-done-and-why)).
* **One source of truth.** The instruction set is one table
  (`src/core/opcodes.def`), from which the assembler, disassembler, checker,
  verifier, optimizer and `docs/ISA.md` all derive what they know. The native
  registry generates `docs/NATIVES.md` the same way.
* **Tested in its own language.** The test suite is written in MLWSVM
  assembly, and every test mode runs all of it (§22).

## 2. The whole system

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
                    register_module: find imports, choose library
                    versions; link_module: names to objects, load
                    native code, decode, verify, quicken    (src/vm/loader.c)
                                          │
                                          ▼
             LModule { Function { Insn[] } }  immutable, shared by all threads
                                          │
       processes ──► scheduler: N worker threads, run queues, stealing
                     │   └─ the interpreter runs a time slice of one process
                     ├─ poller thread: processes waiting for sockets and pipes
                     └─ I/O thread pool: blocking native calls
```

A **module** is the unit of everything. It is what the assembler produces,
what a `.mlwso` file holds, what the cache stores, what the optimizer
rewrites and what the loader links. `mlwsvm dis` turns a module back into
assembly.

**Loaded code is immutable and shared** by every process and thread.
Everything that changes belongs to exactly one process. This one rule is
what lets the interpreter and the collector run without locks.

### 2.1 The main data structures

| Structure | Defined in | What it is | Owned by | Lives |
|---|---|---|---|---|
| `OpInfo` | `core/opcodes.h` | One instruction's description, from `opcodes.def` | static table | forever |
| `Module` | `core/module.h` | A module's tables and encoded code, as on disk | the `LModule` | until the VM ends |
| `LModule` | `vm/vm.h` | A loaded module: its `Module`, resolved tables, `Function`s | `VM.mods` | until the VM ends |
| `Function` | `vm/vm.h` | A function ready to run: decoded `Insn[]`, window layout, clear run | its `LModule` | until the VM ends |
| `Insn`, `Operand` | `core/insn.h` | A decoded instruction (32 bytes) and its operands (72 bytes each) | its `Function` | until the VM ends |
| `VM` | `vm/vm.h` | Modules, natives, libraries, options, programs, the scheduler | the host | one run |
| `Process` | `vm/vm.h` | Everything mutable: registers, frames, stack, globals, heap, mailbox | the scheduler's table | spawn to exit |
| `Frame` | `vm/vm.h` | One call: function, pc, window base, saved `SP`/`FP` | its process | call to return |
| `Segment` | `vm/vm.h` | A block of addressable memory: a stack, globals, a raw block | its process | varies |
| `Obj` | `heap/object.h` | The header of every heap object | its process's heap | until collected |
| `Sched`, `Worker` | `vm/sched.c` | Process table, run queues, timers, threads | the `VM` | one run |
| `OFunc`, `OCfg` | `opt/opt_internal.h` | The optimizer's working form of a function, and its basic blocks | the optimizer | one optimization |

### 2.2 The source tree

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

### 3.1 The opcode table

Each line of `src/core/opcodes.def` is an X-macro entry:

```c
OP(ADD,    0x0020, "add",    "Arithmetic", "DS+", TS_NUM | TS_PTR | TS_BIGDEC, 0,
   "Sum. `add dst, s` means dst += s; `add dst, s1, s2, ...` means dst = s1 + s2 + ....")
```

It is expanded twice:

* `opcodes.h` makes the dense enum `Opcode` (`OP_ADD`, ..., `OP__COUNT`,
  130 instructions). The dense index is what every other part uses.
* `opcodes.c` makes `OpInfo op_info[OP__COUNT]`: the stable 16-bit `code`
  written to files, the mnemonic, category, pattern, type set (a bit mask of
  the type suffixes allowed; 0 means none), flags and documentation.

`op_init()` derives the rest once: the pattern's letters without its
modifier (`elems`, `nelems`), the modifier (`?`, `*` or `+`), and from them
the operand count range `minops..maxops`. It also checks that the codes are
strictly increasing, which lets `op_from_code` turn a code read from a file
into the dense index by binary search. Codes are never renumbered.

**The pattern** gives each operand position a letter:

| Letter | Operand | Read or written as |
|---|---|---|
| `D` `S` | destination, source | the instruction's type |
| `W` `V` | destination, source | the instruction's type, or the operand's natural type when untyped |
| `B` | destination | `b` |
| `Q` `J` | source, destination | `i64` |
| `H` | destination | `f64` |
| `A` `E` | source, destination | `p` |
| `O` `X` | source, destination | `r` |
| `M` | a memory location | |
| `K` `I` | a string constant; a non-negative integer immediate | |
| `L` `F` `N` `T` | a label, function, native, struct type | |
| `C` | a call block | |

A trailing `?`, `*` or `+` applies to the last letter. `op_pattern_at(oi, k)`
gives the letter for position `k`, repeating the last one for `*` and `+`.
Everything that needs an instruction's shape reads the pattern:

* `core/check.c:check_insn` decides which addressing modes each position
  accepts and checks register banks against types and writability;
* `core/regflow.c:insn_effect` decides which registers an instruction reads
  and writes (the letters `DWBEXJH` are destinations);
* the assembler parses each position by its letter;
* the disassembler, the optimizer and `mlwsvm doc isa` read it too.

**Flags**: `OPF_TERM` (never falls through: `jmp`, `ret`, `tcall`, `throw`,
`halt`), `OPF_NATURAL` (the untyped form keeps operands' natural types:
`mov`, `swap`), `OPF_TYPE2` (a second type byte follows: `cvt`, `bitcast`),
`OPF_CALL` (the operands are a call block: target, argument count,
arguments, results), `OPF_PERVAL` (each operand is typed on its own when the
instruction is untyped).

### 3.2 Types, banks and canonical values

An instruction's type is a suffix: `b c i8 i16 i32 i64 u8 u16 u32 u64 f32
f64 r p big dec`. There are four register banks of 32 registers: `I`
(integers), `F` (floats), `R` (references the collector traces) and `P`
(checked pointers), plus `SP`, `FP` and `PID`. A register is one byte: the
bank in the top 3 bits and the index in the low 5 (`REG_MAKE`). Each type
belongs to one bank.

* Integers are kept **canonical**: a register holds the 64-bit value of the
  type last written, sign- or zero-extended (`canon_int`); an `f32` is kept
  rounded to float (`canon_float`). So narrow arithmetic wraps correctly,
  and an `i64` handler never looks at the type.
* References have their own bank, which is what makes the collector precise
  without a tag on every register (§11).

### 3.3 Variable arity and operands

An instruction has 0 to 32 operands (`MAX_OPERANDS`). `add I0, I1, I2, I3`
folds left to right, and `call f (a, b) -> (q, r)` carries its arguments and
results in one instruction. A fixed three-operand format measured no faster,
and its calls were 13–17% slower
([REGISTER_VS_STACK.md, part 4](REGISTER_VS_STACK.md#part-4-up-to-32-operands-against-a-fixed-3)).

Any operand can be in memory, including the destination
([ADDRESSING.md](ADDRESSING.md)). The operand modes (`OperandMode` in
`core/insn.h`):

| Mode | Syntax | Payload |
|---|---|---|
| `OM_REG` | `I3`, `SP` | `reg:u8` |
| `OM_IMM`, `OM_FIMM` | `42`, `3.5` | `sleb`; `f64le` |
| `OM_CONST` | `"text"`, `12n`, `1.5d` | constant index |
| `OM_IND`, `OM_DISP`, `OM_DISPB` | `[P1]`, `SP[4]`, `P1{13}` | `reg`, displacement (words or bytes) |
| `OM_IDX`, `OM_IDXB` | `P1[I2*2+1]` | `reg ireg scale disp` |
| `OM_POSTINC`, `OM_PREDEC` (and `B` forms) | `[SP]+`, `-[SP]` | `reg` |
| `OM_GLOBAL`, `OM_GLOBALIDX` (and `B` forms) | `@x[3]`, `@x[I1]` | global reference index, displacement |
| `OM_MIND`, `OM_MINDB` | `[FP[2]][3]` | an inner memory operand, displacement |
| `OM_LABEL` | `loop` | instruction index in the function |
| `OM_FUNC`, `OM_NATIVE`, `OM_STRUCT` | `fib`, `"io.print"`, `Point` | table index |
| `OM_ELEM`, `OM_ELEMI` | `R2[I4+1]`, `R2[7]` | `reg ireg disp`; `reg index` |
| `OM_FIELD` | `R2->Point.x` | `reg`, field reference index |
| `OM_UPVAL` | `UP[2]` | capture index |

`ARG[n]` and `LOC[n]` are spelled as `FP[n]` and `FP[-1-n]` (`OM_DISP`).

### 3.4 Encoding

```
insn    := opcode:u16le  type:u8  [type2:u8 if OPF_TYPE2]  nops:u8  operand*
operand := mode:u8  payload
```

Immediates and displacements are signed LEB128, indices unsigned LEB128
(`core/bytes.c`), so code is compact and has no alignment. Nothing in the
encoding is a machine address: functions, globals, constants, natives,
struct types, fields and labels are indices. For example, from
`bench/fib.masm`:

```
blt.i64 I0, 2, .L7        75 00 | 06 | 03 | 01 00 | 02 02 | 14 07
                          op      i64  3    REG I0  IMM 2   LABEL 7
call fib (I1) -> (I2)     80 00 | 00 | 04 | 15 00 | 02 01 | 01 01 | 01 02
                          op      -    4    FUNC 0  argc 1  REG I1  REG I2
```

## 4. Modules and the binary format

`Module` (`core/module.h`) is the in-memory form of a module. Everything in
it is a table, and code refers to the tables by index:

```
Module
  name, source, srcfiles[]          dotted name; the file; files .loc names and included files
  imports[]                         module names
  consts[]   {kind, bytes}          strings, BigInts, decimals (as text)
  funcs[]    MFunc                  see below; imports are entries too
  syms[]     {name, offset, size, flags}       data symbols in the globals
  grefs[]    {LOCAL offset | IMPORT module, name}   what @name operands use
  natives[]                         native names
  structs[]  {name, fields+types | IMPORT module | BUILTIN}
  frefs[]    {struct index, field name}        what R->T.f operands use
  gdata[gsize], gtags[gsize/8]      the global segment's first image, a tag per word
  code[codelen]                     every function's encoded instructions
  extensions[], externs[], cstructs[], ffrees[], callbacks[]   native code
  requires[] {lib, constraint}; lib_name, lib_version, publics[]   libraries

MFunc
  name, flags (EXPORT, IMPORT, RETS_DECLARED), import_idx
  nparams, params[], nrets, rets[]  types
  nregs[4]                          window size per bank
  code_off, code_len, ninsns        its bytes in Module.code
  file                              its .masm file: source, or an included one (srcfiles)
  lines[ninsns]                     the line of each instruction in that file
  locs[]  {start, file, line}       .loc runs: from instruction `start` on
  handlers[] {start, end, target}   .try regions, as instruction indices
```

Every reference outside the module is **by name**: an imported function is
an `MFunc` with the `IMPORT` flag, its module's index and its name; an
imported global is a `GREF_IMPORT`; an imported struct is an `MSF_IMPORT`
entry. So a module is complete in itself. It can be assembled, cached,
optimized or archived alone, moved on disk (its name never says where its
file is), and loaded into any VM.

**The `.mlwso` file** (`loader/module_io.c:module_write`) is the magic
`MLWS`, a format version, then the tables in the order above, each a
LEB128 count and its items, strings as a length and bytes. `module_read`
checks everything a later stage relies on: the exact format version; every
count against the bytes left before allocating; every index into another
table; parameter, result and field types; register counts (at most 32);
handler and `.loc` ranges; each function's code range; symbol ranges; the
global segment's word tags (numbers and raw bytes only, so a file cannot
plant a reference); and no trailing bytes. It does not decode the code: that is the linker's job,
followed by the verifier (§6, §7).

**A library archive** (`.mlwsl`, `loader/library_io.c`) is the magic
`MLWL`, the manifest (a module image holding `.library`, `.requires` and
`.public`), and a table of `{name, module image}`. `lib_read` reads the
manifest and the table; `lib_module` decodes one module only when it is
imported ([BINARY_FORMAT.md](BINARY_FORMAT.md)).

## 5. The assembler

The assembler (`src/asm/`) is a pipeline of three stages. The preprocessor
runs over the whole file first, then the assembler parses each line it
produced, then `finish()` resolves the module and encodes it.

### 5.1 The lexer

`lexer.c:lex_line` turns one line into tokens, after cutting the comment
(the first `;` outside quotes). Identifiers may contain dots, so `add.i64`,
`.func` and `math.gcd` are one token each. Numbers are decimal, `0x`, `0b`
or `0o`, with `_` separators; `123n` and `1.5d` are BigInt and decimal
literals; `1b` and `2f` are references to numeric local labels. Two-character
operators (`->`, `<<`, `==`, ...) are matched first.

### 5.2 The preprocessor

`preproc.c` turns the file into a flat vector of lines, each a `PLine`:
its text, `loc` (where the text physically is), `origin` (the line of the
user's file it came from) and `macro` (the innermost macro it came from).
Error messages use these to say both where the faulty text is and which
line expanded it.

* **Conditionals.** Each `pp_lines` call keeps a stack of up to 64
  `{parent_active, active, taken, seen_else}` entries. Conditional
  directives are processed even in inactive regions, so nesting is tracked;
  everything else there is skipped.
* **Macros.** `.macro` stores its body unexpanded, with its parameters,
  defaults and an optional variadic last parameter. A line whose first word
  (after any labels) is a macro name is expanded: arguments are split at
  top-level commas, then `subst` replaces `\name` with an argument, `\@` with
  a number unique to this expansion (for labels), and `\()` with nothing.
  The result is run through the preprocessor again, so macros may use
  macros, up to a depth of 64.
* **Repetition.** `.rept n[, var]` and `.irp var, values...` expand their
  body once per count or value, with the variable substituted.
* **Includes.** `.include "f"` looks in the directory of the file that
  contains the directive, then on the library path. Each file is included
  once, keyed by its real path.
* **Constants** (`.const`, `.equ`, `.set`) are evaluated values, not text.
  Each name keeps its values with the output line each applies from
  (`ConstDef`); the assembler reads names as of the line it is on
  (`PP.asof`), so a `.set` name has, on each line, its value there.

### 5.3 Expressions

`expr.c` evaluates constant expressions by precedence climbing: each binary
operator has a level (`||` 1 up to `* / %` 10), `parse_binary(minprec)`
parses a primary and then takes operators of at least that level, recursing
with one level more for the right-hand side, so operators associate left.
Integers are 64-bit and wrap; a float operand makes the result a float.
Names are looked up among the preprocessor's constants.

### 5.4 Parsing into a module

The assembler keeps an `Asm` for the module and an `AsmFunc` for the
function being assembled. Most of their tables are a vector plus a `StrMap`
from a key to the index, which also removes duplicates:

* constants, keyed by kind and quoted text;
* imports and their aliases;
* local functions, and imported functions (keyed `imp:name`, appended after
  the local ones);
* data symbols, global references, natives;
* structs (local, built-in or imported) and field references.

**Labels need no patching.** An operand that names something keeps the name
and is resolved later:

* At `.endf`, labels are resolved to instruction indices. A numeric
  reference `1b` takes the nearest label `1:` before it, `1f` the nearest
  after it (by statement order).
* At `.endf`, `.try` regions become `{start, end, target}` instruction
  ranges; the register count of each bank becomes the highest register used
  plus one, at least the parameter count, at least `.regs`; and the last
  instruction must not fall through.
* At `finish()`, function, global, struct and field names are resolved. A
  dotted name such as `geo.vec.length` is split by trying the longest prefix
  that is an import or alias.

**Types are inferred** when an instruction has no suffix: from the first
register operand's bank or a field's declared type, else from the first
source literal, else a default from the instruction's type set. `mov` and
`swap` stay untyped and use natural types.

**Data** directives append to the global image, and record each 8-byte
word's tag: a whole aligned word gets its type, a partial or unaligned
write marks it raw, and reserved space stays untagged.

**`.loc "file", line`** sets the translated program's position. At encode
time a new `{start, file, line}` run begins wherever the position changes.

**Every instruction is checked** by `core/check.c:check_insn`, the same
function the verifier uses, as soon as its types are known. The error then
names the line and column of the operand at fault.

`finish()` then encodes every function into one code buffer (`encode_insn`)
and moves the tables into a `Module`.

## 6. Loading and linking

`src/vm/loader.c` turns modules into runnable code.

### 6.1 Finding modules

An import `a.b` is the file `a/b.masm` or `a/b.mlwso` (the newer one if both
exist), looked for in:

1. the program's own directory, for the program's own modules;
2. each directory of the library path;
3. the library archives in those directories (§6.4).

A module found in a program's directory belongs to it. The VM knows each
module by a key (`module_key`): a library-path module by its name, a
program's module by `"\1" + realpath(directory) + "\1" + name`. One `Home`
(`dir`, `real`) is kept per program directory. So two programs from
different directories can each have a `util`, and a library-path module,
whose imports are only looked for on the library path, can never be given a
program's file in place of a library's.

### 6.2 Registering a module

```
require(name, home)                          find, or reuse, the module `name`
  ├─ modmap[key] is READY      → it
  ├─ modmap[key] is LOADING    → E_LINK_CYCLE, printing the `loading` stack
  ├─ a file in home, or on the library path → read_module_file
  │       .mlwso → module_read;  .masm → modcache_assemble (§6.5)
  └─ else an archive → require_from_library (§6.4)
        │
        ▼
register_module(m)
  1. a second module with the same key → E_LOAD_NAME, naming both files
  2. if --opt: m = modcache_optimize(m)                       (§19)
  3. LModule { index = mods.n, state = LOADING }; push to mods, modmap, `loading`
  4. state its .requires                                      (§6.4)
  5. for each import: require(import), check it may be imported (private modules)
  6. link_module
  7. pop `loading`; state = READY
```

Imports are linked before the module that imports them, depth first. A
module seen again while it is still `LOADING` is on the `loading` stack, so
it imports itself through the chain, and the error prints that chain.

### 6.3 Linking

`link_module` turns names into objects, in this order:

1. **Constants** are interned in the VM-wide pool `vm->const_pool`, keyed by
   kind and text: every module's `"name"` or `3n` is one immortal object,
   so identity tests agree across modules (§11.7).
2. **Global references** become checked pointers: a local one
   `PTR_MAKE(2 + module index, 1, offset)`, an imported one the exporting
   module's symbol. These are not host addresses: every process has its own
   copy of each global segment, at that segment number (§10).
3. **Structs**: a local one gets a `StructDef`, a built-in one (`Error`,
   `Down`) the VM's, an imported one the exporting module's.
4. **Native code** the module declares: extensions, then `.cstruct`,
   `.ffi_free`, `.callback` and `.extern`, each through the trust checks of
   §17.
5. **Natives** by name, from `vm->natives`.
6. **Field references** become `{StructDef, field index}`.
7. **Functions**: a local one targets itself; an imported one targets the
   exporting module's `Function` (`Function.target`).
8. **Decoding** (`decode_function`): each local function's bytes become an
   `Insn[]`, and `resolve_operand` fills each operand's linked fields:
   * a displacement and scale in bytes (`bdisp`, `bscale`);
   * `rv.ptr`: the constant's object, the target `Function`, the
     `NativeDef`, the `StructDef` or the `FieldRef`;
   * `rv.gbase`: a global's checked pointer;
   * `rv.target`: a label's instruction index, checked to be in the
     function.

   It also lays out the function's register window: `roff[b]` is where bank
   `b` starts (the banks one after another, `I` first) and `rsize` the whole
   window, in words.
9. **Verifying** the module (§7).
10. **Quickening** each function (§8.2).

Any failure stops the load before any of the module's code runs.

### 6.4 Libraries

Archives are found by `scan_archives`, which reads every `.mlwsl` in the
program directories and on the library path, once each, when an import
first misses the plain files. The version choice uses two structures:
`vm->wants`, every `{library, constraint, who}` stated so far, and
`vm->libsel`, the version chosen for each library.

* `want(lib, constraint, who)` checks the constraint at once if `lib` is the
  VM itself or already chosen, and otherwise records it.
* `choose_library(lib)`, on the first import of one of `lib`'s modules,
  takes the newest archive of `lib` that satisfies every recorded
  constraint, records the choice, and then states the chosen version's own
  `.requires` in turn. A constraint that cannot be met is `E_LINK_VERSION`,
  listing who asked for what.

When several programs start together, all their root modules' `.requires`
are stated before any import is followed, so the choice does not depend on
the order of the programs. A module the manifest does not list as `.public`
can be imported only by modules of the same library (`E_LINK_PRIVATE`).
Versions and constraints are parsed and compared by `core/semver.c`.

### 6.5 The module cache

`vm/modcache.c` keeps assembled modules, so a large generated program is
assembled once.

* **The key** of an entry is a SHA-256 of a label, the running executable
  (path, size and modification time, so a rebuilt VM never reads an older
  one's entries), the default module name and the source text. A compiler
  that writes the same program to a new temporary file each time still hits.
* **An entry** (`<hash>.mlwsc`) is the magic `MLWC`, a version, the
  included files, each with a SHA-256 of its contents, what their
  resolution depended on (the working directory, the source's directory and
  the search path), and then the module as a `.mlwso` image.
* **A hit** requires the same resolution context and the same hash for every
  included file.
* **Writing** goes to a temporary file that is renamed into place, so a
  reader never sees half an entry. Past 512 entries, the oldest are removed
  down to 384.
* **Optimized modules** are entries too, keyed by the executable, the set of
  passes and the module's bytes (less its source path), whether it came
  from a `.masm`, a `.mlwso` or an archive.
* **The directory** (`$MLWSVM_CACHE`, else the XDG cache directory) is made
  private and must pass the same trust check as native code, because a
  module can declare foreign functions.

### 6.6 Loading while a program runs

`module.load` (`native/modload.c`) loads a module by name while processes
run, under `vm->mods_lock`. Each process copies a module's globals when it
starts (`process.c:proc_new`), so a process that started before the load
finds the new segment missing the first time it touches it, and
`proc_grow_globals` adds it (§10.3). A load that fails marks the modules it
had not linked `LM_FAILED` and leaves their names free. Modules are never
unloaded, so a closure never outlives its code.

## 7. The verifier

`vm/verify.c` establishes, once, everything the interpreter relies on
without checking again. A `.mlwso` file from anywhere gets the same checks
as freshly assembled source. For each function (`verify_function`):

1. It has code, and each handler region is within it.
2. Each instruction passes `check_insn`: operand count; the addressing
   modes each position allows; register banks against types (a memory base
   must be a `P` register, `SP` or `FP`; an index an `I` register; an object
   base an `R` register); writability; a call block's shape.
3. Every register an operand names, including bases and indices, is inside
   the function's window for its bank.
4. A typed access to a struct field matches the field's bank.
5. Calls match their callee's signature: argument count; each argument
   readable as its parameter's type; each result writable as the declared
   result type; no more results taken than declared. The callee is the
   defining module's `Function` (linked in step 7 of §6), so this holds
   across modules. `spawn`, `closure`, `ncall` (against the `NativeDef`)
   and a `ret` in a function with declared results are checked the same way.
   Closure calls and coroutine switches, whose callee is known only at run
   time, are checked when they run.
6. The last instruction does not fall through.

Label targets and table indices were already checked by `resolve_operand`.

### 7.1 Planning the register window

A new frame's registers must read as zero. Clearing whole windows costs a
`memset` per call, so `plan_window` finds which registers really need it:

* **The analysis** is a forward must-analysis over instructions. The fact at
  each instruction is the set of registers **definitely written** on every
  path to it (a `RegSet`: one 32-bit mask per bank). The entry starts with
  the parameters (bound at every call). Each handler target starts from the
  entry set too (sound, since every path to it passes the entry).
* **The transfer** adds the registers the instruction always writes
  (`regflow.c:insn_effect`; a `recvt` writes nothing if it times out).
  **The meet** is intersection. Successors are the next instruction (unless
  `OPF_TERM`) and every label operand. A LIFO worklist runs to a fixpoint;
  sets only shrink, so it terminates.
* **The result**: `need` is the union, over every instruction, of the
  registers it reads that are not definitely written before it. `R`
  registers are always included, because the collector scans the whole `R`
  window. `zoff` and `zlen` are the one run of words from the first needed
  register to the last, in window order (`I`, `F`, `R`, `P`), and
  `read_first` records the `I`, `F` and `P` registers that made it longer,
  which the profiler shows.

A call then clears `zlen` words from `zoff` (§9.4). Compiled Lua's typed
`fib` clears nothing. `make check-poison` fills each new window with
garbage before clearing the run, so a register read before it is written
but not cleared makes a test fail.

## 8. The interpreter

### 8.1 Decoded instructions

Each function runs from an array of `Insn` (`core/insn.h`), decoded once at
load time:

```
Insn (32 bytes)                                   filled by
   0  u16 op          the dense opcode              decode
   2  u16 xop         what runs: op, or a quickened form     interp_quicken
   4  u8  type, type2                               decode
   6  u8  nops                                      decode
   8  Operand *ops    ──► Operand[nops] (+ a call plan)       decode, quicken
  16  u8  r[3]        a quickened form's registers           interp_quicken
  20  u32 target      its branch target, field or element index
  24  k {i64 | f64 | void *}  its immediate, struct type or call plan

Operand (72 bytes)
   0  u8 mode, reg, ireg, pad                       decode (pad: the register's word in the window)
   8  i64 imm         immediate, or displacement in encoded units
  16  u64 scale       index scale in encoded units
  24  u64 index       constant, global, label, function, native, struct, field or capture index
  32  f64 f           a float immediate
  40  Operand *inner  the inner operand of [mem][d]
  48  i64 bdisp       displacement in bytes          link
  56  bscale | kw     scale in bytes, or a quickened constant's word
  64  rv {ptr | gbase | target}   the linked object, global pointer or label
```

The real opcode stays in `op`, so the disassembler, tracer, profiler and
debugger see the instruction as written. The 72-byte operands serve the
generic path and the tools; the quickened handlers read only the 32-byte
`Insn`.

### 8.2 Quickening

`interp_quicken` gives common instruction shapes a specialised handler, as
the instruction's `xop`. The quickened forms are numbered after the real
opcodes (`Q__FIRST = OP__COUNT`), and there are about 200 of them, in
families named by their operands: `Q_ADD_III` is an `i64` add of three
`I` registers, `Q_ADD_IIK` one with a constant, `Q_BLT_IK` a
compare-and-branch against a constant, `Q_LDF_*` a field load, and so on.
The selection looks at the opcode, the type and each operand's mode and
bank. Among the shapes:

* 64-bit integer and `f64` arithmetic, shifts, `inc`, `dec`, `div` and
  `rem` by a constant other than 0 and -1;
* moves between registers, of constants, and of 64-bit globals;
* compare-and-branch, null tests, `jmp`, `jtab`;
* field and element loads and stores, closure captures (`UP[n]`);
* calls, closure calls and returns, with registers or constants;
* `new`, `lnew`, `lpush`, `len`, `mget`, `isa` and `bisa`, `cvt.f64.i64`.

A shape qualifies only when its generic meaning is trivially the same: no
width canonicalisation, and no trap that cannot be checked cheaply. A
handler that meets anything unusual (a null object, another struct type, an
index out of range) hands the instruction to the generic form, which
raises the usual error. The shapes were chosen from profiles of compiled
Lox and Lua ([PERFORMANCE.md](PERFORMANCE.md#how-the-interpreter-is-made-fast)).

When the form is chosen, the operands the handler needs are copied into the
`Insn` itself: register indices to `r[]`, a label to `target` (relative to
the instruction in the tail-call build, so a jump is one addition), an
immediate or object to `k`. Before that, `interp_quicken_function` stores
each register operand's word offset in the window in its `pad` byte.

**Call plans.** A quickened `call` or `ret` gets a plan, stored in the same
allocation just after its operands, with `k.p` pointing at it:

```
ops ──► | Operand 0 | Operand 1 | ... | QPlan                                   |
                                       | Function *fn | argc | nres | slot[]  |

slot[] for a call:   [0, argc)          each argument's word in the caller's window (0xFF: a constant)
                     [argc, 2·argc)     the matching parameter's word in the callee's window
                     [2·argc, +nres)    each result's word in the caller's window
slot[] for a ret:    each value's word in its own window

fib's `call fib (I1) -> (I2)`:  fn = fib, argc 1, nres 1, slot = {1, 0, 2}
```

`Q_CALL_R` pushes the frame and copies each argument with one load and one
store, `new[slot[argc+k]] = old[slot[k]]`, with no test of its bank; a
constant argument (`Q_CALL_RK`) copies the word prepared at load time. It
marks the new frame `qret`. A quickened `ret` in a `qret` frame copies its
values straight into the caller's result words, using its own plan and the
caller's call's plan (`ret_insn->k.p`). Any other `ret` takes the generic
path.

### 8.3 Dispatch

The handlers are written once (`vm/qforms.inc` for the quickened forms,
`vm/generic.inc` for the rest) in a small vocabulary of macros (`QCASE`,
`NEXT`, `JUMP`, `GENERIC`, `SPILL`, `RELOAD`), and compiled one of three
ways:

| Build | Handlers | State held in registers | Used when |
|---|---|---|---|
| Tail calls | one C function each, ending in a `musttail` call through `qhandlers[next->xop]` | the instruction, the slice counter, the `I`, `F` and `R` windows; with Clang's `preserve_none` also the frame and its code | Clang, GCC 15 |
| Labels as values | the cases of one loop, `goto *qtab[in->xop]` | the loop's locals | other GCC-compatible compilers |
| `switch` | the same loop as `switch` cases | the loop's locals | anything else |

A quickened handler in the tail-call build, as the macros expand it:

```c
static int h_Q_ADD_III(Ctx *c, const Insn *in, uint32_t left,
                       int64_t *I, double *F, Obj **R, Frame *qfr, const Insn *qcode) {
    const Insn *nx = in + 1;
    I[in->r[0]] = (int64_t)((uint64_t)I[in->r[1]] + (uint64_t)I[in->r[2]]);
    if (left == 0) return h_slice(c, nx, left, I, F, R, qfr, qcode);   /* musttail */
    left--;
    return qhandlers[nx->xop](c, nx, left, I, F, R, qfr, qcode);       /* musttail */
}
```

Each handler's jump is a separate indirect branch, predicted on its own,
and each handler's registers are allocated on their own. The rest of the
interpreter state is a `Ctx`: the process, the frame, the four window
pointers, and in the tail-call build the code, the instruction count base
and the budget.

**The slice counter.** `left` counts down the instructions left in the time
slice (2,000). When it reaches 0, `h_slice` writes the frame's `pc` and the
process's instruction count back, and returns `SLICE_YIELD`. With a tool's
hook installed, the counter is set to 0 for every instruction, so each one
passes through `h_slice`, which calls the hook (§20). Without one, the
dispatch pays nothing for tools.

**Writing state back.** `Frame.pc` (the index after the current
instruction, which is both the return address and the fault position) and
`Process.insns` are written only where something can see them: before a
generic instruction (`SPILL`), at calls and returns, at the end of the slice
and before a hook. After anything that may have moved the register stack
(a call, or a native that called back into the VM), `RELOAD` re-reads the
window pointers.

### 8.4 The generic path

Every instruction that is not quickened, and every quickened one that gave
up, runs through one big `switch` in `generic.inc`, inside a single
function (`h_generic`) in the tail-call build. Operands are read with
`rdn`/`rd` and written with `wr`:

* a register: read from the window, canonicalised to the type read, or
  written with a bank check (`E_TYPE`) and canonicalised;
* an immediate or constant: its value, or its natural type when untyped;
* a memory operand: `op_addr` computes a checked pointer (base plus scaled
  index plus displacement, or through an inner pointer for `[mem][d]`),
  then `mem_read`/`mem_write` check it (§10);
* an object operand: `obj_loc` checks the object (non-null, the right
  struct type, the index in range) and returns a slot whose type is fixed
  (a struct field, an array element) or carried in a tag (a list element, a
  closure capture).

Shortcuts in `rdn` and `wr` handle an array element or struct field of
exactly the instruction's type without building a slot.

### 8.5 Safepoints

An allocation only sets `gc_pending`, and a kill from the process's own
thread sets `kill_pending`. Both are checked at a **safepoint**: at the
start of every slice, and after every generic instruction. Quickened
handlers do not trap, and the two that allocate (`new` and `lnew`) save the
`pc` first, so a collection never runs inside an instruction, and a run of
quickened instructions delays one by at most a slice.

### 8.6 Around the dispatch

`interp_run` calls the dispatch (`run_loop`) and handles what it returns
rather than runs:

* `SLICE_EXT`: a coroutine instruction (`conew`, `coresume`, `coyield`,
  `costatus`, `cocur`, `coclose`), run by `co_exec` (§16);
* `SLICE_DONE` while a coroutine's first frame has returned: the coroutine
  ends, and its results go to the `coresume` waiting for them.

Keeping rare work out of the dispatch keeps its register allocation, and so
every handler's speed, untouched by it.

### 8.7 Shared arithmetic

The pure arithmetic is `static inline` code in `core/arith.h`:
`arith_binop` (with the `INT64_MIN / -1` case), `arith_unop`,
`arith_shift`, `arith_compare`, `arith_convert` (saturating, NaN to 0) and
`arith_bitcast`. Division by zero is left to the caller: the interpreter
traps, and the optimizer refuses to fold it. The interpreter and the
optimizer's constant folding both use this code, so a folded value is
always the value the instruction would have computed.

## 9. A process in memory

### 9.1 What a process owns

`struct Process` (`vm/vm.h`, about 3 KB) holds everything mutable that a
process has. Grouped by purpose:

| Group | Fields | See |
|---|---|---|
| The running context | `regs`, `rcap`, `rtop` (the register stack); `frames`, `nframes`, `fcap`, `flimit`; `stack` (a `Segment`), `sp`, `fp` | §9.2–9.5 |
| Memory | `globals[nglobals]` (one segment per module), `old_globals`; `raw` (segments), `rawfree` | §10 |
| The heap | `heap` (the `malloc` list), `chunks`, `slab_free[16]`, `bchunks`, `buf_free[16]`, `heap_bytes`, `next_gc`, `gc_pending`, `markstack`; `handles`, `handle_free` | §11 |
| Exceptions | `jb` (the slice's jump buffer), `exception`, `err`, `backtrace` | §12 |
| Messages and scheduling | `lock`; `mbox` (taken messages), `inbox` (delivered, not yet taken), `mcount`; `links`, `monitors`; `wake_at`; kill flags; `quota[6]`; `reply` | §13, §14 |
| Native calls | `cur_native`, `in_foreign`, `native_req`, `bcall` (a call on the I/O pool), `fdwait`; `cb_base`, `pins` | §15, §17 |
| Coroutines | `co` (the running one), `stack_gen` | §16 |
| Results and statistics | `results[]`, `exit_code`; instruction, memory, message and time counters | |

A new process (`proc_new`) has no frames, registers or data stack yet: the
first call allocates 8 frames and 64 register words, and the data stack is
allocated the first time something touches it. A process waiting in `recv`
takes about 4 KB, which is mostly the `Process` record and those first
frames and registers.

### 9.2 The register stack

All of a process's registers are one array of 8-byte words, `regs`. Each
frame owns a window of it, `regs[base .. base + rsize)`, holding its `I`,
`F`, `R` and `P` registers one after another at the function's offsets
`roff[]` (§6). A word is read and written with its bank's C type
(`int64_t`, `double`, `Obj *`, `uint64_t`). Registers are not addressable:
no pointer can name a register.

### 9.3 Frames

```c
typedef struct {                 /* 56 bytes */
    Function *fn;
    uint32_t pc;                 /* the index after the instruction running in this frame */
    uint8_t qret;                /* pushed by a quickened call: a quickened ret copies straight back */
    uint32_t base;               /* its register window: regs[base .. base + fn->rsize) */
    uint64_t entry_sp;           /* SP at the call, restored on return */
    uint64_t saved_fp;           /* the caller's FP */
    const Insn *ret_insn;        /* the caller's call, which receives the results; NULL for an entry frame */
    Obj *closure;                /* the closure being run (UP[n] reads it), or NULL */
} Frame;
```

**A call pushes nothing on the data stack.** The return address (`pc` in the
caller's frame and `ret_insn`), the saved `FP` and the caller's `SP` all
live in the frame array, which is ordinary memory outside the address space
programs can reach. So a program cannot overwrite a return address, however
it uses its stack.

### 9.4 Pushing and popping a frame

`proc_push_frame` (`vm/proc_internal.h`):

1. **Fit check:** `nframes < flimit && rtop + rsize <= rcap`. `flimit` is
   the smaller of the frame array's capacity and the depth limit
   (`--max-frames`, 100,000 by default), so one comparison covers both.
2. If it does not fit, `proc_frame_slow` traps `E_CALL_DEPTH` at the limit,
   or doubles the frame array (from 8) and the register stack (from 64
   words). Either may move, so callers re-read their pointers afterwards.
3. Fill the frame: `entry_sp = sp`, `saved_fp = fp`, then `fp = sp`;
   `base = rtop`, `rtop += rsize`.
4. Clear the window's one run of `zlen` words from `zoff` (§7).

`proc_pop_frame` is `sp = entry_sp; fp = saved_fp; rtop = base`.

**Calls.** The generic `call` reads each argument in the caller's window as
its parameter's type, pushes the frame, and binds the arguments per bank in
order (`bind_args`: the first integer-class parameter to `I0`, the first
float to `F0`, and so on). The generic `ret` reads its values, pops the
frame, and writes them to the caller's result operands. A tail call reads
its arguments, pops the current frame, and pushes the callee in its place
with the same `ret_insn`. A native call pushes no frame at all (§17).

### 9.5 The data stack

Each process has a data stack segment (1 MiB by default, `--stack-words`),
allocated at first use as one block holding the data and then one tag byte
per word. It **grows down**: `SP` starts one past the top, `push`
decrements it by a word and writes, `pop` reads and increments. `SP` and
`FP` belong to the process, not to frames; a frame saves and restores them.

* `ARG[n]` is `FP[n]`, the `n`th word the caller pushed before the call.
* `LOC[n]` is `FP[-1-n]`, the `n`th word reserved by `enter`.
* `enter n` lowers `SP` by `n` zeroed words. There is no `leave`: `ret`
  restores `SP` from the frame.

`main` calls `f` with two stack arguments; `f` reserves one local and calls
`g` with one; `g` reserves two locals:

```
Register stack (regs)                          Frame array
  word  0..2    main's window   ◄── base 0      frames[0] main  pc = after `call f`  entry_sp = S       saved_fp = S
  word  3..7    f's window      ◄── base 3      frames[1] f     pc = after `call g`  entry_sp = S-16    saved_fp = S
  word  8..11   g's window      ◄── base 8      frames[2] g     pc = current         entry_sp = S-32    saved_fp = S-16
  word 12..     free            ◄── rtop                                      ret_insn: the caller's call instruction

Data stack (segment 1), S = its size in bytes
  offset   S       ─────────────────────── main's FP; SP at the start
           S-8     11      f's ARG[1]       (main: push 11, 10)
           S-16    10      f's ARG[0]   ◄── f's FP (= SP at `call f`)
           S-24    0       f's LOC[0]       (f: enter 1)
           S-32    77      g's ARG[0]   ◄── g's FP (= SP at `call g`)    (f: push 77)
           S-40    0       g's LOC[0]       (g: enter 2)
           S-48    0       g's LOC[1]   ◄── SP
           ...     free, down to offset 0
```

When `g` returns, `SP` goes back to `S-32` and `FP` to `S-16`. The caller's
stack arguments stay, above its `FP`; the caller drops them.

## 10. Pointers, segments and tags

### 10.1 Checked pointers

A `p` value is 64 bits:

```
 63            44 43        32 31                       0
+----------------+------------+-------------------------+
|  segment (20)  |  gen (12)  |      offset (32)        |
+----------------+------------+-------------------------+

segment 0          null
segment 1          the running data stack (the process's, or the running coroutine's)
segments 2..4095   the global segment of module k is 2 + k
segments 4096..    raw heap blocks: raw[segment - 4096]
```

Each process has its own segments, so the same pointer means different
memory in different processes, and never another process's. A `Segment` is
`{data, tags, size, gen, kind, live}` (24 bytes).

**A dereference** (`interp.c:op_addr`, `mem_ptr`):

1. `proc_segment_fast` (inline) picks the segment by number and checks that
   it is live and its generation matches. Anything unusual goes to
   `proc_segment`, which allocates a data stack on first use, adds a
   module's global segment loaded since the process started, or traps:
   `E_NULL`, `E_UAF` (a freed raw block), `E_BAD_PTR` (no such segment, or a
   stack that is not running).
2. `mem_ptr` checks `offset + width <= size` (`E_BOUNDS`, or
   `E_STACK_UNDERFLOW` above the top of a stack).
3. Pointer arithmetic (`ptr_add`) changes only the offset, and traps if it
   leaves `0 .. 2^32-1` (`E_STACK_OVERFLOW` below the bottom of a stack).

No instruction converts an integer to a pointer. A pointer stored in raw
memory is plain bytes, so a program can build one there, but every
dereference is still checked against its own process's segments.

### 10.2 Tags

The data stack and the global segments keep one tag byte per 8-byte word:
the `MlwsType` it was last written as, `T_NONE` (never written) or
`TAG_RAW` (written in part, unaligned, or by `memcpy`/`memset`). Raw heap
blocks have no tags.

* **Writing** a value aligned to a word sets the word's tag to the value's
  type. An unaligned write marks every word it touches raw. A write of
  anything but a reference that covers only part of a reference word
  (narrow, unaligned, `memcpy`, `memset`) zeroes that word first, so the
  bytes it leaves cannot be read as part of an address.
* **Reading a reference** requires the tag to be `T_R`, or `T_NONE`, which
  reads as null. Anything else traps `E_TYPE`.
* **Reading anything else** traps if it overlaps a word tagged `T_R`, and
  `memcpy` refuses to copy one. So a reference can be neither forged from an
  integer nor read as one.
* **Storing a reference** needs a tagged, aligned word. A store into the
  data stack lowers `stack_ref_lo`, the lowest word ever tagged as a
  reference, where the collector's scan of the stack starts (§11.4).
* **Untyped** `mov`, `push`, `pop` and `swap` read a word as the type of its
  tag, so values keep their types when spilled and reloaded.

### 10.3 Global segments

Every process has its own copy of every module's globals: `proc_new` copies
each module's image (`gdata` and `gtags`) into a new segment. A module
loaded while the process runs is added the first time the process touches
its segment (`proc_grow_globals`). The old array of segment descriptors is
kept until the process ends, because the instruction that caused the growth
may still hold a pointer into it.

### 10.4 Raw blocks

`alloc` (`process.c:proc_alloc`) takes a free slot from `rawfree` or adds
one to `raw`, bumps that slot's generation, and allocates zeroed memory.
`free` checks that the pointer is a block's start (`E_BAD_FREE`) and that
the block is live with that generation (`E_DOUBLE_FREE`), then frees the
memory and returns the slot. `realloc` bumps the generation even when the
block does not move, so every pointer to the old block traps `E_UAF`. The
blocks still allocated when a process ends are freed with it.

### 10.5 Handles

A handle is an index into the process's `handles` table plus one, so 0 is
never valid; `hfree` empties the slot and puts it on `handle_free` for
reuse. Each slot is a collector root, which is how raw memory or a host
refers to an object.

## 11. The heap and the collector

### 11.1 Objects

Every object starts with a 16-byte header:

```c
typedef struct Obj {
    uint8_t kind;        /* OBJ_STRING ... OBJ_CORO, also what typeof returns */
    uint8_t flags;       /* IMMORTAL, MARK, BORROWED */
    uint16_t slab;       /* its size class + 1 if carved from the process's chunks; 0: malloc */
    uint32_t pad2;
    struct Obj *next;    /* the malloc list, or a free list */
} Obj;
```

| Kind | Layout after the header | Notes |
|---|---|---|
| string | `len`, `hash`, bytes, NUL | Immutable. The hash (FNV-1a) is computed when it is made |
| bytes | `len`, `cap`, `data *` | Mutable; the buffer grows by doubling |
| array | `elem` type, `len`, words | Fixed length; traced if `elem` is `r` |
| struct | `StructDef *`, fields | The type comes from its module; fields are traced if their declared type is a reference |
| list | `len`, `cap`, `items *`, `tags *` | Values and their tags in one block (below) |
| map | `count`, `nentries`, `entry_cap`, `entries *`, `index *`, `index_cap` | Insertion-ordered hash table (below) |
| closure | `Function *`, `nup`, `tags *`, captures, their tags | One allocation; captures are values with a tag each |
| BigInt | `sign`, `n`, 32-bit limbs | Immutable |
| decimal | `sign`, `exp`, `n`, limbs | value = sign × coefficient × 10^exp; immutable |
| foreign | `type`, `ptr`, `lib` | A C resource; finalized when collected |
| coroutine | status, body, resumer chain, a whole execution context | §16 |

**Lists.** A list's storage is `cap` words followed by `cap` tag bytes. A
list made with a capacity of up to 24 has its storage inside its own
allocation, so `lnew R0, 4` is one allocation. Otherwise the storage is a
buffer, which grows by doubling from 8.

**Maps** follow the compact design CPython uses:

```
entries: dense, in insertion order        index: power of two, open addressing
  0  {key, val, hash, ktag, vtag, live}     slot → entry number, -1 empty, -2 deleted
  1  {...}                                  probe: h & (cap-1), then +1 (linear)
  ...
```

* Lookup probes `index` from the key's hash, skipping deleted slots and
  stopping at an empty one, and compares the stored hash before the key.
* Insertion appends to `entries` and puts the entry's number in the first
  empty or deleted slot.
* Deletion marks the entry dead and the slot deleted. The entries stay in
  place until the next rebuild, which keeps iteration in insertion order.
* A rebuild, when `entries` is full, compacts the live entries (doubling
  their capacity if more than half are live) and reinserts them by their
  stored hashes, with `index` at least twice as large as `entries`.
* **Keys** are compared by class: integers of any width by value, floats by
  value (`+0.0` and `-0.0` are one key, and NaN equals NaN), strings,
  BigInts and decimals by value, and other objects by identity. Keys of
  different classes are never equal, so `1`, `1.0` and `1n` are three keys.
  Integers, floats and identities are hashed with a 64-bit mixer; strings
  use their stored hash.

### 11.2 Allocation

Every object is made by `gc_alloc(p, kind, size)`:

* **Up to 256 bytes**, from the process's own chunks, in 16 size classes of
  16 bytes. The class's free list is popped, or the object is carved from
  the newest chunk. A chunk is a header and then objects back to back;
  chunks grow from 1 KiB to 64 KiB, so a small process stays small. The
  object is zeroed with a constant-size `memset` per class, which the
  compiler inlines. There is no lock and no `malloc`.
* **Larger objects** come from `malloc`, on the process's `heap` list.
* **Buffers** an object owns (a list's storage) come from separate buffer
  chunks with the same 16 classes, while they are small and the object
  itself came from a chunk.

AddressSanitizer builds and `MLWSVM_NO_SLABS=1` use `malloc` for everything,
so each object's lifetime is visible.

`gc_account(p, bytes)` charges each allocation to `heap_bytes`. It sets
`gc_pending` when the heap reaches `next_gc` (or always, under
`--gc-stress`), and traps `E_QUOTA` when the heap quota is exceeded, except
while the VM builds an `Error` or foreign code runs. Whatever grows is made
consistent before it is charged, since the trap can come from the charge;
`gc_alloc` checks the quota before it makes the object, so a trap never
leaves the sweep an object its constructor did not finish.

### 11.3 When to collect

`gc_threshold(live)` sets `next_gc` after each collection:

* twice the live heap, and at least the minimum (`--gc-min`, 1 MiB);
* with a heap quota, no more than three quarters of it, but always room for
  an eighth more than the live data, so a process near its quota collects a
  few times and then gets `E_QUOTA`, rather than collecting at every
  allocation.

The collection itself runs at the next safepoint (§8.5).

### 11.4 Marking

`gc_collect` marks from these roots:

1. every frame's `R` window, and its closure;
2. the data stack's words tagged `T_R`, from `stack_ref_lo` to the top;
3. every global segment's words tagged `T_R`;
4. the handle table;
5. the exception being raised, the running coroutine (and through it, its
   resumers), objects pinned during a native call, an exception held from a
   callback, a blocking call's arguments and results;
6. the messages in the mailbox.

Marking is iterative: `mark` sets an object's mark bit and pushes it on
`markstack`; the loop pops objects and scans them by kind, marking what
they refer to. Immortal objects are never marked, so they are never
written, which is what makes sharing them between threads safe. The cost
is proportional to the root words scanned plus the live objects and their
references.

The collector is **precise**: it knows which words are references from the
register bank, the memory tags, array and field types, and list and map
tags. No integer ever keeps an object alive.

### 11.5 Sweeping

1. **The chunks**, each in address order. Each object's header gives its
   size, so the sweep steps from object to object without a list. A marked
   object is unmarked. An unmarked one releases what it owns (a foreign
   object's finalizer runs, a coroutine's context is freed, a list's or
   map's buffers are freed), is subtracted from `heap_bytes`, and becomes a
   free slot. Each class's free list is rebuilt in the order the sweep
   meets the slots, so later allocations run through memory in order.
2. **The `malloc` list**: unmarked objects are released and freed.

Then `next_gc` is set from what is left. When a process ends, its foreign
objects are finalized first (so its files and sockets close before its
`Down` messages go out), and then its chunks are freed whole.

### 11.6 Why this collector

It is a non-moving, non-incremental, non-generational mark-sweep, one per
process, with no barriers and no locks. A heap belongs to one process and
is collected only on its own thread, between instructions, so the
collector never needs to stop another thread or find roots in C code. A
generational collector was built and measured, and was not kept
([PERFORMANCE.md](PERFORMANCE.md#what-is-not-done-and-why)).

### 11.7 Immortal constants

A module's constants (strings, BigInts, decimals) are made once per VM, at
link time, in `vm->const_pool` (keyed by kind and text) and owned by the VM.
They carry `OBJF_IMMORTAL`, are on no heap list, are never marked, copied or
freed, and are shared read-only by every process. Linked operands point
straight at them.

### 11.8 BigInt and decimal

`heap/numeric.c` works on magnitudes of 32-bit limbs in temporary buffers,
then makes an immutable object. Multiplication is schoolbook; division is
Knuth's Algorithm D (with a fast path for a one-limb divisor). A decimal
is a coefficient and a power of ten: addition aligns the exponents exactly,
multiplication adds them, and division keeps at least 28 digits and rounds
half to even. Temporaries are freed before the result is charged to the
heap, so a quota trap leaks nothing.

### 11.9 Unicode tables

`core/unicode.c` reads tables generated from the Unicode Character Database
(`build-tools/gen_unicode.py`, `unicode_tables.inc`). Character properties
are a two-stage table: the code point's block of 128 selects a deduplicated
block of record numbers, which selects one of a few hundred distinct
records (category, combining class, break classes, width, flags and simple
case deltas). Full case mappings, decompositions and compositions are
sorted tables searched by binary search. Normalization decomposes, orders
combining marks and recomposes, per UAX #15; grapheme and word boundaries
are state machines over the break classes, per UAX #29.

## 12. Exceptions and traps

**One jump buffer per slice.** `proc_run_slice` (`vm/process.c`) puts a
`jmp_buf` on the C stack, records it in `p->jb`, and calls `setjmp` once,
then runs the interpreter for the slice. Every trap and every `throw` is a
`longjmp` to it, so the cost of `setjmp` is paid once per slice, not per
instruction or call. On macOS and the BSDs the VM uses `_setjmp`, which
does not save the signal mask with a system call.

**Raising.**

* `vm_trap(p, code, ...)` builds the backtrace, makes a built-in `Error`
  struct with `code`, `message` and `trace` strings (exempt from the heap
  quota while it does), and throws it.
* `proc_throw(p, obj)` records the backtrace and the exception, and
  `longjmp`s.

**Handlers are tables.** A `.try` region is an `MHandler {start, end,
target}` beside the function's code, so a region costs nothing until
something is thrown.

**Unwinding** (`unwind`, `unwind_frames`), back at the `setjmp`:

```
for each frame, innermost first, down to cb_base:
    pc = the faulting instruction (frame.pc - 1: the instruction, or the call in progress)
    the smallest handler region containing pc wins
    found:  frame.pc = handler; return SLICE_YIELD (the handler runs next slice)
    else:   pop the frame (SP, FP, registers restored as ret would)
past a coroutine's first frame: the coroutine dies; go on at its coresume
nothing found: the process fails with the exception's code and message, and
    the backtrace of an Error's own trace (where it was made, through any rethrow)
```

When an outer frame catches, `SP` is what it was at its call; in the frame
that faulted, it is what it was at the fault. A native's callback runs
above `cb_base`, and its exceptions never unwind the frames of the code
that called C (§17).

**Backtraces** list the running frames innermost first, collapsing a
repeated line, keeping the outermost frames of a very deep stack, and then
each resumer's frames after "(in a coroutine resumed by)". Each line names
the function and its assembly file and line, and the translated program's
line where a `.loc` gives one.

## 13. Processes and the scheduler

### 13.1 The structures

`vm/sched.c` keeps a `Sched` per VM, and a `Worker` per worker thread (one
per core by default, at most 256):

```
Sched
  chunks[2^18] ──► Process *[4096]      the process table: pid → Process
  stripes[64]                          table locks, by pid % 64
  workers[] ──► Worker { qlock, q (ring of ready pids), frag, current, thread }
  lock, work_cv, stw_cv, resume_cv     idle workers, stop-the-world
  timers (min-heap of {at, pid}), next_timer
  nidle, nwaiting, waking              who is idle, and the wake-one protocol
  io_queue, io_threads, io_inflight    the I/O pool (§15)
  poller, poll_thread, io_waiting      the poller (§15)
  host_frag, host_lock                 messages from outside the VM (§14)
```

**The process table.** Pid `n` is slot `(n-1) % 4096` of chunk
`(n-1) / 4096`. A chunk is allocated when its first pid is published and
installed with a compare-and-swap; chunks never move, so a lookup needs no
global lock. Pids come from a counter, start at 1 (the first `main`) and
are never reused, so a pid never refers to a different process later.

**Pinning.** `pin(pid)` takes the pid's stripe lock, reads the slot, takes
the process's own lock, and releases the stripe. The process is returned
locked, and cannot be freed while it is pinned: `proc_free` first clears
the slot under the stripe lock (no new pin can find it), then takes the
process's lock once more (waiting out any current pin). Run queues hold
pids, not pointers, so a stale entry simply fails to pin.

**Locks** are always taken in one order, so they cannot deadlock:

```
table stripe  →  one process  →  a run queue | the scheduler lock | vm->lock | vm->out_lock
```

No thread ever holds two process locks. Short critical sections use a
`PlatLock` (`os_unfair_lock` on macOS, otherwise a `pthread` mutex); the
scheduler and I/O pool use a `PlatMutex` with condition variables.

**What is locked.** A running process touches only its own state, so the
interpreter takes no locks. A process's lock guards only what other threads
touch: its inbox, links, monitors, kill request, scheduling state, mailbox
quota and a pending reply (the fields marked `[L]` in `vm.h`).

### 13.2 Process states

```
               spawn                      a worker takes it
   (new) ───────────► READY ──────────────────────────────► RUNNING
                        ▲                                      │ the slice ends with:
                        ├──────────────────────────────────────┤ YIELD
                        ├─ a message, or the deadline ─ WAITING ◄─ BLOCK  (recv, proc.call)
                        ├─ its timer ──────────────── SLEEPING ◄─ SLEEP
                        ├─ the descriptor, or timeout ── IOWAIT ◄─ IOWAIT (§15)
                        ├─ the call returns ──────────── NATIVE ◄─ NATIVE (§15)
                        └─ (the debugger continues)               BREAK
                                                         DONE / FAILED ◄─ DONE, FAILED
```

### 13.3 A worker's loop

```
worker_loop(w):
    now = clock()
    while the run is not over:
        park while a tool has stopped the world
        worker 0 only: serve the monitor (vm->poll) at most once a millisecond
        if the earliest timer is due: timers_service(now)
        p = the process a tool stopped, if any; else take(w): own queue, else steal
        if none: idle(w); continue
        if p was killed from another thread: finish(p, failed); continue
        p->state = RUNNING; move its inbox into its mailbox
        r = proc_run_slice(p, 2000)
        end = clock(); p->cpu_ns += end - now; now = end     (one clock read per slice)
        after_slice(p, r)
```

`after_slice` handles the result: requeue (`YIELD`); wait (`BLOCK`, unless
a message or reply arrived during the slice, which would otherwise be a
lost wake-up); start a timer (`SLEEP`, or a deadline); hand the process to
the poller or the I/O pool; or `finish` it.

**Work stealing.** A worker whose queue is empty looks at the others in
turn, starting after itself, and takes half of the first non-empty queue
(at most 32 pids). It runs the first and queues the rest.

**Where a woken process goes.** `ready_locked` puts a woken process on the
queue of the thread that woke it: a receiver on its sender's worker, a
sleeper on the worker that serviced its timer, and anything woken by the
poller, an I/O thread or the host on worker 0. So a pair of processes
talking to each other stays on one thread, where a message costs no
cross-thread wake-up.

**Waking one thread at a time.** Waking a sleeping thread costs
microseconds, far more than a message. So a ready process wakes another
worker only if its queue has a backlog, or if it was woken by a thread that
is not a worker. And while one woken worker is on its way (`waking`), no
other is woken; the woken one wakes the next only if it steals more than it
can run. A process spawning a million processes that each wait for a
message, then waking them all, takes 0.9 s on one thread and 1.7 s on 18;
with a wake-up for every ready process it took 6.5 s on 18.

**Idling.** An idle worker services due timers, flushes buffered program
output, and waits on `work_cv`, with a timeout if a timer is pending. It
checks for work while holding the scheduler lock until the wait releases
it, and a wake-up needs that lock, so none is lost.

### 13.4 Timers

`sleep`, `recvt`, `proc.call` deadlines and I/O timeouts add `{at, pid}` to
one min-heap, under the scheduler lock. `next_timer` holds the earliest
time, so a worker checks it with one atomic load per slice. An entry is
live only if its process is still waiting for that same deadline
(`wake_at == at`); anything else (a process woken early by a message,
killed, or waiting for a different deadline) is dropped when it reaches the
top. The timed-out instruction is run again and sees that its time has
passed.

### 13.5 Ending, links and monitors

`finish(p, failed)`, when a process ends:

1. Write out its unfinished output line; set it `DONE` or `FAILED`; detach
   its links and monitors; record the exit for the monitor.
2. Finalize its foreign objects (closing its files and sockets) and release
   its registered names, *before* anyone is told, so a supervisor can
   restart it at once on the same port and name.
3. Send a `Down {pid, exit, code, message}` to each monitor.
4. Kill each linked process that is still alive with `E_LINKED`, if this one
   failed (`finish` runs recursively along chains of links); a normal exit
   only removes the link.
5. If it failed with nobody watching, print a crash report.
6. Free it, unless it is a program's `main`, which is kept for the run's
   result. When the last program's `main` ends, the run is over; when
   another program's `main` ends, that program's other processes are killed.

**Kills.** `kill` claims a process that is waiting, sleeping or ready, and
finishes it at once. A process that is running on its own thread stops at
the next safepoint (`kill_pending`); one running on another thread, or in a
call on the I/O pool, gets `kill_req` and stops when its slice or call ends.
`alive` reports it dead straight away.

### 13.6 Deadlock

When a worker goes idle and finds that every worker is idle, no queue has
work, no timer is pending, no I/O is in progress or awaited, and nothing
outside the VM can send a message (an embedding host's `mlws_start`, or a
live asynchronous callback), then no process can ever be woken. Every
program `main` that is waiting is then woken with a flag, and its `recv` (or
`proc.call`) raises `E_DEADLOCK`.

### 13.7 Stopping the world

The monitor reads every process's state, so for each request it asks the
other workers to stop (`sched_stop_world`). Each one parks at the top of
its loop or in `idle`, and counts itself parked; the requester waits until
all have, serves the request, and lets them go (`sched_start_world`). The
debugger, tracer and profiler instead run the program on one worker.

## 14. Messages

```
 sender S, on worker W                              receiver R
 ─────────────────────                              ──────────
 send pid, value
   pin(R); check it is alive and its mailbox is under quota; unlock
   copy the value into W's scratch process (W.frag)      (§11: msg_copy)
     → InMsg { value, tag, objects (a detached list), bytes, count }
   pin(R) again (it may have ended meanwhile)
   push the InMsg on R.inbox; R.mcount++
   if R is WAITING: ready it, on W's queue
   unlock R
                                         when R next runs (or at recv):
                                           under R's lock, for each InMsg:
                                             gc_adopt: splice its objects onto R.heap, charge the bytes
                                             append {value, tag} to R.mbox
                                         recv: read the oldest message as the destination's type
                                           (a mismatch traps E_TYPE and leaves it)
```

* **No heap is touched by two threads.** The sender builds the copy in its
  worker's own scratch process (`frag`), whose objects are `malloc`'d and
  never collected; the receiver adopts the finished list on its own thread.
  A callback from a blocking C call sends from an I/O thread, which has a
  scratch process of its own (`tls_io_frag`).
* **The copy happens outside any lock.** It may trap (a pointer or a
  foreign handle cannot be sent), and a trap must not leave a lock held.
  `frag_reset` frees whatever a trapped copy left behind.
* **Ordering.** A sender runs on one thread at a time and appends in
  program order, so messages from one process to another arrive in order. A
  process's `Down` is sent after it has stopped running, so after all its
  messages.
* **Values that are not references**, and null, skip the copy.
* `spawn` copies its arguments straight into the new process's heap, since
  nothing else can see it yet.
* **Replies** (`proc.call`, `proc.reply`) go into a reserved slot of the
  caller, never its mailbox, so a reply cannot be confused with a message.
  The call's id makes a late reply, after a timeout, harmless.
* **Messages from outside the VM** (`sched_post`, used by embedding hosts,
  extensions and asynchronous callbacks) are built in `host_frag`, under
  `host_lock`, from any thread, and delivered the same way.

## 15. Input and output

A native that may wait is a *checked* native (`NF_CHECKED`). Instead of
blocking a worker, it records what it needs in `Process.native_req` and
returns. The interpreter then backs up to the `ncall` (`pc--`) and ends the
slice; when the need is met, the `ncall` runs again with
`Process.resumed` set, and the native picks up where it left off (a partial
write remembers how far it got).

```
ncall ─► native returns with native_req =
   NONE         results written; done
   WAIT_FD      ─► IOWAIT: arm the poller with token = pid<<16 | gen  [+ a timer for a timeout]
                   poller thread: event → pin(pid) → state and gen still match → ready
                   timer: timed_out = true → ready                     ─► ncall runs again
   BLOCKING     ─► NATIVE: the call goes to the I/O pool
   (or an NF_BLOCKING native)  I/O thread: run the native (it may block) → bcall DONE → ready
                                                                       ─► ncall takes the results
   WAIT_REPLY   ─► WAITING until proc.reply, the deadline or a deadlock
   EXIT         ─► os.exit: the run ends
```

* **The poller** is one thread (kqueue on macOS and the BSDs, epoll on
  Linux), started at first need. Registrations are one-shot. The generation
  in the token makes an event for an earlier wait harmless. Pipes and
  sockets the VM created are non-blocking, so a process waiting on one
  holds no thread: 10,000 open connections are 10,000 small processes.
* **The I/O pool** runs calls that cannot be polled: regular files,
  inherited descriptors, `os.run`, and natives declared blocking. It starts
  a thread when none is idle, up to 64. At the end of a run, a call that has
  not returned within a second is abandoned, and the VM is not freed.
* `io_inflight` and `io_waiting` count the processes in each, which is what
  keeps a wait for I/O from being taken for a deadlock (§13.6).
* **Descriptors move between processes** with `fs.give` (which closes the
  handle and records `{random ticket, fd}`) and `fs.take`.

## 16. Coroutines

A coroutine has its own register stack, frames and data stack, and shares
everything else with its process ([ABI.md §8](ABI.md#8-coroutines)).

```c
typedef struct {                    /* an execution context: what a switch exchanges */
    uint64_t *regs; uint32_t rcap, rtop;
    Frame *frames; uint32_t nframes, fcap, flimit;
    Segment stack; uint64_t sp, fp;
    uint32_t stack_low_off, stack_ref_lo;
} ExecCtx;

typedef struct ObjCoro {            /* a heap object */
    Obj h;
    uint8_t status;                 /* suspended, running, normal, dead */
    uint32_t words;                 /* its data stack's size, charged to the heap */
    ObjClosure *fn;                 /* its body */
    struct ObjCoro *prev;           /* the coroutine that resumed it (NULL: the process's own code) */
    const Insn *wait;               /* suspended: its coyield; running: the coresume waiting for it */
    ExecCtx ctx;                    /* suspended: its own context; running: its resumer's */
} ObjCoro;
```

**The process always holds the running context.** `coro_swap` exchanges
the twelve fields of `ExecCtx` between the `Process` and the coroutine
object. So the interpreter, the collector, traps and the tools always find
the running context where they expect it, and never need to know a
coroutine is running.

```
main resumes A, which resumes B:

  Process: B's registers, frames and stack (stack generation 3)
  p->co ──► B  { running, prev ─► A, ctx = A's context (stack generation 2) }
                                   │
                                   ▼
                     A  { normal, prev = NULL, ctx = main's context (stack generation 1) }
```

* `coresume` marks the current coroutine `normal`, links the new one
  through `prev`, swaps, and on the first resume pushes the body's frame;
  later it delivers the values to the waiting `coyield`.
* `coyield` and the body's last `ret` swap back (`coro_leave`) and deliver
  their values to the `coresume`. A dead coroutine's context is freed at
  once.
* **Each data stack has its own generation** (the process's is 1; each
  coroutine gets another), so a pointer into a stack that is not running
  fails the generation check (§10.1).
* **The collector** marks the running context from the `Process`, the
  running coroutine, and through `prev` and each object's `ctx` the
  suspended contexts below it. A suspended coroutine nothing refers to is
  collected with everything it holds.
* The coroutine instructions run outside the fast dispatch (§8.6). A
  resume and a yield take 35 ns.

## 17. Natives, extensions and the FFI

### 17.1 Natives

```c
typedef void (*NativeFn)(Process *p, Word *args, Word *rets);
typedef struct NativeDef {
    const char *name;
    uint8_t np, pt[8];              /* parameter count and types */
    uint8_t nr, rt[4];              /* result count and types */
    NativeFn fn;
    const char *doc;
    uint32_t flags;                 /* FOREIGN, BLOCKING, SERIAL, CHECKED, OS */
    void *data;                     /* an extension's data, or an FFI call description */
    void (*xfn)(void);              /* an extension's function, called through a trampoline */
    ExtLib *lib;                    /* where foreign code came from */
} NativeDef;
```

Natives are registered by name in `vm->natives` (`io.*`, `text.*`,
`json.*`, `fs.*`, `os.*`, `net.*`, `proc.*`, `module.*` and more), and the
linker points each `ncall`'s operand at its `NativeDef`. The verifier
checks each call against the signature. `ncall` reads the arguments into a
`Word` array, calls the native, and writes the results; no frame is pushed.

* **No flags** (most built-ins): called directly.
* **Any flag**: through `native_call`, which counts the call against the
  foreign (4) or OS (5) quota, runs it with `in_foreign` set, and then
  raises any error it recorded (`native_complete`).

**Foreign code is never unwound.** C code may hold locks or resources of
its own, so nothing traps while it runs: errors are recorded
(`native_fail`; the first one wins), heap quotas are not enforced until it
returns, and then the error is raised as a normal trap. Values need no
rooting, because the collector only runs between instructions, except
when a native calls back into the VM: then the objects it is using are
`pins`, which the collector treats as roots.

### 17.2 Extensions

An extension (`.mlwsx`) is a shared library with one entry point,
`mlwsvm_extension_init_v1(const MlwsApi *api, ...)`. `MlwsApi` (in
`include/mlwsvm/mlwsvm.h`) is a table of functions: define a native, report
an error, make and read strings, bytes, lists and maps, make and read
foreign handles, post messages from any thread, and hold the VM open for
messages to come. It starts with its version and size; later versions only
add to the end. The version is also in the entry point's name, so an
extension built for another version fails to load with a clear error.

### 17.3 The declarative FFI

`.extern` (`native/ffi.c`) turns a declaration into a `NativeDef` whose
`data` is an `FfiCall`: the C function's address (`dlsym`), a libffi call
interface prepared once (`ffi_prep_cif`, or `ffi_prep_cif_var` for a
variadic function), and how to convert each argument and result. At each
call `ffi_native` converts the arguments (numbers; a string's own bytes as
`char *`; byte buffers the callee may change; foreign handles of the right
tag; structs packed into C layout, and copied back for `struct*`), calls
`ffi_call`, and converts the results (a returned pointer becomes a foreign
handle, freed by the tag's `.ffi_free` function).

* **`.cstruct`** computes the C layout field by field (each aligned to its
  own alignment) and checks it against libffi's.
* **`.callback`**: a closure passed as a C function pointer is a libffi
  closure whose trampoline calls `proc_callback`, which runs the closure on
  top of the current frames with `cb_base` set: exceptions stop there, and
  instructions that would wait trap `E_CALLBACK`. A call from another C
  thread cannot run that way; an asynchronous callback instead posts a
  `Callback` message to a chosen process.
* **Finalizers** run when a handle is collected or released, and when its
  process ends. A `serial` library's natives and finalizers run one at a
  time, under the library's lock. `Process.serial_held` chains the serial
  locks of the calls in progress (one per callback level, on the C stack). A
  finalizer whose library is in one of them (a callback collected or
  released the handle) goes on `fin_deferred` and runs when that call
  returns; a callback's call into such a library fails `E_CALLBACK`.

### 17.4 Trust

Native code is the only way around isolation, so loading it is a deliberate
act ([EXTENDING.md](EXTENDING.md#security), `vm/extensions.c`):

* An extension is looked for only in the trusted extension directories,
  never in a program's own directory.
* Each directory has a `TRUSTED` manifest, written by `mlwsvm ext`, with
  lines `ext NAME sha256:HEX FILE` and `ffi LIBRARY`. An extension loads only
  if its SHA-256 matches its line; an `.extern` library only if a line
  allows it.
* The directory, the manifest and the extension must not be writable by
  group or others, and must belong to the user or root (the check SSH makes
  of `~/.ssh`).
* Quota 4 shuts native code out of a sandbox entirely.

## 18. Embedding

`libmlwsvm` (`include/mlwsvm/embed.h`, `src/embed/embed.c`) is a thin layer
over the same loader and run helper (`vm/run.c`) the command line uses
([EMBEDDING.md](EMBEDDING.md)).

* **`mlws_call`** finds an exported function, checks and converts the
  arguments, and runs it as a new program's `main`, on the host's thread,
  which becomes worker 0 (more workers start as threads). When it returns,
  any processes it left are stopped, and its results are copied out (a
  string as text) until the next call.
* **Fresh globals per call** follow from the process model: globals belong
  to processes (§10.3), and each call is a new process.
* **`mlws_start`** runs a function on a background thread as a long-lived
  service, marking the VM as open to outside input (so waiting is never a
  deadlock). **`mlws_send`** posts a message to it from any thread
  (`sched_post`), and **`mlws_wait`** waits for it to end.
* **Host functions** (`mlws_define`) are natives of a library named
  `host`.

## 19. The optimizer

The optimizer (`src/opt/`, [OPTIMIZER.md](OPTIMIZER.md)) rewrites a
`Module` before it is linked: as it loads (`--opt`) or ahead of time
(`mlwsvm opt`). Its output is an ordinary module, which the verifier then
checks like any other.

### 19.1 The working form

```c
typedef struct {            /* one instruction */
    Insn in;                /* decoded (§8.1) */
    uint32_t line;          /* its source line */
    uint32_t rb, rn;        /* the .loc runs that start at it */
    bool dead;              /* removed at the next compaction */
} OInsn;

typedef struct {            /* one function */
    VEC(OInsn) code;
    MLoc *runs;             /* its .loc runs */
    VEC(MHandler) h;        /* handler regions, as instruction indices */
    uint32_t before, regs_before;   /* for the report and the inlining budget */
    ...
} OFunc;

typedef struct { uint32_t b[4]; } RegSet;            /* a bit per register, per bank */
typedef struct { RegSet use, def, may; } Effect;     /* per instruction */
```

* **Labels are instruction indices**, so instructions can be removed or
  inserted without byte offsets. Passes mark instructions `dead`;
  `ofn_compact` squeezes them out, moving labels, handler bounds and `.loc`
  runs to the next live instruction. `ofn_insert` shifts everything after
  an insertion point.
* **Effects** come from `core/regflow.c:insn_effect`, the same code the
  verifier uses: `use` is every register read (sources, address bases and
  indices, and the destination of a read-modify-write form such as
  `add I0, I1`); `def` the registers always written; `may` also those
  possibly written (a `recvt` that times out writes nothing; `[P]+` writes
  `P`). `may` is what kills a forward fact; `def` is what kills liveness.
* **`insn_pure`** decides which instructions may be folded, reused or
  removed: arithmetic, bitwise, shifts, compares, `sel`, `cvt`, `bitcast`,
  `tagof`, `typeof` and register moves, with only register and constant
  operands, and no `big`, `dec` (which allocate) or division by anything
  but a nonzero constant.
* **Re-encoding** (`ofn_store`) compacts each function, encodes it, and
  rebuilds the module's code, line tables, `.loc` runs and handler tables.
  With no passes, decoding and re-encoding reproduces a module byte for
  byte, which `make check-tools` checks on every module.

### 19.2 Basic blocks

`ocfg_build` splits a function into basic blocks:

* **Leaders**: instruction 0, every label target, every handler target, and
  the instruction after any that ends a block (one that never falls
  through, or any branch). Calls do not end blocks.
* **Edges**: `succ[2]` holds the fall-through and a single label target; a
  `jtab`'s targets go in `extra[]`. `of[i]` maps an instruction to its
  block.

```
code:   0 ...  4 bz       5 ...  9 jtab L1,L2,L3     10 ... 14 ret      15 (a handler) ...
block:  B0 [0,5)  succ = {B1, the bz target}
        B1 [5,10) succ = {B2 (fall-through)}  extra = {L1, L2, L3}
        B2 [10,15)  no successors
        Bh [15,..)  a leader because a handler starts there; no edge into it
```

There are no exception edges: each analysis treats handlers itself (below).
There are no dominators, and there is no loop analysis except where
inlining asks whether a call is in a loop.

### 19.3 Constants, branch facts and folding (`fold.c`)

One analysis serves three passes (`fold`, `constprop`, `simplify`):

* **A global forward dataflow over blocks.** The state per block is a value
  for each `I`, `F` and `R` register (`CVal r[3][32]`). `P` registers and
  memory are not tracked.
* **The lattice:**

  ```
                  TOP (not reached yet)
         /              |               \
   WORD(v)         POOL(i)           NONNULL
   an exact word   pool constant i    some object
   (null is 0)          \______ ______/
          \                    |
           -------------->  BOTTOM (varies)
  ```

  The meet of two different objects is `NONNULL`; of null and an object, or
  any other two different values, `BOTTOM`. A value also records whether
  this function computed it (`made`), as opposed to a new window's zero or
  a fact learned from a branch.
* **Entry**: every register is 0 (a new window reads zero), parameters are
  `BOTTOM`, and every handler block starts at `BOTTOM`.
* **Transfer**: a pure instruction with all-known operands gets its value
  from `arith.h` (§8.7); anything else sets the registers it may write to
  `BOTTOM`. An instruction that reads a field or element of an `R` register
  and completes proves it holds an object (`NONNULL`).
* **Branch facts.** At a conditional branch, the state sent along each edge
  is refined by what the test established: after `bz R0, L`, `R0` is null
  at `L` and `NONNULL` on the fall-through; after `beq I0, 5, L`, `I0` is 5
  at `L`; after `bisa R0, T, L`, `R0` is an object at `L`. Facts are taken
  only at full width and never from float equality (`-0.0 == 0.0`). A
  branch whose outcome is known sends nothing along the edge never taken.
* **The fixpoint** uses a LIFO worklist of blocks; the lattice is finite,
  so it ends.
* **The rewrite** replays each reached block from its entry state:
  * a pure instruction that writes the value its register already holds is
    removed (this removes most of inlining's zeroing moves);
  * an instruction whose result is known becomes `mov dst, constant`;
  * a decided branch becomes `jmp` or disappears;
  * a register operand with a known value becomes the constant, where the
    instruction's own checks accept one. In a `call` or `ret`, only values
    the function `made` are substituted: those calls copy register words
    straight across (§8.2), and a constant among them costs a test per
    value, worth it only if the instruction that computed it can then go;
  * `simplify` puts constants second (where the quickened forms want
    them), and rewrites identities: `x+0`, `x*1`, `x|0` to a copy; `x-x`,
    `x^x`, `x*0` to 0; `x*2^k` to a shift; `beq x, 0` to `bz`.

  Every rewritten instruction must pass `check_insn`, or it is left alone.

### 19.4 The other passes

| Pass | Kind | Algorithm |
|---|---|---|
| `cse` | local, extended along chains | A table of up to 48 available expressions and loads per block. A block inherits its only predecessor's table, so a value is never assumed on a path that does not compute it. An entry dies when its register or a register it reads may be written; loads die at any instruction that may write memory (a store, a call, an object update). A repeat becomes `mov dst, holder`. |
| `copyprop` | local | Per block, a map from register to the register it is an exact copy of (same bank, full width); reads are renamed through it, address registers included. |
| `dce` | global, backward | Liveness per block, iterated over blocks in reverse to a fixpoint. Registers live in a handler are live across every instruction its region covers. A pure instruction whose writes are all dead is removed, except an `R` write that could free an object: a forward must-analysis (`immortal_regs`) proves which `R` registers hold only null or a pool constant, and only those writes may go. |
| `unreachable` | global | Depth-first search from block 0, then repeatedly any handler whose region holds a reached instruction. The rest is removed. |
| `branches` | linear | Threads jumps through chains of `jmp` (up to 32); decides branches on constants; removes a jump to the next instruction; turns `bcc L1; jmp L2; L1:` into `bncc L2` (integer compares only, because of NaN); turns a `jmp` to a `ret` into the `ret`. |
| `peephole` | linear | Removes `nop`, `mov x, x`, and the second of `mov a, b; mov b, a`. |
| `inline` | per call | Below. |
| `compact` | per function | Below. |
| `deadfuncs` | module | Functions reachable from the exported ones, `main` and anything undecodable, by `OM_FUNC` operands; the rest are removed and function indices renumbered. |
| `tailcall` | per call | `call g (...) -> (r...)` followed by `ret r...` becomes `tcall g (...)`, when `g` is in the module, both declare the same results, and the call is not in a `.try` region. |

### 19.5 Inlining

* **Which calls**: callees of up to 12 instructions anywhere, and up to 40
  at a call inside a loop. Loops are found with Tarjan's strongly connected
  components over the blocks: a block in a component of more than one
  block, or with an edge to itself, is in a loop. A callee must be in the
  same module, not the caller itself, have no handlers, declare its
  results, and use nothing tied to a frame (the data stack, `SP`, `FP`,
  `UP[n]`, `catch`, tail calls, coroutine switches).
* **Budgets**: a caller grows to at most twice its size at load plus 64
  instructions, and never past 4,000.
* **Registers**: each register the callee uses maps to the lowest register
  of the same bank the caller does not use, and the caller's window grows
  to cover them. Running out of registers (32 per bank) stops that inline.
* **The code** that replaces the call:

  ```
  mov  <param k>, <argument k>           for each argument
  mov  <each other callee register>, 0   a new window reads zero
  <the callee's body, registers mapped, labels shifted>
      each ret  →  mov <result j>, <value j> ... ; jmp done
  done:                                  (the instruction after the call)
  ```

  Constant propagation then removes the zeroing moves the body does not
  need. The inlined code keeps the callee's source lines. A fault inside it
  is reported in the caller's frame, which is why inlining is in `-O3`.

### 19.6 Compaction

`compact` renumbers each bank's registers so the window is as small as it
can be, and so the run a call must clear (§7) is as short as it can be.
Parameters keep their numbers. Among the rest, the registers that must be
cleared go next to the `R` bank (last in `I` and `F`, first in `P`), so the
one cleared run covers few others:

```
[ I: params | no clear | CLEAR ][ F: params | no clear | CLEAR ][ R: all ][ P: params | CLEAR | no clear ]
                       <──────────────── the one run a call clears ────────────────>
```

The read-before-write analysis is the verifier's (`plan_window`), redone on
the working form.

### 19.7 The driver

`opt_module` loads each function, then:

1. `cleanup` per function, up to 8 rounds, until a round changes nothing:
   constants (with branch facts), `cse`, `copyprop` and `peephole`,
   `branches`, `unreachable`, `dce`, compacting after each.
2. If `inline` is on: inline into each function in module order, and run
   `cleanup` again where anything changed. Then `tailcall`, if named.
3. `deadfuncs`, then `compact`, if on.
4. Re-encode the module.

**Passes and levels** are one X-macro (`OPT_PASSES` in `opt.h`): each
entry's name, level and description. A spec such as `2,-inline,tailcall`
is parsed left to right into a bit set (`OptConfig.passes`): a level
replaces the set with every pass of that level or lower, a name adds a
pass, `-name` removes one. **The report** (`OptReport`) counts changes per
pass, records each function's instructions and registers before and after,
and with `-v` logs each change (`ochange`) with its source line.

**The cache.** `modcache_optimize` looks up the optimized module under a
SHA-256 of the executable, the pass set and the module's bytes; a miss
optimizes and stores it. A run that asks for a report always optimizes, so
the report describes passes that ran.

## 20. The tools

The tools are built into the `mlwsvm` binary, and reach the VM through two
hooks in `VM` ([TOOLING.md](TOOLING.md)).

**The instruction hook** (`VM.hook`, `bool (*)(Process *, uint32_t pc)`) is
called before every instruction while a tool needs it (§8.3), and returning
true stops the scheduler (`SLICE_BREAK`). The program then runs on one
worker, so the tool sees every instruction in order.

* **The debugger** (`tools/debugger.c`) keeps its breakpoints in a per-
  function byte array (`Function.bp`), so the hook's test is one load.
  Stepping records the process and frame depth it started from: `step`
  stops at the next instruction, `next` at the next one at the same depth
  or shallower, `finish` at a shallower one. `run`/`continue` call
  `sched_run`, which returns when the hook stops it. Because a debugger
  shows registers the program never wrote, it makes every call clear its
  whole window (`vm_whole_windows`).
* **The profiler** (`tools/hooks.c`) counts each instruction executed per
  function (`Function.prof`), counts which opcodes ran quickened and which
  generically (the list that chose the quickened shapes), and every 997th
  instruction (a prime, so samples do not alias with loops) records the
  call stack, for flame graphs.
* **The tracer** prints each instruction with its process and place.

**The poll hook** (`VM.poll`) is called by worker 0 between slices. The
**monitor** (`tools/monitor.c`) uses it: a small HTTP/1.0 server with no
thread of its own, which for each request stops the world (§13.7) and
serves the dashboard (`web/dashboard.html`, compiled into the binary by
`build-tools/embed.c`), JSON (`/api/vm`, `/api/procs`, `/api/proc/N`,
`/api/exits`), or the table `mlwsvm top` shows (`/top`).

**Source positions.** Each instruction's assembly line is in
`MFunc.lines`; a translated program's line comes from the `.loc` runs
(`module_srcpos`, a binary search). Backtraces, the debugger and the
profiler show both.

**The test runner** (`tools/testrunner.c`) runs each `.masm` file in a
directory, reading its expectations from comments (`; expect-stdout:`,
`; expect-error:`, `; option:`, `; debug-script:`, `; expect-opt:` and
others; §22), and stops the whole run, naming the test, if one runs past a
timeout. **`mlwsvm doc`** generates `docs/ISA.md` from the opcode table and
`docs/NATIVES.md` from the native registry. **`mlwsvm bindgen`** asks Clang
for a header's syntax tree as JSON and writes `.extern`, `.cstruct`,
`.callback` and `.const` lines from it, with a comment for anything it
cannot express.

## 21. The platform layer

`src/platform/platform.h` is the only OS-specific interface, implemented by
`posix.c`, `posix_io.c` and `win32.c`:

* threads, `PlatLock` (short sections: `os_unfair_lock` on macOS),
  `PlatMutex` and `PlatCond`;
* clocks, sleeping and random bytes;
* files, directories, modification times and real paths;
* processes and pipes; sockets; the poller (kqueue or epoll);
* dynamic loading;
* the permission check behind trusted directories (`plat_path_trusted`).

Everything else is plain C11 with `<stdatomic.h>`. Memory is little-endian
on every host, and the binary formats spell out their byte order. The
Windows implementation is written but untested
([ROADMAP.md](ROADMAP.md#3-platform)).

## 22. Testing

**Tests are MLWSVM programs.** `tests/` holds the suite (302 programs) and
`examples/` holds self-checking examples, all in documented assembly. The
test runner reads expectations from comments in each file: expected output
lines, exit code or error code; VM options; a debugger script; what the
optimizer must do (`; expect-opt:`, `; expect-insns:`) or must not
(`; no-opt:`); and skips, each with a reason. Directories whose names start
with `_` hold helper modules.

**The same suite runs in many modes**, each aimed at a class of bug:

| Mode | What it catches |
|---|---|
| `make test` | Wrong results |
| `make check-gc` | A collection at every allocation (freed memory filled with a pattern): missing roots, use after free |
| `make check-opt` | Every module optimized by every pass, also under GC stress: optimizer bugs, on all the suite's code |
| `make check-asan` | AddressSanitizer and UBSan, also with every module optimized: memory errors and undefined behaviour in the VM |
| `make check-tsan` | ThreadSanitizer, on several workers: data races |
| `make check-poison` | New register windows filled with garbage: registers read before they are written but not cleared |
| `make check-switch`, `check-loop` | The fallback dispatch builds |
| a build without libffi | The FFI's tests are skipped cleanly |
| `make check-tools` | The monitor, `top`, the profiler, the tracer, `.loc`, the module cache, archives, and the optimizer's round trips and reports |
| `make check-embed`, `check-bindgen` | The embedding library and its example host, and generated bindings |

Unicode support is checked against the Unicode Consortium's own
normalization and segmentation test files. CI runs the modes on Linux (GCC
and Clang) and macOS (Clang). The compilers project (`mlws-compilers`)
generates large amounts of real code, and its test suites are a second,
independent test of the VM and the optimizer.

## 23. What is left out, and why

Each of these was considered, and most were measured
([PERFORMANCE.md](PERFORMANCE.md#what-is-not-done-and-why),
[ROADMAP.md](ROADMAP.md)).

* **Shared mutable state between processes.** Isolation is the design. It
  keeps the collector free of locks and failure contained. Constants and
  loaded code are the only shared things.
* **Unloading modules.** Closures may refer to any loaded function, so
  loaded code lives as long as the VM.
* **Turning integers into pointers.** That would defeat the checks of §10.
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
  carry one. Shrinking the 72-byte generic `Operand` is the fix to make if
  large programs show cache misses.
* **Removing type checks in the optimizer.** That needs the source
  language's types, so it stays in the compilers.
* **Lock-free run queues and spinning before sleeping.** The queue locks
  are almost always uncontended, and waking one thread at a time avoided the
  latency problem in every benchmark so far.
* **Windows.** Written, untested: it needs CI, MSVC or clang-cl builds, and
  an IOCP poller.
