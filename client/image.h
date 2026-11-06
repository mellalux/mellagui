/*
 * Image decoders for lumen-web (no libraries): PNG (all colour types and bit depths, interlaced), JPEG (baseline and
 * progressive, 8 bit, grayscale/YCbCr), GIF (first frame, transparency), BMP (1/4/8/24/32 bit uncompressed). Pixels are
 * 0xAARRGGBB with straight alpha. Images larger than 24 megapixels are refused.
 */
#ifndef LUMEN_IMAGE_H
#define LUMEN_IMAGE_H

#include <stddef.h>
#include <stdint.h>

struct image {
    int w, h;
    uint32_t *px;
};

/* NULL + message in err on failure. */
struct image *image_decode(const uint8_t *data, size_t len, char *err, size_t errn);
void image_free(struct image *im);
/* A scaled copy (box filter when shrinking, bilinear when enlarging); NULL if w/h <= 0. */
struct image *image_scale(const struct image *src, int w, int h);

/* zlib / deflate (exposed for tests): returns a malloc'ed buffer or NULL. */
uint8_t *image_inflate(const uint8_t *in, size_t n, int zlib_header, size_t *outn);

#endif
