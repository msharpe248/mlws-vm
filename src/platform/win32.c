/* win32.c - platform layer for Windows. Not yet exercised by CI; kept in step
 * with posix.c so the rest of the code base stays OS-independent. */
#ifdef _WIN32
#include "platform.h"

#include <sys/stat.h>
#include <windows.h>

char *plat_realpath(const char *path) {
    char buf[MAX_PATH];
    if (!_fullpath(buf, path, sizeof buf)) return NULL;
    if (GetFileAttributesA(buf) == INVALID_FILE_ATTRIBUTES) return NULL;
    return xstrdup(buf);
}

int64_t plat_mtime(const char *path) {
    struct _stat64 st;
    if (_stat64(path, &st) != 0) return -1;
    return (int64_t)st.st_mtime;
}

bool plat_is_dir(const char *path) {
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool plat_list_dir(const char *path, char ***names, size_t *n) {
    char *pat = path_join(path, "*");
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    free(pat);
    if (h == INVALID_HANDLE_VALUE) return false;
    VEC(char *) v = {0};
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        VEC_PUSH(v, xstrdup(fd.cFileName));
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    *names = v.v;
    *n = v.n;
    return true;
}

uint64_t plat_now_ns(void) {
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart * 1000000000.0 / (double)freq.QuadPart);
}

char *plat_exe_path(const char *argv0) {
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, buf, sizeof buf);
    if (n > 0 && n < sizeof buf) return xstrdup(buf);
    return argv0 ? plat_realpath(argv0) : NULL;
}

char plat_path_sep(void) { return ';'; }
#else
typedef int mlwsvm_win32_unused; /* ISO C forbids an empty translation unit */
#endif
