#include "webp.h"

#include "webp_tables.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WEBP_MAX_PIXELS (64u * 1024u * 1024u)

static void werr(char *err, size_t n, const char *msg)
{
    if (err && n)
        snprintf(err, n, "%s", msg);
}

int lm_webp_sniff(const uint8_t *d, size_t n)
{
    return n >= 16 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WEBP", 4);
}

static uint32_t le24(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16; }
static uint32_t le32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* ============================================================== VP8L: lossless ============================================================== */
struct lbr {
    const uint8_t *p;
    size_t n, pos;
    uint64_t val;
    int nbits;
    bool eos;
};

static void lb_fill(struct lbr *b)
{
    while (b->nbits <= 56) {
        if (b->pos < b->n)
            b->val |= (uint64_t)b->p[b->pos++] << b->nbits;
        else if (b->pos < b->n + 16)
            b->pos++;                                                       /* zeros past the end; counted so that reading much further is an error */
        else {
            b->eos = true;
            return;
        }
        b->nbits += 8;
    }
}

static uint32_t lb_bits(struct lbr *b, int n)
{
    if (n == 0)
        return 0;
    if (b->nbits < n)
        lb_fill(b);
    uint32_t v = (uint32_t)(b->val & ((1ull << n) - 1));
    b->val >>= n;
    b->nbits -= n;
    if (b->nbits < 0) {
        b->nbits = 0;
        b->eos = true;
    }
    return v;
}

#define HUFF_FAST 8

struct huff {
    uint16_t fast[1 << HUFF_FAST];                                          /* (symbol << 4 | length) for codes up to 8 bits, 0 = longer / none */
    uint16_t count[16], *sorted;
    int nsym;
    int single;                                                             /* only one symbol: it takes no bits (-1: none) */
};

static void huff_free(struct huff *h)
{
    free(h->sorted);
    h->sorted = NULL;
}

/* Builds the canonical code of the lengths (deflate order); false when the lengths are no complete code. */
static bool huff_build(struct huff *h, const uint8_t *len, int n)
{
    memset(h, 0, sizeof(*h));
    h->single = -1;
    int used = 0, last = -1;
    for (int i = 0; i < n; i++)
        if (len[i]) {
            used++;
            last = i;
            h->count[len[i]]++;
        }
    if (used == 0)
        return false;
    if (used == 1) {
        h->single = last;
        return true;
    }
    int left = 1;
    for (int l = 1; l < 16; l++) {
        left <<= 1;
        left -= h->count[l];
        if (left < 0)
            return false;
    }
    if (left != 0)
        return false;
    h->sorted = malloc((size_t)used * sizeof(uint16_t));
    if (!h->sorted)
        return false;
    uint16_t offs[16];
    offs[1] = 0;
    for (int l = 1; l < 15; l++)
        offs[l + 1] = (uint16_t)(offs[l] + h->count[l]);
    uint16_t first_code[16];
    int code = 0;
    for (int l = 1; l < 16; l++) {
        code = (code + (l > 1 ? h->count[l - 1] : 0)) << 1;
        first_code[l] = (uint16_t)code;
    }
    uint16_t o2[16];
    memcpy(o2, offs, sizeof(o2));
    for (int i = 0; i < n; i++)
        if (len[i]) {
            int l = len[i];
            h->sorted[o2[l]] = (uint16_t)i;
            if (l <= HUFF_FAST) {
                int c = first_code[l] + (o2[l] - offs[l]);
                int rev = 0;
                for (int k = 0; k < l; k++)
                    if (c & (1 << k))
                        rev |= 1 << (l - 1 - k);
                for (int f = rev; f < (1 << HUFF_FAST); f += 1 << l)
                    h->fast[f] = (uint16_t)(i << 4 | l);
            }
            o2[l]++;
        }
    h->nsym = used;
    return true;
}

static int huff_read(struct lbr *b, const struct huff *h)
{
    if (h->single >= 0)
        return h->single;
    if (b->nbits < 16)
        lb_fill(b);
    unsigned peek = (unsigned)(b->val & ((1u << HUFF_FAST) - 1));
    unsigned e = h->fast[peek];
    if (e) {
        int l = e & 15;
        b->val >>= l;
        b->nbits -= l;
        return e >> 4;
    }
    int code = 0, first = 0, index = 0;
    for (int l = 1; l < 16; l++) {
        code |= (int)(b->val & 1);
        b->val >>= 1;
        b->nbits--;
        int cnt = h->count[l];
        if (code - first < cnt)
            return h->sorted[index + code - first];
        index += cnt;
        first += cnt;
        first <<= 1;
        code <<= 1;
    }
    b->eos = true;
    return 0;
}

struct hgroup {
    struct huff h[5];
};

static const uint8_t k_cl_order[19] = { 17, 18, 0, 1, 2, 3, 4, 5, 16, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };

static bool read_code(struct lbr *b, struct huff *h, int alpha)
{
    uint8_t *lens = calloc((size_t)alpha, 1);
    if (!lens)
        return false;
    bool ok = false;
    if (lb_bits(b, 1)) {                                                    /* a simple code: one or two symbols */
        int ns = (int)lb_bits(b, 1) + 1;
        int first8 = (int)lb_bits(b, 1);
        int s0 = (int)lb_bits(b, first8 ? 8 : 1);
        if (s0 >= alpha)
            goto done;
        lens[s0] = 1;
        if (ns == 2) {
            int s1 = (int)lb_bits(b, 8);
            if (s1 >= alpha)
                goto done;
            lens[s1] = 1;
        }
        ok = huff_build(h, lens, alpha);
        if (ok && ns == 2 && h->single >= 0)
            ok = true;                                                      /* two equal symbols: one code */
    } else {
        uint8_t cl[19] = { 0 };
        int ncl = 4 + (int)lb_bits(b, 4);
        for (int i = 0; i < ncl; i++)
            cl[k_cl_order[i]] = (uint8_t)lb_bits(b, 3);
        struct huff ch;
        if (!huff_build(&ch, cl, 19))
            goto done;
        int max_symbol = alpha;
        if (lb_bits(b, 1)) {
            int nb = 2 + 2 * (int)lb_bits(b, 3);
            max_symbol = 2 + (int)lb_bits(b, nb);
        }
        int sym = 0, prev = 8;
        while (sym < alpha && !b->eos) {
            if (max_symbol-- == 0)
                break;
            int cs = huff_read(b, &ch);
            if (cs < 16) {
                lens[sym++] = (uint8_t)cs;
                if (cs)
                    prev = cs;
            } else {
                int extra = cs == 16 ? 2 : cs == 17 ? 3 : 7, off = cs == 18 ? 11 : 3;
                int rep = (int)lb_bits(b, extra) + off;
                if (sym + rep > alpha) {
                    huff_free(&ch);
                    goto done;
                }
                uint8_t v = cs == 16 ? (uint8_t)prev : 0;
                while (rep--)
                    lens[sym++] = v;
            }
        }
        huff_free(&ch);
        ok = !b->eos && huff_build(h, lens, alpha);
    }
done:
    free(lens);
    return ok;
}

static void groups_free(struct hgroup *g, size_t n)
{
    for (size_t i = 0; i < n; i++)
        for (int k = 0; k < 5; k++)
            huff_free(&g[i].h[k]);
    free(g);
}

static uint32_t cache_hash(uint32_t argb, int bits)
{
    return (0x1e35a7bdu * argb) >> (32 - bits);
}

static int prefix_value(struct lbr *b, int sym)
{
    if (sym < 4)
        return sym + 1;
    int extra = (sym - 2) >> 1, off = (2 + (sym & 1)) << extra;
    return off + (int)lb_bits(b, extra) + 1;
}

struct transform {
    int type, bits;
    int xsize;                                                              /* the width this transform works on */
    uint32_t *data;                                                         /* sub-image, or the colour table */
    int ncolors;
};

static bool decode_image_stream(struct lbr *b, int w, int h, bool level0, uint32_t **out, char *err, size_t errsz);

/* The entropy-coded part: colour cache, (meta prefix), codes, the pixels. `xsize` is the width being coded. */
static bool decode_pixels(struct lbr *b, int xsize, int ysize, bool level0, uint32_t **out, char *err, size_t errsz)
{
    int cache_bits = 0;
    if (lb_bits(b, 1)) {
        cache_bits = (int)lb_bits(b, 4);
        if (cache_bits < 1 || cache_bits > 11) {
            werr(err, errsz, "WebP: a bad colour cache");
            return false;
        }
    }
    uint32_t *meta = NULL;
    int meta_bits = 0, meta_w = 0;
    size_t ngroups = 1;
    if (level0 && lb_bits(b, 1)) {
        meta_bits = (int)lb_bits(b, 3) + 2;
        meta_w = (xsize + (1 << meta_bits) - 1) >> meta_bits;
        int meta_h = (ysize + (1 << meta_bits) - 1) >> meta_bits;
        if (!decode_image_stream(b, meta_w, meta_h, false, &meta, err, errsz))
            return false;
        uint32_t mx = 0;
        for (size_t i = 0; i < (size_t)meta_w * meta_h; i++) {
            meta[i] = (meta[i] >> 8) & 0xffff;
            if (meta[i] > mx)
                mx = meta[i];
        }
        ngroups = (size_t)mx + 1;
        if (ngroups > 65536) {
            free(meta);
            werr(err, errsz, "WebP: too many prefix groups");
            return false;
        }
    }
    struct hgroup *groups = calloc(ngroups, sizeof(*groups));
    bool ok = groups != NULL;
    int cache_size = cache_bits ? 1 << cache_bits : 0;
    for (size_t g = 0; ok && g < ngroups; g++) {
        static const int base[5] = { 256 + 24, 256, 256, 256, 40 };
        for (int k = 0; ok && k < 5; k++)
            ok = read_code(b, &groups[g].h[k], base[k] + (k == 0 ? cache_size : 0));
    }
    uint32_t *px = NULL;
    uint32_t *cache = NULL;
    if (ok) {
        size_t total = (size_t)xsize * ysize;
        px = malloc(total * sizeof(uint32_t) + 4);
        cache = cache_bits ? calloc((size_t)cache_size, sizeof(uint32_t)) : NULL;
        ok = px && (!cache_bits || cache);
        size_t pos = 0;
        int x = 0, y = 0;
        const struct hgroup *g = &groups[0];
        while (ok && pos < total) {
            if (meta && (x & ((1 << meta_bits) - 1)) == 0)
                g = &groups[meta[(size_t)(y >> meta_bits) * meta_w + (x >> meta_bits)]];
            else if (meta && pos == 0)
                g = &groups[meta[0]];
            int s = huff_read(b, &g->h[0]);
            if (b->eos) {
                ok = false;
                break;
            }
            if (s < 256) {
                uint32_t r = (uint32_t)huff_read(b, &g->h[1]), bl = (uint32_t)huff_read(b, &g->h[2]), a = (uint32_t)huff_read(b, &g->h[3]);
                uint32_t argb = a << 24 | r << 16 | (uint32_t)s << 8 | bl;
                px[pos++] = argb;
                if (cache)
                    cache[cache_hash(argb, cache_bits)] = argb;
                if (++x == xsize) {
                    x = 0;
                    y++;
                }
            } else if (s < 256 + 24) {
                int len = prefix_value(b, s - 256);
                int ds = huff_read(b, &g->h[4]);
                int dcode = prefix_value(b, ds);
                int dist;
                if (dcode > 120)
                    dist = dcode - 120;
                else {
                    int xi = k_lossless_distance_map[(dcode - 1) * 2], yi = k_lossless_distance_map[(dcode - 1) * 2 + 1];
                    dist = xi + yi * xsize;
                    if (dist < 1)
                        dist = 1;
                }
                if ((size_t)dist > pos || (size_t)len > total - pos || b->eos) {
                    ok = false;
                    break;
                }
                for (int i = 0; i < len; i++) {
                    uint32_t argb = px[pos - (size_t)dist];
                    px[pos++] = argb;
                    if (cache)
                        cache[cache_hash(argb, cache_bits)] = argb;
                    if (++x == xsize) {
                        x = 0;
                        y++;
                    }
                    if (meta && (x & ((1 << meta_bits) - 1)) == 0 && pos < total)
                        g = &groups[meta[(size_t)(y >> meta_bits) * meta_w + (x >> meta_bits)]];
                }
                if (meta && pos < total)
                    g = &groups[meta[(size_t)(y >> meta_bits) * meta_w + (x >> meta_bits)]];
            } else {
                int idx = s - 256 - 24;
                if (!cache || idx >= cache_size) {
                    ok = false;
                    break;
                }
                uint32_t argb = cache[idx];
                px[pos++] = argb;
                cache[cache_hash(argb, cache_bits)] = argb;
                if (++x == xsize) {
                    x = 0;
                    y++;
                }
            }
        }
        if (ok && b->eos)
            ok = false;
    }
    free(cache);
    groups_free(groups, ngroups);
    free(meta);
    if (!ok) {
        free(px);
        werr(err, errsz, "WebP: the lossless data is damaged");
        return false;
    }
    *out = px;
    return true;
}

static uint32_t add_px(uint32_t a, uint32_t b)
{
    return (((a & 0xff00ff00u) + (b & 0xff00ff00u)) & 0xff00ff00u) | (((a & 0x00ff00ffu) + (b & 0x00ff00ffu)) & 0x00ff00ffu);
}

static uint32_t avg2(uint32_t a, uint32_t b)
{
    return (((a ^ b) & 0xfefefefeu) >> 1) + (a & b);
}

static int clampi(int v)
{
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

static uint32_t select_px(uint32_t L, uint32_t T, uint32_t TL)
{
    int pl = 0, pt = 0;
    for (int s = 0; s < 32; s += 8) {
        int l = (int)(L >> s & 255), t = (int)(T >> s & 255), tl = (int)(TL >> s & 255);
        int p = l + t - tl;
        pl += abs(p - l);
        pt += abs(p - t);
    }
    return pl < pt ? L : T;
}

static uint32_t clamp_add_sub_full(uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t r = 0;
    for (int s = 0; s < 32; s += 8)
        r |= (uint32_t)clampi((int)(a >> s & 255) + (int)(b >> s & 255) - (int)(c >> s & 255)) << s;
    return r;
}

static uint32_t clamp_add_sub_half(uint32_t a, uint32_t b)
{
    uint32_t r = 0;
    for (int s = 0; s < 32; s += 8) {
        int x = (int)(a >> s & 255), y = (int)(b >> s & 255);
        r |= (uint32_t)clampi(x + (x - y) / 2) << s;
    }
    return r;
}

static uint32_t predict(int mode, uint32_t L, uint32_t T, uint32_t TR, uint32_t TL)
{
    switch (mode) {
    case 1: return L;
    case 2: return T;
    case 3: return TR;
    case 4: return TL;
    case 5: return avg2(avg2(L, TR), T);
    case 6: return avg2(L, TL);
    case 7: return avg2(L, T);
    case 8: return avg2(TL, T);
    case 9: return avg2(T, TR);
    case 10: return avg2(avg2(L, TL), avg2(T, TR));
    case 11: return select_px(L, T, TL);
    case 12: return clamp_add_sub_full(L, T, TL);
    case 13: return clamp_add_sub_half(avg2(L, T), TL);
    default: return 0xff000000u;
    }
}

static int8_t s8(uint32_t v) { return (int8_t)(v & 255); }
static int ct_delta(int8_t t, int8_t c) { return ((int)t * (int)c) >> 5; }

static void inverse_transform(const struct transform *t, int ysize, uint32_t *px, uint32_t *tmp_out)
{
    int w = t->xsize;
    if (t->type == 0) {                                                     /* predictor */
        int bw = (w + (1 << t->bits) - 1) >> t->bits;
        for (int y = 0; y < ysize; y++)
            for (int x = 0; x < w; x++) {
                size_t i = (size_t)y * w + x;
                uint32_t pred;
                if (y == 0)
                    pred = x == 0 ? 0xff000000u : px[i - 1];
                else if (x == 0)
                    pred = px[i - w];
                else {
                    int mode = (int)((t->data[(size_t)(y >> t->bits) * bw + (x >> t->bits)] >> 8) & 15);
                    pred = predict(mode, px[i - 1], px[i - w], px[i - w + 1], px[i - w - 1]);
                }
                px[i] = add_px(px[i], pred);
            }
    } else if (t->type == 1) {                                              /* colour transform */
        int bw = (w + (1 << t->bits) - 1) >> t->bits;
        for (int y = 0; y < ysize; y++)
            for (int x = 0; x < w; x++) {
                uint32_t e = t->data[(size_t)(y >> t->bits) * bw + (x >> t->bits)];
                int8_t g2r = s8(e), g2b = s8(e >> 8), r2b = s8(e >> 16);
                uint32_t p = px[(size_t)y * w + x];
                int8_t green = s8(p >> 8);
                int red = (int)(p >> 16 & 255), blue = (int)(p & 255);
                red += ct_delta(g2r, green);
                red &= 255;
                blue += ct_delta(g2b, green);
                blue += ct_delta(r2b, (int8_t)red);
                blue &= 255;
                px[(size_t)y * w + x] = (p & 0xff00ff00u) | (uint32_t)red << 16 | (uint32_t)blue;
            }
    } else if (t->type == 2) {                                              /* subtract green */
        for (size_t i = 0; i < (size_t)w * ysize; i++) {
            uint32_t p = px[i], g = p >> 8 & 255;
            uint32_t r = ((p >> 16 & 255) + g) & 255, b = ((p & 255) + g) & 255;
            px[i] = (p & 0xff00ff00u) | r << 16 | b;
        }
    }
    (void)tmp_out;
}

static bool decode_image_stream(struct lbr *b, int w, int h, bool level0, uint32_t **out, char *err, size_t errsz)
{
    struct transform tr[4];
    int ntr = 0;
    bool used[4] = { false };
    int xsize = w;
    if (level0)
        while (lb_bits(b, 1)) {
            int type = (int)lb_bits(b, 2);
            if (used[type] || ntr >= 4) {
                werr(err, errsz, "WebP: a transform is used twice");
                goto fail;
            }
            used[type] = true;
            struct transform *t = &tr[ntr++];
            memset(t, 0, sizeof(*t));
            t->type = type;
            t->xsize = xsize;
            if (type == 0 || type == 1) {
                t->bits = (int)lb_bits(b, 3) + 2;
                int bw = (xsize + (1 << t->bits) - 1) >> t->bits, bh = (h + (1 << t->bits) - 1) >> t->bits;
                if (!decode_image_stream(b, bw, bh, false, &t->data, err, errsz))
                    goto fail;
            } else if (type == 3) {
                t->ncolors = (int)lb_bits(b, 8) + 1;
                if (!decode_image_stream(b, t->ncolors, 1, false, &t->data, err, errsz))
                    goto fail;
                for (int i = 1; i < t->ncolors; i++)
                    t->data[i] = add_px(t->data[i], t->data[i - 1]);
                int bits = t->ncolors <= 2 ? 3 : t->ncolors <= 4 ? 2 : t->ncolors <= 16 ? 1 : 0;
                t->bits = bits;
                xsize = (xsize + (1 << bits) - 1) >> bits;
            }
        }
    uint32_t *px;
    if (!decode_pixels(b, xsize, h, level0, &px, err, errsz))
        goto fail;
    for (int i = ntr - 1; i >= 0; i--) {
        struct transform *t = &tr[i];
        if (t->type == 3) {                                                 /* colour indexing: unpack the bundled indices */
            int fw = t->xsize;
            uint32_t *full = malloc((size_t)fw * h * sizeof(uint32_t) + 4);
            if (!full) {
                free(px);
                werr(err, errsz, "out of memory");
                goto fail;
            }
            int bpp = t->bits == 3 ? 1 : t->bits == 2 ? 2 : t->bits == 1 ? 4 : 8, per = 1 << t->bits, mask = (1 << bpp) - 1;
            int rw = (fw + per - 1) >> t->bits;
            for (int y = 0; y < h; y++)
                for (int x = 0; x < fw; x++) {
                    uint32_t p = px[(size_t)y * rw + (x >> t->bits)];
                    int idx = t->bits ? (int)((p >> 8) >> (bpp * (x & (per - 1)))) & mask : (int)(p >> 8 & 255);
                    full[(size_t)y * fw + x] = idx < t->ncolors ? t->data[idx] : 0;
                }
            free(px);
            px = full;
        } else {
            inverse_transform(t, h, px, NULL);
        }
    }
    for (int i = 0; i < ntr; i++)
        free(tr[i].data);
    *out = px;
    return true;
fail:
    for (int i = 0; i < ntr; i++)
        free(tr[i].data);
    return false;
}

/* ARGB pixels of a VP8L chunk. */
static uint32_t *vp8l_decode(const uint8_t *d, size_t n, int *w, int *h, char *err, size_t errsz)
{
    if (n < 5 || d[0] != 0x2f) {
        werr(err, errsz, "WebP: not a lossless bitstream");
        return NULL;
    }
    uint32_t v = le32(d + 1);
    int ww = (int)(v & 0x3fff) + 1, hh = (int)(v >> 14 & 0x3fff) + 1;
    if ((v >> 29) != 0 || (uint64_t)ww * hh > WEBP_MAX_PIXELS) {
        werr(err, errsz, "WebP: a version or size that is not supported");
        return NULL;
    }
    struct lbr b = { d + 5, n - 5, 0, 0, 0, false };
    uint32_t *px;
    if (!decode_image_stream(&b, ww, hh, true, &px, err, errsz))
        return NULL;
    *w = ww;
    *h = hh;
    return px;
}

/* ============================================================== VP8: lossy ============================================================== */
struct bd {
    const uint8_t *p, *end;
    uint32_t value, range;
    int bit_count;
};

static void bd_init(struct bd *d, const uint8_t *p, size_t n)
{
    d->p = p;
    d->end = p + n;
    d->value = 0;
    for (int i = 0; i < 2; i++)
        d->value = d->value << 8 | (d->p < d->end ? *d->p++ : 0);
    d->range = 255;
    d->bit_count = 0;
}

static int bd_bool(struct bd *d, int prob)
{
    uint32_t split = 1 + (((d->range - 1) * (uint32_t)prob) >> 8), SPLIT = split << 8;
    int ret;
    if (d->value >= SPLIT) {
        ret = 1;
        d->range -= split;
        d->value -= SPLIT;
    } else {
        ret = 0;
        d->range = split;
    }
    while (d->range < 128) {
        d->value <<= 1;
        d->range <<= 1;
        if (++d->bit_count == 8) {
            d->bit_count = 0;
            d->value |= d->p < d->end ? *d->p++ : 0;
        }
    }
    return ret;
}

static int bd_lit(struct bd *d, int n)
{
    int v = 0;
    while (n--)
        v = v << 1 | bd_bool(d, 128);
    return v;
}

static int bd_signed(struct bd *d, int n)
{
    int v = bd_lit(d, n);
    return bd_bool(d, 128) ? -v : v;
}

enum { M_DC = 0, M_TM = 1, M_V = 2, M_H = 3, B_LD = 4, B_RD = 5, B_VR = 6, B_VL = 7, B_HD = 8, B_HU = 9 };

static const int8_t k_bmode_tree[18] = { -M_DC, 2, -M_TM, 4, -M_V, 6, 8, 12, -M_H, 10, -B_RD, -B_VR, -B_LD, 14, -B_VL, 16, -B_HD, -B_HU };
static const uint8_t k_zigzag[16] = { 0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15 };
static const uint8_t k_bands[17] = { 0, 1, 2, 3, 6, 4, 5, 6, 6, 6, 6, 6, 6, 6, 6, 7, 0 };
static const uint8_t k_cat3[] = { 173, 148, 140, 0 }, k_cat4[] = { 176, 155, 140, 135, 0 }, k_cat5[] = { 180, 157, 141, 134, 130, 0 },
                     k_cat6[] = { 254, 254, 243, 230, 196, 177, 153, 140, 133, 130, 129, 0 };
static const uint8_t *const k_cats[4] = { k_cat3, k_cat4, k_cat5, k_cat6 };

struct fstrength {
    int limit, ilevel, hev, inner;
};

struct mbinfo {
    uint8_t is_i4, segment, skip_filter;                                    /* skip_filter: no coefficients (the inner edges are left alone) */
};

struct vp8 {
    int w, h, mbw, mbh;
    uint8_t *y, *u, *v;
    int ystride, uvstride;
    /* headers */
    int use_segment, update_map, abs_delta;
    int8_t seg_quant[4], seg_filter[4];
    uint8_t seg_prob[3];
    int simple, level, sharp, use_lf_delta;
    int ref_lf_delta[4], mode_lf_delta[4];
    int nparts;
    uint8_t coef[4][8][3][11];
    int use_skip, skip_p;
    int dq[4][3][2];                                                        /* per segment: y1, y2, uv: dc, ac */
    struct fstrength fs[4][2];
};

static int clip_q(int v, int m)
{
    return v < 0 ? 0 : v > m ? m : v;
}

static int get_large(struct bd *d, const uint8_t *p)
{
    int v;
    if (!bd_bool(d, p[3])) {
        if (!bd_bool(d, p[4]))
            v = 2;
        else
            v = 3 + bd_bool(d, p[5]);
    } else if (!bd_bool(d, p[6])) {
        if (!bd_bool(d, p[7]))
            v = 5 + bd_bool(d, 159);
        else {
            v = 7 + 2 * bd_bool(d, 165);
            v += bd_bool(d, 145);
        }
    } else {
        int b1 = bd_bool(d, p[8]), b0 = bd_bool(d, p[9 + b1]), cat = 2 * b1 + b0;
        v = 0;
        for (const uint8_t *t = k_cats[cat]; *t; t++)
            v += v + bd_bool(d, *t);
        v += 3 + (8 << cat);
    }
    return v;
}

/* Tokens of one block into out (dequantised, raster order); returns the position after the last one (libwebp's "nz"). */
static int get_coeffs(struct bd *d, const uint8_t (*prob)[3][11], int ctx, const int *dq, int n, int16_t *out)
{
    const uint8_t *p = prob[k_bands[n]][ctx];
    for (; n < 16; n++) {
        if (!bd_bool(d, p[0]))
            return n;
        while (!bd_bool(d, p[1])) {
            p = prob[k_bands[++n]][0];
            if (n == 16)
                return 16;
        }
        const uint8_t *pn1 = prob[k_bands[n + 1]][1], *pn2 = prob[k_bands[n + 1]][2];
        int v;
        if (!bd_bool(d, p[2])) {
            v = 1;
            p = pn1;
        } else {
            v = get_large(d, p);
            p = pn2;
        }
        if (bd_bool(d, 128))
            v = -v;
        out[k_zigzag[n]] = (int16_t)(v * dq[n > 0]);
    }
    return 16;
}

#define MUL1(a) ((int)((((int64_t)(a) * 20091) >> 16) + (a)))              /* 64 bit: damaged data can have big coefficients */
#define MUL2(a) ((int)(((int64_t)(a) * 35468) >> 16))

static uint8_t clip8(int v)
{
    return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v;
}

static void idct_add(const int16_t *in, uint8_t *dst, int stride)
{
    int C[16], *tmp = C;
    for (int i = 0; i < 4; i++) {
        int a = in[0] + in[8], b = in[0] - in[8];
        int c = MUL2(in[4]) - MUL1(in[12]), d = MUL1(in[4]) + MUL2(in[12]);
        tmp[0] = a + d;
        tmp[1] = b + c;
        tmp[2] = b - c;
        tmp[3] = a - d;
        tmp += 4;
        in++;
    }
    tmp = C;
    for (int i = 0; i < 4; i++) {
        int dc = tmp[0] + 4;
        int a = dc + tmp[8], b = dc - tmp[8];
        int c = MUL2(tmp[4]) - MUL1(tmp[12]), d = MUL1(tmp[4]) + MUL2(tmp[12]);
        dst[0] = clip8(dst[0] + ((a + d) >> 3));
        dst[1] = clip8(dst[1] + ((b + c) >> 3));
        dst[2] = clip8(dst[2] + ((b - c) >> 3));
        dst[3] = clip8(dst[3] + ((a - d) >> 3));
        tmp++;
        dst += stride;
    }
}

static void iwht(const int16_t *in, int16_t *out)                           /* out[16 * k]: the DC of block k */
{
    int tmp[16];
    for (int i = 0; i < 4; i++) {
        int a0 = in[0 + i] + in[12 + i], a1 = in[4 + i] + in[8 + i], a2 = in[4 + i] - in[8 + i], a3 = in[0 + i] - in[12 + i];
        tmp[0 + i] = a0 + a1;
        tmp[8 + i] = a0 - a1;
        tmp[4 + i] = a3 + a2;
        tmp[12 + i] = a3 - a2;
    }
    for (int i = 0; i < 4; i++) {
        int dc = tmp[0 + i * 4] + 3;
        int a0 = dc + tmp[3 + i * 4], a1 = tmp[1 + i * 4] + tmp[2 + i * 4], a2 = tmp[1 + i * 4] - tmp[2 + i * 4], a3 = dc - tmp[3 + i * 4];
        out[(i * 4 + 0) * 16] = (int16_t)((a0 + a1) >> 3);
        out[(i * 4 + 1) * 16] = (int16_t)((a3 + a2) >> 3);
        out[(i * 4 + 2) * 16] = (int16_t)((a0 - a1) >> 3);
        out[(i * 4 + 3) * 16] = (int16_t)((a3 - a2) >> 3);
    }
}

/* The picture's pixel with the frame's edge values: 127 above the frame, 129 to its left, the last column repeated to the right. */
static int px_at(const uint8_t *pl, int stride, int cols, int x, int y)
{
    if (y < 0)
        return 127;
    if (x < 0)
        return 129;
    if (x >= cols)
        x = cols - 1;
    return pl[y * stride + x];
}

#define AVG3(a, b, c) (((a) + 2 * (b) + (c) + 2) >> 2)
#define AVG2(a, b) (((a) + (b) + 1) >> 1)

static void predict_block(uint8_t *pl, int stride, int cols, int x0, int y0, int size, int mode, bool have_top, bool have_left)
{
    uint8_t *dst = pl + y0 * stride + x0;
    if (mode == M_DC) {
        int dc = 0, shift = size == 16 ? 3 : 2;
        if (have_top && have_left) {
            for (int i = 0; i < size; i++)
                dc += px_at(pl, stride, cols, x0 + i, y0 - 1) + px_at(pl, stride, cols, x0 - 1, y0 + i);
            dc = (dc + size) >> (shift + 2);
        } else if (have_top || have_left) {
            for (int i = 0; i < size; i++)
                dc += have_top ? px_at(pl, stride, cols, x0 + i, y0 - 1) : px_at(pl, stride, cols, x0 - 1, y0 + i);
            dc = (dc + size / 2) >> (shift + 1);
        } else
            dc = 128;
        for (int y = 0; y < size; y++)
            memset(dst + y * stride, dc, (size_t)size);
    } else if (mode == M_TM) {
        int tl = px_at(pl, stride, cols, x0 - 1, y0 - 1);
        for (int y = 0; y < size; y++) {
            int l = px_at(pl, stride, cols, x0 - 1, y0 + y);
            for (int x = 0; x < size; x++)
                dst[y * stride + x] = clip8(l + px_at(pl, stride, cols, x0 + x, y0 - 1) - tl);
        }
    } else if (mode == M_V) {
        for (int y = 0; y < size; y++)
            for (int x = 0; x < size; x++)
                dst[y * stride + x] = (uint8_t)px_at(pl, stride, cols, x0 + x, y0 - 1);
    } else {
        for (int y = 0; y < size; y++)
            memset(dst + y * stride, px_at(pl, stride, cols, x0 - 1, y0 + y), (size_t)size);
    }
}

static void predict4(uint8_t *pl, int stride, int cols, int bx, int by, int mode, int tr_x, int tr_y)
{
    uint8_t *dst = pl + by * stride + bx;
    int X = px_at(pl, stride, cols, bx - 1, by - 1);
    int A = px_at(pl, stride, cols, bx, by - 1), B = px_at(pl, stride, cols, bx + 1, by - 1), C = px_at(pl, stride, cols, bx + 2, by - 1), D = px_at(pl, stride, cols, bx + 3, by - 1);
    int E = px_at(pl, stride, cols, tr_x, tr_y), F = px_at(pl, stride, cols, tr_x + 1, tr_y), G = px_at(pl, stride, cols, tr_x + 2, tr_y), H = px_at(pl, stride, cols, tr_x + 3, tr_y);
    int I = px_at(pl, stride, cols, bx - 1, by), J = px_at(pl, stride, cols, bx - 1, by + 1), K = px_at(pl, stride, cols, bx - 1, by + 2), L = px_at(pl, stride, cols, bx - 1, by + 3);
    uint8_t o[4][4];                                                        /* o[y][x] */
#define DST(x, y) o[y][x]
    switch (mode) {
    case M_DC: {
        int dc = 4 + A + B + C + D + I + J + K + L;
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++)
                o[y][x] = (uint8_t)(dc >> 3);
        break;
    }
    case M_TM: {
        int top[4] = { A, B, C, D }, left[4] = { I, J, K, L };
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++)
                o[y][x] = clip8(top[x] + left[y] - X);
        break;
    }
    case M_V: {
        uint8_t v[4] = { (uint8_t)AVG3(X, A, B), (uint8_t)AVG3(A, B, C), (uint8_t)AVG3(B, C, D), (uint8_t)AVG3(C, D, E) };
        for (int y = 0; y < 4; y++)
            memcpy(o[y], v, 4);
        break;
    }
    case M_H: {
        uint8_t r[4] = { (uint8_t)AVG3(X, I, J), (uint8_t)AVG3(I, J, K), (uint8_t)AVG3(J, K, L), (uint8_t)AVG3(K, L, L) };
        for (int y = 0; y < 4; y++)
            memset(o[y], r[y], 4);
        break;
    }
    case B_RD:
        DST(0, 3) = (uint8_t)AVG3(J, K, L);
        DST(1, 3) = DST(0, 2) = (uint8_t)AVG3(I, J, K);
        DST(2, 3) = DST(1, 2) = DST(0, 1) = (uint8_t)AVG3(X, I, J);
        DST(3, 3) = DST(2, 2) = DST(1, 1) = DST(0, 0) = (uint8_t)AVG3(A, X, I);
        DST(3, 2) = DST(2, 1) = DST(1, 0) = (uint8_t)AVG3(B, A, X);
        DST(3, 1) = DST(2, 0) = (uint8_t)AVG3(C, B, A);
        DST(3, 0) = (uint8_t)AVG3(D, C, B);
        break;
    case B_VR:
        DST(0, 0) = DST(1, 2) = (uint8_t)AVG2(X, A);
        DST(1, 0) = DST(2, 2) = (uint8_t)AVG2(A, B);
        DST(2, 0) = DST(3, 2) = (uint8_t)AVG2(B, C);
        DST(3, 0) = (uint8_t)AVG2(C, D);
        DST(0, 3) = (uint8_t)AVG3(K, J, I);
        DST(0, 2) = (uint8_t)AVG3(J, I, X);
        DST(0, 1) = DST(1, 3) = (uint8_t)AVG3(I, X, A);
        DST(1, 1) = DST(2, 3) = (uint8_t)AVG3(X, A, B);
        DST(2, 1) = DST(3, 3) = (uint8_t)AVG3(A, B, C);
        DST(3, 1) = (uint8_t)AVG3(B, C, D);
        break;
    case B_LD:
        DST(0, 0) = (uint8_t)AVG3(A, B, C);
        DST(1, 0) = DST(0, 1) = (uint8_t)AVG3(B, C, D);
        DST(2, 0) = DST(1, 1) = DST(0, 2) = (uint8_t)AVG3(C, D, E);
        DST(3, 0) = DST(2, 1) = DST(1, 2) = DST(0, 3) = (uint8_t)AVG3(D, E, F);
        DST(3, 1) = DST(2, 2) = DST(1, 3) = (uint8_t)AVG3(E, F, G);
        DST(3, 2) = DST(2, 3) = (uint8_t)AVG3(F, G, H);
        DST(3, 3) = (uint8_t)AVG3(G, H, H);
        break;
    case B_VL:
        DST(0, 0) = (uint8_t)AVG2(A, B);
        DST(1, 0) = DST(0, 2) = (uint8_t)AVG2(B, C);
        DST(2, 0) = DST(1, 2) = (uint8_t)AVG2(C, D);
        DST(3, 0) = DST(2, 2) = (uint8_t)AVG2(D, E);
        DST(0, 1) = (uint8_t)AVG3(A, B, C);
        DST(1, 1) = DST(0, 3) = (uint8_t)AVG3(B, C, D);
        DST(2, 1) = DST(1, 3) = (uint8_t)AVG3(C, D, E);
        DST(3, 1) = DST(2, 3) = (uint8_t)AVG3(D, E, F);
        DST(3, 2) = (uint8_t)AVG3(E, F, G);
        DST(3, 3) = (uint8_t)AVG3(F, G, H);
        break;
    case B_HD:
        DST(0, 0) = DST(2, 1) = (uint8_t)AVG2(I, X);
        DST(0, 1) = DST(2, 2) = (uint8_t)AVG2(J, I);
        DST(0, 2) = DST(2, 3) = (uint8_t)AVG2(K, J);
        DST(0, 3) = (uint8_t)AVG2(L, K);
        DST(3, 0) = (uint8_t)AVG3(A, B, C);
        DST(2, 0) = (uint8_t)AVG3(X, A, B);
        DST(1, 0) = DST(3, 1) = (uint8_t)AVG3(I, X, A);
        DST(1, 1) = DST(3, 2) = (uint8_t)AVG3(J, I, X);
        DST(1, 2) = DST(3, 3) = (uint8_t)AVG3(K, J, I);
        DST(1, 3) = (uint8_t)AVG3(L, K, J);
        break;
    default:                                                                /* B_HU */
        DST(0, 0) = (uint8_t)AVG2(I, J);
        DST(2, 0) = DST(0, 1) = (uint8_t)AVG2(J, K);
        DST(2, 1) = DST(0, 2) = (uint8_t)AVG2(K, L);
        DST(1, 0) = (uint8_t)AVG3(I, J, K);
        DST(3, 0) = DST(1, 1) = (uint8_t)AVG3(J, K, L);
        DST(3, 1) = DST(1, 2) = (uint8_t)AVG3(K, L, L);
        DST(3, 2) = DST(2, 2) = DST(0, 3) = DST(1, 3) = DST(2, 3) = DST(3, 3) = (uint8_t)L;
        break;
    }
#undef DST
    for (int y = 0; y < 4; y++)
        memcpy(dst + y * stride, o[y], 4);
}

/* ---- the loop filter (libwebp's form) ---- */
static int sclip1(int v) { return v < -128 ? -128 : v > 127 ? 127 : v; }
static int sclip2(int v) { return v < -16 ? -16 : v > 15 ? 15 : v; }

static void filter2(uint8_t *p, int s)
{
    int p1 = p[-2 * s], p0 = p[-s], q0 = p[0], q1 = p[s];
    int a = 3 * (q0 - p0) + sclip1(p1 - q1);
    int a1 = sclip2((a + 4) >> 3), a2 = sclip2((a + 3) >> 3);
    p[-s] = clip8(p0 + a2);
    p[0] = clip8(q0 - a1);
}

static void filter4(uint8_t *p, int s)
{
    int p1 = p[-2 * s], p0 = p[-s], q0 = p[0], q1 = p[s];
    int a = 3 * (q0 - p0);
    int a1 = sclip2((a + 4) >> 3), a2 = sclip2((a + 3) >> 3), a3 = (a1 + 1) >> 1;
    p[-2 * s] = clip8(p1 + a3);
    p[-s] = clip8(p0 + a2);
    p[0] = clip8(q0 - a1);
    p[s] = clip8(q1 - a3);
}

static void filter6(uint8_t *p, int s)
{
    int p2 = p[-3 * s], p1 = p[-2 * s], p0 = p[-s], q0 = p[0], q1 = p[s], q2 = p[2 * s];
    int a = sclip1(3 * (q0 - p0) + sclip1(p1 - q1));
    int a1 = (27 * a + 63) >> 7, a2 = (18 * a + 63) >> 7, a3 = (9 * a + 63) >> 7;
    p[-3 * s] = clip8(p2 + a3);
    p[-2 * s] = clip8(p1 + a2);
    p[-s] = clip8(p0 + a1);
    p[0] = clip8(q0 - a1);
    p[s] = clip8(q1 - a2);
    p[2 * s] = clip8(q2 - a3);
}

static bool hev(const uint8_t *p, int s, int t)
{
    return abs(p[-2 * s] - p[-s]) > t || abs(p[s] - p[0]) > t;
}

static bool needs2(const uint8_t *p, int s, int t, int it)
{
    int p3 = p[-4 * s], p2 = p[-3 * s], p1 = p[-2 * s], p0 = p[-s], q0 = p[0], q1 = p[s], q2 = p[2 * s], q3 = p[3 * s];
    if (4 * abs(p0 - q0) + abs(p1 - q1) > t)
        return false;
    return abs(p3 - p2) <= it && abs(p2 - p1) <= it && abs(p1 - p0) <= it && abs(q3 - q2) <= it && abs(q2 - q1) <= it && abs(q1 - q0) <= it;
}

static void loop26(uint8_t *p, int hs, int vs, int size, int thresh, int ithresh, int hevt)
{
    int t2 = 2 * thresh + 1;
    for (; size > 0; size--, p += vs)
        if (needs2(p, hs, t2, ithresh)) {
            if (hev(p, hs, hevt))
                filter2(p, hs);
            else
                filter6(p, hs);
        }
}

static void loop24(uint8_t *p, int hs, int vs, int size, int thresh, int ithresh, int hevt)
{
    int t2 = 2 * thresh + 1;
    for (; size > 0; size--, p += vs)
        if (needs2(p, hs, t2, ithresh)) {
            if (hev(p, hs, hevt))
                filter2(p, hs);
            else
                filter4(p, hs);
        }
}

static void simple_loop(uint8_t *p, int hs, int vs, int thresh)
{
    int t2 = 2 * thresh + 1;
    for (int i = 0; i < 16; i++, p += vs)
        if (4 * abs(p[-hs] - p[0]) + abs(p[-2 * hs] - p[hs]) <= t2)
            filter2(p, hs);
}

static void filter_frame(struct vp8 *v, const struct mbinfo *mi)
{
    for (int my = 0; my < v->mbh; my++)
        for (int mx = 0; mx < v->mbw; mx++) {
            const struct mbinfo *m = &mi[my * v->mbw + mx];
            struct fstrength f = v->fs[m->segment][m->is_i4];
            if (f.limit == 0)
                continue;
            f.inner |= !m->skip_filter;
            uint8_t *yd = v->y + my * 16 * v->ystride + mx * 16, *ud = v->u + my * 8 * v->uvstride + mx * 8, *vd = v->v + my * 8 * v->uvstride + mx * 8;
            int ys = v->ystride, us = v->uvstride, limit = f.limit;
            if (v->simple) {
                if (mx > 0)
                    simple_loop(yd, 1, ys, limit + 4);
                if (f.inner)
                    for (int k = 1; k < 4; k++)
                        simple_loop(yd + 4 * k, 1, ys, limit);
                if (my > 0)
                    simple_loop(yd, ys, 1, limit + 4);
                if (f.inner)
                    for (int k = 1; k < 4; k++)
                        simple_loop(yd + 4 * k * ys, ys, 1, limit);
            } else {
                int il = f.ilevel, ht = f.hev;
                if (mx > 0) {
                    loop26(yd, 1, ys, 16, limit + 4, il, ht);
                    loop26(ud, 1, us, 8, limit + 4, il, ht);
                    loop26(vd, 1, us, 8, limit + 4, il, ht);
                }
                if (f.inner) {
                    for (int k = 1; k < 4; k++)
                        loop24(yd + 4 * k, 1, ys, 16, limit, il, ht);
                    loop24(ud + 4, 1, us, 8, limit, il, ht);
                    loop24(vd + 4, 1, us, 8, limit, il, ht);
                }
                if (my > 0) {
                    loop26(yd, ys, 1, 16, limit + 4, il, ht);
                    loop26(ud, us, 1, 8, limit + 4, il, ht);
                    loop26(vd, us, 1, 8, limit + 4, il, ht);
                }
                if (f.inner) {
                    for (int k = 1; k < 4; k++)
                        loop24(yd + 4 * k * ys, ys, 1, 16, limit, il, ht);
                    loop24(ud + 4 * us, us, 1, 8, limit, il, ht);
                    loop24(vd + 4 * us, us, 1, 8, limit, il, ht);
                }
            }
        }
}

/* A VP8 key frame into planes; returns the vp8 struct (planes owned) or NULL. */
static struct vp8 *vp8_decode(const uint8_t *d, size_t n, char *err, size_t errsz)
{
    if (n < 10) {
        werr(err, errsz, "WebP: a lossy frame that is too short");
        return NULL;
    }
    uint32_t tag = le24(d);
    if (tag & 1) {
        werr(err, errsz, "WebP: not a key frame");
        return NULL;
    }
    uint32_t psize = tag >> 5;
    if (d[3] != 0x9d || d[4] != 0x01 || d[5] != 0x2a) {
        werr(err, errsz, "WebP: a bad VP8 start code");
        return NULL;
    }
    int w = (d[6] | d[7] << 8) & 0x3fff, h = (d[8] | d[9] << 8) & 0x3fff;
    if (w < 1 || h < 1 || (uint64_t)w * h > WEBP_MAX_PIXELS) {
        werr(err, errsz, "WebP: a size that is not supported");
        return NULL;
    }
    const uint8_t *p = d + 10;
    size_t left = n - 10;
    if (psize > left) {
        werr(err, errsz, "WebP: the first partition is cut off");
        return NULL;
    }
    struct vp8 *v = calloc(1, sizeof(*v));
    if (!v)
        return NULL;
    v->w = w;
    v->h = h;
    v->mbw = (w + 15) >> 4;
    v->mbh = (h + 15) >> 4;
    struct bd br;
    bd_init(&br, p, psize);
    bd_lit(&br, 2);                                                         /* colour space, clamping type */
    v->use_segment = bd_lit(&br, 1);
    if (v->use_segment) {
        v->update_map = bd_lit(&br, 1);
        if (bd_lit(&br, 1)) {
            v->abs_delta = bd_lit(&br, 1);
            for (int s = 0; s < 4; s++)
                v->seg_quant[s] = (int8_t)(bd_lit(&br, 1) ? bd_signed(&br, 7) : 0);
            for (int s = 0; s < 4; s++)
                v->seg_filter[s] = (int8_t)(bd_lit(&br, 1) ? bd_signed(&br, 6) : 0);
        }
        if (v->update_map)
            for (int s = 0; s < 3; s++)
                v->seg_prob[s] = (uint8_t)(bd_lit(&br, 1) ? bd_lit(&br, 8) : 255);
    }
    v->simple = bd_lit(&br, 1);
    v->level = bd_lit(&br, 6);
    v->sharp = bd_lit(&br, 3);
    v->use_lf_delta = bd_lit(&br, 1);
    if (v->use_lf_delta && bd_lit(&br, 1)) {
        for (int i = 0; i < 4; i++)
            if (bd_lit(&br, 1))
                v->ref_lf_delta[i] = bd_signed(&br, 6);
        for (int i = 0; i < 4; i++)
            if (bd_lit(&br, 1))
                v->mode_lf_delta[i] = bd_signed(&br, 6);
    }
    int filter_type = v->level == 0 ? 0 : v->simple ? 1 : 2;
    v->nparts = 1 << bd_lit(&br, 2);
    const uint8_t *pp = p + psize;
    size_t pleft = left - psize;
    if (pleft < (size_t)3 * (v->nparts - 1)) {
        werr(err, errsz, "WebP: the partition sizes are cut off");
        free(v);
        return NULL;
    }
    const uint8_t *part[8];
    size_t psz[8];
    const uint8_t *data = pp + 3 * (v->nparts - 1);
    size_t dleft = pleft - 3 * (size_t)(v->nparts - 1);
    for (int i = 0; i < v->nparts; i++) {
        size_t s = i < v->nparts - 1 ? le24(pp + 3 * i) : dleft;
        if (s > dleft)
            s = dleft;
        part[i] = data;
        psz[i] = s;
        data += s;
        dleft -= s;
    }
    int base_q = bd_lit(&br, 7);
    int dqy1_dc = bd_lit(&br, 1) ? bd_signed(&br, 4) : 0, dqy2_dc = bd_lit(&br, 1) ? bd_signed(&br, 4) : 0, dqy2_ac = bd_lit(&br, 1) ? bd_signed(&br, 4) : 0;
    int dquv_dc = bd_lit(&br, 1) ? bd_signed(&br, 4) : 0, dquv_ac = bd_lit(&br, 1) ? bd_signed(&br, 4) : 0;
    for (int s = 0; s < 4; s++) {
        int q = base_q;
        if (v->use_segment) {
            q = v->seg_quant[s];
            if (!v->abs_delta)
                q += base_q;
        }
        v->dq[s][0][0] = k_dc_qlookup[clip_q(q + dqy1_dc, 127)];
        v->dq[s][0][1] = k_ac_qlookup[clip_q(q, 127)];
        v->dq[s][1][0] = k_dc_qlookup[clip_q(q + dqy2_dc, 127)] * 2;
        v->dq[s][1][1] = (k_ac_qlookup[clip_q(q + dqy2_ac, 127)] * 101581) >> 16;
        if (v->dq[s][1][1] < 8)
            v->dq[s][1][1] = 8;
        v->dq[s][2][0] = k_dc_qlookup[clip_q(q + dquv_dc, 117)];
        v->dq[s][2][1] = k_ac_qlookup[clip_q(q + dquv_ac, 127)];
        /* the loop filter strengths of this segment */
        int base_level = v->level;
        if (v->use_segment) {
            base_level = v->seg_filter[s];
            if (!v->abs_delta)
                base_level += v->level;
        }
        for (int i4 = 0; i4 < 2; i4++) {
            int level = base_level;
            if (v->use_lf_delta) {
                level += v->ref_lf_delta[0];
                if (i4)
                    level += v->mode_lf_delta[0];
            }
            level = clip_q(level, 63);
            struct fstrength *f = &v->fs[s][i4];
            if (level > 0) {
                int il = level;
                if (v->sharp > 0) {
                    il >>= v->sharp > 4 ? 2 : 1;
                    if (il > 9 - v->sharp)
                        il = 9 - v->sharp;
                }
                if (il < 1)
                    il = 1;
                f->ilevel = il;
                f->limit = 2 * level + il;
                f->hev = level >= 40 ? 2 : level >= 15 ? 1 : 0;
            } else
                f->limit = 0;
            f->inner = i4;
        }
    }
    bd_lit(&br, 1);                                                         /* refresh_entropy_probs */
    memcpy(v->coef, k_coeff_default_probs, sizeof(v->coef));
    for (int t = 0; t < 4; t++)
        for (int b = 0; b < 8; b++)
            for (int c = 0; c < 3; c++)
                for (int k = 0; k < 11; k++)
                    if (bd_bool(&br, k_coeff_update_probs[((t * 8 + b) * 3 + c) * 11 + k]))
                        v->coef[t][b][c][k] = (uint8_t)bd_lit(&br, 8);
    v->use_skip = bd_lit(&br, 1);
    if (v->use_skip)
        v->skip_p = bd_lit(&br, 8);
    /* the planes */
    v->ystride = v->mbw * 16;
    v->uvstride = v->mbw * 8;
    size_t ysz = (size_t)v->ystride * v->mbh * 16, usz = (size_t)v->uvstride * v->mbh * 8;
    v->y = calloc(ysz + usz * 2 + 16, 1);
    struct mbinfo *mi = calloc((size_t)v->mbw * v->mbh, sizeof(*mi));
    uint8_t *top_modes = calloc((size_t)v->mbw * 4, 1);
    uint8_t *top_nz = calloc((size_t)v->mbw * 9, 1);                        /* per column: 4 Y, 2 U, 2 V, 1 Y2 */
    if (!v->y || !mi || !top_modes || !top_nz) {
        free(v->y);
        free(v);
        free(mi);
        free(top_modes);
        free(top_nz);
        werr(err, errsz, "out of memory");
        return NULL;
    }
    v->u = v->y + ysz;
    v->v = v->u + usz;
    struct bd tok[8];
    for (int i = 0; i < v->nparts; i++)
        bd_init(&tok[i], part[i], psz[i]);
    static const uint8_t k_seg_dummy = 0;
    (void)k_seg_dummy;
    for (int my = 0; my < v->mbh; my++) {
        uint8_t left_modes[4];
        uint8_t left_nz[9];
        memset(left_modes, M_DC, 4);
        memset(left_nz, 0, 9);
        struct bd *tb = &tok[my & (v->nparts - 1)];
        for (int mx = 0; mx < v->mbw; mx++) {
            struct mbinfo *m = &mi[my * v->mbw + mx];
            /* the macroblock header, from the first partition */
            if (v->update_map)
                m->segment = (uint8_t)(!bd_bool(&br, v->seg_prob[0]) ? bd_bool(&br, v->seg_prob[1]) : bd_bool(&br, v->seg_prob[2]) + 2);
            int skip = v->use_skip ? bd_bool(&br, v->skip_p) : 0;
            m->is_i4 = !bd_bool(&br, 145);
            uint8_t modes[16];
            uint8_t *top = top_modes + mx * 4;
            int ymode = M_DC;
            if (!m->is_i4) {
                ymode = bd_bool(&br, 156) ? (bd_bool(&br, 128) ? M_TM : M_H) : (bd_bool(&br, 163) ? M_V : M_DC);
                memset(top, ymode, 4);
                memset(left_modes, ymode, 4);
            } else {
                for (int y = 0; y < 4; y++) {
                    int ym = left_modes[y];
                    for (int x = 0; x < 4; x++) {
                        const uint8_t *pr = &k_kf_bmode_probs[(top[x] * 10 + ym) * 9];
                        int i = 0;
                        do
                            i = k_bmode_tree[i + bd_bool(&br, pr[i >> 1])];
                        while (i > 0);
                        ym = -i;
                        top[x] = (uint8_t)ym;
                        modes[y * 4 + x] = (uint8_t)ym;
                    }
                    left_modes[y] = (uint8_t)ym;
                }
            }
            int uvmode = !bd_bool(&br, 142) ? M_DC : !bd_bool(&br, 114) ? M_V : bd_bool(&br, 183) ? M_TM : M_H;
            /* the residual */
            int16_t coef[25][16];
            memset(coef, 0, sizeof(coef));
            bool any = false;
            uint8_t *tn = top_nz + mx * 9;
            int seg = v->use_segment ? m->segment : 0;
            const int(*dq)[2] = v->dq[seg];
            if (!skip) {
                if (!m->is_i4) {
                    int16_t dc[16] = { 0 };
                    int ctx = tn[8] + left_nz[8];
                    int nz = get_coeffs(tb, v->coef[1], ctx, dq[1], 0, dc);
                    tn[8] = left_nz[8] = nz > 0;
                    iwht(dc, &coef[0][0]);
                }
                int first = m->is_i4 ? 0 : 1;
                const uint8_t(*yp)[3][11] = v->coef[m->is_i4 ? 3 : 0];
                for (int by = 0; by < 4; by++)
                    for (int bx = 0; bx < 4; bx++) {
                        int ctx = tn[bx] + left_nz[by];
                        int nz = get_coeffs(tb, yp, ctx, dq[0], first, coef[by * 4 + bx]);
                        tn[bx] = left_nz[by] = nz > first;
                        if (nz > 1 || coef[by * 4 + bx][0])
                            any = true;
                    }
                for (int pl = 0; pl < 2; pl++)
                    for (int by = 0; by < 2; by++)
                        for (int bx = 0; bx < 2; bx++) {
                            int ti = 4 + pl * 2 + bx, li = 4 + pl * 2 + by;
                            int ctx = tn[ti] + left_nz[li];
                            int nz = get_coeffs(tb, v->coef[2], ctx, dq[2], 0, coef[16 + pl * 4 + by * 2 + bx]);
                            tn[ti] = left_nz[li] = nz > 0;
                            if (nz > 0)
                                any = true;
                        }
            } else {
                memset(tn, 0, 8);
                memset(left_nz, 0, 8);
                if (!m->is_i4)
                    tn[8] = left_nz[8] = 0;
            }
            m->skip_filter = !any;
            /* prediction + residual */
            int x0 = mx * 16, y0 = my * 16;
            if (!m->is_i4) {
                predict_block(v->y, v->ystride, v->ystride, x0, y0, 16, ymode, my > 0, mx > 0);
                for (int b = 0; b < 16; b++)
                    idct_add(coef[b], v->y + (y0 + (b >> 2) * 4) * v->ystride + x0 + (b & 3) * 4, v->ystride);
            } else {
                for (int b = 0; b < 16; b++) {
                    int bx = x0 + (b & 3) * 4, by = y0 + (b >> 2) * 4;
                    int trx = bx + 4, try_ = by - 1;
                    if ((b & 3) == 3 && (b >> 2) > 0) {                     /* the right column reads the above-right of the macroblock's first row */
                        trx = x0 + 16;
                        try_ = y0 - 1;
                    }
                    predict4(v->y, v->ystride, v->ystride, bx, by, modes[b], trx, try_);
                    idct_add(coef[b], v->y + by * v->ystride + bx, v->ystride);
                }
            }
            for (int pl = 0; pl < 2; pl++) {
                uint8_t *plane = pl ? v->v : v->u;
                predict_block(plane, v->uvstride, v->uvstride, mx * 8, my * 8, 8, uvmode, my > 0, mx > 0);
                for (int b = 0; b < 4; b++)
                    idct_add(coef[16 + pl * 4 + b], plane + (my * 8 + (b >> 1) * 4) * v->uvstride + mx * 8 + (b & 1) * 4, v->uvstride);
            }
        }
    }
    free(top_modes);
    free(top_nz);
    if (filter_type)
        filter_frame(v, mi);
    free(mi);
    return v;
}

/* ---- YUV -> RGB with libwebp's "fancy" chroma upsampling ---- */
static int mult_hi(int v, int c) { return (v * c) >> 8; }
static int clip_yuv(int v) { return (v & ~((256 << 6) - 1)) == 0 ? v >> 6 : v < 0 ? 0 : 255; }

static void put_yuv(uint8_t *dst, int y, int u, int v)
{
    dst[0] = (uint8_t)clip_yuv(mult_hi(y, 19077) + mult_hi(v, 26149) - 14234);
    dst[1] = (uint8_t)clip_yuv(mult_hi(y, 19077) - mult_hi(u, 6419) - mult_hi(v, 13320) + 8708);
    dst[2] = (uint8_t)clip_yuv(mult_hi(y, 19077) + mult_hi(u, 33050) - 17685);
    dst[3] = 255;
}

static void upsample_pair(const uint8_t *top_y, const uint8_t *bot_y, const uint8_t *top_u, const uint8_t *top_v, const uint8_t *cur_u, const uint8_t *cur_v, uint8_t *top_dst,
                          uint8_t *bot_dst, int len)
{
#define LOAD_UV(u, v) ((uint32_t)(u) | (uint32_t)(v) << 16)
    int last_pair = (len - 1) >> 1;
    uint32_t tl = LOAD_UV(top_u[0], top_v[0]), l = LOAD_UV(cur_u[0], cur_v[0]);
    uint32_t uv0 = (3 * tl + l + 0x00020002u) >> 2;
    put_yuv(top_dst, top_y[0], (int)(uv0 & 0xff), (int)(uv0 >> 16));
    if (bot_y) {
        uv0 = (3 * l + tl + 0x00020002u) >> 2;
        put_yuv(bot_dst, bot_y[0], (int)(uv0 & 0xff), (int)(uv0 >> 16));
    }
    for (int x = 1; x <= last_pair; x++) {
        uint32_t t_uv = LOAD_UV(top_u[x], top_v[x]), uv = LOAD_UV(cur_u[x], cur_v[x]);
        uint32_t avg = tl + t_uv + l + uv + 0x00080008u;
        uint32_t diag_12 = (avg + 2 * (t_uv + l)) >> 3, diag_03 = (avg + 2 * (tl + uv)) >> 3;
        uint32_t a = (diag_12 + tl) >> 1, b = (diag_03 + t_uv) >> 1;
        put_yuv(top_dst + (2 * x - 1) * 4, top_y[2 * x - 1], (int)(a & 0xff), (int)(a >> 16));
        put_yuv(top_dst + (2 * x) * 4, top_y[2 * x], (int)(b & 0xff), (int)(b >> 16));
        if (bot_y) {
            a = (diag_03 + l) >> 1;
            b = (diag_12 + uv) >> 1;
            put_yuv(bot_dst + (2 * x - 1) * 4, bot_y[2 * x - 1], (int)(a & 0xff), (int)(a >> 16));
            put_yuv(bot_dst + (2 * x) * 4, bot_y[2 * x], (int)(b & 0xff), (int)(b >> 16));
        }
        tl = t_uv;
        l = uv;
    }
    if (!(len & 1)) {
        uv0 = (3 * tl + l + 0x00020002u) >> 2;
        put_yuv(top_dst + (len - 1) * 4, top_y[len - 1], (int)(uv0 & 0xff), (int)(uv0 >> 16));
        if (bot_y) {
            uv0 = (3 * l + tl + 0x00020002u) >> 2;
            put_yuv(bot_dst + (len - 1) * 4, bot_y[len - 1], (int)(uv0 & 0xff), (int)(uv0 >> 16));
        }
    }
#undef LOAD_UV
}

static uint8_t *vp8_to_rgba(const struct vp8 *v)
{
    int w = v->w, h = v->h;
    uint8_t *out = malloc((size_t)w * h * 4 + 4);
    if (!out)
        return NULL;
    const uint8_t *Y = v->y, *U = v->u, *V = v->v;
    int ys = v->ystride, us = v->uvstride;
    upsample_pair(Y, NULL, U, V, U, V, out, NULL, w);                       /* the first row has its own chroma on both sides */
    int y = 1;
    for (; y + 1 < h; y += 2) {
        int cr = y >> 1;                                                    /* chroma rows cr (above) and cr + 1 (below) */
        upsample_pair(Y + y * ys, Y + (y + 1) * ys, U + cr * us, V + cr * us, U + (cr + 1) * us, V + (cr + 1) * us, out + (size_t)y * w * 4, out + (size_t)(y + 1) * w * 4, w);
    }
    if (y < h) {                                                            /* an even height: the last row alone */
        int cr = y >> 1;
        upsample_pair(Y + y * ys, NULL, U + cr * us, V + cr * us, U + cr * us, V + cr * us, out + (size_t)y * w * 4, NULL, w);
    }
    return out;
}

static void vp8_free(struct vp8 *v)
{
    if (v) {
        free(v->y);
        free(v);
    }
}

/* ---- ALPH: the alpha plane of a lossy picture ---- */
static uint8_t *alph_decode(const uint8_t *d, size_t n, int w, int h, char *err, size_t errsz)
{
    if (n < 2)
        return NULL;
    int method = d[0] & 3, filter = (d[0] >> 2) & 3;
    size_t total = (size_t)w * h;
    uint8_t *a = malloc(total + 1);
    if (!a)
        return NULL;
    if (method == 0) {
        if (n - 1 < total) {
            free(a);
            werr(err, errsz, "WebP: the alpha data is cut off");
            return NULL;
        }
        memcpy(a, d + 1, total);
    } else if (method == 1) {
        struct lbr b = { d + 1, n - 1, 0, 0, 0, false };
        uint32_t *px;
        if (!decode_image_stream(&b, w, h, true, &px, err, errsz)) {
            free(a);
            return NULL;
        }
        for (size_t i = 0; i < total; i++)
            a[i] = (uint8_t)(px[i] >> 8);
        free(px);
    } else {
        free(a);
        werr(err, errsz, "WebP: an alpha compression that is not known");
        return NULL;
    }
    if (filter) {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                uint8_t *p = a + (size_t)y * w + x;
                int pred;
                if (y == 0)
                    pred = x == 0 ? 0 : p[-1];
                else if (x == 0)
                    pred = p[-w];
                else if (filter == 1)
                    pred = p[-1];
                else if (filter == 2)
                    pred = p[-w];
                else {
                    int g = p[-1] + p[-w] - p[-w - 1];
                    pred = g < 0 ? 0 : g > 255 ? 255 : g;
                }
                *p = (uint8_t)(*p + pred);
            }
    }
    return a;
}

/* ---- the container ---- */
struct frame {
    const uint8_t *vp8, *vp8l, *alph;
    size_t vp8_n, vp8l_n, alph_n;
};

/* Chunks of a still image or of one animation frame: ALPH, VP8, VP8L (the others are skipped). */
static void scan_chunks(const uint8_t *p, size_t n, struct frame *f)
{
    size_t pos = 0;
    while (pos + 8 <= n) {
        uint32_t sz = le32(p + pos + 4);
        const uint8_t *body = p + pos + 8;
        size_t avail = n - pos - 8;
        size_t use = sz < avail ? sz : avail;
        if (!memcmp(p + pos, "ALPH", 4) && !f->alph) {
            f->alph = body;
            f->alph_n = use;
        } else if (!memcmp(p + pos, "VP8 ", 4) && !f->vp8 && !f->vp8l) {
            f->vp8 = body;
            f->vp8_n = use;
        } else if (!memcmp(p + pos, "VP8L", 4) && !f->vp8 && !f->vp8l) {
            f->vp8l = body;
            f->vp8l_n = use;
        }
        if (sz > avail)
            break;
        pos += 8 + (size_t)sz + (sz & 1);
    }
}

static uint8_t *decode_frame(const struct frame *f, int *w, int *h, char *err, size_t errsz)
{
    if (f->vp8l) {
        uint32_t *px = vp8l_decode(f->vp8l, f->vp8l_n, w, h, err, errsz);
        if (!px)
            return NULL;
        size_t total = (size_t)*w * *h;
        uint8_t *out = malloc(total * 4 + 4);
        if (!out) {
            free(px);
            return NULL;
        }
        for (size_t i = 0; i < total; i++) {
            uint32_t p = px[i];
            out[4 * i] = (uint8_t)(p >> 16);
            out[4 * i + 1] = (uint8_t)(p >> 8);
            out[4 * i + 2] = (uint8_t)p;
            out[4 * i + 3] = (uint8_t)(p >> 24);
        }
        free(px);
        return out;
    }
    if (f->vp8) {
        struct vp8 *v = vp8_decode(f->vp8, f->vp8_n, err, errsz);
        if (!v)
            return NULL;
        uint8_t *rgba = vp8_to_rgba(v);
        *w = v->w;
        *h = v->h;
        vp8_free(v);
        if (!rgba)
            return NULL;
        if (f->alph) {
            uint8_t *a = alph_decode(f->alph, f->alph_n, *w, *h, err, errsz);
            if (a) {
                for (size_t i = 0; i < (size_t)*w * *h; i++)
                    rgba[4 * i + 3] = a[i];
                free(a);
            }
        }
        return rgba;
    }
    werr(err, errsz, "WebP: no picture data");
    return NULL;
}

uint8_t *lm_webp_decode(const uint8_t *d, size_t n, int *w, int *h, char *err, size_t errsz)
{
    if (!lm_webp_sniff(d, n)) {
        werr(err, errsz, "not a WebP file");
        return NULL;
    }
    size_t riff = (size_t)le32(d + 4) + 8;
    if (riff < 20) {
        werr(err, errsz, "WebP: a damaged RIFF header");
        return NULL;
    }
    if (riff < n)
        n = riff;
    const uint8_t *body = d + 12;
    size_t bn = n - 12;
    struct frame f = { 0 };
    int cw = 0, ch = 0;
    bool anim = false;
    if (bn >= 18 && !memcmp(body, "VP8X", 4)) {
        uint32_t sz = le32(body + 4);
        if (sz >= 10 && bn >= 8 + (size_t)sz) {
            anim = (body[8] & 2) != 0;
            cw = (int)le24(body + 12) + 1;
            ch = (int)le24(body + 15) + 1;
        }
    }
    if (!anim) {
        scan_chunks(body, bn, &f);
        return decode_frame(&f, w, h, err, errsz);
    }
    /* an animation: the first frame (ANMF) on the canvas */
    size_t pos = 0;
    while (pos + 8 <= bn) {
        uint32_t sz = le32(body + pos + 4);
        if (sz > bn - pos - 8)
            break;
        if (!memcmp(body + pos, "ANMF", 4) && sz >= 16) {
            const uint8_t *fp = body + pos + 8;
            int fx = (int)le24(fp) * 2, fy = (int)le24(fp + 3) * 2;
            scan_chunks(fp + 16, sz - 16, &f);
            int fw = 0, fh = 0;
            uint8_t *img = decode_frame(&f, &fw, &fh, err, errsz);
            if (!img)
                return NULL;
            if ((uint64_t)cw * ch > WEBP_MAX_PIXELS || cw < 1 || ch < 1) {
                free(img);
                werr(err, errsz, "WebP: a canvas size that is not supported");
                return NULL;
            }
            uint8_t *canvas = calloc((size_t)cw * ch * 4 + 4, 1);
            if (!canvas) {
                free(img);
                return NULL;
            }
            for (int y = 0; y < fh && fy + y < ch; y++)
                for (int x = 0; x < fw && fx + x < cw; x++)
                    memcpy(canvas + ((size_t)(fy + y) * cw + fx + x) * 4, img + ((size_t)y * fw + x) * 4, 4);
            free(img);
            *w = cw;
            *h = ch;
            return canvas;
        }
        pos += 8 + (size_t)sz + (sz & 1);
    }
    werr(err, errsz, "WebP: an animation without frames");
    return NULL;
}
