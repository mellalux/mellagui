/*
 * cardart.h - playing cards as pictures, for MellaGUI's card games (header only, no files, no fonts).
 *
 *   cardart_face(rank, suit, w, h)      a malloc'd w x h picture (straight ARGB, the corners are see-through) of one card: rank 1 (Ace) .. 13 (King),
 *                                       suit 0 spades, 1 hearts, 2 diamonds, 3 clubs. The card is 1 wide and 1.4 high in its own units, so any
 *                                       size with that shape is sharp (w : h = 5 : 7).
 *   cardart_back(w, h)                  the back
 *   cardart_suit(suit, size, rgb)       a suit mark alone, size x size, for the empty piles
 *
 * Everything is drawn from distance functions with an anti-aliased edge: no scaled bitmaps. The pips of 2 .. 10 are where they are on a printed card
 * (the lower half turned round), the Ace has one big mark in a laurel of dots, the Jack, Queen and King are busts that are the same upside down
 * (a cap and a halberd, a crown and a flower, a crown and a sceptre). The index at the corner is drawn from strokes (cardart_glyph), so it can be
 * turned round at the other corner whatever font the program has.
 */
#ifndef MELLAGUI_CARDART_H
#define MELLAGUI_CARDART_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { CA_SP, CA_HE, CA_DI, CA_CL };

#define CA_H 1.4f                                           /* the card is 1 wide and CA_H high */

#define CA_INK_RED 0xc6232au
#define CA_INK_BLACK 0x1b1d24u
#define CA_GOLD 0xe2ae28u
#define CA_BLUE 0x2a4fa0u
#define CA_SKIN 0xf2cfaeu
#define CA_PARCH 0xfcf3d6u
#define CA_BROWN 0x5a3a1au
#define CA_FACE 0xfdfcf6u
#define CA_EDGE 0x8c8c84u

/* ---- the canvas of one pixel: colour built up by laying shapes over each other ---- */

struct ca_px {
    float r, g, b, a;                                       /* premultiplied */
};

static inline float ca_clamp(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static inline void ca_over(struct ca_px *p, float cov, uint32_t rgb, float alpha)
{
    float a = ca_clamp(cov, 0, 1) * alpha;
    if (a <= 0)
        return;
    p->r = p->r * (1 - a) + (float)((rgb >> 16) & 255) * a;
    p->g = p->g * (1 - a) + (float)((rgb >> 8) & 255) * a;
    p->b = p->b * (1 - a) + (float)(rgb & 255) * a;
    p->a = p->a * (1 - a) + a;
}

/* coverage of a shape at signed distance d (negative = inside) for a pixel `aa` wide */
static inline float ca_cov(float d, float aa) { return ca_clamp(0.5f - d / aa, 0, 1); }

static inline void ca_lay(struct ca_px *p, float d, float aa, uint32_t rgb, float alpha)
{
    if (d < aa)                                             /* (outside by more than a pixel: nothing) */
        ca_over(p, ca_cov(d, aa), rgb, alpha);
}

/* ---- distance functions ---- */

static inline float ca_circle(float x, float y, float cx, float cy, float r)
{
    return hypotf(x - cx, y - cy) - r;
}

static inline float ca_rbox(float x, float y, float cx, float cy, float hx, float hy, float r)
{
    float qx = fabsf(x - cx) - hx + r, qy = fabsf(y - cy) - hy + r;
    return hypotf(fmaxf(qx, 0), fmaxf(qy, 0)) + fminf(fmaxf(qx, qy), 0) - r;
}

static inline float ca_seg(float x, float y, float ax, float ay, float bx, float by)
{
    float px = x - ax, py = y - ay, dx = bx - ax, dy = by - ay;
    float h = ca_clamp((px * dx + py * dy) / (dx * dx + dy * dy + 1e-9f), 0, 1);
    return hypotf(px - dx * h, py - dy * h);
}

static inline float ca_tri(float x, float y, float ax, float ay, float bx, float by, float cx, float cy)
{
    float d = fminf(fminf(ca_seg(x, y, ax, ay, bx, by), ca_seg(x, y, bx, by, cx, cy)), ca_seg(x, y, cx, cy, ax, ay));
    float s1 = (bx - ax) * (y - ay) - (by - ay) * (x - ax);
    float s2 = (cx - bx) * (y - by) - (cy - by) * (x - bx);
    float s3 = (ax - cx) * (y - cy) - (ay - cy) * (x - cx);
    bool in = (s1 >= 0 && s2 >= 0 && s3 >= 0) || (s1 <= 0 && s2 <= 0 && s3 <= 0);
    return in ? -d : d;
}

/* a polygon of n points: signed distance (negative inside, even-odd) */
static inline float ca_poly(float x, float y, const float *pt, int n)
{
    float d = 1e9f;
    bool in = false;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        float ax = pt[2 * j], ay = pt[2 * j + 1], bx = pt[2 * i], by = pt[2 * i + 1];
        d = fminf(d, ca_seg(x, y, ax, ay, bx, by));
        if (((ay > y) != (by > y)) && (x < (bx - ax) * (y - ay) / (by - ay) + ax))
            in = !in;
    }
    return in ? -d : d;
}

static inline float ca_smin(float a, float b, float k)
{
    float h = ca_clamp(0.5f + 0.5f * (b - a) / k, 0, 1);
    return b + (a - b) * h - k * h * (1 - h);
}

/* ---- the four suits: signed distance in a box of 1 x 1 centred on the origin, y down ---- */

static inline float ca_heart(float x, float y)              /* y down, the lobes up */
{
    float d = ca_smin(ca_circle(x, y, -0.25f, -0.17f, 0.25f), ca_circle(x, y, 0.25f, -0.17f, 0.25f), 0.06f);
    float t = ca_tri(x, y, -0.485f, -0.07f, 0.485f, -0.07f, 0.0f, 0.5f);
    return ca_smin(d, t, 0.04f);
}

static inline float ca_suit_sd(int suit, float x, float y)
{
    switch (suit) {
    case CA_HE: return ca_heart(x, y);
    case CA_DI: {
        float ax = fabsf(x) / 0.40f, ay = fabsf(y) / 0.5f;
        return (ax + ay - 1.0f) * 0.31f;                    /* a rhombus; the factor makes it about a distance */
    }
    case CA_SP: {
        float d = ca_heart(x, -y - 0.02f);                  /* the heart upside down, a little up */
        float stem = ca_tri(x, y, -0.2f, 0.5f, 0.2f, 0.5f, 0.0f, 0.12f);
        return ca_smin(d, stem, 0.05f);
    }
    default: {
        float d = ca_circle(x, y, 0.0f, -0.26f, 0.235f);
        d = ca_smin(d, ca_circle(x, y, -0.27f, 0.07f, 0.235f), 0.04f);
        d = ca_smin(d, ca_circle(x, y, 0.27f, 0.07f, 0.235f), 0.04f);
        d = ca_smin(d, ca_circle(x, y, 0.0f, -0.02f, 0.2f), 0.06f);
        return ca_smin(d, ca_tri(x, y, -0.2f, 0.5f, 0.2f, 0.5f, 0.0f, 0.04f), 0.05f);
    }
    }
}

/* A suit at (cx, cy), `size` wide; flip turns it upside down (the lower half of a card). */
static inline void ca_pip(struct ca_px *p, float u, float v, float aa, int suit, float cx, float cy, float size, bool flip, uint32_t rgb)
{
    float x = (u - cx) / size, y = (v - cy) / size;
    if (fabsf(x) > 0.62f || fabsf(y) > 0.62f)
        return;
    if (flip)
        y = -y;
    ca_lay(p, ca_suit_sd(suit, x, y) * size, aa, rgb, 1.0f);
}

/* ---- the index: letters and figures from strokes ---- */

struct ca_glyph {
    char ch;
    float w;                                                /* width / height of the figure */
    int n;                                                  /* the points: pairs, with 9999 between two strokes */
    float pt[48];
};

#define CA_BREAK 9999.0f

static const struct ca_glyph ca_glyphs[] = {
    { 'A', 0.74f, 6, { 0, 1, 0.5f, 0, 1, 1, CA_BREAK, CA_BREAK, 0.2f, 0.68f, 0.8f, 0.68f } },
    { '1', 0.40f, 3, { 0.05f, 0.22f, 0.6f, 0, 0.6f, 1 } },
    { '2', 0.62f, 7, { 0.04f, 0.22f, 0.2f, 0.04f, 0.55f, 0.0f, 0.9f, 0.14f, 0.92f, 0.38f, 0.0f, 1, 1, 1 } },
    { '3', 0.62f, 9, { 0.05f, 0.1f, 0.5f, 0, 0.92f, 0.14f, 0.9f, 0.38f, 0.45f, 0.5f, 0.95f, 0.64f, 0.95f, 0.88f, 0.5f, 1, 0.04f, 0.9f } },
    { '4', 0.68f, 4, { 0.75f, 1, 0.75f, 0, 0.0f, 0.7f, 1, 0.7f } },
    { '5', 0.62f, 8, { 0.92f, 0, 0.18f, 0, 0.1f, 0.45f, 0.55f, 0.38f, 0.92f, 0.55f, 0.95f, 0.8f, 0.5f, 1, 0.05f, 0.9f } },
    { '6', 0.62f, 11, { 0.85f, 0.05f, 0.5f, 0, 0.15f, 0.2f, 0.05f, 0.6f, 0.15f, 0.9f, 0.5f, 1, 0.9f, 0.88f, 0.95f, 0.64f, 0.7f, 0.46f, 0.3f, 0.5f, 0.07f, 0.66f } },
    { '7', 0.62f, 3, { 0.04f, 0, 0.96f, 0, 0.34f, 1 } },
    { '8', 0.62f, 13, { 0.5f, 0.5f, 0.14f, 0.38f, 0.14f, 0.12f, 0.5f, 0, 0.86f, 0.12f, 0.86f, 0.38f, 0.5f, 0.5f, 0.06f, 0.66f, 0.06f, 0.9f, 0.5f, 1, 0.94f, 0.9f, 0.94f, 0.66f, 0.5f, 0.5f } },
    { '9', 0.62f, 11, { 0.15f, 0.95f, 0.5f, 1, 0.85f, 0.8f, 0.95f, 0.4f, 0.85f, 0.1f, 0.5f, 0, 0.1f, 0.12f, 0.05f, 0.36f, 0.3f, 0.54f, 0.7f, 0.5f, 0.93f, 0.34f } },
    { '0', 0.62f, 9, { 0.5f, 0, 0.12f, 0.18f, 0.05f, 0.5f, 0.12f, 0.82f, 0.5f, 1, 0.88f, 0.82f, 0.95f, 0.5f, 0.88f, 0.18f, 0.5f, 0 } },
    { 'J', 0.55f, 6, { 0.78f, 0, 0.78f, 0.72f, 0.62f, 0.94f, 0.36f, 1, 0.12f, 0.9f, 0.04f, 0.7f } },
    { 'Q', 0.74f, 12, { 0.5f, 0, 0.1f, 0.18f, 0.03f, 0.5f, 0.1f, 0.82f, 0.5f, 1, 0.9f, 0.82f, 0.97f, 0.5f, 0.9f, 0.18f, 0.5f, 0, CA_BREAK, CA_BREAK, 0.55f, 0.72f, 0.98f, 1.08f } },
    { 'K', 0.70f, 8, { 0.08f, 0, 0.08f, 1, CA_BREAK, CA_BREAK, 0.92f, 0, 0.1f, 0.56f, CA_BREAK, CA_BREAK, 0.34f, 0.38f, 0.96f, 1 } },
};

/* signed distance to figure `ch` drawn with its top left at (ox, oy), `h` high, strokes `sw` wide */
static inline float ca_glyph_sd(char ch, float x, float y, float ox, float oy, float h, float sw)
{
    for (size_t g = 0; g < sizeof(ca_glyphs) / sizeof(ca_glyphs[0]); g++) {
        if (ca_glyphs[g].ch != ch)
            continue;
        const struct ca_glyph *gl = &ca_glyphs[g];
        float w = gl->w * h, d = 1e9f;
        if (x < ox - sw || x > ox + w + sw || y < oy - sw || y > oy + h + sw)
            return 1e9f;
        int i = 0, np = 0;
        float lx = 0, ly = 0;
        while (np < gl->n) {
            float px = gl->pt[2 * np], py = gl->pt[2 * np + 1];
            np++;
            if (px == CA_BREAK) {
                i = 0;
                continue;
            }
            float ax = ox + px * w, ay = oy + py * h;
            if (i > 0)
                d = fminf(d, ca_seg(x, y, lx, ly, ax, ay));
            lx = ax;
            ly = ay;
            i++;
        }
        return d - sw / 2;
    }
    return 1e9f;
}

static inline float ca_glyph_w(char ch, float h)
{
    for (size_t g = 0; g < sizeof(ca_glyphs) / sizeof(ca_glyphs[0]); g++)
        if (ca_glyphs[g].ch == ch)
            return ca_glyphs[g].w * h;
    return 0;
}

/* The rank at the corner (top left of the card) at point (u, v); `ink` its colour. */
static inline void ca_index(struct ca_px *p, float u, float v, float aa, int rank, int suit, uint32_t ink)
{
    static const char *const names[] = { "A", "2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K" };
    const char *s = names[rank - 1];
    const float h = 0.15f, sw = 0.026f, gap = 0.025f;
    float total = 0;
    for (const char *c = s; *c; c++)
        total += ca_glyph_w(*c, h) + (c[1] ? gap : 0);
    float cx = 0.115f;                                      /* the figures and the suit under them share a middle */
    float x0 = cx - total / 2;
    for (const char *c = s; *c; c++) {
        float d = ca_glyph_sd(*c, u, v, x0, 0.05f, h, sw);
        ca_lay(p, d, aa, ink, 1.0f);
        x0 += ca_glyph_w(*c, h) + gap;
    }
    ca_pip(p, u, v, aa, suit, cx, 0.05f + h + 0.085f, 0.125f, false, ink);
}

/* ---- the pips of 2 .. 10 ---- */

struct ca_pos {
    float x, y;                                             /* x: 0 left, 1 middle, 2 right; y in rows */
};

/* where the pips are, as (column, row of 0..6 steps from the top pip to the bottom one); rows 0 and 6 are the top and the bottom pip */
static int ca_layout(int rank, float *xs, float *ys)
{
    const float L = 0.31f, C = 0.5f, R = 0.69f;
    const float top = 0.27f, bot = CA_H - 0.27f, mid = CA_H / 2;
    const float q1 = top + (bot - top) / 3, q2 = top + 2 * (bot - top) / 3;
    int n = 0;
#define PIP(x, y) do { xs[n] = (x); ys[n] = (y); n++; } while (0)
    switch (rank) {
    case 2: PIP(C, top); PIP(C, bot); break;
    case 3: PIP(C, top); PIP(C, mid); PIP(C, bot); break;
    case 4: PIP(L, top); PIP(R, top); PIP(L, bot); PIP(R, bot); break;
    case 5: PIP(L, top); PIP(R, top); PIP(C, mid); PIP(L, bot); PIP(R, bot); break;
    case 6: PIP(L, top); PIP(R, top); PIP(L, mid); PIP(R, mid); PIP(L, bot); PIP(R, bot); break;
    case 7: PIP(L, top); PIP(R, top); PIP(C, (top + mid) / 2); PIP(L, mid); PIP(R, mid); PIP(L, bot); PIP(R, bot); break;
    case 8: PIP(L, top); PIP(R, top); PIP(C, (top + mid) / 2); PIP(L, mid); PIP(R, mid); PIP(C, (mid + bot) / 2); PIP(L, bot); PIP(R, bot); break;
    case 9: PIP(L, top); PIP(R, top); PIP(L, q1); PIP(R, q1); PIP(C, mid); PIP(L, q2); PIP(R, q2); PIP(L, bot); PIP(R, bot); break;
    case 10: PIP(L, top); PIP(R, top); PIP(C, (top + q1) / 2); PIP(L, q1); PIP(R, q1); PIP(L, q2); PIP(R, q2); PIP(C, (q2 + bot) / 2); PIP(L, bot); PIP(R, bot); break;
    default: break;
    }
#undef PIP
    return n;
}

/* ---- the Ace ---- */

static void ca_ace(struct ca_px *p, float u, float v, float aa, int suit, uint32_t ink)
{
    const float cx = 0.5f, cy = CA_H / 2;
    for (int k = 0; k < 16; k++) {                          /* a ring of dots round the mark */
        float a = (float)k * 6.2831853f / 16;
        float dx = cx + 0.33f * cosf(a), dy = cy + 0.33f * sinf(a);
        if (fabsf(u - dx) < 0.03f && fabsf(v - dy) < 0.03f)
            ca_lay(p, ca_circle(u, v, dx, dy, (k % 2) ? 0.011f : 0.017f), aa, ink, 0.55f);
    }
    ca_lay(p, fabsf(ca_circle(u, v, cx, cy, 0.385f)) - 0.004f, aa, ink, 0.5f);
    ca_lay(p, fabsf(ca_circle(u, v, cx, cy, 0.285f)) - 0.003f, aa, ink, 0.35f);
    const float size = 0.46f;
    float x = (u - cx) / size, y = (v - cy) / size;
    if (fabsf(x) < 0.7f && fabsf(y) < 0.7f) {
        float d = ca_suit_sd(suit, x, y) * size;
        ca_lay(p, d, aa, ink, 1.0f);
        ca_lay(p, fabsf(d + 0.03f) - 0.004f, aa, 0xffffff, 0.42f);       /* an engraved line just inside the edge */
    }
}

/* ---- the court cards: a bust, drawn for the upper half and used again upside down for the lower ---- */

static float ca_ellipse(float x, float y, float cx, float cy, float rx, float ry)
{
    return (hypotf((x - cx) / rx, (y - cy) / ry) - 1.0f) * fminf(rx, ry);
}

static void ca_court(struct ca_px *p, float u, float v, float aa, int rank, int suit)
{
    bool red = suit == CA_HE || suit == CA_DI;
    uint32_t robe = red ? CA_INK_RED : CA_BLUE, trim = red ? CA_BLUE : CA_INK_RED, gold = CA_GOLD, dark = CA_BROWN;
    if (suit == CA_DI)
        trim = 0x2f8a4a;                                    /* the diamonds wear green with their red */
    if (suit == CA_CL)
        robe = 0x1f3f86;
    /* the robe */
    static const float body[] = { 0.25f, 0.71f, 0.255f, 0.61f, 0.32f, 0.54f, 0.42f, 0.505f, 0.58f, 0.505f, 0.68f, 0.54f, 0.745f, 0.61f, 0.75f, 0.71f };
    ca_lay(p, ca_poly(u, v, body, 8), aa, robe, 1.0f);
    ca_lay(p, fabsf(ca_poly(u, v, body, 8)) - 0.004f, aa, dark, 0.8f);
    ca_lay(p, ca_tri(u, v, 0.41f, 0.505f, 0.59f, 0.505f, 0.5f, 0.64f), aa, gold, 1.0f);           /* the collar */
    ca_lay(p, ca_rbox(u, v, 0.5f, 0.675f, 0.245f, 0.022f, 0.01f), aa, trim, 1.0f);                  /* a band along the hem */
    for (int k = 0; k < 3; k++)                                                                   /* buttons */
        ca_lay(p, ca_circle(u, v, 0.5f, 0.56f + 0.035f * (float)k, 0.009f), aa, dark, 0.9f);
    ca_lay(p, ca_rbox(u, v, 0.5f, 0.485f, 0.04f, 0.04f, 0.01f), aa, CA_SKIN, 1.0f);               /* the neck */
    /* hair and beard behind and under the face */
    if (rank == 12) {                                                                              /* the Queen's long hair */
        static const float hl[] = { 0.41f, 0.34f, 0.375f, 0.43f, 0.355f, 0.55f, 0.43f, 0.52f, 0.44f, 0.42f };
        static const float hr[] = { 0.59f, 0.34f, 0.625f, 0.43f, 0.645f, 0.55f, 0.57f, 0.52f, 0.56f, 0.42f };
        ca_lay(p, ca_poly(u, v, hl, 5), aa, 0xb5792e, 1.0f);
        ca_lay(p, ca_poly(u, v, hr, 5), aa, 0xb5792e, 1.0f);
    }
    /* the face */
    ca_lay(p, ca_circle(u, v, 0.5f, 0.405f, 0.082f), aa, CA_SKIN, 1.0f);
    ca_lay(p, fabsf(ca_circle(u, v, 0.5f, 0.405f, 0.082f)) - 0.003f, aa, dark, 0.8f);
    ca_lay(p, ca_circle(u, v, 0.468f, 0.395f, 0.0085f), aa, dark, 1.0f);
    ca_lay(p, ca_circle(u, v, 0.532f, 0.395f, 0.0085f), aa, dark, 1.0f);
    ca_lay(p, ca_seg(u, v, 0.48f, 0.438f, 0.52f, 0.438f) - 0.004f, aa, 0x9a3a2a, 1.0f);
    ca_lay(p, ca_circle(u, v, 0.452f, 0.425f, 0.014f), aa, 0xe99a8a, 0.4f);                         /* cheeks */
    ca_lay(p, ca_circle(u, v, 0.548f, 0.425f, 0.014f), aa, 0xe99a8a, 0.4f);
    if (rank == 13) {                                                                              /* the King's beard and moustache */
        static const float beard[] = { 0.425f, 0.43f, 0.575f, 0.43f, 0.56f, 0.5f, 0.5f, 0.545f, 0.44f, 0.5f };
        ca_lay(p, ca_poly(u, v, beard, 5), aa, 0xe6e0d2, 1.0f);
        ca_lay(p, fabsf(ca_poly(u, v, beard, 5)) - 0.003f, aa, dark, 0.6f);
        ca_lay(p, ca_ellipse(u, v, 0.5f, 0.425f, 0.045f, 0.012f), aa, 0xcfc8b8, 1.0f);
    }
    if (rank == 11) {                                                                              /* the Jack's short hair */
        ca_lay(p, ca_circle(u, v, 0.41f, 0.405f, 0.03f), aa, 0x7a4a1c, 1.0f);
        ca_lay(p, ca_circle(u, v, 0.59f, 0.405f, 0.03f), aa, 0x7a4a1c, 1.0f);
    }
    /* what is on the head */
    if (rank == 13 || rank == 12) {
        float h = rank == 13 ? 0.075f : 0.06f;
        float pts[] = { 0.415f, 0.33f, 0.41f, 0.33f - h, 0.457f, 0.33f - h * 0.45f, 0.5f, 0.33f - h * 1.25f, 0.543f, 0.33f - h * 0.45f, 0.59f, 0.33f - h, 0.585f, 0.33f };
        ca_lay(p, ca_poly(u, v, pts, 7), aa, gold, 1.0f);
        ca_lay(p, fabsf(ca_poly(u, v, pts, 7)) - 0.003f, aa, dark, 0.8f);
        ca_lay(p, ca_rbox(u, v, 0.5f, 0.335f, 0.088f, 0.014f, 0.006f), aa, 0xf4cc55, 1.0f);
        ca_lay(p, ca_circle(u, v, 0.5f, 0.335f, 0.009f), aa, trim, 1.0f);
        ca_lay(p, ca_circle(u, v, 0.5f, 0.33f - h * 1.25f - 0.012f, 0.013f), aa, trim, 1.0f);       /* a jewel on the top */
        if (rank == 13) {
            ca_lay(p, ca_circle(u, v, 0.41f, 0.33f - h - 0.01f, 0.011f), aa, trim, 1.0f);
            ca_lay(p, ca_circle(u, v, 0.59f, 0.33f - h - 0.01f, 0.011f), aa, trim, 1.0f);
        }
    } else {                                                                                       /* a cap with a feather */
        ca_lay(p, ca_ellipse(u, v, 0.5f, 0.31f, 0.093f, 0.062f), aa, trim, 1.0f);
        ca_lay(p, fabsf(ca_ellipse(u, v, 0.5f, 0.31f, 0.093f, 0.062f)) - 0.003f, aa, dark, 0.8f);
        ca_lay(p, ca_rbox(u, v, 0.5f, 0.337f, 0.098f, 0.014f, 0.007f), aa, gold, 1.0f);
        ca_lay(p, ca_seg(u, v, 0.58f, 0.285f, 0.7f, 0.205f) - 0.008f, aa, gold, 1.0f);             /* the feather */
        ca_lay(p, ca_seg(u, v, 0.585f, 0.285f, 0.665f, 0.23f) - 0.004f, aa, 0xf4cc55, 1.0f);
    }
    /* what is held: a sceptre, a flower, a halberd */
    if (rank == 13) {
        ca_lay(p, ca_seg(u, v, 0.705f, 0.46f, 0.705f, 0.7f) - 0.009f, aa, gold, 1.0f);
        ca_lay(p, ca_circle(u, v, 0.705f, 0.44f, 0.026f), aa, gold, 1.0f);
        ca_lay(p, ca_circle(u, v, 0.705f, 0.44f, 0.012f), aa, trim, 1.0f);
    } else if (rank == 12) {
        ca_lay(p, ca_seg(u, v, 0.69f, 0.52f, 0.69f, 0.7f) - 0.005f, aa, 0x2f8a4a, 1.0f);
        for (int k = 0; k < 5; k++) {
            float a = (float)k * 6.2831853f / 5;
            ca_lay(p, ca_circle(u, v, 0.69f + 0.022f * cosf(a), 0.49f + 0.022f * sinf(a), 0.016f), aa, CA_INK_RED, 1.0f);
        }
        ca_lay(p, ca_circle(u, v, 0.69f, 0.49f, 0.012f), aa, gold, 1.0f);
    } else {
        ca_lay(p, ca_seg(u, v, 0.70f, 0.36f, 0.70f, 0.7f) - 0.007f, aa, 0x7a5a30, 1.0f);
        ca_lay(p, ca_tri(u, v, 0.70f, 0.37f, 0.76f, 0.43f, 0.70f, 0.49f), aa, 0xc9ced6, 1.0f);       /* the blade */
        ca_lay(p, ca_tri(u, v, 0.70f, 0.37f, 0.67f, 0.34f, 0.70f, 0.31f), aa, 0xc9ced6, 1.0f);
    }
}

/* ---- the whole card ---- */

static inline uint32_t ca_finish(const struct ca_px *p)
{
    if (p->a <= 0.0005f)
        return 0;
    float inv = 1.0f / p->a;
    uint32_t a = (uint32_t)(ca_clamp(p->a, 0, 1) * 255 + 0.5f);
    uint32_t r = (uint32_t)ca_clamp(p->r * inv + 0.5f, 0, 255), g = (uint32_t)ca_clamp(p->g * inv + 0.5f, 0, 255), b = (uint32_t)ca_clamp(p->b * inv + 0.5f, 0, 255);
    return a << 24 | r << 16 | g << 8 | b;
}

static void ca_body(struct ca_px *p, float u, float v, float aa)
{
    float d = ca_rbox(u, v, 0.5f, CA_H / 2, 0.5f, CA_H / 2, 0.075f);
    ca_lay(p, d, aa, CA_EDGE, 1.0f);                        /* the thin edge, then the face a hair inside */
    ca_lay(p, d + 0.008f, aa, CA_FACE, 1.0f);
}

static uint32_t *cardart_face(int rank, int suit, int w, int h)
{
    uint32_t *out = malloc((size_t)w * (size_t)h * 4);
    if (!out)
        return NULL;
    uint32_t ink = suit == CA_HE || suit == CA_DI ? CA_INK_RED : CA_INK_BLACK;
    float xs[12], ys[12];
    int np = rank >= 2 && rank <= 10 ? ca_layout(rank, xs, ys) : 0;
    float aa = 1.0f / (float)w, psize = rank == 1 ? 0 : 0.2f;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float u = ((float)x + 0.5f) * aa, v = ((float)y + 0.5f) * aa;
            struct ca_px p = { 0, 0, 0, 0 };
            ca_body(&p, u, v, aa);
            if (p.a > 0) {
                ca_index(&p, u, v, aa, rank, suit, ink);                             /* the corner index, and the same turned round at the other corner */
                ca_index(&p, 1.0f - u, CA_H - v, aa, rank, suit, ink);
                if (rank == 1) {
                    ca_ace(&p, u, v, aa, suit, ink);
                } else if (rank <= 10) {
                    for (int k = 0; k < np; k++)
                        ca_pip(&p, u, v, aa, suit, xs[k], ys[k], psize, ys[k] > CA_H / 2 + 0.01f, ink);
                } else {                                                             /* a court card: a frame with the bust in it, twice */
                    float fd = ca_rbox(u, v, 0.5f, CA_H / 2, 0.285f, 0.49f, 0.02f);
                    ca_lay(&p, fd, aa, CA_PARCH, 1.0f);
                    ca_lay(&p, fabsf(fd + 0.004f) - 0.004f, aa, ink, 1.0f);
                    ca_lay(&p, fabsf(v - CA_H / 2) - 0.003f, aa, ink, fd < 0 ? 0.8f : 0.0f);
                    if (fd < 0) {
                        if (v <= CA_H / 2)
                            ca_court(&p, u, v, aa, rank, suit);
                        else
                            ca_court(&p, 1.0f - u, CA_H - v, aa, rank, suit);
                        float cd = ca_rbox(u, v, 0.5f, CA_H / 2, 0.285f, 0.49f, 0.02f);
                        ca_lay(&p, fabsf(cd + 0.004f) - 0.004f, aa, ink, 1.0f);
                    }
                }
            }
            out[(size_t)y * (size_t)w + (size_t)x] = ca_finish(&p);
        }
    }
    return out;
}

static uint32_t *cardart_back(int w, int h)
{
    uint32_t *out = malloc((size_t)w * (size_t)h * 4);
    if (!out)
        return NULL;
    const uint32_t BLUE = 0x1f4aa0, LIGHT = 0x4f7fd6, DEEP = 0x143578;
    float aa = 1.0f / (float)w;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float u = ((float)x + 0.5f) * aa, v = ((float)y + 0.5f) * aa;
            struct ca_px p = { 0, 0, 0, 0 };
            ca_body(&p, u, v, aa);
            if (p.a > 0) {
                float inner = ca_rbox(u, v, 0.5f, CA_H / 2, 0.455f, CA_H / 2 - 0.045f, 0.045f);
                ca_lay(&p, inner, aa, BLUE, 1.0f);
                /* a lattice of diamonds */
                float a = fabsf(fmodf((u + v) * 7.0f, 1.0f) - 0.5f), b = fabsf(fmodf((u - v) * 7.0f + 100.0f, 1.0f) - 0.5f);
                float line = fminf(a, b) / 7.0f * 1.2f;
                ca_lay(&p, line - 0.0045f, aa, LIGHT, inner < 0 ? 0.55f : 0.0f);
                ca_lay(&p, fabsf(inner + 0.022f) - 0.006f, aa, 0xf4f1e6, 1.0f);       /* a pale line inside the edge */
                /* the medallion */
                float cx = 0.5f, cy = CA_H / 2, r = hypotf(u - cx, v - cy);
                ca_lay(&p, r - 0.215f, aa, DEEP, 1.0f);
                ca_lay(&p, fabsf(r - 0.2f) - 0.008f, aa, CA_GOLD, 1.0f);
                ca_lay(&p, fabsf(r - 0.175f) - 0.003f, aa, 0xf4f1e6, 0.85f);
                for (int k = 0; k < 8; k++) {                                          /* an eight pointed star */
                    float ang = (float)k * 6.2831853f / 8 + 0.3927f, ang2 = ang + 0.3927f;
                    float tip[] = { cx, cy, cx + 0.15f * cosf(ang), cy + 0.15f * sinf(ang), cx + 0.045f * cosf(ang2), cy + 0.045f * sinf(ang2) };
                    ca_lay(&p, ca_tri(u, v, tip[0], tip[1], tip[2], tip[3], tip[4], tip[5]), aa, CA_GOLD, 1.0f);
                }
                ca_lay(&p, r - 0.03f, aa, 0xf4f1e6, 1.0f);
            }
            out[(size_t)y * (size_t)w + (size_t)x] = ca_finish(&p);
        }
    }
    return out;
}

static uint32_t *cardart_suit(int suit, int size, uint32_t rgb)
{
    uint32_t *out = malloc((size_t)size * (size_t)size * 4);
    if (!out)
        return NULL;
    float aa = 1.0f / (float)size;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            float u = ((float)x + 0.5f) * aa - 0.5f, v = ((float)y + 0.5f) * aa - 0.5f;
            struct ca_px p = { 0, 0, 0, 0 };
            ca_lay(&p, ca_suit_sd(suit, u, v), aa, rgb, 1.0f);
            out[(size_t)y * (size_t)size + (size_t)x] = ca_finish(&p);
        }
    }
    return out;
}

#endif
