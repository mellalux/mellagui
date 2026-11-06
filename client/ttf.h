/*
 * A small TrueType renderer for lumen-web (no libraries): reads glyf outlines (simple and composite glyphs, so accented
 * letters work), cmap formats 4 and 12, hmtx; renders anti-aliased coverage bitmaps with exact area coverage. System fonts
 * are loaded from /usr/share/fonts at run time; nothing is embedded.
 */
#ifndef LUMEN_TTF_H
#define LUMEN_TTF_H

#include <stdint.h>

struct ttf;

struct ttf_glyph {
    int w, h;                       /* bitmap size */
    int left, top;                  /* offset of the bitmap's top-left from the pen position / baseline (top is above it) */
    int advance;                    /* pixels */
    uint8_t *cov;                   /* w*h coverage 0..255 (NULL for blank glyphs) */
};

struct ttf *ttf_load(const char *path);                 /* NULL if missing or not a TrueType font */
const struct ttf_glyph *ttf_glyph(struct ttf *f, uint32_t codepoint, int px);   /* cached; never NULL for a loaded font */
int ttf_advance(struct ttf *f, uint32_t codepoint, int px);
int ttf_ascent(struct ttf *f, int px);
int ttf_descent(struct ttf *f, int px);                 /* positive pixels below the baseline */

#endif
