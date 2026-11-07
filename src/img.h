/*
 * img.h - image decoders for mellagui-view: PNG (own inflate, all colour types and bit depths, no interlacing) and
 * binary PPM (P6). Transparent pixels are composited over a grey checkerboard, the result is opaque XRGB8888.
 */
#ifndef MELLAGUI_IMG_H
#define MELLAGUI_IMG_H

#include <stddef.h>
#include <stdint.h>

#define LM_IMG_MAX_PIXELS (64u * 1024u * 1024u)

struct lm_image {
    int w, h;
    uint32_t *px;                   /* w * h, malloc'd */
};

/* Both return 0, or -1 with a short message in err. */
int lm_img_load(const char *path, struct lm_image *img, char *err, size_t errsz);
int lm_img_decode(const uint8_t *data, size_t len, struct lm_image *img, char *err, size_t errsz);   /* PNG or P6 by content */
void lm_img_free(struct lm_image *img);

#endif
