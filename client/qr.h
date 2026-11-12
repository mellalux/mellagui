#ifndef MELLAGUI_QR_H
#define MELLAGUI_QR_H

/*
 * A QR code encoder (ISO/IEC 18004) for short texts: byte mode, error correction level M, versions 1..10 (up to 213 bytes), the mask with the
 * lowest penalty. The smart home program shows its login code with it.
 */

#include <stdbool.h>
#include <stdint.h>

#define QR_MAX_SIZE 57                  /* version 10 */

struct qr {
    int size;                           /* modules per side */
    uint8_t m[QR_MAX_SIZE * QR_MAX_SIZE];       /* 1 = dark; row by row */
};

/* False when the text is too long. `mask` 0..7 forces that mask (tests), -1 picks the best one. */
bool qr_encode(const char *text, struct qr *out, int mask);

/* Checks the parts against known values; the number of failures (messages on stderr). */
int qr_selftest(void);

static inline bool qr_dark(const struct qr *q, int x, int y)
{
    return x >= 0 && y >= 0 && x < q->size && y < q->size && q->m[y * q->size + x];
}

#endif
