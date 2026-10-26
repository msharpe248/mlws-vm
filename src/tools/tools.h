/* tools.h - command-line tools built into the mlwsvm binary. */
#ifndef MLWSVM_TOOLS_H
#define MLWSVM_TOOLS_H

#include "../vm/vm.h"

/* Library search path shared by every command: -L dirs, then MLWSVM_PATH, then
 * the lib/ directory next to the executable. */
typedef struct {
    VEC(char *) libpaths;
} ToolEnv;

void tool_env_libpaths(const ToolEnv *env, VM *vm);

int tool_test(int argc, char **argv, const ToolEnv *env);
int tool_doc(int argc, char **argv);

#endif
