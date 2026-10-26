/* gc.c - precise, non-moving mark-sweep collector, one heap per process.
 *
 * Roots: the R register stack (every live frame's window), reference-tagged
 * words of the data stack and of every global segment, and the handle table.
 * Because collections only run between instructions, no other roots exist. */
#include "heap.h"

#include "../platform/platform.h"
#include "numeric.h"

#define GC_MIN_THRESHOLD (1u << 20)

size_t obj_footprint(const Obj *o) {
    switch (o->kind) {
    case OBJ_STRING: return sizeof(ObjString) + ((const ObjString *)o)->len + 1;
    case OBJ_BYTES: return sizeof(ObjBytes) + ((const ObjBytes *)o)->cap;
    case OBJ_ARRAY: return sizeof(ObjArray) + ((const ObjArray *)o)->len * sizeof(Word);
    case OBJ_STRUCT: return sizeof(ObjStruct) + ((const ObjStruct *)o)->def->nfields * sizeof(Word);
    case OBJ_LIST: return sizeof(ObjList) + ((const ObjList *)o)->cap * (sizeof(Word) + 1);
    case OBJ_MAP: {
        const ObjMap *m = (const ObjMap *)o;
        return sizeof(ObjMap) + m->entry_cap * sizeof(MapEntry) + m->index_cap * sizeof(int32_t);
    }
    case OBJ_CLOSURE: return sizeof(ObjClosure) + ((const ObjClosure *)o)->nup * (sizeof(Word) + 1);
    case OBJ_BIG: case OBJ_DEC: return num_footprint(o);
    case OBJ_FOREIGN: return sizeof(ObjForeign);
    default: return sizeof(Obj);
    }
}

/* Next collection threshold: twice the live heap, at least the minimum, and
 * early enough to reclaim garbage before a heap quota is hit. */
static uint64_t gc_threshold(Process *p, uint64_t live) {
    uint64_t min = p->vm->gc_min_threshold ? p->vm->gc_min_threshold : GC_MIN_THRESHOLD;
    uint64_t t = live * 2 > min ? live * 2 : min;
    uint64_t q = p->quota[QUOTA_HEAP];
    if (q != UINT64_MAX) {
        if (t > q / 4 * 3) t = q / 4 * 3; /* collect well before the quota... */
        /* ...but always let the heap grow by an eighth first: a heap whose live
         * data is near its quota must not be collected at every allocation
         * (that is quadratic). It reaches the quota after a few collections. */
        uint64_t room = live + live / 8 + 4096;
        if (t < room) t = room < q ? room : q;
    }
    return t;
}

void gc_account(Process *p, int64_t delta) {
    p->heap_bytes = (uint64_t)((int64_t)p->heap_bytes + delta);
    if (p->heap_bytes > p->heap_peak) p->heap_peak = p->heap_bytes;
    if (p->heap_bytes >= p->next_gc || p->vm->gc_stress) p->gc_pending = true;
    /* Only the running process can trap; copies into other heaps (spawn
     * arguments, messages) are counted and caught at their next allocation. */
    if (delta > 0 && p->heap_bytes > p->quota[QUOTA_HEAP] && p->state == PROC_RUNNING && !p->raising &&
        !p->in_foreign) /* checked when foreign code returns: it must not be unwound */
        vm_trap(p, "E_QUOTA", "heap quota of %llu bytes exceeded", (unsigned long long)p->quota[QUOTA_HEAP]);
}

Obj *gc_alloc_unaccounted(Process *p, uint8_t kind, size_t size) {
    Obj *o = xcalloc(1, size);
    o->kind = kind;
    o->next = p->heap;
    p->heap = o;
    p->objects_allocated++;
    if (!p->next_gc) p->next_gc = gc_threshold(p, 0);
    return o;
}

Obj *gc_alloc(Process *p, uint8_t kind, size_t size) {
    Obj *o = gc_alloc_unaccounted(p, kind, size);
    gc_account(p, (int64_t)size);
    return o;
}

static void obj_release(Obj *o) {
    switch (o->kind) {
    case OBJ_FOREIGN: foreign_finalize(o); break;
    case OBJ_BYTES: free(((ObjBytes *)o)->data); break;
    case OBJ_LIST:
        free(((ObjList *)o)->items);
        free(((ObjList *)o)->tags);
        break;
    case OBJ_MAP:
        free(((ObjMap *)o)->entries);
        free(((ObjMap *)o)->index);
        break;
    default: break;
    }
    free(o);
}

static void obj_free(Process *p, Obj *o) {
    p->heap_bytes -= obj_footprint(o);
    obj_release(o);
}

void gc_free_list(Obj *list) {
    while (list) {
        Obj *o = list;
        list = o->next;
        obj_release(o);
    }
}

void gc_adopt(Process *p, Obj *list, uint64_t bytes, uint64_t nobjs) {
    if (!list) return;
    Obj *tail = list;
    while (tail->next) tail = tail->next;
    tail->next = p->heap;
    p->heap = list;
    p->objects_allocated += nobjs;
    if (!p->next_gc) p->next_gc = gc_threshold(p, 0);
    /* Like gc_account, but never traps: a heap quota is caught at the next allocation. */
    p->heap_bytes += bytes;
    if (p->heap_bytes > p->heap_peak) p->heap_peak = p->heap_bytes;
    if (p->heap_bytes >= p->next_gc || p->vm->gc_stress) p->gc_pending = true;
}

static void mark(Process *p, Obj *o) {
    if (!o || (o->flags & (OBJF_MARK | OBJF_IMMORTAL))) return;
    o->flags |= OBJF_MARK;
    VEC_PUSH(p->markstack, o);
}

static void scan(Process *p, Obj *o) {
    switch (o->kind) {
    case OBJ_ARRAY: {
        ObjArray *a = (ObjArray *)o;
        if (a->elem == T_R)
            for (uint32_t i = 0; i < a->len; i++) mark(p, a->data[i].r);
        break;
    }
    case OBJ_STRUCT: {
        ObjStruct *s = (ObjStruct *)o;
        for (uint32_t i = 0; i < s->def->nfields; i++)
            if (type_is_ref(s->def->types[i])) mark(p, s->fields[i].r);
        break;
    }
    case OBJ_LIST: {
        ObjList *l = (ObjList *)o;
        for (uint32_t i = 0; i < l->len; i++)
            if (l->tags[i] == T_R) mark(p, l->items[i].r);
        break;
    }
    case OBJ_CLOSURE: {
        ObjClosure *cl = (ObjClosure *)o;
        for (uint32_t i = 0; i < cl->nup; i++)
            if (cl->tags[i] == T_R) mark(p, cl->up[i].r);
        break;
    }
    case OBJ_MAP: {
        ObjMap *m = (ObjMap *)o;
        for (uint32_t i = 0; i < m->nentries; i++) {
            MapEntry *e = &m->entries[i];
            if (!e->live) continue;
            if (e->ktag == T_R) mark(p, e->key.r);
            if (e->vtag == T_R) mark(p, e->val.r);
        }
        break;
    }
    default: break;
    }
}

static void mark_segment(Process *p, const Segment *s, uint32_t from_word) {
    if (!s->tags) return;
    for (uint32_t w = from_word; w < s->size / 8; w++) {
        if (s->tags[w] != T_R) continue;
        Obj *o;
        memcpy(&o, s->data + (size_t)w * 8, sizeof o);
        mark(p, o);
    }
}

void gc_collect(Process *p) {
    uint64_t t0 = plat_now_ns();
    for (uint32_t i = 0; i < p->rtop[BANK_R]; i++) mark(p, p->rreg[i]);
    for (uint32_t i = 0; i < p->nframes; i++) mark(p, p->frames[i].closure); /* closures being run */
    /* The whole stack: code may legally write references below SP. */
    mark_segment(p, &p->stack, 0);
    for (uint32_t i = 0; i < p->nglobals; i++) mark_segment(p, &p->globals[i], 0);
    for (size_t i = 0; i < p->handles.n; i++) mark(p, p->handles.v[i]);
    mark(p, p->exception);
    if (p->bcall.state != BCALL_NONE) { /* a blocking native call's arguments and results */
        const NativeDef *nd = p->bcall.nd;
        for (int k = 0; k < nd->np; k++)
            if (type_is_ref(nd->pt[k])) mark(p, p->bcall.args[k].r);
        if (p->bcall.state == BCALL_DONE)
            for (int k = 0; k < nd->nr; k++)
                if (type_is_ref(nd->rt[k])) mark(p, p->bcall.rets[k].r);
    }
    for (size_t i = p->mbox_head; i < p->mbox.n; i++)
        if (p->mbox.v[i].tag == T_R) mark(p, p->mbox.v[i].v.r);
    while (p->markstack.n) scan(p, p->markstack.v[--p->markstack.n]);
    Obj **pp = &p->heap;
    while (*pp) {
        Obj *o = *pp;
        if (o->flags & OBJF_MARK) {
            o->flags &= (uint8_t)~OBJF_MARK;
            pp = &o->next;
        } else {
            *pp = o->next;
            obj_free(p, o);
            p->gc_freed_objects++;
        }
    }
    p->next_gc = gc_threshold(p, p->heap_bytes);
    p->gc_pending = false;
    p->gc_count++;
    p->gc_ns += plat_now_ns() - t0;
}

void heap_free_all(Process *p) {
    while (p->heap) {
        Obj *o = p->heap;
        p->heap = o->next;
        obj_free(p, o);
    }
    VEC_FREE(p->handles);
    VEC_FREE(p->handle_free);
    VEC_FREE(p->markstack);
}

/* ---- handles ------------------------------------------------------------- */

int64_t handle_new(Process *p, Obj *o) {
    if (!o) vm_trap(p, "E_NULL", "cannot make a handle for null");
    uint32_t idx;
    if (p->handle_free.n) {
        idx = p->handle_free.v[--p->handle_free.n];
        p->handles.v[idx] = o;
    } else {
        idx = (uint32_t)p->handles.n;
        VEC_PUSH(p->handles, o);
    }
    return (int64_t)idx + 1;
}

static uint32_t handle_index(Process *p, int64_t h) {
    if (h <= 0 || (uint64_t)h > p->handles.n || !p->handles.v[h - 1])
        vm_trap(p, "E_BAD_HANDLE", "invalid or freed handle %lld", (long long)h);
    return (uint32_t)(h - 1);
}

Obj *handle_get(Process *p, int64_t h) { return p->handles.v[handle_index(p, h)]; }

void handle_free(Process *p, int64_t h) {
    uint32_t i = handle_index(p, h);
    p->handles.v[i] = NULL;
    VEC_PUSH(p->handle_free, i);
}
