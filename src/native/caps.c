/* caps.c - capabilities: what a process may reach, by name. A process has
 * none set (it may reach anything) until it is restricted; from then on,
 * everything not granted is denied, and it can only narrow what it has.
 * Children inherit it. docs/STDLIB.md, "Sandboxing". */
#include <ctype.h>

#include "../heap/heap.h"
#include "../platform/platform.h"

static const char *const KINDS[CAP__COUNT] = {"read", "write", "connect", "listen", "run", "env"};

int cap_kind(const char *name, size_t n) {
    for (int i = 0; i < CAP__COUNT; i++)
        if (strlen(KINDS[i]) == n && !memcmp(KINDS[i], name, n)) return i;
    return -1;
}

void caps_retain(Caps *c) {
    if (c) __atomic_add_fetch(&c->refs, 1, __ATOMIC_RELAXED);
}

void caps_release(Caps *c) {
    if (!c || __atomic_sub_fetch(&c->refs, 1, __ATOMIC_ACQ_REL)) return;
    for (int k = 0; k < CAP__COUNT; k++) {
        for (size_t i = 0; i < c->allow[k].n; i++) free(c->allow[k].v[i]);
        VEC_FREE(c->allow[k]);
    }
    free(c);
}

/* A path as the system would reach it, following its links: the longest part
 * that exists, resolved, and the rest as written (the file to be made). NULL if
 * the rest climbs with "..": nothing says where that would go. */
static char *canonical(const char *path) {
    char *real = plat_realpath(path);
    if (real) return real;
    char *dir = path_dirname(path);
    const char *base = path_basename(path);
    if (!strcmp(base, "..") || !strcmp(dir, path)) {
        free(dir);
        return NULL;
    }
    char *up = canonical(*dir ? dir : ".");
    free(dir);
    if (!up) return NULL;
    char *full = path_join(up, base);
    free(up);
    return full;
}

static bool path_within(const char *path, const char *dir) {
    size_t n = strlen(dir);
    if (n && (dir[n - 1] == '/' || dir[n - 1] == '\\')) n--; /* the root */
    return !strncmp(path, dir, n) && (path[n] == 0 || path[n] == '/' || path[n] == '\\');
}

static bool same_host(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    return true;
}

static bool name_matches(const char *name, const char *pattern) {
    size_t n = strlen(pattern);
    if (n && pattern[n - 1] == '*') return !strncmp(name, pattern, n - 1);
    return !strcmp(name, pattern);
}

/* host:port against an entry host:port, host:*, *:port, or *; port -1: any. */
static bool net_matches(const char *host, int64_t port, const char *entry) {
    if (!strcmp(entry, "*")) return true;
    const char *colon = strrchr(entry, ':');
    if (!colon) return false;
    size_t hn = (size_t)(colon - entry);
    bool host_ok = (hn == 1 && entry[0] == '*') || (hn == strlen(host) && same_host(entry, host, hn));
    return host_ok && (port < 0 || !strcmp(colon + 1, "*") || strtoll(colon + 1, NULL, 10) == port);
}

bool caps_allow_path(const Caps *c, int kind, const char *path) {
    if (!c) return true;
    char *full = canonical(path);
    bool ok = false;
    for (size_t i = 0; full && i < c->allow[kind].n && !ok; i++) ok = path_within(full, c->allow[kind].v[i]);
    free(full);
    return ok;
}

bool caps_allow_net(const Caps *c, int kind, const char *host, int64_t port) {
    if (!c) return true;
    for (size_t i = 0; i < c->allow[kind].n; i++)
        if (net_matches(host, port, c->allow[kind].v[i])) return true;
    return false;
}

bool caps_allow_name(const Caps *c, int kind, const char *name) {
    if (!c) return true;
    for (size_t i = 0; i < c->allow[kind].n; i++)
        if (name_matches(name, c->allow[kind].v[i])) return true;
    return false;
}

/* What an entry asks for is within what c grants. (A path's entry is
 * canonical: that is what it is compared with.) */
static bool granted(const Caps *c, int kind, const char *entry) {
    if (!c) return true;
    if (kind == CAP_READ || kind == CAP_WRITE) return caps_allow_path(c, kind, entry);
    if (kind == CAP_RUN || kind == CAP_ENV) {
        for (size_t i = 0; i < c->allow[kind].n; i++) {
            const char *have = c->allow[kind].v[i];
            size_t n = strlen(have);
            if (!strcmp(have, entry) || (n && have[n - 1] == '*' && !strncmp(entry, have, n - 1))) return true;
        }
        return false;
    }
    for (size_t i = 0; i < c->allow[kind].n; i++) { /* connect, listen: no wider than an entry */
        const char *have = c->allow[kind].v[i], *colon = strrchr(entry, ':');
        if (!strcmp(have, "*") || (strlen(have) == strlen(entry) && same_host(have, entry, strlen(have)))) return true;
        if (!colon) continue;
        char host[256];
        size_t hn = (size_t)(colon - entry) < sizeof host - 1 ? (size_t)(colon - entry) : sizeof host - 1;
        memcpy(host, entry, hn);
        host[hn] = 0;
        if (strcmp(host, "*") && strcmp(colon + 1, "*") && net_matches(host, strtoll(colon + 1, NULL, 10), have)) return true;
    }
    return false;
}

Caps *caps_new(void) {
    Caps *c = xcalloc(1, sizeof *c);
    c->refs = 1;
    return c;
}

/* An entry goes in if what it grants is held already; a path, as it resolves. */
void caps_grant(Caps *c, const Caps *within, int kind, const char *entry) {
    char *e = NULL;
    if (kind == CAP_READ || kind == CAP_WRITE) e = canonical(entry);
    else e = xstrdup(entry);
    if (e && granted(within, kind, e)) VEC_PUSH(c->allow[kind], e);
    else free(e);
}

/* ---- natives ---- */

static void n_cap_restrict(Process *p, Word *a, Word *r) {
    (void)r;
    Obj *o = a[0].r;
    if (!o) vm_trap(p, "E_NULL", "cap.restrict of null");
    if (o->kind != OBJ_MAP) vm_trap(p, "E_TYPE", "cap.restrict: a map from kinds to lists of what they allow");
    ObjMap *m = (ObjMap *)o;
    Caps *c = caps_new();
    for (uint32_t i = 0; i < m->nentries; i++) {
        const MapEntry *e = &m->entries[i];
        if (!e->live) continue;
        const Obj *k = e->key.r, *v = e->val.r;
        int kind = e->ktag == T_R && k && k->kind == OBJ_STRING ? cap_kind(((const ObjString *)k)->data, ((const ObjString *)k)->len) : -1;
        if (kind < 0 || e->vtag != T_R || !v || v->kind != OBJ_LIST) {
            caps_release(c);
            vm_trap(p, "E_TYPE", "cap.restrict: a map from kinds (read, write, connect, listen, run, env) to lists");
        }
        const ObjList *l = (const ObjList *)v;
        for (uint32_t j = 0; j < l->len; j++) {
            const Obj *s = l->tags[j] == T_R ? l->items[j].r : NULL;
            if (!s || s->kind != OBJ_STRING) {
                caps_release(c);
                vm_trap(p, "E_TYPE", "cap.restrict: what a kind allows is a list of strings");
            }
            caps_grant(c, p->caps, kind, ((const ObjString *)s)->data);
        }
    }
    caps_release(p->caps);
    p->caps = c;
    sched_set_quota(p, QUOTA_FOREIGN, 0); /* foreign code could reach anything */
}

static void n_cap_allowed(Process *p, Word *a, Word *r) {
    const Obj *ko = a[0].r, *vo = a[1].r;
    if (!ko || ko->kind != OBJ_STRING || !vo || vo->kind != OBJ_STRING) vm_trap(p, "E_TYPE", "cap.allowed: a kind and a string");
    const ObjString *ks = (const ObjString *)ko;
    const char *v = ((const ObjString *)vo)->data;
    int kind = cap_kind(ks->data, ks->len);
    if (kind < 0) vm_trap(p, "E_TYPE", "cap.allowed: no kind '%s' (read, write, connect, listen, run, env)", ks->data);
    if (kind == CAP_READ || kind == CAP_WRITE) r[0].i = caps_allow_path(p->caps, kind, v);
    else if (kind == CAP_RUN || kind == CAP_ENV) r[0].i = caps_allow_name(p->caps, kind, v);
    else {
        const char *colon = strrchr(v, ':');
        if (!colon) vm_trap(p, "E_TYPE", "cap.allowed: %s wants host:port", ks->data);
        char *host = xstrndup(v, (size_t)(colon - v));
        r[0].i = caps_allow_net(p->caps, kind, host, strtoll(colon + 1, NULL, 10));
        free(host);
    }
}

static void n_cap_limited(Process *p, Word *a, Word *r) {
    (void)a;
    r[0].i = p->caps != NULL;
}

#define N(name, np, pt, nr, rt, fn, doc) {name, np, pt, nr, rt, fn, doc, 0, NULL, NULL, NULL}
#define T(...) {__VA_ARGS__}
static const NativeDef CAP_NATIVES[] = {
    N("cap.restrict", 1, T(T_R), 0, T(0), n_cap_restrict,
      "From now on this process, and those it spawns, may reach only what a map grants: from read, write, connect, listen, run or env to a list of what each allows. What is not granted is denied, foreign code too, and a process can only narrow what it has."),
    N("cap.allowed", 2, T(T_R, T_R), 1, T(T_B), n_cap_allowed,
      "Whether this process may reach something: a kind, and a path, host:port, command or variable."),
    N("cap.limited", 0, T(0), 1, T(T_B), n_cap_limited, "Whether this process has been restricted."),
};
#undef N
#undef T

void natives_register_caps(VM *vm) {
    for (size_t i = 0; i < sizeof CAP_NATIVES / sizeof CAP_NATIVES[0]; i++) vm_register_native(vm, &CAP_NATIVES[i]);
}

const NativeDef *natives_caps_table(size_t *n) {
    *n = sizeof CAP_NATIVES / sizeof CAP_NATIVES[0];
    return CAP_NATIVES;
}
