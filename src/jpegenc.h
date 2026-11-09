#ifndef MELLAGUI_JPEGENC_H
#define MELLAGUI_JPEGENC_H

/*
 * Baseline JPEG encoder (sequential DCT, 8 bit, YCbCr 4:4:4 so that text stays sharp, the standard Huffman tables of ITU T.81
 * Annex K, JFIF header). For the screen recorder: every frame of its MP4 is one such picture. Owner: jpegenc.c.
 */

#include <stddef.h>
#include <stdint.h>

/*
 * Encodes XRGB8888 pixels (0x00RRGGBB, `stride` in bytes) as a JPEG file image. `quality` 1..100 (the libjpeg scale; 80 is a
 * good screen quality). Returns a malloc'd buffer (*len bytes) or NULL (bad size / out of memory). Width and height 1..65535.
 * Threads may encode at the same time once lm_jpeg_encode_init() has been called (the first call of the encoder does it otherwise, which
 * is then not thread safe).
 */
void lm_jpeg_encode_init(void);
uint8_t *lm_jpeg_encode_xrgb(const uint32_t *px, int w, int h, size_t stride, int quality, size_t *len);

#endif
