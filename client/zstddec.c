#include "zstddec.h"

#include <stdbool.h>
#include <string.h>

#define MAX_BLOCK (128u * 1024)

/* ---- bit readers ---- */

struct fwd {                            /* forward, least significant bit first (FSE table headers) */
    const uint8_t *p;
    size_t len, bit;
    bool bad;
};

static uint32_t fwd_read(struct fwd *f, int n)
{
    uint32_t v = 0;
    if (f->bit + (size_t)n > f->len * 8) { f->bad = true; return 0; }
    for (int i = 0; i < n; i++, f->bit++)
        v |= (uint32_t)((f->p[f->bit >> 3] >> (f->bit & 7)) & 1) << i;
    return v;
}

struct bwd {                            /* backward: from the last byte's marker bit down to the first bit */
    const uint8_t *p;
    size_t len;
    int64_t pos;                        /* bits not yet read */
    bool over;
};

static bool bwd_init(struct bwd *b, const uint8_t *p, size_t len)
{
    if (!len || !p[len - 1]) return false;
    b->p = p;
    b->len = len;
    b->pos = (int64_t)(len - 1) * 8 + (31 - __builtin_clz((unsigned)p[len - 1]));
    b->over = false;
    return true;
}

/* The next n (<= 32) bits without taking them (bits before the start read as zero). */
static uint32_t bwd_peek(const struct bwd *b, int n)
{
    if (n == 0) return 0;
    int64_t lo = b->pos - n;
    int64_t from = lo < 0 ? 0 : lo;
    int cnt = (int)(b->pos - from);
    if (cnt <= 0) return 0;
    size_t byte = (size_t)(from >> 3);
    uint64_t w = 0;
    for (int k = 0; k < 8 && byte + (size_t)k < b->len; k++)
        w |= (uint64_t)b->p[byte + (size_t)k] << (8 * k);
    uint64_t v = (w >> (from & 7)) & (cnt >= 32 ? 0xffffffffull : ((1ull << cnt) - 1));
    return (uint32_t)(lo < 0 ? v << (-lo) : v);
}

static void bwd_skip(struct bwd *b, int n)
{
    b->pos -= n;
    if (b->pos < 0) b->over = true;
}

static uint32_t bwd_read(struct bwd *b, int n)
{
    uint32_t v = bwd_peek(b, n);
    bwd_skip(b, n);
    return v;
}

/* ---- FSE ---- */

#define FSE_MAX_AL 9
struct fse {
    int al;
    uint8_t sym[1 << FSE_MAX_AL], nb[1 << FSE_MAX_AL];
    uint16_t nw[1 << FSE_MAX_AL];
    bool ok;
};

static int highbit(unsigned v) { return 31 - __builtin_clz(v); }

/* A table from normalised counts (-1 = "less than one"). */
static bool fse_build(struct fse *t, const int16_t *norm, int nsym, int al)
{
    if (al > FSE_MAX_AL || nsym < 1 || nsym > 256) return false;
    int size = 1 << al, high = size - 1, total = 0;
    uint16_t next[256];
    for (int s = 0; s < nsym; s++) {
        if (norm[s] == -1) {
            if (high < 0) return false;
            t->sym[high--] = (uint8_t)s;
            next[s] = 1;
            total += 1;
        } else if (norm[s] >= 0) {
            next[s] = (uint16_t)norm[s];
            total += norm[s];
        } else {
            return false;
        }
    }
    if (total != size) return false;
    int step = (size >> 1) + (size >> 3) + 3, mask = size - 1, pos = 0;
    for (int s = 0; s < nsym; s++)
        for (int i = 0; i < norm[s]; i++) {
            t->sym[pos] = (uint8_t)s;
            do pos = (pos + step) & mask; while (pos > high);
        }
    if (pos != 0) return false;
    for (int u = 0; u < size; u++) {
        unsigned ns = next[t->sym[u]]++;
        int nb = al - highbit(ns);
        t->nb[u] = (uint8_t)nb;
        t->nw[u] = (uint16_t)((ns << nb) - (unsigned)size);
    }
    t->al = al;
    t->ok = true;
    return true;
}

static void fse_rle(struct fse *t, int sym)
{
    t->al = 0;
    t->sym[0] = (uint8_t)sym;
    t->nb[0] = 0;
    t->nw[0] = 0;
    t->ok = true;
}

/* Reads a table description; returns the bytes it took, -1 on error. */
static long fse_read(struct fse *t, const uint8_t *p, size_t len, int maxal, int maxsym)
{
    struct fwd f = { p, len, 0, false };
    int al = (int)fwd_read(&f, 4) + 5;
    if (f.bad || al > maxal) return -1;
    int16_t norm[256];
    int remaining = (1 << al) + 1, threshold = 1 << al, nbits = al + 1, sym = 0;
    bool prev0 = false;
    while (remaining > 1 && sym <= maxsym) {
        if (prev0) {
            for (;;) {                                                  /* runs of zero counts, 2 bits at a time */
                uint32_t r = fwd_read(&f, 2);
                if (f.bad) return -1;
                for (uint32_t k = 0; k < r; k++) {
                    if (sym > maxsym) return -1;
                    norm[sym++] = 0;
                }
                if (r < 3) break;
            }
            prev0 = false;
            if (sym > maxsym) break;
        }
        int max = 2 * threshold - 1 - remaining;
        uint32_t small = 0;
        size_t save = f.bit;
        small = fwd_read(&f, nbits - 1);
        if (f.bad) return -1;
        int count;
        if ((int)small < max) {
            count = (int)small;
        } else {
            f.bit = save;
            uint32_t v = fwd_read(&f, nbits);
            if (f.bad) return -1;
            count = (int)(v & (uint32_t)(2 * threshold - 1));
            if (count >= threshold) count -= max;
        }
        count--;
        remaining -= count < 0 ? -count : count;
        norm[sym++] = (int16_t)count;
        prev0 = count == 0;
        while (remaining < threshold) { nbits--; threshold >>= 1; }
    }
    if (remaining != 1 || sym > maxsym + 1) return -1;
    if (!fse_build(t, norm, sym, al)) return -1;
    return (long)((f.bit + 7) / 8);
}

/* ---- Huffman ---- */

#define HUF_MAX_BITS 11
struct huf {
    int maxbits;
    uint8_t sym[1 << HUF_MAX_BITS], nb[1 << HUF_MAX_BITS];
    bool ok;
};

/* The tree description at p; returns the bytes it took, -1 on error. */
static long huf_read(struct huf *h, const uint8_t *p, size_t len)
{
    if (!len) return -1;
    uint8_t w[256];
    int nw;
    size_t used;
    unsigned hb = p[0];
    if (hb >= 128) {
        nw = (int)hb - 127;
        size_t bytes = ((size_t)nw + 1) / 2;
        if (1 + bytes > len) return -1;
        for (int i = 0; i < nw; i++)
            w[i] = i & 1 ? p[1 + i / 2] & 15 : p[1 + i / 2] >> 4;
        used = 1 + bytes;
    } else {
        if (hb == 0 || 1 + (size_t)hb > len) return -1;
        struct fse t;
        long hl = fse_read(&t, p + 1, hb, 6, 255);
        if (hl < 0) return -1;
        struct bwd b;
        if (!bwd_init(&b, p + 1 + hl, hb - (size_t)hl)) return -1;
        uint32_t s1 = bwd_read(&b, t.al), s2 = bwd_read(&b, t.al);
        nw = 0;
        for (;;) {                                                      /* two interleaved states until the stream runs out */
            if (nw >= 255) return -1;
            w[nw++] = t.sym[s1];
            s1 = t.nw[s1] + bwd_read(&b, t.nb[s1]);
            if (b.over) { if (nw >= 255) return -1; w[nw++] = t.sym[s2]; break; }
            if (nw >= 255) return -1;
            w[nw++] = t.sym[s2];
            s2 = t.nw[s2] + bwd_read(&b, t.nb[s2]);
            if (b.over) { if (nw >= 255) return -1; w[nw++] = t.sym[s1]; break; }
        }
        used = 1 + (size_t)hb;
    }
    unsigned total = 0;
    for (int i = 0; i < nw; i++) {
        if (w[i] > HUF_MAX_BITS) return -1;
        if (w[i]) total += 1u << (w[i] - 1);
    }
    if (!total) return -1;
    int maxbits = highbit(total) + 1;
    if (maxbits > HUF_MAX_BITS) return -1;
    unsigned rest = (1u << maxbits) - total;
    if (!rest || (rest & (rest - 1))) return -1;                        /* the last symbol's weight makes a power of two */
    w[nw] = (uint8_t)(highbit(rest) + 1);
    nw++;
    h->maxbits = maxbits;
    unsigned pos = 0;
    for (int wt = 1; wt <= maxbits; wt++)
        for (int s = 0; s < nw; s++)
            if (w[s] == wt) {
                unsigned n = 1u << (wt - 1);
                if (pos + n > (1u << maxbits)) return -1;
                for (unsigned k = 0; k < n; k++) {
                    h->sym[pos + k] = (uint8_t)s;
                    h->nb[pos + k] = (uint8_t)(maxbits + 1 - wt);
                }
                pos += n;
            }
    if (pos != (1u << maxbits)) return -1;
    h->ok = true;
    return (long)used;
}

static bool huf_stream(const struct huf *h, const uint8_t *p, size_t len, uint8_t *out, size_t n)
{
    struct bwd b;
    if (!bwd_init(&b, p, len)) return false;
    for (size_t i = 0; i < n; i++) {
        uint32_t v = bwd_peek(&b, h->maxbits);
        out[i] = h->sym[v];
        bwd_skip(&b, h->nb[v]);
    }
    return !b.over;
}

/* ---- blocks ---- */

static const int16_t LL_DEF[36] = { 4, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 2, 1, 1, 1, 1, 1, -1, -1, -1, -1 };
static const int16_t ML_DEF[53] = { 1, 4, 3, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1, -1, -1 };
static const int16_t OF_DEF[29] = { 1, 1, 1, 1, 1, 1, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1 };

static const uint32_t LL_BASE[36] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 18, 20, 22, 24, 28, 32, 40, 48, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536 };
static const uint8_t LL_BITS[36] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
static const uint32_t ML_BASE[53] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34,
                                      35, 37, 39, 41, 43, 47, 51, 59, 67, 83, 99, 131, 259, 515, 1027, 2051, 4099, 8195, 16387, 32771, 65539 };
static const uint8_t ML_BITS[53] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                     1, 1, 1, 1, 2, 2, 3, 3, 4, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };

struct zctx {
    struct huf huf;
    struct fse ll, of, ml;
    uint32_t rep[3];
};

static bool lit_section(struct zctx *z, const uint8_t **pp, const uint8_t *end, uint8_t *lit, size_t *nlit)
{
    const uint8_t *p = *pp;
    if (p >= end) return false;
    int type = p[0] & 3, fmt = (p[0] >> 2) & 3;
    size_t regen, comp = 0, hdr;
    if (type < 2) {
        if (fmt == 0 || fmt == 2) { regen = p[0] >> 3; hdr = 1; }
        else if (fmt == 1) { if (end - p < 2) return false; regen = (p[0] >> 4) | ((size_t)p[1] << 4); hdr = 2; }
        else { if (end - p < 3) return false; regen = (p[0] >> 4) | ((size_t)p[1] << 4) | ((size_t)p[2] << 12); hdr = 3; }
        if (regen > MAX_BLOCK) return false;
        p += hdr;
        if (type == 0) {
            if ((size_t)(end - p) < regen) return false;
            memcpy(lit, p, regen);
            p += regen;
        } else {
            if (p >= end) return false;
            memset(lit, p[0], regen);
            p++;
        }
        *nlit = regen;
        *pp = p;
        return true;
    }
    int streams = fmt == 0 ? 1 : 4, bits = fmt <= 1 ? 10 : fmt == 2 ? 14 : 18;
    hdr = fmt <= 1 ? 3 : fmt == 2 ? 4 : 5;
    if ((size_t)(end - p) < hdr) return false;
    uint64_t v = 0;
    for (size_t i = 0; i < hdr; i++) v |= (uint64_t)p[i] << (8 * i);
    v >>= 4;
    regen = (size_t)(v & ((1u << bits) - 1));
    comp = (size_t)((v >> bits) & ((1u << bits) - 1));
    p += hdr;
    if (regen > MAX_BLOCK || (size_t)(end - p) < comp) return false;
    const uint8_t *cend = p + comp;
    if (type == 2) {
        long u = huf_read(&z->huf, p, comp);
        if (u < 0) return false;
        p += u;
    } else if (!z->huf.ok) {
        return false;
    }
    if (streams == 1) {
        if (!huf_stream(&z->huf, p, (size_t)(cend - p), lit, regen)) return false;
    } else {
        if (cend - p < 6) return false;
        size_t s1 = p[0] | ((size_t)p[1] << 8), s2 = p[2] | ((size_t)p[3] << 8), s3 = p[4] | ((size_t)p[5] << 8);
        p += 6;
        if (s1 + s2 + s3 > (size_t)(cend - p)) return false;
        size_t s4 = (size_t)(cend - p) - s1 - s2 - s3, per = (regen + 3) / 4;
        if (per * 3 > regen) return false;
        if (!huf_stream(&z->huf, p, s1, lit, per) || !huf_stream(&z->huf, p + s1, s2, lit + per, per) ||
            !huf_stream(&z->huf, p + s1 + s2, s3, lit + 2 * per, per) || !huf_stream(&z->huf, p + s1 + s2 + s3, s4, lit + 3 * per, regen - 3 * per))
            return false;
    }
    *nlit = regen;
    *pp = cend;
    return true;
}

static bool seq_table(struct fse *t, int mode, const uint8_t **pp, const uint8_t *end, const int16_t *def, int ndef, int defal, int maxal, int maxsym)
{
    const uint8_t *p = *pp;
    if (mode == 0) {
        return fse_build(t, def, ndef, defal);
    } else if (mode == 1) {
        if (p >= end || p[0] > maxsym) return false;
        fse_rle(t, p[0]);
        *pp = p + 1;
        return true;
    } else if (mode == 2) {
        long u = fse_read(t, p, (size_t)(end - p), maxal, maxsym);
        if (u < 0) return false;
        *pp = p + u;
        return true;
    }
    return t->ok;                                                       /* repeat the table of the block before */
}

/* One compressed block's content into out[*op..]; `base` = where this frame began (offsets may not reach before it). */
static bool block(struct zctx *z, const uint8_t *p, size_t len, uint8_t *out, size_t cap, size_t *op, size_t base)
{
    static uint8_t lit[MAX_BLOCK + 8];                                  /* (one thread: the preview's loop) */
    const uint8_t *end = p + len;
    size_t nlit;
    if (!lit_section(z, &p, end, lit, &nlit)) return false;
    if (p >= end) return false;
    size_t nseq = p[0];
    p++;
    if (nseq == 0) {
        if (*op + nlit > cap) return false;
        memcpy(out + *op, lit, nlit);
        *op += nlit;
        return p == end;
    }
    if (nseq >= 128) {
        if (nseq < 255) { if (p >= end) return false; nseq = ((nseq - 128) << 8) + p[0]; p++; }
        else { if (end - p < 2) return false; nseq = p[0] + ((size_t)p[1] << 8) + 0x7f00; p += 2; }
    }
    if (p >= end) return false;
    int modes = p[0];
    p++;
    if (modes & 3) return false;
    if (!seq_table(&z->ll, modes >> 6, &p, end, LL_DEF, 36, 6, 9, 35) || !seq_table(&z->of, (modes >> 4) & 3, &p, end, OF_DEF, 29, 5, 8, 31) ||
        !seq_table(&z->ml, (modes >> 2) & 3, &p, end, ML_DEF, 53, 6, 9, 52))
        return false;
    struct bwd b;
    if (!bwd_init(&b, p, (size_t)(end - p))) return false;
    uint32_t sl = bwd_read(&b, z->ll.al), so = bwd_read(&b, z->of.al), sm = bwd_read(&b, z->ml.al);
    size_t lp = 0;
    for (size_t i = 0; i < nseq; i++) {
        int oc = z->of.sym[so], mc = z->ml.sym[sm], lc = z->ll.sym[sl];
        if (oc > 31 || mc > 52 || lc > 35) return false;
        uint64_t ov = ((uint64_t)1 << oc) + bwd_read(&b, oc);
        size_t ml = ML_BASE[mc] + bwd_read(&b, ML_BITS[mc]);
        size_t ll = LL_BASE[lc] + bwd_read(&b, LL_BITS[lc]);
        if (i + 1 < nseq) {
            sl = z->ll.nw[sl] + bwd_read(&b, z->ll.nb[sl]);
            sm = z->ml.nw[sm] + bwd_read(&b, z->ml.nb[sm]);
            so = z->of.nw[so] + bwd_read(&b, z->of.nb[so]);
        }
        if (b.over) return false;
        uint64_t off;
        if (ov > 3) {
            off = ov - 3;
            z->rep[2] = z->rep[1];
            z->rep[1] = z->rep[0];
            z->rep[0] = (uint32_t)off;
        } else {
            int idx = (int)ov + (ll == 0);
            if (idx == 1) {
                off = z->rep[0];
            } else if (idx == 2) {
                off = z->rep[1];
                z->rep[1] = z->rep[0];
                z->rep[0] = (uint32_t)off;
            } else if (idx == 3) {
                off = z->rep[2];
                z->rep[2] = z->rep[1];
                z->rep[1] = z->rep[0];
                z->rep[0] = (uint32_t)off;
            } else {
                off = (uint64_t)z->rep[0] - 1;
                if (off == 0) return false;
                z->rep[2] = z->rep[1];
                z->rep[1] = z->rep[0];
                z->rep[0] = (uint32_t)off;
            }
        }
        if (ll > nlit - lp || *op + ll + ml > cap) return false;
        memcpy(out + *op, lit + lp, ll);
        lp += ll;
        *op += ll;
        if (off == 0 || off > *op - base) return false;
        for (size_t k = 0; k < ml; k++, (*op)++)
            out[*op] = out[*op - off];
    }
    if (*op + (nlit - lp) > cap) return false;
    memcpy(out + *op, lit + lp, nlit - lp);                             /* the rest of the literals */
    *op += nlit - lp;
    return true;
}

static long zfail(bool prefix, size_t op, int *complete)
{
    if (complete)
        *complete = 0;
    return prefix && op ? (long)op : -1;
}

static long zstd_run(const uint8_t *in, size_t inlen, uint8_t *out, size_t outcap, bool prefix, int *complete)
{
    const uint8_t *p = in, *end = in + inlen;
    size_t op = 0;
    static struct zctx z;
    while (p < end) {
        if (end - p < 4) return zfail(prefix, op, complete);
        uint32_t magic = p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
        p += 4;
        if ((magic & 0xfffffff0u) == 0x184d2a50u) {                     /* a skippable frame */
            if (end - p < 4) return zfail(prefix, op, complete);
            uint32_t sz = p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
            p += 4;
            if (sz > (size_t)(end - p)) return zfail(prefix, op, complete);
            p += sz;
            continue;
        }
        if (magic != 0xfd2fb528u || p >= end) return zfail(prefix, op, complete);
        unsigned fhd = *p++;
        int fcs_flag = fhd >> 6, single = (fhd >> 5) & 1, checksum = (fhd >> 2) & 1, dict = fhd & 3;
        if (fhd & 8) return zfail(prefix, op, complete);
        if (!single) { if (p >= end) return zfail(prefix, op, complete); p++; }                  /* the window descriptor: the output buffer is the window */
        size_t dsz = dict == 0 ? 0 : dict == 1 ? 1 : dict == 2 ? 2 : 4;
        size_t fsz = fcs_flag == 0 ? (single ? 1 : 0) : fcs_flag == 1 ? 2 : fcs_flag == 2 ? 4 : 8;
        if ((size_t)(end - p) < dsz + fsz) return zfail(prefix, op, complete);
        for (size_t i = 0; i < dsz; i++)
            if (p[i]) return zfail(prefix, op, complete);                                        /* a dictionary: not supported */
        p += dsz + fsz;
        memset(&z, 0, sizeof(z));
        z.rep[0] = 1;
        z.rep[1] = 4;
        z.rep[2] = 8;
        size_t base = op;
        for (;;) {
            if (end - p < 3) return zfail(prefix, op, complete);
            uint32_t bh = p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16;
            p += 3;
            int last = bh & 1, type = (bh >> 1) & 3;
            size_t bs = bh >> 3;
            if (type == 3 || bs > MAX_BLOCK) return zfail(prefix, op, complete);
            if (type == 1) {
                if (p >= end || op + bs > outcap) return zfail(prefix, op, complete);
                memset(out + op, p[0], bs);
                op += bs;
                p++;
            } else {
                if (bs > (size_t)(end - p)) return zfail(prefix, op, complete);
                if (type == 0) {
                    if (op + bs > outcap) return zfail(prefix, op, complete);
                    memcpy(out + op, p, bs);
                    op += bs;
                } else if (!block(&z, p, bs, out, outcap, &op, base)) {
                    return zfail(prefix, op, complete);
                }
                p += bs;
            }
            if (last) break;
        }
        if (checksum) {
            if (end - p < 4) return zfail(prefix, op, complete);
            p += 4;
        }
    }
    if (complete)
        *complete = 1;
    return (long)op;
}

long lm_zstd_decompress(const uint8_t *in, size_t inlen, uint8_t *out, size_t outcap)
{
    return zstd_run(in, inlen, out, outcap, false, NULL);
}

long lm_zstd_prefix(const uint8_t *in, size_t inlen, uint8_t *out, size_t outcap, int *complete)
{
    return zstd_run(in, inlen, out, outcap, true, complete);
}
