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
 * The moves are cleaned up afterwards (constprop, copyprop, dce). Only a
 * function that does nothing that depends on having a frame of its own: no
 * exception handlers, no stack (push, pop, enter, SP, FP, ARG, LOC), no
 * closure captures (UP), no catch, no tail call (it would end the caller's
 * frame), no coroutine switch. It may call other functions: they run from the
 * caller's frame. A fault inside it is reported in the caller's frame, and
 * caught by the caller's handlers, as it would be after the call returned the
 * error to it. A caller grows by at most its own size (and no more than
 * CALLER_MAX), so inlining cannot multiply code without bound. A function of
 * up to INLINE_SMALL instructions is inlined at any call; one up to
 * INLINE_MAX only at a call in a loop (a block on a cycle of the function's
 * blocks), where the call is repeated: elsewhere the copy would cost more
 * space than the call costs time. */
#include "opt_internal.h"

#define INLINE_SMALL 12    /* a function this small is inlined at any call */
#define INLINE_MAX 40      /* a larger one, up to this, only at a call in a loop */
#define CALLER_MAX 4000    /* instructions a caller may grow to */


static bool frameless(const Insn *in) {
    switch (in->op) {
    case OP_PUSH: case OP_POP: case OP_DROP: case OP_ENTER: case OP_LEA: case OP_CATCH: case OP_BREAK:
    case OP_TCALL: case OP_TCALLI: case OP_CORESUME: case OP_COYIELD: case OP_CONEW: case OP_COCUR:
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

static bool inlinable(const OFunc *g, bool in_loop) {
    const MFunc *mf = g->mf;
    if (g->code.n == 0 || g->code.n > (in_loop ? INLINE_MAX : INLINE_SMALL) || g->h.n) return false;
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
    if (f->code.n + g->code.n > 2 * (size_t)f->before + 64) return false; /* the caller's budget */
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
        uint32_t gl = g->mf->file == f->mf->file ? g->code.v[i].line : line; /* a line of another file: the call's */
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

/* For each instruction, whether its block is on a cycle: the blocks' strongly
 * connected components (Tarjan's algorithm, without recursion), a component
 * of more than one block, or of one that branches to itself. */
static uint8_t *loop_blocks(const OFunc *f) {
    uint32_t n = (uint32_t)f->code.n;
    uint8_t *at = xcalloc(n + 1, 1);
    if (!n) return at;
    OCfg g;
    ocfg_build(f, &g);
    uint32_t nb = g.n, counter = 0, sp = 0, cs = 0;
    uint32_t *index = xmalloc(nb * sizeof *index), *low = xmalloc(nb * sizeof *low), *stack = xmalloc(nb * sizeof *stack);
    uint32_t *cstack = xmalloc(nb * sizeof *cstack), *edge = xcalloc(nb, sizeof *edge);
    uint8_t *on = xcalloc(nb, 1), *cyc = xcalloc(nb, 1);
    for (uint32_t b = 0; b < nb; b++) index[b] = UINT32_MAX;
    for (uint32_t root = 0; root < nb; root++) {
        if (index[root] != UINT32_MAX) continue;
        cstack[cs++] = root;
        index[root] = low[root] = counter++;
        stack[sp++] = root, on[root] = 1;
        while (cs) {
            uint32_t v = cstack[cs - 1];
            uint32_t succ[OCFG_MAX_SUCCS];
            uint32_t ns = ocfg_succs(&g, v, succ, OCFG_MAX_SUCCS);
            if (edge[v] < ns) {
                uint32_t w = succ[edge[v]++];
                if (w == v) cyc[v] = 1;
                if (index[w] == UINT32_MAX) {
                    index[w] = low[w] = counter++;
                    stack[sp++] = w, on[w] = 1;
                    cstack[cs++] = w;
                } else if (on[w] && index[w] < low[v]) {
                    low[v] = index[w];
                }
                continue;
            }
            cs--;
            if (cs && low[v] < low[cstack[cs - 1]]) low[cstack[cs - 1]] = low[v];
            if (low[v] == index[v]) { /* v roots a component: pop it */
                uint32_t size = 0, top = sp;
                while (stack[--top] != v) size++;
                size++;
                for (uint32_t k = top; k < sp; k++) {
                    on[stack[k]] = 0;
                    if (size > 1) cyc[stack[k]] = 1;
                }
                sp = top;
            }
        }
    }
    for (uint32_t i = 0; i < n; i++) at[i] = cyc[g.of[i]];
    free(index);
    free(low);
    free(stack);
    free(cstack);
    free(edge);
    free(on);
    free(cyc);
    ocfg_free(&g);
    return at;
}

bool pass_inline(OFunc *f) {
    OModule *om = f->om;
    Module *m = om->m;
    bool ch = false;
    uint8_t *loop = loop_blocks(f); /* by instruction, in the numbering before any insertion */
    uint32_t shift = 0;             /* instructions inserted so far */
    for (uint32_t i = 0; i < f->code.n; i++) {
        OInsn *oi = &f->code.v[i];
        if (oi->dead || oi->in.op != OP_CALL) continue;
        int64_t gi = func_index_of(&oi->in.ops[0]);
        if (gi < 0 || (uint64_t)gi >= m->nfuncs || (uint32_t)gi == f->fi) continue;
        OFunc *g = om->fn[gi];
        if (!g || !inlinable(g, loop[i - shift])) continue;
        uint32_t before = (uint32_t)f->code.n;
        if (inline_call(f, i, g)) {
            ch = true;
            shift += (uint32_t)f->code.n - before;
            i += (uint32_t)f->code.n - before; /* past what was inserted, onto the dead call */
        }
    }
    free(loop);
    return ch;
}
