/* copy.c - deep copy of a value from one process's heap into another's, used
 * for messages and spawn arguments. The object graph is copied preserving
 * sharing and cycles (an old->new pointer map) without recursion (a worklist).
 * Constant-pool strings are immortal and shared rather than copied. Raw
 * pointers are process-local, so meeting one traps E_TYPE in the sender.
 *
 * The map and worklist are per-thread scratch, not on the C stack, so a trap
 * partway through leaks nothing: the half-built copy is unreachable garbage in
 * the target heap. */
#include "heap.h"
#include "numeric.h"

typedef struct {
    Obj *old, *copy;
} CopyPair;

struct CopyState {
    CopyPair *slots; /* open-addressing map old -> copy */
    size_t cap, n;
    VEC(CopyPair) work;
};

static MLWS_THREAD_LOCAL struct CopyState *tls_copy;

static uint32_t ptr_hash(const void *p) {
    uint64_t x = (uint64_t)(uintptr_t)p;
    x ^= x >> 29;
    x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 32;
    return (uint32_t)x;
}

static Obj *map_lookup(struct CopyState *cs, Obj *o) {
    if (!cs->cap) return NULL;
    for (size_t i = ptr_hash(o) & (cs->cap - 1);; i = (i + 1) & (cs->cap - 1)) {
        if (!cs->slots[i].old) return NULL;
        if (cs->slots[i].old == o) return cs->slots[i].copy;
    }
}

static void map_insert(struct CopyState *cs, Obj *o, Obj *c) {
    if ((cs->n + 1) * 2 > cs->cap) {
        size_t nc = cs->cap ? cs->cap * 2 : 64;
        CopyPair *ns = xcalloc(nc, sizeof *ns);
        for (size_t i = 0; i < cs->cap; i++) {
            if (!cs->slots[i].old) continue;
            size_t j = ptr_hash(cs->slots[i].old) & (nc - 1);
            while (ns[j].old) j = (j + 1) & (nc - 1);
            ns[j] = cs->slots[i];
        }
        free(cs->slots);
        cs->slots = ns;
        cs->cap = nc;
    }
    size_t j = ptr_hash(o) & (cs->cap - 1);
    while (cs->slots[j].old) j = (j + 1) & (cs->cap - 1);
    cs->slots[j].old = o;
    cs->slots[j].copy = c;
    cs->n++;
}

static void no_pointer(Process *from, uint8_t tag) {
    if (tag == T_P) vm_trap(from, "E_TYPE", "pointers cannot be sent to another process (they are process-local)");
}

/* Allocate a copy of o without its references (they are fixed up from the worklist). */
static Obj *shallow(Process *from, Process *to, struct CopyState *cs, Obj *o) {
    if (!o || (o->flags & OBJF_IMMORTAL)) return o;
    Obj *c = map_lookup(cs, o);
    if (c) return c;
    switch (o->kind) {
    case OBJ_STRING: {
        ObjString *s = (ObjString *)o;
        c = &str_new(to, s->data, s->len)->h;
        break;
    }
    case OBJ_BYTES: {
        ObjBytes *b = (ObjBytes *)o, *nb = bytes_new(to, b->len);
        if (b->len) memcpy(nb->data, b->data, b->len);
        c = &nb->h;
        break;
    }
    case OBJ_ARRAY: {
        ObjArray *a = (ObjArray *)o;
        no_pointer(from, a->elem);
        ObjArray *na = array_new(to, (MlwsType)a->elem, a->len);
        if (a->elem != T_R) memcpy(na->data, a->data, a->len * sizeof(Word));
        c = &na->h;
        break;
    }
    case OBJ_STRUCT: {
        ObjStruct *s = (ObjStruct *)o, *ns = struct_new(to, s->def);
        for (uint32_t i = 0; i < s->def->nfields; i++) {
            no_pointer(from, s->def->types[i]);
            if (!type_is_ref(s->def->types[i])) ns->fields[i] = s->fields[i];
        }
        c = &ns->h;
        break;
    }
    case OBJ_LIST: {
        ObjList *l = (ObjList *)o, *nl = list_new(to, l->len);
        for (uint32_t i = 0; i < l->len; i++) {
            no_pointer(from, l->tags[i]);
            Word w = l->tags[i] == T_R ? (Word){.u = 0} : l->items[i];
            list_push(to, nl, w, l->tags[i]);
        }
        c = &nl->h;
        break;
    }
    case OBJ_MAP: c = &map_new(to)->h; break; /* entries are added from the worklist */
    case OBJ_CLOSURE: {
        ObjClosure *cl = (ObjClosure *)o, *nc = closure_new(to, cl->fn, cl->nup);
        for (uint32_t i = 0; i < cl->nup; i++) {
            no_pointer(from, cl->tags[i]);
            nc->tags[i] = cl->tags[i];
            if (cl->tags[i] != T_R) nc->up[i] = cl->up[i];
        }
        c = &nc->h;
        break;
    }
    case OBJ_BIG: case OBJ_DEC: { /* immutable, but heaps never share objects */
        size_t size = num_footprint(o);
        c = gc_alloc_unaccounted(to, o->kind, size);
        memcpy((char *)c + sizeof(Obj), (const char *)o + sizeof(Obj), size - sizeof(Obj));
        gc_account(to, (int64_t)size);
        break;
    }
    case OBJ_FOREIGN:
        vm_trap(from, "E_TYPE", "foreign handles cannot be sent to another process (they are process-local)");
    case OBJ_CORO:
        vm_trap(from, "E_TYPE", "coroutines cannot be sent to another process (they are process-local)");
    default: vm_trap(from, "E_INTERNAL", "cannot copy object kind %d", o->kind);
    }
    map_insert(cs, o, c);
    CopyPair pr = {o, c};
    VEC_PUSH(cs->work, pr);
    return c;
}

static Word copy_slot(Process *from, Process *to, struct CopyState *cs, Word v, uint8_t tag) {
    no_pointer(from, tag);
    if (type_is_ref(tag)) v.r = shallow(from, to, cs, v.r);
    return v;
}

Word msg_copy(Process *from, Process *to, Word v, uint8_t tag) {
    no_pointer(from, tag);
    if (!type_is_ref(tag) || !v.r) return v;
    if (!tls_copy) tls_copy = xcalloc(1, sizeof(struct CopyState));
    struct CopyState *cs = tls_copy;
    if (cs->cap) memset(cs->slots, 0, cs->cap * sizeof *cs->slots);
    cs->n = 0;
    cs->work.n = 0;
    Word out;
    out.r = shallow(from, to, cs, v.r);
    while (cs->work.n) {
        CopyPair pr = cs->work.v[--cs->work.n];
        switch (pr.old->kind) {
        case OBJ_ARRAY: {
            ObjArray *a = (ObjArray *)pr.old, *na = (ObjArray *)pr.copy;
            if (a->elem == T_R)
                for (uint32_t i = 0; i < a->len; i++) na->data[i].r = shallow(from, to, cs, a->data[i].r);
            break;
        }
        case OBJ_STRUCT: {
            ObjStruct *s = (ObjStruct *)pr.old, *ns = (ObjStruct *)pr.copy;
            for (uint32_t i = 0; i < s->def->nfields; i++)
                if (type_is_ref(s->def->types[i])) ns->fields[i].r = shallow(from, to, cs, s->fields[i].r);
            break;
        }
        case OBJ_LIST: {
            ObjList *l = (ObjList *)pr.old, *nl = (ObjList *)pr.copy;
            for (uint32_t i = 0; i < l->len; i++)
                if (l->tags[i] == T_R) nl->items[i].r = shallow(from, to, cs, l->items[i].r);
            break;
        }
        case OBJ_CLOSURE: {
            ObjClosure *cl = (ObjClosure *)pr.old, *nc = (ObjClosure *)pr.copy;
            for (uint32_t i = 0; i < cl->nup; i++)
                if (cl->tags[i] == T_R) nc->up[i].r = shallow(from, to, cs, cl->up[i].r);
            break;
        }
        case OBJ_MAP: {
            ObjMap *m = (ObjMap *)pr.old, *nm = (ObjMap *)pr.copy;
            for (uint32_t i = 0; i < m->nentries; i++) {
                MapEntry *e = &m->entries[i];
                if (!e->live) continue;
                Word k = copy_slot(from, to, cs, e->key, e->ktag);
                Word val = copy_slot(from, to, cs, e->val, e->vtag);
                map_set(to, nm, k, e->ktag, val, e->vtag);
            }
            break;
        }
        default: break;
        }
    }
    return out;
}

void copy_state_free(void) {
    struct CopyState *cs = tls_copy;
    if (!cs) return;
    free(cs->slots);
    VEC_FREE(cs->work);
    free(cs);
    tls_copy = NULL;
}
