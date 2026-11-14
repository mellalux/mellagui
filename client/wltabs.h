/*
 * wltabs.h: the logic of a row of tabs (terminal, editor): where the tabs lie, which one a pointer is over, and the drag of a tab
 * (reorder inside the bar, or pull it out of the bar to make it a window of its own). Nothing is painted here: every program
 * draws its tabs with its own helpers, in LOGICAL pixels, from the same numbers.
 *
 *   bar:   x0 .. x0 + w, y .. y + WLTABS_H
 *   tab i: wltabs_x(x0, tw, i), width tw (tw = wltabs_tw(w, n)), 2 px apart; the "+" button after the last one
 *   drag:  press on a tab -> wltabs_press; motion -> wltabs_motion (returns a new place for the tab when it crossed a neighbour);
 *          release -> wltabs_release: WLTABS_CLICK (it never moved: a plain click), WLTABS_MOVED (it was only reordered) or
 *          WLTABS_DETACH (it was let go away from the bar: make it a window of its own)
 */
#ifndef LM_WLTABS_H
#define LM_WLTABS_H

#include <stdbool.h>

#include "font.h"
#include "util.h"

#define WLTABS_H (LM_FONT_H + 8)            /* the bar's height */
#define WLTABS_PLUS_W 26                    /* the "+" button */
#define WLTABS_MIN_W 30
#define WLTABS_MAX_W 200
#define WLTABS_CLOSE_W 16                   /* the x mark at the right end of a tab */
#define WLTABS_PAD 4
#define WLTABS_OUT 28                       /* a tab let go this far (px) beyond the bar's top or bottom edge leaves the window */

enum { WLTABS_NONE = -1, WLTABS_PLUS = -2 };
enum { WLTABS_CLICK, WLTABS_MOVED, WLTABS_DETACH };

/* The width of one tab for `n` tabs in a bar `w` wide. */
static inline int wltabs_tw(int w, int n)
{
    int avail = w - 2 * WLTABS_PAD - WLTABS_PLUS_W - 2 * n;
    return LM_CLAMP(n > 0 ? avail / n : WLTABS_MAX_W, WLTABS_MIN_W, WLTABS_MAX_W);
}

static inline int wltabs_x(int x0, int tw, int i) { return x0 + WLTABS_PAD + i * (tw + 2); }
static inline int wltabs_plus_x(int x0, int tw, int n) { return wltabs_x(x0, tw, n); }

/* Which tab (0..n-1), WLTABS_PLUS or WLTABS_NONE is at (px, py); *on_close is set when it is over the tab's x mark. */
static inline int wltabs_hit(double px, double py, int x0, int y, int w, int n, bool *on_close)
{
    if (on_close)
        *on_close = false;
    if (py < y || py >= y + WLTABS_H || px < x0 || px >= x0 + w)
        return WLTABS_NONE;
    int tw = wltabs_tw(w, n);
    for (int i = 0; i < n; i++) {
        int tx = wltabs_x(x0, tw, i);
        if (px >= tx && px < tx + tw) {
            if (on_close)
                *on_close = tw >= 60 && px >= tx + tw - WLTABS_CLOSE_W;
            return i;
        }
    }
    int px0 = wltabs_plus_x(x0, tw, n);
    if (px >= px0 && px < px0 + WLTABS_PLUS_W)
        return WLTABS_PLUS;
    return WLTABS_NONE;
}

struct wltabs_drag {
    bool active;            /* a tab is held down */
    bool moved;             /* the pointer went further than a click's wobble */
    bool out;               /* the pointer is away from the bar: letting go now makes a window of the tab */
    int idx;                /* the tab being dragged (its place changes while it crosses the others) */
    double press_x, press_y;
    double grab;            /* pixels between the tab's left edge and the pointer at the press */
    double x, y;            /* the pointer now */
};

static inline void wltabs_press(struct wltabs_drag *d, int idx, double px, double py, int x0, int w, int n)
{
    int tw = wltabs_tw(w, n);
    d->active = true;
    d->moved = d->out = false;
    d->idx = idx;
    d->press_x = d->x = px;
    d->press_y = d->y = py;
    d->grab = px - wltabs_x(x0, tw, idx);
}

/* The pointer moved with the tab held. Returns the place the tab now has (the caller moves it there) or -1 if it stays. */
static inline int wltabs_motion(struct wltabs_drag *d, double px, double py, int x0, int y, int w, int n)
{
    d->x = px;
    d->y = py;
    if (!d->moved && (px - d->press_x) * (px - d->press_x) + (py - d->press_y) * (py - d->press_y) >= 36)
        d->moved = true;
    d->out = d->moved && (py < y - WLTABS_OUT || py >= y + WLTABS_H + WLTABS_OUT || px < x0 - WLTABS_OUT || px >= x0 + w + WLTABS_OUT);
    if (!d->moved || d->out || n < 2)
        return -1;
    int tw = wltabs_tw(w, n);
    double centre = px - d->grab + tw / 2.0;                    /* where the middle of the held tab is */
    int to = LM_CLAMP((int)((centre - x0 - WLTABS_PAD) / (tw + 2)), 0, n - 1);
    if (to == d->idx)
        return -1;
    d->idx = to;
    return to;
}

static inline int wltabs_release(struct wltabs_drag *d)
{
    int r = !d->moved ? WLTABS_CLICK : d->out ? WLTABS_DETACH : WLTABS_MOVED;
    d->active = d->moved = d->out = false;
    return r;
}

#endif
