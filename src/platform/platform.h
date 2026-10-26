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

#endif
