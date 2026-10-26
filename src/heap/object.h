/* object.h - layouts of garbage-collected heap objects and struct types.
 *
 * Every object starts with an Obj header and lives on its owning process's
 * heap list. String constants from module constant pools are IMMORTAL: shared,
 * read-only, and never on a heap list. */
#ifndef MLWSVM_OBJECT_H
#define MLWSVM_OBJECT_H

#include "../core/types.h"

/* Object kinds; also the values returned by the `typeof` instruction. */
enum {
    OBJ_NULL = 0, /* typeof only: a null reference */
    OBJ_STRING = 1,
    OBJ_BYTES = 2,
    OBJ_ARRAY = 3,
    OBJ_STRUCT = 4,
    OBJ_LIST = 5,
    OBJ_MAP = 6,
    OBJ_CLOSURE = 7,
    OBJ_BIG = 8,
    OBJ_DEC = 9,
    OBJ_FOREIGN = 10, /* a C resource owned by native code (docs/EXTENDING.md) */
};

#define OBJF_IMMORTAL 0x01
#define OBJF_MARK 0x02

typedef struct Obj {
    uint8_t kind;
    uint8_t flags;
    uint16_t pad;
    uint32_t pad2;
    struct Obj *next; /* heap list */
} Obj;

/* Immutable UTF-8 (by convention) byte string, NUL-terminated for C. */
typedef struct {
    Obj h;
    uint32_t len;
    uint32_t hash;
    char data[];
} ObjString;

/* Mutable, growable byte buffer. */
typedef struct {
    Obj h;
    uint32_t len, cap;
    uint8_t *data;
} ObjBytes;

/* Fixed-length array; every element is one Word of type `elem`. */
typedef struct {
    Obj h;
    uint8_t elem; /* MlwsType */
    uint32_t len;
    Word data[];
} ObjArray;

/* A struct type declared with .struct, owned by its loaded module. */
typedef struct StructDef {
    const char *name;
    const char *module;
    uint32_t nfields;
    char **fields;
    uint8_t *types; /* MlwsType per field */
} StructDef;

typedef struct {
    Obj h;
    const StructDef *def;
    Word fields[];
} ObjStruct;

/* Growable list of dynamically typed values (each with a type tag). */
typedef struct {
    Obj h;
    uint32_t len, cap;
    Word *items;
    uint8_t *tags;
} ObjList;

typedef struct {
    Word key, val;
    uint32_t hash;
    uint8_t ktag, vtag;
    uint8_t live;
} MapEntry;

/* Insertion-ordered hash map: entries in order, plus an open-addressing index. */
typedef struct {
    Obj h;
    uint32_t count;      /* live entries */
    uint32_t nentries;   /* used entry slots, including deleted ones */
    uint32_t entry_cap;
    MapEntry *entries;
    int32_t *index;      /* -1 empty, -2 deleted, else entry number */
    uint32_t index_cap;  /* power of two */
} ObjMap;

struct Function;

/* A function value: code plus captured values (UP[0..nup-1]), each with a tag. */
typedef struct {
    Obj h;
    struct Function *fn;
    uint32_t nup;
    uint8_t *tags; /* points just past up[] */
    Word up[];
} ObjClosure;

const char *obj_kind_name(int kind);

/* A foreign handle: an opaque C pointer with a typed finalizer. MLWSVM code
 * can hold one as a reference but cannot see or forge the pointer, and it
 * cannot leave its process. */
typedef struct {
    Obj h;
    const void *type; /* const MlwsForeignType * (include/mlwsvm/mlwsvm.h) */
    void *ptr;        /* NULL once released */
    struct ExtLib *lib;
} ObjForeign;

#endif
