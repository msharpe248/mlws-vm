# MLWSVM Roadmap

What is left to do after milestones M0–M9, in suggested order. Milestones M0–M9
are complete. Their status, and the measurements behind each deferral, are in
[README.md](../README.md#status-and-roadmap) and
[PERFORMANCE.md](PERFORMANCE.md).

## 1. Calling existing libraries (proposed: M10)

Today natives are compiled into the VM: a table of `NativeDef` entries in
`src/native/natives.c`, called with `ncall "name" (args) -> (results)` and
resolved at load time. The plan has three layers, each usable without the next.

1. **A stable native-extension API.** Publish `include/mlwsvm/mlwsvm.h`, a small
   versioned C API for writing natives outside the VM: argument and result
   access, creating strings, bytes, lists and maps, raising errors, and handles to
   foreign resources with a finalizer the GC runs. Extensions are shared
   libraries, loaded on request (`.import native "sqlite3"` or `--native lib`),
   that register their natives by name. This is the model of Lua's C API,
   CPython extension modules, JNI and Erlang NIFs.
2. **Declarative FFI**: call a C function directly from a declaration in
   assembly, with no C glue. For example:
   `.extern "libm" cos(f64) -> f64` or `.extern "libsqlite3" sqlite3_open(cstr, out ptr) -> i32`.
   The VM opens the library, finds the symbol, converts arguments (string to
   NUL-terminated `char *`, bytes to a pointer and length, foreign pointers as
   opaque handles), and makes the call through libffi. libffi is an optional
   build dependency, and the extension API (layer 1) works without it. This is
   the model of Python's ctypes and cffi, LuaJIT's FFI and Java's FFM API.
3. **A binding generator**: `mlwsvm bindgen header.h`, which writes the
   `.extern` declarations for a C header, in the spirit of Rust's bindgen and
   ctypesgen.

Cross-cutting requirements:

* **Blocking calls must not block a worker thread.** A native marked `blocking`
  runs on a separate pool of I/O threads, and the calling process waits as it
  does in `recv` (Erlang's dirty schedulers).
* **Foreign code is trusted code.** Loading native code needs explicit
  permission (`--allow-native`), and a sandboxed process cannot load any.
* **Memory safety at the boundary.** Raw C pointers never enter `P` registers
  (those are checked pointers). They are opaque `foreign` handles. Buffers passed
  to C are valid only for the call, which the non-moving GC makes cheap to
  guarantee.
* **Callbacks from C into the VM** (comparators, event handlers) need a
  re-entrant interpreter entry point. This is deferred until a library actually
  needs it.

## 2. Standard library (proposed: M11)

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
| `std.text` | number parsing and formatting (`parse_int`, `parse_float`, printf-style `format`), UTF-8 iteration, validation and case mapping, split/join/trim/find/replace |
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

* Build and test on Windows (`src/platform/win32.c` is written but untested,
  including the thread layer), and on Linux in CI (LeakSanitizer runs there).
* Continuous integration running `make test check-gc check-asan check-tsan`.

## 4. Performance (when measurements call for it)

Each of these was measured and deferred; see [PERFORMANCE.md](PERFORMANCE.md).

* A size-class allocator for the GC heap: `malloc` is about 10% of
  allocation-heavy benchmarks.
* Specialised memory and array operand forms, chosen from what the
  scripting-language compiler actually emits.
* A generational collector, if collection grows past roughly 10% of real
  workloads.
* Computed-goto dispatch, if profiles ever show dispatch dominating.
* Lock-free run queues and spinning before sleeping, for many-thread,
  message-heavy programs.

## 5. The scripting language

A separate project that targets MLWSVM's assembly
([TARGETING.md](TARGETING.md)). Its generated code should drive the decisions in
section 4.
