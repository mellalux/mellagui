#include "xmltree.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DEPTH 60
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

static struct json *mk(struct P *P, enum json_type t, const char *key, size_t klen)
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
    if (key) {
        j->key = malloc(klen + 1);
        if (j->key) {
            memcpy(j->key, key, klen);
            j->key[klen] = 0;
        }
    }
    return j;
}

static void utf8(struct sb *b, unsigned cp)
{
    char o[4];
    size_t n;
    if (cp < 0x80) { o[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { o[0] = (char)(0xc0 | cp >> 6); o[1] = (char)(0x80 | (cp & 63)); n = 2; }
    else if (cp < 0x10000) { o[0] = (char)(0xe0 | cp >> 12); o[1] = (char)(0x80 | ((cp >> 6) & 63)); o[2] = (char)(0x80 | (cp & 63)); n = 3; }
    else { o[0] = (char)(0xf0 | cp >> 18); o[1] = (char)(0x80 | ((cp >> 12) & 63)); o[2] = (char)(0x80 | ((cp >> 6) & 63)); o[3] = (char)(0x80 | (cp & 63)); n = 4; }
    sb_add(b, o, n);
}

/* Text with its entities decoded, appended to `b`. */
static void decode(struct sb *b, const char *s, size_t n)
{
    for (size_t i = 0; i < n;) {
        if (s[i] != '&') {
            size_t j = i;
            while (j < n && s[j] != '&') j++;
            sb_add(b, s + i, j - i);
            i = j;
            continue;
        }
        const char *semi = memchr(s + i, ';', n - i < 12 ? n - i : 12);
        if (!semi) { sb_add(b, "&", 1); i++; continue; }
        size_t el = (size_t)(semi - (s + i)) - 1;
        const char *e = s + i + 1;
        if (el == 2 && !memcmp(e, "lt", 2)) sb_add(b, "<", 1);
        else if (el == 2 && !memcmp(e, "gt", 2)) sb_add(b, ">", 1);
        else if (el == 3 && !memcmp(e, "amp", 3)) sb_add(b, "&", 1);
        else if (el == 4 && !memcmp(e, "quot", 4)) sb_add(b, "\"", 1);
        else if (el == 4 && !memcmp(e, "apos", 4)) sb_add(b, "'", 1);
        else if (el >= 2 && e[0] == '#') {
            char num[12];
            snprintf(num, sizeof(num), "%.*s", (int)(el - 1 < 10 ? el - 1 : 10), e + 1);
            unsigned long cp = num[0] == 'x' || num[0] == 'X' ? strtoul(num + 1, NULL, 16) : strtoul(num, NULL, 10);
            if (cp && cp < 0x110000) utf8(b, (unsigned)cp);
            else sb_add(b, s + i, el + 2);
        } else {
            sb_add(b, s + i, el + 2);                                   /* an entity we do not know stays as it is */
        }
        i += el + 2;
    }
}

static bool name_char(char c)
{
    return isalnum((unsigned char)c) || c == '_' || c == ':' || c == '-' || c == '.' || (unsigned char)c >= 0x80;
}

static void skip_ws(struct P *P)
{
    while (*P->p == ' ' || *P->p == '\t' || *P->p == '\n' || *P->p == '\r') P->p++;
}

/* Steps over <!-- --> <? ?> <!DOCTYPE ...> at P->p. False when there was none. */
static bool skip_misc(struct P *P)
{
    const char *p = P->p;
    if (!strncmp(p, "<!--", 4)) {
        const char *e = strstr(p + 4, "-->");
        if (!e) { P->fail = true; return false; }
        P->p = e + 3;
        return true;
    }
    if (!strncmp(p, "<?", 2)) {
        const char *e = strstr(p + 2, "?>");
        if (!e) { P->fail = true; return false; }
        P->p = e + 2;
        return true;
    }
    if (!strncmp(p, "<!DOCTYPE", 9)) {
        int depth = 0;
        for (p += 9; *p; p++) {
            if (*p == '[') depth++;
            else if (*p == ']') depth--;
            else if (*p == '>' && depth <= 0) break;
        }
        if (!*p) { P->fail = true; return false; }
        P->p = p + 1;
        return true;
    }
    return false;
}

/* Whitespace runs in text become one blank and the ends are trimmed. */
static char *collapse(const char *s, size_t n)
{
    char *o = malloc(n + 1);
    if (!o) return NULL;
    size_t k = 0;
    bool gap = false;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r') {
            gap = k > 0;
            continue;
        }
        if (gap) { o[k++] = ' '; gap = false; }
        o[k++] = s[i];
    }
    o[k] = 0;
    return o;
}

static struct json *element(struct P *P, int depth)
{
    if (depth > MAX_DEPTH) { P->fail = true; return NULL; }
    const char *p = P->p + 1;                                           /* after '<' */
    const char *ns = p;
    while (name_char(*p)) p++;
    if (p == ns) { P->fail = true; return NULL; }
    struct json *el = mk(P, JSON_OBJ, ns, (size_t)(p - ns)), *tail = NULL;
    if (!el) return NULL;
    char *tag = strndup(ns, (size_t)(p - ns));
    P->p = p;
#define APPEND(c) do { if (tail) tail->next = (c); else el->child = (c); tail = (c); } while (0)
#define FAIL() do { P->fail = true; json_free(el); free(tag); return NULL; } while (0)
    for (;;) {                                                          /* the attributes */
        skip_ws(P);
        if (*P->p == '>' || (P->p[0] == '/' && P->p[1] == '>'))
            break;
        const char *an = P->p;
        while (name_char(*P->p)) P->p++;
        if (P->p == an) FAIL();
        size_t al = (size_t)(P->p - an);
        skip_ws(P);
        if (*P->p != '=') FAIL();
        P->p++;
        skip_ws(P);
        char q = *P->p;
        if (q != '"' && q != '\'') FAIL();
        const char *vs = ++P->p;
        const char *ve = strchr(vs, q);
        if (!ve) FAIL();
        char key[256];
        snprintf(key, sizeof(key), "@%.*s", (int)(al < sizeof(key) - 2 ? al : sizeof(key) - 2), an);
        struct json *a = mk(P, JSON_STR, key, strlen(key));
        if (!a) FAIL();
        struct sb v = { 0 };
        decode(&v, vs, (size_t)(ve - vs));
        a->str = v.s ? v.s : strdup("");
        APPEND(a);
        P->p = ve + 1;
    }
    bool has_attr = el->child != NULL, has_child = false;
    if (P->p[0] == '/') {                                               /* <tag/> */
        P->p += 2;
        free(tag);
        return el;
    }
    P->p++;                                                             /* '>' */
    struct sb text = { 0 };
    for (;;) {
        const char *lt = strchr(P->p, '<');
        if (!lt) { free(text.s); FAIL(); }
        if (lt > P->p) {
            struct sb raw = { 0 };
            decode(&raw, P->p, (size_t)(lt - P->p));
            char *c = raw.s ? collapse(raw.s, raw.n) : NULL;
            free(raw.s);
            if (c && *c) {
                if (text.n) sb_add(&text, " ", 1);
                sb_add(&text, c, strlen(c));
            }
            free(c);
        }
        P->p = lt;
        if (lt[1] == '/') {                                             /* the end tag must be ours */
            size_t tl = strlen(tag);
            if (strncmp(lt + 2, tag, tl)) { free(text.s); FAIL(); }
            const char *e = lt + 2 + tl;
            while (*e == ' ' || *e == '\t' || *e == '\n' || *e == '\r') e++;
            if (*e != '>') { free(text.s); FAIL(); }
            P->p = e + 1;
            break;
        }
        if (!strncmp(lt, "<![CDATA[", 9)) {
            const char *e = strstr(lt + 9, "]]>");
            if (!e) { free(text.s); FAIL(); }
            if (text.n && e > lt + 9) sb_add(&text, " ", 1);
            sb_add(&text, lt + 9, (size_t)(e - lt - 9));
            P->p = e + 3;
            continue;
        }
        if (skip_misc(P))
            continue;
        if (P->fail) { free(text.s); FAIL(); }
        struct json *c = element(P, depth + 1);
        if (!c) { free(text.s); json_free(el); free(tag); return NULL; }
        has_child = true;
        APPEND(c);
    }
    if (text.n) {
        if (!has_attr && !has_child) {                                  /* only text: the element is that string */
            el->type = JSON_STR;
            el->str = text.s;
            free(tag);
            return el;
        }
        struct json *t = mk(P, JSON_STR, "#text", 5);
        if (!t) { free(text.s); FAIL(); }
        t->str = text.s;
        APPEND(t);
    }
    free(tag);
    return el;
#undef APPEND
#undef FAIL
}

struct json *xml_parse(const char *text)
{
    struct P P = { text, 0, false };
    if (!strncmp(P.p, "\xef\xbb\xbf", 3)) P.p += 3;                     /* a BOM */
    for (;;) {
        skip_ws(&P);
        if (!skip_misc(&P))
            break;
    }
    if (P.fail || *P.p != '<' || P.p[1] == '/' || P.p[1] == '!' || P.p[1] == '?')
        return NULL;
    struct json *root = element(&P, 0);
    if (!root || P.fail)
        return NULL;
    for (;;) {
        skip_ws(&P);
        if (!skip_misc(&P))
            break;
    }
    if (P.fail || *P.p) {                                               /* something after the root */
        json_free(root);
        return NULL;
    }
    return root;
}
