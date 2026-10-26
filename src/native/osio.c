/* osio.c - std.fs and std.os: files, directories, the program's arguments and
 * environment, and child processes. See docs/STDLIB.md.
 *
 * These natives are "checked" (NF_CHECKED): they report errors with
 * native_fail and never trap, so they can run on the I/O thread pool. Where an
 * operation would block, they ask for what they need and return:
 *   - a pipe or socket we own is non-blocking; when it is not ready the native
 *     asks to wait for it (native_wait_fd) and runs again when the poller says
 *     it is ready. The process waits like `recv`: no thread is held.
 *   - regular files, and descriptors we did not create (stdin), cannot be
 *     waited for that way, so the native asks to run on the I/O thread pool
 *     (native_run_blocking), where it may block.
 * Every one counts towards the OS-call quota (kind 5), so a sandbox can be
 * denied the file system, commands and the environment. */
#include "../../include/mlwsvm/mlwsvm.h"
#include "../heap/heap.h"
#include "../platform/platform.h"

#define MAX_READ (16u << 20) /* the most one fs.read returns */
#define CHUNK 65536

/* ---- file handles ------------------------------------------------------------------ */

enum {
    H_POOL = 1,   /* a regular file or an inherited descriptor: blocking, on the I/O pool */
    H_STREAM = 2, /* a pipe or socket we made: non-blocking, waited for with the poller */
    H_STDOUT = 3, /* the program's output (through the VM, so tests and tools see it) */
    H_STDERR = 4,
};

typedef struct {
    int fd;
    int kind;
    bool owned; /* close the descriptor when the handle goes */
    bool eof;
    int64_t timeout_ms;      /* for waits on streams; -1 = none */
    uint8_t *rbuf;           /* input read ahead (read_line, read_all) */
    size_t rpos, rlen, rcap;
    const void *wobj;        /* a write in progress across waits: the data, and how much is done */
    size_t wdone;
} FileH;

static void file_finalize(void *ptr) {
    FileH *h = ptr;
    if (h->owned && h->fd >= 0) plat_fd_close(h->fd);
    free(h->rbuf);
    free(h);
}

static const MlwsForeignType FILE_T = {"fs.file", file_finalize};

static Obj *new_file(Process *p, int fd, int kind, bool owned) {
    FileH *h = xcalloc(1, sizeof *h);
    h->fd = fd;
    h->kind = kind;
    h->owned = owned;
    h->timeout_ms = -1;
    if (kind == H_STREAM) plat_fd_set_nonblocking(fd, true);
    return &foreign_new_obj(p, &FILE_T, h, NULL)->h;
}

static FileH *get_file(Process *p, Word v) {
    ObjForeign *o = v.r;
    if (!o || o->h.kind != OBJ_FOREIGN || o->type != &FILE_T) {
        native_fail(p, "E_TYPE", "%s: expected a file handle", p->cur_native->name);
        return NULL;
    }
    if (!o->ptr) native_fail(p, "E_CLOSED", "%s: the file is closed", p->cur_native->name);
    return o->ptr;
}

/* A string argument, NUL-terminated (strings are; byte buffers are not accepted). */
static const char *get_str(Process *p, Word v, const char *what) {
    Obj *o = v.r;
    if (o && o->kind == OBJ_STRING) return ((ObjString *)o)->data;
    native_fail(p, "E_TYPE", "%s: %s must be a string", p->cur_native->name, what);
    return NULL;
}

static bool get_data(Process *p, Word v, const uint8_t **data, size_t *n) {
    Obj *o = v.r;
    if (o && o->kind == OBJ_STRING) {
        *data = (const uint8_t *)((ObjString *)o)->data;
        *n = ((ObjString *)o)->len;
        return true;
    }
    if (o && o->kind == OBJ_BYTES) {
        *data = ((ObjBytes *)o)->data;
        *n = ((ObjBytes *)o)->len;
        return true;
    }
    native_fail(p, "E_TYPE", "%s: expected a string or byte buffer", p->cur_native->name);
    return false;
}

static void fail_os(Process *p, int err, const char *what) {
    native_fail(p, plat_err_code(err), "%s: %s: %s", p->cur_native->name, what, plat_err_text(err));
}

/* Before blocking work: on a worker thread, move to the I/O pool (returns false). */
static bool on_pool(Process *p) {
    if (p->on_io_thread) return true;
    native_run_blocking(p);
    return false;
}

/* After a wait on a stream: did it time out? */
static bool timed_out(Process *p, FileH *h) {
    if (!p->resumed || !p->fdwait.timed_out) return false;
    p->fdwait.timed_out = false;
    h->wobj = NULL;
    h->wdone = 0;
    native_fail(p, "E_TIMEOUT", "%s: timed out after %lld ms", p->cur_native->name, (long long)h->timeout_ms);
    return true;
}

static ObjBytes *bytes_of(Process *p, const uint8_t *data, size_t n) {
    ObjBytes *b = bytes_new(p, (int64_t)n);
    if (n) memcpy(b->data, data, n);
    return b;
}

/* Read more input into h->rbuf. Returns 1 (got some), 0 (end of input), -1
 * (waiting, or failed: the native returns). */
static int fill(Process *p, FileH *h) {
    if (h->eof) return 0;
    if (h->rpos == h->rlen) h->rpos = h->rlen = 0;
    if (h->rcap - h->rlen < CHUNK) {
        if (h->rpos) { /* shift the unread part down first */
            memmove(h->rbuf, h->rbuf + h->rpos, h->rlen - h->rpos);
            h->rlen -= h->rpos;
            h->rpos = 0;
        }
        if (h->rcap - h->rlen < CHUNK) {
            h->rcap = h->rlen + CHUNK * 2;
            h->rbuf = xrealloc(h->rbuf, h->rcap);
        }
    }
    int err = 0;
    long r = plat_fd_read(h->fd, h->rbuf + h->rlen, h->rcap - h->rlen, &err);
    if (r == PLAT_IO_AGAIN) {
        native_wait_fd(p, h->fd, PLAT_POLL_READ, h->timeout_ms);
        return -1;
    }
    if (r < 0) {
        fail_os(p, err, "read");
        return -1;
    }
    if (r == 0) {
        h->eof = true;
        return 0;
    }
    h->rlen += (size_t)r;
    return 1;
}

/* Common start of a read: the handle, and whether this thread may do it now. */
static FileH *reader(Process *p, Word v) {
    FileH *h = get_file(p, v);
    if (!h) return NULL;
    if (h->kind == H_STDOUT || h->kind == H_STDERR) {
        native_fail(p, "E_TYPE", "%s: stdout and stderr cannot be read", p->cur_native->name);
        return NULL;
    }
    if (h->kind == H_POOL && !on_pool(p)) return NULL;
    if (h->kind == H_STREAM && timed_out(p, h)) return NULL;
    return h;
}

/* ---- std.fs: handles ------------------------------------------------------------------ */

static int open_mode(const char *m) {
    int mode = 0;
    bool plus = strchr(m, '+') != NULL;
    switch (m[0]) {
    case 'r': mode = plus ? PLAT_O_RDWR : PLAT_O_READ; break;
    case 'w': mode = (plus ? PLAT_O_RDWR : PLAT_O_WRITE) | PLAT_O_CREATE | PLAT_O_TRUNCATE; break;
    case 'a': mode = (plus ? PLAT_O_RDWR : PLAT_O_WRITE) | PLAT_O_CREATE | PLAT_O_APPEND; break;
    default: return -1;
    }
    for (const char *c = m + 1; *c; c++) {
        if (*c == 'x' && m[0] != 'r') mode |= PLAT_O_EXCLUSIVE;
        else if (*c != '+') return -1;
    }
    return mode;
}

static void fs_open(Process *p, Word *a, Word *r) {
    const char *path = get_str(p, a[0], "the path"), *m = get_str(p, a[1], "the mode");
    if (!path || !m) return;
    int mode = open_mode(m);
    if (mode < 0) {
        native_fail(p, "E_TYPE", "fs.open: bad mode \"%s\" (use r, w, a, r+, w+, a+; x = must not exist)", m);
        return;
    }
    int err = 0, fd = plat_file_open(path, mode, &err);
    if (fd < 0) {
        fail_os(p, err, path);
        return;
    }
    r[0].r = new_file(p, fd, H_POOL, true);
}

static void fs_close(Process *p, Word *a, Word *r) {
    (void)r;
    if (get_file(p, a[0])) foreign_finalize(a[0].r);
}

static void fs_read(Process *p, Word *a, Word *r) {
    FileH *h = reader(p, a[0]);
    if (!h) return;
    int64_t max = a[1].i;
    if (max <= 0 || max > (int64_t)MAX_READ) max = MAX_READ;
    if (h->rpos == h->rlen && fill(p, h) < 0) return;
    size_t n = h->rlen - h->rpos < (size_t)max ? h->rlen - h->rpos : (size_t)max;
    r[0].r = bytes_of(p, h->rbuf + h->rpos, n); /* empty at the end of input */
    h->rpos += n;
}

static void fs_read_line(Process *p, Word *a, Word *r) {
    FileH *h = reader(p, a[0]);
    if (!h) return;
    for (;;) {
        uint8_t *start = h->rbuf + h->rpos, *nl = h->rlen > h->rpos ? memchr(start, '\n', h->rlen - h->rpos) : NULL;
        if (nl || h->eof) {
            size_t n = nl ? (size_t)(nl - start) : h->rlen - h->rpos;
            if (!nl && !n) {
                r[0].r = NULL; /* the end of input */
                return;
            }
            h->rpos += n + (nl ? 1 : 0);
            if (n && start[n - 1] == '\r') n--;
            r[0].r = str_new(p, (const char *)start, n);
            return;
        }
        if (fill(p, h) < 0) return;
    }
}

static void fs_read_all(Process *p, Word *a, Word *r) {
    FileH *h = reader(p, a[0]);
    if (!h) return;
    for (;;) {
        int got = fill(p, h);
        if (got < 0) return; /* waiting (what was read so far stays in the buffer) */
        if (got == 0) break;
    }
    r[0].r = bytes_of(p, h->rbuf + h->rpos, h->rlen - h->rpos);
    h->rpos = h->rlen = 0;
}

static void fs_write(Process *p, Word *a, Word *r) {
    (void)r;
    FileH *h = get_file(p, a[0]);
    const uint8_t *data;
    size_t n;
    if (!h || !get_data(p, a[1], &data, &n)) return;
    if (h->kind == H_STDOUT) {
        vm_write(p->vm, (const char *)data, n);
        return;
    }
    if (h->kind == H_STDERR) {
        plat_lock(p->vm->out_lock);
        if (p->vm->err_out) p->vm->err_out(p->vm->err_ctx, (const char *)data, n);
        else fwrite(data, 1, n, stderr);
        plat_unlock(p->vm->out_lock);
        return;
    }
    if (h->kind == H_POOL && !on_pool(p)) return;
    if (h->kind == H_STREAM && timed_out(p, h)) return;
    size_t done = h->wobj == a[1].r ? h->wdone : 0; /* continuing after a wait */
    while (done < n) {
        int err = 0;
        long w = plat_fd_write(h->fd, data + done, n - done, &err);
        if (w == PLAT_IO_AGAIN) {
            h->wobj = a[1].r;
            h->wdone = done;
            native_wait_fd(p, h->fd, PLAT_POLL_WRITE, h->timeout_ms);
            return;
        }
        if (w < 0) {
            h->wobj = NULL;
            fail_os(p, err, "write");
            return;
        }
        done += (size_t)w;
    }
    h->wobj = NULL;
    h->wdone = 0;
}

static void fs_seek(Process *p, Word *a, Word *r) {
    FileH *h = get_file(p, a[0]);
    if (!h) return;
    if (h->kind != H_POOL) {
        native_fail(p, "E_TYPE", "fs.seek: only files can seek");
        return;
    }
    int err = 0;
    int64_t whence = a[2].i, off = a[1].i;
    if (whence == 1) off -= (int64_t)(h->rlen - h->rpos); /* relative to what the program has read */
    h->rpos = h->rlen = 0;
    h->eof = false;
    int64_t at = plat_fd_seek(h->fd, off, (int)whence, &err);
    if (at < 0) fail_os(p, err, "seek");
    else r[0].i = at;
}

static void fs_set_timeout(Process *p, Word *a, Word *r) {
    (void)r;
    FileH *h = get_file(p, a[0]);
    if (h) h->timeout_ms = a[1].i < 0 ? -1 : a[1].i;
}

static void fs_stdin(Process *p, Word *a, Word *r) {
    (void)a;
    r[0].r = new_file(p, 0, H_POOL, false); /* inherited: never made non-blocking */
}
static void fs_stdout(Process *p, Word *a, Word *r) {
    (void)a;
    r[0].r = new_file(p, 1, H_STDOUT, false);
}
static void fs_stderr(Process *p, Word *a, Word *r) {
    (void)a;
    r[0].r = new_file(p, 2, H_STDERR, false);
}

/* ---- std.fs: whole files and paths (on the I/O pool) ------------------------------------ */

static void fs_read_file(Process *p, Word *a, Word *r) {
    const char *path = get_str(p, a[0], "the path");
    if (!path) return;
    int err = 0, fd = plat_file_open(path, PLAT_O_READ, &err);
    if (fd < 0) {
        fail_os(p, err, path);
        return;
    }
    ObjBytes *b = bytes_new(p, 0);
    uint8_t buf[CHUNK];
    for (;;) {
        long n = plat_fd_read(fd, buf, sizeof buf, &err);
        if (n < 0) {
            fail_os(p, err, path);
            break;
        }
        if (n == 0) break;
        bytes_append(p, b, buf, (size_t)n);
    }
    plat_fd_close(fd);
    r[0].r = b;
}

static void fs_write_file(Process *p, Word *a, Word *r) {
    (void)r;
    const char *path = get_str(p, a[0], "the path");
    const uint8_t *data;
    size_t n;
    if (!path || !get_data(p, a[1], &data, &n)) return;
    int err = 0, fd = plat_file_open(path, PLAT_O_WRITE | PLAT_O_CREATE | PLAT_O_TRUNCATE, &err);
    if (fd < 0) {
        fail_os(p, err, path);
        return;
    }
    for (size_t done = 0; done < n;) {
        long w = plat_fd_write(fd, data + done, n - done, &err);
        if (w < 0) {
            fail_os(p, err, path);
            break;
        }
        done += (size_t)w;
    }
    if (!plat_fd_close(fd)) fail_os(p, plat_last_error(), path);
}

static void fs_stat(Process *p, Word *a, Word *r) {
    const char *path = get_str(p, a[0], "the path");
    if (!path) return;
    PlatStat st;
    int err = 0;
    if (!plat_stat(path, true, &st, &err)) {
        fail_os(p, err, path);
        return;
    }
    ObjStruct *s = struct_new(p, &VM_BUILTIN_DEFS[BUILTIN_FILEINFO]);
    s->fields[FIF_SIZE].i = st.size;
    s->fields[FIF_MTIME].i = st.mtime_ns;
    s->fields[FIF_MODE].i = st.mode;
    s->fields[FIF_KIND].i = st.kind;
    r[0].r = s;
}

static void fs_exists(Process *p, Word *a, Word *r) {
    const char *path = get_str(p, a[0], "the path");
    PlatStat st;
    int err = 0;
    if (path) r[0].i = plat_stat(path, true, &st, &err);
}

static int cmp_names(const void *x, const void *y) { return strcmp(*(char *const *)x, *(char *const *)y); }

static void fs_list(Process *p, Word *a, Word *r) {
    const char *path = get_str(p, a[0], "the path");
    if (!path) return;
    char **names;
    size_t n;
    if (!plat_list_dir(path, &names, &n)) {
        PlatStat st;
        int err = 0;
        if (!plat_stat(path, true, &st, &err)) fail_os(p, err, path);
        else native_fail(p, "E_NOT_DIR", "fs.list: %s is not a directory", path);
        return;
    }
    qsort(names, n, sizeof *names, cmp_names);
    ObjList *l = list_new(p, (int64_t)n);
    for (size_t i = 0; i < n; i++) {
        Word w = {.r = str_new(p, names[i], strlen(names[i]))};
        list_push(p, l, w, T_R);
        free(names[i]);
    }
    free(names);
    r[0].r = l;
}

#define PATH_OP(NAME, CALL)                                                    \
    static void NAME(Process *p, Word *a, Word *r) {                           \
        (void)r;                                                               \
        const char *path = get_str(p, a[0], "the path");                       \
        int err = 0;                                                           \
        if (path && !(CALL)) fail_os(p, err, path);                            \
    }
PATH_OP(fs_mkdir, plat_mkdir_err(path, &err))
PATH_OP(fs_remove, plat_remove(path, &err))
PATH_OP(fs_rmdir, plat_rmdir(path, &err))

static void fs_rename(Process *p, Word *a, Word *r) {
    (void)r;
    const char *from = get_str(p, a[0], "the old path"), *to = get_str(p, a[1], "the new path");
    int err = 0;
    if (from && to && !plat_rename(from, to, &err)) fail_os(p, err, from);
}

static void fs_cwd(Process *p, Word *a, Word *r) {
    (void)a;
    char *d = plat_getcwd();
    if (!d) {
        native_fail(p, "E_IO", "fs.cwd: the current directory is not available");
        return;
    }
    r[0].r = str_new(p, d, strlen(d));
    free(d);
}

/* ---- std.os ------------------------------------------------------------------------------ */

static void os_args(Process *p, Word *a, Word *r) {
    (void)a;
    const StrVec *args = &p->vm->prog_args;
    ObjList *l = list_new(p, (int64_t)args->n);
    for (size_t i = 0; i < args->n; i++) {
        Word w = {.r = str_new(p, args->v[i], strlen(args->v[i]))};
        list_push(p, l, w, T_R);
    }
    r[0].r = l;
}

static void os_env(Process *p, Word *a, Word *r) {
    const char *name = get_str(p, a[0], "the name");
    if (!name) return;
    const char *v = getenv(name);
    r[0].r = v ? str_new(p, v, strlen(v)) : NULL;
}

static void os_environ(Process *p, Word *a, Word *r) {
    (void)a;
    ObjMap *m = map_new(p);
    for (char *const *e = plat_environ(); e && *e; e++) {
        const char *eq = strchr(*e, '=');
        if (!eq || eq == *e) continue;
        Word k = {.r = str_new(p, *e, (size_t)(eq - *e))}, v = {.r = str_new(p, eq + 1, strlen(eq + 1))};
        map_set(p, m, k, T_R, v, T_R);
    }
    r[0].r = m;
}

static void os_exit(Process *p, Word *a, Word *r) {
    (void)r;
    sched_request_exit(p->vm, a[0].i);
    p->native_req = NREQ_EXIT;
}

static void os_platform(Process *p, Word *a, Word *r) {
    (void)a;
    const char *n = plat_os_name();
    r[0].r = str_new(p, n, strlen(n));
}

/* Child processes. */
typedef struct {
    int64_t pid;
    bool waited;
    int code;
} ProcH;

static void proc_finalize(void *ptr) { free(ptr); } /* an unwaited child is left to the OS */

static const MlwsForeignType PROC_T = {"os.process", proc_finalize};

/* argv from a list of strings (NULL-terminated; the strings belong to the list). */
static char **get_argv(Process *p, Word v) {
    ObjList *l = v.r;
    if (!l || l->h.kind != OBJ_LIST || l->len == 0) {
        native_fail(p, "E_TYPE", "%s: expected a non-empty list of strings (the command and its arguments)",
                    p->cur_native->name);
        return NULL;
    }
    char **argv = xcalloc(l->len + 1, sizeof *argv);
    for (uint32_t i = 0; i < l->len; i++) {
        Obj *o = l->tags[i] == T_R ? l->items[i].r : NULL;
        if (!o || o->kind != OBJ_STRING) {
            native_fail(p, "E_TYPE", "%s: argument %u is not a string", p->cur_native->name, i);
            free(argv);
            return NULL;
        }
        argv[i] = ((ObjString *)o)->data;
    }
    return argv;
}

static ProcH *get_proc(Process *p, Word v) {
    ObjForeign *o = v.r;
    if (!o || o->h.kind != OBJ_FOREIGN || o->type != &PROC_T || !o->ptr) {
        native_fail(p, "E_TYPE", "%s: expected a process handle (from os.spawn)", p->cur_native->name);
        return NULL;
    }
    return o->ptr;
}

/* os.spawn(argv, pipes) -> (process, stdin, stdout, stderr): pipes is a mask of
 * 1 stdin, 2 stdout, 4 stderr, 8 stderr into stdout; unpiped streams are
 * inherited and come back null. */
static void os_spawn(Process *p, Word *a, Word *r) {
    char **argv = get_argv(p, a[0]);
    if (!argv) return;
    PlatChild c;
    int err = 0;
    if (!plat_spawn(argv, (int)(a[1].i & 15), &c, &err)) {
        fail_os(p, err, argv[0]);
        free(argv);
        return;
    }
    free(argv);
    ProcH *h = xcalloc(1, sizeof *h);
    h->pid = c.pid;
    r[0].r = foreign_new_obj(p, &PROC_T, h, NULL);
    int fds[3] = {c.in_fd, c.out_fd, c.err_fd};
    for (int k = 0; k < 3; k++) r[1 + k].r = fds[k] >= 0 ? new_file(p, fds[k], H_STREAM, true) : NULL;
}

static void os_wait(Process *p, Word *a, Word *r) {
    ProcH *h = get_proc(p, a[0]);
    if (!h) return;
    if (!h->waited) {
        if (!on_pool(p)) return;
        int err = 0;
        if (!plat_wait_child(h->pid, &h->code, &err)) {
            fail_os(p, err, "wait");
            return;
        }
        h->waited = true;
    }
    r[0].i = h->code;
}

static void os_kill(Process *p, Word *a, Word *r) {
    (void)r;
    ProcH *h = get_proc(p, a[0]);
    if (h && !h->waited) plat_signal_child(h->pid, a[1].i ? 9 : 15);
}

/* os.run(argv) -> (exit code, output): run a command to completion, capturing
 * its stdout (stderr is inherited). */
static void os_run(Process *p, Word *a, Word *r) {
    char **argv = get_argv(p, a[0]);
    if (!argv) return;
    PlatChild c;
    int err = 0;
    if (!plat_spawn(argv, PLAT_PIPE_OUT, &c, &err)) {
        fail_os(p, err, argv[0]);
        free(argv);
        return;
    }
    free(argv);
    ObjBytes *out = bytes_new(p, 0);
    uint8_t buf[CHUNK];
    for (;;) {
        long n = plat_fd_read(c.out_fd, buf, sizeof buf, &err);
        if (n <= 0) break;
        bytes_append(p, out, buf, (size_t)n);
    }
    plat_fd_close(c.out_fd);
    int code = 255;
    if (!plat_wait_child(c.pid, &code, &err)) fail_os(p, err, "wait");
    r[0].i = code;
    r[1].r = out;
}

/* ---- the table ---------------------------------------------------------------------------- */

#define F (NF_CHECKED | NF_OS)
#define B (NF_CHECKED | NF_OS | NF_BLOCKING)
#define N(name, np, pt, nr, rt, fn, flags, doc) {name, np, pt, nr, rt, fn, doc, flags, NULL, NULL, NULL}
#define T(...) {__VA_ARGS__}

static const NativeDef OS_NATIVES[] = {
    N("fs.open", 2, T(T_R, T_R), 1, T(T_R), fs_open, B,
      "Open a file: mode \"r\", \"w\" (create or truncate), \"a\" (append), \"r+\", \"w+\", \"a+\"; add \"x\" to fail if it exists."),
    N("fs.close", 1, T(T_R), 0, T(0), fs_close, F, "Close a file now (it is also closed when collected)."),
    N("fs.read", 2, T(T_R, T_I64), 1, T(T_R), fs_read, F,
      "Read up to n bytes (n <= 0: up to 16 MiB), as a byte buffer; empty at the end of input."),
    N("fs.read_line", 1, T(T_R), 1, T(T_R), fs_read_line, F,
      "Read a line, without its \\n (or \\r\\n), as a string; null at the end of input."),
    N("fs.read_all", 1, T(T_R), 1, T(T_R), fs_read_all, F, "Read everything up to the end of input, as a byte buffer."),
    N("fs.write", 2, T(T_R, T_R), 0, T(0), fs_write, F, "Write all of a string or byte buffer."),
    N("fs.seek", 3, T(T_R, T_I64, T_I64), 1, T(T_I64), fs_seek, F,
      "Move in a file: whence 0 from the start, 1 from here, 2 from the end; returns the new offset."),
    N("fs.set_timeout", 2, T(T_R, T_I64), 0, T(0), fs_set_timeout, F,
      "Give later reads and writes on a pipe or socket a timeout in ms (E_TIMEOUT); negative: none."),
    N("fs.stdin", 0, T(0), 1, T(T_R), fs_stdin, F, "The program's standard input."),
    N("fs.stdout", 0, T(0), 1, T(T_R), fs_stdout, F, "The program's standard output (the same output as io.print)."),
    N("fs.stderr", 0, T(0), 1, T(T_R), fs_stderr, F, "The program's standard error."),
    N("fs.read_file", 1, T(T_R), 1, T(T_R), fs_read_file, B, "A whole file, as a byte buffer."),
    N("fs.write_file", 2, T(T_R, T_R), 0, T(0), fs_write_file, B,
      "Create or replace a file with a string or byte buffer."),
    N("fs.stat", 1, T(T_R), 1, T(T_R), fs_stat, B,
      "A FileInfo for a path: size, mtime (ns), mode, kind (1 file, 2 directory, 3 link, 4 other)."),
    N("fs.exists", 1, T(T_R), 1, T(T_B), fs_exists, B, "Whether a path exists."),
    N("fs.list", 1, T(T_R), 1, T(T_R), fs_list, B, "The names in a directory, sorted, as a list of strings."),
    N("fs.mkdir", 1, T(T_R), 0, T(0), fs_mkdir, B, "Create a directory."),
    N("fs.remove", 1, T(T_R), 0, T(0), fs_remove, B, "Delete a file."),
    N("fs.rmdir", 1, T(T_R), 0, T(0), fs_rmdir, B, "Delete an empty directory."),
    N("fs.rename", 2, T(T_R, T_R), 0, T(0), fs_rename, B, "Rename or move a file or directory."),
    N("fs.cwd", 0, T(0), 1, T(T_R), fs_cwd, F, "The current directory."),
    N("os.args", 0, T(0), 1, T(T_R), os_args, F,
      "The program's arguments (after the program file on the command line), as a list of strings."),
    N("os.env", 1, T(T_R), 1, T(T_R), os_env, F, "An environment variable, or null."),
    N("os.environ", 0, T(0), 1, T(T_R), os_environ, F, "The whole environment, as a map of strings."),
    N("os.exit", 1, T(T_I64), 0, T(0), os_exit, F,
      "End the whole program now, from any process, with this exit code."),
    N("os.platform", 0, T(0), 1, T(T_R), os_platform, F, "\"macos\", \"linux\", \"freebsd\", ... or \"windows\"."),
    N("os.spawn", 2, T(T_R, T_I64), 4, T(T_R, T_R, T_R, T_R), os_spawn, F,
      "Start a command (a list: the program, found on PATH, then its arguments). Pipes: 1 stdin, 2 stdout, "
      "4 stderr, 8 stderr into stdout. Returns the process and its piped streams (null if not piped)."),
    N("os.wait", 1, T(T_R), 1, T(T_I64), os_wait, F,
      "Wait for a command to finish; its exit code (128 + N if killed by signal N)."),
    N("os.kill", 2, T(T_R, T_B), 0, T(0), os_kill, F, "Stop a command: ask it to terminate, or kill it (true)."),
    N("os.run", 1, T(T_R), 2, T(T_I64, T_R), os_run, B,
      "Run a command to completion: its exit code and its standard output, as a byte buffer."),
};

#undef F
#undef B
#undef N
#undef T

void natives_register_os(VM *vm) {
    for (size_t i = 0; i < sizeof OS_NATIVES / sizeof OS_NATIVES[0]; i++) vm_register_native(vm, &OS_NATIVES[i]);
}

const NativeDef *natives_os_table(size_t *n) {
    *n = sizeof OS_NATIVES / sizeof OS_NATIVES[0];
    return OS_NATIVES;
}
