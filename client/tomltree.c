#include "tomltree.h"

#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DEPTH 48
#define MAX_NODES 60000

struct P {
    const char *p;
    int nodes;
    bool fail;
};

struct sb {
    char *s;
    size_t n, cap;
};

static void sb_add(struct sb *b, const char *s, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2;
        b->s = realloc(b->s, b->cap);
    }
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = 0;
}

static void sb_cp(struct sb *b, unsigned cp)
{
    char o[4];
    size_t n;
    if (cp < 0x80) { o[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { o[0] = (char)(0xc0 | cp >> 6); o[1] = (char)(0x80 | (cp & 63)); n = 2; }
    else if (cp < 0x10000) { o[0] = (char)(0xe0 | cp >> 12); o[1] = (char)(0x80 | ((cp >> 6) & 63)); o[2] = (char)(0x80 | (cp & 63)); n = 3; }
    else { o[0] = (char)(0xf0 | cp >> 18); o[1] = (char)(0x80 | ((cp >> 12) & 63)); o[2] = (char)(0x80 | ((cp >> 6) & 63)); o[3] = (char)(0x80 | (cp & 63)); n = 4; }
    sb_add(b, o, n);
}

static struct json *mk(struct P *P, enum json_type t, const char *key)
{
    if (++P->nodes > MAX_NODES) {
        P->fail = true;
        return NULL;
    }
    struct json *j = calloc(1, sizeof(*j));
    if (!j) {
        P->fail = true;
        return NULL;
    }
    j->type = t;
    if (key)
        j->key = strdup(key);
    return j;
}

static void append(struct json *parent, struct json *c)
{
    struct json *t = parent->child;
    if (!t) {
        parent->child = c;
        return;
    }
    while (t->next)
        t = t->next;
    t->next = c;
}

static struct json *find(const struct json *o, const char *key)
{
    for (struct json *c = o->child; c; c = c->next)
        if (c->key && !strcmp(c->key, key))
            return c;
    return NULL;
}

/* Blanks and tabs. */
static void sp(struct P *P)
{
    while (*P->p == ' ' || *P->p == '\t')
        P->p++;
}

/* Blanks, line ends and comments (inside arrays and between statements). */
static void skip_all(struct P *P)
{
    for (;;) {
        while (*P->p == ' ' || *P->p == '\t' || *P->p == '\n' || *P->p == '\r')
            P->p++;
        if (*P->p == '#')
            while (*P->p && *P->p != '\n')
                P->p++;
        else
            return;
    }
}

/* The end of a statement: blanks, an optional comment, a line end (or the end of the text). */
static bool eol(struct P *P)
{
    sp(P);
    if (*P->p == '#')
        while (*P->p && *P->p != '\n')
            P->p++;
    if (*P->p == '\r')
        P->p++;
    if (*P->p == '\n') {
        P->p++;
        return true;
    }
    return *P->p == 0;
}

static bool bare_char(char c)
{
    return isalnum((unsigned char)c) || c == '_' || c == '-';
}

/* A string at P->p in one of its four forms; the text, malloc'd (NULL: fail). */
static char *string(struct P *P)
{
    const char *p = P->p;
    struct sb b = { 0 };
    char q = *p;
    bool multi = !strncmp(p, q == '"' ? "\"\"\"" : "'''", 3);
    p += multi ? 3 : 1;
    if (multi && *p == '\r') p++;
    if (multi && *p == '\n') p++;                                       /* the first line end is dropped */
    for (;;) {
        if (!*p || (!multi && (*p == '\n'))) { free(b.s); P->fail = true; return NULL; }
        if (multi ? !strncmp(p, q == '"' ? "\"\"\"" : "'''", 3) : *p == q) {
            if (multi) {                                                /* up to two more quotes belong to the text */
                p += 3;
                while (*p == q && b.n < 1 << 20) { sb_add(&b, &q, 1); p++; if (!(*p == q)) break; }
            } else {
                p++;
            }
            break;
        }
        if (q == '"' && *p == '\\') {
            p++;
            char e = *p++;
            if (e == 'n') sb_add(&b, "\n", 1);
            else if (e == 't') sb_add(&b, "\t", 1);
            else if (e == 'r') sb_add(&b, "\r", 1);
            else if (e == 'b') sb_add(&b, "\b", 1);
            else if (e == 'f') sb_add(&b, "\f", 1);
            else if (e == '"' || e == '\\') sb_add(&b, &e, 1);
            else if (e == 'u' || e == 'U') {
                int nd = e == 'u' ? 4 : 8;
                char hex[9] = { 0 };
                for (int i = 0; i < nd; i++) {
                    if (!isxdigit((unsigned char)p[i])) { free(b.s); P->fail = true; return NULL; }
                    hex[i] = p[i];
                }
                p += nd;
                sb_cp(&b, (unsigned)strtoul(hex, NULL, 16));
            } else if (multi && (e == '\n' || e == ' ' || e == '\t' || e == '\r')) {      /* a backslash at the end of a line: the white space goes */
                while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
            } else {
                free(b.s);
                P->fail = true;
                return NULL;
            }
            continue;
        }
        sb_add(&b, p, 1);
        p++;
    }
    P->p = p;
    return b.s ? b.s : strdup("");
}

/* One key part: bare or quoted. */
static char *key_part(struct P *P)
{
    sp(P);
    if (*P->p == '"' || *P->p == '\'')
        return string(P);
    const char *s = P->p;
    while (bare_char(*P->p)) P->p++;
    if (P->p == s) { P->fail = true; return NULL; }
    return strndup(s, (size_t)(P->p - s));
}

/* The member `key` of `o`: found, or made as `type` (an existing one of another kind is an error). */
static struct json *member(struct P *P, struct json *o, const char *key, enum json_type type)
{
    struct json *c = find(o, key);
    if (c) {
        if (c->type != type && !(type == JSON_OBJ && c->type == JSON_ARR && c->child && c->child->type == JSON_OBJ)) { P->fail = true; return NULL; }
        return c;
    }
    c = mk(P, type, key);
    if (c) append(o, c);
    return c;
}

static struct json *value(struct P *P, const char *key, int depth);

/* `a.b.c` from P->p, walking down from `base`: every part but the last is a table (the last element of an array of tables for an array); the last
 * part's name goes to *last (malloc'd) and the table that holds it is returned. */
static struct json *key_path(struct P *P, struct json *base, char **last)
{
    struct json *cur = base;
    char *k = key_part(P);
    if (!k) return NULL;
    for (;;) {
        sp(P);
        if (*P->p != '.') break;
        P->p++;
        struct json *next = member(P, cur, k, JSON_OBJ);
        free(k);
        if (!next) return NULL;
        if (next->type == JSON_ARR) {
            struct json *t = next->child;
            while (t && t->next) t = t->next;
            next = t;
            if (!next) { P->fail = true; return NULL; }
        }
        cur = next;
        k = key_part(P);
        if (!k) return NULL;
    }
    *last = k;
    return cur;
}

static struct json *inline_table(struct P *P, const char *key, int depth)
{
    struct json *o = mk(P, JSON_OBJ, key);
    if (!o) return NULL;
    P->p++;                                                             /* { */
    skip_all(P);
    if (*P->p == '}') { P->p++; return o; }
    for (;;) {
        skip_all(P);
        char *last = NULL;
        struct json *tbl = key_path(P, o, &last);
        if (!tbl) { json_free(o); return NULL; }
        sp(P);
        if (*P->p != '=') { free(last); json_free(o); P->fail = true; return NULL; }
        P->p++;
        sp(P);
        if (find(tbl, last)) { free(last); json_free(o); P->fail = true; return NULL; }
        struct json *v = value(P, last, depth + 1);
        free(last);
        if (!v) { json_free(o); return NULL; }
        append(tbl, v);
        skip_all(P);
        if (*P->p == ',') { P->p++; continue; }
        if (*P->p == '}') { P->p++; return o; }
        json_free(o);
        P->fail = true;
        return NULL;
    }
}

static struct json *array(struct P *P, const char *key, int depth)
{
    struct json *a = mk(P, JSON_ARR, key);
    if (!a) return NULL;
    P->p++;                                                             /* [ */
    for (;;) {
        skip_all(P);
        if (*P->p == ']') { P->p++; return a; }
        struct json *v = value(P, NULL, depth + 1);
        if (!v) { json_free(a); return NULL; }
        append(a, v);
        skip_all(P);
        if (*P->p == ',') { P->p++; continue; }
        if (*P->p == ']') { P->p++; return a; }
        json_free(a);
        P->fail = true;
        return NULL;
    }
}

static bool all_digits(const char *s, const char *e)
{
    if (s == e) return false;
    for (; s < e; s++)
        if (!isdigit((unsigned char)*s)) return false;
    return true;
}

static struct json *value(struct P *P, const char *key, int depth)
{
    if (depth > MAX_DEPTH) { P->fail = true; return NULL; }
    char c = *P->p;
    if (c == '"' || c == '\'') {
        char *s = string(P);
        if (!s) return NULL;
        struct json *j = mk(P, JSON_STR, key);
        if (!j) { free(s); return NULL; }
        j->str = s;
        return j;
    }
    if (c == '[') return array(P, key, depth);
    if (c == '{') return inline_table(P, key, depth);
    const char *s = P->p, *e = s;
    while (*e && *e != ',' && *e != ']' && *e != '}' && *e != '#' && *e != '\n' && *e != '\r') e++;
    while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;
    if (e == s) { P->fail = true; return NULL; }
    size_t n = (size_t)(e - s);
    char tok[96];
    if (n >= sizeof(tok)) { P->fail = true; return NULL; }
    memcpy(tok, s, n);
    tok[n] = 0;
    P->p = e;
    if (!strcmp(tok, "true") || !strcmp(tok, "false")) {
        struct json *j = mk(P, JSON_BOOL, key);
        if (j) j->b = tok[0] == 't';
        return j;
    }
    /* a date or a time: 1979-05-27, 07:32:00, 1979-05-27T07:32:00Z, 1979-05-27 07:32:00 */
    if ((n >= 10 && all_digits(tok, tok + 4) && tok[4] == '-') || (n >= 8 && isdigit((unsigned char)tok[0]) && isdigit((unsigned char)tok[1]) && tok[2] == ':')) {
        struct json *j = mk(P, JSON_STR, key);
        if (j) j->str = strdup(tok);
        return j;
    }
    char clean[96];
    size_t k = 0;
    for (size_t i = 0; i < n; i++)
        if (tok[i] != '_') clean[k++] = tok[i];
    clean[k] = 0;
    const char *d = clean + (clean[0] == '+' || clean[0] == '-');
    double v;
    bool ok = true;
    if (!strcmp(d, "inf")) v = clean[0] == '-' ? -INFINITY : INFINITY;
    else if (!strcmp(d, "nan")) v = NAN;
    else if (d[0] == '0' && (d[1] == 'x' || d[1] == 'o' || d[1] == 'b') && d == clean) {
        char *end;
        v = (double)strtoull(d + 2, &end, d[1] == 'x' ? 16 : d[1] == 'o' ? 8 : 2);
        ok = *end == 0 && d[2];
    } else {
        char *end;
        ok = isdigit((unsigned char)d[0]);
        v = strtod(clean, &end);
        ok = ok && *end == 0;
    }
    if (!ok) { P->fail = true; return NULL; }
    struct json *j = mk(P, JSON_NUM, key);
    if (j) { j->num = v; j->str = strdup(tok); }
    return j;
}

struct json *toml_parse(const char *text)
{
    struct P P = { text, 0, false };
    if (!strncmp(P.p, "\xef\xbb\xbf", 3)) P.p += 3;
    struct json *root = mk(&P, JSON_OBJ, NULL);
    if (!root) return NULL;
    struct json *cur = root;
    for (;;) {
        skip_all(&P);
        if (!*P.p) break;
        if (*P.p == '[') {                                              /* [table] and [[array of tables]] */
            bool arr = P.p[1] == '[';
            P.p += arr ? 2 : 1;
            char *last = NULL;
            struct json *parent = key_path(&P, root, &last);
            if (!parent) break;
            sp(&P);
            if (!(arr ? !strncmp(P.p, "]]", 2) : *P.p == ']')) { free(last); P.fail = true; break; }
            P.p += arr ? 2 : 1;
            if (!eol(&P)) { free(last); P.fail = true; break; }
            if (arr) {
                struct json *a = member(&P, parent, last, JSON_ARR);
                free(last);
                if (!a) break;
                struct json *t = mk(&P, JSON_OBJ, NULL);
                if (!t) break;
                append(a, t);
                cur = t;
            } else {
                struct json *t = find(parent, last);
                if (!t) t = member(&P, parent, last, JSON_OBJ);
                free(last);
                if (!t) break;
                if (t->type == JSON_ARR) {                              /* [a.b] under an array of tables: the last element */
                    struct json *l = t->child;
                    while (l && l->next) l = l->next;
                    t = l;
                }
                if (!t || t->type != JSON_OBJ) { P.fail = true; break; }
                cur = t;
            }
            continue;
        }
        char *last = NULL;
        struct json *tbl = key_path(&P, cur, &last);
        if (!tbl) break;
        sp(&P);
        if (*P.p != '=') { free(last); P.fail = true; break; }
        P.p++;
        sp(&P);
        if (find(tbl, last)) { free(last); P.fail = true; break; }       /* a key twice */
        struct json *v = value(&P, last, 0);
        free(last);
        if (!v) break;
        append(tbl, v);
        if (!eol(&P)) { P.fail = true; break; }
    }
    if (P.fail || !root->child) {
        json_free(root);
        return NULL;
    }
    return root;
}
