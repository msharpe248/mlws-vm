/* embed.h - running MLWSVM inside a C or C++ program (version 1).
 *
 * Build the library with `make lib` (build/libmlwsvm.a, and libmlwsvm.so or
 * libmlwsvm.dylib) and link with it plus -lm, -lpthread, and -lffi and -ldl
 * where the build used them (`make lib` prints the flags). examples/embed/host.c
 * is a complete example; docs/EMBEDDING.md explains the model.
 *
 *     MlwsVM *vm = mlws_new();
 *     mlws_add_path(vm, "/usr/local/lib/mlwsvm");      // for .import std.*
 *     mlws_load_source(vm, "calc", "...assembly...");
 *     MlwsArg in[2] = {MLWS_INT(6), MLWS_INT(7)}, out[1];
 *     if (mlws_call(vm, "calc", "mul", in, 2, out, 1, NULL)) printf("%lld\n", (long long)out[0].v.i);
 *     else fprintf(stderr, "%s\n", mlws_error(vm));
 *     mlws_free(vm);
 *
 * Each mlws_call (and mlws_run_main) runs the function as the program's main
 * process: it may spawn processes, use files and the network, and call the
 * host's own natives (mlws_define); when the function returns, any processes it
 * left running are stopped. For a long-lived service, mlws_start runs a
 * function in the background, mlws_send gives it messages from any host
 * thread, and mlws_wait waits for it to finish.
 *
 * A VM is not itself thread-safe: call it from one thread at a time, except for
 * mlws_send, which any thread may call while a background run is going. */
#ifndef MLWSVM_EMBED_H
#define MLWSVM_EMBED_H

#include "mlwsvm.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MlwsVM MlwsVM;

/* An argument, result or message: a number of the given type, or a string
 * (MLWS_T_R). A string result is NUL-terminated and stays valid until the next
 * call on that VM. A result that is another kind of reference comes back as its
 * text (as `tostr` shows it); return json.stringify(...) for structured data. */
typedef struct {
    uint8_t type; /* MLWS_T_* */
    MlwsValue v;  /* a number */
    const char *str; /* MLWS_T_R: the string, or NULL for null */
    size_t len;
} MlwsArg;

#define MLWS_INT(x) ((MlwsArg){MLWS_T_I64, {.i = (x)}, NULL, 0})
#define MLWS_FLOAT(x) ((MlwsArg){MLWS_T_F64, {.f = (x)}, NULL, 0})
#define MLWS_BOOL(x) ((MlwsArg){MLWS_T_B, {.i = (x) ? 1 : 0}, NULL, 0})
#define MLWS_STR(s) ((MlwsArg){MLWS_T_R, {.i = 0}, (s), (s) ? strlen(s) : 0}) /* needs <string.h> */

MlwsVM *mlws_new(void);
void mlws_free(MlwsVM *vm);

/* The last failure: "E_CODE: message" and its code ("" after a success). A
 * runtime error in MLWSVM code also has a backtrace. */
const char *mlws_error(const MlwsVM *vm);
const char *mlws_error_code(const MlwsVM *vm);
const char *mlws_backtrace(const MlwsVM *vm);

/* Settings: VM options as on the command line (mlws_set_option(vm,
 * "max-frames", "500000"), "threads", "quota", "ext-path", ...), the library
 * path for .import (add the directory holding std/), program output (default:
 * stdout), and what os.args returns. */
bool mlws_set_option(MlwsVM *vm, const char *name, const char *value);
void mlws_add_path(MlwsVM *vm, const char *dir);
void mlws_set_output(MlwsVM *vm, void (*write)(void *ctx, const char *s, size_t n), void *ctx);
void mlws_set_args(MlwsVM *vm, int argc, const char *const *argv);

/* A host function MLWSVM code can call with ncall, written exactly like an
 * extension's native (include/mlwsvm/mlwsvm.h), with the table from mlws_api.
 * Define natives before loading code that calls them. */
bool mlws_define(MlwsVM *vm, const MlwsNativeDesc *desc);
const MlwsApi *mlws_api(void);

/* Load a module and what it imports: from a .masm or .mlwso file (whose
 * imports are looked for in its own directory first, then on the library
 * path), or from assembly source in memory (`module` is its name, as .import
 * would use; its imports come from the library path). */
bool mlws_load_file(MlwsVM *vm, const char *path);
bool mlws_load_source(MlwsVM *vm, const char *module, const char *source);

/* Call an exported function of a loaded module and wait for it to return.
 * Arguments must match its parameters (numbers for I/F parameters, strings
 * for R parameters); up to `nresults` results are stored, and *got (if not
 * NULL) says how many it returned. */
bool mlws_call(MlwsVM *vm, const char *module, const char *function, const MlwsArg *args, int nargs,
               MlwsArg *results, int nresults, int *got);
/* Run a loaded module's main, as `mlwsvm run` does. */
bool mlws_run_main(MlwsVM *vm, const char *module, int64_t *exit_code);

/* Run an exported function in the background (on the VM's own threads): its
 * pid comes back at once. Send it messages with mlws_send (from any thread;
 * numbers or strings, received with recv), and collect its results with
 * mlws_wait. Only one background run at a time; other calls fail meanwhile. */
bool mlws_start(MlwsVM *vm, const char *module, const char *function, const MlwsArg *args, int nargs,
                uint64_t *pid);
bool mlws_send(MlwsVM *vm, uint64_t pid, const MlwsArg *message);
bool mlws_wait(MlwsVM *vm, MlwsArg *results, int nresults, int *got);

#ifdef __cplusplus
}
#endif

#endif
