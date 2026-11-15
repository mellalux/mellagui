#ifndef MELLAGUI_CERTREAD_H
#define MELLAGUI_CERTREAD_H

/*
 * Certificates and keys for the file manager's preview, with no library: PEM blocks and bare DER are read (ASN.1 by hand) and described as JSON text, which the
 * preview shows in its tree view. Understood: X.509 certificates (subject, issuer, serial, validity and whether it has run out, signature algorithm, public key
 * (RSA size, EC curve, Ed25519 ...), the usual extensions: subject alternative names, basic constraints, key usage, extended key usage, key identifiers,
 * and the SHA-256 fingerprint), certificate revocation lists, certificate requests, and keys (public: algorithm and size; PRIVATE keys: only the kind and
 * size - the key itself is never read out or shown). Not understood: PKCS#7 / PKCS#12 bundles. Every length from the file is checked.
 */

#include <stddef.h>

/* The items found in the bytes of a file as JSON text (malloc'd; an array when there are several, an object for one), or NULL when there are none.
 * `info` gets a short line about the first item. */
char *cert_json(const unsigned char *buf, size_t n, char *info, size_t infon);

#endif
