#include "pdfread.h"

#include "../src/img.h"

#include <ctype.h>
#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PDF_MAX_FILE (96u << 20)
#define MAX_OBJS 3000000
#define STREAM_CAP (24u << 20)
#define MAX_DEPTH 24
#define MAX_FONTS 96

static void seterr(char *err, size_t n, const char *s)
{
    if (err && n)
        snprintf(err, n, "%s", s);
}

/* ---- a growing string ---- */
struct sbuf {
    char *s;
    size_t n, cap;
};

static void sb_add(struct sbuf *b, const char *p, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2;
        char *t = realloc(b->s, b->cap);
        if (!t)
            return;
        b->s = t;
    }
    memcpy(b->s + b->n, p, n);
    b->n += n;
    b->s[b->n] = 0;
}

static void sb_utf8(struct sbuf *b, unsigned cp)
{
    char t[4];
    size_t n;
    if (cp < 0x80) { t[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { t[0] = (char)(0xc0 | cp >> 6); t[1] = (char)(0x80 | (cp & 0x3f)); n = 2; }
    else if (cp < 0x10000) { t[0] = (char)(0xe0 | cp >> 12); t[1] = (char)(0x80 | ((cp >> 6) & 0x3f)); t[2] = (char)(0x80 | (cp & 0x3f)); n = 3; }
    else { t[0] = (char)(0xf0 | cp >> 18); t[1] = (char)(0x80 | ((cp >> 12) & 0x3f)); t[2] = (char)(0x80 | ((cp >> 6) & 0x3f)); t[3] = (char)(0x80 | (cp & 0x3f)); n = 4; }
    sb_add(b, t, n);
}

/* ---- PDF values ---- */
enum { T_NULL, T_BOOL, T_INT, T_REAL, T_STR, T_NAME, T_ARR, T_DICT, T_REF };

struct pv {
    int t;
    double num;                         /* a number, a bool (0/1), a reference's object number */
    int gen;
    char *s;                            /* a string's bytes, a name's text */
    size_t slen;
    struct pv *it;                      /* array items, or dictionary values */
    char **key;                         /* dictionary keys */
    int n;
};

static void pv_free(struct pv *v)
{
    if (!v)
        return;
    free(v->s);
    for (int i = 0; i < v->n; i++) {
        pv_free(&v->it[i]);
        if (v->key)
            free(v->key[i]);
    }
    free(v->it);
    free(v->key);
    memset(v, 0, sizeof(*v));
}

static const struct pv *dict_get(const struct pv *d, const char *k)
{
    if (!d || d->t != T_DICT)
        return NULL;
    for (int i = 0; i < d->n; i++)
        if (!strcmp(d->key[i], k))
            return &d->it[i];
    return NULL;
}

static bool is_ws(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == 0;
}

static bool is_delim(int c)
{
    return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{' || c == '}' || c == '/' || c == '%';
}

static void skip_ws(const uint8_t **p, const uint8_t *end)
{
    while (*p < end) {
        if (is_ws(**p))
            (*p)++;
        else if (**p == '%') {
            while (*p < end && **p != '\n' && **p != '\r')
                (*p)++;
        } else {
            break;
        }
    }
}

static int hexv(int c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static bool pv_parse(const uint8_t **pp, const uint8_t *end, struct pv *out, int depth);

static void container_add(struct pv *c, const char *key, struct pv *val)
{
    c->it = realloc(c->it, (size_t)(c->n + 1) * sizeof(struct pv));
    if (c->t == T_DICT) {
        c->key = realloc(c->key, (size_t)(c->n + 1) * sizeof(char *));
        c->key[c->n] = strdup(key ? key : "");
    }
    c->it[c->n++] = *val;
}

/* After an integer: "gen R" makes it a reference. */
static bool try_ref(const uint8_t **pp, const uint8_t *end, struct pv *v)
{
    const uint8_t *p = *pp;
    skip_ws(&p, end);
    const uint8_t *q = p;
    long gen = 0;
    int digits = 0;
    while (q < end && *q >= '0' && *q <= '9' && digits < 6) {
        gen = gen * 10 + (*q - '0');
        q++;
        digits++;
    }
    if (!digits)
        return false;
    skip_ws(&q, end);
    if (q < end && *q == 'R' && (q + 1 >= end || is_ws(q[1]) || is_delim(q[1]))) {
        v->t = T_REF;
        v->gen = (int)gen;
        *pp = q + 1;
        return true;
    }
    return false;
}

static bool pv_parse(const uint8_t **pp, const uint8_t *end, struct pv *out, int depth)
{
    memset(out, 0, sizeof(*out));
    if (depth > MAX_DEPTH)
        return false;
    skip_ws(pp, end);
    if (*pp >= end)
        return false;
    const uint8_t *p = *pp;
    int c = *p;
    if (c == '<' && p + 1 < end && p[1] == '<') {
        p += 2;
        out->t = T_DICT;
        while (true) {
            skip_ws(&p, end);
            if (p >= end)
                break;
            if (p + 1 < end && p[0] == '>' && p[1] == '>') {
                p += 2;
                break;
            }
            struct pv key = { 0 }, val = { 0 };
            if (*p != '/' || !pv_parse(&p, end, &key, depth + 1) || key.t != T_NAME) {
                pv_free(&key);
                *pp = p;
                return out->n > 0 || true;
            }
            if (!pv_parse(&p, end, &val, depth + 1)) {
                pv_free(&key);
                break;
            }
            if (out->n < 4000)
                container_add(out, key.s, &val);
            else
                pv_free(&val);
            pv_free(&key);
        }
        *pp = p;
        return true;
    }
    if (c == '[') {
        p++;
        out->t = T_ARR;
        while (true) {
            skip_ws(&p, end);
            if (p >= end)
                break;
            if (*p == ']') {
                p++;
                break;
            }
            struct pv val = { 0 };
            if (!pv_parse(&p, end, &val, depth + 1))
                break;
            if (out->n < 200000)
                container_add(out, NULL, &val);
            else
                pv_free(&val);
        }
        *pp = p;
        return true;
    }
    if (c == '(') {
        p++;
        struct sbuf b = { 0 };
        int nest = 1;
        while (p < end && nest > 0) {
            int ch = *p++;
            if (ch == '\\' && p < end) {
                int e = *p++;
                switch (e) {
                case 'n': ch = '\n'; break;
                case 'r': ch = '\r'; break;
                case 't': ch = '\t'; break;
                case 'b': ch = '\b'; break;
                case 'f': ch = '\f'; break;
                case '\r': if (p < end && *p == '\n') p++; continue;
                case '\n': continue;
                default:
                    if (e >= '0' && e <= '7') {
                        int v = e - '0';
                        for (int k = 0; k < 2 && p < end && *p >= '0' && *p <= '7'; k++)
                            v = v * 8 + (*p++ - '0');
                        ch = v & 255;
                    } else {
                        ch = e;
                    }
                }
            } else if (ch == '(') {
                nest++;
            } else if (ch == ')') {
                if (--nest == 0)
                    break;
            }
            char cc = (char)ch;
            sb_add(&b, &cc, 1);
        }
        out->t = T_STR;
        out->s = b.s ? b.s : strdup("");
        out->slen = b.n;
        *pp = p;
        return true;
    }
    if (c == '<') {
        p++;
        struct sbuf b = { 0 };
        int hi = -1;
        while (p < end && *p != '>') {
            int h = hexv(*p++);
            if (h < 0)
                continue;
            if (hi < 0) {
                hi = h;
            } else {
                char cc = (char)(hi << 4 | h);
                sb_add(&b, &cc, 1);
                hi = -1;
            }
        }
        if (hi >= 0) {
            char cc = (char)(hi << 4);
            sb_add(&b, &cc, 1);
        }
        if (p < end)
            p++;
        out->t = T_STR;
        out->s = b.s ? b.s : strdup("");
        out->slen = b.n;
        *pp = p;
        return true;
    }
    if (c == '/') {
        p++;
        struct sbuf b = { 0 };
        while (p < end && !is_ws(*p) && !is_delim(*p)) {
            int ch = *p++;
            if (ch == '#' && p + 1 < end && hexv(p[0]) >= 0 && hexv(p[1]) >= 0) {
                ch = hexv(p[0]) << 4 | hexv(p[1]);
                p += 2;
            }
            char cc = (char)ch;
            sb_add(&b, &cc, 1);
        }
        out->t = T_NAME;
        out->s = b.s ? b.s : strdup("");
        out->slen = b.n;
        *pp = p;
        return true;
    }
    if (c == '+' || c == '-' || c == '.' || (c >= '0' && c <= '9')) {
        const uint8_t *q = p;
        bool real = false;
        if (*q == '+' || *q == '-')
            q++;
        while (q < end && ((*q >= '0' && *q <= '9') || *q == '.' || *q == '-')) {          /* ("--5" and "5-3" happen in damaged files) */
            real |= *q == '.';
            q++;
        }
        char tmp[64];
        size_t l = (size_t)(q - p) < sizeof(tmp) - 1 ? (size_t)(q - p) : sizeof(tmp) - 1;
        memcpy(tmp, p, l);
        tmp[l] = 0;
        out->num = strtod(tmp, NULL);
        out->t = real ? T_REAL : T_INT;
        *pp = q;
        if (!real && out->num >= 0 && depth < MAX_DEPTH && try_ref(pp, end, out)) {
            out->num = out->num;                                         /* (the object number stays in num) */
        }
        return true;
    }
    /* a keyword: true false null, or an operator in a content stream */
    const uint8_t *q = p;
    while (q < end && !is_ws(*q) && !is_delim(*q))
        q++;
    if (q == p) {                                                       /* a stray delimiter */
        *pp = p + 1;
        out->t = T_NULL;
        return true;
    }
    size_t l = (size_t)(q - p);
    if (l == 4 && !memcmp(p, "true", 4)) { out->t = T_BOOL; out->num = 1; }
    else if (l == 5 && !memcmp(p, "false", 5)) { out->t = T_BOOL; out->num = 0; }
    else { out->t = T_NULL; out->s = malloc(l + 1); memcpy(out->s, p, l); out->s[l] = 0; out->slen = l; }       /* (null, or a keyword: its text is in s) */
    *pp = q;
    return true;
}

/* ---- the file and its objects ---- */
struct obj {
    size_t off;                         /* where the value starts in the file; or, when `in_stm`, in the decoded object stream */
    int gen;
    int stm;                            /* the number of the object stream it lies in, 0 for none */
    bool used;
};

struct pdf {
    uint8_t *d;
    size_t n;
    struct obj *o;
    int nobj;                           /* the table's size */
    struct { int num; uint8_t *data; size_t len; } stm[64];                 /* decoded object streams (kept) */
    int nstm;
    int root, info;
    bool encrypted;
};

static const struct obj *obj_find(const struct pdf *pdf, int num)
{
    return num > 0 && num < pdf->nobj && pdf->o[num].used ? &pdf->o[num] : NULL;
}

/* Parses the value of object `num` (not a stream's data) into v. */
static bool obj_value(struct pdf *pdf, int num, struct pv *v);

static const struct pv *resolve(struct pdf *pdf, const struct pv *v, struct pv *tmp, int depth)
{
    memset(tmp, 0, sizeof(*tmp));
    while (v && v->t == T_REF && depth < 8) {
        pv_free(tmp);
        if (!obj_value(pdf, (int)v->num, tmp))
            return NULL;
        v = tmp;
        depth++;
    }
    return v && v->t != T_REF ? v : NULL;
}

/* ---- stream filters ---- */
static uint8_t *flate(const uint8_t *in, size_t n, size_t *outn, size_t cap)
{
    if (n < 3)
        return NULL;
    size_t skip = (in[0] & 0x0f) == 8 && ((in[0] << 8 | in[1]) % 31) == 0 ? 2 : 0;
    size_t oc = n * 6 + 1024 < cap ? n * 6 + 1024 : cap;
    while (true) {
        uint8_t *out = malloc(oc + 1);
        if (!out)
            return NULL;
        long r = lm_inflate_prefix(in + skip, n - skip, out, oc);
        if (r < 0) {
            free(out);
            return NULL;
        }
        if ((size_t)r < oc || oc >= cap) {
            *outn = (size_t)r;
            return out;
        }
        free(out);
        oc = oc * 4 > cap ? cap : oc * 4;
    }
}

static uint8_t *ascii85(const uint8_t *in, size_t n, size_t *outn)
{
    uint8_t *out = malloc(n / 5 * 4 + 8);
    size_t w = 0;
    uint32_t acc = 0;
    int k = 0;
    if (!out)
        return NULL;
    for (size_t i = 0; i < n; i++) {
        int c = in[i];
        if (is_ws(c))
            continue;
        if (c == '~')
            break;
        if (c == 'z' && k == 0) {
            memset(out + w, 0, 4);
            w += 4;
            continue;
        }
        if (c < '!' || c > 'u')
            continue;
        acc = acc * 85 + (uint32_t)(c - '!');
        if (++k == 5) {
            for (int j = 3; j >= 0; j--)
                out[w++] = (uint8_t)(acc >> (8 * j));
            acc = 0;
            k = 0;
        }
    }
    if (k > 1) {
        for (int j = k; j < 5; j++)
            acc = acc * 85 + 84;
        for (int j = 3; j > 4 - k; j--)
            out[w++] = (uint8_t)(acc >> (8 * j));
    }
    *outn = w;
    return out;
}

static uint8_t *asciihex(const uint8_t *in, size_t n, size_t *outn)
{
    uint8_t *out = malloc(n / 2 + 2);
    size_t w = 0;
    int hi = -1;
    if (!out)
        return NULL;
    for (size_t i = 0; i < n && in[i] != '>'; i++) {
        int h = hexv(in[i]);
        if (h < 0)
            continue;
        if (hi < 0)
            hi = h;
        else {
            out[w++] = (uint8_t)(hi << 4 | h);
            hi = -1;
        }
    }
    if (hi >= 0)
        out[w++] = (uint8_t)(hi << 4);
    *outn = w;
    return out;
}

static uint8_t *runlength(const uint8_t *in, size_t n, size_t *outn)
{
    size_t cap = n * 8 + 16 < STREAM_CAP ? n * 8 + 16 : STREAM_CAP, w = 0;
    uint8_t *out = malloc(cap);
    if (!out)
        return NULL;
    for (size_t i = 0; i < n;) {
        int l = in[i++];
        if (l == 128)
            break;
        if (l < 128) {
            size_t c = (size_t)l + 1;
            if (c > n - i)
                c = n - i;
            if (w + c > cap)
                break;
            memcpy(out + w, in + i, c);
            w += c;
            i += c;
        } else if (i < n) {
            size_t c = 257 - (size_t)l;
            if (w + c > cap)
                break;
            memset(out + w, in[i++], c);
            w += c;
        }
    }
    *outn = w;
    return out;
}

static uint8_t *lzw(const uint8_t *in, size_t n, size_t *outn, int early)
{
    size_t cap = n * 12 + 1024 < STREAM_CAP ? n * 12 + 1024 : STREAM_CAP, w = 0;
    uint8_t *out = malloc(cap);
    uint16_t *prefix = malloc(4096 * sizeof(uint16_t));
    uint8_t *suffix = malloc(4096), *stack = malloc(4097);
    if (!out || !prefix || !suffix || !stack) {
        free(out); free(prefix); free(suffix); free(stack);
        return NULL;
    }
    int next = 258, bits = 9, old = -1;
    uint32_t acc = 0;
    int nb = 0;
    size_t ip = 0;
    while (true) {
        while (nb < bits && ip < n) {
            acc = acc << 8 | in[ip++];
            nb += 8;
        }
        if (nb < bits)
            break;
        int code = (int)((acc >> (nb - bits)) & ((1u << bits) - 1));
        nb -= bits;
        if (code == 256) {
            next = 258;
            bits = 9;
            old = -1;
            continue;
        }
        if (code == 257)
            break;
        int sp = 0, cur = code;
        uint8_t first;
        if (old < 0) {
            if (code > 255)
                break;
            stack[sp++] = (uint8_t)code;
            first = (uint8_t)code;
        } else {
            if (code > next || (code == next && next >= 4096))
                break;
            if (code == next) {
                cur = old;
                while (cur > 255 && sp < 4096) {
                    stack[sp++] = suffix[cur];
                    cur = prefix[cur];
                }
                first = (uint8_t)cur;
                /* the string of `old` followed by its first letter: written below */
                uint8_t tmp[4097];
                int tn = 0;
                tmp[tn++] = (uint8_t)cur;
                for (int k = sp - 1; k >= 0; k--)
                    tmp[tn++] = stack[k];
                tmp[tn++] = first;
                if (w + (size_t)tn > cap) {
                    break;
                }
                memcpy(out + w, tmp, (size_t)tn);
                w += (size_t)tn;
                if (next < 4096) {
                    prefix[next] = (uint16_t)old;
                    suffix[next] = first;
                    next++;
                }
                old = code;
                if (next + early >= (1 << bits) && bits < 12)
                    bits++;
                continue;
            }
            while (cur > 255 && sp < 4096) {
                stack[sp++] = suffix[cur];
                cur = prefix[cur];
            }
            stack[sp++] = (uint8_t)cur;
            first = (uint8_t)cur;
            if (next < 4096) {
                prefix[next] = (uint16_t)old;
                suffix[next] = first;
                next++;
            }
        }
        if (w + (size_t)sp > cap)
            break;
        while (sp > 0)
            out[w++] = stack[--sp];
        old = code;
        if (next + early >= (1 << bits) && bits < 12)
            bits++;
    }
    free(prefix);
    free(suffix);
    free(stack);
    *outn = w;
    return out;
}

/* The PNG (10..15) and TIFF (2) predictors of a Flate / LZW stream. */
static uint8_t *predict(uint8_t *in, size_t *n, int pred, int colors, int bpc, int columns)
{
    if (pred < 2 || colors < 1 || colors > 32 || bpc < 1 || bpc > 16 || columns < 1 || columns > (1 << 24))
        return in;
    size_t bpp = ((size_t)colors * bpc + 7) / 8, rowb = ((size_t)colors * bpc * columns + 7) / 8;
    if (pred == 2) {
        if (bpc == 8)
            for (size_t r = 0; r + rowb <= *n; r += rowb)
                for (size_t i = bpp; i < rowb; i++)
                    in[r + i] = (uint8_t)(in[r + i] + in[r + i - bpp]);
        return in;
    }
    size_t rows = *n / (rowb + 1);
    uint8_t *out = malloc(rows * rowb + 1);
    if (!out)
        return in;
    for (size_t r = 0; r < rows; r++) {
        const uint8_t *src = in + r * (rowb + 1) + 1;
        uint8_t *dst = out + r * rowb, *up = r ? dst - rowb : NULL;
        int ft = in[r * (rowb + 1)];
        for (size_t i = 0; i < rowb; i++) {
            int a = i >= bpp ? dst[i - bpp] : 0, b = up ? up[i] : 0, c = (up && i >= bpp) ? up[i - bpp] : 0, x = src[i];
            switch (ft) {
            case 1: x += a; break;
            case 2: x += b; break;
            case 3: x += (a + b) / 2; break;
            case 4: {
                int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
                x += (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                break;
            }
            default: break;
            }
            dst[i] = (uint8_t)x;
        }
    }
    free(in);
    *n = rows * rowb;
    return out;
}

/* The raw bytes of a stream object, unfiltered; NULL when it is no stream or uses a filter that is not read (images). */
static uint8_t *stream_data(struct pdf *pdf, int num, size_t *outn, struct pv *dict_out, size_t cap)
{
    const struct obj *o = obj_find(pdf, num);
    if (!o || o->stm)
        return NULL;
    const uint8_t *p = pdf->d + o->off, *end = pdf->d + pdf->n;
    struct pv dict = { 0 };
    if (!pv_parse(&p, end, &dict, 0) || dict.t != T_DICT) {
        pv_free(&dict);
        return NULL;
    }
    skip_ws(&p, end);
    if (end - p < 6 || memcmp(p, "stream", 6)) {
        pv_free(&dict);
        return NULL;
    }
    p += 6;
    if (p < end && *p == '\r')
        p++;
    if (p < end && *p == '\n')
        p++;
    const uint8_t *s = p, *e = NULL;
    const struct pv *len = dict_get(&dict, "Length");
    struct pv tmp = { 0 };
    const struct pv *lv = resolve(pdf, len, &tmp, 0);
    if (lv && (lv->t == T_INT || lv->t == T_REAL) && lv->num >= 0 && lv->num <= (double)(end - s)) {
        const uint8_t *cand = s + (size_t)lv->num, *q = cand;
        while (q < end && is_ws(*q))
            q++;
        if (end - q >= 9 && !memcmp(q, "endstream", 9))
            e = cand;
    }
    pv_free(&tmp);
    if (!e) {                                                           /* the length lies: look for endstream */
        const uint8_t *q = memmem(s, (size_t)(end - s), "endstream", 9);
        if (!q) {
            pv_free(&dict);
            return NULL;
        }
        e = q;
        if (e > s && e[-1] == '\n')
            e--;
        if (e > s && e[-1] == '\r')
            e--;
    }
    size_t n = (size_t)(e - s);
    uint8_t *data = malloc(n + 1);
    if (!data) {
        pv_free(&dict);
        return NULL;
    }
    memcpy(data, s, n);
    const struct pv *filt = dict_get(&dict, "Filter");
    if (!filt)
        filt = dict_get(&dict, "F");
    const struct pv *parms = dict_get(&dict, "DecodeParms");
    if (!parms)
        parms = dict_get(&dict, "DP");
    struct pv ft = { 0 }, pt = { 0 };
    const struct pv *fv = resolve(pdf, filt, &ft, 0), *pm = resolve(pdf, parms, &pt, 0);
    int nf = !fv ? 0 : fv->t == T_ARR ? fv->n : 1;
    bool ok = true;
    for (int i = 0; i < nf && ok; i++) {
        const struct pv *f = fv->t == T_ARR ? &fv->it[i] : fv;
        struct pv t1 = { 0 }, t2 = { 0 };
        const struct pv *fn = resolve(pdf, f, &t1, 0);
        const struct pv *dp = pm ? (pm->t == T_ARR ? (i < pm->n ? &pm->it[i] : NULL) : pm) : NULL;
        const struct pv *dpv = dp ? resolve(pdf, dp, &t2, 0) : NULL;
        const char *name = fn && fn->t == T_NAME ? fn->s : "";
        uint8_t *out = NULL;
        size_t on = 0;
        int pred = 1, colors = 1, bpc = 8, columns = 1, early = 1;
        if (dpv && dpv->t == T_DICT) {
            const struct pv *x;
            if ((x = dict_get(dpv, "Predictor"))) pred = (int)x->num;
            if ((x = dict_get(dpv, "Colors"))) colors = (int)x->num;
            if ((x = dict_get(dpv, "BitsPerComponent"))) bpc = (int)x->num;
            if ((x = dict_get(dpv, "Columns"))) columns = (int)x->num;
            if ((x = dict_get(dpv, "EarlyChange"))) early = (int)x->num;
        }
        if (!strcmp(name, "FlateDecode") || !strcmp(name, "Fl")) {
            out = flate(data, n, &on, cap);
            if (out && pred > 1)
                out = predict(out, &on, pred, colors, bpc, columns);
        } else if (!strcmp(name, "LZWDecode") || !strcmp(name, "LZW")) {
            out = lzw(data, n, &on, early);
            if (out && pred > 1)
                out = predict(out, &on, pred, colors, bpc, columns);
        } else if (!strcmp(name, "ASCII85Decode") || !strcmp(name, "A85")) {
            out = ascii85(data, n, &on);
        } else if (!strcmp(name, "ASCIIHexDecode") || !strcmp(name, "AHx")) {
            out = asciihex(data, n, &on);
        } else if (!strcmp(name, "RunLengthDecode") || !strcmp(name, "RL")) {
            out = runlength(data, n, &on);
        }
        if (dpv)
            pv_free(&t2);
        pv_free(&t1);
        free(data);
        data = out;
        n = on;
        ok = out != NULL;
    }
    pv_free(&ft);
    pv_free(&pt);
    if (!ok) {
        pv_free(&dict);
        free(data);
        return NULL;
    }
    if (dict_out)
        *dict_out = dict;
    else
        pv_free(&dict);
    *outn = n;
    return data;
}

/* ---- the object table ---- */
static bool obj_value(struct pdf *pdf, int num, struct pv *v)
{
    memset(v, 0, sizeof(*v));
    const struct obj *o = obj_find(pdf, num);
    if (!o)
        return false;
    const uint8_t *p, *end;
    if (o->stm) {
        int k;
        for (k = 0; k < pdf->nstm && pdf->stm[k].num != o->stm; k++)
            ;
        if (k == pdf->nstm || o->off >= pdf->stm[k].len)
            return false;
        p = pdf->stm[k].data + o->off;
        end = pdf->stm[k].data + pdf->stm[k].len;
    } else {
        p = pdf->d + o->off;
        end = pdf->d + pdf->n;
    }
    return pv_parse(&p, end, v, 0);
}

static void obj_set(struct pdf *pdf, int num, size_t off, int gen, int stm)
{
    if (num <= 0 || num >= MAX_OBJS)
        return;
    if (num >= pdf->nobj) {
        int nn = pdf->nobj ? pdf->nobj : 1024;
        while (nn <= num)
            nn *= 2;
        struct obj *t = realloc(pdf->o, (size_t)nn * sizeof(struct obj));
        if (!t)
            return;
        memset(t + pdf->nobj, 0, (size_t)(nn - pdf->nobj) * sizeof(struct obj));
        pdf->o = t;
        pdf->nobj = nn;
    }
    pdf->o[num] = (struct obj){ off, gen, stm, true };
}

/* The objects: "N G obj" markers found in file order; after each object the scan goes on behind it (behind its stream), so numbers inside binary data are not taken. */
static void scan_objects(struct pdf *pdf, int *objstms, int *nobjstm, int max_stm)
{
    const uint8_t *d = pdf->d, *end = pdf->d + pdf->n;
    size_t pos = 0;
    while (pos < pdf->n) {
        const uint8_t *q = memmem(d + pos, pdf->n - pos, "obj", 3);
        if (!q)
            break;
        pos = (size_t)(q - d) + 3;
        if (q == d || (q + 3 < end && !is_ws(q[3]) && !is_delim(q[3])))
            continue;
        const uint8_t *b = q;
        if (!is_ws(b[-1]))
            continue;
        while (b > d && is_ws(b[-1]))
            b--;
        const uint8_t *ge = b;
        while (b > d && b[-1] >= '0' && b[-1] <= '9')
            b--;
        if (b == ge || ge - b > 6)
            continue;
        long gen = strtol((const char *)b, NULL, 10);
        if (b == d || !is_ws(b[-1]))
            continue;
        while (b > d && is_ws(b[-1]))
            b--;
        const uint8_t *ne = b;
        while (b > d && b[-1] >= '0' && b[-1] <= '9')
            b--;
        if (b == ne || ne - b > 8)
            continue;
        long num = strtol((const char *)b, NULL, 10);
        size_t voff = (size_t)(q + 3 - d);
        obj_set(pdf, (int)num, voff, (int)gen, 0);
        const uint8_t *p = q + 3;
        struct pv v = { 0 };
        if (!pv_parse(&p, end, &v, 0)) {
            pv_free(&v);
            continue;
        }
        if (v.t == T_DICT) {
            const struct pv *ty = dict_get(&v, "Type");
            if (ty && ty->t == T_NAME) {
                if (!strcmp(ty->s, "ObjStm") && *nobjstm < max_stm)
                    objstms[(*nobjstm)++] = (int)num;
                else if (!strcmp(ty->s, "XRef") || !strcmp(ty->s, "Catalog")) {
                    const struct pv *r = dict_get(&v, "Root");
                    if (r && r->t == T_REF)
                        pdf->root = (int)r->num;
                    if (!strcmp(ty->s, "Catalog"))
                        pdf->root = pdf->root ? pdf->root : (int)num;
                    const struct pv *in = dict_get(&v, "Info");
                    if (in && in->t == T_REF)
                        pdf->info = (int)in->num;
                    if (dict_get(&v, "Encrypt"))
                        pdf->encrypted = true;
                }
            }
            skip_ws(&p, end);
            if (end - p >= 6 && !memcmp(p, "stream", 6)) {                    /* behind the stream's data */
                const struct pv *len = dict_get(&v, "Length");
                const uint8_t *s = p + 6;
                if (s < end && *s == '\r')
                    s++;
                if (s < end && *s == '\n')
                    s++;
                bool done = false;
                if (len && len->t == T_INT && len->num >= 0 && len->num <= (double)(end - s)) {
                    const uint8_t *e = s + (size_t)len->num, *w = e;
                    while (w < end && is_ws(*w))
                        w++;
                    if (end - w >= 9 && !memcmp(w, "endstream", 9)) {
                        pos = (size_t)(w + 9 - d);
                        done = true;
                    }
                }
                if (!done) {
                    const uint8_t *e = memmem(s, (size_t)(end - s), "endstream", 9);
                    pos = e ? (size_t)(e + 9 - d) : pdf->n;
                }
            } else {
                pos = (size_t)(p - d);
            }
        } else {
            pos = (size_t)(p - d);
        }
        pv_free(&v);
    }
}

static void trailers(struct pdf *pdf)
{
    const uint8_t *d = pdf->d, *end = pdf->d + pdf->n;
    size_t pos = 0;
    while (pos < pdf->n) {
        const uint8_t *q = memmem(d + pos, pdf->n - pos, "trailer", 7);
        if (!q)
            break;
        pos = (size_t)(q - d) + 7;
        const uint8_t *p = q + 7;
        struct pv v = { 0 };
        if (pv_parse(&p, end, &v, 0) && v.t == T_DICT) {
            const struct pv *r = dict_get(&v, "Root"), *in = dict_get(&v, "Info");
            if (r && r->t == T_REF)
                pdf->root = (int)r->num;
            if (in && in->t == T_REF)
                pdf->info = (int)in->num;
            if (dict_get(&v, "Encrypt"))
                pdf->encrypted = true;
        }
        pv_free(&v);
    }
}

static void expand_objstm(struct pdf *pdf, int stm_num)
{
    if (pdf->nstm >= 64)
        return;
    struct pv dict = { 0 };
    size_t n = 0;
    uint8_t *data = stream_data(pdf, stm_num, &n, &dict, STREAM_CAP);
    if (!data)
        return;
    const struct pv *nn = dict_get(&dict, "N"), *first = dict_get(&dict, "First");
    if (nn && first && nn->t == T_INT && first->t == T_INT && nn->num > 0 && nn->num < 1000000 && first->num >= 0 && first->num < (double)n) {
        const uint8_t *p = data, *end = data + (size_t)first->num;
        pdf->stm[pdf->nstm].num = stm_num;
        pdf->stm[pdf->nstm].data = data;
        pdf->stm[pdf->nstm].len = n;
        pdf->nstm++;
        for (long i = 0; i < (long)nn->num; i++) {
            struct pv a = { 0 }, b = { 0 };
            if (!pv_parse(&p, end, &a, 0) || a.t != T_INT || !pv_parse(&p, end, &b, 0) || b.t != T_INT) {
                pv_free(&a);
                pv_free(&b);
                break;
            }
            if (!obj_find(pdf, (int)a.num))
                obj_set(pdf, (int)a.num, (size_t)first->num + (size_t)b.num, 0, stm_num);
        }
    } else {
        free(data);
    }
    pv_free(&dict);
}

/* ---- text strings of the document information ---- */
static void text_string(const struct pv *v, char *out, size_t cap)
{
    out[0] = 0;
    if (!v || v->t != T_STR)
        return;
    struct sbuf b = { 0 };
    const uint8_t *s = (const uint8_t *)v->s;
    if (v->slen >= 2 && s[0] == 0xfe && s[1] == 0xff) {
        for (size_t i = 2; i + 1 < v->slen; i += 2) {
            unsigned cp = (unsigned)s[i] << 8 | s[i + 1];
            if (cp >= 0xd800 && cp < 0xdc00 && i + 3 < v->slen) {
                unsigned lo = (unsigned)s[i + 2] << 8 | s[i + 3];
                if (lo >= 0xdc00 && lo < 0xe000) {
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                    i += 2;
                }
            }
            sb_utf8(&b, cp < 0x20 ? ' ' : cp);
        }
    } else if (v->slen >= 3 && s[0] == 0xef && s[1] == 0xbb && s[2] == 0xbf) {
        sb_add(&b, v->s + 3, v->slen - 3);
    } else {
        for (size_t i = 0; i < v->slen; i++)
            sb_utf8(&b, s[i] < 0x20 ? ' ' : s[i]);
    }
    snprintf(out, cap, "%s", b.s ? b.s : "");
    free(b.s);
}

/* ---- glyph names, encodings ---- */
static const uint16_t k_mac_hi[128] = {
    0x00c4, 0x00c5, 0x00c7, 0x00c9, 0x00d1, 0x00d6, 0x00dc, 0x00e1, 0x00e0, 0x00e2, 0x00e4, 0x00e3, 0x00e5, 0x00e7, 0x00e9, 0x00e8, 0x00ea, 0x00eb, 0x00ed, 0x00ec, 0x00ee, 0x00ef,
    0x00f1, 0x00f3, 0x00f2, 0x00f4, 0x00f6, 0x00f5, 0x00fa, 0x00f9, 0x00fb, 0x00fc, 0x2020, 0x00b0, 0x00a2, 0x00a3, 0x00a7, 0x2022, 0x00b6, 0x00df, 0x00ae, 0x00a9, 0x2122, 0x00b4,
    0x00a8, 0x2260, 0x00c6, 0x00d8, 0x221e, 0x00b1, 0x2264, 0x2265, 0x00a5, 0x00b5, 0x2202, 0x2211, 0x220f, 0x03c0, 0x222b, 0x00aa, 0x00ba, 0x03a9, 0x00e6, 0x00f8, 0x00bf, 0x00a1,
    0x00ac, 0x221a, 0x0192, 0x2248, 0x2206, 0x00ab, 0x00bb, 0x2026, 0x00a0, 0x00c0, 0x00c3, 0x00d5, 0x0152, 0x0153, 0x2013, 0x2014, 0x201c, 0x201d, 0x2018, 0x2019, 0x00f7, 0x25ca,
    0x00ff, 0x0178, 0x2044, 0x20ac, 0x2039, 0x203a, 0xfb01, 0xfb02, 0x2021, 0x00b7, 0x201a, 0x201e, 0x2030, 0x00c2, 0x00ca, 0x00c1, 0x00cb, 0x00c8, 0x00cd, 0x00ce, 0x00cf, 0x00cc,
    0x00d3, 0x00d4, 0xf8ff, 0x00d2, 0x00da, 0x00db, 0x00d9, 0x0131, 0x02c6, 0x02dc, 0x00af, 0x02d8, 0x02d9, 0x02da, 0x00b8, 0x02dd, 0x02db, 0x02c7,
};

static const uint16_t k_win_80[32] = {
    0x20ac, 0x2022, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021, 0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0x2022, 0x017d, 0x2022,
    0x2022, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014, 0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0x2022, 0x017e, 0x0178,
};

static const struct { const char *n; uint16_t c; } k_glyphs[] = {
    {"space",0x20},{"exclam",0x21},{"quotedbl",0x22},{"numbersign",0x23},{"dollar",0x24},{"percent",0x25},{"ampersand",0x26},{"quotesingle",0x27},{"parenleft",0x28},
    {"parenright",0x29},{"asterisk",0x2a},{"plus",0x2b},{"comma",0x2c},{"hyphen",0x2d},{"period",0x2e},{"slash",0x2f},{"zero",0x30},{"one",0x31},{"two",0x32},{"three",0x33},
    {"four",0x34},{"five",0x35},{"six",0x36},{"seven",0x37},{"eight",0x38},{"nine",0x39},{"colon",0x3a},{"semicolon",0x3b},{"less",0x3c},{"equal",0x3d},{"greater",0x3e},
    {"question",0x3f},{"at",0x40},{"bracketleft",0x5b},{"backslash",0x5c},{"bracketright",0x5d},{"asciicircum",0x5e},{"underscore",0x5f},{"grave",0x60},{"braceleft",0x7b},
    {"bar",0x7c},{"braceright",0x7d},{"asciitilde",0x7e},{"nbspace",0xa0},{"exclamdown",0xa1},{"cent",0xa2},{"sterling",0xa3},{"currency",0xa4},{"yen",0xa5},{"brokenbar",0xa6},
    {"section",0xa7},{"dieresis",0xa8},{"copyright",0xa9},{"ordfeminine",0xaa},{"guillemotleft",0xab},{"logicalnot",0xac},{"sfthyphen",0xad},{"registered",0xae},{"macron",0xaf},
    {"degree",0xb0},{"plusminus",0xb1},{"twosuperior",0xb2},{"threesuperior",0xb3},{"acute",0xb4},{"mu",0xb5},{"paragraph",0xb6},{"periodcentered",0xb7},{"cedilla",0xb8},
    {"onesuperior",0xb9},{"ordmasculine",0xba},{"guillemotright",0xbb},{"onequarter",0xbc},{"onehalf",0xbd},{"threequarters",0xbe},{"questiondown",0xbf},
    {"Agrave",0xc0},{"Aacute",0xc1},{"Acircumflex",0xc2},{"Atilde",0xc3},{"Adieresis",0xc4},{"Aring",0xc5},{"AE",0xc6},{"Ccedilla",0xc7},{"Egrave",0xc8},{"Eacute",0xc9},
    {"Ecircumflex",0xca},{"Edieresis",0xcb},{"Igrave",0xcc},{"Iacute",0xcd},{"Icircumflex",0xce},{"Idieresis",0xcf},{"Eth",0xd0},{"Ntilde",0xd1},{"Ograve",0xd2},{"Oacute",0xd3},
    {"Ocircumflex",0xd4},{"Otilde",0xd5},{"Odieresis",0xd6},{"multiply",0xd7},{"Oslash",0xd8},{"Ugrave",0xd9},{"Uacute",0xda},{"Ucircumflex",0xdb},{"Udieresis",0xdc},
    {"Yacute",0xdd},{"Thorn",0xde},{"germandbls",0xdf},{"agrave",0xe0},{"aacute",0xe1},{"acircumflex",0xe2},{"atilde",0xe3},{"adieresis",0xe4},{"aring",0xe5},{"ae",0xe6},
    {"ccedilla",0xe7},{"egrave",0xe8},{"eacute",0xe9},{"ecircumflex",0xea},{"edieresis",0xeb},{"igrave",0xec},{"iacute",0xed},{"icircumflex",0xee},{"idieresis",0xef},
    {"eth",0xf0},{"ntilde",0xf1},{"ograve",0xf2},{"oacute",0xf3},{"ocircumflex",0xf4},{"otilde",0xf5},{"odieresis",0xf6},{"divide",0xf7},{"oslash",0xf8},{"ugrave",0xf9},
    {"uacute",0xfa},{"ucircumflex",0xfb},{"udieresis",0xfc},{"yacute",0xfd},{"thorn",0xfe},{"ydieresis",0xff},{"Scaron",0x160},{"scaron",0x161},{"Zcaron",0x17d},{"zcaron",0x17e},
    {"Euro",0x20ac},{"bullet",0x2022},{"endash",0x2013},{"emdash",0x2014},{"quoteleft",0x2018},{"quoteright",0x2019},{"quotesinglbase",0x201a},{"quotedblleft",0x201c},
    {"quotedblright",0x201d},{"quotedblbase",0x201e},{"ellipsis",0x2026},{"dagger",0x2020},{"daggerdbl",0x2021},{"perthousand",0x2030},{"guilsinglleft",0x2039},
    {"guilsinglright",0x203a},{"trademark",0x2122},{"fi",0xfb01},{"fl",0xfb02},{"ff",0xfb00},{"ffi",0xfb03},{"ffl",0xfb04},{"minus",0x2212},{"fraction",0x2044},{"florin",0x192},
    {"circumflex",0x2c6},{"tilde",0x2dc},{"breve",0x2d8},{"dotaccent",0x2d9},{"ring",0x2da},{"hungarumlaut",0x2dd},{"ogonek",0x2db},{"caron",0x2c7},{"dotlessi",0x131},
    {"Lslash",0x141},{"lslash",0x142},{"OE",0x152},{"oe",0x153},{"Ydieresis",0x178},{"lessequal",0x2264},{"greaterequal",0x2265},{"notequal",0x2260},{"infinity",0x221e},
    {"summation",0x2211},{"product",0x220f},{"pi",0x3c0},{"Omega",0x3a9},{"radical",0x221a},{"approxequal",0x2248},{"arrowright",0x2192},{"arrowleft",0x2190},
    {"alpha",0x3b1},{"beta",0x3b2},{"gamma",0x3b3},{"delta",0x3b4},{"epsilon",0x3b5},{"lambda",0x3bb},{"sigma",0x3c3},{"theta",0x3b8},{"integral",0x222b},{"partialdiff",0x2202},
};

static unsigned glyph_unicode(const char *name)
{
    if (!name[0])
        return 0;
    if (!name[1])
        return (unsigned char)name[0];
    if (!strncmp(name, "uni", 3) && strlen(name) >= 7 && hexv(name[3]) >= 0)
        return (unsigned)strtoul(name + 3, NULL, 16) & 0xffff;
    if (name[0] == 'u' && strlen(name) >= 5 && strlen(name) <= 7 && hexv(name[1]) >= 0 && isxdigit((unsigned char)name[1]))
        return (unsigned)strtoul(name + 1, NULL, 16);
    for (size_t i = 0; i < sizeof(k_glyphs) / sizeof(k_glyphs[0]); i++)
        if (!strcmp(k_glyphs[i].n, name))
            return k_glyphs[i].c;
    char base[64];
    const char *dot = strchr(name, '.');                                /* "a.sc", "A.swash": the plain letter */
    if (dot && dot > name && (size_t)(dot - name) < sizeof(base)) {
        memcpy(base, name, (size_t)(dot - name));
        base[dot - name] = 0;
        return glyph_unicode(base);
    }
    return 0;
}

static unsigned enc_code(const char *enc, unsigned code)
{
    if (code < 0x20)
        return 0;
    if (code < 0x7f) {
        if (enc && !strcmp(enc, "StandardEncoding")) {
            if (code == 0x27) return 0x2019;
            if (code == 0x60) return 0x2018;
        }
        return code;
    }
    if (enc && !strcmp(enc, "MacRomanEncoding"))
        return code >= 0x80 ? k_mac_hi[code - 0x80] : 0;
    if (code >= 0x80 && code < 0xa0)
        return k_win_80[code - 0x80];
    return code >= 0xa0 ? code : 0;
}

/* ---- fonts ---- */
struct font {
    int num;
    bool two_byte, usable;
    char **map;                         /* code -> UTF-8 (a string of its own) */
    int mapn;
    double *w;                          /* widths in 1/1000 em for the codes first .. first + nw - 1 */
    int first, nw;
    double defw;
    struct { int lo, hi; double w; } *cw;                                   /* CID widths */
    int ncw;
};

static void font_free(struct font *f)
{
    if (f->map)
        for (int i = 0; i < f->mapn; i++)
            free(f->map[i]);
    free(f->map);
    free(f->w);
    free(f->cw);
    memset(f, 0, sizeof(*f));
}

static void font_set(struct font *f, unsigned code, const char *utf8)
{
    if (code >= (unsigned)f->mapn)
        return;
    free(f->map[code]);
    f->map[code] = strdup(utf8);
}

static void font_set_cp(struct font *f, unsigned code, unsigned cp)
{
    struct sbuf b = { 0 };
    if (cp == 0xfb01) sb_add(&b, "fi", 2);
    else if (cp == 0xfb02) sb_add(&b, "fl", 2);
    else if (cp == 0xfb00) sb_add(&b, "ff", 2);
    else if (cp == 0xfb03) sb_add(&b, "ffi", 3);
    else if (cp == 0xfb04) sb_add(&b, "ffl", 3);
    else if (cp == 0xa0) sb_add(&b, " ", 1);
    else sb_utf8(&b, cp);
    if (b.s)
        font_set(f, code, b.s);
    free(b.s);
}

/* The big-endian number of a hex string (<0041>) and its UTF-16BE text as UTF-8. */
static unsigned hex_code(const struct pv *s)
{
    unsigned v = 0;
    for (size_t i = 0; i < s->slen && i < 4; i++)
        v = v << 8 | (uint8_t)s->s[i];
    return v;
}

static void utf16_to_utf8(const struct pv *s, struct sbuf *o)
{
    const uint8_t *p = (const uint8_t *)s->s;
    if (s->slen == 1) {
        sb_utf8(o, p[0]);
        return;
    }
    for (size_t i = 0; i + 1 < s->slen; i += 2) {
        unsigned cp = (unsigned)p[i] << 8 | p[i + 1];
        if (cp >= 0xd800 && cp < 0xdc00 && i + 3 < s->slen) {
            unsigned lo = (unsigned)p[i + 2] << 8 | p[i + 3];
            if (lo >= 0xdc00 && lo < 0xe000) {
                cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                i += 2;
            }
        }
        if (cp == 0xfb01) sb_add(o, "fi", 2);
        else if (cp == 0xfb02) sb_add(o, "fl", 2);
        else if (cp == 0xa0) sb_add(o, " ", 1);
        else if (cp >= 0x20 || cp == '\t') sb_utf8(o, cp);
    }
}

/* A /ToUnicode CMap: bfchar and bfrange sections. */
static void parse_tounicode(struct font *f, const uint8_t *d, size_t n)
{
    const uint8_t *p = d, *end = d + n;
    struct pv stack[4];
    int ns = 0;
    memset(stack, 0, sizeof(stack));
    while (p < end) {
        struct pv v = { 0 };
        if (!pv_parse(&p, end, &v, 0))
            break;
        if (v.t == T_NULL && v.s) {                                     /* a keyword */
            if (!strcmp(v.s, "beginbfchar")) {
                while (p < end) {
                    struct pv a = { 0 }, b = { 0 };
                    if (!pv_parse(&p, end, &a, 0))
                        break;
                    if (a.t == T_NULL) {                                /* endbfchar */
                        pv_free(&a);
                        break;
                    }
                    if (!pv_parse(&p, end, &b, 0)) {
                        pv_free(&a);
                        break;
                    }
                    if (a.t == T_STR && b.t == T_STR) {
                        struct sbuf o = { 0 };
                        utf16_to_utf8(&b, &o);
                        if (o.s)
                            font_set(f, hex_code(&a), o.s);
                        free(o.s);
                    }
                    pv_free(&a);
                    pv_free(&b);
                }
            } else if (!strcmp(v.s, "beginbfrange")) {
                while (p < end) {
                    struct pv a = { 0 }, b = { 0 }, c = { 0 };
                    if (!pv_parse(&p, end, &a, 0))
                        break;
                    if (a.t == T_NULL) {
                        pv_free(&a);
                        break;
                    }
                    if (!pv_parse(&p, end, &b, 0) || !pv_parse(&p, end, &c, 0)) {
                        pv_free(&a);
                        pv_free(&b);
                        break;
                    }
                    if (a.t == T_STR && b.t == T_STR) {
                        unsigned lo = hex_code(&a), hi = hex_code(&b);
                        if (hi - lo < 70000 && hi >= lo) {
                            if (c.t == T_STR) {
                                struct sbuf base = { 0 };
                                utf16_to_utf8(&c, &base);
                                unsigned last = c.slen >= 2 ? ((uint8_t)c.s[c.slen - 2] << 8 | (uint8_t)c.s[c.slen - 1]) : (c.slen ? (uint8_t)c.s[0] : 0);
                                for (unsigned k = lo; k <= hi && k < (unsigned)f->mapn; k++) {
                                    struct pv tmp = c;
                                    uint8_t b2[8];
                                    size_t bl = c.slen < 8 ? c.slen : 8;
                                    memcpy(b2, c.s, bl);
                                    unsigned v2 = last + (k - lo);
                                    if (bl >= 2) {
                                        b2[bl - 2] = (uint8_t)(v2 >> 8);
                                        b2[bl - 1] = (uint8_t)v2;
                                    } else if (bl == 1) {
                                        b2[0] = (uint8_t)v2;
                                    }
                                    tmp.s = (char *)b2;
                                    tmp.slen = bl;
                                    struct sbuf o = { 0 };
                                    utf16_to_utf8(&tmp, &o);
                                    if (o.s)
                                        font_set(f, k, o.s);
                                    free(o.s);
                                }
                                free(base.s);
                            } else if (c.t == T_ARR) {
                                for (int k = 0; k < c.n && lo + (unsigned)k <= hi; k++)
                                    if (c.it[k].t == T_STR) {
                                        struct sbuf o = { 0 };
                                        utf16_to_utf8(&c.it[k], &o);
                                        if (o.s)
                                            font_set(f, lo + (unsigned)k, o.s);
                                        free(o.s);
                                    }
                            }
                        }
                    }
                    pv_free(&a);
                    pv_free(&b);
                    pv_free(&c);
                }
            }
        }
        pv_free(&v);
    }
    (void)stack;
    (void)ns;
}

static const struct pv *dget_r(struct pdf *pdf, const struct pv *d, const char *k, struct pv *tmp)
{
    memset(tmp, 0, sizeof(*tmp));
    const struct pv *v = dict_get(d, k);
    return v ? resolve(pdf, v, tmp, 0) : NULL;
}

static bool font_load(struct pdf *pdf, int num, const struct pv *fd, struct font *f)
{
    memset(f, 0, sizeof(*f));
    f->num = num;
    struct pv t1 = { 0 }, t2 = { 0 }, t3 = { 0 }, t4 = { 0 };
    const struct pv *sub = dget_r(pdf, fd, "Subtype", &t1);
    bool type0 = sub && sub->t == T_NAME && !strcmp(sub->s, "Type0");
    f->two_byte = type0;
    f->mapn = type0 ? 65536 : 256;
    f->map = calloc((size_t)f->mapn, sizeof(char *));
    f->defw = 500;
    if (!f->map) {
        pv_free(&t1);
        return false;
    }
    const struct pv *enc = dget_r(pdf, fd, "Encoding", &t2);
    const char *base = enc && enc->t == T_NAME ? enc->s : NULL;
    if (!type0) {
        const struct pv *diff = NULL;
        struct pv t5 = { 0 };
        if (enc && enc->t == T_DICT) {
            const struct pv *be = dget_r(pdf, enc, "BaseEncoding", &t5);
            if (be && be->t == T_NAME)
                base = be->s;
            else
                pv_free(&t5);
            diff = dict_get(enc, "Differences");
        }
        for (unsigned c = 0; c < 256; c++) {
            unsigned cp = enc_code(base, c);
            if (cp)
                font_set_cp(f, c, cp);
        }
        if (diff && diff->t == T_ARR) {
            unsigned code = 0;
            for (int i = 0; i < diff->n; i++) {
                const struct pv *e = &diff->it[i];
                if (e->t == T_INT)
                    code = (unsigned)e->num;
                else if (e->t == T_NAME) {
                    unsigned cp = glyph_unicode(e->s);
                    if (cp && code < 256)
                        font_set_cp(f, code, cp);
                    else if (code < 256) {
                        free(f->map[code]);
                        f->map[code] = NULL;
                    }
                    code++;
                }
            }
        }
        /* widths */
        const struct pv *fc = dget_r(pdf, fd, "FirstChar", &t3), *ws = dict_get(fd, "Widths");
        struct pv t6 = { 0 };
        const struct pv *wsv = ws ? resolve(pdf, ws, &t6, 0) : NULL;
        if (fc && wsv && wsv->t == T_ARR && wsv->n > 0 && wsv->n <= 1024) {
            f->first = (int)fc->num;
            f->nw = wsv->n;
            f->w = calloc((size_t)wsv->n, sizeof(double));
            for (int i = 0; f->w && i < wsv->n; i++)
                f->w[i] = wsv->it[i].num;
        }
        pv_free(&t6);
        pv_free(&t3);
    } else {
        /* the CID font's widths */
        const struct pv *desc = dict_get(fd, "DescendantFonts");
        struct pv t7 = { 0 }, t8 = { 0 };
        const struct pv *dv = desc ? resolve(pdf, desc, &t7, 0) : NULL;
        if (dv && dv->t == T_ARR && dv->n > 0) {
            const struct pv *cf = resolve(pdf, &dv->it[0], &t8, 0);
            if (cf && cf->t == T_DICT) {
                struct pv t9 = { 0 }, t10 = { 0 };
                const struct pv *dw = dget_r(pdf, cf, "DW", &t9), *w = dget_r(pdf, cf, "W", &t10);
                f->defw = dw && (dw->t == T_INT || dw->t == T_REAL) ? dw->num : 1000;
                if (w && w->t == T_ARR) {
                    int cap = 0;
                    for (int i = 0; i < w->n;) {
                        if (w->it[i].t != T_INT)
                            break;
                        int c0 = (int)w->it[i].num;
                        if (i + 1 < w->n && w->it[i + 1].t == T_ARR) {
                            for (int k = 0; k < w->it[i + 1].n && f->ncw < 20000; k++) {
                                if (f->ncw == cap) {
                                    cap = cap ? cap * 2 : 64;
                                    f->cw = realloc(f->cw, (size_t)cap * sizeof(f->cw[0]));
                                }
                                f->cw[f->ncw].lo = f->cw[f->ncw].hi = c0 + k;
                                f->cw[f->ncw++].w = w->it[i + 1].it[k].num;
                            }
                            i += 2;
                        } else if (i + 2 < w->n) {
                            if (f->ncw < 20000) {
                                if (f->ncw == cap) {
                                    cap = cap ? cap * 2 : 64;
                                    f->cw = realloc(f->cw, (size_t)cap * sizeof(f->cw[0]));
                                }
                                f->cw[f->ncw].lo = c0;
                                f->cw[f->ncw].hi = (int)w->it[i + 1].num;
                                f->cw[f->ncw++].w = w->it[i + 2].num;
                            }
                            i += 3;
                        } else {
                            break;
                        }
                    }
                }
                pv_free(&t9);
                pv_free(&t10);
            }
            pv_free(&t8);
        }
        pv_free(&t7);
    }
    /* ToUnicode wins over everything */
    const struct pv *tu = dict_get(fd, "ToUnicode");
    bool have_tu = false;
    if (tu && tu->t == T_REF) {
        size_t n = 0;
        uint8_t *data = stream_data(pdf, (int)tu->num, &n, NULL, STREAM_CAP);
        if (data) {
            parse_tounicode(f, data, n);
            free(data);
            have_tu = true;
        }
    }
    f->usable = !type0 || have_tu;
    if (!type0 && !have_tu)
        f->usable = true;
    pv_free(&t1);
    pv_free(&t2);
    (void)t4;
    return true;
}

/* ---- the content stream: text only ---- */
struct mat {
    double a, b, c, d, e, f;
};

static struct mat m_mul(struct mat m, struct mat n)                    /* m first, then n (the PDF way) */
{
    struct mat r = {
        m.a * n.a + m.b * n.c, m.a * n.b + m.b * n.d, m.c * n.a + m.d * n.c, m.c * n.b + m.d * n.d,
        m.e * n.a + m.f * n.c + n.e, m.e * n.b + m.f * n.d + n.f,
    };
    return r;
}

struct interp {
    struct pdf *pdf;
    struct font *fonts;
    int nfonts;
    struct sbuf *out;
    size_t limit;                       /* stop when the page's text is this long */
    long ops;                           /* a budget */
    int missing;                        /* letters that no font could turn into text */
    int shown;
    double last_x, last_y, last_h;      /* where the last text ended (page space): shared with the forms drawn in between */
    bool have_pos, pending_space;
};

struct tstate {
    struct mat ctm, tm, tlm;
    struct font *font;
    double fs, leading, tc, tw, th;
    struct mat stack[32];
    int sp;
};

static struct font *font_lookup(struct interp *in, const struct pv *res, const char *name)
{
    struct pv t1 = { 0 }, t2 = { 0 };
    const struct pv *fonts = res ? dget_r(in->pdf, res, "Font", &t1) : NULL;
    if (!fonts || fonts->t != T_DICT) {
        pv_free(&t1);
        return NULL;
    }
    const struct pv *ref = dict_get(fonts, name);
    struct font *found = NULL;
    if (ref && ref->t == T_REF) {
        for (int i = 0; i < in->nfonts; i++)
            if (in->fonts[i].num == (int)ref->num)
                found = &in->fonts[i];
        if (!found && in->nfonts < MAX_FONTS) {
            const struct pv *fd = resolve(in->pdf, ref, &t2, 0);
            if (fd && fd->t == T_DICT && font_load(in->pdf, (int)ref->num, fd, &in->fonts[in->nfonts]))
                found = &in->fonts[in->nfonts++];
            pv_free(&t2);
        }
    }
    pv_free(&t1);
    return found;
}

static double font_width(const struct font *f, unsigned code)
{
    if (f->two_byte) {
        for (int i = 0; i < f->ncw; i++)
            if ((int)code >= f->cw[i].lo && (int)code <= f->cw[i].hi)
                return f->cw[i].w;
        return f->defw;
    }
    if (f->w && (int)code >= f->first && (int)code < f->first + f->nw)
        return f->w[(int)code - f->first];
    return f->defw;
}

static void out_char_sep(struct interp *in, struct tstate *st, const char *what)
{
    (void)st;
    sb_add(in->out, what, strlen(what));
}

/* One string shown: the position decides a line break or a blank before it, then every code is mapped and the text matrix advanced. */
static void show_string(struct interp *in, struct tstate *st, const uint8_t *s, size_t n)
{
    if (!st->font) {
        return;
    }
    struct mat prod = m_mul(st->tm, st->ctm);
    double scale = sqrt(fabs(prod.a * prod.d - prod.b * prod.c));
    double fse = fabs(st->fs) * (scale > 1e-9 ? scale : 1);
    if (fse < 1)
        fse = 1;
    if (in->out->n < in->limit && in->out->n > 0) {
        if (in->have_pos) {
            double dy = fabs(prod.f - in->last_y);
            if (dy > 0.45 * fse && dy > 0.45 * in->last_h * 0 + 0) {
                if (in->out->s[in->out->n - 1] != '\n')
                    out_char_sep(in, st, "\n");
                in->pending_space = false;
            } else {
                double gap = prod.e - in->last_x;
                if (gap > 0.18 * fse || gap < -1.5 * fse || in->pending_space) {
                    if (in->out->s[in->out->n - 1] != ' ' && in->out->s[in->out->n - 1] != '\n')
                        out_char_sep(in, st, " ");
                }
            }
        }
    }
    in->pending_space = false;
    size_t i = 0;
    while (i < n && in->out->n < in->limit) {
        unsigned code = s[i++];
        if (st->font->two_byte) {
            if (i >= n)
                break;
            code = code << 8 | s[i++];
        }
        const char *txt = code < (unsigned)st->font->mapn ? st->font->map[code] : NULL;
        if (txt && txt[0]) {
            sb_add(in->out, txt, strlen(txt));
            in->shown++;
        } else if (!(code == 32 && !st->font->two_byte)) {
            in->missing++;
        }
        double w = font_width(st->font, code) / 1000 * st->fs + st->tc + ((code == 32 && !st->font->two_byte) ? st->tw : 0);
        struct mat adv = { 1, 0, 0, 1, w * st->th, 0 };
        st->tm = m_mul(adv, st->tm);
    }
    struct mat after = m_mul(st->tm, st->ctm);
    in->last_x = after.e;
    in->last_y = after.f;
    in->last_h = fse;
    in->have_pos = true;
}

static void interpret(struct interp *in, const uint8_t *d, size_t n, const struct pv *res, struct mat ctm0, int depth);

static void interpret_xobject(struct interp *in, const struct pv *res, const char *name, struct tstate *st, int depth)
{
    if (depth > 3 || !res)
        return;
    struct pv t1 = { 0 }, t2 = { 0 }, t3 = { 0 };
    const struct pv *xo = dget_r(in->pdf, res, "XObject", &t1);
    const struct pv *ref = xo && xo->t == T_DICT ? dict_get(xo, name) : NULL;
    if (ref && ref->t == T_REF) {
        const struct pv *fd = resolve(in->pdf, ref, &t2, 0);
        size_t n = 0;
        struct pv dict = { 0 };
        uint8_t *data = NULL;
        if (fd && fd->t == T_DICT) {
            const struct pv *sub = dict_get(fd, "Subtype");
            if (sub && sub->t == T_NAME && !strcmp(sub->s, "Form"))
                data = stream_data(in->pdf, (int)ref->num, &n, &dict, STREAM_CAP);
        }
        if (data) {
            struct mat m = { 1, 0, 0, 1, 0, 0 };
            const struct pv *mx = dict_get(&dict, "Matrix");
            if (mx && mx->t == T_ARR && mx->n == 6)
                m = (struct mat){ mx->it[0].num, mx->it[1].num, mx->it[2].num, mx->it[3].num, mx->it[4].num, mx->it[5].num };
            const struct pv *r2 = dget_r(in->pdf, &dict, "Resources", &t3);
            interpret(in, data, n, r2 && r2->t == T_DICT ? r2 : res, m_mul(m, st->ctm), depth + 1);
            pv_free(&t3);
            free(data);
        }
        pv_free(&dict);
        pv_free(&t2);
    }
    pv_free(&t1);
}

static void interpret(struct interp *in, const uint8_t *d, size_t n, const struct pv *res, struct mat ctm0, int depth)
{
    const uint8_t *p = d, *end = d + n;
    struct tstate st;
    memset(&st, 0, sizeof(st));
    st.ctm = ctm0;
    st.tm = st.tlm = (struct mat){ 1, 0, 0, 1, 0, 0 };
    st.th = 1;
    struct pv ops[64];
    int nop = 0;
    while (p < end && in->out->n < in->limit && in->ops < 3000000) {
        struct pv v = { 0 };
        if (!pv_parse(&p, end, &v, 0))
            break;
        in->ops++;
        if (!(v.t == T_NULL && v.s && strcmp(v.s, "null"))) {           /* an operand */
            if (nop < 64) {
                ops[nop++] = v;
            } else {
                pv_free(&v);
            }
            continue;
        }
        const char *op = v.s;
        if (!strcmp(op, "q")) {
            if (st.sp < 32)
                st.stack[st.sp++] = st.ctm;
        } else if (!strcmp(op, "Q")) {
            if (st.sp > 0)
                st.ctm = st.stack[--st.sp];
        } else if (!strcmp(op, "cm") && nop >= 6) {
            struct mat m = { ops[nop - 6].num, ops[nop - 5].num, ops[nop - 4].num, ops[nop - 3].num, ops[nop - 2].num, ops[nop - 1].num };
            st.ctm = m_mul(m, st.ctm);
        } else if (!strcmp(op, "BT")) {
            st.tm = st.tlm = (struct mat){ 1, 0, 0, 1, 0, 0 };
        } else if (!strcmp(op, "Tf") && nop >= 2) {
            st.font = ops[nop - 2].t == T_NAME ? font_lookup(in, res, ops[nop - 2].s) : NULL;
            st.fs = ops[nop - 1].num;
        } else if (!strcmp(op, "Td") && nop >= 2) {
            struct mat t = { 1, 0, 0, 1, ops[nop - 2].num, ops[nop - 1].num };
            st.tlm = m_mul(t, st.tlm);
            st.tm = st.tlm;
        } else if (!strcmp(op, "TD") && nop >= 2) {
            st.leading = -ops[nop - 1].num;
            struct mat t = { 1, 0, 0, 1, ops[nop - 2].num, ops[nop - 1].num };
            st.tlm = m_mul(t, st.tlm);
            st.tm = st.tlm;
        } else if (!strcmp(op, "Tm") && nop >= 6) {
            st.tm = st.tlm = (struct mat){ ops[nop - 6].num, ops[nop - 5].num, ops[nop - 4].num, ops[nop - 3].num, ops[nop - 2].num, ops[nop - 1].num };
        } else if (!strcmp(op, "T*")) {
            struct mat t = { 1, 0, 0, 1, 0, -st.leading };
            st.tlm = m_mul(t, st.tlm);
            st.tm = st.tlm;
        } else if (!strcmp(op, "TL") && nop >= 1) {
            st.leading = ops[nop - 1].num;
        } else if (!strcmp(op, "Tc") && nop >= 1) {
            st.tc = ops[nop - 1].num;
        } else if (!strcmp(op, "Tw") && nop >= 1) {
            st.tw = ops[nop - 1].num;
        } else if (!strcmp(op, "Tz") && nop >= 1) {
            st.th = ops[nop - 1].num / 100;
        } else if (!strcmp(op, "Tj") && nop >= 1 && ops[nop - 1].t == T_STR) {
            show_string(in, &st, (const uint8_t *)ops[nop - 1].s, ops[nop - 1].slen);
        } else if ((!strcmp(op, "'") || !strcmp(op, "\"")) && nop >= 1 && ops[nop - 1].t == T_STR) {
            if (op[0] == '"' && nop >= 3) {
                st.tw = ops[nop - 3].num;
                st.tc = ops[nop - 2].num;
            }
            struct mat t = { 1, 0, 0, 1, 0, -st.leading };
            st.tlm = m_mul(t, st.tlm);
            st.tm = st.tlm;
            show_string(in, &st, (const uint8_t *)ops[nop - 1].s, ops[nop - 1].slen);
        } else if (!strcmp(op, "TJ") && nop >= 1 && ops[nop - 1].t == T_ARR) {
            const struct pv *a = &ops[nop - 1];
            for (int i = 0; i < a->n; i++) {
                if (a->it[i].t == T_STR) {
                    show_string(in, &st, (const uint8_t *)a->it[i].s, a->it[i].slen);
                } else if (a->it[i].t == T_INT || a->it[i].t == T_REAL) {
                    double adj = a->it[i].num;
                    struct mat t = { 1, 0, 0, 1, -adj / 1000 * st.fs * st.th, 0 };
                    st.tm = m_mul(t, st.tm);
                    if (adj < -150)
                        in->pending_space = true;
                    struct mat after = m_mul(st.tm, st.ctm);
                    in->last_x = after.e;
                }
            }
        } else if (!strcmp(op, "Do") && nop >= 1 && ops[nop - 1].t == T_NAME) {
            interpret_xobject(in, res, ops[nop - 1].s, &st, depth);
        } else if (!strcmp(op, "BI")) {                                 /* an inline image: its binary data is skipped */
            const uint8_t *q = p;
            const uint8_t *idp = NULL;
            while (q + 2 < end) {
                if (is_ws(q[0]) && q[1] == 'I' && q[2] == 'D' && (q + 3 >= end || is_ws(q[3]))) {
                    idp = q + 3;
                    break;
                }
                q++;
            }
            if (idp) {
                const uint8_t *e = idp;
                while (e + 2 < end && !(is_ws(e[0]) && e[1] == 'E' && e[2] == 'I' && (e + 3 >= end || is_ws(e[3]))))
                    e++;
                p = e + 2 < end ? e + 3 : end;
            }
        }
        for (int i = 0; i < nop; i++)
            pv_free(&ops[i]);
        nop = 0;
        pv_free(&v);
    }
    for (int i = 0; i < nop; i++)
        pv_free(&ops[i]);
}

/* ---- pages ---- */
struct page {
    int num;
    struct pv res;                      /* the (inherited) resources, a copy */
    bool has_res;
};

static void pv_clone(const struct pv *s, struct pv *d)
{
    *d = *s;
    d->s = s->s ? malloc(s->slen + 1) : NULL;
    if (d->s) {
        memcpy(d->s, s->s, s->slen);
        d->s[s->slen] = 0;
    }
    if (s->n) {
        d->it = calloc((size_t)s->n, sizeof(struct pv));
        d->key = s->key ? calloc((size_t)s->n, sizeof(char *)) : NULL;
        for (int i = 0; i < s->n; i++) {
            pv_clone(&s->it[i], &d->it[i]);
            if (s->key)
                d->key[i] = strdup(s->key[i]);
        }
    } else {
        d->it = NULL;
        d->key = NULL;
    }
}

static void walk_pages(struct pdf *pdf, int num, const struct pv *inherited, struct page **pages, int *np, int *cap, int depth, int *visited)
{
    if (depth > 24 || *np >= 400 || ++*visited > 5000)
        return;
    struct pv node = { 0 };
    if (!obj_value(pdf, num, &node)) {
        return;
    }
    if (node.t == T_DICT) {
        struct pv res = { 0 };
        bool have = false;
        const struct pv *r = dict_get(&node, "Resources");
        if (r) {
            struct pv tmp = { 0 };
            const struct pv *rv = resolve(pdf, r, &tmp, 0);
            if (rv && rv->t == T_DICT) {
                pv_clone(rv, &res);
                have = true;
            }
            pv_free(&tmp);
        } else if (inherited && inherited->t == T_DICT) {
            pv_clone(inherited, &res);
            have = true;
        }
        const struct pv *kids = dict_get(&node, "Kids");
        struct pv tk = { 0 };
        const struct pv *kv = kids ? resolve(pdf, kids, &tk, 0) : NULL;
        if (kv && kv->t == T_ARR) {
            for (int i = 0; i < kv->n; i++)
                if (kv->it[i].t == T_REF)
                    walk_pages(pdf, (int)kv->it[i].num, have ? &res : NULL, pages, np, cap, depth + 1, visited);
        } else {
            const struct pv *ty = dict_get(&node, "Type");
            if (!ty || ty->t != T_NAME || strcmp(ty->s, "Pages")) {
                if (*np == *cap) {
                    *cap = *cap ? *cap * 2 : 32;
                    *pages = realloc(*pages, (size_t)*cap * sizeof(struct page));
                }
                struct page *pg = &(*pages)[(*np)++];
                pg->num = num;
                pg->has_res = have;
                if (have)
                    pv_clone(&res, &pg->res);
                else
                    memset(&pg->res, 0, sizeof(pg->res));
            }
        }
        pv_free(&tk);
        pv_free(&res);
    }
    pv_free(&node);
}

static void page_text(struct pdf *pdf, struct interp *in, const struct page *pg)
{
    struct pv node = { 0 };
    if (!obj_value(pdf, pg->num, &node))
        return;
    struct pv tc = { 0 };
    const struct pv *c = dict_get(&node, "Contents");
    const struct pv *cv = c ? resolve(pdf, c, &tc, 0) : NULL;
    struct sbuf all = { 0 };
    /* the streams of the page, one after another */
    int refs[64], nr = 0;
    if (c && c->t == T_REF && cv && cv->t != T_ARR)
        refs[nr++] = (int)c->num;
    else if (cv && cv->t == T_ARR)
        for (int i = 0; i < cv->n && nr < 64; i++)
            if (cv->it[i].t == T_REF)
                refs[nr++] = (int)cv->it[i].num;
    for (int i = 0; i < nr && all.n < STREAM_CAP; i++) {
        size_t n = 0;
        uint8_t *data = stream_data(pdf, refs[i], &n, NULL, STREAM_CAP);
        if (data) {
            sb_add(&all, (const char *)data, n);
            sb_add(&all, "\n", 1);
            free(data);
        }
    }
    if (all.s)
        interpret(in, (const uint8_t *)all.s, all.n, pg->has_res ? &pg->res : NULL, (struct mat){ 1, 0, 0, 1, 0, 0 }, 0);
    free(all.s);
    pv_free(&tc);
    pv_free(&node);
}

static void pdf_date(const char *s, char *out, size_t cap)
{
    out[0] = 0;
    if (!strncmp(s, "D:", 2))
        s += 2;
    int y, mo = 1, d = 1, h = 0, mi = 0;
    if (sscanf(s, "%4d%2d%2d%2d%2d", &y, &mo, &d, &h, &mi) >= 1)
        snprintf(out, cap, "%04d-%02d-%02d %02d:%02d", y, mo, d, h, mi);
}

static void clean_text(struct sbuf *b)
{
    /* trailing blanks off each line, runs of blank lines and blanks made one */
    char *o = b->s;
    if (!o)
        return;
    size_t w = 0;
    int blank_lines = 0;
    size_t ls = 0;
    for (size_t i = 0; i <= b->n; i++) {
        if (i == b->n || b->s[i] == '\n') {
            size_t le = w;
            while (le > ls && o[le - 1] == ' ')
                le--;
            if (le == ls) {
                if (++blank_lines > 1 || ls == 0) {
                    w = ls;
                    continue;
                }
            } else {
                blank_lines = 0;
            }
            w = le;
            if (i < b->n)
                o[w++] = '\n';
            ls = w;
        } else if (b->s[i] == ' ' && w > ls && o[w - 1] == ' ') {
            continue;
        } else if (b->s[i] == '\t' || b->s[i] == '\r' || (unsigned char)b->s[i] < 0x20) {
            if (w > ls && o[w - 1] != ' ')
                o[w++] = ' ';
        } else {
            o[w++] = b->s[i];
        }
    }
    o[w] = 0;
    b->n = w;
}

int pdf_text(const char *path, int max_pages, size_t max_chars, struct pdf_info *info, char **text, char *err, size_t errn)
{
    memset(info, 0, sizeof(*info));
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 20 || (uint64_t)st.st_size > PDF_MAX_FILE) {
        close(fd);
        return -1;
    }
    struct pdf pdf = { 0 };
    pdf.n = (size_t)st.st_size;
    pdf.d = malloc(pdf.n + 1);
    ssize_t got = pdf.d ? read(fd, pdf.d, pdf.n) : -1;
    close(fd);
    if (got != (ssize_t)pdf.n) {
        free(pdf.d);
        return -1;
    }
    pdf.d[pdf.n] = 0;
    const uint8_t *hdr = memmem(pdf.d, pdf.n < 1024 ? pdf.n : 1024, "%PDF-", 5);
    if (!hdr) {
        free(pdf.d);
        return -1;
    }
    snprintf(info->version, sizeof(info->version), "%.3s", (const char *)hdr + 5);
    int result = -2;
    int objstms[512], nobjstm = 0;
    struct page *pages = NULL;
    int np = 0, cap = 0;
    struct interp in = { 0 };
    struct sbuf out = { 0 };
    scan_objects(&pdf, objstms, &nobjstm, 512);
    trailers(&pdf);
    if (pdf.encrypted) {
        seterr(err, errn, "An encrypted PDF: its text is not shown.");
        goto done;
    }
    for (int i = 0; i < nobjstm; i++)
        expand_objstm(&pdf, objstms[i]);
    /* the pages */
    int visited = 0;
    if (pdf.root) {
        struct pv cat = { 0 };
        if (obj_value(&pdf, pdf.root, &cat) && cat.t == T_DICT) {
            const struct pv *pr = dict_get(&cat, "Pages");
            if (pr && pr->t == T_REF)
                walk_pages(&pdf, (int)pr->num, NULL, &pages, &np, &cap, 0, &visited);
        }
        pv_free(&cat);
    }
    if (np == 0) {                                                      /* no usable catalogue: every /Type /Page object in order */
        for (int i = 1; i < pdf.nobj && np < 400; i++)
            if (pdf.o[i].used) {
                struct pv v = { 0 };
                if (obj_value(&pdf, i, &v) && v.t == T_DICT) {
                    const struct pv *ty = dict_get(&v, "Type");
                    if (ty && ty->t == T_NAME && !strcmp(ty->s, "Page")) {
                        if (np == cap) {
                            cap = cap ? cap * 2 : 32;
                            pages = realloc(pages, (size_t)cap * sizeof(struct page));
                        }
                        pages[np].num = i;
                        pages[np].has_res = false;
                        memset(&pages[np].res, 0, sizeof(struct pv));
                        np++;
                    }
                }
                pv_free(&v);
            }
    }
    if (np == 0) {
        seterr(err, errn, "A PDF in which no pages were found.");
        goto done;
    }
    info->pages = np;
    if (pdf.root) {                                                     /* /Count of the page tree, when it is bigger than what was walked */
        struct pv cat = { 0 }, tp = { 0 };
        if (obj_value(&pdf, pdf.root, &cat) && cat.t == T_DICT) {
            const struct pv *pr = dict_get(&cat, "Pages");
            const struct pv *pn = pr ? resolve(&pdf, pr, &tp, 0) : NULL;
            const struct pv *cn = pn && pn->t == T_DICT ? dict_get(pn, "Count") : NULL;
            if (cn && cn->t == T_INT && cn->num > np && cn->num < 1000000)
                info->pages = (int)cn->num;
            pv_free(&tp);
        }
        pv_free(&cat);
    }
    if (pdf.info) {
        struct pv iv = { 0 };
        if (obj_value(&pdf, pdf.info, &iv) && iv.t == T_DICT) {
            struct pv t1 = { 0 };
            text_string(dget_r(&pdf, &iv, "Title", &t1), info->title, sizeof(info->title));
            pv_free(&t1);
            text_string(dget_r(&pdf, &iv, "Author", &t1), info->author, sizeof(info->author));
            pv_free(&t1);
            text_string(dget_r(&pdf, &iv, "Subject", &t1), info->subject, sizeof(info->subject));
            pv_free(&t1);
            text_string(dget_r(&pdf, &iv, "Producer", &t1), info->producer, sizeof(info->producer));
            pv_free(&t1);
            char d[64];
            text_string(dget_r(&pdf, &iv, "CreationDate", &t1), d, sizeof(d));
            pdf_date(d, info->created, sizeof(info->created));
            pv_free(&t1);
        }
        pv_free(&iv);
    }
    in.pdf = &pdf;
    in.fonts = calloc(MAX_FONTS, sizeof(struct font));
    int total_missing = 0, total_shown = 0;
    for (int i = 0; i < np && i < max_pages && out.n < max_chars; i++) {
        struct sbuf page = { 0 };
        in.out = &page;
        in.limit = max_chars - out.n > 20000 ? 20000 : max_chars - out.n;
        in.ops = 0;
        in.missing = in.shown = 0;
        in.have_pos = in.pending_space = false;
        page_text(&pdf, &in, &pages[i]);
        clean_text(&page);
        char head[48];
        snprintf(head, sizeof(head), "%s=== Page %d ===\n", out.n ? "\n" : "", i + 1);
        sb_add(&out, head, strlen(head));
        if (page.n)
            sb_add(&out, page.s, page.n);
        else
            sb_add(&out, "(no text on this page)", 22);
        sb_add(&out, "\n", 1);
        total_missing += in.missing;
        total_shown += in.shown;
        info->pages_read++;
        free(page.s);
    }
    if (total_shown == 0 && total_missing > 0)
        sb_add(&out, "\n(the fonts of this PDF have no text map: its letters cannot be shown)\n", 70);
    *text = out.s ? out.s : strdup("");
    out.s = NULL;
    result = 0;
done:
    free(out.s);
    if (in.fonts) {
        for (int i = 0; i < in.nfonts; i++)
            font_free(&in.fonts[i]);
        free(in.fonts);
    }
    for (int i = 0; i < np; i++)
        pv_free(&pages[i].res);
    free(pages);
    for (int i = 0; i < pdf.nstm; i++)
        free(pdf.stm[i].data);
    free(pdf.o);
    free(pdf.d);
    return result;
}
