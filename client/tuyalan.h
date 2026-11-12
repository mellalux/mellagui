#ifndef MELLAGUI_TUYALAN_H
#define MELLAGUI_TUYALAN_H

/*
 * tuyalan - the Smart Life / Tuya devices in the local network: finding them and telling them things. Every device announces itself a few times a minute with
 * a UDP broadcast (port 6666 in plain text for the old protocol 3.1, port 6667 encrypted with a key everybody knows for 3.3 and later) that says
 * its id, its address in this network and the protocol version it speaks. The cloud only knows the public address, so this is how the two meet.
 *
 * The two frame formats (the open tinytuya project is the reference):
 *   000055AA  seq  cmd  length  [return code]  payload  crc32  0000AA55            payload: JSON, or AES-128-ECB of it (PKCS#7 padding)
 *   00006699  00 00  seq  cmd  length  nonce(12)  AES-128-GCM text  tag(16)  00009966   (3.5; the 14 header bytes after the prefix are the AAD)
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct tuyalan_found {
    char id[40], ip[48], version[8], product_key[40];
};

/* One broadcast into its JSON text; false when it is not one or does not decrypt. */
bool tuyalan_decode(const uint8_t *msg, size_t n, char *json, size_t jn);
/* Listens for `ms` milliseconds; the devices heard (each once) go to out[0..max). Returns how many, -1 when no port could be opened (err). */
/* stats (may be NULL): [0] = the datagrams that arrived on the two ports, [1] = those that were no broadcast this code understands. */
int tuyalan_scan(int ms, struct tuyalan_found *out, int max, int stats[2], char *err, size_t errsz);
/*
 * Tells a device something over the local network: one short TCP connection to port 6668 ($MELLAGUI_TUYA_LAN_PORT in tests). `dps` is the JSON
 * object of the values by number, e.g. {"1":true}. Protocol 3.3 (the command AES-128-ECB encrypted with the device's local key, CRC32) and 3.4 (a
 * session key is agreed first with two random numbers; every frame carries an HMAC-SHA256). Returns
 *    1  the device confirmed it
 *    0  it did not get as far as sending the command (no connection, the key is not accepted, the version is not one of the two): the caller may
 *       go another way
 *   -1  the command was sent but not confirmed (it must not be sent again blindly: feeding the cat twice is worse than once)
 */
int tuyalan_set(const char *ip, const char *version, const char *dev_id, const char *local_key, const char *dps, char *err, size_t errsz);
/*
 * Asks a device for its values over the local network (the same short connection, protocols 3.3 and 3.4): `json` gets the answer's text, an object
 * with the member "dps" = the values by number ({"dps":{"1":true,"9":0}}). `ask` (may be NULL) is an object with every number the caller knows and the
 * value null ({"1":null,"9":null}): a device that does not know the plain question is asked with it. False with `err` when it does not answer.
 */
bool tuyalan_get(const char *ip, const char *version, const char *dev_id, const char *local_key, const char *ask, char *json, size_t jn, char *err,
                 size_t errsz);
int tuyalan_selftest(void);

#endif
