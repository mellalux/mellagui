#include "img.h"
#include "webp.h"
#include "svg.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- inflate (RFC 1951; the structure follows zlib's puff.c) --------------------------------------------------------- */

#define MAXBITS 15
#define MAXLCODES 286
#define MAXDCODES 30
#define FIXLCODES 288

struct istate {
    uint8_t *out;
    size_t outlen, outcnt;
    const uint8_t *in;
    size_t inlen, incnt;
    int partial, full;                  /* lm_inflate_prefix: a full output ends the stream instead of failing */
    uint32_t bitbuf;
    int bitcnt;
};

struct huffman {
    short count[MAXBITS + 1];
    short symbol[FIXLCODES];
};

static int bits(struct istate *s, int need)             /* -1 when the input ends */
{
    uint32_t val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->incnt == s->inlen)
            return -1;
        val |= (uint32_t)s->in[s->incnt++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = val >> need;
    s->bitcnt -= need;
    return (int)(val & ((1u << need) - 1));
}

static int decode(struct istate *s, const struct huffman *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= MAXBITS; len++) {
        int b = bits(s, 1);
        if (b < 0)
            return -1;
        code |= b;
        int count = h->count[len];
        if (code - count < first)
            return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -2;
}

static int construct(struct huffman *h, const short *length, int n)
{
    short offs[MAXBITS + 1];
    for (int len = 0; len <= MAXBITS; len++)
        h->count[len] = 0;
    for (int symbol = 0; symbol < n; symbol++)
        h->count[length[symbol]]++;
    if (h->count[0] == n)
        return 0;
    int left = 1;
    for (int len = 1; len <= MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0)
            return left;                                /* over-subscribed */
    }
    offs[1] = 0;
    for (int len = 1; len < MAXBITS; len++)
        offs[len + 1] = (short)(offs[len] + h->count[len]);
    for (int symbol = 0; symbol < n; symbol++)
        if (length[symbol] != 0)
            h->symbol[offs[length[symbol]]++] = (short)symbol;
    return left;
}

static int codes(struct istate *s, const struct huffman *lencode, const struct huffman *distcode)
{
    static const short lens[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115,
                                    131, 163, 195, 227, 258 };
    static const short lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
    static const short dists[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025,
                                     1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
    static const short dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
    int symbol;
    do {
        symbol = decode(s, lencode);
        if (symbol < 0)
            return -1;
        if (symbol < 256) {
            if (s->outcnt == s->outlen) {
                s->full = s->partial;
                return s->partial ? 0 : -1;
            }
            s->out[s->outcnt++] = (uint8_t)symbol;
        } else if (symbol > 256) {
            symbol -= 257;
            if (symbol >= 29)
                return -1;
            int e = bits(s, lext[symbol]);
            if (e < 0)
                return -1;
            int len = lens[symbol] + e;
            symbol = decode(s, distcode);
            if (symbol < 0 || symbol >= 30)
                return -1;
            e = bits(s, dext[symbol]);
            if (e < 0)
                return -1;
            size_t dist = (size_t)(dists[symbol] + e);
            if (dist > s->outcnt)
                return -1;
            if (s->outcnt + (size_t)len > s->outlen) {
                if (!s->partial)
                    return -1;
                len = (int)(s->outlen - s->outcnt);
                s->full = 1;
            }
            while (len--) {
                s->out[s->outcnt] = s->out[s->outcnt - dist];
                s->outcnt++;
            }
        }
    } while (symbol != 256);
    return 0;
}

static int stored_block(struct istate *s)
{
    s->bitbuf = 0;
    s->bitcnt = 0;
    if (s->incnt + 4 > s->inlen)
        return -1;
    unsigned len = s->in[s->incnt] | (unsigned)s->in[s->incnt + 1] << 8;
    unsigned nlen = s->in[s->incnt + 2] | (unsigned)s->in[s->incnt + 3] << 8;
    s->incnt += 4;
    if (len != (~nlen & 0xffff) || s->incnt + len > s->inlen)
        return -1;
    if (s->outcnt + len > s->outlen) {
        if (!s->partial)
            return -1;
        len = (unsigned)(s->outlen - s->outcnt);
        s->full = 1;
    }
    memcpy(s->out + s->outcnt, s->in + s->incnt, len);
    s->incnt += len;
    s->outcnt += len;
    return 0;
}

static int fixed_block(struct istate *s)
{
    static struct huffman lencode, distcode;
    static int built;
    if (!built) {
        short lengths[FIXLCODES];
        int symbol;
        for (symbol = 0; symbol < 144; symbol++) lengths[symbol] = 8;
        for (; symbol < 256; symbol++) lengths[symbol] = 9;
        for (; symbol < 280; symbol++) lengths[symbol] = 7;
        for (; symbol < FIXLCODES; symbol++) lengths[symbol] = 8;
        construct(&lencode, lengths, FIXLCODES);
        for (symbol = 0; symbol < MAXDCODES; symbol++) lengths[symbol] = 5;
        construct(&distcode, lengths, MAXDCODES);
        built = 1;
    }
    return codes(s, &lencode, &distcode);
}

static int dynamic_block(struct istate *s)
{
    static const short order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    short lengths[MAXLCODES + MAXDCODES];
    struct huffman lencode, distcode;
    int nlen = bits(s, 5), ndist = bits(s, 5), ncode = bits(s, 4);
    if (nlen < 0 || ndist < 0 || ncode < 0)
        return -1;
    nlen += 257;
    ndist += 1;
    ncode += 4;
    if (nlen > MAXLCODES || ndist > MAXDCODES)
        return -1;
    int index;
    for (index = 0; index < ncode; index++) {
        int v = bits(s, 3);
        if (v < 0)
            return -1;
        lengths[order[index]] = (short)v;
    }
    for (; index < 19; index++)
        lengths[order[index]] = 0;
    if (construct(&lencode, lengths, 19) != 0)
        return -1;
    index = 0;
    while (index < nlen + ndist) {
        int symbol = decode(s, &lencode);
        if (symbol < 0)
            return -1;
        if (symbol < 16) {
            lengths[index++] = (short)symbol;
        } else {
            int len = 0, rep;
            if (symbol == 16) {
                if (index == 0)
                    return -1;
                len = lengths[index - 1];
                rep = bits(s, 2);
                rep = rep < 0 ? rep : rep + 3;
            } else if (symbol == 17) {
                rep = bits(s, 3);
                rep = rep < 0 ? rep : rep + 3;
            } else {
                rep = bits(s, 7);
                rep = rep < 0 ? rep : rep + 11;
            }
            if (rep < 0 || index + rep > nlen + ndist)
                return -1;
            while (rep--)
                lengths[index++] = (short)len;
        }
    }
    if (lengths[256] == 0)
        return -1;
    int err = construct(&lencode, lengths, nlen);
    if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1))
        return -1;
    err = construct(&distcode, lengths + nlen, ndist);
    if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1))
        return -1;
    return codes(s, &lencode, &distcode);
}

/* zlib stream -> exactly `outlen` bytes. */
static int zlib_inflate(const uint8_t *in, size_t inlen, uint8_t *out, size_t outlen)
{
    if (inlen < 2 || (in[0] & 15) != 8 || ((in[0] << 8) | in[1]) % 31 != 0 || (in[1] & 0x20))
        return -1;
    struct istate s = { .out = out, .outlen = outlen, .in = in + 2, .inlen = inlen - 2 };
    int last;
    do {
        last = bits(&s, 1);
        int type = bits(&s, 2);
        if (last < 0 || type < 0)
            return -1;
        int err = type == 0 ? stored_block(&s) : type == 1 ? fixed_block(&s) : type == 2 ? dynamic_block(&s) : -1;
        if (err)
            return -1;
    } while (!last);
    return s.outcnt == outlen ? 0 : -1;
}

/* A raw deflate stream (no zlib / gzip wrapper) -> exactly `outlen` bytes; 0 = ok. For file formats that carry their own wrapper (Parquet's gzip pages). */
int lm_inflate_raw(const uint8_t *in, size_t inlen, uint8_t *out, size_t outlen)
{
    struct istate s = { .out = out, .outlen = outlen, .in = in, .inlen = inlen };
    int last;
    do {
        last = bits(&s, 1);
        int type = bits(&s, 2);
        if (last < 0 || type < 0)
            return -1;
        int err = type == 0 ? stored_block(&s) : type == 1 ? fixed_block(&s) : type == 2 ? dynamic_block(&s) : -1;
        if (err)
            return -1;
    } while (!last);
    return s.outcnt == outlen ? 0 : -1;
}

/* The first bytes of a raw deflate stream: up to `outcap` of them, then it stops (a big spreadsheet's first rows need not the whole sheet inflated).
 * Returns how many bytes were made (the whole stream when it is shorter; a stream cut or broken later keeps what came before), or -1 when none. */
long lm_inflate_prefix(const uint8_t *in, size_t inlen, uint8_t *out, size_t outcap)
{
    struct istate s = { .out = out, .outlen = outcap, .in = in, .inlen = inlen, .partial = 1 };
    int last;
    do {
        last = bits(&s, 1);
        int type = bits(&s, 2);
        if (last < 0 || type < 0)
            break;
        int err = type == 0 ? stored_block(&s) : type == 1 ? fixed_block(&s) : type == 2 ? dynamic_block(&s) : -1;
        if (err)
            break;
    } while (!last && !s.full);
    if (!s.full && !last)                       /* the input ended or broke in the middle: what came before is still good */
        return s.outcnt ? (long)s.outcnt : -1;
    return (long)s.outcnt;
}

/* ---- PNG ---------------------------------------------------------------------------------------------------------------- */

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void fail(char *err, size_t n, const char *msg)
{
    snprintf(err, n, "%s", msg);
}

static int g_keep_alpha;                                /* lm_img_load_alpha: PNG pixels keep their alpha (ARGB, not premultiplied) */

static uint32_t over_checker(int x, int y, int r, int g, int b, int a)
{
    if (g_keep_alpha)
        return (uint32_t)a << 24 | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
    int bg = (((x >> 3) ^ (y >> 3)) & 1) ? 0x66 : 0x44;
    if (a >= 255)
        return (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
    r = (r * a + bg * (255 - a)) / 255;
    g = (g * a + bg * (255 - a)) / 255;
    b = (b * a + bg * (255 - a)) / 255;
    return (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
}

static int decode_png(const uint8_t *d, size_t len, struct lm_image *img, char *err, size_t errsz)
{
    uint32_t w = 0, h = 0;
    int depth = 0, ctype = -1;
    uint8_t plte[256 * 3], trns[256];
    int nplte = 0, ntrns = 0;
    memset(trns, 255, sizeof(trns));
    uint8_t *idat = NULL;
    size_t idat_len = 0, idat_cap = 0;
    int rc = -1;
    uint8_t *raw = NULL;
    uint32_t *px = NULL;

    size_t off = 8;
    int seen_ihdr = 0, seen_iend = 0;
    while (off + 12 <= len && !seen_iend) {
        uint32_t n = be32(d + off);
        const uint8_t *type = d + off + 4, *body = d + off + 8;
        if (n > len - off - 12) {
            fail(err, errsz, "truncated PNG");
            goto out;
        }
        if (!memcmp(type, "IHDR", 4) && n == 13) {
            w = be32(body);
            h = be32(body + 4);
            depth = body[8];
            ctype = body[9];
            if (body[10] != 0 || body[11] != 0) {
                fail(err, errsz, "unknown PNG compression or filter method");
                goto out;
            }
            if (body[12] != 0) {
                fail(err, errsz, "interlaced PNG is not supported");
                goto out;
            }
            seen_ihdr = 1;
        } else if (!memcmp(type, "PLTE", 4) && n % 3 == 0 && n <= 768) {
            memcpy(plte, body, n);
            nplte = (int)(n / 3);
        } else if (!memcmp(type, "tRNS", 4) && ctype == 3 && n <= 256) {
            memcpy(trns, body, n);
            ntrns = (int)n;
        } else if (!memcmp(type, "IDAT", 4)) {
            if (idat_len + n > idat_cap) {
                size_t nc = (idat_len + n) * 2 + 1024;
                uint8_t *t = realloc(idat, nc);
                if (!t) {
                    fail(err, errsz, "out of memory");
                    goto out;
                }
                idat = t;
                idat_cap = nc;
            }
            memcpy(idat + idat_len, body, n);
            idat_len += n;
        } else if (!memcmp(type, "IEND", 4)) {
            seen_iend = 1;
        }
        off += 12 + (size_t)n;
    }
    (void)ntrns;
    if (!seen_ihdr || !idat_len) {
        fail(err, errsz, "not a complete PNG");
        goto out;
    }
    int channels = ctype == 0 ? 1 : ctype == 2 ? 3 : ctype == 3 ? 1 : ctype == 4 ? 2 : ctype == 6 ? 4 : 0;
    int depth_ok = depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16;
    if (!channels || !depth_ok || (ctype == 3 && depth == 16) || ((ctype == 2 || ctype == 4 || ctype == 6) && depth < 8)) {
        fail(err, errsz, "unsupported PNG colour type or bit depth");
        goto out;
    }
    if (w == 0 || h == 0 || w > 32768 || h > 32768 || (uint64_t)w * h > LM_IMG_MAX_PIXELS) {
        fail(err, errsz, "image too large");
        goto out;
    }
    if (ctype == 3 && nplte == 0) {
        fail(err, errsz, "palette missing");
        goto out;
    }
    size_t rowbytes = ((size_t)w * (size_t)channels * (size_t)depth + 7) / 8;
    size_t rawlen = (rowbytes + 1) * h;
    raw = malloc(rawlen);
    px = malloc((size_t)w * h * 4);
    if (!raw || !px) {
        fail(err, errsz, "out of memory");
        goto out;
    }
    if (zlib_inflate(idat, idat_len, raw, rawlen) < 0) {
        fail(err, errsz, "damaged PNG data");
        goto out;
    }
    size_t bpp = ((size_t)channels * (size_t)depth + 7) / 8;
    for (uint32_t y = 0; y < h; y++) {                   /* undo the row filters in place */
        uint8_t *row = raw + y * (rowbytes + 1) + 1, ft = row[-1];
        const uint8_t *up = y ? row - (rowbytes + 1) : NULL;
        if (ft > 4) {
            fail(err, errsz, "damaged PNG filter");
            goto out;
        }
        for (size_t i = 0; i < rowbytes; i++) {
            int a = i >= bpp ? row[i - bpp] : 0, b = up ? up[i] : 0, c = (up && i >= bpp) ? up[i - bpp] : 0, v = row[i];
            switch (ft) {
            case 1: v += a; break;
            case 2: v += b; break;
            case 3: v += (a + b) / 2; break;
            case 4: {
                int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
                v += (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                break;
            }
            }
            row[i] = (uint8_t)v;
        }
    }
    int mask = (1 << (depth < 8 ? depth : 8)) - 1;
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *row = raw + y * (rowbytes + 1) + 1;
        for (uint32_t x = 0; x < w; x++) {
            int r, g, b, a = 255;
            if (depth < 8) {                             /* grey or palette, 1/2/4 bits */
                size_t bit = (size_t)x * (size_t)depth;
                int v = (row[bit / 8] >> (8 - depth - (int)(bit % 8))) & mask;
                if (ctype == 3) {
                    if (v >= nplte) {
                        fail(err, errsz, "palette index out of range");
                        goto out;
                    }
                    r = plte[v * 3]; g = plte[v * 3 + 1]; b = plte[v * 3 + 2]; a = trns[v];
                } else {
                    r = g = b = v * 255 / mask;
                }
            } else {
                size_t step = depth == 16 ? 2 : 1;
                const uint8_t *p = row + (size_t)x * (size_t)channels * step;
                if (ctype == 3) {
                    int v = p[0];
                    if (v >= nplte) {
                        fail(err, errsz, "palette index out of range");
                        goto out;
                    }
                    r = plte[v * 3]; g = plte[v * 3 + 1]; b = plte[v * 3 + 2]; a = trns[v];
                } else if (ctype == 0 || ctype == 4) {
                    r = g = b = p[0];
                    if (ctype == 4)
                        a = p[step];
                } else {
                    r = p[0]; g = p[step]; b = p[2 * step];
                    if (ctype == 6)
                        a = p[3 * step];
                }
            }
            px[(size_t)y * w + x] = over_checker((int)x, (int)y, r, g, b, a);
        }
    }
    img->w = (int)w;
    img->h = (int)h;
    img->px = px;
    px = NULL;
    rc = 0;
out:
    free(idat);
    free(raw);
    free(px);
    return rc;
}

/* ---- PPM (P6, maxval 255) ----------------------------------------------------------------------------------------------- */

static int ppm_int(const uint8_t *d, size_t len, size_t *off, unsigned *v)
{
    while (*off < len && (d[*off] == ' ' || d[*off] == '\n' || d[*off] == '\t' || d[*off] == '\r' || d[*off] == '#')) {
        if (d[*off] == '#')
            while (*off < len && d[*off] != '\n')
                (*off)++;
        else
            (*off)++;
    }
    if (*off >= len || d[*off] < '0' || d[*off] > '9')
        return -1;
    unsigned x = 0;
    while (*off < len && d[*off] >= '0' && d[*off] <= '9') {
        x = x * 10 + (d[*off] - '0');
        if (x > 1000000)
            return -1;
        (*off)++;
    }
    *v = x;
    return 0;
}

static int decode_ppm(const uint8_t *d, size_t len, struct lm_image *img, char *err, size_t errsz)
{
    size_t off = 2;
    unsigned w, h, maxv;
    if (ppm_int(d, len, &off, &w) < 0 || ppm_int(d, len, &off, &h) < 0 || ppm_int(d, len, &off, &maxv) < 0 ||
        off >= len || maxv != 255 || !w || !h || (uint64_t)w * h > LM_IMG_MAX_PIXELS) {
        fail(err, errsz, "unsupported PPM");
        return -1;
    }
    off++;                                               /* the single whitespace byte before the data */
    if (len - off < (size_t)w * h * 3) {
        fail(err, errsz, "truncated PPM");
        return -1;
    }
    uint32_t *px = malloc((size_t)w * h * 4);
    if (!px) {
        fail(err, errsz, "out of memory");
        return -1;
    }
    for (size_t i = 0; i < (size_t)w * h; i++)
        px[i] = (uint32_t)d[off + i * 3] << 16 | (uint32_t)d[off + i * 3 + 1] << 8 | d[off + i * 3 + 2];
    img->w = (int)w;
    img->h = (int)h;
    img->px = px;
    return 0;
}

/* ---- GIF (the first picture), BMP, ICO / CUR, PNM P1..P5 ------------------------------------------------------------------ */

#define IMG_MAX_PIXELS (100u * 1000 * 1000)

static bool img_alloc(struct lm_image *img, uint32_t w, uint32_t h, char *err, size_t errsz)
{
    if (w < 1 || h < 1 || w > 65535 || h > 65535 || (uint64_t)w * h > IMG_MAX_PIXELS) {
        fail(err, errsz, "the picture has a size that is not supported");
        return false;
    }
    img->px = calloc((size_t)w * h, sizeof(uint32_t));
    if (!img->px) {
        fail(err, errsz, "out of memory");
        return false;
    }
    img->w = (int)w;
    img->h = (int)h;
    return true;
}

static uint32_t le16(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* GIF: the first picture on the logical screen (later pictures of an animation are not decoded); the colour of the transparent index shows as the checker. */
static int decode_gif(const uint8_t *d, size_t len, struct lm_image *img, char *err, size_t errsz)
{
    if (len < 13) {
        fail(err, errsz, "GIF: too short");
        return -1;
    }
    uint32_t sw = le16(d + 6), sh = le16(d + 8);
    unsigned flags = d[10];
    size_t p = 13;
    uint8_t gct[768];
    int gn = 0;
    if (flags & 0x80) {
        gn = 2 << (flags & 7);
        if (p + (size_t)gn * 3 > len) {
            fail(err, errsz, "GIF: the colour table is cut off");
            return -1;
        }
        memcpy(gct, d + p, (size_t)gn * 3);
        p += (size_t)gn * 3;
    }
    int transparent = -1;
    while (p < len) {
        uint8_t b = d[p++];
        if (b == 0x21) {                                                /* an extension */
            if (p >= len)
                break;
            uint8_t label = d[p++];
            if (label == 0xf9 && p + 5 < len && d[p] >= 4 && (d[p + 1] & 1))
                transparent = d[p + 4];
            while (p < len && d[p]) {                                   /* its sub-blocks */
                p += (size_t)d[p] + 1;
            }
            p++;
        } else if (b == 0x2c) {                                         /* the picture */
            if (p + 9 > len)
                break;
            uint32_t left = le16(d + p), top = le16(d + p + 2), iw = le16(d + p + 4), ih = le16(d + p + 6);
            unsigned f = d[p + 8];
            p += 9;
            uint8_t lct[768];
            const uint8_t *pal = gct;
            int pn = gn;
            if (f & 0x80) {
                pn = 2 << (f & 7);
                if (p + (size_t)pn * 3 > len) {
                    fail(err, errsz, "GIF: the colour table is cut off");
                    return -1;
                }
                memcpy(lct, d + p, (size_t)pn * 3);
                pal = lct;
                p += (size_t)pn * 3;
            }
            if (!pn || p >= len) {
                fail(err, errsz, "GIF: no colours");
                return -1;
            }
            if (sw == 0 || sh == 0) {
                sw = left + iw;
                sh = top + ih;
            }
            if (!img_alloc(img, sw, sh, err, errsz))
                return -1;
            for (uint32_t y = 0; y < sh; y++)
                for (uint32_t x = 0; x < sw; x++)
                    img->px[(size_t)y * sw + x] = over_checker((int)x, (int)y, 0, 0, 0, 0);       /* outside the picture: transparent */
            int minbits = d[p++];
            if (minbits < 1 || minbits > 11) {
                lm_img_free(img);
                fail(err, errsz, "GIF: bad code size");
                return -1;
            }
            /* the sub-blocks -> one buffer */
            size_t cap = 0, dn = 0;
            uint8_t *data = NULL;
            while (p < len && d[p]) {
                size_t bl = d[p++];
                if (p + bl > len)
                    bl = len - p;
                if (dn + bl > cap) {
                    cap = (dn + bl) * 2;
                    data = realloc(data, cap);
                }
                memcpy(data + dn, d + p, bl);
                dn += bl;
                p += bl;
            }
            uint16_t *prefix = malloc(4096 * sizeof(uint16_t));
            uint8_t *suffix = malloc(4096), *stack = malloc(4097);
            if (!data || !prefix || !suffix || !stack) {
                free(data); free(prefix); free(suffix); free(stack);
                lm_img_free(img);
                fail(err, errsz, "out of memory");
                return -1;
            }
            int clear = 1 << minbits, eoi = clear + 1, next = clear + 2, size = minbits + 1, old = -1;
            uint8_t first = 0;
            uint32_t acc = 0;
            int nb = 0;
            size_t ip = 0;
            uint64_t pixel = 0, total = (uint64_t)iw * ih;
            int pass = 0, row = 0, step = 8;
            uint32_t x = 0;
            while (pixel < total) {
                while (nb < size && ip < dn) {
                    acc |= (uint32_t)data[ip++] << nb;
                    nb += 8;
                }
                if (nb < size)
                    break;
                int code = (int)(acc & ((1u << size) - 1));
                acc >>= size;
                nb -= size;
                if (code == clear) {
                    next = clear + 2;
                    size = minbits + 1;
                    old = -1;
                    continue;
                }
                if (code == eoi)
                    break;
                int sp = 0, cur = code;
                if (old < 0) {
                    if (code >= clear)
                        break;
                    stack[sp++] = (uint8_t)code;
                    first = (uint8_t)code;
                } else {
                    if (code > next || (code == next && next >= 4096))
                        break;
                    if (code == next) {
                        stack[sp++] = first;
                        cur = old;
                    }
                    while (cur >= clear) {
                        if (sp >= 4096 || cur >= 4096)
                            break;
                        stack[sp++] = suffix[cur];
                        cur = prefix[cur];
                    }
                    stack[sp++] = (uint8_t)cur;
                    first = (uint8_t)cur;
                    if (next < 4096) {
                        prefix[next] = (uint16_t)old;
                        suffix[next] = first;
                        next++;
                        if (next == (1 << size) && size < 12)
                            size++;
                    }
                }
                old = code;
                while (sp > 0 && pixel < total) {
                    int idx = stack[--sp];
                    uint32_t yy = (uint32_t)row + top, xx = x + left;
                    if (yy < sh && xx < sw) {
                        uint32_t col;
                        if (idx == transparent || idx >= pn)
                            col = over_checker((int)xx, (int)yy, 0, 0, 0, 0);
                        else
                            col = over_checker((int)xx, (int)yy, pal[idx * 3], pal[idx * 3 + 1], pal[idx * 3 + 2], 255);
                        img->px[(size_t)yy * sw + xx] = col;
                    }
                    pixel++;
                    if (++x == iw) {
                        x = 0;
                        if (f & 0x40) {                                 /* interlaced: rows 0, 8, 16 ... then 4, 12 ... then 2, 6 ... then 1, 3 ... */
                            row += step;
                            while ((uint32_t)row >= ih && pass < 3) {
                                pass++;
                                row = pass == 1 ? 4 : pass == 2 ? 2 : 1;
                                step = pass == 1 ? 8 : pass == 2 ? 4 : 2;
                            }
                        } else {
                            row++;
                        }
                    }
                }
            }
            free(data); free(prefix); free(suffix); free(stack);
            return 0;
        } else if (b == 0x3b) {
            break;
        } else {
            break;
        }
    }
    fail(err, errsz, "GIF: no picture in the file");
    return -1;
}

/* A BMP / DIB: `d` points at the info header (the file header, if any, is the caller's), `data` at the pixels. `ico`: the height counts the XOR picture and the AND mask. */
static int decode_dib(const uint8_t *d, size_t len, size_t data_off, bool ico, struct lm_image *img, char *err, size_t errsz)
{
    if (len < 12) {
        fail(err, errsz, "BMP: too short");
        return -1;
    }
    uint32_t hs = le32(d);
    int32_t w, h;
    unsigned bpp, comp = 0, used = 0;
    size_t pal_entry = 4;
    if (hs == 12) {                                                     /* OS/2 */
        w = (int16_t)le16(d + 4);
        h = (int16_t)le16(d + 6);
        bpp = le16(d + 10);
        pal_entry = 3;
    } else if (hs >= 40 && hs <= 256 && len >= hs) {
        w = (int32_t)le32(d + 4);
        h = (int32_t)le32(d + 8);
        bpp = le16(d + 14);
        comp = le32(d + 16);
        used = le32(d + 32);
    } else {
        fail(err, errsz, "BMP: an unknown header");
        return -1;
    }
    bool topdown = h < 0;
    uint32_t aw = (uint32_t)(w < 0 ? -w : w), ah = (uint32_t)(h < 0 ? -h : h);
    if (ico)
        ah /= 2;
    if (aw == 0 || ah == 0 || aw > 65535 || ah > 65535 || (uint64_t)aw * ah > IMG_MAX_PIXELS) {
        fail(err, errsz, "BMP: a size that is not supported");
        return -1;
    }
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) {
        fail(err, errsz, "BMP: a colour depth that is not supported");
        return -1;
    }
    if (comp > 3 && comp != 6) {
        fail(err, errsz, "BMP: a compression that is not supported (JPEG / PNG inside)");
        return -1;
    }
    uint32_t masks[4] = { 0, 0, 0, 0 };
    size_t pal_off = hs;
    if (comp == 3 || comp == 6) {
        if (hs >= 52) {
            for (int i = 0; i < 3; i++)
                masks[i] = le32(d + 40 + 4 * i);
            if (hs >= 56)
                masks[3] = le32(d + 52);
        } else if (len >= hs + 12) {
            for (int i = 0; i < 3; i++)
                masks[i] = le32(d + hs + 4 * i);
            pal_off = hs + 12;
        }
    } else if (bpp == 16) {
        masks[0] = 0x7c00; masks[1] = 0x3e0; masks[2] = 0x1f;
    } else if (bpp == 32) {
        masks[0] = 0xff0000; masks[1] = 0xff00; masks[2] = 0xff;
    }
    uint32_t ncol = bpp <= 8 ? (used ? used : 1u << bpp) : 0;
    if (ncol > 256)
        ncol = 256;
    uint32_t pal[256];
    for (uint32_t i = 0; i < ncol; i++) {
        size_t o = pal_off + i * pal_entry;
        pal[i] = o + 3 <= len ? (uint32_t)d[o + 2] << 16 | (uint32_t)d[o + 1] << 8 | d[o] : 0;
    }
    if (data_off == 0)
        data_off = pal_off + ncol * pal_entry;                          /* (a DIB in an ICO: the pixels follow the palette) */
    if (data_off > len) {
        fail(err, errsz, "BMP: the pixels are missing");
        return -1;
    }
    const uint8_t *px = d + data_off;
    size_t pxlen = len - data_off;
    if (!img_alloc(img, aw, ah, err, errsz))
        return -1;
    uint8_t *idxbuf = NULL;                                             /* RLE: the indices, row by row */
    if (comp == 1 || comp == 2) {
        idxbuf = calloc((size_t)aw, ah);
        if (!idxbuf) {
            lm_img_free(img);
            fail(err, errsz, "out of memory");
            return -1;
        }
        size_t i = 0;
        uint32_t x = 0, y = 0;                                          /* y counts from the bottom of a bottom-up picture */
        while (i + 1 < pxlen && y < ah) {
            unsigned n = px[i++], v = px[i++];
            if (n) {
                for (unsigned k = 0; k < n && x < aw; k++, x++)
                    idxbuf[(size_t)y * aw + x] = (uint8_t)(comp == 1 ? v : (k & 1 ? v & 15 : v >> 4));
            } else if (v == 0) {
                x = 0;
                y++;
            } else if (v == 1) {
                break;
            } else if (v == 2) {
                if (i + 1 >= pxlen)
                    break;
                x += px[i++];
                y += px[i++];
            } else {
                unsigned cnt = v;
                size_t bytes = comp == 1 ? cnt : (cnt + 1) / 2;
                for (unsigned k = 0; k < cnt && i + (comp == 1 ? k : k / 2) < pxlen && x < aw; k++, x++) {
                    unsigned b = px[i + (comp == 1 ? k : k / 2)];
                    idxbuf[(size_t)y * aw + x] = (uint8_t)(comp == 1 ? b : (k & 1 ? b & 15 : b >> 4));
                }
                i += (bytes + 1) & ~1u;
            }
        }
    }
    size_t stride = ((size_t)aw * bpp + 31) / 32 * 4;
    int sh[4] = { 0 }, nbits[4] = { 8, 8, 8, 0 };
    for (int c = 0; c < 4; c++) {
        uint32_t m = masks[c];
        if (m) {
            while (!(m & 1)) {
                m >>= 1;
                sh[c]++;
            }
            nbits[c] = 0;
            while (m & 1) {
                m >>= 1;
                nbits[c]++;
            }
        }
    }
    bool has_alpha = masks[3] != 0;
    for (uint32_t y = 0; y < ah; y++) {
        uint32_t sy = topdown ? y : ah - 1 - y;
        const uint8_t *row = px + (size_t)sy * stride;
        bool in = comp == 1 || comp == 2 ? true : (size_t)(sy + 1) * stride <= pxlen;
        for (uint32_t x = 0; x < aw; x++) {
            int r = 0, g = 0, b = 0, a = 255;
            if (comp == 1 || comp == 2) {
                uint32_t c = pal[idxbuf[(size_t)(topdown ? ah - 1 - y : sy) * aw + x] % (ncol ? ncol : 1)];
                r = (int)(c >> 16) & 255;
                g = (int)(c >> 8) & 255;
                b = (int)c & 255;
            } else if (!in) {
                r = g = b = 0;
            } else if (bpp <= 8) {
                unsigned idx = bpp == 8 ? row[x] : bpp == 4 ? (row[x / 2] >> (x & 1 ? 0 : 4)) & 15 : (row[x / 8] >> (7 - (x & 7))) & 1;
                uint32_t c = idx < ncol ? pal[idx] : 0;
                r = (int)(c >> 16) & 255;
                g = (int)(c >> 8) & 255;
                b = (int)c & 255;
            } else if (bpp == 24) {
                b = row[x * 3];
                g = row[x * 3 + 1];
                r = row[x * 3 + 2];
            } else {
                uint32_t v = bpp == 16 ? le16(row + x * 2) : le32(row + x * 4);
                uint32_t comps[4];
                for (int c = 0; c < 4; c++) {
                    if (!masks[c]) {
                        comps[c] = c == 3 ? 255 : 0;
                        continue;
                    }
                    uint32_t m = (v >> sh[c]) & ((1u << nbits[c]) - 1);
                    comps[c] = nbits[c] >= 8 ? m >> (nbits[c] - 8) : m * 255 / ((1u << nbits[c]) - 1);
                }
                r = (int)comps[0];
                g = (int)comps[1];
                b = (int)comps[2];
                a = has_alpha ? (int)comps[3] : 255;
            }
            img->px[(size_t)y * aw + x] = over_checker((int)x, (int)y, r, g, b, a);
        }
    }
    free(idxbuf);
    if (ico) {                                                          /* transparency of an icon: the alpha bytes of a 32 bit picture, else the AND mask (1 = transparent) */
        size_t mstride = ((size_t)aw + 31) / 32 * 4, moff = data_off + stride * ah;
        bool have_mask = moff + mstride * ah <= len;
        bool alpha32 = false;
        if (bpp == 32 && comp == 0)
            for (uint32_t y = 0; y < ah && !alpha32; y++)
                for (uint32_t x = 0; x < aw; x++)
                    if (data_off + (size_t)y * stride + x * 4 + 3 < len && d[data_off + (size_t)y * stride + x * 4 + 3]) {
                        alpha32 = true;
                        break;
                    }
        for (uint32_t y = 0; y < ah; y++) {
            uint32_t sy = topdown ? y : ah - 1 - y;
            for (uint32_t x = 0; x < aw; x++) {
                if (alpha32) {
                    const uint8_t *q = d + data_off + (size_t)sy * stride + x * 4;
                    if (q + 4 <= d + len)
                        img->px[(size_t)y * aw + x] = over_checker((int)x, (int)y, q[2], q[1], q[0], q[3]);
                } else if (have_mask && !(has_alpha) && ((d[moff + sy * mstride + x / 8] >> (7 - (x & 7))) & 1)) {
                    img->px[(size_t)y * aw + x] = over_checker((int)x, (int)y, 0, 0, 0, 0);
                }
            }
        }
    }
    return 0;
}

static int decode_bmp(const uint8_t *d, size_t len, struct lm_image *img, char *err, size_t errsz)
{
    if (len < 26) {
        fail(err, errsz, "BMP: too short");
        return -1;
    }
    size_t off = le32(d + 10);
    return decode_dib(d + 14, len - 14, off >= 14 && off <= len ? off - 14 : 0, false, img, err, errsz);
}

/* ICO / CUR: the biggest picture of the file (PNG or DIB inside). */
static int decode_ico(const uint8_t *d, size_t len, struct lm_image *img, char *err, size_t errsz)
{
    unsigned count = len >= 6 ? le16(d + 4) : 0;
    if (count == 0 || 6 + (size_t)count * 16 > len) {
        fail(err, errsz, "ICO: no pictures");
        return -1;
    }
    int best = -1;
    uint64_t bestscore = 0;
    for (unsigned i = 0; i < count; i++) {
        const uint8_t *e = d + 6 + i * 16;
        uint32_t w = e[0] ? e[0] : 256, h = e[1] ? e[1] : 256, size = le32(e + 8), off = le32(e + 12);
        if (off >= len || size > len - off || size < 8)
            continue;
        uint64_t score = (uint64_t)w * h * 64 + (d[0] == 0 ? le16(e + 6) : 0);
        if (best < 0 || score > bestscore) {
            best = (int)i;
            bestscore = score;
        }
    }
    if (best < 0) {
        fail(err, errsz, "ICO: damaged");
        return -1;
    }
    const uint8_t *e = d + 6 + best * 16;
    uint32_t size = le32(e + 8), off = le32(e + 12);
    if (size >= 8 && !memcmp(d + off, "\x89PNG\r\n\x1a\n", 8))
        return decode_png(d + off, size, img, err, errsz);
    return decode_dib(d + off, size, 0, true, img, err, errsz);
}

/* PNM P1..P5 (bits / grey, text or binary); P6 is decode_ppm's. */
static int decode_pnm(const uint8_t *d, size_t len, struct lm_image *img, char *err, size_t errsz)
{
    int kind = d[1] - '0';
    size_t p = 2;
    long vals[3];
    for (int i = 0; i < (kind == 1 || kind == 4 ? 2 : 3); i++) {
        while (p < len && (d[p] == ' ' || d[p] == '\t' || d[p] == '\n' || d[p] == '\r' || d[p] == '#')) {
            if (d[p] == '#')
                while (p < len && d[p] != '\n')
                    p++;
            else
                p++;
        }
        long v = 0;
        size_t q = p;
        while (p < len && d[p] >= '0' && d[p] <= '9' && v < 100000000)
            v = v * 10 + (d[p++] - '0');
        if (q == p) {
            fail(err, errsz, "PNM: a bad header");
            return -1;
        }
        vals[i] = v;
    }
    uint32_t w = (uint32_t)vals[0], h = (uint32_t)vals[1];
    long maxv = kind == 1 || kind == 4 ? 1 : vals[2];
    if (maxv < 1 || maxv > 65535 || !img_alloc(img, w, h, err, errsz))
        return -1;
    if (kind == 4 || kind == 5)
        p++;                                                            /* one blank after the header */
    size_t bytes_per = maxv > 255 ? 2 : 1;
    size_t bstride = (w + 7) / 8;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            long v = 0;
            if (kind == 4) {
                size_t o = p + (size_t)y * bstride + x / 8;
                v = o < len ? !((d[o] >> (7 - (x & 7))) & 1) : 1;      /* 1 = black */
                v *= 255;
            } else if (kind == 5) {
                size_t o = p + ((size_t)y * w + x) * bytes_per;
                if (o + bytes_per <= len)
                    v = bytes_per == 2 ? (long)(d[o] << 8 | d[o + 1]) * 255 / maxv : d[o] * 255 / maxv;
            } else {                                                    /* text */
                while (p < len && (d[p] == ' ' || d[p] == '\t' || d[p] == '\n' || d[p] == '\r'))
                    p++;
                long n = 0;
                bool got = false;
                while (p < len && d[p] >= '0' && d[p] <= '9') {
                    if (kind == 1) {                                    /* P1: the digits need no blanks between them */
                        n = d[p++] - '0';
                        got = true;
                        break;
                    }
                    n = n * 10 + (d[p++] - '0');
                    got = true;
                }
                if (!got)
                    n = 0;
                v = kind == 1 ? (n ? 0 : 255) : n * 255 / maxv;
            }
            v = v < 0 ? 0 : v > 255 ? 255 : v;
            img->px[(size_t)y * w + x] = (uint32_t)v << 16 | (uint32_t)v << 8 | (uint32_t)v;
        }
    return 0;
}


/* ---- TIFF (the first picture: strips or tiles; none, LZW, Deflate, PackBits; 1 .. 16 bits; grey, palette, RGB, alpha) and TGA ----------------------------- */

struct tiff {
    const uint8_t *d;
    size_t len;
    bool big;
};

static uint32_t tf16(const struct tiff *t, const uint8_t *p)
{
    return t->big ? (uint32_t)(p[0] << 8 | p[1]) : (uint32_t)(p[0] | p[1] << 8);
}

static uint32_t tf32(const struct tiff *t, const uint8_t *p)
{
    return t->big ? (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3] : le32(p);
}

/* The values of a tag of the IFD: up to `max` numbers into out; returns the count (0 when absent). */
static unsigned tiff_tag(const struct tiff *t, uint32_t ifd, unsigned tag, uint32_t *out, unsigned max)
{
    if (ifd + 2 > t->len)
        return 0;
    unsigned n = tf16(t, t->d + ifd);
    for (unsigned i = 0; i < n; i++) {
        size_t e = (size_t)ifd + 2 + (size_t)i * 12;
        if (e + 12 > t->len)
            return 0;
        if (tf16(t, t->d + e) != tag)
            continue;
        unsigned type = tf16(t, t->d + e + 2), cnt = tf32(t, t->d + e + 4);
        size_t tsz = type == 3 ? 2 : type == 4 ? 4 : type == 1 || type == 2 || type == 6 || type == 7 ? 1 : type == 8 ? 2 : type == 9 ? 4 : 0;
        if (!tsz)
            return 0;
        const uint8_t *v = t->d + e + 8;
        if ((uint64_t)cnt * tsz > 4) {
            uint32_t off = tf32(t, v);
            if ((uint64_t)off + (uint64_t)cnt * tsz > t->len)
                return 0;
            v = t->d + off;
        }
        unsigned m = cnt < max ? cnt : max;
        for (unsigned k = 0; k < m; k++)
            out[k] = tsz == 1 ? v[k] : tsz == 2 ? tf16(t, v + 2 * k) : tf32(t, v + 4 * k);
        return m;
    }
    return 0;
}

/* TIFF's LZW: codes of 9 to 12 bits, most significant bit first, the width grows one code early. */
static long tiff_lzw(const uint8_t *in, size_t n, uint8_t *out, size_t outlen)
{
    uint16_t *prefix = malloc(4096 * sizeof(uint16_t));
    uint8_t *suffix = malloc(4096), *stack = malloc(4097);
    if (!prefix || !suffix || !stack) {
        free(prefix); free(suffix); free(stack);
        return -1;
    }
    size_t w = 0, ip = 0;
    int next = 258, bits = 9, old = -1;
    uint32_t acc = 0;
    int nb = 0;
    while (true) {
        while (nb < bits && ip < n) {
            acc = (acc << 8) | in[ip++];
            nb += 8;
        }
        if (nb < bits)
            break;
        int code = (int)((acc >> (nb - bits)) & ((1u << bits) - 1));
        nb -= bits;
        if (code == 257)
            break;
        if (code == 256) {
            next = 258;
            bits = 9;
            old = -1;
            continue;
        }
        int sp = 0, cur = code;
        uint8_t first;
        if (old < 0) {
            if (code > 255)
                break;
            stack[sp++] = (uint8_t)code;
            first = (uint8_t)code;
        } else {
            if (code > next)
                break;
            if (code == next) {
                cur = old;
                int sp0 = 0;
                uint8_t tmp[4097];
                while (cur > 255 && sp0 < 4096) {
                    tmp[sp0++] = suffix[cur];
                    cur = prefix[cur];
                }
                first = (uint8_t)cur;
                uint8_t *o = out + w;
                if (w + (size_t)sp0 + 2 > outlen) {
                    if (w < outlen) {
                        size_t room = outlen - w, k = 0;
                        if (k < room) o[k++] = first;
                        for (int j = sp0 - 1; j >= 0 && k < room; j--) o[k++] = tmp[j];
                        if (k < room) o[k++] = first;
                        w += k;
                    }
                    break;
                }
                *o++ = first;
                for (int j = sp0 - 1; j >= 0; j--)
                    *o++ = tmp[j];
                *o++ = first;
                w = (size_t)(o - out);
                if (next < 4096) {
                    prefix[next] = (uint16_t)old;
                    suffix[next] = first;
                    next++;
                }
                old = code;
                if (next + 1 >= (1 << bits) && bits < 12)
                    bits++;
                continue;
            }
            while (cur > 255 && sp < 4096) {
                stack[sp++] = suffix[cur];
                cur = prefix[cur];
            }
            stack[sp++] = (uint8_t)cur;
            first = (uint8_t)cur;
            if (next < 4096) {
                prefix[next] = (uint16_t)old;
                suffix[next] = first;
                next++;
            }
        }
        if (w + (size_t)sp > outlen) {
            while (sp > 0 && w < outlen)
                out[w++] = stack[--sp];
            break;
        }
        while (sp > 0)
            out[w++] = stack[--sp];
        old = code;
        if (next + 1 >= (1 << bits) && bits < 12)
            bits++;
    }
    free(prefix);
    free(suffix);
    free(stack);
    return (long)w;
}

static long packbits(const uint8_t *in, size_t n, uint8_t *out, size_t outlen)
{
    size_t w = 0;
    for (size_t i = 0; i < n && w < outlen;) {
        int l = (int8_t)in[i++];
        if (l >= 0) {
            size_t c = (size_t)l + 1;
            if (c > n - i)
                c = n - i;
            if (c > outlen - w)
                c = outlen - w;
            memcpy(out + w, in + i, c);
            w += c;
            i += (size_t)l + 1 > n - i ? n - i : (size_t)l + 1;
        } else if (l != -128 && i < n) {
            size_t c = (size_t)(1 - l);
            if (c > outlen - w)
                c = outlen - w;
            memset(out + w, in[i++], c);
            w += c;
        }
    }
    return (long)w;
}

static int decode_tiff(const uint8_t *d, size_t len, struct lm_image *img, char *err, size_t errsz)
{
    struct tiff t = { d, len, d[0] == 'M' };
    if (len < 8) {
        fail(err, errsz, "TIFF: too short");
        return -1;
    }
    uint32_t ifd = tf32(&t, d + 4), v[4], w = 0, h = 0, comp = 1, photo = 1, spp = 1, planar = 1, pred = 1, rps = 0xffffffffu, fill = 1, tw = 0, tl = 0;
    uint32_t bps[8] = { 1 }, extra[4] = { 0 };
    if (!tiff_tag(&t, ifd, 256, v, 1)) {
        fail(err, errsz, "TIFF: no width");
        return -1;
    }
    w = v[0];
    if (!tiff_tag(&t, ifd, 257, v, 1)) {
        fail(err, errsz, "TIFF: no height");
        return -1;
    }
    h = v[0];
    unsigned nbps = tiff_tag(&t, ifd, 258, bps, 8);
    if (!nbps) {
        nbps = 1;
        bps[0] = 1;
    }
    if (tiff_tag(&t, ifd, 259, v, 1)) comp = v[0];
    if (tiff_tag(&t, ifd, 262, v, 1)) photo = v[0];
    if (tiff_tag(&t, ifd, 277, v, 1)) spp = v[0];
    if (tiff_tag(&t, ifd, 284, v, 1)) planar = v[0];
    if (tiff_tag(&t, ifd, 317, v, 1)) pred = v[0];
    if (tiff_tag(&t, ifd, 278, v, 1)) rps = v[0];
    if (tiff_tag(&t, ifd, 266, v, 1)) fill = v[0];
    (void)fill;
    unsigned nextra = tiff_tag(&t, ifd, 338, extra, 4);
    bool tiled = tiff_tag(&t, ifd, 322, v, 1) && (tw = v[0]) && tiff_tag(&t, ifd, 323, v, 1) && (tl = v[0]);
    unsigned bpc = bps[0];
    for (unsigned i = 1; i < nbps && i < spp; i++)
        if (bps[i] != bpc) {
            fail(err, errsz, "TIFF: samples of different sizes are not supported");
            return -1;
        }
    if (planar != 1 || spp < 1 || spp > 4 || (bpc != 1 && bpc != 2 && bpc != 4 && bpc != 8 && bpc != 16) || (photo > 3 && photo != 2) || photo == 4 || (photo == 3 && spp != 1)) {
        fail(err, errsz, "TIFF: a colour layout that is not supported");
        return -1;
    }
    if (comp != 1 && comp != 5 && comp != 8 && comp != 32946 && comp != 32773) {
        fail(err, errsz, "TIFF: a compression that is not supported (JPEG, CCITT ...)");
        return -1;
    }
    if (w < 1 || h < 1 || w > 65535 || h > 65535 || (uint64_t)w * h > IMG_MAX_PIXELS || (photo == 2 && spp < 3)) {
        fail(err, errsz, "TIFF: a size that is not supported");
        return -1;
    }
    size_t rowbits = (size_t)w * spp * bpc, rowb = (rowbits + 7) / 8;
    uint8_t *raw = calloc(rowb * h + 1, 1);
    if (!raw) {
        fail(err, errsz, "out of memory");
        return -1;
    }
    uint32_t *offs = NULL, *cnts = NULL;
    unsigned nstrip = 0;
    uint32_t tpr = 0, tpc = 0;
    if (tiled) {
        tpr = (w + tw - 1) / tw;
        tpc = (h + tl - 1) / tl;
        nstrip = tpr * tpc;
    } else {
        if (rps == 0 || rps > h)
            rps = h;
        nstrip = (h + rps - 1) / rps;
    }
    if (nstrip == 0 || nstrip > 1000000) {
        free(raw);
        fail(err, errsz, "TIFF: no strips");
        return -1;
    }
    offs = calloc(nstrip, sizeof(uint32_t));
    cnts = calloc(nstrip, sizeof(uint32_t));
    unsigned no = tiff_tag(&t, ifd, tiled ? 324 : 273, offs, nstrip), nc = tiff_tag(&t, ifd, tiled ? 325 : 279, cnts, nstrip);
    int rc = 0;
    if (no != nstrip) {
        rc = -1;
        fail(err, errsz, "TIFF: the strip offsets are missing");
    }
    for (unsigned i = 0; rc == 0 && i < nstrip; i++) {
        size_t off = offs[i], cnt = i < nc ? cnts[i] : (len > off ? len - off : 0);
        if (off > len || cnt > len - off)
            cnt = off > len ? 0 : len - off;
        uint32_t rows, cols_bytes;
        size_t y0, x0b = 0;
        if (tiled) {
            rows = tl;
            cols_bytes = (uint32_t)(((size_t)tw * spp * bpc + 7) / 8);
            y0 = (size_t)(i / tpr) * tl;
            x0b = (size_t)(i % tpr) * (((size_t)tw * spp * bpc + 7) / 8);
        } else {
            y0 = (size_t)i * rps;
            rows = (uint32_t)(y0 + rps <= h ? rps : h - y0);
            cols_bytes = (uint32_t)rowb;
        }
        size_t need = (size_t)rows * cols_bytes;
        uint8_t *buf = malloc(need + 1);
        if (!buf) {
            rc = -1;
            break;
        }
        long got = 0;
        if (comp == 1) {
            got = (long)(cnt < need ? cnt : need);
            memcpy(buf, d + off, (size_t)got);
        } else if (comp == 5) {
            got = tiff_lzw(d + off, cnt, buf, need);
        } else if (comp == 32773) {
            got = packbits(d + off, cnt, buf, need);
        } else {
            got = zlib_inflate(d + off, cnt, buf, need) == 0 ? (long)need : -1;
        }
        if (got < 0) {
            free(buf);
            rc = -1;
            fail(err, errsz, "TIFF: a strip does not unpack");
            break;
        }
        if ((size_t)got < need)
            memset(buf + got, 0, need - (size_t)got);
        if (pred == 2 && (bpc == 8 || bpc == 16))                       /* horizontal differencing */
            for (uint32_t r = 0; r < rows; r++) {
                uint8_t *row = buf + (size_t)r * cols_bytes;
                size_t bytes = bpc == 8 ? 1 : 2;
                size_t pixels = cols_bytes / (spp * bytes);
                for (size_t x = 1; x < pixels; x++)
                    for (unsigned c = 0; c < spp; c++) {
                        uint8_t *p = row + (x * spp + c) * bytes, *q = row + ((x - 1) * spp + c) * bytes;
                        if (bytes == 1)
                            p[0] = (uint8_t)(p[0] + q[0]);
                        else {
                            unsigned s = t.big ? (unsigned)(p[0] << 8 | p[1]) + (unsigned)(q[0] << 8 | q[1]) : (unsigned)(p[0] | p[1] << 8) + (unsigned)(q[0] | q[1] << 8);
                            if (t.big) { p[0] = (uint8_t)(s >> 8); p[1] = (uint8_t)s; } else { p[0] = (uint8_t)s; p[1] = (uint8_t)(s >> 8); }
                        }
                    }
            }
        for (uint32_t r = 0; r < rows && y0 + r < h; r++) {
            size_t take = tiled ? (x0b + cols_bytes <= rowb ? cols_bytes : (rowb > x0b ? rowb - x0b : 0)) : rowb;
            memcpy(raw + (y0 + r) * rowb + x0b, buf + (size_t)r * cols_bytes, take);
        }
        free(buf);
    }
    free(offs);
    free(cnts);
    if (rc) {
        free(raw);
        return -1;
    }
    uint32_t cmap[3 * 256], ncmap = 0;
    uint32_t *cm = NULL;
    if (photo == 3) {
        size_t cn = (size_t)3 << bpc;
        cm = calloc(cn, sizeof(uint32_t));
        if (cm)
            ncmap = tiff_tag(&t, ifd, 320, cm, (unsigned)cn);
        (void)cmap;
    }
    if (!img_alloc(img, w, h, err, errsz)) {
        free(raw);
        free(cm);
        return -1;
    }
    bool alpha = (spp == 2 && photo != 2 && photo != 3) || (spp == 4 && photo == 2) || (spp == 2 && photo == 3) || (nextra > 0 && spp > (photo == 2 ? 3u : 1u));
    bool assoc = nextra > 0 && extra[0] == 1;
    unsigned maxv = (1u << (bpc > 8 ? 8 : bpc)) - 1;
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *row = raw + (size_t)y * rowb;
        for (uint32_t x = 0; x < w; x++) {
            unsigned s[4] = { 0 };
            for (unsigned c = 0; c < spp; c++) {
                size_t idx = (size_t)x * spp + c;
                if (bpc == 8) s[c] = row[idx];
                else if (bpc == 16) s[c] = t.big ? row[idx * 2] : row[idx * 2 + 1];
                else {
                    size_t bit = idx * bpc;
                    s[c] = (row[bit / 8] >> (8 - bpc - bit % 8)) & maxv;
                }
            }
            int r, g, b, a = 255;
            if (photo == 2) {
                r = (int)s[0]; g = (int)s[1]; b = (int)s[2];
                if (alpha && spp >= 4) a = (int)s[3];
            } else if (photo == 3) {
                unsigned idx = s[0];
                size_t cn = (size_t)1 << bpc;
                r = ncmap >= 3 * cn ? (int)(cm[idx] >> 8) : (int)idx;
                g = ncmap >= 3 * cn ? (int)(cm[cn + idx] >> 8) : (int)idx;
                b = ncmap >= 3 * cn ? (int)(cm[2 * cn + idx] >> 8) : (int)idx;
            } else {
                unsigned gv = s[0];
                if (photo == 0)
                    gv = maxv - gv;
                r = g = b = bpc >= 8 ? (int)gv : (int)(gv * 255 / maxv);
                if (alpha && spp >= 2) a = (int)s[1];
            }
            if (bpc < 8 && photo == 2) {
                r = r * 255 / (int)maxv; g = g * 255 / (int)maxv; b = b * 255 / (int)maxv;
            }
            if (a < 255 && assoc && a > 0) {
                r = r * 255 / a > 255 ? 255 : r * 255 / a;
                g = g * 255 / a > 255 ? 255 : g * 255 / a;
                b = b * 255 / a > 255 ? 255 : b * 255 / a;
            }
            img->px[(size_t)y * w + x] = over_checker((int)x, (int)y, r, g, b, a);
        }
    }
    free(raw);
    free(cm);
    return 0;
}

/* TGA: has no magic number, so the header has to make sense and (for the uncompressed kinds) the size has to fit. */
static bool tga_plausible(const uint8_t *d, size_t len)
{
    if (len < 18 + 1)
        return false;
    unsigned idlen = d[0], cmt = d[1], it = d[2], cmlen = le16(d + 5), cmbits = d[7], w = le16(d + 12), h = le16(d + 14), bpp = d[16];
    if (cmt > 1 || !(it == 1 || it == 2 || it == 3 || it == 9 || it == 10 || it == 11) || w == 0 || h == 0 || (d[17] & 0xc0))
        return false;
    if (!(bpp == 8 || bpp == 15 || bpp == 16 || bpp == 24 || bpp == 32))
        return false;
    if ((it == 1 || it == 9) != (cmt == 1) && !(cmt == 1 && (it == 2 || it == 3 || it == 10 || it == 11)))
        return false;
    if (cmt == 1 && !(cmbits == 15 || cmbits == 16 || cmbits == 24 || cmbits == 32))
        return false;
    size_t cmsz = cmt ? (size_t)cmlen * ((cmbits + 7) / 8) : 0, hdr = 18 + idlen + cmsz;
    if (hdr > len)
        return false;
    bool footer = len >= 26 && !memcmp(d + len - 18, "TRUEVISION-XFILE", 16);
    if (it == 1 || it == 2 || it == 3) {
        size_t data = (size_t)w * h * ((bpp + 7) / 8);
        return hdr + data <= len && (footer || len - hdr - data <= 2048);
    }
    return footer || len > hdr + 16;
}

static int decode_tga(const uint8_t *d, size_t len, struct lm_image *img, char *err, size_t errsz)
{
    unsigned idlen = d[0], cmt = d[1], it = d[2], cmstart = le16(d + 3), cmlen = le16(d + 5), cmbits = d[7], w = le16(d + 12), h = le16(d + 14), bpp = d[16], desc = d[17];
    bool rle = it >= 9, gray = (it & 7) == 3, mapped = (it & 7) == 1;
    size_t cmbytes = (cmbits + 7) / 8, pos = 18 + idlen;
    uint32_t pal[256] = { 0 };
    if (cmt) {
        for (unsigned i = 0; i < cmlen; i++) {
            size_t o = pos + (size_t)i * cmbytes;
            if (o + cmbytes > len)
                break;
            uint32_t c;
            if (cmbytes == 2) {
                unsigned v = le16(d + o);
                c = ((v >> 10 & 31) * 255 / 31) << 16 | ((v >> 5 & 31) * 255 / 31) << 8 | ((v & 31) * 255 / 31);
            } else {
                c = (uint32_t)d[o + 2] << 16 | (uint32_t)d[o + 1] << 8 | d[o];
            }
            if (cmstart + i < 256)
                pal[cmstart + i] = c;
        }
        pos += (size_t)cmlen * cmbytes;
    }
    if (!img_alloc(img, w, h, err, errsz))
        return -1;
    size_t px_bytes = (bpp + 7) / 8, total = (size_t)w * h;
    bool alpha_bits = (desc & 15) > 0 && bpp == 32;
    size_t n = 0;
    uint32_t run_px[1];
    (void)run_px;
    while (n < total && pos < len) {
        size_t count = 1;
        bool run = false;
        if (rle) {
            unsigned hdr = d[pos++];
            count = (hdr & 127) + 1;
            run = hdr & 128;
        }
        const uint8_t *first = d + pos;
        if (pos + px_bytes > len)
            break;
        for (size_t k = 0; k < count && n < total; k++) {
            const uint8_t *p = run ? first : d + pos + k * px_bytes;
            if (!run && p + px_bytes > d + len)
                goto done;
            int r, g, b, a = 255;
            if (mapped) {
                uint32_t c = pal[p[0]];
                r = (int)(c >> 16 & 255); g = (int)(c >> 8 & 255); b = (int)(c & 255);
            } else if (gray) {
                r = g = b = p[0];
                if (bpp == 16)
                    a = p[1];
            } else if (bpp == 15 || bpp == 16) {
                unsigned v = le16(p);
                r = (int)((v >> 10 & 31) * 255 / 31);
                g = (int)((v >> 5 & 31) * 255 / 31);
                b = (int)((v & 31) * 255 / 31);
            } else {
                b = p[0]; g = p[1]; r = p[2];
                if (bpp == 32 && alpha_bits)
                    a = p[3];
            }
            size_t idx = n++;
            uint32_t x = (uint32_t)(idx % w), y = (uint32_t)(idx / w);
            if (!(desc & 0x20))
                y = h - 1 - y;
            if (desc & 0x10)
                x = w - 1 - x;
            img->px[(size_t)y * w + x] = over_checker((int)x, (int)y, r, g, b, a);
        }
        pos += run ? px_bytes : count * px_bytes;
    }
done:
    return 0;
}

int lm_img_decode(const uint8_t *data, size_t len, struct lm_image *img, char *err, size_t errsz)
{
    img->px = NULL;
    if (len >= 8 && !memcmp(data, "\x89PNG\r\n\x1a\n", 8))
        return decode_png(data, len, img, err, errsz);
    if (len >= 3 && data[0] == 0xff && data[1] == 0xd8 && data[2] == 0xff)
        return lm_jpeg_decode(data, len, img, err, errsz);
    if (len >= 2 && data[0] == 'P' && data[1] == '6')
        return decode_ppm(data, len, img, err, errsz);
    if (len >= 3 && data[0] == 'P' && data[1] >= '1' && data[1] <= '5' && (data[2] == ' ' || data[2] == '\n' || data[2] == '\r' || data[2] == '\t' || data[2] == '#'))
        return decode_pnm(data, len, img, err, errsz);
    if (len >= 6 && !memcmp(data, "GIF8", 4) && (data[4] == '7' || data[4] == '9') && data[5] == 'a')
        return decode_gif(data, len, img, err, errsz);
    if (len >= 26 && data[0] == 'B' && data[1] == 'M')
        return decode_bmp(data, len, img, err, errsz);
    if (len >= 22 && data[0] == 0 && data[1] == 0 && (data[2] == 1 || data[2] == 2) && data[3] == 0 && !tga_plausible(data, len))      /* (a TGA of type 2 starts the same way) */
        return decode_ico(data, len, img, err, errsz);
    if (len >= 8 && ((data[0] == 'I' && data[1] == 'I' && data[2] == 42 && data[3] == 0) || (data[0] == 'M' && data[1] == 'M' && data[2] == 0 && data[3] == 42)))
        return decode_tiff(data, len, img, err, errsz);
    if (len >= 8 && ((data[0] == 'I' && data[1] == 'I' && data[2] == 43) || (data[0] == 'M' && data[1] == 'M' && data[3] == 43))) {
        fail(err, errsz, "BigTIFF: not supported");
        return -1;
    }
    if (tga_plausible(data, len))
        return decode_tga(data, len, img, err, errsz);
    if (lm_webp_sniff(data, len)) {
        int ww = 0, wh = 0;
        uint8_t *rgba = lm_webp_decode(data, len, &ww, &wh, err, errsz);
        if (!rgba)
            return -1;
        if (!img_alloc(img, (uint32_t)ww, (uint32_t)wh, err, errsz)) {
            free(rgba);
            return -1;
        }
        for (int y = 0; y < wh; y++)
            for (int x = 0; x < ww; x++) {
                const uint8_t *p = rgba + ((size_t)y * ww + x) * 4;
                img->px[(size_t)y * ww + x] = over_checker(x, y, p[0], p[1], p[2], p[3]);
            }
        free(rgba);
        return 0;
    }
    if (lm_svg_sniff(data, len)) {                                      /* SVG: drawn by svg.c (the checker shows through the transparent parts unless the alpha is kept) */
        int rc = lm_svg_render(data, len, img, err, errsz);
        if (rc == 0 && !g_keep_alpha)
            for (int y = 0; y < img->h; y++)
                for (int x = 0; x < img->w; x++) {
                    uint32_t p = img->px[(size_t)y * (size_t)img->w + (size_t)x];
                    img->px[(size_t)y * (size_t)img->w + (size_t)x] = over_checker(x, y, (int)((p >> 16) & 255), (int)((p >> 8) & 255), (int)(p & 255), (int)(p >> 24));
                }
        return rc;
    }
    fail(err, errsz, "not a PNG, JPEG, GIF, BMP, ICO, PNM, TIFF, TGA, WebP or SVG image");
    return -1;
}

int lm_img_load(const char *path, struct lm_image *img, char *err, size_t errsz)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errsz, "cannot open %s", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    if (n <= 0 || n > 512L * 1024 * 1024) {
        fclose(f);
        fail(err, errsz, "empty or huge file");
        return -1;
    }
    uint8_t *buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        fail(err, errsz, "cannot read the file");
        return -1;
    }
    fclose(f);
    int rc = lm_img_decode(buf, (size_t)n, img, err, errsz);
    free(buf);
    return rc;
}

int lm_img_load_alpha(const char *path, struct lm_image *img, char *err, size_t errsz)
{
    g_keep_alpha = 1;
    int rc = lm_img_load(path, img, err, errsz);
    g_keep_alpha = 0;
    if (rc == 0 && img->px) {                           /* JPEG and PPM have no alpha: make them opaque */
        size_t n = (size_t)img->w * (size_t)img->h;
        if (!(img->px[0] >> 24) && !(img->px[n - 1] >> 24)) {
            bool any = false;
            for (size_t i = 0; i < n && !any; i++)
                any = (img->px[i] >> 24) != 0;
            if (!any)
                for (size_t i = 0; i < n; i++)
                    img->px[i] |= 0xff000000u;
        }
    }
    return rc;
}

void lm_img_free(struct lm_image *img)
{
    free(img->px);
    img->px = NULL;
    img->w = img->h = 0;
}
