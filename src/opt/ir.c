/* ir.c - the optimizer's working form of a function: decoded instructions
 * with their source lines and .loc runs, which passes rewrite, delete and
 * insert, and which is encoded back into the module at the end. Also the
 * facts about instructions the passes share: what they read and write, which
 * are pure, and the blocks of a function. */
#include "opt_internal.h"

#include "../core/disasm.h"

OFunc *ofn_load(OModule *om, uint32_t fi) {
    Module *m = om->m;
    MFunc *mf = &m->funcs[fi];
    OFunc *f = xcalloc(1, sizeof *f);
    f->om = om;
    f->fi = fi;
    f->mf = mf;
    ByteReader r = {m->code + mf->code_off, mf->code_len, 0, false};
    while (r.pos < r.n) {
        OInsn oi = {0};
        MlwsError err;
        if (!decode_insn(&r, &oi.in, &err)) { /* a damaged module: the loader reports it; leave this function be */
            ofn_free(f);
            return NULL;
        }
        uint32_t i = (uint32_t)f->code.n;
        oi.line = mf->lines && i < mf->ninsns ? mf->lines[i] : 0;
        VEC_PUSH(f->code, oi);
    }
    f->nruns = mf->nlocs;
    f->runs = xcalloc(mf->nlocs + 1, sizeof *f->runs);
    if (mf->nlocs) memcpy(f->runs, mf->locs, mf->nlocs * sizeof *f->runs);
    for (uint32_t k = 0; k < mf->nlocs; k++) {
        uint32_t at = mf->locs[k].start;
        uint32_t *rb, *rn;
        if (at < f->code.n) rb = &f->code.v[at].rb, rn = &f->code.v[at].rn;
        else rb = &f->tail_rb, rn = &f->tail_rn;
        if (!*rn) *rb = k;
        (*rn)++;
    }
    for (uint32_t k = 0; k < mf->nhandlers; k++) VEC_PUSH(f->h, mf->handlers[k]);
    f->before = (uint32_t)f->code.n;
    f->regs_before = ofn_regs(f);
    return f;
}

void ofn_free(OFunc *f) {
    if (!f) return;
    for (size_t i = 0; i < f->code.n; i++) insn_free(&f->code.v[i].in);
    VEC_FREE(f->code);
    VEC_FREE(f->h);
    free(f->runs);
    free(f);
}

uint32_t ofn_regs(const OFunc *f) {
    uint32_t n = 0;
    for (int b = 0; b < NBANKS; b++) n += f->mf->nregs[b];
    return n;
}

void ofn_compact(OFunc *f) {
    uint32_t n = (uint32_t)f->code.n;
    uint32_t *fwd = xmalloc((n + 1) * sizeof *fwd); /* old index -> new index of it, or of the next live one */
    uint32_t live = 0;
    for (uint32_t i = 0; i < n; i++) live += !f->code.v[i].dead;
    uint32_t next = live;
    for (uint32_t i = n; i-- > 0;) {
        if (!f->code.v[i].dead) next--;
        fwd[i] = next;
    }
    fwd[n] = live;
    if (live == n) {
        free(fwd);
        return;
    }
    uint32_t pb = 0, pn = 0, j = 0;
    for (uint32_t i = 0; i < n; i++) {
        OInsn *o = &f->code.v[i];
        if (o->dead) {
            if (o->rn) {
                if (!pn) pb = o->rb;
                pn += o->rn;
            }
            insn_free(&o->in);
            continue;
        }
        if (pn) { /* the runs of the removed instructions before it come first */
            o->rb = pb;
            o->rn += pn;
            pn = 0;
        }
        f->code.v[j++] = *o;
    }
    if (pn) { /* removed instructions at the end: their runs start at the end */
        f->tail_rb = pb;
        f->tail_rn += pn;
    }
    f->code.n = j;
    for (uint32_t i = 0; i < j; i++) {
        Insn *in = &f->code.v[i].in;
        for (int k = 0; k < in->nops; k++)
            if (in->ops[k].mode == OM_LABEL) in->ops[k].index = fwd[in->ops[k].index];
    }
    size_t h = 0;
    for (size_t k = 0; k < f->h.n; k++) {
        MHandler x = f->h.v[k];
        x.start = fwd[x.start];
        x.end = fwd[x.end];
        x.target = fwd[x.target];
        if (x.start < x.end) f->h.v[h++] = x; /* a region with nothing left in it protects nothing */
    }
    f->h.n = h;
    free(fwd);
}

void ofn_insert(OFunc *f, uint32_t at, OInsn *ins, uint32_t n) {
    if (!n) return;
    for (size_t i = 0; i < f->code.n; i++) {
        Insn *in = &f->code.v[i].in;
        for (int k = 0; k < in->nops; k++)
            if (in->ops[k].mode == OM_LABEL && in->ops[k].index > at) in->ops[k].index += n;
    }
    for (size_t k = 0; k < f->h.n; k++) {
        MHandler *x = &f->h.v[k];
        if (x->start > at) x->start += n;
        if (x->end > at) x->end += n;
        if (x->target > at) x->target += n;
    }
    VEC_GROW(f->code, f->code.n + n);
    memmove(&f->code.v[at + n], &f->code.v[at], (f->code.n - at) * sizeof *f->code.v);
    for (uint32_t k = 0; k < n; k++) {
        f->code.v[at + k] = ins[k];
        f->code.v[at + k].rb = f->code.v[at + k].rn = 0;
        f->code.v[at + k].dead = false;
    }
    f->code.n += n;
    OInsn *was = &f->code.v[at + n]; /* its runs now start at the first inserted instruction */
    f->code.v[at].rb = was->rb;
    f->code.v[at].rn = was->rn;
    was->rn = 0;
}

void ofn_store(OModule *om) {
    Module *m = om->m;
    ByteBuf code = {0};
    for (uint32_t fi = 0; fi < m->nfuncs; fi++) {
        MFunc *mf = &m->funcs[fi];
        OFunc *f = om->fn ? om->fn[fi] : NULL;
        if (!f) { /* an import, or a function that could not be decoded: its bytes as they were */
            if (mf->flags & MF_IMPORT) continue;
            uint32_t off = (uint32_t)code.n;
            bb_bytes(&code, m->code + mf->code_off, mf->code_len);
            mf->code_off = off;
            continue;
        }
        ofn_compact(f);
        uint32_t n = (uint32_t)f->code.n;
        mf->code_off = (uint32_t)code.n;
        for (uint32_t i = 0; i < n; i++) encode_insn(&code, &f->code.v[i].in);
        mf->code_len = (uint32_t)code.n - mf->code_off;
        mf->ninsns = n;
        if (mf->lines) {
            free(mf->lines);
            mf->lines = xcalloc(n + 1, sizeof *mf->lines);
            for (uint32_t i = 0; i < n; i++) mf->lines[i] = f->code.v[i].line;
        }
        /* Of the runs that start at an instruction, the last is the one in
         * force (the others now cover nothing, their instructions gone), and
         * runs at the end cover nothing at all: the format has at most one
         * run per instruction, in order. */
        uint32_t nl = 0;
        for (uint32_t i = 0; i < n; i++) nl += f->code.v[i].rn > 0;
        MLoc *locs = nl ? xcalloc(nl, sizeof *locs) : NULL;
        uint32_t j = 0;
        for (uint32_t i = 0; i < n; i++) {
            const OInsn *o = &f->code.v[i];
            if (!o->rn) continue;
            locs[j] = f->runs[o->rb + o->rn - 1];
            locs[j++].start = i;
        }
        free(mf->locs);
        mf->locs = locs;
        mf->nlocs = nl;
        free(mf->handlers);
        mf->nhandlers = (uint32_t)f->h.n;
        mf->handlers = f->h.n ? xcalloc(f->h.n, sizeof *mf->handlers) : NULL;
        if (f->h.n) memcpy(mf->handlers, f->h.v, f->h.n * sizeof *mf->handlers);
    }
    free(m->code);
    m->code = code.b;
    m->codelen = (uint32_t)code.n;
}

/* ---- building instructions ---- */

void insn_copy(Insn *dst, const Insn *src) {
    *dst = *src;
    dst->ops = xcalloc(src->nops ? src->nops : 1, sizeof(Operand));
    for (int k = 0; k < src->nops; k++) {
        dst->ops[k] = src->ops[k];
        if (src->ops[k].inner) {
            dst->ops[k].inner = xmalloc(sizeof(Operand));
            *dst->ops[k].inner = *src->ops[k].inner;
        }
    }
}

Insn insn_make(int op, MlwsType t, int nops) {
    Insn in;
    memset(&in, 0, sizeof in);
    in.op = in.xop = (uint16_t)op;
    in.type = (uint8_t)t;
    in.nops = (uint8_t)nops;
    in.ops = xcalloc(nops ? (size_t)nops : 1, sizeof(Operand));
    return in;
}

Operand op_reg(uint8_t reg) {
    Operand o;
    memset(&o, 0, sizeof o);
    o.mode = OM_REG;
    o.reg = reg;
    return o;
}

Operand op_imm(int64_t v) {
    Operand o;
    memset(&o, 0, sizeof o);
    o.mode = OM_IMM;
    o.imm = v;
    return o;
}

Operand op_fimm(double v) {
    Operand o;
    memset(&o, 0, sizeof o);
    o.mode = OM_FIMM;
    o.f = v;
    return o;
}

Operand op_label(uint32_t target) {
    Operand o;
    memset(&o, 0, sizeof o);
    o.mode = OM_LABEL;
    o.index = target;
    return o;
}

void ochange(OFunc *f, int pass, uint32_t pc, const Insn *was, const char *what) {
    OModule *om = f->om;
    if (om->stat) om->stat->counts[pass]++;
    if (!om->rep || !om->rep->log) return;
    StrBuf b = {0};
    const Module *m = om->m;
    uint32_t line = pc < f->code.n ? f->code.v[pc].line : 0;
    const char *src = module_func_file(m, f->mf);
    sb_printf(&b, "%s:%u %s+%u %s: %s", src && *src ? src : m->name, line, f->mf->name, pc, opt_pass_name(pass),
              what);
    if (was) {
        sb_puts(&b, ": ");
        disasm_insn(&b, m, was);
    }
    VEC_PUSH(om->rep->changes, b.s);
}

/* ---- what instructions do ---- */

static bool is_rmw(const Insn *in) { /* `add I0, I1` is I0 = I0 + I1: the destination is read too */
    const OpInfo *oi = &op_info[in->op];
    if (in->op == OP_INC || in->op == OP_DEC || in->op == OP_SWAP) return true;
    return oi->nelems && oi->elems[0] == 'D' &&
           ((oi->last_mod == '?' && in->nops == oi->nelems - 1) || (oi->last_mod == '+' && in->nops == oi->nelems));
}

bool is_dst_pos(const Insn *in, int k) {
    const OpInfo *oi = &op_info[in->op];
    if (oi->flags & OPF_CALL) return k >= 2 + (int)in->ops[1].imm;
    return strchr("DWBEXJH", op_pattern_at(oi, k)) != NULL;
}

bool is_src_pos(const Insn *in, int k) {
    const OpInfo *oi = &op_info[in->op];
    if (oi->flags & OPF_CALL) return k >= 2 && k < 2 + (int)in->ops[1].imm;
    return strchr("SVQAO", op_pattern_at(oi, k)) != NULL;
}

static void add_modified_bases(const Operand *o, RegSet *s) {
    if (o->mode >= OM_POSTINC && o->mode <= OM_PREDECB) rs_add(s, o->reg);
    if ((o->mode == OM_MIND || o->mode == OM_MINDB) && o->inner) add_modified_bases(o->inner, s);
}

void oeffect(const Insn *in, Effect *e) {
    insn_effect(in, &e->use, &e->def);
    e->may = e->def;
    if (is_rmw(in) && in->nops && in->ops[0].mode == OM_REG) rs_add(&e->use, in->ops[0].reg);
    for (int k = 0; k < in->nops; k++) {
        if (is_dst_pos(in, k) && in->ops[k].mode == OM_REG) rs_add(&e->may, in->ops[k].reg);
        add_modified_bases(&in->ops[k], &e->may);
    }
}

Effect *ofn_effects(const OFunc *f) {
    Effect *e = xcalloc(f->code.n + 1, sizeof *e);
    for (size_t i = 0; i < f->code.n; i++) oeffect(&f->code.v[i].in, &e[i]);
    return e;
}

static bool plain_operand(const Operand *o) {
    return o->mode == OM_REG || o->mode == OM_IMM || o->mode == OM_FIMM || o->mode == OM_CONST;
}

/* The value a source operand reads, as type t, is never zero: an immediate
 * that stays nonzero at that width. */
static bool nonzero_imm(const Operand *o, MlwsType t) {
    if (o->mode != OM_IMM) return false;
    return canon_int(t, o->imm) != 0;
}

bool insn_pure(const Insn *in) {
    MlwsType t = (MlwsType)in->type;
    switch (in->op) {
    case OP_MOV:
        if (t == T_BIG || t == T_DEC) return false; /* makes a number object */
        break;
    case OP_ADD: case OP_SUB: case OP_MUL: case OP_MIN: case OP_MAX: case OP_NEG: case OP_ABS: case OP_INC:
    case OP_DEC: case OP_AND: case OP_OR: case OP_XOR: case OP_NOT: case OP_SHL: case OP_SHR: case OP_SAR:
    case OP_ROL: case OP_ROR: case OP_POPCNT: case OP_CLZ: case OP_CTZ: case OP_BSWAP: case OP_SQRT: case OP_FLOOR:
    case OP_CEIL: case OP_ROUND: case OP_TRUNC: case OP_FMA: case OP_SEL: case OP_BITCAST:
        if (!type_is_int(t) && !type_is_float(t)) return false;
        break;
    case OP_SEQ: case OP_SNE: case OP_SLT: case OP_SLE: case OP_SGT: case OP_SGE:
        if (!type_is_int(t) && !type_is_float(t) && t != T_R) return false;
        break;
    case OP_DIV: case OP_REM:
        if (type_is_float(t)) break;
        if (!type_is_int(t)) return false;
        for (int k = in->nops == 2 ? 1 : 2; k < in->nops; k++)
            if (!nonzero_imm(&in->ops[k], t)) return false;
        break;
    case OP_CVT:
        if (type_is_ref(in->type) || type_is_ref(in->type2)) return false;
        break;
    case OP_TAGOF: case OP_TYPEOF:
        break;
    default: return false;
    }
    for (int k = 0; k < in->nops; k++) {
        const Operand *o = &in->ops[k];
        if (!plain_operand(o)) return false;
        if (o->mode == OM_REG && REG_BANK(o->reg) >= NBANKS) return false; /* SP, FP, PID */
    }
    if (in->ops[0].mode != OM_REG) return false;
    if (in->op == OP_MOV && t == T_NONE) { /* untyped: the source's own type must suit the destination */
        const Operand *s = &in->ops[1];
        Bank want = (Bank)REG_BANK(in->ops[0].reg);
        Bank have = s->mode == OM_REG ? (Bank)REG_BANK(s->reg) : s->mode == OM_FIMM ? BANK_F : s->mode == OM_CONST ? BANK_R : BANK_I;
        if (want != have) return false;
    }
    return true;
}

bool in_try(const OFunc *f, uint32_t pc) {
    for (size_t k = 0; k < f->h.n; k++)
        if (pc >= f->h.v[k].start && pc < f->h.v[k].end) return true;
    return false;
}

uint32_t next_live(const OFunc *f, uint32_t pc) {
    while (pc < f->code.n && f->code.v[pc].dead) pc++;
    return pc;
}

int64_t func_index_of(const Operand *o) { return o->mode == OM_FUNC ? (int64_t)o->index : -1; }

/* ---- blocks ---- */

static bool ends_block(const Insn *in) {
    if (op_info[in->op].flags & OPF_TERM) return true;
    for (int k = 0; k < in->nops; k++)
        if (in->ops[k].mode == OM_LABEL) return true;
    return false;
}

void ocfg_build(const OFunc *f, OCfg *g) {
    uint32_t n = (uint32_t)f->code.n;
    uint8_t *lead = xcalloc(n + 1, 1);
    if (n) lead[0] = 1;
    for (uint32_t i = 0; i < n; i++) {
        const Insn *in = &f->code.v[i].in;
        for (int k = 0; k < in->nops; k++)
            if (in->ops[k].mode == OM_LABEL && in->ops[k].index < n) lead[in->ops[k].index] = 1;
        if (ends_block(in) && i + 1 < n) lead[i + 1] = 1;
    }
    for (size_t k = 0; k < f->h.n; k++)
        if (f->h.v[k].target < n) lead[f->h.v[k].target] = 1;
    g->n = 0;
    for (uint32_t i = 0; i < n; i++) g->n += lead[i];
    g->b = xcalloc(g->n + 1, sizeof *g->b);
    g->of = xcalloc(n + 1, sizeof *g->of);
    uint32_t bi = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (lead[i] && i) bi++;
        if (lead[i]) g->b[bi].start = i;
        g->b[bi].end = i + 1;
        g->of[i] = bi;
    }
    for (uint32_t b = 0; b < g->n; b++) {
        OBlock *B = &g->b[b];
        const Insn *last = &f->code.v[B->end - 1].in;
        if (!(op_info[last->op].flags & OPF_TERM) && B->end < n) B->succ[B->nsucc++] = g->of[B->end];
        int labels = 0;
        for (int k = 0; k < last->nops; k++) labels += last->ops[k].mode == OM_LABEL;
        if (labels > 1) B->extra = xcalloc((size_t)labels, sizeof *B->extra);
        for (int k = 0; k < last->nops; k++) {
            if (last->ops[k].mode != OM_LABEL || last->ops[k].index >= n) continue;
            uint32_t s = g->of[last->ops[k].index];
            if (labels > 1) B->extra[B->nextra++] = s;
            else B->succ[B->nsucc++] = s;
        }
    }
    free(lead);
}

void ocfg_free(OCfg *g) {
    for (uint32_t b = 0; b < g->n; b++) free(g->b[b].extra);
    free(g->b);
    free(g->of);
    memset(g, 0, sizeof *g);
}

uint32_t ocfg_succs(const OCfg *g, uint32_t i, uint32_t *out, uint32_t cap) {
    const OBlock *B = &g->b[i];
    uint32_t n = 0;
    for (uint32_t k = 0; k < B->nsucc && n < cap; k++) out[n++] = B->succ[k];
    for (uint32_t k = 0; k < B->nextra && n < cap; k++) out[n++] = B->extra[k];
    return n;
}
