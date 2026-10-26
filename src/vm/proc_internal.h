/* proc_internal.h - frame management shared by process.c and interp.c. */
#ifndef MLWSVM_PROC_INTERNAL_H
#define MLWSVM_PROC_INTERNAL_H

#include "vm.h"

void proc_push_frame(Process *p, Function *fn, const Insn *ret_insn);
void proc_pop_frame(Process *p);
void proc_backtrace(Process *p);

#endif
