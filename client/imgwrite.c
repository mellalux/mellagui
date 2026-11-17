/* Picture file writers (see imgwrite.h). */
#include "imgwrite.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "deflate.h"

/* ---- a growing buffer ---------------------------------------------------------------------------------------------------------- */

struct buf {
    uint8_t *p;
    size_t n, cap;
    bool bad;
};

static void bput(struct buf *b, const void *d, size_t n)
{
    if (b->bad)
        return;
    if (b->n + n > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 4096;
        while (nc < b->n + n)
            nc *= 2;
        uint8_t *np = realloc(b->p, nc);
        if (!np) {
            b->bad = true;
            return;
        }
        b->p = np;
        b->cap = nc;
    }
    if (n)
        memcpy(b->p + b->n, d, n);
    b->n += n;
}

static void b8(struct buf *b, unsigned v)
{
    uint8_t c = (uint8_t)v;
    bput(b, &c, 1);
}

static void b16(struct buf *b, unsigned v)
{
    b8(b, v & 255);
    b8(b, (v >> 8) & 255);
}

static void b32(struct buf *b, uint32_t v)
{
    b16(b, v & 0xffff);
    b16(b, v >> 16);
}

static uint8_t *finish(struct buf *b, size_t *n)
{
    if (b->bad) {
        free(b->p);
        return NULL;
    }
    *n = b->n;
    return b->p;
}

static bool has_alpha(const uint32_t *px, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if ((px[i] >> 24) != 255)
            return true;
    return false;
}

/* The colour channel laid on white by its alpha. */
static uint8_t on_white(uint32_t c, int shift)
{
    int a = (int)(c >> 24), v = (int)((c >> shift) & 255);
    return (uint8_t)((v * a + 255 * (255 - a)) / 255);
}

/* ---- BMP ----------------------------------------------------------------------------------------------------------------------- */

uint8_t *lm_enc_bmp(const uint32_t *px, int w, int h, size_t *out_n)
{
    bool alpha = has_alpha(px, (size_t)w * (size_t)h);
    size_t stride = alpha ? (size_t)w * 4 : (((size_t)w * 3 + 3) & ~(size_t)3);
    size_t hdr = alpha ? 14 + 108 : 14 + 40, total = hdr + stride * (size_t)h;
    struct buf b = { 0 };
    bput(&b, "BM", 2);
    b32(&b, (uint32_t)total);
    b32(&b, 0);
    b32(&b, (uint32_t)hdr);
    b32(&b, alpha ? 108 : 40);
    b32(&b, (uint32_t)w);
    b32(&b, (uint32_t)h);                                               /* positive: the rows are stored from the bottom */
    b16(&b, 1);
    b16(&b, alpha ? 32 : 24);
    b32(&b, alpha ? 3 : 0);                                             /* BI_BITFIELDS with an alpha mask, or plain RGB */
    b32(&b, (uint32_t)(stride * (size_t)h));
    b32(&b, 2835);
    b32(&b, 2835);
    b32(&b, 0);
    b32(&b, 0);
    if (alpha) {
        b32(&b, 0x00ff0000u);
        b32(&b, 0x0000ff00u);
        b32(&b, 0x000000ffu);
        b32(&b, 0xff000000u);
        b32(&b, 0x73524742u);                                           /* "sRGB" */
        for (int i = 0; i < 12; i++)                                    /* endpoints and gamma: unused */
            b32(&b, 0);
    }
    for (int y = h - 1; y >= 0; y--) {
        const uint32_t *row = px + (size_t)y * (size_t)w;
        if (alpha) {
            for (int x = 0; x < w; x++)
                b32(&b, row[x]);
        } else {
            for (int x = 0; x < w; x++) {
                b8(&b, row[x] & 255);
                b8(&b, (row[x] >> 8) & 255);
                b8(&b, (row[x] >> 16) & 255);
            }
            for (size_t k = (size_t)w * 3; k < stride; k++)
                b8(&b, 0);
        }
    }
    return finish(&b, out_n);
}

/* ---- TGA ----------------------------------------------------------------------------------------------------------------------- */

uint8_t *lm_enc_tga(const uint32_t *px, int w, int h, size_t *out_n)
{
    if (w > 65535 || h > 65535)
        return NULL;
    bool alpha = has_alpha(px, (size_t)w * (size_t)h);
    struct buf b = { 0 };
    b8(&b, 0);
    b8(&b, 0);
    b8(&b, 2);                                                          /* uncompressed true colour */
    for (int i = 0; i < 5; i++)
        b8(&b, 0);
    b16(&b, 0);
    b16(&b, 0);
    b16(&b, (unsigned)w);
    b16(&b, (unsigned)h);
    b8(&b, alpha ? 32 : 24);
    b8(&b, (alpha ? 8 : 0) | 0x20);                                     /* alpha bits, origin at the top left */
    for (size_t i = 0; i < (size_t)w * (size_t)h; i++) {
        b8(&b, px[i] & 255);
        b8(&b, (px[i] >> 8) & 255);
        b8(&b, (px[i] >> 16) & 255);
        if (alpha)
            b8(&b, px[i] >> 24);
    }
    b32(&b, 0);
    b32(&b, 0);
    bput(&b, "TRUEVISION-XFILE.", 18);                                  /* (with its zero byte) the footer that makes it a version 2 file */
    return finish(&b, out_n);
}

/* ---- PPM ----------------------------------------------------------------------------------------------------------------------- */

uint8_t *lm_enc_ppm(const uint32_t *px, int w, int h, size_t *out_n)
{
    struct buf b = { 0 };
    char head[48];
    int hl = snprintf(head, sizeof(head), "P6\n%d %d\n255\n", w, h);
    bput(&b, head, (size_t)hl);
    for (size_t i = 0; i < (size_t)w * (size_t)h; i++) {
        b8(&b, on_white(px[i], 16));
        b8(&b, on_white(px[i], 8));
        b8(&b, on_white(px[i], 0));
    }
    return finish(&b, out_n);
}

/* ---- TIFF ---------------------------------------------------------------------------------------------------------------------- */

static uint32_t adler32(const uint8_t *p, size_t n)
{
    uint32_t a = 1, b = 0;
    while (n) {
        size_t chunk = n < 5552 ? n : 5552;
        for (size_t i = 0; i < chunk; i++) {
            a += p[i];
            b += a;
        }
        a %= 65521;
        b %= 65521;
        p += chunk;
        n -= chunk;
    }
    return (b << 16) | a;
}

uint8_t *lm_enc_tiff(const uint32_t *px, int w, int h, size_t *out_n)
{
    bool alpha = has_alpha(px, (size_t)w * (size_t)h);
    int spp = alpha ? 4 : 3;
    size_t rawn = (size_t)w * (size_t)h * (size_t)spp, k = 0;
    uint8_t *raw = malloc(rawn ? rawn : 1);
    if (!raw)
        return NULL;
    for (size_t i = 0; i < (size_t)w * (size_t)h; i++) {
        raw[k++] = (uint8_t)(px[i] >> 16);
        raw[k++] = (uint8_t)(px[i] >> 8);
        raw[k++] = (uint8_t)px[i];
        if (alpha)
            raw[k++] = (uint8_t)(px[i] >> 24);
    }
    size_t zn = 0;
    uint8_t *z = lm_deflate_buf(raw, rawn, 6, &zn);
    uint32_t ad = adler32(raw, rawn);
    free(raw);
    if (!z)
        return NULL;
    int ne = alpha ? 11 : 10;
    uint32_t ifd = 8, bits_off = ifd + 2 + (uint32_t)ne * 12 + 4, data_off = bits_off + (uint32_t)spp * 2, total = (uint32_t)zn + 6;
    struct buf b = { 0 };
    bput(&b, "II", 2);
    b16(&b, 42);
    b32(&b, ifd);
    b16(&b, (unsigned)ne);
#define ENT(tag, type, cnt, val) do { b16(&b, tag); b16(&b, type); b32(&b, cnt); \
        if ((type) == 3 && (cnt) == 1) { b16(&b, (unsigned)(val)); b16(&b, 0); } else b32(&b, (uint32_t)(val)); } while (0)
    ENT(256, 4, 1, w);
    ENT(257, 4, 1, h);
    ENT(258, 3, spp, bits_off);                                         /* BitsPerSample: 8 8 8 (8) */
    ENT(259, 3, 1, 8);                                                  /* Compression: Deflate */
    ENT(262, 3, 1, 2);                                                  /* RGB */
    ENT(273, 4, 1, data_off);
    ENT(277, 3, 1, spp);
    ENT(278, 4, 1, h);
    ENT(279, 4, 1, total);
    ENT(284, 3, 1, 1);                                                  /* chunky */
    if (alpha)
        ENT(338, 3, 1, 2);                                              /* ExtraSamples: unassociated alpha */
#undef ENT
    b32(&b, 0);
    for (int i = 0; i < spp; i++)
        b16(&b, 8);
    b8(&b, 0x78);
    b8(&b, 0x9c);
    bput(&b, z, zn);
    b8(&b, ad >> 24);
    b8(&b, ad >> 16);
    b8(&b, ad >> 8);
    b8(&b, ad);
    free(z);
    return finish(&b, out_n);
}

/* ---- GIF ----------------------------------------------------------------------------------------------------------------------- */

struct lzw {
    struct buf *b;
    uint32_t acc;
    int nb;
    uint8_t blk[255];
    int nblk;
};

static void lzw_block(struct lzw *l)
{
    if (l->nblk) {
        b8(l->b, (unsigned)l->nblk);
        bput(l->b, l->blk, (size_t)l->nblk);
        l->nblk = 0;
    }
}

static void lzw_emit(struct lzw *l, unsigned code, int size)
{
    l->acc |= (uint32_t)code << l->nb;
    l->nb += size;
    while (l->nb >= 8) {
        l->blk[l->nblk++] = (uint8_t)(l->acc & 255);
        l->acc >>= 8;
        l->nb -= 8;
        if (l->nblk == 255)
            lzw_block(l);
    }
}

#define LZW_HS 16384

/* The indexes as GIF's variable width LZW (codes of mcs + 1 .. 12 bits, a clear code when the table is full). */
static bool gif_lzw(struct buf *b, const uint8_t *idx, size_t n, int mcs)
{
    uint32_t *keys = calloc(LZW_HS, sizeof(uint32_t));
    uint16_t *vals = malloc(LZW_HS * sizeof(uint16_t));
    if (!keys || !vals) {
        free(keys);
        free(vals);
        return false;
    }
    b8(b, (unsigned)mcs);
    struct lzw l = { .b = b };
    int clear = 1 << mcs, eoi = clear + 1, next = eoi + 1, cs = mcs + 1;
    lzw_emit(&l, (unsigned)clear, cs);
    unsigned prefix = idx[0];
    for (size_t i = 1; i < n; i++) {
        unsigned c = idx[i];
        uint32_t key = (((uint32_t)prefix << 8) | c) + 1;
        unsigned h = (key * 2654435761u) >> 18;
        while (keys[h] && keys[h] != key)
            h = (h + 1) & (LZW_HS - 1);
        if (keys[h]) {
            prefix = vals[h];
            continue;
        }
        lzw_emit(&l, prefix, cs);
        int e = next++;
        keys[h] = key;
        vals[h] = (uint16_t)e;
        if (e == (1 << cs) && cs < 12)                                  /* the decoder's table grows one step behind ours */
            cs++;
        if (next == 4096) {
            lzw_emit(&l, (unsigned)clear, cs);
            memset(keys, 0, LZW_HS * sizeof(uint32_t));
            next = eoi + 1;
            cs = mcs + 1;
        }
        prefix = c;
    }
    lzw_emit(&l, prefix, cs);
    if (next == (1 << cs) && cs < 12)
        cs++;
    lzw_emit(&l, (unsigned)eoi, cs);
    if (l.nb > 0)
        l.blk[l.nblk++] = (uint8_t)(l.acc & 255);
    lzw_block(&l);
    b8(b, 0);
    free(keys);
    free(vals);
    return true;
}

struct bin {
    int rgb5;                           /* r << 10 | g << 5 | b */
    uint32_t count;
};

static int g_chan;                      /* the channel qsort looks at */

static int bin_cmp(const void *a, const void *b)
{
    const struct bin *x = a, *y = b;
    int sh = g_chan == 0 ? 10 : g_chan == 1 ? 5 : 0;
    int cx = (x->rgb5 >> sh) & 31, cy = (y->rgb5 >> sh) & 31;
    return cx != cy ? cx - cy : x->rgb5 - y->rgb5;
}

struct box {
    int s, e;                           /* bins [s, e) */
    uint64_t pop;
};

uint8_t *lm_enc_gif(const uint32_t *px, int w, int h, size_t *out_n)
{
    if (w > 65535 || h > 65535)
        return NULL;
    size_t np = (size_t)w * (size_t)h;
    bool transparent = false;
    for (size_t i = 0; i < np; i++)
        if ((px[i] >> 24) < 128) {
            transparent = true;
            break;
        }
    int maxcol = transparent ? 255 : 256;
    uint8_t *idx = malloc(np ? np : 1);
    if (!idx)
        return NULL;
    uint32_t pal[256];
    int npal = 0;
    /* the colours themselves, when there are few enough */
    uint32_t tab[1024] = { 0 };                                         /* 0 = empty, else 0x01000000 | rgb */
    int tabidx[1024];
    bool exact = true;
    for (size_t i = 0; i < np && exact; i++) {
        if ((px[i] >> 24) < 128)
            continue;
        uint32_t c = 0x01000000u | (px[i] & 0xffffffu);
        unsigned hh = (c * 2654435761u) >> 22;
        while (tab[hh] && tab[hh] != c)
            hh = (hh + 1) & 1023;
        if (!tab[hh]) {
            if (npal >= maxcol) {
                exact = false;
                break;
            }
            tab[hh] = c;
            tabidx[hh] = npal;
            pal[npal++] = c & 0xffffffu;
        }
    }
    int tidx;
    if (exact) {
        tidx = npal;
        for (size_t i = 0; i < np; i++) {
            if ((px[i] >> 24) < 128) {
                idx[i] = (uint8_t)tidx;
                continue;
            }
            uint32_t c = 0x01000000u | (px[i] & 0xffffffu);
            unsigned hh = (c * 2654435761u) >> 22;
            while (tab[hh] != c)
                hh = (hh + 1) & 1023;
            idx[i] = (uint8_t)tabidx[hh];
        }
    } else {                                                            /* median cut over a histogram of 5 bit colours */
        uint32_t *hist = calloc(32768, sizeof(uint32_t));
        uint8_t *lut = malloc(32768);
        struct bin *bins = malloc(32768 * sizeof(struct bin));
        if (!hist || !lut || !bins) {
            free(hist);
            free(lut);
            free(bins);
            free(idx);
            return NULL;
        }
        for (size_t i = 0; i < np; i++)
            if ((px[i] >> 24) >= 128)
                hist[(((px[i] >> 19) & 31) << 10) | (((px[i] >> 11) & 31) << 5) | ((px[i] >> 3) & 31)]++;
        int nb = 0;
        for (int v = 0; v < 32768; v++)
            if (hist[v]) {
                bins[nb].rgb5 = v;
                bins[nb].count = hist[v];
                nb++;
            }
        struct box boxes[256];
        int nbox = 1;
        boxes[0].s = 0;
        boxes[0].e = nb;
        boxes[0].pop = 0;
        for (int i = 0; i < nb; i++)
            boxes[0].pop += bins[i].count;
        while (nbox < maxcol) {
            int best = -1;
            uint64_t bp = 0;
            for (int i = 0; i < nbox; i++)
                if (boxes[i].e - boxes[i].s > 1 && boxes[i].pop > bp) {
                    bp = boxes[i].pop;
                    best = i;
                }
            if (best < 0)
                break;
            struct box *bx = &boxes[best];
            int lo[3] = { 31, 31, 31 }, hi[3] = { 0, 0, 0 };
            for (int i = bx->s; i < bx->e; i++)
                for (int c = 0; c < 3; c++) {
                    int v = (bins[i].rgb5 >> (c == 0 ? 10 : c == 1 ? 5 : 0)) & 31;
                    lo[c] = v < lo[c] ? v : lo[c];
                    hi[c] = v > hi[c] ? v : hi[c];
                }
            int ch = 0;
            for (int c = 1; c < 3; c++)
                if (hi[c] - lo[c] > hi[ch] - lo[ch])
                    ch = c;
            g_chan = ch;
            qsort(&bins[bx->s], (size_t)(bx->e - bx->s), sizeof(struct bin), bin_cmp);
            uint64_t acc = 0;
            int cut = bx->s;
            for (int i = bx->s; i < bx->e - 1; i++) {
                acc += bins[i].count;
                cut = i + 1;
                if (acc * 2 >= bx->pop)
                    break;
            }
            struct box nbx = { cut, bx->e, 0 };
            for (int i = cut; i < nbx.e; i++)
                nbx.pop += bins[i].count;
            bx->e = cut;
            bx->pop -= nbx.pop;
            boxes[nbox++] = nbx;
        }
        for (int bi = 0; bi < nbox; bi++) {
            uint64_t sr = 0, sg = 0, sb = 0, pop = 0;
            for (int i = boxes[bi].s; i < boxes[bi].e; i++) {
                int r5 = bins[i].rgb5 >> 10, g5 = (bins[i].rgb5 >> 5) & 31, b5 = bins[i].rgb5 & 31;
                sr += (uint64_t)((r5 << 3) | (r5 >> 2)) * bins[i].count;
                sg += (uint64_t)((g5 << 3) | (g5 >> 2)) * bins[i].count;
                sb += (uint64_t)((b5 << 3) | (b5 >> 2)) * bins[i].count;
                pop += bins[i].count;
                lut[bins[i].rgb5] = (uint8_t)bi;
            }
            pal[bi] = pop ? (uint32_t)(((sr / pop) << 16) | ((sg / pop) << 8) | (sb / pop)) : 0;
        }
        npal = nbox;
        tidx = npal;
        for (size_t i = 0; i < np; i++)
            idx[i] = (px[i] >> 24) < 128 ? (uint8_t)tidx
                                         : lut[(((px[i] >> 19) & 31) << 10) | (((px[i] >> 11) & 31) << 5) | ((px[i] >> 3) & 31)];
        free(hist);
        free(lut);
        free(bins);
    }
    int ncol = npal + (transparent ? 1 : 0), k = 1;
    while ((1 << k) < ncol)
        k++;
    struct buf b = { 0 };
    bput(&b, "GIF89a", 6);
    b16(&b, (unsigned)w);
    b16(&b, (unsigned)h);
    b8(&b, 0x80 | (7 << 4) | (k - 1));
    b8(&b, 0);
    b8(&b, 0);
    for (int i = 0; i < (1 << k); i++) {
        uint32_t c = i < npal ? pal[i] : 0;
        b8(&b, c >> 16);
        b8(&b, c >> 8);
        b8(&b, c);
    }
    if (transparent) {                                                  /* graphic control: the transparent index */
        b8(&b, 0x21);
        b8(&b, 0xf9);
        b8(&b, 4);
        b8(&b, 0x01);
        b16(&b, 0);
        b8(&b, (unsigned)tidx);
        b8(&b, 0);
    }
    b8(&b, 0x2c);
    b16(&b, 0);
    b16(&b, 0);
    b16(&b, (unsigned)w);
    b16(&b, (unsigned)h);
    b8(&b, 0);
    bool ok = np > 0 && gif_lzw(&b, idx, np, k < 2 ? 2 : k);
    b8(&b, 0x3b);
    free(idx);
    if (!ok) {
        free(b.p);
        return NULL;
    }
    return finish(&b, out_n);
}

/* ---- WebP, lossless (VP8L) ----------------------------------------------------------------------------------------------------- */

struct bw {
    uint8_t *p;
    size_t n, cap;
    uint64_t acc;
    int nb;
    bool bad;
};

static void bw_byte(struct bw *w, uint8_t v)
{
    if (w->bad)
        return;
    if (w->n == w->cap) {
        size_t nc = w->cap ? w->cap * 2 : 4096;
        uint8_t *np = realloc(w->p, nc);
        if (!np) {
            w->bad = true;
            return;
        }
        w->p = np;
        w->cap = nc;
    }
    w->p[w->n++] = v;
}

/* `v`'s low `nbits` bits, least significant first. */
static void bw_put(struct bw *w, uint32_t v, int nbits)
{
    w->acc |= (uint64_t)v << w->nb;
    w->nb += nbits;
    while (w->nb >= 8) {
        bw_byte(w, (uint8_t)(w->acc & 255));
        w->acc >>= 8;
        w->nb -= 8;
    }
}

struct hnode {
    uint64_t f;
    int l, r;                           /* children, or l = -1 and r = the symbol */
};

/* Code lengths (at most `maxlen`) of a Huffman code for the frequencies: the frequencies are halved until the tree is shallow enough. */
static void huff_len(const uint32_t *freq, int n, int maxlen, uint8_t *len)
{
    memset(len, 0, (size_t)n);
    uint64_t f[300];
    int used = 0, last = -1;
    for (int i = 0; i < n; i++) {
        f[i] = freq[i];
        if (freq[i]) {
            used++;
            last = i;
        }
    }
    if (used == 0)
        return;
    if (used == 1) {
        len[last] = 1;
        return;
    }
    for (;;) {
        struct hnode nd[600];
        int act[600], nn = 0, na = 0;
        for (int i = 0; i < n; i++)
            if (f[i]) {
                nd[nn] = (struct hnode){ f[i], -1, i };
                act[na++] = nn++;
            }
        while (na > 1) {
            int a = 0, b = 1;
            if (nd[act[b]].f < nd[act[a]].f) {
                a = 1;
                b = 0;
            }
            for (int i = 2; i < na; i++) {
                if (nd[act[i]].f < nd[act[a]].f) {
                    b = a;
                    a = i;
                } else if (nd[act[i]].f < nd[act[b]].f)
                    b = i;
            }
            nd[nn] = (struct hnode){ nd[act[a]].f + nd[act[b]].f, act[a], act[b] };
            int hi = a > b ? a : b, lo = a > b ? b : a;
            act[lo] = nn++;
            act[hi] = act[--na];
        }
        int stack[600], depth[600], sp = 0, maxd = 0;
        stack[sp] = act[0];
        depth[sp++] = 0;
        while (sp) {
            sp--;
            int node = stack[sp], d = depth[sp];
            if (nd[node].l < 0) {
                len[nd[node].r] = (uint8_t)d;
                maxd = d > maxd ? d : maxd;
            } else {
                stack[sp] = nd[node].l;
                depth[sp++] = d + 1;
                stack[sp] = nd[node].r;
                depth[sp++] = d + 1;
            }
        }
        if (maxd <= maxlen)
            return;
        for (int i = 0; i < n; i++)
            if (f[i])
                f[i] = (f[i] >> 1) | 1;
    }
}

/* The canonical codes of the lengths, each reversed (the stream carries a code's first bit first, and bw_put sends the low bit first). */
static void huff_rcodes(const uint8_t *len, int n, uint16_t *rcode)
{
    int count[16] = { 0 }, next[16];
    for (int i = 0; i < n; i++)
        count[len[i]]++;
    count[0] = 0;
    int code = 0;
    for (int l = 1; l < 16; l++) {
        code = (code + count[l - 1]) << 1;
        next[l] = code;
    }
    for (int i = 0; i < n; i++) {
        if (!len[i]) {
            rcode[i] = 0;
            continue;
        }
        int c = next[len[i]]++, r = 0;
        for (int b = 0; b < len[i]; b++)
            r |= ((c >> b) & 1) << (len[i] - 1 - b);
        rcode[i] = (uint16_t)r;
    }
}

struct pcode {
    uint8_t len[300];
    uint16_t rcode[300];
    bool single;                        /* one symbol only: it takes no bits at all */
};

static const uint8_t k_clorder[19] = { 17, 18, 0, 1, 2, 3, 4, 5, 16, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };

/* Writes the prefix code of an alphabet of `nsym` symbols with the given frequencies, and keeps what is needed to write the symbols. */
static void webp_code(struct bw *w, const uint32_t *freq, int nsym, struct pcode *pc)
{
    int used = 0, first = 0;
    for (int i = nsym - 1; i >= 0; i--)
        if (freq[i]) {
            used++;
            first = i;
        }
    memset(pc, 0, sizeof(*pc));
    if (used <= 1) {                                                    /* a simple code of one symbol */
        unsigned sym = used ? (unsigned)first : 0;
        bw_put(w, 1, 1);
        bw_put(w, 0, 1);
        if (sym <= 1) {
            bw_put(w, 0, 1);
            bw_put(w, sym, 1);
        } else {
            bw_put(w, 1, 1);
            bw_put(w, sym, 8);
        }
        pc->single = true;
        return;
    }
    huff_len(freq, nsym, 15, pc->len);
    huff_rcodes(pc->len, nsym, pc->rcode);
    int tok[600], ext[600], nt = 0;                                     /* the lengths, with runs of zeros */
    for (int i = 0; i < nsym;) {
        if (pc->len[i] == 0) {
            int r = 1;
            while (i + r < nsym && pc->len[i + r] == 0)
                r++;
            int rem = r;
            while (rem >= 3) {
                if (rem >= 11) {
                    int k = rem < 138 ? rem : 138;
                    tok[nt] = 18;
                    ext[nt++] = k - 11;
                    rem -= k;
                } else {
                    tok[nt] = 17;
                    ext[nt++] = rem - 3;
                    rem = 0;
                }
            }
            while (rem-- > 0) {
                tok[nt] = 0;
                ext[nt++] = 0;
            }
            i += r;
        } else {
            tok[nt] = pc->len[i];
            ext[nt++] = 0;
            i++;
        }
    }
    uint32_t hist[19] = { 0 };
    for (int i = 0; i < nt; i++)
        hist[tok[i]]++;
    uint8_t cl[19];
    uint16_t crc[19];
    huff_len(hist, 19, 7, cl);
    int nz = 0, only = 0;
    for (int i = 0; i < 19; i++)
        if (cl[i]) {
            nz++;
            only = i;
        }
    if (nz < 2)                                                         /* a code needs two symbols to be a code */
        cl[only == 0 ? 1 : 0] = 1;
    huff_rcodes(cl, 19, crc);
    int ncl = 19;
    while (ncl > 4 && cl[k_clorder[ncl - 1]] == 0)
        ncl--;
    bw_put(w, 0, 1);                                                    /* not simple */
    bw_put(w, (uint32_t)(ncl - 4), 4);
    for (int i = 0; i < ncl; i++)
        bw_put(w, cl[k_clorder[i]], 3);
    bw_put(w, 0, 1);                                                    /* every symbol has its length written */
    for (int i = 0; i < nt; i++) {
        bw_put(w, crc[tok[i]], cl[tok[i]]);
        if (tok[i] == 16)
            bw_put(w, (uint32_t)ext[i], 2);
        else if (tok[i] == 17)
            bw_put(w, (uint32_t)ext[i], 3);
        else if (tok[i] == 18)
            bw_put(w, (uint32_t)ext[i], 7);
    }
}

static void webp_sym(struct bw *w, const struct pcode *pc, int sym)
{
    if (!pc->single)
        bw_put(w, pc->rcode[sym], pc->len[sym]);
}

uint8_t *lm_enc_webp(const uint32_t *px, int w, int h, size_t *out_n)
{
    if (w < 1 || h < 1 || w > 16384 || h > 16384)
        return NULL;
    size_t np = (size_t)w * (size_t)h;
    bool alpha = has_alpha(px, np);
    uint32_t hg[280] = { 0 }, hr[256] = { 0 }, hb[256] = { 0 }, ha[256] = { 0 }, hd[40] = { 0 };
    for (size_t i = 0; i < np; i++) {                                   /* subtract green: red and blue are kept as the difference to green */
        uint32_t c = px[i];
        int g = (int)((c >> 8) & 255);
        hg[g]++;
        hr[(((int)(c >> 16) & 255) - g) & 255]++;
        hb[(((int)c & 255) - g) & 255]++;
        ha[c >> 24]++;
    }
    struct bw bw = { 0 };
    bw_put(&bw, 0x2f, 8);
    bw_put(&bw, (uint32_t)(w - 1), 14);
    bw_put(&bw, (uint32_t)(h - 1), 14);
    bw_put(&bw, alpha ? 1 : 0, 1);
    bw_put(&bw, 0, 3);                                                  /* version */
    bw_put(&bw, 1, 1);                                                  /* a transform follows ... */
    bw_put(&bw, 2, 2);                                                  /* ... subtract green */
    bw_put(&bw, 0, 1);                                                  /* no more transforms */
    bw_put(&bw, 0, 1);                                                  /* no colour cache */
    bw_put(&bw, 0, 1);                                                  /* one group of prefix codes for the whole picture */
    struct pcode *pcs = malloc(5 * sizeof(struct pcode));
    if (!pcs) {
        free(bw.p);
        return NULL;
    }
    webp_code(&bw, hg, 280, &pcs[0]);
    webp_code(&bw, hr, 256, &pcs[1]);
    webp_code(&bw, hb, 256, &pcs[2]);
    webp_code(&bw, ha, 256, &pcs[3]);
    webp_code(&bw, hd, 40, &pcs[4]);
    for (size_t i = 0; i < np; i++) {
        uint32_t c = px[i];
        int g = (int)((c >> 8) & 255);
        webp_sym(&bw, &pcs[0], g);
        webp_sym(&bw, &pcs[1], (((int)(c >> 16) & 255) - g) & 255);
        webp_sym(&bw, &pcs[2], (((int)c & 255) - g) & 255);
        webp_sym(&bw, &pcs[3], (int)(c >> 24));
    }
    free(pcs);
    if (bw.nb > 0)
        bw_byte(&bw, (uint8_t)(bw.acc & 255));
    if (bw.bad) {
        free(bw.p);
        return NULL;
    }
    size_t dn = bw.n, pad = dn & 1, total = 12 + 8 + dn + pad;
    struct buf b = { 0 };
    bput(&b, "RIFF", 4);
    b32(&b, (uint32_t)(total - 8));
    bput(&b, "WEBPVP8L", 8);
    b32(&b, (uint32_t)dn);
    bput(&b, bw.p, dn);
    if (pad)
        b8(&b, 0);
    free(bw.p);
    return finish(&b, out_n);
}
