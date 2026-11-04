# 7. Processes and the scheduler

So far one program has run one function at a time. Real programs wait for
the network, serve many clients and use several cores, so a VM needs some
way to run many things at once. This chapter starts with the two common
answers, threads that share memory and processes that share nothing, and
says why MLWSVM took the second, as Erlang does. Then it follows the
pieces in the source: what a process is, how `spawn`, `send` and `recv`
work, how a process is stopped after its time slice, how worker threads
share out the ready processes, how a failure reaches the processes that
watch for it, and how a process can wait for I/O without holding a thread.
The choice matters well beyond concurrency: because no heap is ever
touched by two threads, the collector of [chapter 5](05-memory-and-gc.md)
and the interpreter of chapters 3 and 4 need no locks at all.

## Two ways to run things at once

The first model is **threads with shared memory**. Several threads run in
one address space and can all reach the same objects. That is what C,
Java, C# and Go give you. It is cheap to pass data (pass a pointer), but
every object two threads can reach needs a lock or careful atomic
operations, and a missing one is a data race: a bug that shows up rarely,
differently on each machine, and is hard to reproduce. The VM pays as
well. A collector must find every reference in every thread, so it either
stops all threads (the JVM's safepoints) or runs alongside them with
barriers on every store. CPython kept one global interpreter lock for
decades largely so that its reference counts and objects stayed
consistent.

The second model is **isolated processes with messages**, from Erlang. A
process is a small unit of execution with its own heap. Nothing mutable is
shared; the only way to affect another process is to send it a message,
which is copied into the receiver's heap. Copying costs time, and data
that is large and read-mostly has to be copied or held elsewhere. In
return there are no data races, a failing process cannot corrupt another,
and each heap can be collected on its own, without stopping anyone else.

| | Threads, shared memory | Processes, messages |
|---|---|---|
| Passing data | a pointer | a copy |
| Correctness | locks around shared objects | nothing is shared |
| Garbage collection | all threads together | one heap at a time |
| A failure | can leave shared state broken | ends one process, which others can watch |
| Examples | C, Java, Go, CPython threads | Erlang/BEAM |

MLWSVM's goals ([DESIGN.md §1](../DESIGN.md#1-goals)) choose the second:
"Concurrency means many small processes that share nothing mutable and talk
by copying messages, as in Erlang. That keeps the collector, the scheduler
and the safety argument simple: no heap is ever touched by two threads."
The safety goal (no program can reach memory that is not its own) is much
easier to argue when no reference ever crosses from one heap to another.
[PRIOR_ART.md](../PRIOR_ART.md) lists what was borrowed: BEAM's processes,
mailboxes and reduction counting, its per-thread schedulers, and Go's
scheduler for the run queues.

## What a process is

A process is a `struct Process` in [src/vm/vm.h](../../src/vm/vm.h). It
holds everything one thread of execution needs, and most of its fields are
only ever touched by the worker thread running the process at the time.
Trimmed:

```c
struct Process {
    VM *vm;
    uint64_t pid;
    int state;
    int64_t exit_code;
    /* the register stack: each frame owns a window of 8-byte words, its I, F,
     * R and P registers one after another (Function.roff), each word read and
     * written with its bank's type */
    uint64_t *regs;
    uint32_t rcap, rtop;
    Frame *frames;
    ...
    Segment stack;
    Segment *globals;
    ...
    /* garbage-collected heap (heap/gc.c) */
    Obj *heap;                   /* the objects not in the chunks below: from malloc */
    ...
    /* processes (sched.c). Fields marked [L] are shared with other threads and
     * guarded by `lock`; the rest belong to the thread running the process. */
    Function *entry;
    struct PlatLock *lock;
    VEC(Message) mbox;           /* taken messages, in this heap (GC roots) */
    size_t mbox_head;
    VEC(InMsg) inbox;            /* [L] delivered, not yet taken */
    _Atomic uint64_t mcount;     /* messages waiting: mbox + inbox */
    uint64_t wake_at;            /* SLEEPING, or WAITING with a recvt deadline */
    ...
    PidVec links;                /* [L] */
    PidVec monitors;             /* [L] pids watching this process */
    ...
    uint64_t quota[QUOTA__COUNT]; /* the mailbox quota is [L] */
```

The registers, frames, data stack and globals of
[chapter 6](06-calls-frames-exceptions.md) are all here, per process. So is
the heap of chapter 5. Only the fields marked `[L]` are ever touched by
another thread (the inbox where senders leave messages, the links and
monitors, the scheduling flags), and only under the process's own `lock`.
The test [tests/processes/isolation.masm](../../tests/processes/isolation.masm)
shows that a child gets a fresh copy of a module's globals: it adds one to
`@counter` and the parent's copy still reads 100.

A process moves between a few states
([DESIGN.md §13.2](../DESIGN.md#132-process-states)):

```text
               spawn                      a worker takes it
   (new) ───────────► READY ──────────────────────────────► RUNNING
                        ▲                                      │ the slice ends with:
                        ├──────────────────────────────────────┤ YIELD
                        ├─ a message, or the deadline ─ WAITING ◄─ BLOCK  (recv, proc.call)
                        ├─ its timer ──────────────── SLEEPING ◄─ SLEEP
                        ├─ the descriptor, or timeout ── IOWAIT ◄─ IOWAIT (§15)
                        ├─ the call returns ──────────── NATIVE ◄─ NATIVE (§15)
                        └─ (the debugger continues)               BREAK
                                                         DONE / FAILED ◄─ DONE, FAILED
```

The process table maps a pid to its `Process`. Pids start at 1 (the first
`main`) and are never reused, so a stale pid can never name a different
process later. Run queues and messages hold pids, not pointers: a lookup
*pins* the process (takes its lock while the table's stripe lock is held),
and a pid whose process has gone simply fails to pin.

## Spawning

`spawn worker (a, b) -> (I0)` starts a new process running `worker` and puts
its pid in `I0`. The generic handler in
[src/vm/generic.inc](../../src/vm/generic.inc) (`OP_SPAWN`) makes the
process, copies the arguments into it, and queues it:

```c
        case OP_SPAWN:
        case OP_SPAWNL:
        case OP_SPAWNM: {
            ...
            Process *child = proc_new(vm);
            child->prog = p->prog; /* the same program */
            memcpy(child->quota, p->quota, sizeof child->quota);
            p->pending_spawn = child; /* freed by proc_run_slice if a copy traps */
            for (int k = 0; k < argc; k++) args[k] = msg_copy(p, child, args[k], fn->mf->params[k]);
            p->pending_spawn = NULL;
            spawn_watch(p, child, in->op == OP_SPAWNL, in->op == OP_SPAWNM);
            uint64_t cpid = child->pid; /* once started, the child may run, end and be freed at once */
            proc_start(child, fn, NULL, args, argc);
```

Arguments are copied with `msg_copy`, the same deep copy as a message
(below), straight into the child's heap, since nothing else can see the
child yet. The child inherits its parent's quotas. The comment on `cpid`
is the first sign of parallelism: once `proc_start` queues the child,
another worker may run it to the end and free it before this line
returns. That is also why `spawnm` and `spawnl` exist. With `spawn`
followed by `monitor`, a quick child can finish in between, and the
monitor would report that the process no longer exists; `spawn_watch` sets
up the monitor or link before the child can run.

## Sending is a deep copy

`send pid, value` puts a copy of `value` in another process's mailbox. A
number is just a word and needs no copy. A reference is the hard case: the
receiver must get its own copy of everything the value reaches, because a
shared object would be reachable from two heaps. Two things make a naive
recursive copy wrong:

* **Sharing.** If a list holds the same inner list twice, the copy must hold
  one copied inner list twice, not two different ones.
* **Cycles.** A list that contains itself would make a recursive copy run
  forever.

The fix is the one a copying garbage collector uses (its *forwarding
pointers*): remember, for each object already copied, where its copy is.
`msg_copy` in [src/heap/copy.c](../../src/heap/copy.c) keeps a hash map
from each original to its copy, and a worklist instead of recursion, so a
deep structure cannot overflow the C stack. `shallow` copies one object
without its references, or returns the copy already made:

```c
static Obj *shallow(Process *from, Process *to, struct CopyState *cs, Obj *o) {
    if (!o || (o->flags & OBJF_IMMORTAL)) return o;
    Obj *c = map_lookup(cs, o);
    if (c) return c;
    switch (o->kind) {
    case OBJ_STRING: {
        ObjString *s = (ObjString *)o;
        c = &str_new(to, s->data, s->len)->h;
        break;
    }
    ...
    case OBJ_FOREIGN:
        vm_trap(from, "E_TYPE", "foreign handles cannot be sent to another process (they are process-local)");
    ...
    }
    map_insert(cs, o, c);
    CopyPair pr = {o, c};
    VEC_PUSH(cs->work, pr);
    return c;
}
```

`msg_copy` then pops pairs off the worklist and fills in each copy's
references by calling `shallow` on each one, until the worklist is empty.
Immortal objects (constant-pool strings) are shared, since nothing can
change them. Raw pointers, foreign handles and coroutines belong to one
process and trap `E_TYPE` in the sender.

Where the copy is built matters too. `sched_send` in
[src/vm/sched.c](../../src/vm/sched.c) copies into a scratch process that
belongs to the sender's worker thread (`frag`), outside any lock, because a
copy can trap and a trap must not leave a lock held. Then it looks the
receiver up again, since it may have ended meanwhile:

```c
void sched_send(Process *p, uint64_t pid, Word v, uint8_t tag) {
    Sched *s = p->vm->sched;
    InMsg m = {v, tag, NULL, 0, 0};
    Process *to = pin(s, pid);
    if (to && accepts(to) && to->mcount < to->quota[QUOTA_MAILBOX] && type_is_ref(tag) && v.r) {
        /* Copy outside the lock (copying can trap), then look again: the
         * receiver may have finished, or filled up, meanwhile. */
        plat_unlock(to->lock);
        Process *f = my_frag(s);
        frag_reset(f);
        m = frag_take(f, msg_copy(p, f, v, tag), tag);
        to = pin(s, pid);
    }
    ...
    push_locked(s, to, &m);
    plat_unlock(to->lock);
}
```

The result is an `InMsg`: the value plus a detached list of the objects
the copy made. `push_locked` appends it to the receiver's `inbox` and, if
the receiver is waiting, makes it ready. When the receiver next runs, on
its own thread, `take_inbox_locked` moves each message into its mailbox
(`mbox`), and `gc_adopt` splices the message's objects onto its heap. So
the sender never writes into the receiver's heap, and the receiver
never reads the sender's. BEAM does something similar with its heap
fragments, but only when the receiver's heap is busy; MLWSVM always does
it this way. Messages from one process to another arrive in the order they
were sent ([ABI.md §1.1](../ABI.md#11-scheduling)).

This program checks all three properties: sharing, a cycle, and that the
receiver's change does not reach the sender.

```asm
; A message is a deep copy: sharing and cycles survive, and the
; receiver's changes do not reach the sender.
.include "std/test.inc"

.func child(i64 parent)
    recv R0                            ; [inner, inner, itself]
    mov R1, R0[0]
    mov R2, R0[1]
    ASSERT_EQ R1, R2, r                ; one inner list, not two
    mov R3, R0[2]
    ASSERT_EQ R3, R0, r                ; the cycle is still a cycle
    lpush R1, 99                       ; change our copy
    send parent, R0
    ret
.endf

.func main
    spawn child (PID) -> (I9)
    lnew R1                            ; inner = [1]
    lpush R1, 1
    lnew R0
    lpush R0, R1, R1, R0               ; outer = [inner, inner, outer]
    send I9, R0
    recv R2                            ; what the child sent back
    len I0, R1
    ASSERT_EQ I0, 1                    ; our inner list is unchanged
    mov R3, R2[0]
    len I1, R3
    ASSERT_EQ I1, 2                    ; theirs has the 99
    ASSERT_NE R2, R0, r                ; and it is a different object
    ret 0
.endf
```

```sh
$ ./mlwsvm test copy.masm
1 passed, 0 failed, 0 skipped (1 files)
```

`ASSERT_EQ a, b, r` compares two references for identity (chapter 12 shows
the macros). Change `lpush R0, R1, R1, R0` to push two separate lists and
the first assertion fails in the child.

## Receiving

`recv dst` takes the oldest message. If there is none, the process must
stop running until one arrives, without holding the worker thread. The
interpreter cannot wait inside an instruction, so the handler backs up and
ends the slice; when the process is woken, the same `recv` runs again and
finds the message. From `OP_RECV` in generic.inc:

```c
        case OP_RECV:
        case OP_RECVT: {
            if (p->mbox_head == p->mbox.n) sched_take_inbox(p);
            if (p->mbox_head < p->mbox.n) {
                ...
                Loc L = mbox_loc(&p->mbox.v[p->mbox_head]);
                Word v = slot_read(CX, &L, &t); /* a type mismatch traps and leaves the message */
                mbox_take(p);
                p->deadline_set = false;
                wr(CX, &ops[0], t, v);
                break;
            }
            ...
            } else if (p->deadlock) {
                p->deadlock = false;
                vm_trap(p, "E_DEADLOCK", "every process is waiting: no message can ever arrive");
            }
            ...
            fr->pc--; /* re-execute this receive when woken */
            p->insns--;
            return SLICE_BLOCK;
        }
```

`recvt dst, ms, Ltimeout` is the same with a deadline: the first time it
runs it records `wake_at`, and when it runs again after the deadline it
jumps to the timeout label. Running an instruction again after a wait is a
pattern the VM uses everywhere a process waits: for a message, a timer, a
descriptor or a blocking call. No C stack is kept for a waiting process;
its state is all in its registers and frames, as it is between any two
instructions.

Erlang's `receive` is *selective*: it scans the mailbox for the first
message that matches a pattern and leaves the others. MLWSVM has no
selective receive in the instruction set. A compiler that needs it loops on
`recv`, tests each message with `isa` or `typeof`, and keeps the ones it
does not want yet in a list ([TARGETING.md §9](../TARGETING.md#9-concurrency)).
The commonest need, waiting for the reply to one request, is met another
way: `proc.call` puts its reply in a reserved slot of the caller rather
than its mailbox, so the caller never searches for it, and a late reply
after a timeout is dropped by its call id
([STDLIB.md](../STDLIB.md#processes-names-calls-and-supervisors)).
[PERFORMANCE.md](../PERFORMANCE.md#compiled-erlang) notes that most of what
the compiled Erlang `ring` benchmark runs is that compiler's selective
receive, written in VM code around `recv`.

## Time slices and preemption

A scheduler can be *cooperative*, where a process runs until it chooses to
give way (early Mac OS, JavaScript's event loop, Python's `asyncio`), or
*preemptive*, where it is stopped after a while whatever it is doing. A
cooperative scheduler is simpler, but one process in an endless loop
stops everything. Operating systems preempt with a timer interrupt. A VM
can do better than a timer: it is already counting through instructions,
so it can stop a process after a fixed amount of work. BEAM counts
*reductions* (roughly, function calls); MLWSVM counts instructions. A time
slice is 2,000 instructions by default (`--slice N`).

In the tail-call interpreter of chapter 3, every handler ends with the
`NEXT` macro of [src/vm/interp.c](../../src/vm/interp.c). It already
carries the count of what is left of the slice, `left`, in a register:

```c
#define NEXT                                                                   \
    do {                                                                       \
        if (__builtin_expect(left == 0, 0)) MUSTTAIL return h_slice(QARGSN(nx)); \
        left--;                                                                \
        MUSTTAIL return qhandlers[nx->xop](QARGSN(nx));                        \
    } while (0)
```

So preemption costs one compare and one decrement per instruction, and
when `left` reaches zero `h_slice` writes back the program counter and
returns `SLICE_YIELD` to the scheduler. The machine code of the JIT keeps
the same count exactly ([chapter 9](09-jit-interpreter.md)), so a slice
ends at the same instruction with or without it. The end of a slice is
also a *safepoint*: a point between instructions where every live
reference is in a register or a heap object, so a pending collection can
run or a kill from another thread can take effect.

The instruction count does one more job. `proc_run_slice` in
[src/vm/process.c](../../src/vm/process.c) shortens the slice to what the
process's instruction quota allows, so `quota 3, N` limits how much CPU a
process may ever use, which is how a sandbox stops untrusted code that
loops.

## The scheduler

Each worker thread (one per core by default, `--threads N`) runs this loop,
`worker_loop` in sched.c, trimmed:

```c
    while (!s->done) {
        ...
        uint64_t nt = s->next_timer;
        if (nt && nt <= now) timers_service(s, now);
        Process *p = NULL;
        ...
        if (!p) p = take(w);
        if (!p) {
            idle(w);
            now = plat_now_ns();
            continue;
        }
        ...
        p->state = PROC_RUNNING;
        take_inbox_locked(p);
        plat_unlock(p->lock);
        w->current = p;
        int r = proc_run_slice(p, vm->slice_insns);
        uint64_t end = plat_now_ns();
        p->cpu_ns += end - now;
        now = end;
        ...
        if (after_slice(w, p, r)) {
```

Take a ready process, run one slice, and look at why the slice ended:
`after_slice` requeues it (`YIELD`), parks it (`BLOCK`, `SLEEP`), hands it
to the poller or the I/O pool, or finishes it. The clock is read once per
slice: the end of one slice is the start of the next.
[PERFORMANCE.md](../PERFORMANCE.md#the-multi-threaded-scheduler) measured
why that matters. In message-heavy code a slice may be a handful of
instructions, and with three clock reads per slice and plain mutexes
instead of `os_unfair_lock`, the `pingpong` benchmark took twice as long.

**Run queues.** Each worker has its own queue of ready pids, a ring buffer
under its own small lock (`q_push`, `q_take`). One shared queue would be
simpler, but every thread would contend for its lock on every slice. A
worker whose queue is empty steals from the others (`steal`), as Go's
scheduler and Cilk do: it takes half of the first non-empty queue it
finds, at most 32 pids, runs the first and queues the rest.

**Where a woken process goes.** `ready_locked` puts a woken process on the
queue of the thread that woke it, so a receiver lands on its sender's
worker:

```c
static void ready_locked(Sched *s, Process *p) {
    p->state = PROC_READY;
    if (p->in_runq) return;
    p->in_runq = true;
    Worker *w = my_worker(s);
    size_t n = q_push(w, p->pid);
    ...
    if (n >= 2 || w != tls_worker) wake_next(s);
}
```

A pair of processes talking to each other therefore stays on one thread,
and a message between them costs no cross-thread wake-up. Waking a
sleeping thread costs microseconds, far more than a message, so another
worker is woken only when this queue has a backlog, and only one at a time
(`waking`): the woken worker wakes the next if it steals more than it can
run. [DESIGN.md §13.3](../DESIGN.md#133-a-workers-loop) gives the
measurement: a process spawning a million processes that each wait for a
message, then waking them all, takes 0.9 s on one thread and 1.7 s on 18;
with a wake-up for every ready process it took 6.5 s on 18.

You can see the pair staying together with the `pingpong` benchmark, 300,000
round trips between two processes. On the M5 Pro it takes 0.03 s on one
thread and on eighteen, and every `recv` that finds the mailbox empty ends
a slice:

```sh
$ ./mlwsvm run --stats bench/pingpong.masm
300000
--- mlwsvm stats ---
...
wall time:        32.336 ms
...
processes:        2 spawned, 2 at most at once, 600002 time slices
all instructions: 2400006 (every process)
```

**The lost wake-up.** A classic concurrency bug hides in "block": a
process looks at its mailbox, finds it empty, and decides to wait, and a
message arrives between the look and the wait. If nothing checks again, the
process sleeps forever with a message in its inbox. `after_slice` closes
the gap under the process's lock, which every sender takes too:

```c
    case SLICE_BLOCK:
        if (p->inbox.n || p->reply_ready) { /* a message (or proc.call's reply) arrived after it looked */
            ready_locked(s, p);
            break;
        }
        p->state = PROC_WAITING;
        if (p->deadline_set) timer_add(s, p);
        break;
```

**Timers.** `sleep`, `recvt` deadlines and I/O timeouts put `{at, pid}`
into one binary min-heap (`timer_add`, `timers_service`). An entry is not
removed when its process wakes early; instead, when it reaches the top, it
is used only if the process is still waiting for that same deadline
(`p->wake_at == t.at`). Stale entries are simply dropped, which saves
finding and removing an entry in the middle of the heap.

**Deadlock.** When a worker finds nothing to do, `idle` checks whether
anything could ever make work again
([DESIGN.md §13.6](../DESIGN.md#136-deadlock)). Every worker idle, no
queue with work, no timer, no I/O in flight and no one outside the VM who
could send: then no process can ever be woken, and
each program's `main` that is waiting is woken with a flag that makes its
`recv` raise `E_DEADLOCK`. That turns a hang into an error with a line
number:

```asm
; The child fails before it answers. Nobody watches it, so the VM prints a
; crash report; main then waits for a message that can never come.
.func child(i64 parent, i64 n)
    div I2, 100, n                     ; n is 0: E_DIV_ZERO
    send parent, I2
    ret
.endf

.func main
    spawn child (PID, 0)
    recv I0
    ret I0
.endf
```

```sh
$ ./mlwsvm run lost.masm
process 2 (child) failed: [E_DIV_ZERO] integer division by zero
  at lost.child (lost.masm:4)
runtime error[E_DEADLOCK]: every process is waiting: no message can ever arrive
  at lost.main (lost.masm:11)
```

## Links, monitors and supervisors

Erlang's approach to errors is "let it crash": a process that hits an
error it cannot handle ends, and another process that watches it decides
what to do. That needs two mechanisms, both in MLWSVM:

* A **monitor** (`monitor pid`, or `spawnm`) asks to be told when a process
  ends. The watcher receives a built-in `Down` struct with the pid, exit
  code, and error code and message (null for a normal exit).
* A **link** (`link pid`, or `spawnl`) ties two processes together: if
  either fails, the other is killed with `E_LINKED`. A normal exit only
  removes the link.

`finish` in sched.c runs when a process ends. The order of its steps is
deliberate ([DESIGN.md §13.5](../DESIGN.md#135-ending-links-and-monitors)):

```c
    heap_release_foreign(p);
    if (p->names.n) unregister_all(vm, p); /* before the Downs: a restarted service can take its name again */
    for (size_t i = 0; i < mons.n; i++) send_down(s, mons.v[i], p, failed);
    bool observed = mons.n > 0 || links.n > 0;
    /* Linked processes die with a failing one; a normal exit just unlinks. */
    for (size_t i = 0; i < links.n; i++) {
        ...
        if (failed && is_live(l)) {
            ...
            claimed = kill_locked(l, "E_LINKED", msg);
        }
        plat_unlock(l->lock);
        if (claimed) finish(s, l, true);
    }
    ...
    if (failed && !observed && !is_main) report_unobserved(vm, p);
```

The dead process's files and sockets are closed, and its registered names
released, *before* anyone hears of it, so a supervisor that restarts it can
open the same port at once. A failure nobody watches is printed, as in the
`lost.masm` run above. With `spawnm` instead of `spawn`, the same failure
arrives as a message. `watched.masm` is the same child with this `main`
(and `.include "std/io.inc"` for the printing macros):

```asm
.func main
    spawnm child (PID, 0) -> (I9)
    recv R0                            ; a Down, not a number
    PRINT_S "child failed: "
    PRINTLN_S R0->Down.code
    ret 0
.endf
```

```sh
$ ./mlwsvm run watched.masm
child failed: E_DIV_ZERO
```

A **supervisor** is a process whose job is watching others and restarting
them. [lib/std/supervisor.masm](../../lib/std/supervisor.masm) is one,
written in ordinary MLWSVM assembly. It starts each child with `spawnim`
(spawn a closure, monitored), then loops on `recv`. When a `Down` with an
error code arrives, it counts the failure and restarts the child, one
child (one-for-one) or all of them (one-for-all):

```asm
failed:
    ncall "time.now_ns" () -> (I9)
    call count_restart (R5, I9, period_ms) -> (I10)
    bgt I10, max_restarts, give_up
    beq strategy, ONE_FOR_ALL, all
    call start_child (children, I8) -> (I6)
    mov R4[I8], I6
    jmp loop
```

If more than `max_restarts` failures happen within `period_ms`, restarting
is not helping, so the supervisor kills its children and fails itself with
`E_RESTARTS`, for the supervisor above it to notice. That is Erlang/OTP's
*restart intensity*. [examples/workers.masm](../../examples/workers.masm)
uses `spawnm` by hand to run a pool of workers and report the one that
crashes.

## I/O without blocking a worker

A worker thread that calls a blocking `read` stops every process queued on
it until the read returns. With one worker per core and thousands of
connections, that would be fatal. There are two standard answers, and
MLWSVM uses both, as Go's runtime and libuv do:

* For pipes and sockets, make the descriptor non-blocking, and when a read
  would block, park the process and ask the operating system to say when
  the descriptor is ready (kqueue on macOS and the BSDs, epoll on Linux).
  One *poller* thread waits for all of them.
* For things that cannot be polled (regular files, running a command, a C
  library call that blocks), run the call on a pool of I/O threads while the
  process waits.

A native that may wait does not block. It records what it needs and
returns. In [src/native/osio.c](../../src/native/osio.c), `fill` reads into
a file handle's buffer:

```c
    long r = plat_fd_read(h->fd, h->rbuf + h->rlen, h->rcap - h->rlen, &err);
    if (r == PLAT_IO_AGAIN) {
        native_wait_fd(p, h->fd, PLAT_POLL_READ, h->timeout_ms);
        return -1;
    }
```

`native_wait_fd` ([src/native/extapi.c](../../src/native/extapi.c)) sets
`p->native_req = NREQ_WAIT_FD` and the descriptor. The `ncall` handler sees
the request and does what `recv` does: back up and end the slice.

```c
                    if (req == NREQ_WAIT_FD) { /* wait for the descriptor; then this instruction runs again */
                        p->resumed = true;
                        fr->pc--;
                        p->insns--;
                        return SLICE_IOWAIT;
                    }
```

`after_slice` hands the process to the poller; when the descriptor is
ready, the poller thread makes it ready again, and the `ncall` runs once
more with `p->resumed` set, so the native continues where it stopped.
Natives that need a thread ask for `NREQ_BLOCKING` instead and go to the
I/O pool, which grows up to 64 threads. Both kinds of waiting are counted
(`io_waiting`, `io_inflight`), which is what keeps a server waiting for
connections from being taken for a deadlock. A process waiting on a socket
holds no thread, so 10,000 open connections are 10,000 small processes
([DESIGN.md §15](../DESIGN.md#15-input-and-output)).

## Worked example: one thread and several

The one-thread case is worth knowing well, because it is deterministic:
with `--threads 1`, scheduling is plain round robin, and the same program
gives the same output every time. With more threads, the only order is
what messages impose. This program starts three workers that each print
three lines, with a loop of about 3,000 instructions between lines, more
than one slice:

```asm
; Three processes each print three lines, with a busy loop between them
; that is longer than one time slice.
.include "std/io.inc"

.func worker(i64 id)
    mov I1, 1                          ; step
1:  PRINTLN "worker", id, I1
    mov I2, 0
2:  inc I2                             ; about 3,000 instructions:
    blt I2, 1500, 2b                   ; more than one 2,000-instruction slice
    inc I1
    ble I1, 3, 1b
    ret
.endf

.func main
    mov I0, 1
1:  spawnm worker (I0) -> (I1)
    inc I0
    ble I0, 3, 1b
    mov I0, 0
2:  recv R0                            ; a Down for each worker
    inc I0
    blt I0, 3, 2b
    ret 0
.endf
```

On one thread, each worker prints a line and is preempted in the middle
of its loop, so the lines take turns, the same way on every run:

```sh
$ ./mlwsvm run --threads 1 interleave.masm
worker 1 1
worker 2 1
worker 3 1
worker 1 2
worker 2 2
worker 3 2
worker 1 3
worker 2 3
worker 3 3
```

With `--slice 100000`, longer than a worker's whole run, there is no
preemption: worker 1 prints its three lines, then worker 2, then worker 3.

On four threads the workers run at the same time, and four runs on the M5
Pro gave four orders (each run's lines shown on one line here):

```text
worker 1 1|worker 3 1|worker 2 1|worker 3 2|worker 1 2|worker 3 3|worker 1 3|worker 2 2|worker 2 3|
worker 1 1|worker 2 1|worker 3 1|worker 1 2|worker 3 2|worker 1 3|worker 2 2|worker 3 3|worker 2 3|
worker 1 1|worker 2 1|worker 1 2|worker 3 1|worker 1 3|worker 2 2|worker 3 2|worker 2 3|worker 3 3|
worker 1 1|worker 3 1|worker 3 2|worker 1 2|worker 2 1|worker 3 3|worker 1 3|worker 2 2|worker 2 3|
```

Each worker's own lines stay in order, and no line is ever cut by another:
the VM gathers a process's output until a newline and writes the whole line
at once ([ABI.md §1.1](../ABI.md#11-scheduling)). That is why the test suite
also runs on one thread (`MLWSVM_THREADS=1`), and why a test whose output
depends on the order of independent processes is wrong.

To watch processes while they run, start a program with `--monitor` and
point `mlwsvm top` at it ([TOOLING.md](../TOOLING.md#terminal-view-mlwsvm-top)).
Here `busy.masm` spawns two processes that spin in `1: jmp 1b` forever
and one that waits in `recv`, then its `main` sleeps for two seconds:

```sh
$ ./mlwsvm run --threads 4 --monitor 127.0.0.1:7171 busy.masm &
$ ./mlwsvm top 127.0.0.1:7171 --once
MLWSVM  up 1.0s  processes 4 (max 4, spawned 4)  insns 4.0G  heap 0B

   PID STATE     %CPU    INSNS     HEAP      RAW   STACK  MBOX MSGS IN/OUT   GC  FUNCTION
     2 ready     99.9     2.0G        0        0       0     0         0/0    0  busy.spinner+0 (busy.masm:3)
     3 ready     99.8     2.0G        0        0       0     0         0/0    0  busy.spinner+0 (busy.masm:3)
     1 sleeping   0.0        4        0        0       0     0         0/0    0  busy.main+4 (busy.masm:16)
     4 waiting    0.0        0        0        0       0     0         0/0    0  busy.waiter+0 (busy.masm:7)
```

The two spinners each use a whole core, on two different workers, while
the sleeping and waiting processes use none. They show as `ready` rather
than `running` because the monitor stops the other workers at the end of a
slice before it reads the table
([DESIGN.md §13.7](../DESIGN.md#137-stopping-the-world)).

## Where to look

| What | Where |
|---|---|
| The process | `struct Process` in [src/vm/vm.h](../../src/vm/vm.h); `proc_new`, `proc_start`, `proc_run_slice` in [src/vm/process.c](../../src/vm/process.c) |
| Spawn, send, receive | `OP_SPAWN`, `OP_SEND`, `OP_RECV` in [src/vm/generic.inc](../../src/vm/generic.inc); `mbox_take`, `spawn_watch` in [src/vm/interp.c](../../src/vm/interp.c) |
| The deep copy | `msg_copy`, `shallow` in [src/heap/copy.c](../../src/heap/copy.c); `gc_adopt` in [src/heap/gc.c](../../src/heap/gc.c) |
| Slices | `NEXT`, `h_slice`, `run_loop` in [src/vm/interp.c](../../src/vm/interp.c) |
| The scheduler | `worker_loop`, `after_slice`, `take`, `steal`, `ready_locked`, `idle`, `timers_service`, `sched_send`, `finish` in [src/vm/sched.c](../../src/vm/sched.c) |
| Waiting for I/O | `native_wait_fd` in [src/native/extapi.c](../../src/native/extapi.c); `OP_NCALL` in generic.inc; `wait_for_fd`, `poll_main`, `io_main` in sched.c |
| Supervisors | [lib/std/supervisor.masm](../../lib/std/supervisor.masm); [examples/workers.masm](../../examples/workers.masm) |
| Tests | [tests/processes/](../../tests/processes/), [tests/threads/](../../tests/threads/) (`; threads: 4`) |
| Reference | [DESIGN.md §13](../DESIGN.md#13-processes-and-the-scheduler), [§14](../DESIGN.md#14-messages) and [§15](../DESIGN.md#15-input-and-output); [ABI.md §1](../ABI.md#1-processes); [PERFORMANCE.md](../PERFORMANCE.md#the-multi-threaded-scheduler); [TARGETING.md §9](../TARGETING.md#9-concurrency) |

Previous: [6. Calls, frames and exceptions](06-calls-frames-exceptions.md) · Next: [8. JIT 1: what a JIT is](08-jit-what.md)
