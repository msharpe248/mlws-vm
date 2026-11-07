/* tools.h - command-line tools built into the mlwsvm binary. */
#ifndef MLWSVM_TOOLS_H
#define MLWSVM_TOOLS_H

#include "../vm/vm.h"

/* Library search path shared by every command: -L dirs, then MLWSVM_PATH, then
 * the lib/ directory next to the executable. */
typedef struct {
    VEC(char *) libpaths;
    char *exe;     /* this executable, or NULL */
    char *ext_dir; /* the default trusted extension directory (lib/ext next to mlwsvm) */
    VEC(char *) vmopts; /* debug and dap: VM options from the command line, name and value pairs */
} ToolEnv;

/* Add the library path and the default extension directory to a VM. */
void tool_env_libpaths(const ToolEnv *env, VM *vm);
/* The VM options of `mlwsvm debug` and `dap`: the environment's, except
 * MLWSVM_OPT (the debuggers optimize only when asked), then the command
 * line's. False, with *err, for a bad value. */
bool tool_env_options(const ToolEnv *env, VM *vm, MlwsError *err);
int tool_ext(int argc, char **argv, const ToolEnv *env); /* `mlwsvm ext` */
int tool_lib(int argc, char **argv, const ToolEnv *env); /* `mlwsvm lib` (libtool.c) */
int tool_opt(int argc, char **argv, const ToolEnv *env); /* `mlwsvm opt` (opttool.c) */
int tool_bundle(int argc, char **argv, const ToolEnv *env); /* `mlwsvm bundle` (bundle.c) */
int tool_dap(int argc, char **argv, const ToolEnv *env);    /* `mlwsvm dap` (dap.c) */

int tool_test(int argc, char **argv, const ToolEnv *env);
int tool_doc(int argc, char **argv);
int tool_top(int argc, char **argv);
int tool_bindgen(int argc, char **argv); /* `mlwsvm bindgen` */
int tool_fuzz(int argc, char **argv);    /* `mlwsvm fuzz` (fuzz.c) */

/* --trace / --profile (hooks.c) */
bool tools_trace(VM *vm, const char *path, uint64_t limit);
void tools_profile(VM *vm);
void tools_profile_report(VM *vm, FILE *out, const char *flame_path);

/* --monitor (monitor.c): serve stats over HTTP while the program runs. */
bool tools_monitor(VM *vm, const char *addr, char *err, size_t errlen);
const char *tools_monitor_addr(VM *vm); /* "HOST:PORT" actually listened on */

/* mlwsvm debug (debugger.c) */
typedef struct {
    OutputFn out;
    void *out_ctx;
    bool (*read_line)(void *ctx, char *buf, size_t n); /* false at end of input */
    void *in_ctx;
    bool echo;          /* print the (mdb) prompt */
    bool echo_commands; /* print each command after the prompt (scripts, tests) */
} DebugIO;
int debugger_run(VM *vm, LModule *root, Function *main_fn, DebugIO *io);

#endif
