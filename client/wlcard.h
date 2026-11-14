/* wlcard - the look of a modal card for the wlwin programs (header only): the page behind it dimmed, a round card with an accent rim and a title,
 * and drop-like pill buttons at its bottom right (a darker rim, the body, a glint at the upper left: the same look as the editor's rail).
 * A program lays the contents out itself inside the card and handles its own keys; the card only draws the frame and finds the buttons.
 *
 *   struct wlcard c;  wlcard_frame(w, &c, width, height, "Title", accent, body, ink);   (dims the window, centres the card)
 *   wlcard_buttons(w, &c, labels, n, colours, hover, down)                              the buttons, the LAST one is the main one, at the right
 *   wlcard_button_at(&c, n, x, y)                                                       which button (0..n-1) is at the point, -1 none
 *   wlcard_has(&c, x, y)                                                                the point is on the card (a click outside cancels)
 */
#ifndef MELLAGUI_WLCARD_H
#define MELLAGUI_WLCARD_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "font.h"
#include "uikit.h"
#include "wlwin.h"

struct wlcard {
    int x, y, w, h;
};

#define WLCARD_PAD 16
#define WLCARD_BTN_W 88
#define WLCARD_BTN_H 28

static inline int wlcard_text_w(const char *s)
{
    return lm_font_ui_cells_get() ? (int)strlen(s) * LM_FONT_W : lm_font_text_width(s);
}

static inline void wlcard_frame(struct wlwin *w, struct wlcard *c, int cw, int ch, const char *title, uint32_t accent, uint32_t body, uint32_t ink)
{
    if (cw > w->bw - 16)
        cw = w->bw - 16;
    if (ch > w->bh - 16)
        ch = w->bh - 16;
    c->w = cw;
    c->h = ch;
    c->x = (w->bw - cw) / 2;
    c->y = (w->bh - ch) / 2;
    wlwin_dim(w, 0.55);
    wlwin_round_fill(w, c->x - 1, c->y - 1, cw + 2, ch + 2, 15, accent);
    wlwin_round_fill(w, c->x, c->y, cw, ch, 14, body);
    wlwin_text(w, c->x + WLCARD_PAD, c->y + WLCARD_PAD - 2, title, 1, ink, cw - 2 * WLCARD_PAD);
}

static inline void wlcard_button_rect(const struct wlcard *c, int k, int n, int *x, int *y, int *bw, int *bh)
{
    *bw = WLCARD_BTN_W;
    *bh = WLCARD_BTN_H;
    *x = c->x + c->w - WLCARD_PAD - (n - k) * WLCARD_BTN_W - (n - 1 - k) * 8;       /* (8 px between buttons) */
    *y = c->y + c->h - WLCARD_PAD - WLCARD_BTN_H;
}

/* One drop-like pill: `down` squashes it, `hot` lights it. */
static inline void wlcard_drop(struct wlwin *w, int x, int y, int bw, int bh, uint32_t fill, bool hot, bool down, const char *label, uint32_t ink)
{
    int dw = down ? bw + 4 : bw, dh = down ? bh - 4 : bh;
    int x0 = x - (dw - bw) / 2, y0 = y + (bh - dh) / 2;
    wlwin_drop(w, x0, y0, dw, dh, lm_button_radius(dh / 2), hot ? lm_mix_rgb(fill, 0xffffff, 0.18) : fill);
    wlwin_text(w, x0 + (dw - wlcard_text_w(label)) / 2, y0 + (dh - 2 - LM_FONT_H) / 2, label, 1, ink, 0);
}

static inline void wlcard_buttons(struct wlwin *w, const struct wlcard *c, const char *const *labels, int n, const uint32_t *colours, int hot, int down, uint32_t ink)
{
    for (int k = 0; k < n; k++) {
        int x, y, bw, bh;
        wlcard_button_rect(c, k, n, &x, &y, &bw, &bh);
        wlcard_drop(w, x, y, bw, bh, colours[k], hot == k, down == k, labels[k], ink);
    }
}

static inline int wlcard_button_at(const struct wlcard *c, int n, int px, int py)
{
    for (int k = 0; k < n; k++) {
        int x, y, bw, bh;
        wlcard_button_rect(c, k, n, &x, &y, &bw, &bh);
        if (px >= x && px < x + bw && py >= y && py < y + bh)
            return k;
    }
    return -1;
}

static inline bool wlcard_has(const struct wlcard *c, int px, int py)
{
    return px >= c->x && px < c->x + c->w && py >= c->y && py < c->y + c->h;
}

#endif
