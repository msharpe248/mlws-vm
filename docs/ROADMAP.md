# MLWSVM Roadmap

What is left to do after milestones M0–M9, in suggested order. Milestones M0–M9
are complete. Their status, and the measurements behind each deferral, are in
[README.md](../README.md#status-and-roadmap) and
[PERFORMANCE.md](PERFORMANCE.md).

## 1. Calling existing libraries (M10: done)

Done: the native-extension API (`include/mlwsvm/mlwsvm.h`), the declarative FFI
(`.extern`, via libffi), blocking and serial natives, foreign handles,
trusted installation (`mlwsvm ext`, SHA-256 pinning, permission checks), and
the foreign-call quota. See [EXTENDING.md](EXTENDING.md).

Still to do:

* **A binding generator**: `mlwsvm bindgen header.h`, which writes `.extern`
  declarations for a C header, in the spirit of Rust's bindgen and ctypesgen.
* **Callbacks from C into the VM** (comparators, event handlers). These need a
  re-entrant interpreter entry point. Deferred until a library needs them.
* **More FFI shapes**: structs by value, variadic functions, finalizers for
  `.extern` handles (`ptr<tag>` freed by a named C function).
* **Posting messages from native threads** to a process, for event-driven C
  libraries.
* **Windows**: ACL checks in `plat_path_trusted` (POSIX permission checks are
  done).

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
