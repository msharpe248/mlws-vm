/* posix_io.c - file descriptors, file metadata, child processes and the I/O
 * poller (kqueue on macOS and the BSDs, epoll on Linux), for std.fs, std.os and
 * std.net. Errors are errno values; plat_err_code names them for MLWSVM. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#ifdef __linux__
#define _GNU_SOURCE /* pipe2 */
#endif
#include "platform.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#define USE_KQUEUE 1
#include <sys/event.h>
#elif defined(__linux__)
#define USE_EPOLL 1
#include <sys/epoll.h>
#include <sys/eventfd.h>
#else
#error "no I/O poller for this platform (kqueue or epoll is needed)"
#endif

extern char **environ;

int plat_last_error(void) { return errno; }

/* Name resolution errors are not errno values. */
#define ERR_UNKNOWN_HOST 100001
#define ERR_RESOLVE 100002

const char *plat_err_code(int err) {
    switch (err) {
    case ERR_UNKNOWN_HOST: return "E_NOT_FOUND";
    case ERR_RESOLVE: return "E_IO";
    case ENOENT: case ENOTDIR: return "E_NOT_FOUND";
    case EEXIST: case ENOTEMPTY: return "E_EXISTS";
    case EACCES: case EPERM: case EROFS: return "E_PERMISSION";
    case EISDIR: return "E_IS_DIR";
    case EBADF: return "E_CLOSED";
    case ETIMEDOUT: return "E_TIMEOUT";
    case ECONNREFUSED: return "E_REFUSED";
    case ECONNRESET: case EPIPE: return "E_RESET";
    case EADDRINUSE: return "E_ADDR_IN_USE";
    case EMFILE: case ENFILE: case ENOMEM: case ENOSPC: return "E_RESOURCES";
    default: return "E_IO";
    }
}

const char *plat_err_text(int err) {
    if (err == ERR_UNKNOWN_HOST) return "unknown host";
    if (err == ERR_RESOLVE) return "name resolution failed";
    return strerror(err);
}

void plat_ignore_sigpipe(void) { signal(SIGPIPE, SIG_IGN); } /* a closed pipe or socket: EPIPE, not death */

void plat_raise_fd_limit(void) {
    struct rlimit r;
    if (getrlimit(RLIMIT_NOFILE, &r) != 0) return;
    /* As high as the hard limit allows (macOS refuses values above its per-process maximum). */
    static const rlim_t tries[] = {1048576, 262144, 65536, 24576, 10240};
    for (size_t i = 0; i < sizeof tries / sizeof tries[0]; i++) {
        rlim_t want = r.rlim_max < tries[i] ? r.rlim_max : tries[i];
        if (want <= r.rlim_cur) return;
        struct rlimit n = {want, r.rlim_max};
        if (setrlimit(RLIMIT_NOFILE, &n) == 0) return;
    }
}

/* ---- file descriptors ----------------------------------------------------------- */

int plat_file_open(const char *path, int mode, int *err) {
    int flags = O_CLOEXEC;
    switch (mode & PLAT_O_ACCESS) {
    case PLAT_O_READ: flags |= O_RDONLY; break;
    case PLAT_O_WRITE: flags |= O_WRONLY; break;
    default: flags |= O_RDWR; break;
    }
    if (mode & PLAT_O_CREATE) flags |= O_CREAT;
    if (mode & PLAT_O_TRUNCATE) flags |= O_TRUNC;
    if (mode & PLAT_O_APPEND) flags |= O_APPEND;
    if (mode & PLAT_O_EXCLUSIVE) flags |= O_EXCL;
    int fd = open(path, flags, 0666);
    if (fd < 0) *err = errno;
    return fd;
}

long plat_fd_read(int fd, void *buf, size_t n, int *err) {
    for (;;) {
        ssize_t r = read(fd, buf, n);
        if (r >= 0) return (long)r;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return PLAT_IO_AGAIN;
        *err = errno;
        return PLAT_IO_ERR;
    }
}

long plat_fd_write(int fd, const void *buf, size_t n, int *err) {
    for (;;) {
        ssize_t r = write(fd, buf, n);
        if (r >= 0) return (long)r;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return PLAT_IO_AGAIN;
        *err = errno;
        return PLAT_IO_ERR;
    }
}

bool plat_fd_close(int fd) { return close(fd) == 0; }

bool plat_fd_pollable(int fd) {
    struct stat st;
    if (fstat(fd, &st) != 0) return false;
    return S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode) || S_ISCHR(st.st_mode);
}

bool plat_fd_set_nonblocking(int fd, bool on) {
    int fl = fcntl(fd, F_GETFL);
    if (fl < 0) return false;
    return fcntl(fd, F_SETFL, on ? fl | O_NONBLOCK : fl & ~O_NONBLOCK) == 0;
}

int64_t plat_fd_seek(int fd, int64_t off, int whence, int *err) {
    off_t r = lseek(fd, (off_t)off, whence == 1 ? SEEK_CUR : whence == 2 ? SEEK_END : SEEK_SET);
    if (r < 0) *err = errno;
    return (int64_t)r;
}

static int kind_of(mode_t m) {
    if (S_ISREG(m)) return PLAT_KIND_FILE;
    if (S_ISDIR(m)) return PLAT_KIND_DIR;
    if (S_ISLNK(m)) return PLAT_KIND_LINK;
    return PLAT_KIND_OTHER;
}

bool plat_stat(const char *path, bool follow, PlatStat *out, int *err) {
    struct stat st;
    if ((follow ? stat(path, &st) : lstat(path, &st)) != 0) {
        *err = errno;
        return false;
    }
    out->size = (int64_t)st.st_size;
#ifdef __APPLE__
    out->mtime_ns = (int64_t)st.st_mtimespec.tv_sec * 1000000000 + st.st_mtimespec.tv_nsec;
#else
    out->mtime_ns = (int64_t)st.st_mtim.tv_sec * 1000000000 + st.st_mtim.tv_nsec;
#endif
    out->mode = (uint32_t)(st.st_mode & 07777);
    out->kind = kind_of(st.st_mode);
    return true;
}

bool plat_remove(const char *path, int *err) {
    if (unlink(path) == 0) return true;
    *err = errno;
    return false;
}

bool plat_rmdir(const char *path, int *err) {
    if (rmdir(path) == 0) return true;
    *err = errno;
    return false;
}

bool plat_rename(const char *from, const char *to, int *err) {
    if (rename(from, to) == 0) return true;
    *err = errno;
    return false;
}

bool plat_mkdir_err(const char *path, int *err) {
    if (mkdir(path, 0777) == 0) return true;
    *err = errno;
    return false;
}

char *plat_getcwd(void) {
    char buf[4096];
    return getcwd(buf, sizeof buf) ? xstrdup(buf) : NULL;
}

char *const *plat_environ(void) { return environ; }

const char *plat_os_name(void) {
#if defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#elif defined(__FreeBSD__)
    return "freebsd";
#elif defined(__OpenBSD__)
    return "openbsd";
#elif defined(__NetBSD__)
    return "netbsd";
#else
    return "unix";
#endif
}

/* ---- child processes ------------------------------------------------------------- */

static bool cloexec_pipe(int fds[2]) {
#ifdef __linux__
    return pipe2(fds, O_CLOEXEC) == 0;
#else
    if (pipe(fds) != 0) return false; /* POSIX_SPAWN_CLOEXEC_DEFAULT keeps them out of children */
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    return true;
#endif
}

bool plat_spawn(char *const argv[], int pipes, PlatChild *c, int *err) {
    int p[3][2] = {{-1, -1}, {-1, -1}, {-1, -1}};
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t attr;
    posix_spawn_file_actions_init(&fa);
    posix_spawnattr_init(&attr);
    bool ok = true;
    for (int k = 0; k < 3 && ok; k++) {
        if (!(pipes & (1 << k))) {
            posix_spawn_file_actions_adddup2(&fa, k, k); /* inherit (also clears CLOEXEC) */
            continue;
        }
        if (k == 2 && (pipes & PLAT_MERGE_ERR)) continue;
        ok = cloexec_pipe(p[k]);
        if (ok) posix_spawn_file_actions_adddup2(&fa, k == 0 ? p[k][0] : p[k][1], k);
    }
    if (ok && (pipes & PLAT_MERGE_ERR)) posix_spawn_file_actions_adddup2(&fa, 1, 2);
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);
#endif
    pid_t pid = 0;
    int rc = ok ? posix_spawnp(&pid, argv[0], &fa, &attr, argv, environ) : errno;
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    int fds[3] = {-1, -1, -1};
    for (int k = 0; k < 3; k++) {
        if (p[k][0] < 0) continue;
        close(k == 0 ? p[k][0] : p[k][1]); /* the child's ends */
        fds[k] = k == 0 ? p[k][1] : p[k][0];
        if (rc != 0) close(fds[k]), fds[k] = -1;
    }
    if (rc != 0) {
        *err = rc;
        return false;
    }
    c->pid = pid;
    c->in_fd = fds[0];
    c->out_fd = fds[1];
    c->err_fd = fds[2];
    return true;
}

bool plat_wait_child(int64_t pid, int *code, int *err) {
    int st;
    for (;;) {
        pid_t r = waitpid((pid_t)pid, &st, 0);
        if (r >= 0) break;
        if (errno == EINTR) continue;
        *err = errno;
        return false;
    }
    *code = WIFEXITED(st) ? WEXITSTATUS(st) : WIFSIGNALED(st) ? 128 + WTERMSIG(st) : 255;
    return true;
}

bool plat_signal_child(int64_t pid, int sig) { return kill((pid_t)pid, sig == 9 ? SIGKILL : SIGTERM) == 0; }

/* ---- sockets ------------------------------------------------------------------------- */

static void set_cloexec_nonblock(int fd) {
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

bool plat_host_numeric(const char *host) {
    unsigned char buf[16];
    return !host || !*host || inet_pton(AF_INET, host, buf) == 1 || inet_pton(AF_INET6, host, buf) == 1;
}

static bool resolve(const char *host, int port, int type, bool passive, struct addrinfo **res, int *err) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = type;
    hints.ai_flags = AI_NUMERICSERV | (passive ? AI_PASSIVE : 0);
    char ps[16];
    snprintf(ps, sizeof ps, "%d", port);
    int rc = getaddrinfo(host && *host ? host : NULL, ps, &hints, res);
    if (rc == 0) return true;
    if (rc == EAI_SYSTEM) *err = errno;
    else *err = rc == EAI_NONAME ? ERR_UNKNOWN_HOST : ERR_RESOLVE;
    return false;
}

int plat_sock_listen(const char *host, int port, int *err) {
    struct addrinfo *res;
    if (!resolve(host, port, SOCK_STREAM, true, &res, err)) return -1;
    int fd = -1;
    for (struct addrinfo *a = res; a && fd < 0; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) {
            *err = errno;
            continue;
        }
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (bind(fd, a->ai_addr, a->ai_addrlen) != 0 || listen(fd, 1024) != 0) {
            *err = errno;
            close(fd);
            fd = -1;
        }
    }
    freeaddrinfo(res);
    if (fd >= 0) set_cloexec_nonblock(fd);
    return fd;
}

int plat_sock_accept(int lfd, int *err) {
    for (;;) {
        int c = accept(lfd, NULL, NULL);
        if (c >= 0) {
            set_cloexec_nonblock(c);
            int one = 1;
            setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            return c;
        }
        if (errno == EINTR || errno == ECONNABORTED) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return PLAT_IO_AGAIN;
        *err = errno;
        return PLAT_IO_ERR;
    }
}

/* Blocking (run on the I/O pool): resolve, then try each address in turn. */
int plat_sock_connect(const char *host, int port, int64_t timeout_ms, int *err) {
    struct addrinfo *res;
    if (!resolve(host, port, SOCK_STREAM, false, &res, err)) return -1;
    int fd = -1;
    for (struct addrinfo *a = res; a && fd < 0; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) {
            *err = errno;
            continue;
        }
        set_cloexec_nonblock(fd);
        int rc = connect(fd, a->ai_addr, a->ai_addrlen);
        if (rc != 0 && errno == EINPROGRESS) {
            struct pollfd pf = {fd, POLLOUT, 0};
            int n = poll(&pf, 1, timeout_ms < 0 ? -1 : (int)timeout_ms);
            int soerr = 0;
            socklen_t len = sizeof soerr;
            if (n == 0) soerr = ETIMEDOUT;
            else if (n < 0) soerr = errno;
            else getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len);
            rc = soerr ? -1 : 0;
            errno = soerr;
        }
        if (rc != 0) {
            *err = errno;
            close(fd);
            fd = -1;
        }
    }
    freeaddrinfo(res);
    if (fd >= 0) {
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    }
    return fd;
}

int plat_sock_port(int fd) {
    struct sockaddr_storage ss;
    socklen_t len = sizeof ss;
    if (getsockname(fd, (struct sockaddr *)&ss, &len) != 0) return -1;
    return ntohs(ss.ss_family == AF_INET6 ? ((struct sockaddr_in6 *)&ss)->sin6_port : ((struct sockaddr_in *)&ss)->sin_port);
}

static void format_addr(const struct sockaddr *sa, char *buf, size_t n, int *port) {
    char ip[INET6_ADDRSTRLEN] = "?";
    if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *s6 = (const struct sockaddr_in6 *)sa;
        inet_ntop(AF_INET6, &s6->sin6_addr, ip, sizeof ip);
        *port = ntohs(s6->sin6_port);
    } else {
        const struct sockaddr_in *s4 = (const struct sockaddr_in *)sa;
        inet_ntop(AF_INET, &s4->sin_addr, ip, sizeof ip);
        *port = ntohs(s4->sin_port);
    }
    snprintf(buf, n, "%s", ip);
}

bool plat_sock_peer(int fd, char *buf, size_t n, int *port) {
    struct sockaddr_storage ss;
    socklen_t len = sizeof ss;
    if (getpeername(fd, (struct sockaddr *)&ss, &len) != 0) return false;
    format_addr((struct sockaddr *)&ss, buf, n, port);
    return true;
}

bool plat_sock_shutdown_write(int fd) { return shutdown(fd, SHUT_WR) == 0; }

bool plat_resolve(const char *name, char ***out, size_t *n, int *err) {
    struct addrinfo *res;
    if (!resolve(name, 0, SOCK_STREAM, false, &res, err)) return false;
    VEC(char *) v = {0};
    for (struct addrinfo *a = res; a; a = a->ai_next) {
        char ip[INET6_ADDRSTRLEN];
        int port;
        format_addr(a->ai_addr, ip, sizeof ip, &port);
        bool dup = false;
        for (size_t i = 0; i < v.n; i++) dup = dup || !strcmp(v.v[i], ip);
        if (!dup) VEC_PUSH(v, xstrdup(ip));
    }
    freeaddrinfo(res);
    *out = v.v;
    *n = v.n;
    return true;
}

int plat_udp_open(const char *host, int port, int *err) {
    struct addrinfo *res;
    if (!resolve(host && *host ? host : "0.0.0.0", port, SOCK_DGRAM, true, &res, err)) return -1;
    int fd = -1;
    for (struct addrinfo *a = res; a && fd < 0; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) {
            *err = errno;
            continue;
        }
        if (bind(fd, a->ai_addr, a->ai_addrlen) != 0) {
            *err = errno;
            close(fd);
            fd = -1;
        }
    }
    freeaddrinfo(res);
    if (fd >= 0) set_cloexec_nonblock(fd);
    return fd;
}

long plat_udp_send(int fd, const char *host, int port, const void *buf, size_t n, int *err) {
    struct addrinfo *res;
    if (!resolve(host, port, SOCK_DGRAM, false, &res, err)) return PLAT_IO_ERR;
    long r = PLAT_IO_ERR;
    for (;;) {
        ssize_t s = sendto(fd, buf, n, 0, res->ai_addr, res->ai_addrlen);
        if (s >= 0) r = (long)s;
        else if (errno == EINTR) continue;
        else if (errno == EAGAIN || errno == EWOULDBLOCK) r = PLAT_IO_AGAIN;
        else *err = errno;
        break;
    }
    freeaddrinfo(res);
    return r;
}

long plat_udp_recv(int fd, void *buf, size_t n, char *addr, size_t alen, int *port, int *err) {
    for (;;) {
        struct sockaddr_storage ss;
        socklen_t len = sizeof ss;
        ssize_t r = recvfrom(fd, buf, n, 0, (struct sockaddr *)&ss, &len);
        if (r >= 0) {
            format_addr((struct sockaddr *)&ss, addr, alen, port);
            return (long)r;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return PLAT_IO_AGAIN;
        *err = errno;
        return PLAT_IO_ERR;
    }
}

bool plat_random_bytes(void *buf, size_t n) {
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, (char *)buf + got, n - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    return got == n;
}

/* ---- the poller -------------------------------------------------------------------- */

struct PlatPoller {
    int fd;
#ifdef USE_EPOLL
    int wake_fd; /* an eventfd */
#endif
};

PlatPoller *plat_poller_new(void) {
    PlatPoller *p = xcalloc(1, sizeof *p);
#ifdef USE_KQUEUE
    p->fd = kqueue();
    struct kevent ev;
    EV_SET(&ev, 0, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);
    if (p->fd >= 0) kevent(p->fd, &ev, 1, NULL, 0, NULL);
#else
    p->fd = epoll_create1(EPOLL_CLOEXEC);
    p->wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    struct epoll_event ev = {.events = EPOLLIN, .data.u64 = 0};
    if (p->fd >= 0) epoll_ctl(p->fd, EPOLL_CTL_ADD, p->wake_fd, &ev);
#endif
    if (p->fd < 0) {
        free(p);
        return NULL;
    }
    return p;
}

void plat_poller_free(PlatPoller *p) {
    if (!p) return;
    close(p->fd);
#ifdef USE_EPOLL
    close(p->wake_fd);
#endif
    free(p);
}

bool plat_poller_arm(PlatPoller *p, int fd, int events, uint64_t token) {
#ifdef USE_KQUEUE
    struct kevent ev;
    EV_SET(&ev, fd, events & PLAT_POLL_WRITE ? EVFILT_WRITE : EVFILT_READ, EV_ADD | EV_ONESHOT, 0, 0,
           (void *)(uintptr_t)token);
    return kevent(p->fd, &ev, 1, NULL, 0, NULL) == 0;
#else
    struct epoll_event ev = {.events = (events & PLAT_POLL_WRITE ? EPOLLOUT : EPOLLIN) | EPOLLONESHOT,
                             .data.u64 = token};
    if (epoll_ctl(p->fd, EPOLL_CTL_MOD, fd, &ev) == 0) return true;
    return errno == ENOENT && epoll_ctl(p->fd, EPOLL_CTL_ADD, fd, &ev) == 0;
#endif
}

void plat_poller_forget(PlatPoller *p, int fd) {
#ifdef USE_EPOLL
    epoll_ctl(p->fd, EPOLL_CTL_DEL, fd, NULL); /* kqueue forgets closed descriptors itself */
#else
    (void)p, (void)fd;
#endif
}

int plat_poller_wait(PlatPoller *p, uint64_t *tokens, int max, int64_t timeout_ns) {
    if (max > 64) max = 64;
#ifdef USE_KQUEUE
    struct kevent evs[64];
    struct timespec ts, *tp = NULL;
    if (timeout_ns >= 0) {
        ts.tv_sec = (time_t)(timeout_ns / 1000000000);
        ts.tv_nsec = (long)(timeout_ns % 1000000000);
        tp = &ts;
    }
    int n = kevent(p->fd, NULL, 0, evs, max, tp);
    int k = 0;
    for (int i = 0; i < n; i++)
        if (evs[i].filter != EVFILT_USER) tokens[k++] = (uint64_t)(uintptr_t)evs[i].udata;
    return n < 0 ? 0 : k;
#else
    struct epoll_event evs[64];
    int n = epoll_wait(p->fd, evs, max, timeout_ns < 0 ? -1 : (int)(timeout_ns / 1000000));
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (evs[i].data.u64 == 0) {
            uint64_t v;
            if (read(p->wake_fd, &v, sizeof v) < 0) { /* already drained */
            }
            continue;
        }
        tokens[k++] = evs[i].data.u64;
    }
    return n < 0 ? 0 : k;
#endif
}

void plat_poller_wake(PlatPoller *p) {
#ifdef USE_KQUEUE
    struct kevent ev;
    EV_SET(&ev, 0, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
    kevent(p->fd, &ev, 1, NULL, 0, NULL);
#else
    uint64_t one = 1;
    if (write(p->wake_fd, &one, sizeof one) < 0) { /* the counter is full: a wake-up is pending anyway */
    }
#endif
}

#else
typedef int mlwsvm_posix_io_unused; /* ISO C forbids an empty translation unit */
#endif
