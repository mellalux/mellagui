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
#include <dirent.h>
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
#include "fontread.h"
#include "img.h"
#include "gifframes.h"
#include "imgwrite.h"
#include "jpegenc.h"
#include "keymap.h"
#include "metrics.h"
#include "pathcomp.h"
#include "protocol.h"
#include "theme.h"
#include "ttf.h"
#include "uikit.h"
#include "util.h"
#include "wlc.h"
#include "wlclip.h"
#include "wlcard.h"
#include "wlpicker.h"
#include "wlsbar.h"
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

enum blendm { BM_NORMAL, BM_MULTIPLY, BM_SCREEN, BM_OVERLAY, BM_DARKEN, BM_LIGHTEN, BM_DIFFERENCE, BM_COUNT, BM_PASS = BM_COUNT };   /* (BM_PASS = pass through: only a group has it) */
static const char *const BM_NAMES[BM_COUNT] = { "Normal", "Multiply", "Screen", "Overlay", "Darken", "Lighten", "Difference" };
static const char *const BM_ORA[BM_COUNT] = { "svg:src-over", "svg:multiply", "svg:screen", "svg:overlay", "svg:darken", "svg:lighten", "svg:difference" };

#define MAX_LAYERS 32

/* The text a layer was made from, kept so that it can be typed into again. It is only believed while the layer's pixels are still the ones the text made
 * (`hash`): paint on the layer, move it or filter it and it is a plain picture from then on. */
struct ltext {
    bool on;
    int len, x, y, size, align, line, letter, outline, shadow, angle, bend, opacity;
    bool bold, italic, under, strike, box;
    uint32_t fg, bg, hash;
    char font[64];
    char buf[1024];
};

/* What the layer's pixels add up to (the colour of a see-through pixel does not count: a file may write it as it likes). */
static uint32_t pix_hash_ink(const struct pix *im)
{
    uint32_t h = 2166136261u;
    size_t n = (size_t)im->w * (size_t)im->h;
    for (size_t i = 0; i < n; i++) {
        uint32_t c = im->p[i];
        if (!(c >> 24))
            c = 0;
        h = (h ^ c) * 16777619u;
    }
    return h ^ ((uint32_t)im->w * 40503u) ^ ((uint32_t)im->h * 9973u);
}

struct layer {
    struct pix im;
    struct pix mask;                    /* an optional mask: white shows the layer, black hides it (grey values in the colour, see-through counts as white); .p NULL = none */
    struct ltext tx;
    char name[40];
    int opacity;                        /* 0..100 */
    bool visible;
    int blend;                          /* enum blendm */
    int gid;                            /* the innermost group the layer is in: 0 = none, else the group's slot + 1 */
};

#define MAX_GROUPS 16

/* A group of layers (a folder): the layers of one are next to each other in the stack, a group may lie inside another. It has no pixels, only how its layers are put together:
 * `opacity` and `blend` (BM_PASS = pass through: the layers inside mix with what is below the group as if the group were not there), shown or hidden, and whether the panel
 * lists what is inside. `parent` is the group it lies in (0 = none, else slot + 1). */
struct lgroup {
    bool used, visible, collapsed;
    char name[40];
    int opacity, blend, parent;
};

struct doc {
    struct layer L[MAX_LAYERS];
    struct lgroup G[MAX_GROUPS];
    int n, cur;
    uint32_t paper;                     /* the colour of the sheet itself, behind every layer (0 = none: the checkerboard shows); it is not a layer, so Move leaves it where it is */
};

static int doc_w(const struct doc *d) { return d->n ? d->L[0].im.w : 0; }
static int doc_h(const struct doc *d) { return d->n ? d->L[0].im.h : 0; }

static void doc_free(struct doc *d)
{
    for (int i = 0; i < d->n; i++) {
        pix_free(&d->L[i].im);
        pix_free(&d->L[i].mask);
    }
    memset(d, 0, sizeof(*d));
}

static bool doc_copy(const struct doc *s, struct doc *d)
{
    memset(d, 0, sizeof(*d));
    for (int i = 0; i < s->n; i++) {
        d->L[i] = s->L[i];
        d->L[i].im = (struct pix){ 0 };
        d->L[i].mask = (struct pix){ 0 };
        if (!pix_copy(&s->L[i].im, &d->L[i].im)) {
            d->n = i;
            doc_free(d);
            return false;
        }
        if (s->L[i].mask.p && !pix_copy(&s->L[i].mask, &d->L[i].mask)) {
            d->n = i + 1;
            doc_free(d);
            return false;
        }
    }
    d->n = s->n;
    d->cur = s->cur;
    d->paper = s->paper;
    memcpy(d->G, s->G, sizeof(d->G));
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
    d->L[at].gid = d->L[d->cur].gid;                                    /* (a layer made next to one in a group is in the group) */
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
    struct pix mk = { 0 };
    if (d->L[d->cur].mask.p && !pix_copy(&d->L[d->cur].mask, &mk)) {
        pix_free(&im);
        memmove(&d->L[at], &d->L[at + 1], sizeof(d->L[0]) * (size_t)(d->n - at));
        return false;
    }
    d->L[at] = d->L[d->cur];
    d->L[at].im = im;
    d->L[at].mask = mk;
    memcpy(d->L[at].name, nm, sizeof(nm));
    d->n++;
    d->cur = at;
    return true;
}

static void doc_prune_groups(struct doc *d);

static bool doc_del(struct doc *d)
{
    if (d->n <= 1)
        return false;
    pix_free(&d->L[d->cur].im);
    pix_free(&d->L[d->cur].mask);
    memmove(&d->L[d->cur], &d->L[d->cur + 1], sizeof(d->L[0]) * (size_t)(d->n - d->cur - 1));
    d->n--;
    memset(&d->L[d->n], 0, sizeof(d->L[0]));
    if (d->cur >= d->n)
        d->cur = d->n - 1;
    doc_prune_groups(d);
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
/* How much of the layer a mask lets through at a pixel (0..255): the mask's grey, a see-through mask pixel counting as white. */
static int mask_val(uint32_t c)
{
    int a = ch_a(c), l = (ch_r(c) * 299 + ch_g(c) * 587 + ch_b(c) * 114) / 1000;
    return (l * a + 255 * (255 - a)) / 255;
}

/* The layer's pixel with its mask laid over it. */
static uint32_t layer_px(const struct layer *l, size_t i)
{
    uint32_t c = l->im.p[i];
    if (l->mask.p && l->mask.w == l->im.w && l->mask.h == l->im.h) {
        int m = mask_val(l->mask.p[i]);
        if (m < 255)
            c = (c & 0x00ffffffu) | ((uint32_t)(ch_a(c) * m / 255) << 24);
    }
    return c;
}

/* The mask becomes part of the layer's pixels (and goes). */
static void layer_bake_mask(struct layer *l)
{
    if (!l->mask.p)
        return;
    size_t n = (size_t)l->im.w * (size_t)l->im.h;
    if (l->mask.w == l->im.w && l->mask.h == l->im.h)
        for (size_t i = 0; i < n; i++)
            l->im.p[i] = layer_px(l, i);
    pix_free(&l->mask);
}

/* Every mask of the picture applied: a change of the sheet's shape (a turn, a new size, a crop) works on plain layers only. */
static void doc_bake_masks(struct doc *d)
{
    for (int i = 0; i < d->n; i++)
        layer_bake_mask(&d->L[i]);
}

/* ---- groups of layers ------------------------------------------------------------------------------------------------------------ */

/* The groups a group slot lies in, outermost first (`gid` = the innermost one, 0 = none); returns how many. */
static int gchain(const struct doc *d, int gid, int *out)
{
    int tmp[MAX_GROUPS], k = 0;
    while (gid > 0 && gid <= MAX_GROUPS && d->G[gid - 1].used && k < MAX_GROUPS) {
        tmp[k++] = gid;
        gid = d->G[gid - 1].parent;
    }
    for (int i = 0; i < k; i++)
        out[i] = tmp[k - 1 - i];
    return k;
}

/* Whether a thing in group `gid` (0 = none) is inside group `anc` (the group itself or one of its parents counts). */
static bool g_within(const struct doc *d, int gid, int anc)
{
    for (int k = 0; gid > 0 && gid <= MAX_GROUPS && k <= MAX_GROUPS; k++) {
        if (gid == anc)
            return true;
        gid = d->G[gid - 1].parent;
    }
    return false;
}

/* The first and last layer of a group (everything inside it, groups in groups too); false when it holds none. */
static bool group_range(const struct doc *d, int g, int *lo, int *hi)
{
    int a = -1, b = -1;
    for (int i = 0; i < d->n; i++)
        if (g_within(d, d->L[i].gid, g)) {
            if (a < 0)
                a = i;
            b = i;
        }
    if (a < 0)
        return false;
    *lo = a;
    *hi = b;
    return true;
}

static int groups_used(const struct doc *d)
{
    int n = 0;
    for (int i = 0; i < MAX_GROUPS; i++)
        n += d->G[i].used;
    return n;
}

static int g_new_slot(const struct doc *d)
{
    for (int i = 0; i < MAX_GROUPS; i++)
        if (!d->G[i].used)
            return i + 1;
    return 0;
}

static void group_init(struct lgroup *g, const char *name, int parent)
{
    memset(g, 0, sizeof(*g));
    g->used = g->visible = true;
    g->opacity = 100;
    g->blend = BM_PASS;
    g->parent = parent;
    snprintf(g->name, sizeof(g->name), "%s", name);
}

/* Groups with no layer in them go: a group has no place of its own, only its layers give it one. */
static void doc_prune_groups(struct doc *d)
{
    bool has[MAX_GROUPS + 1] = { false };
    for (int i = 0; i < d->n; i++) {
        int g = d->L[i].gid;
        for (int k = 0; g > 0 && g <= MAX_GROUPS && k <= MAX_GROUPS; k++) {
            has[g] = true;
            g = d->G[g - 1].parent;
        }
    }
    for (int g = 1; g <= MAX_GROUPS; g++)
        if (d->G[g - 1].used && !has[g])
            memset(&d->G[g - 1], 0, sizeof(d->G[g - 1]));
}

static const char *bm_name(int b)
{
    return b == BM_PASS ? "Pass through" : b >= 0 && b < BM_COUNT ? BM_NAMES[b] : "?";
}

/* A new group round the chosen layer (inside the group the layer is in, if it is in one). */
static bool doc_group_layer(struct doc *d)
{
    int slot = g_new_slot(d);
    if (!slot || !d->n)
        return false;
    char nm[40];
    snprintf(nm, sizeof(nm), "Group %d", groups_used(d) + 1);
    group_init(&d->G[slot - 1], nm, d->L[d->cur].gid);
    d->L[d->cur].gid = slot;
    return true;
}

/* The group goes, what is in it stays (in the group the group was in). */
static bool doc_ungroup(struct doc *d, int g)
{
    if (g < 1 || g > MAX_GROUPS || !d->G[g - 1].used)
        return false;
    int up = d->G[g - 1].parent;
    for (int i = 0; i < d->n; i++)
        if (d->L[i].gid == g)
            d->L[i].gid = up;
    for (int k = 0; k < MAX_GROUPS; k++)
        if (d->G[k].used && d->G[k].parent == g)
            d->G[k].parent = up;
    memset(&d->G[g - 1], 0, sizeof(d->G[g - 1]));
    return true;
}

/* The layers an item takes up (a layer, or a group when `grp` > 0) and the group it lies in. */
static bool item_block(const struct doc *d, int layer, int grp, int *lo, int *hi, int *level)
{
    if (grp > 0) {
        if (grp > MAX_GROUPS || !d->G[grp - 1].used || !group_range(d, grp, lo, hi))
            return false;
        *level = d->G[grp - 1].parent;
        return true;
    }
    if (layer < 0 || layer >= d->n)
        return false;
    *lo = *hi = layer;
    *level = d->L[layer].gid;
    return true;
}

/* The item next to lo..hi on the same level, above (dir > 0) or below: its layers, and its group (0 when it is one layer). */
static bool item_sibling(const struct doc *d, int lo, int hi, int level, int dir, int *slo, int *shi, int *sgrp)
{
    int j = dir > 0 ? hi + 1 : lo - 1;
    if (j < 0 || j >= d->n)
        return false;
    int g = d->L[j].gid;
    if (g == level) {
        *slo = *shi = j;
        *sgrp = 0;
        return true;
    }
    for (int k = 0; g > 0 && g <= MAX_GROUPS && d->G[g - 1].parent != level && k <= MAX_GROUPS; k++)
        g = d->G[g - 1].parent;
    if (g <= 0 || g > MAX_GROUPS || d->G[g - 1].parent != level || !group_range(d, g, slo, shi))
        return false;
    *sgrp = g;
    return true;
}

/* The two blocks of layers lo..mid-1 and mid..hi change places. */
static void layers_rotate(struct doc *d, int lo, int mid, int hi)
{
    static struct layer tmp[MAX_LAYERS];
    int n = hi - lo + 1, na = mid - lo, nb = hi - mid + 1;
    memcpy(tmp, &d->L[lo], sizeof(struct layer) * (size_t)n);
    memcpy(&d->L[lo], &tmp[na], sizeof(struct layer) * (size_t)nb);
    memcpy(&d->L[lo + nb], &tmp[0], sizeof(struct layer) * (size_t)na);
    if (d->cur >= lo && d->cur < mid)
        d->cur += nb;
    else if (d->cur >= mid && d->cur <= hi)
        d->cur -= na;
}

/* An item (a layer, or the group `grp`) changes places with the one next to it on its level (dir > 0: up the stack). */
static bool doc_move_item(struct doc *d, int layer, int grp, int dir)
{
    int lo, hi, level, slo, shi, sg;
    if (!item_block(d, layer, grp, &lo, &hi, &level) || !item_sibling(d, lo, hi, level, dir, &slo, &shi, &sg))
        return false;
    if (dir > 0)
        layers_rotate(d, lo, slo, shi);
    else
        layers_rotate(d, slo, lo, hi);
    return true;
}

static bool doc_move(struct doc *d, int dir)                           /* +1 = up the stack */
{
    return doc_move_item(d, d->cur, 0, dir);
}

/* The item goes into the group next to it (above: dir > 0) as its lowest / highest part. */
static bool doc_enter_group(struct doc *d, int layer, int grp, int dir)
{
    int lo, hi, level, slo, shi, sg;
    if (!item_block(d, layer, grp, &lo, &hi, &level) || !item_sibling(d, lo, hi, level, dir, &slo, &shi, &sg) || !sg)
        return false;
    if (grp > 0)
        d->G[grp - 1].parent = sg;
    else
        d->L[layer].gid = sg;
    return true;
}

/* The item leaves its group: it goes up to the top of it (past what is above it in the group), then out. */
static bool doc_leave_group(struct doc *d, int layer, int grp)
{
    int lo, hi, level;
    if (!item_block(d, layer, grp, &lo, &hi, &level) || level == 0)
        return false;
    for (int guard = 0; guard < MAX_LAYERS; guard++) {
        int slo, shi, sg;
        if (!item_sibling(d, lo, hi, level, 1, &slo, &shi, &sg))
            break;
        int len = hi - lo + 1, slen = shi - slo + 1;
        layers_rotate(d, lo, slo, shi);
        lo += slen;
        hi = lo + len - 1;
        if (grp == 0)
            layer = lo;
    }
    int up = d->G[level - 1].parent;
    if (grp > 0)
        d->G[grp - 1].parent = up;
    else
        d->L[layer].gid = up;
    return true;
}

/* The layers of the box `r` composited over `paper` into `out` (the same size as the layers), groups and all: a group is put together first (a pass through group lets the
 * layers in it mix with what is below it, any other is laid over as one picture with its opacity and blend mode); a hidden group shows nothing. `incl` (or NULL = all)
 * says which layers take part. */
static void composite_px(const struct doc *d, const uint8_t *incl, uint32_t paper, struct pix *out, struct irect r)
{
    irect_clip(&r, out->w, out->h);
    struct cstep {
        int nclose, nopen, open[MAX_GROUPS];
    } steps[MAX_LAYERS];
    int st[MAX_GROUPS], sd = 0;
    for (int l = 0; l < d->n; l++) {                                    /* when going on to layer l: which groups end, which begin */
        int ch[MAX_GROUPS], cl = gchain(d, d->L[l].gid, ch), p = 0;
        while (p < sd && p < cl && st[p] == ch[p])
            p++;
        steps[l].nclose = sd - p;
        sd = p;
        steps[l].nopen = cl - p;
        for (int k = 0; k < cl - p; k++) {
            steps[l].open[k] = ch[p + k];
            st[sd++] = ch[p + k];
        }
    }
    int tail = sd;
    for (int y = r.y0; y < r.y1; y++)
        for (int x = r.x0; x < r.x1; x++) {
            size_t i = (size_t)y * (size_t)out->w + (size_t)x;
            uint32_t acc[MAX_GROUPS + 1];
            bool inl[MAX_GROUPS + 1], hid[MAX_GROUPS + 1];
            int gs[MAX_GROUPS + 1], sp = 0;
            acc[0] = paper;
            hid[0] = inl[0] = false;
            gs[0] = 0;
#define CLOSE_TOP()                                                                                                                                                   \
    do {                                                                                                                                                              \
        const struct lgroup *g_ = &d->G[gs[sp] - 1];                                                                                                                  \
        if (!hid[sp])                                                                                                                                                 \
            acc[sp - 1] = inl[sp] ? acc[sp] : layer_over(acc[sp - 1], acc[sp], g_->opacity, g_->blend == BM_PASS ? BM_NORMAL : g_->blend);                            \
        sp--;                                                                                                                                                         \
    } while (0)
            for (int l = 0; l < d->n; l++) {
                for (int c = 0; c < steps[l].nclose; c++)
                    CLOSE_TOP();
                for (int k = 0; k < steps[l].nopen; k++) {
                    const struct lgroup *g = &d->G[steps[l].open[k] - 1];
                    sp++;
                    gs[sp] = steps[l].open[k];
                    hid[sp] = hid[sp - 1] || !g->visible;
                    inl[sp] = g->blend == BM_PASS && g->opacity == 100;
                    acc[sp] = inl[sp] ? acc[sp - 1] : 0;
                }
                if (!hid[sp] && d->L[l].visible && (!incl || incl[l]))
                    acc[sp] = layer_over(acc[sp], layer_px(&d->L[l], i), d->L[l].opacity, d->L[l].blend);
            }
            for (int c = 0; c < tail; c++)
                CLOSE_TOP();
#undef CLOSE_TOP
            out->p[i] = acc[0];
        }
}

static void doc_composite(const struct doc *d, struct pix *out, struct irect r)
{
    composite_px(d, NULL, d->paper, out, r);
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
    if (up->gid != lo->gid)                                             /* (the layer below is in another group: there is nothing to merge with) */
        return false;
    layer_bake_mask(lo);                                                /* (both masks count in what they show) */
    size_t n = (size_t)lo->im.w * (size_t)lo->im.h;
    for (size_t i = 0; i < n; i++)
        lo->im.p[i] = up->visible ? layer_over(lo->im.p[i], layer_px(up, i), up->opacity, up->blend) : lo->im.p[i];
    int keep = d->cur - 1;
    doc_del(d);
    d->cur = keep;
    return true;
}

/* The group and everything in it go (the last layers stay). */
static bool doc_delete_group(struct doc *d, int g)
{
    int lo, hi;
    if (g < 1 || g > MAX_GROUPS || !d->G[g - 1].used || !group_range(d, g, &lo, &hi) || hi - lo + 1 >= d->n)
        return false;
    for (int i = lo; i <= hi; i++) {
        pix_free(&d->L[i].im);
        pix_free(&d->L[i].mask);
    }
    int k = hi - lo + 1;
    memmove(&d->L[lo], &d->L[hi + 1], sizeof(d->L[0]) * (size_t)(d->n - hi - 1));
    memset(&d->L[d->n - k], 0, sizeof(d->L[0]) * (size_t)k);
    d->n -= k;
    d->cur = LM_CLAMP(lo - 1, 0, d->n - 1);
    doc_prune_groups(d);
    return true;
}

/* A copy of the group (its layers, the groups inside it) right above it. */
static int doc_dup_group(struct doc *d, int g)
{
    int lo, hi;
    if (g < 1 || g > MAX_GROUPS || !d->G[g - 1].used || !group_range(d, g, &lo, &hi))
        return 0;
    int k = hi - lo + 1, map[MAX_GROUPS + 1] = { 0 }, need = 0;
    for (int s = 1; s <= MAX_GROUPS; s++)
        if (d->G[s - 1].used && g_within(d, s, g))
            need++;
    if (d->n + k > MAX_LAYERS || groups_used(d) + need > MAX_GROUPS)
        return 0;
    static struct layer cp[MAX_LAYERS];
    int made = 0;
    bool ok = true;
    memset(cp, 0, sizeof(struct layer) * (size_t)k);
    for (int i = 0; i < k && ok; i++) {
        cp[i] = d->L[lo + i];
        cp[i].im = (struct pix){ 0 };
        cp[i].mask = (struct pix){ 0 };
        ok = pix_copy(&d->L[lo + i].im, &cp[i].im) && (!d->L[lo + i].mask.p || pix_copy(&d->L[lo + i].mask, &cp[i].mask));
        made = i + 1;
    }
    if (!ok) {
        for (int i = 0; i < made; i++) {
            pix_free(&cp[i].im);
            pix_free(&cp[i].mask);
        }
        return 0;
    }
    for (int s = 1; s <= MAX_GROUPS; s++)                               /* the groups of the copy, in the free slots */
        if (d->G[s - 1].used && g_within(d, s, g))
            map[s] = g_new_slot(d), d->G[map[s] - 1].used = true;      /* (taken at once, so that the next one is another slot) */
    for (int s = 1; s <= MAX_GROUPS; s++)
        if (map[s]) {
            struct lgroup *ng = &d->G[map[s] - 1];
            *ng = d->G[s - 1];
            ng->parent = s == g ? d->G[g - 1].parent : map[d->G[s - 1].parent] ? map[d->G[s - 1].parent] : d->G[s - 1].parent;
            if (s == g) {
                char nm[40];
                snprintf(nm, sizeof(nm), "%.30s copy", d->G[g - 1].name);
                memcpy(ng->name, nm, sizeof(nm));
            }
        }
    memmove(&d->L[hi + 1 + k], &d->L[hi + 1], sizeof(d->L[0]) * (size_t)(d->n - hi - 1));
    for (int i = 0; i < k; i++) {
        d->L[hi + 1 + i] = cp[i];
        if (cp[i].gid > 0 && map[cp[i].gid])
            d->L[hi + 1 + i].gid = map[cp[i].gid];
    }
    d->n += k;
    d->cur = hi + k;
    return map[g];
}

/* The group becomes one layer (what it shows, in its place; its opacity, blend mode and visibility go to the layer). */
static bool doc_merge_group(struct doc *d, int g)
{
    int lo, hi;
    if (g < 1 || g > MAX_GROUPS || !d->G[g - 1].used || !group_range(d, g, &lo, &hi))
        return false;
    struct pix im;
    if (!pix_alloc(&im, doc_w(d), doc_h(d), 0))
        return false;
    uint8_t incl[MAX_LAYERS] = { 0 };
    for (int i = lo; i <= hi; i++)
        incl[i] = 1;
    struct lgroup save = d->G[g - 1];
    d->G[g - 1].visible = true;                                         /* the layer takes the group's own settings; the picture is what is inside */
    d->G[g - 1].opacity = 100;
    d->G[g - 1].blend = BM_PASS;
    composite_px(d, incl, 0, &im, (struct irect){ 0, 0, im.w, im.h });
    d->G[g - 1] = save;
    for (int i = lo; i <= hi; i++) {
        pix_free(&d->L[i].im);
        pix_free(&d->L[i].mask);
    }
    int k = hi - lo + 1;
    memmove(&d->L[lo + 1], &d->L[hi + 1], sizeof(d->L[0]) * (size_t)(d->n - hi - 1));
    memset(&d->L[d->n - k + 1], 0, sizeof(d->L[0]) * (size_t)(k - 1));
    d->n -= k - 1;
    memset(&d->L[lo], 0, sizeof(d->L[0]));
    layer_init(&d->L[lo], save.name);
    d->L[lo].im = im;
    d->L[lo].opacity = save.opacity;
    d->L[lo].blend = save.blend == BM_PASS ? BM_NORMAL : save.blend;
    d->L[lo].visible = save.visible;
    d->L[lo].gid = save.parent;
    d->cur = lo;
    doc_prune_groups(d);
    return true;
}

/* A bigger or smaller sheet: the layers keep their pixels, `ax` / `ay` (0 left / top, 1 middle, 2 right / bottom) say where they sit on it;
 * the new room is transparent, on the lowest layer `fill`. */
static bool doc_canvas(struct doc *d, int nw, int nh, int ax, int ay, uint32_t fill)
{
    doc_bake_masks(d);
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

/* A text layer's text in one attribute of stack.xml (our own, other programs skip it): numbers and the font and text as hex, so no character needs escaping. */
static void tx_to_attr(const struct ltext *t, char *out, size_t n)
{
    size_t o = (size_t)snprintf(out, n, "v1,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%08x,%08x,", t->x, t->y, t->size, t->align, t->line, t->letter, t->outline, t->shadow, t->angle, t->bend,
                                t->opacity, (t->bold ? 1 : 0) | (t->italic ? 2 : 0) | (t->under ? 4 : 0) | (t->strike ? 8 : 0) | (t->box ? 16 : 0), t->fg, t->bg);
    for (const char *c = t->font; *c && o + 3 < n; c++)
        o += (size_t)snprintf(out + o, n - o, "%02x", (unsigned char)*c);
    if (o + 2 < n)
        out[o++] = ',';
    for (int i = 0; i < t->len && o + 3 < n; i++)
        o += (size_t)snprintf(out + o, n - o, "%02x", (unsigned char)t->buf[i]);
    out[o < n ? o : n - 1] = 0;
}

static bool hex_bytes(const char *s, size_t n, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; i + 1 < n && o + 1 < cap; i += 2) {
        unsigned v;
        if (sscanf(s + i, "%2x", &v) != 1)
            return false;
        out[o++] = (char)v;
    }
    out[o] = 0;
    return true;
}

static bool tx_from_attr(const char *attr, struct ltext *t)
{
    memset(t, 0, sizeof(*t));
    int flags = 0, used = 0;
    unsigned fg = 0, bg = 0;
    if (sscanf(attr, "v1,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%x,%x,%n", &t->x, &t->y, &t->size, &t->align, &t->line, &t->letter, &t->outline, &t->shadow, &t->angle, &t->bend, &t->opacity, &flags, &fg,
               &bg, &used) < 14 || used <= 0)
        return false;
    const char *p = attr + used, *comma = strchr(p, ',');
    if (!comma)
        return false;
    if (!hex_bytes(p, (size_t)(comma - p), t->font, sizeof(t->font)))
        return false;
    p = comma + 1;
    size_t hl = strlen(p);
    if (hl > 2 * (sizeof(t->buf) - 1) || !hex_bytes(p, hl, t->buf, sizeof(t->buf)))
        return false;
    t->len = (int)strlen(t->buf);
    t->size = LM_CLAMP(t->size, 6, 400);
    t->align = LM_CLAMP(t->align, 0, 2);
    t->line = LM_CLAMP(t->line, 50, 300);
    t->letter = LM_CLAMP(t->letter, -5, 40);
    t->outline = LM_CLAMP(t->outline, 0, 16);
    t->shadow = LM_CLAMP(t->shadow, 0, 30);
    t->angle = LM_CLAMP(t->angle, -180, 180);
    t->bend = LM_CLAMP(t->bend, -100, 100);
    t->opacity = LM_CLAMP(t->opacity, 1, 100);
    t->bold = flags & 1;
    t->italic = (flags & 2) != 0;
    t->under = (flags & 4) != 0;
    t->strike = (flags & 8) != 0;
    t->box = (flags & 16) != 0;
    t->fg = fg;
    t->bg = bg;
    t->on = t->len > 0;
    return t->on;
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
    size_t xcap = 512 + (size_t)d->n * 2800 + (size_t)MAX_GROUPS * 700, xn = 0;
    char *xml = malloc(xcap);
    ok = ok && xml;
    if (ok) {
        xn += (size_t)snprintf(xml + xn, xcap - xn, "<?xml version='1.0' encoding='UTF-8'?>\n<image version=\"0.0.3\" w=\"%d\" h=\"%d\" mellagui-paper=\"%08x\"><stack>\n", doc_w(d), doc_h(d), d->paper);
        int open[MAX_GROUPS], nopen = 0;                                /* the groups that are open: <stack> inside <stack>, the top layer first */
        for (int l = d->n - 1; l >= 0 && ok; l--) {
            int ch[MAX_GROUPS], cl = gchain(d, d->L[l].gid, ch), p = 0;
            while (p < nopen && p < cl && open[p] == ch[p])
                p++;
            for (; nopen > p; nopen--)
                xn += (size_t)snprintf(xml + xn, xcap - xn, "</stack>\n");
            for (; p < cl; p++) {
                const struct lgroup *gr = &d->G[ch[p] - 1];
                char gn[200];
                xml_escape(gr->name, gn, sizeof(gn));
                xn += (size_t)snprintf(xml + xn, xcap - xn, "<stack name=\"%s\" opacity=\"%.2f\" visibility=\"%s\" composite-op=\"%s\" isolation=\"%s\"%s>\n", gn, gr->opacity / 100.0,
                                       gr->visible ? "visible" : "hidden", BM_ORA[gr->blend == BM_PASS ? BM_NORMAL : gr->blend], gr->blend == BM_PASS ? "auto" : "isolate",
                                       gr->collapsed ? " mellagui-collapsed=\"1\"" : "");
                open[nopen++] = ch[p];
            }
            char nm[200], src[48];
            xml_escape(d->L[l].name, nm, sizeof(nm));
            snprintf(src, sizeof(src), "data/layer%d.png", l);
            char txa[2400] = "";
            if (d->L[l].tx.on && pix_hash_ink(&d->L[l].im) == d->L[l].tx.hash) {                       /* the text is only kept while the pixels are still what it made */
                char enc[2300];
                tx_to_attr(&d->L[l].tx, enc, sizeof(enc));
                snprintf(txa, sizeof(txa), " mellagui-text=\"%s\"", enc);
            }
            if (d->L[l].mask.p)
                snprintf(txa + strlen(txa), sizeof(txa) - strlen(txa), " mellagui-mask=\"data/mask%d.png\"", l);
            xn += (size_t)snprintf(xml + xn, xcap - xn, "<layer name=\"%s\" src=\"%s\" x=\"0\" y=\"0\" opacity=\"%.2f\" visibility=\"%s\" composite-op=\"%s\"%s/>\n", nm, src,
                                   d->L[l].opacity / 100.0, d->L[l].visible ? "visible" : "hidden", BM_ORA[d->L[l].blend], txa);
        }
        for (; nopen > 0; nopen--)
            xn += (size_t)snprintf(xml + xn, xcap - xn, "</stack>\n");
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
        if (ok && d->L[l].mask.p) {                                     /* a mask is a picture of its own (our attribute names it; other programs show the layer unmasked) */
            png = png_encode(&d->L[l].mask, &n, NULL);
            snprintf(nm, sizeof(nm), "data/mask%d.png", l);
            ok = png && zip_add(f, z, &nz, nm, png, n);
            free(png);
        }
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
        struct { char src[200], name[40], mask[200]; int x, y, opacity, blend, gid; bool visible; struct ltext tx; } ent[MAX_LAYERS];
        int ne = 0, ng = 0, gstk[MAX_GROUPS + 8], sp = 0, nstack = 0;
        struct lgroup rg[MAX_GROUPS];
        memset(rg, 0, sizeof(rg));
        const char *scan = xml;
        while (scan && *scan && ne < MAX_LAYERS) {                       /* <stack> inside <stack> are groups, the <layer>s in them belong to them */
            const char *tl = strstr(scan, "<layer"), *ts = strstr(scan, "<stack"), *te = strstr(scan, "</stack>"), *pick = tl;
            if (ts && (!pick || ts < pick))
                pick = ts;
            if (te && (!pick || te < pick))
                pick = te;
            if (!pick)
                break;
            if (pick == te) {
                if (sp > 0)
                    sp--;
                scan = te + 8;
                continue;
            }
            const char *e0 = strchr(pick, '>');
            if (!e0)
                break;
            if (pick == ts) {
                int curg = sp ? gstk[sp - 1] : 0;
                if (nstack++ == 0 || ng >= MAX_GROUPS || sp >= MAX_GROUPS + 6) {                 /* the root, or more than fit: no group of its own */
                    gstk[sp++] = nstack == 1 ? 0 : curg;
                } else {
                    struct lgroup *gr = &rg[ng];
                    group_init(gr, "Group", curg);
                    if (!xml_attr(ts, e0, "name", gr->name, sizeof(gr->name)))
                        snprintf(gr->name, sizeof(gr->name), "Group %d", ng + 1);
                    gr->opacity = xml_attr(ts, e0, "opacity", val, sizeof(val)) ? LM_CLAMP((int)(atof(val) * 100.0 + 0.5), 0, 100) : 100;
                    gr->visible = !(xml_attr(ts, e0, "visibility", val, sizeof(val)) && !strcmp(val, "hidden"));
                    gr->collapsed = xml_attr(ts, e0, "mellagui-collapsed", val, sizeof(val)) && atoi(val) != 0;
                    gr->blend = BM_NORMAL;
                    if (xml_attr(ts, e0, "composite-op", val, sizeof(val)))
                        for (int b = 0; b < BM_COUNT; b++)
                            if (!strcmp(val, BM_ORA[b]))
                                gr->blend = b;
                    if (xml_attr(ts, e0, "isolation", val, sizeof(val)) && !strcmp(val, "auto") && gr->blend == BM_NORMAL)
                        gr->blend = BM_PASS;
                    gstk[sp++] = ++ng;
                }
                scan = e0 + 1;
                continue;
            }
            const char *p = pick, *e = e0;
            scan = e0 + 1;
            ent[ne].gid = sp ? gstk[sp - 1] : 0;
            if (!xml_attr(p, e, "src", ent[ne].src, sizeof(ent[ne].src)) || !ora_name_ok(ent[ne].src))
                continue;
            if (!xml_attr(p, e, "name", ent[ne].name, sizeof(ent[ne].name)))
                snprintf(ent[ne].name, sizeof(ent[ne].name), "Layer %d", ne + 1);
            ent[ne].x = xml_attr(p, e, "x", val, sizeof(val)) ? atoi(val) : 0;
            ent[ne].y = xml_attr(p, e, "y", val, sizeof(val)) ? atoi(val) : 0;
            ent[ne].opacity = xml_attr(p, e, "opacity", val, sizeof(val)) ? LM_CLAMP((int)(atof(val) * 100.0 + 0.5), 0, 100) : 100;
            ent[ne].visible = !(xml_attr(p, e, "visibility", val, sizeof(val)) && !strcmp(val, "hidden"));
            ent[ne].blend = BM_NORMAL;
            memset(&ent[ne].tx, 0, sizeof(ent[ne].tx));
            ent[ne].mask[0] = 0;
            if (xml_attr(p, e, "mellagui-mask", ent[ne].mask, sizeof(ent[ne].mask)) && !ora_name_ok(ent[ne].mask))
                ent[ne].mask[0] = 0;
            {
                static char enc[2400];
                if (xml_attr(p, e, "mellagui-text", enc, sizeof(enc)))
                    tx_from_attr(enc, &ent[ne].tx);
            }
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
                    if (ent[i].tx.on) {
                        l->tx = ent[i].tx;
                        l->tx.hash = pix_hash_ink(&l->im);
                    }
                    if (ent[i].mask[0]) {
                        char mp[PATH_MAX + 220];
                        struct pix ms = { 0 };
                        snprintf(mp, sizeof(mp), "%s/%s", dir, ent[i].mask);
                        char merr[160];
                        if (load_file(mp, &ms, merr, sizeof(merr))) {
                            if (pix_alloc(&l->mask, w, h, 0xffffffffu))
                                for (int y = 0; y < ms.h && y < h; y++)
                                    for (int x = 0; x < ms.w && x < w; x++)
                                        l->mask.p[(size_t)y * (size_t)w + (size_t)x] = ms.p[(size_t)y * (size_t)ms.w + (size_t)x];
                            pix_free(&ms);
                        }
                    }
                } else
                    ok = false;
            }
            memcpy(d->G, rg, sizeof(d->G));
            for (int i = 0; i < d->n; i++)
                d->L[i].gid = ent[d->n - 1 - i].gid;
            doc_prune_groups(d);
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

/* An animated GIF as layers, one for each frame (named "Frame 3 @120": how long it is shown, in ms). 1 = loaded, 0 = a GIF of one picture (the usual way loads it), -1 = trouble. */
static int load_gif_frames(const char *path, struct doc *d, char *err, size_t errsz)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    uint8_t head[6];
    if (fread(head, 1, 6, f) != 6 || memcmp(head, "GIF8", 4)) {
        fclose(f);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz < 13 || sz > 128L << 20) {
        fclose(f);
        return 0;
    }
    uint8_t *buf = malloc((size_t)sz);
    size_t got = buf ? fread(buf, 1, (size_t)sz, f) : 0;
    fclose(f);
    if (!buf || got != (size_t)sz) {
        free(buf);
        return 0;
    }
    struct gif_anim g;
    char gerr[160] = "";
    int rc = lm_gif_frames(buf, (size_t)sz, MAX_LAYERS, &g, gerr, sizeof(gerr));
    free(buf);
    if (rc != 0 || g.n < 2) {
        if (rc == 0)
            lm_gif_anim_free(&g);
        return 0;
    }
    memset(d, 0, sizeof(*d));
    bool ok = true;
    for (int i = 0; i < g.n && ok; i++) {
        struct layer *l = &d->L[d->n];
        if (!pix_alloc(&l->im, g.w, g.h, 0)) {
            snprintf(err, errsz, "that size is not possible");
            ok = false;
            break;
        }
        memcpy(l->im.p, g.px[i], (size_t)g.w * (size_t)g.h * 4);
        char nm[40];
        snprintf(nm, sizeof(nm), "Frame %d @%d", i + 1, g.delay_ms[i]);
        layer_init(l, nm);
        d->n++;
    }
    lm_gif_anim_free(&g);
    if (!ok) {
        doc_free(d);
        return -1;
    }
    d->cur = d->n - 1;
    d->paper = 0;
    return 1;
}

/* Any file as a document: .ora with its layers, an animated GIF with a layer for each frame, everything else as one layer. */
static bool load_doc(const char *path, struct doc *d, char *err, size_t errsz)
{
    err[0] = 0;
    if (is_ora(path))
        return load_ora(path, d, err, errsz);
    int ga = load_gif_frames(path, d, err, errsz);
    if (ga != 0)
        return ga > 0;
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

enum tool { T_SELECT, T_ELLIPSE_SEL, T_LASSO, T_WAND, T_MOVE, T_PEN, T_SPRAY, T_ERASER, T_LINE, T_BOX, T_CIRCLE, T_POLY, T_FILL, T_GRADIENT, T_TEXT, T_PICK, T_FREEPOLY, T_CURVE, T_CLONE, T_SMUDGE, T_BLURBR, T_HAND, T_COUNT };
static const char *const TOOL_NAMES[T_COUNT] = { "Select", "EllipseSelect", "Lasso", "Wand", "Move", "Pen", "Airbrush", "Eraser", "Line", "Box", "Circle", "Shape", "Fill",
                                                 "Gradient", "Text", "Picker", "FreePolygon", "Curve", "Clone", "Smudge", "BlurBrush", "Hand" };
static const char *const TOOL_TITLES[T_COUNT] = { "Select a rectangle", "Select an ellipse", "Lasso", "Magic wand", "Move", "Pen", "Airbrush", "Eraser", "Line", "Box", "Circle",
                                                  "Polygon / star", "Fill", "Gradient", "Text", "Picker", "Free polygon", "Curve", "Clone stamp", "Smudge", "Blur brush", "Hand" };
static const char TOOL_KEYS[T_COUNT] = { 's', 'o', 'q', 'w', 'm', 'p', 'a', 'e', 'l', 'b', 'c', 'n', 'f', 'g', 't', 'i', 'j', 'v', 'k', 'r', 'h', 'y' };
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
    "Click the corners; Enter, a double click or a click on the first corner closes it (Esc cancels); Filled makes it solid",
    "Drag a line, then drag the two handles to bend it; Enter puts it down (Esc cancels)",
    "Alt+click (or the right button) takes the spot to copy from, then paint: the picture there is painted here",
    "Drag to smudge the colours along (Opacity is the strength)",
    "Drag to blur what the brush touches (Opacity is the strength)",
    "Drag to move the picture about in the window (any tool does it while Space is held, or with the middle button); the layers stay where they are",
};

enum slider { S_SIZE, S_OPACITY, S_HARD, S_TOL, S_CORNER, S_SIDES, S_DENSITY, S_LINE, S_LETTER, S_OUTLINE, S_SHADOW, S_ANGLE, S_SMOOTH, S_SYM, S_BEND, S_COUNT };
static const struct { const char *name; int mn, mx, def; const char *unit; } SLIDERS[S_COUNT] = {
    { "Size", 1, 200, 4, " px" }, { "Opacity", 1, 100, 100, "%" }, { "Hardness", 0, 100, 100, "%" }, { "Tolerance", 0, 100, 10, "%" },
    { "Corner", 0, 200, 0, " px" }, { "Sides", 3, 12, 5, "" }, { "Density", 5, 100, 40, "%" },
    { "Line", 50, 300, 100, "%" }, { "Letters", -5, 40, 0, " px" }, { "Outline", 0, 16, 0, " px" }, { "Shadow", 0, 30, 0, " px" }, { "Angle", -180, 180, 0, " deg" },
    { "Smooth", 0, 100, 0, "%" }, { "Symmetry", 1, 16, 1, "" }, { "Bend", -100, 100, 0, "%" },
};

enum toggle { TG_FILLED, TG_STAR, TG_ARROW, TG_RADIAL, TG_FADE, TG_BOLD, TG_GLOBAL, TG_IMAGE, TG_ITALIC, TG_UNDER, TG_STRIKE, TG_BOX, TG_MIRROR, TG_PIXEL, TG_ADD, TG_SUB, TG_RATIO, TG_TLAYER, TG_COUNT };
static const char *const TG_NAMES[TG_COUNT] = { "Filled", "Star", "Arrow", "Radial", "To clear", "Bold", "All of it", "Image", "Italic", "Underline", "Strike", "Box", "Mirror", "Pixel", "Add", "Subtract", "Ratio", "Layer" };

/* What each tool shows in the strip: up to three sliders and two switches (-1 = none). */
static const struct { int sl[3], tg[3]; } TOPT[T_COUNT] = {
    [T_SELECT] = { { -1, -1, -1 }, { TG_ADD, TG_SUB, TG_RATIO } },
    [T_ELLIPSE_SEL] = { { -1, -1, -1 }, { TG_ADD, TG_SUB, -1 } },
    [T_LASSO] = { { -1, -1, -1 }, { TG_ADD, TG_SUB, -1 } },
    [T_WAND] = { { S_TOL, -1, -1 }, { TG_GLOBAL, TG_IMAGE, TG_ADD } },
    [T_MOVE] = { { -1, -1, -1 }, { -1, -1, -1 } },
    [T_PEN] = { { S_SIZE, S_OPACITY, S_HARD }, { -1, -1, -1 } },
    [T_SPRAY] = { { S_SIZE, S_OPACITY, S_DENSITY }, { -1, -1, -1 } },
    [T_ERASER] = { { S_SIZE, S_OPACITY, S_HARD }, { -1, -1, -1 } },
    [T_LINE] = { { S_SIZE, S_OPACITY, -1 }, { TG_ARROW, -1, -1 } },
    [T_BOX] = { { S_SIZE, S_OPACITY, S_CORNER }, { TG_FILLED, -1, -1 } },
    [T_CIRCLE] = { { S_SIZE, S_OPACITY, -1 }, { TG_FILLED, -1, -1 } },
    [T_POLY] = { { S_SIZE, S_OPACITY, S_SIDES }, { TG_STAR, TG_FILLED, -1 } },
    [T_FILL] = { { S_OPACITY, S_TOL, -1 }, { TG_GLOBAL, TG_IMAGE, -1 } },
    [T_GRADIENT] = { { S_OPACITY, -1, -1 }, { TG_RADIAL, TG_FADE, -1 } },
    [T_TEXT] = { { S_SIZE, S_OPACITY, -1 }, { -1, -1, -1 } },                    /* (the rest of the text's settings are in the bar above the strip) */
    [T_PICK] = { { -1, -1, -1 }, { TG_IMAGE, -1, -1 } },
    [T_FREEPOLY] = { { S_SIZE, S_OPACITY, -1 }, { TG_FILLED, -1, -1 } },
    [T_CURVE] = { { S_SIZE, S_OPACITY, -1 }, { -1, -1, -1 } },
    [T_CLONE] = { { S_SIZE, S_OPACITY, S_HARD }, { TG_IMAGE, -1, -1 } },
    [T_SMUDGE] = { { S_SIZE, S_OPACITY, S_HARD }, { -1, -1, -1 } },
    [T_BLURBR] = { { S_SIZE, S_OPACITY, S_HARD }, { -1, -1, -1 } },
    [T_HAND] = { { -1, -1, -1 }, { -1, -1, -1 } },
};

#define N_RECENT 16
#define N_QUICK 24
static const uint32_t QUICK[N_QUICK] = {
    0x00000000, 0xff000000, 0xff404040, 0xff808080, 0xffc0c0c0, 0xffffffff, 0xff7f1d1d, 0xff795548,
    0xffe53935, 0xfffb8c00, 0xfffdd835, 0xffa4c639, 0xff43a047, 0xff00897b, 0xff29b6f6, 0xff1e88e5,
    0xff7e57c2, 0xffec407a, 0xffffcdd2, 0xffffe0b2, 0xfffff9c4, 0xffc8e6c9, 0xffb3e5fc, 0xffd1c4e9,
};

enum prompt { P_NONE, P_OPEN, P_SAVEAS, P_NEW, P_RESIZE, P_DISCARD, P_REPLACE, P_HELP, P_COLOUR, P_CANVAS, P_ADJUST, P_LAYERNAME, P_PLACE, P_FONT, P_EFFECTS };
enum after { AF_NONE, AF_OPEN, AF_NEW, AF_QUIT, AF_CLIPNEW };

/* The two rails' buttons. */
enum btn { B_SELECT, B_MOVE, B_PEN, B_RETOUCH, B_ERASER, B_SHAPES, B_FILL, B_GRADIENT, B_TEXT, B_PICK,        /* the left rail: the tools */
           B_NEW, B_OPEN, B_SAVE, B_UNDO, B_REDO, B_TRANSFORM, B_FX, B_CROP, B_FIT, B_HELP, B_COUNT };
#define N_LEFT 10
#define N_RIGHT 10

static bool tool_in_select(enum tool t) { return t == T_SELECT || t == T_ELLIPSE_SEL || t == T_LASSO || t == T_WAND; }
static bool tool_in_shapes(enum tool t) { return t == T_LINE || t == T_BOX || t == T_CIRCLE || t == T_POLY || t == T_FREEPOLY || t == T_CURVE; }

/* The rail buttons that stand for several tools: the tools of each, and the menu a click on the lit button opens. */
enum { G_SELECT, G_BRUSH, G_RETOUCH, G_SHAPES, G_MOVE, G_COUNT };
static const int GRP_BTN[G_COUNT] = { B_SELECT, B_PEN, B_RETOUCH, B_SHAPES, B_MOVE };
static const enum tool GRP_TOOLS[G_COUNT][6] = {
    { T_SELECT, T_ELLIPSE_SEL, T_LASSO, T_WAND },
    { T_PEN, T_SPRAY },
    { T_CLONE, T_SMUDGE, T_BLURBR },
    { T_LINE, T_BOX, T_CIRCLE, T_POLY, T_FREEPOLY, T_CURVE },
    { T_MOVE, T_HAND },
};
static const int GRP_N[G_COUNT] = { 4, 2, 3, 6, 2 };
static const char *const GRP_MENU[G_COUNT] = { "Rectangle  S\nEllipse  O\nLasso  Q\nMagic wand  W", "Pen  P\nAirbrush  A", "Clone stamp  K\nSmudge  R\nBlur brush  H",
                                              "Line  L\nBox  B\nCircle  C\nPolygon / star  N\nFree polygon  J\nCurve  V", "Move the layer or the selection  M\nHand: move the picture in the window  Y" };
#define MENU_GROUP0 10

static int group_of_button(int id)
{
    for (int g = 0; g < G_COUNT; g++)
        if (GRP_BTN[g] == id)
            return g;
    return -1;
}

static int group_of_tool(enum tool t)
{
    for (int g = 0; g < G_COUNT; g++)
        for (int i = 0; i < GRP_N[g]; i++)
            if (GRP_TOOLS[g][i] == t)
                return g;
    return -1;
}

/* The rail button that stands for a tool. */
static int tool_button(enum tool t)
{
    int g = group_of_tool(t);
    if (g >= 0)
        return GRP_BTN[g];
    switch (t) {
    case T_MOVE: return B_MOVE;
    case T_ERASER: return B_ERASER;
    case T_FILL: return B_FILL;
    case T_GRADIENT: return B_GRADIENT;
    case T_TEXT: return B_TEXT;
    default: return B_PICK;
    }
}

/* ---- undo: a layer's pixels, the whole document, or only the layers' settings ----------------------------------------------------- */

enum snapk { SK_PIX, SK_DOC, SK_PROPS, SK_MASK };

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
    uint32_t settings;                          /* mellagui_settings_v1: a copied picture goes to the compositor's clipboard through it */
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
    enum tool grp[G_COUNT];                     /* the variant each grouped rail button stands for */
    uint32_t fg, bg;
    uint32_t recent[N_RECENT];
    int nrecent;
    int val[S_COUNT];
    int tsize;                                  /* the size of text */
    char tfont_name[64];                        /* the text's font family, empty = the interface font */
    int talign;                                 /* 0 left, 1 centre, 2 right */
    int font_sel, font_top;                     /* the Font card: the chosen row and the first row shown */
    int font_grab;                              /* the font card's scroll bar thumb is held (where in it; -1 = not) */
    bool space_down;                            /* Space is held: the left button moves the picture whatever the tool */
    char font_find[24];                         /* what was typed to jump to a family ("rob" -> Roboto) and when the last letter came */
    int64_t font_find_ms;
    int tb_seen;                                /* the height of the text bar the view was last fitted for */
    double smx, smy;                            /* where the smoothed stroke is */
    int ratio;                                  /* the rectangle selection's ratio (RATIO_NAMES) */
    bool poly_on;                               /* a free polygon is being laid out: its corners so far */
    int poly_n;
    double poly_x[256], poly_y[256];
    bool curve_on;                              /* a curve: the two ends and the two handles; phase 0 draws the ends, 1 and 2 pull the handles */
    int curve_phase;
    double cv_pt[4][2];
    bool clone_have, clone_off_set;             /* the clone stamp: the spot it copies from and the shift from where the stroke began */
    double clone_sx, clone_sy, clone_ox, clone_oy;
    struct pix finger;                          /* the smudge: the colours the finger carries */
    int64_t last_click_ms;
    double last_click_x, last_click_y;
    bool rulers, snap;                          /* rulers and guides on the picture; tools snap to the guides, the edges and the middle */
    int ruler_unit, ruler_dpi;                  /* the rulers' unit (RU_*) and how many pixels make an inch (what mm, cm, in and pt are worth in pixels) */
    int nguides, guide_drag;
    struct { bool vert; int pos; } guide[32];
    int sel_mode;                               /* a selection being made: 0 replaces, 1 adds, 2 takes away, 3 keeps what both have */
    struct sel sel_base;                        /* ... and the selection it works on */
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
    int anim_delay, anim_loops;                 /* the animation card's answers ... */
    bool anim_bg, anim_ping, anim_go, anim_save;  /* ... and where it stands: go = ask for the file, save = the next Save as writes an animation */
    int gsel;                                   /* the group the panel has chosen (slot + 1; 0 = a layer is chosen): its opacity, blend mode and eye are what the panel's controls show
                                                   and the buttons act on the whole group; drawing on the picture goes back to the layer */
    int lmenu[16], nlmenu;                      /* what each entry of the open layer / group menu does (LA_* / GA_*) */
    int mask_edit;                              /* the layer whose MASK the tools paint on (-1: none, they paint on the layer) */
    int txt_edit;                               /* the text layer that is being typed into again (-1: a new text) */
    struct pix tx_orig;                         /* ... and its pixels as they were, for Esc */

    struct pix clipimg;                         /* Copy / Cut keep their picture here */

    enum prompt prompt;
    enum after after;                           /* what to do once a discard question was answered with "Discard" */
    char pbuf[PATH_MAX];                        /* the text field of a prompt */
    char perr[160];                             /* what was wrong with the last answer */
    int pcard_hover, pcard_down;
    int help_page, help_top, help_hover, help_btn;          /* the help card: the page, how far it is scrolled, what the pointer is on */
    bool help_drag;                             /* its scroll bar's thumb is held ... */
    int help_grab;                              /* ... and where in it */
    int lay_grab;                               /* the layers panel's scroll bar thumb is held (where in it; -1 = not) */
    struct wlpicker pk;                         /* the folder list of the Open / Save as card (wlpicker.h, the same in every program) */
    int menu_kind;                              /* the menu that is open: 0 none, 1 effects, 2 transform, 3 selects, 4 shapes, 5 blend, 6 layer */
    struct cpick cp;
    int anchor_x, anchor_y;                     /* the canvas card: where the picture sits (0..2) */
    bool canvas_fill_bg;                        /* ... and what the new room is */
    bool new_transparent;                       /* the New card: a transparent first layer */
    int fxk;                                    /* the effect whose card is open (K_*) and its sliders */
    int fxv[4];
    struct pix adj_before;                      /* the layer as it was when the card opened */
    struct sel fx_sel0;                         /* the selection as it was (selection effects) */
    int fx_newl;                                /* the layer an effect made for its result */
    struct snap fx_snap;                        /* the document before that layer was made: the undo step */
    bool fx_snap_ok;
    struct irect fx_box;                        /* what the last scale / turn covered */
    int adj_drag;
    int adj_x, adj_y;                           /* where the user put the effect panel (adj_placed), else it sits at the right */
    bool adj_placed, adj_moving;                /* it was moved; it is being dragged by its title */
    double adj_gx, adj_gy;                      /* where the pointer took hold of it (from its corner) */

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

/* What the tools draw on: the chosen layer's pixels, or its mask while that is being edited. */
/* The chosen layer's pixels of a plain document (no mask editing). */
#define IMD(d) ((d)->L[(d)->cur].im)
#define IM(a) (*((a)->mask_edit == (a)->doc.cur && (a)->doc.L[(a)->doc.cur].mask.p ? &(a)->doc.L[(a)->doc.cur].mask : &(a)->doc.L[(a)->doc.cur].im))
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
#define TB_H 84
/* The Text tool has a bar of its own (font, style, alignment, spacing, outline, shadow, turn) above the strip. */
#define BB_H 48
static bool brush_bar_tool(enum tool t) { return t == T_PEN || t == T_SPRAY || t == T_ERASER || t == T_LINE; }
struct app;
static int sym_count(const struct app *a);
static int tbar_h(const struct app *a) { return a->tool == T_TEXT ? TB_H : brush_bar_tool(a->tool) ? BB_H : 0; }
static int cv_h(const struct app *a) { return LM_MAX(a->win.bh - PAL_H - STAT_H - tbar_h(a), 1); }
static int pal_y(const struct app *a) { return a->win.bh - PAL_H - STAT_H; }
static int tb_y(const struct app *a) { return pal_y(a) - tbar_h(a); }
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
    if (a->gsel && (a->gsel > MAX_GROUPS || !a->doc.G[a->gsel - 1].used))
        a->gsel = 0;                                                    /* (the group went) */
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
    if (s->kind == SK_PIX || s->kind == SK_MASK)
        return (size_t)s->im.w * (size_t)s->im.h * 4;
    if (s->kind == SK_DOC && s->doc) {
        size_t b = 0;
        for (int i = 0; i < s->doc->n; i++)
            b += ((size_t)s->doc->L[i].im.w * (size_t)s->doc->L[i].im.h + (size_t)s->doc->L[i].mask.w * (size_t)s->doc->L[i].mask.h) * 4;
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
    if (kind == SK_MASK)
        return layer >= 0 && layer < a->doc.n && a->doc.L[layer].mask.p && pix_copy(&a->doc.L[layer].mask, &out->im);
    out->doc = malloc(sizeof(*out->doc));
    if (!out->doc)
        return false;
    if (kind == SK_DOC)
        return doc_copy(&a->doc, out->doc) || (free(out->doc), out->doc = NULL, false);
    *out->doc = a->doc;                                                 /* SK_PROPS: only what is not a pixel */
    for (int i = 0; i < out->doc->n; i++) {
        out->doc->L[i].im.p = NULL;
        out->doc->L[i].mask.p = NULL;
    }
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
    } else if (s->kind == SK_MASK) {
        if (s->layer >= 0 && s->layer < a->doc.n && a->doc.L[s->layer].mask.p && s->im.w == a->doc.L[s->layer].mask.w && s->im.h == a->doc.L[s->layer].mask.h) {
            pix_free(&a->doc.L[s->layer].mask);
            a->doc.L[s->layer].mask = s->im;
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
        for (int k = 0; k < MAX_GROUPS; k++)
            if (a->doc.G[k].used && s->doc->G[k].used) {
                memcpy(a->doc.G[k].name, s->doc->G[k].name, sizeof(a->doc.G[k].name));
                a->doc.G[k].opacity = s->doc->G[k].opacity;
                a->doc.G[k].blend = s->doc->G[k].blend;
                a->doc.G[k].visible = s->doc->G[k].visible;
                a->doc.G[k].collapsed = s->doc->G[k].collapsed;
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
static void checkpoint(struct app *a) { checkpoint_kind(a, a->mask_edit == a->doc.cur && a->doc.L[a->doc.cur].mask.p ? SK_MASK : SK_PIX); }
static void checkpoint_doc(struct app *a) { checkpoint_kind(a, SK_DOC); }
static void checkpoint_props(struct app *a) { checkpoint_kind(a, SK_PROPS); }

/* The same with a copy of `im` as it was (the layer is already changed: text, the Adjust card). */
static void checkpoint_with(struct app *a, const struct pix *im)
{
    struct snap sn;
    memset(&sn, 0, sizeof(sn));
    sn.kind = a->mask_edit == a->doc.cur && a->doc.L[a->doc.cur].mask.p ? SK_MASK : SK_PIX;
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
    if (top.kind != SK_MASK)
        a->mask_edit = -1;
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

static bool clipboard_image(struct app *a, struct pix *out);

/* A new picture made of what is on the clipboard (Ctrl+Shift+V): a screenshot, a picture copied somewhere else. */
static void new_from_clipboard(struct app *a)
{
    struct pix im = { 0 };
    if (!clipboard_image(a, &im)) {
        say(a, "No picture on the clipboard");
        return;
    }
    finish_pending(a);
    struct doc d;
    memset(&d, 0, sizeof(d));
    doc_from_pix(&d, &im);
    doc_free(&a->doc);
    a->doc = d;
    snprintf(a->doc.L[0].name, sizeof(a->doc.L[0].name), "Clipboard");
    drop_snaps(a->undo, &a->nundo);
    drop_snaps(a->redo, &a->nredo);
    a->path[0] = 0;
    a->modified = true;
    sel_clear(&a->sel);
    a->lay_top = 0;
    flat_mark_all(a);
    view_fit(a);
    say(a, "New picture from the clipboard, %d x %d", CW(a), CH(a));
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

/* ---- animations: the layers are the frames ------------------------------------------------------------------------------------------- */

static bool anim_name(const char *path)
{
    const char *dot = strrchr(path, '.');
    return dot && (!strcasecmp(dot, ".gif") || !strcasecmp(dot, ".webp"));
}

/* The items on the top level of the picture, bottom first: a layer, or a group with everything in it (a group is one frame). */
struct aitem {
    int lo, hi;
    const char *name;
    bool visible;
};

static int anim_items(const struct doc *d, struct aitem *it)
{
    int n = 0;
    for (int i = 0; i < d->n;) {
        int ch[MAX_GROUPS], cl = gchain(d, d->L[i].gid, ch), lo, hi;
        if (cl == 0) {
            it[n++] = (struct aitem){ i, i, d->L[i].name, d->L[i].visible };
            i++;
        } else if (group_range(d, ch[0], &lo, &hi)) {
            it[n++] = (struct aitem){ lo, hi, d->G[ch[0] - 1].name, d->G[ch[0] - 1].visible };
            i = hi + 1;
        } else
            i++;
    }
    return n;
}

/* Writes the visible layers, bottom first, as the frames of an animated GIF or WebP. A layer named "... @250" is shown 250 ms, the others `delay_ms`; with `bg` the
 * first visible layer stays under every frame; with `ping` the animation goes back again (without repeating its ends). */
static bool write_anim(const struct doc *d, const char *path, int delay_ms, int loops, bool bg, bool ping, int *nframes, char *err, size_t errsz)
{
    struct aitem items[MAX_LAYERS];
    int ni = anim_items(d, items), vis[MAX_LAYERS], nv = 0;
    for (int i = 0; i < ni; i++)
        if (items[i].visible)
            vis[nv++] = i;
    int first = bg ? 1 : 0, nf = nv - first;
    if (nf < 1 || nv < 2) {
        snprintf(err, errsz, "an animation needs at least two visible layers or groups (each is a frame)");
        return false;
    }
    int w = doc_w(d), h = doc_h(d);
    if ((uint64_t)w * (uint64_t)h * 4u * (uint64_t)(nf + 1) > (uint64_t)512 << 20) {
        snprintf(err, errsz, "the frames would take too much memory");
        return false;
    }
    struct pix *fr = calloc((size_t)nf, sizeof(*fr));
    int *dl = calloc((size_t)nf, sizeof(int));
    bool ok = fr && dl;
    for (int f = 0; f < nf && ok; f++) {
        const struct aitem *it = &items[vis[first + f]];
        uint8_t incl[MAX_LAYERS] = { 0 };
        if (bg)
            for (int i = items[vis[0]].lo; i <= items[vis[0]].hi; i++)
                incl[i] = 1;
        for (int i = it->lo; i <= it->hi; i++)
            incl[i] = 1;
        ok = pix_alloc(&fr[f], w, h, 0);
        if (ok)
            composite_px(d, incl, d->paper, &fr[f], (struct irect){ 0, 0, w, h });
        const char *at = strrchr(it->name, '@');
        int own = at ? atoi(at + 1) : 0;
        dl[f] = own > 0 ? LM_CLAMP(own, 10, 60000) : delay_ms;
    }
    int order[MAX_LAYERS * 2], no = 0;
    for (int f = 0; f < nf; f++)
        order[no++] = f;
    if (ping)
        for (int f = nf - 2; f >= 1; f--)
            order[no++] = f;
    int *delays = calloc((size_t)no, sizeof(int));
    const uint32_t **frames = calloc((size_t)no, sizeof(*frames));
    ok = ok && delays && frames;
    for (int k = 0; k < no && ok; k++) {
        frames[k] = fr[order[k]].p;
        delays[k] = dl[order[k]];
    }
    size_t n = 0;
    uint8_t *file = NULL;
    if (ok) {
        const char *dot = strrchr(path, '.');
        if (dot && !strcasecmp(dot, ".webp"))
            file = lm_enc_webp_anim(frames, no, w, h, delays, loops, &n);
        else {
            int *cs = calloc((size_t)no, sizeof(int));
            if (cs) {
                for (int k = 0; k < no; k++)
                    cs[k] = LM_MAX((delays[k] + 5) / 10, 2);
                file = lm_enc_gif_anim(frames, no, w, h, cs, loops, &n);
                free(cs);
            }
        }
    }
    if (!file)
        snprintf(err, errsz, "%s", ok ? "could not make the animation (too big?)" : "out of memory");
    else {
        char tmp[PATH_MAX + 32];
        snprintf(tmp, sizeof(tmp), "%s.paint-%ld.tmp", path, (long)getpid());
        int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
        bool wrote = fd >= 0;
        for (size_t off = 0; wrote && off < n;) {
            ssize_t k = write(fd, file + off, n - off);
            if (k <= 0)
                wrote = false;
            else
                off += (size_t)k;
        }
        if (fd >= 0 && close(fd) != 0)
            wrote = false;
        if (wrote && rename(tmp, path) != 0)
            wrote = false;
        if (!wrote) {
            snprintf(err, errsz, "%s", strerror(errno));
            unlink(tmp);
            free(file);
            file = NULL;
        }
    }
    bool done = file != NULL;
    if (nframes)
        *nframes = no;
    free(file);
    for (int f = 0; f < nf && fr; f++)
        pix_free(&fr[f]);
    free(fr);
    free(dl);
    free(delays);
    free(frames);
    return done;
}

static bool write_image(struct app *a, const char *path)
{
    char err[160] = "";
    finish_pending(a);
    if (a->anim_save && anim_name(path)) {                              /* from the Animation card: the layers are written as an animation, the picture stays as it is */
        int nf = 0;
        if (!write_anim(&a->doc, path, a->anim_delay, a->anim_loops, a->anim_bg, a->anim_ping, &nf, err, sizeof(err))) {
            say(a, "Could not save the animation: %s", err);
            snprintf(a->perr, sizeof(a->perr), "%s", err);
            return false;
        }
        a->anim_save = false;
        const char *bn = strrchr(path, '/');
        say(a, "Saved the animation %s: %d frames", bn ? bn + 1 : path, nf);
        return true;
    }
    a->anim_save = false;
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
    if (path != a->path)                                                /* (Ctrl+S passes the name it already has: copying a string onto itself empties it) */
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
    doc_bake_masks(&a->doc);
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

/* How the selection being made joins the one there was (Shift or the Add switch: adds; Alt or Subtract: takes away; Ctrl: keeps what both have). */
static int sel_mode_now(const struct app *a)
{
    uint32_t m = a->win.mods;
    if (m & LM_MOD_ALT)
        return 2;
    if (m & LM_MOD_CTRL)
        return 3;
    if ((m & LM_MOD_SHIFT) || a->tg[TG_ADD])
        return 1;
    return a->tg[TG_SUB] ? 2 : 0;
}

static bool sel_dup(struct sel *to, const struct sel *from);

/* The selection becomes what `region` covers inside `rbox`, put together with the old selection (a->sel_base) as the mode says. */
static void sel_combine(struct app *a, const struct cov *region, struct irect rbox)
{
    if (a->sel_mode == 0 || !a->sel_base.on) {
        sel_set_cov(a, region, rbox);
        return;
    }
    int w = CW(a), h = CH(a);
    struct irect u = rbox;
    irect_add(&u, a->sel_base.r.x0, a->sel_base.r.y0, a->sel_base.r.x1, a->sel_base.r.y1);
    irect_clip(&u, w, h);
    struct cov out;
    if (!cov_alloc(&out, w, h))
        return;
    for (int y = u.y0; y < u.y1; y++)
        for (int x = u.x0; x < u.x1; x++) {
            int b = sel_at(&a->sel_base, x, y), g = x >= rbox.x0 && x < rbox.x1 && y >= rbox.y0 && y < rbox.y1 ? region->p[(size_t)y * (size_t)w + (size_t)x] : 0;
            out.p[(size_t)y * (size_t)w + (size_t)x] = (uint8_t)(a->sel_mode == 1 ? LM_MAX(b, g) : a->sel_mode == 2 ? b * (255 - g) / 255 : LM_MIN(b, g));
        }
    sel_set_cov(a, &out, u);
    cov_free(&out);
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

/* ---- the system clipboard's pictures --------------------------------------------------------------------------------------------- */

/* The compositor's clipboard takes a picture as a PNG file (the settings value `clip-png`, the same way the screenshot tool copies): the file is read while
 * the call is answered, so it can go at once. */
static void clip_to_system(struct app *a, const struct pix *img)
{
    if (!a->settings || !img->p)
        return;
    const char *rt = getenv("XDG_RUNTIME_DIR");
    char tmp[PATH_MAX], err[160];
    snprintf(tmp, sizeof(tmp), "%s/mellagui-paint-%d.png", rt && *rt ? rt : "/tmp", (int)getpid());
    if (save_file(img, tmp, err, sizeof(err))) {
        WLC_SEND(a->win.c, a->settings, LM_MELLAGUI_SETTINGS_V1_REQ_SET, { .s = "clip-png" }, { .s = tmp });
        wlc_roundtrip(a->win.c);
    }
    unlink(tmp);
}

/* The picture on the system clipboard (a screenshot, something copied in a browser or another program), or false when there is none. */
static bool clipboard_image(struct app *a, struct pix *out)
{
    if (!wlclip_has_mime(&a->clip, "image/png"))
        return false;
    size_t n = 0;
    uint8_t *png = wlclip_paste_mime(&a->clip, "image/png", 64u << 20, &n);
    if (!png)
        return false;
    const char *rt = getenv("XDG_RUNTIME_DIR");
    char tmp[PATH_MAX], err[160];
    snprintf(tmp, sizeof(tmp), "%s/mellagui-paint-in-%d.png", rt && *rt ? rt : "/tmp", (int)getpid());
    bool ok = false;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd >= 0) {
        size_t off = 0;
        while (off < n) {
            ssize_t k = write(fd, png + off, n - off);
            if (k <= 0)
                break;
            off += (size_t)k;
        }
        close(fd);
        ok = off == n && load_file(tmp, out, err, sizeof(err));
    }
    unlink(tmp);
    free(png);
    return ok;
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
    clip_to_system(a, &a->clipimg);
    if (cut)
        do_delete_sel(a);
    say(a, "%s %d x %d", cut ? "Cut" : "Copied", img.w, img.h);
}

static void tool_set(struct app *a, enum tool t);

/* Paste: the picture becomes a new layer at the top left of what is in view; the Move tool is chosen so that it can be put in place. */
static void do_paste(struct app *a)
{
    finish_pending(a);
    struct pix sys = { 0 };
    if (clipboard_image(a, &sys)) {                                     /* a picture from the system clipboard is the newer copy */
        pix_free(&a->clipimg);
        a->clipimg = sys;
    }
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
    if (i < 0 || i >= a->doc.n)
        return;
    if (i == a->doc.cur && !a->gsel)
        return;
    finish_pending(a);
    a->gsel = 0;
    a->doc.cur = i;
    a->mask_edit = -1;
    a->win.redraw = true;
}

static void layer_add(struct app *a)
{
    finish_pending(a);
    a->mask_edit = -1;
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
    a->gsel = 0;
    changed(a);
    say(a, "New layer");
}

static void layer_dup(struct app *a)
{
    finish_pending(a);
    a->mask_edit = -1;
    if (a->gsel) {                                                      /* the whole group */
        checkpoint_doc(a);
        int ng = doc_dup_group(&a->doc, a->gsel);
        if (!ng) {
            snap_free(&a->undo[--a->nundo]);
            say(a, "Not possible: there is no room for the copy (%d layers, %d groups at most)", MAX_LAYERS, MAX_GROUPS);
            return;
        }
        a->gsel = ng;                                                   /* (the copy is the one chosen) */
        changed(a);
        say(a, "Group duplicated");
        return;
    }
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
    a->mask_edit = -1;
    if (a->gsel) {                                                      /* the group and what is in it */
        checkpoint_doc(a);
        if (!doc_delete_group(&a->doc, a->gsel)) {
            snap_free(&a->undo[--a->nundo]);
            say(a, "The last layers stay");
            return;
        }
        a->gsel = 0;
        changed(a);
        say(a, "Group deleted with its layers");
        return;
    }
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
    a->mask_edit = -1;
    checkpoint_doc(a);
    if (!(a->gsel ? doc_move_item(&a->doc, 0, a->gsel, dir) : doc_move(&a->doc, dir))) {
        snap_free(&a->undo[--a->nundo]);                               /* (nothing moved: no step) */
        say(a, dir > 0 ? "Already the top of its group" : "Already the bottom of its group");
        return;
    }
    changed(a);
}

static void layer_merge(struct app *a)
{
    finish_pending(a);
    a->mask_edit = -1;
    if (a->gsel) {                                                      /* the group becomes one layer */
        checkpoint_doc(a);
        if (!doc_merge_group(&a->doc, a->gsel)) {
            snap_free(&a->undo[--a->nundo]);
            say(a, "Not possible (out of memory?)");
            return;
        }
        a->gsel = 0;
        changed(a);
        say(a, "The group is one layer now");
        return;
    }
    if (a->doc.cur < 1 || a->doc.L[a->doc.cur].gid != a->doc.L[a->doc.cur - 1].gid) {
        say(a, "There is no layer below in the same group to merge into");
        return;
    }
    checkpoint_doc(a);
    doc_merge_down(&a->doc);
    changed(a);
    say(a, "Merged down");
}

/* A mask for the chosen layer: all white (everything shows), or from the selection (inside shows, outside is hidden). The tools paint on it from then on. */
static void mask_add(struct app *a)
{
    finish_pending(a);
    struct layer *l = &a->doc.L[a->doc.cur];
    if (l->mask.p) {
        say(a, "The layer has a mask already");
        return;
    }
    struct pix m;
    if (!pix_alloc(&m, CW(a), CH(a), 0xffffffffu)) {
        say(a, "Not possible (out of memory?)");
        return;
    }
    checkpoint_doc(a);
    bool from_sel = a->sel.on;
    if (from_sel)
        for (int y = 0; y < m.h; y++)
            for (int x = 0; x < m.w; x++) {
                int v = sel_at(&a->sel, x, y);
                m.p[(size_t)y * (size_t)m.w + (size_t)x] = argb(255, v, v, v);
            }
    l->mask = m;
    a->mask_edit = a->doc.cur;
    sel_clear(&a->sel);
    changed(a);
    say(a, from_sel ? "Mask made from the selection: paint black to hide, white to show" : "Mask added: paint black to hide, white to show");
}

static void mask_edit_toggle(struct app *a)
{
    finish_pending(a);
    if (!a->doc.L[a->doc.cur].mask.p) {
        say(a, "The layer has no mask: add one from the layer's menu (right click)");
        return;
    }
    a->mask_edit = a->mask_edit == a->doc.cur ? -1 : a->doc.cur;
    say(a, a->mask_edit >= 0 ? "Painting on the mask: black hides, white shows" : "Painting on the layer");
    a->win.redraw = true;
}

static void mask_apply(struct app *a)
{
    finish_pending(a);
    if (!a->doc.L[a->doc.cur].mask.p)
        return;
    checkpoint_doc(a);
    layer_bake_mask(&a->doc.L[a->doc.cur]);
    a->mask_edit = -1;
    changed(a);
    say(a, "The mask is part of the layer now");
}

static void mask_delete(struct app *a)
{
    finish_pending(a);
    if (!a->doc.L[a->doc.cur].mask.p)
        return;
    checkpoint_doc(a);
    pix_free(&a->doc.L[a->doc.cur].mask);
    a->mask_edit = -1;
    changed(a);
    say(a, "Mask deleted: the whole layer shows");
}

static void layer_toggle(struct app *a, int i);
static void prompt_open(struct app *a, enum prompt p, const char *initial);
static void mask_add(struct app *a);
static void mask_edit_toggle(struct app *a);
static void mask_apply(struct app *a);
static void mask_delete(struct app *a);

/* ---- groups: what the panel does with them ------------------------------------------------------------------------------------------- */

/* The chosen layer (or the chosen group) goes into a new group of its own. */
static void group_make(struct app *a)
{
    finish_pending(a);
    a->mask_edit = -1;
    if (groups_used(&a->doc) >= MAX_GROUPS) {
        say(a, "No more than %d groups", MAX_GROUPS);
        return;
    }
    checkpoint_doc(a);
    if (a->gsel) {                                                      /* a group in a new group */
        int slot = g_new_slot(&a->doc);
        group_init(&a->doc.G[slot - 1], "Group", a->doc.G[a->gsel - 1].parent);
        snprintf(a->doc.G[slot - 1].name, sizeof(a->doc.G[0].name), "Group %d", groups_used(&a->doc));
        a->doc.G[a->gsel - 1].parent = slot;
        a->gsel = slot;
    } else {
        doc_group_layer(&a->doc);
        a->gsel = a->doc.L[a->doc.cur].gid;
    }
    changed(a);
    say(a, "A group: Up / Down, the menu (right click) and New work on the whole group; double click renames it");
}

static void group_ungroup(struct app *a)
{
    finish_pending(a);
    if (!a->gsel)
        return;
    checkpoint_doc(a);
    doc_ungroup(&a->doc, a->gsel);
    a->gsel = 0;
    changed(a);
    say(a, "Ungrouped: the layers stay");
}

/* The chosen item leaves its group (out at the top of it), or goes into the group above (dir > 0) or below it. */
static void group_move_in_out(struct app *a, int what, int dir)
{
    finish_pending(a);
    a->mask_edit = -1;
    checkpoint_doc(a);
    int layer = a->gsel ? 0 : a->doc.cur, grp = a->gsel;
    bool ok = what == 0 ? doc_leave_group(&a->doc, layer, grp) : doc_enter_group(&a->doc, layer, grp, dir);
    if (!ok) {
        snap_free(&a->undo[--a->nundo]);
        say(a, what == 0 ? "It is not in a group" : dir > 0 ? "There is no group right above it" : "There is no group right below it");
        return;
    }
    changed(a);
    say(a, what == 0 ? "Taken out of the group" : "Put into the group");
}

static void group_toggle_visible(struct app *a, int g)
{
    if (g < 1 || g > MAX_GROUPS || !a->doc.G[g - 1].used)
        return;
    checkpoint_props(a);
    a->doc.G[g - 1].visible = !a->doc.G[g - 1].visible;
    changed(a);
}

static void group_toggle_fold(struct app *a, int g)
{
    if (g < 1 || g > MAX_GROUPS || !a->doc.G[g - 1].used)
        return;
    a->doc.G[g - 1].collapsed = !a->doc.G[g - 1].collapsed;
    a->modified = true;
    a->win.redraw = true;
}

/* The group's row was chosen: the panel's controls are the group's now, the chosen layer is the highest one in it. */
static void group_select(struct app *a, int g)
{
    int lo, hi;
    if (g < 1 || g > MAX_GROUPS || !a->doc.G[g - 1].used || !group_range(&a->doc, g, &lo, &hi))
        return;
    finish_pending(a);
    a->gsel = g;
    a->doc.cur = hi;
    a->mask_edit = -1;
    a->win.redraw = true;
}

enum { LA_RENAME, LA_DUP, LA_DEL, LA_MERGE, LA_SHOW, LA_MASK_ADD, LA_MASK_EDIT, LA_MASK_APPLY, LA_MASK_DEL, LA_GROUP, LA_OUT, LA_IN_ABOVE, LA_IN_BELOW, LA_UNGROUP, LA_FOLD, LA_COUNT };

static const char *const LA_LABEL[2][LA_COUNT] = {
    { "Rename...", "Duplicate", "Delete", "Merge down", "Show / hide", "Add a mask", "Edit the mask / the layer", "Apply the mask", "Delete the mask", "Group the layer", "Take out of the group",
      "Into the group above", "Into the group below", "Ungroup", "Fold / unfold" },
    { "Rename...", "Duplicate the group", "Delete the group and its layers", "Merge the group into one layer", "Show / hide", "Add a mask", "Edit the mask / the layer", "Apply the mask",
      "Delete the mask", "Put the group in a new group", "Take out of the group", "Into the group above", "Into the group below", "Ungroup", "Fold / unfold" },
};

/* The menu of a right click on a layer (grp = false) or a group's row: only what applies. */
static bool item_menu(struct app *a, bool grp)
{
    char buf[700] = "";
    int lo, hi, level, slo, shi, sg;
    a->nlmenu = 0;
#define LADD(x)                                                                                                                                                       \
    do {                                                                                                                                                              \
        a->lmenu[a->nlmenu++] = (x);                                                                                                                                  \
        if (a->nlmenu > 1)                                                                                                                                            \
            strncat(buf, "\n", sizeof(buf) - strlen(buf) - 1);                                                                                                        \
        strncat(buf, LA_LABEL[grp][x], sizeof(buf) - strlen(buf) - 1);                                                                                                \
    } while (0)
    LADD(LA_RENAME);
    if (grp)
        LADD(LA_UNGROUP);
    LADD(LA_DUP);
    LADD(LA_DEL);
    LADD(LA_MERGE);
    if (grp)
        LADD(LA_FOLD);
    LADD(LA_SHOW);
    if (!grp) {
        if (a->doc.L[a->doc.cur].mask.p) {
            LADD(LA_MASK_EDIT);
            LADD(LA_MASK_APPLY);
            LADD(LA_MASK_DEL);
        } else
            LADD(LA_MASK_ADD);
    }
    LADD(LA_GROUP);
    if (item_block(&a->doc, a->doc.cur, grp ? a->gsel : 0, &lo, &hi, &level)) {
        if (level)
            LADD(LA_OUT);
        if (item_sibling(&a->doc, lo, hi, level, 1, &slo, &shi, &sg) && sg)
            LADD(LA_IN_ABOVE);
        if (item_sibling(&a->doc, lo, hi, level, -1, &slo, &shi, &sg) && sg)
            LADD(LA_IN_BELOW);
    }
#undef LADD
    if (!wlwin_menu(&a->win, buf))
        return false;
    a->menu_kind = grp ? 8 : 6;
    return true;
}

static void item_action(struct app *a, int act)
{
    switch (act) {
    case LA_RENAME:
        prompt_open(a, P_LAYERNAME, a->gsel ? a->doc.G[a->gsel - 1].name : a->doc.L[a->doc.cur].name);
        wltext_select_all(a->pbuf);
        break;
    case LA_DUP: layer_dup(a); break;
    case LA_DEL: layer_delete(a); break;
    case LA_MERGE: layer_merge(a); break;
    case LA_SHOW:
        if (a->gsel)
            group_toggle_visible(a, a->gsel);
        else
            layer_toggle(a, a->doc.cur);
        break;
    case LA_MASK_ADD: mask_add(a); break;
    case LA_MASK_EDIT: mask_edit_toggle(a); break;
    case LA_MASK_APPLY: mask_apply(a); break;
    case LA_MASK_DEL: mask_delete(a); break;
    case LA_GROUP: group_make(a); break;
    case LA_OUT: group_move_in_out(a, 0, 0); break;
    case LA_IN_ABOVE: group_move_in_out(a, 1, 1); break;
    case LA_IN_BELOW: group_move_in_out(a, 1, -1); break;
    case LA_UNGROUP: group_ungroup(a); break;
    case LA_FOLD: group_toggle_fold(a, a->gsel); break;
    default: break;
    }
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
    if (a->gsel)
        snprintf(a->doc.G[a->gsel - 1].name, sizeof(a->doc.G[0].name), "%.39s", name);
    else
        snprintf(a->doc.L[a->doc.cur].name, sizeof(a->doc.L[0].name), "%.39s", name);
    a->modified = true;
    a->win.redraw = true;
}

/* Turns every layer (the whole picture): `fn` runs on each. */
static bool all_layers(struct app *a, bool (*fn)(struct pix *, void *), void *ud)
{
    doc_bake_masks(&a->doc);
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

/* The fonts the text tool can use: every TrueType family found in the font folders (`$MELLAGUI_FONT_DIRS` replaces the list), with up to four files each
 * (regular, bold, italic, bold italic); a style a family does not have is made from the regular one. The first choice, "Interface font", is the program's own labels. */
struct tfam {
    char name[64];
    char file[4][PATH_MAX];
    struct lm_ttf *ft[4];
    bool tried[4];
};

static struct tfam *g_fam;                                              /* (grows: a collection of free fonts has a hundred families and more) */
static int g_nfam, g_fam_cap;
static bool g_fam_scanned;

static void fam_add(const char *path, const struct font_info *fi)
{
    if (!fi->family[0])
        return;
    bool bold = fi->weight >= 600 || strcasestr(fi->style, "bold"), italic = fi->italic || strcasestr(fi->style, "italic") || strcasestr(fi->style, "oblique");
    int slot = (bold ? 1 : 0) + (italic ? 2 : 0), at = -1;
    for (int i = 0; i < g_nfam; i++)
        if (!strcmp(g_fam[i].name, fi->family))
            at = i;
    if (at < 0) {
        if (g_nfam >= g_fam_cap) {
            struct tfam *bigger = realloc(g_fam, (size_t)(g_fam_cap + 64) * sizeof(*g_fam));
            if (!bigger)
                return;
            g_fam = bigger;
            g_fam_cap += 64;
        }
        at = g_nfam++;
        memset(&g_fam[at], 0, sizeof(g_fam[at]));
        snprintf(g_fam[at].name, sizeof(g_fam[at].name), "%.63s", fi->family);
    }
    if (!g_fam[at].file[slot][0])
        snprintf(g_fam[at].file[slot], PATH_MAX, "%s", path);
}

static void fonts_scan_dir(const char *dir, int depth, int *budget)
{
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) && *budget > 0) {
        if (e->d_name[0] == '.')
            continue;
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < 5)
                fonts_scan_dir(path, depth + 1, budget);
            continue;
        }
        const char *dot = strrchr(e->d_name, '.');
        if (!dot || strcasecmp(dot, ".ttf") || st.st_size > (24 << 20))
            continue;
        (*budget)--;
        struct font_info fi;
        char err[120];
        if (font_read_names(path, &fi, err, sizeof(err)) == 0) {
            bool ok = !fi.cff && !fi.woff;                              /* (the outlines have to be TrueType: lm_ttf_open below says whether they can be painted) */
            static const char *const symbol[] = { "cmex", "cmmi", "cmsy", "msam", "msbm", "wasy", "esint", "stmary", "rsfs", "eufm", "dsrom", "lasy" };
            for (size_t k = 0; ok && k < sizeof(symbol) / sizeof(symbol[0]); k++)
                ok = strncasecmp(fi.family, symbol[k], strlen(symbol[k])) != 0;
            if (ok) {                                                   /* a font for text has every letter and digit (symbol fonts do not) */
                struct lm_ttf *t = lm_ttf_open(path, NULL);
                ok = t != NULL;
                for (const char *c = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"; ok && *c; c++)
                    ok = lm_ttf_glyph(t, (unsigned char)*c) != 0;
                if (t)
                    lm_ttf_close(t);
            }
            if (ok)
                fam_add(path, &fi);
            font_free(&fi);
        }
    }
    closedir(d);
}

static int fam_cmp(const void *a, const void *b)
{
    return strcasecmp(((const struct tfam *)a)->name, ((const struct tfam *)b)->name);
}

static void fonts_scan(void)
{
    if (g_fam_scanned)
        return;
    g_fam_scanned = true;
    const char *env = getenv("MELLAGUI_FONT_DIRS"), *home = getenv("HOME");
    char list[2048] = "";
    if (env && *env)
        snprintf(list, sizeof(list), "%s", env);
    else {
        snprintf(list, sizeof(list), "/usr/share/fonts:/usr/local/share/fonts:%s/.local/share/fonts:%s/.fonts", home ? home : "", home ? home : "");
    }
    int budget = 4000;                                                  /* font files looked at in all */
    for (char *save = NULL, *dir = strtok_r(list, ":", &save); dir; dir = strtok_r(NULL, ":", &save))
        if (*dir)
            fonts_scan_dir(dir, 0, &budget);
    qsort(g_fam, (size_t)g_nfam, sizeof(g_fam[0]), fam_cmp);
}

static int fam_find(const char *name)
{
    fonts_scan();
    for (int i = 0; i < g_nfam; i++)
        if (!strcasecmp(g_fam[i].name, name))
            return i;
    return -1;
}

/* The font file of a family for a style, and whether bold / italic still have to be made from it. */
static struct lm_ttf *fam_font(int fam, bool bold, bool italic, bool *synth_bold, bool *synth_italic)
{
    struct tfam *f = &g_fam[fam];
    int want = (bold ? 1 : 0) + (italic ? 2 : 0), slot = -1;
    int order[4][4] = { { 0, 1, 2, 3 }, { 1, 0, 3, 2 }, { 2, 0, 3, 1 }, { 3, 1, 2, 0 } };
    for (int k = 0; k < 4 && slot < 0; k++)
        if (f->file[order[want][k]][0])
            slot = order[want][k];
    if (slot < 0)
        return NULL;
    *synth_bold = bold && !(slot & 1);
    *synth_italic = italic && !(slot & 2);
    if (!f->tried[slot]) {
        f->tried[slot] = true;
        f->ft[slot] = lm_ttf_open(f->file[slot], NULL);
    }
    return f->ft[slot];
}

/* What draws the letters: a TrueType file at a size, or the program's own labels. */
struct tprov {
    struct lm_ttf *f;                   /* NULL = the interface font */
    double sx;                          /* pixels per font unit */
    double asc, desc, lh0;              /* the baseline's depth below the line's top, the part below it, the line's height (pixels) */
    double scale;                       /* the label font's scale */
    bool sb, si;                        /* bold / italic to make by hand */
    bool bold;                          /* ask the label font for its bold face */
    int size;
};

static void tprov_init(struct tprov *p, int fam, bool bold, bool italic, int size)
{
    memset(p, 0, sizeof(*p));
    p->size = size;
    if (fam >= 0 && fam < g_nfam) {
        bool sb = false, si = false;
        struct lm_ttf *f = fam_font(fam, bold, italic, &sb, &si);
        int units = f ? lm_ttf_units(f) : 0;
        if (f && units > 0) {
            p->f = f;
            p->sx = (double)size / units;
            p->asc = lm_ttf_ascent(f) * p->sx;
            p->desc = -lm_ttf_descent(f) * p->sx;
            p->lh0 = p->asc + p->desc;
            p->sb = sb;
            p->si = si;
            return;
        }
    }
    p->scale = LM_MAX(size / (double)LM_FONT_H, 0.5);
    p->lh0 = ceil(LM_FONT_H * p->scale);
    p->asc = p->lh0 * 0.8;
    p->desc = p->lh0 - p->asc;
    p->bold = bold;
    p->si = italic;
}

static double tprov_adv(const struct tprov *p, uint32_t cp)
{
    if (p->f) {
        int g = lm_ttf_glyph(p->f, cp);
        if (!g)
            g = lm_ttf_glyph(p->f, ' ');
        return g ? lm_ttf_advance(p->f, g) * p->sx : p->size * 0.5;
    }
    struct lm_font_glyph gl;
    lm_font_label_scaled(cp, p->scale, p->bold, &gl);
    return gl.advance;
}

/* `src` (sw x sh coverage) laid into `ink` with its top left at (dx, dy), the biggest of what is there and what comes; the rows lean to the right above row `base` by `slant` a row. */
static void blit_cov(uint8_t *ink, int iw, int ih, const uint8_t *src, int sw, int sh, int dx, int dy, double base, double slant)
{
    for (int y = 0; y < sh; y++) {
        int ty = dy + y;
        if (ty < 0 || ty >= ih)
            continue;
        double shift = slant > 0 ? (base - y) * slant : 0.0;
        for (int x = 0; x < sw + 3; x++) {
            int tx = dx + x;
            if (tx < 0 || tx >= iw)
                continue;
            double sxp = x - shift;
            int i0 = (int)floor(sxp);
            double f = sxp - i0;
            int a = i0 >= 0 && i0 < sw ? src[(size_t)y * (size_t)sw + (size_t)i0] : 0, b = i0 + 1 >= 0 && i0 + 1 < sw ? src[(size_t)y * (size_t)sw + (size_t)(i0 + 1)] : 0;
            int v = (int)(a * (1.0 - f) + b * f + 0.5);
            if (v > ink[(size_t)ty * (size_t)iw + (size_t)tx])
                ink[(size_t)ty * (size_t)iw + (size_t)tx] = (uint8_t)v;
        }
    }
}

/* One letter into `ink` with its pen at x = penx and its line's top at `top`. */
static void tprov_draw(const struct tprov *p, uint32_t cp, uint8_t *ink, int iw, int ih, double penx, int top)
{
    if (cp == ' ')
        return;
    if (!p->f) {
        struct lm_font_glyph g;
        lm_font_label_scaled(cp, p->scale, p->bold, &g);
        if (g.px)
            blit_cov(ink, iw, ih, g.px, g.w, g.h, (int)floor(penx) - g.left, top, g.h * 0.8, p->si ? 0.21 : 0.0);
        return;
    }
    int gl = lm_ttf_glyph(p->f, cp);
    if (!gl)
        return;
    double adv = lm_ttf_advance(p->f, gl) * p->sx;
    int mg = (int)ceil(p->size * 0.4) + 3, gw = (int)ceil(adv) + 2 * mg, gh = (int)ceil(p->asc + p->desc) + 4;
    if (gw < 1 || gh < 1 || (int64_t)gw * gh > (1 << 22))
        return;
    uint8_t *tmp = malloc((size_t)gw * (size_t)gh), *more = p->sb ? malloc((size_t)gw * (size_t)gh) : NULL;
    if (!tmp || (p->sb && !more)) {
        free(tmp);
        free(more);
        return;
    }
    double frac = penx - floor(penx), base = p->asc + 2;
    lm_ttf_render(p->f, gl, p->sx, p->sx, mg + frac, base, tmp, gw, gh);
    if (p->sb) {                                                        /* bold by hand: the same letter a little to the right, again */
        int steps = LM_MAX(p->size / 20, 1);
        for (int k = 1; k <= steps; k++) {
            lm_ttf_render(p->f, gl, p->sx, p->sx, mg + frac + k * 0.8, base, more, gw, gh);
            for (size_t i = 0; i < (size_t)gw * (size_t)gh; i++)
                if (more[i] > tmp[i])
                    tmp[i] = more[i];
        }
    }
    blit_cov(ink, iw, ih, tmp, gw, gh, (int)floor(penx) - mg, top - 2, base, p->si ? 0.21 : 0.0);
    free(tmp);
    free(more);
}

/* A coverage picture grown by `r` pixels: everything within r of a pixel with at least `thr` coverage is covered (softly at the edge); the distance is the
 * two pass chamfer approximation, so it is quick on a big picture. */
static void dilate_cov_thr(const uint8_t *src, uint8_t *dst, int w, int h, int r, int thr)
{
    size_t n = (size_t)w * (size_t)h;
    float *d = malloc(n * sizeof(float));
    if (!d) {
        memcpy(dst, src, n);
        return;
    }
    for (size_t i = 0; i < n; i++)
        d[i] = src[i] >= thr ? 0.0f : 1e9f;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            size_t i = (size_t)y * (size_t)w + (size_t)x;
            float v = d[i];
            if (x > 0 && d[i - 1] + 1 < v)
                v = d[i - 1] + 1;
            if (y > 0) {
                if (d[i - w] + 1 < v)
                    v = d[i - w] + 1;
                if (x > 0 && d[i - w - 1] + 1.4142f < v)
                    v = d[i - w - 1] + 1.4142f;
                if (x < w - 1 && d[i - w + 1] + 1.4142f < v)
                    v = d[i - w + 1] + 1.4142f;
            }
            d[i] = v;
        }
    for (int y = h - 1; y >= 0; y--)
        for (int x = w - 1; x >= 0; x--) {
            size_t i = (size_t)y * (size_t)w + (size_t)x;
            float v = d[i];
            if (x < w - 1 && d[i + 1] + 1 < v)
                v = d[i + 1] + 1;
            if (y < h - 1) {
                if (d[i + w] + 1 < v)
                    v = d[i + w] + 1;
                if (x < w - 1 && d[i + w + 1] + 1.4142f < v)
                    v = d[i + w + 1] + 1.4142f;
                if (x > 0 && d[i + w - 1] + 1.4142f < v)
                    v = d[i + w - 1] + 1.4142f;
            }
            d[i] = v;
            float c = r + 0.5f - v;
            dst[i] = c <= 0 ? 0 : c >= 1 ? 255 : (uint8_t)(c * 255);
        }
    free(d);
}

static void dilate_cov(const uint8_t *src, uint8_t *dst, int w, int h, int r)
{
    dilate_cov_thr(src, dst, w, h, r, 1);
}

static void blur_plane(uint8_t *p, int w, int h, int r, int passes);

/* A box blur of radius r (the shadow of the text). */
static void blur_cov(uint8_t *buf, int w, int h, int r)
{
    blur_plane(buf, w, h, r, 2);
}

#define TEXT_LINES 400

/* Where a text lies: its box, the caret and the turn, all in picture pixels. */
struct tmetrics {
    double w, h;                        /* the box of the text (without room for the outline or the shadow) */
    double lh;                          /* a line's height */
    double cax, cay;                    /* the caret, from the box's top left */
    double ax, ay;                      /* the box's top left, unturned */
    double ccx, ccy;                    /* the centre the text is turned about */
    double ang;                         /* the turn, radians (clockwise on the screen) */
    int ox, oy;                         /* where the finished picture of the text lies */
    double bR;                          /* the bend: the radius of the arc the text is laid on (0 = straight), its side (+1 arches over, -1 sags) */
    int bs, bpad;
    double bw0, bh0;                    /* the straight picture's size, which the bend maps */
};

/* A point of the straight picture of the text (from its middle) goes onto the arc: where it lies when the text is bent. */
static void bend_point(double R, int s, double u, double v, double *X, double *Y)
{
    double phi = u / R, r = s > 0 ? R - v : R + v;
    if (r < 0)
        r = 0;
    *X = r * sin(phi);
    *Y = s > 0 ? -r * cos(phi) : r * cos(phi);
}

static void tm_to_pic(const struct tmetrics *m, double lx, double ly, double *px, double *py)
{
    if (m->bs) {                                                        /* bent text: the point moves onto the arc first */
        double X, Y;
        bend_point(m->bR, m->bs, m->bpad + lx - m->bw0 / 2.0, m->bpad + ly - m->bh0 / 2.0, &X, &Y);
        lx = X - m->bpad + m->bw0 / 2.0;
        ly = Y + m->bs * m->bR - m->bpad + m->bh0 / 2.0;
    }
    double dx = m->ax + lx - m->ccx, dy = m->ay + ly - m->ccy, c = cos(m->ang), s = sin(m->ang);
    *px = m->ccx + dx * c - dy * s;
    *py = m->ccy + dx * s + dy * c;
}

static void tm_from_pic(const struct tmetrics *m, double px, double py, double *lx, double *ly)
{
    double dx = px - m->ccx, dy = py - m->ccy, c = cos(m->ang), s = sin(m->ang);
    *lx = m->ccx + dx * c + dy * s - m->ax;
    *ly = m->ccy - dx * s + dy * c - m->ay;
}

static int text_family(struct app *a)
{
    return a->tfont_name[0] ? fam_find(a->tfont_name) : -1;
}

/* The picture of straight text laid along an arc of radius R (s = +1: over the top, the ends going down; -1: along the bottom). `ax`, `ay` come back as where
 * the middle of the straight picture lies in the new one (the point the text is turned about). */
static bool bend_pic(const struct pix *src, double R, int s, struct pix *out, double *ax, double *ay)
{
    double L = src->w, H = src->h, minx = 1e18, maxx = -1e18, miny = 1e18, maxy = -1e18;
    for (int e = 0; e < 4; e++)
        for (int i = 0; i <= 64; i++) {
            double t = i / 64.0, u = e < 2 ? (e ? 1 : 0) * L - L / 2 : t * L - L / 2, v = e < 2 ? t * H - H / 2 : (e == 2 ? 0 : H) - H / 2, X, Y;
            bend_point(R, s, u, v, &X, &Y);
            minx = fmin(minx, X);
            maxx = fmax(maxx, X);
            miny = fmin(miny, Y);
            maxy = fmax(maxy, Y);
        }
    int ow = (int)ceil(maxx - minx) + 2, oh = (int)ceil(maxy - miny) + 2;
    if (ow < 1 || oh < 1 || (int64_t)ow * oh > (16 << 20) || !pix_alloc(out, ow, oh, 0))
        return false;
    double cxo = -minx + 1, cyo = -miny + 1, half = L / (2 * R) + 0.02;   /* the circle's centre in the new picture, and the half angle of the sector */
    for (int y = 0; y < oh; y++)
        for (int x = 0; x < ow; x++) {
            double X = x + 0.5 - cxo, Y = y + 0.5 - cyo, r = hypot(X, Y), phi = s > 0 ? atan2(X, -Y) : atan2(X, Y);
            if (fabs(phi) > half)
                continue;
            double u = R * phi, v = s > 0 ? R - r : r - R, sx = u + L / 2 - 0.5, sy = v + H / 2 - 0.5;
            int x0 = (int)floor(sx), y0 = (int)floor(sy);
            double fx = sx - x0, fy = sy - y0, A = 0, Rr = 0, G = 0, B = 0;
            for (int j = 0; j < 2; j++)
                for (int i = 0; i < 2; i++) {
                    int px = x0 + i, py = y0 + j;
                    if (px < 0 || py < 0 || px >= src->w || py >= src->h)
                        continue;
                    uint32_t c = src->p[(size_t)py * (size_t)src->w + (size_t)px];
                    double wgt = (i ? fx : 1 - fx) * (j ? fy : 1 - fy), al = ch_a(c) * wgt;
                    A += al;
                    Rr += ch_r(c) * al;
                    G += ch_g(c) * al;
                    B += ch_b(c) * al;
                }
            out->p[(size_t)y * (size_t)ow + (size_t)x] = A > 0.5 ? argb((int)(A + 0.5), (int)(Rr / A + 0.5), (int)(G / A + 0.5), (int)(B / A + 0.5)) : 0;
        }
    *ax = cxo;
    *ay = cyo - s * R;
    return true;
}

/* The text of the Text tool as a picture (ARGB; `out` NULL = only the measures): lines with their alignment, letter and line spacing, bold, italic, underline,
 * strike, a box behind it, an outline in the second colour, a soft shadow, and the turn. Returns false when it would be too big to make. */
static bool text_build(struct app *a, struct pix *out, struct tmetrics *m)
{
    struct tprov P;
    tprov_init(&P, text_family(a), a->tg[TG_BOLD], a->tg[TG_ITALIC], a->tsize);
    double lh = LM_MAX(P.lh0 * a->val[S_LINE] / 100.0, 1.0), letter = a->val[S_LETTER];
    static double lw[TEXT_LINES + 1];
    static int ls[TEXT_LINES + 1];
    int nl = 1, car_line = 0;
    double pen = 0, car_x = 0;
    ls[0] = 0;
    for (int i = 0; i <= a->txt.len;) {
        if (i == a->txt.caret) {
            car_line = nl - 1;
            car_x = pen;
        }
        if (i == a->txt.len)
            break;
        int used = 1;
        uint32_t cp = lm_utf8_decode(a->txt.buf + i, a->txt.len - i, &used);
        i += used > 0 ? used : 1;
        if (cp == '\n') {
            lw[nl - 1] = LM_MAX(pen - letter, 0.0);
            if (nl < TEXT_LINES) {
                ls[nl] = i;
                nl++;
            }
            pen = 0;
            continue;
        }
        pen += tprov_adv(&P, cp) + letter;
    }
    lw[nl - 1] = LM_MAX(pen - letter, 0.0);
    double W = 2;
    for (int i = 0; i < nl; i++)
        W = LM_MAX(W, lw[i]);
    int align = a->talign;
    memset(m, 0, sizeof(*m));
    m->w = W;
    m->h = nl * lh;
    m->lh = lh;
    m->cax = (W - lw[car_line]) * align / 2.0 + car_x;
    m->cay = car_line * lh;
    m->ax = a->txt.x;
    m->ay = a->txt.y;
    int outline = a->val[S_OUTLINE], shadow = a->val[S_SHADOW];
    bool box = a->tg[TG_BOX];
    int bpad = box ? a->tsize / 6 + 2 : 0;
    int pad = 4 + outline + (shadow ? shadow + shadow / 2 + 2 : 0) + bpad + (P.si ? (int)(a->tsize * 0.25) : 0);
    int bw = (int)ceil(W) + 2 * pad, bh = (int)ceil(m->h) + 2 * pad;
    m->ang = a->val[S_ANGLE] * M_PI / 180.0;
    m->ccx = m->ax - pad + bw / 2.0;
    m->ccy = m->ay - pad + bh / 2.0;
    m->ox = (int)floor(m->ax) - pad;
    m->oy = (int)floor(m->ay) - pad;
    if (a->val[S_BEND] != 0) {
        double theta = abs(a->val[S_BEND]) * 2.4 * M_PI / 180.0;
        m->bR = bw / theta;
        m->bs = a->val[S_BEND] > 0 ? 1 : -1;
        m->bpad = pad;
        m->bw0 = bw;
        m->bh0 = bh;
    }
    if (!out)
        return true;
    if (bw < 1 || bh < 1 || (int64_t)bw * bh > (16 << 20))
        return false;
    uint8_t *ink = calloc((size_t)bw * (size_t)bh, 1);
    if (!ink)
        return false;
    for (int li = 0; li < nl; li++) {
        int end = li + 1 < nl ? ls[li + 1] - 1 : a->txt.len, top = pad + (int)lround(li * lh);
        double x = pad + (W - lw[li]) * align / 2.0, x0 = x;
        for (int i = ls[li]; i < end;) {
            int used = 1;
            uint32_t cp = lm_utf8_decode(a->txt.buf + i, a->txt.len - i, &used);
            i += used > 0 ? used : 1;
            tprov_draw(&P, cp, ink, bw, bh, x, top);
            x += tprov_adv(&P, cp) + letter;
        }
        int th = LM_MAX(a->tsize / 16, 1);
        double xe = x - letter;
        if (a->tg[TG_UNDER] && xe > x0)
            for (int y = top + (int)(P.asc + th); y < top + (int)(P.asc + 2 * th) + 0 && y < bh; y++)
                for (int xx = (int)x0; xx < (int)xe && xx < bw; xx++)
                    if (y >= 0 && xx >= 0)
                        ink[(size_t)y * (size_t)bw + (size_t)xx] = 255;
        if (a->tg[TG_STRIKE] && xe > x0)
            for (int y = top + (int)(P.asc * 0.62); y < top + (int)(P.asc * 0.62) + th && y < bh; y++)
                for (int xx = (int)x0; xx < (int)xe && xx < bw; xx++)
                    if (y >= 0 && xx >= 0)
                        ink[(size_t)y * (size_t)bw + (size_t)xx] = 255;
    }
    struct pix tp;
    if (!pix_alloc(&tp, bw, bh, 0)) {
        free(ink);
        return false;
    }
    if (box && ch_a(a->bg) > 0) {
        int x0 = LM_MAX(pad - bpad, 0), y0 = LM_MAX(pad - bpad, 0), x1 = LM_MIN(pad + (int)ceil(W) + bpad, bw), y1 = LM_MIN(pad + (int)ceil(m->h) + bpad, bh);
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++)
                tp.p[(size_t)y * (size_t)bw + (size_t)x] = a->bg;
    }
    uint8_t *ol = NULL;
    if (outline > 0 || shadow > 0) {
        ol = malloc((size_t)bw * (size_t)bh);
        if (ol) {
            if (outline > 0)
                dilate_cov(ink, ol, bw, bh, outline);
            else
                memcpy(ol, ink, (size_t)bw * (size_t)bh);
        }
    }
    if (ol && shadow > 0) {
        uint8_t *sh = calloc((size_t)bw * (size_t)bh, 1);
        if (sh) {
            int d = shadow * 6 / 10 + 1;
            for (int y = 0; y < bh - d; y++)
                memcpy(&sh[(size_t)(y + d) * (size_t)bw + (size_t)d], &ol[(size_t)y * (size_t)bw], (size_t)(bw - d));
            blur_cov(sh, bw, bh, LM_MAX(shadow / 3, 1));
            for (size_t i = 0; i < (size_t)bw * (size_t)bh; i++)
                if (sh[i])
                    tp.p[i] = blend(tp.p[i], 0xff000000u, sh[i] * 150 / 255);
            free(sh);
        }
    }
    if (ol && outline > 0) {
        uint32_t oc = ch_a(a->bg) ? a->bg : 0xffffffffu;
        for (size_t i = 0; i < (size_t)bw * (size_t)bh; i++)
            if (ol[i])
                tp.p[i] = blend(tp.p[i], oc, ol[i]);
    }
    free(ol);
    for (size_t i = 0; i < (size_t)bw * (size_t)bh; i++)
        if (ink[i])
            tp.p[i] = blend(tp.p[i], a->fg, ink[i]);
    free(ink);
    double pcx = bw / 2.0, pcy = bh / 2.0;                              /* the point the text is turned about: the middle of the picture, or where the bend left it */
    if (m->bs) {
        struct pix bp;
        if (!bend_pic(&tp, m->bR, m->bs, &bp, &pcx, &pcy)) {
            pix_free(&tp);
            return false;
        }
        pix_free(&tp);
        tp = bp;
        bw = bp.w;
        bh = bp.h;
        if (a->val[S_ANGLE] == 0) {
            m->ox = (int)lround(m->ccx - pcx);
            m->oy = (int)lround(m->ccy - pcy);
            *out = tp;
            return true;
        }
    }
    if (a->val[S_ANGLE] == 0) {
        *out = tp;
        return true;
    }
    double ca = cos(m->ang), sa = sin(m->ang);
    int rw = (int)ceil(fabs(bw * ca) + fabs(bh * sa)) + 2, rh = (int)ceil(fabs(bw * sa) + fabs(bh * ca)) + 2;
    double ocx = rw / 2.0, ocy = rh / 2.0;
    if (m->bs) {                                                        /* turned about a point that is not the middle: the extent of the rotated corners */
        double minx = 1e18, maxx = -1e18, miny = 1e18, maxy = -1e18;
        for (int k = 0; k < 4; k++) {
            double qx = (k & 1 ? bw : 0) - pcx, qy = (k & 2 ? bh : 0) - pcy, rx = qx * ca - qy * sa, ry = qx * sa + qy * ca;
            minx = fmin(minx, rx);
            maxx = fmax(maxx, rx);
            miny = fmin(miny, ry);
            maxy = fmax(maxy, ry);
        }
        rw = (int)ceil(maxx - minx) + 2;
        rh = (int)ceil(maxy - miny) + 2;
        ocx = -minx + 1;
        ocy = -miny + 1;
    }
    struct pix rp;
    if ((int64_t)rw * rh > (16 << 20) || !pix_alloc(&rp, rw, rh, 0)) {
        pix_free(&tp);
        return false;
    }
    for (int y = 0; y < rh; y++)
        for (int x = 0; x < rw; x++) {
            double rx = x + 0.5 - ocx, ry = y + 0.5 - ocy;
            double sx = rx * ca + ry * sa + pcx - 0.5, sy = -rx * sa + ry * ca + pcy - 0.5;
            int x0 = (int)floor(sx), y0 = (int)floor(sy);
            double fx = sx - x0, fy = sy - y0, A = 0, R = 0, G = 0, B = 0;
            for (int j = 0; j < 2; j++)
                for (int i = 0; i < 2; i++) {
                    int px = x0 + i, py = y0 + j;
                    if (px < 0 || py < 0 || px >= bw || py >= bh)
                        continue;
                    uint32_t c = tp.p[(size_t)py * (size_t)bw + (size_t)px];
                    double wgt = (i ? fx : 1 - fx) * (j ? fy : 1 - fy), al = ch_a(c) * wgt;
                    A += al;
                    R += ch_r(c) * al;
                    G += ch_g(c) * al;
                    B += ch_b(c) * al;
                }
            rp.p[(size_t)y * (size_t)rw + (size_t)x] = A > 0.5 ? argb((int)(A + 0.5), (int)(R / A + 0.5), (int)(G / A + 0.5), (int)(B / A + 0.5)) : 0;
        }
    pix_free(&tp);
    *out = rp;
    m->ox = (int)lround(m->ccx - ocx);
    m->oy = (int)lround(m->ccy - ocy);
    return true;
}

/* Draws the text on the layer from the layer as it was before (so that it can be redrawn at every key and every change of a setting). */
static void text_preview(struct app *a)
{
    struct irect old = a->preview, nu = { 0, 0, 0, 0 };
    int w = CW(a), h = CH(a);
    if (!irect_empty(&old)) {                                           /* what the last picture covered goes back to what it was */
        irect_clip(&old, w, h);
        for (int y = old.y0; y < old.y1; y++)
            memcpy(&IM(a).p[(size_t)y * (size_t)w + (size_t)old.x0], &a->tbefore.p[(size_t)y * (size_t)w + (size_t)old.x0], (size_t)(old.x1 - old.x0) * 4);
    }
    struct pix tp = { 0 };
    struct tmetrics m;
    if (a->txt.len > 0 && text_build(a, &tp, &m)) {
        nu = (struct irect){ m.ox, m.oy, m.ox + tp.w, m.oy + tp.h };
        irect_clip(&nu, w, h);
        for (int y = nu.y0; y < nu.y1; y++)
            for (int x = nu.x0; x < nu.x1; x++) {
                uint32_t src = tp.p[(size_t)(y - m.oy) * (size_t)tp.w + (size_t)(x - m.ox)];
                if (!ch_a(src))
                    continue;
                size_t i = (size_t)y * (size_t)w + (size_t)x;
                int k = (a->txt_edit >= 0 || a->tg[TG_TLAYER] ? 255 : sel_at(&a->sel, x, y)) * a->val[S_OPACITY] / 100;     /* (a text layer is not cut by the selection) */
                if (k > 0)
                    IM(a).p[i] = blend(a->tbefore.p[i], src, k);
            }
    }
    pix_free(&tp);
    struct irect r = old;
    if (!irect_empty(&nu))
        irect_add(&r, nu.x0, nu.y0, nu.x1, nu.y1);
    a->preview = nu;
    irect_clip(&r, w, h);
    if (!irect_empty(&r))
        flat_mark(a, r);
    a->win.redraw = true;
}

static void text_end(struct app *a)
{
    pix_free(&a->tbefore);
    pix_free(&a->tx_orig);
    a->txt_edit = -1;
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
    a->txt.on = true;
    a->txt.x = ix;
    a->txt.y = iy;
    a->txt.len = a->txt.caret = 0;
    a->txt.buf[0] = 0;
    a->preview = (struct irect){ 0, 0, 0, 0 };
    a->win.redraw = true;
}

/* The text and how it is set, written down on a layer so that it can be edited again. */
static void tx_capture(const struct app *a, struct ltext *t)
{
    memset(t, 0, sizeof(*t));
    t->on = true;
    t->len = LM_MIN(a->txt.len, (int)sizeof(t->buf) - 1);
    memcpy(t->buf, a->txt.buf, (size_t)t->len);
    t->x = a->txt.x;
    t->y = a->txt.y;
    t->size = a->tsize;
    t->align = a->talign;
    t->line = a->val[S_LINE];
    t->letter = a->val[S_LETTER];
    t->outline = a->val[S_OUTLINE];
    t->shadow = a->val[S_SHADOW];
    t->angle = a->val[S_ANGLE];
    t->bend = a->val[S_BEND];
    t->opacity = a->val[S_OPACITY];
    t->bold = a->tg[TG_BOLD];
    t->italic = a->tg[TG_ITALIC];
    t->under = a->tg[TG_UNDER];
    t->strike = a->tg[TG_STRIKE];
    t->box = a->tg[TG_BOX];
    t->fg = a->fg;
    t->bg = a->bg;
    snprintf(t->font, sizeof(t->font), "%s", a->tfont_name);
}

static void tx_load(struct app *a, const struct ltext *t)
{
    memcpy(a->txt.buf, t->buf, (size_t)t->len);
    a->txt.buf[t->len] = 0;
    a->txt.len = a->txt.caret = t->len;
    a->txt.x = t->x;
    a->txt.y = t->y;
    a->tsize = t->size;
    a->talign = t->align;
    a->val[S_LINE] = t->line;
    a->val[S_LETTER] = t->letter;
    a->val[S_OUTLINE] = t->outline;
    a->val[S_SHADOW] = t->shadow;
    a->val[S_ANGLE] = t->angle;
    a->val[S_BEND] = t->bend;
    a->val[S_OPACITY] = t->opacity;
    a->tg[TG_BOLD] = t->bold;
    a->tg[TG_ITALIC] = t->italic;
    a->tg[TG_UNDER] = t->under;
    a->tg[TG_STRIKE] = t->strike;
    a->tg[TG_BOX] = t->box;
    a->fg = t->fg;
    a->bg = t->bg;
    snprintf(a->tfont_name, sizeof(a->tfont_name), "%s", t->font);
}

/* The name a text layer gets: the first words of its text. */
static void tx_name(const char *buf, char *out, size_t n)
{
    char w[28];
    size_t o = 0;
    for (size_t i = 0; buf[i] && o < 20; i++) {
        if (buf[i] == '\n')
            break;
        w[o++] = buf[i];
    }
    while (o > 0 && (w[o - 1] & 0xc0) == 0x80 && o >= 20)                /* not in the middle of a letter */
        o--;
    w[o] = 0;
    snprintf(out, n, "Text: %s", w);
}

static void text_commit(struct app *a)
{
    if (!a->txt.on)
        return;
    if (a->txt_edit >= 0 && a->txt_edit < a->doc.n) {                   /* typed into again: the pixels are on the layer already, the snapshot was taken when it opened */
        struct layer *l = &a->doc.L[a->txt_edit];
        if (a->txt.len > 0) {
            tx_capture(a, &l->tx);
            tx_name(a->txt.buf, l->name, sizeof(l->name));
            l->tx.hash = pix_hash_ink(&l->im);
            push_recent(a, a->fg);
            say(a, "Text changed");
        } else if (a->doc.n > 1) {                                      /* nothing left of it: the layer goes */
            a->doc.cur = a->txt_edit;
            doc_del(&a->doc);
            say(a, "Text layer removed");
        } else {
            l->tx.on = false;
        }
        changed(a);
    } else if (a->txt.len > 0 && a->tg[TG_TLAYER] && a->doc.n < MAX_LAYERS) {      /* a text of its own layer, which keeps the text */
        struct pix tp = { 0 };
        struct tmetrics m;
        if (a->tbefore.p && a->tbefore.w == IM(a).w && a->tbefore.h == IM(a).h) {  /* the layer it was previewed on goes back to what it was */
            memcpy(IM(a).p, a->tbefore.p, (size_t)IM(a).w * (size_t)IM(a).h * 4);
            flat_mark_all(a);
        }
        if (text_build(a, &tp, &m)) {
            struct ltext t;
            tx_capture(a, &t);
            checkpoint_doc(a);
            if (doc_add(&a->doc, "Text", 0)) {
                struct layer *l = &a->doc.L[a->doc.cur];
                int w = CW(a), h = CH(a), k = 255 * a->val[S_OPACITY] / 100;
                for (int y = LM_MAX(m.oy, 0); y < LM_MIN(m.oy + tp.h, h); y++)
                    for (int x = LM_MAX(m.ox, 0); x < LM_MIN(m.ox + tp.w, w); x++) {
                        uint32_t src = tp.p[(size_t)(y - m.oy) * (size_t)tp.w + (size_t)(x - m.ox)];
                        if (ch_a(src))
                            l->im.p[(size_t)y * (size_t)w + (size_t)x] = blend(0, src, k);
                    }
                l->tx = t;
                tx_name(t.buf, l->name, sizeof(l->name));
                l->tx.hash = pix_hash_ink(&l->im);
                push_recent(a, a->fg);
                changed(a);
                say(a, "Text on a layer of its own: click it with the Text tool to change it");
            } else {
                snap_free(&a->undo[--a->nundo]);
                say(a, "Not possible (out of memory?)");
            }
        }
        pix_free(&tp);
    } else if (a->txt.len > 0) {
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
    if (a->txt_edit >= 0) {                                             /* Esc on a text that was opened again: it is as it was, and the snapshot is not needed */
        if (a->tx_orig.p && a->tx_orig.w == IM(a).w && a->tx_orig.h == IM(a).h)
            memcpy(IM(a).p, a->tx_orig.p, (size_t)IM(a).w * (size_t)IM(a).h * 4);
        flat_mark_all(a);
        if (a->nundo > 0)
            snap_free(&a->undo[--a->nundo]);
    } else if (a->tbefore.p && a->tbefore.w == IM(a).w && a->tbefore.h == IM(a).h) {
        memcpy(IM(a).p, a->tbefore.p, (size_t)IM(a).w * (size_t)IM(a).h * 4);
        flat_mark_all(a);
    }
    text_end(a);
}

/* The text layer a click on the picture lands on (the topmost one whose text is still what its pixels show), -1 for none. */
static int text_layer_at(const struct app *a, double ix, double iy)
{
    for (int i = a->doc.n - 1; i >= 0; i--) {
        const struct layer *l = &a->doc.L[i];
        if (!l->tx.on || !l->visible || !l->im.p)
            continue;
        int x0 = l->im.w, y0 = l->im.h, x1 = -1, y1 = -1;
        for (int y = 0; y < l->im.h; y++)
            for (int x = 0; x < l->im.w; x++)
                if (ch_a(l->im.p[(size_t)y * (size_t)l->im.w + (size_t)x])) {
                    x0 = LM_MIN(x0, x);
                    x1 = LM_MAX(x1, x);
                    y0 = LM_MIN(y0, y);
                    y1 = LM_MAX(y1, y);
                }
        if (x1 < 0 || ix < x0 - 4 || ix > x1 + 5 || iy < y0 - 4 || iy > y1 + 5)
            continue;
        if (pix_hash_ink(&l->im) != l->tx.hash)
            continue;                                                   /* painted on since: a picture now */
        return i;
    }
    return -1;
}

/* Opens the text of a text layer to be typed into again: the settings it was made with come back, the layer shows nothing meanwhile (the preview draws it). */
static void text_reedit(struct app *a, int li)
{
    a->doc.cur = li;
    checkpoint_doc(a);                                                  /* undo takes the whole layer back, text and all */
    struct ltext t = a->doc.L[li].tx;
    if (!pix_copy(&IM(a), &a->tx_orig)) {
        snap_free(&a->undo[--a->nundo]);
        return;
    }
    pix_free(&a->tbefore);
    if (!pix_alloc(&a->tbefore, CW(a), CH(a), 0)) {
        pix_free(&a->tx_orig);
        snap_free(&a->undo[--a->nundo]);
        return;
    }
    tx_load(a, &t);
    a->txt_edit = li;
    a->txt.on = true;
    memset(IM(a).p, 0, (size_t)CW(a) * (size_t)CH(a) * 4);
    a->preview = (struct irect){ 0, 0, 0, 0 };
    flat_mark_all(a);
    text_preview(a);
    say(a, "Typing into the text again: Ctrl+Enter or a click elsewhere puts it down, Esc leaves it as it was");
}

/* Whatever is half done (a text being typed) is put down before something else happens. */
static void shapes_finish(struct app *a);

static void finish_pending(struct app *a)
{
    text_commit(a);
    shapes_finish(a);
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
    int g = group_of_tool(t);
    if (g >= 0)
        a->grp[g] = t;
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
    wlcard_center(&a->win, &g->c);                                      /* (where the user carried the card, too) */
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

/* ---- the effects: engine ------------------------------------------------------------------------------------------------------------ */

/* One line of a box blur (a sliding window; the window is cut at the ends). */
static void blur_line(const uint8_t *src, uint8_t *dst, int n, int stride, int r)
{
    int sum = 0, cnt = 0;
    for (int i = 0; i <= r && i < n; i++) {
        sum += src[(size_t)i * (size_t)stride];
        cnt++;
    }
    for (int x = 0; x < n; x++) {
        dst[(size_t)x * (size_t)stride] = (uint8_t)((sum + cnt / 2) / cnt);
        int add = x + r + 1, rem = x - r;
        if (add < n) {
            sum += src[(size_t)add * (size_t)stride];
            cnt++;
        }
        if (rem >= 0) {
            sum -= src[(size_t)rem * (size_t)stride];
            cnt--;
        }
    }
}

/* A box blur of radius r over a plane, `passes` times each way (three passes come close to a Gaussian). */
static void blur_plane(uint8_t *p, int w, int h, int r, int passes)
{
    if (r < 1 || w < 1 || h < 1)
        return;
    uint8_t *tmp = malloc((size_t)w * (size_t)h);
    if (!tmp)
        return;
    for (int k = 0; k < passes; k++) {
        for (int y = 0; y < h; y++)
            blur_line(p + (size_t)y * (size_t)w, tmp + (size_t)y * (size_t)w, w, 1, r);
        for (int x = 0; x < w; x++)
            blur_line(tmp + x, p + x, h, w, r);
    }
    free(tmp);
}

/* A blur of the colours of the box `r` (by alpha, so a see-through edge does not turn dark). */
static bool blur_pix(struct pix *im, struct irect r, int radius)
{
    clip_rect(im, &r);
    int w = r.x1 - r.x0, h = r.y1 - r.y0;
    if (w < 1 || h < 1 || radius < 1)
        return true;
    size_t n = (size_t)w * (size_t)h;
    uint8_t *pl = malloc(n * 4);
    if (!pl)
        return false;
    uint8_t *A = pl, *R = pl + n, *G = pl + 2 * n, *B = pl + 3 * n;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t c = im->p[(size_t)(y + r.y0) * (size_t)im->w + (size_t)(x + r.x0)];
            size_t i = (size_t)y * (size_t)w + (size_t)x;
            int a = ch_a(c);
            A[i] = (uint8_t)a;
            R[i] = (uint8_t)(ch_r(c) * a / 255);
            G[i] = (uint8_t)(ch_g(c) * a / 255);
            B[i] = (uint8_t)(ch_b(c) * a / 255);
        }
    for (int k = 0; k < 4; k++)
        blur_plane(pl + (size_t)k * n, w, h, radius, 3);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            size_t i = (size_t)y * (size_t)w + (size_t)x;
            int a = A[i];
            im->p[(size_t)(y + r.y0) * (size_t)im->w + (size_t)(x + r.x0)] =
                a ? argb(a, clamp255(R[i] * 255 / a), clamp255(G[i] * 255 / a), clamp255(B[i] * 255 / a)) : 0;
        }
    free(pl);
    return true;
}

static bool unsharp_pix(struct pix *im, struct irect r, int amount, int radius)
{
    clip_rect(im, &r);
    struct pix bl;
    if (!pix_copy(im, &bl))
        return false;
    bool ok = blur_pix(&bl, r, radius);
    for (int y = r.y0; ok && y < r.y1; y++)
        for (int x = r.x0; x < r.x1; x++) {
            size_t i = (size_t)y * (size_t)im->w + (size_t)x;
            uint32_t o = im->p[i], b = bl.p[i];
            if (!ch_a(o))
                continue;
            im->p[i] = argb(ch_a(o), clamp255(ch_r(o) + (ch_r(o) - ch_r(b)) * amount / 100), clamp255(ch_g(o) + (ch_g(o) - ch_g(b)) * amount / 100),
                            clamp255(ch_b(o) + (ch_b(o) - ch_b(b)) * amount / 100));
        }
    pix_free(&bl);
    return ok;
}

static void levels_apply(struct pix *im, struct irect r, int black, int white, int gamma100)
{
    clip_rect(im, &r);
    uint8_t lut[256];
    if (white <= black)
        white = black + 1;
    for (int v = 0; v < 256; v++) {
        double t = LM_CLAMP((v - black) / (double)(white - black), 0.0, 1.0);
        lut[v] = (uint8_t)(pow(t, 100.0 / LM_MAX(gamma100, 1)) * 255.0 + 0.5);
    }
    for (int y = r.y0; y < r.y1; y++)
        for (int x = r.x0; x < r.x1; x++) {
            uint32_t *q = &im->p[(size_t)y * (size_t)im->w + (size_t)x];
            *q = argb(ch_a(*q), lut[ch_r(*q)], lut[ch_g(*q)], lut[ch_b(*q)]);
        }
}

static void vignette_apply(struct pix *im, struct irect r, int strength, int size)
{
    clip_rect(im, &r);
    double cx = (r.x0 + r.x1) / 2.0, cy = (r.y0 + r.y1) / 2.0, hw = LM_MAX((r.x1 - r.x0) / 2.0, 1.0), hh = LM_MAX((r.y1 - r.y0) / 2.0, 1.0), start = size / 100.0;
    for (int y = r.y0; y < r.y1; y++)
        for (int x = r.x0; x < r.x1; x++) {
            double nx = (x + 0.5 - cx) / hw, ny = (y + 0.5 - cy) / hh, d = sqrt((nx * nx + ny * ny) / 2.0);
            double t = LM_CLAMP((d - start) / (1.0 - start + 1e-6), 0.0, 1.0);
            t = t * t * (3.0 - 2.0 * t);
            double f = 1.0 - strength / 100.0 * t;
            uint32_t *q = &im->p[(size_t)y * (size_t)im->w + (size_t)x];
            *q = argb(ch_a(*q), (int)(ch_r(*q) * f + 0.5), (int)(ch_g(*q) * f + 0.5), (int)(ch_b(*q) * f + 0.5));
        }
}

/* Blocks of the average colour, `bs` pixels a side. */
static void pixelate_block(struct pix *im, struct irect r, int bs)
{
    clip_rect(im, &r);
    bs = LM_MAX(bs, 1);
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
}

/* The colours near `from` become `to` (the pixel keeps its alpha). */
static void recolour(struct pix *im, struct irect r, uint32_t from, uint32_t to, int tol100)
{
    clip_rect(im, &r);
    int t = tol100 * 255 / 100;
    for (int y = r.y0; y < r.y1; y++)
        for (int x = r.x0; x < r.x1; x++) {
            uint32_t *q = &im->p[(size_t)y * (size_t)im->w + (size_t)x];
            if (ch_a(*q) && abs(ch_r(*q) - ch_r(from)) <= t && abs(ch_g(*q) - ch_g(from)) <= t && abs(ch_b(*q) - ch_b(from)) <= t)
                *q = argb(ch_a(*q), ch_r(to), ch_g(to), ch_b(to));
        }
}

/* The colours near `col` turn see-through, softly at the edge of the tolerance. */
static void colour_to_alpha(struct pix *im, struct irect r, uint32_t col, int tol100)
{
    clip_rect(im, &r);
    double t = tol100 / 100.0;
    for (int y = r.y0; y < r.y1; y++)
        for (int x = r.x0; x < r.x1; x++) {
            uint32_t *q = &im->p[(size_t)y * (size_t)im->w + (size_t)x];
            if (!ch_a(*q))
                continue;
            double d = LM_MAX(abs(ch_r(*q) - ch_r(col)), LM_MAX(abs(ch_g(*q) - ch_g(col)), abs(ch_b(*q) - ch_b(col)))) / 255.0;
            double k = LM_CLAMP((d - t) / 0.12, 0.0, 1.0);
            *q = k <= 0 ? 0 : argb((int)(ch_a(*q) * k + 0.5), ch_r(*q), ch_g(*q), ch_b(*q));
        }
}

/* The bilinear sample of a picture at a point (by alpha: colours of see-through pixels do not leak). */
static uint32_t pix_bilinear(const struct pix *p, double sx, double sy)
{
    int x0 = (int)floor(sx), y0 = (int)floor(sy);
    double fx = sx - x0, fy = sy - y0, A = 0, R = 0, G = 0, B = 0;
    for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++) {
            int px = x0 + i, py = y0 + j;
            if (px < 0 || py < 0 || px >= p->w || py >= p->h)
                continue;
            uint32_t c = p->p[(size_t)py * (size_t)p->w + (size_t)px];
            double wgt = (i ? fx : 1 - fx) * (j ? fy : 1 - fy), al = ch_a(c) * wgt;
            A += al;
            R += ch_r(c) * al;
            G += ch_g(c) * al;
            B += ch_b(c) * al;
        }
    return A > 0.5 ? argb((int)(A + 0.5), (int)(R / A + 0.5), (int)(G / A + 0.5), (int)(B / A + 0.5)) : 0;
}

/* `lift` (whose top left lies at (lx, ly) of `dst`) scaled and turned about its centre, laid over `dst`; the box it covers. */
static struct irect xform_lay(struct pix *dst, const struct pix *lift, int lx, int ly, double scale, double deg)
{
    double cx = lx + lift->w / 2.0, cy = ly + lift->h / 2.0, a = deg * M_PI / 180.0, ca = cos(a), sa = sin(a);
    double hw = lift->w * scale / 2.0, hh = lift->h * scale / 2.0, ex = fabs(hw * ca) + fabs(hh * sa) + 1, ey = fabs(hw * sa) + fabs(hh * ca) + 1;
    struct irect box = { (int)floor(cx - ex), (int)floor(cy - ey), (int)ceil(cx + ex), (int)ceil(cy + ey) };
    irect_clip(&box, dst->w, dst->h);
    for (int y = box.y0; y < box.y1; y++)
        for (int x = box.x0; x < box.x1; x++) {
            double rx = x + 0.5 - cx, ry = y + 0.5 - cy;
            double sx = (rx * ca + ry * sa) / scale + lift->w / 2.0 - 0.5, sy = (-rx * sa + ry * ca) / scale + lift->h / 2.0 - 0.5;
            uint32_t c = pix_bilinear(lift, sx, sy);
            if (ch_a(c)) {
                uint32_t *q = &dst->p[(size_t)y * (size_t)dst->w + (size_t)x];
                *q = blend(*q, c, 255);
            }
        }
    return box;
}

/* Every layer turned by `deg` degrees about the middle of a sheet that grows to hold the corners. */
static bool rotate_doc(struct doc *d, double deg)
{
    int w = doc_w(d), h = doc_h(d);
    double a = deg * M_PI / 180.0, ca = cos(a), sa = sin(a);
    int nw = (int)ceil(fabs(w * ca) + fabs(h * sa) - 1e-6), nh = (int)ceil(fabs(w * sa) + fabs(h * ca) - 1e-6);
    if (nw < 1 || nh < 1 || nw > MAX_SIDE || nh > MAX_SIDE || (uint64_t)nw * (uint64_t)nh > LM_IMG_MAX_PIXELS)
        return false;
    doc_bake_masks(d);
    struct pix np[MAX_LAYERS];
    for (int l = 0; l < d->n; l++)
        if (!pix_alloc(&np[l], nw, nh, 0)) {
            for (int k = 0; k < l; k++)
                pix_free(&np[k]);
            return false;
        }
    for (int l = 0; l < d->n; l++) {
        for (int y = 0; y < nh; y++)
            for (int x = 0; x < nw; x++) {
                double rx = x + 0.5 - nw / 2.0, ry = y + 0.5 - nh / 2.0;
                np[l].p[(size_t)y * (size_t)nw + (size_t)x] = pix_bilinear(&d->L[l].im, rx * ca + ry * sa + w / 2.0 - 0.5, -rx * sa + ry * ca + h / 2.0 - 0.5);
            }
        pix_free(&d->L[l].im);
        d->L[l].im = np[l];
    }
    return true;
}

/* A shadow of a layer's shape: shifted down and right, softened, in `col`. */
static bool make_shadow_pix(const struct pix *src, struct pix *out, int dist, int blur, int opacity, uint32_t col)
{
    int w = src->w, h = src->h;
    size_t n = (size_t)w * (size_t)h;
    if (!pix_alloc(out, w, h, 0))
        return false;
    uint8_t *pl = calloc(n, 1);
    if (!pl) {
        pix_free(out);
        return false;
    }
    int d = (int)lround(dist * 0.75);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int tx = x + d, ty = y + d;
            if (tx >= 0 && ty >= 0 && tx < w && ty < h)
                pl[(size_t)ty * (size_t)w + (size_t)tx] = (uint8_t)ch_a(src->p[(size_t)y * (size_t)w + (size_t)x]);
        }
    blur_plane(pl, w, h, blur, 3);
    for (size_t i = 0; i < n; i++)
        if (pl[i])
            out->p[i] = argb(pl[i] * opacity / 100, ch_r(col), ch_g(col), ch_b(col));
    free(pl);
    return true;
}

/* The shape grown by `width` pixels, in `col`. */
static bool make_outline_pix(const struct pix *src, struct pix *out, int width, int soft, uint32_t col)
{
    int w = src->w, h = src->h;
    size_t n = (size_t)w * (size_t)h;
    if (!pix_alloc(out, w, h, 0))
        return false;
    uint8_t *pl = malloc(n), *dl = malloc(n);
    if (!pl || !dl) {
        free(pl);
        free(dl);
        pix_free(out);
        return false;
    }
    for (size_t i = 0; i < n; i++)
        pl[i] = (uint8_t)ch_a(src->p[i]);
    dilate_cov(pl, dl, w, h, width);
    blur_plane(dl, w, h, soft, 2);
    for (size_t i = 0; i < n; i++)
        if (dl[i])
            out->p[i] = argb(dl[i] * ch_a(col) / 255, ch_r(col), ch_g(col), ch_b(col));
    free(pl);
    free(dl);
    return true;
}

static bool sel_dup(struct sel *to, const struct sel *from)
{
    *to = *from;
    to->mask = NULL;
    if (from->mask) {
        size_t n = (size_t)(from->r.x1 - from->r.x0) * (size_t)(from->r.y1 - from->r.y0);
        to->mask = malloc(n);
        if (!to->mask) {
            to->on = false;
            return false;
        }
        memcpy(to->mask, from->mask, n);
    }
    return true;
}

/* The selection as a mask the size of the picture. */
static bool sel_to_cov(const struct sel *s, struct cov *c, int w, int h)
{
    if (!cov_alloc(c, w, h))
        return false;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            c->p[(size_t)y * (size_t)w + (size_t)x] = (uint8_t)sel_at(s, x, y);
    return true;
}

/* ---- the effects: the card with its sliders ----------------------------------------------------------------------------------------- */

enum fxk { K_ADJUST, K_BLUR, K_UNSHARP, K_LEVELS, K_VIGNETTE, K_MOSAIC, K_REPLACE, K_TOALPHA, K_SHADOW, K_OUTLINE, K_XFORM, K_ROTIMG, K_FEATHER, K_GROW, K_ANIM, K_COUNT };
enum { FM_LAYER, FM_NEWLAYER, FM_SEL, FM_IMAGE, FM_XFORM, FM_EXPORT };

static const struct fxkind {
    const char *id, *title;
    int mode, n;
    struct { const char *name; int mn, mx, def; } p[4];
} FXK[K_COUNT] = {
    [K_ADJUST] = { "adjust", "Adjust", FM_LAYER, 4, { { "Brightness", -100, 100, 0 }, { "Contrast", -100, 100, 0 }, { "Hue", -180, 180, 0 }, { "Saturation", -100, 100, 0 } } },
    [K_BLUR] = { "blur", "Blur", FM_LAYER, 1, { { "Radius", 0, 60, 4 } } },
    [K_UNSHARP] = { "unsharp", "Unsharp mask", FM_LAYER, 2, { { "Amount", 0, 400, 100 }, { "Radius", 1, 30, 3 } } },
    [K_LEVELS] = { "levels", "Levels", FM_LAYER, 3, { { "Black", 0, 250, 0 }, { "White", 5, 255, 255 }, { "Gamma", 10, 300, 100 } } },
    [K_VIGNETTE] = { "vignette", "Vignette", FM_LAYER, 2, { { "Strength", 0, 100, 60 }, { "Size", 5, 95, 50 } } },
    [K_MOSAIC] = { "mosaic", "Mosaic", FM_LAYER, 1, { { "Block", 1, 80, 8 } } },
    [K_REPLACE] = { "replace", "Pen colour becomes the second colour", FM_LAYER, 1, { { "Tolerance", 0, 100, 15 } } },
    [K_TOALPHA] = { "toalpha", "The second colour becomes transparent", FM_LAYER, 1, { { "Tolerance", 0, 100, 20 } } },
    [K_SHADOW] = { "shadow", "Drop shadow (on a new layer below)", FM_NEWLAYER, 3, { { "Distance", 0, 80, 8 }, { "Blur", 0, 40, 6 }, { "Opacity", 0, 100, 60 } } },
    [K_OUTLINE] = { "outline", "Outline in the pen colour (new layer below)", FM_NEWLAYER, 2, { { "Width", 1, 40, 4 }, { "Softness", 0, 20, 0 } } },
    [K_XFORM] = { "xform", "Scale and turn the selection or the layer", FM_XFORM, 2, { { "Scale", 10, 400, 100 }, { "Turn", -180, 180, 0 } } },
    [K_ANIM] = { "animate", "Animation of the layers (delay in ms, 0 loops = for ever)", FM_EXPORT, 4, { { "Delay", 20, 2000, 100 }, { "Loops", 0, 20, 0 }, { "Background", 0, 1, 0 }, { "Ping-pong", 0, 1, 0 } } },
    [K_ROTIMG] = { "rotate", "Turn the whole picture by any angle", FM_IMAGE, 1, { { "Angle", -180, 180, 0 } } },
    [K_FEATHER] = { "feather", "Soften the edge of the selection", FM_SEL, 1, { { "Radius", 0, 60, 6 } } },
    [K_GROW] = { "grow", "Grow or shrink the selection", FM_SEL, 1, { { "Pixels", -40, 40, 5 } } },
};

static int *adj_ptr(struct app *a, int i) { return &a->fxv[i]; }

/* The effect again from the layer as it was (every move of a slider). */
static void fx_update(struct app *a)
{
    const struct fxkind *k = &FXK[a->fxk];
    int *v = a->fxv, w = CW(a), h = CH(a);
    if (k->mode == FM_LAYER || k->mode == FM_XFORM) {
        if (!a->adj_before.p || a->adj_before.w != IM(a).w || a->adj_before.h != IM(a).h)
            return;
        memcpy(IM(a).p, a->adj_before.p, (size_t)IM(a).w * (size_t)IM(a).h * 4);
        struct irect r = a->sel.on ? a->sel.r : (struct irect){ 0, 0, w, h };
        irect_clip(&r, w, h);
        if (k->mode == FM_XFORM) {
            struct pix lift;
            if (irect_empty(&r) || !pix_alloc(&lift, r.x1 - r.x0, r.y1 - r.y0, 0))
                return;
            for (int y = r.y0; y < r.y1; y++)
                for (int x = r.x0; x < r.x1; x++) {
                    size_t i = (size_t)y * (size_t)w + (size_t)x;
                    uint32_t c = a->adj_before.p[i];
                    int m = sel_at(&a->sel, x, y);
                    lift.p[(size_t)(y - r.y0) * (size_t)lift.w + (size_t)(x - r.x0)] = m >= 255 ? c : m <= 0 ? 0 : argb(ch_a(c) * m / 255, ch_r(c), ch_g(c), ch_b(c));
                    IM(a).p[i] = m >= 255 ? 0 : m <= 0 ? c : blend(c, 0, m);
                }
            a->fx_box = xform_lay(&IM(a), &lift, r.x0, r.y0, v[0] / 100.0, v[1]);
            pix_free(&lift);
        } else {
            switch (a->fxk) {
            case K_ADJUST: {
                struct adjust ad = { v[0], v[1], v[2], v[3] };
                if (ad.brightness || ad.contrast || ad.hue || ad.saturation)
                    adjust_region(&IM(a), r, &ad);
                break;
            }
            case K_BLUR: blur_pix(&IM(a), r, v[0]); break;
            case K_UNSHARP: unsharp_pix(&IM(a), r, v[0], v[1]); break;
            case K_LEVELS: levels_apply(&IM(a), r, v[0], v[1], v[2]); break;
            case K_VIGNETTE: vignette_apply(&IM(a), r, v[0], v[1]); break;
            case K_MOSAIC: pixelate_block(&IM(a), r, v[0]); break;
            case K_REPLACE: recolour(&IM(a), r, a->fg, a->bg, v[0]); break;
            case K_TOALPHA: colour_to_alpha(&IM(a), r, a->bg, v[0]); break;
            default: break;
            }
            sel_restore(&IM(a), &a->adj_before, r, &a->sel);
        }
        flat_mark_all(a);
    } else if (k->mode == FM_NEWLAYER) {
        struct pix np;
        bool ok = a->fxk == K_SHADOW ? make_shadow_pix(&a->adj_before, &np, v[0], v[1], v[2], 0xff000000u) : make_outline_pix(&a->adj_before, &np, v[0], v[1], a->fg);
        if (ok && a->fx_newl >= 0 && a->fx_newl < a->doc.n) {
            pix_free(&a->doc.L[a->fx_newl].im);
            a->doc.L[a->fx_newl].im = np;
            flat_mark_all(a);
        }
    } else if (k->mode == FM_SEL) {
        sel_clear(&a->sel);
        sel_dup(&a->sel, &a->fx_sel0);
        struct cov c, d;
        if (!sel_to_cov(&a->fx_sel0, &c, w, h))
            return;
        if (a->fxk == K_FEATHER) {
            blur_plane(c.p, w, h, v[0], 3);
            sel_clear(&a->sel);
            sel_from_cov(&a->sel, &c, (struct irect){ 0, 0, w, h });
        } else if (v[0] != 0 && cov_alloc(&d, w, h)) {
            if (v[0] > 0)
                dilate_cov_thr(c.p, d.p, w, h, v[0], 128);
            else {
                for (size_t i = 0; i < (size_t)w * (size_t)h; i++)
                    c.p[i] = (uint8_t)(255 - c.p[i]);
                dilate_cov_thr(c.p, d.p, w, h, -v[0], 128);
                for (size_t i = 0; i < (size_t)w * (size_t)h; i++)
                    d.p[i] = (uint8_t)(255 - d.p[i]);
            }
            sel_clear(&a->sel);
            sel_from_cov(&a->sel, &d, (struct irect){ 0, 0, w, h });
            cov_free(&d);
        }
        cov_free(&c);
        a->vdirty = true;
    }
    a->win.redraw = true;
}

static void fx_reset_values(struct app *a)
{
    const struct fxkind *k = &FXK[a->fxk];
    for (int i = 0; i < 4; i++)
        a->fxv[i] = i < k->n ? k->p[i].def : 0;
}

static void fx_open(struct app *a, int kind)
{
    finish_pending(a);
    const struct fxkind *k = &FXK[kind];
    if (k->mode == FM_SEL && !a->sel.on) {
        say(a, "Select an area first (the Select tools)");
        return;
    }
    if (k->mode == FM_EXPORT && a->doc.n < 2) {
        say(a, "An animation needs layers: every layer is a frame (add some with New or Copy)");
        return;
    }
    a->fxk = kind;
    a->adj_drag = -1;
    fx_reset_values(a);
    pix_free(&a->adj_before);
    sel_clear(&a->fx_sel0);
    a->fx_newl = -1;
    a->fx_snap_ok = false;
    if (k->mode != FM_SEL && k->mode != FM_IMAGE && k->mode != FM_EXPORT && !pix_copy(&IM(a), &a->adj_before)) {
        say(a, "Not possible (out of memory?)");
        return;
    }
    if (k->mode == FM_NEWLAYER) {                                       /* the layer for the result is made now, so that it can be seen while the sliders move */
        if (a->doc.n >= MAX_LAYERS || !snap_make(a, SK_DOC, a->doc.cur, &a->fx_snap)) {
            pix_free(&a->adj_before);
            say(a, a->doc.n >= MAX_LAYERS ? "No more layers" : "Not possible (out of memory?)");
            return;
        }
        a->fx_snap_ok = true;
        if (!doc_add(&a->doc, kind == K_SHADOW ? "Shadow" : "Outline", 0)) {
            snap_free(&a->fx_snap);
            a->fx_snap_ok = false;
            pix_free(&a->adj_before);
            say(a, "Not possible (out of memory?)");
            return;
        }
        doc_move(&a->doc, -1);
        a->fx_newl = a->doc.cur;
    }
    if (k->mode == FM_SEL)
        sel_dup(&a->fx_sel0, &a->sel);
    a->prompt = P_ADJUST;
    a->perr[0] = 0;
    a->pcard_hover = a->pcard_down = -1;
    fx_update(a);
    a->win.redraw = true;
}

static void adj_open(struct app *a) { fx_open(a, K_ADJUST); }

/* The card goes: with `apply` the change stays (one undo step), else everything is as it was. */
static void fx_close(struct app *a, bool apply)
{
    const struct fxkind *k = &FXK[a->fxk];
    bool same_size = a->adj_before.p && a->adj_before.w == CW(a) && a->adj_before.h == CH(a);
    switch (k->mode) {
    case FM_LAYER:
    case FM_XFORM:
        if (apply) {
            checkpoint_with(a, &a->adj_before);
            a->modified = true;
            if (k->mode == FM_XFORM && !irect_empty(&a->fx_box)) {      /* the selection follows what was scaled and turned */
                sel_clear(&a->sel);
                a->sel.on = true;
                a->sel.r = a->fx_box;
                a->vdirty = true;
            }
        } else if (same_size) {
            memcpy(IM(a).p, a->adj_before.p, (size_t)IM(a).w * (size_t)IM(a).h * 4);
            flat_mark_all(a);
        }
        break;
    case FM_NEWLAYER:
        if (apply) {
            if (a->fx_snap_ok) {
                snaps_push(a->undo, &a->nundo, &a->fx_snap);
                drop_snaps(a->redo, &a->nredo);
                a->fx_snap_ok = false;
            }
            a->modified = true;
            a->doc.cur = LM_MIN(a->fx_newl + 1, a->doc.n - 1);          /* the layer that was shaded stays the chosen one */
        } else {
            if (a->fx_newl >= 0 && a->fx_newl < a->doc.n) {
                a->doc.cur = a->fx_newl;
                doc_del(&a->doc);
                a->doc.cur = LM_MIN(a->fx_newl, a->doc.n - 1);
            }
            if (a->fx_snap_ok) {
                snap_free(&a->fx_snap);
                a->fx_snap_ok = false;
            }
            flat_mark_all(a);
        }
        break;
    case FM_SEL:
        if (!apply) {
            sel_clear(&a->sel);
            sel_dup(&a->sel, &a->fx_sel0);
            a->vdirty = true;
        }
        break;
    case FM_IMAGE:
        if (apply) {
            checkpoint_doc(a);
            if (!rotate_doc(&a->doc, a->fxv[0])) {
                do_undo(a, false);
                say(a, "Not possible (too big, or out of memory)");
            } else {
                sel_clear(&a->sel);
                changed(a);
                if (a->fitted)
                    view_fit(a);
                say(a, "Turned by %d degrees", a->fxv[0]);
            }
        }
        break;
    case FM_EXPORT:
        if (apply) {                                                    /* the card goes, then the Save as card asks where (prompt_close) */
            a->anim_delay = a->fxv[0];
            a->anim_loops = a->fxv[1];
            a->anim_bg = a->fxv[2] != 0;
            a->anim_ping = a->fxv[3] != 0;
            a->anim_go = true;
        }
        break;
    default: break;
    }
    pix_free(&a->adj_before);
    sel_clear(&a->fx_sel0);
    a->fx_box = (struct irect){ 0, 0, 0, 0 };
}

/* The card's rows: where each slider's track lies. */
static void adj_track(const struct wlcard *c, int i, int *x, int *y, int *w)
{
    *x = c->x + 128;
    *w = c->w - 128 - 76;
    *y = c->y + 52 + i * 38 + 8;
}

/* ---- the Effects card: every effect one button ---------------------------------------------------------------------------------------- */

static const struct { const char *label; int simple, kind, group; } EFX[] = {
    { "Grayscale", FX_GRAY, -1, 0 }, { "Sepia", FX_SEPIA, -1, 0 }, { "Invert", FX_INVERT, -1, 0 }, { "Brighter", FX_BRIGHTER, -1, 0 },
    { "Darker", FX_DARKER, -1, 0 }, { "More contrast", FX_MORE_CONTRAST, -1, 0 }, { "Less contrast", FX_LESS_CONTRAST, -1, 0 }, { "Adjust...", -1, K_ADJUST, 0 },
    { "Levels...", -1, K_LEVELS, 0 }, { "Replace colour...", -1, K_REPLACE, 0 }, { "Colour to alpha...", -1, K_TOALPHA, 0 },
    { "Blur...", -1, K_BLUR, 1 }, { "Sharpen", FX_SHARPEN, -1, 1 }, { "Unsharp mask...", -1, K_UNSHARP, 1 }, { "Mosaic...", -1, K_MOSAIC, 1 },
    { "Pixelate", FX_PIXELATE, -1, 1 }, { "Noise", FX_NOISE, -1, 1 }, { "Emboss", FX_EMBOSS, -1, 1 }, { "Edges", FX_EDGES, -1, 1 }, { "Vignette...", -1, K_VIGNETTE, 1 },
    { "Drop shadow...", -1, K_SHADOW, 2 }, { "Outline...", -1, K_OUTLINE, 2 },
    { "Scale and turn...", -1, K_XFORM, 3 }, { "Turn picture...", -1, K_ROTIMG, 3 }, { "Feather selection...", -1, K_FEATHER, 3 }, { "Grow or shrink...", -1, K_GROW, 3 }, { "Animation...", -1, K_ANIM, 3 },
};
#define N_EFX ((int)(sizeof(EFX) / sizeof(EFX[0])))
#define EFX_W 132
#define EFX_H 26

static void efx_rect(const struct wlcard *c, int i, int *x, int *y, int *w, int *h)
{
    *x = c->x + WLCARD_PAD + (i % 4) * (EFX_W + 6);
    *y = c->y + 42 + (i / 4) * (EFX_H + 6);
    *w = EFX_W;
    *h = EFX_H;
}

static void effects_run(struct app *a, int i)
{
    a->prompt = P_NONE;
    a->win.redraw = true;
    if (EFX[i].simple >= 0)
        do_effect(a, (enum fx)EFX[i].simple);
    else
        fx_open(a, EFX[i].kind);
}

static void prompt_close(struct app *a, bool apply);
static void ask_saveas_anim(struct app *a);

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

/* ---- the help: pages of text with a list of topics ---------------------------------------------------------------------------------- */

/* A page is a list of lines: "# Heading", "- a bullet", "|keys|what they do" (a row of a table: the keys in an accent colour, the words wrapped beside them),
 * "" (a gap) and anything else is a paragraph. Single blanks between words only: the lines are wrapped to the window by `help_layout`. */
static const char *const HP_WELCOME[] = {
    "Paint draws, edits and layers pictures. What is in the window:",
    "- The rail at the left holds the tools. A small triangle on a button means a group of tools: click the lit button again, or right click it, to pick another one.",
    "- The rail at the right: New, Open, Save, Undo, Redo, the Transform and Effects menus, Crop, Fit and this help.",
    "- The picture in the middle. The layers panel is at its right (Tab hides it).",
    "- Under the picture: the strip with the colours and the sliders of the tool you are using, and the status line with hints and the place of the pointer.",
    "# The mouse",
    "|Left button|Draws with the pen colour.",
    "|Right button|Draws with the second colour (the small square behind the pen colour).",
    "|Wheel|Zooms the picture. Over a slider it steps the slider, over the layers panel it scrolls the list, over a layer's Opacity it changes it.",
    "|Middle button|Drags the picture about. So does the left button with the Hand tool (Y) - it shares the Move button: click the lit button again to pick it - or with any tool while Space is held.",
    "# Looking at the picture",
    "|+ and -|Zoom in and out.",
    "|0|Fit the picture to the window.",
    "|1|Show it at its real size (100 %).",
    "|Arrow keys|Move the picture (with Shift: further).",
    "|Space|Hold it and drag with the left button: the picture moves, whatever the tool.",
    "|Ctrl+G|A grid of pixels (when you are zoomed in).",
    "|Tab|Hide or show the layers panel.",
    "|Scroll bars|Every list here has the same one, as wide as [windows] scrollbar-width: drag the thumb, click beside it to turn a page.",
    "# Good to know",
    "- Everything can be undone: Ctrl+Z, and Ctrl+Y or Ctrl+Shift+Z to do it again. The last 32 steps are kept.",
    "- A picture is only written to disk when you save it. Closing the window asks first when there is something unsaved.",
    "- Your colours, the sliders, the rulers and the panel are remembered in ~/.config/mellagui/paint.conf.",
    "- The pages of this help: Left and Right (or the list at the left) change the page, Up, Down and the wheel scroll, Esc closes it.",
};

static const char *const HP_DRAWING[] = {
    "# Tools that draw",
    "|P|Pen: drag to draw. Size, Opacity and Hardness (how soft the edge is) are in the strip.",
    "|A|Airbrush: hold the button to spray; Density is in the strip.",
    "|E|Eraser: takes the colour of the layer away, so the layer (or the paper behind it) shows through.",
    "|L|Line: drag it (Shift keeps it at 45 degrees). The Arrow switch gives it a head.",
    "|B|Box: drag it (Shift: a square). Corner rounds it, Filled makes it solid.",
    "|C|Circle or ellipse: drag it (Shift: a circle). Filled makes it solid.",
    "|N|Polygon or star: drag it; Sides and Star are in the strip.",
    "|J|Free polygon: click the corners. Enter, a double click or a click on the first corner closes it, Esc gives it up.",
    "|V|Curve: drag a line, then drag its two handles to bend it. Enter puts it down.",
    "|F|Fill: click to colour an area of a similar colour (Tolerance). All of it fills every similar pixel, not only the joined area; Image looks at what you see, not at the layer.",
    "|G|Gradient: drag from where the colour starts to where it ends. Radial goes round the start, To clear fades to see-through instead of the second colour.",
    "|I|Picker: click a pixel to take its colour (a right click takes it as the second colour).",
    "# The brush bar",
    "Pen, Airbrush, Eraser and Line have a bar above the strip:",
    "- Smooth steadies the hand: the line follows the pointer on a rubber band.",
    "- Symmetry (1 to 16) draws turned copies of every stroke round the middle of the picture; Mirror adds a mirror image. The lines the copies come from are shown on the picture.",
    "- Pixel draws hard squares with no soft edge, for pixel art.",
    "# Colours",
    "- The two squares at the left of the strip are the pen colour and the second colour. A click opens the colour card: a square for saturation and brightness, a bar for the hue, a bar for how see-through it is, a field for a code like #ff8800 or #ff880080, and quick colours. Esc puts the old colour back.",
    "- The small swap button beside them (or the X key) swaps the two colours.",
    "- The small squares are the colours used lately: a click makes one the pen colour, a right click the second colour.",
    "# Sliders",
    "Click or drag a slider, or turn the wheel over it (with Shift in bigger steps). [ and ] make the brush smaller and bigger.",
};

static const char *const HP_SELECTING[] = {
    "# Selecting",
    "|S|Rectangle: drag it. The Ratio switch keeps its shape (1:1, 4:3 ...).",
    "|O|Ellipse (Shift: a circle).",
    "|Q|Lasso: drag round what you want; it is selected when you let go.",
    "|W|Magic wand: click to select the area of a similar colour (Tolerance; All of it selects everywhere in the picture).",
    "# Adding and taking away",
    "Hold Shift while you drag to add to the selection, Alt to take away from it, Ctrl to keep only what both have. The Add and Subtract switches in the strip do the same.",
    "# What a selection does",
    "- Tools draw only inside it, and effects work only on it.",
    "|Delete|Clears what is selected (the whole layer when nothing is).",
    "|Backspace|Fills it with the pen colour.",
    "|Enter|Crops the picture to it.",
    "|Esc or Ctrl+D|Drops the selection.",
    "|Ctrl+A|Selects everything.",
    "|Ctrl+I|Inverts the selection.",
    "- Effects > Feather selection softens its edge, Grow or shrink makes it bigger or smaller.",
    "# Moving and copying",
    "|M|Move: drag to move what is selected, or the whole layer when nothing is.",
    "|Ctrl+C and Ctrl+X|Copy or cut the selection (the whole layer when nothing is selected).",
    "|Ctrl+V|Pastes as a layer of its own and chooses Move, so you can put it in place.",
    "- Effects > Scale and turn... scales and turns the selection or the layer.",
};

static const char *const HP_RETOUCH[] = {
    "The retouching tools are one group on the left rail (the third button from the top, a small triangle in its corner).",
    "# Clone stamp (K)",
    "Alt+click (or the right button) takes the spot to copy from. Then paint: the picture round that spot appears where you paint, and keeps moving with you. Take a new spot any time.",
    "With the Image switch on it copies what you see (all the layers); otherwise only the chosen layer.",
    "# Smudge (R)",
    "Drag to smear the colours along, as with a finger in wet paint. Opacity is how strong it is.",
    "# Blur brush (H)",
    "Drag to soften what the brush touches. Opacity is how strong it is.",
    "# Hints",
    "- Size and Hardness are in the strip as for the pen; the ring on the picture shows how big the brush is.",
    "- A selection keeps all three inside it.",
    "- For a blur of the whole layer or the selection use Effects > Blur.",
};

static const char *const HP_TEXT[] = {
    "# Writing",
    "Press T, click where the text starts and type. Enter makes a new line. Ctrl+Enter or a click somewhere else puts the text down, Esc gives it up. Ctrl+V pastes text into it.",
    "The size is the Size slider in the strip. The text bar above the strip holds the rest:",
    "- Font: a card with the font families found on the computer, each shown in its own letters (Interface font is the program's own). Type a name to jump to it (\"lob\" finds Lobster), or use the arrow keys, the wheel and the bar.",
    "- More fonts: the script tools/get-google-fonts.sh downloads about ninety of Google's free fonts (Lato, Roboto, Montserrat, Lobster, Pacifico, Fira Code ...) into ~/.local/share/fonts/google; Paint lists them the next time it starts. A font file (.ttf) put in ~/.local/share/fonts is found too. Fonts with PostScript outlines (.otf) are not read.",
    "- B, I, U, S: bold, italic, underline, strike. Box puts a box behind the text in the second colour.",
    "- Left, centre and right alignment.",
    "- Line is the space between lines, Letters the space between letters.",
    "- Outline draws a rim in the second colour, Shadow a soft shadow, Angle turns the text.",
    "- Bend lays the text along an arc: plus arches it over, minus lets it sag, up to 240 degrees.",
    "# Text on its own layer",
    "The Layer switch (on by default) puts the text on a layer of its own, named Text: and its first words.",
    "Click the text with the Text tool again and you can type into it again, with the settings it was made with. Esc leaves it as it was.",
    "It stays a text only as long as nothing else is done to that layer: painting on it, filtering it, moving it or turning the picture makes it a plain picture, and a click then starts a new text. An .ora file remembers the text.",
    "With the Layer switch off the text goes straight onto the chosen layer and is picture from then on.",
};

static const char *const HP_LAYERS[] = {
    "# The panel",
    "The layers panel is at the right of the picture. The top of the list is the top of the stack; you draw on the chosen layer.",
    "|Eye|Shows or hides a layer.",
    "|Click|Chooses the layer. Double click renames it; right click opens a menu.",
    "|New|A new empty layer above the chosen one.",
    "|Copy|Duplicates the layer.",
    "|Delete|Deletes it (the last layer stays).",
    "|Up and Down|Move the layer in the stack.",
    "|Merge|Lays the layer onto the one below it; the two become one.",
    "|Opacity|How much of the layer shows (0 hides it).",
    "|Blend|How it mixes with what is below: Normal, Multiply, Screen, Overlay, Darken, Lighten, Difference.",
    "# The paper",
    "The sheet behind all the layers is not a layer: a new picture is one empty layer on white paper, so the eraser shows the paper and Move carries only the drawing. Transform > Paper colour = the second colour changes it.",
    "# Keys",
    "|PageUp and PageDown|Choose the layer above or below.",
    "|Ctrl+Shift+N|New layer.",
    "|Ctrl+J|Duplicate the layer.",
    "|Ctrl+E|Merge down.",
    "|Tab|Hide or show the panel.",
    "# Saving layers",
    ".ora keeps all the layers (and groups, masks and text). Any other file type gets the flat picture, and the status line says so.",
};

static const char *const HP_GROUPS[] = {
    "# Groups",
    "A group is a folder of layers. Right click a layer and choose Group the layer; the group gets a row of its own with a fold arrow, a folder and an eye.",
    "- Click the group's row to choose the group. The panel's Opacity, Blend and eye are then the group's, and New, Copy, Delete, Up, Down and Merge work on the whole group (New adds a layer on top inside it, Delete removes the group with its layers, Merge makes it one layer).",
    "- Click the arrow to fold the group (its layers leave the list); double click the name to rename it.",
    "- Blend has Pass through (the usual): the layers in the group mix with what is below it as if the group were not there. Any other mode, or an opacity under 100, lays the group over as one picture.",
    "- Up and Down move a layer or group past the next one on the same level. Right click > Into the group above / below puts it in a group, Take out of the group takes it out.",
    "- A group can be in a group. Ungroup (in the group's menu) keeps the layers. Ctrl+Alt+G groups the layer, or ungroups the chosen group.",
    "# Masks",
    "A mask decides where a layer shows: white shows it, black hides it, grey half. Right click a layer > Add a mask (with a selection, the inside shows and the outside is hidden).",
    "- The mask's small picture sits beside the layer's in the panel. Click it to paint on the mask instead of the layer (the status line says (mask)); click again to go back.",
    "- Paint black to hide, white to show; every tool, the fill and the gradient work on it.",
    "- Apply the mask makes the hidden parts see-through for good, Delete the mask shows the whole layer again.",
    "- A change of the picture's shape (turn, resize, crop, canvas size) applies masks first.",
    "- .ora keeps groups and masks.",
};

static const char *const HP_EFFECTS[] = {
    "# Effects",
    "The Effects button (right rail) opens a card of buttons. They work on the chosen layer, or on the selection:",
    "- Colour: Grayscale, Sepia, Invert, Brighter, Darker, More and Less contrast, Adjust (brightness, contrast, hue, saturation; Ctrl+U), Levels, Replace colour (one colour becomes another), Colour to alpha (a colour becomes see-through).",
    "- Detail: Blur, Sharpen, Unsharp mask, Mosaic, Pixelate, Noise, Emboss, Edges, Vignette.",
    "- On a new layer below: Drop shadow and Outline (in the pen colour); the sliders show the result as you move them.",
    "- Shapes: Scale and turn, Turn picture by any angle, Feather selection, Grow or shrink.",
    "- Animation: see that page.",
    "Cards with sliders show the result at once; Cancel puts everything back, Reset sets the sliders to their start, Apply keeps it (one undo step).",
    "- The panel floats over the picture and does not darken it: take it by its title (the dots) and put it where it hides nothing; it keeps its place while Paint runs. A click beside it does nothing, the wheel zooms the picture and the middle button moves it, as usual; the keys + - 0 1 zoom too.",
    "- Every other card (Open, Save as, New, this help ...) can be moved by its title as well.",
    "# Size and shape",
    "|Transform|The button on the right rail: turn left or right, flip, Resize, Canvas size, flip the layer only, Place picture as a layer, the paper colour, Turn by any angle.",
    "|Ctrl+R|Resize: type 800x600, a width (800) or a percentage (50%).",
    "|Ctrl+Alt+C|Canvas size: a bigger or smaller sheet with a 3 x 3 anchor for where the picture sits. The new room is transparent or the second colour.",
    "|Ctrl+Left and Ctrl+Right|Turn the whole picture a quarter.",
    "|Crop|The button on the right rail (or Enter): crops to the selection.",
};

static const char *const HP_RULERS[] = {
    "# Rulers",
    "Ctrl+L shows rulers along the top and the left of the picture, like Photoshop's: the numbers go on round the picture, below zero and past its far edge, and the part over the picture is a little lighter. The pointer's place is marked on them.",
    "- Click the square in the corner to change the unit: pixels, millimetres, centimetres, inches, points. The unit's name stands in the corner.",
    "- Right click a ruler for a menu of the units and the resolution (72, 96, 150 or 300 dpi). A picture has no size of its own, so a millimetre is worked out from the dpi: 96 dpi is 3.78 pixels a millimetre.",
    "- The status line then also tells the pointer's place in that unit.",
    "# Guides",
    "- Drag out of a ruler to make a guide (up to 32). Drag it again to move it. Drag it back onto a ruler, or off the picture, to take it away.",
    "- Guides are only for the eye and for snapping; they are not saved in the file.",
    "# Snapping",
    "Ctrl+B makes the selection, shape, gradient and text tools stick to a guide, to the edges and to the middle of the picture when they come within a few pixels.",
    "# Grid",
    "Ctrl+G shows a grid of pixels when you are zoomed in far enough.",
    "- The ruler, unit, resolution and snapping are remembered next time.",
};

static const char *const HP_FILES[] = {
    "# Files",
    "|Ctrl+O|Open: a list of the folder you type, filtering as you type (Up, Down, Enter; Tab completes the name).",
    "|Ctrl+S|Save. A picture with no name asks for one.",
    "|Ctrl+Shift+S|Save as: click a format and the extension in the name changes.",
    "|Ctrl+N|New picture: type a size, 800x600. White paper or a transparent first layer.",
    "- .ora (OpenRaster, which GIMP and Krita read) keeps layers, groups, masks and text. .png, .webp, .gif, .tif, .bmp, .tga, .jpg and .ppm get the flat picture; saving layers as one of those says so. See-through is kept by .png, .webp, .gif, .tif, .bmp and .tga; .jpg and .ppm lay the picture on white.",
    "- Transform > Place picture as a layer puts another picture on a layer of its own.",
    "# The clipboard",
    "- Ctrl+C and Ctrl+X put the picture on the computer's clipboard (as a PNG), so other programs can paste it.",
    "- Ctrl+V pastes a picture from the clipboard (a screenshot, something copied in a browser) as a new layer. Ctrl+Shift+V makes a new picture of it.",
    "- The screenshot tool (Super+Shift+S) has a Paint button (Ctrl+E): it saves the picture and opens it here.",
    "# From the command line",
    "|mellagui-paint FILE|Opens a picture. --new 800x600 [FILE] starts a new one.",
    "|--apply IN OUT OP...|Does things without a window: rotate-left, flip-h, resize=50%, crop=X,Y,W,H, gray, sepia, invert, blur, sharpen ...",
    "|--animate OUT.gif FRAME...|Makes an animation out of pictures (see Animations).",
};

static const char *const HP_ANIM[] = {
    "# Making an animation",
    "The layers are the frames. Draw each frame on a layer of its own (bottom layer first), then open Effects > Animation... A group counts as one frame, so a frame can be made of several layers.",
    "|Delay|How long each frame is shown, in milliseconds.",
    "|Loops|How many times it plays; 0 is for ever.",
    "|Background|The first layer stays under every frame instead of being a frame.",
    "|Ping-pong|After the last frame it goes back again (without showing the ends twice).",
    "Apply asks where to save: a name ending in .gif or .webp (.gif is suggested). The picture itself stays as it is; the animation is a separate file.",
    "- A layer or group named like Walk @250 is shown for 250 ms, whatever the Delay says.",
    "- Hidden layers are left out.",
    "- GIF frames have a colour table of up to 255 colours each, so very colourful frames lose some colours. WebP is lossless.",
    "# Opening an animation",
    "An animated GIF opens with a layer for every frame (named Frame 1 @100 ...), the way a GIF player would show it. Edit them and save again with Effects > Animation... Animated WebP files open as their first picture only.",
    "# From the command line",
    "mellagui-paint --animate out.gif a.png b.png c.png --delay 80 --loops 3 [--bg] [--pingpong] makes it without a window (the pictures must have one size; a name like b@250.png gives that frame 250 ms).",
};

static const char *const HP_KEYS[] = {
    "# Tools",
    "|S O Q W|Select: rectangle, ellipse, lasso, magic wand",
    "|M Y|Move (the layer or the selection), Hand (the picture in the window)",
    "|P A E|Pen, airbrush, eraser",
    "|K R H|Clone stamp, smudge, blur brush",
    "|L B C N|Line, box, circle, polygon or star",
    "|J V|Free polygon, curve",
    "|F G T I|Fill, gradient, text, picker",
    "# Picture",
    "|Ctrl+O N S|Open, new, save (Shift+S: save as)",
    "|Ctrl+Z Y|Undo, redo (Ctrl+Shift+Z also redoes)",
    "|Ctrl+C X V|Copy, cut, paste (Ctrl+Shift+V: a new picture from the clipboard)",
    "|Ctrl+A D I|Select all, select none, invert the selection",
    "|Delete Backspace Enter|Clear, fill, crop to the selection",
    "|Ctrl+R|Resize",
    "|Ctrl+Alt+C|Canvas size",
    "|Ctrl+U|Adjust brightness, contrast, hue, saturation",
    "|Ctrl+Left Right|Turn the picture a quarter",
    "# Looking",
    "|+ - 0 1|Zoom in, out, fit, real size",
    "|Arrow keys|Move the picture (Shift: further)",
    "|Ctrl+G|Pixel grid",
    "|Ctrl+L|Rulers",
    "|Ctrl+B|Snap to guides",
    "|Tab|Hide or show the layers panel",
    "# Colours and brush",
    "|X|Swap the pen and the second colour",
    "|[ ]|Smaller and bigger brush",
    "# Layers",
    "|PageUp PageDown|Choose the layer above or below",
    "|Ctrl+Shift+N|New layer",
    "|Ctrl+J|Duplicate",
    "|Ctrl+E|Merge down",
    "|Ctrl+Alt+G|Group the layer, or ungroup the chosen group",
    "# Others",
    "|Esc|Drop the selection; give up a shape or text being made",
    "|Enter|Close a free polygon, put down a curve; crop to a selection",
    "|Ctrl+Enter|Put the text down",
    "|F1|This help",
    "|Ctrl+Q|Quit (asks about unsaved changes)",
};

#define HP_N(arr) ((int)(sizeof(arr) / sizeof(arr[0])))
static const struct hpage {
    const char *title;
    const char *const *lines;
    int n;
} HP[] = {
    { "Welcome", HP_WELCOME, HP_N(HP_WELCOME) },
    { "Drawing and colours", HP_DRAWING, HP_N(HP_DRAWING) },
    { "Selecting and moving", HP_SELECTING, HP_N(HP_SELECTING) },
    { "Retouching", HP_RETOUCH, HP_N(HP_RETOUCH) },
    { "Text", HP_TEXT, HP_N(HP_TEXT) },
    { "Layers", HP_LAYERS, HP_N(HP_LAYERS) },
    { "Groups and masks", HP_GROUPS, HP_N(HP_GROUPS) },
    { "Effects and sizes", HP_EFFECTS, HP_N(HP_EFFECTS) },
    { "Rulers and guides", HP_RULERS, HP_N(HP_RULERS) },
    { "Files and clipboard", HP_FILES, HP_N(HP_FILES) },
    { "Animations", HP_ANIM, HP_N(HP_ANIM) },
    { "All the keys", HP_KEYS, HP_N(HP_KEYS) },
};
#define HP_COUNT ((int)(sizeof(HP) / sizeof(HP[0])))

/* One line of a page as it will be drawn: a piece of a source line (wrapped), where, and what kind. */
enum { HL_TEXT, HL_HEAD, HL_KEY, HL_DOT };
struct hl {
    const char *p;
    int len, x, y, kind;
};
#define HL_MAX 900
#define HELP_LH (LM_FONT_H + 4)
static struct hl g_hl[HL_MAX];
static int g_hl_n, g_hl_h, g_hl_page = -1, g_hl_w = -1;
static char g_hl_pool[8192];                                            /* text the lines point into that is not a source line (the keys of a table row) */
static int g_hl_pool_n;

/* How wide a line of this kind is drawn: headings and keys are bold, which is wider. */
static int hl_text_w(const char *s, int kind)
{
    if (kind == HL_HEAD || kind == HL_KEY)
        return lm_font_ui_cells_get() ? (int)strlen(s) * LM_FONT_W : lm_font_text_width_bold(s);
    return wlcard_text_w(s);
}

/* Wraps `s` (single blanks between words) to `width` pixels from `x`, one `struct hl` per line; `*y` moves down. */
static int hl_wrap(const char *s, int x, int width, int *y, int kind)
{
    size_t n = strlen(s), pos = 0;
    int lines = 0;
    while (pos < n && g_hl_n < HL_MAX) {
        size_t end = pos;
        char buf[600];
        for (;;) {
            size_t nx = end;
            while (nx < n && s[nx] == ' ')
                nx++;
            while (nx < n && s[nx] != ' ')
                nx++;
            if (nx == end)
                break;
            size_t len = nx - pos;
            if (len >= sizeof(buf))
                len = sizeof(buf) - 1;
            memcpy(buf, s + pos, len);
            buf[len] = 0;
            if (end > pos && hl_text_w(buf, kind) > width)
                break;                                                  /* (a word that does not fit starts the next line) */
            end = nx;
            if (end >= n)
                break;
        }
        if (end == pos)
            break;
        g_hl[g_hl_n++] = (struct hl){ s + pos, (int)(end - pos), x, *y, kind };
        *y += HELP_LH;
        lines++;
        pos = end;
        while (pos < n && s[pos] == ' ')
            pos++;
    }
    return lines;
}

/* Sets a page out for a content width; the result is in g_hl and g_hl_h (its height). Kept until the page or the width changes. */
static void help_layout(int page, int width)
{
    if (page == g_hl_page && width == g_hl_w)
        return;
    g_hl_page = page;
    g_hl_w = width;
    g_hl_n = 0;
    g_hl_pool_n = 0;
    int y = 0;
    const struct hpage *hp = &HP[page];
    for (int i = 0; i < hp->n; i++) {
        const char *s = hp->lines[i];
        if (!*s) {
            y += 8;
        } else if (s[0] == '#') {
            if (y)
                y += 10;
            hl_wrap(s + 2, 0, width, &y, HL_HEAD);
            y += 3;
        } else if (s[0] == '|') {                                       /* |keys|words: a table row */
            const char *bar = strchr(s + 1, '|');
            if (!bar)
                continue;
            int kw = LM_MIN(190, width / 3), y0 = y;
            size_t kl = (size_t)(bar - s - 1);
            if (g_hl_pool_n + (int)kl + 1 > (int)sizeof(g_hl_pool))
                continue;
            char *key = g_hl_pool + g_hl_pool_n;                        /* (the lines keep pointing at it: not a local) */
            memcpy(key, s + 1, kl);
            key[kl] = 0;
            g_hl_pool_n += (int)kl + 1;
            int kn = hl_wrap(key, 0, kw, &y, HL_KEY);
            int dn = hl_wrap(bar + 1, kw + 12, width - kw - 12, &y0, HL_TEXT);
            (void)dn;
            y = LM_MAX(y, y0) + 4;
            (void)kn;
        } else if (s[0] == '-' && s[1] == ' ') {                        /* a bullet */
            int y0 = y;
            if (g_hl_n < HL_MAX)
                g_hl[g_hl_n++] = (struct hl){ "", 0, 4, y0, HL_DOT };
            hl_wrap(s + 2, 18, width - 18, &y, HL_TEXT);
            y += 3;
        } else {
            hl_wrap(s, 0, width, &y, HL_TEXT);
            y += 4;
        }
    }
    g_hl_h = y;
}

/* Where the help's parts lie, from the card. */
struct hgeo {
    int sx, sy, sw, rowh;                       /* the list of pages */
    int cx, cy, cw, ch;                         /* the text */
    int bx, by, bw, bh;                         /* the scroll bar's track */
    int py, pw, ph, prev_x, next_x;             /* the footer's buttons */
};

static void help_geom(const struct wlcard *c, struct hgeo *g)
{
    int top = c->y + 40, bot = c->y + c->h - WLCARD_PAD - WLCARD_BTN_H - 12;
    g->sx = c->x + WLCARD_PAD;
    g->sy = top;
    g->sw = 164;
    g->rowh = LM_CLAMP((bot - top) / HP_COUNT, 18, 28);
    g->cx = g->sx + g->sw + 16;
    g->cy = top;
    g->cw = c->x + c->w - WLCARD_PAD - wlsbar_w() - 8 - g->cx;                  /* (the text ends where the scroll bar begins) */
    g->ch = bot - top;
    g->bw = wlsbar_w();
    g->bx = c->x + c->w - WLCARD_PAD - g->bw;
    g->by = top;
    g->bh = bot - top;
    g->py = c->y + c->h - WLCARD_PAD - WLCARD_BTN_H;
    g->pw = 84;
    g->ph = WLCARD_BTN_H;
    g->prev_x = g->sx;
    g->next_x = g->sx + g->pw + 6;
}

/* The help's scroll bar (the pages are measured in pixels: a page is what the text area shows). */
static struct wlsbar help_sbar(const struct hgeo *g, int top)
{
    return (struct wlsbar){ g->bx, g->by, g->bw, g->bh, g_hl_h, g->ch, top };
}

static int help_max_top(const struct hgeo *g)
{
    return LM_MAX(g_hl_h - g->ch, 0);
}


static void prompt_accept(struct app *a);

static bool paint_name_ok(const char *name)
{
    const char *dot = strrchr(name, '.');
    return wlpicker_picture(name) || (dot && !strcasecmp(dot, ".ora"));
}
#define card_name_ok paint_name_ok

#define FONT_ROW_H 34
static int font_rows_fit(const struct app *a);
static void font_choose(struct app *a, int item);
static void font_show(struct app *a);

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
    case P_HELP: ch = 600; cw = 940; break;
    case P_DISCARD: case P_REPLACE: ch = 130; break;
    case P_NEW: ch = 178; break;
    case P_CANVAS: ch = 232; break;
    case P_ADJUST: ch = 56 + FXK[a->fxk].n * 38 + 20 + WLCARD_BTN_H; cw = 420; break;
    case P_EFFECTS: ch = 42 + ((N_EFX + 3) / 4) * (EFX_H + 6) + 14 + WLCARD_BTN_H + 8; cw = 4 * EFX_W + 3 * 6 + 2 * WLCARD_PAD; break;
    case P_COLOUR: ch = CP_H; cw = CP_W; break;
    case P_FONT: ch = 40 + font_rows_fit(a) * FONT_ROW_H + 16 + WLCARD_BTN_H + 6; cw = 440; break;
    default: break;
    }
    int rows = card_list_rows(a);
    if (rows)
        ch = 170 + rows * WLPICKER_ROW_H;
    *nb = a->prompt == P_DISCARD || a->prompt == P_ADJUST ? 3 : a->prompt == P_HELP || a->prompt == P_FONT || a->prompt == P_EFFECTS ? 1 : 2;
    c->w = LM_MIN(cw, a->win.bw - 16);
    c->h = LM_MIN(ch, a->win.bh - 16);
    wlcard_center(&a->win, c);
    if (a->prompt == P_ADJUST) {                                        /* the effect panel floats: at the right until it is moved, always inside the window */
        int x = a->adj_placed ? a->adj_x : a->win.bw - WLRAIL_W - c->w - 6, y = a->adj_placed ? a->adj_y : 24;
        c->x = LM_CLAMP(x, 0, LM_MAX(a->win.bw - c->w, 0));
        c->y = LM_CLAMP(y, 0, LM_MAX(a->win.bh - c->h, 0));
    }
}

static void help_open(struct app *a)
{
    a->prompt = P_HELP;
    a->pcard_hover = a->pcard_down = -1;
    a->help_hover = -1;                                                 /* (the page it was left on is kept) */
    a->help_btn = 0;
    a->help_drag = false;
    a->help_top = 0;
    a->win.redraw = true;
}

static void help_set_page(struct app *a, int page)
{
    page = LM_CLAMP(page, 0, HP_COUNT - 1);
    if (page != a->help_page)
        a->help_top = 0;
    a->help_page = page;
    a->win.redraw = true;
}

static void help_scroll(struct app *a, int dy)
{
    struct wlcard c;
    int nb;
    card_rect(a, &c, &nb);
    struct hgeo g;
    help_geom(&c, &g);
    help_layout(a->help_page, g.cw);
    a->help_top = LM_CLAMP(a->help_top + dy, 0, help_max_top(&g));
    a->win.redraw = true;
}

static void paint_help(struct app *a, const struct wlcard *c)
{
    struct wlwin *w = &a->win;
    struct hgeo g;
    help_geom(c, &g);
    help_layout(a->help_page, g.cw);
    a->help_top = LM_CLAMP(a->help_top, 0, help_max_top(&g));
    for (int i = 0; i < HP_COUNT; i++) {                                /* the pages */
        int y = g.sy + i * g.rowh;
        bool cur = i == a->help_page, hot = i == a->help_hover;
        if (cur)
            wlwin_round_fill(w, g.sx - 4, y, g.sw + 4, g.rowh - 2, 7, lm_theme.control_hot);
        else if (hot)
            wlwin_round_fill(w, g.sx - 4, y, g.sw + 4, g.rowh - 2, 7, lm_theme.control);
        char t[64];
        snprintf(t, sizeof(t), "%d  %s", i + 1, HP[i].title);
        if (cur)
            wlwin_text_bold(w, g.sx + 2, y + (g.rowh - 2 - LM_FONT_H) / 2, t, 1, lm_theme.text_bright, g.sw - 4);
        else
            wlwin_text(w, g.sx + 2, y + (g.rowh - 2 - LM_FONT_H) / 2, t, 1, COL_TEXT, g.sw - 4);
    }
    wlwin_fill(w, g.cx - 9, g.cy, 1, g.ch, lm_theme.border);
    for (int i = 0; i < g_hl_n; i++) {                                  /* the text of the page: only the lines that fit whole */
        const struct hl *l = &g_hl[i];
        int y = l->y - a->help_top;
        if (y < 0 || y + HELP_LH > g.ch)
            continue;
        char buf[600];
        int n = LM_MIN(l->len, (int)sizeof(buf) - 1);
        memcpy(buf, l->p, (size_t)n);
        buf[n] = 0;
        int x = g.cx + l->x, py = g.cy + y;
        switch (l->kind) {
        case HL_HEAD: wlwin_text_bold(w, x, py, buf, 1, COL_ACCENT, g.cw - l->x); break;
        case HL_KEY: wlwin_text_bold(w, x, py, buf, 1, lm_theme.text_bright, g.cw - l->x); break;
        case HL_DOT: wlwin_round_fill(w, x, py + LM_FONT_H / 2 - 2, 5, 5, 2, COL_ACCENT); break;
        default: wlwin_text(w, x, py, buf, 1, COL_TEXT, g.cw - l->x); break;
        }
    }
    struct wlsbar sb = help_sbar(&g, a->help_top);                      /* the scroll bar */
    wlsbar_paint(w, &sb, a->help_drag || (a->has_pointer && wlsbar_has(&sb, a->px, a->py)));
    bool can_prev = a->help_page > 0, can_next = a->help_page < HP_COUNT - 1;                    /* the footer */
    wlcard_drop(w, g.prev_x, g.py, g.pw, g.ph, can_prev ? lm_theme.btn_slate : lm_theme.btn_grey, a->help_btn == 1 && can_prev, false, "< Back", lm_theme.on_colour);
    wlcard_drop(w, g.next_x, g.py, g.pw, g.ph, can_next ? lm_theme.btn_slate : lm_theme.btn_grey, a->help_btn == 2 && can_next, false, "Next >", lm_theme.on_colour);
    char pg[48];
    snprintf(pg, sizeof(pg), "Page %d of %d", a->help_page + 1, HP_COUNT);
    wlwin_text(w, g.next_x + g.pw + 14, g.py + (g.ph - 2 - LM_FONT_H) / 2, pg, 1, COL_DIM, 0);
    int hx = g.next_x + g.pw + 14 + wlcard_text_w(pg) + 20, hw = c->x + c->w - WLCARD_PAD - WLCARD_BTN_W - 16 - hx;
    if (hw > 120)
        wlwin_text(w, hx, g.py + (g.ph - 2 - LM_FONT_H) / 2, "Left / Right: page   Up / Down / wheel: scroll   Esc: close", 1, COL_DIM, hw);
}

/* What the pointer is on: 1000 + a page row, 1 Back, 2 Next, 3 the scroll bar, 0 nothing. */
static int help_hit(const struct app *a, double x, double y)
{
    struct wlcard c;
    int nb;
    card_rect(a, &c, &nb);
    struct hgeo g;
    help_geom(&c, &g);
    if (x >= g.sx - 4 && x < g.sx + g.sw && y >= g.sy && y < g.sy + HP_COUNT * g.rowh)
        return 1000 + (int)((y - g.sy) / g.rowh);
    if (y >= g.py && y < g.py + g.ph) {
        if (x >= g.prev_x && x < g.prev_x + g.pw)
            return 1;
        if (x >= g.next_x && x < g.next_x + g.pw)
            return 2;
    }
    struct wlsbar sb = help_sbar(&g, a->help_top);
    if (wlsbar_has(&sb, x, y))
        return 3;
    return 0;
}

static bool help_key(struct app *a, uint32_t ks)
{
    switch (ks) {
    case LM_KS_Escape: case LM_KS_Return: case LM_KS_KP_Enter: case LM_KS_F1:
        prompt_close(a, false);
        break;
    case LM_KS_Left: help_set_page(a, a->help_page - 1); break;
    case LM_KS_Right: help_set_page(a, a->help_page + 1); break;
    case LM_KS_Up: help_scroll(a, -HELP_LH * 2); break;
    case LM_KS_Down: help_scroll(a, HELP_LH * 2); break;
    case LM_KS_Page_Up: help_scroll(a, -200); break;
    case LM_KS_Page_Down: case ' ': help_scroll(a, 200); break;
    case LM_KS_Home: a->help_top = 0; a->win.redraw = true; break;
    case LM_KS_End: help_scroll(a, 100000); break;
    default:
        if (ks >= '1' && ks <= '9')
            help_set_page(a, (int)(ks - '1'));
        break;
    }
    a->win.redraw = true;
    return true;
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

/* Where the animation goes: the same Save as card, the name suggested with .gif; the next write makes an animation (.gif or .webp) of the layers. */
static void ask_saveas_anim(struct app *a)
{
    char d[PATH_MAX];
    if (a->path[0]) {
        snprintf(d, sizeof(d), "%s", a->path);
        char *dot = strrchr(d, '.'), *slash = strrchr(d, '/');
        if (dot && (!slash || dot > slash))
            *dot = 0;
        strncat(d, ".gif", sizeof(d) - strlen(d) - 1);
    } else {
        default_dir(a, d, sizeof(d));
        strncat(d, "animation.gif", sizeof(d) - strlen(d) - 1);
    }
    prompt_open(a, P_SAVEAS, d);
    a->anim_save = true;
    say(a, "The animation is saved as .gif or .webp");
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

static void new_from_clipboard(struct app *a);

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
    case AF_CLIPNEW: new_from_clipboard(a); break;
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
    bool saving = a->prompt == P_SAVEAS || a->prompt == P_REPLACE;
    if (a->prompt == P_ADJUST)
        fx_close(a, apply);
    a->prompt = P_NONE;
    a->after = AF_NONE;
    if (saving)
        a->anim_save = false;                                           /* (the animation's Save as card was given up) */
    a->win.redraw = true;
    if (a->anim_go) {
        a->anim_go = false;
        ask_saveas_anim(a);
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
        if (FXK[a->fxk].mode != FM_EXPORT)
            say(a, "Adjusted");
        prompt_close(a, true);
        break;
    case P_FONT:
        font_choose(a, a->font_sel);
        break;
    case P_EFFECTS:
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
static enum tool g_grp_var[G_COUNT] = { T_SELECT, T_PEN, T_CLONE, T_LINE, T_MOVE };
static icon_fn tool_icon(enum tool t);

static void group_mark(struct wlwin *w, int cx, int cy, uint32_t c)
{
    wlwin_fill(w, cx + 4, cy + 6, 5, 1, c);
    wlwin_fill(w, cx + 5, cy + 7, 3, 1, c);
    wlwin_fill(w, cx + 6, cy + 8, 1, 1, c);
}

#define GROUP_ICON(NAME, G) \
    static void NAME(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg) \
    { \
        tool_icon(g_grp_var[G])(w, cx - 2, cy - 2, c, bg); \
        group_mark(w, cx, cy, c); \
    }
GROUP_ICON(ic_grp_select, G_SELECT)
GROUP_ICON(ic_grp_brush, G_BRUSH)
GROUP_ICON(ic_grp_retouch, G_RETOUCH)
GROUP_ICON(ic_grp_move, G_MOVE)
GROUP_ICON(ic_grp_shapes, G_SHAPES)

static void ic_freepoly(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    (void)bg;
    wlrail_line(w, cx - 7, cy + 6, cx - 8, cy - 3, 2, c);
    wlrail_line(w, cx - 8, cy - 3, cx - 1, cy - 8, 2, c);
    wlrail_line(w, cx - 1, cy - 8, cx + 7, cy - 2, 2, c);
    wlrail_line(w, cx + 7, cy - 2, cx + 3, cy + 7, 2, c);
    wlwin_fill(w, cx - 1, cy + 7, 3, 2, c);
}

static void ic_curve(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    (void)bg;
    int px = cx - 8, py = cy + 6;
    for (int i = 1; i <= 12; i++) {
        double t = i / 12.0, u = 1 - t;
        int x = cx + (int)lround(u * u * u * -8 + 3 * u * u * t * -8 + 3 * u * t * t * 8 + t * t * t * 8), y = cy + (int)lround(u * u * u * 6 + 3 * u * u * t * -12 + 3 * u * t * t * 12 + t * t * t * -6);
        wlrail_line(w, px, py, x, y, 2, c);
        px = x;
        py = y;
    }
}

static void ic_clone(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    wlwin_round_fill(w, cx - 6, cy - 8, 12, 12, 6, c);                  /* a rubber stamp: its pad and its handle */
    wlwin_round_fill(w, cx - 4, cy - 6, 8, 8, 4, bg);
    wlwin_fill(w, cx - 1, cy + 2, 3, 4, c);
    wlwin_fill(w, cx - 8, cy + 6, 16, 3, c);
}

static void ic_smudge(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    (void)bg;
    wlrail_line(w, cx - 8, cy + 5, cx - 2, cy - 3, 3, c);              /* a finger dragging a colour along */
    wlrail_line(w, cx - 2, cy - 3, cx + 7, cy - 6, 3, c);
    wlrail_line(w, cx - 7, cy + 8, cx + 3, cy + 5, 2, c);
}

static void ic_blurbr(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    wlwin_round_fill(w, cx - 7, cy - 7, 14, 14, 7, c);                  /* a soft dot: rings that fade */
    wlwin_round_fill(w, cx - 4, cy - 4, 8, 8, 4, mix_rgb24(c & 0xffffffu, bg & 0xffffffu, 120));
    wlwin_round_fill(w, cx - 2, cy - 2, 4, 4, 2, bg);
}

static void ic_hand(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    (void)bg;
    wlwin_round_fill(w, cx - 6, cy - 1, 12, 10, 4, c);                  /* the palm */
    for (int i = 0; i < 4; i++)                                         /* four fingers, the middle ones longest */
        wlwin_round_fill(w, cx - 6 + i * 3, cy - 8 - (i == 1 || i == 2 ? 2 : 0), 3, 10, 1, c);
    wlrail_line(w, cx - 6, cy + 4, cx - 9, cy - 1, 3, c);               /* the thumb */
}

static const int LEFT_IDS[N_LEFT] = { B_SELECT, B_MOVE, B_PEN, B_RETOUCH, B_ERASER, B_SHAPES, B_FILL, B_GRADIENT, B_TEXT, B_PICK };
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
    case T_FREEPOLY: return ic_freepoly;
    case T_CURVE: return ic_curve;
    case T_CLONE: return ic_clone;
    case T_SMUDGE: return ic_smudge;
    case T_BLURBR: return ic_blurbr;
    case T_HAND: return ic_hand;
    default: return ic_pick;
    }
}

static void rails(const struct app *a, struct wlrail *L, struct wlrail *R)
{
    static char tipbuf[B_COUNT][64];
    static const char *const tips[B_COUNT] = {
        NULL, "Move  M", NULL, NULL, "Eraser  E", NULL, "Fill  F", "Gradient  G", "Text  T", "Picker  I",
        "New  Ctrl+N", "Open  Ctrl+O", "Save  Ctrl+S", "Undo  Ctrl+Z", "Redo  Ctrl+Y", "Turn, flip, size, canvas", "Effects", "Crop to the selection  Enter", "Fit the window  0", "Keys  F1" };
    static void (*const icons[B_COUNT])(struct wlwin *, int, int, uint32_t, uint32_t) = {
        NULL, ic_move, NULL, NULL, ic_eraser, NULL, ic_fill, ic_gradient, ic_text, ic_pick,
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
    L->y1 = R->y1 = a->win.bh > 0 ? a->win.bh - PAL_H - STAT_H : 400;      /* (the rails keep their height when the text bar is up: it lies between them) */
    L->n = N_LEFT;
    for (int i = 0; i < N_LEFT; i++) {
        int id = LEFT_IDS[i];
        const char *tip = tips[id];
        icon_fn ic = icons[id];
        int g = group_of_button(id);
        if (g >= 0) {
            enum tool v = a->grp[g];
            snprintf(tipbuf[id], sizeof(tipbuf[id]), "%s  %c  (click again or right click: more)", TOOL_TITLES[v], toupper((unsigned char)TOOL_KEYS[v]));
            tip = tipbuf[id];
            for (int k = 0; k < G_COUNT; k++)
                g_grp_var[k] = a->grp[k];
            static const icon_fn gic[G_COUNT] = { ic_grp_select, ic_grp_brush, ic_grp_retouch, ic_grp_shapes, ic_grp_move };
            ic = gic[g];
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
    struct { int id, x, y, w, h; } tg[3];
};

#define SL_TRACK_DY 29
#define SL_KNOB 14

static const char *const RATIO_NAMES[6] = { "Free", "1:1", "4:3", "3:2", "16:9", "9:16" };
static const int RATIO_W[6] = { 0, 1, 4, 3, 16, 9 }, RATIO_H[6] = { 0, 1, 3, 2, 9, 16 };

/* What a switch of the strip says (the ratio says which ratio). */
static const char *tg_label(const struct app *a, int id)
{
    static char buf[24];
    if (id == TG_RATIO) {
        snprintf(buf, sizeof(buf), "Ratio: %s", RATIO_NAMES[a->ratio]);
        return buf;
    }
    return TG_NAMES[id];
}

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
    for (int i = 0; i < 3; i++)
        if (TOPT[a->tool].tg[i] >= 0) {
            int id = TOPT[a->tool].tg[i], w = LM_MAX(wlcard_text_w(tg_label(a, id)) + 24, 56);
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
        const char *t = tg_label(a, s.tg[i].id);
        wlwin_text(w, s.tg[i].x + (s.tg[i].w - wlcard_text_w(t)) / 2, s.tg[i].y + (s.tg[i].h - 2 - LM_FONT_H) / 2, t, 1, lm_theme.on_colour, 0);
    }
}

/* ---- the text bar: the Text tool's settings ---------------------------------------------------------------------------------------- */

struct tbar_g {
    struct { int x, y, w, h; } font;
    struct { int id, x, y, w, h; } tg[6];
    struct { int x, y, w, h; } al[3];
    int nsl;
    struct { int id, x, y, w, h; } sl[6];
};

static const int TB_TGL[6] = { TG_BOLD, TG_ITALIC, TG_UNDER, TG_STRIKE, TG_BOX, TG_TLAYER };
static const char *const TB_TGL_LABEL[6] = { "B", "I", "U", "S", "Box", "Layer" };
static const int TB_SL[6] = { S_LINE, S_LETTER, S_OUTLINE, S_SHADOW, S_ANGLE, S_BEND };

static void tbar_layout(const struct app *a, struct tbar_g *g)
{
    memset(g, 0, sizeof(*g));
    int y = tb_y(a);
    g->font.x = WLRAIL_W + 10;
    g->font.y = y + 7;
    g->font.w = 170;
    g->font.h = 26;
    int x = g->font.x + g->font.w + 10;
    for (int i = 0; i < 6; i++) {
        int w = i == 4 ? 48 : i == 5 ? 50 : 28;
        g->tg[i].id = TB_TGL[i];
        g->tg[i].x = x;
        g->tg[i].y = y + 7;
        g->tg[i].w = w;
        g->tg[i].h = 26;
        x += w + 4;
        if (i == 3 || i == 4)
            x += 10;
    }
    x += 10;
    for (int i = 0; i < 3; i++) {
        g->al[i].x = x;
        g->al[i].y = y + 7;
        g->al[i].w = 30;
        g->al[i].h = 26;
        x += 34;
    }
    int avail = a->win.bw - 2 * WLRAIL_W - 20 - 5 * 14;
    int sw = LM_CLAMP(avail / 6, 56, 190), sx = WLRAIL_W + 10;
    g->nsl = 6;
    for (int i = 0; i < 6; i++) {
        g->sl[i].id = TB_SL[i];
        g->sl[i].x = sx;
        g->sl[i].y = y + 38;
        g->sl[i].w = sw;
        g->sl[i].h = 42;
        sx += sw + 14;
    }
}

enum { TH_NONE, TH_FONT, TH_TGL, TH_ALIGN, TH_SLIDER };

static int tbar_hit(const struct app *a, double x, double y, int *idx)
{
    *idx = -1;
    if (a->tool != T_TEXT || y < tb_y(a) || y >= tb_y(a) + TB_H)
        return TH_NONE;
    struct tbar_g g;
    tbar_layout(a, &g);
    if (x >= g.font.x && x < g.font.x + g.font.w && y >= g.font.y && y < g.font.y + g.font.h)
        return TH_FONT;
    for (int i = 0; i < 6; i++)
        if (x >= g.tg[i].x && x < g.tg[i].x + g.tg[i].w && y >= g.tg[i].y && y < g.tg[i].y + g.tg[i].h) {
            *idx = i;
            return TH_TGL;
        }
    for (int i = 0; i < 3; i++)
        if (x >= g.al[i].x && x < g.al[i].x + g.al[i].w && y >= g.al[i].y && y < g.al[i].y + g.al[i].h) {
            *idx = i;
            return TH_ALIGN;
        }
    for (int i = 0; i < g.nsl; i++)
        if (x >= g.sl[i].x - 4 && x < g.sl[i].x + g.sl[i].w + 4 && y >= g.sl[i].y && y < g.sl[i].y + g.sl[i].h) {
            *idx = i;
            return TH_SLIDER;
        }
    return TH_NONE;
}

static const char *tbar_font_label(struct app *a)
{
    return a->tfont_name[0] && text_family(a) >= 0 ? a->tfont_name : "Interface font";
}

static void paint_tbar(struct app *a)
{
    struct wlwin *w = &a->win;
    if (a->tool != T_TEXT)
        return;
    struct tbar_g g;
    tbar_layout(a, &g);
    wlwin_fill(w, WLRAIL_W, tb_y(a), w->bw - 2 * WLRAIL_W, TB_H, COL_BAR);
    wlwin_fill(w, WLRAIL_W, tb_y(a), w->bw - 2 * WLRAIL_W, 1, lm_theme.border);
    wlwin_drop(w, g.font.x, g.font.y, g.font.w, g.font.h, lm_button_radius(g.font.h / 2), lm_theme.btn_purple);
    char fl[96];
    snprintf(fl, sizeof(fl), "%s", tbar_font_label(a));
    wlwin_text(w, g.font.x + 12, g.font.y + (g.font.h - 2 - LM_FONT_H) / 2, fl, 1, lm_theme.on_colour, g.font.w - 36);
    wlwin_text(w, g.font.x + g.font.w - 18, g.font.y + (g.font.h - 2 - LM_FONT_H) / 2, "v", 1, lm_theme.on_colour, 0);
    for (int i = 0; i < 6; i++) {
        bool on = a->tg[g.tg[i].id];
        wlwin_drop(w, g.tg[i].x, g.tg[i].y, g.tg[i].w, g.tg[i].h, lm_button_radius(g.tg[i].h / 2), on ? lm_theme.btn_green : lm_theme.btn_grey);
        int tx = g.tg[i].x + (g.tg[i].w - wlcard_text_w(TB_TGL_LABEL[i])) / 2, ty = g.tg[i].y + (g.tg[i].h - 2 - LM_FONT_H) / 2;
        if (i == 0)
            wlwin_text_bold(w, tx, ty, TB_TGL_LABEL[i], 1, lm_theme.on_colour, 0);
        else
            wlwin_text(w, tx, ty, TB_TGL_LABEL[i], 1, lm_theme.on_colour, 0);
        if (i == 1)                                                     /* an italic I leans: a stroke across the letter's foot */
            wlwin_fill(w, g.tg[i].x + g.tg[i].w / 2 - 3, g.tg[i].y + g.tg[i].h - 7, 7, 1, lm_theme.on_colour);
        if (i == 2)
            wlwin_fill(w, tx, ty + LM_FONT_H - 1, wlcard_text_w(TB_TGL_LABEL[i]), 1, lm_theme.on_colour);
        if (i == 3)
            wlwin_fill(w, tx - 1, ty + LM_FONT_H / 2, wlcard_text_w(TB_TGL_LABEL[i]) + 2, 1, lm_theme.on_colour);
    }
    for (int i = 0; i < 3; i++) {                                       /* the alignment: three lines that lean left, to the middle or right */
        wlwin_drop(w, g.al[i].x, g.al[i].y, g.al[i].w, g.al[i].h, lm_button_radius(g.al[i].h / 2), a->talign == i ? lm_theme.btn_blue : lm_theme.btn_grey);
        static const int widths[3] = { 14, 9, 12 };
        for (int k = 0; k < 3; k++) {
            int lw = k == 1 ? widths[1] : k == 0 ? widths[0] : widths[2], lx = g.al[i].x + 8;
            if (i == 1)
                lx = g.al[i].x + (g.al[i].w - lw) / 2;
            else if (i == 2)
                lx = g.al[i].x + g.al[i].w - 8 - lw;
            wlwin_fill(w, lx, g.al[i].y + 6 + k * 5, lw, 2, lm_theme.on_colour);
        }
    }
    for (int i = 0; i < g.nsl; i++) {
        int id = g.sl[i].id, mn, mx;
        slider_range(a, id, &mn, &mx);
        paint_slider(a, g.sl[i].x, g.sl[i].y, g.sl[i].w, SLIDERS[id].name, *slider_ptr(a, id), mn, mx, SLIDERS[id].unit, a->sl_drag == id);
    }
}

static void font_open(struct app *a);

/* A press in the text bar. */
static void tbar_press(struct app *a, uint32_t button)
{
    int idx;
    int hit = tbar_hit(a, a->px, a->py, &idx);
    struct tbar_g g;
    tbar_layout(a, &g);
    if (button != 272 || hit == TH_NONE)
        return;
    switch (hit) {
    case TH_FONT: font_open(a); return;
    case TH_TGL: a->tg[g.tg[idx].id] = !a->tg[g.tg[idx].id]; break;
    case TH_ALIGN: a->talign = idx; break;
    case TH_SLIDER: {
        int id = g.sl[idx].id, mn, mx;
        slider_range(a, id, &mn, &mx);
        a->sl_drag = id;
        slider_set(a, id, slider_value_at(mn, mx, a->px, g.sl[idx].x, g.sl[idx].w));
        break;
    }
    default: break;
    }
    if (a->txt.on)
        text_preview(a);
    a->win.redraw = true;
}

/* ---- the brush bar: smoothing, symmetry and pixels for the pen, the airbrush, the eraser and the line --------------------------------------- */

struct bbar_g {
    int nsl;
    struct { int id, x, y, w, h; } sl[2];
    int ntg;
    struct { int id, x, y, w, h; } tg[2];
};

static void bbar_layout(const struct app *a, struct bbar_g *g)
{
    memset(g, 0, sizeof(*g));
    int y = tb_y(a), x = WLRAIL_W + 10, sw = LM_CLAMP((a->win.bw - 2 * WLRAIL_W - 20 - 2 * 14 - 150) / 2, 80, 190);
    if (a->tool != T_LINE) {
        g->sl[g->nsl].id = S_SMOOTH;
        g->sl[g->nsl].x = x;
        g->sl[g->nsl].y = y + 3;
        g->sl[g->nsl].w = sw;
        g->sl[g->nsl].h = 42;
        g->nsl++;
        x += sw + 14;
    }
    g->sl[g->nsl].id = S_SYM;
    g->sl[g->nsl].x = x;
    g->sl[g->nsl].y = y + 3;
    g->sl[g->nsl].w = sw;
    g->sl[g->nsl].h = 42;
    g->nsl++;
    x += sw + 20;
    static const int ids[2] = { TG_MIRROR, TG_PIXEL };
    for (int i = 0; i < 2; i++) {
        int w = LM_MAX(wlcard_text_w(TG_NAMES[ids[i]]) + 24, 56);
        g->tg[i].id = ids[i];
        g->tg[i].x = x;
        g->tg[i].y = y + (BB_H - 24) / 2;
        g->tg[i].w = w;
        g->tg[i].h = 24;
        x += w + 6;
        g->ntg++;
    }
}

static int bbar_hit(const struct app *a, double x, double y, int *idx)
{
    *idx = -1;
    if (!brush_bar_tool(a->tool) || y < tb_y(a) || y >= tb_y(a) + BB_H)
        return TH_NONE;
    struct bbar_g g;
    bbar_layout(a, &g);
    for (int i = 0; i < g.nsl; i++)
        if (x >= g.sl[i].x - 4 && x < g.sl[i].x + g.sl[i].w + 4 && y >= g.sl[i].y && y < g.sl[i].y + g.sl[i].h) {
            *idx = i;
            return TH_SLIDER;
        }
    for (int i = 0; i < g.ntg; i++)
        if (x >= g.tg[i].x && x < g.tg[i].x + g.tg[i].w && y >= g.tg[i].y && y < g.tg[i].y + g.tg[i].h) {
            *idx = i;
            return TH_TGL;
        }
    return TH_NONE;
}

static void paint_bbar(struct app *a)
{
    struct wlwin *w = &a->win;
    if (!brush_bar_tool(a->tool))
        return;
    struct bbar_g g;
    bbar_layout(a, &g);
    wlwin_fill(w, WLRAIL_W, tb_y(a), w->bw - 2 * WLRAIL_W, BB_H, COL_BAR);
    wlwin_fill(w, WLRAIL_W, tb_y(a), w->bw - 2 * WLRAIL_W, 1, lm_theme.border);
    for (int i = 0; i < g.nsl; i++) {
        int id = g.sl[i].id, mn, mx;
        slider_range(a, id, &mn, &mx);
        paint_slider(a, g.sl[i].x, g.sl[i].y, g.sl[i].w, SLIDERS[id].name, *slider_ptr(a, id), mn, mx, SLIDERS[id].unit, a->sl_drag == id);
    }
    for (int i = 0; i < g.ntg; i++) {
        bool on = a->tg[g.tg[i].id];
        wlwin_drop(w, g.tg[i].x, g.tg[i].y, g.tg[i].w, g.tg[i].h, lm_button_radius(g.tg[i].h / 2), on ? lm_theme.btn_green : lm_theme.btn_grey);
        const char *t = TG_NAMES[g.tg[i].id];
        wlwin_text(w, g.tg[i].x + (g.tg[i].w - wlcard_text_w(t)) / 2, g.tg[i].y + (g.tg[i].h - 2 - LM_FONT_H) / 2, t, 1, lm_theme.on_colour, 0);
    }
}

static void bbar_press(struct app *a, uint32_t button)
{
    int idx;
    int hit = bbar_hit(a, a->px, a->py, &idx);
    struct bbar_g g;
    bbar_layout(a, &g);
    if (button != 272 || hit == TH_NONE)
        return;
    if (hit == TH_TGL)
        a->tg[g.tg[idx].id] = !a->tg[g.tg[idx].id];
    else {
        int id = g.sl[idx].id, mn, mx;
        slider_range(a, id, &mn, &mx);
        a->sl_drag = id;
        slider_set(a, id, slider_value_at(mn, mx, a->px, g.sl[idx].x, g.sl[idx].w));
    }
    a->win.redraw = true;
}

/* ---- the font card ---------------------------------------------------------------------------------------------------------------- */

static int font_rows_fit(const struct app *a)
{
    return LM_CLAMP((a->win.bh - 16 - 110) / FONT_ROW_H, 3, 9);
}

static int font_current(struct app *a)
{
    int f = a->tfont_name[0] ? text_family(a) : -1;
    return f < 0 ? 0 : f + 1;
}

static void font_show(struct app *a)
{
    int rows = font_rows_fit(a), n = g_nfam + 1;
    if (a->font_sel < a->font_top)
        a->font_top = a->font_sel;
    if (a->font_sel >= a->font_top + rows)
        a->font_top = a->font_sel - rows + 1;
    a->font_top = LM_CLAMP(a->font_top, 0, LM_MAX(n - rows, 0));
}

static void font_open(struct app *a)
{
    fonts_scan();
    a->font_find[0] = 0;
    a->prompt = P_FONT;
    a->perr[0] = 0;
    a->pcard_hover = a->pcard_down = -1;
    a->font_sel = font_current(a);
    font_show(a);
    a->win.redraw = true;
}

static void font_choose(struct app *a, int item)
{
    if (item < 0 || item > g_nfam)
        return;
    if (item == 0)
        a->tfont_name[0] = 0;
    else
        snprintf(a->tfont_name, sizeof(a->tfont_name), "%s", g_fam[item - 1].name);
    a->prompt = P_NONE;
    if (a->txt.on)
        text_preview(a);
    a->win.redraw = true;
}

/* The top of row i of the card (the card's own place `c`). */
static int font_row_y(const struct wlcard *c, int i) { return c->y + 40 + i * FONT_ROW_H; }

/* The family's name set in the family's own letters (physical pixels, so it is sharp on a scaled screen). */
static void font_sample(struct app *a, int fam, const char *text, int x, int y, int size, int maxw, uint32_t col)
{
    int s = wlwin_scale(&a->win);
    struct tprov P;
    tprov_init(&P, fam, false, false, size * s);
    double pen = 0;
    for (int i = 0; text[i];) {
        int used = 1;
        uint32_t cp = lm_utf8_decode(text + i, (int)strlen(text + i), &used);
        i += used > 0 ? used : 1;
        pen += tprov_adv(&P, cp);
    }
    int w = LM_MIN((int)ceil(pen) + 6 * s, maxw * s), h = (int)ceil(P.lh0) + 8 * s;
    uint8_t *ink = calloc((size_t)w * (size_t)h, 1);
    uint32_t *px = malloc((size_t)w * (size_t)h * 4);
    if (!ink || !px) {
        free(ink);
        free(px);
        return;
    }
    pen = 2 * s;
    for (int i = 0; text[i];) {
        int used = 1;
        uint32_t cp = lm_utf8_decode(text + i, (int)strlen(text + i), &used);
        i += used > 0 ? used : 1;
        tprov_draw(&P, cp, ink, w, h, pen, 3 * s);
        pen += tprov_adv(&P, cp);
    }
    for (size_t i = 0; i < (size_t)w * (size_t)h; i++)
        px[i] = ((uint32_t)ink[i] << 24) | (col & 0xffffffu);
    wlwin_blit_px_over(&a->win, x, y, px, w, h, w);
    free(ink);
    free(px);
}

/* A line of a few pixels' width that stays inside the canvas (the text's frame and caret). */
static void canvas_line(struct app *a, double x0, double y0, double x1, double y1, int t, uint32_t col)
{
    double len = LM_MAX(fabs(x1 - x0), fabs(y1 - y0));
    int n = LM_MAX((int)ceil(len), 1);
    for (int i = 0; i <= n; i++) {
        int x = (int)lround(x0 + (x1 - x0) * i / n), y = (int)lround(y0 + (y1 - y0) * i / n);
        if (in_canvas(a, x, y))
            wlwin_fill(&a->win, x - t / 2, y - t / 2, t, t, col);
    }
}

/* The font card's scroll bar: the global one, at the right of the rows. */
static struct wlsbar font_sbar(const struct app *a, const struct wlcard *c)
{
    int rows = font_rows_fit(a);
    return (struct wlsbar){ c->x + c->w - WLCARD_PAD - wlsbar_w(), font_row_y(c, 0), wlsbar_w(), rows * FONT_ROW_H, g_nfam + 1, rows, a->font_top };
}

static void paint_font_card(struct app *a, const struct wlcard *c)
{
    struct wlwin *w = &a->win;
    int rows = font_rows_fit(a), n = g_nfam + 1, cur = font_current(a);
    bool typed = a->font_find[0] && now_ms() - a->font_find_ms < 3000;                    /* the title row: what was typed, or how to jump */
    const char *hint = typed ? a->font_find : n > rows ? "type a name to jump to it" : "";
    if (*hint) {
        int hw = wlcard_text_w(hint);
        wlwin_text(w, c->x + c->w - WLCARD_PAD - hw, c->y + WLCARD_PAD - 2, hint, 1, typed ? COL_ACCENT : COL_DIM, 0);
    }
    struct wlsbar fb = font_sbar(a, c);
    int rw = c->w - 2 * WLCARD_PAD - (wlsbar_on(&fb) ? wlsbar_w() + 4 : 0);                // (the rows end where the bar begins)
    for (int r = 0; r < rows && a->font_top + r < n; r++) {
        int it = a->font_top + r, y = font_row_y(c, r);
        bool hot = a->has_pointer && a->px >= c->x + WLCARD_PAD && a->px < c->x + WLCARD_PAD + rw && a->py >= y && a->py < y + FONT_ROW_H;
        if (it == a->font_sel || hot)
            wlwin_round_fill(w, c->x + WLCARD_PAD, y, rw, FONT_ROW_H - 2, 8, it == a->font_sel ? lm_theme.control_hot : lm_theme.control);
        if (it == 0)
            wlwin_text(w, c->x + WLCARD_PAD + 12, y + (FONT_ROW_H - 2 - LM_FONT_H) / 2, "Interface font (the program's own)", 1, lm_theme.text_bright, rw - 40);
        else
            font_sample(a, it - 1, g_fam[it - 1].name, c->x + WLCARD_PAD + 12, y + 2, 20, rw - 44, lm_theme.text_bright);
        if (it == cur)
            wlwin_round_fill(w, c->x + WLCARD_PAD + rw - 22, y + (FONT_ROW_H - 12) / 2, 10, 10, 5, COL_ACCENT);
    }
    wlsbar_paint(w, &fb, a->font_grab >= 0 || (a->has_pointer && wlsbar_has(&fb, a->px, a->py)));
}

/* ---- the layers panel ------------------------------------------------------------------------------------------------------------ */

#define LP_INDENT 12                                                    /* how far a row moves right for every group it is in */

enum { LB_NEW, LB_DUP, LB_DEL, LB_UP, LB_DOWN, LB_MERGE, LB_COUNT };
static const char *const LB_NAMES[LB_COUNT] = { "New", "Copy", "Delete", "Up", "Down", "Merge" };
static const char *const LB_HINTS[LB_COUNT] = { "A new empty layer above this one (Ctrl+Shift+N)", "Duplicate the layer (Ctrl+J)", "Delete the layer",
                                                "Move the layer up the stack", "Move the layer down the stack", "Merge the layer into the one below (Ctrl+E)" };

static const char *const LB_GHINTS[LB_COUNT] = { "A new empty layer on top of the group", "Duplicate the whole group", "Delete the group with all its layers",
                                                 "Move the group up the stack", "Move the group down the stack", "Merge the group into one layer" };

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

/* The panel's list, top of the stack first: a group has a row of its own above its layers, and what is in a collapsed group has none. */
struct lrow {
    int layer;                                  /* a layer's index, or -1 for a group's row */
    int group;                                  /* a group row's group (slot + 1) */
    int depth;                                  /* how many groups it is in */
};

static int lp_rows(const struct app *a, struct lrow *rows)
{
    const struct doc *d = &a->doc;
    int n = 0;
    bool done[MAX_GROUPS + 1] = { false };
    for (int i = d->n - 1; i >= 0; i--) {
        int ch[MAX_GROUPS], cl = gchain(d, d->L[i].gid, ch);
        bool hidden = false;                                            /* inside a collapsed group */
        for (int k = 0; k < cl; k++) {
            if (!done[ch[k]]) {
                done[ch[k]] = true;
                if (!hidden)
                    rows[n++] = (struct lrow){ -1, ch[k], k };
            }
            if (d->G[ch[k] - 1].collapsed)
                hidden = true;
        }
        if (!hidden)
            rows[n++] = (struct lrow){ i, 0, cl };
    }
    return n;
}

/* The row the chosen layer (or group) is on; a layer inside a collapsed group is "on" the group's row. */
static int lp_cur_row(const struct app *a, const struct lrow *rows, int n)
{
    int want_g = a->gsel;
    if (!want_g) {
        for (int i = 0; i < n; i++)
            if (rows[i].layer == a->doc.cur)
                return i;
        int ch[MAX_GROUPS], cl = gchain(&a->doc, a->doc.L[a->doc.cur].gid, ch);
        for (int k = 0; k < cl; k++)
            if (a->doc.G[ch[k] - 1].collapsed) {
                want_g = ch[k];
                break;
            }
    }
    for (int i = 0; i < n; i++)
        if (rows[i].layer < 0 && rows[i].group == want_g)
            return i;
    return 0;
}

/* The panel's opacity and blend mode belong to the chosen group, or else to the chosen layer. */
static int *lay_opacity(struct app *a) { return a->gsel ? &a->doc.G[a->gsel - 1].opacity : &a->doc.L[a->doc.cur].opacity; }
static int lay_blend(const struct app *a) { return a->gsel ? a->doc.G[a->gsel - 1].blend : a->doc.L[a->doc.cur].blend; }

static void lay_show(struct app *a)
{
    if (!layers_on(a))
        return;
    struct lp_g g;
    lp_geom(a, &g);
    struct lrow rows[MAX_LAYERS + MAX_GROUPS];
    int nr = lp_rows(a, rows), row = lp_cur_row(a, rows, nr), nv = LM_MAX(g.nvis, 1);
    if (row < a->lay_top)
        a->lay_top = row;
    if (row >= a->lay_top + nv)
        a->lay_top = row - nv + 1;
    a->lay_top = LM_CLAMP(a->lay_top, 0, LM_MAX(nr - nv, 0));
}

/* The layers panel's scroll bar: the global one, at the right edge of the list (the rows end before it while it is there). */
static struct wlsbar lp_sbar(const struct lp_g *g, int nr, int top)
{
    return (struct wlsbar){ g->x + LP_W - wlsbar_w() - 2, g->ly, wlsbar_w(), g->nvis * LP_ROW_H, nr, g->nvis, top };
}

enum { LH_NONE, LH_BTN, LH_OPACITY, LH_BLEND, LH_ROW, LH_EYE, LH_MASK, LH_GROUP, LH_GEYE, LH_GFOLD, LH_BAR };

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
        struct lrow rows[MAX_LAYERS + MAX_GROUPS];
        int nr = lp_rows(a, rows), i = (int)((y - g.ly) / LP_ROW_H), ri = a->lay_top + i;
        struct wlsbar sb = lp_sbar(&g, nr, a->lay_top);
        if (wlsbar_has(&sb, x, y))
            return LH_BAR;
        if (ri < 0 || ri >= nr)
            return LH_NONE;
        if (rows[ri].layer < 0) {                                       /* a group's row: the eye, the fold arrow, or the row */
            *idx = rows[ri].group;
            if (x < g.x + 30)
                return LH_GEYE;
            int ax = g.x + 32 + rows[ri].depth * LP_INDENT;
            return x >= ax && x < ax + 16 ? LH_GFOLD : LH_GROUP;
        }
        int layer = rows[ri].layer;
        *idx = layer;
        int ind = rows[ri].depth * LP_INDENT;
        if (a->doc.L[layer].mask.p && x >= g.x + 80 + ind && x < g.x + 110 + ind)
            return LH_MASK;
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

/* Whether the chosen item (the group when one is chosen, else the layer) has a neighbour on its level to change places with. */
static bool can_move(const struct app *a, int dir)
{
    int lo, hi, level, slo, shi, sg;
    return item_block(&a->doc, a->doc.cur, a->gsel, &lo, &hi, &level) && item_sibling(&a->doc, lo, hi, level, dir, &slo, &shi, &sg);
}

/* A folder: a tab and a body. */
static void folder_icon(struct wlwin *w, int x, int y, uint32_t col)
{
    wlwin_round_fill(w, x, y, 8, 4, 1, col);
    wlwin_round_fill(w, x, y + 2, 16, 11, 2, col);
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
    wlwin_text_bold(w, g.x + 10, 6, a->gsel ? "Group" : "Layers", 1, lm_theme.text_bright, LP_W - 20);
    char info[32];
    snprintf(info, sizeof(info), "%d / %d", a->doc.n - a->doc.cur, a->doc.n);
    wlwin_text(w, g.x + LP_W - 10 - wlcard_text_w(info), 6, info, 1, COL_DIM, 0);
    for (int i = 0; i < LB_COUNT; i++) {
        bool idle = (i == LB_DEL && !a->gsel && a->doc.n <= 1) || (i == LB_UP && !can_move(a, 1)) || (i == LB_DOWN && !can_move(a, -1)) ||
                    (i == LB_MERGE && !a->gsel && (a->doc.cur <= 0 || a->doc.L[a->doc.cur].gid != a->doc.L[a->doc.cur - 1].gid));
        uint32_t c = idle ? lm_theme.btn_grey : i == LB_NEW ? lm_theme.btn_green : i == LB_DEL ? lm_theme.btn_rust : i == LB_DUP ? lm_theme.btn_blue : lm_theme.btn_slate;
        wlwin_drop(w, g.btn[i][0], g.btn[i][1], g.btn[i][2], g.btn[i][3], lm_button_radius(g.btn[i][3] / 2), c);
        wlwin_text(w, g.btn[i][0] + (g.btn[i][2] - wlcard_text_w(LB_NAMES[i])) / 2, g.btn[i][1] + (g.btn[i][3] - 2 - LM_FONT_H) / 2, LB_NAMES[i], 1, lm_theme.on_colour, 0);
    }
    paint_slider(a, g.ox, g.oy, g.ow, "Opacity", *lay_opacity(a), 0, 100, "%", a->sl_drag == 100);
    wlwin_drop(w, g.bx, g.by, g.bw, g.bh, lm_button_radius(g.bh / 2), lm_theme.btn_purple);
    char bl[48];
    snprintf(bl, sizeof(bl), "Blend: %s", bm_name(lay_blend(a)));
    wlwin_text(w, g.bx + (g.bw - wlcard_text_w(bl)) / 2, g.by + (g.bh - 2 - LM_FONT_H) / 2, bl, 1, lm_theme.on_colour, 0);
    wlwin_fill(w, g.x + 6, g.ly - 4, LP_W - 12, 1, lm_theme.border);
    int tw = 44, th = 30;
    uint32_t *tb = malloc((size_t)tw * s * (size_t)th * s * 4);
    struct lrow rows[MAX_LAYERS + MAX_GROUPS];
    int nr = lp_rows(a, rows);
    struct wlsbar lsb = lp_sbar(&g, nr, a->lay_top);
    int rs = wlsbar_on(&lsb) ? wlsbar_w() + 4 : 0;                      /* (the rows end where the scroll bar begins) */
    for (int i = 0; i < g.nvis; i++) {
        int ri = a->lay_top + i;
        if (ri >= nr)
            break;
        int y = g.ly + i * LP_ROW_H, ind = rows[ri].depth * LP_INDENT;
        if (rows[ri].layer < 0) {                                       /* a group */
            const struct lgroup *gr = &a->doc.G[rows[ri].group - 1];
            bool is_cur = a->gsel == rows[ri].group;
            if (is_cur)
                wlwin_round_fill(w, g.x + 4, y + 1, LP_W - 8 - rs, LP_ROW_H - 2, 8, lm_theme.control_hot);
            int ex = g.x + 10, ey = y + (LP_ROW_H - 16) / 2;
            wlwin_round_fill(w, ex, ey, 16, 16, 5, lm_theme.border);
            wlwin_round_fill(w, ex + 1, ey + 1, 14, 14, 4, COL_FIELD);
            if (gr->visible)
                wlwin_round_fill(w, ex + 4, ey + 4, 8, 8, 3, is_cur ? lm_theme.text_bright : COL_TEXT);
            int ax = g.x + 32 + ind, ay = y + (LP_ROW_H - 10) / 2;      /* the fold arrow: points right when closed, down when open */
            uint32_t ac = is_cur ? lm_theme.text_bright : COL_TEXT;
            if (gr->collapsed)
                for (int k = 0; k < 5; k++)
                    wlwin_fill(w, ax + 2 + k, ay + k, 1, 10 - 2 * k, ac);
            else
                for (int k = 0; k < 5; k++)
                    wlwin_fill(w, ax + k, ay + 2 + k, 10 - 2 * k, 1, ac);
            folder_icon(w, ax + 16, y + (LP_ROW_H - 13) / 2, gr->visible ? lm_theme.btn_amber : lm_theme.btn_grey);
            char nm[64];
            snprintf(nm, sizeof(nm), "%s", gr->name);
            if (gr->opacity < 100 || (gr->blend != BM_PASS))
                snprintf(nm + strlen(nm), sizeof(nm) - strlen(nm), " (%d%%)", gr->opacity);
            wlwin_text_bold(w, ax + 38, y + (LP_ROW_H - LM_FONT_H) / 2, nm, 1, gr->visible ? (is_cur ? lm_theme.text_bright : COL_TEXT) : COL_DIM, LP_W - (ax + 38 - g.x) - 6 - rs);
            continue;
        }
        int layer = rows[ri].layer;
        const struct layer *l = &a->doc.L[layer];
        bool is_cur = layer == a->doc.cur && !a->gsel;
        if (is_cur)
            wlwin_round_fill(w, g.x + 4, y + 1, LP_W - 8 - rs, LP_ROW_H - 2, 8, lm_theme.control_hot);
        int ex = g.x + 10, ey = y + (LP_ROW_H - 16) / 2;
        wlwin_round_fill(w, ex, ey, 16, 16, 5, lm_theme.border);
        wlwin_round_fill(w, ex + 1, ey + 1, 14, 14, 4, COL_FIELD);
        if (l->visible)
            wlwin_round_fill(w, ex + 4, ey + 4, 8, 8, 3, is_cur ? lm_theme.text_bright : COL_TEXT);
        if (ind)                                                        /* a line down the side marks what is in a group */
            wlwin_fill(w, g.x + 32 + ind - 5, y, 1, LP_ROW_H, lm_theme.border);
        int tx = g.x + 32 + ind, ty = y + (LP_ROW_H - th) / 2;
        if (tb) {
            layer_thumb(l, tb, tw * s, th * s);
            wlwin_round_fill(w, tx - 1, ty - 1, tw + 2, th + 2, 2, lm_theme.border);
            wlwin_blit_px(w, tx, ty, tb, tw * s, th * s, tw * s, tx, ty, tw, th);
        }
        int nx = g.x + 82 + ind;
        if (l->mask.p && l->mask.w > 0 && l->mask.h > 0) {                 /* the mask's own small picture: white shows, black hides; lit while the tools paint on it */
            int mw = 26, mh = 26, mx = g.x + 80 + ind, my = y + (LP_ROW_H - mh) / 2;
            bool editing = a->mask_edit == layer;
            wlwin_round_fill(w, mx - 1, my - 1, mw + 2, mh + 2, 2, editing ? lm_theme.accent : lm_theme.border);
            uint32_t *mb = malloc((size_t)mw * s * (size_t)mh * s * 4);
            if (mb) {
                for (int yy = 0; yy < mh * s; yy++)
                    for (int xx = 0; xx < mw * s; xx++) {
                        int v = mask_val(l->mask.p[(size_t)(yy * l->mask.h / (mh * s)) * (size_t)l->mask.w + (size_t)(xx * l->mask.w / (mw * s))]);
                        mb[(size_t)yy * (size_t)mw * s + (size_t)xx] = argb(255, v, v, v);
                    }
                wlwin_blit_px(w, mx, my, mb, mw * s, mh * s, mw * s, mx, my, mw, mh);
                free(mb);
            }
            nx = g.x + 112 + ind;
        }
        wlwin_text(w, nx, y + (LP_ROW_H - LM_FONT_H) / 2, l->name, 1, l->visible ? (is_cur ? lm_theme.text_bright : COL_TEXT) : COL_DIM, LP_W - (nx - g.x) - 6 - rs);
    }
    free(tb);
    wlsbar_paint(w, &lsb, a->lay_grab >= 0 || (a->has_pointer && wlsbar_has(&lsb, a->px, a->py)));
}

/* ---- the status line ------------------------------------------------------------------------------------------------------------- */

static const char *hover_hint(const struct app *a)
{
    if (!a->has_pointer)
        return NULL;
    int idx;
    int lh = lp_hit(a, a->px, a->py, &idx);
    if (lh == LH_BTN)
        return a->gsel ? LB_GHINTS[idx] : LB_HINTS[idx];
    if (lh == LH_OPACITY)
        return a->gsel ? "The opacity of the group: it is laid over as one picture (pass through at 100 % mixes the layers in it with what is below)" : "The opacity of the layer: 0 hides it, 100 shows it as it is";
    if (lh == LH_BLEND)
        return a->gsel ? "How the group mixes with what is below it (Pass through: its layers mix one by one)" : "How the layer mixes with the ones below it";
    if (lh == LH_GEYE)
        return "Show or hide the group";
    if (lh == LH_GFOLD)
        return "Fold the group (hide its layers in the list) or unfold it";
    if (lh == LH_GROUP)
        return "A group of layers: click to choose it (the buttons and sliders then act on all of it); double click renames; right click for more";
    if (lh == LH_EYE)
        return "Show or hide the layer";
    if (lh == LH_MASK)
        return "The layer's mask: click to paint on it (black hides, white shows); click again for the layer";
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

/* ---- rulers, guides and snapping ----------------------------------------------------------------------------------------------------- */

#define RULER 16                                                        /* the top ruler's height */
#define RULER_W 28                                                      /* the left one's width (it holds the numbers) */

/* The rulers' units. A picture has no size in the world of its own, so what an inch is comes from the resolution: dpi pixels. */
enum { RU_PX, RU_MM, RU_CM, RU_IN, RU_PT, RU_COUNT };
static const char *const RU_ABBR[RU_COUNT] = { "px", "mm", "cm", "in", "pt" };
static const char *const RU_NAME[RU_COUNT] = { "Pixels", "Millimetres", "Centimetres", "Inches", "Points" };
static const int RU_DPI[4] = { 72, 96, 150, 300 };

/* How many pixels one unit is. */
static double ruler_ppu(const struct app *a)
{
    double dpi = a->ruler_dpi > 0 ? a->ruler_dpi : 96;
    switch (a->ruler_unit) {
    case RU_MM: return dpi / 25.4;
    case RU_CM: return dpi / 2.54;
    case RU_IN: return dpi;
    case RU_PT: return dpi / 72.0;
    default: return 1.0;
    }
}

/* The spacing of the numbers on a ruler (in units) and of the small ticks between them, for a picture shown at `zoom`: the first step that is 56 screen pixels or more. */
static void ruler_steps(const struct app *a, double *step, double *minor)
{
    static const double steps[] = { 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000, 50000, 100000 };
    double spu = ruler_ppu(a) * a->zoom;
    *step = steps[sizeof(steps) / sizeof(steps[0]) - 1];
    for (size_t k = 0; k < sizeof(steps) / sizeof(steps[0]); k++) {
        if (a->ruler_unit == RU_PX && steps[k] < 1)
            continue;                                                   /* (no tenth of a pixel) */
        if (steps[k] * spu >= 56) {
            *step = steps[k];
            break;
        }
    }
    static const int div[] = { 10, 5, 2 };
    *minor = *step;
    for (int i = 0; i < 3; i++) {
        double m = *step / div[i];
        if (m * spu >= 5 && (a->ruler_unit != RU_PX || m >= 1)) {
            *minor = m;
            break;
        }
    }
}

/* A length in pixels as the rulers say it ("12.7 mm"). */
static void ruler_text(const struct app *a, double px, char *out, size_t n)
{
    double v = px / ruler_ppu(a);
    snprintf(out, n, a->ruler_unit == RU_PX ? "%.0f %s" : "%.2f %s", v, RU_ABBR[a->ruler_unit]);
}

static bool tool_snaps(enum tool t)
{
    return tool_in_select(t) || tool_in_shapes(t) || t == T_GRADIENT || t == T_TEXT;
}

/* A point pulled to a guide, an edge or the middle of the picture when it is within a few screen pixels of one. */
static void snap_pic(const struct app *a, double *ix, double *iy)
{
    if (!a->snap)
        return;
    double th = 6.0 / a->zoom, bx = th, by = th, tx = *ix, ty = *iy;
    double xs[40], ys[40];
    int nx = 0, ny = 0;
    xs[nx++] = 0;
    xs[nx++] = CW(a);
    xs[nx++] = CW(a) / 2.0;
    ys[ny++] = 0;
    ys[ny++] = CH(a);
    ys[ny++] = CH(a) / 2.0;
    for (int i = 0; a->rulers && i < a->nguides; i++) {
        if (a->guide[i].vert)
            xs[nx++] = a->guide[i].pos;
        else
            ys[ny++] = a->guide[i].pos;
    }
    for (int i = 0; i < nx; i++)
        if (fabs(*ix - xs[i]) < bx) {
            bx = fabs(*ix - xs[i]);
            tx = xs[i];
        }
    for (int i = 0; i < ny; i++)
        if (fabs(*iy - ys[i]) < by) {
            by = fabs(*iy - ys[i]);
            ty = ys[i];
        }
    *ix = tx;
    *iy = ty;
}

/* The guide under a point of the window (within 3 pixels), -1 for none. */
static int guide_at(const struct app *a, double sx, double sy)
{
    if (!a->rulers || !in_canvas(a, sx, sy))
        return -1;
    for (int i = 0; i < a->nguides; i++) {
        double p = a->guide[i].vert ? cv_x(a) + a->vx + a->guide[i].pos * a->zoom : a->vy + a->guide[i].pos * a->zoom;
        if (fabs((a->guide[i].vert ? sx : sy) - p) <= 3.0)
            return i;
    }
    return -1;
}

/* Which ruler a point of the window is on: 1 the top one, 2 the left one, 0 neither. */
static int ruler_at(const struct app *a, double sx, double sy)
{
    if (!a->rulers || !in_canvas(a, sx, sy))
        return 0;
    if (sy < RULER && sx < cv_x(a) + RULER_W)
        return 3;                                                       /* the corner: the unit */
    if (sy < RULER && sx >= cv_x(a) + RULER_W)
        return 1;
    if (sx < cv_x(a) + RULER_W && sy >= RULER)
        return 2;
    return 0;
}

static void paint_guides_and_rulers(struct app *a)
{
    if (!a->rulers)
        return;
    struct wlwin *w = &a->win;
    for (int i = 0; i < a->nguides; i++) {
        bool vert = a->guide[i].vert;
        int p = (int)lround(vert ? cv_x(a) + a->vx + a->guide[i].pos * a->zoom : a->vy + a->guide[i].pos * a->zoom);
        uint32_t col = a->guide_drag == i ? 0xffcc66 : 0x33ccff;
        if (vert && p >= cv_x(a) && p < cv_x(a) + cv_w(a))
            wlwin_fill(w, p, 0, 1, cv_h(a), col);
        else if (!vert && p >= 0 && p < cv_h(a))
            wlwin_fill(w, cv_x(a), p, cv_w(a), 1, col);
    }
    wlwin_fill(w, cv_x(a), 0, cv_w(a), RULER, COL_BAR);
    wlwin_fill(w, cv_x(a), 0, RULER_W, cv_h(a), COL_BAR);
    {                                                                   /* the part of each ruler over the picture itself is a little lighter */
        int pxa = (int)lround(cv_x(a) + a->vx), pxb = (int)lround(cv_x(a) + a->vx + CW(a) * a->zoom), pya = (int)lround(a->vy), pyb = (int)lround(a->vy + CH(a) * a->zoom);
        int x0 = LM_MAX(pxa, cv_x(a) + RULER_W), x1 = LM_MIN(pxb, cv_x(a) + cv_w(a)), y0 = LM_MAX(pya, RULER), y1 = LM_MIN(pyb, cv_h(a));
        if (x1 > x0)
            wlwin_fill(w, x0, 0, x1 - x0, RULER, lm_theme.control);
        if (y1 > y0)
            wlwin_fill(w, cv_x(a), y0, RULER_W, y1 - y0, lm_theme.control);
    }
    wlwin_fill(w, cv_x(a), RULER - 1, cv_w(a), 1, lm_theme.border);
    wlwin_fill(w, cv_x(a) + RULER_W - 1, 0, 1, cv_h(a), lm_theme.border);
    double step, minor, ppu = ruler_ppu(a);
    ruler_steps(a, &step, &minor);
    int nper = (int)lround(step / minor), dec = step >= 1 ? 0 : step >= 0.1 ? 1 : 2;
    if (a->ruler_unit != RU_PX && step < 10 && dec == 0 && fmod(step, 1.0) != 0)
        dec = 1;
    double ix0, iy0;
    to_pic(a, cv_x(a), 0, &ix0, &iy0);
    for (int axis = 0; axis < 2; axis++) {
        double first = (axis == 0 ? ix0 : iy0) / ppu, last = first + (axis == 0 ? cv_w(a) : cv_h(a)) / a->zoom / ppu;
        long k0 = (long)floor(first / minor);
        for (long k = k0; k * minor <= last + 1e-9; k++) {              /* (like Photoshop's: the numbers go on round the picture, below zero and past its far edge) */
            double pos = k * minor;                                     /* in units */
            int s = (int)lround(axis == 0 ? cv_x(a) + a->vx + pos * ppu * a->zoom : a->vy + pos * ppu * a->zoom);
            bool major = nper > 0 && k % nper == 0;
            char t[24];
            snprintf(t, sizeof(t), "%.*f", dec, pos);
            if (axis == 0 && s >= cv_x(a) + RULER_W && s < cv_x(a) + cv_w(a)) {
                wlwin_fill(w, s, major ? 3 : RULER - 5, 1, major ? RULER - 4 : 4, COL_TEXT);
                if (major)
                    wlwin_text_small(w, s + 3, 0, t, 9, COL_DIM, 60);
            } else if (axis == 1 && s >= RULER && s < cv_h(a)) {
                wlwin_fill(w, cv_x(a) + (major ? 3 : RULER_W - 5), s, major ? RULER_W - 4 : 4, 1, COL_TEXT);
                if (major)
                    wlwin_text_small(w, cv_x(a) + 1, s + 1, t, 9, COL_DIM, RULER_W - 2);
            }
        }
    }
    wlwin_text_small(w, cv_x(a) + 3, 1, RU_ABBR[a->ruler_unit], 9, COL_ACCENT, RULER_W - 4);        /* the unit stands in the corner (a click there changes it) */
    if (a->has_pointer && in_canvas(a, a->px, a->py)) {                 /* where the pointer is */
        wlwin_fill(w, (int)a->px, 0, 1, RULER, COL_ACCENT);
        wlwin_fill(w, cv_x(a), (int)a->py, RULER_W, 1, COL_ACCENT);
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
    {
        char ux[24] = "", uy[24] = "", where[80];
        if (a->rulers && a->ruler_unit != RU_PX) {                      /* the rulers' unit too */
            ruler_text(a, ix, ux, sizeof(ux));
            ruler_text(a, iy, uy, sizeof(uy));
            snprintf(where, sizeof(where), "%d, %d (%s, %s)", (int)ix, (int)iy, ux, uy);
        } else
            snprintf(where, sizeof(where), "%d, %d", (int)ix, (int)iy);
        snprintf(right, sizeof(right), "%s     %d x %d     %d %%     layer %d / %d%s", where, CW(a), CH(a), (int)(a->zoom * 100.0 + 0.5), a->doc.cur + 1, a->doc.n, a->mask_edit == a->doc.cur ? " (mask)" : "");
    }
    else
        snprintf(right, sizeof(right), "%d x %d     %d %%     layer %d / %d%s", CW(a), CH(a), (int)(a->zoom * 100.0 + 0.5), a->doc.cur + 1, a->doc.n, a->mask_edit == a->doc.cur ? " (mask)" : "");
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
    if (a->pan || (a->space_down && in_canvas(a, a->px, a->py)))
        return CUR_MOVE;
    if (!in_canvas(a, a->px, a->py) && !a->drag)
        return CUR_ARROW;
    if (a->hover >= 0 && !a->drag)
        return CUR_ARROW;
    switch (a->tool) {
    case T_SELECT: case T_ELLIPSE_SEL: return CUR_CROSS;
    case T_LASSO: return CUR_LASSO;
    case T_WAND: return CUR_WAND;
    case T_MOVE: case T_HAND: return CUR_MOVE;
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
    case T_FREEPOLY: return CUR_POLY;
    case T_CURVE: return CUR_LINE;
    case T_CLONE: return CUR_CROSS;
    case T_SMUDGE: case T_BLURBR: return CUR_DOT;
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
        snprintf(title, sizeof(title), "Help: %s", HP[a->help_page].title);
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
    case P_EFFECTS:
        snprintf(title, sizeof(title), "Effects%s", a->sel.on ? " (on the selection)" : "");
        labels[0] = "Close";
        nb = 1;
        field = false;
        break;
    case P_FONT:
        snprintf(title, sizeof(title), "Font of the text");
        labels[0] = "Cancel";
        nb = 1;
        field = false;
        break;
    case P_ADJUST:
        snprintf(title, sizeof(title), "%s%s", FXK[a->fxk].title, FXK[a->fxk].mode == FM_LAYER && a->sel.on ? " (the selection)" : "");
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
    bool dims = a->prompt != P_ADJUST && a->prompt != P_EFFECTS;        /* (the effect panels leave the picture as it is) */
    wlcard_frame_at(w, &c, geo.x, geo.y, geo.w, geo.h, title, COL_ACCENT, COL_BG, lm_theme.text_bright, dims);
    if (a->prompt == P_ADJUST) {                                        /* a grip in the title: it can be dragged */
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 2; j++)
                wlwin_round_fill(w, c.x + c.w - WLCARD_PAD - 22 + i * 8, c.y + 12 + j * 6, 4, 4, 2, a->adj_moving ? COL_ACCENT : COL_DIM);
    }
    if (a->prompt == P_HELP) {
        paint_help(a, &c);
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
    } else if (a->prompt == P_EFFECTS) {
        for (int i = 0; i < N_EFX; i++) {
            int x, y, bw, bh;
            efx_rect(&c, i, &x, &y, &bw, &bh);
            uint32_t col = EFX[i].group == 0 ? lm_theme.btn_teal : EFX[i].group == 1 ? lm_theme.btn_blue : EFX[i].group == 2 ? lm_theme.btn_purple : lm_theme.btn_orange;
            bool dim = (EFX[i].kind == K_FEATHER || EFX[i].kind == K_GROW) && !a->sel.on;
            wlcard_drop(w, x, y, bw, bh, dim ? lm_theme.btn_grey : col, a->has_pointer && a->px >= x && a->px < x + bw && a->py >= y && a->py < y + bh, false, EFX[i].label, lm_theme.on_colour);
        }
    } else if (a->prompt == P_FONT) {
        paint_font_card(a, &c);
    } else if (a->prompt == P_ADJUST) {
        for (int i = 0; i < FXK[a->fxk].n; i++) {
            int tx, ty, tw;
            adj_track(&c, i, &tx, &ty, &tw);
            int v = *adj_ptr(a, i), mn = FXK[a->fxk].p[i].mn, mx = FXK[a->fxk].p[i].mx;
            wlwin_text(w, c.x + WLCARD_PAD, ty - 7, FXK[a->fxk].p[i].name, 1, COL_TEXT, 100);
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
    if (a->tb_seen != tbar_h(a)) {                                      /* the text bar came or went: the picture's room changed */
        a->tb_seen = tbar_h(a);
        if (a->fitted)
            view_fit(a);
        else
            view_clamp(a);
        a->vdirty = true;
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
    if (a->txt.on) {                                                    /* the text's frame (turned with it) and its caret */
        struct tmetrics m;
        text_build(a, NULL, &m);
        double cx[4] = { -2, m.w + 2, m.w + 2, -2 }, cy[4] = { -2, -2, m.h + 2, m.h + 2 };
        int steps = m.bs ? 24 : 1;                                      /* a bent text's frame bends with it */
        for (int i = 0; i < 4; i++)
            for (int k = 0; k < steps; k++) {
                double t0 = (double)k / steps, t1 = (double)(k + 1) / steps, p0x, p0y, p1x, p1y;
                tm_to_pic(&m, cx[i] + (cx[(i + 1) % 4] - cx[i]) * t0, cy[i] + (cy[(i + 1) % 4] - cy[i]) * t0, &p0x, &p0y);
                tm_to_pic(&m, cx[i] + (cx[(i + 1) % 4] - cx[i]) * t1, cy[i] + (cy[(i + 1) % 4] - cy[i]) * t1, &p1x, &p1y);
                double ax = cv_x(a) + a->vx + p0x * a->zoom, ay = a->vy + p0y * a->zoom, bx = cv_x(a) + a->vx + p1x * a->zoom, by = a->vy + p1y * a->zoom;
                canvas_line(a, ax, ay, bx, by, 3, 0x000000);
                canvas_line(a, ax, ay, bx, by, 1, 0xffffff);
            }
        double p0x, p0y, p1x, p1y;
        tm_to_pic(&m, m.cax, m.cay, &p0x, &p0y);
        tm_to_pic(&m, m.cax, m.cay + m.lh, &p1x, &p1y);
        double x0 = cv_x(a) + a->vx + p0x * a->zoom, y0 = a->vy + p0y * a->zoom, x1 = cv_x(a) + a->vx + p1x * a->zoom, y1 = a->vy + p1y * a->zoom;
        canvas_line(a, x0, y0, x1, y1, 3, 0x000000);
        canvas_line(a, x0, y0, x1, y1, 1, 0xffffff);
    }
    if (brush_bar_tool(a->tool) && sym_count(a) > 1) {                  /* where the copies of a stroke come from */
        double cx = CW(a) / 2.0, cy = CH(a) / 2.0, reach = hypot((double)CW(a), (double)CH(a));
        int n = a->val[S_SYM];
        for (int k = 0; k < (n > 1 ? n : 0); k++) {
            double ang = 6.2831853 * k / n - 1.5707963;
            canvas_line(a, cv_x(a) + a->vx + cx * a->zoom, a->vy + cy * a->zoom, cv_x(a) + a->vx + (cx + cos(ang) * reach) * a->zoom, a->vy + (cy + sin(ang) * reach) * a->zoom, 1, 0x66ccff);
        }
        if (a->tg[TG_MIRROR])
            canvas_line(a, cv_x(a) + a->vx + cx * a->zoom, a->vy, cv_x(a) + a->vx + cx * a->zoom, a->vy + CH(a) * a->zoom, 1, 0xffcc66);
    }
    if (a->curve_on) {                                                  /* the curve's handles */
        double sx[4], sy[4];
        for (int i = 0; i < 4; i++) {
            sx[i] = cv_x(a) + a->vx + a->cv_pt[i][0] * a->zoom;
            sy[i] = a->vy + a->cv_pt[i][1] * a->zoom;
        }
        canvas_line(a, sx[0], sy[0], sx[2], sy[2], 1, 0x66ccff);
        canvas_line(a, sx[1], sy[1], sx[3], sy[3], 1, 0x66ccff);
        for (int i = 0; i < 4; i++)
            if (in_canvas(a, sx[i], sy[i]))
                wlwin_fill(w, (int)sx[i] - 3, (int)sy[i] - 3, 7, 7, i < 2 ? 0xffffff : 0x66ccff);
    }
    if (a->poly_on)
        for (int i = 0; i < a->poly_n; i++) {
            int x = (int)(cv_x(a) + a->vx + a->poly_x[i] * a->zoom), y = (int)(a->vy + a->poly_y[i] * a->zoom);
            if (in_canvas(a, x, y))
                wlwin_fill(w, x - (i ? 2 : 3), y - (i ? 2 : 3), i ? 5 : 7, i ? 5 : 7, i ? 0x66ccff : 0xffcc66);
        }
    bool brush = a->tool == T_PEN || a->tool == T_ERASER || a->tool == T_SPRAY || a->tool == T_CLONE || a->tool == T_SMUDGE || a->tool == T_BLURBR;
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
    paint_guides_and_rulers(a);
    paint_layers(a);
    paint_tbar(a);
    paint_bbar(a);
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
    char tmpdump[PATH_MAX + 8];
    snprintf(tmpdump, sizeof(tmpdump), "%s.new", g_dump);
    FILE *f = fopen(tmpdump, "w");                                      /* (written whole and renamed: a test reading it never sees half of it) */
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
    {
        double rstep, rminor;
        ruler_steps(a, &rstep, &rminor);
        fprintf(f, "rulers on=%d snap=%d guides=%d unit=%s dpi=%d ppu=%.4f step=%g minor=%g\n", a->rulers ? 1 : 0, a->snap ? 1 : 0, a->nguides, RU_ABBR[a->ruler_unit], a->ruler_dpi, ruler_ppu(a), rstep, rminor);
    }
    for (int i = 0; i < a->nguides; i++)
        fprintf(f, "guide %s pos=%d\n", a->guide[i].vert ? "v" : "h", a->guide[i].pos);
    fprintf(f, "cursor kind=%d hotx=%d hoty=%d\n", a->cur_kind, a->cur_kind >= 0 ? CUR_HOT[a->cur_kind][0] : -1, a->cur_kind >= 0 ? CUR_HOT[a->cur_kind][1] : -1);
    char tfont_dump[64];
    snprintf(tfont_dump, sizeof(tfont_dump), "%s", a->tfont_name);
    for (char *q = tfont_dump; *q; q++)
        if (*q == ' ')
            *q = '_';
    fprintf(f, "text on=%d x=%d y=%d len=%d caret=%d font=%s align=%d bold=%d italic=%d underline=%d strike=%d box=%d line=%d letters=%d outline=%d shadow=%d angle=%d bend=%d\n", a->txt.on ? 1 : 0, a->txt.x, a->txt.y,
            a->txt.len, a->txt.caret, a->tfont_name[0] ? tfont_dump : "-", a->talign, a->tg[TG_BOLD], a->tg[TG_ITALIC], a->tg[TG_UNDER], a->tg[TG_STRIKE], a->tg[TG_BOX], a->val[S_LINE],
            a->val[S_LETTER], a->val[S_OUTLINE], a->val[S_SHADOW], a->val[S_ANGLE], a->val[S_BEND]);
    if (a->tool == T_TEXT) {
        struct tbar_g tg;
        tbar_layout(a, &tg);
        fprintf(f, "tfont x=%d y=%d w=%d h=%d\n", tg.font.x, tg.font.y, tg.font.w, tg.font.h);
        for (int i = 0; i < 6; i++)
            fprintf(f, "ttoggle %s x=%d y=%d w=%d h=%d on=%d\n", TG_NAMES[tg.tg[i].id], tg.tg[i].x, tg.tg[i].y, tg.tg[i].w, tg.tg[i].h, a->tg[tg.tg[i].id] ? 1 : 0);
        static const char *const an[3] = { "Left", "Centre", "Right" };
        for (int i = 0; i < 3; i++)
            fprintf(f, "talign %s x=%d y=%d w=%d h=%d on=%d\n", an[i], tg.al[i].x, tg.al[i].y, tg.al[i].w, tg.al[i].h, a->talign == i ? 1 : 0);
        for (int i = 0; i < tg.nsl; i++) {
            int mn, mx;
            slider_range(a, tg.sl[i].id, &mn, &mx);
            fprintf(f, "tslider %s x=%d y=%d w=%d h=%d min=%d max=%d value=%d tx=%d tw=%d ty=%d\n", SLIDERS[tg.sl[i].id].name, tg.sl[i].x, tg.sl[i].y, tg.sl[i].w, tg.sl[i].h, mn, mx,
                    *slider_ptr(a, tg.sl[i].id), tg.sl[i].x + SL_KNOB / 2, tg.sl[i].w - SL_KNOB, tg.sl[i].y + SL_TRACK_DY);
        }
    }
    if (brush_bar_tool(a->tool)) {
        struct bbar_g bg;
        bbar_layout(a, &bg);
        for (int i = 0; i < bg.nsl; i++) {
            int mn, mx;
            slider_range(a, bg.sl[i].id, &mn, &mx);
            fprintf(f, "bslider %s x=%d y=%d w=%d h=%d min=%d max=%d value=%d tx=%d tw=%d ty=%d\n", SLIDERS[bg.sl[i].id].name, bg.sl[i].x, bg.sl[i].y, bg.sl[i].w, bg.sl[i].h, mn, mx,
                    *slider_ptr(a, bg.sl[i].id), bg.sl[i].x + SL_KNOB / 2, bg.sl[i].w - SL_KNOB, bg.sl[i].y + SL_TRACK_DY);
        }
        for (int i = 0; i < bg.ntg; i++)
            fprintf(f, "btoggle %s x=%d y=%d w=%d h=%d on=%d\n", TG_NAMES[bg.tg[i].id], bg.tg[i].x, bg.tg[i].y, bg.tg[i].w, bg.tg[i].h, a->tg[bg.tg[i].id] ? 1 : 0);
    }
    if (a->prompt == P_FONT) {
        struct wlcard fc;
        int fnb;
        card_rect(a, &fc, &fnb);
        fprintf(f, "fontcard rows=%d families=%d sel=%d top=%d\n", font_rows_fit(a), g_nfam, a->font_sel, a->font_top);
        {
            struct wlcard fcc;
            int fnb;
            card_rect(a, &fcc, &fnb);
            struct wlsbar fb = font_sbar(a, &fcc);
            fprintf(f, "fontbar x=%d y=%d w=%d h=%d\n", fb.x, fb.y, fb.w, fb.h);
        }
        for (int r = 0; r < font_rows_fit(a) && a->font_top + r <= g_nfam; r++)
            fprintf(f, "fontrow %d x=%d y=%d w=%d h=%d name=%s\n", a->font_top + r, fc.x + WLCARD_PAD, font_row_y(&fc, r), fc.w - 2 * WLCARD_PAD, FONT_ROW_H,
                    a->font_top + r == 0 ? "Interface_font" : g_fam[a->font_top + r - 1].name);
    }
    struct wlrail L, R;
    rails(a, &L, &R);
    int ys[WLRAIL_MAX];
    wlrail_layout(&L, ys);
    static const char *const lnames[N_LEFT] = { "Select", "Move", "Pen", "Retouch", "Eraser", "Shapes", "Fill", "Gradient", "Text", "Picker" };
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
        fprintf(f, "toggle %s x=%d y=%d w=%d h=%d on=%d\n", TG_NAMES[s.tg[i].id], s.tg[i].x, s.tg[i].y, s.tg[i].w, s.tg[i].h, s.tg[i].id == TG_RATIO ? a->ratio : a->tg[s.tg[i].id] ? 1 : 0);
    if (layers_on(a)) {
        struct lp_g g;
        lp_geom(a, &g);
        fprintf(f, "layers x=%d y=0 w=%d list_y=%d rows=%d top=%d\n", g.x, g.w, g.ly, g.nvis, a->lay_top);
        for (int i = 0; i < LB_COUNT; i++)
            fprintf(f, "lbtn %s x=%d y=%d w=%d h=%d\n", LB_NAMES[i], g.btn[i][0], g.btn[i][1], g.btn[i][2], g.btn[i][3]);
        fprintf(f, "lopacity x=%d y=%d w=%d h=%d tx=%d tw=%d ty=%d\n", g.ox, g.oy, g.ow, g.oh, g.ox + SL_KNOB / 2, g.ow - SL_KNOB, g.oy + SL_TRACK_DY);
        fprintf(f, "lblend x=%d y=%d w=%d h=%d\n", g.bx, g.by, g.bw, g.bh);
        struct lrow rows[MAX_LAYERS + MAX_GROUPS];
        int nr = lp_rows(a, rows);
        for (int i = 0; i < g.nvis; i++) {
            int ri = a->lay_top + i;
            if (ri >= nr)
                break;
            int y = g.ly + i * LP_ROW_H;
            if (rows[ri].layer >= 0)
                fprintf(f, "lrow %d x=%d y=%d w=%d h=%d eye=%d,%d depth=%d\n", rows[ri].layer, g.x, y, g.w, LP_ROW_H, g.x + 18, y + LP_ROW_H / 2, rows[ri].depth);
            else
                fprintf(f, "grow %d x=%d y=%d w=%d h=%d eye=%d,%d fold=%d,%d depth=%d\n", rows[ri].group, g.x, y, g.w, LP_ROW_H, g.x + 18, y + LP_ROW_H / 2, g.x + 32 + rows[ri].depth * LP_INDENT + 8,
                        y + LP_ROW_H / 2, rows[ri].depth);
        }
    }
    for (int i = 0; i < a->doc.n; i++) {
        struct pix lp = a->doc.L[i].im;
        fprintf(f, "layer %d name=%s opacity=%d visible=%d blend=%s text=%d mask=%d group=%d hash=%016llx\n", i, a->doc.L[i].name, a->doc.L[i].opacity, a->doc.L[i].visible ? 1 : 0, BM_NAMES[a->doc.L[i].blend], a->doc.L[i].tx.on ? 1 : 0, !a->doc.L[i].mask.p ? 0 : a->mask_edit == i ? 2 : 1, a->doc.L[i].gid,
                (unsigned long long)pix_hash(&lp));
    }
    for (int i = 0; i < MAX_GROUPS; i++)
        if (a->doc.G[i].used) {
            int glo = 0, ghi = 0;
            group_range(&a->doc, i + 1, &glo, &ghi);
            fprintf(f, "group %d name=%s opacity=%d blend=%s visible=%d collapsed=%d parent=%d first=%d last=%d chosen=%d\n", i + 1, a->doc.G[i].name, a->doc.G[i].opacity, bm_name(a->doc.G[i].blend),
                    a->doc.G[i].visible ? 1 : 0, a->doc.G[i].collapsed ? 1 : 0, a->doc.G[i].parent, glo, ghi, a->gsel == i + 1 ? 1 : 0);
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
        fprintf(f, "fxcard kind=%s\n", FXK[a->fxk].id);
        for (int i = 0; i < FXK[a->fxk].n; i++) {
            int tx, ty, tw;
            adj_track(&c, i, &tx, &ty, &tw);
            fprintf(f, "adjust %s tx=%d ty=%d tw=%d min=%d max=%d value=%d\n", FXK[a->fxk].p[i].name, tx, ty, tw, FXK[a->fxk].p[i].mn, FXK[a->fxk].p[i].mx, *adj_ptr(a, i));
        }
    }
    if (a->prompt == P_EFFECTS) {
        struct wlcard ec;
        int enb;
        card_rect(a, &ec, &enb);
        for (int i = 0; i < N_EFX; i++) {
            int x, y, bw, bh;
            efx_rect(&ec, i, &x, &y, &bw, &bh);
            char nm[40];
            snprintf(nm, sizeof(nm), "%s", EFX[i].label);
            for (char *q = nm; *q; q++)
                if (*q == ' ')
                    *q = '_';
            fprintf(f, "efx %s x=%d y=%d w=%d h=%d\n", nm, x, y, bw, bh);
        }
    }
    if (a->prompt != P_NONE) {
        struct wlcard c;
        int nb;
        card_rect(a, &c, &nb);
        fprintf(f, "card x=%d y=%d w=%d h=%d field=%s\n", c.x, c.y, c.w, c.h, a->pbuf);
        if (a->prompt == P_HELP) {
            struct hgeo g;
            help_geom(&c, &g);
            help_layout(a->help_page, g.cw);
            char ti[64];
            snprintf(ti, sizeof(ti), "%s", HP[a->help_page].title);
            for (char *q = ti; *q; q++)
                if (*q == ' ')
                    *q = '_';
            fprintf(f, "help page=%d pages=%d title=%s top=%d height=%d view=%d lines=%d\n", a->help_page, HP_COUNT, ti, a->help_top, g_hl_h, g.ch, g_hl_n);
            for (int i = 0; i < HP_COUNT; i++)
                fprintf(f, "hpage %d x=%d y=%d w=%d h=%d\n", i, g.sx, g.sy + i * g.rowh, g.sw, g.rowh);
            fprintf(f, "hbtn back x=%d y=%d w=%d h=%d\nhbtn next x=%d y=%d w=%d h=%d\n", g.prev_x, g.py, g.pw, g.ph, g.next_x, g.py, g.pw, g.ph);
            fprintf(f, "hbar x=%d y=%d w=%d h=%d\n", g.bx, g.by, g.bw, g.bh);
        }
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
    rename(tmpdump, g_dump);
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
static void clone_commit(struct app *a, struct irect d);

static bool pixel_tool(const struct app *a) { return a->tg[TG_PIXEL] && (a->tool == T_PEN || a->tool == T_ERASER || a->tool == T_LINE); }

/* How many copies of a stroke the symmetry makes, and where copy i of a point lies: turned about the middle of the picture by a share of a full turn,
 * the mirrored ones first reflected left to right. */
static int sym_count(const struct app *a)
{
    return brush_bar_tool(a->tool) ? a->val[S_SYM] * (a->tg[TG_MIRROR] ? 2 : 1) : 1;
}

static void sym_pt(const struct app *a, int i, double x, double y, double *ox, double *oy)
{
    int n = LM_MAX(a->val[S_SYM], 1), k = i % n;
    bool m = a->tg[TG_MIRROR] && i >= n;
    double cx = CW(a) / 2.0, cy = CH(a) / 2.0, px = x - cx, py = y - cy;
    if (m)
        px = -px;
    double ang = 6.2831853 * k / n, c = cos(ang), s = sin(ang);
    *ox = cx + px * c - py * s;
    *oy = cy + px * s + py * c;
}

static void stroke_commit(struct app *a, struct irect d, uint32_t col, bool erase)
{
    irect_clip(&d, CW(a), CH(a));
    if (irect_empty(&d))
        return;
    if (pixel_tool(a))                                                  /* pixel art: every pixel is all or nothing */
        for (int y = d.y0; y < d.y1; y++)
            for (int x = d.x0; x < d.x1; x++) {
                uint8_t *q = &a->cov.p[(size_t)y * (size_t)CW(a) + (size_t)x];
                *q = *q >= 128 ? 255 : 0;
            }
    if (a->tool == T_CLONE) {
        clone_commit(a, d);
        return;
    }
    cov_commit(&IM(a), &a->before, &a->cov, d, col, a->val[S_OPACITY], erase, &a->sel);
    changed_r(a, d);
}

/* A piece of a freehand stroke, with its symmetric copies; pixel art snaps it to pixel centres. */
static void brush_seg(struct app *a, double x0, double y0, double x1, double y1, struct irect *d)
{
    double r = a->val[S_SIZE] / 2.0, hard = a->val[S_HARD] / 100.0;
    if (pixel_tool(a)) {
        x0 = floor(x0) + 0.5;
        y0 = floor(y0) + 0.5;
        x1 = floor(x1) + 0.5;
        y1 = floor(y1) + 0.5;
    }
    for (int i = 0; i < sym_count(a); i++) {
        double ax, ay, bx, by;
        sym_pt(a, i, x0, y0, &ax, &ay);
        sym_pt(a, i, x1, y1, &bx, &by);
        cov_stroke(&a->cov, ax, ay, bx, by, r, hard, d);
    }
}

static void spray_at(struct app *a, double ix, double iy)
{
    if (!a->have_before)
        return;
    double r = a->val[S_SIZE] / 2.0;
    int n = LM_MAX((int)(a->val[S_DENSITY] / 100.0 * r * 3.0), 1);
    struct irect d = { 0, 0, 0, 0 };
    for (int s = 0; s < sym_count(a); s++) {
        double cx, cy;
        sym_pt(a, s, ix, iy, &cx, &cy);
        for (int i = 0; i < n; i++) {
            double ang = (rnd32(a) & 0xffff) / 65536.0 * 6.2831853, rad = sqrt((rnd32(a) & 0xffff) / 65536.0) * r;
            int x = (int)floor(cx + rad * cos(ang)), y = (int)floor(cy + rad * sin(ang));
            if (x < 0 || y < 0 || x >= CW(a) || y >= CH(a))
                continue;
            uint8_t *q = &a->cov.p[(size_t)y * (size_t)CW(a) + (size_t)x];
            *q = (uint8_t)LM_MIN(*q + 110, 255);
            irect_add(&d, x, y, x + 1, y + 1);
        }
    }
    if (!irect_empty(&d))
        stroke_commit(a, d, a->drag_button == 273 ? a->bg : a->fg, false);
}

/* ---- the free polygon and the curve: shapes of several clicks, and the retouching brushes --------------------------------------------------- */

/* Gives a half made shape up: the layer is as it was, and the undo step it took is gone. */
static void shape_cancel(struct app *a)
{
    if (a->have_before && a->before.w == IM(a).w && a->before.h == IM(a).h) {
        memcpy(IM(a).p, a->before.p, (size_t)IM(a).w * (size_t)IM(a).h * 4);
        flat_mark_all(a);
    }
    stroke_end(a);
    if (a->poly_on || a->curve_on) {
        if (a->nundo > 0)
            snap_free(&a->undo[--a->nundo]);
    }
    a->poly_on = a->curve_on = false;
    a->poly_n = 0;
    a->curve_phase = 0;
    a->win.redraw = true;
}

/* The free polygon as far as it is laid out, with a rubber band to the pointer (`closed` joins the last corner to the first). */
static void poly_draw(struct app *a, double hx, double hy, bool rubber, bool closed)
{
    if (!a->have_before || !a->cov.p)
        return;
    struct irect old = a->preview, d = { 0, 0, 0, 0 };
    if (!irect_empty(&old))
        cov_clear(&a->cov, old);
    double xs[260], ys[260];
    int n = a->poly_n;
    for (int i = 0; i < n; i++) {
        xs[i] = a->poly_x[i];
        ys[i] = a->poly_y[i];
    }
    if (rubber) {
        xs[n] = hx;
        ys[n] = hy;
        n++;
    }
    double r = a->val[S_SIZE] / 2.0;
    if (a->tg[TG_FILLED] && n >= 3)
        cov_fill_poly(&a->cov, xs, ys, n, &d);
    for (int i = 0; i + 1 < n; i++)
        cov_stroke(&a->cov, xs[i], ys[i], xs[i + 1], ys[i + 1], r, 1.0, &d);
    if (closed && n >= 3)
        cov_stroke(&a->cov, xs[n - 1], ys[n - 1], xs[0], ys[0], r, 1.0, &d);
    irect_clip(&d, CW(a), CH(a));
    struct irect all = old;
    if (!irect_empty(&d))
        irect_add(&all, d.x0, d.y0, d.x1, d.y1);
    a->preview = d;
    stroke_commit(a, all, a->fg, false);
}

static void poly_commit(struct app *a)
{
    if (!a->poly_on)
        return;
    if (a->poly_n < 2) {
        shape_cancel(a);
        return;
    }
    poly_draw(a, 0, 0, false, true);
    push_recent(a, a->fg);
    stroke_end(a);
    a->poly_on = false;
    a->poly_n = 0;
    a->win.redraw = true;
}

/* The curve flattened into short pieces. */
static void curve_draw(struct app *a)
{
    if (!a->have_before || !a->cov.p)
        return;
    struct irect old = a->preview, d = { 0, 0, 0, 0 };
    if (!irect_empty(&old))
        cov_clear(&a->cov, old);
    const double (*p)[2] = a->cv_pt;
    double len = hypot(p[1][0] - p[0][0], p[1][1] - p[0][1]) + hypot(p[2][0] - p[0][0], p[2][1] - p[0][1]) + hypot(p[3][0] - p[1][0], p[3][1] - p[1][1]);
    int n = LM_CLAMP((int)(len / 2.0), 8, 400);
    double r = a->val[S_SIZE] / 2.0, px = p[0][0], py = p[0][1];
    for (int i = 1; i <= n; i++) {
        double t = (double)i / n, u = 1 - t;
        double x = u * u * u * p[0][0] + 3 * u * u * t * p[2][0] + 3 * u * t * t * p[3][0] + t * t * t * p[1][0];
        double y = u * u * u * p[0][1] + 3 * u * u * t * p[2][1] + 3 * u * t * t * p[3][1] + t * t * t * p[1][1];
        cov_stroke(&a->cov, px, py, x, y, r, 1.0, &d);
        px = x;
        py = y;
    }
    irect_clip(&d, CW(a), CH(a));
    struct irect all = old;
    if (!irect_empty(&d))
        irect_add(&all, d.x0, d.y0, d.x1, d.y1);
    a->preview = d;
    stroke_commit(a, all, a->fg, false);
}

static void curve_commit(struct app *a)
{
    if (!a->curve_on)
        return;
    if (a->curve_phase == 0) {
        shape_cancel(a);
        return;
    }
    curve_draw(a);
    push_recent(a, a->fg);
    stroke_end(a);
    a->curve_on = false;
    a->curve_phase = 0;
    a->win.redraw = true;
}

/* Something half made is put down (or given up) before anything else happens. */
static void shapes_finish(struct app *a)
{
    if (a->poly_on)
        poly_commit(a);
    if (a->curve_on)
        curve_commit(a);
}

/* The clone stamp: what the source spot shows is laid on where the brush went. */
static void clone_commit(struct app *a, struct irect d)
{
    const struct pix *src = &a->before;
    if (a->tg[TG_IMAGE]) {
        flat_refresh(a);
        if (a->flat.p)
            src = &a->flat;
    }
    int ox = (int)lround(a->clone_ox), oy = (int)lround(a->clone_oy), w = CW(a);
    for (int y = d.y0; y < d.y1; y++)
        for (int x = d.x0; x < d.x1; x++) {
            size_t i = (size_t)y * (size_t)w + (size_t)x;
            int cv = a->cov.p[i];
            if (cv && a->sel.on)
                cv = cv * sel_at(&a->sel, x, y) / 255;
            int k = cv * a->val[S_OPACITY] / 100, sx = x + ox, sy = y + oy;
            if (k <= 0 || sx < 0 || sy < 0 || sx >= src->w || sy >= src->h) {
                IM(a).p[i] = a->before.p[i];
                continue;
            }
            IM(a).p[i] = blend(a->before.p[i], src->p[(size_t)sy * (size_t)src->w + (size_t)sx], k);
        }
    changed_r(a, d);
}

static uint32_t mix_px(uint32_t a, uint32_t b, double t)
{
    double aa = ch_a(a), ab = ch_a(b), A = aa + (ab - aa) * t;
    if (A < 0.5)
        return 0;
    double R = (ch_r(a) * aa * (1 - t) + ch_r(b) * ab * t) / A, G = (ch_g(a) * aa * (1 - t) + ch_g(b) * ab * t) / A, B = (ch_b(a) * aa * (1 - t) + ch_b(b) * ab * t) / A;
    return argb((int)(A + 0.5), clamp255((int)(R + 0.5)), clamp255((int)(G + 0.5)), clamp255((int)(B + 0.5)));
}

/* The smudge starts with the colours under the brush: a square of the layer round the first point. */
static void smudge_begin(struct app *a, double cx, double cy)
{
    int R = (int)ceil(a->val[S_SIZE] / 2.0) + 1, fd = 2 * R + 1;
    pix_free(&a->finger);
    if (!pix_alloc(&a->finger, fd, fd, 0))
        return;
    int fox = (int)floor(cx) - R, foy = (int)floor(cy) - R;
    for (int y = 0; y < fd; y++)
        for (int x = 0; x < fd; x++) {
            int lx = fox + x, ly = foy + y;
            if (lx >= 0 && ly >= 0 && lx < CW(a) && ly < CH(a))
                a->finger.p[(size_t)y * (size_t)fd + (size_t)x] = IM(a).p[(size_t)ly * (size_t)CW(a) + (size_t)lx];
        }
}

/* One touch of the smudge or the blur brush at a point. */
static void retouch_dab(struct app *a, double cx, double cy)
{
    double r = a->val[S_SIZE] / 2.0, str = a->val[S_OPACITY] / 100.0, hard = a->val[S_HARD] / 100.0;
    int w = CW(a), h = CH(a);
    struct irect box = { (int)floor(cx - r) - 1, (int)floor(cy - r) - 1, (int)ceil(cx + r) + 2, (int)ceil(cy + r) + 2 };
    irect_clip(&box, w, h);
    if (irect_empty(&box))
        return;
    bool smudge = a->tool == T_SMUDGE;
    struct pix win = { 0 };
    int wx0 = LM_MAX(box.x0 - 1, 0), wy0 = LM_MAX(box.y0 - 1, 0), wx1 = LM_MIN(box.x1 + 1, w), wy1 = LM_MIN(box.y1 + 1, h);
    if (!smudge) {                                                      /* the blur reads the layer as it was before this touch */
        if (!pix_alloc(&win, wx1 - wx0, wy1 - wy0, 0))
            return;
        for (int y = wy0; y < wy1; y++)
            memcpy(&win.p[(size_t)(y - wy0) * (size_t)win.w], &IM(a).p[(size_t)y * (size_t)w + (size_t)wx0], (size_t)win.w * 4);
    }
    int R = (int)ceil(a->val[S_SIZE] / 2.0) + 1, fd = a->finger.w, fox = (int)floor(cx) - R, foy = (int)floor(cy) - R;
    for (int y = box.y0; y < box.y1; y++)
        for (int x = box.x0; x < box.x1; x++) {
            double d = hypot(x + 0.5 - cx, y + 0.5 - cy);
            if (d > r + 0.5)
                continue;
            double wgt = hard >= 0.999 ? LM_CLAMP(r + 0.5 - d, 0.0, 1.0) : (d <= r * hard ? 1.0 : smooth01((r + 0.5 - d) / (r + 0.5 - r * hard)));
            wgt *= sel_at(&a->sel, x, y) / 255.0;
            if (wgt <= 0)
                continue;
            uint32_t *q = &IM(a).p[(size_t)y * (size_t)w + (size_t)x];
            if (smudge) {
                int fx = x - fox, fy = y - foy;
                if (!a->finger.p || fx < 0 || fy < 0 || fx >= fd || fy >= fd)
                    continue;
                uint32_t *f = &a->finger.p[(size_t)fy * (size_t)fd + (size_t)fx], old = *q;
                *q = mix_px(old, *f, str * wgt);
                *f = mix_px(*f, old, 0.35);
            } else {
                double A = 0, Rr = 0, G = 0, B = 0;
                int n = 0;
                for (int j = -1; j <= 1; j++)
                    for (int i = -1; i <= 1; i++) {
                        int px = LM_CLAMP(x + i, 0, w - 1), py = LM_CLAMP(y + j, 0, h - 1);
                        uint32_t c = win.p[(size_t)(py - wy0) * (size_t)win.w + (size_t)(px - wx0)];
                        double al = ch_a(c);
                        A += al;
                        Rr += ch_r(c) * al;
                        G += ch_g(c) * al;
                        B += ch_b(c) * al;
                        n++;
                    }
                uint32_t avg = A > 0.5 ? argb((int)(A / n + 0.5), (int)(Rr / A + 0.5), (int)(G / A + 0.5), (int)(B / A + 0.5)) : 0;
                *q = mix_px(*q, avg, str * wgt);
            }
        }
    pix_free(&win);
    changed_r(a, box);
}

/* The brush along a piece of a stroke, a touch every few pixels. */
static void retouch_seg(struct app *a, double x0, double y0, double x1, double y1)
{
    double r = a->val[S_SIZE] / 2.0, step = LM_MAX(r / 3.0, 1.0), len = hypot(x1 - x0, y1 - y0);
    int n = LM_MAX((int)ceil(len / step), 1);
    for (int i = 1; i <= n; i++)
        retouch_dab(a, x0 + (x1 - x0) * i / n, y0 + (y1 - y0) * i / n);
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
        for (int i = 0; i < sym_count(a); i++) {
            double ax, ay, bx, by;
            sym_pt(a, i, x0 + 0.5, y0 + 0.5, &ax, &ay);
            sym_pt(a, i, x1 + 0.5, y1 + 0.5, &bx, &by);
            if (a->tg[TG_ARROW])
                cov_arrow(&a->cov, ax, ay, bx, by, r, &d);
            else
                cov_stroke(&a->cov, ax, ay, bx, by, r, 1.0, &d);
        }
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
    sel_combine(a, &a->cov, box);
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
    sel_combine(a, &a->cov, old);
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
    if (tool_snaps(a->tool))
        snap_pic(a, &ix, &iy);
    a->gsel = 0;                                                        /* drawing is on a layer: the panel goes back to the chosen layer */
    a->drag = true;
    a->drag_button = button;
    a->sx = a->lx = ix;
    a->sy = a->ly = iy;
    if (!a->poly_on && !a->curve_on) {                                  /* (a shape of several clicks keeps its picture between them) */
        a->have_before = false;
        a->preview = (struct irect){ 0, 0, 0, 0 };
    }
    uint32_t col = draw_color(a);
    double r = a->val[S_SIZE] / 2.0;
    int px = (int)floor(ix), py = (int)floor(iy);
    switch (a->tool) {
    case T_SELECT:
    case T_ELLIPSE_SEL:
    case T_LASSO:
        sel_clear(&a->sel_base);
        a->sel_mode = a->sel.on ? sel_mode_now(a) : 0;
        if (a->sel_mode)
            sel_dup(&a->sel_base, &a->sel);                             /* (the selection stays as it was until the shape grows over it) */
        else
            sel_clear(&a->sel);
        a->preview = (struct irect){ 0, 0, 0, 0 };
        a->vdirty = true;
        if (a->tool == T_SELECT) {
            if (a->sel_mode && !cov_alloc(&a->cov, CW(a), CH(a)))
                a->drag = false;
            break;
        }
        if (a->tool == T_ELLIPSE_SEL) {
            if (!cov_alloc(&a->cov, CW(a), CH(a)))
                a->drag = false;
            break;
        }
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
            if (cov_flood(src, px, py, a->val[S_TOL] * 255 / 100, a->tg[TG_GLOBAL], &c, &d)) {
                sel_clear(&a->sel_base);
                a->sel_mode = a->sel.on ? sel_mode_now(a) : 0;
                if (a->sel_mode)
                    sel_dup(&a->sel_base, &a->sel);
                sel_combine(a, &c, d);
                sel_clear(&a->sel_base);
                a->sel_mode = 0;
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
            a->smx = ix;
            a->smy = iy;
            (void)r;
            brush_seg(a, ix, iy, ix, iy, &d);
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
    case T_FREEPOLY: {
        a->drag = false;
        if (button != 272)
            break;
        int64_t now = now_ms();
        bool dbl = now - a->last_click_ms < lm_metrics.double_click_ms && hypot(ix - a->last_click_x, iy - a->last_click_y) * a->zoom < 6.0;
        a->last_click_ms = now;
        a->last_click_x = ix;
        a->last_click_y = iy;
        if (!a->poly_on) {
            if (!stroke_begin(a))
                break;
            a->poly_on = true;
            a->poly_n = 0;
            a->drag_button = 272;
        }
        if (a->poly_n >= 2 && (dbl || (a->poly_n >= 3 && hypot((ix - a->poly_x[0]) * a->zoom, (iy - a->poly_y[0]) * a->zoom) < 7.0))) {
            poly_commit(a);                                             /* a double click, or a click on the first corner, closes it */
            break;
        }
        if (a->poly_n < 256) {
            a->poly_x[a->poly_n] = ix;
            a->poly_y[a->poly_n] = iy;
            a->poly_n++;
        }
        poly_draw(a, ix, iy, true, false);
        break;
    }
    case T_CURVE:
        if (button != 272) {
            a->drag = false;
            break;
        }
        if (!a->curve_on) {
            if (!stroke_begin(a)) {
                a->drag = false;
                break;
            }
            a->curve_on = true;
            a->curve_phase = 0;
            a->drag_button = 272;
            for (int i = 0; i < 4; i++) {
                a->cv_pt[i][0] = ix;
                a->cv_pt[i][1] = iy;
            }
        } else if (a->curve_phase == 1) {
            a->cv_pt[2][0] = ix;
            a->cv_pt[2][1] = iy;
        } else {
            a->cv_pt[3][0] = ix;
            a->cv_pt[3][1] = iy;
        }
        curve_draw(a);
        break;
    case T_CLONE:
        if ((a->win.mods & LM_MOD_ALT) || button == 273) {              /* Alt+click or the right button: the spot to copy from */
            a->clone_have = true;
            a->clone_off_set = false;
            a->clone_sx = ix;
            a->clone_sy = iy;
            a->drag = false;
            say(a, "Copying from %d, %d: now paint", px, py);
            break;
        }
        if (!a->clone_have) {
            a->drag = false;
            say(a, "Alt+click (or right click) on the spot to copy from first");
            break;
        }
        if (!stroke_begin(a)) {
            a->drag = false;
            break;
        }
        if (!a->clone_off_set) {
            a->clone_ox = a->clone_sx - ix;
            a->clone_oy = a->clone_sy - iy;
            a->clone_off_set = true;
        }
        {
            struct irect d = { 0, 0, 0, 0 };
            cov_stroke(&a->cov, ix, iy, ix, iy, r, a->val[S_HARD] / 100.0, &d);
            stroke_commit(a, d, col, false);
        }
        break;
    case T_SMUDGE:
    case T_BLURBR:
        checkpoint(a);
        if (a->tool == T_SMUDGE)
            smudge_begin(a, ix, iy);
        retouch_dab(a, ix, iy);
        break;
    case T_TEXT: {
        a->drag = false;
        if (a->txt.on) {
            struct tmetrics m;
            double lx, ly;
            text_build(a, NULL, &m);
            tm_from_pic(&m, ix, iy, &lx, &ly);
            if (!m.bs && lx >= -2 && lx < m.w + 2 && ly >= -2 && ly < m.h + 2)
                break;                                                  /* a click on the text itself: it stays open */
            if (m.bs) {                                                 /* bent: anywhere on its picture */
                struct pix tp = { 0 };
                bool inside = text_build(a, &tp, &m) && ix >= m.ox && iy >= m.oy && ix < m.ox + tp.w && iy < m.oy + tp.h;
                pix_free(&tp);
                if (inside)
                    break;
            }
            text_commit(a);
        }
        int tl = text_layer_at(a, ix, iy);
        if (tl >= 0) {                                                  /* a click on a text layer's text: type into it again */
            text_reedit(a, tl);
            break;
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
            double k = 1.0 - a->val[S_SMOOTH] / 100.0 * 0.93;           /* how far the stroke follows the pointer each time: a lazy brush draws smoother lines */
            a->smx += (ix - a->smx) * k;
            a->smy += (iy - a->smy) * k;
            brush_seg(a, a->lx, a->ly, a->smx, a->smy, &d);
            stroke_commit(a, d, draw_color(a), a->tool == T_ERASER);
            a->lx = a->smx;
            a->ly = a->smy;
        }
        break;
    case T_SPRAY:
        a->lx = ix;
        a->ly = iy;
        break;
    case T_CLONE:
        if (a->have_before) {
            struct irect d = { 0, 0, 0, 0 };
            cov_stroke(&a->cov, a->lx, a->ly, ix, iy, a->val[S_SIZE] / 2.0, a->val[S_HARD] / 100.0, &d);
            stroke_commit(a, d, a->fg, false);
        }
        a->lx = ix;
        a->ly = iy;
        break;
    case T_SMUDGE:
    case T_BLURBR:
        retouch_seg(a, a->lx, a->ly, ix, iy);
        a->lx = ix;
        a->ly = iy;
        break;
    case T_CURVE: {
        double (*p)[2] = a->cv_pt;
        if (a->curve_phase == 0) {
            p[1][0] = ix;
            p[1][1] = iy;
            for (int k = 0; k < 2; k++) {                                 /* until the handles are pulled they lie a third and two thirds of the way */
                p[2][k] = p[0][k] + (p[1][k] - p[0][k]) / 3.0;
                p[3][k] = p[0][k] + (p[1][k] - p[0][k]) * 2.0 / 3.0;
            }
        } else if (a->curve_phase == 1) {
            p[2][0] = ix;
            p[2][1] = iy;
        } else {
            p[3][0] = ix;
            p[3][1] = iy;
        }
        curve_draw(a);
        break;
    }
    case T_LINE: case T_BOX: case T_CIRCLE: case T_POLY: drag_shape(a, ix, iy, shift); break;
    case T_GRADIENT:
        a->lx = ix;
        a->ly = iy;
        drag_gradient(a, ix, iy);
        break;
    case T_SELECT: {
        double ex = ix, ey = iy;
        if (a->ratio) {                                                 /* a fixed shape: the height follows the width */
            double ady = fabs(ix - a->sx) * RATIO_H[a->ratio] / RATIO_W[a->ratio];
            ey = a->sy + (iy < a->sy ? -ady : ady);
        }
        struct irect r = { (int)floor(LM_MIN(a->sx, ex)), (int)floor(LM_MIN(a->sy, ey)), (int)floor(LM_MAX(a->sx, ex)) + 1, (int)floor(LM_MAX(a->sy, ey)) + 1 };
        irect_clip(&r, CW(a), CH(a));
        if (a->sel_mode == 0) {
            a->sel.on = true;
            a->sel.r = r;
        } else if (a->cov.p) {
            struct irect old = a->preview;
            if (!irect_empty(&old))
                cov_clear(&a->cov, old);
            for (int y = r.y0; y < r.y1; y++)
                memset(&a->cov.p[(size_t)y * (size_t)CW(a) + (size_t)r.x0], 255, (size_t)(r.x1 - r.x0));
            a->preview = r;
            irect_add(&old, r.x0, r.y0, r.x1, r.y1);
            sel_combine(a, &a->cov, old);
        }
        a->vdirty = true;
        break;
    }
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
    if (a->tool == T_CURVE) {                                           /* the ends, then the first handle, then the second one: each is let go */
        if (a->curve_phase == 0 && hypot(a->cv_pt[1][0] - a->cv_pt[0][0], a->cv_pt[1][1] - a->cv_pt[0][1]) < 2.0)
            shape_cancel(a);
        else if (a->curve_phase < 2)
            a->curve_phase++;
        else
            curve_commit(a);
        a->win.redraw = true;
        return;
    }
    if (a->tool == T_FREEPOLY) {
        a->win.redraw = true;
        return;
    }
    if (a->tool == T_SMUDGE || a->tool == T_BLURBR) {
        pix_free(&a->finger);
        a->win.redraw = true;
        return;
    }
    if (a->tool == T_CLONE) {
        stroke_end(a);
        a->win.redraw = true;
        return;
    }
    if ((a->tool == T_PEN || a->tool == T_ERASER) && a->have_before && a->val[S_SMOOTH] > 0) {       /* a lazy stroke catches up with the pointer at the end */
        double ix, iy;
        struct irect d = { 0, 0, 0, 0 };
        to_pic(a, a->px, a->py, &ix, &iy);
        brush_seg(a, a->lx, a->ly, ix, iy, &d);
        stroke_commit(a, d, draw_color(a), a->tool == T_ERASER);
    }
    if (a->tool == T_PEN || a->tool == T_ERASER || tool_in_shapes(a->tool) || a->tool == T_SPRAY || a->tool == T_GRADIENT) {
        if (a->tool != T_ERASER)
            push_recent(a, draw_color(a));
        stroke_end(a);
    } else if (a->tool == T_MOVE)
        move_end(a);
    else if (a->tool == T_LASSO || a->tool == T_ELLIPSE_SEL) {
        sel_clear(&a->sel_base);
        a->sel_mode = 0;
        cov_free(&a->cov);
        a->preview = (struct irect){ 0, 0, 0, 0 };
        sel_clip(a);
        if (a->sel.on)
            say(a, "Selected %d x %d at %d, %d", a->sel.r.x1 - a->sel.r.x0, a->sel.r.y1 - a->sel.r.y0, a->sel.r.x0, a->sel.r.y0);
    } else if (a->tool == T_SELECT) {
        cov_free(&a->cov);
        sel_clear(&a->sel_base);
        a->sel_mode = 0;
        a->preview = (struct irect){ 0, 0, 0, 0 };
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
    case B_PEN:
    case B_RETOUCH:
    case B_MOVE:
    case B_SHAPES: {
        int g = group_of_button(id);
        if (group_of_tool(a->tool) == g) {                              /* the lit one again: the menu of the others */
            if (!wlwin_menu(&a->win, GRP_MENU[g]))
                say(a, "The compositor has no menu service");
            else
                a->menu_kind = MENU_GROUP0 + g;
        } else
            tool_set(a, a->grp[g]);
        break;
    }
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
        if (wlwin_menu(&a->win, "Turn left\nTurn right\nFlip left-right\nFlip up-down\nResize...\nCanvas size...\nFlip layer left-right\nFlip layer up-down\nPlace picture as layer...\nPaper colour = the second colour\nTurn by any angle..."))
            a->menu_kind = 2;
        else
            say(a, "The compositor has no menu service");
        break;
    case B_CROP: do_crop(a); break;
    case B_FX:
        a->prompt = P_EFFECTS;
        a->perr[0] = 0;
        a->pcard_hover = a->pcard_down = -1;
        break;
    case B_FIT: view_fit(a); break;
    case B_HELP: help_open(a); break;
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
    case 10: fx_open(a, K_ROTIMG); break;
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
    {                                                                   /* masks */
        struct pix mim;
        struct doc md, mc;
        CHECK(pix_alloc(&mim, 4, 4, 0xffff0000u), "alloc for a mask");
        doc_from_pix(&md, &mim);
        md.paper = 0xff000000u;
        CHECK(pix_alloc(&md.L[0].mask, 4, 4, 0xffffffffu), "a mask");
        md.L[0].mask.p[5] = 0xff000000u;                                /* black: hides */
        md.L[0].mask.p[6] = 0xff808080u;                                /* grey: half */
        md.L[0].mask.p[7] = 0x00000000u;                                /* see-through counts as white */
        CHECK(doc_flatten(&md, &fl) && fl.p[0] == 0xffff0000u && fl.p[5] == 0xff000000u && ch_r(fl.p[6]) > 120 && ch_r(fl.p[6]) < 135 && fl.p[7] == 0xffff0000u, "a mask hides, half hides, and see-through shows (%08x %08x %08x)", fl.p[5], fl.p[6], fl.p[7]);
        pix_free(&fl);
        CHECK(doc_copy(&md, &mc) && mc.L[0].mask.p && mc.L[0].mask.p != md.L[0].mask.p && mc.L[0].mask.p[5] == 0xff000000u, "a copy of the document has its own mask");
        doc_free(&mc);
        CHECK(doc_add(&md, "Up", 0) && md.n == 2, "a second layer");
        md.L[1].im.p[5] = 0xff00ff00u;
        CHECK(doc_dup(&md) && md.n == 3 && !md.L[2].mask.p, "a duplicate of a layer with no mask has none");
        md.cur = 0;
        CHECK(doc_dup(&md) && md.L[1].mask.p && md.L[1].mask.p != md.L[0].mask.p, "a duplicate of a masked layer has a mask of its own");
        CHECK(doc_del(&md) && md.n == 3, "and goes with its mask");
        layer_bake_mask(&md.L[0]);
        CHECK(!md.L[0].mask.p && ch_a(md.L[0].im.p[5]) == 0 && md.L[0].im.p[0] == 0xffff0000u && ch_a(md.L[0].im.p[6]) > 120 && ch_a(md.L[0].im.p[6]) < 135, "baking the mask makes the layer see-through there (%08x)", md.L[0].im.p[6]);
        doc_free(&md);
    }
    {                                                                   /* groups */
        struct pix gim;
        struct doc gd;
        CHECK(pix_alloc(&gim, 4, 4, 0xffff0000u), "alloc for groups");
        doc_from_pix(&gd, &gim);                                        /* 0: red */
        gd.paper = 0xff000000u;
        CHECK(doc_add(&gd, "A", 0) && gd.n == 2, "a second layer");
        for (size_t i = 0; i < 16; i++)
            gd.L[1].im.p[i] = 0xff00ff00u;                              /* 1: green */
        CHECK(doc_group_layer(&gd) && gd.L[1].gid == 1 && groups_used(&gd) == 1, "a group round a layer");
        CHECK(doc_add(&gd, "In", 0) && gd.n == 3 && gd.L[2].gid == 1 && gd.cur == 2, "a layer made next to one in a group is in the group");
        for (size_t i = 0; i < 16; i++)
            gd.L[2].im.p[i] = 0x80ffffffu;                              /* 2: half white, in the group */
        CHECK(doc_add(&gd, "Out", 0) && gd.n == 4 && gd.L[3].gid == 1, "(a layer above the group's top layer is in it too)");
        gd.L[3].gid = 0;                                                /* 3: outside, see-through */
        int lo, hi;
        CHECK(group_range(&gd, 1, &lo, &hi) && lo == 1 && hi == 2, "the group's layers (%d..%d)", lo, hi);
        struct pix gf;
        CHECK(doc_flatten(&gd, &gf), "flatten with a group");
        uint32_t plain = gf.p[5];                                       /* pass through at 100 %: as if the group were not there */
        pix_free(&gf);
        CHECK(ch_g(plain) > 200 && ch_r(plain) > 100 && ch_r(plain) < 160 && ch_b(plain) > 100, "half white over green (%08x)", plain);
        gd.G[0].visible = false;
        CHECK(doc_flatten(&gd, &gf) && gf.p[5] == 0xffff0000u, "a hidden group shows nothing (%08x)", gf.p[5]);
        pix_free(&gf);
        gd.G[0].visible = true;
        gd.G[0].blend = BM_NORMAL;
        gd.G[0].opacity = 50;
        CHECK(doc_flatten(&gd, &gf) && ch_r(gf.p[5]) > 120 && ch_r(gf.p[5]) < 200 && ch_g(gf.p[5]) > 100, "a group at 50%% is laid over as one picture (%08x)", gf.p[5]);
        pix_free(&gf);
        gd.G[0].opacity = 100;                                          /* pass through lets a multiply layer inside see what is below the group, an isolated group does not */
        gd.G[0].blend = BM_PASS;
        gd.L[2].blend = BM_MULTIPLY;
        for (size_t i = 0; i < 16; i++)
            gd.L[2].im.p[i] = 0xff00ffffu;                              /* cyan, multiplied */
        gd.L[1].visible = false;
        CHECK(doc_flatten(&gd, &gf), "pass through");
        uint32_t through = gf.p[5];
        pix_free(&gf);
        gd.G[0].blend = BM_NORMAL;
        gd.G[0].opacity = 99;
        CHECK(doc_flatten(&gd, &gf), "isolated");
        uint32_t iso = gf.p[5];
        pix_free(&gf);
        CHECK(ch_r(through) < 30 && ch_g(through) < 30 && ch_b(through) < 30 && ch_g(iso) > 200 && ch_b(iso) > 200,
              "multiply sees the red below only through a pass through group (%08x, %08x)", through, iso);
        gd.L[1].visible = true;
        gd.L[2].blend = BM_NORMAL;
        gd.G[0].opacity = 100;
        gd.G[0].blend = BM_PASS;
        gd.cur = 0;                                                     /* moving: the layer at the bottom goes up past a whole group */
        CHECK(doc_move(&gd, 1) && gd.cur == 2 && gd.L[0].gid == 1 && gd.L[1].gid == 1 && gd.L[2].gid == 0, "a layer moves past a group (cur %d)", gd.cur);
        CHECK(doc_move(&gd, -1) && gd.cur == 0 && gd.L[1].gid == 1, "and back");
        CHECK(doc_move_item(&gd, 0, 1, 1) && gd.L[1].gid == 0 && gd.L[2].gid == 1 && gd.L[3].gid == 1, "a group moves as a block");
        CHECK(group_range(&gd, 1, &lo, &hi) && hi - lo == 1, "the group stays whole (%d..%d)", lo, hi);
        /* in and out: the layer outside the group next to it goes in, and (from the middle of it) out again */
        int outside = lo > 0 ? lo - 1 : hi + 1;
        CHECK(doc_enter_group(&gd, outside, 0, outside < lo ? 1 : -1) && gd.L[outside].gid == 1, "a layer goes into the group next to it");
        CHECK(group_range(&gd, 1, &lo, &hi) && hi - lo == 2, "which is three layers now");
        CHECK(doc_leave_group(&gd, lo, 0) && gd.L[hi].gid == 0 && group_range(&gd, 1, &lo, &hi) && hi - lo == 1, "a layer from the bottom of a group goes to the top of it and out");
        CHECK(doc_ungroup(&gd, 1) && groups_used(&gd) == 0 && gd.L[1].gid == 0 && gd.L[2].gid == 0, "ungroup keeps the layers");
        doc_free(&gd);
        /* nested groups, a copy, a merge, a delete */
        CHECK(pix_alloc(&gim, 4, 4, 0xffff0000u), "alloc");
        doc_from_pix(&gd, &gim);
        for (int i = 1; i < 6; i++)
            CHECK(doc_add(&gd, "L", 0), "layer %d", i);
        for (int l = 1; l < 5; l++)
            for (size_t i = 0; i < 16; i++)
                gd.L[l].im.p[i] = argb(100 + l * 30, 40 * l, 255 - 40 * l, l * 20);
        group_init(&gd.G[0], "Outer", 0);
        group_init(&gd.G[1], "Inner", 1);
        gd.L[1].gid = 1;
        gd.L[2].gid = 2;
        gd.L[3].gid = 2;
        gd.L[4].gid = 1;
        CHECK(group_range(&gd, 1, &lo, &hi) && lo == 1 && hi == 4 && group_range(&gd, 2, &lo, &hi) && lo == 2 && hi == 3, "nested ranges");
        CHECK(doc_dup_group(&gd, 1) == 3 && gd.n == 10 && groups_used(&gd) == 4 && gd.G[3].parent == 3, "a copy of the outer group, with a copy of the inner one inside it");
        CHECK(doc_delete_group(&gd, 3) && gd.n == 6 && groups_used(&gd) == 2, "delete a group with what is in it (and the groups in it)");
        {
            uint8_t incl[MAX_LAYERS] = { 0 };
            for (int i = 1; i <= 4; i++)
                incl[i] = 1;
            struct pix want;
            CHECK(pix_alloc(&want, 4, 4, 0), "alloc");
            composite_px(&gd, incl, 0, &want, (struct irect){ 0, 0, 4, 4 });
            struct doc keep;
            CHECK(doc_merge_group(&gd, 2) && gd.n == 5 && gd.L[2].gid == 1 && groups_used(&gd) == 1, "the inner group becomes a layer in the outer one");
            CHECK(doc_merge_group(&gd, 1) && gd.n == 3 && gd.L[1].gid == 0 && groups_used(&gd) == 0 && memcmp(gd.L[1].im.p, want.p, 64) == 0, "and the outer one: what it showed, as one layer");
            pix_free(&want);
            (void)keep;
        }
        gd.cur = 0;
        CHECK(!doc_delete_group(&gd, 1), "no group to delete");
        doc_free(&gd);
    }
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
    for (int k = 0; k < S_COUNT; k++)
        T.val[k] = SLIDERS[k].def;
    T.tsize = 24;
    T.fg = 0xff000000u;
    T.bg = 0xffff0000u;
    snprintf(T.txt.buf, sizeof(T.txt.buf), "Hi\nyo yo");
    T.txt.len = 8;
    T.txt.caret = 8;
    T.txt.x = 4;
    T.txt.y = 4;
    struct tmetrics tm, tm2;
    struct pix tpx = { 0 };
    CHECK(text_build(&T, &tpx, &tm) && tpx.w > 10, "text is built");
    int ink = 0;
    for (size_t i = 0; i < (size_t)tpx.w * (size_t)tpx.h; i++)
        ink += ch_a(tpx.p[i]) != 0;
    CHECK(ink > 20 && tm.cay > 0 && tm.cax > 0 && tm.h > tm.lh * 1.5 && tm.w > 4, "text is laid out (%d pixels, caret %.0f,%.0f, line %.0f, box %.0fx%.0f)", ink, tm.cax, tm.cay, tm.lh, tm.w, tm.h);
    pix_free(&tpx);
    T.talign = 2;                                                       /* right aligned: the short first line's caret would sit further right */
    T.txt.caret = 2;
    T.talign = 0;
    text_build(&T, NULL, &tm);
    T.talign = 2;
    text_build(&T, NULL, &tm2);
    CHECK(tm2.cax > tm.cax + 5, "right aligned text starts further right (%.1f against %.1f)", tm2.cax, tm.cax);
    T.talign = 0;
    T.val[S_LETTER] = 6;
    text_build(&T, NULL, &tm2);
    CHECK(tm2.w > tm.w + 6, "letter spacing makes it wider");
    T.val[S_LETTER] = 0;
    T.val[S_LINE] = 200;
    text_build(&T, NULL, &tm2);
    CHECK(tm2.h > tm.h * 1.6, "line spacing makes it taller");
    T.val[S_LINE] = 100;
    T.val[S_OUTLINE] = 3;
    T.val[S_SHADOW] = 6;
    struct pix big = { 0 }, plain = { 0 };
    T.val[S_OUTLINE] = 0;
    T.val[S_SHADOW] = 0;
    CHECK(text_build(&T, &plain, &tm), "plain text");
    T.val[S_OUTLINE] = 3;
    T.val[S_SHADOW] = 6;
    CHECK(text_build(&T, &big, &tm2) && big.w > plain.w + 6 && big.h > plain.h + 6, "an outline and a shadow need room");
    bool red = false;
    for (size_t i = 0; i < (size_t)big.w * (size_t)big.h && !red; i++)
        red = ch_a(big.p[i]) == 255 && ch_r(big.p[i]) == 255 && ch_g(big.p[i]) == 0;
    CHECK(red, "the outline is in the second colour");
    pix_free(&big);
    T.val[S_OUTLINE] = 0;
    T.val[S_SHADOW] = 0;
    T.val[S_ANGLE] = 90;
    CHECK(text_build(&T, &big, &tm2) && big.w < plain.w && big.h > plain.h, "turned a quarter, it is tall and narrow (%dx%d against %dx%d)", big.w, big.h, plain.w, plain.h);
    pix_free(&big);
    pix_free(&plain);
    T.val[S_ANGLE] = 0;
    {
        double lx, ly, px, py;
        T.val[S_ANGLE] = 30;
        text_build(&T, NULL, &tm);
        tm_to_pic(&tm, 5, 7, &px, &py);
        tm_from_pic(&tm, px, py, &lx, &ly);
        CHECK(fabs(lx - 5) < 1e-6 && fabs(ly - 7) < 1e-6, "a point goes into the turned frame and back");
        T.val[S_ANGLE] = 0;
    }
    {                                                                   /* bent text */
        struct pix st = { 0 }, ar = { 0 }, bo = { 0 };
        T.txt.len = T.txt.caret = 2;
        snprintf(T.txt.buf, sizeof(T.txt.buf), "Hello World");
        T.txt.len = T.txt.caret = 11;
        CHECK(text_build(&T, &st, &tm), "straight text for the bend");
        T.val[S_BEND] = 60;
        CHECK(text_build(&T, &ar, &tm2) && ar.h > st.h + 10 && ar.w < st.w + st.h, "an arch needs more height (%dx%d against %dx%d)", ar.w, ar.h, st.w, st.h);
        int topleft = -1, topmid = -1;                                  /* the ends of an arch hang lower than its middle */
        for (int y = 0; y < ar.h && (topleft < 0 || topmid < 0); y++) {
            for (int x = 0; x < ar.w / 5 && topleft < 0; x++)
                if (ch_a(ar.p[(size_t)y * (size_t)ar.w + (size_t)x]))
                    topleft = y;
            for (int x = ar.w * 2 / 5; x < ar.w * 3 / 5 && topmid < 0; x++)
                if (ch_a(ar.p[(size_t)y * (size_t)ar.w + (size_t)x]))
                    topmid = y;
        }
        CHECK(topleft > topmid + 3, "an arch: the left end (%d) is lower than the middle (%d)", topleft, topmid);
        T.val[S_BEND] = -60;
        CHECK(text_build(&T, &bo, &tm2) && bo.h > st.h + 10, "a bowl needs height too");
        int botleft = -1, botmid = -1;
        for (int y = bo.h - 1; y >= 0 && (botleft < 0 || botmid < 0); y--) {
            for (int x = 0; x < bo.w / 5 && botleft < 0; x++)
                if (ch_a(bo.p[(size_t)y * (size_t)bo.w + (size_t)x]))
                    botleft = y;
            for (int x = bo.w * 2 / 5; x < bo.w * 3 / 5 && botmid < 0; x++)
                if (ch_a(bo.p[(size_t)y * (size_t)bo.w + (size_t)x]))
                    botmid = y;
        }
        CHECK(botleft < botmid - 3, "a bowl: the left end (%d) is higher than the middle (%d)", botleft, botmid);
        T.val[S_ANGLE] = 40;
        struct pix tb = { 0 };
        CHECK(text_build(&T, &tb, &tm2) && tb.w > 10, "bent and turned");
        pix_free(&tb);
        T.val[S_ANGLE] = 0;
        T.val[S_BEND] = 0;
        pix_free(&st);
        pix_free(&ar);
        pix_free(&bo);
        T.txt.len = T.txt.caret = 8;
        snprintf(T.txt.buf, sizeof(T.txt.buf), "Hi\nyo yo");
    }
    {                                                                   /* the rulers' units */
        T.ruler_dpi = 96;
        T.zoom = 1.0;
        T.ruler_unit = RU_PX;
        double step, minor;
        ruler_steps(&T, &step, &minor);
        CHECK(ruler_ppu(&T) == 1.0 && step == 100 && minor == 10, "pixels at 100 %%: a number every %g, ticks every %g", step, minor);
        T.ruler_unit = RU_IN;
        CHECK(ruler_ppu(&T) == 96.0, "an inch is the dpi");
        ruler_steps(&T, &step, &minor);
        CHECK(step == 1 && minor == 0.1, "inches at 96 dpi: a number every %g, ticks every %g", step, minor);
        T.ruler_unit = RU_MM;
        CHECK(fabs(ruler_ppu(&T) - 96.0 / 25.4) < 1e-9, "a millimetre at 96 dpi is %.4f pixels", ruler_ppu(&T));
        ruler_steps(&T, &step, &minor);
        CHECK(step == 20 && minor == 2, "millimetres: a number every %g, ticks every %g", step, minor);
        T.ruler_unit = RU_CM;
        T.ruler_dpi = 300;
        ruler_steps(&T, &step, &minor);
        CHECK(fabs(ruler_ppu(&T) - 300.0 / 2.54) < 1e-9 && step == 0.5 && fabs(minor - 0.05) < 1e-12, "centimetres at 300 dpi (%.2f px each): %g, %g", ruler_ppu(&T), step, minor);
        T.zoom = 8.0;
        T.ruler_unit = RU_PT;
        T.ruler_dpi = 72;
        ruler_steps(&T, &step, &minor);
        CHECK(ruler_ppu(&T) == 1.0 && step == 10 && minor == 1, "points at 72 dpi, zoomed in 8 times: %g, %g", step, minor);
        char t[40];
        T.ruler_unit = RU_MM;
        T.ruler_dpi = 254;
        ruler_text(&T, 100, t, sizeof(t));
        CHECK(!strcmp(t, "10.00 mm"), "100 pixels at 254 dpi are 10 mm (%s)", t);
        T.ruler_unit = RU_PX;
        T.zoom = 1.0;
    }
    /* the help: every page sets out to a sane height at every width, and no line is wider than its room unless one word is */
    {
        for (int p = 0; p < HP_COUNT; p++) {
            CHECK(HP[p].title && *HP[p].title && HP[p].n > 3, "help page %d has a title and text", p);
            for (int width = 300; width <= 760; width += 230) {
                g_hl_page = -1;
                help_layout(p, width);
                CHECK(g_hl_n > 3 && g_hl_n < HL_MAX - 20, "help page %d at %d px: %d lines (the table has %d)", p, width, g_hl_n, HL_MAX);
                CHECK(g_hl_h > 40 && g_hl_h < 12000, "help page %d at %d px is %d px high", p, width, g_hl_h);
                for (int i = 0; i < g_hl_n; i++) {
                    char buf[600];
                    int n = LM_MIN(g_hl[i].len, (int)sizeof(buf) - 1);
                    memcpy(buf, g_hl[i].p, (size_t)n);
                    buf[n] = 0;
                    bool one_word = !strchr(buf, ' ');
                    CHECK(one_word || g_hl[i].kind == HL_DOT || hl_text_w(buf, g_hl[i].kind) + g_hl[i].x <= width + 2, "help page %d at %d px: a line is %d px wide (%.30s)", p, width, hl_text_w(buf, g_hl[i].kind), buf);
                    CHECK(g_hl[i].kind == HL_DOT || n > 0, "help page %d has an empty line", p);
                    for (int k = 0; k < n; k++)
                        if ((unsigned char)buf[k] < 32 || (unsigned char)buf[k] == 127) {
                            CHECK(0, "help page %d at %d px: line %d holds a control character (a dangling pointer?)", p, width, i);
                            break;
                        }
                }
            }
        }
        g_hl_page = -1;
    }
    /* the effects */
    {
        struct pix fxp;
        CHECK(pix_alloc(&fxp, 20, 20, 0xff000000u), "alloc for effects");
        for (int y = 0; y < 20; y++)
            for (int x = 10; x < 20; x++)
                fxp.p[(size_t)y * 20 + (size_t)x] = 0xffffffffu;
        CHECK(blur_pix(&fxp, (struct irect){ 0, 0, 20, 20 }, 3) && ch_r(fxp.p[10 * 20 + 9]) > 20 && ch_r(fxp.p[10 * 20 + 9]) < 235 && ch_r(fxp.p[10 * 20 + 0]) < 20, "a blur softens an edge");
        pix_free(&fxp);
        CHECK(pix_alloc(&fxp, 8, 8, 0xff804020u), "alloc");
        levels_apply(&fxp, (struct irect){ 0, 0, 8, 8 }, 0, 255, 100);
        CHECK(fxp.p[0] == 0xff804020u, "levels that change nothing change nothing");
        levels_apply(&fxp, (struct irect){ 0, 0, 8, 8 }, 0x20, 0xc0, 100);
        CHECK(ch_r(fxp.p[0]) > 0x80 - 0x20 && ch_b(fxp.p[0]) == 0, "levels stretch the middle and cut the dark");
        recolour(&fxp, (struct irect){ 0, 0, 8, 8 }, fxp.p[0], 0xff00ff00u, 5);
        CHECK(ch_g(fxp.p[0]) == 255 && ch_r(fxp.p[0]) == 0, "a colour is replaced");
        colour_to_alpha(&fxp, (struct irect){ 0, 0, 8, 8 }, 0xff00ff00u, 10);
        CHECK(ch_a(fxp.p[0]) == 0, "a colour becomes transparent");
        pix_free(&fxp);
        uint8_t pl[40 * 40] = { 0 }, dl[40 * 40];
        pl[20 * 40 + 20] = 255;
        dilate_cov(pl, dl, 40, 40, 5);
        CHECK(dl[20 * 40 + 24] > 200 && dl[20 * 40 + 27] == 0 && dl[14 * 40 + 14] == 0, "a point grows into a disc");
        struct doc rd;
        CHECK(pix_alloc(&fxp, 10, 4, 0xffff0000u), "alloc for turn");
        doc_from_pix(&rd, &fxp);
        CHECK(rotate_doc(&rd, 90) && doc_w(&rd) == 4 && doc_h(&rd) == 10 && ch_a(rd.L[0].im.p[5 * 4 + 2]) == 255, "a quarter turn swaps the sides");
        CHECK(rotate_doc(&rd, 45) && doc_w(&rd) > 9 && ch_a(rd.L[0].im.p[0]) == 0, "a slanted turn grows the sheet and leaves the corners empty");
        doc_free(&rd);
        struct pix sh, src2;
        CHECK(pix_alloc(&src2, 30, 30, 0) && (src2.p[15 * 30 + 15] = 0xff0000ffu, make_shadow_pix(&src2, &sh, 8, 0, 50, 0xff000000u)) && ch_a(sh.p[21 * 30 + 21]) > 100 && ch_a(sh.p[15 * 30 + 15]) == 0, "a shadow lies down and to the right");
        pix_free(&sh);
        CHECK(make_outline_pix(&src2, &sh, 3, 0, 0xffff0000u) && ch_a(sh.p[15 * 30 + 17]) > 200 && ch_a(sh.p[15 * 30 + 22]) == 0, "an outline reaches as far as it was told");
        pix_free(&sh);
        pix_free(&src2);
    }
    fonts_scan();
    if (g_nfam > 0) {                                                   /* a real font: the same text is a different picture */
        struct pix a1 = { 0 }, a2 = { 0 };
        T.tfont_name[0] = 0;
        text_build(&T, &a1, &tm);
        snprintf(T.tfont_name, sizeof(T.tfont_name), "%s", g_fam[0].name);
        CHECK(text_build(&T, &a2, &tm2) && (a1.w != a2.w || a1.h != a2.h || memcmp(a1.p, a2.p, (size_t)a1.w * (size_t)a1.h * 4)), "a font of the family %s changes the picture", g_fam[0].name);
        T.tg[TG_BOLD] = true;
        T.tg[TG_ITALIC] = true;
        struct pix a3 = { 0 };
        CHECK(text_build(&T, &a3, &tm) && a3.p, "bold italic is made (from the file or by hand)");
        pix_free(&a1);
        pix_free(&a2);
        pix_free(&a3);
    }

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
    else if (kind >= MENU_GROUP0 && kind < MENU_GROUP0 + G_COUNT && index >= 0 && index < GRP_N[kind - MENU_GROUP0]) {
        tool_set(a, GRP_TOOLS[kind - MENU_GROUP0][index]);
    } else if (kind == 5 && index >= 0 && index < BM_COUNT) {
        checkpoint_props(a);
        a->doc.L[a->doc.cur].blend = index;
        changed(a);
    } else if (kind == 9 && index >= 0 && index < RU_COUNT + 4) {                       /* the rulers' unit or resolution */
        if (index < RU_COUNT)
            a->ruler_unit = index;
        else
            a->ruler_dpi = RU_DPI[index - RU_COUNT];
        a->vdirty = true;
        say(a, "Rulers in %s at %d dpi", RU_NAME[a->ruler_unit], a->ruler_dpi);
    } else if ((kind == 6 || kind == 8) && index >= 0 && index < a->nlmenu) {
        item_action(a, a->lmenu[index]);
    } else if (kind == 7 && index >= 0 && index <= BM_COUNT && a->gsel) {      /* the group's blend mode: 0 = pass through */
        checkpoint_props(a);
        a->doc.G[a->gsel - 1].blend = index == 0 ? BM_PASS : index - 1;
        changed(a);
    } else if (kind == 0 && index >= 0)                                    /* the answer of a text field's menu */
        wltext_menu_do(&a->clip, a->pbuf, sizeof(a->pbuf), index);
    w->redraw = true;
}

static void adj_set_from(struct app *a, int i, double x, const struct wlcard *c)
{
    int tx, ty, tw;
    adj_track(c, i, &tx, &ty, &tw);
    double t = LM_CLAMP((x - tx) / (double)tw, 0.0, 1.0);
    *adj_ptr(a, i) = FXK[a->fxk].p[i].mn + (int)floor(t * (FXK[a->fxk].p[i].mx - FXK[a->fxk].p[i].mn) + 0.5);
    fx_update(a);
    a->win.redraw = true;
}

/* The picture follows the pointer while the middle button pans it. */
static void pan_motion(struct app *a)
{
    a->vx = a->pan_vx + (a->px - a->pan_x);
    a->vy = a->pan_vy + (a->py - a->pan_y);
    a->fitted = false;
    view_clamp(a);
    a->vdirty = true;
}

static void card_pointer(struct app *a, uint32_t op, const union lm_arg *args)
{
    struct wlcard c;
    int nb;
    card_rect(a, &c, &nb);
    int crows = card_list_rows(a), fx = c.x + WLCARD_PAD, fw = c.w - 2 * WLCARD_PAD, ly = card_list_y(&c);
    bool field = a->prompt == P_OPEN || a->prompt == P_SAVEAS || a->prompt == P_NEW || a->prompt == P_RESIZE || a->prompt == P_CANVAS || a->prompt == P_LAYERNAME || a->prompt == P_PLACE;
    if (op == LM_WL_POINTER_EVT_MOTION) {
        if (a->prompt != P_ADJUST && wlcard_drag_motion(&a->win, &c, a->px, a->py))             /* a card held by its title follows the pointer */
            return;
        int k = wlcard_button_at(&c, nb, (int)a->px, (int)a->py);
        if (k != a->pcard_hover)
            a->pcard_hover = k;
        if (crows) {
            char base[PATH_MAX];
            default_dir(a, base, sizeof(base));
            wlpicker_motion(&a->pk, base, a->pbuf, ly, crows, a->py);               /* (the scroll bar's thumb is dragged) */
        }
        if (a->prompt == P_HELP) {
            int h = help_hit(a, a->px, a->py);
            a->help_hover = h >= 1000 ? h - 1000 : -1;
            a->help_btn = h == 1 || h == 2 ? h : 0;
            if (a->help_drag) {                                 /* the scroll bar's thumb follows the pointer */
                struct wlcard hc;
                int hnb;
                card_rect(a, &hc, &hnb);
                struct hgeo hg;
                help_geom(&hc, &hg);
                help_layout(a->help_page, hg.cw);
                struct wlsbar sb = help_sbar(&hg, a->help_top);
                a->help_top = wlsbar_drag(&sb, a->py, a->help_grab);
            }
        }
        if (a->prompt == P_COLOUR && a->cp.drag)
            cp_drag_to(a, a->px, a->py);
        if (a->prompt == P_FONT && a->font_grab >= 0) {                         /* the scroll bar's thumb follows the pointer */
            struct wlsbar fb = font_sbar(a, &c);
            a->font_top = wlsbar_drag(&fb, a->py, a->font_grab);
        }
        if (a->prompt == P_ADJUST && a->adj_drag >= 0)
            adj_set_from(a, a->adj_drag, a->px, &c);
        if (a->prompt == P_ADJUST && a->adj_moving) {                           /* the panel follows the pointer by its title */
            a->adj_x = LM_CLAMP((int)(a->px - a->adj_gx), 0, LM_MAX(a->win.bw - c.w, 0));
            a->adj_y = LM_CLAMP((int)(a->py - a->adj_gy), 0, LM_MAX(a->win.bh - c.h, 0));
            a->adj_placed = true;
        }
        if (a->prompt == P_ADJUST && a->pan)
            pan_motion(a);
        a->win.redraw = true;                                                   /* (the list rows light under the pointer) */
        return;
    }
    if (op == LM_WL_POINTER_EVT_AXIS) {
        if (a->prompt == P_ADJUST && args[1].u == 0 && !wlcard_has(&c, (int)a->px, (int)a->py)) {        /* over the picture: the wheel zooms it, as usual */
            if (args[2].f != 0 && in_canvas(a, a->px, a->py))
                zoom_to(a, a->zoom * (args[2].f < 0 ? 1.25 : 1.0 / 1.25), a->px - cv_x(a), a->py);
            a->win.redraw = true;
            return;
        }
        if (a->prompt == P_HELP && args[1].u == 0) {
            help_scroll(a, (args[2].f > 0 ? 1 : -1) * HELP_LH * 3);
            return;
        }
        if (a->prompt == P_FONT && args[1].u == 0) {
            int fr = font_rows_fit(a), fn = g_nfam + 1;
            a->font_top = LM_CLAMP(a->font_top + (args[2].f > 0 ? 1 : -1), 0, LM_MAX(fn - fr, 0));
            a->win.redraw = true;
            return;
        }
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
        a->help_drag = false;
        a->font_grab = -1;
        a->adj_moving = false;
        a->pan = false;
        wlcard_drag_release(&a->win);
        if (a->pcard_down >= 0 && a->pcard_down == k) {                                        /* a button is done when it is let go on */
            int which = k;
            a->pcard_down = -1;
            if (a->prompt == P_HELP || which == 0)
                prompt_close(a, false);
            else if (a->prompt == P_DISCARD)
                discard_chosen(a, which == 2);
            else if (a->prompt == P_ADJUST && which == 1) {
                fx_reset_values(a);
                fx_update(a);
            } else
                prompt_accept(a);
        }
        a->pcard_down = -1;
        a->win.redraw = true;
        return;
    }
    if (args[2].u == 274 && a->prompt == P_ADJUST) {                                            /* the middle button pans the picture behind the panel */
        if (!wlcard_has(&c, (int)a->px, (int)a->py) && in_canvas(a, a->px, a->py)) {
            a->pan = true;
            a->pan_x = a->px;
            a->pan_y = a->py;
            a->pan_vx = a->vx;
            a->pan_vy = a->vy;
        }
        return;
    }
    if (args[2].u == 272) {
        if (k >= 0) {
            a->pcard_down = k;
        } else if (a->prompt != P_ADJUST && wlcard_drag_press(&a->win, &c, a->px, a->py)) {
            a->win.redraw = true;                                                               /* the title: take hold of the card */
        } else if (!wlcard_has(&c, (int)a->px, (int)a->py)) {
            if (a->prompt != P_ADJUST)                                                          /* (an effect panel stays: a stray click must not lose the work) */
                prompt_close(a, false);                                                         /* a click outside the card cancels */
        } else if (a->prompt == P_ADJUST && a->py < c.y + 38) {                                 /* the title bar: take hold of the panel */
            a->adj_moving = true;
            a->adj_gx = a->px - c.x;
            a->adj_gy = a->py - c.y;
        } else if (a->prompt == P_HELP) {
            int h = help_hit(a, a->px, a->py);
            if (h >= 1000)
                help_set_page(a, h - 1000);
            else if (h == 1)
                help_set_page(a, a->help_page - 1);
            else if (h == 2)
                help_set_page(a, a->help_page + 1);
            else if (h == 3) {                                  /* the scroll bar: the thumb is taken, the track pages */
                struct hgeo hg;
                help_geom(&c, &hg);
                help_layout(a->help_page, hg.cw);
                struct wlsbar sb = help_sbar(&hg, a->help_top);
                int grab = 0;
                if (wlsbar_press(&sb, a->px, a->py, &a->help_top, &grab) == 1) {
                    a->help_drag = true;
                    a->help_grab = grab;
                }
            }
        } else if (a->prompt == P_EFFECTS) {
            for (int i = 0; i < N_EFX; i++) {
                int x, y, bw, bh;
                efx_rect(&c, i, &x, &y, &bw, &bh);
                if (a->px >= x && a->px < x + bw && a->py >= y && a->py < y + bh) {
                    effects_run(a, i);
                    break;
                }
            }
        } else if (a->prompt == P_FONT) {
            struct wlsbar fb = font_sbar(a, &c);
            int grab = 0, hit = wlsbar_press(&fb, a->px, a->py, &a->font_top, &grab);          /* the scroll bar: the thumb is taken, the track pages */
            if (hit == 1)
                a->font_grab = grab;
            if (hit)
                a->win.redraw = true;
            int fr = font_rows_fit(a), r = hit ? -1 : (int)((a->py - font_row_y(&c, 0)) / FONT_ROW_H);
            if (a->py >= font_row_y(&c, 0) && r >= 0 && r < fr && a->font_top + r <= g_nfam && a->px >= c.x + WLCARD_PAD && a->px < c.x + c.w - WLCARD_PAD)
                font_choose(a, a->font_top + r);
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
            for (int i = 0; i < FXK[a->fxk].n; i++) {
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
    if (hit != LH_ROW && hit != LH_GROUP)
        a->lay_click_idx = -1;                                           /* (a double click is two clicks in a row on the same row) */
    switch (hit) {
    case LH_BAR: {                                                       /* the scroll bar: the thumb is taken, the track pages */
        if (button != 272)
            break;
        struct lp_g g;
        lp_geom(a, &g);
        struct lrow rows[MAX_LAYERS + MAX_GROUPS];
        struct wlsbar sb = lp_sbar(&g, lp_rows(a, rows), a->lay_top);
        int grab = 0;
        if (wlsbar_press(&sb, a->px, a->py, &a->lay_top, &grab) == 1)
            a->lay_grab = grab;
        break;
    }
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
            *lay_opacity(a) = slider_value_at(0, 100, a->px, g.ox + SL_KNOB / 2 - SL_KNOB / 2, g.ow);
            changed(a);
        }
        break;
    case LH_BLEND:
        if (button != 272)
            break;
        if (a->gsel) {                                                  /* a group's blend mode has "Pass through" too */
            if (wlwin_menu(&a->win, "Pass through\nNormal\nMultiply\nScreen\nOverlay\nDarken\nLighten\nDifference"))
                a->menu_kind = 7;
        } else if (wlwin_menu(&a->win, "Normal\nMultiply\nScreen\nOverlay\nDarken\nLighten\nDifference"))
            a->menu_kind = 5;
        break;
    case LH_GEYE:
        if (button == 272)
            group_toggle_visible(a, idx);
        break;
    case LH_GFOLD:
        if (button == 272)
            group_toggle_fold(a, idx);
        break;
    case LH_GROUP: {
        int64_t now = now_ms();
        bool dbl = button == 272 && 1000 + idx == a->lay_click_idx && now - a->lay_click_ms < lm_metrics.double_click_ms;      /* (a group's key is 1000 + its slot; a layer's is its index) */
        a->lay_click_idx = button == 272 ? 1000 + idx : -1;
        a->lay_click_ms = now;
        group_select(a, idx);
        if (dbl) {
            prompt_open(a, P_LAYERNAME, a->doc.G[idx - 1].name);
            wltext_select_all(a->pbuf);
        } else if (button == 273)
            item_menu(a, true);
        break;
    }
    case LH_EYE:
        if (button == 272)
            layer_toggle(a, idx);
        break;
    case LH_MASK:
        if (button == 272) {
            layer_select(a, idx);
            mask_edit_toggle(a);
        }
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
        } else if (button == 273)
            item_menu(a, false);
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
        if (button == 272) {
            if (s.tg[idx].id == TG_RATIO)
                a->ratio = (a->ratio + 1) % 6;
            else
                a->tg[s.tg[idx].id] = !a->tg[s.tg[idx].id];
        }
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
        *lay_opacity(a) = slider_value_at(0, 100, a->px, g.ox, g.ow);
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
    if (brush_bar_tool(a->tool)) {
        struct bbar_g bg;
        bbar_layout(a, &bg);
        for (int i = 0; i < bg.nsl; i++)
            if (bg.sl[i].id == a->sl_drag) {
                int mn, mx;
                slider_range(a, a->sl_drag, &mn, &mx);
                slider_set(a, a->sl_drag, slider_value_at(mn, mx, a->px, bg.sl[i].x, bg.sl[i].w));
            }
    }
    if (a->tool == T_TEXT) {
        struct tbar_g g;
        tbar_layout(a, &g);
        for (int i = 0; i < g.nsl; i++)
            if (g.sl[i].id == a->sl_drag) {
                int mn, mx;
                slider_range(a, a->sl_drag, &mn, &mx);
                slider_set(a, a->sl_drag, slider_value_at(mn, mx, a->px, g.sl[i].x, g.sl[i].w));
                if (a->txt.on)
                    text_preview(a);
            }
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
        if (a->guide_drag >= 0) {                                      /* a guide follows the pointer */
            double ix, iy;
            to_pic(a, a->px, a->py, &ix, &iy);
            a->guide[a->guide_drag].pos = (int)lround(a->guide[a->guide_drag].vert ? ix : iy);
        } else if (a->lay_grab >= 0) {                                   /* the layers panel's scroll bar thumb follows the pointer */
            struct lp_g g;
            lp_geom(a, &g);
            struct lrow rows[MAX_LAYERS + MAX_GROUPS];
            struct wlsbar sb = lp_sbar(&g, lp_rows(a, rows), a->lay_top);
            a->lay_top = wlsbar_drag(&sb, a->py, a->lay_grab);
        } else if (a->sl_drag >= 0)
            slider_drag(a);
        else if (a->pan)
            pan_motion(a);
        else if (a->drag) {
            double ix, iy;
            to_pic(a, a->px, a->py, &ix, &iy);
            if (tool_snaps(a->tool))
                snap_pic(a, &ix, &iy);
            drag_motion(a, ix, iy, (w->mods & LM_MOD_SHIFT) != 0);
        } else if (a->poly_on && a->tool == T_FREEPOLY) {                 /* a rubber band follows the pointer */
            double ix, iy;
            to_pic(a, a->px, a->py, &ix, &iy);
            snap_pic(a, &ix, &iy);
            poly_draw(a, ix, iy, true, false);
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
            a->lay_grab = -1;
            if (a->sl_drag >= 0) {
                a->sl_drag = -1;
                a->modified = a->modified || false;
            }
            if (a->guide_drag >= 0 && button == 272) {                     /* let go on a ruler (or outside the picture): the guide is taken away */
                int gi = a->guide_drag;
                a->guide_drag = -1;
                if (ruler_at(a, a->px, a->py) || !in_canvas(a, a->px, a->py)) {
                    a->guide[gi] = a->guide[a->nguides - 1];
                    a->nguides--;
                }
            } else if (button == 274 || (button == 272 && a->pan))
                a->pan = false;
            else if ((int)button == a->drag_button || a->drag)
                end_drag(a);
            w->redraw = true;
            break;
        }
        int bi = button_at(a, a->px, a->py);
        if (button == 273 && group_of_button(bi) >= 0) {                              /* a right click on a group opens its menu at once */
            int g = group_of_button(bi);
            finish_pending(a);
            if (!wlwin_menu(&a->win, GRP_MENU[g]))
                say(a, "The compositor has no menu service");
            else
                a->menu_kind = MENU_GROUP0 + g;
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
        if (tbar_h(a) && a->py >= tb_y(a) && a->py < pal_y(a)) {
            if (a->tool == T_TEXT)
                tbar_press(a, button);
            else
                bbar_press(a, button);
            break;
        }
        if (a->py >= pal_y(a) && a->py < pal_y(a) + PAL_H) {
            strip_press(a, button);
            break;
        }
        if (button == 273 && a->rulers && ruler_at(a, a->px, a->py)) {        /* a right click on a ruler: the unit and the resolution */
            char m[300] = "";
            for (int i = 0; i < RU_COUNT; i++)
                snprintf(m + strlen(m), sizeof(m) - strlen(m), "%s%s%s", i ? "\n" : "", a->ruler_unit == i ? "* " : "", RU_NAME[i]);
            for (int i = 0; i < 4; i++)
                snprintf(m + strlen(m), sizeof(m) - strlen(m), "\n%s%d dpi", a->ruler_dpi == RU_DPI[i] ? "* " : "", RU_DPI[i]);
            if (wlwin_menu(&a->win, m))
                a->menu_kind = 9;
            break;
        }
        if (button == 272 && a->rulers) {                              /* a guide: taken from a ruler, or an old one moved */
            int ru = ruler_at(a, a->px, a->py), gi = ru ? -1 : guide_at(a, a->px, a->py);
            if (ru == 3) {                                              /* the corner: the next unit */
                a->ruler_unit = (a->ruler_unit + 1) % RU_COUNT;
                say(a, "Rulers in %s (%d dpi: right click a ruler to change it)", RU_NAME[a->ruler_unit], a->ruler_dpi);
                a->vdirty = true;
                w->redraw = true;
                break;
            }
            if (ru && a->nguides < 32) {
                double ix, iy;
                to_pic(a, a->px, a->py, &ix, &iy);
                a->guide[a->nguides].vert = ru == 2;
                a->guide[a->nguides].pos = (int)lround(ru == 2 ? ix : iy);
                a->guide_drag = a->nguides++;
                w->redraw = true;
                break;
            }
            if (gi >= 0) {
                a->guide_drag = gi;
                w->redraw = true;
                break;
            }
            if (ru)
                break;
        }
        if ((button == 274 || (button == 272 && (a->tool == T_HAND || a->space_down))) && in_canvas(a, a->px, a->py)) {     /* the middle button, the Hand tool, or Space: the picture follows the pointer */
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
            int tidx = -1, th = tbar_hit(a, a->px, a->py, &tidx), bidx = -1, bh = bbar_hit(a, a->px, a->py, &bidx);
            if (bh == TH_SLIDER) {
                struct bbar_g bg;
                bbar_layout(a, &bg);
                int id = bg.sl[bidx].id, mn, mx;
                slider_range(a, id, &mn, &mx);
                int step = (w->mods & LM_MOD_SHIFT) ? LM_MAX((mx - mn) / 10, 1) : LM_MAX((mx - mn) / 50, 1);
                slider_set(a, id, *slider_ptr(a, id) + (v < 0 ? step : -step));
            } else if (th == TH_SLIDER) {
                struct tbar_g tg;
                tbar_layout(a, &tg);
                int id = tg.sl[tidx].id, mn, mx;
                slider_range(a, id, &mn, &mx);
                int step = (w->mods & LM_MOD_SHIFT) ? LM_MAX((mx - mn) / 10, 1) : LM_MAX((mx - mn) / 50, 1);
                slider_set(a, id, *slider_ptr(a, id) + (v < 0 ? step : -step));
                if (a->txt.on)
                    text_preview(a);
            } else if (hit == SH_SLIDER) {
                int id = s.sl[sidx].id, mn, mx;
                slider_range(a, id, &mn, &mx);
                int step = (w->mods & LM_MOD_SHIFT) ? LM_MAX((mx - mn) / 10, 1) : LM_MAX((mx - mn) / 50, 1);
                slider_set(a, id, *slider_ptr(a, id) + (v < 0 ? step : -step));
                if (a->txt.on)
                    text_preview(a);
            } else if (lh == LH_OPACITY) {
                checkpoint_props(a);
                *lay_opacity(a) = LM_CLAMP(*lay_opacity(a) + (v < 0 ? 5 : -5), 0, 100);
                changed(a);
            } else if (layers_on(a) && a->px >= panel_x(a) && a->px < panel_x(a) + LP_W && a->py < cv_h(a)) {
                struct lp_g g;
                lp_geom(a, &g);
                struct lrow wr[MAX_LAYERS + MAX_GROUPS];
                a->lay_top = LM_CLAMP(a->lay_top + (v < 0 ? -1 : 1), 0, LM_MAX(lp_rows(a, wr) - g.nvis, 0));
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
        help_key(a, ks);
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
    if (a->prompt == P_FONT) {
        int fn = g_nfam + 1, fr = font_rows_fit(a);
        if (ks == LM_KS_Escape)
            prompt_close(a, false);
        else if (ks == LM_KS_Return || ks == LM_KS_KP_Enter)
            font_choose(a, a->font_sel);
        else if (ks == LM_KS_Up || ks == LM_KS_Down || ks == LM_KS_Page_Up || ks == LM_KS_Page_Down || ks == LM_KS_Home || ks == LM_KS_End) {
            int d = ks == LM_KS_Up ? -1 : ks == LM_KS_Down ? 1 : ks == LM_KS_Page_Up ? -fr : ks == LM_KS_Page_Down ? fr : 0;
            a->font_sel = ks == LM_KS_Home ? 0 : ks == LM_KS_End ? fn - 1 : LM_CLAMP(a->font_sel + d, 0, fn - 1);
            a->font_find[0] = 0;
            font_show(a);
        } else if (ks == LM_KS_BackSpace || (ks >= 32 && ks < 127 && !(a->win.mods & (LM_MOD_CTRL | LM_MOD_ALT)))) {                          /* type a name to jump to the family: "lo" finds Lobster, "lob" the same */
            size_t n = strlen(a->font_find);
            int64_t now = now_ms();
            if (now - a->font_find_ms > 1500)
                n = 0;                                                                          /* (a pause starts a new word) */
            if (ks == LM_KS_BackSpace)
                n = n ? n - 1 : 0;
            else if (n + 1 < sizeof(a->font_find))
                a->font_find[n++] = (char)ks;
            a->font_find[n] = 0;
            a->font_find_ms = now;
            for (int i = 0; n && i < g_nfam; i++)
                if (!strncasecmp(g_fam[i].name, a->font_find, n)) {
                    a->font_sel = i + 1;
                    font_show(a);
                    break;
                }
        }
        return;
    }
    if (a->prompt == P_ADJUST) {
        if (ks == LM_KS_Escape)
            prompt_close(a, false);
        else if (ks == LM_KS_Return || ks == LM_KS_KP_Enter)
            prompt_accept(a);
        else if (ks == '+' || ks == '=')                                                        /* look at the result: zoom keys work meanwhile */
            zoom_to(a, a->zoom * 1.25, cv_w(a) / 2.0, cv_h(a) / 2.0);
        else if (ks == '-')
            zoom_to(a, a->zoom / 1.25, cv_w(a) / 2.0, cv_h(a) / 2.0);
        else if (ks == '0')
            view_fit(a);
        else if (ks == '1')
            zoom_to(a, 1.0, cv_w(a) / 2.0, cv_h(a) / 2.0);
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
    if (op == LM_WL_KEYBOARD_EVT_LEAVE && a->space_down) {              /* (Space may come up while another window has the keys) */
        a->space_down = false;
        w->redraw = true;
    }
    if (op != LM_WL_KEYBOARD_EVT_KEY)
        return;
    a->clip.serial = args[0].u;
    if (args[2].u == 57 && args[3].u == 0 && a->space_down) {           /* Space let go */
        a->space_down = false;
        w->redraw = true;
        return;
    }
    if (args[3].u == 0)
        return;
    if (args[2].u == 57 && a->prompt == P_NONE && !a->txt.on && !(w->mods & (LM_MOD_CTRL | LM_MOD_ALT))) {      /* Space held: the picture can be moved with the left button */
        a->space_down = true;
        w->redraw = true;
        return;
    }
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
        if (alt && k == 'g') {                                          /* Ctrl+Alt+G: group the layer; with a group chosen, ungroup it */
            if (a->gsel)
                group_ungroup(a);
            else
                group_make(a);
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
        case 'v':
            if (shift)
                guard(a, AF_CLIPNEW);
            else
                do_paste(a);
            break;
        case 'j': layer_dup(a); break;
        case 'e': layer_merge(a); break;
        case 'g': a->grid = !a->grid; a->vdirty = true; break;
        case 'l': a->rulers = !a->rulers; a->vdirty = true; say(a, a->rulers ? "Rulers on: drag from them to make a guide, drag a guide back to take it away" : "Rulers off"); break;
        case 'b': a->snap = !a->snap; say(a, a->snap ? "Snapping on: tools stick to guides, edges and the middle" : "Snapping off"); break;
        default: break;
        }
        return;
    }
    if ((a->poly_on || a->curve_on) && (ks == LM_KS_Escape || ks == LM_KS_Return || ks == LM_KS_KP_Enter)) {
        if (ks == LM_KS_Escape)
            shape_cancel(a);
        else {
            poly_commit(a);
            curve_commit(a);
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
    case LM_KS_F1: help_open(a); break;
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
    if (!strcmp(iface, "mellagui_settings_v1"))
        a->settings = wlc_bind(w->c, w->registry, name, &lm_mellagui_settings_v1_interface, 1, NULL, NULL);
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
        else if (!strcmp(k, "text-font"))
            snprintf(a->tfont_name, sizeof(a->tfont_name), "%s", v);
        else if (!strcmp(k, "text-align"))
            a->talign = LM_CLAMP(atoi(v), 0, 2);
        else if (!strcmp(k, "text-line"))
            a->val[S_LINE] = LM_CLAMP(atoi(v), SLIDERS[S_LINE].mn, SLIDERS[S_LINE].mx);
        else if (!strcmp(k, "text-letters"))
            a->val[S_LETTER] = LM_CLAMP(atoi(v), SLIDERS[S_LETTER].mn, SLIDERS[S_LETTER].mx);
        else if (!strcmp(k, "text-outline"))
            a->val[S_OUTLINE] = LM_CLAMP(atoi(v), SLIDERS[S_OUTLINE].mn, SLIDERS[S_OUTLINE].mx);
        else if (!strcmp(k, "text-layer"))
            a->tg[TG_TLAYER] = atoi(v) != 0;
        else if (!strcmp(k, "text-bend"))
            a->val[S_BEND] = LM_CLAMP(atoi(v), SLIDERS[S_BEND].mn, SLIDERS[S_BEND].mx);
        else if (!strcmp(k, "text-shadow"))
            a->val[S_SHADOW] = LM_CLAMP(atoi(v), SLIDERS[S_SHADOW].mn, SLIDERS[S_SHADOW].mx);
        else if (!strcmp(k, "smooth"))
            a->val[S_SMOOTH] = LM_CLAMP(atoi(v), SLIDERS[S_SMOOTH].mn, SLIDERS[S_SMOOTH].mx);
        else if (!strcmp(k, "brush-style")) {
            a->tg[TG_PIXEL] = strchr(v, 'p') != NULL;
            a->tg[TG_MIRROR] = strchr(v, 'm') != NULL;
        } else if (!strcmp(k, "text-style")) {
            a->tg[TG_BOLD] = strchr(v, 'b') != NULL;
            a->tg[TG_ITALIC] = strchr(v, 'i') != NULL;
            a->tg[TG_UNDER] = strchr(v, 'u') != NULL;
            a->tg[TG_STRIKE] = strchr(v, 's') != NULL;
            a->tg[TG_BOX] = strchr(v, 'x') != NULL;
        } else if (!strcmp(k, "layers"))
            a->show_layers = atoi(v) != 0;
        else if (!strcmp(k, "rulers"))
            a->rulers = atoi(v) != 0;
        else if (!strcmp(k, "snap"))
            a->snap = atoi(v) != 0;
        else if (!strcmp(k, "ruler-unit")) {
            for (int i = 0; i < RU_COUNT; i++)
                if (!strcmp(v, RU_ABBR[i]))
                    a->ruler_unit = i;
        } else if (!strcmp(k, "ruler-dpi")) {
            int dv = atoi(v);
            a->ruler_dpi = LM_CLAMP(dv, 1, 2400);
        }
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
    fprintf(f, "rulers = %d\nsnap = %d\nruler-unit = %s\nruler-dpi = %d\n", a->rulers ? 1 : 0, a->snap ? 1 : 0, RU_ABBR[a->ruler_unit], a->ruler_dpi);
    fprintf(f, "smooth = %d\nbrush-style = %s%s\n", a->val[S_SMOOTH], a->tg[TG_PIXEL] ? "p" : "", a->tg[TG_MIRROR] ? "m" : "");
    fprintf(f, "text-font = %s\ntext-align = %d\ntext-line = %d\ntext-letters = %d\ntext-outline = %d\ntext-shadow = %d\ntext-layer = %d\ntext-bend = %d\ntext-style = %s%s%s%s%s\n", a->tfont_name, a->talign, a->val[S_LINE],
            a->val[S_LETTER], a->val[S_OUTLINE], a->val[S_SHADOW], a->tg[TG_TLAYER] ? 1 : 0, a->val[S_BEND], a->tg[TG_BOLD] ? "b" : "", a->tg[TG_ITALIC] ? "i" : "", a->tg[TG_UNDER] ? "u" : "", a->tg[TG_STRIKE] ? "s" : "",
            a->tg[TG_BOX] ? "x" : "");
    fclose(f);
}

/* ---- main ------------------------------------------------------------------------------------------------------------------------ */

/* mellagui-paint --animate OUT.gif|OUT.webp FRAME... [--delay MS] [--loops N] [--bg] [--pingpong]: the pictures are the frames, no window. */
static int animate_cmd(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: mellagui-paint --animate OUT.gif|OUT.webp FRAME... [--delay MS] [--loops N] [--bg] [--pingpong]\n");
        return 2;
    }
    const char *out = argv[0];
    int delay = 100, loops = 0, nfiles = 0;
    bool bg = false, ping = false;
    const char *files[MAX_LAYERS];
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--delay") && i + 1 < argc) {
            int v = atoi(argv[++i]);                                    /* (LM_CLAMP uses its argument more than once) */
            delay = LM_CLAMP(v, 10, 60000);
        } else if (!strcmp(argv[i], "--loops") && i + 1 < argc) {
            int v = atoi(argv[++i]);
            loops = LM_CLAMP(v, 0, 65535);
        } else if (!strcmp(argv[i], "--bg"))
            bg = true;
        else if (!strcmp(argv[i], "--pingpong"))
            ping = true;
        else if (nfiles < MAX_LAYERS)
            files[nfiles++] = argv[i];
    }
    if (!anim_name(out)) {
        fprintf(stderr, "mellagui-paint: the animation is written as .gif or .webp\n");
        return 2;
    }
    struct doc d;
    memset(&d, 0, sizeof(d));
    char err[160] = "";
    for (int i = 0; i < nfiles; i++) {
        struct pix im = { 0 };
        if (!load_file(files[i], &im, err, sizeof(err))) {
            fprintf(stderr, "mellagui-paint: %s: %s\n", files[i], err);
            doc_free(&d);
            return 1;
        }
        if (i == 0) {
            doc_from_pix(&d, &im);
            d.paper = 0;
        } else {
            bool same = im.w == doc_w(&d) && im.h == doc_h(&d);
            if (!same || !doc_add(&d, files[i], 0)) {
                fprintf(stderr, "mellagui-paint: %s: %s\n", files[i], same ? "out of memory" : "the frames must all have the same size");
                pix_free(&im);
                doc_free(&d);
                return 1;
            }
            memcpy(IMD(&d).p, im.p, (size_t)im.w * (size_t)im.h * 4);
            pix_free(&im);
        }
        const char *bn = strrchr(files[i], '/');
        snprintf(d.L[d.cur].name, sizeof(d.L[d.cur].name), "%.39s", bn ? bn + 1 : files[i]);
    }
    int nf = 0;
    bool ok = write_anim(&d, out, delay, loops, bg, ping, &nf, err, sizeof(err));
    doc_free(&d);
    if (!ok) {
        fprintf(stderr, "mellagui-paint: %s\n", err);
        return 1;
    }
    printf("%s: %d frames\n", out, nf);
    return 0;
}

int main(int argc, char **argv)
{
    wlwin_hidpi();                                                      /* draws through the wlwin_* calls only: sharp on a scaled screen */
    lm_theme_load_env();
    signal(SIGPIPE, SIG_IGN);
    if (argc > 1 && !strcmp(argv[1], "--selftest"))
        return selftest();
    if (argc > 1 && !strcmp(argv[1], "--apply"))
        return apply_cmd(argc - 2, argv + 2);
    if (argc > 1 && !strcmp(argv[1], "--animate"))
        return animate_cmd(argc - 2, argv + 2);
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
    a.grp[G_SELECT] = T_SELECT;
    a.grp[G_BRUSH] = T_PEN;
    a.grp[G_RETOUCH] = T_CLONE;
    a.grp[G_SHAPES] = T_LINE;
    a.grp[G_MOVE] = T_MOVE;
    a.tool = T_PEN;
    a.sl_drag = -1;
    a.txt_edit = -1;
    a.mask_edit = -1;
    a.ruler_dpi = 96;
    a.tg[TG_TLAYER] = true;
    a.guide_drag = -1;
    a.adj_drag = -1;
    a.font_grab = a.lay_grab = -1;
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
    pix_free(&a.finger);
    sel_clear(&a.fx_sel0);
    sel_clear(&a.sel_base);
    if (a.fx_snap_ok)
        snap_free(&a.fx_snap);
    sel_clear(&a.sel);
    free(a.lx_pts);
    free(a.ly_pts);
    free(a.vbuf);
    wlwin_destroy(&a.win);
    return 0;
}
