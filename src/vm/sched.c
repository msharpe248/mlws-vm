/* sched.c - the process scheduler.
 *
 * N worker threads (vm->threads; default one per core) each own a run queue of
 * ready pids and run one process at a time for a time slice of
 * vm->slice_insns instructions. A worker that runs out of work steals half of
 * another worker's queue, and sleeps when there is nothing to steal.
 *
 * Processes share nothing, so a running process's own state needs no locking.
 * What other threads do touch - the inbox of delivered messages, links,
 * monitors and the scheduling fields - is guarded by the process's lock (the
 * fields marked [L] in vm.h). A message is copied by the sender into a detached
 * list of objects; the receiver links that list into its own heap on its own
 * thread (sched_take_inbox), so no heap is ever touched by two threads.
 *
 * Processes are found by pid in a table of fixed-size chunks (never moved, so
 * lookups need no global lock). A lookup "pins" the process by taking its lock
 * while holding the table stripe lock; a process is only freed after it has
 * been removed from the table and its lock has been taken once more, so a
 * pinned process cannot disappear.
 *
 * Lock order (never take an earlier lock while holding a later one):
 *   table stripe -> one process -> run queue | sched lock | vm->lock | vm->out_lock
 *
 * With one worker thread, scheduling is deterministic round robin. See
 * docs/ABI.md ("Scheduling"). */
#include "../heap/heap.h"
#include "../platform/platform.h"
#include "proc_internal.h"

#define PT_CHUNK 4096u       /* table slots per chunk */
#define PT_CHUNKS (1u << 18) /* chunks: up to 2^30 pids per VM */
#define PT_STRIPES 64u       /* table locks, by pid */
#define MAX_THREADS 256
#define STEAL_MAX 32

typedef struct {
    uint64_t at, pid; /* wake process `pid` at `at` (ns), if still applicable */
} Timer;

typedef struct Worker {
    VM *vm;
    int id;
    PlatLock *qlock;
    uint64_t *q; /* ring buffer of ready pids */
    size_t qcap, qhead, qlen;
    Process *frag;    /* allocation target for outgoing messages (never runs) */
    Process *current; /* the process this worker is running, or NULL */
    PlatThread *thread;
    uint64_t last_poll;
} Worker;

typedef struct Sched {
    _Atomic(Process **) chunks[PT_CHUNKS];
    PlatLock *stripes[PT_STRIPES];
    PlatMutex *lock; /* the fields below, timers, idle and stop-the-world state */
    PlatCond *work_cv;   /* idle workers wait here */
    PlatCond *stw_cv;    /* a stop-the-world requester waits here */
    PlatCond *resume_cv; /* workers paused by stop-the-world wait here */
    Worker **workers;
    int nworkers; /* allocated */
    int nrun;     /* taking part in the current sched_run */
    bool running;
    VEC(Timer) timers; /* min-heap by wake time */
    _Atomic uint64_t next_timer; /* earliest timer, 0 = none (a hint read without the lock) */
    _Atomic int nidle;           /* workers that found no work */
    int nparked, nexited;
    _Atomic bool done; /* main has finished */
    _Atomic bool stw;  /* stop the world requested */
    Worker *stw_owner;
    /* the I/O thread pool for blocking native calls (io_lock guards these) */
    PlatMutex *io_lock;
    PlatCond *io_cv;      /* idle I/O threads wait here */
    PlatCond *io_done_cv; /* shutdown waits here for calls in progress */
    VEC(Process *) io_queue;
    size_t io_head;
    VEC(PlatThread *) io_threads;
    int io_idle, io_busy;
    bool io_stop, io_abandoned;
    _Atomic int io_inflight; /* queued or running: other work that can still wake processes */
    /* the poller thread: processes waiting for descriptors (PROC_IOWAIT) */
    PlatPoller *poller;
    PlatThread *poll_thread;
    _Atomic bool poll_stop;
    _Atomic int io_waiting;
} Sched;

#define IO_MAX_THREADS 64
#define IO_SHUTDOWN_WAIT_NS 1000000000ull /* how long the end of a program waits for blocking calls */

static MLWS_THREAD_LOCAL Worker *tls_worker;

static bool is_live(const Process *p) { return p->state != PROC_DONE && p->state != PROC_FAILED; }

/* ---- setup ---------------------------------------------------------------- */

static Worker *worker_new(VM *vm, int id) {
    Worker *w = xcalloc(1, sizeof *w);
    w->vm = vm;
    w->id = id;
    w->qlock = plat_lock_new();
    Process *f = xcalloc(1, sizeof *f);
    f->vm = vm;
    f->state = PROC_DONE; /* never running, so allocation never traps */
    for (int i = 0; i < QUOTA__COUNT; i++) f->quota[i] = UINT64_MAX;
    w->frag = f;
    return w;
}

static Sched *sched_of(VM *vm) {
    if (vm->sched) return vm->sched;
    Sched *s = xcalloc(1, sizeof *s);
    for (unsigned i = 0; i < PT_STRIPES; i++) s->stripes[i] = plat_lock_new();
    s->lock = plat_mutex_new();
    s->work_cv = plat_cond_new();
    s->stw_cv = plat_cond_new();
    s->resume_cv = plat_cond_new();
    s->io_lock = plat_mutex_new();
    s->io_cv = plat_cond_new();
    s->io_done_cv = plat_cond_new();
    int n = vm->threads > 0 ? vm->threads : plat_cpu_count();
    if (n < 1) n = 1;
    if (n > MAX_THREADS) n = MAX_THREADS;
    s->nworkers = n;
    s->workers = xcalloc((size_t)n, sizeof *s->workers);
    for (int i = 0; i < n; i++) s->workers[i] = worker_new(vm, i);
    vm->sched = s;
    return s;
}

/* The worker of the calling thread; a thread that is not a worker (the main
 * thread before or between runs) uses worker 0's queue and scratch. */
static Worker *my_worker(Sched *s) {
    Worker *w = tls_worker;
    return w && w->vm->sched == s ? w : s->workers[0];
}

/* ---- the process table ------------------------------------------------------ */

static Process **slot(Sched *s, uint64_t pid, bool create) {
    if (!pid) return NULL;
    uint64_t i = pid - 1;
    if (i / PT_CHUNK >= PT_CHUNKS) return NULL;
    Process **c = atomic_load_explicit(&s->chunks[i / PT_CHUNK], memory_order_acquire);
    if (!c && create) {
        Process **fresh = xcalloc(PT_CHUNK, sizeof *fresh);
        Process **expect = NULL;
        if (atomic_compare_exchange_strong(&s->chunks[i / PT_CHUNK], &expect, fresh)) c = fresh;
        else free(fresh), c = expect;
    }
    return c ? &c[i % PT_CHUNK] : NULL;
}

/* The process with this pid, locked; NULL if it is not in the table. It may
 * have finished (main stays in the table until its runner frees it). */
static Process *pin(Sched *s, uint64_t pid) {
    Process **sl = slot(s, pid, false);
    if (!sl) return NULL;
    PlatLock *st = s->stripes[pid % PT_STRIPES];
    plat_lock(st);
    Process *p = *sl;
    if (p) plat_lock(p->lock);
    plat_unlock(st);
    return p;
}

void sched_unpublish(Process *p) {
    Sched *s = p->vm->sched;
    PlatLock *st = s->stripes[p->pid % PT_STRIPES];
    plat_lock(st);
    Process **sl = slot(s, p->pid, false);
    if (sl && *sl == p) *sl = NULL;
    plat_unlock(st);
    plat_lock(p->lock); /* wait until nobody has it pinned */
    p->published = false;
    plat_unlock(p->lock);
}

Process *vm_proc_at(VM *vm, uint64_t pid) {
    if (!vm->sched) return NULL;
    Process **sl = slot(vm->sched, pid, false);
    return sl ? *sl : NULL;
}

uint64_t vm_pid_end(VM *vm) {
    plat_lock(vm->lock);
    uint64_t n = vm->next_pid;
    plat_unlock(vm->lock);
    return n;
}

Process *vm_process(VM *vm, uint64_t pid) {
    Process *p = vm_proc_at(vm, pid);
    return p && is_live(p) ? p : NULL;
}

/* ---- run queues ------------------------------------------------------------- */

static size_t q_push(Worker *w, uint64_t pid) {
    plat_lock(w->qlock);
    if (w->qlen == w->qcap) { /* grow, unwrapping the ring */
        size_t nc = w->qcap ? w->qcap * 2 : 64;
        uint64_t *v = xmalloc(nc * sizeof *v);
        for (size_t i = 0; i < w->qlen; i++) v[i] = w->q[(w->qhead + i) % w->qcap];
        free(w->q);
        w->q = v;
        w->qcap = nc;
        w->qhead = 0;
    }
    w->q[(w->qhead + w->qlen) % w->qcap] = pid;
    size_t n = ++w->qlen;
    plat_unlock(w->qlock);
    return n;
}

/* Take up to `max` pids from the head of w's queue. */
static size_t q_take(Worker *w, uint64_t *out, size_t max) {
    plat_lock(w->qlock);
    size_t k = w->qlen < max ? w->qlen : max;
    for (size_t i = 0; i < k; i++) {
        out[i] = w->q[w->qhead];
        w->qhead = (w->qhead + 1) % w->qcap;
    }
    w->qlen -= k;
    plat_unlock(w->qlock);
    return k;
}

static size_t q_len(Worker *w) {
    plat_lock(w->qlock);
    size_t n = w->qlen;
    plat_unlock(w->qlock);
    return n;
}

/* Steal half of the first non-empty queue after w's; returns one pid to run
 * (the rest go on w's queue) or 0. */
static uint64_t steal(Worker *w) {
    Sched *s = w->vm->sched;
    for (int k = 1; k < s->nworkers; k++) {
        Worker *v = s->workers[(w->id + k) % s->nworkers];
        size_t n = q_len(v);
        if (!n) continue;
        uint64_t got[STEAL_MAX];
        size_t want = (n + 1) / 2;
        size_t m = q_take(v, got, want < STEAL_MAX ? want : STEAL_MAX);
        if (!m) continue;
        for (size_t i = 1; i < m; i++) q_push(w, got[i]);
        return got[0];
    }
    return 0;
}

static void wake_one(Sched *s) {
    plat_mutex_lock(s->lock);
    plat_cond_signal(s->work_cv);
    plat_mutex_unlock(s->lock);
}

/* Make a locked process ready, on the calling worker's queue. Another worker
 * is woken only when this queue has a backlog: a process woken by a message is
 * usually best run right here, after the sender blocks. */
static void ready_locked(Sched *s, Process *p) {
    p->state = PROC_READY;
    if (p->in_runq) return;
    p->in_runq = true;
    Worker *w = my_worker(s);
    size_t n = q_push(w, p->pid);
    if (atomic_load(&s->nidle) > 0 && (n >= 2 || w != tls_worker)) wake_one(s);
}

void sched_start(VM *vm, Process *p) {
    Sched *s = sched_of(vm);
    Process **sl = slot(s, p->pid, true);
    if (!sl) {
        fprintf(stderr, "mlwsvm: too many processes (the process table is full)\n");
        abort();
    }
    plat_lock(s->stripes[p->pid % PT_STRIPES]);
    *sl = p;
    p->published = true;
    plat_unlock(s->stripes[p->pid % PT_STRIPES]);
    plat_lock(p->lock);
    ready_locked(s, p);
    plat_unlock(p->lock);
}

/* ---- timers (binary min-heap under the sched lock; stale entries are skipped) */

static void timers_changed(Sched *s) { atomic_store(&s->next_timer, s->timers.n ? s->timers.v[0].at : 0); }

static void timer_add(Sched *s, Process *p) {
    plat_mutex_lock(s->lock);
    Timer t = {p->wake_at, p->pid};
    VEC_PUSH(s->timers, t);
    size_t i = s->timers.n - 1;
    while (i > 0) {
        size_t parent = (i - 1) / 2;
        if (s->timers.v[parent].at <= s->timers.v[i].at) break;
        Timer tmp = s->timers.v[parent];
        s->timers.v[parent] = s->timers.v[i];
        s->timers.v[i] = tmp;
        i = parent;
    }
    timers_changed(s);
    if (i == 0 && s->nidle > 0) plat_cond_signal(s->work_cv); /* a new earliest: an idle worker re-arms */
    plat_mutex_unlock(s->lock);
}

static void timer_pop(Sched *s) {
    s->timers.v[0] = s->timers.v[--s->timers.n];
    size_t i = 0;
    for (;;) {
        size_t l = 2 * i + 1, r = l + 1, m = i;
        if (l < s->timers.n && s->timers.v[l].at < s->timers.v[m].at) m = l;
        if (r < s->timers.n && s->timers.v[r].at < s->timers.v[m].at) m = r;
        if (m == i) break;
        Timer tmp = s->timers.v[m];
        s->timers.v[m] = s->timers.v[i];
        s->timers.v[i] = tmp;
        i = m;
    }
}

/* Wake every process whose timer has expired and drop stale entries at the
 * top. Returns the earliest pending wake time, or 0 if there are no timers. */
static uint64_t timers_service(Sched *s, uint64_t now) {
    for (;;) {
        plat_mutex_lock(s->lock);
        if (!s->timers.n) {
            plat_mutex_unlock(s->lock);
            return 0;
        }
        Timer t = s->timers.v[0];
        plat_mutex_unlock(s->lock);
        Process *p = pin(s, t.pid);
        /* wake_at and deadline_set are only read once the process is not running */
        bool live = p &&
                    (p->state == PROC_SLEEPING || (p->state == PROC_WAITING && p->deadline_set) ||
                     (p->state == PROC_IOWAIT && p->fdwait.deadline)) &&
                    p->wake_at == t.at;
        if (live && t.at > now) {
            plat_unlock(p->lock);
            return t.at;
        }
        plat_mutex_lock(s->lock);
        if (s->timers.n && s->timers.v[0].at == t.at && s->timers.v[0].pid == t.pid) timer_pop(s);
        timers_changed(s);
        plat_mutex_unlock(s->lock);
        if (live && p->state == PROC_IOWAIT) { /* an I/O timeout: the native runs again and reports it */
            p->fdwait.timed_out = true;
            s->io_waiting--;
        }
        if (live) ready_locked(s, p); /* recvt re-executes and sees that its deadline passed */
        if (p) plat_unlock(p->lock);
    }
}

/* ---- messages ---------------------------------------------------------------- */

static void frag_reset(Process *f) { /* drop what a trapped copy left behind */
    gc_free_list(f->heap);
    f->heap = NULL;
    f->heap_bytes = f->objects_allocated = 0;
}

static InMsg frag_take(Process *f, Word v, uint8_t tag) {
    InMsg m = {v, tag, f->heap, f->heap_bytes, f->objects_allocated};
    f->heap = NULL;
    f->heap_bytes = f->objects_allocated = 0;
    return m;
}

static bool accepts(const Process *to) { return is_live(to) && !to->kill_req; }

static void push_locked(Sched *s, Process *to, const InMsg *m) {
    VEC_PUSH(to->inbox, *m);
    to->mcount++;
    if (to->state == PROC_WAITING) ready_locked(s, to);
}

/* Put a message in a process's inbox, or free it if the process is gone. */
static void deliver(Sched *s, uint64_t pid, InMsg *m) {
    Process *to = pin(s, pid);
    if (to && accepts(to)) push_locked(s, to, m);
    else gc_free_list(m->objs);
    if (to) plat_unlock(to->lock);
}

void sched_send(Process *p, uint64_t pid, Word v, uint8_t tag) {
    Sched *s = p->vm->sched;
    InMsg m = {v, tag, NULL, 0, 0};
    Process *to = pin(s, pid);
    if (to && accepts(to) && to->mcount < to->quota[QUOTA_MAILBOX] && type_is_ref(tag) && v.r) {
        /* Copy outside the lock (copying can trap), then look again: the
         * receiver may have finished, or filled up, meanwhile. */
        plat_unlock(to->lock);
        Process *f = my_worker(s)->frag;
        frag_reset(f);
        m = frag_take(f, msg_copy(p, f, v, tag), tag);
        to = pin(s, pid);
    }
    if (!to || !accepts(to)) { /* finished or unknown: dropped */
        if (to) plat_unlock(to->lock);
        gc_free_list(m.objs);
        return;
    }
    if (to->mcount >= to->quota[QUOTA_MAILBOX]) {
        plat_unlock(to->lock);
        gc_free_list(m.objs);
        vm_trap(p, "E_QUOTA", "mailbox of process %llu is full", (unsigned long long)pid);
    }
    push_locked(s, to, &m);
    plat_unlock(to->lock);
}

static void take_inbox_locked(Process *p) {
    for (size_t i = 0; i < p->inbox.n; i++) {
        InMsg *in = &p->inbox.v[i];
        gc_adopt(p, in->objs, in->bytes, in->nobjs);
        Message m = {in->v, in->tag};
        VEC_PUSH(p->mbox, m);
    }
    p->inbox.n = 0;
}

void sched_take_inbox(Process *p) {
    plat_lock(p->lock);
    take_inbox_locked(p);
    plat_unlock(p->lock);
}

/* ---- exits, links, monitors, kill ------------------------------------------- */

static void report_unobserved(VM *vm, Process *p) {
    StrBuf b = {0};
    sb_printf(&b, "process %llu (%s) failed: [%s] %s\n%s", (unsigned long long)p->pid,
              p->entry ? p->entry->name : "?", p->err.code, p->err.msg, p->backtrace.s ? p->backtrace.s : "");
    plat_lock(vm->out_lock);
    if (vm->err_out) vm->err_out(vm->err_ctx, b.s, b.n);
    else fputs(b.s, stderr);
    plat_unlock(vm->out_lock);
    sb_free(&b);
}

static void send_down(Sched *s, uint64_t watcher, Process *dead, bool failed) {
    Process *f = my_worker(s)->frag;
    frag_reset(f);
    ObjStruct *d = struct_new(f, &VM_DOWN_DEF);
    d->fields[DOWNF_PID].i = (int64_t)dead->pid;
    d->fields[DOWNF_EXIT].i = dead->exit_code;
    if (failed) {
        d->fields[DOWNF_CODE].r = str_new(f, dead->err.code, strlen(dead->err.code));
        d->fields[DOWNF_MESSAGE].r = str_new(f, dead->err.msg, strlen(dead->err.msg));
    }
    InMsg m = frag_take(f, (Word){.r = d}, T_R);
    deliver(s, watcher, &m);
}

static void unlink_pid(PidVec *v, uint64_t pid) {
    for (size_t i = 0; i < v->n; i++)
        if (v->v[i] == pid) v->v[i--] = v->v[--v->n];
}

/* Kill a locked, live process. A process running on this thread stops at its
 * next instruction, one running on another thread at the end of its time
 * slice; any other is claimed (marked failed) and the caller must finish it:
 * returns true then. */
static bool kill_locked(Process *t, const char *code, const char *msg) {
    if (t->state == PROC_RUNNING || t->state == PROC_NATIVE) { /* in a blocking call: stops when it returns */
        if (tls_worker && tls_worker->current == t) {
            snprintf(t->err.code, sizeof t->err.code, "%s", code);
            snprintf(t->err.msg, sizeof t->err.msg, "%s", msg);
            t->kill_pending = true;
        } else if (!t->kill_req) {
            t->kill_req = true;
            snprintf(t->kill_code, sizeof t->kill_code, "%s", code);
            snprintf(t->kill_msg, sizeof t->kill_msg, "%s", msg);
        }
        return false;
    }
    snprintf(t->err.code, sizeof t->err.code, "%s", code);
    snprintf(t->err.msg, sizeof t->err.msg, "%s", msg);
    t->backtrace.n = 0; /* no backtrace: it was stopped from outside */
    if (t->backtrace.s) t->backtrace.s[0] = 0;
    if (t->state == PROC_IOWAIT) t->vm->sched->io_waiting--;
    t->state = PROC_FAILED;
    return true;
}

static void record_exit(VM *vm, Process *p, bool failed) {
    ExitRecord r;
    memset(&r, 0, sizeof r);
    r.pid = p->pid;
    r.entry = p->entry ? p->entry->name : "?";
    r.failed = failed;
    if (failed) snprintf(r.code, sizeof r.code, "%s", p->err.code);
    r.exit_code = p->exit_code;
    r.insns = p->insns;
    r.cpu_ns = p->cpu_ns;
    r.ended_ns = plat_now_ns();
    if (vm->exits.n < 64) {
        VEC_PUSH(vm->exits, r);
    } else {
        vm->exits.v[vm->exits_next] = r;
        vm->exits_next = (vm->exits_next + 1) % vm->exits.n;
    }
}

/* A process has ended (on this thread, or claimed by kill_locked): notify its
 * monitors, take linked processes down with a failing one, and free it unless
 * it is main (freed by its runner, after collecting statistics). */
static void finish(Sched *s, Process *p, bool failed) {
    VM *vm = p->vm;
    plat_lock(p->lock);
    p->state = failed ? PROC_FAILED : PROC_DONE;
    PidVec links = p->links, mons = p->monitors;
    memset(&p->links, 0, sizeof p->links);
    memset(&p->monitors, 0, sizeof p->monitors);
    plat_unlock(p->lock);
    plat_lock(vm->lock);
    vm->live--;
    record_exit(vm, p, failed);
    plat_unlock(vm->lock);
    for (size_t i = 0; i < mons.n; i++) send_down(s, mons.v[i], p, failed);
    bool observed = mons.n > 0 || links.n > 0;
    /* Linked processes die with a failing one; a normal exit just unlinks. */
    for (size_t i = 0; i < links.n; i++) {
        Process *l = pin(s, links.v[i]);
        if (!l) continue;
        unlink_pid(&l->links, p->pid);
        bool claimed = false;
        if (failed && is_live(l)) {
            char msg[256];
            snprintf(msg, sizeof msg, "linked process %llu failed: %s", (unsigned long long)p->pid, p->err.code);
            claimed = kill_locked(l, "E_LINKED", msg);
        }
        plat_unlock(l->lock);
        if (claimed) finish(s, l, true);
    }
    VEC_FREE(links);
    VEC_FREE(mons);
    if (failed && !observed && p->pid != vm->main_pid) report_unobserved(vm, p);
    if (p->pid == vm->main_pid) { /* the program ends with main */
        plat_mutex_lock(s->lock);
        s->done = true;
        plat_cond_broadcast(s->work_cv);
        plat_cond_broadcast(s->resume_cv);
        plat_cond_broadcast(s->stw_cv);
        plat_mutex_unlock(s->lock);
    } else {
        proc_free(p);
    }
}

void sched_kill(Process *p, uint64_t pid, const char *code, const char *msg) {
    Sched *s = p->vm->sched;
    Process *t = pin(s, pid);
    if (!t) return;
    bool claimed = is_live(t) && kill_locked(t, code, msg);
    plat_unlock(t->lock);
    if (claimed) finish(s, t, true);
}

bool sched_alive(VM *vm, uint64_t pid) {
    Process *t = pin(vm->sched, pid);
    if (!t) return false;
    bool r = is_live(t) && !t->kill_req; /* a pending kill counts as dead already */
    plat_unlock(t->lock);
    return r;
}

static bool has_pid(const PidVec *v, uint64_t pid) {
    for (size_t i = 0; i < v->n; i++)
        if (v->v[i] == pid) return true;
    return false;
}

void sched_link(Process *p, uint64_t pid) {
    Sched *s = p->vm->sched;
    Process *t = pin(s, pid);
    if (!t || !is_live(t) || t->kill_req) {
        if (t) plat_unlock(t->lock);
        vm_trap(p, "E_NOPROC", "link to process %llu, which does not exist or has finished", (unsigned long long)pid);
    }
    if (t == p) {
        plat_unlock(t->lock);
        return;
    }
    /* The target's side first: if it dies before our side is added, it still
     * finds us (and our stale link to it is harmless). */
    if (!has_pid(&t->links, p->pid)) VEC_PUSH(t->links, p->pid);
    plat_unlock(t->lock);
    sched_add_link(p, pid);
}

void sched_add_link(Process *p, uint64_t pid) {
    plat_lock(p->lock);
    if (!has_pid(&p->links, pid)) VEC_PUSH(p->links, pid);
    plat_unlock(p->lock);
}

void sched_monitor(Process *p, uint64_t pid) {
    Sched *s = p->vm->sched;
    Process *t = pin(s, pid);
    if (t && is_live(t) && !t->kill_req) {
        VEC_PUSH(t->monitors, p->pid);
        plat_unlock(t->lock);
        return;
    }
    if (t) plat_unlock(t->lock);
    sched_take_inbox(p); /* the Down goes after messages already delivered */
    ObjStruct *d = struct_new(p, &VM_DOWN_DEF);
    d->fields[DOWNF_PID].i = (int64_t)pid;
    d->fields[DOWNF_CODE].r = str_new(p, "E_NOPROC", 8);
    d->fields[DOWNF_MESSAGE].r = str_new(p, "no such process", 15);
    Message m = {{.r = d}, T_R};
    VEC_PUSH(p->mbox, m);
    p->mcount++;
}

void sched_set_quota(Process *p, int kind, uint64_t limit) {
    plat_lock(p->lock);
    if (limit < p->quota[kind]) p->quota[kind] = limit;
    plat_unlock(p->lock);
}

/* ---- stop the world ------------------------------------------------------------ */

static void park_locked(Sched *s, PlatCond *cv, uint64_t timeout_ns) {
    s->nparked++;
    if (s->stw) plat_cond_signal(s->stw_cv);
    plat_cond_wait(cv, s->lock, timeout_ns);
    s->nparked--;
}

void sched_stop_world(VM *vm) {
    Sched *s = vm->sched;
    if (!s || !s->running || s->nrun <= 1) return;
    plat_mutex_lock(s->lock);
    s->stw = true;
    s->stw_owner = tls_worker;
    while (!s->done && s->nparked + s->nexited < s->nrun - 1) plat_cond_wait(s->stw_cv, s->lock, 0);
    plat_mutex_unlock(s->lock);
}

void sched_start_world(VM *vm) {
    Sched *s = vm->sched;
    if (!s) return;
    plat_mutex_lock(s->lock);
    if (s->stw) {
        s->stw = false;
        s->stw_owner = NULL;
        plat_cond_broadcast(s->resume_cv);
    }
    plat_mutex_unlock(s->lock);
}

/* ---- the worker loop ---------------------------------------------------------- */

static bool work_available(Sched *s) {
    for (int i = 0; i < s->nworkers; i++)
        if (q_len(s->workers[i])) return true;
    return false;
}

/* The next ready process, locked: from this worker's queue, else stolen. */
static Process *take(Worker *w) {
    Sched *s = w->vm->sched;
    for (;;) {
        uint64_t pid;
        if (!q_take(w, &pid, 1)) pid = steal(w);
        if (!pid) return NULL;
        Process *p = pin(s, pid);
        if (!p) continue;
        p->in_runq = false;
        if (p->state == PROC_READY) return p;
        plat_unlock(p->lock); /* killed while queued */
    }
}

/* Nothing to run: wait for work, a timer, the end, or (worker 0 with a tool
 * attached) serve the tool. Detects deadlock: every worker idle, no timers. */
static void idle(Worker *w) {
    VM *vm = w->vm;
    Sched *s = vm->sched;
    timers_service(s, plat_now_ns()); /* also drops stale timers, so `next_timer` is real */
    bool poller = w->id == 0 && vm->poll;
    bool deadlock = false;
    uint64_t poll_wait = 0;
    plat_mutex_lock(s->lock);
    s->nidle++;
    for (;;) {
        if (s->done) break;
        if (s->stw && s->stw_owner != w) {
            park_locked(s, s->resume_cv, 0);
            continue;
        }
        if (work_available(s)) break;
        uint64_t nt = s->next_timer, now = plat_now_ns();
        if (nt && nt <= now) break;
        if (!nt && s->nidle == s->nrun && !s->io_inflight && !s->io_waiting) { /* nothing can ever run again */
            deadlock = true;
            break;
        }
        if (poller) { /* idle: let the tool wait for requests (other workers may make work) */
            poll_wait = nt ? nt - now : 1000000;
            if (s->nrun > 1 && poll_wait > 1000000) poll_wait = 1000000;
            break;
        }
        park_locked(s, s->work_cv, nt ? nt - now : 0);
    }
    s->nidle--;
    plat_mutex_unlock(s->lock);
    if (deadlock) { /* main is waiting with no possible sender: its recv raises E_DEADLOCK */
        Process *m = pin(s, vm->main_pid);
        if (m) {
            if (m->state == PROC_WAITING) {
                m->deadlock = true;
                ready_locked(s, m);
            }
            plat_unlock(m->lock);
        }
    }
    if (poll_wait) {
        vm->poll(vm, poll_wait);
        w->last_poll = plat_now_ns();
    }
}

/* A kill_req is carried out: the process ends with the killer's error and no
 * backtrace (it was stopped from outside). */
static void kill_from_outside(Process *p) {
    snprintf(p->err.code, sizeof p->err.code, "%s", p->kill_code);
    snprintf(p->err.msg, sizeof p->err.msg, "%s", p->kill_msg);
    p->backtrace.n = 0;
    if (p->backtrace.s) p->backtrace.s[0] = 0;
}

/* ---- the I/O thread pool ------------------------------------------------------- */

/* Blocking natives run here, so a worker thread is never stuck in one. The
 * process is in PROC_NATIVE, and only this thread touches it until the call
 * has returned and the process is ready again. */
static void io_main(void *arg) {
    Sched *s = arg;
    plat_mutex_lock(s->io_lock);
    for (;;) {
        while (!s->io_stop && s->io_head == s->io_queue.n) {
            s->io_idle++;
            plat_cond_wait(s->io_cv, s->io_lock, 0);
            s->io_idle--;
        }
        if (s->io_stop) break;
        Process *p = s->io_queue.v[s->io_head++];
        if (s->io_head == s->io_queue.n) s->io_head = s->io_queue.n = 0;
        s->io_busy++;
        plat_mutex_unlock(s->io_lock);
        p->on_io_thread = true;
        native_invoke(p, p->bcall.nd, p->bcall.args, p->bcall.rets);
        p->on_io_thread = false;
        plat_mutex_lock(s->io_lock);
        bool stopping = s->io_stop;
        plat_mutex_unlock(s->io_lock);
        if (!stopping) { /* the process continues: its ncall completes when it runs again */
            plat_lock(p->lock);
            p->bcall.state = BCALL_DONE;
            ready_locked(s, p);
            plat_unlock(p->lock);
        }
        s->io_inflight--;
        plat_mutex_lock(s->io_lock);
        s->io_busy--;
        plat_cond_broadcast(s->io_done_cv);
    }
    plat_mutex_unlock(s->io_lock);
}

static void io_submit(Sched *s, Process *p) {
    plat_mutex_lock(s->io_lock);
    VEC_PUSH(s->io_queue, p);
    if (s->io_idle == 0 && (int)s->io_threads.n < IO_MAX_THREADS) {
        PlatThread *t = plat_thread_start(io_main, s);
        if (t) VEC_PUSH(s->io_threads, t);
    }
    plat_cond_signal(s->io_cv);
    plat_mutex_unlock(s->io_lock);
}

/* At the end of the program: drop calls not yet started, and wait (a while)
 * for those in progress. A call that never returns (say, a read from a
 * terminal) is abandoned with its process, and the VM is then leaked rather
 * than freed under it. */
static void io_shutdown(Sched *s) {
    plat_mutex_lock(s->io_lock);
    s->io_stop = true;
    s->io_queue.n = s->io_head = 0;
    plat_cond_broadcast(s->io_cv);
    uint64_t deadline = plat_now_ns() + IO_SHUTDOWN_WAIT_NS;
    while (s->io_busy > 0) {
        uint64_t now = plat_now_ns();
        if (now >= deadline) break;
        plat_cond_wait(s->io_done_cv, s->io_lock, deadline - now);
    }
    s->io_abandoned = s->io_busy > 0;
    plat_mutex_unlock(s->io_lock);
    if (s->io_abandoned) return;
    for (size_t i = 0; i < s->io_threads.n; i++) plat_thread_join(s->io_threads.v[i]);
    s->io_threads.n = 0;
}

bool sched_abandoned(VM *vm) { return vm->sched && vm->sched->io_abandoned; }

/* ---- the poller ------------------------------------------------------------------- */

/* Wakes processes whose descriptors are ready. A token is pid << 16 | the wait's
 * generation, so a wake-up for a wait that has since timed out (or a process
 * that has gone) is ignored. */
static void poll_main(void *arg) {
    Sched *s = arg;
    uint64_t tokens[64];
    while (!s->poll_stop) {
        int n = plat_poller_wait(s->poller, tokens, 64, -1);
        for (int i = 0; i < n; i++) {
            Process *p = pin(s, tokens[i] >> 16);
            if (!p) continue;
            if (p->state == PROC_IOWAIT && (p->fdwait.gen & 0xFFFFu) == (tokens[i] & 0xFFFFu)) {
                s->io_waiting--;
                ready_locked(s, p);
            }
            plat_unlock(p->lock);
        }
    }
}

/* Start the poller on first use. Called with a process lock held (leaf locks only). */
static bool poller_ready(Sched *s) {
    plat_mutex_lock(s->lock);
    if (!s->poller) {
        s->poller = plat_poller_new();
        if (s->poller) s->poll_thread = plat_thread_start(poll_main, s);
    }
    bool ok = s->poller && s->poll_thread;
    plat_mutex_unlock(s->lock);
    return ok;
}

static void poller_shutdown(Sched *s) {
    if (!s->poll_thread) return;
    s->poll_stop = true;
    plat_poller_wake(s->poller);
    plat_thread_join(s->poll_thread);
    s->poll_thread = NULL;
}

/* A process asked to wait for a descriptor (SLICE_IOWAIT); p is locked. */
static void wait_for_fd(Sched *s, Process *p) {
    p->state = PROC_IOWAIT;
    s->io_waiting++;
    p->fdwait.gen++;
    uint64_t token = p->pid << 16 | (p->fdwait.gen & 0xFFFFu);
    if (!poller_ready(s) || !plat_poller_arm(s->poller, p->fdwait.fd, p->fdwait.events, token)) {
        s->io_waiting--; /* cannot wait: run again now, and the operation reports the error */
        ready_locked(s, p);
        return;
    }
    if (p->fdwait.deadline) {
        p->wake_at = p->fdwait.deadline;
        timer_add(s, p);
    }
}

void sched_request_exit(VM *vm, int64_t code) {
    Sched *s = vm->sched;
    vm->exit_code = code;
    vm->exit_requested = true;
    plat_mutex_lock(s->lock);
    s->done = true;
    plat_cond_broadcast(s->work_cv);
    plat_cond_broadcast(s->resume_cv);
    plat_cond_broadcast(s->stw_cv);
    plat_mutex_unlock(s->lock);
}

/* After a time slice: requeue, park, or finish the process. Returns true if a
 * tool hook stopped the scheduler. */
static bool after_slice(Worker *w, Process *p, int r) {
    Sched *s = w->vm->sched;
    if (r == SLICE_DONE || r == SLICE_FAILED) {
        finish(s, p, r == SLICE_FAILED);
        return false;
    }
    plat_lock(p->lock);
    if (p->kill_req) { /* killed from another thread during the slice */
        kill_from_outside(p);
        plat_unlock(p->lock);
        finish(s, p, true);
        return false;
    }
    bool stop = false;
    switch (r) {
    case SLICE_YIELD: ready_locked(s, p); break;
    case SLICE_BLOCK:
        if (p->inbox.n) { /* a message arrived after recv looked */
            ready_locked(s, p);
            break;
        }
        p->state = PROC_WAITING;
        if (p->deadline_set) timer_add(s, p);
        break;
    case SLICE_SLEEP:
        p->state = PROC_SLEEPING;
        timer_add(s, p);
        break;
    case SLICE_BREAK:
        p->state = PROC_READY;
        w->vm->stopped = p;
        stop = true;
        break;
    case SLICE_IOWAIT: wait_for_fd(s, p); break;
    case SLICE_NATIVE: /* a blocking native call: the I/O pool makes it */
        p->state = PROC_NATIVE;
        s->io_inflight++;
        plat_unlock(p->lock);
        io_submit(s, p);
        return false;
    default: break;
    }
    plat_unlock(p->lock);
    return stop;
}

/* Returns true if a tool hook stopped the scheduler (single-threaded only). */
static bool worker_loop(Worker *w) {
    VM *vm = w->vm;
    Sched *s = vm->sched;
    tls_worker = w;
    bool stopped = false;
    /* One clock read per slice: the end of one slice is the start of the next. */
    uint64_t now = plat_now_ns();
    while (!s->done) {
        if (s->stw) { /* a tool is reading other processes: pause here */
            plat_mutex_lock(s->lock);
            while (s->stw && !s->done && s->stw_owner != w) park_locked(s, s->resume_cv, 0);
            plat_mutex_unlock(s->lock);
            now = plat_now_ns();
        }
        if (w->id == 0 && vm->poll && now - w->last_poll > 1000000) { /* serve tools about once a millisecond */
            vm->poll(vm, 0);
            now = w->last_poll = plat_now_ns();
        }
        uint64_t nt = s->next_timer;
        if (nt && nt <= now) timers_service(s, now);
        Process *p = NULL;
        if (vm->stopped) { /* a process stopped by a tool resumes first */
            Process *q = vm->stopped;
            vm->stopped = NULL;
            plat_lock(q->lock);
            if (q->state == PROC_READY && !q->in_runq) p = q; /* if queued, take it in order */
            else plat_unlock(q->lock);
        }
        if (!p) p = take(w);
        if (!p) {
            idle(w);
            now = plat_now_ns();
            continue;
        }
        if (p->kill_req) { /* killed during a blocking native call */
            kill_from_outside(p);
            plat_unlock(p->lock);
            finish(s, p, true);
            continue;
        }
        p->state = PROC_RUNNING;
        take_inbox_locked(p);
        plat_unlock(p->lock);
        w->current = p;
        int r = proc_run_slice(p, vm->slice_insns);
        uint64_t end = plat_now_ns();
        p->cpu_ns += end - now;
        now = end;
        w->current = NULL;
        vm->slices++;
        if (after_slice(w, p, r)) {
            stopped = true;
            break;
        }
    }
    tls_worker = NULL;
    return stopped;
}

static void worker_main(void *arg) {
    Worker *w = arg;
    Sched *s = w->vm->sched;
    worker_loop(w);
    copy_state_free();
    plat_mutex_lock(s->lock);
    s->nexited++;
    plat_cond_signal(s->stw_cv);
    plat_mutex_unlock(s->lock);
}

bool sched_run(VM *vm, Process *main) {
    Sched *s = sched_of(vm);
    if (!vm->slice_insns) vm->slice_insns = 2000;
    s->done = !is_live(main);
    if (s->done) return false;
    /* Tools that see every instruction (debugger, trace, profiler) run on one thread. */
    int n = vm->hook ? 1 : s->nworkers;
    s->nrun = n;
    s->nidle = 0;
    s->nparked = s->nexited = 0;
    s->stw = false;
    s->running = true;
    for (int i = 1; i < n; i++) {
        s->workers[i]->thread = plat_thread_start(worker_main, s->workers[i]);
        if (!s->workers[i]->thread) { /* run with the threads we have */
            plat_mutex_lock(s->lock);
            s->nrun = i;
            plat_cond_broadcast(s->work_cv);
            plat_mutex_unlock(s->lock);
            n = i;
            break;
        }
    }
    bool stopped = worker_loop(s->workers[0]);
    for (int i = 1; i < n; i++) {
        plat_thread_join(s->workers[i]->thread);
        s->workers[i]->thread = NULL;
    }
    s->running = false;
    return stopped;
}

/* ---- teardown -------------------------------------------------------------------- */

/* Free every process except `keep` (the main process). Single-threaded. */
void sched_shutdown(VM *vm, Process *keep) {
    Sched *s = vm->sched;
    if (!s) return;
    poller_shutdown(s); /* it looks processes up: stop it before any is freed */
    if (!s->io_stop) io_shutdown(s);
    if (s->io_abandoned) return; /* a blocking call still uses its process: free nothing */
    for (uint32_t c = 0; c < PT_CHUNKS; c++) {
        Process **chunk = atomic_load(&s->chunks[c]);
        if (!chunk) continue;
        for (uint32_t i = 0; i < PT_CHUNK; i++)
            if (chunk[i] && chunk[i] != keep) proc_free(chunk[i]);
    }
    for (int i = 0; i < s->nworkers; i++) s->workers[i]->qlen = 0;
    s->timers.n = 0;
    timers_changed(s);
}

void sched_free(VM *vm) {
    Sched *s = vm->sched;
    if (!s) return;
    sched_shutdown(vm, NULL);
    if (s->io_abandoned) return; /* see io_shutdown */
    plat_mutex_free(s->io_lock);
    plat_cond_free(s->io_cv);
    plat_cond_free(s->io_done_cv);
    VEC_FREE(s->io_queue);
    VEC_FREE(s->io_threads);
    for (uint32_t c = 0; c < PT_CHUNKS; c++) free(atomic_load(&s->chunks[c]));
    for (int i = 0; i < s->nworkers; i++) {
        Worker *w = s->workers[i];
        heap_free_all(w->frag);
        free(w->frag);
        free(w->q);
        plat_lock_free(w->qlock);
        free(w);
    }
    free(s->workers);
    for (unsigned i = 0; i < PT_STRIPES; i++) plat_lock_free(s->stripes[i]);
    plat_mutex_free(s->lock);
    plat_cond_free(s->work_cv);
    plat_cond_free(s->stw_cv);
    plat_cond_free(s->resume_cv);
    plat_poller_free(s->poller);
    VEC_FREE(s->timers);
    free(s);
    vm->sched = NULL;
}
