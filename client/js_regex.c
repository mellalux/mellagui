/*
 * Regular expressions for the JavaScript interpreter: a backtracking matcher over UTF-8 text (characters are code points,
 * indexes are byte offsets like everywhere else in this engine). Syntax: classes, escapes (\d \w \s \b \B \xHH \uHHHH
 * \u{...} \cX), groups (capturing, (?:), named (?<n>)), alternation, greedy/lazy quantifiers, {n,m}, backreferences (\1,
 * \k<n>), lookahead (?= (?!) and lookbehind (?<= (?<!). Flags g i m s u y. Not supported: \p{...}.
 */
#include <ctype.h>
#include <math.h>

#include "js_lib.h"

/* ---- program -------------------------------------------------------------------------------------------------- */

enum { RI_CHAR, RI_ANY, RI_CLASS, RI_SPLIT, RI_JMP, RI_SAVE, RI_BOL, RI_EOL, RI_WORDB, RI_NWORDB, RI_BACKREF, RI_LOOK,
       RI_MATCH, RI_LBEND, RI_LOOPSTART, RI_LOOPEND };

struct re_inst { uint8_t op, flag; int x, y; uint32_t c; };

struct re_range { uint32_t lo, hi; };
struct re_class { struct re_range *r; int n, cap; bool neg; };

#define RF_ICASE 1
#define RF_MULTI 2
#define RF_DOTALL 4
#define RF_GLOBAL 8
#define RF_STICKY 16
#define RF_UNICODE 32

struct regex {
    struct re_inst *code;
    int ncode, capcode;
    struct re_class *classes;
    int nclasses, capclasses;
    int ncaps;                      /* capture groups including group 0 */
    int nloops;
    char **names;                   /* names[i] = name of group i or NULL */
    int flags;
    char *source;
    char flagstr[8];
};

/* ---- AST ------------------------------------------------------------------------------------------------------ */

enum { A_EMPTY, A_CHAR, A_ANY, A_CLASS, A_CAT, A_ALT, A_GROUP, A_REPEAT, A_BOL, A_EOL, A_WORDB, A_NWORDB, A_BACKREF, A_LOOK };

struct anode {
    int t;
    uint32_t c;
    int cap;                        /* group index (-1 = non capturing) / class index / backref index */
    int min, max;                   /* repeat; max < 0 = infinite */
    bool greedy, neg, behind;
    struct anode **kids;
    int nk;
    char *name;                     /* \k<name> */
};

struct rparser {
    struct regex *re;
    const char *s, *p, *end;
    int ngroups;                    /* groups opened so far */
    int total_groups;               /* pre-counted */
    char err[120];
    bool failed;
};

static struct anode *an(int t)
{
    struct anode *n = calloc(1, sizeof(*n));
    if (!n)
        exit(1);
    n->t = t;
    n->cap = -1;
    n->greedy = true;
    return n;
}

static void an_free(struct anode *n)
{
    if (!n)
        return;
    for (int i = 0; i < n->nk; i++)
        an_free(n->kids[i]);
    free(n->kids);
    free(n->name);
    free(n);
}

static void an_add(struct anode *n, struct anode *k)
{
    n->kids = realloc(n->kids, sizeof(*n->kids) * (size_t)(n->nk + 1));
    n->kids[n->nk++] = k;
}

static void rfail(struct rparser *P, const char *m)
{
    if (!P->failed) {
        P->failed = true;
        snprintf(P->err, sizeof(P->err), "%s", m);
    }
}

static int new_class(struct regex *re)
{
    if (re->nclasses == re->capclasses) {
        re->capclasses = re->capclasses ? re->capclasses * 2 : 4;
        re->classes = realloc(re->classes, sizeof(*re->classes) * (size_t)re->capclasses);
    }
    struct re_class *c = &re->classes[re->nclasses];
    memset(c, 0, sizeof(*c));
    return re->nclasses++;
}

static void cls_add(struct re_class *c, uint32_t lo, uint32_t hi)
{
    if (c->n == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 8;
        c->r = realloc(c->r, sizeof(*c->r) * (size_t)c->cap);
    }
    c->r[c->n].lo = lo;
    c->r[c->n].hi = hi;
    c->n++;
}

static void cls_add_escape(struct re_class *c, char e)
{
    switch (e) {
    case 'd': cls_add(c, '0', '9'); break;
    case 'D': cls_add(c, 0, '0' - 1); cls_add(c, '9' + 1, 0x10ffff); break;
    case 'w': cls_add(c, '0', '9'); cls_add(c, 'A', 'Z'); cls_add(c, '_', '_'); cls_add(c, 'a', 'z'); break;
    case 'W': cls_add(c, 0, '0' - 1); cls_add(c, '9' + 1, 'A' - 1); cls_add(c, 'Z' + 1, '_' - 1); cls_add(c, '_' + 1, 'a' - 1);
              cls_add(c, 'z' + 1, 0x10ffff); break;
    case 's':
        cls_add(c, 9, 13); cls_add(c, ' ', ' '); cls_add(c, 0xa0, 0xa0); cls_add(c, 0x1680, 0x1680); cls_add(c, 0x2000, 0x200a);
        cls_add(c, 0x2028, 0x2029); cls_add(c, 0x202f, 0x202f); cls_add(c, 0x205f, 0x205f); cls_add(c, 0x3000, 0x3000);
        cls_add(c, 0xfeff, 0xfeff);
        break;
    case 'S':
        cls_add(c, 0, 8); cls_add(c, 14, 31); cls_add(c, 33, 0x9f); cls_add(c, 0xa1, 0x167f); cls_add(c, 0x1681, 0x1fff);
        cls_add(c, 0x200b, 0x2027); cls_add(c, 0x202a, 0x202e); cls_add(c, 0x2030, 0x205e); cls_add(c, 0x2060, 0x2fff);
        cls_add(c, 0x3001, 0xfefe); cls_add(c, 0xff00, 0x10ffff);
        break;
    }
}

static uint32_t utf8_dec(const char *s, const char *end, int *n)
{
    unsigned char c = (unsigned char)*s;
    if (c < 0x80) { *n = 1; return c; }
    int l = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
    if (s + l > end || l == 1) { *n = 1; return c; }
    uint32_t cp = c & (0xffu >> (l + 1));
    for (int i = 1; i < l; i++) {
        if (((unsigned char)s[i] & 0xc0) != 0x80) { *n = 1; return c; }
        cp = (cp << 6) | ((unsigned char)s[i] & 63);
    }
    *n = l;
    return cp;
}

static int hexd(int c) { return isdigit(c) ? c - '0' : isxdigit(c) ? tolower(c) - 'a' + 10 : -1; }

/* Parse an escape after the backslash that denotes a single character; returns false if it is not a character escape. */
static bool char_escape(struct rparser *P, uint32_t *out)
{
    char c = *P->p;
    switch (c) {
    case 't': *out = '\t'; P->p++; return true;
    case 'n': *out = '\n'; P->p++; return true;
    case 'r': *out = '\r'; P->p++; return true;
    case 'v': *out = '\v'; P->p++; return true;
    case 'f': *out = '\f'; P->p++; return true;
    case '0': if (P->p + 1 >= P->end || !isdigit((unsigned char)P->p[1])) { *out = 0; P->p++; return true; } return false;
    case 'x':
        if (P->p + 2 < P->end + 0 && P->p + 2 <= P->end - 0 && hexd((unsigned char)P->p[1]) >= 0 && P->p + 2 < P->end &&
            hexd((unsigned char)P->p[2]) >= 0) {
            *out = (uint32_t)(hexd((unsigned char)P->p[1]) * 16 + hexd((unsigned char)P->p[2]));
            P->p += 3;
            return true;
        }
        *out = 'x';
        P->p++;
        return true;
    case 'u': {
        if (P->p + 1 < P->end && P->p[1] == '{') {
            const char *q = P->p + 2;
            uint32_t v = 0;
            while (q < P->end && hexd((unsigned char)*q) >= 0) v = v * 16 + (uint32_t)hexd((unsigned char)*q++);
            if (q < P->end && *q == '}') { *out = v; P->p = q + 1; return true; }
        } else if (P->p + 4 < P->end + 0 && P->p + 4 <= P->end - 1 + 0) {
            uint32_t v = 0;
            bool ok = true;
            for (int i = 1; i <= 4; i++) {
                if (hexd((unsigned char)P->p[i]) < 0) { ok = false; break; }
                v = v * 16 + (uint32_t)hexd((unsigned char)P->p[i]);
            }
            if (ok) {
                P->p += 5;
                if (v >= 0xd800 && v < 0xdc00 && P->p + 5 < P->end && P->p[0] == '\\' && P->p[1] == 'u') {
                    uint32_t lo = 0;
                    bool ok2 = true;
                    for (int i = 2; i <= 5; i++) {
                        if (hexd((unsigned char)P->p[i]) < 0) { ok2 = false; break; }
                        lo = lo * 16 + (uint32_t)hexd((unsigned char)P->p[i]);
                    }
                    if (ok2 && lo >= 0xdc00 && lo < 0xe000) { v = 0x10000 + ((v - 0xd800) << 10) + (lo - 0xdc00); P->p += 6; }
                }
                *out = v;
                return true;
            }
        }
        *out = 'u';
        P->p++;
        return true;
    }
    case 'c':
        if (P->p + 1 < P->end && isalpha((unsigned char)P->p[1])) { *out = (uint32_t)(P->p[1] % 32); P->p += 2; return true; }
        *out = '\\';
        return true;
    default:
        return false;
    }
}

static struct anode *parse_alt(struct rparser *P);

static struct anode *parse_class(struct rparser *P)
{
    struct anode *n = an(A_CLASS);
    n->cap = new_class(P->re);
    int ci = n->cap;
    bool neg = false;
    if (P->p < P->end && *P->p == '^') {
        neg = true;
        P->p++;
    }
    bool first = true;
    while (P->p < P->end && (*P->p != ']' || false)) {
        first = false;
        uint32_t lo;
        bool is_set = false;
        if (*P->p == '\\') {
            P->p++;
            if (P->p >= P->end) { rfail(P, "\\ at end of pattern"); break; }
            if (strchr("dDwWsS", *P->p)) {
                cls_add_escape(&P->re->classes[ci], *P->p++);
                is_set = true;
                lo = 0;
            } else if (*P->p == 'b') {
                lo = 8;
                P->p++;
            } else if (*P->p == 'p' || *P->p == 'P') {
                rfail(P, "unicode property escapes are not supported");
                break;
            } else if (!char_escape(P, &lo)) {
                int l;
                lo = utf8_dec(P->p, P->end, &l);
                P->p += l;
            }
        } else {
            int l;
            lo = utf8_dec(P->p, P->end, &l);
            P->p += l;
        }
        if (is_set)
            continue;
        uint32_t hi = lo;
        if (P->p + 1 < P->end && *P->p == '-' && P->p[1] != ']') {
            const char *save = P->p;
            P->p++;
            uint32_t h2;
            bool ok = true;
            if (*P->p == '\\') {
                P->p++;
                if (P->p < P->end && strchr("dDwWsS", *P->p)) {
                    ok = false;
                } else if (!char_escape(P, &h2)) {
                    int l;
                    h2 = utf8_dec(P->p, P->end, &l);
                    P->p += l;
                }
            } else {
                int l;
                h2 = utf8_dec(P->p, P->end, &l);
                P->p += l;
            }
            if (ok) {
                if (h2 < lo) { rfail(P, "range out of order in character class"); break; }
                hi = h2;
            } else {
                P->p = save;                                     /* "a-\d": the dash is a literal */
            }
        }
        cls_add(&P->re->classes[ci], lo, hi);
    }
    (void)first;
    if (P->p >= P->end) {
        rfail(P, "unterminated character class");
        return n;
    }
    P->p++;                                                       /* ] */
    P->re->classes[ci].neg = neg;
    return n;
}

static bool parse_int(const char **p, const char *end, int *v)
{
    if (*p >= end || !isdigit((unsigned char)**p))
        return false;
    long x = 0;
    while (*p < end && isdigit((unsigned char)**p)) {
        x = x * 10 + (**p - '0');
        if (x > 100000) x = 100000;
        (*p)++;
    }
    *v = (int)x;
    return true;
}

static struct anode *parse_atom(struct rparser *P)
{
    char c = *P->p;
    if (c == '(') {
        P->p++;
        struct anode *g = an(A_GROUP);
        if (P->p + 1 < P->end && P->p[0] == '?') {
            char k = P->p[1];
            if (k == ':') { P->p += 2; }
            else if (k == '=' || k == '!') {
                g->t = A_LOOK;
                g->neg = k == '!';
                P->p += 2;
            } else if (k == '<' && P->p + 2 < P->end && (P->p[2] == '=' || P->p[2] == '!')) {
                g->t = A_LOOK;
                g->behind = true;
                g->neg = P->p[2] == '!';
                P->p += 3;
            } else if (k == '<') {
                const char *s = P->p + 2, *q = s;
                while (q < P->end && *q != '>') q++;
                if (q >= P->end) { rfail(P, "invalid group name"); return g; }
                g->cap = ++P->ngroups;
                P->re->names = realloc(P->re->names, sizeof(char *) * (size_t)(P->total_groups + 2));
                P->re->names[g->cap] = strndup(s, (size_t)(q - s));
                P->p = q + 1;
            } else {
                rfail(P, "invalid group");
                return g;
            }
        } else {
            g->cap = ++P->ngroups;
        }
        struct anode *body = parse_alt(P);
        if (P->p >= P->end || *P->p != ')') {
            rfail(P, "unterminated group");
            an_free(body);
            return g;
        }
        P->p++;
        an_add(g, body);
        return g;
    }
    if (c == '[') {
        P->p++;
        return parse_class(P);
    }
    if (c == '.') { P->p++; return an(A_ANY); }
    if (c == '^') { P->p++; return an(A_BOL); }
    if (c == '$') { P->p++; return an(A_EOL); }
    if (c == '\\') {
        P->p++;
        if (P->p >= P->end) { rfail(P, "\\ at end of pattern"); return an(A_EMPTY); }
        char e = *P->p;
        if (e == 'b') { P->p++; return an(A_WORDB); }
        if (e == 'B') { P->p++; return an(A_NWORDB); }
        if (strchr("dDwWsS", e)) {
            struct anode *n = an(A_CLASS);
            n->cap = new_class(P->re);
            cls_add_escape(&P->re->classes[n->cap], e);
            P->p++;
            return n;
        }
        if (e == 'p' || e == 'P') { rfail(P, "unicode property escapes are not supported"); return an(A_EMPTY); }
        if (e >= '1' && e <= '9') {
            const char *q = P->p;
            int v;
            parse_int(&q, P->end, &v);
            if (v <= P->total_groups) {
                P->p = q;
                struct anode *n = an(A_BACKREF);
                n->cap = v;
                return n;
            }
        }
        if (e == 'k' && P->p + 1 < P->end && P->p[1] == '<') {
            const char *s = P->p + 2, *q = s;
            while (q < P->end && *q != '>') q++;
            if (q < P->end) {
                struct anode *n = an(A_BACKREF);
                n->name = strndup(s, (size_t)(q - s));
                P->p = q + 1;
                return n;
            }
        }
        uint32_t cp;
        if (!char_escape(P, &cp)) {
            int l;
            cp = utf8_dec(P->p, P->end, &l);
            P->p += l;
        }
        struct anode *n = an(A_CHAR);
        n->c = cp;
        return n;
    }
    int l;
    uint32_t cp = utf8_dec(P->p, P->end, &l);
    P->p += l;
    struct anode *n = an(A_CHAR);
    n->c = cp;
    return n;
}

static struct anode *parse_term(struct rparser *P)
{
    struct anode *a = parse_atom(P);
    if (P->failed)
        return a;
    if (P->p >= P->end)
        return a;
    int min = -1, max = -1;
    char c = *P->p;
    if (c == '*') { min = 0; max = -1; P->p++; }
    else if (c == '+') { min = 1; max = -1; P->p++; }
    else if (c == '?') { min = 0; max = 1; P->p++; }
    else if (c == '{') {
        const char *q = P->p + 1;
        int lo, hi;
        if (parse_int(&q, P->end, &lo)) {
            if (q < P->end && *q == '}') { min = max = lo; P->p = q + 1; }
            else if (q < P->end && *q == ',') {
                q++;
                if (q < P->end && *q == '}') { min = lo; max = -1; P->p = q + 1; }
                else if (parse_int(&q, P->end, &hi) && q < P->end && *q == '}') { min = lo; max = hi; P->p = q + 1; }
            }
        }
    }
    if (min < 0)
        return a;
    if (a->t == A_BOL || a->t == A_EOL || a->t == A_WORDB || a->t == A_NWORDB || (a->t == A_LOOK && a->behind)) {
        rfail(P, "nothing to repeat");
        return a;
    }
    struct anode *r = an(A_REPEAT);
    r->min = min;
    r->max = max;
    if (max >= 0 && max < min) { rfail(P, "numbers out of order in {} quantifier"); return r; }
    if (P->p < P->end && *P->p == '?') {
        r->greedy = false;
        P->p++;
    }
    an_add(r, a);
    return r;
}

static struct anode *parse_cat(struct rparser *P)
{
    struct anode *cat = an(A_CAT);
    while (P->p < P->end && *P->p != '|' && *P->p != ')' && !P->failed) {
        if ((*P->p == '*' || *P->p == '+' || *P->p == '?')) { rfail(P, "nothing to repeat"); break; }
        an_add(cat, parse_term(P));
    }
    return cat;
}

static struct anode *parse_alt(struct rparser *P)
{
    struct anode *first = parse_cat(P);
    if (P->p >= P->end || *P->p != '|')
        return first;
    struct anode *alt = an(A_ALT);
    an_add(alt, first);
    while (P->p < P->end && *P->p == '|' && !P->failed) {
        P->p++;
        an_add(alt, parse_cat(P));
    }
    return alt;
}

/* ---- code generation ------------------------------------------------------------------------------------------- */

static int emit(struct regex *re, int op, int x, int y, uint32_t c)
{
    if (re->ncode == re->capcode) {
        re->capcode = re->capcode ? re->capcode * 2 : 64;
        re->code = realloc(re->code, sizeof(*re->code) * (size_t)re->capcode);
    }
    struct re_inst *i = &re->code[re->ncode];
    i->op = (uint8_t)op;
    i->flag = 0;
    i->x = x;
    i->y = y;
    i->c = c;
    return re->ncode++;
}

#define MAX_CODE 200000

static void gen(struct rparser *P, struct anode *n)
{
    struct regex *re = P->re;
    if (re->ncode > MAX_CODE) {
        rfail(P, "regular expression is too large");
        return;
    }
    switch (n->t) {
    case A_EMPTY: break;
    case A_CHAR: emit(re, RI_CHAR, 0, 0, n->c); break;
    case A_ANY: emit(re, RI_ANY, 0, 0, 0); break;
    case A_CLASS: emit(re, RI_CLASS, n->cap, 0, 0); break;
    case A_BOL: emit(re, RI_BOL, 0, 0, 0); break;
    case A_EOL: emit(re, RI_EOL, 0, 0, 0); break;
    case A_WORDB: emit(re, RI_WORDB, 0, 0, 0); break;
    case A_NWORDB: emit(re, RI_NWORDB, 0, 0, 0); break;
    case A_BACKREF: {
        int idx = n->cap;
        if (n->name) {
            idx = -1;
            for (int i = 1; i <= P->total_groups; i++)
                if (re->names && re->names[i] && !strcmp(re->names[i], n->name))
                    idx = i;
            if (idx < 0) { rfail(P, "invalid named reference"); return; }
        }
        emit(re, RI_BACKREF, idx, 0, 0);
        break;
    }
    case A_CAT:
        for (int i = 0; i < n->nk; i++)
            gen(P, n->kids[i]);
        break;
    case A_ALT: {
        int *jumps = malloc(sizeof(int) * (size_t)n->nk);
        for (int i = 0; i < n->nk; i++) {
            int split = -1;
            if (i + 1 < n->nk)
                split = emit(re, RI_SPLIT, 0, 0, 0);
            if (split >= 0)
                re->code[split].x = re->ncode;
            gen(P, n->kids[i]);
            jumps[i] = i + 1 < n->nk ? emit(re, RI_JMP, 0, 0, 0) : -1;
            if (split >= 0)
                re->code[split].y = re->ncode;
        }
        for (int i = 0; i < n->nk; i++)
            if (jumps[i] >= 0)
                re->code[jumps[i]].x = re->ncode;
        free(jumps);
        break;
    }
    case A_GROUP:
        if (n->cap >= 0)
            emit(re, RI_SAVE, n->cap * 2, 0, 0);
        gen(P, n->kids[0]);
        if (n->cap >= 0)
            emit(re, RI_SAVE, n->cap * 2 + 1, 0, 0);
        break;
    case A_LOOK: {
        int look = emit(re, RI_LOOK, 0, 0, 0);
        re->code[look].flag = (uint8_t)((n->neg ? 1 : 0) | (n->behind ? 2 : 0));
        gen(P, n->kids[0]);
        emit(re, n->behind ? RI_LBEND : RI_MATCH, 0, 0, 0);
        re->code[look].x = re->ncode;
        break;
    }
    case A_REPEAT: {
        struct anode *kid = n->kids[0];
        for (int i = 0; i < n->min; i++) {
            gen(P, kid);
            if (P->failed || re->ncode > MAX_CODE) { rfail(P, "regular expression is too large"); return; }
        }
        if (n->max < 0) {
            int loop = re->nloops++;
            int l = emit(re, RI_SPLIT, 0, 0, 0);
            int body = re->ncode;
            emit(re, RI_LOOPSTART, loop, 0, 0);
            gen(P, kid);
            int le = emit(re, RI_LOOPEND, loop, 0, 0);          /* no progress in an iteration: leave the loop */
            emit(re, RI_JMP, l, 0, 0);
            int exit = re->ncode;
            re->code[le].y = exit;
            if (n->greedy) { re->code[l].x = body; re->code[l].y = exit; }
            else { re->code[l].x = exit; re->code[l].y = body; }
        } else {
            int count = n->max - n->min;
            int *splits = malloc(sizeof(int) * (size_t)(count > 0 ? count : 1));
            for (int i = 0; i < count; i++) {
                splits[i] = emit(re, RI_SPLIT, 0, 0, 0);
                int body = re->ncode;
                gen(P, kid);
                if (n->greedy) re->code[splits[i]].x = body;
                else re->code[splits[i]].y = body;
                if (P->failed || re->ncode > MAX_CODE) { rfail(P, "regular expression is too large"); free(splits); return; }
            }
            for (int i = 0; i < count; i++) {
                if (n->greedy) re->code[splits[i]].y = re->ncode;
                else re->code[splits[i]].x = re->ncode;
            }
            free(splits);
        }
        break;
    }
    }
}

static int count_groups(const char *s, const char *end)
{
    int n = 0;
    bool in_class = false;
    for (const char *p = s; p < end; p++) {
        if (*p == '\\') { p++; continue; }
        if (in_class) { if (*p == ']') in_class = false; continue; }
        if (*p == '[') in_class = true;
        else if (*p == '(' && (p + 1 >= end || p[1] != '?' || (p + 2 < end && p[2] == '<' && p + 3 < end && p[3] != '=' && p[3] != '!')))
            n++;
    }
    return n;
}

static void regex_free(struct regex *re)
{
    if (!re)
        return;
    free(re->code);
    for (int i = 0; i < re->nclasses; i++)
        free(re->classes[i].r);
    free(re->classes);
    if (re->names)
        for (int i = 0; i <= re->ncaps; i++)
            free(re->names[i]);
    free(re->names);
    free(re->source);
    free(re);
}

static struct regex *regex_compile(const char *src, size_t len, const char *flags, char *err, size_t errn)
{
    struct regex *re = calloc(1, sizeof(*re));
    if (!re)
        exit(1);
    for (const char *f = flags; *f; f++) {
        int bit = *f == 'g' ? RF_GLOBAL : *f == 'i' ? RF_ICASE : *f == 'm' ? RF_MULTI : *f == 's' ? RF_DOTALL :
                  *f == 'u' ? RF_UNICODE : *f == 'y' ? RF_STICKY : *f == 'd' ? 0 : -1;
        if (bit < 0 || (bit && (re->flags & bit))) {
            snprintf(err, errn, "Invalid flags supplied to RegExp constructor '%s'", flags);
            free(re);
            return NULL;
        }
        re->flags |= bit;
    }
    struct rparser P = { .re = re, .s = src, .p = src, .end = src + len };
    P.total_groups = count_groups(src, src + len);
    re->names = calloc((size_t)P.total_groups + 2, sizeof(char *));
    re->ncaps = P.total_groups + 1;
    struct anode *root = parse_alt(&P);
    if (!P.failed && P.p < P.end)
        rfail(&P, *P.p == ')' ? "unmatched ')'" : "unexpected character");
    if (!P.failed) {
        emit(re, RI_SAVE, 0, 0, 0);
        gen(&P, root);
        emit(re, RI_SAVE, 1, 0, 0);
        emit(re, RI_MATCH, 0, 0, 0);
    }
    an_free(root);
    if (P.failed) {
        snprintf(err, errn, "Invalid regular expression: /%.*s/: %s", (int)(len > 60 ? 60 : len), src, P.err);
        regex_free(re);
        return NULL;
    }
    re->source = strndup(src, len);
    char *o = re->flagstr;
    if (re->flags & RF_GLOBAL) *o++ = 'g';
    if (re->flags & RF_ICASE) *o++ = 'i';
    if (re->flags & RF_MULTI) *o++ = 'm';
    if (re->flags & RF_DOTALL) *o++ = 's';
    if (re->flags & RF_UNICODE) *o++ = 'u';
    if (re->flags & RF_STICKY) *o++ = 'y';
    *o = 0;
    return re;
}

/* ---- matcher ---------------------------------------------------------------------------------------------------- */

struct bt { int kind; int pc; long pos; };      /* kind 0: branch (pc, pos); 1: restore cap[pc] = pos; 2: restore loop[pc] = pos */

struct matcher {
    const struct regex *re;
    const char *s;
    size_t len;
    long *caps;                     /* 2 * ncaps */
    long *loops;
    long lb_target;                 /* lookbehind: required end position */
    uint64_t steps;
    bool overflow;
};

static uint32_t fold(uint32_t c)
{
    if (c < 128) return (uint32_t)tolower((int)c);
    if ((c >= 0xc0 && c <= 0xde && c != 0xd7) || (c >= 0x410 && c <= 0x42f)) return c + 0x20;
    if (c >= 0x400 && c <= 0x40f) return c + 0x50;
    return c;
}

static bool is_word_at(const struct matcher *M, long pos)
{
    if (pos < 0 || (size_t)pos >= M->len)
        return false;
    unsigned char c = (unsigned char)M->s[pos];
    return isalnum(c) || c == '_';
}

static bool class_match(const struct re_class *c, uint32_t ch, bool icase)
{
    bool in = false;
    for (int i = 0; i < c->n && !in; i++)
        if (ch >= c->r[i].lo && ch <= c->r[i].hi)
            in = true;
    if (!in && icase) {
        uint32_t f = fold(ch), u = ch;
        if (ch < 128) u = (uint32_t)toupper((int)ch);
        else if (f != ch) u = f;
        else if ((ch >= 0xe0 && ch <= 0xfe && ch != 0xf7) || (ch >= 0x430 && ch <= 0x44f)) u = ch - 0x20;
        for (int i = 0; i < c->n && !in; i++)
            if ((f >= c->r[i].lo && f <= c->r[i].hi) || (u >= c->r[i].lo && u <= c->r[i].hi))
                in = true;
    }
    return in != c->neg;
}

static bool run(struct matcher *M, int pc, long pos, long *endpos);

static bool run(struct matcher *M, int pc, long pos, long *endpos)
{
    const struct regex *re = M->re;
    int ncap2 = re->ncaps * 2;
    struct bt *stack = NULL;
    int sp = 0, scap = 0;
#define PUSH(k, p_, v) do { if (sp == scap) { scap = scap ? scap * 2 : 64; stack = realloc(stack, sizeof(*stack) * (size_t)scap); } \
                            stack[sp].kind = (k); stack[sp].pc = (p_); stack[sp].pos = (v); sp++; } while (0)
    for (;;) {
        if (++M->steps > 20000000ULL) {
            M->overflow = true;
            free(stack);
            return false;
        }
        const struct re_inst *in = &re->code[pc];
        bool fail = false;
        switch (in->op) {
        case RI_CHAR: {
            if ((size_t)pos >= M->len) { fail = true; break; }
            int l;
            uint32_t ch = utf8_dec(M->s + pos, M->s + M->len, &l);
            if (ch == in->c || ((re->flags & RF_ICASE) && fold(ch) == fold(in->c))) { pos += l; pc++; }
            else fail = true;
            break;
        }
        case RI_ANY: {
            if ((size_t)pos >= M->len) { fail = true; break; }
            int l;
            uint32_t ch = utf8_dec(M->s + pos, M->s + M->len, &l);
            if (!(re->flags & RF_DOTALL) && (ch == '\n' || ch == '\r' || ch == 0x2028 || ch == 0x2029)) { fail = true; break; }
            pos += l;
            pc++;
            break;
        }
        case RI_CLASS: {
            if ((size_t)pos >= M->len) { fail = true; break; }
            int l;
            uint32_t ch = utf8_dec(M->s + pos, M->s + M->len, &l);
            if (class_match(&re->classes[in->x], ch, re->flags & RF_ICASE)) { pos += l; pc++; }
            else fail = true;
            break;
        }
        case RI_SPLIT:
            PUSH(0, in->y, pos);
            pc = in->x;
            break;
        case RI_JMP: pc = in->x; break;
        case RI_SAVE:
            PUSH(1, in->x, M->caps[in->x]);
            M->caps[in->x] = pos;
            pc++;
            break;
        case RI_BOL:
            if (pos == 0 || ((re->flags & RF_MULTI) && (M->s[pos - 1] == '\n' || M->s[pos - 1] == '\r'))) pc++;
            else fail = true;
            break;
        case RI_EOL:
            if ((size_t)pos == M->len || ((re->flags & RF_MULTI) && (M->s[pos] == '\n' || M->s[pos] == '\r'))) pc++;
            else fail = true;
            break;
        case RI_WORDB:
        case RI_NWORDB: {
            bool b = is_word_at(M, pos - 1) != is_word_at(M, pos);
            if (b == (in->op == RI_WORDB)) pc++;
            else fail = true;
            break;
        }
        case RI_BACKREF: {
            long s0 = M->caps[in->x * 2], e0 = M->caps[in->x * 2 + 1];
            if (s0 < 0 || e0 < 0) { pc++; break; }                  /* unmatched group matches empty */
            long l = e0 - s0;
            if ((size_t)(pos + l) > M->len) { fail = true; break; }
            bool same = true;
            for (long i = 0; i < l && same; i++) {
                unsigned char a = (unsigned char)M->s[s0 + i], b = (unsigned char)M->s[pos + i];
                same = a == b || ((re->flags & RF_ICASE) && tolower(a) == tolower(b));
            }
            if (same) { pos += l; pc++; }
            else fail = true;
            break;
        }
        case RI_LOOK: {
            bool neg = in->flag & 1, behind = in->flag & 2;
            long saved_target = M->lb_target;
            long *saved = malloc(sizeof(long) * (size_t)ncap2);
            memcpy(saved, M->caps, sizeof(long) * (size_t)ncap2);
            bool ok = false;
            long e;
            if (!behind) {
                ok = run(M, pc + 1, pos, &e);
            } else {
                for (long st = pos; st >= 0 && !ok && !M->overflow; st--) {
                    M->lb_target = pos;
                    ok = run(M, pc + 1, st, &e);
                }
            }
            M->lb_target = saved_target;
            if (M->overflow) { free(saved); free(stack); return false; }
            if (neg || !ok)
                memcpy(M->caps, saved, sizeof(long) * (size_t)ncap2);      /* captures of a failed/negated look are dropped */
            else
                for (int i = 0; i < ncap2; i++)
                    if (saved[i] != M->caps[i])
                        PUSH(1, i, saved[i]);
            free(saved);
            if (ok != neg) pc = in->x;
            else fail = true;
            break;
        }
        case RI_LBEND:
            if (pos == M->lb_target) { *endpos = pos; free(stack); return true; }
            fail = true;
            break;
        case RI_LOOPSTART:
            PUSH(2, in->x, M->loops[in->x]);
            M->loops[in->x] = pos;
            pc++;
            break;
        case RI_LOOPEND:
            if (M->loops[in->x] == pos) pc = in->y;                    /* empty iteration: stop looping */
            else pc++;
            break;
        case RI_MATCH:
            *endpos = pos;
            free(stack);
            return true;
        }
        if (fail) {
            for (;;) {
                if (sp == 0) {
                    free(stack);
                    return false;
                }
                struct bt *b = &stack[--sp];
                if (b->kind == 1) { M->caps[b->pc] = b->pos; continue; }
                if (b->kind == 2) { M->loops[b->pc] = b->pos; continue; }
                pc = b->pc;
                pos = b->pos;
                break;
            }
        }
    }
#undef PUSH
}

/* Try to match at exactly `start`. On success fills caps (2 * ncaps longs). 1 = match, 0 = no match, -1 = too complex. */
static int match_at(const struct regex *re, const char *s, size_t len, long start, long *caps)
{
    struct matcher M = { .re = re, .s = s, .len = len, .caps = caps, .lb_target = -1 };
    M.loops = malloc(sizeof(long) * (size_t)(re->nloops + 1));
    for (int i = 0; i < re->ncaps * 2; i++)
        caps[i] = -1;
    for (int i = 0; i <= re->nloops; i++)
        M.loops[i] = -1;
    long e;
    bool ok = run(&M, 0, start, &e);
    free(M.loops);
    if (M.overflow)
        return -1;
    return ok ? 1 : 0;
}

/* Search from `start` (forward). Sticky: only at start. */
static int regex_search(const struct regex *re, const char *s, size_t len, long start, bool sticky, long *caps)
{
    for (long i = start; (size_t)i <= len; i++) {
        if (i > start && !sticky && ((unsigned char)s[i - 1] >= 0x80) && ((unsigned char)s[i] & 0xc0) == 0x80 && (size_t)i < len)
            continue;                                   /* stay on character boundaries */
        int r = match_at(re, s, len, i, caps);
        if (r)
            return r;
        if (sticky)
            break;
    }
    return 0;
}

/* ---- JavaScript objects --------------------------------------------------------------------------------------- */

static struct regex *rx_of(jsval v)
{
    if (v.t == JS_OBJ && v.u.o->cls == JC_REGEXP)
        return v.u.o->host;
    return NULL;
}

void js_regex_free_host(jsobj *o)
{
    if (o->cls == JC_REGEXP)
        regex_free(o->host);
}

static jsval regexp_make(js *J, const char *src, size_t len, const char *flags, jsobj *into)
{
    char err[200];
    struct regex *re = regex_compile(src, len, flags, err, sizeof(err));
    if (!re)
        return js_throw_error(J, "SyntaxError", "%s", err);
    jsobj *o = into ? into : obj_new(J, JC_REGEXP, J->regexp_proto);
    o->cls = JC_REGEXP;
    o->host = re;
    obj_add_prop(J, o, js_intern(J, "lastIndex", 9), js_num(0), PF_WRITE);
    obj_add_prop(J, o, js_intern(J, "source", 6), js_strn(J, src, len), 0);
    obj_add_prop(J, o, js_intern(J, "flags", 5), js_cstr(J, re->flagstr), 0);
    obj_add_prop(J, o, js_intern(J, "global", 6), js_bool(re->flags & RF_GLOBAL), 0);
    obj_add_prop(J, o, js_intern(J, "ignoreCase", 10), js_bool(re->flags & RF_ICASE), 0);
    obj_add_prop(J, o, js_intern(J, "multiline", 9), js_bool(re->flags & RF_MULTI), 0);
    obj_add_prop(J, o, js_intern(J, "sticky", 6), js_bool(re->flags & RF_STICKY), 0);
    obj_add_prop(J, o, js_intern(J, "unicode", 7), js_bool(re->flags & RF_UNICODE), 0);
    obj_add_prop(J, o, js_intern(J, "dotAll", 6), js_bool(re->flags & RF_DOTALL), 0);
    return js_objv(o);
}

jsval js_regexp_literal(js *J, jsstr *pattern, jsstr *flags)
{
    return regexp_make(J, pattern->s, pattern->len, flags ? flags->s : "", NULL);
}

NATIVE(RegExp_ctor)
{
    jsval pat = ARG(0), fl = ARG(1);
    jsobj *into = (J->newtarget.t != JS_UNDEF && thisv.t == JS_OBJ) ? thisv.u.o : NULL;
    if (rx_of(pat)) {
        struct regex *src = rx_of(pat);
        if (fl.t == JS_UNDEF)
            return regexp_make(J, src->source, strlen(src->source), src->flagstr, into);
    }
    const char *ps = "";
    size_t pl = 0;
    if (rx_of(pat)) {
        ps = rx_of(pat)->source;
        pl = strlen(ps);
    } else if (pat.t != JS_UNDEF) {
        jsstr *s = to_jsstr(J, pat);
        THROW_IF(J);
        ps = s->s;
        pl = s->len;
    }
    const char *fs = "";
    if (fl.t != JS_UNDEF) {
        jsstr *f = to_jsstr(J, fl);
        THROW_IF(J);
        fs = f->s;
    }
    return regexp_make(J, ps, pl, fs, into);
}

static jsval build_match(js *J, const struct regex *re, jsstr *s, const long *caps)
{
    jsobj *a = js_array_new(J);
    for (int i = 0; i < re->ncaps; i++) {
        if (caps[i * 2] < 0 || caps[i * 2 + 1] < 0)
            js_array_push(J, a, JS_UNDEFINED);
        else
            js_array_push(J, a, js_strn(J, s->s + caps[i * 2], (size_t)(caps[i * 2 + 1] - caps[i * 2])));
    }
    obj_add_prop(J, a, js_intern(J, "index", 5), js_num((double)caps[0]), PF_ENUM | PF_WRITE);
    obj_add_prop(J, a, js_intern(J, "input", 5), js_strv(s), PF_ENUM | PF_WRITE);
    jsval groups = JS_UNDEFINED;
    bool any = false;
    for (int i = 1; i < re->ncaps; i++)
        if (re->names[i])
            any = true;
    if (any) {
        jsobj *g = js_object_new(J);
        for (int i = 1; i < re->ncaps; i++)
            if (re->names[i])
                set_prop(J, js_objv(g), js_intern(J, re->names[i], strlen(re->names[i])), a->items[i]);
        groups = js_objv(g);
    }
    obj_add_prop(J, a, js_intern(J, "groups", 6), groups, PF_ENUM | PF_WRITE);
    return js_objv(a);
}

static jsval too_complex(js *J) { return js_throw_error(J, "RangeError", "Regular expression too complex"); }

static jsval exec_impl(js *J, jsval rxv, jsstr *s)
{
    struct regex *re = rx_of(rxv);
    bool use_last = re->flags & (RF_GLOBAL | RF_STICKY);
    jsstr *lik = js_intern(J, "lastIndex", 9);
    double li = 0;
    if (use_last) {
        li = to_number(J, get_prop(J, rxv, lik));
        THROW_IF(J);
        if (!(li >= 0))
            li = 0;
    }
    if (li > s->len) {
        set_prop(J, rxv, lik, js_num(0));
        return JS_NULLV;
    }
    long *caps = malloc(sizeof(long) * (size_t)re->ncaps * 2);
    int r = regex_search(re, s->s, s->len, (long)li, re->flags & RF_STICKY, caps);
    if (r < 0) {
        free(caps);
        return too_complex(J);
    }
    if (!r) {
        if (use_last)
            set_prop(J, rxv, lik, js_num(0));
        free(caps);
        return JS_NULLV;
    }
    if (use_last)
        set_prop(J, rxv, lik, js_num((double)caps[1]));
    jsval m = build_match(J, re, s, caps);
    free(caps);
    return m;
}

NATIVE(RegExp_exec)
{
    if (!rx_of(thisv))
        return throw_type(J, "RegExp.prototype.exec called on an incompatible receiver");
    jsstr *s = to_jsstr(J, ARG(0));
    THROW_IF(J);
    return exec_impl(J, thisv, s);
}
NATIVE(RegExp_test)
{
    if (!rx_of(thisv))
        return throw_type(J, "RegExp.prototype.test called on an incompatible receiver");
    jsstr *s = to_jsstr(J, ARG(0));
    THROW_IF(J);
    jsval r = exec_impl(J, thisv, s);
    THROW_IF(J);
    return js_bool(r.t != JS_NULL);
}
NATIVE(RegExp_toString)
{
    struct regex *re = rx_of(thisv);
    if (!re)
        return throw_type(J, "RegExp.prototype.toString called on an incompatible receiver");
    struct sbuf sb = { 0 };
    sb_putc(&sb, '/');
    sb_addz(&sb, re->source[0] ? re->source : "(?:)");
    sb_putc(&sb, '/');
    sb_addz(&sb, re->flagstr);
    return sb_to_val(J, &sb);
}

/* ---- String methods taking regular expressions ------------------------------------------------------------------ */

bool js_is_regexp(jsval v) { return rx_of(v) != NULL; }

/* A pattern argument: a RegExp, or anything else converted to a regular expression source (match/search). */
static jsval as_regexp(js *J, jsval v)
{
    if (rx_of(v))
        return v;
    jsstr *s = v.t == JS_UNDEF ? J->k_empty : to_jsstr(J, v);
    THROW_IF(J);
    return regexp_make(J, s->s, s->len, "", NULL);
}

static size_t char_len_at(jsstr *s, long pos)
{
    if ((size_t)pos >= s->len)
        return 1;
    unsigned char c = (unsigned char)s->s[pos];
    size_t l = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
    return pos + l > s->len ? 1 : l;
}

jsval js_regex_match(js *J, jsstr *s, jsval pat)
{
    jsval rxv = as_regexp(J, pat);
    THROW_IF(J);
    struct regex *re = rx_of(rxv);
    if (!(re->flags & RF_GLOBAL))
        return exec_impl(J, rxv, s);
    jsobj *res = js_array_new(J);
    long *caps = malloc(sizeof(long) * (size_t)re->ncaps * 2);
    long pos = 0;
    while ((size_t)pos <= s->len) {
        int r = regex_search(re, s->s, s->len, pos, re->flags & RF_STICKY, caps);
        if (r < 0) { free(caps); return too_complex(J); }
        if (!r)
            break;
        js_array_push(J, res, js_strn(J, s->s + caps[0], (size_t)(caps[1] - caps[0])));
        pos = caps[1] == caps[0] ? caps[1] + (long)char_len_at(s, caps[1]) : caps[1];
    }
    free(caps);
    set_prop(J, rxv, js_intern(J, "lastIndex", 9), js_num(0));
    if (!res->len)
        return JS_NULLV;
    return js_objv(res);
}

jsval js_regex_match_all(js *J, jsstr *s, jsval pat)
{
    if (!rx_of(pat) || !(rx_of(pat)->flags & RF_GLOBAL))
        return throw_type(J, "String.prototype.matchAll called with a non-global RegExp argument");
    struct regex *re = rx_of(pat);
    jsobj *res = js_array_new(J);
    long *caps = malloc(sizeof(long) * (size_t)re->ncaps * 2);
    long pos = 0;
    while ((size_t)pos <= s->len) {
        int r = regex_search(re, s->s, s->len, pos, re->flags & RF_STICKY, caps);
        if (r < 0) { free(caps); return too_complex(J); }
        if (!r)
            break;
        js_array_push(J, res, build_match(J, re, s, caps));
        pos = caps[1] == caps[0] ? caps[1] + (long)char_len_at(s, caps[1]) : caps[1];
    }
    free(caps);
    return js_objv(res);
}

jsval js_regex_search(js *J, jsstr *s, jsval pat)
{
    jsval rxv = as_regexp(J, pat);
    THROW_IF(J);
    struct regex *re = rx_of(rxv);
    long *caps = malloc(sizeof(long) * (size_t)re->ncaps * 2);
    int r = regex_search(re, s->s, s->len, 0, re->flags & RF_STICKY, caps);
    double idx = r > 0 ? (double)caps[0] : -1;
    free(caps);
    if (r < 0)
        return too_complex(J);
    return js_num(idx);
}

/* $1, $&, $<name> ... in a replacement template */
static void expand_template(struct sbuf *sb, const char *t, size_t tl, jsstr *s, const struct regex *re, const long *caps)
{
    for (size_t i = 0; i < tl; i++) {
        if (t[i] != '$' || i + 1 >= tl) {
            sb_putc(sb, t[i]);
            continue;
        }
        char c = t[i + 1];
        if (c == '$') { sb_putc(sb, '$'); i++; }
        else if (c == '&') { sb_add(sb, s->s + caps[0], (size_t)(caps[1] - caps[0])); i++; }
        else if (c == '`') { sb_add(sb, s->s, (size_t)caps[0]); i++; }
        else if (c == '\'') { sb_add(sb, s->s + caps[1], s->len - (size_t)caps[1]); i++; }
        else if (isdigit((unsigned char)c)) {
            int n = c - '0';
            size_t used = 1;
            if (i + 2 < tl && isdigit((unsigned char)t[i + 2]) && n * 10 + (t[i + 2] - '0') < re->ncaps && n * 10 + (t[i + 2] - '0') > 0) {
                n = n * 10 + (t[i + 2] - '0');
                used = 2;
            }
            if (n >= 1 && n < re->ncaps) {
                if (caps[n * 2] >= 0 && caps[n * 2 + 1] >= 0)
                    sb_add(sb, s->s + caps[n * 2], (size_t)(caps[n * 2 + 1] - caps[n * 2]));
                i += used;
            } else {
                sb_putc(sb, '$');
            }
        } else if (c == '<') {
            const char *q = memchr(t + i + 2, '>', tl - i - 2);
            bool done = false;
            if (q) {
                for (int g = 1; g < re->ncaps && !done; g++)
                    if (re->names[g] && strlen(re->names[g]) == (size_t)(q - (t + i + 2)) && !strncmp(re->names[g], t + i + 2, strlen(re->names[g]))) {
                        if (caps[g * 2] >= 0)
                            sb_add(sb, s->s + caps[g * 2], (size_t)(caps[g * 2 + 1] - caps[g * 2]));
                        done = true;
                    }
                if (done) { i = (size_t)(q - t); continue; }
            }
            sb_putc(sb, '$');
        } else {
            sb_putc(sb, '$');
        }
    }
}

jsval js_regex_replace(js *J, jsstr *s, jsval rxv, jsval repl, bool all)
{
    struct regex *re = rx_of(rxv);
    if (all && !(re->flags & RF_GLOBAL))
        return throw_type(J, "replaceAll must be called with a global RegExp");
    bool global = re->flags & RF_GLOBAL;
    jsstr *rs = NULL;
    if (!js_is_function(repl)) {
        rs = to_jsstr(J, repl);
        THROW_IF(J);
    }
    struct sbuf sb = { 0 };
    long *caps = malloc(sizeof(long) * (size_t)re->ncaps * 2);
    long pos = global ? 0 : (re->flags & RF_STICKY ? 0 : 0), prev = 0;
    while ((size_t)pos <= s->len) {
        int r = regex_search(re, s->s, s->len, pos, re->flags & RF_STICKY, caps);
        if (r < 0) { free(caps); sb_free(&sb); return too_complex(J); }
        if (!r)
            break;
        sb_add(&sb, s->s + prev, (size_t)(caps[0] - prev));
        if (rs) {
            expand_template(&sb, rs->s, rs->len, s, re, caps);
        } else {
            int na = re->ncaps + 2;
            bool named = false;
            for (int g = 1; g < re->ncaps; g++) if (re->names[g]) named = true;
            jsval *args = malloc(sizeof(jsval) * (size_t)(na + 1));
            jsval m = build_match(J, re, s, caps);
            for (int g = 0; g < re->ncaps; g++)
                args[g] = m.u.o->items[g];
            args[re->ncaps] = js_num((double)caps[0]);
            args[re->ncaps + 1] = js_strv(s);
            if (named)
                args[na++] = js_get(J, m, "groups");
            jsval rv = call_function(J, repl.u.o, JS_UNDEFINED, na, args, JS_UNDEFINED);
            free(args);
            if (J->thrown) { free(caps); sb_free(&sb); return JS_UNDEFINED; }
            jsstr *rstr = to_jsstr(J, rv);
            if (!rstr) { free(caps); sb_free(&sb); return JS_UNDEFINED; }
            sb_add(&sb, rstr->s, rstr->len);
        }
        prev = caps[1];
        pos = caps[1] == caps[0] ? caps[1] + (long)char_len_at(s, caps[1]) : caps[1];
        if (!global)
            break;
    }
    if (global)
        set_prop(J, rxv, js_intern(J, "lastIndex", 9), js_num(0));
    sb_add(&sb, s->s + prev, s->len - (size_t)prev);
    free(caps);
    return sb_to_val(J, &sb);
}

jsval js_regex_split(js *J, jsstr *s, jsval rxv, double limit)
{
    struct regex *re = rx_of(rxv);
    jsobj *res = js_array_new(J);
    if (limit == 0)
        return js_objv(res);
    long *caps = malloc(sizeof(long) * (size_t)re->ncaps * 2);
    if (s->len == 0) {
        int r = match_at(re, s->s, 0, 0, caps);
        free(caps);
        if (r < 0) return too_complex(J);
        if (!r)
            js_array_push(J, res, js_strv(s));
        return js_objv(res);
    }
    long p = 0, q = 0;
    while ((size_t)q < s->len) {
        int r = match_at(re, s->s, s->len, q, caps);
        if (r < 0) { free(caps); return too_complex(J); }
        if (!r || caps[1] == p || (caps[1] == q && caps[1] == caps[0] && false)) {
            q += (long)char_len_at(s, q);
            continue;
        }
        if (caps[1] == caps[0] && caps[0] == p) {
            q += (long)char_len_at(s, q);
            continue;
        }
        js_array_push(J, res, js_strn(J, s->s + p, (size_t)(q - p)));
        if (res->len >= limit) { free(caps); return js_objv(res); }
        for (int g = 1; g < re->ncaps; g++) {
            js_array_push(J, res, caps[g * 2] < 0 ? JS_UNDEFINED : js_strn(J, s->s + caps[g * 2], (size_t)(caps[g * 2 + 1] - caps[g * 2])));
            if (res->len >= limit) { free(caps); return js_objv(res); }
        }
        p = caps[1];
        q = p == q && caps[1] == caps[0] ? q + (long)char_len_at(s, q) : p;
    }
    js_array_push(J, res, js_strn(J, s->s + p, s->len - (size_t)p));
    free(caps);
    return js_objv(res);
}

void js_init_regex(js *J)
{
    J->regexp_proto = obj_new(J, JC_OBJECT, J->object_proto);
    lib_ctor(J, "RegExp", RegExp_ctor, 2, J->regexp_proto);
    lib_def(J, J->regexp_proto, "exec", RegExp_exec, 1);
    lib_def(J, J->regexp_proto, "test", RegExp_test, 1);
    lib_def(J, J->regexp_proto, "toString", RegExp_toString, 0);
}
