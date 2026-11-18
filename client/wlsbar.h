/*
 * wlsbar.h - the scroll bar of a list, the same everywhere: `[windows] scrollbar-width` (lm_metrics.scrollbar_width, 14 px by default) wide, a rounded
 * thumb inset by 2 px in the theme's `control` colour (`control_hot` while the pointer is on the bar or the thumb is held), as long as the part of the list in
 * view; a press on the thumb takes it (drag it), a press on the track beside it turns a page. Header only, no state of its own: the program keeps `top` and the
 * grab. A list gets this - not a 3 px strip of its own; a program makes the room for it (the rows end `wlsbar_w()` earlier while the bar is there).
 *
 *   struct wlsbar s = { x, y, w, h, n, vis, top };      the track, the number of lines / pixels there are, how many show, the first one that does
 *   wlsbar_on(&s)                                        is there something to scroll
 *   wlsbar_paint(win, &s, hot)
 *   wlsbar_press(&s, px, py, &top, &grab)                0 not on the bar, 1 the thumb was taken (`grab` = where in it), 2 a page (`top` is changed)
 *   top = wlsbar_drag(&s, py, grab)                      while the thumb is held
 * Units are the program's: lines for a list of rows, pixels for a page of text; a page is `vis`.
 */
#ifndef MELLAGUI_WLSBAR_H
#define MELLAGUI_WLSBAR_H

#include <stdbool.h>

#include "metrics.h"
#include "theme.h"
#include "wlwin.h"

struct wlsbar {
    int x, y, w, h;                     /* the track: w is wlsbar_w() */
    int n, vis, top;
};

static inline int wlsbar_w(void) { return lm_metrics.scrollbar_width; }

static inline bool wlsbar_on(const struct wlsbar *s) { return s->vis > 0 && s->n > s->vis && s->h > 8; }

static inline void wlsbar_thumb(const struct wlsbar *s, int *ty, int *th)
{
    int t = (int)((long long)s->h * s->vis / (s->n > 0 ? s->n : 1));
    t = t < 24 ? 24 : t > s->h ? s->h : t;
    int maxtop = s->n - s->vis, top = s->top < 0 ? 0 : s->top > maxtop ? maxtop : s->top;
    *th = t;
    *ty = s->y + (maxtop > 0 ? (int)((long long)(s->h - t) * top / maxtop) : 0);
}

static inline void wlsbar_paint(struct wlwin *w, const struct wlsbar *s, bool hot)
{
    if (!wlsbar_on(s))
        return;
    int ty, th;
    wlsbar_thumb(s, &ty, &th);
    wlwin_round_fill(w, s->x + 2, ty, s->w - 4, th, (s->w - 4) / 2, hot ? lm_theme.control_hot : lm_theme.control);
}

static inline bool wlsbar_has(const struct wlsbar *s, double px, double py)
{
    return wlsbar_on(s) && px >= s->x && px < s->x + s->w && py >= s->y && py < s->y + s->h;
}

static inline int wlsbar_press(const struct wlsbar *s, double px, double py, int *top, int *grab)
{
    if (!wlsbar_has(s, px, py))
        return 0;
    int ty, th;
    wlsbar_thumb(s, &ty, &th);
    int maxtop = s->n - s->vis;
    if (py >= ty && py < ty + th) {
        *grab = (int)py - ty;
        return 1;
    }
    int t = s->top + (py < ty ? -s->vis : s->vis);
    *top = t < 0 ? 0 : t > maxtop ? maxtop : t;
    return 2;
}

static inline int wlsbar_drag(const struct wlsbar *s, double py, int grab)
{
    int ty, th;
    wlsbar_thumb(s, &ty, &th);
    int room = s->h - th > 1 ? s->h - th : 1, maxtop = s->n - s->vis;
    int t = (int)((long long)((int)py - grab - s->y) * maxtop / room);
    return t < 0 ? 0 : t > maxtop ? maxtop : t;
}

#endif
