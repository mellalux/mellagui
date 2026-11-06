/* JavaScript lexer and parser (see js.h for what is supported). Produces the AST of js_int.h. */
#include <ctype.h>
#include <math.h>

#include "js_int.h"

enum { TK_EOF, TK_NUM, TK_STR, TK_TPL, TK_ID, TK_P, TK_REGEX };

struct tpl {
    int n;                          /* number of expressions; there are n + 1 strings */
    jsstr **strs;
    const char **ex;
    size_t *exlen;
    int *exline;
};

struct tok {
    int t;
    bool nl;                        /* a line break precedes the token */
    bool esc;                       /* string/identifier contained escapes (keywords written with escapes are not keywords) */
    jsstr *s;                       /* identifier name / string value / regex pattern */
    jsstr *s2;                      /* regex flags */
    double num;
    char p[5];                      /* punctuator */
    struct tpl *tpl;
    const char *start, *end;
    int line;
};

struct pctx {
    struct fninfo *fi;
    struct pctx *up;
};

struct parser {
    js *J;
    bool next_gen, next_async;      /* flags for the function parsed next (`*name(){}` / `async name(){}` methods) */
    const char *src, *p, *end;
    int line;
    struct tok t;
    const char *prev_end;
    bool prev_value;                /* the previous token ends an expression (so a following / divides) */
    uint64_t pmask;                 /* per open parenthesis: does it close a statement header `if (...)`, `for (...)` ... */
    int pdepth;
    bool rparen_hdr;                /* the last `)` closed such a header (a / after it starts a regular expression) */
    struct pctx *ctx;
    bool no_in;
    bool lexflag;
    jmp_buf jb;
    char err[256];
};

static void perr(struct parser *P, const char *fmt, ...) __attribute__((noreturn, format(printf, 2, 3)));
static void perr(struct parser *P, const char *fmt, ...)
{
    char m[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m, sizeof(m), fmt, ap);
    va_end(ap);
    char near[48] = "";
    const char *at = P->t.start && P->t.start < P->end ? P->t.start : P->p;
    if (at && at < P->end) {
        size_t n = (size_t)(P->end - at);
        if (n > 24) n = 24;
        for (size_t i = 0; i < n && i + 1 < sizeof(near); i++)
            near[i] = at[i] == '\n' || at[i] == '\r' ? ' ' : at[i];
        near[n] = 0;
    }
    snprintf(P->err, sizeof(P->err), "SyntaxError: %s (line %d, near \"%s\")", m, P->t.line ? P->t.line : P->line, near);
    longjmp(P->jb, 1);
}

/* ---- lexer ------------------------------------------------------------------------------------------------- */

static int utf8_enc(uint32_t cp, char *o)
{
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xc0 | cp >> 6); o[1] = (char)(0x80 | (cp & 63)); return 2; }
    if (cp < 0x10000) {
        o[0] = (char)(0xe0 | cp >> 12); o[1] = (char)(0x80 | ((cp >> 6) & 63)); o[2] = (char)(0x80 | (cp & 63));
        return 3;
    }
    o[0] = (char)(0xf0 | cp >> 18); o[1] = (char)(0x80 | ((cp >> 12) & 63));
    o[2] = (char)(0x80 | ((cp >> 6) & 63)); o[3] = (char)(0x80 | (cp & 63));
    return 4;
}

static int hexv(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool id_start(int c) { return isalpha(c) || c == '_' || c == '$' || c >= 0x80 || c == '\\'; }
static bool id_part(int c) { return isalnum(c) || c == '_' || c == '$' || c >= 0x80; }

/* Append a string-literal escape sequence starting after the backslash; returns the new source position. */
static const char *read_escape(struct parser *P, const char *p, const char *end, char **o, bool tpl)
{
    (void)tpl;
    if (p >= end)
        perr(P, "unterminated string");
    char c = *p++;
    switch (c) {
    case 'n': *(*o)++ = '\n'; break;
    case 't': *(*o)++ = '\t'; break;
    case 'r': *(*o)++ = '\r'; break;
    case 'b': *(*o)++ = '\b'; break;
    case 'f': *(*o)++ = '\f'; break;
    case 'v': *(*o)++ = '\v'; break;
    case '0': if (p < end && isdigit((unsigned char)*p)) *(*o)++ = '0'; else *(*o)++ = 0; break;
    case '\r': if (p < end && *p == '\n') p++; P->line++; break;
    case '\n': P->line++; break;
    case 'x': {
        if (p + 2 > end || hexv(p[0]) < 0 || hexv(p[1]) < 0)
            perr(P, "bad \\x escape");
        *o += utf8_enc((uint32_t)(hexv(p[0]) * 16 + hexv(p[1])), *o);
        p += 2;
        break;
    }
    case 'u': {
        uint32_t cp = 0;
        if (p < end && *p == '{') {
            p++;
            while (p < end && *p != '}' && hexv(*p) >= 0)
                cp = cp * 16 + (uint32_t)hexv(*p++);
            if (p >= end || *p != '}')
                perr(P, "bad \\u{} escape");
            p++;
        } else {
            if (p + 4 > end)
                perr(P, "bad \\u escape");
            for (int i = 0; i < 4; i++) {
                if (hexv(p[i]) < 0)
                    perr(P, "bad \\u escape");
                cp = cp * 16 + (uint32_t)hexv(p[i]);
            }
            p += 4;
            if (cp >= 0xd800 && cp < 0xdc00 && p + 6 <= end && p[0] == '\\' && p[1] == 'u') {   /* surrogate pair */
                uint32_t lo = 0;
                bool ok = true;
                for (int i = 0; i < 4; i++) {
                    if (hexv(p[2 + i]) < 0) { ok = false; break; }
                    lo = lo * 16 + (uint32_t)hexv(p[2 + i]);
                }
                if (ok && lo >= 0xdc00 && lo < 0xe000) {
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                    p += 6;
                }
            }
        }
        *o += utf8_enc(cp, *o);
        break;
    }
    default:
        *(*o)++ = c;
    }
    return p;
}

static const char *const PUNCT[] = {
    ">>>=", "...", "===", "!==", "**=", "<<=", ">>=", ">>>", "&&=", "||=", "?\?=",
    "=>", "==", "!=", "<=", ">=", "&&", "||", "??", "?.", "++", "--", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=",
    "<<", ">>", "**",
    "{", "}", "(", ")", "[", "]", ";", ",", "<", ">", "+", "-", "*", "/", "%", "&", "|", "^", "!", "~", "?", ":", "=",
    ".", "@", "#", NULL
};

static void lex_template(struct parser *P, struct tok *t)
{
    const char *p = P->p;                   /* after the opening backtick */
    struct tpl *tp = js_alloc(P->J, sizeof(*tp));
    int cap = 4;
    tp->strs = js_alloc(P->J, sizeof(jsstr *) * (size_t)cap);
    tp->ex = js_alloc(P->J, sizeof(char *) * (size_t)cap);
    tp->exlen = js_alloc(P->J, sizeof(size_t) * (size_t)cap);
    tp->exline = js_alloc(P->J, sizeof(int) * (size_t)cap);
    int ns = 0;
    char *buf = malloc((size_t)(P->end - p) + 8);
    if (!buf)
        perr(P, "out of memory");
    char *o = buf;
    for (;;) {
        if (p >= P->end) {
            free(buf);
            perr(P, "unterminated template literal");
        }
        if (*p == '`') {
            p++;
            break;
        }
        if (*p == '\\') {
            P->p = p + 1;
            p = read_escape(P, p + 1, P->end, &o, true);
            continue;
        }
        if (*p == '$' && p + 1 < P->end && p[1] == '{') {
            if (ns + 2 > cap) {
                cap *= 2;
                jsstr **ns_ = js_alloc(P->J, sizeof(jsstr *) * (size_t)cap);
                const char **ne = js_alloc(P->J, sizeof(char *) * (size_t)cap);
                size_t *nl = js_alloc(P->J, sizeof(size_t) * (size_t)cap);
                int *nli = js_alloc(P->J, sizeof(int) * (size_t)cap);
                memcpy(ns_, tp->strs, sizeof(jsstr *) * (size_t)ns);
                memcpy(ne, tp->ex, sizeof(char *) * (size_t)ns);
                memcpy(nl, tp->exlen, sizeof(size_t) * (size_t)ns);
                memcpy(nli, tp->exline, sizeof(int) * (size_t)ns);
                tp->strs = ns_; tp->ex = ne; tp->exlen = nl; tp->exline = nli;
            }
            tp->strs[ns] = js_str_new(P->J, buf, (size_t)(o - buf));
            o = buf;
            p += 2;
            const char *es = p;
            int depth = 1;
            while (p < P->end && depth > 0) {
                char c = *p;
                if (c == '\'' || c == '"') {
                    p++;
                    while (p < P->end && *p != c) {
                        if (*p == '\\') p++;
                        p++;
                    }
                } else if (c == '`') {              /* nested template: skip it (its own ${} may contain braces) */
                    int d2 = 0;
                    p++;
                    while (p < P->end && !(*p == '`' && d2 == 0)) {
                        if (*p == '\\') p++;
                        else if (*p == '$' && p + 1 < P->end && p[1] == '{') { d2++; p++; }
                        else if (*p == '}' && d2 > 0) d2--;
                        p++;
                    }
                } else if (c == '{') {
                    depth++;
                } else if (c == '}') {
                    depth--;
                    if (depth == 0)
                        break;
                } else if (c == '\n') {
                    P->line++;
                }
                p++;
            }
            if (p >= P->end) {
                free(buf);
                perr(P, "unterminated template expression");
            }
            tp->ex[ns] = es;
            tp->exlen[ns] = (size_t)(p - es);
            tp->exline[ns] = P->line;
            ns++;
            p++;                                    /* the } */
            continue;
        }
        if (*p == '\n')
            P->line++;
        if (*p == '\r' && p + 1 < P->end && p[1] == '\n')
            p++;
        *o++ = *p++;
    }
    tp->strs[ns] = js_str_new(P->J, buf, (size_t)(o - buf));
    tp->n = ns;
    free(buf);
    P->p = p;
    t->t = TK_TPL;
    t->tpl = tp;
}

static bool token_ends_value(const struct parser *P, const struct tok *t)
{
    switch (t->t) {
    case TK_NUM: case TK_STR: case TK_TPL: case TK_REGEX: return true;
    case TK_ID: {
        static const char *const before_expr[] = { "return", "typeof", "instanceof", "in", "of", "new", "delete", "void",
                                                    "throw", "case", "do", "else", "yield", "await", NULL };
        if (t->esc)
            return true;
        for (int i = 0; before_expr[i]; i++)
            if (!strcmp(t->s->s, before_expr[i]))
                return false;
        return true;
    }
    case TK_P:
        if (!strcmp(t->p, ")"))
            return !P->rparen_hdr;
        return !strcmp(t->p, "]") || !strcmp(t->p, "++") || !strcmp(t->p, "--");
    default: return false;
    }
}

static void lex(struct parser *P)
{
    struct tok *t = &P->t;
    P->prev_end = t->end;
    P->prev_value = token_ends_value(P, t);
    bool prev_hdr_kw = t->t == TK_ID && !t->esc && (!strcmp(t->s->s, "if") || !strcmp(t->s->s, "for") ||
                                                     !strcmp(t->s->s, "while") || !strcmp(t->s->s, "with"));
    memset(t, 0, sizeof(*t));
    const char *p = P->p;
    for (;;) {
        while (p < P->end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == '\v' || *p == '\f' ||
                              (unsigned char)*p == 0xc2 || (unsigned char)*p == 0xef)) {
            if ((unsigned char)*p == 0xc2) {                /* U+00A0 */
                if (p + 1 < P->end && (unsigned char)p[1] == 0xa0) { p += 2; continue; }
                break;
            }
            if ((unsigned char)*p == 0xef) {                /* BOM U+FEFF */
                if (p + 2 < P->end && (unsigned char)p[1] == 0xbb && (unsigned char)p[2] == 0xbf) { p += 3; continue; }
                break;
            }
            if (*p == '\n') {
                P->line++;
                t->nl = true;
            }
            p++;
        }
        if (p + 1 < P->end && p[0] == '/' && p[1] == '/') {
            while (p < P->end && *p != '\n')
                p++;
            continue;
        }
        if (p + 1 < P->end && p[0] == '/' && p[1] == '*') {
            p += 2;
            while (p + 1 < P->end && !(p[0] == '*' && p[1] == '/')) {
                if (*p == '\n') {
                    P->line++;
                    t->nl = true;
                }
                p++;
            }
            p = p + 1 < P->end ? p + 2 : P->end;
            continue;
        }
        if (p + 3 < P->end && p[0] == '<' && p[1] == '!' && p[2] == '-' && p[3] == '-') {   /* <!-- html comment */
            while (p < P->end && *p != '\n')
                p++;
            continue;
        }
        break;
    }
    t->start = p;
    t->line = P->line;
    if (p >= P->end) {
        t->t = TK_EOF;
        t->end = p;
        P->p = p;
        return;
    }
    unsigned char c = (unsigned char)*p;
    if (id_start(c)) {
        const char *s = p;
        while (p < P->end && id_part((unsigned char)*p) && *p != '\\')
            p++;
        if (p < P->end && *p == '\\') {                     /* identifier with \uXXXX escapes */
            char *buf = malloc((size_t)(P->end - s) + 8);
            if (!buf)
                perr(P, "out of memory");
            char *o = buf;
            memcpy(o, s, (size_t)(p - s));
            o += p - s;
            while (p < P->end && (id_part((unsigned char)*p) || *p == '\\')) {
                if (*p == '\\') {
                    if (p + 1 >= P->end || p[1] != 'u') {
                        free(buf);
                        perr(P, "bad escape in identifier");
                    }
                    p = read_escape(P, p + 1, P->end, &o, false);
                } else {
                    *o++ = *p++;
                }
            }
            t->esc = true;
            t->s = js_intern(P->J, buf, (size_t)(o - buf));
            free(buf);
        } else {
            t->s = js_intern(P->J, s, (size_t)(p - s));
        }
        t->t = TK_ID;
    } else if (isdigit(c) || (c == '.' && p + 1 < P->end && isdigit((unsigned char)p[1]))) {
        char num[128];
        size_t nl = 0;
        const char *s = p;
        if (c == '0' && p + 1 < P->end && strchr("xXoObB", p[1])) {
            int base = (p[1] == 'x' || p[1] == 'X') ? 16 : (p[1] == 'o' || p[1] == 'O') ? 8 : 2;
            p += 2;
            double v = 0;
            while (p < P->end && (hexv((unsigned char)*p) >= 0 || *p == '_')) {
                if (*p != '_') {
                    int d = hexv((unsigned char)*p);
                    if (d >= base)
                        break;
                    v = v * base + d;
                }
                p++;
            }
            t->num = v;
        } else {
            while (p < P->end && (isdigit((unsigned char)*p) || *p == '_')) {
                if (*p != '_' && nl < sizeof(num) - 1) num[nl++] = *p;
                p++;
            }
            if (p < P->end && *p == '.') {
                if (nl < sizeof(num) - 1) num[nl++] = *p;
                p++;
                while (p < P->end && (isdigit((unsigned char)*p) || *p == '_')) {
                    if (*p != '_' && nl < sizeof(num) - 1) num[nl++] = *p;
                    p++;
                }
            }
            if (p < P->end && (*p == 'e' || *p == 'E')) {
                const char *q = p + 1;
                if (q < P->end && (*q == '+' || *q == '-'))
                    q++;
                if (q < P->end && isdigit((unsigned char)*q)) {
                    while (p < q && nl < sizeof(num) - 1) num[nl++] = *p++;
                    while (p < P->end && isdigit((unsigned char)*p)) {
                        if (nl < sizeof(num) - 1) num[nl++] = *p;
                        p++;
                    }
                }
            }
            num[nl] = 0;
            t->num = strtod(num, NULL);
        }
        (void)s;
        if (p < P->end && id_start((unsigned char)*p))
            perr(P, "identifier directly after a number (BigInt is not supported)");
        t->t = TK_NUM;
    } else if (c == '"' || c == '\'') {
        char q = (char)c;
        p++;
        char *buf = malloc((size_t)(P->end - p) + 8);
        if (!buf)
            perr(P, "out of memory");
        char *o = buf;
        while (p < P->end && *p != q) {
            if (*p == '\n') {
                free(buf);
                perr(P, "unterminated string");
            }
            if (*p == '\\') {
                p = read_escape(P, p + 1, P->end, &o, false);
                t->esc = true;
            } else {
                *o++ = *p++;
            }
        }
        if (p >= P->end) {
            free(buf);
            perr(P, "unterminated string");
        }
        p++;
        t->t = TK_STR;
        t->s = js_str_new(P->J, buf, (size_t)(o - buf));
        free(buf);
    } else if (c == '`') {
        P->p = p + 1;
        lex_template(P, t);
        p = P->p;
    } else if (c == '/' && !P->prev_value) {                  /* regular expression literal */
        const char *q = p + 1;
        bool in_class = false;
        for (;;) {
            if (q >= P->end || *q == '\n')
                perr(P, "unterminated regular expression");
            if (*q == '\\') { q += 2; continue; }
            if (*q == '[') in_class = true;
            else if (*q == ']') in_class = false;
            else if (*q == '/' && !in_class) break;
            q++;
        }
        t->t = TK_REGEX;
        t->s = js_str_new(P->J, p + 1, (size_t)(q - p - 1));
        q++;
        const char *fs = q;
        while (q < P->end && id_part((unsigned char)*q))
            q++;
        t->s2 = js_str_new(P->J, fs, (size_t)(q - fs));
        p = q;
    } else {
        t->t = TK_P;
        bool found = false;
        for (int i = 0; PUNCT[i]; i++) {
            size_t l = strlen(PUNCT[i]);
            if ((size_t)(P->end - p) >= l && !strncmp(p, PUNCT[i], l)) {
                if (!strcmp(PUNCT[i], "?.") && p + 2 < P->end && isdigit((unsigned char)p[2]))
                    continue;                           /* a ? .5 : x */
                memcpy(t->p, PUNCT[i], l + 1);
                p += l;
                found = true;
                break;
            }
        }
        if (!found)
            perr(P, "unexpected character '%c'", c);
        if (!strcmp(t->p, "(")) {
            if (P->pdepth < 63) {
                if (prev_hdr_kw) P->pmask |= 1ULL << P->pdepth;
                else P->pmask &= ~(1ULL << P->pdepth);
            }
            P->pdepth++;
        } else if (!strcmp(t->p, ")")) {
            if (P->pdepth > 0)
                P->pdepth--;
            P->rparen_hdr = P->pdepth < 63 && ((P->pmask >> P->pdepth) & 1);
        }
    }
    t->end = p;
    P->p = p;
}

/* ---- token helpers ----------------------------------------------------------------------------------------- */

static inline bool is_p(struct parser *P, const char *s) { return P->t.t == TK_P && !strcmp(P->t.p, s); }
static inline bool is_id(struct parser *P, const char *s) { return P->t.t == TK_ID && !P->t.esc && !strcmp(P->t.s->s, s); }

static bool eat_p(struct parser *P, const char *s)
{
    if (is_p(P, s)) {
        lex(P);
        return true;
    }
    return false;
}

static bool eat_id(struct parser *P, const char *s)
{
    if (is_id(P, s)) {
        lex(P);
        return true;
    }
    return false;
}

static void expect_p(struct parser *P, const char *s)
{
    if (!eat_p(P, s)) {
        if (P->t.t == TK_EOF)
            perr(P, "unexpected end of script, expected '%s'", s);
        perr(P, "expected '%s'", s);
    }
}

static const char *const RESERVED[] = {
    "break", "case", "catch", "class", "const", "continue", "debugger", "default", "delete", "do", "else", "export",
    "extends", "finally", "for", "function", "if", "import", "in", "instanceof", "new", "return", "super", "switch",
    "this", "throw", "try", "typeof", "var", "void", "while", "with", "null", "true", "false", "enum", NULL
};

static bool is_reserved(const char *s)
{
    for (int i = 0; RESERVED[i]; i++)
        if (!strcmp(RESERVED[i], s))
            return true;
    return false;
}

struct lexstate { const char *p, *prev_end; int line; struct tok t; bool prev_value; uint64_t pmask; int pdepth; bool rparen_hdr; };

static void save(struct parser *P, struct lexstate *ls) { ls->p = P->p; ls->prev_end = P->prev_end; ls->line = P->line; ls->t = P->t; ls->prev_value = P->prev_value; ls->pmask = P->pmask; ls->pdepth = P->pdepth; ls->rparen_hdr = P->rparen_hdr; }
static void restore(struct parser *P, const struct lexstate *ls) { P->p = ls->p; P->prev_end = ls->prev_end; P->line = ls->line; P->t = ls->t; P->prev_value = ls->prev_value; P->pmask = ls->pmask; P->pdepth = ls->pdepth; P->rparen_hdr = ls->rparen_hdr; }

static void consume_semi(struct parser *P)
{
    if (eat_p(P, ";"))
        return;
    if (is_p(P, "}") || P->t.t == TK_EOF || P->t.nl)
        return;
    perr(P, "unexpected token");
}

/* ---- nodes -------------------------------------------------------------------------------------------------- */

static struct node *nnew(struct parser *P, int type)
{
    struct node *n = js_alloc(P->J, sizeof(*n));
    n->t = (uint8_t)type;
    n->line = P->t.line;
    return n;
}

struct nlist { struct node **v; int n, cap; };

static void nl_add(struct parser *P, struct nlist *l, struct node *n)
{
    if (l->n == l->cap) {
        int nc = l->cap ? l->cap * 2 : 4;
        struct node **nv = js_alloc(P->J, sizeof(*nv) * (size_t)nc);
        if (l->n)
            memcpy(nv, l->v, sizeof(*nv) * (size_t)l->n);
        l->v = nv;
        l->cap = nc;
    }
    l->v[l->n++] = n;
}

static void finish_list(struct node *n, struct nlist *l)
{
    n->list = l->v;
    n->n = l->n;
}

static struct node *parse_assign(struct parser *P);
static struct node *parse_expr(struct parser *P);
static struct node *parse_stmt(struct parser *P);
static struct node *parse_function(struct parser *P, bool is_decl, bool is_expr, bool is_method, int method_kind);
static struct node *parse_class(struct parser *P, bool is_decl);
static struct node *parse_block(struct parser *P);

static void fn_add_var(struct parser *P, jsstr *name)
{
    struct fninfo *fi = P->ctx->fi;
    for (int i = 0; i < fi->nvars; i++)
        if (fi->vars[i] == name)
            return;
    jsstr **nv = js_alloc(P->J, sizeof(*nv) * (size_t)(fi->nvars + 1));
    if (fi->nvars)
        memcpy(nv, fi->vars, sizeof(*nv) * (size_t)fi->nvars);
    nv[fi->nvars++] = name;
    fi->vars = nv;
}

static void collect_pattern_vars(struct parser *P, struct node *pat)
{
    if (!pat)
        return;
    switch (pat->t) {
    case N_IDENT: fn_add_var(P, pat->s); break;
    case N_ASSIGN: collect_pattern_vars(P, pat->a); break;
    case N_SPREAD: collect_pattern_vars(P, pat->a); break;
    case N_ARRAY:
        for (int i = 0; i < pat->n; i++)
            collect_pattern_vars(P, pat->list[i]);
        break;
    case N_OBJECT:
        for (int i = 0; i < pat->n; i++)
            collect_pattern_vars(P, pat->list[i]->b);
        break;
    case N_PROP: collect_pattern_vars(P, pat->b); break;
    default: break;
    }
}

/* ---- binding patterns (declarations, parameters) ---------------------------------------------------------- */

static struct node *parse_binding(struct parser *P);

static struct node *parse_binding_elem(struct parser *P)
{
    struct node *t = parse_binding(P);
    if (is_p(P, "=")) {
        struct node *a = nnew(P, N_ASSIGN);
        lex(P);
        a->op = OP_ASSIGN;
        a->a = t;
        bool old = P->no_in;
        P->no_in = false;
        a->b = parse_assign(P);
        P->no_in = old;
        return a;
    }
    return t;
}

static struct node *parse_binding(struct parser *P)
{
    if (P->t.t == TK_ID && !is_reserved(P->t.s->s)) {
        struct node *n = nnew(P, N_IDENT);
        n->s = P->t.s;
        lex(P);
        return n;
    }
    if (is_p(P, "[")) {
        struct node *n = nnew(P, N_ARRAY);
        struct nlist l = { 0 };
        lex(P);
        while (!is_p(P, "]")) {
            if (is_p(P, ",")) {
                nl_add(P, &l, nnew(P, N_HOLE));
                lex(P);
                continue;
            }
            if (is_p(P, "...")) {
                struct node *s = nnew(P, N_SPREAD);
                lex(P);
                s->a = parse_binding(P);
                nl_add(P, &l, s);
            } else {
                nl_add(P, &l, parse_binding_elem(P));
            }
            if (!is_p(P, "]"))
                expect_p(P, ",");
        }
        lex(P);
        finish_list(n, &l);
        return n;
    }
    if (is_p(P, "{")) {
        struct node *n = nnew(P, N_OBJECT);
        struct nlist l = { 0 };
        lex(P);
        while (!is_p(P, "}")) {
            struct node *pr = nnew(P, N_PROP);
            if (is_p(P, "...")) {
                lex(P);
                pr->kind = PK_SPREAD;
                pr->b = parse_binding(P);
            } else {
                if (P->t.t == TK_ID || P->t.t == TK_STR) {
                    pr->a = nnew(P, N_STR);
                    pr->a->s = P->t.s;
                    bool ident = P->t.t == TK_ID;
                    jsstr *name = P->t.s;
                    lex(P);
                    if (is_p(P, ":")) {
                        lex(P);
                        pr->b = parse_binding_elem(P);
                    } else if (ident) {                     /* shorthand, maybe with a default */
                        struct node *id = nnew(P, N_IDENT);
                        id->s = name;
                        pr->b = id;
                        if (is_p(P, "=")) {
                            struct node *a = nnew(P, N_ASSIGN);
                            lex(P);
                            a->op = OP_ASSIGN;
                            a->a = id;
                            a->b = parse_assign(P);
                            pr->b = a;
                        }
                    } else {
                        perr(P, "bad destructuring pattern");
                    }
                } else if (P->t.t == TK_NUM) {
                    pr->a = nnew(P, N_NUM);
                    pr->a->num = P->t.num;
                    lex(P);
                    expect_p(P, ":");
                    pr->b = parse_binding_elem(P);
                } else if (is_p(P, "[")) {
                    lex(P);
                    pr->flag = 1;
                    pr->a = parse_assign(P);
                    expect_p(P, "]");
                    expect_p(P, ":");
                    pr->b = parse_binding_elem(P);
                } else {
                    perr(P, "bad destructuring pattern");
                }
            }
            nl_add(P, &l, pr);
            if (!is_p(P, "}"))
                expect_p(P, ",");
        }
        lex(P);
        finish_list(n, &l);
        return n;
    }
    perr(P, "expected a variable name");
}

/* ---- functions ---------------------------------------------------------------------------------------------- */

static void parse_params(struct parser *P, struct fninfo *fi)
{
    struct nlist l = { 0 };
    expect_p(P, "(");
    while (!is_p(P, ")")) {
        if (is_p(P, "...")) {
            struct node *s = nnew(P, N_SPREAD);
            lex(P);
            s->a = parse_binding(P);
            nl_add(P, &l, s);
        } else {
            nl_add(P, &l, parse_binding_elem(P));
        }
        if (!is_p(P, ")"))
            expect_p(P, ",");
    }
    lex(P);
    fi->params = l.v;
    fi->nparams = l.n;
    fi->nformal = 0;
    for (int i = 0; i < l.n; i++) {
        if (l.v[i]->t == N_SPREAD || l.v[i]->t == N_ASSIGN)
            break;
        fi->nformal++;
    }
}

static struct fninfo *new_fninfo(struct parser *P)
{
    return js_alloc(P->J, sizeof(struct fninfo));
}

/* function body `{ ... }` (the opening brace is the current token) */
static struct node *parse_fn_body(struct parser *P, struct fninfo *fi)
{
    struct pctx ctx = { .fi = fi, .up = P->ctx };
    P->ctx = &ctx;
    bool old_lex = P->lexflag, old_no_in = P->no_in;
    P->no_in = false;
    struct node *body = nnew(P, N_BLOCK);
    struct nlist l = { 0 };
    expect_p(P, "{");
    while (!is_p(P, "}")) {
        if (P->t.t == TK_EOF)
            perr(P, "unexpected end of script in function body");
        nl_add(P, &l, parse_stmt(P));
    }
    lex(P);
    finish_list(body, &l);
    P->ctx = ctx.up;
    P->lexflag = old_lex;
    P->no_in = old_no_in;
    return body;
}

/* `function [name](params) { body }`; the current token is `function` (or, for methods, the name). */
static struct node *parse_function(struct parser *P, bool is_decl, bool is_expr, bool is_method, int method_kind)
{
    const char *start = P->t.start;
    struct node *n = nnew(P, N_FUNC);
    struct fninfo *fi = new_fninfo(P);
    n->fi = fi;
    fi->is_gen = P->next_gen;
    fi->is_async = P->next_async;
    P->next_gen = P->next_async = false;
    if (!is_method) {
        lex(P);                                         /* `function` */
        if (eat_p(P, "*"))
            fi->is_gen = 1;
        if (P->t.t == TK_ID && !is_reserved(P->t.s->s)) {
            fi->name = P->t.s;
            lex(P);
        } else if (is_decl) {
            perr(P, "function name expected");
        }
    }
    (void)is_expr;
    fi->is_method = is_method;
    fi->is_getter = method_kind == PK_GET;
    fi->is_setter = method_kind == PK_SET;
    struct pctx ctx = { .fi = fi, .up = P->ctx };
    P->ctx = &ctx;
    parse_params(P, fi);
    P->ctx = ctx.up;
    fi->body = parse_fn_body(P, fi);
    fi->src = start;
    fi->src_len = (uint32_t)(P->prev_end - start);
    n->s = fi->name;
    return n;
}

static bool arrow_ahead(struct parser *P)
{
    /* current token is "(": is the matching ")" followed by "=>" ? */
    struct lexstate ls;
    save(P, &ls);
    int depth = 0;
    bool res = false;
    for (;;) {
        if (P->t.t == TK_EOF)
            break;
        if (P->t.t == TK_P) {
            if (!strcmp(P->t.p, "(") || !strcmp(P->t.p, "[") || !strcmp(P->t.p, "{"))
                depth++;
            else if (!strcmp(P->t.p, ")") || !strcmp(P->t.p, "]") || !strcmp(P->t.p, "}")) {
                depth--;
                if (depth == 0) {
                    lex(P);
                    res = is_p(P, "=>") && !P->t.nl;
                    break;
                }
            }
        }
        lex(P);
    }
    restore(P, &ls);
    return res;
}

static struct node *parse_arrow_body(struct parser *P, struct fninfo *fi, const char *start)
{
    struct node *n = nnew(P, N_FUNC);
    n->fi = fi;
    fi->is_arrow = 1;
    if (is_p(P, "{")) {
        fi->body = parse_fn_body(P, fi);
    } else {
        struct pctx ctx = { .fi = fi, .up = P->ctx };
        P->ctx = &ctx;
        fi->expr_body = 1;
        fi->body = parse_assign(P);
        P->ctx = ctx.up;
    }
    fi->src = start;
    fi->src_len = (uint32_t)(P->prev_end - start);
    return n;
}

static struct node *parse_arrow(struct parser *P, bool is_async)
{
    /* at an identifier or "(" known to start an arrow function */
    const char *start = P->t.start;
    struct fninfo *fi = new_fninfo(P);
    fi->is_async = is_async;
    struct pctx ctx = { .fi = fi, .up = P->ctx };
    P->ctx = &ctx;
    if (P->t.t == TK_ID) {
        struct node *id = nnew(P, N_IDENT);
        id->s = P->t.s;
        lex(P);
        fi->params = js_alloc(P->J, sizeof(struct node *));
        fi->params[0] = id;
        fi->nparams = fi->nformal = 1;
    } else {
        parse_params(P, fi);
    }
    P->ctx = ctx.up;
    expect_p(P, "=>");
    return parse_arrow_body(P, fi, start);
}

/* ---- classes ------------------------------------------------------------------------------------------------- */

static struct node *parse_prop_key(struct parser *P, struct node *pr)
{
    /* property name: identifier/keyword, string, number, or [computed] */
    if (P->t.t == TK_ID || P->t.t == TK_STR) {
        pr->a = nnew(P, N_STR);
        pr->a->s = P->t.s;
        lex(P);
    } else if (P->t.t == TK_NUM) {
        pr->a = nnew(P, N_NUM);
        pr->a->num = P->t.num;
        lex(P);
    } else if (is_p(P, "[")) {
        lex(P);
        pr->flag |= 1;
        pr->a = parse_assign(P);
        expect_p(P, "]");
    } else if (is_p(P, "#")) {
        perr(P, "private class members are not supported");
    } else {
        perr(P, "bad property name");
    }
    return pr->a;
}

static struct node *parse_class(struct parser *P, bool is_decl)
{
    struct node *n = nnew(P, N_CLASS);
    const char *start = P->t.start;
    lex(P);                                             /* class */
    if (P->t.t == TK_ID && !is_p(P, "{") && !is_id(P, "extends")) {
        n->s = P->t.s;
        lex(P);
    } else if (is_decl) {
        perr(P, "class name expected");
    }
    if (eat_id(P, "extends"))
        n->a = parse_assign(P);                          /* (left-hand-side expression in the spec) */
    expect_p(P, "{");
    struct nlist l = { 0 };
    while (!is_p(P, "}")) {
        if (eat_p(P, ";"))
            continue;
        if (P->t.t == TK_EOF)
            perr(P, "unexpected end of script in class body");
        struct node *pr = nnew(P, N_PROP);
        pr->kind = PK_METHOD;
        if (is_id(P, "static")) {
            struct lexstate ls;
            save(P, &ls);
            lex(P);
            if (is_p(P, "(") || is_p(P, "=") || is_p(P, ";") || is_p(P, "}"))
                restore(P, &ls);                         /* a member called "static" */
            else
                pr->flag |= 2;
        }
        int kind = PK_METHOD;
        if ((is_id(P, "get") || is_id(P, "set")) ) {
            struct lexstate ls;
            save(P, &ls);
            bool getter = is_id(P, "get");
            lex(P);
            if (is_p(P, "(") || is_p(P, "=") || is_p(P, ";") || is_p(P, "}"))
                restore(P, &ls);
            else
                kind = getter ? PK_GET : PK_SET;
        }
        bool m_async = false, m_gen = false;
        if (is_id(P, "async")) {
            struct lexstate ls;
            save(P, &ls);
            lex(P);
            if (is_p(P, "(") || is_p(P, "=") || is_p(P, ";") || is_p(P, "}") || P->t.nl)
                restore(P, &ls);
            else
                m_async = true;
        }
        if (is_p(P, "*")) {
            lex(P);
            m_gen = true;
        }
        parse_prop_key(P, pr);
        if (is_p(P, "(")) {
            pr->kind = (uint8_t)kind;
            P->next_async = m_async;
            P->next_gen = m_gen;
            pr->b = parse_function(P, false, true, true, kind);
        } else {                                        /* class field */
            pr->kind = PK_INIT;
            if (eat_p(P, "=")) {
                struct fninfo *fi = new_fninfo(P);       /* initialiser runs as a method of the instance (this) */
                struct pctx ctx = { .fi = fi, .up = P->ctx };
                P->ctx = &ctx;
                pr->b = parse_assign(P);
                P->ctx = ctx.up;
            }
            consume_semi(P);
        }
        nl_add(P, &l, pr);
    }
    lex(P);
    finish_list(n, &l);
    struct fninfo *fi = new_fninfo(P);                   /* source text for toString */
    fi->src = start;
    fi->src_len = (uint32_t)(P->prev_end - start);
    n->fi = fi;
    if (is_decl)
        P->lexflag = true;
    return n;
}

/* ---- expressions ---------------------------------------------------------------------------------------------- */

static struct node *parse_template(struct parser *P, struct tpl *tp, int line)
{
    struct node *n = nnew(P, N_TEMPLATE);
    n->line = line;
    struct nlist l = { 0 };
    for (int i = 0; i <= tp->n; i++) {
        struct node *s = nnew(P, N_STR);
        s->s = tp->strs[i];
        nl_add(P, &l, s);
        if (i < tp->n) {
            struct parser sub = *P;                      /* a sub-parser over the ${...} text, same function context */
            sub.src = sub.p = tp->ex[i];
            sub.end = tp->ex[i] + tp->exlen[i];
            sub.line = tp->exline[i];
            memset(&sub.t, 0, sizeof(sub.t));
            if (setjmp(sub.jb)) {
                memcpy(P->err, sub.err, sizeof(P->err));
                longjmp(P->jb, 1);
            }
            lex(&sub);
            struct node *e = parse_expr(&sub);
            if (sub.t.t != TK_EOF)
                perr(&sub, "unexpected token in template expression");
            P->lexflag = sub.lexflag;
            nl_add(P, &l, e);
        }
    }
    finish_list(n, &l);
    return n;
}

static struct node *parse_object_literal(struct parser *P)
{
    struct node *n = nnew(P, N_OBJECT);
    struct nlist l = { 0 };
    lex(P);                                             /* { */
    while (!is_p(P, "}")) {
        struct node *pr = nnew(P, N_PROP);
        pr->kind = PK_INIT;
        if (is_p(P, "...")) {
            lex(P);
            pr->kind = PK_SPREAD;
            pr->b = parse_assign(P);
            nl_add(P, &l, pr);
            if (!is_p(P, "}"))
                expect_p(P, ",");
            continue;
        }
        int kind = PK_INIT;
        if (is_id(P, "get") || is_id(P, "set")) {
            struct lexstate ls;
            save(P, &ls);
            bool getter = is_id(P, "get");
            lex(P);
            if (is_p(P, ",") || is_p(P, ":") || is_p(P, "(") || is_p(P, "}") || is_p(P, "="))
                restore(P, &ls);
            else
                kind = getter ? PK_GET : PK_SET;
        }
        bool m_async = false, m_gen = false;
        if (is_id(P, "async")) {
            struct lexstate ls;
            save(P, &ls);
            lex(P);
            bool plain = is_p(P, ",") || is_p(P, ":") || is_p(P, "(") || is_p(P, "}") || is_p(P, "=");
            if (plain)
                restore(P, &ls);
            else
                m_async = true;
        }
        if (is_p(P, "*")) {
            lex(P);
            m_gen = true;
        }
        bool ident = P->t.t == TK_ID && !m_async && !m_gen;
        jsstr *name = P->t.s;
        parse_prop_key(P, pr);
        if (kind == PK_GET || kind == PK_SET) {
            pr->kind = (uint8_t)kind;
            pr->b = parse_function(P, false, true, true, kind);
        } else if (is_p(P, "(")) {
            pr->kind = PK_METHOD;
            P->next_async = m_async;
            P->next_gen = m_gen;
            pr->b = parse_function(P, false, true, true, PK_METHOD);
        } else if (eat_p(P, ":")) {
            pr->b = parse_assign(P);
        } else if (ident && !(pr->flag & 1)) {          /* shorthand {a} or {a = 1} (pattern default) */
            struct node *id = nnew(P, N_IDENT);
            id->s = name;
            pr->b = id;
            if (is_p(P, "=")) {
                struct node *a = nnew(P, N_ASSIGN);
                lex(P);
                a->op = OP_ASSIGN;
                a->a = id;
                a->b = parse_assign(P);
                pr->b = a;
            }
        } else {
            perr(P, "expected ':' in object literal");
        }
        nl_add(P, &l, pr);
        if (!is_p(P, "}"))
            expect_p(P, ",");
    }
    lex(P);
    finish_list(n, &l);
    return n;
}

static struct node *parse_array_literal(struct parser *P)
{
    struct node *n = nnew(P, N_ARRAY);
    struct nlist l = { 0 };
    lex(P);
    bool old = P->no_in;
    P->no_in = false;
    while (!is_p(P, "]")) {
        if (is_p(P, ",")) {
            nl_add(P, &l, nnew(P, N_HOLE));
            lex(P);
            continue;
        }
        if (is_p(P, "...")) {
            struct node *s = nnew(P, N_SPREAD);
            lex(P);
            s->a = parse_assign(P);
            nl_add(P, &l, s);
        } else {
            nl_add(P, &l, parse_assign(P));
        }
        if (!is_p(P, "]"))
            expect_p(P, ",");
    }
    P->no_in = old;
    lex(P);
    finish_list(n, &l);
    return n;
}

static struct node *parse_primary(struct parser *P)
{
    struct tok *t = &P->t;
    switch (t->t) {
    case TK_NUM: {
        struct node *n = nnew(P, N_NUM);
        n->num = t->num;
        lex(P);
        return n;
    }
    case TK_STR: {
        struct node *n = nnew(P, N_STR);
        n->s = t->s;
        lex(P);
        return n;
    }
    case TK_TPL: {
        struct tpl *tp = t->tpl;
        int line = t->line;
        lex(P);
        return parse_template(P, tp, line);
    }
    case TK_ID: {
        const char *s = t->s->s;
        if (t->esc || !is_reserved(s)) {
            if (!strcmp(s, "async") && !t->esc) {
                struct lexstate ls;
                save(P, &ls);
                lex(P);
                bool fn = is_id(P, "function") && !P->t.nl;
                if (!fn) {
                    restore(P, &ls);
                } else {
                    P->next_async = true;
                    return parse_function(P, false, true, false, PK_METHOD);
                }
            }
            struct node *n = nnew(P, N_IDENT);
            n->s = t->s;
            if (t->s == P->J->k_arguments) {
                for (struct pctx *c = P->ctx; c; c = c->up)
                    if (!c->fi->is_arrow) {
                        c->fi->uses_args = 1;
                        break;
                    }
            }
            lex(P);
            return n;
        }
        if (!strcmp(s, "this")) {
            struct node *n = nnew(P, N_THIS);
            lex(P);
            return n;
        }
        if (!strcmp(s, "null") || !strcmp(s, "true") || !strcmp(s, "false")) {
            struct node *n = nnew(P, N_LIT);
            n->kind = s[0] == 'n' ? 1 : s[0] == 't' ? 2 : 3;
            lex(P);
            return n;
        }
        if (!strcmp(s, "function"))
            return parse_function(P, false, true, false, PK_METHOD);
        if (!strcmp(s, "class"))
            return parse_class(P, false);
        if (!strcmp(s, "super")) {
            struct node *n = nnew(P, N_SUPER);
            lex(P);
            return n;
        }
        perr(P, "unexpected keyword '%s'", s);
    }
    case TK_P:
        if (!strcmp(t->p, "(")) {
            lex(P);
            bool old = P->no_in;
            P->no_in = false;
            struct node *e = parse_expr(P);
            P->no_in = old;
            expect_p(P, ")");
            if (e->t == N_IDENT || e->t == N_MEMBER || e->t == N_FUNC)
                e->flag |= 0;                            /* (a) stays a plain expression */
            return e;
        }
        if (!strcmp(t->p, "["))
            return parse_array_literal(P);
        if (!strcmp(t->p, "{"))
            return parse_object_literal(P);
        perr(P, "unexpected '%s'", t->p);
    case TK_REGEX: {
        struct node *n = nnew(P, N_REGEX);
        n->s = t->s;
        struct node *fl = nnew(P, N_STR);
        fl->s = t->s2;
        n->a = fl;
        lex(P);
        return n;
    }
    case TK_EOF:
        perr(P, "unexpected end of script");
    }
    perr(P, "unexpected token");
}

static void parse_args(struct parser *P, struct node *call)
{
    struct nlist l = { 0 };
    expect_p(P, "(");
    bool old = P->no_in;
    P->no_in = false;
    while (!is_p(P, ")")) {
        if (is_p(P, "...")) {
            struct node *s = nnew(P, N_SPREAD);
            lex(P);
            s->a = parse_assign(P);
            nl_add(P, &l, s);
        } else {
            nl_add(P, &l, parse_assign(P));
        }
        if (!is_p(P, ")"))
            expect_p(P, ",");
    }
    P->no_in = old;
    lex(P);
    finish_list(call, &l);
}

static struct node *parse_member_name(struct parser *P)
{
    if (P->t.t != TK_ID)
        perr(P, "expected a property name after '.'");
    struct node *s = nnew(P, N_STR);
    s->s = P->t.s;
    lex(P);
    return s;
}

static struct node *parse_postfix_chain(struct parser *P, bool allow_call)
{
    struct node *e;
    if (is_id(P, "new")) {
        struct node *n = nnew(P, N_NEW);
        lex(P);
        if (is_p(P, ".")) {
            perr(P, "new.target is not supported");
        }
        n->a = parse_postfix_chain(P, false);
        if (is_p(P, "("))
            parse_args(P, n);
        e = n;
    } else {
        e = parse_primary(P);
    }
    bool opt_seen = false;
    for (;;) {
        if (is_p(P, ".")) {
            lex(P);
            struct node *m = nnew(P, N_MEMBER);
            m->a = e;
            m->b = parse_member_name(P);
            e = m;
        } else if (is_p(P, "?.")) {
            lex(P);
            opt_seen = true;
            if (is_p(P, "(")) {
                if (!allow_call)
                    perr(P, "unexpected '?.('");
                struct node *c = nnew(P, N_CALL);
                c->a = e;
                c->flag |= 2;
                parse_args(P, c);
                e = c;
            } else if (is_p(P, "[")) {
                lex(P);
                struct node *m = nnew(P, N_MEMBER);
                m->a = e;
                m->flag = 1 | 2;
                bool old = P->no_in;
                P->no_in = false;
                m->b = parse_expr(P);
                P->no_in = old;
                expect_p(P, "]");
                e = m;
            } else {
                struct node *m = nnew(P, N_MEMBER);
                m->a = e;
                m->flag = 2;
                m->b = parse_member_name(P);
                e = m;
            }
        } else if (is_p(P, "[")) {
            lex(P);
            struct node *m = nnew(P, N_MEMBER);
            m->a = e;
            m->flag = 1;
            bool old = P->no_in;
            P->no_in = false;
            m->b = parse_expr(P);
            P->no_in = old;
            expect_p(P, "]");
            e = m;
        } else if (is_p(P, "(") && allow_call) {
            struct node *c = nnew(P, N_CALL);
            c->a = e;
            parse_args(P, c);
            e = c;
        } else if (P->t.t == TK_TPL && allow_call) {
            perr(P, "tagged templates are not supported");
        } else {
            break;
        }
    }
    if (opt_seen)
        e->kind |= 8;                                   /* outermost node of an optional chain */
    return e;
}

static struct node *parse_unary(struct parser *P)
{
    struct tok *t = &P->t;
    if (t->t == TK_P) {
        int op = 0;
        if (!strcmp(t->p, "!")) op = OP_NOT;
        else if (!strcmp(t->p, "-")) op = OP_NEG;
        else if (!strcmp(t->p, "+")) op = OP_POS;
        else if (!strcmp(t->p, "~")) op = OP_BNOT;
        if (op) {
            struct node *n = nnew(P, N_UNARY);
            lex(P);
            n->op = (uint8_t)op;
            n->a = parse_unary(P);
            return n;
        }
        if (!strcmp(t->p, "++") || !strcmp(t->p, "--")) {
            struct node *n = nnew(P, N_UPDATE);
            n->op = t->p[0] == '+' ? OP_INC : OP_DEC;
            n->flag = 1;
            lex(P);
            n->a = parse_unary(P);
            return n;
        }
    } else if (t->t == TK_ID && !t->esc) {
        int op = 0;
        if (!strcmp(t->s->s, "await") && P->ctx->fi->is_async) {
            struct node *n = nnew(P, N_AWAIT);
            lex(P);
            n->a = parse_unary(P);
            return n;
        }
        if (!strcmp(t->s->s, "typeof")) op = OP_TYPEOF;
        else if (!strcmp(t->s->s, "void")) op = OP_VOID;
        else if (!strcmp(t->s->s, "delete")) op = OP_DELETE;
        if (op) {
            struct node *n = nnew(P, N_UNARY);
            lex(P);
            n->op = (uint8_t)op;
            n->a = parse_unary(P);
            return n;
        }
    }
    struct node *e = parse_postfix_chain(P, true);
    if (P->t.t == TK_P && !P->t.nl && (is_p(P, "++") || is_p(P, "--"))) {
        struct node *n = nnew(P, N_UPDATE);
        n->op = P->t.p[0] == '+' ? OP_INC : OP_DEC;
        n->a = e;
        lex(P);
        return n;
    }
    return e;
}

static int binop_of(struct parser *P, int *prec, bool *right)
{
    *right = false;
    if (P->t.t == TK_ID && !P->t.esc) {
        if (!strcmp(P->t.s->s, "instanceof")) { *prec = 8; return OP_INSTANCEOF; }
        if (!strcmp(P->t.s->s, "in") && !P->no_in) { *prec = 8; return OP_IN; }
        return 0;
    }
    if (P->t.t != TK_P)
        return 0;
    const char *p = P->t.p;
    struct { const char *s; int op, prec; } tab[] = {
        { "??", OP_NULLISH, 1 }, { "||", OP_OR, 2 }, { "&&", OP_AND, 3 }, { "|", OP_BOR, 4 }, { "^", OP_BXOR, 5 },
        { "&", OP_BAND, 6 }, { "==", OP_EQ, 7 }, { "!=", OP_NE, 7 }, { "===", OP_SEQ, 7 }, { "!==", OP_SNE, 7 },
        { "<", OP_LT, 8 }, { ">", OP_GT, 8 }, { "<=", OP_LE, 8 }, { ">=", OP_GE, 8 },
        { "<<", OP_SHL, 9 }, { ">>", OP_SHR, 9 }, { ">>>", OP_USHR, 9 }, { "+", OP_ADD, 10 }, { "-", OP_SUB, 10 },
        { "*", OP_MUL, 11 }, { "/", OP_DIV, 11 }, { "%", OP_MOD, 11 }, { "**", OP_EXP, 12 },
    };
    for (size_t i = 0; i < sizeof(tab) / sizeof(tab[0]); i++)
        if (!strcmp(tab[i].s, p)) {
            *prec = tab[i].prec;
            *right = tab[i].op == OP_EXP;
            return tab[i].op;
        }
    return 0;
}

static struct node *parse_binary(struct parser *P, int minprec)
{
    struct node *left = parse_unary(P);
    for (;;) {
        int prec, op;
        bool right;
        op = binop_of(P, &prec, &right);
        if (!op || prec < minprec)
            return left;
        struct node *n = nnew(P, op == OP_AND || op == OP_OR || op == OP_NULLISH ? N_LOGICAL : N_BINARY);
        lex(P);
        n->op = (uint8_t)op;
        n->a = left;
        n->b = parse_binary(P, right ? prec : prec + 1);
        left = n;
    }
}

static struct node *parse_cond(struct parser *P)
{
    struct node *c = parse_binary(P, 1);
    if (is_p(P, "?")) {
        struct node *n = nnew(P, N_COND);
        lex(P);
        n->a = c;
        bool old = P->no_in;
        P->no_in = false;
        n->b = parse_assign(P);
        P->no_in = old;
        expect_p(P, ":");
        n->c = parse_assign(P);
        return n;
    }
    return c;
}

static int assign_op(const char *p)
{
    struct { const char *s; int op; } tab[] = {
        { "=", OP_ASSIGN }, { "+=", OP_ADD }, { "-=", OP_SUB }, { "*=", OP_MUL }, { "/=", OP_DIV }, { "%=", OP_MOD },
        { "**=", OP_EXP }, { "<<=", OP_SHL }, { ">>=", OP_SHR }, { ">>>=", OP_USHR }, { "&=", OP_BAND },
        { "|=", OP_BOR }, { "^=", OP_BXOR }, { "&&=", OP_AND }, { "||=", OP_OR }, { "?\?=", OP_NULLISH },
    };
    for (size_t i = 0; i < sizeof(tab) / sizeof(tab[0]); i++)
        if (!strcmp(tab[i].s, p))
            return tab[i].op;
    return 0;
}

static struct node *parse_assign(struct parser *P)
{
    /* async arrow functions: async x => ..., async (a, b) => ... */
    if (is_id(P, "async")) {
        struct lexstate ls;
        save(P, &ls);
        lex(P);
        bool arrow = false;
        if (!P->t.nl) {
            if (P->t.t == TK_ID && !is_reserved(P->t.s->s)) {
                lex(P);
                arrow = is_p(P, "=>");
            } else if (is_p(P, "(")) {
                arrow = arrow_ahead(P);
            }
        }
        restore(P, &ls);
        if (arrow) {
            lex(P);                                        /* async */
            return parse_arrow(P, true);
        }
    }
    /* yield inside generators */
    if (is_id(P, "yield") && P->ctx->fi->is_gen) {
        struct node *y = nnew(P, N_YIELD);
        lex(P);
        if (eat_p(P, "*"))
            y->flag = 1;
        if (!P->t.nl && !is_p(P, ")") && !is_p(P, "]") && !is_p(P, "}") && !is_p(P, ",") && !is_p(P, ";") &&
            !is_p(P, ":") && P->t.t != TK_EOF && !is_id(P, "in") && !is_id(P, "of"))
            y->a = parse_assign(P);
        return y;
    }
    /* arrow functions */
    if (P->t.t == TK_ID && !P->t.esc && !is_reserved(P->t.s->s)) {
        struct lexstate ls;
        save(P, &ls);
        lex(P);
        bool arrow = is_p(P, "=>") && !P->t.nl;
        restore(P, &ls);
        if (arrow)
            return parse_arrow(P, false);
    } else if (is_p(P, "(") && arrow_ahead(P)) {
        return parse_arrow(P, false);
    }
    struct node *left = parse_cond(P);
    if (P->t.t == TK_P) {
        int op = assign_op(P->t.p);
        if (op) {
            if (left->t != N_IDENT && left->t != N_MEMBER && left->t != N_ARRAY && left->t != N_OBJECT)
                perr(P, "invalid assignment target");
            if (op != OP_ASSIGN && left->t != N_IDENT && left->t != N_MEMBER)
                perr(P, "invalid compound assignment target");
            struct node *n = nnew(P, N_ASSIGN);
            lex(P);
            n->op = (uint8_t)op;
            n->a = left;
            n->b = parse_assign(P);
            return n;
        }
    }
    return left;
}

static struct node *parse_expr(struct parser *P)
{
    struct node *e = parse_assign(P);
    if (is_p(P, ",")) {
        struct node *seq = nnew(P, N_SEQ);
        struct nlist l = { 0 };
        nl_add(P, &l, e);
        while (eat_p(P, ","))
            nl_add(P, &l, parse_assign(P));
        finish_list(seq, &l);
        return seq;
    }
    return e;
}

/* ---- statements -------------------------------------------------------------------------------------------- */

static struct node *parse_var_decl(struct parser *P, int kind, bool in_for)
{
    struct node *v = nnew(P, N_VAR);
    v->kind = (uint8_t)kind;
    struct nlist l = { 0 };
    do {
        struct node *d = nnew(P, N_DECL);
        d->a = parse_binding(P);
        if (kind == VK_VAR)
            collect_pattern_vars(P, d->a);
        else
            P->lexflag = true;
        if (eat_p(P, "="))
            d->b = parse_assign(P);
        else if (!in_for || !(is_id(P, "of") || is_id(P, "in"))) {
            if (kind == VK_CONST)
                perr(P, "missing initializer in const declaration");
            if (d->a->t != N_IDENT)
                perr(P, "missing initializer in destructuring declaration");
        }
        nl_add(P, &l, d);
    } while (eat_p(P, ","));
    finish_list(v, &l);
    return v;
}

static struct node *parse_block(struct parser *P)
{
    struct node *b = nnew(P, N_BLOCK);
    struct nlist l = { 0 };
    bool old = P->lexflag;
    P->lexflag = false;
    expect_p(P, "{");
    while (!is_p(P, "}")) {
        if (P->t.t == TK_EOF)
            perr(P, "unexpected end of script, expected '}'");
        nl_add(P, &l, parse_stmt(P));
    }
    lex(P);
    finish_list(b, &l);
    b->flag = P->lexflag;
    P->lexflag = old;
    return b;
}

static bool let_starts_decl(struct parser *P)
{
    struct lexstate ls;
    save(P, &ls);
    lex(P);
    bool r = (P->t.t == TK_ID && !(is_id(P, "in") || is_id(P, "instanceof") || is_id(P, "of"))) || is_p(P, "[") || is_p(P, "{");
    restore(P, &ls);
    return r;
}

static struct node *parse_for(struct parser *P)
{
    lex(P);                                             /* for */
    bool is_await = false;
    if (is_id(P, "await")) {
        if (!P->ctx->fi->is_async)
            perr(P, "for await is only valid in async functions");
        lex(P);
        is_await = true;
    }
    expect_p(P, "(");
    bool old_lex = P->lexflag;
    P->lexflag = false;
    struct node *init = NULL;
    bool old_in = P->no_in;
    int vkind = -1;
    if (is_p(P, ";")) {
        /* no init */
    } else if (is_id(P, "var") || is_id(P, "const") || (is_id(P, "let") && let_starts_decl(P))) {
        vkind = is_id(P, "var") ? VK_VAR : is_id(P, "let") ? VK_LET : VK_CONST;
        lex(P);
        P->no_in = true;
        init = parse_var_decl(P, vkind, true);
        P->no_in = old_in;
    } else {
        P->no_in = true;
        init = parse_expr(P);
        P->no_in = old_in;
    }
    struct node *n;
    if (init && (is_id(P, "of") || is_id(P, "in"))) {
        bool of = is_id(P, "of");
        n = nnew(P, of ? N_FOROF : N_FORIN);
        lex(P);
        n->a = init;                                    /* N_VAR (one declarator) or an assignment target expression */
        if (init->t == N_VAR && init->n != 1)
            perr(P, "invalid for-%s declaration", of ? "of" : "in");
        n->b = of ? parse_assign(P) : parse_expr(P);
        expect_p(P, ")");
        n->c = parse_stmt(P);
    } else {
        n = nnew(P, N_FOR);
        n->a = init;
        expect_p(P, ";");
        if (!is_p(P, ";"))
            n->b = parse_expr(P);
        expect_p(P, ";");
        if (!is_p(P, ")"))
            n->c = parse_expr(P);
        expect_p(P, ")");
        n->d = parse_stmt(P);
    }
    n->flag = (uint8_t)((vkind == VK_LET || vkind == VK_CONST) ? 1 : 0);
    if (is_await)
        n->kind = 1;
    P->lexflag = old_lex;
    return n;
}

static struct node *parse_stmt(struct parser *P)
{
    struct tok *t = &P->t;
    if (t->t == TK_P) {
        if (!strcmp(t->p, "{"))
            return parse_block(P);
        if (!strcmp(t->p, ";")) {
            struct node *n = nnew(P, N_EMPTY);
            lex(P);
            return n;
        }
    }
    if (t->t == TK_ID && !t->esc) {
        const char *s = t->s->s;
        if (!strcmp(s, "var") || !strcmp(s, "const") || (!strcmp(s, "let") && let_starts_decl(P))) {
            int kind = s[0] == 'v' ? VK_VAR : s[0] == 'c' ? VK_CONST : VK_LET;
            lex(P);
            struct node *v = parse_var_decl(P, kind, false);
            consume_semi(P);
            return v;
        }
        if (!strcmp(s, "function")) {
            struct node *f = parse_function(P, true, false, false, PK_METHOD);
            struct fninfo *cur = P->ctx->fi;
            struct node **nf = js_alloc(P->J, sizeof(*nf) * (size_t)(cur->nfuncs + 1));
            if (cur->nfuncs)
                memcpy(nf, cur->funcs, sizeof(*nf) * (size_t)cur->nfuncs);
            nf[cur->nfuncs++] = f;
            cur->funcs = nf;
            return nnew(P, N_EMPTY);
        }
        if (!strcmp(s, "async")) {
            struct lexstate ls;
            save(P, &ls);
            lex(P);
            bool decl = is_id(P, "function") && !P->t.nl;
            if (!decl)
                restore(P, &ls);
            else {
                P->next_async = true;
                struct node *f = parse_function(P, true, false, false, PK_METHOD);
                struct fninfo *cur = P->ctx->fi;
                struct node **nf = js_alloc(P->J, sizeof(*nf) * (size_t)(cur->nfuncs + 1));
                if (cur->nfuncs)
                    memcpy(nf, cur->funcs, sizeof(*nf) * (size_t)cur->nfuncs);
                nf[cur->nfuncs++] = f;
                cur->funcs = nf;
                return nnew(P, N_EMPTY);
            }
        }
        if (!strcmp(s, "class")) {
            struct node *c = parse_class(P, true);
            struct node *v = nnew(P, N_VAR);
            v->kind = VK_LET;
            struct node *d = nnew(P, N_DECL);
            struct node *id = nnew(P, N_IDENT);
            id->s = c->s;
            d->a = id;
            d->b = c;
            v->list = js_alloc(P->J, sizeof(struct node *));
            v->list[0] = d;
            v->n = 1;
            P->lexflag = true;
            return v;
        }
        if (!strcmp(s, "if")) {
            struct node *n = nnew(P, N_IF);
            lex(P);
            expect_p(P, "(");
            n->a = parse_expr(P);
            expect_p(P, ")");
            n->b = parse_stmt(P);
            if (eat_id(P, "else"))
                n->c = parse_stmt(P);
            return n;
        }
        if (!strcmp(s, "for"))
            return parse_for(P);
        if (!strcmp(s, "while")) {
            struct node *n = nnew(P, N_WHILE);
            lex(P);
            expect_p(P, "(");
            n->a = parse_expr(P);
            expect_p(P, ")");
            n->b = parse_stmt(P);
            return n;
        }
        if (!strcmp(s, "do")) {
            struct node *n = nnew(P, N_DOWHILE);
            lex(P);
            n->b = parse_stmt(P);
            if (!eat_id(P, "while"))
                perr(P, "expected 'while' after do body");
            expect_p(P, "(");
            n->a = parse_expr(P);
            expect_p(P, ")");
            eat_p(P, ";");
            return n;
        }
        if (!strcmp(s, "return")) {
            struct node *n = nnew(P, N_RETURN);
            lex(P);
            if (!is_p(P, ";") && !is_p(P, "}") && P->t.t != TK_EOF && !P->t.nl)
                n->a = parse_expr(P);
            consume_semi(P);
            return n;
        }
        if (!strcmp(s, "break") || !strcmp(s, "continue")) {
            struct node *n = nnew(P, s[0] == 'b' ? N_BREAK : N_CONTINUE);
            lex(P);
            if (P->t.t == TK_ID && !P->t.nl && !is_reserved(P->t.s->s)) {
                n->s = P->t.s;
                lex(P);
            }
            consume_semi(P);
            return n;
        }
        if (!strcmp(s, "throw")) {
            struct node *n = nnew(P, N_THROW);
            lex(P);
            if (P->t.nl)
                perr(P, "illegal newline after throw");
            n->a = parse_expr(P);
            consume_semi(P);
            return n;
        }
        if (!strcmp(s, "try")) {
            struct node *n = nnew(P, N_TRY);
            lex(P);
            n->a = parse_block(P);
            if (eat_id(P, "catch")) {
                if (eat_p(P, "(")) {
                    n->b = parse_binding(P);
                    expect_p(P, ")");
                }
                n->c = parse_block(P);
                n->flag = 1;
            }
            if (eat_id(P, "finally"))
                n->d = parse_block(P);
            if (!n->flag && !n->d)
                perr(P, "try needs catch or finally");
            return n;
        }
        if (!strcmp(s, "switch")) {
            struct node *n = nnew(P, N_SWITCH);
            lex(P);
            expect_p(P, "(");
            n->a = parse_expr(P);
            expect_p(P, ")");
            expect_p(P, "{");
            struct nlist cases = { 0 };
            bool old = P->lexflag;
            P->lexflag = false;
            while (!is_p(P, "}")) {
                struct node *c = nnew(P, N_CASE);
                if (eat_id(P, "case")) {
                    c->a = parse_expr(P);
                } else if (!eat_id(P, "default")) {
                    perr(P, "expected 'case' or 'default'");
                }
                expect_p(P, ":");
                struct nlist body = { 0 };
                while (!is_p(P, "}") && !is_id(P, "case") && !is_id(P, "default")) {
                    if (P->t.t == TK_EOF)
                        perr(P, "unexpected end of script in switch");
                    nl_add(P, &body, parse_stmt(P));
                }
                finish_list(c, &body);
                nl_add(P, &cases, c);
            }
            lex(P);
            finish_list(n, &cases);
            n->flag = P->lexflag;
            P->lexflag = old;
            return n;
        }
        if (!strcmp(s, "debugger")) {
            lex(P);
            consume_semi(P);
            return nnew(P, N_EMPTY);
        }
        if (!strcmp(s, "import") || !strcmp(s, "export"))
            perr(P, "modules are not supported");
        if (!strcmp(s, "with")) {
            struct node *n = nnew(P, N_WITH);
            lex(P);
            expect_p(P, "(");
            n->a = parse_expr(P);
            expect_p(P, ")");
            n->b = parse_stmt(P);
            return n;
        }
        if (!is_reserved(s)) {                          /* label? */
            struct lexstate ls;
            save(P, &ls);
            jsstr *name = t->s;
            lex(P);
            if (is_p(P, ":")) {
                struct node *n = nnew(P, N_LABEL);
                lex(P);
                n->s = name;
                n->a = parse_stmt(P);
                return n;
            }
            restore(P, &ls);
        }
    }
    struct node *n = nnew(P, N_EXPR);
    n->a = parse_expr(P);
    consume_semi(P);
    return n;
}

struct node *js_parse_program(js *J, const char *src, size_t len, char *err, size_t errn)
{
    struct parser P = { .J = J, .line = 1 };
    char *copy = js_alloc(J, len + 1);                  /* function sources point into this copy */
    memcpy(copy, src, len);
    P.src = P.p = copy;
    P.end = copy + len;
    if (len >= 2 && copy[0] == '#' && copy[1] == '!')   /* shebang */
        while (P.p < P.end && *P.p != '\n')
            P.p++;
    struct fninfo *fi = new_fninfo(&P);
    struct pctx ctx = { .fi = fi };
    P.ctx = &ctx;
    if (setjmp(P.jb)) {
        snprintf(err, errn, "%s", P.err);
        return NULL;
    }
    lex(&P);
    struct node *prog = nnew(&P, N_PROGRAM);
    struct nlist l = { 0 };
    while (P.t.t != TK_EOF)
        nl_add(&P, &l, parse_stmt(&P));
    finish_list(prog, &l);
    prog->fi = fi;
    return prog;
}
