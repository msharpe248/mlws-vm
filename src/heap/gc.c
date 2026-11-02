/* gc.c - precise, non-moving, generational mark-region collector, one heap
 * per process.
 *
 * Roots: the R register stack (every live frame's window), reference-tagged
 * words of the data stack and of every global segment, and the handle table.
 * Because collections only run between instructions, no other roots exist. */
#include "heap.h"

#include "../platform/platform.h"
#include "numeric.h"

#define GC_MIN_THRESHOLD (64u << 10) /* the young generation's start: in the caches, for many processes */
#define GC_NURSERY_MAX (16u << 20)

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

/* Next collection threshold: the heap plus a nursery of the minimum (the
 * young objects a minor collection walks), and early enough to reclaim
 * garbage before a heap quota is hit. */
static uint64_t gc_threshold(Process *p, uint64_t live) {
    uint64_t min = p->nursery;
    if (!min) min = p->nursery = p->vm->gc_min_threshold ? p->vm->gc_min_threshold : GC_MIN_THRESHOLD;
    if (p->vm->gc_verify && !p->vm->gc_min_threshold) min = 16384; /* minor collections often, each checked */
    uint64_t t = live + min;
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

/* The heap's peak is updated lazily: the fast path of gc_alloc never shrinks
 * the heap, so its largest size is seen before anything does (here, and at a
 * collection's start). */
static void note_peak(Process *p) {
    if (p->heap_bytes > p->heap_peak) p->heap_peak = p->heap_bytes;
}

uint64_t gc_heap_peak(const Process *p) { return p->heap_bytes > p->heap_peak ? p->heap_bytes : p->heap_peak; }

/* What gc_alloc's fast path checks: below the next collection's threshold
 * (once one is due, it runs at the next safepoint: only the quota then),
 * and within the quota (none under GC stress, which collects at every
 * allocation). */
static void set_alloc_limit(Process *p) {
    uint64_t q = p->quota[QUOTA_HEAP], l = p->gc_pending ? UINT64_MAX : p->next_gc;
    if (q < l) l = q + 1;
    p->alloc_limit = p->vm->gc_stress ? 0 : l;
}

void gc_account(Process *p, int64_t delta) {
    note_peak(p);
    p->heap_bytes = (uint64_t)((int64_t)p->heap_bytes + delta);
    note_peak(p);
    if (p->heap_bytes >= p->next_gc || p->vm->gc_stress) p->gc_pending = true;
    if (delta > 0) quota_check(p, p->heap_bytes);
}

/* ---- the small-object allocator ------------------------------------------------------
 *
 * Objects up to SLAB_MAX bytes are bump-allocated from the process's own
 * chunks: no malloc call, no lock, no free list. Chunks start small (a
 * process may hold only a few objects) and double up to 64 KiB.
 *
 * A chunk is divided into lines of LINE bytes, each with a mark byte, set by
 * a collection when an object on that line survives it (objects may span
 * lines). The allocator moves through the holes, the runs of lines no
 * survivor marked, zeroing a little ahead of itself, and each object takes the
 * next bytes of the hole. So an object that dies young is never touched
 * again: a collection marks what survives, and every line it did not mark is
 * free for the next cycle. These are Immix's lines without its evacuation:
 * nothing moves, since C code and map keys rely on objects' addresses.
 *
 * An object that owns memory besides itself (a byte buffer's bytes, a map's
 * entries, a list's storage, a foreign handle's resource, a coroutine's
 * stack) is on an owner list, which a collection walks to free what the dead
 * ones own. Objects from malloc (too big for a chunk, or allocated without
 * chunks) are on the list Process.heap instead. Objects' own small buffers (a
 * list's storage) come from chunks of their own, with a free list per 16-byte
 * size class.
 *
 * A process's objects never move to another heap (messages are copied), so its
 * chunks are freed with it. Objects that do move are those built in a
 * fragment heap for a message, which uses malloc (Process.slabs is false). */
#define SLAB_GRAIN 16
#define SLAB_MAX 256
#define SLAB_CLASSES (SLAB_MAX / SLAB_GRAIN)
#define CHUNK_MIN 1024
#define CHUNK_MAX 65536
#define LINE_SHIFT 7
#define LINE (1u << LINE_SHIFT)
#define ZERO_STEP 2048 /* how far ahead of the allocator a hole is zeroed at a time */
/* The kinds that always own memory besides themselves (a list may come to). */
#define OWNER_KINDS (1u << OBJ_BYTES | 1u << OBJ_MAP | 1u << OBJ_FOREIGN | 1u << OBJ_CORO)

typedef struct Chunk {
    struct Chunk *next;
    uint32_t size, used;  /* used: buffer chunks only */
    uint32_t nlines, pad;
    uint8_t *lines;       /* object chunks: a mark byte per line, after the data */
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
        c->size = (uint32_t)size;
        c->used = 0;
        c->lines = NULL;
        c->next = *chunks;
        *chunks = c;
    }
    void *b = c->data + c->used;
    c->used += (uint32_t)n;
    return b;
}

static Chunk *chunk_new(Process *p) {
    uint32_t size = p->chunk_next ? p->chunk_next : CHUNK_MIN;
    p->chunk_next = size < CHUNK_MAX ? size * 2 : size;
    uint32_t nlines = size >> LINE_SHIFT;
    Chunk *c = xmalloc(sizeof(Chunk) + size + nlines);
    c->size = size;
    c->used = 0;
    c->nlines = nlines;
    c->lines = c->data + size;
    memset(c->lines, 0, nlines);
    c->next = p->chunks;
    p->chunks = c;
    p->chunk_bytes += size;
    return c;
}

/* The next hole of at least n bytes in the chunk being allocated into. */
static bool next_hole(Process *p, size_t n) {
    Chunk *c = p->bump_chunk;
    uint32_t l = p->bump_line;
    while (l < c->nlines) {
        while (l < c->nlines && c->lines[l]) l++;
        uint32_t start = l;
        while (l < c->nlines && !c->lines[l]) l++;
        if ((size_t)(l - start) << LINE_SHIFT >= n) {
            p->bump = p->bump_lim = c->data + ((size_t)start << LINE_SHIFT);
            p->hole_end = c->data + ((size_t)l << LINE_SHIFT);
            p->bump_line = l;
            return true;
        }
    }
    p->bump_line = l;
    return false;
}

/* The allocator's slow path: n more bytes zeroed, in this hole, or the next
 * hole that fits, in this chunk, the next chunk with free lines, or a new one. */
static MLWS_NOINLINE unsigned char *bump_refill(Process *p, size_t n) {
    for (;;) {
        if (p->bump && (size_t)(p->hole_end - p->bump) >= n) {
            unsigned char *lim = p->bump_lim + ZERO_STEP;
            if (lim < p->bump + n) lim = p->bump + n;
            if (lim > p->hole_end) lim = p->hole_end;
            memset(p->bump_lim, 0, (size_t)(lim - p->bump_lim));
            p->bump_lim = lim;
            return p->bump;
        }
        if (p->bump_chunk && next_hole(p, n)) continue;
        Chunk *c;
        if (p->recycle_i < p->recycle.n) {
            c = p->recycle.v[p->recycle_i++];
        } else {
            c = chunk_new(p);
            VEC_PUSH(p->recycle, c);
            p->recycle_i = p->recycle.n;
        }
        p->bump_chunk = c;
        p->bump_base = c->data;
        p->bump_line = 0;
        p->bump = p->bump_lim = p->hole_end = NULL;
    }
}

/* o, in the chunks, owns memory besides itself from now on: a collection
 * that finds it dead must free that. */
static void gc_own(Process *p, Obj *o) {
    o->flags |= OBJF_OWNS;
    if (o->flags & OBJF_OLD) VEC_PUSH(p->old_owners, o);
    else VEC_PUSH(p->young_owners, o);
}

Obj *gc_alloc_unaccounted(Process *p, uint8_t kind, size_t size) {
    Obj *o;
    if (p->slabs && size <= SLAB_MAX) {
        size_t n = (size + SLAB_GRAIN - 1) & ~(size_t)(SLAB_GRAIN - 1);
        unsigned char *b = p->bump;
        if (__builtin_expect((size_t)(p->bump_lim - b) < n, 0)) b = bump_refill(p, n);
        p->bump = b + n;
        o = (Obj *)b;
        o->kind = kind;
        o->slab = (uint16_t)(n / SLAB_GRAIN);
        o->off = (uint32_t)(b - p->bump_base);
        if ((1u << kind) & OWNER_KINDS) gc_own(p, o);
        else p->young_plain_bytes += size;
    } else {
        o = xcalloc(1, size);
        o->kind = kind;
        o->next = p->heap;
        p->heap = o;
    }
    p->objects_allocated++;
    if (!p->next_gc) p->next_gc = gc_threshold(p, 0);
    return o;
}

/* The quota is checked first: a trap after the object is linked would leave
 * the collector an object its constructor never set up (a struct with no def). */
Obj *gc_alloc_slow(Process *p, uint8_t kind, size_t size) {
    quota_check(p, p->heap_bytes + size);
    Obj *o = gc_alloc_unaccounted(p, kind, size);
    gc_account(p, (int64_t)size);
    set_alloc_limit(p);
    return o;
}

/* An object's own buffer of n zeroed bytes (a list's storage): from p's buffer
 * chunks if the object came from the object chunks and the buffer is small,
 * else from malloc. The same test in gc_buf_free tells which. Either way, an
 * object in the chunks becomes an owner. */
void *gc_buf_alloc(Process *p, Obj *o, size_t n) {
    if (o->slab && !(o->flags & OBJF_OWNS)) {
        if (!(o->flags & OBJF_OLD)) p->young_plain_bytes -= obj_footprint(o); /* counted as an owner now */
        gc_own(p, o);
    }
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
    if (!p->next_gc) p->next_gc = gc_threshold(p, 0), set_alloc_limit(p);
    /* Like gc_account, but never traps: a heap quota is caught at the next allocation. */
    p->heap_bytes += bytes;
    note_peak(p);
    if (p->heap_bytes >= p->next_gc || p->vm->gc_stress) p->gc_pending = true;
}

static void verify_reach(Process *p, Obj *o);

static MLWS_COLD void footprint_drift(const Obj *o, size_t fp) {
    fprintf(stderr, "mlwsvm: gc verify: a %s of %zu bytes was allocated as %u granules (its accounting drifted)\n",
            obj_kind_name(o->kind), fp, o->slab);
    abort();
}

/* Marking makes an object old: its flags become OLD plus the current mark
 * bit. A minor collection skips every old object (each has the current bit),
 * so it marks only young ones, and finds those an old one holds from the
 * remembered set. A major collection first flips the bit, so that every old
 * object reads as unmarked; no pass is needed to clear the marks after.
 * Marking also marks the lines an object of the chunks lies on, and counts
 * what it keeps. */
static void mark(Process *p, Obj *o) {
    if (!o) return;
    if (p->gc_verifying) {
        verify_reach(p, o);
        return;
    }
    uint8_t f = o->flags;
    if ((f & (OBJF_OLD | OBJF_MARK)) == (OBJF_OLD | p->mark_bit) || (f & OBJF_IMMORTAL)) return;
    o->flags = (uint8_t)((f & ~OBJF_MARK) | OBJF_OLD | p->mark_bit);
    size_t fp = obj_footprint(o);
    p->gc_live++;
    p->gc_live_bytes += fp;
    if (o->slab) {
        Chunk *c = (Chunk *)((unsigned char *)o - o->off - offsetof(Chunk, data));
        uint32_t l = o->off >> LINE_SHIFT, last = (o->off + o->slab * SLAB_GRAIN - 1u) >> LINE_SHIFT;
        do c->lines[l] = 1;
        while (++l <= last);
        if (!(f & (OBJF_OLD | OBJF_OWNS))) { /* young, and counted in young_plain_bytes */
            p->gc_kept_bytes += fp;
            if (p->vm->gc_verify && (fp + SLAB_GRAIN - 1) / SLAB_GRAIN != o->slab) footprint_drift(o, fp);
        }
    }
    VEC_PUSH(p->markstack, o);
}
void gc_remember(Process *p, Obj *o) {
    o->flags |= OBJF_REMEMBERED;
    VEC_PUSH(p->remset, o);
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

/* Whether a chunk has a free line (under GC stress, garbage-fill those, so
 * that a use after a free goes wrong loudly: ASan cannot see inside the
 * chunks; the allocator zeroes them again). */
static bool chunk_free_lines(Chunk *c, bool stress) {
    bool any = false;
    for (uint32_t l = 0; l < c->nlines; l++) {
        if (c->lines[l]) continue;
        any = true;
        if (stress) memset(c->data + ((size_t)l << LINE_SHIFT), 0xDB, LINE);
        else break;
    }
    return any;
}

/* After a collection, the chunks to allocate into, those with free lines.
 * After a minor one, only the chunks allocated into since the last (the
 * first recycle_i) can have changed: they go first, the most recent first,
 * whose memory is the likeliest to be in the caches. A major one may have
 * freed lines anywhere. */
static void recycle_chunks(Process *p, bool major) {
    bool stress = p->vm->gc_stress;
    if (major) {
        p->recycle.n = 0;
        for (Chunk *c = p->chunks; c; c = c->next)
            if (chunk_free_lines(c, stress)) VEC_PUSH(p->recycle, c);
    } else {
        size_t used = p->recycle_i, k = 0;
        for (size_t i = 0, j = used; i + 1 < j; i++, j--) {
            Chunk *t = p->recycle.v[i];
            p->recycle.v[i] = p->recycle.v[j - 1];
            p->recycle.v[j - 1] = t;
        }
        for (size_t i = 0; i < p->recycle.n; i++) {
            Chunk *c = p->recycle.v[i];
            if (i < used && !chunk_free_lines(c, stress)) continue;
            p->recycle.v[k++] = c;
        }
        p->recycle.n = k;
    }
    p->recycle_i = 0;
    p->bump_chunk = NULL;
    p->bump = p->bump_lim = p->hole_end = NULL;
}
/* The roots: what is live whatever the heap holds. */
static void mark_roots(Process *p) {
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
}

static void drain(Process *p) {
    while (p->markstack.n) scan(p, p->markstack.v[--p->markstack.n]);
}

/* ---- checking the write barrier (MLWSVM_GC_VERIFY) ----
 * After a minor collection's marking, a walk of the whole heap from the roots
 * must find no young object that the marking missed: one would be freed
 * while an old object still held it, because a store into that old object
 * did not call the write barrier. The walk's own bit is OBJF_VERIFY. */
static MLWS_THREAD_LOCAL VEC(Obj *) verify_seen, verify_stack; /* (freed after each walk) */

static void verify_reach(Process *p, Obj *o) {
    if (o->flags & (OBJF_VERIFY | OBJF_IMMORTAL)) return;
    if (!(o->flags & OBJF_OLD)) {
        const Obj *by = p->gc_parent;
        fprintf(stderr, "mlwsvm: gc verify: a young %s is held by %s %s that was not remembered (a write barrier is missing)\n",
                obj_kind_name(o->kind), by ? (by->flags & OBJF_OLD ? "an old" : "a young") : "a root, not",
                by ? obj_kind_name(by->kind) : "marked");
        abort();
    }
    o->flags |= OBJF_VERIFY;
    VEC_PUSH(verify_seen, o);
    VEC_PUSH(verify_stack, o);
}

static void gc_verify(Process *p) {
    p->gc_verifying = true;
    p->gc_parent = NULL;
    mark_roots(p);
    while (verify_stack.n) {
        Obj *o = verify_stack.v[--verify_stack.n];
        p->gc_parent = o;
        scan(p, o);
    }
    for (size_t i = 0; i < verify_seen.n; i++) verify_seen.v[i]->flags &= (uint8_t)~OBJF_VERIFY;
    VEC_FREE(verify_seen);
    VEC_FREE(verify_stack);
    p->gc_verifying = false;
}

/* An object a major collection marked (an owner, or one from malloc). */
static bool owner_live(const Process *p, const Obj *o) {
    return (o->flags & (OBJF_OLD | OBJF_MARK)) == (OBJF_OLD | p->mark_bit);
}

/* A minor collection: the young objects reached from the roots and from
 * the remembered old objects become old, and the others are free. Neither the
 * old objects nor the dead young ones are touched, but for the owners. */
static void gc_minor(Process *p) {
    uint64_t old0 = p->old_bytes;
    p->gc_live = p->gc_live_bytes = p->gc_kept_bytes = 0;
    mark_roots(p);
    uint64_t rescanned = 0;
    for (size_t i = 0; i < p->remset.n; i++) {
        Obj *o = p->remset.v[i];
        o->flags &= (uint8_t)~OBJF_REMEMBERED;
        rescanned += obj_footprint(o);
        scan(p, o);
    }
    p->remset.n = 0;
    drain(p);
    if (p->vm->gc_verify) gc_verify(p);
    for (size_t i = 0; i < p->young_owners.n; i++) {
        Obj *o = p->young_owners.v[i];
        if (o->flags & OBJF_OLD) {
            VEC_PUSH(p->old_owners, o);
        } else {
            p->heap_bytes -= obj_footprint(o);
            obj_release_buffers(p, o);
        }
    }
    p->young_owners.n = 0;
    Obj **pp = &p->heap; /* the objects from malloc since the last collection: the list's head */
    while (*pp && *pp != p->heap_old) {
        Obj *o = *pp;
        if (o->flags & OBJF_OLD) {
            pp = &o->next;
        } else {
            *pp = o->next;
            p->heap_bytes -= obj_footprint(o);
            obj_release(p, o);
        }
    }
    p->heap_bytes -= p->young_plain_bytes - p->gc_kept_bytes;
    p->gc_freed_objects += p->objects_allocated - p->gc_allocated0 - p->gc_live;
    p->old_count += p->gc_live;
    p->gc_allocated0 = p->objects_allocated;
    p->young_plain_bytes = 0;
    p->old_bytes += p->gc_live_bytes;
    recycle_chunks(p, false);
    /* More than an eighth of the nursery survived: it is too small for what the
     * program keeps a while, which it promotes (and the next major frees). */
    if ((p->old_bytes - old0) * 8 > p->nursery && p->nursery < GC_NURSERY_MAX) p->nursery *= 2;
    /* Large old objects written now and then (a big map) are rescanned whole
     * at each minor collection: never more often than once per as many bytes
     * allocated, as often as the whole heap was once (at twice its size). */
    uint64_t want = rescanned < p->old_bytes ? rescanned : p->old_bytes;
    if (want > p->nursery) p->nursery = want;
}

/* A major collection: everything, old or young, is marked again from the
 * roots; what survives is old. */
static void gc_major(Process *p) {
    for (size_t i = 0; i < p->remset.n; i++) p->remset.v[i]->flags &= (uint8_t)~OBJF_REMEMBERED;
    p->remset.n = 0;
    p->mark_bit ^= OBJF_MARK;
    for (Chunk *c = p->chunks; c; c = c->next) memset(c->lines, 0, c->nlines);
    p->gc_live = p->gc_live_bytes = p->gc_kept_bytes = 0;
    mark_roots(p);
    drain(p);
    size_t k = 0;
    for (size_t i = 0; i < p->old_owners.n; i++) {
        Obj *o = p->old_owners.v[i];
        if (owner_live(p, o)) p->old_owners.v[k++] = o;
        else obj_release_buffers(p, o);
    }
    p->old_owners.n = k;
    for (size_t i = 0; i < p->young_owners.n; i++) {
        Obj *o = p->young_owners.v[i];
        if (owner_live(p, o)) VEC_PUSH(p->old_owners, o);
        else obj_release_buffers(p, o);
    }
    p->young_owners.n = 0;
    Obj **pp = &p->heap;
    while (*pp) {
        Obj *o = *pp;
        if (owner_live(p, o)) {
            pp = &o->next;
        } else {
            *pp = o->next;
            obj_release(p, o);
        }
    }
    p->gc_freed_objects += p->old_count + (p->objects_allocated - p->gc_allocated0) - p->gc_live;
    p->old_count = p->gc_live;
    p->gc_allocated0 = p->objects_allocated;
    p->young_plain_bytes = 0;
    p->heap_bytes = p->old_bytes = p->gc_live_bytes;
    recycle_chunks(p, true);
    uint64_t min = p->vm->gc_min_threshold ? p->vm->gc_min_threshold : GC_MIN_THRESHOLD;
    p->next_major = p->old_bytes * 2 > 4 * min ? p->old_bytes * 2 : 4 * min;
    p->gc_majors++;
}
/* A minor collection, or a major one when the old objects have doubled since
 * the last (or a quota is near, or gc.collect asked). Under GC stress, every
 * 8th is major. */
void gc_collect(Process *p) {
    uint64_t t0 = plat_now_ns();
    note_peak(p);
    bool major = p->gc_major_req || p->old_bytes >= p->next_major ||
                 (p->vm->gc_stress && p->gc_count % 8 == 7);
    uint64_t q = p->quota[QUOTA_HEAP];
    if (q != UINT64_MAX && p->heap_bytes >= q / 4 * 3) major = true;
    if (major) gc_major(p);
    else gc_minor(p);
    p->heap_old = p->heap;
    p->gc_major_req = false;
    p->next_gc = gc_threshold(p, p->heap_bytes);
    p->gc_pending = false;
    set_alloc_limit(p);
    p->gc_count++;
    p->gc_ns += plat_now_ns() - t0;
}

void gc_chunk_usage(const Process *p, uint64_t *bytes, uint64_t *lines, uint64_t *used) {
    *bytes = p->chunk_bytes;
    *lines = *used = 0;
    for (const Chunk *c = p->chunks; c; c = c->next) {
        *lines += c->nlines;
        for (uint32_t l = 0; l < c->nlines; l++) *used += c->lines[l];
    }
}

void heap_release_foreign(Process *p) {
    for (Obj *o = p->heap; o; o = o->next)
        if (o->kind == OBJ_FOREIGN) foreign_finalize(p, o); /* idempotent: the handle forgets its pointer */
    for (size_t i = 0; i < p->young_owners.n; i++)
        if (p->young_owners.v[i]->kind == OBJ_FOREIGN) foreign_finalize(p, p->young_owners.v[i]);
    for (size_t i = 0; i < p->old_owners.n; i++)
        if (p->old_owners.v[i]->kind == OBJ_FOREIGN) foreign_finalize(p, p->old_owners.v[i]);
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
        obj_release(NULL, o); /* the buffer chunks go below, all at once */
    }
    for (size_t i = 0; i < p->young_owners.n; i++) obj_release_buffers(NULL, p->young_owners.v[i]);
    for (size_t i = 0; i < p->old_owners.n; i++) obj_release_buffers(NULL, p->old_owners.v[i]);
    free_chunks(&p->chunks);
    free_chunks(&p->bchunks);
    memset(p->buf_free, 0, sizeof p->buf_free);
    p->chunk_next = p->bchunk_next = 0;
    p->chunk_bytes = p->heap_bytes = 0;
    p->bump_chunk = NULL;
    p->bump = p->bump_lim = p->hole_end = NULL;
    VEC_FREE(p->recycle);
    p->recycle_i = 0;
    VEC_FREE(p->young_owners);
    VEC_FREE(p->old_owners);
    p->young_plain_bytes = p->old_count = 0;
    p->gc_allocated0 = p->objects_allocated;
    VEC_FREE(p->handles);
    VEC_FREE(p->handle_free);
    VEC_FREE(p->markstack);
    VEC_FREE(p->remset);
    p->heap_old = NULL;
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
