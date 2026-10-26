/* posix.c - platform layer for POSIX systems (Linux, macOS, BSDs). */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "platform.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
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
#else
typedef int mlwsvm_posix_unused; /* ISO C forbids an empty translation unit */
#endif
