/* libc.h - a few C library functions, for `make check-bindgen`. They are
 * declared here, not taken from the system headers, so the generated
 * bindings are the same on every platform (LP64: Linux and macOS). */
typedef unsigned long size_t;
typedef long time_t;

typedef struct { int quot; int rem; } div_t;   /* as glibc and macOS declare it */

struct tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
    long tm_gmtoff;
    const char *tm_zone;
};

enum level { LOW, MID = 10, HIGH };

size_t strlen(const char *s);
int abs(int n);
double ldexp(double x, int exp);
div_t div(int num, int den);
char *strchr(const char *s, int c);
time_t timegm(struct tm *tm);
struct tm *gmtime_r(const time_t *t, struct tm *out);
void qsort(void *base, size_t n, size_t size, int (*compar)(const void *, const void *));
int snprintf(char *buf, size_t n, const char *fmt, ...);
long double fabsl(long double x);   /* cannot be bound: skipped, with the reason */
