# MLWSVM

A portable virtual machine in the spirit of the JVM, but a **register machine**
rather than a stack machine. It has variable-arity instructions (0–32 operands),
about twenty addressing modes, an assembler with macros, a module loader with
libraries, and checked memory safety. It is written in plain C11 with no
dependencies, so it builds anywhere a C compiler exists.

```asm
.import std.io as io

.func fib(i64 n) -> i64
    blt n, 2, base
    sub I1, n, 1
    call fib (I1) -> (I2)
    sub I1, n, 2
    call fib (I1) -> (I3)
    add I0, I2, I3
    ret I0
base:
    ret n
.endf

.func main
    call fib (25) -> (I0)
    call io.println_i (I0)
    ret 0
.endf
```

## Highlights

* **Typed register banks.** `I0–I31` (integers), `F0–F31` (floats), `R0–R31` (GC
  references) and `P0–P31` (checked pointers), plus `SP`, `FP` and `PID`. Every call
  gets a fresh register window, so there are no caller/callee-saved conventions.
* **Rich types** as instruction suffixes: `b c i8 i16 i32 i64 u8 u16 u32 u64 f32 f64 r p`.
  Narrow integers wrap correctly.
* **Operands can be anything**: registers, literals, strings, `SP[42]`, `P1{13}`,
  `P0[I1*2+1]`, `[SP]+`, `-[SP]`, `@global[I3]`, `[FP[2]][3]`, `ARG[n]`, `LOC[n]`. Any
  operand can be in memory, including the destination.
* **Variadic instructions**: `add I0, I1, I2, I3, 4, SP[0]`. Calls have multiple
  arguments and results: `call f (a, b) -> (q, r)`. Tail calls are supported.
* **An assembler that is easy to hand-write and easy to target**: labels, numeric local
  labels, constants and expressions, macros with defaults, varargs and unique labels,
  `.rept`/`.irp`, conditional assembly, includes, and register aliases.
* **A loader with libraries**: `.import std.math as math`, a library search path,
  `.masm` or binary `.mlwso`, cycle detection, and a load-time verifier.
* **Safe by construction**: bounds-checked memory, use-after-free detection,
  unforgeable references, and stable error codes with source backtraces.
* **Tested in its own assembly**: 80+ test programs, also run under
  AddressSanitizer and UBSan.

## Build and run

```sh
make                         # builds ./mlwsvm (needs only a C11 compiler)
make test                    # runs tests/ and examples/
make check-asan              # the same, under AddressSanitizer + UBSan

./mlwsvm run examples/hello.masm
./mlwsvm run --stats examples/fib.masm
./mlwsvm asm prog.masm -o prog.mlwso && ./mlwsvm run prog.mlwso
./mlwsvm dis prog.mlwso          # disassemble
./mlwsvm asm -E prog.masm      # show the source after macro expansion
./mlwsvm test tests/           # run assembly tests
make docs                    # regenerate docs/ISA.md and docs/NATIVES.md
```

## Documentation

| | |
|---|---|
| [docs/ASSEMBLY.md](docs/ASSEMBLY.md) | The assembly language: syntax, directives, functions, data, macros, modules |
| [docs/ADDRESSING.md](docs/ADDRESSING.md) | Every addressing mode |
| [docs/ISA.md](docs/ISA.md) | Instruction reference (generated from `src/core/opcodes.def`) |
| [docs/ABI.md](docs/ABI.md) | Execution model: registers, calling convention, stack, memory, traps |
| [docs/TARGETING.md](docs/TARGETING.md) | A guide for compilers that target MLWSVM |
| [docs/NATIVES.md](docs/NATIVES.md) | Host functions (generated) |
| [docs/ERRORS.md](docs/ERRORS.md) | Error codes |
| [docs/BINARY_FORMAT.md](docs/BINARY_FORMAT.md) | The `.mlwso` file and instruction encoding |

## Source layout

```
src/core/       types, opcode table (opcodes.def), instruction codec, shared checker, disassembler
src/asm/        lexer, constant expressions, preprocessor (macros etc.), assembler
src/loader/     .mlwso reader/writer
src/vm/         loader/linker, verifier, processes and memory, interpreter, run helper
src/native/     built-in native functions
src/tools/      test runner, doc generator
src/platform/   the only OS-specific code (POSIX and Windows)
lib/std/        standard library and macro files, written in MLWSVM assembly
tests/          the test suite, in MLWSVM assembly
examples/       runnable, self-checking example programs
```

`src/core/opcodes.def` is the single source of truth for the instruction set. The
assembler, disassembler, verifier and `docs/ISA.md` are all derived from it.

## Status and roadmap

| Milestone | Status |
|---|---|
| M0 Skeleton: build, opcode table, encoding, disassembler | done |
| M1 Assembler: labels, expressions, directives, macros, includes | done |
| M2 Loader and interpreter: register windows, data stack, all addressing modes, verifier, test runner | done |
| M3 Libraries: imports/exports, library path, `lib/std` | done |
| M4 GC heap: strings, byte buffers, arrays, structs, lists, maps, handles | next |
| M5 Structured exceptions | planned |
| M6 Many concurrent isolated processes: scheduler, messages, quotas, accounting | planned |
| M7 Tooling: `mlwsvm top`, debugger, trace/profiler, web dashboard | planned |
| M8 Closures, BigInt, decimal | planned |
| M9 Performance: computed-goto dispatch, multi-threaded scheduler, generational GC | planned |
