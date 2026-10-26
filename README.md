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
* **Rich heap types with a garbage collector**: strings, byte buffers, typed
  arrays, structs declared in assembly (`R0->Point.x`), dynamically typed lists,
  insertion-ordered maps, closures, BigInts (`123n`) and exact decimals (`19.99d`).
  The collector is a precise, per-process mark-sweep.
* **Many isolated processes**: preemptive scheduling, `spawn`/`send`/`recv` with
  deep-copied messages, timeouts, Erlang-style `link`/`monitor` supervision, and
  per-process quotas on heap, raw memory, mailbox size and CPU.
* **Structured exceptions**: `.try` regions, `throw`/`catch`, and zero cost until
  something is thrown. Every runtime fault is catchable as an `Error`.
* **Tooling**: a gdb-style debugger (`mlwsvm debug`), an exact profiler with
  flame-graph output, an instruction tracer, and a live monitor with a web
  dashboard, JSON API and `mlwsvm top`.
* **Safe by construction**: bounds-checked memory, use-after-free detection,
  unforgeable references, and stable error codes with source backtraces.
* **Tested in its own assembly**: 100+ test programs, also run under
  AddressSanitizer and UBSan, and with a garbage collection at every allocation.

## Build and run

```sh
make                         # builds ./mlwsvm (needs only a C11 compiler)
make test                    # runs tests/ and examples/
make check-gc                # the same, collecting garbage at every allocation
make check-tools             # smoke-test the monitor, top, profiler and tracer
make bench                   # benchmarks, with output checks (docs/PERFORMANCE.md)
make check-asan              # both of the above, under AddressSanitizer + UBSan

./mlwsvm run examples/hello.masm
./mlwsvm run --stats examples/fib.masm
./mlwsvm run --profile examples/fib.masm
./mlwsvm debug examples/fib.masm          # then: break fib, run, bt, print I0, ...
./mlwsvm run --monitor 7070 --hold examples/workers.masm   # open http://127.0.0.1:7070/
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
| [docs/TOOLING.md](docs/TOOLING.md) | Debugger, profiler, tracer, monitor, dashboard and `top` |
| [docs/PERFORMANCE.md](docs/PERFORMANCE.md) | Benchmarks, results, and what was optimised (and why) |
| [docs/PRIOR_ART.md](docs/PRIOR_ART.md) | Where each idea comes from (Parrot, Lua, VAX, BEAM, JVM, WebAssembly, ...) and what may be new in the combination |

## Source layout

```
src/core/       types, opcode table (opcodes.def), instruction codec, shared checker, disassembler
src/asm/        lexer, constant expressions, preprocessor (macros etc.), assembler
src/loader/     .mlwso reader/writer
src/vm/         loader/linker, verifier, processes and memory, interpreter, run helper
src/heap/       garbage collector and heap objects (strings, bytes, arrays, structs, lists, maps)
src/native/     built-in native functions
src/tools/      test runner, doc generator, debugger, profiler/tracer, monitor, top
web/            the monitor's dashboard (compiled into the binary)
src/platform/   the only OS-specific code (POSIX and Windows)
lib/std/        standard library and macro files, written in MLWSVM assembly
tests/          the test suite, in MLWSVM assembly
bench/          benchmarks (make bench)
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
| M4 GC heap: strings, byte buffers, arrays, structs, lists, maps, handles | done |
| M5 Structured exceptions | done |
| M6 Many concurrent isolated processes: scheduler, messages, quotas, accounting | done |
| M7 Tooling: `mlwsvm top`, debugger, trace/profiler, web dashboard | done |
| M8 Closures, BigInt, decimal | done |
| M9 Performance: single-thread interpreter (quickening, frames, `_setjmp`; 1.2–7.8× faster) | done |
| M9 Performance: multi-threaded scheduler | next |
| M9 Performance: generational GC, more specialised memory forms | deferred: measured as low value for now ([PERFORMANCE.md](docs/PERFORMANCE.md)) |
