#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DEPTH 64

struct parser {
    const char *p;
    bool bad;
};

static void skip_ws(struct parser *ps)
{
    while (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r')
        ps->p++;
}

static int hex4(const char *s)
{
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0)
            return -1;
        v = v * 16 + d;
    }
    return v;
}

static size_t put_utf8(char *out, unsigned cp)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xc0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3f));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xe0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        out[2] = (char)(0x80 | (cp & 0x3f));
        return 3;
    }
    out[0] = (char)(0xf0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
    out[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
}

/* The string at ps->p (which is at the opening quote) as a new text; NULL and `bad` on an error. */
static char *parse_string(struct parser *ps)
{
    const char *s = ps->p + 1;
    const char *e = s;
    while (*e && *e != '"') {
        if (*e == '\\' && e[1])
            e++;
        e++;
    }
    if (*e != '"') {
        ps->bad = true;
        return NULL;
    }
    char *out = malloc((size_t)(e - s) + 1);                    /* an escape is never shorter than what it stands for */
    if (!out) {
        ps->bad = true;
        return NULL;
    }
    size_t n = 0;
    while (s < e) {
        if (*s != '\\') {
            out[n++] = *s++;
            continue;
        }
        s++;
        switch (*s) {
        case 'n': out[n++] = '\n'; s++; break;
        case 't': out[n++] = '\t'; s++; break;
        case 'r': out[n++] = '\r'; s++; break;
        case 'b': out[n++] = '\b'; s++; break;
        case 'f': out[n++] = '\f'; s++; break;
        case 'u': {
            int cp = e - s >= 5 ? hex4(s + 1) : -1;
            if (cp < 0) {
                free(out);
                ps->bad = true;
                return NULL;
            }
            s += 5;
            if (cp >= 0xd800 && cp < 0xdc00 && e - s >= 6 && s[0] == '\\' && s[1] == 'u') {     /* a surrogate pair */
                int lo = hex4(s + 2);
                if (lo >= 0xdc00 && lo < 0xe000) {
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                    s += 6;
                }
            }
            if (cp >= 0xd800 && cp < 0xe000)
                cp = 0xfffd;
            if (cp == 0)
                cp = ' ';
            n += put_utf8(out + n, (unsigned)cp);
            break;
        }
        default: out[n++] = *s++; break;                        /* \" \\ \/ */
        }
    }
    out[n] = 0;
    ps->p = e + 1;
    return out;
}

static struct json *parse_value(struct parser *ps, int depth)
{
    skip_ws(ps);
    struct json *j = calloc(1, sizeof(*j));
    if (!j || depth > MAX_DEPTH) {
        free(j);
        ps->bad = true;
        return NULL;
    }
    char c = *ps->p;
    if (c == '{' || c == '[') {
        bool obj = c == '{';
        char close = obj ? '}' : ']';
        j->type = obj ? JSON_OBJ : JSON_ARR;
        ps->p++;
        skip_ws(ps);
        struct json **tail = &j->child;
        if (*ps->p == close) {
            ps->p++;
            return j;
        }
        for (;;) {
            char *key = NULL;
            skip_ws(ps);
            if (obj) {
                if (*ps->p != '"' || !(key = parse_string(ps)))
                    break;
                skip_ws(ps);
                if (*ps->p != ':') {
                    free(key);
                    break;
                }
                ps->p++;
            }
            struct json *v = parse_value(ps, depth + 1);
            if (!v) {
                free(key);
                break;
            }
            v->key = key;
            *tail = v;
            tail = &v->next;
            skip_ws(ps);
            if (*ps->p == ',') {
                ps->p++;
                continue;
            }
            if (*ps->p == close) {
                ps->p++;
                return j;
            }
            break;
        }
        ps->bad = true;
        json_free(j);
        return NULL;
    }
    if (c == '"') {
        j->type = JSON_STR;
        j->str = parse_string(ps);
        if (!j->str) {
            free(j);
            return NULL;
        }
        return j;
    }
    if (!strncmp(ps->p, "true", 4) || !strncmp(ps->p, "false", 5)) {
        j->type = JSON_BOOL;
        j->b = c == 't';
        ps->p += j->b ? 4 : 5;
        return j;
    }
    if (!strncmp(ps->p, "null", 4)) {
        ps->p += 4;
        return j;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        char *end;
        j->type = JSON_NUM;
        j->num = strtod(ps->p, &end);
        if (end > ps->p && (j->str = strndup(ps->p, (size_t)(end - ps->p)))) {
            ps->p = end;
            return j;
        }
    }
    free(j);
    ps->bad = true;
    return NULL;
}

struct json *json_parse(const char *text)
{
    if (!text)
        return NULL;
    struct parser ps = { .p = text };
    struct json *j = parse_value(&ps, 0);
    if (!j)
        return NULL;
    skip_ws(&ps);
    if (*ps.p || ps.bad) {
        json_free(j);
        return NULL;
    }
    return j;
}

void json_free(struct json *j)
{
    while (j) {
        struct json *next = j->next;
        json_free(j->child);
        free(j->key);
        free(j->str);
        free(j);
        j = next;
    }
}

struct json *json_get(const struct json *obj, const char *key)
{
    if (!obj || obj->type != JSON_OBJ)
        return NULL;
    for (struct json *c = obj->child; c; c = c->next)
        if (c->key && !strcmp(c->key, key))
            return c;
    return NULL;
}

struct json *json_detach(struct json *obj, const char *key)
{
    if (!obj || obj->type != JSON_OBJ)
        return NULL;
    for (struct json **pp = &obj->child; *pp; pp = &(*pp)->next)
        if ((*pp)->key && !strcmp((*pp)->key, key)) {
            struct json *c = *pp;
            *pp = c->next;
            c->next = NULL;
            return c;
        }
    return NULL;
}

const char *json_str(const struct json *obj, const char *key, const char *def)
{
    const struct json *c = json_get(obj, key);
    return c && (c->type == JSON_STR || c->type == JSON_NUM) && c->str ? c->str : def;
}

double json_num(const struct json *obj, const char *key, double def)
{
    const struct json *c = json_get(obj, key);
    if (c && c->type == JSON_NUM)
        return c->num;
    if (c && c->type == JSON_STR && c->str && *c->str) {
        char *end;
        double v = strtod(c->str, &end);
        if (!*end)
            return v;
    }
    return def;
}

bool json_bool(const struct json *obj, const char *key, bool def)
{
    const struct json *c = json_get(obj, key);
    return c && c->type == JSON_BOOL ? c->b : def;
}

char *json_quote(const char *s, char *out, size_t n)
{
    size_t o = 0;
    if (n < 3) {
        if (n)
            out[0] = 0;
        return out;
    }
    out[o++] = '"';
    for (; *s && o + 8 < n; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c < 0x20) {
            o += (size_t)snprintf(out + o, 7, "\\u%04x", c);
        } else {
            out[o++] = (char)c;
        }
    }
    out[o++] = '"';
    out[o] = 0;
    return out;
}
