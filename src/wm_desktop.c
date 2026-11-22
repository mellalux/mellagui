/* Part of the window manager (see wm_priv.h): the desktop's icons.
 *
 * The icons of the folder ~/Desktop are a program of their own, `mellagui-desktop` ([commands] desktop), started with the compositor. Its window
 * (app id "mellagui.desktop") has a role in the wm like a gadget's, with two differences: it covers the whole output under the panel (the wm fits it:
 * `desktop_fit`, at the map and whenever an output changes), and it lies under the gadgets, at the very bottom of the stack (`desktop_lower`), so the
 * cards and every window are above it. It is on every workspace, has no frame, is never raised, minimized, maximized or moved, and is left out of
 * every list (win_is_aux). A click on it gives it the keyboard focus (so Enter, F2, Delete work on the chosen icon), which takes the focus away from
 * the window that had it: the desktop is not a window the wm ever falls back on (top_visible skips it).
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#include "wm_priv.h"

/* App ids: "mellagui.desktop" is the desktop of the first output (the leftmost), "mellagui.desktop.N" that of the N-th from the left. */
bool win_is_desktop(const struct lm_window *w)
{
    return w->app_id && !strncmp(w->app_id, "mellagui.desktop", 16) && (w->app_id[16] == 0 || w->app_id[16] == '.');
}

static int desktop_index(const struct lm_window *w)
{
    return w->app_id[16] == '.' ? atoi(w->app_id + 17) : 0;
}

/* The idx-th output from the left (then from the top); NULL when there are not that many. */
static struct lm_output *output_by_index(struct lm_wm *wm, int idx)
{
    struct lm_output *picked[8];
    int n = 0;
    struct lm_output *o;
    if (!wm->srv->backend)
        return NULL;
    lm_list_for_each(o, &wm->srv->backend->outputs, link) {
        int k = n;
        while (k > 0 && (picked[k - 1]->x > o->x || (picked[k - 1]->x == o->x && picked[k - 1]->y > o->y))) {
            if (k < 8)
                picked[k] = picked[k - 1];
            k--;
        }
        if (k < 8) {
            picked[k] = o;
            if (n < 8)
                n++;
        }
    }
    return idx >= 0 && idx < n ? picked[idx] : NULL;
}

static int output_count(struct lm_wm *wm)
{
    int n = 0;
    struct lm_output *o;
    if (wm->srv->backend)
        lm_list_for_each(o, &wm->srv->backend->outputs, link)
            n++;
    return n;
}

/* The first output's desktop window, or NULL. */
struct lm_window *desktop_window(struct lm_wm *wm)
{
    struct lm_window *w;
    lm_list_for_each(w, &wm->windows, link)
        if (w->mapped && win_is_desktop(w) && desktop_index(w) == 0)
            return w;
    return NULL;
}

/* The rectangle a desktop has: its output below the panel. false when there is no such output. */
static bool desktop_rect(struct lm_wm *wm, int idx, struct lm_rect *r)
{
    struct lm_output *o = output_by_index(wm, idx);
    if (!o)
        return false;
    *r = lm_rect_make(o->x, o->y + LM_PANEL_HEIGHT, o->width, LM_MAX(o->height - LM_PANEL_HEIGHT, 1));
    return true;
}

/* Puts every desktop window where it belongs (under the panel of its output, that output's size) and asks its program to draw it at that size. */
void desktop_fit(struct lm_wm *wm)
{
    struct lm_window *w;
    lm_list_for_each(w, &wm->windows, link) {
        struct lm_rect r;
        if (!w->mapped || !win_is_desktop(w) || !desktop_rect(wm, desktop_index(w), &r))
            continue;
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
}

/* The desktops are the bottom of the stack: under the gadgets, which are put at the bottom when they come. */
void desktop_lower(struct lm_wm *wm)
{
    int k = 0;
    struct lm_window *w;
    lm_list_for_each(w, &wm->windows, link)
        if (win_is_desktop(w))
            k++;
    if (!k)
        return;
    int lead = 0;                                               /* already the first k windows of the stack? */
    lm_list_for_each(w, &wm->windows, link) {
        if (!win_is_desktop(w))
            break;
        lead++;
    }
    if (lead == k)
        return;
    struct lm_window *list[8];
    int n = 0;
    lm_list_for_each(w, &wm->windows, link)
        if (win_is_desktop(w) && n < 8)
            list[n++] = w;
    for (int i = n - 1; i >= 0; i--) {
        lm_list_remove(&list[i]->link);
        lm_list_insert(&wm->windows, &list[i]->link);
        damage_frame(wm, list[i]);
    }
}

void desktop_map(struct lm_wm *wm, struct lm_window *w)
{
    WIN(w)->placed = true;
    struct lm_rect r;
    if (desktop_rect(wm, desktop_index(w), &r)) {
        w->x = r.x;
        w->y = r.y;
    }
    w->mapped = true;
    w->workspace = wm->workspace;
    w->minimized = false;
    lm_list_remove(&w->link);
    lm_list_insert(&wm->windows, &w->link);
    desktop_lower(wm);
    desktop_fit(wm);
    update_outputs(wm, w);
    damage_frame(wm, w);
    refresh_pointer(wm);
}

/* The window of the first output's desktop: x, y, width, height (what the control socket's `desktop` shows). */
bool lm_wm_desktop_rect(struct lm_wm *wm, struct lm_rect *r)
{
    struct lm_window *w = desktop_window(wm);
    if (!w)
        return false;
    *r = lm_rect_make(w->x, w->y, w->width, w->height);
    return true;
}

/* ---- one program for each output --------------------------------------------------------------------------------------------------- */

static bool desk_alive(struct lm_wm *wm, int i)
{
    if (wm->desk_pid[i] <= 0)
        return false;
    int st;
    pid_t r = waitpid(wm->desk_pid[i], &st, WNOHANG);
    if (r == wm->desk_pid[i] || (r < 0 && kill(wm->desk_pid[i], 0) != 0)) {
        wm->desk_pid[i] = 0;
        return false;
    }
    return true;
}

/* The first output's desktop is started by main.c; here the others: `mellagui-desktop --output N` for the N-th output from the left, ended again when
 * the output goes (a monitor that comes back gets a new one). */
void lm_wm_desktops_sync(struct lm_wm *wm)
{
    if (!wm)
        return;
    struct lm_server *srv = wm->srv;
    const char *cmd = srv->config.desktop_cmd;
    wm->desk_started = true;
    bool off = !cmd || !*cmd || !strcmp(cmd, "none") || srv->config.kiosk ||
               (!strcmp(srv->config.backend, "headless") && !getenv("MELLAGUI_DESKTOP_PRELOAD"));
    int n = output_count(wm);
    for (int i = 1; i < 8; i++) {
        bool want = !off && i < n;
        if (want && !desk_alive(wm, i)) {
            char full[1100];
            snprintf(full, sizeof(full), "%s --output %d", cmd, i);
            wm->desk_pid[i] = lm_server_spawn(srv, full);
            if (wm->desk_pid[i] < 0)
                wm->desk_pid[i] = 0;
        } else if (!want && desk_alive(wm, i)) {
            kill(wm->desk_pid[i], SIGTERM);
            wm->desk_pid[i] = 0;
        }
    }
}
