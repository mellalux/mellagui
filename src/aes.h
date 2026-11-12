#ifndef MELLAGUI_AES_H
#define MELLAGUI_AES_H

/*
 * AES-128 (FIPS 197) and AES-128-GCM (NIST SP 800-38D, 12 byte nonce, 16 byte tag), written from scratch because no crypto library may be a
 * dependency. Used by the smart home program (client/tuya.c): the cloud API wraps every request and answer in AES-GCM. Table free except for the
 * S-box (built at the first use), not constant time - it protects data on the wire, the keys are per request.
 * No dependencies on server code.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct lm_aes128 {
    uint8_t rk[176];
};

void lm_aes128_init(struct lm_aes128 *a, const uint8_t key[16]);
void lm_aes128_encrypt_block(const struct lm_aes128 *a, const uint8_t in[16], uint8_t out[16]);
void lm_aes128_decrypt_block(const struct lm_aes128 *a, const uint8_t in[16], uint8_t out[16]);

/* out gets len bytes (it may be `in`), tag the 16 byte tag. */
void lm_aes128_gcm_encrypt(const uint8_t key[16], const uint8_t iv[12], const uint8_t *aad, size_t aadlen, const uint8_t *in, size_t len,
                           uint8_t *out, uint8_t tag[16]);
/* False (and out is zeroed) when the tag does not match. */
bool lm_aes128_gcm_decrypt(const uint8_t key[16], const uint8_t iv[12], const uint8_t *aad, size_t aadlen, const uint8_t *in, size_t len,
                           const uint8_t tag[16], uint8_t *out);

#endif
