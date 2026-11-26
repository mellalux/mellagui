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

/* Dialog cards can be moved. Where there are card windows (wlwin.h) the card IS a window of its own: it opens where it would be drawn and can be carried
 * anywhere on the screen by its title; the program draws and hit-tests it in the window's coordinates all the same. Without them it stays inside the window:
 * a press on the title strip takes hold of the card, the pointer carries it, the button lets it go. The offset from the middle
 * of the window lives in the wlwin (card_dx / card_dy), so every program that uses a card gets it the same way and a card that closes is forgotten (wlwin.c
 * puts the offset back to 0 when a picture was made without a card). A program does three things:
 *   - its card's rectangle is made with wlcard_center(&a->win, &c) after c->w / c->h are set (not (bw - w) / 2),
 *   - a button press while a card is open first asks wlcard_drag_press(&a->win, &c, px, py) and ends the event when it says true,
 *   - a motion first asks wlcard_drag_motion(...) (true = it moved the card: redraw) and a release calls wlcard_drag_release(&a->win). */
#define WLCARD_TITLE_H 34

/* The card's place from its size: in the middle, plus what the user moved it, kept inside the window. */
static inline void wlcard_center(const struct wlwin *w, struct wlcard *c)
{
    int x = (w->bw - c->w) / 2 + w->card_dx, y = (w->bh - c->h) / 2 + w->card_dy;
    c->x = x < 0 ? 0 : x > w->bw - c->w ? (w->bw - c->w < 0 ? 0 : w->bw - c->w) : x;
    c->y = y < 0 ? 0 : y > w->bh - c->h ? (w->bh - c->h < 0 ? 0 : w->bh - c->h) : y;
}

/* A press on the card's title strip takes hold of it. */
static inline bool wlcard_drag_press(struct wlwin *w, const struct wlcard *c, double px, double py)
{
    if (px < c->x || px >= c->x + c->w || py < c->y || py >= c->y + WLCARD_TITLE_H)
        return false;
    if (wlwin_pane_shown(w, WLPANE_CARD)) {
        if (w->on_pane != WLPANE_CARD)
            return false;
        wlwin_pane_move(w, WLPANE_CARD);                        /* the compositor carries the card's window */
        return true;
    }
    w->card_drag = true;
    w->card_gx = px - c->x;
    w->card_gy = py - c->y;
    return true;
}

/* The pointer moved: while the card is held it follows (true: the program should repaint). */
static inline bool wlcard_drag_motion(struct wlwin *w, const struct wlcard *c, double px, double py)
{
    if (!w->card_drag)
        return false;
    double nx = px - w->card_gx, ny = py - w->card_gy, maxx = w->bw - c->w, maxy = w->bh - c->h;
    nx = nx < 0 ? 0 : nx > maxx ? (maxx < 0 ? 0 : maxx) : nx;
    ny = ny < 0 ? 0 : ny > maxy ? (maxy < 0 ? 0 : maxy) : ny;
    w->card_dx = (int)nx - (w->bw - c->w) / 2;
    w->card_dy = (int)ny - (w->bh - c->h) / 2;
    w->redraw = true;
    return true;
}

static inline void wlcard_drag_release(struct wlwin *w) { w->card_drag = false; }

/* From here to the end of the paint (or wlwin_pane_end) what is drawn belongs to the card whose top left corner is at (x, y): its own window where
 * there are card windows. For a program that draws a card's frame itself; wlcard_frame does it. `modal` false: the window behind stays in use. */
static inline void wlcard_pane(struct wlwin *w, int x, int y, int cw, int ch, int radius, uint32_t rim, bool modal)
{
    w->card_shown = true;
    wlwin_pane_begin(w, WLPANE_CARD, x - 1, y - 1, cw + 2, ch + 2, x - 1, y - 1, radius, rim, modal);
}

/* The same at a place of the program's choosing (x, y = the card's top left corner); `dim` false leaves the page behind it as it is (a panel that
 * floats over the picture it works on and can be moved: it must not darken it). */
static inline void wlcard_frame_at(struct wlwin *w, struct wlcard *c, int x, int y, int cw, int ch, const char *title, uint32_t accent, uint32_t body, uint32_t ink, bool dim)
{
    if (cw > w->bw - 16)
        cw = w->bw - 16;
    if (ch > w->bh - 16)
        ch = w->bh - 16;
    c->w = cw;
    c->h = ch;
    c->x = x;
    c->y = y;
    w->card_shown = true;
    if (dim)
        wlwin_dim(w, 0.55);
    wlcard_pane(w, c->x, c->y, cw, ch, 15, accent, !w->card_float);
    wlwin_round_fill(w, c->x - 1, c->y - 1, cw + 2, ch + 2, 15, accent);
    wlwin_round_fill(w, c->x, c->y, cw, ch, 14, body);
    wlwin_text(w, c->x + WLCARD_PAD, c->y + WLCARD_PAD - 2, title, 1, ink, cw - 2 * WLCARD_PAD);
}

static inline void wlcard_frame(struct wlwin *w, struct wlcard *c, int cw, int ch, const char *title, uint32_t accent, uint32_t body, uint32_t ink)
{
    if (cw > w->bw - 16)
        cw = w->bw - 16;
    if (ch > w->bh - 16)
        ch = w->bh - 16;
    struct wlcard at = { 0, 0, cw, ch };
    wlcard_center(w, &at);
    wlcard_frame_at(w, c, at.x, at.y, cw, ch, title, accent, body, ink, true);
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

/* Is the pointer on the card, for a panel that leaves the window behind it in use (wlcard_frame_at without dimming): when the card is a window of its own,
 * the place it was first drawn at belongs to the window again. */
static inline bool wlcard_over(const struct wlwin *w, const struct wlcard *c, int px, int py)
{
    return (!wlwin_pane_shown(w, WLPANE_CARD) || w->on_pane == WLPANE_CARD) && wlcard_has(c, px, py);
}

#endif
