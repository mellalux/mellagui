#ifndef MELLAGUI_MD5_H
#define MELLAGUI_MD5_H

/*
 * MD5 (RFC 1321), written from scratch. Not for anything that needs a secure hash: the smart home cloud API (client/tuya.c) derives its request
 * keys from an MD5 text, so the program has to speak it. No dependencies on server code.
 */

#include <stddef.h>
#include <stdint.h>

#define LM_MD5_LEN 16

void lm_md5(const void *data, size_t len, uint8_t out[LM_MD5_LEN]);

#endif
