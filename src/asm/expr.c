/* expr.c - precedence-climbing evaluator for assembler constant expressions.
 *
 * Precedence (lowest first): ||  &&  |  ^  &  == !=  < <= > >=  << >>  + -
 * * / %  then unary - + ~ !. Integers are 64-bit two's complement (wrapping);
 * an expression becomes float if any operand is float. */
#include "expr.h"

#include <math.h>

#include "../core/types.h"

static const Token *peek(ExprParser *ep) { return &ep->t[ep->pos < ep->n ? ep->pos : ep->n - 1]; }

static bool fail(ExprParser *ep, const Token *t, const char *msg) {
    snprintf(ep->err, sizeof ep->err, "%s", msg);
    ep->errcol = t->col;
    return false;
}

static double as_f(ExprVal v) { return v.is_float ? v.f : (double)v.i; }

static ExprVal mk_i(int64_t i) {
    ExprVal v = {false, i, 0};
    return v;
}

static ExprVal mk_f(double f) {
    ExprVal v = {true, 0, f};
    return v;
}

bool expr_can_start(const Token *t) {
    if (t->kind == TK_INT || t->kind == TK_FLOAT || t->kind == TK_CHAR || t->kind == TK_IDENT) return true;
    return tok_is(t, "(") || tok_is(t, "-") || tok_is(t, "+") || tok_is(t, "~") || tok_is(t, "!");
}

static bool parse_binary(ExprParser *ep, int minprec, ExprVal *out);

static bool parse_primary(ExprParser *ep, ExprVal *out) {
    const Token *t = peek(ep);
    switch (t->kind) {
    case TK_INT:
    case TK_CHAR:
        ep->pos++;
        *out = mk_i(t->ival);
        return true;
    case TK_FLOAT:
        ep->pos++;
        *out = mk_f(t->fval);
        return true;
    case TK_PUNCT:
        if (tok_is(t, "(")) {
            ep->pos++;
            if (!parse_binary(ep, 0, out)) return false;
            if (!tok_is(peek(ep), ")")) return fail(ep, peek(ep), "expected ')'");
            ep->pos++;
            return true;
        }
        if (tok_is(t, "-") || tok_is(t, "+") || tok_is(t, "~") || tok_is(t, "!")) {
            ep->pos++;
            ExprVal v;
            if (!parse_primary(ep, &v)) return false;
            switch (t->p[0]) {
            case '-': *out = v.is_float ? mk_f(-v.f) : mk_i((int64_t)(0 - (uint64_t)v.i)); break;
            case '+': *out = v; break;
            case '~':
                if (v.is_float) return fail(ep, t, "'~' needs an integer");
                *out = mk_i(~v.i);
                break;
            default: *out = mk_i(v.is_float ? v.f == 0 : v.i == 0); break;
            }
            return true;
        }
        return fail(ep, t, "expected an expression");
    case TK_IDENT: {
        ep->pos++;
        if (tok_ident(t, "true")) return *out = mk_i(1), true;
        if (tok_ident(t, "false")) return *out = mk_i(0), true;
        if (tok_ident(t, "inf")) return *out = mk_f(INFINITY), true;
        if (tok_ident(t, "nan")) return *out = mk_f(NAN), true;
        if (tok_ident(t, "defined") || tok_ident(t, "sizeof")) {
            bool is_def = tok_ident(t, "defined");
            if (!tok_is(peek(ep), "(")) return fail(ep, peek(ep), "expected '('");
            ep->pos++;
            const Token *a = peek(ep);
            if (a->kind != TK_IDENT) return fail(ep, a, "expected a name");
            ep->pos++;
            if (!tok_is(peek(ep), ")")) return fail(ep, peek(ep), "expected ')'");
            ep->pos++;
            if (is_def) {
                *out = mk_i(ep->defined && ep->defined(ep->ctx, a->start, (size_t)a->len));
                return true;
            }
            MlwsType ty;
            if (!type_from_name(a->start, (size_t)a->len, &ty)) return fail(ep, a, "sizeof() needs a type name");
            *out = mk_i(type_size(ty));
            return true;
        }
        if (ep->lookup && ep->lookup(ep->ctx, t->start, (size_t)t->len, out)) return true;
        snprintf(ep->err, sizeof ep->err, "undefined constant '%.*s'", t->len, t->start);
        ep->errcol = t->col;
        return false;
    }
    default:
        return fail(ep, t, "expected an expression");
    }
}

static int binprec(const Token *t) {
    if (t->kind != TK_PUNCT) return -1;
    static const struct {
        const char *op;
        int prec;
    } tab[] = {
        {"||", 1}, {"&&", 2}, {"|", 3}, {"^", 4}, {"&", 5}, {"==", 6}, {"!=", 6},
        {"<", 7}, {"<=", 7}, {">", 7}, {">=", 7}, {"<<", 8}, {">>", 8},
        {"+", 9}, {"-", 9}, {"*", 10}, {"/", 10}, {"%", 10},
    };
    for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++)
        if (strcmp(t->p, tab[i].op) == 0) return tab[i].prec;
    return -1;
}

static bool apply(ExprParser *ep, const Token *op, ExprVal a, ExprVal b, ExprVal *out) {
    const char *o = op->p;
    bool fl = a.is_float || b.is_float;
    if (!strcmp(o, "||")) return *out = mk_i(as_f(a) != 0 || as_f(b) != 0), true;
    if (!strcmp(o, "&&")) return *out = mk_i(as_f(a) != 0 && as_f(b) != 0), true;
    if (!strcmp(o, "==")) return *out = mk_i(fl ? as_f(a) == as_f(b) : a.i == b.i), true;
    if (!strcmp(o, "!=")) return *out = mk_i(fl ? as_f(a) != as_f(b) : a.i != b.i), true;
    if (!strcmp(o, "<")) return *out = mk_i(fl ? as_f(a) < as_f(b) : a.i < b.i), true;
    if (!strcmp(o, "<=")) return *out = mk_i(fl ? as_f(a) <= as_f(b) : a.i <= b.i), true;
    if (!strcmp(o, ">")) return *out = mk_i(fl ? as_f(a) > as_f(b) : a.i > b.i), true;
    if (!strcmp(o, ">=")) return *out = mk_i(fl ? as_f(a) >= as_f(b) : a.i >= b.i), true;
    if (fl) {
        double x = as_f(a), y = as_f(b);
        switch (o[0]) {
        case '+': *out = mk_f(x + y); return true;
        case '-': *out = mk_f(x - y); return true;
        case '*': *out = mk_f(x * y); return true;
        case '/': *out = mk_f(x / y); return true;
        case '%': *out = mk_f(fmod(x, y)); return true;
        default: return fail(ep, op, "operator needs integer operands");
        }
    }
    uint64_t x = (uint64_t)a.i, y = (uint64_t)b.i;
    if (!strcmp(o, "<<")) return *out = mk_i((int64_t)(x << (y & 63))), true;
    if (!strcmp(o, ">>")) return *out = mk_i(a.i >> (y & 63)), true;
    switch (o[0]) {
    case '+': *out = mk_i((int64_t)(x + y)); return true;
    case '-': *out = mk_i((int64_t)(x - y)); return true;
    case '*': *out = mk_i((int64_t)(x * y)); return true;
    case '/':
    case '%':
        if (b.i == 0) return fail(ep, op, "division by zero in constant expression");
        if (a.i == INT64_MIN && b.i == -1) return *out = mk_i(o[0] == '/' ? INT64_MIN : 0), true;
        *out = mk_i(o[0] == '/' ? a.i / b.i : a.i % b.i);
        return true;
    case '&': *out = mk_i(a.i & b.i); return true;
    case '|': *out = mk_i(a.i | b.i); return true;
    case '^': *out = mk_i(a.i ^ b.i); return true;
    }
    return fail(ep, op, "unknown operator");
}

static bool parse_binary(ExprParser *ep, int minprec, ExprVal *out) {
    ExprVal lhs;
    if (!parse_primary(ep, &lhs)) return false;
    for (;;) {
        const Token *op = peek(ep);
        int prec = binprec(op);
        if (prec < 0 || prec < minprec) break;
        ep->pos++;
        ExprVal rhs;
        if (!parse_binary(ep, prec + 1, &rhs)) return false;
        if (!apply(ep, op, lhs, rhs, &lhs)) return false;
    }
    *out = lhs;
    return true;
}

bool expr_eval(ExprParser *ep, ExprVal *out) { return parse_binary(ep, 1, out); }

bool expr_eval_term(ExprParser *ep, ExprVal *out) { return parse_primary(ep, out); }
