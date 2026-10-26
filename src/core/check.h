/* check.h - context-free validation of a single instruction: operand count,
 * addressing modes allowed per operand position, register banks vs types and
 * writability. Shared by the assembler (for precise source errors) and the
 * loader's verifier (for binaries from any source). */
#ifndef MLWSVM_CHECK_H
#define MLWSVM_CHECK_H

#include "insn.h"

typedef struct {
    char msg[256];
    int operand; /* 0-based operand index, or -1 for the whole instruction */
} CheckErr;

bool check_insn(const Insn *in, CheckErr *ce);
/* Check one operand used as a value of type t (T_NONE = natural); k is its
 * 0-based position for messages. */
bool check_operand(const Operand *o, int k, MlwsType t, bool write, CheckErr *ce);
/* The type with which operand k is read/written (T_NONE = natural). */
MlwsType operand_type(const Insn *in, int k);

#endif
