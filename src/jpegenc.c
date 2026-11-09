#include "jpegenc.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---- tables (T.81 Annex K) ------------------------------------------------------------------------------------------- */

static const uint8_t zigzag[64] = {
     0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

static const uint8_t qlum[64] = {            /* natural (row major) order */
    16, 11, 10, 16,  24,  40,  51,  61,
    12, 12, 14, 19,  26,  58,  60,  55,
    14, 13, 16, 24,  40,  57,  69,  56,
    14, 17, 22, 29,  51,  87,  80,  62,
    18, 22, 37, 56,  68, 109, 103,  77,
    24, 35, 55, 64,  81, 104, 113,  92,
    49, 64, 78, 87, 103, 121, 120, 101,
    72, 92, 95, 98, 112, 100, 103,  99,
};
static const uint8_t qchr[64] = {
    17, 18, 24, 47, 99, 99, 99, 99,
    18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99,
    47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
};

static const uint8_t dc_lum_n[16] = { 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 };
static const uint8_t dc_chr_n[16] = { 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0 };
static const uint8_t dc_vals[12] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
static const uint8_t ac_lum_n[16] = { 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d };
static const uint8_t ac_lum_v[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
    0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
    0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
    0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
    0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa,
};
static const uint8_t ac_chr_n[16] = { 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77 };
static const uint8_t ac_chr_v[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
    0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
    0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
    0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
    0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
    0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
    0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
    0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa,
};

struct huff {
    uint16_t code[256];
    uint8_t len[256];
};

static void huff_build(struct huff *h, const uint8_t *n, const uint8_t *vals)
{
    memset(h, 0, sizeof(*h));
    uint32_t code = 0;
    int k = 0;
    for (int bits = 1; bits <= 16; bits++) {
        for (int i = 0; i < n[bits - 1]; i++) {
            h->code[vals[k]] = (uint16_t)code;
            h->len[vals[k]] = (uint8_t)bits;
            code++;
            k++;
        }
        code <<= 1;
    }
}

/* ---- bit writer ------------------------------------------------------------------------------------------------------ */

struct out {
    uint8_t *p;
    size_t n, cap;
    uint32_t acc;               /* bits waiting, MSB first in the low `nacc` bits */
    int nacc;
    int oom;
};

static void put_byte(struct out *o, uint8_t b)
{
    if (o->n == o->cap) {
        size_t nc = o->cap ? o->cap * 2 : 65536;
        uint8_t *np = realloc(o->p, nc);
        if (!np) {
            o->oom = 1;
            return;
        }
        o->p = np;
        o->cap = nc;
    }
    o->p[o->n++] = b;
}

static void put16(struct out *o, unsigned v)
{
    put_byte(o, (uint8_t)(v >> 8));
    put_byte(o, (uint8_t)v);
}

static void put_bits(struct out *o, unsigned code, int len)
{
    if (len == 0)
        return;
    o->acc = (o->acc << len) | (code & ((1u << len) - 1));
    o->nacc += len;
    while (o->nacc >= 8) {
        uint8_t b = (uint8_t)(o->acc >> (o->nacc - 8));
        put_byte(o, b);
        if (b == 0xff)
            put_byte(o, 0);                     /* byte stuffing */
        o->nacc -= 8;
    }
    o->acc &= (1u << o->nacc) - 1;
}

/* ---- DCT ------------------------------------------------------------------------------------------------------------- */

static float dct_cos[8][8];                      /* cos((2x+1) u pi / 16) * c(u) / 2, built once */
static int dct_ready;

static void dct_init(void)
{
    for (int u = 0; u < 8; u++)
        for (int x = 0; x < 8; x++)
            dct_cos[u][x] = (float)(cos((2 * x + 1) * u * M_PI / 16.0) * (u == 0 ? sqrt(0.125) : 0.5));
    dct_ready = 1;
}

/* One 8 point DCT of in[0], in[stride] ... (even / odd split: the cosines are (anti)symmetric, 32 products instead of 64). */
static inline void dct8(const float *in, int is, float *out, int os)
{
    float s[4], d[4];
    for (int i = 0; i < 4; i++) {
        s[i] = in[i * is] + in[(7 - i) * is];
        d[i] = in[i * is] - in[(7 - i) * is];
    }
    for (int u = 0; u < 8; u += 2)
        out[u * os] = s[0] * dct_cos[u][0] + s[1] * dct_cos[u][1] + s[2] * dct_cos[u][2] + s[3] * dct_cos[u][3];
    for (int u = 1; u < 8; u += 2)
        out[u * os] = d[0] * dct_cos[u][0] + d[1] * dct_cos[u][1] + d[2] * dct_cos[u][2] + d[3] * dct_cos[u][3];
}

/* Separable 8x8 forward DCT: in[64] level shifted samples (row major), out[64] coefficients (row major). A block of one
 * value (most of a desktop) has only the DC term. */
static void fdct(const float *in, float *out)
{
    int flat = 1;
    for (int i = 1; i < 64; i++)
        if (in[i] != in[0]) {
            flat = 0;
            break;
        }
    if (flat) {
        memset(out, 0, 64 * sizeof(float));
        out[0] = in[0] * 8.0f;
        return;
    }
    float tmp[64];
    for (int y = 0; y < 8; y++)
        dct8(in + y * 8, 1, tmp + y * 8, 1);                    /* rows: tmp[y][u] */
    for (int u = 0; u < 8; u++)
        dct8(tmp + u, 8, out + u, 8);                           /* columns: out[v][u] */
}

static int category(int v)                       /* number of bits of |v| */
{
    unsigned a = (unsigned)(v < 0 ? -v : v);
    int n = 0;
    while (a) {
        n++;
        a >>= 1;
    }
    return n;
}

static void put_block(struct out *o, const float *coef, const float *qinv, int *pred, const struct huff *dc, const struct huff *ac)
{
    int q[64];
    for (int i = 0; i < 64; i++) {
        float v = coef[i] * qinv[i];
        q[i] = (int)(v < 0 ? v - 0.5f : v + 0.5f);
    }
    int diff = q[0] - *pred;
    *pred = q[0];
    int cat = category(diff);
    put_bits(o, dc->code[cat], dc->len[cat]);
    if (cat)
        put_bits(o, (unsigned)(diff < 0 ? diff - 1 : diff), cat);
    int run = 0;
    for (int i = 1; i < 64; i++) {
        int v = q[zigzag[i]];
        if (v == 0) {
            run++;
            continue;
        }
        while (run > 15) {
            put_bits(o, ac->code[0xf0], ac->len[0xf0]);
            run -= 16;
        }
        int c = category(v);
        int sym = (run << 4) | c;
        put_bits(o, ac->code[sym], ac->len[sym]);
        put_bits(o, (unsigned)(v < 0 ? v - 1 : v), c);
        run = 0;
    }
    if (run)
        put_bits(o, ac->code[0], ac->len[0]);       /* EOB */
}

static void put_dqt(struct out *o, int id, const uint8_t *q /* natural order, already scaled */)
{
    put16(o, 0xffdb);
    put16(o, 67);
    put_byte(o, (uint8_t)id);
    for (int i = 0; i < 64; i++)
        put_byte(o, q[zigzag[i]]);
}

static void put_dht(struct out *o, int tc_th, const uint8_t *n, const uint8_t *vals, int nvals)
{
    put16(o, 0xffc4);
    put16(o, (unsigned)(2 + 1 + 16 + nvals));
    put_byte(o, (uint8_t)tc_th);
    for (int i = 0; i < 16; i++)
        put_byte(o, n[i]);
    for (int i = 0; i < nvals; i++)
        put_byte(o, vals[i]);
}

uint8_t *lm_jpeg_encode_xrgb(const uint32_t *px, int w, int h, size_t stride, int quality, size_t *len)
{
    if (!px || w < 1 || h < 1 || w > 65535 || h > 65535 || stride < (size_t)w * 4)
        return NULL;
    if (!dct_ready)
        dct_init();                              /* benign race: every thread computes the same values */
    if (quality < 1)
        quality = 1;
    if (quality > 100)
        quality = 100;
    int scale = quality < 50 ? 5000 / quality : 200 - quality * 2;
    uint8_t ql[64], qc[64];
    float qli[64], qci[64];
    for (int i = 0; i < 64; i++) {
        int a = (qlum[i] * scale + 50) / 100, b = (qchr[i] * scale + 50) / 100;
        ql[i] = (uint8_t)(a < 1 ? 1 : a > 255 ? 255 : a);
        qc[i] = (uint8_t)(b < 1 ? 1 : b > 255 ? 255 : b);
        qli[i] = 1.0f / ql[i];
        qci[i] = 1.0f / qc[i];
    }
    static struct huff dcl, dcc, acl, acc;
    static int huff_ready;
    if (!huff_ready) {
        huff_build(&dcl, dc_lum_n, dc_vals);
        huff_build(&dcc, dc_chr_n, dc_vals);
        huff_build(&acl, ac_lum_n, ac_lum_v);
        huff_build(&acc, ac_chr_n, ac_chr_v);
        huff_ready = 1;
    }

    struct out o = { 0 };
    put16(&o, 0xffd8);                                           /* SOI */
    put16(&o, 0xffe0);                                           /* JFIF APP0 */
    put16(&o, 16);
    static const uint8_t jfif[] = { 'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0 };
    for (size_t i = 0; i < sizeof(jfif); i++)
        put_byte(&o, jfif[i]);
    put_dqt(&o, 0, ql);
    put_dqt(&o, 1, qc);
    put16(&o, 0xffc0);                                           /* SOF0 */
    put16(&o, 17);
    put_byte(&o, 8);
    put16(&o, (unsigned)h);
    put16(&o, (unsigned)w);
    put_byte(&o, 3);
    for (int c = 0; c < 3; c++) {
        put_byte(&o, (uint8_t)(c + 1));
        put_byte(&o, 0x11);                                      /* 4:4:4 */
        put_byte(&o, c ? 1 : 0);
    }
    put_dht(&o, 0x00, dc_lum_n, dc_vals, 12);
    put_dht(&o, 0x10, ac_lum_n, ac_lum_v, 162);
    put_dht(&o, 0x01, dc_chr_n, dc_vals, 12);
    put_dht(&o, 0x11, ac_chr_n, ac_chr_v, 162);
    put16(&o, 0xffda);                                           /* SOS */
    put16(&o, 12);
    put_byte(&o, 3);
    for (int c = 0; c < 3; c++) {
        put_byte(&o, (uint8_t)(c + 1));
        put_byte(&o, c ? 0x11 : 0x00);
    }
    put_byte(&o, 0);
    put_byte(&o, 63);
    put_byte(&o, 0);

    int pred[3] = { 0, 0, 0 };
    for (int by = 0; by < h; by += 8) {
        for (int bx = 0; bx < w; bx += 8) {
            float blk[3][64], co[64];
            for (int y = 0; y < 8; y++) {
                int sy = by + y < h ? by + y : h - 1;            /* edge pixels are repeated */
                const uint32_t *row = (const uint32_t *)((const uint8_t *)px + (size_t)sy * stride);
                for (int x = 0; x < 8; x++) {
                    uint32_t p = row[bx + x < w ? bx + x : w - 1];
                    float r = (float)((p >> 16) & 0xff), g = (float)((p >> 8) & 0xff), b = (float)(p & 0xff);
                    blk[0][y * 8 + x] = 0.299f * r + 0.587f * g + 0.114f * b - 128.0f;
                    blk[1][y * 8 + x] = -0.168736f * r - 0.331264f * g + 0.5f * b;
                    blk[2][y * 8 + x] = 0.5f * r - 0.418688f * g - 0.081312f * b;
                }
            }
            fdct(blk[0], co);
            put_block(&o, co, qli, &pred[0], &dcl, &acl);
            fdct(blk[1], co);
            put_block(&o, co, qci, &pred[1], &dcc, &acc);
            fdct(blk[2], co);
            put_block(&o, co, qci, &pred[2], &dcc, &acc);
        }
        if (o.oom)
            break;
    }
    if (o.nacc > 0)
        put_bits(&o, 0x7f, 8 - o.nacc);                          /* pad with 1 bits */
    put16(&o, 0xffd9);                                           /* EOI */
    if (o.oom) {
        free(o.p);
        return NULL;
    }
    *len = o.n;
    return o.p;
}
