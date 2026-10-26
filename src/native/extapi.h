/* extapi.h - internal entry points of extapi.c that use the public extension
 * API's types (include/mlwsvm/mlwsvm.h). */
#ifndef MLWSVM_EXTAPI_H
#define MLWSVM_EXTAPI_H

#include "../../include/mlwsvm/mlwsvm.h"
#include "../vm/vm.h"

/* Check and register a native: an extension's (foreign, counted by quota 4)
 * or the embedding host's. On failure, says why. */
bool native_define(VM *vm, ExtLib *lib, const MlwsNativeDesc *d, bool foreign, char *why, size_t whylen);
/* The table of VM functions natives use. */
const MlwsApi *native_api(void);

#endif
