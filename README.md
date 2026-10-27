# MLWSVM

A portable virtual machine in the spirit of the JVM, but a **register machine**
rather than a stack machine. It has variable-arity instructions (0–32 operands),
25 addressing modes, an assembler with macros, a module loader with
libraries, and checked memory safety. It is written in plain C11 with no
required dependencies (libffi, if present, enables the FFI), so it builds
anywhere a C compiler exists.

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
* **Rich types** as instruction suffixes: `b c i8 i16 i32 i64 u8 u16 u32 u64 f32 f64 r p big dec`.
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
* **Many isolated processes, in parallel**: preemptive scheduling on one worker
  thread per core with work stealing, `spawn`/`send`/`recv` with deep-copied
  messages, timeouts, Erlang-style `link`/`monitor` supervision, and per-process
  quotas on heap, raw memory, mailbox size and CPU.
* **Files, commands, the OS and the network** (`std.fs`, `std.os`, `std.net`): a
  process waiting on a pipe or socket waits in the poller (kqueue/epoll) like it
  waits in `recv`, holding no thread, so 10,000 open connections are 10,000 small
  processes. File I/O runs on a thread pool. Sandboxes can deny OS access with a
  quota.
* **Text and JSON**: a typed, safe `text.format` (`{:>8.2f}`), string utilities,
  UTF-8 iteration, and JSON that maps onto maps and lists with exact integers.
* **Standard library**: dates and ISO 8601 times, CRC-32 / SHA-256 / SipHash,
  random numbers, hex / base64 / URL / CSV, and sorting and searching with
  comparator closures.
* **Embeddable**: `libmlwsvm` and `include/mlwsvm/embed.h` let a C or C++ program
  load MLWSVM code from files or memory, call it, give it host functions, and
  run background services it feeds with messages.
* **Calling C**: native extensions written against a small, versioned C API
  (`include/mlwsvm/mlwsvm.h`), and a declarative FFI (`.extern`, via libffi) that
  calls C functions directly: variadic functions, structs by value and by
  pointer, finalizers, and callbacks (closures as C function pointers, or calls
  from C threads delivered as messages). `mlwsvm bindgen` writes the
  declarations for a C header. Blocking and non-thread-safe code is declared as
  such, and native code loads only from trusted, hash-pinned directories.
* **Structured exceptions**: `.try` regions, `throw`/`catch`, and zero cost until
  something is thrown. Every runtime fault is catchable as an `Error`.
* **Coroutines**: stackful coroutines inside a process (`conew`, `coresume`,
  `coyield`), passing values both ways, for generators and Lua-style
  `coroutine` libraries.
* **Tooling**: a gdb-style debugger (`mlwsvm debug`), an exact profiler with
  flame-graph output, an instruction tracer, and a live monitor with a web
  dashboard, JSON API and `mlwsvm top`.
* **Safe by construction**: bounds-checked memory, use-after-free detection,
  unforgeable references, and stable error codes with source backtraces.
* **Tested in its own assembly**: 244 test programs, also run under
  AddressSanitizer and UBSan, under ThreadSanitizer on several threads, and with a
  garbage collection at every allocation.

## Build and run

```sh
make                         # builds ./mlwsvm (a C11 compiler; libffi is used for .extern if found)
make test                    # runs tests/ and examples/
make check-gc                # the same, collecting garbage at every allocation
make check-tools             # smoke-test the monitor, top, profiler and tracer
make bench                   # benchmarks, with output checks (docs/PERFORMANCE.md)
make check-asan              # test and check-gc, under AddressSanitizer + UBSan
make check-tsan              # the tests on several threads, under ThreadSanitizer
make check-switch            # the tests with switch dispatch (for compilers without computed goto)
make lib check-embed         # the embedding library, and its example host program
make check-bindgen           # generate, compare and call bindings for a test header (needs clang)

./mlwsvm run examples/hello.masm
./mlwsvm run --stats examples/fib.masm
./mlwsvm run --threads 1 examples/workers.masm   # one worker thread: deterministic order
./mlwsvm run examples/http_server.masm 8080     # a supervised HTTP server: a process per connection
./mlwsvm run --max-frames 1000000 --quota heap=256M prog.masm   # VM options (docs/TOOLING.md)
./mlwsvm run --profile examples/fib.masm
./mlwsvm debug examples/fib.masm          # then: break fib, run, bt, print I0, ...
./mlwsvm run --monitor 7070 --hold examples/workers.masm   # open http://127.0.0.1:7070/
./mlwsvm asm prog.masm -o prog.mlwso && ./mlwsvm run prog.mlwso
./mlwsvm dis prog.mlwso          # disassemble
./mlwsvm asm -E prog.masm      # show the source after macro expansion
./mlwsvm test tests/           # run assembly tests
./mlwsvm bindgen -l libz -p z /usr/include/zlib.h -o zlib.inc   # FFI declarations for a C header
make docs                    # regenerate docs/ISA.md and docs/NATIVES.md
./mlwsvm --help                # every command and option
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
| [docs/TOOLING.md](docs/TOOLING.md) | Debugger, profiler, tracer, monitor, dashboard, `top`, and the bindings generator |
| [docs/STDLIB.md](docs/STDLIB.md) | The standard library: files, commands, networking, text and JSON, time, hashes, random numbers, encodings, sorting, and how I/O waits |
| [docs/EMBEDDING.md](docs/EMBEDDING.md) | Running MLWSVM inside a C or C++ program (`libmlwsvm`) |
| [docs/EXTENDING.md](docs/EXTENDING.md) | Calling C: native extensions, the declarative FFI, and the security model |
| [docs/PERFORMANCE.md](docs/PERFORMANCE.md) | Benchmarks and results, how the interpreter and scheduler are made fast, and what is left alone (and why) |
| [docs/REGISTER_VS_STACK.md](docs/REGISTER_VS_STACK.md) | Register machines and stack machines: how each works, their trade-offs and history, the same programs measured on MLWSVM, Lua, the JVM and CPython, and what 0–32 operands cost compared with a fixed 3 |
| [docs/ROADMAP.md](docs/ROADMAP.md) | Milestones M0–M23 and what remains: Windows and the deferred performance work |
| [docs/PRIOR_ART.md](docs/PRIOR_ART.md) | Where each idea comes from (Parrot, Lua, VAX, BEAM, JVM, WebAssembly, ...) and what may be new in the combination |

## Source layout

```
src/main.c      the mlwsvm command line
src/core/       types, opcode table (opcodes.def), instruction codec, shared checker, disassembler
src/asm/        lexer, constant expressions, preprocessor (macros etc.), assembler
src/loader/     .mlwso reader/writer
src/vm/         loader/linker, verifier, processes and memory, coroutines, scheduler, interpreter, VM options, run helper
src/heap/       garbage collector, heap objects (strings, bytes, arrays, structs, lists, maps), BigInt and decimal, message copying
src/native/     built-in and standard-library natives, the extension API and the FFI
src/embed/      the embedding API (libmlwsvm)
src/tools/      test runner, doc generator, debugger, profiler/tracer, monitor, top, ext, bindgen
src/platform/   the only OS-specific code (POSIX and Windows)
include/mlwsvm/ public C headers: the extension API (mlwsvm.h) and the embedding API (embed.h)
web/            the monitor's dashboard (compiled into the binary)
build-tools/    build helper that compiles the dashboard into C
extensions/     an example native extension (also a test fixture)
lib/std/        standard library and macro files, written in MLWSVM assembly
tests/          the test suite, in MLWSVM assembly
bench/          benchmarks (make bench); bench/compare: fib and loop in Lua, Java, WebAssembly and Python
examples/       runnable, self-checking example programs (examples/embed: a C host program)
```

`src/core/opcodes.def` is the single source of truth for the instruction set. The
assembler, disassembler, verifier and `docs/ISA.md` are all derived from it.

## Status

Everything described in `docs/` is implemented and tested on macOS and Linux
(GitHub Actions: Linux GCC and Clang, macOS Clang), in every test mode: the
plain suite, GC stress, AddressSanitizer, ThreadSanitizer, the tools, and builds
without libffi or computed goto. Windows support is written but untested.
[docs/ROADMAP.md](docs/ROADMAP.md) lists the milestones and what remains.
