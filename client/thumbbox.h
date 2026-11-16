#ifndef MELLAGUI_THUMBBOX_H
#define MELLAGUI_THUMBBOX_H

/*
 * thumbbox.h - squeeze an XRGB picture into a smaller one by averaging blocks (header only): the small pictures of mellagui-cam's strip and of
 * mellagui-view's strip, the tiles of the camera's grid of effects. A destination bigger than the source repeats pixels (nearest).
 *
 *   lm_thumb_box(src, sw, sh, dst, dw, dh)      src is sw x sh (stride sw), dst dw x dh (stride dw), opaque result
 */

#include <stdint.h>

static inline void lm_thumb_box(const uint32_t *s, int sw, int sh, uint32_t *d, int dw, int dh)
{
    for (int y = 0; y < dh; y++) {
        int y0 = (int)((int64_t)y * sh / dh), y1 = (int)((int64_t)(y + 1) * sh / dh);
        y1 = y1 > y0 + 1 ? y1 : y0 + 1;
        y1 = y1 < sh ? y1 : sh;
        for (int x = 0; x < dw; x++) {
            int x0 = (int)((int64_t)x * sw / dw), x1 = (int)((int64_t)(x + 1) * sw / dw);
            x1 = x1 > x0 + 1 ? x1 : x0 + 1;
            x1 = x1 < sw ? x1 : sw;
            unsigned r = 0, g = 0, b = 0, n = 0;
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++) {
                    uint32_t p = s[(size_t)yy * (size_t)sw + (size_t)xx];
                    r += (p >> 16) & 255;
                    g += (p >> 8) & 255;
                    b += p & 255;
                    n++;
                }
            d[(size_t)y * (size_t)dw + (size_t)x] = 0xff000000u | (r / n) << 16 | (g / n) << 8 | (b / n);
        }
    }
}

#endif
