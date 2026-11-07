/* regex.c - see regex.h. A pattern is parsed to a tree and compiled to a small
 * program; regex_search runs the program as Thompson's and Pike's NFA
 * simulation: one thread per state, in priority order, each with its captures.
 * A state is entered once per position, which is what bounds the work. */
#include <stdarg.h>
#include <stdio.h>

#include "regex.h"
#include "unicode.h"

#define MAX_PROGRAM 65536     /* instructions */
#define MAX_THREAD_WORDS (1u << 22) /* instructions x capture slots: the scratch memory of a search */
#define MAX_REPEAT 1000
#define MAX_DEPTH 500         /* of parentheses */

/* ---- the tree ---- */

enum { N_EMPTY, N_CHAR, N_ANY, N_CLASS, N_ASSERT, N_CAT, N_ALT, N_REPEAT, N_GROUP };
enum { A_BOL, A_EOL, A_BOT, A_EOT, A_WORD, A_NOT_WORD };

typedef struct Node {
    int kind;
    uint32_t cp;      /* N_CHAR */
    int index;        /* N_CLASS; N_ASSERT: A_*; N_GROUP: its number, or -1 */
    int min, max;     /* N_REPEAT; max -1: no limit */
    bool greedy;
    bool icase, dotall, multiline;
    struct Node *a, *b;
} Node;

/* A class: ranges and named sets, all or'd, perhaps negated. */
enum { S_DIGIT, S_WORD, S_SPACE, S_LETTER, S_NUMBER, S_PUNCT, S_UPPER, S_LOWER };

typedef struct {
    uint32_t lo, hi;
    int set;     /* S_*, or -1 for a range */
    bool negate; /* of the set */
} ClassItem;

typedef struct {
    VEC(ClassItem) items;
    bool negate, icase;
} Class;

/* ---- the program ---- */

enum { R_CHAR, R_CHAR_I, R_ANY, R_ANY_NL, R_CLASS, R_SPLIT, R_JMP, R_SAVE, R_ASSERT, R_MATCH };

typedef struct {
    uint8_t op;
    uint32_t x, y; /* R_SPLIT: x is preferred */
} RInsn;

struct Regex {
    VEC(RInsn) code;
    VEC(Class) classes;
    int groups;
    VEC(char *) names; /* names.v[i]: group i + 1's, or NULL */
    bool multiline;
};

/* ---- parsing ---- */

typedef struct {
    Regex *re;
    const unsigned char *s;
    size_t n, i;
    unsigned flags;
    int depth;
    char *err;
    size_t errlen;
    bool failed;
    VEC(Node *) nodes; /* all of them, to free */
} Parser;

static void fail(Parser *p, const char *fmt, ...) MLWS_PRINTF(2, 3);
static void fail(Parser *p, const char *fmt, ...) {
    if (p->failed) return;
    p->failed = true;
    char msg[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    snprintf(p->err, p->errlen, "%s at offset %zu", msg, p->i);
}

static Node *node(Parser *p, int kind) {
    Node *n = xcalloc(1, sizeof *n);
    n->kind = kind;
    n->icase = (p->flags & REGEX_ICASE) != 0;
    n->dotall = (p->flags & REGEX_DOTALL) != 0;
    n->multiline = (p->flags & REGEX_MULTILINE) != 0;
    VEC_PUSH(p->nodes, n);
    return n;
}

static bool more(const Parser *p) { return p->i < p->n; }
static int peek(const Parser *p) { return more(p) ? p->s[p->i] : -1; }
static bool eat(Parser *p, char c) {
    if (peek(p) != (unsigned char)c) return false;
    p->i++;
    return true;
}

static uint32_t next_cp(Parser *p) {
    size_t i = p->i;
    uint32_t cp = uni_utf8_next(p->s, p->n, &i);
    p->i = i;
    return cp;
}

static int hex_digit(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if ((c | 32) >= 'a' && (c | 32) <= 'f') return (c | 32) - 'a' + 10;
    return -1;
}

/* \xHH, \x{H...} or \uHHHH: after the letter. */
static uint32_t parse_hex(Parser *p, int digits) {
    uint32_t cp = 0;
    if (digits == 2 && eat(p, '{')) {
        int k = 0;
        for (; hex_digit(peek(p)) >= 0 && k < 8; k++) cp = cp * 16 + (uint32_t)hex_digit(p->s[p->i++]);
        if (!k || !eat(p, '}') || cp > 0x10FFFF) fail(p, "bad \\x{...}");
        return cp;
    }
    for (int k = 0; k < digits; k++) {
        int d = hex_digit(peek(p));
        if (d < 0) {
            fail(p, "bad hexadecimal escape");
            return 0;
        }
        cp = cp * 16 + (uint32_t)d;
        p->i++;
    }
    return cp;
}

/* \d \w \s and their negations, \p{...}: after the backslash, at the letter. */
static bool parse_set(Parser *p, ClassItem *it) {
    int c = peek(p);
    it->lo = it->hi = 0;
    switch (c) {
    case 'd': case 'D': it->set = S_DIGIT; break;
    case 'w': case 'W': it->set = S_WORD; break;
    case 's': case 'S': it->set = S_SPACE; break;
    case 'p': case 'P': {
        p->i++;
        const char *name;
        size_t len;
        if (eat(p, '{')) {
            name = (const char *)p->s + p->i;
            while (more(p) && peek(p) != '}') p->i++;
            len = (size_t)((const char *)p->s + p->i - name);
            if (!eat(p, '}')) fail(p, "missing } in \\p");
        } else {
            name = (const char *)p->s + p->i;
            len = more(p) ? 1 : 0;
            p->i += len;
        }
        static const struct { const char *name; int set; } SETS[] = {
            {"L", S_LETTER}, {"N", S_NUMBER}, {"P", S_PUNCT}, {"Lu", S_UPPER}, {"Ll", S_LOWER}, {"Nd", S_DIGIT},
        };
        it->set = -1;
        for (size_t k = 0; k < sizeof SETS / sizeof SETS[0]; k++)
            if (strlen(SETS[k].name) == len && !memcmp(SETS[k].name, name, len)) it->set = SETS[k].set;
        if (it->set < 0) fail(p, "unknown class \\p{%.*s}", (int)(len > 20 ? 20 : len), name);
        it->negate = c == 'P';
        return true;
    }
    default: return false;
    }
    it->negate = c >= 'A' && c <= 'Z';
    p->i++;
    return true;
}

/* A character after a backslash, at the letter. */
static uint32_t parse_escape_char(Parser *p) {
    if (!more(p)) {
        fail(p, "a pattern ends in a backslash");
        return 0;
    }
    uint32_t c = next_cp(p);
    switch (c) {
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case 'f': return '\f';
    case 'v': return '\v';
    case 'a': return 7;
    case '0': return 0;
    case 'e': return 27;
    case 'x': return parse_hex(p, 2);
    case 'u': return parse_hex(p, 4);
    default:
        if ((c >= '1' && c <= '9')) fail(p, "backreferences are not supported");
        else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) fail(p, "unknown escape \\%c", (char)c);
        return c;
    }
}

static int add_class(Parser *p, Class c) {
    VEC_PUSH(p->re->classes, c);
    return (int)p->re->classes.n - 1;
}

static Node *parse_class(Parser *p) {
    Class c = {0};
    c.icase = (p->flags & REGEX_ICASE) != 0;
    c.negate = eat(p, '^');
    bool first = true;
    while (more(p) && (first || peek(p) != ']')) {
        first = false;
        ClassItem it = {0, 0, -1, false};
        uint32_t lo;
        if (eat(p, '\\')) {
            if (parse_set(p, &it)) {
                VEC_PUSH(c.items, it);
                continue;
            }
            lo = parse_escape_char(p);
        } else {
            lo = next_cp(p);
        }
        uint32_t hi = lo;
        if (peek(p) == '-' && p->i + 1 < p->n && p->s[p->i + 1] != ']') {
            p->i++;
            if (eat(p, '\\')) hi = parse_escape_char(p);
            else hi = next_cp(p);
            if (hi < lo) fail(p, "a range out of order");
        }
        it.lo = lo;
        it.hi = hi;
        VEC_PUSH(c.items, it);
    }
    if (!eat(p, ']')) fail(p, "missing ]");
    Node *n = node(p, N_CLASS);
    n->index = add_class(p, c);
    return n;
}

static Node *parse_alt(Parser *p);

/* (?flags), (?flags:...), (?:...), (?P<name>...), (?<name>...), (...) */
static Node *parse_group(Parser *p) {
    unsigned saved = p->flags;
    bool capture = true, scoped = true;
    char *name = NULL;
    if (eat(p, '?')) {
        capture = false;
        if (peek(p) == 'P' || peek(p) == '<') {
            eat(p, 'P');
            if (!eat(p, '<')) fail(p, "bad group");
            size_t start = p->i;
            while (more(p) && peek(p) != '>') p->i++;
            name = xstrndup((const char *)p->s + start, p->i - start);
            if (!eat(p, '>') || !*name) fail(p, "bad group name");
            capture = true;
        } else if (peek(p) == '=' || peek(p) == '!' || peek(p) == '<') {
            fail(p, "lookaround is not supported");
        } else {
            bool on = true;
            while (more(p) && peek(p) != ':' && peek(p) != ')') {
                int c = p->s[p->i++];
                unsigned f = c == 'i' ? REGEX_ICASE : c == 'm' ? REGEX_MULTILINE : c == 's' ? REGEX_DOTALL : 0;
                if (c == '-') on = false;
                else if (!f) fail(p, "unknown flag");
                else if (on) p->flags |= f;
                else p->flags &= ~f;
            }
            if (eat(p, ')')) { /* for the rest of the enclosing group */
                return node(p, N_EMPTY);
            }
            if (!eat(p, ':')) fail(p, "bad group");
        }
    }
    int index = -1;
    if (capture) {
        index = ++p->re->groups;
        VEC_PUSH(p->re->names, name);
    } else {
        free(name);
    }
    if (++p->depth > MAX_DEPTH) fail(p, "groups nested too deep");
    Node *body = parse_alt(p);
    p->depth--;
    if (!eat(p, ')')) fail(p, "missing )");
    if (scoped) p->flags = saved;
    Node *n = node(p, N_GROUP);
    n->index = index;
    n->a = body;
    return n;
}

static Node *assertion(Parser *p, int which) {
    Node *n = node(p, N_ASSERT);
    n->index = which;
    return n;
}

static Node *parse_atom(Parser *p) {
    if (eat(p, '(')) return parse_group(p);
    if (eat(p, '[')) return parse_class(p);
    if (eat(p, '.')) return node(p, N_ANY);
    if (eat(p, '^')) return assertion(p, A_BOL);
    if (eat(p, '$')) return assertion(p, A_EOL);
    if (eat(p, '\\')) {
        if (eat(p, 'b')) return assertion(p, A_WORD);
        if (eat(p, 'B')) return assertion(p, A_NOT_WORD);
        if (eat(p, 'A')) return assertion(p, A_BOT);
        if (eat(p, 'z')) return assertion(p, A_EOT);
        ClassItem it = {0, 0, -1, false};
        if (parse_set(p, &it)) {
            Class c = {0};
            VEC_PUSH(c.items, it);
            Node *n = node(p, N_CLASS);
            n->index = add_class(p, c);
            return n;
        }
        Node *n = node(p, N_CHAR);
        n->cp = parse_escape_char(p);
        return n;
    }
    int c = peek(p);
    if (c == '*' || c == '+' || c == '?') fail(p, "nothing to repeat");
    Node *n = node(p, N_CHAR);
    n->cp = next_cp(p);
    return n;
}

/* {m}, {m,}, {m,n}: true if there is one (a { that does not start one is a
 * character, as in most engines). */
static bool parse_counts(Parser *p, int *min, int *max) {
    size_t at = p->i;
    if (!eat(p, '{')) return false;
    long lo = 0, hi = -1;
    bool any = false;
    while (peek(p) >= '0' && peek(p) <= '9' && lo <= MAX_REPEAT) lo = lo * 10 + (p->s[p->i++] - '0'), any = true;
    if (any && eat(p, ',')) {
        if (peek(p) >= '0' && peek(p) <= '9') {
            hi = 0;
            while (peek(p) >= '0' && peek(p) <= '9' && hi <= MAX_REPEAT) hi = hi * 10 + (p->s[p->i++] - '0');
        }
    } else {
        hi = lo;
    }
    if (!any || !eat(p, '}')) {
        p->i = at;
        return false;
    }
    if (lo > MAX_REPEAT || hi > MAX_REPEAT) fail(p, "a repeat of more than %d", MAX_REPEAT);
    if (hi >= 0 && hi < lo) fail(p, "a repeat out of order");
    *min = (int)lo;
    *max = (int)hi;
    return true;
}

static Node *parse_repeat(Parser *p) {
    Node *a = parse_atom(p);
    for (;;) {
        int min, max;
        if (eat(p, '*')) min = 0, max = -1;
        else if (eat(p, '+')) min = 1, max = -1;
        else if (eat(p, '?')) min = 0, max = 1;
        else if (!parse_counts(p, &min, &max)) return a;
        Node *n = node(p, N_REPEAT);
        n->a = a;
        n->min = min;
        n->max = max;
        n->greedy = !eat(p, '?');
        a = n;
    }
}

static Node *parse_cat(Parser *p) {
    Node *left = NULL;
    while (more(p) && peek(p) != '|' && peek(p) != ')' && !p->failed) {
        Node *r = parse_repeat(p);
        if (!left) {
            left = r;
            continue;
        }
        Node *n = node(p, N_CAT);
        n->a = left;
        n->b = r;
        left = n;
    }
    return left ? left : node(p, N_EMPTY);
}

static Node *parse_alt(Parser *p) {
    Node *left = parse_cat(p);
    while (eat(p, '|')) {
        Node *n = node(p, N_ALT);
        n->a = left;
        n->b = parse_cat(p);
        left = n;
    }
    return left;
}

/* ---- compiling ---- */

static uint32_t emit(Parser *p, uint8_t op, uint32_t x, uint32_t y) {
    if (p->re->code.n >= MAX_PROGRAM) {
        fail(p, "the pattern is too large");
        return 0;
    }
    RInsn in = {op, x, y};
    VEC_PUSH(p->re->code, in);
    return (uint32_t)p->re->code.n - 1;
}

static uint32_t here(const Parser *p) { return (uint32_t)p->re->code.n; }

static void gen(Parser *p, const Node *n);

/* Whether a tree can match nothing. */
static bool nullable(const Node *n) {
    switch (n->kind) {
    case N_EMPTY: case N_ASSERT: return true;
    case N_CAT: return nullable(n->a) && nullable(n->b);
    case N_ALT: return nullable(n->a) || nullable(n->b);
    case N_GROUP: return nullable(n->a);
    case N_REPEAT: return !n->min || nullable(n->a);
    default: return false;
    }
}

static void set_split(Parser *p, uint32_t at, uint32_t body, bool greedy) {
    p->re->code.v[at].x = greedy ? body : here(p);
    p->re->code.v[at].y = greedy ? here(p) : body;
}

/* A loop. With a body that can match nothing it is (x+)?, with the choice
 * after the body: a pass that matched nothing finds that choice's way back
 * already taken at this position and leaves the loop, as a backtracking matcher
 * does. */
static void gen_star(Parser *p, const Node *body, bool greedy) {
    uint32_t split = emit(p, R_SPLIT, 0, 0);
    gen(p, body);
    if (nullable(body)) {
        uint32_t again = emit(p, R_SPLIT, 0, 0);
        if (p->failed) return;
        set_split(p, split, split + 1, greedy);
        set_split(p, again, split + 1, greedy);
        return;
    }
    emit(p, R_JMP, split, 0);
    if (p->failed) return;
    set_split(p, split, split + 1, greedy);
}

static void gen_repeat(Parser *p, const Node *n) {
    for (int i = 0; i < n->min && !p->failed; i++) gen(p, n->a);
    if (n->max < 0) {
        gen_star(p, n->a, n->greedy);
        return;
    }
    VEC(uint32_t) splits = {0};
    for (int i = n->min; i < n->max && !p->failed; i++) {
        VEC_PUSH(splits, emit(p, R_SPLIT, 0, 0));
        gen(p, n->a);
    }
    for (size_t i = 0; i < splits.n && !p->failed; i++) {
        RInsn *s = &p->re->code.v[splits.v[i]];
        s->x = n->greedy ? splits.v[i] + 1 : here(p);
        s->y = n->greedy ? here(p) : splits.v[i] + 1;
    }
    VEC_FREE(splits);
}

static uint32_t fold(uint32_t cp) { return (uint32_t)((int32_t)cp + uni_record(cp)->d_fold); }

static void gen(Parser *p, const Node *n) {
    if (p->failed) return;
    switch (n->kind) {
    case N_EMPTY: break;
    case N_CHAR:
        if (n->icase) emit(p, R_CHAR_I, fold(n->cp), 0);
        else emit(p, R_CHAR, n->cp, 0);
        break;
    case N_ANY: emit(p, n->dotall ? R_ANY_NL : R_ANY, 0, 0); break;
    case N_CLASS: emit(p, R_CLASS, (uint32_t)n->index, 0); break;
    case N_ASSERT: {
        int a = n->index;
        emit(p, R_ASSERT, (uint32_t)(a == A_BOL && !n->multiline ? A_BOT : a == A_EOL && !n->multiline ? A_EOT : a), 0);
        break;
    }
    case N_CAT:
        gen(p, n->a);
        gen(p, n->b);
        break;
    case N_ALT: {
        uint32_t split = emit(p, R_SPLIT, 0, 0);
        gen(p, n->a);
        uint32_t jmp = emit(p, R_JMP, 0, 0);
        uint32_t second = here(p);
        gen(p, n->b);
        if (p->failed) return;
        p->re->code.v[split].x = split + 1;
        p->re->code.v[split].y = second;
        p->re->code.v[jmp].x = here(p);
        break;
    }
    case N_REPEAT: gen_repeat(p, n); break;
    case N_GROUP:
        if (n->index >= 0) emit(p, R_SAVE, (uint32_t)n->index * 2, 0);
        gen(p, n->a);
        if (n->index >= 0) emit(p, R_SAVE, (uint32_t)n->index * 2 + 1, 0);
        break;
    default: break;
    }
}

void regex_free(Regex *re) {
    if (!re) return;
    VEC_FREE(re->code);
    for (size_t i = 0; i < re->classes.n; i++) VEC_FREE(re->classes.v[i].items);
    VEC_FREE(re->classes);
    for (size_t i = 0; i < re->names.n; i++) free(re->names.v[i]);
    VEC_FREE(re->names);
    free(re);
}

Regex *regex_compile(const char *pattern, size_t n, unsigned flags, char *err, size_t errlen) {
    Regex *re = xcalloc(1, sizeof *re);
    Parser p = {re, (const unsigned char *)pattern, n, 0, flags, 0, err, errlen, false, {0}};
    Node *root = parse_alt(&p);
    if (more(&p)) fail(&p, "unmatched )");
    if (!p.failed) {
        emit(&p, R_SAVE, 0, 0);
        gen(&p, root);
        emit(&p, R_SAVE, 1, 0);
        emit(&p, R_MATCH, 0, 0);
    }
    if (!p.failed && re->code.n * (size_t)(2 * (re->groups + 1)) > MAX_THREAD_WORDS) fail(&p, "the pattern is too large");
    for (size_t i = 0; i < p.nodes.n; i++) free(p.nodes.v[i]);
    VEC_FREE(p.nodes);
    if (p.failed) {
        regex_free(re);
        return NULL;
    }
    return re;
}

int regex_groups(const Regex *re) { return re->groups; }

int regex_group_named(const Regex *re, const char *name) {
    for (size_t i = 0; i < re->names.n; i++)
        if (re->names.v[i] && !strcmp(re->names.v[i], name)) return (int)i + 1;
    return -1;
}

/* ---- running ---- */

static bool in_set(uint32_t cp, int set) {
    switch (set) {
    case S_DIGIT: return uni_digit(cp) >= 0;
    case S_WORD: return cp == '_' || uni_is_letter(cp) || uni_is_number(cp);
    case S_SPACE: return uni_has(cp, UNI_P_WHITE);
    case S_LETTER: return uni_is_letter(cp);
    case S_NUMBER: return uni_is_number(cp);
    case S_PUNCT: return uni_is_punct(cp);
    case S_UPPER: return uni_has(cp, UNI_P_UPPER);
    default: return uni_has(cp, UNI_P_LOWER);
    }
}

static bool class_has(const Class *c, uint32_t cp) {
    for (size_t i = 0; i < c->items.n; i++) {
        const ClassItem *it = &c->items.v[i];
        if (it->set >= 0 ? in_set(cp, it->set) != it->negate : cp >= it->lo && cp <= it->hi) return true;
    }
    return false;
}

static bool class_match(const Class *c, uint32_t cp) {
    bool in = class_has(c, cp);
    if (!in && c->icase) {
        const UniRecord *r = uni_record(cp);
        in = class_has(c, (uint32_t)((int32_t)cp + r->d_fold)) || class_has(c, (uint32_t)((int32_t)cp + r->d_lower)) ||
             class_has(c, (uint32_t)((int32_t)cp + r->d_upper));
    }
    return in != c->negate;
}

static bool is_word(uint32_t cp) { return in_set(cp, S_WORD); }

/* The character before byte i. */
static uint32_t cp_before(const unsigned char *s, size_t i) {
    size_t k = i - 1;
    while (k > 0 && i - k < 4 && (s[k] & 0xC0) == 0x80) k--;
    size_t at = k;
    uint32_t cp = uni_utf8_next(s, i, &at);
    return at == i ? cp : 0xFFFD;
}

static bool assertion_holds(int which, const unsigned char *s, size_t n, size_t i) {
    switch (which) {
    case A_BOT: return i == 0;
    case A_EOT: return i == n;
    case A_BOL: return i == 0 || s[i - 1] == '\n';
    case A_EOL: return i == n || s[i] == '\n';
    default: {
        size_t at = i;
        bool before = i > 0 && is_word(cp_before(s, i)), after = i < n && is_word(uni_utf8_next(s, n, &at));
        return (before != after) == (which == A_WORD);
    }
    }
}

/* A list of threads, in priority order: a sparse set over the program, so a
 * state enters once. A thread's captures are at caps + its place * nslots. */
typedef struct {
    uint32_t *dense, *sparse;
    int64_t *caps;
    uint32_t n;
} Threads;

typedef struct {
    uint32_t pc;
    int slot;     /* >= 0: restore caps[slot] to `old` when this comes off the stack */
    int64_t old;
} Follow;

typedef struct {
    const Regex *re;
    const unsigned char *s;
    size_t n;
    int nslots;
    int64_t *cur;  /* the captures being followed */
    Follow *stack;
    uint64_t steps;
} Run;

static bool seen(const Threads *t, uint32_t pc) {
    uint32_t k = t->sparse[pc];
    return k < t->n && t->dense[k] == pc;
}

/* Add a thread at pc, following what takes no input, at byte i. */
static void add_thread(Run *r, Threads *t, uint32_t pc0, size_t i) {
    const RInsn *code = r->re->code.v;
    size_t top = 0;
    r->stack[top++] = (Follow){pc0, -1, 0};
    while (top) {
        Follow f = r->stack[--top];
        if (f.slot >= 0) {
            r->cur[f.slot] = f.old;
            continue;
        }
        uint32_t pc = f.pc;
        if (seen(t, pc)) continue;
        t->sparse[pc] = t->n;
        t->dense[t->n] = pc;
        uint32_t place = t->n++;
        r->steps++;
        const RInsn *in = &code[pc];
        switch (in->op) {
        case R_JMP: r->stack[top++] = (Follow){in->x, -1, 0}; break;
        case R_SPLIT:
            r->stack[top++] = (Follow){in->y, -1, 0};
            r->stack[top++] = (Follow){in->x, -1, 0};
            break;
        case R_SAVE:
            r->stack[top++] = (Follow){0, (int)in->x, r->cur[in->x]};
            r->cur[in->x] = (int64_t)i;
            r->stack[top++] = (Follow){pc + 1, -1, 0};
            break;
        case R_ASSERT:
            if (assertion_holds((int)in->x, r->s, r->n, i)) r->stack[top++] = (Follow){pc + 1, -1, 0};
            break;
        default: memcpy(t->caps + (size_t)place * (size_t)r->nslots, r->cur, (size_t)r->nslots * sizeof(int64_t)); break;
        }
    }
}

int regex_search(const Regex *re, const char *text, size_t n, size_t start, int64_t *caps, uint64_t *steps) {
    size_t np = re->code.n;
    int nslots = 2 * (re->groups + 1);
    /* Each split, save and jump adds to the stack at most twice over one state's visit. */
    Follow *stack = xmalloc((2 * np + 4) * sizeof *stack);
    Threads a, b, *cl = &a, *nl = &b;
    for (Threads *t = &a; t; t = t == &a ? &b : NULL) {
        t->dense = xmalloc(np * sizeof *t->dense);
        t->sparse = xcalloc(np, sizeof *t->sparse);
        t->caps = xmalloc(np * (size_t)nslots * sizeof *t->caps);
        t->n = 0;
    }
    int64_t *cur = xmalloc((size_t)nslots * sizeof *cur);
    Run r = {re, (const unsigned char *)text, n, nslots, cur, stack, 0};
    bool matched = false;
    size_t i = start;
    int result = REGEX_NONE;
    const uint64_t budget = *steps;
    for (;;) {
        if (!matched) { /* a new start, after all the earlier ones in priority */
            for (int k = 0; k < nslots; k++) cur[k] = -1;
            add_thread(&r, cl, 0, i);
        }
        if (!cl->n) break;
        if (r.steps > budget) {
            result = REGEX_OUT_OF_STEPS;
            break;
        }
        size_t next = i;
        uint32_t cp = i < n ? uni_utf8_next(r.s, n, &next) : 0;
        bool at_end = i >= n;
        uint32_t cpf = at_end ? 0 : fold(cp);
        nl->n = 0;
        for (uint32_t k = 0; k < cl->n; k++) {
            uint32_t pc = cl->dense[k];
            const RInsn *in = &re->code.v[pc];
            bool go = false;
            r.steps++;
            switch (in->op) {
            case R_CHAR: go = !at_end && cp == in->x; break;
            case R_CHAR_I: go = !at_end && cpf == in->x; break;
            case R_ANY: go = !at_end && cp != '\n'; break;
            case R_ANY_NL: go = !at_end; break;
            case R_CLASS: go = !at_end && class_match(&re->classes.v[in->x], cp); break;
            case R_MATCH:
                memcpy(caps, cl->caps + (size_t)k * (size_t)nslots, (size_t)nslots * sizeof(int64_t));
                matched = true;
                result = REGEX_MATCH;
                k = cl->n; /* the threads after it have lower priority */
                break;
            default: break;
            }
            if (go) {
                memcpy(cur, cl->caps + (size_t)k * (size_t)nslots, (size_t)nslots * sizeof(int64_t));
                add_thread(&r, nl, pc + 1, next);
            }
        }
        Threads *t = cl;
        cl = nl;
        nl = t;
        if (at_end) break;
        i = next;
        if (matched && !cl->n) break;
    }
    if (result == REGEX_OUT_OF_STEPS) *steps = 0;
    else *steps = r.steps > budget ? 0 : budget - r.steps;
    free(stack);
    free(cur);
    for (Threads *t = &a; t; t = t == &a ? &b : NULL) {
        free(t->dense);
        free(t->sparse);
        free(t->caps);
    }
    return result;
}
