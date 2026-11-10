/* pdebug.c - a process debugged alone, from the monitor: /api/debug. Where
 * `mlwsvm debug` and `mlwsvm dap` run a whole program interpreted on one
 * thread and stop all of it, this debugs one process of a VM that goes on as
 * it did: the process is interpreted, each instruction after pdebug_hook
 * (VM.phook), and when it is to stop it alone is paused (PROC_PAUSED), to be
 * read and resumed by requests to the monitor. Its state is Process.dbg; the
 * breakpoints are the VM's, and stop whichever debugged process reaches one.
 * Every request here is served with the world stopped. See docs/TOOLING.md. */
#include <ctype.h>

#include "../core/disasm.h"
#include "../heap/heap.h"
#include "../platform/platform.h"
#include "tools.h"
#include "tools_internal.h"

typedef struct {
    Function *fn;
    uint32_t pc;
} BpAt;

/* A breakpoint: at a line of a file, or at a function's first instruction. It
 * is kept as it was asked for, and placed again whenever the modules change,
 * so one may wait for code not loaded yet, and follows a reload. */
typedef struct {
    uint32_t id;
    char *file, *function; /* one of them */
    uint32_t line;         /* as asked */
    uint32_t line_at;      /* the line with code it is at (0: nowhere yet) */
    VEC(BpAt) at;
} PBreak;

typedef struct PDebug {
    VEC(PBreak) bps;
    uint32_t next_id;
    uint64_t mods_sig; /* the modules, as they were when the breakpoints were placed */
} PDebug;

static PDebug *pdebug_of(VM *vm) {
    Tools *t = tools_of(vm);
    if (!t->pdbg) t->pdbg = xcalloc(1, sizeof *t->pdbg);
    return t->pdbg;
}

static void pbreak_free(PBreak *b) {
    free(b->file);
    free(b->function);
    VEC_FREE(b->at);
}

void pdebug_free(VM *vm) {
    Tools *t = vm->tools;
    if (!t || !t->pdbg) return;
    for (size_t i = 0; i < t->pdbg->bps.n; i++) pbreak_free(&t->pdbg->bps.v[i]);
    VEC_FREE(t->pdbg->bps);
    free(t->pdbg);
    t->pdbg = NULL;
}

/* ---- where code is ----------------------------------------------------------- */

/* An instruction's source: the translated program's line if `.loc` says, else
 * the assembly's. line 0: unknown. */
static void position(const Function *f, uint32_t pc, const char **file, uint32_t *line) {
    if (module_srcpos(f->mod->m, f->mf, pc, file, line)) return;
    *file = fn_file(f);
    *line = f->mf->lines && pc < f->mf->ninsns ? f->mf->lines[pc] : 0;
}

static bool same_file(const char *have, const char *asked) {
    return !strcmp(have, asked) || !strcmp(path_basename(have), path_basename(asked));
}

/* ---- the hook ------------------------------------------------------------------ */

/* Before each instruction of a debugged process, on whichever thread runs it:
 * true stops it there. Nothing stops inside a callback from C, which has no
 * way to wait (and may be on an I/O thread, with the world stopped). */
static bool pdebug_hook(Process *p, uint32_t pc) {
    ProcDebug *d = &p->dbg;
    if (p->cb_base) return false;
    const Function *f = p->frames[p->nframes - 1].fn;
    uint8_t why = d->hold; /* (asked to pause: here, though it be an instruction it has begun) */
    if (!why && d->skip) {
        d->skip = false;
        if (f == d->skip_fn && pc == d->skip_pc && p->nframes == d->skip_depth) {
            d->at_fn = f, d->at_pc = pc, d->at_depth = p->nframes;
            return false;
        }
    }
    if (!why && d->step) {
        bool here = d->step == STEP_INTO || (d->step == STEP_OVER && p->nframes <= d->depth0) ||
                    (d->step == STEP_OUT && p->nframes < d->depth0);
        if (here && d->by_line && d->step != STEP_OUT) { /* a line's step: until another line is reached */
            const char *file;
            uint32_t line;
            position(f, pc, &file, &line);
            here = line && (p->nframes != d->depth0 || f != d->fn0 || line != d->line0 || file != d->file0);
        }
        if (here) why = PDBG_STEP;
    }
    if (!why && f->bp && pc < f->ncode && f->bp[pc]) why = PDBG_BREAKPOINT;
    if (!why) { /* it begins this instruction (the scheduler: has it finished it, when the process next waits?) */
        d->at_fn = f, d->at_pc = pc, d->at_depth = p->nframes;
        return false;
    }
    d->hold = d->step = 0;
    d->reason = why;
    d->seq++;
    return true;
}

void pdebug_enable(VM *vm) { vm->phook = pdebug_hook; }

bool pdebug_entry(VM *vm, uint64_t pid) {
    Process *p = vm_proc_at(vm, pid);
    if (!p || vm->hook) return false;
    p->dbg.hold = PDBG_ENTRY;
    p->dbg.children = true; /* the whole program: what it spawns too */
    atomic_store(&p->dbg.on, true);
    return true;
}

/* ---- breakpoints --------------------------------------------------------------- */

static uint64_t mods_sig(VM *vm) {
    uint64_t h = vm->mods.n;
    for (size_t i = 0; i < vm->mods.n; i++) h = h * 31 + (uint64_t)vm->mods.v[i]->state * 2 + vm->mods.v[i]->replaced;
    return h;
}

static void at_add(PBreak *b, Function *f, uint32_t pc) {
    BpAt a = {f, pc};
    VEC_PUSH(b->at, a);
}

/* Where a breakpoint is, in the modules as they are: in every version of a
 * module that still runs. A line without code moves to the next that has
 * some, up to ten lines on. */
static void place(VM *vm, PBreak *b) {
    b->at.n = 0;
    b->line_at = 0;
    if (b->function) {
        const char *dot = strrchr(b->function, '.');
        for (size_t m = 0; m < vm->mods.n; m++) {
            LModule *lm = vm->mods.v[m];
            if (lm->state != LM_READY) continue;
            if (dot && (strlen(lm->m->name) != (size_t)(dot - b->function) || strncmp(lm->m->name, b->function, (size_t)(dot - b->function))))
                continue;
            for (uint32_t i = 0; i < lm->m->nfuncs; i++) {
                Function *f = &lm->funcs[i];
                if (f->target == f && f->ncode && !strcmp(f->name, dot ? dot + 1 : b->function)) at_add(b, f, 0);
            }
        }
        return;
    }
    /* A line of the assembly: the first instruction of the nearest line with code, in each module. */
    uint32_t best = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (size_t m = 0; m < vm->mods.n; m++) {
            LModule *lm = vm->mods.v[m];
            if (lm->state != LM_READY) continue;
            for (uint32_t i = 0; i < lm->m->nfuncs; i++) {
                Function *f = &lm->funcs[i];
                if (f->target != f || !f->mf->lines || !same_file(fn_file(f), b->file)) continue;
                for (uint32_t pc = 0; pc < f->ncode && pc < f->mf->ninsns; pc++) {
                    uint32_t l = f->mf->lines[pc];
                    if (l < b->line || l > b->line + 10) continue;
                    if (pass == 0) {
                        if (!best || l < best) best = l;
                    } else if (l == best) {
                        at_add(b, f, pc);
                        break; /* (a line is one function's: its first instruction) */
                    }
                }
            }
        }
        if (!best) break;
    }
    if (best) {
        b->line_at = best;
        return;
    }
    /* A line of a program a compiler translated (`.loc`): where each function begins code for it. */
    for (int pass = 0; pass < 2; pass++) {
        for (size_t m = 0; m < vm->mods.n; m++) {
            LModule *lm = vm->mods.v[m];
            if (lm->state != LM_READY) continue;
            for (uint32_t i = 0; i < lm->m->nfuncs; i++) {
                Function *f = &lm->funcs[i];
                if (f->target != f) continue;
                for (uint32_t k = 0; k < f->mf->nlocs; k++) {
                    const MLoc *l = &f->mf->locs[k];
                    if (!l->file || l->line < b->line || l->line > b->line + 10 || l->start >= f->ncode) continue;
                    if (!same_file(lm->m->srcfiles[l->file - 1], b->file)) continue;
                    if (pass == 0) {
                        if (!best || l->line < best) best = l->line;
                    } else if (l->line == best) {
                        at_add(b, f, l->start);
                        break;
                    }
                }
            }
        }
        if (!best) break;
    }
    b->line_at = best;
}

/* Every breakpoint placed again, and the functions marked: when the modules
 * have changed (a load, a reload, a purge), or the breakpoints have. */
static void place_all(VM *vm, PDebug *d) {
    for (size_t m = 0; m < vm->mods.n; m++) {
        LModule *lm = vm->mods.v[m];
        if (lm->state != LM_READY) continue;
        for (uint32_t i = 0; i < lm->m->nfuncs; i++)
            if (lm->funcs[i].bp) memset(lm->funcs[i].bp, 0, lm->funcs[i].ncode);
    }
    for (size_t i = 0; i < d->bps.n; i++) {
        PBreak *b = &d->bps.v[i];
        place(vm, b);
        for (size_t k = 0; k < b->at.n; k++) {
            Function *f = b->at.v[k].fn;
            if (!f->bp) f->bp = xcalloc(f->ncode, 1);
            f->bp[b->at.v[k].pc] = 1;
        }
    }
    d->mods_sig = mods_sig(vm);
}

void pdebug_refresh(VM *vm) {
    Tools *t = vm->tools;
    if (t && t->pdbg && t->pdbg->bps.n && t->pdbg->mods_sig != mods_sig(vm)) place_all(vm, t->pdbg);
}

/* ---- JSON ---------------------------------------------------------------------- */

static void js(StrBuf *b, const char *s) { tools_json_str(b, s); }

static void where_json(StrBuf *b, const Function *f, uint32_t pc) {
    StrBuf q = {0};
    fn_qualname(&q, f);
    sb_puts(b, "{\"function\":");
    js(b, q.s);
    sb_free(&q);
    sb_printf(b, ",\"pc\":%u,\"file\":", pc);
    js(b, fn_file(f));
    sb_printf(b, ",\"line\":%u", f->mf->lines && pc < f->mf->ninsns ? f->mf->lines[pc] : 0);
    const char *file;
    uint32_t line;
    if (module_srcpos(f->mod->m, f->mf, pc, &file, &line)) {
        sb_puts(b, ",\"source\":");
        js(b, file);
        sb_printf(b, ",\"source_line\":%u", line);
    }
    sb_putc(b, '}');
}

static void break_json(StrBuf *b, const PBreak *k) {
    sb_printf(b, "{\"id\":%u,\"file\":", k->id);
    if (k->file) js(b, k->file);
    else sb_puts(b, "null");
    sb_printf(b, ",\"line\":%u,\"line_at\":%u,\"function\":", k->line, k->line_at);
    if (k->function) js(b, k->function);
    else sb_puts(b, "null");
    sb_puts(b, ",\"at\":[");
    for (size_t i = 0; i < k->at.n; i++) {
        if (i) sb_putc(b, ',');
        where_json(b, k->at.v[i].fn, k->at.v[i].pc);
    }
    sb_puts(b, "]}");
}

static void breaks_json(StrBuf *b, PDebug *d) {
    sb_putc(b, '[');
    for (size_t i = 0; i < d->bps.n; i++) {
        if (i) sb_putc(b, ',');
        break_json(b, &d->bps.v[i]);
    }
    sb_putc(b, ']');
}

static const char *const STATES[] = {"ready", "running", "waiting", "sleeping", "done", "failed", "native", "io", "paused"};
static const char *const REASONS[] = {NULL, "pause", "entry", "step", "breakpoint", "break", "error"};

/* The frame's instruction: the one it is stopped at, or the call it is in, or
 * (paused where it failed) the one that failed, which it is past. */
static uint32_t frame_pc(const Process *p, uint32_t i) {
    const Frame *fr = &p->frames[i];
    return i == p->nframes - 1 && !p->dbg.failing ? fr->pc : (fr->pc ? fr->pc - 1 : 0);
}

#define FRAMES_MAX 200

/* A process to its debugger: whether it is debugged, its state, why it is
 * paused, and (frames) its calls, the innermost first. One in a blocking call
 * an I/O thread is still making is not to be read: where it made the call. */
static void status_json(StrBuf *b, Process *p, bool frames) {
    ProcSeen s;
    plat_lock(p->lock);
    sched_seen(p, &s);
    const ProcDebug *d = &p->dbg;
    bool paused = s.state == PROC_PAUSED;
    sb_printf(b, "{\"pid\":%llu,\"debugged\":%s,\"state\":\"%s\",\"away\":%s,\"pausing\":%s,\"stepping\":%s,\"reason\":",
              (unsigned long long)p->pid, d->on ? "true" : "false", STATES[s.state], s.away ? "true" : "false",
              d->hold && !paused ? "true" : "false", d->step && !paused ? "true" : "false");
    if (paused && d->reason < sizeof REASONS / sizeof REASONS[0] && REASONS[d->reason]) js(b, REASONS[d->reason]);
    else sb_puts(b, "null");
    sb_puts(b, ",\"error\":");
    if (paused && d->failing) { /* what it failed with, and will end with */
        sb_puts(b, "{\"code\":");
        js(b, p->err.code);
        sb_puts(b, ",\"message\":");
        js(b, p->err.msg);
        sb_putc(b, '}');
    } else {
        sb_puts(b, "null");
    }
    sb_printf(b, ",\"seq\":%llu,\"children\":%s,\"entry\":", (unsigned long long)d->seq, d->children ? "true" : "false");
    js(b, p->entry ? p->entry->name : "?");
    sb_printf(b, ",\"program\":%d,\"depth\":%u,\"at\":", p->prog ? (int)p->prog->id : -1, s.depth);
    if (s.fn) where_json(b, s.fn, s.pc && paused && d->failing ? s.pc - 1 : s.pc);
    else sb_puts(b, "null");
    if (frames) {
        sb_puts(b, ",\"frames\":[");
        if (s.away && s.fn) where_json(b, s.fn, s.pc);
        uint32_t shown = 0;
        for (uint32_t i = s.away ? 0 : p->nframes; i-- > 0 && shown < FRAMES_MAX; shown++) {
            if (shown) sb_putc(b, ',');
            where_json(b, p->frames[i].fn, frame_pc(p, i));
        }
        sb_putc(b, ']');
    }
    sb_putc(b, '}');
    plat_unlock(p->lock);
}

/* ---- variables ----------------------------------------------------------------- */

#define CHILDREN_MAX 1000

/* How many values a value opens to show (0: none). */
static uint32_t children_of(Word w, MlwsType t) {
    if (!type_is_ref(t) || !w.r) return 0;
    const Obj *o = w.r;
    switch (o->kind) {
    case OBJ_ARRAY: return ((const ObjArray *)o)->len;
    case OBJ_LIST: return ((const ObjList *)o)->len;
    case OBJ_STRUCT: return ((const ObjStruct *)o)->def->nfields;
    case OBJ_MAP: return ((const ObjMap *)o)->count;
    case OBJ_CLOSURE: return ((const ObjClosure *)o)->nup;
    default: return 0;
    }
}

static const char *kind_of(Word w, MlwsType t) {
    if (!type_is_ref(t)) return type_name(t);
    if (!w.r) return "null";
    const Obj *o = w.r;
    return o->kind == OBJ_STRUCT ? ((const ObjStruct *)o)->def->name : obj_kind_name(o->kind);
}

/* Child i of an object, with its name: false past its end. */
static bool child_at(Word w, MlwsType t, uint32_t i, Word *cw, MlwsType *ct, StrBuf *name) {
    if (i >= children_of(w, t)) return false;
    Obj *o = w.r;
    switch (o->kind) {
    case OBJ_ARRAY:
        *cw = ((ObjArray *)o)->data[i], *ct = (MlwsType)((ObjArray *)o)->elem;
        sb_printf(name, "[%u]", i);
        return true;
    case OBJ_LIST:
        *cw = ((ObjList *)o)->items[i], *ct = (MlwsType)((ObjList *)o)->tags[i];
        sb_printf(name, "[%u]", i);
        return true;
    case OBJ_STRUCT:
        *cw = ((ObjStruct *)o)->fields[i], *ct = (MlwsType)((ObjStruct *)o)->def->types[i];
        sb_puts(name, ((ObjStruct *)o)->def->fields[i]);
        return true;
    case OBJ_CLOSURE:
        *cw = ((ObjClosure *)o)->up[i], *ct = (MlwsType)((ObjClosure *)o)->tags[i];
        sb_printf(name, "UP[%u]", i);
        return true;
    case OBJ_MAP: { /* its entries in their order: the i-th that is there */
        ObjMap *m = (ObjMap *)o;
        uint32_t seen = 0;
        for (uint32_t k = 0; k < m->nentries; k++) {
            MapEntry *e = &m->entries[k];
            if (!e->live || seen++ != i) continue;
            *cw = e->val, *ct = (MlwsType)e->vtag;
            fmt_value(name, e->key, (MlwsType)e->ktag, 0);
            return true;
        }
        return false;
    }
    default: return false;
    }
}

/* {"name","value","kind","children","path"}: a value as a debugger shows it. */
static void var_json(StrBuf *b, const char *name, const char *value, const char *kind, uint32_t children, const char *path) {
    sb_puts(b, "{\"name\":");
    js(b, name);
    sb_puts(b, ",\"value\":");
    tools_json_strn(b, value ? value : "", value ? strlen(value) : 0, 4096);
    sb_puts(b, ",\"kind\":");
    js(b, kind);
    sb_printf(b, ",\"children\":%u,\"path\":", children);
    js(b, path);
    sb_putc(b, '}');
}

static void value_json(StrBuf *b, const char *name, Word w, MlwsType t, const char *path) {
    StrBuf v = {0};
    fmt_value(&v, w, t, 1);
    var_json(b, name, v.s, kind_of(w, t), children_of(w, t), path);
    sb_free(&v);
}

static MlwsType bank_type(int bank) { return bank == BANK_I ? T_I64 : bank == BANK_F ? T_F64 : bank == BANK_R ? T_R : T_P; }

/* The frame's named variables live at its instruction, as their formatters
 * show them, and its registers by bank. */
static void frame_vars_json(StrBuf *b, Process *p, uint32_t fi) {
    const Frame *fr = &p->frames[fi];
    uint32_t pc = frame_pc(p, fi);
    char path[32], name[16];
    sb_puts(b, "{\"locals\":[");
    bool first = true;
    for (uint32_t k = 0; k < fr->fn->mf->nvars; k++) {
        const MVar *v = &fr->fn->mf->vars[k];
        if (pc < v->start || pc >= v->end || dbg_var_at(fr->fn, pc, v->name) != v) continue;
        StrBuf val = {0};
        dbg_show_var(p, fr, v, 1, &val);
        Word w = {0};
        MlwsType t = T_NONE;
        if (v->nregs == 1) dbg_var_reg(p, fr, v, 0, &w, &t);
        snprintf(path, sizeof path, "L%u", k);
        if (!first) sb_putc(b, ',');
        first = false;
        var_json(b, v->name, val.s, v->nregs == 1 ? kind_of(w, t) : "registers", v->nregs == 1 ? children_of(w, t) : v->nregs, path);
        sb_free(&val);
    }
    sb_puts(b, "],\"registers\":{");
    for (int bank = 0; bank < 4; bank++) {
        sb_printf(b, "%s\"%s\":[", bank ? "," : "", bank_prefix((Bank)bank));
        for (uint32_t i = 0; i < fr->fn->mf->nregs[bank]; i++) {
            Word w;
            memcpy(&w, frame_reg(p->regs, fr, bank, (int)i), sizeof w);
            snprintf(name, sizeof name, "%s%u", bank_prefix((Bank)bank), i);
            if (i) sb_putc(b, ',');
            value_json(b, name, w, bank_type(bank), name);
        }
        sb_putc(b, ']');
    }
    sb_puts(b, "}}");
}

/* The values a path opens to show: a path is a root (L3, a named variable by
 * its number; I0, R2, ..., a register) and then, with a / before each, the
 * number of a child. False for a path that leads nowhere (the process went on). */
static bool path_children_json(StrBuf *b, Process *p, uint32_t fi, const char *path, uint32_t from) {
    const Frame *fr = &p->frames[fi];
    const char *s = path;
    Word w = {0};
    MlwsType t = T_NONE;
    const MVar *regs_of = NULL; /* a named variable of several registers: its children are they */
    char *end;
    if (*s == 'L') {
        unsigned long k = strtoul(s + 1, &end, 10);
        if (end == s + 1 || k >= fr->fn->mf->nvars) return false;
        const MVar *v = &fr->fn->mf->vars[k];
        if (v->nregs == 1) dbg_var_reg(p, fr, v, 0, &w, &t);
        else regs_of = v;
    } else {
        const char *banks = "IFRP", *bk = *s ? strchr(banks, *s) : NULL;
        if (!bk) return false;
        unsigned long i = strtoul(s + 1, &end, 10);
        if (end == s + 1 || i >= fr->fn->mf->nregs[bk - banks]) return false;
        memcpy(&w, frame_reg(p->regs, fr, (int)(bk - banks), (int)i), sizeof w);
        t = bank_type((int)(bk - banks));
    }
    for (s = end; *s == '/';) {
        unsigned long i = strtoul(s + 1, &end, 10);
        if (end == s + 1) return false;
        s = end;
        if (regs_of) {
            if (i >= regs_of->nregs) return false;
            dbg_var_reg(p, fr, regs_of, (int)i, &w, &t);
            regs_of = NULL;
            continue;
        }
        StrBuf nm = {0};
        bool ok = child_at(w, t, (uint32_t)i, &w, &t, &nm);
        sb_free(&nm);
        if (!ok) return false;
    }
    if (*s) return false;
    uint32_t n = regs_of ? regs_of->nregs : children_of(w, t);
    sb_printf(b, "{\"path\":");
    js(b, path);
    sb_printf(b, ",\"count\":%u,\"from\":%u,\"children\":[", n, from);
    StrBuf cp = {0};
    for (uint32_t i = from, shown = 0; i < n && shown < CHILDREN_MAX; i++, shown++) {
        Word cw = {0};
        MlwsType ct = T_NONE;
        StrBuf nm = {0};
        if (regs_of) {
            char rb[8];
            dbg_var_reg(p, fr, regs_of, (int)i, &cw, &ct);
            sb_puts(&nm, reg_name(regs_of->regs[i], rb));
        } else if (!child_at(w, t, i, &cw, &ct, &nm)) {
            sb_free(&nm);
            break;
        }
        cp.n = 0;
        sb_printf(&cp, "%s/%u", path, i);
        if (shown) sb_putc(b, ',');
        value_json(b, nm.s ? nm.s : "", cw, ct, cp.s);
        sb_free(&nm);
    }
    sb_free(&cp);
    sb_puts(b, "]}");
    return true;
}

/* A frame's function as its instructions, each with the lines it came from. */
static void code_json(StrBuf *b, const Process *p, uint32_t fi) {
    const Function *f = p->frames[fi].fn;
    StrBuf q = {0};
    fn_qualname(&q, f);
    sb_puts(b, "{\"function\":");
    js(b, q.s);
    sb_free(&q);
    sb_printf(b, ",\"pc\":%u,\"file\":", frame_pc(p, fi));
    js(b, fn_file(f));
    sb_puts(b, ",\"insns\":[");
    for (uint32_t pc = 0; pc < f->ncode; pc++) {
        StrBuf t = {0};
        disasm_insn(&t, f->mod->m, &f->code[pc]);
        sb_printf(b, "%s{\"pc\":%u,\"line\":%u,\"text\":", pc ? "," : "", pc, f->mf->lines && pc < f->mf->ninsns ? f->mf->lines[pc] : 0);
        js(b, t.s ? t.s : "");
        sb_free(&t);
        const char *file;
        uint32_t line;
        if (module_srcpos(f->mod->m, f->mf, pc, &file, &line)) {
            sb_puts(b, ",\"source\":");
            js(b, file);
            sb_printf(b, ",\"source_line\":%u", line);
        }
        sb_printf(b, ",\"breakpoint\":%s}", f->bp && f->bp[pc] ? "true" : "false");
    }
    sb_puts(b, "]}");
}

/* ---- sources ------------------------------------------------------------------- */

#define SOURCE_MAX ((size_t)4 << 20)

/* The module some loaded code in `file` belongs to: a module's assembly, or a
 * file its `.loc`s name. Only such files are read, for whoever may debug. */
static const LModule *source_module(VM *vm, const char *file) {
    for (size_t m = 0; m < vm->mods.n; m++) {
        LModule *lm = vm->mods.v[m];
        if (lm->state != LM_READY) continue;
        for (uint32_t i = 0; i < lm->m->nsrcfiles; i++)
            if (!strcmp(lm->m->srcfiles[i], file)) return lm;
        for (uint32_t i = 0; i < lm->m->nfuncs; i++)
            if (lm->funcs[i].target == &lm->funcs[i] && lm->funcs[i].ncode && !strcmp(fn_file(&lm->funcs[i]), file)) return lm;
    }
    return NULL;
}

/* Its text: the file as named, or (a compiler names a program's file as it was
 * given it) one of that name beside the module's own file. NULL if neither reads. */
static char *source_text(const LModule *lm, const char *file, size_t *len) {
    char *text = read_file(file, len);
    if (text || !lm->path) return text;
    char *dir = path_dirname(lm->path), *beside = path_join(dir, file);
    text = read_file(beside, len);
    free(dir);
    free(beside);
    return text;
}

/* ---- requests ------------------------------------------------------------------ */

static int fail(StrBuf *out, int status, const char *msg) {
    out->n = 0;
    sb_puts(out, "{\"error\":");
    js(out, msg);
    sb_putc(out, '}');
    return status;
}

/* A parameter of a query (a=1&b=two), decoded, into out: false if it is not there. */
static bool qparam(const char *qs, const char *name, char *out, size_t n) {
    size_t k = strlen(name);
    for (const char *s = qs; s && *s; s = strchr(s, '&') ? strchr(s, '&') + 1 : NULL) {
        if (strncmp(s, name, k) || s[k] != '=') continue;
        size_t m = strcspn(s + k + 1, "&");
        if (m >= n) m = n - 1;
        memcpy(out, s + k + 1, m);
        out[m] = 0;
        tools_url_decode(out);
        return true;
    }
    return false;
}

static const char *unavailable(VM *vm) {
    return vm->hook ? "this VM runs under a tool that sees every instruction (the profiler, the tracer or mlwsvm debug)" : NULL;
}

/* The process a path names (/PID...), and the rest of the path. */
static Process *path_proc(VM *vm, const char **path) {
    char *end;
    if (**path != '/' || !isdigit((unsigned char)(*path)[1])) return NULL;
    uint64_t pid = strtoull(*path + 1, &end, 10);
    *path = end;
    return vm_proc_at(vm, pid);
}

/* The frame a query names (frame=N, the innermost 0), as its place among the
 * process's frames: false if there is none. */
static bool query_frame(Process *p, const char *qs, uint32_t *fi) {
    char v[24];
    unsigned long n = qparam(qs, "frame", v, sizeof v) ? strtoul(v, NULL, 10) : 0;
    if (n >= p->nframes) return false;
    *fi = p->nframes - 1 - (uint32_t)n;
    return true;
}

/* May p's frames and values be read? Not while an I/O thread makes a call of its. */
static bool readable(Process *p) {
    ProcSeen s;
    plat_lock(p->lock);
    sched_seen(p, &s);
    plat_unlock(p->lock);
    return !s.away;
}

int pdebug_get(VM *vm, const char *path, const char *qs, StrBuf *out) {
    PDebug *d = pdebug_of(vm);
    pdebug_refresh(vm);
    if (!*path) { /* the breakpoints, and each process debugged */
        const char *why = unavailable(vm);
        sb_printf(out, "{\"available\":%s,\"why\":", why ? "false" : "true");
        if (why) js(out, why);
        else sb_puts(out, "null");
        sb_puts(out, ",\"breakpoints\":");
        breaks_json(out, d);
        sb_puts(out, ",\"processes\":[");
        bool first = true;
        for (uint64_t pid = 1, end = vm_pid_end(vm); pid < end; pid++) {
            Process *p = vm_proc_at(vm, pid);
            if (!p || !p->dbg.on) continue;
            if (!first) sb_putc(out, ',');
            first = false;
            status_json(out, p, false);
        }
        sb_puts(out, "]}");
        return 200;
    }
    if (!strcmp(path, "/breakpoints")) {
        breaks_json(out, d);
        return 200;
    }
    if (!strcmp(path, "/source")) {
        char file[1024];
        if (!qparam(qs, "file", file, sizeof file)) return fail(out, 400, "file=NAME is needed");
        const LModule *lm = source_module(vm, file);
        if (!lm) return fail(out, 404, "no code loaded came from that file");
        size_t len = 0;
        char *text = source_text(lm, file, &len);
        if (!text) return fail(out, 404, "the file cannot be read: it may have moved since the code was made");
        if (len > SOURCE_MAX) {
            free(text);
            return fail(out, 413, "the file is over 4 MiB");
        }
        sb_puts(out, "{\"file\":");
        js(out, file);
        sb_puts(out, ",\"text\":");
        tools_json_strn(out, text, len, SOURCE_MAX);
        sb_putc(out, '}');
        free(text);
        return 200;
    }
    Process *p = path_proc(vm, &path);
    if (!p) return fail(out, 404, "no such process");
    if (!*path) {
        status_json(out, p, true);
        return 200;
    }
    if (strcmp(path, "/vars") && strcmp(path, "/code")) return fail(out, 404, "no such thing to read");
    if (!readable(p)) return fail(out, 409, "the process is in a blocking call: it can be read when the call returns");
    uint32_t fi;
    if (!query_frame(p, qs, &fi)) return fail(out, 404, "the process has no such frame");
    if (!strcmp(path, "/code")) {
        code_json(out, p, fi);
        return 200;
    }
    char vpath[256], from[24];
    if (!qparam(qs, "path", vpath, sizeof vpath)) {
        frame_vars_json(out, p, fi);
        return 200;
    }
    uint32_t at = qparam(qs, "from", from, sizeof from) ? (uint32_t)strtoul(from, NULL, 10) : 0;
    if (!path_children_json(out, p, fi, vpath, at)) return fail(out, 404, "there is no such value now");
    return 200;
}

/* A paused process goes on: the instruction it is stopped at runs first. */
static void go_on(VM *vm, Process *p) {
    ProcDebug *d = &p->dbg;
    if (p->nframes) {
        d->skip = true;
        d->skip_fn = p->frames[p->nframes - 1].fn;
        d->skip_pc = p->frames[p->nframes - 1].pc;
        d->skip_depth = p->nframes;
    }
    d->reason = PDBG_NONE;
    sched_resume(vm, p->pid);
}

/* Does a pause or a continue ask for the whole program: {"program": true}? */
static bool whole_program(const char *body, size_t n) {
    static const char *const NAMES[] = {"program"};
    char *v[1] = {0};
    bool yes = n && tools_json_flat(body, n, NAMES, v, 1) && v[0] && !strcmp(v[0], "true");
    free(v[0]);
    return yes;
}

static void event(const char **ev, StrBuf *audit, const char *name, uint64_t pid) {
    *ev = name;
    sb_printf(audit, "{\"pid\":%llu}", (unsigned long long)pid);
}

int pdebug_post(VM *vm, const char *path, const char *body, size_t n, StrBuf *out, const char **ev, StrBuf *audit) {
    PDebug *d = pdebug_of(vm);
    const char *why = unavailable(vm);
    if (why) return fail(out, 409, why);
    pdebug_refresh(vm);
    if (!strcmp(path, "/breakpoints")) {
        static const char *const NAMES[] = {"file", "line", "function"};
        char *v[3] = {0};
        bool ok = tools_json_flat(body, n, NAMES, v, 3) && ((v[0] && v[1] && !v[2]) || (v[2] && !v[0] && !v[1]));
        uint32_t line = ok && v[1] ? (uint32_t)strtoul(v[1], NULL, 10) : 0;
        if (!ok || (v[1] && !line) || (v[2] && !*v[2])) {
            for (int i = 0; i < 3; i++) free(v[i]);
            return fail(out, 400, "expected {\"file\": NAME, \"line\": N} or {\"function\": NAME}");
        }
        for (size_t i = 0; i < d->bps.n; i++) { /* the same again: the one there */
            PBreak *k = &d->bps.v[i];
            if ((v[2] && k->function && !strcmp(k->function, v[2])) || (v[0] && k->file && !strcmp(k->file, v[0]) && k->line == line)) {
                for (int j = 0; j < 3; j++) free(v[j]);
                break_json(out, k);
                return 200;
            }
        }
        if (d->bps.n >= 1000) {
            for (int i = 0; i < 3; i++) free(v[i]);
            return fail(out, 409, "there are a thousand breakpoints already");
        }
        PBreak k = {.id = ++d->next_id, .file = v[0], .function = v[2], .line = line};
        free(v[1]);
        VEC_PUSH(d->bps, k);
        place_all(vm, d);
        break_json(out, &d->bps.v[d->bps.n - 1]);
        *ev = "monitor.debug_break";
        sb_puts(audit, "{");
        if (k.file) {
            sb_puts(audit, "\"file\":");
            js(audit, k.file);
            sb_printf(audit, ",\"line\":%u}", line);
        } else {
            sb_puts(audit, "\"function\":");
            js(audit, k.function);
            sb_putc(audit, '}');
        }
        return 200;
    }
    unsigned id;
    char verb[16] = "";
    if (sscanf(path, "/breakpoints/%u/%15[a-z]", &id, verb) == 2 && !strcmp(verb, "delete")) {
        for (size_t i = 0; i < d->bps.n; i++) {
            if (d->bps.v[i].id != id) continue;
            pbreak_free(&d->bps.v[i]);
            for (size_t j = i + 1; j < d->bps.n; j++) d->bps.v[j - 1] = d->bps.v[j];
            d->bps.n--;
            place_all(vm, d);
            sb_puts(out, "{\"ok\":true}");
            return 200;
        }
        return fail(out, 404, "no such breakpoint");
    }
    Process *p = path_proc(vm, &path);
    if (!p) return fail(out, 404, "no such process");
    ProcDebug *pd = &p->dbg;
    plat_lock(p->lock);
    int state = p->state;
    plat_unlock(p->lock);
    if (state == PROC_DONE || state == PROC_FAILED) return fail(out, 409, "the process has ended");
    if (!strcmp(path, "/attach")) {
        if (p->prog && !strcmp(p->prog->path, "(serve)")) return fail(out, 403, "that process is mlwsvm serve's own: it is not to be debugged");
        static const char *const NAMES[] = {"pause", "children"};
        char *v[2] = {0};
        if (n && !tools_json_flat(body, n, NAMES, v, 2)) return fail(out, 400, "expected {\"pause\": true, \"children\": true}, either, or nothing");
        bool pause = v[0] && !strcmp(v[0], "true");
        if (v[1]) pd->children = !strcmp(v[1], "true");
        free(v[0]);
        free(v[1]);
        bool was = atomic_exchange(&pd->on, true);
        if (pause && state != PROC_PAUSED) pd->hold = PDBG_PAUSE;
        if (!was) event(ev, audit, "monitor.debug_attach", p->pid);
    } else if (!strcmp(path, "/detach")) {
        bool was = atomic_exchange(&pd->on, false);
        pd->hold = pd->step = 0;
        pd->skip = pd->children = false;
        if (state == PROC_PAUSED) go_on(vm, p);
        if (was) event(ev, audit, "monitor.debug_detach", p->pid);
    } else if (whole_program(body, n)) { /* {"program": true}: every process of its program */
        bool pause = !strcmp(path, "/pause");
        if (!pause && strcmp(path, "/continue")) return fail(out, 404, "no such action on a program");
        if (!p->prog || !strcmp(p->prog->path, "(serve)")) return fail(out, 403, "that process is of no program to pause");
        for (uint64_t q = 1, end = vm_pid_end(vm); q < end; q++) {
            Process *o = vm_proc_at(vm, q);
            if (!o || o->prog != p->prog) continue;
            plat_lock(o->lock);
            int os = o->state;
            plat_unlock(o->lock);
            if (os == PROC_DONE || os == PROC_FAILED) continue;
            if (pause) { /* each debugged from now, and what it spawns: nothing of the program runs on */
                o->dbg.children = true;
                atomic_store(&o->dbg.on, true);
                if (os != PROC_PAUSED) o->dbg.hold = PDBG_PAUSE;
            } else if (os == PROC_PAUSED) {
                go_on(vm, o);
            } else {
                o->dbg.hold = 0; /* (asked to pause, and not there yet: no longer) */
            }
        }
        *ev = pause ? "monitor.debug_pause" : NULL;
        if (pause) sb_printf(audit, "{\"program\":%u}", p->prog->id);
    } else if (!pd->on) {
        return fail(out, 409, "the process is not being debugged: attach first");
    } else if (!strcmp(path, "/pause")) {
        if (state != PROC_PAUSED) pd->hold = PDBG_PAUSE;
        event(ev, audit, "monitor.debug_pause", p->pid);
    } else if (!strcmp(path, "/continue")) {
        if (state != PROC_PAUSED && !pd->hold) return fail(out, 409, "the process is not paused");
        if (state == PROC_PAUSED) go_on(vm, p);
        else pd->hold = 0; /* (asked to pause, and not there yet: no longer) */
    } else if (!strcmp(path, "/step")) {
        static const char *const NAMES[] = {"kind", "by"};
        char *v[2] = {0};
        bool ok = tools_json_flat(body, n, NAMES, v, 2);
        int kind = !ok || !v[0] ? 0 : !strcmp(v[0], "into") ? STEP_INTO : !strcmp(v[0], "over") ? STEP_OVER : !strcmp(v[0], "out") ? STEP_OUT : 0;
        int by = !ok ? -1 : !v[1] || !strcmp(v[1], "line") ? 1 : !strcmp(v[1], "insn") ? 0 : -1;
        free(v[0]);
        free(v[1]);
        if (!kind || by < 0) return fail(out, 400, "expected {\"kind\": \"into\" | \"over\" | \"out\", \"by\": \"line\" | \"insn\"}");
        if (state != PROC_PAUSED) return fail(out, 409, "the process is not paused");
        if (pd->failing) return fail(out, 409, "the process has failed: it can only go on to its end (continue)");
        if (!p->nframes) return fail(out, 409, "the process has not begun");
        const Frame *fr = &p->frames[p->nframes - 1];
        pd->step = (uint8_t)kind;
        pd->by_line = by == 1;
        pd->depth0 = p->nframes;
        pd->fn0 = fr->fn;
        position(fr->fn, fr->pc, &pd->file0, &pd->line0);
        go_on(vm, p);
    } else {
        return fail(out, 404, "no such action");
    }
    out->n = 0;
    status_json(out, p, false);
    return 200;
}
