/* win32_io.c - files, pipes, child processes, sockets and the I/O poller on
 * Windows, for std.fs, std.os and std.net. Descriptors are entries of
 * win32.c's table; errors are Windows' and Winsock's codes, which
 * plat_err_code names for MLWSVM.
 *
 * Non-blocking pipes use PIPE_NOWAIT: a read of an empty pipe fails with
 * ERROR_NO_DATA, and a write takes what fits. The poller waits for sockets
 * with WSAPoll, and looks at the pipes it waits for every millisecond (an
 * anonymous pipe has no readiness to wait for). */
#ifdef _WIN32
#include "win32.h"

#include <bcrypt.h>
#include <ctype.h>
#include <mstcpip.h>
#include <mswsock.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

int plat_last_error(void) { return (int)GetLastError(); }

const char *plat_err_code(int err) {
    switch (err) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_INVALID_NAME: case ERROR_BAD_NETPATH:
    case ERROR_BAD_PATHNAME: case ERROR_MOD_NOT_FOUND: case ERROR_DIRECTORY:
    case WSAHOST_NOT_FOUND: case WSANO_DATA:
        return "E_NOT_FOUND";
    case ERROR_FILE_EXISTS: case ERROR_ALREADY_EXISTS: case ERROR_DIR_NOT_EMPTY:
        return "E_EXISTS";
    case ERROR_ACCESS_DENIED: case ERROR_SHARING_VIOLATION: case ERROR_LOCK_VIOLATION: case ERROR_WRITE_PROTECT:
    case ERROR_PRIVILEGE_NOT_HELD: case WSAEACCES:
        return "E_PERMISSION";
    case WERR_IS_DIR:
        return "E_IS_DIR";
    case ERROR_INVALID_HANDLE: case WSAENOTSOCK: case WSAEBADF:
        return "E_CLOSED";
    case WSAETIMEDOUT: case ERROR_TIMEOUT: case WAIT_TIMEOUT:
        return "E_TIMEOUT";
    case WSAECONNREFUSED: case ERROR_CONNECTION_REFUSED:
        return "E_REFUSED";
    case WSAECONNRESET: case WSAECONNABORTED: case WSAESHUTDOWN: case WSAENETRESET: case ERROR_BROKEN_PIPE:
    case ERROR_NO_DATA: case ERROR_PIPE_NOT_CONNECTED: case ERROR_NETNAME_DELETED:
        return "E_RESET";
    case WSAEADDRINUSE: case ERROR_ADDRESS_ALREADY_ASSOCIATED:
        return "E_ADDR_IN_USE";
    case ERROR_TOO_MANY_OPEN_FILES: case ERROR_NOT_ENOUGH_MEMORY: case ERROR_OUTOFMEMORY: case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL: case ERROR_NO_SYSTEM_RESOURCES: case WSAEMFILE: case WSAENOBUFS:
        return "E_RESOURCES";
    default:
        return "E_IO";
    }
}

const char *plat_err_text(int err) {
    static MLWS_THREAD_LOCAL char buf[400];
    if (err == WERR_IS_DIR) return "is a directory";
    wchar_t w[400];
    DWORD k = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, (DWORD)err,
                             MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), w, 400, NULL);
    if (!k) k = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, (DWORD)err, 0, w,
                               400, NULL);
    while (k && (w[k - 1] == L'\r' || w[k - 1] == L'\n' || w[k - 1] == L'.' || w[k - 1] == L' ')) w[--k] = 0;
    if (!k) {
        snprintf(buf, sizeof buf, "error %d", err);
        return buf;
    }
    char *u = u_str(w);
    snprintf(buf, sizeof buf, "%c%s", tolower((unsigned char)u[0]), u + 1); /* as strerror's are */
    free(u);
    return buf;
}

void plat_ignore_sigpipe(void) {} /* a write to a closed pipe fails; nothing is signalled */
void plat_raise_fd_limit(void) {} /* the table holds a million */

/* ---- descriptors ------------------------------------------------------------------- */

static int handle_fd(int kind, HANDLE h) {
    int fd = wfd_new(kind, h, INVALID_SOCKET);
    if (fd < 0) CloseHandle(h);
    return fd;
}

static int set_err(int *err, DWORD e) {
    *err = (int)e;
    return -1;
}

int plat_file_open(const char *path, int mode, int *err) {
    DWORD access = 0, disp;
    int acc = mode & PLAT_O_ACCESS;
    if (acc != PLAT_O_WRITE) access |= GENERIC_READ;
    if (acc != PLAT_O_READ) access |= mode & PLAT_O_APPEND ? FILE_APPEND_DATA | SYNCHRONIZE : GENERIC_WRITE;
    if (mode & PLAT_O_CREATE)
        disp = mode & PLAT_O_EXCLUSIVE ? CREATE_NEW : mode & PLAT_O_TRUNCATE ? CREATE_ALWAYS : OPEN_ALWAYS;
    else
        disp = mode & PLAT_O_TRUNCATE ? TRUNCATE_EXISTING : OPEN_EXISTING;
    if (disp == TRUNCATE_EXISTING) access |= GENERIC_WRITE;
    wchar_t *w = w_str(path);
    /* Sharing everything lets other handles read, write, rename and delete it,
     * as on POSIX. */
    HANDLE h = CreateFileW(w, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, disp,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD e = GetLastError();
    if (h == INVALID_HANDLE_VALUE && e == ERROR_ACCESS_DENIED) {
        DWORD a = GetFileAttributesW(w);
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) e = WERR_IS_DIR;
    }
    free(w);
    if (h == INVALID_HANDLE_VALUE) return set_err(err, e);
    int fd = handle_fd(WFD_FILE, h);
    if (fd < 0) *err = ERROR_TOO_MANY_OPEN_FILES;
    return fd;
}

/* A write to a non-blocking pipe takes nothing if it is larger than the
 * pipe's free space can ever be: write at most this much at once. */
#define PIPE_WRITE_MAX 16384

int plat_sock_dup(int fd, int *err) {
    WFd *e = wfd_get(fd);
    if (!e || e->kind != WFD_SOCKET) {
        *err = ERROR_INVALID_HANDLE;
        return -1;
    }
    WSAPROTOCOL_INFOW info;
    if (WSADuplicateSocketW(e->s, GetCurrentProcessId(), &info)) {
        *err = WSAGetLastError();
        return -1;
    }
    SOCKET s = WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, &info, 0, WSA_FLAG_OVERLAPPED);
    if (s == INVALID_SOCKET) {
        *err = WSAGetLastError();
        return -1;
    }
    int d = wfd_new(WFD_SOCKET, INVALID_HANDLE_VALUE, s);
    if (d < 0) {
        closesocket(s);
        *err = ERROR_TOO_MANY_OPEN_FILES;
    }
    return d;
}

int plat_fd_socket(int fd) {
    WFd *e = wfd_get(fd);
    return e ? (int)e->s : -1;
}

long plat_fd_read(int fd, void *buf, size_t n, int *err) {
    WFd *e = wfd_get(fd);
    if (!e) return set_err(err, ERROR_INVALID_HANDLE);
    if (n > 0x7fffffff) n = 0x7fffffff;
    if (e->kind == WFD_SOCKET) {
        int r = recv(e->s, buf, (int)n, 0);
        if (r >= 0) return r;
        int x = WSAGetLastError();
        if (x == WSAEWOULDBLOCK) return PLAT_IO_AGAIN;
        return set_err(err, (DWORD)x);
    }
    DWORD got = 0;
    if (ReadFile(e->h, buf, (DWORD)n, &got, NULL)) return (long)got;
    DWORD x = GetLastError();
    if (e->kind == WFD_PIPE && x == ERROR_NO_DATA) return PLAT_IO_AGAIN; /* empty, PIPE_NOWAIT */
    if (x == ERROR_BROKEN_PIPE || x == ERROR_HANDLE_EOF || x == ERROR_PIPE_NOT_CONNECTED) return 0;
    return set_err(err, x);
}

long plat_fd_write(int fd, const void *buf, size_t n, int *err) {
    WFd *e = wfd_get(fd);
    if (!e) return set_err(err, ERROR_INVALID_HANDLE);
    if (n > 0x7fffffff) n = 0x7fffffff;
    if (e->kind == WFD_SOCKET) {
        int r = send(e->s, buf, (int)n, 0);
        if (r >= 0) return r;
        int x = WSAGetLastError();
        if (x == WSAEWOULDBLOCK) return PLAT_IO_AGAIN;
        return set_err(err, (DWORD)x);
    }
    if (e->kind == WFD_PIPE && e->nonblock && n > PIPE_WRITE_MAX) n = PIPE_WRITE_MAX;
    DWORD put = 0;
    if (!WriteFile(e->h, buf, (DWORD)n, &put, NULL)) return set_err(err, GetLastError());
    if (put == 0 && n > 0 && e->kind == WFD_PIPE && e->nonblock) return PLAT_IO_AGAIN; /* full */
    return (long)put;
}

bool plat_fd_close(int fd) {
    WFd *e = wfd_get(fd);
    if (!e) {
        SetLastError(ERROR_INVALID_HANDLE);
        return false;
    }
    WFd c = *e;
    wfd_release(fd);
    if (c.kind == WFD_SOCKET) {
        if (closesocket(c.s) == 0) return true;
        SetLastError((DWORD)WSAGetLastError());
        return false;
    }
    return CloseHandle(c.h);
}

bool plat_fd_pollable(int fd) {
    WFd *e = wfd_get(fd);
    return e && (e->kind == WFD_PIPE || e->kind == WFD_SOCKET);
}

bool plat_fd_set_nonblocking(int fd, bool on) {
    WFd *e = wfd_get(fd);
    if (!e) return false;
    if (e->kind == WFD_SOCKET) {
        u_long nb = on;
        if (ioctlsocket(e->s, FIONBIO, &nb) != 0) return false;
    } else if (e->kind == WFD_PIPE) {
        DWORD m = PIPE_READMODE_BYTE | (on ? PIPE_NOWAIT : PIPE_WAIT);
        if (!SetNamedPipeHandleState(e->h, &m, NULL, NULL)) return false;
    } else {
        return false;
    }
    e->nonblock = on;
    return true;
}

int64_t plat_fd_seek(int fd, int64_t off, int whence, int *err) {
    WFd *e = wfd_get(fd);
    if (!e || e->kind == WFD_SOCKET) return set_err(err, ERROR_INVALID_HANDLE);
    LARGE_INTEGER d, r;
    d.QuadPart = off;
    if (!SetFilePointerEx(e->h, d, &r, whence == 1 ? FILE_CURRENT : whence == 2 ? FILE_END : FILE_BEGIN))
        return set_err(err, GetLastError());
    return r.QuadPart;
}

/* ---- the file system ------------------------------------------------------------------ */

bool plat_stat(const char *path, bool follow, PlatStat *out, int *err) {
    wchar_t *w = w_str(path);
    WIN32_FILE_ATTRIBUTE_DATA a;
    bool ok = GetFileAttributesExW(w, GetFileExInfoStandard, &a);
    DWORD e = GetLastError();
    bool link = ok && (a.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
    if (ok && link && follow) { /* what the link leads to */
        HANDLE h = CreateFileW(w, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                               OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
        BY_HANDLE_FILE_INFORMATION bi;
        ok = h != INVALID_HANDLE_VALUE && GetFileInformationByHandle(h, &bi);
        e = GetLastError();
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        if (ok) {
            a.dwFileAttributes = bi.dwFileAttributes;
            a.ftLastWriteTime = bi.ftLastWriteTime;
            a.nFileSizeHigh = bi.nFileSizeHigh;
            a.nFileSizeLow = bi.nFileSizeLow;
        }
        link = false;
    }
    free(w);
    if (!ok) {
        *err = (int)e;
        return false;
    }
    bool dir = a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
    out->size = dir ? 0 : (int64_t)((uint64_t)a.nFileSizeHigh << 32 | a.nFileSizeLow);
    out->mtime_ns = win_filetime_ns(a.ftLastWriteTime);
    /* No permission bits on Windows: what they would be. */
    out->mode = dir ? 0755 : a.dwFileAttributes & FILE_ATTRIBUTE_READONLY ? 0444 : 0644;
    out->kind = link ? PLAT_KIND_LINK : dir ? PLAT_KIND_DIR : PLAT_KIND_FILE;
    return true;
}

static bool path_op(BOOL ok, int *err) {
    if (!ok) *err = (int)GetLastError();
    return ok;
}

bool plat_remove(const char *path, int *err) {
    wchar_t *w = w_str(path);
    bool ok = path_op(DeleteFileW(w), err);
    if (!ok && *err == ERROR_ACCESS_DENIED) { /* a directory, as unlink says */
        DWORD a = GetFileAttributesW(w);
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) *err = WERR_IS_DIR;
    }
    free(w);
    return ok;
}

bool plat_rmdir(const char *path, int *err) {
    wchar_t *w = w_str(path);
    bool ok = path_op(RemoveDirectoryW(w), err);
    free(w);
    return ok;
}

/* Replaces what `to` names, as rename does on POSIX. */
bool plat_rename(const char *from, const char *to, int *err) {
    wchar_t *a = w_str(from), *b = w_str(to);
    bool ok = path_op(MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING), err);
    free(a);
    free(b);
    return ok;
}

bool plat_mkdir_err(const char *path, int *err) {
    wchar_t *w = w_str(path);
    bool ok = path_op(CreateDirectoryW(w, NULL), err);
    free(w);
    return ok;
}

char *plat_getcwd(void) {
    wchar_t buf[32768];
    DWORD n = GetCurrentDirectoryW(32768, buf);
    return n > 0 && n < 32768 ? u_str(buf) : NULL;
}

char *const *plat_environ(void) { return _environ; }

/* An empty value removes it, from the C library's copy and from the one
 * children inherit. */
void plat_unsetenv(const char *name) { _putenv_s(name, ""); }

const char *plat_os_name(void) { return "windows"; }

/* ---- child processes ----------------------------------------------------------------- */

/* The handles of the children started and not yet waited for, by process id. */
static SRWLOCK child_lock = SRWLOCK_INIT;
static VEC(struct Child { DWORD pid; HANDLE h; }) children;

static HANDLE child_handle(int64_t pid) {
    HANDLE h = NULL;
    AcquireSRWLockShared(&child_lock);
    for (size_t i = 0; i < children.n; i++)
        if (children.v[i].pid == (DWORD)pid) h = children.v[i].h;
    ReleaseSRWLockShared(&child_lock);
    return h;
}

/* One argument of a command line, quoted as the C runtime's parser expects:
 * backslashes are literal except before a quote. */
static void quote_arg(StrBuf *b, const char *a) {
    if (*a && !strpbrk(a, " \t\n\v\"")) {
        sb_puts(b, a);
        return;
    }
    sb_putc(b, '"');
    for (const char *p = a;; p++) {
        size_t k = 0;
        while (*p == '\\') p++, k++;
        if (!*p) {
            for (size_t i = 0; i < 2 * k; i++) sb_putc(b, '\\');
            break;
        }
        for (size_t i = 0; i < (*p == '"' ? 2 * k + 1 : k); i++) sb_putc(b, '\\');
        sb_putc(b, *p);
    }
    sb_putc(b, '"');
}

/* An inheritable copy of a handle, for a child (NULL if h is none). */
static HANDLE inheritable(HANDLE h) {
    HANDLE d = NULL;
    if (h && h != INVALID_HANDLE_VALUE)
        DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &d, 0, TRUE, DUPLICATE_SAME_ACCESS);
    return d;
}

bool plat_spawn(char *const argv[], int pipes, PlatChild *c, int *err) {
    /* The child's three handles (inheritable copies) and our ends of its pipes. */
    HANDLE child[3] = {NULL, NULL, NULL}, ours[3] = {NULL, NULL, NULL};
    static const DWORD which[3] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
    bool ok = true;
    for (int k = 0; k < 3 && ok; k++) {
        if (k == 2 && (pipes & PLAT_MERGE_ERR)) {
            child[2] = inheritable(child[1]);
            continue;
        }
        if (!(pipes & (1 << k))) {
            child[k] = inheritable(GetStdHandle(which[k]));
            continue;
        }
        HANDLE r, w;
        ok = CreatePipe(&r, &w, NULL, 65536);
        if (!ok) break;
        child[k] = inheritable(k == 0 ? r : w);
        ours[k] = k == 0 ? w : r;
        CloseHandle(k == 0 ? r : w);
        ok = child[k] != NULL;
    }
    DWORD e = GetLastError();
    StrBuf cmd = {0};
    for (size_t i = 0; argv[i]; i++) {
        if (i) sb_putc(&cmd, ' ');
        quote_arg(&cmd, argv[i]);
    }
    wchar_t *wcmd = w_str(cmd.s ? cmd.s : "");
    free(cmd.s);
    /* Only these three handles are inherited, whatever else other threads
     * have open and inheritable at this moment. */
    HANDLE list[3];
    int nlist = 0;
    for (int k = 0; k < 3; k++)
        if (child[k]) list[nlist++] = child[k];
    STARTUPINFOEXW si;
    memset(&si, 0, sizeof si);
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = child[0];
    si.StartupInfo.hStdOutput = child[1];
    si.StartupInfo.hStdError = child[2];
    SIZE_T asize = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &asize);
    si.lpAttributeList = xmalloc(asize);
    bool attrs = ok && InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &asize);
    ok = attrs && (!nlist || UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, list,
                                                         nlist * sizeof(HANDLE), NULL, NULL));
    PROCESS_INFORMATION pi;
    ok = ok && CreateProcessW(NULL, wcmd, NULL, NULL, nlist > 0, EXTENDED_STARTUPINFO_PRESENT, NULL, NULL,
                              &si.StartupInfo, &pi);
    if (!ok) e = GetLastError();
    if (attrs) DeleteProcThreadAttributeList(si.lpAttributeList);
    free(si.lpAttributeList);
    free(wcmd);
    for (int k = 0; k < 3; k++)
        if (child[k]) CloseHandle(child[k]);
    int fds[3] = {-1, -1, -1};
    for (int k = 0; k < 3; k++) {
        if (!ours[k]) continue;
        if (ok) fds[k] = handle_fd(WFD_PIPE, ours[k]);
        else CloseHandle(ours[k]);
    }
    if (!ok) {
        *err = (int)e;
        return false;
    }
    CloseHandle(pi.hThread);
    AcquireSRWLockExclusive(&child_lock);
    VEC_PUSH(children, ((struct Child){pi.dwProcessId, pi.hProcess}));
    ReleaseSRWLockExclusive(&child_lock);
    c->pid = pi.dwProcessId;
    c->in_fd = fds[0];
    c->out_fd = fds[1];
    c->err_fd = fds[2];
    return true;
}

bool plat_wait_child(int64_t pid, int *code, int *err) {
    HANDLE h = child_handle(pid);
    if (!h) return set_err(err, ERROR_INVALID_PARAMETER), false;
    DWORD x = 255;
    bool ok = WaitForSingleObject(h, INFINITE) == WAIT_OBJECT_0 && GetExitCodeProcess(h, &x);
    if (!ok) return set_err(err, GetLastError()), false;
    AcquireSRWLockExclusive(&child_lock);
    for (size_t i = 0; i < children.n; i++)
        if (children.v[i].pid == (DWORD)pid) {
            children.v[i] = children.v[--children.n];
            break;
        }
    ReleaseSRWLockExclusive(&child_lock);
    CloseHandle(h);
    *code = (int)x;
    return true;
}

/* Windows has no signals: the child is ended, with the code a POSIX shell
 * reports for the signal (128 + N). */
bool plat_signal_child(int64_t pid, int sig) {
    HANDLE h = child_handle(pid);
    return h && TerminateProcess(h, sig == 9 ? 128 + 9 : 128 + 15);
}

/* ---- sockets ---------------------------------------------------------------------------- */

static int sock_err(int *err) {
    *err = WSAGetLastError();
    return -1;
}

static int sock_fd(SOCKET s, int *err) {
    int fd = wfd_new(WFD_SOCKET, NULL, s);
    if (fd < 0) {
        closesocket(s);
        *err = WSAEMFILE;
    }
    return fd;
}

bool plat_host_numeric(const char *host) {
    unsigned char buf[16];
    return !host || !*host || inet_pton(AF_INET, host, buf) == 1 || inet_pton(AF_INET6, host, buf) == 1;
}

static bool resolve(const char *host, int port, int type, bool passive, struct addrinfo **res, int *err) {
    wsa_init();
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = type;
    hints.ai_flags = AI_NUMERICSERV | (passive ? AI_PASSIVE : 0);
    char ps[16];
    snprintf(ps, sizeof ps, "%d", port);
    int rc = getaddrinfo(host && *host ? host : NULL, ps, &hints, res);
    if (rc == 0) return true;
    *err = rc; /* a Winsock error code */
    return false;
}

static SOCKET new_socket(const struct addrinfo *a) {
    return WSASocketW(a->ai_family, a->ai_socktype, a->ai_protocol, NULL, 0, WSA_FLAG_NO_HANDLE_INHERIT);
}

static void set_nonblock(SOCKET s) {
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
}

static void set_nodelay(SOCKET s) {
    BOOL one = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
}

/* Windows lets a port in TIME_WAIT be bound again by default; SO_REUSEADDR
 * would let another socket take a port in use. */
int plat_sock_listen(const char *host, int port, int *err) {
    struct addrinfo *res;
    if (!resolve(host, port, SOCK_STREAM, true, &res, err)) return -1;
    SOCKET s = INVALID_SOCKET;
    for (struct addrinfo *a = res; a && s == INVALID_SOCKET; a = a->ai_next) {
        s = new_socket(a);
        if (s == INVALID_SOCKET) {
            sock_err(err);
            continue;
        }
        BOOL one = TRUE;
        setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&one, sizeof one);
        if (bind(s, a->ai_addr, (int)a->ai_addrlen) != 0 || listen(s, SOMAXCONN) != 0) {
            sock_err(err);
            closesocket(s);
            s = INVALID_SOCKET;
        }
    }
    freeaddrinfo(res);
    if (s == INVALID_SOCKET) return -1;
    set_nonblock(s);
    return sock_fd(s, err);
}

int plat_sock_accept(int lfd, int *err) {
    WFd *e = wfd_get(lfd);
    if (!e || e->kind != WFD_SOCKET) return set_err(err, WSAENOTSOCK), PLAT_IO_ERR;
    for (;;) {
        SOCKET c = accept(e->s, NULL, NULL);
        if (c != INVALID_SOCKET) {
            SetHandleInformation((HANDLE)c, HANDLE_FLAG_INHERIT, 0);
            set_nonblock(c);
            set_nodelay(c);
            int fd = sock_fd(c, err);
            return fd < 0 ? PLAT_IO_ERR : fd;
        }
        int x = WSAGetLastError();
        if (x == WSAECONNRESET || x == WSAEINTR) continue; /* gone before it was accepted */
        if (x == WSAEWOULDBLOCK) return PLAT_IO_AGAIN;
        *err = x;
        return PLAT_IO_ERR;
    }
}

static bool is_loopback(const struct sockaddr *sa) {
    if (sa->sa_family == AF_INET) return (ntohl(((const struct sockaddr_in *)sa)->sin_addr.s_addr) >> 24) == 127;
    return sa->sa_family == AF_INET6 && IN6_IS_ADDR_LOOPBACK(&((const struct sockaddr_in6 *)sa)->sin6_addr);
}

/* Windows answers a reset to its SYN by sending the SYN again, twice, so a
 * refused connection takes 2 s to fail. On the loopback interface nothing is
 * lost: send it once. */
static void no_syn_retries(SOCKET s) {
    TCP_INITIAL_RTO_PARAMETERS rto = {TCP_INITIAL_RTO_UNSPECIFIED_RTT, TCP_INITIAL_RTO_NO_SYN_RETRANSMISSIONS};
    DWORD got;
    WSAIoctl(s, SIO_TCP_INITIAL_RTO, &rto, sizeof rto, NULL, 0, &got, NULL, NULL);
}

/* Blocking (run on the I/O pool): resolve, then try each address in turn. */
int plat_sock_connect(const char *host, int port, int64_t timeout_ms, int *err) {
    struct addrinfo *res;
    if (!resolve(host, port, SOCK_STREAM, false, &res, err)) return -1;
    SOCKET s = INVALID_SOCKET;
    for (struct addrinfo *a = res; a && s == INVALID_SOCKET; a = a->ai_next) {
        s = new_socket(a);
        if (s == INVALID_SOCKET) {
            sock_err(err);
            continue;
        }
        set_nonblock(s);
        if (is_loopback(a->ai_addr)) no_syn_retries(s);
        int x = 0;
        if (connect(s, a->ai_addr, (int)a->ai_addrlen) != 0) {
            x = WSAGetLastError();
            if (x == WSAEWOULDBLOCK) {
                /* select reports a failed connect in the exception set, which
                 * WSAPoll does not do reliably. */
                fd_set wr, ex;
                FD_ZERO(&wr);
                FD_ZERO(&ex);
                FD_SET(s, &wr);
                FD_SET(s, &ex);
                struct timeval tv = {(long)(timeout_ms / 1000), (long)(timeout_ms % 1000) * 1000};
                int n = select(0, NULL, &wr, &ex, timeout_ms < 0 ? NULL : &tv);
                if (n == 0) x = WSAETIMEDOUT;
                else if (n < 0) x = WSAGetLastError();
                else if (FD_ISSET(s, &ex)) {
                    int len = sizeof x;
                    getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&x, &len);
                    if (!x) x = WSAECONNREFUSED;
                } else x = 0;
            }
        }
        if (x) {
            *err = x;
            closesocket(s);
            s = INVALID_SOCKET;
        }
    }
    freeaddrinfo(res);
    if (s == INVALID_SOCKET) return -1;
    set_nodelay(s);
    return sock_fd(s, err);
}

static SOCKET sock_of(int fd) {
    WFd *e = wfd_get(fd);
    return e && e->kind == WFD_SOCKET ? e->s : INVALID_SOCKET;
}

int plat_sock_port(int fd) {
    struct sockaddr_storage ss;
    int len = sizeof ss;
    if (getsockname(sock_of(fd), (struct sockaddr *)&ss, &len) != 0) return -1;
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
    int len = sizeof ss;
    if (getpeername(sock_of(fd), (struct sockaddr *)&ss, &len) != 0) return false;
    format_addr((struct sockaddr *)&ss, buf, n, port);
    return true;
}

bool plat_sock_shutdown_write(int fd) {
    if (shutdown(sock_of(fd), SD_SEND) == 0) return true;
    SetLastError((DWORD)WSAGetLastError());
    return false;
}

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
    SOCKET s = INVALID_SOCKET;
    for (struct addrinfo *a = res; a && s == INVALID_SOCKET; a = a->ai_next) {
        s = new_socket(a);
        if (s == INVALID_SOCKET) {
            sock_err(err);
            continue;
        }
        if (bind(s, a->ai_addr, (int)a->ai_addrlen) != 0) {
            sock_err(err);
            closesocket(s);
            s = INVALID_SOCKET;
        }
    }
    freeaddrinfo(res);
    if (s == INVALID_SOCKET) return -1;
    /* A datagram to a port no one listens on would otherwise make the next
     * receive fail with WSAECONNRESET. */
    BOOL off = FALSE;
    DWORD got;
    WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof off, NULL, 0, &got, NULL, NULL);
    set_nonblock(s);
    return sock_fd(s, err);
}

long plat_udp_send(int fd, const char *host, int port, const void *buf, size_t n, int *err) {
    struct addrinfo *res;
    if (!resolve(host, port, SOCK_DGRAM, false, &res, err)) return PLAT_IO_ERR;
    long r = PLAT_IO_ERR;
    int s = sendto(sock_of(fd), buf, (int)n, 0, res->ai_addr, (int)res->ai_addrlen);
    if (s >= 0) r = s;
    else if (WSAGetLastError() == WSAEWOULDBLOCK) r = PLAT_IO_AGAIN;
    else *err = WSAGetLastError();
    freeaddrinfo(res);
    return r;
}

long plat_udp_recv(int fd, void *buf, size_t n, char *addr, size_t alen, int *port, int *err) {
    struct sockaddr_storage ss;
    int len = sizeof ss;
    int r = recvfrom(sock_of(fd), buf, (int)n, 0, (struct sockaddr *)&ss, &len);
    if (r >= 0 || WSAGetLastError() == WSAEMSGSIZE) { /* a datagram larger than buf: its start */
        format_addr((struct sockaddr *)&ss, addr, alen, port);
        return r >= 0 ? r : (long)n;
    }
    if (WSAGetLastError() == WSAEWOULDBLOCK) return PLAT_IO_AGAIN;
    *err = WSAGetLastError();
    return PLAT_IO_ERR;
}

bool plat_random_bytes(void *buf, size_t n) {
    while (n) {
        ULONG k = n > 0x40000000 ? 0x40000000 : (ULONG)n;
        if (!BCRYPT_SUCCESS(BCryptGenRandom(NULL, buf, k, BCRYPT_USE_SYSTEM_PREFERRED_RNG))) return false;
        buf = (char *)buf + k;
        n -= k;
    }
    return true;
}

/* ---- the poller ----------------------------------------------------------------------
 * The waits armed (one-shot), under a lock, with an index from descriptor to
 * wait. plat_poller_wait polls the sockets among them with WSAPoll, and one
 * end of a loopback TCP connection of its own, to whose other end
 * plat_poller_wake writes a byte. (Not one UDP socket that sends to itself:
 * a send on a socket another thread is in WSAPoll with does not return.)
 * While it waits for a pipe, it wakes every millisecond to look at it. */

#define PIPE_LOOK_MS 1

typedef struct {
    int fd;
    int events;
    uint64_t token;
    bool seen; /* in the set the last wait polled (a pipe waited for writing: it is ready after one look) */
} Arm;

struct PlatPoller {
    SRWLOCK lock;
    VEC(Arm) arms;
    VEC(uint32_t) at; /* by descriptor: 1 + its wait's index in arms, or 0 */
    SOCKET wake, wake_tx; /* the connection's ends: polled, written */
    volatile LONG woken;
    VEC(WSAPOLLFD) pfd; /* the set being polled (the wait's: one thread waits) */
    VEC(int) pfd_fd;
};

/* A connected pair of loopback TCP sockets (Windows has no socketpair). */
static bool loopback_pair(SOCKET *a, SOCKET *b) {
    SOCKET l = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_NO_HANDLE_INHERIT);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int len = sizeof addr;
    *a = *b = INVALID_SOCKET;
    if (l != INVALID_SOCKET && bind(l, (struct sockaddr *)&addr, len) == 0 && listen(l, 1) == 0 &&
        getsockname(l, (struct sockaddr *)&addr, &len) == 0) {
        *b = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_NO_HANDLE_INHERIT);
        if (*b != INVALID_SOCKET && connect(*b, (struct sockaddr *)&addr, len) == 0) *a = accept(l, NULL, NULL);
    }
    if (l != INVALID_SOCKET) closesocket(l);
    if (*a == INVALID_SOCKET) {
        if (*b != INVALID_SOCKET) closesocket(*b);
        return false;
    }
    set_nonblock(*a);
    set_nonblock(*b);
    set_nodelay(*b);
    return true;
}

PlatPoller *plat_poller_new(void) {
    wsa_init();
    PlatPoller *p = xcalloc(1, sizeof *p);
    InitializeSRWLock(&p->lock);
    if (!loopback_pair(&p->wake, &p->wake_tx)) {
        free(p);
        return NULL;
    }
    return p;
}

void plat_poller_free(PlatPoller *p) {
    if (!p) return;
    closesocket(p->wake);
    closesocket(p->wake_tx);
    VEC_FREE(p->arms);
    VEC_FREE(p->at);
    VEC_FREE(p->pfd);
    VEC_FREE(p->pfd_fd);
    free(p);
}

/* The index of fd's wait, or -1. The lock is held. */
static long arm_find(PlatPoller *p, int fd) {
    return fd >= 0 && (size_t)fd < p->at.n ? (long)p->at.v[fd] - 1 : -1;
}

static void arm_remove(PlatPoller *p, size_t i) {
    p->at.v[p->arms.v[i].fd] = 0;
    p->arms.v[i] = p->arms.v[--p->arms.n];
    if (i < p->arms.n) p->at.v[p->arms.v[i].fd] = (uint32_t)i + 1;
}

bool plat_poller_arm(PlatPoller *p, int fd, int events, uint64_t token) {
    WFd *e = wfd_get(fd);
    if (!e || (e->kind != WFD_SOCKET && e->kind != WFD_PIPE)) return false;
    AcquireSRWLockExclusive(&p->lock);
    long i = arm_find(p, fd);
    if (i < 0) {
        while (p->at.n <= (size_t)fd) VEC_PUSH(p->at, 0);
        VEC_PUSH(p->arms, ((Arm){0}));
        i = (long)p->arms.n - 1;
        p->at.v[fd] = (uint32_t)i + 1;
    }
    p->arms.v[i] = (Arm){fd, events, token, false};
    ReleaseSRWLockExclusive(&p->lock);
    plat_poller_wake(p); /* so a wait in progress polls it too */
    return true;
}

void plat_poller_forget(PlatPoller *p, int fd) {
    AcquireSRWLockExclusive(&p->lock);
    long i = arm_find(p, fd);
    if (i >= 0) arm_remove(p, (size_t)i);
    ReleaseSRWLockExclusive(&p->lock);
    if (i >= 0) plat_poller_wake(p); /* stop polling the socket before it is closed */
}

static bool pipe_ready(const Arm *a, HANDLE h) {
    if (a->events & PLAT_POLL_WRITE) return a->seen;
    DWORD avail = 0;
    return !PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL) || avail > 0; /* data, or the other end closed */
}

/* A descriptor closed while it was waited for is reported ready: whoever
 * waited finds out. */
int plat_poller_wait(PlatPoller *p, uint64_t *tokens, int max, int64_t timeout_ns) {
    uint64_t start = plat_now_ns();
    int k = 0;
    for (;;) {
        p->pfd.n = p->pfd_fd.n = 0;
        VEC_PUSH(p->pfd, ((WSAPOLLFD){p->wake, POLLRDNORM, 0}));
        VEC_PUSH(p->pfd_fd, -1);
        bool pipes = false;
        AcquireSRWLockExclusive(&p->lock);
        for (size_t i = 0; i < p->arms.n; i++) {
            Arm *a = &p->arms.v[i];
            WFd *e = wfd_get(a->fd);
            if (e && e->kind == WFD_SOCKET) {
                VEC_PUSH(p->pfd, ((WSAPOLLFD){e->s, (short)(a->events & PLAT_POLL_WRITE ? POLLWRNORM : POLLRDNORM), 0}));
                VEC_PUSH(p->pfd_fd, a->fd);
            } else {
                pipes = true; /* (or closed) */
                a->seen = true;
            }
        }
        ReleaseSRWLockExclusive(&p->lock);
        int64_t left = timeout_ns < 0 ? -1 : timeout_ns - (int64_t)(plat_now_ns() - start);
        int ms = timeout_ns < 0 ? -1 : left <= 0 ? 0 : (int)((left + 999999) / 1000000);
        if (pipes && (ms < 0 || ms > PIPE_LOOK_MS)) ms = PIPE_LOOK_MS;
        int n = WSAPoll(p->pfd.v, (ULONG)p->pfd.n, ms);
        bool woken = n > 0 && p->pfd.v[0].revents;
        /* Drained, then cleared: a wake-up before the clear armed a wait the
         * next wait's set will have; one after it sends another byte. (The
         * other order loses a byte sent between the two, and with it every
         * later wake-up.) */
        if (woken) {
            char drain[64];
            while (recv(p->wake, drain, sizeof drain, 0) > 0) {
            }
            InterlockedExchange(&p->woken, 0);
        }
        AcquireSRWLockExclusive(&p->lock);
        for (size_t j = 1; n > 0 && j < p->pfd.n && k < max; j++) {
            if (!p->pfd.v[j].revents) continue;
            long i = arm_find(p, p->pfd_fd.v[j]);
            WFd *e = wfd_get(p->pfd_fd.v[j]);
            if (i < 0 || !e || e->kind != WFD_SOCKET || e->s != p->pfd.v[j].fd) continue; /* since forgotten */
            tokens[k++] = p->arms.v[i].token;
            arm_remove(p, (size_t)i);
        }
        for (size_t i = 0; pipes && i < p->arms.n && k < max;) {
            Arm *a = &p->arms.v[i];
            WFd *e = wfd_get(a->fd);
            if (e && e->kind == WFD_SOCKET) {
                i++;
                continue;
            }
            if (e && !pipe_ready(a, e->h)) {
                i++;
                continue;
            }
            tokens[k++] = a->token;
            arm_remove(p, i);
        }
        ReleaseSRWLockExclusive(&p->lock);
        bool out_of_time = timeout_ns >= 0 && (int64_t)(plat_now_ns() - start) >= timeout_ns;
        if (k || woken || out_of_time || n < 0) break;
    }
    return k;
}

void plat_poller_wake(PlatPoller *p) {
    if (InterlockedExchange(&p->woken, 1)) return; /* a wake-up is pending anyway */
    char b = 1;
    send(p->wake_tx, &b, 1, 0);
}
#else
typedef int mlwsvm_win32_io_unused; /* ISO C forbids an empty translation unit */
#endif
