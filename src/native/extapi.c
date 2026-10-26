/* extapi.c - calling foreign natives (extensions and the FFI), and the API
 * table handed to extensions (include/mlwsvm/mlwsvm.h).
 *
 * Foreign code must never be unwound with longjmp (it may hold locks or
 * resources of its own), so nothing here traps while it runs: API misuse is
 * recorded as an error, heap quotas are not enforced (Process.in_foreign), and
 * both are raised by native_complete once the foreign code has returned.
 * The collector only runs between instructions, so values need no rooting. */
#include "extapi.h"
#include "../heap/heap.h"
#include "../platform/platform.h"

#define MAX_LEN (((size_t)1 << 31) - 1) /* objects.c's element limit */

static Process *P(MlwsCtx *cx) { return (Process *)cx; }

static void record(Process *p, const char *code, const char *fmt, va_list ap) {
    if (p->foreign_err) return; /* the first error wins */
    p->foreign_err = true;
    snprintf(p->foreign_code, sizeof p->foreign_code, "%s", code && *code ? code : "E_NATIVE");
    vsnprintf(p->foreign_msg, sizeof p->foreign_msg, fmt, ap);
}

static void fail(Process *p, const char *code, const char *fmt, ...) MLWS_PRINTF(3, 4);
static void fail(Process *p, const char *code, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    record(p, code, fmt, ap);
    va_end(ap);
}

void native_fail(Process *p, const char *code, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    record(p, code, fmt, ap);
    va_end(ap);
}

/* ---- calling ------------------------------------------------------------------ */

void native_count_call(Process *p, const NativeDef *nd) {
    if (p->resumed) return; /* run again after a wait: still the same call */
    if (nd->flags & NF_FOREIGN) {
        if (p->foreign_calls >= p->quota[QUOTA_FOREIGN])
            vm_trap(p, "E_QUOTA", "foreign call quota of %llu exceeded", (unsigned long long)p->quota[QUOTA_FOREIGN]);
        p->foreign_calls++;
    }
    if (nd->flags & NF_OS) {
        if (p->os_calls >= p->quota[QUOTA_OS])
            vm_trap(p, "E_QUOTA", "OS call quota of %llu exceeded (files, commands, environment, network)",
                    (unsigned long long)p->quota[QUOTA_OS]);
        p->os_calls++;
    }
}

void native_wait_fd(Process *p, int fd, int events, int64_t timeout_ms) {
    p->native_req = NREQ_WAIT_FD;
    p->fdwait.fd = fd;
    p->fdwait.events = events;
    p->fdwait.timed_out = false;
    /* A timeout covers the whole operation, across however many waits it takes. */
    if (!p->fdwait.deadline && timeout_ms >= 0) p->fdwait.deadline = plat_now_ns() + (uint64_t)timeout_ms * 1000000u;
}

void native_run_blocking(Process *p) { p->native_req = NREQ_BLOCKING; }

void native_invoke(Process *p, const NativeDef *nd, Word *args, Word *rets) {
    PlatLock *serial = nd->flags & NF_SERIAL ? nd->lib->serial : NULL;
    p->cur_native = nd;
    p->in_foreign = true;
    p->foreign_err = false;
    p->native_req = NREQ_NONE;
    if (serial) plat_lock(serial);
    nd->fn(p, args, rets);
    if (serial) plat_unlock(serial);
    p->in_foreign = false;
    p->cur_native = NULL;
}

void native_complete(Process *p) {
    if (p->foreign_err) {
        p->foreign_err = false;
        vm_trap(p, p->foreign_code, "%s", p->foreign_msg);
    }
    if (p->heap_bytes > p->quota[QUOTA_HEAP] && !p->raising)
        vm_trap(p, "E_QUOTA", "heap quota of %llu bytes exceeded", (unsigned long long)p->quota[QUOTA_HEAP]);
}

void native_call(Process *p, const NativeDef *nd, Word *args, Word *rets) {
    native_count_call(p, nd);
    native_invoke(p, nd, args, rets);
    if (p->foreign_err) { /* an error ends the call: the next one starts afresh */
        p->native_req = NREQ_NONE;
        p->resumed = false;
        p->fdwait.deadline = 0;
    }
    native_complete(p);
}

void foreign_finalize(Obj *o) {
    ObjForeign *f = (ObjForeign *)o;
    const MlwsForeignType *t = f->type;
    if (!f->ptr || !t->finalize) return;
    void *ptr = f->ptr;
    f->ptr = NULL;
    PlatLock *serial = f->lib ? f->lib->serial : NULL;
    if (serial) plat_lock(serial);
    t->finalize(ptr);
    if (serial) plat_unlock(serial);
}

/* ---- the API: values ------------------------------------------------------------ */

static void api_error(MlwsCtx *cx, const char *code, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    record(P(cx), code, fmt, ap);
    va_end(ap);
}

static void *api_native_data(MlwsCtx *cx) { return P(cx)->cur_native ? P(cx)->cur_native->data : NULL; }
static uint64_t api_self_pid(MlwsCtx *cx) { return P(cx)->pid; }
static int api_kind(MlwsCtx *cx, MlwsValue v) { return v.r ? ((Obj *)v.r)->kind : OBJ_NULL; }

static Obj *want(Process *p, MlwsValue v, int kind, const char *what) {
    Obj *o = v.r;
    if (o && o->kind == kind) return o;
    fail(p, "E_TYPE", "%s: expected a %s, got %s", p->cur_native ? p->cur_native->name : "native", obj_kind_name(kind),
         o ? obj_kind_name(o->kind) : "null");
    (void)what;
    return NULL;
}

static const char *api_str_data(MlwsCtx *cx, MlwsValue v, size_t *len) {
    Obj *o = v.r;
    if (o && o->kind == OBJ_STRING) {
        if (len) *len = ((ObjString *)o)->len;
        return ((ObjString *)o)->data;
    }
    if (o && o->kind == OBJ_BYTES) {
        if (len) *len = ((ObjBytes *)o)->len;
        return (const char *)((ObjBytes *)o)->data;
    }
    want(P(cx), v, OBJ_STRING, "string");
    if (len) *len = 0;
    return NULL;
}

static MlwsValue api_str_new(MlwsCtx *cx, const char *s, size_t len) {
    MlwsValue r = {.r = NULL};
    if (len > MAX_LEN) fail(P(cx), "E_BAD_ALLOC", "string of %zu bytes is too long", len);
    else r.r = str_new(P(cx), s ? s : "", s ? len : 0);
    return r;
}

static MlwsValue api_bytes_new(MlwsCtx *cx, const void *data, size_t len) {
    MlwsValue r = {.r = NULL};
    if (len > MAX_LEN) {
        fail(P(cx), "E_BAD_ALLOC", "byte buffer of %zu bytes is too long", len);
        return r;
    }
    ObjBytes *b = bytes_new(P(cx), (int64_t)len);
    if (len && data) memcpy(b->data, data, len);
    r.r = b;
    return r;
}

static uint8_t *api_bytes_data(MlwsCtx *cx, MlwsValue v, size_t *len) {
    ObjBytes *b = (ObjBytes *)want(P(cx), v, OBJ_BYTES, "bytes");
    if (len) *len = b ? b->len : 0;
    return b ? b->data : NULL;
}

static bool valid_type(Process *p, uint8_t t) {
    if (t >= T_B && t <= T_R) return true;
    fail(p, "E_TYPE", "%s: invalid value type %u", p->cur_native ? p->cur_native->name : "native", t);
    return false;
}

static MlwsValue api_list_new(MlwsCtx *cx) { return (MlwsValue){.r = list_new(P(cx), 0)}; }

static int64_t api_list_len(MlwsCtx *cx, MlwsValue v) {
    ObjList *l = (ObjList *)want(P(cx), v, OBJ_LIST, "list");
    return l ? (int64_t)l->len : -1;
}

static bool api_list_get(MlwsCtx *cx, MlwsValue v, int64_t i, MlwsValue *out, uint8_t *type) {
    ObjList *l = (ObjList *)want(P(cx), v, OBJ_LIST, "list");
    if (!l || i < 0 || i >= l->len) return false;
    if (out) memcpy(out, &l->items[i], sizeof *out);
    if (type) *type = l->tags[i];
    return true;
}

static bool api_list_push(MlwsCtx *cx, MlwsValue v, MlwsValue x, uint8_t type) {
    Process *p = P(cx);
    ObjList *l = (ObjList *)want(p, v, OBJ_LIST, "list");
    if (!l || !valid_type(p, type)) return false;
    if ((size_t)l->len + 1 >= MAX_LEN) {
        fail(p, "E_BAD_ALLOC", "list too large");
        return false;
    }
    Word w;
    memcpy(&w, &x, sizeof w);
    list_push(p, l, w, type);
    return true;
}

static MlwsValue api_map_new(MlwsCtx *cx) { return (MlwsValue){.r = map_new(P(cx))}; }

static bool api_map_get(MlwsCtx *cx, MlwsValue m, MlwsValue key, uint8_t kt, MlwsValue *out, uint8_t *vt) {
    Process *p = P(cx);
    ObjMap *map = (ObjMap *)want(p, m, OBJ_MAP, "map");
    if (!map || !valid_type(p, kt)) return false;
    Word k;
    memcpy(&k, &key, sizeof k);
    MapEntry *e = map_find(map, k, kt);
    if (!e) return false;
    if (out) memcpy(out, &e->val, sizeof *out);
    if (vt) *vt = e->vtag;
    return true;
}

static bool api_map_set(MlwsCtx *cx, MlwsValue m, MlwsValue key, uint8_t kt, MlwsValue val, uint8_t vt) {
    Process *p = P(cx);
    ObjMap *map = (ObjMap *)want(p, m, OBJ_MAP, "map");
    if (!map || !valid_type(p, kt) || !valid_type(p, vt)) return false;
    if ((size_t)map->count + 2 >= MAX_LEN) {
        fail(p, "E_BAD_ALLOC", "map too large");
        return false;
    }
    Word k, v;
    memcpy(&k, &key, sizeof k);
    memcpy(&v, &val, sizeof v);
    map_set(p, map, k, kt, v, vt);
    return true;
}

/* ---- the API: foreign handles ------------------------------------------------------ */

ObjForeign *foreign_new_obj(Process *p, const void *type, void *ptr, ExtLib *lib) {
    ObjForeign *f = (ObjForeign *)gc_alloc(p, OBJ_FOREIGN, sizeof(ObjForeign));
    f->type = type;
    f->ptr = ptr;
    f->lib = lib;
    return f;
}

static MlwsValue api_foreign_new(MlwsCtx *cx, const MlwsForeignType *type, void *ptr) {
    Process *p = P(cx);
    if (!type || !type->name) {
        fail(p, "E_TYPE", "foreign_new needs a foreign type with a name");
        return (MlwsValue){.r = NULL};
    }
    return (MlwsValue){.r = foreign_new_obj(p, type, ptr, p->cur_native ? p->cur_native->lib : NULL)};
}

static void *api_foreign_get(MlwsCtx *cx, MlwsValue v, const MlwsForeignType *type) {
    Process *p = P(cx);
    ObjForeign *f = (ObjForeign *)want(p, v, OBJ_FOREIGN, "foreign handle");
    if (!f) return NULL;
    if (f->type != type) {
        fail(p, "E_TYPE", "expected a foreign %s, got a foreign %s", type ? type->name : "?",
             ((const MlwsForeignType *)f->type)->name);
        return NULL;
    }
    if (!f->ptr) fail(p, "E_TYPE", "the foreign %s has already been released", type->name);
    return f->ptr;
}

static void api_foreign_release(MlwsCtx *cx, MlwsValue v) {
    Obj *o = want(P(cx), v, OBJ_FOREIGN, "foreign handle");
    if (o) foreign_finalize(o);
}

/* ---- the API: init ---------------------------------------------------------------- */

/* MlwsCtx is the Process and MlwsValue has Word's layout; only the function
 * type differs, so call through the right one. */
static void ext_trampoline(Process *p, Word *args, Word *rets) {
    ((MlwsNativeFn)p->cur_native->xfn)((MlwsCtx *)p, (const MlwsValue *)(void *)args, (MlwsValue *)(void *)rets);
}

struct MlwsExtension {
    VM *vm;
    ExtLib *lib;
    MlwsError *err;
    bool failed;
};

static void api_init_error(MlwsExtension *ext, const char *fmt, ...) {
    if (ext->failed) return;
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    mlws_err(ext->err, "E_LINK_EXT", "extension %s: %s", ext->lib->name, msg);
    ext->failed = true;
}

/* Check and register a native described with the extension API's types: an
 * extension's (foreign: quota 4), or the embedding host's. On failure, `why`. */
bool native_define(VM *vm, ExtLib *lib, const MlwsNativeDesc *d, bool foreign, char *why, size_t whylen) {
    if (!d || !d->name || !*d->name || !strchr(d->name, '.')) {
        snprintf(why, whylen, "native '%s' needs a prefix (module.name)", d && d->name ? d->name : "(null)");
        return false;
    }
    if (!d->fn || d->nparams > MLWS_MAX_PARAMS || d->nresults > MLWS_MAX_RESULTS) {
        snprintf(why, whylen, "native %s: missing function, or too many parameters or results", d->name);
        return false;
    }
    for (int k = 0; k < d->nparams + d->nresults; k++) {
        uint8_t t = k < d->nparams ? d->params[k] : d->results[k - d->nparams];
        if (t < MLWS_T_B || t > MLWS_T_R) {
            snprintf(why, whylen, "native %s: invalid type code %u", d->name, t);
            return false;
        }
    }
    if (d->flags & ~(uint32_t)(MLWS_NATIVE_BLOCKING | MLWS_NATIVE_SERIAL)) {
        snprintf(why, whylen, "native %s: unknown flags 0x%x", d->name, d->flags);
        return false;
    }
    void *old;
    if (smap_get(&vm->natives, d->name, &old)) {
        snprintf(why, whylen, "native %s is already defined", d->name);
        return false;
    }
    NativeDef *nd = xcalloc(1, sizeof *nd);
    nd->name = xstrdup(d->name);
    nd->doc = xstrdup(d->doc ? d->doc : "");
    nd->np = d->nparams;
    memcpy(nd->pt, d->params, d->nparams);
    nd->nr = d->nresults;
    memcpy(nd->rt, d->results, d->nresults);
    nd->fn = ext_trampoline;
    nd->xfn = (void (*)(void))d->fn;
    nd->data = d->data;
    nd->lib = lib;
    nd->flags = (foreign ? NF_FOREIGN : 0) | NF_CHECKED | (d->flags & MLWS_NATIVE_BLOCKING ? NF_BLOCKING : 0) |
                (d->flags & MLWS_NATIVE_SERIAL ? NF_SERIAL : 0);
    VEC_PUSH(vm->foreign_natives, nd);
    vm_register_native(vm, nd);
    return true;
}

static bool api_define(MlwsExtension *ext, const MlwsNativeDesc *d) {
    if (ext->failed) return false;
    size_t nl = strlen(ext->lib->name);
    if (!d || !d->name || strncmp(d->name, ext->lib->name, nl) != 0 || d->name[nl] != '.' || !d->name[nl + 1]) {
        api_init_error(ext, "native '%s' must be named %s.something", d && d->name ? d->name : "(null)", ext->lib->name);
        return false;
    }
    char why[512];
    if (!native_define(ext->vm, ext->lib, d, true, why, sizeof why)) {
        api_init_error(ext, "%s", why);
        return false;
    }
    return true;
}


static const MlwsApi API = {
    .version = MLWSVM_API_VERSION,
    .size = sizeof(MlwsApi),
    .define = api_define,
    .init_error = api_init_error,
    .error = api_error,
    .native_data = api_native_data,
    .self_pid = api_self_pid,
    .kind = api_kind,
    .str_data = api_str_data,
    .str_new = api_str_new,
    .bytes_new = api_bytes_new,
    .bytes_data = api_bytes_data,
    .list_new = api_list_new,
    .list_len = api_list_len,
    .list_get = api_list_get,
    .list_push = api_list_push,
    .map_new = api_map_new,
    .map_get = api_map_get,
    .map_set = api_map_set,
    .foreign_new = api_foreign_new,
    .foreign_get = api_foreign_get,
    .foreign_release = api_foreign_release,
};

const MlwsApi *native_api(void) { return &API; }

/* Run an extension's init function; on failure `err` says why. */
bool ext_run_init(VM *vm, ExtLib *lib, void *init, MlwsError *err) {
    MlwsExtension ext = {vm, lib, err, false};
    MlwsExtensionInitFn fn; /* ISO C has no object-to-function pointer cast; dlsym's result is a function */
    memcpy(&fn, &init, sizeof fn);
    int rc = fn(&API, &ext);
    if (rc != 0 && !ext.failed) mlws_err(err, "E_LINK_EXT", "extension %s: initialisation failed (%d)", lib->name, rc);
    return rc == 0 && !ext.failed;
}
