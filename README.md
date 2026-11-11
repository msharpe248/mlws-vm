# MLWSVM

A portable virtual machine in the spirit of the JVM, but a **register machine**
rather than a stack machine. It has variable-arity instructions (0–32 operands),
25 addressing modes, an assembler with macros, a module loader with
libraries, and checked memory safety. Processes share nothing, can reach other
machines, can have their code replaced while they run, and can be limited to what
they are granted. It is written in plain C11 with no
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
  A program ships as one executable (`mlwsvm bundle`).
  Libraries ship as one versioned archive (`mlwsvm lib build`), state which
  versions of each other they need (`.requires json5, "^1.2"`), and keep
  their internal modules private. A running program can load more modules by
  name (`module.load`), call them through closures, and replace a module
  under running processes (`module.reload`), Erlang-style, with a generic
  server (`std.server`) that carries its state to the new code, as
  `gen_server`'s `code_change` does. Each program's own
  modules come from its own directory, so programs run together in one VM may
  both have a `main` or a `util`.
* **Rich heap types with a garbage collector**: strings, byte buffers, typed
  arrays, structs declared in assembly (`R0->Point.x`), dynamically typed lists,
  insertion-ordered maps, closures, BigInts (`123n`) and exact decimals (`19.99d`).
  The collector is precise and generational, one per process, and allocates
  by bumping a pointer through the free lines of its chunks (mark-region).
  Weak references, weak maps (with ephemerons) and finalizers are built in.
* **Many isolated processes, in parallel**: preemptive scheduling on one worker
  thread per core with work stealing, `spawn`/`send`/`recv` with deep-copied
  messages (large strings go by reference, so sending one costs the same at
  any size), timeouts, Erlang-style `link`/`monitor` supervision, and per-process
  quotas on heap, raw memory, mailbox size and CPU. A waiting process takes
  about 4 KB, and spawning, waking and ending one takes one to two
  microseconds.
* **Processes across machines** (`std.node`): VMs connect as nodes, `send` works on
  a pid on another node, and processes can be spawned, found and watched there.
  A message keeps its sharing and cycles, and is encoded once, where it is
  sent: between two VMs on one machine, about 4 million small messages a
  second.
* **Environments**: named values in the VM that a program's processes share
  (and named ones that services join), any value a message can be, loaded from
  `.mlws.env` files and watched for changes, as Erlang's application
  environment.
* **Files, commands, the OS and the network** (`std.fs`, `std.os`, `std.net`,
  and TLS through the system's OpenSSL, loaded when asked for): a
  process waiting on a pipe or socket waits in the poller (kqueue/epoll) like it
  waits in `recv`, holding no thread, so 10,000 open connections are 10,000 small
  processes. File I/O runs on a thread pool. Sandboxes can deny OS access with a
  quota, or grant it by name (`cap.restrict`, `--cap`): these paths, those hosts.
* **Text and JSON**: a typed, safe `text.format` (`{:>8.2f}`), regular expressions
  in linear time (`regex.*`, Unicode-aware), string utilities,
  UTF-8 iteration, and JSON that maps onto maps and lists with exact integers.
* **Unicode 17**: full case mapping and folding, NFC/NFD/NFKC/NFKD
  normalization, grapheme clusters and words, display width, and character
  properties (categories, identifiers), passing the Unicode Consortium's own
  normalization and segmentation tests.
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
* **Tooling**: a gdb-style debugger (`mlwsvm debug`), a debugger for editors
  (`mlwsvm dap`, the Debug Adapter Protocol), an exact profiler with
  flame-graph output, an instruction tracer, and a live monitor with a web
  dashboard, JSON API and `mlwsvm top`, which with an admin's key can also
  kill, send, reload and purge, and, under `mlwsvm serve`, start and stop
  programs in a VM that stays up, and debug any process of it in a page of
  its own (source, breakpoints, steps, the call stack, variables, the
  mailbox) while the others run. Keys that may read, or also act, are kept
  as hashes in a tokens file (`mlwsvm token`). A compiler marks which line of its
  source each stretch of code came from (`.loc "fib.lua", 3`), and backtraces,
  the debugger and the profiler show that line.
* **An optimizer**: `--opt 2` (or `mlwsvm opt` ahead of time) removes what
  compilers leave behind: constants computed at run time, dead moves, copies,
  jumps to jumps and unreachable code. `--opt 3` also inlines small functions.
  Each pass can be turned on or off, and a report says
  what each did.
* **A JIT**: `--jit on` compiles each hot function to arm64 or x86-64 machine
  code, which runs in mixed mode with the interpreter on the same registers
  and frames. Calls and loops take 52–79% less time, and the compilers'
  Lua, Erlang and Lox benchmarks 28–78% less (all but message passing);
  results, errors, instruction counts and preemption are the interpreter's.
* **A module cache**: a `.masm` file is assembled once and its module kept (and
  optimized once, with `--opt`), so a large generated program starts in
  milliseconds on later runs.
* **Safe by construction**: bounds-checked memory, use-after-free detection,
  unforgeable references, and stable error codes with source backtraces.
* **Tested in its own assembly**: 324 test programs, also run under
  AddressSanitizer and UBSan, under ThreadSanitizer on several threads, with a
  garbage collection at every allocation, with every module optimized, and
  with every function compiled by the JIT. A differential fuzzer (`mlwsvm
  fuzz`) runs random programs in each of these modes against the interpreter
  and shrinks any that disagree, and `make release-check` runs everything,
  the compilers' suites included, before a release.

## Performance

The Lua and Erlang compilers in
[mlws-compilers](https://github.com/msharpe248/mlws-compilers) compile each
benchmark's source to MLWSVM, which runs it interpreting and with its JIT
(`--jit on`). The reference implementations run the same source. Times are
whole runs in milliseconds, start-up included, the median of five on an
Apple M5 Pro. Every engine printed the same output.

| Lua | MLWSVM | MLWSVM JIT | Lua 5.5 | LuaJIT 2.1 |
|---|---|---|---|---|
| fib (32) | 60 | **29** | 53 | 10 |
| loop (30,000,000 iterations) | 202 | **54** | 218 | 17 |
| methods (2M method calls and objects) | 541 | **212** | 348 | 71 |
| strings (format, gmatch, gsub) | 198 | **104** | 134 | 100 |
| iterate (pairs, ipairs) | 73 | **32** | 58 | 6 |
| binarytrees (15) | 333 | **163** | 382 | 148 |
| nbody (500,000) | 862 | **362** | 424 | 25 |
| spectralnorm (500) | 233 | **90** | 185 | 9 |
| fannkuch (10) | 2,005 | **657** | 808 | 125 |

| Erlang | MLWSVM | MLWSVM JIT | Erlang/OTP 29 (JIT) |
|---|---|---|---|
| fib (30) | 42 | **17** | 90 |
| ring (1,000 processes, 1,000,000 messages) | 235 | **215** | 265 |
| binarytrees (16) | 566 | 198 | **129** |
| nbody (100,000 steps) | 484 | 187 | **148** |
| sorting (200,000 integers, maps) | 372 | 215 | **115** |

Bold marks the faster of MLWSVM's JIT and the reference interpreter or
BEAM; LuaJIT is there for scale. With its JIT, MLWSVM runs all nine Lua
programs faster than Lua 5.5 here, on the M5 (seven of them on the i7). LuaJIT,
a tracing JIT that specializes code to the types it sees, is 1.04–14.5× faster
than MLWSVM's JIT. BEAM's times include its start-up of
about 80 ms, which is most of its `fib` and almost a third of its `ring`.
Its code is 1.3–1.9× faster than MLWSVM's JIT on the other three. Start-up
is 3 ms for MLWSVM (with the module cache) and 2 ms for Lua.
[docs/PERFORMANCE.md](docs/PERFORMANCE.md) has the benchmarks on three
machines, the JIT's effect on each, the collector, and comparisons with the
JVM and CPython.

## Download

Each release's page has an archive for Linux (x86-64 and arm64, glibc 2.35
and later), macOS (Apple silicon and Intel, macOS 12 and later) and Windows
(x86-64 and arm64): `mlwsvm` with its `lib` directory, the examples, and a
`.sha256` file to check it by. Unpack it anywhere and run it from there:

```sh
tar xzf mlwsvm-VERSION-linux-x86_64.tar.gz && cd mlwsvm-VERSION-linux-x86_64
./mlwsvm run examples/hello.masm
```

It is not signed: macOS keeps a file a browser downloaded from running until
`xattr -d com.apple.quarantine mlwsvm` (one fetched with `curl` is not held),
and Windows' SmartScreen may ask once.

## Build and run

```sh
make                         # builds ./mlwsvm (a C11 compiler; libffi is used for .extern if found)
make test                    # runs tests/ and examples/
make check-gc                # the same, collecting garbage at every allocation, and checking the write barrier
make check-opt               # the same, with every module optimized (--opt all), also under GC stress
make check-jit               # the same, with every function and block in machine code (--jit all), and hot functions
make check-tools             # smoke-test the monitor, top, profiler, tracer, .loc, the module cache and the fuzzer
make check-fuzz              # 300 random programs in every mode, against the interpreter (mlwsvm fuzz)
make bench                   # benchmarks, with output checks (docs/PERFORMANCE.md)
make check-asan              # test and check-gc, under AddressSanitizer + UBSan, and fuzzed programs
make check-tsan              # the tests on several threads, under ThreadSanitizer
make check-switch            # the tests with switch dispatch (for compilers without computed goto)
make check-loop              # the tests with the dispatch loop (for compilers without guaranteed tail calls)
make check-poison            # the tests with new register windows full of garbage (checks what calls clear)
make check-regex             # regular expressions against Python's re, on 12,000 random patterns and texts
make lib check-embed         # the embedding library, and its example host program
make check-bindgen           # generate, compare and call bindings for a test header (needs clang)
make release-check           # all of the above, longer fuzzing, and the compilers' suites in every mode

./mlwsvm run examples/hello.masm
./mlwsvm run --stats examples/fib.masm
./mlwsvm run --threads 1 examples/workers.masm   # one worker thread: deterministic order
./mlwsvm run server.masm 8080 --also client.masm  # two programs in one VM, until both end
./mlwsvm run -L ~/mlws-libs prog.masm   # find imports there too (or --lib-path, $MLWSVM_PATH)
./mlwsvm lib build json5/                # a versioned library archive, json5-1.2.0.mlwsl
./mlwsvm lib list -L ~/mlws-libs         # the archives on the library path
./mlwsvm bundle -o app --jit on app.masm # one executable: the VM, the program and all it imports
./mlwsvm run examples/http_server.masm 8080     # a supervised HTTP server: a process per connection
./mlwsvm run --max-frames 1000000 --quota heap=256M prog.masm   # VM options (docs/TOOLING.md)
./mlwsvm run --opt 2 prog.masm         # optimize every module as it loads (docs/OPTIMIZER.md)
./mlwsvm run --jit on prog.masm        # hot functions as machine code (docs/JIT.md)
./mlwsvm opt -O2 -v prog.masm -o prog.mlwso   # optimize ahead of time, with a report of every change
./mlwsvm run --profile examples/fib.masm
./mlwsvm debug examples/fib.masm          # then: break fib, run, bt, print I0, ...
./mlwsvm run --monitor 7070 --hold examples/workers.masm   # open http://127.0.0.1:7070/
./mlwsvm serve --programs examples/serve   # start echo.masm in the dashboard, and Send it messages
./mlwsvm names                             # a name service, for nodes that connect by name (std.node)
./mlwsvm asm prog.masm -o prog.mlwso && ./mlwsvm run prog.mlwso
./mlwsvm dis prog.mlwso          # disassemble
./mlwsvm asm -E prog.masm      # show the source after macro expansion
./mlwsvm test tests/           # run assembly tests
./mlwsvm bindgen -l libz -p z /usr/include/zlib.h -o zlib.inc   # FFI declarations for a C header
make docs                    # regenerate docs/ISA.md and docs/NATIVES.md
./mlwsvm --help                # every command and option
```

On Windows, build in [MSYS2](https://www.msys2.org/)'s CLANG64 shell, where
the same `make` targets work (all but `check-asan` and `check-tsan`). MSYS2 is
needed only to build: `mlwsvm.exe` uses no DLL but Windows' own (libffi is
linked in), and runs anywhere with the `lib` directory beside it.

```sh
pacman -S make diffutils mingw-w64-clang-x86_64-clang mingw-w64-clang-x86_64-libffi mingw-w64-clang-x86_64-pkgconf
make CC=clang && make CC=clang test
```

## Platforms

MLWSVM is C11 and needs only the C library, its math library and threads
(libffi is optional). All of its OS-specific code is in `src/platform/`.
Where it runs, and how much of that is tested:

| Platform | Tested | JIT |
|---|---|---|
| macOS, arm64 (Apple silicon) | CI (Clang), and an Apple M5 Pro | yes |
| Linux, x86-64 | CI (GCC and Clang), and two Ubuntu 26.04 desktops: an Intel i7-11700KF (GCC) and an AMD Ryzen 9 5900X (Clang) | yes |
| Linux, arm64 | CI (Clang): the tests, the JIT, the fuzzer and AddressSanitizer | yes |
| Windows, x86-64 | CI (Clang in MSYS2), every test mode but the sanitizers | yes |
| macOS, x86-64 | CI (Clang): the tests, GC stress, the optimizer, the JIT, the fuzzer and the tools | yes |
| Windows, arm64 | CI (Clang in MSYS2): the same | yes |
| FreeBSD 15, x86-64 and arm64 | CI, in VMs: the tests, GC stress, the JIT and the fuzzer | yes |
| OpenBSD 8, x86-64 | CI, in a VM: the same | yes |
| NetBSD 11, x86-64 | CI, in a VM: the same | no (its GCC has no guaranteed tail calls) |
| Linux, RISC-V (riscv64) | CI, under QEMU: the tests, GC stress, the optimizer and the fuzzer | no |
| Linux, s390x (big-endian) | CI, under QEMU: the same | no |
| Linux, 32-bit x86 (i386) | CI (`gcc -m32`, with libffi): the tests, GC stress, the optimizer, the fuzzer and the tools | no |

On macOS, Linux x86-64 and Windows, CI runs every test mode: the plain
suite, GC stress, the optimizer, the JIT, the fuzzer and the tools, and
builds without libffi, tail calls or computed goto; AddressSanitizer and
ThreadSanitizer run on Linux and macOS. The other platforms are in the
Platforms workflow (`.github/workflows/platforms.yml`). Before each release,
`make release-check` runs all of it, and the compilers' suites, on the
three machines named above.

The JIT needs arm64 or x86-64 and the tail-call interpreter, which needs
Clang or GCC 15 and later. Without them `--jit` is accepted and programs
run in the interpreter, with the same results. MLWSVM's memory is
little-endian on every host, so a program reads the same bytes on s390x as
on x86-64. On a 32-bit host, lists, arrays and maps hold up to 2^26 elements
and raw blocks up to 1 GiB ([ABI.md §2.8](docs/ABI.md#28-limits)). In short, MLWSVM
needs a C11 compiler and POSIX threads or Windows; every system tried so far
passes its tests.

## Documentation

| | |
|---|---|
| [docs/GUIDE.md](docs/GUIDE.md) | Writing programs, a tour: the first ones, functions, values, errors, the library, processes, servers and pools, the web, testing, debugging, speed, and shipping |
| [examples/](examples/README.md) | Complete programs, commented, by topic: Unix tools (`cat`, `grep`, `wc`), CSV and JSON, exact decimals, generators, pipelines of processes, timers, errors, a key-value server, an HTTP server |
| [docs/learn/](docs/learn/README.md) | How MLWSVM works, for readers new to virtual machines: twelve chapters from bytecode and the interpreter loop to garbage collection, processes and the JIT, each linked to the code that does it |
| [docs/DESIGN.md](docs/DESIGN.md) | How the whole system is put together and why: the path from source to running code, and each part's data structures, algorithms and connections (a process in memory, the collector, the scheduler, the optimizer's analyses, ...) |
| [docs/ASSEMBLY.md](docs/ASSEMBLY.md) | The assembly language: syntax, directives, functions, data, macros, modules and libraries |
| [docs/ADDRESSING.md](docs/ADDRESSING.md) | Every addressing mode |
| [docs/ISA.md](docs/ISA.md) | Instruction reference (generated from `src/core/opcodes.def`) |
| [docs/ABI.md](docs/ABI.md) | Execution model: registers, calling convention, stack, memory, traps |
| [docs/TARGETING.md](docs/TARGETING.md) | A guide for compilers that target MLWSVM |
| [docs/NATIVES.md](docs/NATIVES.md) | Host functions (generated) |
| [docs/ERRORS.md](docs/ERRORS.md) | Error codes |
| [docs/BINARY_FORMAT.md](docs/BINARY_FORMAT.md) | The `.mlwso` file and instruction encoding |
| [docs/OPTIMIZER.md](docs/OPTIMIZER.md) | The optimizer: its levels and passes, what it keeps the same, its report, and what it gains |
| [docs/JIT.md](docs/JIT.md) | The JIT: when functions are compiled, what it compiles, how it runs with the interpreter, its memory, platforms, report, tests and gains |
| [docs/TOOLING.md](docs/TOOLING.md) | Debugger (and one for editors), profiler, tracer, monitor, dashboard, `top`, one-file executables, several programs in one VM, library archives (`mlwsvm lib`), the module cache, VM options, and the bindings generator |
| [docs/STDLIB.md](docs/STDLIB.md) | The standard library: files, commands, networking, TLS, other VMs, text, regular expressions and JSON, time, hashes, random numbers, encodings, sorting, and how I/O waits |
| [docs/EMBEDDING.md](docs/EMBEDDING.md) | Running MLWSVM inside a C or C++ program (`libmlwsvm`) |
| [docs/EXTENDING.md](docs/EXTENDING.md) | Calling C: native extensions, the declarative FFI, and the security model |
| [docs/PERFORMANCE.md](docs/PERFORMANCE.md) | Benchmarks and results, how the interpreter and scheduler are made fast, and what is left alone (and why) |
| [docs/REGISTER_VS_STACK.md](docs/REGISTER_VS_STACK.md) | Register machines and stack machines: how each works, their trade-offs and history, the same programs measured on MLWSVM, Lua, the JVM and CPython, and what 0–32 operands cost compared with a fixed 3 |
| [docs/ROADMAP.md](docs/ROADMAP.md) | Milestones M0–M118 and what remains: the deferred performance work |
| [docs/PRIOR_ART.md](docs/PRIOR_ART.md) | Where each idea comes from (Parrot, Lua, VAX, BEAM, JVM, WebAssembly, ...) and what may be new in the combination |

## Source layout

```
src/main.c      the mlwsvm command line
src/core/       types, opcode table (opcodes.def), instruction codec, shared checker, disassembler, arithmetic semantics, Unicode, version constraints
src/asm/        lexer, constant expressions, preprocessor (macros etc.), assembler
src/loader/     .mlwso module, .mlwsl library archive and bundle reader/writer
src/vm/         loader/linker, verifier, module cache, processes and memory, coroutines, scheduler, interpreter, JIT (jit.inc, with jit_a64.inc and jit_x64.inc), extensions, VM options, run helper
src/heap/       garbage collector, heap objects (strings, bytes, arrays, structs, lists, maps), BigInt and decimal, message copying
src/opt/        the optimizer: its working form of a function, the passes, the report
src/native/     built-in and standard-library natives (text, files, the OS and network, TLS, capabilities, nodes, module.*), the extension API and the FFI
src/embed/      the embedding API (libmlwsvm)
src/tools/      test runner, doc generator, debugger, the DAP server, bundle, profiler/tracer, monitor, top, ext, lib, opt, bindgen, fuzz
src/platform/   the only OS-specific code (POSIX and Windows)
include/mlwsvm/ public C headers: the extension API (mlwsvm.h) and the embedding API (embed.h)
web/            the monitor's dashboard and its debugger's page (compiled into the binary)
editors/        a VS Code extension that declares `mlwsvm dap`
build-tools/    build helpers: the monitor's pages compiled into C, the Unicode tables generated from the UCD
extensions/     an example native extension (also a test fixture)
lib/std/        standard library and macro files, written in MLWSVM assembly (std.node: distribution)
tests/          the test suite, in MLWSVM assembly
bench/          benchmarks (make bench); bench/compare: fib and loop in Lua, Java, WebAssembly and Python
examples/       runnable, self-checking example programs (examples/embed: a C host program)
```

`src/core/opcodes.def` is the single source of truth for the instruction set. The
assembler, disassembler, verifier and `docs/ISA.md` are all derived from it.

## Status

The current release is 0.28.0 (`mlwsvm version`); releases are tagged `vX.Y.Z`
on GitHub. Everything described in `docs/` is implemented and tested on macOS, Linux and
Windows ([Platforms](#platforms) has the details).
[docs/ROADMAP.md](docs/ROADMAP.md) lists the milestones and what remains.

## License

MIT; see [LICENSE](LICENSE).
