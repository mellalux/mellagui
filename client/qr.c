#include "qr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Level M: the error correction codewords of a block, and the blocks as (count, data codewords) of the short and the long group. */
static const struct {
    int total, ec, n1, d1, n2, d2;
} VERS[11] = {
    { 0, 0, 0, 0, 0, 0 },
    { 26, 10, 1, 16, 0, 0 },
    { 44, 16, 1, 28, 0, 0 },
    { 70, 26, 1, 44, 0, 0 },
    { 100, 18, 2, 32, 0, 0 },
    { 134, 24, 2, 43, 0, 0 },
    { 172, 16, 4, 27, 0, 0 },
    { 196, 18, 4, 31, 0, 0 },
    { 242, 22, 2, 38, 2, 39 },
    { 292, 22, 3, 36, 2, 37 },
    { 346, 26, 4, 43, 1, 44 },
};

static const uint8_t ALIGN[11][3] = {
    { 0 }, { 0 }, { 6, 18 }, { 6, 22 }, { 6, 26 }, { 6, 30 }, { 6, 34 }, { 6, 22, 38 }, { 6, 24, 42 }, { 6, 26, 46 }, { 6, 28, 50 },
};

struct grid {
    int size;
    uint8_t *m;                         /* the modules */
    uint8_t func[QR_MAX_SIZE * QR_MAX_SIZE];    /* 1 = a function module (not data, not masked) */
};

static void set_func(struct grid *g, int x, int y, bool dark)
{
    if (x < 0 || y < 0 || x >= g->size || y >= g->size)
        return;
    g->m[y * g->size + x] = dark;
    g->func[y * g->size + x] = 1;
}

/* ---- Reed-Solomon over GF(256), polynomial 0x11d ---------------------------------------------------------------------------------------- */

static uint8_t rs_mul(uint8_t x, uint8_t y)
{
    int z = 0;
    for (int i = 7; i >= 0; i--) {
        z = (z << 1) ^ ((z >> 7) * 0x11d);
        z ^= ((y >> i) & 1) * x;
    }
    return (uint8_t)z;
}

static void rs_generator(int degree, uint8_t *gen)
{
    memset(gen, 0, (size_t)degree);
    gen[degree - 1] = 1;
    uint8_t root = 1;
    for (int i = 0; i < degree; i++) {
        for (int j = 0; j < degree; j++) {
            gen[j] = rs_mul(gen[j], root);
            if (j + 1 < degree)
                gen[j] ^= gen[j + 1];
        }
        root = rs_mul(root, 2);
    }
}

static void rs_remainder(const uint8_t *data, int n, const uint8_t *gen, int degree, uint8_t *res)
{
    memset(res, 0, (size_t)degree);
    for (int i = 0; i < n; i++) {
        uint8_t factor = data[i] ^ res[0];
        memmove(res, res + 1, (size_t)degree - 1);
        res[degree - 1] = 0;
        for (int j = 0; j < degree; j++)
            res[j] ^= rs_mul(gen[j], factor);
    }
}

/* ---- the function patterns ---------------------------------------------------------------------------------------------------------------- */

static void draw_format(struct grid *g, int mask)
{
    int data = (0 << 3) | mask, rem = data;                     /* level M = 00 */
    for (int i = 0; i < 10; i++)
        rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    int bits = ((data << 10) | rem) ^ 0x5412;
#define BIT(i) (((bits >> (i)) & 1) != 0)
    for (int i = 0; i <= 5; i++)
        set_func(g, 8, i, BIT(i));
    set_func(g, 8, 7, BIT(6));
    set_func(g, 8, 8, BIT(7));
    set_func(g, 7, 8, BIT(8));
    for (int i = 9; i < 15; i++)
        set_func(g, 14 - i, 8, BIT(i));
    for (int i = 0; i < 8; i++)
        set_func(g, g->size - 1 - i, 8, BIT(i));
    for (int i = 8; i < 15; i++)
        set_func(g, 8, g->size - 15 + i, BIT(i));
#undef BIT
    set_func(g, 8, g->size - 8, true);
}

static void draw_function_patterns(struct grid *g, int ver)
{
    int size = g->size;
    for (int i = 0; i < size; i++) {                            /* timing */
        set_func(g, 6, i, i % 2 == 0);
        set_func(g, i, 6, i % 2 == 0);
    }
    static const int corner[3][2] = { { 3, 3 }, { -4, 3 }, { 3, -4 } };
    for (int k = 0; k < 3; k++) {                               /* finders with their separators */
        int cx = corner[k][0] < 0 ? size + corner[k][0] : corner[k][0], cy = corner[k][1] < 0 ? size + corner[k][1] : corner[k][1];
        for (int dy = -4; dy <= 4; dy++)
            for (int dx = -4; dx <= 4; dx++) {
                int dist = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
                set_func(g, cx + dx, cy + dy, dist != 2 && dist != 4);
            }
    }
    int na = ver == 1 ? 0 : ver < 7 ? 2 : 3;
    for (int i = 0; i < na; i++)
        for (int j = 0; j < na; j++) {
            if ((i == 0 && j == 0) || (i == 0 && j == na - 1) || (i == na - 1 && j == 0))
                continue;                                       /* the finders' corners */
            for (int dy = -2; dy <= 2; dy++)
                for (int dx = -2; dx <= 2; dx++)
                    set_func(g, ALIGN[ver][i] + dx, ALIGN[ver][j] + dy, (abs(dx) > abs(dy) ? abs(dx) : abs(dy)) != 1);
        }
    draw_format(g, 0);                                          /* reserves the places; the real one comes with the mask */
    if (ver >= 7) {
        int rem = ver;
        for (int i = 0; i < 12; i++)
            rem = (rem << 1) ^ ((rem >> 11) * 0x1f25);
        int bits = (ver << 12) | rem;
        for (int i = 0; i < 18; i++) {
            bool bit = (bits >> i) & 1;
            int a = size - 11 + i % 3, b = i / 3;
            set_func(g, a, b, bit);
            set_func(g, b, a, bit);
        }
    }
}

static bool mask_bit(int mask, int x, int y)
{
    switch (mask) {
    case 0: return (x + y) % 2 == 0;
    case 1: return y % 2 == 0;
    case 2: return x % 3 == 0;
    case 3: return (x + y) % 3 == 0;
    case 4: return (x / 3 + y / 2) % 2 == 0;
    case 5: return x * y % 2 + x * y % 3 == 0;
    case 6: return (x * y % 2 + x * y % 3) % 2 == 0;
    default: return ((x + y) % 2 + x * y % 3) % 2 == 0;
    }
}

static void apply_mask(struct grid *g, int mask)
{
    for (int y = 0; y < g->size; y++)
        for (int x = 0; x < g->size; x++)
            if (!g->func[y * g->size + x] && mask_bit(mask, x, y))
                g->m[y * g->size + x] ^= 1;
}

/* The penalty of the standard's four rules (runs, 2x2 blocks, finder-like patterns, the balance of dark and light). */
static long penalty(const struct grid *g)
{
    int size = g->size;
    long score = 0;
    for (int pass = 0; pass < 2; pass++)                        /* rows, then columns */
        for (int a = 0; a < size; a++) {
            int run = 0, colour = -1;
            unsigned hist = 0;
            for (int b = 0; b < size; b++) {
                int v = pass ? g->m[b * size + a] : g->m[a * size + b];
                if (v == colour) {
                    run++;
                    if (run == 5)
                        score += 3;
                    else if (run > 5)
                        score++;
                } else {
                    colour = v;
                    run = 1;
                }
                hist = ((hist << 1) | (unsigned)v) & 0x7ff;     /* the last 11 modules */
                if (b >= 10 && (hist == 0x5d0 || hist == 0x05d))    /* 10111010000 / 00001011101 */
                    score += 40;
            }
        }
    int dark = 0;
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            int v = g->m[y * size + x];
            dark += v;
            if (x + 1 < size && y + 1 < size && v == g->m[y * size + x + 1] && v == g->m[(y + 1) * size + x] && v == g->m[(y + 1) * size + x + 1])
                score += 3;
        }
    int total = size * size;
    int k = (abs(dark * 20 - total * 10) + total - 1) / total - 1;
    return score + (k > 0 ? k * 10 : 0);
}

bool qr_encode(const char *text, struct qr *out, int mask)
{
    size_t len = strlen(text);
    int ver = 0;
    for (int v = 1; v <= 10 && !ver; v++) {
        int data = VERS[v].n1 * VERS[v].d1 + VERS[v].n2 * VERS[v].d2;
        if (len * 8 + 4 + (v < 10 ? 8 : 16) <= (size_t)data * 8)
            ver = v;
    }
    if (!ver)
        return false;
    int ndata = VERS[ver].n1 * VERS[ver].d1 + VERS[ver].n2 * VERS[ver].d2, ec = VERS[ver].ec, nblocks = VERS[ver].n1 + VERS[ver].n2;

    /* the data codewords: mode, length, the bytes, a terminator, padding */
    uint8_t data[220] = { 0 };
    int bits = 0;
#define PUT(value, count) do { for (int b_ = (count) - 1; b_ >= 0; b_--, bits++) if (((value) >> b_) & 1) data[bits / 8] |= (uint8_t)(0x80 >> (bits % 8)); } while (0)
    PUT(4, 4);
    PUT((int)len, ver < 10 ? 8 : 16);
    for (size_t i = 0; i < len; i++)
        PUT((unsigned char)text[i], 8);
    int term = ndata * 8 - bits < 4 ? ndata * 8 - bits : 4;
    PUT(0, term);
    bits = (bits + 7) / 8 * 8;
    for (int pad = 0xec; bits < ndata * 8; pad ^= 0xec ^ 0x11)
        PUT(pad, 8);
#undef PUT

    /* the blocks with their error correction, interleaved */
    uint8_t gen[30], ecc[5][30], all[346];
    const uint8_t *blk[5];
    int blen[5], n = 0, off = 0;
    rs_generator(ec, gen);
    for (int b = 0; b < nblocks; b++) {
        blen[b] = b < VERS[ver].n1 ? VERS[ver].d1 : VERS[ver].d2;
        blk[b] = data + off;
        rs_remainder(blk[b], blen[b], gen, ec, ecc[b]);
        off += blen[b];
    }
    int longest = VERS[ver].n2 ? VERS[ver].d2 : VERS[ver].d1;
    for (int i = 0; i < longest; i++)
        for (int b = 0; b < nblocks; b++)
            if (i < blen[b])
                all[n++] = blk[b][i];
    for (int i = 0; i < ec; i++)
        for (int b = 0; b < nblocks; b++)
            all[n++] = ecc[b][i];

    struct grid g;
    memset(&g, 0, sizeof(g));
    memset(out, 0, sizeof(*out));
    g.size = out->size = 17 + 4 * ver;
    g.m = out->m;
    draw_function_patterns(&g, ver);

    /* the codewords in the zigzag from the bottom right corner */
    int i = 0, size = g.size;
    for (int right = size - 1; right >= 1; right -= 2) {
        if (right == 6)
            right = 5;
        for (int vert = 0; vert < size; vert++)
            for (int j = 0; j < 2; j++) {
                int x = right - j;
                bool upward = ((right + 1) & 2) == 0;
                int y = upward ? size - 1 - vert : vert;
                if (!g.func[y * size + x] && i < n * 8) {
                    g.m[y * size + x] = (all[i / 8] >> (7 - i % 8)) & 1;
                    i++;
                }
            }
    }

    if (mask < 0 || mask > 7) {
        long best = -1;
        for (int k = 0; k < 8; k++) {
            apply_mask(&g, k);
            draw_format(&g, k);
            long p = penalty(&g);
            if (best < 0 || p < best) {
                best = p;
                mask = k;
            }
            apply_mask(&g, k);                                  /* (the mask is its own inverse) */
        }
    }
    apply_mask(&g, mask);
    draw_format(&g, mask);
    return true;
}

int qr_selftest(void)
{
    int bad = 0;
    /* the error correction of the standard's 1-M example ("01234567") */
    static const uint8_t data[16] = { 0x10, 0x20, 0x0c, 0x56, 0x61, 0x80, 0xec, 0x11, 0xec, 0x11, 0xec, 0x11, 0xec, 0x11, 0xec, 0x11 };
    static const uint8_t want[10] = { 0xa5, 0x24, 0xd4, 0xc1, 0xed, 0x36, 0xc7, 0x87, 0x2c, 0x55 };
    uint8_t gen[30], ec[30];
    rs_generator(10, gen);
    rs_remainder(data, 16, gen, 10, ec);
    if (memcmp(ec, want, 10)) {
        fprintf(stderr, "qr selftest: the error correction codewords differ\n");
        bad++;
    }
    struct qr q;
    static const struct { int len, size; } sizes[] = { { 14, 21 }, { 15, 25 }, { 62, 33 }, { 213, 57 } };
    char text[300];
    for (int i = 0; i < 4; i++) {
        memset(text, 'a', (size_t)sizes[i].len);
        text[sizes[i].len] = 0;
        if (!qr_encode(text, &q, -1) || q.size != sizes[i].size) {
            fprintf(stderr, "qr selftest: %d bytes give size %d, not %d\n", sizes[i].len, q.size, sizes[i].size);
            bad++;
        }
    }
    memset(text, 'a', 214);
    text[214] = 0;
    if (qr_encode(text, &q, -1)) {
        fprintf(stderr, "qr selftest: 214 bytes fit\n");
        bad++;
    }
    qr_encode("mellagui", &q, 2);
    /* the finder's centre and ring, the timing row, the dark module, the format bits of level M mask 2 (101111001111100) */
    static const char fmt[] = "101111001111100";
    bool ok = qr_dark(&q, 3, 3) && !qr_dark(&q, 1, 1) && qr_dark(&q, 0, 0) && !qr_dark(&q, 7, 7) && qr_dark(&q, 8, 6) && !qr_dark(&q, 9, 6) && qr_dark(&q, 8, q.size - 8);
    for (int i = 0; i < 8; i++)                                 /* (the text has the highest bit first; module i carries bit i) */
        ok = ok && qr_dark(&q, q.size - 1 - i, 8) == (fmt[14 - i] == '1');
    if (!ok) {
        fprintf(stderr, "qr selftest: the function patterns are wrong\n");
        bad++;
    }
    return bad;
}
