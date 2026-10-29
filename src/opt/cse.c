/* cse.c - a computation, or a load, done again where nothing can have changed
 * its result is replaced by a copy of the register that holds it (cse):
 *
 *   mul F3, F1, F2               mul F3, F1, F2
 *   ...                          ...
 *   mul F7, F1, F2      ->       mov F7, F3       (copyprop and dce clean up)
 *
 *   mov R30, R17->Tab.hm         mov R30, R17->Tab.hm
 *   mov R31, R17->Tab.hm  ->     mov R31, R30
 *
 * The same instruction, apart from its destination: the same operation and
 * types, and the same operands. It stays available while its result
 * register, and every register it reads, are unchanged; a load also only
 * while nothing can have written memory (any instruction that is not pure,
 * not a load and not a branch: a store, a call, an object update). Within a
 * block, and into a block whose only predecessor it is known after: chains
 * of blocks, not joins, so a value is never assumed on a path that does not
 * compute it. */
#include "opt_internal.h"

#define CSE_MAX 48 /* expressions remembered at once */

typedef struct {
    uint32_t pc;   /* the instruction that computed it */
    uint8_t reg;   /* the register holding it */
    bool load;     /* reads memory */
    RegSet reads;  /* the registers it reads */
} Avail;

typedef struct {
    Avail a[CSE_MAX];
    int n;
} Table;

static bool same_operand(const Operand *a, const Operand *b) {
    if (a->mode != b->mode || a->reg != b->reg || a->ireg != b->ireg || a->imm != b->imm || a->scale != b->scale ||
        a->index != b->index)
        return false;
    if (a->mode == OM_FIMM && memcmp(&a->f, &b->f, sizeof a->f)) return false;
    if (a->mode == OM_MIND || a->mode == OM_MINDB) return false;
    return true;
}

/* The same instruction but for its destination. */
static bool same_expr(const Insn *a, const Insn *b) {
    if (a->op != b->op || a->type != b->type || a->type2 != b->type2 || a->nops != b->nops) return false;
    for (int k = 1; k < a->nops; k++)
        if (!same_operand(&a->ops[k], &b->ops[k])) return false;
    return true;
}

static bool memory_source(const Operand *o) {
    switch (o->mode) {
    case OM_IND: case OM_DISP: case OM_DISPB: case OM_IDX: case OM_IDXB: case OM_GLOBAL: case OM_GLOBALB:
    case OM_GLOBALIDX: case OM_GLOBALIDXB: case OM_ELEM: case OM_ELEMI: case OM_FIELD: case OM_UPVAL:
        return true;
    default: return false;
    }
}

/* `mov reg, memory`: a load (never [P1]+ or -[P1], which change P1). */
static bool is_load(const Insn *in) {
    if (in->op != OP_MOV || in->nops != 2 || in->ops[0].mode != OM_REG) return false;
    if (REG_BANK(in->ops[0].reg) >= NBANKS || in->type == T_BIG || in->type == T_DEC) return false;
    if (!memory_source(&in->ops[1])) return false;
    return in->ops[1].mode < OM_POSTINC || in->ops[1].mode > OM_PREDECB;
}

/* A computation worth remembering: pure, not a plain move, and with every
 * source distinct from its destination (`add I0, I1` reads I0 too). */
static bool is_computation(const Insn *in) {
    if (!insn_pure(in) || in->op == OP_MOV || in->ops[0].mode != OM_REG) return false;
    for (int k = 1; k < in->nops; k++)
        if (in->ops[k].mode == OM_REG && in->ops[k].reg == in->ops[0].reg) return false;
    return in->nops >= 2 && (in->nops > 2 || in->op == OP_CVT || in->op == OP_BITCAST || in->op == OP_TAGOF ||
                             in->op == OP_TYPEOF);
}

static bool leaves_memory(const Insn *in) {
    if (insn_pure(in) || is_load(in)) return true;
    switch (in->op) {
    case OP_JMP: case OP_BZ: case OP_BNZ: case OP_BEQ: case OP_BNE: case OP_BLT: case OP_BLE: case OP_BGT:
    case OP_BGE: case OP_BISA: case OP_BNISA: case OP_JTAB: case OP_NOP: case OP_LEN: case OP_STRCMP:
        return true;
    case OP_MOV: /* a move into a register (of a constant or another register) */
        return in->ops[0].mode == OM_REG && !memory_source(&in->ops[1]);
    default: return false;
    }
}

static bool rs_meets(const RegSet *a, const RegSet *b) {
    for (int k = 0; k < NBANKS; k++)
        if (a->b[k] & b->b[k]) return true;
    return false;
}

/* After an instruction with effect e: what it changed is no longer known. */
static void forget(Table *t, const Insn *in, const Effect *e) {
    bool mem = !leaves_memory(in);
    int j = 0;
    for (int i = 0; i < t->n; i++) {
        Avail *a = &t->a[i];
        if ((mem && a->load) || rs_has(&e->may, a->reg) || rs_meets(&e->may, &a->reads)) continue;
        t->a[j++] = *a;
    }
    t->n = j;
}

bool pass_cse(OFunc *f) {
    uint32_t n = (uint32_t)f->code.n;
    if (!n) return false;
    OCfg g;
    ocfg_build(f, &g);
    uint32_t *preds = xcalloc(g.n + 1, sizeof *preds), *pred = xcalloc(g.n + 1, sizeof *pred);
    for (uint32_t b = 0; b < g.n; b++) {
        uint32_t succ[64];
        uint32_t ns = ocfg_succs(&g, b, succ, 64);
        for (uint32_t k = 0; k < ns; k++) preds[succ[k]]++, pred[succ[k]] = b;
        for (uint32_t k = 64; k < g.b[b].nextra; k++) preds[g.b[b].extra[k]] += 2;
    }
    for (size_t k = 0; k < f->h.n; k++) /* a handler is entered from anywhere in its region */
        if (f->h.v[k].target < n) preds[g.of[f->h.v[k].target]] += 2;
    preds[0]++; /* the entry */
    Table *out = xcalloc(g.n + 1, sizeof *out);
    uint8_t *done = xcalloc(g.n + 1, 1);
    Effect *eff = ofn_effects(f);
    bool ch = false;
    for (uint32_t b = 0; b < g.n; b++) {
        Table t;
        t.n = 0;
        if (preds[b] == 1 && done[pred[b]]) t = out[pred[b]]; /* known after its only predecessor */
        for (uint32_t i = g.b[b].start; i < g.b[b].end; i++) {
            Insn *in = &f->code.v[i].in;
            bool load = is_load(in), comp = !load && is_computation(in);
            if (load || comp) {
                for (int k = 0; k < t.n; k++) {
                    Avail *a = &t.a[k];
                    const Insn *was = &f->code.v[a->pc].in;
                    uint8_t d = in->ops[0].reg;
                    if (a->reg == d || REG_BANK(a->reg) != REG_BANK(d) || !same_expr(in, was)) continue;
                    Insn m = insn_make(OP_MOV, T_NONE, 2);
                    m.ops[0] = op_reg(d);
                    m.ops[1] = op_reg(a->reg);
                    ochange(f, PASS_CSE, i, in, load ? "loaded again: a copy of the register that holds it"
                                                       : "computed again: a copy of the register that holds it");
                    insn_free(in);
                    *in = m;
                    oeffect(in, &eff[i]);
                    ch = true;
                    load = comp = false;
                    break;
                }
            }
            forget(&t, in, &eff[i]);
            if ((load || comp) && t.n < CSE_MAX && !rs_has(&eff[i].use, in->ops[0].reg)) {
                Avail a = {i, in->ops[0].reg, load, eff[i].use};
                t.a[t.n++] = a;
            }
        }
        out[b] = t;
        done[b] = 1;
    }
    free(eff);
    free(done);
    free(out);
    free(pred);
    free(preds);
    ocfg_free(&g);
    return ch;
}
