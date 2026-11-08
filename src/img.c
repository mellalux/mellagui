#include "img.h"

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
            if (s->outcnt == s->outlen)
                return -1;
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
            if (dist > s->outcnt || s->outcnt + (size_t)len > s->outlen)
                return -1;
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
    if (len != (~nlen & 0xffff) || s->incnt + len > s->inlen || s->outcnt + len > s->outlen)
        return -1;
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

int lm_img_decode(const uint8_t *data, size_t len, struct lm_image *img, char *err, size_t errsz)
{
    img->px = NULL;
    if (len >= 8 && !memcmp(data, "\x89PNG\r\n\x1a\n", 8))
        return decode_png(data, len, img, err, errsz);
    if (len >= 3 && data[0] == 0xff && data[1] == 0xd8 && data[2] == 0xff)
        return lm_jpeg_decode(data, len, img, err, errsz);
    if (len >= 2 && data[0] == 'P' && data[1] == '6')
        return decode_ppm(data, len, img, err, errsz);
    fail(err, errsz, "not a PNG, JPEG or PPM image");
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
