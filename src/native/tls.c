/* tls.c - see tls.h. Only what is declared here is used of the library, by
 * name, so no headers are needed: the functions and control numbers are the
 * stable ones of OpenSSL 1.1 and later and of LibreSSL. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/common.h"
#include "../platform/platform.h"
#include "tls.h"

#define SSL_ERROR_NONE 0
#define SSL_ERROR_SSL 1
#define SSL_ERROR_WANT_READ 2
#define SSL_ERROR_WANT_WRITE 3
#define SSL_ERROR_SYSCALL 5
#define SSL_ERROR_ZERO_RETURN 6
#define SSL_VERIFY_NONE 0
#define SSL_VERIFY_PEER 1
#define SSL_CTRL_MODE 33
#define SSL_CTRL_SET_TLSEXT_HOSTNAME 55
#define SSL_CTRL_SET_MIN_PROTO_VERSION 123
#define TLS1_2_VERSION 0x0303
#define SSL_MODE_ENABLE_PARTIAL_WRITE 1
#define SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER 2
#define SSL_FILETYPE_PEM 1
#define X509_V_OK 0

typedef struct SSL SSL;
typedef struct SSL_CTX SSL_CTX;
typedef struct SSL_METHOD SSL_METHOD;

/* The library's functions: name, result, parameters. */
#define SSL_FUNCS(X)                                                                    \
    X(TLS_client_method, const SSL_METHOD *, (void))                                    \
    X(TLS_server_method, const SSL_METHOD *, (void))                                    \
    X(SSL_CTX_new, SSL_CTX *, (const SSL_METHOD *))                                     \
    X(SSL_CTX_free, void, (SSL_CTX *))                                                  \
    X(SSL_CTX_ctrl, long, (SSL_CTX *, int, long, void *))                               \
    X(SSL_CTX_set_verify, void, (SSL_CTX *, int, void *))                               \
    X(SSL_CTX_set_default_verify_paths, int, (SSL_CTX *))                               \
    X(SSL_CTX_load_verify_locations, int, (SSL_CTX *, const char *, const char *))      \
    X(SSL_CTX_use_certificate_chain_file, int, (SSL_CTX *, const char *))               \
    X(SSL_CTX_use_PrivateKey_file, int, (SSL_CTX *, const char *, int))                 \
    X(SSL_new, SSL *, (SSL_CTX *))                                                      \
    X(SSL_free, void, (SSL *))                                                          \
    X(SSL_set_fd, int, (SSL *, int))                                                    \
    X(SSL_set_connect_state, void, (SSL *))                                             \
    X(SSL_set_accept_state, void, (SSL *))                                              \
    X(SSL_set1_host, int, (SSL *, const char *))                                        \
    X(SSL_ctrl, long, (SSL *, int, long, void *))                                       \
    X(SSL_do_handshake, int, (SSL *))                                                   \
    X(SSL_read, int, (SSL *, void *, int))                                              \
    X(SSL_write, int, (SSL *, const void *, int))                                       \
    X(SSL_shutdown, int, (SSL *))                                                       \
    X(SSL_get_error, int, (const SSL *, int))                                           \
    X(SSL_get_verify_result, long, (const SSL *))                                       \
    X(SSL_get_version, const char *, (const SSL *))
#define CRYPTO_FUNCS(X)                                                                 \
    X(ERR_get_error, unsigned long, (void))                                             \
    X(ERR_clear_error, void, (void))                                                    \
    X(ERR_error_string_n, void, (unsigned long, char *, size_t))                        \
    X(X509_verify_cert_error_string, const char *, (long))

#define X(name, ret, params) static ret(*p_##name) params;
SSL_FUNCS(X)
CRYPTO_FUNCS(X)
#undef X

struct Tls {
    SSL_CTX *ctx;
    SSL *ssl;
};

/* Where the library may be, the newest first. Apple's own libssl aborts a
 * program that loads it, so on macOS only Homebrew's is looked for. */
static const char *const LIBS[][2] = {
#if defined(__APPLE__)
    {"/opt/homebrew/opt/openssl/lib/libssl.dylib", "/opt/homebrew/opt/openssl/lib/libcrypto.dylib"},
    {"/opt/homebrew/opt/openssl@3/lib/libssl.dylib", "/opt/homebrew/opt/openssl@3/lib/libcrypto.dylib"},
    {"/usr/local/opt/openssl/lib/libssl.dylib", "/usr/local/opt/openssl/lib/libcrypto.dylib"},
    {"/usr/local/opt/openssl@3/lib/libssl.dylib", "/usr/local/opt/openssl@3/lib/libcrypto.dylib"},
#elif defined(_WIN32)
#if defined(__aarch64__)
    {"libssl-3-arm64.dll", "libcrypto-3-arm64.dll"},
#else
    {"libssl-3-x64.dll", "libcrypto-3-x64.dll"},
#endif
#else
    {"libssl.so.4", "libcrypto.so.4"},
    {"libssl.so.3", "libcrypto.so.3"},
    {"libssl.so.1.1", "libcrypto.so.1.1"},
    {"libssl.so", "libcrypto.so"},
#endif
};

static bool load_from(const char *ssl_name, const char *crypto_name) {
    char why[256];
    void *ssl = plat_dlopen(ssl_name, why, sizeof why);
    void *crypto = ssl ? plat_dlopen(crypto_name, why, sizeof why) : NULL;
    if (!crypto) return false;
    bool ok = true;
#define X(name, ret, params) ok = ok && (*(void **)&p_##name = plat_dlsym(ssl, #name)) != NULL;
    SSL_FUNCS(X)
#undef X
#define X(name, ret, params) ok = ok && (*(void **)&p_##name = plat_dlsym(crypto, #name)) != NULL;
    CRYPTO_FUNCS(X)
#undef X
    return ok;
}

bool tls_available(void) {
    static int state; /* 0: not tried; 1: loaded; -1: not there (a race only tries twice) */
    int st = __atomic_load_n(&state, __ATOMIC_ACQUIRE);
    if (st) return st > 0;
    bool ok = false;
    for (size_t i = 0; i < sizeof LIBS / sizeof LIBS[0] && !ok; i++) ok = load_from(LIBS[i][0], LIBS[i][1]);
    __atomic_store_n(&state, ok ? 1 : -1, __ATOMIC_RELEASE);
    return ok;
}

/* Sessions are set up one at a time. ThreadSanitizer reported two threads in
 * SSL_CTX_new touching the same memory, inside libcrypto, where it cannot see how
 * that is synchronized. Whether or not that is a race, setting up a session is
 * rare and slow, so here it does not happen at once. */
static PlatMutex *setup_lock(void) {
    static PlatMutex *lock;
    PlatMutex *m = __atomic_load_n(&lock, __ATOMIC_ACQUIRE);
    if (m) return m;
    PlatMutex *mine = plat_mutex_new();
    if (__atomic_compare_exchange_n(&lock, &m, mine, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return mine;
    plat_mutex_free(mine);
    return m;
}

static void say_error(char *err, size_t errlen, const char *what) {
    unsigned long e = p_ERR_get_error();
    char text[160] = "";
    if (e) p_ERR_error_string_n(e, text, sizeof text);
    snprintf(err, errlen, "%s%s%s", what, *text ? ": " : "", text);
}

void tls_free(Tls *t) {
    if (t->ssl) p_SSL_free(t->ssl);
    if (t->ctx) p_SSL_CTX_free(t->ctx);
    free(t);
}

static Tls *tls_new(bool server, char *err, size_t errlen) {
    p_ERR_clear_error();
    Tls *t = xcalloc(1, sizeof *t);
    t->ctx = p_SSL_CTX_new(server ? p_TLS_server_method() : p_TLS_client_method());
    if (!t->ctx) {
        say_error(err, errlen, "cannot set up TLS");
        tls_free(t);
        return NULL;
    }
    p_SSL_CTX_ctrl(t->ctx, SSL_CTRL_SET_MIN_PROTO_VERSION, TLS1_2_VERSION, NULL);
    return t;
}

static bool tls_attach(Tls *t, int sock, char *err, size_t errlen) {
    t->ssl = p_SSL_new(t->ctx);
    if (!t->ssl || !p_SSL_set_fd(t->ssl, sock)) {
        say_error(err, errlen, "cannot set up TLS");
        return false;
    }
    p_SSL_ctrl(t->ssl, SSL_CTRL_MODE, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER, NULL);
    return true;
}

static Tls *client_setup(int sock, const char *host, const char *ca_file, char *err, size_t errlen) {
    Tls *t = tls_new(false, err, errlen);
    if (!t) return NULL;
    p_SSL_CTX_set_verify(t->ctx, SSL_VERIFY_PEER, NULL);
    int loaded = *ca_file ? p_SSL_CTX_load_verify_locations(t->ctx, ca_file, NULL) : p_SSL_CTX_set_default_verify_paths(t->ctx);
    if (!loaded) {
        say_error(err, errlen, "cannot load the certificate authorities");
        tls_free(t);
        return NULL;
    }
    if (!tls_attach(t, sock, err, errlen)) {
        tls_free(t);
        return NULL;
    }
    p_SSL_set_connect_state(t->ssl);
    p_SSL_ctrl(t->ssl, SSL_CTRL_SET_TLSEXT_HOSTNAME, 0, (void *)host); /* (SNI; the certificate check is next) */
    if (!p_SSL_set1_host(t->ssl, host)) {
        say_error(err, errlen, "bad host name");
        tls_free(t);
        return NULL;
    }
    return t;
}

Tls *tls_client(int sock, const char *host, const char *ca_file, char *err, size_t errlen) {
    if (!tls_available()) {
        snprintf(err, errlen, "no TLS library (OpenSSL or LibreSSL) was found");
        return NULL;
    }
    plat_mutex_lock(setup_lock());
    Tls *t = client_setup(sock, host, ca_file, err, errlen);
    plat_mutex_unlock(setup_lock());
    return t;
}

static Tls *server_setup(int sock, const char *cert_file, const char *key_file, char *err, size_t errlen) {
    Tls *t = tls_new(true, err, errlen);
    if (!t) return NULL;
    p_SSL_CTX_set_verify(t->ctx, SSL_VERIFY_NONE, NULL);
    if (p_SSL_CTX_use_certificate_chain_file(t->ctx, cert_file) != 1 ||
        p_SSL_CTX_use_PrivateKey_file(t->ctx, key_file, SSL_FILETYPE_PEM) != 1) {
        say_error(err, errlen, "cannot load the certificate or its key");
        tls_free(t);
        return NULL;
    }
    if (!tls_attach(t, sock, err, errlen)) {
        tls_free(t);
        return NULL;
    }
    p_SSL_set_accept_state(t->ssl);
    return t;
}

Tls *tls_server(int sock, const char *cert_file, const char *key_file, char *err, size_t errlen) {
    if (!tls_available()) {
        snprintf(err, errlen, "no TLS library (OpenSSL or LibreSSL) was found");
        return NULL;
    }
    plat_mutex_lock(setup_lock());
    Tls *t = server_setup(sock, cert_file, key_file, err, errlen);
    plat_mutex_unlock(setup_lock());
    return t;
}

/* After a call that returned r: TLS_WANT_*, or TLS_FAILED with why. */
static int after(const Tls *t, int r, const char *what, char *err, size_t errlen) {
    int e = p_SSL_get_error(t->ssl, r);
    if (e == SSL_ERROR_WANT_READ) return TLS_WANT_READ;
    if (e == SSL_ERROR_WANT_WRITE) return TLS_WANT_WRITE;
    long v = p_SSL_get_verify_result(t->ssl);
    if (v != X509_V_OK) snprintf(err, errlen, "%s: certificate: %s", what, p_X509_verify_cert_error_string(v));
    else if (e == SSL_ERROR_SYSCALL) snprintf(err, errlen, "%s: the connection went away", what);
    else say_error(err, errlen, what);
    return TLS_FAILED;
}

int tls_handshake(Tls *t, char *err, size_t errlen) {
    p_ERR_clear_error();
    int r = p_SSL_do_handshake(t->ssl);
    return r == 1 ? TLS_OK : after(t, r, "handshake", err, errlen);
}

long tls_read(Tls *t, void *buf, size_t n, char *err, size_t errlen) {
    p_ERR_clear_error();
    int r = p_SSL_read(t->ssl, buf, n > 1u << 30 ? 1 << 30 : (int)n);
    if (r > 0) return r;
    int e = p_SSL_get_error(t->ssl, r);
    if (e == SSL_ERROR_ZERO_RETURN) return 0;
    return after(t, r, "read", err, errlen);
}

long tls_write(Tls *t, const void *buf, size_t n, char *err, size_t errlen) {
    p_ERR_clear_error();
    int r = p_SSL_write(t->ssl, buf, n > 1u << 30 ? 1 << 30 : (int)n);
    return r > 0 ? r : after(t, r, "write", err, errlen);
}

void tls_close_write(Tls *t) {
    p_ERR_clear_error();
    p_SSL_shutdown(t->ssl); /* sends close_notify; the socket may be full, and then the close says it */
}

const char *tls_version(const Tls *t) { return p_SSL_get_version(t->ssl); }
