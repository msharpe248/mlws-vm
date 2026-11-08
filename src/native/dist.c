/* dist.c - distribution's C: pids that name a node, values as bytes for the
 * wire, and the challenge that admits a peer. The rest of it, connections and
 * their processes, is MLWSVM code: lib/std/node.masm. What arrives is untrusted
 * until a peer has proved the cookie, and decoding checks everything anyway:
 * every length against what is left, and how deep it goes. */
#include "../core/sha256.h"
#include "../heap/heap.h"
#include "../heap/numeric.h"
#include "../platform/platform.h"

#define MAX_DEPTH 200
#define MAX_NODE 65535

/* ---- values as bytes ---- *
 * A value is its type code, then a word for what is not a reference, or for a
 * reference the object's kind and contents. Lengths are u32, little-endian. */

typedef struct {
    Process *p;
    ObjBytes *out; /* on the heap: a value that cannot be sent traps part way through */
} Writer;

static void put8(Writer *b, uint8_t v) { bytes_append(b->p, b->out, &v, 1); }

static void put32(Writer *b, uint32_t v) {
    uint8_t s[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    bytes_append(b->p, b->out, s, 4);
}

static void put_bytes(Writer *b, const void *s, size_t n) {
    put32(b, (uint32_t)n);
    bytes_append(b->p, b->out, s, n);
}

static void put_str(Writer *b, const char *s) { put_bytes(b, s, strlen(s)); }

static void put_word(Writer *b, Word w) {
    for (int i = 0; i < 8; i++) put8(b, (uint8_t)(w.u >> (8 * i)));
}

static void encode(Process *p, Writer *b, Word w, uint8_t tag, int depth);

/* A closure: 0, its module's name and version's hash, its function's name and
 * signature, and its captured values; or 1 for a module.handle, the module's
 * and function's names and the signature (no version: a handle calls the
 * current one). A stand-in for one of another node (vm_remote_stub) goes as
 * what it stands for. */
static void put_signature(Writer *b, const MFunc *mf) {
    put8(b, mf->nparams);
    for (int i = 0; i < mf->nparams; i++) put8(b, mf->params[i]);
    put8(b, mf->nrets);
    for (int i = 0; i < mf->nrets; i++) put8(b, mf->rets[i]);
}

static void encode_closure(Process *p, Writer *b, const ObjClosure *c, int depth) {
    Function *fn = c->fn->target ? c->fn->target : c->fn;
    LModule *lm = fn->mod;
    if (lm->handle || (lm->remote_mod && lm->remote_handle)) {
        Function *imp = NULL;
        for (uint32_t i = 0; lm->handle && i < lm->m->nfuncs && !imp; i++)
            if (lm->funcs[i].mf->flags & MF_IMPORT) imp = &lm->funcs[i];
        put8(b, 1);
        put_str(b, lm->handle ? lm->deps[0]->m->name : lm->remote_mod);
        put_str(b, lm->handle ? imp->name : lm->remote_fn);
        put_signature(b, fn->mf);
        return;
    }
    put8(b, 0);
    uint8_t h[16];
    if (lm->remote_mod) memcpy(h, lm->remote_hash, 16);
    else vm_module_hash(p->vm, lm, h);
    put_str(b, lm->remote_mod ? lm->remote_mod : lm->m->name);
    bytes_append(b->p, b->out, h, 16);
    put_str(b, lm->remote_mod ? lm->remote_fn : fn->name);
    put_signature(b, fn->mf);
    put32(b, c->nup);
    for (uint32_t i = 0; i < c->nup; i++) encode(p, b, c->up[i], c->tags[i], depth + 1);
}

static void encode_obj(Process *p, Writer *b, const Obj *o, int depth) {
    if (!o) {
        put8(b, OBJ_NULL);
        return;
    }
    if (depth > MAX_DEPTH) vm_trap(p, "E_TYPE", "node: a value nested too deep (or a cycle) cannot be sent to a node");
    put8(b, o->kind);
    switch (o->kind) {
    case OBJ_STRING: put_bytes(b, ((const ObjString *)o)->data, ((const ObjString *)o)->len); break;
    case OBJ_BYTES: put_bytes(b, bytes_data((ObjBytes *)o), ((const ObjBytes *)o)->len); break;
    case OBJ_ARRAY: {
        const ObjArray *x = (const ObjArray *)o;
        put8(b, x->elem);
        put32(b, x->len);
        for (uint32_t i = 0; i < x->len; i++) encode(p, b, x->data[i], x->elem, depth + 1);
        break;
    }
    case OBJ_STRUCT: {
        const ObjStruct *x = (const ObjStruct *)o;
        put_str(b, x->def->module);
        put_str(b, x->def->name);
        put32(b, x->def->nfields);
        for (uint32_t i = 0; i < x->def->nfields; i++) encode(p, b, x->fields[i], x->def->types[i], depth + 1);
        break;
    }
    case OBJ_LIST: {
        const ObjList *x = (const ObjList *)o;
        put32(b, x->len);
        for (uint32_t i = 0; i < x->len; i++) encode(p, b, x->items[i], x->tags[i], depth + 1);
        break;
    }
    case OBJ_MAP: {
        const ObjMap *x = (const ObjMap *)o;
        put32(b, x->count);
        for (uint32_t i = 0; i < x->nentries; i++) {
            const MapEntry *e = &x->entries[i];
            if (!e->live) continue;
            encode(p, b, e->key, e->ktag, depth + 1);
            encode(p, b, e->val, e->vtag, depth + 1);
        }
        break;
    }
    case OBJ_CLOSURE: encode_closure(p, b, (const ObjClosure *)o, depth); break;
    case OBJ_BIG: case OBJ_DEC: {
        StrBuf t = {0};
        num_format(&t, o);
        ObjBytes *text = bytes_new(p, (int64_t)t.n); /* (a heap quota may trap: the text is first in no place to leak) */
        if (t.n) memcpy(text->data, t.s, t.n);
        sb_free(&t);
        put_bytes(b, bytes_data(text), text->len);
        break;
    }
    default: vm_trap(p, "E_TYPE", "node: a %s cannot be sent to a node", obj_kind_name(o->kind));
    }
}

static void encode(Process *p, Writer *b, Word w, uint8_t tag, int depth) {
    if (tag == T_P) vm_trap(p, "E_TYPE", "node: a pointer cannot be sent to a node");
    put8(b, tag);
    if (type_is_ref((MlwsType)tag)) encode_obj(p, b, w.r, depth);
    else put_word(b, w);
}

typedef struct {
    Process *p;
    const uint8_t *s;
    size_t n, i;
} Reader;

static MLWS_NORETURN void bad(Reader *r) { vm_trap(r->p, "E_PARSE", "node: a malformed value at offset %zu", r->i); }

static uint8_t get8(Reader *r) {
    if (r->i >= r->n) bad(r);
    return r->s[r->i++];
}

static uint32_t get32(Reader *r) {
    if (r->n - r->i < 4) bad(r);
    uint32_t v = 0;
    for (int k = 0; k < 4; k++) v |= (uint32_t)r->s[r->i++] << (8 * k);
    return v;
}

/* A length, of things at least `each` bytes long: no more than can be there. */
static uint32_t get_count(Reader *r, size_t each) {
    uint32_t n = get32(r);
    if (n > (r->n - r->i) / each) bad(r);
    return n;
}

static const char *get_span(Reader *r, uint32_t *len) {
    *len = get_count(r, 1);
    const char *s = (const char *)r->s + r->i;
    r->i += *len;
    return s;
}

static const StructDef *find_struct(Process *p, const char *module, const char *name) {
    if (!strcmp(module, "(builtin)")) {
        int b = builtin_struct_find(name);
        return b < 0 ? NULL : &VM_BUILTIN_DEFS[b];
    }
    const LModule *lm = vm_module_named(p->vm, module, p->nframes ? p->frames[p->nframes - 1].fn->mod : NULL);
    for (uint32_t i = 0; lm && i < lm->m->nstructs; i++)
        if (!(lm->m->structs[i].flags & (MSF_IMPORT | MSF_BUILTIN)) && !strcmp(lm->structs[i]->name, name)) return lm->structs[i];
    return NULL;
}

static uint8_t decode(Reader *r, Word *out, int depth);

/* A signature as it was sent: parameters and results, each a type a function
 * may have. */
static int get_types(Reader *r, uint8_t *t) {
    int n = get8(r);
    if (n > 64) bad(r);
    for (int i = 0; i < n; i++) {
        t[i] = get8(r);
        if (t[i] >= T__COUNT || t[i] == T_NONE) bad(r);
    }
    return n;
}

static bool fits(const MFunc *mf, const uint8_t *pt, int np, const uint8_t *rt, int nr) {
    return mf->nparams == np && mf->nrets == nr && !memcmp(mf->params, pt, (size_t)np) && !memcmp(mf->rets, rt, (size_t)nr);
}

static Obj *decode_closure(Reader *r, int depth) {
    Process *p = r->p;
    VM *vm = p->vm;
    uint8_t kind = get8(r), hash[16], pt[64], rt[64];
    if (kind > 1) bad(r);
    uint32_t mn, fnn;
    const char *ms = get_span(r, &mn);
    if (!kind) {
        if (r->n - r->i < 16) bad(r);
        memcpy(hash, r->s + r->i, 16);
        r->i += 16;
    }
    const char *fs = get_span(r, &fnn);
    int np = get_types(r, pt), nr = get_types(r, rt);
    uint32_t nup = kind ? 0 : get_count(r, 2);
    char *mod = xstrndup(ms, mn), *fname = xstrndup(fs, fnn);
    Function *fn = NULL;
    MlwsError err = {{0}, {0}};
    if (kind) { /* a handle: to this node's current version, if it exports the function */
        LModule *lm = vm_module_named(vm, mod, NULL);
        int i = lm ? module_find_export_func(lm->m, fname) : -1;
        if (i >= 0 && !(lm->m->funcs[i].flags & MF_IMPORT) && fits(&lm->m->funcs[i], pt, np, rt, nr))
            fn = vm_handle(vm, lm, fname, &err);
    } else {
        LModule *lm = vm_module_version(vm, mod, hash);
        Function *f = lm ? vm_module_func(lm, fname) : NULL;
        if (f && fits(f->mf, pt, np, rt, nr)) fn = f;
    }
    if (!fn) fn = vm_remote_stub(vm, mod, kind ? NULL : hash, fname, pt, np, rt, nr, &err);
    free(mod);
    free(fname);
    if (!fn) vm_trap(p, "E_TYPE", "node: a closure that cannot be made here: %s", err.msg);
    proc_grow_globals(p); /* (its module may be new to this process) */
    ObjClosure *c = closure_new(p, fn, nup);
    for (uint32_t i = 0; i < nup; i++) c->up[i] = (Word){0}, c->tags[i] = T_I64; /* (until decoded) */
    for (uint32_t i = 0; i < nup; i++) {
        Word w;
        uint8_t t = decode(r, &w, depth + 1);
        c->up[i] = w;
        c->tags[i] = t;
    }
    return &c->h;
}

static Obj *decode_obj(Reader *r, int depth) {
    Process *p = r->p;
    if (depth > MAX_DEPTH) bad(r);
    uint8_t kind = get8(r);
    uint32_t n;
    switch (kind) {
    case OBJ_NULL: return NULL;
    case OBJ_CLOSURE: return decode_closure(r, depth);
    case OBJ_STRING: {
        const char *s = get_span(r, &n);
        return &str_new(p, s, n)->h;
    }
    case OBJ_BYTES: {
        const char *s = get_span(r, &n);
        ObjBytes *b = bytes_new(p, n);
        if (n) memcpy(b->data, s, n);
        return &b->h;
    }
    case OBJ_ARRAY: {
        uint8_t elem = get8(r);
        if (elem >= T__COUNT || elem == T_NONE || elem == T_P) bad(r);
        n = get_count(r, 2);
        ObjArray *a = array_new(p, (MlwsType)elem, n);
        for (uint32_t i = 0; i < n; i++) {
            Word w;
            if (decode(r, &w, depth + 1) != elem) bad(r);
            a->data[i] = w;
        }
        return &a->h;
    }
    case OBJ_STRUCT: {
        uint32_t mn, nn;
        const char *ms = get_span(r, &mn), *ns = get_span(r, &nn);
        char *module = xstrndup(ms, mn), *name = xstrndup(ns, nn);
        const StructDef *d = find_struct(p, module, name);
        if (!d) {
            char msg[200];
            snprintf(msg, sizeof msg, "node: no struct %.80s.%.80s here", module, name);
            free(module);
            free(name);
            vm_trap(p, "E_TYPE", "%s", msg);
        }
        free(module);
        free(name);
        n = get_count(r, 2);
        if (n != d->nfields) bad(r);
        ObjStruct *s = struct_new(p, d);
        for (uint32_t i = 0; i < n; i++) {
            Word w;
            if (decode(r, &w, depth + 1) != d->types[i]) bad(r);
            s->fields[i] = w;
        }
        return &s->h;
    }
    case OBJ_LIST: {
        n = get_count(r, 2);
        ObjList *l = list_new(p, n);
        for (uint32_t i = 0; i < n; i++) {
            Word w;
            uint8_t t = decode(r, &w, depth + 1);
            list_push(p, l, w, t);
        }
        return &l->h;
    }
    case OBJ_MAP: {
        n = get_count(r, 4);
        ObjMap *m = map_new(p);
        for (uint32_t i = 0; i < n; i++) {
            Word k, v;
            uint8_t kt = decode(r, &k, depth + 1), vt = decode(r, &v, depth + 1);
            map_set(p, m, k, kt, v, vt);
        }
        return &m->h;
    }
    case OBJ_BIG: case OBJ_DEC: {
        const char *s = get_span(r, &n);
        Obj *o = num_parse(p, kind == OBJ_BIG ? T_BIG : T_DEC, s, n);
        if (!o) bad(r);
        return o;
    }
    default: bad(r);
    }
}

static uint8_t decode(Reader *r, Word *out, int depth) {
    uint8_t tag = get8(r);
    if (tag == T_NONE || tag >= T__COUNT || tag == T_P) bad(r);
    if (type_is_ref((MlwsType)tag)) {
        out->r = decode_obj(r, depth);
        return tag;
    }
    if (r->n - r->i < 8) bad(r);
    uint64_t v = 0;
    for (int k = 0; k < 8; k++) v |= (uint64_t)r->s[r->i++] << (8 * k);
    out->u = v;
    return tag;
}

/* ---- natives ---- */

static const uint8_t *data_arg(Process *p, Word a, uint32_t *n) {
    const Obj *o = a.r;
    if (o && o->kind == OBJ_STRING) return *n = ((const ObjString *)o)->len, (const uint8_t *)((const ObjString *)o)->data;
    if (o && o->kind == OBJ_BYTES) return *n = ((const ObjBytes *)o)->len, bytes_data((ObjBytes *)o);
    vm_trap(p, "E_TYPE", "node: expected a string or byte buffer");
}

/* Who routes messages between VMs, and what runs for a peer, is not for a
 * sandbox to change. */
static void no_caps(Process *p, const char *fn) {
    if (p->caps) vm_trap(p, "E_PERMISSION", "%s: not allowed in a restricted process", fn);
}

static void n_set_self(Process *p, Word *a, Word *r) {
    (void)r;
    no_caps(p, "node.set_self");
    if (a[0].i < 1 || a[0].i > MAX_NODE) vm_trap(p, "E_RANGE", "node.set_self: a node's number is 1 to %d", MAX_NODE);
    uint32_t none = 0;
    if (!__atomic_compare_exchange_n(&p->vm->node_self, &none, (uint32_t)a[0].i, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) &&
        none != (uint32_t)a[0].i)
        vm_trap(p, "E_EXISTS", "node.set_self: this VM is node %u", none);
}

static void n_self(Process *p, Word *a, Word *r) {
    (void)a;
    r[0].i = __atomic_load_n(&p->vm->node_self, __ATOMIC_ACQUIRE);
}

static void n_global(Process *p, Word *a, Word *r) {
    uint32_t self = __atomic_load_n(&p->vm->node_self, __ATOMIC_ACQUIRE);
    if (!self) vm_trap(p, "E_NODE", "node.global: this VM is no node yet (std.node's start)");
    r[0].u = a[0].u >> NODE_SHIFT ? a[0].u : a[0].u | (uint64_t)self << NODE_SHIFT;
}

static void n_node_of(Process *p, Word *a, Word *r) {
    (void)p;
    r[0].i = (int64_t)(a[0].u >> NODE_SHIFT);
}

static void n_attach(Process *p, Word *a, Word *r) {
    (void)r;
    no_caps(p, "node.attach");
    if (a[0].i < 1 || a[0].i > MAX_NODE) vm_trap(p, "E_RANGE", "node.attach: a node's number is 1 to %d", MAX_NODE);
    VM *vm = p->vm;
    plat_lock(vm->lock);
    if (!vm->node_route) vm->node_route = xcalloc(MAX_NODE + 1, sizeof *vm->node_route);
    plat_unlock(vm->lock);
    uint64_t old = __atomic_exchange_n(&vm->node_route[a[0].i], a[1].u, __ATOMIC_ACQ_REL);
    if (old && !a[1].u) { /* the connection has gone */
        sched_node_down(vm, (uint32_t)a[0].i);
        sched_node_event(vm, (uint32_t)a[0].i, false);
    } else if (!old && a[1].u) {
        sched_node_event(vm, (uint32_t)a[0].i, true);
    }
}

/* ---- names ----
 * A node may have a name besides its number (Erlang's name@host), given once
 * with node.set_name and sent to each node it connects to (std.node's
 * handshake), which keeps it, as the name of that number, after the
 * connection goes. */

#define MAX_NODE_NAME 255

static const char *name_arg(Process *p, Word a, const char *fn, uint32_t *n) {
    const Obj *o = a.r;
    if (!o || o->kind != OBJ_STRING) vm_trap(p, "E_TYPE", "%s: expected a name (a string)", fn);
    const ObjString *s = (const ObjString *)o;
    if (s->len > MAX_NODE_NAME || memchr(s->data, 0, s->len))
        vm_trap(p, "E_RANGE", "%s: a node's name is at most %d bytes, with no NUL", fn, MAX_NODE_NAME);
    *n = s->len;
    return s->data;
}

static bool connected_now(const VM *vm, uint32_t n) {
    return vm->node_route && __atomic_load_n(&vm->node_route[n], __ATOMIC_ACQUIRE);
}

/* The number of the node by this name: this VM's, or a connected node's if
 * one has it, or else the last that had it; 0 if none. (Under vm->lock.) */
static uint32_t number_named(const VM *vm, const char *s, uint32_t len) {
    if (vm->node_name && strlen(vm->node_name) == len && !memcmp(vm->node_name, s, len))
        return __atomic_load_n(&vm->node_self, __ATOMIC_ACQUIRE);
    uint32_t found = 0;
    for (uint32_t n = 1; vm->node_names && n <= MAX_NODE; n++) {
        const char *x = vm->node_names[n];
        if (!x || strlen(x) != len || memcmp(x, s, len)) continue;
        if (connected_now(vm, n)) return n;
        found = n;
    }
    return found;
}

static void n_set_name(Process *p, Word *a, Word *r) {
    (void)r;
    no_caps(p, "node.set_name");
    uint32_t len;
    const char *s = name_arg(p, a[0], "node.set_name", &len);
    if (!len) vm_trap(p, "E_RANGE", "node.set_name: a node's name is not empty");
    VM *vm = p->vm;
    char had[MAX_NODE_NAME + 1] = "";
    plat_lock(vm->lock);
    if (!vm->node_name) vm->node_name = xstrndup(s, len);
    bool ok = strlen(vm->node_name) == len && !memcmp(vm->node_name, s, len); /* (once; again the same is no change) */
    if (!ok) snprintf(had, sizeof had, "%s", vm->node_name);
    plat_unlock(vm->lock);
    if (!ok) vm_trap(p, "E_EXISTS", "node.set_name: this VM is named %s", had);
}

/* A node's name: this VM's for 0 or its own number; "" if it has none. */
static void n_name(Process *p, Word *a, Word *r) {
    if (a[0].i < 0 || a[0].i > MAX_NODE) vm_trap(p, "E_RANGE", "node.name: a node's number is 0 to %d", MAX_NODE);
    VM *vm = p->vm;
    uint32_t n = (uint32_t)a[0].i;
    char s[MAX_NODE_NAME + 1] = "";
    plat_lock(vm->lock);
    const char *x = !n || n == __atomic_load_n(&vm->node_self, __ATOMIC_ACQUIRE) ? vm->node_name
                    : vm->node_names                                         ? vm->node_names[n]
                                                                             : NULL;
    if (x) snprintf(s, sizeof s, "%s", x);
    plat_unlock(vm->lock);
    r[0].r = &str_new(p, s, strlen(s))->h;
}

static void n_number(Process *p, Word *a, Word *r) {
    uint32_t len;
    const char *s = name_arg(p, a[0], "node.number", &len);
    plat_lock(p->vm->lock);
    r[0].i = len ? number_named(p->vm, s, len) : 0;
    plat_unlock(p->vm->lock);
}

/* For std.node's handshake: node n calls itself this. False, keeping nothing,
 * if another node connected now, or this VM, has the name. */
static void n_named(Process *p, Word *a, Word *r) {
    no_caps(p, "node.named");
    if (a[0].i < 1 || a[0].i > MAX_NODE) vm_trap(p, "E_RANGE", "node.named: a node's number is 1 to %d", MAX_NODE);
    uint32_t len, n = (uint32_t)a[0].i;
    const char *s = name_arg(p, a[1], "node.named", &len);
    VM *vm = p->vm;
    plat_lock(vm->lock);
    uint32_t has = len ? number_named(vm, s, len) : 0;
    bool ok = !has || has == n || (has != __atomic_load_n(&vm->node_self, __ATOMIC_ACQUIRE) && !connected_now(vm, has));
    if (vm->node_name && strlen(vm->node_name) == len && !memcmp(vm->node_name, s, len)) ok = false;
    if (ok) {
        if (!vm->node_names) vm->node_names = xcalloc(MAX_NODE + 1, sizeof *vm->node_names);
        free(vm->node_names[n]);
        vm->node_names[n] = len ? xstrndup(s, len) : NULL;
    }
    plat_unlock(vm->lock);
    r[0].i = ok;
}

/* node.watch(on): whether this process is told, by a NodeEvent, of each
 * connection to another node that comes or goes. */
static void n_watch(Process *p, Word *a, Word *r) {
    (void)r;
    VM *vm = p->vm;
    plat_lock(vm->lock);
    bool had = false;
    for (size_t i = 0; i < vm->node_watchers.n && !had; i++) had = vm->node_watchers.v[i] == p->pid;
    if (a[0].i && !had) VEC_PUSH(vm->node_watchers, p->pid);
    if (!a[0].i && had)
        for (size_t i = 0; i < vm->node_watchers.n; i++)
            if (vm->node_watchers.v[i] == p->pid) vm->node_watchers.v[i] = vm->node_watchers.v[--vm->node_watchers.n];
    plat_unlock(vm->lock);
}

/* The numbers of the nodes connected now, in order. */
static void n_connected_nodes(Process *p, Word *a, Word *r) {
    (void)a;
    VM *vm = p->vm;
    uint32_t count = 0;
    for (uint32_t n = 1; vm->node_route && n <= MAX_NODE; n++) count += connected_now(vm, n);
    ObjList *l = list_new(p, count);
    for (uint32_t n = 1; vm->node_route && n <= MAX_NODE && l->len < count; n++)
        if (connected_now(vm, n)) list_push(p, l, (Word){.i = n}, T_I64);
    r[0].r = &l->h;
}

static void n_route(Process *p, Word *a, Word *r) {
    if (a[0].i < 1 || a[0].i > MAX_NODE) vm_trap(p, "E_RANGE", "node.route: a node's number is 1 to %d", MAX_NODE);
    r[0].u = p->vm->node_route ? __atomic_load_n(&p->vm->node_route[a[0].i], __ATOMIC_ACQUIRE) : 0;
}

/* A frame for the wire: its length, then the value. */
static void n_frame(Process *p, Word *a, Word *r) {
    Writer b = {p, bytes_new(p, 0)};
    put32(&b, 0);
    encode(p, &b, a[0], T_R, 0);
    uint32_t len = b.out->len - 4;
    for (int i = 0; i < 4; i++) b.out->data[i] = (uint8_t)(len >> (8 * i));
    r[0].r = &b.out->h;
}

/* A closure for an exported function of a module, as a peer names it: the one
 * loaded under that name, whoever loaded it, or else from the library path. */
static void n_function(Process *p, Word *a, Word *r) {
    no_caps(p, "node.function");
    const Obj *mo = a[0].r, *fo = a[1].r;
    if (!mo || mo->kind != OBJ_STRING || !fo || fo->kind != OBJ_STRING) vm_trap(p, "E_TYPE", "node.function: a module and a function");
    const char *name = ((const ObjString *)mo)->data, *fname = ((const ObjString *)fo)->data;
    MlwsError err = {{0}, {0}};
    LModule *lm = vm_module_named(p->vm, name, NULL);
    if (!lm) lm = vm_load_dynamic(p->vm, name, NULL, &err);
    if (!lm) vm_trap(p, err.code, "%s", err.msg);
    proc_grow_globals(p);
    int i = module_find_export_func(lm->m, fname);
    if (i < 0 || (lm->m->funcs[i].flags & MF_IMPORT)) vm_trap(p, "E_LINK_UNDEF", "node.function: module %s does not export '%s'", name, fname);
    r[0].r = &closure_new(p, &lm->funcs[i], 0)->h;
}

/* A frame of a list and one more value, typed: a message, [0, pid] and [pid, value]. */
static void n_frame_value(Process *p, Word *a, Word *r) {
    ObjList *head = a[0].r;
    const ObjList *m = a[1].r;
    if (!head || head->h.kind != OBJ_LIST || !m || m->h.kind != OBJ_LIST || m->len != 2)
        vm_trap(p, "E_TYPE", "node.frame_value: expected a list and [pid, value]");
    list_push(p, head, m->items[1], m->tags[1]);
    n_frame(p, a, r);
}

static void n_decode(Process *p, Word *a, Word *r) {
    uint32_t n;
    const uint8_t *s = data_arg(p, a[0], &n);
    Reader rd = {p, s, n, 0};
    Word w;
    uint8_t t = decode(&rd, &w, 0);
    if (t != T_R || rd.i != n) bad(&rd);
    r[0] = w;
}

/* Give the message a frame carries, [to, value], to its process here. */
static void n_deliver(Process *p, Word *a, Word *r) {
    (void)r;
    const ObjList *l = a[0].r;
    if (!l || l->h.kind != OBJ_LIST || l->len != 2 || !type_is_int((MlwsType)l->tags[0]))
        vm_trap(p, "E_PARSE", "node.deliver: expected [pid, value]");
    sched_node_deliver(p, l->items[0].u, l->items[1], l->tags[1]);
}

/* A control frame from a node, for link, monitor, proc.call and kill across nodes:
 * [kind, pid, ...] (sched.c, "across nodes"). */
static void n_control(Process *p, Word *a, Word *r) {
    (void)r;
    no_caps(p, "node.control");
    sched_node_control(p, a[0].r);
}

/* HMAC-SHA-256 of a role and a nonce: the proof that the prover knows the
 * cookie, for this connection (the nonce) and this end of it (the role, so that
 * one end's proof cannot be sent back to it). */
static void proof(const uint8_t *key, uint32_t kn, const uint8_t *role, uint32_t rn, const uint8_t *nonce, uint32_t nn, uint8_t out[32]) {
    uint8_t k[64] = {0}, pad[64], inner[32];
    Sha256 h;
    if (kn > 64) {
        sha256_init(&h);
        sha256_update(&h, key, kn);
        sha256_final(&h, k);
    } else {
        memcpy(k, key, kn);
    }
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    sha256_init(&h);
    sha256_update(&h, pad, 64);
    sha256_update(&h, role, rn);
    sha256_update(&h, nonce, nn);
    sha256_final(&h, inner);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    sha256_init(&h);
    sha256_update(&h, pad, 64);
    sha256_update(&h, inner, 32);
    sha256_final(&h, out);
}

static void n_proof(Process *p, Word *a, Word *r) {
    uint32_t kn, rn, nn;
    const uint8_t *k = data_arg(p, a[0], &kn), *role = data_arg(p, a[1], &rn), *nonce = data_arg(p, a[2], &nn);
    ObjBytes *o = bytes_new(p, 32);
    proof(k, kn, role, rn, nonce, nn, o->data);
    r[0].r = &o->h;
}

/* Whether a proof is right, in time that does not say how nearly. */
static void n_proof_ok(Process *p, Word *a, Word *r) {
    uint32_t kn, rn, nn, pn;
    const uint8_t *k = data_arg(p, a[0], &kn), *role = data_arg(p, a[1], &rn), *nonce = data_arg(p, a[2], &nn), *given = data_arg(p, a[3], &pn);
    uint8_t want[32], diff = pn != 32;
    proof(k, kn, role, rn, nonce, nn, want);
    for (int i = 0; i < 32 && pn == 32; i++) diff |= want[i] ^ given[i];
    r[0].i = !diff;
}

#define N(name, np, pt, nr, rt, fn, doc) {name, np, pt, nr, rt, fn, doc, 0, NULL, NULL, NULL}
#define T(...) {__VA_ARGS__}
static const NativeDef NODE_NATIVES[] = {
    N("node.set_self", 1, T(T_I64), 0, T(0), n_set_self, "Give this VM its node number, 1 to 65535: once, and unique in its cluster."),
    N("node.self", 0, T(0), 1, T(T_I64), n_self, "This VM's node number, or 0."),
    N("node.global", 1, T(T_I64), 1, T(T_I64), n_global, "A pid as other nodes can use it, naming this node."),
    N("node.of", 1, T(T_I64), 1, T(T_I64), n_node_of, "The node a pid names, or 0 for a plain local one."),
    N("node.attach", 2, T(T_I64, T_I64), 0, T(0), n_attach, "Messages for a node go to this process, as [pid, value] (0: nowhere). For std.node."),
    N("node.set_name", 1, T(T_R), 0, T(0), n_set_name,
      "Give this VM its name among nodes (as Erlang's name@host): once, not empty, at most 255 bytes. Sent to each node it connects to."),
    N("node.name", 1, T(T_I64), 1, T(T_R), n_name,
      "A node's name, as it gave it when it last connected; this VM's for 0 or its own number; \"\" if none is known."),
    N("node.number", 1, T(T_R), 1, T(T_I64), n_number,
      "The number of the node with this name: this VM's, a connected node's, or else the last node that had it; 0 if none."),
    N("node.named", 2, T(T_I64, T_R), 1, T(T_B), n_named,
      "Record the name a node gave; false if this VM or another connected node has it. For std.node."),
    N("node.watch", 1, T(T_B), 0, T(0), n_watch,
      "Whether this process gets a NodeEvent {node, name, up} for each connection to another node that comes (up) or goes, from now on."),
    N("node.nodes", 0, T(0), 1, T(T_R), n_connected_nodes, "The numbers of the nodes connected now, as a list, in order."),
    N("node.route", 1, T(T_I64), 1, T(T_I64), n_route, "The process messages for a node go to, or 0. For std.node."),
    N("node.function", 2, T(T_R, T_R), 1, T(T_R), n_function, "A closure for a module's exported function, as a peer names it. For std.node."),
    N("node.frame", 1, T(T_R), 1, T(T_R), n_frame, "A value as bytes for a connection, with its length. For std.node."),
    N("node.frame_value", 2, T(T_R, T_R), 1, T(T_R), n_frame_value, "A frame of a list with the second item of [pid, value] added. For std.node."),
    N("node.decode", 1, T(T_R), 1, T(T_R), n_decode, "A value from a frame's bytes. Untrusted bytes raise E_PARSE, never worse. For std.node."),
    N("node.deliver", 1, T(T_R), 0, T(0), n_deliver, "Send [pid, value] to its process here. For std.node."),
    N("node.control", 1, T(T_R), 0, T(0), n_control,
      "Act on a control frame from a node: link, unlink, exit, monitor, down, reply, noproc or kill. For std.node."),
    N("node.proof", 3, T(T_R, T_R, T_R), 1, T(T_R), n_proof, "HMAC-SHA-256 of a role and a nonce, keyed by a cookie. For std.node."),
    N("node.proof_ok", 4, T(T_R, T_R, T_R, T_R), 1, T(T_B), n_proof_ok, "Whether a proof is right, in constant time. For std.node."),
};
#undef N
#undef T

void natives_register_node(VM *vm) {
    for (size_t i = 0; i < sizeof NODE_NATIVES / sizeof NODE_NATIVES[0]; i++) vm_register_native(vm, &NODE_NATIVES[i]);
}

const NativeDef *natives_node_table(size_t *n) {
    *n = sizeof NODE_NATIVES / sizeof NODE_NATIVES[0];
    return NODE_NATIVES;
}
