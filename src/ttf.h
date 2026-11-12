#ifndef MELLAGUI_TTF_H
#define MELLAGUI_TTF_H

/*
 * A small TrueType reader and rasteriser, from scratch (no FreeType). Owner: font.c, which is its only user.
 *
 * What it reads: .ttf files (and the first font of a .ttc) with `glyf` outlines - simple and compound glyphs -, the
 * `cmap` formats 4 and 12, `head`, `maxp`, `hhea`, `hmtx`, `loca`, optional `OS/2` (x height). Not read: CFF
 * outlines (.otf), hinting instructions, kerning, ligatures, colour glyphs.
 *
 * Every offset is checked against the file's size: a damaged file gives missing glyphs, never a crash.
 * Not thread safe per font object (one scratch buffer inside).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct lm_ttf;

/* Opens a font file (read whole into memory). NULL when it cannot be read or is not a TrueType font;
 * `why`, when not NULL, then points at a static text. */
struct lm_ttf *lm_ttf_open(const char *path, const char **why);
void lm_ttf_close(struct lm_ttf *f);

/* Glyph index of a code point, 0 = the font has none. */
int lm_ttf_glyph(const struct lm_ttf *f, uint32_t codepoint);

/* Font units: units per em, ascender (> 0), descender (< 0), the x height (0 = unknown). */
int lm_ttf_units(const struct lm_ttf *f);
int lm_ttf_ascent(const struct lm_ttf *f);
int lm_ttf_descent(const struct lm_ttf *f);
int lm_ttf_xheight(const struct lm_ttf *f);
/* True when the letters i and M and the digits are equally wide (a font for cells). */
bool lm_ttf_monospace(const struct lm_ttf *f);
/* Advance width of a glyph in font units. */
int lm_ttf_advance(const struct lm_ttf *f, int glyph);

/*
 * Paints one glyph into `out` (w * h bytes, coverage 0..255, cleared first): font unit (ux, uy) lands on the pixel
 * position (ox + ux * sx, baseline - uy * sy); what falls outside the picture is cut off.
 * Returns false when the glyph has no outline (a space) or cannot be read - `out` is then all zero.
 */
bool lm_ttf_render(struct lm_ttf *f, int glyph, double sx, double sy, double ox, double baseline,
                   uint8_t *out, int w, int h);

#endif
