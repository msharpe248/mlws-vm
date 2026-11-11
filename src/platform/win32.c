/* win32.c - platform layer for Windows: paths, clocks, threads, shared
 * libraries, the trust checks on directories, memory for machine code, and the
 * monitor's TCP. File, process and socket I/O and the poller are in
 * win32_io.c. Strings are UTF-8, converted to UTF-16 for Windows' wide APIs. */
#ifdef _WIN32
#include "win32.h"

#include <fcntl.h>
#include <io.h>
#include <sddl.h>
#include <aclapi.h>

/* ---- UTF-8 <-> UTF-16 ---------------------------------------------------------- */

wchar_t *w_str(const char *s) {
    if (!s) return NULL;
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) n = 1;
    wchar_t *w = xmalloc((size_t)n * sizeof *w);
    if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n)) w[0] = 0;
    return w;
}

char *u_str(const wchar_t *w) {
    if (!w) return NULL;
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) n = 1;
    char *s = xmalloc((size_t)n);
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL)) s[0] = 0;
    return s;
}

/* ---- descriptors ------------------------------------------------------------------
 * A two-level table, so an entry never moves: chunks of 1024 entries, made as
 * needed and kept. Freed numbers are reused, lowest last. */
#define WFD_CHUNK 1024
#define WFD_CHUNKS 1024
static WFd *wfd_chunks[WFD_CHUNKS];
static SRWLOCK wfd_lock = SRWLOCK_INIT;
static int wfd_top = 3; /* 0-2: the standard handles */
static VEC(int) wfd_free;
static INIT_ONCE std_once = INIT_ONCE_STATIC_INIT;

static WFd *slot(int fd) {
    WFd *c = wfd_chunks[fd / WFD_CHUNK];
    return c ? &c[fd % WFD_CHUNK] : NULL;
}

static int kind_of_handle(HANDLE h) {
    switch (GetFileType(h)) {
    case FILE_TYPE_DISK: return WFD_FILE;
    case FILE_TYPE_PIPE: return WFD_PIPE;
    default: return WFD_CHAR;
    }
}

static BOOL CALLBACK std_init(PINIT_ONCE once, PVOID arg, PVOID *ctx) {
    (void)once, (void)arg, (void)ctx;
    wfd_chunks[0] = xcalloc(WFD_CHUNK, sizeof(WFd));
    static const DWORD which[3] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
    for (int i = 0; i < 3; i++) {
        HANDLE h = GetStdHandle(which[i]);
        if (h == NULL || h == INVALID_HANDLE_VALUE) continue;
        wfd_chunks[0][i].kind = kind_of_handle(h);
        wfd_chunks[0][i].h = h;
    }
    return TRUE;
}

int wfd_new(int kind, HANDLE h, SOCKET s) {
    InitOnceExecuteOnce(&std_once, std_init, NULL, NULL);
    AcquireSRWLockExclusive(&wfd_lock);
    int fd = -1;
    if (wfd_free.n) fd = wfd_free.v[--wfd_free.n];
    else if (wfd_top < WFD_CHUNK * WFD_CHUNKS) {
        fd = wfd_top++;
        if (!wfd_chunks[fd / WFD_CHUNK]) wfd_chunks[fd / WFD_CHUNK] = xcalloc(WFD_CHUNK, sizeof(WFd));
    }
    if (fd >= 0) *slot(fd) = (WFd){.kind = kind, .h = h, .s = s};
    ReleaseSRWLockExclusive(&wfd_lock);
    return fd;
}

WFd *wfd_get(int fd) {
    InitOnceExecuteOnce(&std_once, std_init, NULL, NULL);
    if (fd < 0 || fd >= WFD_CHUNK * WFD_CHUNKS) return NULL;
    WFd *e = slot(fd);
    return e && e->kind ? e : NULL;
}

void wfd_release(int fd) {
    WFd *e = wfd_get(fd);
    if (!e) return;
    AcquireSRWLockExclusive(&wfd_lock);
    e->kind = 0;
    if (fd >= 3) VEC_PUSH(wfd_free, fd);
    ReleaseSRWLockExclusive(&wfd_lock);
}

static BOOL CALLBACK wsa_start(PINIT_ONCE once, PVOID arg, PVOID *ctx) {
    (void)once, (void)arg, (void)ctx;
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
    return TRUE;
}

void wsa_init(void) {
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&once, wsa_start, NULL, NULL);
}

/* ---- paths ------------------------------------------------------------------------ */

static bool attrs(const char *path, WIN32_FILE_ATTRIBUTE_DATA *a) {
    wchar_t *w = w_str(path);
    bool ok = GetFileAttributesExW(w, GetFileExInfoStandard, a);
    free(w);
    return ok;
}

char *plat_realpath(const char *path) {
    wchar_t *w = w_str(path), buf[32768];
    DWORD n = GetFullPathNameW(w, 32768, buf, NULL);
    free(w);
    if (n == 0 || n >= 32768) return NULL;
    if (GetFileAttributesW(buf) == INVALID_FILE_ATTRIBUTES) return NULL;
    return u_str(buf);
}

int64_t win_filetime_ns(FILETIME ft) {
    uint64_t t = (uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime;
    return ((int64_t)t - 116444736000000000ll) * 100;
}

int64_t plat_mtime(const char *path) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!attrs(path, &a)) return -1;
    return win_filetime_ns(a.ftLastWriteTime) / 1000000000;
}

bool plat_is_dir(const char *path) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    return attrs(path, &a) && (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool plat_list_dir(const char *path, char ***names, size_t *n) {
    char *pat = path_join(path, "*");
    wchar_t *w = w_str(pat);
    free(pat);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(w, &fd);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return false;
    VEC(char *) v = {0};
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        VEC_PUSH(v, u_str(fd.cFileName));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    *names = v.v;
    *n = v.n;
    return true;
}

long plat_stdin_read(void *buf, size_t n, uint64_t timeout_ns) {
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    if (timeout_ns != UINT64_MAX) { /* a pipe, as editors give: a file or console simply reads */
        DWORD avail = 0;
        if (PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL) && !avail) return -1;
    }
    DWORD got = 0;
    return ReadFile(h, buf, (DWORD)n, &got, NULL) ? (long)got : 0;
}

static WFd saved_stdin; /* plat_stdin_feed: table entry 0 as it was */
static HANDLE saved_std;
static bool stdin_fed;

bool plat_stdin_feed(const char *data, size_t n) {
    HANDLE r, w;
    if (stdin_fed || n > 0x10000 || !CreatePipe(&r, &w, NULL, (DWORD)n + 4096)) return false;
    DWORD put = 0;
    BOOL ok = n ? WriteFile(w, data, (DWORD)n, &put, NULL) : TRUE;
    CloseHandle(w);
    if (!ok || put != n) {
        CloseHandle(r);
        return false;
    }
    InitOnceExecuteOnce(&std_once, std_init, NULL, NULL);
    saved_stdin = *slot(0);
    *slot(0) = (WFd){.kind = WFD_PIPE, .h = r};
    saved_std = GetStdHandle(STD_INPUT_HANDLE);
    SetStdHandle(STD_INPUT_HANDLE, r);
    stdin_fed = true;
    return true;
}

void plat_stdin_restore(void) {
    if (!stdin_fed) return;
    CloseHandle(slot(0)->h);
    *slot(0) = saved_stdin;
    SetStdHandle(STD_INPUT_HANDLE, saved_std);
    stdin_fed = false;
}

bool plat_make_executable(const char *path) {
    (void)path; /* the name says it */
    return true;
}

char *plat_exe_path(const char *argv0) {
    wchar_t buf[32768];
    DWORD n = GetModuleFileNameW(NULL, buf, 32768);
    if (n > 0 && n < 32768) return u_str(buf);
    return argv0 ? plat_realpath(argv0) : NULL;
}

char plat_path_sep(void) { return ';'; }

/* ---- clocks -------------------------------------------------------------------------- */

int64_t plat_wall_ns(void) {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    return win_filetime_ns(ft);
}

int64_t plat_local_offset_s(int64_t secs) {
    TIME_ZONE_INFORMATION tz;
    (void)secs; /* the current rule, not the one in force at secs */
    DWORD r = GetTimeZoneInformation(&tz);
    LONG bias = tz.Bias + (r == TIME_ZONE_ID_DAYLIGHT ? tz.DaylightBias : 0);
    return -(int64_t)bias * 60;
}

uint64_t plat_now_ns(void) {
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    uint64_t f = (uint64_t)freq.QuadPart, t = (uint64_t)c.QuadPart;
    return t / f * 1000000000u + t % f * 1000000000u / f;
}

/* A high-resolution timer where there is one (Windows 10 1803 and later):
 * Sleep() rounds up to the scheduler's 15.6 ms tick. */
void plat_sleep_ns(uint64_t ns) {
    static MLWS_THREAD_LOCAL HANDLE timer;
    if (!timer) timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER due;
    due.QuadPart = -(LONGLONG)((ns + 99) / 100); /* relative, in 100 ns units */
    if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(timer, INFINITE);
    else Sleep((DWORD)((ns + 999999) / 1000000));
}

/* ---- the monitor's TCP (descriptors from the table) ------------------------------ */

static int socket_fd(SOCKET s) {
    int fd = wfd_new(WFD_SOCKET, NULL, s);
    if (fd < 0) closesocket(s);
    return fd;
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
    SOCKET s = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_NO_HANDLE_INHERIT);
    if (s == INVALID_SOCKET || bind(s, (struct sockaddr *)&a, sizeof a) != 0 || listen(s, 16) != 0) {
        snprintf(err, errlen, "cannot listen on %s:%d", host, port);
        if (s != INVALID_SOCKET) closesocket(s);
        return -1;
    }
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    return socket_fd(s);
}

int plat_tcp_accept(int listen_fd) {
    WFd *e = wfd_get(listen_fd);
    if (!e || e->kind != WFD_SOCKET) return -1;
    SOCKET c = accept(e->s, NULL, NULL);
    if (c == INVALID_SOCKET) return -1;
    u_long nb = 0;
    ioctlsocket(c, FIONBIO, &nb);
    return socket_fd(c);
}

/* A blocking connection to host (an address or a name: each of its addresses
 * in turn) and port, or -1. */
int plat_tcp_connect(const char *host, int port) {
    wsa_init();
    char ps[16];
    snprintf(ps, sizeof ps, "%d", port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, ps, &hints, &res) != 0) return -1;
    SOCKET s = INVALID_SOCKET;
    for (struct addrinfo *a = res; a && s == INVALID_SOCKET; a = a->ai_next) {
        s = WSASocketW(a->ai_family, a->ai_socktype, a->ai_protocol, NULL, 0, WSA_FLAG_NO_HANDLE_INHERIT);
        if (s != INVALID_SOCKET && connect(s, a->ai_addr, (int)a->ai_addrlen) != 0) closesocket(s), s = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    return s == INVALID_SOCKET ? -1 : socket_fd(s);
}

/* A pipe has no readiness to wait for: look at it every millisecond. */
bool plat_wait_readable(int fd, uint64_t timeout_ns) {
    WFd *e = wfd_get(fd);
    if (!e) return false;
    if (e->kind == WFD_SOCKET) {
        WSAPOLLFD pf = {e->s, POLLRDNORM, 0};
        return WSAPoll(&pf, 1, (INT)((timeout_ns + 999999) / 1000000)) > 0;
    }
    if (e->kind != WFD_PIPE) return true;
    uint64_t end = plat_now_ns() + timeout_ns;
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(e->h, NULL, 0, NULL, &avail, NULL) || avail) return true; /* data, or the end */
        if (plat_now_ns() >= end) return false;
        plat_sleep_ns(1000000);
    }
}

long plat_sock_read(int fd, void *buf, size_t n, uint64_t timeout_ns) {
    WFd *e = wfd_get(fd);
    if (!e || e->kind != WFD_SOCKET || !plat_wait_readable(fd, timeout_ns)) return -1;
    int r = recv(e->s, buf, (int)(n > INT_MAX ? INT_MAX : n), 0);
    return r < 0 ? -1 : (long)r;
}

bool plat_sock_write_all(int fd, const void *buf, size_t n) {
    WFd *e = wfd_get(fd);
    if (!e || e->kind != WFD_SOCKET) return false;
    const char *p = buf;
    while (n) {
        int w = send(e->s, p, (int)(n > INT_MAX ? INT_MAX : n), 0);
        if (w <= 0) return false;
        p += w;
        n -= (size_t)w;
    }
    return true;
}

void plat_sock_close(int fd) { plat_fd_close(fd); }

bool plat_is_tty(void) { return _isatty(1) != 0; }

void plat_stdio_init(void) {
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    SetConsoleOutputCP(CP_UTF8);
}

/* ---- threads --------------------------------------------------------------------------- */

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

/* The PlatThread struct is still read by thread_main; it is leaked deliberately. */
void plat_thread_detach(PlatThread *t) { CloseHandle(t->h); }

int plat_cpu_count(void) {
    DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return n > 0 ? (int)n : 1;
}

/* ---- memory for machine code: each piece has pages of its own, written, then
 * made executable ---- */

void *plat_code_reserve(size_t size) { return VirtualAlloc(NULL, size, MEM_RESERVE, PAGE_NOACCESS); }

size_t plat_code_align(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwPageSize;
}

bool plat_code_write(void *at, const void *src, size_t n) {
    size_t page = plat_code_align(), len = (n + page - 1) & ~(page - 1);
    DWORD old;
    if (!VirtualAlloc(at, len, MEM_COMMIT, PAGE_READWRITE)) return false;
    memcpy(at, src, n);
    if (!VirtualProtect(at, len, PAGE_EXECUTE_READ, &old)) return false;
    return FlushInstructionCache(GetCurrentProcess(), at, n);
}

void plat_code_free(void *region, size_t size) { (void)size, VirtualFree(region, 0, MEM_RELEASE); }

/* ---- shared libraries ------------------------------------------------------------------- */

static void last_error_text(char *buf, size_t n, DWORD e) {
    wchar_t w[512];
    DWORD k = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, e, 0, w, 512, NULL);
    while (k && (w[k - 1] == L'\r' || w[k - 1] == L'\n' || w[k - 1] == L'.' || w[k - 1] == L' ')) w[--k] = 0;
    if (!k) {
        snprintf(buf, n, "error %lu", (unsigned long)e);
        return;
    }
    char *u = u_str(w);
    snprintf(buf, n, "%s", u);
    free(u);
}

/* A path is made absolute, with backslashes, and the libraries it needs are
 * looked for next to it first; a bare name is looked for as LoadLibrary
 * does (the program's directory, the system's, then PATH). */
void *plat_dlopen(const char *path, char *err, size_t errlen) {
    if (!path) return (void *)GetModuleHandleW(NULL);
    wchar_t *w = w_str(path), full[32768];
    HMODULE h;
    if (strpbrk(path, "/\\:")) {
        DWORD n = GetFullPathNameW(w, 32768, full, NULL);
        h = n > 0 && n < 32768 ? LoadLibraryExW(full, NULL, LOAD_WITH_ALTERED_SEARCH_PATH) : NULL;
    } else {
        h = LoadLibraryW(w);
    }
    free(w);
    if (!h && err) {
        char text[400];
        last_error_text(text, sizeof text, GetLastError());
        snprintf(err, errlen, "%s: %s", path, text);
    }
    return (void *)h;
}

/* The program's own symbols (lib NULL) are its exports, then the C library's,
 * as dlsym(RTLD_DEFAULT) finds libc's on POSIX. */
void *plat_dlsym(void *lib, const char *name) {
    if (lib) return (void *)GetProcAddress((HMODULE)lib, name);
    static const wchar_t *const libs[] = {NULL, L"ucrtbase.dll", L"msvcrt.dll", L"kernel32.dll"};
    for (size_t i = 0; i < sizeof libs / sizeof libs[0]; i++) {
        HMODULE m = GetModuleHandleW(libs[i]);
        void *f = m ? (void *)GetProcAddress(m, name) : NULL;
        if (f) return f;
    }
    return NULL;
}

const char *plat_dl_suffix(void) { return ".dll"; }

/* ---- trust: owners and access control lists -------------------------------------------
 * A path is trusted if its owner is the current user or an administrator (the
 * Administrators group, SYSTEM or TrustedInstaller), and no one else may write
 * to it: no entry of its access control list allows writing to another
 * account. A directory made private, or whose writes are restricted, gets a
 * list of its own (not inherited) that allows only those. */

static bool sid_string(PSID sid, char *buf, size_t n) {
    wchar_t *w = NULL;
    if (!ConvertSidToStringSidW(sid, &w)) return false;
    char *u = u_str(w);
    snprintf(buf, n, "%s", u);
    free(u);
    LocalFree(w);
    return true;
}

static bool user_sid(char *buf, size_t n) {
    HANDLE tok;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    union {
        TOKEN_USER u;
        char space[256];
    } info;
    DWORD len;
    bool ok = GetTokenInformation(tok, TokenUser, &info, sizeof info, &len) && sid_string(info.u.User.Sid, buf, n);
    CloseHandle(tok);
    return ok;
}

static bool admin_sid(const char *s) {
    return !strcmp(s, "S-1-5-32-544") /* Administrators */ || !strcmp(s, "S-1-5-18") /* SYSTEM */ ||
           !strcmp(s, "S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464") /* TrustedInstaller */;
}

/* Rights that change a file or a directory's contents, or who may. */
#define WRITE_RIGHTS                                                                                       \
    (FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_DELETE_CHILD | DELETE | WRITE_DAC | WRITE_OWNER | GENERIC_WRITE | \
     GENERIC_ALL)

bool plat_path_trusted(const char *path, char *why, size_t whylen) {
    wchar_t *w = w_str(path);
    PSID owner = NULL;
    PACL dacl = NULL;
    PSECURITY_DESCRIPTOR sd = NULL;
    DWORD rc = GetNamedSecurityInfoW(w, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                     &owner, NULL, &dacl, NULL, &sd);
    free(w);
    if (rc != ERROR_SUCCESS) {
        char text[300];
        last_error_text(text, sizeof text, rc);
        snprintf(why, whylen, "%s: %s", path, text);
        return false;
    }
    char me[200] = "", who[200] = "";
    bool ok = user_sid(me, sizeof me) && sid_string(owner, who, sizeof who);
    if (ok && strcmp(who, me) && !admin_sid(who)) {
        snprintf(why, whylen, "%s is owned by another user (%s)", path, who);
        ok = false;
    } else if (ok && !dacl) {
        snprintf(why, whylen, "%s is writable by other users (it has no access control list)", path);
        ok = false;
    }
    for (DWORD i = 0; ok && i < dacl->AceCount; i++) {
        ACE_HEADER *ace;
        if (!GetAce(dacl, i, (void **)&ace) || ace->AceType != ACCESS_ALLOWED_ACE_TYPE) continue;
        if (ace->AceFlags & INHERIT_ONLY_ACE) continue; /* for what is made inside it, not for it */
        ACCESS_ALLOWED_ACE *a = (ACCESS_ALLOWED_ACE *)ace;
        if (!(a->Mask & WRITE_RIGHTS)) continue;
        char s[200];
        if (!sid_string((PSID)&a->SidStart, s, sizeof s) || !strcmp(s, me) || !strcmp(s, who) || admin_sid(s) ||
            !strcmp(s, "S-1-3-0") /* CREATOR OWNER */ || !strcmp(s, "S-1-3-4") /* OWNER RIGHTS */)
            continue;
        snprintf(why, whylen, "%s is writable by other users (%s)", path, s);
        ok = false;
    }
    if (!me[0]) snprintf(why, whylen, "%s: cannot tell who the current user is", path);
    LocalFree(sd);
    return ok;
}

/* Give a file or directory an access control list of its own: the current
 * user, SYSTEM and Administrators may do anything; other users may read (and
 * run, and list) it if `readable`. */
static bool set_private_acl(const char *path, bool readable) {
    char me[200], sddl[600];
    if (!user_sid(me, sizeof me)) return false;
    snprintf(sddl, sizeof sddl, "D:P(A;OICI;FA;;;%s)(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)%s", me,
             readable ? "(A;OICI;0x1200a9;;;BU)" : "");
    wchar_t *wsddl = w_str(sddl), *w = w_str(path);
    PSECURITY_DESCRIPTOR sd = NULL;
    PACL dacl = NULL;
    BOOL present = FALSE, defaulted = FALSE;
    bool ok = ConvertStringSecurityDescriptorToSecurityDescriptorW(wsddl, SDDL_REVISION_1, &sd, NULL) &&
              GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted) &&
              SetNamedSecurityInfoW(w, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                    NULL, NULL, dacl, NULL) == ERROR_SUCCESS;
    if (sd) LocalFree(sd);
    free(wsddl);
    free(w);
    return ok;
}

bool plat_mkdir(const char *path) {
    wchar_t *w = w_str(path);
    bool ok = CreateDirectoryW(w, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
    free(w);
    return ok;
}

/* Made private when it is made; one that exists already is left as it is
 * (plat_path_trusted then decides), as mkdir(path, 0700) does on POSIX. */
bool plat_mkdir_private(const char *path) {
    wchar_t *w = w_str(path);
    bool made = CreateDirectoryW(w, NULL);
    bool exists = made || GetLastError() == ERROR_ALREADY_EXISTS;
    free(w);
    return exists && (!made || set_private_acl(path, false));
}

bool plat_restrict_writes(const char *path) { return set_private_acl(path, true); }
#else
typedef int mlwsvm_win32_unused; /* ISO C forbids an empty translation unit */
#endif
