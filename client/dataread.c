#include "dataread.h"

#include "json.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define OUT_MAX (8u << 20)
#define MAX_DEPTH 48

/* ---- a growing string ---- */
struct sb {
    char *s;
    size_t n, cap;
    bool full;
};

static void sb_add(struct sb *b, const char *p, size_t n)
{
    if (b->n + n > OUT_MAX) {
        b->full = true;
        return;
    }
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

static void sb_puts(struct sb *b, const char *s)
{
    sb_add(b, s, strlen(s));
}

static void sb_printf(struct sb *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void sb_printf(struct sb *b, const char *fmt, ...)
{
    char t[1024];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    vsnprintf(t, sizeof(t), fmt, ap);
    __builtin_va_end(ap);
    sb_puts(b, t);
}

static void utf8_put(struct sb *b, unsigned cp)
{
    char t[4];
    size_t n;
    if (cp < 0x80) { t[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { t[0] = (char)(0xc0 | cp >> 6); t[1] = (char)(0x80 | (cp & 0x3f)); n = 2; }
    else if (cp < 0x10000) { t[0] = (char)(0xe0 | cp >> 12); t[1] = (char)(0x80 | ((cp >> 6) & 0x3f)); t[2] = (char)(0x80 | (cp & 0x3f)); n = 3; }
    else { t[0] = (char)(0xf0 | cp >> 18); t[1] = (char)(0x80 | ((cp >> 12) & 0x3f)); t[2] = (char)(0x80 | ((cp >> 6) & 0x3f)); t[3] = (char)(0x80 | (cp & 0x3f)); n = 4; }
    sb_add(b, t, n);
}

/* A JSON string; bytes that are no UTF-8 become ?. */
static void jstr_out(struct sb *b, const uint8_t *s, size_t n)
{
    sb_add(b, "\"", 1);
    for (size_t i = 0; i < n;) {
        unsigned char c = s[i];
        size_t k = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        bool ok = k > 0 && i + k <= n;
        for (size_t j = 1; ok && j < k; j++)
            ok = (s[i + j] & 0xc0) == 0x80;
        if (!ok) {
            sb_add(b, "?", 1);
            i++;
            continue;
        }
        if (c == '"' || c == '\\') {
            char t[2] = { '\\', (char)c };
            sb_add(b, t, 2);
        } else if (c < 0x20 || c == 0x7f) {
            char t[8];
            snprintf(t, sizeof(t), "\\u%04x", c);
            sb_puts(b, t);
        } else {
            sb_add(b, (const char *)s + i, k);
        }
        i += k;
    }
    sb_add(b, "\"", 1);
}

static void iso_time(double secs_unix, char *o, size_t cap)
{
    time_t t = (time_t)floor(secs_unix);
    struct tm tm;
    if (secs_unix < -62135596800.0 || secs_unix > 253402300799.0 || !gmtime_r(&t, &tm)) {
        snprintf(o, cap, "%g", secs_unix);
        return;
    }
    snprintf(o, cap, "%04d-%02d-%02d %02d:%02d:%02d UTC", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
}

/* ---- binary property lists ---- */
struct bp {
    const uint8_t *d;
    size_t n;
    size_t off_table, nobj, top;
    unsigned off_size, ref_size;
    long budget;
};

static uint64_t be_n(const uint8_t *p, unsigned n)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < n; i++)
        v = v << 8 | p[i];
    return v;
}

static bool bp_offset(const struct bp *b, uint64_t idx, size_t *off)
{
    if (idx >= b->nobj)
        return false;
    size_t o = b->off_table + (size_t)idx * b->off_size;
    if (o + b->off_size > b->n)
        return false;
    uint64_t v = be_n(b->d + o, b->off_size);
    if (v >= b->n)
        return false;
    *off = (size_t)v;
    return true;
}

/* The length field of a marker: the low nibble, or (for 0xF) an integer object after it. Moves *p. */
static bool bp_count(const struct bp *b, size_t *p, unsigned low, uint64_t *count)
{
    if (low != 15) {
        *count = low;
        return true;
    }
    if (*p >= b->n || (b->d[*p] >> 4) != 1)
        return false;
    unsigned sz = 1u << (b->d[*p] & 15);
    if (sz > 8 || *p + 1 + sz > b->n)
        return false;
    *count = be_n(b->d + *p + 1, sz);
    *p += 1 + sz;
    return true;
}

static bool bp_obj(struct bp *b, uint64_t idx, struct sb *o, int depth);

static bool bp_key_string(struct bp *b, uint64_t idx, struct sb *o, int depth)
{
    size_t off;
    if (!bp_offset(b, idx, &off))
        return false;
    unsigned m = b->d[off];
    if ((m >> 4) == 5 || (m >> 4) == 6)
        return bp_obj(b, idx, o, depth);
    struct sb t = { 0 };                                                /* a key that is no string: its JSON text as the key */
    bool ok = bp_obj(b, idx, &t, depth);
    if (ok && t.s)
        jstr_out(o, (const uint8_t *)t.s, t.n);
    free(t.s);
    return ok;
}

static bool bp_obj(struct bp *b, uint64_t idx, struct sb *o, int depth)
{
    size_t off;
    if (depth > MAX_DEPTH || --b->budget < 0 || !bp_offset(b, idx, &off))
        return false;
    unsigned m = b->d[off], hi = m >> 4, low = m & 15;
    size_t p = off + 1;
    uint64_t count;
    if (m == 0x00) { sb_puts(o, "null"); return true; }
    if (m == 0x08) { sb_puts(o, "false"); return true; }
    if (m == 0x09) { sb_puts(o, "true"); return true; }
    if (hi == 1) {                                                      /* an integer of 2^low bytes */
        unsigned sz = 1u << low;
        if (sz > 16 || p + sz > b->n)
            return false;
        if (sz == 8) {
            int64_t v = (int64_t)be_n(b->d + p, 8);
            sb_printf(o, "%lld", (long long)v);
        } else if (sz == 16) {
            sb_printf(o, "%llu", (unsigned long long)be_n(b->d + p + 8, 8));
        } else {
            sb_printf(o, "%llu", (unsigned long long)be_n(b->d + p, sz));
        }
        return true;
    }
    if (hi == 2) {                                                      /* a real: 4 or 8 bytes */
        unsigned sz = 1u << low;
        if ((sz != 4 && sz != 8) || p + sz > b->n)
            return false;
        double v;
        if (sz == 4) {
            uint32_t u = (uint32_t)be_n(b->d + p, 4);
            float f;
            memcpy(&f, &u, 4);
            v = f;
        } else {
            uint64_t u = be_n(b->d + p, 8);
            memcpy(&v, &u, 8);
        }
        if (!isfinite(v))
            sb_puts(o, "null");
        else
            sb_printf(o, "%.17g", v);
        return true;
    }
    if (hi == 3) {                                                      /* a date: seconds since 2001-01-01 */
        if (p + 8 > b->n)
            return false;
        uint64_t u = be_n(b->d + p, 8);
        double v;
        memcpy(&v, &u, 8);
        char t[48];
        iso_time(isfinite(v) ? v + 978307200.0 : 0, t, sizeof(t));
        jstr_out(o, (const uint8_t *)t, strlen(t));
        return true;
    }
    if (hi == 4) {                                                      /* data */
        if (!bp_count(b, &p, low, &count) || count > b->n - p)
            return false;
        char t[160];
        int w = snprintf(t, sizeof(t), "<data, %llu bytes:", (unsigned long long)count);
        for (uint64_t i = 0; i < count && i < 16; i++)
            w += snprintf(t + w, sizeof(t) - (size_t)w, " %02x", b->d[p + i]);
        snprintf(t + w, sizeof(t) - (size_t)w, "%s>", count > 16 ? " ..." : "");
        jstr_out(o, (const uint8_t *)t, strlen(t));
        return true;
    }
    if (hi == 5) {                                                      /* ASCII */
        if (!bp_count(b, &p, low, &count) || count > b->n - p)
            return false;
        jstr_out(o, b->d + p, (size_t)count);
        return true;
    }
    if (hi == 6) {                                                      /* UTF-16BE */
        if (!bp_count(b, &p, low, &count) || count > (b->n - p) / 2)
            return false;
        struct sb t = { 0 };
        for (uint64_t i = 0; i < count; i++) {
            unsigned cp = (unsigned)b->d[p + 2 * i] << 8 | b->d[p + 2 * i + 1];
            if (cp >= 0xd800 && cp < 0xdc00 && i + 1 < count) {
                unsigned lo = (unsigned)b->d[p + 2 * i + 2] << 8 | b->d[p + 2 * i + 3];
                if (lo >= 0xdc00 && lo < 0xe000) {
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                    i++;
                }
            }
            utf8_put(&t, cp);
        }
        jstr_out(o, (const uint8_t *)(t.s ? t.s : ""), t.n);
        free(t.s);
        return true;
    }
    if (hi == 8) {                                                      /* a UID */
        unsigned sz = low + 1;
        if (sz > 8 || p + sz > b->n)
            return false;
        sb_printf(o, "{\"UID\":%llu}", (unsigned long long)be_n(b->d + p, sz));
        return true;
    }
    if (hi == 0xa || hi == 0xc) {                                       /* an array or a set */
        if (!bp_count(b, &p, low, &count) || count > (b->n - p) / b->ref_size)
            return false;
        sb_add(o, "[", 1);
        for (uint64_t i = 0; i < count; i++) {
            if (i)
                sb_add(o, ",", 1);
            if (!bp_obj(b, be_n(b->d + p + (size_t)i * b->ref_size, b->ref_size), o, depth + 1))
                return false;
        }
        sb_add(o, "]", 1);
        return true;
    }
    if (hi == 0xd) {                                                    /* a dictionary: the keys, then the values */
        if (!bp_count(b, &p, low, &count) || count > (b->n - p) / (2 * b->ref_size))
            return false;
        sb_add(o, "{", 1);
        for (uint64_t i = 0; i < count; i++) {
            if (i)
                sb_add(o, ",", 1);
            if (!bp_key_string(b, be_n(b->d + p + (size_t)i * b->ref_size, b->ref_size), o, depth + 1))
                return false;
            sb_add(o, ":", 1);
            if (!bp_obj(b, be_n(b->d + p + ((size_t)count + (size_t)i) * b->ref_size, b->ref_size), o, depth + 1))
                return false;
        }
        sb_add(o, "}", 1);
        return true;
    }
    return false;
}

char *bplist_json(const uint8_t *d, size_t n, char *info, size_t infon)
{
    info[0] = 0;
    if (n < 8 + 32 || memcmp(d, "bplist", 6))
        return NULL;
    const uint8_t *t = d + n - 32;
    struct bp b = { .d = d, .n = n, .off_size = t[6], .ref_size = t[7], .budget = 3000000 };
    uint64_t nobj = be_n(t + 8, 8), top = be_n(t + 16, 8), ot = be_n(t + 24, 8);
    if (b.off_size < 1 || b.off_size > 8 || b.ref_size < 1 || b.ref_size > 8 || nobj == 0 || nobj > n || top >= nobj || ot >= n || ot + nobj * b.off_size > n - 32)
        return NULL;
    b.nobj = (size_t)nobj;
    b.top = (size_t)top;
    b.off_table = (size_t)ot;
    struct sb o = { 0 };
    if (!bp_obj(&b, top, &o, 0) || o.full || !o.s) {
        free(o.s);
        return NULL;
    }
    snprintf(info, infon, "Binary property list, version %.2s, %llu objects", (const char *)d + 6, (unsigned long long)nobj);
    return o.s;
}

/* ---- SHA-1 ---- */
static uint32_t rol(uint32_t v, int s)
{
    return v << s | v >> (32 - s);
}

void data_sha1(const uint8_t *d, size_t n, uint8_t out[20])
{
    uint32_t h[5] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0 };
    uint8_t blk[128];
    size_t done = 0;
    uint64_t bits = (uint64_t)n * 8;
    while (true) {
        size_t chunk = n - done >= 64 ? 64 : n - done;
        const uint8_t *p = d + done;
        size_t nblocks = 1;
        if (chunk < 64) {                                               /* the last one or two blocks with the padding */
            memset(blk, 0, sizeof(blk));
            memcpy(blk, p, chunk);
            blk[chunk] = 0x80;
            nblocks = chunk >= 56 ? 2 : 1;
            for (int i = 0; i < 8; i++)
                blk[nblocks * 64 - 1 - i] = (uint8_t)(bits >> (8 * i));
            p = blk;
        }
        for (size_t blkn = 0; blkn < nblocks; blkn++) {
            uint32_t w[80];
            const uint8_t *q = p + blkn * 64;
            for (int i = 0; i < 16; i++)
                w[i] = (uint32_t)q[4 * i] << 24 | (uint32_t)q[4 * i + 1] << 16 | (uint32_t)q[4 * i + 2] << 8 | q[4 * i + 3];
            for (int i = 16; i < 80; i++)
                w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
            uint32_t a = h[0], bb = h[1], c = h[2], dd = h[3], e = h[4];
            for (int i = 0; i < 80; i++) {
                uint32_t f, k;
                if (i < 20) { f = (bb & c) | (~bb & dd); k = 0x5a827999; }
                else if (i < 40) { f = bb ^ c ^ dd; k = 0x6ed9eba1; }
                else if (i < 60) { f = (bb & c) | (bb & dd) | (c & dd); k = 0x8f1bbcdc; }
                else { f = bb ^ c ^ dd; k = 0xca62c1d6; }
                uint32_t tmp = rol(a, 5) + f + e + k + w[i];
                e = dd;
                dd = c;
                c = rol(bb, 30);
                bb = a;
                a = tmp;
            }
            h[0] += a; h[1] += bb; h[2] += c; h[3] += dd; h[4] += e;
        }
        if (chunk < 64)
            break;
        done += 64;
    }
    for (int i = 0; i < 5; i++) {
        out[4 * i] = (uint8_t)(h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(h[i] >> 8);
        out[4 * i + 3] = (uint8_t)h[i];
    }
}

/* ---- bencode / torrent ---- */
enum { B_INT, B_STR, B_LIST, B_DICT };

struct bv {
    int t;
    long long i;
    const uint8_t *s;
    size_t sl;
    struct bv *it;                      /* list items; dictionary values */
    struct bv *key;                     /* dictionary keys (strings) */
    int n;
    size_t start, end;                  /* the bytes of this value in the file */
};

static void bv_free(struct bv *v)
{
    for (int i = 0; i < v->n; i++) {
        bv_free(&v->it[i]);
        if (v->key)
            bv_free(&v->key[i]);
    }
    free(v->it);
    free(v->key);
    memset(v, 0, sizeof(*v));
}

static bool bv_parse(const uint8_t *d, size_t n, size_t *pos, struct bv *v, int depth, long *budget)
{
    memset(v, 0, sizeof(*v));
    if (depth > MAX_DEPTH || *pos >= n || --*budget < 0)
        return false;
    v->start = *pos;
    uint8_t c = d[*pos];
    if (c == 'i') {
        size_t p = *pos + 1;
        bool neg = p < n && d[p] == '-';
        if (neg)
            p++;
        long long x = 0;
        size_t digits = 0;
        while (p < n && d[p] >= '0' && d[p] <= '9' && digits < 18) {
            x = x * 10 + (d[p++] - '0');
            digits++;
        }
        if (!digits || p >= n || d[p] != 'e')
            return false;
        v->t = B_INT;
        v->i = neg ? -x : x;
        *pos = p + 1;
    } else if (c >= '0' && c <= '9') {
        size_t p = *pos;
        size_t len = 0, digits = 0;
        while (p < n && d[p] >= '0' && d[p] <= '9' && digits < 12) {
            len = len * 10 + (size_t)(d[p++] - '0');
            digits++;
        }
        if (p >= n || d[p] != ':' || len > n - p - 1)
            return false;
        v->t = B_STR;
        v->s = d + p + 1;
        v->sl = len;
        *pos = p + 1 + len;
    } else if (c == 'l' || c == 'd') {
        bool dict = c == 'd';
        v->t = dict ? B_DICT : B_LIST;
        size_t p = *pos + 1;
        int cap = 0;
        while (p < n && d[p] != 'e') {
            if (v->n >= 100000)
                { bv_free(v); return false; }
            if (v->n == cap) {
                cap = cap ? cap * 2 : 8;
                v->it = realloc(v->it, (size_t)cap * sizeof(struct bv));
                if (dict)
                    v->key = realloc(v->key, (size_t)cap * sizeof(struct bv));
            }
            if (dict) {
                if (!bv_parse(d, n, &p, &v->key[v->n], depth + 1, budget)) {
                    bv_free(v);
                    return false;
                }
                if (v->key[v->n].t != B_STR) {
                    bv_free(&v->key[v->n]);
                    bv_free(v);
                    return false;
                }
            }
            if (!bv_parse(d, n, &p, &v->it[v->n], depth + 1, budget)) {
                if (dict)
                    bv_free(&v->key[v->n]);
                bv_free(v);
                return false;
            }
            v->n++;
        }
        if (p >= n) {
            bv_free(v);
            return false;
        }
        *pos = p + 1;
    } else {
        return false;
    }
    v->end = *pos;
    return true;
}

static const struct bv *bd_get(const struct bv *d, const char *k)
{
    if (!d || d->t != B_DICT)
        return NULL;
    size_t kl = strlen(k);
    for (int i = 0; i < d->n; i++)
        if (d->key[i].sl == kl && !memcmp(d->key[i].s, k, kl))
            return &d->it[i];
    return NULL;
}

static void human(long long n, char *o, size_t cap, bool exact)
{
    static const char *const u[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    double v = (double)n;
    int k = 0;
    while (v >= 1024 && k < 4) {
        v /= 1024;
        k++;
    }
    if (k == 0)
        snprintf(o, cap, "%lld B", n);
    else
        if (exact)
            snprintf(o, cap, "%.1f %s (%lld bytes)", v, u[k], n);
        else
            snprintf(o, cap, "%.1f %s", v, u[k]);
}

static void kv_str(struct sb *o, bool *first, const char *k, const uint8_t *s, size_t n)
{
    if (!*first)
        sb_add(o, ",", 1);
    *first = false;
    jstr_out(o, (const uint8_t *)k, strlen(k));
    sb_add(o, ":", 1);
    jstr_out(o, s, n);
}

static void kv_text(struct sb *o, bool *first, const char *k, const char *s)
{
    kv_str(o, first, k, (const uint8_t *)s, strlen(s));
}

static void pct_encode(struct sb *o, const uint8_t *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        uint8_t c = s[i];
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_' || c == '.' || c == '~')
            sb_add(o, (const char *)&c, 1);
        else
            sb_printf(o, "%%%02X", c);
    }
}

char *torrent_json(const uint8_t *d, size_t n, char *info, size_t infon)
{
    info[0] = 0;
    if (n < 20 || d[0] != 'd')
        return NULL;
    struct bv top;
    size_t pos = 0;
    long budget = 2000000;
    if (!bv_parse(d, n, &pos, &top, 0, &budget) || top.t != B_DICT) {
        bv_free(&top);
        return NULL;
    }
    const struct bv *inf = bd_get(&top, "info");
    if (!inf || inf->t != B_DICT) {
        bv_free(&top);
        return NULL;
    }
    struct sb o = { 0 };
    bool first = true;
    sb_add(&o, "{", 1);
    const struct bv *name = bd_get(inf, "name.utf-8");
    if (!name || name->t != B_STR)
        name = bd_get(inf, "name");
    if (name && name->t == B_STR)
        kv_str(&o, &first, "name", name->s, name->sl);
    /* the info hash: SHA-1 of the bencoded info dictionary exactly as it lies in the file */
    uint8_t h[20];
    data_sha1(d + inf->start, inf->end - inf->start, h);
    char hex[41];
    for (int i = 0; i < 20; i++)
        snprintf(hex + 2 * i, 3, "%02x", h[i]);
    kv_text(&o, &first, "info hash", hex);
    {
        struct sb m = { 0 };
        sb_printf(&m, "magnet:?xt=urn:btih:%s", hex);
        if (name && name->t == B_STR) {
            sb_puts(&m, "&dn=");
            pct_encode(&m, name->s, name->sl);
        }
        const struct bv *an = bd_get(&top, "announce");
        if (an && an->t == B_STR) {
            sb_puts(&m, "&tr=");
            pct_encode(&m, an->s, an->sl);
        }
        if (m.s)
            kv_text(&o, &first, "magnet link", m.s);
        free(m.s);
    }
    /* sizes and files */
    long long total = 0;
    const struct bv *files = bd_get(inf, "files"), *len = bd_get(inf, "length");
    int nfiles = 1;
    if (files && files->t == B_LIST) {
        nfiles = files->n;
        for (int i = 0; i < files->n; i++) {
            const struct bv *l = bd_get(&files->it[i], "length");
            if (l && l->t == B_INT && l->i > 0)
                total += l->i;
        }
    } else if (len && len->t == B_INT) {
        total = len->i;
    }
    char hs[80];
    human(total, hs, sizeof(hs), true);
    kv_text(&o, &first, "size", hs);
    sb_printf(&o, ",\"files\":%d", nfiles);
    const struct bv *pl = bd_get(inf, "piece length"), *pieces = bd_get(inf, "pieces");
    if (pl && pl->t == B_INT && pieces && pieces->t == B_STR) {
        char t[120];
        snprintf(t, sizeof(t), "%zu pieces of %lld bytes", pieces->sl / 20, pl->i);
        kv_text(&o, &first, "pieces", t);
    }
    const struct bv *priv = bd_get(inf, "private");
    kv_text(&o, &first, "private", priv && priv->t == B_INT && priv->i == 1 ? "yes" : "no");
    const struct bv *cd = bd_get(&top, "creation date");
    if (cd && cd->t == B_INT) {
        char t[48];
        iso_time((double)cd->i, t, sizeof(t));
        kv_text(&o, &first, "created", t);
    }
    const struct bv *cb = bd_get(&top, "created by"), *cm = bd_get(&top, "comment");
    if (cb && cb->t == B_STR)
        kv_str(&o, &first, "created by", cb->s, cb->sl);
    if (cm && cm->t == B_STR)
        kv_str(&o, &first, "comment", cm->s, cm->sl);
    /* trackers */
    const struct bv *an = bd_get(&top, "announce"), *al = bd_get(&top, "announce-list");
    int shown = 0;
    sb_puts(&o, ",\"trackers\":[");
    if (an && an->t == B_STR) {
        jstr_out(&o, an->s, an->sl);
        shown++;
    }
    if (al && al->t == B_LIST)
        for (int i = 0; i < al->n && shown < 60; i++) {
            const struct bv *tier = &al->it[i];
            for (int k = 0; tier->t == B_LIST && k < tier->n && shown < 60; k++)
                if (tier->it[k].t == B_STR && !(an && an->t == B_STR && an->sl == tier->it[k].sl && !memcmp(an->s, tier->it[k].s, an->sl))) {
                    if (shown)
                        sb_add(&o, ",", 1);
                    jstr_out(&o, tier->it[k].s, tier->it[k].sl);
                    shown++;
                }
        }
    sb_add(&o, "]", 1);
    if (files && files->t == B_LIST) {
        sb_puts(&o, ",\"file list\":[");
        for (int i = 0; i < files->n && i < 500; i++) {
            const struct bv *path = bd_get(&files->it[i], "path.utf-8");
            if (!path || path->t != B_LIST)
                path = bd_get(&files->it[i], "path");
            const struct bv *l = bd_get(&files->it[i], "length");
            struct sb e = { 0 };
            for (int k = 0; path && path->t == B_LIST && k < path->n; k++) {
                if (k)
                    sb_add(&e, "/", 1);
                if (path->it[k].t == B_STR)
                    sb_add(&e, (const char *)path->it[k].s, path->it[k].sl);
            }
            char sz[80];
            human(l && l->t == B_INT ? l->i : 0, sz, sizeof(sz), false);
            sb_printf(&e, "  (%s)", sz);
            if (i)
                sb_add(&o, ",", 1);
            jstr_out(&o, (const uint8_t *)(e.s ? e.s : ""), e.n);
            free(e.s);
        }
        if (files->n > 500)
            sb_printf(&o, ",\"... %d more files\"", files->n - 500);
        sb_add(&o, "]", 1);
    }
    sb_add(&o, "}", 1);
    snprintf(info, infon, "Torrent, %.60s, %s, %d file%s", name && name->t == B_STR ? (const char *)name->s : "", hs, nfiles, nfiles == 1 ? "" : "s");
    if (name && name->t == B_STR && name->sl < 60)
        snprintf(info, infon, "Torrent, %.*s, %s, %d file%s", (int)name->sl, (const char *)name->s, hs, nfiles, nfiles == 1 ? "" : "s");
    bv_free(&top);
    if (o.full) {
        free(o.s);
        return NULL;
    }
    return o.s;
}

/* ---- Jupyter notebooks ---- */
/* The source of a cell: a string, or an array of strings (lines with their line breaks), joined. ANSI colour codes are taken out. */
static void nb_text(struct sb *o, const struct json *v, bool strip_ansi)
{
    if (!v)
        return;
    if (v->type == JSON_STR && v->str) {
        for (const char *p = v->str; *p; p++) {
            if (strip_ansi && *p == '\x1b' && p[1] == '[') {
                p += 2;
                while (*p && !((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')))
                    p++;
                if (!*p)
                    break;
                continue;
            }
            if (*p == '\r')
                continue;
            sb_add(o, p, 1);
        }
    } else if (v->type == JSON_ARR) {
        for (const struct json *c = v->child; c; c = c->next)
            nb_text(o, c, strip_ansi);
    }
}

/* Text as a fenced block: the lines cut to `max_lines`. */
static void nb_fence(struct sb *out, const char *lang, const char *text, size_t n, int max_lines)
{
    while (n && (text[n - 1] == '\n' || text[n - 1] == ' '))
        n--;
    if (!n)
        return;
    const char *fence = "```";
    for (size_t i = 0; i + 2 < n; i++)
        if (!memcmp(text + i, "```", 3)) {
            fence = "~~~~";
            break;
        }
    sb_printf(out, "\n%s%s\n", fence, lang);
    int lines = 0;
    size_t i = 0;
    while (i < n) {
        const char *nl = memchr(text + i, '\n', n - i);
        size_t l = nl ? (size_t)(nl - (text + i)) : n - i;
        if (++lines > max_lines) {
            sb_puts(out, "... (more lines)\n");
            break;
        }
        if (l > 400) {
            sb_add(out, text + i, 400);
            sb_puts(out, " ...\n");
        } else {
            sb_add(out, text + i, l);
            sb_add(out, "\n", 1);
        }
        i += l + 1;
    }
    sb_printf(out, "%s\n", fence);
}

char *ipynb_markdown(const char *text, size_t n, size_t *len, char *info, size_t infon)
{
    info[0] = 0;
    char *copy = malloc(n + 1);
    if (!copy)
        return NULL;
    memcpy(copy, text, n);
    copy[n] = 0;
    struct json *root = json_parse(copy);
    free(copy);
    const struct json *cells = root && root->type == JSON_OBJ ? json_get(root, "cells") : NULL;
    if (!cells || cells->type != JSON_ARR) {
        json_free(root);
        return NULL;
    }
    const struct json *meta = json_get(root, "metadata"), *ks = meta ? json_get(meta, "kernelspec") : NULL, *li = meta ? json_get(meta, "language_info") : NULL;
    const char *lang = json_str(ks, "language", json_str(li, "name", "python"));
    const char *kname = json_str(ks, "display_name", json_str(li, "name", ""));
    struct sb o = { 0 };
    int ncells = 0, ncode = 0;
    for (const struct json *c = cells->child; c && !o.full; c = c->next) {
        if (c->type != JSON_OBJ)
            continue;
        ncells++;
        const char *type = json_str(c, "cell_type", "");
        struct sb src = { 0 };
        nb_text(&src, json_get(c, "source"), false);
        if (!strcmp(type, "markdown")) {
            if (src.s) {
                sb_add(&o, "\n", 1);
                sb_add(&o, src.s, src.n);
                sb_add(&o, "\n", 1);
            }
        } else if (!strcmp(type, "code")) {
            ncode++;
            if (src.s)
                nb_fence(&o, lang, src.s, src.n, 400);
            const struct json *outs = json_get(c, "outputs");
            for (const struct json *oo = outs && outs->type == JSON_ARR ? outs->child : NULL; oo; oo = oo->next) {
                const char *ot = json_str(oo, "output_type", "");
                struct sb t = { 0 };
                if (!strcmp(ot, "stream")) {
                    nb_text(&t, json_get(oo, "text"), true);
                } else if (!strcmp(ot, "execute_result") || !strcmp(ot, "display_data")) {
                    const struct json *data = json_get(oo, "data");
                    const struct json *plain = data ? json_get(data, "text/plain") : NULL;
                    if (plain) {
                        nb_text(&t, plain, true);
                    } else if (data) {
                        sb_puts(&t, "[output: ");
                        bool f = true;
                        for (const struct json *k = data->child; k; k = k->next) {
                            sb_printf(&t, "%s%s", f ? "" : ", ", k->key ? k->key : "?");
                            f = false;
                        }
                        sb_puts(&t, "]");
                    }
                } else if (!strcmp(ot, "error")) {
                    sb_printf(&t, "%s: %s", json_str(oo, "ename", "Error"), json_str(oo, "evalue", ""));
                }
                if (t.s)
                    nb_fence(&o, "text", t.s, t.n, 40);
                free(t.s);
            }
        } else if (src.s) {
            nb_fence(&o, "", src.s, src.n, 200);
        }
        free(src.s);
    }
    if (o.full)
        sb_puts(&o, "\n*(the rest of the notebook is not shown)*\n");
    snprintf(info, infon, "Jupyter notebook%s%s, %d cell%s, %d code", kname[0] ? ", " : "", kname, ncells, ncells == 1 ? "" : "s", ncode);
    json_free(root);
    *len = o.n;
    return o.s ? o.s : strdup("");
}
