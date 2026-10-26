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

typedef struct {
    Process *p;
    Frame *fr;
    int64_t *I;
    double *F;
    Obj **R;
    uint64_t *P;
} Ctx;

static void ctx_load(Ctx *c) {
    Process *p = c->p;
    c->fr = &p->frames[p->nframes - 1];
    c->I = p->ireg + c->fr->base[BANK_I];
    c->F = p->freg + c->fr->base[BANK_F];
    c->R = p->rreg + c->fr->base[BANK_R];
    c->P = p->preg + c->fr->base[BANK_P];
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
    int64_t idx = o->mode == OM_ELEM ? c->I[REG_IDX(o->ireg)] : (int64_t)o->index;
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

static void mem_write(Ctx *c, const Loc *L, MlwsType t, Word w) {
    int width = type_size(t);
    uint8_t *m = mem_ptr(c, L, width);
    if (t == T_R) {
        if (!L->seg->tags || L->off % 8)
            vm_trap(c->p, "E_TYPE", "references can only be stored in word-aligned stack or global slots");
        memcpy(m, &w.r, sizeof w.r);
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
    default: {
        Loc L;
        loc_of(c, o, *t ? type_size(*t) : 8, &L);
        return rd_loc(c, &L, t);
    }
    }
}

static Word rd(Ctx *c, const Operand *o, MlwsType t) { return rdn(c, o, &t); }

static void wr(Ctx *c, const Operand *o, MlwsType t, Word w) {
    t = AS_REF(t);
    if (o->mode == OM_REG) {
        reg_write(c, o->reg, t, w);
        return;
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

/* ---- quickening ------------------------------------------------------------------
 * After verification, common instruction shapes get a specialised execution
 * opcode (Insn.xop) that skips the generic operand machinery. The original
 * opcode stays in Insn.op for the disassembler, tracer, profiler and debugger.
 * Only shapes whose generic semantics are trivially the same qualify: 64-bit
 * integers in I registers (no width canonicalisation needed), f64 in F registers. */

#define QARITH_OPS(X) X(ADD, +) X(SUB, -) X(MUL, *) X(AND, &) X(OR, |) X(XOR, ^)
#define QFARITH_OPS(X) X(ADD, +) X(SUB, -) X(MUL, *) X(DIV, /)
#define QBRANCH_OPS(X) X(BEQ, ==) X(BNE, !=) X(BLT, <) X(BLE, <=) X(BGT, >) X(BGE, >=)

enum {
    Q__FIRST = OP__COUNT,
    Q_MOV_II, Q_MOV_IK, Q_MOV_FF, Q_MOV_FK, Q_MOV_RR, Q_MOV_PP,
#define X(N, o) Q_##N##_III, Q_##N##_IIK, Q_##N##_II, Q_##N##_IK,
    QARITH_OPS(X)
#undef X
#define X(N, o) Q_F##N##_FFF, Q_F##N##_FFK, Q_F##N##_FF, Q_F##N##_FK,
    QFARITH_OPS(X)
#undef X
#define X(N, o) Q_##N##_II, Q_##N##_IK,
    QBRANCH_OPS(X)
#undef X
    Q_REM_IIK, Q_REM_IK, Q_DIV_IIK, Q_DIV_IK,
    Q_INC_I, Q_DEC_I, Q_BZ_I, Q_BNZ_I, Q_JMP,
    Q_CALL_R, /* call whose arguments and results are registers of the right banks */
    Q_RET_R,  /* ret of registers matching the declared 64-bit result types */
};

/* 64-bit register types: values move between registers without canonicalisation. */
static bool wide_type(int t) { return t == T_I64 || t == T_U64 || t == T_F64 || t == T_R || t == T_P; }

static bool reg_of_type(const Operand *o, int t) {
    return o->mode == OM_REG && REG_BANK(o->reg) < NBANKS && REG_BANK(o->reg) == (int)type_bank((MlwsType)t) &&
           wide_type(t);
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

void interp_quicken_function(Function *f) {
    quicken_fn = f->mf;
    for (uint32_t k = 0; k < f->ncode; k++) interp_quicken(&f->code[k]);
    quicken_fn = NULL;
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
    float_arith:
        if (t != T_F64) break;
        switch (in->op) {
#define X(N, op_)                                                              \
    case OP_##N:                                                               \
        if (n == 3 && FREG(0) && FREG(1) && FREG(2)) in->xop = Q_F##N##_FFF;   \
        else if (n == 3 && FREG(0) && FREG(1) && KFLT(2)) in->xop = Q_F##N##_FFK; \
        else if (n == 2 && FREG(0) && FREG(1)) in->xop = Q_F##N##_FF;         \
        else if (n == 2 && FREG(0) && KFLT(1)) in->xop = Q_F##N##_FK;         \
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
        break;
    case OP_BNZ:
        if (i64 && IREG(0)) in->xop = Q_BNZ_I;
        break;
    case OP_JMP: in->xop = Q_JMP; break;
    case OP_CALL:
        if (quicken_call(in)) in->xop = Q_CALL_R;
        break;
    case OP_RET:
        if (quicken_fn && (quicken_fn->flags & MF_RETS_DECLARED) && n == quicken_fn->nrets) {
            bool ok = true;
            for (int k = 0; k < n; k++) ok = ok && reg_of_type(&o[k], quicken_fn->rets[k]);
            if (ok) in->xop = Q_RET_R;
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
        }                                                                      \
        break;
        QBRANCH_OPS(X)
#undef X
    default: break;
    }
#undef IREG
#undef FREG
#undef RREG
#undef PREG
#undef KINT
#undef KFLT
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

int interp_run(Process *p, uint32_t budget) {
    Ctx c;
    VM *vm = p->vm;
    c.p = p;
    ctx_load(&c);
    for (;;) {
        if (budget-- == 0) return SLICE_YIELD; /* time slice used up */
        /* Safepoint: between instructions every live reference is in a root. */
        if (p->gc_pending | p->kill_pending) {
            if (p->kill_pending) {
                proc_backtrace(p);
                return SLICE_FAILED;
            }
            gc_collect(p);
        }
        Frame *fr = c.fr;
        if (vm->hook && vm->hook(p, fr->pc)) return SLICE_BREAK; /* debugger stop: pc not advanced */
        const Insn *in = &fr->fn->code[fr->pc++];
        const Operand *ops = in->ops;
        MlwsType T = (MlwsType)in->type;
        MlwsType TN = T; /* the type as written: .big / .dec select number arithmetic */
        if (T == T_BIG || T == T_DEC) T = T_R;
        p->insns++;
#define IR(k) c.I[REG_IDX(ops[k].reg)]
#define FR(k) c.F[REG_IDX(ops[k].reg)]
#define U(x) ((uint64_t)(x))
        switch (in->xop) {
        /* ---- quickened forms (see interp_quicken) ---- */
        case Q_MOV_II: IR(0) = IR(1); break;
        case Q_MOV_IK: IR(0) = ops[1].imm; break;
        case Q_MOV_FF: FR(0) = FR(1); break;
        case Q_MOV_FK: FR(0) = ops[1].f; break;
        case Q_MOV_RR: c.R[REG_IDX(ops[0].reg)] = c.R[REG_IDX(ops[1].reg)]; break;
        case Q_MOV_PP: c.P[REG_IDX(ops[0].reg)] = c.P[REG_IDX(ops[1].reg)]; break;
#define X(N, op_)                                                              \
        case Q_##N##_III: IR(0) = (int64_t)(U(IR(1)) op_ U(IR(2))); break;     \
        case Q_##N##_IIK: IR(0) = (int64_t)(U(IR(1)) op_ U(ops[2].imm)); break; \
        case Q_##N##_II: IR(0) = (int64_t)(U(IR(0)) op_ U(IR(1))); break;      \
        case Q_##N##_IK: IR(0) = (int64_t)(U(IR(0)) op_ U(ops[1].imm)); break;
        QARITH_OPS(X)
#undef X
#define X(N, op_)                                                              \
        case Q_F##N##_FFF: FR(0) = FR(1) op_ FR(2); break;                     \
        case Q_F##N##_FFK: FR(0) = FR(1) op_ ops[2].f; break;                  \
        case Q_F##N##_FF: FR(0) = FR(0) op_ FR(1); break;                      \
        case Q_F##N##_FK: FR(0) = FR(0) op_ ops[1].f; break;
        QFARITH_OPS(X)
#undef X
        case Q_DIV_IIK: IR(0) = IR(1) / ops[2].imm; break; /* divisor is a non-zero, non -1 constant */
        case Q_DIV_IK: IR(0) = IR(0) / ops[1].imm; break;
        case Q_REM_IIK: IR(0) = IR(1) % ops[2].imm; break;
        case Q_REM_IK: IR(0) = IR(0) % ops[1].imm; break;
        case Q_INC_I: IR(0) = (int64_t)(U(IR(0)) + 1); break;
        case Q_DEC_I: IR(0) = (int64_t)(U(IR(0)) - 1); break;
        case Q_BZ_I: if (IR(0) == 0) fr->pc = ops[1].rv.target; break;
        case Q_BNZ_I: if (IR(0) != 0) fr->pc = ops[1].rv.target; break;
        case Q_JMP: fr->pc = ops[0].rv.target; break;
        case Q_CALL_R: {
            Function *fn = ops[0].rv.ptr;
            int argc = (int)ops[1].imm;
            Word args[MAX_OPERANDS];
            for (int k = 0; k < argc; k++) {
                uint8_t r = ops[2 + k].reg;
                switch (REG_BANK(r)) {
                case BANK_I: args[k].i = c.I[REG_IDX(r)]; break;
                case BANK_F: args[k].f = c.F[REG_IDX(r)]; break;
                case BANK_R: args[k].r = c.R[REG_IDX(r)]; break;
                default: args[k].p = c.P[REG_IDX(r)]; break;
                }
            }
            proc_push_frame(p, fn, in);
            ctx_load(&c);
            bind_args(&c, NULL, fn->mf->params, argc, args);
            break;
        }
        case Q_RET_R: {
            Word vals[MAX_OPERANDS];
            int n = in->nops;
            for (int k = 0; k < n; k++) {
                uint8_t r = ops[k].reg;
                switch (REG_BANK(r)) {
                case BANK_I: vals[k].i = c.I[REG_IDX(r)]; break;
                case BANK_F: vals[k].f = c.F[REG_IDX(r)]; break;
                case BANK_R: vals[k].r = c.R[REG_IDX(r)]; break;
                default: vals[k].p = c.P[REG_IDX(r)]; break;
                }
            }
            const MFunc *mf = fr->fn->mf;
            const Insn *ri = fr->ret_insn;
            proc_pop_frame(p);
            if (p->nframes == 0) {
                p->exit_code = n > 0 && type_is_int((MlwsType)mf->rets[0]) ? vals[0].i : 0;
                return SLICE_DONE;
            }
            ctx_load(&c);
            if (!ri) break;
            int argc = (int)ri->ops[1].imm, nres = ri->nops - 2 - argc;
            if (nres > n)
                vm_trap(p, "E_RET_COUNT", "%s returned %d value(s) but the caller expects %d", fr->fn->name, n, nres);
            if (ri->xop == Q_CALL_R) { /* both sides are registers of the right banks */
                for (int k = 0; k < nres; k++) {
                    uint8_t r = ri->ops[2 + argc + k].reg;
                    switch (REG_BANK(r)) {
                    case BANK_I: c.I[REG_IDX(r)] = vals[k].i; break;
                    case BANK_F: c.F[REG_IDX(r)] = vals[k].f; break;
                    case BANK_R: c.R[REG_IDX(r)] = vals[k].r; break;
                    default: c.P[REG_IDX(r)] = vals[k].p; break;
                    }
                }
            } else {
                for (int k = 0; k < nres; k++)
                    wr(&c, &ri->ops[2 + argc + k], n ? (MlwsType)mf->rets[k] : T_NONE, vals[k]);
            }
            break;
        }
#define X(N, op_)                                                              \
        case Q_##N##_II: if (IR(0) op_ IR(1)) fr->pc = ops[2].rv.target; break; \
        case Q_##N##_IK: if (IR(0) op_ ops[1].imm) fr->pc = ops[2].rv.target; break;
        QBRANCH_OPS(X)
#undef X
#undef U
        /* ---- everything else ---- */
        case OP_NOP:
            break;
        case OP_BREAK: /* a debugger stops here; without one it is a no-op */
            if (vm->hook) {
                vm->stopped = p;
                return SLICE_BREAK;
            }
            break;
        case OP_HALT:
            p->exit_code = in->nops ? rd(&c, &ops[0], T_I64).i : 0;
            return SLICE_DONE;
        case OP_ASSERT:
            if (rd(&c, &ops[0], T_I64).i == 0)
                vm_trap(p, "E_ASSERT", "%s",
                        in->nops > 1 && ((Obj *)ops[1].rv.ptr)->kind == OBJ_STRING ? ((ObjString *)ops[1].rv.ptr)->data
                                                                                  : "assertion failed");
            break;

        case OP_MOV: {
            Loc d;
            loc_of(&c, &ops[0], T ? type_size(T) : 8, &d);
            if (TN != T) { /* mov.big / mov.dec: literals become numbers */
                wr_num(&c, &d, rd_num(&c, &ops[1], TN));
                break;
            }
            MlwsType t = T;
            Word v = rdn(&c, &ops[1], &t);
            wr_loc(&c, &d, t, v);
            break;
        }
        case OP_SWAP: {
            Loc a, b;
            loc_of(&c, &ops[0], T ? type_size(T) : 8, &a);
            loc_of(&c, &ops[1], T ? type_size(T) : 8, &b);
            MlwsType ta = T, tb = T;
            Word va = rd_loc(&c, &a, &ta), vb = rd_loc(&c, &b, &tb);
            wr_loc(&c, &a, tb, vb);
            wr_loc(&c, &b, ta, va);
            break;
        }
        case OP_LEA: {
            Loc d;
            loc_of(&c, &ops[0], 8, &d);
            Word w;
            w.p = op_addr(&c, &ops[1], 8);
            wr_loc(&c, &d, T_P, w);
            break;
        }
        case OP_PUSH:
            for (int k = 0; k < in->nops; k++) {
                MlwsType t = T;
                Word v = rdn(&c, &ops[k], &t);
                push_word(&c, t, v);
            }
            break;
        case OP_POP:
            for (int k = 0; k < in->nops; k++) {
                MlwsType t = T;
                if (t == T_NONE && ops[k].mode == OM_REG) t = reg_natural_type(ops[k].reg);
                Loc top = mem_loc(p, p->sp);
                Word v = rd_loc(&c, &top, &t);
                p->sp = ptr_add(&c, p->sp, 8);
                wr(&c, &ops[k], t, v);
            }
            break;
        case OP_DROP:
            p->sp = ptr_add(&c, p->sp, (in->nops ? ops[0].imm : 1) * 8);
            break;
        case OP_ENTER: {
            int64_t n = ops[0].imm;
            if (n > (int64_t)p->stack.size) vm_trap(p, "E_STACK_OVERFLOW", "data stack overflow");
            uint64_t sp = ptr_add(&c, p->sp, -n * 8);
            Loc L = mem_loc(p, sp);
            memset(mem_ptr(&c, &L, (int)(n * 8)), 0, (size_t)n * 8);
            if (L.seg->tags) memset(L.seg->tags + L.off / 8, T_NONE, (size_t)n);
            p->sp = sp;
            note_sp(p);
            break;
        }
        case OP_CVT:
        case OP_BITCAST: {
            if (in->op == OP_CVT && (type_is_ref(in->type) || type_is_ref(in->type2))) {
                convert_num(&c, in);
                break;
            }
            Loc d;
            loc_of(&c, &ops[0], type_size(T), &d);
            MlwsType from = (MlwsType)in->type2;
            Word v = rd(&c, &ops[in->nops - 1], from);
            wr_loc(&c, &d, T, in->op == OP_CVT ? convert(T, from, v) : bitcast(T, from, v));
            break;
        }

        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_REM: case OP_MIN: case OP_MAX:
        case OP_AND: case OP_OR: case OP_XOR:
            if (TN != T) arith_num(&c, in, TN);
            else arith(&c, in);
            break;
        case OP_NEG: case OP_ABS: case OP_NOT: case OP_POPCNT: case OP_CLZ: case OP_CTZ: case OP_BSWAP:
        case OP_SQRT: case OP_FLOOR: case OP_CEIL: case OP_ROUND: case OP_TRUNC: {
            Loc d;
            loc_of(&c, &ops[0], type_size(T), &d);
            if (TN != T) { /* neg/abs of a number */
                const Obj *a;
                if (in->nops == 2) a = rd_num(&c, &ops[1], TN);
                else {
                    MlwsType rt = T_R;
                    a = rd_loc(&c, &d, &rt).r;
                    if (!num_kind_matches(a, TN)) vm_trap(p, "E_TYPE", "destination does not hold a %s", type_name(TN));
                }
                wr_num(&c, &d, num_unary(p, in->op, a));
                break;
            }
            MlwsType t = T;
            Word a = in->nops == 2 ? rd(&c, &ops[1], T) : rd_loc(&c, &d, &t);
            wr_loc(&c, &d, T, unop(&c, in->op, T, a));
            break;
        }
        case OP_INC:
        case OP_DEC: {
            Loc d;
            loc_of(&c, &ops[0], type_size(T), &d);
            MlwsType t = T;
            Word a = rd_loc(&c, &d, &t), one;
            if (type_is_float(T)) one.f = 1.0;
            else one.i = 1;
            wr_loc(&c, &d, T, binop(&c, in->op == OP_INC ? OP_ADD : OP_SUB, T, a, one));
            break;
        }
        case OP_SHL: case OP_SHR: case OP_SAR: case OP_ROL: case OP_ROR: {
            Loc d;
            loc_of(&c, &ops[0], type_size(T), &d);
            MlwsType t = T;
            Word a = in->nops == 3 ? rd(&c, &ops[1], T) : rd_loc(&c, &d, &t);
            Word n = rd(&c, &ops[in->nops - 1], T);
            wr_loc(&c, &d, T, shiftop(in->op, T, a, n));
            break;
        }
        case OP_FMA: {
            Loc d;
            loc_of(&c, &ops[0], type_size(T), &d);
            Word a = rd(&c, &ops[1], T), b = rd(&c, &ops[2], T), z = rd(&c, &ops[3], T), r;
            r.f = canon_float(T, fma(a.f, b.f, z.f));
            wr_loc(&c, &d, T, r);
            break;
        }

        case OP_SEQ: case OP_SNE: case OP_SLT: case OP_SLE: case OP_SGT: case OP_SGE: {
            Loc d;
            loc_of(&c, &ops[0], 1, &d);
            Word r;
            if (TN != T) { /* compare numbers: as integers -1/0/1 */
                Word x, y;
                x.i = num_cmp(rd_num(&c, &ops[1], TN), rd_num(&c, &ops[2], TN));
                y.i = 0;
                r.i = compare(in->op, T_I64, x, y);
            } else {
                Word a = rd(&c, &ops[1], T), b = rd(&c, &ops[2], T);
                r.i = compare(in->op, T, a, b);
            }
            wr_loc(&c, &d, T_B, r);
            break;
        }
        case OP_SEL: {
            Loc d;
            loc_of(&c, &ops[0], type_size(T), &d);
            int64_t cond = rd(&c, &ops[1], T_I64).i;
            wr_loc(&c, &d, T, rd(&c, &ops[cond ? 2 : 3], T));
            break;
        }

        case OP_JMP:
            fr->pc = ops[0].rv.target;
            break;
        case OP_BZ:
        case OP_BNZ: {
            bool z = is_zero(T, rd(&c, &ops[0], T));
            if (z == (in->op == OP_BZ)) fr->pc = ops[1].rv.target;
            break;
        }
        case OP_BEQ: case OP_BNE: case OP_BLT: case OP_BLE: case OP_BGT: case OP_BGE: {
            bool taken;
            if (TN != T) {
                Word x, y;
                x.i = num_cmp(rd_num(&c, &ops[0], TN), rd_num(&c, &ops[1], TN));
                y.i = 0;
                taken = compare(in->op, T_I64, x, y);
            } else {
                Word a = rd(&c, &ops[0], T), b = rd(&c, &ops[1], T);
                taken = compare(in->op, T, a, b);
            }
            if (taken) fr->pc = ops[2].rv.target;
            break;
        }
        case OP_JTAB: {
            int64_t idx = rd(&c, &ops[0], T_I64).i;
            if (idx >= 0 && idx < in->nops - 1) fr->pc = ops[1 + idx].rv.target;
            break;
        }

        case OP_CALL:
        case OP_TCALL: {
            Function *fn = ops[0].rv.ptr;
            int argc = (int)ops[1].imm;
            Word args[MAX_OPERANDS];
            const MFunc *mf = fn->mf;
            for (int k = 0; k < argc; k++) args[k] = rd(&c, &ops[2 + k], (MlwsType)mf->params[k]);
            const Insn *ret_insn = in;
            if (in->op == OP_TCALL) {
                ret_insn = fr->ret_insn;
                proc_pop_frame(p);
            }
            proc_push_frame(p, fn, ret_insn);
            ctx_load(&c);
            bind_args(&c, NULL, mf->params, argc, args);
            break;
        }
        case OP_RET: {
            Word vals[MAX_OPERANDS];
            MlwsType vt[MAX_OPERANDS];
            const MFunc *mf = fr->fn->mf;
            bool decl = (mf->flags & MF_RETS_DECLARED) != 0;
            for (int k = 0; k < in->nops; k++) {
                vt[k] = decl ? (MlwsType)mf->rets[k] : T_NONE;
                vals[k] = rdn(&c, &ops[k], &vt[k]);
            }
            const Insn *ri = fr->ret_insn;
            const char *callee = fr->fn->name;
            proc_pop_frame(p);
            if (p->nframes == 0) {
                p->exit_code = in->nops > 0 && type_is_int(vt[0]) ? vals[0].i : 0;
                return SLICE_DONE;
            }
            ctx_load(&c);
            if (ri) {
                int argc = (int)ri->ops[1].imm;
                int nres = ri->nops - 2 - argc;
                if (nres > in->nops)
                    vm_trap(p, "E_RET_COUNT", "%s returned %d value(s) but the caller expects %d", callee, in->nops, nres);
                for (int k = 0; k < nres; k++) wr(&c, &ri->ops[2 + argc + k], vt[k], vals[k]);
            }
            break;
        }
        case OP_NCALL: {
            const NativeDef *nd = ops[0].rv.ptr;
            int argc = (int)ops[1].imm;
            Word args[NATIVE_MAX_ARGS], rets[NATIVE_MAX_RETS];
            memset(rets, 0, sizeof rets);
            if (!nd->flags) { /* built in: the fast path */
                for (int k = 0; k < argc; k++) args[k] = rd(&c, &ops[2 + k], (MlwsType)nd->pt[k]);
                nd->fn(p, args, rets);
            } else if (p->bcall.state == BCALL_DONE && p->bcall.insn == in) { /* back from the I/O pool */
                memcpy(rets, p->bcall.rets, sizeof rets);
                p->bcall.state = BCALL_NONE;
                p->resumed = false;
                p->fdwait.deadline = 0;
                native_complete(p);
            } else {
                for (int k = 0; k < argc; k++) args[k] = rd(&c, &ops[2 + k], (MlwsType)nd->pt[k]);
                bool to_pool = (nd->flags & NF_BLOCKING) != 0;
                if (to_pool) {
                    native_count_call(p, nd);
                } else {
                    native_call(p, nd, args, rets);
                    int req = p->native_req;
                    p->native_req = NREQ_NONE;
                    if (req == NREQ_WAIT_FD) { /* wait for the descriptor; then this instruction runs again */
                        p->resumed = true;
                        fr->pc--;
                        p->insns--;
                        return SLICE_IOWAIT;
                    }
                    if (req == NREQ_WAIT_REPLY) { /* proc.call: wait like recv; then this instruction runs again */
                        p->resumed = true;
                        fr->pc--;
                        p->insns--;
                        return SLICE_BLOCK;
                    }
                    if (req == NREQ_EXIT) { /* os.exit: the program ends (sched_request_exit) */
                        p->exit_code = vm->exit_code;
                        return SLICE_DONE;
                    }
                    to_pool = req == NREQ_BLOCKING;
                    if (to_pool) p->resumed = true;
                    else p->resumed = false, p->fdwait.deadline = 0;
                }
                if (to_pool) { /* run it on the I/O thread pool; this instruction runs again after it */
                    p->bcall.state = BCALL_PENDING;
                    p->bcall.nd = nd;
                    p->bcall.insn = in;
                    memcpy(p->bcall.args, args, sizeof args);
                    memset(p->bcall.rets, 0, sizeof p->bcall.rets);
                    fr->pc--;
                    p->insns--;
                    return SLICE_NATIVE;
                }
            }
            for (int k = 0; k < in->nops - 2 - argc; k++) wr(&c, &ops[2 + argc + k], (MlwsType)nd->rt[k], rets[k]);
            break;
        }

        case OP_ALLOC: {
            Loc d;
            loc_of(&c, &ops[0], 8, &d);
            Word w;
            w.p = proc_alloc(p, rd(&c, &ops[1], T_I64).i);
            wr_loc(&c, &d, T_P, w);
            break;
        }
        case OP_FREE:
            proc_free_block(p, rd(&c, &ops[0], T_P).p);
            break;
        case OP_REALLOC: {
            Loc d;
            loc_of(&c, &ops[0], 8, &d);
            uint64_t old = rd(&c, &ops[1], T_P).p;
            Word w;
            w.p = proc_realloc(p, old, rd(&c, &ops[2], T_I64).i);
            wr_loc(&c, &d, T_P, w);
            break;
        }
        case OP_MEMCPY:
        case OP_MEMSET: {
            uint64_t dst = rd(&c, &ops[0], T_P).p;
            Word second = rd(&c, &ops[1], in->op == OP_MEMCPY ? T_P : T_I64);
            int64_t n = rd(&c, &ops[2], T_I64).i;
            if (n < 0) vm_trap(p, "E_BOUNDS", "negative length %lld", (long long)n);
            if (n == 0) break;
            if (n > (int64_t)UINT32_MAX) vm_trap(p, "E_BOUNDS", "length %lld too large", (long long)n);
            Loc dl = mem_loc(p, dst);
            uint8_t *dm = mem_ptr(&c, &dl, (int)0);
            if ((uint64_t)dl.off + (uint64_t)n > dl.seg->size)
                vm_trap(p, "E_BOUNDS", "%s of %lld bytes overruns the destination", op_info[in->op].mnem, (long long)n);
            if (in->op == OP_MEMCPY) {
                Loc sl = mem_loc(p, second.p);
                const uint8_t *sm = mem_ptr(&c, &sl, 0);
                if ((uint64_t)sl.off + (uint64_t)n > sl.seg->size)
                    vm_trap(p, "E_BOUNDS", "memcpy of %lld bytes overruns the source", (long long)n);
                if (sl.seg->tags)
                    for (uint64_t w = sl.off / 8; w <= (sl.off + (uint64_t)n - 1) / 8; w++)
                        if (sl.seg->tags[w] == T_R) vm_trap(p, "E_TYPE", "memcpy cannot copy a reference slot");
                memmove(dm, sm, (size_t)n);
            } else {
                memset(dm, (int)(uint8_t)second.i, (size_t)n);
            }
            mark_raw(dl.seg, dl.off, (uint64_t)n);
            break;
        }
        /* ---- objects ---- */
        case OP_NEW:
            wr_ref(&c, &ops[0], struct_new(p, ops[1].rv.ptr));
            break;
        case OP_ANEW:
            wr_ref(&c, &ops[0], array_new(p, T, rd(&c, &ops[1], T_I64).i));
            break;
        case OP_LEN: {
            Obj *ob = rd_obj(&c, &ops[1], 0, "len");
            int64_t n;
            switch (ob->kind) {
            case OBJ_STRING: n = ((ObjString *)ob)->len; break;
            case OBJ_BYTES: n = ((ObjBytes *)ob)->len; break;
            case OBJ_ARRAY: n = ((ObjArray *)ob)->len; break;
            case OBJ_LIST: n = ((ObjList *)ob)->len; break;
            case OBJ_MAP: n = ((ObjMap *)ob)->count; break;
            default: vm_trap(p, "E_TYPE", "len of a %s", obj_desc(ob));
            }
            wr_int(&c, &ops[0], T_I64, n);
            break;
        }
        case OP_TYPEOF: {
            Obj *ob = rd(&c, &ops[1], T_R).r;
            wr_int(&c, &ops[0], T_I64, ob ? ob->kind : OBJ_NULL);
            break;
        }
        case OP_ISA: {
            Obj *ob = rd(&c, &ops[1], T_R).r;
            bool is = ob && ob->kind == OBJ_STRUCT && ((ObjStruct *)ob)->def == ops[2].rv.ptr;
            wr_int(&c, &ops[0], T_B, is);
            break;
        }
        case OP_RESIZE: {
            Obj *ob = rd_obj(&c, &ops[0], 0, "resize");
            int64_t n = rd(&c, &ops[1], T_I64).i;
            if (ob->kind == OBJ_BYTES) bytes_resize(p, (ObjBytes *)ob, n);
            else if (ob->kind == OBJ_LIST) list_resize(p, (ObjList *)ob, n);
            else vm_trap(p, "E_TYPE", "resize needs a byte buffer or list, got %s", obj_desc(ob));
            break;
        }

        /* ---- strings and bytes ---- */
        case OP_STRCAT: {
            /* Read every part before allocating, so a trap leaks nothing. */
            const uint8_t *part[MAX_OPERANDS];
            uint32_t plen[MAX_OPERANDS];
            uint64_t total = 0;
            for (int k = 1; k < in->nops; k++) {
                part[k] = rd_text(&c, &ops[k], "strcat", &plen[k]);
                total += plen[k];
            }
            if (total >= (uint64_t)1 << 31) vm_trap(p, "E_BAD_ALLOC", "strcat result too long");
            char *buf = xmalloc((size_t)total + 1);
            size_t at = 0;
            for (int k = 1; k < in->nops; k++) {
                memcpy(buf + at, part[k], plen[k]);
                at += plen[k];
            }
            ObjString *s = str_new(p, buf, (size_t)total);
            free(buf);
            wr_ref(&c, &ops[0], s);
            break;
        }
        case OP_SUBSTR: {
            uint32_t n;
            const uint8_t *d = rd_text(&c, &ops[1], "substr", &n);
            int64_t start = rd(&c, &ops[2], T_I64).i, cnt = rd(&c, &ops[3], T_I64).i;
            if (start < 0 || cnt < 0 || start + cnt > (int64_t)n)
                vm_trap(p, "E_BOUNDS", "substr(%lld, %lld) out of range for length %u", (long long)start, (long long)cnt, n);
            wr_ref(&c, &ops[0], str_new(p, (const char *)d + start, (size_t)cnt));
            break;
        }
        case OP_STRCMP: {
            uint32_t na, nb;
            const uint8_t *a = rd_text(&c, &ops[1], "strcmp", &na);
            const uint8_t *b = rd_text(&c, &ops[2], "strcmp", &nb);
            int r = memcmp(a, b, na < nb ? na : nb);
            if (r == 0) r = na < nb ? -1 : na > nb;
            wr_int(&c, &ops[0], T_I64, r < 0 ? -1 : r > 0);
            break;
        }
        case OP_STRFIND: {
            uint32_t nh, nn;
            const uint8_t *h = rd_text(&c, &ops[1], "strfind", &nh);
            const uint8_t *nd = rd_text(&c, &ops[2], "strfind", &nn);
            int64_t start = in->nops > 3 ? rd(&c, &ops[3], T_I64).i : 0, found = -1;
            if (start < 0) start = 0;
            for (int64_t i = start; i + nn <= nh; i++) {
                if (memcmp(h + i, nd, nn) == 0) {
                    found = i;
                    break;
                }
            }
            wr_int(&c, &ops[0], T_I64, found);
            break;
        }
        case OP_TOSTR: {
            MlwsType t = T;
            Word v = rdn(&c, &ops[1], &t);
            wr_ref(&c, &ops[0], value_to_string(p, v, t));
            break;
        }
        case OP_PARSE: {
            uint32_t n;
            const uint8_t *d = rd_text(&c, &ops[1], "parse", &n);
            Word v;
            if (TN != T) {
                v.r = num_parse(p, TN, (const char *)d, n);
                if (!v.r)
                    vm_trap(p, "E_PARSE", "cannot parse \"%.*s\" as %s", (int)(n > 60 ? 60 : n), (const char *)d, type_name(TN));
                wr(&c, &ops[0], T_R, v);
                break;
            }
            if (parse_number(&c, T, d, n, &v) < 0)
                vm_trap(p, "E_PARSE", "cannot parse \"%.*s\" as %s", (int)(n > 60 ? 60 : n), (const char *)d, type_name(T));
            wr(&c, &ops[0], T, v);
            break;
        }
        case OP_BUFNEW:
            wr_ref(&c, &ops[0], bytes_new(p, rd(&c, &ops[1], T_I64).i));
            break;
        case OP_BUFCAT: {
            ObjBytes *b = (ObjBytes *)rd_obj(&c, &ops[0], OBJ_BYTES, "bufcat");
            for (int k = 1; k < in->nops; k++) {
                uint32_t n;
                const uint8_t *d = rd_text(&c, &ops[k], "bufcat", &n);
                bytes_append(p, b, d, n);
            }
            break;
        }
        case OP_BUFPUSH: {
            ObjBytes *b = (ObjBytes *)rd_obj(&c, &ops[0], OBJ_BYTES, "bufpush");
            for (int k = 1; k < in->nops; k++) {
                uint8_t byte = (uint8_t)rd(&c, &ops[k], T_I64).i;
                bytes_append(p, b, &byte, 1);
            }
            break;
        }

        /* ---- lists and maps ---- */
        case OP_LNEW:
            wr_ref(&c, &ops[0], list_new(p, in->nops > 1 ? rd(&c, &ops[1], T_I64).i : 0));
            break;
        case OP_LPUSH: {
            ObjList *l = (ObjList *)rd_obj(&c, &ops[0], OBJ_LIST, "lpush");
            for (int k = 1; k < in->nops; k++) {
                MlwsType t = T;
                Word v = rdn(&c, &ops[k], &t);
                list_push(p, l, v, (uint8_t)t);
            }
            break;
        }
        case OP_LPOP: {
            ObjList *l = (ObjList *)rd_obj(&c, &ops[1], OBJ_LIST, "lpop");
            if (l->len == 0) vm_trap(p, "E_BOUNDS", "lpop of an empty list");
            MlwsType t = T;
            if (t == T_NONE && ops[0].mode == OM_REG) t = reg_natural_type(ops[0].reg);
            Loc L;
            memset(&L, 0, sizeof L);
            L.is_reg = LK_SLOT;
            L.slot = &l->items[l->len - 1];
            L.tag = &l->tags[l->len - 1];
            Word v = slot_read(&c, &L, &t);
            list_resize(p, l, l->len - 1);
            wr(&c, &ops[0], t, v);
            break;
        }
        case OP_LINS: {
            ObjList *l = (ObjList *)rd_obj(&c, &ops[0], OBJ_LIST, "lins");
            int64_t at = rd(&c, &ops[1], T_I64).i;
            MlwsType t = T;
            Word v = rdn(&c, &ops[2], &t);
            list_insert(p, l, at, v, (uint8_t)t);
            break;
        }
        case OP_LDEL: {
            ObjList *l = (ObjList *)rd_obj(&c, &ops[0], OBJ_LIST, "ldel");
            list_delete(p, l, rd(&c, &ops[1], T_I64).i);
            break;
        }
        case OP_MNEW:
            wr_ref(&c, &ops[0], map_new(p));
            break;
        case OP_MSET: {
            ObjMap *m = (ObjMap *)rd_obj(&c, &ops[0], OBJ_MAP, "mset");
            MlwsType kt = T_NONE, vt = T;
            Word k = rdn(&c, &ops[1], &kt);
            Word v = rdn(&c, &ops[2], &vt);
            map_set(p, m, k, (uint8_t)kt, v, (uint8_t)vt);
            break;
        }
        case OP_MGET: {
            ObjMap *m = (ObjMap *)rd_obj(&c, &ops[1], OBJ_MAP, "mget");
            MlwsType kt = T_NONE;
            Word k = rdn(&c, &ops[2], &kt);
            MapEntry *e = map_find(m, k, (uint8_t)kt);
            MlwsType t = T;
            if (t == T_NONE && ops[0].mode == OM_REG) t = reg_natural_type(ops[0].reg);
            Word v;
            if (e) {
                Loc L;
                memset(&L, 0, sizeof L);
                L.is_reg = LK_SLOT;
                L.slot = &e->val;
                L.tag = &e->vtag;
                v = slot_read(&c, &L, &t);
            } else if (in->nops > 3) {
                v = rdn(&c, &ops[3], &t);
            } else {
                char kb[96];
                describe_key(k, kt, kb, sizeof kb);
                vm_trap(p, "E_KEY", "key %s not found in map", kb);
            }
            wr(&c, &ops[0], t, v);
            break;
        }
        case OP_MHAS: {
            ObjMap *m = (ObjMap *)rd_obj(&c, &ops[1], OBJ_MAP, "mhas");
            MlwsType kt = T_NONE;
            Word k = rdn(&c, &ops[2], &kt);
            wr_int(&c, &ops[0], T_B, map_find(m, k, (uint8_t)kt) != NULL);
            break;
        }
        case OP_MDEL: {
            ObjMap *m = (ObjMap *)rd_obj(&c, &ops[0], OBJ_MAP, "mdel");
            MlwsType kt = T_NONE;
            Word k = rdn(&c, &ops[1], &kt);
            map_delete(m, k, (uint8_t)kt);
            break;
        }
        case OP_MKEYS:
            wr_ref(&c, &ops[0], map_keys(p, (ObjMap *)rd_obj(&c, &ops[1], OBJ_MAP, "mkeys")));
            break;

        /* ---- handles ---- */
        case OP_HNEW:
            wr_int(&c, &ops[0], T_I64, handle_new(p, rd(&c, &ops[1], T_R).r));
            break;
        case OP_HGET:
            wr_ref(&c, &ops[0], handle_get(p, rd(&c, &ops[1], T_I64).i));
            break;
        case OP_HFREE:
            handle_free(p, rd(&c, &ops[0], T_I64).i);
            break;

        /* ---- closures ---- */
        case OP_CLOSURE: {
            Function *fn = ops[0].rv.ptr;
            int nup = (int)ops[1].imm;
            Word vals[MAX_OPERANDS];
            uint8_t tags[MAX_OPERANDS];
            for (int k = 0; k < nup; k++) {
                MlwsType t = T_NONE;
                vals[k] = rdn(&c, &ops[2 + k], &t);
                tags[k] = (uint8_t)t;
            }
            ObjClosure *cl = closure_new(p, fn, (uint32_t)nup);
            for (int k = 0; k < nup; k++) {
                cl->up[k] = vals[k];
                cl->tags[k] = tags[k];
            }
            wr_ref(&c, &ops[2 + nup], cl);
            break;
        }
        case OP_CALLI:
        case OP_TCALLI: {
            ObjClosure *cl = rd_closure(&c, &ops[0]);
            Function *fn = cl->fn;
            const MFunc *mf = fn->mf;
            int argc = (int)ops[1].imm;
            if (argc != mf->nparams)
                vm_trap(p, "E_TYPE", "closure %s takes %d argument(s), got %d", fn->name, mf->nparams, argc);
            Word args[MAX_OPERANDS];
            for (int k = 0; k < argc; k++) args[k] = rd_param(&c, &ops[2 + k], (MlwsType)mf->params[k]);
            const Insn *ret_insn = in;
            if (in->op == OP_TCALLI) {
                ret_insn = fr->ret_insn;
                proc_pop_frame(p);
            }
            proc_push_frame(p, fn, ret_insn);
            p->frames[p->nframes - 1].closure = &cl->h;
            ctx_load(&c);
            bind_args(&c, NULL, mf->params, argc, args);
            break;
        }
        case OP_SPAWNI:
        case OP_SPAWNIL:
        case OP_SPAWNIM: {
            ObjClosure *cl = rd_closure(&c, &ops[0]);
            const MFunc *mf = cl->fn->mf;
            int argc = (int)ops[1].imm;
            if (argc != mf->nparams)
                vm_trap(p, "E_TYPE", "closure %s takes %d argument(s), got %d", cl->fn->name, mf->nparams, argc);
            Word args[MAX_OPERANDS];
            for (int k = 0; k < argc; k++) {
                if (mf->params[k] == T_P) vm_trap(p, "E_TYPE", "pointer parameters cannot cross processes");
                args[k] = rd_param(&c, &ops[2 + k], (MlwsType)mf->params[k]);
            }
            Process *child = proc_new(vm);
            memcpy(child->quota, p->quota, sizeof child->quota);
            p->pending_spawn = child;
            Word clw = msg_copy(p, child, (Word){.r = cl}, T_R);
            for (int k = 0; k < argc; k++) args[k] = msg_copy(p, child, args[k], mf->params[k]);
            p->pending_spawn = NULL;
            spawn_watch(p, child, in->op == OP_SPAWNIL, in->op == OP_SPAWNIM);
            proc_start(child, cl->fn, clw.r, args, argc);
            if (in->nops > 2 + argc) wr_int(&c, &ops[2 + argc], T_I64, (int64_t)child->pid);
            break;
        }

        /* ---- exceptions ---- */
        case OP_THROW: {
            Obj *ex = rd(&c, &ops[0], T_R).r;
            if (!ex) vm_trap(p, "E_NULL", "throw of null");
            proc_throw(p, ex);
        }
        case OP_CATCH:
            wr_ref(&c, &ops[0], p->exception);
            break;

        /* ---- processes ---- */
        case OP_SPAWN:
        case OP_SPAWNL:
        case OP_SPAWNM: {
            Function *fn = ops[0].rv.ptr;
            int argc = (int)ops[1].imm;
            Word args[MAX_OPERANDS];
            for (int k = 0; k < argc; k++) args[k] = rd(&c, &ops[2 + k], (MlwsType)fn->mf->params[k]);
            Process *child = proc_new(vm);
            memcpy(child->quota, p->quota, sizeof child->quota);
            p->pending_spawn = child; /* freed by proc_run_slice if a copy traps */
            for (int k = 0; k < argc; k++) args[k] = msg_copy(p, child, args[k], fn->mf->params[k]);
            p->pending_spawn = NULL;
            spawn_watch(p, child, in->op == OP_SPAWNL, in->op == OP_SPAWNM);
            proc_start(child, fn, NULL, args, argc);
            if (in->nops > 2 + argc) wr_int(&c, &ops[2 + argc], T_I64, (int64_t)child->pid);
            break;
        }
        case OP_SEND: {
            int64_t pid = rd(&c, &ops[0], T_I64).i;
            MlwsType t = T;
            Word v = rdn(&c, &ops[1], &t);
            if (t == T_P) vm_trap(p, "E_TYPE", "pointers cannot be sent to another process (they are process-local)");
            p->msgs_sent++;
            sched_send(p, (uint64_t)pid, v, (uint8_t)t); /* to a finished or unknown process: dropped */
            break;
        }
        case OP_RECV:
        case OP_RECVT: {
            if (p->mbox_head == p->mbox.n) sched_take_inbox(p);
            if (p->mbox_head < p->mbox.n) {
                MlwsType t = T;
                if (t == T_NONE && ops[0].mode == OM_REG) t = reg_natural_type(ops[0].reg);
                Loc L = mbox_loc(&p->mbox.v[p->mbox_head]);
                Word v = slot_read(&c, &L, &t); /* a type mismatch traps and leaves the message */
                mbox_take(p);
                p->deadline_set = false;
                wr(&c, &ops[0], t, v);
                break;
            }
            if (in->op == OP_RECVT) {
                uint64_t now = plat_now_ns();
                if (p->deadline_set) {
                    if (now >= p->wake_at) { /* timed out */
                        p->deadline_set = false;
                        fr->pc = ops[2].rv.target;
                        break;
                    }
                } else {
                    int64_t ms = rd(&c, &ops[1], T_I64).i;
                    if (ms <= 0) { /* poll */
                        fr->pc = ops[2].rv.target;
                        break;
                    }
                    p->deadline_set = true;
                    p->wake_at = now + (uint64_t)ms * 1000000u;
                }
            } else if (p->deadlock) {
                p->deadlock = false;
                vm_trap(p, "E_DEADLOCK", "every process is waiting: no message can ever arrive");
            }
            fr->pc--; /* re-execute this receive when woken */
            p->insns--;
            return SLICE_BLOCK;
        }
        case OP_YIELD:
            return SLICE_YIELD;
        case OP_SLEEP: {
            int64_t ms = rd(&c, &ops[0], T_I64).i;
            if (ms <= 0) return SLICE_YIELD;
            p->wake_at = plat_now_ns() + (uint64_t)ms * 1000000u;
            return SLICE_SLEEP;
        }
        case OP_KILL: {
            int64_t pid = rd(&c, &ops[0], T_I64).i;
            char msg[96];
            snprintf(msg, sizeof msg, "killed by process %llu", (unsigned long long)p->pid);
            sched_kill(p, (uint64_t)pid, "E_KILLED", msg);
            break; /* killing ourselves (or a linked chain back to us) stops at the next instruction */
        }
        case OP_LINK: {
            sched_link(p, (uint64_t)rd(&c, &ops[0], T_I64).i);
            break;
        }
        case OP_MONITOR: {
            sched_monitor(p, (uint64_t)rd(&c, &ops[0], T_I64).i);
            break;
        }
        case OP_ALIVE:
            wr_int(&c, &ops[0], T_B, sched_alive(vm, (uint64_t)rd(&c, &ops[1], T_I64).i));
            break;
        case OP_QUOTA: {
            int64_t kind = ops[0].imm, limit = rd(&c, &ops[1], T_I64).i;
            if (kind >= QUOTA__COUNT) vm_trap(p, "E_QUOTA", "unknown quota kind %lld", (long long)kind);
            if (limit < 0) vm_trap(p, "E_QUOTA", "a quota cannot be negative");
            sched_set_quota(p, (int)kind, (uint64_t)limit); /* only ever lowered */
            if (kind == QUOTA_HEAP) p->next_gc = 0, p->gc_pending = true;   /* re-derive the GC threshold */
            break;
        }
        case OP_MLEN:
            wr_int(&c, &ops[0], T_I64, (int64_t)p->mcount);
            break;

        case OP__COUNT:
        default:
            vm_trap(p, "E_INTERNAL", "unimplemented opcode %s", op_info[in->op].mnem);
        }
    }
}
