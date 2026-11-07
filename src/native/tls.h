/* tls.h - TLS on a connection, through the system's OpenSSL or LibreSSL, loaded
 * when first asked for, so that mlwsvm itself needs neither to build or run. */
#ifndef MLWSVM_TLS_H
#define MLWSVM_TLS_H

#include <stdbool.h>
#include <stddef.h>

typedef struct Tls Tls;

enum { TLS_OK = 0, TLS_WANT_READ = -1, TLS_WANT_WRITE = -2, TLS_FAILED = -3 };

bool tls_available(void);
/* A TLS session on fd, a descriptor of the socket that is the session's own
 * (closed with it): a client verifying the certificate against host (with
 * ca_file's authorities, or the system's if it is empty), or a server with a
 * certificate chain and key. NULL: why, in err, and fd is the caller's again.
 * Two handles may share a session (net.split): every call takes its lock, and
 * the last tls_release ends it. */
Tls *tls_client(int fd, const char *host, const char *ca_file, char *err, size_t errlen);
Tls *tls_server(int fd, const char *cert_file, const char *key_file, char *err, size_t errlen);
void tls_retain(Tls *t);
void tls_release(Tls *t); /* the last: close_notify, then the session and its descriptor go */
/* Each returns TLS_OK or a byte count, or: wait for the socket and call again
 * with the same arguments, or TLS_FAILED, with err. */
int tls_handshake(Tls *t, char *err, size_t errlen);
long tls_read(Tls *t, void *buf, size_t n, char *err, size_t errlen);  /* 0: the end of input */
long tls_write(Tls *t, const void *buf, size_t n, char *err, size_t errlen);
void tls_close_write(Tls *t);
const char *tls_version(const Tls *t);

#endif
