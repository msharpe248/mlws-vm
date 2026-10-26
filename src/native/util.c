/* util.c - the rest of the standard library's natives (docs/STDLIB.md):
 * time and dates, hashes, random numbers, encodings (hex, base64, URL, CSV),
 * sorting and searching lists with a comparator closure, set operations on
 * maps, and integer math. These are ordinary built-in natives: they run on the
 * calling worker thread and raise errors directly.
 *
 * Dates use the proleptic Gregorian calendar and are computed here, not by the
 * C library, so they do not depend on the platform or the time zone database:
 * a time is nanoseconds since 1970-01-01T00:00:00Z, and an offset is minutes
 * east of UTC. Only time.local_offset asks the operating system. */
#include "../heap/heap.h"
#include "../core/builtins.h"
#include "../core/sha256.h"
#include "../platform/platform.h"

#include <time.h>

/* ---- helpers ------------------------------------------------------------------------ */

static const char *fname(Process *p) { return p->cur_native ? p->cur_native->name : "native"; }

/* A string or byte buffer's bytes. */
static const uint8_t *data_arg(Process *p, Word a, uint32_t *len) {
    Obj *o = a.r;
    if (!o) vm_trap(p, "E_NULL", "%s: null where a string or byte buffer was expected", fname(p));
    if (o->kind == OBJ_STRING) {
        *len = ((ObjString *)o)->len;
        return (const uint8_t *)((ObjString *)o)->data;
    }
    if (o->kind == OBJ_BYTES) {
        *len = ((ObjBytes *)o)->len;
        return ((ObjBytes *)o)->data;
    }
    vm_trap(p, "E_TYPE", "%s: expected a string or byte buffer, got %s", fname(p), obj_kind_name(o->kind));
}

static ObjList *list_arg(Process *p, Word a) {
    Obj *o = a.r;
    if (o && o->kind == OBJ_LIST) return (ObjList *)o;
    vm_trap(p, "E_TYPE", "%s: expected a list", fname(p));
}

static ObjMap *map_arg(Process *p, Word a) {
    Obj *o = a.r;
    if (o && o->kind == OBJ_MAP) return (ObjMap *)o;
    vm_trap(p, "E_TYPE", "%s: expected a map", fname(p));
}

static Word sb_string(Process *p, StrBuf *b) {
    Word w = {.r = str_new(p, b->s ? b->s : "", b->n)};
    sb_free(b);
    return w;
}

static Word new_bytes(Process *p, const void *data, size_t n) {
    ObjBytes *b = bytes_new(p, (int64_t)n);
    if (n) memcpy(b->data, data, n);
    return (Word){.r = b};
}

/* ---- time --------------------------------------------------------------------------- */

#define NS_PER_S 1000000000ll

static int64_t floor_div(int64_t a, int64_t b) { return a / b - (a % b != 0 && (a < 0) != (b < 0)); }

/* Days since 1970-01-01 <-> a civil date (Howard Hinnant's algorithms). */
static int64_t days_from_civil(int64_t y, int64_t m, int64_t d) {
    y -= m <= 2;
    int64_t era = floor_div(y, 400);
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(int64_t z, int64_t *y, int64_t *m, int64_t *d) {
    z += 719468;
    int64_t era = floor_div(z, 146097);
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = yoe + era * 400 + (*m <= 2);
}

static bool leap(int64_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

static int64_t month_days(int64_t y, int64_t m) {
    static const int8_t D[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m == 2 && leap(y) ? 29 : D[m - 1];
}

typedef struct {
    int64_t year, month, day, hour, minute, second, nano, weekday, yday;
} Civil;

/* A time (ns since the epoch) as a civil date and time at an offset. */
static Civil to_civil(int64_t ns, int64_t offset_min) {
    Civil c;
    int64_t s = floor_div(ns, NS_PER_S) + offset_min * 60;
    c.nano = ns - floor_div(ns, NS_PER_S) * NS_PER_S;
    int64_t days = floor_div(s, 86400), rest = s - days * 86400;
    civil_from_days(days, &c.year, &c.month, &c.day);
    c.hour = rest / 3600;
    c.minute = rest / 60 % 60;
    c.second = rest % 60;
    c.weekday = floor_div(days + 3, 7) * -7 + days + 3 + 1; /* ISO: Monday 1 ... Sunday 7; 1970-01-01 was a Thursday */
    c.yday = days - days_from_civil(c.year, 1, 1) + 1;
    return c;
}

static bool check_offset(Process *p, int64_t off) {
    if (off < -24 * 60 || off > 24 * 60) vm_trap(p, "E_RANGE", "%s: offset %lld minutes is out of range", fname(p), (long long)off);
    return true;
}

static void put_offset(StrBuf *b, int64_t off) {
    if (!off) {
        sb_putc(b, 'Z');
        return;
    }
    sb_printf(b, "%c%02lld:%02lld", off < 0 ? '-' : '+', (long long)(llabs(off) / 60), (long long)(llabs(off) % 60));
}

static void n_time_now(Process *p, Word *a, Word *r) {
    (void)p, (void)a;
    r[0].i = plat_wall_ns();
}

static void n_time_monotonic(Process *p, Word *a, Word *r) {
    (void)p, (void)a;
    r[0].i = (int64_t)plat_now_ns();
}

/* time.format(ns, offset) -> "2026-10-26T14:05:09.250+01:00" (ISO 8601 / RFC 3339) */
static void n_time_format(Process *p, Word *a, Word *r) {
    check_offset(p, a[1].i);
    Civil c = to_civil(a[0].i, a[1].i);
    StrBuf b = {0};
    sb_printf(&b, "%04lld-%02lld-%02lldT%02lld:%02lld:%02lld", (long long)c.year, (long long)c.month, (long long)c.day,
              (long long)c.hour, (long long)c.minute, (long long)c.second);
    if (c.nano) { /* as few digits as it takes: milli-, micro- or nanoseconds */
        if (c.nano % 1000000 == 0) sb_printf(&b, ".%03lld", (long long)(c.nano / 1000000));
        else if (c.nano % 1000 == 0) sb_printf(&b, ".%06lld", (long long)(c.nano / 1000));
        else sb_printf(&b, ".%09lld", (long long)c.nano);
    }
    put_offset(&b, a[1].i);
    r[0] = sb_string(p, &b);
}

/* Parse n digits at *s; false if there are not n. */
static bool digits(const char **s, const char *end, int n, int64_t *v) {
    *v = 0;
    for (int k = 0; k < n; k++, (*s)++) {
        if (*s >= end || **s < '0' || **s > '9') return false;
        *v = *v * 10 + (**s - '0');
    }
    return true;
}

/* time.parse("2026-10-26", "2026-10-26T14:05:09.25+01:00", ...) -> ns */
static void n_time_parse(Process *p, Word *a, Word *r) {
    uint32_t n;
    const char *s0 = (const char *)data_arg(p, a[0], &n), *s = s0, *end = s0 + n;
    int64_t y, mo, d, h = 0, mi = 0, sec = 0, nano = 0, off = 0;
    if (!digits(&s, end, 4, &y) || s >= end || *s++ != '-' || !digits(&s, end, 2, &mo) || s >= end || *s++ != '-' ||
        !digits(&s, end, 2, &d))
        goto bad;
    if (mo < 1 || mo > 12 || d < 1 || d > month_days(y, mo)) goto bad;
    if (s < end && (*s == 'T' || *s == 't' || *s == ' ')) {
        s++;
        if (!digits(&s, end, 2, &h) || s >= end || *s++ != ':' || !digits(&s, end, 2, &mi)) goto bad;
        if (s < end && *s == ':') {
            s++;
            if (!digits(&s, end, 2, &sec)) goto bad;
            if (s < end && (*s == '.' || *s == ',')) {
                s++;
                int k = 0;
                for (; s < end && *s >= '0' && *s <= '9'; s++, k++)
                    if (k < 9) nano = nano * 10 + (*s - '0');
                if (!k) goto bad;
                for (; k < 9; k++) nano *= 10;
            }
        }
        if (h > 23 || mi > 59 || sec > 60) goto bad; /* 60: a leap second */
        if (s < end) {
            if (*s == 'Z' || *s == 'z') s++;
            else if (*s == '+' || *s == '-') {
                int sign = *s++ == '-' ? -1 : 1;
                int64_t oh, om = 0;
                if (!digits(&s, end, 2, &oh)) goto bad;
                if (s < end && *s == ':') s++;
                if (s < end && !digits(&s, end, 2, &om)) goto bad;
                if (oh > 23 || om > 59) goto bad;
                off = sign * (oh * 60 + om);
            } else goto bad;
        }
    }
    if (s != end) goto bad;
    int64_t secs = days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + sec - off * 60;
    if (secs > INT64_MAX / NS_PER_S - 1 || secs < INT64_MIN / NS_PER_S + 1)
        vm_trap(p, "E_RANGE", "time.parse: %.*s is outside the years 1678 to 2261", (int)n, s0);
    r[0].i = secs * NS_PER_S + nano;
    return;
bad:
    vm_trap(p, "E_PARSE", "time.parse: not an ISO 8601 date or time: \"%.*s\"", (int)(n > 64 ? 64 : n), s0);
}

/* time.date(ns, offset) -> Date */
static void n_time_date(Process *p, Word *a, Word *r) {
    check_offset(p, a[1].i);
    Civil c = to_civil(a[0].i, a[1].i);
    ObjStruct *s = struct_new(p, &VM_BUILTIN_DEFS[BUILTIN_DATE]);
    int64_t v[] = {c.year, c.month, c.day, c.hour, c.minute, c.second, c.nano, c.weekday, c.yday, a[1].i};
    for (int k = 0; k < DATEF__COUNT; k++) s->fields[k].i = v[k];
    r[0].r = s;
}

/* time.from_date(Date) -> ns. Fields out of range carry over, as in C's
 * mktime: month 13 is January of the next year, day 0 the last of the month
 * before. weekday and yday are ignored. */
static void n_time_from_date(Process *p, Word *a, Word *r) {
    ObjStruct *s = a[0].r;
    if (!s || s->h.kind != OBJ_STRUCT || s->def != &VM_BUILTIN_DEFS[BUILTIN_DATE])
        vm_trap(p, "E_TYPE", "time.from_date needs a Date");
    const Word *f = s->fields;
    /* Bounded fields keep every step below within an i64. */
    static const int64_t LIMIT[DATEF__COUNT] = {1000000, 12000000, 1000000000000, 1000000000000, 1000000000000,
                                                 1000000000000, 1000000000000, INT64_MAX, INT64_MAX, 1000000000000};
    for (int k = 0; k < DATEF__COUNT; k++)
        if (f[k].i > LIMIT[k] || f[k].i < -LIMIT[k])
            vm_trap(p, "E_RANGE", "time.from_date: %s is out of range", VM_BUILTIN_DEFS[BUILTIN_DATE].fields[k]);
    int64_t y = f[DATEF_YEAR].i, m = f[DATEF_MONTH].i - 1;
    y += floor_div(m, 12);
    m -= floor_div(m, 12) * 12;
    int64_t secs = (days_from_civil(y, m + 1, 1) + f[DATEF_DAY].i - 1) * 86400 + f[DATEF_HOUR].i * 3600 +
                   f[DATEF_MINUTE].i * 60 + f[DATEF_SECOND].i - f[DATEF_OFFSET].i * 60;
    if (secs > 9200000000ll || secs < -9200000000ll) vm_trap(p, "E_RANGE", "time.from_date: outside the years 1678 to 2261");
    r[0].i = secs * NS_PER_S + f[DATEF_NANO].i;
}

/* time.local_offset(ns) -> minutes east of UTC in the local time zone at that time */
static void n_time_local_offset(Process *p, Word *a, Word *r) {
    (void)p;
    r[0].i = plat_local_offset_s(floor_div(a[0].i, NS_PER_S)) / 60;
}

/* ---- hashes ------------------------------------------------------------------------- */

static uint32_t crc_table[256]; /* filled when the natives are registered, before any worker runs */

static void crc_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
}

/* hash.crc32(data) -> u32: the CRC-32 of zlib, PNG and Ethernet */
static void n_crc32(Process *p, Word *a, Word *r) {
    uint32_t n;
    const uint8_t *s = data_arg(p, a[0], &n);
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++) c = crc_table[(c ^ s[i]) & 0xFF] ^ (c >> 8);
    r[0].u = c ^ 0xFFFFFFFFu;
}

static void n_sha256(Process *p, Word *a, Word *r) {
    uint32_t n;
    const uint8_t *s = data_arg(p, a[0], &n);
    Sha256 h;
    uint8_t out[32];
    sha256_init(&h);
    sha256_update(&h, s, n);
    sha256_final(&h, out);
    r[0] = new_bytes(p, out, 32);
}

/* SipHash-2-4 (Aumasson and Bernstein): a keyed hash for hash tables that
 * attackers must not be able to flood. */
#define ROTL(x, b) (uint64_t)(((x) << (b)) | ((x) >> (64 - (b))))
#define SIPROUND                                                               \
    do {                                                                       \
        v0 += v1, v1 = ROTL(v1, 13), v1 ^= v0, v0 = ROTL(v0, 32);              \
        v2 += v3, v3 = ROTL(v3, 16), v3 ^= v2;                                 \
        v0 += v3, v3 = ROTL(v3, 21), v3 ^= v0;                                 \
        v2 += v1, v1 = ROTL(v1, 17), v1 ^= v2, v2 = ROTL(v2, 32);              \
    } while (0)

static uint64_t le64(const uint8_t *s) {
    uint64_t v = 0;
    for (int k = 7; k >= 0; k--) v = v << 8 | s[k];
    return v;
}

static uint64_t siphash(const uint8_t key[16], const uint8_t *s, size_t n) {
    uint64_t k0 = le64(key), k1 = le64(key + 8);
    uint64_t v0 = k0 ^ 0x736f6d6570736575ull, v1 = k1 ^ 0x646f72616e646f6dull, v2 = k0 ^ 0x6c7967656e657261ull,
             v3 = k1 ^ 0x7465646279746573ull;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t m = le64(s + i);
        v3 ^= m;
        SIPROUND;
        SIPROUND;
        v0 ^= m;
    }
    uint64_t b = (uint64_t)n << 56;
    for (size_t k = 0; i + k < n; k++) b |= (uint64_t)s[i + k] << (8 * k);
    v3 ^= b;
    SIPROUND;
    SIPROUND;
    v0 ^= b;
    v2 ^= 0xff;
    SIPROUND;
    SIPROUND;
    SIPROUND;
    SIPROUND;
    return v0 ^ v1 ^ v2 ^ v3;
}

static void n_siphash(Process *p, Word *a, Word *r) {
    uint32_t kn, n;
    const uint8_t *key = data_arg(p, a[0], &kn);
    if (kn != 16) vm_trap(p, "E_RANGE", "hash.siphash: the key must be 16 bytes, not %u", kn);
    const uint8_t *s = data_arg(p, a[1], &n);
    r[0].u = siphash(key, s, n);
}

/* ---- random numbers ----------------------------------------------------------------- */

/* xoshiro256** (Blackman and Vigna), one generator per process, seeded from
 * the OS's entropy unless random.seed sets it. Not for keys or tokens: use
 * random.bytes for those. */
static uint64_t splitmix(uint64_t *x) {
    uint64_t z = (*x += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

static void seed_with(Process *p, uint64_t seed) {
    for (int k = 0; k < 4; k++) p->rng[k] = splitmix(&seed);
    p->rng_seeded = true;
}

static uint64_t next_u64(Process *p) {
    if (!p->rng_seeded) {
        uint64_t seed;
        if (!plat_random_bytes(&seed, sizeof seed)) seed = plat_now_ns() ^ p->pid * 0x9e3779b97f4a7c15ull;
        seed_with(p, seed);
    }
    uint64_t *s = p->rng;
    uint64_t result = ROTL(s[1] * 5, 7) * 9, t = s[1] << 17;
    s[2] ^= s[0];
    s[3] ^= s[1];
    s[1] ^= s[2];
    s[0] ^= s[3];
    s[2] ^= t;
    s[3] = ROTL(s[3], 45);
    return result;
}

/* Uniform in [0, n), without modulo bias (Lemire's method would do too). */
static uint64_t below(Process *p, uint64_t n) {
    uint64_t limit = UINT64_MAX - UINT64_MAX % n;
    uint64_t x;
    do x = next_u64(p);
    while (x >= limit);
    return x % n;
}

static void n_random_seed(Process *p, Word *a, Word *r) {
    (void)r;
    seed_with(p, a[0].u);
}

/* random.int(lo, hi) -> uniform in [lo, hi] */
static void n_random_int(Process *p, Word *a, Word *r) {
    int64_t lo = a[0].i, hi = a[1].i;
    if (lo > hi) vm_trap(p, "E_RANGE", "random.int: lo %lld is above hi %lld", (long long)lo, (long long)hi);
    uint64_t span = (uint64_t)hi - (uint64_t)lo;
    r[0].u = (uint64_t)lo + (span == UINT64_MAX ? next_u64(p) : below(p, span + 1));
}

/* random.float() -> uniform in [0, 1), 53 random bits */
static void n_random_float(Process *p, Word *a, Word *r) {
    (void)a;
    r[0].f = (double)(next_u64(p) >> 11) * 0x1.0p-53;
}

/* random.bytes(n) -> n bytes from the OS's entropy source (for keys and tokens) */
static void n_random_bytes(Process *p, Word *a, Word *r) {
    int64_t n = a[0].i;
    if (n < 0 || n > (1 << 24)) vm_trap(p, "E_RANGE", "random.bytes: %lld bytes (0 to 16 MiB)", (long long)n);
    ObjBytes *b = bytes_new(p, n);
    if (n && !plat_random_bytes(b->data, (size_t)n)) vm_trap(p, "E_IO", "random.bytes: no entropy source");
    r[0].r = b;
}

/* random.shuffle(list): in place, every order equally likely */
static void n_random_shuffle(Process *p, Word *a, Word *r) {
    (void)r;
    ObjList *l = list_arg(p, a[0]);
    for (uint32_t i = l->len; i > 1; i--) {
        uint32_t j = (uint32_t)below(p, i);
        Word w = l->items[i - 1];
        uint8_t t = l->tags[i - 1];
        l->items[i - 1] = l->items[j], l->tags[i - 1] = l->tags[j];
        l->items[j] = w, l->tags[j] = t;
    }
}

/* ---- encodings ---------------------------------------------------------------------- */

static const char HEX[] = "0123456789abcdef";

static int hex_val(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void n_hex_encode(Process *p, Word *a, Word *r) {
    uint32_t n;
    const uint8_t *s = data_arg(p, a[0], &n);
    StrBuf b = {0};
    for (uint32_t i = 0; i < n; i++) sb_putc(&b, HEX[s[i] >> 4]), sb_putc(&b, HEX[s[i] & 15]);
    r[0] = sb_string(p, &b);
}

static void n_hex_decode(Process *p, Word *a, Word *r) {
    uint32_t n;
    const uint8_t *s = data_arg(p, a[0], &n);
    if (n % 2) vm_trap(p, "E_PARSE", "hex.decode: an odd number of digits");
    ObjBytes *b = bytes_new(p, n / 2);
    for (uint32_t i = 0; i < n; i += 2) {
        int hi = hex_val(s[i]), lo = hex_val(s[i + 1]);
        if (hi < 0 || lo < 0) vm_trap(p, "E_PARSE", "hex.decode: not a hex digit at offset %u", hi < 0 ? i : i + 1);
        b->data[i / 2] = (uint8_t)(hi << 4 | lo);
    }
    r[0].r = b;
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char B64URL[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

/* base64.encode(data, url) -> string: RFC 4648, padded; url: the URL-safe
 * alphabet, without padding */
static void n_base64_encode(Process *p, Word *a, Word *r) {
    uint32_t n;
    const uint8_t *s = data_arg(p, a[0], &n);
    bool url = a[1].i != 0;
    const char *A = url ? B64URL : B64;
    StrBuf b = {0};
    uint32_t i = 0;
    for (; i + 3 <= n; i += 3) {
        uint32_t v = (uint32_t)s[i] << 16 | (uint32_t)s[i + 1] << 8 | s[i + 2];
        sb_putc(&b, A[v >> 18]), sb_putc(&b, A[v >> 12 & 63]), sb_putc(&b, A[v >> 6 & 63]), sb_putc(&b, A[v & 63]);
    }
    if (n - i == 1) {
        uint32_t v = (uint32_t)s[i] << 16;
        sb_putc(&b, A[v >> 18]), sb_putc(&b, A[v >> 12 & 63]);
        if (!url) sb_puts(&b, "==");
    } else if (n - i == 2) {
        uint32_t v = (uint32_t)s[i] << 16 | (uint32_t)s[i + 1] << 8;
        sb_putc(&b, A[v >> 18]), sb_putc(&b, A[v >> 12 & 63]), sb_putc(&b, A[v >> 6 & 63]);
        if (!url) sb_putc(&b, '=');
    }
    r[0] = sb_string(p, &b);
}

static int b64_val(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

/* base64.decode(text) -> bytes: either alphabet, padding optional, whitespace ignored */
static void n_base64_decode(Process *p, Word *a, Word *r) {
    uint32_t n;
    const uint8_t *s = data_arg(p, a[0], &n);
    StrBuf b = {0};
    uint32_t acc = 0, bits = 0, pad = 0;
    for (uint32_t i = 0; i < n; i++) {
        int c = s[i];
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        if (c == '=') {
            pad++;
            continue;
        }
        int v = b64_val(c);
        if (v < 0 || pad) {
            sb_free(&b);
            vm_trap(p, "E_PARSE", "base64.decode: unexpected '%c' at offset %u", c, i);
        }
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            sb_putc(&b, (char)(acc >> bits & 0xFF));
        }
    }
    if (bits >= 6 || pad > 2 || (acc & ((1u << bits) - 1))) { /* a lone 6-bit group, or stray bits */
        sb_free(&b);
        vm_trap(p, "E_PARSE", "base64.decode: the input is truncated");
    }
    r[0] = new_bytes(p, b.s, b.n);
    sb_free(&b);
}

/* url.encode(text) -> text with every byte except A-Z a-z 0-9 - . _ ~ as %XX (RFC 3986) */
static void n_url_encode(Process *p, Word *a, Word *r) {
    uint32_t n;
    const uint8_t *s = data_arg(p, a[0], &n);
    StrBuf b = {0};
    for (uint32_t i = 0; i < n; i++) {
        int c = s[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
            c == '_' || c == '~')
            sb_putc(&b, (char)c);
        else sb_printf(&b, "%%%02X", c);
    }
    r[0] = sb_string(p, &b);
}

/* url.decode(text) -> text: %XX decoded; with plus true, '+' is a space (form data) */
static void n_url_decode(Process *p, Word *a, Word *r) {
    uint32_t n;
    const uint8_t *s = data_arg(p, a[0], &n);
    bool plus = a[1].i != 0;
    StrBuf b = {0};
    for (uint32_t i = 0; i < n; i++) {
        if (s[i] == '%') {
            int hi = i + 2 < n ? hex_val(s[i + 1]) : -1, lo = i + 2 < n ? hex_val(s[i + 2]) : -1;
            if (hi < 0 || lo < 0) {
                sb_free(&b);
                vm_trap(p, "E_PARSE", "url.decode: a bad %% escape at offset %u", i);
            }
            sb_putc(&b, (char)(hi << 4 | lo));
            i += 2;
        } else sb_putc(&b, plus && s[i] == '+' ? ' ' : (char)s[i]);
    }
    r[0] = sb_string(p, &b);
}

/* csv.parse(text) -> a list of rows, each a list of strings (RFC 4180: quoted
 * fields may hold commas, quotes as "", and line breaks; rows end with \n or
 * \r\n). */
static void n_csv_parse(Process *p, Word *a, Word *r) {
    uint32_t n;
    const char *s = (const char *)data_arg(p, a[0], &n);
    ObjList *rows = list_new(p, 0);
    ObjList *row = NULL;
    StrBuf f = {0};
    uint32_t i = 0;
    while (i < n) {
        if (!row) {
            row = list_new(p, 0);
            list_push(p, rows, (Word){.r = row}, T_R);
        }
        f.n = 0;
        if (s[i] == '"') { /* a quoted field */
            uint32_t start = i++;
            for (;;) {
                if (i >= n) {
                    sb_free(&f);
                    vm_trap(p, "E_PARSE", "csv.parse: the quote at offset %u is never closed", start);
                }
                if (s[i] == '"') {
                    if (i + 1 < n && s[i + 1] == '"') {
                        sb_putc(&f, '"');
                        i += 2;
                        continue;
                    }
                    i++;
                    break;
                }
                sb_putc(&f, s[i++]);
            }
            while (i < n && s[i] != ',' && s[i] != '\n' && s[i] != '\r') sb_putc(&f, s[i++]); /* lenient */
        } else {
            while (i < n && s[i] != ',' && s[i] != '\n' && s[i] != '\r') sb_putc(&f, s[i++]);
        }
        list_push(p, row, (Word){.r = str_new(p, f.s ? f.s : "", f.n)}, T_R);
        if (i < n && s[i] == ',') {
            i++;
            if (i == n) list_push(p, row, (Word){.r = str_new(p, "", 0)}, T_R); /* a trailing empty field */
            continue;
        }
        if (i < n && s[i] == '\r') i++;
        if (i < n && s[i] == '\n') i++;
        row = NULL; /* the end of a row */
    }
    sb_free(&f);
    r[0].r = rows;
}

/* csv.format(rows) -> text: fields quoted when they need it; numbers and other
 * values as tostr writes them; rows end with \n */
static void n_csv_format(Process *p, Word *a, Word *r) {
    ObjList *rows = list_arg(p, a[0]);
    StrBuf b = {0};
    for (uint32_t i = 0; i < rows->len; i++) {
        Obj *ro = rows->tags[i] == T_R ? rows->items[i].r : NULL;
        if (!ro || ro->kind != OBJ_LIST) {
            sb_free(&b);
            vm_trap(p, "E_TYPE", "csv.format: row %u is not a list", i);
        }
        ObjList *row = (ObjList *)ro;
        for (uint32_t k = 0; k < row->len; k++) {
            if (k) sb_putc(&b, ',');
            const char *t;
            uint32_t tn;
            Obj *o = row->tags[k] == T_R ? row->items[k].r : NULL;
            if (o && o->kind == OBJ_STRING) t = ((ObjString *)o)->data, tn = ((ObjString *)o)->len;
            else if (row->tags[k] == T_R && !o) t = "", tn = 0; /* null: an empty field */
            else {
                ObjString *v = value_to_string(p, row->items[k], (MlwsType)row->tags[k]);
                t = v->data, tn = v->len;
            }
            bool quote = false;
            for (uint32_t j = 0; j < tn && !quote; j++) quote = t[j] == ',' || t[j] == '"' || t[j] == '\n' || t[j] == '\r';
            if (!quote) {
                sb_putn(&b, t, tn);
                continue;
            }
            sb_putc(&b, '"');
            for (uint32_t j = 0; j < tn; j++) {
                if (t[j] == '"') sb_putc(&b, '"');
                sb_putc(&b, t[j]);
            }
            sb_putc(&b, '"');
        }
        sb_putc(&b, '\n');
    }
    r[0] = sb_string(p, &b);
}

/* ---- lists: sorting and searching --------------------------------------------------------- */

static bool same_bank(MlwsType a, MlwsType b) { return type_bank(a) == type_bank(b) && type_is_ref(a) == type_is_ref(b); }

/* Natural order: numbers by value, strings and byte buffers bytewise. */
static int natural_cmp(Process *p, Word x, uint8_t xt, Word y, uint8_t yt) {
    bool xr = type_is_ref((MlwsType)xt), yr = type_is_ref((MlwsType)yt);
    if (!xr && !yr) {
        bool xf = type_bank((MlwsType)xt) == BANK_F, yf = type_bank((MlwsType)yt) == BANK_F;
        if (xf || yf) {
            double a = xf ? x.f : (double)x.i, b = yf ? y.f : (double)y.i;
            if (a != a || b != b) return (a != a) - (b != b); /* NaN last */
            return (a > b) - (a < b);
        }
        if (xt == T_U64 && yt == T_U64) return (x.u > y.u) - (x.u < y.u);
        return (x.i > y.i) - (x.i < y.i);
    }
    Obj *a = xr ? x.r : NULL, *b = yr ? y.r : NULL;
    if (a && b && (a->kind == OBJ_STRING || a->kind == OBJ_BYTES) && (b->kind == OBJ_STRING || b->kind == OBJ_BYTES)) {
        uint32_t an, bn;
        const uint8_t *as = data_arg(p, x, &an), *bs = data_arg(p, y, &bn);
        int c = memcmp(as, bs, an < bn ? an : bn);
        return c ? (c > 0) - (c < 0) : (an > bn) - (an < bn);
    }
    return 2; /* not comparable */
}

typedef struct {
    Process *p;
    ObjClosure *cl;
    ObjList *snap; /* the elements being sorted (pinned) */
    bool failed;   /* the comparator raised an error: p->err has it */
    const char *bad; /* or the elements could not be compared: why */
} Sorter;

/* Compare elements i and j: negative, zero or positive. */
static int sort_cmp(Sorter *so, uint32_t i, uint32_t j) {
    ObjList *l = so->snap;
    if (!so->cl) {
        int c = natural_cmp(so->p, l->items[i], l->tags[i], l->items[j], l->tags[j]);
        if (c == 2) so->bad = "elements of different kinds (give a comparator)";
        return c == 2 ? 0 : c;
    }
    const MFunc *mf = so->cl->fn->mf;
    if (!same_bank((MlwsType)mf->params[0], (MlwsType)l->tags[i]) ||
        !same_bank((MlwsType)mf->params[1], (MlwsType)l->tags[j])) {
        so->bad = "the comparator's parameter types do not match the elements";
        return 0;
    }
    Word args[2] = {l->items[i], l->items[j]};
    Process *p = so->p;
    if (!proc_callback(p, so->cl, args)) {
        so->failed = true;
        return 0;
    }
    if (!p->nresults || type_bank((MlwsType)p->result_types[0]) != BANK_I || type_is_ref((MlwsType)p->result_types[0])) {
        so->bad = "the comparator must return an integer";
        return 0;
    }
    int64_t c = p->results[0].i;
    return (c > 0) - (c < 0);
}

/* A stable merge sort of the index array idx[0..n), using tmp. */
static void merge_sort(Sorter *so, uint32_t *idx, uint32_t *tmp, uint32_t n) {
    for (uint32_t w = 1; w < n && !so->failed && !so->bad; w *= 2) {
        for (uint32_t lo = 0; lo < n; lo += 2 * w) {
            uint32_t mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n;
            uint32_t a = lo, b = mid, k = lo;
            while (a < mid && b < hi) {
                if (so->failed || so->bad) { /* stop comparing; keep a permutation */
                    while (a < mid) tmp[k++] = idx[a++];
                    while (b < hi) tmp[k++] = idx[b++];
                    break;
                }
                tmp[k++] = sort_cmp(so, idx[b], idx[a]) < 0 ? idx[b++] : idx[a++];
            }
            while (a < mid) tmp[k++] = idx[a++];
            while (b < hi) tmp[k++] = idx[b++];
        }
        memcpy(idx, tmp, n * sizeof *idx);
    }
}

/* A closure called back failed: raise what it threw, or why it stopped. */
static MLWS_NORETURN void rethrow(Process *p, const char *where) {
    Obj *ex = p->cb_exception;
    p->cb_exception = NULL;
    if (ex) proc_throw(p, ex);
    vm_trap(p, p->err.code, "%s: %s", where, p->err.msg);
}

static ObjClosure *closure_arg(Process *p, Word a, int nparams) {
    Obj *o = a.r;
    if (!o) return NULL;
    if (o->kind != OBJ_CLOSURE) vm_trap(p, "E_TYPE", "%s: expected a closure", fname(p));
    ObjClosure *cl = (ObjClosure *)o;
    if (cl->fn->mf->nparams != nparams)
        vm_trap(p, "E_TYPE", "%s: the closure %s must take %d argument(s)", fname(p), cl->fn->name, nparams);
    return cl;
}

/* list.sort(list, cmp): stable, in place. cmp(a, b) returns negative, zero or
 * positive; null sorts numbers, strings and byte buffers in natural order. */
static void n_list_sort(Process *p, Word *a, Word *r) {
    (void)r;
    ObjList *l = list_arg(p, a[0]);
    ObjClosure *cl = closure_arg(p, a[1], 2);
    uint32_t n = l->len;
    if (n < 2) return;
    /* The comparator may change the list, or drop it: sort a pinned copy. */
    ObjList *snap = list_new(p, n);
    for (uint32_t i = 0; i < n; i++) list_push(p, snap, l->items[i], l->tags[i]);
    uint32_t *idx = xmalloc(2 * (size_t)n * sizeof *idx);
    for (uint32_t i = 0; i < n; i++) idx[i] = i;
    size_t npins = p->pins.n;
    VEC_PUSH(p->pins, &snap->h);
    VEC_PUSH(p->pins, &l->h);
    Sorter so = {p, cl, snap, false, NULL};
    merge_sort(&so, idx, idx + n, n);
    p->pins.n = npins;
    if (so.failed || so.bad) {
        free(idx);
        if (so.failed) rethrow(p, "list.sort: in the comparator");
        vm_trap(p, "E_TYPE", "list.sort: %s", so.bad);
    }
    if (l->len != n) {
        free(idx);
        vm_trap(p, "E_BOUNDS", "list.sort: the comparator changed the list's length");
    }
    for (uint32_t i = 0; i < n; i++) l->items[i] = snap->items[idx[i]], l->tags[i] = snap->tags[idx[i]];
    free(idx);
}

/* list.bsearch(sorted list, probe) -> index, or -(insertion point) - 1.
 * probe(x) says where x lies relative to what is sought: negative if before
 * it, 0 if it is the one, positive if after. */
static void n_list_bsearch(Process *p, Word *a, Word *r) {
    ObjList *l = list_arg(p, a[0]);
    ObjClosure *cl = closure_arg(p, a[1], 1);
    if (!cl) vm_trap(p, "E_NULL", "list.bsearch needs a probe closure");
    size_t npins = p->pins.n;
    VEC_PUSH(p->pins, &l->h);
    int64_t lo = 0, hi = (int64_t)l->len - 1;
    while (lo <= hi) {
        int64_t mid = lo + (hi - lo) / 2;
        if (mid >= l->len) break; /* the probe shortened the list */
        if (!same_bank((MlwsType)cl->fn->mf->params[0], (MlwsType)l->tags[mid])) {
            p->pins.n = npins;
            vm_trap(p, "E_TYPE", "list.bsearch: the probe's parameter type does not match the elements");
        }
        Word arg = l->items[mid];
        if (!proc_callback(p, cl, &arg)) {
            p->pins.n = npins;
            rethrow(p, "list.bsearch: in the probe");
        }
        if (!p->nresults || type_bank((MlwsType)p->result_types[0]) != BANK_I || type_is_ref((MlwsType)p->result_types[0])) {
            p->pins.n = npins;
            vm_trap(p, "E_TYPE", "list.bsearch: the probe must return an integer");
        }
        int64_t c = p->results[0].i;
        if (c == 0) {
            p->pins.n = npins;
            r[0].i = mid;
            return;
        }
        if (c < 0) lo = mid + 1;
        else hi = mid - 1;
    }
    p->pins.n = npins;
    r[0].i = -lo - 1;
}

/* ---- maps as sets ------------------------------------------------------------------------- */

/* map.union(a, b) -> a new map: a's entries, then b's (b's values win) */
static void n_map_union(Process *p, Word *a, Word *r) {
    ObjMap *x = map_arg(p, a[0]), *y = map_arg(p, a[1]);
    ObjMap *m = map_new(p);
    r[0].r = m;
    for (uint32_t i = 0; i < x->nentries; i++)
        if (x->entries[i].live) map_set(p, m, x->entries[i].key, x->entries[i].ktag, x->entries[i].val, x->entries[i].vtag);
    for (uint32_t i = 0; i < y->nentries; i++)
        if (y->entries[i].live) map_set(p, m, y->entries[i].key, y->entries[i].ktag, y->entries[i].val, y->entries[i].vtag);
}

/* map.intersect(a, b) -> a new map: a's entries whose keys b has too */
static void n_map_intersect(Process *p, Word *a, Word *r) {
    ObjMap *x = map_arg(p, a[0]), *y = map_arg(p, a[1]);
    ObjMap *m = map_new(p);
    r[0].r = m;
    for (uint32_t i = 0; i < x->nentries; i++) {
        MapEntry *e = &x->entries[i];
        if (e->live && map_find(y, e->key, e->ktag)) map_set(p, m, e->key, e->ktag, e->val, e->vtag);
    }
}

/* map.difference(a, b) -> a new map: a's entries whose keys b does not have */
static void n_map_difference(Process *p, Word *a, Word *r) {
    ObjMap *x = map_arg(p, a[0]), *y = map_arg(p, a[1]);
    ObjMap *m = map_new(p);
    r[0].r = m;
    for (uint32_t i = 0; i < x->nentries; i++) {
        MapEntry *e = &x->entries[i];
        if (e->live && !map_find(y, e->key, e->ktag)) map_set(p, m, e->key, e->ktag, e->val, e->vtag);
    }
}

/* ---- integer math --------------------------------------------------------------------------- */

static uint64_t mulmod(uint64_t a, uint64_t b, uint64_t m) {
#ifdef __SIZEOF_INT128__
    __extension__ typedef unsigned __int128 U128; /* (a GCC and Clang extension) */
    return (uint64_t)((U128)a * b % m);
#else
    uint64_t r = 0;
    a %= m;
    while (b) {
        if (b & 1) r = r >= m - a ? r - (m - a) : r + a;
        a = a >= m - a ? a - (m - a) : a + a;
        b >>= 1;
    }
    return r;
#endif
}

/* math.powmod(b, e, m) -> b^e mod m, for e >= 0 and m > 0 (the result is in [0, m)) */
static void n_powmod(Process *p, Word *a, Word *r) {
    int64_t b = a[0].i, e = a[1].i, m = a[2].i;
    if (m <= 0 || e < 0) vm_trap(p, "E_RANGE", "math.powmod needs e >= 0 and m > 0");
    int64_t rem = b % m; /* in (-m, m): adding m cannot overflow */
    uint64_t base = (uint64_t)(rem < 0 ? rem + m : rem), res = 1 % (uint64_t)m;
    for (uint64_t x = (uint64_t)e; x; x >>= 1) {
        if (x & 1) res = mulmod(res, base, (uint64_t)m);
        base = mulmod(base, base, (uint64_t)m);
    }
    r[0].u = res;
}

static uint64_t gcd_u(uint64_t x, uint64_t y) {
    while (y) {
        uint64_t t = x % y;
        x = y, y = t;
    }
    return x;
}

static uint64_t uabs(int64_t v) { return v < 0 ? 0 - (uint64_t)v : (uint64_t)v; }

static void n_gcd(Process *p, Word *a, Word *r) {
    (void)p;
    r[0].u = gcd_u(uabs(a[0].i), uabs(a[1].i));
}

static void n_lcm(Process *p, Word *a, Word *r) {
    uint64_t x = uabs(a[0].i), y = uabs(a[1].i);
    if (!x || !y) {
        r[0].i = 0;
        return;
    }
    uint64_t l = x / gcd_u(x, y) * y;
    if (l / y != x / gcd_u(x, y) || l > INT64_MAX) vm_trap(p, "E_OVERFLOW", "math.lcm overflows i64");
    r[0].u = l;
}

/* ---- the table ------------------------------------------------------------------------------- */

#define N(name, np, pt, nr, rt, fn, doc) {name, np, pt, nr, rt, fn, doc, 0, NULL, NULL, NULL}
#define T(...) {__VA_ARGS__}

static const NativeDef UTIL_NATIVES[] = {
    N("time.now", 0, T(0), 1, T(T_I64), n_time_now, "The wall clock: nanoseconds since 1970-01-01T00:00:00Z."),
    N("time.monotonic", 0, T(0), 1, T(T_I64), n_time_monotonic,
      "A clock that never goes backwards, in nanoseconds from an arbitrary start: for measuring durations."),
    N("time.format", 2, T(T_I64, T_I64), 1, T(T_R), n_time_format,
      "A time as ISO 8601 (RFC 3339) at an offset in minutes east of UTC: 2026-10-26T14:05:09.25+01:00, or ...Z for 0."),
    N("time.parse", 1, T(T_R), 1, T(T_I64), n_time_parse,
      "Parse ISO 8601: a date (2026-10-26), or a date and time with optional seconds, fraction and offset (no "
      "offset: UTC)."),
    N("time.date", 2, T(T_I64, T_I64), 1, T(T_R), n_time_date,
      "A time as a Date at an offset: year, month 1-12, day, hour, minute, second, nano, weekday (1 Monday - 7 "
      "Sunday), yday (1-366), offset."),
    N("time.from_date", 1, T(T_R), 1, T(T_I64), n_time_from_date,
      "The time a Date names (weekday and yday are ignored; other fields out of range carry over)."),
    N("time.local_offset", 1, T(T_I64), 1, T(T_I64), n_time_local_offset,
      "The local time zone's offset from UTC, in minutes east, at a time."),
    N("hash.crc32", 1, T(T_R), 1, T(T_U32), n_crc32, "CRC-32 (as zlib, PNG and Ethernet) of a string or byte buffer."),
    N("hash.sha256", 1, T(T_R), 1, T(T_R), n_sha256, "SHA-256 of a string or byte buffer, as 32 bytes."),
    N("hash.siphash", 2, T(T_R, T_R), 1, T(T_U64), n_siphash,
      "SipHash-2-4 of data with a 16-byte key: a keyed hash that resists hash flooding."),
    N("random.seed", 1, T(T_I64), 0, T(0), n_random_seed,
      "Seed this process's generator, for a repeatable sequence (it starts from OS entropy)."),
    N("random.int", 2, T(T_I64, T_I64), 1, T(T_I64), n_random_int, "A uniformly random integer in [lo, hi]."),
    N("random.float", 0, T(0), 1, T(T_F64), n_random_float, "A uniformly random float in [0, 1)."),
    N("random.bytes", 1, T(T_I64), 1, T(T_R), n_random_bytes,
      "n bytes from the OS's entropy source: for keys, tokens and nonces."),
    N("random.shuffle", 1, T(T_R), 0, T(0), n_random_shuffle, "Shuffle a list in place."),
    N("hex.encode", 1, T(T_R), 1, T(T_R), n_hex_encode, "Lower-case hex of a string or byte buffer."),
    N("hex.decode", 1, T(T_R), 1, T(T_R), n_hex_decode, "Bytes from hex (either case)."),
    N("base64.encode", 2, T(T_R, T_B), 1, T(T_R), n_base64_encode,
      "Base64 (RFC 4648), padded; with url true, the URL-safe alphabet without padding."),
    N("base64.decode", 1, T(T_R), 1, T(T_R), n_base64_decode,
      "Bytes from base64 in either alphabet; padding is optional and whitespace is ignored."),
    N("url.encode", 1, T(T_R), 1, T(T_R), n_url_encode,
      "Percent-encode every byte except A-Z a-z 0-9 - . _ ~ (RFC 3986)."),
    N("url.decode", 2, T(T_R, T_B), 1, T(T_R), n_url_decode,
      "Decode %XX escapes; with plus true, + is a space too (HTML form data)."),
    N("csv.parse", 1, T(T_R), 1, T(T_R), n_csv_parse,
      "CSV (RFC 4180) as a list of rows, each a list of strings."),
    N("csv.format", 1, T(T_R), 1, T(T_R), n_csv_format,
      "A list of rows as CSV: fields quoted when needed, other values as tostr writes them, rows ending in \\n."),
    N("list.sort", 2, T(T_R, T_R), 0, T(0), n_list_sort,
      "Sort a list in place, stably: with a comparator closure cmp(a, b) -> negative, 0 or positive, or null for "
      "numbers and strings in natural order."),
    N("list.bsearch", 2, T(T_R, T_R), 1, T(T_I64), n_list_bsearch,
      "Binary search of a sorted list with probe(x) -> negative (x is before), 0 (found) or positive: the index, "
      "or -(insertion point) - 1."),
    N("map.union", 2, T(T_R, T_R), 1, T(T_R), n_map_union, "A new map with the entries of both (b's values win)."),
    N("map.intersect", 2, T(T_R, T_R), 1, T(T_R), n_map_intersect, "A new map of a's entries whose keys b has too."),
    N("map.difference", 2, T(T_R, T_R), 1, T(T_R), n_map_difference,
      "A new map of a's entries whose keys b does not have."),
    N("math.powmod", 3, T(T_I64, T_I64, T_I64), 1, T(T_I64), n_powmod, "b^e mod m, without overflow (e >= 0, m > 0)."),
    N("math.gcd", 2, T(T_I64, T_I64), 1, T(T_I64), n_gcd, "Greatest common divisor (of the absolute values)."),
    N("math.lcm", 2, T(T_I64, T_I64), 1, T(T_I64), n_lcm, "Least common multiple (E_OVERFLOW if it does not fit)."),
};

void natives_register_util(VM *vm) {
    crc_init();
    for (size_t i = 0; i < sizeof UTIL_NATIVES / sizeof UTIL_NATIVES[0]; i++) vm_register_native(vm, &UTIL_NATIVES[i]);
}

const NativeDef *natives_util_table(size_t *n) {
    *n = sizeof UTIL_NATIVES / sizeof UTIL_NATIVES[0];
    return UTIL_NATIVES;
}
