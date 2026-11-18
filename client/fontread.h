#ifndef MELLAGUI_FONTREAD_H
#define MELLAGUI_FONTREAD_H

/*
 * A font file for the file manager's preview: what it is called, who made it, under what licence, how many glyphs and which scripts it covers, and a sample of
 * text set in the font itself (the picture is coverage only, 0..255; the caller picks the colours). Read: TrueType (.ttf, the first font of a .ttc), OpenType
 * with CFF outlines (.otf: the information, no sample - the rasteriser reads only TrueType outlines) and WOFF 1 (its tables are unpacked in memory). WOFF 2
 * (Brotli and a transformed glyph table) is recognised and said not to be read. Every length from the file is checked.
 */

#include <stddef.h>
#include <stdint.h>

struct font_info {
    char family[128], style[128], full[160], version[96], psname[96];
    char copyright[400], license[600], license_url[200], manufacturer[120], designer[120], description[400];
    int glyphs, units, weight;          /* weight class 100..900, 0 = unknown */
    int italic, monospace, cff, woff;   /* flags */
    char scripts[200];                  /* "Latin, Estonian, Cyrillic, Greek" */
    int w, h;                           /* the sample picture; alpha is NULL when the font's outlines cannot be painted */
    uint8_t *alpha;
};

/* 0 = ok; -1 = not a font; -2 = a font that cannot be read (the reason in err). */
int font_read(const char *path, struct font_info *fi, char *err, size_t errn);
/* The same without the picture and the scripts (fi->alpha stays NULL, scripts empty): a few milliseconds a font instead of tens, for a list of fonts. */
int font_read_names(const char *path, struct font_info *fi, char *err, size_t errn);
void font_free(struct font_info *fi);

#endif
