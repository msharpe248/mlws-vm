/* host.c - embedding MLWSVM in a C program (include/mlwsvm/embed.h).
 *
 *     make embed-example && ./build/embed-host lib
 *
 * Loads a module from a string, calls its functions, lets it call back into
 * the host, runs a service in the background and feeds it messages from two
 * host threads. It checks every result, so `make check-embed` runs it as a
 * test. */
#define _POSIX_C_SOURCE 200809L /* pthreads */
#include <mlwsvm/embed.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "host.c:%d: check failed: %s\n", __LINE__, #cond); \
            failures++;                                                        \
        }                                                                      \
    } while (0)

/* ---- host functions that MLWSVM code calls ------------------------------------ */

static const MlwsApi *api;
static int logged;

/* host.log(message): the host keeps a count (and could write a log file) */
static void host_log(MlwsCtx *cx, const MlwsValue *args, MlwsValue *results) {
    (void)results;
    size_t n;
    const char *s = api->str_data(cx, args[0], &n);
    if (s) logged++;
}

/* host.scale(x) -> x * the host's factor */
static int64_t factor = 3;
static void host_scale(MlwsCtx *cx, const MlwsValue *args, MlwsValue *results) {
    (void)cx;
    results[0].i = args[0].i * factor;
}

/* ---- the MLWSVM side --------------------------------------------------------------- */

static const char *SOURCE =
    ".module calc\n"
    ".export add, greet, scaled_sum, stats, divide, service\n"
    ".func add(i64 a, i64 b) -> i64\n"
    "    add I2, a, b\n"
    "    ret I2\n"
    ".endf\n"
    ".func greet(ref name) -> ref\n"
    "    strcat R1, \"hello, \", name\n"
    "    ncall \"host.log\" (R1)\n" /* calls back into the host */
    "    ret R1\n"
    ".endf\n"
    /* sum of host.scale(i) for i < n, computed by n processes in parallel */
    ".func part(i64 parent, i64 i)\n"
    "    ncall \"host.scale\" (i) -> (I2)\n"
    "    send parent, I2\n"
    "    ret\n"
    ".endf\n"
    ".func scaled_sum(i64 n) -> i64\n"
    "    mov I1, 0\n"
    "1:  spawn part (PID, I1)\n"
    "    inc I1\n"
    "    blt I1, n, 1b\n"
    "    mov I2, 0\n"
    "    mov I1, 0\n"
    "2:  recv I3\n"
    "    add I2, I3\n"
    "    inc I1\n"
    "    blt I1, n, 2b\n"
    "    ret I2\n"
    ".endf\n"
    ".func stats(i64 a, f64 x) -> (ref, i64)\n" /* structured data: JSON */
    "    mnew R0\n"
    "    mset R0, \"a\", a\n"
    "    mset R0, \"x\", x\n"
    "    ncall \"json.stringify\" (R0, 0) -> (R1)\n"
    "    ret R1, 2\n"
    ".endf\n"
    ".func divide(i64 a, i64 b) -> i64\n"
    "    div I2, a, b\n" /* E_DIV_ZERO for b = 0 */
    "    ret I2\n"
    ".endf\n"
    /* a service: counts the bytes of the strings it is sent, until "stop" */
    ".func service -> i64\n"
    "    mov I1, 0\n"
    "1:  recv R0\n"
    "    strcmp I2, R0, \"stop\"\n"
    "    bz I2, 2f\n"
    "    len I3, R0\n"
    "    add I1, I3\n"
    "    jmp 1b\n"
    "2:  ret I1\n"
    ".endf\n";

typedef struct {
    MlwsVM *vm;
    uint64_t pid;
    int count;
} Sender;

static void *sender(void *arg) {
    Sender *s = arg;
    for (int i = 0; i < s->count; i++) mlws_send(s->vm, s->pid, &MLWS_STR("abcd")); /* 4 bytes each */
    return NULL;
}

int main(int argc, char **argv) {
    const char *lib = argc > 1 ? argv[1] : "lib";
    api = mlws_api();
    MlwsVM *vm = mlws_new();
    mlws_add_path(vm, lib);

    MlwsNativeDesc log_d = {"host.log", 1, {MLWS_T_R}, 0, {0}, 0, host_log, NULL, "log a message"};
    MlwsNativeDesc scale_d = {"host.scale", 1, {MLWS_T_I64}, 1, {MLWS_T_I64}, 0, host_scale, NULL, "x * factor"};
    CHECK(mlws_define(vm, &log_d));
    CHECK(mlws_define(vm, &scale_d));
    if (!mlws_load_source(vm, "calc", SOURCE)) {
        fprintf(stderr, "load: %s\n", mlws_error(vm));
        return 1;
    }

    MlwsArg out[2];
    int got = 0;
    MlwsArg add_in[2] = {MLWS_INT(40), MLWS_INT(2)};
    CHECK(mlws_call(vm, "calc", "add", add_in, 2, out, 1, &got) && got == 1 && out[0].v.i == 42);

    MlwsArg greet_in[1] = {MLWS_STR("world")};
    CHECK(mlws_call(vm, "calc", "greet", greet_in, 1, out, 1, NULL) && out[0].str && !strcmp(out[0].str, "hello, world"));
    CHECK(logged == 1);

    MlwsArg sum_in[1] = {MLWS_INT(100)}; /* 3 * (0 + ... + 99), by 100 processes */
    CHECK(mlws_call(vm, "calc", "scaled_sum", sum_in, 1, out, 1, NULL) && out[0].v.i == 3 * 4950);

    MlwsArg stats_in[2] = {MLWS_INT(7), MLWS_FLOAT(2.5)};
    CHECK(mlws_call(vm, "calc", "stats", stats_in, 2, out, 2, &got) && got == 2);
    CHECK(out[0].str && !strcmp(out[0].str, "{\"a\":7,\"x\":2.5}") && out[1].v.i == 2);

    /* Errors: a runtime error in MLWSVM code, wrong arguments, a missing function. */
    MlwsArg div_in[2] = {MLWS_INT(1), MLWS_INT(0)};
    CHECK(!mlws_call(vm, "calc", "divide", div_in, 2, out, 1, NULL));
    CHECK(!strcmp(mlws_error_code(vm), "E_DIV_ZERO") && strstr(mlws_backtrace(vm), "calc.divide"));
    CHECK(!mlws_call(vm, "calc", "add", greet_in, 1, out, 1, NULL) && !strcmp(mlws_error_code(vm), "E_TYPE"));
    CHECK(!mlws_call(vm, "calc", "nothing", NULL, 0, out, 1, NULL) && !strcmp(mlws_error_code(vm), "E_LINK_UNDEF"));

    /* A background service, fed from two host threads. */
    uint64_t pid = 0;
    CHECK(mlws_start(vm, "calc", "service", NULL, 0, &pid) && pid > 0);
    Sender a = {vm, pid, 500}, b = {vm, pid, 500};
    pthread_t ta, tb;
    pthread_create(&ta, NULL, sender, &a);
    pthread_create(&tb, NULL, sender, &b);
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);
    CHECK(mlws_send(vm, pid, &MLWS_STR("stop")));
    CHECK(mlws_wait(vm, out, 1, NULL) && out[0].v.i == 1000 * 4);

    /* The VM keeps its modules: call again after the background run. And the
     * standard library loads through .import (from the lib directory). */
    CHECK(mlws_call(vm, "calc", "add", add_in, 2, out, 1, NULL) && out[0].v.i == 42);
    CHECK(mlws_load_source(vm, "uses_std", ".module uses_std\n.import std.math as m\n.export g\n"
                                           ".func g(i64 a, i64 b) -> i64\n    call m.gcd (a, b) -> (I2)\n"
                                           "    ret I2\n.endf\n"));
    MlwsArg gcd_in[2] = {MLWS_INT(84), MLWS_INT(36)};
    CHECK(mlws_call(vm, "uses_std", "g", gcd_in, 2, out, 1, NULL) && out[0].v.i == 12);

    mlws_free(vm);
    if (failures) {
        fprintf(stderr, "embed: %d check(s) failed\n", failures);
        return 1;
    }
    printf("embed: all checks passed\n");
    return 0;
}
