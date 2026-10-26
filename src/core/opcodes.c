/* opcodes.c - opcode table built from opcodes.def. */
#include "opcodes.h"

OpInfo op_info[OP__COUNT] = {
#define OP(e, c, m, cat, pat, ts, fl, d) {c, m, cat, pat, ts, fl, d, {0}, 0, 0, 0, 0},
#include "opcodes.def"
#undef OP
};

static bool op_ready;

void op_init(void) {
    if (op_ready) return;
    for (int i = 0; i < OP__COUNT; i++) {
        OpInfo *oi = &op_info[i];
        if (i > 0 && oi->code <= op_info[i - 1].code) {
            fprintf(stderr, "mlwsvm: opcodes.def codes must be strictly increasing (%s)\n", oi->mnem);
            abort();
        }
        oi->nelems = 0;
        oi->last_mod = 0;
        for (const char *p = oi->pattern; *p; p++) {
            if (*p == '?' || *p == '*' || *p == '+') {
                oi->last_mod = *p;
                if (p[1]) {
                    fprintf(stderr, "mlwsvm: modifier must be last in pattern of %s\n", oi->mnem);
                    abort();
                }
            } else {
                oi->elems[oi->nelems++] = *p;
            }
        }
        if (oi->flags & OPF_CALL) {
            /* target + argc, then up to 30 args/results */
            oi->minops = 2;
            oi->maxops = MAX_OPERANDS;
            continue;
        }
        int fixed = oi->nelems;
        oi->minops = fixed;
        oi->maxops = fixed;
        if (oi->last_mod == '?') {
            oi->minops = fixed - 1;
        } else if (oi->last_mod == '*') {
            oi->minops = fixed - 1;
            oi->maxops = MAX_OPERANDS;
        } else if (oi->last_mod == '+') {
            oi->maxops = MAX_OPERANDS;
        }
    }
    op_ready = true;
}

int op_lookup(const char *mnem, size_t n) {
    for (int i = 0; i < OP__COUNT; i++) {
        if (strlen(op_info[i].mnem) == n && memcmp(op_info[i].mnem, mnem, n) == 0) return i;
    }
    return -1;
}

int op_from_code(uint16_t code) {
    int lo = 0, hi = OP__COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (op_info[mid].code == code) return mid;
        if (op_info[mid].code < code) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

char op_pattern_at(const OpInfo *oi, int k) {
    if (oi->nelems == 0) return 0;
    if (k < oi->nelems) return oi->elems[k];
    return oi->last_mod == '*' || oi->last_mod == '+' ? oi->elems[oi->nelems - 1] : 0;
}
