/* wlrail - a side rail of round buttons for the wlwin programs (header only). The buttons are drops of water (wlwin_drop): they swell under the pointer, flatten while
 * pressed, wobble back on springs and send a ripple out after a press; the name of the one under the pointer shows beside it.
 *
 * A program keeps one `struct wlrail` per side (filled again before it paints or hit-tests: the items are plain data) and one `struct wlrail_anim` for the springs:
 *
 *   r.x = 0 (the left rail) or bw - WLRAIL_W (the right one); r.y0 / r.y1 = the band of the window it lives in; items: tip (name), text or icon, colours, flags
 *   wlrail_at(&r, px, py)            the item under a point, -1 none
 *   wlrail_press(&an, i, now)        a press (starts the squash and the ripple); wlrail_release(&an) when the button goes up
 *   wlrail_step(&an, &r, hover, now) once per loop turn: true while something still moves (ask for a frame in 16 ms)
 *   wlrail_paint(w, &r, &an, hover, now, bar, tip_bg, tip_rim, tip_ink)
 *
 * Items anchored to the top are stacked from the top (`gap` = 10 px more before it), the ones with `bottom` from the bottom up; when the two groups meet, the
 * top ones that no longer fit are left out. `[windows] animation-ms` 0 switches the movement off (the buttons only change colour). */
#ifndef MELLAGUI_WLRAIL_H
#define MELLAGUI_WLRAIL_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "font.h"
#include "metrics.h"
#include "uikit.h"
#include "wlcard.h"
#include "wlwin.h"

#define WLRAIL_W 52
#define WLRAIL_MAX 16
#define WLRAIL_PITCH 36
#define WLRAIL_D 32

struct wlrail_item {
    const char *tip;                    /* the name shown beside the button (NULL = none) */
    const char *text;                   /* a short label in the drop, when there is no icon */
    void (*icon)(struct wlwin *w, int cx, int cy, uint32_t ink, uint32_t bg);     /* a picture of about 16 x 16 centred on (cx, cy) */
    uint32_t base, lite;                /* the colours: at rest, and lit (pointer on it) */
    bool show, idle, lit, bottom, gap;  /* shown at all, dimmed (nothing to do), shown lit, anchored to the bottom, 10 px of room before it */
};

struct wlrail {
    struct wlrail_item it[WLRAIL_MAX];
    int n;
    int x;                              /* the left edge of the rail */
    int y0, y1;                         /* the band the buttons live in */
};

struct wlrail_anim {
    struct { float sx, sy, vx, vy; int64_t ripple_ms; } d[WLRAIL_MAX];
    int down;                           /* the item held down, -1 = none */
    int64_t ms;
    bool init;
};

/* Rails are wanted unless the program's own variable (MELLAGUI_<APP>_RAIL) or MELLAGUI_RAILS says 0 (the tests that were written for the bars use it). */
static inline bool wlrail_wanted(const char *app_var)
{
    const char *a = app_var ? getenv(app_var) : NULL, *g = getenv("MELLAGUI_RAILS");
    return !(a && !strcmp(a, "0")) && !(g && !strcmp(g, "0"));
}

/* The top of every item (-1 = not shown / no room). Returns the number that show. */
static inline int wlrail_layout(const struct wlrail *r, int *y)
{
    int top = r->y0 + 6, bottom = r->y1 - 6, low = bottom, shown = 0;
    for (int j = 0; j < r->n; j++)
        y[j] = -1;
    for (int j = r->n - 1; j >= 0; j--)                                 /* the bottom group, from the bottom up */
        if (r->it[j].show && r->it[j].bottom) {
            bottom -= WLRAIL_PITCH;
            y[j] = bottom + 4;
            low = bottom;
        }
    for (int j = 0; j < r->n; j++)
        if (r->it[j].show && !r->it[j].bottom) {
            if (r->it[j].gap)
                top += 10;
            if (top + WLRAIL_D <= low - 4)
                y[j] = top;
            top += WLRAIL_PITCH;
        }
    for (int j = 0; j < r->n; j++)
        shown += y[j] >= 0;
    return shown;
}

static inline int wlrail_at(const struct wlrail *r, double px, double py)
{
    if (px < r->x + (WLRAIL_W - WLRAIL_D) / 2 || px >= r->x + (WLRAIL_W + WLRAIL_D) / 2)
        return -1;
    int y[WLRAIL_MAX];
    wlrail_layout(r, y);
    for (int j = 0; j < r->n; j++)
        if (y[j] >= 0 && py >= y[j] && py < y[j] + WLRAIL_D)
            return j;
    return -1;
}

static inline void wlrail_press(struct wlrail_anim *an, int i, int64_t now)
{
    an->down = i;
    if (i >= 0 && i < WLRAIL_MAX)
        an->d[i].ripple_ms = lm_metrics.animation_ms > 0 ? now : 0;
}

static inline void wlrail_release(struct wlrail_anim *an)
{
    an->down = -1;                                                      /* (the drop springs back) */
}

static inline void wlrail_init(struct wlrail_anim *an)
{
    for (int i = 0; i < WLRAIL_MAX; i++)
        an->d[i].sx = an->d[i].sy = 1;
    an->down = -1;
    an->init = true;
}

/* One spring step for every drop: swells under the pointer, flattens while pressed, wobbles back (zeta about 0.4). True while something still moves. */
static inline bool wlrail_step(struct wlrail_anim *an, const struct wlrail *r, int hover, int64_t now)
{
    if (!an->init)
        wlrail_init(an);
    float dt = an->ms ? (float)(now - an->ms < 0 ? 0 : now - an->ms > 50 ? 50 : now - an->ms) / 1000.0f : 0;
    an->ms = now;
    bool on = lm_metrics.animation_ms > 0, active = false;
    for (int i = 0; i < r->n && i < WLRAIL_MAX; i++) {
        float tx = 1, ty = 1;
        if (an->down == i) { tx = 1.16f; ty = 0.80f; }                  /* pressed: a drop that is pushed flat */
        else if (hover == i) { tx = 1.07f; ty = 1.07f; }
        if (!on) {
            an->d[i].sx = tx;
            an->d[i].sy = ty;
            an->d[i].vx = an->d[i].vy = 0;
            continue;
        }
        for (float t = 0; t < dt; t += 0.004f) {
            float h = dt - t < 0.004f ? dt - t : 0.004f;
            an->d[i].vx += (260.0f * (tx - an->d[i].sx) - 13.0f * an->d[i].vx) * h;
            an->d[i].vy += (260.0f * (ty - an->d[i].sy) - 13.0f * an->d[i].vy) * h;
            an->d[i].sx += an->d[i].vx * h;
            an->d[i].sy += an->d[i].vy * h;
        }
        if (fabsf(an->d[i].sx - tx) > 0.002f || fabsf(an->d[i].sy - ty) > 0.002f || fabsf(an->d[i].vx) > 0.02f || fabsf(an->d[i].vy) > 0.02f)
            active = true;
        if (an->d[i].ripple_ms && now - an->d[i].ripple_ms < 500)
            active = true;
        else
            an->d[i].ripple_ms = 0;
    }
    return active;
}

/* The part of the window's left column under the rail (the status line's row), painted in the rail's colour: a program with an inset has no drawing there. */
static inline void wlrail_foot(struct wlwin *w, const struct wlrail *r, uint32_t color)
{
    int inset = w->inx;
    w->inx = 0;
    if (w->bh > r->y1)
        wlwin_fill(w, r->x, r->y1, WLRAIL_W, w->bh - r->y1, color);
    w->inx = inset;
}

static inline int64_t wlrail_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* For a program's loop: the springs of one rail for this turn. Returns the timeout to wait with (`timeout`, or 16 ms while a drop moves). */
static inline int wlrail_loop(struct wlwin *w, struct wlrail_anim *an, const struct wlrail *r, int hover, int timeout)
{
    if (!wlrail_step(an, r, hover, wlrail_now()))
        return timeout;
    w->redraw = true;
    return timeout < 0 || timeout > 16 ? 16 : timeout;
}

/* ---- pictures (about 16 x 16, centred on cx, cy) -------------------------------------------------------------------------------------- */

static inline void wlrail_line(struct wlwin *w, int x0, int y0, int x1, int y1, int t, uint32_t c)
{
    int dx = abs(x1 - x0), dy = abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, e = dx - dy;
    for (;;) {
        wlwin_fill(w, x0, y0, t, t, c);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * e;
        if (e2 > -dy) { e -= dy; x0 += sx; }
        if (e2 < dx) { e += dx; y0 += sy; }
    }
}

static inline void wlrail_box(struct wlwin *w, int x, int y, int bw, int bh, uint32_t c)
{
    wlwin_fill(w, x, y, bw, 2, c);
    wlwin_fill(w, x, y + bh - 2, bw, 2, c);
    wlwin_fill(w, x, y, 2, bh, c);
    wlwin_fill(w, x + bw - 2, y, 2, bh, c);
}

static inline void wlrail_ic_new(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    (void)bg;
    int x = cx - 8, y = cy - 8;
    wlrail_box(w, x + 3, y + 1, 10, 14, c);
    wlwin_fill(w, x + 5, y + 6, 6, 2, c);
    wlwin_fill(w, x + 5, y + 10, 6, 2, c);
}

static inline void wlrail_ic_open(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    (void)bg;
    int x = cx - 8, y = cy - 8;
    wlwin_fill(w, x + 1, y + 3, 6, 3, c);
    wlrail_box(w, x + 1, y + 5, 14, 9, c);
}

static inline void wlrail_ic_save(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    (void)bg;
    int x = cx - 8 + 2, y = cy - 8;
    wlrail_box(w, x, y + 2, 12, 12, c);
    wlwin_fill(w, x + 3, y + 2, 6, 4, c);
    wlwin_fill(w, x + 3, y + 9, 6, 5, c);
}

static inline void wlrail_ic_saveas(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    (void)bg;
    int x = cx - 8, y = cy - 8;
    wlrail_box(w, x, y + 2, 12, 12, c);
    wlwin_fill(w, x + 3, y + 2, 6, 4, c);
    wlwin_fill(w, x + 3, y + 9, 6, 5, c);
    wlwin_fill(w, x + 12, y + 8, 4, 2, c);
    wlwin_fill(w, x + 13, y + 7, 2, 4, c);
}

static inline void wlrail_ic_cut(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    (void)bg;
    int x = cx - 8, y = cy - 8;
    wlrail_line(w, x + 3, y + 1, x + 11, y + 11, 2, c);
    wlrail_line(w, x + 12, y + 1, x + 4, y + 11, 2, c);
    wlrail_box(w, x + 1, y + 11, 5, 4, c);
    wlrail_box(w, x + 10, y + 11, 5, 4, c);
}

static inline void wlrail_ic_copy(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    int x = cx - 8, y = cy - 8;
    wlrail_box(w, x + 2, y + 1, 9, 10, c);
    wlwin_fill(w, x + 5, y + 5, 9, 10, bg);
    wlrail_box(w, x + 5, y + 5, 9, 10, c);
}

static inline void wlrail_ic_paste(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    (void)bg;
    int x = cx - 8, y = cy - 8;
    wlrail_box(w, x + 2, y + 3, 12, 12, c);
    wlwin_fill(w, x + 5, y + 1, 6, 4, c);
    wlwin_fill(w, x + 5, y + 8, 6, 2, c);
}

static inline void wlrail_ic_find(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    int x = cx - 8, y = cy - 8;
    wlwin_round_fill(w, x + 1, y + 1, 11, 11, 5, c);
    wlwin_round_fill(w, x + 3, y + 3, 7, 7, 3, bg);
    wlrail_line(w, x + 10, y + 10, x + 14, y + 14, 2, c);
}

static inline void wlrail_ic_up(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    (void)bg;
    wlrail_line(w, cx, cy - 7, cx - 6, cy - 1, 2, c);
    wlrail_line(w, cx, cy - 7, cx + 6, cy - 1, 2, c);
    wlwin_fill(w, cx - 1, cy - 6, 2, 13, c);
}

static inline void wlrail_ic_down(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    (void)bg;
    wlrail_line(w, cx, cy + 7, cx - 6, cy + 1, 2, c);
    wlrail_line(w, cx, cy + 7, cx + 6, cy + 1, 2, c);
    wlwin_fill(w, cx - 1, cy - 7, 2, 13, c);
    wlwin_fill(w, cx - 7, cy + 8, 14, 2, c);
}

static inline void wlrail_ic_home(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    (void)bg;
    wlrail_line(w, cx, cy - 7, cx - 7, cy, 2, c);
    wlrail_line(w, cx, cy - 7, cx + 7, cy, 2, c);
    wlrail_box(w, cx - 5, cy - 1, 10, 8, c);
    wlwin_fill(w, cx - 1, cy + 2, 3, 5, c);
}

static inline void wlrail_ic_eye(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    wlwin_round_fill(w, cx - 8, cy - 5, 16, 10, 5, c);
    wlwin_round_fill(w, cx - 6, cy - 3, 12, 6, 3, bg);
    wlwin_round_fill(w, cx - 3, cy - 3, 6, 6, 3, c);
}

static inline void wlrail_ic_term(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    (void)bg;
    wlrail_box(w, cx - 8, cy - 6, 16, 12, c);
    wlrail_line(w, cx - 5, cy - 3, cx - 2, cy, 2, c);
    wlrail_line(w, cx - 5, cy + 3, cx - 2, cy, 2, c);
    wlwin_fill(w, cx + 1, cy + 2, 5, 2, c);
}

static inline void wlrail_ic_panes(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    (void)bg;
    wlrail_box(w, cx - 8, cy - 6, 16, 12, c);
    wlwin_fill(w, cx - 1, cy - 6, 2, 12, c);
    wlwin_fill(w, cx + 2, cy - 3, 4, 2, c);
    wlwin_fill(w, cx + 2, cy + 1, 4, 2, c);
}

static inline void wlrail_ic_trash(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    (void)bg;
    wlwin_fill(w, cx - 7, cy - 5, 14, 2, c);
    wlwin_fill(w, cx - 2, cy - 7, 4, 2, c);
    wlrail_box(w, cx - 5, cy - 3, 10, 11, c);
    wlwin_fill(w, cx - 2, cy - 1, 1, 7, c);
    wlwin_fill(w, cx + 1, cy - 1, 1, 7, c);
}

static inline void wlrail_ic_net(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    (void)bg;
    wlrail_line(w, cx, cy - 5, cx - 6, cy + 5, 1, c);
    wlrail_line(w, cx, cy - 5, cx + 6, cy + 5, 1, c);
    wlwin_fill(w, cx - 6, cy + 5, 12, 1, c);
    wlwin_round_fill(w, cx - 3, cy - 8, 6, 6, 3, c);
    wlwin_round_fill(w, cx - 9, cy + 2, 6, 6, 3, c);
    wlwin_round_fill(w, cx + 3, cy + 2, 6, 6, 3, c);
}


static inline void wlrail_ic_camera(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    wlwin_round_fill(w, cx - 8, cy - 5, 16, 11, 3, c);
    wlwin_fill(w, cx - 4, cy - 7, 6, 2, c);
    wlwin_round_fill(w, cx - 4, cy - 3, 8, 8, 4, bg);
    wlwin_round_fill(w, cx - 2, cy - 1, 4, 4, 2, c);
}

static inline void wlrail_ic_rec(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    (void)bg;
    wlwin_round_fill(w, cx - 6, cy - 6, 12, 12, 6, c);
}

static inline void wlrail_ic_mic(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    (void)bg;
    wlwin_round_fill(w, cx - 3, cy - 8, 6, 10, 3, c);
    wlrail_box(w, cx - 6, cy - 3, 12, 8, c);
    wlwin_fill(w, cx - 1, cy + 5, 2, 3, c);
    wlwin_fill(w, cx - 4, cy + 7, 8, 2, c);
}

static inline void wlrail_ic_mirror(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    (void)bg;
    wlwin_fill(w, cx - 1, cy - 8, 2, 16, c);
    wlrail_line(w, cx - 3, cy - 5, cx - 8, cy, 2, c);
    wlrail_line(w, cx - 8, cy, cx - 3, cy + 5, 2, c);
    wlrail_line(w, cx + 3, cy - 5, cx + 8, cy, 2, c);
    wlrail_line(w, cx + 8, cy, cx + 3, cy + 5, 2, c);
}

static inline void wlrail_ic_pause(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    (void)bg;
    wlwin_fill(w, cx - 6, cy - 7, 4, 14, c);
    wlwin_fill(w, cx + 2, cy - 7, 4, 14, c);
}

static inline void wlrail_ic_size(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    (void)bg;
    wlwin_fill(w, cx - 8, cy - 8, 6, 2, c);
    wlwin_fill(w, cx - 8, cy - 8, 2, 6, c);
    wlwin_fill(w, cx + 2, cy - 8, 6, 2, c);
    wlwin_fill(w, cx + 6, cy - 8, 2, 6, c);
    wlwin_fill(w, cx - 8, cy + 6, 6, 2, c);
    wlwin_fill(w, cx - 8, cy + 2, 2, 6, c);
    wlwin_fill(w, cx + 2, cy + 6, 6, 2, c);
    wlwin_fill(w, cx + 6, cy + 2, 2, 6, c);
}

static inline void wlrail_ic_swap(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    (void)bg;
    wlwin_fill(w, cx - 7, cy - 4, 13, 2, c);
    wlrail_line(w, cx + 2, cy - 8, cx + 7, cy - 3, 2, c);
    wlrail_line(w, cx + 7, cy - 3, cx + 2, cy + 2, 2, c);
    wlwin_fill(w, cx - 6, cy + 3, 13, 2, c);
    wlrail_line(w, cx - 2, cy - 1, cx - 7, cy + 4, 2, c);
    wlrail_line(w, cx - 7, cy + 4, cx - 2, cy + 9, 2, c);
}

/* ---- painting ------------------------------------------------------------------------------------------------------------------------- */

static inline void wlrail_paint(struct wlwin *w, const struct wlrail *r, const struct wlrail_anim *an, int hover, int64_t now,
                                uint32_t bar, uint32_t tip_bg, uint32_t tip_rim, uint32_t tip_ink)
{
    int inset = w->inx;                                                 /* the rail is in window pixels, not in the shifted drawing area */
    w->inx = 0;
    wlwin_fill(w, r->x, r->y0, WLRAIL_W, r->y1 - r->y0, bar);
    int y[WLRAIL_MAX];
    wlrail_layout(r, y);
    int cx = r->x + WLRAIL_W / 2;
    for (int i = 0; i < r->n; i++) {                                    /* the ripples first: the drops lie on top of them */
        if (y[i] < 0 || !an->d[i].ripple_ms || now - an->d[i].ripple_ms >= 500)
            continue;
        float p = (float)(now - an->d[i].ripple_ms) / 500.0f;
        int rad = LM_MIN(17 + (int)(p * 9.0f), WLRAIL_W / 2 - 1), cy = y[i] + WLRAIL_D / 2;
        wlwin_round_fill(w, cx - rad, cy - rad, 2 * rad, 2 * rad, lm_button_radius(rad), lm_mix_rgb(r->it[i].lite, bar, (double)p));
        wlwin_round_fill(w, cx - rad + 2, cy - rad + 2, 2 * rad - 4, 2 * rad - 4, lm_button_radius(rad - 2), bar);
    }
    int hy = -1;
    for (int i = 0; i < r->n; i++) {
        if (y[i] < 0)
            continue;
        const struct wlrail_item *it = &r->it[i];
        uint32_t fill = it->idle ? lm_theme.control_idle : (hover == i || it->lit) ? it->lite : it->base;
        uint32_t ink = it->idle ? lm_theme.title_text_idle : lm_theme.on_colour;
        float sx = an->d[i].sx > 0 ? an->d[i].sx : 1, sy = an->d[i].sy > 0 ? an->d[i].sy : 1;
        int dw = LM_MAX((int)(WLRAIL_D * sx + 0.5f), 8), dh = LM_MAX((int)(WLRAIL_D * sy + 0.5f), 8);
        int cy = y[i] + WLRAIL_D / 2;
        wlwin_drop(w, cx - dw / 2, cy - dh / 2, dw, dh, lm_button_radius(LM_MIN(dw, dh) / 2), fill);        /* ([windows] button-roundness) */
        if (it->icon)
            it->icon(w, cx, cy - 1, ink, fill);
        else if (it->text)
            wlwin_text(w, cx - wlcard_text_w(it->text) / 2, cy - LM_FONT_H / 2 - 1, it->text, 1, ink, 0);
        if (hover == i)
            hy = y[i];
    }
    if (hover >= 0 && hy >= 0 && r->it[hover].tip) {                    /* the name of the button under the pointer, beside it */
        const char *name = r->it[hover].tip;
        int tw = wlcard_text_w(name) + 16;
        int tx = r->x == 0 ? WLRAIL_W + 3 : r->x - 3 - (tw + 2);
        wlwin_round_fill(w, tx, hy + 3, tw + 2, 26, 13, tip_rim);
        wlwin_round_fill(w, tx + 1, hy + 4, tw, 24, 12, tip_bg);
        wlwin_text(w, tx + 9, hy + (WLRAIL_D - LM_FONT_H) / 2, name, 1, tip_ink, 0);
    }
    w->inx = inset;
}

#endif
