/* hooks.c - the instruction hook, the tracer (--trace) and the profiler
 * (--profile, --flame). See docs/TOOLING.md. */
#include "../core/disasm.h"
#include "../heap/object.h"
#include "tools.h"
#include "tools_internal.h"

/* ---- formatting ------------------------------------------------------------- */

void fmt_location(StrBuf *b, const Function *f, uint32_t pc) {
    uint32_t line = f->mf->lines && pc < f->mf->ninsns ? f->mf->lines[pc] : 0;
    const char *src = f->mod->m->source && *f->mod->m->source ? f->mod->m->source : f->mod->path;
    sb_printf(b, "%s.%s+%u (%s:%u)", f->mod->m->name, f->name, pc, src, line);
}

static void fmt_string(StrBuf *b, const char *s, uint32_t n) {
    if (n > 60) {
        sb_put_quoted(b, s, 60);
        sb_printf(b, "...(%u bytes)", n);
    } else {
        sb_put_quoted(b, s, n);
    }
}

void fmt_value(StrBuf *b, Word v, MlwsType t, int depth) {
    char buf[64];
    switch (t) {
    case T_NONE: sb_puts(b, "0"); return;
    case T_B: sb_puts(b, v.i ? "true" : "false"); return;
    case T_C:
        if (v.u >= 32 && v.u < 127) sb_printf(b, "'%c'", (int)v.u);
        else sb_printf(b, "U+%04llX", (unsigned long long)v.u);
        return;
    case T_F32: case T_F64:
        fmt_double(v.f, buf, sizeof buf);
        sb_puts(b, buf);
        return;
    case T_P:
        if (!v.p) sb_puts(b, "null");
        else sb_printf(b, "<ptr seg=%u gen=%u off=%u>", PTR_SEG(v.p), PTR_GEN(v.p), PTR_OFF(v.p));
        return;
    case T_R: break;
    default:
        if (type_is_signed(t) || t == T_NONE) sb_printf(b, "%lld", (long long)v.i);
        else sb_printf(b, "%llu", (unsigned long long)v.u);
        return;
    }
    Obj *o = v.r;
    if (!o) {
        sb_puts(b, "null");
        return;
    }
    switch (o->kind) {
    case OBJ_STRING: fmt_string(b, ((ObjString *)o)->data, ((ObjString *)o)->len); return;
    case OBJ_BYTES:
        sb_puts(b, "bytes ");
        fmt_string(b, (const char *)((ObjBytes *)o)->data, ((ObjBytes *)o)->len);
        return;
    default: break;
    }
    if (depth <= 0) {
        sb_printf(b, "<%s>", o->kind == OBJ_STRUCT ? ((ObjStruct *)o)->def->name : obj_kind_name(o->kind));
        return;
    }
    switch (o->kind) {
    case OBJ_ARRAY: {
        ObjArray *a = (ObjArray *)o;
        sb_printf(b, "%s[%u] [", type_name((MlwsType)a->elem), a->len);
        for (uint32_t i = 0; i < a->len && i < 8; i++) {
            if (i) sb_puts(b, ", ");
            fmt_value(b, a->data[i], (MlwsType)a->elem, depth - 1);
        }
        sb_puts(b, a->len > 8 ? ", ...]" : "]");
        return;
    }
    case OBJ_LIST: {
        ObjList *l = (ObjList *)o;
        sb_printf(b, "list(%u) [", l->len);
        for (uint32_t i = 0; i < l->len && i < 8; i++) {
            if (i) sb_puts(b, ", ");
            fmt_value(b, l->items[i], (MlwsType)l->tags[i], depth - 1);
        }
        sb_puts(b, l->len > 8 ? ", ...]" : "]");
        return;
    }
    case OBJ_MAP: {
        ObjMap *m = (ObjMap *)o;
        sb_printf(b, "map(%u) {", m->count);
        uint32_t shown = 0;
        for (uint32_t i = 0; i < m->nentries && shown < 8; i++) {
            MapEntry *e = &m->entries[i];
            if (!e->live) continue;
            if (shown++) sb_puts(b, ", ");
            fmt_value(b, e->key, (MlwsType)e->ktag, depth - 1);
            sb_puts(b, ": ");
            fmt_value(b, e->val, (MlwsType)e->vtag, depth - 1);
        }
        sb_puts(b, m->count > 8 ? ", ...}" : "}");
        return;
    }
    case OBJ_STRUCT: {
        ObjStruct *s = (ObjStruct *)o;
        sb_printf(b, "%s {", s->def->name);
        for (uint32_t i = 0; i < s->def->nfields; i++) {
            sb_printf(b, "%s%s: ", i ? ", " : "", s->def->fields[i]);
            fmt_value(b, s->fields[i], (MlwsType)s->def->types[i], depth - 1);
        }
        sb_puts(b, "}");
        return;
    }
    default: sb_puts(b, "<object>"); return;
    }
}

/* ---- the hook --------------------------------------------------------------- */

static void trace_insn(Tools *t, Process *p, Function *f, uint32_t pc) {
    if (t->trace_limit && t->traced >= t->trace_limit) return;
    t->traced++;
    StrBuf b = {0};
    sb_printf(&b, "[%llu] ", (unsigned long long)p->pid);
    fmt_location(&b, f, pc);
    sb_puts(&b, "  ");
    disasm_insn(&b, f->mod->m, &f->code[pc]);
    sb_putc(&b, '\n');
    fwrite(b.s, 1, b.n, t->trace);
    sb_free(&b);
}

static void profile_insn(Tools *t, Process *p, Function *f, uint32_t pc) {
    t->op_counts[f->code[pc].op]++;
    if (!f->prof) {
        f->prof = xcalloc(f->ncode, sizeof(uint64_t));
        VEC_PUSH(t->prof_funcs, f);
    }
    f->prof[pc]++;
    if (++t->sample_tick < t->sample_every) return;
    t->sample_tick = 0;
    t->samples++;
    StrBuf b = {0};
    for (uint32_t i = 0; i < p->nframes; i++) {
        const Function *fi = p->frames[i].fn;
        sb_printf(&b, "%s%s.%s", i ? ";" : "", fi->mod->m->name, fi->name);
    }
    void *v;
    uintptr_t n = smap_get(&t->stacks, b.s, &v) ? (uintptr_t)v : 0;
    smap_put(&t->stacks, b.s, (void *)(n + 1));
    sb_free(&b);
}

static bool tools_hook(Process *p, uint32_t pc) {
    Tools *t = p->vm->tools;
    Function *f = p->frames[p->nframes - 1].fn;
    if (t->dbg && debugger_hook(t->dbg, p, pc)) return true;
    if (t->trace) trace_insn(t, p, f, pc);
    if (t->profile) profile_insn(t, p, f, pc);
    return false;
}

Tools *tools_of(VM *vm) {
    if (!vm->tools) {
        Tools *t = xcalloc(1, sizeof *t);
        t->sample_every = 997; /* a prime, so samples don't alias with loops */
        vm->tools = t;
        vm->tools_free = tools_free;
    }
    return vm->tools;
}

/* The per-instruction hook costs a call per instruction, so it is only
 * installed for the tools that need it (not for the monitor). */
void tools_enable_hook(VM *vm) {
    tools_of(vm);
    vm->hook = tools_hook;
}

void tools_free(VM *vm) {
    Tools *t = vm->tools;
    if (!t) return;
    tools_monitor_close(vm);
    if (t->trace && t->trace != stderr && t->trace != stdout) fclose(t->trace);
    VEC_FREE(t->prof_funcs);
    smap_free(&t->stacks);
    free(t);
    vm->tools = NULL;
    vm->hook = NULL;
}

/* ---- public setup and reports ------------------------------------------------- */

bool tools_trace(VM *vm, const char *path, uint64_t limit) {
    tools_enable_hook(vm);
    Tools *t = tools_of(vm);
    t->trace = !path || !*path || !strcmp(path, "-") ? stderr : fopen(path, "w");
    t->trace_limit = limit;
    return t->trace != NULL;
}

void tools_profile(VM *vm) {
    tools_enable_hook(vm);
    tools_of(vm)->profile = true;
}

typedef struct {
    const Function *f;
    uint32_t pc;
    uint64_t n;
} Hot;

static int hot_cmp(const void *a, const void *b) {
    uint64_t x = ((const Hot *)a)->n, y = ((const Hot *)b)->n;
    return x < y ? 1 : x > y ? -1 : 0;
}

void tools_profile_report(VM *vm, FILE *out, const char *flame_path) {
    Tools *t = vm->tools;
    if (!t || !t->profile) return;
    uint64_t total = 0;
    for (int i = 0; i < OP__COUNT; i++) total += t->op_counts[i];
    if (!total) total = 1;
    fprintf(out, "--- mlwsvm profile: %llu instructions ---\n", (unsigned long long)total);

    VEC(Hot) fn = {0}, lines = {0};
    for (size_t i = 0; i < t->prof_funcs.n; i++) {
        const Function *f = t->prof_funcs.v[i];
        Hot h = {f, 0, 0};
        for (uint32_t pc = 0; pc < f->ncode; pc++) {
            h.n += f->prof[pc];
            uint32_t line = f->mf->lines ? f->mf->lines[pc] : 0;
            /* aggregate per source line within this function */
            size_t j;
            for (j = 0; j < lines.n; j++)
                if (lines.v[j].f == f && lines.v[j].pc == line) break;
            if (j == lines.n) {
                Hot l = {f, line, 0};
                VEC_PUSH(lines, l);
            }
            lines.v[j].n += f->prof[pc];
        }
        VEC_PUSH(fn, h);
    }
    qsort(fn.v, fn.n, sizeof *fn.v, hot_cmp);
    qsort(lines.v, lines.n, sizeof *lines.v, hot_cmp);
    fprintf(out, "\nfunctions (instructions executed in the function itself; entries = runs of its first\n"
                 "instruction, i.e. calls plus any loop back to it):\n");
    fprintf(out, "  %6s %12s %10s  %s\n", "%", "insns", "entries", "function");
    for (size_t i = 0; i < fn.n && i < 20; i++) {
        const Function *f = fn.v[i].f;
        fprintf(out, "  %5.1f%% %12llu %10llu  %s.%s\n", 100.0 * (double)fn.v[i].n / (double)total,
                (unsigned long long)fn.v[i].n, (unsigned long long)f->prof[0], f->mod->m->name, f->name);
    }
    fprintf(out, "\nhot lines:\n");
    fprintf(out, "  %6s %12s  %s\n", "%", "insns", "location");
    for (size_t i = 0; i < lines.n && i < 15; i++) {
        const Function *f = lines.v[i].f;
        const char *src = f->mod->m->source && *f->mod->m->source ? f->mod->m->source : f->mod->path;
        fprintf(out, "  %5.1f%% %12llu  %s:%u (%s.%s)\n", 100.0 * (double)lines.v[i].n / (double)total,
                (unsigned long long)lines.v[i].n, src, lines.v[i].pc, f->mod->m->name, f->name);
    }
    Hot ops[OP__COUNT];
    int nops = 0;
    for (int i = 0; i < OP__COUNT; i++)
        if (t->op_counts[i]) ops[nops++] = (Hot){NULL, (uint32_t)i, t->op_counts[i]};
    qsort(ops, (size_t)nops, sizeof *ops, hot_cmp);
    fprintf(out, "\nopcodes:\n");
    for (int i = 0; i < nops && i < 15; i++)
        fprintf(out, "  %5.1f%% %12llu  %s\n", 100.0 * (double)ops[i].n / (double)total, (unsigned long long)ops[i].n,
                op_info[ops[i].pc].mnem);
    VEC_FREE(fn);
    VEC_FREE(lines);
    if (flame_path) {
        FILE *f = fopen(flame_path, "w");
        if (!f) {
            fprintf(out, "cannot write %s\n", flame_path);
            return;
        }
        for (size_t i = 0; i < t->stacks.cap; i++)
            if (t->stacks.keys[i]) fprintf(f, "%s %llu\n", t->stacks.keys[i], (unsigned long long)(uintptr_t)t->stacks.vals[i]);
        fclose(f);
        fprintf(out, "\n%llu stack samples written to %s (collapsed format, for flamegraph.pl or speedscope)\n",
                (unsigned long long)t->samples, flame_path);
    }
}
