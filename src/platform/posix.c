/* posix.c - platform layer for POSIX systems (Linux, macOS, BSDs). */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
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
#include <sys/socket.h>
#include <limits.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
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

char *plat_exe_path(const char *argv0) {
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
#endif
    return argv0 ? plat_realpath(argv0) : NULL;
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
#else
typedef int mlwsvm_posix_unused; /* ISO C forbids an empty translation unit */
#endif
