/* debugger.c - `mlwsvm debug`: an interactive, gdb-like debugger. It runs the
 * program under the scheduler with the instruction hook installed, stopping at
 * breakpoints and after steps. Commands come from stdin (or a script, which is
 * how tests drive it); everything is printed through one output callback so a
 * transcript interleaves correctly with the program's own output.
 * See docs/TOOLING.md for the command reference. */
#include <ctype.h>

#include "../core/disasm.h"
#include "../heap/heap.h"
#include "tools.h"
#include "tools_internal.h"

enum { STEP_NONE, STEP_INTO, STEP_OVER, STEP_OUT };

typedef struct {
    char *spec;
    Function *fn;
    uint32_t pc;
    uint64_t hits;
    bool live;
} Breakpoint;

typedef struct Debugger {
    VM *vm;
    Process *main;
    Function *main_fn;
    LModule *root;
    VEC(Breakpoint) bps;
    int step;
    uint64_t step_pid;
    uint32_t step_depth;
    /* After a stop, the stopped instruction must run once before it can stop again. */
    bool skip;
    uint64_t skip_pid;
    Function *skip_fn;
    uint32_t skip_pc;
    int hit;            /* breakpoint that caused the stop, or -1 */
    uint64_t sel;       /* pid of the process being inspected */
    bool started, ended;
    OutputFn out;
    void *out_ctx;
} Debugger;

static void say(Debugger *d, const char *fmt, ...) MLWS_PRINTF(2, 3);
static void say(Debugger *d, const char *fmt, ...) {
    StrBuf b = {0};
    va_list ap;
    va_start(ap, fmt);
    sb_vprintf(&b, fmt, ap);
    va_end(ap);
    d->out(d->out_ctx, b.s ? b.s : "", b.n);
    sb_free(&b);
}

static void say_sb(Debugger *d, StrBuf *b) {
    d->out(d->out_ctx, b->s ? b->s : "", b->n);
    sb_free(b);
}

/* ---- the hook ---------------------------------------------------------------- */

bool debugger_hook(Debugger *d, Process *p, uint32_t pc) {
    Function *f = p->frames[p->nframes - 1].fn;
    if (d->skip) {
        d->skip = false;
        if (p->pid == d->skip_pid && f == d->skip_fn && pc == d->skip_pc) return false;
    }
    bool stop = false;
    d->hit = -1;
    if (p->pid == d->step_pid) {
        if (d->step == STEP_INTO) stop = true;
        else if (d->step == STEP_OVER && p->nframes <= d->step_depth) stop = true;
        else if (d->step == STEP_OUT && p->nframes < d->step_depth) stop = true;
    }
    if (f->bp && f->bp[pc]) {
        for (size_t i = 0; i < d->bps.n; i++) {
            Breakpoint *b = &d->bps.v[i];
            if (b->live && b->fn == f && b->pc == pc) {
                b->hits++;
                d->hit = (int)i;
                stop = true;
            }
        }
    }
    if (stop) {
        d->step = STEP_NONE;
        d->sel = p->pid;
    }
    return stop;
}

/* ---- helpers ------------------------------------------------------------------- */

static Process *selected(Debugger *d) {
    Process *p = vm_process(d->vm, d->sel);
    if (!p && d->sel == d->main->pid && d->started) p = d->main; /* main is kept after it ends */
    return p && p->nframes ? p : NULL;
}

static void show_insn(Debugger *d, const Function *f, uint32_t pc, bool mark) {
    StrBuf b = {0};
    uint32_t line = f->mf->lines && pc < f->mf->ninsns ? f->mf->lines[pc] : 0;
    sb_printf(&b, "%s %4u  line %-4u  ", mark ? "=>" : "  ", pc, line);
    disasm_insn(&b, f->mod->m, &f->code[pc]);
    sb_putc(&b, '\n');
    say_sb(d, &b);
}

static void show_stop(Debugger *d) {
    Process *p = selected(d);
    if (!p) return;
    const Frame *fr = &p->frames[p->nframes - 1];
    StrBuf b = {0};
    if (d->hit >= 0) sb_printf(&b, "Breakpoint %d, ", d->hit + 1);
    sb_printf(&b, "process %llu at ", (unsigned long long)p->pid);
    fmt_location(&b, fr->fn, fr->pc);
    sb_putc(&b, '\n');
    say_sb(d, &b);
    show_insn(d, fr->fn, fr->pc, true);
}

static Function *find_function(Debugger *d, const char *name) {
    const char *dot = strrchr(name, '.');
    for (size_t m = 0; m < d->vm->mods.n; m++) {
        LModule *lm = d->vm->mods.v[m];
        bool root = lm == d->root;
        for (uint32_t i = 0; i < lm->m->nfuncs; i++) {
            Function *f = &lm->funcs[i];
            if (f->target != f) continue;
            if (!dot && root && !strcmp(f->name, name)) return f;
            if (dot && !strcmp(f->name, dot + 1) && strlen(lm->m->name) == (size_t)(dot - name) &&
                !strncmp(lm->m->name, name, (size_t)(dot - name)))
                return f;
        }
    }
    if (!dot) { /* an unqualified name defined only in another module */
        for (size_t m = 0; m < d->vm->mods.n; m++) {
            LModule *lm = d->vm->mods.v[m];
            for (uint32_t i = 0; i < lm->m->nfuncs; i++)
                if (lm->funcs[i].target == &lm->funcs[i] && !strcmp(lm->funcs[i].name, name)) return &lm->funcs[i];
        }
    }
    return NULL;
}

static bool source_matches(const LModule *lm, const char *file) {
    const char *src = lm->m->source && *lm->m->source ? lm->m->source : lm->path;
    return !strcmp(src, file) || !strcmp(path_basename(src), path_basename(file));
}

static void add_bp(Debugger *d, const char *spec, Function *f, uint32_t pc) {
    if (!f->bp) f->bp = xcalloc(f->ncode, 1);
    f->bp[pc] = 1;
    Breakpoint b = {xstrdup(spec), f, pc, 0, true};
    VEC_PUSH(d->bps, b);
    StrBuf s = {0};
    sb_printf(&s, "Breakpoint %zu at ", d->bps.n);
    fmt_location(&s, f, pc);
    sb_putc(&s, '\n');
    say_sb(d, &s);
}

static void cmd_break(Debugger *d, const char *arg) {
    if (!*arg) {
        say(d, "usage: break FUNCTION | LINE | FILE:LINE\n");
        return;
    }
    const char *colon = strrchr(arg, ':');
    bool all_digits = true;
    for (const char *c = arg; *c; c++) all_digits = all_digits && isdigit((unsigned char)*c);
    if (!colon && !all_digits) {
        Function *f = find_function(d, arg);
        if (!f) say(d, "No function named '%s'.\n", arg);
        else add_bp(d, arg, f, 0);
        return;
    }
    uint32_t line = (uint32_t)strtoul(colon ? colon + 1 : arg, NULL, 10);
    char *file = colon ? xstrndup(arg, (size_t)(colon - arg)) : NULL;
    bool found = false;
    for (size_t m = 0; m < d->vm->mods.n && !found; m++) {
        LModule *lm = d->vm->mods.v[m];
        if (file ? !source_matches(lm, file) : lm != d->root) continue;
        for (uint32_t i = 0; i < lm->m->nfuncs && !found; i++) {
            Function *f = &lm->funcs[i];
            if (f->target != f || !f->mf->lines) continue;
            for (uint32_t pc = 0; pc < f->ncode; pc++) {
                if (f->mf->lines[pc] == line) {
                    add_bp(d, arg, f, pc);
                    found = true;
                    break;
                }
            }
        }
    }
    if (!found) say(d, "No code at line %u%s%s.\n", line, file ? " of " : "", file ? file : "");
    free(file);
}

static void cmd_delete(Debugger *d, const char *arg) {
    for (size_t i = 0; i < d->bps.n; i++) {
        Breakpoint *b = &d->bps.v[i];
        if (*arg && (size_t)atoi(arg) != i + 1) continue;
        if (b->live) {
            b->live = false;
            b->fn->bp[b->pc] = 0;
            for (size_t j = 0; j < d->bps.n; j++) /* another breakpoint on the same spot */
                if (d->bps.v[j].live && d->bps.v[j].fn == b->fn && d->bps.v[j].pc == b->pc) b->fn->bp[b->pc] = 1;
        }
    }
    say(d, *arg ? "Deleted breakpoint %s.\n" : "Deleted all breakpoints.\n", arg);
}

static void cmd_info_break(Debugger *d) {
    bool any = false;
    for (size_t i = 0; i < d->bps.n; i++) {
        Breakpoint *b = &d->bps.v[i];
        if (!b->live) continue;
        any = true;
        StrBuf s = {0};
        sb_printf(&s, "%3zu  %-20s ", i + 1, b->spec);
        fmt_location(&s, b->fn, b->pc);
        sb_printf(&s, "  hits %llu\n", (unsigned long long)b->hits);
        say_sb(d, &s);
    }
    if (!any) say(d, "No breakpoints.\n");
}

static bool parse_reg(const char *s, uint8_t *reg) {
    if (!strcmp(s, "SP")) return *reg = REG_MAKE(BANK_S, SREG_SP), true;
    if (!strcmp(s, "FP")) return *reg = REG_MAKE(BANK_S, SREG_FP), true;
    if (!strcmp(s, "PID")) return *reg = REG_MAKE(BANK_S, SREG_PID), true;
    const char *banks = "IFRP";
    const char *bk = s[0] ? strchr(banks, s[0]) : NULL;
    if (!bk || !isdigit((unsigned char)s[1])) return false;
    char *end;
    long idx = strtol(s + 1, &end, 10);
    if (*end || idx < 0 || idx >= NREGS) return false;
    *reg = REG_MAKE((int)(bk - banks), (int)idx);
    return true;
}

static void reg_value(const Process *p, uint8_t reg, Word *w, MlwsType *t) {
    const Frame *fr = &p->frames[p->nframes - 1];
    int b = REG_BANK(reg), i = REG_IDX(reg);
    switch (b) {
    case BANK_I: w->i = *(const int64_t *)frame_reg(p->regs, fr, b, i); *t = T_I64; break;
    case BANK_F: w->f = *(const double *)frame_reg(p->regs, fr, b, i); *t = T_F64; break;
    case BANK_R: w->r = *(Obj *const *)frame_reg(p->regs, fr, b, i); *t = T_R; break;
    case BANK_P: w->p = *frame_reg(p->regs, fr, b, i); *t = T_P; break;
    default:
        *t = i == SREG_PID ? T_I64 : T_P;
        w->u = i == SREG_SP ? p->sp : i == SREG_FP ? p->fp : p->pid;
        break;
    }
}

static void cmd_print(Debugger *d, const char *arg) {
    Process *p = selected(d);
    uint8_t reg;
    if (!p) {
        say(d, "The program is not stopped in a process.\n");
        return;
    }
    if (!parse_reg(arg, &reg)) {
        say(d, "usage: print REGISTER   (I0..I31, F0..F31, R0..R31, P0..P31, SP, FP, PID)\n");
        return;
    }
    const Frame *fr = &p->frames[p->nframes - 1];
    int b = REG_BANK(reg);
    if (b < NBANKS && REG_IDX(reg) >= fr->fn->mf->nregs[b]) {
        say(d, "%s is outside %s's register window (%u %s registers).\n", arg, fr->fn->name, fr->fn->mf->nregs[b],
            bank_prefix((Bank)b));
        return;
    }
    Word w;
    MlwsType t;
    reg_value(p, reg, &w, &t);
    StrBuf s = {0};
    sb_printf(&s, "%s = ", arg);
    fmt_value(&s, w, t, 3);
    if (t == T_I64 && (w.i < -9 || w.i > 9)) sb_printf(&s, "  (0x%llx)", (unsigned long long)w.u);
    sb_putc(&s, '\n');
    say_sb(d, &s);
}

static void cmd_regs(Debugger *d) {
    Process *p = selected(d);
    if (!p) {
        say(d, "The program is not stopped in a process.\n");
        return;
    }
    const Frame *fr = &p->frames[p->nframes - 1];
    for (int b = 0; b < NBANKS; b++) {
        uint32_t n = fr->fn->mf->nregs[b];
        if (!n) continue;
        StrBuf s = {0};
        for (uint32_t i = 0; i < n; i++) {
            Word w;
            MlwsType t;
            reg_value(p, REG_MAKE(b, (int)i), &w, &t);
            sb_printf(&s, "%s%s%u=", i % 4 ? "  " : (i ? "\n" : ""), bank_prefix((Bank)b), i);
            fmt_value(&s, w, t, 1);
        }
        sb_putc(&s, '\n');
        say_sb(d, &s);
    }
    StrBuf s = {0};
    sb_puts(&s, "SP=");
    fmt_value(&s, (Word){.p = p->sp}, T_P, 0);
    sb_puts(&s, "  FP=");
    fmt_value(&s, (Word){.p = p->fp}, T_P, 0);
    sb_printf(&s, "  PID=%llu\n", (unsigned long long)p->pid);
    say_sb(d, &s);
}

static void cmd_stack(Debugger *d, const char *arg) {
    Process *p = selected(d);
    if (!p) {
        say(d, "The program is not stopped in a process.\n");
        return;
    }
    if (PTR_SEG(p->sp) != SEG_STACK) {
        say(d, "SP does not point into the data stack.\n");
        return;
    }
    uint32_t n = *arg ? (uint32_t)atoi(arg) : 8, off = PTR_OFF(p->sp);
    if (off >= p->stack.size) {
        say(d, "The data stack is empty.\n");
        return;
    }
    for (uint32_t i = 0; i < n && off + i * 8 + 8 <= p->stack.size; i++) {
        uint32_t o = off + i * 8;
        uint8_t tag = p->stack.tags[o / 8];
        Word w;
        memcpy(&w, p->stack.data + o, 8);
        StrBuf s = {0};
        sb_printf(&s, "SP[%u] = ", i);
        if (tag == TAG_RAW) sb_printf(&s, "0x%016llx (raw bytes)", (unsigned long long)w.u);
        else {
            MlwsType t = tag == T_NONE ? T_I64 : (MlwsType)tag;
            if (t == T_F32) { /* f32 values occupy the low 4 bytes */
                float fv;
                memcpy(&fv, p->stack.data + o, 4);
                w.f = fv;
            }
            fmt_value(&s, w, t, 1);
            sb_printf(&s, "  (%s)", tag == T_NONE ? "untyped" : type_name(t));
        }
        sb_putc(&s, '\n');
        say_sb(d, &s);
    }
}

static void cmd_bt(Debugger *d) {
    Process *p = selected(d);
    if (!p) {
        say(d, "The program is not stopped in a process.\n");
        return;
    }
    for (uint32_t i = p->nframes; i-- > 0;) {
        const Frame *fr = &p->frames[i];
        uint32_t pc = i == p->nframes - 1 ? fr->pc : (fr->pc ? fr->pc - 1 : 0);
        StrBuf s = {0};
        sb_printf(&s, "#%u  ", p->nframes - 1 - i);
        fmt_location(&s, fr->fn, pc);
        sb_putc(&s, '\n');
        say_sb(d, &s);
    }
}

static void cmd_list(Debugger *d) {
    Process *p = selected(d);
    if (!p) {
        say(d, "The program is not stopped in a process.\n");
        return;
    }
    const Frame *fr = &p->frames[p->nframes - 1];
    uint32_t from = fr->pc > 4 ? fr->pc - 4 : 0, to = fr->pc + 5 < fr->fn->ncode ? fr->pc + 5 : fr->fn->ncode;
    say(d, "%s.%s:\n", fr->fn->mod->m->name, fr->fn->name);
    for (uint32_t pc = from; pc < to; pc++) show_insn(d, fr->fn, pc, pc == fr->pc);
}

static void cmd_procs(Debugger *d) {
    static const char *const states[] = {"ready", "running", "waiting", "sleeping", "done", "failed", "native", "io"};
    say(d, "  PID  STATE     INSNS       MAILBOX  FUNCTION\n");
    for (uint64_t pid = 1, end = vm_pid_end(d->vm); pid < end; pid++) {
        Process *p = vm_proc_at(d->vm, pid);
        if (!p || !p->nframes) continue;
        const Frame *fr = &p->frames[p->nframes - 1];
        say(d, "%s%4llu  %-8s  %-10llu  %-7zu  %s.%s\n", p->pid == d->sel ? "*" : " ", (unsigned long long)p->pid,
            states[p->state], (unsigned long long)p->insns, (size_t)p->mcount, fr->fn->mod->m->name,
            fr->fn->name);
    }
}

static void report_end(Debugger *d) {
    d->ended = true;
    Process *m = d->main;
    if (m->state == PROC_DONE) {
        say(d, "Program exited with code %lld.\n", (long long)m->exit_code);
    } else {
        say(d, "Program failed: [%s] %s\n", m->err.code, m->err.msg);
        if (m->backtrace.s && *m->backtrace.s) say(d, "%s", m->backtrace.s);
    }
}

/* Run (or resume) until the next stop or the end of the program. */
static void go(Debugger *d, int step) {
    if (d->ended) {
        say(d, "The program has finished.\n");
        return;
    }
    if (!d->started) {
        if (step != STEP_NONE) {
            say(d, "The program is not running. Use 'run' first.\n");
            return;
        }
        d->started = true;
        proc_start(d->main, d->main_fn, NULL, NULL, 0);
    }
    Process *p = selected(d); /* stepping follows the process being inspected */
    d->step = step;
    d->step_pid = p ? p->pid : 0;
    d->step_depth = p ? p->nframes : 0;
    /* The process that resumes first is the one that stopped: let the instruction
     * it is stopped at run once before it can stop there again. */
    Process *r = d->vm->stopped ? d->vm->stopped : p;
    if (r && r->nframes) {
        d->skip = true;
        d->skip_pid = r->pid;
        d->skip_fn = r->frames[r->nframes - 1].fn;
        d->skip_pc = r->frames[r->nframes - 1].pc;
    }
    d->hit = -1;
    if (sched_run(d->vm, d->main)) {
        if (d->vm->stopped) d->sel = d->vm->stopped->pid; /* hook stops and `break` alike */
        show_stop(d);
    } else {
        report_end(d);
    }
}

static const char *HELP =
    "Commands (unambiguous prefixes work too, as do the gdb short forms):\n"
    "  run (r)              start the program\n"
    "  continue (c)         resume until a breakpoint or the end\n"
    "  step (s)             execute one instruction, entering calls\n"
    "  next (n)             execute one instruction, stepping over calls\n"
    "  finish (fin)         run until the current function returns\n"
    "  break (b) SPEC       breakpoint at a FUNCTION, LINE (main file) or FILE:LINE\n"
    "  delete (d) [N]       delete breakpoint N, or all\n"
    "  info break           list breakpoints\n"
    "  print (p) REG        show a register: I3, F0, R1 (objects are expanded), P2, SP, FP, PID\n"
    "  regs                 all registers of the current frame\n"
    "  stack [N]            the top N data-stack words with their types\n"
    "  bt                   backtrace\n"
    "  list (l)             disassemble around the current instruction\n"
    "  procs                list processes (* = the one being inspected)\n"
    "  process PID          inspect another process\n"
    "  quit (q)             stop the program and exit\n";

int debugger_run(VM *vm, LModule *root, Function *main_fn, DebugIO *io) {
    Debugger d;
    memset(&d, 0, sizeof d);
    d.vm = vm;
    d.root = root;
    d.main_fn = main_fn;
    d.out = io->out;
    d.out_ctx = io->out_ctx;
    d.hit = -1;
    tools_enable_hook(vm);
    Tools *t = tools_of(vm);
    t->dbg = &d;
    d.main = proc_new(vm);
    vm->main_pid = d.main->pid;
    d.sel = d.main->pid;
    say(&d, "MLWSVM debugger. Type 'help' for commands.\n");
    char line[1024];
    for (;;) {
        if (io->echo) say(&d, "(mdb) ");
        if (!io->read_line(io->in_ctx, line, sizeof line)) {
            if (io->echo) say(&d, "\n");
            break;
        }
        size_t n = strlen(line);
        while (n && isspace((unsigned char)line[n - 1])) line[--n] = 0;
        char *cmd = line;
        while (isspace((unsigned char)*cmd)) cmd++;
        if (io->echo_commands) say(&d, "%s\n", cmd);
        char *arg = cmd;
        while (*arg && !isspace((unsigned char)*arg)) arg++;
        if (*arg) *arg++ = 0;
        while (isspace((unsigned char)*arg)) arg++;
#define IS(full, shortest) (strlen(cmd) >= strlen(shortest) && !strncmp(full, cmd, strlen(cmd)))
        if (!*cmd) continue;
        else if (IS("help", "h")) say(&d, "%s", HELP);
        else if (IS("run", "r")) d.started ? say(&d, "The program is already running; use 'continue'.\n") : go(&d, STEP_NONE);
        else if (IS("continue", "c")) go(&d, STEP_NONE);
        else if (IS("stack", "st")) cmd_stack(&d, arg);
        else if (IS("step", "s")) go(&d, STEP_INTO);
        else if (IS("next", "n")) go(&d, STEP_OVER);
        else if (IS("finish", "fin")) go(&d, STEP_OUT);
        else if (IS("break", "b")) cmd_break(&d, arg);
        else if (IS("delete", "d")) cmd_delete(&d, arg);
        else if (IS("info", "i")) cmd_info_break(&d);
        else if (IS("print", "p")) cmd_print(&d, arg);
        else if (IS("regs", "reg")) cmd_regs(&d);
        else if (IS("bt", "bt") || IS("backtrace", "ba") || IS("where", "wh")) cmd_bt(&d);
        else if (IS("list", "l") || IS("dis", "dis")) cmd_list(&d);
        else if (IS("procs", "pr")) cmd_procs(&d);
        else if (IS("process", "proc")) {
            uint64_t pid = strtoull(arg, NULL, 10);
            if (vm_process(vm, pid)) d.sel = pid, say(&d, "Inspecting process %llu.\n", (unsigned long long)pid);
            else say(&d, "No live process %s.\n", arg);
        } else if (IS("quit", "q")) break;
        else say(&d, "Unknown command '%s'. Type 'help'.\n", cmd);
#undef IS
    }
    int rc = d.ended && d.main->state == PROC_DONE ? (int)(d.main->exit_code & 0xFF) : 0;
    sched_shutdown(vm, d.main);
    proc_free(d.main);
    for (size_t i = 0; i < d.bps.n; i++) free(d.bps.v[i].spec);
    VEC_FREE(d.bps);
    t->dbg = NULL;
    return rc;
}
