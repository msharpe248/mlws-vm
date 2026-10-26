/* expr.h - constant expression evaluation for the assembler. */
#ifndef MLWSVM_EXPR_H
#define MLWSVM_EXPR_H

#include "lexer.h"

typedef struct {
    bool is_float;
    int64_t i;
    double f;
} ExprVal;

/* Look up a named constant. Return false if `name` is not a constant. */
typedef bool (*ExprLookup)(void *ctx, const char *name, size_t len, ExprVal *out);
/* Return true if a constant or macro named `name` is defined (for defined()). */
typedef bool (*ExprDefined)(void *ctx, const char *name, size_t len);

typedef struct {
    const Token *t;
    size_t n, pos;
    ExprLookup lookup;
    ExprDefined defined;
    void *ctx;
    char err[256];
    int errcol;
} ExprParser;

/* Parse and evaluate one expression starting at ep->pos. Stops at the first
 * token that cannot continue the expression (',', ']', ')', EOF, ...). */
bool expr_eval(ExprParser *ep, ExprVal *out);
/* Parse only a primary/unary term (a literal, name, or parenthesised expression). */
bool expr_eval_term(ExprParser *ep, ExprVal *out);
/* True if the token can begin a constant expression. */
bool expr_can_start(const Token *t);

#endif
