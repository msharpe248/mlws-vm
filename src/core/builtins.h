/* builtins.h - struct types provided by the VM in every module without a
 * declaration. The assembler uses this table to check field names and infer
 * types; the loader binds MSF_BUILTIN struct entries to the VM's definitions. */
#ifndef MLWSVM_BUILTINS_H
#define MLWSVM_BUILTINS_H

#include "types.h"

typedef struct {
    const char *name;
    uint32_t nfields;
    const char *const *fields;
    const uint8_t *types;
} BuiltinStruct;

/* Error: raised for every runtime fault.  Down: a monitor notification.
 * FileInfo: what fs.stat reports. */
enum { BUILTIN_ERROR = 0, BUILTIN_DOWN = 1, BUILTIN_FILEINFO = 2, BUILTIN_CALL = 3, BUILTIN__COUNT };
enum { ERRF_CODE = 0, ERRF_MESSAGE = 1, ERRF_TRACE = 2 };
enum { DOWNF_PID = 0, DOWNF_EXIT = 1, DOWNF_CODE = 2, DOWNF_MESSAGE = 3 };
enum { FIF_SIZE = 0, FIF_MTIME = 1, FIF_MODE = 2, FIF_KIND = 3 };
/* Call: a request from proc.call, answered with proc.reply. */
enum { CALLF_FROM = 0, CALLF_ID = 1, CALLF_BODY = 2 };

extern const BuiltinStruct BUILTIN_STRUCTS[BUILTIN__COUNT];

/* Index into BUILTIN_STRUCTS, or -1. */
int builtin_struct_find(const char *name);

#endif
