/* flag.h - the flags of the keyboard layouts' countries, as a few coloured rectangles in a 22 x 14 box (shared by the compositor's panel and the settings
 * panel, which both fill rectangles). The colours are literals on purpose: a flag is a flag in every theme. Header only. */
#ifndef LM_FLAG_H
#define LM_FLAG_H

#include <stdint.h>
#include <string.h>

#define LM_FLAG_W 22
#define LM_FLAG_H 14
#define LM_FLAG_MAX 24                  /* rectangles in a flag at most (the US one has 20) */

struct lm_flag_rect { int x, y, w, h; uint32_t rgb; };

/* The rectangles (painted in order, the first covers the whole flag) of the flag of layout `id` ("us" or "US" ...); 0 = this layout has no flag. */
static inline int lm_flag_rects(const char *id, struct lm_flag_rect out[LM_FLAG_MAX])
{
    char u[3] = { 0, 0, 0 };
    for (int i = 0; i < 2 && id[i]; i++)
        u[i] = (char)(id[i] >= 'a' && id[i] <= 'z' ? id[i] - 32 : id[i]);
    int n = 0;
#define R(rx, ry, rw, rh, col) out[n++] = (struct lm_flag_rect){ (rx), (ry), (rw), (rh), (col) }
    if (!strcmp(u, "US")) {                         /* seven stripes, a blue field with white stars (dots) */
        for (int i = 0; i < 7; i++)
            R(0, i * 2, LM_FLAG_W, 2, i % 2 ? 0xffffffu : 0xb22234u);
        R(0, 0, 10, 8, 0x3c3b6eu);
        for (int r = 0; r < 3; r++)
            for (int k = 0; k < 4; k++)
                R(1 + k * 2 + (r % 2), 1 + r * 2, 1, 1, 0xffffffu);
    } else if (!strcmp(u, "EE")) {                  /* blue, black, white */
        R(0, 0, LM_FLAG_W, 5, 0x0072ceu);
        R(0, 5, LM_FLAG_W, 4, 0x000000u);
        R(0, 9, LM_FLAG_W, 5, 0xffffffu);
    } else if (!strcmp(u, "DE")) {                  /* black, red, gold */
        R(0, 0, LM_FLAG_W, 5, 0x000000u);
        R(0, 5, LM_FLAG_W, 4, 0xdd0000u);
        R(0, 9, LM_FLAG_W, 5, 0xffce00u);
    } else if (!strcmp(u, "FI")) {                  /* a blue cross on white */
        R(0, 0, LM_FLAG_W, LM_FLAG_H, 0xffffffu);
        R(5, 0, 4, LM_FLAG_H, 0x003580u);
        R(0, 5, LM_FLAG_W, 4, 0x003580u);
    }
#undef R
    return n;
}

#endif
