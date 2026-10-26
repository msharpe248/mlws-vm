/* module.h - in-memory form of an MLWSVM module (.mlwso), as produced by the
 * assembler or read from disk, before linking. See docs/BINARY_FORMAT.md. */
#ifndef MLWSVM_MODULE_H
#define MLWSVM_MODULE_H

#include "types.h"

#define MLWSO_MAGIC "MLWS"
#define MLWSO_VERSION 5

/* A call has a target and an argc operand, leaving 30 for arguments/results. */
#define MAX_PARAMS 30
#define MAX_PARAMS_STORAGE 32

enum { CONST_STRING = 1, CONST_BIG = 2, CONST_DEC = 3 }; /* numbers are stored as decimal text */

typedef struct {
    uint8_t kind;
    uint32_t len;
    char *data;
} MConst;

/* An exception handler region: instructions [start, end) are protected by the
 * handler at `target`. Indices are instruction numbers within the function. */
typedef struct {
    uint32_t start, end, target;
} MHandler;

/* Function flags */
#define MF_EXPORT 0x01
#define MF_IMPORT 0x02       /* entry refers to a function in another module */
#define MF_RETS_DECLARED 0x04

typedef struct {
    char *name;            /* local name, or symbol name in the imported module */
    uint32_t flags;
    uint32_t import_idx;   /* MF_IMPORT: index into Module.imports */
    uint8_t nparams, nrets;
    uint8_t params[MAX_PARAMS_STORAGE];
    uint8_t rets[MAX_PARAMS_STORAGE];
    uint16_t nregs[NBANKS];
    uint32_t code_off, code_len; /* byte range within Module.code */
    uint32_t ninsns;
    uint32_t *lines;       /* source line per instruction (ninsns entries) */
    uint32_t nhandlers;
    MHandler *handlers;
} MFunc;

/* Data symbol flags */
#define MS_EXPORT 0x01

typedef struct {
    char *name;
    uint32_t offset, size; /* byte range within the module's global segment */
    uint32_t flags;
} MSym;

enum { GREF_LOCAL = 0, GREF_IMPORT = 1 };

/* A global reference used by @name operands. */
typedef struct {
    uint8_t kind;
    uint32_t offset;     /* GREF_LOCAL: byte offset into own globals */
    uint32_t import_idx; /* GREF_IMPORT: module, and exported data symbol name */
    char *sym;
} MGref;

/* Struct type flags */
#define MSF_EXPORT 0x01
#define MSF_IMPORT 0x02
#define MSF_BUILTIN 0x04 /* provided by the VM (Error); name only */

/* A struct type: defined here (fields listed), or imported by name. */
typedef struct {
    char *name;
    uint32_t flags;
    uint32_t import_idx; /* MSF_IMPORT */
    uint32_t nfields;
    char **fields;
    uint8_t *types;      /* MlwsType per field */
} MStruct;

/* A field reference used by R->Type.field operands, resolved by name at link time. */
typedef struct {
    uint32_t struct_idx;
    char *field;
} MFieldRef;

/* FFI parameter and result kinds (`.extern`). FFI_OUT marks an "out"
 * parameter: no argument is passed; the callee fills a temporary whose value
 * becomes an extra result. */
enum {
    FFI_VOID = 0,
    FFI_I8, FFI_I16, FFI_I32, FFI_I64, FFI_U8, FFI_U16, FFI_U32, FFI_U64,
    FFI_F32, FFI_F64,
    FFI_CSTR, /* const char *: a string or byte buffer (copied, NUL-terminated); returned: copied into a string */
    FFI_BUF,  /* void *: a byte buffer's own bytes (the callee may write them) */
    FFI_PTR,  /* void *: an opaque foreign handle, optionally tagged ptr<tag> */
    FFI_STRUCT,  /* a .cstruct by value (tag: its name); with FFI_OUT, returned through a pointer */
    FFI_STRUCTP, /* a pointer to a .cstruct: copied in, and copied back after the call */
    FFI__COUNT
};
#define FFI_OUT 0x80
#define FFI_MAX_ARGS 8

/* `.extern` flags (as MLWS_NATIVE_* in include/mlwsvm/mlwsvm.h) */
#define MX_BLOCKING 0x1
#define MX_SERIAL 0x2

/* `.extern name, "library", "symbol" (params) -> (result), flags`: a C
 * function made callable as the native `name`. */
typedef struct MExtern {
    char *name, *lib, *sym;
    uint32_t flags;
    uint8_t nfixed;             /* parameters before `...` (= nargs if not variadic) */
    uint8_t nargs;
    uint8_t args[FFI_MAX_ARGS]; /* FFI_* | FFI_OUT */
    char *tags[FFI_MAX_ARGS];   /* ptr<tag>, or NULL */
    uint8_t ret;                /* FFI_* (FFI_VOID: none) */
    char *ret_tag;
} MExtern;

/* `.cstruct NAME`: a C struct's fields, in order. A struct of the same name
 * is defined in the module's struct table, for MLWSVM code to use. */
typedef struct MCStruct {
    char *name;
    uint32_t nfields;
    char **fields;
    uint8_t *kinds;   /* FFI_* (not OUT/STRUCTP) */
    char **tags;      /* ptr<tag> / nested struct name, or NULL */
    uint32_t *counts; /* > 0: a fixed array of that many */
} MCStruct;

/* `.ffi_free tag, "library", "symbol"`: handles tagged ptr<tag> are freed by
 * calling symbol(ptr) when they are collected. */
typedef struct MFfiFree {
    char *tag, *lib, *sym;
} MFfiFree;

typedef struct Module {
    char *name;   /* dotted module name, e.g. "std.io" */
    char *source; /* source file the module was assembled from (debug) */
    char **imports;
    uint32_t nimports;
    MConst *consts;
    uint32_t nconsts;
    MFunc *funcs;
    uint32_t nfuncs;
    MSym *syms;
    uint32_t nsyms;
    MGref *grefs;
    uint32_t ngrefs;
    char **natives;
    uint32_t nnatives;
    MStruct *structs;
    uint32_t nstructs;
    MFieldRef *frefs;
    uint32_t nfrefs;
    uint8_t *gdata;  /* initial global segment image */
    uint8_t *gtags;  /* one MlwsType tag per 8-byte word */
    uint32_t gsize;  /* bytes, multiple of 8 */
    uint8_t *code;
    uint32_t codelen;
    char **extensions; /* `.extension "name"`: native extensions this module needs */
    uint32_t nextensions;
    MExtern *externs;
    uint32_t nexterns;
    MCStruct *cstructs;
    uint32_t ncstructs;
    MFfiFree *ffrees;
    uint32_t nffrees;
} Module;

void module_free(Module *m);
/* Index of an exported function/data symbol by name, or -1. */
int module_find_export_func(const Module *m, const char *name);
int module_find_export_sym(const Module *m, const char *name);
int module_find_export_struct(const Module *m, const char *name);

/* Serialisation (module_io.c). */
#include "bytes.h"
void module_write(const Module *m, ByteBuf *out);
Module *module_read(const uint8_t *data, size_t len, MlwsError *err);

#endif
