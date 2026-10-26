/* heap.h - per-process garbage-collected heap: allocation, collection and the
 * operations on each object kind.
 *
 * Collection only ever happens between instructions (interp.c checks
 * Process.gc_pending), so C code may hold unrooted object pointers for the
 * duration of one instruction or native call. */
#ifndef MLWSVM_HEAP_H
#define MLWSVM_HEAP_H

#include "../vm/vm.h"

/* ---- allocation and collection (gc.c) ------------------------------------ */

Obj *gc_alloc(Process *p, uint8_t kind, size_t size); /* zeroed and linked */
/* Linked but not yet charged: call gc_account(p, size) once temporaries are freed. */
Obj *gc_alloc_unaccounted(Process *p, uint8_t kind, size_t size);
void gc_account(Process *p, int64_t delta);           /* external buffer growth */
void gc_collect(Process *p);
void heap_free_all(Process *p);
/* Messages: free a detached object list, or link one into p's heap. */
void gc_free_list(Obj *list);
void gc_adopt(Process *p, Obj *list, uint64_t bytes, uint64_t nobjs);
size_t obj_footprint(const Obj *o);

/* Handles let raw memory refer to GC objects: an i64 id kept as a root. */
int64_t handle_new(Process *p, Obj *o);
Obj *handle_get(Process *p, int64_t h);
void handle_free(Process *p, int64_t h);

/* ---- objects (objects.c) -------------------------------------------------- */

ObjString *str_new(Process *p, const char *s, size_t n);
ObjBytes *bytes_new(Process *p, int64_t len);
void bytes_append(Process *p, ObjBytes *b, const void *data, size_t n);
void bytes_resize(Process *p, ObjBytes *b, int64_t len);
ObjArray *array_new(Process *p, MlwsType elem, int64_t len);
ObjStruct *struct_new(Process *p, const StructDef *def);
ObjList *list_new(Process *p, int64_t cap);
void list_push(Process *p, ObjList *l, Word v, uint8_t tag);
void list_insert(Process *p, ObjList *l, int64_t at, Word v, uint8_t tag);
void list_delete(Process *p, ObjList *l, int64_t at);
void list_resize(Process *p, ObjList *l, int64_t len);
ObjMap *map_new(Process *p);
MapEntry *map_find(ObjMap *m, Word key, uint8_t ktag);
void map_set(Process *p, ObjMap *m, Word key, uint8_t ktag, Word val, uint8_t vtag);
bool map_delete(ObjMap *m, Word key, uint8_t ktag);
ObjList *map_keys(Process *p, ObjMap *m);
ObjClosure *closure_new(Process *p, struct Function *fn, uint32_t nup);

/* A printable representation of a value (`tostr`). */
ObjString *value_to_string(Process *p, Word v, MlwsType t);

#endif
