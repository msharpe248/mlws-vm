/* win32.h - what win32.c and win32_io.c share: UTF-8 <-> UTF-16 strings, and
 * the table that gives Windows handles and sockets the small integer
 * descriptors the platform layer passes around. */
#ifndef MLWSVM_WIN32_H
#define MLWSVM_WIN32_H
#ifdef _WIN32
#include "platform.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

/* The platform layer's strings are UTF-8; Windows' wide APIs take UTF-16.
 * Both return malloc'd strings (NULL for NULL). */
wchar_t *w_str(const char *s);
char *u_str(const wchar_t *s);

/* Descriptors. 0, 1 and 2 are the process's standard handles. */
enum { WFD_FILE = 1, WFD_PIPE, WFD_CHAR, WFD_SOCKET };
typedef struct {
    int kind;      /* WFD_*, or 0 if free */
    HANDLE h;      /* for files, pipes and character devices */
    SOCKET s;      /* for sockets */
    bool nonblock; /* a pipe in PIPE_NOWAIT mode, or a non-blocking socket */
    SOCKET base;   /* a socket's base handle, for the poller (0: not yet asked) */
} WFd;
int wfd_new(int kind, HANDLE h, SOCKET s); /* -1 if the table is full */
WFd *wfd_get(int fd);                      /* NULL if fd is not open */
void wfd_release(int fd);                  /* the entry only: the caller closes the handle */

void wsa_init(void);
int64_t win_filetime_ns(FILETIME ft); /* 100 ns units since 1601 -> ns since 1970 */

/* Error codes of our own, outside Windows' range (bit 29: "customer" codes). */
#define WERR_IS_DIR 0x20000001
#define WERR_NO_PORT 0x20000002 /* connect: no local port free */

#endif
#endif
