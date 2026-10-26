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

struct PlatThread {
    HANDLE h;
    void (*fn)(void *);
    void *arg;
};
struct PlatMutex {
    SRWLOCK l;
};
struct PlatCond {
    CONDITION_VARIABLE c;
};
struct PlatLock {
    SRWLOCK l;
};

PlatLock *plat_lock_new(void) {
    PlatLock *l = xcalloc(1, sizeof *l);
    InitializeSRWLock(&l->l);
    return l;
}

void plat_lock_free(PlatLock *l) { free(l); }
void plat_lock(PlatLock *l) { AcquireSRWLockExclusive(&l->l); }
void plat_unlock(PlatLock *l) { ReleaseSRWLockExclusive(&l->l); }

static DWORD WINAPI thread_main(LPVOID x) {
    PlatThread *t = x;
    t->fn(t->arg);
    return 0;
}

PlatThread *plat_thread_start(void (*fn)(void *), void *arg) {
    PlatThread *t = xcalloc(1, sizeof *t);
    t->fn = fn;
    t->arg = arg;
    t->h = CreateThread(NULL, 0, thread_main, t, 0, NULL);
    if (!t->h) {
        free(t);
        return NULL;
    }
    return t;
}

void plat_thread_join(PlatThread *t) {
    WaitForSingleObject(t->h, INFINITE);
    CloseHandle(t->h);
    free(t);
}

PlatMutex *plat_mutex_new(void) {
    PlatMutex *m = xcalloc(1, sizeof *m);
    InitializeSRWLock(&m->l);
    return m;
}

void plat_mutex_free(PlatMutex *m) { free(m); }
void plat_mutex_lock(PlatMutex *m) { AcquireSRWLockExclusive(&m->l); }
void plat_mutex_unlock(PlatMutex *m) { ReleaseSRWLockExclusive(&m->l); }

PlatCond *plat_cond_new(void) {
    PlatCond *c = xcalloc(1, sizeof *c);
    InitializeConditionVariable(&c->c);
    return c;
}

void plat_cond_free(PlatCond *c) { free(c); }

void plat_cond_wait(PlatCond *c, PlatMutex *m, uint64_t timeout_ns) {
    SleepConditionVariableSRW(&c->c, &m->l, timeout_ns ? (DWORD)((timeout_ns + 999999) / 1000000) : INFINITE, 0);
}

void plat_cond_signal(PlatCond *c) { WakeConditionVariable(&c->c); }
void plat_cond_broadcast(PlatCond *c) { WakeAllConditionVariable(&c->c); }

void plat_thread_detach(PlatThread *t) { CloseHandle(t->h); }

void *plat_dlopen(const char *path, char *err, size_t errlen) {
    HMODULE h = path ? LoadLibraryA(path) : GetModuleHandleA(NULL);
    if (!h && err) snprintf(err, errlen, "LoadLibrary failed (error %lu)", (unsigned long)GetLastError());
    return (void *)h;
}

void *plat_dlsym(void *lib, const char *name) {
    return (void *)GetProcAddress(lib ? (HMODULE)lib : GetModuleHandleA(NULL), name);
}

const char *plat_dl_suffix(void) { return ".dll"; }

bool plat_path_trusted(const char *path, char *why, size_t whylen) {
    /* TODO: check the file's ACL. Until then, only existence is checked. */
    DWORD a = GetFileAttributesA(path);
    if (a == INVALID_FILE_ATTRIBUTES) {
        snprintf(why, whylen, "%s: not found", path);
        return false;
    }
    return true;
}

bool plat_mkdir(const char *path) { return CreateDirectoryA(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS; }

bool plat_restrict_writes(const char *path) { return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES; }

int plat_cpu_count(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors > 0 ? (int)si.dwNumberOfProcessors : 1;
}
#else
typedef int mlwsvm_win32_unused; /* ISO C forbids an empty translation unit */
#endif
