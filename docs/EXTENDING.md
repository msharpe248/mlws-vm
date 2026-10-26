# Extending MLWSVM: Native Extensions and the FFI

MLWSVM code can call C in two ways. Both appear to MLWSVM code as ordinary
**natives**, called with `ncall "name" (args) -> (results)` exactly like the
built-in ones in [NATIVES.md](NATIVES.md):

| | Extension API | Declarative FFI |
|---|---|---|
| What you write | A small C library against [`include/mlwsvm/mlwsvm.h`](../include/mlwsvm/mlwsvm.h) | One `.extern` line per C function, in assembly |
| Good for | Wrapping a library properly: converting types, owning resources, reporting errors well | Calling existing C functions directly, with no C code at all |
| Values | Strings, bytes, lists, maps and foreign handles, through the API | Numbers, strings, byte buffers and opaque pointers |
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
  under that lock.
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
| `ptr`, `ptr<tag>` | an opaque foreign handle; as a result, a new handle (without a finalizer); NULL is null | `void *` |
| `out T` | no argument: C gets a pointer to a temporary, whose value becomes an extra result (numbers and `ptr`) | `T *` |

Tagged pointers keep different kinds of pointer apart: a `ptr<mem>` result can
only be passed back as a `ptr<mem>`, so a `sqlite3 *` can never reach `free`.
Pointers passed to C are valid only during the call: C must not keep them.

Not supported yet: variadic functions (`printf`), structs passed by value, and
callbacks from C into MLWSVM. Until then, wrap those in an extension. All three
are planned; the designs are in [ROADMAP.md](ROADMAP.md) §1.

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
| `E_LINK_FFI` | `.extern`: the library cannot be loaded, the symbol is missing, or MLWSVM was built without libffi |
| `E_QUOTA` | The foreign-call quota is used up |
| `E_TYPE` | A value of the wrong kind reached foreign code, or a handle was sent to another process |
| `E_NATIVE` | The default code for an error recorded by an extension without one |
