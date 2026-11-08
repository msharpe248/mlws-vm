/* win32_io.c - files, pipes, child processes, sockets and the I/O poller on
 * Windows, for std.fs, std.os and std.net. Descriptors are entries of
 * win32.c's table; errors are Windows' and Winsock's codes, which
 * plat_err_code names for MLWSVM.
 *
 * Non-blocking pipes use PIPE_NOWAIT: a read of an empty pipe fails with
 * ERROR_NO_DATA, and a write takes what fits. Our end of a child's output is
 * instead a named pipe opened for overlapped I/O (WFd.ovl), as libuv makes
 * them: a read looks first (PeekNamedPipe) if it must not wait. The poller
 * waits for sockets on an I/O completion port, through the AFD driver's poll
 * requests, and for overlapped pipes there too, by a zero-byte read, which
 * completes once there is data or the other end has closed; it looks at
 * other pipes (an anonymous pipe, such as a stdin given to us, has no
 * readiness to wait for), and pipes waited for writing, every millisecond. */
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
    case WERR_NO_PORT:
        return "E_RESOURCES";
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
    if (err == WERR_NO_PORT) return NO_PORT_TEXT;
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
    SetHandleInformation((HANDLE)s, HANDLE_FLAG_INHERIT, 0);
    int d = wfd_new(WFD_SOCKET, NULL, s);
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

/* An event for this thread's overlapped reads, made once. Its handle's low
 * bit, set in an OVERLAPPED, keeps a read's completion off a port. */
static HANDLE read_event(void) {
    static MLWS_THREAD_LOCAL HANDLE ev;
    if (!ev) ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    return ev;
}

static bool pipe_gone(DWORD x) { return x == ERROR_BROKEN_PIPE || x == ERROR_HANDLE_EOF || x == ERROR_PIPE_NOT_CONNECTED; }

/* A read of an overlapped pipe: one that must not wait reads only what is
 * there. */
static long ovl_read(WFd *e, void *buf, size_t n, int *err) {
    if (e->nonblock) {
        DWORD avail = 0;
        if (!PeekNamedPipe(e->h, NULL, 0, NULL, &avail, NULL)) {
            DWORD x = GetLastError();
            return pipe_gone(x) ? 0 : set_err(err, x);
        }
        if (!avail) return PLAT_IO_AGAIN;
        if (n > avail) n = avail;
    }
    HANDLE ev = read_event();
    if (!ev) return set_err(err, GetLastError());
    OVERLAPPED ov;
    memset(&ov, 0, sizeof ov);
    ov.hEvent = (HANDLE)((uintptr_t)ev | 1);
    DWORD got = 0;
    if (!ReadFile(e->h, buf, (DWORD)n, NULL, &ov)) {
        DWORD x = GetLastError();
        if (x != ERROR_IO_PENDING) return pipe_gone(x) ? 0 : set_err(err, x);
        WaitForSingleObject(ev, INFINITE);
    }
    if (!GetOverlappedResult(e->h, &ov, &got, FALSE)) {
        DWORD x = GetLastError();
        return pipe_gone(x) ? 0 : set_err(err, x);
    }
    return (long)got;
}

long plat_fd_read(int fd, void *buf, size_t n, int *err) {
    WFd *e = wfd_get(fd);
    if (!e) return set_err(err, ERROR_INVALID_HANDLE);
    if (n > 0x7fffffff) n = 0x7fffffff;
    if (e->kind == WFD_PIPE && e->ovl) return ovl_read(e, buf, n, err);
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
    if (pipe_gone(x)) return 0;
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
    } else if (e->kind == WFD_PIPE && e->ovl) {
        /* (an overlapped pipe stays PIPE_WAIT: a read that must not wait looks first) */
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

/* A pipe for a child's output: our end, read, a named pipe opened for
 * overlapped I/O; the child's, written, a plain handle (as libuv's
 * uv_spawn makes them). The name is ours alone (FILE_FLAG_FIRST_PIPE_INSTANCE),
 * and the pipe takes no other client. */
static bool output_pipe(HANDLE *ours, HANDLE *theirs) {
    static volatile LONG serial;
    for (int tries = 0; tries < 8; tries++) {
        char narrow[96];
        snprintf(narrow, sizeof narrow, "\\\\.\\pipe\\mlwsvm-%lu-%ld", (unsigned long)GetCurrentProcessId(),
                 (long)InterlockedIncrement(&serial));
        wchar_t *name = w_str(narrow);
        HANDLE s = CreateNamedPipeW(name, PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536,
                                    65536, 0, NULL);
        if (s == INVALID_HANDLE_VALUE) {
            DWORD x = GetLastError();
            free(name);
            if (x == ERROR_ACCESS_DENIED || x == ERROR_PIPE_BUSY) continue; /* (the name is taken) */
            SetLastError(x);
            return false;
        }
        HANDLE c = CreateFileW(name, GENERIC_WRITE | FILE_READ_ATTRIBUTES, 0, NULL, OPEN_EXISTING, 0, NULL);
        free(name);
        if (c == INVALID_HANDLE_VALUE) {
            DWORD x = GetLastError();
            CloseHandle(s);
            SetLastError(x);
            return false;
        }
        OVERLAPPED ov;
        memset(&ov, 0, sizeof ov);
        ov.hEvent = (HANDLE)((uintptr_t)read_event() | 1);
        if (!ConnectNamedPipe(s, &ov) && GetLastError() != ERROR_PIPE_CONNECTED) { /* (connected already: the client is there) */
            DWORD x = GetLastError();
            if (x == ERROR_IO_PENDING) CancelIoEx(s, &ov), GetOverlappedResult(s, &ov, &(DWORD){0}, TRUE);
            CloseHandle(c);
            CloseHandle(s);
            SetLastError(x);
            return false;
        }
        *ours = s;
        *theirs = c;
        return true;
    }
    return false;
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
        ok = k == 0 ? CreatePipe(&r, &w, NULL, 65536) : output_pipe(&r, &w);
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
        WFd *pe = fds[k] >= 0 ? wfd_get(fds[k]) : NULL;
        if (pe && k > 0) pe->ovl = true;
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
        BOOL one = TRUE, off = FALSE;
        setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&one, sizeof one);
        /* "" is every address: IPv6's takes IPv4 too only if told, here. */
        if (a->ai_family == AF_INET6) setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&off, sizeof off);
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
            /* An unbound socket's connect: no local port was free. */
            *err = x == WSAEADDRINUSE ? WERR_NO_PORT : x;
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
 * One I/O completion port. A wait for a socket is a poll request to the AFD
 * driver, the kernel side of Winsock, which completes on the port once the
 * socket is ready: what libuv's uv_poll, wepoll and Rust's mio do, through
 * ntdll's NtDeviceIoControlFile, which Windows does not document but has kept
 * since NT. So a socket waited for costs nothing until it is ready, however
 * many there are. plat_poller_wake posts a completion of its own. An
 * overlapped pipe (our end of a child's output) waited for reading is a
 * zero-byte read on the port (PipeReq), which completes once there is data or
 * the other end has closed, as libuv waits for its pipes. Other pipes have no
 * readiness to wait for (an anonymous pipe cannot be overlapped), and a pipe
 * waited for writing is ready after one look: while one is waited for, the
 * wait wakes every millisecond to look at it.
 *
 * The waits armed (one-shot) are kept under a lock, with an index from
 * descriptor to wait. A request (SockPoll, PipeReq) lives until its
 * completion is taken from the port, even if its wait is forgotten or replaced
 * first: then it is cancelled, and freed when that completion comes. */

#include <winternl.h>

#define PIPE_LOOK_MS 1
#define WAKE_KEY 1
#define AFD_KEY 2
#define PIPE_KEY 3
#ifndef FILE_OPEN
#define FILE_OPEN 0x00000001
#endif
#ifndef SIO_BASE_HANDLE
#define SIO_BASE_HANDLE _WSAIOR(IOC_WS2, 34)
#endif

/* The AFD driver's poll request (as wepoll and mio define it). */
#define IOCTL_AFD_POLL 0x00012024
#define AFD_POLL_RECEIVE 0x0001
#define AFD_POLL_SEND 0x0004
#define AFD_POLL_DISCONNECT 0x0008
#define AFD_POLL_ABORT 0x0010
#define AFD_POLL_LOCAL_CLOSE 0x0020
#define AFD_POLL_ACCEPT 0x0080
#define AFD_POLL_CONNECT_FAIL 0x0100
typedef struct {
    HANDLE handle;
    ULONG events;
    NTSTATUS status;
} AfdPollHandle;
typedef struct {
    LARGE_INTEGER timeout;
    ULONG nhandles;
    ULONG exclusive;
    AfdPollHandle handles[1];
} AfdPollInfo;

typedef NTSTATUS(NTAPI *NtCreateFileFn)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER,
                                        ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
typedef NTSTATUS(NTAPI *NtDeviceIoControlFileFn)(HANDLE, HANDLE, PVOID, PVOID, PIO_STATUS_BLOCK, ULONG, PVOID, ULONG,
                                                 PVOID, ULONG);
typedef NTSTATUS(NTAPI *NtCancelIoFileExFn)(HANDLE, PIO_STATUS_BLOCK, PIO_STATUS_BLOCK);
static struct {
    NtCreateFileFn create;
    NtDeviceIoControlFileFn ioctl;
    NtCancelIoFileExFn cancel;
} nt;

static BOOL CALLBACK nt_load(PINIT_ONCE once, PVOID arg, PVOID *ctx) {
    (void)once, (void)arg, (void)ctx;
    HMODULE m = GetModuleHandleW(L"ntdll.dll");
    if (!m) return TRUE;
    nt.create = (NtCreateFileFn)(void (*)(void))GetProcAddress(m, "NtCreateFile");
    nt.ioctl = (NtDeviceIoControlFileFn)(void (*)(void))GetProcAddress(m, "NtDeviceIoControlFile");
    nt.cancel = (NtCancelIoFileExFn)(void (*)(void))GetProcAddress(m, "NtCancelIoFileEx");
    return TRUE;
}

typedef struct {
    IO_STATUS_BLOCK iosb; /* the request's: its completion's lpOverlapped is &iosb */
    AfdPollInfo info;
    int fd;
    bool live; /* still its wait's (not forgotten, replaced or ended) */
} SockPoll;

typedef struct {
    OVERLAPPED ov; /* the zero-byte read's: its completion's lpOverlapped is &ov */
    HANDLE h;
    int fd;
    bool live; /* still its wait's */
} PipeReq;

typedef struct {
    int fd;
    int events;
    uint64_t token;
    bool pipe;    /* a pipe looked at every millisecond */
    bool seen;    /* such a pipe: looked at once already (one waited for writing is ready after one look) */
    SockPoll *sp; /* a socket: its request, until its completion is taken */
    PipeReq *pr;  /* an overlapped pipe: its zero-byte read, until its completion is taken */
} Arm;

struct PlatPoller {
    SRWLOCK lock;
    VEC(Arm) arms;
    VEC(uint32_t) at; /* by descriptor: 1 + its wait's index in arms, or 0 */
    size_t pipes;     /* how many of the waits are for pipes */
    size_t pending;   /* requests not yet taken from the port */
    HANDLE iocp, afd;
    volatile LONG woken;
};

/* The driver's handle to give a socket's poll: its base, beneath any layered
 * provider. */
static HANDLE base_socket(WFd *e) {
    if (!e->base) {
        SOCKET b = INVALID_SOCKET;
        DWORD n = 0;
        if (WSAIoctl(e->s, SIO_BASE_HANDLE, NULL, 0, &b, sizeof b, &n, NULL, NULL) != 0 || b == INVALID_SOCKET) b = e->s;
        e->base = b;
    }
    return (HANDLE)e->base;
}

/* A handle on the AFD driver, with its completions on the port. */
static HANDLE afd_open(HANDLE iocp) {
    static const WCHAR name[] = L"\\Device\\Afd\\Mlwsvm";
    UNICODE_STRING us = {.Length = sizeof name - sizeof(WCHAR), .MaximumLength = sizeof name, .Buffer = (PWSTR)name};
    OBJECT_ATTRIBUTES oa = {.Length = sizeof oa, .ObjectName = &us};
    IO_STATUS_BLOCK iosb;
    HANDLE h = NULL;
    if (nt.create(&h, SYNCHRONIZE, &oa, &iosb, NULL, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN, 0, NULL, 0) != 0)
        return NULL;
    if (!CreateIoCompletionPort(h, iocp, AFD_KEY, 0)) {
        CloseHandle(h);
        return NULL;
    }
    SetFileCompletionNotificationModes(h, FILE_SKIP_SET_EVENT_ON_HANDLE);
    return h;
}

PlatPoller *plat_poller_new(void) {
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    wsa_init();
    InitOnceExecuteOnce(&once, nt_load, NULL, NULL);
    if (!nt.create || !nt.ioctl || !nt.cancel) return NULL;
    PlatPoller *p = xcalloc(1, sizeof *p);
    InitializeSRWLock(&p->lock);
    p->iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
    p->afd = p->iocp ? afd_open(p->iocp) : NULL;
    if (!p->afd) {
        if (p->iocp) CloseHandle(p->iocp);
        free(p);
        return NULL;
    }
    return p;
}

/* Stop a socket's request (the lock is held): its completion still comes, and
 * frees it. */
static void drop_request(PlatPoller *p, Arm *a) {
    if (a->pr) {
        a->pr->live = false;
        CancelIoEx(a->pr->h, &a->pr->ov); /* (not found: it has completed already) */
        a->pr = NULL;
    }
    if (!a->sp) return;
    IO_STATUS_BLOCK cancel;
    a->sp->live = false;
    nt.cancel(p->afd, &a->sp->iosb, &cancel); /* (not found: it has completed already) */
    a->sp = NULL;
}

/* Take a completion from the port (the lock is held): the wait it ends, if it
 * is still that wait's, else -1. */
static long completed(PlatPoller *p, SockPoll *sp) {
    long i = -1;
    p->pending--;
    if (sp->live && sp->fd >= 0 && (size_t)sp->fd < p->at.n) {
        i = (long)p->at.v[sp->fd] - 1;
        if (i >= 0 && p->arms.v[i].sp != sp) i = -1;
    }
    if (i >= 0) p->arms.v[i].sp = NULL;
    free(sp);
    return i;
}

static long pipe_completed(PlatPoller *p, PipeReq *pr) {
    long i = -1;
    p->pending--;
    if (pr->live && pr->fd >= 0 && (size_t)pr->fd < p->at.n) {
        i = (long)p->at.v[pr->fd] - 1;
        if (i >= 0 && p->arms.v[i].pr != pr) i = -1;
    }
    if (i >= 0) p->arms.v[i].pr = NULL;
    free(pr);
    return i;
}

/* Wait for an overlapped pipe to have data, or to be closed at the other end:
 * a zero-byte read, its completion on the port. False if the pipe is tied to
 * another poller's port, or the read fails at once: then it is looked at. The
 * lock is held. */
static bool pipe_request(PlatPoller *p, WFd *e, Arm *a) {
    if (!e->port) {
        if (!CreateIoCompletionPort(e->h, p->iocp, PIPE_KEY, 0)) return false;
        e->port = p->iocp;
    }
    if (e->port != p->iocp) return false;
    PipeReq *pr = xcalloc(1, sizeof *pr);
    pr->h = e->h;
    pr->fd = a->fd;
    pr->live = true;
    static char none;
    if (!ReadFile(e->h, &none, 0, NULL, &pr->ov) && GetLastError() != ERROR_IO_PENDING) {
        free(pr); /* (failed at once: no completion comes) */
        return false;
    }
    a->pr = pr; /* (done at once or pending: its completion comes) */
    p->pending++;
    return true;
}

void plat_poller_free(PlatPoller *p) {
    if (!p) return;
    AcquireSRWLockExclusive(&p->lock);
    for (size_t i = 0; i < p->arms.n; i++) drop_request(p, &p->arms.v[i]);
    ReleaseSRWLockExclusive(&p->lock);
    /* Every request's memory belongs to the kernel until its completion is
     * taken: take them all (cancelled, they come at once), or keep it all. */
    uint64_t until = plat_now_ns() + 2000000000u;
    while (p->pending && plat_now_ns() < until) {
        OVERLAPPED_ENTRY ev[64];
        ULONG n = 0;
        if (!GetQueuedCompletionStatusEx(p->iocp, ev, 64, &n, 100, FALSE)) continue;
        for (ULONG j = 0; j < n; j++)
            if (ev[j].lpCompletionKey == AFD_KEY) completed(p, (SockPoll *)ev[j].lpOverlapped);
            else if (ev[j].lpCompletionKey == PIPE_KEY) pipe_completed(p, (PipeReq *)ev[j].lpOverlapped);
    }
    if (p->pending) return; /* (the kernel still has some: never freed) */
    CloseHandle(p->afd);
    CloseHandle(p->iocp);
    VEC_FREE(p->arms);
    VEC_FREE(p->at);
    free(p);
}

/* The index of fd's wait, or -1. The lock is held. */
static long arm_find(PlatPoller *p, int fd) {
    return fd >= 0 && (size_t)fd < p->at.n ? (long)p->at.v[fd] - 1 : -1;
}

static void arm_remove(PlatPoller *p, size_t i) {
    if (p->arms.v[i].pipe) p->pipes--;
    p->at.v[p->arms.v[i].fd] = 0;
    p->arms.v[i] = p->arms.v[--p->arms.n];
    if (i < p->arms.n) p->at.v[p->arms.v[i].fd] = (uint32_t)i + 1;
}

/* What a wait for reading or writing is told of: anything that makes the next
 * call not wait, failures and closes included. */
static ULONG afd_events(int events) {
    ULONG e = AFD_POLL_ABORT | AFD_POLL_LOCAL_CLOSE | AFD_POLL_CONNECT_FAIL;
    if (events & PLAT_POLL_READ) e |= AFD_POLL_RECEIVE | AFD_POLL_ACCEPT | AFD_POLL_DISCONNECT;
    if (events & PLAT_POLL_WRITE) e |= AFD_POLL_SEND;
    return e;
}

bool plat_poller_arm(PlatPoller *p, int fd, int events, uint64_t token) {
    WFd *e = wfd_get(fd);
    if (!e || (e->kind != WFD_SOCKET && e->kind != WFD_PIPE)) return false;
    bool pipe = e->kind == WFD_PIPE, ok = true;
    AcquireSRWLockExclusive(&p->lock);
    long i = arm_find(p, fd);
    if (i >= 0) {
        drop_request(p, &p->arms.v[i]);
        if (p->arms.v[i].pipe) p->pipes--;
    } else {
        while (p->at.n <= (size_t)fd) VEC_PUSH(p->at, 0);
        VEC_PUSH(p->arms, ((Arm){0}));
        i = (long)p->arms.n - 1;
        p->at.v[fd] = (uint32_t)i + 1;
    }
    p->arms.v[i] = (Arm){fd, events, token, pipe, false, NULL, NULL};
    if (pipe && e->ovl && events == PLAT_POLL_READ && pipe_request(p, e, &p->arms.v[i])) {
        p->arms.v[i].pipe = false;
        pipe = false; /* (no look needed) */
    } else if (pipe) {
        p->pipes++;
    } else {
        SockPoll *sp = xcalloc(1, sizeof *sp);
        sp->fd = fd;
        sp->live = true;
        sp->info.timeout.QuadPart = INT64_MAX;
        sp->info.nhandles = 1;
        sp->info.handles[0].handle = base_socket(e);
        sp->info.handles[0].events = afd_events(events);
        sp->iosb.Status = (NTSTATUS)STATUS_PENDING;
        NTSTATUS st = nt.ioctl(p->afd, NULL, NULL, &sp->iosb, &sp->iosb, IOCTL_AFD_POLL, &sp->info, sizeof sp->info,
                               &sp->info, sizeof sp->info);
        if (st == (NTSTATUS)STATUS_PENDING || st == 0) { /* (0: done already, its completion is queued) */
            p->arms.v[i].sp = sp;
            p->pending++;
        } else {
            free(sp);
            arm_remove(p, (size_t)i);
            ok = false;
        }
    }
    ReleaseSRWLockExclusive(&p->lock);
    if (pipe) plat_poller_wake(p); /* so that a wait in progress looks at it */
    return ok;
}

void plat_poller_forget(PlatPoller *p, int fd) {
    AcquireSRWLockExclusive(&p->lock);
    long i = arm_find(p, fd);
    if (i >= 0) {
        drop_request(p, &p->arms.v[i]);
        arm_remove(p, (size_t)i);
    }
    ReleaseSRWLockExclusive(&p->lock);
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
    OVERLAPPED_ENTRY ev[64];
    for (;;) {
        AcquireSRWLockExclusive(&p->lock);
        bool pipes = p->pipes > 0;
        ReleaseSRWLockExclusive(&p->lock);
        int64_t left = timeout_ns < 0 ? -1 : timeout_ns - (int64_t)(plat_now_ns() - start);
        DWORD ms = timeout_ns < 0 ? INFINITE : left <= 0 ? 0 : (DWORD)((left + 999999) / 1000000);
        if (pipes && (ms == INFINITE || ms > PIPE_LOOK_MS)) ms = PIPE_LOOK_MS;
        ULONG n = 0, want = (ULONG)(max - k < 64 ? max - k : 64);
        if (!GetQueuedCompletionStatusEx(p->iocp, ev, want, &n, ms, FALSE)) n = 0; /* (timed out) */
        bool woken = false;
        AcquireSRWLockExclusive(&p->lock);
        for (ULONG j = 0; j < n; j++) {
            if (ev[j].lpCompletionKey == WAKE_KEY) {
                woken = true;
                continue;
            }
            long i = ev[j].lpCompletionKey == PIPE_KEY ? pipe_completed(p, (PipeReq *)ev[j].lpOverlapped)
                                                       : completed(p, (SockPoll *)ev[j].lpOverlapped);
            if (i < 0) continue; /* (forgotten or replaced: cancelled) */
            tokens[k++] = p->arms.v[i].token;
            arm_remove(p, (size_t)i);
        }
        for (size_t i = 0; pipes && i < p->arms.n && k < max;) {
            Arm *a = &p->arms.v[i];
            WFd *e = wfd_get(a->fd);
            if (!a->pipe || (e && e->kind == WFD_PIPE && !pipe_ready(a, e->h))) {
                if (a->pipe) a->seen = true;
                i++;
                continue;
            }
            tokens[k++] = a->token; /* ready, or closed */
            arm_remove(p, i);
        }
        ReleaseSRWLockExclusive(&p->lock);
        /* Taken, then cleared: a wake-up before the clear is answered by this
         * return; one after it posts another. */
        if (woken) InterlockedExchange(&p->woken, 0);
        bool out_of_time = timeout_ns >= 0 && (int64_t)(plat_now_ns() - start) >= timeout_ns;
        if (k || woken || out_of_time) break;
    }
    return k;
}

void plat_poller_wake(PlatPoller *p) {
    if (InterlockedExchange(&p->woken, 1)) return; /* a wake-up is pending anyway */
    PostQueuedCompletionStatus(p->iocp, 0, WAKE_KEY, NULL);
}
#else
typedef int mlwsvm_win32_io_unused; /* ISO C forbids an empty translation unit */
#endif
