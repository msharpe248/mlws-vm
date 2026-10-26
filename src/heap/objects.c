/* objects.c - construction and operations for each heap object kind. Every
 * size change goes through gc_account so heap statistics and GC triggering
 * stay accurate. Limits trap with E_BAD_ALLOC rather than exhausting the host. */
#include <math.h>
#include "../../include/mlwsvm/mlwsvm.h"

#include "heap.h"
#include "numeric.h"

#define MAX_ELEMS ((int64_t)1 << 31)

const char *obj_kind_name(int kind) {
    static const char *const names[] = {"null", "string", "bytes",  "array",   "struct", "list",
                                        "map",  "closure", "bigint", "decimal", "foreign"};
    return kind >= 0 && kind <= OBJ_FOREIGN ? names[kind] : "object";
}

static void check_len(Process *p, int64_t n, const char *what) {
    if (n < 0 || n >= MAX_ELEMS) vm_trap(p, "E_BAD_ALLOC", "invalid %s length %lld", what, (long long)n);
}

/* ---- strings and bytes ---------------------------------------------------- */

ObjString *str_new(Process *p, const char *s, size_t n) {
    check_len(p, (int64_t)n, "string");
    ObjString *o = (ObjString *)gc_alloc(p, OBJ_STRING, sizeof(ObjString) + n + 1);
    o->len = (uint32_t)n;
    if (n) memcpy(o->data, s, n);
    o->data[n] = 0;
    o->hash = hash_bytes(s, n);
    return o;
}

ObjBytes *bytes_new(Process *p, int64_t len) {
    check_len(p, len, "bytes");
    ObjBytes *b = (ObjBytes *)gc_alloc(p, OBJ_BYTES, sizeof(ObjBytes));
    if (len) {
        b->data = xcalloc((size_t)len, 1);
        b->cap = b->len = (uint32_t)len;
        gc_account(p, len);
    }
    return b;
}

static void bytes_reserve(Process *p, ObjBytes *b, uint64_t need) {
    if (need <= b->cap) return;
    check_len(p, (int64_t)need, "bytes");
    uint64_t nc = b->cap ? b->cap : 16;
    while (nc < need) nc *= 2;
    if (nc >= (uint64_t)MAX_ELEMS) nc = (uint64_t)MAX_ELEMS - 1;
    b->data = xrealloc(b->data, (size_t)nc);
    memset(b->data + b->cap, 0, (size_t)(nc - b->cap));
    uint64_t grown = nc - b->cap;
    b->cap = (uint32_t)nc; /* consistent before accounting, which may trap E_QUOTA */
    gc_account(p, (int64_t)grown);
}

void bytes_append(Process *p, ObjBytes *b, const void *data, size_t n) {
    /* `data` may point into b itself (bufcat buf, buf), so re-derive it after growth. */
    const uint8_t *d = data;
    bool self = b->data && d >= b->data && d < b->data + b->cap;
    size_t off = self ? (size_t)(d - b->data) : 0;
    bytes_reserve(p, b, (uint64_t)b->len + n);
    if (self) data = b->data + off;
    if (n) memmove(b->data + b->len, data, n);
    b->len += (uint32_t)n;
}

void bytes_resize(Process *p, ObjBytes *b, int64_t len) {
    check_len(p, len, "bytes");
    bytes_reserve(p, b, (uint64_t)len);
    if ((uint32_t)len < b->len) memset(b->data + len, 0, b->len - (uint32_t)len);
    b->len = (uint32_t)len;
}

/* ---- arrays and structs ---------------------------------------------------- */

ObjArray *array_new(Process *p, MlwsType elem, int64_t len) {
    check_len(p, len, "array");
    ObjArray *a = (ObjArray *)gc_alloc(p, OBJ_ARRAY, sizeof(ObjArray) + (size_t)len * sizeof(Word));
    a->elem = (uint8_t)elem;
    a->len = (uint32_t)len;
    return a;
}

ObjStruct *struct_new(Process *p, const StructDef *def) {
    ObjStruct *s = (ObjStruct *)gc_alloc(p, OBJ_STRUCT, sizeof(ObjStruct) + def->nfields * sizeof(Word));
    s->def = def;
    return s;
}

/* ---- lists ------------------------------------------------------------------ */

ObjList *list_new(Process *p, int64_t cap) {
    check_len(p, cap, "list");
    ObjList *l = (ObjList *)gc_alloc(p, OBJ_LIST, sizeof(ObjList));
    if (cap) {
        l->items = xcalloc((size_t)cap, sizeof(Word));
        l->tags = xcalloc((size_t)cap, 1);
        l->cap = (uint32_t)cap;
        gc_account(p, cap * (int64_t)(sizeof(Word) + 1));
    }
    return l;
}

static void list_reserve(Process *p, ObjList *l, uint64_t need) {
    if (need <= l->cap) return;
    check_len(p, (int64_t)need, "list");
    uint64_t nc = l->cap ? l->cap : 8;
    while (nc < need) nc *= 2;
    if (nc >= (uint64_t)MAX_ELEMS) nc = (uint64_t)MAX_ELEMS - 1;
    l->items = xrealloc(l->items, (size_t)nc * sizeof(Word));
    l->tags = xrealloc(l->tags, (size_t)nc);
    memset(l->items + l->cap, 0, (size_t)(nc - l->cap) * sizeof(Word));
    memset(l->tags + l->cap, 0, (size_t)(nc - l->cap));
    uint64_t grown = nc - l->cap;
    l->cap = (uint32_t)nc; /* consistent before accounting, which may trap E_QUOTA */
    gc_account(p, (int64_t)grown * (int64_t)(sizeof(Word) + 1));
}

void list_push(Process *p, ObjList *l, Word v, uint8_t tag) {
    list_reserve(p, l, (uint64_t)l->len + 1);
    l->items[l->len] = v;
    l->tags[l->len] = tag;
    l->len++;
}

void list_insert(Process *p, ObjList *l, int64_t at, Word v, uint8_t tag) {
    if (at < 0 || at > (int64_t)l->len)
        vm_trap(p, "E_BOUNDS", "list insert position %lld out of range (length %u)", (long long)at, l->len);
    list_reserve(p, l, (uint64_t)l->len + 1);
    memmove(l->items + at + 1, l->items + at, (l->len - (size_t)at) * sizeof(Word));
    memmove(l->tags + at + 1, l->tags + at, l->len - (size_t)at);
    l->items[at] = v;
    l->tags[at] = tag;
    l->len++;
}

void list_delete(Process *p, ObjList *l, int64_t at) {
    if (at < 0 || at >= (int64_t)l->len)
        vm_trap(p, "E_BOUNDS", "list index %lld out of range (length %u)", (long long)at, l->len);
    memmove(l->items + at, l->items + at + 1, (l->len - (size_t)at - 1) * sizeof(Word));
    memmove(l->tags + at, l->tags + at + 1, l->len - (size_t)at - 1);
    l->len--;
    l->items[l->len].u = 0;
    l->tags[l->len] = T_NONE;
}

void list_resize(Process *p, ObjList *l, int64_t len) {
    check_len(p, len, "list");
    list_reserve(p, l, (uint64_t)len);
    for (uint32_t i = (uint32_t)len; i < l->len; i++) {
        l->items[i].u = 0;
        l->tags[i] = T_NONE;
    }
    l->len = (uint32_t)len;
}

/* ---- maps ------------------------------------------------------------------ */

enum { KC_INT, KC_FLOAT, KC_REF, KC_PTR };

static int key_class(uint8_t tag) {
    if (tag == T_R) return KC_REF;
    if (tag == T_P) return KC_PTR;
    if (tag == T_F32 || tag == T_F64) return KC_FLOAT;
    return KC_INT; /* all integer-class types (and untagged zero) */
}

static uint32_t mix64(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ull;
    x ^= x >> 33;
    return (uint32_t)x;
}

static uint32_t key_hash(Word k, uint8_t tag) {
    switch (key_class(tag)) {
    case KC_FLOAT: {
        if (k.f == 0) return 0; /* +0 and -0 are the same key */
        uint64_t bits;
        memcpy(&bits, &k.f, 8);
        return mix64(bits);
    }
    case KC_REF: {
        Obj *o = k.r;
        if (o && o->kind == OBJ_STRING) return ((ObjString *)o)->hash;
        if (o && (o->kind == OBJ_BIG || o->kind == OBJ_DEC)) return num_hash(o); /* by value */
        return mix64((uint64_t)(uintptr_t)o);
    }
    default: return mix64(k.u);
    }
}

static bool key_eq(Word a, uint8_t at, Word b, uint8_t bt) {
    int ca = key_class(at);
    if (ca != key_class(bt)) return false;
    switch (ca) {
    case KC_FLOAT: return a.f == b.f || (isnan(a.f) && isnan(b.f));
    case KC_REF: {
        Obj *x = a.r, *y = b.r;
        if (x == y) return true;
        if (x && y && x->kind == y->kind && (x->kind == OBJ_BIG || x->kind == OBJ_DEC)) return num_equal(x, y);
        if (!x || !y || x->kind != OBJ_STRING || y->kind != OBJ_STRING) return false;
        const ObjString *sx = (const ObjString *)x, *sy = (const ObjString *)y;
        return sx->len == sy->len && sx->hash == sy->hash && memcmp(sx->data, sy->data, sx->len) == 0;
    }
    default: return a.u == b.u;
    }
}

ObjMap *map_new(Process *p) { return (ObjMap *)gc_alloc(p, OBJ_MAP, sizeof(ObjMap)); }

/* Index slot holding the key's entry, or -1. */
static int64_t find_slot(const ObjMap *m, Word key, uint8_t ktag, uint32_t h) {
    if (!m->index_cap) return -1;
    uint32_t mask = m->index_cap - 1;
    for (uint32_t i = h & mask;; i = (i + 1) & mask) {
        int32_t e = m->index[i];
        if (e == -1) return -1;
        if (e >= 0) {
            const MapEntry *me = &m->entries[e];
            if (me->hash == h && key_eq(me->key, me->ktag, key, ktag)) return i;
        }
    }
}

MapEntry *map_find(ObjMap *m, Word key, uint8_t ktag) {
    int64_t slot = find_slot(m, key, ktag, key_hash(key, ktag));
    return slot < 0 ? NULL : &m->entries[m->index[slot]];
}

/* Compact live entries into a table of `cap` entries and rebuild the index. */
static void map_rebuild(Process *p, ObjMap *m, uint32_t cap) {
    size_t before = obj_footprint(&m->h);
    MapEntry *ne = xcalloc(cap, sizeof(MapEntry));
    uint32_t n = 0;
    for (uint32_t i = 0; i < m->nentries; i++)
        if (m->entries[i].live) ne[n++] = m->entries[i];
    free(m->entries);
    m->entries = ne;
    m->entry_cap = cap;
    m->nentries = n;
    uint32_t icap = 8;
    while (icap < cap * 2) icap *= 2;
    free(m->index);
    m->index = xmalloc(icap * sizeof(int32_t));
    for (uint32_t i = 0; i < icap; i++) m->index[i] = -1;
    m->index_cap = icap;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t j = ne[i].hash & (icap - 1);
        while (m->index[j] != -1) j = (j + 1) & (icap - 1);
        m->index[j] = (int32_t)i;
    }
    gc_account(p, (int64_t)obj_footprint(&m->h) - (int64_t)before);
}

void map_set(Process *p, ObjMap *m, Word key, uint8_t ktag, Word val, uint8_t vtag) {
    uint32_t h = key_hash(key, ktag);
    int64_t slot = find_slot(m, key, ktag, h);
    if (slot >= 0) {
        MapEntry *e = &m->entries[m->index[slot]];
        e->val = val;
        e->vtag = vtag;
        return;
    }
    if (m->nentries == m->entry_cap) {
        uint32_t cap = m->count < m->entry_cap / 2 ? m->entry_cap : (m->entry_cap ? m->entry_cap * 2 : 8);
        if (m->count + 1 >= (uint32_t)MAX_ELEMS) vm_trap(p, "E_BAD_ALLOC", "map too large");
        map_rebuild(p, m, cap);
    }
    uint32_t idx = m->nentries++;
    MapEntry *e = &m->entries[idx];
    e->key = key;
    e->ktag = ktag;
    e->val = val;
    e->vtag = vtag;
    e->hash = h;
    e->live = 1;
    m->count++;
    uint32_t mask = m->index_cap - 1;
    uint32_t j = h & mask;
    while (m->index[j] >= 0) j = (j + 1) & mask; /* reuse deleted (-2) or empty (-1) slots */
    m->index[j] = (int32_t)idx;
}

bool map_delete(ObjMap *m, Word key, uint8_t ktag) {
    int64_t slot = find_slot(m, key, ktag, key_hash(key, ktag));
    if (slot < 0) return false;
    MapEntry *e = &m->entries[m->index[slot]];
    e->live = 0;
    e->key.u = e->val.u = 0;
    e->ktag = e->vtag = T_NONE;
    m->index[slot] = -2;
    m->count--;
    return true;
}

ObjList *map_keys(Process *p, ObjMap *m) {
    ObjList *l = list_new(p, m->count);
    for (uint32_t i = 0; i < m->nentries; i++)
        if (m->entries[i].live) list_push(p, l, m->entries[i].key, m->entries[i].ktag);
    return l;
}

/* ---- tostr --------------------------------------------------------------- */

static ObjString *str_utf8(Process *p, uint32_t cp) {
    char buf[4];
    int n;
    if (cp < 0x80) buf[0] = (char)cp, n = 1;
    else if (cp < 0x800) buf[0] = (char)(0xC0 | (cp >> 6)), buf[1] = (char)(0x80 | (cp & 0x3F)), n = 2;
    else if (cp < 0x10000)
        buf[0] = (char)(0xE0 | (cp >> 12)), buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F)),
        buf[2] = (char)(0x80 | (cp & 0x3F)), n = 3;
    else
        buf[0] = (char)(0xF0 | (cp >> 18)), buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F)),
        buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F)), buf[3] = (char)(0x80 | (cp & 0x3F)), n = 4;
    return str_new(p, buf, (size_t)n);
}

ObjString *value_to_string(Process *p, Word v, MlwsType t) {
    char buf[96];
    switch (t) {
    case T_B: return str_new(p, v.i ? "true" : "false", v.i ? 4 : 5);
    case T_C: return str_utf8(p, (uint32_t)v.u);
    case T_U8: case T_U16: case T_U32: case T_U64:
        snprintf(buf, sizeof buf, "%llu", (unsigned long long)v.u);
        break;
    case T_F32: case T_F64: fmt_double(v.f, buf, sizeof buf); break;
    case T_P:
        if (!v.p) snprintf(buf, sizeof buf, "null");
        else snprintf(buf, sizeof buf, "<ptr seg=%u gen=%u off=%u>", PTR_SEG(v.p), PTR_GEN(v.p), PTR_OFF(v.p));
        break;
    case T_R: {
        Obj *o = v.r;
        if (!o) return str_new(p, "null", 4);
        switch (o->kind) {
        case OBJ_STRING: return (ObjString *)o;
        case OBJ_BYTES: return str_new(p, (const char *)((ObjBytes *)o)->data, ((ObjBytes *)o)->len);
        case OBJ_ARRAY:
            snprintf(buf, sizeof buf, "<array %s[%u]>", type_name((MlwsType)((ObjArray *)o)->elem), ((ObjArray *)o)->len);
            break;
        case OBJ_STRUCT: snprintf(buf, sizeof buf, "<%s>", ((ObjStruct *)o)->def->name); break;
        case OBJ_LIST: snprintf(buf, sizeof buf, "<list len=%u>", ((ObjList *)o)->len); break;
        case OBJ_MAP: snprintf(buf, sizeof buf, "<map len=%u>", ((ObjMap *)o)->count); break;
        case OBJ_CLOSURE: {
            const Function *f = ((ObjClosure *)o)->fn;
            snprintf(buf, sizeof buf, "<closure %.40s.%.40s>", f->mod->m->name, f->name);
            break;
        }
        case OBJ_BIG: case OBJ_DEC: {
            StrBuf nb = {0};
            num_format(&nb, o);
            ObjString *s = str_new(p, nb.s, nb.n);
            sb_free(&nb);
            return s;
        }
        case OBJ_FOREIGN: {
            const ObjForeign *fo = (const ObjForeign *)o;
            snprintf(buf, sizeof buf, "<foreign %.60s%s>", ((const MlwsForeignType *)fo->type)->name, fo->ptr ? "" : " (released)");
            break;
        }
        default: snprintf(buf, sizeof buf, "<object>"); break;
        }
        break;
    }
    default: snprintf(buf, sizeof buf, "%lld", (long long)v.i); break;
    }
    return str_new(p, buf, strlen(buf));
}

/* ---- closures ------------------------------------------------------------------ */

ObjClosure *closure_new(Process *p, struct Function *fn, uint32_t nup) {
    size_t size = sizeof(ObjClosure) + nup * (sizeof(Word) + 1);
    ObjClosure *c = (ObjClosure *)gc_alloc(p, OBJ_CLOSURE, size);
    c->fn = fn;
    c->nup = nup;
    c->tags = (uint8_t *)(c->up + nup);
    return c;
}
