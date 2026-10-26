/* opcodes.h - opcode enumeration and metadata generated from opcodes.def. */
#ifndef MLWSVM_OPCODES_H
#define MLWSVM_OPCODES_H

#include "types.h"

/* Opcode flags. */
#define OPF_TERM 0x01    /* never falls through (jmp, ret, halt, tcall) */
#define OPF_NATURAL 0x02 /* untyped form uses operand natural types (mov, swap) */
#define OPF_TYPE2 0x04   /* has a second (source) type byte: cvt.<to>.<from> */
#define OPF_CALL 0x08    /* operands: target, argc, args..., results... */
#define OPF_PERVAL 0x10  /* each operand typed independently when untyped */

/* Dense opcode index (NOT the on-disk code; see OpInfo.code). */
typedef enum {
#define OP(e, code, m, cat, pat, ts, fl, doc) OP_##e,
#include "opcodes.def"
#undef OP
    OP__COUNT
} Opcode;

typedef struct {
    uint16_t code;       /* stable on-disk opcode number */
    const char *mnem;
    const char *category;
    const char *pattern;
    uint32_t types;      /* allowed instruction types (TM bitset); 0 = untyped */
    uint32_t flags;
    const char *doc;
    /* Derived from the pattern by op_init(): */
    char elems[8];       /* pattern letters */
    int nelems;
    char last_mod;       /* 0, '?', '*' or '+' for the last letter */
    int minops, maxops;
} OpInfo;

#define MAX_OPERANDS 32

extern OpInfo op_info[OP__COUNT];

void op_init(void);                    /* idempotent */
int op_lookup(const char *mnem, size_t n); /* -> Opcode or -1 */
int op_from_code(uint16_t code);           /* -> Opcode or -1 */
/* Pattern letter for operand position k (non-call opcodes). */
char op_pattern_at(const OpInfo *oi, int k);

#endif
