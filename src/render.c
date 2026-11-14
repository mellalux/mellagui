/*
 * Software compositor. See render.h for the contract.
 *
 * Per output we keep the damage accumulated since the last presented frame
 * plus the damage of the last HIST frames; together with the framebuffer age
 * reported by the backend that tells us exactly what to redraw.
 */
#ifdef __SSE2__
#include <emmintrin.h>
#endif
#include "render.h"
#include "flag.h"
#include "theme.h"
#include "mixer.h"
#include "sysinfo.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "dockicon.h"
#include "font.h"
#include "smarthome.h"
#include "gl.h"
#include "img.h"
#include "log.h"
#include "lock.h"
#include "output.h"
#include "seat.h"
#include "surface.h"
#include "wm.h"
#include <stdio.h>
#include <linux/input-event-codes.h>

#define HIST 3
#define MAX_REDRAW_RECTS 12

/* ---- colour scheme (XRGB8888) --------------------------------------------------- */

#define COL_WALL_TOP (lm_theme.wallpaper_top)
#define COL_WALL_BOTTOM (lm_theme.wallpaper_bottom)
#define COL_FRAME_ACTIVE (lm_theme.frame)
#define COL_FRAME_IDLE (lm_theme.frame_idle)
#define COL_CONTENT_BG (lm_theme.content)
#define COL_TEXT_ACTIVE (lm_theme.title_text)
#define COL_TEXT_IDLE (lm_theme.title_text_idle)
#define COL_BTN_HOVER (lm_theme.title_button_hover)
#define COL_BTN_PRESSED (lm_theme.title_button_pressed)
#define COL_BTN_CLOSE (lm_theme.title_button_close)
#define COL_PANEL (lm_theme.background)
#define COL_PANEL_BTN (lm_theme.button)
#define COL_PANEL_HOVER (lm_theme.button_hover)
#define COL_PANEL_ACCENT (lm_theme.accent)
#define COL_PANEL_TEXT (lm_theme.text)
#define COL_PANEL_DANGER (lm_theme.danger)
#define COL_PANEL_DIM (lm_theme.text_dim)

#define ALPHA(a, rgb) ((((uint32_t)(a) & 0xffu) << 24) | ((rgb) & 0x00ffffffu))     /* an alpha byte and an 0xRRGGBB colour */

struct out_state {
    struct lm_output *out;
    struct lm_region pending;           /* global coordinates */
    struct lm_region hist[HIST];        /* damage of the last HIST presented frames */
    bool ready;                         /* backend allows drawing */
    bool presented;                     /* a frame was presented, frame-done not yet sent */
    bool full;                          /* next frame must repaint everything */
    void *gl;                           /* struct gl_out: the GL renderer's per-output state (render_gl.inc) */
    uint32_t *wall_px;                  /* the wallpaper scaled for this output (wall_rect of it), built on demand */
    struct lm_rect wall_rect;           /* output-local */
    uint32_t wall_gen;
    int wall_w, wall_h;
    uint32_t *lock_px;                  /* the wallpaper blurred and dimmed for the lock screen: lock_w x lock_h, a small picture that is scaled up */
    int lock_w, lock_h;
    uint32_t lock_gen, lock_ink;        /* what it was made from: the wallpaper's generation and the dimming colour */
    int lock_pw, lock_ph;               /* the output's physical size it was made for */
    uint32_t *mag_px;                   /* the scene before it is magnified (software renderer), output sized */
    int mag_w, mag_h;
};

struct wallpaper {
    struct lm_image img;
    int mode;                           /* 0 fill, 1 fit, 2 stretch, 3 center */
    uint32_t gen;                       /* bumped on every change: the per-output copies are rebuilt */
    char status[160];                   /* "", "ok", "error: ..." */
};

/* Screenshot feedback: a white flash over the captured area (150 ms) and a card with a thumbnail and a label in the
 * bottom right corner of the output (MELLAGUI_TOAST_MS, default 8 s). Painted above the scene, below the cursor. */
#define SHOT_THUMB_W 240
#define SHOT_THUMB_H 150
#define SHOT_FLASH_TICKS 3
#define SHOT_TICK_MS 50
struct shot_fx {
    bool active;
    struct lm_output *out;
    struct lm_rect area;                /* what was captured (flashed), global */
    uint32_t *thumb;
    int tw, th;
    char label[96];
    uint32_t gen;                       /* changes with every new card (the GL renderer repaints its texture then) */
    char path[512];                     /* the saved file ("" = clipboard only): a click on the card opens it */
    int ticks;
    int hold_ms;
    struct lm_timer *timer;
};

struct lm_renderer {
    struct shot_fx shot;
    struct wallpaper wall;
    struct lm_gl *gl;                   /* the GL renderer; NULL = software (render_gl.inc) */
    bool gl_owned;                      /* we made it (headless); the DRM backend owns its own */
    unsigned sprite_tex[LM_CURSOR_POINTER + 1];
    int sprite_k[LM_CURSOR_POINTER + 1];        /* ... and the magnification each was made for */
    struct lm_server *srv;
    struct lm_idle *idle;
    struct lm_timer *frame_timer;
    struct lm_rect cursor_rect;         /* last damaged cursor rectangle (global) */
    const struct lm_output *mag_out;    /* the output that was magnified last (the pointer was on it) */
};

struct paint_ctx {
    struct lm_renderer *r;
    struct lm_output *out;
    struct lm_framebuffer *fb;
    double s;                           /* the output's scale (1.0 = one logical pixel is one pixel of the framebuffer) */
    int ox, oy;                         /* the global position of framebuffer pixel (0, 0): out->x / out->y; 0 in a physical view */
    int fbx, fby;                       /* where framebuffer pixel (0, 0) lies in the output's physical pixels (a scratch picture of a part of the output) */
    struct lm_rect clip;                /* global, already clipped to the output */
    int idx;                            /* scene items seen so far */
    const struct lm_surface *skip_until; /* the items below this opaque cover are not painted (NULL: paint all) */
    int cov_idx;                        /* occlusion search: the scene item that covers `cov_rect` opaquely, -1 none */
    const struct lm_surface *cov_root;  /* ... and its surface: the second pass finds it by identity, because it walks a smaller clip
                                         * and so sees fewer items than the first pass (an index would point at the wrong one) */
    struct lm_rect cov_rect;
    int64_t cov_area;
    bool snapshot;                      /* painting for lm_render_snapshot: `out` is a stand-in, scene items of any output are painted */
};

/* ---- cursor sprites --------------------------------------------------------------- */

#define SPR_W 17
#define SPR_H 19

struct sprite {
    uint32_t px[SPR_W * SPR_H];         /* ARGB premultiplied; 0 = transparent */
    int hot_x, hot_y;
    bool built;
};

static struct sprite sprites[LM_CURSOR_POINTER + 1];

static const char *arrow_art[] = {
    "X           ",
    "XX          ",
    "X.X         ",
    "X..X        ",
    "X...X       ",
    "X....X      ",
    "X.....X     ",
    "X......X    ",
    "X.......X   ",
    "X........X  ",
    "X.........X ",
    "X......XXXXX",
    "X...X..X    ",
    "X..X X..X   ",
    "X.X  X..X   ",
    "XX    X..X  ",
    "X     X..X  ",
    "       X..X ",
    "       XXX  ",
};

static void spr_outline(struct sprite *s)
{
    /* White pixels are 1, outline is derived: any empty neighbour of a white pixel turns black. */
    uint32_t tmp[SPR_W * SPR_H];
    memcpy(tmp, s->px, sizeof(tmp));
    for (int y = 0; y < SPR_H; y++)
        for (int x = 0; x < SPR_W; x++) {
            if (tmp[y * SPR_W + x] != 0xffffffffu)
                continue;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= SPR_W || ny >= SPR_H)
                        continue;
                    if (s->px[ny * SPR_W + nx] == 0)
                        s->px[ny * SPR_W + nx] = 0xff000000u;
                }
        }
}

static void spr_put(struct sprite *s, int x, int y)
{
    if (x >= 0 && y >= 0 && x < SPR_W && y < SPR_H)
        s->px[y * SPR_W + x] = 0xffffffffu;
}

static void spr_double_arrow(struct sprite *s, int dx, int dy)
{
    int cx = SPR_W / 2, cy = SPR_H / 2;
    int px = -dy, py = dx; /* perpendicular */
    for (int t = -6; t <= 6; t++) {
        spr_put(s, cx + dx * t, cy + dy * t);
        if (dx == 0 || dy == 0) {
            /* axis aligned lines are one pixel wide, make them 2 wide for visibility */
        }
    }
    for (int end = -1; end <= 1; end += 2)
        for (int k = 0; k <= 3; k++)
            for (int w = -k; w <= k; w++)
                spr_put(s, cx + dx * (end * (6 - k)) + px * w, cy + dy * (end * (6 - k)) + py * w);
}

static void sprite_build(enum lm_cursor_shape shape)
{
    struct sprite *s = &sprites[shape];
    if (s->built)
        return;
    memset(s->px, 0, sizeof(s->px));
    s->hot_x = s->hot_y = 0;
    switch (shape) {
    case LM_CURSOR_RESIZE_N: case LM_CURSOR_RESIZE_S:
        spr_double_arrow(s, 0, 1); s->hot_x = SPR_W / 2; s->hot_y = SPR_H / 2; break;
    case LM_CURSOR_RESIZE_W: case LM_CURSOR_RESIZE_E:
        spr_double_arrow(s, 1, 0); s->hot_x = SPR_W / 2; s->hot_y = SPR_H / 2; break;
    case LM_CURSOR_RESIZE_NW: case LM_CURSOR_RESIZE_SE:
        spr_double_arrow(s, 1, 1); s->hot_x = SPR_W / 2; s->hot_y = SPR_H / 2; break;
    case LM_CURSOR_RESIZE_NE: case LM_CURSOR_RESIZE_SW:
        spr_double_arrow(s, 1, -1); s->hot_x = SPR_W / 2; s->hot_y = SPR_H / 2; break;
    case LM_CURSOR_MOVE:
        spr_double_arrow(s, 0, 1);
        spr_double_arrow(s, 1, 0);
        s->hot_x = SPR_W / 2; s->hot_y = SPR_H / 2; break;
    default:
        for (size_t y = 0; y < LM_ARRAY_LEN(arrow_art); y++)
            for (size_t x = 0; arrow_art[y][x]; x++) {
                char c = arrow_art[y][x];
                if (c == 'X')
                    s->px[y * SPR_W + x] = 0xff000000u;
                else if (c == '.')
                    s->px[y * SPR_W + x] = 0xffffffffu;
            }
        s->built = true;
        return;
    }
    spr_outline(s);
    s->built = true;
}

/* ---- pixel helpers --------------------------------------------------------------------- */

static inline uint32_t div255(uint32_t x)
{
    return ((x + 128) * 257) >> 16;
}

/* A premultiplied ARGB pixel at `alpha` / 255 of its strength. */
static inline uint32_t fade_px(uint32_t p, int alpha)
{
    uint32_t a = (uint32_t)alpha;
    return (((p >> 24) * a / 255) << 24) | ((((p >> 16) & 255) * a / 255) << 16) | ((((p >> 8) & 255) * a / 255) << 8) | ((p & 255) * a / 255);
}

static inline uint32_t blend_px(uint32_t src, uint32_t dst)
{
    uint32_t a = src >> 24;
    if (a == 255)
        return src & 0x00ffffffu;
    if (a == 0)
        return dst;
    uint32_t da = 255 - a;
    uint32_t r = ((src >> 16) & 255) + div255(((dst >> 16) & 255) * da);
    uint32_t g = ((src >> 8) & 255) + div255(((dst >> 8) & 255) * da);
    uint32_t b = (src & 255) + div255((dst & 255) * da);
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return (r << 16) | (g << 8) | b;
}

static inline uint32_t *fb_row(const struct paint_ctx *c, int gy)
{
    return (uint32_t *)((uint8_t *)c->fb->pixels +
                        (size_t)(gy - c->oy) * (size_t)c->fb->stride) - c->ox;
}

/* ---- scale ---------------------------------------------------------------------------------------------------------------
 * Painting code works in LOGICAL coordinates (global, as the wm has them: out->x / out->y .. + out->width / height). The
 * framebuffer is PHYSICAL: `scale` times as many pixels. The primitives convert. prect() maps a logical rectangle to
 * physical pixels relative to the output's corner; BOTH edges are rounded, so rectangles that touch still touch and a
 * damage rectangle covers exactly the pixels its painting touched. pview() makes a context that addresses the framebuffer
 * in those physical pixels (origin 0, clip converted): the *_px functions are the pixel loops and work in it.
 * With scale 1 everything is the identity and the pictures are those of before the scale existed.
 */
static inline void ctx_init(struct paint_ctx *c)
{
    c->s = lm_output_scalef(c->out);
    c->ox = c->out->x;
    c->oy = c->out->y;
}

static inline int sround(double v)
{
    return (int)lround(v);
}

static inline struct lm_rect prect_at(int ox, int oy, double s, const struct lm_rect *r)
{
    if (s == 1.0)
        return lm_rect_make(r->x - ox, r->y - oy, r->w, r->h);
    int x0 = sround((double)(r->x - ox) * s), y0 = sround((double)(r->y - oy) * s);
    int x1 = sround((double)(r->x + r->w - ox) * s), y1 = sround((double)(r->y + r->h - oy) * s);
    return lm_rect_make(x0, y0, x1 - x0, y1 - y0);
}

static inline struct lm_rect prect(const struct paint_ctx *c, const struct lm_rect *r)
{
    return prect_at(c->ox, c->oy, c->s, r);
}

static inline void pview(const struct paint_ctx *c, struct paint_ctx *pc)
{
    *pc = *c;
    pc->clip = prect(c, &c->clip);
    pc->ox = c->fbx;                                    /* physical pixel (x, y) of the output is framebuffer pixel (x - fbx, y - fby) */
    pc->oy = c->fby;
    pc->s = 1.0;
    pc->fbx = pc->fby = 0;
}

/* A length (radius, thickness) in physical pixels; never 0 for something that is there. */
static inline int plen(const struct paint_ctx *c, int v)
{
    return v <= 0 ? 0 : c->s == 1.0 ? v : LM_MAX(sround((double)v * c->s), 1);
}

static void blit_scaled_px(const struct paint_ctx *pc, const uint32_t *px, int stride_px, int tw, int th, const struct lm_rect *src,
                           const struct lm_rect *pdst, int alpha, bool opaque);
static void blit_tex(const struct paint_ctx *c, const uint32_t *px, int stride_px, int w, int h, const struct lm_rect *dst, int alpha, bool opaque);

/* The physical pixels of a logical rectangle of an output (what the backend is told is damaged). */
static inline struct lm_rect out_prect(const struct lm_output *o, const struct lm_rect *r)
{
    return prect_at(o->x, o->y, lm_output_scalef(o), r);
}

/* Fill n pixels with one value (SSE2 stores when available). */
static inline void fill32(uint32_t *p, uint32_t v, size_t n)
{
    size_t i = 0;
#ifdef __SSE2__
    __m128i vv = _mm_set1_epi32((int)v);
    for (; i + 8 <= n; i += 8) {
        _mm_storeu_si128((__m128i *)(void *)(p + i), vv);
        _mm_storeu_si128((__m128i *)(void *)(p + i + 4), vv);
    }
    for (; i + 4 <= n; i += 4)
        _mm_storeu_si128((__m128i *)(void *)(p + i), vv);
#endif
    for (; i < n; i++)
        p[i] = v;
}

static void fill_rect_px(const struct paint_ctx *c, const struct lm_rect *rect, uint32_t color)
{
    struct lm_rect r;
    if (!lm_rect_intersect(rect, &c->clip, &r))
        return;
    for (int y = r.y; y < r.y + r.h; y++)
        fill32(fb_row(c, y) + r.x, color, (size_t)r.w);
}

static void fill_rect(const struct paint_ctx *c, const struct lm_rect *rect, uint32_t color)
{
    struct paint_ctx pc;
    pview(c, &pc);
    struct lm_rect pr = prect(c, rect);
    fill_rect_px(&pc, &pr, color);
}

static inline void put_px(const struct paint_ctx *c, int x, int y, uint32_t color)
{
    if (x < c->clip.x || y < c->clip.y || x >= c->clip.x + c->clip.w || y >= c->clip.y + c->clip.h)
        return;
    fb_row(c, y)[x] = color;
}

/* One character of a label, its pen at (x, y = top of the line) in logical pixels, cut to `lim`; returns how far the pen moves
 * (logical). The picture is rasterised at the output's scale: the shell's text is as sharp at 2x as at 1x. */
static int draw_label_glyph(const struct paint_ctx *c, const struct lm_rect *lim, int x, int y, uint32_t cp, int scale, uint32_t color)
{
    int sc = scale < 0 ? -scale : scale;
    bool bold = scale < 0;
    struct lm_font_glyph g;
    if (c->s == 1.0)
        lm_font_label_styled(cp, sc, bold, &g);
    else
        lm_font_label_scaled(cp, (double)sc * c->s, bold, &g);
    int advance = lm_font_advance_styled(cp, bold) * sc;
    struct lm_rect area, pl;
    if (!lm_rect_intersect(lim, &c->clip, &area))
        return advance;
    pl = prect(c, &area);
    int px = sround((double)(x - c->ox) * c->s), py = sround((double)(y - c->oy) * c->s);
    int gx = px - g.left;
    if (gx + g.w <= pl.x || gx >= pl.x + pl.w)
        return advance;
    int x0 = LM_MAX(gx, pl.x), x1 = LM_MIN(gx + g.w, pl.x + pl.w);
    struct paint_ctx pc;
    pview(c, &pc);
    for (int row = 0; row < g.h; row++) {
        int ry = py + row;
        if (ry < pl.y || ry >= pl.y + pl.h)
            continue;
        uint32_t *dst = fb_row(&pc, ry);
        const uint8_t *src = g.px + (size_t)row * (size_t)g.w - gx;
        for (int xx = x0; xx < x1; xx++) {
            unsigned a = src[xx];
            if (a)
                dst[xx] = lm_font_mix(dst[xx], color, a);
        }
    }
    return advance;
}

static void draw_text_w(const struct paint_ctx *c, const struct lm_rect *area, int x, int y,
                        const char *text, uint32_t color, bool bold)
{
    struct lm_rect lim;
    if (!lm_rect_intersect(area, &c->clip, &lim))
        return;
    size_t len = strlen(text);
    for (size_t i = 0; i < len;) {
        int used;
        uint32_t cp = lm_utf8_decode(text + i, (int)(len - i), &used);
        i += used > 0 ? (size_t)used : 1;
        x += draw_label_glyph(c, &lim, x, y, cp, bold ? -1 : 1, color);
        if (x >= lim.x + lim.w + 8)
            break;
    }
}

static void draw_text(const struct paint_ctx *c, const struct lm_rect *area, int x, int y,
                      const char *text, uint32_t color)
{
    draw_text_w(c, area, x, y, text, color, false);
}

/* The bold weight (titles of cards); with the bitmap font it is the same picture. */
static void draw_text_bold(const struct paint_ctx *c, const struct lm_rect *area, int x, int y,
                           const char *text, uint32_t color)
{
    draw_text_w(c, area, x, y, text, color, true);
}

static void draw_text_centered(const struct paint_ctx *c, const struct lm_rect *area,
                               const char *text, uint32_t color)
{
    int w = lm_font_text_width(text);
    int x = area->x + (area->w - w) / 2;
    if (x < area->x)
        x = area->x;
    int y = area->y + (area->h - LM_FONT_H) / 2;
    draw_text(c, area, x, y, text, color);
}

/* ---- scene items -------------------------------------------------------------------------- */

static uint32_t lerp_color(uint32_t a, uint32_t b, int num, int den)
{
    if (den <= 0)
        return a;
    uint32_t r = 0;
    for (int sh = 16; sh >= 0; sh -= 8) {
        int ca = (a >> sh) & 255, cb = (b >> sh) & 255;
        r |= (uint32_t)(ca + (cb - ca) * num / den) << sh;
    }
    return r;
}

static struct out_state *state_of(struct lm_output *out);

/* img (iw x ih) region (sx0, sy0, sw x sh) onto dw x dh pixels: a box average when it shrinks, bilinear when it grows. */
static void wall_scale(const struct lm_image *img, double sx0, double sy0, double sw, double sh, uint32_t *dst, int dw, int dh)
{
    double fx = sw / dw, fy = sh / dh;
    int iw = img->w, ih = img->h;
    for (int y = 0; y < dh; y++) {
        for (int x = 0; x < dw; x++) {
            uint32_t out;
            if (fx > 1.0 || fy > 1.0) {
                int x0 = LM_CLAMP((int)floor(sx0 + x * fx), 0, iw - 1), x1 = LM_CLAMP((int)ceil(sx0 + (x + 1) * fx), x0 + 1, iw);
                int y0 = LM_CLAMP((int)floor(sy0 + y * fy), 0, ih - 1), y1 = LM_CLAMP((int)ceil(sy0 + (y + 1) * fy), y0 + 1, ih);
                uint32_t rr = 0, gg = 0, bb = 0, n = 0;
                for (int yy = y0; yy < y1; yy++)
                    for (int xx = x0; xx < x1; xx++) {
                        uint32_t v = img->px[(size_t)yy * (size_t)iw + (size_t)xx];
                        rr += (v >> 16) & 255;
                        gg += (v >> 8) & 255;
                        bb += v & 255;
                        n++;
                    }
                out = ((rr / n) << 16) | ((gg / n) << 8) | (bb / n);
            } else {
                double u = LM_CLAMP(sx0 + (x + 0.5) * fx - 0.5, 0.0, iw - 1.0), v = LM_CLAMP(sy0 + (y + 0.5) * fy - 0.5, 0.0, ih - 1.0);
                int ux = (int)u, vy = (int)v, ux1 = LM_MIN(ux + 1, iw - 1), vy1 = LM_MIN(vy + 1, ih - 1);
                double tx = u - ux, ty = v - vy;
                uint32_t p[4] = { img->px[(size_t)vy * (size_t)iw + (size_t)ux], img->px[(size_t)vy * (size_t)iw + (size_t)ux1],
                                  img->px[(size_t)vy1 * (size_t)iw + (size_t)ux], img->px[(size_t)vy1 * (size_t)iw + (size_t)ux1] };
                double wgt[4] = { (1 - tx) * (1 - ty), tx * (1 - ty), (1 - tx) * ty, tx * ty };
                double acc[3] = { 0, 0, 0 };
                for (int k = 0; k < 4; k++) {
                    acc[0] += wgt[k] * (double)((p[k] >> 16) & 255);
                    acc[1] += wgt[k] * (double)((p[k] >> 8) & 255);
                    acc[2] += wgt[k] * (double)(p[k] & 255);
                }
                out = ((uint32_t)LM_MIN(acc[0] + 0.5, 255.0) << 16) | ((uint32_t)LM_MIN(acc[1] + 0.5, 255.0) << 8) |
                      (uint32_t)LM_MIN(acc[2] + 0.5, 255.0);
            }
            dst[(size_t)y * (size_t)dw + (size_t)x] = out;
        }
    }
}

/* The wallpaper for an output of w x h pixels: the destination rectangle (output-local) and its pixels. */
static void wall_build(struct lm_renderer *r, struct out_state *st)
{
    const struct lm_image *img = &r->wall.img;
    int W = lm_output_pw(st->out), H = lm_output_ph(st->out);      /* the picture is made for the physical pixels: sharp at any scale */
    free(st->wall_px);
    st->wall_px = NULL;
    st->wall_gen = r->wall.gen;
    st->wall_w = W;
    st->wall_h = H;
    if (!img->px || W <= 0 || H <= 0)
        return;
    double iw = img->w, ih = img->h;
    struct lm_rect dest = lm_rect_make(0, 0, W, H);
    double sx0 = 0, sy0 = 0, sw = iw, sh = ih;
    switch (r->wall.mode) {
    case 0: {                                           /* fill: cover the output, crop the overflow */
        double sc = LM_MAX((double)W / iw, (double)H / ih);
        sw = W / sc;
        sh = H / sc;
        sx0 = (iw - sw) / 2;
        sy0 = (ih - sh) / 2;
        break;
    }
    case 1: {                                           /* fit: the whole picture, bars around it */
        double sc = LM_MIN((double)W / iw, (double)H / ih);
        int dw = LM_MAX((int)(iw * sc + 0.5), 1), dh = LM_MAX((int)(ih * sc + 0.5), 1);
        dest = lm_rect_make((W - dw) / 2, (H - dh) / 2, dw, dh);
        break;
    }
    case 3: {                                           /* center: 1:1 */
        int dw = LM_MIN(img->w, W), dh = LM_MIN(img->h, H);
        dest = lm_rect_make((W - dw) / 2, (H - dh) / 2, dw, dh);
        sx0 = (iw - dw) / 2;
        sy0 = (ih - dh) / 2;
        sw = dw;
        sh = dh;
        break;
    }
    default:                                            /* stretch */
        break;
    }
    st->wall_px = malloc((size_t)dest.w * (size_t)dest.h * 4);
    if (!st->wall_px)
        return;
    st->wall_rect = dest;
    wall_scale(img, sx0, sy0, sw, sh, st->wall_px, dest.w, dest.h);
}

static void paint_wallpaper(const struct paint_ctx *c)
{
    struct paint_ctx pc;
    pview(c, &pc);                                      /* physical pixels from here on */
    struct lm_rect r = pc.clip;
    int H = lm_output_ph(c->out);
    for (int y = r.y; y < r.y + r.h; y++) {
        uint32_t col = lerp_color(COL_WALL_TOP, COL_WALL_BOTTOM, y, H);
        fill32(fb_row(&pc, y) + r.x, col, (size_t)r.w);
    }
    struct out_state *st = c->r->wall.img.px ? state_of(c->out) : NULL;
    if (!st)
        return;
    if (!st->wall_px || st->wall_gen != c->r->wall.gen || st->wall_w != lm_output_pw(c->out) || st->wall_h != H)
        wall_build(c->r, st);
    if (!st->wall_px)
        return;
    struct lm_rect img = st->wall_rect, v;
    if (!lm_rect_intersect(&img, &r, &v))
        return;
    for (int y = v.y; y < v.y + v.h; y++)
        memcpy(fb_row(&pc, y) + v.x, st->wall_px + (size_t)(y - img.y) * (size_t)img.w + (size_t)(v.x - img.x), (size_t)v.w * 4);
}

static float cov01(float v);
static float seg_dist(float px, float py, float ax, float ay, float bx, float by);

static void draw_line(const struct paint_ctx *c, int x0, int y0, int x1, int y1, uint32_t color)
{
    if (c->s != 1.0) {                                  /* scaled: an anti-aliased stroke `scale` pixels thick */
        struct paint_ctx pc;
        pview(c, &pc);
        float s = (float)c->s;
        float ax = ((float)(x0 - c->ox) + 0.5f) * s, ay = ((float)(y0 - c->oy) + 0.5f) * s;
        float bx = ((float)(x1 - c->ox) + 0.5f) * s, by = ((float)(y1 - c->oy) + 0.5f) * s;
        struct lm_rect box = lm_rect_make((int)floorf(fminf(ax, bx) - s) - 1, (int)floorf(fminf(ay, by) - s) - 1,
                                          (int)(fabsf(bx - ax) + 2.0f * s) + 3, (int)(fabsf(by - ay) + 2.0f * s) + 3), r;
        if (!lm_rect_intersect(&box, &pc.clip, &r))
            return;
        for (int y = r.y; y < r.y + r.h; y++) {
            uint32_t *row = fb_row(&pc, y);
            for (int x = r.x; x < r.x + r.w; x++) {
                float cov = cov01(s * 0.5f - seg_dist((float)x + 0.5f, (float)y + 0.5f, ax, ay, bx, by) + 0.5f);
                uint32_t pa = (uint32_t)(cov * 255.0f + 0.5f);
                if (!pa)
                    continue;
                uint32_t pr = div255(((color >> 16) & 255) * pa), pg = div255(((color >> 8) & 255) * pa), pb = div255((color & 255) * pa);
                row[x] = blend_px((pa << 24) | (pr << 16) | (pg << 8) | pb, row[x]);
            }
        }
        return;
    }
    struct paint_ctx pc;
    pview(c, &pc);
    x0 -= c->ox;                                        /* scale 1: the physical pixel is the logical one, relative to the output */
    y0 -= c->oy;
    x1 -= c->ox;
    y1 -= c->oy;
    int dx = abs(x1 - x0), dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        put_px(&pc, x0, y0, color);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static uint32_t mix_rgb(uint32_t a, uint32_t b, int t);
static void paint_drop(const struct paint_ctx *c, const struct lm_rect *r, int radius, uint32_t rgb);
static int btn_radius(int r);
static void paint_round_button(const struct paint_ctx *c, float cx, float cy, float R, uint32_t rgb, uint32_t alpha);

/* The title bar is a drop of water floating on a darker frame: the bar and the borders are the dark water (`col`), the title lies in a drop in the frame's
 * own colour and its buttons are small beads of the same water. */
static void frame_bar_pill(const struct lm_frame_info *f, struct lm_rect *pill)
{
    *pill = lm_rect_make(f->titlebar.x + 2, f->titlebar.y + 2, f->titlebar.w - 4, f->titlebar.h - 4);
}

static void draw_frame_button(const struct paint_ctx *c, const struct lm_frame_info *f,
                              const struct lm_rect *b, uint32_t which, uint32_t fg)
{
    uint32_t drop = f->active ? COL_FRAME_ACTIVE : COL_FRAME_IDLE;
    uint32_t base = which == LM_FRAME_HOVER_CLOSE ? lm_theme.btn_red : which == LM_FRAME_HOVER_MAXIMIZE ? lm_theme.btn_green : lm_theme.btn_amber;
    uint32_t lit = which == LM_FRAME_HOVER_CLOSE ? lm_theme.btn_red_lit : which == LM_FRAME_HOVER_MAXIMIZE ? lm_theme.btn_green_lit : lm_theme.btn_amber_lit;
    uint32_t bg = base;                                                          /* close red, maximize green, minimize amber */
    if (f->pressed & which)
        bg = mix_rgb(base, 0x000000u, 70);
    else if (f->hover & which)
        bg = lit;
    if (!f->active)
        bg = mix_rgb(bg, drop, 120);                                             /* a window that is not in front has duller beads */
    struct lm_rect pill;
    frame_bar_pill(f, &pill);
    int sz = LM_MIN(LM_MIN(b->w, b->h), pill.h - 2);
    if (sz >= 8) {
        struct lm_rect bead = lm_rect_make(b->x + (b->w - sz) / 2, b->y + (b->h - sz) / 2, sz, sz);
        paint_drop(c, &bead, btn_radius(sz / 2), bg);
    }
    fg = lm_theme.on_colour;                                                     /* the sign on a coloured bead is white */
    int cx = b->x + b->w / 2, cy = b->y + b->h / 2;
    switch (which) {
    case LM_FRAME_HOVER_CLOSE:
        draw_line(c, cx - 4, cy - 4, cx + 4, cy + 4, fg);
        draw_line(c, cx - 4, cy - 3, cx + 3, cy + 4, fg);
        draw_line(c, cx - 3, cy - 4, cx + 4, cy + 3, fg);
        draw_line(c, cx - 4, cy + 4, cx + 4, cy - 4, fg);
        draw_line(c, cx - 4, cy + 3, cx + 3, cy - 4, fg);
        draw_line(c, cx - 3, cy + 4, cx + 4, cy - 3, fg);
        break;
    case LM_FRAME_HOVER_MAXIMIZE:
        if (f->maximized) {
            draw_line(c, cx - 4, cy - 1, cx + 2, cy - 1, fg);
            draw_line(c, cx - 4, cy + 4, cx + 2, cy + 4, fg);
            draw_line(c, cx - 4, cy - 1, cx - 4, cy + 4, fg);
            draw_line(c, cx + 2, cy - 1, cx + 2, cy + 4, fg);
            draw_line(c, cx - 2, cy - 4, cx + 4, cy - 4, fg);
            draw_line(c, cx + 4, cy - 4, cx + 4, cy + 1, fg);
            draw_line(c, cx - 2, cy - 4, cx - 2, cy - 2, fg);
        } else {
            draw_line(c, cx - 4, cy - 4, cx + 4, cy - 4, fg);
            draw_line(c, cx - 4, cy + 4, cx + 4, cy + 4, fg);
            draw_line(c, cx - 4, cy - 4, cx - 4, cy + 4, fg);
            draw_line(c, cx + 4, cy - 4, cx + 4, cy + 4, fg);
            draw_line(c, cx - 4, cy - 3, cx + 4, cy - 3, fg);
        }
        break;
    case LM_FRAME_HOVER_MINIMIZE:
        draw_line(c, cx - 4, cy + 4, cx + 4, cy + 4, fg);
        draw_line(c, cx - 4, cy + 3, cx + 4, cy + 3, fg);
        break;
    }
}

/* The solid parts of a frame (everything but the title bar, which has the buttons and the title). Shared with the GL renderer. */
struct frame_parts {
    struct lm_rect left, right, bottom, body;
    uint32_t col;                       /* title bar and the three border strips (the dark water) */
    uint32_t drop;                      /* the title's drop and its buttons */
    uint32_t body_col;                  /* behind the client area (shows where the client has not drawn) */
    bool has_body;
};

static void frame_parts(const struct lm_frame_info *f, struct frame_parts *p)
{
    const struct lm_rect *o = &f->outer;
    int bw = LM_BORDER_WIDTH;
    memset(p, 0, sizeof(*p));
    p->drop = f->active ? COL_FRAME_ACTIVE : COL_FRAME_IDLE;
    p->col = mix_rgb(p->drop, 0x000000u, 70);
    p->body_col = COL_CONTENT_BG;
    if (o->h > f->titlebar.h) {
        p->has_body = true;
        p->left = lm_rect_make(o->x, o->y + f->titlebar.h, bw, o->h - f->titlebar.h);
        p->right = lm_rect_make(o->x + o->w - bw, p->left.y, bw, p->left.h);
        p->bottom = lm_rect_make(o->x + bw, o->y + o->h - bw, o->w - 2 * bw, bw);
        p->body = lm_rect_make(o->x + bw, p->left.y, o->w - 2 * bw, p->left.h - bw);
    }
}

static void paint_frame(const struct paint_ctx *c, const struct lm_frame_info *f)
{
    uint32_t fg = f->active ? COL_TEXT_ACTIVE : COL_TEXT_IDLE;
    struct frame_parts fp;
    frame_parts(f, &fp);

    fill_rect(c, &f->titlebar, fp.col);
    struct lm_rect pill;
    frame_bar_pill(f, &pill);
    paint_drop(c, &pill, btn_radius(pill.h / 2), fp.drop);                  /* the title bar itself: a drop of water */
    if (fp.has_body) {
        fill_rect(c, &fp.left, fp.col);
        fill_rect(c, &fp.right, fp.col);
        fill_rect(c, &fp.bottom, fp.col);
        fill_rect(c, &fp.body, fp.body_col);
    }
    draw_frame_button(c, f, &f->btn_min, LM_FRAME_HOVER_MINIMIZE, fg);
    draw_frame_button(c, f, &f->btn_max, LM_FRAME_HOVER_MAXIMIZE, fg);
    draw_frame_button(c, f, &f->btn_close, LM_FRAME_HOVER_CLOSE, fg);
    if (f->title && f->title[0]) {
        int ty = f->title_text.y + (f->title_text.h - LM_FONT_H) / 2;
        draw_text(c, &f->title_text, f->title_text.x + 4, ty, f->title, fg);
    }
}

/* The Alt+Tab list: a bordered box, the selected row highlighted, the workspace of each window on the right. */
static void paint_switcher(const struct paint_ctx *c, const struct lm_switcher_info *s)
{
    fill_rect(c, &s->rect, COL_PANEL_ACCENT);                           /* 1 px border */
    struct lm_rect in = lm_rect_make(s->rect.x + 1, s->rect.y + 1, s->rect.w - 2, s->rect.h - 2);
    fill_rect(c, &in, COL_PANEL);
    for (int i = 0; i < s->n; i++) {
        struct lm_rect r = lm_rect_make(s->rect.x + 4, s->rect.y + 4 + i * s->item_h, s->rect.w - 8, s->item_h);
        if (i == s->sel)
            fill_rect(c, &r, COL_PANEL_HOVER);
        uint32_t col = s->minimized[i] ? COL_PANEL_DIM : i == s->sel ? COL_PANEL_TEXT : lm_theme.task_text;
        int ty = r.y + (r.h - LM_FONT_H) / 2;
        struct lm_rect text = lm_rect_make(r.x + 8, r.y, r.w - 16 - 5 * LM_FONT_W, r.h);
        draw_text(c, &text, text.x, ty, s->label[i], col);
        char tag[16];
        snprintf(tag, sizeof(tag), "%s[%d]", s->minimized[i] ? "_ " : "", s->ws[i] + 1);
        int tw = lm_font_text_width(tag);
        draw_text(c, &r, r.x + r.w - 8 - tw, ty, tag, s->ws[i] == s->current_ws ? COL_PANEL_ACCENT : COL_PANEL_DIM);
    }
}

/* A notification card: bordered box, the title in the accent colour, the wrapped text below, a dim x top right (a click closes it). */
static void paint_notify(const struct paint_ctx *c, const struct lm_notify_info *n)
{
    fill_rect(c, &n->rect, (0x00000000u | lm_theme.border));                                /* 1 px border */
    struct lm_rect in = lm_rect_make(n->rect.x + 1, n->rect.y + 1, n->rect.w - 2, n->rect.h - 2);
    fill_rect(c, &in, COL_PANEL);
    struct lm_rect bar = lm_rect_make(n->rect.x + 1, n->rect.y + 1, 3, n->rect.h - 2);
    fill_rect(c, &bar, COL_PANEL_ACCENT);                               /* the accent stripe on the left */
    int tx = n->rect.x + 12, ty = n->rect.y + 8;
    struct lm_rect tr = lm_rect_make(tx, ty, n->rect.w - 12 - 28, LM_FONT_H);
    draw_text_bold(c, &tr, tx, ty, n->title, COL_PANEL_ACCENT);
    draw_text(c, &n->rect, n->rect.x + n->rect.w - 8 - LM_FONT_W, ty, "x", COL_PANEL_DIM);
    for (int i = 0; i < n->nlines; i++) {
        struct lm_rect lr = lm_rect_make(tx, ty + 22 + i * LM_FONT_H, n->rect.w - 12 - 8, LM_FONT_H);
        draw_text(c, &lr, tx, lr.y, n->line[i], COL_PANEL_TEXT);
    }
}

static void fill_round(const struct paint_ctx *c, const struct lm_rect *rect, int radius, uint32_t argb);

/* Text `scale` times the normal size (the lock screen's clock): real outlines with a TrueType font, blocks with the bitmap. */
static void draw_text_scaled(const struct paint_ctx *c, int x, int y, const char *text, int scale, uint32_t color)
{
    size_t len = strlen(text);
    for (size_t i = 0; i < len;) {
        int used;
        uint32_t cp = lm_utf8_decode(text + i, (int)(len - i), &used);
        i += used > 0 ? (size_t)used : 1;
        x += draw_label_glyph(c, &c->clip, x, y, cp, scale, color);
    }
}

enum shape { SHAPE_DISC, SHAPE_POWER, SHAPE_LOCK, SHAPE_UP, SHAPE_DOWN, SHAPE_CROSS, SHAPE_TRASH, SHAPE_PLUS, SHAPE_LEFT, SHAPE_RIGHT, SHAPE_MOON, SHAPE_RESTART };
static void paint_shape(const struct paint_ctx *c, float cx, float cy, float R, enum shape s, uint32_t rgb, uint32_t alpha);

/* The lock screen: a dark screen with the time, a box with the dots of what was typed and the last message. */
/* The lock screen's backdrop: the wallpaper (cropped to fill) shrunk to about 1/8, blurred with two box passes and dimmed towards the theme's ink, kept per
 * output; it is scaled back up with a bilinear filter, which blurs it further. false = no wallpaper (or no picture): the caller paints plain ink. */
static bool paint_lock_backdrop(const struct paint_ctx *c)
{
    const struct lm_image *img = &c->r->wall.img;
    struct out_state *st = img->px ? state_of(c->out) : NULL;
    if (!st)
        return false;
    int W = lm_output_pw(c->out), H = lm_output_ph(c->out);
    if (W <= 0 || H <= 0)
        return false;
    if (!st->lock_px || st->lock_gen != c->r->wall.gen || st->lock_ink != lm_theme.ink || st->lock_pw != W || st->lock_ph != H) {
        free(st->lock_px);
        int tw = LM_MAX(W / 8, 16), th = LM_MAX(H / 8, 9);
        uint32_t *px = malloc((size_t)tw * (size_t)th * 4), *tmp = malloc((size_t)tw * (size_t)th * 4);
        st->lock_px = NULL;
        if (!px || !tmp) {
            free(px);
            free(tmp);
            return false;
        }
        double iw = img->w, ih = img->h, sc = LM_MAX((double)tw / iw, (double)th / ih), sw = tw / sc, sh = th / sc;
        wall_scale(img, (iw - sw) / 2, (ih - sh) / 2, sw, sh, px, tw, th);
        for (int pass = 0; pass < 2; pass++) {                  /* box blur, radius 2, horizontal then vertical */
            for (int dir = 0; dir < 2; dir++) {
                uint32_t *src = dir ? tmp : px, *dst = dir ? px : tmp;
                int len = dir ? th : tw, lines = dir ? tw : th, step = dir ? tw : 1, lstep = dir ? 1 : tw;
                for (int l = 0; l < lines; l++)
                    for (int i = 0; i < len; i++) {
                        unsigned r = 0, g = 0, b = 0, n = 0;
                        for (int k = -2; k <= 2; k++) {
                            int j = LM_CLAMP(i + k, 0, len - 1);
                            uint32_t p = src[(size_t)l * (size_t)lstep + (size_t)j * (size_t)step];
                            r += (p >> 16) & 255;
                            g += (p >> 8) & 255;
                            b += p & 255;
                            n++;
                        }
                        dst[(size_t)l * (size_t)lstep + (size_t)i * (size_t)step] = 0xff000000u | ((r / n) << 16) | ((g / n) << 8) | (b / n);
                    }
            }
        }
        for (size_t i = 0; i < (size_t)tw * (size_t)th; i++)    /* dimmed: 62 % of the theme's dark colour over it */
            px[i] = 0xff000000u | mix_rgb(px[i] & 0xffffffu, lm_theme.ink, 158);
        free(tmp);
        st->lock_px = px;
        st->lock_w = tw;
        st->lock_h = th;
        st->lock_gen = c->r->wall.gen;
        st->lock_ink = lm_theme.ink;
        st->lock_pw = W;
        st->lock_ph = H;
    }
    struct paint_ctx pc;
    pview(c, &pc);
    struct lm_rect src = lm_rect_make(0, 0, st->lock_w, st->lock_h), dst = lm_rect_make(0, 0, W, H);
    blit_scaled_px(&pc, st->lock_px, st->lock_w, st->lock_w, st->lock_h, &src, &dst, 255, true);
    return true;
}

static void paint_lock(const struct paint_ctx *c, const struct lm_lock_info *li)
{
    struct lm_rect o = lm_output_rect(c->out);
    if (!li->wallpaper || !paint_lock_backdrop(c))
        fill_rect(c, &o, lm_theme.ink);
    int cx = o.x + o.w / 2, cy = o.y + o.h / 2;

    int scale = o.w >= 1600 ? 8 : o.w >= 900 ? 6 : 3;
    int clock_h = LM_FONT_H * scale, top = cy - clock_h - 70;
    int tw = lm_font_text_width(li->clock) * scale;
    draw_text_scaled(c, cx - tw / 2, top, li->clock, scale, COL_PANEL_TEXT);
    struct lm_rect line = lm_rect_make(o.x, top + clock_h + 8, o.w, 20);
    draw_text_centered(c, &line, li->date, COL_PANEL_DIM);

    /* The input: a pill with a soft glow and a two pixel ring in the accent colour (red while blocked), a little padlock at the left and round dots. */
    int bw = LM_MIN(o.w - 40, 380), bh = 48;
    struct lm_rect box = lm_rect_make(cx - bw / 2, top + clock_h + 44, bw, bh);
    uint32_t ring = li->blocked ? COL_PANEL_DANGER : COL_PANEL_ACCENT;
    struct lm_rect glow = lm_rect_make(box.x - 6, box.y - 6, box.w + 12, box.h + 12);
    fill_round(c, &glow, glow.h / 2, 0x28000000u | (ring & 0xffffffu));
    struct lm_rect glow2 = lm_rect_make(box.x - 3, box.y - 3, box.w + 6, box.h + 6);
    fill_round(c, &glow2, glow2.h / 2, 0x40000000u | (ring & 0xffffffu));
    fill_round(c, &box, bh / 2, 0xff000000u | ring);
    struct lm_rect inner = lm_rect_make(box.x + 2, box.y + 2, box.w - 4, box.h - 4);
    uint32_t fieldc = li->blocked ? lm_theme.lock_box_blocked : lm_theme.lock_box;
    fill_round(c, &inner, inner.h / 2, 0xff000000u | fieldc);
    paint_shape(c, (float)box.x + 32.0f, (float)box.y + (float)bh / 2.0f, 10.0f, SHAPE_LOCK,
                li->nchars || li->blocked ? ring : COL_PANEL_DIM, 255);       /* the padlock lights up while something is typed */
    struct lm_rect text_area = lm_rect_make(box.x + 48, box.y, box.w - 48 - 24, box.h);
    if (li->nchars == 0 && !li->blocked) {
        draw_text_centered(c, &text_area, li->pin ? "Enter PIN" : "Enter password", COL_PANEL_DIM);
    } else {
        int pitch = 22;
        int shown = LM_MIN(li->nchars, text_area.w / pitch);
        float x0 = (float)text_area.x + ((float)text_area.w - (float)shown * (float)pitch) / 2.0f + (float)pitch / 2.0f;
        for (int i = 0; i < shown; i++)
            paint_shape(c, x0 + (float)(i * pitch), (float)box.y + (float)box.h / 2.0f, 6.0f, SHAPE_DISC, lm_theme.text, 255);
    }
    struct lm_rect msg = lm_rect_make(o.x, box.y + box.h + 14, o.w, 20);
    if (li->message[0])
        draw_text_centered(c, &msg, li->message, COL_PANEL_DANGER);
    else
        draw_text_centered(c, &msg, "Enter unlocks, Esc clears", COL_PANEL_DIM);
    struct lm_rect foot = lm_rect_make(o.x, o.y + o.h - 40, o.w, 20);
    char tail[64];
    snprintf(tail, sizeof(tail), "MellaGUI is locked%s%s", li->layout[0] ? "   Layout: " : "", li->layout);
    draw_text_centered(c, &foot, tail, COL_PANEL_DIM);
}


/* Rounded rectangle with anti-aliased corners, blended with `argb` (alpha in the top byte) over what is there. */
static void fill_round_px(const struct paint_ctx *c, const struct lm_rect *rect, int radius, uint32_t argb)
{
    struct lm_rect r;
    if (!lm_rect_intersect(rect, &c->clip, &r))
        return;
    int rad = LM_MIN(radius, LM_MIN(rect->w, rect->h) / 2);
    uint32_t a = argb >> 24;
    for (int y = r.y; y < r.y + r.h; y++) {
        uint32_t *row = fb_row(c, y);
        int dy = 0;                                     /* distance of the row into the corner zone (0 = outside) */
        if (y < rect->y + rad) dy = rect->y + rad - y;
        else if (y >= rect->y + rect->h - rad) dy = y - (rect->y + rect->h - rad) + 1;
        for (int x = r.x; x < r.x + r.w; x++) {
            uint32_t pa = a;
            int dx = 0;
            if (x < rect->x + rad) dx = rect->x + rad - x;
            else if (x >= rect->x + rect->w - rad) dx = x - (rect->x + rect->w - rad) + 1;
            if (dx && dy) {                             /* corner pixel: coverage from the distance to the arc */
                float d = sqrtf((float)((dx - 1) * (dx - 1) + (dy - 1) * (dy - 1))) + 0.5f;
                float cov = (float)rad - d + 0.5f;
                if (cov <= 0.0f)
                    continue;
                if (cov < 1.0f)
                    pa = (uint32_t)((float)a * cov);
            }
            if (pa == 0)
                continue;
            uint32_t src = argb & 0x00ffffffu;                  /* premultiply for blend_px */
            uint32_t pr = div255(((src >> 16) & 255) * pa), pg = div255(((src >> 8) & 255) * pa),
                     pb = div255((src & 255) * pa);
            row[x] = blend_px((pa << 24) | (pr << 16) | (pg << 8) | pb, row[x]);
        }
    }
}

static void fill_round(const struct paint_ctx *c, const struct lm_rect *rect, int radius, uint32_t argb)
{
    struct paint_ctx pc;
    pview(c, &pc);
    struct lm_rect pr = prect(c, rect);
    fill_round_px(&pc, &pr, plen(c, radius), argb);
}

/* ---- round things: discs and the icons of the power button / session dialog ------------------------------------------- */


static float cov01(float v)
{
    return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}

/* Distance from (px, py) to the segment a-b. */
static float seg_dist(float px, float py, float ax, float ay, float bx, float by)
{
    float vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
    float len2 = vx * vx + vy * vy;
    float t = len2 > 0.0f ? cov01((wx * vx + wy * vy) / len2) : 0.0f;
    float dx = px - (ax + t * vx), dy = py - (ay + t * vy);
    return sqrtf(dx * dx + dy * dy);
}

/* Coverage (0..1, one pixel wide anti-aliased edge) of `shape` at (dx, dy) from its centre; R is its half size. */
static float shape_cov(enum shape s, float dx, float dy, float R)
{
    float d = sqrtf(dx * dx + dy * dy);
    switch (s) {
    case SHAPE_DISC:
        return cov01(R - d + 0.5f);
    case SHAPE_POWER: {                                 /* a ring open at the top with a bar through the gap */
        float t = R * 0.24f < 1.3f ? 1.3f : R * 0.24f;
        float ring = cov01(R - d + 0.5f);
        float inner = cov01(d - (R - t) + 0.5f);
        ring = ring < inner ? ring : inner;
        if (dy < 0.0f) {                                /* the gap */
            float g = cov01(fabsf(dx) - t * 0.95f + 0.5f);
            ring *= g;
        }
        float bar = cov01(t * 0.5f - seg_dist(dx, dy, 0.0f, -R + t * 0.5f, 0.0f, -R * 0.1f) + 0.5f);
        return ring > bar ? ring : bar;
    }
    case SHAPE_MOON: {                                  /* a crescent: a disc with another one taken out of it */
        float d2 = sqrtf((dx - R * 0.42f) * (dx - R * 0.42f) + (dy + R * 0.28f) * (dy + R * 0.28f));
        float a = cov01(R - d + 0.5f), b = cov01(d2 - R * 0.82f + 0.5f);
        return a < b ? a : b;
    }
    case SHAPE_RESTART: {                               /* a ring with a gap at the upper right and an arrow head at the end of the arc */
        float t = R * 0.22f < 1.3f ? 1.3f : R * 0.22f, rr = R * 0.82f;
        float ring = cov01(rr + t * 0.5f - d + 0.5f), inner = cov01(d - (rr - t * 0.5f) + 0.5f);
        ring = ring < inner ? ring : inner;
        float ang = atan2f(dy, dx);                     /* the gap: from -70 to -15 degrees (up and to the right) */
        if (ang > -1.22f && ang < -0.26f)
            ring = 0.0f;
        float ex = rr * cosf(-0.26f), ey = rr * sinf(-0.26f);      /* the arc ends here and goes on clockwise */
        float tx = -sinf(-0.26f), ty = cosf(-0.26f);              /* the way it goes on */
        float nx = cosf(-0.26f), ny = sinf(-0.26f);
        float tipx = ex + tx * t * 1.9f, tipy = ey + ty * t * 1.9f;
        float w1 = seg_dist(dx, dy, tipx, tipy, ex + nx * t * 1.9f, ey + ny * t * 1.9f);
        float w2 = seg_dist(dx, dy, tipx, tipy, ex - nx * t * 1.9f, ey - ny * t * 1.9f);
        float head = cov01(t * 0.5f - (w1 < w2 ? w1 : w2) + 0.5f);
        return ring > head ? ring : head;
    }
    case SHAPE_UP:
    case SHAPE_DOWN: {                                  /* a chevron: two strokes meeting at the tip (a small arrow) */
        float t = R * 0.26f < 1.2f ? 1.2f : R * 0.26f;
        float sy = s == SHAPE_UP ? 1.0f : -1.0f;
        float d1 = seg_dist(dx, dy, -R * 0.62f, sy * R * 0.28f, 0.0f, -sy * R * 0.30f);
        float d2 = seg_dist(dx, dy, R * 0.62f, sy * R * 0.28f, 0.0f, -sy * R * 0.30f);
        float d = d1 < d2 ? d1 : d2;
        return cov01(t * 0.5f - d + 0.5f);
    }
    case SHAPE_LEFT:                                    /* the chevron turned a quarter */
        return shape_cov(SHAPE_UP, dy, dx, R);
    case SHAPE_RIGHT:
        return shape_cov(SHAPE_UP, dy, -dx, R);
    case SHAPE_CROSS: {                                 /* an x */
        float t = R * 0.30f < 1.1f ? 1.1f : R * 0.30f;
        float d1 = seg_dist(dx, dy, -R * 0.62f, -R * 0.62f, R * 0.62f, R * 0.62f);
        float d2 = seg_dist(dx, dy, -R * 0.62f, R * 0.62f, R * 0.62f, -R * 0.62f);
        float d = d1 < d2 ? d1 : d2;
        return cov01(t * 0.5f - d + 0.5f);
    }
    case SHAPE_PLUS: {                                  /* a plus */
        float t = R * 0.22f < 1.2f ? 1.2f : R * 0.22f;
        float d1 = seg_dist(dx, dy, -R * 0.62f, 0.0f, R * 0.62f, 0.0f);
        float d2 = seg_dist(dx, dy, 0.0f, -R * 0.62f, 0.0f, R * 0.62f);
        float d = d1 < d2 ? d1 : d2;
        return cov01(t * 0.5f - d + 0.5f);
    }
    case SHAPE_TRASH: {                                 /* a bin: a lid with a handle, a tapering body with two slits */
        float t = R * 0.17f < 1.0f ? 1.0f : R * 0.17f;
        float d = seg_dist(dx, dy, -R * 0.78f, -R * 0.50f, R * 0.78f, -R * 0.50f);                    /* the lid */
        float h = seg_dist(dx, dy, -R * 0.24f, -R * 0.78f, R * 0.24f, -R * 0.78f);                     /* its handle */
        d = d < h ? d : h;
        h = seg_dist(dx, dy, -R * 0.55f, -R * 0.30f, -R * 0.43f, R * 0.80f);                           /* the body: left side, bottom, right side */
        d = d < h ? d : h;
        h = seg_dist(dx, dy, -R * 0.43f, R * 0.80f, R * 0.43f, R * 0.80f);
        d = d < h ? d : h;
        h = seg_dist(dx, dy, R * 0.43f, R * 0.80f, R * 0.55f, -R * 0.30f);
        d = d < h ? d : h;
        float s1 = seg_dist(fabsf(dx), dy, R * 0.17f, -R * 0.05f, R * 0.17f, R * 0.52f);               /* two slits (mirrored) */
        d = d < s1 ? d : s1;
        return cov01(t * 0.5f - d + 0.5f);
    }
    case SHAPE_LOCK: {                                  /* a padlock: body with a keyhole, shackle above */
        dy += R * 0.08f;                                /* (optical centring) */
        float bw = R * 0.64f, bh = R * 0.46f, bcy = R * 0.30f, cr = R * 0.16f;
        float qx = fabsf(dx) - (bw - cr), qy = fabsf(dy - bcy) - (bh - cr);
        float ox = qx > 0.0f ? qx : 0.0f, oy = qy > 0.0f ? qy : 0.0f;
        float inside = qx > qy ? qx : qy;
        float sd = sqrtf(ox * ox + oy * oy) + (inside < 0.0f ? inside : 0.0f) - cr;
        float body = cov01(0.5f - sd);
        float hole = cov01(R * 0.11f - sqrtf(dx * dx + (dy - bcy) * (dy - bcy)) + 0.5f);
        body *= 1.0f - hole;
        float cys = -R * 0.16f, sr = R * 0.44f, t = R * 0.17f < 1.2f ? 1.2f : R * 0.17f;
        float sdist = sqrtf(dx * dx + (dy - cys) * (dy - cys));
        float ring = cov01(sr - sdist + 0.5f);
        float inner = cov01(sdist - (sr - t) + 0.5f);
        ring = (ring < inner ? ring : inner) * cov01(0.5f - (dy - cys));
        float xl = sr - t * 0.5f;
        float leg = cov01(t * 0.5f - seg_dist(fabsf(dx), dy, xl, cys, xl, cys + R * 0.12f) + 0.5f);
        float sh = ring > leg ? ring : leg;
        return body > sh ? body : sh;
    }
    }
    return 0.0f;
}

/* Paints `s` centred on (cx, cy) (pixel units, may be fractional) with half size R in colour rgb at `alpha` (0..255). */
static void paint_shape_px(const struct paint_ctx *c, float cx, float cy, float R, enum shape s, uint32_t rgb, uint32_t alpha)
{
    struct lm_rect box = lm_rect_make((int)floorf(cx - R) - 1, (int)floorf(cy - R) - 1, (int)(2.0f * R) + 4, (int)(2.0f * R) + 4), r;
    if (!lm_rect_intersect(&box, &c->clip, &r))
        return;
    for (int y = r.y; y < r.y + r.h; y++) {
        uint32_t *row = fb_row(c, y);
        for (int x = r.x; x < r.x + r.w; x++) {
            float cov = shape_cov(s, (float)x + 0.5f - cx, (float)y + 0.5f - cy, R);
            uint32_t pa = (uint32_t)(cov * (float)alpha + 0.5f);
            if (pa == 0)
                continue;
            uint32_t pr = div255(((rgb >> 16) & 255) * pa), pg = div255(((rgb >> 8) & 255) * pa), pb = div255((rgb & 255) * pa);
            row[x] = blend_px((pa << 24) | (pr << 16) | (pg << 8) | pb, row[x]);
        }
    }
}

static void paint_shape(const struct paint_ctx *c, float cx, float cy, float R, enum shape s, uint32_t rgb, uint32_t alpha)
{
    struct paint_ctx pc;
    pview(c, &pc);
    float k = (float)c->s;
    paint_shape_px(&pc, ((cx - (float)c->ox) * k), ((cy - (float)c->oy) * k), R * k, s, rgb, alpha);
}

/* ---- the logo: the round brand picture of the panel button and the About dialog -------------------------------------------- */

#include "logo_data.h"

static uint32_t dock_image_px(const struct lm_image *img, const struct lm_rect *box, int x, int y);
static void paint_dock_icon(const struct paint_ctx *c, const struct lm_rect *b, const struct lm_dock_item *it, float lift);

/* Paints the logo (premultiplied ARGB, already a circle) into `box`, from the 32 px picture when that is enough, else from the 96 px one. */
static void paint_logo_px(const struct paint_ctx *c, const struct lm_rect *box)
{
    static const struct lm_image small = { LOGO_SMALL, LOGO_SMALL, (uint32_t *)logo_small },
                                 big = { LOGO_BIG, LOGO_BIG, (uint32_t *)logo_big };
    const struct lm_image *img = box->w <= 48 ? &small : &big;
    struct lm_rect r;
    if (!lm_rect_intersect(box, &c->clip, &r))
        return;
    for (int y = r.y; y < r.y + r.h; y++) {
        uint32_t *row = fb_row(c, y);
        for (int x = r.x; x < r.x + r.w; x++) {
            uint32_t px = dock_image_px(img, box, x, y);
            if (px >> 24)
                row[x] = blend_px(px, row[x]);
        }
    }
}

static void paint_logo(const struct paint_ctx *c, const struct lm_rect *box)
{
    struct paint_ctx pc;
    pview(c, &pc);
    struct lm_rect pb = prect(c, box);
    paint_logo_px(&pc, &pb);
}

static void fit_text(char *out, size_t n, const char *text, int maxw, bool tail);

/* The dock's right-click menu: a rounded card with the button's name in the accent colour and four entries; the highlighted one is lit, an entry
 * that does nothing right now (no window to close) is dim. */
static void paint_menu(const struct paint_ctx *c, const struct lm_menu_info *mi)
{
    fill_round(c, &mi->rect, 12, 0xff000000u | lm_theme.border);
    struct lm_rect in = lm_rect_make(mi->rect.x + 1, mi->rect.y + 1, mi->rect.w - 2, mi->rect.h - 2);
    fill_round(c, &in, 11, 0xf2000000u | COL_PANEL);
    if (mi->title[0]) {
        struct lm_rect head = lm_rect_make(mi->rect.x + 12, mi->rect.y + 6, mi->rect.w - 24, LM_FONT_H);
        char fit[LM_DOCK_NAME_MAX + 4];
        fit_text(fit, sizeof(fit), mi->title, head.w, false);
        draw_text(c, &head, head.x, head.y + 2, fit, COL_PANEL_ACCENT);
        struct lm_rect line = lm_rect_make(mi->rect.x + 10, mi->rect.y + LM_MENU_HEAD - 2, mi->rect.w - 20, 1);
        fill_rect(c, &line, lm_theme.border);
    }
    for (int i = 0; i < mi->n; i++) {
        const struct lm_rect *e = &mi->entry[i];
        if (mi->sel == i && mi->enabled[i])
            fill_round(c, e, 8, 0xff000000u | COL_PANEL_HOVER);
        uint32_t col = !mi->enabled[i] ? COL_PANEL_DIM : mi->sel == i ? COL_PANEL_TEXT : lm_theme.task_text;
        draw_text(c, e, e->x + 12, e->y + (e->h - LM_FONT_H) / 2, mi->label[i], col);
    }
}

/* The session dialog: the output dimmed, a rounded card with a title and two round buttons with their icons and names. */
static void paint_power(const struct paint_ctx *c, const struct lm_power_info *pw)
{
    fill_round(c, &pw->rect, 0, 0xb0000000u);
    fill_round(c, &pw->card, 14, (0xff000000u | lm_theme.border));
    struct lm_rect in = lm_rect_make(pw->card.x + 1, pw->card.y + 1, pw->card.w - 2, pw->card.h - 2);
    fill_round(c, &in, 13, 0xff000000u | COL_PANEL);
    static const char *const questions[5] = { "", "", "Suspend the computer?", "Restart the computer?", "Shut the computer down?" };
    struct lm_rect title = lm_rect_make(pw->card.x, pw->card.y + 14, pw->card.w, LM_FONT_H);
    draw_text_centered(c, &title, pw->confirm ? questions[pw->confirm] : "Session", pw->confirm ? COL_PANEL_ACCENT : COL_PANEL_TEXT);
    struct lm_rect hint = lm_rect_make(pw->card.x, pw->card.y + pw->card.h - LM_FONT_H - 10, pw->card.w, LM_FONT_H);
    draw_text_centered(c, &hint, pw->confirm ? "Enter answers  Y yes  N no" : "Esc cancels", COL_PANEL_DIM);
    static const char *const names[LM_POWER_N] = { "Lock", "Exit", "Suspend", "Restart", "Shutdown" };
    static const enum shape shapes[LM_POWER_N] = { SHAPE_LOCK, SHAPE_CROSS, SHAPE_MOON, SHAPE_RESTART, SHAPE_POWER };
    for (int i = 0; i < LM_POWER_N; i++) {
        const struct lm_rect *b = &pw->button[i];
        if (b->w <= 0)
            continue;                                                             /* (not in this view) */
        enum shape sh = shapes[i];
        const char *name = names[i];
        bool danger = i == 1 || i == 4;                                           /* Exit and Shutdown */
        if (pw->confirm) {                                                        /* "Are you sure?": Yes (the action's own icon) and Cancel */
            sh = i == 0 ? shapes[pw->confirm] : SHAPE_CROSS;
            name = i == 0 ? "Yes" : "Cancel";
            danger = i == 0 && pw->confirm != LM_LOCK_ACT_SUSPEND;
        }
        float cx = (float)b->x + (float)b->w / 2.0f, cy = (float)b->y + (float)b->h / 2.0f, R = (float)b->w / 2.0f;
        bool sel = pw->sel == i && !pw->off[i];
        uint8_t alpha = pw->off[i] ? 0x50 : 255;                                  /* a button that does nothing here is faint */
        uint32_t base = sel ? (danger ? COL_PANEL_DANGER : COL_PANEL_ACCENT) : COL_PANEL_BTN;
        if (sel)
            paint_shape(c, cx, cy, R + 4.0f, SHAPE_DISC, base, 0x50);             /* a soft halo */
        paint_round_button(c, cx, cy, R, base, alpha);
        paint_shape(c, cx, cy, R * (i >= 2 && !pw->confirm ? 0.56f : 0.52f), sh, sel && !danger ? COL_PANEL : COL_PANEL_TEXT, alpha);
        struct lm_rect label = lm_rect_make(pw->hit[i].w > 0 ? pw->hit[i].x : b->x - 8, b->y + b->h + 8, pw->hit[i].w > 0 ? pw->hit[i].w : b->w + 16, LM_FONT_H);
        draw_text_centered(c, &label, name, sel ? COL_PANEL_TEXT : COL_PANEL_DIM);
    }
    if (pw->confirm && pw->confirm != LM_LOCK_ACT_SUSPEND && pw->others > 0) {   /* a server: other people may be logged in */
        char warn[96];
        snprintf(warn, sizeof(warn), "%d other login session%s will end", pw->others, pw->others == 1 ? "" : "s");
        struct lm_rect w = lm_rect_make(pw->card.x, pw->card.y + 46 + 64 + 8 + LM_FONT_H + 22, pw->card.w, LM_FONT_H);
        draw_text_centered(c, &w, warn, COL_PANEL_DANGER);
    }
}

/* The About dialog: the output dimmed, a card with the logo, the name, the system information and a Close button. */
static void paint_about(const struct paint_ctx *c, const struct lm_about_info *ai)
{
    fill_round(c, &ai->rect, 0, 0xb0000000u);
    fill_round(c, &ai->card, 14, (0xff000000u | lm_theme.border));
    struct lm_rect in = lm_rect_make(ai->card.x + 1, ai->card.y + 1, ai->card.w - 2, ai->card.h - 2);
    fill_round(c, &in, 13, 0xff000000u | COL_PANEL);
    float lcx = (float)ai->logo.x + (float)ai->logo.w / 2.0f, lcy = (float)ai->logo.y + (float)ai->logo.h / 2.0f;
    paint_shape(c, lcx, lcy, (float)ai->logo.w / 2.0f + 5.0f, SHAPE_DISC, COL_PANEL_ACCENT, 0x40);         /* a soft halo */
    paint_logo(c, &ai->logo);
    int y = ai->logo.y + ai->logo.h + 12;
    struct lm_rect title = lm_rect_make(ai->card.x, y, ai->card.w, LM_FONT_H);
    draw_text_centered(c, &title, "Mella's Graphical User Interface", COL_PANEL_TEXT);
    struct lm_rect sub = lm_rect_make(ai->card.x, y + LM_FONT_H + 6, ai->card.w, LM_FONT_H);
    draw_text_centered(c, &sub, "MellaGUI: a Wayland compositor and shell", COL_PANEL_DIM);
    y += 2 * LM_FONT_H + 6 + 16;
    struct lm_rect rule = lm_rect_make(ai->card.x + 24, y - 8, ai->card.w - 48, 1);
    fill_rect(c, &rule, lm_theme.border);
    struct lm_rect text = lm_rect_make(ai->card.x + 24, y, ai->card.w - 48, ai->close.y - y);
    for (int i = 0; i < ai->n; i++) {
        int ry = y + i * 18;
        if (ry + LM_FONT_H > ai->close.y - 6)
            break;
        draw_text(c, &text, text.x, ry, ai->row[i].label, COL_PANEL_DIM);
        draw_text(c, &text, text.x + 96, ry, ai->row[i].value, COL_PANEL_TEXT);
    }
    paint_drop(c, &ai->close, btn_radius(LM_MIN(ai->close.w, ai->close.h) / 2), ai->hover_close ? COL_PANEL_ACCENT : COL_PANEL_BTN);
    draw_text_centered(c, &ai->close, "Close", ai->hover_close ? COL_PANEL : COL_PANEL_TEXT);
}

/* A one pixel ring just inside `rect` (the border of a translucent card: filling a border colour below the card would shine through). */
static void stroke_round_px(const struct paint_ctx *c, const struct lm_rect *rect, int radius, uint32_t argb, float bw)
{
    struct lm_rect r;
    if (!lm_rect_intersect(rect, &c->clip, &r))
        return;
    float hw = (float)rect->w / 2.0f, hh = (float)rect->h / 2.0f, rad = (float)LM_MIN(radius, LM_MIN(rect->w, rect->h) / 2);
    float cx = (float)rect->x + hw, cy = (float)rect->y + hh;
    uint32_t src = argb & 0x00ffffffu;
    for (int y = r.y; y < r.y + r.h; y++) {
        uint32_t *row = fb_row(c, y);
        for (int x = r.x; x < r.x + r.w; x++) {
            float qx = fabsf((float)x + 0.5f - cx) - (hw - rad), qy = fabsf((float)y + 0.5f - cy) - (hh - rad);
            float ox = qx > 0.0f ? qx : 0.0f, oy = qy > 0.0f ? qy : 0.0f, m = qx > qy ? qx : qy;
            float d = sqrtf(ox * ox + oy * oy) + (m < 0.0f ? m : 0.0f) - rad;
            float cov = bw == 1.0f ? cov01(0.5f - fabsf(d + 0.5f)) : 0.5f * cov01(1.0f - fabsf(d + 0.5f * bw) / (0.5f * bw));   /* a line `bw` pixels wide */
            uint32_t pa = (uint32_t)(cov * (float)(argb >> 24) + 0.5f);
            if (pa == 0)
                continue;
            uint32_t pr = div255(((src >> 16) & 255) * pa), pg = div255(((src >> 8) & 255) * pa), pb = div255((src & 255) * pa);
            row[x] = blend_px((pa << 24) | (pr << 16) | (pg << 8) | pb, row[x]);
        }
    }
}

static void stroke_round(const struct paint_ctx *c, const struct lm_rect *rect, int radius, uint32_t argb)
{
    struct paint_ctx pc;
    pview(c, &pc);
    struct lm_rect pr = prect(c, rect);
    stroke_round_px(&pc, &pr, plen(c, radius), argb, (float)c->s);
}

/* ---- buttons as beads of water --------------------------------------------------------------------------------------------- */
/* The look of every MellaGUI button (the programs' wlwin_drop, client/uikit.h lm_drop_px): a dark rim, an interior a little darker at the top that lights up
 * towards the bottom (eight soft lens shapes, each smaller and lighter), a small glint near the upper left and, on a wide button, a thin sheen along the top.
 * All in logical pixels, so it is exact at any output scale. `paint_drop` is for opaque buttons, `paint_glint` puts the glint and the light on something that
 * is translucent already (glass keys, the dock's icons). */
static void paint_drop(const struct paint_ctx *c, const struct lm_rect *r, int radius, uint32_t rgb)
{
    if (r->w < 10 || r->h < 12) {
        fill_round(c, r, radius, 0xff000000u | rgb);
        return;
    }
    int rr = LM_MIN(radius, LM_MIN(r->w, r->h) / 2);
    fill_round(c, r, rr, 0xff000000u | mix_rgb(rgb, 0x000000u, 87));                                   /* the rim */
    struct lm_rect body = lm_rect_make(r->x + 1, r->y + 1, r->w - 2, r->h - 3);
    fill_round(c, &body, LM_MAX(rr - 1, 0), 0xff000000u | mix_rgb(rgb, 0x000000u, 26));
    for (int i = 0; i < 8; i++) {
        float t = (float)i / 7.0f;
        int side = 2 + rr / 6 + (int)(t * (float)(rr * 3 / 4 + 3));
        int ly = r->y + (int)((float)r->h * (0.28f + 0.46f * t)), lh = r->y + r->h - 3 - ly, lw = r->w - 2 * side;
        if (lh < 3 || lw < 6)
            continue;
        struct lm_rect lens = lm_rect_make(r->x + side, ly, lw, lh);
        fill_round(c, &lens, LM_MIN(lh / 2, LM_MAX(rr - 1, 1)), 0xff000000u | mix_rgb(rgb, 0xffffffu, 5 + (int)(76.0f * t * t)));
    }
    bool tall = r->h >= 28;
    int gx = r->x + LM_MAX(rr * 7 / 10, 5), gw = LM_CLAMP(r->w / 4, 6, tall ? 14 : 9), gy = tall ? 2 : 1, gh = tall ? 3 : 2;
    struct lm_rect glint = lm_rect_make(gx, r->y + gy, gw, gh);
    fill_round(c, &glint, 1, 0xff000000u | mix_rgb(rgb, 0xffffffu, tall ? 217 : 153));
    if (r->w > 44) {
        int sx = gx + gw + 3, sw = r->x + r->w - rr - 4 - sx;
        if (sw > 0) {
            struct lm_rect sheen = lm_rect_make(sx, r->y + gy, sw, 1);
            fill_rect(c, &sheen, 0xff000000u | mix_rgb(rgb, 0xffffffu, tall ? 89 : 64));
        }
    }
}

static void paint_glint(const struct paint_ctx *c, const struct lm_rect *r, int radius, int alpha)
{
    if (alpha <= 0 || r->w < 10 || r->h < 12)
        return;
    int rr = LM_MIN(radius, LM_MIN(r->w, r->h) / 2);
    struct lm_rect lens = lm_rect_make(r->x + 2 + rr / 4, r->y + r->h * 11 / 20, r->w - 4 - rr / 2, r->h * 9 / 20 - 2);      /* the light the drop gathers */
    if (lens.h >= 3 && lens.w >= 6)
        fill_round(c, &lens, LM_MIN(lens.h / 2, LM_MAX(rr - 1, 1)), ALPHA(alpha * 36 / 255, 0xffffffu));
    bool tall = r->h >= 28;
    int gx = r->x + LM_MAX(rr * 7 / 10, 5), gw = LM_CLAMP(r->w / 4, 6, tall ? 14 : 9);
    struct lm_rect glint = lm_rect_make(gx, r->y + (tall ? 2 : 1), gw, tall ? 3 : 2);
    fill_round(c, &glint, 1, ALPHA(alpha * (tall ? 200 : 150) / 255, 0xffffffu));
    if (r->w > 44) {
        struct lm_rect sheen = lm_rect_make(gx + gw + 3, r->y + (tall ? 2 : 1), LM_MAX(r->x + r->w - rr - 4 - (gx + gw + 3), 0), 1);
        fill_rect(c, &sheen, ALPHA(alpha * 70 / 255, 0xffffffu));
    }
    stroke_round(c, r, rr, ALPHA(alpha * 70 / 255, 0x000000u));                                  /* the edge that bends the light */
}

/* `text` cut to `maxw` pixels: with ".." at the end (names of icons) or, for fields, its tail (so the caret stays visible). */
static void fit_text(char *out, size_t n, const char *text, int maxw, bool tail)
{
    snprintf(out, n, "%s", text);
    if (lm_font_text_width(out) <= maxw)
        return;
    if (tail) {
        const char *p = text;
        while (*p && lm_font_text_width(p) > maxw) {
            p++;
            while (((unsigned char)*p & 0xc0) == 0x80)
                p++;
        }
        snprintf(out, n, "%s", p);
        return;
    }
    size_t len = strlen(out);
    while (len > 0 && lm_font_text_width(out) + 2 * LM_FONT_W > maxw) {
        len--;
        while (len > 0 && ((unsigned char)out[len] & 0xc0) == 0x80)
            len--;
        out[len] = 0;
    }
    snprintf(out + len, n - len, "..");
}

/* Text on the light glass of the launcher: a one pixel dark shadow keeps it readable over any wallpaper. */
static void lc_text(const struct paint_ctx *c, const struct lm_rect *area, int x, int y, const char *text, uint32_t color)
{
    struct lm_rect sh = *area;
    sh.x++;
    sh.y++;
    draw_text(c, &sh, x + 1, y + 1, text, lm_theme.glass_shadow);
    draw_text(c, area, x, y, text, color);
}

static void lc_text_centered(const struct paint_ctx *c, const struct lm_rect *area, const char *text, uint32_t color)
{
    int w = lm_font_text_width(text);
    lc_text(c, area, area->x + (area->w - w) / 2, area->y + (area->h - LM_FONT_H) / 2, text, color);
}

static void paint_launcher_button(const struct paint_ctx *c, const struct lm_rect *r, const char *text, bool hot, bool on)
{
    int rad = btn_radius(LM_MIN(r->w, r->h) / 2);
    fill_round(c, r, rad, hot ? (0xf0000000u | COL_PANEL_ACCENT) : on ? ALPHA(0x70u, lm_theme.glass) : ALPHA(0x48u, lm_theme.glass));
    stroke_round(c, r, rad, ALPHA(0x80u, lm_theme.glass));
    paint_glint(c, r, rad, hot ? 255 : 200);
    if (hot)
        draw_text_centered(c, r, text, COL_PANEL);
    else
        lc_text_centered(c, r, text, lm_theme.glass_text);
}

/* The launcher dialog: the output dimmed a little, a big light translucent card with the dock's icons as large round buttons, a "+" tile and (edit mode)
 * a red x on every icon; the add form replaces the icons. */
static void paint_launcher(const struct paint_ctx *c, const struct lm_launcher_info *li)
{
    if (li->veil > 0.0f)
        fill_round(c, &li->rect, 0, (uint32_t)(0x40 * LM_CLAMP(li->veil, 0.0f, 1.0f)) << 24);
    if (li->veil_only)
        return;
    fill_round(c, &li->card, 18, ALPHA(0x3cu, lm_theme.glass));                                  /* light glass: a white veil over the wallpaper */
    stroke_round(c, &li->card, 18, ALPHA(0x8cu, lm_theme.glass));
    if (li->searching) {
        char st[64];
        snprintf(st, sizeof(st), "Search: %s_", li->search);
        lc_text_centered(c, &li->title, st, lm_theme.glass_text);
    } else {
        lc_text_centered(c, &li->title, li->form ? (li->editing ? "Change an application" : "Add an application") : "Applications", lm_theme.glass_text);
    }
    if (!li->form) {
        for (int g = 0; g < li->n; g++) {
            const struct lm_rect *b = &li->icon[g].box;
            bool sel = li->sel == g;
            if (sel) {
                struct lm_rect hl = li->icon[g].hit;
                hl.x += 4;
                hl.w -= 8;
                fill_round(c, &hl, 12, ALPHA(0x48u, lm_theme.glass));
            }
            paint_dock_icon(c, b, li->icon[g].item, sel ? 1.0f : 0.0f);
            char fit[LM_DOCK_NAME_MAX + 4];
            fit_text(fit, sizeof(fit), li->icon[g].item->name, LM_LAUNCHER_CELL_W - 12, false);
            struct lm_rect label = lm_rect_make(li->icon[g].hit.x, b->y + b->h + 10, li->icon[g].hit.w, LM_FONT_H);
            lc_text_centered(c, &label, fit, lm_theme.glass_text);
            if (li->icon[g].running)
                paint_shape(c, (float)(b->x + b->w / 2), (float)(label.y + LM_FONT_H + 4), 2.2f, SHAPE_DISC, COL_PANEL_TEXT, 0xdd);
            if (li->edit) {
                const struct lm_rect *x = &li->icon[g].remove;
                float xc = (float)x->x + (float)x->w / 2.0f, yc = (float)x->y + (float)x->h / 2.0f;
                paint_shape(c, xc, yc, 10.5f, SHAPE_DISC, COL_PANEL, 0xff);
                paint_shape(c, xc, yc, 9.0f, SHAPE_DISC, COL_PANEL_DANGER, 0xff);
                paint_shape(c, xc, yc, 5.5f, SHAPE_CROSS, lm_theme.on_colour, 0xff);
            }
        }
        if (li->add.w > 0) {
            bool sel = li->sel == li->n;
            float cx = (float)li->add.x + (float)li->add.w / 2.0f, cy = (float)li->add.y + 6.0f + (float)LM_LAUNCHER_ICON / 2.0f;
            if (sel) {
                struct lm_rect hl = li->add;
                hl.x += 4;
                hl.w -= 8;
                fill_round(c, &hl, 12, ALPHA(0x48u, lm_theme.glass));
            }
            paint_shape(c, cx, cy, (float)LM_LAUNCHER_ICON / 2.0f, SHAPE_DISC, sel ? COL_PANEL_ACCENT : lm_theme.glass, sel ? 0xff : 0x70);
            paint_shape(c, cx, cy, (float)LM_LAUNCHER_ICON * 0.27f, SHAPE_PLUS, sel ? COL_PANEL : lm_theme.glass, 0xff);
            struct lm_rect label = lm_rect_make(li->add.x, (int)cy + LM_LAUNCHER_ICON / 2 + 10, li->add.w, LM_FONT_H);
            lc_text_centered(c, &label, "Add", lm_theme.glass_text);
        }
        if (li->n == 0 && li->add.w <= 0) {
            struct lm_rect none = lm_rect_make(li->card.x, li->card.y + li->card.h / 2, li->card.w, LM_FONT_H);
            lc_text_centered(c, &none, li->searching ? "Nothing found" : "Nothing here", lm_theme.glass_text);
        }
    } else {
        for (int i = 0; i < 3; i++) {
            const struct lm_rect *f = &li->field[i];
            struct lm_rect lab = lm_rect_make(f->x, f->y - LM_FONT_H - 4, f->w, LM_FONT_H);
            lc_text(c, &lab, lab.x, lab.y, li->field_label[i], lm_theme.glass_text);
            fill_round(c, f, 8, 0x58000000u);
            stroke_round(c, f, 8, li->focus == i ? (0xff000000u | COL_PANEL_ACCENT) : ALPHA(0x90u, lm_theme.glass));
            char fit[LM_DOCK_CMD_MAX];
            int vs, ve, avail = f->w - 20 - LM_LAUNCHER_HELP_D - 8;           /* room for the "?" at the end */
            lm_launcher_field_view(li->field_text[i], li->caret[i], avail, &vs, &ve);
            snprintf(fit, sizeof(fit), "%.*s", ve - vs, li->field_text[i] + vs);
            int ty = f->y + (f->h - LM_FONT_H) / 2;
            draw_text(c, f, f->x + 10, ty, fit, COL_PANEL_TEXT);
            if (li->focus == i) {
                char before[LM_DOCK_CMD_MAX];
                snprintf(before, sizeof(before), "%.*s", li->caret[i] - vs, li->field_text[i] + vs);
                struct lm_rect caret = lm_rect_make(f->x + 10 + lm_font_text_width(before), ty, 2, LM_FONT_H);
                fill_rect(c, &caret, COL_PANEL_ACCENT);
            }
            const struct lm_rect *hb = &li->help_btn[i];                     /* the small "?" circle */
            float hx = (float)hb->x + (float)hb->w / 2.0f, hy = (float)hb->y + (float)hb->h / 2.0f;
            paint_shape(c, hx, hy, (float)hb->w / 2.0f, SHAPE_DISC, li->help == i + 1 ? COL_PANEL_ACCENT : lm_theme.glass, li->help == i + 1 ? 0xff : 0x90);
            draw_text_centered(c, hb, "?", li->help == i + 1 ? COL_PANEL : COL_PANEL_TEXT);
        }
        struct lm_rect kl = lm_rect_make(li->card.x + 28, li->kind_box[0].y - LM_FONT_H - 6, li->card.w - 56, LM_FONT_H);
        char lab[64];
        snprintf(lab, sizeof(lab), "Built-in icons   (%s)", li->kind_name);
        lc_text(c, &kl, kl.x, kl.y, lab, lm_theme.glass_text);
        for (int i = 0; i < LM_LAUNCHER_KIND_N; i++) {
            const struct lm_rect *b = &li->kind_box[i];
            bool on = li->kind == i - 1;
            float cx = (float)b->x + (float)b->w / 2.0f, cy = (float)b->y + (float)b->h / 2.0f, R = (float)b->w / 2.0f - 2.0f;
            if (on)
                paint_shape(c, cx, cy, R + 3.0f, SHAPE_DISC, li->focus == 3 ? COL_PANEL_ACCENT : lm_theme.glass, 0xd0);
            if (i == 0) {
                paint_shape(c, cx, cy, R, SHAPE_DISC, COL_PANEL_BTN, 0xc0);
                draw_text_centered(c, b, "auto", COL_PANEL_TEXT);
                continue;
            }
            struct lm_rect box = lm_rect_make((int)(cx - R), (int)(cy - R), (int)(2.0f * R), (int)(2.0f * R));
            struct lm_dock_item it = { .kind = i - 1 };
            paint_dock_icon(c, &box, &it, 0.0f);
        }
        if (li->message[0]) {
            struct lm_rect m = lm_rect_make(li->card.x + 12, li->card.y + 342, li->card.w - 24, LM_FONT_H);
            char fit[160];
            fit_text(fit, sizeof(fit), li->message, m.w, false);
            lc_text_centered(c, &m, fit, lm_theme.glass_error);
        }
    }
    struct lm_rect hint = lm_rect_make(li->card.x, li->button[0].y - LM_FONT_H - 12, li->card.w, LM_FONT_H);
    lc_text_centered(c, &hint, li->hint, lm_theme.glass_hint);
    for (int i = 0; i < 2; i++)
        paint_launcher_button(c, &li->button[i], li->button_text[i], li->hover_button == i, i == 0 && li->edit);
    if (li->form && li->help) {                                         /* a help card above the form: the card is veiled, the help in front of it */
        fill_round(c, &li->card, 20, 0xa0000000u);
        fill_round(c, &li->help_card, 14, (0xf0000000u | lm_theme.help_card));
        stroke_round(c, &li->help_card, 14, 0xff000000u | COL_PANEL_ACCENT);
        struct lm_rect tt = lm_rect_make(li->help_card.x + 20, li->help_card.y + 16, li->help_card.w - 40, LM_FONT_H);
        lc_text(c, &tt, tt.x, tt.y, li->help_title, 0xff000000u | COL_PANEL_ACCENT);
        int cols = LM_MAX((li->help_card.w - 40) / LM_FONT_W, 10), ly = li->help_card.y + 44;
        char line[160];
        for (const char *p = li->help_text; *p && ly + LM_FONT_H < li->help_close.y;) {          /* word wrap */
            int len = 0, brk = -1, px = 0;
            while (p[len] && len < (int)sizeof(line) - 1) {              /* as many letters as cols * 8 pixels hold */
                int used;
                uint32_t cp = lm_utf8_decode(p + len, (int)strlen(p + len), &used);
                used = used > 0 ? used : 1;
                if (px + lm_font_advance(cp) > cols * LM_FONT_W || len + used > (int)sizeof(line) - 1)
                    break;
                if (p[len] == ' ')
                    brk = len;
                px += lm_font_advance(cp);
                len += used;
            }
            if (p[len] && brk > 0)
                len = brk;
            snprintf(line, sizeof(line), "%.*s", len, p);
            struct lm_rect lr = lm_rect_make(tt.x, ly, tt.w, LM_FONT_H);
            lc_text(c, &lr, lr.x, lr.y, line, ALPHA(0xffu, lm_theme.glass));
            p += len;
            while (*p == ' ')
                p++;
            ly += LM_FONT_H + 3;
        }
        paint_launcher_button(c, &li->help_close, "Close", false, false);
    }
    if (li->drag >= 0 && li->icon[li->drag].item) {                     /* an icon in the hand: the dock lights up as the place to drop it */
        if (li->drop.w > 0) {
            bool hot = li->drop_pos >= 0;
            fill_round(c, &li->drop, 22, hot ? ALPHA(0x40u, lm_theme.glass) : ALPHA(0x1cu, lm_theme.glass));
            stroke_round(c, &li->drop, 22, hot ? ALPHA(0xc0u, lm_theme.glass) : ALPHA(0x60u, lm_theme.glass));
            struct lm_rect tl = lm_rect_make(li->drop.x, li->drop.y + 8, li->drop.w, LM_FONT_H);
            lc_text_centered(c, &tl, "Drop it on the dock", lm_theme.glass_text);
            if (hot)
                fill_round(c, &li->marker, 1, 0xf0000000u | COL_PANEL_ACCENT);
        }
        struct lm_rect hl = li->icon[li->drag].hit;
        hl.x += 4;
        hl.w -= 8;
        fill_round(c, &hl, 12, 0x30000000u);                              /* the place it came from dims */
        paint_dock_icon(c, &li->drag_box, li->icon[li->drag].item, 1.0f);
    }
}

/* ---- the dock ------------------------------------------------------------------------------------------------------------ */

/* Premultiplied pixel of a picture icon at (px, py) inside `box`: the picture is fitted into the square and sampled bilinearly. */
static uint32_t dock_image_px(const struct lm_image *img, const struct lm_rect *box, int x, int y)
{
    float s = (float)box->w / (float)(img->w > img->h ? img->w : img->h);
    float iw = (float)img->w * s, ih = (float)img->h * s;
    float fx = ((float)(x - box->x) + 0.5f - ((float)box->w - iw) / 2.0f) / s - 0.5f;
    float fy = ((float)(y - box->y) + 0.5f - ((float)box->h - ih) / 2.0f) / s - 0.5f;
    if (fx < -0.5f || fy < -0.5f || fx > (float)img->w - 0.5f || fy > (float)img->h - 0.5f)
        return 0;
    int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
    float tx = fx - (float)x0, ty = fy - (float)y0;
    float acc[4] = { 0, 0, 0, 0 };
    for (int k = 0; k < 4; k++) {
        int xx = LM_CLAMP(x0 + (k & 1), 0, img->w - 1), yy = LM_CLAMP(y0 + (k >> 1), 0, img->h - 1);
        float wgt = ((k & 1) ? tx : 1.0f - tx) * ((k >> 1) ? ty : 1.0f - ty);
        uint32_t p = img->px[(size_t)yy * (size_t)img->w + (size_t)xx];
        acc[0] += wgt * (float)(p >> 24);
        acc[1] += wgt * (float)((p >> 16) & 255);
        acc[2] += wgt * (float)((p >> 8) & 255);
        acc[3] += wgt * (float)(p & 255);
    }
    return ((uint32_t)(acc[0] + 0.5f) << 24) | ((uint32_t)(acc[1] + 0.5f) << 16) | ((uint32_t)(acc[2] + 0.5f) << 8) | (uint32_t)(acc[3] + 0.5f);
}

/* One round icon (a picture or a built-in one) in `b`; lift 0..1 brightens it (hovered). */
/* An icon, `alpha` (0..255) as solid; a box that is not square (an icon melting into the edge of the screen) squashes the picture. */
static void paint_dock_icon_px(const struct paint_ctx *c, const struct lm_rect *b, const struct lm_dock_item *it, float lift, int alpha)
{
    if (alpha <= 0 || b->w <= 0 || b->h <= 0)
        return;
    float R = (float)b->w / 2.0f, cx = (float)b->x + R, cy = (float)b->y + (float)b->h / 2.0f, aspect = (float)b->w / (float)b->h;
    int plate = it->img && it->plate ? lm_dock_plate_rgb() : -1;     /* a logo with no background of its own sits on a round plate */
    bool shadowed = !it->img || plate >= 0;
    int m = !shadowed ? 1 : (int)(R * 0.45f) + 2;                    /* built-in icons and plates have a shadow beyond the box */
    struct lm_rect box = lm_rect_make(b->x - m, b->y - m, b->w + 2 * m, b->h + 2 * m + (shadowed ? 2 : 0)), r;
    if (!lm_rect_intersect(&box, &c->clip, &r))
        return;
    int in = (int)lroundf((float)b->w * 0.68f), inh = (int)lroundf((float)b->h * 0.68f);       /* the logo on the plate */
    struct lm_rect logo = lm_rect_make(b->x + (b->w - in) / 2, b->y + (b->h - inh) / 2, in, inh);
    for (int y = r.y; y < r.y + r.h; y++) {
        uint32_t *row = fb_row(c, y);
        for (int x = r.x; x < r.x + r.w; x++) {
            uint32_t px;
            float dy = ((float)y + 0.5f - cy) * aspect;
            if (!it->img) {
                px = lm_dockicon_pixel(it->kind, (float)x + 0.5f - cx, dy, R, lift);
            } else if (plate >= 0) {
                px = lm_dockicon_plate_pixel((uint32_t)plate, (float)x + 0.5f - cx, dy, R, lift);
                if (alpha < 255)
                    px = fade_px(px, alpha);
                if (px >> 24)
                    row[x] = blend_px(px, row[x]);
                px = dock_image_px(it->img, &logo, x, y);
            } else {
                px = dock_image_px(it->img, b, x, y);
                if (it->round && (px >> 24))                          /* a picture that fills its square takes the shape of the icon */
                    px = fade_px(px, (int)(lm_dockicon_shape_coverage((float)x + 0.5f - cx, dy, R) * 255.0f + 0.5f));
            }
            if (alpha < 255)
                px = fade_px(px, alpha);
            if (px >> 24)
                row[x] = blend_px(px, row[x]);
        }
    }
}

static void paint_dock_icon_a(const struct paint_ctx *c, const struct lm_rect *b, const struct lm_dock_item *it, float lift, int alpha)
{
    struct paint_ctx pc;
    pview(c, &pc);
    struct lm_rect pb = prect(c, b);
    paint_dock_icon_px(&pc, &pb, it, lift, alpha);
    if (alpha > 0 && b->w >= 24 && b->h >= 24) {                         /* the bead of water: the light it gathers, a glint and the edge that bends the light */
        struct lm_rect ring = *b;
        paint_glint(c, &ring, LM_MIN(b->w, b->h) / 2 * lm_metrics.button_roundness / 100, alpha);
    }
}

static void paint_dock_icon(const struct paint_ctx *c, const struct lm_rect *b, const struct lm_dock_item *it, float lift)
{
    paint_dock_icon_a(c, b, it, lift, 255);
}

/* The dock: round icons without any background, a dot under the ones that have a window, the name of the hovered one above it. An icon that is
 * rising out of the edge of the screen (autohide) is vague: a few copies of it a little apart, each faint, with a fainter one in the middle. */
static void paint_dock(const struct paint_ctx *c, const struct lm_dock_info *d)
{
    for (int i = 0; i < d->n; i++) {
        const struct lm_rect *b = &d->icon[i].box;
        const struct lm_dock_item *it = d->icon[i].item;
        int alpha = d->icon[i].alpha;
        float lift = i == d->hover ? 1.0f : 0.0f;
        if (d->icon[i].blur > 0.03f && alpha > 0) {
            int r = 1 + (int)(d->icon[i].blur * 6.0f);
            static const int off[4][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
            for (int k = 0; k < 4; k++) {
                struct lm_rect g = lm_rect_make(b->x + off[k][0] * r, b->y + off[k][1] * r, b->w, b->h);
                paint_dock_icon_a(c, &g, it, lift, alpha * 22 / 100);
            }
            paint_dock_icon_a(c, b, it, lift, alpha * (100 - (int)(d->icon[i].blur * 55.0f)) / 100);
        } else {
            paint_dock_icon_a(c, b, it, lift, alpha);
        }
        if (d->icon[i].running && alpha > 40)
            paint_shape(c, (float)(b->x + b->w / 2), (float)d->dot_y, 2.6f, SHAPE_DISC, COL_PANEL_TEXT, 0xdd * (uint32_t)alpha / 255);
    }
    if (d->drag_item) {                                                  /* the icon in the hand; a red x says that letting go removes it */
        paint_dock_icon(c, &d->drag_box, d->drag_item, 1.0f);
        if (d->drag_remove) {
            float xc = (float)d->drag_box.x + 6.0f, yc = (float)d->drag_box.y + 6.0f;
            paint_shape(c, xc, yc, 10.5f, SHAPE_DISC, COL_PANEL, 0xff);
            paint_shape(c, xc, yc, 9.0f, SHAPE_DISC, COL_PANEL_DANGER, 0xff);
            paint_shape(c, xc, yc, 5.5f, SHAPE_CROSS, lm_theme.on_colour, 0xff);
        }
    }
    if (d->label_text) {
        fill_round(c, &d->label, 7, 0xff000000u | lm_theme.border);
        struct lm_rect in = lm_rect_make(d->label.x + 1, d->label.y + 1, d->label.w - 2, d->label.h - 2);
        fill_round(c, &in, 6, 0xf0000000u | COL_PANEL);
        draw_text_centered(c, &d->label, d->label_text, COL_PANEL_TEXT);
    }
}

/* Shadow, hairline ring and body of a gadget card (shared by all gadgets). */
static uint32_t mix_rgb(uint32_t a, uint32_t b, int t)
{
    uint32_t out = 0;
    for (int sh = 16; sh >= 0; sh -= 8)
        out |= (uint32_t)((((a >> sh) & 255) * (uint32_t)(255 - t) + ((b >> sh) & 255) * (uint32_t)t + 127) / 255) << sh;
    return out;
}

/* Bright, evenly spaced colours of the round panel buttons (workspaces and keyboard layouts). */
#define panel_palette ((const uint32_t[8]){ lm_theme.hue_1, lm_theme.hue_2, lm_theme.hue_3, lm_theme.hue_4, lm_theme.hue_5, lm_theme.hue_6, lm_theme.hue_7, lm_theme.hue_8 })

static void paint_panel_shadow_px(const struct paint_ctx *c, const struct lm_rect *rect)
{
    struct lm_rect r;
    if (!lm_rect_intersect(rect, &c->clip, &r))
        return;
    for (int y = r.y; y < r.y + r.h; y++) {
        float t = 1.0f - ((float)(y - rect->y) + 0.5f) / (float)rect->h;           /* 1 at the bar, 0 at the end of the shadow */
        uint32_t a = (uint32_t)(t * t * 125.0f + 0.5f);
        if (!a)
            continue;
        uint32_t *row = fb_row(c, y);
        for (int x = r.x; x < r.x + r.w; x++)
            row[x] = blend_px(a << 24, row[x]);
    }
}

static void paint_panel_shadow(const struct paint_ctx *c, const struct lm_rect *rect)
{
    struct paint_ctx pc;
    pview(c, &pc);
    struct lm_rect pr = prect(c, rect);
    paint_panel_shadow_px(&pc, &pr);
}

/* The panel's buttons follow [windows] button-roundness like the dock's icons: the round ones (workspace cells, layout badge, power) become rounded squares
 * below 100, the rounded rectangles (taskbar, tray, Sticky, Kbd) get a smaller corner radius. */
static int btn_radius(int r)
{
    return r * lm_metrics.button_roundness / 100;
}

static void paint_flat_button(const struct paint_ctx *c, float cx, float cy, float R, uint32_t rgb, uint32_t alpha)
{
    if (lm_metrics.button_roundness >= 100) {
        paint_shape(c, cx, cy, R, SHAPE_DISC, rgb, alpha);
        return;
    }
    struct lm_rect r = lm_rect_make((int)lroundf(cx - R), (int)lroundf(cy - R), (int)lroundf(R * 2.0f), (int)lroundf(R * 2.0f));
    fill_round(c, &r, (int)lroundf(R * (float)lm_metrics.button_roundness / 100.0f), (alpha << 24) | (rgb & 0xffffffu));
}

/* A round (or, with a lower [windows] button-roundness, rounded square) button: a drop of water; a faint one (alpha under 255) stays flat. */
static void paint_round_button(const struct paint_ctx *c, float cx, float cy, float R, uint32_t rgb, uint32_t alpha)
{
    if (alpha < 255 || R < 7.0f) {
        paint_flat_button(c, cx, cy, R, rgb, alpha);
        return;
    }
    struct lm_rect r = lm_rect_make((int)lroundf(cx - R), (int)lroundf(cy - R), (int)lroundf(R * 2.0f), (int)lroundf(R * 2.0f));
    paint_drop(c, &r, (int)lroundf(R * (float)lm_metrics.button_roundness / 100.0f), rgb);
}

/* The flag of the country a keyboard layout belongs to, 22 x 14 pixels centred in `box` (the colours are literals on purpose: a flag is a flag in
 * every theme). `shade` 0..255 fades it towards the panel (the layouts that are not in use). false = no flag for that layout id (upper case). */
static bool paint_flag(const struct paint_ctx *c, const struct lm_rect *box, const char *id, int shade, bool ring)
{
    struct lm_flag_rect fr[LM_FLAG_MAX];
    int n = lm_flag_rects(id, fr);
    if (!n)
        return false;
    int x = box->x + (box->w - LM_FLAG_W) / 2, y = box->y + (box->h - LM_FLAG_H) / 2;
    struct lm_rect edge = lm_rect_make(x - 1, y - 1, LM_FLAG_W + 2, LM_FLAG_H + 2);
    fill_round(c, &edge, btn_radius(3), 0xff000000u | (ring ? lm_theme.on_colour : mix_rgb(COL_PANEL, 0xffffffu, 40)));
    for (int i = 0; i < n; i++) {
        struct lm_rect r = lm_rect_make(x + fr[i].x, y + fr[i].y, fr[i].w, fr[i].h);
        fill_rect(c, &r, 0xff000000u | mix_rgb(fr[i].rgb, COL_PANEL, shade));
    }
    return true;
}

static void paint_panel(const struct paint_ctx *c, const struct lm_panel_info *p)
{
    fill_rect(c, &p->bar, COL_PANEL);
    /* thin accent line at the bottom edge */
    struct lm_rect line = lm_rect_make(p->bar.x, p->bar.y + p->bar.h - 1, p->bar.w, 1);
    fill_rect(c, &line, lm_theme.panel_line);

    if (p->hover_launcher || p->about_open)                                  /* the round logo button; a ring shows it is live */
        paint_shape(c, (float)p->launcher.x + (float)p->launcher.w / 2.0f, (float)p->launcher.y + (float)p->launcher.h / 2.0f,
                    (float)p->launcher.w / 2.0f + 1.5f, SHAPE_DISC, COL_PANEL_ACCENT, p->about_open ? 0xff : 0xa0);
    paint_logo(c, &p->launcher);

    for (int i = 0; i < LM_NUM_WORKSPACES; i++) {          /* round, each workspace in its own colour: bright = the current one, tinted = the others */
        const struct lm_rect *w = &p->workspace[i];
        bool cur = i == p->current_workspace, occupied = (p->workspace_occupied >> i) & 1;
        uint32_t col = panel_palette[i % 8];
        uint32_t bg = cur ? (i == p->hover_workspace ? mix_rgb(col, 0xffffffu, 40) : col)
                          : mix_rgb(COL_PANEL, col, i == p->hover_workspace ? 215 : 165);
        float cx = (float)w->x + (float)w->w / 2.0f, cy = (float)w->y + (float)w->h / 2.0f, R = (float)LM_MIN(w->w, w->h) / 2.0f;
        if (cur)                                            /* the current one: a white ring and dark digit, the others a duller disc and a white digit */
            paint_flat_button(c, cx, cy, R, lm_theme.on_colour, 255);
        paint_round_button(c, cx, cy, cur ? R - 2.0f : R, bg, 255);
        char n[2] = { (char)('1' + i), 0 };
        draw_text_centered(c, w, n, cur ? lm_theme.ink : lm_theme.on_colour);
        if (!cur && occupied)                               /* a window lives there: a small white dot under the digit */
            paint_shape(c, cx, cy + R * 0.66f, 1.7f, SHAPE_DISC, lm_theme.on_colour, 255);
    }
    for (int i = 0; i < p->ntasks; i++) {
        const struct lm_rect *r = &p->task[i].rect;
        uint32_t bg = p->task[i].active ? COL_PANEL_ACCENT : (p->task[i].hover ? COL_PANEL_HOVER : COL_PANEL_BTN);
        uint32_t fg = p->task[i].active ? COL_PANEL : (p->task[i].minimized ? COL_PANEL_DIM : COL_PANEL_TEXT);
        paint_drop(c, r, btn_radius(LM_MIN(r->w, r->h) / 2), bg);
        struct lm_rect label = lm_rect_make(r->x + 4, r->y, r->w - 8, r->h);
        draw_text_centered(c, &label, p->task[i].title[0] ? p->task[i].title : "-", fg);
    }
    if (!p->ntasks && p->active_title && p->active_title[0])
        draw_text_centered(c, &p->title_area, p->active_title, COL_PANEL_TEXT);
    for (int i = 0; i < p->ntray; i++) {                 /* programs' icons: a button with the icon and / or a label */
        const struct lm_rect *r = &p->tray[i].rect;
        const struct lm_texture *ic = p->tray[i].icon;
        paint_drop(c, r, btn_radius(LM_MIN(r->w, r->h) / 2), p->tray[i].hover ? COL_PANEL_HOVER : COL_PANEL_BTN);
        int x = r->x + 6, tw = p->tray[i].text[0] ? lm_font_text_width(p->tray[i].text) : 0;
        if (ic && !tw)
            x = r->x + (r->w - ic->width) / 2;
        if (ic) {
            struct lm_rect box = lm_rect_make(x, r->y + (r->h - ic->height) / 2, ic->width, ic->height);
            blit_tex(c, ic->pixels, ic->width, ic->width, ic->height, &box, 255, false);
            x += ic->width + 4;
        }
        if (tw)
            draw_text(c, r, x, r->y + (r->h - LM_FONT_H) / 2, p->tray[i].text, COL_PANEL_TEXT);
    }
    if (p->caps.w > 0) {
        paint_drop(c, &p->caps, btn_radius(LM_MIN(p->caps.w, p->caps.h) / 2), COL_PANEL_DANGER);
        draw_text_centered(c, &p->caps, p->caps_text, COL_PANEL_TEXT);
    }
    {                                                   /* the keyboard layout: a round badge, every layout in its own colour; with layout-badges one for each layout, the active one bright */
        int nbadge = p->nbadges > 0 ? p->nbadges : 1;
        for (int i = 0; i < nbadge; i++) {
            const struct lm_rect *r = p->nbadges > 0 ? &p->badge[i].rect : &p->layout;
            const char *text = p->nbadges > 0 ? p->badge[i].text : p->layout_text;
            bool active = p->nbadges > 0 ? p->badge[i].active : true;
            bool hover = p->nbadges > 0 ? p->badge[i].hover : p->hover_layout;
            if (p->nbadges > 0) {                       /* a layout with a flag shows the flag (and its label) instead of the coloured disc */
                const char *label = p->badge[i].label;
                struct lm_rect fb = *r;
                if (label[0]) {
                    fb.x += 3;
                    fb.w = LM_FLAG_W + 6;
                }
                if (paint_flag(c, &fb, text, active ? 0 : hover ? 90 : 150, active)) {
                    if (label[0])
                        draw_text(c, r, fb.x + fb.w + 3, r->y + (r->h - LM_FONT_H) / 2, label, active ? COL_PANEL_TEXT : COL_PANEL_DIM);
                    continue;
                }
            }
            unsigned h = 0;
            for (const char *q = text; *q; q++)
                h = h * 31 + (unsigned char)*q;
            uint32_t col = panel_palette[(h + 3) % 8];
            if (hover)
                col = mix_rgb(col, 0xffffffu, 55);
            float cx = (float)r->x + (float)r->w / 2.0f, cy = (float)r->y + (float)r->h / 2.0f;
            paint_round_button(c, cx, cy, (float)LM_MIN(r->w, r->h) / 2.0f, col, active ? 255 : 110);
            draw_text_centered(c, r, text, active ? lm_theme.ink : COL_PANEL_TEXT);
        }
    }
    uint32_t sbg = p->sticky_on ? COL_PANEL_ACCENT : mix_rgb(COL_PANEL_BTN, 0xffffffu, p->hover_sticky ? 30 : 18);
    if (p->sticky_on && p->hover_sticky)
        sbg = lm_theme.sticky_hot;
    paint_drop(c, &p->sticky, btn_radius(LM_MIN(p->sticky.w, p->sticky.h) / 2), sbg);
    draw_text_centered(c, &p->sticky, p->sticky_text, p->sticky_on ? lm_theme.ink : COL_PANEL_TEXT);
    {                                                   /* the on-screen keyboard: a teal button with a little keyboard, white while it is showing */
        uint32_t obg = p->osk_open ? lm_theme.on_colour : p->hover_osk ? mix_rgb(panel_palette[4], 0xffffffu, 55) : panel_palette[4];
        uint32_t ofg = p->osk_open ? panel_palette[4] : lm_theme.on_colour;
        paint_drop(c, &p->osk, btn_radius(LM_MIN(p->osk.w, p->osk.h) / 2), obg);
        int cx = p->osk.x + p->osk.w / 2, cy = p->osk.y + p->osk.h / 2, bx = cx - 11, by = cy - 6;
        struct lm_rect body = lm_rect_make(bx, by, 22, 12);
        fill_round(c, &body, 3, 0xff000000u | ofg);
        for (int i = 0; i < 5; i++) {                   /* the keys are holes in the body: two rows and a space bar */
            struct lm_rect k = lm_rect_make(bx + 3 + i * 4, by + 2, 2, 2);
            fill_rect(c, &k, obg);
        }
        for (int i = 0; i < 4; i++) {
            struct lm_rect k = lm_rect_make(bx + 5 + i * 4, by + 5, 2, 2);
            fill_rect(c, &k, obg);
        }
        struct lm_rect bar = lm_rect_make(bx + 6, by + 8, 10, 2);
        fill_rect(c, &bar, obg);
    }
    {                                                   /* quick settings: three sliders (lines with knobs); the button is lit while the card shows */
        uint32_t qbg = p->quick_open ? COL_PANEL_ACCENT : mix_rgb(COL_PANEL_BTN, 0xffffffu, p->hover_quick ? 30 : 18);
        uint32_t qfg = p->quick_open ? COL_PANEL : COL_PANEL_TEXT;
        paint_drop(c, &p->quick, btn_radius(LM_MIN(p->quick.w, p->quick.h) / 2), qbg);
        int cx = p->quick.x + p->quick.w / 2, cy = p->quick.y + p->quick.h / 2;
        static const int knob[3] = { -4, 3, -1 };
        for (int i = 0; i < 3; i++) {
            struct lm_rect line = lm_rect_make(cx - 8, cy - 7 + i * 6, 16, 2);
            fill_rect(c, &line, qfg);
            struct lm_rect k = lm_rect_make(cx + knob[i] - 2, cy - 9 + i * 6, 5, 6);
            fill_round(c, &k, 2, 0xff000000u | qbg);                               /* a gap round the knob ... */
            struct lm_rect k2 = lm_rect_make(cx + knob[i] - 1, cy - 8 + i * 6, 3, 4);
            fill_round(c, &k2, 1, 0xff000000u | qfg);                              /* ... and the knob itself */
        }
    }
    draw_text_centered(c, &p->clock, p->clock_text, COL_PANEL_TEXT);
    {                                                   /* the round power button: a disc with the power symbol */
        float cx = (float)p->power.x + (float)p->power.w / 2.0f, cy = (float)p->power.y + (float)p->power.h / 2.0f;
        float R = (float)p->power.w / 2.0f;
        uint32_t bg = p->power_open ? mix_rgb(COL_PANEL_DANGER, 0xffffffu, 45) : p->hover_power ? mix_rgb(COL_PANEL_DANGER, 0xffffffu, 25) : COL_PANEL_DANGER;
        paint_round_button(c, cx, cy, R, bg, 255);                                        /* red: it ends the session */
        paint_shape(c, cx, cy, R * 0.52f, SHAPE_POWER, lm_theme.on_colour, 255);
    }
}

/* The on-screen keyboard, in the launcher's light glass: a white veil over whatever is below, round corners, pill shaped colourful keys (a colour
 * per row, the modifiers steel blue, Hide red) with white text and a 1 px shadow. The key under the pointer is lighter, the one held down
 * nearly white, a latched modifier or Caps Lock is lit and has a white rim. A small grip in the top strip (move) and in the bottom right
 * corner (resize) shows where to take hold. Single characters are drawn twice as big as the font when the keys are tall enough. */
static void paint_osk(const struct paint_ctx *c, const struct lm_osk_info *oi)
{
    fill_round(c, &oi->rect, 22, ALPHA(0x8cu, lm_theme.glass));                                  /* the rim ... */
    struct lm_rect in = lm_rect_make(oi->rect.x + 1, oi->rect.y + 1, oi->rect.w - 2, oi->rect.h - 2);
    fill_round(c, &in, 21, ALPHA(0x4cu, lm_theme.glass));                                        /* ... and the veil inside it, translucent all through */
    struct lm_rect grip = lm_rect_make(oi->rect.x + oi->rect.w / 2 - 22, oi->rect.y + 8, 44, 5);
    fill_round(c, &grip, 2, ALPHA(0xb0u, lm_theme.glass));
    for (int k = 0; k < 3; k++) {                                               /* three dots along the diagonal of the bottom right corner */
        int d = 4 + k * 4;
        struct lm_rect dot = lm_rect_make(oi->rect.x + oi->rect.w - 6 - d, oi->rect.y + oi->rect.h - 6 - (8 - d + 4), 2, 2);
        fill_rect(c, &dot, lm_theme.glass);
    }
    for (int i = 0; i < oi->n; i++) {
        const struct lm_rect *r = &oi->key[i].rect;
        uint32_t st = oi->key[i].state;
        uint32_t col = panel_palette[0];                                    /* the keys that type something: one colour ... */
        uint32_t kc = oi->key[i].code;
        if (oi->key[i].kind == LM_OSK_HIDE)
            col = panel_palette[5];
        else if (oi->key[i].kind != LM_OSK_KEY)
            col = lm_theme.key_modifier;                                                /* ... the modifiers steel blue ... */
        else if (kc == KEY_ESC || (kc >= KEY_F1 && kc <= KEY_F10) || kc == KEY_F11 || kc == KEY_F12)
            col = panel_palette[2];                                         /* ... and the others by what they do */
        else if (kc == KEY_TAB || kc == KEY_BACKSPACE || kc == KEY_ENTER || kc == KEY_DELETE)
            col = panel_palette[3];
        else if (kc == KEY_LEFT || kc == KEY_RIGHT || kc == KEY_UP || kc == KEY_DOWN)
            col = panel_palette[4];
        uint32_t bg = col;
        if (st & LM_OSK_KEY_HOVER)
            bg = mix_rgb(bg, 0xffffffu, 55);
        if (st & LM_OSK_KEY_ON)
            bg = mix_rgb(col, 0xffffffu, 80);
        if (st & LM_OSK_KEY_DOWN)
            bg = mix_rgb(col, 0xffffffu, 170);
        int rad = btn_radius(r->h / 2);                                     /* [windows] button-roundness, like every other button */
        if (st & LM_OSK_KEY_ON) {
            struct lm_rect rim = lm_rect_make(r->x - 2, r->y - 2, r->w + 4, r->h + 4);
            fill_round(c, &rim, rad + 2, ALPHA(0xf4u, lm_theme.glass));
        }
        paint_drop(c, r, rad, bg);                                          /* every key is a drop of water */
        uint32_t fg = (st & LM_OSK_KEY_DOWN) ? lm_theme.ink : lm_theme.on_colour;
        const char *t = oi->key[i].label;
        enum shape arrow = oi->key[i].code == KEY_LEFT ? SHAPE_LEFT : oi->key[i].code == KEY_RIGHT ? SHAPE_RIGHT :
                           oi->key[i].code == KEY_UP ? SHAPE_UP : oi->key[i].code == KEY_DOWN ? SHAPE_DOWN : SHAPE_DISC;
        if (arrow != SHAPE_DISC) {                                          /* the arrow keys carry a drawn arrow instead of a letter */
            float cx = (float)r->x + (float)r->w / 2.0f, cy = (float)r->y + (float)r->h / 2.0f;
            float R = (float)LM_MIN(r->w, r->h) * 0.34f;
            paint_shape(c, cx + 1.0f, cy + 1.0f, R, arrow, lm_theme.text_shadow, 0xa0);   /* the same 1 px shadow as the letters */
            paint_shape(c, cx, cy, R, arrow, fg, 0xff);
            continue;
        }
        int chars = 0;
        for (const char *q = t; *q; q++)
            chars += ((unsigned char)*q & 0xc0) != 0x80;
        if (chars == 1 && r->h >= 40) {
            int tx = r->x + (r->w - lm_font_text_width(t) * 2) / 2, ty = r->y + (r->h - LM_FONT_H * 2) / 2;
            draw_text_scaled(c, tx + 1, ty + 1, t, 2, lm_theme.text_shadow);            /* a 1 px shadow keeps white on the lighter colours readable */
            draw_text_scaled(c, tx, ty, t, 2, fg);
            draw_text_scaled(c, tx + 1, ty, t, 2, fg);                      /* bold: the glyph struck twice, a pixel apart */
        } else {
            struct lm_rect sh = *r;
            sh.x++;
            sh.y++;
            draw_text_centered(c, &sh, t, lm_theme.text_shadow);
            draw_text_centered(c, r, t, fg);
            struct lm_rect bold = *r;
            bold.x++;
            draw_text_centered(c, &bold, t, fg);
        }
    }
}


/* ---- the desktop overview ------------------------------------------------------------------------------------------------ */

/* The workspace strip, the frame of the chosen / hovered picture and the labels (the dimming and the pictures themselves are painted apart). */
static void paint_overview_ui(const struct paint_ctx *c, const struct lm_overview_info *ov)
{
    if (!ov->ui)
        return;
    for (int i = 0; i < LM_NUM_WORKSPACES; i++) {
        const struct lm_rect *r = &ov->ws[i].rect;
        fill_round(c, r, 10, ov->ws[i].active ? ALPHA(0xf0, lm_theme.accent) : ov->ws[i].hover ? ALPHA(0xc0, lm_theme.button_hover) : ALPHA(0x90, lm_theme.button));
        char num[8];
        snprintf(num, sizeof(num), "%d", i + 1);
        uint32_t fg = ov->ws[i].active ? lm_theme.background : lm_theme.text;
        draw_text_centered(c, r, num, fg);
        if (ov->ws[i].count > 0) {                                                      /* a dot per window (up to 6) */
            int n = LM_MIN(ov->ws[i].count, 6), x = r->x + (r->w - (n * 8 - 3)) / 2;
            for (int k = 0; k < n; k++) {
                struct lm_rect d = lm_rect_make(x + k * 8, r->y + r->h - 12, 5, 5);
                fill_round(c, &d, 2, ALPHA(0xff, fg));
            }
        }
    }
    for (int i = 0; i < ov->n; i++) {
        if (ov->item[i].sel)
            fill_round(c, &ov->item[i].frame, 10, ALPHA(0xe8, lm_theme.accent));
        else if (ov->item[i].hover)
            fill_round(c, &ov->item[i].frame, 10, ALPHA(0x58, lm_theme.glass));
        char fit[96];
        fit_text(fit, sizeof(fit), ov->item[i].title, ov->item[i].label.w - 8, false);
        const struct lm_rect *lb = &ov->item[i].label;
        struct lm_rect sh = *lb;
        sh.x++;
        sh.y++;
        draw_text_centered(c, &sh, fit, lm_theme.glass_shadow);
        draw_text_centered(c, lb, fit, ov->item[i].minimized ? lm_theme.text_dim : lm_theme.glass_text);
    }
}

static void paint_overview(const struct paint_ctx *c, const struct lm_overview_info *ov)
{
    if (ov->dim > 0)
        fill_round(c, &ov->rect, 0, ALPHA(ov->dim, lm_theme.ink));
    paint_overview_ui(c, ov);
}

/* ---- surface blitting ------------------------------------------------------------------------ */

/*
 * One row of a translucent (premultiplied ARGB) texture over the framebuffer. Windows are almost always fully opaque
 * even when their buffer format has an alpha channel (Firefox, GTK), so groups of 4 pixels whose alpha is all 0xff are copied
 * with one store, all-transparent groups are skipped, and only the mixed ones go through the scalar blend.
 */
static void blit_row_argb(uint32_t *d, const uint32_t *s, int n)
{
    int x = 0;
#ifdef __SSE2__
    const __m128i amask = _mm_set1_epi32((int)0xff000000u), rgb = _mm_set1_epi32(0x00ffffff), zero = _mm_setzero_si128();
    for (; x + 4 <= n; x += 4) {
        __m128i v = _mm_loadu_si128((const __m128i *)(const void *)(s + x));
        __m128i a = _mm_and_si128(v, amask);
        if (_mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(a, amask))) == 0xf) {
            _mm_storeu_si128((__m128i *)(void *)(d + x), _mm_and_si128(v, rgb));
            continue;
        }
        if (_mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(a, zero))) == 0xf)
            continue;
        for (int k = 0; k < 4; k++) {
            uint32_t p = s[x + k], al = p >> 24;
            if (al == 255)
                d[x + k] = p & 0x00ffffffu;
            else if (al)
                d[x + k] = blend_px(p, d[x + k]);
        }
    }
#endif
    for (; x < n; x++) {
        uint32_t p = s[x], al = p >> 24;
        if (al == 255)
            d[x] = p & 0x00ffffffu;
        else if (al)
            d[x] = blend_px(p, d[x]);
    }
}

static bool region_covers(const struct lm_region *rg, const struct lm_rect *r);

static void blit_surface_1(const struct paint_ctx *c, struct lm_surface *s, int32_t ox, int32_t oy)
{
    if (!s->has_content || !s->tex.pixels)
        return;
    struct lm_rect dst = lm_rect_make(ox, oy, s->width, s->height), r;
    if (!lm_rect_intersect(&dst, &c->clip, &r))
        return;
    const struct lm_texture *t = &s->tex;
    int scale = s->scale > 0 ? s->scale : 1;
    struct lm_rect local = lm_rect_make(r.x - ox, r.y - oy, r.w, r.h);
    bool force_solid = scale == 1 && region_covers(&s->opaque, &local);      /* the client promised these pixels are opaque */
    for (int y = r.y; y < r.y + r.h; y++) {
        uint32_t *drow = fb_row(c, y);
        int sy = (y - oy) * scale;
        if (sy >= t->height)
            break;
        const uint32_t *srow = t->pixels + (size_t)sy * (size_t)(t->stride / 4);
        if (scale == 1 && (t->opaque || t->solid || force_solid)) {
            memcpy(drow + r.x, srow + (r.x - ox), (size_t)r.w * 4);
            continue;
        }
        if (scale == 1) {                                       /* translucent texture, 1:1: 4 pixels at a time */
            blit_row_argb(drow + r.x, srow + (r.x - ox), r.w);
            continue;
        }
        for (int x = r.x; x < r.x + r.w; x++) {
            int sx = (x - ox) * scale;
            if (sx >= t->width)
                break;
            uint32_t p = srow[sx];
            uint32_t a = p >> 24;
            if (a == 255 || t->opaque)
                drow[x] = p & 0x00ffffffu;
            else if (a)
                drow[x] = blend_px(p, drow[x]);
        }
    }
}


/* A client surface at its logical place (ox, oy): copied 1:1 at scale 1; at another scale the buffer is mapped onto the
 * physical pixels the surface covers (1:1 when the client draws at exactly the output's scale, else bilinear). */
static void blit_surface(const struct paint_ctx *c, struct lm_surface *s, int32_t ox, int32_t oy)
{
    if (c->s == 1.0 && c->fbx == 0 && c->fby == 0) {
        blit_surface_1(c, s, ox, oy);
        return;
    }
    if (!s->has_content || !s->tex.pixels)
        return;
    struct lm_rect dst = lm_rect_make(ox, oy, s->width, s->height);
    if (!lm_rect_overlaps(&dst, &c->clip))
        return;
    struct paint_ctx pc;
    pview(c, &pc);
    struct lm_rect pd = prect(c, &dst), whole = lm_rect_make(0, 0, s->tex.width, s->tex.height);
    blit_scaled_px(&pc, s->tex.pixels, s->tex.stride / 4, s->tex.width, s->tex.height, &whole, &pd, 255, s->tex.opaque);
}

static bool visit_blit(struct lm_surface *s, int32_t x, int32_t y, void *data)
{
    blit_surface(data, s, x, y);
    return true;
}

/* ---- cursor ----------------------------------------------------------------------------------- */

struct cursor_view {
    bool visible;
    struct lm_rect rect;                /* global */
    const struct sprite *sprite;        /* built-in */
    struct lm_surface *surface;         /* client cursor */
    int32_t origin_x, origin_y;         /* surface origin, global */
    int32_t ix, iy;                     /* the pointer, global (logical) */
    int k;                              /* the built-in sprite is drawn k times as big (the pointer's output scale, rounded) */
};

static void cursor_view(struct lm_renderer *r, struct cursor_view *v)
{
    memset(v, 0, sizeof(*v));
    if (!r->srv->seat)
        return;
    struct lm_cursor cur;
    double px, py;
    lm_seat_get_cursor(r->srv->seat, &cur);
    lm_seat_pointer_position(r->srv->seat, &px, &py);
    if (cur.shape == LM_CURSOR_HIDDEN || cur.shape == LM_CURSOR_NONE)
        return;
    int32_t ix = (int32_t)px, iy = (int32_t)py;
    struct lm_output *po = lm_output_nearest(r->srv, px, py);
    double ps = po ? lm_output_scalef(po) : 1.0;
    v->ix = ix;
    v->iy = iy;
    v->k = LM_MAX(sround(ps), 1);
    if (cur.shape == LM_CURSOR_SURFACE && cur.surface) {
        struct lm_rect ext;
        if (!lm_surface_tree_extents(cur.surface, &ext))
            return;
        v->surface = cur.surface;
        v->origin_x = ix - cur.hot_x;
        v->origin_y = iy - cur.hot_y;
        v->rect = lm_rect_make(v->origin_x + ext.x, v->origin_y + ext.y, ext.w, ext.h);
        v->visible = true;
        return;
    }
    enum lm_cursor_shape shape = cur.shape;
    if (shape < LM_CURSOR_DEFAULT || shape > LM_CURSOR_POINTER || shape == LM_CURSOR_SURFACE)
        shape = LM_CURSOR_DEFAULT;
    sprite_build(shape);
    v->sprite = &sprites[shape];
    if (ps == 1.0) {
        v->rect = lm_rect_make(ix - v->sprite->hot_x, iy - v->sprite->hot_y, SPR_W, SPR_H);
    } else {                                                            /* the logical box that covers the k times bigger sprite */
        int hx = v->sprite->hot_x * v->k, hy = v->sprite->hot_y * v->k;
        v->rect = lm_rect_make(ix - (int)ceil(hx / ps) - 1, iy - (int)ceil(hy / ps) - 1, (int)ceil(SPR_W * v->k / ps) + 3,
                               (int)ceil(SPR_H * v->k / ps) + 3);
    }
    v->visible = true;
}

static void paint_cursor(const struct paint_ctx *c)
{
    struct cursor_view v;
    cursor_view(c->r, &v);
    if (!v.visible || !lm_rect_overlaps(&v.rect, &c->clip))
        return;
    if (v.surface) {
        lm_surface_tree_visit(v.surface, v.origin_x, v.origin_y, visit_blit, (void *)c);
        return;
    }
    struct paint_ctx pc;
    pview(c, &pc);
    int k = c->s == 1.0 ? 1 : v.k;
    int x0 = sround((double)(v.ix - c->ox) * c->s) - v.sprite->hot_x * k, y0 = sround((double)(v.iy - c->oy) * c->s) - v.sprite->hot_y * k;
    for (int y = 0; y < SPR_H * k; y++) {
        int py = y0 + y;
        if (py < pc.clip.y || py >= pc.clip.y + pc.clip.h)
            continue;
        uint32_t *row = fb_row(&pc, py);
        for (int x = 0; x < SPR_W * k; x++) {
            int px = x0 + x;
            if (px < pc.clip.x || px >= pc.clip.x + pc.clip.w)
                continue;
            uint32_t p = v.sprite->px[(y / k) * SPR_W + x / k];
            if (p)
                row[px] = blend_px(p, row[px]);
        }
    }
}

/* ---- painting a rectangle of an output ------------------------------------------------------- */

/*
 * Occlusion: when an opaque surface covers the whole rectangle being repainted, nothing underneath it can show, so the
 * wallpaper, the gadgets, the frames and the windows below it are not painted at all (the first pass finds the topmost
 * such surface, the second pass paints from there). Without this every frame of a full-screen video or Firefox window
 * first painted the wallpaper and the window's background and then overwrote them.
 */
/* True when one rectangle of `rg` (surface-local) contains `r`. */
static bool region_covers(const struct lm_region *rg, const struct lm_rect *r)
{
    for (int i = 0; i < rg->n; i++) {
        const struct lm_rect *q = &rg->rects[i];
        if (q->x <= r->x && q->y <= r->y && q->x + q->w >= r->x + r->w && q->y + q->h >= r->y + r->h)
            return true;
    }
    return false;
}

/*
 * The largest rectangle of `clip` that surface `s` (at ox, oy) paints opaquely: the whole surface when its texture has no
 * alpha (or never used it), else the biggest rectangle of the opaque region the client declared
 * (wl_surface.set_opaque_region: GTK/Firefox windows have transparent corners and say so). Area 0 = none.
 */
static int64_t surface_opaque_rect(const struct lm_surface *s, int32_t ox, int32_t oy, const struct lm_rect *clip,
                                   struct lm_rect *out)
{
    *out = (struct lm_rect){ 0 };
    if (!s || !s->has_content || (!s->tex.pixels && !s->gl_dmabuf))
        return 0;
    int64_t best = 0;
    if (s->tex.opaque || s->tex.solid) {
        struct lm_rect whole = lm_rect_make(ox, oy, s->width, s->height), r;
        if (lm_rect_intersect(&whole, clip, &r) && (int64_t)r.w * r.h > best) {
            best = (int64_t)r.w * r.h;
            *out = r;
        }
        return best;
    }
    for (int i = 0; i < s->opaque.n; i++) {
        struct lm_rect q = lm_rect_make(s->opaque.rects[i].x + ox, s->opaque.rects[i].y + oy, s->opaque.rects[i].w,
                                        s->opaque.rects[i].h), r;
        if (lm_rect_intersect(&q, clip, &r) && (int64_t)r.w * r.h > best) {
            best = (int64_t)r.w * r.h;
            *out = r;
        }
    }
    return best;
}

/* First pass: the topmost scene item with an opaque rectangle worth the trouble (at least half of the area being repainted). */
static bool cover_cb(const struct lm_scene_item *item, void *data)
{
    struct paint_ctx *c = data;
    if (item->kind == LM_SCENE_SURFACE) {
        struct lm_rect r;
        int64_t area = surface_opaque_rect(item->u.surface.root, item->u.surface.x, item->u.surface.y, &c->clip, &r);
        if (area > 0 && area * 2 >= (int64_t)c->clip.w * c->clip.h && area >= c->cov_area) {
            c->cov_idx = c->idx;
            c->cov_root = item->u.surface.root;
            c->cov_rect = r;
            c->cov_area = area;
        }
    }
    c->idx++;
    return true;
}

static void paint_ghost(const struct paint_ctx *c, const struct lm_ghost *g, const struct lm_rect *src, const struct lm_rect *dst, int alpha);

static bool scene_cb(const struct lm_scene_item *item, void *data)
{
    struct paint_ctx *c = data;
    if (c->skip_until) {
        if (item->kind != LM_SCENE_SURFACE || item->u.surface.root != c->skip_until)
            return true;
        c->skip_until = NULL;
    }
    switch (item->kind) {
    case LM_SCENE_WALLPAPER:
        if (item->output == c->out)
            paint_wallpaper(c);
        break;
    case LM_SCENE_FRAME:
        paint_frame(c, &item->u.frame);
        break;
    case LM_SCENE_SURFACE:
        lm_surface_tree_visit(item->u.surface.root, item->u.surface.x, item->u.surface.y,
                              visit_blit, c);
        break;
    case LM_SCENE_OVERVIEW:
        if (item->u.overview.rect.x == c->out->x && item->u.overview.rect.y == c->out->y)
            paint_overview(c, &item->u.overview);
        break;
    case LM_SCENE_GHOST:
        paint_ghost(c, item->u.ghost.ghost, &item->u.ghost.src, &item->u.ghost.dst, item->u.ghost.alpha);
        break;
    case LM_SCENE_PANEL_SHADOW:
        if (item->output == c->out)
            paint_panel_shadow(c, &item->u.shadow);
        break;
    case LM_SCENE_PANEL:
        if (item->output == c->out)
            paint_panel(c, &item->u.panel);
        break;
    case LM_SCENE_DOCK:
        if (item->output == c->out)
            paint_dock(c, &item->u.dock);
        break;
    case LM_SCENE_MENU:
        if (item->output == c->out)
            paint_menu(c, &item->u.menu);
        break;
    case LM_SCENE_ABOUT:
        if (item->output == c->out)
            paint_about(c, &item->u.about);
        break;
    case LM_SCENE_LAUNCHER:
        if (item->output == c->out || c->snapshot)
            paint_launcher(c, &item->u.launcher);
        break;
    case LM_SCENE_NOTIFY:
        paint_notify(c, &item->u.notify);
        break;
    case LM_SCENE_POWER:
        if (item->output == c->out)
            paint_power(c, &item->u.power);
        break;
    case LM_SCENE_OSK:
        if (item->output == c->out)
            paint_osk(c, &item->u.osk);
        break;
    case LM_SCENE_SWITCHER:
        paint_switcher(c, &item->u.switcher);
        break;
    case LM_SCENE_LOCK:
        if (item->output == c->out)
            paint_lock(c, &item->u.lock);
        break;
    }
    return true;
}


/* ---- ghosts: a window that opens, closes or is minimized ----------------------------------------------------------- */

/* One channel of the four neighbours, weights in 1/65536. */
#define LERP4(sh, p00, p10, p01, p11, w00, w10, w01, w11) \
    (((((p00) >> (sh)) & 255u) * (w00) + (((p10) >> (sh)) & 255u) * (w10) + (((p01) >> (sh)) & 255u) * (w01) + \
      (((p11) >> (sh)) & 255u) * (w11) + 32768u) >> 16)

/* Paints premultiplied ARGB pixels (`px`, `stride_px` pixels a row, tw x th) from the part `src` of the picture into the
 * physical rectangle `pdst` of the physical view `pc`, at opacity `alpha` (0..255): bilinear when the sizes differ, a plain
 * blend when they match. `opaque`: the pixels have no alpha (XRGB). */
static void blit_scaled_px(const struct paint_ctx *pc, const uint32_t *px, int stride_px, int tw, int th, const struct lm_rect *src,
                           const struct lm_rect *pdst, int alpha, bool opaque)
{
    struct lm_rect r;
    if (!px || src->w <= 0 || src->h <= 0 || pdst->w <= 0 || pdst->h <= 0 || alpha <= 0 || !lm_rect_intersect(pdst, &pc->clip, &r))
        return;
    uint32_t al = (uint32_t)LM_MIN(alpha, 255);
    if (src->w == pdst->w && src->h == pdst->h && src->x >= 0 && src->y >= 0 && src->x + src->w <= tw && src->y + src->h <= th) {
        for (int y = r.y; y < r.y + r.h; y++) {                         /* 1:1 (a client that draws at the output's scale) */
            uint32_t *drow = fb_row(pc, y);
            const uint32_t *srow = px + (size_t)(src->y + y - pdst->y) * (size_t)stride_px + (size_t)(src->x + r.x - pdst->x);
            if (al == 255 && opaque) {
                memcpy(drow + r.x, srow, (size_t)r.w * 4);
                continue;
            }
            if (al == 255) {
                blit_row_argb(drow + r.x, srow, r.w);
                continue;
            }
            for (int x = 0; x < r.w; x++) {
                uint32_t p = opaque ? srow[x] | 0xff000000u : srow[x];
                if (p >> 24)
                    drow[r.x + x] = blend_px(fade_px(p, (int)al), drow[r.x + x]);
            }
        }
        return;
    }
    int *sx0 = malloc((size_t)r.w * sizeof(int) * 2);
    if (!sx0)
        return;
    int *fx = sx0 + r.w;
    for (int x = 0; x < r.w; x++) {
        float u = ((float)(r.x + x - pdst->x) + 0.5f) * (float)src->w / (float)pdst->w - 0.5f + (float)src->x;
        if (u < 0) u = 0;
        if (u > (float)(tw - 1)) u = (float)(tw - 1);
        int i = (int)u;
        sx0[x] = i;
        fx[x] = (int)((u - (float)i) * 256.0f);
    }
    for (int y = r.y; y < r.y + r.h; y++) {
        float v = ((float)(y - pdst->y) + 0.5f) * (float)src->h / (float)pdst->h - 0.5f + (float)src->y;
        if (v < 0) v = 0;
        if (v > (float)(th - 1)) v = (float)(th - 1);
        int j = (int)v, fy = (int)((v - (float)j) * 256.0f), j1 = LM_MIN(j + 1, th - 1);
        const uint32_t *row0 = px + (size_t)j * (size_t)stride_px, *row1 = px + (size_t)j1 * (size_t)stride_px;
        uint32_t *drow = fb_row(pc, y);
        for (int x = 0; x < r.w; x++) {
            int i = sx0[x], i1 = LM_MIN(i + 1, tw - 1);
            uint32_t w10 = (uint32_t)fx[x] * (uint32_t)(256 - fy), w11 = (uint32_t)fx[x] * (uint32_t)fy;
            uint32_t w00 = (uint32_t)(256 - fx[x]) * (uint32_t)(256 - fy), w01 = (uint32_t)(256 - fx[x]) * (uint32_t)fy;
            uint32_t p00 = row0[i], p10 = row0[i1], p01 = row1[i], p11 = row1[i1];
            if (opaque) {
                p00 |= 0xff000000u; p10 |= 0xff000000u; p01 |= 0xff000000u; p11 |= 0xff000000u;
            }
            uint32_t a = LERP4(24, p00, p10, p01, p11, w00, w10, w01, w11);
            if (!a)
                continue;
            uint32_t rr = LERP4(16, p00, p10, p01, p11, w00, w10, w01, w11);
            uint32_t gg = LERP4(8, p00, p10, p01, p11, w00, w10, w01, w11);
            uint32_t bb = LERP4(0, p00, p10, p01, p11, w00, w10, w01, w11);
            if (al < 255) {
                a = div255(a * al);
                rr = div255(rr * al);
                gg = div255(gg * al);
                bb = div255(bb * al);
            }
            drow[r.x + x] = blend_px((a << 24) | (rr << 16) | (gg << 8) | bb, drow[r.x + x]);
        }
    }
    free(sx0);
}

/* A texture (premultiplied ARGB, w x h) into the logical rectangle `dst` of the output, at the output's scale. */
static void blit_tex(const struct paint_ctx *c, const uint32_t *px, int stride_px, int w, int h, const struct lm_rect *dst, int alpha, bool opaque)
{
    struct paint_ctx pc;
    pview(c, &pc);
    struct lm_rect pd = prect(c, dst), whole = lm_rect_make(0, 0, w, h);
    blit_scaled_px(&pc, px, stride_px, w, h, &whole, &pd, alpha, opaque);
}

/* Paints the ghost scaled into `dst` (global) with opacity `alpha` (0..255) over the target: bilinear sampling of the premultiplied pixels. */
static void paint_ghost(const struct paint_ctx *c, const struct lm_ghost *g, const struct lm_rect *src, const struct lm_rect *dst, int alpha)
{
    struct lm_rect whole = lm_rect_make(0, 0, g ? g->w : 0, g ? g->h : 0);
    if (!src || src->w <= 0 || src->h <= 0)
        src = &whole;
    if (!g || !g->px || dst->w <= 0 || dst->h <= 0)
        return;
    struct paint_ctx pc;
    pview(c, &pc);
    struct lm_rect pd = prect(c, dst);
    blit_scaled_px(&pc, g->px, g->w, g->w, g->h, src, &pd, alpha, false);
}

static bool mag_view(struct lm_renderer *r, const struct lm_output *out, float *zoom, float *ox, float *oy);
static void lm_render_ghost_gl_free(struct lm_renderer *r, struct lm_ghost *g);      /* render_gl.inc */

void lm_render_ghost_free(struct lm_renderer *r, struct lm_ghost *g)
{
    if (!g)
        return;
    lm_render_ghost_gl_free(r, g);
    free(g->px);
    memset(g, 0, sizeof(*g));
}

bool lm_render_snapshot(struct lm_renderer *r, const struct lm_scene_item *items, int n, const struct lm_rect *rect,
                        struct lm_ghost *g)
{
    memset(g, 0, sizeof(*g));
    int w = rect->w, h = rect->h;
    if (w <= 0 || h <= 0 || !r->srv->backend || lm_list_empty(&r->srv->backend->outputs) || (int64_t)w * h > 8 * 1000 * 1000)
        return false;
    for (int i = 0; i < n; i++)                                         /* a zero-copy (dmabuf) window has no pixels to copy */
        if (items[i].kind == LM_SCENE_SURFACE && items[i].u.surface.root->gl_dmabuf)
            return false;
    size_t np = (size_t)w * (size_t)h;
    uint32_t *black = malloc(np * 4), *white = malloc(np * 4);
    if (!black || !white) {
        free(black);
        free(white);
        return false;
    }
    struct lm_output *any = lm_container_of(r->srv->backend->outputs.next, struct lm_output, link);
    for (size_t i = 0; i < np; i++) {
        black[i] = 0x000000u;
        white[i] = 0xffffffu;
    }
    uint32_t *bufs[2] = { black, white };
    for (int k = 0; k < 2; k++) {                                       /* the same trick as gl_scratch_alpha: over black and over white */
        struct lm_output fake = *any;
        fake.x = rect->x;
        fake.y = rect->y;
        fake.width = fake.mode_w = w;
        fake.height = fake.mode_h = h;
        fake.scale_milli = 1000;                                        /* the picture is taken at logical size (animations are short) */
        struct lm_framebuffer fb = { .pixels = bufs[k], .width = w, .height = h, .stride = w * 4 };
        struct paint_ctx c = { .r = r, .out = &fake, .fb = &fb, .clip = *rect, .cov_idx = -1, .snapshot = true };
        ctx_init(&c);
        for (int i = 0; i < n; i++)
            scene_cb(&items[i], &c);
    }
    for (size_t i = 0; i < np; i++) {
        uint32_t b = black[i], wh = white[i];
        int d = (int)((((wh >> 16) & 255) - ((b >> 16) & 255)) + (((wh >> 8) & 255) - ((b >> 8) & 255)) +
                      ((wh & 255) - (b & 255))) / 3;
        uint32_t a = (uint32_t)LM_CLAMP(255 - d, 0, 255);
        black[i] = (a << 24) | (LM_MIN((b >> 16) & 255, a) << 16) | (LM_MIN((b >> 8) & 255, a) << 8) | LM_MIN(b & 255, a);
    }
    free(white);
    g->px = black;
    g->w = w;
    g->h = h;
    return true;
}

static struct out_state *state_of(struct lm_output *out);

/* ---- screenshot feedback ------------------------------------------------------------------------ */

static struct lm_rect shot_card_rect(const struct shot_fx *fx)
{
    int w = fx->tw + 16, h = fx->th + 16 + LM_FONT_H + 8;
    struct lm_rect o = lm_output_rect(fx->out);
    return lm_rect_make(o.x + o.w - w - 16, o.y + o.h - h - 16, w, h);
}

static int shot_flash_alpha(const struct shot_fx *fx)
{
    return fx->ticks < SHOT_FLASH_TICKS ? (SHOT_FLASH_TICKS - fx->ticks) * 55 : 0;
}

/* The card: translucent body, thumbnail, file name. (The GL renderer paints it over black and over white: matting.) */
static void paint_shot_card(const struct paint_ctx *c, const struct shot_fx *fx)
{
    struct lm_rect card = shot_card_rect(fx);
    struct lm_rect hit;
    if (!lm_rect_intersect(&card, &c->clip, &hit))
        return;
    fill_round(c, &card, 8, (0xf2000000u | lm_theme.card));
    struct lm_rect th = lm_rect_make(card.x + 8, card.y + 8, fx->tw, fx->th);
    struct lm_rect ring = lm_rect_make(th.x - 1, th.y - 1, th.w + 2, th.h + 2);
    fill_rect(c, &ring, (0x00000000u | lm_theme.border));
    blit_tex(c, fx->thumb, fx->tw, fx->tw, fx->th, &th, 255, true);
    struct lm_rect text = lm_rect_make(card.x + 8, card.y + 8 + fx->th + 6, fx->tw, LM_FONT_H);
    draw_text(c, &text, text.x, text.y, fx->label, lm_theme.bar_text);
}

static void paint_shot(const struct paint_ctx *c)
{
    const struct shot_fx *fx = &c->r->shot;
    if (!fx->active || fx->out != c->out || (c->r->srv->lock && lm_lock_active(c->r->srv->lock)))
        return;                                         /* (never over the lock screen) */
    int a = shot_flash_alpha(fx);
    if (a > 0)
        fill_round(c, &fx->area, 0, ((uint32_t)a << 24) | 0x00ffffffu);
    paint_shot_card(c, fx);
}

static void shot_end(struct lm_renderer *r)
{
    struct shot_fx *fx = &r->shot;
    if (!fx->active)
        return;
    struct lm_rect card = shot_card_rect(fx), area = fx->area;
    fx->active = false;
    if (fx->timer)
        lm_timer_arm_ms(fx->timer, 0, 0);
    free(fx->thumb);
    fx->thumb = NULL;
    lm_render_damage(r, &card);
    lm_render_damage(r, &area);
}

static void shot_tick(void *data)
{
    struct lm_renderer *r = data;
    struct shot_fx *fx = &r->shot;
    if (!fx->active)
        return;
    bool was_flash = shot_flash_alpha(fx) > 0;
    fx->ticks++;
    if (was_flash)
        lm_render_damage(r, &fx->area);                 /* the flash got weaker (or is gone) */
    if (fx->ticks == SHOT_FLASH_TICKS)
        lm_timer_arm_ms(fx->timer, fx->hold_ms > 0 ? fx->hold_ms : 1, 0);  /* the card stays for the rest of the time */
    else if (fx->ticks > SHOT_FLASH_TICKS)
        shot_end(r);
}

bool lm_render_shot_card_at(struct lm_renderer *r, double x, double y)
{
    const struct shot_fx *fx = &r->shot;
    if (!fx->active || (r->srv->lock && lm_lock_active(r->srv->lock)))
        return false;
    struct lm_rect card = shot_card_rect(fx);
    return lm_rect_contains(&card, (int32_t)x, (int32_t)y);
}

bool lm_render_shot_dismiss(struct lm_renderer *r, char *path, size_t n)
{
    bool had = r->shot.active && r->shot.path[0];
    if (path && n)
        snprintf(path, n, "%s", had ? r->shot.path : "");
    shot_end(r);
    return had;
}

void lm_render_shot_feedback(struct lm_renderer *r, const struct lm_rect *where, const uint32_t *px, int w, int h,
                             const char *label, const char *path)
{
    if (!r->srv->backend || w <= 0 || h <= 0)
        return;
    struct lm_output *out = lm_output_nearest(r->srv, where->x + where->w / 2.0, where->y + where->h / 2.0);
    if (!out || !state_of(out))
        return;
    if (!r->shot.timer)
        r->shot.timer = lm_loop_add_timer(r->srv->loop, shot_tick, r);
    if (!r->shot.timer)
        return;
    shot_end(r);
    int tw = LM_MIN(SHOT_THUMB_W, w), th = (int)((int64_t)h * tw / w);
    if (th > SHOT_THUMB_H) {
        th = SHOT_THUMB_H;
        tw = (int)((int64_t)w * th / h);
    }
    tw = LM_MAX(tw, 1);
    th = LM_MAX(th, 1);
    uint32_t *thumb = malloc((size_t)tw * (size_t)th * 4);
    if (!thumb)
        return;
    for (int y = 0; y < th; y++) {                      /* box filter */
        int y0 = (int)((int64_t)y * h / th), y1 = LM_MAX((int)((int64_t)(y + 1) * h / th), y0 + 1);
        for (int x = 0; x < tw; x++) {
            int x0 = (int)((int64_t)x * w / tw), x1 = LM_MAX((int)((int64_t)(x + 1) * w / tw), x0 + 1);
            uint32_t sr = 0, sg = 0, sb = 0, n = 0;
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++) {
                    uint32_t v = px[(size_t)yy * (size_t)w + (size_t)xx];
                    sr += (v >> 16) & 255;
                    sg += (v >> 8) & 255;
                    sb += v & 255;
                    n++;
                }
            thumb[(size_t)y * (size_t)tw + (size_t)x] = ((sr / n) << 16) | ((sg / n) << 8) | (sb / n);
        }
    }
    struct shot_fx *fx = &r->shot;
    fx->thumb = thumb;
    fx->tw = tw;
    fx->th = th;
    fx->out = out;
    fx->area = *where;
    snprintf(fx->label, sizeof(fx->label), "%s", label ? label : "");
    snprintf(fx->path, sizeof(fx->path), "%s", path ? path : "");
    fx->ticks = 0;
    const char *ms = getenv("MELLAGUI_TOAST_MS");
    int hold = ms ? atoi(ms) : 8000;
    fx->hold_ms = LM_MAX(hold - SHOT_FLASH_TICKS * SHOT_TICK_MS, 0);
    fx->gen++;
    fx->active = true;
    lm_timer_arm_ms(fx->timer, SHOT_TICK_MS, SHOT_TICK_MS);
    struct lm_rect card = shot_card_rect(fx);
    lm_render_damage(r, &card);
    lm_render_damage(r, &fx->area);
}

static void paint_rect(struct lm_renderer *r, struct out_state *st, struct lm_framebuffer *fb,
                       const struct lm_rect *rect, bool with_overlays)
{
    struct paint_ctx c = { .r = r, .out = st->out, .fb = fb, .cov_idx = -1 };
    ctx_init(&c);
    struct lm_rect orect = lm_output_rect(st->out);
    if (!lm_rect_intersect(rect, &orect, &c.clip))
        return;
    if (!r->srv->wm) {
        paint_wallpaper(&c);
        if (with_overlays)
            paint_cursor(&c);
        return;
    }
    struct lm_rect full = c.clip;
    lm_wm_scene_for_each(r->srv->wm, &full, cover_cb, &c);
    if (c.cov_idx < 0) {                                                /* nothing covers: paint everything */
        c.idx = 0;
        c.skip_until = NULL;
        lm_wm_scene_for_each(r->srv->wm, &full, scene_cb, &c);
    } else {
        /* An opaque rectangle `cov` hides everything below its surface: inside it only the items from the cover upwards
         * are painted; the strips around it get the full stack (those are the transparent corners and edges). */
        struct lm_rect cov = c.cov_rect;
        const struct lm_surface *cov_root = c.cov_root;
        struct lm_rect strips[4] = {
            lm_rect_make(full.x, full.y, full.w, cov.y - full.y),                                  /* above */
            lm_rect_make(full.x, cov.y + cov.h, full.w, full.y + full.h - (cov.y + cov.h)),        /* below */
            lm_rect_make(full.x, cov.y, cov.x - full.x, cov.h),                                    /* left */
            lm_rect_make(cov.x + cov.w, cov.y, full.x + full.w - (cov.x + cov.w), cov.h),          /* right */
        };
        for (int i = 0; i < 4; i++) {
            if (strips[i].w <= 0 || strips[i].h <= 0)
                continue;
            c.clip = strips[i];
            c.idx = 0;
            c.skip_until = NULL;
            lm_wm_scene_for_each(r->srv->wm, &c.clip, scene_cb, &c);
        }
        c.clip = cov;
        c.idx = 0;
        c.skip_until = cov_root;
        lm_wm_scene_for_each(r->srv->wm, &c.clip, scene_cb, &c);
    }
    c.clip = full;
    if (with_overlays) {
        paint_shot(&c);
        paint_cursor(&c);
    }
}

/* ---- damage & scheduling -------------------------------------------------------------------- */

static struct out_state *state_of(struct lm_output *out)
{
    return out->render_data;
}

static void send_frame_done(struct lm_renderer *r, uint32_t t)
{
    if (r->srv->wm)
        lm_wm_send_frame_done(r->srv->wm, t);
    if (r->srv->seat)
        lm_seat_send_frame_done(r->srv->seat, t);
}

#include "render_gl.inc"

/* Returns true if a frame was presented. */
/* The screen magnifier: the output is shown zoomed by config.magnifier_zoom round the pointer. The point under the pointer stays where it is (the view's origin is
 * pointer * (1 - 1/zoom)), so the pointer keeps pointing at what it points at and nothing has to scroll. Only the output the pointer is on is magnified. */
static bool mag_view(struct lm_renderer *r, const struct lm_output *out, float *zoom, float *ox, float *oy)
{
    const struct lm_config *cfg = &r->srv->config;
    if (!cfg->magnifier || cfg->magnifier_zoom <= 100 || !r->srv->seat)
        return false;
    double px, py;
    lm_seat_pointer_position(r->srv->seat, &px, &py);
    if (lm_output_nearest(r->srv, px, py) != out)
        return false;
    float z = (float)cfg->magnifier_zoom / 100.0f, k = 1.0f - 1.0f / z;
    *zoom = z;
    float sc = (float)lm_output_scalef(out);                            /* the origin is in framebuffer (physical) pixels */
    *ox = (float)(px - out->x) * sc * k;
    *oy = (float)(py - out->y) * sc * k;
    return true;
}

/* A frame of the magnified output (software): the whole scene into a scratch picture, then a nearest-neighbour zoom into the framebuffer. */
static bool draw_output_mag(struct lm_renderer *r, struct out_state *st, struct lm_framebuffer *fb, float zoom, float mox, float moy)
{
    struct lm_output *out = st->out;
    int w = lm_output_pw(out), h = lm_output_ph(out);
    if (!st->mag_px || st->mag_w != w || st->mag_h != h) {
        free(st->mag_px);
        st->mag_px = calloc((size_t)w * (size_t)h, 4);
        st->mag_w = w;
        st->mag_h = h;
    }
    if (!st->mag_px)
        return false;
    struct lm_framebuffer scratch = { .pixels = st->mag_px, .width = w, .height = h, .stride = w * 4, .age = 0 };
    struct lm_rect orect = lm_output_rect(out);
    paint_rect(r, st, &scratch, &orect, true);
    int *xmap = malloc((size_t)w * sizeof(int));
    if (!xmap)
        return false;
    for (int x = 0; x < w; x++) {
        int sx = (int)(mox + ((float)x + 0.5f) / zoom);
        xmap[x] = sx < 0 ? 0 : sx >= w ? w - 1 : sx;
    }
    for (int y = 0; y < h; y++) {
        int sy = (int)(moy + ((float)y + 0.5f) / zoom);
        sy = sy < 0 ? 0 : sy >= h ? h - 1 : sy;
        const uint32_t *src = st->mag_px + (size_t)sy * (size_t)w;
        uint32_t *dst = (uint32_t *)(void *)((uint8_t *)fb->pixels + (size_t)y * (size_t)fb->stride);
        for (int x = 0; x < w; x++)
            dst[x] = src[xmap[x]];
    }
    free(xmap);
    lm_output_present(out, NULL, 0);
    for (int i = 0; i < HIST; i++) {                    /* (every frame is whole while it is on, so there is no damage history to keep) */
        lm_region_clear(&st->hist[i]);
    }
    lm_region_clear(&st->pending);
    st->full = false;
    st->ready = false;
    st->presented = true;
    return true;
}

static bool draw_output(struct lm_renderer *r, struct out_state *st, bool force)
{
    if (r->gl)
        return draw_output_gl(r, st, force);
    struct lm_output *out = st->out;
    if (!force && !st->ready)
        return false;
    bool has_damage = !lm_region_is_empty(&st->pending) || st->full;
    if (!has_damage)
        return false;
    struct lm_framebuffer *fb = lm_output_get_framebuffer(out);
    if (!fb || !fb->pixels)
        return false;
    {
        float mz, mx, my;
        if (mag_view(r, out, &mz, &mx, &my))
            return draw_output_mag(r, st, fb, mz, mx, my);
    }

    struct lm_rect orect = lm_output_rect(out);
    struct lm_region redraw;
    lm_region_init(&redraw);
    bool full = st->full || fb->age <= 0 || fb->age > HIST + 1;
    if (full) {
        lm_region_add_rect(&redraw, &orect);
    } else {
        lm_region_add_region(&redraw, &st->pending);
        for (int i = 0; i < fb->age - 1 && i < HIST; i++)
            lm_region_add_region(&redraw, &st->hist[i]);
    }
    if (redraw.n > MAX_REDRAW_RECTS) {
        struct lm_rect e;
        lm_region_extents(&redraw, &e);
        lm_region_clear(&redraw);
        lm_region_add_rect(&redraw, &e);
    }
    lm_region_intersect_rect(&redraw, &orect);

    for (int i = 0; i < redraw.n; i++)
        paint_rect(r, st, fb, &redraw.rects[i], true);
    if (lm_rect_is_empty(&r->cursor_rect)) {            /* the first frame drew the cursor: remember where, so that it is erased later */
        struct cursor_view v;
        cursor_view(r, &v);
        if (v.visible)
            r->cursor_rect = v.rect;
    }

    /* Present with output-local damage. */
    struct lm_rect *local = malloc(sizeof(*local) * (size_t)(redraw.n ? redraw.n : 1));
    int nlocal = 0;
    if (local && !full) {
        for (int i = 0; i < redraw.n; i++)
            local[nlocal++] = out_prect(out, &redraw.rects[i]);          /* the backend counts in framebuffer pixels */
    }
    lm_output_present(out, (local && nlocal) ? local : NULL, nlocal);
    free(local);

    /* Rotate history; the frame's own change set becomes hist[0]. */
    lm_region_fini(&st->hist[HIST - 1]);
    for (int i = HIST - 1; i > 0; i--)
        st->hist[i] = st->hist[i - 1];
    lm_region_init(&st->hist[0]);
    lm_region_copy(&st->hist[0], &st->pending);
    lm_region_clear(&st->pending);
    lm_region_fini(&redraw);
    st->full = false;
    st->ready = false;
    st->presented = true;
    return true;
}

static uint32_t *gl_capture(struct lm_renderer *r, struct out_state *st, const struct lm_rect *clip, int *w, int *h);

uint32_t *lm_render_capture(struct lm_renderer *r, const struct lm_rect *rect, int *w, int *h)
{
    return lm_render_capture_cursor(r, rect, w, h, false);
}

uint32_t *lm_render_capture_cursor(struct lm_renderer *r, const struct lm_rect *rect, int *w, int *h, bool cursor)
{
    if (!r->srv->backend || rect->w <= 0 || rect->h <= 0)
        return NULL;
    struct lm_output *out = lm_output_nearest(r->srv, rect->x + rect->w / 2.0, rect->y + rect->h / 2.0);
    struct out_state *st = out ? state_of(out) : NULL;
    struct lm_rect orect, clip;
    if (!st)
        return NULL;
    orect = lm_output_rect(out);
    if (!lm_rect_intersect(rect, &orect, &clip))
        return NULL;
    bool whole_out = cursor && clip.x == orect.x && clip.y == orect.y && clip.w == orect.w && clip.h == orect.h;
    if (r->gl) {                                        /* zero-copy surfaces have no CPU pixels: the GPU renders the shot */
        uint32_t *g = gl_capture(r, st, &clip, w, h);
        if (g && whole_out) {                               /* (the pointer is not in the GL picture: it is put on top of what was read back) */
            struct lm_framebuffer gfb = { .pixels = g, .width = lm_output_pw(out), .height = lm_output_ph(out), .stride = (int32_t)lm_output_pw(out) * 4 };
            struct paint_ctx gc = { .r = r, .out = out, .fb = &gfb, .cov_idx = -1, .clip = orect };
            ctx_init(&gc);
            paint_cursor(&gc);
        }
        return g;
    }
    /* The picture has the output's PHYSICAL pixels: a shot of a 2x screen is twice as wide as its logical size. */
    struct lm_rect pclip = out_prect(out, &clip);
    size_t stride = (size_t)lm_output_pw(out) * 4;
    uint32_t *whole = calloc((size_t)lm_output_ph(out), stride);
    uint32_t *res = malloc((size_t)pclip.w * (size_t)pclip.h * 4);
    if (!whole || !res) {
        free(whole);
        free(res);
        return NULL;
    }
    struct lm_framebuffer fb = { .pixels = whole, .width = lm_output_pw(out), .height = lm_output_ph(out), .stride = (int32_t)stride };
    paint_rect(r, st, &fb, &clip, false);
    if (whole_out) {
        struct paint_ctx cc = { .r = r, .out = out, .fb = &fb, .cov_idx = -1, .clip = orect };
        ctx_init(&cc);
        paint_cursor(&cc);
    }
    for (int y = 0; y < pclip.h; y++)
        memcpy(res + (size_t)y * (size_t)pclip.w,
               (const uint8_t *)whole + (size_t)(pclip.y + y) * stride + (size_t)pclip.x * 4,
               (size_t)pclip.w * 4);
    free(whole);
    *w = pclip.w;
    *h = pclip.h;
    return res;
}

static void idle_draw(void *data)
{
    struct lm_renderer *r = data;
    struct lm_backend *b = r->srv->backend;
    if (!b)
        return;
    struct lm_output *o;
    lm_list_for_each(o, &b->outputs, link) {
        struct out_state *st = state_of(o);
        if (st)
            draw_output(r, st, false);
    }
}

static void frame_timer_fn(void *data)
{
    struct lm_renderer *r = data;
    send_frame_done(r, lm_now_msec32());
}

struct lm_renderer *lm_render_create(struct lm_server *srv)
{
    struct lm_renderer *r = calloc(1, sizeof(*r));
    if (!r)
        return NULL;
    r->srv = srv;
    bool headless = !strcmp(srv->config.backend, "headless");
    const char *env = getenv("MELLAGUI_RENDERER");                  /* tests: run everything through the GL path */
    int mode = srv->config.renderer;
    if (mode < 0 && env)
        mode = !strcmp(env, "gl") ? 2 : !strcmp(env, "auto") ? 1 : 0;
    if (mode < 0)
        mode = headless ? 0 : 1;
    if (mode > 0) {
        char why[200] = "";
        r->gl = headless ? lm_gl_create(-1, why, sizeof(why)) : NULL;
        r->gl_owned = r->gl != NULL;
        if (!headless) {
            /* the DRM backend makes the context (it needs its device) and the renderer picks it up in output_added */
        } else if (r->gl) {
            lm_info("renderer: OpenGL ES (%s), the GPU composites the windows", lm_gl_renderer_name(r->gl));
        } else if (mode == 2) {
            lm_error("the GL renderer is required (--renderer gl) but not usable: %s", why);
            free(r);
            return NULL;
        } else if (headless) {
            lm_info("renderer: software (GL not used: %s)", why);
        }
    }
    r->idle = lm_loop_add_idle(srv->loop, idle_draw, r);
    r->frame_timer = lm_loop_add_timer(srv->loop, frame_timer_fn, r);
    return r;
}

void lm_render_destroy(struct lm_renderer *r)
{
    if (!r)
        return;
    if (r->srv->backend) {
        struct lm_output *o;
        lm_list_for_each(o, &r->srv->backend->outputs, link)
            lm_render_output_removed(r, o);
    }
    if (r->idle)
        lm_idle_destroy(r->idle);
    if (r->frame_timer)
        lm_timer_destroy(r->frame_timer);
    if (r->shot.timer)
        lm_timer_destroy(r->shot.timer);
    free(r->shot.thumb);
    lm_img_free(&r->wall.img);
    if (r->gl) {
        lm_gl_make_current(r->gl);
        for (int i = 0; i <= LM_CURSOR_POINTER; i++)
            if (r->sprite_tex[i])
                lm_gl_tex_delete(r->gl, r->sprite_tex[i]);
        if (r->gl_owned)
            lm_gl_destroy(r->gl);
    }
    free(r);
}

void lm_render_output_added(struct lm_renderer *r, struct lm_output *out)
{
    if (!r->gl && out->backend && out->backend->gl) {               /* the DRM backend renders with GL: scanout through GBM */
        r->gl = out->backend->gl;
        r->gl_owned = false;
        lm_info("renderer: OpenGL ES, the GPU composites the windows and scans them out");
    }
    struct out_state *st = calloc(1, sizeof(*st));
    if (!st)
        return;
    st->out = out;
    lm_region_init(&st->pending);
    for (int i = 0; i < HIST; i++)
        lm_region_init(&st->hist[i]);
    st->full = true;
    out->render_data = st;
    lm_idle_schedule(r->idle);
}

void lm_render_output_changed(struct lm_renderer *r, struct lm_output *out)
{
    struct out_state *st = state_of(out);
    if (!st)
        return;
    st->full = true;
    for (int i = 0; i < HIST; i++)
        lm_region_clear(&st->hist[i]);
    lm_idle_schedule(r->idle);
}

void lm_render_output_removed(struct lm_renderer *r, struct lm_output *out)
{
    if (r->shot.active && r->shot.out == out) {         /* no damage: the output is going away */
        r->shot.active = false;
        free(r->shot.thumb);
        r->shot.thumb = NULL;
        if (r->shot.timer)
            lm_timer_arm_ms(r->shot.timer, 0, 0);
    }
    struct out_state *st = state_of(out);
    if (!st)
        return;
    if (r->gl)
        gl_out_free(r, st);
    free(st->wall_px);
    free(st->lock_px);
    free(st->mag_px);
    lm_region_fini(&st->pending);
    for (int i = 0; i < HIST; i++)
        lm_region_fini(&st->hist[i]);
    free(st);
    out->render_data = NULL;
}

void lm_render_damage(struct lm_renderer *r, const struct lm_rect *rect)
{
    struct lm_backend *b = r->srv->backend;
    if (!b || lm_rect_is_empty(rect))
        return;
    struct lm_rect in = *rect;
    if (!lm_rect_sanitize(&in))
        return;
    struct lm_output *o;
    lm_list_for_each(o, &b->outputs, link) {
        struct out_state *st = state_of(o);
        struct lm_rect orect = lm_output_rect(o), x;
        if (st && lm_rect_intersect(&in, &orect, &x))
            lm_region_add_rect(&st->pending, &x);
    }
    lm_idle_schedule(r->idle);
}

void lm_render_damage_all(struct lm_renderer *r)
{
    struct lm_backend *b = r->srv->backend;
    if (!b)
        return;
    struct lm_output *o;
    lm_list_for_each(o, &b->outputs, link) {
        struct out_state *st = state_of(o);
        if (st)
            st->full = true;
    }
    lm_idle_schedule(r->idle);
}

void lm_render_cursor_changed(struct lm_renderer *r)
{
    if (r->srv->config.magnifier && r->srv->config.magnifier_zoom > 100 && r->srv->seat && r->srv->backend) {       /* the view follows the pointer: the whole output */
        double px, py;
        lm_seat_pointer_position(r->srv->seat, &px, &py);
        struct lm_output *po = lm_output_nearest(r->srv, px, py);
        if (r->mag_out && r->mag_out != po) {                           /* the pointer left an output: it shows its own picture again, all of it */
            struct lm_output *o;
            lm_list_for_each(o, &r->srv->backend->outputs, link)
                if (o == r->mag_out) {
                    struct lm_rect old = lm_output_rect(o);
                    lm_render_damage(r, &old);
                }
        }
        r->mag_out = po;
        if (po) {
            struct lm_rect all = lm_output_rect(po);
            lm_render_damage(r, &all);
        }
    } else {
        r->mag_out = NULL;
    }
    struct cursor_view v;
    cursor_view(r, &v);
    struct lm_rect old = r->cursor_rect;
    r->cursor_rect = v.visible ? v.rect : lm_rect_make(0, 0, 0, 0);
    if (!lm_rect_is_empty(&old))
        lm_render_damage(r, &old);
    if (!lm_rect_is_empty(&r->cursor_rect))
        lm_render_damage(r, &r->cursor_rect);
}

void lm_render_request_frame(struct lm_renderer *r)
{
    if (r->frame_timer && !lm_timer_is_armed(r->frame_timer))
        lm_timer_arm_ms(r->frame_timer, 16, 0);
}

void lm_render_output_ready(struct lm_renderer *r, struct lm_output *out, uint32_t time_ms)
{
    struct out_state *st = state_of(out);
    if (!st)
        return;
    st->ready = true;
    if (st->presented) {
        st->presented = false;
        send_frame_done(r, time_ms);
    }
    draw_output(r, st, false);
}

void lm_render_flush(struct lm_renderer *r)
{
    struct lm_backend *b = r->srv->backend;
    if (!b)
        return;
    struct lm_output *o;
    lm_list_for_each(o, &b->outputs, link) {
        struct out_state *st = state_of(o);
        if (st)
            draw_output(r, st, true);
    }
}

void lm_render_surface_gone(struct lm_renderer *r, struct lm_surface *s)
{
    if (r->gl && (s->gl_tex || s->gl_img)) {
        lm_gl_make_current(r->gl);
        if (s->gl_img)
            lm_gl_image_destroy(r->gl, s->gl_img);
        if (s->gl_tex)
            lm_gl_tex_delete(r->gl, s->gl_tex);
    }
    s->gl_tex = 0;
    s->gl_img = NULL;
}

struct lm_gl *lm_render_gl(struct lm_renderer *r)
{
    return r ? r->gl : NULL;
}

void lm_render_set_wallpaper(struct lm_renderer *r, const char *path, int mode)
{
    lm_img_free(&r->wall.img);
    r->wall.status[0] = 0;
    r->wall.mode = mode;
    if (path && *path) {
        char full[512];
        const char *home = getenv("HOME");
        if (path[0] == '~' && path[1] == '/' && home && *home)
            snprintf(full, sizeof(full), "%s%s", home, path + 1);
        else
            snprintf(full, sizeof(full), "%s", path);
        char err[120];
        if (lm_img_load(full, &r->wall.img, err, sizeof(err)) == 0) {
            snprintf(r->wall.status, sizeof(r->wall.status), "ok");
            lm_info("wallpaper: %s (%dx%d)", full, r->wall.img.w, r->wall.img.h);
        } else {
            snprintf(r->wall.status, sizeof(r->wall.status), "error: %s", err);
            lm_warn("wallpaper: %s", err);
        }
    }
    r->wall.gen++;
    lm_render_damage_all(r);
}

const char *lm_render_wallpaper_status(const struct lm_renderer *r)
{
    return r->wall.status;
}
