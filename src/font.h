#ifndef MELLAGUI_FONT_H
#define MELLAGUI_FONT_H

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

#include <stdbool.h>
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

/*
 * ---- the font that is drawn --------------------------------------------------------------------------------------
 *
 * Text is still laid out in cells of LM_FONT_W x LM_FONT_H pixels (times a zoom), but what is painted into a cell is
 * either the bitmap above or a glyph of a TrueType file, rasterised with anti-aliasing by ttf.c at exactly that cell
 * size - so big text (the lock screen's clock, a zoomed terminal) gets real outlines instead of blown-up pixels.
 *
 * Which font: lm_font_use(), `[font] file = auto | bitmap | /path/to/Mono.ttf` in mellagui.conf (lm_font_conf), or
 * the environment variable MELLAGUI_FONT with the same words - the compositor exports what it chose, so the programs
 * it starts draw with the same font. Never chosen = MELLAGUI_FONT, else "auto": the first monospace font of a short
 * list of well known ones (DejaVu Sans Mono, Liberation Mono, Noto Sans Mono, ...), else the bitmap.
 * The font should be monospace: a glyph wider than its cell is squeezed into it.
 *
 * The box-drawing and block characters (U+2500..U+259F) always come from the bitmap, because their lines must meet
 * the cell's edges exactly; so does any character the file lacks and the bitmap has.
 *
 * Not thread safe (one cache): call from one thread. lm_font_glyph() stays safe everywhere.
 */

/* Coverage of one character in a cell of cw x ch pixels: cw * ch bytes, 0 = background .. 255 = ink, rows from the
 * top. Static storage, valid until the next call. With the bitmap font a whole multiple of 8 x 16 gives only 0 and
 * 255 (the old crisp pixels). cw is clamped to 1..512, ch to 1..1024 - the picture has the clamped size. */
const uint8_t *lm_font_cell(uint32_t codepoint, int cw, int ch);

/* "auto", "bitmap" or a file. False when the file cannot be used (the bitmap is used then; a warning names why). */
bool lm_font_use(const char *spec);
/* The file in use, or "bitmap". */
const char *lm_font_name(void);
/* Counts the changes of the font (cached pictures of text are stale when it moved). */
uint32_t lm_font_generation(void);

/* The `[font]` section: lm_font_reset() before the file is read, lm_font_conf() per line (false = unknown key or
 * bad value), lm_font_apply() afterwards - it chooses the font (MELLAGUI_FONT from before the first call wins over
 * the file) and exports the choice as MELLAGUI_FONT for the programs started from here. */
void lm_font_reset(void);
bool lm_font_conf(void *ud, const char *section, const char *key, const char *value);
void lm_font_apply(void);

/* dst with `a`/255 of src over it, all four channels (so opaque stays opaque, premultiplied stays right). */
static inline uint32_t lm_font_mix(uint32_t dst, uint32_t src, unsigned a)
{
    if (a >= 255)
        return src;
    if (a == 0)
        return dst;
    uint32_t rb = (dst & 0x00ff00ffu) * (255 - a) + (src & 0x00ff00ffu) * a + 0x00800080u;
    uint32_t ag = ((dst >> 8) & 0x00ff00ffu) * (255 - a) + ((src >> 8) & 0x00ff00ffu) * a + 0x00800080u;
    rb = ((rb + ((rb >> 8) & 0x00ff00ffu)) >> 8) & 0x00ff00ffu;
    ag = (ag + ((ag >> 8) & 0x00ff00ffu)) & 0xff00ff00u;
    return rb | ag;
}

#endif
