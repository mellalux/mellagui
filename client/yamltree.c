#include "yamltree.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DEPTH 48
#define MAX_NODES 60000

struct yl {
    char *text;                         /* from the first character that is no indent, to the end of the line */
    int indent;
    bool sig;                           /* a line that means something: not blank, not only a comment */
};

struct P {
    struct yl *l;
    int n, cur, depth, nodes;
    bool fail;
};

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

/* A line's text without its comment (a # at the start or after a blank, outside quotes) and without blanks at the ends. */
static void content(const char *t, char *out, size_t cap)
{
    size_t n = 0;
    char q = 0;
    bool start = true;                                                  /* at the start of a value: a quote here opens a string */
    for (const char *p = t; *p && n + 1 < cap; p++) {
        if (q) {
            if (q == '"' && *p == '\\' && p[1]) {
                out[n++] = *p++;
                if (n + 1 >= cap) break;
            } else if (*p == q) {
                if (q == '\'' && p[1] == '\'') {
                    out[n++] = *p++;
                    if (n + 1 >= cap) break;
                } else {
                    q = 0;
                }
            }
            out[n++] = *p;
            continue;
        }
        if (*p == '#' && (p == t || p[-1] == ' ' || p[-1] == '\t'))
            break;
        if ((*p == '"' || *p == '\'') && start)
            q = *p;
        start = *p == ' ' || *p == ',' || *p == '[' || *p == '{' || (*p == ':' && (p[1] == ' ' || !p[1])) || (*p == '-' && (p[1] == ' ' || !p[1]));
        out[n++] = *p;
    }
    while (n && (out[n - 1] == ' ' || out[n - 1] == '\t' || out[n - 1] == '\r'))
        n--;
    out[n] = 0;
}

static const struct yl *peek(struct P *P)
{
    while (P->cur < P->n && !P->l[P->cur].sig)
        P->cur++;
    return P->cur < P->n ? &P->l[P->cur] : NULL;
}

static bool is_dash(const char *c)
{
    return c[0] == '-' && (c[1] == ' ' || !c[1]);
}

/* A scalar's text turned into its value: quotes taken off, `~` / null, true / false, numbers, else a string. */
static struct json *scalar(struct P *P, const char *key, const char *s)
{
    bool as_text = false;
    while (*s == '&' || *s == '!') {                                    /* an anchor or a tag in front of it (!!str: it stays a string) */
        as_text |= !strncmp(s, "!!str", 5);
        while (*s && *s != ' ') s++;
        while (*s == ' ') s++;
    }
    size_t len = strlen(s);
    if (as_text && !(len >= 2 && (s[0] == '"' || s[0] == '\''))) {
        struct json *j = mk(P, JSON_STR, key);
        if (j) j->str = strdup(s);
        return j;
    }
    if (len >= 2 && (s[0] == '"' || s[0] == '\'') && s[len - 1] == s[0]) {
        struct json *j = mk(P, JSON_STR, key);
        if (!j) return NULL;
        char *o = malloc(len + 1), *w = o;
        if (!o) { free(j); P->fail = true; return NULL; }
        for (size_t i = 1; i + 1 < len; i++) {
            if (s[0] == '\'' && s[i] == '\'' && i + 2 < len && s[i + 1] == '\'') {
                *w++ = '\'';
                i++;
            } else if (s[0] == '"' && s[i] == '\\' && i + 2 < len) {
                char e = s[++i];
                if (e == 'n') *w++ = '\n';
                else if (e == 't') *w++ = '\t';
                else if (e == 'u' && i + 4 < len) {
                    unsigned cp = (unsigned)strtoul((char[]){ s[i + 1], s[i + 2], s[i + 3], s[i + 4], 0 }, NULL, 16);
                    i += 4;
                    if (cp < 0x80) *w++ = (char)cp;
                    else if (cp < 0x800) { *w++ = (char)(0xc0 | cp >> 6); *w++ = (char)(0x80 | (cp & 63)); }
                    else { *w++ = (char)(0xe0 | cp >> 12); *w++ = (char)(0x80 | ((cp >> 6) & 63)); *w++ = (char)(0x80 | (cp & 63)); }
                } else *w++ = e;
            } else {
                *w++ = s[i];
            }
        }
        *w = 0;
        j->str = o;
        return j;
    }
    if (!*s || !strcmp(s, "~") || !strcasecmp(s, "null"))
        return mk(P, JSON_NULL, key);
    if (!strcasecmp(s, "true") || !strcasecmp(s, "false")) {
        struct json *j = mk(P, JSON_BOOL, key);
        if (j) j->b = tolower((unsigned char)s[0]) == 't';
        return j;
    }
    if (isdigit((unsigned char)s[0]) || ((s[0] == '-' || s[0] == '+' || s[0] == '.') && (isdigit((unsigned char)s[1]) || (s[1] == '.' && isdigit((unsigned char)s[2]))))) {
        char *end;
        double v = strtod(s, &end);
        if (!*end) {
            struct json *j = mk(P, JSON_NUM, key);
            if (j) { j->num = v; j->str = strdup(s); }
            return j;
        }
    }
    struct json *j = mk(P, JSON_STR, key);                              /* (an alias `*name` is just its text) */
    if (j) j->str = strdup(s);
    return j;
}

/* ---- flow collections: [a, b, {c: d}] ---- */

static void skipws(const char **p)
{
    while (**p == ' ' || **p == '\t' || **p == '\n')
        (*p)++;
}

static struct json *flow(struct P *P, const char **p, const char *key, int depth);

/* A plain or quoted scalar inside a flow collection: up to , ] } (and : for a key). */
static char *flow_text(const char **p, bool is_key)
{
    const char *s = *p;
    size_t cap = strlen(s) + 1, n = 0;
    char *o = malloc(cap);
    if (!o) return NULL;
    if (*s == '"' || *s == '\'') {
        char q = *s;
        o[n++] = *s++;
        while (*s) {
            if (q == '"' && *s == '\\' && s[1]) { o[n++] = *s++; o[n++] = *s++; continue; }
            if (*s == q) {
                if (q == '\'' && s[1] == '\'') { o[n++] = *s++; o[n++] = *s++; continue; }
                o[n++] = *s++;
                break;
            }
            o[n++] = *s++;
        }
    } else {
        while (*s && *s != ',' && *s != ']' && *s != '}' && !(is_key && *s == ':' && (s[1] == ' ' || s[1] == ',' || s[1] == '}' || !s[1])))
            o[n++] = *s++;
        while (n && (o[n - 1] == ' ' || o[n - 1] == '\t')) n--;
    }
    o[n] = 0;
    *p = s;
    return o;
}

static struct json *flow(struct P *P, const char **p, const char *key, int depth)
{
    if (depth > MAX_DEPTH) { P->fail = true; return NULL; }
    skipws(p);
    if (**p == '[' || **p == '{') {
        bool map = **p == '{';
        char close = map ? '}' : ']';
        struct json *j = mk(P, map ? JSON_OBJ : JSON_ARR, key), *tail = NULL;
        if (!j) return NULL;
        (*p)++;
        for (;;) {
            skipws(p);
            if (**p == close) { (*p)++; return j; }
            if (!**p) { json_free(j); P->fail = true; return NULL; }
            struct json *c;
            if (map) {
                char *k = flow_text(p, true);
                if (!k) { json_free(j); P->fail = true; return NULL; }
                struct json *kn = scalar(P, NULL, k);
                char *kstr = kn && kn->str ? strdup(kn->str) : strdup(k);
                json_free(kn);
                free(k);
                skipws(p);
                if (**p == ':') {
                    (*p)++;
                    c = flow(P, p, kstr, depth + 1);
                } else {
                    c = mk(P, JSON_NULL, kstr);
                }
                free(kstr);
            } else {
                c = flow(P, p, NULL, depth + 1);
            }
            if (!c) { json_free(j); P->fail = true; return NULL; }
            if (tail) tail->next = c; else j->child = c;
            tail = c;
            skipws(p);
            if (**p == ',') (*p)++;
            else if (**p != close) { json_free(j); P->fail = true; return NULL; }
        }
    }
    char *t = flow_text(p, false);
    if (!t) { P->fail = true; return NULL; }
    struct json *j = scalar(P, key, t);
    free(t);
    return j;
}

static int flow_balance(const char *s)
{
    int d = 0;
    char q = 0;
    for (; *s; s++) {
        if (q) {
            if (q == '"' && *s == '\\' && s[1]) s++;
            else if (*s == q) q = 0;
        } else if (*s == '"' || *s == '\'') {
            q = *s;
        } else if (*s == '[' || *s == '{') {
            d++;
        } else if (*s == ']' || *s == '}') {
            d--;
        }
    }
    return d;
}

/* ---- block structure ---- */

static struct json *node(struct P *P, int min_indent, const char *key);

/* Where a mapping line's key ends: sets *klen and returns the start of the value (after the colon and blanks), or NULL when the line is no `key: value`. */
static const char *split_key(const char *c, size_t *klen)
{
    const char *p = c;
    if (*c == '"' || *c == '\'') {
        char q = *c++;
        while (*c) {
            if (q == '"' && *c == '\\' && c[1]) { c += 2; continue; }
            if (*c == q) {
                if (q == '\'' && c[1] == '\'') { c += 2; continue; }
                break;
            }
            c++;
        }
        if (*c != q) return NULL;
        c++;
        if (*c != ':' || (c[1] && c[1] != ' ')) return NULL;
        *klen = (size_t)(c - p);
    } else {
        if (*c == '[' || *c == '{' || *c == '|' || *c == '>' || *c == '?' || *c == '%' || *c == '@' || *c == '`')
            return NULL;
        for (; *c; c++)
            if (*c == ':' && (c[1] == ' ' || !c[1]))
                break;
        if (*c != ':') return NULL;
        *klen = (size_t)(c - p);
        while (*klen && (p[*klen - 1] == ' ' || p[*klen - 1] == '\t')) (*klen)--;
    }
    c++;
    while (*c == ' ') c++;
    return c;
}

/* The value that starts at `rest` on the current line (already stepped over); the lines after it belong to it when they are indented more than `pind`. */
static struct json *value(struct P *P, const char *rest, int pind, const char *key)
{
    char hdr[4096];
    snprintf(hdr, sizeof(hdr), "%s", rest);
    char *r = hdr;
    while (*r == '&' || *r == '!') {                                    /* anchor / tag: drop it; the value may be on the next lines */
        while (*r && *r != ' ') r++;
        while (*r == ' ') r++;
    }
    if (!*r) {
        const struct yl *nx = peek(P);
        if (nx && nx->indent > pind) {
            char c[4096];
            content(nx->text, c, sizeof(c));
            (void)c;
            return node(P, nx->indent, key);
        }
        if (nx && nx->indent == pind) {
            char c[4096];
            content(nx->text, c, sizeof(c));
            if (is_dash(c))
                return node(P, nx->indent, key);
        }
        return mk(P, JSON_NULL, key);
    }
    if (*r == '|' || *r == '>') {                                       /* a block scalar: the lines that are indented more than the key */
        bool fold = *r == '>', keep = strchr(r, '+') != NULL, strip = strchr(r, '-') != NULL;
        size_t cap = 256, n = 0;
        char *o = malloc(cap);
        if (!o) { P->fail = true; return NULL; }
        int bi = -1, pending_blank = 0;
        for (; P->cur < P->n; P->cur++) {
            const struct yl *L = &P->l[P->cur];
            bool blank = !L->text[0] || strspn(L->text, " \t\r") == strlen(L->text);
            if (blank) { pending_blank++; continue; }
            if (L->indent <= pind) break;
            if (bi < 0) bi = L->indent;
            size_t add = strlen(L->text) + 8;
            if (n + add + (size_t)pending_blank + 2 > cap) { cap = (n + add + (size_t)pending_blank) * 2; o = realloc(o, cap); if (!o) { P->fail = true; return NULL; } }
            if (n) {
                if (!fold || pending_blank)
                    for (int k = 0; k < (fold ? pending_blank : pending_blank + 1); k++) o[n++] = '\n';
                else
                    o[n++] = ' ';
            }
            pending_blank = 0;
            int extra = L->indent > bi ? L->indent - bi : 0;
            for (int k = 0; k < extra; k++) o[n++] = ' ';
            size_t tl = strlen(L->text);
            while (tl && (L->text[tl - 1] == '\r')) tl--;
            memcpy(o + n, L->text, tl);
            n += tl;
        }
        (void)keep;
        (void)strip;
        o[n] = 0;
        struct json *j = mk(P, JSON_STR, key);
        if (!j) { free(o); return NULL; }
        j->str = o;
        return j;
    }
    if (*r == '[' || *r == '{') {                                       /* a flow collection, over as many lines as it needs */
        size_t cap = strlen(r) + 1;
        char *buf = malloc(cap);
        if (!buf) { P->fail = true; return NULL; }
        strcpy(buf, r);
        while (flow_balance(buf) > 0) {
            const struct yl *nx = peek(P);
            if (!nx || nx->indent <= pind) break;
            char c[4096];
            content(nx->text, c, sizeof(c));
            P->cur++;
            cap += strlen(c) + 2;
            buf = realloc(buf, cap);
            if (!buf) { P->fail = true; return NULL; }
            strcat(buf, " ");
            strcat(buf, c);
        }
        const char *p = buf;
        struct json *j = flow(P, &p, key, 0);
        skipws(&p);
        if (j && *p) { json_free(j); j = NULL; P->fail = true; }
        free(buf);
        return j;
    }
    if (*r == '"' || *r == '\'') {                                      /* a quoted scalar must end on its line */
        size_t len = strlen(r);
        if (len < 2 || r[len - 1] != *r) { P->fail = true; return NULL; }
        return scalar(P, key, r);
    }
    /* a plain scalar: it goes on over the lines that are indented more */
    char acc[8192];
    snprintf(acc, sizeof(acc), "%s", r);
    for (;;) {
        const struct yl *nx = peek(P);
        if (!nx || nx->indent <= pind) break;
        char c[4096];
        content(nx->text, c, sizeof(c));
        size_t al = strlen(acc);
        if (al + strlen(c) + 2 >= sizeof(acc)) break;
        acc[al] = ' ';
        strcpy(acc + al + 1, c);
        P->cur++;
    }
    return scalar(P, key, acc);
}

static struct json *seq(struct P *P, int ind, const char *key)
{
    struct json *j = mk(P, JSON_ARR, key), *tail = NULL;
    if (!j) return NULL;
    for (;;) {
        const struct yl *L = peek(P);
        if (!L || L->indent != ind) break;
        char c[4096];
        content(L->text, c, sizeof(c));
        if (!is_dash(c)) break;
        int li = P->cur;
        const char *rest = c + 1;
        int sp = 0;
        while (*rest == ' ') { rest++; sp++; }
        P->cur++;
        struct json *item;
        size_t kl;
        if (*rest && (is_dash(rest) || split_key(rest, &kl))) {         /* "- key: value" / "- - x": the rest is a block that starts here, further in */
            struct yl *vl = &P->l[li];
            size_t off = (size_t)(rest - c);
            vl->text += off;
            vl->indent += (int)off;
            P->cur = li;
            item = node(P, vl->indent, NULL);
        } else {
            item = value(P, rest, ind, NULL);
        }
        (void)sp;
        if (!item || P->fail) { json_free(j); P->fail = true; return NULL; }
        if (tail) tail->next = item; else j->child = item;
        tail = item;
    }
    return j;
}

static struct json *map(struct P *P, int ind, const char *key)
{
    struct json *j = mk(P, JSON_OBJ, key), *tail = NULL;
    if (!j) return NULL;
    for (;;) {
        const struct yl *L = peek(P);
        if (!L || L->indent != ind) {
            if (L && L->indent > ind) { json_free(j); P->fail = true; return NULL; }
            break;
        }
        char c[4096];
        content(L->text, c, sizeof(c));
        if (is_dash(c))
            break;
        size_t kl;
        const char *rest = split_key(c, &kl);
        if (!rest) { json_free(j); P->fail = true; return NULL; }
        char k[512];
        snprintf(k, sizeof(k), "%.*s", (int)(kl < sizeof(k) - 1 ? kl : sizeof(k) - 1), c);
        struct json *kn = scalar(P, NULL, k);
        char *kstr = kn && kn->str ? strdup(kn->str) : strdup(k);
        json_free(kn);
        P->cur++;
        struct json *v = value(P, rest, ind, kstr);
        free(kstr);
        if (!v || P->fail) { json_free(j); P->fail = true; return NULL; }
        if (tail) tail->next = v; else j->child = v;
        tail = v;
    }
    return j;
}

static struct json *node(struct P *P, int min_indent, const char *key)
{
    if (++P->depth > MAX_DEPTH) { P->fail = true; P->depth--; return NULL; }
    struct json *r = NULL;
    const struct yl *L = peek(P);
    if (L && L->indent >= min_indent) {
        char c[4096];
        content(L->text, c, sizeof(c));
        size_t kl;
        if (is_dash(c))
            r = seq(P, L->indent, key);
        else if (split_key(c, &kl))
            r = map(P, L->indent, key);
        else {
            P->cur++;
            r = value(P, c, L->indent - 1, key);
        }
    } else {
        r = mk(P, JSON_NULL, key);
    }
    P->depth--;
    return r;
}

struct json *yaml_parse(const char *text)
{
    size_t len = strlen(text);
    char *buf = malloc(len + 1);
    if (!buf) return NULL;
    memcpy(buf, text, len + 1);
    int cap = 64, n = 0;
    struct yl *lines = malloc((size_t)cap * sizeof(*lines));
    if (!lines) { free(buf); return NULL; }
    bool started = false, ok = true;
    for (char *p = buf; *p && ok;) {
        char *e = strchr(p, '\n');
        if (e) *e = 0;
        size_t ll = strlen(p);
        if (ll && p[ll - 1] == '\r') p[--ll] = 0;
        int ind = 0;
        while (p[ind] == ' ') ind++;
        if (p[ind] == '\t' && p[ind + 1] && p[ind + 1] != '\t') ok = false;       /* tabs as indentation: not YAML */
        bool marker = ind == 0 && ((!strncmp(p, "---", 3) && (!p[3] || p[3] == ' ')) || (!strncmp(p, "...", 3) && !p[3]));
        if (marker && started) break;                                             /* the second document / the end: only the first is read */
        if (n == cap) {
            cap *= 2;
            struct yl *t = realloc(lines, (size_t)cap * sizeof(*lines));
            if (!t) { ok = false; break; }
            lines = t;
        }
        char c[4096];
        content(p + ind, c, sizeof(c));
        bool sig = c[0] && !marker && !(ind == 0 && p[0] == '%');
        if (sig) started = true;
        lines[n++] = (struct yl){ p + ind, ind, sig };
        if (!e) break;
        p = e + 1;
    }
    struct json *res = NULL;
    if (ok) {
        struct P P = { lines, n, 0, 0, 0, false };
        const struct yl *first = peek(&P);
        if (first) {
            char c[4096];
            content(first->text, c, sizeof(c));
            size_t kl;
            if (is_dash(c) || split_key(c, &kl)) {
                res = node(&P, first->indent, NULL);
                if (res && (P.fail || peek(&P))) { json_free(res); res = NULL; }       /* something was left over: it did not parse as one document */
            }
        }
    }
    free(lines);
    free(buf);
    return res;
}
