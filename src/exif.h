#ifndef MELLAGUI_EXIF_H
#define MELLAGUI_EXIF_H

/*
 * EXIF metadata of a picture (camera, lens, exposure, time, place), read without a library from the TIFF structure that JPEG (APP1 "Exif"), PNG (eXIf), WebP
 * (EXIF chunk) and TIFF files carry: IFD0, the Exif sub-IFD and the GPS sub-IFD. Every offset and length is checked; a damaged block gives what was read.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct lm_exif {
    char make[64], model[96], software[64], artist[64], copyright[96], description[160];
    char lens[96], taken[24];                       /* "2023-03-14 12:00:00" */
    double exposure;                                /* seconds, 0 = unknown */
    double fnumber, focal, focal35, bias;           /* 0 = unknown (bias: has_bias) */
    bool has_bias;
    int iso;
    int flash;                                      /* the EXIF flash word, -1 = unknown */
    int orientation;                                /* 1..8, 0 = unknown */
    int width, height;                              /* the pixel dimensions the camera wrote, 0 = unknown */
    bool has_gps, has_alt;
    double lat, lon, alt;                           /* degrees (south / west negative), metres */
    bool any;                                       /* something was found */
};

/* A TIFF block ("II*\0" or "MM\0*" first). Returns true when it parsed as one. */
bool lm_exif_parse_tiff(const uint8_t *tiff, size_t n, struct lm_exif *x);
/* A picture file (JPEG, PNG, WebP or TIFF, told by its first bytes). Returns true when EXIF data was found. */
bool lm_exif_read_file(const char *path, struct lm_exif *x);
bool lm_exif_read_mem(const uint8_t *d, size_t n, struct lm_exif *x);
/* Readable lines ("Camera: Canon EOS 5D", "Exposure: 1/250 s  f/2.8  ISO 200  85 mm" ...); returns how many (at most max). */
int lm_exif_lines(const struct lm_exif *x, char lines[][200], int max);

#endif
