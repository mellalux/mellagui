/*
 * jpeg.c - a JPEG decoder written for MellaGUI (no libjpeg): baseline / extended sequential and progressive Huffman JPEG,
 * 8 bit, one component (grey) or three (YCbCr, or RGB when the file says so), sampling factors up to 4x4, restart
 * intervals, chroma upsampling with the triangle filter where a plane is half size, and the EXIF orientation tag.
 * Not supported (a clear message instead): arithmetic coding, lossless, 12 bit, CMYK / YCCK, DNL.
 *
 * Every read of the input is bounds checked and every table lookup is validated, so a damaged or hostile file ends in an
 * error and nothing worse (the viewer and the wallpaper loader hand files of unknown origin to this code).
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "img.h"

#define MAXC 3
#define FAST_BITS 9

static const uint8_t zigzag[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
    30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

struct huff {
    bool defined;
    uint16_t fast[1 << FAST_BITS];      /* (length << 8) | symbol for codes of at most FAST_BITS bits, 0 = longer / invalid */
    int maxcode[18];                    /* largest code of each length (left aligned later), -1 = none */
    int valptr[17];
    int mincode[17];
    uint8_t vals[256];
    int nvals;
};

struct comp {
    int id, h, v, tq;
    int w, hgt;                         /* size of the component in samples (ceil) */
    int bw, bh;                         /* blocks covering it (not padded) */
    int pbw, pbh;                       /* blocks incl. the padding to whole MCUs: the plane's size in blocks */
    uint8_t *plane;                     /* pbw*8 x pbh*8 decoded samples */
    int16_t *coef;                      /* progressive only: pbw*pbh*64 coefficients, natural order, not dequantized */
    int dc_pred;
    int td, ta;                         /* table selectors of the running scan */
};

struct jpeg {
    const uint8_t *d;
    size_t len, pos;
    char *err;
    size_t errsz;
    /* frame */
    int w, h, ncomp;
    bool progressive, have_frame;
    struct comp c[MAXC];
    int hmax, vmax, mcux, mcuy;
    uint16_t qt[4][64];                 /* zigzag order, as in the file */
    bool qdef[4];
    struct huff dc[4], ac[4];
    int restart;                        /* restart interval in MCUs, 0 = none */
    int adobe;                          /* -1 none, else the transform flag */
    int orientation;                    /* EXIF, 1..8 */
    /* entropy decoder */
    uint32_t bits;
    int nbits;
    size_t fed;                         /* zero bytes made up after the data ended */
    bool marker;                        /* a marker was met inside the scan */
    int eobrun;
};

static int fail(struct jpeg *j, const char *msg)
{
    if (j->err && j->errsz)
        snprintf(j->err, j->errsz, "%s", msg);
    return -1;
}

/* ---- bit reader --------------------------------------------------------------------------------------------------- */

static void fill(struct jpeg *j)
{
    while (j->nbits <= 24) {
        int b = 0;
        if (j->marker || j->pos >= j->len) {
            j->fed++;                                   /* past the end of the scan: zeros, but not for ever */
        } else {
            b = j->d[j->pos];
            if (b == 0xff) {
                int n = j->pos + 1 < j->len ? j->d[j->pos + 1] : 0xd9;
                if (n == 0) {
                    j->pos += 2;                        /* a stuffed 0xFF */
                } else {
                    j->marker = true;                   /* a real marker: the scan ends here */
                    b = 0;
                    j->fed++;
                }
            } else {
                j->pos++;
            }
        }
        j->bits |= (uint32_t)b << (24 - j->nbits);
        j->nbits += 8;
    }
}

static int get_bits(struct jpeg *j, int n)             /* n = 0..16 */
{
    if (!n)
        return 0;
    if (j->nbits < n)
        fill(j);
    int v = (int)(j->bits >> (32 - n));
    j->bits <<= n;
    j->nbits -= n;
    return v;
}

static int get_bit(struct jpeg *j) { return get_bits(j, 1); }

static int extend(int v, int t) { return v < (1 << (t - 1)) ? v - (1 << t) + 1 : v; }

static int huff_decode(struct jpeg *j, const struct huff *h)
{
    if (j->nbits < 16)
        fill(j);
    int look = (int)(j->bits >> (32 - FAST_BITS));
    int f = h->fast[look];
    if (f) {
        int len = f >> 8;
        j->bits <<= len;
        j->nbits -= len;
        return f & 255;
    }
    int code = (int)(j->bits >> (32 - 16));
    for (int len = FAST_BITS + 1; len <= 16; len++) {
        int c = code >> (16 - len);
        if (h->maxcode[len] >= 0 && c <= h->maxcode[len] && c >= h->mincode[len]) {
            int idx = h->valptr[len] + c - h->mincode[len];
            if (idx < 0 || idx >= h->nvals)
                return -1;
            j->bits <<= len;
            j->nbits -= len;
            return h->vals[idx];
        }
    }
    return -1;
}

static void reset_bits(struct jpeg *j)
{
    j->bits = 0;
    j->nbits = 0;
    j->marker = false;
    j->fed = 0;
}

/* ---- Huffman tables ----------------------------------------------------------------------------------------------- */

static int build_huff(struct huff *h, const uint8_t counts[16], const uint8_t *vals, int n)
{
    memset(h, 0, sizeof(*h));
    memcpy(h->vals, vals, (size_t)n);
    h->nvals = n;
    int code = 0, k = 0;
    for (int len = 1; len <= 16; len++) {
        h->maxcode[len] = -1;
        h->valptr[len] = k;
        h->mincode[len] = code;
        for (int i = 0; i < counts[len - 1]; i++, k++, code++) {
            if (len <= FAST_BITS) {
                int first = code << (FAST_BITS - len), count = 1 << (FAST_BITS - len);
                if (first + count > (1 << FAST_BITS))
                    return -1;
                for (int f = 0; f < count; f++)
                    h->fast[first + f] = (uint16_t)((len << 8) | vals[k]);
            }
        }
        if (counts[len - 1])
            h->maxcode[len] = code - 1;
        if (code > (1 << len))
            return -1;                                  /* more codes than the length has room for */
        code <<= 1;
    }
    h->defined = true;
    return 0;
}

/* ---- IDCT ---------------------------------------------------------------------------------------------------------- */

#define F2F(x) ((int)((x) * 4096 + 0.5))
#define FSH(x) ((x) * 4096)

#define IDCT_1D(s0, s1, s2, s3, s4, s5, s6, s7) \
    int t0, t1, t2, t3, p1, p2, p3, p4, p5, x0, x1, x2, x3; \
    p2 = s2; p3 = s6; \
    p1 = (p2 + p3) * F2F(0.5411961f); \
    t2 = p1 + p3 * F2F(-1.847759065f); \
    t3 = p1 + p2 * F2F(0.765366865f); \
    p2 = s0; p3 = s4; \
    t0 = FSH(p2 + p3); \
    t1 = FSH(p2 - p3); \
    x0 = t0 + t3; x3 = t0 - t3; x1 = t1 + t2; x2 = t1 - t2; \
    t0 = s7; t1 = s5; t2 = s3; t3 = s1; \
    p3 = t0 + t2; p4 = t1 + t3; p1 = t0 + t3; p2 = t1 + t2; \
    p5 = (p3 + p4) * F2F(1.175875602f); \
    t0 = t0 * F2F(0.298631336f); \
    t1 = t1 * F2F(2.053119869f); \
    t2 = t2 * F2F(3.072711026f); \
    t3 = t3 * F2F(1.501321110f); \
    p1 = p5 + p1 * F2F(-0.899976223f); \
    p2 = p5 + p2 * F2F(-2.562915447f); \
    p3 = p3 * F2F(-1.961570560f); \
    p4 = p4 * F2F(-0.390180644f); \
    t3 += p1 + p4; t2 += p2 + p3; t1 += p2 + p4; t0 += p1 + p3;

static uint8_t clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }

/* d: 64 dequantized coefficients in natural order -> an 8x8 block of samples written with the given stride. */
static void idct_block(uint8_t *out, int stride, const int *d)
{
    int val[64];
    for (int i = 0; i < 8; i++) {
        if (!d[i + 8] && !d[i + 16] && !d[i + 24] && !d[i + 32] && !d[i + 40] && !d[i + 48] && !d[i + 56]) {
            int dc = d[i] * 4;
            for (int r = 0; r < 8; r++)
                val[i + r * 8] = dc;
        } else {
            IDCT_1D(d[i], d[i + 8], d[i + 16], d[i + 24], d[i + 32], d[i + 40], d[i + 48], d[i + 56])
            x0 += 512; x1 += 512; x2 += 512; x3 += 512;
            val[i] = (x0 + t3) >> 10;
            val[i + 56] = (x0 - t3) >> 10;
            val[i + 8] = (x1 + t2) >> 10;
            val[i + 48] = (x1 - t2) >> 10;
            val[i + 16] = (x2 + t1) >> 10;
            val[i + 40] = (x2 - t1) >> 10;
            val[i + 24] = (x3 + t0) >> 10;
            val[i + 32] = (x3 - t0) >> 10;
        }
    }
    for (int r = 0; r < 8; r++) {
        const int *v = val + r * 8;
        uint8_t *o = out + r * stride;
        IDCT_1D(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7])
        x0 += 65536 + (128 << 17);
        x1 += 65536 + (128 << 17);
        x2 += 65536 + (128 << 17);
        x3 += 65536 + (128 << 17);
        o[0] = clamp8((x0 + t3) >> 17);
        o[7] = clamp8((x0 - t3) >> 17);
        o[1] = clamp8((x1 + t2) >> 17);
        o[6] = clamp8((x1 - t2) >> 17);
        o[2] = clamp8((x2 + t1) >> 17);
        o[5] = clamp8((x2 - t1) >> 17);
        o[3] = clamp8((x3 + t0) >> 17);
        o[4] = clamp8((x3 - t0) >> 17);
    }
}

/* ---- scan decoding ------------------------------------------------------------------------------------------------ */

struct scan {
    int ncomp;
    int ci[MAXC];                       /* component indexes of this scan */
    int ss, se, ah, al;
};

/* One block of a sequential scan: decoded, dequantized, transformed straight into the plane. */
static int decode_block_seq(struct jpeg *j, struct comp *c, int bx, int by)
{
    int blk[64] = { 0 };
    const struct huff *hd = &j->dc[c->td], *ha = &j->ac[c->ta];
    const uint16_t *q = j->qt[c->tq];
    int t = huff_decode(j, hd);
    if (t < 0 || t > 11)
        return -1;
    int diff = t ? extend(get_bits(j, t), t) : 0;
    c->dc_pred += diff;
    blk[0] = c->dc_pred * q[0];
    for (int k = 1; k < 64;) {
        int rs = huff_decode(j, ha);
        if (rs < 0)
            return -1;
        int r = rs >> 4, s = rs & 15;
        if (!s) {
            if (r != 15)
                break;
            k += 16;
            continue;
        }
        k += r;
        if (k > 63)
            return -1;
        blk[zigzag[k]] = extend(get_bits(j, s), s) * q[k];
        k++;
    }
    idct_block(c->plane + (size_t)by * 8 * (size_t)c->pbw * 8 + (size_t)bx * 8, c->pbw * 8, blk);
    return 0;
}

static int decode_block_prog(struct jpeg *j, struct comp *c, int16_t *b, const struct scan *s)
{
    if (s->ss == 0) {                                   /* DC */
        if (s->ah == 0) {
            int t = huff_decode(j, &j->dc[c->td]);
            if (t < 0 || t > 11)
                return -1;
            int diff = t ? extend(get_bits(j, t), t) : 0;
            c->dc_pred += diff;
            b[0] = (int16_t)(c->dc_pred * (1 << s->al));
        } else if (get_bit(j)) {
            b[0] = (int16_t)(b[0] | (1 << s->al));
        }
        return 0;
    }
    const struct huff *ha = &j->ac[c->ta];
    if (s->ah == 0) {                                   /* AC, first pass */
        if (j->eobrun > 0) {
            j->eobrun--;
            return 0;
        }
        for (int k = s->ss; k <= s->se; k++) {
            int rs = huff_decode(j, ha);
            if (rs < 0)
                return -1;
            int r = rs >> 4, sz = rs & 15;
            if (sz) {
                k += r;
                if (k > 63)
                    return -1;
                b[zigzag[k]] = (int16_t)(extend(get_bits(j, sz), sz) * (1 << s->al));
            } else if (r < 15) {
                j->eobrun = (1 << r) - 1;
                if (r)
                    j->eobrun += get_bits(j, r);
                break;
            } else {
                k += 15;
            }
        }
        return 0;
    }
    /* AC, refinement */
    int p1 = 1 << s->al, m1 = -(1 << s->al);
    int k = s->ss;
    if (j->eobrun <= 0) {
        for (; k <= s->se; k++) {
            int rs = huff_decode(j, ha);
            if (rs < 0)
                return -1;
            int r = rs >> 4, sz = rs & 15, val = 0;
            if (sz) {
                if (sz != 1)
                    return -1;
                val = get_bit(j) ? p1 : m1;
            } else if (r != 15) {
                j->eobrun = 1 << r;
                if (r)
                    j->eobrun += get_bits(j, r);
                break;
            }
            do {
                int16_t *cf = &b[zigzag[k]];
                if (*cf) {
                    if (get_bit(j) && (*cf & p1) == 0)
                        *cf = (int16_t)(*cf + (*cf >= 0 ? p1 : m1));
                } else if (--r < 0) {
                    break;
                }
                k++;
            } while (k <= s->se);
            if (val) {
                if (k > 63)
                    return -1;
                b[zigzag[k]] = (int16_t)val;
            }
        }
    }
    if (j->eobrun > 0) {
        for (; k <= s->se; k++) {
            int16_t *cf = &b[zigzag[k]];
            if (*cf && get_bit(j) && (*cf & p1) == 0)
                *cf = (int16_t)(*cf + (*cf >= 0 ? p1 : m1));
        }
        j->eobrun--;
    }
    return 0;
}

/* After a restart interval: byte align, find RSTn, reset the predictors. */
static int do_restart(struct jpeg *j, const struct scan *s)
{
    reset_bits(j);
    while (j->pos + 1 < j->len && !(j->d[j->pos] == 0xff && j->d[j->pos + 1] >= 0xd0 && j->d[j->pos + 1] <= 0xd7)) {
        if (j->d[j->pos] == 0xff && j->d[j->pos + 1] != 0 && j->d[j->pos + 1] != 0xff)
            return -1;                                  /* some other marker: the restart marker is missing */
        j->pos++;
    }
    if (j->pos + 1 >= j->len)
        return -1;
    j->pos += 2;
    for (int i = 0; i < s->ncomp; i++)
        j->c[s->ci[i]].dc_pred = 0;
    j->eobrun = 0;
    return 0;
}

static int decode_scan(struct jpeg *j, const struct scan *s)
{
    reset_bits(j);
    j->eobrun = 0;
    for (int i = 0; i < s->ncomp; i++)
        j->c[s->ci[i]].dc_pred = 0;
    int count = 0;
    if (s->ncomp == 1) {                                /* one component: its own blocks in raster order, no MCU interleaving */
        struct comp *c = &j->c[s->ci[0]];
        for (int by = 0; by < c->bh; by++) {
            for (int bx = 0; bx < c->bw; bx++) {
                if (j->restart && count && count % j->restart == 0 && do_restart(j, s) < 0)
                    return -1;
                count++;
                int rc = j->progressive ? decode_block_prog(j, c, c->coef + ((size_t)by * c->pbw + bx) * 64, s)
                                        : decode_block_seq(j, c, bx, by);
                if (rc < 0 || j->fed > 64)
                    return -1;
            }
        }
        return 0;
    }
    for (int my = 0; my < j->mcuy; my++) {
        for (int mx = 0; mx < j->mcux; mx++) {
            if (j->restart && count && count % j->restart == 0 && do_restart(j, s) < 0)
                return -1;
            count++;
            for (int i = 0; i < s->ncomp; i++) {
                struct comp *c = &j->c[s->ci[i]];
                for (int v = 0; v < c->v; v++) {
                    for (int h = 0; h < c->h; h++) {
                        int bx = mx * c->h + h, by = my * c->v + v;
                        int rc = j->progressive ? decode_block_prog(j, c, c->coef + ((size_t)by * c->pbw + bx) * 64, s)
                                                : decode_block_seq(j, c, bx, by);
                        if (rc < 0 || j->fed > 64)
                            return -1;
                    }
                }
            }
        }
    }
    return 0;
}

/* ---- markers ------------------------------------------------------------------------------------------------------ */

static int u16(const uint8_t *p) { return p[0] << 8 | p[1]; }

static int parse_dqt(struct jpeg *j, const uint8_t *p, size_t n)
{
    while (n) {
        int pq = p[0] >> 4, tq = p[0] & 15;
        size_t need = 1 + (pq ? 128 : 64);
        if (tq > 3 || pq > 1 || n < need)
            return fail(j, "bad quantization table");
        for (int i = 0; i < 64; i++)
            j->qt[tq][i] = pq ? (uint16_t)u16(p + 1 + i * 2) : p[1 + i];
        j->qdef[tq] = true;
        p += need;
        n -= need;
    }
    return 0;
}

static int parse_dht(struct jpeg *j, const uint8_t *p, size_t n)
{
    while (n >= 17) {
        int tc = p[0] >> 4, th = p[0] & 15, total = 0;
        if (tc > 1 || th > 3)
            return fail(j, "bad Huffman table");
        for (int i = 0; i < 16; i++)
            total += p[1 + i];
        if (total > 256 || n < (size_t)17 + (size_t)total)
            return fail(j, "bad Huffman table");
        if (build_huff(tc ? &j->ac[th] : &j->dc[th], p + 1, p + 17, total) < 0)
            return fail(j, "bad Huffman table");
        p += 17 + total;
        n -= 17 + (size_t)total;
    }
    return n ? fail(j, "bad Huffman table") : 0;
}

static int parse_sof(struct jpeg *j, const uint8_t *p, size_t n, int marker)
{
    if (j->have_frame)
        return fail(j, "second frame header");
    if (n < 6)
        return fail(j, "short JPEG frame header");
    if (p[0] != 8)
        return fail(j, p[0] == 12 ? "12 bit JPEG is not supported" : "unsupported JPEG precision");
    j->h = u16(p + 1);
    j->w = u16(p + 3);
    j->ncomp = p[5];
    if (!j->w || !j->h)
        return fail(j, "JPEG with no size");
    if (j->ncomp == 4)
        return fail(j, "CMYK JPEG is not supported");
    if (j->ncomp != 1 && j->ncomp != 3)
        return fail(j, "unsupported number of JPEG components");
    if (n < 6 + (size_t)j->ncomp * 3)
        return fail(j, "short JPEG frame header");
    if ((uint64_t)j->w * (uint64_t)j->h > LM_IMG_MAX_PIXELS)
        return fail(j, "image too large");
    j->progressive = marker == 0xc2;
    j->hmax = j->vmax = 1;
    for (int i = 0; i < j->ncomp; i++) {
        struct comp *c = &j->c[i];
        c->id = p[6 + i * 3];
        c->h = p[7 + i * 3] >> 4;
        c->v = p[7 + i * 3] & 15;
        c->tq = p[8 + i * 3];
        if (c->h < 1 || c->h > 4 || c->v < 1 || c->v > 4 || c->tq > 3)
            return fail(j, "bad JPEG sampling factors");
        if (c->h > j->hmax)
            j->hmax = c->h;
        if (c->v > j->vmax)
            j->vmax = c->v;
    }
    if (j->ncomp == 1)                                  /* a lone component is one block per MCU whatever its factors say */
        j->c[0].h = j->c[0].v = j->hmax = j->vmax = 1;
    j->mcux = (j->w + 8 * j->hmax - 1) / (8 * j->hmax);
    j->mcuy = (j->h + 8 * j->vmax - 1) / (8 * j->vmax);
    for (int i = 0; i < j->ncomp; i++) {
        struct comp *c = &j->c[i];
        c->w = (j->w * c->h + j->hmax - 1) / j->hmax;
        c->hgt = (j->h * c->v + j->vmax - 1) / j->vmax;
        c->bw = (c->w + 7) / 8;
        c->bh = (c->hgt + 7) / 8;
        c->pbw = j->mcux * c->h;
        c->pbh = j->mcuy * c->v;
        size_t samples = (size_t)c->pbw * 8 * (size_t)c->pbh * 8;
        c->plane = calloc(samples, 1);
        if (!c->plane)
            return fail(j, "out of memory");
        if (j->progressive) {
            c->coef = calloc((size_t)c->pbw * (size_t)c->pbh * 64, sizeof(int16_t));
            if (!c->coef)
                return fail(j, "out of memory");
        }
    }
    j->have_frame = true;
    return 0;
}

static int parse_sos(struct jpeg *j, const uint8_t *p, size_t n, struct scan *s)
{
    if (!j->have_frame)
        return fail(j, "scan before the frame header");
    if (n < 1)
        return fail(j, "bad scan header");
    s->ncomp = p[0];
    if (s->ncomp < 1 || s->ncomp > j->ncomp || n < 4 + (size_t)s->ncomp * 2)
        return fail(j, "bad scan header");
    for (int i = 0; i < s->ncomp; i++) {
        int id = p[1 + i * 2], found = -1;
        for (int k = 0; k < j->ncomp; k++)
            if (j->c[k].id == id)
                found = k;
        if (found < 0)
            return fail(j, "scan names an unknown component");
        j->c[found].td = p[2 + i * 2] >> 4;
        j->c[found].ta = p[2 + i * 2] & 15;
        if (j->c[found].td > 3 || j->c[found].ta > 3)
            return fail(j, "bad Huffman table selector");
        s->ci[i] = found;
    }
    const uint8_t *t = p + 1 + s->ncomp * 2;
    s->ss = t[0];
    s->se = t[1];
    s->ah = t[2] >> 4;
    s->al = t[2] & 15;
    if (!j->progressive) {
        s->ss = 0;
        s->se = 63;
        s->ah = s->al = 0;
    } else if (s->ss > s->se || s->se > 63 || s->al > 13 || (s->ss == 0 && s->se != 0) || (s->ss > 0 && s->ncomp != 1)) {
        return fail(j, "bad progressive scan parameters");
    }
    for (int i = 0; i < s->ncomp; i++) {
        struct comp *c = &j->c[s->ci[i]];
        if (!j->qdef[c->tq])
            return fail(j, "missing quantization table");
        bool need_dc = !j->progressive || (s->ss == 0 && s->ah == 0);
        bool need_ac = !j->progressive || s->ss > 0;
        if ((need_dc && !j->dc[c->td].defined) || (need_ac && !j->ac[c->ta].defined))
            return fail(j, "missing Huffman table");
    }
    return 0;
}

/* EXIF (APP1): only the orientation of IFD0. */
static void parse_exif(struct jpeg *j, const uint8_t *p, size_t n)
{
    if (n < 14 || memcmp(p, "Exif\0\0", 6))
        return;
    const uint8_t *t = p + 6;
    size_t tn = n - 6;
    bool le = t[0] == 'I' && t[1] == 'I';
    if (!le && !(t[0] == 'M' && t[1] == 'M'))
        return;
#define RD16(q) (le ? (q)[0] | (q)[1] << 8 : (q)[0] << 8 | (q)[1])
#define RD32(q) (le ? (uint32_t)(q)[0] | (uint32_t)(q)[1] << 8 | (uint32_t)(q)[2] << 16 | (uint32_t)(q)[3] << 24 \
                    : (uint32_t)(q)[0] << 24 | (uint32_t)(q)[1] << 16 | (uint32_t)(q)[2] << 8 | (uint32_t)(q)[3])
    if (RD16(t + 2) != 42)
        return;
    uint32_t off = RD32(t + 4);
    if (off > tn || tn - off < 2)
        return;
    int count = RD16(t + off);
    for (int i = 0; i < count; i++) {
        size_t e = (size_t)off + 2 + (size_t)i * 12;
        if (e + 12 > tn)
            return;
        if (RD16(t + e) == 0x0112 && RD16(t + e + 2) == 3) {
            int o = RD16(t + e + 8);
            if (o >= 1 && o <= 8)
                j->orientation = o;
            return;
        }
    }
#undef RD16
#undef RD32
}

/* ---- output ------------------------------------------------------------------------------------------------------- */

/* The row `y` of a component stretched to the full image width: the triangle filter where it is half size, else the nearest sample. */
static void upsample_row(const struct jpeg *j, const struct comp *c, int y, uint8_t *out, int *tmp)
{
    int stride = c->pbw * 8;
    int cw = c->w, chh = c->hgt;
    bool vup = c->v * 2 == j->vmax, hup = c->h * 2 == j->hmax;
    int sy = vup ? y >> 1 : (c->v == j->vmax ? y : y * c->v / j->vmax);
    if (sy >= chh)
        sy = chh - 1;
    const uint8_t *r0 = c->plane + (size_t)sy * stride;
    if (vup) {                                          /* blend with the nearer neighbouring row: 3/4 + 1/4 */
        int ny = (y & 1) ? sy + 1 : sy - 1;
        ny = ny < 0 ? 0 : ny >= chh ? chh - 1 : ny;
        const uint8_t *r1 = c->plane + (size_t)ny * stride;
        for (int x = 0; x < cw; x++)
            tmp[x] = 3 * r0[x] + r1[x];                 /* scaled by 4 */
    } else {
        for (int x = 0; x < cw; x++)
            tmp[x] = 4 * r0[x];
    }
    for (int x = 0; x < j->w; x++) {
        int sx = hup ? x >> 1 : (c->h == j->hmax ? x : x * c->h / j->hmax);
        if (sx >= cw)
            sx = cw - 1;
        if (hup) {
            int nx = (x & 1) ? sx + 1 : sx - 1;
            nx = nx < 0 ? 0 : nx >= cw ? cw - 1 : nx;
            out[x] = (uint8_t)((3 * tmp[sx] + tmp[nx] + 8) >> 4);
        } else {
            out[x] = (uint8_t)((tmp[sx] + 2) >> 2);
        }
    }
}

static int to_pixels(struct jpeg *j, struct lm_image *img)
{
    size_t npx = (size_t)j->w * (size_t)j->h;
    uint32_t *px = malloc(npx * sizeof(uint32_t));
    uint8_t *row = malloc((size_t)j->w * MAXC);
    int *tmp = malloc(((size_t)j->mcux * 8 * 4 + 16) * sizeof(int));
    if (!px || !row || !tmp) {
        free(px);
        free(row);
        free(tmp);
        return fail(j, "out of memory");
    }
    bool rgb = j->ncomp == 3 && (j->adobe == 0 || (j->c[0].id == 'R' && j->c[1].id == 'G' && j->c[2].id == 'B'));
    for (int y = 0; y < j->h; y++) {
        uint32_t *o = px + (size_t)y * (size_t)j->w;
        if (j->ncomp == 1) {
            const uint8_t *src = j->c[0].plane + (size_t)y * j->c[0].pbw * 8;
            for (int x = 0; x < j->w; x++)
                o[x] = (uint32_t)src[x] * 0x010101u;
            continue;
        }
        for (int i = 0; i < 3; i++)
            upsample_row(j, &j->c[i], y, row + (size_t)i * j->w, tmp);
        const uint8_t *Y = row, *Cb = row + j->w, *Cr = row + 2 * (size_t)j->w;
        for (int x = 0; x < j->w; x++) {
            if (rgb) {
                o[x] = (uint32_t)Y[x] << 16 | (uint32_t)Cb[x] << 8 | Cr[x];
            } else {
                int yy = Y[x] << 16, cb = Cb[x] - 128, cr = Cr[x] - 128;
                int r = (yy + 91881 * cr + 32768) >> 16;
                int g = (yy - 22554 * cb - 46802 * cr + 32768) >> 16;
                int b = (yy + 116130 * cb + 32768) >> 16;
                o[x] = (uint32_t)clamp8(r) << 16 | (uint32_t)clamp8(g) << 8 | clamp8(b);
            }
        }
    }
    free(row);
    free(tmp);
    img->w = j->w;
    img->h = j->h;
    img->px = px;
    return 0;
}

/* EXIF orientation 2..8: turn / mirror the picture so it shows the way the camera held it. */
static int orient(struct lm_image *img, int o)
{
    if (o < 2 || o > 8)
        return 0;
    int w = img->w, h = img->h, nw = o >= 5 ? h : w, nh = o >= 5 ? w : h;
    uint32_t *n = malloc((size_t)nw * (size_t)nh * sizeof(uint32_t));
    if (!n)
        return -1;
    for (int y = 0; y < nh; y++) {
        for (int x = 0; x < nw; x++) {
            int sx, sy;
            switch (o) {
            case 2: sx = w - 1 - x; sy = y; break;
            case 3: sx = w - 1 - x; sy = h - 1 - y; break;
            case 4: sx = x; sy = h - 1 - y; break;
            case 5: sx = y; sy = x; break;
            case 6: sx = y; sy = h - 1 - x; break;
            case 7: sx = w - 1 - y; sy = h - 1 - x; break;
            default: sx = w - 1 - y; sy = x; break;
            }
            n[(size_t)y * (size_t)nw + (size_t)x] = img->px[(size_t)sy * (size_t)w + (size_t)sx];
        }
    }
    free(img->px);
    img->px = n;
    img->w = nw;
    img->h = nh;
    return 0;
}

/* Progressive: every block's coefficients are complete, now dequantize and transform them. */
static void finish_progressive(struct jpeg *j)
{
    for (int i = 0; i < j->ncomp; i++) {
        struct comp *c = &j->c[i];
        int q[64];
        for (int k = 0; k < 64; k++)
            q[zigzag[k]] = j->qt[c->tq][k];
        for (int by = 0; by < c->pbh; by++) {
            for (int bx = 0; bx < c->pbw; bx++) {
                const int16_t *cf = c->coef + ((size_t)by * c->pbw + bx) * 64;
                int d[64];
                for (int k = 0; k < 64; k++)
                    d[k] = cf[k] * q[k];
                idct_block(c->plane + (size_t)by * 8 * (size_t)c->pbw * 8 + (size_t)bx * 8, c->pbw * 8, d);
            }
        }
    }
}

int lm_jpeg_decode(const uint8_t *data, size_t len, struct lm_image *img, char *err, size_t errsz)
{
    struct jpeg *j = calloc(1, sizeof(*j));
    if (!j) {
        snprintf(err, errsz, "out of memory");
        return -1;
    }
    j->d = data;
    j->len = len;
    j->err = err;
    j->errsz = errsz;
    j->adobe = -1;
    j->orientation = 1;
    int rc = -1;
    bool got_scan = false;

    if (len < 4 || data[0] != 0xff || data[1] != 0xd8) {
        fail(j, "not a JPEG");
        goto done;
    }
    j->pos = 2;
    for (;;) {
        while (j->pos < len && data[j->pos] != 0xff)             /* garbage between segments */
            j->pos++;
        while (j->pos < len && data[j->pos] == 0xff)             /* fill bytes */
            j->pos++;
        if (j->pos >= len) {
            if (got_scan)
                break;                                           /* no EOI but we have the picture */
            fail(j, "JPEG ends too soon");
            goto done;
        }
        int m = data[j->pos++];
        if (m == 0xd9)
            break;                                               /* EOI */
        if (m == 0 || m == 0x01 || (m >= 0xd0 && m <= 0xd8))
            continue;                                            /* no length: stuffing, TEM, RSTn, SOI */
        if (j->pos + 2 > len) {
            fail(j, "JPEG ends too soon");
            goto done;
        }
        size_t seg = (size_t)u16(data + j->pos);
        if (seg < 2 || j->pos + seg > len) {
            fail(j, "damaged JPEG segment");
            goto done;
        }
        const uint8_t *p = data + j->pos + 2;
        size_t n = seg - 2;
        j->pos += seg;
        switch (m) {
        case 0xc0: case 0xc1: case 0xc2:
            if (parse_sof(j, p, n, m) < 0)
                goto done;
            break;
        case 0xc3: case 0xc5: case 0xc6: case 0xc7: case 0xc9: case 0xca: case 0xcb: case 0xcd: case 0xce: case 0xcf:
            fail(j, "this kind of JPEG (lossless, hierarchical or arithmetic coding) is not supported");
            goto done;
        case 0xc4:
            if (parse_dht(j, p, n) < 0)
                goto done;
            break;
        case 0xdb:
            if (parse_dqt(j, p, n) < 0)
                goto done;
            break;
        case 0xdd:
            if (n < 2) {
                fail(j, "bad restart interval");
                goto done;
            }
            j->restart = u16(p);
            break;
        case 0xe1:
            parse_exif(j, p, n);
            break;
        case 0xee:
            if (n >= 12 && !memcmp(p, "Adobe", 5))
                j->adobe = p[11];
            break;
        case 0xda: {
            struct scan s;
            if (parse_sos(j, p, n, &s) < 0)
                goto done;
            if (decode_scan(j, &s) < 0) {
                if (!got_scan || !j->progressive) {
                    fail(j, "damaged JPEG data");
                    goto done;
                }
                /* a damaged later progressive scan: keep what the earlier ones gave */
            }
            got_scan = true;
            /* the entropy data ends at the next real marker */
            while (j->pos + 1 < len && !(data[j->pos] == 0xff && data[j->pos + 1] != 0 && !(data[j->pos + 1] >= 0xd0 && data[j->pos + 1] <= 0xd7)))
                j->pos++;
            break;
        }
        default:
            break;                                               /* APPn, COM and the rest are skipped */
        }
    }
    if (!j->have_frame || !got_scan) {
        fail(j, "JPEG has no picture data");
        goto done;
    }
    if (j->progressive)
        finish_progressive(j);
    if (to_pixels(j, img) < 0)
        goto done;
    if (orient(img, j->orientation) < 0) {
        free(img->px);
        img->px = NULL;
        fail(j, "out of memory");
        goto done;
    }
    rc = 0;
done:
    for (int i = 0; i < MAXC; i++) {
        free(j->c[i].plane);
        free(j->c[i].coef);
    }
    free(j);
    return rc;
}
