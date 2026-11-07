/*
 * SHA-256 / HMAC / PBKDF2. See sha256.h.
 */
#include "sha256.h"

#include <string.h>

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

struct sha_ctx {
    uint32_t h[8];
    uint8_t buf[64];
    size_t fill;
    uint64_t total;
};

static void sha_init(struct sha_ctx *c)
{
    static const uint32_t iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memcpy(c->h, iv, sizeof(iv));
    c->fill = 0;
    c->total = 0;
}

static void sha_block(struct sha_ctx *c, const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4], f = c->h[5], g = c->h[6], h = c->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22);
        uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void sha_update(struct sha_ctx *c, const void *data, size_t len)
{
    const uint8_t *p = data;
    c->total += len;
    if (c->fill) {
        size_t take = 64 - c->fill < len ? 64 - c->fill : len;
        memcpy(c->buf + c->fill, p, take);
        c->fill += take;
        p += take;
        len -= take;
        if (c->fill == 64) {
            sha_block(c, c->buf);
            c->fill = 0;
        }
    }
    while (len >= 64) {
        sha_block(c, p);
        p += 64;
        len -= 64;
    }
    if (len) {
        memcpy(c->buf, p, len);
        c->fill = len;
    }
}

static void sha_final(struct sha_ctx *c, uint8_t out[LM_SHA256_LEN])
{
    uint64_t bits = c->total * 8;
    uint8_t pad = 0x80;
    sha_update(c, &pad, 1);
    pad = 0;
    while (c->fill != 56)
        sha_update(c, &pad, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; i++)
        len[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha_update(c, len, 8);
    for (int i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(c->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(c->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)c->h[i];
    }
}

void lm_sha256(const void *data, size_t len, uint8_t out[LM_SHA256_LEN])
{
    struct sha_ctx c;
    sha_init(&c);
    sha_update(&c, data, len);
    sha_final(&c, out);
    lm_secure_zero(&c, sizeof(c));
}

/* HMAC with the two padded-key states precomputed, so that PBKDF2 pays two compressions per HMAC call. */
struct hmac_ctx {
    struct sha_ctx inner, outer;
};

static void hmac_init(struct hmac_ctx *h, const void *key, size_t keylen)
{
    uint8_t k[64] = { 0 }, pad[64];
    if (keylen > 64)
        lm_sha256(key, keylen, k);
    else
        memcpy(k, key, keylen);
    for (int i = 0; i < 64; i++)
        pad[i] = k[i] ^ 0x36;
    sha_init(&h->inner);
    sha_update(&h->inner, pad, 64);
    for (int i = 0; i < 64; i++)
        pad[i] = k[i] ^ 0x5c;
    sha_init(&h->outer);
    sha_update(&h->outer, pad, 64);
    lm_secure_zero(k, sizeof(k));
    lm_secure_zero(pad, sizeof(pad));
}

static void hmac_run(const struct hmac_ctx *h, const void *msg, size_t len, uint8_t out[LM_SHA256_LEN])
{
    struct sha_ctx c = h->inner;
    uint8_t ih[LM_SHA256_LEN];
    sha_update(&c, msg, len);
    sha_final(&c, ih);
    c = h->outer;
    sha_update(&c, ih, sizeof(ih));
    sha_final(&c, out);
}

void lm_hmac_sha256(const void *key, size_t keylen, const void *msg, size_t msglen, uint8_t out[LM_SHA256_LEN])
{
    struct hmac_ctx h;
    hmac_init(&h, key, keylen);
    hmac_run(&h, msg, msglen, out);
    lm_secure_zero(&h, sizeof(h));
}

void lm_pbkdf2_sha256(const void *pw, size_t pwlen, const void *salt, size_t saltlen, uint32_t iterations,
                      uint8_t *dk, size_t dklen)
{
    struct hmac_ctx h;
    hmac_init(&h, pw, pwlen);
    if (iterations < 1)
        iterations = 1;
    for (uint32_t block = 1; dklen > 0; block++) {
        uint8_t first[256], u[LM_SHA256_LEN], t[LM_SHA256_LEN];
        size_t sl = saltlen < sizeof(first) - 4 ? saltlen : sizeof(first) - 4;      /* salts here are 16 bytes */
        memcpy(first, salt, sl);
        first[sl] = (uint8_t)(block >> 24);
        first[sl + 1] = (uint8_t)(block >> 16);
        first[sl + 2] = (uint8_t)(block >> 8);
        first[sl + 3] = (uint8_t)block;
        hmac_run(&h, first, sl + 4, u);
        memcpy(t, u, sizeof(t));
        for (uint32_t i = 1; i < iterations; i++) {
            hmac_run(&h, u, sizeof(u), u);
            for (int j = 0; j < LM_SHA256_LEN; j++)
                t[j] ^= u[j];
        }
        size_t take = dklen < LM_SHA256_LEN ? dklen : LM_SHA256_LEN;
        memcpy(dk, t, take);
        dk += take;
        dklen -= take;
        lm_secure_zero(u, sizeof(u));
        lm_secure_zero(t, sizeof(t));
    }
    lm_secure_zero(&h, sizeof(h));
}

bool lm_ct_equal(const void *a, const void *b, size_t n)
{
    const volatile uint8_t *x = a, *y = b;
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++)
        d |= (uint8_t)(x[i] ^ y[i]);
    return d == 0;
}

void lm_hex_encode(const uint8_t *in, size_t n, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = digits[in[i] >> 4];
        out[2 * i + 1] = digits[in[i] & 15];
    }
    out[2 * n] = 0;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool lm_hex_decode(const char *hex, uint8_t *out, size_t n)
{
    if (!hex || strlen(hex) != 2 * n)
        return false;
    for (size_t i = 0; i < n; i++) {
        int hi = hexval(hex[2 * i]), lo = hexval(hex[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return true;
}

void lm_secure_zero(void *p, size_t n)
{
    volatile uint8_t *v = p;
    while (n--)
        *v++ = 0;
}
