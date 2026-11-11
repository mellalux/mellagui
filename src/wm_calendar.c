/* Part of the window manager (see wm_priv.h): the calendar */
#include "wm_priv.h"

/* ---- the calendar ------------------------------------------------------------------------------------------------------- */

struct lm_window *calendar_window(struct lm_wm *wm)
{
    struct lm_window *w;
    lm_list_for_each(w, &wm->windows, link)
        if (w->mapped && win_is_calendar(w) && !win_is_quick(w))
            return w;
    return NULL;
}

struct lm_window *quick_window(struct lm_wm *wm)
{
    struct lm_window *w;
    lm_list_for_each(w, &wm->windows, link)
        if (w->mapped && win_is_quick(w))
            return w;
    return NULL;
}

static bool quick_alive(struct lm_wm *wm)
{
    if (wm->quick_pid <= 0)
        return false;
    int st;
    pid_t r = waitpid(wm->quick_pid, &st, WNOHANG);
    if (r == wm->quick_pid || (r < 0 && kill(wm->quick_pid, 0) != 0)) {
        wm->quick_pid = 0;
        return false;
    }
    return true;
}

bool lm_wm_quick_toggle(struct lm_wm *wm, const struct lm_rect *anchor)
{
    const char *cmd = wm ? wm->srv->config.quick_cmd : NULL;
    if (!wm || !cmd || !*cmd || !strcmp(cmd, "none"))
        return false;
    if (anchor) {
        wm->quick_anchor = *anchor;
    } else {
        struct lm_output *o = lm_output_primary(wm->srv);
        if (!o)
            return false;
        struct lm_panel_info pi;
        panel_info(wm, o, &pi);
        wm->quick_anchor = pi.quick;
    }
    struct lm_window *w = quick_window(wm);
    if (!w) {                                                /* not there (yet, or it ended): start it and show it when its window comes */
        wm->quick_pending = true;
        wm->quick_pending_ms = lm_now_ms();
        if (!quick_alive(wm)) {
            wm->quick_pid = lm_server_spawn(wm->srv, cmd);
            if (wm->quick_pid < 0)
                wm->quick_pid = 0;
        }
        return true;
    }
    if (win_visible(wm, w)) {
        lm_wm_window_request_minimize(w);
        return true;
    }
    struct lm_output *o = lm_output_nearest(wm->srv, wm->quick_anchor.x, wm->quick_anchor.y);
    int32_t right = wm->quick_anchor.x + wm->quick_anchor.w;
    int32_t ox = o ? o->x : 0, ow = o ? o->width : 1280;
    w->x = right - LM_BORDER_WIDTH - w->width;
    if (w->x < ox + LM_BORDER_WIDTH)
        w->x = ox + LM_BORDER_WIDTH;
    if (w->x + w->width + LM_BORDER_WIDTH > ox + ow)
        w->x = ox + ow - w->width - LM_BORDER_WIDTH;
    w->y = wm->quick_anchor.y + wm->quick_anchor.h + 4 + LM_TITLEBAR_HEIGHT;
    w->workspace = wm->workspace;
    w->minimized = false;
    update_outputs(wm, w);
    raise_window(wm, w);
    damage_frame(wm, w);
    set_focus(wm, w);
    damage_panels(wm);
    refresh_pointer(wm);
    return true;
}

size_t lm_wm_quick_describe(struct lm_wm *wm, char *buf, size_t size)
{
    struct lm_output *o = lm_output_primary(wm->srv);
    if (!o || size == 0) {
        if (size)
            buf[0] = 0;
        return 0;
    }
    struct lm_panel_info pi;
    panel_info(wm, o, &pi);
    struct lm_window *w = quick_window(wm);
    size_t n = (size_t)snprintf(buf, size, "quick x=%d y=%d w=%d h=%d window=%d\n", pi.quick.x, pi.quick.y, pi.quick.w, pi.quick.h,
                                !w ? 0 : win_visible(wm, w) ? 2 : 1);
    return n < size ? n : size - 1;
}

static bool calendar_alive(struct lm_wm *wm)
{
    if (wm->cal_pid <= 0)
        return false;
    int st;
    pid_t r = waitpid(wm->cal_pid, &st, WNOHANG);
    if (r == wm->cal_pid || (r < 0 && kill(wm->cal_pid, 0) != 0)) {
        wm->cal_pid = 0;
        return false;
    }
    return true;
}

static void calendar_spawn(struct lm_wm *wm)
{
    const char *cmd = wm->srv->config.calendar_cmd;
    if (!wm->srv->config.cal_enabled || !cmd || !*cmd || calendar_alive(wm))
        return;
    wm->cal_pid = lm_server_spawn(wm->srv, cmd);
    if (wm->cal_pid < 0)
        wm->cal_pid = 0;
}

void lm_wm_calendar_start(struct lm_wm *wm)
{
    if (wm)
        calendar_spawn(wm);
}

bool lm_wm_calendar_toggle(struct lm_wm *wm, const struct lm_rect *clock)
{
    if (!wm || !wm->srv->config.cal_enabled || !wm->srv->config.calendar_cmd || !*wm->srv->config.calendar_cmd)
        return false;
    struct lm_output *o = NULL;
    if (clock) {
        wm->cal_clock = *clock;
    } else {
        o = lm_output_primary(wm->srv);
        if (!o)
            return false;
        struct lm_panel_info pi;
        panel_info(wm, o, &pi);
        wm->cal_clock = pi.clock;
    }
    struct lm_window *w = calendar_window(wm);
    if (!w) {                                                /* not there (yet, or it ended): start it and show it when its window comes */
        wm->cal_pending = true;
        wm->cal_pending_ms = lm_now_ms();
        calendar_spawn(wm);
        return true;
    }
    if (win_visible(wm, w)) {
        lm_wm_window_request_minimize(w);
        return true;
    }
    o = lm_output_nearest(wm->srv, wm->cal_clock.x, wm->cal_clock.y);
    int32_t right = wm->cal_clock.x + wm->cal_clock.w;
    int32_t ox = o ? o->x : 0, ow = o ? o->width : 1280;
    w->x = right - LM_BORDER_WIDTH - w->width;
    if (w->x < ox + LM_BORDER_WIDTH)
        w->x = ox + LM_BORDER_WIDTH;
    if (w->x + w->width + LM_BORDER_WIDTH > ox + ow)
        w->x = ox + ow - w->width - LM_BORDER_WIDTH;
    w->y = wm->cal_clock.y + wm->cal_clock.h + 4 + LM_TITLEBAR_HEIGHT;
    w->workspace = wm->workspace;
    w->minimized = false;
    update_outputs(wm, w);
    raise_window(wm, w);
    damage_frame(wm, w);
    set_focus(wm, w);
    damage_panels(wm);
    refresh_pointer(wm);
    return true;
}

bool lm_wm_has_fullscreen(const struct lm_wm *wm)
{
    struct lm_window *w;
    lm_list_for_each(w, &((struct lm_wm *)wm)->windows, link)
        if (win_visible(wm, w) && (w->states & LM_WINDOW_FULLSCREEN))
            return true;
    return false;
}

bool lm_wm_surface_visible(const struct lm_wm *wm, struct lm_surface *s)
{
    if (!wm || !s)
        return false;
    struct lm_surface *root = lm_surface_root(s);
    struct lm_window *w;
    lm_list_for_each(w, &((struct lm_wm *)wm)->windows, link)
        if (w->surface == root)
            return win_visible(wm, w);
    return false;
}

size_t lm_wm_dock_describe(struct lm_wm *wm, char *buf, size_t size)
{
    size_t o = 0;
    struct lm_output *out = dock_output(wm);
    struct lm_dock_info d;
    bool shown = out && dock_shown(wm);
    if (out)
        dock_info(wm, out, &d);
    o += (size_t)snprintf(buf + o, size - o, "dock shown=%d hover=%d n=%d drag=%d to=%d\n", shown, shown ? d.hover : -1, out ? d.n : 0,
                          wm->dock_drag ? wm->dock_drag_idx : -1, wm->dock_drag ? wm->dock_drag_to : -1);
    for (int i = 0; out && i < d.n && o < size; i++)
        o += (size_t)snprintf(buf + o, size - o, "icon %d \"%s\" x=%d y=%d w=%d h=%d running=%d launching=%d cmd=%s\n", i, d.icon[i].item->name,
                              d.icon[i].box.x, d.icon[i].box.y, d.icon[i].box.w, d.icon[i].box.h, d.icon[i].running, d.icon[i].launching, d.icon[i].item->cmd);
    return o < size ? o : size - 1;
}
