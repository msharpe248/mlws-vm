/* preproc.h - assembler preprocessor: .include, .macro, .rept/.irp,
 * .if/.ifdef/.ifb, .const/.set and .error. Produces a flat list of source
 * lines with locations for the assembler proper. */
#ifndef MLWSVM_PREPROC_H
#define MLWSVM_PREPROC_H

#include "expr.h"

typedef struct {
    const char *file; /* interned in PP.files */
    int line;
} SrcLoc;

typedef struct {
    char *text;        /* comment-free source text */
    SrcLoc loc;        /* where the text came from (macro body lines included) */
    SrcLoc origin;     /* the outermost user line it was expanded from */
    const char *macro; /* innermost macro being expanded, or NULL */
} PLine;

typedef struct Macro {
    char *name;
    char **params;
    char **defaults; /* NULL entry = no default */
    int nparams;
    bool varargs;    /* last parameter collects the remaining arguments */
    PLine *body;
    size_t nbody;
    SrcLoc loc;
} Macro;

/* A named constant's values, each with the first output line it applies to:
 * a `.set` name has one per assignment that changed it. */
typedef struct {
    size_t n;
    struct {
        size_t from; /* index into PP.out */
        ExprVal v;
    } *vals;
} ConstDef;

typedef struct PP {
    StrMap macros;   /* name -> Macro* */
    StrMap consts;   /* name -> ConstDef* */
    StrMap readonly; /* names defined with .const (not .set) */
    StrMap included; /* realpath -> NULL */
    VEC(char *) files;
    const char *const *libpaths;
    size_t nlib;
    int uniq;
    int depth;
    VEC(PLine) out;
    /* Constants read as they were at this output line (the assembler sets it
     * for each line); SIZE_MAX, while preprocessing: their latest values. */
    size_t asof;
    MlwsError *err;
} PP;

void pp_init(PP *pp, const char *const *libpaths, size_t nlib, MlwsError *err);
bool pp_run_file(PP *pp, const char *path);
bool pp_run_text(PP *pp, const char *name, const char *text);
void pp_free(PP *pp);

/* Report an error at a line: "file:line:col: message (in macro X, expanded at ...)". */
bool pp_line_error(MlwsError *err, const PLine *ln, int col, const char *code, const char *fmt, ...) MLWS_PRINTF(5, 6);

bool pp_const_lookup(void *pp, const char *name, size_t len, ExprVal *out);
bool pp_defined(void *pp, const char *name, size_t len);

#endif
