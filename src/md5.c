#include "md5.h"

#include <string.h>

static uint32_t rol(uint32_t x, int n)
{
    return (x << n) | (x >> (32 - n));
}

static void md5_block(uint32_t st[4], const uint8_t *p)
{
    static const uint8_t shift[4][4] = { { 7, 12, 17, 22 }, { 5, 9, 14, 20 }, { 4, 11, 16, 23 }, { 6, 10, 15, 21 } };
    static const uint32_t k[64] = {          /* floor(|sin(i + 1)| * 2^32): written out, so that threads never see it half made */
        0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
        0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu, 0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
        0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau, 0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
        0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu, 0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
        0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu, 0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
        0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u, 0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
        0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
        0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u, 0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u,
    };
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
