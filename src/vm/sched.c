/* sched.c - the process scheduler: a run queue of ready processes, a timer heap
 * for sleep/recvt deadlines, process exit handling (links and monitors), and
 * kill. Single OS thread; each process runs a time slice of vm->slice_insns
 * instructions. See docs/ABI.md ("Processes"). */
#include "../heap/heap.h"
#include "../platform/platform.h"
#include "proc_internal.h"

Process *vm_process(VM *vm, uint64_t pid) {
    if (pid == 0 || pid > vm->procs.n) return NULL;
    Process *p = vm->procs.v[pid - 1];
    return p && p->state != PROC_DONE && p->state != PROC_FAILED ? p : NULL;
}

/* ---- run queue (ring buffer of pids) ---------------------------------------- */

void sched_ready(VM *vm, Process *p) {
    p->state = PROC_READY;
    if (p->in_runq) return;
    if (vm->runq_len == vm->runq.n) { /* grow, unwrapping the ring */
        size_t old = vm->runq.n, nn = old ? old * 2 : 64;
        uint64_t *v = xmalloc(nn * sizeof *v);
        for (size_t i = 0; i < vm->runq_len; i++) v[i] = vm->runq.v[(vm->runq_head + i) % old];
        free(vm->runq.v);
        vm->runq.v = v;
        vm->runq.n = vm->runq.cap = nn;
        vm->runq_head = 0;
    }
    vm->runq.v[(vm->runq_head + vm->runq_len) % vm->runq.n] = p->pid;
    vm->runq_len++;
    p->in_runq = true;
}

static Process *runq_pop(VM *vm) {
    while (vm->runq_len) {
        uint64_t pid = vm->runq.v[vm->runq_head];
        vm->runq_head = (vm->runq_head + 1) % vm->runq.n;
        vm->runq_len--;
        Process *p = vm_process(vm, pid);
        if (p) {
            p->in_runq = false;
            if (p->state == PROC_READY) return p;
        }
    }
    return NULL;
}

/* ---- timers (binary min-heap; stale entries are skipped when popped) ------ */

void sched_add_timer(VM *vm, Process *p) {
    Timer t = {p->wake_at, p->pid};
    VEC_PUSH(vm->timers, t);
    size_t i = vm->timers.n - 1;
    while (i > 0) {
        size_t parent = (i - 1) / 2;
        if (vm->timers.v[parent].at <= vm->timers.v[i].at) break;
        Timer tmp = vm->timers.v[parent];
        vm->timers.v[parent] = vm->timers.v[i];
        vm->timers.v[i] = tmp;
        i = parent;
    }
}

static void timer_pop(VM *vm) {
    vm->timers.v[0] = vm->timers.v[--vm->timers.n];
    size_t i = 0;
    for (;;) {
        size_t l = 2 * i + 1, r = l + 1, m = i;
        if (l < vm->timers.n && vm->timers.v[l].at < vm->timers.v[m].at) m = l;
        if (r < vm->timers.n && vm->timers.v[r].at < vm->timers.v[m].at) m = r;
        if (m == i) break;
        Timer tmp = vm->timers.v[m];
        vm->timers.v[m] = vm->timers.v[i];
        vm->timers.v[i] = tmp;
        i = m;
    }
}

/* Is the timer entry still meaningful (process sleeping, or waiting on a deadline)? */
static Process *timer_target(VM *vm, const Timer *t) {
    Process *p = vm_process(vm, t->pid);
    if (!p || p->wake_at != t->at) return NULL;
    if (p->state == PROC_SLEEPING || (p->state == PROC_WAITING && p->deadline_set)) return p;
    return NULL;
}

/* Wake every process whose timer has expired. Returns the earliest pending
 * wake time, or 0 if there are no live timers. */
static uint64_t wake_due(VM *vm, uint64_t now) {
    while (vm->timers.n) {
        Timer t = vm->timers.v[0];
        Process *p = timer_target(vm, &t);
        if (!p) {
            timer_pop(vm);
            continue;
        }
        if (t.at > now) return t.at;
        timer_pop(vm);
        sched_ready(vm, p); /* recvt re-executes and sees that its deadline passed */
    }
    return 0;
}

/* ---- exits, links, monitors, kill ------------------------------------------- */

static void report_unobserved(VM *vm, Process *p) {
    StrBuf b = {0};
    sb_printf(&b, "process %llu (%s) failed: [%s] %s\n%s", (unsigned long long)p->pid,
              p->entry ? p->entry->name : "?", p->err.code, p->err.msg, p->backtrace.s ? p->backtrace.s : "");
    if (vm->err_out) vm->err_out(vm->err_ctx, b.s, b.n);
    else fputs(b.s, stderr);
    sb_free(&b);
}

static void send_down(VM *vm, Process *watcher, Process *dead, bool failed) {
    ObjStruct *d = struct_new(watcher, &VM_DOWN_DEF);
    d->fields[DOWNF_PID].i = (int64_t)dead->pid;
    d->fields[DOWNF_EXIT].i = dead->exit_code;
    if (failed) {
        d->fields[DOWNF_CODE].r = str_new(watcher, dead->err.code, strlen(dead->err.code));
        d->fields[DOWNF_MESSAGE].r = str_new(watcher, dead->err.msg, strlen(dead->err.msg));
    }
    Message m = {{.r = d}, T_R};
    VEC_PUSH(watcher->mbox, m);
    if (watcher->state == PROC_WAITING) sched_ready(vm, watcher);
}

void sched_down_noproc(VM *vm, Process *watcher, uint64_t pid) {
    ObjStruct *d = struct_new(watcher, &VM_DOWN_DEF);
    d->fields[DOWNF_PID].i = (int64_t)pid;
    d->fields[DOWNF_CODE].r = str_new(watcher, "E_NOPROC", 8);
    d->fields[DOWNF_MESSAGE].r = str_new(watcher, "no such process", 15);
    Message m = {{.r = d}, T_R};
    VEC_PUSH(watcher->mbox, m);
    (void)vm;
}

static void unlink_pid(PidVec *v, uint64_t pid) {
    for (size_t i = 0; i < v->n; i++)
        if (v->v[i] == pid) v->v[i--] = v->v[--v->n];
}

void sched_kill(VM *vm, Process *target, const char *code, const char *msg) {
    if (!target || target->state == PROC_DONE || target->state == PROC_FAILED) return;
    snprintf(target->err.code, sizeof target->err.code, "%s", code);
    snprintf(target->err.msg, sizeof target->err.msg, "%s", msg);
    if (target == vm->current) { /* stop at the next instruction boundary */
        target->kill_pending = true;
        return;
    }
    target->backtrace.n = 0; /* no backtrace: it was stopped from outside */
    if (target->backtrace.s) target->backtrace.s[0] = 0;
    sched_finish(vm, target, true);
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

void sched_finish(VM *vm, Process *p, bool failed) {
    p->state = failed ? PROC_FAILED : PROC_DONE;
    vm->live--;
    record_exit(vm, p, failed);
    for (size_t i = 0; i < p->monitors.n; i++) {
        Process *w = vm_process(vm, p->monitors.v[i]);
        if (w) send_down(vm, w, p, failed);
    }
    bool observed = p->monitors.n > 0 || p->links.n > 0;
    /* Linked processes die with a failing one; a normal exit just unlinks. */
    PidVec links = p->links;
    memset(&p->links, 0, sizeof p->links);
    for (size_t i = 0; i < links.n; i++) {
        Process *l = vm_process(vm, links.v[i]);
        if (!l) continue;
        unlink_pid(&l->links, p->pid);
        if (failed) {
            char msg[256];
            snprintf(msg, sizeof msg, "linked process %llu failed: %s", (unsigned long long)p->pid, p->err.code);
            sched_kill(vm, l, "E_LINKED", msg);
        }
    }
    VEC_FREE(links);
    if (failed && !observed && p->pid != vm->main_pid) report_unobserved(vm, p);
    if (p->pid != vm->main_pid) { /* main is freed by its runner, after collecting stats */
        vm->procs.v[p->pid - 1] = NULL;
        proc_free(p);
    }
}

/* ---- the scheduling loop ------------------------------------------------------ */

static bool finished(const Process *p) { return p->state == PROC_DONE || p->state == PROC_FAILED; }

bool sched_run(VM *vm, Process *main) {
    if (!vm->slice_insns) vm->slice_insns = 2000;
    uint64_t last_poll = 0;
    while (!finished(main)) {
        uint64_t now0 = plat_now_ns();
        if (vm->poll && now0 - last_poll > 1000000) { /* service tools about once a millisecond */
            vm->poll(vm, 0);
            last_poll = now0;
        }
        uint64_t next_timer = vm->timers.n ? wake_due(vm, now0) : 0;
        Process *p = vm->stopped; /* a process stopped by a tool resumes first */
        if (p) {
            vm->stopped = NULL;
            if (p->state != PROC_READY) p = NULL;
            else if (p->in_runq) p = NULL; /* already queued; take it in order */
        }
        if (!p) p = runq_pop(vm);
        if (!p) {
            if (next_timer) { /* only sleepers: wait for the earliest */
                uint64_t now = plat_now_ns();
                if (next_timer > now) {
                    if (vm->poll) vm->poll(vm, next_timer - now); /* idle: let tools wait for requests */
                    else plat_sleep_ns(next_timer - now);
                }
                continue;
            }
            /* Nothing can ever run again: main is waiting with no possible sender. */
            main->deadlock = true;
            sched_ready(vm, main);
            continue;
        }
        vm->current = p;
        p->state = PROC_RUNNING;
        uint64_t t0 = plat_now_ns();
        int r = proc_run_slice(p, vm->slice_insns);
        p->cpu_ns += plat_now_ns() - t0;
        vm->current = NULL;
        vm->slices++;
        switch (r) {
        case SLICE_YIELD: sched_ready(vm, p); break;
        case SLICE_BLOCK:
            p->state = PROC_WAITING;
            if (p->deadline_set) sched_add_timer(vm, p);
            break;
        case SLICE_SLEEP:
            p->state = PROC_SLEEPING;
            sched_add_timer(vm, p);
            break;
        case SLICE_DONE: sched_finish(vm, p, false); break;
        case SLICE_BREAK:
            p->state = PROC_READY;
            vm->stopped = p;
            return true;
        default: sched_finish(vm, p, true); break;
        }
    }
    return false;
}

/* Kill every remaining process except `keep` (the main process). */
void sched_shutdown(VM *vm, Process *keep) {
    for (size_t i = 0; i < vm->procs.n; i++) {
        Process *p = vm->procs.v[i];
        if (!p || p == keep) continue;
        vm->procs.v[i] = NULL;
        proc_free(p);
    }
    vm->runq_len = 0;
    vm->timers.n = 0;
}
