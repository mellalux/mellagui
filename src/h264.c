#include "h264.h"

#include <stdlib.h>
#include <string.h>

/* ---- bit writer ------------------------------------------------------------------------------------------------------------ */

static void bw_byte(struct lm_bw *b, uint8_t v)
{
    if (b->n == b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 4096;
        uint8_t *np = realloc(b->p, nc);
        if (!np) {
            b->oom = 1;
            return;
        }
        b->p = np;
        b->cap = nc;
    }
    b->p[b->n++] = v;
}

void lm_bw_put(struct lm_bw *b, uint32_t value, int nbits)
{
    if (nbits <= 0)
        return;
    b->acc = (b->acc << nbits) | (nbits == 32 ? value : (value & ((1u << nbits) - 1)));
    b->nacc += nbits;
    while (b->nacc >= 8) {
        bw_byte(b, (uint8_t)(b->acc >> (b->nacc - 8)));
        b->nacc -= 8;
    }
    b->acc &= b->nacc ? ((1ull << b->nacc) - 1) : 0;
}

void lm_bw_ue(struct lm_bw *b, uint32_t v)
{
    uint64_t x = (uint64_t)v + 1;
    int len = 0;
    while ((x >> len) > 1)
        len++;
    lm_bw_put(b, 0, len);
    lm_bw_put(b, (uint32_t)x, len + 1);            /* len <= 31 for 32 bit values: len + 1 <= 32 */
}

void lm_bw_se(struct lm_bw *b, int32_t v)
{
    lm_bw_ue(b, v > 0 ? (uint32_t)(2 * v - 1) : (uint32_t)(-2 * (int64_t)v));
}

void lm_bw_trailing(struct lm_bw *b)
{
    lm_bw_put(b, 1, 1);
    if (b->nacc)
        lm_bw_put(b, 0, 8 - b->nacc);
}

size_t lm_bw_bits(const struct lm_bw *b)
{
    return b->n * 8 + (size_t)b->nacc;
}

void lm_bw_free(struct lm_bw *b)
{
    free(b->p);
    memset(b, 0, sizeof(*b));
}

/* ---- bit reader -------------------------------------------------------------------------------------------------------------- */

uint32_t lm_br_peek(const struct lm_br *r, int nbits)
{
    uint32_t v = 0;
    for (int i = 0; i < nbits; i++) {
        size_t at = r->pos + (size_t)i;
        uint32_t bit = at < r->nbits ? (uint32_t)(r->p[at >> 3] >> (7 - (at & 7))) & 1u : 0;
        v = (v << 1) | bit;
    }
    return v;
}

uint32_t lm_br_get(struct lm_br *r, int nbits)
{
    uint32_t v = lm_br_peek(r, nbits);
    r->pos += (size_t)nbits;
    return v;
}

int lm_br_ue(struct lm_br *r, uint32_t *v)
{
    int zeros = 0;
    for (;;) {
        if (r->pos + (size_t)zeros >= r->nbits || zeros > 31)
            return -1;
        size_t at = r->pos + (size_t)zeros;
        if ((r->p[at >> 3] >> (7 - (at & 7))) & 1)
            break;
        zeros++;
    }
    r->pos += (size_t)zeros + 1;
    uint32_t rest = 0;
    for (int i = 0; i < zeros; i++) {
        size_t at = r->pos + (size_t)i;
        if (at >= r->nbits)
            return -1;
        rest = (rest << 1) | ((uint32_t)(r->p[at >> 3] >> (7 - (at & 7))) & 1u);
    }
    r->pos += (size_t)zeros;
    *v = (uint32_t)(((1ull << zeros) - 1) + rest);
    return 0;
}

int lm_br_se(struct lm_br *r, int32_t *v)
{
    uint32_t u;
    if (lm_br_ue(r, &u) < 0)
        return -1;
    *v = (u & 1) ? (int32_t)((u + 1) / 2) : -(int32_t)(u / 2);
    return 0;
}

/* ---- NAL units ----------------------------------------------------------------------------------------------------------------- */

uint8_t *lm_h264_nal(int ref_idc, int type, const uint8_t *rbsp, size_t n, size_t *out_len)
{
    uint8_t *out = malloc(1 + n + n / 2 + 8);
    if (!out)
        return NULL;
    size_t o = 0;
    out[o++] = (uint8_t)((ref_idc << 5) | type);
    int zeros = 0;
    for (size_t i = 0; i < n; i++) {
        if (zeros >= 2 && rbsp[i] <= 3) {
            out[o++] = 3;
            zeros = 0;
        }
        out[o++] = rbsp[i];
        zeros = rbsp[i] == 0 ? zeros + 1 : 0;
    }
    *out_len = o;
    return out;
}

size_t lm_h264_unescape(const uint8_t *nal, size_t n, uint8_t *out)
{
    size_t o = 0;
    int zeros = 0;
    for (size_t i = 0; i < n; i++) {
        if (zeros >= 2 && nal[i] == 3) {
            zeros = 0;
            continue;
        }
        out[o++] = nal[i];
        zeros = nal[i] == 0 ? zeros + 1 : 0;
    }
    return o;
}

/* ---- tables ---------------------------------------------------------------------------------------------------------------------- */

const uint8_t lm_h264_coeff_token_len[4][68] = {
    { 1, 0, 0, 0,
      6, 2, 0, 0,     8, 6, 3, 0,     9, 8, 7, 5,    10, 9, 8, 6,
     11,10, 9, 7,    13,11,10, 8,    13,13,11, 9,    13,13,13,10,
     14,14,13,11,    14,14,14,13,    15,15,14,14,    15,15,15,14,
     16,15,15,15,    16,16,16,15,    16,16,16,16,    16,16,16,16 },
    { 2, 0, 0, 0,
      6, 2, 0, 0,     6, 5, 3, 0,     7, 6, 6, 4,     8, 6, 6, 4,
      8, 7, 7, 5,     9, 8, 8, 6,    11, 9, 9, 6,    11,11,11, 7,
     12,11,11, 9,    12,12,12,11,    12,12,12,11,    13,13,13,12,
     13,13,13,13,    13,14,13,13,    14,14,14,13,    14,14,14,14 },
    { 4, 0, 0, 0,
      6, 4, 0, 0,     6, 5, 4, 0,     6, 5, 5, 4,     7, 5, 5, 4,
      7, 5, 5, 4,     7, 6, 6, 4,     7, 6, 6, 4,     8, 7, 7, 5,
      8, 8, 7, 6,     9, 8, 8, 7,     9, 9, 8, 8,     9, 9, 9, 8,
     10, 9, 9, 9,    10,10,10,10,    10,10,10,10,    10,10,10,10 },
    { 6, 0, 0, 0,
      6, 6, 0, 0,     6, 6, 6, 0,     6, 6, 6, 6,     6, 6, 6, 6,
      6, 6, 6, 6,     6, 6, 6, 6,     6, 6, 6, 6,     6, 6, 6, 6,
      6, 6, 6, 6,     6, 6, 6, 6,     6, 6, 6, 6,     6, 6, 6, 6,
      6, 6, 6, 6,     6, 6, 6, 6,     6, 6, 6, 6,     6, 6, 6, 6 },
};

const uint8_t lm_h264_coeff_token_bits[4][68] = {
    { 1, 0, 0, 0,
      5, 1, 0, 0,     7, 4, 1, 0,     7, 6, 5, 3,     7, 6, 5, 3,
      7, 6, 5, 4,    15, 6, 5, 4,    11,14, 5, 4,     8,10,13, 4,
     15,14, 9, 4,    11,10,13,12,    15,14, 9,12,    11,10,13, 8,
     15, 1, 9,12,    11,14,13, 8,     7,10, 9,12,     4, 6, 5, 8 },
    { 3, 0, 0, 0,
     11, 2, 0, 0,     7, 7, 3, 0,     7,10, 9, 5,     7, 6, 5, 4,
      4, 6, 5, 6,     7, 6, 5, 8,    15, 6, 5, 4,    11,14,13, 4,
     15,10, 9, 4,    11,14,13,12,     8,10, 9, 8,    15,14,13,12,
     11,10, 9,12,     7,11, 6, 8,     9, 8,10, 1,     7, 6, 5, 4 },
    {15, 0, 0, 0,
     15,14, 0, 0,    11,15,13, 0,     8,12,14,12,    15,10,11,11,
     11, 8, 9,10,     9,14,13, 9,     8,10, 9, 8,    15,14,13,13,
     11,14,10,12,    15,10,13,12,    11,14, 9,12,     8,10,13, 8,
     13, 7, 9,12,     9,12,11,10,     5, 8, 7, 6,     1, 4, 3, 2 },
    { 3, 0, 0, 0,
      0, 1, 0, 0,     4, 5, 6, 0,     8, 9,10,11,    12,13,14,15,
     16,17,18,19,    20,21,22,23,    24,25,26,27,    28,29,30,31,
     32,33,34,35,    36,37,38,39,    40,41,42,43,    44,45,46,47,
     48,49,50,51,    52,53,54,55,    56,57,58,59,    60,61,62,63 },
};

const uint8_t lm_h264_cdc_token_len[20] = { 2, 0, 0, 0,  6, 1, 0, 0,  6, 6, 3, 0,  6, 7, 7, 6,  6, 8, 8, 7 };
const uint8_t lm_h264_cdc_token_bits[20] = { 1, 0, 0, 0,  7, 1, 0, 0,  4, 6, 1, 0,  3, 3, 2, 5,  2, 3, 2, 0 };

const uint8_t lm_h264_total_zeros_len[15][16] = {
    {1,3,3,4,4,5,5,6,6,7,7,8,8,9,9,9},
    {3,3,3,3,3,4,4,4,4,5,5,6,6,6,6},
    {4,3,3,3,4,4,3,3,4,5,5,6,5,6},
    {5,3,4,4,3,3,3,4,3,4,5,5,5},
    {4,4,4,3,3,3,3,3,4,5,4,5},
    {6,5,3,3,3,3,3,3,4,3,6},
    {6,5,3,3,3,2,3,4,3,6},
    {6,4,5,3,2,2,3,3,6},
    {6,6,4,2,2,3,2,5},
    {5,5,3,2,2,2,4},
    {4,4,3,3,1,3},
    {4,4,2,1,3},
    {3,3,1,2},
    {2,2,1},
    {1,1},
};

const uint8_t lm_h264_total_zeros_bits[15][16] = {
    {1,3,2,3,2,3,2,3,2,3,2,3,2,3,2,1},
    {7,6,5,4,3,5,4,3,2,3,2,3,2,1,0},
    {5,7,6,5,4,3,4,3,2,3,2,1,1,0},
    {3,7,5,4,6,5,4,3,3,2,2,1,0},
    {5,4,3,7,6,5,4,3,2,1,1,0},
    {1,1,7,6,5,4,3,2,1,1,0},
    {1,1,5,4,3,3,2,1,1,0},
    {1,1,1,3,3,2,2,1,0},
    {1,0,1,3,2,1,1,1},
    {1,0,1,3,2,1,1},
    {0,1,1,2,1,3},
    {0,1,1,1,1},
    {0,1,1,1},
    {0,1,1},
    {0,1},
};

const uint8_t lm_h264_cdc_zeros_len[3][4] = { { 1, 2, 3, 3 }, { 1, 2, 2, 0 }, { 1, 1, 0, 0 } };
const uint8_t lm_h264_cdc_zeros_bits[3][4] = { { 1, 1, 1, 0 }, { 1, 1, 0, 0 }, { 1, 0, 0, 0 } };

const uint8_t lm_h264_run_len[7][16] = {
    {1,1},
    {1,2,2},
    {2,2,2,2},
    {2,2,2,3,3},
    {2,2,3,3,3,3},
    {2,3,3,3,3,3,3},
    {3,3,3,3,3,3,3,4,5,6,7,8,9,10,11},
};

const uint8_t lm_h264_run_bits[7][16] = {
    {1,0},
    {1,1,0},
    {3,2,1,0},
    {3,2,1,1,0},
    {3,2,3,2,1,0},
    {3,0,1,3,2,5,4},
    {7,6,5,4,3,2,1,1,1,1,1,1,1,1,1},
};

const uint8_t lm_h264_inter_cbp[48] = {
    0,  16, 1,  2,  4,  8,  32, 3,  5,  10, 12, 15, 47, 7,  11, 13,
    14, 6,  9,  31, 35, 37, 42, 44, 33, 34, 36, 40, 39, 43, 45, 46,
    17, 18, 20, 24, 19, 21, 26, 28, 23, 27, 29, 30, 22, 25, 38, 41,
};

const uint8_t lm_h264_zigzag4[16] = { 0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15 };
const uint8_t lm_h264_blk_x[16] = { 0, 1, 0, 1, 2, 3, 2, 3, 0, 1, 0, 1, 2, 3, 2, 3 };
const uint8_t lm_h264_blk_y[16] = { 0, 0, 1, 1, 0, 0, 1, 1, 2, 2, 3, 3, 2, 2, 3, 3 };

int lm_h264_chroma_qp(int qp)
{
    static const uint8_t t[22] = { 29, 30, 31, 32, 32, 33, 34, 34, 35, 35, 36, 36, 37, 37, 37, 38, 38, 38, 39, 39, 39, 39 };
    return qp < 30 ? qp : t[qp - 30];
}

/* ---- transforms ------------------------------------------------------------------------------------------------------------------ */

static const int mf_tab[6][3] = {
    { 13107, 5243, 8066 }, { 11916, 4660, 7490 }, { 10082, 4194, 6554 },
    { 9362, 3647, 5825 }, { 8192, 3355, 5243 }, { 7282, 2893, 4559 },
};
static const int v_tab[6][3] = {
    { 10, 16, 13 }, { 11, 18, 14 }, { 13, 20, 16 }, { 14, 23, 18 }, { 16, 25, 20 }, { 18, 29, 23 },
};

static int pos_class(int i)                          /* raster index of a 4x4 block -> 0: both even, 1: both odd, 2: mixed */
{
    int r = i >> 2, c = i & 3;
    return (r & 1) == 0 && (c & 1) == 0 ? 0 : (r & 1) == 1 && (c & 1) == 1 ? 1 : 2;
}

void lm_h264_fdct4(const int *r, int *w)
{
    int t[16];
    for (int i = 0; i < 4; i++) {
        const int *p = r + i * 4;
        int a0 = p[0] + p[3], a1 = p[1] + p[2], a2 = p[1] - p[2], a3 = p[0] - p[3];
        t[i * 4 + 0] = a0 + a1;
        t[i * 4 + 2] = a0 - a1;
        t[i * 4 + 1] = 2 * a3 + a2;
        t[i * 4 + 3] = a3 - 2 * a2;
    }
    for (int j = 0; j < 4; j++) {
        int a0 = t[j] + t[12 + j], a1 = t[4 + j] + t[8 + j], a2 = t[4 + j] - t[8 + j], a3 = t[j] - t[12 + j];
        w[j] = a0 + a1;
        w[8 + j] = a0 - a1;
        w[4 + j] = 2 * a3 + a2;
        w[12 + j] = a3 - 2 * a2;
    }
}

int lm_h264_quant4(const int *w, int *levels, int qp, int intra, int skip_dc)
{
    int per = qp / 6, rem = qp % 6, qbits = 15 + per, nz = 0;
    int f = (1 << qbits) / (intra ? 3 : 6);
    for (int i = 0; i < 16; i++) {
        if (skip_dc && i == 0) {
            levels[0] = 0;
            continue;
        }
        int a = w[i] < 0 ? -w[i] : w[i];
        int l = (int)(((int64_t)a * mf_tab[rem][pos_class(i)] + f) >> qbits);
        levels[i] = w[i] < 0 ? -l : l;
        nz += l != 0;
    }
    return nz;
}

static void idct4(const int *d, int *r)
{
    int t[16];
    for (int i = 0; i < 4; i++) {
        const int *p = d + i * 4;
        int e0 = p[0] + p[2], e1 = p[0] - p[2], e2 = (p[1] >> 1) - p[3], e3 = p[1] + (p[3] >> 1);
        t[i * 4 + 0] = e0 + e3;
        t[i * 4 + 1] = e1 + e2;
        t[i * 4 + 2] = e1 - e2;
        t[i * 4 + 3] = e0 - e3;
    }
    for (int j = 0; j < 4; j++) {
        int e0 = t[j] + t[8 + j], e1 = t[j] - t[8 + j], e2 = (t[4 + j] >> 1) - t[12 + j], e3 = t[4 + j] + (t[12 + j] >> 1);
        r[j] = (e0 + e3 + 32) >> 6;
        r[4 + j] = (e1 + e2 + 32) >> 6;
        r[8 + j] = (e1 - e2 + 32) >> 6;
        r[12 + j] = (e0 - e3 + 32) >> 6;
    }
}

void lm_h264_dequant_idct4(const int *levels, int qp, const int *dc, int *r)
{
    int per = qp / 6, rem = qp % 6, d[16];
    for (int i = 0; i < 16; i++)
        d[i] = levels[i] * v_tab[rem][pos_class(i)] * (1 << per);
    if (dc)
        d[0] = *dc;
    idct4(d, r);
}

/* 4x4 Hadamard (unnormalized): out = H in H with H = [1 1 1 1; 1 1 -1 -1; 1 -1 -1 1; 1 -1 1 -1] */
static void hadamard4(const int *in, int *out)
{
    int t[16];
    for (int i = 0; i < 4; i++) {
        const int *p = in + i * 4;
        int a = p[0] + p[1], b = p[0] - p[1], c = p[2] + p[3], d = p[2] - p[3];
        t[i * 4 + 0] = a + c;
        t[i * 4 + 1] = a - c;
        t[i * 4 + 2] = b - d;
        t[i * 4 + 3] = b + d;
    }
    for (int j = 0; j < 4; j++) {
        int a = t[j] + t[4 + j], b = t[j] - t[4 + j], c = t[8 + j] + t[12 + j], d = t[8 + j] - t[12 + j];
        out[j] = a + c;
        out[4 + j] = a - c;
        out[8 + j] = b - d;
        out[12 + j] = b + d;
    }
}

int lm_h264_quant_luma_dc(const int *dc16, int *levels, int qp)
{
    int h[16], per = qp / 6, rem = qp % 6, qbits = 15 + per, nz = 0;
    hadamard4(dc16, h);
    int f = (1 << qbits) / 3;
    for (int i = 0; i < 16; i++) {
        int v = h[i] / 2;                                      /* (the forward transform halves the Hadamard sums) */
        int a = v < 0 ? -v : v;
        int l = (int)(((int64_t)a * mf_tab[rem][0] + 2 * f) >> (qbits + 1));
        levels[i] = v < 0 ? -l : l;
        nz += l != 0;
    }
    return nz;
}

void lm_h264_dequant_luma_dc(const int *levels, int qp, int *dc16)
{
    int f[16], per = qp / 6, rem = qp % 6, ls = v_tab[rem][0] * 16;
    hadamard4(levels, f);
    for (int i = 0; i < 16; i++)
        dc16[i] = qp >= 36 ? f[i] * ls * (1 << (per - 6)) : (f[i] * ls + (1 << (5 - per))) >> (6 - per);
}

int lm_h264_quant_chroma_dc(const int *dc4, int *levels, int qpc)
{
    int per = qpc / 6, rem = qpc % 6, qbits = 15 + per, nz = 0;
    int h[4] = { dc4[0] + dc4[1] + dc4[2] + dc4[3], dc4[0] - dc4[1] + dc4[2] - dc4[3], dc4[0] + dc4[1] - dc4[2] - dc4[3],
                 dc4[0] - dc4[1] - dc4[2] + dc4[3] };
    int f = (1 << qbits) / 3;
    for (int i = 0; i < 4; i++) {
        int a = h[i] < 0 ? -h[i] : h[i];
        int l = (int)(((int64_t)a * mf_tab[rem][0] + 2 * f) >> (qbits + 1));
        levels[i] = h[i] < 0 ? -l : l;
        nz += l != 0;
    }
    return nz;
}

void lm_h264_dequant_chroma_dc(const int *levels, int qpc, int *dc4)
{
    int per = qpc / 6, rem = qpc % 6, ls = v_tab[rem][0] * 16;
    int f[4] = { levels[0] + levels[1] + levels[2] + levels[3], levels[0] - levels[1] + levels[2] - levels[3],
                 levels[0] + levels[1] - levels[2] - levels[3], levels[0] - levels[1] - levels[2] + levels[3] };
    for (int i = 0; i < 4; i++)
        dc4[i] = (f[i] * ls * (1 << per)) >> 5;
}

/* ---- intra prediction ---------------------------------------------------------------------------------------------------------- */

int lm_h264_luma_dc_pred(const uint8_t *Y, int ys, int mbx, int mby)
{
    const uint8_t *p = Y + (size_t)mby * 16 * (size_t)ys + (size_t)mbx * 16;
    int top = mby > 0, left = mbx > 0, st = 0, sl = 0;
    if (top)
        for (int i = 0; i < 16; i++)
            st += p[-ys + i];
    if (left)
        for (int i = 0; i < 16; i++)
            sl += p[(size_t)i * (size_t)ys - 1];
    return top && left ? (st + sl + 16) >> 5 : left ? (sl + 8) >> 4 : top ? (st + 8) >> 4 : 128;
}

int lm_h264_chroma_dc_pred(const uint8_t *plane, int cs, int mbx, int mby, int bx, int by)
{
    const uint8_t *p = plane + (size_t)mby * 8 * (size_t)cs + (size_t)mbx * 8;
    int top = mby > 0, left = mbx > 0, sT = 0, sL = 0;
    if (top)
        for (int i = 0; i < 4; i++)
            sT += p[-cs + bx * 4 + i];
    if (left)
        for (int i = 0; i < 4; i++)
            sL += p[(size_t)(by * 4 + i) * (size_t)cs - 1];
    if ((bx == 0 && by == 0) || (bx > 0 && by > 0))
        return top && left ? (sT + sL + 4) >> 3 : left ? (sL + 2) >> 2 : top ? (sT + 2) >> 2 : 128;
    if (bx > 0 && by == 0)
        return top ? (sT + 2) >> 2 : left ? (sL + 2) >> 2 : 128;
    return left ? (sL + 2) >> 2 : top ? (sT + 2) >> 2 : 128;
}
