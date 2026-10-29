/* opt_internal.h - the optimizer's working form of a function, and what the
 * passes share. */
#ifndef MLWSVM_OPT_INTERNAL_H
#define MLWSVM_OPT_INTERNAL_H

#include "../core/check.h"
#include "../core/regflow.h"
#include "opt.h"

/* One instruction. `rb`/`rn` are the `.loc` runs (MFunc.locs) that start
 * just before it, as indices into OFunc.runs: when the instruction goes, they
 * move to the next one, so every run keeps its place (and the last of them is
 * the one in force there). */
typedef struct {
    Insn in;
    uint32_t line; /* MFunc.lines */
    uint32_t rb, rn;
    bool dead;     /* removed at the next ofn_compact */
} OInsn;

typedef struct OModule OModule;

typedef struct {
    OModule *om;
    uint32_t fi;      /* index in Module.funcs */
    MFunc *mf;
    VEC(OInsn) code;
    MLoc *runs;       /* the function's .loc runs (their starts are not used) */
    uint32_t nruns;
    uint32_t tail_rb, tail_rn; /* runs that start at the end */
    VEC(MHandler) h;
    uint32_t before;  /* instructions when loaded */
    uint32_t regs_before;
    bool removed;     /* deadfuncs */
} OFunc;

struct OModule {
    Module *m;
    const OptConfig *cfg;
    OptReport *rep;
    OptModStat *stat;
    OFunc **fn;       /* per Module.funcs entry; NULL for imports */
};

static inline bool opt_on(const OFunc *f, int pass) { return (f->om->cfg->passes >> pass) & 1u; }

/* ---- ir.c ---- */
OFunc *ofn_load(OModule *om, uint32_t fi);
void ofn_free(OFunc *f);
void ofn_store(OModule *om);                 /* re-encode every function into the module */
void ofn_compact(OFunc *f);                  /* drop dead instructions, remapping everything */
void ofn_insert(OFunc *f, uint32_t at, OInsn *ins, uint32_t n); /* before `at`; labels to `at` reach ins[0] */
void insn_copy(Insn *dst, const Insn *src);
Insn insn_make(int op, MlwsType t, int nops);
Operand op_reg(uint8_t reg);
Operand op_imm(int64_t v);
Operand op_fimm(double v);
Operand op_label(uint32_t target);
/* Count one change by `pass` at instruction pc, and log it ("what", then the
 * instruction as it was, if given). */
void ochange(OFunc *f, int pass, uint32_t pc, const Insn *was, const char *what);

/* Registers an instruction reads, writes on completion (`def`), and may write
 * (`may`: also a recvt that times out, and the base of [P1]+ / -[P1]). */
typedef struct {
    RegSet use, def, may;
} Effect;
void oeffect(const Insn *in, Effect *e);
Effect *ofn_effects(const OFunc *f); /* each instruction's, computed once (free it) */
static inline bool rs_has(const RegSet *s, uint8_t reg) {
    return REG_BANK(reg) < NBANKS && (s->b[REG_BANK(reg)] >> REG_IDX(reg) & 1u);
}
bool is_dst_pos(const Insn *in, int k);       /* operand k is written */
bool is_src_pos(const Insn *in, int k);       /* operand k is only read, as a value */
bool insn_pure(const Insn *in);               /* no trap, no memory, no allocation: removable when unused */
bool in_try(const OFunc *f, uint32_t pc);
uint32_t next_live(const OFunc *f, uint32_t pc); /* the first live instruction at or after pc */
int64_t func_index_of(const Operand *o);      /* OM_FUNC index, or -1 */

/* Blocks: leaders are the entry, branch targets, handler targets, and what
 * follows a branch or a terminator. */
typedef struct {
    uint32_t start, end; /* [start, end) */
    uint32_t succ[2], nsucc; /* fall-through and one target; more in `extra` */
    uint32_t *extra, nextra; /* jtab's targets */
} OBlock;
typedef struct {
    OBlock *b;
    uint32_t n;
    uint32_t *of; /* block of each instruction */
} OCfg;
void ocfg_build(const OFunc *f, OCfg *g);
void ocfg_free(OCfg *g);
/* The successors of block i, in order: fills out (up to cap) and returns how many. */
uint32_t ocfg_succs(const OCfg *g, uint32_t i, uint32_t *out, uint32_t cap);

/* ---- the passes ---- */
bool pass_constants(OFunc *f);    /* fold, constprop, simplify (fold.c) */
bool pass_copyprop(OFunc *f);     /* copy.c */
bool pass_peephole(OFunc *f);     /* copy.c */
bool pass_dce(OFunc *f);          /* dce.c */
bool pass_unreachable(OFunc *f);  /* dce.c */
bool pass_branches(OFunc *f);     /* branch.c */
bool pass_tailcall(OFunc *f);     /* tail.c */
bool pass_inline(OFunc *f);       /* inline.c */
bool pass_compact(OFunc *f);      /* regs.c */
void pass_deadfuncs(OModule *om); /* funcs.c */
uint32_t ofn_regs(const OFunc *f);   /* registers in the window */
void ofn_used_regs(const OFunc *f, uint32_t used[NBANKS]); /* registers its live code names (regs.c) */

#endif
