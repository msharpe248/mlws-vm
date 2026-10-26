/* platform.h - the only OS-specific surface of MLWSVM. Implementations live in
 * posix.c and win32.c; everything else is plain C11. */
#ifndef MLWSVM_PLATFORM_H
#define MLWSVM_PLATFORM_H

#include "../core/common.h"

/* Canonical absolute path (malloc'd) or NULL if the file does not exist. */
char *plat_realpath(const char *path);
/* Modification time in seconds since the epoch, or -1. */
int64_t plat_mtime(const char *path);
bool plat_is_dir(const char *path);
/* List directory entries (excluding . and ..). Caller frees each name and the array. */
bool plat_list_dir(const char *path, char ***names, size_t *n);
/* Monotonic clock in nanoseconds. */
uint64_t plat_now_ns(void);
int64_t plat_wall_ns(void);                  /* the wall clock: ns since 1970-01-01T00:00:00Z */
int64_t plat_local_offset_s(int64_t secs);   /* the local time zone's offset from UTC at a time */
void plat_sleep_ns(uint64_t ns);
/* Absolute path of the running executable (malloc'd), or NULL. */
char *plat_exe_path(const char *argv0);
/* Path list separator for MLWSVM_PATH (':' on POSIX, ';' on Windows). */
char plat_path_sep(void);

/* Minimal TCP for the monitor (all sockets are plain ints; -1 = error). */
int plat_tcp_listen(const char *host, int port, char *err, size_t errlen); /* non-blocking listener */
int plat_tcp_accept(int listen_fd);           /* -1 if nothing is pending */
int plat_tcp_connect(const char *host, int port);
bool plat_wait_readable(int fd, uint64_t timeout_ns);
long plat_sock_read(int fd, void *buf, size_t n, uint64_t timeout_ns); /* 0 = closed, -1 = error/timeout */
bool plat_sock_write_all(int fd, const void *buf, size_t n);
void plat_sock_close(int fd);
bool plat_is_tty(void);                       /* stdout is a terminal */

/* Threads (pthreads on POSIX, Win32 threads/SRW locks/condition variables). */
typedef struct PlatThread PlatThread;
/* A lock for short critical sections (os_unfair_lock on macOS); PlatMutex is
 * the one to use with a condition variable. */
typedef struct PlatLock PlatLock;
PlatLock *plat_lock_new(void);
void plat_lock_free(PlatLock *l);
void plat_lock(PlatLock *l);
void plat_unlock(PlatLock *l);
typedef struct PlatMutex PlatMutex;
typedef struct PlatCond PlatCond;
PlatThread *plat_thread_start(void (*fn)(void *), void *arg);
void plat_thread_join(PlatThread *t);
PlatMutex *plat_mutex_new(void);
void plat_mutex_free(PlatMutex *m);
void plat_mutex_lock(PlatMutex *m);
void plat_mutex_unlock(PlatMutex *m);
PlatCond *plat_cond_new(void);
void plat_cond_free(PlatCond *c);
void plat_cond_wait(PlatCond *c, PlatMutex *m, uint64_t timeout_ns); /* 0 = no timeout */
void plat_cond_signal(PlatCond *c);
void plat_cond_broadcast(PlatCond *c);
int plat_cpu_count(void);
void plat_thread_detach(PlatThread *t); /* let it run on; its resources go when it ends */

/* Shared libraries (native extensions and the FFI). */
void *plat_dlopen(const char *path, char *err, size_t errlen); /* NULL: the program's own symbols */
void *plat_dlsym(void *lib, const char *name);
const char *plat_dl_suffix(void);              /* ".dylib", ".so" or ".dll" */
/* Is a file or directory safe to trust code from: not writable by other users,
 * and owned by the current user or the administrator? If not, says why. */
bool plat_path_trusted(const char *path, char *why, size_t whylen);
bool plat_mkdir(const char *path);            /* one level; true if it now exists */
bool plat_restrict_writes(const char *path);  /* writable only by its owner (0644 / 0755) */

/* ---- I/O for std.fs, std.os and std.net (posix_io.c) ------------------------
 * Functions report failure with an errno-style code in *err; plat_err_code
 * names it for MLWSVM (E_NOT_FOUND, E_PERMISSION, ...). */
int plat_last_error(void);
const char *plat_err_code(int err);
const char *plat_err_text(int err);

#define PLAT_O_READ 1
#define PLAT_O_WRITE 2
#define PLAT_O_RDWR 3
#define PLAT_O_ACCESS 3
#define PLAT_O_CREATE 4
#define PLAT_O_TRUNCATE 8
#define PLAT_O_APPEND 16
#define PLAT_O_EXCLUSIVE 32
#define PLAT_IO_ERR (-1)
#define PLAT_IO_AGAIN (-2) /* a non-blocking descriptor is not ready */
int plat_file_open(const char *path, int mode, int *err);
long plat_fd_read(int fd, void *buf, size_t n, int *err);
long plat_fd_write(int fd, const void *buf, size_t n, int *err);
bool plat_fd_close(int fd);
bool plat_fd_pollable(int fd); /* pipe, socket or terminal: waited for with the poller */
bool plat_fd_set_nonblocking(int fd, bool on);
int64_t plat_fd_seek(int fd, int64_t off, int whence, int *err); /* whence 0 start, 1 current, 2 end */

enum { PLAT_KIND_FILE = 1, PLAT_KIND_DIR = 2, PLAT_KIND_LINK = 3, PLAT_KIND_OTHER = 4 };
typedef struct {
    int64_t size, mtime_ns;
    uint32_t mode; /* permission bits */
    int kind;      /* PLAT_KIND_* */
} PlatStat;
bool plat_stat(const char *path, bool follow_links, PlatStat *st, int *err);
bool plat_remove(const char *path, int *err);
bool plat_rmdir(const char *path, int *err);
bool plat_rename(const char *from, const char *to, int *err);
bool plat_mkdir_err(const char *path, int *err);
char *plat_getcwd(void); /* malloc'd */
char *const *plat_environ(void); /* NAME=VALUE strings, NULL-terminated */
const char *plat_os_name(void);  /* "macos", "linux", "freebsd", "windows", ... */

/* Child processes. `pipes` selects which of stdin/stdout/stderr get a pipe
 * (the others are inherited); PLAT_MERGE_ERR sends stderr to stdout. */
#define PLAT_PIPE_IN 1
#define PLAT_PIPE_OUT 2
#define PLAT_PIPE_ERR 4
#define PLAT_MERGE_ERR 8
typedef struct {
    int64_t pid;
    int in_fd, out_fd, err_fd; /* our ends of the pipes, or -1 */
} PlatChild;
bool plat_spawn(char *const argv[], int pipes, PlatChild *c, int *err); /* searches PATH */
bool plat_wait_child(int64_t pid, int *exit_code, int *err);           /* blocks; 128+N for signal N */
bool plat_signal_child(int64_t pid, int sig);                          /* 9 = kill, else terminate */

/* Sockets (std.net). Listening and accepted sockets are non-blocking; connect
 * blocks (run it on the I/O pool). Hosts may be names unless noted. */
void plat_ignore_sigpipe(void);
void plat_raise_fd_limit(void);
bool plat_host_numeric(const char *host); /* "" or an IPv4/IPv6 literal: no DNS needed */
int plat_sock_listen(const char *host, int port, int *err);
int plat_sock_accept(int listen_fd, int *err); /* PLAT_IO_AGAIN if none is waiting */
int plat_sock_connect(const char *host, int port, int64_t timeout_ms, int *err);
int plat_sock_port(int fd); /* the local port */
bool plat_sock_peer(int fd, char *ip, size_t n, int *port);
bool plat_sock_shutdown_write(int fd);
bool plat_resolve(const char *name, char ***addrs, size_t *n, int *err); /* malloc'd strings */
int plat_udp_open(const char *host, int port, int *err);
long plat_udp_send(int fd, const char *host, int port, const void *buf, size_t n, int *err);
long plat_udp_recv(int fd, void *buf, size_t n, char *ip, size_t iplen, int *port, int *err);
bool plat_random_bytes(void *buf, size_t n); /* from the OS's entropy source */

/* The poller: one-shot waits for descriptors to become readable or writable. */
typedef struct PlatPoller PlatPoller;
#define PLAT_POLL_READ 1
#define PLAT_POLL_WRITE 2
PlatPoller *plat_poller_new(void);
void plat_poller_free(PlatPoller *p);
bool plat_poller_arm(PlatPoller *p, int fd, int events, uint64_t token); /* token != 0 */
void plat_poller_forget(PlatPoller *p, int fd);                          /* before closing fd */
int plat_poller_wait(PlatPoller *p, uint64_t *tokens, int max, int64_t timeout_ns); /* -1: forever */
void plat_poller_wake(PlatPoller *p);

#endif
