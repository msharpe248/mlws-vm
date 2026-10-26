/* demo.c - an example MLWSVM extension, also used by the tests (tests/native).
 *
 * Build:    cc -shared -fPIC -I include -o demo.mlwsx extensions/demo/demo.c
 *           (make builds it as build/ext/demo.mlwsx)
 * Install:  mlwsvm ext install demo.mlwsx
 * Use:      .extension "demo"   then   ncall "demo.add" (I0, I1) -> (I2)
 *
 * It shows each part of the API: numbers, strings, lists and maps, errors,
 * foreign handles with a finalizer, a blocking call, a serial (not
 * thread-safe) one, and messages posted from a thread of its own. */
#define _POSIX_C_SOURCE 200809L /* nanosleep */
#include <mlwsvm/mlwsvm.h>

#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const MlwsApi *api;

/* demo.add(i64, i64) -> i64 */
static void add(MlwsCtx *cx, const MlwsValue *a, MlwsValue *r) {
    (void)cx;
    r[0].i = a[0].i + a[1].i;
}

/* demo.greet(name) -> "hello, NAME" */
static void greet(MlwsCtx *cx, const MlwsValue *a, MlwsValue *r) {
    size_t n;
    const char *name = api->str_data(cx, a[0], &n);
    if (!name) return; /* str_data recorded E_TYPE */
    char buf[256];
    int len = snprintf(buf, sizeof buf, "hello, %.*s", (int)(n > 200 ? 200 : n), name);
    r[0] = api->str_new(cx, buf, (size_t)len);
}

/* demo.split(text, sep) -> a list of strings */
static void split(MlwsCtx *cx, const MlwsValue *a, MlwsValue *r) {
    size_t n, sn;
    const char *s = api->str_data(cx, a[0], &n);
    const char *sep = api->str_data(cx, a[1], &sn);
    if (!s || !sep) return;
    if (!sn) {
        api->error(cx, "E_DEMO", "demo.split: the separator is empty");
        return;
    }
    MlwsValue list = api->list_new(cx);
    size_t start = 0;
    for (size_t i = 0; i + sn <= n;) {
        if (!memcmp(s + i, sep, sn)) {
            api->list_push(cx, list, api->str_new(cx, s + start, i - start), MLWS_T_R);
            i += sn;
            start = i;
        } else {
            i++;
        }
    }
    api->list_push(cx, list, api->str_new(cx, s + start, n - start), MLWS_T_R);
    r[0] = list;
}

/* demo.counts(list of strings) -> a map from each string to its count */
static void counts(MlwsCtx *cx, const MlwsValue *a, MlwsValue *r) {
    int64_t n = api->list_len(cx, a[0]);
    if (n < 0) return;
    MlwsValue map = api->map_new(cx);
    for (int64_t i = 0; i < n; i++) {
        MlwsValue item, old;
        uint8_t t, ot;
        api->list_get(cx, a[0], i, &item, &t);
        if (t != MLWS_T_R || api->kind(cx, item) != MLWS_KIND_STRING) {
            api->error(cx, "E_TYPE", "demo.counts: element %lld is not a string", (long long)i);
            return;
        }
        MlwsValue c = {.i = 1};
        if (api->map_get(cx, map, item, MLWS_T_R, &old, &ot)) c.i = old.i + 1;
        api->map_set(cx, map, item, MLWS_T_R, c, MLWS_T_I64);
    }
    r[0] = map;
}

/* demo.fail(message): always fails, with code E_DEMO */
static void fail(MlwsCtx *cx, const MlwsValue *a, MlwsValue *r) {
    (void)r;
    size_t n;
    const char *msg = api->str_data(cx, a[0], &n);
    api->error(cx, "E_DEMO", "%.*s", (int)n, msg ? msg : "");
}

/* demo.fill(bytes, value): set every byte of a buffer, in place */
static void fill(MlwsCtx *cx, const MlwsValue *a, MlwsValue *r) {
    (void)r;
    size_t n;
    uint8_t *b = api->bytes_data(cx, a[0], &n);
    if (b) memset(b, (int)a[1].i, n);
}

/* ---- a foreign resource: a counter, freed by its finalizer ---- */

typedef struct {
    int64_t value;
} Counter;

static int64_t finalized; /* guarded by the extension's lock: finalizers are serial */

static void counter_finalize(void *p) {
    free(p);
    finalized++;
}

static const MlwsForeignType COUNTER = {"demo.counter", counter_finalize};

static void counter_new(MlwsCtx *cx, const MlwsValue *a, MlwsValue *r) {
    Counter *c = calloc(1, sizeof *c);
    c->value = a[0].i;
    r[0] = api->foreign_new(cx, &COUNTER, c);
}

static void counter_add(MlwsCtx *cx, const MlwsValue *a, MlwsValue *r) {
    Counter *c = api->foreign_get(cx, a[0], &COUNTER);
    if (!c) return;
    c->value += a[1].i;
    r[0].i = c->value;
}

static void counter_close(MlwsCtx *cx, const MlwsValue *a, MlwsValue *r) {
    (void)r;
    api->foreign_release(cx, a[0]);
}

static void finalized_count(MlwsCtx *cx, const MlwsValue *a, MlwsValue *r) {
    (void)cx, (void)a;
    r[0].i = finalized;
}

/* ---- blocking and serial ---- */

/* demo.sleep(ms) -> ms: blocks its thread, so it is declared BLOCKING */
static void sleep_ms(MlwsCtx *cx, const MlwsValue *a, MlwsValue *r) {
    (void)cx;
    struct timespec ts = {(time_t)(a[0].i / 1000), (long)(a[0].i % 1000) * 1000000L};
    nanosleep(&ts, NULL);
    r[0].i = a[0].i;
}

/* demo.bump() -> the new total: a plain global, so it is declared SERIAL */
static int64_t total;

static void bump(MlwsCtx *cx, const MlwsValue *a, MlwsValue *r) {
    (void)cx, (void)a;
    int64_t t = total; /* a deliberately non-atomic read-modify-write */
    r[0].i = total = t + 1;
}

static void get_total(MlwsCtx *cx, const MlwsValue *a, MlwsValue *r) {
    (void)cx, (void)a;
    r[0].i = total;
}

/* demo.data() -> the per-native data pointer, as a number */
static int64_t the_data = 4242;

static void data(MlwsCtx *cx, const MlwsValue *a, MlwsValue *r) {
    (void)a;
    r[0].i = *(int64_t *)api->native_data(cx);
}

/* ---- messages from another thread ---- */

/* demo.ticker(pid, n) -> handle: a thread of the extension's own posts the
 * numbers 1..n and then "done" to pid. demo.ticker_join(handle) waits for it:
 * an extension must not let such a thread outlive the VM. */
typedef struct {
    pthread_t thread;
    MlwsHost *host;
    uint64_t pid;
    int64_t n;
    int joined;
} Ticker;

static void *ticker_main(void *arg) {
    Ticker *t = arg;
    for (int64_t i = 1; i <= t->n; i++) api->post_int(t->host, t->pid, i);
    api->post_str(t->host, t->pid, "done", 4);
    api->hold(t->host, -1);
    return NULL;
}

static void ticker_finalize(void *p) {
    Ticker *t = p;
    if (!t->joined) pthread_join(t->thread, NULL);
    free(t);
}
static const MlwsForeignType TICKER = {"demo.ticker", ticker_finalize};

static void ticker(MlwsCtx *cx, const MlwsValue *a, MlwsValue *r) {
    Ticker *t = calloc(1, sizeof *t);
    t->host = api->host(cx);
    t->pid = a[0].u;
    t->n = a[1].i;
    api->hold(t->host, +1); /* the process may wait in recv for these */
    if (pthread_create(&t->thread, NULL, ticker_main, t)) {
        api->hold(t->host, -1);
        free(t);
        api->error(cx, "E_DEMO", "cannot start a thread");
        return;
    }
    r[0] = api->foreign_new(cx, &TICKER, t);
}

static void ticker_join(MlwsCtx *cx, const MlwsValue *a, MlwsValue *r) {
    (void)r;
    Ticker *t = api->foreign_get(cx, a[0], &TICKER);
    if (t && !t->joined) {
        pthread_join(t->thread, NULL);
        t->joined = 1;
    }
}

MLWSVM_EXPORT int MLWSVM_EXTENSION_INIT(const MlwsApi *a, MlwsExtension *ext) {
    if (a->version < 1) return 1;
    api = a;
    const MlwsNativeDesc natives[] = {
        {"demo.add", 2, {MLWS_T_I64, MLWS_T_I64}, 1, {MLWS_T_I64}, 0, add, NULL, "a + b"},
        {"demo.greet", 1, {MLWS_T_R}, 1, {MLWS_T_R}, 0, greet, NULL, "\"hello, \" + name"},
        {"demo.split", 2, {MLWS_T_R, MLWS_T_R}, 1, {MLWS_T_R}, 0, split, NULL, "split a string into a list"},
        {"demo.counts", 1, {MLWS_T_R}, 1, {MLWS_T_R}, 0, counts, NULL, "count the strings in a list"},
        {"demo.fail", 1, {MLWS_T_R}, 0, {0}, 0, fail, NULL, "fail with E_DEMO"},
        {"demo.fill", 2, {MLWS_T_R, MLWS_T_U8}, 0, {0}, 0, fill, NULL, "fill a byte buffer"},
        {"demo.counter_new", 1, {MLWS_T_I64}, 1, {MLWS_T_R}, 0, counter_new, NULL, "a new counter"},
        {"demo.counter_add", 2, {MLWS_T_R, MLWS_T_I64}, 1, {MLWS_T_I64}, 0, counter_add, NULL, "add to a counter"},
        {"demo.counter_close", 1, {MLWS_T_R}, 0, {0}, 0, counter_close, NULL, "free a counter now"},
        {"demo.finalized", 0, {0}, 1, {MLWS_T_I64}, MLWS_NATIVE_SERIAL, finalized_count, NULL, "counters freed"},
        {"demo.sleep", 1, {MLWS_T_I64}, 1, {MLWS_T_I64}, MLWS_NATIVE_BLOCKING, sleep_ms, NULL, "sleep (blocking)"},
        {"demo.bump", 0, {0}, 1, {MLWS_T_I64}, MLWS_NATIVE_SERIAL, bump, NULL, "add 1 to a global"},
        {"demo.total", 0, {0}, 1, {MLWS_T_I64}, MLWS_NATIVE_SERIAL, get_total, NULL, "the global"},
        {"demo.data", 0, {0}, 1, {MLWS_T_I64}, 0, data, &the_data, "per-native data"},
        {"demo.ticker", 2, {MLWS_T_I64, MLWS_T_I64}, 1, {MLWS_T_R}, 0, ticker, NULL, "post 1..n, then \"done\""},
        {"demo.ticker_join", 1, {MLWS_T_R}, 0, {0}, MLWS_NATIVE_BLOCKING, ticker_join, NULL, "wait for a ticker"},
    };
    for (size_t i = 0; i < sizeof natives / sizeof natives[0]; i++)
        if (!a->define(ext, &natives[i])) return 1;
    return 0;
}
