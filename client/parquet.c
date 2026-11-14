#include "parquet.h"

#include "../src/img.h"

#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_FOOTER (32u << 20)
#define MAX_PAGE (96u << 20)
#define MAX_COLS 256
#define MAX_SCHEMA 2048
#define MAX_RGS 4096
#define MAX_DICT 400000
#define MAX_CELL 200

/* ---- Thrift compact protocol ---- */

struct tr {
    const uint8_t *p, *e;
    bool bad;
};

static uint64_t tr_varint(struct tr *t)
{
    uint64_t v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (t->p >= t->e) { t->bad = true; return 0; }
        uint8_t b = *t->p++;
        v |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80))
            return v;
    }
    t->bad = true;
    return 0;
}

static int64_t tr_zz(struct tr *t)
{
    uint64_t v = tr_varint(t);
    return (int64_t)(v >> 1) ^ -(int64_t)(v & 1);
}

/* The next field of a struct: false at its end (or on damage). */
static bool tr_field(struct tr *t, int *last, int *id, int *ty)
{
    if (t->bad || t->p >= t->e) { t->bad = true; return false; }
    uint8_t b = *t->p++;
    if (!b)
        return false;
    *ty = b & 15;
    int d = b >> 4;
    *id = d ? *last + d : (int)tr_zz(t);
    *last = *id;
    return !t->bad;
}

static void tr_skip(struct tr *t, int ty, int depth);

static int tr_list(struct tr *t, int *et)
{
    if (t->p >= t->e) { t->bad = true; return 0; }
    uint8_t b = *t->p++;
    int n = b >> 4;
    *et = b & 15;
    if (n == 15)
        n = (int)tr_varint(t);
    if (n < 0 || n > 4000000) { t->bad = true; return 0; }
    return n;
}

static const uint8_t *tr_bin(struct tr *t, size_t *n)
{
    uint64_t l = tr_varint(t);
    if (t->bad || l > (uint64_t)(t->e - t->p)) { t->bad = true; *n = 0; return NULL; }
    const uint8_t *r = t->p;
    t->p += l;
    *n = (size_t)l;
    return r;
}

static void tr_skip(struct tr *t, int ty, int depth)
{
    if (t->bad || depth > 24) { t->bad = true; return; }
    switch (ty) {
    case 1: case 2: break;
    case 3: if (t->p >= t->e) t->bad = true; else t->p++; break;
    case 4: case 5: case 6: tr_varint(t); break;
    case 7: if (t->e - t->p < 8) t->bad = true; else t->p += 8; break;
    case 8: { size_t n; tr_bin(t, &n); break; }
    case 9: case 10: {
        int et, n = tr_list(t, &et);
        for (int i = 0; i < n && !t->bad; i++)
            tr_skip(t, et == 1 || et == 2 ? 3 : et, depth + 1);
        break;
    }
    case 11: {
        uint64_t n = tr_varint(t);
        if (n && t->p < t->e) {
            uint8_t kv = *t->p++;
            for (uint64_t i = 0; i < n && !t->bad; i++) {
                tr_skip(t, kv >> 4, depth + 1);
                tr_skip(t, kv & 15, depth + 1);
            }
        }
        break;
    }
    case 12: {
        int last = 0, id, fty;
        while (tr_field(t, &last, &id, &fty))
            tr_skip(t, fty, depth + 1);
        break;
    }
    default: t->bad = true;
    }
}

/* ---- the file's metadata ---- */

enum { T_BOOL, T_I32, T_I64, T_I96, T_FLOAT, T_DOUBLE, T_BYTES, T_FIXED };
enum { L_NONE, L_STRING, L_DATE, L_TS_MS, L_TS_US, L_TS_NS };

struct se {                             /* a schema element */
    int type, tlen, rep, nchild, conv, logical;
    char name[96];
};

struct chunk {
    int type, codec;
    long long nvals, data_off, dict_off;
};

struct rg {
    long long nrows;
    int ncols;
    struct chunk col[MAX_COLS];
};

struct leaf {
    int se, max_def, max_rep;
    char name[160];
};

static void parse_logical(struct tr *t, struct se *s)
{
    int last = 0, id, ty;
    while (tr_field(t, &last, &id, &ty)) {
        if (id == 1) s->logical = L_STRING;
        else if (id == 6) s->logical = L_DATE;
        else if (id == 8 && ty == 12) {                                 /* TIMESTAMP { isAdjustedToUTC, unit } */
            int l2 = 0, i2, t2;
            while (tr_field(t, &l2, &i2, &t2)) {
                if (i2 == 2 && t2 == 12) {
                    int l3 = 0, i3, t3;
                    while (tr_field(t, &l3, &i3, &t3)) {
                        s->logical = i3 == 1 ? L_TS_MS : i3 == 2 ? L_TS_US : L_TS_NS;
                        tr_skip(t, t3, 0);
                    }
                } else {
                    tr_skip(t, t2, 0);
                }
            }
        } else tr_skip(t, ty, 0);
    }
}

static void parse_schema_el(struct tr *t, struct se *s)
{
    memset(s, 0, sizeof(*s));
    s->type = -1;
    int last = 0, id, ty;
    while (tr_field(t, &last, &id, &ty)) {
        switch (id) {
        case 1: s->type = (int)tr_zz(t); break;
        case 2: s->tlen = (int)tr_zz(t); break;
        case 3: s->rep = (int)tr_zz(t); break;
        case 4: { size_t n; const uint8_t *b = tr_bin(t, &n); if (b) snprintf(s->name, sizeof(s->name), "%.*s", (int)(n < sizeof(s->name) - 1 ? n : sizeof(s->name) - 1), b); break; }
        case 5: s->nchild = (int)tr_zz(t); break;
        case 6: s->conv = (int)tr_zz(t); break;
        case 10: if (ty == 12) parse_logical(t, s); else tr_skip(t, ty, 0); break;
        default: tr_skip(t, ty, 0);
        }
    }
}

static void parse_chunk_meta(struct tr *t, struct chunk *c)
{
    int last = 0, id, ty;
    while (tr_field(t, &last, &id, &ty)) {
        switch (id) {
        case 1: c->type = (int)tr_zz(t); break;
        case 4: c->codec = (int)tr_zz(t); break;
        case 5: c->nvals = tr_zz(t); break;
        case 9: c->data_off = tr_zz(t); break;
        case 11: c->dict_off = tr_zz(t); break;
        default: tr_skip(t, ty, 0);
        }
    }
}

static void parse_rg(struct tr *t, struct rg *g)
{
    memset(g, 0, sizeof(*g));
    int last = 0, id, ty;
    while (tr_field(t, &last, &id, &ty)) {
        if (id == 1 && ty == 9) {
            int et, n = tr_list(t, &et);
            for (int i = 0; i < n && !t->bad; i++) {
                struct chunk c = { 0 };
                int l2 = 0, i2, t2;
                while (tr_field(t, &l2, &i2, &t2)) {                    /* ColumnChunk */
                    if (i2 == 3 && t2 == 12) parse_chunk_meta(t, &c);
                    else tr_skip(t, t2, 0);
                }
                if (g->ncols < MAX_COLS) g->col[g->ncols++] = c;
            }
        } else if (id == 3) {
            g->nrows = tr_zz(t);
        } else tr_skip(t, ty, 0);
    }
}

/* ---- snappy ---- */

long pq_snappy(const unsigned char *in, size_t inlen, unsigned char *out, size_t outcap)
{
    const unsigned char *p = in, *e = in + inlen;
    uint64_t want = 0;
    int shift = 0;
    for (;;) {
        if (p >= e || shift > 35) return -1;
        unsigned char b = *p++;
        want |= (uint64_t)(b & 0x7f) << shift;
        shift += 7;
        if (!(b & 0x80)) break;
    }
    if (want > outcap) return -1;
    size_t o = 0;
    while (p < e) {
        unsigned tag = *p++;
        size_t len, off;
        if ((tag & 3) == 0) {                                           /* a literal */
            len = tag >> 2;
            if (len >= 60) {
                size_t nb = len - 59;
                if ((size_t)(e - p) < nb) return -1;
                len = 0;
                for (size_t i = 0; i < nb; i++) len |= (size_t)p[i] << (8 * i);
                p += nb;
            }
            len++;
            if ((size_t)(e - p) < len || o + len > want) return -1;
            memcpy(out + o, p, len);
            p += len;
            o += len;
            continue;
        }
        if ((tag & 3) == 1) {
            if (p >= e) return -1;
            len = 4 + ((tag >> 2) & 7);
            off = ((size_t)(tag >> 5) << 8) | *p++;
        } else if ((tag & 3) == 2) {
            if (e - p < 2) return -1;
            len = 1 + (tag >> 2);
            off = p[0] | ((size_t)p[1] << 8);
            p += 2;
        } else {
            if (e - p < 4) return -1;
            len = 1 + (tag >> 2);
            off = p[0] | ((size_t)p[1] << 8) | ((size_t)p[2] << 16) | ((size_t)p[3] << 24);
            p += 4;
        }
        if (off == 0 || off > o || o + len > want) return -1;
        for (size_t i = 0; i < len; i++, o++)                           /* (may overlap: byte by byte) */
            out[o] = out[o - off];
    }
    return o == want ? (long)o : -1;
}

/* A page body made whole: `out` of exactly `outlen` bytes. */
static uint8_t *decompress(int codec, const uint8_t *in, size_t inlen, size_t outlen)
{
    uint8_t *out = malloc(outlen ? outlen : 1);
    if (!out) return NULL;
    if (codec == 0) {
        if (inlen < outlen) { free(out); return NULL; }
        memcpy(out, in, outlen);
        return out;
    }
    if (codec == 1) {
        if (pq_snappy(in, inlen, out, outlen) != (long)outlen) { free(out); return NULL; }
        return out;
    }
    if (codec == 2) {                                                   /* gzip: a header, a raw deflate stream, a trailer */
        if (inlen < 18 || in[0] != 0x1f || in[1] != 0x8b || in[2] != 8) { free(out); return NULL; }
        size_t i = 10;
        unsigned flg = in[3];
        if (flg & 4) { if (i + 2 > inlen) { free(out); return NULL; } i += 2 + (in[i] | (in[i + 1] << 8)); }
        if (flg & 8) { while (i < inlen && in[i]) i++; i++; }
        if (flg & 16) { while (i < inlen && in[i]) i++; i++; }
        if (flg & 2) i += 2;
        if (i >= inlen || lm_inflate_raw(in + i, inlen - i, out, outlen) < 0) { free(out); return NULL; }
        return out;
    }
    free(out);
    return NULL;
}

/* ---- values ---- */

static uint32_t le32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }

static void civil(int64_t z, int *y, int *m, int *d)                    /* days since 1970-01-01 */
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yy = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yy + (*m <= 2));
}

static char *fmt_date(int64_t days)
{
    char b[32];
    int y, m, d;
    civil(days, &y, &m, &d);
    snprintf(b, sizeof(b), "%04d-%02d-%02d", y, m, d);
    return strdup(b);
}

/* seconds since the epoch and a fraction of `fd` digits */
static char *fmt_ts(int64_t sec, int64_t frac, int fd)
{
    int64_t days = sec / 86400, rem = sec % 86400;
    if (rem < 0) { rem += 86400; days--; }
    int y, m, d;
    civil(days, &y, &m, &d);
    char b[64];
    int n = snprintf(b, sizeof(b), "%04d-%02d-%02d %02d:%02d:%02d", y, m, d, (int)(rem / 3600), (int)(rem % 3600 / 60), (int)(rem % 60));
    if (fd) snprintf(b + n, sizeof(b) - (size_t)n, ".%0*lld", fd, (long long)frac);
    return strdup(b);
}

static char *fmt_text(const uint8_t *p, size_t n, bool as_text)
{
    if (!as_text) {                                                     /* bytes that are no declared string: text when they look like one */
        size_t k = n < 64 ? n : 64;
        for (size_t i = 0; i < k; i++)
            if (p[i] < 0x20 && p[i] != '\t' && p[i] != '\n') {
                char b[40];
                snprintf(b, sizeof(b), "<binary %zu>", n);
                return strdup(b);
            }
    }
    size_t k = n < MAX_CELL ? n : MAX_CELL;
    char *s = malloc(k + 1);
    if (!s) return NULL;
    for (size_t i = 0; i < k; i++)
        s[i] = p[i] < 0x20 ? ' ' : (char)p[i];
    s[k] = 0;
    return s;
}

/* One PLAIN value of the column's type at p (len bytes left): the text, and how many bytes it took (0: the data is short). */
static char *plain_value(const struct se *s, int type, const uint8_t *p, size_t len, size_t *used)
{
    char b[64];
    *used = 0;
    switch (type) {
    case T_I32:
        if (len < 4) return NULL;
        *used = 4;
        if (s->logical == L_DATE || s->conv == 6) return fmt_date((int32_t)le32(p));
        if (s->conv >= 11 && s->conv <= 13) snprintf(b, sizeof(b), "%u", le32(p));
        else snprintf(b, sizeof(b), "%d", (int32_t)le32(p));
        return strdup(b);
    case T_I64: {
        if (len < 8) return NULL;
        *used = 8;
        int64_t v = (int64_t)le64(p);
        if (s->logical == L_TS_MS || s->conv == 9) return fmt_ts(v >= 0 ? v / 1000 : -((-v + 999) / 1000), ((v % 1000) + 1000) % 1000, 3);
        if (s->logical == L_TS_US || s->conv == 10) return fmt_ts(v >= 0 ? v / 1000000 : -((-v + 999999) / 1000000), ((v % 1000000) + 1000000) % 1000000, 6);
        if (s->logical == L_TS_NS) return fmt_ts(v >= 0 ? v / 1000000000 : -((-v + 999999999) / 1000000000), ((v % 1000000000) + 1000000000) % 1000000000, 9);
        if (s->conv == 14) snprintf(b, sizeof(b), "%llu", (unsigned long long)v);
        else snprintf(b, sizeof(b), "%lld", (long long)v);
        return strdup(b);
    }
    case T_I96: {                                                       /* nanoseconds in the day, the Julian day */
        if (len < 12) return NULL;
        *used = 12;
        int64_t nanos = (int64_t)le64(p), jd = (int32_t)le32(p + 8);
        return fmt_ts((jd - 2440588) * 86400 + nanos / 1000000000, nanos % 1000000000 / 1000000, 3);
    }
    case T_FLOAT: {
        if (len < 4) return NULL;
        *used = 4;
        uint32_t u = le32(p);
        float f;
        memcpy(&f, &u, 4);
        snprintf(b, sizeof(b), "%.9g", (double)f);
        return strdup(b);
    }
    case T_DOUBLE: {
        if (len < 8) return NULL;
        *used = 8;
        uint64_t u = le64(p);
        double f;
        memcpy(&f, &u, 8);
        snprintf(b, sizeof(b), "%.15g", f);
        return strdup(b);
    }
    case T_BYTES: {
        if (len < 4) return NULL;
        uint32_t n = le32(p);
        if (n > len - 4) return NULL;
        *used = 4 + (size_t)n;
        return fmt_text(p + 4, n, s->logical == L_STRING || s->conv == 0 || s->conv == 4 || s->conv == 19);
    }
    case T_FIXED: {
        size_t n = (size_t)s->tlen;
        if (n == 0 || n > len) return NULL;
        *used = n;
        char hex[2 * 24 + 8];
        size_t k = n < 24 ? n : 24, o = 0;
        for (size_t i = 0; i < k; i++) o += (size_t)snprintf(hex + o, sizeof(hex) - o, "%02x", p[i]);
        return strdup(hex);
    }
    default: return NULL;
    }
}

/* The RLE / bit-packed hybrid: `count` values of `bw` bits into out; the bytes used, -1 on damage. */
static long rle_hybrid(const uint8_t *p, size_t len, int bw, size_t count, uint32_t *out)
{
    const uint8_t *s = p, *e = p + len;
    size_t n = 0;
    if (bw < 0 || bw > 32) return -1;
    while (n < count) {
        uint64_t h = 0;
        int shift = 0;
        for (;;) {
            if (p >= e || shift > 35) return -1;
            uint8_t b = *p++;
            h |= (uint64_t)(b & 0x7f) << shift;
            shift += 7;
            if (!(b & 0x80)) break;
        }
        if (h & 1) {                                                    /* bit-packed groups of 8 */
            size_t groups = (size_t)(h >> 1), bytes = groups * (size_t)bw;
            if (groups > (1u << 24) || bytes > (size_t)(e - p)) return -1;
            size_t cnt = groups * 8, bitpos = 0;
            for (size_t i = 0; i < cnt && n < count; i++, n++) {
                uint32_t v = 0;
                for (int k = 0; k < bw; k++, bitpos++)
                    v |= (uint32_t)((p[bitpos / 8] >> (bitpos % 8)) & 1) << k;
                out[n] = v;
            }
            p += bytes;
        } else {                                                        /* a run */
            size_t run = (size_t)(h >> 1), vb = ((size_t)bw + 7) / 8;
            if ((size_t)(e - p) < vb) return -1;
            uint32_t v = 0;
            for (size_t k = 0; k < vb; k++) v |= (uint32_t)p[k] << (8 * k);
            p += vb;
            for (size_t i = 0; i < run && n < count; i++) out[n++] = v;
        }
    }
    return (long)(p - s);
}

static int bits_for(int maxv)
{
    int b = 0;
    while ((1 << b) <= maxv) b++;
    return b;
}

static void free_strs(char **v, size_t n)
{
    for (size_t i = 0; i < n; i++) free(v[i]);
    free(v);
}

/* ---- a column chunk ---- */

struct cc {
    int fd;
    long long fsize;
    const struct chunk *ch;
    const struct se *se;
    const struct leaf *lf;
    char **dict;
    size_t ndict;
    char **out;                         /* the cells of this column for the rows wanted */
    int need, filled;
    char err[120];
};

static uint8_t *read_at(int fd, long long off, size_t n)
{
    uint8_t *b = malloc(n ? n : 1);
    if (!b) return NULL;
    if (pread(fd, b, n, (off_t)off) != (ssize_t)n) { free(b); return NULL; }
    return b;
}

/* The values of one data page into the column's cells. */
static bool data_page(struct cc *c, const uint8_t *body, size_t blen, int nvals, int enc, bool v2, long rep_len, long def_len)
{
    const struct leaf *L = c->lf;
    const uint8_t *p = body, *e = body + blen;
    uint32_t *def = NULL;
    bool ok = false;
    char **vals = NULL;
    size_t nn = 0;
    if (nvals < 0 || nvals > (1 << 26)) return false;
    if (!v2) {
        if (L->max_rep > 0) {
            if (e - p < 4) return false;
            size_t l = le32(p);
            if (l > (size_t)(e - p - 4)) return false;
            p += 4 + l;
        }
    } else {
        p += rep_len;
    }
    def = calloc((size_t)nvals + 1, sizeof(uint32_t));
    if (!def) return false;
    if (L->max_def > 0) {
        if (!v2) {
            if (e - p < 4) goto done;
            size_t l = le32(p);
            if (l > (size_t)(e - p - 4)) goto done;
            if (rle_hybrid(p + 4, l, bits_for(L->max_def), (size_t)nvals, def) < 0) goto done;
            p += 4 + l;
        } else {
            if (def_len < 0 || def_len > e - p) goto done;
            if (rle_hybrid(p, (size_t)def_len, bits_for(L->max_def), (size_t)nvals, def) < 0) goto done;
            p += def_len;
        }
        for (int i = 0; i < nvals; i++) nn += def[i] == (uint32_t)L->max_def;
    } else {
        nn = (size_t)nvals;
    }
    size_t left = (size_t)(e - p);
    vals = calloc(nn ? nn : 1, sizeof(char *));
    if (!vals) goto done;
    if (enc == 2 || enc == 8) {                                         /* dictionary indices */
        if (!c->dict || left < 1) goto done;
        int bw = *p++;
        left--;
        uint32_t *idx = malloc((nn ? nn : 1) * sizeof(uint32_t));
        if (!idx) goto done;
        if (rle_hybrid(p, left, bw, nn, idx) < 0) { free(idx); goto done; }
        for (size_t i = 0; i < nn; i++)
            vals[i] = idx[i] < c->ndict && c->dict[idx[i]] ? strdup(c->dict[idx[i]]) : strdup("?");
        free(idx);
    } else if (enc == 0) {                                              /* PLAIN */
        if (c->ch->type == T_BOOL) {
            if (nn / 8 > left + 1) goto done;
            for (size_t i = 0; i < nn; i++)
                vals[i] = strdup(((p[i / 8] >> (i % 8)) & 1) ? "true" : "false");
        } else {
            for (size_t i = 0; i < nn; i++) {
                size_t used;
                vals[i] = plain_value(c->se, c->ch->type, p, left, &used);
                if (!vals[i]) goto done;
                p += used;
                left -= used;
            }
        }
    } else if (enc == 3 && c->ch->type == T_BOOL) {                     /* RLE booleans: a length, then the hybrid */
        if (left < 4) goto done;
        size_t l = le32(p);
        if (l > left - 4) goto done;
        uint32_t *bv = malloc((nn ? nn : 1) * sizeof(uint32_t));
        if (!bv) goto done;
        if (rle_hybrid(p + 4, l, 1, nn, bv) < 0) { free(bv); goto done; }
        for (size_t i = 0; i < nn; i++) vals[i] = strdup(bv[i] ? "true" : "false");
        free(bv);
    } else {
        snprintf(c->err, sizeof(c->err), "an encoding (%d) that is not read", enc);
        goto done;
    }
    size_t k = 0;
    for (int i = 0; i < nvals && c->filled < c->need; i++) {
        bool present = L->max_def == 0 || def[i] == (uint32_t)L->max_def;
        c->out[c->filled++] = present ? vals[k++] : strdup("");
        if (present) vals[k - 1] = NULL;
    }
    ok = true;
done:
    if (vals) free_strs(vals, nn);
    free(def);
    return ok;
}

/* Reads a column chunk; NULL = ok, else the reason. */
static const char *read_chunk(struct cc *c)
{
    const struct chunk *ch = c->ch;
    if (ch->codec != 0 && ch->codec != 1 && ch->codec != 2) {
        static const char *const names[] = { "", "", "", "LZO", "Brotli", "LZ4", "ZSTD", "LZ4" };
        snprintf(c->err, sizeof(c->err), "compressed with %s: not read", ch->codec >= 0 && ch->codec < 8 ? names[ch->codec] : "an unknown codec");
        return c->err;
    }
    long long off = ch->dict_off > 0 && ch->dict_off < ch->data_off ? ch->dict_off : ch->data_off;
    long long seen = 0;
    int guard = 0;
    while (seen < ch->nvals && c->filled < c->need && off > 0 && off < c->fsize && guard++ < 100000) {
        size_t hn = (size_t)(c->fsize - off < 16384 ? c->fsize - off : 16384);
        uint8_t *hb = read_at(c->fd, off, hn);
        if (!hb) { snprintf(c->err, sizeof(c->err), "a page cannot be read"); return c->err; }
        struct tr t = { hb, hb + hn, false };
        long long type = -1, usize = 0, csize = 0;
        long long nvals = 0, enc = 0, rep_len = 0, def_len = 0, num_dict = 0;
        bool compressed = true;
        int last = 0, id, ty;
        while (tr_field(&t, &last, &id, &ty)) {
            if (id == 1) type = tr_zz(&t);
            else if (id == 2) usize = tr_zz(&t);
            else if (id == 3) csize = tr_zz(&t);
            else if ((id == 5 || id == 8) && ty == 12) {
                int l2 = 0, i2, t2;
                while (tr_field(&t, &l2, &i2, &t2)) {
                    if (id == 5) { if (i2 == 1) nvals = tr_zz(&t); else if (i2 == 2) enc = tr_zz(&t); else tr_skip(&t, t2, 0); }
                    else if (i2 == 1) nvals = tr_zz(&t);
                    else if (i2 == 4) enc = tr_zz(&t);
                    else if (i2 == 5) def_len = tr_zz(&t);
                    else if (i2 == 6) rep_len = tr_zz(&t);
                    else if (i2 == 7) compressed = t2 == 1;
                    else tr_skip(&t, t2, 0);
                }
            } else if (id == 7 && ty == 12) {
                int l2 = 0, i2, t2;
                while (tr_field(&t, &l2, &i2, &t2)) {
                    if (i2 == 1) num_dict = tr_zz(&t); else if (i2 == 2) enc = tr_zz(&t); else tr_skip(&t, t2, 0);
                }
            } else tr_skip(&t, ty, 0);
        }
        size_t hl = (size_t)(t.p - hb);
        bool bad = t.bad || usize < 0 || csize < 0 || (unsigned long long)usize > MAX_PAGE || (unsigned long long)csize > MAX_PAGE;
        free(hb);
        if (bad) { snprintf(c->err, sizeof(c->err), "a page header is damaged"); return c->err; }
        uint8_t *raw = read_at(c->fd, off + (long long)hl, (size_t)csize);
        off += (long long)hl + csize;
        if (!raw) { snprintf(c->err, sizeof(c->err), "a page is cut off"); return c->err; }
        if (type == 2 || type == 0) {                                   /* a dictionary page / a v1 data page: the whole body is compressed */
            uint8_t *body = decompress(ch->codec, raw, (size_t)csize, (size_t)usize);
            free(raw);
            if (!body) { snprintf(c->err, sizeof(c->err), "a page does not decompress"); return c->err; }
            if (type == 2) {
                if (num_dict < 0 || num_dict > MAX_DICT) { free(body); snprintf(c->err, sizeof(c->err), "a dictionary that is too big"); return c->err; }
                if (c->dict) {                                          /* (a second dictionary page replaces the first) */
                    free_strs(c->dict, c->ndict);
                    c->dict = NULL;
                    c->ndict = 0;
                }
                c->dict = calloc((size_t)num_dict + 1, sizeof(char *));
                const uint8_t *p = body;
                size_t left = (size_t)usize;
                bool okd = c->dict != NULL && ch->type != T_BOOL;
                for (long long i = 0; okd && i < num_dict; i++) {
                    size_t used;
                    c->dict[i] = plain_value(c->se, ch->type, p, left, &used);
                    if (!c->dict[i]) okd = false;
                    p += used;
                    left -= used;
                }
                c->ndict = (size_t)num_dict;
                free(body);
                if (!okd) { snprintf(c->err, sizeof(c->err), "a dictionary that cannot be read"); return c->err; }
            } else {
                bool okp = data_page(c, body, (size_t)usize, (int)nvals, (int)enc, false, 0, 0);
                free(body);
                if (!okp) { if (!c->err[0]) snprintf(c->err, sizeof(c->err), "a data page cannot be read"); return c->err; }
                seen += nvals;
            }
        } else if (type == 3) {                                         /* v2: the levels are outside the compression */
            if (rep_len < 0 || def_len < 0 || rep_len + def_len > csize || rep_len + def_len > usize) { free(raw); snprintf(c->err, sizeof(c->err), "a page header is damaged"); return c->err; }
            size_t lv = (size_t)(rep_len + def_len), vlen = (size_t)usize - lv;
            uint8_t *body = malloc((size_t)usize ? (size_t)usize : 1);
            bool okp = body != NULL;
            if (okp) {
                memcpy(body, raw, lv);
                if (compressed && ch->codec != 0) {
                    uint8_t *v = decompress(ch->codec, raw + lv, (size_t)csize - lv, vlen);
                    if (!v) okp = false; else { memcpy(body + lv, v, vlen); free(v); }
                } else if ((size_t)csize - lv >= vlen) {
                    memcpy(body + lv, raw + lv, vlen);
                } else okp = false;
            }
            free(raw);
            if (okp) okp = data_page(c, body, (size_t)usize, (int)nvals, (int)enc, true, (long)rep_len, (long)def_len);
            free(body);
            if (!okp) { if (!c->err[0]) snprintf(c->err, sizeof(c->err), "a data page cannot be read"); return c->err; }
            seen += nvals;
        } else {
            free(raw);                                                  /* an index page: nothing */
        }
    }
    return NULL;
}

/* ---- the whole file ---- */

static int walk(const struct se *s, int n, int i, int def, int rep, const char *path, struct leaf *lv, int *nl)
{
    if (i >= n) return n;
    const struct se *e = &s[i];
    char p[160];
    if (i == 0) snprintf(p, sizeof(p), "%s", path);
    else snprintf(p, sizeof(p), "%s%s%s", path, path[0] ? "." : "", e->name);
    if (i > 0) {
        if (e->rep == 1) def++;
        else if (e->rep == 2) { def++; rep++; }
    }
    if (e->nchild > 0) {
        int j = i + 1;
        for (int k = 0; k < e->nchild && j < n; k++)
            j = walk(s, n, j, def, rep, p, lv, nl);
        return j;
    }
    if (*nl < MAX_COLS) {
        lv[*nl] = (struct leaf){ i, def, rep, "" };
        snprintf(lv[*nl].name, sizeof(lv[*nl].name), "%s", p);
        (*nl)++;
    }
    return i + 1;
}

int pq_read(const char *path, int max_rows, struct pq_table *t, char *err, size_t errn)
{
    memset(t, 0, sizeof(*t));
    err[0] = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    off_t fsz = lseek(fd, 0, SEEK_END);
    uint8_t head[4], tail[8];
    if (fsz < 12 || pread(fd, head, 4, 0) != 4 || pread(fd, tail, 8, fsz - 8) != 8 || memcmp(head, "PAR1", 4) || memcmp(tail + 4, "PAR1", 4)) {
        close(fd);
        return -1;
    }
    uint32_t fl = le32(tail);
    if (fl > MAX_FOOTER || (off_t)fl + 8 > fsz) {
        close(fd);
        snprintf(err, errn, "the footer of the file is damaged");
        return -2;
    }
    uint8_t *foot = read_at(fd, fsz - 8 - (off_t)fl, fl);
    if (!foot) { close(fd); snprintf(err, errn, "the footer cannot be read"); return -2; }
    struct tr tr = { foot, foot + fl, false };
    struct se *schema = malloc(MAX_SCHEMA * sizeof(*schema));
    struct rg *rgs = malloc(sizeof(*rgs));
    int nschema = 0;
    long long total = 0;
    int rc = -2;
    struct leaf leaves[MAX_COLS];
    int nl = 0;
    t->cell = NULL;
    if (!schema || !rgs) goto out;
    {   /* the metadata: the schema first, the row groups (only the first ones are kept, one at a time: re-read below) */
        int last = 0, id, ty;
        const uint8_t *rg_pos[MAX_RGS];
        int rg_n = 0;
        while (tr_field(&tr, &last, &id, &ty)) {
            if (id == 2 && ty == 9) {
                int et, n = tr_list(&tr, &et);
                for (int i = 0; i < n && !tr.bad; i++) {
                    struct se s;
                    parse_schema_el(&tr, &s);
                    if (nschema < MAX_SCHEMA) schema[nschema++] = s;
                }
            } else if (id == 3) {
                total = tr_zz(&tr);
            } else if (id == 4 && ty == 9) {
                int et, n = tr_list(&tr, &et);
                for (int i = 0; i < n && !tr.bad; i++) {
                    if (rg_n < MAX_RGS) rg_pos[rg_n++] = tr.p;
                    tr_skip(&tr, 12, 0);
                }
            } else if (id == 6) {
                size_t n;
                const uint8_t *b = tr_bin(&tr, &n);
                if (b) snprintf(t->created_by, sizeof(t->created_by), "%.*s", (int)(n < sizeof(t->created_by) - 1 ? n : sizeof(t->created_by) - 1), b);
            } else tr_skip(&tr, ty, 0);
        }
        if (tr.bad || nschema < 2) { snprintf(err, errn, "the metadata of the file is damaged"); goto out; }
        walk(schema, nschema, 0, 0, 0, "", leaves, &nl);
        t->total_rows = total;
        t->row_groups = rg_n;
        t->ncols = nl;
        t->name = calloc((size_t)nl, sizeof(char *));
        for (int c = 0; c < nl; c++) t->name[c] = strdup(leaves[c].name);
        t->cell = calloc((size_t)(max_rows > 0 ? max_rows : 1), sizeof(char **));
        int rows = 0;
        for (int g = 0; g < rg_n && rows < max_rows; g++) {
            struct tr gt = { rg_pos[g], foot + fl, false };
            parse_rg(&gt, rgs);
            if (gt.bad || rgs->ncols != nl) { snprintf(err, errn, "a row group does not match the schema"); goto out; }
            int take = rgs->nrows < max_rows - rows ? (int)rgs->nrows : max_rows - rows;
            if (take <= 0) continue;
            char ***cols = calloc((size_t)nl, sizeof(char **));
            const char *why = NULL;
            for (int c = 0; c < nl && !why; c++) {
                cols[c] = calloc((size_t)take, sizeof(char *));
                if (leaves[c].max_rep > 0) {                            /* a list or a map: not unfolded */
                    for (int r = 0; r < take; r++) cols[c][r] = strdup("<list>");
                    continue;
                }
                struct cc cc = { fd, fsz, &rgs->col[c], &schema[leaves[c].se], &leaves[c], NULL, 0, cols[c], take, 0, "" };
                why = read_chunk(&cc);
                if (!why && cc.filled < take) why = "fewer values than rows";
                if (why) snprintf(err, errn, "column %s: %s", leaves[c].name, why);
                if (cc.dict) free_strs(cc.dict, cc.ndict);
            }
            if (why) {
                for (int c = 0; c < nl; c++) if (cols[c]) free_strs(cols[c], (size_t)take);
                free(cols);
                goto out;
            }
            for (int r = 0; r < take; r++) {
                t->cell[rows + r] = calloc((size_t)nl, sizeof(char *));
                for (int c = 0; c < nl; c++) {
                    t->cell[rows + r][c] = cols[c][r];
                    cols[c][r] = NULL;
                }
            }
            for (int c = 0; c < nl; c++) free(cols[c]);
            free(cols);
            rows += take;
            t->nrows = rows;
        }
        rc = 0;
    }
out:
    free(foot);
    free(schema);
    free(rgs);
    close(fd);
    if (rc != 0) pq_free(t);
    return rc;
}

void pq_free(struct pq_table *t)
{
    for (int c = 0; c < t->ncols; c++) free(t->name ? t->name[c] : NULL);
    free(t->name);
    for (int r = 0; r < t->nrows; r++) {
        for (int c = 0; c < t->ncols; c++) free(t->cell[r][c]);
        free(t->cell[r]);
    }
    free(t->cell);
    memset(t, 0, sizeof(*t));
}
