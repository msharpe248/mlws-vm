/* posix.c - platform layer for POSIX systems (Linux, macOS, BSDs). */
#ifndef _WIN32
/* FreeBSD's headers hide their own interfaces (MAP_ANON, _SC_NPROCESSORS_ONLN)
 * when _POSIX_C_SOURCE is defined, and show POSIX's without it. */
#if !defined(__FreeBSD__) && !defined(__DragonFly__)
#define _POSIX_C_SOURCE 200809L
#endif
#define _DARWIN_C_SOURCE
#define _NETBSD_SOURCE
#ifdef __linux__
#define _GNU_SOURCE /* RTLD_DEFAULT */
#endif
#include "platform.h"

#include <arpa/inet.h>
#include <dlfcn.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#ifdef __APPLE__
#include <os/lock.h>
#endif
#include <poll.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <limits.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#if defined(__FreeBSD__) || defined(__DragonFly__) || defined(__NetBSD__)
#include <sys/sysctl.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#if defined(__APPLE__) && defined(__aarch64__)
#include <libkern/OSCacheControl.h>
#define CODE_MAP_JIT 1 /* Apple silicon: MAP_JIT memory, writable or executable per thread */
#endif

char *plat_realpath(const char *path) {
    char buf[PATH_MAX];
    if (!realpath(path, buf)) return NULL;
    return xstrdup(buf);
}

int64_t plat_mtime(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (int64_t)st.st_mtime;
}

bool plat_is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool plat_list_dir(const char *path, char ***names, size_t *n) {
    DIR *d = opendir(path);
    if (!d) return false;
    VEC(char *) v = {0};
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        VEC_PUSH(v, xstrdup(e->d_name));
    }
    closedir(d);
    *names = v.v;
    *n = v.n;
    return true;
}

int64_t plat_wall_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}

int64_t plat_local_offset_s(int64_t secs) {
    time_t t = (time_t)secs;
    struct tm tm;
    if (!localtime_r(&t, &tm)) return 0;
    return (int64_t)tm.tm_gmtoff;
}

uint64_t plat_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void plat_sleep_ns(uint64_t ns) {
    struct timespec ts = {(time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull)};
    while (nanosleep(&ts, &ts) != 0) {}
}

/* The executable's path from the system, where it has a call for it. */
static char *exe_path_system(void) {
#ifdef __APPLE__
    char buf[PATH_MAX];
    uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size) == 0) return plat_realpath(buf);
#elif defined(__linux__)
    char buf[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (len > 0) {
        buf[len] = 0;
        return xstrdup(buf);
    }
#elif defined(__FreeBSD__) || defined(__DragonFly__) || defined(__NetBSD__)
#ifdef __NetBSD__
    int mib[4] = {CTL_KERN, KERN_PROC_ARGS, -1, KERN_PROC_PATHNAME};
#else
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1};
#endif
    char buf[PATH_MAX];
    size_t len = sizeof buf;
    if (sysctl(mib, 4, buf, &len, NULL, 0) == 0 && len > 1) return xstrdup(buf);
#endif
    return NULL;
}

/* Elsewhere (OpenBSD) from argv[0], found through $PATH if it has no '/'. main
 * passes it before any thread starts; later calls without it get the same. */
char *plat_exe_path(const char *argv0) {
    static char *from_argv0;
    char *p = exe_path_system();
    if (p) return p;
    if (argv0 && !from_argv0) {
        char *found = strchr(argv0, '/') ? NULL : plat_find_in_path(argv0);
        from_argv0 = plat_realpath(found ? found : argv0);
        free(found);
    }
    return from_argv0 ? xstrdup(from_argv0) : NULL;
}

/* The first executable file named `name` in a directory of $PATH. */
char *plat_find_in_path(const char *name) {
    const char *path = getenv("PATH");
    if (!path || !*name) return NULL;
    size_t nlen = strlen(name);
    for (const char *d = path;; d++) {
        const char *e = strchr(d, ':');
        size_t dlen = e ? (size_t)(e - d) : strlen(d);
        char *f = xmalloc(dlen + nlen + 3);
        if (dlen) memcpy(f, d, dlen), f[dlen] = '/', memcpy(f + dlen + 1, name, nlen + 1);
        else memcpy(f, "./", 2), memcpy(f + 2, name, nlen + 1); /* an empty entry is the current directory */
        struct stat st;
        if (stat(f, &st) == 0 && S_ISREG(st.st_mode) && access(f, X_OK) == 0) return f;
        free(f);
        if (!e) return NULL;
        d = e;
    }
}

char plat_path_sep(void) { return ':'; }

int plat_tcp_listen(const char *host, int port, char *err, size_t errlen) {
    signal(SIGPIPE, SIG_IGN); /* a client that disconnects must not kill the VM */
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) {
        snprintf(err, errlen, "bad IPv4 address '%s'", host);
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(err, errlen, "socket: %s", strerror(errno));
        return -1;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0 || listen(fd, 16) != 0) {
        snprintf(err, errlen, "cannot listen on %s:%d: %s", host, port, strerror(errno));
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    return fd;
}

int plat_tcp_accept(int listen_fd) {
    int c = accept(listen_fd, NULL, NULL);
    if (c < 0) return -1;
    fcntl(c, F_SETFL, fcntl(c, F_GETFL) & ~O_NONBLOCK);
    return c;
}

int plat_tcp_connect(const char *host, int port) {
    signal(SIGPIPE, SIG_IGN);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) return -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

bool plat_wait_readable(int fd, uint64_t timeout_ns) {
    struct pollfd pf = {fd, POLLIN, 0};
    int ms = (int)((timeout_ns + 999999) / 1000000);
    return poll(&pf, 1, ms) > 0;
}

long plat_sock_read(int fd, void *buf, size_t n, uint64_t timeout_ns) {
    if (!plat_wait_readable(fd, timeout_ns)) return -1;
    ssize_t r = recv(fd, buf, n, 0);
    return r < 0 ? -1 : (long)r;
}

bool plat_sock_write_all(int fd, const void *buf, size_t n) {
    const char *p = buf;
    while (n) {
        ssize_t w = send(fd, p, n, 0);
        if (w <= 0) return false;
        p += w;
        n -= (size_t)w;
    }
    return true;
}

void plat_sock_close(int fd) { close(fd); }

bool plat_is_tty(void) { return isatty(1); }

void plat_stdio_init(void) {}

struct PlatThread {
    pthread_t t;
    void (*fn)(void *);
    void *arg;
};
struct PlatMutex {
    pthread_mutex_t m;
};
struct PlatLock {
#ifdef __APPLE__
    os_unfair_lock l;
#else
    pthread_mutex_t m;
#endif
};
struct PlatCond {
    pthread_cond_t c;
};

static void *thread_main(void *x) {
    PlatThread *t = x;
    t->fn(t->arg);
    return NULL;
}

PlatThread *plat_thread_start(void (*fn)(void *), void *arg) {
    PlatThread *t = xcalloc(1, sizeof *t);
    t->fn = fn;
    t->arg = arg;
    if (pthread_create(&t->t, NULL, thread_main, t) != 0) {
        free(t);
        return NULL;
    }
    return t;
}

void plat_thread_join(PlatThread *t) {
    pthread_join(t->t, NULL);
    free(t);
}

PlatMutex *plat_mutex_new(void) {
    PlatMutex *m = xcalloc(1, sizeof *m);
    pthread_mutex_init(&m->m, NULL);
    return m;
}

void plat_mutex_free(PlatMutex *m) {
    if (!m) return;
    pthread_mutex_destroy(&m->m);
    free(m);
}

void plat_mutex_lock(PlatMutex *m) { pthread_mutex_lock(&m->m); }

PlatLock *plat_lock_new(void) {
    PlatLock *l = xcalloc(1, sizeof *l);
#ifdef __APPLE__
    l->l = OS_UNFAIR_LOCK_INIT;
#else
    pthread_mutex_init(&l->m, NULL);
#endif
    return l;
}

void plat_lock_free(PlatLock *l) {
    if (!l) return;
#ifndef __APPLE__
    pthread_mutex_destroy(&l->m);
#endif
    free(l);
}

#ifdef __APPLE__
void plat_lock(PlatLock *l) { os_unfair_lock_lock(&l->l); }
void plat_unlock(PlatLock *l) { os_unfair_lock_unlock(&l->l); }
#else
void plat_lock(PlatLock *l) { pthread_mutex_lock(&l->m); }
void plat_unlock(PlatLock *l) { pthread_mutex_unlock(&l->m); }
#endif
void plat_mutex_unlock(PlatMutex *m) { pthread_mutex_unlock(&m->m); }

PlatCond *plat_cond_new(void) {
    PlatCond *c = xcalloc(1, sizeof *c);
    pthread_cond_init(&c->c, NULL);
    return c;
}

void plat_cond_free(PlatCond *c) {
    if (!c) return;
    pthread_cond_destroy(&c->c);
    free(c);
}

void plat_cond_wait(PlatCond *c, PlatMutex *m, uint64_t timeout_ns) {
    if (!timeout_ns) {
        pthread_cond_wait(&c->c, &m->m);
        return;
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts); /* pthread_cond_timedwait uses the realtime clock */
    uint64_t ns = (uint64_t)ts.tv_nsec + timeout_ns;
    ts.tv_sec += (time_t)(ns / 1000000000ull);
    ts.tv_nsec = (long)(ns % 1000000000ull);
    pthread_cond_timedwait(&c->c, &m->m, &ts);
}

void plat_cond_signal(PlatCond *c) { pthread_cond_signal(&c->c); }
void plat_cond_broadcast(PlatCond *c) { pthread_cond_broadcast(&c->c); }

void plat_thread_detach(PlatThread *t) {
    pthread_detach(t->t);
    /* The PlatThread struct is still read by thread_main; it is leaked deliberately. */
}

void *plat_dlopen(const char *path, char *err, size_t errlen) {
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h && err) snprintf(err, errlen, "%s", dlerror());
    return h;
}

void *plat_dlsym(void *lib, const char *name) { return dlsym(lib ? lib : RTLD_DEFAULT, name); }

const char *plat_dl_suffix(void) {
#ifdef __APPLE__
    return ".dylib";
#else
    return ".so";
#endif
}

bool plat_path_trusted(const char *path, char *why, size_t whylen) {
    struct stat st;
    if (stat(path, &st) != 0) {
        snprintf(why, whylen, "%s: %s", path, strerror(errno));
        return false;
    }
    if (st.st_mode & (S_IWGRP | S_IWOTH)) {
        snprintf(why, whylen, "%s is writable by other users (mode %03o)", path, (unsigned)(st.st_mode & 0777));
        return false;
    }
    if (st.st_uid != getuid() && st.st_uid != 0) {
        snprintf(why, whylen, "%s is owned by another user (uid %u)", path, (unsigned)st.st_uid);
        return false;
    }
    return true;
}

bool plat_mkdir(const char *path) { return mkdir(path, 0755) == 0 || errno == EEXIST; }

bool plat_mkdir_private(const char *path) { return mkdir(path, 0700) == 0 || errno == EEXIST; }

bool plat_restrict_writes(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return false;
    return chmod(path, st.st_mode & 0755) == 0;
}

int plat_cpu_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}

/* ---- memory for machine code ----
 * On Apple silicon, MAP_JIT memory is writable or executable for each thread
 * in turn; elsewhere each piece has pages of its own, written, then made
 * executable. */
void *plat_code_reserve(size_t size) {
#ifdef CODE_MAP_JIT
    void *m = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
#else
    void *m = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
#endif
    return m == MAP_FAILED ? NULL : m;
}

size_t plat_code_align(void) {
#ifdef CODE_MAP_JIT
    return 16;
#else
    return (size_t)sysconf(_SC_PAGESIZE);
#endif
}

bool plat_code_write(void *at, const void *src, size_t n) {
#ifdef CODE_MAP_JIT
    pthread_jit_write_protect_np(0);
    memcpy(at, src, n);
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(at, n);
#else
    size_t page = plat_code_align(), len = (n + page - 1) & ~(page - 1);
    if (mprotect(at, len, PROT_READ | PROT_WRITE)) return false;
    memcpy(at, src, n);
    if (mprotect(at, len, PROT_READ | PROT_EXEC)) return false;
    __builtin___clear_cache((char *)at, (char *)at + n);
#endif
    return true;
}

void plat_code_free(void *region, size_t size) { munmap(region, size); }
#else
typedef int mlwsvm_posix_unused; /* ISO C forbids an empty translation unit */
#endif
