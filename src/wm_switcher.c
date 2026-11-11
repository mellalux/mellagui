/* Part of the window manager (see wm_priv.h): Alt+Tab window switcher */
#include "wm_priv.h"

/* ---- Alt+Tab window switcher ------------------------------------------------------------------------------------------ */


int sw_first(const struct lm_wm *wm)            /* index of the first entry shown (the list scrolls to keep the selection) */
{
    if (wm->sw_n <= LM_SW_MAX)
        return 0;
    return LM_CLAMP(wm->sw_idx - LM_SW_MAX / 2, 0, wm->sw_n - LM_SW_MAX);
}

const char *sw_label(const struct lm_window *w)
{
    return w->title[0] ? w->title : w->app_id[0] ? w->app_id : "(untitled)";
}

struct lm_rect sw_rect(const struct lm_wm *wm)
{
    struct lm_output *o = NULL;
    if (wm->srv->backend && wm->srv->seat) {
        double x, y;
        lm_seat_pointer_position(wm->srv->seat, &x, &y);
        o = lm_output_nearest(wm->srv, x, y);
    }
    if (!o)
        return lm_rect_make(0, 0, 0, 0);
    int n = LM_MIN(wm->sw_n, LM_SW_MAX), first = sw_first(wm), chars = 12;
    for (int i = 0; i < n; i++)
        chars = LM_MAX(chars, LM_MIN((int)strlen(sw_label(wm->sw_win[first + i])), 56));
    int w = LM_MIN(chars * LM_FONT_W + 24 + 6 * LM_FONT_W, o->width - 16), h = n * SW_ITEM_H + 8;
    return lm_rect_make(o->x + (o->width - w) / 2, o->y + (o->height - h) / 2, w, h);
}

void sw_close(struct lm_wm *wm)
{
    if (!wm->sw_open)
        return;
    struct lm_rect r = sw_rect(wm);
    wm->sw_open = false;
    wm->sw_n = 0;
    damage_rect(wm, &r);
}

void sw_step(struct lm_wm *wm, bool backwards)
{
    menu_set(wm, false);
    struct lm_rect before = wm->sw_open ? sw_rect(wm) : lm_rect_make(0, 0, 0, 0);
    if (!wm->sw_open) {
        wm->sw_n = 0;
        struct lm_window *w;
        lm_list_for_each_reverse(w, &wm->windows, link)           /* top of the stack first, then by when it last had the focus */
            if (w->mapped && wm->sw_n < SW_MAX_WINDOWS && !win_is_calendar(w))
                wm->sw_win[wm->sw_n++] = w;
        for (int i = 1; i < wm->sw_n; i++) {                      /* stable insertion sort, most recently focused first */
            struct lm_window *t = wm->sw_win[i];
            int j = i;
            while (j > 0 && WIN(wm->sw_win[j - 1])->focus_seq < WIN(t)->focus_seq) {
                wm->sw_win[j] = wm->sw_win[j - 1];
                j--;
            }
            wm->sw_win[j] = t;
        }
        if (wm->sw_n < 2)
            return;
        wm->sw_open = true;
        wm->sw_idx = backwards ? wm->sw_n - 1 : 1;
    } else {
        wm->sw_idx = (wm->sw_idx + (backwards ? wm->sw_n - 1 : 1)) % wm->sw_n;
    }
    struct lm_rect now = sw_rect(wm);
    damage_rect(wm, &now);
    if (before.w > 0)
        damage_rect(wm, &before);
}

void sw_commit(struct lm_wm *wm)
{
    struct lm_window *w = wm->sw_open ? wm->sw_win[wm->sw_idx] : NULL;
    sw_close(wm);
    if (w)
        lm_wm_focus_window(wm, w);
}

struct lm_output *gadget_output(const struct lm_wm *wm)
{
    return lm_output_primary(wm->srv);
}
