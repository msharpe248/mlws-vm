/* common.h - allocation helpers, growable arrays, string builders, string maps,
 * error reporting and small file/path utilities shared by every MLWSVM component. */
#ifndef MLWSVM_COMMON_H
#define MLWSVM_COMMON_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__) || defined(__clang__)
#define MLWS_PRINTF(f, a) __attribute__((format(printf, f, a)))
#define MLWS_NORETURN __attribute__((noreturn))
#define MLWS_COLD __attribute__((cold, noinline))
#define MLWS_NOINLINE __attribute__((noinline))
#else
#define MLWS_PRINTF(f, a)
#define MLWS_NORETURN
#define MLWS_COLD
#define MLWS_NOINLINE
#endif

#if defined(_MSC_VER) && !defined(__clang__)
#define MLWS_THREAD_LOCAL __declspec(thread)
#else
#define MLWS_THREAD_LOCAL _Thread_local
#endif

void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t sz);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);

/* Growable array of T: declare with VEC(T) name; zero-initialise before use. */
#define VEC(T) struct { T *v; size_t n, cap; }
#define VEC_GROW(vec, need)                                                    \
    do {                                                                       \
        if ((need) > (vec).cap) {                                              \
            size_t nc_ = (vec).cap ? (vec).cap * 2 : 8;                        \
            while (nc_ < (need)) nc_ *= 2;                                     \
            (vec).v = xrealloc((vec).v, nc_ * sizeof *(vec).v);                \
            (vec).cap = nc_;                                                   \
        }                                                                      \
    } while (0)
#define VEC_PUSH(vec, x)                                                       \
    do {                                                                       \
        VEC_GROW(vec, (vec).n + 1);                                            \
        (vec).v[(vec).n++] = (x);                                              \
    } while (0)
#define VEC_FREE(vec)                                                          \
    do {                                                                       \
        free((vec).v);                                                         \
        (vec).v = NULL;                                                        \
        (vec).n = (vec).cap = 0;                                               \
    } while (0)

typedef VEC(char *) StrVec;

/* String builder. */
typedef struct {
    char *s;
    size_t n, cap;
} StrBuf;
void sb_putc(StrBuf *b, char c);
void sb_putn(StrBuf *b, const char *s, size_t n);
void sb_puts(StrBuf *b, const char *s);
void sb_printf(StrBuf *b, const char *fmt, ...) MLWS_PRINTF(2, 3);
void sb_vprintf(StrBuf *b, const char *fmt, va_list ap);
void sb_free(StrBuf *b);
/* Append s as a quoted, escaped string literal in MLWSVM assembly syntax. */
void sb_put_quoted(StrBuf *b, const char *s, size_t n);

/* Open-addressing string -> pointer map. Keys are copied. */
typedef struct {
    char **keys;
    void **vals;
    size_t cap, n;
} StrMap;
void smap_put(StrMap *m, const char *key, void *val);
bool smap_get(const StrMap *m, const char *key, void **val);
bool smap_has(const StrMap *m, const char *key);
void smap_free(StrMap *m);

uint32_t hash_bytes(const void *p, size_t n);

/* Shortest decimal representation of d that round-trips ("nan", "inf", "-inf"). */
void fmt_double(double d, char *buf, size_t n);

/* Errors carry a stable machine-readable code (e.g. "E_ASM_UNDEF_LABEL")
 * and a human-readable message that usually starts with a location. */
typedef struct {
    char code[32];
    char msg[2048];
} MlwsError;
/* Fills e (if non-NULL) and returns false so callers can `return mlws_err(...)`. */
bool mlws_err(MlwsError *e, const char *code, const char *fmt, ...) MLWS_PRINTF(3, 4);
void mlws_err_clear(MlwsError *e);

/* Files and paths. */
char *read_file(const char *path, size_t *len);
bool write_file(const char *path, const void *data, size_t len);
bool file_exists(const char *path);
char *path_dirname(const char *path);
char *path_join(const char *a, const char *b);
const char *path_basename(const char *path);
bool str_ends_with(const char *s, const char *suffix);

#endif
