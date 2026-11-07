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

#define GC_SMALL_MAX 256 /* the largest object from the chunks */
/* The kinds that always own memory besides themselves (a list may come to). */
#define GC_OWNER_KINDS (1u << OBJ_BYTES | 1u << OBJ_MAP | 1u << OBJ_FOREIGN | 1u << OBJ_CORO)

Obj *gc_alloc_slow(Process *p, uint8_t kind, size_t size);
/* A new object, zeroed and linked. The fast path takes the next bytes of the
 * hole being allocated into (gc.c), if the object fits there, owns nothing
 * besides itself, and keeps the heap below both the next collection's
 * threshold and the heap quota (Process.alloc_limit, 0 under GC stress). */
static inline Obj *gc_alloc(Process *p, uint8_t kind, size_t size) {
    size_t n = (size + 15) & ~(size_t)15;
    unsigned char *b = p->bump;
    uint64_t hb = p->heap_bytes + size;
    if (__builtin_expect(size <= GC_SMALL_MAX && !((1u << kind) & GC_OWNER_KINDS) &&
                             (size_t)(p->bump_lim - b) >= n && hb < p->alloc_limit, 1)) {
        p->bump = b + n;
        p->heap_bytes = hb;
        Obj *o = (Obj *)b;
        o->kind = kind;
        o->slab = (uint16_t)(n / 16);
        o->off = (uint32_t)(b - p->bump_base);
        p->young_plain_bytes += size;
        p->objects_allocated++;
        return o;
    }
    return gc_alloc_slow(p, kind, size);
}
/* Linked but not yet charged: call gc_account(p, size) once temporaries are freed. */
Obj *gc_alloc_unaccounted(Process *p, uint8_t kind, size_t size);
void gc_account(Process *p, int64_t delta);           /* external buffer growth */
void *gc_buf_alloc(Process *p, Obj *o, size_t n); /* o's own buffer, zeroed */
void gc_buf_free(Process *p, const Obj *o, void *b, size_t n); /* n: as allocated */
void gc_collect(Process *p);
uint64_t gc_heap_peak(const Process *p); /* the heap's largest size so far */
/* The chunks' size, their lines, and the lines old objects were on at the
 * last collection (--stats: how fragmented they are). */
void gc_chunk_usage(const Process *p, uint64_t *bytes, uint64_t *lines, uint64_t *used);

/* The write barrier. An old object that is given a young one is remembered,
 * so that the next minor collection finds the young one from it: every store
 * of a reference into an existing object (a field, an element, a capture, a
 * map entry, a coroutine's context) calls gc_write after it, or gc_touch
 * when what was stored is not known. An object just allocated is young and
 * needs neither. */
void gc_remember(Process *p, Obj *o);
static inline bool gc_young(const Obj *v) { return v && !(v->flags & (OBJF_OLD | OBJF_IMMORTAL)); }
static inline void gc_write(Process *p, Obj *o, const Obj *v) {
    if (__builtin_expect((o->flags & (OBJF_OLD | OBJF_REMEMBERED)) == OBJF_OLD, 0) && gc_young(v)) gc_remember(p, o);
}
static inline void gc_write_word(Process *p, Obj *o, Word w, uint8_t tag) {
    if (type_is_ref((MlwsType)tag)) gc_write(p, o, w.r);
}
static inline void gc_touch(Process *p, Obj *o) {
    if ((o->flags & (OBJF_OLD | OBJF_REMEMBERED)) == OBJF_OLD) gc_remember(p, o);
}
void heap_free_all(Process *p);
/* Messages: free a detached object list, or link one into p's heap. */
void gc_free_list(Obj *list);
void gc_adopt(Process *p, Obj *list, uint64_t bytes, uint64_t nobjs);
void heap_release_foreign(Process *p); /* finalize every foreign handle now (files, sockets, C resources) */
bool gc_slabs_enabled(void); /* the small-object allocator is on (not under ASan) */
size_t obj_footprint(const Obj *o);

/* Handles let raw memory refer to GC objects: an i64 id kept as a root. */
int64_t handle_new(Process *p, Obj *o);
Obj *handle_get(Process *p, int64_t h);
void handle_free(Process *p, int64_t h);

/* ---- objects (objects.c) -------------------------------------------------- */

ObjString *str_new(Process *p, const char *s, size_t n);
ObjString *str_alloc(Process *p, size_t n); /* to fill in */
uint32_t str_hash(ObjString *s);
ObjBytes *bytes_new(Process *p, int64_t len);
void bytes_append(Process *p, ObjBytes *b, const void *data, size_t n);
void bytes_resize(Process *p, ObjBytes *b, int64_t len);
ObjArray *array_new(Process *p, MlwsType elem, int64_t len);
ObjStruct *struct_new(Process *p, const StructDef *def);
/* Lists made with a capacity up to this keep their storage in their own
 * allocation, which stays within the largest small object (GC_SMALL_MAX):
 * one allocation instead of two. */
#define LIST_INLINE_MAX 24
ObjList *list_new(Process *p, int64_t cap);
void list_push(Process *p, ObjList *l, Word v, uint8_t tag);
void list_grow(Process *p, ObjList *l, uint64_t need); /* capacity for need elements (may collect, or trap) */
void list_insert(Process *p, ObjList *l, int64_t at, Word v, uint8_t tag);
void list_delete(Process *p, ObjList *l, int64_t at);
void list_resize(Process *p, ObjList *l, int64_t len);
ObjMap *map_new(Process *p);
MapEntry *map_find(ObjMap *m, Word key, uint8_t ktag);
void map_set(Process *p, ObjMap *m, Word key, uint8_t ktag, Word val, uint8_t vtag);
bool map_delete(ObjMap *m, Word key, uint8_t ktag);
void map_delete_entry(ObjMap *m, MapEntry *e); /* without comparing keys: a collection, whose dead keys may be anything */
ObjList *map_keys(Process *p, ObjMap *m);
ObjClosure *closure_new(Process *p, struct Function *fn, uint32_t nup);

/* A printable representation of a value (`tostr`). */
ObjString *value_to_string(Process *p, Word v, MlwsType t);

#endif
