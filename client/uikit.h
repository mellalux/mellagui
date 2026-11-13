#ifndef MELLAGUI_UIKIT_H
#define MELLAGUI_UIKIT_H

/*
 * Small drawing helpers shared by the software drawn clients (header only, no state):
 *   - lm_round_fill: a rounded box (pill shaped buttons) with anti-aliased corners, blended over what is there;
 *   - the font zoom the terminal and the editor share: the list of sizes (LM_ZOOM_LEVELS, percent), the next / previous size
 *     (lm_zoom_step), the weights of the 8x16 font pixels under every pixel of a zoomed cell (lm_zoom_build) and one glyph
 *     drawn with them (lm_zoom_glyph: whole multiples of 100 % are crisp, other sizes are area averaged, the glyph is
 *     blended over the background that is already in the buffer);
 *   - lm_zoom_conf_load / _save: the `zoom=PERCENT` file ($XDG_CONFIG_HOME/mellagui/NAME.conf) both programs keep.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "font.h"
#include "metrics.h"
#include "util.h"

static inline uint32_t lm_mix_rgb(uint32_t a, uint32_t b, double t)       /* t = 0: a, t = 1: b */
{
    uint32_t out = 0xff000000u;
    for (int sh = 0; sh <= 16; sh += 8) {
        double ca = (a >> sh) & 0xff, cb = (b >> sh) & 0xff;
        out |= (uint32_t)(ca + (cb - ca) * t + 0.5) << sh;
    }
    return out;
}

static inline void lm_fill_px(uint32_t *px, int stride, int bw, int bh, int x0, int y0, int w, int h, uint32_t rgb)
{
    for (int y = y0 < 0 ? 0 : y0; y < y0 + h && y < bh; y++)
        for (int x = x0 < 0 ? 0 : x0; x < x0 + w && x < bw; x++)
            px[(size_t)y * (size_t)stride + (size_t)x] = 0xff000000u | rgb;
}

/* The corner radius of a coloured button: `pill` is the radius of the full pill / circle (half the height), scaled by [windows] button-roundness (100 = the
 * pill, 0 = square corners). The dock's and the launcher's icons follow the same setting. */
static inline int lm_button_radius(int pill)
{
    return pill * lm_metrics.button_roundness / 100;
}

/* A box with rounded corners of radius r (r >= min(w, h) / 2 makes a pill). */
static inline void lm_round_fill(uint32_t *px, int stride, int bw, int bh, int x, int y, int w, int h, int r, uint32_t rgb)
{
    r = LM_MIN(r, LM_MIN(w, h) / 2);
    lm_fill_px(px, stride, bw, bh, x, y + r, w, h - 2 * r, rgb);
    lm_fill_px(px, stride, bw, bh, x + r, y, w - 2 * r, r, rgb);
    lm_fill_px(px, stride, bw, bh, x + r, y + h - r, w - 2 * r, r, rgb);
    for (int cy = 0; cy < r; cy++)
        for (int cx = 0; cx < r; cx++) {
            double dx = r - cx - 0.5, dy = r - cy - 0.5;                /* the pixel centre from the corner centre */
            double cov = (double)r + 0.5 - sqrt(dx * dx + dy * dy);
            if (cov <= 0.0)
                continue;
            if (cov > 1.0)
                cov = 1.0;
            int xs[2] = { x + cx, x + w - 1 - cx }, ys[2] = { y + cy, y + h - 1 - cy };
            for (int i = 0; i < 2; i++)
                for (int j = 0; j < 2; j++) {
                    if (xs[i] < 0 || ys[j] < 0 || xs[i] >= bw || ys[j] >= bh)
                        continue;
                    uint32_t *p = &px[(size_t)ys[j] * (size_t)stride + (size_t)xs[i]];
                    *p = lm_mix_rgb(*p, rgb, cov);
                }
        }
}

/* The same on a premultiplied ARGB buffer whose corners are transparent (a card with rounded corners): the anti-aliased edge pixels get
 * the coverage as their alpha instead of being mixed with what is below. */
static inline void lm_round_fill_argb(uint32_t *px, int stride, int bw, int bh, int x, int y, int w, int h, int r, uint32_t rgb)
{
    r = LM_MIN(r, LM_MIN(w, h) / 2);
    lm_fill_px(px, stride, bw, bh, x, y + r, w, h - 2 * r, rgb);
    lm_fill_px(px, stride, bw, bh, x + r, y, w - 2 * r, r, rgb);
    lm_fill_px(px, stride, bw, bh, x + r, y + h - r, w - 2 * r, r, rgb);
    for (int cy = 0; cy < r; cy++)
        for (int cx = 0; cx < r; cx++) {
            double dx = r - cx - 0.5, dy = r - cy - 0.5;
            double cov = (double)r + 0.5 - sqrt(dx * dx + dy * dy);
            if (cov <= 0.0)
                continue;
            if (cov > 1.0)
                cov = 1.0;
            int xs[2] = { x + cx, x + w - 1 - cx }, ys[2] = { y + cy, y + h - 1 - cy };
            for (int i = 0; i < 2; i++)
                for (int j = 0; j < 2; j++) {
                    if (xs[i] < 0 || ys[j] < 0 || xs[i] >= bw || ys[j] >= bh)
                        continue;
                    uint32_t *p = &px[(size_t)ys[j] * (size_t)stride + (size_t)xs[i]];
                    uint32_t d = *p, da = d >> 24;
                    double oa = cov + ((double)da / 255.0) * (1.0 - cov);
                    uint32_t out = (uint32_t)(oa * 255.0 + 0.5) << 24;
                    for (int sh = 0; sh <= 16; sh += 8) {                  /* premultiplied: new colour * its coverage + what was there * the rest */
                        double c = (double)((rgb >> sh) & 255) * cov + (double)((d >> sh) & 255) * (1.0 - cov);
                        out |= (uint32_t)(c > 255.0 ? 255.0 : c + 0.5) << sh;
                    }
                    *p = out;
                }
        }
}

/* ---- font zoom ------------------------------------------------------------------------------------------------------------ */

static const int LM_ZOOM_LEVELS[] = { 50, 60, 75, 90, 100, 110, 125, 150, 175, 200, 250, 300, 400, 500 };
#define LM_N_ZOOM ((int)(sizeof(LM_ZOOM_LEVELS) / sizeof(LM_ZOOM_LEVELS[0])))
#define LM_ZOOM_DEFAULT 100

/* delta +1 / -1: the next level up / down from `cur`; 0: back to 100 %. */
static inline int lm_zoom_step(int cur, int delta)
{
    if (delta == 0)
        return LM_ZOOM_DEFAULT;
    int i = 0;
    while (i < LM_N_ZOOM - 1 && LM_ZOOM_LEVELS[i] < cur)
        i++;                                                    /* the first level that is >= the current size */
    if (delta > 0 && LM_ZOOM_LEVELS[i] <= cur)
        i++;
    else if (delta < 0)
        i--;
    return LM_ZOOM_LEVELS[LM_CLAMP(i, 0, LM_N_ZOOM - 1)];
}

struct lm_zt {
    uint8_t c0, n, w[4];
};

struct lm_zoomtab {
    int pct, cw, ch;                                            /* size in percent and the cell size in pixels */
    struct lm_zt xt[96], yt[192];
};

/* Which source pixels of the 8x16 glyph each destination pixel covers, in units of 1/dst of a source pixel. */
static inline void lm_zoom_axis(struct lm_zt *t, int dst, int src)
{
    for (int d = 0; d < dst; d++) {
        int lo = src * d, hi = src * d + src;
        t[d].n = 0;
        for (int c = lo / dst; c < src && c * dst < hi && t[d].n < 4; c++) {
            int ov = LM_MIN(hi, (c + 1) * dst) - LM_MAX(lo, c * dst);
            if (ov <= 0)
                continue;
            if (t[d].n == 0)
                t[d].c0 = (uint8_t)c;
            t[d].w[t[d].n++] = (uint8_t)ov;
        }
    }
}

static inline void lm_zoom_build(struct lm_zoomtab *z, int pct)
{
    z->pct = pct;
    z->cw = LM_MAX((pct * LM_FONT_W + 50) / 100, 1);
    z->ch = LM_MAX((pct * LM_FONT_H + 50) / 100, 1);
    lm_zoom_axis(z->xt, LM_MIN(z->cw, 96), LM_FONT_W);
    lm_zoom_axis(z->yt, LM_MIN(z->ch, 192), LM_FONT_H);
}

/* One glyph with its top left corner at (x, y), blended over the pixels already there. */
static inline void lm_zoom_glyph(uint32_t *px, int stride, int bw, int bh, const struct lm_zoomtab *z, int x, int y,
                                 uint32_t cp, uint32_t fg)
{
    const uint8_t *g = lm_font_cell(cp, z->cw, z->ch);          /* drawn at the cell's own size, whatever the zoom */
    for (int r = 0; r < z->ch; r++) {
        int py = y + r;
        if (py < 0 || py >= bh)
            continue;
        for (int c = 0; c < z->cw; c++) {
            int pxx = x + c;
            unsigned a = g[r * z->cw + c];
            if (!a || pxx < 0 || pxx >= bw)
                continue;
            uint32_t *p = &px[(size_t)py * (size_t)stride + (size_t)pxx];
            *p = lm_font_mix(*p, 0xff000000u | fg, a);
        }
    }
}

/* ---- the zoom file ------------------------------------------------------------------------------------------------------- */

static inline bool lm_zoom_conf_path(const char *name, char *out, size_t n)
{
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    int w;
    if (xdg && *xdg)
        w = snprintf(out, n, "%s/mellagui/%s.conf", xdg, name);
    else if (home && *home)
        w = snprintf(out, n, "%s/.config/mellagui/%s.conf", home, name);
    else
        return false;
    return w > 0 && (size_t)w < n;
}

/* The saved zoom, or 0 when there is none (or it is out of range). */
static inline int lm_zoom_conf_load(const char *name)
{
    char path[1024], line[128];
    int zoom = 0;
    if (!lm_zoom_conf_path(name, path, sizeof(path)))
        return 0;
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    while (fgets(line, sizeof(line), f)) {
        int v;
        if (sscanf(line, "zoom=%d", &v) == 1 && v >= LM_ZOOM_LEVELS[0] && v <= LM_ZOOM_LEVELS[LM_N_ZOOM - 1])
            zoom = v;
    }
    fclose(f);
    return zoom;
}

static inline void lm_zoom_conf_save(const char *name, int zoom)
{
    char path[1024], tmp[1100];
    if (!lm_zoom_conf_path(name, path, sizeof(path)))
        return;
    snprintf(tmp, sizeof(tmp), "%s.%d.tmp", path, (int)getpid());
    for (char *p = path + 1; *p; p++) {                         /* parent directories */
        if (*p != '/')
            continue;
        *p = 0;
        mkdir(path, 0755);
        *p = '/';
    }
    FILE *f = fopen(tmp, "w");
    if (!f)
        return;
    fprintf(f, "# mellagui-%s: written by the program itself\nzoom=%d\n", name, zoom);
    if (fclose(f) != 0 || rename(tmp, path) < 0)
        unlink(tmp);
}

#endif
