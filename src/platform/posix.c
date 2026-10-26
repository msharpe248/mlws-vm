/* posix.c - platform layer for POSIX systems (Linux, macOS, BSDs). */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "platform.h"

#include <dirent.h>
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
#else
typedef int mlwsvm_posix_unused; /* ISO C forbids an empty translation unit */
#endif
