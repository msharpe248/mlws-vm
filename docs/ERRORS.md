# MLWSVM Error Codes

Every error the toolchain reports has a stable code, printed as `error[CODE]` or
`runtime error[CODE]`. Tests match on these codes with `; expect-error: CODE`.

## Assembler (`E_ASM_*`)

Reported as `file:line:col: message`. If the error is inside a macro expansion,
the message also names the macro and where it was expanded.

| Code | Meaning |
|---|---|
| `E_ASM_SYNTAX` | Malformed statement, unbalanced `.if`/`.macro`/`.rept`, a function that can run off its end, a misplaced directive |
| `E_ASM_UNKNOWN_OP` | Unknown instruction or macro name |
| `E_ASM_BAD_OPERAND` | Operand not allowed here (literal destination, wrong register bank, bad base/index register, write to PID, ...) |
| `E_ASM_BAD_TYPE` | Unknown type suffix, or a type the instruction doesn't support |
| `E_ASM_OPERAND_COUNT` | Too few or too many operands |
| `E_ASM_UNDEF_LABEL` | Branch to a label not defined in the function |
| `E_ASM_UNDEF_SYM` | Unknown function, data symbol, constant or alias |
| `E_ASM_DUP_SYM` | Duplicate label, function, symbol, macro or constant |
| `E_ASM_EXPR` | Invalid constant expression (undefined name, division by zero, ...) |
| `E_ASM_RANGE` | Value out of range (`.align`, `.rept` count, scale, ...) |
| `E_ASM_MACRO` | Wrong number of macro arguments, or recursion too deep |
| `E_ASM_INCLUDE` | `.include` file not found or unreadable |
| `E_ASM_USER` | Raised by `.error "message"` |

## Loader and linker (`E_LOAD_*`, `E_LINK_*`, `E_VERIFY`)

| Code | Meaning |
|---|---|
| `E_LOAD_NOT_FOUND` | Program file or imported module not found on the library path |
| `E_LOAD_IO` | File could not be read |
| `E_LOAD_FORMAT` | Malformed or truncated `.mlwso` file, or unknown opcode/mode |
| `E_LOAD_NAME` | A module declares a different name than it was imported as, or the name is already loaded |
| `E_LOAD_LIMIT` | Too many modules |
| `E_LINK_CYCLE` | Import cycle (the message shows the chain) |
| `E_LINK_UNDEF` | Imported function, data symbol or struct type not exported by the module, or a struct field that does not exist |
| `E_LINK_NATIVE` | Unknown native function name in `ncall`, an `.extern` whose name another native already has with a different declaration, or a host native that `mlws_define` refused ([EMBEDDING.md](EMBEDDING.md)) |
| `E_LINK_EXT` | A native extension is not installed, cannot be loaded, or failed to initialise ([EXTENDING.md](EXTENDING.md)) |
| `E_LINK_UNTRUSTED` | Native code failed a trust check: not installed, changed since installation, unsafe permissions, or an FFI library not allowed |
| `E_LINK_FFI` | `.extern`, `.ffi_free`, `.cstruct` or `.callback`: library or symbol not found, a missing or conflicting `.cstruct`, `.callback` or free function, a call libffi cannot make, or built without libffi. Also raised at run time when libffi cannot make a callback closure (`ffi.async_callback`) |
| `E_LINK_NO_MAIN` | The program has no `main`, or `main` takes parameters |
| `E_VERIFY` | Bytecode verification failed: an operand shape, a register outside the window, a branch target, call or native arity or argument kinds, `ret` versus declared results, or code that runs off the end. Reported with source file, line and function |

## VM options

| Code | Meaning |
|---|---|
| `E_BAD_OPTION` | A VM option (`--max-frames`, `--quota`, ...) has a bad name or value. Reported by `mlwsvm run` (exit code 2), by the test runner and by `mlws_set_option`; never raised at run time |

## Embedding API

The embedding API ([EMBEDDING.md](EMBEDDING.md)) reports the codes above (a
module that fails to load, a runtime error in the called function, an argument
of the wrong type: `E_TYPE`), and one of its own:

| Code | Meaning |
|---|---|
| `E_BUSY` | A background run (`mlws_start`) is in progress, so the VM cannot load or run anything else until `mlws_wait`; or `mlws_wait` with nothing running in the background |

## Runtime traps

Every runtime trap is raised as a catchable exception: an `Error` struct whose
`code` field is the code below (see [ASSEMBLY.md §5.3](ASSEMBLY.md#53-exceptions)).
If it is not caught, it is reported with a backtrace and `mlwsvm run` exits with
status 70.

| Code | Meaning |
|---|---|
| `E_DIV_ZERO` | Integer division or remainder by zero |
| `E_NULL` | Dereference of a null pointer, or a null argument to a native |
| `E_BOUNDS` | Memory access outside its block, pointer arithmetic out of range, or an object index out of range (including `lpop` of an empty list and `substr` past the end) |
| `E_BAD_PTR` | Pointer to an unknown segment, or a stack or global pointer with the wrong generation |
| `E_UAF` | Use of a pointer to a freed or reallocated block |
| `E_DOUBLE_FREE` | `free`/`realloc` of an already freed block |
| `E_BAD_FREE` | `free`/`realloc` of something that is not the start of a raw block |
| `E_BAD_ALLOC` | Invalid allocation size or object length, or too many live blocks |
| `E_TYPE` | Type violation: reading a non-reference as a reference (or the reverse); storing a value of the wrong bank in a register, array element, struct field or list slot; a field access on the wrong struct type; an object of the wrong kind for an instruction (e.g. `strcat` of a list); writing to an immutable string |
| `E_OVERFLOW` | A BigInt or decimal does not fit the target of `cvt`, a float that is infinite or NaN was converted to one, or a result such as `math.lcm`'s does not fit an `i64` |
| `E_RANGE` | A native's argument is out of range: a time-zone offset beyond a day, `random.int` with lo > hi, `random.bytes` of a negative count or more than 16 MiB, a SipHash key that is not 16 bytes, `math.powmod` with e < 0 or m <= 0, a time or `Date` outside the years 1678 to 2261 |
| `E_CALLBACK` | Code called back from C (or from `list.sort` and `list.bsearch`) tried to wait (`recv`, `sleep`, waiting I/O, `proc.call`) or `halt`, or was stopped before it returned (killed, or `os.exit`), or C called a callback from another thread, or re-entered a `serial` library ([EXTENDING.md](EXTENDING.md#callbacks)) |
| `E_KEY` | `mget` without a default for a key that is not in the map |
| `E_PARSE` | `parse` input is not a valid number of the requested type, or malformed text for `time.parse`, `hex.decode`, `base64.decode`, `url.decode` or `csv.parse` |
| `E_BAD_HANDLE` | `hget`/`hfree` of an unknown or freed handle |
| `E_STACK_OVERFLOW` | Data stack exhausted |
| `E_STACK_UNDERFLOW` | Access above the top of the data stack (popping an empty stack) |
| `E_CALL_DEPTH` | More than the maximum number of nested calls |
| `E_RET_COUNT` | A caller expects more results than the callee returned |
| `E_ASSERT` | `assert` failed, or `test.fail` was called (as the `std/test.inc` macros do) |
| `E_UNCAUGHT` | An exception that is not an `Error` struct (e.g. `throw "text"`) was not caught |
| `E_DEADLOCK` | `recv` (or `proc.call`) in `main` while every process is waiting, so no message can ever arrive; or `proc.call` of the calling process itself |
| `E_QUOTA` | A heap, raw-memory, mailbox, foreign-call or OS-call quota was exceeded (catchable), or the instruction quota (the process is stopped); also `quota` with an unknown kind or a negative limit |
| `E_NOT_FOUND`, `E_EXISTS`, `E_PERMISSION`, `E_IS_DIR`, `E_NOT_DIR`, `E_CLOSED`, `E_TIMEOUT`, `E_RESET`, `E_REFUSED`, `E_ADDR_IN_USE`, `E_RESOURCES`, `E_IO` | Files, commands, sockets and other OS calls ([STDLIB.md](STDLIB.md#errors)). Also `E_EXISTS` for `proc.register` of a name a live process holds, and `E_TIMEOUT` for a `proc.call` with no reply in time |
| `E_RESTARTS` | A supervisor (`std.supervisor`) gave up: more failures than its restart limit within its period |
| `E_JSON` | `json.parse` of malformed JSON (the message gives line and column) or of a document that is not an object or array, or `json.stringify` of a value with no JSON form |
| `E_NATIVE` | An extension reported an error without giving a code (extensions normally use their own codes, such as `E_DEMO`) |
| `E_KILLED` | The process was killed with `kill` |
| `E_LINKED` | A process linked to this one failed |
| `E_NOPROC` | `link`, `proc.register` or `proc.call` of a process that does not exist or has finished. It is also the `code` of the `Down` that `monitor` of such a process delivers |
| `E_INTERNAL` | A VM bug. Please report it |
