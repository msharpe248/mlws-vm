/* fold.c - constants: which registers hold a known value at each instruction
 * (constprop), instructions whose operands are all known computed now (fold),
 * and algebraic identities (simplify).
 *
 * The analysis is a forward dataflow over blocks. A register is unknown (not
 * reached yet), a constant, or varying. A function starts with its
 * parameters varying and every other register 0, as a new window reads; an
 * exception handler starts with everything varying. Values are the 64-bit
 * words registers hold (canon_int / canon_float of the type that wrote them),
 * and computing uses the interpreter's own arithmetic (core/arith.h), so a
 * folded value is the one the instruction would have produced. P registers
 * are not tracked (pointer arithmetic can trap), and neither is anything in
 * memory.
 *
 * A branch says something on each of its edges: after `bz R0, L` R0 is null
 * at L and an object on the fall-through; after `beq I0, 5, L`, I0 is 5 at
 * L. An R register can also be known to be not null without a known value:
 * a bnz or bisa taken, or a field or element of it read or written without
 * a trap. These facts decide later branches (a type test repeated on a path
 * that has made it already), and a branch decided this way sends nothing
 * along the edge it never takes. */
#include "opt_internal.h"

#include "../core/arith.h"

enum { CV_TOP = 0, CV_CONST = 1, CV_BOT = 2 };
enum { CK_WORD = 0, CK_POOL = 1, CK_NONNULL = 2 }; /* a number (or null), a constant-pool object, or some object */

typedef struct {
    uint8_t k, kind;
    uint8_t made; /* the value was computed by an instruction of the function (not a new window's 0, a
                     parameter or what a branch said), which a constant in its place may leave dead */
    Word w;       /* CK_POOL: w.u is the constant's index */
} CVal;

#define TRACKED 3 /* banks I, F, R */
typedef struct {
    CVal r[TRACKED][NREGS];
} CState;

static bool is_object(const CVal *v) { return v->k == CV_CONST && (v->kind == CK_POOL || v->kind == CK_NONNULL); }

static CVal *slot(CState *s, uint8_t reg) {
    Bank b = (Bank)REG_BANK(reg);
    return b < TRACKED ? &s->r[b][REG_IDX(reg)] : NULL;
}

static bool cv_same(const CVal *a, const CVal *b) {
    return a->k == b->k && (a->k != CV_CONST || (a->kind == b->kind && a->w.u == b->w.u));
}

/* Meet b into a; true if a changed. */
static bool meet(CState *a, const CState *b) {
    bool ch = false;
    for (int bk = 0; bk < TRACKED; bk++)
        for (int i = 0; i < NREGS; i++) {
            CVal *x = &a->r[bk][i];
            const CVal *y = &b->r[bk][i];
            if (y->k == CV_TOP || x->k == CV_BOT) continue;
            if (x->k == CV_TOP) *x = *y, ch = true;
            else if (y->k == CV_BOT) x->k = CV_BOT, ch = true;
            else if (cv_same(x, y)) {
                if (x->made && !y->made) x->made = 0, ch = true;
                continue;
            }
            else if (is_object(x) && is_object(y)) { /* two objects: still an object */
                if (x->kind != CK_NONNULL) x->kind = CK_NONNULL, ch = true;
            } else x->k = CV_BOT, ch = true;
        }
    return ch;
}

/* The value a source operand reads as type t (T_NONE: its natural type). */
static bool val_of(const CState *s, const Operand *o, MlwsType t, CVal *out) {
    out->k = CV_CONST;
    out->kind = CK_WORD;
    switch (o->mode) {
    case OM_IMM:
        if (t == T_NONE) t = T_I64;
        if (type_is_float(t)) out->w.f = canon_float(t, (double)o->imm);
        else out->w.i = canon_int(t, o->imm);
        return true;
    case OM_FIMM:
        if (!type_is_float(t) && t != T_NONE) return false;
        out->w.f = canon_float(t == T_NONE ? T_F64 : t, o->f);
        return true;
    case OM_CONST:
        out->kind = CK_POOL;
        out->w.u = o->index;
        return true;
    case OM_REG: {
        const CVal *v = slot((CState *)s, o->reg);
        if (!v || v->k != CV_CONST || v->kind == CK_NONNULL) return false;
        *out = *v;
        if (v->kind == CK_WORD) {
            Bank b = (Bank)REG_BANK(o->reg);
            if (b == BANK_I && t != T_NONE && type_is_int(t)) out->w.i = canon_int(t, v->w.i);
            if (b == BANK_F && t == T_F32) out->w.f = (double)(float)v->w.f;
        }
        return true;
    }
    default: return false;
    }
}

/* The word a register holds after an instruction of type t writes w. */
static Word written(uint8_t reg, MlwsType t, Word w) {
    Bank b = (Bank)REG_BANK(reg);
    if (b == BANK_I && type_is_int(t)) w.i = canon_int(t, w.i);
    if (b == BANK_F && type_is_float(t)) w.f = canon_float(t, w.f);
    return w;
}

/* ---- folding ---- */

static bool numeric(MlwsType t) { return type_is_int(t) || type_is_float(t); }

/* What a pure instruction computes when every value it reads is known: the
 * value and the type it writes. */
static bool evaluate(const CState *s, const Insn *in, CVal *res, MlwsType *rt) {
    MlwsType t = (MlwsType)in->type;
    const Operand *ops = in->ops;
    CVal a, b, c;
    res->k = CV_CONST;
    res->kind = CK_WORD;
    *rt = t;
    switch (in->op) {
    case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_REM: case OP_MIN: case OP_MAX:
    case OP_AND: case OP_OR: case OP_XOR: {
        if (!numeric(t)) return false;
        int k = in->nops == 2 ? 1 : 2;
        if (!val_of(s, &ops[k - 1], t, &a) || a.kind != CK_WORD) return false;
        Word acc = a.w;
        for (; k < in->nops; k++) {
            if (!val_of(s, &ops[k], t, &b) || b.kind != CK_WORD) return false;
            if ((in->op == OP_DIV || in->op == OP_REM) && type_is_int(t) && b.w.u == 0) return false; /* the trap stays */
            acc = arith_binop(in->op, t, acc, b.w);
        }
        res->w = acc;
        return true;
    }
    case OP_NEG: case OP_ABS: case OP_NOT: case OP_POPCNT: case OP_CLZ: case OP_CTZ: case OP_BSWAP:
    case OP_SQRT: case OP_FLOOR: case OP_CEIL: case OP_ROUND: case OP_TRUNC:
        if (!numeric(t) || !val_of(s, &ops[in->nops == 2 ? 1 : 0], t, &a)) return false;
        res->w = arith_unop(in->op, t, a.w);
        return true;
    case OP_INC: case OP_DEC: {
        if (!numeric(t) || !val_of(s, &ops[0], t, &a)) return false;
        Word one;
        if (type_is_float(t)) one.f = 1.0;
        else one.i = 1;
        res->w = arith_binop(in->op == OP_INC ? OP_ADD : OP_SUB, t, a.w, one);
        return true;
    }
    case OP_SHL: case OP_SHR: case OP_SAR: case OP_ROL: case OP_ROR:
        if (!type_is_int(t) || !val_of(s, &ops[in->nops == 3 ? 1 : 0], t, &a) ||
            !val_of(s, &ops[in->nops - 1], t, &b))
            return false;
        res->w = arith_shift(in->op, t, a.w, b.w);
        return true;
    case OP_FMA:
        if (!type_is_float(t) || !val_of(s, &ops[1], t, &a) || !val_of(s, &ops[2], t, &b) || !val_of(s, &ops[3], t, &c))
            return false;
        res->w.f = canon_float(t, fma(a.w.f, b.w.f, c.w.f));
        return true;
    case OP_SEQ: case OP_SNE: case OP_SLT: case OP_SLE: case OP_SGT: case OP_SGE:
        *rt = T_B;
        if (!val_of(s, &ops[1], t, &a) || !val_of(s, &ops[2], t, &b)) return false;
        if (t == T_R) { /* identity: null against null, or null against a constant (never null) */
            if (in->op != OP_SEQ && in->op != OP_SNE) return false;
            if (a.kind == CK_POOL && b.kind == CK_POOL) return false;
            bool eq = a.kind == b.kind && a.w.u == b.w.u;
            res->w.i = eq == (in->op == OP_SEQ);
            return true;
        }
        if (!numeric(t)) return false;
        res->w.i = arith_compare(in->op, t, a.w, b.w);
        return true;
    case OP_SEL:
        if (!numeric(t) || !val_of(s, &ops[1], T_I64, &a)) return false;
        if (!val_of(s, &ops[a.w.i ? 2 : 3], t, &b)) return false;
        res->w = b.w;
        return true;
    case OP_CVT: case OP_BITCAST: {
        MlwsType from = (MlwsType)in->type2;
        if (!numeric(t) || !numeric(from) || !val_of(s, &ops[in->nops - 1], from, &a)) return false;
        res->w = in->op == OP_CVT ? arith_convert(t, from, a.w) : arith_bitcast(t, from, a.w);
        return true;
    }
    default: return false;
    }
}

/* `mov.<t> dst, value`, or false if no such instruction is valid. */
static bool make_mov(const Operand *dst, MlwsType t, const CVal *v, Insn *out) {
    *out = insn_make(OP_MOV, t, 2);
    out->ops[0] = *dst;
    out->ops[0].inner = NULL;
    if (v->kind == CK_POOL) {
        memset(&out->ops[1], 0, sizeof out->ops[1]);
        out->ops[1].mode = OM_CONST;
        out->ops[1].index = v->w.u;
    } else if (type_is_float(t)) {
        out->ops[1] = op_fimm(v->w.f);
    } else {
        out->ops[1] = op_imm(v->w.i);
    }
    CheckErr ce;
    if (dst->mode != OM_REG || !check_insn(out, &ce)) {
        insn_free(out);
        return false;
    }
    return true;
}

/* ---- the transfer function ---- */

static void step(CState *s, const Insn *in, const Effect *e) {
    CVal res;
    MlwsType rt;
    bool known = false;
    if (insn_pure(in) && in->ops[0].mode == OM_REG) {
        if (in->op == OP_MOV) {
            MlwsType t = (MlwsType)in->type;
            known = val_of(s, &in->ops[1], t, &res);
            rt = t == T_NONE ? T_I64 : t;
            if (known && in->ops[1].mode == OM_FIMM) rt = t == T_NONE ? T_F64 : t;
            if (known && REG_BANK(in->ops[0].reg) == BANK_R && res.kind == CK_WORD && res.w.u != 0) known = false;
        } else {
            known = evaluate(s, in, &res, &rt);
        }
    }
    for (int bk = 0; bk < TRACKED; bk++)
        for (uint32_t m = e->may.b[bk], i = 0; m; m >>= 1, i++)
            if (m & 1u) s->r[bk][i].k = CV_BOT;
    if (known) {
        uint8_t d = in->ops[0].reg;
        CVal *v = slot(s, d);
        if (v) {
            *v = res;
            v->made = 1;
            if (res.kind == CK_WORD) v->w = written(d, rt, res.w);
        }
    }
    /* a field or element of an R register used without a trap: it is an object */
    for (int k = 0; k < in->nops; k++) {
        const Operand *o = &in->ops[k];
        if (o->mode != OM_FIELD && o->mode != OM_ELEM && o->mode != OM_ELEMI) continue;
        if (REG_BANK(o->reg) != BANK_R || rs_has(&e->may, o->reg)) continue;
        CVal *v = slot(s, o->reg);
        if (!is_object(v)) v->k = CV_CONST, v->kind = CK_NONNULL, v->made = 0;
    }
}

/* ---- what branches say ---- */

static bool full_width(MlwsType t, Bank b) {
    return b == BANK_R ? t == T_R : b == BANK_I && (t == T_I64 || t == T_U64);
}

/* What branch `in` says about its operand on the edge it takes (taken) or
 * the one it falls through. */
static void refine(CState *s, const Insn *in, bool taken) {
    MlwsType t = (MlwsType)in->type;
    switch (in->op) {
    case OP_BZ: case OP_BNZ: {
        const Operand *o = &in->ops[0];
        if (o->mode != OM_REG) return;
        Bank b = (Bank)REG_BANK(o->reg);
        if (!full_width(t, b)) return;
        CVal *v = slot(s, o->reg);
        bool zero = (in->op == OP_BZ) == taken;
        if (zero) {
            v->k = CV_CONST, v->kind = CK_WORD, v->w.u = 0, v->made = 0;
        } else if (b == BANK_R && !is_object(v)) {
            v->k = CV_CONST, v->kind = CK_NONNULL, v->made = 0;
        }
        return;
    }
    case OP_BEQ: case OP_BNE: {
        if ((in->op == OP_BEQ) != taken) return; /* only equality says what the value is */
        for (int k = 0; k < 2; k++) {
            const Operand *o = &in->ops[k], *c = &in->ops[1 - k];
            if (o->mode != OM_REG || !full_width(t, (Bank)REG_BANK(o->reg))) continue;
            CVal *v = slot(s, o->reg);
            if (c->mode == OM_IMM) {
                if (REG_BANK(o->reg) == BANK_R && c->imm != 0) continue;
                v->k = CV_CONST, v->kind = CK_WORD, v->w.i = c->imm, v->made = 0;
            } else if (c->mode == OM_CONST && REG_BANK(o->reg) == BANK_R) {
                v->k = CV_CONST, v->kind = CK_POOL, v->w.u = c->index, v->made = 0;
            }
        }
        return;
    }
    case OP_BISA: case OP_BNISA: {
        const Operand *o = &in->ops[0];
        if ((in->op == OP_BISA) != taken || o->mode != OM_REG || REG_BANK(o->reg) != BANK_R) return;
        CVal *v = slot(s, o->reg);
        if (!is_object(v)) v->k = CV_CONST, v->kind = CK_NONNULL, v->made = 0;
        return;
    }
    default: return;
    }
}

/* Whether a conditional branch is taken, from what is known: 1, 0, or -1. */
static int decide_known(const CState *s, const Insn *in) {
    MlwsType t = (MlwsType)in->type;
    switch (in->op) {
    case OP_BZ: case OP_BNZ: {
        const Operand *o = &in->ops[0];
        bool zero;
        if (o->mode == OM_REG && REG_BANK(o->reg) == BANK_R) {
            const CVal *v = slot((CState *)s, o->reg);
            if (v->k != CV_CONST) return -1;
            zero = v->kind == CK_WORD && v->w.u == 0;
        } else if (type_is_int(t)) {
            CVal v;
            if (!val_of(s, o, t, &v) || v.kind != CK_WORD) return -1;
            zero = v.w.i == 0;
        } else {
            return -1;
        }
        return zero == (in->op == OP_BZ);
    }
    case OP_BISA: case OP_BNISA: {
        const Operand *o = &in->ops[0];
        if (o->mode != OM_REG || REG_BANK(o->reg) != BANK_R) return -1;
        const CVal *v = slot((CState *)s, o->reg);
        if (v->k != CV_CONST || v->kind != CK_WORD) return -1; /* only null is known not to be the struct */
        return in->op == OP_BNISA;
    }
    case OP_BEQ: case OP_BNE: case OP_BLT: case OP_BLE: case OP_BGT: case OP_BGE: {
        if (in->nops != 3) return -1;
        CVal a, b;
        if (t == T_R) {
            if (in->op != OP_BEQ && in->op != OP_BNE) return -1;
            const CVal *x = in->ops[0].mode == OM_REG ? slot((CState *)s, in->ops[0].reg) : NULL;
            const CVal *y = in->ops[1].mode == OM_REG ? slot((CState *)s, in->ops[1].reg) : NULL;
            CVal xi, yi;
            if (!x) {
                if (!val_of(s, &in->ops[0], t, &xi)) return -1;
                x = &xi;
            }
            if (!y) {
                if (!val_of(s, &in->ops[1], t, &yi)) return -1;
                y = &yi;
            }
            if (x->k != CV_CONST || y->k != CV_CONST) return -1;
            bool xnull = x->kind == CK_WORD && x->w.u == 0, ynull = y->kind == CK_WORD && y->w.u == 0;
            bool eq;
            if (xnull && ynull) eq = true;
            else if ((xnull && is_object(y)) || (ynull && is_object(x))) eq = false;
            else if (x->kind == CK_POOL && y->kind == CK_POOL && x->w.u == y->w.u) eq = true;
            else return -1;
            return eq == (in->op == OP_BEQ);
        }
        if (!(type_is_int(t) || type_is_float(t))) return -1;
        if (!val_of(s, &in->ops[0], t, &a) || !val_of(s, &in->ops[1], t, &b)) return -1;
        if (a.kind != CK_WORD || b.kind != CK_WORD) return -1;
        return arith_compare(in->op, t, a.w, b.w);
    }
    default: return -1;
    }
}

static bool cond_branch_op(int op) {
    return op == OP_BZ || op == OP_BNZ || op == OP_BEQ || op == OP_BNE || op == OP_BLT || op == OP_BLE ||
           op == OP_BGT || op == OP_BGE || op == OP_BISA || op == OP_BNISA;
}

/* ---- rewriting ---- */

/* The type operand k is read as. A ret reads its values as the function's
 * declared results, and a call its arguments as the callee's parameters,
 * when they are known (a function of this module). */
static MlwsType pos_type(const OFunc *f, const Insn *in, int k) {
    const Module *m = f->om->m;
    if (in->op == OP_RET) {
        const MFunc *mf = f->mf;
        return (mf->flags & MF_RETS_DECLARED) && k < mf->nrets ? (MlwsType)mf->rets[k] : T_NONE;
    }
    if (in->op == OP_CALL || in->op == OP_TCALL) {
        int64_t g = func_index_of(&in->ops[0]);
        if (g < 0 || (uint64_t)g >= m->nfuncs || (m->funcs[g].flags & MF_IMPORT)) return T_NONE;
        int a = k - 2;
        return a >= 0 && a < m->funcs[g].nparams ? (MlwsType)m->funcs[g].params[a] : T_NONE;
    }
    return operand_type(in, k);
}

/* The constant operand that reads like register `reg` (holding v) in
 * operand position k of in: in a typed position, any constant that reads the
 * same; where the operand's own type counts (untyped moves, pushes, calls,
 * returns, list and map values), only one whose natural type is the
 * register's (so a stored value's type tag is the same): an integer for an I
 * register, a float for an F register, a pool constant for an R register,
 * never a null (whose natural type is i64). */
static bool const_operand(const OFunc *f, const Insn *in, int k, uint8_t reg, const CVal *v, Operand *out) {
    Bank b = (Bank)REG_BANK(reg);
    MlwsType t = pos_type(f, in, k);
    memset(out, 0, sizeof *out);
    if (b == BANK_I) {
        if (t != T_NONE && !type_is_int(t)) return false;
        *out = op_imm(v->w.i);
    } else if (b == BANK_F) {
        if (t != T_NONE && !type_is_float(t)) return false;
        *out = op_fimm(v->w.f);
    } else if (b == BANK_R) {
        if (v->kind == CK_POOL) {
            out->mode = OM_CONST;
            out->index = v->w.u;
        } else {
            if (t != T_R) return false; /* null only where the operand is read as a reference */
            *out = op_imm(0);
        }
    } else {
        return false;
    }
    return true;
}

/* A store whose fast form takes its value from a register: a constant there
 * would send it to the general path (an element, a global or a closure
 * capture; a field has a form that stores a constant). */

/* A call's arguments and a ret's values: the register-only forms copy words
 * straight across, and with a constant among them each value's kind is
 * tested as it is copied. So a constant goes there only if it was computed in
 * the function, whose computing may then be removed; never for a value that
 * was only known (a new window's 0, or what a branch said). */
static bool passes_values(const Insn *in) {
    return in->op == OP_RET || in->op == OP_CALL || in->op == OP_TCALL || op_calls_closure(in->op);
}
static bool store_wants_register(const Insn *in) {
    if (in->op != OP_MOV || in->nops != 2) return false;
    uint8_t m = in->ops[0].mode;
    return m == OM_ELEM || m == OM_ELEMI || m == OM_GLOBAL || m == OM_GLOBALIDX || m == OM_UPVAL;
}

static bool substitute(OFunc *f, uint32_t pc, const CState *s) {
    Insn *in = &f->code.v[pc].in;
    MlwsType it = (MlwsType)in->type;
    if (it == T_BIG || it == T_DEC) return false; /* number arithmetic reads its operands as numbers */
    if (store_wants_register(in)) return false;
    bool any = false;
    for (int k = 0; k < in->nops; k++) {
        Operand *o = &in->ops[k];
        if (o->mode != OM_REG || !is_src_pos(in, k)) continue;
        const CVal *v = slot((CState *)s, o->reg);
        if (!v || v->k != CV_CONST || v->kind == CK_NONNULL) continue;
        if (passes_values(in) && !v->made) continue;
        Operand c, was = *o;
        if (!const_operand(f, in, k, o->reg, v, &c)) continue;
        *o = c;
        CheckErr ce;
        if (!check_insn(in, &ce)) {
            *o = was;
            continue;
        }
        any = true;
        ochange(f, PASS_CONSTPROP, pc, NULL, "a register read replaced by its constant");
    }
    return any;
}

static bool is_imm(const Operand *o, int64_t v) { return o->mode == OM_IMM && o->imm == v; }
static bool is_fimm(const Operand *o, double v) {
    return (o->mode == OM_FIMM && o->f == v && !signbit(o->f)) || (o->mode == OM_IMM && (double)o->imm == v);
}
static bool same_reg(const Operand *a, const Operand *b) { return a->mode == OM_REG && b->mode == OM_REG && a->reg == b->reg; }

static void replace(OFunc *f, uint32_t pc, Insn *with, int pass, const char *what) {
    Insn was = f->code.v[pc].in;
    ochange(f, pass, pc, &was, what);
    insn_free(&f->code.v[pc].in);
    f->code.v[pc].in = *with;
}

/* `op.t dst, src` for a one-source rewrite: a copy, or a constant. */
static bool to_mov(OFunc *f, uint32_t pc, const Operand *src, const char *what) {
    Insn *in = &f->code.v[pc].in;
    Insn m = insn_make(OP_MOV, (MlwsType)in->type, 2);
    m.ops[0] = in->ops[0];
    m.ops[1] = *src;
    if (src->inner) { /* a memory-indirect source: no */
        m.ops[0].inner = m.ops[1].inner = NULL;
        insn_free(&m);
        return false;
    }
    if (m.ops[0].inner) m.ops[0].inner = NULL;
    CheckErr ce;
    if (in->ops[0].mode != OM_REG || !check_insn(&m, &ce)) {
        insn_free(&m);
        return false;
    }
    replace(f, pc, &m, PASS_SIMPLIFY, what);
    return true;
}

static bool is_const_mode(const Operand *o) { return o->mode == OM_IMM || o->mode == OM_FIMM || o->mode == OM_CONST; }

/* The condition with its operands the other way round: a < b is b > a. */
static int mirrored(int op) {
    switch (op) {
    case OP_BLT: return OP_BGT;
    case OP_BGT: return OP_BLT;
    case OP_BLE: return OP_BGE;
    case OP_BGE: return OP_BLE;
    case OP_SLT: return OP_SGT;
    case OP_SGT: return OP_SLT;
    case OP_SLE: return OP_SGE;
    case OP_SGE: return OP_SLE;
    default: return op; /* eq, ne */
    }
}

/* A constant goes second, where the quickener's fast forms take one: a
 * compare (or compare and branch) with a constant first has its operands
 * swapped and its condition mirrored (exact, NaN included); commutative
 * arithmetic swaps them (for floats only add and mul: min and max of +0 and
 * -0 depend on the order). */
static bool constant_second(OFunc *f, uint32_t pc) {
    Insn *in = &f->code.v[pc].in;
    MlwsType t = (MlwsType)in->type;
    int a, b;
    switch (in->op) {
    case OP_BEQ: case OP_BNE: case OP_BLT: case OP_BLE: case OP_BGT: case OP_BGE:
        if (in->nops != 3) return false;
        a = 0, b = 1;
        break;
    case OP_SEQ: case OP_SNE: case OP_SLT: case OP_SLE: case OP_SGT: case OP_SGE:
        a = 1, b = 2;
        break;
    case OP_ADD: case OP_MUL:
        if (in->nops != 3 || !(type_is_int(t) || type_is_float(t))) return false;
        a = 1, b = 2;
        break;
    case OP_AND: case OP_OR: case OP_XOR: case OP_MIN: case OP_MAX:
        if (in->nops != 3 || !type_is_int(t)) return false;
        a = 1, b = 2;
        break;
    default: return false;
    }
    if (!is_const_mode(&in->ops[a]) || in->ops[b].mode != OM_REG) return false;
    if (t == T_BIG || t == T_DEC) return false;
    Operand x = in->ops[a];
    in->ops[a] = in->ops[b];
    in->ops[b] = x;
    int op = in->op;
    in->op = in->xop = (uint16_t)mirrored(op);
    CheckErr ce;
    if (!check_insn(in, &ce)) { /* (cannot happen: the same operands) */
        in->ops[b] = in->ops[a];
        in->ops[a] = x;
        in->op = in->xop = (uint16_t)op;
        return false;
    }
    ochange(f, PASS_SIMPLIFY, pc, NULL, "the constant put second, where the fast forms take it");
    return true;
}

/* Identities that hold for every value of the other operand, for three-operand
 * integer forms (and the few that are exact for floats). */
static bool simplify(OFunc *f, uint32_t pc) {
    if (constant_second(f, pc)) return true;
    Insn *in = &f->code.v[pc].in;
    MlwsType t = (MlwsType)in->type;
    if (in->ops[0].mode != OM_REG || REG_BANK(in->ops[0].reg) >= NBANKS) return false;
    if (in->nops == 3 && (type_is_int(t) || type_is_float(t))) {
        Operand *a = &in->ops[1], *b = &in->ops[2];
        bool ints = type_is_int(t);
        bool regs = (a->mode == OM_REG || a->mode == OM_IMM) && (b->mode == OM_REG || b->mode == OM_IMM);
        switch (in->op) {
        case OP_ADD:
            if (ints && is_imm(b, 0)) return to_mov(f, pc, a, "x + 0 is x");
            if (ints && is_imm(a, 0)) return to_mov(f, pc, b, "0 + x is x");
            break;
        case OP_SUB:
            if (ints ? is_imm(b, 0) : is_fimm(b, 0.0)) return to_mov(f, pc, a, "x - 0 is x");
            if (ints && same_reg(a, b)) {
                Operand z = op_imm(0);
                return to_mov(f, pc, &z, "x - x is 0");
            }
            break;
        case OP_MUL:
            if (ints ? is_imm(b, 1) : is_fimm(b, 1.0)) return to_mov(f, pc, a, "x * 1 is x");
            if (ints ? is_imm(a, 1) : is_fimm(a, 1.0)) return to_mov(f, pc, b, "1 * x is x");
            if (ints && regs && (is_imm(a, 0) || is_imm(b, 0))) {
                Operand z = op_imm(0);
                return to_mov(f, pc, &z, "x * 0 is 0");
            }
            if (ints && b->mode == OM_IMM && b->imm > 1 && (b->imm & (b->imm - 1)) == 0) {
                int sh = 0;
                while ((1ll << sh) != b->imm) sh++;
                Insn s = insn_make(OP_SHL, t, 3);
                s.ops[0] = in->ops[0];
                s.ops[1] = *a;
                s.ops[2] = op_imm(sh);
                if (a->inner) {
                    s.ops[1].inner = NULL;
                    insn_free(&s);
                    break;
                }
                CheckErr ce;
                if (!check_insn(&s, &ce)) {
                    insn_free(&s);
                    break;
                }
                in->ops[1].inner = NULL; /* now s's */
                replace(f, pc, &s, PASS_SIMPLIFY, "x * 2^k is x << k");
                return true;
            }
            break;
        case OP_DIV:
            if (ints ? is_imm(b, 1) : is_fimm(b, 1.0)) return to_mov(f, pc, a, "x / 1 is x");
            break;
        case OP_OR: case OP_XOR:
            if (ints && is_imm(b, 0)) return to_mov(f, pc, a, "x | 0 and x ^ 0 are x");
            if (ints && is_imm(a, 0)) return to_mov(f, pc, b, "0 | x and 0 ^ x are x");
            if (ints && in->op == OP_XOR && same_reg(a, b)) {
                Operand z = op_imm(0);
                return to_mov(f, pc, &z, "x ^ x is 0");
            }
            break;
        case OP_AND:
            if (ints && is_imm(b, -1)) return to_mov(f, pc, a, "x & -1 is x");
            if (ints && is_imm(a, -1)) return to_mov(f, pc, b, "-1 & x is x");
            if (ints && regs && (is_imm(a, 0) || is_imm(b, 0))) {
                Operand z = op_imm(0);
                return to_mov(f, pc, &z, "x & 0 is 0");
            }
            break;
        case OP_SHL: case OP_SHR: case OP_SAR: case OP_ROL: case OP_ROR:
            if (ints && is_imm(b, 0)) return to_mov(f, pc, a, "a shift by 0 is x");
            break;
        default: break;
        }
    }
    /* beq x, 0, L is bz x, L (for the types bz takes) */
    if ((in->op == OP_BEQ || in->op == OP_BNE) && in->nops == 3 && (type_is_int(t) || t == T_R || t == T_P)) {
        int x = is_imm(&in->ops[1], 0) ? 0 : is_imm(&in->ops[0], 0) ? 1 : -1;
        if (x >= 0) {
            Insn z = insn_make(in->op == OP_BEQ ? OP_BZ : OP_BNZ, t, 2);
            z.ops[0] = in->ops[x];
            z.ops[1] = in->ops[2];
            CheckErr ce;
            if (check_insn(&z, &ce)) {
                in->ops[x].inner = NULL;
                replace(f, pc, &z, PASS_SIMPLIFY, "a compare with 0 is bz / bnz");
                return true;
            }
            z.ops[0].inner = NULL;
            insn_free(&z);
        }
    }
    return false;
}

/* A pure instruction that writes its register the value it already has. */
static bool redundant(const CState *s, const Insn *in) {
    if (!insn_pure(in) || in->ops[0].mode != OM_REG) return false;
    uint8_t d = in->ops[0].reg;
    const CVal *old = slot((CState *)s, d);
    if (!old || old->k != CV_CONST) return false;
    CVal res;
    MlwsType rt;
    bool known;
    if (in->op == OP_MOV) {
        MlwsType t = (MlwsType)in->type;
        known = val_of(s, &in->ops[1], t, &res);
        rt = t != T_NONE ? t : in->ops[1].mode == OM_FIMM ? T_F64 : T_I64;
    } else {
        known = evaluate(s, in, &res, &rt);
    }
    if (!known || res.kind != old->kind) return false;
    if (res.kind == CK_WORD) res.w = written(d, rt, res.w);
    if (REG_BANK(d) == BANK_R && res.kind == CK_WORD && res.w.u != 0) return false;
    return res.w.u == old->w.u;
}

bool pass_constants(OFunc *f) {
    uint32_t n = (uint32_t)f->code.n;
    if (!n) return false;
    OCfg g;
    ocfg_build(f, &g);
    CState *in = xcalloc(g.n, sizeof *in);
    uint8_t *queued = xcalloc(g.n, 1);
    uint32_t *work = xmalloc((g.n + 1) * sizeof *work), nwork = 0;
    CState entry;
    memset(&entry, 0, sizeof entry);
    for (int bk = 0; bk < TRACKED; bk++)
        for (int i = 0; i < NREGS; i++) entry.r[bk][i].k = CV_CONST; /* a new window reads as zero */
    int cnt[NBANKS] = {0};
    for (int k = 0; k < f->mf->nparams; k++) {
        Bank b = type_bank((MlwsType)f->mf->params[k]);
        if (b < TRACKED) entry.r[b][cnt[b]].k = CV_BOT;
        cnt[b]++;
    }
    CState bot;
    memset(&bot, 0, sizeof bot);
    for (int bk = 0; bk < TRACKED; bk++)
        for (int i = 0; i < NREGS; i++) bot.r[bk][i].k = CV_BOT;
    meet(&in[0], &entry);
    work[nwork++] = 0, queued[0] = 1;
    for (size_t k = 0; k < f->h.n; k++) {
        uint32_t b = g.of[f->h.v[k].target];
        meet(&in[b], &bot);
        if (!queued[b]) queued[b] = 1, work[nwork++] = b;
    }
    uint8_t *reached = xcalloc(g.n, 1);
    Effect *eff = ofn_effects(f);
    bool facts = opt_on(f, PASS_BRANCHES); /* what branches say, and the branches it decides */
    while (nwork) {
        uint32_t b = work[--nwork];
        queued[b] = 0;
        reached[b] = 1;
        CState s = in[b];
        for (uint32_t i = g.b[b].start; i < g.b[b].end; i++) step(&s, &f->code.v[i].in, &eff[i]);
        uint32_t succ[64];
        uint32_t ns = ocfg_succs(&g, b, succ, 64);
        if (g.b[b].nextra > 64) { /* a jtab with very many targets: give up on what follows it */
            for (uint32_t k = 0; k < g.b[b].nextra; k++) {
                uint32_t t = g.b[b].extra[k];
                if (meet(&in[t], &bot) || !reached[t])
                    if (!queued[t]) queued[t] = 1, work[nwork++] = t;
            }
        }
        const Insn *last = &f->code.v[g.b[b].end - 1].in;
        bool cond = cond_branch_op(last->op);
        uint32_t tb = UINT32_MAX, fb = UINT32_MAX; /* the blocks of its target and of what follows it */
        int d = -1;
        if (cond) {
            uint32_t at = last->ops[last->nops - 1].index;
            if (at < n) tb = g.of[at];
            if (g.b[b].end < n) fb = g.of[g.b[b].end];
            if (tb == fb) cond = false;
            else if (facts) d = decide_known(&s, last);
        }
        for (uint32_t k = 0; k < ns; k++) {
            uint32_t t = succ[k];
            CState es = s;
            if (cond && (t == tb || t == fb)) {
                bool taken = t == tb;
                if (d >= 0 && d != (int)taken) continue; /* never goes this way */
                if (facts) refine(&es, last, taken);
            }
            if (meet(&in[t], &es) || !reached[t])
                if (!queued[t]) {
                    queued[t] = 1;
                    if (nwork >= g.n) work = xrealloc(work, (nwork + 1) * sizeof *work);
                    work[nwork++] = t;
                }
        }
    }
    bool ch = false;
    bool fold = opt_on(f, PASS_FOLD), prop = opt_on(f, PASS_CONSTPROP), simp = opt_on(f, PASS_SIMPLIFY);
    for (uint32_t b = 0; b < g.n; b++) {
        if (!reached[b]) continue;
        CState s = in[b];
        for (uint32_t i = g.b[b].start; i < g.b[b].end; i++) {
            Insn *ins = &f->code.v[i].in;
            CVal res;
            MlwsType rt;
            if (prop && redundant(&s, ins)) { /* the register already holds that value */
                ochange(f, PASS_CONSTPROP, i, ins, "the register already holds that value");
                f->code.v[i].dead = true;
                ch = true;
                continue;
            }
            if (fold && ins->op != OP_MOV && insn_pure(ins) && evaluate(&s, ins, &res, &rt)) {
                Insn m;
                if (make_mov(&ins->ops[0], rt, &res, &m)) {
                    replace(f, i, &m, PASS_FOLD, "computed now");
                    oeffect(&f->code.v[i].in, &eff[i]);
                    ch = true;
                }
            }
            if (facts && cond_branch_op(ins->op)) {
                int d = decide_known(&s, ins);
                if (d == 0) {
                    ochange(f, PASS_BRANCHES, i, ins, "known not to be taken (from what earlier code established)");
                    f->code.v[i].dead = true;
                    ch = true;
                    continue;
                }
                if (d == 1) {
                    Insn j = insn_make(OP_JMP, T_NONE, 1);
                    j.ops[0] = op_label(ins->ops[ins->nops - 1].index);
                    replace(f, i, &j, PASS_BRANCHES, "known to be taken (from what earlier code established)");
                    oeffect(&f->code.v[i].in, &eff[i]);
                    ch = true;
                    continue;
                }
            }
            bool was = ch;
            ch = false;
            if (prop) ch |= substitute(f, i, &s);
            if (simp) ch |= simplify(f, i);
            if (ch) oeffect(&f->code.v[i].in, &eff[i]);
            ch |= was;
            step(&s, &f->code.v[i].in, &eff[i]);
        }
    }
    free(eff);
    free(reached);
    free(work);
    free(queued);
    free(in);
    ocfg_free(&g);
    return ch;
}
