/*
 * png.h - minimal PNG writer: 8-bit RGB, no interlacing, "stored" (uncompressed) deflate blocks, so no zlib.
 * Used by the screenshot keys; a 2048x864 frame is ~5 MB.
 */
#ifndef MELLAGUI_PNG_H
#define MELLAGUI_PNG_H

#include <stddef.h>
#include <stdint.h>

/* Encodes XRGB8888 pixels (0x00RRGGBB, `stride` in bytes). Returns a malloc'd file image (*len bytes) or NULL. */
uint8_t *lm_png_encode_xrgb(const uint32_t *px, int w, int h, size_t stride, size_t *len);

#endif
