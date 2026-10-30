# Extending MLWSVM: Native Extensions and the FFI

MLWSVM code can call C in two ways. Both appear to MLWSVM code as ordinary
**natives**, called with `ncall "name" (args) -> (results)` exactly like the
built-in ones in [NATIVES.md](NATIVES.md):

| | Extension API | Declarative FFI |
|---|---|---|
| What you write | A small C library against [`include/mlwsvm/mlwsvm.h`](../include/mlwsvm/mlwsvm.h) | One `.extern` line per C function, in assembly |
| Good for | Wrapping a library properly: converting types, owning resources, reporting errors well | Calling existing C functions directly, with no C code at all |
| Values | Strings, bytes, lists, maps and foreign handles, through the API | Numbers, strings, byte buffers, opaque pointers, C structs and callbacks |
| Models | Lua's C API, CPython extensions, JNI, Erlang NIFs | Python's ctypes and cffi, LuaJIT's FFI, Java's FFM API |
| Needs | A C compiler | libffi, found when MLWSVM is built (it ships with macOS) |

Native code runs inside the VM's process with all of its privileges. A bug in it
can crash the VM, and a malicious library can do anything the user could.
MLWSVM's isolation and quotas only apply to MLWSVM code, so native code is only
loaded from places the user has deliberately trusted. See [Security](#security).

---

## Writing an extension

An extension is a shared library with the file suffix `.mlwsx`. It exports one
function, `MLWSVM_EXTENSION_INIT`, which receives a table of VM functions
(`MlwsApi`) and defines natives. It links against nothing from the VM.
[`extensions/demo/demo.c`](../extensions/demo/demo.c) is a complete example
that uses every part of the API.

```c
#include <mlwsvm/mlwsvm.h>
#include <stdio.h>

static const MlwsApi *api;

/* hello.greet(name) -> "hello, NAME" */
static void greet(MlwsCtx *cx, const MlwsValue *args, MlwsValue *results) {
    size_t n;
    const char *name = api->str_data(cx, args[0], &n);
    if (!name) return;                       /* not a string: str_data recorded E_TYPE */
    char buf[256];
    int len = snprintf(buf, sizeof buf, "hello, %.*s", (int)n, name);
    results[0] = api->str_new(cx, buf, (size_t)len);
}

MLWSVM_EXPORT int MLWSVM_EXTENSION_INIT(const MlwsApi *a, MlwsExtension *ext) {
    api = a;
    MlwsNativeDesc d = {"hello.greet", 1, {MLWS_T_R}, 1, {MLWS_T_R}, 0, greet, NULL, "greet someone"};
    return a->define(ext, &d) ? 0 : 1;
}
```

Build it, install it, and use it:

```sh
cc -shared -fPIC -I include -o hello.mlwsx hello.c
mlwsvm ext install hello.mlwsx
```

```asm
.extension "hello"                 ; load hello.mlwsx from a trusted directory
.func main
    ncall "hello.greet" ("world") -> (R0)
    ...
```

### Natives

An `MlwsNativeDesc` gives the native's name, its parameter and result types
(`MLWS_T_*`, the same codes as instruction type suffixes; at most 8 parameters
and 4 results), flags, the C function, a `data` pointer (returned by
`api->native_data` during calls), and a one-line description. The name must
start with the extension's name and a dot (`hello.greet`), so extensions cannot
replace built-in natives or each other's. The verifier checks every `ncall`
against the declared types when the program is loaded, exactly as it does for
built-in natives.

Integers arrive canonical (sign- or zero-extended to 64 bits), floats as
`double`, references as `MlwsValue.r` (NULL for null).

### Rules

* **Values are valid only during the call.** The collector never runs during a
  native call, so nothing needs to be rooted or pinned. A native may hold
  pointers to strings and buffers freely until it returns. To keep a value,
  return it or store it in a VM object that is returned.
* **Fail by recording an error, then return.** `api->error(cx, "E_CODE", fmt,
  ...)` is raised in the caller as an ordinary, catchable `Error` once the native
  returns. Results are then discarded. API functions given the wrong kind of
  value record `E_TYPE` and return NULL or false. Foreign code is never unwound
  with `longjmp`, so it can hold locks and resources safely. Even a heap quota
  is enforced only after it returns.
* **Thread safety.** Natives can be called from several worker threads at once.
  Declare `MLWS_NATIVE_SERIAL` for code that is not thread-safe: the VM then
  holds the extension's lock around every such call. Finalizers always run
  under that lock. One that falls due during a call of its extension (a
  callback dropped or released the handle) runs as soon as that call
  returns.
* **Blocking.** Declare `MLWS_NATIVE_BLOCKING` for anything that may wait: I/O,
  sleeping, waiting for a lock or a network. A blocking native runs on a separate
  pool of I/O threads while its process waits, in the same way as in `recv`.
  Worker threads keep running other processes, so one slow call never stalls
  the program. Undeclared blocking would stall a whole worker thread.
* **Kills wait for foreign code.** A process killed during a blocking native
  stops when the native returns. `alive` reports it dead at once. If the program
  ends while blocking calls are still running, the VM waits up to a second for
  them. A call that still has not returned (say, a read from a terminal) is
  abandoned, and the VM's memory is deliberately not freed under it.

### Foreign handles

C resources (a database connection, a file, a parser) are wrapped in **foreign
handles**:

```c
static void db_close(void *p) { sqlite3_close(p); }
static const MlwsForeignType DB = {"sqlite.db", db_close};   /* defined once */

results[0] = api->foreign_new(cx, &DB, db);          /* open */
sqlite3 *db = api->foreign_get(cx, args[0], &DB);    /* use: NULL (and E_TYPE) if not a sqlite.db */
api->foreign_release(cx, args[0]);                   /* close now */
```

MLWSVM code holds a handle as an ordinary reference. It prints as
`<foreign sqlite.db>`, and `typeof` gives 10. Code cannot see the pointer,
forge one, or pass a handle of one type where another is expected. The
finalizer runs exactly once: when the handle is released, when it is collected,
or when its process ends. A released handle is refused by `foreign_get`, so a
use-after-close becomes an error rather than a crash. Handles cannot be sent to
another process (`E_TYPE`), because the resource belongs to its process.

### API summary

| Function | |
|---|---|
| `define`, `init_error` | During init only: define a native; report why init failed |
| `error` | Fail the current call |
| `native_data`, `self_pid`, `kind` | The native's data pointer, the caller's pid, a reference's kind |
| `str_data`, `str_new`, `bytes_new`, `bytes_data` | Strings and byte buffers (`bytes_data` is mutable) |
| `list_new`, `list_len`, `list_get`, `list_push` | Lists; each element keeps its type |
| `map_new`, `map_get`, `map_set` | Maps, with typed keys and values |
| `foreign_new`, `foreign_get`, `foreign_release` | Foreign handles |
| `host`, `post_int`, `post_str`, `hold` | Messages from any thread (below) |

### Messages from other threads

A library with threads or an event loop of its own can deliver events as
messages. `api->host(cx)`, called during a call, returns the VM. From then on,
any thread can `post_int(host, pid, n)` or `post_str(host, pid, s, len)` to
send a process a message. This is Erlang's `enif_send`. A post to a process
that has ended returns false.

```c
t->host = api->host(cx);
api->hold(t->host, +1);                        /* before the thread starts posting */
pthread_create(&t->thread, NULL, worker, t);   /* worker: api->post_int(t->host, pid, n), ... */
                                               /* and api->hold(t->host, -1) when it is done */
```

While a hold is taken, a process waiting in `recv` is not reported as
deadlocked, since messages may still arrive. The host stays valid until the VM
is freed, so join posting threads before the program can end (`demo.ticker`
and `demo.ticker_join` in `extensions/demo/demo.c` show how).

The table is versioned: `MLWSVM_API_VERSION` and `api->size`. A later version
only appends functions. An extension built for a different API version exports a
differently named init function (`mlwsvm_extension_init_v1`), so a mismatch is
reported at load time and never crashes.

---

## The declarative FFI

`.extern` makes a C function callable as a native, with no C code:

```asm
.extern m.pow,    "libm.so.6|libm", "pow"    (f64, f64) -> (f64)
.extern m.frexp,  "libm.so.6|libm", "frexp"  (f64, out i32) -> (f64)
.extern c.strlen, "",               "strlen" (cstr) -> (u64)
.extern c.malloc, "",               "malloc" (u64) -> (ptr<mem>)
.extern c.free,   "",               "free"   (ptr<mem>)
.extern c.usleep, "",               "usleep" (u32) -> (i32), blocking

    ncall "m.pow" (2.0, 10.0) -> (F0)          ; 1024
    ncall "m.frexp" (8.0) -> (F1, I0)          ; 0.5 and 4: out parameters are extra results
```

`.extern NAME, "LIBRARY", "SYMBOL" (PARAMS) -> (RESULT) [, blocking] [, serial]`

`mlwsvm bindgen header.h` writes these declarations for a whole C header
([TOOLING.md](TOOLING.md#bindings-generator-mlwsvm-bindgen)).

* **NAME** is the native's name. It needs a prefix (`m.pow`). Another module may
  declare the same name only with an identical declaration.
* **LIBRARY** names a shared library. Write alternatives separated by `|`, which
  are tried in order; each is tried as written and with the platform's suffix
  (`libm` → `libm.dylib` or `libm.so`). `""` means the program's own symbols,
  which include the C library.
* **blocking** and **serial** mean the same as the extension flags above.

| Type | MLWSVM side | C side |
|---|---|---|
| `i8` … `i64`, `u8` … `u64` | an integer | that C integer type |
| `f32`, `f64` | a float | `float`, `double` |
| `cstr` | a string (its bytes, NUL-terminated) or a byte buffer (copied and NUL-terminated); as a result, the C string is copied into a new string; NULL is null | `const char *` |
| `buf` | a byte buffer, passed as its own bytes, which C may change | `void *` |
| `ptr`, `ptr<tag>` | an opaque foreign handle; as a result, a new handle (freed by the tag's `.ffi_free` function, if it has one); NULL is null | `void *` |
| `callback NAME` | a closure, or an async callback handle (below) | a function pointer |
| `struct NAME`, `struct* NAME` | a `.cstruct` (below): by value, or a pointer to a copy | `struct NAME`, `struct NAME *` |
| `out T` | no argument: C gets a pointer to a temporary, whose value becomes an extra result (numbers and `ptr`) | `T *` |

Tagged pointers keep different kinds of pointer apart: a `ptr<mem>` result can
only be passed back as a `ptr<mem>`, so a `sqlite3 *` can never reach `free`.
Pointers passed to C are valid only during the call: C must not keep them.

### Handles that free themselves

```asm
.ffi_free db, "libsqlite3", "sqlite3_close"
.extern sql.open, "libsqlite3", "sqlite3_open" (cstr, out ptr<db>) -> (i32)
```

`.ffi_free TAG, "LIBRARY", "SYMBOL"` gives every `ptr<TAG>` handle a finalizer.
The C function is called with the pointer, once: when the handle is garbage
collected, or earlier with `ncall "foreign.release" (handle)`. After that the
handle is released, and passing it to C fails with `E_TYPE` instead of passing
a dangling pointer. The function must take the pointer as its only argument
(its result is ignored). Declare it only once per tag: another `.ffi_free` for
the same tag must name the same function.

Don't also declare the free function as an `.extern` and call it yourself:
the handle would still free the pointer again later. Use `foreign.release`.

### C structs

```asm
.cstruct Tm                      ; struct tm, fields in C order
    tm_sec: i32
    ...
    tm_gmtoff: i64
    tm_zone: cstr
.ends
.extern c.timegm,   "", "timegm"   (struct* Tm) -> (i64)
.extern c.gmtime_r, "", "gmtime_r" (struct* TimeT, out struct Tm) -> (ptr)
.extern c.div,      "", "div"      (i32, i32) -> (struct DivT)

    new R0, Tm                   ; an ordinary MLWSVM struct
    mov R0->Tm.tm_mday, 32
    ncall "c.timegm" (R0) -> (I0) ; C normalizes it, and R0 sees the changes
```

`.cstruct NAME` … `.ends` describes a C struct, and also declares an MLWSVM
struct of the same name to build and read it with. Fields are laid out exactly
as a C compiler would: each aligned to its own size, and the whole padded to its
largest alignment. Field types:

| Field | In the MLWSVM struct | In C |
|---|---|---|
| `i8` … `u64`, `f32`, `f64` | that type | that type |
| `cstr` | a string, or null | `const char *` (as for parameters) |
| `ptr`, `ptr<tag>` | a foreign handle, or null | `void *` |
| `struct NAME` | a NAME struct, or null (zeros) | the struct itself, nested |
| `u8[N]`, `i8[N]` | a byte buffer | `uint8_t x[N]` |
| other `T[N]` | a list of numbers | `T x[N]` |

An array shorter than N is padded with zeros, and a longer one is truncated.
Read back from C, arrays are always exactly N long. A nested `struct` must be
declared first.

A struct crosses into C in one of three ways:

* `struct NAME`: **by value**, as C passes structs (in registers or on the
  stack, as the platform's ABI says). A result may also be a `struct NAME`.
* `struct* NAME`: **a pointer** to a copy that lives for the call. Whatever C
  writes there is copied back into the MLWSVM struct afterwards. Fields that C
  left alone keep their objects; a `cstr` or `ptr` that C changed becomes a new
  string or handle. `null` passes NULL.
* `out struct NAME`: no argument. C fills a zeroed struct, which becomes an
  extra result.

### Variadic functions

```asm
.extern c.snprintf, "", "snprintf" (buf, u64, cstr, ..., i64, f64, cstr) -> (i32)
```

`...` marks where the fixed parameters end. The types after it are the ones this
declaration passes in the variadic part. To pass different ones, declare the
function again under another name. C promotes small integers to `int` and
`float` to `double` in a variadic call, so `i8`, `u8`, `i16`, `u16` and `f32`
are refused after `...` (write `i32` or `f64`), as are `out` parameters and
structs by value.

### Callbacks

```asm
.callback cmp_t (ptr, ptr) -> (i32)            ; a C function-pointer type
.extern c.qsort, "", "qsort" (buf, u64, u64, callback cmp_t)

.func compare(ref a, ref b) -> i32             ; parameters match the callback's
    ...
.endf
    closure compare () -> (R1)
    ncall "c.qsort" (R0, I0, 4, R1)            ; C calls compare while qsort runs
```

`.callback NAME (PARAMS) -> (RESULT)` declares a function-pointer type, and an
`.extern` parameter `callback NAME` takes a closure for it. For the duration of
the call, the closure becomes a real C function pointer (a libffi closure). Each
time C calls it, the closure runs in the calling process, on the same thread, on
top of the frames that made the foreign call, and its result goes back to C.
When the call returns, the pointer is freed: C must not keep it.

* **Types.** Parameters are numbers, `cstr` (copied into a new string) and
  `ptr<tag>` (a borrowed handle, see below). The result is a number, a `ptr`, or
  nothing. The closure's parameter types must be of the same kind (integer,
  float, reference), or the call fails with `E_TYPE`.
* **Exceptions stop at the boundary.** An exception the closure catches is
  ordinary. One it does not catch never unwinds C's frames. Instead, that call
  returns 0 to C, later calls of the same pointer return 0 without running, and
  the exception is raised when the foreign call returns, exactly as it was
  thrown.
* **No waiting.** C is waiting for the callback, so the callback cannot wait in
  turn: `recv` with no message, `sleep`, and natives that would wait (network
  and pipe I/O, `proc.call`) trap `E_CALLBACK` inside it, as does `halt`
  (`yield` does nothing there). Everything else
  works: allocation and garbage collection, other foreign calls (including ones
  that call back again), blocking natives (they run right there), and spawning
  and sending. The debugger does not stop inside callbacks.
* **Threads.** A callback runs only on the thread making the foreign call. If C
  calls it from another thread, the callback does not run, C gets 0, and the
  foreign call fails with `E_CALLBACK`. Use an async callback for that. A
  `blocking` extern's callbacks run on its I/O thread, which is correct.
* **Safety.** The objects passed to the call are kept alive until it returns,
  even if the callback drops every other reference to them. A `serial`
  library's callback cannot call back into that same library (`E_CALLBACK`,
  where it would otherwise deadlock).

**Borrowed pointers.** A pointer that C passes to a callback, or that is read
from a struct field, still belongs to C. Its handle is *borrowed*: the tag's
`.ffi_free` function is never called on it. Only function results and `out`
parameters make handles that own their pointer.

### Async callbacks

```asm
.callback start_t (u64), async
.extern c.pthread_create, "", "pthread_create" (out u64, ptr, callback start_t, u64) -> (i32)

    ncall "ffi.async_callback" ("start_t", I0) -> (R0)   ; calls become messages to process I0
    ncall "c.pthread_create" (null, R0, 42) -> (I1, I2)
    recv R1                       ; Callback{name: "start_t", args: [42]}
```

Event loops, timers and libraries with threads of their own call back later,
from any thread, when the process may be busy or running elsewhere. Running
MLWSVM code there would break process isolation, so an `async` callback type
turns each call into a **message** instead: a built-in struct
`Callback{name, args}`, with the arguments as a list. The C call returns at
once. So an async callback returns nothing, and its parameters are numbers and
`cstr`.

`ffi.async_callback(type, pid)` makes the function pointer as a foreign handle,
and the `callback` parameter takes that handle. The handle owns the pointer:
keep it for as long as C may call it, and make sure C has stopped before
releasing it (`foreign.release`) or letting it be collected. While one exists,
waiting in `recv` is not reported as a deadlock.


---

## Security

Native code is the one way around MLWSVM's isolation, so loading it is a
deliberate, checked act:

1. **Only trusted directories.** Extensions are only loaded from the default
   extension directory (`lib/ext` next to the `mlwsvm` executable), and from
   directories named with `--ext-path DIR` or `MLWSVM_EXT_PATH`, never from a
   program's own directory. So a downloaded program cannot bring native code
   with it.
2. **Installed, and pinned by hash.** Each trusted directory has a `TRUSTED`
   manifest. `mlwsvm ext install x.mlwsx` copies the file in and records its
   SHA-256. At load time the file must match: one changed afterwards, or a
   different file put in its place, is refused (`E_LINK_UNTRUSTED`). Installing
   never runs the extension's code.
3. **Safe permissions.** The directory, its manifest and each extension must not
   be writable by other users, and must be owned by the current user or the
   administrator; this is the check SSH makes of `~/.ssh`. Otherwise nothing in
   the directory is trusted.
4. **FFI libraries by name.** `.extern` may only load libraries that a trusted
   manifest allows: `mlwsvm ext allow-ffi libm`, or `-` for the program's own
   symbols.
5. **Sandboxes.** Quota kind 4 counts a process's foreign calls. `quota 4, 0`
   (or `--quota foreign=0`) shuts native code out completely, and children
   inherit the limit, like the other quotas.
6. **Development.** `--unsafe-native` (or `MLWSVM_UNSAFE_NATIVE=1`) skips
   checks 1–4, with a warning. Do not use it to run code you don't trust.

What this cannot protect against is a trusted library that is itself buggy or
malicious: trusting it means trusting it completely. Windows checks only that
the file exists for now: ACL checks are still to do.

### `mlwsvm ext`

```
mlwsvm ext list      [--dir D]              show what is trusted, and check each hash
mlwsvm ext install   [--dir D] FILE.mlwsx   copy in and pin by SHA-256
mlwsvm ext remove    [--dir D] NAME
mlwsvm ext allow-ffi [--dir D] LIBRARY      ("-": the program's own symbols)
mlwsvm ext deny-ffi  [--dir D] LIBRARY
```

`--dir` defaults to `lib/ext` next to `mlwsvm`.

### Errors

| Code | When |
|---|---|
| `E_LINK_EXT` | An extension is not installed, does not load, has no init function for this API version, or its init failed |
| `E_LINK_UNTRUSTED` | An extension or FFI library fails a trust check; the message says which |
| `E_LINK_FFI` | `.extern`, `.ffi_free`, `.cstruct` or `.callback`: the library cannot be loaded, the symbol is missing, a `.cstruct` or `.callback` is missing or declared differently elsewhere, libffi cannot make the call, or MLWSVM was built without libffi |
| `E_QUOTA` | The foreign-call quota is used up |
| `E_TYPE` | A value of the wrong kind reached foreign code (including a released handle, or a closure that does not match its callback type), or a handle was sent to another process |
| `E_CALLBACK` | A callback tried to wait or `halt`, C called it from another thread, or it called back into its own `serial` library |
| `E_NATIVE` | The default code for an error recorded by an extension without one |
