/* monitor.c - `--monitor HOST:PORT`: a tiny HTTP/1.0 server polled by the
 * first worker thread between time slices, so it needs no thread of its own.
 * Each request is served with the other workers paused (sched_stop_world), so
 * it can read every process. It serves read-only JSON statistics, a plain-text
 * table for `mlwsvm top`, and the embedded web dashboard. See docs/TOOLING.md. */
#include "../heap/heap.h"
#include "../platform/platform.h"
#include "tools.h"
#include "tools_internal.h"

extern const char DASHBOARD_HTML[];
extern const size_t DASHBOARD_HTML_LEN;

typedef struct Monitor {
    int fd;
    char addr[96]; /* HOST:PORT */
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
        sb_printf(&f, "%s.%s", fr->fn->mod->m->name, fr->fn->name);
        json_str(b, f.s);
        sb_free(&f);
        sb_printf(b, ",\"line\":%u", fr->fn->mf->lines && pc < fr->fn->mf->ninsns ? fr->fn->mf->lines[pc] : 0);
    } else {
        sb_puts(b, "null,\"line\":0");
    }
    sb_printf(b,
              ",\"insns\":%llu,\"cpu_ns\":%llu,\"heap_bytes\":%llu,\"heap_peak\":%llu,\"raw_bytes\":%llu,"
              "\"stack_bytes\":%u,\"depth\":%u,\"mailbox\":%zu,\"msgs_sent\":%llu,\"msgs_received\":%llu,"
              "\"gc_count\":%llu,\"gc_ns\":%llu,\"exceptions\":%llu,\"objects\":%llu",
              (unsigned long long)p->insns, (unsigned long long)p->cpu_ns, (unsigned long long)p->heap_bytes,
              (unsigned long long)p->heap_peak, (unsigned long long)p->raw_bytes, stack_bytes(p), p->nframes,
              (size_t)p->mcount, (unsigned long long)p->msgs_sent, (unsigned long long)p->msgs_received,
              (unsigned long long)p->gc_count, (unsigned long long)p->gc_ns, (unsigned long long)p->exceptions_thrown,
              (unsigned long long)(p->objects_allocated - p->gc_freed_objects));
    sb_puts(b, ",\"links\":[");
    for (size_t i = 0; i < p->links.n; i++) sb_printf(b, "%s%llu", i ? "," : "", (unsigned long long)p->links.v[i]);
    sb_puts(b, "],\"monitors\":[");
    for (size_t i = 0; i < p->monitors.n; i++)
        sb_printf(b, "%s%llu", i ? "," : "", (unsigned long long)p->monitors.v[i]);
    sb_puts(b, "]");
    if (detail) {
        static const char *const qn[] = {"heap", "raw", "mailbox", "insns", "foreign", "os"};
        sb_puts(b, ",\"quota\":{");
        for (int q = 0; q < QUOTA__COUNT; q++) {
            sb_printf(b, "%s\"%s\":", q ? "," : "", qn[q]);
            if (p->quota[q] == UINT64_MAX) sb_puts(b, "null");
            else sb_printf(b, "%llu", (unsigned long long)p->quota[q]);
        }
        sb_puts(b, "},\"backtrace\":[");
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
            const char *src = fr->fn->mod->m->source && *fr->fn->mod->m->source ? fr->fn->mod->m->source : fr->fn->mod->path;
            uint32_t line = fr->fn->mf->lines && fr->pc < fr->fn->mf->ninsns ? fr->fn->mf->lines[fr->pc] : 0;
            sb_printf(&loc, "%s.%s+%u (%s:%u)", fr->fn->mod->m->name, fr->fn->name, fr->pc, path_basename(src), line);
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

static void respond(int fd, int status, const char *type, const char *body, size_t n) {
    StrBuf h = {0};
    sb_printf(&h,
              "HTTP/1.0 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\n"
              "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",
              status, status == 200 ? "OK" : "Not Found", type, n);
    if (plat_sock_write_all(fd, h.s, h.n)) plat_sock_write_all(fd, body, n);
    sb_free(&h);
}

static void serve(Monitor *mon, VM *vm, int fd) {
    char req[2048];
    size_t n = 0;
    while (n < sizeof req - 1) { /* read the request head (bodies are not used) */
        long r = plat_sock_read(fd, req + n, sizeof req - 1 - n, 200000000);
        if (r <= 0) break;
        n += (size_t)r;
        req[n] = 0;
        if (strstr(req, "\r\n\r\n") || strstr(req, "\n\n")) break;
    }
    req[n] = 0;
    char method[8] = {0}, path[256] = {0};
    if (sscanf(req, "%7s %255s", method, path) != 2 || strcmp(method, "GET") != 0) {
        respond(fd, 404, "text/plain", "only GET is supported\n", 22);
        return;
    }
    char *q = strchr(path, '?');
    if (q) *q = 0;
    StrBuf b = {0};
    const char *json = "application/json";
    if (!strcmp(path, "/") || !strcmp(path, "/index.html")) {
        respond(fd, 200, "text/html; charset=utf-8", DASHBOARD_HTML, DASHBOARD_HTML_LEN);
        return;
    } else if (!strcmp(path, "/api/vm")) {
        vm_json(&b, vm);
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
            respond(fd, 404, json, "{\"error\":\"no such process\"}", 27);
            return;
        }
        proc_json(&b, p, true);
    } else if (!strcmp(path, "/api/exits")) {
        exits_json(&b, vm);
    } else if (!strcmp(path, "/top")) {
        top_text(&b, mon, vm);
        respond(fd, 200, "text/plain; charset=utf-8", b.s, b.n);
        sb_free(&b);
        return;
    } else {
        respond(fd, 404, "text/plain", "not found\n", 10);
        return;
    }
    respond(fd, 200, json, b.s, b.n);
    sb_free(&b);
}

static void monitor_poll(VM *vm, uint64_t wait_ns) {
    Monitor *mon = ((Tools *)vm->tools)->mon;
    if (wait_ns) plat_wait_readable(mon->fd, wait_ns); /* idle: sleep until a request or the timer */
    int c;
    while ((c = plat_tcp_accept(mon->fd)) >= 0) {
        sched_stop_world(vm); /* other worker threads pause while we read their processes */
        serve(mon, vm, c);
        sched_start_world(vm);
        plat_sock_close(c);
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
    vm->poll = monitor_poll;
    return true;
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
    free(t->mon);
    t->mon = NULL;
    vm->poll = NULL;
}
