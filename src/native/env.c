/* env.c - environments: named values in the VM that a set of processes shares,
 * as Erlang's application environment (docs/STDLIB.md, "Environments"). Not
 * the operating system's environment, which os.env reads.
 *
 * Each program has one of its own, which its processes join as they are
 * spawned; a process may join a named one instead (env.join), which lives as
 * long as the VM. A value is any value a message can be, kept as the bytes
 * that carry it between nodes (dist.c's encoding), so that each env.get makes
 * the reader its own copy, as a message is: processes still share nothing.
 * `mlwsvm run --env FILE` fills each program's from a file of `name = JSON`
 * lines (vm_env_load_file), and `mlwsvm serve` a program's from the
 * NAME.mlws.env beside it. A value marked secret (`secret name = ...` in a
 * file) the monitor never shows; the monitor lists and changes values here
 * too (env_values_json, env_monitor_change). */
#include "../heap/heap.h"
#include "../platform/platform.h"
#include <ctype.h>

typedef struct EnvVal {
    size_t n;
    bool secret; /* the monitor never shows it (until the name is deleted) */
    uint8_t b[];
} EnvVal;

struct Env {
    _Atomic int refs;
    char *name; /* NULL: a program's own, or a process's */
    PlatLock *lock;
    StrMap vals; /* [lock] name -> EnvVal * */
    PidVec watchers; /* [lock] told of each change (env.watch) */
};

Env *env_new(const char *name) {
    Env *e = xcalloc(1, sizeof *e);
    atomic_store(&e->refs, 1);
    e->name = name ? xstrdup(name) : NULL;
    e->lock = plat_lock_new();
    return e;
}

void env_retain(Env *e) {
    if (e) atomic_fetch_add(&e->refs, 1);
}

void env_release(Env *e) {
    if (!e || atomic_fetch_sub(&e->refs, 1) != 1) return;
    for (size_t i = 0; i < e->vals.cap; i++)
        if (e->vals.keys && e->vals.keys[i]) free(e->vals.vals[i]);
    smap_free(&e->vals);
    VEC_FREE(e->watchers);
    plat_lock_free(e->lock);
    free(e->name);
    free(e);
}

/* A value set: secret if it is marked so, or the one it replaces was. */
static void put_locked(Env *e, const char *key, const uint8_t *b, size_t n, bool secret) {
    void *old = NULL;
    if (smap_get(&e->vals, key, &old) && old) secret = secret || ((EnvVal *)old)->secret, free(old);
    EnvVal *v = xmalloc(sizeof *v + n);
    v->n = n;
    v->secret = secret;
    if (n) memcpy(v->b, b, n);
    smap_put(&e->vals, key, v);
}

/* Every value of `from` into `to` (a program's from --env's files). */
void env_copy_into(Env *to, Env *from) {
    plat_lock(from->lock);
    plat_lock(to->lock);
    for (size_t i = 0; i < from->vals.cap; i++)
        if (from->vals.keys && from->vals.keys[i] && from->vals.vals[i]) {
            const EnvVal *v = from->vals.vals[i];
            put_locked(to, from->vals.keys[i], v->b, v->n, v->secret);
        }
    plat_unlock(to->lock);
    plat_unlock(from->lock);
}

/* ---- files of `name = JSON` lines ---- */

/* A scratch heap that never runs, for a file read before any program does (as
 * the scheduler's message fragments). */
static Process *scratch_new(VM *vm) {
    Process *f = xcalloc(1, sizeof *f);
    f->vm = vm;
    f->state = PROC_DONE;
    f->is_frag = true;
    for (int i = 0; i < QUOTA__COUNT; i++) f->quota[i] = UINT64_MAX;
    return f;
}

static void scratch_free(Process *f) {
    heap_free_all(f);
    sb_free(&f->backtrace); /* (a trap caught in it: env_values_json) */
    free(f);
}

static bool name_ok(const char *s, size_t n) {
    if (!n || !(isalpha((unsigned char)s[0]) || s[0] == '_')) return false;
    for (size_t i = 1; i < n; i++)
        if (!(isalnum((unsigned char)s[i]) || s[i] == '_' || s[i] == '.' || s[i] == '-')) return false;
    return true;
}

/* A file's text into e: `name = value` lines (`secret name = value`: one the
 * monitor never shows), the value JSON after each
 * ${VAR} is replaced by the operating system's environment variable VAR (empty
 * if it has none; escaped as JSON within a string, as a Windows path needs);
 * `#` lines and blank ones are skipped. Parsed and encoded in
 * heap's heap. False, with where and why, at the first line that is not one. */
static bool load_text(Process *heap, Env *e, const char *text, size_t len, const char *path, char *err, size_t errlen) {
    size_t line = 0;
    for (size_t at = 0; at < len;) {
        size_t end = at;
        while (end < len && text[end] != '\n') end++;
        line++;
        const char *s = text + at, *z = text + end;
        at = end + 1;
        while (s < z && isspace((unsigned char)*s)) s++;
        while (z > s && isspace((unsigned char)z[-1])) z--;
        if (s == z || *s == '#') continue;
        bool secret = false;
        if (z - s > 7 && !memcmp(s, "secret", 6) && isspace((unsigned char)s[6])) { /* (not "secret = ...": a name) */
            const char *n = s + 7;
            while (n < z && isspace((unsigned char)*n)) n++;
            if (n < z && *n != '=') secret = true, s = n;
        }
        const char *eq = memchr(s, '=', (size_t)(z - s));
        if (!eq) return snprintf(err, errlen, "%s:%zu: expected name = value", path, line), false;
        const char *ne = eq;
        while (ne > s && isspace((unsigned char)ne[-1])) ne--;
        if (!name_ok(s, (size_t)(ne - s)))
            return snprintf(err, errlen, "%s:%zu: a name is a letter or _, then letters, digits, _, . or -", path, line), false;
        char *key = xstrndup(s, (size_t)(ne - s));
        const char *v = eq + 1;
        while (v < z && isspace((unsigned char)*v)) v++;
        StrBuf json = {0};
        bool in_str = false; /* (within a JSON string: a variable's text goes in escaped) */
        for (const char *c = v; c < z; c++) { /* ${VAR} */
            if (in_str && *c == '\\' && c + 1 < z) {
                sb_putc(&json, *c++);
                sb_putc(&json, *c);
                continue;
            }
            if (*c == '"') in_str = !in_str;
            const char *close = c[0] == '$' && c + 1 < z && c[1] == '{' ? memchr(c + 2, '}', (size_t)(z - c - 2)) : NULL;
            if (!close) {
                sb_putc(&json, *c);
                continue;
            }
            char *var = xstrndup(c + 2, (size_t)(close - c - 2));
            const char *val = getenv(var);
            for (const char *s2 = val; s2 && *s2; s2++) {
                unsigned char ch = (unsigned char)*s2;
                if (!in_str) sb_putc(&json, (char)ch);
                else if (ch == '"' || ch == '\\') sb_putc(&json, '\\'), sb_putc(&json, (char)ch);
                else if (ch < 0x20) sb_printf(&json, "\\u%04x", ch);
                else sb_putc(&json, (char)ch);
            }
            free(var);
            c = close;
        }
        Word w;
        uint8_t tag;
        char why[200];
        if (!json.n || !text_json_parse(heap, json.s, json.n, &w, &tag, why, sizeof why)) {
            snprintf(err, errlen, "%s:%zu: %s: the value is not JSON%s%s", path, line, key, json.n ? ": " : "",
                     json.n ? why : "");
            sb_free(&json);
            free(key);
            return false;
        }
        sb_free(&json);
        ObjBytes *b = node_encode_value(heap, heap, w, tag);
        plat_lock(e->lock);
        put_locked(e, key, bytes_data(b), b->len, secret);
        plat_unlock(e->lock);
        free(key);
    }
    return true;
}

/* --env FILE: into the values every program starts with. */
bool vm_env_load_file(VM *vm, const char *path, char *err, size_t errlen) {
    size_t len;
    char *text = read_file(path, &len);
    if (!text) return snprintf(err, errlen, "%s: cannot read it", path), false;
    if (!vm->env_init) vm->env_init = env_new(NULL);
    Process *f = scratch_new(vm);
    bool ok = load_text(f, vm->env_init, text, len, path, err, errlen);
    scratch_free(f);
    free(text);
    return ok;
}

/* A program's own environment, from --env's files and, for mlwsvm serve, the
 * NAME.mlws.env beside its file, if there is one. */
bool vm_program_env(VM *vm, Program *pr, bool own_file, MlwsError *err) {
    pr->env = env_new(NULL);
    if (vm->env_init) env_copy_into(pr->env, vm->env_init);
    if (pr->main) {
        env_release(pr->main->env);
        pr->main->env = pr->env;
        env_retain(pr->env);
    }
    if (!own_file || !pr->path || !*pr->path) return true;
    StrBuf p = {0};
    const char *dot = strrchr(pr->path, '.'), *slash = strrchr(pr->path, '/');
    size_t stem = dot && (!slash || dot > slash) ? (size_t)(dot - pr->path) : strlen(pr->path);
    sb_putn(&p, pr->path, stem);
    sb_puts(&p, ".mlws.env");
    size_t len;
    char *text = read_file(p.s, &len);
    bool ok = true;
    if (text) {
        char why[600];
        Process *f = scratch_new(vm);
        ok = load_text(f, pr->env, text, len, p.s, why, sizeof why);
        scratch_free(f);
        if (!ok) mlws_err(err, "E_ENV", "%s", why);
        free(text);
    }
    sb_free(&p);
    return ok;
}

/* ---- the natives ---- */

/* The environment p belongs to: its program's, unless it joined another; one
 * of its own if it has neither (a process an embedding host started). */
static Env *env_of(Process *p) {
    if (!p->env) p->env = env_new(NULL);
    return p->env;
}

static const char *key_arg(Process *p, Word a, const char *fn) {
    const Obj *o = a.r;
    if (!o || o->kind != OBJ_STRING || !((const ObjString *)o)->len)
        vm_trap(p, "E_TYPE", "%s: a name is a non-empty string", fn);
    return ((const ObjString *)o)->data;
}

/* A restricted process reads, and changes nothing. */
static void may_change(Process *p, const char *fn) {
    if (p->caps) vm_trap(p, "E_PERMISSION", "%s: not allowed in a restricted process", fn);
}

/* The value under key, decoded into p's heap: its tag, or 0 if there is none. */
static uint8_t get(Process *p, const char *key, Word *out) {
    Env *e = env_of(p);
    plat_lock(e->lock);
    void *v = NULL;
    EnvVal *copy = NULL;
    if (smap_get(&e->vals, key, &v) && v) { /* (copied out: another may change it while it is decoded) */
        const EnvVal *ev = v;
        copy = xmalloc(sizeof *copy + ev->n);
        copy->n = ev->n;
        memcpy(copy->b, ev->b, ev->n);
    }
    plat_unlock(e->lock);
    if (!copy) return 0;
    ObjBytes *b = bytes_new(p, (int64_t)copy->n); /* (in the heap, so that a trap as it is decoded leaks nothing) */
    if (copy->n) memcpy(b->data, copy->b, copy->n);
    free(copy);
    return node_decode_value(p, bytes_data(b), b->len, out);
}

/* Tell e's watchers that key changed (with no lock held). */
static void tell(Process *p, Env *e, const char *key) {
    plat_lock(e->lock);
    PidVec to = {0};
    for (size_t i = 0; i < e->watchers.n; i++) VEC_PUSH(to, e->watchers.v[i]);
    plat_unlock(e->lock);
    if (!to.n) return;
    ObjStruct *c = struct_new(p, &VM_BUILTIN_DEFS[BUILTIN_ENVCHANGE]);
    c->fields[ECF_KEY].r = &str_new(p, key, strlen(key))->h;
    for (size_t i = 0; i < to.n; i++) sched_send(p, to.v[i], (Word){.r = c}, T_R);
    VEC_FREE(to);
}

static void set(Process *p, const char *fn, Word key, Word v, uint8_t tag) {
    may_change(p, fn);
    const char *k = key_arg(p, key, fn);
    ObjBytes *b = node_encode_value(p, p, v, tag); /* (E_TYPE for what cannot be sent) */
    Env *e = env_of(p);
    plat_lock(e->lock);
    put_locked(e, k, bytes_data(b), b->len, false);
    plat_unlock(e->lock);
    tell(p, e, k);
}

static MLWS_NORETURN void wrong(Process *p, const char *fn, const char *key, uint8_t tag, const char *use) {
    vm_trap(p, "E_TYPE", "%s: \"%s\" holds a %s (%s)", fn, key, type_name((MlwsType)tag), use);
}

static void n_get(Process *p, Word *a, Word *r) {
    const char *k = key_arg(p, a[0], "env.get");
    Word w;
    uint8_t t = get(p, k, &w);
    if (!t) {
        r[0] = a[1];
        return;
    }
    if (!type_is_ref((MlwsType)t)) wrong(p, "env.get", k, t, type_is_float((MlwsType)t) ? "env.get_f" : "env.get_i");
    r[0] = w;
}

static void n_get_i(Process *p, Word *a, Word *r) {
    const char *k = key_arg(p, a[0], "env.get_i");
    Word w;
    uint8_t t = get(p, k, &w);
    if (!t) {
        r[0] = a[1];
        return;
    }
    if (!type_is_int((MlwsType)t)) wrong(p, "env.get_i", k, t, type_is_float((MlwsType)t) ? "env.get_f" : "env.get");
    r[0] = w;
}

static void n_get_f(Process *p, Word *a, Word *r) {
    const char *k = key_arg(p, a[0], "env.get_f");
    Word w;
    uint8_t t = get(p, k, &w);
    if (!t) {
        r[0] = a[1];
        return;
    }
    if (!type_is_float((MlwsType)t)) wrong(p, "env.get_f", k, t, type_is_int((MlwsType)t) ? "env.get_i" : "env.get");
    r[0] = w;
}

/* env.get_any(name): any value, as the only element of a list (so that a
 * number or boolean keeps its type, for tagof); an empty list if there is
 * none (as json.parse_any). */
static void n_get_any(Process *p, Word *a, Word *r) {
    const char *k = key_arg(p, a[0], "env.get_any");
    Word w;
    uint8_t t = get(p, k, &w);
    ObjList *l = list_new(p, t ? 1 : 0);
    if (t) list_push(p, l, w, t);
    r[0].r = &l->h;
}

static void n_set(Process *p, Word *a, Word *r) {
    (void)r;
    set(p, "env.set", a[0], a[1], T_R);
}
static void n_set_i(Process *p, Word *a, Word *r) {
    (void)r;
    set(p, "env.set_i", a[0], a[1], T_I64);
}
static void n_set_f(Process *p, Word *a, Word *r) {
    (void)r;
    set(p, "env.set_f", a[0], a[1], T_F64);
}

static void n_has(Process *p, Word *a, Word *r) {
    const char *k = key_arg(p, a[0], "env.has");
    Env *e = env_of(p);
    void *v = NULL;
    plat_lock(e->lock);
    r[0].i = smap_get(&e->vals, k, &v) && v;
    plat_unlock(e->lock);
}

static void n_delete(Process *p, Word *a, Word *r) {
    (void)r;
    may_change(p, "env.delete");
    const char *k = key_arg(p, a[0], "env.delete");
    Env *e = env_of(p);
    void *v = NULL;
    plat_lock(e->lock);
    bool had = smap_get(&e->vals, k, &v) && v;
    if (had) free(v), smap_put(&e->vals, k, NULL);
    plat_unlock(e->lock);
    if (had) tell(p, e, k);
}

static void n_keys(Process *p, Word *a, Word *r) {
    (void)a;
    Env *e = env_of(p);
    StrVec ks = {0};
    plat_lock(e->lock);
    for (size_t i = 0; i < e->vals.cap; i++)
        if (e->vals.keys && e->vals.keys[i] && e->vals.vals[i]) VEC_PUSH(ks, xstrdup(e->vals.keys[i]));
    plat_unlock(e->lock);
    ObjList *l = list_new(p, (int64_t)ks.n);
    r[0].r = &l->h;
    for (size_t i = 0; i < ks.n; i++) {
        list_push(p, l, (Word){.r = &str_new(p, ks.v[i], strlen(ks.v[i]))->h}, T_R);
        free(ks.v[i]);
    }
    VEC_FREE(ks);
}

/* env.join(name): this process belongs to the environment of that name from
 * now on (made if there is none: it lasts as long as the VM), and so do the
 * processes it spawns; "" is its program's own again. */
static void n_join(Process *p, Word *a, Word *r) {
    (void)r;
    may_change(p, "env.join");
    const Obj *o = a[0].r;
    if (!o || o->kind != OBJ_STRING) vm_trap(p, "E_TYPE", "env.join: a name is a string");
    const ObjString *s = (const ObjString *)o;
    VM *vm = p->vm;
    Env *e;
    if (!s->len) {
        e = p->prog ? p->prog->env : NULL;
        if (e) env_retain(e);
        else e = env_new(NULL);
    } else {
        plat_lock(vm->lock);
        void *v = NULL;
        if (!smap_get(&vm->env_named, s->data, &v) || !v) {
            v = env_new(s->data);
            smap_put(&vm->env_named, s->data, v);
        }
        e = v;
        env_retain(e); /* (the VM keeps its own reference) */
        plat_unlock(vm->lock);
    }
    env_release(p->env);
    p->env = e;
}

static void n_name(Process *p, Word *a, Word *r) {
    (void)a;
    const char *n = env_of(p)->name;
    r[0].r = &str_new(p, n ? n : "", n ? strlen(n) : 0)->h;
}

/* env.watch(on): whether this process gets a built-in EnvChange {key} for
 * each value set or deleted in its environment (the one it is in now). */
static void n_watch(Process *p, Word *a, Word *r) {
    (void)r;
    Env *e = env_of(p);
    plat_lock(e->lock);
    size_t at = e->watchers.n;
    for (size_t i = 0; i < e->watchers.n; i++)
        if (e->watchers.v[i] == p->pid) at = i;
    if (a[0].i && at == e->watchers.n) VEC_PUSH(e->watchers, p->pid);
    if (!a[0].i && at < e->watchers.n) e->watchers.v[at] = e->watchers.v[--e->watchers.n];
    plat_unlock(e->lock);
}

/* env.load(path): a file of `name = JSON` lines into this process's
 * environment (as --env's). */
static void n_load(Process *p, Word *a, Word *r) {
    (void)r;
    may_change(p, "env.load");
    const char *path = key_arg(p, a[0], "env.load");
    if (p->os_calls >= p->quota[QUOTA_OS])
        vm_trap(p, "E_QUOTA", "OS call quota of %llu exceeded (env.load reads a file)", (unsigned long long)p->quota[QUOTA_OS]);
    p->os_calls++;
    size_t len;
    char *text = read_file(path, &len);
    if (!text) vm_trap(p, "E_NOT_FOUND", "env.load: cannot read %s", path);
    char why[600];
    bool ok = load_text(p, env_of(p), text, len, path, why, sizeof why);
    free(text);
    if (!ok) vm_trap(p, "E_ENV", "env.load: %s", why);
}

/* ---- for the monitor (tools/monitor.c) ---- */

const char *env_name(const Env *e) { return e->name; }

size_t env_watchers(Env *e) {
    plat_lock(e->lock);
    size_t n = e->watchers.n;
    plat_unlock(e->lock);
    return n;
}

static void jstr(StrBuf *b, const char *s) {
    sb_putc(b, '"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') sb_printf(b, "\\%c", c);
        else if (c < 0x20) sb_printf(b, "\\u%04x", c);
        else sb_putc(b, (char)c);
    }
    sb_putc(b, '"');
}

/* A trap's message in a scratch heap, into err. */
static void trap_text(Process *f, char *err, size_t errlen) {
    const ObjStruct *x = (const ObjStruct *)f->exception;
    const ObjString *m = x && x->h.kind == OBJ_STRUCT ? (const ObjString *)x->fields[ERRF_MESSAGE].r : NULL;
    snprintf(err, errlen, "%s", m ? m->data : "failed");
}

#define ENV_JSON_MAX 16384 /* a value's JSON longer than this is not shown */

/* A value as JSON, decoded into a scratch heap; false, with why, for one that
 * has no JSON form (a pid, a closure...) or is too long to show. */
static bool value_json(VM *vm, const uint8_t *bytes, size_t n, StrBuf *out, char *why, size_t whylen) {
    Process *f = scratch_new(vm);
    jmp_buf jb;
    f->jb = &jb;
    volatile bool ok = false;
    if (!VM_SETJMP(jb)) {
        ObjBytes *b = bytes_new(f, (int64_t)n);
        if (n) memcpy(b->data, bytes, n);
        Word w;
        uint8_t t = node_decode_value(f, bytes_data(b), b->len, &w);
        StrBuf j = {0};
        text_json_append(f, w, t, &j);
        if (j.n > ENV_JSON_MAX) {
            snprintf(why, whylen, "%zu bytes of JSON: too long to show", j.n);
        } else {
            sb_putn(out, j.s, j.n);
            ok = true;
        }
        sb_free(&j);
    } else {
        trap_text(f, why, whylen);
    }
    scratch_free(f);
    return ok;
}

typedef struct {
    char *key;
    bool secret;
    EnvVal *v; /* a copy; NULL when values are not shown */
} Shown;

static int shown_cmp(const void *a, const void *b) { return strcmp(((const Shown *)a)->key, ((const Shown *)b)->key); }

/* An environment's names, by name, as a JSON array: each one's "key" and
 * "secret", and, if values, its "value" (null for a secret, or with "note"
 * saying why it is not shown). */
void env_values_json(VM *vm, Env *e, bool values, StrBuf *b) {
    VEC(Shown) all = {0};
    plat_lock(e->lock);
    for (size_t i = 0; i < e->vals.cap; i++) {
        if (!e->vals.keys || !e->vals.keys[i] || !e->vals.vals[i]) continue;
        const EnvVal *v = e->vals.vals[i];
        Shown s = {xstrdup(e->vals.keys[i]), v->secret, NULL};
        if (values && !v->secret) {
            s.v = xmalloc(sizeof *s.v + v->n);
            memcpy(s.v, v, sizeof *v + v->n);
        }
        VEC_PUSH(all, s);
    }
    plat_unlock(e->lock);
    if (all.n) qsort(all.v, all.n, sizeof all.v[0], shown_cmp);
    sb_putc(b, '[');
    for (size_t i = 0; i < all.n; i++) {
        Shown *s = &all.v[i];
        sb_puts(b, i ? ",{\"key\":" : "{\"key\":");
        jstr(b, s->key);
        sb_printf(b, ",\"secret\":%s", s->secret ? "true" : "false");
        if (values) {
            char why[300];
            sb_puts(b, ",\"value\":");
            if (s->secret) {
                sb_puts(b, "null");
            } else if (!value_json(vm, s->v->b, s->v->n, b, why, sizeof why)) {
                sb_puts(b, "null,\"note\":");
                jstr(b, why);
            }
        }
        sb_putc(b, '}');
        free(s->key);
        free(s->v);
    }
    sb_putc(b, ']');
    VEC_FREE(all);
}

static Word envchange(Process *heap, void *key) {
    ObjStruct *c = struct_new(heap, &VM_BUILTIN_DEFS[BUILTIN_ENVCHANGE]);
    c->fields[ECF_KEY].r = &str_new(heap, key, strlen(key))->h;
    return (Word){.r = &c->h};
}

/* Tell e's watchers that key changed, from outside any process. */
static void tell_outside(VM *vm, Env *e, const char *key) {
    plat_lock(e->lock);
    PidVec to = {0};
    for (size_t i = 0; i < e->watchers.n; i++) VEC_PUSH(to, e->watchers.v[i]);
    plat_unlock(e->lock);
    for (size_t i = 0; i < to.n; i++) sched_post_build(vm, to.v[i], envchange, (void *)key);
    VEC_FREE(to);
}

static MapEntry *field(ObjMap *m, Process *f, const char *name) {
    MapEntry *x = map_find(m, (Word){.r = &str_new(f, name, strlen(name))->h}, T_R);
    return x && x->live ? x : NULL;
}

static const char *field_str(MapEntry *x) {
    const Obj *o = x && x->vtag == T_R ? x->val.r : NULL;
    return o && o->kind == OBJ_STRING ? ((const ObjString *)o)->data : NULL;
}

/* The monitor's POST /api/envs/set or /delete: a JSON object naming the
 * environment ("env": a named one's name, or "program": a program's number),
 * "key", and for a set, "value" (and "secret": true to mark it so). Its HTTP
 * status: with err for one that is not 200, and audit, for the log (never
 * the value). Watchers hear of it, as of env.set. */
int env_monitor_change(VM *vm, bool del, const char *body, size_t n, StrBuf *audit, char *err, size_t errlen) {
    Process *f = scratch_new(vm);
    jmp_buf jb;
    f->jb = &jb;
    Env *volatile e = NULL;
    volatile int status = 400;
    if (VM_SETJMP(jb)) {
        trap_text(f, err, errlen);
        goto done;
    }
    Word w;
    uint8_t t;
    char why[200];
    if (!text_json_parse(f, body, n, &w, &t, why, sizeof why)) {
        snprintf(err, errlen, "the body is not JSON: %s", why);
        goto done;
    }
    ObjMap *m = t == T_R && w.r && ((Obj *)w.r)->kind == OBJ_MAP ? (ObjMap *)w.r : NULL;
    MapEntry *ek = m ? field(m, f, "env") : NULL, *pk = m ? field(m, f, "program") : NULL;
    const char *key = m ? field_str(field(m, f, "key")) : NULL, *name = field_str(ek);
    MapEntry *vk = m ? field(m, f, "value") : NULL, *sk = m ? field(m, f, "secret") : NULL;
    if (!key || !name_ok(key, strlen(key)) || (!name && !(pk && type_is_int((MlwsType)pk->vtag))) || (!del && !vk)) {
        snprintf(err, errlen, "expected {\"env\": NAME or \"program\": NUMBER, \"key\": NAME%s}", del ? "" : ", \"value\": JSON");
        goto done;
    }
    plat_lock(vm->lock);
    void *found = NULL;
    if (name) {
        if (smap_get(&vm->env_named, name, &found)) e = found;
    } else if (pk->val.i >= 0 && (uint64_t)pk->val.i < vm->progs.n) {
        e = vm->progs.v[pk->val.i]->env;
    }
    if (e) env_retain(e);
    plat_unlock(vm->lock);
    if (!e) {
        status = 404;
        snprintf(err, errlen, "no such environment");
        goto done;
    }
    if (del) {
        void *v = NULL;
        plat_lock(e->lock);
        bool had = smap_get(&e->vals, key, &v) && v;
        if (had) free(v), smap_put(&e->vals, key, NULL);
        plat_unlock(e->lock);
        if (!had) {
            status = 404;
            snprintf(err, errlen, "no value of that name");
            goto done;
        }
    } else {
        ObjBytes *b = node_encode_value(f, f, vk->val, vk->vtag);
        plat_lock(e->lock);
        put_locked(e, key, bytes_data(b), b->len, sk && sk->vtag == T_B && sk->val.i);
        plat_unlock(e->lock);
    }
    tell_outside(vm, e, key);
    sb_puts(audit, "{\"env\":");
    if (name) jstr(audit, name);
    else sb_puts(audit, "null");
    if (name) sb_puts(audit, ",\"program\":null");
    else sb_printf(audit, ",\"program\":%lld", (long long)pk->val.i);
    sb_puts(audit, ",\"key\":");
    jstr(audit, key);
    sb_putc(audit, '}');
    status = 200;
done:
    env_release(e);
    scratch_free(f);
    return status;
}

#define N(name, np, pt, nr, rt, fn, doc) {name, np, pt, nr, rt, fn, doc, 0, NULL, NULL, NULL}
#define T(...) {__VA_ARGS__}

static const NativeDef ENV_NATIVES[] = {
    N("env.get", 2, T(T_R, T_R), 1, T(T_R), n_get,
      "A value of this process's environment that is a reference, as a copy of its own; the second argument if there is none (E_TYPE for a number: env.get_i, env.get_f)."),
    N("env.get_i", 2, T(T_R, T_I64), 1, T(T_I64), n_get_i, "An integer value of the environment, or the second argument if there is none."),
    N("env.get_f", 2, T(T_R, T_F64), 1, T(T_F64), n_get_f, "A float value of the environment, or the second argument if there is none."),
    N("env.get_any", 1, T(T_R), 1, T(T_R), n_get_any,
      "Any value of the environment, as the only element of a list, keeping its type (tagof tells it); an empty list if there is none."),
    N("env.set", 2, T(T_R, T_R), 0, T(0), n_set,
      "Set a value of the environment: any value a message can be, copied in; every process of the environment sees it at once. Not in a restricted process (E_PERMISSION)."),
    N("env.set_i", 2, T(T_R, T_I64), 0, T(0), n_set_i, "Set an integer value of the environment."),
    N("env.set_f", 2, T(T_R, T_F64), 0, T(0), n_set_f, "Set a float value of the environment."),
    N("env.has", 1, T(T_R), 1, T(T_B), n_has, "Whether the environment has a value of that name."),
    N("env.delete", 1, T(T_R), 0, T(0), n_delete, "Remove a value of the environment."),
    N("env.keys", 0, T(0), 1, T(T_R), n_keys, "The names of the environment's values, as a list."),
    N("env.join", 1, T(T_R), 0, T(0), n_join,
      "Belong to the environment of that name from now on (made if there is none; it lasts as long as the VM), as do the processes spawned from now on; \"\": the program's own again."),
    N("env.name", 0, T(0), 1, T(T_R), n_name, "The name of this process's environment (\"\": its program's own)."),
    N("env.watch", 1, T(T_B), 0, T(0), n_watch,
      "Whether this process gets a built-in EnvChange {key} for each value set or deleted in its environment."),
    N("env.load", 1, T(T_R), 0, T(0), n_load,
      "Load a file of `name = JSON` lines (${VAR}: the operating system's environment variable) into the environment."),
};

void natives_register_env(VM *vm) {
    for (size_t i = 0; i < sizeof ENV_NATIVES / sizeof ENV_NATIVES[0]; i++) vm_register_native(vm, &ENV_NATIVES[i]);
}

const NativeDef *natives_env_table(size_t *n) {
    *n = sizeof ENV_NATIVES / sizeof ENV_NATIVES[0];
    return ENV_NATIVES;
}
