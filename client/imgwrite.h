#ifndef MELLAGUI_IMGWRITE_H
#define MELLAGUI_IMGWRITE_H

/*
 * Picture file writers for the picture editor (mellagui-paint), no library. Every function takes `w` x `h` pixels of straight (not premultiplied) ARGB and
 * returns a malloc'd file (`*n` bytes) or NULL. A picture with no see-through pixel is written without an alpha channel where the format has the choice.
 *
 *   lm_enc_bmp    Windows bitmap: 24 bit, or 32 bit with an alpha mask (a V4 header) when something is see-through; bottom up
 *   lm_enc_tga    Targa, uncompressed 24 / 32 bit, top down
 *   lm_enc_tiff   TIFF, little endian, one strip packed with deflate, RGB or RGBA (unassociated alpha)
 *   lm_enc_ppm    binary PPM (P6): no alpha, see-through pixels are laid on white
 *   lm_enc_gif    GIF89a, one picture: the colours themselves when there are at most 256 (255 and one transparent), else a median cut palette; a pixel
 *                 that is more than half see-through becomes the transparent colour
 *   lm_enc_webp   WebP, lossless (VP8L): the subtract-green transform and Huffman codes for every channel; alpha stays
 */

#include <stddef.h>
#include <stdint.h>

uint8_t *lm_enc_bmp(const uint32_t *px, int w, int h, size_t *n);
uint8_t *lm_enc_tga(const uint32_t *px, int w, int h, size_t *n);
uint8_t *lm_enc_tiff(const uint32_t *px, int w, int h, size_t *n);
uint8_t *lm_enc_ppm(const uint32_t *px, int w, int h, size_t *n);
uint8_t *lm_enc_gif(const uint32_t *px, int w, int h, size_t *n);
uint8_t *lm_enc_webp(const uint32_t *px, int w, int h, size_t *n);

#endif
