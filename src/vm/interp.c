/* interp.c - the instruction interpreter.
 *
 * Code is pre-decoded by the loader into Insn arrays with resolved operands, so
 * the loop below only evaluates addressing modes and dispatches on the dense
 * opcode. Every fault traps (longjmp) out through vm_trap with a stable error
 * code; the verifier has already checked everything static. */
#include <ctype.h>
#include <errno.h>
#include <math.h>

#include "../heap/heap.h"
#include "../heap/numeric.h"
#include "../platform/platform.h"
#include "proc_internal.h"

/* Tail-call dispatch (see run_loop): each quickened form is a function that
 * ends by tail-calling the next instruction's, which needs a compiler that can
 * guarantee the tail call. Other compilers, and MLWS_NO_TAILCALL builds, use
 * one dispatch loop. */
/* ThreadSanitizer builds use the loop: with the tail-call chain, its runtime
 * failed an internal check (in its allocator) after the tests ran. */
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define MLWS_NO_TAILCALL 1
#endif
#endif
#if defined(__SANITIZE_THREAD__) && !defined(MLWS_NO_TAILCALL)
#define MLWS_NO_TAILCALL 1
#endif
#if defined(__has_attribute) && !defined(MLWS_NO_TAILCALL) && !defined(MLWS_NO_THREADED)
#if __has_attribute(musttail)
#define MLWS_TAILCALL 1
#endif
#endif
#ifndef MLWS_TAILCALL
#define MLWS_TAILCALL 0
#endif

typedef struct {
    Process *p;
    Frame *fr;
    int64_t *I;
    double *F;
    Obj **R;
    uint64_t *P;
#if MLWS_TAILCALL
    /* the slice, as run_loop's locals hold it in the dispatch loop */
    VM *vm;
    const Insn *code;  /* the current frame's function's code */
    uint64_t ibase;    /* Process.insns = ibase - left */
    uint64_t insns0;   /* Process.insns at the start of the slice */
    uint32_t budget;   /* instructions the slice may run */
    uint32_t hleft;    /* with a tool's hook: instructions left (the handlers' `left` stays 0) */
    bool hooked;
#endif
} Ctx;

/* The current frame, for code where `fr` names another variable (interp_run's generic forms). */
static inline Frame *ctx_frame(const Ctx *c) { return c->fr; }

/* The register windows of frame fr, whose function is fn and whose window
 * starts at w: the call handlers know all three. */
static inline void ctx_set(Ctx *c, Frame *fr, const Function *fn, uint64_t *w) {
    c->fr = fr;
    c->I = (int64_t *)w; /* roff[BANK_I] is 0 */
    c->F = (double *)(w + fn->roff[BANK_F]);
    c->R = (Obj **)(w + fn->roff[BANK_R]);
    c->P = w + fn->roff[BANK_P];
}

static void ctx_load(Ctx *c) {
    Process *p = c->p;
    Frame *fr = &p->frames[p->nframes - 1];
    ctx_set(c, fr, fr->fn, p->regs + fr->base);
}

/* A resolved operand location: segment memory, a register, a Word slot of a
 * heap object (array/struct with a fixed type, or list/map with a tag), or a
 * byte of a string or byte buffer. */
enum { LK_MEM = 0, LK_REG = 1, LK_SLOT = 2, LK_BYTE = 3 };

typedef struct {
    uint8_t is_reg; /* LK_* */
    uint8_t reg;
    Segment *seg;
    uint32_t off;
    Word *slot;
    uint8_t *tag;   /* LK_SLOT with a per-element tag (lists, map values) */
    uint8_t fixed;  /* LK_SLOT with a fixed type (arrays, struct fields) */
    uint8_t ro;     /* LK_BYTE of an immutable string */
    uint8_t *byte;
} Loc;

/* Location of the memory a pointer designates (checks the segment). */
static Loc mem_loc(Process *p, uint64_t ptr) {
    Loc L;
    memset(&L, 0, sizeof L);
    L.is_reg = LK_MEM;
    L.seg = proc_segment_fast(p, ptr);
    L.off = PTR_OFF(ptr);
    return L;
}

/* ---- registers ---------------------------------------------------------------- */

static uint64_t reg_ptr(Ctx *c, uint8_t reg) {
    if (REG_BANK(reg) == BANK_P) return c->P[REG_IDX(reg)];
    return REG_IDX(reg) == SREG_SP ? c->p->sp : c->p->fp;
}

static void note_sp(Process *p) {
    if (PTR_SEG(p->sp) == SEG_STACK && PTR_OFF(p->sp) < p->stack_low_off) p->stack_low_off = PTR_OFF(p->sp);
}

static void set_reg_ptr(Ctx *c, uint8_t reg, uint64_t v) {
    if (REG_BANK(reg) == BANK_P) {
        c->P[REG_IDX(reg)] = v;
    } else if (REG_IDX(reg) == SREG_SP) {
        c->p->sp = v;
        note_sp(c->p);
    } else {
        c->p->fp = v;
    }
}

static Word reg_read(Ctx *c, uint8_t reg, MlwsType t) {
    Word w;
    int idx = REG_IDX(reg);
    switch (REG_BANK(reg)) {
    case BANK_I: w.i = t != T_I64 && t != T_U64 && t != T_NONE ? canon_int(t, c->I[idx]) : c->I[idx]; break;
    case BANK_F: w.f = t == T_F32 ? (double)(float)c->F[idx] : c->F[idx]; break;
    case BANK_R: w.r = c->R[idx]; break;
    case BANK_P: w.p = c->P[idx]; break;
    default:
        w.p = idx == SREG_SP ? c->p->sp : idx == SREG_FP ? c->p->fp : c->p->pid;
        break;
    }
    return w;
}

static void reg_write(Ctx *c, uint8_t reg, MlwsType t, Word w) {
    int idx = REG_IDX(reg);
    int bank = REG_BANK(reg);
    char b[8];
    if (bank == BANK_S) {
        if (t != T_P) vm_trap(c->p, "E_TYPE", "cannot store a %s value in %s", type_name(t), reg_name(reg, b));
        set_reg_ptr(c, reg, w.p);
        return;
    }
    if (type_bank(t) != (Bank)bank)
        vm_trap(c->p, "E_TYPE", "cannot store a %s value in %s", type_name(t), reg_name(reg, b));
    switch (bank) {
    case BANK_I: c->I[idx] = canon_int(t, w.i); break;
    case BANK_F: c->F[idx] = canon_float(t, w.f); break;
    case BANK_R: c->R[idx] = w.r; break;
    default: c->P[idx] = w.p; break;
    }
}

/* ---- addresses ---------------------------------------------------------------- */

static uint64_t ptr_add(Ctx *c, uint64_t ptr, int64_t delta) {
    int64_t off = (int64_t)PTR_OFF(ptr) + delta;
    if (off < 0 || off > (int64_t)UINT32_MAX) {
        if (PTR_SEG(ptr) == SEG_STACK && off < 0) vm_trap(c->p, "E_STACK_OVERFLOW", "data stack overflow");
        vm_trap(c->p, "E_BOUNDS", "pointer arithmetic leaves its block (offset %lld)", (long long)off);
    }
    return (ptr & ~(uint64_t)0xFFFFFFFFu) | (uint32_t)off;
}

static Word mem_read(Ctx *c, const Loc *L, MlwsType t);

/* Compute the pointer an operand designates, applying auto-increment side
 * effects. `width` is the access size, used by the byte auto-step modes. */
static uint64_t op_addr(Ctx *c, const Operand *o, int width) {
    uint64_t a;
    switch (o->mode) {
    case OM_IND: return reg_ptr(c, o->reg);
    case OM_DISP: case OM_DISPB: return ptr_add(c, reg_ptr(c, o->reg), o->bdisp);
    case OM_IDX: case OM_IDXB:
        return ptr_add(c, reg_ptr(c, o->reg), (int64_t)((uint64_t)c->I[REG_IDX(o->ireg)] * (uint64_t)o->bscale) + o->bdisp);
    case OM_POSTINC: case OM_POSTINCB:
        a = reg_ptr(c, o->reg);
        set_reg_ptr(c, o->reg, ptr_add(c, a, o->mode == OM_POSTINC ? 8 : width));
        return a;
    case OM_PREDEC: case OM_PREDECB:
        a = ptr_add(c, reg_ptr(c, o->reg), o->mode == OM_PREDEC ? -8 : -width);
        set_reg_ptr(c, o->reg, a);
        return a;
    case OM_GLOBAL: case OM_GLOBALB: return ptr_add(c, o->rv.gbase, o->bdisp);
    case OM_GLOBALIDX: case OM_GLOBALIDXB:
        return ptr_add(c, o->rv.gbase, (int64_t)((uint64_t)c->I[REG_IDX(o->ireg)] * (uint64_t)o->bscale) + o->bdisp);
    case OM_MIND: case OM_MINDB: {
        uint64_t ia = op_addr(c, o->inner, 8);
        Loc L = mem_loc(c->p, ia);
        return ptr_add(c, mem_read(c, &L, T_P).p, o->bdisp);
    }
    default: vm_trap(c->p, "E_INTERNAL", "operand is not a memory location");
    }
}

static const char *obj_desc(const Obj *o) {
    return o->kind == OBJ_STRUCT ? ((const ObjStruct *)o)->def->name : obj_kind_name(o->kind);
}

static Obj *obj_reg(Ctx *c, uint8_t reg) {
    Obj *o = c->R[REG_IDX(reg)];
    char b[8];
    if (!o) vm_trap(c->p, "E_NULL", "%s is null", reg_name(reg, b));
    return o;
}

static void obj_loc(Ctx *c, const Operand *o, Loc *L) {
    L->tag = NULL;
    L->fixed = T_NONE;
    L->ro = 0;
    if (o->mode == OM_UPVAL) {
        ObjClosure *cl = (ObjClosure *)c->fr->closure;
        if (!cl) vm_trap(c->p, "E_TYPE", "UP[%llu] used in %s, which was not called as a closure", (unsigned long long)o->index, c->fr->fn->name);
        if (o->index >= cl->nup)
            vm_trap(c->p, "E_BOUNDS", "UP[%llu] out of range: the closure captured %u value(s)", (unsigned long long)o->index, cl->nup);
        L->is_reg = LK_SLOT;
        L->slot = &cl->up[o->index];
        L->tag = &cl->tags[o->index];
        return;
    }
    Obj *ob = obj_reg(c, o->reg);
    if (o->mode == OM_FIELD) {
        const FieldRef *fr = o->rv.ptr;
        if (ob->kind != OBJ_STRUCT || ((ObjStruct *)ob)->def != fr->def)
            vm_trap(c->p, "E_TYPE", "expected a %s struct, got %s", fr->def->name, obj_desc(ob));
        L->is_reg = LK_SLOT;
        L->slot = &((ObjStruct *)ob)->fields[fr->index];
        L->fixed = fr->def->types[fr->index];
        return;
    }
    int64_t idx = o->mode == OM_ELEM ? (int64_t)((uint64_t)c->I[REG_IDX(o->ireg)] + (uint64_t)o->imm) : (int64_t)o->index;
    uint32_t len;
    switch (ob->kind) {
    case OBJ_ARRAY: len = ((ObjArray *)ob)->len; break;
    case OBJ_LIST: len = ((ObjList *)ob)->len; break;
    case OBJ_BYTES: len = ((ObjBytes *)ob)->len; break;
    case OBJ_STRING: len = ((ObjString *)ob)->len; break;
    default: vm_trap(c->p, "E_TYPE", "cannot index a %s", obj_desc(ob));
    }
    if (idx < 0 || idx >= (int64_t)len)
        vm_trap(c->p, "E_BOUNDS", "index %lld out of range for %s of length %u", (long long)idx, obj_desc(ob), len);
    switch (ob->kind) {
    case OBJ_ARRAY:
        L->is_reg = LK_SLOT;
        L->slot = &((ObjArray *)ob)->data[idx];
        L->fixed = ((ObjArray *)ob)->elem;
        break;
    case OBJ_LIST:
        L->is_reg = LK_SLOT;
        L->slot = &((ObjList *)ob)->items[idx];
        L->tag = &((ObjList *)ob)->tags[idx];
        break;
    case OBJ_BYTES:
        L->is_reg = LK_BYTE;
        L->byte = &((ObjBytes *)ob)->data[idx];
        break;
    default:
        L->is_reg = LK_BYTE;
        L->byte = (uint8_t *)&((ObjString *)ob)->data[idx];
        L->ro = 1;
        break;
    }
}

static void loc_of(Ctx *c, const Operand *o, int width, Loc *L) {
    if (o->mode == OM_REG) {
        L->is_reg = 1;
        L->reg = o->reg;
        return;
    }
    if (mode_is_object(o->mode)) {
        obj_loc(c, o, L);
        return;
    }
    uint64_t a = op_addr(c, o, width);
    L->is_reg = 0;
    L->seg = proc_segment_fast(c->p, a);
    L->off = PTR_OFF(a);
}

/* ---- memory --------------------------------------------------------------- */

static uint8_t *mem_ptr(Ctx *c, const Loc *L, int width) {
    if ((uint64_t)L->off + (uint64_t)width > L->seg->size) {
        if (L->seg->kind == SEGK_STACK && L->off >= L->seg->size)
            vm_trap(c->p, "E_STACK_UNDERFLOW", "read or write above the top of the data stack (pop of an empty stack?)");
        vm_trap(c->p, "E_BOUNDS", "%d-byte access at offset %u of a %u-byte %s", width, L->off, L->seg->size,
                L->seg->kind == SEGK_RAW ? "raw block" : L->seg->kind == SEGK_STACK ? "stack" : "global segment");
    }
    return L->seg->data + L->off;
}

static MlwsType mem_natural(const Loc *L) {
    if (L->seg->tags && L->off % 8 == 0) {
        uint8_t t = L->seg->tags[L->off / 8];
        if (t != T_NONE && t != TAG_RAW && t < T__COUNT) return (MlwsType)t;
    }
    return T_I64;
}

/* VM memory is little-endian on every host. */
static uint64_t ld_le(const uint8_t *m, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n; i++) v |= (uint64_t)m[i] << (8 * i);
    return v;
}

static void st_le(uint8_t *m, uint64_t v, int n) {
    for (int i = 0; i < n; i++) m[i] = (uint8_t)(v >> (8 * i));
}

/* True if [off, off+width) overlaps a word tagged as a reference. */
static bool overlaps_ref(const Loc *L, int width) {
    if (!L->seg->tags) return false;
    for (uint32_t w = L->off / 8; w <= (L->off + (uint32_t)width - 1) / 8; w++)
        if (L->seg->tags[w] == T_R) return true;
    return false;
}

static Word mem_read(Ctx *c, const Loc *L, MlwsType t) {
    Word w;
    int width = type_size(t);
    const uint8_t *m = mem_ptr(c, L, width);
    if (t == T_R) {
        uint8_t tag = L->seg->tags && L->off % 8 == 0 ? L->seg->tags[L->off / 8] : TAG_RAW;
        if (tag == T_NONE) { /* never-written word: null */
            w.r = NULL;
            return w;
        }
        if (tag != T_R) vm_trap(c->p, "E_TYPE", "memory at offset %u does not hold a reference", L->off);
        memcpy(&w.r, m, sizeof w.r); /* host pointer; never observable as an integer */
        return w;
    }
    if (overlaps_ref(L, width))
        vm_trap(c->p, "E_TYPE", "cannot read a reference slot as %s", type_name(t));
    uint64_t bits = ld_le(m, width);
    switch (t) {
    case T_B: w.i = bits != 0; break;
    case T_I8: w.i = (int8_t)bits; break;
    case T_I16: w.i = (int16_t)bits; break;
    case T_I32: w.i = (int32_t)bits; break;
    case T_F32: {
        uint32_t u = (uint32_t)bits;
        float f;
        memcpy(&f, &u, 4);
        w.f = f;
        break;
    }
    case T_F64: memcpy(&w.f, &bits, 8); break;
    default: w.u = bits; break; /* unsigned, char, i64, p */
    }
    return w;
}

/* The collector scans the stack from the lowest word ever tagged as a reference.
 * Out of line: inside mem_write, this one comparison made GCC's build run
 * bench/loop.masm (which never stores a reference) 27% slower. */
static MLWS_COLD void note_stack_ref(Process *p, uint32_t word) {
    if (word < p->stack_ref_lo) p->stack_ref_lo = word;
}

static void mem_write(Ctx *c, const Loc *L, MlwsType t, Word w) {
    int width = type_size(t);
    uint8_t *m = mem_ptr(c, L, width);
    if (t == T_R) {
        if (!L->seg->tags || L->off % 8)
            vm_trap(c->p, "E_TYPE", "references can only be stored in word-aligned stack or global slots");
        memcpy(m, &w.r, sizeof w.r);
        if (L->seg->kind == SEGK_STACK) note_stack_ref(c->p, L->off / 8);
    } else if (t == T_F32) {
        float f = (float)w.f;
        uint32_t u;
        memcpy(&u, &f, 4);
        st_le(m, u, 4);
    } else if (t == T_F64) {
        uint64_t u;
        memcpy(&u, &w.f, 8);
        st_le(m, u, 8);
    } else {
        st_le(m, w.u, width);
    }
    if (L->seg->tags) {
        uint32_t first = L->off / 8, last = (L->off + (uint32_t)width - 1) / 8;
        if (L->off % 8 == 0) {
            L->seg->tags[first] = (uint8_t)t;
        } else {
            for (uint32_t i = first; i <= last; i++) L->seg->tags[i] = TAG_RAW;
        }
    }
}

static void mark_raw(Segment *s, uint32_t off, uint64_t n) {
    if (!s->tags || n == 0) return;
    for (uint64_t i = off / 8; i <= (off + n - 1) / 8; i++) s->tags[i] = TAG_RAW;
}

/* ---- operand values --------------------------------------------------------- */

/* Same register bank, and references never mix with non-references. */
static bool same_class(MlwsType a, MlwsType b) { return type_bank(a) == type_bank(b) && type_is_ref(a) == type_is_ref(b); }

/* BigInt and decimal values are plain references when moved around. */
#define AS_REF(t) ((t) == T_BIG || (t) == T_DEC ? T_R : (t))

static Word slot_read(Ctx *c, const Loc *L, MlwsType *t) {
    MlwsType have = L->fixed ? (MlwsType)L->fixed : (MlwsType)*L->tag;
    Word w;
    if (have == T_NONE) { /* never written: zero of any type */
        if (*t == T_NONE) *t = T_I64;
        w.u = 0;
        return w;
    }
    if (*t == T_NONE) *t = have;
    if (!same_class(have, *t)) vm_trap(c->p, "E_TYPE", "element holds %s but is read as %s", type_name(have), type_name(*t));
    w = *L->slot;
    if (type_is_int(*t)) w.i = canon_int(*t, w.i);
    else if (type_is_float(*t)) w.f = canon_float(*t, w.f);
    return w;
}

static void slot_write(Ctx *c, const Loc *L, MlwsType t, Word w) {
    if (L->fixed) {
        MlwsType f = (MlwsType)L->fixed;
        if (!same_class(f, t)) vm_trap(c->p, "E_TYPE", "cannot store %s in a %s element or field", type_name(t), type_name(f));
        if (type_is_int(f)) w.i = canon_int(f, w.i);
        else if (type_is_float(f)) w.f = canon_float(f, w.f);
    } else {
        *L->tag = (uint8_t)t;
    }
    *L->slot = w;
}

static Word rd_loc(Ctx *c, const Loc *L, MlwsType *t) {
    *t = AS_REF(*t);
    switch (L->is_reg) {
    case LK_REG:
        if (*t == T_NONE) *t = reg_natural_type(L->reg);
        return reg_read(c, L->reg, *t);
    case LK_SLOT: return slot_read(c, L, t);
    case LK_BYTE: {
        if (*t == T_NONE) *t = T_U8;
        if (!type_is_int(*t)) vm_trap(c->p, "E_TYPE", "bytes are integers, not %s", type_name(*t));
        Word w;
        w.i = canon_int(*t, *L->byte);
        return w;
    }
    default:
        if (*t == T_NONE) *t = mem_natural(L);
        return mem_read(c, L, *t);
    }
}

static void wr_loc(Ctx *c, const Loc *L, MlwsType t, Word w) {
    t = AS_REF(t);
    switch (L->is_reg) {
    case LK_REG: reg_write(c, L->reg, t, w); break;
    case LK_SLOT: slot_write(c, L, t, w); break;
    case LK_BYTE:
        if (L->ro) vm_trap(c->p, "E_TYPE", "strings are immutable (use a byte buffer)");
        if (!type_is_int(t)) vm_trap(c->p, "E_TYPE", "bytes are integers, not %s", type_name(t));
        *L->byte = (uint8_t)w.i;
        break;
    default: mem_write(c, L, t, w); break;
    }
}

/* 64-bit types: values of these need no width canonicalisation. */
static bool wide_type(int t) { return t == T_I64 || t == T_U64 || t == T_F64 || t == T_R || t == T_P; }

/* Read an operand as type t; T_NONE reads its natural type and reports it. */
static Word rdn(Ctx *c, const Operand *o, MlwsType *t) {
    Word w;
    *t = AS_REF(*t);
    switch (o->mode) {
    case OM_REG:
        if (*t == T_NONE) *t = reg_natural_type(o->reg);
        return reg_read(c, o->reg, *t);
    case OM_IMM:
        if (*t == T_NONE) *t = T_I64;
        if (type_is_float(*t)) w.f = canon_float(*t, (double)o->imm);
        else w.i = canon_int(*t, o->imm);
        return w;
    case OM_FIMM:
        if (*t == T_NONE) *t = T_F64;
        w.f = canon_float(*t, o->f);
        return w;
    case OM_CONST:
        *t = T_R;
        w.r = o->rv.ptr;
        return w;
    case OM_ELEM: { /* shortcut: an array element of exactly this type, in range */
        const ObjArray *a = (const ObjArray *)c->R[REG_IDX(o->reg)];
        uint64_t i = (uint64_t)c->I[REG_IDX(o->ireg)] + (uint64_t)o->imm;
        if (a && a->h.kind == OBJ_ARRAY && a->elem == *t && i < a->len) return a->data[i];
        break;
    }
    case OM_FIELD: { /* shortcut: a field of exactly this type */
        const ObjStruct *so = (const ObjStruct *)c->R[REG_IDX(o->reg)];
        const FieldRef *f = o->rv.ptr;
        if (so && so->h.kind == OBJ_STRUCT && so->def == f->def && f->def->types[f->index] == *t)
            return so->fields[f->index];
        break;
    }
    default: break;
    }
    Loc L;
    loc_of(c, o, *t ? type_size(*t) : 8, &L);
    return rd_loc(c, &L, t);
}

static Word rd(Ctx *c, const Operand *o, MlwsType t) { return rdn(c, o, &t); }

static void wr(Ctx *c, const Operand *o, MlwsType t, Word w) {
    t = AS_REF(t);
    if (o->mode == OM_REG) {
        reg_write(c, o->reg, t, w);
        return;
    }
    if (o->mode == OM_ELEM) { /* shortcut: an array element of exactly this type, in range */
        ObjArray *a = (ObjArray *)c->R[REG_IDX(o->reg)];
        uint64_t i = (uint64_t)c->I[REG_IDX(o->ireg)] + (uint64_t)o->imm;
        if (a && a->h.kind == OBJ_ARRAY && a->elem == t && i < a->len && wide_type(t)) {
            a->data[i] = w;
            return;
        }
    } else if (o->mode == OM_FIELD) { /* shortcut: a field of exactly this type */
        ObjStruct *so = (ObjStruct *)c->R[REG_IDX(o->reg)];
        const FieldRef *f = o->rv.ptr;
        if (so && so->h.kind == OBJ_STRUCT && so->def == f->def && f->def->types[f->index] == t && wide_type(t)) {
            so->fields[f->index] = w;
            return;
        }
    }
    Loc L;
    loc_of(c, o, type_size(t), &L);
    wr_loc(c, &L, t, w);
}

/* ---- data stack --------------------------------------------------------------- */

static void push_word(Ctx *c, MlwsType t, Word v) {
    Process *p = c->p;
    uint64_t sp = ptr_add(c, p->sp, -8);
    Loc L = mem_loc(p, sp);
    memset(mem_ptr(c, &L, 8), 0, 8);
    mem_write(c, &L, t, v);
    p->sp = sp;
    note_sp(p);
}

/* ---- arithmetic helpers ---------------------------------------------------------- */

static uint64_t width_mask(MlwsType t) {
    int bits = type_bits(t);
    return bits == 64 ? ~(uint64_t)0 : (((uint64_t)1 << bits) - 1);
}

static Word binop(Ctx *c, int op, MlwsType t, Word a, Word b) {
    Word r;
    if (type_is_float(t)) {
        double x = a.f, y = b.f, z;
        switch (op) {
        case OP_ADD: z = x + y; break;
        case OP_SUB: z = x - y; break;
        case OP_MUL: z = x * y; break;
        case OP_DIV: z = x / y; break;
        case OP_REM: z = fmod(x, y); break;
        case OP_MIN: z = (isnan(x) || isnan(y)) ? NAN : (x < y ? x : y); break;
        default: z = (isnan(x) || isnan(y)) ? NAN : (x > y ? x : y); break;
        }
        r.f = canon_float(t, z);
        return r;
    }
    bool u = !type_is_signed(t);
    uint64_t x = a.u, y = b.u;
    switch (op) {
    case OP_ADD: r.u = x + y; break;
    case OP_SUB: r.u = x - y; break;
    case OP_MUL: r.u = x * y; break;
    case OP_DIV:
    case OP_REM:
        if (y == 0) vm_trap(c->p, "E_DIV_ZERO", "integer division by zero");
        if (u) r.u = op == OP_DIV ? x / y : x % y;
        else if (a.i == INT64_MIN && b.i == -1) r.i = op == OP_DIV ? INT64_MIN : 0;
        else r.i = op == OP_DIV ? a.i / b.i : a.i % b.i;
        break;
    case OP_MIN: r = (u ? x < y : a.i < b.i) ? a : b; break;
    case OP_MAX: r = (u ? x > y : a.i > b.i) ? a : b; break;
    case OP_AND: r.u = x & y; break;
    case OP_OR: r.u = x | y; break;
    default: r.u = x ^ y; break;
    }
    r.i = canon_int(t, r.i);
    return r;
}

static void arith(Ctx *c, const Insn *in) {
    MlwsType t = (MlwsType)in->type;
    const Operand *ops = in->ops;
    Loc d;
    loc_of(c, &ops[0], type_size(t), &d);
    if (t == T_P) {
        MlwsType pt = T_P;
        uint64_t base = in->nops == 2 ? rd_loc(c, &d, &pt).p : rd(c, &ops[1], T_P).p;
        int64_t delta = 0;
        for (int k = in->nops == 2 ? 1 : 2; k < in->nops; k++) {
            int64_t v = rd(c, &ops[k], T_I64).i;
            delta = (int64_t)(in->op == OP_ADD ? (uint64_t)delta + (uint64_t)v : (uint64_t)delta - (uint64_t)v);
        }
        Word w;
        w.p = ptr_add(c, base, delta);
        wr_loc(c, &d, T_P, w);
        return;
    }
    Word acc;
    int k;
    if (in->nops == 2) {
        MlwsType tt = t;
        acc = rd_loc(c, &d, &tt);
        k = 1;
    } else {
        acc = rd(c, &ops[1], t);
        k = 2;
    }
    for (; k < in->nops; k++) acc = binop(c, in->op, t, acc, rd(c, &ops[k], t));
    wr_loc(c, &d, t, acc);
}

static int popcount64(uint64_t v) {
    int n = 0;
    while (v) {
        v &= v - 1;
        n++;
    }
    return n;
}

static Word unop(Ctx *c, int op, MlwsType t, Word a) {
    Word r;
    (void)c;
    if (type_is_float(t)) {
        double x = a.f, z;
        switch (op) {
        case OP_NEG: z = -x; break;
        case OP_ABS: z = fabs(x); break;
        case OP_SQRT: z = sqrt(x); break;
        case OP_FLOOR: z = floor(x); break;
        case OP_CEIL: z = ceil(x); break;
        case OP_ROUND: z = round(x); break;
        default: z = trunc(x); break;
        }
        r.f = canon_float(t, z);
        return r;
    }
    int bits = type_bits(t);
    uint64_t v = a.u & width_mask(t);
    switch (op) {
    case OP_NEG: r.u = 0 - a.u; break;
    case OP_ABS: r.u = (type_is_signed(t) && a.i < 0) ? 0 - a.u : a.u; break;
    case OP_NOT: r.u = t == T_B ? (uint64_t)(a.u == 0) : ~a.u; break;
    case OP_POPCNT: r.i = popcount64(v); break;
    case OP_CLZ: {
        int n = 0;
        for (int i = bits - 1; i >= 0 && !((v >> i) & 1); i--) n++;
        r.i = n;
        break;
    }
    case OP_CTZ: {
        int n = 0;
        while (n < bits && !((v >> n) & 1)) n++;
        r.i = n;
        break;
    }
    default: { /* BSWAP */
        uint64_t out = 0;
        int bytes = bits / 8;
        for (int i = 0; i < bytes; i++) out |= ((v >> (8 * i)) & 0xFF) << (8 * (bytes - 1 - i));
        r.u = out;
        break;
    }
    }
    r.i = canon_int(t, r.i);
    return r;
}

static Word shiftop(int op, MlwsType t, Word a, Word n) {
    int bits = type_bits(t);
    uint64_t mask = width_mask(t);
    unsigned s = (unsigned)(n.u % (uint64_t)bits);
    uint64_t v = a.u & mask;
    Word r;
    switch (op) {
    case OP_SHL: r.u = v << s; break;
    case OP_SHR: r.u = v >> s; break;
    case OP_SAR: {
        int64_t sv = (int64_t)(v << (64 - bits)) >> (64 - bits);
        r.i = sv >> s;
        break;
    }
    case OP_ROL: r.u = s ? ((v << s) | (v >> (bits - (int)s))) & mask : v; break;
    default: r.u = s ? ((v >> s) | (v << (bits - (int)s))) & mask : v; break;
    }
    r.i = canon_int(t, r.i);
    return r;
}

static bool cmp_eq(MlwsType t, Word a, Word b) {
    if (type_is_float(t)) return a.f == b.f;
    if (t == T_R) return a.r == b.r;
    return a.u == b.u;
}

static bool cmp_lt(MlwsType t, Word a, Word b) {
    if (type_is_float(t)) return a.f < b.f;
    if (type_is_signed(t)) return a.i < b.i;
    return a.u < b.u;
}

static bool cmp_le(MlwsType t, Word a, Word b) {
    if (type_is_float(t)) return a.f <= b.f;
    if (type_is_signed(t)) return a.i <= b.i;
    return a.u <= b.u;
}

static bool compare(int op, MlwsType t, Word a, Word b) {
    switch (op) {
    case OP_SEQ: case OP_BEQ: return cmp_eq(t, a, b);
    case OP_SNE: case OP_BNE: return !cmp_eq(t, a, b);
    case OP_SLT: case OP_BLT: return cmp_lt(t, a, b);
    case OP_SLE: case OP_BLE: return cmp_le(t, a, b);
    case OP_SGT: case OP_BGT: return cmp_lt(t, b, a);
    default: return cmp_le(t, b, a);
    }
}

static Word convert(MlwsType to, MlwsType from, Word v) {
    Word r;
    if (type_is_float(from)) {
        double d = v.f;
        if (type_is_float(to)) {
            r.f = canon_float(to, d);
        } else if (to == T_B) {
            r.i = d != 0;
        } else if (isnan(d)) {
            r.i = 0;
        } else {
            d = trunc(d);
            int bits = type_bits(to);
            if (type_is_signed(to)) {
                double hi = ldexp(1.0, bits - 1); /* exclusive */
                if (d >= hi) r.i = (int64_t)((((uint64_t)1) << (bits - 1)) - 1);
                else if (d < -hi) r.i = (int64_t)(0 - (((uint64_t)1) << (bits - 1)));
                else r.i = (int64_t)d;
            } else {
                double hi = ldexp(1.0, bits);
                if (d <= 0) r.u = 0;
                else if (d >= hi) r.u = width_mask(to);
                else r.u = (uint64_t)d;
            }
            r.i = canon_int(to, r.i);
        }
        return r;
    }
    if (type_is_float(to)) r.f = canon_float(to, from == T_U64 ? (double)v.u : (double)v.i);
    else r.i = canon_int(to, v.i);
    return r;
}

static Word bitcast(MlwsType to, MlwsType from, Word v) {
    uint64_t bits;
    if (from == T_F32) {
        float f = (float)v.f;
        uint32_t u;
        memcpy(&u, &f, 4);
        bits = u;
    } else if (from == T_F64) {
        memcpy(&bits, &v.f, 8);
    } else {
        bits = v.u & width_mask(from);
    }
    Word r;
    if (to == T_F32) {
        uint32_t u = (uint32_t)bits;
        float f;
        memcpy(&f, &u, 4);
        r.f = f;
    } else if (to == T_F64) {
        memcpy(&r.f, &bits, 8);
    } else {
        r.i = canon_int(to, (int64_t)bits);
    }
    return r;
}

static bool is_zero(MlwsType t, Word v) { return t == T_R ? v.r == NULL : v.u == 0; }

/* ---- object helpers --------------------------------------------------------- */

static Obj *rd_obj(Ctx *c, const Operand *o, int kind, const char *what) {
    Obj *ob = rd(c, o, T_R).r;
    if (!ob) vm_trap(c->p, "E_NULL", "%s of null", what);
    if (kind && ob->kind != kind)
        vm_trap(c->p, "E_TYPE", "%s needs a %s, got %s", what, obj_kind_name(kind), obj_desc(ob));
    return ob;
}

/* Bytes of a string or byte buffer operand. */
static const uint8_t *rd_text(Ctx *c, const Operand *o, const char *what, uint32_t *len) {
    Obj *ob = rd_obj(c, o, 0, what);
    if (ob->kind == OBJ_STRING) {
        *len = ((ObjString *)ob)->len;
        return (const uint8_t *)((ObjString *)ob)->data;
    }
    if (ob->kind == OBJ_BYTES) {
        *len = ((ObjBytes *)ob)->len;
        return ((ObjBytes *)ob)->data;
    }
    vm_trap(c->p, "E_TYPE", "%s needs a string or byte buffer, got %s (convert with tostr)", what, obj_desc(ob));
}

static void wr_ref(Ctx *c, const Operand *o, void *obj) {
    Word w;
    w.r = obj;
    wr(c, o, T_R, w);
}

static void wr_int(Ctx *c, const Operand *o, MlwsType t, int64_t v) {
    Word w;
    w.i = v;
    wr(c, o, t, w);
}

static int64_t parse_number(Ctx *c, MlwsType t, const uint8_t *s, uint32_t n, Word *out) {
    char buf[128];
    if (n == 0 || n >= sizeof buf) return -1;
    memcpy(buf, s, n);
    buf[n] = 0;
    if (isspace((unsigned char)buf[0])) return -1;
    char *end;
    if (type_is_float(t)) {
        double d = strtod(buf, &end);
        if (*end) return -1;
        out->f = canon_float(t, d);
        return 0;
    }
    if (t == T_B) {
        if (!strcmp(buf, "true") || !strcmp(buf, "1")) out->i = 1;
        else if (!strcmp(buf, "false") || !strcmp(buf, "0")) out->i = 0;
        else return -1;
        return 0;
    }
    const char *q = buf;
    bool neg = false;
    if (*q == '-' || *q == '+') neg = *q++ == '-';
    int base = 10;
    if (q[0] == '0' && (q[1] == 'x' || q[1] == 'X')) base = 16, q += 2;
    else if (q[0] == '0' && (q[1] == 'b' || q[1] == 'B')) base = 2, q += 2;
    else if (q[0] == '0' && (q[1] == 'o' || q[1] == 'O')) base = 8, q += 2;
    if (!*q || *q == '-' || *q == '+' || isspace((unsigned char)*q)) return -1;
    errno = 0;
    unsigned long long mag = strtoull(q, &end, base);
    if (*end || errno == ERANGE) return -1;
    int bits = type_bits(t);
    if (type_is_signed(t)) {
        uint64_t lim = (uint64_t)1 << (bits - 1); /* |min| */
        if (neg ? mag > lim : mag > lim - 1) return -1;
        out->i = neg ? (int64_t)(0 - (uint64_t)mag) : (int64_t)mag;
    } else {
        if (neg && mag != 0) return -1;
        if (bits < 64 && mag > (((uint64_t)1 << bits) - 1)) return -1;
        if (t == T_C && mag > 0x10FFFF) return -1;
        out->u = mag;
    }
    (void)c;
    return 0;
}

static void describe_key(Word k, MlwsType t, char *buf, size_t n) {
    if (t == T_R) {
        Obj *o = k.r;
        if (o && o->kind == OBJ_STRING) snprintf(buf, n, "\"%.60s\"", ((ObjString *)o)->data);
        else snprintf(buf, n, "<%s>", o ? obj_kind_name(o->kind) : "null");
    } else if (type_is_float(t)) {
        fmt_double(k.f, buf, n);
    } else {
        snprintf(buf, n, "%lld", (long long)k.i);
    }
}

/* ---- BigInt and decimal ------------------------------------------------------- */

/* A .big/.dec operand: a number object, or an integer (or, for .dec, float)
 * literal converted on the fly. */
static const Obj *rd_num(Ctx *c, const Operand *o, MlwsType t) {
    if (o->mode == OM_IMM) return num_from_i64(c->p, t, o->imm, false);
    if (o->mode == OM_FIMM) return num_from_f64(c->p, t, o->f);
    Obj *ob = rd(c, o, T_R).r;
    if (!ob) vm_trap(c->p, "E_NULL", "%s operand is null", type_name(t));
    if (!num_kind_matches(ob, t))
        vm_trap(c->p, "E_TYPE", "expected a %s, got a %s (convert with cvt)", t == T_BIG ? "bigint" : "decimal",
                obj_kind_name(ob->kind));
    return ob;
}

static void wr_num(Ctx *c, const Loc *d, const Obj *o) {
    Word w;
    w.r = (void *)o;
    wr_loc(c, d, T_R, w);
}

static void arith_num(Ctx *c, const Insn *in, MlwsType t) {
    const Operand *ops = in->ops;
    Loc d;
    loc_of(c, &ops[0], 8, &d);
    const Obj *acc;
    int k;
    if (in->nops == 2) {
        MlwsType rt = T_R;
        Obj *cur = rd_loc(c, &d, &rt).r;
        if (!num_kind_matches(cur, t)) vm_trap(c->p, cur ? "E_TYPE" : "E_NULL", "destination does not hold a %s", type_name(t));
        acc = cur;
        k = 1;
    } else {
        acc = rd_num(c, &ops[1], t);
        k = 2;
    }
    for (; k < in->nops; k++) acc = num_arith(c->p, in->op, acc, rd_num(c, &ops[k], t));
    wr_num(c, &d, acc);
}

/* cvt where either side is .big or .dec */
static void convert_num(Ctx *c, const Insn *in) {
    MlwsType to = (MlwsType)in->type, from = (MlwsType)in->type2;
    Loc d;
    loc_of(c, &in->ops[0], 8, &d);
    const Operand *src = &in->ops[1];
    if (to == T_BIG || to == T_DEC) {
        const Obj *r;
        if (from == T_BIG || from == T_DEC) r = num_convert(c->p, to, rd_num(c, src, from));
        else if (type_is_float(from)) r = num_from_f64(c->p, to, rd(c, src, from).f);
        else r = num_from_i64(c->p, to, rd(c, src, from).i, !type_is_signed(from) && from != T_B && from != T_C ? true : false);
        wr_num(c, &d, r);
        return;
    }
    const Obj *n = rd_num(c, src, from);
    Word w;
    if (type_is_float(to)) {
        w.f = canon_float(to, num_to_f64(n));
    } else if (!num_to_int(n, to, &w)) {
        StrBuf b = {0};
        num_format(&b, n);
        char shown[64]; /* copied out: the trap does not return to free the buffer */
        snprintf(shown, sizeof shown, "%.60s", b.s);
        sb_free(&b);
        vm_trap(c->p, "E_OVERFLOW", "%s does not fit in %s", shown, type_name(to));
    }
    wr_loc(c, &d, to, w);
}

/* Read a closure-call argument with the parameter's type, checking the
 * register bank at run time (the callee is not known statically). */
static Word rd_param(Ctx *c, const Operand *o, MlwsType t) {
    char b[8];
    if (o->mode == OM_REG) {
        int bank = REG_BANK(o->reg);
        bool ok = bank == BANK_S ? (REG_IDX(o->reg) == SREG_PID ? type_is_int(t) : t == T_P) : (Bank)bank == type_bank(t);
        if (!ok) vm_trap(c->p, "E_TYPE", "closure argument %s cannot be passed as %s", reg_name(o->reg, b), type_name(t));
    } else if (o->mode == OM_FIMM && !type_is_float(t) && t != T_DEC) {
        vm_trap(c->p, "E_TYPE", "a float literal cannot be passed as %s", type_name(t));
    } else if (o->mode == OM_CONST && !type_is_ref(t)) {
        vm_trap(c->p, "E_TYPE", "a constant cannot be passed as %s", type_name(t));
    } else if (o->mode == OM_IMM && (t == T_R || t == T_P) && o->imm) {
        vm_trap(c->p, "E_TYPE", "an integer cannot be passed as %s", type_name(t));
    }
    return rd(c, o, t);
}

static ObjClosure *rd_closure(Ctx *c, const Operand *o) {
    Obj *ob = rd(c, o, T_R).r;
    if (!ob) vm_trap(c->p, "E_NULL", "call through a null closure");
    if (ob->kind != OBJ_CLOSURE) vm_trap(c->p, "E_TYPE", "calli needs a closure, got a %s", obj_kind_name(ob->kind));
    return (ObjClosure *)ob;
}

/* ---- calls -------------------------------------------------------------------- */

static void bind_args(Ctx *c, const MlwsType *types, const uint8_t *ptypes, int n, const Word *args) {
    int cnt[NBANKS] = {0};
    for (int k = 0; k < n; k++) {
        MlwsType t = types ? types[k] : (MlwsType)ptypes[k];
        Bank b = type_bank(t);
        int idx = cnt[b]++;
        switch (b) {
        case BANK_I: c->I[idx] = args[k].i; break;
        case BANK_F: c->F[idx] = args[k].f; break;
        case BANK_R: c->R[idx] = args[k].r; break;
        default: c->P[idx] = args[k].p; break;
        }
    }
}

/* ---- coroutines (coro.c) ------------------------------------------------------------ */

/* run_loop's code for an instruction it leaves to interp_run (never a scheduler's). */
#define SLICE_EXT 100
/* exec_generic: go on with the next instruction (tail-call dispatch) */
#define GEN_NEXT (-1)

static const char *co_state(const ObjCoro *co) {
    static const char *const st[] = {"suspended", "running", "normal", "dead"};
    return st[co->status];
}

/* Write values to the results of a coresume or coyield, as ret does to a call's. */
static void co_deliver(Ctx *c, const Insn *ri, const Word *vals, const MlwsType *vt, int n, const char *what) {
    int argc = (int)ri->ops[1].imm, nres = ri->nops - 2 - argc;
    if (nres > n)
        vm_trap(c->p, "E_RET_COUNT", "%s %d value(s) but %s expects %d", what, n, op_info[ri->op].mnem, nres);
    for (int k = 0; k < nres; k++) wr(c, &ri->ops[2 + argc + k], vt[k], vals[k]);
}

/* The running coroutine's function returned (its results are in p->results,
 * as an entry function's are): it is dead, and the coresume that ran it
 * receives them. */
static MLWS_COLD void co_finish(Ctx *c) {
    Process *p = c->p;
    ObjCoro *co = coro_leave(p, CO_DEAD);
    MlwsType vt[MAX_OPERANDS];
    for (int k = 0; k < p->nresults; k++) vt[k] = (MlwsType)p->result_types[k];
    ctx_load(c);
    co_deliver(c, co->wait, p->results, vt, p->nresults, "the coroutine returned");
}

static MLWS_COLD void co_resume(Ctx *c, const Insn *in) {
    Process *p = c->p;
    const Operand *ops = in->ops;
    ObjCoro *co = (ObjCoro *)rd_obj(c, &ops[0], OBJ_CORO, "coresume");
    if (p->cb_base) vm_trap(p, "E_CALLBACK", "coresume inside a callback from C");
    if (co->status != CO_SUSPENDED) vm_trap(p, "E_CORO", "cannot resume a %s coroutine", co_state(co));
    const MFunc *mf = co->fn->fn->mf;
    const Insn *yi = co->wait; /* where it is suspended; NULL: not started */
    int argc = (int)ops[1].imm;
    Word args[MAX_OPERANDS];
    MlwsType at[MAX_OPERANDS];
    if (!yi && argc != mf->nparams)
        vm_trap(p, "E_TYPE", "closure %s takes %d argument(s), got %d", co->fn->fn->name, mf->nparams, argc);
    for (int k = 0; k < argc; k++) {
        at[k] = yi ? T_NONE : (MlwsType)mf->params[k];
        args[k] = yi ? rdn(c, &ops[2 + k], &at[k]) : rd_param(c, &ops[2 + k], at[k]);
    }
    if (p->co) p->co->status = CO_NORMAL;
    co->prev = p->co;
    co->status = CO_RUNNING;
    co->wait = in;
    p->co = co;
    coro_swap(p, co);
    if (!yi) { /* its first frame, on its own stacks */
        proc_push_frame(p, co->fn->fn, NULL);
        p->frames[0].closure = &co->fn->h;
        ctx_load(c);
        bind_args(c, NULL, mf->params, argc, args);
    } else {
        ctx_load(c);
        co_deliver(c, yi, args, at, argc, "coresume passed");
    }
}

static MLWS_COLD void co_yield(Ctx *c, const Insn *in) {
    Process *p = c->p;
    ObjCoro *co = p->co;
    if (!co) vm_trap(p, "E_CORO", "coyield outside a coroutine");
    if (p->cb_base) vm_trap(p, "E_CALLBACK", "coyield inside a callback from C");
    int n = (int)in->ops[1].imm;
    Word vals[MAX_OPERANDS];
    MlwsType vt[MAX_OPERANDS];
    for (int k = 0; k < n; k++) {
        vt[k] = T_NONE;
        vals[k] = rdn(c, &in->ops[2 + k], &vt[k]);
    }
    const Insn *ri = co->wait;
    co->wait = in;
    coro_leave(p, CO_SUSPENDED);
    ctx_load(c);
    co_deliver(c, ri, vals, vt, n, "the coroutine yielded");
}

/* The instructions that run_loop leaves to interp_run: the coroutines'. */
static MLWS_COLD void co_exec(Ctx *c, const Insn *in) {
    Process *p = c->p;
    const Operand *ops = in->ops;
    switch (in->op) {
    case OP_CONEW: {
        ObjClosure *cl = (ObjClosure *)rd_obj(c, &ops[1], OBJ_CLOSURE, "conew");
        int64_t words = in->nops > 2 ? rd(c, &ops[2], T_I64).i : 0;
        wr_ref(c, &ops[0], coro_new(p, cl, words));
        break;
    }
    case OP_CORESUME: co_resume(c, in); break;
    case OP_COYIELD: co_yield(c, in); break;
    case OP_COSTATUS:
        wr_int(c, &ops[0], T_I64, ((ObjCoro *)rd_obj(c, &ops[1], OBJ_CORO, "costatus"))->status);
        break;
    case OP_COCUR: wr_ref(c, &ops[0], p->co); break;
    case OP_COCLOSE: {
        ObjCoro *co = (ObjCoro *)rd_obj(c, &ops[0], OBJ_CORO, "coclose");
        if (co->status == CO_RUNNING || co->status == CO_NORMAL)
            vm_trap(p, "E_CORO", "cannot close a %s coroutine", co_state(co));
        if (co->status == CO_SUSPENDED) {
            co->status = CO_DEAD;
            coro_free_ctx(p, co);
        }
        break;
    }
    default: vm_trap(p, "E_INTERNAL", "unimplemented opcode %s", op_info[in->op].mnem);
    }
}

/* ---- quickening ------------------------------------------------------------------
 * After verification, common instruction shapes get a specialised execution
 * opcode (Insn.xop) that skips the generic operand machinery. The original
 * opcode stays in Insn.op for the disassembler, tracer, profiler and debugger.
 * Only shapes whose generic semantics are trivially the same qualify: 64-bit
 * integers in I registers (no width canonicalisation needed), f64 in F registers. */

#define QARITH_OPS(X) X(ADD, +) X(SUB, -) X(MUL, *) X(AND, &) X(OR, |) X(XOR, ^)
#define QFARITH_OPS(X) X(ADD, +) X(SUB, -) X(MUL, *) X(DIV, /)
#define QBRANCH_OPS(X) X(BEQ, ==) X(BNE, !=) X(BLT, <) X(BLE, <=) X(BGT, >) X(BGE, >=)
#define QSET_OPS(X) X(SEQ, ==) X(SNE, !=) X(SLT, <) X(SLE, <=) X(SGT, >) X(SGE, >=)
#define QFUNARY_OPS(X) X(SQRT, sqrt) X(NEG, -) X(ABS, fabs)
/* unsigned 64-bit orderings (equality is the signed forms') */
#define QUBRANCH_OPS(X) X(BLT, <) X(BLE, <=) X(BGT, >) X(BGE, >=)
/* 64-bit shifts, by a count taken modulo 64 (shiftop) */
#define QSHIFT_OPS(X) X(SHL, (uint64_t)a << (n & 63)) X(SHR, (uint64_t)a >> (n & 63)) X(SAR, a >> (n & 63))

enum {
    Q__FIRST = OP__COUNT,
    Q_MOV_II, Q_MOV_IK, Q_MOV_FF, Q_MOV_FK, Q_MOV_RR, Q_MOV_PP,
#define X(N, o) Q_##N##_III, Q_##N##_IIK, Q_##N##_II, Q_##N##_IK,
    QARITH_OPS(X)
#undef X
#define X(N, o) Q_F##N##_FFF, Q_F##N##_FFK, Q_F##N##_FF, Q_F##N##_FK, Q_F##N##_FKF,
    QFARITH_OPS(X)
#undef X
#define X(N, o) Q_##N##_II, Q_##N##_IK,
    QBRANCH_OPS(X)
#undef X
    Q_REM_IIK, Q_REM_IK, Q_DIV_IIK, Q_DIV_IK,
    Q_INC_I, Q_DEC_I, Q_BZ_I, Q_BNZ_I, Q_JMP,
    Q_CALL_R, /* call whose arguments and results are registers of the right banks */
    Q_RET_R,  /* ret of registers matching the declared 64-bit result types */
    Q_BZ_R, Q_BNZ_R, /* null tests */
    /* mov between a register and a struct field (LDF: load, STF: store) or an
     * array or list element indexed by a register (LDE, STE), for the 64-bit
     * types of the I, F and R banks. They check the object at run time and leave
     * anything else (another kind, type or struct, a bad index) to the generic
     * mov, which raises the error. */
    Q_LDF_I, Q_LDF_F, Q_LDF_R, Q_STF_I, Q_STF_F, Q_STF_R,
    Q_LDE_I, Q_LDE_F, Q_LDE_R, Q_STE_I, Q_STE_F, Q_STE_R,
    Q_CALLI_R, /* closure call with register arguments and results (banks checked per call) */
    /* Shapes compilers of dynamically typed languages emit (M24): constants
     * among a call's arguments or a ret's values (CALL_RK, CALLI_RK, RET_RK),
     * a reference constant or global into an R register (MOV_RK, LDG_R),
     * reference branches against a register, constant or global (RR, RK, RG),
     * f64 and reference set-compares (FF, FK, RR, RK), isa, and mget of a
     * constant key with a null default. */
    Q_CALL_RK, Q_CALLI_RK, Q_RET_RK,
    Q_MOV_RK,
    Q_BEQ_RR, Q_BNE_RR, Q_BEQ_RK, Q_BNE_RK, Q_BEQ_RG, Q_BNE_RG,
#define X(N, o) Q_##N##_FF, Q_##N##_FK,
    QSET_OPS(X)
#undef X
#define X(N, o) Q_##N##_FFL, Q_##N##_FKL, /* f64 compare-and-branch */
    QBRANCH_OPS(X)
#undef X
    Q_SEQ_RR, Q_SNE_RR, Q_SEQ_RK, Q_SNE_RK,
    Q_ISA,
    Q_MGET_R,
    /* M25: 64-bit globals, elements at a constant index (R1[5]) and closure
     * captures (UP[n]) moved to or from a register of each bank; len; tagof of
     * an element indexed by a register. */
    Q_LDG_I, Q_LDG_F, Q_LDG_R, Q_STG_I, Q_STG_F, Q_STG_R,
    Q_LDEK_I, Q_LDEK_F, Q_LDEK_R, Q_STEK_I, Q_STEK_F, Q_STEK_R,
    Q_LDU_I, Q_LDU_F, Q_LDU_R, Q_STU_I, Q_STU_F, Q_STU_R,
    Q_LEN, Q_TAGOF_E,
    /* M26: lpush of register values into a list's spare capacity, and f64
     * sqrt, neg and abs, in place or from another register. */
    Q_LPUSH_R,
#define X(N, f) Q_##N##_F, Q_##N##_FF,
    QFUNARY_OPS(X)
#undef X
    /* M29: shapes from compiled Lua's profile: a closure tail call with
     * register arguments; new of a struct and lnew of a constant capacity; a
     * constant stored to a struct field; mget of a register key into each
     * bank, with a constant default; jtab; cvt.f64.i64; 64-bit shifts; and
     * f64 arithmetic with a constant first operand (FKF, above). */
    Q_TCALLI_R,
    /* M30: ldv from a list or array element indexed by a register or a
     * constant, into an I and an F register. */
    Q_LDV_E, Q_LDV_EK,
    Q_NEW, Q_LNEW, Q_STFK,
    Q_MGETV_I, Q_MGETV_F, Q_MGETV_R,
    Q_JTAB, Q_CVT_FI,
    /* M32: elements at a register index plus a constant (R1[I2+1]): moves in
     * each bank and ldv; the offset in k (r[2] for ldv). Forms of their own,
     * so that the plain index's pay nothing for it. */
    Q_LDED_I, Q_LDED_F, Q_LDED_R, Q_STED_I, Q_STED_F, Q_STED_R, Q_LDV_ED,
    Q_BISA, Q_BNISA,
    /* M33: lpush with constants among the values (a constructor's keys and
     * values): each constant's word in its kw and its type in its pad. */
    Q_LPUSH_RK,
#define X(N, o) Q_##N##_UU, Q_##N##_UK, /* u64 compare-and-branch */
    QUBRANCH_OPS(X)
#undef X
#define X(N, e) Q_##N##_III, Q_##N##_IIK, Q_##N##_II, Q_##N##_IK,
    QSHIFT_OPS(X)
#undef X
    Q__COUNT
};

static bool reg_of_type(const Operand *o, int t) {
    return o->mode == OM_REG && REG_BANK(o->reg) < NBANKS && REG_BANK(o->reg) == (int)type_bank((MlwsType)t) &&
           wide_type(t);
}

/* A constant call argument or ret value as a value of type t, if it is one the
 * quickened forms pass as is (the generic path converts or rejects the rest,
 * with the same results). */
static inline bool const_word(const Operand *o, int t, Word *w) {
    switch (o->mode) {
    case OM_IMM:
        if (t == T_I64 || t == T_U64) w->i = o->imm;
        else if (t == T_F64) w->f = (double)o->imm;
        else if (t == T_R && o->imm == 0) w->r = NULL;
        else return false;
        return true;
    case OM_FIMM:
        if (t != T_F64) return false;
        w->f = o->f;
        return true;
    case OM_CONST:
        if (t != T_R) return false;
        w->r = o->rv.ptr;
        return true;
    default: return false;
    }
}

/* A register of t's bank, or a constant of type t. */
static bool reg_or_const(const Operand *o, int t) {
    Word w;
    return reg_of_type(o, t) || (wide_type(t) && const_word(o, t, &w));
}

/* A global word operand (`@name`): its module's segment and byte offset, packed
 * into k for Q_LDG_R and Q_B*_RG, which read the process's own copy of it. */
static bool quicken_global(Insn *in, const Operand *o) {
    if (o->mode != OM_GLOBAL && o->mode != OM_GLOBALB) return false;
    uint64_t seg = PTR_SEG(o->rv.gbase);
    int64_t off = (int64_t)PTR_OFF(o->rv.gbase) + o->bdisp;
    if (seg < SEG_GLOBAL0 || seg >= SEG_RAW0 || off < 0 || off % 8 || off > (int64_t)UINT32_MAX - 8) return false;
    in->k.i = (int64_t)((seg - SEG_GLOBAL0) << 32 | (uint64_t)off);
    return true;
}

/* A constant argument or result of type t, converted once into the word it
 * passes (Operand.kw). */
static void quicken_const_word(Operand *o, int t) {
    Word w = {0};
    const_word(o, t, &w);
    memcpy(&o->kw, &w, 8);
}

/* Constant arguments too: Q_CALL_RK. */
static bool quicken_call_rk(const Insn *in) {
    const Function *fn = in->ops[0].rv.ptr;
    const MFunc *mf = fn->mf;
    int argc = (int)in->ops[1].imm, nres = in->nops - 2 - argc;
    if (nres && !(mf->flags & MF_RETS_DECLARED)) return false;
    for (int k = 0; k < argc; k++)
        if (!reg_or_const(&in->ops[2 + k], mf->params[k])) return false;
    for (int k = 0; k < nres; k++)
        if (!reg_of_type(&in->ops[2 + argc + k], mf->rets[k])) return false;
    return true;
}

static bool quicken_call(const Insn *in) {
    const Function *fn = in->ops[0].rv.ptr;
    const MFunc *mf = fn->mf;
    int argc = (int)in->ops[1].imm, nres = in->nops - 2 - argc;
    if (nres && !(mf->flags & MF_RETS_DECLARED)) return false;
    for (int k = 0; k < argc; k++)
        if (!reg_of_type(&in->ops[2 + k], mf->params[k])) return false;
    for (int k = 0; k < nres; k++)
        if (!reg_of_type(&in->ops[2 + argc + k], mf->rets[k])) return false;
    return true;
}

static const MFunc *quicken_fn; /* function being quickened (for ret) */
static uint32_t quicken_idx;      /* the instruction's index in it */

typedef struct {
    Function *fn; /* a direct call's callee */
    uint8_t argc, nres;
    uint8_t slot[];
} QPlan;
#define QPLAN_K 0xFF

/* A quickened call's or ret's operands as its handler reads them: bytes, in
 * one cache line, instead of a 72-byte Operand each. It lives just after the
 * instruction's operands, in the same allocation (so insn_free frees it), and
 * Insn.k points to it. A call's slots are each argument's slot in the caller
 * (QPLAN_K: a constant, whose word is the operand's kw), then, for a direct
 * call, each parameter's slot in the callee, then each result's slot in the
 * caller; a ret's are its values' slots (or QPLAN_K). */
static void quicken_plan(Insn *in) {
    bool call = in->xop == Q_CALL_R || in->xop == Q_CALL_RK || in->xop == Q_CALLI_R || in->xop == Q_CALLI_RK;
    bool ret = in->xop == Q_RET_R || in->xop == Q_RET_RK;
    if (!call && !ret) return;
    int n = in->nops, argc = call ? (int)in->ops[1].imm : 0, nres = call ? n - 2 - argc : n;
    size_t at = (size_t)(n ? n : 1) * sizeof(Operand);
    in->ops = xrealloc(in->ops, at + sizeof(QPlan) + (size_t)(2 * argc + nres));
    const Operand *o = in->ops;
    QPlan *pl = (QPlan *)((char *)in->ops + at);
    bool direct = in->xop == Q_CALL_R || in->xop == Q_CALL_RK;
    pl->fn = direct ? o[0].rv.ptr : NULL;
    pl->argc = (uint8_t)argc;
    pl->nres = (uint8_t)nres;
    const Operand *v = call ? o + 2 : o;
    for (int k = 0; k < argc; k++) {
        pl->slot[k] = v[k].mode == OM_REG ? v[k].pad : QPLAN_K;
        pl->slot[argc + k] = direct ? v[k].ireg : 0;
    }
    for (int k = 0; k < nres; k++) pl->slot[2 * argc + k] = v[argc + k].mode == OM_REG ? v[argc + k].pad : QPLAN_K;
    in->k.p = pl;
}

void interp_quicken_function(Function *f) {
    quicken_fn = f->mf;
    /* Every register operand's slot in f's register window (Function.roff), in
     * its spare `pad` byte: the calls and rets copy words between slots. */
    for (uint32_t k = 0; k < f->ncode; k++)
        for (int j = 0; j < f->code[k].nops; j++) {
            Operand *o = &f->code[k].ops[j];
            if (o->mode == OM_REG && REG_BANK(o->reg) < NBANKS)
                o->pad = (uint8_t)(f->roff[REG_BANK(o->reg)] + REG_IDX(o->reg));
        }
    for (uint32_t k = 0; k < f->ncode; k++) {
        quicken_idx = k;
        interp_quicken(&f->code[k]);
        quicken_plan(&f->code[k]);
    }
    quicken_fn = NULL;
}

/* An unsigned ordering's register-register form (the constant one is next). */
static int quicken_ubranch(int op) {
    switch (op) {
#define X(N, o)                                                                \
    case OP_##N: return Q_##N##_UU;
        QUBRANCH_OPS(X)
#undef X
    default: return Q_BLT_UU; /* not reached: equality is quickened as i64 */
    }
}

/* mov of a constant to a 64-bit struct field: Q_STFK, with the constant as the
 * word it stores in Operand.kw. */
static void quicken_field_const(Insn *in, MlwsType t) {
    Operand *o = in->ops;
    Word w = {0};
    if (o[0].mode != OM_FIELD || o[1].mode == OM_REG || !const_word(&o[1], t, &w)) return;
    const FieldRef *f = o[0].rv.ptr;
    if (f->def->types[f->index] != t) return;
    in->xop = Q_STFK;
    in->k.p = f->def;
    in->target = f->index;
    in->r[0] = (uint8_t)REG_IDX(o[0].reg);
    memcpy(&o[1].kw, &w, 8);
}

/* mov of a 64-bit I, F or R value between a register and a struct field or a
 * register-indexed element: see Q_LDF_I. */
static void quicken_slot(Insn *in, MlwsType t) {
    const Operand *o = in->ops;
    int q = type_bank(t) == BANK_I ? 0 : type_bank(t) == BANK_F ? 1 : 2;
    bool load = o[0].mode == OM_REG && REG_BANK(o[0].reg) == (int)type_bank(t);
    bool store = o[1].mode == OM_REG && REG_BANK(o[1].reg) == (int)type_bank(t);
    const Operand *slot = load ? &o[1] : store ? &o[0] : NULL;
    if (!slot) return;
    if (slot->mode == OM_FIELD) {
        const FieldRef *f = slot->rv.ptr;
        if (f->def->types[f->index] != t) return;
        in->k.p = f->def;
        in->target = f->index;
        in->xop = (uint16_t)((load ? Q_LDF_I : Q_STF_I) + q);
    } else if (slot->mode == OM_ELEM) {
        in->r[2] = (uint8_t)REG_IDX(slot->ireg);
        if (slot->imm) { /* elements past the index */
            in->k.i = slot->imm;
            in->xop = (uint16_t)((load ? Q_LDED_I : Q_STED_I) + q);
        } else {
            in->xop = (uint16_t)((load ? Q_LDE_I : Q_STE_I) + q);
        }
    } else if (slot->mode == OM_ELEMI && slot->index <= UINT32_MAX) {
        in->target = (uint32_t)slot->index;
        in->xop = (uint16_t)((load ? Q_LDEK_I : Q_STEK_I) + q);
    } else if (slot->mode == OM_UPVAL && slot->index <= UINT32_MAX) {
        in->target = (uint32_t)slot->index;
        in->xop = (uint16_t)((load ? Q_LDU_I : Q_STU_I) + q);
        return; /* no object register */
    } else {
        return;
    }
    /* the object's register: r[1] for a load (r[0] is the destination), r[0]
     * for a store (r[1] is the source) */
    in->r[load ? 1 : 0] = (uint8_t)REG_IDX(slot->reg);
}

void interp_quicken(Insn *in) {
    Operand *o = in->ops;
    int n = in->nops;
    MlwsType t = (MlwsType)in->type;
#define IREG(k) (o[k].mode == OM_REG && REG_BANK(o[k].reg) == BANK_I)
#define FREG(k) (o[k].mode == OM_REG && REG_BANK(o[k].reg) == BANK_F)
#define RREG(k) (o[k].mode == OM_REG && REG_BANK(o[k].reg) == BANK_R)
#define PREG(k) (o[k].mode == OM_REG && REG_BANK(o[k].reg) == BANK_P)
#define KINT(k) (o[k].mode == OM_IMM)
#define KFLT(k) (o[k].mode == OM_FIMM || o[k].mode == OM_IMM)
#define KREF(k) (o[k].mode == OM_CONST || (o[k].mode == OM_IMM && o[k].imm == 0)) /* a constant, or null */
    bool i64 = t == T_I64 || t == T_U64;
    /* float constants are converted once, here */
    for (int k = 0; k < n; k++)
        if (t == T_F64 && o[k].mode == OM_IMM) o[k].f = (double)o[k].imm;
    in->xop = in->op;
    switch (in->op) {
    case OP_MOV:
        if (n != 2) break;
        if (i64 && IREG(0) && IREG(1)) in->xop = Q_MOV_II;
        else if (i64 && IREG(0) && KINT(1)) in->xop = Q_MOV_IK;
        else if (t == T_F64 && FREG(0) && FREG(1)) in->xop = Q_MOV_FF;
        else if (t == T_F64 && FREG(0) && KFLT(1)) in->xop = Q_MOV_FK;
        else if (t == T_R && RREG(0) && RREG(1)) in->xop = Q_MOV_RR;
        else if (t == T_P && PREG(0) && PREG(1)) in->xop = Q_MOV_PP;
        else if (t == T_R && RREG(0) && KREF(1)) {
            in->xop = Q_MOV_RK;
            in->k.p = o[1].mode == OM_CONST ? o[1].rv.ptr : NULL;
        } else if (t != T_P && wide_type(t) && o[0].mode == OM_REG &&
                   REG_BANK(o[0].reg) == (int)type_bank(t) && quicken_global(in, &o[1])) {
            in->xop = (uint16_t)(Q_LDG_I + (t == T_F64 ? 1 : t == T_R ? 2 : 0));
        } else if (t != T_P && wide_type(t) && o[1].mode == OM_REG &&
                   REG_BANK(o[1].reg) == (int)type_bank(t) && quicken_global(in, &o[0])) {
            in->xop = (uint16_t)(Q_STG_I + (t == T_F64 ? 1 : t == T_R ? 2 : 0));
        } else if (t != T_P && wide_type(t)) {
            quicken_slot(in, t);
        }
        break;
#define X(N, op_)                                                              \
    case OP_##N:                                                               \
        if (i64 && n == 3 && IREG(0) && IREG(1) && IREG(2)) in->xop = Q_##N##_III; \
        else if (i64 && n == 3 && IREG(0) && IREG(1) && KINT(2)) in->xop = Q_##N##_IIK; \
        else if (i64 && n == 2 && IREG(0) && IREG(1)) in->xop = Q_##N##_II;   \
        else if (i64 && n == 2 && IREG(0) && KINT(1)) in->xop = Q_##N##_IK;   \
        else goto float_arith;                                                 \
        break;
        QARITH_OPS(X)
#undef X
    case OP_DIV:
        if (t == T_I64 && n == 3 && IREG(0) && IREG(1) && KINT(2) && o[2].imm != 0 && o[2].imm != -1) in->xop = Q_DIV_IIK;
        else if (t == T_I64 && n == 2 && IREG(0) && KINT(1) && o[1].imm != 0 && o[1].imm != -1) in->xop = Q_DIV_IK;
        else goto float_arith;
        break;
    case OP_REM:
        if (t == T_I64 && n == 3 && IREG(0) && IREG(1) && KINT(2) && o[2].imm != 0 && o[2].imm != -1) in->xop = Q_REM_IIK;
        else if (t == T_I64 && n == 2 && IREG(0) && KINT(1) && o[1].imm != 0 && o[1].imm != -1) in->xop = Q_REM_IK;
        break;
#define X(N, e)                                                                \
    case OP_##N:                                                               \
        if (i64 && n == 3 && IREG(0) && IREG(1) && IREG(2)) in->xop = Q_##N##_III; \
        else if (i64 && n == 3 && IREG(0) && IREG(1) && KINT(2)) in->xop = Q_##N##_IIK; \
        else if (i64 && n == 2 && IREG(0) && IREG(1)) in->xop = Q_##N##_II;   \
        else if (i64 && n == 2 && IREG(0) && KINT(1)) in->xop = Q_##N##_IK;   \
        break;
        QSHIFT_OPS(X)
#undef X
    float_arith:
        if (t != T_F64) break;
        switch (in->op) {
#define X(N, op_)                                                              \
    case OP_##N:                                                               \
        if (n == 3 && FREG(0) && FREG(1) && FREG(2)) in->xop = Q_F##N##_FFF;   \
        else if (n == 3 && FREG(0) && FREG(1) && KFLT(2)) in->xop = Q_F##N##_FFK; \
        else if (n == 2 && FREG(0) && FREG(1)) in->xop = Q_F##N##_FF;         \
        else if (n == 2 && FREG(0) && KFLT(1)) in->xop = Q_F##N##_FK;         \
        else if (n == 3 && FREG(0) && KFLT(1) && FREG(2)) in->xop = Q_F##N##_FKF; \
        break;
            QFARITH_OPS(X)
#undef X
        default: break;
        }
        break;
    case OP_INC:
        if (i64 && IREG(0)) in->xop = Q_INC_I;
        break;
    case OP_DEC:
        if (i64 && IREG(0)) in->xop = Q_DEC_I;
        break;
    case OP_BZ:
        if (i64 && IREG(0)) in->xop = Q_BZ_I;
        else if (t == T_R && RREG(0)) in->xop = Q_BZ_R;
        break;
    case OP_BNZ:
        if (i64 && IREG(0)) in->xop = Q_BNZ_I;
        else if (t == T_R && RREG(0)) in->xop = Q_BNZ_R;
        break;
    case OP_CALLI: {
        bool regs = RREG(0), rk = RREG(0);
        for (int k = 2; k < n; k++) {
            bool reg = o[k].mode == OM_REG && REG_BANK(o[k].reg) < NBANKS;
            regs = regs && reg;
            /* constants as arguments (checked against the callee per call); results in registers */
            rk = rk && (reg || (k < 2 + (int)o[1].imm && (o[k].mode == OM_IMM || o[k].mode == OM_FIMM ||
                                                          o[k].mode == OM_CONST)));
        }
        if (regs) in->xop = Q_CALLI_R;
        else if (rk) in->xop = Q_CALLI_RK;
        break;
    }
    case OP_TCALLI: {
        bool regs = RREG(0);
        for (int k = 2; k < n; k++) regs = regs && o[k].mode == OM_REG && REG_BANK(o[k].reg) < NBANKS;
        if (regs) in->xop = Q_TCALLI_R;
        break;
    }
    case OP_NEW:
        if (n == 2 && RREG(0)) {
            in->xop = Q_NEW;
            in->k.p = o[1].rv.ptr;
        }
        break;
    case OP_LNEW: /* the capacity's range is list_new's to check */
        if (RREG(0) && (n == 1 || (n == 2 && KINT(1)))) {
            in->xop = Q_LNEW;
            in->k.i = n == 2 ? o[1].imm : 0;
        }
        break;
    case OP_JTAB:
        if (IREG(0)) in->xop = Q_JTAB;
        break;
    case OP_LDV: /* ldv I0, F0, R1[I2], R1[I2+d] or R1[5]: the object's register in k, the index's in target, d in r[2] */
        if (IREG(0) && FREG(1) && REG_BANK(o[2].reg) == BANK_R) {
            if (o[2].mode == OM_ELEM && REG_BANK(o[2].ireg) == BANK_I && o[2].imm >= INT8_MIN && o[2].imm <= INT8_MAX) {
                in->xop = o[2].imm ? Q_LDV_ED : Q_LDV_E;
                in->target = (uint32_t)REG_IDX(o[2].ireg);
                in->r[2] = (uint8_t)(int8_t)o[2].imm;
            } else if (o[2].mode == OM_ELEMI && o[2].index <= UINT32_MAX) {
                in->xop = Q_LDV_EK;
                in->target = (uint32_t)o[2].index;
            } else {
                break;
            }
            in->k.i = REG_IDX(o[2].reg);
        }
        break;
    case OP_CVT:
        if (n == 2 && t == T_F64 && in->type2 == T_I64 && FREG(0) && IREG(1)) in->xop = Q_CVT_FI;
        break;
    case OP_JMP: in->xop = Q_JMP; break;
    case OP_CALL:
        if (quicken_call(in)) {
            in->xop = Q_CALL_R;
            /* Each argument register's `ireg` (unused by a register operand) holds
             * its destination: the parameter's slot in the callee's window. */
            const Function *callee = o[0].rv.ptr;
            const MFunc *mf = callee->mf;
            int cnt[NBANKS] = {0};
            for (int k = 0; k < (int)o[1].imm; k++) {
                int b = (int)type_bank((MlwsType)mf->params[k]);
                o[2 + k].ireg = (uint8_t)(callee->roff[b] + cnt[b]++);
            }
        } else if (quicken_call_rk(in)) {
            in->xop = Q_CALL_RK;
            /* The same slots, and each constant as the word it passes. */
            const Function *callee = o[0].rv.ptr;
            const MFunc *mf = callee->mf;
            int cnt[NBANKS] = {0};
            for (int k = 0; k < (int)o[1].imm; k++) {
                int b = (int)type_bank((MlwsType)mf->params[k]);
                o[2 + k].ireg = (uint8_t)(callee->roff[b] + cnt[b]++);
                if (o[2 + k].mode != OM_REG) quicken_const_word(&o[2 + k], mf->params[k]);
            }
        }
        break;
    case OP_RET:
        if (quicken_fn && (quicken_fn->flags & MF_RETS_DECLARED) && n == quicken_fn->nrets) {
            bool ok = true;
            for (int k = 0; k < n; k++) ok = ok && reg_of_type(&o[k], quicken_fn->rets[k]);
            bool rk = true;
            for (int k = 0; k < n; k++) rk = rk && reg_or_const(&o[k], quicken_fn->rets[k]);
            if (ok) {
                in->xop = Q_RET_R;
            } else if (rk) {
                in->xop = Q_RET_RK;
                for (int k = 0; k < n; k++)
                    if (o[k].mode != OM_REG) quicken_const_word(&o[k], quicken_fn->rets[k]);
            }
        } else if (quicken_fn && n == 0) {
            in->xop = Q_RET_R;
        }
        break;
#define X(N, op_)                                                              \
    case OP_##N:                                                               \
        /* signed order needs i64; equality also works for u64 */             \
        if (t == T_I64 || ((in->op == OP_BEQ || in->op == OP_BNE) && t == T_U64)) { \
            if (IREG(0) && IREG(1)) in->xop = Q_##N##_II;                       \
            else if (IREG(0) && KINT(1)) in->xop = Q_##N##_IK;                  \
        } else if (t == T_U64 && IREG(0) && (IREG(1) || KINT(1))) {           \
            in->xop = (uint16_t)(quicken_ubranch(in->op) + !IREG(1));         \
        } else if (t == T_F64 && FREG(0)) {                                   \
            if (FREG(1)) in->xop = Q_##N##_FFL;                                 \
            else if (KFLT(1)) in->xop = Q_##N##_FKL;                            \
        } else if (t == T_R && (in->op == OP_BEQ || in->op == OP_BNE) && RREG(0)) { \
            int eq = in->op == OP_BEQ;                                         \
            if (RREG(1)) in->xop = eq ? Q_BEQ_RR : Q_BNE_RR;                    \
            else if (KREF(1)) {                                                \
                in->xop = eq ? Q_BEQ_RK : Q_BNE_RK;                            \
                in->k.p = o[1].mode == OM_CONST ? o[1].rv.ptr : NULL;          \
            } else if (quicken_global(in, &o[1])) {                            \
                in->xop = eq ? Q_BEQ_RG : Q_BNE_RG;                            \
            }                                                                  \
        }                                                                      \
        break;
        QBRANCH_OPS(X)
#undef X
#define X(N, op_)                                                              \
    case OP_##N:                                                               \
        if (n == 3 && t == T_F64 && IREG(0) && FREG(1) && FREG(2)) in->xop = Q_##N##_FF; \
        else if (n == 3 && t == T_F64 && IREG(0) && FREG(1) && KFLT(2)) in->xop = Q_##N##_FK; \
        else if (n == 3 && t == T_R && IREG(0) && RREG(1) && (in->op == OP_SEQ || in->op == OP_SNE)) { \
            int eq = in->op == OP_SEQ;                                         \
            if (RREG(2)) in->xop = eq ? Q_SEQ_RR : Q_SNE_RR;                    \
            else if (KREF(2)) {                                                \
                in->xop = eq ? Q_SEQ_RK : Q_SNE_RK;                            \
                in->k.p = o[2].mode == OM_CONST ? o[2].rv.ptr : NULL;          \
            }                                                                  \
        }                                                                      \
        break;
        QSET_OPS(X)
#undef X
    case OP_LEN:
        if (IREG(0) && RREG(1)) in->xop = Q_LEN;
        break;
    case OP_LPUSH: { /* untyped: each value its register's or constant's natural type */
        bool regs = t == T_NONE && RREG(0), rk = regs;
        for (int k = 1; k < n; k++) {
            bool reg = IREG(k) || FREG(k) || RREG(k);
            regs = regs && reg;
            rk = rk && (reg || o[k].mode == OM_IMM || o[k].mode == OM_FIMM || o[k].mode == OM_CONST);
        }
        if (regs) {
            in->xop = Q_LPUSH_R;
        } else if (rk) {
            in->xop = Q_LPUSH_RK;
            for (int k = 1; k < n; k++) { /* as rdn reads them untyped */
                Word w = {0};
                if (o[k].mode == OM_IMM) w.i = o[k].imm, o[k].pad = T_I64;
                else if (o[k].mode == OM_FIMM) w.f = o[k].f, o[k].pad = T_F64;
                else if (o[k].mode == OM_CONST) w.r = o[k].rv.ptr, o[k].pad = T_R;
                else continue;
                memcpy(&o[k].kw, &w, 8);
            }
        }
        break;
    }
#define X(N, f)                                                                \
    case OP_##N:                                                               \
        if (t == T_F64 && n == 1 && FREG(0)) in->xop = Q_##N##_F;              \
        else if (t == T_F64 && n == 2 && FREG(0) && FREG(1)) in->xop = Q_##N##_FF; \
        break;
        QFUNARY_OPS(X)
#undef X
    case OP_TAGOF:
        if (IREG(0) && o[1].mode == OM_ELEM && !o[1].imm) {
            in->xop = Q_TAGOF_E;
            in->r[1] = (uint8_t)REG_IDX(o[1].reg);
            in->r[2] = (uint8_t)REG_IDX(o[1].ireg);
        }
        break;
    case OP_ISA:
        if (IREG(0) && RREG(1)) {
            in->xop = Q_ISA;
            in->k.p = o[2].rv.ptr;
        }
        break;
    case OP_BISA: /* bisa R0, Point, L: the struct type in k (the label is taken below) */
    case OP_BNISA:
        if (RREG(0)) {
            in->xop = in->op == OP_BISA ? Q_BISA : Q_BNISA;
            in->k.p = o[1].rv.ptr;
        }
        break;
    case OP_MGET: /* mget.r R0, R1, "key" [, null] */
        if (t == T_R && RREG(0) && RREG(1) && o[2].mode == OM_CONST &&
            ((const Obj *)o[2].rv.ptr)->kind == OBJ_STRING && (n == 3 || (KINT(3) && o[3].imm == 0))) {
            in->xop = Q_MGET_R;
            in->k.p = o[2].rv.ptr;
        } else if (n >= 3 && o[0].mode == OM_REG && REG_BANK(o[0].reg) < BANK_P && RREG(1) &&
                   o[2].mode == OM_REG && REG_BANK(o[2].reg) < BANK_P) { /* mget I0, R1, R2 [, -1]: any key register */
            /* a value of the destination's natural type (no suffix), or i64, f64, r */
            MlwsType vt = t == T_NONE ? reg_natural_type(o[0].reg) : t;
            Word w = {0};
            if ((vt != T_I64 && vt != T_F64 && vt != T_R) || REG_BANK(o[0].reg) != (int)type_bank(vt) ||
                (n == 4 && !const_word(&o[3], vt, &w)))
                break;
            in->xop = (uint16_t)(Q_MGETV_I + (int)type_bank(vt));
            in->k.i = w.i;                       /* the default */
            in->target = REG_BANK(o[2].reg) == BANK_I ? T_I64 : REG_BANK(o[2].reg) == BANK_F ? T_F64 : T_R; /* the key's type */
        }
        break;
    default: break;
    }
    if (in->xop >= Q__FIRST) /* the compact operand copy the quickened handlers read */
        for (int k = 0; k < n && k < 3; k++) {
            switch (o[k].mode) {
            case OM_REG: in->r[k] = (uint8_t)REG_IDX(o[k].reg); break;
            case OM_IMM: /* a float form's integer literal was converted at load time */
                if (t == T_F64) in->k.f = o[k].f;
                else in->k.i = o[k].imm;
                break;
            case OM_FIMM: in->k.f = o[k].f; break;
#if MLWS_TAILCALL /* relative: the handlers have the instruction, not the code array */
            case OM_LABEL: in->target = (uint32_t)((int64_t)o[k].rv.target - (int64_t)quicken_idx); break;
#else
            case OM_LABEL: in->target = o[k].rv.target; break;
#endif
            default: break;
            }
        }
    /* after the compact copy, which would take the constant for k */
    if (in->xop == OP_MOV && n == 2 && t != T_P && wide_type(t)) quicken_field_const(in, t);
#undef IREG
#undef FREG
#undef RREG
#undef PREG
#undef KINT
#undef KFLT
#undef KREF
}

/* ---- main loop ---------------------------------------------------------------- */

/* Message at the head of the mailbox, as a tagged slot location. */
static Loc mbox_loc(Message *m) {
    Loc L;
    memset(&L, 0, sizeof L);
    L.is_reg = LK_SLOT;
    L.slot = &m->v;
    L.tag = &m->tag;
    return L;
}

static void mbox_take(Process *p) {
    p->mbox_head++;
    p->msgs_received++;
    p->mcount--;
    if (p->mbox_head == p->mbox.n) {
        p->mbox_head = p->mbox.n = 0;
    } else if (p->mbox_head > 64 && p->mbox_head * 2 > p->mbox.n) {
        memmove(p->mbox.v, p->mbox.v + p->mbox_head, (p->mbox.n - p->mbox_head) * sizeof(Message));
        p->mbox.n -= p->mbox_head;
        p->mbox_head = 0;
    }
}

/* spawnl / spawnm: link or monitor a child before it is started, so it cannot
 * end first. Nobody else can see the child yet. */
static void spawn_watch(Process *p, Process *child, bool link, bool monitor) {
    if (link) {
        VEC_PUSH(child->links, p->pid);
        sched_add_link(p, child->pid);
    }
    if (monitor) VEC_PUSH(child->monitors, p->pid);
}

/* The entry function, a callback or a coroutine returned: keep its results
 * (for the embedding API, proc_callback's caller or interp_run). */
static void keep_results(Process *p, const Word *vals, const MlwsType *types, int n) {
    p->nresults = (uint8_t)n;
    for (int k = 0; k < p->nresults; k++) {
        p->results[k] = vals[k];
        p->result_types[k] = (uint8_t)types[k];
    }
}

void interp_enter(Process *p, ObjClosure *cl, const Word *args) {
    Ctx c;
    c.p = p;
    proc_push_frame(p, cl->fn, NULL); /* no caller instruction: its ret ends the callback */
    p->frames[p->nframes - 1].closure = &cl->h;
    ctx_load(&c);
    bind_args(&c, NULL, cl->fn->mf->params, cl->fn->mf->nparams, args);
}

/* Q_CALLI_RK: a closure call with constants among its arguments, checked
 * against the callee here. Returns false, having done nothing, for anything the
 * generic call must handle. */
static MLWS_NOINLINE bool call_rk(Process *p, const Insn *in) {
    const Operand *qo = in->ops;
    const Frame *cf = &p->frames[p->nframes - 1];
    Obj *clo;
    memcpy(&clo, &p->regs[cf->base + qo[0].pad], sizeof clo);
    if (!clo || clo->kind != OBJ_CLOSURE) return false;
    Function *fn = ((ObjClosure *)clo)->fn;
    const MFunc *mf = fn->mf;
    int argc = (int)qo[1].imm, nres = in->nops - 2 - argc;
    if (argc != mf->nparams || nres > mf->nrets || (nres && !(mf->flags & MF_RETS_DECLARED))) return false;
    for (int k = 0; k < argc; k++) {
        int pt = mf->params[k];
        Word w;
        if (!wide_type(pt)) return false;
        if (qo[2 + k].mode == OM_REG ? REG_BANK(qo[2 + k].reg) != (int)type_bank((MlwsType)pt)
                                     : !const_word(&qo[2 + k], pt, &w))
            return false;
    }
    for (int k = 0; k < nres; k++)
        if (!wide_type(mf->rets[k]) || REG_BANK(qo[2 + argc + k].reg) != (int)type_bank((MlwsType)mf->rets[k]))
            return false;
    proc_push_frame(p, fn, in); /* may move the frames and the banks */
    Frame *nf = &p->frames[p->nframes - 1];
    const Frame *of = nf - 1;
    nf->closure = clo;
    nf->qret = 1;
    uint64_t *nw = p->regs + nf->base;
    const uint64_t *ow = p->regs + of->base;
    int cnt[NBANKS] = {0};
    for (int k = 0; k < argc; k++) {
        const Operand *a = &qo[2 + k];
        int b = (int)type_bank((MlwsType)mf->params[k]);
        uint64_t *d = &nw[fn->roff[b] + cnt[b]++];
        if (a->mode != OM_REG) {
            Word w = {0};
            const_word(a, mf->params[k], &w);
            memcpy(d, &w, 8);
        } else {
            memcpy(d, &ow[a->pad], 8); /* the caller's slot (interp_quicken_function) */
        }
    }
    return true;
}

/* Threaded dispatch needs GNU C's labels as values; other compilers use the switch. */
#if (defined(__GNUC__) || defined(__clang__)) && !defined(MLWS_NO_THREADED)
#define MLWS_THREADED 1
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#ifdef __clang__
#pragma GCC diagnostic ignored "-Winitializer-overrides"
#else
#pragma GCC diagnostic ignored "-Woverride-init"
#endif
#else
#define MLWS_THREADED 0
#endif

#if MLWS_TAILCALL
/* ---- tail-call dispatch ---------------------------------------------------------
 * Every quickened form is a function of its own (h_Q_*, from qforms.inc) that
 * ends by tail-calling the next instruction's handler, with the fast path's
 * state in argument registers: the Ctx, the instruction, the slice counter and
 * the I, F and R windows (six arguments, x86-64's register limit). A compiler
 * allocates each handler's registers on its own, so no handler's speed depends
 * on the code of any other, as it does in one dispatch loop. Branch targets
 * are relative to the instruction (interp_quicken), so no code base is needed.
 * Instructions without a quickened form go to h_generic, which runs them with
 * exec_generic (generic.inc); h_slice ends the slice and runs the tools' hook. */
#define MUSTTAIL __attribute__((musttail))
/* No register is callee-saved between handlers (Clang): a handler that needs
 * many registers does not save and restore them on every instruction. */
/* (Not in sanitizer builds: with AddressSanitizer, Clang's code for run_loop
 * expects a register back that a preserve_none handler need not preserve.) */
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
#define MLWS_SANITIZED 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define MLWS_SANITIZED 1
#endif
#if defined(__has_attribute) && !defined(MLWS_SANITIZED)
#if __has_attribute(preserve_none)
#define MLWS_PRESERVE_NONE 1
#endif
#endif
#ifdef MLWS_PRESERVE_NONE
#define QCC __attribute__((preserve_none))
#define QWIDE 1
#else
#define QCC
#define QWIDE 0
#endif
/* With preserve_none there are registers for more arguments: the current frame
 * and its code travel with the rest, not through c->fr and c->code (a store in
 * one handler and a load on the critical path of the next). */
#if QWIDE
#define QPARAMS Ctx *c, const Insn *in, uint32_t left, int64_t *I, double *F, Obj **R, Frame *qfr, const Insn *qcode
#define QARGSN(n) c, n, left, I, F, R, qfr, qcode
#define QFRAME qfr
#define QCODE qcode
#define QSETFRAME() (qfr = c->fr, qcode = qfr->fn->code)
#else
#define QPARAMS Ctx *c, const Insn *in, uint32_t left, int64_t *I, double *F, Obj **R
#define QARGSN(n) c, n, left, I, F, R
#define QFRAME (c->fr)
#define QCODE (c->code)
#define QSETFRAME() (c->code = c->fr->fn->code)
#endif
#define QARGS QARGSN(in)
typedef QCC int (*QHandler)(QPARAMS);
static const QHandler qhandlers[Q__COUNT];
static QCC int h_generic(QPARAMS);
static QCC int h_slice(QPARAMS);

#define QI I
#define QF F
#define QR R
#define QP (c->P)
#define QB(b) Q##b
#define QCTX c
/* The process and frame as locals of each handler (unused ones cost nothing):
 * read through `c`, every byte stored (a frame's qret, a list tag) could
 * alias them, and the compiler would read them again after each one. */
#define QPRC qp
#define QFRM qf
#define QLOCALS                                                                \
    const Insn *nx = in + 1;                                                   \
    Process *const qp __attribute__((unused)) = c->p;                          \
    Frame *const qf __attribute__((unused)) = QFRAME;
#define IR(k) QI[in->r[k]]
#define FR(k) QF[in->r[k]]
#define U(x) ((uint64_t)(x))
/* Frame.pc is the index after `in` (the return address, or where a fault is),
 * and Process.insns counts every instruction started, as in the loop. */
#define SPILL() (QFRM->pc = (uint32_t)(in - QCODE) + 1, QPRC->insns = c->ibase - left)
#define RELOAD() (I = c->I, F = c->F, R = c->R, QSETFRAME(), nx = QCODE + QFRAME->pc)
#define JUMP() (nx = in + (int32_t)in->target)
#define JUMPTO(t) (nx = QCODE + (t))
#define GENERIC()                                                              \
    do {                                                                       \
        MUSTTAIL return h_generic(QARGS);                                      \
    } while (0)
#define NEXT                                                                   \
    do {                                                                       \
        if (__builtin_expect(left == 0, 0)) MUSTTAIL return h_slice(QARGSN(nx)); \
        left--;                                                                \
        MUSTTAIL return qhandlers[nx->xop](QARGSN(nx));                        \
    } while (0)
/* Each QCASE ends the previous handler's function and starts the next: the
 * first one ends this placeholder, and the brace after the include the last. */
#define QCASE(x)                                                               \
    }                                                                          \
    static QCC int h_##x(QPARAMS) {                                            \
        QLOCALS
static __attribute__((unused)) QCC int qforms_start(QPARAMS) {
    (void)c, (void)in, (void)left, (void)I, (void)F, (void)R;
#if QWIDE
    (void)qfr, (void)qcode;
#endif
    return 0;
#define QFORMS_RET_INLINE 0
/* A call that needs the stacks to grow (or reaches the depth limit) is the
 * generic call's: without a function call of its own, the handler needs no
 * stack frame. */
#define QPUSH_FRAME(f, ri)                                                     \
    do {                                                                       \
        if (__builtin_expect(!proc_frame_fits(QPRC, (f)->rsize), 0)) GENERIC();   \
        proc_push_frame_fitting(QPRC, f, ri);                                  \
    } while (0)
#include "qforms.inc"
#undef QPUSH_FRAME
#undef QFORMS_RET_INLINE
}
#undef QCASE
#undef NEXT
#undef GENERIC
#undef JUMP
#undef JUMPTO
#undef RELOAD
#undef SPILL
#undef U
#undef IR
#undef FR
#undef QI
#undef QF
#undef QR
#undef QP
#undef QB
#undef QCTX
#undef QPRC
#undef QFRM
#undef QLOCALS

/* `in` is the next instruction to run: the slice is used up, or a tool's hook
 * runs before each instruction (then `left` stays 0 and hleft counts). */
static MLWS_NOINLINE QCC int h_slice(QPARAMS) {
    Process *p = c->p;
    uint32_t pc = (uint32_t)(in - QCODE);
    QFRAME->pc = pc;
    p->insns = c->ibase - left;
    if (!c->hooked || c->hleft == 0) return SLICE_YIELD; /* time slice used up */
    if (c->vm->hook(p, pc)) return SLICE_BREAK; /* debugger stop: pc not advanced */
    c->hleft--, c->ibase++;
    left = 0;
    MUSTTAIL return qhandlers[in->xop](QARGS);
}

/* The generic forms, in this one function: with the handlers' calling
 * convention it saves no registers for its caller, and it goes on to the next
 * instruction with a tail call like any handler. Its own register allocation
 * (for the generic forms' many paths) cannot affect the quickened forms'. */
#if defined(__has_attribute) && __has_attribute(no_stack_protector)
#define QNOSP __attribute__((no_stack_protector))
#else
#define QNOSP
#endif
/* (Without a stack-protector check: its arrays, bounded by the operand count,
 * would otherwise cost a canary per generic instruction, which the dispatch
 * loop pays once per slice.) */
static QCC QNOSP int h_generic(QPARAMS) {
    Process *p = c->p;
    VM *vm = p->vm;
    Frame *fr = QFRAME;
    fr->pc = (uint32_t)(in - QCODE) + 1;
    p->insns = c->ibase - left;
#define CX c
#include "generic.inc"
#undef CX
    /* Safepoint: between instructions every live reference is in a root. */
    if (p->gc_pending | p->kill_pending) {
        if (p->kill_pending) {
            proc_backtrace(p);
            return SLICE_FAILED;
        }
        gc_collect(p);
    }
    /* What is left of the slice: unchanged, unless a native's callback ran
     * instructions too. */
    if (p->insns != c->ibase - left) {
        uint64_t used = p->insns - c->insns0;
        uint32_t rest = used < c->budget ? c->budget - (uint32_t)used : 0;
        if (c->hooked) c->hleft = rest, left = 0;
        else left = rest;
        c->ibase = c->insns0 + c->budget - c->hleft;
    }
    QSETFRAME();
    const Insn *nx = QCODE + QFRAME->pc;
    I = c->I, F = c->F, R = c->R;
    if (__builtin_expect(left == 0, 0)) MUSTTAIL return h_slice(QARGSN(nx));
    left--;
    MUSTTAIL return qhandlers[nx->xop](QARGSN(nx));
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#ifdef __clang__
#pragma GCC diagnostic ignored "-Winitializer-overrides"
#else
#pragma GCC diagnostic ignored "-Woverride-init"
#endif
static const QHandler qhandlers[Q__COUNT] = {
    [0 ... Q__COUNT - 1] = h_generic,
#define QL(x) [x] = h_##x,
#include "qlist.inc"
#undef QL
};
#pragma GCC diagnostic pop

static MLWS_NOINLINE int run_loop(Process *p, uint32_t budget) {
    Ctx c;
    c.p = p;
    ctx_load(&c);
    c.vm = p->vm;
    c.hooked = p->vm->hook != NULL; /* set before a run starts (debugger, tracer, profiler) */
    c.insns0 = p->insns;
    c.budget = budget;
    c.hleft = c.hooked ? budget : 0;
    c.ibase = c.insns0 + budget - c.hleft;
    c.code = c.fr->fn->code;
    uint32_t left = c.hooked ? 0 : budget;
    if (p->gc_pending | p->kill_pending) {
        if (p->kill_pending) {
            proc_backtrace(p);
            return SLICE_FAILED;
        }
        gc_collect(p);
    }
    const Insn *in = c.code + c.fr->pc;
#if QWIDE
#define QSTART &c, in, left, c.I, c.F, c.R, c.fr, c.code
#else
#define QSTART &c, in, left, c.I, c.F, c.R
#endif
    if (left == 0) return h_slice(QSTART);
    left--;
    return qhandlers[in->xop](QSTART);
#undef QSTART
}
#else /* the dispatch loop */

static MLWS_NOINLINE int run_loop(Process *p, uint32_t budget) {
    Ctx c;
    VM *vm = p->vm;
    c.p = p;
    ctx_load(&c);
    /* The fast path keeps the program counter in `pc` and counts the slice down
     * in `left`; Frame.pc and Process.insns are written back (SPILL) only where
     * something else can look at them: before a generic instruction (it may trap,
     * call a native or switch frames), a call or return, the tools' hook, and
     * the end of the slice. Quickened forms neither allocate nor trap (calls and
     * returns spill first), so the safepoint for a collection or a pending kill
     * is after each generic instruction, and once at the start of the slice. */
    const bool hooked = vm->hook != NULL; /* set before a run starts (debugger, tracer, profiler) */
    const uint64_t insns0 = p->insns;
    uint32_t left = budget;
    Frame *fr = c.fr;
    const Insn *code = fr->fn->code;
    uint32_t pc = fr->pc;
#define SPILL() (fr->pc = pc, p->insns = insns0 + (budget - left))
#define RELOAD() (fr = c.fr, code = fr->fn->code, pc = fr->pc)
#define NEXT continue
    if (p->gc_pending | p->kill_pending) {
        if (p->kill_pending) {
            proc_backtrace(p);
            return SLICE_FAILED;
        }
        gc_collect(p);
    }
#if MLWS_THREADED
    /* Threaded dispatch: each quickened handler ends by jumping straight to the
     * next instruction's handler, which branch predictors handle better than one
     * shared switch. Instructions that are not quickened go to `generic`. */
    static const void *const qtab[Q__COUNT] = {
        [0 ... Q__COUNT - 1] = &&generic,
#define QL(x) [x] = &&L_##x,
#include "qlist.inc"
#undef QL
    };
#define QCASE(x) L_##x:
#undef NEXT
#define NEXT                                                                   \
    {                                                                          \
        if (__builtin_expect(left == 0 || hooked, 0)) goto top;                \
        left--;                                                                \
        in = &code[pc++];                                                      \
        goto *qtab[in->xop];                                                   \
    }
#else
#define QCASE(x) case x:
#endif
    const Insn *in;
    for (;;) {
#if MLWS_THREADED
    top:
#endif
        if (left == 0) { /* time slice used up */
            SPILL();
            return SLICE_YIELD;
        }
        if (hooked) {
            SPILL();
            if (vm->hook(p, pc)) return SLICE_BREAK; /* debugger stop: pc not advanced */
        }
        left--;
        in = &code[pc++];
#define QI c.I
#define QF c.F
#define QR c.R
#define QP c.P
#define QB(b) Q##b
#define QCTX (&c)
#define QPRC p
#define QFRM fr
#define GENERIC() goto generic
#define JUMP() (pc = in->target)
#define JUMPTO(t) (pc = (t))
#define IR(k) QI[in->r[k]]
#define FR(k) QF[in->r[k]]
#define U(x) ((uint64_t)(x))
#if MLWS_THREADED
        goto *qtab[in->xop];
#else
        switch (in->xop) {
#endif
#define QFORMS_RET_INLINE 1
#define QPUSH_FRAME(f, ri) proc_push_frame(QPRC, f, ri)
#include "qforms.inc"
#undef QPUSH_FRAME
#undef QFORMS_RET_INLINE
#if MLWS_THREADED
    generic:
#else
        default:
            break; /* not quickened: the generic forms below */
        }
    generic:
#endif
#undef IR
#undef FR
#undef QI
#undef QF
#undef QR
#undef QP
#undef QB
#undef QCTX
#undef QPRC
#undef QFRM
#undef GENERIC
#undef JUMP
#undef JUMPTO

        /* ---- generic forms ---- */
        SPILL();
#if defined(__x86_64__) || defined(__i386__)
        /* On x86, with its 16 registers, the generic forms take their
         * instruction and frame afresh (gin, gfr), so the fast path's `in` and
         * `fr` are not live across these forms' calls: kept live, GCC moved
         * `in` to the stack and every quickened handler paid a load for it.
         * On arm64 (31 registers) the same change made `loop` 46% slower. */
        const Insn *const gin = &fr->fn->code[fr->pc - 1];
        Frame *gfr = fr;
#define in gin
#define fr gfr
#endif
#define CX (&c)
#include "generic.inc"
#undef CX
#if defined(__x86_64__) || defined(__i386__)
#undef in
#undef fr
#endif
        /* Safepoint: between instructions every live reference is in a root. */
        if (p->gc_pending | p->kill_pending) {
            if (p->kill_pending) {
                proc_backtrace(p);
                return SLICE_FAILED;
            }
            gc_collect(p);
        }
        /* The slice counter is recomputed from what SPILL wrote, not kept: then
         * it is not live across this path's calls, and the compiler is free to
         * keep it in a register in the quickened forms. (Instructions run by a
         * callback during a native call count against the slice too.) */
        {
            uint64_t used = p->insns - insns0;
            left = used < budget ? budget - (uint32_t)used : 0;
        }
        RELOAD(); /* the instruction may have branched, called, returned or thrown */
    }
#undef SPILL
#undef RELOAD
#undef NEXT
#undef QCASE
}
#if MLWS_THREADED
#pragma GCC diagnostic pop
#endif
#endif /* MLWS_TAILCALL */

/* run_loop's main loop is sensitive to every line added to it: how a compiler
 * then allocates its registers can move the speed of the quickened forms by
 * 10% or more, differently with each compiler. So rare instructions (the
 * coroutines') and a coroutine's end are handled here, outside it: run_loop
 * returns SLICE_EXT for an instruction it does not run, and a coroutine's last
 * frame returns like an entry function does (SLICE_DONE with no frames left). */
int interp_run(Process *p, uint32_t budget) {
    const uint64_t insns0 = p->insns;
    for (;;) {
        uint64_t used = p->insns - insns0;
        if (used >= budget) return SLICE_YIELD;
        int64_t exit_code = p->exit_code;
        int r = run_loop(p, budget - (uint32_t)used);
        if (r == SLICE_EXT) {
            Frame *fr = &p->frames[p->nframes - 1];
            Ctx c;
            c.p = p;
            ctx_load(&c);
            co_exec(&c, &fr->fn->code[fr->pc - 1]);
        } else if (r == SLICE_DONE && p->co && !p->nframes) {
            p->exit_code = exit_code; /* its ret set this, as an entry function's would */
            Ctx c;
            c.p = p;
            co_finish(&c);
        } else {
            return r;
        }
    }
}
