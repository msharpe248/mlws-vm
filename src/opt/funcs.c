/* funcs.c - deadfuncs: functions of this module that nothing can call are
 * removed. What can be called is what is exported (other modules, module.function
 * and C hosts reach only those), `main`, and every function named by an
 * instruction of one that can be called (a call, a spawn, a closure). */
#include "opt_internal.h"

static void mark(OModule *om, uint32_t fi, uint8_t *live) {
    if (live[fi]) return;
    live[fi] = 1;
    OFunc *f = om->fn[fi];
    if (!f) return;
    for (size_t i = 0; i < f->code.n; i++) {
        if (f->code.v[i].dead) continue;
        const Insn *in = &f->code.v[i].in;
        for (int k = 0; k < in->nops; k++) {
            int64_t g = func_index_of(&in->ops[k]);
            if (g >= 0 && (uint64_t)g < om->m->nfuncs) mark(om, (uint32_t)g, live);
        }
    }
}

void pass_deadfuncs(OModule *om) {
    Module *m = om->m;
    uint32_t n = m->nfuncs;
    uint8_t *live = xcalloc(n + 1, 1);
    for (uint32_t i = 0; i < n; i++) {
        const MFunc *mf = &m->funcs[i];
        if ((mf->flags & (MF_EXPORT | MF_IMPORT)) || !strcmp(mf->name, "main") || !om->fn[i]) mark(om, i, live);
    }
    uint32_t *to = xcalloc(n + 1, sizeof *to), j = 0;
    for (uint32_t i = 0; i < n; i++) to[i] = live[i] ? j++ : UINT32_MAX;
    if (j == n) {
        free(live);
        free(to);
        return;
    }
    for (uint32_t i = 0; i < n; i++) { /* every function that stays refers to the new indices */
        OFunc *f = om->fn[i];
        if (!f || !live[i]) continue;
        for (size_t p = 0; p < f->code.n; p++) {
            Insn *in = &f->code.v[p].in;
            for (int k = 0; k < in->nops; k++)
                if (in->ops[k].mode == OM_FUNC && in->ops[k].index < n) in->ops[k].index = to[in->ops[k].index];
        }
    }
    for (uint32_t i = 0; i < n; i++) {
        if (live[i]) continue;
        OFunc *f = om->fn[i];
        if (om->stat) {
            om->stat->counts[PASS_DEADFUNCS]++;
            om->stat->funcs_removed++;
        }
        if (om->rep && om->rep->log) {
            StrBuf b = {0};
            sb_printf(&b, "%s %s deadfuncs: nothing calls it", m->source ? m->source : m->name, m->funcs[i].name);
            VEC_PUSH(om->rep->changes, b.s);
        }
        ofn_free(f);
        om->fn[i] = NULL;
        MFunc *mf = &m->funcs[i];
        free(mf->name);
        free(mf->lines);
        free(mf->locs);
        free(mf->handlers);
    }
    j = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (!live[i]) continue;
        m->funcs[j] = m->funcs[i];
        om->fn[j] = om->fn[i];
        if (om->fn[j]) {
            om->fn[j]->fi = j;
            om->fn[j]->mf = &m->funcs[j];
        }
        j++;
    }
    m->nfuncs = j;
    free(live);
    free(to);
}
