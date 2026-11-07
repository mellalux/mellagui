#ifndef MELLAGUI_SHA256_H
#define MELLAGUI_SHA256_H

/*
 * SHA-256, HMAC-SHA-256 and PBKDF2-HMAC-SHA-256 (RFC 6234 / 2104 / 8018), written from scratch because no crypto
 * library may be a dependency. Used by the screen lock (lock.c) to keep the password / PIN only as a salted, slow hash.
 * No dependencies on server code (unit-tested in keymap-test).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LM_SHA256_LEN 32

void lm_sha256(const void *data, size_t len, uint8_t out[LM_SHA256_LEN]);
void lm_hmac_sha256(const void *key, size_t keylen, const void *msg, size_t msglen, uint8_t out[LM_SHA256_LEN]);
/* dk = PBKDF2-HMAC-SHA256(password, salt, iterations), dklen bytes (any length). */
void lm_pbkdf2_sha256(const void *pw, size_t pwlen, const void *salt, size_t saltlen, uint32_t iterations,
                      uint8_t *dk, size_t dklen);

/* Constant-time comparison (the time does not depend on where the first difference is). */
bool lm_ct_equal(const void *a, const void *b, size_t n);

/* Lower-case hex of n bytes into out (2n + 1 bytes, NUL-terminated); the reverse returns false on bad input. */
void lm_hex_encode(const uint8_t *in, size_t n, char *out);
bool lm_hex_decode(const char *hex, uint8_t *out, size_t n);

/* Overwrites memory in a way the compiler may not drop (secrets are wiped after use). */
void lm_secure_zero(void *p, size_t n);

#endif
