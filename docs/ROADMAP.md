# MLWSVM Roadmap

Milestones M0–M22 are complete. What remains is Windows (section 3) and the
deferred performance work (section 4). M0–M10 built the VM itself. The aim of
M11–M22 was to make MLWSVM worth targeting: programs on it should be able to do
real work (files, the OS, the network, text and data), and its distinctive
feature (isolated, parallel, supervised processes) should be easy to show off.
The scripting language is a separate project.

## Milestones M0–M10

| Milestone | Contents |
|---|---|
| **M0 Skeleton** | build, opcode table, encoding, disassembler |
| **M1 Assembler** | labels, expressions, directives, macros, includes |
| **M2 Loader and interpreter** | register windows, data stack, all addressing modes, verifier, test runner |
| **M3 Libraries** | imports and exports, library path, `lib/std` |
| **M4 GC heap** | strings, byte buffers, arrays, structs, lists, maps, handles |
| **M5 Structured exceptions** | |
| **M6 Processes** | many concurrent isolated processes: scheduler, messages, quotas, accounting |
| **M7 Tooling** | `mlwsvm top`, debugger, trace and profiler, web dashboard |
| **M8 Advanced types** | closures, BigInt, decimal |
| **M9 Performance** | quickening, cheaper frames, `_setjmp` (1.2–7.8× faster); the multi-threaded scheduler (work stealing; 8.2× on 16 CPU-bound processes). A generational GC and more specialised memory forms were measured and deferred (section 4) |
| **M10 Calling C** | extension API, declarative FFI, trusted installation (`mlwsvm ext`) |

## Milestones M11–M22

| Milestone | Contents | Done when |
|---|---|---|
| **M11 Linux and CI** (done) | Build and pass every test mode on Linux. GitHub Actions on Linux and macOS: `test`, `check-gc`, `check-asan` (with LeakSanitizer on Linux), `check-tsan`, `check-tools`, and a build without libffi | CI is green on both |
| **M12 I/O core, files and the OS** (done) | An event loop inside the scheduler (kqueue on macOS and the BSDs, epoll on Linux): a process waiting for I/O waits like `recv`, costing no thread. `std.fs` (files, directories, metadata, stdin/stdout/stderr as streams), `std.os` (arguments, environment, exit status, running commands with pipes) | A program can copy files, walk directories and run a pipeline of commands, with many processes doing I/O at once |
| **M13 Networking** (done) | `std.net`: TCP listen/accept/connect/read/write, UDP, DNS (on the I/O thread pool), timeouts, all on the event loop | An echo server holds 10,000 idle connections as 10,000 processes |
| **M14 Text and data** (done) | `std.text`: a safe `format`, UTF-8 iteration and validation, case mapping, split/join/trim/find/replace. `std.json`: parse and serialize, with depth and size limits | JSON round-trips a test corpus; `format` covers integers, floats, strings and padding |
| **M15 Showcase: a supervised HTTP server** (done) | `std.proc` (a name → pid registry, request/reply calls with timeouts, supervisors with restart strategies); `examples/http_server.masm`: one process per connection, per-connection quotas, a crash in a handler is contained and logged, watched live with `mlwsvm top`; a load generator (`examples/http_load.masm`) | The example serves requests under load while handlers are killed and restarted |
| **M16 Embedding API** (done) | `libmlwsvm` (static and shared) and `include/mlwsvm/embed.h`: create a VM, set options, load modules from files or memory, call functions, send messages, register natives (reusing the extension API), and an example C host program | A C program embeds the VM and calls MLWSVM code both ways |

Then, in order (Windows is set aside until there is a machine to test on):

| Milestone | Contents |
|---|---|
| **M17 FFI shapes** (done) | variadic C functions (1.1); structs by value and by pointer, `.cstruct` (1.2); finalizers for `.extern` handles (1.5) |
| **M18 Callbacks** (done) | C calling back into MLWSVM during a call, through a re-entrant interpreter (1.3); asynchronous callbacks delivered as messages, and `api->post` for extensions (1.4) |
| **M19 Standard library II** (done) | `std.time`, `std.hash`, `std.random`, `std.encoding` (hex, base64, URL, CSV), `std.collections` (sorting with a comparator closure, using M18's re-entrant calls), more `std.math` (section 2) |
| **M20 Bindgen** (done) | `mlwsvm bindgen header.h`: `.extern`, `.cstruct` and `.callback` declarations from clang's JSON AST (1.5) |
| **M21 Performance** (done) | a per-process size-class allocator for small objects (`binarytrees` and `maps` 17–25% faster on Linux, `binarytrees` 9% on macOS) |
| **M22 Performance** (done) | a faster interpreter loop: the program counter in a CPU register, threaded dispatch, calls that copy arguments straight between windows, compact operands, field and element fast paths (`fib` 26–41% and the tight loop 55–63% less time) |

## 1. Calling existing libraries (M10: done)

Done: the native-extension API (`include/mlwsvm/mlwsvm.h`), the declarative FFI
(`.extern`, via libffi), blocking and serial natives, foreign handles,
trusted installation (`mlwsvm ext`, SHA-256 pinning, permission checks), and
the foreign-call quota. See [EXTENDING.md](EXTENDING.md).

Variadic functions (1.1), C structs (1.2) and finalizers for `.extern`
handles were added in M17, callbacks (1.3, 1.4) in M18, and the binding
generator in M20. Only the Windows trust check (1.5) is still to do.

### 1.1 Variadic functions (M17: done)

`...` in an `.extern` parameter list ends the fixed parameters, and the call
goes through `ffi_prep_cif_var`. That matters on Apple arm64, where variadic
arguments go on the stack even when registers are free. Each declaration fixes
the variadic part's types, and C's promotions are enforced: `f32` and integers
narrower than `int` are refused after `...`. Format strings remain trusted
input: a wrong `%s` or a `%n` crashes or corrupts memory as it would in C.
`text.format` is the safe way for MLWSVM code to format text.

### 1.2 Structs by value and by pointer (M17: done)

`.cstruct NAME` … `.ends` declares a C layout and a struct of the same name;
`.extern` passes it as `struct NAME` (by value, and as a result),
`struct* NAME` (a pointer to a copy, copied back after the call) or
`out struct NAME`. Fields are numbers, `cstr`, `ptr<tag>`, nested structs and
fixed arrays of numbers (`u8[N]` as a byte buffer, others as a list). A struct
crosses the boundary as a copy, so pointers into VM memory never escape. The
VM computes C's offsets itself and checks them against libffi's size.

Not supported: bitfields, `#pragma pack` and unions (declare a union as a
`u8[N]` of its size). Layouts that differ by platform (`struct stat`) are best
generated from the system headers (see 1.5).

### 1.3 Callbacks during a call (M18: done)

`.callback NAME (params) -> (result)` declares a C function-pointer type, and
an `.extern` parameter `callback NAME` takes a closure, which becomes a libffi
closure for the duration of the call. The interpreter is re-entrant: the
callback runs in the calling process, on top of the frames of the foreign call,
until it returns. An uncaught exception stops at the callback and is raised when
the foreign call returns, so it never unwinds C. A callback cannot wait
(`E_CALLBACK`). The call's arguments are pinned against the collector while it
runs. Callbacks may nest, and work in blocking calls (on the I/O thread).

### 1.4 Asynchronous callbacks and posting messages (M18: done)

`.callback NAME (params), async` makes callbacks that C may call later, from any
thread: `ffi.async_callback(type, pid)` makes a function pointer whose every
call becomes a `Callback{name, args}` message. Extensions post messages from
their own threads with `api->post_int` and `api->post_str` (Erlang's
`enif_send`), and `api->hold` tells deadlock detection that messages may still
come. A callback that must return a value to C from another thread is not
supported. It would have to block that thread until the process replied.

### 1.5 Other

* **A binding generator** (M20: done): `mlwsvm bindgen header.h` writes
  `.extern`, `.cstruct`, `.callback` and `.const` declarations from clang's
  JSON syntax tree ([TOOLING.md](TOOLING.md#bindings-generator-mlwsvm-bindgen)).
  Run on the target machine, it also solves platform-dependent layouts.
* **Finalizers for `.extern` handles** (M17: done): `.ffi_free db,
  "libsqlite3", "sqlite3_close"` frees every `ptr<db>` when it is collected or
  released with `foreign.release`.
* **Windows**: ACL checks in `plat_path_trusted` (POSIX permission checks are
  done).

## 2. Standard library (M12–M15 and M19: done)

M19 added `time` (clocks, ISO 8601, calendar dates), `hash` (CRC-32, SHA-256,
SipHash), `random`, the encodings (hex, base64, URL, CSV), `list.sort` and
`list.bsearch` with closures, set operations on maps, and integer maths; see
[STDLIB.md](STDLIB.md). The table below was the plan.

Proposed scope: everything that needs the operating system, or must be fast and
correct (parsing, formatting, hashing), lives in the VM's standard library. Pure
algorithms and idioms belong to the scripting language's own library, built on
top. Details are in the design discussion; this list is the starting point.

| Module | Contents |
|---|---|
| `std.io` / `std.fs` | files (open/read/write/seek/close), stdin/stdout/stderr, directories, paths, file metadata |
| `std.os` | command-line arguments, environment, exit, running external commands (with pipes) |
| `std.net` | TCP and UDP sockets, DNS; non-blocking underneath, so only the calling *process* waits |
| `std.time` | wall and monotonic clocks, ISO 8601 formatting and parsing, durations |
| `std.text` | printf-style `format` (number parsing exists: the `parse` instruction), UTF-8 iteration, validation and case mapping, split/join/trim/find/replace |
| `std.encoding` | hex, base64, URL encoding, JSON parse and serialize, CSV |
| `std.hash` / `std.random` | SipHash, CRC-32, SHA-256; a seeded PRNG plus OS entropy |
| `std.collections` | sort and binary search (with a comparator closure), set operations on maps |
| `std.proc` | a process registry (name → pid), request/reply calls with timeouts, supervisors, worker pools |
| `std.math` | (exists) plus integer helpers: gcd, powmod, bit operations |

Later, through the FFI rather than written from scratch: compression (zlib),
cryptography (the platform's crypto library), SQLite, regular expressions (a
linear-time engine such as RE2).

Possible addition: **capabilities**, meaning permission to use the file system,
the network or native code, granted per process and inherited like quotas, so an
untrusted process cannot reach the OS at all (compare Deno's permissions).

## 3. Platform

* Linux and CI: M11.
* Windows: `src/platform/win32.c` is written but untested, including the
  thread layer. It needs CI on Windows, MSVC or clang-cl builds, C11 atomics
  there, and an IOCP backend for the M12 event loop.

## 4. Performance (when measurements call for it)

Each of these was measured and deferred; see [PERFORMANCE.md](PERFORMANCE.md).

* ~~A size-class allocator for the GC heap~~ (M21: done).
* ~~Threaded dispatch, cheaper calls, compact operands for the quickened
  forms, field and element shapes~~ (M22: done).
* More specialised shapes (arithmetic directly on elements, raw memory),
  chosen from what the scripting-language compiler actually emits.
* A compact decoded form for every instruction. Quickened instructions carry a
  compact copy since M22, but each decoded operand still takes 72 bytes, so
  programs with a lot of hot code may run out of cache before the benchmarks do.
* Calls, further. Lua 5.5 still runs `fib` 1.4× faster: its callee's window
  starts at the caller's argument registers, while MLWSVM sets up a window per
  register bank ([REGISTER_VS_STACK.md](REGISTER_VS_STACK.md)).
* A generational collector, if collection grows past roughly 10% of real
  workloads (it is 11% of `binarytrees`).
* Lock-free run queues and spinning before sleeping, for many-thread,
  message-heavy programs.

## 5. The scripting language

A separate project that targets MLWSVM's assembly
([TARGETING.md](TARGETING.md)). Its generated code should drive the decisions in
section 4.
