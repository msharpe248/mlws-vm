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
    L.seg = proc_segment(p, ptr);
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
    Obj *ob = obj_reg(c, o->reg);
    L->tag = NULL;
    L->fixed = T_NONE;
    L->ro = 0;
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
    L->seg = proc_segment(c->p, a);
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
static bool same_class(MlwsType a, MlwsType b) { return type_bank(a) == type_bank(b) && (a == T_R) == (b == T_R); }

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
    if (p->mbox_head == p->mbox.n) {
        p->mbox_head = p->mbox.n = 0;
    } else if (p->mbox_head > 64 && p->mbox_head * 2 > p->mbox.n) {
        memmove(p->mbox.v, p->mbox.v + p->mbox_head, (p->mbox.n - p->mbox_head) * sizeof(Message));
        p->mbox.n -= p->mbox_head;
        p->mbox_head = 0;
    }
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
        const Insn *in = &fr->fn->code[fr->pc++];
        const Operand *ops = in->ops;
        MlwsType T = (MlwsType)in->type;
        p->insns++;
        switch ((Opcode)in->op) {
        case OP_NOP:
        case OP_BREAK:
            break;
        case OP_HALT:
            p->exit_code = in->nops ? rd(&c, &ops[0], T_I64).i : 0;
            return SLICE_DONE;
        case OP_ASSERT:
            if (rd(&c, &ops[0], T_I64).i == 0)
                vm_trap(p, "E_ASSERT", "%s", in->nops > 1 ? ((ObjString *)ops[1].rv.ptr)->data : "assertion failed");
            break;

        case OP_MOV: {
            Loc d;
            loc_of(&c, &ops[0], T ? type_size(T) : 8, &d);
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
            Loc d;
            loc_of(&c, &ops[0], type_size(T), &d);
            MlwsType from = (MlwsType)in->type2;
            Word v = rd(&c, &ops[in->nops - 1], from);
            wr_loc(&c, &d, T, in->op == OP_CVT ? convert(T, from, v) : bitcast(T, from, v));
            break;
        }

        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_REM: case OP_MIN: case OP_MAX:
        case OP_AND: case OP_OR: case OP_XOR:
            arith(&c, in);
            break;
        case OP_NEG: case OP_ABS: case OP_NOT: case OP_POPCNT: case OP_CLZ: case OP_CTZ: case OP_BSWAP:
        case OP_SQRT: case OP_FLOOR: case OP_CEIL: case OP_ROUND: case OP_TRUNC: {
            Loc d;
            loc_of(&c, &ops[0], type_size(T), &d);
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
            Word a = rd(&c, &ops[1], T), b = rd(&c, &ops[2], T), r;
            r.i = compare(in->op, T, a, b);
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
            Word a = rd(&c, &ops[0], T), b = rd(&c, &ops[1], T);
            if (compare(in->op, T, a, b)) fr->pc = ops[2].rv.target;
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
            for (int k = 0; k < argc; k++) args[k] = rd(&c, &ops[2 + k], (MlwsType)nd->pt[k]);
            nd->fn(p, args, rets);
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
        case OP_SPAWN: {
            Function *fn = ops[0].rv.ptr;
            int argc = (int)ops[1].imm;
            Word args[MAX_OPERANDS];
            for (int k = 0; k < argc; k++) args[k] = rd(&c, &ops[2 + k], (MlwsType)fn->mf->params[k]);
            Process *child = proc_new(vm);
            memcpy(child->quota, p->quota, sizeof child->quota);
            vm->pending_spawn = child; /* freed by proc_run_slice if a copy traps */
            for (int k = 0; k < argc; k++) args[k] = msg_copy(p, child, args[k], fn->mf->params[k]);
            vm->pending_spawn = NULL;
            proc_start(child, fn, args, argc);
            if (in->nops > 2 + argc) wr_int(&c, &ops[2 + argc], T_I64, (int64_t)child->pid);
            break;
        }
        case OP_SEND: {
            int64_t pid = rd(&c, &ops[0], T_I64).i;
            MlwsType t = T;
            Word v = rdn(&c, &ops[1], &t);
            if (t == T_P) vm_trap(p, "E_TYPE", "pointers cannot be sent to another process (they are process-local)");
            Process *to = vm_process(vm, (uint64_t)pid);
            p->msgs_sent++;
            if (!to) break; /* finished or unknown: dropped */
            if (to->mbox.n - to->mbox_head >= to->quota[QUOTA_MAILBOX])
                vm_trap(p, "E_QUOTA", "mailbox of process %lld is full", (long long)pid);
            Message m;
            m.v = msg_copy(p, to, v, (uint8_t)t);
            m.tag = (uint8_t)t;
            VEC_PUSH(to->mbox, m);
            if (to->state == PROC_WAITING) sched_ready(vm, to);
            break;
        }
        case OP_RECV:
        case OP_RECVT: {
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
            Process *target = vm_process(vm, (uint64_t)pid);
            char msg[96];
            snprintf(msg, sizeof msg, "killed by process %llu", (unsigned long long)p->pid);
            sched_kill(vm, target, "E_KILLED", msg);
            break; /* killing ourselves (or a linked chain back to us) stops at the next instruction */
        }
        case OP_LINK: {
            int64_t pid = rd(&c, &ops[0], T_I64).i;
            Process *target = vm_process(vm, (uint64_t)pid);
            if (!target) vm_trap(p, "E_NOPROC", "link to process %lld, which does not exist or has finished", (long long)pid);
            if (target == p) break;
            bool have = false;
            for (size_t i = 0; i < p->links.n; i++) have = have || p->links.v[i] == target->pid;
            if (!have) {
                VEC_PUSH(p->links, target->pid);
                VEC_PUSH(target->links, p->pid);
            }
            break;
        }
        case OP_MONITOR: {
            int64_t pid = rd(&c, &ops[0], T_I64).i;
            Process *target = vm_process(vm, (uint64_t)pid);
            if (target) VEC_PUSH(target->monitors, p->pid);
            else sched_down_noproc(vm, p, (uint64_t)pid);
            break;
        }
        case OP_ALIVE:
            wr_int(&c, &ops[0], T_B, vm_process(vm, (uint64_t)rd(&c, &ops[1], T_I64).i) != NULL);
            break;
        case OP_QUOTA: {
            int64_t kind = ops[0].imm, limit = rd(&c, &ops[1], T_I64).i;
            if (kind >= QUOTA__COUNT) vm_trap(p, "E_QUOTA", "unknown quota kind %lld", (long long)kind);
            if (limit < 0) vm_trap(p, "E_QUOTA", "a quota cannot be negative");
            if ((uint64_t)limit < p->quota[kind]) p->quota[kind] = (uint64_t)limit; /* only ever lowered */
            if (kind == QUOTA_HEAP) p->next_gc = 0, p->gc_pending = true;   /* re-derive the GC threshold */
            break;
        }
        case OP_MLEN:
            wr_int(&c, &ops[0], T_I64, (int64_t)(p->mbox.n - p->mbox_head));
            break;

        case OP__COUNT:
        default:
            vm_trap(p, "E_INTERNAL", "unimplemented opcode %s", op_info[in->op].mnem);
        }
    }
}
