/* asm.h - MLWSVM assembler entry points. */
#ifndef MLWSVM_ASM_H
#define MLWSVM_ASM_H

#include "../core/module.h"

/* Assemble a .masm file into a Module. `default_name` is used when the source
 * has no `.module` directive. Include files are searched in the including
 * file's directory and then in libpaths. */
Module *assemble_file(const char *path, const char *default_name, const char *const *libpaths, size_t nlib,
                      MlwsError *err);
/* Run only the preprocessor and write the expanded source (for `mlwsvm asm -E`). */
bool preprocess_file(const char *path, const char *const *libpaths, size_t nlib, StrBuf *out, MlwsError *err);

#endif
