/* options.c - VM settings by name, shared by the command line (`--max-frames
 * 500000`), the environment (MLWSVM_MAX_FRAMES=500000) and test files
 * (`; option: --max-frames 500000`). See docs/TOOLING.md ("VM options"). */
#include "vm.h"

#include "../platform/platform.h"
#include <ctype.h>
#include <errno.h>

#define MAX_THREADS 256 /* as in sched.c */

typedef enum {
    OPT_STACK_WORDS, OPT_THREADS, OPT_MAX_FRAMES, OPT_SLICE, OPT_GC_MIN, OPT_QUOTA, OPT_EXT_PATH, OPT__COUNT
} OptId;

static const char *const OPT_NAMES[OPT__COUNT] = {"stack-words", "threads", "max-frames", "slice",
                                                  "gc-min",      "quota",   "ext-path"};

static int opt_find(const char *name) {
    for (int i = 0; i < OPT__COUNT; i++)
        if (!strcmp(name, OPT_NAMES[i])) return i;
    return -1;
}

bool vm_option_known(const char *name) { return opt_find(name) >= 0; }

/* A whole non-negative integer; with `bytes`, an optional K, M or G suffix
 * (powers of 1024). */
static bool parse_u64(const char *s, size_t n, bool bytes, uint64_t *out) {
    if (!n || !isdigit((unsigned char)s[0])) return false;
    char buf[32];
    if (n >= sizeof buf) return false;
    memcpy(buf, s, n);
    buf[n] = 0;
    char *end;
    errno = 0;
    unsigned long long v = strtoull(buf, &end, 10);
    if (errno) return false;
    unsigned shift = 0;
    if (bytes && *end) {
        switch (toupper((unsigned char)*end)) {
        case 'K': shift = 10; break;
        case 'M': shift = 20; break;
        case 'G': shift = 30; break;
        default: return false;
        }
        end++;
    }
    if (*end) return false;
    if (shift && v > (UINT64_MAX >> shift)) return false;
    *out = (uint64_t)v << shift;
    return true;
}

static bool parse_range(const char *name, const char *value, bool bytes, uint64_t lo, uint64_t hi, uint64_t *out,
                        MlwsError *err) {
    if (!parse_u64(value, strlen(value), bytes, out))
        return mlws_err(err, "E_BAD_OPTION", "--%s: '%s' is not a %s", name, value,
                        bytes ? "size (a number, optionally with K, M or G)" : "whole number"),
               false;
    if (*out < lo || *out > hi)
        return mlws_err(err, "E_BAD_OPTION", "--%s: %s is out of range (%llu to %llu)", name, value,
                        (unsigned long long)lo, (unsigned long long)hi),
               false;
    return true;
}

/* heap=64M,raw=1G,mailbox=1000,insns=5000000 (any subset; "unlimited" resets one). */
static bool parse_quota(VM *vm, const char *value, MlwsError *err) {
    static const char *const KINDS[QUOTA__COUNT] = {"heap", "raw", "mailbox", "insns", "foreign", "os"};
    uint64_t q[QUOTA__COUNT];
    memcpy(q, vm->default_quota, sizeof q);
    const char *s = value;
    while (*s) {
        const char *comma = strchr(s, ',');
        size_t len = comma ? (size_t)(comma - s) : strlen(s);
        const char *eq = memchr(s, '=', len);
        int kind = -1;
        if (eq)
            for (int k = 0; k < QUOTA__COUNT; k++)
                if ((size_t)(eq - s) == strlen(KINDS[k]) && !strncmp(s, KINDS[k], (size_t)(eq - s))) kind = k;
        if (kind < 0)
            return mlws_err(err, "E_BAD_OPTION",
                            "--quota: expected heap=, raw=, mailbox=, insns=, foreign= or os= (comma-separated), got '%.*s'",
                            (int)len, s),
                   false;
        const char *v = eq + 1;
        size_t vlen = len - (size_t)(v - s);
        uint64_t n;
        if (vlen == 9 && !strncmp(v, "unlimited", 9)) n = UINT64_MAX;
        else if (!parse_u64(v, vlen, kind == QUOTA_HEAP || kind == QUOTA_RAW, &n))
            return mlws_err(err, "E_BAD_OPTION", "--quota: bad value for %s: '%.*s'", KINDS[kind], (int)vlen, v), false;
        q[kind] = n;
        s = comma ? comma + 1 : s + len;
    }
    memcpy(vm->default_quota, q, sizeof q);
    return true;
}

bool vm_set_option(VM *vm, const char *name, const char *value, MlwsError *err) {
    uint64_t n;
    switch (opt_find(name)) {
    case OPT_STACK_WORDS: /* the stack's size in bytes must fit in 32 bits */
        if (!parse_range(name, value, false, 64, UINT32_MAX / 8, &n, err)) return false;
        vm->stack_words = (uint32_t)n;
        return true;
    case OPT_THREADS:
        if (!parse_range(name, value, false, 1, MAX_THREADS, &n, err)) return false;
        vm->threads = (int)n;
        return true;
    case OPT_MAX_FRAMES:
        if (!parse_range(name, value, false, 1, UINT32_MAX, &n, err)) return false;
        vm->max_frames = (uint32_t)n;
        return true;
    case OPT_SLICE:
        if (!parse_range(name, value, false, 1, 1000000000, &n, err)) return false;
        vm->slice_insns = (uint32_t)n;
        return true;
    case OPT_GC_MIN:
        if (!parse_range(name, value, true, 0, UINT64_MAX, &n, err)) return false;
        vm->gc_min_threshold = n; /* 0: the default (1 MiB) */
        return true;
    case OPT_QUOTA: return parse_quota(vm, value, err);
    case OPT_EXT_PATH: { /* trusted extension directories, searched in order; ':'-separated (';' on Windows) */
        char sep = plat_path_sep();
        for (const char *s = value; *s;) {
            const char *e = strchr(s, sep);
            size_t len = e ? (size_t)(e - s) : strlen(s);
            if (len) VEC_PUSH(vm->ext_paths, xstrndup(s, len));
            s = e ? e + 1 : s + len;
        }
        return true;
    }
    default: return mlws_err(err, "E_BAD_OPTION", "unknown option --%s", name), false;
    }
}

void vm_options_from_env(VM *vm) {
    for (int i = 0; i < OPT__COUNT; i++) {
        char env[64] = "MLWSVM_";
        size_t k = strlen(env);
        for (const char *c = OPT_NAMES[i]; *c && k < sizeof env - 1; c++) env[k++] = *c == '-' ? '_' : (char)toupper(*c);
        env[k] = 0;
        const char *v = getenv(env);
        if (!v || !*v) continue;
        MlwsError err;
        if (!vm_set_option(vm, OPT_NAMES[i], v, &err)) fprintf(stderr, "mlwsvm: ignoring %s: %s\n", env, err.msg);
    }
}
