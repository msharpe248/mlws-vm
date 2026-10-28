/* natives.c - built-in native (host) functions callable with `ncall`.
 * docs/NATIVES.md is generated from this table by `mlwsvm doc natives`. */
#include <inttypes.h>
#include <math.h>

#include "../platform/platform.h"
#include "../heap/heap.h"
#include "../vm/vm.h"

static void out_str(Process *p, const char *s) { proc_write(p, s, strlen(s)); }

static void n_print_i(Process *p, Word *a, Word *r) {
    (void)r;
    char buf[32];
    snprintf(buf, sizeof buf, "%" PRId64, a[0].i);
    out_str(p, buf);
}

static void n_print_u(Process *p, Word *a, Word *r) {
    (void)r;
    char buf[32];
    snprintf(buf, sizeof buf, "%" PRIu64, a[0].u);
    out_str(p, buf);
}

static void n_print_hex(Process *p, Word *a, Word *r) {
    (void)r;
    char buf[32];
    snprintf(buf, sizeof buf, "0x%" PRIx64, a[0].u);
    out_str(p, buf);
}

static void n_print_f(Process *p, Word *a, Word *r) {
    (void)r;
    char buf[64];
    fmt_double(a[0].f, buf, sizeof buf);
    out_str(p, buf);
}

/* A string or byte buffer argument's bytes; traps on anything else. */
static const char *text_arg(Process *p, Word a, const char *fn, uint32_t *len) {
    Obj *o = a.r;
    if (!o) vm_trap(p, "E_NULL", "%s of null", fn);
    if (o->kind == OBJ_STRING) {
        *len = ((ObjString *)o)->len;
        return ((ObjString *)o)->data;
    }
    if (o->kind == OBJ_BYTES) {
        *len = ((ObjBytes *)o)->len;
        return (const char *)((ObjBytes *)o)->data;
    }
    vm_trap(p, "E_TYPE", "%s needs a string or byte buffer, got %s", fn, obj_kind_name(o->kind));
}

static void n_print_s(Process *p, Word *a, Word *r) {
    (void)r;
    if (!a[0].r) {
        out_str(p, "null");
        return;
    }
    uint32_t n;
    const char *s = text_arg(p, a[0], "io.print_s", &n);
    proc_write(p, s, n);
}

static void n_print_c(Process *p, Word *a, Word *r) {
    (void)r;
    uint32_t cp = (uint32_t)a[0].u;
    char buf[4];
    int n;
    if (cp < 0x80) buf[0] = (char)cp, n = 1;
    else if (cp < 0x800) buf[0] = (char)(0xC0 | (cp >> 6)), buf[1] = (char)(0x80 | (cp & 0x3F)), n = 2;
    else if (cp < 0x10000)
        buf[0] = (char)(0xE0 | (cp >> 12)), buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F)), buf[2] = (char)(0x80 | (cp & 0x3F)),
        n = 3;
    else
        buf[0] = (char)(0xF0 | (cp >> 18)), buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F)),
        buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F)), buf[3] = (char)(0x80 | (cp & 0x3F)), n = 4;
    proc_write(p, buf, (size_t)n);
}

static void n_print_b(Process *p, Word *a, Word *r) {
    (void)r;
    out_str(p, a[0].i ? "true" : "false");
}

static void n_print_p(Process *p, Word *a, Word *r) {
    (void)r;
    char buf[64];
    uint64_t v = a[0].p;
    if (!v) snprintf(buf, sizeof buf, "null");
    else snprintf(buf, sizeof buf, "<ptr seg=%u gen=%u off=%u>", PTR_SEG(v), PTR_GEN(v), PTR_OFF(v));
    out_str(p, buf);
}

static void n_nl(Process *p, Word *a, Word *r) {
    (void)a;
    (void)r;
    out_str(p, "\n");
}

static void n_flush(Process *p, Word *a, Word *r) {
    (void)a;
    (void)r;
    proc_write_line_out(p);
    vm_flush(p->vm);
}

static void n_now_ns(Process *p, Word *a, Word *r) {
    (void)p;
    (void)a;
    r[0].i = (int64_t)plat_now_ns();
}

static void n_str_len(Process *p, Word *a, Word *r) {
    uint32_t n;
    text_arg(p, a[0], "str.len", &n);
    r[0].i = n;
}

static void n_str_byte(Process *p, Word *a, Word *r) {
    uint32_t n;
    const char *s = text_arg(p, a[0], "str.byte", &n);
    if (a[1].i < 0 || a[1].i >= (int64_t)n)
        vm_trap(p, "E_BOUNDS", "str.byte index %lld out of range (length %u)", (long long)a[1].i, n);
    r[0].i = (uint8_t)s[a[1].i];
}

static void n_gc_collect(Process *p, Word *a, Word *r) {
    (void)a;
    (void)r;
    p->gc_pending = true; /* runs at the next instruction boundary */
}

static void n_gc_heap_bytes(Process *p, Word *a, Word *r) {
    (void)a;
    r[0].i = (int64_t)p->heap_bytes;
}

static void n_proc_count(Process *p, Word *a, Word *r) {
    (void)a;
    r[0].i = (int64_t)p->vm->live;
}

static void n_gc_collections(Process *p, Word *a, Word *r) {
    (void)a;
    r[0].i = (int64_t)p->gc_count;
}

static void n_foreign_release(Process *p, Word *a, Word *r) {
    (void)r;
    Obj *o = a[0].r;
    if (!o || o->kind != OBJ_FOREIGN) vm_trap(p, "E_TYPE", "foreign.release needs a foreign handle");
    foreign_finalize(o); /* idempotent: the handle forgets its pointer */
}

static void n_test_fail(Process *p, Word *a, Word *r) {
    (void)r;
    ObjString *s = a[0].r;
    vm_trap(p, "E_ASSERT", "%s", s ? s->data : "test failure");
}

#define MATH1(name, fn)                                                        \
    static void n_##name(Process *p, Word *a, Word *r) {                       \
        (void)p;                                                               \
        r[0].f = fn(a[0].f);                                                   \
    }
#define MATH2(name, fn)                                                        \
    static void n_##name(Process *p, Word *a, Word *r) {                       \
        (void)p;                                                               \
        r[0].f = fn(a[0].f, a[1].f);                                           \
    }
MATH1(sin, sin)
MATH1(cos, cos)
MATH1(tan, tan)
MATH1(asin, asin)
MATH1(acos, acos)
MATH1(atan, atan)
MATH1(exp, exp)
MATH1(log, log)
MATH1(log2, log2)
MATH1(log10, log10)
MATH2(atan2, atan2)
MATH2(pow, pow)
MATH2(hypot, hypot)

#define F1 1, {T_F64}, 1, {T_F64}
#define F2 2, {T_F64, T_F64}, 1, {T_F64}

/* Built-in natives leave the foreign-native fields (flags, data, lib) zero. */
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
static const NativeDef BUILTINS[] = {
    {"io.print_i", 1, {T_I64}, 0, {0}, n_print_i, "Print a signed integer."},
    {"io.print_u", 1, {T_U64}, 0, {0}, n_print_u, "Print an unsigned integer."},
    {"io.print_hex", 1, {T_U64}, 0, {0}, n_print_hex, "Print an integer in hexadecimal with a 0x prefix."},
    {"io.print_f", 1, {T_F64}, 0, {0}, n_print_f, "Print a float using the shortest representation that round-trips."},
    {"io.print_s", 1, {T_R}, 0, {0}, n_print_s, "Print a string or byte buffer (null prints `null`)."},
    {"io.print_c", 1, {T_C}, 0, {0}, n_print_c, "Print a character (UTF-8 encoded)."},
    {"io.print_b", 1, {T_B}, 0, {0}, n_print_b, "Print `true` or `false`."},
    {"io.print_p", 1, {T_P}, 0, {0}, n_print_p, "Print a pointer for debugging."},
    {"io.nl", 0, {0}, 0, {0}, n_nl, "Print a newline."},
    {"io.flush", 0, {0}, 0, {0}, n_flush, "Write out the program's buffered standard output now, with the calling process's unfinished line (output otherwise goes out a whole line at a time). Output is also flushed when the VM is idle, at exit, and before anything is written to standard error."},
    {"time.now_ns", 0, {0}, 1, {T_I64}, n_now_ns, "Monotonic clock in nanoseconds (the same as time.monotonic)."},
    {"str.len", 1, {T_R}, 1, {T_I64}, n_str_len, "Length of a string or byte buffer in bytes (same as the len instruction)."},
    {"str.byte", 2, {T_R, T_I64}, 1, {T_U8}, n_str_byte, "Byte at an index of a string (bounds checked)."},
    {"test.fail", 1, {T_R}, 0, {0}, n_test_fail, "Trap with E_ASSERT and the given message."},
    {"foreign.release", 1, {T_R}, 0, {0}, n_foreign_release, "Release a foreign handle now: run its finalizer (an extension's, or the tag's .ffi_free function) once; later uses see a released handle."},
    {"ffi.async_callback", 2, {T_R, T_I64}, 1, {T_R}, n_ffi_async_callback, "A C function pointer for an async .callback type (by name) whose calls, from any thread, become Callback{name, args} messages to a process. Keep the handle while C may call it."},
    {"gc.collect", 0, {0}, 0, {0}, n_gc_collect, "Request a garbage collection; it runs before the next instruction."},
    {"gc.heap_bytes", 0, {0}, 1, {T_I64}, n_gc_heap_bytes, "Bytes currently allocated on this process's heap (live and not yet collected)."},
    {"gc.collections", 0, {0}, 1, {T_I64}, n_gc_collections, "Number of garbage collections so far in this process."},
    {"proc.count", 0, {0}, 1, {T_I64}, n_proc_count, "Number of processes currently alive in the VM (including this one)."},
    {"math.sin", F1, n_sin, "Sine (radians)."},
    {"math.cos", F1, n_cos, "Cosine (radians)."},
    {"math.tan", F1, n_tan, "Tangent (radians)."},
    {"math.asin", F1, n_asin, "Arc sine."},
    {"math.acos", F1, n_acos, "Arc cosine."},
    {"math.atan", F1, n_atan, "Arc tangent."},
    {"math.atan2", F2, n_atan2, "atan2(y, x)."},
    {"math.exp", F1, n_exp, "e raised to x."},
    {"math.log", F1, n_log, "Natural logarithm."},
    {"math.log2", F1, n_log2, "Base-2 logarithm."},
    {"math.log10", F1, n_log10, "Base-10 logarithm."},
    {"math.pow", F2, n_pow, "x raised to y."},
    {"math.hypot", F2, n_hypot, "sqrt(x*x + y*y) without undue overflow."},
};

const NativeDef *natives_builtin_table(size_t *n) {
    *n = sizeof BUILTINS / sizeof BUILTINS[0];
    return BUILTINS;
}

void natives_register_builtin(VM *vm) {
    for (size_t i = 0; i < sizeof BUILTINS / sizeof BUILTINS[0]; i++) vm_register_native(vm, &BUILTINS[i]);
    natives_register_os(vm);
    natives_register_text(vm);
    natives_register_util(vm);
    natives_register_proc(vm);
    natives_register_modules(vm);
}
