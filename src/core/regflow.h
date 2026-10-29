/* regflow.h - which registers an instruction reads and writes, for the
 * verifier's window plan (src/vm/verify.c) and the optimizer (src/opt). */
#ifndef MLWSVM_REGFLOW_H
#define MLWSVM_REGFLOW_H

#include "insn.h"

/* A set of registers: a bit per register, per bank (I, F, R, P). */
typedef struct {
    uint32_t b[NBANKS];
} RegSet;

void rs_add(RegSet *s, uint8_t reg);
/* The registers an operand reads: a source's register; and for any operand,
 * those that give its address (a register destination is only written). */
void operand_reads(const Operand *o, bool dst, RegSet *r);
/* The registers an instruction reads, and those it always writes when it
 * completes. */
void insn_effect(const Insn *in, RegSet *rd, RegSet *wr);

#endif
