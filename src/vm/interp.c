/* interp.c - the instruction interpreter.
 *
 * Code is pre-decoded by the loader into Insn arrays with resolved operands, so
 * the loop below only evaluates addressing modes and dispatches on the dense
 * opcode. Every fault traps (longjmp) out through vm_trap with a stable error
 * code; the verifier has already checked everything static. */
#include <math.h>

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

/* A resolved operand location: a register or a byte offset in a segment. */
typedef struct {
    uint8_t is_reg;
    uint8_t reg;
    Segment *seg;
    uint32_t off;
} Loc;

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
        Loc L = {0, 0, proc_segment(c->p, ia), PTR_OFF(ia)};
        return ptr_add(c, mem_read(c, &L, T_P).p, o->bdisp);
    }
    default: vm_trap(c->p, "E_INTERNAL", "operand is not a memory location");
    }
}

static void loc_of(Ctx *c, const Operand *o, int width, Loc *L) {
    if (o->mode == OM_REG) {
        L->is_reg = 1;
        L->reg = o->reg;
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
        if (!L->seg->tags || L->off % 8 || L->seg->tags[L->off / 8] != T_R)
            vm_trap(c->p, "E_TYPE", "memory at offset %u does not hold a reference", L->off);
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

static Word rd_loc(Ctx *c, const Loc *L, MlwsType *t) {
    if (L->is_reg) {
        if (*t == T_NONE) *t = reg_natural_type(L->reg);
        return reg_read(c, L->reg, *t);
    }
    if (*t == T_NONE) *t = mem_natural(L);
    return mem_read(c, L, *t);
}

static void wr_loc(Ctx *c, const Loc *L, MlwsType t, Word w) {
    if (L->is_reg) reg_write(c, L->reg, t, w);
    else mem_write(c, L, t, w);
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
    mem_write(c, &L, t, w);
}

/* ---- data stack --------------------------------------------------------------- */

static void push_word(Ctx *c, MlwsType t, Word v) {
    Process *p = c->p;
    uint64_t sp = ptr_add(c, p->sp, -8);
    Loc L = {0, 0, proc_segment(p, sp), PTR_OFF(sp)};
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

void interp_run(Process *p) {
    Ctx c;
    c.p = p;
    ctx_load(&c);
    for (;;) {
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
            return;
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
                Loc top = {0, 0, proc_segment(p, p->sp), PTR_OFF(p->sp)};
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
            Loc L = {0, 0, proc_segment(p, sp), PTR_OFF(sp)};
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
                return;
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
            Loc dl = {0, 0, proc_segment(p, dst), PTR_OFF(dst)};
            uint8_t *dm = mem_ptr(&c, &dl, (int)0);
            if ((uint64_t)dl.off + (uint64_t)n > dl.seg->size)
                vm_trap(p, "E_BOUNDS", "%s of %lld bytes overruns the destination", op_info[in->op].mnem, (long long)n);
            if (in->op == OP_MEMCPY) {
                Loc sl = {0, 0, proc_segment(p, second.p), PTR_OFF(second.p)};
                const uint8_t *sm = mem_ptr(&c, &sl, 0);
                if ((uint64_t)sl.off + (uint64_t)n > sl.seg->size)
                    vm_trap(p, "E_BOUNDS", "memcpy of %lld bytes overruns the source", (long long)n);
                memmove(dm, sm, (size_t)n);
            } else {
                memset(dm, (int)(uint8_t)second.i, (size_t)n);
            }
            mark_raw(dl.seg, dl.off, (uint64_t)n);
            break;
        }
        case OP__COUNT:
        default:
            vm_trap(p, "E_INTERNAL", "unimplemented opcode %s", op_info[in->op].mnem);
        }
    }
}
