#include "md5.h"

#include <math.h>
#include <string.h>

static uint32_t rol(uint32_t x, int n)
{
    return (x << n) | (x >> (32 - n));
}

static void md5_block(uint32_t st[4], const uint8_t *p)
{
    static const uint8_t shift[4][4] = { { 7, 12, 17, 22 }, { 5, 9, 14, 20 }, { 4, 11, 16, 23 }, { 6, 10, 15, 21 } };
    static uint32_t k[64];
    if (!k[0])
        for (int i = 0; i < 64; i++)
            k[i] = (uint32_t)(fabs(sin((double)(i + 1))) * 4294967296.0);
    uint32_t m[16], a = st[0], b = st[1], c = st[2], d = st[3];
    for (int i = 0; i < 16; i++)
        m[i] = (uint32_t)p[4 * i] | (uint32_t)p[4 * i + 1] << 8 | (uint32_t)p[4 * i + 2] << 16 | (uint32_t)p[4 * i + 3] << 24;
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        switch (i / 16) {
        case 0: f = (b & c) | (~b & d); g = i; break;
        case 1: f = (d & b) | (~d & c); g = (5 * i + 1) % 16; break;
        case 2: f = b ^ c ^ d; g = (3 * i + 5) % 16; break;
        default: f = c ^ (b | ~d); g = (7 * i) % 16; break;
        }
        uint32_t t = d;
        d = c;
        c = b;
        b = b + rol(a + f + k[i] + m[g], shift[i / 16][i % 4]);
        a = t;
    }
    st[0] += a;
    st[1] += b;
    st[2] += c;
    st[3] += d;
}

void lm_md5(const void *data, size_t len, uint8_t out[LM_MD5_LEN])
{
    uint32_t st[4] = { 0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u };
    const uint8_t *p = data;
    size_t left = len;
    for (; left >= 64; left -= 64, p += 64)
        md5_block(st, p);
    uint8_t tail[128] = { 0 };
    memcpy(tail, p, left);
    tail[left] = 0x80;
    size_t n = left < 56 ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++)
        tail[n - 8 + i] = (uint8_t)(bits >> (8 * i));
    md5_block(st, tail);
    if (n == 128)
        md5_block(st, tail + 64);
    for (int i = 0; i < 16; i++)
        out[i] = (uint8_t)(st[i / 4] >> (8 * (i % 4)));
}
