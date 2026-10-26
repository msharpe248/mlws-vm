/* ffi.c - the declarative FFI: `.extern` declarations become natives that call
 * C functions through libffi. Built only with libffi (MLWS_HAVE_FFI; the
 * Makefile finds it with pkg-config); without it `.extern` fails to link.
 *
 * Conversions (docs/EXTENDING.md, "The declarative FFI"):
 *   integers, f32, f64  by value, as the declared C type
 *   cstr   a string (its bytes, NUL-terminated) or byte buffer (copied and
 *          NUL-terminated); as a result, the char * is copied into a string
 *   buf    a byte buffer's own bytes, which the callee may change
 *   ptr    a foreign handle's pointer; as a result, a new foreign handle
 *          (without a finalizer). ptr<tag> handles only match the same tag.
 *   null   NULL, for cstr, buf and ptr
 *   out T  no argument: a pointer to a temporary, whose value is a result
 * Pointers passed in are valid only for the duration of the call. */
#include "../../include/mlwsvm/mlwsvm.h"
#include "../core/module.h"
#include "../heap/heap.h"
#include "../platform/platform.h"

#ifdef MLWS_HAVE_FFI
#include <ffi.h>

typedef struct {
    void *fn;
    ffi_cif cif;
    ffi_type *atypes[FFI_MAX_ARGS];
    uint8_t nargs, args[FFI_MAX_ARGS], ret;
    const MlwsForeignType *tags[FFI_MAX_ARGS], *ret_tag;
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

static const MlwsForeignType *tag_type(VM *vm, const char *tag) {
    const char *name = tag ? tag : "ptr";
    void *v;
    if (smap_get(&vm->ffi_types, name, &v)) return v;
    MlwsForeignType *t = xcalloc(1, sizeof *t);
    t->name = xstrdup(name);
    smap_put(&vm->ffi_types, name, t);
    return t;
}

/* A C value of kind k as a VM value (results and out parameters). */
static Word to_vm(Process *p, uint8_t k, const CVal *v, const MlwsForeignType *tag, const NativeDef *nd) {
    Word w = {.u = 0};
    switch (k & (uint8_t)~FFI_OUT) {
    case FFI_I8: w.i = v->i8; break;
    case FFI_I16: w.i = v->i16; break;
    case FFI_I32: w.i = v->i32; break;
    case FFI_I64: w.i = v->i64; break;
    case FFI_U8: w.u = v->u8; break;
    case FFI_U16: w.u = v->u16; break;
    case FFI_U32: w.u = v->u32; break;
    case FFI_U64: w.u = v->u64; break;
    case FFI_F32: w.f = v->f32; break;
    case FFI_F64: w.f = v->f64; break;
    case FFI_CSTR: w.r = v->ptr ? str_new(p, v->ptr, strlen(v->ptr)) : NULL; break;
    case FFI_PTR: w.r = v->ptr ? foreign_new_obj(p, tag, v->ptr, nd->lib) : NULL; break;
    default: break;
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
    char *copies[FFI_MAX_ARGS] = {0};
    memset(in, 0, sizeof in);
    memset(out, 0, sizeof out);
    memset(&ret, 0, sizeof ret);
    int vi = 0;
    for (int k = 0; k < fc->nargs; k++) {
        uint8_t kind = fc->args[k];
        if (kind & FFI_OUT) {
            outp[k] = &out[k];
            avalues[k] = &outp[k];
            continue;
        }
        Word a = args[vi++];
        Obj *o = a.r;
        switch (kind) {
        case FFI_I8: in[k].i8 = (int8_t)a.i; break;
        case FFI_I16: in[k].i16 = (int16_t)a.i; break;
        case FFI_I32: in[k].i32 = (int32_t)a.i; break;
        case FFI_I64: in[k].i64 = a.i; break;
        case FFI_U8: in[k].u8 = (uint8_t)a.u; break;
        case FFI_U16: in[k].u16 = (uint16_t)a.u; break;
        case FFI_U32: in[k].u32 = (uint32_t)a.u; break;
        case FFI_U64: in[k].u64 = a.u; break;
        case FFI_F32: in[k].f32 = (float)a.f; break;
        case FFI_F64: in[k].f64 = a.f; break;
        case FFI_CSTR:
            if (!o) in[k].ptr = NULL;
            else if (o->kind == OBJ_STRING) in[k].ptr = ((ObjString *)o)->data;
            else if (o->kind == OBJ_BYTES) {
                ObjBytes *b = (ObjBytes *)o;
                copies[k] = xmalloc(b->len + 1);
                memcpy(copies[k], b->data, b->len);
                copies[k][b->len] = 0;
                in[k].ptr = copies[k];
            } else {
                native_fail(p, "E_TYPE", "%s: argument %d must be a string or byte buffer", nd->name, vi);
                goto done;
            }
            break;
        case FFI_BUF:
            if (o && o->kind != OBJ_BYTES) {
                native_fail(p, "E_TYPE", "%s: argument %d must be a byte buffer", nd->name, vi);
                goto done;
            }
            in[k].ptr = o ? ((ObjBytes *)o)->data : NULL;
            break;
        case FFI_PTR:
            if (o && (o->kind != OBJ_FOREIGN || ((ObjForeign *)o)->type != fc->tags[k])) {
                native_fail(p, "E_TYPE", "%s: argument %d must be a foreign %s", nd->name, vi, fc->tags[k]->name);
                goto done;
            }
            if (o && !((ObjForeign *)o)->ptr) {
                native_fail(p, "E_TYPE", "%s: argument %d: the foreign %s has been released", nd->name, vi,
                            fc->tags[k]->name);
                goto done;
            }
            in[k].ptr = o ? ((ObjForeign *)o)->ptr : NULL;
            break;
        default: break;
        }
        avalues[k] = &in[k];
    }
    ffi_call((ffi_cif *)&fc->cif, FFI_FN(fc->fn), &ret, avalues);
    int ri = 0;
    if (fc->ret != FFI_VOID) {
        narrow_ret(fc->ret, &ret);
        rets[ri++] = to_vm(p, fc->ret, &ret, fc->ret_tag, nd);
    }
    for (int k = 0; k < fc->nargs; k++)
        if (fc->args[k] & FFI_OUT) rets[ri++] = to_vm(p, fc->args[k], &out[k], fc->tags[k], nd);
done:
    for (int k = 0; k < fc->nargs; k++) free(copies[k]);
}

static char *describe(const MExtern *x) {
    StrBuf b = {0};
    sb_printf(&b, "%s:%s(", x->lib, x->sym);
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
        fc->args[k] = x->args[k];
        fc->atypes[k] = x->args[k] & FFI_OUT ? &ffi_type_pointer : ctype(x->args[k]);
        if ((x->args[k] & (uint8_t)~FFI_OUT) == FFI_PTR) fc->tags[k] = tag_type(vm, x->tags[k]);
        if (x->args[k] & FFI_OUT) nd->rt[nd->nr++] = (uint8_t)vmtype(x->args[k]);
        else nd->pt[nd->np++] = (uint8_t)vmtype(x->args[k]);
    }
    if (x->ret == FFI_PTR) fc->ret_tag = tag_type(vm, x->ret_tag);
    if (x->ret != FFI_VOID) { /* the return value is the first result */
        memmove(nd->rt + 1, nd->rt, nd->nr);
        nd->rt[0] = (uint8_t)vmtype(x->ret);
        nd->nr++;
    }
    if (ffi_prep_cif(&fc->cif, FFI_DEFAULT_ABI, x->nargs, ctype(x->ret), fc->atypes) != FFI_OK) {
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
    nd->flags = NF_FOREIGN | (x->flags & MX_BLOCKING ? NF_BLOCKING : 0) | (x->flags & MX_SERIAL ? NF_SERIAL : 0);
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

#else /* built without libffi */

bool ffi_available(void) { return false; }

bool ffi_define(VM *vm, const MExtern *x, MlwsError *err) {
    (void)vm;
    return mlws_err(err, "E_LINK_FFI", "extern %s: this mlwsvm was built without libffi, so .extern is unavailable",
                    x->name);
}

void ffi_free_data(void *data) { (void)data; }

#endif
