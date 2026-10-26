# MLWSVM Roadmap

Milestones M0–M10 are complete (see [README.md](../README.md#status-and-roadmap)).
The aim of the next milestones is to make MLWSVM worth targeting: programs on it
should be able to do real work (files, the OS, the network, text and data), and
its distinctive feature (isolated, parallel, supervised processes) should be
easy to show off. The scripting language is a separate project.

## Next milestones

| Milestone | Contents | Done when |
|---|---|---|
| **M11 Linux and CI** (done) | Build and pass every test mode on Linux. GitHub Actions on Linux and macOS: `test`, `check-gc`, `check-asan` (with LeakSanitizer on Linux), `check-tsan`, `check-tools`, and a build without libffi | CI is green on both |
| **M12 I/O core, files and the OS** (done) | An event loop inside the scheduler (kqueue on macOS and the BSDs, epoll on Linux): a process waiting for I/O waits like `recv`, costing no thread. `std.fs` (files, directories, metadata, stdin/stdout/stderr as streams), `std.os` (arguments, environment, exit status, running commands with pipes) | A program can copy files, walk directories and run a pipeline of commands, with many processes doing I/O at once |
| **M13 Networking** (done) | `std.net`: TCP listen/accept/connect/read/write, UDP, DNS (on the I/O thread pool), timeouts, all on the event loop | An echo server holds 10,000 idle connections as 10,000 processes |
| **M14 Text and data** (done) | `std.text`: a safe `format`, UTF-8 iteration and validation, case mapping, split/join/trim/find/replace. `std.json`: parse and serialize, with depth and size limits | JSON round-trips a test corpus; `format` covers integers, floats, strings and padding |
| **M15 Showcase: a supervised HTTP server** | `std.proc` (a name → pid registry, request/reply calls with timeouts, supervisors with restart strategies); `examples/http`: one process per connection, per-connection quotas, a crash in a handler is contained and logged, watched live with `mlwsvm top`; a load-test benchmark | The example serves requests under load while handlers are killed and restarted |
| **M16 Embedding API** | `libmlwsvm` (static and shared) and `include/mlwsvm/embed.h`: create a VM, set options, load modules from files or memory, call functions, send messages, register natives (reusing the extension API), and an example C host program | A C program embeds the VM and calls MLWSVM code both ways |

Later, when real use asks for them: the FFI additions (section 1), the rest of
the standard library (section 2), Windows (section 3), and the performance work
(section 4).

## 1. Calling existing libraries (M10: done)

Done: the native-extension API (`include/mlwsvm/mlwsvm.h`), the declarative FFI
(`.extern`, via libffi), blocking and serial natives, foreign handles,
trusted installation (`mlwsvm ext`, SHA-256 pinning, permission checks), and
the foreign-call quota. See [EXTENDING.md](EXTENDING.md).

Still to do, in suggested order. The three FFI limitations can all be removed:
libffi supports each of them directly, and the work is on the VM side.

### 1.1 Variadic functions (small)

libffi calls them with `ffi_prep_cif_var`. This matters on Apple arm64, where
variadic arguments go on the stack even when registers are free, so treating
`printf` as an ordinary function would be wrong there. The types of the
variadic part are fixed per declaration; declare one extern per shape you
need:

```asm
.extern c.printf_si, "", "printf" (cstr, ..., cstr, i32) -> (i32)
.extern c.printf_f,  "", "printf" (cstr, ..., f64) -> (i32)
```

* Parsing `...` in the parameter list, and recording the fixed-argument
  count in the module (format version 5).
* C's default promotions are checked: `f32` and integers narrower than
  `int` are not allowed after `...`, because C would promote them.
* Caveat: format strings remain trusted input. A wrong `%s` or a `%n` crashes
  or corrupts memory as it would in C. The standard library's own `format`
  (section 2) is the safe way for MLWSVM code to format text.

### 1.2 Structs by value and by pointer (medium)

libffi describes a struct as an `ffi_type` with element types, and computes
its size, alignment and field offsets (`ffi_get_struct_offsets`). The VM needs
a way to declare C layouts and to convert them:

```asm
.cstruct timespec
    tv_sec:  i64
    tv_nsec: i64
.ends
.extern c.nanosleep, "", "nanosleep" (struct* timespec, struct* timespec) -> (i32), blocking
.extern m.div,       "", "div" (i32, i32) -> (struct div_t)          ; returned by value
```

* `.cstruct` fields use the FFI types (integers, floats, `ptr<tag>`, `cstr`
  read-only), nested `.cstruct`s, and fixed arrays (`buf[16]`).
* `struct NAME` passes a value; `struct* NAME` passes a pointer to a
  temporary; `out struct NAME` returns one through a pointer. On the MLWSVM
  side the value is an ordinary `.struct` with the same field names, or a
  byte buffer holding the raw bytes.
* A C struct crosses the boundary as a copy, so pointers into VM memory never
  escape.
* Not expressible in libffi, so not supported: bitfields, `#pragma pack`, and
  unions (a union can be declared as a byte buffer of its size). Layouts that
  differ by platform (`struct stat`) are best generated from the system
  headers (see 1.5).

### 1.3 Callbacks during a call (medium to large)

For example, a `qsort` comparator, `bsearch`, tree walkers, or `sqlite3_exec`
row callbacks: C calls back on the same thread, before the foreign call
returns. libffi closures (`ffi_closure_alloc`, `ffi_prep_closure_loc`) make a
real C function pointer from an MLWSVM closure; modern libffi uses static
trampolines, so this works where writable+executable memory is forbidden
(macOS hardened runtime).

```asm
.callback cmp_t (ptr, ptr) -> (i32)       ; a C function-pointer type
.extern c.qsort, "", "qsort" (buf, u64, u64, callback cmp_t)
    closure compare () -> (R1)
    ncall "c.qsort" (R0, I0, 8, R1)
```

What the VM needs:

* **A re-entrant interpreter entry**: run the closure on the same process, on
  top of the frames of the instruction making the foreign call, until it
  returns. The collector may run inside the callback; that is safe because the
  outer call's values are still in registers (roots), and objects never move.
* **Exceptions stop at the boundary.** A throw inside the callback must not
  unwind through C (`qsort`'s frames). It is caught there: the callback returns
  a zero value to C, and the error is raised once the foreign call returns.
  This is the same deferred-error rule natives follow.
* **No scheduling inside.** The C stack is in use, so a callback runs to
  completion. It is not preempted, and `recv`, `sleep` and other blocking
  instructions trap (`E_CALLBACK`) rather than wait.
* **Lifetime**: a callback is a foreign handle that owns its libffi closure.
  C must not keep the function pointer after the handle is released.
* Works for blocking calls too: the I/O thread runs the callback, since it owns
  the process for the duration of the call.

### 1.4 Asynchronous callbacks and posting messages (medium)

Event loops, timers, audio or GUI toolkits, and libraries with their own
threads call back *later*, often on another thread, when the process may be
running elsewhere. Running MLWSVM code there would break process isolation.
So these callbacks become **messages**:

* `.callback ... , async PID`-style declarations make a C function pointer that
  packs its arguments into a message to a chosen process and returns at once
  (only `void` callbacks).
* The extension API gains `api->post(vm, pid, value)`, callable from any
  thread, to send a message from native code. This is Erlang's `enif_send`.
* A callback that must return a value to C can block its own (foreign) thread
  until the process replies, but never on a worker thread: that is detected and
  refused, because it would deadlock.

### 1.5 Other

* **A binding generator**: `mlwsvm bindgen header.h`, which writes `.extern`,
  `.cstruct` and `.callback` declarations for a C header, in the spirit of
  Rust's bindgen and ctypesgen. Most practical through libclang's JSON AST dump
  (`clang -Xclang -ast-dump=json`). It also solves platform-dependent layouts.
* **Finalizers for `.extern` handles**: `ptr<tag>` freed by a named C function
  (`.extern ... -> (ptr<db> free=sqlite3_close)`), so FFI resources are not
  leaked when MLWSVM code forgets to close them.
* **Windows**: ACL checks in `plat_path_trusted` (POSIX permission checks are
  done).

## 2. Standard library (M12–M15 cover the first part)

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
