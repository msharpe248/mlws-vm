/* dap.c - `mlwsvm dap`: the Debug Adapter Protocol on stdin and stdout, so that
 * an editor can debug any language compiled to MLWSVM, with the source lines
 * its compiler marked with .loc. The debugger's engine (debugger.c) does the
 * running and stopping; this is the protocol, a small JSON reader, and the
 * views of frames, registers and objects. docs/TOOLING.md says what it does. */
#include <math.h>
#include <stdio.h>

#include "../heap/heap.h"
#include "../platform/platform.h"
#include "../vm/run.h"
#include "tools.h"
#include "tools_internal.h"

/* ---- JSON, as far as requests need it ---- */

enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ };

typedef struct Json {
    int kind;
    double num;
    char *str; /* J_STR; in a member of a J_OBJ, also its key (key) */
    char *key;
    VEC(struct Json *) items;
} Json;

static void json_free(Json *j) {
    if (!j) return;
    for (size_t i = 0; i < j->items.n; i++) json_free(j->items.v[i]);
    VEC_FREE(j->items);
    free(j->str);
    free(j->key);
    free(j);
}

static void skip_ws(const char **s) {
    while (**s == ' ' || **s == '\t' || **s == '\n' || **s == '\r') (*s)++;
}

static char *json_string(const char **s) {
    StrBuf b = {0};
    const char *c = *s + 1;
    while (*c && *c != '"') {
        if (*c != '\\') {
            sb_putc(&b, *c++);
            continue;
        }
        c++;
        switch (*c) {
        case 'n': sb_putc(&b, '\n'); break;
        case 't': sb_putc(&b, '\t'); break;
        case 'r': sb_putc(&b, '\r'); break;
        case 'b': sb_putc(&b, '\b'); break;
        case 'f': sb_putc(&b, '\f'); break;
        case 'u': { /* (paths in requests: the Basic Multilingual Plane, as UTF-8) */
            unsigned cp = 0;
            for (int i = 1; i <= 4 && c[i]; i++) cp = cp * 16 + (unsigned)(c[i] <= '9' ? c[i] - '0' : (c[i] | 32) - 'a' + 10);
            c += 4;
            if (cp < 0x80) sb_putc(&b, (char)cp);
            else if (cp < 0x800) sb_putc(&b, (char)(0xC0 | cp >> 6)), sb_putc(&b, (char)(0x80 | (cp & 63)));
            else sb_putc(&b, (char)(0xE0 | cp >> 12)), sb_putc(&b, (char)(0x80 | (cp >> 6 & 63))), sb_putc(&b, (char)(0x80 | (cp & 63)));
            break;
        }
        default: sb_putc(&b, *c); break;
        }
        if (*c) c++;
    }
    *s = *c ? c + 1 : c;
    return b.s ? b.s : xstrdup("");
}

static Json *json_value(const char **s) {
    skip_ws(s);
    Json *j = xcalloc(1, sizeof *j);
    if (**s == '{' || **s == '[') {
        bool obj = **s == '{';
        char end = obj ? '}' : ']';
        j->kind = obj ? J_OBJ : J_ARR;
        (*s)++;
        for (skip_ws(s); **s && **s != end; skip_ws(s)) {
            char *key = NULL;
            if (obj) {
                if (**s != '"') break;
                key = json_string(s);
                skip_ws(s);
                if (**s == ':') (*s)++;
            }
            Json *v = json_value(s);
            v->key = key;
            VEC_PUSH(j->items, v);
            skip_ws(s);
            if (**s == ',') (*s)++;
        }
        if (**s) (*s)++;
    } else if (**s == '"') {
        j->kind = J_STR;
        j->str = json_string(s);
    } else if (!strncmp(*s, "true", 4)) {
        j->kind = J_BOOL, j->num = 1, *s += 4;
    } else if (!strncmp(*s, "false", 5)) {
        j->kind = J_BOOL, *s += 5;
    } else if (!strncmp(*s, "null", 4)) {
        *s += 4;
    } else {
        char *end;
        j->kind = J_NUM;
        j->num = strtod(*s, &end);
        *s = end > *s ? end : *s + (**s != 0);
    }
    return j;
}

static const Json *json_get(const Json *j, const char *key) {
    if (j && j->kind == J_OBJ)
        for (size_t i = 0; i < j->items.n; i++)
            if (!strcmp(j->items.v[i]->key, key)) return j->items.v[i];
    return NULL;
}

static const char *json_str(const Json *j, const char *key) {
    const Json *v = json_get(j, key);
    return v && v->kind == J_STR ? v->str : NULL;
}

static int64_t json_int(const Json *j, const char *key, int64_t or_else) {
    const Json *v = json_get(j, key);
    return v && v->kind == J_NUM ? (int64_t)v->num : or_else;
}

static void sb_json(StrBuf *b, const char *s, size_t n) {
    sb_putc(b, '"');
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') sb_putc(b, '\\'), sb_putc(b, (char)c);
        else if (c == '\n') sb_puts(b, "\\n");
        else if (c == '\r') sb_puts(b, "\\r");
        else if (c == '\t') sb_puts(b, "\\t");
        else if (c < 0x20) sb_printf(b, "\\u%04x", c);
        else sb_putc(b, (char)c);
    }
    sb_putc(b, '"');
}

static void sb_jsons(StrBuf *b, const char *s) { sb_json(b, s, strlen(s)); }

/* ---- the session ---- */

typedef struct {
    int kind; /* V_*; V_BANK's bank -1: the frame's named variables; V_VAR's bank: the variable's index */
    uint64_t pid;
    uint32_t frame; /* V_BANK: its index in the process's frames */
    int bank;
    Obj *obj; /* V_OBJECT: valid until the program runs again */
} VarRef;
enum { V_BANK, V_OBJECT, V_VAR };

typedef struct {
    const ToolEnv *env;
    VM *vm;
    Debugger d;
    bool have_d, launched, over;
    int64_t seq;
    int64_t entry_bp;   /* stop at main */
    char inbuf[65536];
    size_t in_at, in_n;
    bool in_end;
    VEC(Json *) queue;  /* requests read while the program ran */
    bool pause_asked;
    VEC(VarRef) vars;   /* variablesReference n is vars.v[n - 1] */
    VEC(char *) specs;  /* the breakpoint paths set before anything is loaded (none, as launch comes first) */
} Dap;

static void send_body(const char *body, size_t n) {
    fprintf(stdout, "Content-Length: %zu\r\n\r\n", n);
    fwrite(body, 1, n, stdout);
    fflush(stdout);
}

/* {"seq":N,"type":"event","event":name,"body":{...}} */
static void send_event(Dap *a, const char *name, const char *body) {
    StrBuf b = {0};
    sb_printf(&b, "{\"seq\":%lld,\"type\":\"event\",\"event\":", (long long)++a->seq);
    sb_jsons(&b, name);
    if (body) sb_printf(&b, ",\"body\":%s", body);
    sb_putc(&b, '}');
    send_body(b.s, b.n);
    sb_free(&b);
}

static void send_response(Dap *a, const Json *req, const char *body) {
    StrBuf b = {0};
    sb_printf(&b, "{\"seq\":%lld,\"type\":\"response\",\"request_seq\":%lld,\"success\":true,\"command\":", (long long)++a->seq,
              (long long)json_int(req, "seq", 0));
    sb_jsons(&b, json_str(req, "command"));
    if (body) sb_printf(&b, ",\"body\":%s", body);
    sb_putc(&b, '}');
    send_body(b.s, b.n);
    sb_free(&b);
}

static void send_error(Dap *a, const Json *req, const char *msg) {
    StrBuf b = {0};
    sb_printf(&b, "{\"seq\":%lld,\"type\":\"response\",\"request_seq\":%lld,\"success\":false,\"command\":", (long long)++a->seq,
              (long long)json_int(req, "seq", 0));
    sb_jsons(&b, json_str(req, "command"));
    sb_puts(&b, ",\"message\":");
    sb_jsons(&b, msg);
    sb_putc(&b, '}');
    send_body(b.s, b.n);
    sb_free(&b);
}

static void send_output(Dap *a, const char *category, const char *s, size_t n) {
    StrBuf b = {0};
    sb_puts(&b, "{\"category\":");
    sb_jsons(&b, category);
    sb_puts(&b, ",\"output\":");
    sb_json(&b, s, n);
    sb_putc(&b, '}');
    send_event(a, "output", b.s);
    sb_free(&b);
}

static void program_out(void *ctx, const char *s, size_t n) { send_output(ctx, "stdout", s, n); }
static void program_err(void *ctx, const char *s, size_t n) { send_output(ctx, "stderr", s, n); }
static void engine_out(void *ctx, const char *s, size_t n) { (void)ctx, (void)s, (void)n; } /* (its transcript is `debug`'s) */

/* ---- reading requests: "Content-Length: N", a blank line, N bytes ---- */

static bool fill(Dap *a, uint64_t timeout_ns) {
    if (a->in_end) return false;
    if (a->in_at == a->in_n) a->in_at = a->in_n = 0;
    if (a->in_n == sizeof a->inbuf) return false;
    long r = plat_stdin_read(a->inbuf + a->in_n, sizeof a->inbuf - a->in_n, timeout_ns);
    if (r < 0) return false;
    if (r == 0) {
        a->in_end = true;
        return false;
    }
    a->in_n += (size_t)r;
    return true;
}

static int next_byte(Dap *a) {
    if (a->in_at == a->in_n && !fill(a, UINT64_MAX)) return -1;
    return (unsigned char)a->inbuf[a->in_at++];
}

static Json *read_message(Dap *a) {
    size_t len = 0;
    char line[256];
    for (;;) { /* the headers */
        size_t n = 0;
        int c;
        while ((c = next_byte(a)) >= 0 && c != '\n')
            if (n + 1 < sizeof line) line[n++] = (char)c;
        if (c < 0) return NULL;
        if (n && line[n - 1] == '\r') n--;
        line[n] = 0;
        if (!n) break;
        if (!strncmp(line, "Content-Length:", 15)) len = (size_t)strtoull(line + 15, NULL, 10);
    }
    char *body = xmalloc(len + 1);
    for (size_t i = 0; i < len; i++) {
        int c = next_byte(a);
        if (c < 0) {
            free(body);
            return NULL;
        }
        body[i] = (char)c;
    }
    body[len] = 0;
    const char *s = body;
    Json *j = json_value(&s);
    free(body);
    return j;
}

/* While the program runs: requests that have come go in the queue, and a pause
 * stops it. */
static bool poll_requests(void *ctx) {
    Dap *a = ctx;
    while (a->in_at < a->in_n || fill(a, 0)) {
        Json *j = read_message(a);
        if (!j) break;
        const char *cmd = json_str(j, "command");
        if (cmd && !strcmp(cmd, "pause")) {
            send_response(a, j, NULL);
            json_free(j);
            a->pause_asked = true;
        } else {
            VEC_PUSH(a->queue, j);
        }
    }
    bool p = a->pause_asked;
    a->pause_asked = false;
    return p;
}

/* ---- where code is ---- */

/* An instruction's source: the translated program's line if .loc says, else
 * the assembly's. line 0: unknown. */
static void position(const Function *f, uint32_t pc, const char **file, uint32_t *line) {
    if (module_srcpos(f->mod->m, f->mf, pc, file, line)) return;
    *file = fn_file(f);
    *line = f->mf->lines && pc < f->mf->ninsns ? f->mf->lines[pc] : 0;
}

/* The frame's instruction: the one it is stopped at, or the call it is in. */
static uint32_t frame_pc(const Process *p, uint32_t i) {
    const Frame *fr = &p->frames[i];
    return i == p->nframes - 1 ? fr->pc : (fr->pc ? fr->pc - 1 : 0);
}

static void sb_source(StrBuf *b, const char *file) {
    char *real = plat_realpath(file);
    sb_puts(b, "{\"name\":");
    sb_jsons(b, path_basename(file));
    sb_puts(b, ",\"path\":");
    sb_jsons(b, real ? real : file);
    sb_putc(b, '}');
    free(real);
}

/* ---- events after running ---- */

static void reset_vars(Dap *a) { a->vars.n = 0; }

static void report_stop(Dap *a, const char *reason) {
    StrBuf b = {0};
    sb_puts(&b, "{\"reason\":");
    sb_jsons(&b, reason);
    sb_printf(&b, ",\"threadId\":%llu,\"allThreadsStopped\":true}", (unsigned long long)a->d.sel);
    send_event(a, "stopped", b.s);
    sb_free(&b);
}

static void report_end(Dap *a) {
    Process *m = a->d.main;
    int code = 70;
    if (m->state == PROC_DONE) {
        code = (int)(m->exit_code & 0xFF);
    } else {
        StrBuf s = {0};
        sb_printf(&s, "runtime error[%s]: %s\n", m->err.code, m->err.msg);
        if (m->backtrace.s) sb_puts(&s, m->backtrace.s);
        send_output(a, "stderr", s.s, s.n);
        sb_free(&s);
    }
    StrBuf b = {0};
    sb_printf(&b, "{\"exitCode\":%d}", code);
    send_event(a, "exited", b.s);
    sb_free(&b);
    send_event(a, "terminated", NULL);
}

/* Run: true if it stopped, with the event left to the caller. */
static bool run(Dap *a, int step) {
    reset_vars(a);
    return dbg_go(&a->d, step);
}

static void go_on(Dap *a, int step, const char *reason) {
    if (!run(a, step)) report_end(a);
    else report_stop(a, a->d.paused ? "pause" : a->d.hit >= 0 ? "breakpoint" : reason);
}

/* A step of a line: instructions until another line, or a stop of its own. */
static void step_line(Dap *a, int step) {
    Process *p = dbg_selected(&a->d);
    if (!p) {
        go_on(a, step, "step");
        return;
    }
    const char *file0, *file;
    uint32_t line0, line;
    position(p->frames[p->nframes - 1].fn, p->frames[p->nframes - 1].pc, &file0, &line0);
    uint32_t depth0 = p->nframes;
    for (;;) {
        if (!run(a, step)) {
            report_end(a);
            return;
        }
        if (a->d.hit >= 0 || a->d.paused) break;
        p = dbg_selected(&a->d);
        if (!p) break;
        const Frame *fr = &p->frames[p->nframes - 1];
        position(fr->fn, fr->pc, &file, &line);
        if (line && (line != line0 || p->nframes != depth0 || strcmp(file, file0))) break;
    }
    report_stop(a, a->d.paused ? "pause" : a->d.hit >= 0 ? "breakpoint" : "step");
}

/* ---- views ---- */

static int64_t var_ref(Dap *a, VarRef v) {
    VEC_PUSH(a->vars, v);
    return (int64_t)a->vars.n;
}

static bool expandable(Word w, MlwsType t) {
    if (!type_is_ref(t) || !w.r) return false;
    int k = ((const Obj *)w.r)->kind;
    return k == OBJ_ARRAY || k == OBJ_STRUCT || k == OBJ_LIST || k == OBJ_MAP;
}

/* A variable: {"name","value","variablesReference"}. */
static void sb_variable(Dap *a, StrBuf *b, bool first, const char *name, Word w, MlwsType t) {
    StrBuf v = {0};
    fmt_value(&v, w, t, 1);
    if (!first) sb_putc(b, ',');
    sb_puts(b, "{\"name\":");
    sb_jsons(b, name);
    sb_puts(b, ",\"value\":");
    sb_json(b, v.s ? v.s : "", v.n);
    VarRef r = {V_OBJECT, 0, 0, 0, w.r};
    sb_printf(b, ",\"variablesReference\":%lld}", expandable(w, t) ? (long long)var_ref(a, r) : 0LL);
    sb_free(&v);
}

static void sb_object_vars(Dap *a, StrBuf *b, Obj *o) {
    bool first = true;
    char name[64];
    switch (o->kind) {
    case OBJ_ARRAY: {
        ObjArray *x = (ObjArray *)o;
        for (uint32_t i = 0; i < x->len && i < 1000; i++, first = false) {
            snprintf(name, sizeof name, "[%u]", i);
            sb_variable(a, b, first, name, x->data[i], (MlwsType)x->elem);
        }
        break;
    }
    case OBJ_LIST: {
        ObjList *x = (ObjList *)o;
        for (uint32_t i = 0; i < x->len && i < 1000; i++, first = false) {
            snprintf(name, sizeof name, "[%u]", i);
            sb_variable(a, b, first, name, x->items[i], (MlwsType)x->tags[i]);
        }
        break;
    }
    case OBJ_STRUCT: {
        ObjStruct *x = (ObjStruct *)o;
        for (uint32_t i = 0; i < x->def->nfields; i++, first = false)
            sb_variable(a, b, first, x->def->fields[i], x->fields[i], (MlwsType)x->def->types[i]);
        break;
    }
    case OBJ_MAP: {
        ObjMap *x = (ObjMap *)o;
        int shown = 0;
        for (uint32_t i = 0; i < x->nentries && shown < 1000; i++) {
            MapEntry *e = &x->entries[i];
            if (!e->live) continue;
            StrBuf k = {0};
            fmt_value(&k, e->key, (MlwsType)e->ktag, 0);
            sb_variable(a, b, first, k.s ? k.s : "", e->val, (MlwsType)e->vtag);
            sb_free(&k);
            first = false;
            shown++;
        }
        break;
    }
    default: break;
    }
}

static uint32_t frame_pc(const Process *p, uint32_t i);

/* What to expand a named variable into: its object, if it is one register's;
 * its registers, if more; or nothing (0). */
static int64_t var_children(Dap *a, const Process *p, uint32_t frame, const MVar *v) {
    const Frame *fr = &p->frames[frame];
    if (v->nregs > 1) {
        VarRef r = {V_VAR, p->pid, frame, (int)(v - fr->fn->mf->vars), NULL};
        return var_ref(a, r);
    }
    Word w;
    MlwsType t;
    dbg_var_reg(p, fr, v, 0, &w, &t);
    VarRef r = {V_OBJECT, 0, 0, 0, w.r};
    return expandable(w, t) ? var_ref(a, r) : 0;
}

/* The frame's named variables live at its instruction (the latest of a name),
 * as their formatters show them. */
static void sb_named_vars(Dap *a, StrBuf *b, Process *p, uint32_t i) {
    const Frame *fr = &p->frames[i];
    uint32_t pc = frame_pc(p, i);
    bool first = true;
    for (uint32_t k = 0; k < fr->fn->mf->nvars; k++) {
        const MVar *v = &fr->fn->mf->vars[k];
        if (pc < v->start || pc >= v->end || dbg_var_at(fr->fn, pc, v->name) != v) continue;
        StrBuf val = {0};
        dbg_show_var(p, fr, v, 1, &val);
        if (!first) sb_putc(b, ',');
        sb_puts(b, "{\"name\":");
        sb_jsons(b, v->name);
        sb_puts(b, ",\"value\":");
        sb_json(b, val.s ? val.s : "", val.n);
        sb_printf(b, ",\"variablesReference\":%lld}", (long long)var_children(a, p, i, v));
        sb_free(&val);
        first = false;
    }
}

/* A named variable's registers, each as itself. */
static void sb_var_regs(Dap *a, StrBuf *b, const VarRef *r) {
    Process *p = vm_process(a->vm, r->pid);
    if (!p && r->pid == a->d.main->pid) p = a->d.main;
    if (!p || r->frame >= p->nframes || r->bank < 0 || (uint32_t)r->bank >= p->frames[r->frame].fn->mf->nvars) return;
    const Frame *fr = &p->frames[r->frame];
    const MVar *v = &fr->fn->mf->vars[r->bank];
    char rb[8];
    for (int k = 0; k < v->nregs; k++) {
        Word w;
        MlwsType t;
        dbg_var_reg(p, fr, v, k, &w, &t);
        sb_variable(a, b, k == 0, reg_name(v->regs[k], rb), w, t);
    }
}

static void sb_bank_vars(Dap *a, StrBuf *b, const VarRef *v) {
    Process *p = vm_process(a->vm, v->pid);
    if (!p && v->pid == a->d.main->pid) p = a->d.main;
    if (!p || v->frame >= p->nframes) return;
    if (v->bank < 0) {
        sb_named_vars(a, b, p, v->frame);
        return;
    }
    const Frame *fr = &p->frames[v->frame];
    bool first = true;
    char name[16];
    for (uint32_t i = 0; i < fr->fn->mf->nregs[v->bank]; i++, first = false) {
        Word w;
        MlwsType t;
        const void *at = frame_reg(p->regs, fr, v->bank, (int)i);
        memcpy(&w, at, sizeof w);
        t = v->bank == BANK_I ? T_I64 : v->bank == BANK_F ? T_F64 : v->bank == BANK_R ? T_R : T_P;
        snprintf(name, sizeof name, "%s%u", bank_prefix((Bank)v->bank), i);
        sb_variable(a, b, first, name, w, t);
    }
}

/* ---- requests ---- */

/* A launch's `vmOptions`: VM options as on the command line, e.g.
 * ["--max-frames", "500000"], after the command line's. */
static bool launch_options(VM *vm, const Json *o, MlwsError *err) {
    if (!o) return true;
    if (o->kind != J_ARR) return mlws_err(err, "E_BAD_OPTION", "vmOptions: not a list of strings"), false;
    for (size_t i = 0; i < o->items.n; i += 2) {
        const Json *k = o->items.v[i], *v = i + 1 < o->items.n ? o->items.v[i + 1] : NULL;
        if (k->kind != J_STR || strncmp(k->str, "--", 2) || !vm_option_known(k->str + 2))
            return mlws_err(err, "E_BAD_OPTION", "vmOptions: %s is not a VM option",
                            k->kind == J_STR ? k->str : "a non-string"), false;
        if (!v || v->kind != J_STR) return mlws_err(err, "E_BAD_OPTION", "vmOptions: %s needs a value", k->str), false;
        if (!vm_set_option(vm, k->str + 2, v->str, err)) return false;
    }
    return true;
}

static void do_launch(Dap *a, const Json *req) {
    const Json *args = json_get(req, "arguments");
    const char *program = json_str(args, "program");
    if (!program) {
        send_error(a, req, "launch needs a program");
        return;
    }
    VM *vm = a->vm = vm_new();
    MlwsError err = {{0}, {0}};
    if (!tool_env_options(a->env, vm, &err) || !launch_options(vm, json_get(args, "vmOptions"), &err)) {
        send_error(a, req, err.msg);
        return;
    }
    tool_env_libpaths(a->env, vm);
    const Json *pa = json_get(args, "args");
    if (pa && pa->kind == J_ARR)
        for (size_t i = 0; i < pa->items.n; i++)
            if (pa->items.v[i]->kind == J_STR) VEC_PUSH(vm->prog_args, xstrdup(pa->items.v[i]->str));
    LModule *root = vm_load_root(vm, program, &err);
    Function *main_fn = root ? vm_find_main(root, &err) : NULL;
    if (!main_fn) {
        send_error(a, req, err.msg);
        return;
    }
    vm->out = program_out;
    vm->out_ctx = a;
    vm->err_out = program_err;
    vm->err_ctx = a;
    dbg_init(&a->d, vm, root, main_fn, engine_out, a);
    a->d.poll = poll_requests;
    a->d.poll_ctx = a;
    a->have_d = a->launched = true;
    const Json *soe = json_get(args, "stopOnEntry");
    a->entry_bp = soe && soe->num != 0;
    send_response(a, req, NULL);
    send_event(a, "initialized", NULL); /* now breakpoints can find their code */
}

static void do_set_breakpoints(Dap *a, const Json *req) {
    const Json *args = json_get(req, "arguments");
    const char *path = json_str(json_get(args, "source"), "path");
    const Json *bps = json_get(args, "breakpoints");
    if (!path || !a->have_d) {
        send_error(a, req, "no such source");
        return;
    }
    dbg_clear_spec(&a->d, path);
    StrBuf b = {0};
    sb_puts(&b, "{\"breakpoints\":[");
    for (size_t i = 0; bps && i < bps->items.n; i++) {
        uint32_t line = (uint32_t)json_int(bps->items.v[i], "line", 0), at = line;
        bool found = false;
        for (; !found && at < line + 32; at++) /* a blank or a comment: the next line with code */
            found = dbg_break_line(&a->d, path, path, at);
        if (found) sb_printf(&b, "%s{\"verified\":true,\"line\":%u}", i ? "," : "", at - 1);
        else sb_printf(&b, "%s{\"verified\":false,\"line\":%u}", i ? "," : "", line);
    }
    sb_puts(&b, "]}");
    send_response(a, req, b.s);
    sb_free(&b);
}

static void do_threads(Dap *a, const Json *req) {
    StrBuf b = {0};
    sb_puts(&b, "{\"threads\":[");
    bool first = true;
    for (uint64_t pid = 1, end = vm_pid_end(a->vm); pid < end; pid++) {
        Process *p = vm_proc_at(a->vm, pid);
        if (!p || !p->nframes) continue;
        const Function *f = p->frames[0].fn;
        char name[160];
        snprintf(name, sizeof name, "%llu %s", (unsigned long long)pid, f->name);
        sb_printf(&b, "%s{\"id\":%llu,\"name\":", first ? "" : ",", (unsigned long long)pid);
        sb_jsons(&b, name);
        sb_putc(&b, '}');
        first = false;
    }
    if (first) sb_printf(&b, "{\"id\":%llu,\"name\":\"main\"}", (unsigned long long)a->d.main->pid);
    sb_puts(&b, "]}");
    send_response(a, req, b.s);
    sb_free(&b);
}

/* frameId: the process and the frame's index in it. */
#define FRAME_ID(pid, i) ((int64_t)(pid) * 65536 + (int64_t)(i) + 1)

static Process *process_of(Dap *a, uint64_t pid) {
    Process *p = vm_process(a->vm, pid);
    return p ? p : pid == a->d.main->pid ? a->d.main : NULL;
}

static void do_stack(Dap *a, const Json *req) {
    const Json *args = json_get(req, "arguments");
    uint64_t pid = (uint64_t)json_int(args, "threadId", (int64_t)a->d.sel);
    Process *p = process_of(a, pid);
    int64_t start = json_int(args, "startFrame", 0), levels = json_int(args, "levels", 0);
    StrBuf b = {0};
    sb_puts(&b, "{\"stackFrames\":[");
    uint32_t total = p ? p->nframes : 0;
    uint32_t shown = 0;
    for (int64_t k = start; k < (int64_t)total && (!levels || shown < levels); k++, shown++) {
        uint32_t i = total - 1 - (uint32_t)k; /* the innermost first */
        const Frame *fr = &p->frames[i];
        const char *file;
        uint32_t line;
        position(fr->fn, frame_pc(p, i), &file, &line);
        char name[200];
        snprintf(name, sizeof name, "%s.%s", fr->fn->mod->m->name, fr->fn->name);
        sb_printf(&b, "%s{\"id\":%lld,\"name\":", shown ? "," : "", (long long)FRAME_ID(pid, i));
        sb_jsons(&b, name);
        sb_puts(&b, ",\"source\":");
        sb_source(&b, file);
        sb_printf(&b, ",\"line\":%u,\"column\":1}", line);
    }
    sb_printf(&b, "],\"totalFrames\":%u}", total);
    send_response(a, req, b.s);
    sb_free(&b);
}

static void do_scopes(Dap *a, const Json *req) {
    int64_t id = json_int(json_get(req, "arguments"), "frameId", 0) - 1;
    VarRef base = {V_BANK, (uint64_t)(id / 65536), (uint32_t)(id % 65536), 0, NULL};
    static const struct { const char *name; int bank; } SCOPES[] = {
        {"References (R)", BANK_R}, {"Integers (I)", BANK_I}, {"Floats (F)", BANK_F}, {"Pointers (P)", BANK_P},
    };
    Process *p = process_of(a, base.pid);
    StrBuf b = {0};
    sb_puts(&b, "{\"scopes\":[");
    bool first = true;
    if (p && base.frame < p->nframes) { /* the program's own names first, if it has any here */
        uint32_t pc = frame_pc(p, base.frame);
        const MFunc *mf = p->frames[base.frame].fn->mf;
        bool any = false;
        for (uint32_t k = 0; k < mf->nvars && !any; k++) any = pc >= mf->vars[k].start && pc < mf->vars[k].end;
        if (any) {
            VarRef v = base;
            v.bank = -1;
            sb_printf(&b, "{\"name\":\"Locals\",\"presentationHint\":\"locals\",\"variablesReference\":%lld,\"expensive\":false}", (long long)var_ref(a, v));
            first = false;
        }
    }
    for (size_t i = 0; p && base.frame < p->nframes && i < sizeof SCOPES / sizeof SCOPES[0]; i++) {
        if (!p->frames[base.frame].fn->mf->nregs[SCOPES[i].bank]) continue;
        VarRef v = base;
        v.bank = SCOPES[i].bank;
        sb_printf(&b, "%s{\"name\":", first ? "" : ",");
        sb_jsons(&b, SCOPES[i].name);
        sb_printf(&b, ",\"variablesReference\":%lld,\"expensive\":false}", (long long)var_ref(a, v));
        first = false;
    }
    sb_puts(&b, "]}");
    send_response(a, req, b.s);
    sb_free(&b);
}

static void do_variables(Dap *a, const Json *req) {
    int64_t ref = json_int(json_get(req, "arguments"), "variablesReference", 0);
    StrBuf b = {0};
    sb_puts(&b, "{\"variables\":[");
    if (ref > 0 && (size_t)ref <= a->vars.n) {
        VarRef v = a->vars.v[ref - 1]; /* (a copy: more may be added) */
        if (v.kind == V_BANK) sb_bank_vars(a, &b, &v);
        else if (v.kind == V_VAR) sb_var_regs(a, &b, &v);
        else sb_object_vars(a, &b, v.obj);
    }
    sb_puts(&b, "]}");
    send_response(a, req, b.s);
    sb_free(&b);
}

/* A register, in the frame asked about: the editor's watch and hover. */
static void do_evaluate(Dap *a, const Json *req) {
    const Json *args = json_get(req, "arguments");
    const char *expr = json_str(args, "expression");
    int64_t id = json_int(args, "frameId", 0) - 1;
    Process *p = id >= 0 ? process_of(a, (uint64_t)(id / 65536)) : dbg_selected(&a->d);
    uint32_t i = id >= 0 ? (uint32_t)(id % 65536) : (p ? p->nframes - 1 : 0);
    const char *banks = "IFRP", *bk = expr && expr[0] ? strchr(banks, expr[0]) : NULL;
    char *end = NULL;
    long idx = bk ? strtol(expr + 1, &end, 10) : -1;
    const MVar *named = p && i < p->nframes && expr ? dbg_var_at(p->frames[i].fn, frame_pc(p, i), expr) : NULL;
    if (named) { /* a variable's name, before a register's: as its formatter shows it */
        StrBuf v = {0}, b = {0};
        dbg_show_var(p, &p->frames[i], named, 2, &v);
        sb_puts(&b, "{\"result\":");
        sb_json(&b, v.s ? v.s : "", v.n);
        sb_printf(&b, ",\"variablesReference\":%lld}", (long long)var_children(a, p, i, named));
        send_response(a, req, b.s);
        sb_free(&v);
        sb_free(&b);
        return;
    }
    if (!p || i >= p->nframes || !bk || (!named && (end == expr + 1 || *end)) || idx < 0 || idx >= p->frames[i].fn->mf->nregs[bk - banks]) {
        send_error(a, req, "not a variable or register of this frame");
        return;
    }
    int bank = (int)(bk - banks);
    Word w;
    memcpy(&w, frame_reg(p->regs, &p->frames[i], bank, (int)idx), sizeof w);
    MlwsType t = bank == BANK_I ? T_I64 : bank == BANK_F ? T_F64 : bank == BANK_R ? T_R : T_P;
    StrBuf v = {0}, b = {0};
    fmt_value(&v, w, t, 2);
    sb_puts(&b, "{\"result\":");
    sb_json(&b, v.s ? v.s : "", v.n);
    VarRef r = {V_OBJECT, 0, 0, 0, w.r};
    sb_printf(&b, ",\"variablesReference\":%lld}", expandable(w, t) ? (long long)var_ref(a, r) : 0LL);
    send_response(a, req, b.s);
    sb_free(&v);
    sb_free(&b);
}

static void do_configuration_done(Dap *a, const Json *req) {
    send_response(a, req, NULL);
    if (!a->have_d) return;
    if (a->entry_bp) {
        dbg_add_bp(&a->d, "\1entry", a->d.main_fn, 0);
        if (!run(a, STEP_NONE)) {
            report_end(a);
            return;
        }
        dbg_clear_spec(&a->d, "\1entry");
        report_stop(a, "entry");
        return;
    }
    go_on(a, STEP_NONE, "breakpoint");
}

static void handle(Dap *a, const Json *req) {
    const char *cmd = json_str(req, "command");
    if (!cmd) return;
    bool stopped = a->have_d;
#define IS(s) (!strcmp(cmd, s))
    if (IS("initialize")) {
        send_response(a, req, "{\"supportsConfigurationDoneRequest\":true,\"supportsTerminateRequest\":true,"
                              "\"supportsEvaluateForHovers\":true}");
    } else if (IS("launch")) {
        do_launch(a, req);
    } else if (IS("setBreakpoints")) {
        do_set_breakpoints(a, req);
    } else if (IS("setExceptionBreakpoints") || IS("setFunctionBreakpoints")) {
        send_response(a, req, "{\"breakpoints\":[]}");
    } else if (IS("configurationDone")) {
        do_configuration_done(a, req);
    } else if (IS("threads") && stopped) {
        do_threads(a, req);
    } else if (IS("stackTrace") && stopped) {
        do_stack(a, req);
    } else if (IS("scopes") && stopped) {
        do_scopes(a, req);
    } else if (IS("variables") && stopped) {
        do_variables(a, req);
    } else if (IS("evaluate") && stopped) {
        do_evaluate(a, req);
    } else if (IS("continue") && stopped) {
        send_response(a, req, "{\"allThreadsContinued\":true}");
        go_on(a, STEP_NONE, "breakpoint");
    } else if (IS("next") && stopped) {
        send_response(a, req, NULL);
        step_line(a, STEP_OVER);
    } else if (IS("stepIn") && stopped) {
        send_response(a, req, NULL);
        step_line(a, STEP_INTO);
    } else if (IS("stepOut") && stopped) {
        send_response(a, req, NULL);
        go_on(a, STEP_OUT, "step");
    } else if (IS("pause")) {
        send_response(a, req, NULL); /* (already stopped: a running program's is taken by poll_requests) */
    } else if (IS("disconnect") || IS("terminate")) {
        send_response(a, req, NULL);
        if (IS("terminate")) send_event(a, "terminated", NULL);
        a->over = true;
    } else {
        send_error(a, req, "not supported");
    }
#undef IS
}

int tool_dap(int argc, char **argv, const ToolEnv *env) {
    (void)argv;
    if (argc) {
        fprintf(stderr, "usage: mlwsvm dap [-L dir] [VM options]   (requests on stdin, responses on stdout: an editor runs it)\n");
        return 2;
    }
    Dap *a = xcalloc(1, sizeof *a);
    a->env = env;
    while (!a->over) {
        Json *req;
        if (a->queue.n) { /* first come, first answered */
            req = a->queue.v[0];
            memmove(a->queue.v, a->queue.v + 1, --a->queue.n * sizeof *a->queue.v);
        } else {
            req = read_message(a);
        }
        if (!req) break;
        handle(a, req);
        json_free(req);
    }
    if (a->have_d) dbg_finish(&a->d);
    if (a->vm) vm_free(a->vm);
    for (size_t i = 0; i < a->queue.n; i++) json_free(a->queue.v[i]);
    VEC_FREE(a->queue);
    VEC_FREE(a->vars);
    free(a);
    return 0;
}
