/* ffi.c - the declarative FFI: `.extern` declarations become natives that call
 * C functions through libffi; `.cstruct` describes C structs; `.ffi_free`
 * gives tagged pointers a finalizer. Built only with libffi (MLWS_HAVE_FFI;
 * the Makefile finds it with pkg-config); without it these fail to link.
 *
 * Conversions (docs/EXTENDING.md, "The declarative FFI"):
 *   integers, f32, f64  by value, as the declared C type
 *   cstr   a string (its bytes, NUL-terminated) or byte buffer (copied and
 *          NUL-terminated); as a result, the char * is copied into a string
 *   buf    a byte buffer's own bytes, which the callee may change
 *   ptr    a foreign handle's pointer; as a result, a new foreign handle
 *          (freed by the tag's .ffi_free function, if any). ptr<tag> handles
 *          only match the same tag.
 *   struct S    a .cstruct by value, from and to the MLWSVM struct S
 *   struct* S   a pointer to a copy of the struct, copied back after the call
 *   null   NULL, for cstr, buf, ptr and struct*
 *   out T  no argument: a pointer to a temporary, whose value is a result
 *   ...    a variadic function: the types after it are passed as declared
 * Pointers passed in are valid only for the duration of the call. */
#include "../../include/mlwsvm/mlwsvm.h"
#include "../core/module.h"
#include "../heap/heap.h"
#include "../platform/platform.h"

#ifdef MLWS_HAVE_FFI
#include <ffi.h>

/* A tag's handle type. Its finalizer is the tag's .ffi_free function, which
 * takes the one pointer, so it is called directly. */
typedef MlwsForeignType FfiTag;

/* A .cstruct, laid out for libffi. Arrays are expanded into repeated
 * elements, which gives exactly C's layout. */
typedef struct FfiStruct {
    char *name;
    const StructDef *def; /* the MLWSVM struct of the same name */
    uint32_t nfields;
    uint8_t *kinds;
    uint32_t *counts;
    const struct FfiStruct **nested;
    const FfiTag **tags;
    size_t *offsets;      /* of each field (an array: its first element) */
    ffi_type type;
    ffi_type **elems;
    char *decl;
} FfiStruct;

typedef struct {
    void *fn;
    ffi_cif cif;
    ffi_type *atypes[FFI_MAX_ARGS];
    uint8_t nargs, args[FFI_MAX_ARGS], ret;
    const FfiTag *tags[FFI_MAX_ARGS], *ret_tag;
    const FfiStruct *structs[FFI_MAX_ARGS], *ret_struct;
    char *decl; /* "lib:symbol(...)" for comparing duplicate declarations */
} FfiCall;

typedef union {
    int8_t i8;
    int16_t i16;
    int32_t i32;
    int64_t i64;
    uint8_t u8;
    uint16_t u16;
    uint32_t u32;
    uint64_t u64;
    float f32;
    double f64;
    void *ptr;
    ffi_arg arg;
    ffi_sarg sarg;
} CVal;

bool ffi_available(void) { return true; }

static ffi_type *ctype(uint8_t k) {
    switch (k & (uint8_t)~FFI_OUT) {
    case FFI_I8: return &ffi_type_sint8;
    case FFI_I16: return &ffi_type_sint16;
    case FFI_I32: return &ffi_type_sint32;
    case FFI_I64: return &ffi_type_sint64;
    case FFI_U8: return &ffi_type_uint8;
    case FFI_U16: return &ffi_type_uint16;
    case FFI_U32: return &ffi_type_uint32;
    case FFI_U64: return &ffi_type_uint64;
    case FFI_F32: return &ffi_type_float;
    case FFI_F64: return &ffi_type_double;
    case FFI_VOID: return &ffi_type_void;
    default: return &ffi_type_pointer;
    }
}

static MlwsType vmtype(uint8_t k) {
    static const MlwsType T[] = {T_NONE, T_I8, T_I16, T_I32, T_I64, T_U8, T_U16, T_U32, T_U64, T_F32, T_F64};
    k &= (uint8_t)~FFI_OUT;
    return k <= FFI_F64 ? T[k] : T_R;
}

static size_t kind_size(uint8_t k) {
    switch (k) {
    case FFI_I8: case FFI_U8: return 1;
    case FFI_I16: case FFI_U16: return 2;
    case FFI_I32: case FFI_U32: case FFI_F32: return 4;
    case FFI_I64: case FFI_U64: case FFI_F64: return 8;
    default: return sizeof(void *);
    }
}

static const FfiTag *tag_type(VM *vm, const char *tag) {
    const char *name = tag ? tag : "ptr";
    void *v;
    if (smap_get(&vm->ffi_types, name, &v)) return v;
    FfiTag *t = xcalloc(1, sizeof *t);
    t->name = xstrdup(name);
    smap_put(&vm->ffi_types, name, t);
    return t;
}

static const FfiStruct *find_struct(VM *vm, const char *name) {
    void *v;
    return smap_get(&vm->ffi_structs, name, &v) ? v : NULL;
}

/* ---- numbers in memory ----------------------------------------------------------- */

static void put_num(uint8_t *mem, uint8_t k, Word w) {
    CVal v;
    switch (k) {
    case FFI_I8: v.i8 = (int8_t)w.i; break;
    case FFI_I16: v.i16 = (int16_t)w.i; break;
    case FFI_I32: v.i32 = (int32_t)w.i; break;
    case FFI_I64: v.i64 = w.i; break;
    case FFI_U8: v.u8 = (uint8_t)w.u; break;
    case FFI_U16: v.u16 = (uint16_t)w.u; break;
    case FFI_U32: v.u32 = (uint32_t)w.u; break;
    case FFI_U64: v.u64 = w.u; break;
    case FFI_F32: v.f32 = (float)w.f; break;
    default: v.f64 = w.f; break;
    }
    memcpy(mem, &v, kind_size(k));
}

static Word get_num(const uint8_t *mem, uint8_t k) {
    CVal v;
    memcpy(&v, mem, kind_size(k));
    Word w = {.u = 0};
    switch (k) {
    case FFI_I8: w.i = v.i8; break;
    case FFI_I16: w.i = v.i16; break;
    case FFI_I32: w.i = v.i32; break;
    case FFI_I64: w.i = v.i64; break;
    case FFI_U8: w.u = v.u8; break;
    case FFI_U16: w.u = v.u16; break;
    case FFI_U32: w.u = v.u32; break;
    case FFI_U64: w.u = v.u64; break;
    case FFI_F32: w.f = v.f32; break;
    default: w.f = v.f64; break;
    }
    return w;
}

/* ---- structs: MLWSVM <-> C ---------------------------------------------------------- */

typedef struct {
    char *copies[64]; /* NUL-terminated copies of byte buffers passed as cstr */
    int n;
} Temps;

static void temps_free(Temps *t) {
    for (int i = 0; i < t->n; i++) free(t->copies[i]);
}

/* A cstr from a string (its own bytes) or byte buffer (a temporary copy). */
static bool to_cstr(Process *p, Obj *o, char **out, Temps *t, const char *what) {
    if (!o) *out = NULL;
    else if (o->kind == OBJ_STRING) *out = ((ObjString *)o)->data;
    else if (o->kind == OBJ_BYTES && t->n < 64) {
        ObjBytes *b = (ObjBytes *)o;
        char *c = xmalloc(b->len + 1);
        memcpy(c, b->data, b->len);
        c[b->len] = 0;
        t->copies[t->n++] = c;
        *out = c;
    } else {
        native_fail(p, "E_TYPE", "%s: %s must be a string or byte buffer", p->cur_native->name, what);
        return false;
    }
    return true;
}

static bool to_ptr(Process *p, Obj *o, const FfiTag *tag, void **out, const char *what) {
    if (!o) {
        *out = NULL;
        return true;
    }
    if (o->kind != OBJ_FOREIGN || ((ObjForeign *)o)->type != tag) {
        native_fail(p, "E_TYPE", "%s: %s must be a foreign %s", p->cur_native->name, what, tag->name);
        return false;
    }
    if (!((ObjForeign *)o)->ptr) {
        native_fail(p, "E_TYPE", "%s: %s: the foreign %s has been released", p->cur_native->name, what, tag->name);
        return false;
    }
    *out = ((ObjForeign *)o)->ptr;
    return true;
}

/* A struct of the cstruct's type: its own, or another module's declaration of
 * the same cstruct (the same name and field types). */
static bool is_struct(const Obj *o, const FfiStruct *fs) {
    if (!o || o->kind != OBJ_STRUCT) return false;
    const StructDef *d = ((const ObjStruct *)o)->def;
    return d == fs->def || (!strcmp(d->name, fs->name) && d->nfields == fs->nfields &&
                            !memcmp(d->types, fs->def->types, fs->nfields));
}

/* Write an MLWSVM struct's fields into C memory. */
static bool pack(Process *p, const FfiStruct *fs, Obj *o, uint8_t *mem, Temps *t) {
    if (!is_struct(o, fs)) {
        native_fail(p, "E_TYPE", "%s: expected a %s struct", p->cur_native->name, fs->name);
        return false;
    }
    ObjStruct *s = (ObjStruct *)o;
    for (uint32_t k = 0; k < fs->nfields; k++) {
        uint8_t kind = fs->kinds[k];
        uint8_t *at = mem + fs->offsets[k];
        Word w = s->fields[k];
        if (fs->counts[k]) { /* an array: a byte buffer, or a list of numbers */
            Obj *a = w.r;
            size_t esz = kind_size(kind);
            memset(at, 0, esz * fs->counts[k]);
            if (!a) continue;
            if (a->kind == OBJ_BYTES && esz == 1) {
                ObjBytes *b = (ObjBytes *)a;
                memcpy(at, b->data, b->len < fs->counts[k] ? b->len : fs->counts[k]);
            } else if (a->kind == OBJ_LIST) {
                ObjList *l = (ObjList *)a;
                for (uint32_t i = 0; i < l->len && i < fs->counts[k]; i++) put_num(at + i * esz, kind, l->items[i]);
            } else {
                native_fail(p, "E_TYPE", "%s: %s.%s must be a byte buffer or list", p->cur_native->name, fs->name,
                            s->def->fields[k]);
                return false;
            }
            continue;
        }
        switch (kind) {
        case FFI_CSTR: {
            char *c;
            if (!to_cstr(p, w.r, &c, t, s->def->fields[k])) return false;
            memcpy(at, &c, sizeof c);
            break;
        }
        case FFI_PTR: {
            void *ptr;
            if (!to_ptr(p, w.r, fs->tags[k], &ptr, s->def->fields[k])) return false;
            memcpy(at, &ptr, sizeof ptr);
            break;
        }
        case FFI_STRUCT:
            if (!w.r) memset(at, 0, fs->nested[k]->type.size); /* null: all zeros */
            else if (!pack(p, fs->nested[k], w.r, at, t)) return false;
            break;
        default: put_num(at, kind, w); break;
        }
    }
    return true;
}

/* Read C memory into an MLWSVM struct: `into` (copying back a struct*), or a
 * new one. A pointer field that is unchanged keeps its handle. */
static Obj *unpack(Process *p, const FfiStruct *fs, const uint8_t *mem, ObjStruct *into, ExtLib *lib) {
    ObjStruct *s = into ? into : struct_new(p, fs->def);
    for (uint32_t k = 0; k < fs->nfields; k++) {
        uint8_t kind = fs->kinds[k];
        const uint8_t *at = mem + fs->offsets[k];
        if (fs->counts[k]) {
            size_t esz = kind_size(kind);
            if (esz == 1) {
                ObjBytes *b = bytes_new(p, fs->counts[k]);
                memcpy(b->data, at, fs->counts[k]);
                s->fields[k].r = b;
            } else {
                ObjList *l = list_new(p, fs->counts[k]);
                for (uint32_t i = 0; i < fs->counts[k]; i++) list_push(p, l, get_num(at + i * esz, kind), (uint8_t)vmtype(kind));
                s->fields[k].r = l;
            }
            continue;
        }
        switch (kind) {
        case FFI_CSTR: {
            const char *c;
            memcpy(&c, at, sizeof c);
            Obj *old = s->fields[k].r;
            if (old && old->kind == OBJ_STRING && ((ObjString *)old)->data == c) break; /* unchanged */
            s->fields[k].r = c ? str_new(p, c, strlen(c)) : NULL;
            break;
        }
        case FFI_PTR: {
            void *ptr;
            memcpy(&ptr, at, sizeof ptr);
            ObjForeign *old = s->fields[k].r;
            if (old && old->h.kind == OBJ_FOREIGN && old->ptr == ptr) break; /* the same handle */
            s->fields[k].r = ptr ? foreign_new_obj(p, fs->tags[k], ptr, lib) : NULL;
            break;
        }
        case FFI_STRUCT: {
            Obj *old = s->fields[k].r;
            s->fields[k].r = unpack(p, fs->nested[k], at, is_struct(old, fs->nested[k]) ? (ObjStruct *)old : NULL, lib);
            break;
        }
        default: s->fields[k] = get_num(at, kind); break;
        }
    }
    return &s->h;
}

static char *describe_struct(const MCStruct *c) {
    StrBuf b = {0};
    sb_printf(&b, "%s{", c->name);
    for (uint32_t k = 0; k < c->nfields; k++)
        sb_printf(&b, "%s:%d/%s[%u];", c->fields[k], c->kinds[k], c->tags[k] ? c->tags[k] : "", c->counts[k]);
    sb_putc(&b, '}');
    return b.s;
}

bool ffi_define_struct(VM *vm, LModule *lm, const MCStruct *c, MlwsError *err) {
    char *decl = describe_struct(c);
    const FfiStruct *old = find_struct(vm, c->name);
    if (old) { /* declared by another module too: must agree */
        bool same = !strcmp(old->decl, decl);
        free(decl);
        return same ? true : mlws_err(err, "E_LINK_FFI", "cstruct %s conflicts with another of that name", c->name);
    }
    const StructDef *def = NULL;
    for (uint32_t i = 0; i < lm->m->nstructs; i++)
        if (!strcmp(lm->m->structs[i].name, c->name)) def = lm->structs[i];
    bool ok = def && def->nfields == c->nfields; /* the struct must match its cstruct, field by field */
    for (uint32_t k = 0; ok && k < c->nfields; k++)
        ok = !strcmp(def->fields[k], c->fields[k]) && def->types[k] == (c->counts[k] ? T_R : vmtype(c->kinds[k]));
    if (!ok) {
        free(decl);
        return mlws_err(err, "E_LINK_FFI", "cstruct %s: its struct type is missing or different", c->name);
    }
    FfiStruct *fs = xcalloc(1, sizeof *fs);
    fs->name = xstrdup(c->name);
    fs->def = def;
    fs->decl = decl;
    fs->nfields = c->nfields;
    fs->kinds = xmalloc(c->nfields);
    memcpy(fs->kinds, c->kinds, c->nfields);
    fs->counts = xcalloc(c->nfields + 1, sizeof *fs->counts);
    fs->nested = xcalloc(c->nfields + 1, sizeof *fs->nested);
    fs->tags = xcalloc(c->nfields + 1, sizeof *fs->tags);
    fs->offsets = xcalloc(c->nfields + 1, sizeof *fs->offsets);
    size_t nelems = 0;
    for (uint32_t k = 0; k < c->nfields; k++) {
        fs->counts[k] = c->counts[k];
        if (c->kinds[k] == FFI_STRUCT) {
            fs->nested[k] = find_struct(vm, c->tags[k]);
            if (!fs->nested[k]) {
                ffi_free_struct(fs);
                return mlws_err(err, "E_LINK_FFI", "cstruct %s: no cstruct %s", c->name, c->tags[k]);
            }
        }
        if (c->kinds[k] == FFI_PTR) fs->tags[k] = tag_type(vm, c->tags[k]);
        nelems += c->counts[k] ? c->counts[k] : 1;
    }
    fs->elems = xcalloc(nelems + 1, sizeof *fs->elems);
    size_t e = 0, off = 0, align = 1;
    for (uint32_t k = 0; k < c->nfields; k++) {
        ffi_type *et = c->kinds[k] == FFI_STRUCT ? (ffi_type *)&fs->nested[k]->type : ctype(c->kinds[k]);
        size_t n = c->counts[k] ? c->counts[k] : 1;
        off = (off + et->alignment - 1) / et->alignment * et->alignment;
        fs->offsets[k] = off;
        for (size_t i = 0; i < n; i++) fs->elems[e++] = et;
        off += et->size * n;
        if (et->alignment > align) align = et->alignment;
    }
    fs->type.type = FFI_TYPE_STRUCT;
    fs->type.elements = fs->elems;
    /* libffi computes the struct's size and alignment when it is first used in a cif. */
    ffi_cif probe;
    if (ffi_prep_cif(&probe, FFI_DEFAULT_ABI, 0, &fs->type, NULL) != FFI_OK ||
        fs->type.size != (off + align - 1) / align * align) { /* our offsets must agree with libffi's layout */
        ffi_free_struct(fs);
        return mlws_err(err, "E_LINK_FFI", "cstruct %s: libffi cannot lay it out", c->name);
    }
    smap_put(&vm->ffi_structs, fs->name, fs);
    return true;
}

bool ffi_define_free(VM *vm, const MFfiFree *f, MlwsError *err) {
    FfiTag *t = (FfiTag *)tag_type(vm, f->tag);
    ExtLib *lib = ext_ffi_library(vm, f->lib, err);
    if (!lib) return false;
    void *fn = plat_dlsym(lib->handle, f->sym);
    if (!fn) return mlws_err(err, "E_LINK_FFI", ".ffi_free %s: symbol %s not found in \"%s\"", f->tag, f->sym, f->lib);
    void (*free_fn)(void *);
    memcpy(&free_fn, &fn, sizeof fn); /* POSIX: dlsym results are callable */
    if (t->finalize && t->finalize != free_fn)
        return mlws_err(err, "E_LINK_FFI", ".ffi_free %s: a different free function was already declared", f->tag);
    t->finalize = free_fn;
    return true;
}

/* ---- calls --------------------------------------------------------------------------- */

/* A C value of kind k as a VM value (results and out parameters). */
static Word to_vm(Process *p, uint8_t k, const CVal *v, const FfiTag *tag, const NativeDef *nd) {
    Word w = {.u = 0};
    switch (k & (uint8_t)~FFI_OUT) {
    case FFI_CSTR: w.r = v->ptr ? str_new(p, v->ptr, strlen(v->ptr)) : NULL; break;
    case FFI_PTR: w.r = v->ptr ? foreign_new_obj(p, tag, v->ptr, nd->lib) : NULL; break;
    default: w = get_num((const uint8_t *)v, k & (uint8_t)~FFI_OUT); break;
    }
    return w;
}

/* libffi returns small integers widened to ffi_arg. */
static void narrow_ret(uint8_t k, CVal *v) {
    switch (k) {
    case FFI_I8: v->i8 = (int8_t)v->sarg; break;
    case FFI_I16: v->i16 = (int16_t)v->sarg; break;
    case FFI_I32: v->i32 = (int32_t)v->sarg; break;
    case FFI_U8: v->u8 = (uint8_t)v->arg; break;
    case FFI_U16: v->u16 = (uint16_t)v->arg; break;
    case FFI_U32: v->u32 = (uint32_t)v->arg; break;
    default: break;
    }
}

static void ffi_native(Process *p, Word *args, Word *rets) {
    const NativeDef *nd = p->cur_native;
    const FfiCall *fc = nd->data;
    CVal in[FFI_MAX_ARGS], out[FFI_MAX_ARGS], ret;
    void *outp[FFI_MAX_ARGS], *avalues[FFI_MAX_ARGS];
    uint8_t *sbuf[FFI_MAX_ARGS] = {0}; /* struct temporaries */
    Obj *sorig[FFI_MAX_ARGS] = {0};    /* the MLWSVM structs passed by pointer */
    uint8_t *rbuf = NULL;
    Temps t = {{0}, 0};
    memset(in, 0, sizeof in);
    memset(out, 0, sizeof out);
    memset(&ret, 0, sizeof ret);
    int vi = 0;
    for (int k = 0; k < fc->nargs; k++) {
        uint8_t kind = fc->args[k];
        char what[24]; /* "argument N" */
        snprintf(what, sizeof what, "argument %d", vi + 1);
        if (kind == (FFI_OUT | FFI_STRUCT)) { /* filled in by the callee */
            sbuf[k] = xcalloc(1, fc->structs[k]->type.size);
            outp[k] = sbuf[k];
            avalues[k] = &outp[k];
            continue;
        }
        if (kind & FFI_OUT) {
            outp[k] = &out[k];
            avalues[k] = &outp[k];
            continue;
        }
        Word a = args[vi++];
        Obj *o = a.r;
        switch (kind) {
        case FFI_CSTR:
            if (!to_cstr(p, o, (char **)&in[k].ptr, &t, what)) goto done;
            break;
        case FFI_BUF:
            if (o && o->kind != OBJ_BYTES) {
                native_fail(p, "E_TYPE", "%s: %s must be a byte buffer", nd->name, what);
                goto done;
            }
            in[k].ptr = o ? ((ObjBytes *)o)->data : NULL;
            break;
        case FFI_PTR:
            if (!to_ptr(p, o, fc->tags[k], &in[k].ptr, what)) goto done;
            break;
        case FFI_STRUCT:
            sbuf[k] = xcalloc(1, fc->structs[k]->type.size);
            if (!pack(p, fc->structs[k], o, sbuf[k], &t)) goto done;
            avalues[k] = sbuf[k];
            continue;
        case FFI_STRUCTP:
            if (o) {
                sbuf[k] = xcalloc(1, fc->structs[k]->type.size);
                if (!pack(p, fc->structs[k], o, sbuf[k], &t)) goto done;
                sorig[k] = o;
            }
            in[k].ptr = sbuf[k];
            break;
        default: put_num((uint8_t *)&in[k], kind, a); break;
        }
        avalues[k] = &in[k];
    }
    void *rp = &ret;
    if (fc->ret == FFI_STRUCT) rp = rbuf = xcalloc(1, fc->ret_struct->type.size < sizeof(ffi_arg) ? sizeof(ffi_arg) : fc->ret_struct->type.size);
    ffi_call((ffi_cif *)&fc->cif, FFI_FN(fc->fn), rp, avalues);
    int ri = 0;
    if (fc->ret == FFI_STRUCT) {
        rets[ri++].r = unpack(p, fc->ret_struct, rbuf, NULL, nd->lib);
    } else if (fc->ret != FFI_VOID) {
        narrow_ret(fc->ret, &ret);
        rets[ri++] = to_vm(p, fc->ret, &ret, fc->ret_tag, nd);
    }
    for (int k = 0; k < fc->nargs; k++) {
        if (fc->args[k] == (FFI_OUT | FFI_STRUCT)) rets[ri++].r = unpack(p, fc->structs[k], sbuf[k], NULL, nd->lib);
        else if (fc->args[k] & FFI_OUT) rets[ri++] = to_vm(p, fc->args[k], &out[k], fc->tags[k], nd);
        else if (fc->args[k] == FFI_STRUCTP && sorig[k]) /* what the callee wrote */
            unpack(p, fc->structs[k], sbuf[k], (ObjStruct *)sorig[k], nd->lib);
    }
done:
    temps_free(&t);
    for (int k = 0; k < fc->nargs; k++) free(sbuf[k]);
    free(rbuf);
}

static char *describe(const MExtern *x) {
    StrBuf b = {0};
    sb_printf(&b, "%s:%s(%u;", x->lib, x->sym, x->nfixed);
    for (int k = 0; k < x->nargs; k++) sb_printf(&b, "%s%d/%s", k ? "," : "", x->args[k], x->tags[k] ? x->tags[k] : "");
    sb_printf(&b, ")->%d/%s flags %u", x->ret, x->ret_tag ? x->ret_tag : "", x->flags);
    return b.s;
}

bool ffi_define(VM *vm, const MExtern *x, MlwsError *err) {
    char *decl = describe(x);
    void *v;
    if (smap_get(&vm->natives, x->name, &v)) { /* declared by another module too: must agree */
        const NativeDef *old = v;
        bool same = old->fn == ffi_native && !strcmp(((const FfiCall *)old->data)->decl, decl);
        free(decl);
        if (!same) return mlws_err(err, "E_LINK_NATIVE", "extern %s conflicts with another native of that name", x->name);
        return true;
    }
    ExtLib *lib = ext_ffi_library(vm, x->lib, err);
    if (!lib) {
        free(decl);
        return false;
    }
    void *fn = plat_dlsym(lib->handle, x->sym);
    if (!fn) {
        free(decl);
        return mlws_err(err, "E_LINK_FFI", "extern %s: symbol %s not found in \"%s\"", x->name, x->sym, x->lib);
    }
    FfiCall *fc = xcalloc(1, sizeof *fc);
    fc->fn = fn;
    fc->decl = decl;
    fc->nargs = x->nargs;
    fc->ret = x->ret;
    NativeDef *nd = xcalloc(1, sizeof *nd);
    for (int k = 0; k < x->nargs; k++) {
        uint8_t kind = x->args[k], base = kind & (uint8_t)~FFI_OUT;
        fc->args[k] = kind;
        if (base == FFI_STRUCT || base == FFI_STRUCTP) {
            fc->structs[k] = find_struct(vm, x->tags[k]);
            if (!fc->structs[k]) {
                free(fc->decl), free(fc), free(nd);
                return mlws_err(err, "E_LINK_FFI", "extern %s: no cstruct %s", x->name, x->tags[k]);
            }
        }
        if (kind == FFI_STRUCT) fc->atypes[k] = (ffi_type *)&fc->structs[k]->type;
        else fc->atypes[k] = kind & FFI_OUT || base == FFI_STRUCTP ? &ffi_type_pointer : ctype(kind);
        if (base == FFI_PTR) fc->tags[k] = tag_type(vm, x->tags[k]);
        if (kind & FFI_OUT) nd->rt[nd->nr++] = (uint8_t)vmtype(kind);
        else nd->pt[nd->np++] = (uint8_t)vmtype(kind);
    }
    ffi_type *rtype = ctype(x->ret);
    if (x->ret == FFI_PTR) fc->ret_tag = tag_type(vm, x->ret_tag);
    if (x->ret == FFI_STRUCT) {
        fc->ret_struct = find_struct(vm, x->ret_tag);
        if (!fc->ret_struct) {
            free(fc->decl), free(fc), free(nd);
            return mlws_err(err, "E_LINK_FFI", "extern %s: no cstruct %s", x->name, x->ret_tag);
        }
        rtype = (ffi_type *)&fc->ret_struct->type;
    }
    if (x->ret != FFI_VOID) { /* the return value is the first result */
        memmove(nd->rt + 1, nd->rt, nd->nr);
        nd->rt[0] = (uint8_t)vmtype(x->ret);
        nd->nr++;
    }
    ffi_status st = x->nfixed < x->nargs
                        ? ffi_prep_cif_var(&fc->cif, FFI_DEFAULT_ABI, x->nfixed, x->nargs, rtype, fc->atypes)
                        : ffi_prep_cif(&fc->cif, FFI_DEFAULT_ABI, x->nargs, rtype, fc->atypes);
    if (st != FFI_OK) {
        free(fc->decl);
        free(fc);
        free(nd);
        return mlws_err(err, "E_LINK_FFI", "extern %s: libffi cannot make this call", x->name);
    }
    nd->name = xstrdup(x->name);
    StrBuf doc = {0};
    sb_printf(&doc, "C function %s from \"%s\" (.extern)", x->sym, x->lib);
    nd->doc = doc.s;
    nd->fn = ffi_native;
    nd->data = fc;
    nd->lib = lib;
    nd->flags = NF_FOREIGN | NF_CHECKED | (x->flags & MX_BLOCKING ? NF_BLOCKING : 0) |
                (x->flags & MX_SERIAL ? NF_SERIAL : 0);
    VEC_PUSH(vm->foreign_natives, nd);
    vm_register_native(vm, nd);
    return true;
}

void ffi_free_data(void *data) {
    FfiCall *fc = data;
    if (!fc) return;
    free(fc->decl);
    free(fc);
}

void ffi_free_struct(void *v) {
    FfiStruct *fs = v;
    free(fs->name), free(fs->decl), free(fs->kinds), free(fs->counts), free(fs->nested), free(fs->tags);
    free(fs->offsets), free(fs->elems), free(fs);
}

#else /* built without libffi */

bool ffi_available(void) { return false; }

bool ffi_define(VM *vm, const MExtern *x, MlwsError *err) {
    (void)vm;
    return mlws_err(err, "E_LINK_FFI", "extern %s: this mlwsvm was built without libffi, so .extern is unavailable",
                    x->name);
}

bool ffi_define_struct(VM *vm, LModule *lm, const MCStruct *c, MlwsError *err) {
    (void)vm, (void)lm;
    return mlws_err(err, "E_LINK_FFI", "cstruct %s: this mlwsvm was built without libffi", c->name);
}

bool ffi_define_free(VM *vm, const MFfiFree *f, MlwsError *err) {
    (void)vm;
    return mlws_err(err, "E_LINK_FFI", ".ffi_free %s: this mlwsvm was built without libffi", f->tag);
}

void ffi_free_data(void *data) { (void)data; }
void ffi_free_struct(void *v) { (void)v; }

#endif
