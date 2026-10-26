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
/* Absolute path of the running executable (malloc'd), or NULL. */
char *plat_exe_path(const char *argv0);
/* Path list separator for MLWSVM_PATH (':' on POSIX, ';' on Windows). */
char plat_path_sep(void);

#endif
