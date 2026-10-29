/* inline.c - a call to a small function of this module is replaced by the
 * function's body, on registers of the caller's that nothing else uses:
 *
 *   call g (a, b) -> (r)      mov.<p0> X0, a          its parameters
 *                             mov.<p1> X1, b
 *                             mov.i64 X2, 0           its other registers start as 0,
 *                             ...g's body on X0, X1, X2...      as a new window does
 *                             mov.<r0> r, X1          each ret: the results,
 *                             jmp done                then on after the call
 *                     done:
 *
 * The moves are cleaned up afterwards (constprop, copyprop, dce). Only a leaf
 * function that does nothing that depends on having a frame of its own: no
 * calls, no exception handlers, no stack (push, pop, enter, SP, FP, ARG, LOC),
 * no closure captures (UP), no catch. A fault inside it is then reported in
 * the caller's frame, and caught by the caller's handlers, as it would be
 * after the call returned the error to it. */
#include "opt_internal.h"

#define INLINE_MAX 12      /* instructions in the function inlined */
#define CALLER_MAX 4000    /* instructions a caller may grow to */


static bool frameless(const Insn *in) {
    if (op_info[in->op].flags & OPF_CALL) return false;
    switch (in->op) {
    case OP_PUSH: case OP_POP: case OP_DROP: case OP_ENTER: case OP_LEA: case OP_CATCH: case OP_BREAK:
        return false;
    default: break;
    }
    for (int k = 0; k < in->nops; k++) {
        const Operand *o = &in->ops[k];
        if (o->mode == OM_UPVAL || o->mode == OM_MIND || o->mode == OM_MINDB) return false;
        if (o->mode >= OM_POSTINC && o->mode <= OM_PREDECB) return false;
        if ((o->mode == OM_REG || o->mode == OM_IND || o->mode == OM_DISP || o->mode == OM_DISPB ||
             o->mode == OM_IDX || o->mode == OM_IDXB) && REG_BANK(o->reg) >= NBANKS)
            return false; /* SP, FP (ARG, LOC), PID */
    }
    return true;
}

static bool inlinable(const OFunc *g) {
    const MFunc *mf = g->mf;
    if (g->code.n == 0 || g->code.n > INLINE_MAX || g->h.n) return false;
    if (!(mf->flags & MF_RETS_DECLARED)) return false;
    for (int k = 0; k < mf->nparams; k++)
        if (mf->params[k] == T_BIG || mf->params[k] == T_DEC) return false;
    for (int k = 0; k < mf->nrets; k++)
        if (mf->rets[k] == T_BIG || mf->rets[k] == T_DEC) return false;
    for (size_t i = 0; i < g->code.n; i++)
        if (!frameless(&g->code.v[i].in)) return false;
    return true;
}

static uint8_t remap(uint8_t r, int map[NBANKS][NREGS]) {
    if (REG_BANK(r) >= NBANKS) return r;
    return REG_MAKE(REG_BANK(r), map[REG_BANK(r)][REG_IDX(r)]);
}

static void remap_operand(Operand *o, int map[NBANKS][NREGS]) {
    switch (o->mode) {
    case OM_REG: case OM_IND: case OM_DISP: case OM_DISPB: case OM_ELEMI: case OM_FIELD: o->reg = remap(o->reg, map); break;
    case OM_IDX: case OM_IDXB: case OM_ELEM:
        o->reg = remap(o->reg, map);
        o->ireg = remap(o->ireg, map);
        break;
    case OM_GLOBALIDX: case OM_GLOBALIDXB: o->ireg = remap(o->ireg, map); break;
    default: break;
    }
}

static Operand copy_operand(const Operand *o) {
    Operand c = *o;
    if (o->inner) {
        c.inner = xmalloc(sizeof *c.inner);
        *c.inner = *o->inner;
    }
    return c;
}

static OInsn emit(Insn in, uint32_t line) {
    OInsn o;
    memset(&o, 0, sizeof o);
    o.in = in;
    o.line = line;
    return o;
}

static Insn mov(MlwsType t, Operand dst, Operand src) {
    Insn m = insn_make(OP_MOV, t, 2);
    m.ops[0] = dst;
    m.ops[1] = src;
    return m;
}

/* Inline the call at pc to g; false (changing nothing) if it does not fit. */
static bool inline_call(OFunc *f, uint32_t pc, OFunc *g) {
    const Insn *call = &f->code.v[pc].in;
    const MFunc *gm = g->mf;
    int argc = (int)call->ops[1].imm, nres = call->nops - 2 - argc;
    if (argc != gm->nparams || nres > gm->nrets) return false;
    if (f->code.n + g->code.n * 3 + 64 > CALLER_MAX) return false;
    uint32_t gused[NBANKS], fused[NBANKS];
    ofn_used_regs(g, gused);
    ofn_used_regs(f, fused);
    int gp[NBANKS] = {0}, fp[NBANKS] = {0};
    for (int k = 0; k < gm->nparams; k++) gused[type_bank((MlwsType)gm->params[k])] |= 1u << gp[type_bank((MlwsType)gm->params[k])]++;
    for (int k = 0; k < f->mf->nparams; k++) fused[type_bank((MlwsType)f->mf->params[k])] |= 1u << fp[type_bank((MlwsType)f->mf->params[k])]++;
    int map[NBANKS][NREGS];
    for (int b = 0; b < NBANKS; b++) {
        int next = 0;
        for (int r = 0; r < NREGS; r++) {
            map[b][r] = -1;
            if (!(gused[b] >> r & 1u)) continue;
            while (next < NREGS && (fused[b] >> next & 1u)) next++;
            if (next >= NREGS) return false; /* not enough free registers */
            map[b][r] = next++;
        }
    }
    VEC(OInsn) out = {0};
    uint32_t line = f->code.v[pc].line;
    /* the parameters */
    int cnt[NBANKS] = {0};
    for (int k = 0; k < argc; k++) {
        MlwsType t = (MlwsType)gm->params[k];
        Bank b = type_bank(t);
        int r = cnt[b]++;
        VEC_PUSH(out, emit(mov(t, op_reg(REG_MAKE(b, map[b][r])), copy_operand(&call->ops[2 + k])), line));
    }
    /* the other registers start as 0 */
    for (int b = 0; b < NBANKS; b++)
        for (int r = cnt[b]; r < NREGS; r++) {
            if (map[b][r] < 0) continue;
            static const MlwsType Z[NBANKS] = {T_I64, T_F64, T_R, T_P};
            Operand z = b == BANK_F ? op_fimm(0.0) : op_imm(0);
            VEC_PUSH(out, emit(mov(Z[b], op_reg(REG_MAKE(b, map[b][r])), z), line));
        }
    /* the body: where each of its instructions starts */
    uint32_t n = (uint32_t)g->code.n, base = (uint32_t)out.n;
    uint32_t *pos = xcalloc(n + 1, sizeof *pos), at = base;
    for (uint32_t i = 0; i < n; i++) {
        pos[i] = at;
        const Insn *in = &g->code.v[i].in;
        at += in->op == OP_RET ? (uint32_t)nres + (i + 1 < n) : 1;
    }
    pos[n] = at;
    uint32_t done = pc + at; /* the call itself, after the insertion: removed, so its successor */
    for (uint32_t i = 0; i < n; i++) {
        const Insn *in = &g->code.v[i].in;
        uint32_t gl = g->code.v[i].line;
        if (in->op == OP_RET) {
            for (int j = 0; j < nres; j++) {
                Operand v = copy_operand(&in->ops[j]);
                remap_operand(&v, map);
                VEC_PUSH(out, emit(mov((MlwsType)gm->rets[j], copy_operand(&call->ops[2 + argc + j]), v), gl));
            }
            if (i + 1 < n) {
                Insn j = insn_make(OP_JMP, T_NONE, 1);
                j.ops[0] = op_label(done);
                VEC_PUSH(out, emit(j, gl));
            }
            continue;
        }
        Insn c;
        insn_copy(&c, in);
        for (int k = 0; k < c.nops; k++) {
            if (c.ops[k].mode == OM_LABEL) c.ops[k].index = pc + pos[c.ops[k].index < n ? c.ops[k].index : n];
            else remap_operand(&c.ops[k], map);
        }
        VEC_PUSH(out, emit(c, gl));
    }
    free(pos);
    for (size_t i = 0; i < out.n; i++) { /* every instruction is one the verifier takes */
        CheckErr ce;
        if (!check_insn(&out.v[i].in, &ce)) {
            for (size_t k = 0; k < out.n; k++) insn_free(&out.v[k].in);
            VEC_FREE(out);
            return false;
        }
    }
    char what[160];
    snprintf(what, sizeof what, "the call to %s inlined (%zu instructions)", gm->name, out.n);
    ochange(f, PASS_INLINE, pc, call, what);
    uint32_t len = (uint32_t)out.n;
    ofn_insert(f, pc, out.v, len);
    VEC_FREE(out);
    f->code.v[pc + len].dead = true; /* the call */
    for (int b = 0; b < NBANKS; b++)
        for (int r = 0; r < NREGS; r++)
            if (map[b][r] >= 0 && map[b][r] + 1 > f->mf->nregs[b]) f->mf->nregs[b] = (uint16_t)(map[b][r] + 1);
    return true;
}

bool pass_inline(OFunc *f) {
    OModule *om = f->om;
    Module *m = om->m;
    bool ch = false;
    for (uint32_t i = 0; i < f->code.n; i++) {
        OInsn *oi = &f->code.v[i];
        if (oi->dead || oi->in.op != OP_CALL) continue;
        int64_t gi = func_index_of(&oi->in.ops[0]);
        if (gi < 0 || (uint64_t)gi >= m->nfuncs || (uint32_t)gi == f->fi) continue;
        OFunc *g = om->fn[gi];
        if (!g || !inlinable(g)) continue;
        uint32_t before = (uint32_t)f->code.n;
        if (inline_call(f, i, g)) {
            ch = true;
            i += (uint32_t)f->code.n - before; /* past what was inserted, onto the dead call */
        }
    }
    return ch;
}
