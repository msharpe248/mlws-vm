/* opt.c - the optimizer's driver: which passes run, in what order, and the
 * report of what they did. */
#include "opt_internal.h"

#include <ctype.h>

#include "../platform/platform.h"

static const struct {
    const char *name;
    int level;
    const char *doc;
} PASSES[PASS__COUNT] = {
#define X(e, name, level, doc) {name, level, doc},
    OPT_PASSES(X)
#undef X
};

const char *opt_pass_name(int pass) { return PASSES[pass].name; }
const char *opt_pass_doc(int pass) { return PASSES[pass].doc; }
int opt_pass_level(int pass) { return PASSES[pass].level; }

static uint32_t level_passes(int level) {
    uint32_t s = 0;
    for (int p = 0; p < PASS__COUNT; p++)
        if (PASSES[p].level <= level) s |= 1u << p;
    return s;
}

bool opt_parse(const char *spec, OptConfig *cfg, char *err, size_t errlen) {
    uint32_t s = 0;
    const char *p = spec;
    while (*p) {
        const char *e = strchr(p, ',');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        bool minus = n && *p == '-';
        const char *w = p + minus;
        size_t wn = n - minus;
        int pass = -1;
        for (int k = 0; k < PASS__COUNT; k++)
            if (wn == strlen(PASSES[k].name) && !strncmp(w, PASSES[k].name, wn)) pass = k;
        if (pass >= 0) {
            if (minus) s &= ~(1u << pass);
            else s |= 1u << pass;
        } else if (!minus && wn == 1 && *w >= '0' && *w <= '3') {
            s = level_passes(*w - '0');
        } else if (!minus && ((wn == 4 && !strncmp(w, "none", 4)))) {
            s = 0;
        } else if (!minus && wn == 6 && !strncmp(w, "encode", 6)) {
            s |= OPT_ENCODE_ONLY;
        } else if (!minus && wn == 3 && !strncmp(w, "all", 3)) { /* every pass, those run only by name too */
            s = level_passes(OPT_NAMED_ONLY);
        } else {
            snprintf(err, errlen, "unknown optimization '%.*s' (a level 0-3, all, none, or a pass: fold, simplify, "
                                  "constprop, cse, copyprop, dce, branches, unreachable, peephole, deadfuncs, compact, "
                                  "inline, tailcall; -name turns one off)",
                     (int)n, p);
            return false;
        }
        p = e ? e + 1 : p + n;
    }
    cfg->passes = s;
    return true;
}

/* The passes that change one function, until none changes anything (or a
 * few rounds have run: each round only makes code smaller, so this is a
 * bound on time, not on correctness). */
static void cleanup(OFunc *f) {
    for (int round = 0; round < 8; round++) {
        bool ch = false; /* (each pass sees the code without what the one before removed) */
        if (opt_on(f, PASS_FOLD) || opt_on(f, PASS_CONSTPROP) || opt_on(f, PASS_SIMPLIFY)) ch |= pass_constants(f);
        ofn_compact(f);
        if (opt_on(f, PASS_CSE)) ch |= pass_cse(f);
        ofn_compact(f);
        if (opt_on(f, PASS_COPYPROP)) ch |= pass_copyprop(f);
        if (opt_on(f, PASS_PEEPHOLE)) ch |= pass_peephole(f);
        ofn_compact(f);
        if (opt_on(f, PASS_BRANCHES)) ch |= pass_branches(f);
        ofn_compact(f);
        if (opt_on(f, PASS_UNREACHABLE)) ch |= pass_unreachable(f);
        ofn_compact(f);
        if (opt_on(f, PASS_DCE)) ch |= pass_dce(f);
        ofn_compact(f);
        if (!ch) break;
    }
}

void opt_module(Module *m, const OptConfig *cfg, OptReport *rep) {
    if (!cfg->passes) return;
    op_init(); /* the opcode table's operand patterns (a module read from a file may be the first use) */
    uint64_t t0 = plat_now_ns();
    OModule om = {m, cfg, rep, NULL, NULL};
    if (rep) {
        OptModStat st;
        memset(&st, 0, sizeof st);
        st.name = xstrdup(m->name ? m->name : "?");
        st.source = xstrdup(m->source ? m->source : "");
        VEC_PUSH(rep->mods, st);
        om.stat = &rep->mods.v[rep->mods.n - 1];
    }
    om.fn = xcalloc(m->nfuncs + 1, sizeof *om.fn);
    for (uint32_t i = 0; i < m->nfuncs; i++)
        if (!(m->funcs[i].flags & MF_IMPORT)) om.fn[i] = ofn_load(&om, i);
    for (uint32_t i = 0; i < m->nfuncs; i++)
        if (om.fn[i]) cleanup(om.fn[i]);
    if (cfg->passes >> PASS_INLINE & 1u) /* callees are as small as they will get first */
        for (uint32_t i = 0; i < m->nfuncs; i++)
            if (om.fn[i] && pass_inline(om.fn[i])) cleanup(om.fn[i]);
    for (uint32_t i = 0; i < m->nfuncs; i++) {
        OFunc *f = om.fn[i];
        if (!f) continue;
        if (opt_on(f, PASS_TAILCALL) && pass_tailcall(f)) cleanup(f);
    }
    if (cfg->passes >> PASS_DEADFUNCS & 1u) pass_deadfuncs(&om);
    for (uint32_t i = 0; i < m->nfuncs; i++)
        if (om.fn[i] && opt_on(om.fn[i], PASS_COMPACT)) pass_compact(om.fn[i]);
    if (om.stat)
        for (uint32_t i = 0; i < m->nfuncs; i++) {
            OFunc *f = om.fn[i];
            if (!f) continue;
            ofn_compact(f);
            OptFuncStat fs = {xstrdup(f->mf->name), f->before, (uint32_t)f->code.n, f->regs_before, ofn_regs(f)};
            VEC_PUSH(om.stat->funcs, fs);
        }
    ofn_store(&om);
    for (uint32_t i = 0; i < m->nfuncs; i++) ofn_free(om.fn[i]);
    free(om.fn);
    if (rep) rep->ns += plat_now_ns() - t0;
}

/* ---- the report ---- */

static void put_count(FILE *f, uint64_t v) { /* 1,284 */
    char b[32], o[48];
    int n = snprintf(b, sizeof b, "%llu", (unsigned long long)v), j = 0;
    for (int i = 0; i < n; i++) {
        if (i && (n - i) % 3 == 0) o[j++] = ',';
        o[j++] = b[i];
    }
    o[j] = 0;
    fprintf(f, "%9s", o);
}

typedef struct {
    const OptModStat *mod;
    const OptFuncStat *fn;
} FuncRef;

static int by_removed(const void *a, const void *b) {
    const OptFuncStat *x = ((const FuncRef *)a)->fn, *y = ((const FuncRef *)b)->fn;
    int64_t dx = (int64_t)x->before - x->after, dy = (int64_t)y->before - y->after;
    if (dx != dy) return dx > dy ? -1 : 1;
    return strcmp(x->func, y->func);
}

void opt_report_print(const OptReport *rep, FILE *f) {
    uint64_t counts[PASS__COUNT] = {0}, before = 0, after = 0, rb = 0, ra = 0, nf = 0, removed = 0;
    for (size_t i = 0; i < rep->mods.n; i++) {
        const OptModStat *ms = &rep->mods.v[i];
        for (int p = 0; p < PASS__COUNT; p++) counts[p] += ms->counts[p];
        for (size_t k = 0; k < ms->funcs.n; k++) {
            before += ms->funcs.v[k].before;
            after += ms->funcs.v[k].after;
            rb += ms->funcs.v[k].regs_before;
            ra += ms->funcs.v[k].regs_after;
        }
        nf += ms->funcs.n;
        removed += ms->funcs_removed;
    }
    fprintf(f, "mlwsvm opt: %zu module%s, %llu function%s, %.1f ms\n", rep->mods.n, rep->mods.n == 1 ? "" : "s",
            (unsigned long long)nf, nf == 1 ? "" : "s", (double)rep->ns / 1e6);
    fprintf(f, "                   before    after\n");
    fprintf(f, "  instructions  ");
    put_count(f, before);
    put_count(f, after);
    if (before) fprintf(f, "   %+.1f%%", 100.0 * ((double)after - (double)before) / (double)before);
    fprintf(f, "\n  registers     ");
    put_count(f, rb);
    put_count(f, ra);
    fprintf(f, "\n");
    if (removed) fprintf(f, "  functions removed: %llu\n", (unsigned long long)removed);
    fprintf(f, "  pass          changes\n");
    for (int p = 0; p < PASS__COUNT; p++)
        if (counts[p]) {
            fprintf(f, "  %-12s", PASSES[p].name);
            put_count(f, counts[p]);
            fprintf(f, "   %s\n", PASSES[p].doc);
        }
    VEC(FuncRef) fr = {0};
    for (size_t i = 0; i < rep->mods.n; i++)
        for (size_t k = 0; k < rep->mods.v[i].funcs.n; k++) {
            FuncRef r = {&rep->mods.v[i], &rep->mods.v[i].funcs.v[k]};
            if (r.fn->before != r.fn->after || r.fn->regs_before != r.fn->regs_after) VEC_PUSH(fr, r);
        }
    if (fr.n) {
        qsort(fr.v, fr.n, sizeof *fr.v, by_removed);
        fprintf(f, "  functions changed (most instructions removed first):\n");
        for (size_t i = 0; i < fr.n && i < 20; i++) {
            const OptFuncStat *x = fr.v[i].fn;
            fprintf(f, "    %s.%s  %u -> %u", fr.v[i].mod->name, x->func, x->before, x->after);
            if (x->regs_before != x->regs_after) fprintf(f, "  (registers %u -> %u)", x->regs_before, x->regs_after);
            fprintf(f, "\n");
        }
        if (fr.n > 20) fprintf(f, "    ... and %zu more\n", fr.n - 20);
    }
    VEC_FREE(fr);
    if (rep->changes.n) {
        fprintf(f, "  changes:\n");
        for (size_t i = 0; i < rep->changes.n; i++) fprintf(f, "    %s\n", rep->changes.v[i]);
    }
}

void opt_report_free(OptReport *rep) {
    for (size_t i = 0; i < rep->mods.n; i++) {
        OptModStat *ms = &rep->mods.v[i];
        free(ms->name);
        free(ms->source);
        for (size_t k = 0; k < ms->funcs.n; k++) free(ms->funcs.v[k].func);
        VEC_FREE(ms->funcs);
    }
    VEC_FREE(rep->mods);
    for (size_t i = 0; i < rep->changes.n; i++) free(rep->changes.v[i]);
    VEC_FREE(rep->changes);
}
