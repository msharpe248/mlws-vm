/* dist.c - distribution's C: pids that name a node, values as bytes for the
 * wire, and the challenge that admits a peer. The rest of it, connections and
 * their processes, is MLWSVM code: lib/std/node.masm. What arrives is untrusted
 * until a peer has proved the cookie, and decoding checks everything anyway:
 * every length against what is left, and every reference against what has
 * been made. */
#include "../core/sha256.h"
#include "../heap/heap.h"
#include "../heap/numeric.h"
#include "../platform/platform.h"

/* A string the sender has interned (a literal, or text.intern's): a code of
 * its own beside the object kinds, so that it is interned where it arrives
 * too, the same object there as that VM's literal of it. */
#define WIRE_INTERNED 0xF0
/* The same for a BigInt and a decimal literal (vm_intern_num). */
#define WIRE_INTERNED_BIG 0xF1
#define WIRE_INTERNED_DEC 0xF2
/* An object met before in this value: its number (below). */
#define WIRE_REF 0xF3
#define MAX_NODE 65535

/* ---- values as bytes ---- *
 * A value is its type code, then a word for what is not a reference, or for a
 * reference the object's kind and contents. Lengths are u32, little-endian.
 *
 * Every object written whole is numbered, from 0, in the order it is first
 * written; met again, it is WIRE_REF and that number. So sharing and cycles
 * cross as a send within one VM keeps them. Both ends walk the value with a
 * stack of their own, not C's, so a value as deep as a long list of cells
 * crosses too. The tables are this thread's scratch, as copy.c's are, so a
 * trap part way through leaks nothing. */

typedef struct {
    const Obj *o;
    uint32_t idx, gen; /* (a slot of another value's gen is empty) */
} Seen;

typedef struct {
    const Obj *o;
    uint32_t i, n; /* the next of its n values to write */
} EFrame;

typedef struct {
    Obj *o;
    uint32_t i, n;
    Word key; /* a map's: the key read, until its value is */
    uint8_t ktag;
} DFrame;

typedef struct {
    Seen *seen;
    uint32_t cap, n, gen;
    VEC(EFrame) ef;
    VEC(DFrame) df;
    VEC(Obj *) objs;
} Scratch;

static MLWS_THREAD_LOCAL Scratch *tls_wire;

static Scratch *scratch(void) {
    if (!tls_wire) tls_wire = xcalloc(1, sizeof *tls_wire);
    return tls_wire;
}

void node_wire_free(void) {
    Scratch *s = tls_wire;
    if (!s) return;
    free(s->seen);
    VEC_FREE(s->ef);
    VEC_FREE(s->df);
    VEC_FREE(s->objs);
    free(s);
    tls_wire = NULL;
}

static uint32_t ptr_hash(const void *p) {
    uint64_t x = (uint64_t)(uintptr_t)p;
    x ^= x >> 29;
    x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 32;
    return (uint32_t)x;
}

/* An object's number if it has been written, or -1 after giving it the next. */
static int64_t seen_or_add(Scratch *s, const Obj *o) {
    if ((s->n + 1) * 2 > s->cap) {
        uint32_t nc = s->cap ? s->cap * 2 : 64;
        Seen *ns = xcalloc(nc, sizeof *ns);
        for (uint32_t i = 0; i < s->cap; i++) {
            if (s->seen[i].gen != s->gen) continue;
            uint32_t j = ptr_hash(s->seen[i].o) & (nc - 1);
            while (ns[j].gen == s->gen) j = (j + 1) & (nc - 1);
            ns[j] = s->seen[i];
        }
        free(s->seen);
        s->seen = ns;
        s->cap = nc;
    }
    for (uint32_t j = ptr_hash(o) & (s->cap - 1);; j = (j + 1) & (s->cap - 1)) {
        Seen *e = &s->seen[j];
        if (e->gen != s->gen) {
            *e = (Seen){o, s->n++, s->gen};
            return -1;
        }
        if (e->o == o) return e->idx;
    }
}

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
    uint8_t s[8];
    for (int i = 0; i < 8; i++) s[i] = (uint8_t)(w.u >> (8 * i));
    bytes_append(b->p, b->out, s, 8);
}

/* A closure: 0, its module's name and version's hash, its function's name and
 * signature, and the number of its captured values (which follow, as its
 * contents); or 1 for a module.handle, the module's and function's names and
 * the signature (no version: a handle calls the current one). A stand-in for
 * one of another node (vm_remote_stub) goes as what it stands for. */
static void put_signature(Writer *b, const MFunc *mf) {
    put8(b, mf->nparams);
    for (int i = 0; i < mf->nparams; i++) put8(b, mf->params[i]);
    put8(b, mf->nrets);
    for (int i = 0; i < mf->nrets; i++) put8(b, mf->rets[i]);
}

static uint32_t put_closure(Process *p, Writer *b, const ObjClosure *c) {
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
        return 0;
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
    return c->nup;
}

/* One value: its type code and word, or its object: null, a literal, one met
 * before, or its kind and what is not its values, which the caller writes
 * next from the frame pushed for it. */
static void put_value(Process *p, Writer *b, Scratch *s, Word w, uint8_t tag) {
    if (tag == T_P) vm_trap(p, "E_TYPE", "node: a pointer cannot be sent to a node");
    put8(b, tag);
    if (!type_is_ref((MlwsType)tag)) {
        put_word(b, w);
        return;
    }
    const Obj *o = w.r;
    if (!o) {
        put8(b, OBJ_NULL);
        return;
    }
    if (o->kind == OBJ_STRING && (o->flags & OBJF_IMMORTAL)) {
        put8(b, WIRE_INTERNED);
        put_bytes(b, ((const ObjString *)o)->data, ((const ObjString *)o)->len);
        return;
    }
    /* A BigInt or decimal literal goes as one, to be that node's literal of the
     * value; any other, whole. */
    bool num = o->kind == OBJ_BIG || o->kind == OBJ_DEC, lit = num && (o->flags & OBJF_IMMORTAL);
    if (!lit) {
        int64_t k = seen_or_add(s, o);
        if (k >= 0) {
            put8(b, WIRE_REF);
            put32(b, (uint32_t)k);
            return;
        }
    }
    put8(b, lit ? (o->kind == OBJ_BIG ? WIRE_INTERNED_BIG : WIRE_INTERNED_DEC) : o->kind);
    uint32_t n = 0;
    switch (o->kind) {
    case OBJ_STRING: put_bytes(b, ((const ObjString *)o)->data, ((const ObjString *)o)->len); break;
    case OBJ_BYTES: put_bytes(b, bytes_data((ObjBytes *)o), ((const ObjBytes *)o)->len); break;
    case OBJ_ARRAY: {
        const ObjArray *x = (const ObjArray *)o;
        if (x->elem == T_P) vm_trap(p, "E_TYPE", "node: a pointer cannot be sent to a node");
        put8(b, x->elem);
        put32(b, x->len);
        n = x->len;
        break;
    }
    case OBJ_STRUCT: {
        const ObjStruct *x = (const ObjStruct *)o;
        put_str(b, x->def->module);
        put_str(b, x->def->name);
        put32(b, x->def->nfields);
        n = x->def->nfields;
        break;
    }
    case OBJ_LIST:
        put32(b, ((const ObjList *)o)->len);
        n = ((const ObjList *)o)->len;
        break;
    case OBJ_MAP:
        put32(b, ((const ObjMap *)o)->count);
        n = 2 * ((const ObjMap *)o)->nentries; /* (a key and a value of each entry, the dead skipped) */
        break;
    case OBJ_CLOSURE: n = put_closure(p, b, (const ObjClosure *)o); break;
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
    if (n) VEC_PUSH(s->ef, ((EFrame){o, 0, n}));
}

/* The values of the objects put_value began, depth first, as they come. */
static void put_contents(Process *p, Writer *b, Scratch *s) {
    while (s->ef.n) {
        EFrame *f = &s->ef.v[s->ef.n - 1];
        if (f->i >= f->n) {
            s->ef.n--;
            continue;
        }
        const Obj *o = f->o;
        uint32_t i = f->i++;
        Word cw;
        uint8_t ct;
        switch (o->kind) {
        case OBJ_ARRAY: cw = ((const ObjArray *)o)->data[i], ct = ((const ObjArray *)o)->elem; break;
        case OBJ_STRUCT: cw = ((const ObjStruct *)o)->fields[i], ct = ((const ObjStruct *)o)->def->types[i]; break;
        case OBJ_LIST: cw = ((const ObjList *)o)->items[i], ct = ((const ObjList *)o)->tags[i]; break;
        case OBJ_CLOSURE: cw = ((const ObjClosure *)o)->up[i], ct = ((const ObjClosure *)o)->tags[i]; break;
        default: { /* OBJ_MAP */
            const MapEntry *e = &((const ObjMap *)o)->entries[i / 2];
            if (!e->live) {
                f->i++; /* (its value too) */
                continue;
            }
            if (i % 2) cw = e->val, ct = e->vtag;
            else cw = e->key, ct = e->ktag;
        }
        }
        put_value(p, b, s, cw, ct); /* (f may move) */
    }
}

/* Values one after another, numbered from `first` (what the frame already has
 * that its reader numbers: a list they are in, below). */
static void encode(Process *p, Writer *b, uint32_t nv, const Word *vs, const uint8_t *tags, uint32_t first) {
    Scratch *s = scratch();
    s->ef.n = 0;
    s->n = first;
    if (++s->gen == 0) { /* (wrapped: every slot's gen is old again) */
        memset(s->seen, 0, s->cap * sizeof *s->seen);
        s->gen = 1;
    }
    for (uint32_t k = 0; k < nv; k++) {
        put_value(p, b, s, vs[k], tags[k]);
        put_contents(p, b, s);
    }
}

/* ---- frames ---- *
 * A frame is its length, u32, then a value: a list, [0, pid, value] for a
 * message, else [kind, ...]. A message's and the VM's own are made where they
 * are sent (sched.c), in the sending thread's scratch heap, so that the
 * process that carries the connection only writes them, and what cannot be
 * sent is the sender's error. */

#define MAX_FRAME (64u << 20) /* what net.read_frame takes */

static void frame_list(Writer *b, uint32_t n) {
    put32(b, 0);
    put8(b, T_R);
    put8(b, OBJ_LIST);
    put32(b, n);
}

static ObjBytes *frame_end(Process *p, Writer *b) {
    size_t len = b->out->len - 4;
    if (len > MAX_FRAME)
        vm_trap(p, "E_RANGE", "node: a message of %zu bytes is more than a frame to a node holds (64 MiB)", len);
    for (int i = 0; i < 4; i++) b->out->data[i] = (uint8_t)(len >> (8 * i));
    return b->out;
}

/* [0, pid, value], in f; p's error if the value cannot be sent. */
ObjBytes *node_message_frame(Process *p, Process *f, uint64_t pid, Word v, uint8_t tag) {
    Writer b = {f, bytes_new(f, 0)};
    frame_list(&b, 3);
    Word w[3] = {{.i = 0}, {.u = pid}, v};
    uint8_t t[3] = {T_I64, T_I64, tag};
    encode(p, &b, 3, w, t, 1);
    return frame_end(p, &b);
}

/* [12, name, value], in f: a message for whichever process has the name where
 * it arrives (node.take resolves it, as Erlang's REG_SEND). The name is
 * written as a literal, which the reader reads without interning. */
ObjBytes *node_named_frame(Process *p, Process *f, const char *name, Word v, uint8_t tag) {
    Writer b = {f, bytes_new(f, 0)};
    frame_list(&b, 3);
    put8(&b, T_I64);
    put_word(&b, (Word){.i = 12});
    put8(&b, T_R);
    put8(&b, WIRE_INTERNED);
    put_str(&b, name);
    encode(p, &b, 1, &v, &tag, 1);
    return frame_end(p, &b);
}

/* A value as bytes, without a frame's length, in f's heap (env.c). */
ObjBytes *node_encode_value(Process *p, Process *f, Word v, uint8_t tag) {
    Writer b = {f, bytes_new(f, 0)};
    encode(p, &b, 1, &v, &tag, 0);
    return b.out;
}

/* [kind, w...], in f; p's error (or f's, for what the VM sends, which is
 * always sendable). */
ObjBytes *node_control_frame(Process *p, Process *f, int kind, int n, const Word *w, const uint8_t *t) {
    Writer b = {f, bytes_new(f, 0)};
    frame_list(&b, (uint32_t)n + 1);
    put8(&b, T_I64);
    put_word(&b, (Word){.i = kind});
    encode(p, &b, (uint32_t)n, w, t, 1);
    return frame_end(p, &b);
}

typedef struct {
    Process *p;   /* where the value is made */
    Process *who; /* whose error it is when it cannot be (the reader's) */
    const uint8_t *s;
    size_t n, i;
} Reader;

static MLWS_NORETURN void bad(Reader *r) { vm_trap(r->who, "E_PARSE", "node: a malformed value at offset %zu", r->i); }

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
    /* (As from outside any program, and loaded if need be: the decoding runs
     * in std.node, whose home is the library path, and a frame names a module
     * of the sending program.) */
    MlwsError err;
    const LModule *lm = vm_module_for_node(p->vm, module, &err);
    for (uint32_t i = 0; lm && i < lm->m->nstructs; i++)
        if (!(lm->m->structs[i].flags & (MSF_IMPORT | MSF_BUILTIN)) && !strcmp(lm->structs[i]->name, name)) return lm->structs[i];
    return NULL;
}

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

/* A closure, its captured values still to come. */
static ObjClosure *get_closure(Reader *r) {
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
        LModule *lm = vm_module_for_node(vm, mod, &err);
        int i = lm ? module_find_export_func(lm->m, fname) : -1;
        if (i >= 0 && !(lm->m->funcs[i].flags & MF_IMPORT) && fits(&lm->m->funcs[i], pt, np, rt, nr))
            fn = vm_handle(vm, lm, fname, &err);
    } else {
        LModule *lm = vm_module_version(vm, mod, hash);
        if (!lm && vm_module_for_node(vm, mod, &err)) lm = vm_module_version(vm, mod, hash); /* (loaded now: that version?) */
        Function *f = lm ? vm_module_func(lm, fname) : NULL;
        if (f && fits(f->mf, pt, np, rt, nr)) fn = f;
    }
    if (!fn) fn = vm_remote_stub(vm, mod, kind ? NULL : hash, fname, pt, np, rt, nr, &err);
    free(mod);
    free(fname);
    if (!fn) vm_trap(r->who, "E_TYPE", "node: a closure that cannot be made here: %s", err.msg);
    proc_grow_globals(r->who); /* (its module may be new to this process) */
    ObjClosure *c = closure_new(p, fn, nup);
    for (uint32_t i = 0; i < nup; i++) c->up[i] = (Word){0}, c->tags[i] = T_I64; /* (until decoded) */
    return c;
}

/* An object: null, a literal, one met before, or a new one, numbered, whose
 * values, if it has any, come next (the frame pushed for it). Nothing collects
 * while a native runs, so what is half made needs no root. */
static Obj *get_obj(Reader *r, Scratch *s) {
    Process *p = r->p;
    uint8_t kind = get8(r);
    uint32_t n, kids = 0;
    Obj *o;
    switch (kind) {
    case OBJ_NULL: return NULL;
    case WIRE_REF:
        n = get32(r);
        if (n >= s->objs.n) bad(r);
        return s->objs.v[n];
    case WIRE_INTERNED_BIG: case WIRE_INTERNED_DEC: {
        const char *t = get_span(r, &n);
        o = vm_intern_num(p->vm, kind == WIRE_INTERNED_BIG ? T_BIG : T_DEC, t, n);
        if (!o) bad(r);
        return o;
    }
    case WIRE_INTERNED: { /* (one with a NUL in it is no literal: a string of its own, not numbered, as its sender did not) */
        const char *t = get_span(r, &n);
        o = vm_intern(p->vm, t, n);
        return o ? o : &str_new(p, t, n)->h;
    }
    case OBJ_STRING: {
        const char *t = get_span(r, &n);
        o = &str_new(p, t, n)->h;
        break;
    }
    case OBJ_BYTES: {
        const char *t = get_span(r, &n);
        ObjBytes *b = bytes_new(p, n);
        if (n) memcpy(b->data, t, n);
        o = &b->h;
        break;
    }
    case OBJ_ARRAY: {
        uint8_t elem = get8(r);
        if (elem >= T__COUNT || elem == T_NONE || elem == T_P) bad(r);
        kids = get_count(r, 2);
        o = &array_new(p, (MlwsType)elem, kids)->h;
        break;
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
            vm_trap(r->who, "E_TYPE", "%s", msg);
        }
        free(module);
        free(name);
        kids = get_count(r, 2);
        if (kids != d->nfields) bad(r);
        o = &struct_new(p, d)->h;
        break;
    }
    case OBJ_LIST:
        kids = get_count(r, 2);
        o = &list_new(p, kids)->h;
        break;
    case OBJ_MAP:
        kids = 2 * get_count(r, 4);
        o = &map_new(p)->h;
        break;
    case OBJ_CLOSURE: {
        ObjClosure *c = get_closure(r);
        kids = c->nup;
        o = &c->h;
        break;
    }
    case OBJ_BIG: case OBJ_DEC: {
        const char *t = get_span(r, &n);
        o = num_parse(p, kind == OBJ_BIG ? T_BIG : T_DEC, t, n);
        if (!o) bad(r);
        break;
    }
    default: bad(r);
    }
    VEC_PUSH(s->objs, o);
    if (kids) VEC_PUSH(s->df, ((DFrame){o, 0, kids, {0}, 0}));
    return o;
}

static uint8_t get_value(Reader *r, Scratch *s, Word *out) {
    uint8_t tag = get8(r);
    if (tag == T_NONE || tag >= T__COUNT || tag == T_P) bad(r);
    if (type_is_ref((MlwsType)tag)) {
        Obj *o = get_obj(r, s);
        /* (a BigInt's or decimal's type holds only one, or null: the code relies on it) */
        if (o && ((tag == T_BIG && o->kind != OBJ_BIG) || (tag == T_DEC && o->kind != OBJ_DEC))) bad(r);
        out->r = o;
        return tag;
    }
    if (r->n - r->i < 8) bad(r);
    uint64_t v = 0;
    for (int k = 0; k < 8; k++) v |= (uint64_t)r->s[r->i++] << (8 * k);
    out->u = v;
    return tag;
}

/* A value, after `first` objects that the frame numbered (a list it is in,
 * which nothing here can refer to: null if anything does). */
static uint8_t decode(Reader *r, Word *out, uint32_t first) {
    Process *p = r->p;
    Scratch *s = scratch();
    s->df.n = 0;
    s->objs.n = 0;
    for (uint32_t k = 0; k < first; k++) VEC_PUSH(s->objs, NULL);
    uint8_t tag = get_value(r, s, out);
    while (s->df.n) {
        size_t top = s->df.n - 1;
        DFrame *f = &s->df.v[top];
        if (f->i >= f->n) {
            s->df.n--;
            continue;
        }
        uint32_t i = f->i++;
        Word w;
        uint8_t t = get_value(r, s, &w);
        f = &s->df.v[top]; /* (get_value may have pushed, and moved the stack) */
        Obj *o = f->o;
        switch (o->kind) {
        case OBJ_ARRAY:
            if (t != ((ObjArray *)o)->elem) bad(r);
            ((ObjArray *)o)->data[i] = w;
            break;
        case OBJ_STRUCT:
            if (t != ((ObjStruct *)o)->def->types[i]) bad(r);
            ((ObjStruct *)o)->fields[i] = w;
            break;
        case OBJ_LIST: list_push(p, (ObjList *)o, w, t); break;
        case OBJ_CLOSURE:
            ((ObjClosure *)o)->up[i] = w;
            ((ObjClosure *)o)->tags[i] = t;
            break;
        default: /* OBJ_MAP */
            if (i % 2 == 0) f->key = w, f->ktag = t;
            else map_set(p, (ObjMap *)o, f->key, f->ktag, w, t);
        }
    }
    return tag;
}

/* A value as bytes, from node_encode_value, back, into p's heap: its tag. */
uint8_t node_decode_value(Process *p, const uint8_t *s, size_t n, Word *out) {
    Reader rd = {p, p, s, n, 0};
    uint8_t t = decode(&rd, out, 0);
    if (rd.i != n) bad(&rd);
    return t;
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

/* Two connections to one node (both nodes dialed at once, or the node came
 * back before the old connection was noticed gone): both ends keep the one
 * the lower-numbered node dialed, or, if one node dialed both, the newer, so
 * that they agree. Whether a connection node `by` dialed to node n stays here
 * (vm->lock held): against the one routed, or this node's own try to dial n. */
static bool stays_locked(const VM *vm, uint32_t n, uint32_t by) {
    uint64_t old = vm->node_route ? __atomic_load_n(&vm->node_route[n], __ATOMIC_ACQUIRE) : 0;
    if (old) return by <= vm->node_route_by[n];
    if (by == n && vm->node_dial && vm->node_dial[n] == NODE_DIALING) return n < __atomic_load_n(&vm->node_self, __ATOMIC_ACQUIRE);
    return true;
}

/* node.may_attach(n, by) -> b: for the handshake's answering end, before
 * anything goes over a connection node `by` dialed: whether it stays
 * (stays_locked). The dialing end attaches only if told so. */
static void n_may_attach(Process *p, Word *a, Word *r) {
    no_caps(p, "node.may_attach");
    if (a[0].i < 1 || a[0].i > MAX_NODE) vm_trap(p, "E_RANGE", "node.may_attach: a node's number is 1 to %d", MAX_NODE);
    plat_lock(p->vm->lock);
    r[0].i = stays_locked(p->vm, (uint32_t)a[0].i, (uint32_t)a[1].i);
    plat_unlock(p->vm->lock);
}

/* node.attach(n, writer, dialed_by) -> b: a connection to node n is up, carried
 * by process writer, made by node dialed_by (this one's number, or n's): false
 * if another connection to n stays instead (stays_locked), which the caller
 * closes; else the older one, if any, ends. */
static void n_attach(Process *p, Word *a, Word *r) {
    no_caps(p, "node.attach");
    if (a[0].i < 1 || a[0].i > MAX_NODE || !a[1].u) vm_trap(p, "E_RANGE", "node.attach: a node 1 to %d, and a writer", MAX_NODE);
    VM *vm = p->vm;
    uint32_t n = (uint32_t)a[0].i, by = (uint32_t)a[2].i;
    plat_lock(vm->lock);
    if (!vm->node_route) vm->node_route = xcalloc(MAX_NODE + 1, sizeof *vm->node_route);
    if (!vm->node_route_by) vm->node_route_by = xcalloc(MAX_NODE + 1, sizeof *vm->node_route_by);
    if (!vm->node_reader) vm->node_reader = xcalloc(MAX_NODE + 1, sizeof *vm->node_reader);
    if (!vm->node_stats) vm->node_stats = xcalloc(MAX_NODE + 1, sizeof *vm->node_stats);
    uint64_t old = __atomic_load_n(&vm->node_route[n], __ATOMIC_ACQUIRE);
    if (old && by > vm->node_route_by[n]) { /* the one that stays is the other */
        plat_unlock(vm->lock);
        r[0].i = 0;
        return;
    }
    NodeStats *st = vm->node_stats[n]; /* its counts start again */
    if (!st) st = vm->node_stats[n] = xcalloc(1, sizeof *st);
    atomic_store(&st->sent, 0), atomic_store(&st->received, 0), atomic_store(&st->bytes_out, 0);
    atomic_store(&st->bytes_in, 0), atomic_store(&st->waits, 0);
    st->up_ns = plat_now_ns();
    uint64_t old_reader = vm->node_reader[n];
    vm->node_route_by[n] = by;
    vm->node_reader[n] = p->pid;
    __atomic_store_n(&vm->node_route[n], a[1].u, __ATOMIC_RELEASE);
    plat_unlock(vm->lock);
    sched_set_carries(vm, a[1].u, n);
    if (old) { /* the older connection ends: both its processes, so that its socket closes */
        sched_kill(p, old, "E_KILLED", "replaced by another connection to its node");
        if (old_reader && old_reader != p->pid) sched_kill(p, old_reader, "E_KILLED", "replaced by another connection to its node");
    } else {
        sched_node_event(vm, n, true);
    }
    sched_dial_done(vm, n, true); /* (senders waiting for it go on: it is routed now) */
    r[0].i = 1;
}

/* node.disconnect(n): close the connection to node n, as if it had gone: its
 * writer and reader end, so that its socket closes and the other end sees it
 * go too. */
static void n_disconnect(Process *p, Word *a, Word *r) {
    (void)r;
    no_caps(p, "node.disconnect");
    if (a[0].i < 1 || a[0].i > MAX_NODE) vm_trap(p, "E_RANGE", "node.disconnect: a node's number is 1 to %d", MAX_NODE);
    VM *vm = p->vm;
    uint32_t n = (uint32_t)a[0].i;
    plat_lock(vm->lock);
    uint64_t w = vm->node_route ? __atomic_exchange_n(&vm->node_route[n], 0, __ATOMIC_ACQ_REL) : 0;
    uint64_t rd = w && vm->node_reader ? vm->node_reader[n] : 0;
    plat_unlock(vm->lock);
    if (!w) return;
    sched_node_down(vm, n);
    sched_node_event(vm, n, false);
    sched_kill(p, w, "E_KILLED", "disconnected");
    if (rd && rd != p->pid) sched_kill(p, rd, "E_KILLED", "disconnected");
}

/* node.detach(n, writer): the connection writer carried to node n has gone:
 * node n is down, unless another connection carries it now. */
static void n_detach(Process *p, Word *a, Word *r) {
    (void)r;
    no_caps(p, "node.detach");
    if (a[0].i < 1 || a[0].i > MAX_NODE) vm_trap(p, "E_RANGE", "node.detach: a node's number is 1 to %d", MAX_NODE);
    VM *vm = p->vm;
    uint32_t n = (uint32_t)a[0].i;
    uint64_t want = a[1].u;
    if (!vm->node_route || !__atomic_compare_exchange_n(&vm->node_route[n], &want, 0, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return; /* (not routed by it) */
    sched_node_down(vm, n);
    sched_node_event(vm, n, false);
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
/* The numbers that have had a name (vm->lock held): VM.node_numbers, kept
 * with node_names (name_number_set), so that a lookup is one, not a scan of
 * every number. NULL for none. */
static struct NodeNums *numbers_of(const VM *vm, const char *s, uint32_t len) {
    char key[MAX_NODE_NAME + 1];
    if (!vm->node_numbers.n || !len || len > MAX_NODE_NAME || memchr(s, 0, len)) return NULL;
    memcpy(key, s, len);
    key[len] = 0;
    void *v = NULL;
    return smap_get(&vm->node_numbers, key, &v) ? v : NULL;
}

/* Number n has name (or none, NULL) from now on (vm->lock held). */
static void name_number_set(VM *vm, uint32_t n, const char *name, uint32_t len) {
    const char *was = vm->node_names[n];
    struct NodeNums *ns = was ? numbers_of(vm, was, (uint32_t)strlen(was)) : NULL;
    for (size_t i = 0; ns && i < ns->n; i++)
        if (ns->v[i] == n) ns->v[i] = ns->v[--ns->n];
    free(vm->node_names[n]);
    vm->node_names[n] = len ? xstrndup(name, len) : NULL;
    if (!len) return;
    ns = numbers_of(vm, name, len);
    if (!ns) {
        ns = xcalloc(1, sizeof *ns);
        smap_put(&vm->node_numbers, vm->node_names[n], ns);
    }
    VEC_PUSH(*ns, n);
}

/* The number of the node with a name: this VM's, a connected node's, or else
 * the highest that had it; 0 if none (vm->lock held). */
static uint32_t number_named(const VM *vm, const char *s, uint32_t len) {
    if (vm->node_name && strlen(vm->node_name) == len && !memcmp(vm->node_name, s, len))
        return __atomic_load_n(&vm->node_self, __ATOMIC_ACQUIRE);
    const struct NodeNums *ns = numbers_of(vm, s, len);
    uint32_t found = 0;
    for (size_t i = 0; ns && i < ns->n; i++) {
        if (connected_now(vm, ns->v[i])) return ns->v[i];
        if (ns->v[i] > found) found = ns->v[i];
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
    uint32_t has = len ? number_named(vm, s, len) : 0, self = __atomic_load_n(&vm->node_self, __ATOMIC_ACQUIRE);
    bool ok = !has || has == n || (has != self && !connected_now(vm, has));
    if (vm->node_name && strlen(vm->node_name) == len && !memcmp(vm->node_name, s, len)) ok = false;
    /* Nor a number this node has, or a connected node of another name (or of none): the
     * numbers in pids must each mean one node. */
    if (n == self) ok = false;
    const char *was = vm->node_names ? vm->node_names[n] : NULL;
    if (connected_now(vm, n) && (!was || strlen(was) != len || memcmp(was, s, len))) ok = false;
    if (ok) {
        if (!vm->node_names) vm->node_names = xcalloc(MAX_NODE + 1, sizeof *vm->node_names);
        name_number_set(vm, n, s, len);
    }
    plat_unlock(vm->lock);
    r[0].i = ok;
}

/* ---- connecting when a message needs it (sched.c, dial_wait) ---- */

/* node.set_dialer(pid): std.node's dialer, asked [n] to connect to node n. */
static void n_set_dialer(Process *p, Word *a, Word *r) {
    (void)r;
    no_caps(p, "node.set_dialer");
    atomic_store(&p->vm->node_dialer, a[0].u);
}

static void n_dialer(Process *p, Word *a, Word *r) {
    (void)a;
    r[0].u = atomic_load(&p->vm->node_dialer);
}

/* node.auto_connect(on): whether a message to a node not connected waits while
 * the dialer connects to it (true at first). */
static void n_auto_connect(Process *p, Word *a, Word *r) {
    (void)r;
    atomic_store(&p->vm->node_autoconnect, a[0].i != 0);
}

/* node.set_addr(n, host, port, ca): where node n was reached, and how (ca:
 * the TLS authorities' file, "" for the system's, null for plain), to reach it
 * again the same way. */
static void n_set_addr(Process *p, Word *a, Word *r) {
    (void)r;
    no_caps(p, "node.set_addr");
    if (a[0].i < 1 || a[0].i > MAX_NODE) vm_trap(p, "E_RANGE", "node.set_addr: a node's number is 1 to %d", MAX_NODE);
    uint32_t len, clen = 0;
    const char *host = name_arg(p, a[1], "node.set_addr", &len);
    const ObjString *ca = a[3].r && ((const Obj *)a[3].r)->kind == OBJ_STRING ? a[3].r : NULL;
    if (ca) clen = ca->len;
    VM *vm = p->vm;
    plat_lock(vm->lock);
    if (!vm->node_addrs) vm->node_addrs = xcalloc(MAX_NODE + 1, sizeof *vm->node_addrs);
    struct NodeAddr *na = &vm->node_addrs[a[0].i];
    free(na->host);
    free(na->ca);
    na->host = xstrndup(host, len);
    na->port = a[2].i;
    na->ca = ca ? xstrndup(ca->data, clen) : NULL;
    plat_unlock(vm->lock);
}

/* node.dial_info(n): how to reach node n: [name ("": none), host (null: not
 * known), port, ca (null: plain, or not known)]; null if neither is known. */
static void n_dial_info(Process *p, Word *a, Word *r) {
    r[0].r = NULL;
    if (a[0].i < 1 || a[0].i > MAX_NODE) return;
    VM *vm = p->vm;
    char name[MAX_NODE_NAME + 1] = "", host[300] = "", ca[4096] = "";
    int64_t port = 0;
    bool known, tls = false;
    plat_lock(vm->lock);
    if (vm->node_names && vm->node_names[a[0].i]) snprintf(name, sizeof name, "%s", vm->node_names[a[0].i]);
    const struct NodeAddr *na = vm->node_addrs ? &vm->node_addrs[a[0].i] : NULL;
    if (na && na->host) {
        snprintf(host, sizeof host, "%s", na->host), port = na->port;
        if (na->ca) snprintf(ca, sizeof ca, "%s", na->ca), tls = true;
    }
    known = name[0] || host[0];
    plat_unlock(vm->lock);
    if (!known) return;
    ObjList *l = list_new(p, 4);
    r[0].r = &l->h;
    list_push(p, l, (Word){.r = &str_new(p, name, strlen(name))->h}, T_R);
    list_push(p, l, (Word){.r = host[0] ? &str_new(p, host, strlen(host))->h : NULL}, T_R);
    list_push(p, l, (Word){.i = port}, T_I64);
    list_push(p, l, (Word){.r = tls ? &str_new(p, ca, strlen(ca))->h : NULL}, T_R);
}

/* node.dialed(n, ok): the dialer's try to connect to node n is over. */
static void n_dialed(Process *p, Word *a, Word *r) {
    (void)r;
    no_caps(p, "node.dialed");
    sched_dial_done(p->vm, (uint32_t)a[0].i, a[1].i != 0);
}

/* node.set_ticktime(ms): a connection that hears nothing for this long is
 * taken as down, and an idle one sends a tick every quarter of it (0: no
 * ticks, and never down for silence). For connections made from now on; each
 * end uses the longer of its own and the other's. */
static void n_set_ticktime(Process *p, Word *a, Word *r) {
    (void)r;
    if (a[0].i < 0 || (a[0].i > 0 && a[0].i < 400) || a[0].i > 86400000)
        vm_trap(p, "E_RANGE", "node.set_ticktime: 0 (none), or 400 ms to a day");
    atomic_store(&p->vm->node_ticktime, a[0].i);
}

static void n_ticktime(Process *p, Word *a, Word *r) {
    (void)a;
    r[0].i = atomic_load(&p->vm->node_ticktime);
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

/* node.name_pid(number, name): a pid that stands for whichever process has
 * the name registered on node number (0: this VM) when a message arrives
 * there, for send and proc.call: fixed, so it can be kept, while the name may
 * pass from process to process. */
static void n_name_pid(Process *p, Word *a, Word *r) {
    if (a[0].i < 0 || a[0].i > MAX_NODE) vm_trap(p, "E_RANGE", "node.name_pid: a node's number is 0 to %d", MAX_NODE);
    const Obj *o = a[1].r;
    if (!o || o->kind != OBJ_STRING) vm_trap(p, "E_TYPE", "node.name_pid: the name is a string");
    const ObjString *s = (const ObjString *)o;
    if (!s->len || s->len > PID_NAME_MAX || memchr(s->data, 0, s->len))
        vm_trap(p, "E_RANGE", "node.name_pid: a name is 1 to %d bytes, without NUL", PID_NAME_MAX);
    VM *vm = p->vm;
    plat_lock(vm->lock);
    void *v = NULL;
    if (!smap_get(&vm->pid_name_ids, s->data, &v)) {
        VEC_PUSH(vm->pid_names, xstrdup(s->data));
        v = (void *)(uintptr_t)vm->pid_names.n;
        smap_put(&vm->pid_name_ids, s->data, v);
    }
    plat_unlock(vm->lock);
    r[0].u = (uint64_t)a[0].i << NODE_SHIFT | PID_NAMED | (uint64_t)(uintptr_t)v;
}

/* A node's number made from its name, 1 to 65535 (FNV-1a), for nodes that
 * have names but no numbers of their own choosing: two names may give one
 * number, and then the second node to connect is refused (node.named). */
static void n_number_for(Process *p, Word *a, Word *r) {
    uint32_t len;
    const char *s = name_arg(p, a[0], "node.number_for", &len);
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < len; i++) h = (h ^ (uint8_t)s[i]) * 16777619u;
    r[0].i = 1 + (int64_t)(h % MAX_NODE);
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
    uint8_t tag = T_R;
    encode(p, &b, 1, a, &tag, 0);
    r[0].r = &frame_end(p, &b)->h;
}

/* A closure for an exported function of a module, as a peer names it: the one
 * loaded under that name, whoever loaded it, or else from the library path. */
static void n_function(Process *p, Word *a, Word *r) {
    no_caps(p, "node.function");
    const Obj *mo = a[0].r, *fo = a[1].r;
    if (!mo || mo->kind != OBJ_STRING || !fo || fo->kind != OBJ_STRING) vm_trap(p, "E_TYPE", "node.function: a module and a function");
    const char *name = ((const ObjString *)mo)->data, *fname = ((const ObjString *)fo)->data;
    MlwsError err = {{0}, {0}};
    LModule *lm = vm_module_for_node(p->vm, name, &err);
    if (!lm) vm_trap(p, err.code, "%s", err.msg);
    proc_grow_globals(p);
    int i = module_find_export_func(lm->m, fname);
    if (i < 0 || (lm->m->funcs[i].flags & MF_IMPORT)) vm_trap(p, "E_LINK_UNDEF", "node.function: module %s does not export '%s'", name, fname);
    r[0].r = &closure_new(p, &lm->funcs[i], 0)->h;
}

static void n_decode(Process *p, Word *a, Word *r) {
    uint32_t n;
    const uint8_t *s = data_arg(p, a[0], &n);
    Reader rd = {p, p, s, n, 0};
    Word w;
    uint8_t t = decode(&rd, &w, 0);
    if (t != T_R || rd.i != n) bad(&rd);
    r[0] = w;
}

/* The counts of node n's connection, or NULL before it first came. */
NodeStats *node_stats_of(VM *vm, uint32_t n) {
    NodeStats **v = __atomic_load_n(&vm->node_stats, __ATOMIC_ACQUIRE);
    return v && n && n <= MAX_NODE ? __atomic_load_n(&v[n], __ATOMIC_ACQUIRE) : NULL;
}

/* node.take(frame, peer): a frame from node peer, for its reader. A message, [0, pid,
 * value], is decoded straight into what is delivered to that process here, as
 * it was made where it was sent, and null returned; any other frame comes
 * back as node.decode gives it. */
static void n_take(Process *p, Word *a, Word *r) {
    uint32_t n;
    const uint8_t *s = data_arg(p, a[0], &n);
    NodeStats *st = node_stats_of(p->vm, (uint32_t)a[1].i);
    if (st) atomic_fetch_add(&st->bytes_in, (uint64_t)n + 4);
    static const uint8_t head[] = {T_R, OBJ_LIST, 3, 0, 0, 0, T_I64, 0, 0, 0, 0, 0, 0, 0, 0, T_I64};
    static const uint8_t named[] = {T_R, OBJ_LIST, 3, 0, 0, 0, T_I64, 12, 0, 0, 0, 0, 0, 0, 0, T_R, WIRE_INTERNED};
    if (n >= sizeof named + 4 && !memcmp(s, named, sizeof named)) { /* [12, name, value]: to whoever has the name here */
        Reader rd = {sched_frag(p->vm), p, s, n, sizeof named};
        uint32_t len;
        const char *at = get_span(&rd, &len);
        char name[PID_NAME_MAX + 1];
        if (!len || len > PID_NAME_MAX || memchr(at, 0, len)) bad(&rd);
        memcpy(name, at, len);
        name[len] = 0;
        Word w;
        uint8_t t = decode(&rd, &w, 1);
        if (rd.i != n) bad(&rd);
        if (st) atomic_fetch_add(&st->received, 1);
        sched_node_deliver(p, sched_whereis(p->vm, name), w, t); /* (0: nobody has it: dropped, a Call answered) */
        r[0].r = NULL;
        return;
    }
    if (n < sizeof head + 8 || memcmp(s, head, sizeof head)) {
        n_decode(p, a, r);
        return;
    }
    uint64_t pid = 0;
    for (int k = 0; k < 8; k++) pid |= (uint64_t)s[sizeof head + k] << (8 * k);
    Reader rd = {sched_frag(p->vm), p, s, n, sizeof head + 8};
    Word w;
    uint8_t t = decode(&rd, &w, 1);
    if (rd.i != n) bad(&rd);
    if (st) atomic_fetch_add(&st->received, 1);
    sched_node_deliver(p, pid, w, t);
    r[0].r = NULL;
}

/* Give the message a frame carries, [to, value], to its process here. */
/* node.written(n): the carrier of a connection wrote n bytes of the frames
 * sent to it, so that senders waiting for it to have room go on. */
static void n_written(Process *p, Word *a, Word *r) {
    (void)r;
    if (a[0].i > 0) sched_node_written(p, (uint64_t)a[0].i);
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
    N("node.may_attach", 2, T(T_I64, T_I64), 1, T(T_B), n_may_attach, "Whether a connection to a node, which the node of the second number dialed, stays (both dialed at once: the lower's). For std.node."),
    N("node.disconnect", 1, T(T_I64), 0, T(0), n_disconnect, "Close the connection to a node, as if it had gone."),
    N("node.detach", 2, T(T_I64, T_I64), 0, T(0), n_detach, "The connection a writer carried to a node has gone. For std.node."),
    N("node.attach", 3, T(T_I64, T_I64, T_I64), 1, T(T_B), n_attach, "Messages for a node go to this writer, of a connection the node of the third number dialed; false if another connection to it stays instead. For std.node."),
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
    N("node.number_for", 1, T(T_R), 1, T(T_I64), n_number_for,
      "A node number, 1 to 65535, made from a node's name (as std.node's start_named does when given 0)."),
    N("node.nodes", 0, T(0), 1, T(T_R), n_connected_nodes, "The numbers of the nodes connected now, as a list, in order."),
    N("node.route", 1, T(T_I64), 1, T(T_I64), n_route, "The process messages for a node go to, or 0. For std.node."),
    N("node.function", 2, T(T_R, T_R), 1, T(T_R), n_function, "A closure for a module's exported function, as a peer names it. For std.node."),
    N("node.frame", 1, T(T_R), 1, T(T_R), n_frame, "A value as bytes for a connection, with its length. For std.node."),
    N("node.decode", 1, T(T_R), 1, T(T_R), n_decode, "A value from a frame's bytes. Untrusted bytes raise E_PARSE, never worse. For std.node."),
    N("node.name_pid", 2, T(T_I64, T_R), 1, T(T_I64), n_name_pid, "A pid for whichever process has a name registered on a node (0: this VM) when a message arrives there: for send and proc.call, and to keep."),
    N("node.set_dialer", 1, T(T_I64), 0, T(0), n_set_dialer, "std.node's dialer, asked [n] to connect to node n when a message needs it. For std.node."),
    N("node.dialer", 0, T(0), 1, T(T_I64), n_dialer, "std.node's dialer, or 0. For std.node."),
    N("node.auto_connect", 1, T(T_B), 0, T(0), n_auto_connect, "Whether a send or proc.call to a node not connected, but reached before, waits while it is connected again (true at first)."),
    N("node.set_addr", 4, T(T_I64, T_R, T_I64, T_R), 0, T(0), n_set_addr, "Where a node was reached (host, port) and how (TLS's authorities, or null: plain), to reach it again. For std.node."),
    N("node.dial_info", 1, T(T_I64), 1, T(T_R), n_dial_info, "How to reach a node: [name, host or null, port, ca or null], or null. For std.node."),
    N("node.dialed", 2, T(T_I64, T_B), 0, T(0), n_dialed, "The dialer's try to connect to a node is over (ok or not). For std.node."),
    N("node.set_ticktime", 1, T(T_I64), 0, T(0), n_set_ticktime, "How long, in ms, a connection may hear nothing before it is taken as down; idle ones tick every quarter of it (0: no ticks). 60000 at first."),
    N("node.ticktime", 0, T(0), 1, T(T_I64), n_ticktime, "node.set_ticktime's value."),
    N("node.written", 1, T(T_I64), 0, T(0), n_written, "The bytes of frames this connection's writer has written, for senders waiting for room. For std.node."),
    N("node.take", 2, T(T_R, T_I64), 1, T(T_R), n_take, "A frame from a node (the second argument): a message, [0, pid, value], delivered to its process here (null returned), else the frame's value, as node.decode gives it. For std.node."),
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
