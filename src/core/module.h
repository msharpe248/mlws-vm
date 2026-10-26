/* module.h - in-memory form of an MLWSVM module (.mlwso), as produced by the
 * assembler or read from disk, before linking. See docs/BINARY_FORMAT.md. */
#ifndef MLWSVM_MODULE_H
#define MLWSVM_MODULE_H

#include "types.h"

#define MLWSO_MAGIC "MLWS"
#define MLWSO_VERSION 1

/* A call has a target and an argc operand, leaving 30 for arguments/results. */
#define MAX_PARAMS 30
#define MAX_PARAMS_STORAGE 32

enum { CONST_STRING = 1 };

typedef struct {
    uint8_t kind;
    uint32_t len;
    char *data;
} MConst;

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
    uint8_t *gdata;  /* initial global segment image */
    uint8_t *gtags;  /* one MlwsType tag per 8-byte word */
    uint32_t gsize;  /* bytes, multiple of 8 */
    uint8_t *code;
    uint32_t codelen;
} Module;

void module_free(Module *m);
/* Index of an exported function/data symbol by name, or -1. */
int module_find_export_func(const Module *m, const char *name);
int module_find_export_sym(const Module *m, const char *name);

/* Serialisation (module_io.c). */
#include "bytes.h"
void module_write(const Module *m, ByteBuf *out);
Module *module_read(const uint8_t *data, size_t len, MlwsError *err);

#endif
