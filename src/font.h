#ifndef LUMEN_FONT_H
#define LUMEN_FONT_H

/*
 * Built-in 8x16 monochrome bitmap font, shared by the renderer (title bars,
 * panel) and the clients. Owner: render.c's owner (font.c).
 * No dependencies; linkable into clients.
 *
 * Required coverage: ASCII 0x20..0x7e, Latin-1 supplement 0xa0..0xff, the
 * Estonian/Baltic letters U+0160 U+0161 U+017d U+017e (Š š Ž ž), the euro
 * sign, and the box-drawing / block elements a terminal needs
 * (U+2500..U+257f subset, U+2580..U+259f subset). Unknown code points map to
 * a visible replacement glyph (never NULL).
 */

#include <stdint.h>

#define LM_FONT_W 8
#define LM_FONT_H 16

/*
 * Returns 16 bytes, one per row from top to bottom; bit 7 is the leftmost
 * pixel. Pointer is to static storage.
 */
const uint8_t *lm_font_glyph(uint32_t codepoint);

/* Decode one UTF-8 sequence from s (max `len` bytes). Returns the code point
 * (U+FFFD on error) and stores the consumed byte count (>=1 when len>0) in *used. */
uint32_t lm_utf8_decode(const char *s, int len, int *used);

/* Width in pixels of a NUL-terminated UTF-8 string (LM_FONT_W per code point). */
int lm_font_text_width(const char *utf8);

#endif
