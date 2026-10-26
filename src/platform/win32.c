/* win32.c - platform layer for Windows. Not yet exercised by CI; kept in step
 * with posix.c so the rest of the code base stays OS-independent. */
#ifdef _WIN32
#include "platform.h"

#include <sys/stat.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <io.h>
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

void plat_sleep_ns(uint64_t ns) { Sleep((DWORD)((ns + 999999) / 1000000)); }

char *plat_exe_path(const char *argv0) {
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, buf, sizeof buf);
    if (n > 0 && n < sizeof buf) return xstrdup(buf);
    return argv0 ? plat_realpath(argv0) : NULL;
}

char plat_path_sep(void) { return ';'; }

static void wsa_init(void) {
    static bool done;
    if (!done) {
        WSADATA w;
        WSAStartup(MAKEWORD(2, 2), &w);
        done = true;
    }
}

int plat_tcp_listen(const char *host, int port, char *err, size_t errlen) {
    wsa_init();
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) {
        snprintf(err, errlen, "bad IPv4 address '%s'", host);
        return -1;
    }
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET || bind(s, (struct sockaddr *)&a, sizeof a) != 0 || listen(s, 16) != 0) {
        snprintf(err, errlen, "cannot listen on %s:%d", host, port);
        return -1;
    }
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    return (int)s;
}

int plat_tcp_accept(int listen_fd) {
    SOCKET c = accept((SOCKET)listen_fd, NULL, NULL);
    if (c == INVALID_SOCKET) return -1;
    u_long nb = 0;
    ioctlsocket(c, FIONBIO, &nb);
    return (int)c;
}

int plat_tcp_connect(const char *host, int port) {
    wsa_init();
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) return -1;
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET || connect(s, (struct sockaddr *)&a, sizeof a) != 0) return -1;
    return (int)s;
}

bool plat_wait_readable(int fd, uint64_t timeout_ns) {
    WSAPOLLFD pf = {(SOCKET)fd, POLLRDNORM, 0};
    return WSAPoll(&pf, 1, (INT)((timeout_ns + 999999) / 1000000)) > 0;
}

long plat_sock_read(int fd, void *buf, size_t n, uint64_t timeout_ns) {
    if (!plat_wait_readable(fd, timeout_ns)) return -1;
    int r = recv((SOCKET)fd, buf, (int)n, 0);
    return r < 0 ? -1 : (long)r;
}

bool plat_sock_write_all(int fd, const void *buf, size_t n) {
    const char *p = buf;
    while (n) {
        int w = send((SOCKET)fd, p, (int)n, 0);
        if (w <= 0) return false;
        p += w;
        n -= (size_t)w;
    }
    return true;
}

void plat_sock_close(int fd) { closesocket((SOCKET)fd); }

bool plat_is_tty(void) { return _isatty(1) != 0; }
#else
typedef int mlwsvm_win32_unused; /* ISO C forbids an empty translation unit */
#endif
