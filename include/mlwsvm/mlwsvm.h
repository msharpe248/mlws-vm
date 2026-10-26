/* mlwsvm.h - the MLWSVM native extension API, version 1.
 *
 * An extension is a shared library (file suffix .mlwsx) that defines native
 * functions, which MLWSVM code calls with `ncall "name" (args) -> (results)`.
 * It exports one function, MLWSVM_EXTENSION_INIT, which the VM calls when a
 * module that declares `.extension "name"` is loaded. The VM passes a table of
 * functions (MlwsApi); an extension needs nothing else from the VM, and does not
 * link against it. This header has no dependencies beyond the C standard library.
 *
 * Rules for natives (docs/EXTENDING.md explains them):
 *
 *  - Values passed to a native, and values it gets from the API, are valid only
 *    until it returns. The collector never runs during a native call, so
 *    nothing needs to be "rooted". To keep a value, return it or store it in a
 *    VM object that is returned.
 *  - A native never unwinds the VM. To fail, call api->error(...) and return;
 *    the error is raised in the calling process (as a catchable Error) once the
 *    native has returned.
 *  - Natives may be called from several threads at once. Declare
 *    MLWS_NATIVE_SERIAL for code that is not thread-safe: the VM then holds this
 *    extension's lock around every call (and every finalizer).
 *  - Natives that may block (I/O, waiting) must declare MLWS_NATIVE_BLOCKING.
 *    They run on a separate pool of threads while the calling process waits, so
 *    worker threads stay free for other processes.
 *  - C pointers never enter MLWSVM code directly. Wrap a resource in a foreign
 *    handle (api->foreign_new) with a finalizer; MLWSVM code holds the handle as
 *    an ordinary reference and cannot forge or inspect it.
 */
#ifndef MLWSVM_H
#define MLWSVM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MLWSVM_API_VERSION 1

/* The function each extension must export, with C linkage. */
#define MLWSVM_EXTENSION_INIT mlwsvm_extension_init_v1

#ifdef _WIN32
#define MLWSVM_EXPORT __declspec(dllexport)
#else
#define MLWSVM_EXPORT __attribute__((visibility("default")))
#endif

/* One argument or result: the same 8-byte word the VM uses. `r` is a reference
 * to a VM object (or NULL for null). */
typedef union {
    int64_t i;
    uint64_t u;
    double f;
    void *r;
} MlwsValue;

/* Value types, the same codes as instruction type suffixes. Integers arrive
 * canonical (sign- or zero-extended to 64 bits). */
enum {
    MLWS_T_B = 1, /* bool */
    MLWS_T_C = 2, /* char (Unicode scalar value) */
    MLWS_T_I8 = 3,
    MLWS_T_I16 = 4,
    MLWS_T_I32 = 5,
    MLWS_T_I64 = 6,
    MLWS_T_U8 = 7,
    MLWS_T_U16 = 8,
    MLWS_T_U32 = 9,
    MLWS_T_U64 = 10,
    MLWS_T_F32 = 11,
    MLWS_T_F64 = 12,
    MLWS_T_R = 13, /* a reference */
};

/* Reference kinds (api->kind), the same as the `typeof` instruction's. */
enum {
    MLWS_KIND_NULL = 0,
    MLWS_KIND_STRING = 1,
    MLWS_KIND_BYTES = 2,
    MLWS_KIND_ARRAY = 3,
    MLWS_KIND_STRUCT = 4,
    MLWS_KIND_LIST = 5,
    MLWS_KIND_MAP = 6,
    MLWS_KIND_CLOSURE = 7,
    MLWS_KIND_BIGINT = 8,
    MLWS_KIND_DECIMAL = 9,
    MLWS_KIND_FOREIGN = 10,
};

/* Native flags. */
#define MLWS_NATIVE_BLOCKING 0x1u /* may block: run on the I/O thread pool */
#define MLWS_NATIVE_SERIAL 0x2u   /* not thread-safe: one call at a time per extension */

#define MLWS_MAX_PARAMS 8
#define MLWS_MAX_RESULTS 4

typedef struct MlwsCtx MlwsCtx;             /* the calling process, during a call */
typedef struct MlwsExtension MlwsExtension; /* the extension, during init */
typedef struct MlwsHost MlwsHost;           /* the VM, for messages from any thread */

typedef void (*MlwsNativeFn)(MlwsCtx *cx, const MlwsValue *args, MlwsValue *results);

typedef struct {
    const char *name;                 /* "ext.function": must start with the extension's name and a dot */
    uint8_t nparams;
    uint8_t params[MLWS_MAX_PARAMS];  /* MLWS_T_* */
    uint8_t nresults;
    uint8_t results[MLWS_MAX_RESULTS];
    uint32_t flags;                   /* MLWS_NATIVE_* */
    MlwsNativeFn fn;
    void *data;                       /* returned by api->native_data during a call */
    const char *doc;                  /* one line, for `mlwsvm doc natives` */
} MlwsNativeDesc;

/* A kind of foreign resource. Handles are typed: foreign_get only returns the
 * pointer for a handle made with the same MlwsForeignType (compared by address,
 * so define each one once, statically). */
typedef struct {
    const char *name;           /* shown when the handle is printed: <foreign NAME> */
    void (*finalize)(void *ptr); /* called once: by foreign_release, or when the handle is collected */
} MlwsForeignType;

typedef struct MlwsApi {
    uint32_t version; /* the VM's MLWSVM_API_VERSION */
    uint32_t size;    /* sizeof(MlwsApi) in the VM: later versions only append */

    /* ---- during init only ---- */
    bool (*define)(MlwsExtension *ext, const MlwsNativeDesc *desc);
    void (*init_error)(MlwsExtension *ext, const char *fmt, ...);

    /* ---- during a call ---- */
    /* Fail the call: raise Error{code, message} in the caller when the native
     * returns. The first error recorded wins; results are discarded. */
    void (*error)(MlwsCtx *cx, const char *code, const char *fmt, ...);
    void *(*native_data)(MlwsCtx *cx);
    uint64_t (*self_pid)(MlwsCtx *cx);
    int (*kind)(MlwsCtx *cx, MlwsValue ref); /* MLWS_KIND_* */

    /* Strings and byte buffers. str_data accepts either; a string's bytes are
     * NUL-terminated. Returns NULL (and records E_TYPE) for anything else. */
    const char *(*str_data)(MlwsCtx *cx, MlwsValue ref, size_t *len);
    MlwsValue (*str_new)(MlwsCtx *cx, const char *s, size_t len);
    MlwsValue (*bytes_new)(MlwsCtx *cx, const void *data, size_t len);
    uint8_t *(*bytes_data)(MlwsCtx *cx, MlwsValue ref, size_t *len); /* mutable */

    /* Lists and maps. Each element keeps its type (MLWS_T_*). The getters
     * return false if the index or key is absent; all record E_TYPE for a
     * reference of the wrong kind. */
    MlwsValue (*list_new)(MlwsCtx *cx);
    int64_t (*list_len)(MlwsCtx *cx, MlwsValue list);
    bool (*list_get)(MlwsCtx *cx, MlwsValue list, int64_t index, MlwsValue *value, uint8_t *type);
    bool (*list_push)(MlwsCtx *cx, MlwsValue list, MlwsValue value, uint8_t type);
    MlwsValue (*map_new)(MlwsCtx *cx);
    bool (*map_get)(MlwsCtx *cx, MlwsValue map, MlwsValue key, uint8_t ktype, MlwsValue *value, uint8_t *vtype);
    bool (*map_set)(MlwsCtx *cx, MlwsValue map, MlwsValue key, uint8_t ktype, MlwsValue value, uint8_t vtype);

    /* Foreign handles: a C pointer owned by a process. A handle cannot be sent
     * to another process. foreign_get returns NULL (and records E_TYPE) for a
     * handle of another type or one already released. */
    MlwsValue (*foreign_new)(MlwsCtx *cx, const MlwsForeignType *type, void *ptr);
    void *(*foreign_get)(MlwsCtx *cx, MlwsValue ref, const MlwsForeignType *type);
    void (*foreign_release)(MlwsCtx *cx, MlwsValue ref); /* finalize now; idempotent */

    /* ---- messages from any thread, at any time ----
     * host() is called during a call; the MlwsHost stays valid until the VM is
     * freed, so join any thread that posts before the program can end. A post
     * to a process that has ended returns false. While a hold is taken, a
     * process waiting in recv is not reported as deadlocked: messages may
     * still come. */
    MlwsHost *(*host)(MlwsCtx *cx);
    bool (*post_int)(MlwsHost *host, uint64_t pid, int64_t value);
    bool (*post_str)(MlwsHost *host, uint64_t pid, const char *s, size_t n); /* a copy */
    void (*hold)(MlwsHost *host, int delta); /* +1 before posting starts, -1 when done */
} MlwsApi;

/* Return 0 on success. On failure, call api->init_error first to say why. */
typedef int (*MlwsExtensionInitFn)(const MlwsApi *api, MlwsExtension *ext);

#endif
