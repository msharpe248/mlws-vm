/* lexer.h - line-oriented tokenizer for MLWSVM assembly. */
#ifndef MLWSVM_LEXER_H
#define MLWSVM_LEXER_H

#include "../core/common.h"

typedef enum {
    TK_EOF,
    TK_IDENT,    /* [A-Za-z_.$][A-Za-z0-9_.$]* */
    TK_INT,
    TK_FLOAT,
    TK_STRING,
    TK_CHAR,
    TK_PUNCT,
    TK_LOCALREF, /* numeric local label reference: 1b / 1f */
    TK_BIGLIT,   /* 123n: sval holds the decimal digits */
    TK_DECLIT,   /* 1.25d: sval holds the literal text without the suffix */
} TokKind;

typedef struct {
    TokKind kind;
    int col;           /* 1-based column in the line */
    const char *start; /* points into the line text */
    int len;
    int64_t ival;      /* TK_INT, TK_CHAR, TK_LOCALREF (label number) */
    double fval;       /* TK_FLOAT */
    char *sval;        /* TK_STRING (owned, may contain NULs) */
    size_t slen;
    char p[4];         /* TK_PUNCT text; TK_LOCALREF direction "b"/"f" */
} Token;

typedef VEC(Token) TokVec;

/* Tokenize one line (comments stripped). Always appends a TK_EOF token.
 * On error returns false with a message and the 1-based column. */
bool lex_line(const char *text, TokVec *out, char *errmsg, size_t errlen, int *errcol);
void tokvec_free(TokVec *v);

bool tok_is(const Token *t, const char *punct);
bool tok_ident(const Token *t, const char *name);

/* Offset of the start of a ';' comment outside string/char literals, or strlen. */
size_t comment_start(const char *text);
/* Decode one UTF-8 escape sequence body helper: encodes cp into out (<=4 bytes). */
int utf8_encode(uint32_t cp, char out[4]);

#endif
