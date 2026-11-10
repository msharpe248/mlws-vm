/* monitor.c - `--monitor HOST:PORT`: a tiny HTTP/1.0 server polled by the
 * first worker thread between time slices, so it needs no thread of its own,
 * and it waits for no client: a connection is read and written as far as it
 * goes at each poll. A read is answered with the other workers paused
 * (sched_stop_world), so it can read every process (one in a blocking call an
 * I/O thread is making, as it was when the call began: sched_seen). It serves
 * JSON statistics, a plain-text table for `mlwsvm top`, the embedded web
 * dashboard, and, to an admin's key, actions. See docs/TOOLING.md. */
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

#define CONNS_MAX 32 /* connections at once; others wait to be accepted */

/* Wrong keys, counted by the address they came from: WRONG_MAX at once, and
 * one more every WRONG_AGAIN_NS. Past that, a request with a key is refused
 * unread (429), so a key cannot be found by trying. The same wrong key again
 * (a page left open with a key since revoked) counts once. */
#define WRONG_ADDRS 16
#define WRONG_MAX 10
#define WRONG_AGAIN_NS 3000000000ull
#define WRONG_LOG_NS 60000000000ull /* the log says so once a minute for an address */
typedef struct {
    char ip[64];
    uint32_t n;        /* wrong keys counted, less those the time since has given back */
    uint64_t since_ns; /* when the count was last brought up to date */
    uint64_t logged_ns;
    uint8_t last[32];  /* the last wrong key's hash */
} Wrong;

struct Conn;

typedef struct Monitor {
    int fd;
    struct Conn *conns[CONNS_MAX];
    int nconns;
    Wrong wrong[WRONG_ADDRS];
    char addr[96]; /* HOST:PORT */
    char host[64];
    int port;
    bool actions;      /* --monitor-actions, or mlwsvm serve: actions allowed, to an admin key */
    char *token;       /* the key printed at the start (an admin's); NULL: none */
    MonKeyVec keys;    /* every key: its own, the tokens file's, and those made with no file */
    char *keyfile;     /* --monitor-tokens: NULL none */
    int64_t keyfile_mtime, keyfile_size;
    uint32_t keyfile_mode;
    bool keyfile_seen;
    bool need_key;     /* (a tokens file) every request but the page's needs a key */
    char *cert, *key;  /* --monitor-cert, --monitor-key: TLS */
    char *programs;    /* mlwsvm serve: the directory programs are started from (real path); NULL: not serving */
    /* previous /top sample, to turn cumulative CPU time into a percentage */
    VEC(uint64_t) last_cpu;
    uint64_t last_top_ns;
} Monitor;

static void json_str(StrBuf *b, const char *s);

/* A program's name for the page: its file within mlwsvm serve's programs'
 * directory, or its file's name. */
static const char *prog_name(const Monitor *mon, const Program *pr) {
    size_t n = mon->programs ? strlen(mon->programs) : 0;
    if (n && !strncmp(pr->path, mon->programs, n) && (pr->path[n] == '/' || pr->path[n] == '\\')) return pr->path + n + 1;
    return path_basename(pr->path);
}

/* ,"program":ID,"program_name":NAME for a program's id (none for -1). */
static void prog_json(StrBuf *b, const Monitor *mon, VM *vm, int32_t id) {
    if (id < 0 || (size_t)id >= vm->progs.n) {
        sb_puts(b, ",\"program\":null,\"program_name\":null");
        return;
    }
    sb_printf(b, ",\"program\":%d,\"program_name\":", id);
    json_str(b, prog_name(mon, vm->progs.v[id]));
}

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

/* A process, read as sched_seen gives it: one whose blocking call an I/O
 * thread is still making is shown as it was when the call began (`away`), with
 * where it made the call for a backtrace, and no messages. */
static void proc_json(StrBuf *b, const Monitor *mon, VM *vm, Process *p, bool detail) {
    ProcSeen s;
    plat_lock(p->lock);
    sched_seen(p, &s);
    sb_printf(b, "{\"pid\":%llu,\"state\":", (unsigned long long)p->pid);
    json_str(b, STATE_NAMES[s.state]);
    sb_printf(b, ",\"away\":%s,\"entry\":", s.away ? "true" : "false");
    json_str(b, p->entry ? p->entry->name : "?");
    prog_json(b, mon, vm, p->prog ? (int32_t)p->prog->id : -1);
    const char *env = p->env ? env_name(p->env) : NULL; /* (NULL: its program's own) */
    sb_puts(b, ",\"env\":");
    if (env) json_str(b, env);
    else sb_puts(b, "null");
    sb_puts(b, ",\"function\":");
    if (s.fn) {
        StrBuf f = {0};
        fn_qualname(&f, s.fn);
        json_str(b, f.s);
        sb_free(&f);
        sb_puts(b, ",\"file\":");
        json_str(b, fn_file(s.fn));
        sb_printf(b, ",\"line\":%u", s.fn->mf->lines && s.pc < s.fn->mf->ninsns ? s.fn->mf->lines[s.pc] : 0);
        const char *file;
        uint32_t sline;
        if (module_srcpos(s.fn->mod->m, s.fn->mf, s.pc, &file, &sline)) { /* `.loc`: the program's own line */
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
              (unsigned long long)s.insns, (unsigned long long)s.cpu_ns, (unsigned long long)s.heap_bytes,
              (unsigned long long)s.heap_peak, (unsigned long long)s.raw_bytes, s.stack_bytes, s.depth, s.mailbox,
              (unsigned long long)s.msgs_sent, (unsigned long long)s.msgs_received, (unsigned long long)s.gc_count,
              (unsigned long long)s.gc_ns, (unsigned long long)s.thrown, (unsigned long long)s.objects);
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
        for (size_t i = p->mbox_head; !s.away && i < p->mbox.n && shown < 20; i++, shown++) {
            if (shown) sb_putc(b, ',');
            value_brief(b, p->mbox.v[i].v, p->mbox.v[i].tag);
        }
        for (size_t i = 0; !s.away && i < p->inbox.n && shown < 20; i++, shown++) {
            if (shown) sb_putc(b, ',');
            value_brief(b, p->inbox.v[i].v, p->inbox.v[i].tag);
        }
        sb_puts(b, "],\"backtrace\":[");
        if (s.away && s.fn) { /* (its frames are its call's thread's: where the call was made) */
            StrBuf loc = {0};
            fmt_location(&loc, s.fn, s.pc);
            json_str(b, loc.s);
            sb_free(&loc);
        }
        for (uint32_t i = s.away ? 0 : p->nframes; i-- > 0;) {
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
    plat_unlock(p->lock);
}

/* A process's counters alone. */
static void proc_seen(Process *p, ProcSeen *s) {
    plat_lock(p->lock);
    sched_seen(p, s);
    plat_unlock(p->lock);
}

static void vm_json(StrBuf *b, VM *vm) {
    uint64_t insns = vm->insns_retired, heap = 0, raw = 0;
    for (uint64_t pid = 1, end = vm_pid_end(vm); pid < end; pid++) {
        Process *p = vm_proc_at(vm, pid);
        if (!p) continue;
        ProcSeen s;
        proc_seen(p, &s);
        insns += s.insns;
        heap += s.heap_bytes;
        raw += s.raw_bytes;
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

/* A connection's traffic, since it came: the counts (NodeStats), and what its
 * carrier has yet to write and who waits for it to (back-pressure). */
typedef struct {
    uint64_t up_ms, sent, received, bytes_out, bytes_in, waits, queued, waiting;
} NodeTraffic;

static NodeTraffic node_traffic(VM *vm, uint32_t n, uint64_t carrier, uint64_t now) {
    NodeTraffic t = {0};
    const NodeStats *st = node_stats_of(vm, n);
    if (st) {
        t.up_ms = (now - st->up_ns) / 1000000;
        t.sent = atomic_load(&st->sent), t.received = atomic_load(&st->received);
        t.bytes_out = atomic_load(&st->bytes_out), t.bytes_in = atomic_load(&st->bytes_in);
        t.waits = atomic_load(&st->waits);
    }
    const Process *c = vm_proc_at(vm, carrier); /* (the world is paused for the monitor) */
    if (c) t.queued = c->qbytes, t.waiting = c->busy_senders.n;
    return t;
}

/* This VM's node number (0: none) and name ("": none), and the nodes it is
 * connected to, with their names, the process carrying each connection, and
 * its traffic. */
static void nodes_json(StrBuf *b, VM *vm) {
    uint64_t now = plat_now_ns();
    plat_lock(vm->lock); /* (the names) */
    sb_printf(b, "{\"self\":%u,\"name\":", vm->node_self);
    json_str(b, vm->node_name ? vm->node_name : "");
    sb_puts(b, ",\"connected\":[");
    bool first = true;
    for (uint32_t n = 1; vm->node_route && n <= 65535; n++) {
        uint64_t c = __atomic_load_n(&vm->node_route[n], __ATOMIC_ACQUIRE);
        if (!c) continue;
        sb_printf(b, "%s{\"node\":%u,\"name\":", first ? "" : ",", n);
        json_str(b, vm->node_names && vm->node_names[n] ? vm->node_names[n] : "");
        NodeTraffic t = node_traffic(vm, n, c, now);
        sb_printf(b,
                  ",\"carrier\":%llu,\"up_ms\":%llu,\"sent\":%llu,\"received\":%llu,\"bytes_out\":%llu,\"bytes_in\":%llu,"
                  "\"queued\":%llu,\"waiting\":%llu,\"waits\":%llu}",
                  (unsigned long long)c, (unsigned long long)t.up_ms, (unsigned long long)t.sent,
                  (unsigned long long)t.received, (unsigned long long)t.bytes_out, (unsigned long long)t.bytes_in,
                  (unsigned long long)t.queued, (unsigned long long)t.waiting, (unsigned long long)t.waits);
        first = false;
    }
    plat_unlock(vm->lock);
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
static void log_json(StrBuf *b, const Monitor *mon, VM *vm, uint64_t since) {
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
        sb_printf(b, "%s{\"seq\":%llu,\"t_ns\":%llu,\"pid\":%llu", q == from ? "" : ",", (unsigned long long)e->seq,
                  (unsigned long long)e->t_ns, (unsigned long long)e->pid);
        prog_json(b, mon, vm, e->prog);
        sb_putc(b, ',');
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

/* The same entries as text, for `mlwsvm top --log`: a first line with the
 * number to ask from next, then a line for each entry (more, where its text
 * has newlines): time, pid, program, level, text. The dashboard's Download
 * writes the same lines. */
static void log_text(StrBuf *b, const Monitor *mon, VM *vm, uint64_t since) {
    if (!vm->mlog) {
        sb_puts(b, "0\n");
        return;
    }
    plat_lock(vm->mlog_lock);
    uint64_t last = vm->mlog_seq, first = last > vm->mlog_cap ? last - vm->mlog_cap + 1 : 1;
    if (since > last) since = last;
    uint64_t from = since + 1 > first ? since + 1 : first;
    sb_printf(b, "%llu\n", (unsigned long long)last);
    if (from > since + 1) sb_printf(b, "(%llu lines were overwritten before they were read)\n", (unsigned long long)(from - (since + 1)));
    for (uint64_t q = from; q <= last; q++) {
        const MLogEntry *e = &vm->mlog[(q - 1) % vm->mlog_cap];
        char pid[24] = "-";
        if (e->pid) snprintf(pid, sizeof pid, "%llu", (unsigned long long)e->pid);
        const char *prog = e->prog >= 0 && (size_t)e->prog < vm->progs.n ? prog_name(mon, vm->progs.v[e->prog]) : e->pid ? "-" : "monitor";
        sb_printf(b, "%9.3f s  %4s  %s  %-5s  ", (double)e->t_ns / 1e9, pid, prog, e->level < 0 ? "event" : LOG_LEVELS[e->level]);
        if (e->level < 0) sb_printf(b, "%s ", e->name);
        sb_puts(b, e->text);
        sb_putc(b, '\n');
    }
    plat_unlock(vm->mlog_lock);
}

static void exits_json(StrBuf *b, const Monitor *mon, VM *vm) {
    sb_putc(b, '[');
    size_t n = vm->exits.n;
    for (size_t k = 0; k < n; k++) { /* newest first */
        const ExitRecord *r = &vm->exits.v[(vm->exits_next + n - 1 - k) % n];
        sb_printf(b, "%s{\"pid\":%llu,\"entry\":", k ? "," : "", (unsigned long long)r->pid);
        json_str(b, r->entry);
        prog_json(b, mon, vm, r->prog);
        sb_printf(b, ",\"main\":%s,\"failed\":%s,\"code\":", r->main ? "true" : "false", r->failed ? "true" : "false");
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

typedef struct {
    uint64_t pid;
    ProcSeen s;
} TopRow;

static int cmp_cpu(const void *a, const void *b) {
    const TopRow *x = a, *y = b;
    return x->s.cpu_ns < y->s.cpu_ns ? 1 : x->s.cpu_ns > y->s.cpu_ns ? -1 : 0;
}

static void top_text(StrBuf *b, Monitor *mon, VM *vm) {
    uint64_t now = plat_now_ns(), insns = vm->insns_retired, heap = 0;
    VEC(TopRow) ps = {0};
    uint64_t end = vm_pid_end(vm);
    for (uint64_t pid = 1; pid < end; pid++) {
        Process *p = vm_proc_at(vm, pid);
        if (!p) continue;
        TopRow row = {.pid = p->pid};
        proc_seen(p, &row.s);
        insns += row.s.insns;
        heap += row.s.heap_bytes;
        VEC_PUSH(ps, row);
    }
    if (ps.n) qsort(ps.v, ps.n, sizeof *ps.v, cmp_cpu);
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
        const ProcSeen *s = &ps.v[i].s;
        uint64_t pid = ps.v[i].pid;
        /* %CPU since the previous /top sample; the first sample uses the whole uptime. */
        bool have = dt > 0 && pid - 1 < mon->last_cpu.n;
        uint64_t prev = have ? mon->last_cpu.v[pid - 1] : 0;
        double span = have ? dt : (double)(now - vm->start_ns);
        double cpu = span > 0 ? 100.0 * (double)(s->cpu_ns - prev) / span : 0;
        char in[16], h[16], r[16], msgs[48];
        human(in, sizeof in, (double)s->insns);
        human(h, sizeof h, (double)s->heap_bytes);
        human(r, sizeof r, (double)s->raw_bytes);
        snprintf(msgs, sizeof msgs, "%llu/%llu", (unsigned long long)s->msgs_received, (unsigned long long)s->msgs_sent);
        StrBuf loc = {0};
        if (s->fn) { /* compact: module.function+pc (file:line), file without its directory */
            fn_qualname(&loc, s->fn);
            sb_printf(&loc, "+%u (", s->pc);
            fn_where(&loc, s->fn, s->pc, true);
            sb_putc(&loc, ')');
        }
        sb_printf(b, "%6llu %-8s %5.1f %8s %8s %8s %7u %5zu %11s %4llu  %s\n", (unsigned long long)pid,
                  STATE_NAMES[s->state], cpu, in, h, r, s->stack_bytes, s->mailbox, msgs,
                  (unsigned long long)s->gc_count, loc.s ? loc.s : "-");
        sb_free(&loc);
    }
    if (ps.n > 40) sb_printf(b, "   ... %zu more\n", ps.n - 40);
    /* remember CPU totals for the next sample */
    VEC_GROW(mon->last_cpu, end - 1);
    for (size_t i = mon->last_cpu.n; i < end - 1; i++) mon->last_cpu.v[i] = 0;
    mon->last_cpu.n = end - 1;
    for (size_t i = 0; i < ps.n; i++) mon->last_cpu.v[ps.v[i].pid - 1] = ps.v[i].s.cpu_ns;
    mon->last_top_ns = now;
    /* The nodes this VM is connected to: each connection's traffic. */
    bool head = false;
    for (uint32_t n = 1; vm->node_route && n <= 65535; n++) {
        uint64_t c = __atomic_load_n(&vm->node_route[n], __ATOMIC_ACQUIRE);
        if (!c) continue;
        if (!head) {
            plat_lock(vm->lock);
            sb_printf(b, "\nnodes (this VM: node %u%s%s):\n", vm->node_self, vm->node_name ? ", " : "",
                      vm->node_name ? vm->node_name : "");
            plat_unlock(vm->lock);
            sb_printf(b, "%6s %-20s %8s %8s %8s %8s %8s %8s %4s %6s\n", "NODE", "NAME", "UP", "SENT", "RECEIVED", "OUT",
                      "IN", "QUEUED", "WAIT", "WAITS");
            head = true;
        }
        NodeTraffic t = node_traffic(vm, n, c, now);
        char nm[21] = "", sent[16], recv[16], out[16], in[16], q[16];
        plat_lock(vm->lock);
        if (vm->node_names && vm->node_names[n]) snprintf(nm, sizeof nm, "%s", vm->node_names[n]);
        plat_unlock(vm->lock);
        human(sent, sizeof sent, (double)t.sent);
        human(recv, sizeof recv, (double)t.received);
        human(out, sizeof out, (double)t.bytes_out);
        human(in, sizeof in, (double)t.bytes_in);
        human(q, sizeof q, (double)t.queued);
        sb_printf(b, "%6u %-20s %7.1fs %8s %8s %7sB %7sB %7sB %4llu %6llu\n", n, nm[0] ? nm : "-", (double)t.up_ms / 1000,
                  sent, recv, out, in, q, (unsigned long long)t.waiting, (unsigned long long)t.waits);
    }
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

/* ---- connections: plain, or TLS (--monitor-cert, --monitor-key) ----------------------
 * No connection is ever waited for. Each is read as its bytes come, answered
 * into a buffer (a read with the world stopped) and written as its client takes
 * it, a step at each poll. So a client that sends slowly, or does not read,
 * holds one of the places until its time is up, and holds nothing else up. */

#define READ_DEADLINE_NS 2000000000ull   /* a request is read within 2 s, or dropped */
#define WRITE_DEADLINE_NS 10000000000ull /* and its answer taken within 10 s */
#define HEAD_MAX 8192
#define BODY_MAX 65536
#define DRAIN_NS 1000000000ull           /* after an answer to a request not all read: the rest is let come for 1 s */
#define OUT_MAX ((size_t)64 << 20) /* answers not yet taken: past this, a request is answered 503 */

enum { CONN_HANDSHAKE, CONN_READ, CONN_WRITE, CONN_DRAIN };
enum { IO_WAIT = -1, IO_FAIL = -2 }; /* conn_read and conn_write: not now; never */

typedef struct Conn {
    int fd;
    Tls *tls;
    int state;
    uint64_t deadline;
    char ip[64]; /* the client's address */
    StrBuf in;   /* the request, as far as it has come */
    StrBuf out;  /* the answer */
    size_t sent;
    bool unread; /* answered before all of the request was read (too big, or none) */
} Conn;

/* Bytes read (0: the client has closed), or IO_WAIT, or IO_FAIL. */
static long conn_read(Conn *c, char *buf, size_t n) {
    if (c->tls) {
        char err[160];
        long r = tls_read(c->tls, buf, n, err, sizeof err);
        return r >= 0 ? r : r == TLS_FAILED ? IO_FAIL : IO_WAIT;
    }
    int e = 0;
    long r = plat_fd_read(c->fd, buf, n, &e);
    return r >= 0 ? r : r == PLAT_IO_AGAIN ? IO_WAIT : IO_FAIL;
}

/* Bytes written (some), or IO_WAIT, or IO_FAIL. */
static long conn_write(Conn *c, const char *buf, size_t n) {
    if (c->tls) {
        char err[160];
        long r = tls_write(c->tls, buf, n, err, sizeof err);
        return r > 0 ? r : r == TLS_FAILED ? IO_FAIL : IO_WAIT;
    }
    int e = 0;
    long r = plat_fd_write(c->fd, buf, n, &e);
    return r > 0 ? r : r == PLAT_IO_AGAIN || r == 0 ? IO_WAIT : IO_FAIL;
}

static void conn_close(Conn *c) {
    if (c->tls) tls_release(c->tls); /* (and its descriptor) */
    else plat_sock_close(c->fd);
    sb_free(&c->in);
    sb_free(&c->out);
    free(c);
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
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 503: return "Service Unavailable";
    default: return "Error";
    }
}

/* The page: in no other site's frame, and running and fetching only what is its own. */
#define PAGE_HEADERS                                                                                                   \
    "X-Frame-Options: DENY\r\nReferrer-Policy: no-referrer\r\n"                                                        \
    "Content-Security-Policy: default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; "            \
    "connect-src 'self'; img-src data:; base-uri 'none'; form-action 'none'; frame-ancestors 'none'\r\n"

/* An answer, into the connection's buffer: conn_step writes it. Reads may be
 * shown to any page (Access-Control-Allow-Origin: *), as they always were; an
 * action's answer is for the monitor's own page only. */
static void respond(Conn *c, int status, const char *type, const char *body, size_t n, bool any_origin) {
    c->out.n = 0; /* (one answer to a request) */
    sb_printf(&c->out, "HTTP/1.0 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\n%s"
                       "X-Content-Type-Options: nosniff\r\n%sConnection: close\r\n\r\n",
              status, status_text(status), type, n, any_origin ? "Access-Control-Allow-Origin: *\r\n" : "",
              !strncmp(type, "text/html", 9) ? PAGE_HEADERS : "");
    sb_putn(&c->out, body, n);
}

static void respond_json(Conn *c, int status, const char *json) { respond(c, status, "application/json", json, strlen(json), false); }

/* ---- requests ------------------------------------------------------------------------ */

typedef struct {
    char method[8], path[512], host[160], origin[200], auth[200];
    char *body; /* (in the connection's buffer) */
    size_t nbody;
} Req;

/* Whether the n bytes at s start with p, case blind (ASCII). */
static bool starts_ci(const char *s, size_t n, const char *p) {
    for (; *p; s++, n--, p++)
        if (!n || tolower((unsigned char)*s) != tolower((unsigned char)*p)) return false;
    return true;
}

/* A header's value (name ending in ':'), case blind, into out: head is the
 * request's lines, n bytes of them. */
static void header(const char *head, size_t n, const char *name, char *out, size_t outlen) {
    size_t k = strlen(name);
    out[0] = 0;
    const char *z = head + n;
    for (const char *l = memchr(head, '\n', n); l; l = memchr(l, '\n', (size_t)(z - l))) {
        l++;
        if (!starts_ci(l, (size_t)(z - l), name)) continue;
        l += k;
        while (l < z && (*l == ' ' || *l == '\t')) l++;
        size_t m = 0;
        while (l + m < z && l[m] != '\r' && l[m] != '\n') m++;
        if (m >= outlen) m = outlen - 1;
        memcpy(out, l, m);
        out[m] = 0;
        return;
    }
}

/* The request in what has come: 1 with it in q, 0 if there is more to come, or
 * -1 for what is none, with the status to answer (0: nothing to say). */
static int request_of(Conn *c, Req *q, int *status) {
    *status = 0;
    if (!c->in.n) return 0;
    const char *end = strstr(c->in.s, "\r\n\r\n"); /* (a NUL before it: no request) */
    if (!end || (size_t)(end - c->in.s) + 4 > HEAD_MAX) {
        if (end || c->in.n >= HEAD_MAX) return *status = 431, -1;
        return strlen(c->in.s) < c->in.n ? -1 : 0;
    }
    size_t head = (size_t)(end - c->in.s) + 4;
    char first[600];
    snprintf(first, sizeof first, "%.*s", (int)strcspn(c->in.s, "\r\n"), c->in.s);
    if (sscanf(first, "%7s %511s", q->method, q->path) != 2) return *status = 400, -1;
    header(c->in.s, head, "Host:", q->host, sizeof q->host);
    header(c->in.s, head, "Origin:", q->origin, sizeof q->origin);
    header(c->in.s, head, "Authorization:", q->auth, sizeof q->auth);
    char len[32];
    header(c->in.s, head, "Content-Length:", len, sizeof len);
    unsigned long long clen = len[0] ? strtoull(len, NULL, 10) : 0;
    if (clen > BODY_MAX) return *status = 413, -1;
    if (c->in.n - head < clen) return 0;
    q->body = c->in.s + head;
    q->nbody = (size_t)clen;
    q->body[q->nbody] = 0; /* (within the buffer: at most its own terminator) */
    return 1;
}

/* GET /api/envs: each environment, the programs' and then the named ones,
 * with how many processes are in it and watch it, and its names, with their
 * values for an admin (secret ones never). */
static void envs_json(StrBuf *b, const Monitor *mon, VM *vm, bool values) {
    VEC(Env *) es = {0};
    VEC(int32_t) ids = {0};
    plat_lock(vm->lock);
    for (size_t i = mon->programs ? 1 : 0; i < vm->progs.n; i++) /* (not mlwsvm serve's own, nor one that has ended) */
        if (vm->progs.v[i]->env && !vm->progs.v[i]->ended) {
            env_retain(vm->progs.v[i]->env);
            VEC_PUSH(es, vm->progs.v[i]->env);
            VEC_PUSH(ids, (int32_t)i);
        }
    for (size_t i = 0; i < vm->env_named.cap; i++)
        if (vm->env_named.keys && vm->env_named.keys[i] && vm->env_named.vals[i]) {
            env_retain(vm->env_named.vals[i]);
            VEC_PUSH(es, vm->env_named.vals[i]);
            VEC_PUSH(ids, -1);
        }
    plat_unlock(vm->lock);
    sb_putc(b, '[');
    for (size_t i = 0; i < es.n; i++) {
        Env *e = es.v[i];
        size_t procs = 0;
        for (uint64_t pid = 1, end = vm_pid_end(vm); pid < end; pid++) {
            const Process *p = vm_proc_at(vm, pid);
            if (p && p->env == e) procs++;
        }
        sb_puts(b, i ? ",{\"env\":" : "{\"env\":");
        if (env_name(e)) json_str(b, env_name(e));
        else sb_puts(b, "null");
        prog_json(b, mon, vm, ids.v[i]);
        sb_printf(b, ",\"processes\":%zu,\"watchers\":%zu,\"values\":", procs, env_watchers(e));
        env_values_json(vm, e, values, b);
        sb_putc(b, '}');
        env_release(e);
    }
    sb_putc(b, ']');
    VEC_FREE(es);
    VEC_FREE(ids);
}

/* ---- keys (keys.c) -------------------------------------------------------------------- */

static void audit(VM *vm, const MonKey *by, const char *what, const char *json);

/* The tokens file's keys, if it is one to trust: whoever can write it can make
 * a key, so a file that other users can write, or own, gives none. */
static bool keyfile_load(const char *path, MonKeyVec *out, char *err, size_t errlen) {
    PlatStat st;
    int e;
    if (plat_stat(path, true, &st, &e) && !plat_path_trusted(path, err, errlen)) return false;
    return keyfile_read(path, out, err, errlen);
}

/* The tokens file's keys again, if it has changed since they were read: those
 * made otherwise stay, and each keeps when it was last used. If it no longer
 * reads, the keys stay as they were, and the log says why. */
static void keys_refresh(Monitor *mon, VM *vm) {
    if (!mon->keyfile) return;
    PlatStat st;
    int e;
    bool seen = plat_stat(mon->keyfile, true, &st, &e);
    if (seen == mon->keyfile_seen &&
        (!seen || (st.mtime_ns == mon->keyfile_mtime && st.size == mon->keyfile_size && st.mode == mon->keyfile_mode)))
        return;
    MonKeyVec fresh = {0};
    char err[600];
    if (!keyfile_load(mon->keyfile, &fresh, err, sizeof err)) {
        StrBuf a = {0};
        sb_puts(&a, "{\"error\":");
        json_str(&a, err);
        sb_putc(&a, '}');
        audit(vm, NULL, "monitor.keys_unread", a.s);
        sb_free(&a);
        VEC_FREE(fresh);
    } else {
        for (size_t i = 0; i < fresh.n; i++)
            for (size_t j = 0; j < mon->keys.n; j++)
                if (mon->keys.v[j].from == 'f' && !strcmp(mon->keys.v[j].label, fresh.v[i].label))
                    fresh.v[i].used_ns = mon->keys.v[j].used_ns;
        for (size_t j = 0; j < mon->keys.n; j++) /* (its own, then the file's) */
            if (mon->keys.v[j].from != 'f') VEC_PUSH(fresh, mon->keys.v[j]);
        VEC_FREE(mon->keys);
        mon->keys = fresh;
    }
    mon->keyfile_seen = seen;
    mon->keyfile_mtime = seen ? st.mtime_ns : 0;
    mon->keyfile_size = seen ? st.size : 0;
    mon->keyfile_mode = seen ? st.mode : 0;
}

/* The key an Authorization: Bearer TOKEN names, or NULL. Every key's hash is
 * compared whole, so the time taken says nothing of the token. */
static MonKey *key_of(Monitor *mon, const char *auth, uint8_t h[32]) {
    if (strncmp(auth, "Bearer ", 7)) auth = "", memset(h, 0, 32); /* (no token: no key's hash) */
    else key_hash(auth + 7, strlen(auth + 7), h);
    if (!*auth) return NULL;
    MonKey *found = NULL;
    for (size_t i = 0; i < mon->keys.n; i++) {
        unsigned diff = 0;
        for (int j = 0; j < 32; j++) diff |= (unsigned)(h[j] ^ mon->keys.v[i].hash[j]);
        if (!diff) found = &mon->keys.v[i];
    }
    if (found) found->used_ns = plat_now_ns();
    return found;
}

/* The count kept for an address, brought up to date; NULL if none is. */
static Wrong *wrong_of(Monitor *mon, const char *ip, uint64_t now) {
    for (int i = 0; i < WRONG_ADDRS; i++) {
        Wrong *w = &mon->wrong[i];
        if (!w->n) continue;
        uint64_t back = (now - w->since_ns) / WRONG_AGAIN_NS;
        if (back >= w->n) w->n = 0; /* (all given back: the place is free) */
        else w->n -= (uint32_t)back, w->since_ns += back * WRONG_AGAIN_NS;
        if (w->n && !strcmp(w->ip, ip)) return w;
    }
    return NULL;
}

/* May a key from this address be looked at? */
static bool wrong_room(Monitor *mon, const char *ip, uint64_t now) {
    Wrong *w = wrong_of(mon, ip, now);
    return !w || w->n < WRONG_MAX;
}

/* A wrong key came from an address: counted, unless it is the one that came
 * last. With no place free, the address with the fewest gives up its own. */
static void wrong_count(Monitor *mon, VM *vm, const char *ip, const uint8_t h[32], uint64_t now) {
    Wrong *w = wrong_of(mon, ip, now);
    if (w && !memcmp(w->last, h, 32)) return;
    if (!w) {
        w = &mon->wrong[0];
        for (int i = 1; i < WRONG_ADDRS; i++)
            if (mon->wrong[i].n < w->n) w = &mon->wrong[i];
        memset(w, 0, sizeof *w);
        snprintf(w->ip, sizeof w->ip, "%s", ip);
        w->since_ns = now;
    }
    memcpy(w->last, h, 32);
    if (++w->n < WRONG_MAX || (w->logged_ns && now - w->logged_ns < WRONG_LOG_NS)) return;
    w->logged_ns = now;
    StrBuf a = {0};
    sb_puts(&a, "{\"from\":");
    json_str(&a, ip);
    sb_putc(&a, '}');
    audit(vm, NULL, "monitor.keys_refused", a.s);
    sb_free(&a);
}

static MonKey *key_labelled(Monitor *mon, const char *label) {
    for (size_t i = 0; i < mon->keys.n; i++)
        if (!strcmp(mon->keys.v[i].label, label)) return &mon->keys.v[i];
    return NULL;
}

/* GET /api/keys: each key's label, scope, where it is from and when last used. */
static void keys_json(StrBuf *b, const Monitor *mon) {
    uint64_t now = plat_now_ns();
    sb_puts(b, "{\"file\":");
    if (mon->keyfile) json_str(b, mon->keyfile);
    else sb_puts(b, "null");
    sb_puts(b, ",\"keys\":[");
    for (size_t i = 0; i < mon->keys.n; i++) {
        const MonKey *k = &mon->keys.v[i];
        sb_puts(b, i ? ",{\"label\":" : "{\"label\":");
        json_str(b, k->label);
        sb_printf(b, ",\"scope\":\"%s\",\"from\":\"%s\",\"used_ago_ms\":", key_scope_name(k->scope),
                  k->from == 's' ? "start" : k->from == 'f' ? "file" : "api");
        if (k->used_ns) sb_printf(b, "%llu}", (unsigned long long)((now - k->used_ns) / 1000000));
        else sb_puts(b, "null}");
    }
    sb_puts(b, "]}");
}

/* mlwsvm serve's programs (the first, its own, left out): each one's number,
 * file (from the programs' directory), arguments, main process, and how it is. */
static void programs_json(StrBuf *b, const Monitor *mon, VM *vm) {
    sb_putc(b, '[');
    plat_lock(vm->lock);
    for (size_t i = 1; mon->programs && i < vm->progs.n; i++) {
        const Program *pr = vm->progs.v[i];
        const Process *m = pr->main;
        sb_printf(b, "%s{\"id\":%zu,\"path\":", i > 1 ? "," : "", i);
        json_str(b, prog_name(mon, pr));
        sb_puts(b, ",\"cap\":");
        if (pr->cap) json_str(b, pr->cap);
        else sb_puts(b, "null");
        sb_puts(b, ",\"quota\":");
        if (pr->quota) json_str(b, pr->quota);
        else sb_puts(b, "null");
        sb_puts(b, ",\"args\":[");
        for (size_t k = 0; k < pr->args.n; k++) {
            if (k) sb_putc(b, ',');
            json_str(b, pr->args.v[k]);
        }
        int state = m ? (int)m->state : pr->ended; /* (no main: it has ended, and been freed) */
        bool done = state == PROC_DONE, failed = state == PROC_FAILED;
        const char *code = m ? m->err.code : pr->err_code, *msg = m ? m->err.msg : pr->err_msg;
        sb_printf(b, "],\"pid\":%llu,\"state\":\"%s\"", (unsigned long long)pr->main_pid,
                  failed ? "failed" : done ? "done" : "running");
        if (done) sb_printf(b, ",\"exit\":%lld", (long long)(m ? m->exit_code : pr->exit_code));
        if (failed) {
            sb_puts(b, ",\"code\":");
            json_str(b, code);
            sb_puts(b, ",\"message\":");
            json_str(b, msg);
        }
        sb_putc(b, '}');
    }
    plat_unlock(vm->lock);
    sb_putc(b, ']');
}

/* The programs in mlwsvm serve's directory, for the page's picker: each .masm
 * or .mlwso file, by its path within it, at most four directories down and
 * a thousand files in all. */
static void list_programs(StrBuf *b, const char *dir, const char *rel, int depth, int *count) {
    char **names = NULL;
    size_t n = 0;
    if (depth > 4 || !plat_list_dir(dir, &names, &n)) return;
    for (size_t i = 0; i < n; i++) {
        const char *nm = names[i];
        char *full = path_join(dir, nm);
        StrBuf r = {0};
        sb_printf(&r, "%s%s%s", rel, *rel ? "/" : "", nm);
        if (nm[0] != '.' && *count < 1000) {
            if (plat_is_dir(full)) {
                list_programs(b, full, r.s, depth + 1, count);
            } else if (str_ends_with(nm, ".masm") || str_ends_with(nm, ".mlwso")) {
                if ((*count)++) sb_putc(b, ',');
                json_str(b, r.s);
            }
        }
        sb_free(&r);
        free(full);
        free(names[i]);
    }
    free(names);
}

/* The read-only API and the page, with the world stopped: the page to
 * anyone; the rest to a key that may read (anyone, with no tokens file), and
 * the keys to an admin. */
static void serve_get(Monitor *mon, VM *vm, Conn *k, Req *q, const MonKey *key) {
    char *path = q->path;
    char *qs = strchr(path, '?');
    if (qs) *qs++ = 0;
    const char *since = qs && !strncmp(qs, "since=", 6) ? qs + 6 : NULL;
    StrBuf b = {0};
    const char *json = "application/json";
    int scope = key ? key->scope : mon->need_key ? SCOPE_NONE : SCOPE_READ;
    if (!strcmp(path, "/") || !strcmp(path, "/index.html")) {
        respond(k, 200, "text/html; charset=utf-8", DASHBOARD_HTML, DASHBOARD_HTML_LEN, true);
        return;
    }
    int need = !strcmp(path, "/api/keys") ? SCOPE_ADMIN : SCOPE_READ;
    if (scope < need) {
        if (key) respond_json(k, 403, "{\"error\":\"this key may only read: an admin's is needed\"}");
        else respond_json(k, 401, "{\"error\":\"a key is needed: Authorization: Bearer KEY\"}");
        return;
    }
    if (!strcmp(path, "/api/keys")) {
        keys_json(&b, mon);
    } else if (!strcmp(path, "/api/envs")) {
        envs_json(&b, mon, vm, scope >= SCOPE_ADMIN);
    } else if (!strcmp(path, "/api/vm")) {
        vm_json(&b, vm);
        b.n--; /* (its closing brace: one more field, whether actions are on) */
        sb_printf(&b, ",\"actions\":%s,\"serve\":%s,\"keys\":%s,\"you\":", mon->actions ? "true" : "false",
                  mon->programs ? "true" : "false", mon->need_key ? "true" : "false");
        if (key) {
            sb_puts(&b, "{\"label\":");
            json_str(&b, key->label);
            sb_printf(&b, ",\"scope\":\"%s\"}}", key_scope_name(key->scope));
        } else {
            sb_puts(&b, "null}");
        }
    } else if (!strcmp(path, "/api/programs")) {
        programs_json(&b, mon, vm);
    } else if (!strcmp(path, "/api/programs/files") && mon->programs) {
        int count = 0;
        sb_putc(&b, '[');
        list_programs(&b, mon->programs, "", 0, &count);
        sb_putc(&b, ']');
    } else if (!strcmp(path, "/api/procs")) {
        sb_putc(&b, '[');
        bool first = true;
        for (uint64_t pid = 1, end = vm_pid_end(vm); pid < end; pid++) {
            Process *p = vm_proc_at(vm, pid);
            if (!p) continue;
            if (!first) sb_putc(&b, ',');
            first = false;
            proc_json(&b, mon, vm, p, false);
        }
        sb_putc(&b, ']');
    } else if (!strncmp(path, "/api/proc/", 10)) {
        uint64_t pid = strtoull(path + 10, NULL, 10);
        Process *p = vm_proc_at(vm, pid);
        if (!p) {
            respond(k, 404, json, "{\"error\":\"no such process\"}", 27, true);
            return;
        }
        proc_json(&b, mon, vm, p, true);
    } else if (!strcmp(path, "/api/exits")) {
        exits_json(&b, mon, vm);
    } else if (!strcmp(path, "/api/modules")) {
        modules_json(&b, vm);
    } else if (!strcmp(path, "/api/names")) {
        names_json(&b, vm);
    } else if (!strcmp(path, "/api/nodes")) {
        nodes_json(&b, vm);
    } else if (!strcmp(path, "/api/graph")) {
        graph_json(&b, vm);
    } else if (!strcmp(path, "/api/log")) {
        log_json(&b, mon, vm, since ? strtoull(since, NULL, 10) : 0);
    } else if (!strcmp(path, "/top") || !strcmp(path, "/log")) {
        if (path[1] == 't') top_text(&b, mon, vm);
        else log_text(&b, mon, vm, since ? strtoull(since, NULL, 10) : 0);
        respond(k, 200, "text/plain; charset=utf-8", b.s, b.n, true);
        sb_free(&b);
        return;
    } else {
        respond(k, 404, "text/plain", "not found\n", 10, true);
        return;
    }
    respond(k, 200, json, b.s, b.n, need < SCOPE_ADMIN);
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

/* An action, into the log: what, and its JSON with the label of the key that
 * did it ("by") first. */
static void audit(VM *vm, const MonKey *by, const char *what, const char *json) {
    if (!vm->mlog) return;
    StrBuf b = {0};
    if (by && json[0] == '{') {
        sb_puts(&b, "{\"by\":");
        json_str(&b, by->label);
        sb_puts(&b, json[1] == '}' ? "}" : ",");
        if (json[1] != '}') sb_puts(&b, json + 1);
        json = b.s;
    }
    vm_mlog_put(vm, NULL, -1, what, json, strlen(json));
    sb_free(&b);
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

static void serve_programs(Monitor *mon, VM *vm, Conn *k, Req *q, const MonKey *by) {
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
            sb_printf(&b, "{\"ok\":true,\"id\":%zu,\"pid\":%llu}", vm->progs.n - 1, (unsigned long long)pr->main_pid);
            sb_puts(&a, "{\"path\":");
            json_str(&a, r.path);
            sb_printf(&a, ",\"pid\":%llu}", (unsigned long long)pr->main_pid);
            audit(vm, by, "monitor.start", a.s);
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
    } else if (sscanf(q->path, "/api/programs/%zu/%15[a-z]", &id, verb) == 2 && !strcmp(verb, "rerun")) {
        /* the same file, arguments, capabilities and quotas, as a new program */
        char *file = NULL, *cap = NULL, *quota = NULL;
        StrVec args = {0};
        plat_lock(vm->lock);
        if (id >= 1 && id < vm->progs.n) {
            const Program *old = vm->progs.v[id];
            file = xstrdup(old->path);
            cap = old->cap ? xstrdup(old->cap) : NULL;
            quota = old->quota ? xstrdup(old->quota) : NULL;
            for (size_t i = 0; i < old->args.n; i++) VEC_PUSH(args, xstrdup(old->args.v[i]));
        }
        plat_unlock(vm->lock);
        if (!file) {
            respond_json(k, 404, "{\"error\":\"no such program\"}");
        } else {
            MlwsError e = {{0}, {0}};
            sched_stop_world(vm);
            Program *pr = vm_start_program(vm, file, &args, cap, quota, &e);
            sched_start_world(vm);
            if (pr) {
                sb_printf(&b, "{\"ok\":true,\"id\":%u,\"pid\":%llu}", pr->id, (unsigned long long)pr->main_pid);
                sb_printf(&a, "{\"id\":%zu,\"as\":%u,\"pid\":%llu}", id, pr->id, (unsigned long long)pr->main_pid);
                audit(vm, by, "monitor.rerun", a.s);
                respond_json(k, 200, b.s);
            } else {
                sb_puts(&b, "{\"error\":");
                json_str(&b, e.msg);
                sb_putc(&b, '}');
                respond_json(k, 400, b.s);
            }
        }
        free(file);
        free(cap);
        free(quota);
        for (size_t i = 0; i < args.n; i++) free(args.v[i]);
        VEC_FREE(args);
    } else if (sscanf(q->path, "/api/programs/%zu/%15[a-z]", &id, verb) == 2 && !strcmp(verb, "stop")) {
        uint64_t pid = 0;
        plat_lock(vm->lock);
        if (id >= 1 && id < vm->progs.n) pid = vm->progs.v[id]->main_pid;
        plat_unlock(vm->lock);
        if (pid && sched_kill_from_outside(vm, pid, "E_KILLED", "stopped from the monitor")) {
            sb_printf(&a, "{\"id\":%zu,\"pid\":%llu}", id, (unsigned long long)pid);
            audit(vm, by, "monitor.stop", a.s);
            respond_json(k, 200, "{\"ok\":true}");
        } else {
            respond_json(k, 404, "{\"error\":\"no such program running\"}");
        }
    } else if (!strcmp(q->path, "/api/shutdown")) {
        audit(vm, by, "monitor.shutdown", "{}");
        respond_json(k, 200, "{\"ok\":true}");
        vm_serve_stop(vm);
    } else {
        respond_json(k, 404, "{\"error\":\"no such action\"}");
    }
    sb_free(&a);
    sb_free(&b);
}

/* A JSON object of strings only, each named in names, into vals (malloc'd;
 * NULL for one not there). */
static bool flat_strings(const char *body, size_t n, const char *const *names, char **vals, size_t nnames) {
    JText t = {body, body + n};
    jt_ws(&t);
    if (t.s >= t.end || *t.s++ != '{') return false;
    jt_ws(&t);
    if (t.s < t.end && *t.s == '}') return t.s++, jt_ws(&t), t.s == t.end;
    for (;;) {
        char *key = NULL, *v = NULL;
        if (!jt_str(&t, &key)) return false;
        jt_ws(&t);
        bool ok = t.s < t.end && *t.s++ == ':' && jt_str(&t, &v);
        size_t at = nnames;
        for (size_t i = 0; ok && i < nnames; i++)
            if (!strcmp(key, names[i])) at = i;
        ok = ok && at < nnames && !vals[at];
        free(key);
        if (!ok) return free(v), false;
        vals[at] = v;
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
    return t.s == t.end;
}

/* POST /api/keys {"label", "scope"}: a new key, its token in the answer only
 * (and into the tokens file, as its hash, if there is one); POST
 * /api/keys/LABEL/revoke: a key no more, but not the one asking. */
static void serve_keys(Monitor *mon, VM *vm, Conn *k, Req *q, const MonKey *by) {
    StrBuf b = {0}, a = {0};
    char err[600], label[256] = "", verb[16] = "";
    if (!strcmp(q->path, "/api/keys")) {
        static const char *const NAMES[] = {"label", "scope"};
        char *v[2] = {NULL, NULL};
        int scope = SCOPE_NONE;
        if (!flat_strings(q->body ? q->body : "", q->nbody, NAMES, v, 2) || !v[0] || !key_label_ok(v[0]) ||
            !(scope = v[1] ? key_scope_of(v[1]) : SCOPE_READ)) {
            respond_json(k, 400, "{\"error\":\"expected a label (1 to 63 letters, digits, _, ., - or @) and a scope (read or admin)\"}");
        } else if (key_labelled(mon, v[0])) {
            respond_json(k, 409, "{\"error\":\"there is a key with that label\"}");
        } else {
            MonKey nk = {{0}, {0}, scope, mon->keyfile ? 'f' : 'a', 0};
            snprintf(nk.label, sizeof nk.label, "%.63s", v[0]);
            char token[33];
            key_random(token);
            key_hash(token, strlen(token), nk.hash);
            bool ok = true;
            if (mon->keyfile) {
                ok = keyfile_add(mon->keyfile, scope, nk.label, nk.hash, err, sizeof err);
                if (ok) keys_refresh(mon, vm);
            } else {
                VEC_PUSH(mon->keys, nk);
            }
            if (ok) {
                sb_puts(&b, "{\"ok\":true,\"label\":");
                json_str(&b, nk.label);
                sb_printf(&b, ",\"scope\":\"%s\",\"token\":\"%s\"}", key_scope_name(scope), token);
                sb_puts(&a, "{\"label\":");
                json_str(&a, nk.label);
                sb_printf(&a, ",\"scope\":\"%s\"}", key_scope_name(scope));
                audit(vm, by, "monitor.key_new", a.s);
                respond_json(k, 200, b.s);
            } else {
                sb_puts(&b, "{\"error\":");
                json_str(&b, err);
                sb_putc(&b, '}');
                respond_json(k, 500, b.s);
            }
        }
        free(v[0]);
        free(v[1]);
    } else if (sscanf(q->path, "/api/keys/%255[^/]/%15[a-z]", label, verb) == 2 && !strcmp(verb, "revoke")) {
        url_decode(label);
        MonKey *old = key_labelled(mon, label);
        if (!old) {
            respond_json(k, 404, "{\"error\":\"no key with that label\"}");
        } else if (!strcmp(label, by->label)) {
            respond_json(k, 409, "{\"error\":\"a key cannot revoke itself\"}");
        } else {
            bool ok = true;
            if (old->from == 'f') {
                ok = keyfile_remove(mon->keyfile, label, err, sizeof err);
                if (ok) keys_refresh(mon, vm);
            } else {
                *old = mon->keys.v[--mon->keys.n];
            }
            if (ok) {
                sb_puts(&a, "{\"label\":");
                json_str(&a, label);
                sb_putc(&a, '}');
                audit(vm, by, "monitor.key_revoke", a.s);
                respond_json(k, 200, "{\"ok\":true}");
            } else {
                sb_puts(&b, "{\"error\":");
                json_str(&b, err);
                sb_putc(&b, '}');
                respond_json(k, 500, b.s);
            }
        }
    } else {
        respond_json(k, 404, "{\"error\":\"no such action\"}");
    }
    sb_free(&a);
    sb_free(&b);
}

/* A POST: an admin's key, the monitor's own Host when it listens on this
 * machine only, and no Origin but its own; and, but for the keys, actions on. */
static void serve_action(Monitor *mon, VM *vm, Conn *k, Req *q, const MonKey *by) {
    bool keys = !strncmp(q->path, "/api/keys", 9) && (!q->path[9] || q->path[9] == '/');
    if (!keys && !mon->actions) {
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
    if (!by) {
        respond_json(k, 401, "{\"error\":\"an admin's key is needed: Authorization: Bearer KEY\"}");
        return;
    }
    if (by->scope < SCOPE_ADMIN) {
        respond_json(k, 403, "{\"error\":\"this key may only read: an admin's is needed\"}");
        return;
    }
    if (keys) {
        serve_keys(mon, vm, k, q, by);
        return;
    }
    if (mon->programs && (!strncmp(q->path, "/api/programs", 13) || !strcmp(q->path, "/api/shutdown"))) {
        serve_programs(mon, vm, k, q, by);
        return;
    }
    StrBuf b = {0}, a = {0};
    char *path = q->path, err[300] = "";
    uint64_t pid = 0;
    char verb[16] = "", name[256] = "";
    if (!strcmp(path, "/api/envs/set") || !strcmp(path, "/api/envs/delete")) {
        bool del = !strcmp(path, "/api/envs/delete");
        int st = env_monitor_change(vm, del, q->body ? q->body : "", q->nbody, &a, err, sizeof err);
        if (st == 200) {
            audit(vm, by, del ? "monitor.env_delete" : "monitor.env_set", a.s);
            respond_json(k, 200, "{\"ok\":true}");
        } else {
            sb_puts(&b, "{\"error\":");
            json_str(&b, err);
            sb_putc(&b, '}');
            respond_json(k, st, b.s);
        }
    } else if (sscanf(path, "/api/proc/%llu/%15[a-z]", (unsigned long long *)&pid, verb) == 2 && !strcmp(verb, "kill")) {
        if (sched_kill_from_outside(vm, pid, "E_KILLED", "killed from the monitor")) {
            sb_printf(&a, "{\"pid\":%llu}", (unsigned long long)pid);
            audit(vm, by, "monitor.kill", a.s);
            respond_json(k, 200, "{\"ok\":true}");
        } else {
            respond_json(k, 404, "{\"error\":\"no such process\"}");
        }
    } else if (sscanf(path, "/api/proc/%llu/%15[a-z]", (unsigned long long *)&pid, verb) == 2 && !strcmp(verb, "send")) {
        if (sched_post_json(vm, pid, q->body ? q->body : "", q->nbody, err, sizeof err)) {
            sb_printf(&a, "{\"pid\":%llu,\"message\":%s}", (unsigned long long)pid, q->body);
            audit(vm, by, "monitor.send", a.s);
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
            audit(vm, by, !strcmp(verb, "reload") ? "monitor.reload" : "monitor.purge", a.s);
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

/* A whole request, answered into its connection's buffer. */
static void serve(Monitor *mon, VM *vm, Conn *k, Req *q, uint64_t now) {
    size_t waiting = 0; /* answers not yet taken */
    for (int i = 0; i < mon->nconns; i++) waiting += mon->conns[i]->out.n - mon->conns[i]->sent;
    if (waiting > OUT_MAX) {
        respond_json(k, 503, "{\"error\":\"too many answers are waiting to be taken\"}");
        return;
    }
    keys_refresh(mon, vm);
    bool page = !strcmp(q->method, "GET") && (!strcmp(q->path, "/") || !strcmp(q->path, "/index.html"));
    MonKey *found = NULL, key; /* (a copy: the keys may change as it is served) */
    if (q->auth[0] && !page) {
        if (!wrong_room(mon, k->ip, now)) {
            respond_json(k, 429, "{\"error\":\"too many wrong keys from this address: wait a few seconds\"}");
            return;
        }
        uint8_t h[32];
        if (!(found = key_of(mon, q->auth, h))) {
            wrong_count(mon, vm, k->ip, h, now);
            respond_json(k, 401, "{\"error\":\"no such key\"}");
            return;
        }
        key = *found;
    }
    if (!strcmp(q->method, "GET")) {
        sched_stop_world(vm); /* other worker threads pause while we read their processes */
        serve_get(mon, vm, k, q, found ? &key : NULL);
        sched_start_world(vm);
    } else if (!strcmp(q->method, "POST")) {
        serve_action(mon, vm, k, q, found ? &key : NULL);
    } else {
        respond(k, 405, "text/plain", "GET, or POST for an action\n", 27, false);
    }
}

/* A step of a connection, as far as it goes without waiting: false once it is
 * over (answered, failed, or out of time). */
static bool conn_step(Monitor *mon, VM *vm, Conn *c, uint64_t now) {
    if (now >= c->deadline) return false;
    if (c->state == CONN_HANDSHAKE) {
        char err[256];
        int r = tls_handshake(c->tls, err, sizeof err);
        if (r == TLS_FAILED) return false;
        if (r != TLS_OK) return true;
        c->state = CONN_READ;
    }
    if (c->state == CONN_READ) {
        for (;;) {
            Req q = {0};
            int status = 0, got = request_of(c, &q, &status);
            if (got > 0) {
                serve(mon, vm, c, &q, now);
                break;
            }
            if (got < 0) {
                if (!status) return false;
                respond_json(c, status, status == 413   ? "{\"error\":\"the body is over 64 KiB\"}"
                                        : status == 431 ? "{\"error\":\"the request's headers are over 8 KiB\"}"
                                                        : "{\"error\":\"not a request\"}");
                c->unread = true;
                break;
            }
            char buf[4096];
            long r = conn_read(c, buf, sizeof buf);
            if (r == IO_WAIT) return true;
            if (r <= 0) return false; /* gone before a whole request came */
            sb_putn(&c->in, buf, (size_t)r);
        }
        c->state = CONN_WRITE;
        c->deadline = now + WRITE_DEADLINE_NS;
    }
    while (c->sent < c->out.n) {
        long r = conn_write(c, c->out.s + c->sent, c->out.n - c->sent);
        if (r == IO_WAIT) return true;
        if (r < 0) return false;
        c->sent += (size_t)r;
    }
    if (!c->unread) return false;
    /* The rest of what the client sent is still coming, or waiting to be read.
     * A socket closed with that unread is reset, and the reset can reach the
     * client before it has read the answer (on Windows it does). So the
     * answer's end is marked, and what comes is read and dropped until the
     * client closes, for a second at most, and so much at a step. */
    if (c->state != CONN_DRAIN) {
        c->state = CONN_DRAIN;
        if (c->deadline > now + DRAIN_NS) c->deadline = now + DRAIN_NS;
        if (!c->tls) plat_sock_shutdown_write(c->fd);
    }
    for (int i = 0; i < 16; i++) {
        char buf[4096];
        long r = conn_read(c, buf, sizeof buf);
        if (r == IO_WAIT) return true;
        if (r <= 0) return false;
    }
    return true;
}

static void monitor_poll(VM *vm, uint64_t wait_ns) {
    Monitor *mon = ((Tools *)vm->tools)->mon;
    if (wait_ns) { /* idle: sleep until a request or the timer (or, with connections under way, a millisecond) */
        if (mon->nconns && wait_ns > 1000000) wait_ns = 1000000;
        plat_wait_readable(mon->fd, wait_ns);
    }
    int fd;
    uint64_t now = plat_now_ns();
    while (mon->nconns < CONNS_MAX && (fd = plat_tcp_accept(mon->fd)) >= 0) {
        Conn *c = xcalloc(1, sizeof *c);
        c->fd = fd;
        c->deadline = now + READ_DEADLINE_NS;
        c->state = CONN_READ;
        int port;
        if (!plat_sock_peer(fd, c->ip, sizeof c->ip, &port)) snprintf(c->ip, sizeof c->ip, "?");
        char err[256];
        bool ok = plat_fd_set_nonblocking(fd, true);
        if (ok && mon->cert) { /* TLS first */
            c->tls = tls_server(fd, mon->cert, mon->key, err, sizeof err);
            c->state = CONN_HANDSHAKE;
            if (!c->tls) {
                plat_sock_close(fd);
                free(c);
                continue;
            }
        }
        if (!ok) {
            conn_close(c);
            continue;
        }
        mon->conns[mon->nconns++] = c;
    }
    for (int i = mon->nconns - 1; i >= 0; i--) {
        Conn *c = mon->conns[i];
        if (conn_step(mon, vm, c, now)) continue;
        mon->conns[i] = mon->conns[--mon->nconns]; /* (before it is closed: serve counts those there) */
        conn_close(c);
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

void tools_monitor_actions(VM *vm) { ((Tools *)vm->tools)->mon->actions = true; }

bool tools_monitor_tokens(VM *vm, const char *path, char *err, size_t errlen) {
    Monitor *mon = ((Tools *)vm->tools)->mon;
    MonKeyVec ks = {0};
    bool ok = keyfile_load(path, &ks, err, errlen);
    VEC_FREE(ks);
    if (!ok) return false;
    mon->keyfile = xstrdup(path);
    mon->need_key = true;
    keys_refresh(mon, vm);
    return true;
}

const char *tools_monitor_key(VM *vm, const char *token) {
    Monitor *mon = ((Tools *)vm->tools)->mon;
    if (!mon->actions && !mon->keyfile) return NULL;
    if (token && *token) {
        mon->token = xstrdup(token);
    } else {
        mon->token = xmalloc(33);
        key_random(mon->token);
    }
    MonKey k = {"(start)", {0}, SCOPE_ADMIN, 's', 0}; /* (a label no other key can have) */
    key_hash(mon->token, strlen(mon->token), k.hash);
    VEC_PUSH(mon->keys, k);
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
    for (int i = 0; i < t->mon->nconns; i++) conn_close(t->mon->conns[i]);
    VEC_FREE(t->mon->last_cpu);
    free(t->mon->token);
    VEC_FREE(t->mon->keys);
    free(t->mon->keyfile);
    free(t->mon->cert);
    free(t->mon->key);
    free(t->mon->programs);
    free(t->mon);
    t->mon = NULL;
    vm->poll = NULL;
}
