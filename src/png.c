#include "png.h"

#include <stdlib.h>
#include <string.h>

static uint32_t crc_table[256];
static int crc_ready;

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n)
{
    if (!crc_ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
        crc_ready = 1;
    }
    crc = ~crc;
    while (n--)
        crc = crc_table[(crc ^ *p++) & 255] ^ (crc >> 8);
    return ~crc;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* Writes length, type, data and CRC of one chunk at p (data already in place at p + 8); returns the bytes used. */
static size_t finish_chunk(uint8_t *p, const char *type, uint32_t dlen)
{
    put32(p, dlen);
    memcpy(p + 4, type, 4);
    put32(p + 8 + dlen, crc32_update(0, p + 4, 4 + (size_t)dlen));
    return 12 + (size_t)dlen;
}

uint8_t *lm_png_encode_xrgb(const uint32_t *px, int w, int h, size_t stride, size_t *len)
{
    if (!px || w <= 0 || h <= 0 || w > 32768 || h > 32768 || stride < (size_t)w * 4)
        return NULL;
    size_t rowlen = 1 + (size_t)w * 3;
    size_t raw_len = rowlen * (size_t)h;
    if (raw_len > 0x7fff0000u)
        return NULL;
    uint8_t *raw = malloc(raw_len);
    if (!raw)
        return NULL;
    uint32_t a = 1, b = 0;                                  /* Adler-32 of the raw stream */
    for (int y = 0; y < h; y++) {
        const uint32_t *src = (const uint32_t *)(const void *)((const uint8_t *)px + (size_t)y * stride);
        uint8_t *d = raw + (size_t)y * rowlen;
        *d++ = 0;                                           /* filter: none */
        for (int x = 0; x < w; x++) {
            *d++ = (uint8_t)(src[x] >> 16);
            *d++ = (uint8_t)(src[x] >> 8);
            *d++ = (uint8_t)src[x];
        }
    }
    for (size_t i = 0; i < raw_len;) {
        size_t n = raw_len - i < 5552 ? raw_len - i : 5552;  /* largest run without overflowing b */
        for (size_t k = 0; k < n; k++) {
            a += raw[i + k];
            b += a;
        }
        a %= 65521;
        b %= 65521;
        i += n;
    }

    size_t nblocks = (raw_len + 65534) / 65535;
    size_t zlen = 2 + raw_len + nblocks * 5 + 4;
    size_t total = 8 + 25 + (12 + zlen) + 12;
    uint8_t *out = malloc(total);
    if (!out) {
        free(raw);
        return NULL;
    }
    uint8_t *p = out;
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
    memcpy(p, sig, 8);
    p += 8;
    put32(p + 8, (uint32_t)w);                              /* IHDR */
    put32(p + 12, (uint32_t)h);
    p[16] = 8;                                              /* bit depth */
    p[17] = 2;                                              /* colour type: RGB */
    p[18] = p[19] = p[20] = 0;
    p += finish_chunk(p, "IHDR", 13);

    uint8_t *z = p + 8;                                     /* IDAT: zlib header + stored blocks + Adler-32 */
    *z++ = 0x78;
    *z++ = 0x01;
    for (size_t i = 0; i < raw_len;) {
        size_t n = raw_len - i < 65535 ? raw_len - i : 65535;
        *z++ = (i + n == raw_len) ? 1 : 0;                  /* BFINAL, BTYPE=00 */
        *z++ = (uint8_t)n;
        *z++ = (uint8_t)(n >> 8);
        *z++ = (uint8_t)~n;
        *z++ = (uint8_t)(~n >> 8);
        memcpy(z, raw + i, n);
        z += n;
        i += n;
    }
    put32(z, (b << 16) | a);
    p += finish_chunk(p, "IDAT", (uint32_t)zlen);
    p += finish_chunk(p, "IEND", 0);
    free(raw);
    *len = (size_t)(p - out);
    return out;
}
