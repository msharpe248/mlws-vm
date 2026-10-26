/* lexer.c - see lexer.h */
#include "lexer.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>

static bool ident_start(int c) { return isalpha(c) || c == '_' || c == '.' || c == '$'; }
static bool ident_char(int c) { return isalnum(c) || c == '_' || c == '.' || c == '$'; }

size_t comment_start(const char *text) {
    char q = 0;
    for (size_t i = 0; text[i]; i++) {
        char c = text[i];
        if (q) {
            if (c == '\\' && text[i + 1]) i++;
            else if (c == q) q = 0;
        } else if (c == '"' || c == '\'') {
            q = c;
        } else if (c == ';') {
            return i;
        }
    }
    return strlen(text);
}

int utf8_encode(uint32_t cp, char out[4]) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Decode one UTF-8 sequence; returns bytes consumed (0 on error). */
static int utf8_decode(const unsigned char *s, uint32_t *cp) {
    if (s[0] < 0x80) {
        *cp = s[0];
        return 1;
    }
    int n = (s[0] & 0xE0) == 0xC0 ? 2 : (s[0] & 0xF0) == 0xE0 ? 3 : (s[0] & 0xF8) == 0xF0 ? 4 : 0;
    if (!n) return 0;
    uint32_t v = s[0] & (0x7F >> n);
    for (int i = 1; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80) return 0;
        v = (v << 6) | (s[i] & 0x3F);
    }
    *cp = v;
    return n;
}

/* Parse an escape after the backslash at *p. Appends bytes to sb (strings) and
 * returns the code point (chars). */
static bool lex_escape(const char **pp, StrBuf *sb, uint32_t *cp_out, char *err, size_t errlen) {
    const char *p = *pp;
    uint32_t cp;
    switch (*p) {
    case 'n': cp = '\n'; p++; break;
    case 't': cp = '\t'; p++; break;
    case 'r': cp = '\r'; p++; break;
    case '0': cp = 0; p++; break;
    case '\\': cp = '\\'; p++; break;
    case '"': cp = '"'; p++; break;
    case '\'': cp = '\''; p++; break;
    case 'x': {
        p++;
        if (!isxdigit((unsigned char)p[0]) || !isxdigit((unsigned char)p[1])) {
            snprintf(err, errlen, "\\x escape needs two hex digits");
            return false;
        }
        char h[3] = {p[0], p[1], 0};
        cp = (uint32_t)strtoul(h, NULL, 16);
        p += 2;
        if (sb) { /* raw byte */
            sb_putc(sb, (char)cp);
            *pp = p;
            if (cp_out) *cp_out = cp;
            return true;
        }
        break;
    }
    case 'u': {
        p++;
        if (*p != '{') {
            snprintf(err, errlen, "\\u escape must look like \\u{1F600}");
            return false;
        }
        p++;
        char *end;
        cp = (uint32_t)strtoul(p, &end, 16);
        if (end == p || *end != '}' || cp > 0x10FFFF) {
            snprintf(err, errlen, "bad \\u{...} escape");
            return false;
        }
        p = end + 1;
        break;
    }
    default:
        snprintf(err, errlen, "unknown escape \\%c", *p ? *p : ' ');
        return false;
    }
    if (sb) {
        char buf[4];
        sb_putn(sb, buf, (size_t)utf8_encode(cp, buf));
    }
    if (cp_out) *cp_out = cp;
    *pp = p;
    return true;
}

static const char *const PUNCTS[] = {
    "->", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||",
    ",", ":", "[", "]", "{", "}", "(", ")", "+", "-", "*", "/", "%",
    "&", "|", "^", "~", "!", "<", ">", "@", "=", NULL,
};

bool lex_line(const char *text, TokVec *out, char *err, size_t errlen, int *errcol) {
    size_t end = comment_start(text);
    const char *p = text, *lim = text + end;
    while (p < lim) {
        unsigned char c = (unsigned char)*p;
        if (isspace(c)) {
            p++;
            continue;
        }
        Token t;
        memset(&t, 0, sizeof t);
        t.start = p;
        t.col = (int)(p - text) + 1;
        *errcol = t.col;
        if (isdigit(c)) {
            /* numeric local label reference: digits followed by b/f */
            const char *q = p;
            while (q < lim && isdigit((unsigned char)*q)) q++;
            if (q < lim && (*q == 'b' || *q == 'f') && (q + 1 >= lim || !ident_char((unsigned char)q[1])) &&
                !(q - p == 1 && *p == '0' && *q == 'b' && q + 1 < lim && (q[1] == '0' || q[1] == '1'))) {
                t.kind = TK_LOCALREF;
                t.ival = strtoll(p, NULL, 10);
                t.p[0] = *q;
                p = q + 1;
                t.len = (int)(p - t.start);
                VEC_PUSH(*out, t);
                continue;
            }
            char buf[128];
            size_t bn = 0;
            int base = 10;
            bool is_float = false;
            if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) base = 16, p += 2;
            else if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B')) base = 2, p += 2;
            else if (p[0] == '0' && (p[1] == 'o' || p[1] == 'O')) base = 8, p += 2;
            while (p < lim && bn < sizeof buf - 1) {
                char d = *p;
                if (d == '_') {
                    p++;
                    continue;
                }
                if (base == 16 ? isxdigit((unsigned char)d) : isdigit((unsigned char)d)) {
                    buf[bn++] = d;
                    p++;
                } else if (base == 10 && d == '.' && p + 1 < lim && isdigit((unsigned char)p[1])) {
                    is_float = true;
                    buf[bn++] = d;
                    p++;
                } else if (base == 10 && (d == 'e' || d == 'E') &&
                           (isdigit((unsigned char)p[1]) ||
                            ((p[1] == '-' || p[1] == '+') && isdigit((unsigned char)p[2])))) {
                    is_float = true;
                    buf[bn++] = d;
                    p++;
                    if (*p == '-' || *p == '+') buf[bn++] = *p++;
                } else {
                    break;
                }
            }
            buf[bn] = 0;
            if (p < lim && ident_char((unsigned char)*p)) {
                snprintf(err, errlen, "malformed number");
                return false;
            }
            if (bn == 0) {
                snprintf(err, errlen, "malformed number");
                return false;
            }
            if (is_float) {
                t.kind = TK_FLOAT;
                t.fval = strtod(buf, NULL);
            } else {
                t.kind = TK_INT;
                errno = 0;
                unsigned long long v = strtoull(buf, NULL, base);
                if (errno == ERANGE) {
                    snprintf(err, errlen, "integer literal out of range");
                    return false;
                }
                for (size_t i = 0; i < bn; i++) {
                    if (base == 2 && buf[i] > '1') {
                        snprintf(err, errlen, "bad binary digit");
                        return false;
                    }
                    if (base == 8 && buf[i] > '7') {
                        snprintf(err, errlen, "bad octal digit");
                        return false;
                    }
                }
                t.ival = (int64_t)v;
            }
        } else if (ident_start(c)) {
            while (p < lim && ident_char((unsigned char)*p)) p++;
            t.kind = TK_IDENT;
        } else if (c == '"') {
            StrBuf sb = {0};
            p++;
            while (p < lim && *p != '"') {
                if (*p == '\\') {
                    p++;
                    if (!lex_escape(&p, &sb, NULL, err, errlen)) {
                        sb_free(&sb);
                        return false;
                    }
                } else {
                    sb_putc(&sb, *p++);
                }
            }
            if (p >= lim) {
                sb_free(&sb);
                snprintf(err, errlen, "unterminated string");
                return false;
            }
            p++;
            t.kind = TK_STRING;
            t.slen = sb.n;
            t.sval = sb.s ? sb.s : xcalloc(1, 1);
        } else if (c == '\'') {
            p++;
            uint32_t cp = 0;
            if (*p == '\\') {
                p++;
                if (!lex_escape(&p, NULL, &cp, err, errlen)) return false;
            } else {
                int n = utf8_decode((const unsigned char *)p, &cp);
                if (n == 0 || *p == '\'') {
                    snprintf(err, errlen, "bad character literal");
                    return false;
                }
                p += n;
            }
            if (p >= lim || *p != '\'') {
                snprintf(err, errlen, "unterminated character literal");
                return false;
            }
            p++;
            t.kind = TK_CHAR;
            t.ival = cp;
        } else {
            int i;
            for (i = 0; PUNCTS[i]; i++) {
                size_t n = strlen(PUNCTS[i]);
                if ((size_t)(lim - p) >= n && memcmp(p, PUNCTS[i], n) == 0) {
                    memcpy(t.p, PUNCTS[i], n);
                    p += n;
                    break;
                }
            }
            if (!PUNCTS[i]) {
                snprintf(err, errlen, "unexpected character '%c'", c);
                return false;
            }
            t.kind = TK_PUNCT;
        }
        t.len = (int)(p - t.start);
        VEC_PUSH(*out, t);
    }
    Token eof;
    memset(&eof, 0, sizeof eof);
    eof.kind = TK_EOF;
    eof.start = lim;
    eof.col = (int)(lim - text) + 1;
    VEC_PUSH(*out, eof);
    return true;
}

void tokvec_free(TokVec *v) {
    for (size_t i = 0; i < v->n; i++) free(v->v[i].sval);
    VEC_FREE(*v);
}

bool tok_is(const Token *t, const char *punct) { return t->kind == TK_PUNCT && strcmp(t->p, punct) == 0; }

bool tok_ident(const Token *t, const char *name) {
    return t->kind == TK_IDENT && (size_t)t->len == strlen(name) && memcmp(t->start, name, (size_t)t->len) == 0;
}
