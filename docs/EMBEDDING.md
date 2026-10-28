# Embedding MLWSVM

MLWSVM is also a library: an application can run MLWSVM code inside itself,
call it, and let it call back. Think of it as a scripting engine, plugin host
or rules engine, with isolated processes, quotas and a sandbox included. The
API is [`include/mlwsvm/embed.h`](../include/mlwsvm/embed.h).
[`examples/embed/host.c`](../examples/embed/host.c) is a complete,
self-checking example.

## Building and linking

```sh
make lib            # build/libmlwsvm.a, and build/libmlwsvm.so or .dylib
make check-embed    # build and run examples/embed/host.c
```

Compile with `-I include`, and link with `build/libmlwsvm.a -lm -lpthread`,
plus `-lffi` (if it was found) and `-ldl` on Linux. `make lib` prints the exact
flags. The library has no other dependencies.

## A first program

```c
#include <mlwsvm/embed.h>
#include <stdio.h>

int main(void) {
    MlwsVM *vm = mlws_new();
    mlws_add_path(vm, "lib");   /* where .import finds std.* */
    if (!mlws_load_source(vm, "calc",
            ".module calc\n.export mul\n"
            ".func mul(i64 a, i64 b) -> i64\n    mul I2, a, b\n    ret I2\n.endf\n")) {
        fprintf(stderr, "%s\n", mlws_error(vm));
        return 1;
    }
    MlwsArg in[2] = {MLWS_INT(6), MLWS_INT(7)}, out[1];
    if (mlws_call(vm, "calc", "mul", in, 2, out, 1, NULL)) printf("%lld\n", (long long)out[0].v.i);
    else fprintf(stderr, "%s\n%s", mlws_error(vm), mlws_backtrace(vm));
    mlws_free(vm);
    return 0;
}
```

## The model

* **Load** modules from files (`mlws_load_file`, `.masm` or `.mlwso`) or from
  source held in memory (`mlws_load_source`). A file's imports are looked for
  in its own directory first, then on the library path (`mlws_add_path`);
  source in memory has only the library path
  ([ASSEMBLY.md §8](ASSEMBLY.md#8-modules-and-libraries)). A loaded module
  stays loaded for later calls. `mlws_call` and `mlws_run_main` name a module
  as `.import` would; if files from two directories have the same module name,
  give them different names with `.module`.
* **Call** an exported function with `mlws_call`. It runs as the program's main
  process: it may spawn processes, use files and the network, and call the
  host. The call returns when the function does, and any processes it left
  running are stopped. Each call starts with fresh module globals, so calls
  don't interfere with each other. `mlws_run_main` runs a module's `main`, as
  `mlwsvm run` would.
* **Values** crossing the boundary are numbers (`MLWS_INT`, `MLWS_FLOAT`,
  `MLWS_BOOL`, and any `MLWS_T_*` type) and strings (`MLWS_STR`). Arguments are
  checked against the function's parameters (`E_TYPE`). A result that is a
  string (or byte buffer) comes back as a NUL-terminated copy, valid until the
  next call. Any other reference comes back as its text, so return
  `json.stringify(...)` for structured data.
* **Errors**: functions return `false`, and `mlws_error` gives
  `"E_CODE: message"`, with `mlws_error_code` and `mlws_backtrace` for runtime
  errors. MLWSVM code can never crash the host through a bad pointer: the VM's
  checks apply as usual.

## Calling the host

`mlws_define` registers a host function that MLWSVM code calls with `ncall`.
It's written exactly like a native extension's
([EXTENDING.md](EXTENDING.md)), with the same `MlwsApi` table (`mlws_api()`):
strings, lists, maps, foreign handles, errors. Its name needs a prefix
(`app.lookup`) and must not already be taken by another native; otherwise
`mlws_define` fails with `E_LINK_NATIVE`. Host natives may be declared
`MLWS_NATIVE_BLOCKING` (run on the I/O pool) or `MLWS_NATIVE_SERIAL` (one call
at a time). They are the host's own code, so they do not count towards the
foreign-call quota (kind 4). Define them before loading code that calls them.

## Long-lived services

```c
uint64_t pid;
mlws_start(vm, "app", "service", NULL, 0, &pid);  /* runs on the VM's threads */
mlws_send(vm, pid, &MLWS_STR("job 1"));           /* any host thread, any time */
mlws_send(vm, pid, &MLWS_STR("stop"));
mlws_wait(vm, out, 1, NULL);                      /* its results */
```

`mlws_start` runs a function in the background and returns its pid at once.
`mlws_send` posts a message to any process from any host thread; MLWSVM code
receives it with `recv`. While the host may send, a process waiting in `recv`
is never reported as deadlocked. A send to a process that has ended returns
false. `mlws_wait` waits for the function to return and collects its results.
Only one background run can go at a time: until `mlws_wait`, loading, calling
and starting fail with `E_BUSY` (as does `mlws_wait` with nothing running).
`mlws_free` waits for a background run to finish.

## Threads and limits

* A VM is used from one thread at a time, except for `mlws_send`. Several VMs
  can run in one program, each completely separate.
* Each run uses the VM's worker threads (one per core by default:
  `mlws_set_option(vm, "threads", "4")`). All the VM options of
  [TOOLING.md](TOOLING.md#vm-options) work, including quotas
  (`"quota", "heap=64M,os=0,foreign=0"`) to sandbox untrusted scripts.
* Output goes to stdout unless redirected with `mlws_set_output`.
  `mlws_set_args` sets what `os.args` returns.
* The library's internal symbols are not hidden yet, so its global names (all
  lower-case C names such as `vm_new`) could clash with a host's own. Link it
  statically and watch for duplicate-symbol errors until they are hidden.
