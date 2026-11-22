/* Part of the window manager (see wm_priv.h): the desktop's icons.
 *
 * The icons of the folder ~/Desktop are a program of their own, `mellagui-desktop` ([commands] desktop), started with the compositor. Its window
 * (app id "mellagui.desktop") has a role in the wm like a gadget's, with two differences: it covers the whole output under the panel (the wm fits it:
 * `desktop_fit`, at the map and whenever an output changes), and it lies under the gadgets, at the very bottom of the stack (`desktop_lower`), so the
 * cards and every window are above it. It is on every workspace, has no frame, is never raised, minimized, maximized or moved, and is left out of
 * every list (win_is_aux). A click on it gives it the keyboard focus (so Enter, F2, Delete work on the chosen icon), which takes the focus away from
 * the window that had it: the desktop is not a window the wm ever falls back on (top_visible skips it).
 */
#include <string.h>

#include "wm_priv.h"

bool win_is_desktop(const struct lm_window *w)
{
    return w->app_id && !strcmp(w->app_id, "mellagui.desktop");
}

/* The window of the desktop, or NULL. */
struct lm_window *desktop_window(struct lm_wm *wm)
{
    struct lm_window *w;
    lm_list_for_each(w, &wm->windows, link)
        if (w->mapped && win_is_desktop(w))
            return w;
    return NULL;
}

/* The rectangle the icons have: the primary output below the panel. false when there is no output. */
static bool desktop_rect(struct lm_wm *wm, struct lm_rect *r)
{
    struct lm_output *o = lm_output_primary(wm->srv);
    if (!o)
        return false;
    *r = lm_rect_make(o->x, o->y + LM_PANEL_HEIGHT, o->width, LM_MAX(o->height - LM_PANEL_HEIGHT, 1));
    return true;
}

/* Puts the desktop window where it belongs (under the panel, the size of the output) and asks the program to draw it at that size. */
void desktop_fit(struct lm_wm *wm)
{
    struct lm_window *w = desktop_window(wm);
    struct lm_rect r;
    if (!w || !desktop_rect(wm, &r))
        return;
    if (w->x != r.x || w->y != r.y) {
        struct lm_rect before = frame_outer(w);
        w->x = r.x;
        w->y = r.y;
        damage_rect(wm, &before);
        damage_frame(wm, w);
        update_outputs(wm, w);
    }
    if (w->width != r.w || w->height != r.h)
        send_configure(wm, w, r.w, r.h);
}

/* The desktop is the bottom of the stack: under the gadgets, which are put at the bottom when they come. */
void desktop_lower(struct lm_wm *wm)
{
    struct lm_window *w = desktop_window(wm);
    if (!w || wm->windows.next == &w->link)
        return;
    lm_list_remove(&w->link);
    lm_list_insert(&wm->windows, &w->link);
    damage_frame(wm, w);
}

void desktop_map(struct lm_wm *wm, struct lm_window *w)
{
    WIN(w)->placed = true;
    struct lm_rect r;
    if (desktop_rect(wm, &r)) {
        w->x = r.x;
        w->y = r.y;
    }
    w->mapped = true;
    w->workspace = wm->workspace;
    w->minimized = false;
    lm_list_remove(&w->link);
    lm_list_insert(&wm->windows, &w->link);
    desktop_fit(wm);
    update_outputs(wm, w);
    damage_frame(wm, w);
    refresh_pointer(wm);
}

/* The window of the desktop: x, y, width, height (what the control socket's `desktop` shows). */
bool lm_wm_desktop_rect(struct lm_wm *wm, struct lm_rect *r)
{
    struct lm_window *w = desktop_window(wm);
    if (!w)
        return false;
    *r = lm_rect_make(w->x, w->y, w->width, w->height);
    return true;
}
