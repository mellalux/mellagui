/* PNG / JPEG / GIF / BMP decoders, see image.h. */
#include "image.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PIXELS (24u * 1000 * 1000)

void image_free(struct image *im)
{
    if (im) {
        free(im->px);
        free(im);
    }
}

static struct image *image_new(int w, int h, char *err, size_t en)
{
    if (w <= 0 || h <= 0 || (uint64_t)w * (uint64_t)h > MAX_PIXELS) {
        snprintf(err, en, "image size %dx%d is not supported", w, h);
        return NULL;
    }
    struct image *im = calloc(1, sizeof(*im));
    if (!im)
        return NULL;
    im->w = w;
    im->h = h;
    im->px = calloc((size_t)w * (size_t)h, sizeof(uint32_t));
    if (!im->px) {
        free(im);
        snprintf(err, en, "out of memory");
        return NULL;
    }
    return im;
}

/* ---- inflate (RFC 1951) ----------------------------------------------------------------------------------------- */

struct inf {
    const uint8_t *in;
    size_t n, pos;
    uint32_t bitbuf;
    int bitcnt;
    uint8_t *out;
    size_t outn, outcap;
    int err;
};

struct huff { short count[16]; short symbol[320]; };

static int getbits(struct inf *s, int need)
{
    uint32_t v = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->pos >= s->n) {
            s->err = 1;
            return 0;
        }
        v |= (uint32_t)s->in[s->pos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = v >> need;
    s->bitcnt -= need;
    return (int)(v & ((1u << need) - 1));
}

static int hdecode(struct inf *s, const struct huff *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        code |= getbits(s, 1);
        if (s->err)
            return -1;
        int count = h->count[len];
        if (code - count < first)
            return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static int hconstruct(struct huff *h, const short *length, int n)
{
    memset(h->count, 0, sizeof(h->count));
    for (int i = 0; i < n; i++)
        h->count[length[i]]++;
    if (h->count[0] == n)
        return 0;
    int left = 1;
    for (int len = 1; len <= 15; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0)
            return left;
    }
    short offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; len++)
        offs[len + 1] = (short)(offs[len] + h->count[len]);
    for (int i = 0; i < n; i++)
        if (length[i])
            h->symbol[offs[length[i]]++] = (short)i;
    return left;
}

static void put_byte(struct inf *s, uint8_t b)
{
    if (s->outn == s->outcap) {
        s->outcap = s->outcap ? s->outcap * 2 : 65536;
        if (s->outcap > (512u << 20)) {
            s->err = 1;
            return;
        }
        uint8_t *o = realloc(s->out, s->outcap);
        if (!o) {
            s->err = 1;
            return;
        }
        s->out = o;
    }
    s->out[s->outn++] = b;
}

static const short LBASE[] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const short LEXT[] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const short DBASE[] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const short DEXT[] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static int codes(struct inf *s, const struct huff *lc, const struct huff *dc)
{
    for (;;) {
        int sym = hdecode(s, lc);
        if (sym < 0 || s->err)
            return -1;
        if (sym < 256) {
            put_byte(s, (uint8_t)sym);
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29)
                return -1;
            int len = LBASE[sym] + getbits(s, LEXT[sym]);
            int ds = hdecode(s, dc);
            if (ds < 0 || ds >= 30)
                return -1;
            size_t dist = (size_t)DBASE[ds] + (size_t)getbits(s, DEXT[ds]);
            if (s->err || dist > s->outn)
                return -1;
            for (int i = 0; i < len; i++)
                put_byte(s, s->out[s->outn - dist]);
        }
        if (s->err)
            return -1;
    }
}

uint8_t *image_inflate(const uint8_t *in, size_t n, int zlib_header, size_t *outn)
{
    struct inf s = { .in = in, .n = n };
    if (zlib_header) {
        if (n < 2)
            return NULL;
        s.pos = 2;
        if (in[1] & 0x20)
            s.pos += 4;
    }
    static struct huff fixed_l, fixed_d;
    static int fixed_ready;
    if (!fixed_ready) {
        short l[288];
        int i;
        for (i = 0; i < 144; i++) l[i] = 8;
        for (; i < 256; i++) l[i] = 9;
        for (; i < 280; i++) l[i] = 7;
        for (; i < 288; i++) l[i] = 8;
        hconstruct(&fixed_l, l, 288);
        for (i = 0; i < 30; i++) l[i] = 5;
        hconstruct(&fixed_d, l, 30);
        fixed_ready = 1;
    }
    int last;
    do {
        last = getbits(&s, 1);
        int type = getbits(&s, 2);
        if (s.err)
            goto fail;
        if (type == 0) {
            s.bitbuf = 0;
            s.bitcnt = 0;
            if (s.pos + 4 > s.n)
                goto fail;
            unsigned len = in[s.pos] | (in[s.pos + 1] << 8), nlen = in[s.pos + 2] | (in[s.pos + 3] << 8);
            s.pos += 4;
            if (len != (~nlen & 0xffff) || s.pos + len > s.n)
                goto fail;
            for (unsigned i = 0; i < len; i++)
                put_byte(&s, in[s.pos++]);
        } else if (type == 1) {
            if (codes(&s, &fixed_l, &fixed_d) < 0)
                goto fail;
        } else if (type == 2) {
            int nlen = getbits(&s, 5) + 257, ndist = getbits(&s, 5) + 1, ncode = getbits(&s, 4) + 4;
            static const short order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
            short lengths[320];
            struct huff lencode, lc, dc;
            if (nlen > 286 || ndist > 30)
                goto fail;
            int idx;
            for (idx = 0; idx < ncode; idx++)
                lengths[order[idx]] = (short)getbits(&s, 3);
            for (; idx < 19; idx++)
                lengths[order[idx]] = 0;
            if (hconstruct(&lencode, lengths, 19) != 0 || s.err)
                goto fail;
            idx = 0;
            while (idx < nlen + ndist) {
                int sym = hdecode(&s, &lencode);
                if (sym < 0)
                    goto fail;
                if (sym < 16) {
                    lengths[idx++] = (short)sym;
                } else {
                    int len = 0, rep;
                    if (sym == 16) {
                        if (idx == 0)
                            goto fail;
                        len = lengths[idx - 1];
                        rep = 3 + getbits(&s, 2);
                    } else if (sym == 17) {
                        rep = 3 + getbits(&s, 3);
                    } else {
                        rep = 11 + getbits(&s, 7);
                    }
                    if (idx + rep > nlen + ndist)
                        goto fail;
                    while (rep--)
                        lengths[idx++] = (short)len;
                }
            }
            int e1 = hconstruct(&lc, lengths, nlen);
            if (e1 < 0 || (e1 > 0 && nlen - lc.count[0] != 1))
                goto fail;
            int e2 = hconstruct(&dc, lengths + nlen, ndist);
            if (e2 < 0 || (e2 > 0 && ndist - dc.count[0] != 1))
                goto fail;
            if (codes(&s, &lc, &dc) < 0)
                goto fail;
        } else {
            goto fail;
        }
    } while (!last);
    *outn = s.outn;
    return s.out ? s.out : malloc(1);
fail:
    free(s.out);
    return NULL;
}

/* ---- PNG --------------------------------------------------------------------------------------------------------- */

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static int paeth(int a, int b, int c)
{
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

/* Undo the filters of one (sub)image of `rows` rows with `stride` bytes per row, in place; returns false on a bad filter. */
static bool png_unfilter(uint8_t *d, int rows, size_t stride, int bpp)
{
    for (int y = 0; y < rows; y++) {
        uint8_t *row = d + (size_t)y * (stride + 1);
        uint8_t *prev = y ? d + (size_t)(y - 1) * (stride + 1) + 1 : NULL;
        int f = row[0];
        uint8_t *cur = row + 1;
        for (size_t i = 0; i < stride; i++) {
            int a = i >= (size_t)bpp ? cur[i - bpp] : 0, b = prev ? prev[i] : 0, c = prev && i >= (size_t)bpp ? prev[i - bpp] : 0;
            switch (f) {
            case 0: break;
            case 1: cur[i] = (uint8_t)(cur[i] + a); break;
            case 2: cur[i] = (uint8_t)(cur[i] + b); break;
            case 3: cur[i] = (uint8_t)(cur[i] + ((a + b) >> 1)); break;
            case 4: cur[i] = (uint8_t)(cur[i] + paeth(a, b, c)); break;
            default: return false;
            }
        }
    }
    return true;
}

struct pngctx { int w, h, depth, ctype; uint32_t pal[256]; int npal; bool has_trns; uint16_t trns_gray, trns_rgb[3]; uint8_t trns_alpha[256]; };

static uint32_t png_pixel(const struct pngctx *c, const uint8_t *row, int x)
{
    int depth = c->depth;
    switch (c->ctype) {
    case 0: {
        int v;
        if (depth == 16) v = row[x * 2] << 8 | row[x * 2 + 1];
        else { int per = 8 / depth, sh = (per - 1 - x % per) * depth; v = (row[x / per] >> sh) & ((1 << depth) - 1); }
        uint32_t g = depth == 16 ? (uint32_t)(v >> 8) : depth == 8 ? (uint32_t)v : (uint32_t)(v * 255 / ((1 << depth) - 1));
        uint32_t a = c->has_trns && v == c->trns_gray ? 0 : 255;
        return a << 24 | g << 16 | g << 8 | g;
    }
    case 2: {
        uint32_t r, g, b;
        int rr, gg, bb;
        if (depth == 16) { rr = row[x * 6] << 8 | row[x * 6 + 1]; gg = row[x * 6 + 2] << 8 | row[x * 6 + 3]; bb = row[x * 6 + 4] << 8 | row[x * 6 + 5]; r = (uint32_t)rr >> 8; g = (uint32_t)gg >> 8; b = (uint32_t)bb >> 8; }
        else { rr = r = row[x * 3]; gg = g = row[x * 3 + 1]; bb = b = row[x * 3 + 2]; }
        uint32_t a = c->has_trns && rr == c->trns_rgb[0] && gg == c->trns_rgb[1] && bb == c->trns_rgb[2] ? 0 : 255;
        return a << 24 | r << 16 | g << 8 | b;
    }
    case 3: {
        int per = 8 / depth, sh = (per - 1 - x % per) * depth;
        int idx = depth == 8 ? row[x] : (row[x / per] >> sh) & ((1 << depth) - 1);
        if (idx >= c->npal) return 0xff000000u;
        return (uint32_t)c->trns_alpha[idx] << 24 | (c->pal[idx] & 0xffffff);
    }
    case 4: {
        uint32_t g = depth == 16 ? row[x * 4] : row[x * 2], a = depth == 16 ? row[x * 4 + 2] : row[x * 2 + 1];
        return a << 24 | g << 16 | g << 8 | g;
    }
    default: {
        int s = depth == 16 ? 8 : 4;
        uint32_t r = row[x * s], g = row[x * s + (s / 4)], b = row[x * s + 2 * (s / 4)], a = row[x * s + 3 * (s / 4)];
        return a << 24 | r << 16 | g << 8 | b;
    }
    }
}

static struct image *png_decode(const uint8_t *d, size_t n, char *err, size_t en)
{
    struct pngctx c;
    memset(&c, 0, sizeof(c));
    memset(c.trns_alpha, 255, sizeof(c.trns_alpha));
    int interlace = 0;
    uint8_t *idat = NULL;
    size_t idatn = 0;
    size_t pos = 8;
    bool got_ihdr = false;
    while (pos + 12 <= n) {
        uint32_t len = be32(d + pos);
        const uint8_t *type = d + pos + 4, *body = d + pos + 8;
        if (len > n || pos + 12 + len > n)
            break;
        if (!memcmp(type, "IHDR", 4) && len >= 13) {
            c.w = (int)be32(body);
            c.h = (int)be32(body + 4);
            c.depth = body[8];
            c.ctype = body[9];
            interlace = body[12];
            got_ihdr = true;
        } else if (!memcmp(type, "PLTE", 4)) {
            c.npal = (int)(len / 3) > 256 ? 256 : (int)(len / 3);
            for (int i = 0; i < c.npal; i++)
                c.pal[i] = 0xff000000u | (uint32_t)body[i * 3] << 16 | (uint32_t)body[i * 3 + 1] << 8 | body[i * 3 + 2];
        } else if (!memcmp(type, "tRNS", 4)) {
            if (c.ctype == 3) {
                for (uint32_t i = 0; i < len && i < 256; i++)
                    c.trns_alpha[i] = body[i];
            } else if (c.ctype == 0 && len >= 2) {
                c.has_trns = true;
                c.trns_gray = (uint16_t)(body[0] << 8 | body[1]);
                if (c.depth < 16) c.trns_gray &= (uint16_t)((1 << c.depth) - 1);
            } else if (c.ctype == 2 && len >= 6) {
                c.has_trns = true;
                for (int i = 0; i < 3; i++) c.trns_rgb[i] = (uint16_t)(body[i * 2] << 8 | body[i * 2 + 1]);
            }
        } else if (!memcmp(type, "IDAT", 4)) {
            uint8_t *nb = realloc(idat, idatn + len + 1);
            if (!nb) { free(idat); snprintf(err, en, "out of memory"); return NULL; }
            idat = nb;
            memcpy(idat + idatn, body, len);
            idatn += len;
        } else if (!memcmp(type, "IEND", 4)) {
            break;
        }
        pos += 12 + len;
    }
    if (!got_ihdr || !idat) {
        free(idat);
        snprintf(err, en, "damaged PNG");
        return NULL;
    }
    int chans = c.ctype == 0 ? 1 : c.ctype == 2 ? 3 : c.ctype == 3 ? 1 : c.ctype == 4 ? 2 : 4;
    if ((c.depth != 1 && c.depth != 2 && c.depth != 4 && c.depth != 8 && c.depth != 16) || (c.ctype != 0 && c.ctype != 2 && c.ctype != 3 && c.ctype != 4 && c.ctype != 6)) {
        free(idat);
        snprintf(err, en, "unsupported PNG format");
        return NULL;
    }
    struct image *im = image_new(c.w, c.h, err, en);
    if (!im) {
        free(idat);
        return NULL;
    }
    size_t rawn;
    uint8_t *raw = image_inflate(idat, idatn, 1, &rawn);
    free(idat);
    if (!raw) {
        image_free(im);
        snprintf(err, en, "damaged PNG data");
        return NULL;
    }
    int bits_pp = chans * c.depth, bpp = (bits_pp + 7) / 8;
    bool ok = true;
    if (!interlace) {
        size_t stride = ((size_t)c.w * (size_t)bits_pp + 7) / 8;
        if (rawn < (stride + 1) * (size_t)c.h || !png_unfilter(raw, c.h, stride, bpp)) {
            ok = false;
        } else {
            for (int y = 0; y < c.h; y++)
                for (int x = 0; x < c.w; x++)
                    im->px[(size_t)y * (size_t)c.w + (size_t)x] = png_pixel(&c, raw + (size_t)y * (stride + 1) + 1, x);
        }
    } else {
        static const int xs[7] = { 0, 4, 0, 2, 0, 1, 0 }, ys[7] = { 0, 0, 4, 0, 2, 0, 1 }, dx[7] = { 8, 8, 4, 4, 2, 2, 1 }, dy[7] = { 8, 8, 8, 4, 4, 2, 2 };
        size_t off = 0;
        for (int p = 0; p < 7 && ok; p++) {
            int pw = (c.w - xs[p] + dx[p] - 1) / dx[p], ph = (c.h - ys[p] + dy[p] - 1) / dy[p];
            if (pw <= 0 || ph <= 0)
                continue;
            size_t stride = ((size_t)pw * (size_t)bits_pp + 7) / 8;
            if (off + (stride + 1) * (size_t)ph > rawn || !png_unfilter(raw + off, ph, stride, bpp)) {
                ok = false;
                break;
            }
            for (int y = 0; y < ph; y++)
                for (int x = 0; x < pw; x++)
                    im->px[(size_t)(ys[p] + y * dy[p]) * (size_t)c.w + (size_t)(xs[p] + x * dx[p])] =
                        png_pixel(&c, raw + off + (size_t)y * (stride + 1) + 1, x);
            off += (stride + 1) * (size_t)ph;
        }
    }
    free(raw);
    if (!ok) {
        image_free(im);
        snprintf(err, en, "damaged PNG data");
        return NULL;
    }
    return im;
}

/* ---- GIF --------------------------------------------------------------------------------------------------------- */

static struct image *gif_decode(const uint8_t *d, size_t n, char *err, size_t en)
{
    if (n < 13) goto bad;
    int sw = d[6] | d[7] << 8, sh = d[8] | d[9] << 8;
    uint32_t gpal[256];
    int gn = 0;
    size_t pos = 13;
    if (d[10] & 0x80) {
        gn = 2 << (d[10] & 7);
        if (pos + (size_t)gn * 3 > n) goto bad;
        for (int i = 0; i < gn; i++)
            gpal[i] = 0xff000000u | (uint32_t)d[pos + i * 3] << 16 | (uint32_t)d[pos + i * 3 + 1] << 8 | d[pos + i * 3 + 2];
        pos += (size_t)gn * 3;
    }
    int transp = -1;
    while (pos < n) {
        uint8_t b = d[pos++];
        if (b == 0x21) {                                        /* extension */
            if (pos >= n) goto bad;
            uint8_t label = d[pos++];
            if (label == 0xf9 && pos + 5 < n && d[pos] >= 4) {
                if (d[pos + 1] & 1) transp = d[pos + 4];
            }
            while (pos < n) {
                uint8_t sz = d[pos++];
                if (!sz) break;
                pos += sz;
            }
        } else if (b == 0x2c) {
            if (pos + 9 > n) goto bad;
            int ix = d[pos] | d[pos + 1] << 8, iy = d[pos + 2] | d[pos + 3] << 8, iw = d[pos + 4] | d[pos + 5] << 8, ih = d[pos + 6] | d[pos + 7] << 8;
            uint8_t flags = d[pos + 8];
            pos += 9;
            uint32_t pal[256];
            int pn = gn;
            memcpy(pal, gpal, sizeof(gpal));
            if (flags & 0x80) {
                pn = 2 << (flags & 7);
                if (pos + (size_t)pn * 3 > n) goto bad;
                for (int i = 0; i < pn; i++)
                    pal[i] = 0xff000000u | (uint32_t)d[pos + i * 3] << 16 | (uint32_t)d[pos + i * 3 + 1] << 8 | d[pos + i * 3 + 2];
                pos += (size_t)pn * 3;
            }
            if (pos >= n) goto bad;
            int minbits = d[pos++];
            if (minbits < 2 || minbits > 11) goto bad;
            if (sw <= 0 || sh <= 0) { sw = ix + iw; sh = iy + ih; }
            struct image *im = image_new(sw, sh, err, en);
            if (!im) return NULL;
            size_t cap = (size_t)iw * (size_t)ih;
            uint8_t *idx = calloc(cap ? cap : 1, 1);
            /* gather sub-blocks */
            uint8_t *data = malloc(n - pos + 1);
            size_t dn = 0;
            while (pos < n) {
                uint8_t sz = d[pos++];
                if (!sz) break;
                if (pos + sz > n) break;
                memcpy(data + dn, d + pos, sz);
                dn += sz;
                pos += sz;
            }
            /* LZW */
            int clear = 1 << minbits, eoi = clear + 1, next = clear + 2, size = minbits + 1;
            static uint16_t prefix[4096];
            static uint8_t suffix[4096], stack[4097];
            int prev = -1;
            uint32_t acc = 0;
            int nb = 0;
            size_t di = 0, out = 0;
            while (out < cap) {
                while (nb < size && di < dn) { acc |= (uint32_t)data[di++] << nb; nb += 8; }
                if (nb < size) break;
                int code = (int)(acc & ((1u << size) - 1));
                acc >>= size;
                nb -= size;
                if (code == clear) { next = clear + 2; size = minbits + 1; prev = -1; continue; }
                if (code == eoi) break;
                int sp = 0, cur = code;
                if (prev < 0) {
                    if (code >= clear) break;
                    idx[out++] = (uint8_t)code;
                    prev = code;
                    continue;
                }
                if (code >= next) {
                    if (code > next) break;
                    int t = prev;
                    while (t >= clear) t = prefix[t];
                    stack[sp++] = (uint8_t)t;
                    cur = prev;
                }
                while (cur >= clear && sp < 4096) { stack[sp++] = suffix[cur]; cur = prefix[cur]; }
                stack[sp++] = (uint8_t)cur;
                uint8_t first = stack[sp - 1];
                while (sp > 0 && out < cap) idx[out++] = stack[--sp];
                if (next < 4096) {
                    prefix[next] = (uint16_t)prev;
                    suffix[next] = first;
                    next++;
                    if (next == (1 << size) && size < 12) size++;
                }
                prev = code;
            }
            free(data);
            bool inter = flags & 0x40;
            for (int y = 0; y < ih; y++) {
                int ry = y;
                if (inter) {
                    int q = (ih + 7) / 8, r1 = (ih + 3) / 8, r2 = (ih + 1) / 4;
                    ry = y < q ? y * 8 : y < q + r1 ? (y - q) * 8 + 4 : y < q + r1 + r2 ? (y - q - r1) * 4 + 2 : (y - q - r1 - r2) * 2 + 1;
                }
                for (int x = 0; x < iw; x++) {
                    int dxp = ix + x, dyp = iy + ry;
                    if (dxp >= sw || dyp >= sh) continue;
                    int ci = idx[(size_t)y * (size_t)iw + (size_t)x];
                    im->px[(size_t)dyp * (size_t)sw + (size_t)dxp] = ci == transp || ci >= pn ? 0 : pal[ci];
                }
            }
            free(idx);
            return im;
        } else if (b == 0x3b) {
            break;
        } else {
            goto bad;
        }
    }
bad:
    snprintf(err, en, "damaged GIF");
    return NULL;
}

/* ---- BMP --------------------------------------------------------------------------------------------------------- */

static struct image *bmp_decode(const uint8_t *d, size_t n, char *err, size_t en)
{
    if (n < 54) goto bad;
    uint32_t off = d[10] | d[11] << 8 | d[12] << 16 | (uint32_t)d[13] << 24, hs = d[14] | d[15] << 8 | d[16] << 16 | (uint32_t)d[17] << 24;
    if (hs < 40) goto bad;
    int w = (int)be32((uint8_t[]){ d[21], d[20], d[19], d[18] }), h = (int)be32((uint8_t[]){ d[25], d[24], d[23], d[22] });
    int bpp = d[28] | d[29] << 8, comp = d[30] | d[31] << 8 | d[32] << 16 | (uint32_t)d[33] << 24;
    bool top_down = h < 0;
    if (h < 0) h = -h;
    if (comp != 0 && !(comp == 3 && (bpp == 32 || bpp == 16))) { snprintf(err, en, "compressed BMP is not supported"); return NULL; }
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 24 && bpp != 32) { snprintf(err, en, "unsupported BMP depth"); return NULL; }
    struct image *im = image_new(w, h, err, en);
    if (!im) return NULL;
    uint32_t pal[256] = { 0 };
    int ncol = d[46] | d[47] << 8;
    if (bpp <= 8) {
        if (!ncol) ncol = 1 << bpp;
        for (int i = 0; i < ncol && i < 256 && 14 + hs + (size_t)i * 4 + 3 < n; i++)
            pal[i] = 0xff000000u | (uint32_t)d[14 + hs + i * 4 + 2] << 16 | (uint32_t)d[14 + hs + i * 4 + 1] << 8 | d[14 + hs + i * 4];
    }
    size_t stride = (((size_t)w * (size_t)bpp + 31) / 32) * 4;
    for (int y = 0; y < h; y++) {
        size_t ro = off + stride * (size_t)(top_down ? y : h - 1 - y);
        if (ro + stride > n) { image_free(im); goto bad; }
        const uint8_t *row = d + ro;
        for (int x = 0; x < w; x++) {
            uint32_t c;
            if (bpp == 24) c = 0xff000000u | (uint32_t)row[x * 3 + 2] << 16 | (uint32_t)row[x * 3 + 1] << 8 | row[x * 3];
            else if (bpp == 32) c = (uint32_t)row[x * 4 + 3] << 24 | (uint32_t)row[x * 4 + 2] << 16 | (uint32_t)row[x * 4 + 1] << 8 | row[x * 4];
            else if (bpp == 8) c = pal[row[x]];
            else { int per = 8 / bpp, sh = (per - 1 - x % per) * bpp; c = pal[(row[x / per] >> sh) & ((1 << bpp) - 1)]; }
            if (bpp == 32 && !(c >> 24)) c |= 0xff000000u;      /* many writers leave the alpha byte 0 */
            im->px[(size_t)y * (size_t)w + (size_t)x] = c;
        }
    }
    return im;
bad:
    snprintf(err, en, "damaged BMP");
    return NULL;
}

/* ---- JPEG --------------------------------------------------------------------------------------------------------- */

static const uint8_t ZZ[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};

struct jhuff { uint8_t bits[17]; uint8_t vals[256]; int mincode[17], maxcode[18], valptr[17]; bool ok; };

struct jcomp {
    int id, h, v, tq;
    int bw, bh;                     /* blocks per row / column (padded to whole MCUs) */
    int cw, ch;                     /* component size in samples */
    int16_t *coef;                  /* bw * bh * 64, natural order, not dequantized */
    int dc_pred;
    int td, ta;
};

struct jpeg {
    const uint8_t *d;
    size_t n, pos;
    int w, h, ncomp, hmax, vmax, mcux, mcuy;
    bool progressive;
    struct jcomp c[4];
    uint16_t qt[4][64];             /* natural order */
    struct jhuff dc[4], ac[4];
    int restart;
    int adobe;                      /* -1 none, else transform */
    uint32_t bitbuf;
    int bitcnt;
    int eobrun;
    bool hit_marker;
    int marker;
};

static void jhuff_build(struct jhuff *h)
{
    int code = 0, k = 0;
    for (int l = 1; l <= 16; l++) {
        h->valptr[l] = k;
        h->mincode[l] = code;
        code += h->bits[l];
        k += h->bits[l];
        h->maxcode[l] = h->bits[l] ? code - 1 : -1;
        code <<= 1;
    }
    h->maxcode[17] = 0x7fffffff;
    h->ok = true;
}

static int jbit(struct jpeg *j)
{
    if (j->bitcnt == 0) {
        int b = 0;
        if (!j->hit_marker && j->pos < j->n) {
            b = j->d[j->pos++];
            if (b == 0xff) {
                int b2 = j->pos < j->n ? j->d[j->pos] : 0xd9;
                if (b2 == 0) {
                    j->pos++;
                } else {
                    j->hit_marker = true;
                    j->marker = b2;
                    j->pos--;
                    b = 0;
                }
            }
        }
        j->bitbuf = (uint32_t)b;
        j->bitcnt = 8;
    }
    j->bitcnt--;
    return (int)((j->bitbuf >> j->bitcnt) & 1);
}

static int jbits(struct jpeg *j, int n)
{
    int v = 0;
    while (n--)
        v = (v << 1) | jbit(j);
    return v;
}

static int jdecode(struct jpeg *j, const struct jhuff *h)
{
    int code = 0;
    for (int l = 1; l <= 16; l++) {
        code = (code << 1) | jbit(j);
        if (h->maxcode[l] >= 0 && code <= h->maxcode[l] && code >= h->mincode[l])
            return h->vals[h->valptr[l] + code - h->mincode[l]];
    }
    return -1;
}

static int jextend(struct jpeg *j, int s)
{
    if (!s) return 0;
    int v = jbits(j, s);
    return v < (1 << (s - 1)) ? v - (1 << s) + 1 : v;
}

/* One block of a scan. Returns false on a coding error. */
static bool jblock(struct jpeg *j, struct jcomp *c, int16_t *blk, int ss, int se, int ah, int al)
{
    if (!j->progressive) {
        int t = jdecode(j, &j->dc[c->td]);
        if (t < 0 || t > 16) return false;
        c->dc_pred += jextend(j, t);
        blk[0] = (int16_t)c->dc_pred;
        for (int k = 1; k < 64;) {
            int rs = jdecode(j, &j->ac[c->ta]);
            if (rs < 0) return false;
            int s = rs & 15, r = rs >> 4;
            if (!s) {
                if (r != 15) break;
                k += 16;
                continue;
            }
            k += r;
            if (k > 63) return false;
            blk[ZZ[k++]] = (int16_t)jextend(j, s);
        }
        return true;
    }
    if (ss == 0) {                                              /* DC scans */
        if (ah == 0) {
            int t = jdecode(j, &j->dc[c->td]);
            if (t < 0 || t > 16) return false;
            c->dc_pred += jextend(j, t);
            blk[0] = (int16_t)(c->dc_pred * (1 << al));
        } else if (jbit(j)) {
            blk[0] = (int16_t)(blk[0] | (1 << al));
        }
        return true;
    }
    if (ah == 0) {                                              /* AC first pass */
        if (j->eobrun) { j->eobrun--; return true; }
        for (int k = ss; k <= se;) {
            int rs = jdecode(j, &j->ac[c->ta]);
            if (rs < 0) return false;
            int s = rs & 15, r = rs >> 4;
            if (!s) {
                if (r < 15) {
                    j->eobrun = (1 << r) - 1;
                    if (r) j->eobrun += jbits(j, r);
                    break;
                }
                k += 16;
                continue;
            }
            k += r;
            if (k > 63) return false;
            blk[ZZ[k++]] = (int16_t)(jextend(j, s) * (1 << al));
        }
        return true;
    }
    /* AC refinement */
    int bit = 1 << al;
    if (j->eobrun) {
        j->eobrun--;
        for (int k = ss; k <= se; k++) {
            int16_t *p = &blk[ZZ[k]];
            if (*p && jbit(j) && !(*p & bit))
                *p = (int16_t)(*p > 0 ? *p + bit : *p - bit);
        }
        return true;
    }
    int k = ss;
    while (k <= se) {
        int rs = jdecode(j, &j->ac[c->ta]);
        if (rs < 0) return false;
        int s = rs & 15, r = rs >> 4;
        if (!s) {
            if (r < 15) {
                j->eobrun = (1 << r) - 1;
                if (r) j->eobrun += jbits(j, r);
                r = 64;
            }
        } else {
            s = jbit(j) ? bit : -bit;
        }
        while (k <= se) {
            int16_t *p = &blk[ZZ[k++]];
            if (*p) {
                if (jbit(j) && !(*p & bit))
                    *p = (int16_t)(*p > 0 ? *p + bit : *p - bit);
            } else {
                if (r == 0) { *p = (int16_t)s; break; }
                r--;
            }
        }
    }
    return true;
}

static bool jscan(struct jpeg *j, int *comps, int nc, int ss, int se, int ah, int al)
{
    j->bitcnt = 0;
    j->hit_marker = false;
    j->eobrun = 0;
    for (int i = 0; i < nc; i++)
        j->c[comps[i]].dc_pred = 0;
    int total, mcus_x;
    if (nc == 1) {
        struct jcomp *c = &j->c[comps[0]];
        mcus_x = (c->cw + 7) / 8;
        total = mcus_x * ((c->ch + 7) / 8);
    } else {
        mcus_x = j->mcux;
        total = j->mcux * j->mcuy;
    }
    int rst = 0;
    for (int m = 0; m < total; m++) {
        if (j->restart && m && m % j->restart == 0) {
            j->bitcnt = 0;
            /* skip to the RSTn marker */
            while (j->pos + 1 < j->n && !(j->d[j->pos] == 0xff && j->d[j->pos + 1] >= 0xd0 && j->d[j->pos + 1] <= 0xd7))
                j->pos++;
            if (j->pos + 1 >= j->n) return true;
            j->pos += 2;
            j->hit_marker = false;
            j->eobrun = 0;
            for (int i = 0; i < nc; i++) j->c[comps[i]].dc_pred = 0;
            rst++;
        }
        if (nc == 1) {
            struct jcomp *c = &j->c[comps[0]];
            int bx = m % mcus_x, by = m / mcus_x;
            if (!jblock(j, c, c->coef + ((size_t)by * (size_t)c->bw + (size_t)bx) * 64, ss, se, ah, al))
                return false;
        } else {
            int mx = m % mcus_x, my = m / mcus_x;
            for (int i = 0; i < nc; i++) {
                struct jcomp *c = &j->c[comps[i]];
                for (int v = 0; v < c->v; v++)
                    for (int h = 0; h < c->h; h++) {
                        int bx = mx * c->h + h, by = my * c->v + v;
                        if (!jblock(j, c, c->coef + ((size_t)by * (size_t)c->bw + (size_t)bx) * 64, ss, se, ah, al))
                            return false;
                    }
            }
        }
        if (j->hit_marker && j->pos >= j->n)
            return true;
    }
    return true;
}

static uint8_t clamp8(float v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)(v + 0.5f); }

static struct image *jpeg_decode(const uint8_t *d, size_t n, char *err, size_t en)
{
    struct jpeg *j = calloc(1, sizeof(*j));
    if (!j) return NULL;
    j->d = d;
    j->n = n;
    j->pos = 2;
    j->adobe = -1;
    struct image *im = NULL;
    bool got_sof = false;
    const char *why = "damaged JPEG";
    while (j->pos + 4 <= n) {
        if (d[j->pos] != 0xff) { j->pos++; continue; }
        int m = d[j->pos + 1];
        if (m == 0xff) { j->pos++; continue; }
        j->pos += 2;
        if (m == 0xd8 || (m >= 0xd0 && m <= 0xd7) || m == 0x01 || m == 0) continue;
        if (m == 0xd9) break;
        if (j->pos + 2 > n) break;
        size_t len = (size_t)(d[j->pos] << 8 | d[j->pos + 1]);
        if (len < 2 || j->pos + len > n) break;
        const uint8_t *seg = d + j->pos + 2;
        size_t sl = len - 2;
        size_t next = j->pos + len;
        if (m == 0xdb) {
            for (size_t p = 0; p < sl;) {
                int pq = seg[p] >> 4, tq = seg[p] & 15;
                p++;
                if (tq > 3 || p + (pq ? 128 : 64) > sl) break;
                for (int i = 0; i < 64; i++) {
                    j->qt[tq][ZZ[i]] = pq ? (uint16_t)(seg[p] << 8 | seg[p + 1]) : seg[p];
                    p += pq ? 2 : 1;
                }
            }
        } else if (m == 0xc4) {
            for (size_t p = 0; p + 17 <= sl;) {
                int tc = seg[p] >> 4, th = seg[p] & 15;
                struct jhuff *h = tc ? &j->ac[th & 3] : &j->dc[th & 3];
                int cnt = 0;
                h->bits[0] = 0;
                for (int i = 1; i <= 16; i++) { h->bits[i] = seg[p + i]; cnt += seg[p + i]; }
                p += 17;
                if (cnt > 256 || p + (size_t)cnt > sl) break;
                memcpy(h->vals, seg + p, (size_t)cnt);
                p += (size_t)cnt;
                jhuff_build(h);
            }
        } else if (m == 0xc0 || m == 0xc1 || m == 0xc2) {
            if (got_sof || sl < 6 || seg[0] != 8) { why = "unsupported JPEG precision"; goto fail; }
            j->progressive = m == 0xc2;
            j->h = seg[1] << 8 | seg[2];
            j->w = seg[3] << 8 | seg[4];
            j->ncomp = seg[5];
            if ((j->ncomp != 1 && j->ncomp != 3) || sl < 6 + (size_t)j->ncomp * 3 || j->w <= 0 || j->h <= 0 ||
                (uint64_t)j->w * (uint64_t)j->h > MAX_PIXELS) { why = j->ncomp == 4 ? "CMYK JPEG is not supported" : "unsupported JPEG size"; goto fail; }
            for (int i = 0; i < j->ncomp; i++) {
                j->c[i].id = seg[6 + i * 3];
                j->c[i].h = seg[7 + i * 3] >> 4;
                j->c[i].v = seg[7 + i * 3] & 15;
                j->c[i].tq = seg[8 + i * 3] & 3;
                if (j->c[i].h < 1 || j->c[i].h > 4 || j->c[i].v < 1 || j->c[i].v > 4) goto fail;
                if (j->c[i].h > j->hmax) j->hmax = j->c[i].h;
                if (j->c[i].v > j->vmax) j->vmax = j->c[i].v;
            }
            j->mcux = (j->w + 8 * j->hmax - 1) / (8 * j->hmax);
            j->mcuy = (j->h + 8 * j->vmax - 1) / (8 * j->vmax);
            for (int i = 0; i < j->ncomp; i++) {
                struct jcomp *c = &j->c[i];
                c->bw = j->mcux * c->h;
                c->bh = j->mcuy * c->v;
                c->cw = (j->w * c->h + j->hmax - 1) / j->hmax;
                c->ch = (j->h * c->v + j->vmax - 1) / j->vmax;
                c->coef = calloc((size_t)c->bw * (size_t)c->bh * 64, sizeof(int16_t));
                if (!c->coef) { why = "out of memory"; goto fail; }
            }
            got_sof = true;
        } else if (m == 0xc3 || (m >= 0xc5 && m <= 0xcf && m != 0xc4 && m != 0xc8 && m != 0xcc)) {
            why = "unsupported JPEG type (arithmetic or lossless)";
            goto fail;
        } else if (m == 0xdd && sl >= 2) {
            j->restart = seg[0] << 8 | seg[1];
        } else if (m == 0xee && sl >= 12 && !memcmp(seg, "Adobe", 5)) {
            j->adobe = seg[11];
        } else if (m == 0xda) {
            if (!got_sof || sl < 1) goto fail;
            int ns = seg[0];
            if (ns < 1 || ns > 4 || sl < 1 + (size_t)ns * 2 + 3) goto fail;
            int comps[4];
            for (int i = 0; i < ns; i++) {
                int cid = seg[1 + i * 2], tbl = seg[2 + i * 2];
                comps[i] = -1;
                for (int k = 0; k < j->ncomp; k++)
                    if (j->c[k].id == cid) comps[i] = k;
                if (comps[i] < 0) goto fail;
                j->c[comps[i]].td = (tbl >> 4) & 3;
                j->c[comps[i]].ta = tbl & 3;
            }
            int ss = seg[1 + ns * 2], se = seg[2 + ns * 2], ah = seg[3 + ns * 2] >> 4, al = seg[3 + ns * 2] & 15;
            if (!j->progressive) { ss = 0; se = 63; ah = al = 0; }
            if (se > 63 || ss > se) goto fail;
            j->pos = next;
            if (!jscan(j, comps, ns, ss, se, ah, al)) { why = "damaged JPEG data"; goto fail; }
            /* continue after the entropy-coded data: find the next marker */
            while (j->pos + 1 < n && !(d[j->pos] == 0xff && d[j->pos + 1] != 0 && !(d[j->pos + 1] >= 0xd0 && d[j->pos + 1] <= 0xd7)))
                j->pos++;
            continue;
        }
        j->pos = next;
    }
    if (!got_sof) { why = "no image in JPEG"; goto fail; }
    im = image_new(j->w, j->h, err, en);
    if (!im) goto done;
    {
        /* IDCT of every block into component planes */
        static float cs[8][8];
        static bool ready;
        if (!ready) {
            for (int x = 0; x < 8; x++)
                for (int u = 0; u < 8; u++)
                    cs[x][u] = (float)((u == 0 ? sqrt(0.125) : 0.5) * cos((2 * x + 1) * u * M_PI / 16.0));
            ready = true;
        }
        uint8_t *planes[4] = { NULL };
        for (int i = 0; i < j->ncomp; i++) {
            struct jcomp *c = &j->c[i];
            int pw = c->bw * 8, ph = c->bh * 8;
            planes[i] = malloc((size_t)pw * (size_t)ph);
            if (!planes[i]) { why = "out of memory"; for (int k = 0; k < i; k++) free(planes[k]); image_free(im); im = NULL; goto fail; }
            for (int by = 0; by < c->bh; by++)
                for (int bx = 0; bx < c->bw; bx++) {
                    const int16_t *blk = c->coef + ((size_t)by * (size_t)c->bw + (size_t)bx) * 64;
                    float in[64], tmp[64];
                    for (int k = 0; k < 64; k++)
                        in[k] = (float)blk[k] * (float)j->qt[c->tq][k];
                    for (int y = 0; y < 8; y++)                       /* rows */
                        for (int x = 0; x < 8; x++) {
                            float s = 0;
                            for (int u = 0; u < 8; u++)
                                s += in[y * 8 + u] * cs[x][u];
                            tmp[y * 8 + x] = s;
                        }
                    for (int x = 0; x < 8; x++)                       /* columns */
                        for (int y = 0; y < 8; y++) {
                            float s = 0;
                            for (int v = 0; v < 8; v++)
                                s += tmp[v * 8 + x] * cs[y][v];
                            planes[i][((size_t)by * 8 + (size_t)y) * (size_t)pw + (size_t)bx * 8 + (size_t)x] = clamp8(s + 128.0f);
                        }
                }
        }
        bool ycc = j->ncomp == 3 && j->adobe != 0;
        for (int y = 0; y < j->h; y++)
            for (int x = 0; x < j->w; x++) {
                float v[3];
                for (int i = 0; i < j->ncomp; i++) {
                    struct jcomp *c = &j->c[i];
                    if (c->h == j->hmax && c->v == j->vmax) {
                        v[i] = planes[i][(size_t)y * (size_t)(c->bw * 8) + (size_t)x];
                    } else {                                           /* subsampled: bilinear ("fancy") upsampling */
                        float fx = ((float)x + 0.5f) * (float)c->h / (float)j->hmax - 0.5f, fy = ((float)y + 0.5f) * (float)c->v / (float)j->vmax - 0.5f;
                        int ix = (int)floorf(fx), iy = (int)floorf(fy);
                        float tx = fx - (float)ix, ty = fy - (float)iy;
                        int x0 = ix < 0 ? 0 : ix >= c->cw ? c->cw - 1 : ix, x1 = ix + 1 < 0 ? 0 : ix + 1 >= c->cw ? c->cw - 1 : ix + 1;
                        int y0 = iy < 0 ? 0 : iy >= c->ch ? c->ch - 1 : iy, y1 = iy + 1 < 0 ? 0 : iy + 1 >= c->ch ? c->ch - 1 : iy + 1;
                        size_t st = (size_t)(c->bw * 8);
                        float a00 = planes[i][(size_t)y0 * st + (size_t)x0], a10 = planes[i][(size_t)y0 * st + (size_t)x1];
                        float a01 = planes[i][(size_t)y1 * st + (size_t)x0], a11 = planes[i][(size_t)y1 * st + (size_t)x1];
                        v[i] = (a00 * (1 - tx) + a10 * tx) * (1 - ty) + (a01 * (1 - tx) + a11 * tx) * ty;
                    }
                }
                uint8_t r, g, b;
                if (j->ncomp == 1) r = g = b = (uint8_t)v[0];
                else if (ycc) {
                    r = clamp8(v[0] + 1.402f * (v[2] - 128));
                    g = clamp8(v[0] - 0.344136f * (v[1] - 128) - 0.714136f * (v[2] - 128));
                    b = clamp8(v[0] + 1.772f * (v[1] - 128));
                } else { r = (uint8_t)v[0]; g = (uint8_t)v[1]; b = (uint8_t)v[2]; }
                im->px[(size_t)y * (size_t)j->w + (size_t)x] = 0xff000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | b;
            }
        for (int i = 0; i < j->ncomp; i++) free(planes[i]);
    }
    goto done;
fail:
    snprintf(err, en, "%s", why);
done:
    for (int i = 0; i < 4; i++) free(j->c[i].coef);
    free(j);
    return im;
}

/* ---- entry points ------------------------------------------------------------------------------------------------ */

struct image *image_decode(const uint8_t *d, size_t n, char *err, size_t en)
{
    if (n >= 8 && !memcmp(d, "\x89PNG\r\n\x1a\n", 8))
        return png_decode(d, n, err, en);
    if (n >= 3 && d[0] == 0xff && d[1] == 0xd8)
        return jpeg_decode(d, n, err, en);
    if (n >= 6 && (!memcmp(d, "GIF87a", 6) || !memcmp(d, "GIF89a", 6)))
        return gif_decode(d, n, err, en);
    if (n >= 2 && d[0] == 'B' && d[1] == 'M')
        return bmp_decode(d, n, err, en);
    if (n >= 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WEBP", 4))
        snprintf(err, en, "WebP images are not supported");
    else if (n >= 5 && (!memcmp(d, "<?xml", 5) || !memcmp(d, "<svg", 4)))
        snprintf(err, en, "SVG images are not supported");
    else
        snprintf(err, en, "unknown image format");
    return NULL;
}

struct image *image_scale(const struct image *src, int w, int h)
{
    if (w <= 0 || h <= 0 || !src)
        return NULL;
    struct image *o = calloc(1, sizeof(*o));
    if (!o)
        return NULL;
    o->w = w;
    o->h = h;
    o->px = malloc((size_t)w * (size_t)h * sizeof(uint32_t));
    if (!o->px) {
        free(o);
        return NULL;
    }
    double sx = (double)src->w / w, sy = (double)src->h / h;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            double x0 = x * sx, x1 = (x + 1) * sx, y0 = y * sy, y1 = (y + 1) * sy;
            if (sx <= 1 && sy <= 1) {                                    /* enlarging: bilinear */
                double fx = (x + 0.5) * sx - 0.5, fy = (y + 0.5) * sy - 0.5;
                int ix = (int)floor(fx), iy = (int)floor(fy);
                double tx = fx - ix, ty = fy - iy;
                double acc[4] = { 0 };
                for (int k = 0; k < 4; k++) {
                    int px = ix + (k & 1), py = iy + (k >> 1);
                    px = px < 0 ? 0 : px >= src->w ? src->w - 1 : px;
                    py = py < 0 ? 0 : py >= src->h ? src->h - 1 : py;
                    double wgt = ((k & 1) ? tx : 1 - tx) * ((k >> 1) ? ty : 1 - ty);
                    uint32_t c = src->px[(size_t)py * (size_t)src->w + (size_t)px];
                    double a = (c >> 24) * wgt;
                    acc[0] += a;
                    acc[1] += (double)((c >> 16) & 255) * a;
                    acc[2] += (double)((c >> 8) & 255) * a;
                    acc[3] += (double)(c & 255) * a;
                }
                if (acc[0] > 0) { acc[1] /= acc[0]; acc[2] /= acc[0]; acc[3] /= acc[0]; }
                o->px[(size_t)y * (size_t)w + (size_t)x] = (uint32_t)(acc[0] + 0.5) << 24 | (uint32_t)(acc[1] + 0.5) << 16 | (uint32_t)(acc[2] + 0.5) << 8 | (uint32_t)(acc[3] + 0.5);
                continue;
            }
            double wa = 0, r = 0, g = 0, b = 0, tot = 0;
            for (int yy = (int)y0; yy < (int)ceil(y1) && yy < src->h; yy++) {
                double wy = fmin(yy + 1, y1) - fmax(yy, y0);
                for (int xx = (int)x0; xx < (int)ceil(x1) && xx < src->w; xx++) {
                    double wx = fmin(xx + 1, x1) - fmax(xx, x0), wgt = wx * wy;
                    uint32_t c = src->px[(size_t)yy * (size_t)src->w + (size_t)xx];
                    double a = (c >> 24) * wgt;
                    wa += a;
                    r += (double)((c >> 16) & 255) * a;
                    g += (double)((c >> 8) & 255) * a;
                    b += (double)(c & 255) * a;
                    tot += wgt;
                }
            }
            uint32_t a8 = tot > 0 ? (uint32_t)(wa / tot + 0.5) : 0;
            if (wa > 0) { r /= wa; g /= wa; b /= wa; }
            o->px[(size_t)y * (size_t)w + (size_t)x] = a8 << 24 | (uint32_t)(r + 0.5) << 16 | (uint32_t)(g + 0.5) << 8 | (uint32_t)(b + 0.5);
        }
    return o;
}
