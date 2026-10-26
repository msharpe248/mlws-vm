/* disasm.h - module disassembler. */
#ifndef MLWSVM_DISASM_H
#define MLWSVM_DISASM_H

#include "insn.h"
#include "module.h"

bool disasm_module(StrBuf *out, const Module *m, MlwsError *err);
void disasm_insn(StrBuf *out, const Module *m, const Insn *in);

#endif
