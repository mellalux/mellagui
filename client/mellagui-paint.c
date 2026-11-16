/*
 * mellagui-paint - a simple picture editor.
 *
 *   mellagui-paint [FILE]                     open a picture (PNG, JPEG, GIF, BMP, WebP, SVG ...), or an empty one
 *   mellagui-paint --new WxH [FILE]           a white picture of that size (FILE is where Save puts it)
 *   mellagui-paint --size WxH ...             the size the window opens with (default 1000x680)
 *   mellagui-paint --apply IN OUT OP...       no window: the operations in order, the result to OUT (.png / .jpg)
 *                 OP = rotate-left rotate-right flip-h flip-v resize=WxH resize=PCT% crop=X,Y,W,H gray sepia invert brighter darker
 *                      more-contrast less-contrast blur sharpen
 *   mellagui-paint --selftest
 *
 * The window has the look of the other programs: a rail of round drop buttons at the left (the tools) and one at the right (file, undo, turn, flip,
 * size, effects), the colours and the brush sizes in a strip under the picture, the status line at the very bottom.
 *
 *   Tools      Select, Pen, Line, Box, Circle, Fill, Eraser, Picker  (S P L B C F E I)   Shift keeps a line at 45 degrees and a box / circle square
 *   Colours    left click = the pen colour, right click = the second colour (the right button draws with it); the first square is "transparent"
 *              (drawing with it erases);  X swaps the two
 *   Brush      the six dots in the strip, [ and ] a size smaller / bigger;  "Filled" makes boxes and circles solid
 *   Picture    Ctrl+O open, Ctrl+N new, Ctrl+S save, Ctrl+Shift+S save as (.png keeps transparency, .jpg is flattened on white), Ctrl+Z undo,
 *              Ctrl+Y or Ctrl+Shift+Z redo, Enter crops to the selection, Delete clears it, Ctrl+A selects all, Esc takes the selection away
 *   View       wheel zooms, middle button drags, + - zoom, 0 fits the window, 1 is the real size, the arrow keys move the picture
 *   Effects    the menu button of the right rail (grayscale, sepia, invert, brighter, darker, contrast, blur, sharpen): on the selection if there is one
 *   Transform  the other menu button: turn left / right (Ctrl+Left / Right), flip, resize (Ctrl+R: a size, a width or a percentage)
 *
 * Colours of the strip, the rails and the cards come from the theme (like the other programs); the pictures' own colours are the picture's.
 * `$MELLAGUI_PAINT_DUMP=FILE` writes the state after every paint (tests).
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "deflate.h"
#include "font.h"
#include "img.h"
#include "jpegenc.h"
#include "keymap.h"
#include "metrics.h"
#include "pathcomp.h"
#include "protocol.h"
#include "theme.h"
#include "uikit.h"
#include "util.h"
#include "wlc.h"
#include "wlclip.h"
#include "wlcard.h"
#include "wlpicker.h"
#include "wlrail.h"
#include "wltext.h"
#include "wlwin.h"

#define MAX_UNDO 32
#define UNDO_BYTES (768u * 1024u * 1024u)
#define MAX_SIDE 16384
#define PAL_H 34
#define STAT_H 22
#define JPEG_QUALITY 90

/* ---- the picture ----------------------------------------------------------------------------------------------------------------- */

/* ARGB, the colour NOT premultiplied (alpha in the top byte), w * h pixels. */
struct pix {
    uint32_t *p;
    int w, h;
};

struct irect {
    int x0, y0, x1, y1;                 /* x1 / y1 exclusive */
};

static bool pix_alloc(struct pix *im, int w, int h, uint32_t fill)
{
    if (w < 1 || h < 1 || w > MAX_SIDE || h > MAX_SIDE || (uint64_t)w * (uint64_t)h > LM_IMG_MAX_PIXELS)
        return false;
    uint32_t *p = malloc((size_t)w * (size_t)h * 4);
    if (!p)
        return false;
    for (size_t i = 0; i < (size_t)w * (size_t)h; i++)
        p[i] = fill;
    im->p = p;
    im->w = w;
    im->h = h;
    return true;
}

static void pix_free(struct pix *im)
{
    free(im->p);
    im->p = NULL;
    im->w = im->h = 0;
}

static bool pix_copy(const struct pix *src, struct pix *dst)
{
    if (!pix_alloc(dst, src->w, src->h, 0))
        return false;
    memcpy(dst->p, src->p, (size_t)src->w * (size_t)src->h * 4);
    return true;
}

static int ch_a(uint32_t c) { return (int)(c >> 24); }
static int ch_r(uint32_t c) { return (int)((c >> 16) & 255); }
static int ch_g(uint32_t c) { return (int)((c >> 8) & 255); }
static int ch_b(uint32_t c) { return (int)(c & 255); }
static uint32_t argb(int a, int r, int g, int b) { return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b; }
static int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

/* `src` laid over `dst` with `cov` (0..255) more coverage. A source with no alpha at all erases: the destination goes towards transparent. */
static uint32_t blend(uint32_t dst, uint32_t src, int cov)
{
    if (cov <= 0)
        return dst;
    int sa = ch_a(src) * cov / 255, da = ch_a(dst);
    if (ch_a(src) == 0) {
        int na = da * (255 - cov) / 255;
        return na ? argb(na, ch_r(dst), ch_g(dst), ch_b(dst)) : 0;
    }
    if (sa >= 255)
        return src | 0xff000000u;
    int oa = sa + da * (255 - sa) / 255;
    if (oa <= 0)
        return 0;
    int wd = da * (255 - sa) / 255;
    return argb(oa, (ch_r(src) * sa + ch_r(dst) * wd) / oa, (ch_g(src) * sa + ch_g(dst) * wd) / oa, (ch_b(src) * sa + ch_b(dst) * wd) / oa);
}

static void irect_add(struct irect *r, int x0, int y0, int x1, int y1)
{
    if (r->x1 <= r->x0 || r->y1 <= r->y0) {
        *r = (struct irect){ x0, y0, x1, y1 };
        return;
    }
    r->x0 = LM_MIN(r->x0, x0);
    r->y0 = LM_MIN(r->y0, y0);
    r->x1 = LM_MAX(r->x1, x1);
    r->y1 = LM_MAX(r->y1, y1);
}

/* A line with round ends, anti-aliased, `r` the radius in pixels. */
static void stroke(struct pix *im, double x0, double y0, double x1, double y1, double r, uint32_t col, struct irect *dirty)
{
    int bx0 = (int)floor(LM_MIN(x0, x1) - r - 1.0), by0 = (int)floor(LM_MIN(y0, y1) - r - 1.0);
    int bx1 = (int)ceil(LM_MAX(x0, x1) + r + 1.0), by1 = (int)ceil(LM_MAX(y0, y1) + r + 1.0);
    bx0 = LM_MAX(bx0, 0);
    by0 = LM_MAX(by0, 0);
    bx1 = LM_MIN(bx1, im->w);
    by1 = LM_MIN(by1, im->h);
    if (bx1 <= bx0 || by1 <= by0)
        return;
    double dx = x1 - x0, dy = y1 - y0, len2 = dx * dx + dy * dy;
    for (int y = by0; y < by1; y++)
        for (int x = bx0; x < bx1; x++) {
            double px = x + 0.5 - x0, py = y + 0.5 - y0;
            double t = len2 > 0 ? (px * dx + py * dy) / len2 : 0;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            double ex = px - t * dx, ey = py - t * dy, d = sqrt(ex * ex + ey * ey);
            double cov = r + 0.5 - d;
            if (cov <= 0)
                continue;
            uint32_t *q = &im->p[(size_t)y * (size_t)im->w + (size_t)x];
            *q = blend(*q, col, cov >= 1 ? 255 : (int)(cov * 255));
        }
    if (dirty)
        irect_add(dirty, bx0, by0, bx1, by1);
}

static void fill_rect(struct pix *im, int x0, int y0, int x1, int y1, uint32_t col, struct irect *dirty)
{
    x0 = LM_MAX(x0, 0);
    y0 = LM_MAX(y0, 0);
    x1 = LM_MIN(x1, im->w);
    y1 = LM_MIN(y1, im->h);
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            uint32_t *q = &im->p[(size_t)y * (size_t)im->w + (size_t)x];
            *q = blend(*q, col, 255);
        }
    if (dirty && x1 > x0 && y1 > y0)
        irect_add(dirty, x0, y0, x1, y1);
}

static void rect_outline(struct pix *im, int x0, int y0, int x1, int y1, double r, uint32_t col, struct irect *dirty)
{
    double a = LM_MIN(x0, x1) + 0.5, b = LM_MIN(y0, y1) + 0.5, e = LM_MAX(x0, x1) + 0.5, f = LM_MAX(y0, y1) + 0.5;
    stroke(im, a, b, e, b, r, col, dirty);
    stroke(im, e, b, e, f, r, col, dirty);
    stroke(im, e, f, a, f, r, col, dirty);
    stroke(im, a, f, a, b, r, col, dirty);
}

static void ellipse(struct pix *im, int x0, int y0, int x1, int y1, double r, bool filled, uint32_t col, struct irect *dirty)
{
    double cx = (LM_MIN(x0, x1) + LM_MAX(x0, x1) + 1) / 2.0, cy = (LM_MIN(y0, y1) + LM_MAX(y0, y1) + 1) / 2.0;
    double rx = (LM_MAX(x0, x1) - LM_MIN(x0, x1) + 1) / 2.0, ry = (LM_MAX(y0, y1) - LM_MIN(y0, y1) + 1) / 2.0;
    if (filled) {
        int bx0 = LM_MAX((int)floor(cx - rx - 1), 0), by0 = LM_MAX((int)floor(cy - ry - 1), 0);
        int bx1 = LM_MIN((int)ceil(cx + rx + 1), im->w), by1 = LM_MIN((int)ceil(cy + ry + 1), im->h);
        double m = LM_MIN(rx, ry);
        for (int y = by0; y < by1; y++)
            for (int x = bx0; x < bx1; x++) {
                double nx = (x + 0.5 - cx) / rx, ny = (y + 0.5 - cy) / ry, q = sqrt(nx * nx + ny * ny);
                double cov = (1.0 - q) * m + 0.5;
                if (cov <= 0)
                    continue;
                uint32_t *p = &im->p[(size_t)y * (size_t)im->w + (size_t)x];
                *p = blend(*p, col, cov >= 1 ? 255 : (int)(cov * 255));
            }
        if (dirty && bx1 > bx0 && by1 > by0)
            irect_add(dirty, bx0, by0, bx1, by1);
        return;
    }
    rx = LM_MAX(rx - r, 0.5);                                           /* the outline lies inside the box: its middle line is half a brush in */
    ry = LM_MAX(ry - r, 0.5);
    int n = LM_CLAMP((int)(4.0 * (rx + ry)), 32, 2048);
    double px = cx + rx, py = cy;
    for (int i = 1; i <= n; i++) {
        double t = 2.0 * M_PI * i / n, qx = cx + rx * cos(t), qy = cy + ry * sin(t);
        stroke(im, px, py, qx, qy, r, col, dirty);
        px = qx;
        py = qy;
    }
}

/* Replaces the connected area of nearly the same colour with `col`. */
static bool flood(struct pix *im, int sx, int sy, uint32_t col, int tol, struct irect *dirty)
{
    if (sx < 0 || sy < 0 || sx >= im->w || sy >= im->h)
        return false;
    uint32_t target = im->p[(size_t)sy * (size_t)im->w + (size_t)sx];
    if (target == col && tol == 0)
        return false;
    size_t total = (size_t)im->w * (size_t)im->h;
    uint8_t *seen = calloc(total, 1);
    size_t cap = 1024, n = 0;
    int *stack = malloc(cap * 2 * sizeof(int));
    if (!seen || !stack) {
        free(seen);
        free(stack);
        return false;
    }
#define NEAR(c) (abs(ch_a(c) - ch_a(target)) <= tol && abs(ch_r(c) - ch_r(target)) <= tol && abs(ch_g(c) - ch_g(target)) <= tol && abs(ch_b(c) - ch_b(target)) <= tol)
    stack[0] = sx;
    stack[1] = sy;
    n = 1;
    while (n) {
        int x = stack[(n - 1) * 2], y = stack[(n - 1) * 2 + 1];
        n--;
        if (seen[(size_t)y * (size_t)im->w + (size_t)x] || !NEAR(im->p[(size_t)y * (size_t)im->w + (size_t)x]))
            continue;
        int l = x, r = x;
        while (l > 0 && !seen[(size_t)y * (size_t)im->w + (size_t)(l - 1)] && NEAR(im->p[(size_t)y * (size_t)im->w + (size_t)(l - 1)]))
            l--;
        while (r < im->w - 1 && !seen[(size_t)y * (size_t)im->w + (size_t)(r + 1)] && NEAR(im->p[(size_t)y * (size_t)im->w + (size_t)(r + 1)]))
            r++;
        for (int i = l; i <= r; i++) {
            seen[(size_t)y * (size_t)im->w + (size_t)i] = 1;
            im->p[(size_t)y * (size_t)im->w + (size_t)i] = col;
        }
        irect_add(dirty, l, y, r + 1, y + 1);
        for (int dy = -1; dy <= 1; dy += 2) {
            int yy = y + dy;
            if (yy < 0 || yy >= im->h)
                continue;
            bool run = false;
            for (int i = l; i <= r; i++) {
                bool ok = !seen[(size_t)yy * (size_t)im->w + (size_t)i] && NEAR(im->p[(size_t)yy * (size_t)im->w + (size_t)i]);
                if (ok && !run) {
                    if (n + 1 > cap) {
                        cap *= 2;
                        int *ns = realloc(stack, cap * 2 * sizeof(int));
                        if (!ns) {
                            free(seen);
                            free(stack);
                            return true;
                        }
                        stack = ns;
                    }
                    stack[n * 2] = i;
                    stack[n * 2 + 1] = yy;
                    n++;
                }
                run = ok;
            }
        }
    }
#undef NEAR
    free(seen);
    free(stack);
    return true;
}

/* ---- effects and changes of the whole picture ------------------------------------------------------------------------------------ */

enum fx { FX_GRAY, FX_SEPIA, FX_INVERT, FX_BRIGHTER, FX_DARKER, FX_MORE_CONTRAST, FX_LESS_CONTRAST, FX_BLUR, FX_SHARPEN, FX_COUNT };
static const char *const FX_NAMES[FX_COUNT] = { "Grayscale", "Sepia", "Invert", "Brighter", "Darker", "More contrast", "Less contrast", "Blur", "Sharpen" };
static const char *const FX_OPS[FX_COUNT] = { "gray", "sepia", "invert", "brighter", "darker", "more-contrast", "less-contrast", "blur", "sharpen" };

static void clip_rect(const struct pix *im, struct irect *r)
{
    r->x0 = LM_CLAMP(r->x0, 0, im->w);
    r->y0 = LM_CLAMP(r->y0, 0, im->h);
    r->x1 = LM_CLAMP(r->x1, r->x0, im->w);
    r->y1 = LM_CLAMP(r->y1, r->y0, im->h);
}

/* A 3 x 3 filter over the region (the edge pixels repeat); weights sum to `div`. */
static bool convolve(struct pix *im, struct irect r, const int k[9], int div, int bias)
{
    clip_rect(im, &r);
    if (r.x1 <= r.x0 || r.y1 <= r.y0)
        return true;
    struct pix src;
    if (!pix_copy(im, &src))
        return false;
    for (int y = r.y0; y < r.y1; y++)
        for (int x = r.x0; x < r.x1; x++) {
            int sa = 0, sr = 0, sg = 0, sb = 0, wsum = 0;
            for (int j = -1; j <= 1; j++)
                for (int i = -1; i <= 1; i++) {
                    int xx = LM_CLAMP(x + i, 0, im->w - 1), yy = LM_CLAMP(y + j, 0, im->h - 1);
                    uint32_t c = src.p[(size_t)yy * (size_t)im->w + (size_t)xx];
                    int wt = k[(j + 1) * 3 + (i + 1)], a = ch_a(c);
                    sa += a * wt;
                    sr += ch_r(c) * a * wt;                              /* colours count by their alpha, so a see-through edge does not bleed black */
                    sg += ch_g(c) * a * wt;
                    sb += ch_b(c) * a * wt;
                    wsum += wt;
                }
            (void)wsum;
            int oa = clamp255(sa / div + bias);
            uint32_t o = 0;
            if (sa > 0 && oa > 0) {
                int denom = sa > 0 ? sa : 1;
                o = argb(oa, clamp255(sr / denom), clamp255(sg / denom), clamp255(sb / denom));
            }
            im->p[(size_t)y * (size_t)im->w + (size_t)x] = o;
        }
    pix_free(&src);
    return true;
}

static bool apply_fx(struct pix *im, enum fx f, struct irect r)
{
    clip_rect(im, &r);
    if (f == FX_BLUR) {
        static const int k[9] = { 1, 2, 1, 2, 4, 2, 1, 2, 1 };
        return convolve(im, r, k, 16, 0);
    }
    if (f == FX_SHARPEN) {
        static const int k[9] = { 0, -1, 0, -1, 5, -1, 0, -1, 0 };
        return convolve(im, r, k, 1, 0);
    }
    for (int y = r.y0; y < r.y1; y++)
        for (int x = r.x0; x < r.x1; x++) {
            uint32_t *q = &im->p[(size_t)y * (size_t)im->w + (size_t)x];
            int a = ch_a(*q), R = ch_r(*q), G = ch_g(*q), B = ch_b(*q);
            switch (f) {
            case FX_GRAY: R = G = B = (R * 299 + G * 587 + B * 114) / 1000; break;
            case FX_SEPIA: {
                int nr = (R * 393 + G * 769 + B * 189) / 1000, ng = (R * 349 + G * 686 + B * 168) / 1000, nb = (R * 272 + G * 534 + B * 131) / 1000;
                R = clamp255(nr);
                G = clamp255(ng);
                B = clamp255(nb);
                break;
            }
            case FX_INVERT: R = 255 - R; G = 255 - G; B = 255 - B; break;
            case FX_BRIGHTER: R = clamp255(R + 24); G = clamp255(G + 24); B = clamp255(B + 24); break;
            case FX_DARKER: R = clamp255(R - 24); G = clamp255(G - 24); B = clamp255(B - 24); break;
            case FX_MORE_CONTRAST: R = clamp255((R - 128) * 13 / 10 + 128); G = clamp255((G - 128) * 13 / 10 + 128); B = clamp255((B - 128) * 13 / 10 + 128); break;
            case FX_LESS_CONTRAST: R = clamp255((R - 128) * 7 / 10 + 128); G = clamp255((G - 128) * 7 / 10 + 128); B = clamp255((B - 128) * 7 / 10 + 128); break;
            default: break;
            }
            *q = argb(a, R, G, B);
        }
    return true;
}

static bool rotate90(struct pix *im, bool clockwise)
{
    struct pix out;
    if (!pix_alloc(&out, im->h, im->w, 0))
        return false;
    for (int y = 0; y < im->h; y++)
        for (int x = 0; x < im->w; x++) {
            int nx = clockwise ? im->h - 1 - y : y, ny = clockwise ? x : im->w - 1 - x;
            out.p[(size_t)ny * (size_t)out.w + (size_t)nx] = im->p[(size_t)y * (size_t)im->w + (size_t)x];
        }
    pix_free(im);
    *im = out;
    return true;
}

static void flip(struct pix *im, bool horizontal)
{
    if (horizontal) {
        for (int y = 0; y < im->h; y++)
            for (int x = 0; x < im->w / 2; x++) {
                uint32_t *a = &im->p[(size_t)y * (size_t)im->w + (size_t)x], *b = &im->p[(size_t)y * (size_t)im->w + (size_t)(im->w - 1 - x)], t = *a;
                *a = *b;
                *b = t;
            }
    } else {
        for (int y = 0; y < im->h / 2; y++)
            for (int x = 0; x < im->w; x++) {
                uint32_t *a = &im->p[(size_t)y * (size_t)im->w + (size_t)x], *b = &im->p[(size_t)(im->h - 1 - y) * (size_t)im->w + (size_t)x], t = *a;
                *a = *b;
                *b = t;
            }
    }
}

static bool crop(struct pix *im, struct irect r)
{
    clip_rect(im, &r);
    int w = r.x1 - r.x0, h = r.y1 - r.y0;
    struct pix out;
    if (w < 1 || h < 1 || !pix_alloc(&out, w, h, 0))
        return false;
    for (int y = 0; y < h; y++)
        memcpy(&out.p[(size_t)y * (size_t)w], &im->p[(size_t)(y + r.y0) * (size_t)im->w + (size_t)r.x0], (size_t)w * 4);
    pix_free(im);
    *im = out;
    return true;
}

/* One axis of a resize: for every output index the source indices and weights (area average when it gets smaller, linear when it gets bigger). */
struct taps {
    int first, n;
    float *w;
};

static bool make_taps(int in, int out, struct taps **res)
{
    struct taps *t = calloc((size_t)out, sizeof(*t));
    if (!t)
        return false;
    double scale = (double)in / out;
    for (int i = 0; i < out; i++) {
        if (scale > 1.0) {                                              /* the source cells that the output cell covers, by area */
            double a = i * scale, b = (i + 1) * scale;
            int f = (int)floor(a), l = (int)ceil(b) - 1;
            f = LM_MAX(f, 0);
            l = LM_MIN(l, in - 1);
            t[i].first = f;
            t[i].n = l - f + 1;
            t[i].w = malloc((size_t)t[i].n * sizeof(float));
            if (!t[i].w) {
                for (int j = 0; j < i; j++)
                    free(t[j].w);
                free(t);
                return false;
            }
            double sum = 0;
            for (int k = 0; k < t[i].n; k++) {
                double lo = LM_MAX((double)(f + k), a), hi = LM_MIN((double)(f + k + 1), b);
                double wgt = hi > lo ? hi - lo : 0;
                t[i].w[k] = (float)wgt;
                sum += wgt;
            }
            for (int k = 0; k < t[i].n; k++)
                t[i].w[k] = (float)(t[i].w[k] / (sum > 0 ? sum : 1));
        } else {                                                        /* linear between the two nearest cell centres */
            double c = (i + 0.5) * scale - 0.5;
            int f = (int)floor(c);
            double fr = c - f;
            int a = LM_CLAMP(f, 0, in - 1), b = LM_CLAMP(f + 1, 0, in - 1);
            t[i].first = a;
            t[i].n = b > a ? 2 : 1;
            t[i].w = malloc(2 * sizeof(float));
            if (!t[i].w) {
                for (int j = 0; j < i; j++)
                    free(t[j].w);
                free(t);
                return false;
            }
            if (b > a) {
                t[i].w[0] = (float)(1.0 - fr);
                t[i].w[1] = (float)fr;
            } else {
                t[i].w[0] = 1.0f;
            }
        }
    }
    *res = t;
    return true;
}

static void free_taps(struct taps *t, int n)
{
    for (int i = 0; i < n; i++)
        free(t[i].w);
    free(t);
}

/* Premultiplied sums, so a transparent edge does not darken the colours near it. */
static bool resize(struct pix *im, int nw, int nh)
{
    struct pix out;
    struct taps *tx, *ty;
    if (nw < 1 || nh < 1 || nw > MAX_SIDE || nh > MAX_SIDE || (uint64_t)nw * (uint64_t)nh > LM_IMG_MAX_PIXELS)
        return false;
    if (nw == im->w && nh == im->h)
        return true;
    if (!make_taps(im->w, nw, &tx))
        return false;
    if (!make_taps(im->h, nh, &ty)) {
        free_taps(tx, nw);
        return false;
    }
    float *mid = malloc((size_t)nw * (size_t)im->h * 4 * sizeof(float));                 /* after the horizontal pass: premultiplied r g b a */
    bool ok = mid && pix_alloc(&out, nw, nh, 0);
    if (ok) {
        for (int y = 0; y < im->h; y++)
            for (int x = 0; x < nw; x++) {
                float a = 0, r = 0, g = 0, b = 0;
                for (int k = 0; k < tx[x].n; k++) {
                    uint32_t c = im->p[(size_t)y * (size_t)im->w + (size_t)(tx[x].first + k)];
                    float wt = tx[x].w[k], ca = (float)ch_a(c);
                    a += ca * wt;
                    r += (float)ch_r(c) * ca * wt;
                    g += (float)ch_g(c) * ca * wt;
                    b += (float)ch_b(c) * ca * wt;
                }
                float *m = &mid[((size_t)y * (size_t)nw + (size_t)x) * 4];
                m[0] = r;
                m[1] = g;
                m[2] = b;
                m[3] = a;
            }
        for (int y = 0; y < nh; y++)
            for (int x = 0; x < nw; x++) {
                float a = 0, r = 0, g = 0, b = 0;
                for (int k = 0; k < ty[y].n; k++) {
                    const float *m = &mid[((size_t)(ty[y].first + k) * (size_t)nw + (size_t)x) * 4];
                    float wt = ty[y].w[k];
                    r += m[0] * wt;
                    g += m[1] * wt;
                    b += m[2] * wt;
                    a += m[3] * wt;
                }
                uint32_t o = 0;
                if (a > 0.5f)
                    o = argb(clamp255((int)(a + 0.5f)), clamp255((int)(r / a + 0.5f)), clamp255((int)(g / a + 0.5f)), clamp255((int)(b / a + 0.5f)));
                out.p[(size_t)y * (size_t)nw + (size_t)x] = o;
            }
        pix_free(im);
        *im = out;
    }
    free(mid);
    free_taps(tx, nw);
    free_taps(ty, nh);
    return ok;
}

static uint64_t pix_hash(const struct pix *im)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < (size_t)im->w * (size_t)im->h; i++) {
        h ^= im->p[i];
        h *= 1099511628211ull;
    }
    return h ^ ((uint64_t)im->w << 32) ^ (uint64_t)im->h;
}

/* ---- files ------------------------------------------------------------------------------------------------------------------------ */

static uint32_t adler32(const uint8_t *p, size_t n)
{
    uint32_t a = 1, b = 0;
    while (n) {
        size_t chunk = n < 5552 ? n : 5552;
        for (size_t i = 0; i < chunk; i++) {
            a += p[i];
            b += a;
        }
        a %= 65521;
        b %= 65521;
        p += chunk;
        n -= chunk;
    }
    return (b << 16) | a;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static bool write_all(FILE *f, const void *p, size_t n)
{
    return fwrite(p, 1, n, f) == n;
}

/* A compressed PNG: RGB when nothing is see-through, else RGBA; every row with the filter (none / sub / up) that looks smallest. */
static uint8_t *png_encode(const struct pix *im, size_t *out_len, bool *has_alpha_out)
{
    bool alpha = false;
    for (size_t i = 0; i < (size_t)im->w * (size_t)im->h && !alpha; i++)
        alpha = ch_a(im->p[i]) != 255;
    int bpp = alpha ? 4 : 3;
    size_t stride = (size_t)im->w * (size_t)bpp;
    size_t raw_n = (stride + 1) * (size_t)im->h;
    uint8_t *raw = malloc(raw_n), *row = malloc(stride * 3);
    if (!raw || !row) {
        free(raw);
        free(row);
        return NULL;
    }
    uint8_t *none = row, *sub = row + stride, *up = row + 2 * stride;
    for (int y = 0; y < im->h; y++) {
        for (int x = 0; x < im->w; x++) {
            uint32_t c = im->p[(size_t)y * (size_t)im->w + (size_t)x];
            uint8_t *d = &none[(size_t)x * (size_t)bpp];
            d[0] = (uint8_t)ch_r(c);
            d[1] = (uint8_t)ch_g(c);
            d[2] = (uint8_t)ch_b(c);
            if (alpha)
                d[3] = (uint8_t)ch_a(c);
        }
        const uint8_t *prev = y ? &raw[(size_t)(y - 1) * (stride + 1) + 1] : NULL;
        uint8_t *prev_plain = NULL;
        (void)prev_plain;
        long cost[3] = { 0, 0, 0 };
        for (size_t i = 0; i < stride; i++) {
            uint8_t left = i >= (size_t)bpp ? none[i - (size_t)bpp] : 0;
            sub[i] = (uint8_t)(none[i] - left);
            cost[0] += none[i] < 128 ? none[i] : 256 - none[i];
            cost[1] += sub[i] < 128 ? sub[i] : 256 - sub[i];
        }
        int best = cost[0] <= cost[1] ? 0 : 1;
        (void)up;
        (void)prev;
        uint8_t *dst = &raw[(size_t)y * (stride + 1)];
        dst[0] = (uint8_t)best;
        memcpy(dst + 1, best == 0 ? none : sub, stride);
    }
    size_t zn = 0;
    uint8_t *z = lm_deflate_buf(raw, raw_n, 6, &zn);
    uint32_t ad = adler32(raw, raw_n);
    free(raw);
    free(row);
    if (!z)
        return NULL;
    size_t total = 8 + 25 + 12 + (zn + 6) + 12;
    uint8_t *out = malloc(total + 16);
    if (!out) {
        free(z);
        return NULL;
    }
    size_t o = 0;
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
    memcpy(out, sig, 8);
    o = 8;
    uint8_t ihdr[13];
    put32(ihdr, (uint32_t)im->w);
    put32(ihdr + 4, (uint32_t)im->h);
    ihdr[8] = 8;
    ihdr[9] = alpha ? 6 : 2;
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    uint8_t chunk[16];
    #define EMIT(type, data, n) do { \
        put32(out + o, (uint32_t)(n)); memcpy(out + o + 4, type, 4); \
        if ((n)) memcpy(out + o + 8, data, (n)); \
        uint32_t crc_ = lm_crc32(0, (const uint8_t *)type, 4); crc_ = lm_crc32(crc_, out + o + 8, (n)); \
        put32(out + o + 8 + (n), crc_); o += 12 + (n); } while (0)
    EMIT("IHDR", ihdr, 13);
    {                                                                   /* IDAT: zlib header, the deflate stream, the adler */
        uint8_t *idat = malloc(zn + 6);
        if (!idat) {
            free(z);
            free(out);
            return NULL;
        }
        idat[0] = 0x78;
        idat[1] = 0x9c;
        memcpy(idat + 2, z, zn);
        put32(idat + 2 + zn, ad);
        size_t in = zn + 6;
        uint8_t *grown = realloc(out, o + 12 + in + 12 + 16);
        if (!grown) {
            free(idat);
            free(z);
            free(out);
            return NULL;
        }
        out = grown;
        EMIT("IDAT", idat, in);
        free(idat);
    }
    EMIT("IEND", chunk, 0);
    #undef EMIT
    free(z);
    *out_len = o;
    if (has_alpha_out)
        *has_alpha_out = alpha;
    return out;
}

enum fmt { FMT_PNG, FMT_JPEG };

static enum fmt fmt_of(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (dot && (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg") || !strcasecmp(dot, ".jpe")))
        return FMT_JPEG;
    return FMT_PNG;
}

static bool has_known_ext(const char *path)
{
    const char *dot = strrchr(path, '.');
    return dot && (!strcasecmp(dot, ".png") || !strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg") || !strcasecmp(dot, ".jpe"));
}

/* Writes the picture: .png keeps transparency, .jpg is laid over white. The file is made whole under a temporary name and renamed over the old one. */
static bool save_file(const struct pix *im, const char *path, char *err, size_t errsz)
{
    size_t n = 0;
    uint8_t *data = NULL;
    if (fmt_of(path) == FMT_JPEG) {
        uint32_t *flat = malloc((size_t)im->w * (size_t)im->h * 4);
        if (!flat) {
            snprintf(err, errsz, "out of memory");
            return false;
        }
        for (size_t i = 0; i < (size_t)im->w * (size_t)im->h; i++)
        {
            uint32_t c = im->p[i];
            int al = ch_a(c);
            flat[i] = argb(0, (ch_r(c) * al + 255 * (255 - al)) / 255, (ch_g(c) * al + 255 * (255 - al)) / 255, (ch_b(c) * al + 255 * (255 - al)) / 255) & 0x00ffffffu;
        }
        data = lm_jpeg_encode_xrgb(flat, im->w, im->h, (size_t)im->w * 4, JPEG_QUALITY, &n);
        free(flat);
    } else {
        data = png_encode(im, &n, NULL);
    }
    if (!data) {
        snprintf(err, errsz, "could not make the file");
        return false;
    }
    char tmp[PATH_MAX + 32];
    snprintf(tmp, sizeof(tmp), "%s.paint-%ld.tmp", path, (long)getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
    bool ok = fd >= 0;
    if (ok) {
        FILE *f = fdopen(fd, "wb");
        ok = f && write_all(f, data, n);
        if (f && fclose(f) != 0)
            ok = false;
        if (ok && rename(tmp, path) != 0)
            ok = false;
        if (!ok)
            unlink(tmp);
    }
    if (!ok)
        snprintf(err, errsz, "%s", strerror(errno));
    free(data);
    return ok;
}

static bool load_file(const char *path, struct pix *im, char *err, size_t errsz)
{
    struct lm_image li;
    if (lm_img_load_alpha(path, &li, err, errsz) != 0)
        return false;
    if (li.w > MAX_SIDE || li.h > MAX_SIDE) {
        lm_img_free(&li);
        snprintf(err, errsz, "the picture is too big (%d x %d)", li.w, li.h);
        return false;
    }
    im->p = li.px;
    im->w = li.w;
    im->h = li.h;
    return true;
}

/* ---- the same operations without a window (--apply), and the self test ------------------------------------------------------------- */

static bool apply_op(struct pix *im, const char *op, char *err, size_t errsz)
{
    struct irect all = { 0, 0, im->w, im->h };
    int a, b, c, d;
    if (!strcmp(op, "rotate-left"))
        return rotate90(im, false);
    if (!strcmp(op, "rotate-right"))
        return rotate90(im, true);
    if (!strcmp(op, "flip-h")) {
        flip(im, true);
        return true;
    }
    if (!strcmp(op, "flip-v")) {
        flip(im, false);
        return true;
    }
    for (int f = 0; f < FX_COUNT; f++)
        if (!strcmp(op, FX_OPS[f]))
            return apply_fx(im, (enum fx)f, all);
    char junk;
    if (sscanf(op, "resize=%dx%d%c", &a, &b, &junk) == 2)
        return resize(im, a, b);
    if (sscanf(op, "resize=%d%%%c", &a, &junk) == 1 && a > 0)
        return resize(im, LM_MAX(im->w * a / 100, 1), LM_MAX(im->h * a / 100, 1));
    if (sscanf(op, "crop=%d,%d,%d,%d%c", &a, &b, &c, &d, &junk) == 4)
        return crop(im, (struct irect){ a, b, a + c, b + d });
    snprintf(err, errsz, "unknown operation '%s'", op);
    return false;
}

static int apply_cmd(int argc, char **argv)
{
    char err[160] = "";
    struct pix im = { 0 };
    if (argc < 3) {
        fprintf(stderr, "usage: mellagui-paint --apply IN OUT [OP...]\n");
        return 2;
    }
    if (!load_file(argv[0], &im, err, sizeof(err))) {
        fprintf(stderr, "mellagui-paint: %s: %s\n", argv[0], err);
        return 1;
    }
    for (int i = 2; i < argc; i++)
        if (!apply_op(&im, argv[i], err, sizeof(err))) {
            fprintf(stderr, "mellagui-paint: %s\n", *err ? err : "the operation failed");
            pix_free(&im);
            return 1;
        }
    if (!save_file(&im, argv[1], err, sizeof(err))) {
        fprintf(stderr, "mellagui-paint: %s: %s\n", argv[1], err);
        pix_free(&im);
        return 1;
    }
    printf("%dx%d %s\n", im.w, im.h, argv[1]);
    pix_free(&im);
    return 0;
}

static int g_bad;
#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "selftest: " __VA_ARGS__); fputc('\n', stderr); g_bad++; } } while (0)

static int selftest(void)
{
    struct pix im;
    CHECK(pix_alloc(&im, 4, 3, 0xff112233u), "alloc");
    im.p[0] = 0xffff0000u;                                              /* the first pixel red */
    im.p[11] = 0xff0000ffu;                                             /* the last one blue */
    struct pix t;
    CHECK(pix_copy(&im, &t), "copy");
    CHECK(rotate90(&t, true) && t.w == 3 && t.h == 4 && t.p[2] == 0xffff0000u && t.p[(size_t)3 * 3] == 0xff0000ffu, "rotate right puts the top left pixel at the top right");
    CHECK(rotate90(&t, false) && t.w == 4 && t.h == 3 && !memcmp(t.p, im.p, 48), "rotate right then left is the same picture");
    flip(&t, true);
    CHECK(t.p[3] == 0xffff0000u && t.p[8] == 0xff0000ffu, "flip horizontal");
    flip(&t, true);
    flip(&t, false);
    CHECK(t.p[8] == 0xffff0000u && t.p[3] == 0xff0000ffu, "flip vertical");
    flip(&t, false);
    CHECK(!memcmp(t.p, im.p, 48), "two flips undo each other");
    CHECK(crop(&t, (struct irect){ 1, 1, 3, 3 }) && t.w == 2 && t.h == 2 && t.p[0] == 0xff112233u, "crop");
    CHECK(!crop(&t, (struct irect){ 9, 9, 12, 12 }), "a crop outside the picture is refused");
    pix_free(&t);
    pix_free(&im);

    CHECK(pix_alloc(&im, 8, 8, 0xff808080u), "alloc 8x8");
    struct irect all = { 0, 0, 8, 8 };
    CHECK(apply_fx(&im, FX_INVERT, all) && im.p[0] == 0xff7f7f7fu && apply_fx(&im, FX_INVERT, all) && im.p[0] == 0xff808080u, "invert twice");
    im.p[9] = 0xff3366ccu;
    CHECK(apply_fx(&im, FX_GRAY, all) && ch_r(im.p[9]) == ch_g(im.p[9]) && ch_g(im.p[9]) == ch_b(im.p[9]), "gray has no colour");
    CHECK(apply_fx(&im, FX_BRIGHTER, (struct irect){ 0, 0, 2, 2 }) && ch_r(im.p[0]) == 0x80 + 24 && ch_r(im.p[63]) == 0x80, "an effect stays inside the region");
    CHECK(apply_fx(&im, FX_BLUR, all) && apply_fx(&im, FX_SHARPEN, all), "blur and sharpen run");
    struct pix flat;
    CHECK(pix_alloc(&flat, 6, 6, 0xff404040u) && apply_fx(&flat, FX_BLUR, (struct irect){ 0, 0, 6, 6 }) && flat.p[14] == 0xff404040u, "blurring a flat picture changes nothing");
    pix_free(&flat);
    pix_free(&im);

    CHECK(pix_alloc(&im, 10, 10, 0xffffffffu), "alloc 10x10");
    struct irect dirty = { 0, 0, 0, 0 };
    CHECK(flood(&im, 5, 5, 0xffff0000u, 0, &dirty) && im.p[0] == 0xffff0000u && im.p[99] == 0xffff0000u && dirty.x1 == 10, "fill covers an empty picture");
    stroke(&im, 0.5, 5.5, 9.5, 5.5, 0.5, 0xff0000ffu, NULL);
    CHECK(im.p[5 * 10 + 4] == 0xff0000ffu && im.p[4 * 10 + 4] == 0xffff0000u, "a one pixel line is one pixel wide");
    CHECK(flood(&im, 5, 2, 0xff00ff00u, 0, &dirty) && im.p[2 * 10 + 5] == 0xff00ff00u && im.p[7 * 10 + 5] == 0xffff0000u, "a fill stops at a line");
    fill_rect(&im, 2, 2, 4, 4, 0x00000000u, NULL);
    CHECK(ch_a(im.p[3 * 10 + 3]) == 255 || im.p[3 * 10 + 3] == 0xff00ff00u || im.p[3 * 10 + 3] == 0, "a transparent colour erases");
    CHECK(im.p[3 * 10 + 3] == 0, "... and leaves nothing behind");
    pix_free(&im);

    CHECK(pix_alloc(&im, 8, 4, 0xff0000ffu), "alloc for resize");
    CHECK(resize(&im, 4, 2) && im.w == 4 && im.h == 2 && im.p[0] == 0xff0000ffu, "shrinking keeps a flat colour");
    CHECK(resize(&im, 16, 8) && im.w == 16 && im.h == 8 && im.p[40] == 0xff0000ffu, "growing keeps a flat colour");
    pix_free(&im);
    CHECK(pix_alloc(&im, 2, 1, 0), "alloc 2x1");
    im.p[0] = 0xff000000u;
    im.p[1] = 0xffffffffu;
    CHECK(resize(&im, 1, 1) && ch_r(im.p[0]) >= 126 && ch_r(im.p[0]) <= 129, "two pixels averaged: grey (%d)", ch_r(im.p[0]));
    pix_free(&im);
    CHECK(pix_alloc(&im, 2, 1, 0), "alloc see-through");
    im.p[0] = 0xffff0000u;
    im.p[1] = 0x000000ffu;                                              /* fully see-through blue: must not tint the red */
    CHECK(resize(&im, 1, 1) && ch_r(im.p[0]) == 255 && ch_b(im.p[0]) == 0 && ch_a(im.p[0]) >= 126 && ch_a(im.p[0]) <= 129, "a transparent pixel does not tint its neighbour");
    pix_free(&im);

    char err[160];
    CHECK(pix_alloc(&im, 37, 23, 0), "alloc for files");
    for (int y = 0; y < 23; y++)
        for (int x = 0; x < 37; x++)
            im.p[(size_t)y * 37 + (size_t)x] = argb(255, x * 6, y * 10, (x * y) & 255);
    char path[64];
    snprintf(path, sizeof(path), "/tmp/mellagui-paint-selftest-%ld", (long)getpid());
    char png[96], jpg[96];
    snprintf(png, sizeof(png), "%s.png", path);
    snprintf(jpg, sizeof(jpg), "%s.jpg", path);
    CHECK(save_file(&im, png, err, sizeof(err)), "save png: %s", err);
    struct pix back = { 0 };
    CHECK(load_file(png, &back, err, sizeof(err)) && back.w == 37 && back.h == 23 && !memcmp(back.p, im.p, (size_t)37 * 23 * 4), "an opaque PNG comes back as it was");
    pix_free(&back);
    im.p[5] = 0x80ff8040u;
    im.p[6] = 0x00000000u;
    CHECK(save_file(&im, png, err, sizeof(err)), "save png with alpha");
    CHECK(load_file(png, &back, err, sizeof(err)) && back.w == 37 && ch_a(back.p[5]) == 0x80 && ch_g(back.p[5]) == 0x80 && ch_a(back.p[6]) == 0, "a PNG with transparency keeps it");
    pix_free(&back);
    CHECK(save_file(&im, jpg, err, sizeof(err)), "save jpg: %s", err);
    CHECK(load_file(jpg, &back, err, sizeof(err)) && back.w == 37 && back.h == 23, "a JPEG opens");
    CHECK(ch_r(back.p[6]) > 230 && ch_g(back.p[6]) > 230 && ch_b(back.p[6]) > 230, "a see-through pixel is white in a JPEG (%08x)", back.p[6]);
    pix_free(&back);
    unlink(png);
    unlink(jpg);
    pix_free(&im);

    CHECK(!apply_op(&im, "nonsense", err, sizeof(err)) && strstr(err, "unknown"), "an unknown operation is refused");
    if (g_bad)
        return 1;
    puts("mellagui-paint selftest ok");
    return 0;
}

/* ---- the window ------------------------------------------------------------------------------------------------------------------ */

enum tool { T_SELECT, T_PEN, T_LINE, T_BOX, T_CIRCLE, T_FILL, T_ERASER, T_PICK, T_COUNT };
static const char *const TOOL_NAMES[T_COUNT] = { "Select", "Pen", "Line", "Box", "Circle", "Fill", "Eraser", "Picker" };
static const char TOOL_KEYS[T_COUNT] = { 's', 'p', 'l', 'b', 'c', 'f', 'e', 'i' };
static const char *const TOOL_HINTS[T_COUNT] = {
    "Drag a rectangle: Enter crops to it, Delete clears it, effects work on it",
    "Drag to draw (the left button: pen colour, the right one: the second colour)",
    "Drag a line (Shift: 45 degrees)",
    "Drag a box (Shift: a square); Filled in the strip makes it solid",
    "Drag a circle (Shift: round); Filled in the strip makes it solid",
    "Click to fill an area with the pen colour (right click: the second colour)",
    "Drag to rub out (it paints with the second colour; transparent = it erases)",
    "Click a pixel to take its colour (right click: as the second colour)",
};

#define N_PAL 16
static const uint32_t PALETTE[N_PAL] = { 0x00000000, 0xff000000, 0xff555555, 0xffaaaaaa, 0xffffffff, 0xffe53935, 0xfffb8c00, 0xfffdd835, 0xffa4c639, 0xff43a047, 0xff00897b, 0xff29b6f6,
                                         0xff1e88e5, 0xff7e57c2, 0xffec407a, 0xff795548 };
#define N_SIZES 6
static const int BRUSH[N_SIZES] = { 1, 2, 4, 8, 14, 24 };

enum prompt { P_NONE, P_OPEN, P_SAVEAS, P_NEW, P_RESIZE, P_DISCARD, P_REPLACE, P_HELP };
enum after { AF_NONE, AF_OPEN, AF_NEW, AF_QUIT };

/* The two rails' buttons. */
enum btn { B_SELECT, B_PEN, B_LINE, B_BOX, B_CIRCLE, B_FILL, B_ERASER, B_PICK,              /* the left rail: the tools */
           B_NEW, B_OPEN, B_SAVE, B_UNDO, B_REDO, B_TRANSFORM, B_FX, B_CROP, B_FIT, B_HELP, B_COUNT };

struct snap {
    struct pix im;
};

struct app {
    struct wlwin win;
    struct wlclip clip;
    struct wlrail_anim ran[2];
    int hover;                                  /* the rail button under the pointer: B_* of the left or the right rail, -1 none */
    double px, py;                              /* the pointer in drawing coordinates */
    bool has_pointer;

    struct pix im;
    char path[PATH_MAX];
    bool modified;
    int saved_fmt_jpeg;

    struct snap undo[MAX_UNDO], redo[MAX_UNDO];
    int nundo, nredo;

    enum tool tool;
    uint32_t fg, bg;
    int size;                                   /* index into BRUSH */
    bool filled;

    double zoom, vx, vy;                        /* logical screen pixels per picture pixel; where the picture's top left lies in the canvas */
    bool fitted;                                /* the zoom is "fit the window": it follows the window's size */
    uint32_t *vbuf;
    size_t vbuf_cap;
    int vbw, vbh;                               /* the size of what vbuf holds, in physical pixels */
    bool vdirty;

    bool drag, pan;                             /* a tool is being dragged / the picture is being panned with the middle button */
    int drag_button;                            /* 272 or 273 */
    double sx, sy, lx, ly;                      /* where the drag began, and the last point (picture coordinates, may be fractional) */
    double pan_x, pan_y, pan_vx, pan_vy;
    struct pix before;                          /* the picture before a shape that is being dragged */
    struct irect preview;                       /* what a shape preview changed */
    bool have_before;

    bool has_sel;
    struct irect sel;                           /* picture pixels, x1 / y1 exclusive */

    enum prompt prompt;
    enum after after;                           /* what to do once a discard question was answered with "Discard" */
    char pbuf[PATH_MAX];                        /* the text field of a prompt */
    char perr[160];                             /* what was wrong with the last answer */
    int pcard_hover, pcard_down;
    struct wlpicker pk;                         /* the folder list of the Open / Save as card (wlpicker.h, the same in every program) */
    int menu_kind;                              /* the menu that is open: 0 none, 1 effects, 2 transform */

    char msg[160];
    int64_t msg_ms;
    bool quit_now;
};

static const char *g_dump;
static char g_startdir[PATH_MAX];

static int64_t now_ms(void) { return wlrail_now(); }

#define COL_BG (lm_theme.background)
#define COL_BAR (lm_theme.bar)
#define COL_FIELD (lm_theme.field)
#define COL_TEXT (lm_theme.text_soft)
#define COL_DIM (lm_theme.text_dim)
#define COL_ACCENT (lm_theme.accent)

static void say(struct app *a, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void say(struct app *a, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(a->msg, sizeof(a->msg), fmt, ap);
    va_end(ap);
    a->msg_ms = now_ms();
    a->win.redraw = true;
}

/* ---- the geometry --------------------------------------------------------------------------------------------------------------- */

static int cv_x(const struct app *a) { (void)a; return WLRAIL_W; }
static int cv_w(const struct app *a) { return LM_MAX(a->win.bw - 2 * WLRAIL_W, 1); }
static int cv_h(const struct app *a) { return LM_MAX(a->win.bh - PAL_H - STAT_H, 1); }
static int pal_y(const struct app *a) { return a->win.bh - PAL_H - STAT_H; }
static int stat_y(const struct app *a) { return a->win.bh - STAT_H; }

static bool in_canvas(const struct app *a, double x, double y)
{
    return x >= cv_x(a) && x < cv_x(a) + cv_w(a) && y >= 0 && y < cv_h(a);
}

static double fit_zoom(const struct app *a)
{
    if (a->im.w < 1)
        return 1.0;
    double z = LM_MIN((cv_w(a) - 24.0) / a->im.w, (cv_h(a) - 24.0) / a->im.h);
    return z > 1.0 ? 1.0 : z < 0.02 ? 0.02 : z;
}

static void center_axis(const struct app *a, bool horizontal)
{
    (void)a;
    (void)horizontal;
}

static void view_center(struct app *a)
{
    a->vx = (cv_w(a) - a->im.w * a->zoom) / 2.0;
    a->vy = (cv_h(a) - a->im.h * a->zoom) / 2.0;
    a->vdirty = true;
    a->win.redraw = true;
}

static void view_fit(struct app *a)
{
    a->zoom = fit_zoom(a);
    a->fitted = true;
    view_center(a);
}

/* Keeps a good part of the picture in view: it may be moved about, never lost. */
static void view_clamp(struct app *a)
{
    double iw = a->im.w * a->zoom, ih = a->im.h * a->zoom, keep = 48.0;
    if (iw <= cv_w(a))
        a->vx = LM_CLAMP(a->vx, 0.0, cv_w(a) - iw);
    else
        a->vx = LM_CLAMP(a->vx, cv_w(a) - iw, 0.0);
    if (ih <= cv_h(a))
        a->vy = LM_CLAMP(a->vy, 0.0, cv_h(a) - ih);
    else
        a->vy = LM_CLAMP(a->vy, cv_h(a) - ih, 0.0);
    (void)keep;
    (void)center_axis;
}

static void zoom_to(struct app *a, double nz, double ax, double ay)           /* ax / ay: a point of the canvas that stays where it is */
{
    nz = LM_CLAMP(nz, 0.02, 48.0);
    double ix = (ax - a->vx) / a->zoom, iy = (ay - a->vy) / a->zoom;
    a->zoom = nz;
    a->vx = ax - ix * nz;
    a->vy = ay - iy * nz;
    a->fitted = false;
    if (a->im.w * nz <= cv_w(a))
        a->vx = (cv_w(a) - a->im.w * nz) / 2.0;
    if (a->im.h * nz <= cv_h(a))
        a->vy = (cv_h(a) - a->im.h * nz) / 2.0;
    view_clamp(a);
    a->vdirty = true;
    a->win.redraw = true;
}

static void to_pic(const struct app *a, double sx, double sy, double *ix, double *iy)
{
    *ix = (sx - cv_x(a) - a->vx) / a->zoom;
    *iy = (sy - a->vy) / a->zoom;
}

/* ---- undo ----------------------------------------------------------------------------------------------------------------------- */

static size_t snaps_bytes(const struct snap *s, int n)
{
    size_t b = 0;
    for (int i = 0; i < n; i++)
        b += (size_t)s[i].im.w * (size_t)s[i].im.h * 4;
    return b;
}

static void drop_snaps(struct snap *s, int *n)
{
    for (int i = 0; i < *n; i++)
        pix_free(&s[i].im);
    *n = 0;
}

static void push_snap(struct snap *s, int *n, const struct pix *im)
{
    if (*n == MAX_UNDO) {
        pix_free(&s[0].im);
        memmove(&s[0], &s[1], sizeof(s[0]) * (MAX_UNDO - 1));
        (*n)--;
    }
    if (pix_copy(im, &s[*n].im))
        (*n)++;
    while (*n > 1 && snaps_bytes(s, *n) > UNDO_BYTES) {
        pix_free(&s[0].im);
        memmove(&s[0], &s[1], sizeof(s[0]) * (size_t)(*n - 1));
        (*n)--;
    }
}

/* Call before a change: keeps the picture as it is, forgets what could be redone. */
static void checkpoint(struct app *a)
{
    push_snap(a->undo, &a->nundo, &a->im);
    drop_snaps(a->redo, &a->nredo);
}

static void changed(struct app *a)
{
    a->modified = true;
    a->vdirty = true;
    a->win.redraw = true;
}

static void sel_clip(struct app *a)
{
    if (!a->has_sel)
        return;
    clip_rect(&a->im, &a->sel);
    if (a->sel.x1 - a->sel.x0 < 1 || a->sel.y1 - a->sel.y0 < 1)
        a->has_sel = false;
}

static void do_undo(struct app *a, bool redo)
{
    struct snap *from = redo ? a->redo : a->undo, *to = redo ? a->undo : a->redo;
    int *nf = redo ? &a->nredo : &a->nundo, *nt = redo ? &a->nundo : &a->nredo;
    if (*nf == 0) {
        say(a, redo ? "Nothing to redo" : "Nothing to undo");
        return;
    }
    push_snap(to, nt, &a->im);
    pix_free(&a->im);
    a->im = from[*nf - 1].im;
    from[*nf - 1].im = (struct pix){ 0 };
    (*nf)--;
    a->has_sel = false;
    changed(a);
    if (a->fitted)
        view_fit(a);
    else {
        view_clamp(a);
        a->vdirty = true;
    }
    say(a, redo ? "Redone" : "Undone");
}

/* ---- the operations the buttons run ---------------------------------------------------------------------------------------------- */

static void new_image(struct app *a, int w, int h, const char *path)
{
    struct pix im;
    if (!pix_alloc(&im, w, h, 0xffffffffu)) {
        say(a, "That size is not possible (1..%d, at most %u million pixels)", MAX_SIDE, LM_IMG_MAX_PIXELS / 1000000u);
        return;
    }
    pix_free(&a->im);
    a->im = im;
    drop_snaps(a->undo, &a->nundo);
    drop_snaps(a->redo, &a->nredo);
    snprintf(a->path, sizeof(a->path), "%s", path ? path : "");
    a->modified = false;
    a->has_sel = false;
    view_fit(a);
    say(a, "New picture, %d x %d", w, h);
}

static bool open_image(struct app *a, const char *path)
{
    char err[160];
    struct pix im = { 0 };
    if (!load_file(path, &im, err, sizeof(err))) {
        say(a, "Could not open %s: %s", path, err);
        snprintf(a->perr, sizeof(a->perr), "%s", err);
        return false;
    }
    pix_free(&a->im);
    a->im = im;
    drop_snaps(a->undo, &a->nundo);
    drop_snaps(a->redo, &a->nredo);
    char real[PATH_MAX];
    snprintf(a->path, sizeof(a->path), "%s", realpath(path, real) ? real : path);
    a->modified = false;
    a->has_sel = false;
    view_fit(a);
    const char *base = strrchr(a->path, '/');
    say(a, "Opened %s, %d x %d", base ? base + 1 : a->path, im.w, im.h);
    return true;
}

static bool write_image(struct app *a, const char *path)
{
    char err[160];
    if (!save_file(&a->im, path, err, sizeof(err))) {
        say(a, "Could not save: %s", err);
        snprintf(a->perr, sizeof(a->perr), "%s", err);
        return false;
    }
    snprintf(a->path, sizeof(a->path), "%s", path);
    a->modified = false;
    const char *base = strrchr(path, '/');
    say(a, "Saved %s", base ? base + 1 : path);
    return true;
}

static void whole_op(struct app *a, const char *what, bool ok)
{
    if (!ok) {
        say(a, "Not possible (out of memory?)");
        return;
    }
    a->has_sel = false;
    changed(a);
    if (a->fitted)
        view_fit(a);
    else {
        view_clamp(a);
        a->vdirty = true;
    }
    say(a, "%s", what);
}

static void do_effect(struct app *a, enum fx f)
{
    struct irect r = a->has_sel ? a->sel : (struct irect){ 0, 0, a->im.w, a->im.h };
    checkpoint(a);
    if (!apply_fx(&a->im, f, r)) {
        say(a, "Not possible (out of memory?)");
        return;
    }
    changed(a);
    say(a, "%s%s", FX_NAMES[f], a->has_sel ? " (the selection)" : "");
}

static void do_crop(struct app *a)
{
    if (!a->has_sel) {
        say(a, "Select an area first (the Select tool)");
        return;
    }
    checkpoint(a);
    whole_op(a, "Cropped", crop(&a->im, a->sel));
}

static void do_delete_sel(struct app *a)
{
    if (!a->has_sel)
        return;
    checkpoint(a);
    fill_rect(&a->im, a->sel.x0, a->sel.y0, a->sel.x1, a->sel.y1, 0x00000000u, NULL);
    changed(a);
    say(a, "Cleared the selection");
}

/* The text of a prompt's answer: "800x600", "800 600", "50%" -> a size. */
static bool parse_size(const char *s, int cw, int ch, int *w, int *h)
{
    int a, b;
    char junk;
    while (*s == ' ')
        s++;
    if (sscanf(s, "%d%*[xX* ]%d%c", &a, &b, &junk) == 2) {
        *w = a;
        *h = b;
        return a > 0 && b > 0;
    }
    if (sscanf(s, "%d%%%c", &a, &junk) == 1 && a > 0) {
        *w = LM_MAX(cw * a / 100, 1);
        *h = LM_MAX(ch * a / 100, 1);
        return true;
    }
    if (sscanf(s, "%d%c", &a, &junk) == 1 && a > 0 && cw > 0) {                  /* just a width: the height follows */
        *w = a;
        *h = LM_MAX((int)((double)ch * a / cw + 0.5), 1);
        return true;
    }
    return false;
}

/* ---- the prompts (cards) -------------------------------------------------------------------------------------------------------- */

static void prompt_open(struct app *a, enum prompt p, const char *initial)
{
    a->prompt = p;
    snprintf(a->pbuf, sizeof(a->pbuf), "%s", initial ? initial : "");
    a->perr[0] = 0;
    a->pcard_hover = a->pcard_down = -1;
    wlpicker_reset(&a->pk);                                                     /* (the folder list is read again) */
    wltext_end(a->pbuf);
    a->win.redraw = true;
}

static void default_dir(const struct app *a, char *out, size_t n)
{
    if (a->path[0]) {
        snprintf(out, n, "%s", a->path);
        char *slash = strrchr(out, '/');
        if (slash)
            slash[1] = 0;
        return;
    }
    const char *home = getenv("HOME"), *pics = getenv("XDG_PICTURES_DIR");
    char d[PATH_MAX];
    if (pics && *pics)
        snprintf(d, sizeof(d), "%s", pics);
    else
        snprintf(d, sizeof(d), "%s/Pictures", home ? home : "");
    struct stat st;
    if (stat(d, &st) == 0 && S_ISDIR(st.st_mode))
        snprintf(out, n, "%.*s/", (int)n - 2, d);
    else
        snprintf(out, n, "%.*s/", (int)n - 2, home && *home ? home : (g_startdir[0] ? g_startdir : "/"));
}

static const char *const HELP_LINES[] = {
    "Tools  S select  P pen  L line  B box  C circle  F fill  E eraser  I picker",
    "Left button draws with the pen colour, the right button with the second one.",
    "Shift keeps a line at 45 degrees and a box or circle square.",
    "X swaps the two colours.  [ and ] change the brush size.",
    "Ctrl+O open   Ctrl+N new   Ctrl+S save   Ctrl+Shift+S save as",
    "Ctrl+Z undo   Ctrl+Y or Ctrl+Shift+Z redo",
    "Enter crops to the selection, Delete clears it, Ctrl+A selects all, Esc drops it.",
    "Wheel zooms, middle button drags the picture, + - zoom, 0 fits, 1 real size.",
    "Effects (right rail) work on the selection, or on the whole picture.",
    ".png keeps transparency, .jpg is laid on white.",
};

static void prompt_accept(struct app *a);

#define card_name_ok wlpicker_picture

/* How many rows the list has (0 = no list: another card, or a window too low for one). */
static int card_list_rows(const struct app *a)
{
    return a->prompt == P_OPEN || a->prompt == P_SAVEAS ? wlpicker_fit(a->win.bh, 170) : 0;
}

/* The card's place and height (what wlcard_frame will make of it). */
static void card_rect(const struct app *a, struct wlcard *c, int *nb)
{
    int ch = a->prompt == P_HELP ? 56 + (int)(sizeof(HELP_LINES) / sizeof(HELP_LINES[0])) * (LM_FONT_H + 4) + WLCARD_BTN_H
             : (a->prompt == P_DISCARD || a->prompt == P_REPLACE) ? 130 : 150;
    int rows = card_list_rows(a);
    if (rows)
        ch = 170 + rows * WLPICKER_ROW_H;
    *nb = a->prompt == P_DISCARD ? 3 : a->prompt == P_HELP ? 1 : 2;
    c->w = LM_MIN(a->prompt == P_HELP ? 620 : 560, a->win.bw - 16);
    c->h = LM_MIN(ch, a->win.bh - 16);
    c->x = (a->win.bw - c->w) / 2;
    c->y = (a->win.bh - c->h) / 2;
}

/* The text field's top, and the list's. */
static int card_field_y(const struct wlcard *c) { return c->y + 42 + LM_FONT_H + 8; }
static int card_list_y(const struct wlcard *c) { return card_field_y(c) + 30; }

static void ask_open(struct app *a)
{
    char d[PATH_MAX];
    default_dir(a, d, sizeof(d));
    prompt_open(a, P_OPEN, d);
}

static void ask_saveas(struct app *a)
{
    char d[PATH_MAX];
    if (a->path[0])
        snprintf(d, sizeof(d), "%s", a->path);
    else {
        default_dir(a, d, sizeof(d));
        strncat(d, "picture.png", sizeof(d) - strlen(d) - 1);
    }
    prompt_open(a, P_SAVEAS, d);
}

static void do_save(struct app *a)
{
    if (!a->path[0]) {
        ask_saveas(a);
        return;
    }
    write_image(a, a->path);
}

/* Something that would throw the picture away: ask once when it has unsaved changes. */
static void guard(struct app *a, enum after what)
{
    if (a->modified) {
        a->after = what;
        prompt_open(a, P_DISCARD, "");
        return;
    }
    switch (what) {
    case AF_OPEN: ask_open(a); break;
    case AF_NEW: {
        char s[32];
        snprintf(s, sizeof(s), "%dx%d", a->im.w > 0 ? a->im.w : 800, a->im.h > 0 ? a->im.h : 600);
        prompt_open(a, P_NEW, s);
        break;
    }
    case AF_QUIT: a->win.running = false; break;
    default: break;
    }
}

static void prompt_accept(struct app *a)
{
    char text[PATH_MAX];
    snprintf(text, sizeof(text), "%s", a->pbuf);
    char *s = text;
    while (*s == ' ')
        s++;
    size_t n = strlen(s);
    while (n && s[n - 1] == ' ')
        s[--n] = 0;
    if (s[0] == '~' && s[1] == '/' && getenv("HOME")) {
        char t2[PATH_MAX];
        snprintf(t2, sizeof(t2), "%s%s", getenv("HOME"), s + 1);
        snprintf(text, sizeof(text), "%s", t2);
        s = text;
    }
    switch (a->prompt) {
    case P_OPEN:
        if (!*s)
            return;
        if (open_image(a, s))
            a->prompt = P_NONE;
        break;
    case P_SAVEAS: {
        if (!*s || s[strlen(s) - 1] == '/') {
            snprintf(a->perr, sizeof(a->perr), "Type a file name");
            return;
        }
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s", s);
        if (!has_known_ext(path) && strlen(path) + 5 < sizeof(path))
            strcat(path, ".png");
        struct stat st;
        if (stat(path, &st) == 0 && strcmp(path, a->path)) {                    /* never over another file without asking */
            snprintf(a->pbuf, sizeof(a->pbuf), "%s", path);
            a->prompt = P_REPLACE;
            return;
        }
        if (write_image(a, path))
            a->prompt = P_NONE;
        break;
    }
    case P_REPLACE:
        if (write_image(a, a->pbuf))
            a->prompt = P_NONE;
        break;
    case P_NEW: {
        int w, h;
        if (!parse_size(s, 0, 0, &w, &h)) {
            snprintf(a->perr, sizeof(a->perr), "Type a size like 800x600");
            return;
        }
        a->prompt = P_NONE;
        new_image(a, w, h, "");
        break;
    }
    case P_RESIZE: {
        int w, h;
        if (!parse_size(s, a->im.w, a->im.h, &w, &h)) {
            snprintf(a->perr, sizeof(a->perr), "Type a size (800x600), a width (800) or a percentage (50%%)");
            return;
        }
        checkpoint(a);
        if (!resize(&a->im, w, h)) {
            drop_snaps(a->redo, &a->nredo);
            if (a->nundo) {                                                  /* nothing changed: take the checkpoint back */
                pix_free(&a->undo[a->nundo - 1].im);
                a->nundo--;
            }
            snprintf(a->perr, sizeof(a->perr), "That size is not possible (at most %d a side)", MAX_SIDE);
            return;
        }
        a->prompt = P_NONE;
        a->has_sel = false;
        changed(a);
        if (a->fitted)
            view_fit(a);
        say(a, "Resized to %d x %d", a->im.w, a->im.h);
        break;
    }
    case P_DISCARD:
    case P_HELP:
    case P_NONE: break;
    }
    a->win.redraw = true;
}

static void discard_chosen(struct app *a, bool save_first)
{
    enum after what = a->after;
    a->prompt = P_NONE;
    if (save_first) {
        if (!a->path[0]) {                                                   /* it needs a name first: ask, and the question comes again afterwards */
            ask_saveas(a);
            return;
        }
        if (!write_image(a, a->path))
            return;
    }
    a->modified = false;
    a->after = AF_NONE;
    guard(a, what);
}

/* ---- painting ------------------------------------------------------------------------------------------------------------------ */

static uint32_t checker(int x, int y)
{
    return ((x >> 3) + (y >> 3)) & 1 ? 0x999999u : 0xcccccc;
}

static void render_view(struct app *a)
{
    int s = wlwin_scale(&a->win);
    int pw = cv_w(a) * s, ph = cv_h(a) * s;
    if (pw < 1 || ph < 1)
        return;
    if ((size_t)pw * (size_t)ph > a->vbuf_cap) {
        free(a->vbuf);
        a->vbuf = malloc((size_t)pw * (size_t)ph * 4);
        a->vbuf_cap = a->vbuf ? (size_t)pw * (size_t)ph : 0;
        if (!a->vbuf)
            return;
    }
    a->vbw = pw;
    a->vbh = ph;
    double zp = a->zoom * s, ox = a->vx * s, oy = a->vy * s;
    int n = zp >= 1.0 ? 1 : LM_MIN((int)ceil(1.0 / zp), 4);                 /* samples a side when the picture is smaller than the screen: a box average */
    uint32_t back = lm_theme.field & 0xffffffu;
    for (int y = 0; y < ph; y++) {
        uint32_t *row = &a->vbuf[(size_t)y * (size_t)pw];
        double iy0 = (y - oy) / zp;
        if (iy0 + (n > 1 ? 1.0 / zp : 0) < 0 || iy0 >= a->im.h) {
            for (int x = 0; x < pw; x++)
                row[x] = back;
            continue;
        }
        for (int x = 0; x < pw; x++) {
            double ix0 = (x - ox) / zp;
            if (ix0 < 0 || ix0 >= a->im.w || iy0 < 0) {
                row[x] = back;
                continue;
            }
            int sa = 0, sr = 0, sg = 0, sb = 0, cnt = 0;
            for (int j = 0; j < n; j++)
                for (int i = 0; i < n; i++) {
                    int ix = (int)floor(ix0 + (n > 1 ? (i + 0.5) / (n * zp) - 0.5 / zp : 0));
                    int iy = (int)floor(iy0 + (n > 1 ? (j + 0.5) / (n * zp) - 0.5 / zp : 0));
                    ix = LM_CLAMP(ix, 0, a->im.w - 1);
                    iy = LM_CLAMP(iy, 0, a->im.h - 1);
                    uint32_t c = a->im.p[(size_t)iy * (size_t)a->im.w + (size_t)ix];
                    int al = ch_a(c);
                    sa += al;
                    sr += ch_r(c) * al;
                    sg += ch_g(c) * al;
                    sb += ch_b(c) * al;
                    cnt++;
                }
            int al = sa / cnt;
            uint32_t bg = checker(x, y);
            if (al >= 255) {
                row[x] = argb(0, sr / sa * 1, sg / sa * 1, sb / sa * 1) & 0xffffffu;
                continue;
            }
            int r = sa ? sr / sa : 0, g = sa ? sg / sa : 0, b = sa ? sb / sa : 0;
            row[x] = argb(0, (r * al + ch_r(bg) * (255 - al)) / 255, (g * al + ch_g(bg) * (255 - al)) / 255, (b * al + ch_b(bg) * (255 - al)) / 255) & 0xffffffu;
        }
    }
    a->vdirty = false;
}

/* A dashed rectangle in canvas coordinates. */
static void dashed_rect(struct app *a, double x0, double y0, double x1, double y1, uint32_t c1, uint32_t c2)
{
    int ax = cv_x(a) + (int)floor(x0), ay = (int)floor(y0), bx = cv_x(a) + (int)floor(x1), by = (int)floor(y1);
    for (int x = ax; x <= bx; x++) {
        uint32_t c = ((x - ax) / 4) & 1 ? c2 : c1;
        if (in_canvas(a, x, ay))
            wlwin_fill(a->win.bw > 0 ? &a->win : &a->win, x, ay, 1, 1, c);
        if (in_canvas(a, x, by))
            wlwin_fill(&a->win, x, by, 1, 1, c);
    }
    for (int y = ay; y <= by; y++) {
        uint32_t c = ((y - ay) / 4) & 1 ? c2 : c1;
        if (in_canvas(a, ax, y))
            wlwin_fill(&a->win, ax, y, 1, 1, c);
        if (in_canvas(a, bx, y))
            wlwin_fill(&a->win, bx, y, 1, 1, c);
    }
}

/* ---- the rails ------------------------------------------------------------------------------------------------------------------ */

static void ic_select(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    (void)bg;
    for (int i = 0; i < 5; i++) {
        wlwin_fill(w, cx - 8 + i * 4, cy - 7, 2, 2, c);
        wlwin_fill(w, cx - 8 + i * 4, cy + 5, 2, 2, c);
        wlwin_fill(w, cx - 8, cy - 7 + i * 3, 2, 2, c);
        wlwin_fill(w, cx + 6, cy - 7 + i * 3, 2, 2, c);
    }
}

static void ic_pen(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    (void)bg;
    wlrail_line(w, cx - 7, cy + 7, cx + 4, cy - 4, 3, c);
    wlrail_line(w, cx + 5, cy - 5, cx + 7, cy - 7, 2, c);
    wlwin_fill(w, cx - 9, cy + 7, 4, 2, c);
}

static void ic_line(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    (void)bg;
    wlrail_line(w, cx - 7, cy + 7, cx + 7, cy - 7, 2, c);
}

static void ic_box(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    (void)bg;
    wlwin_fill(w, cx - 8, cy - 6, 16, 2, c);
    wlwin_fill(w, cx - 8, cy + 4, 16, 2, c);
    wlwin_fill(w, cx - 8, cy - 6, 2, 12, c);
    wlwin_fill(w, cx + 6, cy - 6, 2, 12, c);
}

static void ic_circle(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    wlwin_round_fill(w, cx - 8, cy - 8, 16, 16, 8, c);
    wlwin_round_fill(w, cx - 6, cy - 6, 12, 12, 6, bg);
}

static void ic_fill(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    (void)bg;
    wlrail_line(w, cx - 6, cy - 1, cx, cy - 7, 2, c);                  /* a tipped bucket and the drop that leaves it */
    wlrail_line(w, cx - 6, cy - 1, cx + 1, cy + 6, 2, c);
    wlrail_line(w, cx, cy - 7, cx + 7, cy, 2, c);
    wlrail_line(w, cx + 1, cy + 6, cx + 7, cy, 2, c);
    wlwin_round_fill(w, cx + 5, cy + 3, 4, 5, 2, c);
}

static void ic_eraser(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    (void)bg;
    wlrail_line(w, cx - 7, cy + 2, cx - 1, cy - 6, 5, c);
    wlrail_line(w, cx - 1, cy - 6, cx + 7, cy, 5, c);
    wlrail_line(w, cx + 7, cy, cx + 1, cy + 7, 5, c);
    wlrail_line(w, cx + 1, cy + 7, cx - 7, cy + 2, 5, c);
    wlwin_fill(w, cx - 8, cy + 8, 16, 1, c);
}

static void ic_pick(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    (void)bg;
    wlrail_line(w, cx - 7, cy + 7, cx + 2, cy - 2, 2, c);
    wlrail_line(w, cx + 1, cy - 5, cx + 5, cy - 1, 5, c);
    wlwin_round_fill(w, cx + 4, cy - 8, 5, 5, 2, c);
}

static void ic_undo(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    (void)bg;
    wlwin_fill(w, cx - 4, cy - 5, 9, 2, c);
    wlrail_line(w, cx + 5, cy - 4, cx + 7, cy + 1, 2, c);
    wlrail_line(w, cx + 7, cy + 1, cx + 3, cy + 5, 2, c);
    wlrail_line(w, cx - 4, cy - 4, cx - 1, cy - 8, 2, c);
    wlrail_line(w, cx - 4, cy - 4, cx - 1, cy, 2, c);
}

static void ic_redo(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    (void)bg;
    wlwin_fill(w, cx - 5, cy - 5, 9, 2, c);
    wlrail_line(w, cx - 5, cy - 4, cx - 7, cy + 1, 2, c);
    wlrail_line(w, cx - 7, cy + 1, cx - 3, cy + 5, 2, c);
    wlrail_line(w, cx + 4, cy - 4, cx + 1, cy - 8, 2, c);
    wlrail_line(w, cx + 4, cy - 4, cx + 1, cy, 2, c);
}

static void ic_rot_r(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    (void)bg;
    wlwin_fill(w, cx - 6, cy - 7, 11, 2, c);
    wlwin_fill(w, cx - 6, cy - 7, 2, 11, c);
    wlwin_fill(w, cx - 6, cy + 3, 11, 2, c);
    wlrail_line(w, cx + 5, cy + 4, cx + 8, cy + 1, 2, c);
    wlrail_line(w, cx + 5, cy + 4, cx + 8, cy + 7, 2, c);
}

static void ic_crop(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    (void)bg;
    wlwin_fill(w, cx - 5, cy - 8, 2, 13, c);
    wlwin_fill(w, cx - 8, cy + 3, 14, 2, c);
    wlwin_fill(w, cx + 4, cy - 5, 2, 13, c);
    wlwin_fill(w, cx - 8, cy - 5, 14, 2, c);
}

static void ic_fx(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    (void)bg;
    wlwin_fill(w, cx - 7, cy - 6, 14, 2, c);                           /* three sliders */
    wlwin_fill(w, cx - 7, cy - 1, 14, 2, c);
    wlwin_fill(w, cx - 7, cy + 4, 14, 2, c);
    wlwin_round_fill(w, cx - 3, cy - 8, 5, 6, 2, c);
    wlwin_round_fill(w, cx + 2, cy - 3, 5, 6, 2, c);
    wlwin_round_fill(w, cx - 5, cy + 2, 5, 6, 2, c);
}

static void ic_fit(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    (void)bg;
    wlrail_ic_size(w, cx, cy, c, bg);
}

static void ic_new(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    wlrail_ic_new(w, cx, cy, c, bg);
}

static const int LEFT_IDS[8] = { B_SELECT, B_PEN, B_LINE, B_BOX, B_CIRCLE, B_FILL, B_ERASER, B_PICK };
#define N_RIGHT 10
static const int RIGHT_IDS[N_RIGHT] = { B_NEW, B_OPEN, B_SAVE, B_UNDO, B_REDO, B_TRANSFORM, B_FX, B_CROP, B_FIT, B_HELP };

static void rails(const struct app *a, struct wlrail *L, struct wlrail *R)
{
    static const char *const tips[B_COUNT] = {
        "Select  S", "Pen  P", "Line  L", "Box  B", "Circle  C", "Fill  F", "Eraser  E", "Picker  I",
        "New  Ctrl+N", "Open  Ctrl+O", "Save  Ctrl+S", "Undo  Ctrl+Z", "Redo  Ctrl+Y", "Turn, flip, resize", "Effects", "Crop to the selection  Enter", "Fit the window  0", "Keys  F1" };
    static void (*const icons[B_COUNT])(struct wlwin *, int, int, uint32_t, uint32_t) = {
        ic_select, ic_pen, ic_line, ic_box, ic_circle, ic_fill, ic_eraser, ic_pick,
        ic_new, wlrail_ic_open, wlrail_ic_save, ic_undo, ic_redo, ic_rot_r, ic_fx, ic_crop, ic_fit, NULL };
    const uint32_t base[B_COUNT] = {
        lm_theme.btn_slate, lm_theme.btn_blue, lm_theme.btn_teal, lm_theme.btn_purple, lm_theme.btn_purple, lm_theme.btn_orange, lm_theme.btn_rust, lm_theme.btn_sky,
        lm_theme.btn_green, lm_theme.btn_blue, lm_theme.btn_amber, lm_theme.btn_slate, lm_theme.btn_slate, lm_theme.btn_teal, lm_theme.btn_orange, lm_theme.btn_rust, lm_theme.btn_grey, lm_theme.btn_rose };
    const uint32_t lite[B_COUNT] = {
        lm_theme.btn_slate_lit, lm_theme.btn_blue_lit, lm_theme.btn_teal_lit, lm_theme.btn_purple_lit, lm_theme.btn_purple_lit, lm_theme.btn_orange_lit, lm_theme.btn_rust_lit, lm_theme.btn_sky_lit,
        lm_theme.btn_green_lit, lm_theme.btn_blue_lit, lm_theme.btn_amber_lit, lm_theme.btn_slate_lit, lm_theme.btn_slate_lit, lm_theme.btn_teal_lit, lm_theme.btn_orange_lit, lm_theme.btn_rust_lit, lm_theme.btn_grey_lit, lm_theme.btn_rose_lit };
    memset(L, 0, sizeof(*L));
    memset(R, 0, sizeof(*R));
    L->x = 0;
    R->x = a->win.bw > 0 ? a->win.bw - WLRAIL_W : 0;
    L->y0 = R->y0 = 0;
    L->y1 = R->y1 = a->win.bh > 0 ? a->win.bh - PAL_H - STAT_H : 400;
    L->n = 8;
    for (int i = 0; i < 8; i++) {
        int id = LEFT_IDS[i];
        L->it[i] = (struct wlrail_item){ .tip = tips[id], .icon = icons[id], .base = base[id], .lite = lite[id], .show = true, .lit = (int)a->tool == id };
    }
    R->n = N_RIGHT;
    for (int j = 0; j < N_RIGHT; j++) {
        int id = RIGHT_IDS[j];
        bool idle = (id == B_UNDO && !a->nundo) || (id == B_REDO && !a->nredo) || (id == B_CROP && !a->has_sel);
        R->it[j] = (struct wlrail_item){ .tip = tips[id], .icon = icons[id], .text = id == B_HELP ? "?" : NULL, .base = base[id], .lite = lite[id], .show = true,
                                         .idle = idle, .lit = (id == B_SAVE && a->modified) || (id == B_FX && a->menu_kind == 1) || (id == B_TRANSFORM && a->menu_kind == 2), .gap = id == B_UNDO || id == B_TRANSFORM,
                                         .bottom = id == B_FIT || id == B_HELP };
    }
}

static int rail_hover_of(const struct app *a, bool right)
{
    if (a->hover < 0)
        return -1;
    if (!right) {
        for (int i = 0; i < 8; i++)
            if (LEFT_IDS[i] == a->hover)
                return i;
        return -1;
    }
    for (int j = 0; j < N_RIGHT; j++)
        if (RIGHT_IDS[j] == a->hover)
            return j;
    return -1;
}

/* The button under a point (a B_* id), -1 for none. */
static int button_at(const struct app *a, double x, double y)
{
    struct wlrail L, R;
    rails(a, &L, &R);
    int i = wlrail_at(&L, x, y);
    if (i >= 0)
        return LEFT_IDS[i];
    int j = wlrail_at(&R, x, y);
    return j >= 0 ? RIGHT_IDS[j] : -1;
}

/* ---- the strip of colours and sizes --------------------------------------------------------------------------------------------- */

struct strip {
    int pal_x, pal_y, pal_s;                    /* the first colour square, its side */
    int cur_x, cur_y;                           /* the pair of current colours */
    int size_x, size_y, size_pitch;             /* the first brush dot */
    int fill_x, fill_y, fill_w, fill_h;         /* the "Filled" switch */
};

static void strip_layout(const struct app *a, struct strip *s)
{
    int y = pal_y(a);
    s->cur_x = cv_x(a) + 4;
    s->cur_y = y + 4;
    s->pal_s = 20;
    s->pal_x = s->cur_x + 44;
    s->pal_y = y + (PAL_H - s->pal_s) / 2;
    s->size_pitch = 28;
    s->size_x = s->pal_x + N_PAL * (s->pal_s + 3) + 18;
    s->size_y = y + PAL_H / 2;
    s->fill_w = 64;
    s->fill_h = 24;
    s->fill_x = s->size_x + N_SIZES * s->size_pitch + 10;
    s->fill_y = y + (PAL_H - s->fill_h) / 2;
}

static int pal_at(const struct app *a, double x, double y)
{
    struct strip s;
    strip_layout(a, &s);
    if (y < s.pal_y || y >= s.pal_y + s.pal_s)
        return -1;
    int i = (int)((x - s.pal_x) / (s.pal_s + 3));
    return x >= s.pal_x && i >= 0 && i < N_PAL && (x - s.pal_x) - i * (s.pal_s + 3) < s.pal_s ? i : -1;
}

static int size_at(const struct app *a, double x, double y)
{
    struct strip s;
    strip_layout(a, &s);
    if (y < pal_y(a) || y >= pal_y(a) + PAL_H || x < s.size_x - s.size_pitch / 2)
        return -1;
    int i = (int)((x - (s.size_x - s.size_pitch / 2)) / s.size_pitch);
    return i >= 0 && i < N_SIZES ? i : -1;
}

static bool fill_at(const struct app *a, double x, double y)
{
    struct strip s;
    strip_layout(a, &s);
    return x >= s.fill_x && x < s.fill_x + s.fill_w && y >= s.fill_y && y < s.fill_y + s.fill_h;
}

static void paint_swatch(struct app *a, int x, int y, int side, uint32_t c, int r)
{
    struct wlwin *w = &a->win;
    if (ch_a(c) == 0) {                                                 /* "transparent": the checkerboard */
        wlwin_round_fill(w, x, y, side, side, r, 0xcccccc);
        wlwin_fill(w, x + side / 2, y, side / 2 - r / 2, side / 2, 0x999999);
        wlwin_fill(w, x, y + side / 2, side / 2, side / 2 - r / 2, 0x999999);
        return;
    }
    wlwin_round_fill(w, x, y, side, side, r, c & 0xffffffu);
}

static void paint_strip(struct app *a)
{
    struct wlwin *w = &a->win;
    struct strip s;
    strip_layout(a, &s);
    wlwin_fill(w, 0, pal_y(a), w->bw, PAL_H, COL_BAR);
    paint_swatch(a, s.cur_x + 14, s.cur_y + 12, 22, a->bg, 5);          /* the second colour lies behind the first */
    wlwin_round_fill(w, s.cur_x - 1, s.cur_y - 1, 26, 26, 7, lm_theme.border);
    paint_swatch(a, s.cur_x, s.cur_y, 24, a->fg, 6);
    for (int i = 0; i < N_PAL; i++) {
        int x = s.pal_x + i * (s.pal_s + 3);
        if (PALETTE[i] == a->fg || PALETTE[i] == a->bg)
            wlwin_round_fill(w, x - 2, s.pal_y - 2, s.pal_s + 4, s.pal_s + 4, 7, PALETTE[i] == a->fg ? COL_ACCENT : COL_DIM);
        paint_swatch(a, x, s.pal_y, s.pal_s, PALETTE[i], 5);
    }
    for (int i = 0; i < N_SIZES; i++) {
        int cx = s.size_x + i * s.size_pitch, d = LM_MIN(BRUSH[i] + 2, 22);
        if (a->size == i)
            wlwin_round_fill(w, cx - 12, s.size_y - 12, 24, 24, 12, lm_theme.control_hot);
        wlwin_round_fill(w, cx - d / 2, s.size_y - d / 2, d, d, d / 2, a->size == i ? lm_theme.text_bright : COL_TEXT);
    }
    bool on = a->filled;
    wlwin_drop(w, s.fill_x, s.fill_y, s.fill_w, s.fill_h, lm_button_radius(s.fill_h / 2), on ? lm_theme.btn_green : lm_theme.btn_grey);
    const char *t = "Filled";
    wlwin_text(w, s.fill_x + (s.fill_w - wlcard_text_w(t)) / 2, s.fill_y + (s.fill_h - 2 - LM_FONT_H) / 2, t, 1, lm_theme.on_colour, 0);
}

static void paint_status(struct app *a)
{
    struct wlwin *w = &a->win;
    wlwin_fill(w, 0, stat_y(a), w->bw, STAT_H, COL_BAR);
    char left[200], right[160];
    double ix, iy;
    to_pic(a, a->px, a->py, &ix, &iy);
    const char *text = a->msg[0] && now_ms() - a->msg_ms < 6000 ? a->msg : TOOL_HINTS[a->tool];
    snprintf(left, sizeof(left), "%s", text);
    if (a->has_pointer && in_canvas(a, a->px, a->py) && ix >= 0 && iy >= 0 && ix < a->im.w && iy < a->im.h)
        snprintf(right, sizeof(right), "%d, %d     %d x %d     %d %%", (int)ix, (int)iy, a->im.w, a->im.h, (int)(a->zoom * 100.0 + 0.5));
    else
        snprintf(right, sizeof(right), "%d x %d     %d %%", a->im.w, a->im.h, (int)(a->zoom * 100.0 + 0.5));
    int rw = wlcard_text_w(right);
    wlwin_text(w, 8, stat_y(a) + (STAT_H - LM_FONT_H) / 2, left, 1, a->msg[0] && now_ms() - a->msg_ms < 6000 ? lm_theme.caution : COL_DIM, w->bw - rw - 32);
    wlwin_text(w, w->bw - rw - 10, stat_y(a) + (STAT_H - LM_FONT_H) / 2, right, 1, COL_TEXT, 0);
}


static void paint_card(struct app *a)
{
    struct wlwin *w = &a->win;
    struct wlcard c;
    char title[64] = "";
    const char *line = "", *line2 = "";
    const char *labels[3] = { "Cancel", "OK", "" };
    uint32_t cols[3] = { lm_theme.btn_grey, lm_theme.btn_blue, 0 };
    int nb = 2, ch = 150;
    bool field = true;
    switch (a->prompt) {
    case P_OPEN: snprintf(title, sizeof(title), "Open a picture"); line = "Pick a picture below or type its path (Tab completes)"; labels[1] = "Open"; break;
    case P_SAVEAS: snprintf(title, sizeof(title), "Save as"); line = "A name ending in .png (keeps transparency) or .jpg (laid on white)"; labels[1] = "Save"; cols[1] = lm_theme.btn_amber; break;
    case P_NEW: snprintf(title, sizeof(title), "New picture"); line = "The size in pixels, like 800x600"; labels[1] = "Create"; cols[1] = lm_theme.btn_green; break;
    case P_RESIZE: snprintf(title, sizeof(title), "Resize"); line = "A size (800x600), a width (800) or a percentage (50%)"; labels[1] = "Resize"; cols[1] = lm_theme.btn_purple; break;
    case P_DISCARD:
        snprintf(title, sizeof(title), "Unsaved changes");
        line = "This picture has changes that are not saved.";
        labels[0] = "Cancel";
        labels[1] = "Discard";
        labels[2] = "Save";
        cols[1] = lm_theme.btn_rust;
        cols[2] = lm_theme.btn_green;
        nb = 3;
        field = false;
        ch = 130;
        break;
    case P_REPLACE:
        snprintf(title, sizeof(title), "Replace the file?");
        line = a->pbuf;
        line2 = "It exists already.";
        labels[1] = "Replace";
        cols[1] = lm_theme.btn_rust;
        field = false;
        ch = 130;
        break;
    case P_HELP:
        snprintf(title, sizeof(title), "Keys");
        labels[0] = "Close";
        cols[0] = lm_theme.btn_blue;
        nb = 1;
        field = false;
        ch = 56 + (int)(sizeof(HELP_LINES) / sizeof(HELP_LINES[0])) * (LM_FONT_H + 4) + WLCARD_BTN_H;
        break;
    default: return;
    }
    struct wlcard geo;
    int nbg;
    card_rect(a, &geo, &nbg);
    int cw = a->prompt == P_HELP ? 620 : 560;
    ch = geo.h;
    wlcard_frame(w, &c, cw, ch, title, COL_ACCENT, COL_BG, lm_theme.text_bright);
    if (a->prompt == P_HELP) {
        for (size_t i = 0; i < sizeof(HELP_LINES) / sizeof(HELP_LINES[0]); i++)
            wlwin_text(w, c.x + WLCARD_PAD, c.y + 40 + (int)i * (LM_FONT_H + 4), HELP_LINES[i], 1, COL_TEXT, c.w - 2 * WLCARD_PAD);
    } else {
        wlwin_text(w, c.x + WLCARD_PAD, c.y + 42, line, 1, field ? COL_DIM : COL_TEXT, c.w - 2 * WLCARD_PAD);
        if (*line2)
            wlwin_text(w, c.x + WLCARD_PAD, c.y + 42 + LM_FONT_H + 4, line2, 1, COL_DIM, c.w - 2 * WLCARD_PAD);
    }
    if (field) {
        int fy = c.y + 42 + LM_FONT_H + 8;
        wlwin_round_fill(w, c.x + WLCARD_PAD, fy, c.w - 2 * WLCARD_PAD, 26, 8, COL_FIELD);
        wltext_paint(w, c.x + WLCARD_PAD + 8, fy + 5, c.w - 2 * WLCARD_PAD - 16, a->pbuf, true, COL_TEXT, lm_theme.button_hover, COL_ACCENT);
        int rows = card_list_rows(a);
        if (rows) {
            char base[PATH_MAX];
            default_dir(a, base, sizeof(base));
            int fx = c.x + WLCARD_PAD, fw = c.w - 2 * WLCARD_PAD;
            wlpicker_paint(w, &a->pk, base, a->pbuf, fx, card_list_y(&c), fw, rows, a->px, a->py, a->has_pointer);
            if (a->perr[0])
                wlwin_text(w, fx, c.y + c.h - WLCARD_PAD - WLCARD_BTN_H - 22, a->perr, 1, lm_theme.error, fw);
        } else if (a->perr[0])
            wlwin_text(w, c.x + WLCARD_PAD, fy + 32, a->perr, 1, lm_theme.error, c.w - 2 * WLCARD_PAD);
    } else if (a->perr[0] && a->prompt != P_HELP) {
        wlwin_text(w, c.x + WLCARD_PAD, c.y + c.h - WLCARD_PAD - WLCARD_BTN_H - LM_FONT_H - 6, a->perr, 1, lm_theme.error, c.w - 2 * WLCARD_PAD);
    }
    wlcard_buttons(w, &c, labels, nb, cols, a->pcard_hover, a->pcard_down, lm_theme.on_colour);
}

static void write_dump(struct app *a);

static void paint(struct wlwin *w)
{
    struct app *a = w->data;
    wlwin_fill(w, 0, 0, w->bw, w->bh, COL_BG);
    if (a->fitted && a->im.w > 0 && fabs(fit_zoom(a) - a->zoom) > 1e-9) {
        a->zoom = fit_zoom(a);
        view_center(a);
    }
    if (a->vdirty || !a->vbuf || a->vbw != cv_w(a) * wlwin_scale(w) || a->vbh != cv_h(a) * wlwin_scale(w))
        render_view(a);
    wlwin_fill(w, cv_x(a), 0, cv_w(a), cv_h(a), COL_FIELD);
    if (a->vbuf)
        wlwin_blit_px(w, cv_x(a), 0, a->vbuf, a->vbw, a->vbh, a->vbw, cv_x(a), 0, cv_w(a), cv_h(a));
    if (a->has_sel) {
        double x0 = a->vx + a->sel.x0 * a->zoom, y0 = a->vy + a->sel.y0 * a->zoom, x1 = a->vx + a->sel.x1 * a->zoom - 1, y1 = a->vy + a->sel.y1 * a->zoom - 1;
        dashed_rect(a, x0, y0, x1, y1, 0xffffff, 0x000000);
    }
    struct wlrail L, R;
    rails(a, &L, &R);
    int64_t now = now_ms();
    wlrail_paint(w, &L, &a->ran[0], rail_hover_of(a, false), now, COL_BAR, COL_BAR, COL_ACCENT, lm_theme.text);
    wlrail_paint(w, &R, &a->ran[1], rail_hover_of(a, true), now, COL_BAR, COL_BAR, COL_ACCENT, lm_theme.text);
    paint_strip(a);
    paint_status(a);
    if (a->prompt != P_NONE)
        paint_card(a);
    write_dump(a);
}

static void write_dump(struct app *a)
{
    if (!g_dump)
        return;
    FILE *f = fopen(g_dump, "w");
    if (!f)
        return;
    fprintf(f, "image w=%d h=%d hash=%016llx modified=%d path=%s\n", a->im.w, a->im.h, (unsigned long long)pix_hash(&a->im), a->modified ? 1 : 0, a->path[0] ? a->path : "-");
    fprintf(f, "state tool=%s fg=%08x bg=%08x size=%d filled=%d zoom=%d fitted=%d undo=%d redo=%d prompt=%d\n", TOOL_NAMES[a->tool], a->fg, a->bg, BRUSH[a->size], a->filled ? 1 : 0,
            (int)(a->zoom * 100.0 + 0.5), a->fitted ? 1 : 0, a->nundo, a->nredo, (int)a->prompt);
    fprintf(f, "sel %s %d,%d,%d,%d\n", a->has_sel ? "yes" : "no", a->sel.x0, a->sel.y0, a->sel.x1, a->sel.y1);
    fprintf(f, "canvas x=%d y=0 w=%d h=%d vx=%.2f vy=%.2f\n", cv_x(a), cv_w(a), cv_h(a), a->vx, a->vy);
    struct wlrail L, R;
    rails(a, &L, &R);
    int ys[WLRAIL_MAX];
    wlrail_layout(&L, ys);
    for (int i = 0; i < L.n; i++)
        if (ys[i] >= 0)
            fprintf(f, "btn %s x=%d y=%d w=%d h=%d\n", TOOL_NAMES[LEFT_IDS[i]], L.x + (WLRAIL_W - WLRAIL_D) / 2, ys[i], WLRAIL_D, WLRAIL_D);
    wlrail_layout(&R, ys);
    static const char *const names[] = { "New", "Open", "Save", "Undo", "Redo", "Transform", "Effects", "Crop", "Fit", "Help" };
    for (int j = 0; j < R.n; j++)
        if (ys[j] >= 0)
            fprintf(f, "btn %s x=%d y=%d w=%d h=%d\n", names[j], R.x + (WLRAIL_W - WLRAIL_D) / 2, ys[j], WLRAIL_D, WLRAIL_D);
    struct strip s;
    strip_layout(a, &s);
    fprintf(f, "pal x=%d y=%d side=%d pitch=%d\n", s.pal_x, s.pal_y, s.pal_s, s.pal_s + 3);
    fprintf(f, "sizes x=%d y=%d pitch=%d\n", s.size_x, s.size_y, s.size_pitch);
    fprintf(f, "filled x=%d y=%d w=%d h=%d\n", s.fill_x, s.fill_y, s.fill_w, s.fill_h);
    if (card_list_rows(a)) {                                                    /* the Open / Save as card: where the list is and what it shows */
        struct wlcard c;
        int nb;
        char base[PATH_MAX];
        card_rect(a, &c, &nb);
        default_dir(a, base, sizeof(base));
        fprintf(f, "card x=%d y=%d w=%d h=%d field=%s\n", c.x, c.y, c.w, c.h, a->pbuf);
        wlpicker_dump(f, &a->pk, base, a->pbuf, c.x + WLCARD_PAD, card_list_y(&c), c.w - 2 * WLCARD_PAD, card_list_rows(a), 0);
    }
    fclose(f);
}

/* ---- what the pointer does ------------------------------------------------------------------------------------------------------ */

static uint32_t draw_color(const struct app *a)
{
    return a->drag_button == 273 ? a->bg : a->fg;
}

static void begin_drag(struct app *a, int button)
{
    double ix, iy;
    to_pic(a, a->px, a->py, &ix, &iy);
    a->drag = true;
    a->drag_button = button;
    a->sx = a->lx = ix;
    a->sy = a->ly = iy;
    a->have_before = false;
    a->preview = (struct irect){ 0, 0, 0, 0 };
    uint32_t col = draw_color(a);
    double r = BRUSH[a->size] / 2.0;
    switch (a->tool) {
    case T_SELECT:
        a->has_sel = false;
        a->vdirty = true;
        break;
    case T_PEN:
    case T_ERASER:
        checkpoint(a);
        stroke(&a->im, ix, iy, ix, iy, r, a->tool == T_ERASER ? (button == 273 ? a->fg : a->bg) : col, NULL);
        changed(a);
        break;
    case T_LINE:
    case T_BOX:
    case T_CIRCLE:
        checkpoint(a);
        a->have_before = pix_copy(&a->im, &a->before);
        break;
    case T_FILL:
        checkpoint(a);
        {
            struct irect d = { 0, 0, 0, 0 };
            if (flood(&a->im, (int)floor(ix), (int)floor(iy), col, 24, &d))
                changed(a);
            else
                say(a, "Nothing to fill there");
        }
        a->drag = false;
        break;
    case T_PICK:
        if (ix >= 0 && iy >= 0 && ix < a->im.w && iy < a->im.h) {
            uint32_t c = a->im.p[(size_t)floor(iy) * (size_t)a->im.w + (size_t)floor(ix)];
            if (button == 273)
                a->bg = c;
            else
                a->fg = c;
            say(a, "Picked #%06x%s", c & 0xffffffu, ch_a(c) < 255 ? " (see-through)" : "");
        }
        a->drag = false;
        break;
    default: break;
    }
    a->win.redraw = true;
}

static void drag_shape(struct app *a, double ix, double iy, bool shift)
{
    if (!a->have_before)
        return;
    int x0 = (int)floor(a->sx), y0 = (int)floor(a->sy), x1 = (int)floor(ix), y1 = (int)floor(iy);
    if (shift) {
        if (a->tool == T_LINE) {
            int dx = x1 - x0, dy = y1 - y0, adx = abs(dx), ady = abs(dy);
            if (adx > 2 * ady)
                y1 = y0;
            else if (ady > 2 * adx)
                x1 = x0;
            else {
                int m = LM_MAX(adx, ady);
                x1 = x0 + (dx < 0 ? -m : m);
                y1 = y0 + (dy < 0 ? -m : m);
            }
        } else {
            int m = LM_MAX(abs(x1 - x0), abs(y1 - y0));
            x1 = x0 + (x1 < x0 ? -m : m);
            y1 = y0 + (y1 < y0 ? -m : m);
        }
    }
    if (a->preview.x1 > a->preview.x0)                                  /* put back what the last preview painted over */
        for (int y = a->preview.y0; y < a->preview.y1; y++)
            memcpy(&a->im.p[(size_t)y * (size_t)a->im.w + (size_t)a->preview.x0], &a->before.p[(size_t)y * (size_t)a->im.w + (size_t)a->preview.x0],
                   (size_t)(a->preview.x1 - a->preview.x0) * 4);
    a->preview = (struct irect){ 0, 0, 0, 0 };
    uint32_t col = draw_color(a);
    double r = BRUSH[a->size] / 2.0;
    if (a->tool == T_LINE)
        stroke(&a->im, x0 + 0.5, y0 + 0.5, x1 + 0.5, y1 + 0.5, r, col, &a->preview);
    else if (a->tool == T_BOX) {
        if (a->filled) {
            fill_rect(&a->im, LM_MIN(x0, x1), LM_MIN(y0, y1), LM_MAX(x0, x1) + 1, LM_MAX(y0, y1) + 1, col, &a->preview);
        } else
            rect_outline(&a->im, x0, y0, x1, y1, r, col, &a->preview);
    } else if (a->tool == T_CIRCLE)
        ellipse(&a->im, x0, y0, x1, y1, r, a->filled, col, &a->preview);
    clip_rect(&a->im, &a->preview);
    changed(a);
}

static void end_drag(struct app *a)
{
    if (!a->drag)
        return;
    a->drag = false;
    if (a->have_before) {
        pix_free(&a->before);
        a->have_before = false;
    }
    if (a->tool == T_SELECT) {
        sel_clip(a);
        if (a->has_sel)
            say(a, "Selected %d x %d at %d, %d", a->sel.x1 - a->sel.x0, a->sel.y1 - a->sel.y0, a->sel.x0, a->sel.y0);
    }
    a->win.redraw = true;
}

static void set_tool(struct app *a, enum tool t)
{
    a->tool = t;
    a->msg[0] = 0;
    a->win.redraw = true;
}

static void press_button(struct app *a, int id)
{
    switch (id) {
    case B_SELECT: case B_PEN: case B_LINE: case B_BOX: case B_CIRCLE: case B_FILL: case B_ERASER: case B_PICK: set_tool(a, (enum tool)id); break;
    case B_NEW: guard(a, AF_NEW); break;
    case B_OPEN: guard(a, AF_OPEN); break;
    case B_SAVE: do_save(a); break;
    case B_UNDO: do_undo(a, false); break;
    case B_REDO: do_undo(a, true); break;
    case B_TRANSFORM:
        if (wlwin_menu(&a->win, "Turn left\nTurn right\nFlip left-right\nFlip up-down\nResize..."))
            a->menu_kind = 2;
        else
            say(a, "The compositor has no menu service");
        break;
    case B_CROP: do_crop(a); break;
    case B_FX: {
        char entries[256] = "";
        for (int i = 0; i < FX_COUNT; i++) {
            strncat(entries, FX_NAMES[i], sizeof(entries) - strlen(entries) - 1);
            if (i + 1 < FX_COUNT)
                strncat(entries, "\n", sizeof(entries) - strlen(entries) - 1);
        }
        if (wlwin_menu(&a->win, entries))
            a->menu_kind = 1;
        else
            say(a, "The compositor has no menu service");
        break;
    }
    case B_FIT: view_fit(a); break;
    case B_HELP: a->prompt = P_HELP; a->pcard_hover = a->pcard_down = -1; break;
    default: break;
    }
    a->win.redraw = true;
}

static void ask_resize(struct app *a)
{
    char s[32];
    snprintf(s, sizeof(s), "%dx%d", a->im.w, a->im.h);
    prompt_open(a, P_RESIZE, s);
    wltext_select_all(a->pbuf);
}

static void do_transform(struct app *a, int what)
{
    switch (what) {
    case 0: checkpoint(a); whole_op(a, "Turned left", rotate90(&a->im, false)); break;
    case 1: checkpoint(a); whole_op(a, "Turned right", rotate90(&a->im, true)); break;
    case 2: checkpoint(a); flip(&a->im, true); whole_op(a, "Flipped left-right", true); break;
    case 3: checkpoint(a); flip(&a->im, false); whole_op(a, "Flipped up-down", true); break;
    case 4: ask_resize(a); break;
    default: break;
    }
}

static void menu_event(struct wlwin *w, int index)
{
    struct app *a = w->data;
    int kind = a->menu_kind;
    a->menu_kind = 0;
    if (kind == 1 && index >= 0 && index < FX_COUNT)
        do_effect(a, (enum fx)index);
    else if (kind == 2 && index >= 0)
        do_transform(a, index);
    else if (kind == 0 && index >= 0)                                    /* the answer of a text field's menu */
        wltext_menu_do(&a->clip, a->pbuf, sizeof(a->pbuf), index);
    w->redraw = true;
}

static void card_pointer(struct app *a, uint32_t op, const union lm_arg *args)
{
    struct wlcard c;
    int nb;
    card_rect(a, &c, &nb);
    int crows = card_list_rows(a), fx = c.x + WLCARD_PAD, fw = c.w - 2 * WLCARD_PAD, ly = card_list_y(&c);
    bool field = a->prompt == P_OPEN || a->prompt == P_SAVEAS || a->prompt == P_NEW || a->prompt == P_RESIZE;
    if (op == LM_WL_POINTER_EVT_MOTION) {
        int k = wlcard_button_at(&c, nb, (int)a->px, (int)a->py);
        if (k != a->pcard_hover)
            a->pcard_hover = k;
        if (crows) {
            char base[PATH_MAX];
            default_dir(a, base, sizeof(base));
            wlpicker_motion(&a->pk, base, a->pbuf, ly, crows, a->py);               /* (the scroll bar's thumb is dragged) */
        }
        a->win.redraw = true;                                                   /* (the list rows light under the pointer) */
        return;
    }
    if (op == LM_WL_POINTER_EVT_AXIS) {
        if (crows && args[1].u == 0) {
            wlpicker_wheel(&a->pk, args[2].f > 0 ? 1 : -1);
            a->win.redraw = true;
        }
        return;
    }
    if (op != LM_WL_POINTER_EVT_BUTTON)
        return;
    a->clip.serial = args[0].u;
    int k = wlcard_button_at(&c, nb, (int)a->px, (int)a->py);
    if (args[3].u != 1) {
        wlpicker_release(&a->pk);
        if (a->pcard_down >= 0 && a->pcard_down == k) {                                        /* a button is done when it is let go on */
            int which = k;
            a->pcard_down = -1;
            if (a->prompt == P_HELP || which == 0) {
                a->prompt = P_NONE;
                a->after = AF_NONE;
            } else if (a->prompt == P_DISCARD)
                discard_chosen(a, which == 2);
            else
                prompt_accept(a);
        }
        a->pcard_down = -1;
        a->win.redraw = true;
        return;
    }
    if (args[2].u == 272) {
        if (k >= 0) {
            a->pcard_down = k;
        } else if (!wlcard_has(&c, (int)a->px, (int)a->py)) {
            a->prompt = P_NONE;                                                                 /* a click outside the card cancels */
            a->after = AF_NONE;
        } else if (field) {
            int fy = c.y + 42 + LM_FONT_H + 8;
            if (a->py >= fy && a->py < fy + 26)
                wltext_click(a->pbuf, (int)a->px - (c.x + WLCARD_PAD + 8), (a->win.mods & LM_MOD_SHIFT) != 0);
            else if (crows) {
                char base[PATH_MAX];
                default_dir(a, base, sizeof(base));
                if (wlpicker_press(&a->pk, base, a->pbuf, sizeof(a->pbuf), fx, ly, fw, crows, a->px, a->py, args[1].u, (uint32_t)lm_metrics.double_click_ms) == WLPICKER_OPEN)
                    prompt_accept(a);
                a->perr[0] = 0;
            }
        }
        a->win.redraw = true;
    } else if (args[2].u == 273 && field && a->py >= c.y + 42 + LM_FONT_H + 8 && a->py < c.y + 42 + LM_FONT_H + 34) {
        wlwin_menu(&a->win, wltext_menu(a->pbuf));
    }
}

static void pointer_event(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
    struct app *a = w->data;
    switch (op) {
    case LM_WL_POINTER_EVT_ENTER:
        a->px = lm_fixed_to_double(args[2].f);
        a->py = lm_fixed_to_double(args[3].f);
        a->has_pointer = true;
        w->redraw = true;
        return;
    case LM_WL_POINTER_EVT_LEAVE:
        a->has_pointer = false;
        a->hover = -1;
        wlrail_release(&a->ran[0]);
        wlrail_release(&a->ran[1]);
        w->redraw = true;
        return;
    case LM_WL_POINTER_EVT_MOTION:
        a->px = lm_fixed_to_double(args[1].f);
        a->py = lm_fixed_to_double(args[2].f);
        a->has_pointer = true;
        break;
    default: break;
    }
    if (a->prompt != P_NONE) {
        card_pointer(a, op, args);
        return;
    }
    switch (op) {
    case LM_WL_POINTER_EVT_MOTION: {
        int h = button_at(a, a->px, a->py);
        if (h != a->hover)
            a->hover = h;
        if (a->pan) {
            a->vx = a->pan_vx + (a->px - a->pan_x);
            a->vy = a->pan_vy + (a->py - a->pan_y);
            a->fitted = false;
            view_clamp(a);
            a->vdirty = true;
        } else if (a->drag) {
            double ix, iy;
            to_pic(a, a->px, a->py, &ix, &iy);
            switch (a->tool) {
            case T_PEN:
            case T_ERASER: {
                uint32_t col = a->tool == T_ERASER ? (a->drag_button == 273 ? a->fg : a->bg) : draw_color(a);
                stroke(&a->im, a->lx, a->ly, ix, iy, BRUSH[a->size] / 2.0, col, NULL);
                a->lx = ix;
                a->ly = iy;
                changed(a);
                break;
            }
            case T_LINE: case T_BOX: case T_CIRCLE: drag_shape(a, ix, iy, (w->mods & LM_MOD_SHIFT) != 0); break;
            case T_SELECT:
                a->has_sel = true;
                a->sel = (struct irect){ (int)floor(LM_MIN(a->sx, ix)), (int)floor(LM_MIN(a->sy, iy)), (int)floor(LM_MAX(a->sx, ix)) + 1, (int)floor(LM_MAX(a->sy, iy)) + 1 };
                clip_rect(&a->im, &a->sel);
                break;
            default: break;
            }
        }
        w->redraw = true;
        break;
    }
    case LM_WL_POINTER_EVT_BUTTON: {
        a->clip.serial = args[0].u;
        uint32_t button = args[2].u, state = args[3].u;
        if (state != 1) {
            wlrail_release(&a->ran[0]);
            wlrail_release(&a->ran[1]);
            if (button == 274)
                a->pan = false;
            else if ((int)button == a->drag_button || a->drag)
                end_drag(a);
            w->redraw = true;
            break;
        }
        int bi = button_at(a, a->px, a->py);
        if (button == 272 && bi >= 0) {
            bool right = false;
            for (int j = 0; j < N_RIGHT; j++)
                right = right || RIGHT_IDS[j] == bi;
            wlrail_press(&a->ran[right ? 1 : 0], rail_hover_of(a, right), now_ms());
            struct wlrail L, R;
            rails(a, &L, &R);
            const struct wlrail *r = right ? &R : &L;
            int idx = rail_hover_of(a, right);
            if (idx < 0 || !r->it[idx].idle)
                press_button(a, bi);
            break;
        }
        if (a->py >= pal_y(a) && a->py < pal_y(a) + PAL_H) {                                    /* the strip: colours, sizes, Filled */
            int pi = pal_at(a, a->px, a->py), si = size_at(a, a->px, a->py);
            if (pi >= 0) {
                if (button == 273)
                    a->bg = PALETTE[pi];
                else
                    a->fg = PALETTE[pi];
            } else if (si >= 0)
                a->size = si;
            else if (fill_at(a, a->px, a->py))
                a->filled = !a->filled;
            else {
                struct strip s;
                strip_layout(a, &s);
                if (a->px >= s.cur_x && a->px < s.cur_x + 40) {                                   /* the pair of current colours: swap */
                    uint32_t t = a->fg;
                    a->fg = a->bg;
                    a->bg = t;
                }
            }
            w->redraw = true;
            break;
        }
        if (button == 274 && in_canvas(a, a->px, a->py)) {
            a->pan = true;
            a->pan_x = a->px;
            a->pan_y = a->py;
            a->pan_vx = a->vx;
            a->pan_vy = a->vy;
            break;
        }
        if ((button == 272 || (button == 273 && a->tool != T_SELECT)) && in_canvas(a, a->px, a->py))
            begin_drag(a, (int)button);
        break;
    }
    case LM_WL_POINTER_EVT_AXIS:
        if (args[1].u == 0 && in_canvas(a, a->px, a->py)) {
            double v = lm_fixed_to_double(args[2].f);
            if (v != 0)
                zoom_to(a, a->zoom * (v < 0 ? 1.25 : 1.0 / 1.25), a->px - cv_x(a), a->py);
        }
        break;
    }
}

/* ---- the keyboard --------------------------------------------------------------------------------------------------------------- */

static void prompt_key(struct app *a, uint32_t ks)
{
    if (a->prompt == P_HELP) {
        if (ks == LM_KS_Escape || ks == LM_KS_Return || ks == LM_KS_KP_Enter || ks == ' ')
            a->prompt = P_NONE;
        return;
    }
    if (a->prompt == P_DISCARD) {
        if (ks == LM_KS_Escape || ks == 'c' || ks == 'C') {
            a->prompt = P_NONE;
            a->after = AF_NONE;
        } else if (ks == 'd' || ks == 'D')
            discard_chosen(a, false);
        else if (ks == LM_KS_Return || ks == LM_KS_KP_Enter || ks == 's' || ks == 'S')
            discard_chosen(a, true);
        return;
    }
    if (a->prompt == P_REPLACE) {
        if (ks == LM_KS_Escape || ks == 'n' || ks == 'N')
            a->prompt = P_NONE;
        else if (ks == LM_KS_Return || ks == LM_KS_KP_Enter || ks == 'y' || ks == 'Y')
            prompt_accept(a);
        return;
    }
    if (ks == LM_KS_Escape) {
        a->prompt = P_NONE;
        a->after = AF_NONE;
    } else if (card_list_rows(a) && (ks == LM_KS_Up || ks == LM_KS_Down || ks == LM_KS_Page_Up || ks == LM_KS_Page_Down ||
                                     ((ks == LM_KS_Return || ks == LM_KS_KP_Enter) && a->pk.sel >= 0))) {
        char base[PATH_MAX];
        default_dir(a, base, sizeof(base));
        if (wlpicker_key(&a->pk, base, a->pbuf, sizeof(a->pbuf), card_list_rows(a), ks) == WLPICKER_OPEN)
            prompt_accept(a);                                                   /* a folder was entered, a picture is taken */
        a->perr[0] = 0;
    } else if (ks == LM_KS_Return || ks == LM_KS_KP_Enter) {
        prompt_accept(a);
    } else if (ks == LM_KS_Tab && (a->prompt == P_OPEN || a->prompt == P_SAVEAS)) {
        char base[PATH_MAX], list[512];
        default_dir(a, base, sizeof(base));
        int n = lm_complete_path(base, a->pbuf, sizeof(a->pbuf), true, list, sizeof(list));
        if (n > 1)
            snprintf(a->perr, sizeof(a->perr), "%s", list);
        else
            a->perr[0] = 0;
        wltext_end(a->pbuf);
        wlpicker_typed(&a->pk);
    } else if (wltext_edit(&a->clip, a->pbuf, sizeof(a->pbuf), ks, a->win.mods)) {
        a->perr[0] = 0;
        wlpicker_typed(&a->pk);                                                 /* (the list follows what is typed) */
    }
}

static void keyboard_event(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
    struct app *a = w->data;
    if (op != LM_WL_KEYBOARD_EVT_KEY)
        return;
    a->clip.serial = args[0].u;
    if (args[3].u == 0)
        return;
    uint32_t ks = lm_keymap_keysym(args[2].u, w->mods);
    bool ctrl = (w->mods & LM_MOD_CTRL) != 0, shift = (w->mods & LM_MOD_SHIFT) != 0, alt = (w->mods & LM_MOD_ALT) != 0;
    w->redraw = true;
    if (a->prompt != P_NONE) {
        prompt_key(a, ks);
        return;
    }
    char txt[8];
    int tl = lm_keysym_to_utf8(ks, txt);
    char c = tl == 1 && !ctrl && !alt ? txt[0] : 0;
    if (ctrl && (ks == LM_KS_Left || ks == LM_KS_Right)) {
        do_transform(a, ks == LM_KS_Left ? 0 : 1);
        return;
    }
    if (ctrl) {
        char k = tl == 1 ? (char)tolower((unsigned char)txt[0]) : 0;
        switch (k) {
        case 'o': guard(a, AF_OPEN); break;
        case 'n': guard(a, AF_NEW); break;
        case 's': if (shift) ask_saveas(a); else do_save(a); break;
        case 'z': do_undo(a, shift); break;
        case 'y': do_undo(a, true); break;
        case 'q': guard(a, AF_QUIT); break;
        case 'r': ask_resize(a); break;
        case 'a':
            a->has_sel = true;
            a->sel = (struct irect){ 0, 0, a->im.w, a->im.h };
            break;
        default: break;
        }
        return;
    }
    switch (ks) {
    case LM_KS_Escape:
        if (a->has_sel)
            a->has_sel = false;
        break;
    case LM_KS_Return:
    case LM_KS_KP_Enter: do_crop(a); break;
    case LM_KS_Delete: do_delete_sel(a); break;
    case LM_KS_F1: a->prompt = P_HELP; a->pcard_hover = a->pcard_down = -1; break;
    case LM_KS_Left: case LM_KS_Right: case LM_KS_Up: case LM_KS_Down: {
        double step = shift ? 200.0 : 40.0;
        a->vx += ks == LM_KS_Left ? step : ks == LM_KS_Right ? -step : 0.0;
        a->vy += ks == LM_KS_Up ? step : ks == LM_KS_Down ? -step : 0.0;
        a->fitted = false;
        view_clamp(a);
        a->vdirty = true;
        break;
    }
    default: break;
    }
    if (!c)
        return;
    for (int t = 0; t < T_COUNT; t++)
        if (c == TOOL_KEYS[t] || c == toupper((unsigned char)TOOL_KEYS[t])) {
            set_tool(a, (enum tool)t);
            return;
        }
    switch (c) {
    case '[': a->size = LM_MAX(a->size - 1, 0); break;
    case ']': a->size = LM_MIN(a->size + 1, N_SIZES - 1); break;
    case 'x': case 'X': {
        uint32_t t = a->fg;
        a->fg = a->bg;
        a->bg = t;
        break;
    }
    case '+': case '=': zoom_to(a, a->zoom * 1.25, cv_w(a) / 2.0, cv_h(a) / 2.0); break;
    case '-': zoom_to(a, a->zoom / 1.25, cv_w(a) / 2.0, cv_h(a) / 2.0); break;
    case '0': view_fit(a); break;
    case '1': zoom_to(a, 1.0, cv_w(a) / 2.0, cv_h(a) / 2.0); break;
    default: break;
    }
}

static bool close_event(struct wlwin *w)
{
    struct app *a = w->data;
    if (!a->modified)
        return true;
    guard(a, AF_QUIT);                                                  /* asks; the window stays until the answer */
    w->redraw = true;
    return false;
}

static void global_event(struct wlwin *w, uint32_t name, const char *iface, uint32_t version)
{
    struct app *a = w->data;
    wlclip_global(&a->clip, w, name, iface, version);
}

/* ---- main ------------------------------------------------------------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    wlwin_hidpi();                                                      /* draws through the wlwin_* calls only: sharp on a scaled screen */
    lm_theme_load_env();
    signal(SIGPIPE, SIG_IGN);
    if (argc > 1 && !strcmp(argv[1], "--selftest"))
        return selftest();
    if (argc > 1 && !strcmp(argv[1], "--apply"))
        return apply_cmd(argc - 2, argv + 2);
    static struct app a;
    wlpicker_init(&a.pk, card_name_ok);
    int nw = 0, nh = 0, win_w = 1000, win_h = 680;
    const char *file = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--new") && i + 1 < argc) {
            char junk;
            if (sscanf(argv[++i], "%dx%d%c", &nw, &nh, &junk) != 2 || nw < 1 || nh < 1) {
                fprintf(stderr, "mellagui-paint: --new wants a size like 800x600\n");
                return 2;
            }
        } else if (!strcmp(argv[i], "--size") && i + 1 < argc) {
            char junk;
            if (sscanf(argv[++i], "%dx%d%c", &win_w, &win_h, &junk) != 2 || win_w < 100 || win_h < 100) {
                fprintf(stderr, "mellagui-paint: --size wants a size like 900x600\n");
                return 2;
            }
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "usage: mellagui-paint [--size WxH] [FILE | --new WxH [FILE]] | --apply IN OUT OP... | --selftest\n");
            return 2;
        } else if (!file) {
            file = argv[i];
        } else {
            fprintf(stderr, "usage: mellagui-paint [FILE | --new WxH [FILE] | --apply IN OUT OP... | --selftest]\n");
            return 2;
        }
    }
    g_dump = getenv("MELLAGUI_PAINT_DUMP");
    if (!getcwd(g_startdir, sizeof(g_startdir)))
        g_startdir[0] = 0;
    a.hover = -1;
    a.fg = 0xff000000u;
    a.bg = 0xffffffffu;
    a.size = 2;
    a.tool = T_PEN;
    a.pcard_hover = a.pcard_down = -1;
    wlrail_init(&a.ran[0]);
    wlrail_init(&a.ran[1]);
    static const struct wlwin_ops ops = { .paint = paint, .global = global_event, .pointer = pointer_event, .keyboard = keyboard_event, .close = close_event, .menu = menu_event };
    a.zoom = 1.0;
    a.fitted = true;
    if (wlwin_connect(&a.win, "mellagui-paint", &ops, &a) < 0)
        return 1;
    if (wlwin_open(&a.win, "Paint", "mellagui.paint", win_w, win_h, 680, 460) < 0)
        return 1;
    wlclip_attach(&a.clip, &a.win);
    if (nw > 0) {
        new_image(&a, nw, nh, file ? file : "");
    } else if (file) {
        if (!open_image(&a, file)) {                                    /* a name that does not exist yet: an empty picture that Save puts there */
            struct stat st;
            if (stat(file, &st) != 0)
                new_image(&a, 800, 600, file);
            else
                new_image(&a, 800, 600, "");
            say(&a, "Could not open %s: it is a new picture", file);
        }
    } else {
        new_image(&a, 800, 600, "");
    }
    while (a.win.running) {
        int timeout = -1;
        struct wlrail L, R;
        rails(&a, &L, &R);
        timeout = wlrail_loop(&a.win, &a.ran[0], &L, rail_hover_of(&a, false), timeout);
        timeout = wlrail_loop(&a.win, &a.ran[1], &R, rail_hover_of(&a, true), timeout);
        if (a.msg[0] && now_ms() - a.msg_ms < 6200 && (timeout < 0 || timeout > 500))
            timeout = 500;                                                /* the message goes away by itself */
        if (!wlwin_step(&a.win, timeout))
            break;
        if (a.msg[0] && now_ms() - a.msg_ms >= 6000) {
            a.msg[0] = 0;
            a.win.redraw = true;
        }
    }
    drop_snaps(a.undo, &a.nundo);
    drop_snaps(a.redo, &a.nredo);
    pix_free(&a.im);
    if (a.have_before)
        pix_free(&a.before);
    free(a.vbuf);
    wlwin_destroy(&a.win);
    return 0;
}
