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

#endif
