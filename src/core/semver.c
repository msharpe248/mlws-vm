/* semver.c - see semver.h. */
#include "semver.h"

#include <ctype.h>

/* A version at s[0, n), with its minor and patch numbers optional. */
static bool parse_n(const char *s, size_t n, SemVer *v) {
    uint32_t part[3] = {0, 0, 0};
    int k = 0;
    size_t i = 0;
    while (k < 3) {
        if (i >= n || !isdigit((unsigned char)s[i])) return false;
        uint64_t x = 0;
        while (i < n && isdigit((unsigned char)s[i])) {
            x = x * 10 + (uint64_t)(s[i++] - '0');
            if (x > UINT32_MAX) return false;
        }
        part[k++] = (uint32_t)x;
        if (i == n) break;
        if (s[i++] != '.') return false;
    }
    if (i != n) return false;
    v->major = part[0], v->minor = part[1], v->patch = part[2];
    return true;
}

bool semver_parse(const char *s, SemVer *v) { return parse_n(s, strlen(s), v); }

int semver_cmp(SemVer a, SemVer b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    return 0;
}

/* One term at s[0, n) (spaces trimmed): whether v satisfies it, in *ok;
 * false if the term is malformed. */
static bool term(const char *s, size_t n, SemVer v, bool *ok) {
    while (n && isspace((unsigned char)*s)) s++, n--;
    while (n && isspace((unsigned char)s[n - 1])) n--;
    if (n == 1 && *s == '*') return *ok = true;
    char op[3] = {0};
    size_t k = 0;
    while (k < n && k < 2 && strchr("^~<>=", s[k])) op[k] = s[k], k++;
    SemVer w;
    if (!parse_n(s + k, n - k, &w)) return false;
    int c = semver_cmp(v, w);
    if (!*op || !strcmp(op, "=")) *ok = c == 0;
    else if (!strcmp(op, ">=")) *ok = c >= 0;
    else if (!strcmp(op, ">")) *ok = c > 0;
    else if (!strcmp(op, "<=")) *ok = c <= 0;
    else if (!strcmp(op, "<")) *ok = c < 0;
    else if (!strcmp(op, "~")) *ok = c >= 0 && v.major == w.major && v.minor == w.minor;
    else if (!strcmp(op, "^")) { /* the leftmost non-zero part must stay */
        if (w.major) *ok = c >= 0 && v.major == w.major;
        else if (w.minor) *ok = c >= 0 && v.major == 0 && v.minor == w.minor;
        else *ok = c >= 0 && v.major == 0 && v.minor == 0 && v.patch == w.patch;
    } else {
        return false;
    }
    return true;
}

static bool check(const char *c, SemVer v, bool *all) {
    *all = true;
    if (!*c) return false;
    for (;;) {
        const char *comma = strchr(c, ',');
        size_t n = comma ? (size_t)(comma - c) : strlen(c);
        bool ok;
        if (!term(c, n, v, &ok)) return false;
        *all = *all && ok;
        if (!comma) return true;
        c = comma + 1;
    }
}

bool semver_constraint_ok(const char *c, char *why, size_t whylen) {
    bool all;
    if (check(c, (SemVer){0, 0, 0}, &all)) return true;
    snprintf(why, whylen, "'%s' is not a version constraint (such as ^1.2, ~1.2.3, >=1.0, <2.0, 1.4.2 or *)", c);
    return false;
}

bool semver_satisfies(SemVer v, const char *constraint) {
    bool all;
    return check(constraint, v, &all) && all;
}
