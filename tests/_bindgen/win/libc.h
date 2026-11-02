/* libc.h - a few C library functions, for `make check-bindgen` on Windows
 * (tests/_bindgen/libc.h is the same for LP64 systems). They are declared
 * here, not taken from the system headers, so the bindings do not depend on
 * the SDK. Windows is LLP64: long is 32 bits, size_t and time_t are 64. */
typedef unsigned long long size_t;
typedef long long time_t;

typedef struct { int quot; int rem; } div_t;

struct tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
};

enum level { LOW, MID = 10, HIGH };

size_t strlen(const char *s);
int abs(int n);
double ldexp(double x, int exp);
div_t div(int num, int den);
char *strchr(const char *s, int c);
time_t _mkgmtime64(struct tm *tm);
int _gmtime64_s(struct tm *out, const time_t *t);
void qsort(void *base, size_t n, size_t size, int (*compar)(const void *, const void *));
long double fabsl(long double x);   /* cannot be bound: skipped, with the reason */
