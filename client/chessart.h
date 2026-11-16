/*
 * chessart.h - chess pieces as pictures (header only, no files, no fonts; the cousin of cardart.h and built on its distance functions).
 *
 *   chessart_piece(type, colour, size)    a malloc'd size x size picture (straight ARGB, see-through around the piece) of one piece:
 *                                         type 1 pawn, 2 knight, 3 bishop, 4 rook, 5 queen, 6 king (the numbers of chess.h), colour 0 white, 1 black.
 *
 * The shapes are drawn per pixel from signed distance functions in a box of -1 .. 1 (y down) with an anti-aliased edge, so a piece is sharp at any
 * size; an outline, a soft shading from the top and a few details cut into the body (the bishop's slit, the knight's eye and mane, the rook's
 * course of bricks, the bands of the queen and the king). White pieces are cream with a dark outline, black pieces are slate with a light detail.
 */
#ifndef MELLAGUI_CHESSART_H
#define MELLAGUI_CHESSART_H

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "cardart.h"
#pragma GCC diagnostic pop

#define CHA_WHITE_FILL 0xf6f1e4u
#define CHA_WHITE_LINE 0x23252bu
#define CHA_BLACK_FILL 0x4b4f5cu
#define CHA_BLACK_LINE 0x0c0d11u
#define CHA_BLACK_DETAIL 0xc9cddau

/* the foot every piece stands on */
static inline float cha_foot(float x, float y)
{
    float d = ca_rbox(x, y, 0.0f, 0.72f, 0.56f, 0.09f, 0.06f);
    return fminf(d, ca_rbox(x, y, 0.0f, 0.56f, 0.44f, 0.07f, 0.045f));
}

static inline float cha_stroke(float x, float y, float ax, float ay, float bx, float by, float w)
{
    return ca_seg(x, y, ax, ay, bx, by) - w;
}

/* `d` = the body (negative inside), `line` = details that are cut into it (negative on a line) */
static void cha_shape(int type, float x, float y, float *d, float *line)
{
    float l = 1e9f, b;
    switch (type) {
    case 1: {                                                   /* pawn */
        static const float body[] = { -0.11f, -0.30f, 0.11f, -0.30f, 0.30f, 0.56f, -0.30f, 0.56f };
        b = ca_smin(ca_circle(x, y, 0.0f, -0.46f, 0.215f), ca_rbox(x, y, 0.0f, -0.24f, 0.20f, 0.045f, 0.03f), 0.07f);
        b = ca_smin(b, ca_poly(x, y, body, 4), 0.09f);
        l = cha_stroke(x, y, -0.20f, -0.215f, 0.20f, -0.215f, 0.016f);
        break;
    }
    case 2: {                                                   /* knight: a horse's head turned to the left */
        static const float head[] = {
            -0.42f, 0.56f, 0.40f, 0.56f, 0.38f, 0.16f, 0.32f, -0.18f, 0.18f, -0.42f, 0.08f, -0.56f, 0.05f, -0.78f, -0.10f, -0.62f,
            -0.28f, -0.52f, -0.56f, -0.14f, -0.52f, 0.0f, -0.38f, 0.03f, -0.22f, 0.12f, -0.34f, 0.30f };
        b = ca_poly(x, y, head, 14);
        b = ca_smin(b, ca_circle(x, y, -0.02f, -0.50f, 0.04f), 0.02f);
        l = ca_circle(x, y, -0.20f, -0.30f, 0.045f);                                        /* the eye */
        l = fminf(l, cha_stroke(x, y, 0.06f, -0.42f, 0.30f, 0.05f, 0.018f));                /* the mane */
        l = fminf(l, cha_stroke(x, y, 0.02f, -0.30f, 0.24f, 0.14f, 0.016f));
        l = fminf(l, ca_circle(x, y, -0.50f, -0.12f, 0.02f));                               /* a nostril */
        break;
    }
    case 3: {                                                   /* bishop */
        static const float flare[] = { -0.15f, 0.20f, 0.15f, 0.20f, 0.34f, 0.56f, -0.34f, 0.56f };
        b = ca_ellipse(x, y, 0.0f, -0.22f, 0.25f, 0.40f);
        b = ca_smin(b, ca_circle(x, y, 0.0f, -0.70f, 0.10f), 0.05f);
        b = ca_smin(b, ca_rbox(x, y, 0.0f, 0.20f, 0.27f, 0.045f, 0.03f), 0.06f);
        b = ca_smin(b, ca_poly(x, y, flare, 4), 0.08f);
        l = cha_stroke(x, y, -0.07f, -0.40f, 0.13f, -0.20f, 0.02f);                         /* the slit of the mitre */
        l = fminf(l, cha_stroke(x, y, -0.20f, 0.20f, 0.20f, 0.20f, 0.014f));
        break;
    }
    case 4: {                                                   /* rook */
        static const float neck[] = { -0.25f, -0.30f, 0.25f, -0.30f, 0.34f, 0.56f, -0.34f, 0.56f };
        b = ca_poly(x, y, neck, 4);
        float top = ca_rbox(x, y, 0.0f, -0.48f, 0.40f, 0.17f, 0.03f);
        float notch = fminf(ca_rbox(x, y, -0.15f, -0.68f, 0.075f, 0.12f, 0.0f), ca_rbox(x, y, 0.15f, -0.68f, 0.075f, 0.12f, 0.0f));
        top = fmaxf(top, -notch);
        b = fminf(b, top);
        l = cha_stroke(x, y, -0.36f, -0.28f, 0.36f, -0.28f, 0.016f);
        l = fminf(l, cha_stroke(x, y, -0.22f, 0.12f, 0.22f, 0.12f, 0.012f));
        break;
    }
    case 5: {                                                   /* queen: five wide spikes with a ball on each */
        static const float body[] = { -0.22f, -0.22f, 0.22f, -0.22f, 0.37f, 0.56f, -0.37f, 0.56f };
        static const float tx[5] = { -0.40f, -0.20f, 0.0f, 0.20f, 0.40f }, ty[5] = { -0.62f, -0.67f, -0.73f, -0.67f, -0.62f };
        b = ca_rbox(x, y, 0.0f, -0.24f, 0.30f, 0.05f, 0.03f);
        for (int i = 0; i < 5; i++) {
            b = fminf(b, ca_tri(x, y, tx[i] * 0.88f - 0.14f, -0.24f, tx[i] * 0.88f + 0.14f, -0.24f, tx[i], ty[i]));
            b = ca_smin(b, ca_circle(x, y, tx[i], ty[i] - 0.015f, 0.065f), 0.04f);
        }
        b = ca_smin(b, ca_poly(x, y, body, 4), 0.07f);
        b = ca_smin(b, ca_rbox(x, y, 0.0f, 0.24f, 0.30f, 0.045f, 0.03f), 0.06f);
        l = cha_stroke(x, y, -0.25f, -0.19f, 0.25f, -0.19f, 0.014f);
        l = fminf(l, cha_stroke(x, y, -0.22f, 0.24f, 0.22f, 0.24f, 0.014f));
        break;
    }
    default: {                                                  /* king */
        static const float body[] = { -0.22f, -0.20f, 0.22f, -0.20f, 0.38f, 0.56f, -0.38f, 0.56f };
        b = ca_ellipse(x, y, 0.0f, -0.40f, 0.29f, 0.22f);
        b = ca_smin(b, ca_poly(x, y, body, 4), 0.07f);
        b = ca_smin(b, ca_rbox(x, y, 0.0f, 0.24f, 0.31f, 0.045f, 0.03f), 0.06f);
        float cross = fminf(ca_rbox(x, y, 0.0f, -0.70f, 0.052f, 0.16f, 0.015f), ca_rbox(x, y, 0.0f, -0.66f, 0.14f, 0.05f, 0.015f));
        b = fminf(b, cross);
        l = cha_stroke(x, y, -0.27f, -0.22f, 0.27f, -0.22f, 0.014f);
        l = fminf(l, cha_stroke(x, y, -0.22f, 0.24f, 0.22f, 0.24f, 0.014f));
        break;
    }
    }
    *d = fminf(b, cha_foot(x, y));
    *line = l;
}

static inline uint32_t cha_mix(uint32_t a, uint32_t b, float t)
{
    t = ca_clamp(t, 0, 1);
    uint32_t r = (uint32_t)((float)((a >> 16) & 255) * (1 - t) + (float)((b >> 16) & 255) * t + 0.5f);
    uint32_t g = (uint32_t)((float)((a >> 8) & 255) * (1 - t) + (float)((b >> 8) & 255) * t + 0.5f);
    uint32_t bl = (uint32_t)((float)(a & 255) * (1 - t) + (float)(b & 255) * t + 0.5f);
    return r << 16 | g << 8 | bl;
}

static uint32_t *chessart_piece(int type, int colour, int size)
{
    if (size < 4)
        size = 4;
    uint32_t *out = malloc((size_t)size * (size_t)size * 4);
    if (!out)
        return NULL;
    uint32_t fill = colour ? CHA_BLACK_FILL : CHA_WHITE_FILL, edge = colour ? CHA_BLACK_LINE : CHA_WHITE_LINE;
    uint32_t detail = colour ? CHA_BLACK_DETAIL : CHA_WHITE_LINE;
    float aa = 2.0f / (float)size * 1.2f / 1.1f, rim = 0.045f;
    for (int py = 0; py < size; py++)
        for (int px = 0; px < size; px++) {
            float x = (((float)px + 0.5f) / (float)size * 2 - 1) / 1.1f, y = (((float)py + 0.5f) / (float)size * 2 - 1) / 1.1f;   /* (the shapes fill 1 / 1.1 of the box) */
            float d, line;
            cha_shape(type, x, y, &d, &line);
            struct ca_px p = { 0, 0, 0, 0 };
            if (d < aa) {
                ca_lay(&p, d, aa, edge, 1.0f);
                /* the body a rim inside, lit from above: lighter at the top, darker at the foot, a bright edge at the left */
                float shade = (y + 0.8f) / 1.6f;
                uint32_t c = cha_mix(cha_mix(fill, 0xffffff, colour ? 0.16f : 0.30f), cha_mix(fill, 0x000000, colour ? 0.30f : 0.20f), ca_clamp(shade, 0, 1));
                float side = ca_clamp(1.0f - (x + 0.5f) / 0.5f, 0, 1) * ca_clamp(-(d + rim) / 0.12f, 0, 1);
                c = cha_mix(c, 0xffffff, side * (colour ? 0.10f : 0.14f));
                ca_lay(&p, d + rim, aa, c, 1.0f);
                if (line < aa && d + rim < 0)
                    ca_lay(&p, line, aa, detail, colour ? 0.85f : 0.9f);
            }
            out[py * size + px] = ca_finish(&p);
        }
    return out;
}

#endif
