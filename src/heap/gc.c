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
    case OBJ_CORO: return sizeof(ObjCoro) + (size_t)((const ObjCoro *)o)->words * 9; /* the stack and its tags */
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

/* Trap E_QUOTA if a heap of `bytes` would exceed p's quota. Only the running
 * process can trap; copies into other heaps (spawn arguments, messages) are
 * counted and caught at their next allocation. */
static void quota_check(Process *p, uint64_t bytes) {
    if (bytes > p->quota[QUOTA_HEAP] && p->state == PROC_RUNNING && !p->raising &&
        !p->in_foreign) /* checked when foreign code returns: it must not be unwound */
        vm_trap(p, "E_QUOTA", "heap quota of %llu bytes exceeded", (unsigned long long)p->quota[QUOTA_HEAP]);
}

void gc_account(Process *p, int64_t delta) {
    p->heap_bytes = (uint64_t)((int64_t)p->heap_bytes + delta);
    if (p->heap_bytes > p->heap_peak) p->heap_peak = p->heap_bytes;
    if (p->heap_bytes >= p->next_gc || p->vm->gc_stress) p->gc_pending = true;
    if (delta > 0) quota_check(p, p->heap_bytes);
}

/* ---- the small-object allocator ------------------------------------------------------
 *
 * Objects up to SLAB_MAX bytes are carved from the process's own chunks, in
 * 16-byte size classes with a free list each: no malloc call and no malloc
 * lock. Chunks start small (a process may hold only a few objects) and double
 * up to 64 KiB.
 *
 * A chunk holds nothing but objects, one after another up to `used`, each
 * starting with its header, whose `slab` gives its size. A free one keeps its
 * header, with kind OBJK_FREE and its free list's link in `next`. So a sweep
 * walks the chunks in address order instead of following a list through
 * memory, and rebuilds the free lists in address order as it goes, so that the
 * program then allocates through memory in order too. Objects from malloc (too
 * big for a class, or allocated without the chunks) are on the list
 * Process.heap instead. Objects' own small buffers (a list's storage) come
 * from chunks of their own, which have no headers to walk.
 *
 * A process's objects never move to another heap (messages are copied), so its
 * chunks are freed with it. Objects that do move are those built in a
 * fragment heap for a message, which uses malloc (Process.slabs is false). */
#define SLAB_GRAIN 16
#define SLAB_MAX 256
#define SLAB_CLASSES (SLAB_MAX / SLAB_GRAIN)
#define CHUNK_MIN 1024
#define CHUNK_MAX 65536
#define OBJK_FREE 0xFF /* the kind of a free slot in a chunk */

typedef struct Chunk {
    struct Chunk *next;
    size_t size, used, pad;
    unsigned char data[]; /* 16-byte aligned: malloc's alignment, after a 32-byte header */
} Chunk;

#if defined(__SANITIZE_ADDRESS__)
#define SLABS_DEFAULT false /* AddressSanitizer must see every object's lifetime */
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define SLABS_DEFAULT false
#endif
#endif
#ifndef SLABS_DEFAULT
#define SLABS_DEFAULT true
#endif

bool gc_slabs_enabled(void) { return SLABS_DEFAULT; }

static void *chunk_carve(Chunk **chunks, uint32_t *next_size, size_t n) {
    Chunk *c = *chunks;
    if (!c || c->used + n > c->size) {
        size_t size = *next_size ? *next_size : CHUNK_MIN;
        *next_size = (uint32_t)(size < CHUNK_MAX ? size * 2 : size);
        c = xmalloc(sizeof(Chunk) + size);
        c->size = size;
        c->used = 0;
        c->next = *chunks;
        *chunks = c;
    }
    void *b = c->data + c->used;
    c->used += n;
    return b;
}

Obj *gc_alloc_unaccounted(Process *p, uint8_t kind, size_t size) {
    Obj *o;
    if (p->slabs && size <= SLAB_MAX) {
        unsigned cls = (unsigned)((size - 1) / SLAB_GRAIN);
        void *b = p->slab_free[cls];
        if (b) p->slab_free[cls] = ((Obj *)b)->next;
        else b = chunk_carve(&p->chunks, &p->chunk_next, (size_t)(cls + 1) * SLAB_GRAIN);
        switch (cls) { /* a constant size for each class: the compiler inlines the stores */
#define ZERO(k) case k: memset(b, 0, (k + 1) * SLAB_GRAIN); break;
            ZERO(0) ZERO(1) ZERO(2) ZERO(3) ZERO(4) ZERO(5) ZERO(6) ZERO(7)
            ZERO(8) ZERO(9) ZERO(10) ZERO(11) ZERO(12) ZERO(13) ZERO(14) ZERO(15)
#undef ZERO
        }
        o = b;
        o->slab = (uint16_t)(cls + 1);
    } else {
        o = xcalloc(1, size);
        o->next = p->heap;
        p->heap = o;
    }
    o->kind = kind;
    p->objects_allocated++;
    if (!p->next_gc) p->next_gc = gc_threshold(p, 0);
    return o;
}

/* The quota is checked first: a trap after the object is linked would leave
 * the sweep an object its constructor never set up (a struct with no def). */
Obj *gc_alloc(Process *p, uint8_t kind, size_t size) {
    quota_check(p, p->heap_bytes + size);
    Obj *o = gc_alloc_unaccounted(p, kind, size);
    gc_account(p, (int64_t)size);
    return o;
}

/* An object's own buffer of n zeroed bytes (a list's storage): from p's buffer
 * chunks if the object came from the object chunks and the buffer is small,
 * else from malloc. The same test in gc_buf_free tells which. */
void *gc_buf_alloc(Process *p, const Obj *o, size_t n) {
    if (!o->slab || n > SLAB_MAX) return xcalloc(1, n);
    unsigned cls = (unsigned)((n - 1) / SLAB_GRAIN);
    void *b = p->buf_free[cls];
    if (b) p->buf_free[cls] = *(void **)b;
    else b = chunk_carve(&p->bchunks, &p->bchunk_next, (size_t)(cls + 1) * SLAB_GRAIN);
    memset(b, 0, (size_t)(cls + 1) * SLAB_GRAIN);
    return b;
}

/* With p NULL, the object's chunks are about to be freed with it. */
void gc_buf_free(Process *p, const Obj *o, void *b, size_t n) {
    if (!b) return;
    if (!o->slab || n > SLAB_MAX) {
        free(b);
        return;
    }
    if (!p) return;
    unsigned cls = (unsigned)((n - 1) / SLAB_GRAIN);
    if (p->vm->gc_stress) memset(b, 0xDB, (size_t)(cls + 1) * SLAB_GRAIN);
    *(void **)b = p->buf_free[cls];
    p->buf_free[cls] = b;
}

/* Free what an object owns besides itself (with p NULL, its process's
 * chunks are about to be freed). */
static void obj_release_buffers(Process *p, Obj *o) {
    switch (o->kind) {
    case OBJ_FOREIGN: foreign_finalize(p, o); break;
    case OBJ_CORO: coro_release((ObjCoro *)o); break;
    case OBJ_BYTES: free(((ObjBytes *)o)->data); break;
    case OBJ_LIST: {
        ObjList *l = (ObjList *)o;
        if (!list_storage_inline(l)) gc_buf_free(p, o, l->items, (size_t)l->cap * (sizeof(Word) + 1));
        break;
    }
    case OBJ_MAP:
        free(((ObjMap *)o)->entries);
        free(((ObjMap *)o)->index);
        break;
    default: break;
    }
}

/* An object from malloc (on a list, not in a chunk). */
static void obj_release(Process *p, Obj *o) {
    obj_release_buffers(p, o);
    free(o);
}

void gc_free_list(Obj *list) { /* objects of a fragment heap: never from chunks */
    while (list) {
        Obj *o = list;
        list = o->next;
        obj_release(NULL, o);
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

static void mark_segment(Process *p, const Segment *s, uint32_t from_word);

/* The R registers of every frame's window, and the closures being run. */
static void mark_frames(Process *p, uint64_t *regs, const Frame *frames, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        const Frame *f = &frames[i];
        Obj *const *r = (Obj *const *)frame_reg(regs, f, BANK_R, 0);
        for (uint32_t k = 0; k < f->fn->mf->nregs[BANK_R]; k++) mark(p, r[k]);
        mark(p, f->closure);
    }
}

/* The roots of an execution context: what gc_collect marks for the running one. */
static void mark_ctx(Process *p, const ExecCtx *x) {
    mark_frames(p, x->regs, x->frames, x->nframes);
    mark_segment(p, &x->stack, x->stack_ref_lo);
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
    case OBJ_CORO: { /* its own context, or (running) its resumer's */
        ObjCoro *co = (ObjCoro *)o;
        mark(p, &co->fn->h);
        if (co->prev) mark(p, &co->prev->h);
        if (co->status != CO_DEAD) mark_ctx(p, &co->ctx);
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

/* Sweep the chunks in address order: unmark the marked objects, free the
 * rest, and rebuild each class's free list from the free slots, in order. */
static void sweep_chunks(Process *p) {
    Obj **tail[SLAB_CLASSES];
    for (int k = 0; k < SLAB_CLASSES; k++) tail[k] = (Obj **)&p->slab_free[k];
    for (Chunk *c = p->chunks; c; c = c->next) {
        for (size_t off = 0; off < c->used;) {
            Obj *o = (Obj *)(c->data + off);
            unsigned cls = o->slab - 1u;
            off += (size_t)o->slab * SLAB_GRAIN;
            if (o->kind != OBJK_FREE) {
                if (o->flags & OBJF_MARK) {
                    o->flags &= (uint8_t)~OBJF_MARK;
                    continue;
                }
                p->heap_bytes -= obj_footprint(o);
                obj_release_buffers(p, o);
                /* Under GC stress, garbage-fill freed objects, so that a use
                 * after a free goes wrong loudly (ASan cannot see inside the
                 * chunks). */
                if (p->vm->gc_stress) memset(o, 0xDB, (size_t)o->slab * SLAB_GRAIN);
                o->kind = OBJK_FREE;
                o->flags = 0;
                o->slab = (uint16_t)(cls + 1);
                p->gc_freed_objects++;
            }
            *tail[cls] = o;
            tail[cls] = &o->next;
        }
    }
    for (int k = 0; k < SLAB_CLASSES; k++) *tail[k] = NULL;
}

void gc_collect(Process *p) {
    uint64_t t0 = plat_now_ns();
    mark_frames(p, p->regs, p->frames, p->nframes);
    /* The stack from the lowest word ever tagged as a reference: code may legally
     * write references below SP, so SP is not the bound. */
    mark_segment(p, &p->stack, p->stack_ref_lo);
    for (uint32_t i = 0; i < p->nglobals; i++) mark_segment(p, &p->globals[i], 0);
    for (size_t i = 0; i < p->handles.n; i++) mark(p, p->handles.v[i]);
    mark(p, p->exception);
    if (p->co) mark(p, &p->co->h); /* the coroutines that resumed it, and the context below each */
    for (size_t i = 0; i < p->pins.n; i++) mark(p, p->pins.v[i]);
    mark(p, p->cb_exception);
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
    sweep_chunks(p);
    Obj **pp = &p->heap;
    while (*pp) {
        Obj *o = *pp;
        if (o->flags & OBJF_MARK) {
            o->flags &= (uint8_t)~OBJF_MARK;
            pp = &o->next;
        } else {
            *pp = o->next;
            p->heap_bytes -= obj_footprint(o);
            obj_release(p, o);
            p->gc_freed_objects++;
        }
    }
    p->next_gc = gc_threshold(p, p->heap_bytes);
    p->gc_pending = false;
    p->gc_count++;
    p->gc_ns += plat_now_ns() - t0;
}

/* Each object in p's chunks that is not free. */
#define FOR_CHUNK_OBJECTS(p, o)                                                \
    for (Chunk *c_ = (p)->chunks; c_; c_ = c_->next)                           \
        for (size_t off_ = 0; off_ < c_->used; off_ += (size_t)((Obj *)(c_->data + off_))->slab * SLAB_GRAIN) \
            for (Obj *o = (Obj *)(c_->data + off_); o && o->kind != OBJK_FREE; o = NULL)

void heap_release_foreign(Process *p) {
    for (Obj *o = p->heap; o; o = o->next)
        if (o->kind == OBJ_FOREIGN) foreign_finalize(p, o); /* idempotent: the handle forgets its pointer */
    FOR_CHUNK_OBJECTS(p, o)
        if (o->kind == OBJ_FOREIGN) foreign_finalize(p, o);
    foreign_run_deferred(p, NULL); /* none, unless a call never returned */
}

static void free_chunks(Chunk **chunks) {
    while (*chunks) {
        Chunk *c = *chunks;
        *chunks = c->next;
        free(c);
    }
}

void heap_free_all(Process *p) {
    while (p->heap) {
        Obj *o = p->heap;
        p->heap = o->next;
        p->heap_bytes -= obj_footprint(o);
        obj_release(NULL, o); /* the buffer chunks go below, all at once */
    }
    FOR_CHUNK_OBJECTS(p, o) {
        p->heap_bytes -= obj_footprint(o);
        obj_release_buffers(NULL, o);
    }
    free_chunks(&p->chunks);
    free_chunks(&p->bchunks);
    memset(p->slab_free, 0, sizeof p->slab_free);
    memset(p->buf_free, 0, sizeof p->buf_free);
    p->chunk_next = p->bchunk_next = 0;
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
