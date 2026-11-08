/* monitor.c - `--monitor HOST:PORT`: a tiny HTTP/1.0 server polled by the
 * first worker thread between time slices, so it needs no thread of its own.
 * Each request is served with the other workers paused (sched_stop_world), so
 * it can read every process. It serves read-only JSON statistics, a plain-text
 * table for `mlwsvm top`, and the embedded web dashboard. See docs/TOOLING.md. */
#include <ctype.h>
#include <math.h>

#include "../heap/heap.h"
#include "../native/tls.h"
#include "../vm/run.h"
#include "../platform/platform.h"
#include "tools.h"
#include "tools_internal.h"

extern const char DASHBOARD_HTML[];
extern const size_t DASHBOARD_HTML_LEN;

typedef struct Monitor {
    int fd;
    char addr[96]; /* HOST:PORT */
    char host[64];
    int port;
    char *token;       /* --monitor-actions: what an action must show; NULL: none allowed */
    char *cert, *key;  /* --monitor-cert, --monitor-key: TLS */
    char *programs;    /* mlwsvm serve: the directory programs are started from (real path); NULL: not serving */
    /* previous /top sample, to turn cumulative CPU time into a percentage */
    VEC(uint64_t) last_cpu;
    uint64_t last_top_ns;
} Monitor;

static const char *const STATE_NAMES[] = {"ready", "running", "waiting", "sleeping", "done", "failed", "native", "io"};

static void json_str(StrBuf *b, const char *s) {
    sb_putc(b, '"');
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') sb_printf(b, "\\%c", c);
        else if (c < 0x20) sb_printf(b, "\\u%04x", c);
        else sb_putc(b, (char)c);
    }
    sb_putc(b, '"');
}

/* The same, for n bytes that may hold anything: cut at a character's start
 * when longer than max. */
static void json_strn(StrBuf *b, const char *s, size_t n, size_t max) {
    bool cut = n > max;
    if (cut) {
        n = max;
        while (n && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
    }
    sb_putc(b, '"');
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') sb_printf(b, "\\%c", c);
        else if (c < 0x20) sb_printf(b, "\\u%04x", c);
        else sb_putc(b, (char)c);
    }
    if (cut) sb_puts(b, "\u2026"); /* ... */
    sb_putc(b, '"');
}

/* A pid of another node, as JSON: {"node": N, "pid": P} (a whole pid can be
 * more than JavaScript's numbers hold exactly). */
static void far_pid_json(StrBuf *b, uint64_t pid) {
    sb_printf(b, "{\"node\":%llu,\"pid\":%llu}", (unsigned long long)(pid >> NODE_SHIFT),
              (unsigned long long)(pid & PID_LOCAL));
}

static void far_pids_json(StrBuf *b, const char *name, const PidVec *v) {
    sb_printf(b, ",\"%s\":[", name);
    for (size_t i = 0; i < v->n; i++) {
        if (i) sb_putc(b, ',');
        far_pid_json(b, v->v[i]);
    }
    sb_putc(b, ']');
}

/* What a message is, for the monitor: its type, size and a short preview. */
static void value_brief(StrBuf *b, Word v, uint8_t tag) {
    if (!type_is_ref(tag)) {
        sb_puts(b, "{\"type\":");
        json_str(b, type_name((MlwsType)tag));
        if (type_is_float((MlwsType)tag)) sb_printf(b, isfinite(v.f) ? ",\"preview\":%.17g}" : ",\"preview\":null}", v.f);
        else if (type_is_signed((MlwsType)tag)) sb_printf(b, ",\"preview\":%lld}", (long long)v.i);
        else sb_printf(b, ",\"preview\":%llu}", (unsigned long long)v.u);
        return;
    }
    const Obj *o = v.r;
    sb_puts(b, "{\"type\":");
    json_str(b, o ? obj_kind_name(o->kind) : "null");
    if (!o) {
        sb_putc(b, '}');
        return;
    }
    switch (o->kind) {
    case OBJ_STRING:
        sb_printf(b, ",\"size\":%u,\"preview\":", ((const ObjString *)o)->len);
        json_strn(b, ((const ObjString *)o)->data, ((const ObjString *)o)->len, 80);
        break;
    case OBJ_BYTES: sb_printf(b, ",\"size\":%u", ((const ObjBytes *)o)->len); break;
    case OBJ_LIST: sb_printf(b, ",\"size\":%u", ((const ObjList *)o)->len); break;
    case OBJ_ARRAY: sb_printf(b, ",\"size\":%u", ((const ObjArray *)o)->len); break;
    case OBJ_MAP: sb_printf(b, ",\"size\":%u", ((const ObjMap *)o)->count); break;
    case OBJ_STRUCT: {
        const StructDef *d = ((const ObjStruct *)o)->def;
        StrBuf q = {0};
        sb_printf(&q, "%s.%s", d->module, d->name);
        sb_puts(b, ",\"preview\":");
        json_str(b, q.s);
        sb_free(&q);
        break;
    }
    default: break;
    }
    sb_putc(b, '}');
}

static uint32_t stack_bytes(const Process *p) {
    return PTR_SEG(p->sp) == SEG_STACK && PTR_OFF(p->sp) <= p->stack.size ? p->stack.size - PTR_OFF(p->sp) : 0;
}

static void proc_json(StrBuf *b, const Process *p, bool detail) {
    const Frame *fr = p->nframes ? &p->frames[p->nframes - 1] : NULL;
    uint32_t pc = fr ? fr->pc : 0;
    sb_printf(b, "{\"pid\":%llu,\"state\":", (unsigned long long)p->pid);
    json_str(b, STATE_NAMES[p->state]);
    sb_puts(b, ",\"entry\":");
    json_str(b, p->entry ? p->entry->name : "?");
    sb_puts(b, ",\"function\":");
    if (fr) {
        StrBuf f = {0};
        fn_qualname(&f, fr->fn);
        json_str(b, f.s);
        sb_free(&f);
        sb_puts(b, ",\"file\":");
        json_str(b, fn_file(fr->fn));
        sb_printf(b, ",\"line\":%u", fr->fn->mf->lines && pc < fr->fn->mf->ninsns ? fr->fn->mf->lines[pc] : 0);
        const char *file;
        uint32_t sline;
        if (module_srcpos(fr->fn->mod->m, fr->fn->mf, pc, &file, &sline)) { /* `.loc`: the program's own line */
            sb_puts(b, ",\"source\":");
            json_str(b, file);
            sb_printf(b, ",\"source_line\":%u", sline);
        }
    } else {
        sb_puts(b, "null,\"file\":null,\"line\":0");
    }
    sb_printf(b,
              ",\"insns\":%llu,\"cpu_ns\":%llu,\"heap_bytes\":%llu,\"heap_peak\":%llu,\"raw_bytes\":%llu,"
              "\"stack_bytes\":%u,\"depth\":%u,\"mailbox\":%zu,\"msgs_sent\":%llu,\"msgs_received\":%llu,"
              "\"gc_count\":%llu,\"gc_ns\":%llu,\"exceptions\":%llu,\"objects\":%llu",
              (unsigned long long)p->insns, (unsigned long long)p->cpu_ns, (unsigned long long)p->heap_bytes,
              (unsigned long long)gc_heap_peak(p), (unsigned long long)p->raw_bytes, stack_bytes(p), p->nframes,
              (size_t)p->mcount, (unsigned long long)p->msgs_sent, (unsigned long long)p->msgs_received,
              (unsigned long long)p->gc_count, (unsigned long long)p->gc_ns, (unsigned long long)p->exceptions_thrown,
              (unsigned long long)(p->objects_allocated - p->gc_freed_objects));
    sb_puts(b, ",\"links\":[");
    for (size_t i = 0; i < p->links.n; i++) sb_printf(b, "%s%llu", i ? "," : "", (unsigned long long)p->links.v[i]);
    sb_puts(b, "],\"monitors\":[");
    for (size_t i = 0; i < p->monitors.n; i++)
        sb_printf(b, "%s%llu", i ? "," : "", (unsigned long long)p->monitors.v[i]);
    sb_puts(b, "]");
    far_pids_json(b, "far_links", &p->rlinks);
    far_pids_json(b, "far_monitors", &p->rmonitors);
    far_pids_json(b, "far_watching", &p->rwatch);
    if (detail) {
        static const char *const qn[] = {"heap", "raw", "mailbox", "insns", "foreign", "os"};
        sb_puts(b, ",\"quota\":{");
        for (int q = 0; q < QUOTA__COUNT; q++) {
            sb_printf(b, "%s\"%s\":", q ? "," : "", qn[q]);
            if (p->quota[q] == UINT64_MAX) sb_puts(b, "null");
            else sb_printf(b, "%llu", (unsigned long long)p->quota[q]);
        }
        /* The first messages waiting: taken into its heap, then delivered. */
        sb_puts(b, "},\"messages\":[");
        size_t shown = 0;
        for (size_t i = p->mbox_head; i < p->mbox.n && shown < 20; i++, shown++) {
            if (shown) sb_putc(b, ',');
            value_brief(b, p->mbox.v[i].v, p->mbox.v[i].tag);
        }
        for (size_t i = 0; i < p->inbox.n && shown < 20; i++, shown++) {
            if (shown) sb_putc(b, ',');
            value_brief(b, p->inbox.v[i].v, p->inbox.v[i].tag);
        }
        sb_puts(b, "],\"backtrace\":[");
        for (uint32_t i = p->nframes; i-- > 0;) {
            const Frame *f = &p->frames[i];
            uint32_t fpc = i == p->nframes - 1 ? f->pc : (f->pc ? f->pc - 1 : 0);
            StrBuf loc = {0};
            fmt_location(&loc, f->fn, fpc);
            sb_puts(b, i == p->nframes - 1 ? "" : ",");
            json_str(b, loc.s);
            sb_free(&loc);
        }
        sb_puts(b, "]");
    }
    sb_putc(b, '}');
}

static void vm_json(StrBuf *b, VM *vm) {
    uint64_t insns = vm->insns_retired, heap = 0, raw = 0;
    for (uint64_t pid = 1, end = vm_pid_end(vm); pid < end; pid++) {
        const Process *p = vm_proc_at(vm, pid);
        if (!p) continue;
        insns += p->insns;
        heap += p->heap_bytes;
        raw += p->raw_bytes;
    }
    sb_printf(b,
              "{\"vm\":\"mlwsvm\",\"uptime_ns\":%llu,\"processes\":%llu,\"spawned\":%llu,\"max_live\":%llu,"
              "\"slices\":%llu,\"insns\":%llu,\"heap_bytes\":%llu,\"raw_bytes\":%llu,\"modules\":%zu,\"main_pid\":%llu}",
              (unsigned long long)(plat_now_ns() - vm->start_ns), (unsigned long long)vm->live,
              (unsigned long long)vm->spawned, (unsigned long long)vm->max_live, (unsigned long long)vm->slices,
              (unsigned long long)insns, (unsigned long long)heap, (unsigned long long)raw, vm->mods.n,
              (unsigned long long)vm->main_pid);
}

static const char *const MOD_STATES[] = {"", "loading", "ready", "failed", "purged"};

/* Every module loaded, each version of one in the order loaded. */
static void modules_json(StrBuf *b, VM *vm) {
    sb_putc(b, '[');
    for (size_t i = 0; i < vm->mods.n; i++) {
        const LModule *lm = vm->mods.v[i];
        uint32_t version = 1;
        for (size_t j = 0; j < i; j++) version += !strcmp(vm->mods.v[j]->m->name, lm->m->name);
        sb_puts(b, i ? ",{\"name\":" : "{\"name\":");
        json_str(b, lm->m->name);
        sb_puts(b, ",\"path\":");
        json_str(b, lm->path);
        sb_printf(b, ",\"version\":%u,\"current\":%s,\"state\":", version,
                  lm->replaced || lm->state != LM_READY ? "false" : "true");
        json_str(b, lm->state >= 1 && lm->state <= 4 ? MOD_STATES[lm->state] : "?");
        sb_printf(b, ",\"functions\":%u,\"globals_bytes\":%u}", lm->m->nfuncs, lm->m->gsize);
    }
    sb_putc(b, ']');
}

/* The registry: each name and its process. */
static void names_json(StrBuf *b, VM *vm) {
    sb_putc(b, '[');
    bool first = true;
    plat_lock(vm->lock);
    for (size_t i = 0; i < vm->registry.cap; i++) {
        if (!vm->registry.keys || !vm->registry.keys[i] || !vm->registry.vals[i]) continue;
        sb_puts(b, first ? "{\"name\":" : ",{\"name\":");
        first = false;
        json_str(b, vm->registry.keys[i]);
        sb_printf(b, ",\"pid\":%llu}", (unsigned long long)(uintptr_t)vm->registry.vals[i]);
    }
    plat_unlock(vm->lock);
    sb_putc(b, ']');
}

/* This VM's node number (0: none), and the nodes it is connected to, with the
 * process carrying each connection. */
static void nodes_json(StrBuf *b, VM *vm) {
    sb_printf(b, "{\"self\":%u,\"connected\":[", vm->node_self);
    bool first = true;
    for (uint32_t n = 1; vm->node_route && n <= 65535; n++) {
        uint64_t c = __atomic_load_n(&vm->node_route[n], __ATOMIC_ACQUIRE);
        if (!c) continue;
        sb_printf(b, "%s{\"node\":%u,\"carrier\":%llu}", first ? "" : ",", n, (unsigned long long)c);
        first = false;
    }
    sb_puts(b, "]}");
}

/* Who is linked with whom, and who watches whom: here, and with other nodes. */
static void graph_json(StrBuf *b, VM *vm) {
    StrBuf links = {0}, mons = {0}, far = {0};
    for (uint64_t pid = 1, end = vm_pid_end(vm); pid < end; pid++) {
        const Process *p = vm_proc_at(vm, pid);
        if (!p) continue;
        for (size_t i = 0; i < p->links.n; i++) /* (each link is on both sides: once) */
            if (p->links.v[i] > pid) sb_printf(&links, "%s[%llu,%llu]", links.n ? "," : "", (unsigned long long)pid, (unsigned long long)p->links.v[i]);
        for (size_t i = 0; i < p->monitors.n; i++)
            sb_printf(&mons, "%s[%llu,%llu]", mons.n ? "," : "", (unsigned long long)p->monitors.v[i], (unsigned long long)pid);
        const PidVec *v[3] = {&p->rlinks, &p->rmonitors, &p->rwatch};
        static const char *const kind[3] = {"link", "watched_by", "watches"};
        for (int k = 0; k < 3; k++)
            for (size_t i = 0; i < v[k]->n; i++) {
                sb_printf(&far, "%s{\"pid\":%llu,\"kind\":\"%s\",\"other\":", far.n ? "," : "", (unsigned long long)pid, kind[k]);
                far_pid_json(&far, v[k]->v[i]);
                sb_putc(&far, '}');
            }
    }
    sb_printf(b, "{\"links\":[%s],\"monitors\":[%s],\"far\":[%s]}", links.n ? links.s : "", mons.n ? mons.s : "",
              far.n ? far.s : "");
    sb_free(&links);
    sb_free(&mons);
    sb_free(&far);
}

static const char *const LOG_LEVELS[] = {"debug", "info", "warn", "error"};

/* The log channel's entries after `since`, oldest first, with the number to
 * ask from next, and how many were lost (overwritten before being asked for). */
static void log_json(StrBuf *b, VM *vm, uint64_t since) {
    if (!vm->mlog) {
        sb_puts(b, "{\"next\":0,\"lost\":0,\"entries\":[]}");
        return;
    }
    plat_lock(vm->mlog_lock);
    uint64_t last = vm->mlog_seq, first = last > vm->mlog_cap ? last - vm->mlog_cap + 1 : 1;
    if (since > last) since = last;
    uint64_t from = since + 1 > first ? since + 1 : first;
    sb_printf(b, "{\"next\":%llu,\"lost\":%llu,\"entries\":[", (unsigned long long)last,
              (unsigned long long)(from - (since + 1)));
    for (uint64_t q = from; q <= last; q++) {
        const MLogEntry *e = &vm->mlog[(q - 1) % vm->mlog_cap];
        sb_printf(b, "%s{\"seq\":%llu,\"t_ns\":%llu,\"pid\":%llu,", q == from ? "" : ",", (unsigned long long)e->seq,
                  (unsigned long long)e->t_ns, (unsigned long long)e->pid);
        if (e->level < 0) {
            sb_puts(b, "\"event\":");
            json_str(b, e->name);
            sb_printf(b, ",\"value\":%s}", e->text); /* (JSON already) */
        } else {
            sb_puts(b, "\"level\":");
            json_str(b, LOG_LEVELS[e->level]);
            sb_puts(b, ",\"text\":");
            json_strn(b, e->text, strlen(e->text), MLOG_TEXT_MAX);
            sb_putc(b, '}');
        }
    }
    sb_puts(b, "]}");
    plat_unlock(vm->mlog_lock);
}

static void exits_json(StrBuf *b, VM *vm) {
    sb_putc(b, '[');
    size_t n = vm->exits.n;
    for (size_t k = 0; k < n; k++) { /* newest first */
        const ExitRecord *r = &vm->exits.v[(vm->exits_next + n - 1 - k) % n];
        sb_printf(b, "%s{\"pid\":%llu,\"entry\":", k ? "," : "", (unsigned long long)r->pid);
        json_str(b, r->entry);
        sb_printf(b, ",\"failed\":%s,\"code\":", r->failed ? "true" : "false");
        if (r->failed) json_str(b, r->code);
        else sb_puts(b, "null");
        sb_printf(b, ",\"exit\":%lld,\"insns\":%llu,\"cpu_ns\":%llu,\"ago_ns\":%llu}", (long long)r->exit_code,
                  (unsigned long long)r->insns, (unsigned long long)r->cpu_ns,
                  (unsigned long long)(plat_now_ns() - r->ended_ns));
    }
    sb_putc(b, ']');
}

static void human(char *buf, size_t n, double v) {
    const char *u = "";
    if (v >= 1e9) v /= 1e9, u = "G";
    else if (v >= 1e6) v /= 1e6, u = "M";
    else if (v >= 1e3) v /= 1e3, u = "K";
    snprintf(buf, n, *u ? "%.1f%s" : "%.0f%s", v, u);
}

static int cmp_cpu(const void *a, const void *b) {
    const Process *x = *(Process *const *)a, *y = *(Process *const *)b;
    return x->cpu_ns < y->cpu_ns ? 1 : x->cpu_ns > y->cpu_ns ? -1 : 0;
}

static void top_text(StrBuf *b, Monitor *mon, VM *vm) {
    uint64_t now = plat_now_ns(), insns = vm->insns_retired, heap = 0;
    VEC(Process *) ps = {0};
    uint64_t end = vm_pid_end(vm);
    for (uint64_t pid = 1; pid < end; pid++) {
        Process *p = vm_proc_at(vm, pid);
        if (!p) continue;
        insns += p->insns;
        heap += p->heap_bytes;
        VEC_PUSH(ps, p);
    }
    qsort(ps.v, ps.n, sizeof *ps.v, cmp_cpu);
    char ib[16], hb[16];
    human(ib, sizeof ib, (double)insns);
    human(hb, sizeof hb, (double)heap);
    sb_printf(b, "MLWSVM  up %.1fs  processes %llu (max %llu, spawned %llu)  insns %s  heap %sB\n\n",
              (double)(now - vm->start_ns) / 1e9, (unsigned long long)vm->live, (unsigned long long)vm->max_live,
              (unsigned long long)vm->spawned, ib, hb);
    sb_printf(b, "%6s %-8s %5s %8s %8s %8s %7s %5s %11s %4s  %s\n", "PID", "STATE", "%CPU", "INSNS", "HEAP", "RAW",
              "STACK", "MBOX", "MSGS IN/OUT", "GC", "FUNCTION");
    double dt = mon->last_top_ns ? (double)(now - mon->last_top_ns) : 0;
    for (size_t i = 0; i < ps.n && i < 40; i++) {
        Process *p = ps.v[i];
        /* %CPU since the previous /top sample; the first sample uses the whole uptime. */
        bool have = dt > 0 && p->pid - 1 < mon->last_cpu.n;
        uint64_t prev = have ? mon->last_cpu.v[p->pid - 1] : 0;
        double span = have ? dt : (double)(now - vm->start_ns);
        double cpu = span > 0 ? 100.0 * (double)(p->cpu_ns - prev) / span : 0;
        char in[16], h[16], r[16], msgs[32];
        human(in, sizeof in, (double)p->insns);
        human(h, sizeof h, (double)p->heap_bytes);
        human(r, sizeof r, (double)p->raw_bytes);
        snprintf(msgs, sizeof msgs, "%llu/%llu", (unsigned long long)p->msgs_received, (unsigned long long)p->msgs_sent);
        const Frame *fr = p->nframes ? &p->frames[p->nframes - 1] : NULL;
        StrBuf loc = {0};
        if (fr) { /* compact: module.function+pc (file:line), file without its directory */
            fn_qualname(&loc, fr->fn);
            sb_printf(&loc, "+%u (", fr->pc);
            fn_where(&loc, fr->fn, fr->pc, true);
            sb_putc(&loc, ')');
        }
        sb_printf(b, "%6llu %-8s %5.1f %8s %8s %8s %7u %5zu %11s %4llu  %s\n", (unsigned long long)p->pid,
                  STATE_NAMES[p->state], cpu, in, h, r, stack_bytes(p), (size_t)p->mcount, msgs,
                  (unsigned long long)p->gc_count, loc.s ? loc.s : "-");
        sb_free(&loc);
    }
    if (ps.n > 40) sb_printf(b, "   ... %zu more\n", ps.n - 40);
    /* remember CPU totals for the next sample */
    VEC_GROW(mon->last_cpu, end - 1);
    for (size_t i = mon->last_cpu.n; i < end - 1; i++) mon->last_cpu.v[i] = 0;
    mon->last_cpu.n = end - 1;
    for (size_t i = 0; i < ps.n; i++) mon->last_cpu.v[ps.v[i]->pid - 1] = ps.v[i]->cpu_ns;
    mon->last_top_ns = now;
    if (vm->exits.n) {
        sb_puts(b, "\nrecent exits:\n");
        size_t n = vm->exits.n;
        for (size_t k = 0; k < n && k < 8; k++) {
            const ExitRecord *e = &vm->exits.v[(vm->exits_next + n - 1 - k) % n];
            char in[16];
            human(in, sizeof in, (double)e->insns);
            if (e->failed) sb_printf(b, "%6llu %-16s failed %-12s %8s insns\n", (unsigned long long)e->pid, e->entry, e->code, in);
            else sb_printf(b, "%6llu %-16s exit %-14lld %8s insns\n", (unsigned long long)e->pid, e->entry, (long long)e->exit_code, in);
        }
    }
    VEC_FREE(ps);
}

/* ---- connections: plain, or TLS (--monitor-cert, --monitor-key) ---------------------- */

#define REQ_DEADLINE_NS 2000000000u /* a request read and answered within 2 s, or dropped */
#define BODY_MAX 65536

typedef struct {
    int fd;
    Tls *tls;
    uint64_t deadline;
} Conn;

static uint64_t left_ns(const Conn *c) {
    uint64_t now = plat_now_ns();
    return now < c->deadline ? c->deadline - now : 0;
}

/* A TLS call's progress: wait for what it wants, false once out of time. */
static bool tls_wait(Conn *c, long r) {
    if (!left_ns(c)) return false;
    if (r == TLS_WANT_READ) return plat_wait_readable(c->fd, left_ns(c));
    plat_sleep_ns(1000000); /* TLS_WANT_WRITE: rare for answers this small */
    return true;
}

static long conn_read(Conn *c, char *buf, size_t n) {
    if (!c->tls) return plat_sock_read(c->fd, buf, n, left_ns(c));
    char err[160];
    for (;;) {
        long r = tls_read(c->tls, buf, n, err, sizeof err);
        if (r >= 0) return r;
        if (r == TLS_FAILED || !tls_wait(c, r)) return -1;
    }
}

static bool conn_write(Conn *c, const char *buf, size_t n) {
    if (!c->tls) return plat_sock_write_all(c->fd, buf, n);
    char err[160];
    while (n) {
        long r = tls_write(c->tls, buf, n, err, sizeof err);
        if (r > 0) buf += r, n -= (size_t)r;
        else if (r == TLS_FAILED || !tls_wait(c, r)) return false;
    }
    return true;
}

static void conn_close(Conn *c) {
    if (c->tls) tls_release(c->tls); /* (and its descriptor) */
    else plat_sock_close(c->fd);
}

static const char *status_text(int status) {
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    default: return "Error";
    }
}

/* Reads may be shown to any page (Access-Control-Allow-Origin: *), as they
 * always were; an action's answer is for the monitor's own page only. */
static void respond(Conn *c, int status, const char *type, const char *body, size_t n, bool any_origin) {
    StrBuf h = {0};
    sb_printf(&h, "HTTP/1.0 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\n%s"
                  "Connection: close\r\n\r\n",
              status, status_text(status), type, n, any_origin ? "Access-Control-Allow-Origin: *\r\n" : "");
    if (conn_write(c, h.s, h.n)) conn_write(c, body, n);
    sb_free(&h);
}

static void respond_json(Conn *c, int status, const char *json) { respond(c, status, "application/json", json, strlen(json), false); }

/* ---- requests ------------------------------------------------------------------------ */

typedef struct {
    char method[8], path[512], host[160], origin[200], auth[200];
    size_t clen;
    bool bad_len; /* a Content-Length past BODY_MAX */
    char *body;
    size_t nbody;
} Req;

/* Whether s starts with p, case blind (ASCII). */
static bool starts_ci(const char *s, const char *p) {
    for (; *p; s++, p++)
        if (tolower((unsigned char)*s) != tolower((unsigned char)*p)) return false;
    return true;
}

/* A header's value (name ending in ':'), case blind, into out. */
static void header(const char *head, const char *name, char *out, size_t n) {
    size_t k = strlen(name);
    out[0] = 0;
    for (const char *l = strchr(head, '\n'); l; l = strchr(l, '\n')) {
        l++;
        if (!starts_ci(l, name)) continue;
        l += k;
        while (*l == ' ' || *l == '\t') l++;
        size_t m = strcspn(l, "\r\n");
        if (m >= n) m = n - 1;
        memcpy(out, l, m);
        out[m] = 0;
        return;
    }
}

static bool read_request(Conn *c, Req *q) {
    char head[8192];
    size_t n = 0;
    char *end = NULL;
    while (n < sizeof head - 1) {
        long r = conn_read(c, head + n, sizeof head - 1 - n);
        if (r <= 0) return false;
        n += (size_t)r;
        head[n] = 0;
        if ((end = strstr(head, "\r\n\r\n"))) {
            end += 4;
            break;
        }
    }
    if (!end) return false;
    if (sscanf(head, "%7s %511s", q->method, q->path) != 2) return false;
    header(head, "Host:", q->host, sizeof q->host);
    header(head, "Origin:", q->origin, sizeof q->origin);
    header(head, "Authorization:", q->auth, sizeof q->auth);
    char len[32];
    header(head, "Content-Length:", len, sizeof len);
    q->clen = len[0] ? strtoull(len, NULL, 10) : 0;
    if (q->clen > BODY_MAX) {
        q->bad_len = true;
        return true;
    }
    q->body = xmalloc(q->clen + 1);
    q->nbody = n - (size_t)(end - head) < q->clen ? n - (size_t)(end - head) : q->clen;
    memcpy(q->body, end, q->nbody);
    while (q->nbody < q->clen) {
        long r = conn_read(c, q->body + q->nbody, q->clen - q->nbody);
        if (r <= 0) return false;
        q->nbody += (size_t)r;
    }
    q->body[q->nbody] = 0;
    return true;
}

/* mlwsvm serve's programs (the first, its own, left out): each one's number,
 * file (from the programs' directory), arguments, main process, and how it is. */
static void programs_json(StrBuf *b, const Monitor *mon, VM *vm) {
    sb_putc(b, '[');
    size_t base = mon->programs ? strlen(mon->programs) + 1 : 0;
    plat_lock(vm->lock);
    for (size_t i = 1; mon->programs && i < vm->progs.n; i++) {
        const Program *pr = vm->progs.v[i];
        const Process *m = pr->main;
        sb_printf(b, "%s{\"id\":%zu,\"path\":", i > 1 ? "," : "", i);
        json_str(b, strlen(pr->path) > base ? pr->path + base : pr->path);
        sb_puts(b, ",\"args\":[");
        for (size_t k = 0; k < pr->args.n; k++) {
            if (k) sb_putc(b, ',');
            json_str(b, pr->args.v[k]);
        }
        bool done = m->state == PROC_DONE, failed = m->state == PROC_FAILED;
        sb_printf(b, "],\"pid\":%llu,\"state\":\"%s\"", (unsigned long long)m->pid,
                  failed ? "failed" : done ? "done" : "running");
        if (done) sb_printf(b, ",\"exit\":%lld", (long long)m->exit_code);
        if (failed) {
            sb_puts(b, ",\"code\":");
            json_str(b, m->err.code);
            sb_puts(b, ",\"message\":");
            json_str(b, m->err.msg);
        }
        sb_putc(b, '}');
    }
    plat_unlock(vm->lock);
    sb_putc(b, ']');
}

/* The read-only API and the page, with the world stopped. */
static void serve_get(Monitor *mon, VM *vm, Conn *k, Req *q) {
    char *path = q->path;
    char *qs = strchr(path, '?');
    if (qs) *qs++ = 0;
    const char *since = qs && !strncmp(qs, "since=", 6) ? qs + 6 : NULL;
    StrBuf b = {0};
    const char *json = "application/json";
    if (!strcmp(path, "/") || !strcmp(path, "/index.html")) {
        respond(k, 200, "text/html; charset=utf-8", DASHBOARD_HTML, DASHBOARD_HTML_LEN, true);
        return;
    } else if (!strcmp(path, "/api/vm")) {
        vm_json(&b, vm);
        b.n--; /* (its closing brace: one more field, whether actions are on) */
        sb_printf(&b, ",\"actions\":%s,\"serve\":%s}", mon->token ? "true" : "false", mon->programs ? "true" : "false");
    } else if (!strcmp(path, "/api/programs")) {
        programs_json(&b, mon, vm);
    } else if (!strcmp(path, "/api/procs")) {
        sb_putc(&b, '[');
        bool first = true;
        for (uint64_t pid = 1, end = vm_pid_end(vm); pid < end; pid++) {
            const Process *p = vm_proc_at(vm, pid);
            if (!p) continue;
            if (!first) sb_putc(&b, ',');
            first = false;
            proc_json(&b, p, false);
        }
        sb_putc(&b, ']');
    } else if (!strncmp(path, "/api/proc/", 10)) {
        uint64_t pid = strtoull(path + 10, NULL, 10);
        const Process *p = vm_proc_at(vm, pid);
        if (!p) {
            respond(k, 404, json, "{\"error\":\"no such process\"}", 27, true);
            return;
        }
        proc_json(&b, p, true);
    } else if (!strcmp(path, "/api/exits")) {
        exits_json(&b, vm);
    } else if (!strcmp(path, "/api/modules")) {
        modules_json(&b, vm);
    } else if (!strcmp(path, "/api/names")) {
        names_json(&b, vm);
    } else if (!strcmp(path, "/api/nodes")) {
        nodes_json(&b, vm);
    } else if (!strcmp(path, "/api/graph")) {
        graph_json(&b, vm);
    } else if (!strcmp(path, "/api/log")) {
        log_json(&b, vm, since ? strtoull(since, NULL, 10) : 0);
    } else if (!strcmp(path, "/top")) {
        top_text(&b, mon, vm);
        respond(k, 200, "text/plain; charset=utf-8", b.s, b.n, true);
        sb_free(&b);
        return;
    } else {
        respond(k, 404, "text/plain", "not found\n", 10, true);
        return;
    }
    respond(k, 200, json, b.s, b.n, true);
    sb_free(&b);
}

/* ---- actions (--monitor-actions) ------------------------------------------------------ *
 * Each needs the token (Authorization: Bearer TOKEN), the monitor's own Host
 * when it listens on this machine only (against DNS rebinding), and no Origin
 * but its own (against cross-site requests). They run with the world going,
 * since a reload stops it itself, and each is written to the program's log. */

static bool loopback(const char *host) {
    return !strcmp(host, "127.0.0.1") || !strcmp(host, "localhost") || !strcmp(host, "::1");
}

/* Host: the address listened on, or another name of this machine for it. */
static bool host_ok(const Monitor *mon, const char *host) {
    if (!loopback(mon->host)) return true; /* (other names for the machine: the token decides) */
    char want[3][96];
    snprintf(want[0], sizeof want[0], "127.0.0.1:%d", mon->port);
    snprintf(want[1], sizeof want[1], "localhost:%d", mon->port);
    snprintf(want[2], sizeof want[2], "[::1]:%d", mon->port);
    for (int i = 0; i < 3; i++)
        if (!strcmp(host, want[i])) return true;
    return false;
}

static bool origin_ok(const Monitor *mon, const Req *q) {
    if (!q->origin[0]) return true; /* not from a page */
    char want[220];
    snprintf(want, sizeof want, "%s://%s", mon->cert ? "https" : "http", q->host);
    return !strcmp(q->origin, want);
}

static bool token_ok(const Monitor *mon, const char *auth) {
    if (strncmp(auth, "Bearer ", 7)) return false;
    const char *t = auth + 7;
    size_t n = strlen(t), m = strlen(mon->token);
    unsigned diff = (unsigned)(n ^ m); /* (in time that does not depend on how much matches) */
    for (size_t i = 0; i < m; i++) diff |= (unsigned)(t[i < n ? i : 0] ^ mon->token[i]);
    return diff == 0;
}

/* A path's segment, %-decoded: module names may be in it. */
static void url_decode(char *s) {
    char *o = s;
    for (; *s; s++) {
        int h, l;
        if (*s == '%' && s[1] && s[2] && sscanf(s + 1, "%1x%1x", (unsigned *)&h, (unsigned *)&l) == 2) {
            *o++ = (char)(h * 16 + l);
            s += 2;
        } else {
            *o++ = *s;
        }
    }
    *o = 0;
}

static void audit(VM *vm, const char *what, const char *json) {
    if (vm->mlog) vm_mlog_put(vm, 0, -1, what, json, strlen(json));
}

/* ---- mlwsvm serve: starting programs ---------------------------------------------------- *
 * The body of POST /api/programs is one JSON object of strings, and a list of
 * strings for "args": read here as such, nothing else needed. */

typedef struct {
    const char *s, *end;
} JText;

static void jt_ws(JText *t) {
    while (t->s < t->end && (*t->s == ' ' || *t->s == '\t' || *t->s == '\n' || *t->s == '\r')) t->s++;
}

/* A JSON string into out (malloc'd), or false. */
static bool jt_str(JText *t, char **out) {
    jt_ws(t);
    if (t->s >= t->end || *t->s != '"') return false;
    StrBuf b = {0};
    for (t->s++; t->s < t->end && *t->s != '"'; t->s++) {
        char c = *t->s;
        if (c != '\\') {
            sb_putc(&b, c);
            continue;
        }
        if (++t->s >= t->end) break;
        switch (*t->s) {
        case 'n': sb_putc(&b, '\n'); break;
        case 't': sb_putc(&b, '\t'); break;
        case 'r': sb_putc(&b, '\r'); break;
        case 'u': { /* (a character of the Basic Multilingual Plane, as UTF-8) */
            unsigned u = 0;
            if (t->end - t->s < 5 || sscanf(t->s + 1, "%4x", &u) != 1) goto bad;
            if (u < 0x80) sb_putc(&b, (char)u);
            else if (u < 0x800) sb_putc(&b, (char)(0xC0 | u >> 6)), sb_putc(&b, (char)(0x80 | (u & 63)));
            else sb_putc(&b, (char)(0xE0 | u >> 12)), sb_putc(&b, (char)(0x80 | (u >> 6 & 63))), sb_putc(&b, (char)(0x80 | (u & 63)));
            t->s += 4;
            break;
        }
        default: sb_putc(&b, *t->s); /* \" \\ \/ */
        }
    }
    if (t->s >= t->end) goto bad;
    t->s++;
    *out = b.s ? b.s : xstrdup("");
    return true;
bad:
    sb_free(&b);
    return false;
}

typedef struct {
    char *path, *cap, *quota;
    StrVec args;
} StartReq;

static void start_req_free(StartReq *r) {
    free(r->path);
    free(r->cap);
    free(r->quota);
    for (size_t i = 0; i < r->args.n; i++) free(r->args.v[i]);
    VEC_FREE(r->args);
}

static bool start_req_parse(const char *body, size_t n, StartReq *r) {
    JText t = {body, body + n};
    jt_ws(&t);
    if (t.s >= t.end || *t.s++ != '{') return false;
    jt_ws(&t);
    if (t.s < t.end && *t.s == '}') return false; /* (a path is needed) */
    for (;;) {
        char *key = NULL;
        if (!jt_str(&t, &key)) return false;
        jt_ws(&t);
        if (t.s >= t.end || *t.s++ != ':') return free(key), false;
        bool ok = true;
        if (!strcmp(key, "args")) {
            jt_ws(&t);
            ok = t.s < t.end && *t.s++ == '[';
            jt_ws(&t);
            if (ok && t.s < t.end && *t.s == ']') t.s++;
            else
                while (ok) {
                    char *a = NULL;
                    if (!(ok = jt_str(&t, &a))) break;
                    VEC_PUSH(r->args, a);
                    jt_ws(&t);
                    if (t.s < t.end && *t.s == ',') t.s++;
                    else if (t.s < t.end && *t.s == ']') {
                        t.s++;
                        break;
                    } else ok = false;
                }
        } else {
            char **slot = !strcmp(key, "path") ? &r->path : !strcmp(key, "cap") ? &r->cap : !strcmp(key, "quota") ? &r->quota : NULL;
            char *v = NULL;
            ok = slot && !*slot && jt_str(&t, &v);
            if (ok) *slot = v;
            else free(v);
        }
        free(key);
        if (!ok) return false;
        jt_ws(&t);
        if (t.s < t.end && *t.s == ',') {
            t.s++;
            continue;
        }
        if (t.s < t.end && *t.s == '}') break;
        return false;
    }
    t.s++;
    jt_ws(&t);
    return t.s == t.end && r->path;
}

/* A program's file: within the programs' directory once its links are
 * followed, or NULL. */
static char *program_file(const Monitor *mon, const char *rel) {
    char *joined = path_join(mon->programs, rel), *real = plat_realpath(joined);
    free(joined);
    size_t n = strlen(mon->programs);
    if (!real || strncmp(real, mon->programs, n) || (real[n] != '/' && real[n] != '\\') || !file_exists(real) ||
        plat_is_dir(real)) {
        free(real);
        return NULL;
    }
    return real;
}

static void serve_programs(Monitor *mon, VM *vm, Conn *k, Req *q) {
    StrBuf b = {0}, a = {0};
    size_t id = 0;
    char verb[16] = "";
    if (!strcmp(q->path, "/api/programs")) {
        StartReq r = {0};
        if (!start_req_parse(q->body ? q->body : "", q->nbody, &r)) {
            start_req_free(&r);
            respond_json(k, 400, "{\"error\":\"expected {\\\"path\\\": FILE, \\\"args\\\": [...], \\\"cap\\\": ..., \\\"quota\\\": ...}\"}");
            return;
        }
        char *file = program_file(mon, r.path);
        if (!file) {
            start_req_free(&r);
            respond_json(k, 403, "{\"error\":\"no such program in the programs' directory\"}");
            return;
        }
        MlwsError e = {{0}, {0}};
        sched_stop_world(vm); /* (loading and starting while nothing else runs) */
        Program *pr = vm_start_program(vm, file, &r.args, r.cap, r.quota, &e);
        sched_start_world(vm);
        if (pr) {
            sb_printf(&b, "{\"ok\":true,\"id\":%zu,\"pid\":%llu}", vm->progs.n - 1, (unsigned long long)pr->main->pid);
            sb_puts(&a, "{\"path\":");
            json_str(&a, r.path);
            sb_printf(&a, ",\"pid\":%llu}", (unsigned long long)pr->main->pid);
            audit(vm, "monitor.start", a.s);
            respond_json(k, 200, b.s);
        } else {
            sb_puts(&b, "{\"error\":");
            json_str(&b, e.msg);
            sb_puts(&b, ",\"code\":");
            json_str(&b, e.code);
            sb_putc(&b, '}');
            respond_json(k, 400, b.s);
        }
        free(file);
        start_req_free(&r);
    } else if (sscanf(q->path, "/api/programs/%zu/%15[a-z]", &id, verb) == 2 && !strcmp(verb, "stop")) {
        uint64_t pid = 0;
        plat_lock(vm->lock);
        if (id >= 1 && id < vm->progs.n) pid = vm->progs.v[id]->main->pid;
        plat_unlock(vm->lock);
        if (pid && sched_kill_from_outside(vm, pid, "E_KILLED", "stopped from the monitor")) {
            sb_printf(&a, "{\"id\":%zu,\"pid\":%llu}", id, (unsigned long long)pid);
            audit(vm, "monitor.stop", a.s);
            respond_json(k, 200, "{\"ok\":true}");
        } else {
            respond_json(k, 404, "{\"error\":\"no such program running\"}");
        }
    } else if (!strcmp(q->path, "/api/shutdown")) {
        audit(vm, "monitor.shutdown", "{}");
        respond_json(k, 200, "{\"ok\":true}");
        vm_serve_stop(vm);
    } else {
        respond_json(k, 404, "{\"error\":\"no such action\"}");
    }
    sb_free(&a);
    sb_free(&b);
}

static void serve_action(Monitor *mon, VM *vm, Conn *k, Req *q) {
    if (!mon->token) {
        respond_json(k, 403, "{\"error\":\"actions are off: start with --monitor-actions\"}");
        return;
    }
    if (!host_ok(mon, q->host)) {
        respond_json(k, 403, "{\"error\":\"not this monitor's Host\"}");
        return;
    }
    if (!origin_ok(mon, q)) {
        respond_json(k, 403, "{\"error\":\"a request from another site\"}");
        return;
    }
    if (!token_ok(mon, q->auth)) {
        respond_json(k, 401, "{\"error\":\"the token is needed: Authorization: Bearer TOKEN\"}");
        return;
    }
    if (mon->programs && (!strncmp(q->path, "/api/programs", 13) || !strcmp(q->path, "/api/shutdown"))) {
        serve_programs(mon, vm, k, q);
        return;
    }
    StrBuf b = {0}, a = {0};
    char *path = q->path, err[300] = "";
    uint64_t pid = 0;
    char verb[16] = "", name[256] = "";
    if (sscanf(path, "/api/proc/%llu/%15[a-z]", (unsigned long long *)&pid, verb) == 2 && !strcmp(verb, "kill")) {
        if (sched_kill_from_outside(vm, pid, "E_KILLED", "killed from the monitor")) {
            sb_printf(&a, "{\"pid\":%llu}", (unsigned long long)pid);
            audit(vm, "monitor.kill", a.s);
            respond_json(k, 200, "{\"ok\":true}");
        } else {
            respond_json(k, 404, "{\"error\":\"no such process\"}");
        }
    } else if (sscanf(path, "/api/proc/%llu/%15[a-z]", (unsigned long long *)&pid, verb) == 2 && !strcmp(verb, "send")) {
        if (sched_post_json(vm, pid, q->body ? q->body : "", q->nbody, err, sizeof err)) {
            sb_printf(&a, "{\"pid\":%llu,\"message\":%s}", (unsigned long long)pid, q->body);
            audit(vm, "monitor.send", a.s);
            respond_json(k, 200, "{\"ok\":true}");
        } else {
            sb_puts(&b, "{\"error\":");
            json_str(&b, err);
            sb_putc(&b, '}');
            respond_json(k, strstr(err, "does not exist") ? 404 : 400, b.s);
        }
    } else if (sscanf(path, "/api/modules/%255[^/]/%15[a-z]", name, verb) == 2 &&
               (!strcmp(verb, "reload") || !strcmp(verb, "purge"))) {
        url_decode(name);
        MlwsError e = {{0}, {0}};
        bool ok;
        if (!strcmp(verb, "reload")) {
            ok = vm_reload(vm, name, NULL, NULL, &e) != NULL;
            if (ok) sb_puts(&b, "{\"ok\":true}");
        } else {
            uint32_t freed = 0, kept = 0;
            ok = vm_purge(vm, name, NULL, &freed, &kept, &e);
            if (ok) sb_printf(&b, "{\"ok\":true,\"freed\":%u,\"kept\":%u}", freed, kept);
        }
        if (ok) {
            sb_puts(&a, "{\"module\":");
            json_str(&a, name);
            sb_putc(&a, '}');
            audit(vm, !strcmp(verb, "reload") ? "monitor.reload" : "monitor.purge", a.s);
            respond_json(k, 200, b.s);
        } else {
            sb_puts(&b, "{\"error\":");
            json_str(&b, e.msg);
            sb_puts(&b, ",\"code\":");
            json_str(&b, e.code);
            sb_putc(&b, '}');
            respond_json(k, !strcmp(e.code, "E_LOAD_NOT_FOUND") ? 404 : 400, b.s);
        }
    } else {
        respond_json(k, 404, "{\"error\":\"no such action\"}");
    }
    sb_free(&a);
    sb_free(&b);
}

static void monitor_poll(VM *vm, uint64_t wait_ns) {
    Monitor *mon = ((Tools *)vm->tools)->mon;
    if (wait_ns) plat_wait_readable(mon->fd, wait_ns); /* idle: sleep until a request or the timer */
    int c;
    while ((c = plat_tcp_accept(mon->fd)) >= 0) {
        Conn k = {c, NULL, plat_now_ns() + REQ_DEADLINE_NS};
        if (mon->cert) { /* TLS first */
            char err[256];
            plat_fd_set_nonblocking(c, true);
            k.tls = tls_server(c, mon->cert, mon->key, err, sizeof err);
            if (!k.tls) {
                plat_sock_close(c);
                continue;
            }
            int r;
            while ((r = tls_handshake(k.tls, err, sizeof err)) != TLS_OK)
                if (r == TLS_FAILED || !tls_wait(&k, r)) break;
            if (r != TLS_OK) {
                conn_close(&k);
                continue;
            }
        }
        Req q = {0};
        if (read_request(&k, &q)) {
            if (q.bad_len) {
                respond_json(&k, 413, "{\"error\":\"the body is over 64 KiB\"}");
            } else if (!strcmp(q.method, "GET")) {
                sched_stop_world(vm); /* other worker threads pause while we read their processes */
                serve_get(mon, vm, &k, &q);
                sched_start_world(vm);
            } else if (!strcmp(q.method, "POST")) {
                serve_action(mon, vm, &k, &q);
            } else {
                respond(&k, 405, "text/plain", "GET, or POST for an action\n", 27, false);
            }
        }
        free(q.body);
        conn_close(&k);
    }
}

bool tools_monitor(VM *vm, const char *addr, char *err, size_t errlen) {
    char host[64] = "127.0.0.1";
    int port;
    const char *colon = strrchr(addr, ':');
    if (colon) {
        size_t hl = (size_t)(colon - addr);
        if (hl >= sizeof host) hl = sizeof host - 1;
        if (hl) memcpy(host, addr, hl), host[hl] = 0;
        port = atoi(colon + 1);
    } else {
        port = atoi(addr);
    }
    if (port <= 0 || port > 65535) {
        snprintf(err, errlen, "bad monitor address '%s' (expected HOST:PORT or PORT)", addr);
        return false;
    }
    int fd = plat_tcp_listen(host, port, err, errlen);
    if (fd < 0) return false;
    Tools *t = tools_of(vm);
    t->mon = xcalloc(1, sizeof *t->mon);
    t->mon->fd = fd;
    snprintf(t->mon->addr, sizeof t->mon->addr, "%s:%d", host, port);
    snprintf(t->mon->host, sizeof t->mon->host, "%s", host);
    t->mon->port = port;
    vm->poll = monitor_poll;
    vm_mlog_enable(vm, 1000); /* monitor.log and monitor.event keep what they are given */
    return true;
}

const char *tools_monitor_actions(VM *vm, const char *token) {
    Monitor *mon = ((Tools *)vm->tools)->mon;
    if (token && *token) {
        mon->token = xstrdup(token);
    } else { /* 128 random bits, as hex */
        uint8_t r[16];
        mon->token = xmalloc(33);
        if (!plat_random_bytes(r, sizeof r)) {
            for (int i = 0; i < 16; i++) r[i] = (uint8_t)(plat_now_ns() >> (i % 8 * 8)); /* (never: the OS has entropy) */
        }
        for (int i = 0; i < 16; i++) snprintf(mon->token + 2 * i, 3, "%02x", r[i]);
    }
    return mon->token;
}

bool tools_monitor_tls(VM *vm, const char *cert, const char *key, char *err, size_t errlen) {
    if (!tls_available()) {
        snprintf(err, errlen, "--monitor-cert: TLS needs OpenSSL or LibreSSL, and neither was found");
        return false;
    }
    Monitor *mon = ((Tools *)vm->tools)->mon;
    mon->cert = xstrdup(cert);
    mon->key = xstrdup(key);
    return true;
}

bool tools_monitor_serve(VM *vm, const char *dir, char *err, size_t errlen) {
    Monitor *mon = ((Tools *)vm->tools)->mon;
    char *real = plat_realpath(dir);
    if (!real || !plat_is_dir(real)) {
        free(real);
        snprintf(err, errlen, "--programs: '%s' is not a directory", dir);
        return false;
    }
    mon->programs = real;
    return true;
}

const char *tools_monitor_scheme(VM *vm) {
    Tools *t = vm->tools;
    return t && t->mon && t->mon->cert ? "https" : "http";
}

const char *tools_monitor_addr(VM *vm) {
    Tools *t = vm->tools;
    return t && t->mon ? t->mon->addr : "";
}

void tools_monitor_close(VM *vm) {
    Tools *t = vm->tools;
    if (!t || !t->mon) return;
    plat_sock_close(t->mon->fd);
    VEC_FREE(t->mon->last_cpu);
    free(t->mon->token);
    free(t->mon->cert);
    free(t->mon->key);
    free(t->mon->programs);
    free(t->mon);
    t->mon = NULL;
    vm->poll = NULL;
}
