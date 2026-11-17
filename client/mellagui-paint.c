/*
 * mellagui-paint - a picture editor with layers.
 *
 *   mellagui-paint [FILE]                     open a picture (PNG, JPEG, GIF, BMP, WebP, SVG ..., or an OpenRaster .ora with its layers), or an empty one
 *   mellagui-paint --new WxH [FILE]           a white picture of that size (FILE is where Save puts it)
 *   mellagui-paint --size WxH ...             the size the window opens with (default 1000x680)
 *   mellagui-paint --apply IN OUT [OP...]    no window: the operations in order, the result to OUT (any format below; no OP = a conversion)
 *                 OP = rotate-left rotate-right flip-h flip-v resize=WxH resize=PCT% crop=X,Y,W,H gray sepia invert brighter darker
 *                      more-contrast less-contrast blur sharpen pixelate emboss edges noise
 *   mellagui-paint --selftest
 *
 * The window has the look of the other programs: a rail of round drop buttons at the left (the tools) and one at the right (file, undo, turn, flip,
 * size, effects), the layers at the right of the picture, the colours and the tool's own sliders in a strip under it, the status line at the very bottom.
 *
 *   Tools      Select (rectangle, ellipse, lasso, magic wand: the first button, click it again for the others), Move, Pen, Airbrush, Eraser,
 *              Shapes (line, box, circle, polygon / star), Fill, Gradient, Text, Picker.  Keys S O Q W M P A E L B C N F G T I.
 *              Shift keeps a line at 45 degrees and a box / circle square.
 *   Sliders    the strip shows what the tool has: Size, Opacity, Hardness (soft edges), Tolerance, Corner, Sides ...; a click or a drag sets one,
 *              the wheel over it steps it, [ and ] change the size.
 *   Colours    the two squares at the left of the strip open the colour picker (a hue bar, a square of saturation and brightness, an alpha bar and a hex
 *              field); under them the colours used lately. Left click = the pen colour, right click = the second colour; X swaps the two.
 *   Layers     the panel at the right of the picture: a click chooses, the eye hides, double click renames, + new, duplicate, delete, up, down, merge down;
 *              opacity and blend mode of the layer in the panel. Everything is drawn on the chosen layer. .ora keeps the layers (OpenRaster: GIMP,
 *              Krita and MyPaint read it); .png .jpg .webp (lossless) .gif .bmp .tga .tif .ppm get the flattened picture. The Save as card has a chip for each.
 *   Selection  Ctrl+A all, Ctrl+D none, Ctrl+I invert, Delete clears it, Backspace fills it, Ctrl+C / X / V copy, cut, paste (as a new layer), Enter crops.
 *   Text       click, type (Enter a new line, Esc cancels, Ctrl+Enter or a click elsewhere puts it on the layer); Size = the height of the letters.
 *   Picture    Ctrl+O open, Ctrl+N new, Ctrl+S save, Ctrl+Shift+S save as, Ctrl+Z undo, Ctrl+Y or Ctrl+Shift+Z redo, Ctrl+R resize, canvas size in the
 *              Transform menu (the picture stays, the sheet grows or shrinks round it).
 *   View       wheel zooms, middle button drags, + - zoom, 0 fits the window, 1 is the real size, the arrow keys move the picture, G a pixel grid.
 *
 * Colours of the strip, the rails and the cards come from the theme (like the other programs); the pictures' own colours are the picture's.
 * `$MELLAGUI_PAINT_DUMP=FILE` writes the state after every paint (tests).
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
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

#include "arcio.h"
#include "deflate.h"
#include "font.h"
#include "img.h"
#include "imgwrite.h"
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
#define PAL_H 46
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

/* ---- coverage: a tool first draws a mask (0..255 a pixel), which is then laid on the layer with a colour, an opacity and the selection ---------- */

struct cov {
    uint8_t *p;
    int w, h;
};

static bool cov_alloc(struct cov *c, int w, int h)
{
    c->p = calloc((size_t)w * (size_t)h, 1);
    c->w = c->p ? w : 0;
    c->h = c->p ? h : 0;
    return c->p != NULL;
}

static void cov_free(struct cov *c)
{
    free(c->p);
    c->p = NULL;
    c->w = c->h = 0;
}

static void irect_clip(struct irect *r, int w, int h)
{
    r->x0 = LM_CLAMP(r->x0, 0, w);
    r->y0 = LM_CLAMP(r->y0, 0, h);
    r->x1 = LM_CLAMP(r->x1, r->x0, w);
    r->y1 = LM_CLAMP(r->y1, r->y0, h);
}

static bool irect_empty(const struct irect *r)
{
    return r->x1 <= r->x0 || r->y1 <= r->y0;
}

static void cov_clear(struct cov *c, struct irect r)
{
    irect_clip(&r, c->w, c->h);
    for (int y = r.y0; y < r.y1; y++)
        memset(&c->p[(size_t)y * (size_t)c->w + (size_t)r.x0], 0, (size_t)(r.x1 - r.x0));
}

static double smooth01(double t)
{
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return t * t * (3.0 - 2.0 * t);
}

/* A line with round ends, `r` the radius in pixels, `hard` 0..1: 1 is a sharp (anti-aliased) edge, 0 fades out over the whole radius. */
static void cov_stroke(struct cov *c, double x0, double y0, double x1, double y1, double r, double hard, struct irect *dirty)
{
    double ext = r + 1.5;
    int bx0 = (int)floor(LM_MIN(x0, x1) - ext), by0 = (int)floor(LM_MIN(y0, y1) - ext);
    int bx1 = (int)ceil(LM_MAX(x0, x1) + ext), by1 = (int)ceil(LM_MAX(y0, y1) + ext);
    bx0 = LM_MAX(bx0, 0);
    by0 = LM_MAX(by0, 0);
    bx1 = LM_MIN(bx1, c->w);
    by1 = LM_MIN(by1, c->h);
    if (bx1 <= bx0 || by1 <= by0)
        return;
    double dx = x1 - x0, dy = y1 - y0, len2 = dx * dx + dy * dy, edge = r + 0.5, inner = r * hard;
    for (int y = by0; y < by1; y++)
        for (int x = bx0; x < bx1; x++) {
            double px = x + 0.5 - x0, py = y + 0.5 - y0;
            double t = len2 > 0 ? (px * dx + py * dy) / len2 : 0;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            double ex = px - t * dx, ey = py - t * dy, d = sqrt(ex * ex + ey * ey), cv;
            if (hard >= 0.999)
                cv = edge - d;
            else
                cv = d <= inner ? 1.0 : smooth01((edge - d) / (edge - inner));
            if (cv <= 0)
                continue;
            uint8_t v = cv >= 1 ? 255 : (uint8_t)(cv * 255 + 0.5);
            uint8_t *q = &c->p[(size_t)y * (size_t)c->w + (size_t)x];
            if (v > *q)
                *q = v;
        }
    if (dirty)
        irect_add(dirty, bx0, by0, bx1, by1);
}

/* A box (its corners rounded by `rad`), solid or an outline `r` wide (the outline's middle line runs along the box's edge). */
static void cov_rrect(struct cov *c, int x0, int y0, int x1, int y1, double rad, double r, bool filled, struct irect *dirty)
{
    int lx = LM_MIN(x0, x1), hx = LM_MAX(x0, x1), ly = LM_MIN(y0, y1), hy = LM_MAX(y0, y1);
    double ax = filled ? lx : lx + 0.5, ay = filled ? ly : ly + 0.5, bx = filled ? hx + 1 : hx + 0.5, by = filled ? hy + 1 : hy + 0.5;
    double cx = (ax + bx) / 2, cy = (ay + by) / 2, hw = (bx - ax) / 2, hh = (by - ay) / 2;
    rad = rad < 0 ? 0 : rad > LM_MIN(hw, hh) ? LM_MIN(hw, hh) : rad;
    double ext = filled ? 1.5 : r + 1.5;
    int bx0 = LM_MAX((int)floor(ax - ext), 0), by0 = LM_MAX((int)floor(ay - ext), 0), bx1 = LM_MIN((int)ceil(bx + ext), c->w), by1 = LM_MIN((int)ceil(by + ext), c->h);
    for (int y = by0; y < by1; y++)
        for (int x = bx0; x < bx1; x++) {
            double px = fabs(x + 0.5 - cx) - (hw - rad), py = fabs(y + 0.5 - cy) - (hh - rad);
            double ox = px > 0 ? px : 0, oy = py > 0 ? py : 0;
            double sd = sqrt(ox * ox + oy * oy) + fmin(fmax(px, py), 0.0) - rad;
            double cv = filled ? 0.5 - sd : r + 0.5 - fabs(sd);
            if (cv <= 0)
                continue;
            uint8_t v = cv >= 1 ? 255 : (uint8_t)(cv * 255 + 0.5);
            uint8_t *q = &c->p[(size_t)y * (size_t)c->w + (size_t)x];
            if (v > *q)
                *q = v;
        }
    if (dirty && bx1 > bx0 && by1 > by0)
        irect_add(dirty, bx0, by0, bx1, by1);
}

static void cov_ellipse(struct cov *c, int x0, int y0, int x1, int y1, double r, bool filled, struct irect *dirty)
{
    double cx = (LM_MIN(x0, x1) + LM_MAX(x0, x1) + 1) / 2.0, cy = (LM_MIN(y0, y1) + LM_MAX(y0, y1) + 1) / 2.0;
    double rx = (LM_MAX(x0, x1) - LM_MIN(x0, x1) + 1) / 2.0, ry = (LM_MAX(y0, y1) - LM_MIN(y0, y1) + 1) / 2.0;
    if (filled) {
        int bx0 = LM_MAX((int)floor(cx - rx - 1), 0), by0 = LM_MAX((int)floor(cy - ry - 1), 0);
        int bx1 = LM_MIN((int)ceil(cx + rx + 1), c->w), by1 = LM_MIN((int)ceil(cy + ry + 1), c->h);
        double m = LM_MIN(rx, ry);
        for (int y = by0; y < by1; y++)
            for (int x = bx0; x < bx1; x++) {
                double nx = (x + 0.5 - cx) / rx, ny = (y + 0.5 - cy) / ry, q = sqrt(nx * nx + ny * ny);
                double cv = (1.0 - q) * m + 0.5;
                if (cv <= 0)
                    continue;
                uint8_t v = cv >= 1 ? 255 : (uint8_t)(cv * 255 + 0.5);
                uint8_t *p = &c->p[(size_t)y * (size_t)c->w + (size_t)x];
                if (v > *p)
                    *p = v;
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
        cov_stroke(c, px, py, qx, qy, r, 1.0, dirty);
        px = qx;
        py = qy;
    }
}

#define POLY_MAX 64

/* A solid polygon (even-odd): four sub-scanlines a row, exact along the row. */
static void cov_fill_poly(struct cov *c, const double *xs, const double *ys, int n, struct irect *dirty)
{
    if (n < 3 || n > 200000)
        return;
    double minx = xs[0], maxx = xs[0], miny = ys[0], maxy = ys[0];
    for (int i = 1; i < n; i++) {
        minx = LM_MIN(minx, xs[i]);
        maxx = LM_MAX(maxx, xs[i]);
        miny = LM_MIN(miny, ys[i]);
        maxy = LM_MAX(maxy, ys[i]);
    }
    int bx0 = LM_MAX((int)floor(minx), 0), bx1 = LM_MIN((int)ceil(maxx) + 1, c->w), by0 = LM_MAX((int)floor(miny), 0), by1 = LM_MIN((int)ceil(maxy) + 1, c->h);
    if (bx1 <= bx0 || by1 <= by0)
        return;
    float *acc = malloc(((size_t)(bx1 - bx0) + 2) * sizeof(float));
    double *xc = malloc((size_t)n * sizeof(double));
    if (!acc || !xc) {
        free(acc);
        free(xc);
        return;
    }
    for (int y = by0; y < by1; y++) {
        memset(acc, 0, ((size_t)(bx1 - bx0) + 2) * sizeof(float));
        for (int s = 0; s < 4; s++) {
            double sy = y + (s + 0.5) / 4.0;
            int k = 0;
            for (int i = 0, j = n - 1; i < n; j = i++)
                if ((ys[i] <= sy) != (ys[j] <= sy))
                    xc[k++] = xs[i] + (sy - ys[i]) * (xs[j] - xs[i]) / (ys[j] - ys[i]);
            for (int i = 1; i < k; i++) {                               /* a few numbers: insertion sort */
                double v = xc[i];
                int j = i - 1;
                while (j >= 0 && xc[j] > v) {
                    xc[j + 1] = xc[j];
                    j--;
                }
                xc[j + 1] = v;
            }
            for (int i = 0; i + 1 < k; i += 2) {
                double xa = LM_MAX(xc[i], (double)bx0), xb = LM_MIN(xc[i + 1], (double)bx1);
                for (int px = (int)floor(xa); px < bx1 && px < xb; px++) {
                    double ov = LM_MIN(xb, px + 1.0) - LM_MAX(xa, (double)px);
                    if (ov > 0)
                        acc[px - bx0] += (float)(0.25 * ov);
                }
            }
        }
        for (int x = bx0; x < bx1; x++) {
            float v = acc[x - bx0];
            if (v <= 0.002f)
                continue;
            uint8_t b = v >= 1.0f ? 255 : (uint8_t)(v * 255 + 0.5f);
            uint8_t *q = &c->p[(size_t)y * (size_t)c->w + (size_t)x];
            if (b > *q)
                *q = b;
        }
    }
    free(acc);
    free(xc);
    if (dirty)
        irect_add(dirty, bx0, by0, bx1, by1);
}

/* The corners of a regular polygon (or a star) that fits the box. */
static int shape_points(int x0, int y0, int x1, int y1, int sides, bool star, double *xs, double *ys)
{
    double cx = (LM_MIN(x0, x1) + LM_MAX(x0, x1) + 1) / 2.0, cy = (LM_MIN(y0, y1) + LM_MAX(y0, y1) + 1) / 2.0;
    double rx = (LM_MAX(x0, x1) - LM_MIN(x0, x1) + 1) / 2.0, ry = (LM_MAX(y0, y1) - LM_MIN(y0, y1) + 1) / 2.0;
    sides = LM_CLAMP(sides, 3, POLY_MAX / 2);
    int n = star ? sides * 2 : sides;
    for (int i = 0; i < n; i++) {
        double ang = -M_PI / 2 + 2.0 * M_PI * i / n, k = star && (i & 1) ? 0.42 : 1.0;
        xs[i] = cx + rx * k * cos(ang);
        ys[i] = cy + ry * k * sin(ang);
    }
    return n;
}

static void cov_shape(struct cov *c, int x0, int y0, int x1, int y1, int sides, bool star, double r, bool filled, struct irect *dirty)
{
    double xs[POLY_MAX], ys[POLY_MAX];
    int n = shape_points(x0, y0, x1, y1, sides, star, xs, ys);
    if (filled)
        cov_fill_poly(c, xs, ys, n, dirty);
    else
        for (int i = 0; i < n; i++)
            cov_stroke(c, xs[i], ys[i], xs[(i + 1) % n], ys[(i + 1) % n], r, 1.0, dirty);
}

/* A line with an arrow head at its end. */
static void cov_arrow(struct cov *c, double x0, double y0, double x1, double y1, double r, struct irect *dirty)
{
    cov_stroke(c, x0, y0, x1, y1, r, 1.0, dirty);
    double dx = x1 - x0, dy = y1 - y0, len = sqrt(dx * dx + dy * dy);
    if (len < 2)
        return;
    double head = LM_CLAMP(r * 5.0 + 8.0, 8.0, len * 0.6), ux = dx / len, uy = dy / len;
    for (int side = -1; side <= 1; side += 2) {
        double ang = side * 0.45, ca = cos(ang), sa = sin(ang);
        double bx = -(ux * ca - uy * sa) * head, by = -(ux * sa + uy * ca) * head;
        cov_stroke(c, x1, y1, x1 + bx, y1 + by, r, 1.0, dirty);
    }
}

/* The connected area of nearly the same colour (or every such pixel with `global`) as a mask. */
static bool cov_flood(const struct pix *im, int sx, int sy, int tol, bool global, struct cov *c, struct irect *dirty)
{
    if (sx < 0 || sy < 0 || sx >= im->w || sy >= im->h || c->w != im->w || c->h != im->h)
        return false;
    uint32_t target = im->p[(size_t)sy * (size_t)im->w + (size_t)sx];
#define NEAR(c) (abs(ch_a(c) - ch_a(target)) <= tol && abs(ch_r(c) - ch_r(target)) <= tol && abs(ch_g(c) - ch_g(target)) <= tol && abs(ch_b(c) - ch_b(target)) <= tol)
    if (global) {
        for (int y = 0; y < im->h; y++)
            for (int x = 0; x < im->w; x++)
                if (NEAR(im->p[(size_t)y * (size_t)im->w + (size_t)x]))
                    c->p[(size_t)y * (size_t)im->w + (size_t)x] = 255;
        irect_add(dirty, 0, 0, im->w, im->h);
        return true;
    }
    size_t cap = 1024, n = 1;
    int *stack = malloc(cap * 2 * sizeof(int));
    if (!stack)
        return false;
    stack[0] = sx;
    stack[1] = sy;
    while (n) {
        int x = stack[(n - 1) * 2], y = stack[(n - 1) * 2 + 1];
        n--;
        uint8_t *row = &c->p[(size_t)y * (size_t)im->w];
        const uint32_t *src = &im->p[(size_t)y * (size_t)im->w];
        if (row[x] || !NEAR(src[x]))
            continue;
        int l = x, r = x;
        while (l > 0 && !row[l - 1] && NEAR(src[l - 1]))
            l--;
        while (r < im->w - 1 && !row[r + 1] && NEAR(src[r + 1]))
            r++;
        for (int i = l; i <= r; i++)
            row[i] = 255;
        irect_add(dirty, l, y, r + 1, y + 1);
        for (int dy = -1; dy <= 1; dy += 2) {
            int yy = y + dy;
            if (yy < 0 || yy >= im->h)
                continue;
            const uint8_t *crow = &c->p[(size_t)yy * (size_t)im->w];
            const uint32_t *s2 = &im->p[(size_t)yy * (size_t)im->w];
            bool run = false;
            for (int i = l; i <= r; i++) {
                bool ok = !crow[i] && NEAR(s2[i]);
                if (ok && !run) {
                    if (n + 1 > cap) {
                        cap *= 2;
                        int *ns = realloc(stack, cap * 2 * sizeof(int));
                        if (!ns) {
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
    free(stack);
    return true;
}

/* ---- the selection: a box and, unless it is a plain rectangle, a mask the size of the box ------------------------------------------ */

struct sel {
    bool on;
    struct irect r;
    uint8_t *mask;                      /* (r.x1 - r.x0) * (r.y1 - r.y0) bytes, NULL = the whole rectangle */
};

static int sel_at(const struct sel *s, int x, int y)
{
    if (!s->on)
        return 255;
    if (x < s->r.x0 || x >= s->r.x1 || y < s->r.y0 || y >= s->r.y1)
        return 0;
    return s->mask ? s->mask[(size_t)(y - s->r.y0) * (size_t)(s->r.x1 - s->r.x0) + (size_t)(x - s->r.x0)] : 255;
}

static void sel_clear(struct sel *s)
{
    free(s->mask);
    memset(s, 0, sizeof(*s));
}

/* The selection becomes what `c` covers inside `r` (a box that has nothing in it is no selection). */
static bool sel_from_cov(struct sel *s, const struct cov *c, struct irect r)
{
    irect_clip(&r, c->w, c->h);
    int x0 = r.x1, y0 = r.y1, x1 = r.x0, y1 = r.y0;
    for (int y = r.y0; y < r.y1; y++)
        for (int x = r.x0; x < r.x1; x++)
            if (c->p[(size_t)y * (size_t)c->w + (size_t)x]) {
                x0 = LM_MIN(x0, x);
                y0 = LM_MIN(y0, y);
                x1 = LM_MAX(x1, x + 1);
                y1 = LM_MAX(y1, y + 1);
            }
    sel_clear(s);
    if (x1 <= x0 || y1 <= y0)
        return false;
    uint8_t *m = malloc((size_t)(x1 - x0) * (size_t)(y1 - y0));
    if (!m)
        return false;
    for (int y = y0; y < y1; y++)
        memcpy(&m[(size_t)(y - y0) * (size_t)(x1 - x0)], &c->p[(size_t)y * (size_t)c->w + (size_t)x0], (size_t)(x1 - x0));
    s->on = true;
    s->r = (struct irect){ x0, y0, x1, y1 };
    s->mask = m;
    return true;
}

/* Lays `col` (alpha counts) over `before` where `c` covers, with `opacity` (0..100) and only inside the selection; erase = takes the layer's alpha
 * away instead. Writes into `dst` the pixels of `r`, which are `before`'s again where nothing is covered (so a preview can be redrawn). */
static void cov_commit(struct pix *dst, const struct pix *before, const struct cov *c, struct irect r, uint32_t col, int opacity, bool erase, const struct sel *sel)
{
    irect_clip(&r, dst->w, dst->h);
    for (int y = r.y0; y < r.y1; y++)
        for (int x = r.x0; x < r.x1; x++) {
            size_t i = (size_t)y * (size_t)dst->w + (size_t)x;
            int cv = c->p[i];
            if (cv && sel && sel->on)
                cv = cv * sel_at(sel, x, y) / 255;
            int k = cv * opacity / 100;
            if (k <= 0)
                dst->p[i] = before->p[i];
            else
                dst->p[i] = erase ? blend(before->p[i], 0, k) : blend(before->p[i], col, k);
        }
}

/* A gradient from `c0` to `c1` along (x0,y0) - (x1,y1), or round about (x0,y0) with the radius to (x1,y1). */
static void gradient_commit(struct pix *dst, const struct pix *before, struct irect r, double x0, double y0, double x1, double y1, bool radial, uint32_t c0, uint32_t c1,
                            int opacity, const struct sel *sel)
{
    irect_clip(&r, dst->w, dst->h);
    double dx = x1 - x0, dy = y1 - y0, len2 = dx * dx + dy * dy, len = sqrt(len2);
    for (int y = r.y0; y < r.y1; y++)
        for (int x = r.x0; x < r.x1; x++) {
            size_t i = (size_t)y * (size_t)dst->w + (size_t)x;
            int cv = sel_at(sel, x, y);
            if (!cv) {
                dst->p[i] = before->p[i];
                continue;
            }
            double px = x + 0.5 - x0, py = y + 0.5 - y0, t;
            if (radial)
                t = len > 0 ? sqrt(px * px + py * py) / len : 1.0;
            else
                t = len2 > 0 ? (px * dx + py * dy) / len2 : 1.0;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            int a = (int)(ch_a(c0) + (ch_a(c1) - ch_a(c0)) * t + 0.5);
            uint32_t col = argb(a, (int)(ch_r(c0) + (ch_r(c1) - ch_r(c0)) * t + 0.5), (int)(ch_g(c0) + (ch_g(c1) - ch_g(c0)) * t + 0.5), (int)(ch_b(c0) + (ch_b(c1) - ch_b(c0)) * t + 0.5));
            int k = cv * opacity / 100;
            dst->p[i] = a <= 0 || k <= 0 ? before->p[i] : blend(before->p[i], col, k);
        }
}
/* ---- effects and changes of the whole picture ------------------------------------------------------------------------------------ */

enum fx { FX_GRAY, FX_SEPIA, FX_INVERT, FX_BRIGHTER, FX_DARKER, FX_MORE_CONTRAST, FX_LESS_CONTRAST, FX_BLUR, FX_SHARPEN, FX_PIXELATE, FX_EMBOSS, FX_EDGES, FX_NOISE, FX_COUNT };
static const char *const FX_NAMES[FX_COUNT] = { "Grayscale", "Sepia", "Invert", "Brighter", "Darker", "More contrast", "Less contrast", "Blur", "Sharpen", "Pixelate", "Emboss", "Edges", "Noise" };
static const char *const FX_OPS[FX_COUNT] = { "gray", "sepia", "invert", "brighter", "darker", "more-contrast", "less-contrast", "blur", "sharpen", "pixelate", "emboss", "edges", "noise" };

static void clip_rect(const struct pix *im, struct irect *r)
{
    irect_clip(r, im->w, im->h);
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
    if (f == FX_EMBOSS) {
        static const int k[9] = { -2, -1, 0, -1, 1, 1, 0, 1, 2 };
        return convolve(im, r, k, 1, 0) && apply_fx(im, FX_GRAY, r);
    }
    if (f == FX_EDGES) {                                                /* dark lines where the brightness changes, on white; the alpha stays */
        struct pix src;
        if (!pix_copy(im, &src))
            return false;
        for (int y = r.y0; y < r.y1; y++)
            for (int x = r.x0; x < r.x1; x++) {
                int l[9], k = 0;
                for (int j = -1; j <= 1; j++)
                    for (int i = -1; i <= 1; i++) {
                        uint32_t c = src.p[(size_t)LM_CLAMP(y + j, 0, im->h - 1) * (size_t)im->w + (size_t)LM_CLAMP(x + i, 0, im->w - 1)];
                        l[k++] = (ch_r(c) * 299 + ch_g(c) * 587 + ch_b(c) * 114) / 1000;
                    }
                int gx = (l[2] + 2 * l[5] + l[8]) - (l[0] + 2 * l[3] + l[6]), gy = (l[6] + 2 * l[7] + l[8]) - (l[0] + 2 * l[1] + l[2]);
                int v = 255 - clamp255((int)sqrt((double)(gx * gx + gy * gy)));
                im->p[(size_t)y * (size_t)im->w + (size_t)x] = argb(ch_a(src.p[(size_t)y * (size_t)im->w + (size_t)x]), v, v, v);
            }
        pix_free(&src);
        return true;
    }
    if (f == FX_PIXELATE) {                                             /* blocks of the average colour, a tenth of the short side wide */
        int bs = LM_CLAMP(LM_MIN(r.x1 - r.x0, r.y1 - r.y0) / 10, 2, 64);
        for (int by = r.y0; by < r.y1; by += bs)
            for (int bx = r.x0; bx < r.x1; bx += bs) {
                int x1 = LM_MIN(bx + bs, r.x1), y1 = LM_MIN(by + bs, r.y1), sa = 0, sr = 0, sg = 0, sb = 0, n = 0;
                for (int y = by; y < y1; y++)
                    for (int x = bx; x < x1; x++) {
                        uint32_t c = im->p[(size_t)y * (size_t)im->w + (size_t)x];
                        sa += ch_a(c);
                        sr += ch_r(c) * ch_a(c);
                        sg += ch_g(c) * ch_a(c);
                        sb += ch_b(c) * ch_a(c);
                        n++;
                    }
                uint32_t o = sa ? argb(sa / n, sr / sa, sg / sa, sb / sa) : 0;
                for (int y = by; y < y1; y++)
                    for (int x = bx; x < x1; x++)
                        im->p[(size_t)y * (size_t)im->w + (size_t)x] = o;
            }
        return true;
    }
    uint32_t seed = 2463534242u;
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
            case FX_NOISE: {                                            /* the same noise for the same picture */
                seed ^= seed << 13;
                seed ^= seed >> 17;
                seed ^= seed << 5;
                int d = (int)(seed % 61) - 30;
                R = clamp255(R + d);
                G = clamp255(G + d);
                B = clamp255(B + d);
                break;
            }
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

/* ---- colour: HSV for the picker, brightness / contrast / hue / saturation for the Adjust card ---------------------------------------- */

static void rgb_to_hsv(int r, int g, int b, double *h, double *s, double *v)
{
    double R = r / 255.0, G = g / 255.0, B = b / 255.0, mx = fmax(R, fmax(G, B)), mn = fmin(R, fmin(G, B)), d = mx - mn;
    *v = mx;
    *s = mx > 0 ? d / mx : 0;
    double hh = 0;
    if (d > 0) {
        if (mx == R)
            hh = fmod((G - B) / d, 6.0);
        else if (mx == G)
            hh = (B - R) / d + 2.0;
        else
            hh = (R - G) / d + 4.0;
        hh *= 60.0;
        if (hh < 0)
            hh += 360.0;
    }
    *h = hh;
}

static void hsv_to_rgb(double h, double s, double v, int *r, int *g, int *b)
{
    h = fmod(h, 360.0);
    if (h < 0)
        h += 360.0;
    double c = v * s, x = c * (1 - fabs(fmod(h / 60.0, 2.0) - 1)), m = v - c, R = 0, G = 0, B = 0;
    int sector = (int)(h / 60.0);
    switch (sector) {
    case 0: R = c; G = x; break;
    case 1: R = x; G = c; break;
    case 2: G = c; B = x; break;
    case 3: G = x; B = c; break;
    case 4: R = x; B = c; break;
    default: R = c; B = x; break;
    }
    *r = clamp255((int)((R + m) * 255.0 + 0.5));
    *g = clamp255((int)((G + m) * 255.0 + 0.5));
    *b = clamp255((int)((B + m) * 255.0 + 0.5));
}

struct adjust {
    int brightness, contrast, hue, saturation;      /* -100..100, -100..100, -180..180, -100..100 */
};

static uint32_t adjust_px(uint32_t c, const struct adjust *ad)
{
    int a = ch_a(c), R = ch_r(c), G = ch_g(c), B = ch_b(c);
    if (ad->contrast || ad->brightness) {
        double k = ad->contrast >= 0 ? 1.0 + ad->contrast / 50.0 : 1.0 + ad->contrast / 100.0, add = ad->brightness * 2.55;
        R = clamp255((int)((R - 128) * k + 128 + add + 0.5));
        G = clamp255((int)((G - 128) * k + 128 + add + 0.5));
        B = clamp255((int)((B - 128) * k + 128 + add + 0.5));
    }
    if (ad->hue || ad->saturation) {
        double h, s, v;
        rgb_to_hsv(R, G, B, &h, &s, &v);
        s = LM_CLAMP(s * (1.0 + ad->saturation / 100.0), 0.0, 1.0);
        hsv_to_rgb(h + ad->hue, s, v, &R, &G, &B);
    }
    return argb(a, R, G, B);
}

static void adjust_region(struct pix *im, struct irect r, const struct adjust *ad)
{
    clip_rect(im, &r);
    for (int y = r.y0; y < r.y1; y++)
        for (int x = r.x0; x < r.x1; x++) {
            uint32_t *q = &im->p[(size_t)y * (size_t)im->w + (size_t)x];
            *q = adjust_px(*q, ad);
        }
}

/* Puts the pixels of `r` back from `before` where the selection does not reach, and part of the way where it is soft: what an effect did to a selection. */
static void sel_restore(struct pix *im, const struct pix *before, struct irect r, const struct sel *sel)
{
    if (!sel || !sel->on)
        return;
    clip_rect(im, &r);
    for (int y = r.y0; y < r.y1; y++)
        for (int x = r.x0; x < r.x1; x++) {
            int m = sel_at(sel, x, y);
            if (m >= 255)
                continue;
            size_t i = (size_t)y * (size_t)im->w + (size_t)x;
            uint32_t o = before->p[i], n = im->p[i];
            if (m <= 0)
                im->p[i] = o;
            else
                im->p[i] = argb((ch_a(o) * (255 - m) + ch_a(n) * m) / 255, (ch_r(o) * (255 - m) + ch_r(n) * m) / 255, (ch_g(o) * (255 - m) + ch_g(n) * m) / 255,
                                (ch_b(o) * (255 - m) + ch_b(n) * m) / 255);
        }
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

enum fmt { FMT_ORA, FMT_PNG, FMT_JPEG, FMT_WEBP, FMT_GIF, FMT_BMP, FMT_TGA, FMT_TIFF, FMT_PPM, FMT_COUNT };

/* What can be saved: the chips of the Save as card (label, the extension a chip writes, what the file keeps). */
static const struct { const char *label, *ext, *note; } SAVEFMTS[FMT_COUNT] = {
    { "ORA", ".ora", ".ora keeps every layer, its opacity, blend mode and the paper (GIMP, Krita and MyPaint open it)" },
    { "PNG", ".png", ".png: one flat picture, transparency stays" },
    { "JPG", ".jpg", ".jpg: one flat picture laid on white, no transparency, small files" },
    { "WEBP", ".webp", ".webp: one flat picture, lossless, transparency stays" },
    { "GIF", ".gif", ".gif: one flat picture of at most 256 colours, a see-through pixel stays see-through" },
    { "BMP", ".bmp", ".bmp: a plain Windows bitmap, transparency stays (32 bit)" },
    { "TGA", ".tga", ".tga: a Targa picture, transparency stays" },
    { "TIFF", ".tif", ".tif: a TIFF picture packed with deflate, transparency stays" },
    { "PPM", ".ppm", ".ppm: the simplest picture file, no transparency (laid on white)" },
};

/* The format an extension asks for, -1 when it is none of them (.jpeg / .jpe / .tiff count, too). */
static int fmt_from_ext(const char *dot)
{
    static const struct { const char *ext; int fmt; } more[] = { { ".jpeg", FMT_JPEG }, { ".jpe", FMT_JPEG }, { ".tiff", FMT_TIFF } };
    if (!dot)
        return -1;
    for (int f = 0; f < FMT_COUNT; f++)
        if (!strcasecmp(dot, SAVEFMTS[f].ext))
            return f;
    for (size_t k = 0; k < sizeof(more) / sizeof(more[0]); k++)
        if (!strcasecmp(dot, more[k].ext))
            return more[k].fmt;
    return -1;
}

static int fmt_of(const char *path)
{
    const char *slash = strrchr(path, '/'), *dot = strrchr(path, '.');
    return dot && (!slash || dot > slash) ? fmt_from_ext(dot) : -1;
}

static bool has_known_ext(const char *path)
{
    return fmt_of(path) >= 0;
}

/* The name with its extension (if it has one of ours) replaced by `ext`; with none the extension is added. */
static void set_ext(char *buf, size_t n, const char *ext)
{
    const char *slash = strrchr(buf, '/'), *dot = strrchr(buf, '.');
    if (dot && (!slash || dot > slash) && fmt_from_ext(dot) >= 0)
        buf[dot - buf] = 0;
    if (strlen(buf) + strlen(ext) < n)
        strcat(buf, ext);
}

/* Writes the picture: .png keeps transparency, .jpg is laid over white. The file is made whole under a temporary name and renamed over the old one. */
static bool save_file(const struct pix *im, const char *path, char *err, size_t errsz)
{
    size_t n = 0;
    uint8_t *data = NULL;
    int fmt = fmt_of(path);
    if (fmt == FMT_JPEG) {
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
    } else if (fmt == FMT_WEBP) {
        data = lm_enc_webp(im->p, im->w, im->h, &n);
    } else if (fmt == FMT_GIF) {
        data = lm_enc_gif(im->p, im->w, im->h, &n);
    } else if (fmt == FMT_BMP) {
        data = lm_enc_bmp(im->p, im->w, im->h, &n);
    } else if (fmt == FMT_TGA) {
        data = lm_enc_tga(im->p, im->w, im->h, &n);
    } else if (fmt == FMT_TIFF) {
        data = lm_enc_tiff(im->p, im->w, im->h, &n);
    } else if (fmt == FMT_PPM) {
        data = lm_enc_ppm(im->p, im->w, im->h, &n);
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

/* ---- the document: layers of one size, composited bottom to top ------------------------------------------------------------------- */

enum blendm { BM_NORMAL, BM_MULTIPLY, BM_SCREEN, BM_OVERLAY, BM_DARKEN, BM_LIGHTEN, BM_DIFFERENCE, BM_COUNT };
static const char *const BM_NAMES[BM_COUNT] = { "Normal", "Multiply", "Screen", "Overlay", "Darken", "Lighten", "Difference" };
static const char *const BM_ORA[BM_COUNT] = { "svg:src-over", "svg:multiply", "svg:screen", "svg:overlay", "svg:darken", "svg:lighten", "svg:difference" };

#define MAX_LAYERS 32

struct layer {
    struct pix im;
    char name[40];
    int opacity;                        /* 0..100 */
    bool visible;
    int blend;                          /* enum blendm */
};

struct doc {
    struct layer L[MAX_LAYERS];
    int n, cur;
    uint32_t paper;                     /* the colour of the sheet itself, behind every layer (0 = none: the checkerboard shows); it is not a layer, so Move leaves it where it is */
};

static int doc_w(const struct doc *d) { return d->n ? d->L[0].im.w : 0; }
static int doc_h(const struct doc *d) { return d->n ? d->L[0].im.h : 0; }

static void doc_free(struct doc *d)
{
    for (int i = 0; i < d->n; i++)
        pix_free(&d->L[i].im);
    memset(d, 0, sizeof(*d));
}

static bool doc_copy(const struct doc *s, struct doc *d)
{
    memset(d, 0, sizeof(*d));
    for (int i = 0; i < s->n; i++) {
        d->L[i] = s->L[i];
        d->L[i].im = (struct pix){ 0 };
        if (!pix_copy(&s->L[i].im, &d->L[i].im)) {
            d->n = i;
            doc_free(d);
            return false;
        }
    }
    d->n = s->n;
    d->cur = s->cur;
    d->paper = s->paper;
    return true;
}

static void layer_init(struct layer *l, const char *name)
{
    snprintf(l->name, sizeof(l->name), "%s", name);
    l->opacity = 100;
    l->visible = true;
    l->blend = BM_NORMAL;
}

/* A document of one layer that takes over `im`. */
static void doc_from_pix(struct doc *d, struct pix *im)
{
    memset(d, 0, sizeof(*d));
    d->n = 1;
    layer_init(&d->L[0], "Background");
    d->L[0].im = *im;
    *im = (struct pix){ 0 };
}

/* A new empty layer above the chosen one (which it becomes). */
static bool doc_add(struct doc *d, const char *name, uint32_t fill)
{
    if (d->n >= MAX_LAYERS || !d->n)
        return false;
    struct pix im;
    if (!pix_alloc(&im, doc_w(d), doc_h(d), fill))
        return false;
    int at = d->cur + 1;
    memmove(&d->L[at + 1], &d->L[at], sizeof(d->L[0]) * (size_t)(d->n - at));
    memset(&d->L[at], 0, sizeof(d->L[0]));
    layer_init(&d->L[at], name);
    d->L[at].im = im;
    d->n++;
    d->cur = at;
    return true;
}

static bool doc_dup(struct doc *d)
{
    if (d->n >= MAX_LAYERS)
        return false;
    struct pix im;
    if (!pix_copy(&d->L[d->cur].im, &im))
        return false;
    int at = d->cur + 1;
    memmove(&d->L[at + 1], &d->L[at], sizeof(d->L[0]) * (size_t)(d->n - at));
    char nm[40];
    snprintf(nm, sizeof(nm), "%.30s copy", d->L[d->cur].name);
    d->L[at] = d->L[d->cur];
    d->L[at].im = im;
    memcpy(d->L[at].name, nm, sizeof(nm));
    d->n++;
    d->cur = at;
    return true;
}

static bool doc_del(struct doc *d)
{
    if (d->n <= 1)
        return false;
    pix_free(&d->L[d->cur].im);
    memmove(&d->L[d->cur], &d->L[d->cur + 1], sizeof(d->L[0]) * (size_t)(d->n - d->cur - 1));
    d->n--;
    memset(&d->L[d->n], 0, sizeof(d->L[0]));
    if (d->cur >= d->n)
        d->cur = d->n - 1;
    return true;
}

static bool doc_move(struct doc *d, int dir)                           /* +1 = up the stack */
{
    int to = d->cur + dir;
    if (to < 0 || to >= d->n)
        return false;
    struct layer t = d->L[to];
    d->L[to] = d->L[d->cur];
    d->L[d->cur] = t;
    d->cur = to;
    return true;
}

static int bm_chan(int mode, int cb, int cs)
{
    switch (mode) {
    case BM_MULTIPLY: return cb * cs / 255;
    case BM_SCREEN: return cb + cs - cb * cs / 255;
    case BM_OVERLAY: return cb < 128 ? 2 * cb * cs / 255 : 255 - 2 * (255 - cb) * (255 - cs) / 255;
    case BM_DARKEN: return LM_MIN(cb, cs);
    case BM_LIGHTEN: return LM_MAX(cb, cs);
    case BM_DIFFERENCE: return abs(cb - cs);
    default: return cs;
    }
}

/* One layer's pixel laid over what is below it (`acc`), with the layer's opacity (0..100) and blend mode. */
static uint32_t layer_over(uint32_t acc, uint32_t src, int opacity, int mode)
{
    if (ch_a(src) * opacity / 100 <= 0)
        return acc;
    int ba = ch_a(acc);
    if (mode != BM_NORMAL && ba > 0) {
        int r = bm_chan(mode, ch_r(acc), ch_r(src)), g = bm_chan(mode, ch_g(acc), ch_g(src)), b = bm_chan(mode, ch_b(acc), ch_b(src));
        src = argb(ch_a(src), (ch_r(src) * (255 - ba) + r * ba) / 255, (ch_g(src) * (255 - ba) + g * ba) / 255, (ch_b(src) * (255 - ba) + b * ba) / 255);
    }
    return blend(acc, src, opacity * 255 / 100);
}

/* The visible layers of the box `r` composited into `out` (the same size as the layers). */
static void doc_composite(const struct doc *d, struct pix *out, struct irect r)
{
    irect_clip(&r, out->w, out->h);
    for (int y = r.y0; y < r.y1; y++)
        for (int x = r.x0; x < r.x1; x++) {
            size_t i = (size_t)y * (size_t)out->w + (size_t)x;
            uint32_t acc = d->paper;
            for (int l = 0; l < d->n; l++)
                if (d->L[l].visible)
                    acc = layer_over(acc, d->L[l].im.p[i], d->L[l].opacity, d->L[l].blend);
            out->p[i] = acc;
        }
}

static bool doc_flatten(const struct doc *d, struct pix *out)
{
    if (!pix_alloc(out, doc_w(d), doc_h(d), 0))
        return false;
    doc_composite(d, out, (struct irect){ 0, 0, out->w, out->h });
    return true;
}

/* The chosen layer laid on the one below it (opacity and blend mode counted); the two become one. */
static bool doc_merge_down(struct doc *d)
{
    if (d->cur < 1)
        return false;
    struct layer *up = &d->L[d->cur], *lo = &d->L[d->cur - 1];
    size_t n = (size_t)lo->im.w * (size_t)lo->im.h;
    for (size_t i = 0; i < n; i++)
        lo->im.p[i] = up->visible ? layer_over(lo->im.p[i], up->im.p[i], up->opacity, up->blend) : lo->im.p[i];
    int keep = d->cur - 1;
    doc_del(d);
    d->cur = keep;
    return true;
}

/* A bigger or smaller sheet: the layers keep their pixels, `ax` / `ay` (0 left / top, 1 middle, 2 right / bottom) say where they sit on it;
 * the new room is transparent, on the lowest layer `fill`. */
static bool doc_canvas(struct doc *d, int nw, int nh, int ax, int ay, uint32_t fill)
{
    int ow = doc_w(d), oh = doc_h(d), ox = (nw - ow) * ax / 2, oy = (nh - oh) * ay / 2;
    struct pix np[MAX_LAYERS];
    for (int l = 0; l < d->n; l++) {
        if (!pix_alloc(&np[l], nw, nh, l == 0 ? fill : 0)) {
            for (int k = 0; k < l; k++)
                pix_free(&np[k]);
            return false;
        }
    }
    for (int l = 0; l < d->n; l++) {
        for (int y = 0; y < nh; y++) {
            int sy = y - oy;
            if (sy < 0 || sy >= oh)
                continue;
            int x0 = LM_MAX(ox, 0), x1 = LM_MIN(ox + ow, nw);
            if (x1 > x0)
                memcpy(&np[l].p[(size_t)y * (size_t)nw + (size_t)x0], &d->L[l].im.p[(size_t)sy * (size_t)ow + (size_t)(x0 - ox)], (size_t)(x1 - x0) * 4);
        }
        pix_free(&d->L[l].im);
        d->L[l].im = np[l];
    }
    return true;
}

/* ---- OpenRaster (.ora): a ZIP of PNG layers and a stack.xml, the layered format GIMP, Krita and MyPaint share ---------------------------- */

struct zent {
    char name[96];
    uint32_t crc, size, off;
};

static void zw16(FILE *f, unsigned v)
{
    fputc((int)(v & 255), f);
    fputc((int)((v >> 8) & 255), f);
}

static void zw32(FILE *f, uint32_t v)
{
    zw16(f, v & 0xffff);
    zw16(f, v >> 16);
}

static bool zip_add(FILE *f, struct zent *z, int *nz, const char *name, const uint8_t *data, size_t n)
{
    if (*nz >= 80 || n > 0xfffffff0u)
        return false;
    struct zent *e = &z[(*nz)++];
    snprintf(e->name, sizeof(e->name), "%s", name);
    e->crc = lm_crc32(0, data, n);
    e->size = (uint32_t)n;
    e->off = (uint32_t)ftell(f);
    zw32(f, 0x04034b50);
    zw16(f, 20);
    zw16(f, 0);
    zw16(f, 0);                                                         /* stored */
    zw16(f, 0);
    zw16(f, 0x21);                                                      /* 1980-01-01 */
    zw32(f, e->crc);
    zw32(f, e->size);
    zw32(f, e->size);
    zw16(f, (unsigned)strlen(name));
    zw16(f, 0);
    fwrite(name, 1, strlen(name), f);
    return n == 0 || fwrite(data, 1, n, f) == n;
}

static void xml_escape(const char *s, char *out, size_t n)
{
    size_t o = 0;
    for (; *s && o + 7 < n; s++) {
        const char *r = *s == '&' ? "&amp;" : *s == '<' ? "&lt;" : *s == '>' ? "&gt;" : *s == '"' ? "&quot;" : NULL;
        if (r) {
            size_t l = strlen(r);
            memcpy(out + o, r, l);
            o += l;
        } else
            out[o++] = *s;
    }
    out[o] = 0;
}

/* Writes the layers (the top one first in stack.xml, as the format wants) and the flattened picture. */
static bool write_ora(const struct doc *d, const char *path, char *err, size_t errsz)
{
    char tmp[PATH_MAX + 32];
    snprintf(tmp, sizeof(tmp), "%s.paint-%ld.tmp", path, (long)getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
    FILE *f = fd >= 0 ? fdopen(fd, "wb") : NULL;
    if (!f) {
        if (fd >= 0)
            close(fd);
        snprintf(err, errsz, "%s", strerror(errno));
        return false;
    }
    struct zent z[80];
    int nz = 0;
    bool ok = zip_add(f, z, &nz, "mimetype", (const uint8_t *)"image/openraster", 16);
    size_t xcap = 512 + (size_t)d->n * 400, xn = 0;
    char *xml = malloc(xcap);
    ok = ok && xml;
    if (ok) {
        xn += (size_t)snprintf(xml + xn, xcap - xn, "<?xml version='1.0' encoding='UTF-8'?>\n<image version=\"0.0.3\" w=\"%d\" h=\"%d\" mellagui-paper=\"%08x\"><stack>\n", doc_w(d), doc_h(d), d->paper);
        for (int l = d->n - 1; l >= 0 && ok; l--) {
            char nm[200], src[48];
            xml_escape(d->L[l].name, nm, sizeof(nm));
            snprintf(src, sizeof(src), "data/layer%d.png", l);
            xn += (size_t)snprintf(xml + xn, xcap - xn, "<layer name=\"%s\" src=\"%s\" x=\"0\" y=\"0\" opacity=\"%.2f\" visibility=\"%s\" composite-op=\"%s\"/>\n", nm, src,
                                   d->L[l].opacity / 100.0, d->L[l].visible ? "visible" : "hidden", BM_ORA[d->L[l].blend]);
        }
        xn += (size_t)snprintf(xml + xn, xcap - xn, "</stack></image>\n");
        ok = zip_add(f, z, &nz, "stack.xml", (const uint8_t *)xml, xn);
    }
    for (int l = 0; l < d->n && ok; l++) {
        size_t n = 0;
        uint8_t *png = png_encode(&d->L[l].im, &n, NULL);
        char nm[48];
        snprintf(nm, sizeof(nm), "data/layer%d.png", l);
        ok = png && zip_add(f, z, &nz, nm, png, n);
        free(png);
    }
    if (ok) {
        struct pix flat;
        if (doc_flatten(d, &flat)) {
            size_t n = 0;
            uint8_t *png = png_encode(&flat, &n, NULL);
            ok = png && zip_add(f, z, &nz, "mergedimage.png", png, n);
            free(png);
            pix_free(&flat);
        } else
            ok = false;
    }
    free(xml);
    if (ok) {
        uint32_t cd = (uint32_t)ftell(f);
        for (int i = 0; i < nz; i++) {
            zw32(f, 0x02014b50);
            zw16(f, 20);
            zw16(f, 20);
            zw16(f, 0);
            zw16(f, 0);
            zw16(f, 0);
            zw16(f, 0x21);
            zw32(f, z[i].crc);
            zw32(f, z[i].size);
            zw32(f, z[i].size);
            zw16(f, (unsigned)strlen(z[i].name));
            zw16(f, 0);
            zw16(f, 0);
            zw16(f, 0);
            zw16(f, 0);
            zw32(f, 0);
            zw32(f, z[i].off);
            fwrite(z[i].name, 1, strlen(z[i].name), f);
        }
        uint32_t cdn = (uint32_t)ftell(f) - cd;
        zw32(f, 0x06054b50);
        zw16(f, 0);
        zw16(f, 0);
        zw16(f, (unsigned)nz);
        zw16(f, (unsigned)nz);
        zw32(f, cdn);
        zw32(f, cd);
        zw16(f, 0);
    }
    if (fclose(f) != 0)
        ok = false;
    if (ok && rename(tmp, path) != 0)
        ok = false;
    if (!ok) {
        snprintf(err, errsz, "could not write the file");
        unlink(tmp);
    }
    return ok;
}

static bool xml_attr(const char *tag, const char *end, const char *name, char *out, size_t n)
{
    char key[40];
    snprintf(key, sizeof(key), " %s=", name);
    for (const char *p = tag; p && p < end; p++) {
        p = strstr(p, key);
        if (!p || p >= end)
            return false;
        char q = p[strlen(key)];
        if (q != '"' && q != '\'')
            continue;
        const char *s = p + strlen(key) + 1, *e = memchr(s, q, (size_t)(end - s));
        if (!e)
            return false;
        size_t o = 0;
        for (; s < e && o + 1 < n; s++) {
            if (*s == '&') {
                static const struct { const char *ent, *ch; } map[] = { { "&amp;", "&" }, { "&lt;", "<" }, { "&gt;", ">" }, { "&quot;", "\"" }, { "&apos;", "'" } };
                bool hit = false;
                for (size_t k = 0; k < sizeof(map) / sizeof(map[0]) && !hit; k++)
                    if (!strncmp(s, map[k].ent, strlen(map[k].ent))) {
                        out[o++] = map[k].ch[0];
                        s += strlen(map[k].ent) - 1;
                        hit = true;
                    }
                if (hit)
                    continue;
            }
            out[o++] = *s;
        }
        out[o] = 0;
        return true;
    }
    return false;
}

static int rm_entry(const char *path, const struct stat *sb, int type, struct FTW *ftw)
{
    (void)sb;
    (void)type;
    (void)ftw;
    return remove(path);
}

static bool ora_name_ok(const char *s)
{
    return *s && *s != '/' && !strstr(s, "..") && strlen(s) < 200;
}

/* Reads an .ora: the ZIP is unpacked into a private temporary folder (the archiver's safe extraction), stack.xml says which PNG is which layer. */
static bool load_ora(const char *path, struct doc *d, char *err, size_t errsz)
{
    struct aio *ar = NULL;
    if (aio_open(path, &ar, err, errsz) != 0) {
        if (!*err)
            snprintf(err, errsz, "not an OpenRaster file");
        return false;
    }
    const char *base = getenv("XDG_RUNTIME_DIR");
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/mellagui-ora-XXXXXX", base && *base ? base : "/tmp");
    if (!mkdtemp(dir)) {
        aio_close(ar);
        snprintf(err, errsz, "no room for a temporary folder");
        return false;
    }
    struct aio_progress pr;
    struct aio_report rep;
    struct aio_opts o = { .overwrite = false };
    aio_progress_init(&pr);
    memset(&rep, 0, sizeof(rep));
    int rc = aio_extract(ar, NULL, 0, dir, &o, &pr, &rep, err, errsz);
    aio_progress_free(&pr);
    aio_close(ar);
    bool ok = false;
    char sp[PATH_MAX + 16];
    snprintf(sp, sizeof(sp), "%s/stack.xml", dir);
    FILE *f = rc == 0 ? fopen(sp, "rb") : NULL;
    char *xml = NULL;
    if (f) {
        xml = calloc(1, 200001);
        size_t n = xml ? fread(xml, 1, 200000, f) : 0;
        if (xml)
            xml[n] = 0;
        fclose(f);
    }
    if (!xml) {
        snprintf(err, errsz, "no stack.xml in the file");
    } else {
        memset(d, 0, sizeof(*d));
        int w = 0, h = 0;
        uint32_t paper = 0;
        char val[200];
        const char *img = strstr(xml, "<image");
        if (img) {
            const char *e = strchr(img, '>');
            if (e && xml_attr(img, e, "w", val, sizeof(val)))
                w = atoi(val);
            if (e && xml_attr(img, e, "h", val, sizeof(val)))
                h = atoi(val);
            if (e && xml_attr(img, e, "mellagui-paper", val, sizeof(val)))
                paper = (uint32_t)strtoul(val, NULL, 16);
        }
        struct { char src[200], name[40]; int x, y, opacity, blend; bool visible; } ent[MAX_LAYERS];
        int ne = 0;
        for (const char *p = xml; (p = strstr(p, "<layer")) && ne < MAX_LAYERS; p++) {
            const char *e = strchr(p, '>');
            if (!e)
                break;
            if (!xml_attr(p, e, "src", ent[ne].src, sizeof(ent[ne].src)) || !ora_name_ok(ent[ne].src))
                continue;
            if (!xml_attr(p, e, "name", ent[ne].name, sizeof(ent[ne].name)))
                snprintf(ent[ne].name, sizeof(ent[ne].name), "Layer %d", ne + 1);
            ent[ne].x = xml_attr(p, e, "x", val, sizeof(val)) ? atoi(val) : 0;
            ent[ne].y = xml_attr(p, e, "y", val, sizeof(val)) ? atoi(val) : 0;
            ent[ne].opacity = xml_attr(p, e, "opacity", val, sizeof(val)) ? LM_CLAMP((int)(atof(val) * 100.0 + 0.5), 0, 100) : 100;
            ent[ne].visible = !(xml_attr(p, e, "visibility", val, sizeof(val)) && !strcmp(val, "hidden"));
            ent[ne].blend = BM_NORMAL;
            if (xml_attr(p, e, "composite-op", val, sizeof(val)))
                for (int b = 0; b < BM_COUNT; b++)
                    if (!strcmp(val, BM_ORA[b]))
                        ent[ne].blend = b;
            ne++;
        }
        if (w < 1 || h < 1 || ne == 0)
            snprintf(err, errsz, "the file has no layers");
        else {
            ok = true;
            for (int i = ne - 1; i >= 0 && ok; i--) {                  /* stack.xml lists the top layer first */
                char lp[PATH_MAX + 220];
                snprintf(lp, sizeof(lp), "%s/%s", dir, ent[i].src);
                struct pix src = { 0 };
                struct layer *l = &d->L[d->n];
                if (!pix_alloc(&l->im, w, h, 0)) {
                    snprintf(err, errsz, "that size is not possible");
                    ok = false;
                    break;
                }
                layer_init(l, ent[i].name);
                l->opacity = ent[i].opacity;
                l->visible = ent[i].visible;
                l->blend = ent[i].blend;
                d->n++;
                if (load_file(lp, &src, err, errsz)) {
                    for (int y = 0; y < src.h; y++) {
                        int dy = y + ent[i].y;
                        if (dy < 0 || dy >= h)
                            continue;
                        for (int x = 0; x < src.w; x++) {
                            int dx = x + ent[i].x;
                            if (dx >= 0 && dx < w)
                                l->im.p[(size_t)dy * (size_t)w + (size_t)dx] = src.p[(size_t)y * (size_t)src.w + (size_t)x];
                        }
                    }
                    pix_free(&src);
                } else
                    ok = false;
            }
            d->cur = d->n - 1;
            d->paper = paper;
            if (!ok)
                doc_free(d);
        }
    }
    free(xml);
    nftw(dir, rm_entry, 8, FTW_DEPTH | FTW_PHYS);
    return ok;
}

static bool is_ora(const char *path)
{
    const char *dot = strrchr(path, '.');
    return dot && !strcasecmp(dot, ".ora");
}

/* Any file as a document: .ora with its layers, everything else as one layer. */
static bool load_doc(const char *path, struct doc *d, char *err, size_t errsz)
{
    err[0] = 0;
    if (is_ora(path))
        return load_ora(path, d, err, errsz);
    struct pix im = { 0 };
    if (!load_file(path, &im, err, errsz))
        return false;
    doc_from_pix(d, &im);
    return true;
}
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
    if (argc < 2) {
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

/* ---- the window ------------------------------------------------------------------------------------------------------------------ */

enum tool { T_SELECT, T_ELLIPSE_SEL, T_LASSO, T_WAND, T_MOVE, T_PEN, T_SPRAY, T_ERASER, T_LINE, T_BOX, T_CIRCLE, T_POLY, T_FILL, T_GRADIENT, T_TEXT, T_PICK, T_COUNT };
static const char *const TOOL_NAMES[T_COUNT] = { "Select", "EllipseSelect", "Lasso", "Wand", "Move", "Pen", "Airbrush", "Eraser", "Line", "Box", "Circle", "Shape", "Fill",
                                                 "Gradient", "Text", "Picker" };
static const char *const TOOL_TITLES[T_COUNT] = { "Select a rectangle", "Select an ellipse", "Lasso", "Magic wand", "Move", "Pen", "Airbrush", "Eraser", "Line", "Box", "Circle",
                                                  "Polygon / star", "Fill", "Gradient", "Text", "Picker" };
static const char TOOL_KEYS[T_COUNT] = { 's', 'o', 'q', 'w', 'm', 'p', 'a', 'e', 'l', 'b', 'c', 'n', 'f', 'g', 't', 'i' };
static const char *const TOOL_HINTS[T_COUNT] = {
    "Drag a rectangle: Enter crops to it, Delete clears it, Backspace fills it, effects work on it",
    "Drag an ellipse (Shift: a circle) to select it",
    "Drag round what you want: it is selected when you let go",
    "Click to select the area of a similar colour (Tolerance in the strip; Shift: all of them)",
    "Drag to move the selection, or the whole layer when nothing is selected",
    "Drag to draw (the left button: pen colour, the right one: the second colour); Size, Opacity and Hardness are in the strip",
    "Hold the button to spray; Density in the strip",
    "Drag to rub out: it takes the layer's colour away (Opacity and Hardness in the strip)",
    "Drag a line (Shift: 45 degrees); Arrow gives it a head",
    "Drag a box (Shift: a square); Corner rounds it, Filled makes it solid",
    "Drag a circle (Shift: round); Filled makes it solid",
    "Drag a polygon or a star; Sides and Star are in the strip",
    "Click to fill an area with the pen colour (right click: the second colour); Tolerance in the strip",
    "Drag from where the colour starts to where it ends (Radial: round about the start)",
    "Click where the text begins and type; Enter a new line, Esc cancels, Ctrl+Enter or a click elsewhere puts it down",
    "Click a pixel to take its colour (right click: as the second colour)",
};

enum slider { S_SIZE, S_OPACITY, S_HARD, S_TOL, S_CORNER, S_SIDES, S_DENSITY, S_COUNT };
static const struct { const char *name; int mn, mx, def; const char *unit; } SLIDERS[S_COUNT] = {
    { "Size", 1, 200, 4, " px" }, { "Opacity", 1, 100, 100, "%" }, { "Hardness", 0, 100, 100, "%" }, { "Tolerance", 0, 100, 10, "%" },
    { "Corner", 0, 200, 0, " px" }, { "Sides", 3, 12, 5, "" }, { "Density", 5, 100, 40, "%" },
};

enum toggle { TG_FILLED, TG_STAR, TG_ARROW, TG_RADIAL, TG_FADE, TG_BOLD, TG_GLOBAL, TG_IMAGE, TG_COUNT };
static const char *const TG_NAMES[TG_COUNT] = { "Filled", "Star", "Arrow", "Radial", "To clear", "Bold", "All of it", "Image" };

/* What each tool shows in the strip: up to three sliders and two switches (-1 = none). */
static const struct { int sl[3], tg[2]; } TOPT[T_COUNT] = {
    [T_SELECT] = { { -1, -1, -1 }, { -1, -1 } },
    [T_ELLIPSE_SEL] = { { -1, -1, -1 }, { -1, -1 } },
    [T_LASSO] = { { -1, -1, -1 }, { -1, -1 } },
    [T_WAND] = { { S_TOL, -1, -1 }, { TG_GLOBAL, TG_IMAGE } },
    [T_MOVE] = { { -1, -1, -1 }, { -1, -1 } },
    [T_PEN] = { { S_SIZE, S_OPACITY, S_HARD }, { -1, -1 } },
    [T_SPRAY] = { { S_SIZE, S_OPACITY, S_DENSITY }, { -1, -1 } },
    [T_ERASER] = { { S_SIZE, S_OPACITY, S_HARD }, { -1, -1 } },
    [T_LINE] = { { S_SIZE, S_OPACITY, -1 }, { TG_ARROW, -1 } },
    [T_BOX] = { { S_SIZE, S_OPACITY, S_CORNER }, { TG_FILLED, -1 } },
    [T_CIRCLE] = { { S_SIZE, S_OPACITY, -1 }, { TG_FILLED, -1 } },
    [T_POLY] = { { S_SIZE, S_OPACITY, S_SIDES }, { TG_STAR, TG_FILLED } },
    [T_FILL] = { { S_OPACITY, S_TOL, -1 }, { TG_GLOBAL, TG_IMAGE } },
    [T_GRADIENT] = { { S_OPACITY, -1, -1 }, { TG_RADIAL, TG_FADE } },
    [T_TEXT] = { { S_SIZE, S_OPACITY, -1 }, { TG_BOLD, -1 } },
    [T_PICK] = { { -1, -1, -1 }, { TG_IMAGE, -1 } },
};

#define N_RECENT 16
#define N_QUICK 24
static const uint32_t QUICK[N_QUICK] = {
    0x00000000, 0xff000000, 0xff404040, 0xff808080, 0xffc0c0c0, 0xffffffff, 0xff7f1d1d, 0xff795548,
    0xffe53935, 0xfffb8c00, 0xfffdd835, 0xffa4c639, 0xff43a047, 0xff00897b, 0xff29b6f6, 0xff1e88e5,
    0xff7e57c2, 0xffec407a, 0xffffcdd2, 0xffffe0b2, 0xfffff9c4, 0xffc8e6c9, 0xffb3e5fc, 0xffd1c4e9,
};

enum prompt { P_NONE, P_OPEN, P_SAVEAS, P_NEW, P_RESIZE, P_DISCARD, P_REPLACE, P_HELP, P_COLOUR, P_CANVAS, P_ADJUST, P_LAYERNAME, P_PLACE };
enum after { AF_NONE, AF_OPEN, AF_NEW, AF_QUIT };

/* The two rails' buttons. */
enum btn { B_SELECT, B_MOVE, B_PEN, B_SPRAY, B_ERASER, B_SHAPES, B_FILL, B_GRADIENT, B_TEXT, B_PICK,        /* the left rail: the tools */
           B_NEW, B_OPEN, B_SAVE, B_UNDO, B_REDO, B_TRANSFORM, B_FX, B_CROP, B_FIT, B_HELP, B_COUNT };
#define N_LEFT 10
#define N_RIGHT 10

static bool tool_in_select(enum tool t) { return t == T_SELECT || t == T_ELLIPSE_SEL || t == T_LASSO || t == T_WAND; }
static bool tool_in_shapes(enum tool t) { return t == T_LINE || t == T_BOX || t == T_CIRCLE || t == T_POLY; }

/* The rail button that stands for a tool. */
static int tool_button(enum tool t)
{
    if (tool_in_select(t))
        return B_SELECT;
    if (tool_in_shapes(t))
        return B_SHAPES;
    switch (t) {
    case T_MOVE: return B_MOVE;
    case T_PEN: return B_PEN;
    case T_SPRAY: return B_SPRAY;
    case T_ERASER: return B_ERASER;
    case T_FILL: return B_FILL;
    case T_GRADIENT: return B_GRADIENT;
    case T_TEXT: return B_TEXT;
    default: return B_PICK;
    }
}

/* ---- undo: a layer's pixels, the whole document, or only the layers' settings ----------------------------------------------------- */

enum snapk { SK_PIX, SK_DOC, SK_PROPS };

struct snap {
    int kind, layer;
    struct pix im;
    struct doc *doc;
};

struct cpick {
    double h, s, v;
    int alpha;
    bool bg;                                    /* the second colour is being chosen */
    uint32_t orig;
    int drag;                                   /* 1 the square, 2 the hue bar, 3 the alpha bar */
};

struct app {
    struct wlwin win;
    struct wlclip clip;
    struct wlrail_anim ran[2];
    int hover;                                  /* the rail button under the pointer: B_* of the left or the right rail, -1 none */
    double px, py;                              /* the pointer in drawing coordinates */
    bool has_pointer;

    struct doc doc;
    struct pix flat;                            /* the layers composited: what the view shows, what a save writes */
    struct irect fdirty;                        /* the part of `flat` that has to be made again */
    char path[PATH_MAX];
    bool modified;

    struct snap undo[MAX_UNDO], redo[MAX_UNDO];
    int nundo, nredo;

    enum tool tool;
    enum tool grp_sel, grp_shape;               /* the variants the grouped rail buttons stand for */
    uint32_t fg, bg;
    uint32_t recent[N_RECENT];
    int nrecent;
    int val[S_COUNT];
    int tsize;                                  /* the size of text */
    bool tg[TG_COUNT];

    double zoom, vx, vy;                        /* logical screen pixels per picture pixel; where the picture's top left lies in the canvas */
    bool fitted;                                /* the zoom is "fit the window": it follows the window's size */
    bool grid;
    uint32_t *vbuf;
    size_t vbuf_cap;
    int vbw, vbh;                               /* the size of what vbuf holds, in physical pixels */
    bool vdirty;

    bool drag, pan;                             /* a tool is being dragged / the picture is being panned with the middle button */
    int drag_button;                            /* 272 or 273 */
    double sx, sy, lx, ly;                      /* where the drag began, and the last point (picture coordinates, may be fractional) */
    double pan_x, pan_y, pan_vx, pan_vy;
    struct pix before;                          /* the layer before the stroke / shape that is being dragged */
    bool have_before;
    struct cov cov;                             /* what the stroke covers so far */
    struct irect preview;                       /* what a shape preview covers */
    int sl_drag;                                /* the slider being dragged: S_*, 100 + the layer's opacity, -1 none */
    double *lx_pts, *ly_pts;                    /* the lasso's points */
    int nlasso, caplasso;
    struct pix mv_base, mv_lift;                /* the move tool: the layer without what is lifted, and what is lifted */
    struct irect mv_sel0;
    bool mv_lifted;
    int64_t spray_ms;
    uint32_t rnd;

    struct sel sel;

    struct {
        bool on;
        int x, y;                               /* where the text starts (picture pixels) */
        char buf[2048];
        int len, caret;                         /* bytes */
    } txt;
    struct pix tbefore;                         /* the layer as it was when the text began */

    struct pix clipimg;                         /* Copy / Cut keep their picture here */

    enum prompt prompt;
    enum after after;                           /* what to do once a discard question was answered with "Discard" */
    char pbuf[PATH_MAX];                        /* the text field of a prompt */
    char perr[160];                             /* what was wrong with the last answer */
    int pcard_hover, pcard_down;
    struct wlpicker pk;                         /* the folder list of the Open / Save as card (wlpicker.h, the same in every program) */
    int menu_kind;                              /* the menu that is open: 0 none, 1 effects, 2 transform, 3 selects, 4 shapes, 5 blend, 6 layer */
    struct cpick cp;
    int anchor_x, anchor_y;                     /* the canvas card: where the picture sits (0..2) */
    bool canvas_fill_bg;                        /* ... and what the new room is */
    bool new_transparent;                       /* the New card: a transparent first layer */
    struct adjust adj;
    struct pix adj_before;
    int adj_drag;

    bool show_layers;
    int lay_seen, lay_n;                        /* the chosen layer and the count the list was last scrolled for */
    int64_t lay_click_ms;
    int lay_click_idx;
    int cur_kind;                               /* the pointer's shape now (CUR_*), -1 = none sent yet */
    int lay_top;                                /* the first row of the layers' list that shows (from the top of the stack) */

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

#define IM(a) ((a)->doc.L[(a)->doc.cur].im)
#define CW(a) doc_w(&(a)->doc)
#define CH(a) doc_h(&(a)->doc)

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

#define LP_W 184
#define LP_ROW_H 40

static bool layers_on(const struct app *a) { return a->show_layers && a->win.bw >= 720; }
static int panel_w(const struct app *a) { return layers_on(a) ? LP_W : 0; }
static int cv_x(const struct app *a) { (void)a; return WLRAIL_W; }
static int cv_w(const struct app *a) { return LM_MAX(a->win.bw - 2 * WLRAIL_W - panel_w(a), 1); }
static int cv_h(const struct app *a) { return LM_MAX(a->win.bh - PAL_H - STAT_H, 1); }
static int pal_y(const struct app *a) { return a->win.bh - PAL_H - STAT_H; }
static int stat_y(const struct app *a) { return a->win.bh - STAT_H; }
static int panel_x(const struct app *a) { return a->win.bw - WLRAIL_W - LP_W; }

static bool in_canvas(const struct app *a, double x, double y)
{
    return x >= cv_x(a) && x < cv_x(a) + cv_w(a) && y >= 0 && y < cv_h(a);
}

static double fit_zoom(const struct app *a)
{
    if (CW(a) < 1)
        return 1.0;
    double z = LM_MIN((cv_w(a) - 24.0) / CW(a), (cv_h(a) - 24.0) / CH(a));
    return z > 1.0 ? 1.0 : z < 0.02 ? 0.02 : z;
}

static void view_center(struct app *a)
{
    a->vx = (cv_w(a) - CW(a) * a->zoom) / 2.0;
    a->vy = (cv_h(a) - CH(a) * a->zoom) / 2.0;
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
    double iw = CW(a) * a->zoom, ih = CH(a) * a->zoom;
    if (iw <= cv_w(a))
        a->vx = LM_CLAMP(a->vx, 0.0, cv_w(a) - iw);
    else
        a->vx = LM_CLAMP(a->vx, cv_w(a) - iw, 0.0);
    if (ih <= cv_h(a))
        a->vy = LM_CLAMP(a->vy, 0.0, cv_h(a) - ih);
    else
        a->vy = LM_CLAMP(a->vy, cv_h(a) - ih, 0.0);
}

static void zoom_to(struct app *a, double nz, double ax, double ay)           /* ax / ay: a point of the canvas that stays where it is */
{
    nz = LM_CLAMP(nz, 0.02, 48.0);
    double ix = (ax - a->vx) / a->zoom, iy = (ay - a->vy) / a->zoom;
    a->zoom = nz;
    a->vx = ax - ix * nz;
    a->vy = ay - iy * nz;
    a->fitted = false;
    if (CW(a) * nz <= cv_w(a))
        a->vx = (cv_w(a) - CW(a) * nz) / 2.0;
    if (CH(a) * nz <= cv_h(a))
        a->vy = (cv_h(a) - CH(a) * nz) / 2.0;
    view_clamp(a);
    a->vdirty = true;
    a->win.redraw = true;
}

static void to_pic(const struct app *a, double sx, double sy, double *ix, double *iy)
{
    *ix = (sx - cv_x(a) - a->vx) / a->zoom;
    *iy = (sy - a->vy) / a->zoom;
}

/* ---- the composited picture ------------------------------------------------------------------------------------------------------ */

static void flat_mark(struct app *a, struct irect r)
{
    irect_clip(&r, CW(a), CH(a));
    if (!irect_empty(&r))
        irect_add(&a->fdirty, r.x0, r.y0, r.x1, r.y1);
    a->vdirty = true;
    a->win.redraw = true;
}

static void flat_mark_all(struct app *a)
{
    flat_mark(a, (struct irect){ 0, 0, CW(a), CH(a) });
}

static void flat_refresh(struct app *a)
{
    int w = CW(a), h = CH(a);
    if (a->flat.w != w || a->flat.h != h) {
        pix_free(&a->flat);
        if (!pix_alloc(&a->flat, w, h, 0))
            return;
        a->fdirty = (struct irect){ 0, 0, w, h };
    }
    if (!irect_empty(&a->fdirty)) {
        doc_composite(&a->doc, &a->flat, a->fdirty);
        a->fdirty = (struct irect){ 0, 0, 0, 0 };
    }
}

static void changed(struct app *a)
{
    a->modified = true;
    flat_mark_all(a);
}

static void changed_r(struct app *a, struct irect r)
{
    a->modified = true;
    flat_mark(a, r);
}

/* ---- undo ----------------------------------------------------------------------------------------------------------------------- */

static size_t snap_bytes(const struct snap *s)
{
    if (s->kind == SK_PIX)
        return (size_t)s->im.w * (size_t)s->im.h * 4;
    if (s->kind == SK_DOC && s->doc) {
        size_t b = 0;
        for (int i = 0; i < s->doc->n; i++)
            b += (size_t)s->doc->L[i].im.w * (size_t)s->doc->L[i].im.h * 4;
        return b;
    }
    return 0;
}

static void snap_free(struct snap *s)
{
    pix_free(&s->im);
    if (s->doc) {
        doc_free(s->doc);
        free(s->doc);
    }
    memset(s, 0, sizeof(*s));
}

static void drop_snaps(struct snap *s, int *n)
{
    for (int i = 0; i < *n; i++)
        snap_free(&s[i]);
    *n = 0;
}

/* The present state as a snapshot of that kind (SK_PIX of layer `layer`). */
static bool snap_make(const struct app *a, int kind, int layer, struct snap *out)
{
    memset(out, 0, sizeof(*out));
    out->kind = kind;
    out->layer = layer;
    if (kind == SK_PIX)
        return layer >= 0 && layer < a->doc.n && pix_copy(&a->doc.L[layer].im, &out->im);
    out->doc = malloc(sizeof(*out->doc));
    if (!out->doc)
        return false;
    if (kind == SK_DOC)
        return doc_copy(&a->doc, out->doc) || (free(out->doc), out->doc = NULL, false);
    *out->doc = a->doc;                                                 /* SK_PROPS: only what is not a pixel */
    for (int i = 0; i < out->doc->n; i++)
        out->doc->L[i].im.p = NULL;
    return true;
}

static void snaps_push(struct snap *s, int *n, struct snap *sn)
{
    if (*n == MAX_UNDO) {
        snap_free(&s[0]);
        memmove(&s[0], &s[1], sizeof(s[0]) * (MAX_UNDO - 1));
        (*n)--;
    }
    s[(*n)++] = *sn;
    for (;;) {
        size_t b = 0;
        for (int i = 0; i < *n; i++)
            b += snap_bytes(&s[i]);
        if (b <= UNDO_BYTES || *n <= 1)
            break;
        snap_free(&s[0]);
        memmove(&s[0], &s[1], sizeof(s[0]) * (size_t)(*n - 1));
        (*n)--;
    }
}

/* Puts a snapshot back (it is used up). */
static void snap_apply(struct app *a, struct snap *s)
{
    if (s->kind == SK_PIX) {
        if (s->layer >= 0 && s->layer < a->doc.n && s->im.w == a->doc.L[s->layer].im.w && s->im.h == a->doc.L[s->layer].im.h) {
            pix_free(&a->doc.L[s->layer].im);
            a->doc.L[s->layer].im = s->im;
            s->im = (struct pix){ 0 };
        }
    } else if (s->kind == SK_DOC && s->doc) {
        doc_free(&a->doc);
        a->doc = *s->doc;
        free(s->doc);
        s->doc = NULL;
    } else if (s->kind == SK_PROPS && s->doc && s->doc->n == a->doc.n) {
        for (int i = 0; i < a->doc.n; i++) {
            memcpy(a->doc.L[i].name, s->doc->L[i].name, sizeof(a->doc.L[i].name));
            a->doc.L[i].opacity = s->doc->L[i].opacity;
            a->doc.L[i].visible = s->doc->L[i].visible;
            a->doc.L[i].blend = s->doc->L[i].blend;
        }
        a->doc.cur = s->doc->cur;
        a->doc.paper = s->doc->paper;
    }
    snap_free(s);
}

static void checkpoint_kind(struct app *a, int kind)
{
    struct snap sn;
    if (snap_make(a, kind, a->doc.cur, &sn))
        snaps_push(a->undo, &a->nundo, &sn);
    drop_snaps(a->redo, &a->nredo);
}

/* Call before a change of the chosen layer's pixels: keeps them as they are, forgets what could be redone. */
static void checkpoint(struct app *a) { checkpoint_kind(a, SK_PIX); }
static void checkpoint_doc(struct app *a) { checkpoint_kind(a, SK_DOC); }
static void checkpoint_props(struct app *a) { checkpoint_kind(a, SK_PROPS); }

/* The same with a copy of `im` as it was (the layer is already changed: text, the Adjust card). */
static void checkpoint_with(struct app *a, const struct pix *im)
{
    struct snap sn;
    memset(&sn, 0, sizeof(sn));
    sn.kind = SK_PIX;
    sn.layer = a->doc.cur;
    if (pix_copy(im, &sn.im))
        snaps_push(a->undo, &a->nundo, &sn);
    drop_snaps(a->redo, &a->nredo);
}

static void sel_clip(struct app *a)
{
    if (!a->sel.on)
        return;
    int w = CW(a), h = CH(a);
    struct irect r = a->sel.r;
    if (!a->sel.mask) {
        irect_clip(&a->sel.r, w, h);
        if (irect_empty(&a->sel.r))
            sel_clear(&a->sel);
        return;
    }
    if (r.x0 < 0 || r.y0 < 0 || r.x1 > w || r.y1 > h || irect_empty(&r))
        sel_clear(&a->sel);                                             /* (a masked selection is only ever cut by a change of the picture's size) */
}

static void finish_pending(struct app *a);

static void do_undo(struct app *a, bool redo)
{
    finish_pending(a);
    struct snap *from = redo ? a->redo : a->undo, *to = redo ? a->undo : a->redo;
    int *nf = redo ? &a->nredo : &a->nundo, *nt = redo ? &a->nundo : &a->nredo;
    if (*nf == 0) {
        say(a, redo ? "Nothing to redo" : "Nothing to undo");
        return;
    }
    struct snap top = from[*nf - 1], inv;
    (*nf)--;
    if (snap_make(a, top.kind, top.layer, &inv))
        snaps_push(to, nt, &inv);
    snap_apply(a, &top);
    sel_clear(&a->sel);
    changed(a);
    if (a->fitted)
        view_fit(a);
    else {
        view_clamp(a);
        a->vdirty = true;
    }
    say(a, redo ? "Redone" : "Undone");
}

/* ---- colours -------------------------------------------------------------------------------------------------------------------- */

static void push_recent(struct app *a, uint32_t c)
{
    if (ch_a(c) == 0)
        return;
    int at = -1;
    for (int i = 0; i < a->nrecent; i++)
        if (a->recent[i] == c)
            at = i;
    if (at == 0)
        return;
    if (at < 0) {
        if (a->nrecent < N_RECENT)
            a->nrecent++;
        at = a->nrecent - 1;
    }
    memmove(&a->recent[1], &a->recent[0], sizeof(a->recent[0]) * (size_t)at);
    a->recent[0] = c;
}

static uint32_t draw_color(const struct app *a)
{
    return a->drag_button == 273 ? a->bg : a->fg;
}

static int *slider_ptr(struct app *a, int id)
{
    return id == S_SIZE && a->tool == T_TEXT ? &a->tsize : &a->val[id];
}

static void slider_range(const struct app *a, int id, int *mn, int *mx)
{
    *mn = SLIDERS[id].mn;
    *mx = SLIDERS[id].mx;
    if (id == S_SIZE && a->tool == T_TEXT) {
        *mn = 6;
        *mx = 400;
    }
}

static void slider_set(struct app *a, int id, int v)
{
    int mn, mx;
    slider_range(a, id, &mn, &mx);
    *slider_ptr(a, id) = LM_CLAMP(v, mn, mx);
    a->win.redraw = true;
}

/* ---- the operations the buttons run ---------------------------------------------------------------------------------------------- */

static void new_doc(struct app *a, int w, int h, const char *path, bool transparent)
{
    struct pix im;
    if (!pix_alloc(&im, w, h, 0)) {
        say(a, "That size is not possible (1..%d, at most %u million pixels)", MAX_SIDE, LM_IMG_MAX_PIXELS / 1000000u);
        return;
    }
    finish_pending(a);
    doc_free(&a->doc);
    doc_from_pix(&a->doc, &im);                                         /* an empty layer: the white is the sheet's own colour (the paper), so Move carries the drawing and not the white */
    snprintf(a->doc.L[0].name, sizeof(a->doc.L[0].name), "Layer 1");
    a->doc.paper = transparent ? 0 : 0xffffffffu;
    drop_snaps(a->undo, &a->nundo);
    drop_snaps(a->redo, &a->nredo);
    snprintf(a->path, sizeof(a->path), "%s", path ? path : "");
    a->modified = false;
    sel_clear(&a->sel);
    a->lay_top = 0;
    flat_mark_all(a);
    view_fit(a);
    say(a, "New picture, %d x %d", w, h);
}

static bool open_image(struct app *a, const char *path)
{
    char err[160];
    struct doc d;
    memset(&d, 0, sizeof(d));
    if (!load_doc(path, &d, err, sizeof(err))) {
        say(a, "Could not open %s: %s", path, err);
        snprintf(a->perr, sizeof(a->perr), "%s", err);
        return false;
    }
    finish_pending(a);
    doc_free(&a->doc);
    a->doc = d;
    drop_snaps(a->undo, &a->nundo);
    drop_snaps(a->redo, &a->nredo);
    char real[PATH_MAX];
    snprintf(a->path, sizeof(a->path), "%s", realpath(path, real) ? real : path);
    a->modified = false;
    sel_clear(&a->sel);
    a->lay_top = 0;
    flat_mark_all(a);
    view_fit(a);
    const char *base = strrchr(a->path, '/');
    say(a, "Opened %s, %d x %d%s", base ? base + 1 : a->path, CW(a), CH(a), d.n > 1 ? ", with layers" : "");
    return true;
}

static bool write_image(struct app *a, const char *path)
{
    char err[160] = "";
    finish_pending(a);
    bool ok;
    int layers = a->doc.n;
    if (is_ora(path))
        ok = write_ora(&a->doc, path, err, sizeof(err));
    else {
        struct pix flat;
        ok = doc_flatten(&a->doc, &flat) && save_file(&flat, path, err, sizeof(err));
        if (flat.p)
            pix_free(&flat);
    }
    if (!ok) {
        say(a, "Could not save: %s", err[0] ? err : "out of memory");
        snprintf(a->perr, sizeof(a->perr), "%s", err);
        return false;
    }
    snprintf(a->path, sizeof(a->path), "%s", path);
    a->modified = false;
    const char *base = strrchr(path, '/');
    if (layers > 1 && !is_ora(path))
        say(a, "Saved %s as one flat picture - use .ora to keep the layers", base ? base + 1 : path);
    else
        say(a, "Saved %s", base ? base + 1 : path);
    return true;
}

static void whole_op(struct app *a, const char *what, bool ok)
{
    if (!ok) {
        say(a, "Not possible (out of memory?)");
        return;
    }
    sel_clear(&a->sel);
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
    finish_pending(a);
    struct irect r = a->sel.on ? a->sel.r : (struct irect){ 0, 0, CW(a), CH(a) };
    struct pix before = { 0 };
    bool ok = !a->sel.on || !a->sel.mask || pix_copy(&IM(a), &before);
    if (ok) {
        checkpoint(a);
        ok = apply_fx(&IM(a), f, r);
        if (ok && before.p)
            sel_restore(&IM(a), &before, r, &a->sel);
    }
    pix_free(&before);
    if (!ok) {
        say(a, "Not possible (out of memory?)");
        return;
    }
    changed(a);
    say(a, "%s%s", FX_NAMES[f], a->sel.on ? " (the selection)" : "");
}

static void do_crop(struct app *a)
{
    if (!a->sel.on) {
        say(a, "Select an area first (the Select tool)");
        return;
    }
    finish_pending(a);
    struct irect r = a->sel.r;
    irect_clip(&r, CW(a), CH(a));
    checkpoint_doc(a);
    bool ok = true;
    for (int i = 0; i < a->doc.n && ok; i++)
        ok = crop(&a->doc.L[i].im, r);
    whole_op(a, "Cropped", ok);
}

/* Takes away what the selection holds (the whole layer when nothing is selected). */
static void do_delete_sel(struct app *a)
{
    finish_pending(a);
    checkpoint(a);
    struct irect r = a->sel.on ? a->sel.r : (struct irect){ 0, 0, CW(a), CH(a) };
    irect_clip(&r, CW(a), CH(a));
    for (int y = r.y0; y < r.y1; y++)
        for (int x = r.x0; x < r.x1; x++) {
            int m = sel_at(&a->sel, x, y);
            if (m <= 0)
                continue;
            uint32_t *q = &IM(a).p[(size_t)y * (size_t)CW(a) + (size_t)x];
            *q = m >= 255 ? 0 : blend(*q, 0, m);
        }
    changed(a);
    say(a, a->sel.on ? "Cleared the selection" : "Cleared the layer");
}

/* Paints the selection (the whole layer when nothing is selected) with the pen colour. */
static void do_fill_sel(struct app *a)
{
    finish_pending(a);
    checkpoint(a);
    struct irect r = a->sel.on ? a->sel.r : (struct irect){ 0, 0, CW(a), CH(a) };
    irect_clip(&r, CW(a), CH(a));
    int op = a->val[S_OPACITY];
    for (int y = r.y0; y < r.y1; y++)
        for (int x = r.x0; x < r.x1; x++) {
            int m = sel_at(&a->sel, x, y) * op / 100;
            if (m <= 0)
                continue;
            uint32_t *q = &IM(a).p[(size_t)y * (size_t)CW(a) + (size_t)x];
            *q = blend(*q, a->fg, m);
        }
    push_recent(a, a->fg);
    changed(a);
    say(a, "Filled %s", a->sel.on ? "the selection" : "the layer");
}

/* The selection from a coverage mask. */
static void sel_set_cov(struct app *a, const struct cov *c, struct irect r)
{
    sel_from_cov(&a->sel, c, r);
    a->win.redraw = true;
    a->vdirty = true;
}

static void sel_all(struct app *a)
{
    sel_clear(&a->sel);
    a->sel.on = true;
    a->sel.r = (struct irect){ 0, 0, CW(a), CH(a) };
    a->vdirty = true;
    a->win.redraw = true;
}

static void sel_invert(struct app *a)
{
    int w = CW(a), h = CH(a);
    uint8_t *m = malloc((size_t)w * (size_t)h);
    if (!m)
        return;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            m[(size_t)y * (size_t)w + (size_t)x] = (uint8_t)(255 - sel_at(&a->sel, x, y));
    sel_clear(&a->sel);
    a->sel.on = true;
    a->sel.r = (struct irect){ 0, 0, w, h };
    a->sel.mask = m;
    a->vdirty = true;
    a->win.redraw = true;
    say(a, "Selection inverted");
}

/* Copy and cut: the chosen layer's pixels in the selection (the whole layer without one), see-through where the selection is soft or does not reach. */
static void do_copy(struct app *a, bool cut)
{
    finish_pending(a);
    struct irect r = a->sel.on ? a->sel.r : (struct irect){ 0, 0, CW(a), CH(a) };
    irect_clip(&r, CW(a), CH(a));
    if (irect_empty(&r))
        return;
    struct pix img;
    if (!pix_alloc(&img, r.x1 - r.x0, r.y1 - r.y0, 0)) {
        say(a, "Not possible (out of memory?)");
        return;
    }
    for (int y = r.y0; y < r.y1; y++)
        for (int x = r.x0; x < r.x1; x++) {
            uint32_t c = IM(a).p[(size_t)y * (size_t)CW(a) + (size_t)x];
            int m = sel_at(&a->sel, x, y);
            img.p[(size_t)(y - r.y0) * (size_t)img.w + (size_t)(x - r.x0)] = m >= 255 ? c : m <= 0 ? 0 : argb(ch_a(c) * m / 255, ch_r(c), ch_g(c), ch_b(c));
        }
    pix_free(&a->clipimg);
    a->clipimg = img;
    if (cut)
        do_delete_sel(a);
    say(a, "%s %d x %d", cut ? "Cut" : "Copied", img.w, img.h);
}

static void tool_set(struct app *a, enum tool t);

/* Paste: the picture becomes a new layer at the top left of what is in view; the Move tool is chosen so that it can be put in place. */
static void do_paste(struct app *a)
{
    finish_pending(a);
    if (!a->clipimg.p) {
        say(a, "Nothing copied yet");
        return;
    }
    if (a->doc.n >= MAX_LAYERS) {
        say(a, "No more than %d layers", MAX_LAYERS);
        return;
    }
    double ix, iy;
    to_pic(a, cv_x(a), 0, &ix, &iy);
    int ox = LM_CLAMP((int)ceil(ix) + 8, 0, LM_MAX(CW(a) - a->clipimg.w, 0)), oy = LM_CLAMP((int)ceil(iy) + 8, 0, LM_MAX(CH(a) - a->clipimg.h, 0));
    checkpoint_doc(a);
    if (!doc_add(&a->doc, "Pasted", 0)) {
        say(a, "Not possible (out of memory?)");
        return;
    }
    for (int y = 0; y < a->clipimg.h && y + oy < CH(a); y++)
        for (int x = 0; x < a->clipimg.w && x + ox < CW(a); x++)
            IM(a).p[(size_t)(y + oy) * (size_t)CW(a) + (size_t)(x + ox)] = a->clipimg.p[(size_t)y * (size_t)a->clipimg.w + (size_t)x];
    sel_clear(&a->sel);
    changed(a);
    tool_set(a, T_MOVE);
    say(a, "Pasted as a new layer: the Move tool puts it in place");
}

/* ---- layers --------------------------------------------------------------------------------------------------------------------- */

static void layer_select(struct app *a, int i)
{
    if (i < 0 || i >= a->doc.n || i == a->doc.cur)
        return;
    finish_pending(a);
    a->doc.cur = i;
    a->win.redraw = true;
}

static void layer_add(struct app *a)
{
    finish_pending(a);
    if (a->doc.n >= MAX_LAYERS) {
        say(a, "No more than %d layers", MAX_LAYERS);
        return;
    }
    checkpoint_doc(a);
    char nm[40];
    snprintf(nm, sizeof(nm), "Layer %d", a->doc.n + 1);
    if (!doc_add(&a->doc, nm, 0)) {
        say(a, "Not possible (out of memory?)");
        return;
    }
    changed(a);
    say(a, "New layer");
}

static void layer_dup(struct app *a)
{
    finish_pending(a);
    if (a->doc.n >= MAX_LAYERS) {
        say(a, "No more than %d layers", MAX_LAYERS);
        return;
    }
    checkpoint_doc(a);
    if (!doc_dup(&a->doc)) {
        say(a, "Not possible (out of memory?)");
        return;
    }
    changed(a);
    say(a, "Layer duplicated");
}

static void layer_delete(struct app *a)
{
    finish_pending(a);
    if (a->doc.n <= 1) {
        say(a, "The last layer stays");
        return;
    }
    checkpoint_doc(a);
    doc_del(&a->doc);
    changed(a);
    say(a, "Layer deleted");
}

static void layer_shift(struct app *a, int dir)
{
    finish_pending(a);
    int to = a->doc.cur + dir;
    if (to < 0 || to >= a->doc.n)
        return;
    checkpoint_doc(a);
    doc_move(&a->doc, dir);
    changed(a);
}

static void layer_merge(struct app *a)
{
    finish_pending(a);
    if (a->doc.cur < 1) {
        say(a, "There is no layer below to merge into");
        return;
    }
    checkpoint_doc(a);
    doc_merge_down(&a->doc);
    changed(a);
    say(a, "Merged down");
}

static void layer_toggle(struct app *a, int i)
{
    if (i < 0 || i >= a->doc.n)
        return;
    checkpoint_props(a);
    a->doc.L[i].visible = !a->doc.L[i].visible;
    changed(a);
}

static void layer_rename(struct app *a, const char *name)
{
    checkpoint_props(a);
    snprintf(a->doc.L[a->doc.cur].name, sizeof(a->doc.L[0].name), "%.39s", name);
    a->modified = true;
    a->win.redraw = true;
}

/* Turns every layer (the whole picture): `fn` runs on each. */
static bool all_layers(struct app *a, bool (*fn)(struct pix *, void *), void *ud)
{
    for (int i = 0; i < a->doc.n; i++)
        if (!fn(&a->doc.L[i].im, ud))
            return false;
    return true;
}

static bool fn_rotate(struct pix *im, void *ud) { return rotate90(im, *(bool *)ud); }
static bool fn_flip(struct pix *im, void *ud) { flip(im, *(bool *)ud); return true; }
static bool fn_resize(struct pix *im, void *ud) { const int *s = ud; return resize(im, s[0], s[1]); }

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

/* ---- text --------------------------------------------------------------------------------------------------------------------------- */

/* Lays the text out from (txt.x, txt.y): draws it into `c` when given, and says where the caret is, how tall a line is and how big the whole is (picture pixels, relative to
 * the start). The letters are the program's labels scaled to the size in the strip. */
static void text_layout(struct app *a, struct cov *c, struct irect *dirty, int *cax, int *cay, int *lhp, int *tw, int *th)
{
    double scale = LM_MAX(a->tsize / (double)LM_FONT_H, 0.5);
    int lh = (int)ceil(LM_FONT_H * scale), penx = 0, peny = 0, maxx = 0, caret_x = 0, caret_y = 0;
    bool bold = a->tg[TG_BOLD];
    for (int i = 0; i <= a->txt.len;) {
        if (i == a->txt.caret) {
            caret_x = penx;
            caret_y = peny;
        }
        if (i == a->txt.len)
            break;
        int used = 1;
        uint32_t cp = lm_utf8_decode(a->txt.buf + i, a->txt.len - i, &used);
        i += used > 0 ? used : 1;
        if (cp == '\n') {
            penx = 0;
            peny += lh;
            continue;
        }
        struct lm_font_glyph g;
        lm_font_label_scaled(cp, scale, bold, &g);
        if (c && g.px) {
            for (int gy = 0; gy < g.h; gy++) {
                int y = a->txt.y + peny + gy;
                if (y < 0 || y >= c->h)
                    continue;
                for (int gx = 0; gx < g.w; gx++) {
                    int x = a->txt.x + penx - g.left + gx;
                    uint8_t v = g.px[(size_t)gy * (size_t)g.w + (size_t)gx];
                    if (x < 0 || x >= c->w || !v)
                        continue;
                    uint8_t *q = &c->p[(size_t)y * (size_t)c->w + (size_t)x];
                    if (v > *q)
                        *q = v;
                }
            }
            if (dirty)
                irect_add(dirty, a->txt.x + penx - g.left, a->txt.y + peny, a->txt.x + penx - g.left + g.w, a->txt.y + peny + g.h);
        }
        penx += g.advance;
        maxx = LM_MAX(maxx, penx);
    }
    if (cax)
        *cax = caret_x;
    if (cay)
        *cay = caret_y;
    if (lhp)
        *lhp = lh;
    if (tw)
        *tw = LM_MAX(maxx, 4);
    if (th)
        *th = peny + lh;
}

/* Draws the text on the layer from the layer as it was before (so that it can be redrawn at every key). */
static void text_preview(struct app *a)
{
    struct irect old = a->preview, nu = { 0, 0, 0, 0 };
    if (!irect_empty(&old))
        cov_clear(&a->cov, old);
    text_layout(a, &a->cov, &nu, NULL, NULL, NULL, NULL, NULL);
    struct irect r = old;
    if (!irect_empty(&nu))
        irect_add(&r, nu.x0, nu.y0, nu.x1, nu.y1);
    a->preview = nu;
    irect_clip(&r, CW(a), CH(a));
    if (!irect_empty(&r)) {
        cov_commit(&IM(a), &a->tbefore, &a->cov, r, a->fg, a->val[S_OPACITY], false, &a->sel);
        flat_mark(a, r);
    }
    a->win.redraw = true;
}

static void text_end(struct app *a)
{
    pix_free(&a->tbefore);
    cov_free(&a->cov);
    a->txt.on = false;
    a->txt.len = a->txt.caret = 0;
    a->txt.buf[0] = 0;
    a->preview = (struct irect){ 0, 0, 0, 0 };
    a->win.redraw = true;
}

static void text_begin(struct app *a, int ix, int iy)
{
    if (!pix_copy(&IM(a), &a->tbefore))
        return;
    if (!cov_alloc(&a->cov, CW(a), CH(a))) {
        pix_free(&a->tbefore);
        return;
    }
    a->txt.on = true;
    a->txt.x = ix;
    a->txt.y = iy;
    a->txt.len = a->txt.caret = 0;
    a->txt.buf[0] = 0;
    a->preview = (struct irect){ 0, 0, 0, 0 };
    a->win.redraw = true;
}

static void text_commit(struct app *a)
{
    if (!a->txt.on)
        return;
    if (a->txt.len > 0) {
        checkpoint_with(a, &a->tbefore);
        push_recent(a, a->fg);
        changed(a);
        say(a, "Text put on the layer");
    }
    text_end(a);
}

static void text_cancel(struct app *a)
{
    if (!a->txt.on)
        return;
    if (a->tbefore.p && a->tbefore.w == IM(a).w && a->tbefore.h == IM(a).h) {
        memcpy(IM(a).p, a->tbefore.p, (size_t)IM(a).w * (size_t)IM(a).h * 4);
        flat_mark_all(a);
    }
    text_end(a);
}

/* Whatever is half done (a text being typed) is put down before something else happens. */
static void finish_pending(struct app *a)
{
    text_commit(a);
}

static void text_insert(struct app *a, const char *s, int n)
{
    if (n <= 0 || a->txt.len + n >= (int)sizeof(a->txt.buf))
        return;
    memmove(a->txt.buf + a->txt.caret + n, a->txt.buf + a->txt.caret, (size_t)(a->txt.len - a->txt.caret) + 1);
    memcpy(a->txt.buf + a->txt.caret, s, (size_t)n);
    a->txt.len += n;
    a->txt.caret += n;
}

static int text_prev(const struct app *a, int i)
{
    if (i <= 0)
        return 0;
    i--;
    while (i > 0 && (a->txt.buf[i] & 0xc0) == 0x80)
        i--;
    return i;
}

static int text_next(const struct app *a, int i)
{
    if (i >= a->txt.len)
        return a->txt.len;
    i++;
    while (i < a->txt.len && (a->txt.buf[i] & 0xc0) == 0x80)
        i++;
    return i;
}

static int text_line_start(const struct app *a, int i)
{
    while (i > 0 && a->txt.buf[i - 1] != '\n')
        i--;
    return i;
}

/* A key while a text is being typed; false = it is none of the text's business. */
static bool text_key(struct app *a, uint32_t ks, uint32_t mods)
{
    bool ctrl = (mods & LM_MOD_CTRL) != 0;
    char txt[8];
    if (ks == LM_KS_Escape) {
        text_cancel(a);
        return true;
    }
    if ((ks == LM_KS_Return || ks == LM_KS_KP_Enter) && ctrl) {
        text_commit(a);
        return true;
    }
    if (ks == LM_KS_Return || ks == LM_KS_KP_Enter)
        text_insert(a, "\n", 1);
    else if (ks == LM_KS_BackSpace) {
        int p = text_prev(a, a->txt.caret);
        memmove(a->txt.buf + p, a->txt.buf + a->txt.caret, (size_t)(a->txt.len - a->txt.caret) + 1);
        a->txt.len -= a->txt.caret - p;
        a->txt.caret = p;
    } else if (ks == LM_KS_Delete) {
        int n = text_next(a, a->txt.caret);
        memmove(a->txt.buf + a->txt.caret, a->txt.buf + n, (size_t)(a->txt.len - n) + 1);
        a->txt.len -= n - a->txt.caret;
    } else if (ks == LM_KS_Left)
        a->txt.caret = text_prev(a, a->txt.caret);
    else if (ks == LM_KS_Right)
        a->txt.caret = text_next(a, a->txt.caret);
    else if (ks == LM_KS_Home)
        a->txt.caret = text_line_start(a, a->txt.caret);
    else if (ks == LM_KS_End) {
        while (a->txt.caret < a->txt.len && a->txt.buf[a->txt.caret] != '\n')
            a->txt.caret++;
    } else if (ks == LM_KS_Up || ks == LM_KS_Down) {
        int ls = text_line_start(a, a->txt.caret), col = 0;
        for (int i = ls; i < a->txt.caret; i = text_next(a, i))
            col++;
        int target;
        if (ks == LM_KS_Up) {
            if (ls == 0)
                return true;
            target = text_line_start(a, ls - 1);
        } else {
            int e = a->txt.caret;
            while (e < a->txt.len && a->txt.buf[e] != '\n')
                e++;
            if (e >= a->txt.len)
                return true;
            target = e + 1;
        }
        int i = target;
        while (col > 0 && i < a->txt.len && a->txt.buf[i] != '\n') {
            i = text_next(a, i);
            col--;
        }
        a->txt.caret = i;
    } else if (ctrl && (ks == 'v' || ks == 'V')) {
        size_t n = 0;
        char *t = wlclip_paste(&a->clip, &n);
        if (t) {
            for (size_t i = 0; i < n; i++)
                if ((t[i] != '\r' && (unsigned char)t[i] >= 32) || t[i] == '\n')
                    text_insert(a, &t[i], 1);
            free(t);
        }
    } else if (!ctrl && lm_keysym_to_utf8(ks, txt) > 0 && (unsigned char)txt[0] >= 32 && txt[0] != 127)
        text_insert(a, txt, (int)strlen(txt));
    else
        return false;                                                   /* (the Ctrl shortcuts and the other keys of the program still work) */
    text_preview(a);
    return true;
}

/* ---- the tool in use ------------------------------------------------------------------------------------------------------------ */

static void tool_set(struct app *a, enum tool t)
{
    finish_pending(a);
    a->tool = t;
    if (tool_in_select(t))
        a->grp_sel = t;
    if (tool_in_shapes(t))
        a->grp_shape = t;
    a->msg[0] = 0;
    a->win.redraw = true;
}

/* ---- the colour card ------------------------------------------------------------------------------------------------------------ */

#define CP_W 580
#define CP_H 336

struct cp_geo {
    struct wlcard c;
    int sqx, sqy, sq;                           /* the square of saturation (across) and brightness (up) */
    int hx, hy, hw, hh;                         /* the hue bar */
    int ax, ay, aw, ah;                         /* the alpha bar */
    int rx;                                     /* the right column */
    int swy;                                    /* the new / old swatches */
    int fx, fy, fw, fh;                         /* the hex field */
    int qx, qy, qside, qpitch;                  /* the quick colours */
};

static void cp_geom(const struct app *a, struct cp_geo *g)
{
    g->c.w = LM_MIN(CP_W, a->win.bw - 16);
    g->c.h = LM_MIN(CP_H, a->win.bh - 16);
    g->c.x = (a->win.bw - g->c.w) / 2;
    g->c.y = (a->win.bh - g->c.h) / 2;
    g->sqx = g->c.x + 16;
    g->sqy = g->c.y + 42;
    g->sq = LM_CLAMP(g->c.h - 42 - 60 - 34, 90, 190);
    g->hx = g->sqx + g->sq + 10;
    g->hy = g->sqy;
    g->hw = 20;
    g->hh = g->sq;
    g->ax = g->sqx;
    g->ay = g->sqy + g->sq + 10;
    g->aw = g->sq + 10 + g->hw;
    g->ah = 18;
    g->rx = g->hx + g->hw + 24;
    g->swy = g->sqy;
    g->fx = g->rx;
    g->fy = g->sqy + 52;
    g->fw = 150;
    g->fh = 26;
    g->qx = g->rx;
    g->qy = g->sqy + 124;
    g->qside = 18;
    g->qpitch = 22;
}

static uint32_t cp_colour(const struct cpick *cp)
{
    int r, g, b;
    hsv_to_rgb(cp->h, cp->s, cp->v, &r, &g, &b);
    return argb(cp->alpha, r, g, b);
}

static void cp_hex_text(uint32_t c, char *out, size_t n)
{
    if (ch_a(c) == 255)
        snprintf(out, n, "#%02x%02x%02x", ch_r(c), ch_g(c), ch_b(c));
    else
        snprintf(out, n, "#%02x%02x%02x%02x", ch_r(c), ch_g(c), ch_b(c), ch_a(c));
}

static bool cp_parse_hex(const char *s, uint32_t *out)
{
    if (*s == '#')
        s++;
    size_t n = strlen(s);
    if (n != 3 && n != 6 && n != 8)
        return false;
    uint32_t v = 0;
    for (size_t i = 0; i < n; i++) {
        if (!isxdigit((unsigned char)s[i]))
            return false;
        v = v * 16 + (uint32_t)(isdigit((unsigned char)s[i]) ? s[i] - '0' : tolower((unsigned char)s[i]) - 'a' + 10);
    }
    if (n == 3)
        *out = argb(255, ((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17);
    else if (n == 6)
        *out = 0xff000000u | v;
    else
        *out = (v << 24) | (v >> 8);                                    /* rrggbbaa */
    return true;
}

/* Sets the picker to a colour (and the field's text unless the text is what is being typed). */
static void cp_set(struct app *a, uint32_t c, bool rewrite)
{
    if (ch_a(c) == 0)
        c = argb(0, ch_r(c), ch_g(c), ch_b(c));
    rgb_to_hsv(ch_r(c), ch_g(c), ch_b(c), &a->cp.h, &a->cp.s, &a->cp.v);
    a->cp.alpha = ch_a(c);
    if (rewrite) {
        cp_hex_text(c, a->pbuf, sizeof(a->pbuf));
        wltext_end(a->pbuf);
    }
    if (a->cp.bg)
        a->bg = c;
    else
        a->fg = c;
    a->win.redraw = true;
}

/* The hue / saturation / brightness changed: the colour follows. */
static void cp_changed(struct app *a)
{
    uint32_t c = cp_colour(&a->cp);
    if (a->cp.bg)
        a->bg = c;
    else
        a->fg = c;
    cp_hex_text(c, a->pbuf, sizeof(a->pbuf));
    wltext_end(a->pbuf);
    a->win.redraw = true;
}

static void cp_open(struct app *a, bool bg)
{
    finish_pending(a);
    a->cp.bg = bg;
    a->cp.orig = bg ? a->bg : a->fg;
    a->cp.drag = 0;
    uint32_t c = a->cp.orig;
    rgb_to_hsv(ch_r(c), ch_g(c), ch_b(c), &a->cp.h, &a->cp.s, &a->cp.v);
    a->cp.alpha = ch_a(c);
    a->prompt = P_COLOUR;
    a->perr[0] = 0;
    a->pcard_hover = a->pcard_down = -1;
    cp_hex_text(c, a->pbuf, sizeof(a->pbuf));
    wltext_end(a->pbuf);
    a->win.redraw = true;
}

/* A press or a drag at (x, y) on one of the picker's three gauges. */
static void cp_drag_to(struct app *a, double x, double y)
{
    struct cp_geo g;
    cp_geom(a, &g);
    if (a->cp.drag == 1) {
        a->cp.s = LM_CLAMP((x - g.sqx) / (g.sq - 1.0), 0.0, 1.0);
        a->cp.v = 1.0 - LM_CLAMP((y - g.sqy) / (g.sq - 1.0), 0.0, 1.0);
    } else if (a->cp.drag == 2)
        a->cp.h = LM_CLAMP((y - g.hy) / (g.hh - 1.0), 0.0, 0.9999) * 360.0;
    else if (a->cp.drag == 3)
        a->cp.alpha = (int)(LM_CLAMP((x - g.ax) / (g.aw - 1.0), 0.0, 1.0) * 255.0 + 0.5);
    cp_changed(a);
}

/* ---- the adjust card ------------------------------------------------------------------------------------------------------------ */

#define ADJ_N 4
static const char *const ADJ_NAMES[ADJ_N] = { "Brightness", "Contrast", "Hue", "Saturation" };
static const int ADJ_MIN[ADJ_N] = { -100, -100, -180, -100 }, ADJ_MAX[ADJ_N] = { 100, 100, 180, 100 };

static int *adj_ptr(struct app *a, int i)
{
    return i == 0 ? &a->adj.brightness : i == 1 ? &a->adj.contrast : i == 2 ? &a->adj.hue : &a->adj.saturation;
}

static void adj_apply(struct app *a)
{
    if (!a->adj_before.p || a->adj_before.w != IM(a).w || a->adj_before.h != IM(a).h)
        return;
    memcpy(IM(a).p, a->adj_before.p, (size_t)IM(a).w * (size_t)IM(a).h * 4);
    struct irect r = a->sel.on ? a->sel.r : (struct irect){ 0, 0, CW(a), CH(a) };
    if (a->adj.brightness || a->adj.contrast || a->adj.hue || a->adj.saturation) {
        adjust_region(&IM(a), r, &a->adj);
        sel_restore(&IM(a), &a->adj_before, r, &a->sel);
    }
    flat_mark_all(a);
}

static void adj_open(struct app *a)
{
    finish_pending(a);
    if (!pix_copy(&IM(a), &a->adj_before)) {
        say(a, "Not possible (out of memory?)");
        return;
    }
    memset(&a->adj, 0, sizeof(a->adj));
    a->adj_drag = -1;
    a->prompt = P_ADJUST;
    a->perr[0] = 0;
    a->pcard_hover = a->pcard_down = -1;
    a->win.redraw = true;
}

/* The card's rows: where each slider's track lies. */
static void adj_track(const struct wlcard *c, int i, int *x, int *y, int *w)
{
    *x = c->x + 128;
    *w = c->w - 128 - 76;
    *y = c->y + 52 + i * 38 + 8;
}

static void prompt_close(struct app *a, bool apply);

static void ask_resize(struct app *a)
{
    char s[32];
    snprintf(s, sizeof(s), "%dx%d", CW(a), CH(a));
    a->prompt = P_RESIZE;
    snprintf(a->pbuf, sizeof(a->pbuf), "%s", s);
    a->perr[0] = 0;
    a->pcard_hover = a->pcard_down = -1;
    wlpicker_reset(&a->pk);
    wltext_select_all(a->pbuf);
    a->win.redraw = true;
}

/* ---- the prompts (cards) -------------------------------------------------------------------------------------------------------- */

static void prompt_open(struct app *a, enum prompt p, const char *initial)
{
    finish_pending(a);
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
    "Tools  S select (O ellipse, Q lasso, W wand)  M move  P pen  A airbrush  E eraser",
    "       L line  B box  C circle  N polygon / star  F fill  G gradient  T text  I picker",
    "Left button draws with the pen colour, the right button with the second one; X swaps them.",
    "The squares at the left of the strip open the colour picker; the small ones are the colours used lately.",
    "The sliders in the strip: Size, Opacity, Hardness ... ([ and ] change the size, the wheel steps a slider).",
    "Shift keeps a line at 45 degrees and a box or circle square.",
    "Layers (the panel at the right, Tab hides it): you draw on the chosen one.",
    "  PageUp / PageDown choose a layer, Ctrl+Shift+N new, Ctrl+J duplicate, Ctrl+E merge down.",
    "  .ora keeps the layers, .png and .jpg get the flat picture.",
    "Ctrl+O open   Ctrl+N new   Ctrl+S save   Ctrl+Shift+S save as   Ctrl+Z undo   Ctrl+Y or Ctrl+Shift+Z redo",
    "Ctrl+A select all   Ctrl+D select none   Ctrl+I invert   Ctrl+C / X / V copy, cut, paste (a new layer)",
    "Delete clears the selection, Backspace fills it, Enter crops to it, Esc drops it.",
    "Text: click, type; Enter a new line, Ctrl+Enter or a click elsewhere puts it down, Esc cancels.",
    "Wheel zooms, middle button drags the picture, + - zoom, 0 fits, 1 real size, Ctrl+G a pixel grid.",
    "Transform (right rail): turn, flip, resize, canvas size (Ctrl+Alt+C), place a picture as a layer.",
    "Ctrl+U adjusts brightness, contrast, hue and saturation (also in the Effects menu).",
};
#define N_HELP ((int)(sizeof(HELP_LINES) / sizeof(HELP_LINES[0])))

static void prompt_accept(struct app *a);

static bool paint_name_ok(const char *name)
{
    const char *dot = strrchr(name, '.');
    return wlpicker_picture(name) || (dot && !strcasecmp(dot, ".ora"));
}
#define card_name_ok paint_name_ok

static bool list_prompt(const struct app *a) { return a->prompt == P_OPEN || a->prompt == P_SAVEAS || a->prompt == P_PLACE; }

/* How many rows the list has (0 = no list: another card, or a window too low for one). */
static int card_list_rows(const struct app *a)
{
    return list_prompt(a) ? wlpicker_fit(a->win.bh, 170) : 0;
}

/* The card's place and height (what wlcard_frame will make of it). */
static void card_rect(const struct app *a, struct wlcard *c, int *nb)
{
    int ch = 150, cw = 560;
    switch (a->prompt) {
    case P_HELP: ch = 56 + N_HELP * (LM_FONT_H + 4) + WLCARD_BTN_H; cw = 760; break;
    case P_DISCARD: case P_REPLACE: ch = 130; break;
    case P_NEW: ch = 178; break;
    case P_CANVAS: ch = 232; break;
    case P_ADJUST: ch = 56 + ADJ_N * 38 + 20 + WLCARD_BTN_H; cw = 500; break;
    case P_COLOUR: ch = CP_H; cw = CP_W; break;
    default: break;
    }
    int rows = card_list_rows(a);
    if (rows)
        ch = 170 + rows * WLPICKER_ROW_H;
    *nb = a->prompt == P_DISCARD || a->prompt == P_ADJUST ? 3 : a->prompt == P_HELP ? 1 : 2;
    c->w = LM_MIN(cw, a->win.bw - 16);
    c->h = LM_MIN(ch, a->win.bh - 16);
    c->x = (a->win.bw - c->w) / 2;
    c->y = (a->win.bh - c->h) / 2;
}

/* The text field's top, and the list's. */
static int card_field_y(const struct wlcard *c) { return c->y + 42 + LM_FONT_H + 8; }
static int card_list_y(const struct wlcard *c) { return card_field_y(c) + 30; }

/* The Save as card's row of formats: one chip each, above the name. */
static void fmt_chip_rect(const struct wlcard *c, int i, int *x, int *y, int *w, int *h)
{
    int inner = c->w - 2 * WLCARD_PAD, cw = (inner - (FMT_COUNT - 1) * 4) / FMT_COUNT;
    *x = c->x + WLCARD_PAD + i * (cw + 4);
    *y = c->y + 36;
    *w = cw;
    *h = 22;
}

/* A click on a chip writes that format's extension on the name. */
static bool fmt_chip_click(struct app *a, const struct wlcard *c, double px, double py)
{
    for (int i = 0; i < FMT_COUNT; i++) {
        int x, y, w, h;
        fmt_chip_rect(c, i, &x, &y, &w, &h);
        if (px >= x && px < x + w && py >= y && py < y + h) {
            set_ext(a->pbuf, sizeof(a->pbuf), SAVEFMTS[i].ext);
            wltext_end(a->pbuf);
            wlpicker_typed(&a->pk);
            a->perr[0] = 0;
            return true;
        }
    }
    return false;
}

/* The New card's background switch, and the Canvas card's anchor squares and room switch. */
static void card_toggle_rect(const struct wlcard *c, int *x, int *y, int *w, int *h)
{
    *x = c->x + WLCARD_PAD;
    *y = card_field_y(c) + 40;
    *w = 220;
    *h = 26;
}

#define ANCHOR_CELL 24
#define ANCHOR_PITCH 28

static void card_anchor_rect(const struct wlcard *c, int i, int *x, int *y)
{
    *x = c->x + WLCARD_PAD + (i % 3) * ANCHOR_PITCH;
    *y = card_field_y(c) + 44 + (i / 3) * ANCHOR_PITCH;
}

static void ask_open(struct app *a)
{
    char d[PATH_MAX];
    default_dir(a, d, sizeof(d));
    prompt_open(a, P_OPEN, d);
}

static void ask_place(struct app *a)
{
    char d[PATH_MAX];
    default_dir(a, d, sizeof(d));
    prompt_open(a, P_PLACE, d);
}

static void ask_saveas(struct app *a)
{
    char d[PATH_MAX];
    const char *def = a->doc.n > 1 ? "picture.ora" : "picture.png";
    if (a->path[0])
        snprintf(d, sizeof(d), "%s", a->path);
    else {
        default_dir(a, d, sizeof(d));
        strncat(d, def, sizeof(d) - strlen(d) - 1);
    }
    prompt_open(a, P_SAVEAS, d);
}

static void do_save(struct app *a)
{
    finish_pending(a);
    if (!a->path[0]) {
        ask_saveas(a);
        return;
    }
    write_image(a, a->path);
}

static void ask_canvas(struct app *a)
{
    char s[32];
    snprintf(s, sizeof(s), "%dx%d", CW(a), CH(a));
    prompt_open(a, P_CANVAS, s);
    wltext_select_all(a->pbuf);
    a->anchor_x = a->anchor_y = 1;
    a->canvas_fill_bg = false;
}

/* Something that would throw the picture away: ask once when it has unsaved changes. */
static void guard(struct app *a, enum after what)
{
    finish_pending(a);
    if (a->modified) {
        a->after = what;
        prompt_open(a, P_DISCARD, "");
        return;
    }
    switch (what) {
    case AF_OPEN: ask_open(a); break;
    case AF_NEW: {
        char s[32];
        snprintf(s, sizeof(s), "%dx%d", CW(a) > 0 ? CW(a) : 800, CH(a) > 0 ? CH(a) : 600);
        prompt_open(a, P_NEW, s);
        break;
    }
    case AF_QUIT: a->win.running = false; break;
    default: break;
    }
}

/* Puts a picture on a layer of its own, at the top left. */
static void place_picture(struct app *a, const char *path)
{
    char err[160];
    struct pix im = { 0 };
    if (!load_file(path, &im, err, sizeof(err))) {
        snprintf(a->perr, sizeof(a->perr), "%s", err);
        return;
    }
    if (a->doc.n >= MAX_LAYERS) {
        pix_free(&im);
        snprintf(a->perr, sizeof(a->perr), "No more than %d layers", MAX_LAYERS);
        return;
    }
    checkpoint_doc(a);
    const char *base = strrchr(path, '/');
    char nm[40];
    snprintf(nm, sizeof(nm), "%.36s", base ? base + 1 : path);
    if (!doc_add(&a->doc, nm, 0)) {
        pix_free(&im);
        snprintf(a->perr, sizeof(a->perr), "Not possible (out of memory?)");
        return;
    }
    for (int y = 0; y < im.h && y < CH(a); y++)
        memcpy(&IM(a).p[(size_t)y * (size_t)CW(a)], &im.p[(size_t)y * (size_t)im.w], (size_t)LM_MIN(im.w, CW(a)) * 4);
    pix_free(&im);
    changed(a);
    a->prompt = P_NONE;
    tool_set(a, T_MOVE);
    say(a, "Placed %s as a new layer: the Move tool puts it in place", nm);
}

/* The card goes away without doing what it asked (or, with `apply`, after it did it). */
static void prompt_close(struct app *a, bool apply)
{
    if (a->prompt == P_COLOUR && !apply) {
        if (a->cp.bg)
            a->bg = a->cp.orig;
        else
            a->fg = a->cp.orig;
    }
    if (a->prompt == P_ADJUST) {
        if (apply) {
            checkpoint_with(a, &a->adj_before);
            a->modified = true;
        } else if (a->adj_before.p && a->adj_before.w == IM(a).w && a->adj_before.h == IM(a).h) {
            memcpy(IM(a).p, a->adj_before.p, (size_t)IM(a).w * (size_t)IM(a).h * 4);
            flat_mark_all(a);
        }
        pix_free(&a->adj_before);
    }
    a->prompt = P_NONE;
    a->after = AF_NONE;
    a->win.redraw = true;
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
    case P_PLACE:
        if (!*s || s[strlen(s) - 1] == '/')
            return;
        place_picture(a, s);
        break;
    case P_SAVEAS: {
        if (!*s || s[strlen(s) - 1] == '/') {
            snprintf(a->perr, sizeof(a->perr), "Type a file name");
            return;
        }
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s", s);
        if (!has_known_ext(path) && strlen(path) + 5 < sizeof(path))
            strcat(path, a->doc.n > 1 ? ".ora" : ".png");
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
        new_doc(a, w, h, "", a->new_transparent);
        break;
    }
    case P_RESIZE: {
        int w, h;
        if (!parse_size(s, CW(a), CH(a), &w, &h)) {
            snprintf(a->perr, sizeof(a->perr), "Type a size (800x600), a width (800) or a percentage (50%%)");
            return;
        }
        if (w > MAX_SIDE || h > MAX_SIDE || (uint64_t)w * (uint64_t)h > LM_IMG_MAX_PIXELS) {
            snprintf(a->perr, sizeof(a->perr), "That size is not possible (at most %d a side)", MAX_SIDE);
            return;
        }
        checkpoint_doc(a);
        int sz[2] = { w, h };
        if (!all_layers(a, fn_resize, sz)) {                                   /* (the layers are what they were, or some of them are already resized: take it back) */
            do_undo(a, false);
            snprintf(a->perr, sizeof(a->perr), "That size is not possible (at most %d a side)", MAX_SIDE);
            return;
        }
        a->prompt = P_NONE;
        sel_clear(&a->sel);
        changed(a);
        if (a->fitted)
            view_fit(a);
        say(a, "Resized to %d x %d", CW(a), CH(a));
        break;
    }
    case P_CANVAS: {
        int w, h;
        if (!parse_size(s, CW(a), CH(a), &w, &h) || w > MAX_SIDE || h > MAX_SIDE || (uint64_t)w * (uint64_t)h > LM_IMG_MAX_PIXELS) {
            snprintf(a->perr, sizeof(a->perr), "Type a size (800x600), a width (800) or a percentage (150%%)");
            return;
        }
        checkpoint_doc(a);
        if (!doc_canvas(&a->doc, w, h, a->anchor_x, a->anchor_y, a->canvas_fill_bg ? a->bg : 0)) {
            do_undo(a, false);
            snprintf(a->perr, sizeof(a->perr), "Not possible (out of memory?)");
            return;
        }
        a->prompt = P_NONE;
        sel_clear(&a->sel);
        changed(a);
        if (a->fitted)
            view_fit(a);
        say(a, "The canvas is %d x %d", CW(a), CH(a));
        break;
    }
    case P_LAYERNAME:
        if (!*s) {
            snprintf(a->perr, sizeof(a->perr), "Type a name");
            return;
        }
        layer_rename(a, s);
        a->prompt = P_NONE;
        break;
    case P_COLOUR:
        push_recent(a, a->cp.bg ? a->bg : a->fg);
        prompt_close(a, true);
        break;
    case P_ADJUST:
        prompt_close(a, true);
        say(a, "Adjusted");
        break;
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

static uint32_t mix_rgb24(uint32_t a, uint32_t b, int t)                  /* t of 255 towards b */
{
    return argb(0, (ch_r(a) * (255 - t) + ch_r(b) * t) / 255, (ch_g(a) * (255 - t) + ch_g(b) * t) / 255, (ch_b(a) * (255 - t) + ch_b(b) * t) / 255) & 0xffffffu;
}

static void render_view(struct app *a)
{
    flat_refresh(a);
    int s = wlwin_scale(&a->win);
    int pw = cv_w(a) * s, ph = cv_h(a) * s;
    if (pw < 1 || ph < 1 || !a->flat.p)
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
    const struct pix *im = &a->flat;
    double zp = a->zoom * s, ox = a->vx * s, oy = a->vy * s;
    int n = zp >= 1.0 ? 1 : LM_MIN((int)ceil(1.0 / zp), 4);                 /* samples a side when the picture is smaller than the screen: a box average */
    uint32_t back = lm_theme.field & 0xffffffu;
    bool grid = a->grid && zp >= 6.0, marching = a->sel.on && a->sel.mask;
    uint8_t *sprev = marching ? calloc((size_t)pw, 1) : NULL, *scur = marching ? calloc((size_t)pw, 1) : NULL;
    for (int y = 0; y < ph; y++) {
        uint32_t *row = &a->vbuf[(size_t)y * (size_t)pw];
        double iy0 = (y - oy) / zp;
        bool row_in = !(iy0 + (n > 1 ? 1.0 / zp : 0) < 0 || iy0 >= im->h);
        for (int x = 0; x < pw; x++) {
            double ix0 = (x - ox) / zp;
            if (!row_in || ix0 < 0 || ix0 >= im->w || iy0 < 0) {
                row[x] = back;
                if (scur)
                    scur[x] = 0;
                continue;
            }
            int sa = 0, sr = 0, sg = 0, sb = 0, cnt = 0;
            for (int j = 0; j < n; j++)
                for (int i = 0; i < n; i++) {
                    int ix = (int)floor(ix0 + (n > 1 ? (i + 0.5) / (n * zp) - 0.5 / zp : 0));
                    int iy = (int)floor(iy0 + (n > 1 ? (j + 0.5) / (n * zp) - 0.5 / zp : 0));
                    ix = LM_CLAMP(ix, 0, im->w - 1);
                    iy = LM_CLAMP(iy, 0, im->h - 1);
                    uint32_t c = im->p[(size_t)iy * (size_t)im->w + (size_t)ix];
                    int al = ch_a(c);
                    sa += al;
                    sr += ch_r(c) * al;
                    sg += ch_g(c) * al;
                    sb += ch_b(c) * al;
                    cnt++;
                }
            int al = sa / cnt;
            uint32_t bgc = checker(x, y), out;
            if (al >= 255)
                out = argb(0, sr / sa, sg / sa, sb / sa) & 0xffffffu;
            else {
                int r = sa ? sr / sa : 0, g = sa ? sg / sa : 0, b = sa ? sb / sa : 0;
                out = argb(0, (r * al + ch_r(bgc) * (255 - al)) / 255, (g * al + ch_g(bgc) * (255 - al)) / 255, (b * al + ch_b(bgc) * (255 - al)) / 255) & 0xffffffu;
            }
            if (grid && (fmod(ix0, 1.0) * zp < 1.0 || fmod(iy0, 1.0) * zp < 1.0))
                out = mix_rgb24(out, 0x808080, 90);
            row[x] = out;
            if (scur)
                scur[x] = sel_at(&a->sel, (int)floor(ix0), (int)floor(iy0)) > 127;
        }
        if (scur) {                                                     /* the edge of the selection: dashes along every change from in to out */
            for (int x = 0; x < pw; x++) {
                bool edge = (x > 0 && scur[x] != scur[x - 1]) || (y > 0 && scur[x] != sprev[x]);
                if (edge)
                    row[x] = (((x + y) / (4 * s)) & 1) ? 0xffffffu : 0x000000u;
            }
            uint8_t *t = sprev;
            sprev = scur;
            scur = t;
        }
    }
    free(sprev);
    free(scur);
    a->vdirty = false;
}

/* A dashed rectangle in canvas coordinates. */
static void dashed_rect(struct app *a, double x0, double y0, double x1, double y1, uint32_t c1, uint32_t c2)
{
    int ax = cv_x(a) + (int)floor(x0), ay = (int)floor(y0), bx = cv_x(a) + (int)floor(x1), by = (int)floor(y1);
    for (int x = ax; x <= bx; x++) {
        uint32_t c = ((x - ax) / 4) & 1 ? c2 : c1;
        if (in_canvas(a, x, ay))
            wlwin_fill(&a->win, x, ay, 1, 1, c);
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

/* A ring of dots (the brush's size under the pointer), in window coordinates. */
static void ring(struct app *a, double cx, double cy, double r, uint32_t col)
{
    int n = LM_CLAMP((int)(r * 6.28), 12, 720);
    for (int i = 0; i < n; i++) {
        double t = 6.2831853 * i / n;
        int x = (int)floor(cx + r * cos(t)), y = (int)floor(cy + r * sin(t));
        if (in_canvas(a, x, y))
            wlwin_fill(&a->win, x, y, 1, 1, col);
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


static void ic_ellipse_sel(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    (void)bg;
    for (int i = 0; i < 12; i += 2) {                                   /* a dashed ellipse */
        double t = 6.2831853 * i / 12.0;
        int x0 = cx + (int)lround(8 * cos(t)), y0 = cy + (int)lround(6 * sin(t));
        int x1 = cx + (int)lround(8 * cos(t + 0.5)), y1 = cy + (int)lround(6 * sin(t + 0.5));
        wlrail_line(w, x0, y0, x1, y1, 2, c);
    }
}

static void ic_lasso(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    (void)bg;
    wlrail_line(w, cx - 7, cy - 2, cx - 3, cy - 7, 2, c);
    wlrail_line(w, cx - 3, cy - 7, cx + 5, cy - 6, 2, c);
    wlrail_line(w, cx + 5, cy - 6, cx + 8, cy, 2, c);
    wlrail_line(w, cx + 8, cy, cx + 1, cy + 3, 2, c);
    wlrail_line(w, cx + 1, cy + 3, cx - 5, cy + 2, 2, c);
    wlrail_line(w, cx - 5, cy + 2, cx - 6, cy + 8, 2, c);
}

static void ic_wand(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    (void)bg;
    wlrail_line(w, cx - 8, cy + 8, cx + 2, cy - 2, 3, c);
    wlwin_fill(w, cx + 4, cy - 9, 2, 6, c);                             /* a spark */
    wlwin_fill(w, cx + 2, cy - 7, 6, 2, c);
    wlwin_fill(w, cx + 7, cy + 1, 2, 2, c);
    wlwin_fill(w, cx - 3, cy - 8, 2, 2, c);
}

static void ic_move(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    (void)bg;
    wlwin_fill(w, cx - 8, cy - 1, 17, 2, c);
    wlwin_fill(w, cx - 1, cy - 8, 2, 17, c);
    wlrail_line(w, cx, cy - 8, cx - 3, cy - 5, 2, c);
    wlrail_line(w, cx, cy - 8, cx + 3, cy - 5, 2, c);
    wlrail_line(w, cx, cy + 8, cx - 3, cy + 5, 2, c);
    wlrail_line(w, cx, cy + 8, cx + 3, cy + 5, 2, c);
    wlrail_line(w, cx - 8, cy, cx - 5, cy - 3, 2, c);
    wlrail_line(w, cx - 8, cy, cx - 5, cy + 3, 2, c);
    wlrail_line(w, cx + 8, cy, cx + 5, cy - 3, 2, c);
    wlrail_line(w, cx + 8, cy, cx + 5, cy + 3, 2, c);
}

static void ic_spray(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    (void)bg;
    wlwin_fill(w, cx - 7, cy + 1, 7, 8, c);                             /* the can */
    wlwin_fill(w, cx - 5, cy - 2, 3, 3, c);
    static const int dots[][2] = { { 2, -6 }, { 5, -3 }, { 7, -7 }, { 4, 0 }, { 8, -1 }, { 1, -2 }, { 6, 2 }, { 3, -9 } };
    for (size_t i = 0; i < sizeof(dots) / sizeof(dots[0]); i++)
        wlwin_fill(w, cx + dots[i][0], cy + dots[i][1], 2, 2, c);
}

static void ic_gradient(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    for (int i = 0; i < 8; i++)                                         /* bars that fade to the right */
        wlwin_fill(w, cx - 8 + i * 2, cy - 7, 2, 14, mix_rgb24(c & 0xffffffu, bg & 0xffffffu, 32 * i));
    wlwin_fill(w, cx - 8, cy - 8, 16, 1, c);
    wlwin_fill(w, cx - 8, cy + 7, 16, 1, c);
}

static void ic_text(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    (void)bg;
    wlwin_fill(w, cx - 7, cy - 7, 15, 3, c);
    wlwin_fill(w, cx - 1, cy - 7, 3, 15, c);
    wlwin_fill(w, cx - 4, cy + 6, 9, 2, c);
}

static void ic_poly(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    (void)bg;
    for (int i = 0; i < 5; i++) {                                       /* a pentagon */
        double t0 = -1.5707963 + 6.2831853 * i / 5.0, t1 = -1.5707963 + 6.2831853 * (i + 1) / 5.0;
        wlrail_line(w, cx + (int)lround(8 * cos(t0)), cy + (int)lround(8 * sin(t0)), cx + (int)lround(8 * cos(t1)), cy + (int)lround(8 * sin(t1)), 2, c);
    }
}

typedef void (*icon_fn)(struct wlwin *, int, int, uint32_t, uint32_t);

/* The grouped rail buttons (Select, Shapes) show the variant in use with a small triangle in the corner: there is more behind them. */
static enum tool g_sel_var = T_SELECT, g_shape_var = T_LINE;
static icon_fn tool_icon(enum tool t);

static void group_mark(struct wlwin *w, int cx, int cy, uint32_t c)
{
    wlwin_fill(w, cx + 4, cy + 6, 5, 1, c);
    wlwin_fill(w, cx + 5, cy + 7, 3, 1, c);
    wlwin_fill(w, cx + 6, cy + 8, 1, 1, c);
}

static void ic_sel_group(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    tool_icon(g_sel_var)(w, cx - 2, cy - 2, c, bg);
    group_mark(w, cx, cy, c);
}

static void ic_shape_group(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    tool_icon(g_shape_var)(w, cx - 2, cy - 2, c, bg);
    group_mark(w, cx, cy, c);
}

static const int LEFT_IDS[N_LEFT] = { B_SELECT, B_MOVE, B_PEN, B_SPRAY, B_ERASER, B_SHAPES, B_FILL, B_GRADIENT, B_TEXT, B_PICK };
static const int RIGHT_IDS[N_RIGHT] = { B_NEW, B_OPEN, B_SAVE, B_UNDO, B_REDO, B_TRANSFORM, B_FX, B_CROP, B_FIT, B_HELP };


static icon_fn tool_icon(enum tool t)
{
    switch (t) {
    case T_SELECT: return ic_select;
    case T_ELLIPSE_SEL: return ic_ellipse_sel;
    case T_LASSO: return ic_lasso;
    case T_WAND: return ic_wand;
    case T_MOVE: return ic_move;
    case T_PEN: return ic_pen;
    case T_SPRAY: return ic_spray;
    case T_ERASER: return ic_eraser;
    case T_LINE: return ic_line;
    case T_BOX: return ic_box;
    case T_CIRCLE: return ic_circle;
    case T_POLY: return ic_poly;
    case T_FILL: return ic_fill;
    case T_GRADIENT: return ic_gradient;
    case T_TEXT: return ic_text;
    default: return ic_pick;
    }
}

static void rails(const struct app *a, struct wlrail *L, struct wlrail *R)
{
    static char tipbuf[B_COUNT][64];
    static const char *const tips[B_COUNT] = {
        NULL, "Move  M", "Pen  P", "Airbrush  A", "Eraser  E", NULL, "Fill  F", "Gradient  G", "Text  T", "Picker  I",
        "New  Ctrl+N", "Open  Ctrl+O", "Save  Ctrl+S", "Undo  Ctrl+Z", "Redo  Ctrl+Y", "Turn, flip, size, canvas", "Effects", "Crop to the selection  Enter", "Fit the window  0", "Keys  F1" };
    static void (*const icons[B_COUNT])(struct wlwin *, int, int, uint32_t, uint32_t) = {
        NULL, ic_move, ic_pen, ic_spray, ic_eraser, NULL, ic_fill, ic_gradient, ic_text, ic_pick,
        ic_new, wlrail_ic_open, wlrail_ic_save, ic_undo, ic_redo, ic_rot_r, ic_fx, ic_crop, ic_fit, NULL };
    const uint32_t base[B_COUNT] = {
        lm_theme.btn_slate, lm_theme.btn_teal, lm_theme.btn_blue, lm_theme.btn_sky, lm_theme.btn_rust, lm_theme.btn_purple, lm_theme.btn_orange, lm_theme.btn_olive, lm_theme.btn_lime, lm_theme.btn_amber,
        lm_theme.btn_green, lm_theme.btn_blue, lm_theme.btn_amber, lm_theme.btn_slate, lm_theme.btn_slate, lm_theme.btn_teal, lm_theme.btn_orange, lm_theme.btn_rust, lm_theme.btn_grey, lm_theme.btn_rose };
    const uint32_t lite[B_COUNT] = {
        lm_theme.btn_slate_lit, lm_theme.btn_teal_lit, lm_theme.btn_blue_lit, lm_theme.btn_sky_lit, lm_theme.btn_rust_lit, lm_theme.btn_purple_lit, lm_theme.btn_orange_lit, lm_theme.btn_olive_lit,
        lm_theme.btn_lime_lit, lm_theme.btn_amber_lit, lm_theme.btn_green_lit, lm_theme.btn_blue_lit, lm_theme.btn_amber_lit, lm_theme.btn_slate_lit, lm_theme.btn_slate_lit,
        lm_theme.btn_teal_lit, lm_theme.btn_orange_lit, lm_theme.btn_rust_lit, lm_theme.btn_grey_lit, lm_theme.btn_rose_lit };
    memset(L, 0, sizeof(*L));
    memset(R, 0, sizeof(*R));
    L->x = 0;
    R->x = a->win.bw > 0 ? a->win.bw - WLRAIL_W : 0;
    L->y0 = R->y0 = 0;
    L->y1 = R->y1 = a->win.bh > 0 ? a->win.bh - PAL_H - STAT_H : 400;
    L->n = N_LEFT;
    for (int i = 0; i < N_LEFT; i++) {
        int id = LEFT_IDS[i];
        const char *tip = tips[id];
        icon_fn ic = icons[id];
        if (id == B_SELECT || id == B_SHAPES) {
            enum tool v = id == B_SELECT ? a->grp_sel : a->grp_shape;
            snprintf(tipbuf[id], sizeof(tipbuf[id]), "%s  %c  (click again or right click: more)", TOOL_TITLES[v], toupper((unsigned char)TOOL_KEYS[v]));
            tip = tipbuf[id];
            g_sel_var = a->grp_sel;
            g_shape_var = a->grp_shape;
            ic = id == B_SELECT ? ic_sel_group : ic_shape_group;
            (void)v;
        }
        L->it[i] = (struct wlrail_item){ .tip = tip, .icon = ic, .base = base[id], .lite = lite[id], .show = true, .lit = tool_button(a->tool) == id };
    }
    R->n = N_RIGHT;
    for (int j = 0; j < N_RIGHT; j++) {
        int id = RIGHT_IDS[j];
        bool idle = (id == B_UNDO && !a->nundo) || (id == B_REDO && !a->nredo) || (id == B_CROP && !a->sel.on);
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
        for (int i = 0; i < N_LEFT; i++)
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

/* ---- the strip: colours and the tool's sliders ------------------------------------------------------------------------------------- */

struct strip_g {
    int fgx, fgy, fgs, bgx, bgy, bgs;           /* the two colour squares (the pen colour lies over the second one) */
    int swx, swy, sww, swh;                     /* the swap button */
    bool recent_on;
    int rx, ry, rs, rp;                         /* the colours used lately: the first square, its side, the pitch; two rows of eight */
    int nsl;
    struct { int id, x, y, w, h; } sl[3];       /* a slider: its whole box (the track is near the bottom of it) */
    int ntg;
    struct { int id, x, y, w, h; } tg[2];
};

#define SL_TRACK_DY 29
#define SL_KNOB 14

static void strip_layout(const struct app *a, struct strip_g *s)
{
    memset(s, 0, sizeof(*s));
    int y = pal_y(a);
    s->fgx = 10;
    s->fgy = y + 6;
    s->fgs = 26;
    s->bgx = 26;
    s->bgy = y + 18;
    s->bgs = 22;
    s->swx = 54;
    s->swy = y + 8;
    s->sww = 26;
    s->swh = 18;
    s->recent_on = a->win.bw >= 780;
    s->rs = 14;
    s->rp = 16;
    s->rx = 90;
    s->ry = y + 7;
    int x0 = s->recent_on ? s->rx + 8 * s->rp + 14 : 90;
    int ntg = 0, nsl = 0, tgw = 0;
    for (int i = 0; i < 2; i++)
        if (TOPT[a->tool].tg[i] >= 0) {
            int id = TOPT[a->tool].tg[i], w = LM_MAX(wlcard_text_w(TG_NAMES[id]) + 24, 56);
            s->tg[ntg].id = id;
            s->tg[ntg].w = w;
            s->tg[ntg].h = 24;
            s->tg[ntg].y = y + (PAL_H - 24) / 2;
            tgw += w + 6;
            ntg++;
        }
    for (int i = 0; i < 3; i++)
        if (TOPT[a->tool].sl[i] >= 0)
            s->sl[nsl++].id = TOPT[a->tool].sl[i];
    s->nsl = nsl;
    s->ntg = ntg;
    int avail = a->win.bw - x0 - 10 - tgw;
    int sw = nsl ? LM_CLAMP(avail / nsl - 14, 70, 190) : 0;
    int x = x0;
    for (int i = 0; i < nsl; i++) {
        s->sl[i].x = x;
        s->sl[i].y = y + 2;
        s->sl[i].w = sw;
        s->sl[i].h = PAL_H - 4;
        x += sw + 14;
    }
    int tx = LM_MAX(x, x0);
    for (int i = 0; i < ntg; i++) {
        s->tg[i].x = tx;
        tx += s->tg[i].w + 6;
    }
}

enum { SH_NONE, SH_FG, SH_BG, SH_SWAP, SH_RECENT, SH_SLIDER, SH_TOGGLE };

/* What is at a point of the strip: the kind and which one. */
static int strip_hit(const struct app *a, double x, double y, int *idx)
{
    struct strip_g s;
    strip_layout(a, &s);
    *idx = -1;
    if (y < pal_y(a) || y >= pal_y(a) + PAL_H)
        return SH_NONE;
    if (x >= s.fgx && x < s.fgx + s.fgs && y >= s.fgy && y < s.fgy + s.fgs)
        return SH_FG;
    if (x >= s.bgx && x < s.bgx + s.bgs && y >= s.bgy && y < s.bgy + s.bgs)
        return SH_BG;
    if (x >= s.swx && x < s.swx + s.sww && y >= s.swy && y < s.swy + s.swh)
        return SH_SWAP;
    if (s.recent_on)
        for (int i = 0; i < a->nrecent; i++) {
            int rx = s.rx + (i % 8) * s.rp, ry = s.ry + (i / 8) * s.rp;
            if (x >= rx && x < rx + s.rs && y >= ry && y < ry + s.rs) {
                *idx = i;
                return SH_RECENT;
            }
        }
    for (int i = 0; i < s.nsl; i++)
        if (x >= s.sl[i].x - 4 && x < s.sl[i].x + s.sl[i].w + 4 && y >= s.sl[i].y && y < s.sl[i].y + s.sl[i].h) {
            *idx = i;
            return SH_SLIDER;
        }
    for (int i = 0; i < s.ntg; i++)
        if (x >= s.tg[i].x && x < s.tg[i].x + s.tg[i].w && y >= s.tg[i].y && y < s.tg[i].y + s.tg[i].h) {
            *idx = i;
            return SH_TOGGLE;
        }
    return SH_NONE;
}

/* The value a click at `x` on a slider with its track from `tx` for `tw` pixels means. */
static int slider_value_at(int mn, int mx, double x, int tx, int tw)
{
    double t = LM_CLAMP((x - tx - SL_KNOB / 2.0) / (tw - SL_KNOB), 0.0, 1.0);
    return mn + (int)floor(t * (mx - mn) + 0.5);
}

/* A slider with a name and a value, in the box (x, y, w). */
static void paint_slider(struct app *a, int x, int y, int w, const char *name, int v, int mn, int mx, const char *unit, bool hot)
{
    struct wlwin *wn = &a->win;
    char val[32];
    snprintf(val, sizeof(val), "%d%s", v, unit);
    wlwin_text(wn, x, y + 1, name, 1, COL_TEXT, w - 40);
    wlwin_text(wn, x + w - wlcard_text_w(val), y + 1, val, 1, lm_theme.text_bright, 0);
    int ty = y + SL_TRACK_DY - 3, tx = x + SL_KNOB / 2, tw = w - SL_KNOB;
    double t = mx > mn ? (double)(v - mn) / (mx - mn) : 0.0;
    int kx = tx + (int)(t * tw);
    wlwin_round_fill(wn, x, ty, w, 6, 3, lm_theme.control);
    wlwin_round_fill(wn, x, ty, kx - x + 3, 6, 3, COL_ACCENT);
    wlwin_round_fill(wn, kx - SL_KNOB / 2 - 1, ty + 3 - SL_KNOB / 2 - 1, SL_KNOB + 2, SL_KNOB + 2, SL_KNOB / 2 + 1, hot ? COL_ACCENT : lm_theme.border);
    wlwin_round_fill(wn, kx - SL_KNOB / 2, ty + 3 - SL_KNOB / 2, SL_KNOB, SL_KNOB, SL_KNOB / 2, hot ? lm_theme.text_bright : COL_TEXT);
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
    if (ch_a(c) < 255) {                                                /* half see-through: the colour over the checkerboard, split down the middle */
        wlwin_round_fill(w, x, y, side, side, r, 0xcccccc);
        wlwin_fill(w, x + side / 2, y, side / 2 - r / 2, side / 2, 0x999999);
        wlwin_fill(w, x, y + side / 2, side / 2, side / 2 - r / 2, 0x999999);
        wlwin_round_fill(w, x + side / 2, y, side - side / 2, side, r / 2 + 1, c & 0xffffffu);
        wlwin_round_fill(w, x, y, side / 2 + 2, side, r / 2 + 1, mix_rgb24(0xaaaaaa, c & 0xffffffu, ch_a(c)));
        return;
    }
    wlwin_round_fill(w, x, y, side, side, r, c & 0xffffffu);
}

static void paint_strip(struct app *a)
{
    struct wlwin *w = &a->win;
    struct strip_g s;
    strip_layout(a, &s);
    wlwin_fill(w, 0, pal_y(a), w->bw, PAL_H, COL_BAR);
    wlwin_fill(w, 0, pal_y(a), w->bw, 1, lm_theme.border);
    wlwin_round_fill(w, s.bgx - 1, s.bgy - 1, s.bgs + 2, s.bgs + 2, 6, lm_theme.border);
    paint_swatch(a, s.bgx, s.bgy, s.bgs, a->bg, 5);                     /* the second colour lies behind the first */
    wlwin_round_fill(w, s.fgx - 1, s.fgy - 1, s.fgs + 2, s.fgs + 2, 8, lm_theme.border);
    paint_swatch(a, s.fgx, s.fgy, s.fgs, a->fg, 7);
    wlwin_drop(w, s.swx, s.swy, s.sww, s.swh, lm_button_radius(s.swh / 2), lm_theme.btn_slate);
    wlwin_text(w, s.swx + (s.sww - wlcard_text_w("<>")) / 2, s.swy + (s.swh - 2 - LM_FONT_H) / 2, "<>", 1, lm_theme.on_colour, 0);
    if (s.recent_on)
        for (int i = 0; i < N_RECENT; i++) {
            int x = s.rx + (i % 8) * s.rp, y = s.ry + (i / 8) * s.rp;
            if (i < a->nrecent) {
                if (a->recent[i] == a->fg || a->recent[i] == a->bg)
                    wlwin_round_fill(w, x - 2, y - 2, s.rs + 4, s.rs + 4, 5, a->recent[i] == a->fg ? COL_ACCENT : COL_DIM);
                paint_swatch(a, x, y, s.rs, a->recent[i], 3);
            } else
                wlwin_round_fill(w, x, y, s.rs, s.rs, 3, lm_theme.control);
        }
    for (int i = 0; i < s.nsl; i++) {
        int id = s.sl[i].id, mn, mx;
        slider_range(a, id, &mn, &mx);
        paint_slider(a, s.sl[i].x, s.sl[i].y, s.sl[i].w, SLIDERS[id].name, *slider_ptr(a, id), mn, mx, SLIDERS[id].unit, a->sl_drag == id);
    }
    for (int i = 0; i < s.ntg; i++) {
        bool on = a->tg[s.tg[i].id];
        wlwin_drop(w, s.tg[i].x, s.tg[i].y, s.tg[i].w, s.tg[i].h, lm_button_radius(s.tg[i].h / 2), on ? lm_theme.btn_green : lm_theme.btn_grey);
        const char *t = TG_NAMES[s.tg[i].id];
        wlwin_text(w, s.tg[i].x + (s.tg[i].w - wlcard_text_w(t)) / 2, s.tg[i].y + (s.tg[i].h - 2 - LM_FONT_H) / 2, t, 1, lm_theme.on_colour, 0);
    }
}

/* ---- the layers panel ------------------------------------------------------------------------------------------------------------ */

enum { LB_NEW, LB_DUP, LB_DEL, LB_UP, LB_DOWN, LB_MERGE, LB_COUNT };
static const char *const LB_NAMES[LB_COUNT] = { "New", "Copy", "Delete", "Up", "Down", "Merge" };
static const char *const LB_HINTS[LB_COUNT] = { "A new empty layer above this one (Ctrl+Shift+N)", "Duplicate the layer (Ctrl+J)", "Delete the layer",
                                                "Move the layer up the stack", "Move the layer down the stack", "Merge the layer into the one below (Ctrl+E)" };

struct lp_g {
    int x, y, w;
    int btn[LB_COUNT][4];
    int ox, oy, ow, oh;                         /* the opacity slider's box */
    int bx, by, bw, bh;                         /* the blend button */
    int ly, lh, nvis;                           /* the list */
};

static void lp_geom(const struct app *a, struct lp_g *g)
{
    memset(g, 0, sizeof(*g));
    g->x = panel_x(a);
    g->w = LP_W;
    int bw = (LP_W - 12 - 8) / 3;
    for (int i = 0; i < LB_COUNT; i++) {
        g->btn[i][0] = g->x + 6 + (i % 3) * (bw + 4);
        g->btn[i][1] = 28 + (i / 3) * 26;
        g->btn[i][2] = bw;
        g->btn[i][3] = 22;
    }
    g->ox = g->x + 10;
    g->oy = 84;
    g->ow = LP_W - 20;
    g->oh = 34;
    g->bx = g->x + 6;
    g->by = 120;
    g->bw = LP_W - 12;
    g->bh = 22;
    g->ly = 150;
    g->lh = LM_MAX(cv_h(a) - g->ly - 4, 0);
    g->nvis = g->lh / LP_ROW_H;
}

static void lay_show(struct app *a)
{
    if (!layers_on(a))
        return;
    struct lp_g g;
    lp_geom(a, &g);
    int row = a->doc.n - 1 - a->doc.cur, nv = LM_MAX(g.nvis, 1);
    if (row < a->lay_top)
        a->lay_top = row;
    if (row >= a->lay_top + nv)
        a->lay_top = row - nv + 1;
    a->lay_top = LM_CLAMP(a->lay_top, 0, LM_MAX(a->doc.n - nv, 0));
}

enum { LH_NONE, LH_BTN, LH_OPACITY, LH_BLEND, LH_ROW, LH_EYE };

/* What is at a point of the layers panel. */
static int lp_hit(const struct app *a, double x, double y, int *idx)
{
    *idx = -1;
    if (!layers_on(a) || x < panel_x(a) || x >= panel_x(a) + LP_W || y < 0 || y >= cv_h(a))
        return LH_NONE;
    struct lp_g g;
    lp_geom(a, &g);
    for (int i = 0; i < LB_COUNT; i++)
        if (x >= g.btn[i][0] && x < g.btn[i][0] + g.btn[i][2] && y >= g.btn[i][1] && y < g.btn[i][1] + g.btn[i][3]) {
            *idx = i;
            return LH_BTN;
        }
    if (x >= g.ox - 4 && x < g.ox + g.ow + 4 && y >= g.oy && y < g.oy + g.oh)
        return LH_OPACITY;
    if (x >= g.bx && x < g.bx + g.bw && y >= g.by && y < g.by + g.bh)
        return LH_BLEND;
    if (y >= g.ly && y < g.ly + g.nvis * LP_ROW_H) {
        int i = (int)((y - g.ly) / LP_ROW_H), layer = a->doc.n - 1 - (a->lay_top + i);
        if (layer < 0 || layer >= a->doc.n)
            return LH_NONE;
        *idx = layer;
        return x < g.x + 30 ? LH_EYE : LH_ROW;
    }
    return LH_NONE;
}

/* A small picture of a layer (physical pixels `tw` x `th`) over the checkerboard. */
static void layer_thumb(const struct layer *l, uint32_t *out, int tw, int th)
{
    for (int y = 0; y < th; y++)
        for (int x = 0; x < tw; x++) {
            int ix = l->im.w > 0 ? x * l->im.w / tw : 0, iy = l->im.h > 0 ? y * l->im.h / th : 0;
            uint32_t c = l->im.p ? l->im.p[(size_t)iy * (size_t)l->im.w + (size_t)ix] : 0, bgc = ((x / 6 + y / 6) & 1) ? 0x999999u : 0xcccccc;
            int al = ch_a(c);
            out[(size_t)y * (size_t)tw + (size_t)x] = argb(0, (ch_r(c) * al + ch_r(bgc) * (255 - al)) / 255, (ch_g(c) * al + ch_g(bgc) * (255 - al)) / 255,
                                                          (ch_b(c) * al + ch_b(bgc) * (255 - al)) / 255) & 0xffffffu;
        }
}

static void paint_layers(struct app *a)
{
    struct wlwin *w = &a->win;
    if (!layers_on(a))
        return;
    struct lp_g g;
    lp_geom(a, &g);
    int s = wlwin_scale(w);
    wlwin_fill(w, g.x, 0, LP_W, cv_h(a), COL_BAR);
    wlwin_fill(w, g.x, 0, 1, cv_h(a), lm_theme.border);
    wlwin_text_bold(w, g.x + 10, 6, "Layers", 1, lm_theme.text_bright, LP_W - 20);
    char info[32];
    snprintf(info, sizeof(info), "%d / %d", a->doc.n - a->doc.cur, a->doc.n);
    wlwin_text(w, g.x + LP_W - 10 - wlcard_text_w(info), 6, info, 1, COL_DIM, 0);
    static const uint32_t cols[LB_COUNT] = { 0, 1, 2, 3, 4, 5 };
    (void)cols;
    for (int i = 0; i < LB_COUNT; i++) {
        bool idle = (i == LB_DEL && a->doc.n <= 1) || (i == LB_UP && a->doc.cur >= a->doc.n - 1) || (i == LB_DOWN && a->doc.cur <= 0) || (i == LB_MERGE && a->doc.cur <= 0);
        uint32_t c = idle ? lm_theme.btn_grey : i == LB_NEW ? lm_theme.btn_green : i == LB_DEL ? lm_theme.btn_rust : i == LB_DUP ? lm_theme.btn_blue : lm_theme.btn_slate;
        wlwin_drop(w, g.btn[i][0], g.btn[i][1], g.btn[i][2], g.btn[i][3], lm_button_radius(g.btn[i][3] / 2), c);
        wlwin_text(w, g.btn[i][0] + (g.btn[i][2] - wlcard_text_w(LB_NAMES[i])) / 2, g.btn[i][1] + (g.btn[i][3] - 2 - LM_FONT_H) / 2, LB_NAMES[i], 1, lm_theme.on_colour, 0);
    }
    struct layer *cur = &a->doc.L[a->doc.cur];
    paint_slider(a, g.ox, g.oy, g.ow, "Opacity", cur->opacity, 0, 100, "%", a->sl_drag == 100);
    wlwin_drop(w, g.bx, g.by, g.bw, g.bh, lm_button_radius(g.bh / 2), lm_theme.btn_purple);
    char bl[48];
    snprintf(bl, sizeof(bl), "Blend: %s", BM_NAMES[cur->blend]);
    wlwin_text(w, g.bx + (g.bw - wlcard_text_w(bl)) / 2, g.by + (g.bh - 2 - LM_FONT_H) / 2, bl, 1, lm_theme.on_colour, 0);
    wlwin_fill(w, g.x + 6, g.ly - 4, LP_W - 12, 1, lm_theme.border);
    int tw = 44, th = 30;
    uint32_t *tb = malloc((size_t)tw * s * (size_t)th * s * 4);
    for (int i = 0; i < g.nvis; i++) {
        int layer = a->doc.n - 1 - (a->lay_top + i);
        if (layer < 0)
            break;
        const struct layer *l = &a->doc.L[layer];
        int y = g.ly + i * LP_ROW_H;
        bool is_cur = layer == a->doc.cur;
        if (is_cur)
            wlwin_round_fill(w, g.x + 4, y + 1, LP_W - 8, LP_ROW_H - 2, 8, lm_theme.control_hot);
        int ex = g.x + 10, ey = y + (LP_ROW_H - 16) / 2;
        wlwin_round_fill(w, ex, ey, 16, 16, 5, lm_theme.border);
        wlwin_round_fill(w, ex + 1, ey + 1, 14, 14, 4, COL_FIELD);
        if (l->visible)
            wlwin_round_fill(w, ex + 4, ey + 4, 8, 8, 3, is_cur ? lm_theme.text_bright : COL_TEXT);
        int tx = g.x + 32, ty = y + (LP_ROW_H - th) / 2;
        if (tb) {
            layer_thumb(l, tb, tw * s, th * s);
            wlwin_round_fill(w, tx - 1, ty - 1, tw + 2, th + 2, 2, lm_theme.border);
            wlwin_blit_px(w, tx, ty, tb, tw * s, th * s, tw * s, tx, ty, tw, th);
        }
        wlwin_text(w, g.x + 82, y + (LP_ROW_H - LM_FONT_H) / 2, l->name, 1, l->visible ? (is_cur ? lm_theme.text_bright : COL_TEXT) : COL_DIM, LP_W - 82 - 6);
    }
    free(tb);
    if (a->doc.n > g.nvis && g.nvis > 0) {                              /* a scroll bar */
        int th2 = LM_MAX(g.nvis * g.lh / a->doc.n, 16), ty2 = g.ly + (g.lh - th2) * a->lay_top / LM_MAX(a->doc.n - g.nvis, 1);
        wlwin_round_fill(w, g.x + LP_W - 5, ty2, 3, th2, 1, lm_theme.control);
    }
}

/* ---- the status line ------------------------------------------------------------------------------------------------------------- */

static const char *hover_hint(const struct app *a)
{
    if (!a->has_pointer)
        return NULL;
    int idx;
    int lh = lp_hit(a, a->px, a->py, &idx);
    if (lh == LH_BTN)
        return LB_HINTS[idx];
    if (lh == LH_OPACITY)
        return "The opacity of the layer: 0 hides it, 100 shows it as it is";
    if (lh == LH_BLEND)
        return "How the layer mixes with the ones below it";
    if (lh == LH_EYE)
        return "Show or hide the layer";
    if (lh == LH_ROW)
        return "Click to draw on this layer; double click renames it; right click for more";
    int k = strip_hit(a, a->px, a->py, &idx);
    switch (k) {
    case SH_FG: return "The pen colour: click to choose another (the left button draws with it)";
    case SH_BG: return "The second colour: click to choose another (the right button draws with it)";
    case SH_SWAP: return "Swap the two colours (X)";
    case SH_RECENT: return "A colour used lately: click = pen colour, right click = second colour";
    case SH_SLIDER: return "Drag, click or turn the wheel";
    default: return NULL;
    }
}

static void paint_status(struct app *a)
{
    struct wlwin *w = &a->win;
    wlwin_fill(w, 0, stat_y(a), w->bw, STAT_H, COL_BAR);
    char left[200], right[200];
    double ix, iy;
    to_pic(a, a->px, a->py, &ix, &iy);
    bool msg = a->msg[0] && now_ms() - a->msg_ms < 6000;
    const char *hint = hover_hint(a);
    const char *text = msg ? a->msg : hint ? hint : TOOL_HINTS[a->tool];
    snprintf(left, sizeof(left), "%s", text);
    if (a->has_pointer && in_canvas(a, a->px, a->py) && ix >= 0 && iy >= 0 && ix < CW(a) && iy < CH(a))
        snprintf(right, sizeof(right), "%d, %d     %d x %d     %d %%     layer %d / %d", (int)ix, (int)iy, CW(a), CH(a), (int)(a->zoom * 100.0 + 0.5), a->doc.cur + 1, a->doc.n);
    else
        snprintf(right, sizeof(right), "%d x %d     %d %%     layer %d / %d", CW(a), CH(a), (int)(a->zoom * 100.0 + 0.5), a->doc.cur + 1, a->doc.n);
    int rw = wlcard_text_w(right);
    wlwin_text(w, 8, stat_y(a) + (STAT_H - LM_FONT_H) / 2, left, 1, msg ? lm_theme.caution : COL_DIM, w->bw - rw - 32);
    wlwin_text(w, w->bw - rw - 10, stat_y(a) + (STAT_H - LM_FONT_H) / 2, right, 1, COL_TEXT, 0);
}

/* ---- the pointer's shapes: each tool has its own, drawn here (32 x 32, white with a black rim) and handed to the compositor ------------- */

enum cur_kind { CUR_ARROW, CUR_CROSS, CUR_DOT, CUR_PEN, CUR_AIRBRUSH, CUR_ERASER, CUR_FILL, CUR_PICK, CUR_TEXT, CUR_MOVE, CUR_LASSO, CUR_WAND, CUR_GRADIENT, CUR_LINE, CUR_BOX, CUR_CIRCLE, CUR_POLY,
                CUR_COUNT };
#define CUR_SIZE 32

static const int CUR_HOT[CUR_COUNT][2] = {
    [CUR_ARROW] = { 2, 2 },   [CUR_CROSS] = { 16, 16 }, [CUR_DOT] = { 16, 16 },  [CUR_PEN] = { 4, 28 },   [CUR_AIRBRUSH] = { 16, 16 }, [CUR_ERASER] = { 16, 16 }, [CUR_FILL] = { 5, 27 },
    [CUR_PICK] = { 5, 27 },   [CUR_TEXT] = { 16, 16 },  [CUR_MOVE] = { 16, 16 }, [CUR_LASSO] = { 16, 16 }, [CUR_WAND] = { 16, 16 },     [CUR_GRADIENT] = { 16, 16 }, [CUR_LINE] = { 16, 16 },
    [CUR_BOX] = { 16, 16 },   [CUR_CIRCLE] = { 16, 16 }, [CUR_POLY] = { 16, 16 },
};

/* A line with round ends, `t` thick, laid over the picture with colour `col`. */
static void cl_seg(uint32_t *img, double x0, double y0, double x1, double y1, double t, uint32_t col)
{
    double r = t / 2.0, dx = x1 - x0, dy = y1 - y0, len2 = dx * dx + dy * dy;
    for (int y = 0; y < CUR_SIZE; y++)
        for (int x = 0; x < CUR_SIZE; x++) {
            double px = x + 0.5 - x0, py = y + 0.5 - y0, u = len2 > 0 ? (px * dx + py * dy) / len2 : 0;
            u = u < 0 ? 0 : u > 1 ? 1 : u;
            double ex = px - u * dx, ey = py - u * dy, cov = r + 0.5 - sqrt(ex * ex + ey * ey);
            if (cov > 0)
                img[y * CUR_SIZE + x] = blend(img[y * CUR_SIZE + x], col, cov >= 1 ? 255 : (int)(cov * 255));
        }
}

static void cl_disc(uint32_t *img, double cx, double cy, double r, uint32_t col)
{
    cl_seg(img, cx, cy, cx, cy, r * 2.0, col);
}

static void cl_ring(uint32_t *img, double cx, double cy, double r, double t, uint32_t col)
{
    for (int i = 0; i < 16; i++) {
        double a0 = 6.2831853 * i / 16.0, a1 = 6.2831853 * (i + 1) / 16.0;
        cl_seg(img, cx + r * cos(a0), cy + r * sin(a0), cx + r * cos(a1), cy + r * sin(a1), t, col);
    }
}

/* One pass of a shape: `extra` makes every stroke wider (the black rim is the same shape, wider, under the white one). */
static void cur_strokes(int kind, uint32_t *img, uint32_t col, double extra)
{
#define L(x0, y0, x1, y1, t) cl_seg(img, x0, y0, x1, y1, (t) + extra, col)
    bool cross = kind == CUR_CROSS || kind == CUR_AIRBRUSH || kind == CUR_LASSO || kind == CUR_WAND || kind == CUR_GRADIENT || kind == CUR_LINE || kind == CUR_BOX || kind == CUR_CIRCLE ||
                 kind == CUR_POLY;
    if (cross) {
        L(16, 3, 16, 12, 1.4);
        L(16, 20, 16, 29, 1.4);
        L(3, 16, 12, 16, 1.4);
        L(20, 16, 29, 16, 1.4);
    }
    switch (kind) {
    case CUR_DOT:
        L(16, 9, 16, 13, 1.4);
        L(16, 19, 16, 23, 1.4);
        L(9, 16, 13, 16, 1.4);
        L(19, 16, 23, 16, 1.4);
        break;
    case CUR_PEN:
        L(10, 22, 25, 7, 4.5);
        L(5, 27, 9, 23, 1.6);
        L(26, 6, 28, 4, 3.0);
        break;
    case CUR_AIRBRUSH:
        cl_disc(img, 23, 8, 1.2 + extra / 2, col);
        cl_disc(img, 26, 12, 1.2 + extra / 2, col);
        cl_disc(img, 21, 24, 1.2 + extra / 2, col);
        cl_disc(img, 25, 22, 1.2 + extra / 2, col);
        break;
    case CUR_ERASER:
        L(8, 12, 24, 12, 1.6);
        L(24, 12, 24, 21, 1.6);
        L(24, 21, 8, 21, 1.6);
        L(8, 21, 8, 12, 1.6);
        L(8, 17, 24, 17, 1.2);
        break;
    case CUR_FILL:
        L(9, 16, 16, 9, 1.8);                                           /* a tipped bucket and its drop */
        L(16, 9, 25, 18, 1.8);
        L(25, 18, 18, 25, 1.8);
        L(18, 25, 9, 16, 1.8);
        L(12, 13, 20, 21, 1.4);
        cl_disc(img, 6, 26, 2.2 + extra / 2, col);
        break;
    case CUR_PICK:
        L(7, 25, 16, 16, 2.4);
        L(16, 16, 23, 9, 5.5);
        L(22, 6, 28, 12, 3.0);
        break;
    case CUR_TEXT:
        L(16, 6, 16, 26, 1.8);
        L(11, 6, 21, 6, 1.8);
        L(11, 26, 21, 26, 1.8);
        break;
    case CUR_MOVE:
        L(16, 3, 16, 29, 1.8);
        L(3, 16, 29, 16, 1.8);
        L(16, 3, 12, 7, 1.8);
        L(16, 3, 20, 7, 1.8);
        L(16, 29, 12, 25, 1.8);
        L(16, 29, 20, 25, 1.8);
        L(3, 16, 7, 12, 1.8);
        L(3, 16, 7, 20, 1.8);
        L(29, 16, 25, 12, 1.8);
        L(29, 16, 25, 20, 1.8);
        break;
    case CUR_LASSO:
        cl_ring(img, 24, 24, 4.0, 1.4 + extra, col);
        break;
    case CUR_WAND:
        L(24, 18, 24, 28, 1.4);
        L(19, 23, 29, 23, 1.4);
        break;
    case CUR_GRADIENT:
        L(20, 28, 29, 19, 2.2);
        break;
    case CUR_LINE:
        L(21, 28, 29, 20, 1.6);
        break;
    case CUR_BOX:
        L(20, 20, 29, 20, 1.4);
        L(29, 20, 29, 29, 1.4);
        L(29, 29, 20, 29, 1.4);
        L(20, 29, 20, 20, 1.4);
        break;
    case CUR_CIRCLE:
        cl_ring(img, 25, 25, 4.0, 1.4 + extra, col);
        break;
    case CUR_POLY:
        L(25, 19, 29, 29, 1.4);
        L(29, 29, 20, 29, 1.4);
        L(20, 29, 25, 19, 1.4);
        break;
    default: break;
    }
#undef L
}

static void cur_render(int kind, uint32_t *img)
{
    memset(img, 0, (size_t)CUR_SIZE * CUR_SIZE * 4);
    if (kind == CUR_ARROW) {
        static const double ax[7] = { 2, 2, 7, 11, 14, 10, 17 }, ay[7] = { 2, 22, 17, 26, 25, 16, 16 };
        for (int i = 0; i < 7; i++)
            cl_seg(img, ax[i], ay[i], ax[(i + 1) % 7], ay[(i + 1) % 7], 2.6, 0xff000000u);
        struct cov c;
        if (cov_alloc(&c, CUR_SIZE, CUR_SIZE)) {
            double ix[7], iy[7], mx = 7.0, my = 14.0;                   /* the white part is the arrow a little smaller */
            for (int i = 0; i < 7; i++) {
                ix[i] = mx + (ax[i] - mx) * 0.86 + 0.4;
                iy[i] = my + (ay[i] - my) * 0.86 + 0.6;
            }
            cov_fill_poly(&c, ix, iy, 7, NULL);
            for (int i = 0; i < CUR_SIZE * CUR_SIZE; i++)
                if (c.p[i])
                    img[i] = blend(img[i], 0xffffffffu, c.p[i]);
            cov_free(&c);
        }
        return;
    }
    cur_strokes(kind, img, 0xff000000u, 2.4);
    cur_strokes(kind, img, 0xffffffffu, 0.0);
}

/* The shape the pointer should have: the arrow over the window's own parts, the tool's over the picture. */
static int want_cursor(const struct app *a)
{
    if (!a->has_pointer || a->prompt != P_NONE)
        return CUR_ARROW;
    if (a->pan)
        return CUR_MOVE;
    if (!in_canvas(a, a->px, a->py) && !a->drag)
        return CUR_ARROW;
    if (a->hover >= 0 && !a->drag)
        return CUR_ARROW;
    switch (a->tool) {
    case T_SELECT: case T_ELLIPSE_SEL: return CUR_CROSS;
    case T_LASSO: return CUR_LASSO;
    case T_WAND: return CUR_WAND;
    case T_MOVE: return CUR_MOVE;
    case T_PEN: return CUR_PEN;
    case T_SPRAY: return CUR_AIRBRUSH;
    case T_ERASER: return CUR_ERASER;
    case T_LINE: return CUR_LINE;
    case T_BOX: return CUR_BOX;
    case T_CIRCLE: return CUR_CIRCLE;
    case T_POLY: return CUR_POLY;
    case T_FILL: return CUR_FILL;
    case T_GRADIENT: return CUR_GRADIENT;
    case T_TEXT: return CUR_TEXT;
    default: return CUR_PICK;
    }
}

static void cursor_update(struct app *a)
{
    if (!a->win.pointer)
        return;
    int k = want_cursor(a);
    if (k == a->cur_kind)
        return;
    uint32_t img[CUR_SIZE * CUR_SIZE];
    cur_render(k, img);
    wlwin_cursor(&a->win, img, CUR_SIZE, CUR_SIZE, CUR_HOT[k][0], CUR_HOT[k][1]);
    a->cur_kind = k;
}

/* ---- the cards ------------------------------------------------------------------------------------------------------------------ */

/* The colour card's three gauges, made as pictures at the buffer's own pixels. */
static void paint_cp_gauges(struct app *a, const struct cp_geo *g)
{
    struct wlwin *w = &a->win;
    int s = wlwin_scale(w);
    {
        int n = g->sq * s;
        uint32_t *b = malloc((size_t)n * (size_t)n * 4);
        if (b) {
            for (int y = 0; y < n; y++)
                for (int x = 0; x < n; x++) {
                    int r, gg, bb;
                    hsv_to_rgb(a->cp.h, (double)x / (n - 1), 1.0 - (double)y / (n - 1), &r, &gg, &bb);
                    b[(size_t)y * (size_t)n + (size_t)x] = argb(0, r, gg, bb) & 0xffffffu;
                }
            wlwin_round_fill(w, g->sqx - 1, g->sqy - 1, g->sq + 2, g->sq + 2, 3, lm_theme.border);
            wlwin_blit_px(w, g->sqx, g->sqy, b, n, n, n, g->sqx, g->sqy, g->sq, g->sq);
            free(b);
        }
    }
    {
        int bw = g->hw * s, bh = g->hh * s;
        uint32_t *b = malloc((size_t)bw * (size_t)bh * 4);
        if (b) {
            for (int y = 0; y < bh; y++) {
                int r, gg, bb;
                hsv_to_rgb(360.0 * y / bh, 1.0, 1.0, &r, &gg, &bb);
                for (int x = 0; x < bw; x++)
                    b[(size_t)y * (size_t)bw + (size_t)x] = argb(0, r, gg, bb) & 0xffffffu;
            }
            wlwin_round_fill(w, g->hx - 1, g->hy - 1, g->hw + 2, g->hh + 2, 3, lm_theme.border);
            wlwin_blit_px(w, g->hx, g->hy, b, bw, bh, bw, g->hx, g->hy, g->hw, g->hh);
            free(b);
        }
    }
    {
        int bw = g->aw * s, bh = g->ah * s;
        uint32_t *b = malloc((size_t)bw * (size_t)bh * 4);
        if (b) {
            int r, gg, bb;
            hsv_to_rgb(a->cp.h, a->cp.s, a->cp.v, &r, &gg, &bb);
            uint32_t col = argb(0, r, gg, bb) & 0xffffffu;
            for (int y = 0; y < bh; y++)
                for (int x = 0; x < bw; x++) {
                    uint32_t back = (((x / (6 * s)) + (y / (6 * s))) & 1) ? 0x999999u : 0xccccccu;
                    b[(size_t)y * (size_t)bw + (size_t)x] = mix_rgb24(back, col, x * 255 / (bw - 1));
                }
            wlwin_round_fill(w, g->ax - 1, g->ay - 1, g->aw + 2, g->ah + 2, 3, lm_theme.border);
            wlwin_blit_px(w, g->ax, g->ay, b, bw, bh, bw, g->ax, g->ay, g->aw, g->ah);
            free(b);
        }
    }
    uint32_t cur = cp_colour(&a->cp) | 0xff000000u;
    int mx = g->sqx + (int)(a->cp.s * (g->sq - 1)), my = g->sqy + (int)((1.0 - a->cp.v) * (g->sq - 1));
    wlwin_round_fill(w, mx - 7, my - 7, 14, 14, 7, 0xffffff);
    wlwin_round_fill(w, mx - 5, my - 5, 10, 10, 5, 0x000000);
    wlwin_round_fill(w, mx - 4, my - 4, 8, 8, 4, cur & 0xffffffu);
    int hy = g->hy + (int)(a->cp.h / 360.0 * (g->hh - 1));
    wlwin_fill(w, g->hx - 3, hy - 2, g->hw + 6, 5, 0x000000);
    wlwin_fill(w, g->hx - 2, hy - 1, g->hw + 4, 3, 0xffffff);
    int ax = g->ax + a->cp.alpha * (g->aw - 1) / 255;
    wlwin_fill(w, ax - 2, g->ay - 3, 5, g->ah + 6, 0x000000);
    wlwin_fill(w, ax - 1, g->ay - 2, 3, g->ah + 4, 0xffffff);
}

static void paint_card(struct app *a)
{
    struct wlwin *w = &a->win;
    struct wlcard c;
    char title[64] = "";
    const char *line = "", *line2 = "";
    const char *labels[3] = { "Cancel", "OK", "" };
    uint32_t cols[3] = { lm_theme.btn_grey, lm_theme.btn_blue, 0 };
    int nb = 2;
    bool field = true;
    switch (a->prompt) {
    case P_OPEN: snprintf(title, sizeof(title), "Open a picture"); line = "Pick a picture below or type its path (Tab completes); an .ora file brings its layers"; labels[1] = "Open"; break;
    case P_PLACE: snprintf(title, sizeof(title), "Place a picture as a layer"); line = "Pick a picture below or type its path: it becomes a new layer"; labels[1] = "Place"; cols[1] = lm_theme.btn_teal; break;
    case P_SAVEAS: snprintf(title, sizeof(title), "Save as"); line = ""; labels[1] = "Save"; cols[1] = lm_theme.btn_amber; break;
    case P_NEW: snprintf(title, sizeof(title), "New picture"); line = "The size in pixels, like 800x600"; labels[1] = "Create"; cols[1] = lm_theme.btn_green; break;
    case P_RESIZE: snprintf(title, sizeof(title), "Resize"); line = "A size (800x600), a width (800) or a percentage (50%): the picture is scaled"; labels[1] = "Resize"; cols[1] = lm_theme.btn_purple; break;
    case P_CANVAS: snprintf(title, sizeof(title), "Canvas size"); line = "The new size of the sheet (800x600, or 150%): the picture is not scaled"; labels[1] = "Change"; cols[1] = lm_theme.btn_purple; break;
    case P_LAYERNAME: snprintf(title, sizeof(title), "Name of the layer"); line = "Type a name"; labels[1] = "Rename"; break;
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
        break;
    case P_REPLACE:
        snprintf(title, sizeof(title), "Replace the file?");
        line = a->pbuf;
        line2 = "It exists already.";
        labels[1] = "Replace";
        cols[1] = lm_theme.btn_rust;
        field = false;
        break;
    case P_HELP:
        snprintf(title, sizeof(title), "Keys");
        labels[0] = "Close";
        cols[0] = lm_theme.btn_blue;
        nb = 1;
        field = false;
        break;
    case P_COLOUR:
        snprintf(title, sizeof(title), "%s", a->cp.bg ? "The second colour" : "The pen colour");
        labels[1] = "OK";
        field = false;
        break;
    case P_ADJUST:
        snprintf(title, sizeof(title), "Adjust the layer%s", a->sel.on ? " (the selection)" : "");
        labels[0] = "Cancel";
        labels[1] = "Reset";
        labels[2] = "Apply";
        cols[1] = lm_theme.btn_slate;
        cols[2] = lm_theme.btn_green;
        nb = 3;
        field = false;
        break;
    default: return;
    }
    struct wlcard geo;
    int nbg;
    card_rect(a, &geo, &nbg);
    wlcard_frame(w, &c, geo.w, geo.h, title, COL_ACCENT, COL_BG, lm_theme.text_bright);
    if (a->prompt == P_HELP) {
        for (int i = 0; i < N_HELP; i++)
            wlwin_text(w, c.x + WLCARD_PAD, c.y + 40 + i * (LM_FONT_H + 4), HELP_LINES[i], 1, COL_TEXT, c.w - 2 * WLCARD_PAD);
    } else if (a->prompt == P_COLOUR) {
        struct cp_geo g;
        cp_geom(a, &g);
        paint_cp_gauges(a, &g);
        uint32_t cur = cp_colour(&a->cp);
        wlwin_text(w, g.rx, g.swy - 2, "New", 1, COL_DIM, 0);
        paint_swatch(a, g.rx + 36, g.swy - 2, 30, cur, 6);
        wlwin_text(w, g.rx + 84, g.swy - 2, "Old", 1, COL_DIM, 0);
        paint_swatch(a, g.rx + 116, g.swy - 2, 30, a->cp.orig, 6);
        wlwin_text(w, g.fx, g.fy - 18, "Hex  (#rrggbb or #rrggbbaa)", 1, COL_DIM, 0);
        wlwin_round_fill(w, g.fx, g.fy, g.fw, g.fh, 8, COL_FIELD);
        wltext_paint(w, g.fx + 8, g.fy + 5, g.fw - 16, a->pbuf, true, COL_TEXT, lm_theme.button_hover, COL_ACCENT);
        char nums[80];
        snprintf(nums, sizeof(nums), "R %d   G %d   B %d   A %d", ch_r(cur), ch_g(cur), ch_b(cur), ch_a(cur));
        wlwin_text(w, g.fx, g.fy + g.fh + 8, nums, 1, COL_TEXT, c.x + c.w - g.fx - 8);
        wlwin_text(w, g.qx, g.qy - 20, "Quick colours", 1, COL_DIM, 0);
        for (int i = 0; i < N_QUICK; i++) {
            int x = g.qx + (i % 8) * g.qpitch, y = g.qy + (i / 8) * g.qpitch;
            if (QUICK[i] == cur)
                wlwin_round_fill(w, x - 2, y - 2, g.qside + 4, g.qside + 4, 5, COL_ACCENT);
            paint_swatch(a, x, y, g.qside, QUICK[i], 4);
        }
    } else if (a->prompt == P_ADJUST) {
        for (int i = 0; i < ADJ_N; i++) {
            int tx, ty, tw;
            adj_track(&c, i, &tx, &ty, &tw);
            int v = *adj_ptr(a, i), mn = ADJ_MIN[i], mx = ADJ_MAX[i];
            wlwin_text(w, c.x + WLCARD_PAD, ty - 7, ADJ_NAMES[i], 1, COL_TEXT, 100);
            double t = (double)(v - mn) / (mx - mn);
            int kx = tx + (int)(t * tw);
            wlwin_round_fill(w, tx, ty - 3, tw, 6, 3, lm_theme.control);
            wlwin_round_fill(w, tx, ty - 3, kx - tx, 6, 3, COL_ACCENT);
            wlwin_round_fill(w, kx - 8, ty - 8, 16, 16, 8, a->adj_drag == i ? lm_theme.text_bright : COL_TEXT);
            char val[16];
            snprintf(val, sizeof(val), "%d", v);
            wlwin_text(w, c.x + c.w - WLCARD_PAD - wlcard_text_w(val), ty - 7, val, 1, lm_theme.text_bright, 0);
        }
    } else {
        wlwin_text(w, c.x + WLCARD_PAD, c.y + 42, line, 1, field ? COL_DIM : COL_TEXT, c.w - 2 * WLCARD_PAD);
        if (*line2)
            wlwin_text(w, c.x + WLCARD_PAD, c.y + 42 + LM_FONT_H + 4, line2, 1, COL_DIM, c.w - 2 * WLCARD_PAD);
    }
    if (field) {
        int fy = card_field_y(&c);
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
        if (a->prompt == P_SAVEAS) {
            int cur = fmt_of(a->pbuf);
            for (int i = 0; i < FMT_COUNT; i++) {
                int x, y, cw, ch;
                fmt_chip_rect(&c, i, &x, &y, &cw, &ch);
                wlwin_drop(w, x, y, cw, ch, lm_button_radius(ch / 2), i == cur ? lm_theme.btn_blue : lm_theme.btn_grey);
                wlwin_text(w, x + (cw - wlcard_text_w(SAVEFMTS[i].label)) / 2, y + (ch - 2 - LM_FONT_H) / 2, SAVEFMTS[i].label, 1, lm_theme.on_colour, 0);
            }
            if (!a->perr[0])
                wlwin_text(w, c.x + WLCARD_PAD, c.y + c.h - WLCARD_PAD - WLCARD_BTN_H - 22, cur >= 0 ? SAVEFMTS[cur].note : "Click a format, or type the name with its extension", 1, COL_DIM,
                           c.w - 2 * WLCARD_PAD);
        }
        if (a->prompt == P_NEW) {
            int x, y, bw, bh;
            card_toggle_rect(&c, &x, &y, &bw, &bh);
            wlwin_drop(w, x, y, bw, bh, lm_button_radius(bh / 2), a->new_transparent ? lm_theme.btn_teal : lm_theme.btn_grey);
            const char *t = a->new_transparent ? "Paper: transparent" : "Paper: white";
            wlwin_text(w, x + (bw - wlcard_text_w(t)) / 2, y + (bh - 2 - LM_FONT_H) / 2, t, 1, lm_theme.on_colour, 0);
        } else if (a->prompt == P_CANVAS) {
            wlwin_text(w, c.x + WLCARD_PAD, card_field_y(&c) + 30, "The picture sits here:", 1, COL_DIM, 0);
            for (int i = 0; i < 9; i++) {
                int x, y;
                card_anchor_rect(&c, i, &x, &y);
                bool on = (i % 3) == a->anchor_x && (i / 3) == a->anchor_y;
                wlwin_round_fill(w, x, y + 12, ANCHOR_CELL, ANCHOR_CELL, 6, on ? COL_ACCENT : lm_theme.control);
            }
            int bx, by, bw, bh;
            bx = c.x + WLCARD_PAD + 3 * ANCHOR_PITCH + 16;
            by = card_field_y(&c) + 44 + 12;
            bw = 230;
            bh = 26;
            wlwin_drop(w, bx, by, bw, bh, lm_button_radius(bh / 2), a->canvas_fill_bg ? lm_theme.btn_amber : lm_theme.btn_grey);
            const char *t = a->canvas_fill_bg ? "New room: the second colour" : "New room: empty (the paper)";
            wlwin_text(w, bx + (bw - wlcard_text_w(t)) / 2, by + (bh - 2 - LM_FONT_H) / 2, t, 1, lm_theme.on_colour, 0);
        }
    } else if (a->perr[0] && a->prompt != P_HELP) {
        wlwin_text(w, c.x + WLCARD_PAD, c.y + c.h - WLCARD_PAD - WLCARD_BTN_H - LM_FONT_H - 6, a->perr, 1, lm_theme.error, c.w - 2 * WLCARD_PAD);
    }
    wlcard_buttons(w, &c, labels, nb, cols, a->pcard_hover, a->pcard_down, lm_theme.on_colour);
}

/* ---- the whole window ----------------------------------------------------------------------------------------------------------- */

static void write_dump(struct app *a);

static void paint(struct wlwin *w)
{
    struct app *a = w->data;
    wlwin_fill(w, 0, 0, w->bw, w->bh, COL_BG);
    if (a->fitted && CW(a) > 0 && fabs(fit_zoom(a) - a->zoom) > 1e-9) {
        a->zoom = fit_zoom(a);
        view_center(a);
    }
    if (a->lay_seen != a->doc.cur || a->lay_n != a->doc.n) {
        a->lay_seen = a->doc.cur;
        a->lay_n = a->doc.n;
        lay_show(a);
    }
    if (a->vdirty || !a->vbuf || a->vbw != cv_w(a) * wlwin_scale(w) || a->vbh != cv_h(a) * wlwin_scale(w) || !irect_empty(&a->fdirty))
        render_view(a);
    wlwin_fill(w, cv_x(a), 0, cv_w(a), cv_h(a), COL_FIELD);
    if (a->vbuf)
        wlwin_blit_px(w, cv_x(a), 0, a->vbuf, a->vbw, a->vbh, a->vbw, cv_x(a), 0, cv_w(a), cv_h(a));
    if (a->sel.on && !a->sel.mask) {
        double x0 = a->vx + a->sel.r.x0 * a->zoom, y0 = a->vy + a->sel.r.y0 * a->zoom, x1 = a->vx + a->sel.r.x1 * a->zoom - 1, y1 = a->vy + a->sel.r.y1 * a->zoom - 1;
        dashed_rect(a, x0, y0, x1, y1, 0xffffff, 0x000000);
    }
    if (a->txt.on) {                                                    /* the text's box and its caret */
        int cax, cay, lh, tw, th;
        text_layout(a, NULL, NULL, &cax, &cay, &lh, &tw, &th);
        double x0 = a->vx + a->txt.x * a->zoom, y0 = a->vy + a->txt.y * a->zoom;
        dashed_rect(a, x0 - 2, y0 - 2, x0 + tw * a->zoom + 2, y0 + th * a->zoom + 2, 0xffffff, 0x000000);
        int cx = cv_x(a) + (int)(x0 + cax * a->zoom), cy = (int)(y0 + cay * a->zoom), ch = (int)(lh * a->zoom);
        if (in_canvas(a, cx, cy))
            wlwin_fill(w, cx, cy, 2, LM_MAX(ch, 4), 0x000000);
        if (in_canvas(a, cx + 1, cy))
            wlwin_fill(w, cx + 1, cy, 1, LM_MAX(ch, 4), 0xffffff);
    }
    bool brush = a->tool == T_PEN || a->tool == T_ERASER || a->tool == T_SPRAY;
    if (brush && a->has_pointer && a->prompt == P_NONE && in_canvas(a, a->px, a->py) && a->hover < 0) {
        double r = LM_MAX(a->val[S_SIZE] / 2.0 * a->zoom, 1.0);
        ring(a, a->px, a->py, r + 1, 0x000000);
        ring(a, a->px, a->py, r, 0xffffff);
    }
    if (a->drag && a->tool == T_GRADIENT) {                             /* the line the gradient runs along */
        int x0 = cv_x(a) + (int)(a->vx + a->sx * a->zoom), y0 = (int)(a->vy + a->sy * a->zoom), x1 = cv_x(a) + (int)(a->vx + a->lx * a->zoom), y1 = (int)(a->vy + a->ly * a->zoom);
        wlrail_line(w, x0, y0, x1, y1, 3, 0x000000);
        wlrail_line(w, x0, y0, x1, y1, 1, 0xffffff);
    }
    struct wlrail L, R;
    rails(a, &L, &R);
    int64_t now = now_ms();
    wlrail_paint(w, &L, &a->ran[0], rail_hover_of(a, false), now, COL_BAR, COL_BAR, COL_ACCENT, lm_theme.text);
    wlrail_paint(w, &R, &a->ran[1], rail_hover_of(a, true), now, COL_BAR, COL_BAR, COL_ACCENT, lm_theme.text);
    paint_layers(a);
    paint_strip(a);
    paint_status(a);
    if (a->prompt != P_NONE)
        paint_card(a);
    cursor_update(a);
    write_dump(a);
}

static void write_dump(struct app *a)
{
    if (!g_dump)
        return;
    FILE *f = fopen(g_dump, "w");
    if (!f)
        return;
    flat_refresh(a);
    fprintf(f, "image w=%d h=%d hash=%016llx modified=%d path=%s layers=%d cur=%d paper=%08x\n", CW(a), CH(a), (unsigned long long)pix_hash(&a->flat), a->modified ? 1 : 0, a->path[0] ? a->path : "-", a->doc.n,
            a->doc.cur, a->doc.paper);
    fprintf(f, "state tool=%s fg=%08x bg=%08x size=%d filled=%d zoom=%d fitted=%d undo=%d redo=%d prompt=%d opacity=%d hardness=%d tolerance=%d corner=%d sides=%d density=%d tsize=%d grid=%d\n",
            TOOL_NAMES[a->tool], a->fg, a->bg, a->val[S_SIZE], a->tg[TG_FILLED] ? 1 : 0, (int)(a->zoom * 100.0 + 0.5), a->fitted ? 1 : 0, a->nundo, a->nredo, (int)a->prompt, a->val[S_OPACITY],
            a->val[S_HARD], a->val[S_TOL], a->val[S_CORNER], a->val[S_SIDES], a->val[S_DENSITY], a->tsize, a->grid ? 1 : 0);
    fprintf(f, "sel %s %d,%d,%d,%d mask=%d\n", a->sel.on ? "yes" : "no", a->sel.r.x0, a->sel.r.y0, a->sel.r.x1, a->sel.r.y1, a->sel.mask ? 1 : 0);
    fprintf(f, "canvas x=%d y=0 w=%d h=%d vx=%.2f vy=%.2f\n", cv_x(a), cv_w(a), cv_h(a), a->vx, a->vy);
    fprintf(f, "cursor kind=%d hotx=%d hoty=%d\n", a->cur_kind, a->cur_kind >= 0 ? CUR_HOT[a->cur_kind][0] : -1, a->cur_kind >= 0 ? CUR_HOT[a->cur_kind][1] : -1);
    fprintf(f, "text on=%d x=%d y=%d len=%d caret=%d\n", a->txt.on ? 1 : 0, a->txt.x, a->txt.y, a->txt.len, a->txt.caret);
    struct wlrail L, R;
    rails(a, &L, &R);
    int ys[WLRAIL_MAX];
    wlrail_layout(&L, ys);
    static const char *const lnames[N_LEFT] = { "Select", "Move", "Pen", "Airbrush", "Eraser", "Shapes", "Fill", "Gradient", "Text", "Picker" };
    for (int i = 0; i < L.n; i++)
        if (ys[i] >= 0)
            fprintf(f, "btn %s x=%d y=%d w=%d h=%d\n", lnames[i], L.x + (WLRAIL_W - WLRAIL_D) / 2, ys[i], WLRAIL_D, WLRAIL_D);
    wlrail_layout(&R, ys);
    static const char *const names[] = { "New", "Open", "Save", "Undo", "Redo", "Transform", "Effects", "Crop", "Fit", "Help" };
    for (int j = 0; j < R.n; j++)
        if (ys[j] >= 0)
            fprintf(f, "btn %s x=%d y=%d w=%d h=%d\n", names[j], R.x + (WLRAIL_W - WLRAIL_D) / 2, ys[j], WLRAIL_D, WLRAIL_D);
    struct strip_g s;
    strip_layout(a, &s);
    fprintf(f, "fgbox x=%d y=%d w=%d h=%d\n", s.fgx, s.fgy, s.fgs, s.fgs);
    fprintf(f, "bgbox x=%d y=%d w=%d h=%d\n", s.bgx, s.bgy, s.bgs, s.bgs);
    fprintf(f, "swap x=%d y=%d w=%d h=%d\n", s.swx, s.swy, s.sww, s.swh);
    fprintf(f, "recent n=%d x=%d y=%d side=%d pitch=%d on=%d", a->nrecent, s.rx, s.ry, s.rs, s.rp, s.recent_on ? 1 : 0);
    for (int i = 0; i < a->nrecent; i++)
        fprintf(f, " %08x", a->recent[i]);
    fputc('\n', f);
    for (int i = 0; i < s.nsl; i++) {
        int mn, mx;
        slider_range(a, s.sl[i].id, &mn, &mx);
        fprintf(f, "slider %s x=%d y=%d w=%d h=%d min=%d max=%d value=%d tx=%d tw=%d ty=%d\n", SLIDERS[s.sl[i].id].name, s.sl[i].x, s.sl[i].y, s.sl[i].w, s.sl[i].h, mn, mx, *slider_ptr(a, s.sl[i].id),
                s.sl[i].x + SL_KNOB / 2, s.sl[i].w - SL_KNOB, s.sl[i].y + SL_TRACK_DY);
    }
    for (int i = 0; i < s.ntg; i++)
        fprintf(f, "toggle %s x=%d y=%d w=%d h=%d on=%d\n", TG_NAMES[s.tg[i].id], s.tg[i].x, s.tg[i].y, s.tg[i].w, s.tg[i].h, a->tg[s.tg[i].id] ? 1 : 0);
    if (layers_on(a)) {
        struct lp_g g;
        lp_geom(a, &g);
        fprintf(f, "layers x=%d y=0 w=%d list_y=%d rows=%d top=%d\n", g.x, g.w, g.ly, g.nvis, a->lay_top);
        for (int i = 0; i < LB_COUNT; i++)
            fprintf(f, "lbtn %s x=%d y=%d w=%d h=%d\n", LB_NAMES[i], g.btn[i][0], g.btn[i][1], g.btn[i][2], g.btn[i][3]);
        fprintf(f, "lopacity x=%d y=%d w=%d h=%d tx=%d tw=%d ty=%d\n", g.ox, g.oy, g.ow, g.oh, g.ox + SL_KNOB / 2, g.ow - SL_KNOB, g.oy + SL_TRACK_DY);
        fprintf(f, "lblend x=%d y=%d w=%d h=%d\n", g.bx, g.by, g.bw, g.bh);
        for (int i = 0; i < g.nvis; i++) {
            int layer = a->doc.n - 1 - (a->lay_top + i);
            if (layer >= 0)
                fprintf(f, "lrow %d x=%d y=%d w=%d h=%d eye=%d,%d\n", layer, g.x, g.ly + i * LP_ROW_H, g.w, LP_ROW_H, g.x + 18, g.ly + i * LP_ROW_H + LP_ROW_H / 2);
        }
    }
    for (int i = 0; i < a->doc.n; i++) {
        struct pix lp = a->doc.L[i].im;
        fprintf(f, "layer %d name=%s opacity=%d visible=%d blend=%s hash=%016llx\n", i, a->doc.L[i].name, a->doc.L[i].opacity, a->doc.L[i].visible ? 1 : 0, BM_NAMES[a->doc.L[i].blend],
                (unsigned long long)pix_hash(&lp));
    }
    if (a->prompt == P_COLOUR) {
        struct cp_geo g;
        cp_geom(a, &g);
        fprintf(f, "colour h=%.1f s=%.3f v=%.3f a=%d hex=%s target=%s\n", a->cp.h, a->cp.s, a->cp.v, a->cp.alpha, a->pbuf, a->cp.bg ? "second" : "pen");
        fprintf(f, "cpsq x=%d y=%d w=%d h=%d\ncphue x=%d y=%d w=%d h=%d\ncpalpha x=%d y=%d w=%d h=%d\ncphex x=%d y=%d w=%d h=%d\n", g.sqx, g.sqy, g.sq, g.sq, g.hx, g.hy, g.hw, g.hh, g.ax, g.ay, g.aw,
                g.ah, g.fx, g.fy, g.fw, g.fh);
        fprintf(f, "cpquick x=%d y=%d side=%d pitch=%d\n", g.qx, g.qy, g.qside, g.qpitch);
    }
    if (a->prompt == P_ADJUST) {
        struct wlcard c;
        int nb;
        card_rect(a, &c, &nb);
        for (int i = 0; i < ADJ_N; i++) {
            int tx, ty, tw;
            adj_track(&c, i, &tx, &ty, &tw);
            fprintf(f, "adjust %s tx=%d ty=%d tw=%d min=%d max=%d value=%d\n", ADJ_NAMES[i], tx, ty, tw, ADJ_MIN[i], ADJ_MAX[i], *adj_ptr(a, i));
        }
    }
    if (a->prompt != P_NONE) {
        struct wlcard c;
        int nb;
        card_rect(a, &c, &nb);
        fprintf(f, "card x=%d y=%d w=%d h=%d field=%s\n", c.x, c.y, c.w, c.h, a->pbuf);
        for (int i = 0; i < nb; i++) {
            int x, y, bw, bh;
            wlcard_button_rect(&c, i, nb, &x, &y, &bw, &bh);
            fprintf(f, "cbtn %d x=%d y=%d w=%d h=%d\n", i, x, y, bw, bh);
        }
        if (a->prompt == P_SAVEAS)
            for (int i = 0; i < FMT_COUNT; i++) {
                int x, y, cw, ch;
                fmt_chip_rect(&c, i, &x, &y, &cw, &ch);
                fprintf(f, "fmt %s x=%d y=%d w=%d h=%d on=%d\n", SAVEFMTS[i].label, x, y, cw, ch, fmt_of(a->pbuf) == i ? 1 : 0);
            }
        if (a->prompt == P_NEW) {
            int x, y, bw, bh;
            card_toggle_rect(&c, &x, &y, &bw, &bh);
            fprintf(f, "cnew x=%d y=%d w=%d h=%d transparent=%d\n", x, y, bw, bh, a->new_transparent ? 1 : 0);
        }
        if (a->prompt == P_CANVAS) {
            for (int i = 0; i < 9; i++) {
                int x, y;
                card_anchor_rect(&c, i, &x, &y);
                fprintf(f, "canchor %d x=%d y=%d w=%d h=%d\n", i, x, y + 12, ANCHOR_CELL, ANCHOR_CELL);
            }
            fprintf(f, "canvascard anchor=%d,%d fill=%s\n", a->anchor_x, a->anchor_y, a->canvas_fill_bg ? "second" : "clear");
        }
    }
    if (card_list_rows(a)) {                                                    /* the Open / Save as card: where the list is and what it shows */
        struct wlcard c;
        int nb;
        char base[PATH_MAX];
        card_rect(a, &c, &nb);
        default_dir(a, base, sizeof(base));
        wlpicker_dump(f, &a->pk, base, a->pbuf, c.x + WLCARD_PAD, card_list_y(&c), c.w - 2 * WLCARD_PAD, card_list_rows(a), 0);
    }
    fclose(f);
}

/* ---- what the pointer does on the picture ---------------------------------------------------------------------------------------- */

static uint32_t rnd32(struct app *a)
{
    uint32_t x = a->rnd ? a->rnd : 2463534242u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    a->rnd = x;
    return x;
}

/* The start of a stroke or a shape: the undo step, the layer as it is (what every redraw starts from) and an empty coverage. */
static bool stroke_begin(struct app *a)
{
    checkpoint(a);
    a->have_before = pix_copy(&IM(a), &a->before);
    if (!a->have_before || !cov_alloc(&a->cov, CW(a), CH(a))) {
        if (a->have_before)
            pix_free(&a->before);
        a->have_before = false;
        cov_free(&a->cov);
        return false;
    }
    a->preview = (struct irect){ 0, 0, 0, 0 };
    return true;
}

static void stroke_end(struct app *a)
{
    if (a->have_before)
        pix_free(&a->before);
    a->have_before = false;
    cov_free(&a->cov);
}

/* Lays what `a->cov` covers inside `d` on the layer. */
static void stroke_commit(struct app *a, struct irect d, uint32_t col, bool erase)
{
    irect_clip(&d, CW(a), CH(a));
    if (irect_empty(&d))
        return;
    cov_commit(&IM(a), &a->before, &a->cov, d, col, a->val[S_OPACITY], erase, &a->sel);
    changed_r(a, d);
}

static void spray_at(struct app *a, double ix, double iy)
{
    if (!a->have_before)
        return;
    double r = a->val[S_SIZE] / 2.0;
    int n = LM_MAX((int)(a->val[S_DENSITY] / 100.0 * r * 3.0), 1);
    struct irect d = { 0, 0, 0, 0 };
    for (int i = 0; i < n; i++) {
        double ang = (rnd32(a) & 0xffff) / 65536.0 * 6.2831853, rad = sqrt((rnd32(a) & 0xffff) / 65536.0) * r;
        int x = (int)floor(ix + rad * cos(ang)), y = (int)floor(iy + rad * sin(ang));
        if (x < 0 || y < 0 || x >= CW(a) || y >= CH(a))
            continue;
        uint8_t *q = &a->cov.p[(size_t)y * (size_t)CW(a) + (size_t)x];
        *q = (uint8_t)LM_MIN(*q + 110, 255);
        irect_add(&d, x, y, x + 1, y + 1);
    }
    if (!irect_empty(&d))
        stroke_commit(a, d, a->drag_button == 273 ? a->bg : a->fg, false);
}

/* The corners of the box a shape is dragged out as; Shift makes a line run at 45 degrees and a box or ellipse square. */
static void shape_corners(const struct app *a, double ix, double iy, bool shift, int *x0, int *y0, int *x1, int *y1)
{
    *x0 = (int)floor(a->sx);
    *y0 = (int)floor(a->sy);
    *x1 = (int)floor(ix);
    *y1 = (int)floor(iy);
    if (!shift)
        return;
    if (a->tool == T_LINE) {
        int dx = *x1 - *x0, dy = *y1 - *y0, adx = abs(dx), ady = abs(dy);
        if (adx > 2 * ady)
            *y1 = *y0;
        else if (ady > 2 * adx)
            *x1 = *x0;
        else {
            int m = LM_MAX(adx, ady);
            *x1 = *x0 + (dx < 0 ? -m : m);
            *y1 = *y0 + (dy < 0 ? -m : m);
        }
    } else {
        int m = LM_MAX(abs(*x1 - *x0), abs(*y1 - *y0));
        *x1 = *x0 + (*x1 < *x0 ? -m : m);
        *y1 = *y0 + (*y1 < *y0 ? -m : m);
    }
}

static void drag_shape(struct app *a, double ix, double iy, bool shift)
{
    if (!a->have_before)
        return;
    int x0, y0, x1, y1;
    shape_corners(a, ix, iy, shift, &x0, &y0, &x1, &y1);
    struct irect old = a->preview, d = { 0, 0, 0, 0 };
    if (!irect_empty(&old))
        cov_clear(&a->cov, old);
    double r = a->val[S_SIZE] / 2.0;
    bool filled = a->tg[TG_FILLED];
    switch (a->tool) {
    case T_LINE:
        if (a->tg[TG_ARROW])
            cov_arrow(&a->cov, x0 + 0.5, y0 + 0.5, x1 + 0.5, y1 + 0.5, r, &d);
        else
            cov_stroke(&a->cov, x0 + 0.5, y0 + 0.5, x1 + 0.5, y1 + 0.5, r, 1.0, &d);
        break;
    case T_BOX: cov_rrect(&a->cov, x0, y0, x1, y1, a->val[S_CORNER], r, filled, &d); break;
    case T_CIRCLE: cov_ellipse(&a->cov, x0, y0, x1, y1, r, filled, &d); break;
    case T_POLY: cov_shape(&a->cov, x0, y0, x1, y1, a->val[S_SIDES], a->tg[TG_STAR], r, filled, &d); break;
    default: break;
    }
    struct irect all = old;
    if (!irect_empty(&d))
        irect_add(&all, d.x0, d.y0, d.x1, d.y1);
    a->preview = d;
    irect_clip(&a->preview, CW(a), CH(a));
    stroke_commit(a, all, draw_color(a), false);
}

static void drag_gradient(struct app *a, double ix, double iy)
{
    if (!a->have_before)
        return;
    uint32_t c0 = a->drag_button == 273 ? a->bg : a->fg, c1 = a->drag_button == 273 ? a->fg : a->bg;
    if (a->tg[TG_FADE])
        c1 = argb(0, ch_r(c0), ch_g(c0), ch_b(c0));
    struct irect r = a->sel.on ? a->sel.r : (struct irect){ 0, 0, CW(a), CH(a) };
    irect_clip(&r, CW(a), CH(a));
    gradient_commit(&IM(a), &a->before, r, a->sx, a->sy, ix, iy, a->tg[TG_RADIAL], c0, c1, a->val[S_OPACITY], &a->sel);
    changed_r(a, r);
}

/* The lasso's polygon and the selection it makes. */
static void lasso_update(struct app *a)
{
    if (a->nlasso < 3 || !a->cov.p)
        return;
    double minx = a->lx_pts[0], maxx = minx, miny = a->ly_pts[0], maxy = miny;
    for (int i = 1; i < a->nlasso; i++) {
        minx = LM_MIN(minx, a->lx_pts[i]);
        maxx = LM_MAX(maxx, a->lx_pts[i]);
        miny = LM_MIN(miny, a->ly_pts[i]);
        maxy = LM_MAX(maxy, a->ly_pts[i]);
    }
    struct irect box = { (int)floor(minx) - 1, (int)floor(miny) - 1, (int)ceil(maxx) + 2, (int)ceil(maxy) + 2 };
    irect_clip(&box, CW(a), CH(a));
    if (!irect_empty(&a->preview))
        cov_clear(&a->cov, a->preview);
    struct irect d = { 0, 0, 0, 0 };
    cov_fill_poly(&a->cov, a->lx_pts, a->ly_pts, a->nlasso, &d);
    a->preview = box;
    sel_set_cov(a, &a->cov, box);
}

static void lasso_add(struct app *a, double ix, double iy)
{
    if (a->nlasso > 0) {
        double dx = ix - a->lx_pts[a->nlasso - 1], dy = iy - a->ly_pts[a->nlasso - 1];
        if (dx * dx + dy * dy < 4.0)
            return;
    }
    if (a->nlasso >= 100000)
        return;
    if (a->nlasso >= a->caplasso) {
        int nc = a->caplasso ? a->caplasso * 2 : 512;
        double *nx = realloc(a->lx_pts, (size_t)nc * sizeof(double)), *ny = realloc(a->ly_pts, (size_t)nc * sizeof(double));
        if (nx)
            a->lx_pts = nx;
        if (ny)
            a->ly_pts = ny;
        if (!nx || !ny)
            return;
        a->caplasso = nc;
    }
    a->lx_pts[a->nlasso] = ix;
    a->ly_pts[a->nlasso] = iy;
    a->nlasso++;
}

/* The ellipse selection while it is dragged. */
static void ellipse_sel_update(struct app *a, double ix, double iy, bool shift)
{
    if (!a->cov.p)
        return;
    int x0, y0, x1, y1;
    shape_corners(a, ix, iy, shift, &x0, &y0, &x1, &y1);
    struct irect box = { LM_MIN(x0, x1) - 1, LM_MIN(y0, y1) - 1, LM_MAX(x0, x1) + 3, LM_MAX(y0, y1) + 3 };
    irect_clip(&box, CW(a), CH(a));
    struct irect old = a->preview;
    if (!irect_empty(&old))
        cov_clear(&a->cov, old);
    struct irect d = { 0, 0, 0, 0 };
    cov_ellipse(&a->cov, x0, y0, x1, y1, 1.0, true, &d);
    a->preview = box;
    irect_add(&old, box.x0, box.y0, box.x1, box.y1);
    sel_set_cov(a, &a->cov, old);
}

/* The Move tool: lifts what is selected (or takes the whole layer) and puts it down shifted. */
static void move_begin(struct app *a)
{
    checkpoint(a);
    memset(&a->mv_lift, 0, sizeof(a->mv_lift));
    if (!pix_copy(&IM(a), &a->mv_base))
        return;
    a->mv_lifted = a->sel.on;
    if (!a->sel.on)
        return;
    struct irect r = a->sel.r;
    irect_clip(&r, CW(a), CH(a));
    a->mv_sel0 = a->sel.r;
    if (irect_empty(&r) || !pix_alloc(&a->mv_lift, r.x1 - r.x0, r.y1 - r.y0, 0)) {
        a->mv_lifted = false;
        return;
    }
    for (int y = r.y0; y < r.y1; y++)
        for (int x = r.x0; x < r.x1; x++) {
            size_t i = (size_t)y * (size_t)CW(a) + (size_t)x;
            uint32_t c = a->mv_base.p[i];
            int m = sel_at(&a->sel, x, y);
            a->mv_lift.p[(size_t)(y - r.y0) * (size_t)a->mv_lift.w + (size_t)(x - r.x0)] = m >= 255 ? c : m <= 0 ? 0 : argb(ch_a(c) * m / 255, ch_r(c), ch_g(c), ch_b(c));
            a->mv_base.p[i] = m >= 255 ? 0 : m <= 0 ? c : blend(c, 0, m);
        }
}

static void move_to(struct app *a, int dx, int dy)
{
    if (!a->mv_base.p)
        return;
    int w = CW(a), h = CH(a);
    if (a->mv_lifted && a->mv_lift.p) {
        memcpy(IM(a).p, a->mv_base.p, (size_t)w * (size_t)h * 4);
        struct irect r = a->mv_sel0;
        irect_clip(&r, w, h);
        for (int y = 0; y < a->mv_lift.h; y++)
            for (int x = 0; x < a->mv_lift.w; x++) {
                uint32_t c = a->mv_lift.p[(size_t)y * (size_t)a->mv_lift.w + (size_t)x];
                int tx = r.x0 + x + dx, ty = r.y0 + y + dy;
                if (!ch_a(c) || tx < 0 || ty < 0 || tx >= w || ty >= h)
                    continue;
                uint32_t *q = &IM(a).p[(size_t)ty * (size_t)w + (size_t)tx];
                *q = blend(*q, c, 255);
            }
        a->sel.r = (struct irect){ a->mv_sel0.x0 + dx, a->mv_sel0.y0 + dy, a->mv_sel0.x1 + dx, a->mv_sel0.y1 + dy };
    } else {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int sx = x - dx, sy = y - dy;
                IM(a).p[(size_t)y * (size_t)w + (size_t)x] = sx < 0 || sy < 0 || sx >= w || sy >= h ? 0 : a->mv_base.p[(size_t)sy * (size_t)w + (size_t)sx];
            }
    }
    changed(a);
}

static void move_end(struct app *a)
{
    pix_free(&a->mv_base);
    pix_free(&a->mv_lift);
    a->mv_lifted = false;
}

/* The picture a Fill, Wand or Picker looks at: the chosen layer, or (switch "Image") what all the layers make. */
static const struct pix *sample_pix(struct app *a)
{
    if (a->tg[TG_IMAGE]) {
        flat_refresh(a);
        if (a->flat.p)
            return &a->flat;
    }
    return &IM(a);
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
    double r = a->val[S_SIZE] / 2.0;
    int px = (int)floor(ix), py = (int)floor(iy);
    switch (a->tool) {
    case T_SELECT:
        sel_clear(&a->sel);
        a->vdirty = true;
        break;
    case T_ELLIPSE_SEL:
        sel_clear(&a->sel);
        if (!cov_alloc(&a->cov, CW(a), CH(a)))
            a->drag = false;
        a->vdirty = true;
        break;
    case T_LASSO:
        sel_clear(&a->sel);
        a->nlasso = 0;
        if (!cov_alloc(&a->cov, CW(a), CH(a)))
            a->drag = false;
        else
            lasso_add(a, ix, iy);
        a->vdirty = true;
        break;
    case T_WAND: {
        struct cov c;
        struct irect d = { 0, 0, 0, 0 };
        const struct pix *src = sample_pix(a);
        a->drag = false;
        if (cov_alloc(&c, CW(a), CH(a))) {
            if (cov_flood(src, px, py, a->val[S_TOL] * 255 / 100, a->tg[TG_GLOBAL] || (a->win.mods & LM_MOD_SHIFT), &c, &d)) {
                sel_set_cov(a, &c, d);
                say(a, "Selected %d x %d", a->sel.r.x1 - a->sel.r.x0, a->sel.r.y1 - a->sel.r.y0);
            }
            cov_free(&c);
        }
        break;
    }
    case T_MOVE:
        move_begin(a);
        if (!a->mv_base.p)
            a->drag = false;
        break;
    case T_PEN:
    case T_ERASER:
        if (!stroke_begin(a)) {
            a->drag = false;
            break;
        }
        {
            struct irect d = { 0, 0, 0, 0 };
            cov_stroke(&a->cov, ix, iy, ix, iy, r, a->val[S_HARD] / 100.0, &d);
            stroke_commit(a, d, col, a->tool == T_ERASER);
        }
        break;
    case T_SPRAY:
        if (!stroke_begin(a)) {
            a->drag = false;
            break;
        }
        a->spray_ms = now_ms();
        spray_at(a, ix, iy);
        break;
    case T_LINE:
    case T_BOX:
    case T_CIRCLE:
    case T_POLY:
    case T_GRADIENT:
        if (!stroke_begin(a))
            a->drag = false;
        break;
    case T_FILL: {
        a->drag = false;
        struct cov c;
        struct irect d = { 0, 0, 0, 0 };
        const struct pix *src = sample_pix(a);
        if (px < 0 || py < 0 || px >= CW(a) || py >= CH(a) || !cov_alloc(&c, CW(a), CH(a)))
            break;
        if (cov_flood(src, px, py, a->val[S_TOL] * 255 / 100, a->tg[TG_GLOBAL], &c, &d)) {
            checkpoint(a);
            struct pix before;
            if (pix_copy(&IM(a), &before)) {
                cov_commit(&IM(a), &before, &c, d, col, a->val[S_OPACITY], false, &a->sel);
                pix_free(&before);
                push_recent(a, col);
                changed_r(a, d);
            }
        } else
            say(a, "Nothing to fill there");
        cov_free(&c);
        break;
    }
    case T_TEXT: {
        a->drag = false;
        if (a->txt.on) {
            int cax, cay, lh, tw, th;
            text_layout(a, NULL, NULL, &cax, &cay, &lh, &tw, &th);
            if (px >= a->txt.x - 2 && px < a->txt.x + tw + 2 && py >= a->txt.y - 2 && py < a->txt.y + th + 2)
                break;                                                  /* a click on the text itself: it stays open */
            text_commit(a);
        }
        text_begin(a, px, py);
        break;
    }
    case T_PICK:
        a->drag = false;
        if (px >= 0 && py >= 0 && px < CW(a) && py < CH(a)) {
            const struct pix *src = sample_pix(a);
            uint32_t c = src->p[(size_t)py * (size_t)src->w + (size_t)px];
            if (button == 273)
                a->bg = c;
            else
                a->fg = c;
            say(a, "Picked #%06x%s", c & 0xffffffu, ch_a(c) < 255 ? " (see-through)" : "");
        }
        break;
    default: break;
    }
    a->win.redraw = true;
}

static void drag_motion(struct app *a, double ix, double iy, bool shift)
{
    switch (a->tool) {
    case T_PEN:
    case T_ERASER:
        if (a->have_before) {
            struct irect d = { 0, 0, 0, 0 };
            cov_stroke(&a->cov, a->lx, a->ly, ix, iy, a->val[S_SIZE] / 2.0, a->val[S_HARD] / 100.0, &d);
            stroke_commit(a, d, draw_color(a), a->tool == T_ERASER);
        }
        a->lx = ix;
        a->ly = iy;
        break;
    case T_SPRAY:
        a->lx = ix;
        a->ly = iy;
        break;
    case T_LINE: case T_BOX: case T_CIRCLE: case T_POLY: drag_shape(a, ix, iy, shift); break;
    case T_GRADIENT:
        a->lx = ix;
        a->ly = iy;
        drag_gradient(a, ix, iy);
        break;
    case T_SELECT:
        a->sel.on = true;
        a->sel.r = (struct irect){ (int)floor(LM_MIN(a->sx, ix)), (int)floor(LM_MIN(a->sy, iy)), (int)floor(LM_MAX(a->sx, ix)) + 1, (int)floor(LM_MAX(a->sy, iy)) + 1 };
        irect_clip(&a->sel.r, CW(a), CH(a));
        a->vdirty = true;
        break;
    case T_ELLIPSE_SEL: ellipse_sel_update(a, ix, iy, shift); break;
    case T_LASSO:
        lasso_add(a, ix, iy);
        lasso_update(a);
        break;
    case T_MOVE: move_to(a, (int)lround(ix - a->sx), (int)lround(iy - a->sy)); break;
    default: break;
    }
}

static void end_drag(struct app *a)
{
    if (!a->drag)
        return;
    a->drag = false;
    if (a->tool == T_PEN || a->tool == T_ERASER || tool_in_shapes(a->tool) || a->tool == T_SPRAY || a->tool == T_GRADIENT) {
        if (a->tool != T_ERASER)
            push_recent(a, draw_color(a));
        stroke_end(a);
    } else if (a->tool == T_MOVE)
        move_end(a);
    else if (a->tool == T_LASSO || a->tool == T_ELLIPSE_SEL) {
        cov_free(&a->cov);
        a->preview = (struct irect){ 0, 0, 0, 0 };
        sel_clip(a);
        if (a->sel.on)
            say(a, "Selected %d x %d at %d, %d", a->sel.r.x1 - a->sel.r.x0, a->sel.r.y1 - a->sel.r.y0, a->sel.r.x0, a->sel.r.y0);
    } else if (a->tool == T_SELECT) {
        sel_clip(a);
        if (a->sel.on)
            say(a, "Selected %d x %d at %d, %d", a->sel.r.x1 - a->sel.r.x0, a->sel.r.y1 - a->sel.r.y0, a->sel.r.x0, a->sel.r.y0);
    }
    a->win.redraw = true;
}

static void press_button(struct app *a, int id)
{
    if (id != B_UNDO && id != B_REDO && id != B_FIT && id != B_HELP)
        finish_pending(a);
    switch (id) {
    case B_SELECT:
        if (tool_in_select(a->tool)) {
            if (!wlwin_menu(&a->win, "Rectangle  S\nEllipse  O\nLasso  Q\nMagic wand  W"))
                say(a, "The compositor has no menu service");
            else
                a->menu_kind = 3;
        } else
            tool_set(a, a->grp_sel);
        break;
    case B_SHAPES:
        if (tool_in_shapes(a->tool)) {
            if (!wlwin_menu(&a->win, "Line  L\nBox  B\nCircle  C\nPolygon / star  N"))
                say(a, "The compositor has no menu service");
            else
                a->menu_kind = 4;
        } else
            tool_set(a, a->grp_shape);
        break;
    case B_MOVE: tool_set(a, T_MOVE); break;
    case B_PEN: tool_set(a, T_PEN); break;
    case B_SPRAY: tool_set(a, T_SPRAY); break;
    case B_ERASER: tool_set(a, T_ERASER); break;
    case B_FILL: tool_set(a, T_FILL); break;
    case B_GRADIENT: tool_set(a, T_GRADIENT); break;
    case B_TEXT: tool_set(a, T_TEXT); break;
    case B_PICK: tool_set(a, T_PICK); break;
    case B_NEW: guard(a, AF_NEW); break;
    case B_OPEN: guard(a, AF_OPEN); break;
    case B_SAVE: do_save(a); break;
    case B_UNDO: do_undo(a, false); break;
    case B_REDO: do_undo(a, true); break;
    case B_TRANSFORM:
        if (wlwin_menu(&a->win, "Turn left\nTurn right\nFlip left-right\nFlip up-down\nResize...\nCanvas size...\nFlip layer left-right\nFlip layer up-down\nPlace picture as layer...\nPaper colour = the second colour"))
            a->menu_kind = 2;
        else
            say(a, "The compositor has no menu service");
        break;
    case B_CROP: do_crop(a); break;
    case B_FX: {
        char entries[512] = "";
        for (int i = 0; i < FX_COUNT; i++) {
            strncat(entries, FX_NAMES[i], sizeof(entries) - strlen(entries) - 1);
            strncat(entries, "\n", sizeof(entries) - strlen(entries) - 1);
        }
        strncat(entries, "Adjust...", sizeof(entries) - strlen(entries) - 1);
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

/* A change of the whole picture, every layer (turn, flip); undone at once when one of them fails. */
static void whole_transform(struct app *a, const char *what, bool (*fn)(struct pix *, void *), void *ud)
{
    finish_pending(a);
    checkpoint_doc(a);
    if (!all_layers(a, fn, ud)) {
        do_undo(a, false);
        say(a, "Not possible (out of memory?)");
        return;
    }
    whole_op(a, what, true);
}

static void do_transform(struct app *a, int what)
{
    bool yes = true, no = false;
    switch (what) {
    case 0: whole_transform(a, "Turned left", fn_rotate, &no); break;
    case 1: whole_transform(a, "Turned right", fn_rotate, &yes); break;
    case 2: whole_transform(a, "Flipped left-right", fn_flip, &yes); break;
    case 3: whole_transform(a, "Flipped up-down", fn_flip, &no); break;
    case 4: finish_pending(a); ask_resize(a); break;
    case 5: ask_canvas(a); break;
    case 6:
    case 7:
        finish_pending(a);
        checkpoint(a);
        flip(&IM(a), what == 6);
        changed(a);
        say(a, what == 6 ? "Layer flipped left-right" : "Layer flipped up-down");
        break;
    case 8: ask_place(a); break;
    case 9:
        finish_pending(a);
        checkpoint_props(a);
        a->doc.paper = a->bg;
        changed(a);
        say(a, "The paper (the colour of the sheet behind the layers) is the second colour now");
        break;
    default: break;
    }
}

/* ---- the self test ---------------------------------------------------------------------------------------------------------------- */

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
    CHECK(apply_fx(&im, FX_EMBOSS, all) && apply_fx(&im, FX_EDGES, all) && apply_fx(&im, FX_PIXELATE, all) && apply_fx(&im, FX_NOISE, all), "emboss, edges, pixelate and noise run");
    CHECK(ch_a(im.p[10]) == 255, "effects keep the alpha");
    struct pix flat;
    CHECK(pix_alloc(&flat, 6, 6, 0xff404040u) && apply_fx(&flat, FX_BLUR, (struct irect){ 0, 0, 6, 6 }) && flat.p[14] == 0xff404040u, "blurring a flat picture changes nothing");
    CHECK(apply_fx(&flat, FX_EDGES, (struct irect){ 0, 0, 6, 6 }) && flat.p[14] == 0xffffffffu, "a flat picture has no edges");
    pix_free(&flat);
    pix_free(&im);

    /* coverage: what the tools draw */
    struct cov cv;
    CHECK(cov_alloc(&cv, 12, 12), "cov alloc");
    struct irect d = { 0, 0, 0, 0 };
    cov_stroke(&cv, 0.5, 5.5, 11.5, 5.5, 0.5, 1.0, &d);
    CHECK(cv.p[5 * 12 + 4] == 255 && cv.p[4 * 12 + 4] == 0 && cv.p[6 * 12 + 4] == 0, "a one pixel line is one pixel wide");
    memset(cv.p, 0, 144);
    cov_stroke(&cv, 6.0, 6.0, 6.0, 6.0, 4.0, 0.0, &d);
    CHECK(cv.p[6 * 12 + 6] > 200 && cv.p[6 * 12 + 2] > 0 && cv.p[6 * 12 + 2] < cv.p[6 * 12 + 5], "a soft brush fades out from its middle");
    memset(cv.p, 0, 144);
    cov_rrect(&cv, 2, 2, 9, 9, 0, 1, true, &d);
    CHECK(cv.p[2 * 12 + 2] == 255 && cv.p[9 * 12 + 9] == 255 && cv.p[1 * 12 + 1] == 0 && cv.p[10 * 12 + 10] == 0, "a filled box covers its pixels and no more");
    memset(cv.p, 0, 144);
    cov_rrect(&cv, 0, 0, 11, 11, 5, 1, true, &d);
    CHECK(cv.p[0] < 100 && cv.p[6 * 12 + 6] == 255, "a rounded box has round corners");
    memset(cv.p, 0, 144);
    double tx[3] = { 1, 10, 1 }, ty[3] = { 1, 1, 10 };
    cov_fill_poly(&cv, tx, ty, 3, &d);
    int area = 0;
    for (int i = 0; i < 144; i++)
        area += cv.p[i];
    CHECK(area > 255 * 38 && area < 255 * 44, "a triangle of 9 x 9 covers about 40 pixels (%d)", area / 255);
    cov_free(&cv);

    CHECK(pix_alloc(&im, 10, 10, 0xffffffffu), "alloc 10x10");
    CHECK(cov_alloc(&cv, 10, 10), "cov alloc 10");
    d = (struct irect){ 0, 0, 0, 0 };
    CHECK(cov_flood(&im, 5, 5, 0, false, &cv, &d) && cv.p[0] == 255 && cv.p[99] == 255 && d.x1 == 10, "a flood covers an empty picture");
    struct pix before;
    CHECK(pix_copy(&im, &before), "copy before");
    struct sel nosel = { 0 };
    cov_commit(&im, &before, &cv, (struct irect){ 0, 0, 10, 10 }, 0xffff0000u, 100, false, &nosel);
    CHECK(im.p[0] == 0xffff0000u && im.p[99] == 0xffff0000u, "painted red");
    cov_commit(&im, &before, &cv, (struct irect){ 0, 0, 10, 10 }, 0xff0000ffu, 50, false, &nosel);
    CHECK(ch_r(im.p[0]) > 120 && ch_r(im.p[0]) < 135 && ch_g(im.p[0]) > 120 && ch_g(im.p[0]) < 135 && ch_b(im.p[0]) == 255, "half opacity mixes the colour with what was there (%08x)", im.p[0]);
    cov_commit(&im, &before, &cv, (struct irect){ 0, 0, 10, 10 }, 0, 50, true, &nosel);
    CHECK(ch_a(im.p[0]) > 120 && ch_a(im.p[0]) < 135, "erasing at half opacity takes half the alpha away (%08x)", im.p[0]);
    struct sel sl = { 0 };
    CHECK(sel_from_cov(&sl, &cv, (struct irect){ 0, 0, 10, 10 }) && sl.r.x1 == 10 && sl.mask && sel_at(&sl, 3, 3) == 255 && sel_at(&sl, 12, 3) == 0, "a selection from a mask");
    sel_clear(&sl);
    sl.on = true;
    sl.r = (struct irect){ 2, 2, 5, 5 };
    cov_commit(&im, &before, &cv, (struct irect){ 0, 0, 10, 10 }, 0xff00ff00u, 100, false, &sl);
    CHECK(im.p[3 * 10 + 3] == 0xff00ff00u && im.p[7 * 10 + 7] == 0xffffffffu, "a selection keeps the paint inside it");
    pix_free(&before);
    cov_free(&cv);
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

    /* colours */
    double h, s, v;
    int r, g, b;
    rgb_to_hsv(255, 128, 0, &h, &s, &v);
    hsv_to_rgb(h, s, v, &r, &g, &b);
    CHECK(r == 255 && g == 128 && b == 0 && h > 29 && h < 31, "hsv and back (%d %d %d, hue %.1f)", r, g, b, h);
    uint32_t pc;
    CHECK(cp_parse_hex("#ff8000", &pc) && pc == 0xffff8000u && cp_parse_hex("f80", &pc) && pc == 0xffff8800u && cp_parse_hex("#11223344", &pc) && pc == 0x44112233u && !cp_parse_hex("#12", &pc) &&
              !cp_parse_hex("#gg0000", &pc), "hex colours");
    char hx[16];
    cp_hex_text(0x44112233u, hx, sizeof(hx));
    CHECK(!strcmp(hx, "#11223344"), "a hex text with alpha (%s)", hx);
    struct adjust ad = { 0, 0, 0, 0 };
    CHECK(adjust_px(0xff336699u, &ad) == 0xff336699u, "no adjustment changes nothing");
    ad.brightness = 100;
    CHECK(ch_r(adjust_px(0xff336699u, &ad)) > 0x33 + 100, "brighter");
    ad = (struct adjust){ 0, 0, 0, -100 };
    uint32_t grey = adjust_px(0xff336699u, &ad);
    CHECK(ch_r(grey) == ch_g(grey) && ch_g(grey) == ch_b(grey), "no saturation is grey");

    /* the document */
    struct doc dc;
    CHECK(pix_alloc(&im, 4, 4, 0xffffffffu), "alloc for the document");
    doc_from_pix(&dc, &im);
    CHECK(dc.n == 1 && doc_w(&dc) == 4 && doc_add(&dc, "Top", 0) && dc.n == 2 && dc.cur == 1, "a document of two layers");
    dc.L[1].im.p[5] = 0xffff0000u;
    struct pix fl;
    CHECK(doc_flatten(&dc, &fl) && fl.p[5] == 0xffff0000u && fl.p[0] == 0xffffffffu, "the layers composite");
    pix_free(&fl);
    dc.L[1].opacity = 50;
    CHECK(doc_flatten(&dc, &fl) && ch_g(fl.p[5]) > 120 && ch_g(fl.p[5]) < 135 && ch_r(fl.p[5]) == 255, "a layer's opacity (%08x)", fl.p[5]);
    pix_free(&fl);
    dc.L[1].opacity = 100;
    dc.L[1].blend = BM_MULTIPLY;
    dc.L[0].im.p[5] = 0xff808080u;
    CHECK(doc_flatten(&dc, &fl) && ch_r(fl.p[5]) > 120 && ch_r(fl.p[5]) < 135 && ch_g(fl.p[5]) == 0, "multiply (%08x)", fl.p[5]);
    pix_free(&fl);
    dc.L[1].blend = BM_NORMAL;
    dc.L[1].visible = false;
    CHECK(doc_flatten(&dc, &fl) && fl.p[5] == 0xff808080u, "a hidden layer does not show");
    pix_free(&fl);
    dc.L[1].visible = true;
    CHECK(doc_dup(&dc) && dc.n == 3 && dc.cur == 2 && doc_move(&dc, -1) && dc.cur == 1 && doc_del(&dc) && dc.n == 2, "duplicate, move, delete");
    dc.cur = 1;
    CHECK(doc_merge_down(&dc) && dc.n == 1 && dc.L[0].im.p[5] == 0xffff0000u, "merge down");
    CHECK(doc_canvas(&dc, 6, 8, 1, 1, 0xff00ff00u) && doc_w(&dc) == 6 && doc_h(&dc) == 8 && dc.L[0].im.p[(size_t)(2 + 1) * 6 + 1 + 1] == 0xffff0000u && dc.L[0].im.p[0] == 0xff00ff00u,
          "the canvas grows round the picture (%08x)", dc.L[0].im.p[(size_t)3 * 6 + 2]);
    CHECK(doc_canvas(&dc, 2, 2, 0, 0, 0) && doc_w(&dc) == 2, "and shrinks");
    doc_free(&dc);
    CHECK(pix_alloc(&im, 4, 4, 0), "alloc for paper");
    doc_from_pix(&dc, &im);
    dc.paper = 0xffffffffu;
    dc.L[0].im.p[5] = 0xff000000u;
    CHECK(doc_flatten(&dc, &fl) && fl.p[0] == 0xffffffffu && fl.p[5] == 0xff000000u, "an empty layer shows the paper, a mark on it shows over the paper");
    pix_free(&fl);
    dc.L[0].blend = BM_MULTIPLY;
    CHECK(doc_flatten(&dc, &fl) && fl.p[0] == 0xffffffffu && fl.p[5] == 0xff000000u, "multiply over a white paper");
    pix_free(&fl);
    doc_free(&dc);

    /* OpenRaster */
    CHECK(pix_alloc(&im, 5, 3, 0xff112233u), "alloc for ora");
    doc_from_pix(&dc, &im);
    CHECK(doc_add(&dc, "Layer <&> two", 0), "second layer");
    dc.L[1].im.p[7] = 0x80ff8040u;
    dc.L[1].opacity = 70;
    dc.L[1].blend = BM_SCREEN;
    dc.L[0].visible = false;
    dc.paper = 0xffeeddccu;
    char opath[96];
    snprintf(opath, sizeof(opath), "/tmp/mellagui-paint-selftest-%ld.ora", (long)getpid());
    char err[160] = "";
    CHECK(write_ora(&dc, opath, err, sizeof(err)), "write ora: %s", err);
    struct doc back;
    memset(&back, 0, sizeof(back));
    CHECK(load_doc(opath, &back, err, sizeof(err)), "load ora: %s", err);
    CHECK(back.n == 2 && doc_w(&back) == 5 && doc_h(&back) == 3 && back.paper == 0xffeeddccu, "the layers and the paper come back");
    if (back.n == 2) {
        CHECK(!strcmp(back.L[1].name, "Layer <&> two") && back.L[1].opacity == 70 && back.L[1].blend == BM_SCREEN && back.L[1].visible && !back.L[0].visible, "names, opacity, blend and visibility");
        CHECK(!memcmp(back.L[0].im.p, dc.L[0].im.p, 60) && !memcmp(back.L[1].im.p, dc.L[1].im.p, 60), "the pixels of every layer");
    }
    doc_free(&back);
    unlink(opath);
    doc_free(&dc);

    /* undo of three kinds */
    static struct app A;
    memset(&A, 0, sizeof(A));
    CHECK(pix_alloc(&im, 4, 4, 0xffffffffu), "alloc for undo");
    doc_from_pix(&A.doc, &im);
    A.val[S_OPACITY] = 100;
    checkpoint(&A);
    IM(&A).p[3] = 0xff000000u;
    layer_add(&A);
    CHECK(A.doc.n == 2 && A.nundo == 2, "a layer is added");
    A.doc.L[1].im.p[1] = 0xffff0000u;
    checkpoint_props(&A);
    A.doc.L[1].opacity = 20;
    do_undo(&A, false);
    CHECK(A.doc.L[1].opacity == 100 && A.doc.n == 2, "undo brings the opacity back");
    do_undo(&A, false);
    CHECK(A.doc.n == 1 && A.doc.L[0].im.p[3] == 0xff000000u, "undo takes the layer away");
    do_undo(&A, false);
    CHECK(A.doc.L[0].im.p[3] == 0xffffffffu, "undo brings the pixel back");
    do_undo(&A, true);
    do_undo(&A, true);
    CHECK(A.doc.n == 2 && A.doc.L[0].im.p[3] == 0xff000000u, "redo does it all again");
    doc_free(&A.doc);
    drop_snaps(A.undo, &A.nundo);
    drop_snaps(A.redo, &A.nredo);
    pix_free(&A.flat);

    /* text */
    static struct app T;
    memset(&T, 0, sizeof(T));
    T.tsize = 24;
    snprintf(T.txt.buf, sizeof(T.txt.buf), "Hi\nyo");
    T.txt.len = 5;
    T.txt.caret = 5;
    struct cov tc;
    CHECK(cov_alloc(&tc, 200, 100), "text cov");
    int cax, cay, lh, tw, th;
    struct irect td = { 0, 0, 0, 0 };
    T.txt.x = 4;
    T.txt.y = 4;
    text_layout(&T, &tc, &td, &cax, &cay, &lh, &tw, &th);
    int ink = 0;
    for (int i = 0; i < 200 * 100; i++)
        ink += tc.p[i] != 0;
    CHECK(ink > 20 && cay == lh && cax > 0 && th == 2 * lh && tw > 4, "text is laid out (%d pixels, caret %d,%d, line %d, size %dx%d)", ink, cax, cay, lh, tw, th);
    cov_free(&tc);

    char path[64];
    CHECK(pix_alloc(&im, 37, 23, 0), "alloc for files");
    for (int y = 0; y < 23; y++)
        for (int x = 0; x < 37; x++)
            im.p[(size_t)y * 37 + (size_t)x] = argb(255, x * 6, y * 10, (x * y) & 255);
    snprintf(path, sizeof(path), "/tmp/mellagui-paint-selftest-%ld", (long)getpid());
    char png[96], jpg[96];
    snprintf(png, sizeof(png), "%s.png", path);
    snprintf(jpg, sizeof(jpg), "%s.jpg", path);
    CHECK(save_file(&im, png, err, sizeof(err)), "save png: %s", err);
    struct pix bk = { 0 };
    CHECK(load_file(png, &bk, err, sizeof(err)) && bk.w == 37 && bk.h == 23 && !memcmp(bk.p, im.p, (size_t)37 * 23 * 4), "an opaque PNG comes back as it was");
    pix_free(&bk);
    im.p[5] = 0x80ff8040u;
    im.p[6] = 0x00000000u;
    CHECK(save_file(&im, png, err, sizeof(err)), "save png with alpha");
    CHECK(load_file(png, &bk, err, sizeof(err)) && bk.w == 37 && ch_a(bk.p[5]) == 0x80 && ch_g(bk.p[5]) == 0x80 && ch_a(bk.p[6]) == 0, "a PNG with transparency keeps it");
    pix_free(&bk);
    CHECK(save_file(&im, jpg, err, sizeof(err)), "save jpg: %s", err);
    CHECK(load_file(jpg, &bk, err, sizeof(err)) && bk.w == 37 && bk.h == 23, "a JPEG opens");
    CHECK(ch_r(bk.p[6]) > 230 && ch_g(bk.p[6]) > 230 && ch_b(bk.p[6]) > 230, "a see-through pixel is white in a JPEG (%08x)", bk.p[6]);
    pix_free(&bk);
    unlink(png);
    unlink(jpg);
    pix_free(&im);

    /* every format we can write comes back as it was (the lossless ones exactly, a GIF with its colours cut down to 255) */
    CHECK(pix_alloc(&im, 23, 17, 0), "alloc for formats");
    for (int y = 0; y < 17; y++)
        for (int x = 0; x < 23; x++)
            im.p[(size_t)y * 23 + (size_t)x] = (x + y) % 7 == 0 ? 0x00000000u : argb(x % 3 == 0 ? 128 : 255, (x * 11) & 255, (y * 15) & 255, ((x ^ y) * 9) & 255);
    static const char *const exts[] = { "png", "bmp", "tga", "tif", "webp", "gif" };
    for (size_t e = 0; e < sizeof(exts) / sizeof(exts[0]); e++) {
        char fp[96];
        snprintf(fp, sizeof(fp), "%s-fmt.%s", path, exts[e]);
        CHECK(save_file(&im, fp, err, sizeof(err)), "save %s: %s", exts[e], err);
        struct pix rb = { 0 };
        bool ok = load_file(fp, &rb, err, sizeof(err)) && rb.w == 23 && rb.h == 17;
        CHECK(ok, "load %s: %s", exts[e], err);
        if (ok)
            for (size_t i = 0; i < 23 * 17; i++) {
                uint32_t a = im.p[i], b = rb.p[i];
                bool same = !strcmp(exts[e], "gif") ? (ch_a(a) < 128 ? ch_a(b) < 128 : (ch_a(b) >= 128 && abs(ch_r(a) - ch_r(b)) < 24 && abs(ch_g(a) - ch_g(b)) < 24 && abs(ch_b(a) - ch_b(b)) < 24)) : ch_a(a) == 0 ? ch_a(b) == 0 : a == b;
                if (!same) {
                    CHECK(0, "%s: pixel %zu is %08x, was %08x", exts[e], i, b, a);
                    break;
                }
            }
        pix_free(&rb);
        unlink(fp);
    }
    CHECK(fmt_of("/a.b/c") < 0 && fmt_of("x.WEBP") == FMT_WEBP && fmt_of("x.jpeg") == FMT_JPEG && fmt_of("x.tiff") == FMT_TIFF && fmt_of("/a.png/c") < 0, "extensions");
    {
        char nm[64] = "/tmp/pic.jpg";
        set_ext(nm, sizeof(nm), ".webp");
        char nm2[64] = "/tmp/pic";
        set_ext(nm2, sizeof(nm2), ".gif");
        char nm3[64] = "/tmp/a.b/pic";
        set_ext(nm3, sizeof(nm3), ".png");
        CHECK(!strcmp(nm, "/tmp/pic.webp") && !strcmp(nm2, "/tmp/pic.gif") && !strcmp(nm3, "/tmp/a.b/pic.png"), "an extension is replaced or added (%s %s %s)", nm, nm2, nm3);
    }
    pix_free(&im);

    CHECK(!apply_op(&im, "nonsense", err, sizeof(err)) && strstr(err, "unknown"), "an unknown operation is refused");
    if (g_bad)
        return 1;
    puts("mellagui-paint selftest ok");
    return 0;
}

static void menu_event(struct wlwin *w, int index)
{
    struct app *a = w->data;
    int kind = a->menu_kind;
    a->menu_kind = 0;
    if (kind == 1 && index >= 0 && index < FX_COUNT)
        do_effect(a, (enum fx)index);
    else if (kind == 1 && index == FX_COUNT)
        adj_open(a);
    else if (kind == 2 && index >= 0)
        do_transform(a, index);
    else if (kind == 3 && index >= 0 && index < 4) {
        static const enum tool t[4] = { T_SELECT, T_ELLIPSE_SEL, T_LASSO, T_WAND };
        tool_set(a, t[index]);
    } else if (kind == 4 && index >= 0 && index < 4) {
        static const enum tool t[4] = { T_LINE, T_BOX, T_CIRCLE, T_POLY };
        tool_set(a, t[index]);
    } else if (kind == 5 && index >= 0 && index < BM_COUNT) {
        checkpoint_props(a);
        a->doc.L[a->doc.cur].blend = index;
        changed(a);
    } else if (kind == 6 && index >= 0) {
        switch (index) {
        case 0: prompt_open(a, P_LAYERNAME, a->doc.L[a->doc.cur].name); wltext_select_all(a->pbuf); break;
        case 1: layer_dup(a); break;
        case 2: layer_delete(a); break;
        case 3: layer_merge(a); break;
        case 4: layer_toggle(a, a->doc.cur); break;
        default: break;
        }
    } else if (kind == 0 && index >= 0)                                    /* the answer of a text field's menu */
        wltext_menu_do(&a->clip, a->pbuf, sizeof(a->pbuf), index);
    w->redraw = true;
}

static void adj_set_from(struct app *a, int i, double x, const struct wlcard *c)
{
    int tx, ty, tw;
    adj_track(c, i, &tx, &ty, &tw);
    double t = LM_CLAMP((x - tx) / (double)tw, 0.0, 1.0);
    *adj_ptr(a, i) = ADJ_MIN[i] + (int)floor(t * (ADJ_MAX[i] - ADJ_MIN[i]) + 0.5);
    adj_apply(a);
    a->win.redraw = true;
}

static void card_pointer(struct app *a, uint32_t op, const union lm_arg *args)
{
    struct wlcard c;
    int nb;
    card_rect(a, &c, &nb);
    int crows = card_list_rows(a), fx = c.x + WLCARD_PAD, fw = c.w - 2 * WLCARD_PAD, ly = card_list_y(&c);
    bool field = a->prompt == P_OPEN || a->prompt == P_SAVEAS || a->prompt == P_NEW || a->prompt == P_RESIZE || a->prompt == P_CANVAS || a->prompt == P_LAYERNAME || a->prompt == P_PLACE;
    if (op == LM_WL_POINTER_EVT_MOTION) {
        int k = wlcard_button_at(&c, nb, (int)a->px, (int)a->py);
        if (k != a->pcard_hover)
            a->pcard_hover = k;
        if (crows) {
            char base[PATH_MAX];
            default_dir(a, base, sizeof(base));
            wlpicker_motion(&a->pk, base, a->pbuf, ly, crows, a->py);               /* (the scroll bar's thumb is dragged) */
        }
        if (a->prompt == P_COLOUR && a->cp.drag)
            cp_drag_to(a, a->px, a->py);
        if (a->prompt == P_ADJUST && a->adj_drag >= 0)
            adj_set_from(a, a->adj_drag, a->px, &c);
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
        a->cp.drag = 0;
        a->adj_drag = -1;
        if (a->pcard_down >= 0 && a->pcard_down == k) {                                        /* a button is done when it is let go on */
            int which = k;
            a->pcard_down = -1;
            if (a->prompt == P_HELP || which == 0)
                prompt_close(a, false);
            else if (a->prompt == P_DISCARD)
                discard_chosen(a, which == 2);
            else if (a->prompt == P_ADJUST && which == 1) {
                memset(&a->adj, 0, sizeof(a->adj));
                adj_apply(a);
            } else
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
            prompt_close(a, false);                                                             /* a click outside the card cancels */
        } else if (a->prompt == P_COLOUR) {
            struct cp_geo g;
            cp_geom(a, &g);
            double x = a->px, y = a->py;
            if (x >= g.sqx && x < g.sqx + g.sq && y >= g.sqy && y < g.sqy + g.sq)
                a->cp.drag = 1;
            else if (x >= g.hx - 2 && x < g.hx + g.hw + 2 && y >= g.hy && y < g.hy + g.hh)
                a->cp.drag = 2;
            else if (x >= g.ax && x < g.ax + g.aw && y >= g.ay - 3 && y < g.ay + g.ah + 3)
                a->cp.drag = 3;
            else if (x >= g.fx && x < g.fx + g.fw && y >= g.fy && y < g.fy + g.fh)
                wltext_click(a->pbuf, (int)x - (g.fx + 8), (a->win.mods & LM_MOD_SHIFT) != 0);
            else
                for (int i = 0; i < N_QUICK; i++) {
                    int qx = g.qx + (i % 8) * g.qpitch, qy = g.qy + (i / 8) * g.qpitch;
                    if (x >= qx && x < qx + g.qside && y >= qy && y < qy + g.qside) {
                        cp_set(a, QUICK[i], true);
                        break;
                    }
                }
            if (a->cp.drag)
                cp_drag_to(a, x, y);
        } else if (a->prompt == P_ADJUST) {
            for (int i = 0; i < ADJ_N; i++) {
                int tx, ty, tw;
                adj_track(&c, i, &tx, &ty, &tw);
                if (a->px >= tx - 10 && a->px < tx + tw + 10 && a->py >= ty - 12 && a->py < ty + 12) {
                    a->adj_drag = i;
                    adj_set_from(a, i, a->px, &c);
                }
            }
        } else if (field) {
            int fy = card_field_y(&c);
            if (a->prompt == P_SAVEAS && fmt_chip_click(a, &c, a->px, a->py))
                ;
            else if (a->py >= fy && a->py < fy + 26)
                wltext_click(a->pbuf, (int)a->px - (c.x + WLCARD_PAD + 8), (a->win.mods & LM_MOD_SHIFT) != 0);
            else if (crows) {
                char base[PATH_MAX];
                default_dir(a, base, sizeof(base));
                if (wlpicker_press(&a->pk, base, a->pbuf, sizeof(a->pbuf), fx, ly, fw, crows, a->px, a->py, args[1].u, (uint32_t)lm_metrics.double_click_ms) == WLPICKER_OPEN)
                    prompt_accept(a);
                a->perr[0] = 0;
            } else if (a->prompt == P_NEW) {
                int x, y, bw, bh;
                card_toggle_rect(&c, &x, &y, &bw, &bh);
                if (a->px >= x && a->px < x + bw && a->py >= y && a->py < y + bh)
                    a->new_transparent = !a->new_transparent;
            } else if (a->prompt == P_CANVAS) {
                for (int i = 0; i < 9; i++) {
                    int x, y;
                    card_anchor_rect(&c, i, &x, &y);
                    if (a->px >= x && a->px < x + ANCHOR_CELL && a->py >= y + 12 && a->py < y + 12 + ANCHOR_CELL) {
                        a->anchor_x = i % 3;
                        a->anchor_y = i / 3;
                    }
                }
                int bx = c.x + WLCARD_PAD + 3 * ANCHOR_PITCH + 16, by = card_field_y(&c) + 44 + 12;
                if (a->px >= bx && a->px < bx + 230 && a->py >= by && a->py < by + 26)
                    a->canvas_fill_bg = !a->canvas_fill_bg;
            }
        }
        a->win.redraw = true;
    } else if (args[2].u == 273 && field && a->py >= c.y + 42 + LM_FONT_H + 8 && a->py < c.y + 42 + LM_FONT_H + 34) {
        wlwin_menu(&a->win, wltext_menu(a->pbuf));
    }
}

/* A press in the layers panel. */
static void layers_press(struct app *a, uint32_t button)
{
    int idx;
    int hit = lp_hit(a, a->px, a->py, &idx);
    if (hit == LH_NONE)
        return;
    finish_pending(a);
    switch (hit) {
    case LH_BTN:
        if (button != 272)
            break;
        switch (idx) {
        case LB_NEW: layer_add(a); break;
        case LB_DUP: layer_dup(a); break;
        case LB_DEL: layer_delete(a); break;
        case LB_UP: layer_shift(a, 1); break;
        case LB_DOWN: layer_shift(a, -1); break;
        case LB_MERGE: layer_merge(a); break;
        default: break;
        }
        break;
    case LH_OPACITY:
        if (button == 272) {
            struct lp_g g;
            lp_geom(a, &g);
            checkpoint_props(a);
            a->sl_drag = 100;
            a->doc.L[a->doc.cur].opacity = slider_value_at(0, 100, a->px, g.ox + SL_KNOB / 2 - SL_KNOB / 2, g.ow);
            changed(a);
        }
        break;
    case LH_BLEND:
        if (button == 272 && wlwin_menu(&a->win, "Normal\nMultiply\nScreen\nOverlay\nDarken\nLighten\nDifference"))
            a->menu_kind = 5;
        break;
    case LH_EYE:
        if (button == 272)
            layer_toggle(a, idx);
        break;
    case LH_ROW: {
        int64_t now = now_ms();
        bool dbl = button == 272 && idx == a->lay_click_idx && now - a->lay_click_ms < lm_metrics.double_click_ms;
        a->lay_click_idx = button == 272 ? idx : -1;
        a->lay_click_ms = now;
        layer_select(a, idx);
        if (dbl) {
            prompt_open(a, P_LAYERNAME, a->doc.L[a->doc.cur].name);
            wltext_select_all(a->pbuf);
        } else if (button == 273 && wlwin_menu(&a->win, "Rename...\nDuplicate\nDelete\nMerge down\nShow / hide"))
            a->menu_kind = 6;
        break;
    }
    default: break;
    }
    a->win.redraw = true;
}

/* A press in the strip: the colour squares, the colours used lately, the sliders and the switches. */
static void strip_press(struct app *a, uint32_t button)
{
    int idx;
    int hit = strip_hit(a, a->px, a->py, &idx);
    struct strip_g s;
    strip_layout(a, &s);
    switch (hit) {
    case SH_FG: cp_open(a, false); break;
    case SH_BG: cp_open(a, true); break;
    case SH_SWAP: {
        uint32_t t = a->fg;
        a->fg = a->bg;
        a->bg = t;
        break;
    }
    case SH_RECENT:
        if (button == 273)
            a->bg = a->recent[idx];
        else
            a->fg = a->recent[idx];
        break;
    case SH_SLIDER:
        if (button == 272) {
            int id = s.sl[idx].id, mn, mx;
            slider_range(a, id, &mn, &mx);
            a->sl_drag = id;
            slider_set(a, id, slider_value_at(mn, mx, a->px, s.sl[idx].x + SL_KNOB / 2 - SL_KNOB / 2, s.sl[idx].w));
        }
        break;
    case SH_TOGGLE:
        if (button == 272)
            a->tg[s.tg[idx].id] = !a->tg[s.tg[idx].id];
        break;
    default: return;
    }
    if (a->txt.on)
        text_preview(a);
    a->win.redraw = true;
}

/* The slider that is being dragged follows the pointer. */
static void slider_drag(struct app *a)
{
    if (a->sl_drag == 100) {
        struct lp_g g;
        lp_geom(a, &g);
        a->doc.L[a->doc.cur].opacity = slider_value_at(0, 100, a->px, g.ox, g.ow);
        changed(a);
        return;
    }
    struct strip_g s;
    strip_layout(a, &s);
    for (int i = 0; i < s.nsl; i++)
        if (s.sl[i].id == a->sl_drag) {
            int mn, mx;
            slider_range(a, a->sl_drag, &mn, &mx);
            slider_set(a, a->sl_drag, slider_value_at(mn, mx, a->px, s.sl[i].x, s.sl[i].w));
            if (a->txt.on)
                text_preview(a);
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
        if (a->sl_drag >= 0)
            slider_drag(a);
        else if (a->pan) {
            a->vx = a->pan_vx + (a->px - a->pan_x);
            a->vy = a->pan_vy + (a->py - a->pan_y);
            a->fitted = false;
            view_clamp(a);
            a->vdirty = true;
        } else if (a->drag) {
            double ix, iy;
            to_pic(a, a->px, a->py, &ix, &iy);
            drag_motion(a, ix, iy, (w->mods & LM_MOD_SHIFT) != 0);
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
            if (a->sl_drag >= 0) {
                a->sl_drag = -1;
                a->modified = a->modified || false;
            }
            if (button == 274)
                a->pan = false;
            else if ((int)button == a->drag_button || a->drag)
                end_drag(a);
            w->redraw = true;
            break;
        }
        int bi = button_at(a, a->px, a->py);
        if (button == 273 && (bi == B_SELECT || bi == B_SHAPES)) {                    /* a right click on a group opens its menu at once */
            finish_pending(a);
            if (!wlwin_menu(&a->win, bi == B_SELECT ? "Rectangle  S\nEllipse  O\nLasso  Q\nMagic wand  W" : "Line  L\nBox  B\nCircle  C\nPolygon / star  N"))
                say(a, "The compositor has no menu service");
            else
                a->menu_kind = bi == B_SELECT ? 3 : 4;
            break;
        }
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
        if (layers_on(a) && a->px >= panel_x(a) && a->px < panel_x(a) + LP_W && a->py < cv_h(a)) {
            layers_press(a, button);
            break;
        }
        if (a->py >= pal_y(a) && a->py < pal_y(a) + PAL_H) {
            strip_press(a, button);
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
        if ((button == 272 || (button == 273 && !tool_in_select(a->tool) && a->tool != T_MOVE && a->tool != T_TEXT)) && in_canvas(a, a->px, a->py))
            begin_drag(a, (int)button);
        break;
    }
    case LM_WL_POINTER_EVT_AXIS:
        if (args[1].u == 0) {
            double v = lm_fixed_to_double(args[2].f);
            int sidx, lidx;
            if (v == 0)
                break;
            int hit = strip_hit(a, a->px, a->py, &sidx);
            struct strip_g s;
            strip_layout(a, &s);
            int lh = lp_hit(a, a->px, a->py, &lidx);
            if (hit == SH_SLIDER) {
                int id = s.sl[sidx].id, mn, mx;
                slider_range(a, id, &mn, &mx);
                int step = (w->mods & LM_MOD_SHIFT) ? LM_MAX((mx - mn) / 10, 1) : LM_MAX((mx - mn) / 50, 1);
                slider_set(a, id, *slider_ptr(a, id) + (v < 0 ? step : -step));
                if (a->txt.on)
                    text_preview(a);
            } else if (lh == LH_OPACITY) {
                checkpoint_props(a);
                a->doc.L[a->doc.cur].opacity = LM_CLAMP(a->doc.L[a->doc.cur].opacity + (v < 0 ? 5 : -5), 0, 100);
                changed(a);
            } else if (layers_on(a) && a->px >= panel_x(a) && a->px < panel_x(a) + LP_W && a->py < cv_h(a)) {
                struct lp_g g;
                lp_geom(a, &g);
                a->lay_top = LM_CLAMP(a->lay_top + (v < 0 ? -1 : 1), 0, LM_MAX(a->doc.n - g.nvis, 0));
            } else if (in_canvas(a, a->px, a->py))
                zoom_to(a, a->zoom * (v < 0 ? 1.25 : 1.0 / 1.25), a->px - cv_x(a), a->py);
            w->redraw = true;
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
    if (a->prompt == P_ADJUST) {
        if (ks == LM_KS_Escape)
            prompt_close(a, false);
        else if (ks == LM_KS_Return || ks == LM_KS_KP_Enter)
            prompt_accept(a);
        return;
    }
    if (ks == LM_KS_Escape) {
        prompt_close(a, false);
    } else if (a->prompt == P_COLOUR) {
        if (ks == LM_KS_Return || ks == LM_KS_KP_Enter)
            prompt_accept(a);
        else if (wltext_edit(&a->clip, a->pbuf, sizeof(a->pbuf), ks, a->win.mods)) {
            uint32_t c;
            if (cp_parse_hex(a->pbuf, &c))
                cp_set(a, c, false);
        }
    } else if (card_list_rows(a) && (ks == LM_KS_Up || ks == LM_KS_Down || ks == LM_KS_Page_Up || ks == LM_KS_Page_Down ||
                                     ((ks == LM_KS_Return || ks == LM_KS_KP_Enter) && a->pk.sel >= 0))) {
        char base[PATH_MAX];
        default_dir(a, base, sizeof(base));
        if (wlpicker_key(&a->pk, base, a->pbuf, sizeof(a->pbuf), card_list_rows(a), ks) == WLPICKER_OPEN)
            prompt_accept(a);                                                   /* a folder was entered, a picture is taken */
        a->perr[0] = 0;
    } else if (ks == LM_KS_Return || ks == LM_KS_KP_Enter) {
        prompt_accept(a);
    } else if (ks == LM_KS_Tab && list_prompt(a)) {
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
    if (a->txt.on && text_key(a, ks, w->mods))
        return;
    char txt[8];
    int tl = lm_keysym_to_utf8(ks, txt);
    char c = tl == 1 && !ctrl && !alt ? txt[0] : 0;
    if (ctrl && (ks == LM_KS_Left || ks == LM_KS_Right)) {
        do_transform(a, ks == LM_KS_Left ? 0 : 1);
        return;
    }
    if (ctrl) {
        char k = tl == 1 ? (char)tolower((unsigned char)txt[0]) : 0;
        if (alt && k == 'c') {
            finish_pending(a);
            ask_canvas(a);
            return;
        }
        switch (k) {
        case 'u': adj_open(a); break;
        case 'o': guard(a, AF_OPEN); break;
        case 'n': if (shift) layer_add(a); else guard(a, AF_NEW); break;
        case 's': if (shift) ask_saveas(a); else do_save(a); break;
        case 'z': do_undo(a, shift); break;
        case 'y': do_undo(a, true); break;
        case 'q': guard(a, AF_QUIT); break;
        case 'r': finish_pending(a); ask_resize(a); break;
        case 'a': finish_pending(a); sel_all(a); break;
        case 'd': finish_pending(a); sel_clear(&a->sel); a->vdirty = true; break;
        case 'i': finish_pending(a); sel_invert(a); break;
        case 'c': do_copy(a, false); break;
        case 'x': do_copy(a, true); break;
        case 'v': do_paste(a); break;
        case 'j': layer_dup(a); break;
        case 'e': layer_merge(a); break;
        case 'g': a->grid = !a->grid; a->vdirty = true; break;
        default: break;
        }
        return;
    }
    switch (ks) {
    case LM_KS_Escape:
        if (a->sel.on) {
            sel_clear(&a->sel);
            a->vdirty = true;
        }
        break;
    case LM_KS_Return:
    case LM_KS_KP_Enter: do_crop(a); break;
    case LM_KS_Delete: do_delete_sel(a); break;
    case LM_KS_BackSpace: do_fill_sel(a); break;
    case LM_KS_Tab: a->show_layers = !a->show_layers; a->fitted ? view_fit(a) : view_clamp(a); a->vdirty = true; break;
    case LM_KS_Page_Up: layer_select(a, a->doc.cur + 1); break;
    case LM_KS_Page_Down: layer_select(a, a->doc.cur - 1); break;
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
            tool_set(a, (enum tool)t);
            return;
        }
    switch (c) {
    case '[': case ']': {
        int *v = slider_ptr(a, S_SIZE), step = LM_MAX(*v / 8, 1);
        slider_set(a, S_SIZE, *v + (c == ']' ? step : -step));
        break;
    }
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
    finish_pending(a);
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

/* ---- what is kept between runs: the colours, the sliders, the panel -------------------------------------------------------------- */

static void conf_path(char *out, size_t n)
{
    const char *x = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
    if (x && *x)
        snprintf(out, n, "%s/mellagui/paint.conf", x);
    else if (h && *h)
        snprintf(out, n, "%s/.config/mellagui/paint.conf", h);
    else
        out[0] = 0;
}

static void conf_load(struct app *a)
{
    char path[PATH_MAX];
    conf_path(path, sizeof(path));
    FILE *f = path[0] ? fopen(path, "r") : NULL;
    if (!f)
        return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        while (*k == ' ')
            k++;
        for (char *e = k + strlen(k); e > k && (e[-1] == ' ' || e[-1] == '\t'); )
            *--e = 0;
        while (*v == ' ')
            v++;
        v[strcspn(v, "\r\n")] = 0;
        uint32_t c;
        if (!strcmp(k, "fg") && cp_parse_hex(v, &c))
            a->fg = c;
        else if (!strcmp(k, "bg") && cp_parse_hex(v, &c))
            a->bg = c;
        else if (!strcmp(k, "recent")) {
            a->nrecent = 0;
            for (char *tok = strtok(v, " "); tok && a->nrecent < N_RECENT; tok = strtok(NULL, " "))
                if (cp_parse_hex(tok, &c))
                    a->recent[a->nrecent++] = c;
        } else if (!strcmp(k, "size"))
            a->val[S_SIZE] = LM_CLAMP(atoi(v), SLIDERS[S_SIZE].mn, SLIDERS[S_SIZE].mx);
        else if (!strcmp(k, "opacity"))
            a->val[S_OPACITY] = LM_CLAMP(atoi(v), SLIDERS[S_OPACITY].mn, SLIDERS[S_OPACITY].mx);
        else if (!strcmp(k, "hardness"))
            a->val[S_HARD] = LM_CLAMP(atoi(v), SLIDERS[S_HARD].mn, SLIDERS[S_HARD].mx);
        else if (!strcmp(k, "tolerance"))
            a->val[S_TOL] = LM_CLAMP(atoi(v), SLIDERS[S_TOL].mn, SLIDERS[S_TOL].mx);
        else if (!strcmp(k, "corner"))
            a->val[S_CORNER] = LM_CLAMP(atoi(v), SLIDERS[S_CORNER].mn, SLIDERS[S_CORNER].mx);
        else if (!strcmp(k, "sides"))
            a->val[S_SIDES] = LM_CLAMP(atoi(v), SLIDERS[S_SIDES].mn, SLIDERS[S_SIDES].mx);
        else if (!strcmp(k, "density"))
            a->val[S_DENSITY] = LM_CLAMP(atoi(v), SLIDERS[S_DENSITY].mn, SLIDERS[S_DENSITY].mx);
        else if (!strcmp(k, "text-size"))
            a->tsize = LM_CLAMP(atoi(v), 6, 400);
        else if (!strcmp(k, "layers"))
            a->show_layers = atoi(v) != 0;
        else if (!strcmp(k, "grid"))
            a->grid = atoi(v) != 0;
    }
    fclose(f);
}

static void conf_save(const struct app *a)
{
    char path[PATH_MAX];
    conf_path(path, sizeof(path));
    if (!path[0])
        return;
    for (char *s = strchr(path + 1, '/'); s; s = strchr(s + 1, '/')) {
        *s = 0;
        if (strchr(s + 1, '/'))
            mkdir(path, 0700);
        *s = '/';
    }
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "fg = %08x\nbg = %08x\nrecent =", a->fg, a->bg);
    for (int i = 0; i < a->nrecent; i++)
        fprintf(f, " %08x", a->recent[i]);
    fprintf(f, "\nsize = %d\nopacity = %d\nhardness = %d\ntolerance = %d\ncorner = %d\nsides = %d\ndensity = %d\ntext-size = %d\nlayers = %d\ngrid = %d\n", a->val[S_SIZE], a->val[S_OPACITY], a->val[S_HARD],
            a->val[S_TOL], a->val[S_CORNER], a->val[S_SIDES], a->val[S_DENSITY], a->tsize, a->show_layers ? 1 : 0, a->grid ? 1 : 0);
    fclose(f);
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
    for (int i = 0; i < S_COUNT; i++)
        a.val[i] = SLIDERS[i].def;
    a.tsize = 28;
    a.show_layers = true;
    a.grp_sel = T_SELECT;
    a.grp_shape = T_LINE;
    a.tool = T_PEN;
    a.sl_drag = -1;
    a.adj_drag = -1;
    a.lay_click_idx = -1;
    a.lay_seen = -1;
    a.cur_kind = -1;
    a.pcard_hover = a.pcard_down = -1;
    a.rnd = ((uint32_t)time(NULL) ^ ((uint32_t)getpid() << 16)) | 1u;
    conf_load(&a);
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
        new_doc(&a, nw, nh, file ? file : "", false);
    } else if (file) {
        if (!open_image(&a, file)) {                                    /* a name that does not exist yet: an empty picture that Save puts there */
            struct stat st;
            if (stat(file, &st) != 0)
                new_doc(&a, 800, 600, file, false);
            else
                new_doc(&a, 800, 600, "", false);
            say(&a, "Could not open %s: it is a new picture", file);
        }
    } else {
        new_doc(&a, 800, 600, "", false);
    }
    while (a.win.running) {
        int timeout = -1;
        struct wlrail L, R;
        rails(&a, &L, &R);
        timeout = wlrail_loop(&a.win, &a.ran[0], &L, rail_hover_of(&a, false), timeout);
        timeout = wlrail_loop(&a.win, &a.ran[1], &R, rail_hover_of(&a, true), timeout);
        if (a.msg[0] && now_ms() - a.msg_ms < 6200 && (timeout < 0 || timeout > 500))
            timeout = 500;                                                /* the message goes away by itself */
        if (a.drag && a.tool == T_SPRAY)
            timeout = 25;                                                 /* the airbrush keeps spraying while the button is held */
        if (!wlwin_step(&a.win, timeout))
            break;
        if (a.drag && a.tool == T_SPRAY && now_ms() - a.spray_ms >= 25) {
            double ix, iy;
            to_pic(&a, a.px, a.py, &ix, &iy);
            spray_at(&a, ix, iy);
            a.spray_ms = now_ms();
        }
        if (a.msg[0] && now_ms() - a.msg_ms >= 6000) {
            a.msg[0] = 0;
            a.win.redraw = true;
        }
    }
    conf_save(&a);
    text_end(&a);
    drop_snaps(a.undo, &a.nundo);
    drop_snaps(a.redo, &a.nredo);
    doc_free(&a.doc);
    pix_free(&a.flat);
    stroke_end(&a);
    move_end(&a);
    pix_free(&a.clipimg);
    pix_free(&a.adj_before);
    sel_clear(&a.sel);
    free(a.lx_pts);
    free(a.ly_pts);
    free(a.vbuf);
    wlwin_destroy(&a.win);
    return 0;
}
