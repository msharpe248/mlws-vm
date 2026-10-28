/* tools.h - command-line tools built into the mlwsvm binary. */
#ifndef MLWSVM_TOOLS_H
#define MLWSVM_TOOLS_H

#include "../vm/vm.h"

/* Library search path shared by every command: -L dirs, then MLWSVM_PATH, then
 * the lib/ directory next to the executable. */
typedef struct {
    VEC(char *) libpaths;
    char *ext_dir; /* the default trusted extension directory (lib/ext next to mlwsvm) */
} ToolEnv;

/* Add the library path and the default extension directory to a VM. */
void tool_env_libpaths(const ToolEnv *env, VM *vm);
int tool_ext(int argc, char **argv, const ToolEnv *env); /* `mlwsvm ext` */
int tool_lib(int argc, char **argv, const ToolEnv *env); /* `mlwsvm lib` (libtool.c) */

int tool_test(int argc, char **argv, const ToolEnv *env);
int tool_doc(int argc, char **argv);
int tool_top(int argc, char **argv);
int tool_bindgen(int argc, char **argv); /* `mlwsvm bindgen` */

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
