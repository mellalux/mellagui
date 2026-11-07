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
#include "mixer.h"
#include "sysinfo.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "font.h"
#include "gl.h"
#include "log.h"
#include "lock.h"
#include "output.h"
#include "seat.h"
#include "surface.h"
#include "wm.h"
#include <stdio.h>

#define HIST 3
#define MAX_REDRAW_RECTS 12

/* ---- colour scheme (XRGB8888) --------------------------------------------------- */

#define COL_WALL_TOP     0x1b2a49u
#define COL_WALL_BOTTOM  0x3a6b72u
#define COL_FRAME_ACTIVE 0x3b4a6bu
#define COL_FRAME_IDLE   0x2a2f3au
#define COL_CONTENT_BG   0x101216u
#define COL_TEXT_ACTIVE  0xffffffu
#define COL_TEXT_IDLE    0x9aa0aeu
#define COL_BTN_HOVER    0x5a6c96u
#define COL_BTN_PRESSED  0x2c3856u
#define COL_BTN_CLOSE    0xc0392bu
#define COL_PANEL        0x14171fu
#define COL_PANEL_BTN    0x2d3a5au
#define COL_PANEL_HOVER  0x41527eu
#define COL_PANEL_ACCENT 0x6fa8ffu
#define COL_PANEL_TEXT   0xe6e9efu
#define COL_PANEL_DANGER 0xc0392bu
#define COL_PANEL_DIM    0x7b8294u

struct out_state {
    struct lm_output *out;
    struct lm_region pending;           /* global coordinates */
    struct lm_region hist[HIST];        /* damage of the last HIST presented frames */
    bool ready;                         /* backend allows drawing */
    bool presented;                     /* a frame was presented, frame-done not yet sent */
    bool full;                          /* next frame must repaint everything */
    void *gl;                           /* struct gl_out: the GL renderer's per-output state (render_gl.inc) */
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
    struct lm_gl *gl;                   /* the GL renderer; NULL = software (render_gl.inc) */
    bool gl_owned;                      /* we made it (headless); the DRM backend owns its own */
    unsigned sprite_tex[LM_CURSOR_POINTER + 1];
    struct lm_server *srv;
    struct lm_idle *idle;
    struct lm_timer *frame_timer;
    struct lm_rect cursor_rect;         /* last damaged cursor rectangle (global) */
};

struct paint_ctx {
    struct lm_renderer *r;
    struct lm_output *out;
    struct lm_framebuffer *fb;
    struct lm_rect clip;                /* global, already clipped to the output */
    int idx, skip_before;               /* scene items seen so far / items below the opaque cover (not painted) */
    int cov_idx;                        /* occlusion search: the scene item that covers `cov_rect` opaquely, -1 none */
    struct lm_rect cov_rect;
    int64_t cov_area;
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
                        (size_t)(gy - c->out->y) * (size_t)c->fb->stride) - c->out->x;
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

static void fill_rect(const struct paint_ctx *c, const struct lm_rect *rect, uint32_t color)
{
    struct lm_rect r;
    if (!lm_rect_intersect(rect, &c->clip, &r))
        return;
    for (int y = r.y; y < r.y + r.h; y++)
        fill32(fb_row(c, y) + r.x, color, (size_t)r.w);
}

static inline void put_px(const struct paint_ctx *c, int x, int y, uint32_t color)
{
    if (x < c->clip.x || y < c->clip.y || x >= c->clip.x + c->clip.w || y >= c->clip.y + c->clip.h)
        return;
    fb_row(c, y)[x] = color;
}

static void draw_text(const struct paint_ctx *c, const struct lm_rect *area, int x, int y,
                      const char *text, uint32_t color)
{
    struct lm_rect lim;
    if (!lm_rect_intersect(area, &c->clip, &lim))
        return;
    size_t len = strlen(text);
    for (size_t i = 0; i < len;) {
        int used;
        uint32_t cp = lm_utf8_decode(text + i, (int)(len - i), &used);
        i += used > 0 ? (size_t)used : 1;
        if (x + LM_FONT_W > lim.x && x < lim.x + lim.w) {
            const uint8_t *g = lm_font_glyph(cp);
            for (int row = 0; row < LM_FONT_H; row++) {
                int py = y + row;
                if (py < lim.y || py >= lim.y + lim.h)
                    continue;
                uint32_t *dst = fb_row(c, py);
                for (int col = 0; col < LM_FONT_W; col++) {
                    int px = x + col;
                    if (px < lim.x || px >= lim.x + lim.w)
                        continue;
                    if (g[row] & (0x80 >> col))
                        dst[px] = color;
                }
            }
        }
        x += LM_FONT_W;
        if (x >= lim.x + lim.w)
            break;
    }
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

static void paint_wallpaper(const struct paint_ctx *c)
{
    struct lm_rect r = c->clip;
    for (int y = r.y; y < r.y + r.h; y++) {
        uint32_t col = lerp_color(COL_WALL_TOP, COL_WALL_BOTTOM, y - c->out->y, c->out->height);
        fill32(fb_row(c, y) + r.x, col, (size_t)r.w);
    }
}

static void draw_line(const struct paint_ctx *c, int x0, int y0, int x1, int y1, uint32_t color)
{
    int dx = abs(x1 - x0), dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        put_px(c, x0, y0, color);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void draw_frame_button(const struct paint_ctx *c, const struct lm_frame_info *f,
                              const struct lm_rect *b, uint32_t which, uint32_t fg)
{
    uint32_t bg = 0;
    bool filled = false;
    if (f->pressed & which) {
        bg = (which == LM_FRAME_HOVER_CLOSE) ? 0x8e2a20u : COL_BTN_PRESSED;
        filled = true;
    } else if (f->hover & which) {
        bg = (which == LM_FRAME_HOVER_CLOSE) ? COL_BTN_CLOSE : COL_BTN_HOVER;
        filled = true;
    }
    if (filled)
        fill_rect(c, b, bg);
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
    uint32_t col;                       /* title bar and the three border strips */
    uint32_t body_col;                  /* behind the client area (shows where the client has not drawn) */
    bool has_body;
};

static void frame_parts(const struct lm_frame_info *f, struct frame_parts *p)
{
    const struct lm_rect *o = &f->outer;
    int bw = LM_BORDER_WIDTH;
    memset(p, 0, sizeof(*p));
    p->col = f->active ? COL_FRAME_ACTIVE : COL_FRAME_IDLE;
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

static void paint_menu(const struct paint_ctx *c, const struct lm_menu_info *m)
{
    fill_rect(c, &m->rect, COL_PANEL_ACCENT);                           /* 1 px border */
    struct lm_rect in = lm_rect_make(m->rect.x + 1, m->rect.y + 1, m->rect.w - 2, m->rect.h - 2);
    fill_rect(c, &in, COL_PANEL);
    for (int i = 0; i < m->n; i++) {
        struct lm_rect r = lm_rect_make(m->rect.x + 2, m->rect.y + 2 + i * m->item_h, m->rect.w - 4, m->item_h);
        if (i == m->hover)
            fill_rect(c, &r, COL_PANEL_HOVER);
        int ty = r.y + (r.h - LM_FONT_H) / 2;
        draw_text(c, &r, r.x + 10, ty, m->label[i], COL_PANEL_TEXT);
    }
}

static void fill_round(const struct paint_ctx *c, const struct lm_rect *rect, int radius, uint32_t argb);

/* Text with every font pixel drawn as a scale x scale block (the lock screen's clock). */
static void draw_text_scaled(const struct paint_ctx *c, int x, int y, const char *text, int scale, uint32_t color)
{
    size_t len = strlen(text);
    for (size_t i = 0; i < len;) {
        int used;
        uint32_t cp = lm_utf8_decode(text + i, (int)(len - i), &used);
        i += used > 0 ? (size_t)used : 1;
        const uint8_t *g = lm_font_glyph(cp);
        for (int row = 0; row < LM_FONT_H; row++)
            for (int col = 0; col < LM_FONT_W; col++)
                if (g[row] & (0x80 >> col)) {
                    struct lm_rect px = lm_rect_make(x + col * scale, y + row * scale, scale, scale);
                    fill_rect(c, &px, color);
                }
        x += LM_FONT_W * scale;
    }
}

/* The lock screen: a dark screen with the time, a box with the dots of what was typed and the last message. */
static void paint_lock(const struct paint_ctx *c, const struct lm_lock_info *li)
{
    struct lm_rect o = lm_output_rect(c->out);
    fill_rect(c, &o, 0x0b0d12u);
    int cx = o.x + o.w / 2, cy = o.y + o.h / 2;

    int scale = o.w >= 1600 ? 8 : o.w >= 900 ? 6 : 3;
    int clock_h = LM_FONT_H * scale, top = cy - clock_h - 70;
    int tw = lm_font_text_width(li->clock) * scale;
    draw_text_scaled(c, cx - tw / 2, top, li->clock, scale, COL_PANEL_TEXT);
    struct lm_rect line = lm_rect_make(o.x, top + clock_h + 8, o.w, 20);
    draw_text_centered(c, &line, li->date, COL_PANEL_DIM);

    int bw = LM_MIN(o.w - 40, 360), bh = 44;
    struct lm_rect box = lm_rect_make(cx - bw / 2, top + clock_h + 44, bw, bh);
    fill_round(c, &box, 8, 0xff000000u | (li->blocked ? 0x2a1a1au : 0x1c212bu));
    struct lm_rect edges[4] = {                                         /* hairline in the accent colour (corners left out) */
        lm_rect_make(box.x + 8, box.y, box.w - 16, 1), lm_rect_make(box.x + 8, box.y + box.h - 1, box.w - 16, 1),
        lm_rect_make(box.x, box.y + 8, 1, box.h - 16), lm_rect_make(box.x + box.w - 1, box.y + 8, 1, box.h - 16),
    };
    for (int i = 0; i < 4; i++)
        fill_rect(c, &edges[i], li->blocked ? COL_PANEL_DANGER : COL_PANEL_ACCENT);

    if (li->nchars == 0 && !li->blocked) {
        draw_text_centered(c, &box, li->pin ? "Enter PIN" : "Enter password", COL_PANEL_DIM);
    } else {
        int shown = LM_MIN(li->nchars, (box.w - 40) / 20);
        int x = box.x + (box.w - shown * 20) / 2 + 4;
        for (int i = 0; i < shown; i++) {
            struct lm_rect dot = lm_rect_make(x + i * 20, box.y + box.h / 2 - 5, 10, 10);
            fill_round(c, &dot, 5, 0xffe6e9efu);
        }
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

/* Next line of at most `cols` code points from text[*pos]: whitespace runs become one space, leading blanks are
 * dropped. Returns true if text remains after the line. */
static bool wrap_line(const char *text, size_t *pos, int cols, char *out, size_t outsz)
{
    size_t len = strlen(text), i = *pos, o = 0;
    int n = 0;
    bool pending_space = false;
    while (i < len && n < cols) {
        int used;
        uint32_t cp = lm_utf8_decode(text + i, (int)(len - i), &used);
        if (used < 1)
            used = 1;
        if (cp == ' ' || cp == '\n' || cp == '\t' || cp == '\r') {
            pending_space = o > 0;
            i += (size_t)used;
            continue;
        }
        if (pending_space) {
            if (n + 1 >= cols)
                break;
            out[o++] = ' ';
            n++;
            pending_space = false;
        }
        if (o + (size_t)used + 1 >= outsz)
            break;
        memcpy(out + o, text + i, (size_t)used);
        o += (size_t)used;
        i += (size_t)used;
        n++;
    }
    out[o] = 0;
    while (i < len && (text[i] == ' ' || text[i] == '\n' || text[i] == '\t' || text[i] == '\r'))
        i++;
    *pos = i;
    return i < len;
}

/* Rounded rectangle with anti-aliased corners, blended with `argb` (alpha in the top byte) over what is there. */
static void fill_round(const struct paint_ctx *c, const struct lm_rect *rect, int radius, uint32_t argb)
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

/* Shadow, hairline ring and body of a gadget card (shared by all gadgets). */
static void paint_card_base(const struct paint_ctx *c, const struct lm_gadget_info *g)
{
    const struct lm_rect card = g->rect;
    /* soft drop shadow: stacked translucent rounded rectangles, the outermost faintest, a little lower than the card */
    for (int k = LM_GADGET_SHADOW - 4; k >= 1; k--) {              /* (the shadow sits 4 px lower: stay inside the scene bounds) */
        struct lm_rect sr = lm_rect_make(card.x - k, card.y - k + 4, card.w + 2 * k, card.h + 2 * k);
        fill_round(c, &sr, 12 + k, 0x06000000u);
    }
    fill_round(c, &card, 12, g->dragging ? 0xaa6fa8ffu : 0x2effffffu);  /* hairline: the 1 px ring around the body */
    struct lm_rect in = lm_rect_make(card.x + 1, card.y + 1, card.w - 2, card.h - 2);
    fill_round(c, &in, 11, 0xf2161a24u);
}

/* Resize grip: a small triangle of dots in the bottom right corner. */
static void paint_grip(const struct paint_ctx *c, const struct lm_rect *card)
{
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 3 - r; k++) {
            struct lm_rect dot = lm_rect_make(card->x + card->w - 11 - k * 4, card->y + card->h - 11 - r * 4, 2, 2);
            fill_round(c, &dot, 0, 0x70ffffffu);
        }
}

static void paint_mixer(const struct paint_ctx *c, const struct lm_gadget_info *g);
static void paint_system(const struct paint_ctx *c, const struct lm_gadget_info *g);

static void paint_gadget(const struct paint_ctx *c, const struct lm_gadget_info *g)
{
    if (g->id == LM_GADGET_MIXER) {
        paint_mixer(c, g);
        return;
    }
    if (g->id == LM_GADGET_SYSTEM) {
        paint_system(c, g);
        return;
    }
    const struct lm_rect card = g->rect;
    paint_card_base(c, g);

    /* header: clipboard glyph, title, count badge */
    int hy = card.y + (g->title_h - LM_FONT_H) / 2;
    struct lm_rect icon = lm_rect_make(card.x + 14, card.y + (g->title_h - 14) / 2, 11, 14);
    fill_round(c, &icon, 2, 0xff6fa8ffu);
    struct lm_rect clip = lm_rect_make(icon.x + 3, icon.y - 2, 5, 4);
    fill_round(c, &clip, 1, 0xffe6e9efu);
    struct lm_rect hole = lm_rect_make(icon.x + 2, icon.y + 4, 7, 8);
    fill_round(c, &hole, 1, 0xff161a24u);
    struct lm_rect head = lm_rect_make(card.x + 1, card.y + 1, card.w - 2, g->title_h - 1);
    draw_text(c, &head, card.x + 32, hy, "Clipboard", COL_PANEL_TEXT);
    char badge[8];
    snprintf(badge, sizeof(badge), "%d", g->total);
    int bw = lm_font_text_width(badge) + 14;
    struct lm_rect pill = lm_rect_make(card.x + card.w - 14 - bw, card.y + (g->title_h - 16) / 2, bw, 16);
    fill_round(c, &pill, 8, 0xff2d3a5au);
    draw_text_centered(c, &pill, badge, COL_PANEL_ACCENT);
    struct lm_rect sep = lm_rect_make(card.x + 12, card.y + g->title_h - 1, card.w - 24, 1);
    fill_round(c, &sep, 0, 0x30ffffffu);

    paint_grip(c, &card);

    int cols = (card.w - 2 * 12 - 2 * 10 - 4) / LM_FONT_W;     /* chip inset 12, text inset 10 (+ accent bar) */
    int lines = LM_MAX(1, (g->entry_h - 12) / LM_FONT_H);
    if (!g->n) {
        struct lm_rect r = lm_rect_make(card.x + 1, card.y + g->title_h, card.w - 2, g->entry_h);
        draw_text_centered(c, &r, "Nothing copied yet", COL_PANEL_DIM);
        return;
    }
    for (int i = 0; i < g->n; i++) {
        struct lm_rect slot = lm_rect_make(card.x + 12, card.y + g->title_h + 4 + i * g->entry_h, card.w - 24,
                                           g->entry_h - 4);
        fill_round(c, &slot, 7, i == 0 ? 0x26ffffffu : 0x12ffffffu);
        if (i == 0) {                                                    /* newest: accent bar on the left */
            struct lm_rect bar = lm_rect_make(slot.x + 4, slot.y + 6, 3, slot.h - 12);
            fill_round(c, &bar, 1, 0xff6fa8ffu);
        }
        if (i == g->cursor) {                                            /* keyboard cursor: an accent outline */
            struct lm_rect edge[4] = { lm_rect_make(slot.x, slot.y, slot.w, 2), lm_rect_make(slot.x, slot.y + slot.h - 2, slot.w, 2),
                                       lm_rect_make(slot.x, slot.y, 2, slot.h), lm_rect_make(slot.x + slot.w - 2, slot.y, 2, slot.h) };
            for (int e = 0; e < 4; e++)
                fill_round(c, &edge[e], 0, 0xff6fa8ffu);
        }
        uint32_t col = i == 0 ? COL_PANEL_TEXT : COL_PANEL_DIM;
        int tx = slot.x + 14;
        int ty = slot.y + (slot.h - lines * LM_FONT_H) / 2;
        size_t pos = 0;
        char line[256];
        for (int l = 0; l < lines; l++) {
            size_t start = pos;
            bool more = wrap_line(g->text[i], &pos, cols, line, sizeof(line));
            if (l == lines - 1 && more) {                                /* cut: the last line ends in "..." */
                pos = start;
                wrap_line(g->text[i], &pos, cols - 3, line, sizeof(line) - 4);
                strcat(line, "...");
            }
            draw_text(c, &slot, tx, ty + l * LM_FONT_H, line, col);
            if (!more && l < lines - 1)
                break;
        }
    }
}

/* Speaker glyph in `r` (a square): cone and, unless muted, two sound waves; a red slash when muted. */
static void paint_speaker(const struct paint_ctx *c, const struct lm_rect *r, bool muted, uint32_t col)
{
    int cx = r->x + r->w / 2 - 3, cy = r->y + r->h / 2;
    struct lm_rect box = lm_rect_make(cx - 5, cy - 3, 4, 6);
    fill_round(c, &box, 1, col);
    for (int i = 0; i < 6; i++) {                                  /* the cone widens to the right */
        struct lm_rect strip = lm_rect_make(cx - 1 + i, cy - 3 - i, 1, 6 + 2 * i);
        fill_round(c, &strip, 0, col);
    }
    if (!muted) {
        for (int w = 1; w <= 2; w++)
            for (int k = -w - 1; k <= w + 1; k++) {
                struct lm_rect dot = lm_rect_make(cx + 7 + w * 3 - (k * k) / (w + 2), cy + k * 2, 1, 2);
                fill_round(c, &dot, 0, (col & 0x00ffffffu) | 0xb0000000u);
            }
    } else {
        for (int k = -7; k <= 7; k++) {
            struct lm_rect dot = lm_rect_make(r->x + r->w / 2 + k, r->y + r->h / 2 + k, 2, 2);
            fill_round(c, &dot, 0, 0xffe05a5au);
        }
    }
}

/* A horizontal slider: track, filled part, knob; `value` 0..100; the percentage is written to its right. */
static void paint_slider(const struct paint_ctx *c, const struct lm_rect *s, int value, bool muted, bool pct_text)
{
    int ty = s->y + s->h / 2 - 3;
    struct lm_rect track = lm_rect_make(s->x, ty, s->w, 6);
    fill_round(c, &track, 3, 0xff2d3a5au);
    int fw = s->w * LM_CLAMP(value, 0, 100) / 100;
    if (fw > 0) {
        struct lm_rect fillr = lm_rect_make(s->x, ty, fw, 6);
        fill_round(c, &fillr, 3, muted ? 0xff7b8294u : 0xff6fa8ffu);
    }
    struct lm_rect knob = lm_rect_make(s->x + fw - 6, s->y + s->h / 2 - 7, 14, 14);
    fill_round(c, &knob, 7, muted ? 0xffa0a6b4u : 0xffe6e9efu);
    if (pct_text) {
        char t[12];
        snprintf(t, sizeof(t), "%d%%", value);
        struct lm_rect tr = lm_rect_make(s->x + s->w + 8, s->y - 4, 46, s->h + 8);
        draw_text(c, &tr, tr.x, tr.y + (tr.h - LM_FONT_H) / 2, t, muted ? COL_PANEL_DIM : COL_PANEL_TEXT);
    }
}

static void paint_mixer(const struct paint_ctx *c, const struct lm_gadget_info *g)
{
    const struct lm_rect card = g->rect;
    const struct lm_mixer_state *st = g->mixer;
    paint_card_base(c, g);

    /* header: speaker glyph, title, badge with the level */
    struct lm_rect icon = lm_rect_make(card.x + 10, card.y + (g->title_h - 20) / 2, 20, 20);
    paint_speaker(c, &icon, st && st->mute, 0xff6fa8ffu);
    int hy = card.y + (g->title_h - LM_FONT_H) / 2;
    struct lm_rect head = lm_rect_make(card.x + 1, card.y + 1, card.w - 2, g->title_h - 1);
    draw_text(c, &head, card.x + 36, hy, "Volume", COL_PANEL_TEXT);
    char badge[16];
    if (!st || st->backend == LM_MIXER_NONE)
        snprintf(badge, sizeof(badge), "--");
    else if (st->mute)
        snprintf(badge, sizeof(badge), "muted");
    else
        snprintf(badge, sizeof(badge), "%d%%", st->volume);
    int bw = lm_font_text_width(badge) + 14;
    struct lm_rect pill = lm_rect_make(card.x + card.w - 14 - bw, card.y + (g->title_h - 16) / 2, bw, 16);
    fill_round(c, &pill, 8, 0xff2d3a5au);
    draw_text_centered(c, &pill, badge, st && st->mute ? 0xe05a5au : COL_PANEL_ACCENT);
    struct lm_rect sep = lm_rect_make(card.x + 12, card.y + g->title_h - 1, card.w - 24, 1);
    fill_round(c, &sep, 0, 0x30ffffffu);
    paint_grip(c, &card);

    if (!st || st->backend == LM_MIXER_NONE) {
        struct lm_rect r = lm_rect_make(card.x + 12, card.y + g->title_h, card.w - 24, card.h - g->title_h - 12);
        draw_text_centered(c, &r, st && st->ready ? "No mixer found (pactl or amixer)" : "Looking for a mixer...", COL_PANEL_DIM);
        return;
    }
    bool pulse = st->backend == LM_MIXER_PULSE;
    struct lm_mixer_layout l;
    lm_mixer_layout(&card, g->title_h, st->nstreams, pulse, &l);

    fill_round(c, &l.master_mute, 6, st->mute ? 0x40e05a5au : 0x26ffffffu);
    paint_speaker(c, &l.master_mute, st->mute, st->mute ? 0xffe05a5au : 0xffe6e9efu);
    paint_slider(c, &l.master_slider, st->volume, st->mute, true);
    if (pulse) {
        fill_round(c, &l.output, 7, 0x1affffffu);
        char label[96];
        snprintf(label, sizeof(label), "%s%s", st->output, st->nsinks > 1 ? "   >" : "");
        draw_text(c, &l.output, l.output.x + 10, l.output.y + (l.output.h - LM_FONT_H) / 2, label, COL_PANEL_TEXT);
    }
    for (int i = 0; i < l.nstreams; i++) {
        const struct lm_mixer_stream *s = &st->streams[i];
        draw_text(c, &l.stream[i].label, l.stream[i].label.x, l.stream[i].label.y, s->name, s->mute ? COL_PANEL_DIM : COL_PANEL_TEXT);
        fill_round(c, &l.stream[i].mute, 5, s->mute ? 0x40e05a5au : 0x26ffffffu);
        struct lm_rect gl = lm_rect_make(l.stream[i].mute.x + 2, l.stream[i].mute.y, 18, 20);
        paint_speaker(c, &gl, s->mute, s->mute ? 0xffe05a5au : 0xffe6e9efu);
        paint_slider(c, &l.stream[i].slider, s->volume, s->mute, true);
    }
    if (l.hint.w > 0 && l.nstreams == st->nstreams) {
        const char *hint = st->backend == LM_MIXER_ALSA ? "ALSA Master (no per-app volume)" :
                           st->backend == LM_MIXER_WPCTL ? "Install pulseaudio-utils for apps" :
                           st->nstreams ? "" : "Applications that play sound show up here.";
        if (*hint)
            draw_text(c, &l.hint, l.hint.x, l.hint.y + 2, hint, COL_PANEL_DIM);
    }
}

/* Colour of a load / temperature bar: calm blue, amber when high, red when critical. */
static uint32_t level_color(int pct, int warn, int crit)
{
    return pct >= crit ? 0xffe05a5au : pct >= warn ? 0xffe0a040u : 0xff6fa8ffu;
}

/* A labelled horizontal bar: `label` left, `value` text right, a track with a fill below. */
static void paint_meter(const struct paint_ctx *c, int x, int y, int w, const char *label, const char *value, int pct,
                        uint32_t col)
{
    struct lm_rect row = lm_rect_make(x, y, w, 16);
    draw_text(c, &row, x, y, label, COL_PANEL_DIM);
    int vw = lm_font_text_width(value);
    draw_text(c, &row, x + w - vw, y, value, COL_PANEL_TEXT);
    struct lm_rect track = lm_rect_make(x, y + 17, w, 6);
    fill_round(c, &track, 3, 0xff2d3a5au);
    int fw = w * LM_CLAMP(pct, 0, 100) / 100;
    if (fw >= 2) {
        struct lm_rect fillr = lm_rect_make(x, y + 17, fw, 6);
        fill_round(c, &fillr, 3, col);
    }
}

static void paint_system(const struct paint_ctx *c, const struct lm_gadget_info *g)
{
    const struct lm_rect card = g->rect;
    const struct lm_sysinfo_state *st = g->sys;
    paint_card_base(c, g);

    /* header: a chip glyph, title, the load in a pill */
    struct lm_rect chip = lm_rect_make(card.x + 14, card.y + (g->title_h - 12) / 2, 12, 12);   /* (title_h is 26: 7 px above and below) */
    fill_round(c, &chip, 2, 0xff6fa8ffu);
    struct lm_rect die = lm_rect_make(chip.x + 3, chip.y + 3, 6, 6);
    fill_round(c, &die, 1, 0xff161a24u);
    for (int i = 0; i < 3; i++) {                                       /* pins */
        struct lm_rect t = lm_rect_make(chip.x + 2 + i * 4, chip.y - 3, 1, 3), b = lm_rect_make(chip.x + 2 + i * 4, chip.y + 12, 1, 3);
        struct lm_rect l = lm_rect_make(chip.x - 3, chip.y + 2 + i * 4, 3, 1), r = lm_rect_make(chip.x + 12, chip.y + 2 + i * 4, 3, 1);
        fill_round(c, &t, 0, 0xffa0a6b4u);
        fill_round(c, &b, 0, 0xffa0a6b4u);
        fill_round(c, &l, 0, 0xffa0a6b4u);
        fill_round(c, &r, 0, 0xffa0a6b4u);
    }
    int hy = card.y + (g->title_h - LM_FONT_H) / 2;
    struct lm_rect head = lm_rect_make(card.x + 1, card.y + 1, card.w - 2, g->title_h - 1);
    draw_text(c, &head, card.x + 38, hy, "System", COL_PANEL_TEXT);
    char badge[16];
    snprintf(badge, sizeof(badge), "%d%%", st ? st->cpu : 0);
    int bw = lm_font_text_width(badge) + 14;
    struct lm_rect pill = lm_rect_make(card.x + card.w - 14 - bw, card.y + (g->title_h - 16) / 2, bw, 16);
    fill_round(c, &pill, 8, 0xff2d3a5au);
    draw_text_centered(c, &pill, badge, st ? level_color(st->cpu, 70, 90) & 0x00ffffffu : COL_PANEL_ACCENT);
    struct lm_rect sep = lm_rect_make(card.x + 12, card.y + g->title_h - 1, card.w - 24, 1);
    fill_round(c, &sep, 0, 0x30ffffffu);
    paint_grip(c, &card);

    if (!st || !st->ready) {
        struct lm_rect r = lm_rect_make(card.x + 12, card.y + g->title_h, card.w - 24, card.h - g->title_h - 12);
        draw_text_centered(c, &r, "Reading...", COL_PANEL_DIM);
        return;
    }
    int x = card.x + 12, w = card.w - 24, y = card.y + g->title_h + 6, bottom = card.y + card.h - 8;
    /* What fits, in order of importance: CPU, memory, temperature are always drawn (the minimum card height holds them),
     * then the CPU graph, the per-core bars and swap as room allows. */
    int fixed = 28 + 30 + 30;
    int room = bottom - y - fixed;
    bool graph = room >= 32, cores;
    if (graph) room -= 32;
    cores = room >= 22 && st->ncores > 1;
    if (cores) room -= 22;
    bool swap = room >= 30 && st->swap_total_kb > 0;

    char val[64];
    snprintf(val, sizeof(val), "%d%%   load %.2f", st->cpu, st->load1);
    paint_meter(c, x, y, w, st->ncores > 1 ? "CPU" : "CPU", val, st->cpu, level_color(st->cpu, 70, 90));
    y += 28;
    if (graph) {                                                        /* the last minute, one bar per second */
        struct lm_rect area = lm_rect_make(x, y, w, 26);
        fill_round(c, &area, 5, 0x12ffffffu);
        int n = st->nhist;
        int bwid = LM_MAX(1, (w - 4) / LM_SYS_HISTORY);
        for (int i = 0; i < n; i++) {
            int v = st->hist[i];
            int hh = LM_MAX(1, (area.h - 6) * v / 100);
            struct lm_rect bar = lm_rect_make(area.x + area.w - 2 - (n - i) * bwid, area.y + area.h - 3 - hh, LM_MAX(1, bwid - 1), hh);
            if (bar.x >= area.x + 1)
                fill_round(c, &bar, 0, (level_color(v, 70, 90) & 0x00ffffffu) | 0xd0000000u);
        }
        y += 32;
    }
    if (cores) {                                                        /* one small bar per logical CPU */
        int n = st->ncores, gap = 3;
        int bwid = LM_MAX(3, (w - gap * (n - 1)) / n);
        for (int i = 0; i < n; i++) {
            struct lm_rect slot = lm_rect_make(x + i * (bwid + gap), y, bwid, 16);
            if (slot.x + slot.w > x + w)
                break;
            fill_round(c, &slot, 2, 0xff2d3a5au);
            int hh = 16 * st->core[i] / 100;
            if (hh > 0) {
                struct lm_rect f = lm_rect_make(slot.x, slot.y + 16 - hh, slot.w, hh);
                fill_round(c, &f, 2, level_color(st->core[i], 70, 90));
            }
        }
        y += 22;
    }
    snprintf(val, sizeof(val), "%.1f / %.1f GiB   %d%%", (double)(st->mem_total_kb - st->mem_avail_kb) / 1048576.0,
             (double)st->mem_total_kb / 1048576.0, st->mem_pct);
    paint_meter(c, x, y, w, "Memory", val, st->mem_pct, level_color(st->mem_pct, 80, 92));
    y += 30;
    if (swap) {
        snprintf(val, sizeof(val), "%.1f / %.1f GiB   %d%%", (double)(st->swap_total_kb - st->swap_free_kb) / 1048576.0,
                 (double)st->swap_total_kb / 1048576.0, st->swap_pct);
        paint_meter(c, x, y, w, "Swap", val, st->swap_pct, level_color(st->swap_pct, 30, 70));
        y += 30;
    }
    if (st->temp_c >= 0) {
        snprintf(val, sizeof(val), "%d \xc2\xb0""C", st->temp_c);
        int pct = LM_CLAMP((st->temp_c - 20) * 100 / 80, 0, 100);       /* the bar spans 20..100 degrees */
        paint_meter(c, x, y, w, "Temperature", val, pct, st->temp_c >= 85 ? 0xffe05a5au : st->temp_c >= 70 ? 0xffe0a040u : 0xff5ac88cu);
    } else {
        paint_meter(c, x, y, w, "Temperature", "no sensor", 0, 0xff6fa8ffu);
    }
    y += 30;

    /* The device: what room is left, in order of importance. */
    char up[24];
    lm_sysinfo_format_uptime(st->uptime_s, up, sizeof(up));
    const struct { const char *label, *value; } info[] = {
        { "Model", st->model }, { "Maker", st->maker }, { "CPU", st->cpu_model }, { "OS", st->os_name },
        { "Kernel", st->kernel }, { "Host", st->host }, { "Uptime", st->uptime_s >= 0 ? up : "" },
    };
    int rows = (bottom - y - 4) / 16;
    if (rows > 0) {
        struct lm_rect sep2 = lm_rect_make(x, y - 6, w, 1);
        fill_round(c, &sep2, 0, 0x30ffffffu);
        int vx = x + 56;
        for (size_t i = 0; i < sizeof(info) / sizeof(info[0]) && rows > 0; i++) {
            if (!info[i].value[0])
                continue;
            struct lm_rect row = lm_rect_make(x, y, w, 16);
            draw_text(c, &row, x, y, info[i].label, COL_PANEL_DIM);
            char fit[96];
            snprintf(fit, sizeof(fit), "%s", info[i].value);
            int maxw = x + w - vx;
            size_t len = strlen(fit);
            if (lm_font_text_width(fit) > maxw) {                       /* cut at a character boundary, end with ".." */
                while (len > 0 && lm_font_text_width(fit) + 2 * LM_FONT_W > maxw) {
                    len--;
                    while (len > 0 && ((unsigned char)fit[len] & 0xc0) == 0x80)
                        len--;
                    fit[len] = 0;
                }
                strcat(fit, "..");
            }
            draw_text(c, &row, vx, y, fit, COL_PANEL_TEXT);
            y += 16;
            rows--;
        }
    }
}

static void paint_panel(const struct paint_ctx *c, const struct lm_panel_info *p)
{
    fill_rect(c, &p->bar, COL_PANEL);
    /* thin accent line at the bottom edge */
    struct lm_rect line = lm_rect_make(p->bar.x, p->bar.y + p->bar.h - 1, p->bar.w, 1);
    fill_rect(c, &line, 0x262c3bu);

    fill_rect(c, &p->launcher, p->hover_launcher ? COL_PANEL_HOVER : COL_PANEL_BTN);
    draw_text_centered(c, &p->launcher, "MellaGUI", COL_PANEL_TEXT);

    for (int i = 0; i < LM_NUM_WORKSPACES; i++) {
        const struct lm_rect *w = &p->workspace[i];
        bool cur = i == p->current_workspace;
        uint32_t bg = cur ? COL_PANEL_ACCENT : (i == p->hover_workspace ? COL_PANEL_HOVER : COL_PANEL_BTN);
        fill_rect(c, w, bg);
        char n[2] = { (char)('1' + i), 0 };
        draw_text_centered(c, w, n, cur ? COL_PANEL : ((p->workspace_occupied >> i) & 1 ? COL_PANEL_TEXT : COL_PANEL_DIM));
        if (!cur && ((p->workspace_occupied >> i) & 1)) {
            struct lm_rect dot = lm_rect_make(w->x + w->w / 2 - 2, w->y + w->h - 5, 4, 2);
            fill_rect(c, &dot, COL_PANEL_ACCENT);
        }
    }
    for (int i = 0; i < p->ntasks; i++) {
        const struct lm_rect *r = &p->task[i].rect;
        uint32_t bg = p->task[i].active ? COL_PANEL_ACCENT : (p->task[i].hover ? COL_PANEL_HOVER : COL_PANEL_BTN);
        uint32_t fg = p->task[i].active ? COL_PANEL : (p->task[i].minimized ? COL_PANEL_DIM : COL_PANEL_TEXT);
        fill_rect(c, r, bg);
        struct lm_rect label = lm_rect_make(r->x + 4, r->y, r->w - 8, r->h);
        draw_text_centered(c, &label, p->task[i].title[0] ? p->task[i].title : "-", fg);
    }
    if (!p->ntasks && p->active_title && p->active_title[0])
        draw_text_centered(c, &p->title_area, p->active_title, COL_PANEL_TEXT);
    if (p->caps.w > 0) {
        fill_rect(c, &p->caps, COL_PANEL_DANGER);
        draw_text_centered(c, &p->caps, p->caps_text, COL_PANEL_TEXT);
    }
    fill_rect(c, &p->layout, p->hover_layout ? COL_PANEL_HOVER : COL_PANEL_BTN);
    draw_text_centered(c, &p->layout, p->layout_text, COL_PANEL_TEXT);
    uint32_t sbg = p->sticky_on ? COL_PANEL_ACCENT : (p->hover_sticky ? COL_PANEL_HOVER : COL_PANEL_BTN);
    if (p->sticky_on && p->hover_sticky)
        sbg = 0x8fbcffu;
    fill_rect(c, &p->sticky, sbg);
    draw_text_centered(c, &p->sticky, p->sticky_text, p->sticky_on ? COL_PANEL : COL_PANEL_DIM);
    fill_rect(c, &p->run, p->hover_run ? COL_PANEL_HOVER : COL_PANEL_BTN);
    draw_text_centered(c, &p->run, "Run", COL_PANEL_TEXT);
    uint32_t ebg = p->exit_armed ? COL_PANEL_DANGER : (p->hover_exit ? COL_PANEL_HOVER : COL_PANEL_BTN);
    fill_rect(c, &p->exit, ebg);
    draw_text_centered(c, &p->exit, p->exit_armed ? "Sure?" : "Exit", COL_PANEL_TEXT);
    draw_text_centered(c, &p->clock, p->clock_text, COL_PANEL_TEXT);
    fill_rect(c, &p->lock, p->hover_lock ? COL_PANEL_HOVER : COL_PANEL_BTN);
    draw_text_centered(c, &p->lock, "Lock", COL_PANEL_TEXT);
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

static void blit_surface(const struct paint_ctx *c, struct lm_surface *s, int32_t ox, int32_t oy)
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
    v->rect = lm_rect_make(ix - v->sprite->hot_x, iy - v->sprite->hot_y, SPR_W, SPR_H);
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
    for (int y = 0; y < SPR_H; y++) {
        int gy = v.rect.y + y;
        if (gy < c->clip.y || gy >= c->clip.y + c->clip.h)
            continue;
        uint32_t *row = fb_row(c, gy);
        for (int x = 0; x < SPR_W; x++) {
            int gx = v.rect.x + x;
            if (gx < c->clip.x || gx >= c->clip.x + c->clip.w)
                continue;
            uint32_t p = v.sprite->px[y * SPR_W + x];
            if (p)
                row[gx] = blend_px(p, row[gx]);
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
            c->cov_rect = r;
            c->cov_area = area;
        }
    }
    c->idx++;
    return true;
}

static bool scene_cb(const struct lm_scene_item *item, void *data)
{
    struct paint_ctx *c = data;
    if (c->idx++ < c->skip_before)
        return true;
    switch (item->kind) {
    case LM_SCENE_WALLPAPER:
        if (item->output == c->out)
            paint_wallpaper(c);
        break;
    case LM_SCENE_GADGET:
        paint_gadget(c, &item->u.gadget);
        break;
    case LM_SCENE_FRAME:
        paint_frame(c, &item->u.frame);
        break;
    case LM_SCENE_SURFACE:
        lm_surface_tree_visit(item->u.surface.root, item->u.surface.x, item->u.surface.y,
                              visit_blit, c);
        break;
    case LM_SCENE_PANEL:
        if (item->output == c->out)
            paint_panel(c, &item->u.panel);
        break;
    case LM_SCENE_MENU:
        paint_menu(c, &item->u.menu);
        break;
    case LM_SCENE_LOCK:
        if (item->output == c->out)
            paint_lock(c, &item->u.lock);
        break;
    }
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
    fill_round(c, &card, 8, 0xf2161a24u);
    struct lm_rect th = lm_rect_make(card.x + 8, card.y + 8, fx->tw, fx->th);
    struct lm_rect ring = lm_rect_make(th.x - 1, th.y - 1, th.w + 2, th.h + 2);
    fill_rect(c, &ring, 0x003a4256u);
    struct lm_rect vis;
    if (lm_rect_intersect(&th, &c->clip, &vis)) {
        for (int y = vis.y; y < vis.y + vis.h; y++)
            memcpy(fb_row(c, y) + vis.x, fx->thumb + (size_t)(y - th.y) * (size_t)fx->tw + (size_t)(vis.x - th.x),
                   (size_t)vis.w * 4);
    }
    struct lm_rect text = lm_rect_make(card.x + 8, card.y + 8 + fx->th + 6, fx->tw, LM_FONT_H);
    draw_text(c, &text, text.x, text.y, fx->label, 0x00d8dde8u);
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
        c.skip_before = 0;
        lm_wm_scene_for_each(r->srv->wm, &full, scene_cb, &c);
    } else {
        /* An opaque rectangle `cov` hides everything below its surface: inside it only the items from the cover upwards
         * are painted; the strips around it get the full stack (those are the transparent corners and edges). */
        struct lm_rect cov = c.cov_rect;
        int cov_idx = c.cov_idx;
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
            c.skip_before = 0;
            lm_wm_scene_for_each(r->srv->wm, &c.clip, scene_cb, &c);
        }
        c.clip = cov;
        c.idx = 0;
        c.skip_before = cov_idx;
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
        for (int i = 0; i < redraw.n; i++) {
            local[nlocal] = redraw.rects[i];
            local[nlocal].x -= out->x;
            local[nlocal].y -= out->y;
            nlocal++;
        }
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
    if (r->gl)                                          /* zero-copy surfaces have no CPU pixels: the GPU renders the shot */
        return gl_capture(r, st, &clip, w, h);
    size_t stride = (size_t)out->width * 4;
    uint32_t *whole = calloc((size_t)out->height, stride);
    uint32_t *res = malloc((size_t)clip.w * (size_t)clip.h * 4);
    if (!whole || !res) {
        free(whole);
        free(res);
        return NULL;
    }
    struct lm_framebuffer fb = { .pixels = whole, .width = out->width, .height = out->height, .stride = (int32_t)stride };
    paint_rect(r, st, &fb, &clip, false);
    for (int y = 0; y < clip.h; y++)
        memcpy(res + (size_t)y * (size_t)clip.w,
               (const uint8_t *)whole + (size_t)(clip.y - out->y + y) * stride + (size_t)(clip.x - out->x) * 4,
               (size_t)clip.w * 4);
    free(whole);
    *w = clip.w;
    *h = clip.h;
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
