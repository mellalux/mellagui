/* Part of the window manager (see wm_priv.h): the desktop overview */
#include "wm_priv.h"

/* ---- the desktop overview ----------------------------------------------------------------------------------------------- */

#define OV_STRIP_H 56                       /* the band of workspace cells at the top */
#define OV_LABEL_H 20
#define OV_GAP 28

struct lm_output *overview_output(const struct lm_wm *wm)
{
    if (!wm->ov.open || !wm->srv->backend)
        return NULL;
    struct lm_output *o;
    lm_list_for_each(o, &wm->srv->backend->outputs, link)
        if (!strcmp(o->name, wm->ov.out))
            return o;
    return NULL;
}

/* True while the window is shown as a picture in the overview (the scene and the picking leave the real one out). */
bool ov_hides(const struct lm_wm *wm, const struct lm_window *w)
{
    if (!wm->ov.open)
        return false;
    for (int i = 0; i < wm->ov.n; i++)
        if (wm->ov.item[i].win == w)
            return true;
    return false;
}

static void overview_free(struct lm_wm *wm)
{
    for (int i = 0; i < wm->ov.n; i++)
        lm_render_ghost_free(wm->srv->render, &wm->ov.item[i].ghost);
    wm->ov.n = 0;
}

void overview_damage(struct lm_wm *wm)
{
    struct lm_output *o = overview_output(wm);
    if (o) {
        struct lm_rect r = lm_output_rect(o);
        damage_rect(wm, &r);
    }
}

/* Slots for the pictures: the grid (rows of `cols`, the last row centred) that makes them largest, below the workspace cells. */
static void overview_layout(struct lm_wm *wm, struct lm_output *o)
{
    struct wm_overview *ov = &wm->ov;
    int n = ov->n;
    if (n == 0)
        return;
    int top = o->y + LM_PANEL_HEIGHT + OV_STRIP_H + 36, bottom = o->y + o->height - 24;
    struct lm_output *dock_o = dock_output(wm);
    if (dock_o == o) {
        struct dock_geo g;
        dock_geometry(o, &g);
        bottom = LM_MIN(bottom, g.strip.y - 12);
    }
    int left = o->x + 40, aw = o->width - 80, ah = LM_MAX(bottom - top, 120);
    int best_cols = 1;
    float best = -1;
    for (int cols = 1; cols <= n; cols++) {
        int rows = (n + cols - 1) / cols, cw = (aw - (cols - 1) * OV_GAP) / cols, ch = (ah - (rows - 1) * OV_GAP) / rows;
        if (cw < 48 || ch < 48 + OV_LABEL_H)
            continue;
        float sum = 0;
        for (int i = 0; i < n; i++) {
            float sx = (float)cw / (float)ov->item[i].home.w, sy = (float)(ch - OV_LABEL_H - 6) / (float)ov->item[i].home.h;
            sum += fminf(1.0f, fminf(sx, sy));
        }
        if (sum > best) {
            best = sum;
            best_cols = cols;
        }
    }
    int cols = best_cols, rows = (n + cols - 1) / cols, cw = (aw - (cols - 1) * OV_GAP) / cols, ch = (ah - (rows - 1) * OV_GAP) / rows;
    cw = LM_MAX(cw, 48);
    ov->cols = cols;
    for (int r = 0; r < rows; r++) {                             /* a row: the pictures side by side with a gap, the row centred */
        int first = r * cols, last = LM_MIN(first + cols, n), total = (last - first - 1) * OV_GAP;
        int ws[LM_OVERVIEW_MAX], hs[LM_OVERVIEW_MAX];
        for (int i = first; i < last; i++) {
            struct lm_rect hm = ov->item[i].home;
            float sc = fminf(1.0f, fminf((float)cw / (float)hm.w, (float)(ch - OV_LABEL_H - 6) / (float)hm.h));
            ws[i - first] = LM_MAX((int)((float)hm.w * sc), 8);
            hs[i - first] = LM_MAX((int)((float)hm.h * sc), 8);
            total += ws[i - first];
        }
        int x = left + (aw - total) / 2, y0 = top + r * (ch + OV_GAP);
        for (int i = first; i < last; i++) {
            ov->item[i].slot = lm_rect_make(x, y0 + (ch - OV_LABEL_H - 6 - hs[i - first]) / 2, ws[i - first], hs[i - first]);
            x += ws[i - first] + OV_GAP;
        }
    }
}

/* The pictures of this workspace's windows on the overview's output, in taskbar order. */
static void overview_build(struct lm_wm *wm)
{
    struct wm_overview *ov = &wm->ov;
    struct lm_output *o = overview_output(wm);
    struct lm_window *prev_sel = ov->sel >= 0 && ov->sel < ov->n ? ov->item[ov->sel].win : NULL;
    overview_free(wm);
    ov->hover = ov->hover_ws = ov->press = -1;
    ov->drag = false;
    if (!o)
        return;
    struct lm_window *list[LM_OVERVIEW_MAX];
    int n = 0;
    struct lm_window *w;
    lm_list_for_each(w, &wm->windows, link) {
        if (!w->mapped || w->workspace != wm->workspace || win_is_calendar(w) || (w->states & LM_WINDOW_FULLSCREEN) ||
            window_output(wm, w) != o || n >= LM_OVERVIEW_MAX)
            continue;
        int i = n++;
        for (; i > 0 && WIN(list[i - 1])->id > WIN(w)->id; i--)
            list[i] = list[i - 1];
        list[i] = w;
    }
    for (int i = 0; i < n; i++) {
        struct wm_ov_item *it = &ov->item[ov->n];
        memset(it, 0, sizeof(*it));
        if (!anim_prepare(wm, list[i], &it->ghost, &it->home))
            continue;                                           /* (no picture: the real window stays) */
        it->win = list[i];
        ov->n++;
    }
    ov->sel = ov->n > 0 ? ov->n - 1 : -1;
    for (int i = 0; i < ov->n; i++)
        if (ov->item[i].win == (prev_sel ? prev_sel : wm->focused))
            ov->sel = i;
    overview_layout(wm, o);
}

/* The state of the control socket / the scene: where picture i is now. */
static struct lm_rect overview_item_rect(const struct wm_overview *ov, int i)
{
    return anim_lerp_rect(&ov->item[i].home, &ov->item[i].slot, ease_smooth(ov->prog));
}

static void overview_info(const struct lm_wm *wm, struct lm_output *o, struct lm_overview_info *oi)
{
    const struct wm_overview *ov = &wm->ov;
    memset(oi, 0, sizeof(*oi));
    oi->rect = lm_output_rect(o);
    oi->dim = (int)(0xb4 * ease_smooth(ov->prog));
    oi->ui = !ov->closing && ov->prog >= 1.0f;
    int cw = LM_MIN(96, (o->width - 80) / LM_NUM_WORKSPACES - 12), total = LM_NUM_WORKSPACES * cw + (LM_NUM_WORKSPACES - 1) * 12;
    for (int i = 0; i < LM_NUM_WORKSPACES; i++) {
        oi->ws[i].rect = lm_rect_make(o->x + (o->width - total) / 2 + i * (cw + 12), o->y + LM_PANEL_HEIGHT + 14, cw, OV_STRIP_H - 14);
        oi->ws[i].active = i == wm->workspace;
        oi->ws[i].hover = i == ov->hover_ws;
        const struct lm_window *w;
        lm_list_for_each(w, &wm->windows, link)
            if (w->mapped && w->workspace == i && !win_is_calendar(w))
                oi->ws[i].count++;
    }
    oi->n = ov->n;
    for (int i = 0; i < ov->n; i++) {
        const struct lm_rect *s = &ov->item[i].slot;
        oi->item[i].frame = lm_rect_make(s->x - 6, s->y - 6, s->w + 12, s->h + 12 + OV_LABEL_H + 2);
        int lw = LM_MAX(s->w + 12, 96);
        oi->item[i].label = lm_rect_make(s->x + s->w / 2 - lw / 2, s->y + s->h + 6, lw, OV_LABEL_H - 2);
        snprintf(oi->item[i].title, sizeof(oi->item[i].title), "%s", ov->item[i].win->title);
        oi->item[i].sel = i == ov->sel;
        oi->item[i].hover = i == ov->hover;
        oi->item[i].minimized = ov->item[i].win->minimized;
    }
}

void overview_finish(struct lm_wm *wm)
{
    if (!wm->ov.open)
        return;
    overview_damage(wm);
    overview_free(wm);
    wm->ov.open = wm->ov.closing = false;
    damage_panels(wm);
    refresh_pointer(wm);
}

/* Closes the overview; `activate` >= 0 first goes to that picture's window (a minimized one comes back, without its own fly-in). */
static void overview_close(struct lm_wm *wm, int activate)
{
    struct wm_overview *ov = &wm->ov;
    if (!ov->open)
        return;
    if (activate >= 0 && activate < ov->n && !ov->closing) {
        wm->anim_suppress = true;
        lm_wm_focus_window(wm, ov->item[activate].win);
        wm->anim_suppress = false;
    }
    if (ov->closing)
        return;
    int dur = anim_duration(wm);
    if (dur <= 0 || ov->n == 0) {
        overview_finish(wm);
        return;
    }
    ov->closing = true;
    ov->from = ov->prog;
    ov->start = lm_now_ms();
    ov->dur = dur;
    overview_damage(wm);
    if (wm->anim_timer && !lm_timer_is_armed(wm->anim_timer))
        lm_timer_arm_ms(wm->anim_timer, ANIM_TICK_MS, ANIM_TICK_MS);
}

void overview_set(struct lm_wm *wm, bool open)
{
    struct wm_overview *ov = &wm->ov;
    if (!open) {
        overview_close(wm, -1);
        return;
    }
    if (lm_lock_active(wm->srv->lock) || !wm->srv->backend)
        return;
    if (ov->open) {
        if (ov->closing) {                                      /* asked again while it flies back: it turns round */
            ov->closing = false;
            ov->from = ov->prog;
            ov->start = lm_now_ms();
            ov->dur = anim_duration(wm);
            if (wm->anim_timer && !lm_timer_is_armed(wm->anim_timer))
                lm_timer_arm_ms(wm->anim_timer, ANIM_TICK_MS, ANIM_TICK_MS);
        }
        return;
    }
    menu_set(wm, false);
    about_set(wm, false, NULL);
    launcher_set(wm, false, NULL);
    power_set(wm, false, NULL);
    sw_close(wm);
    clip_nav_set(wm, false);
    if (wm->focused)
        lm_xdg_dismiss_popups(wm->focused->surface);
    struct lm_output *o = NULL;
    if (wm->srv->seat) {
        double px, py;
        lm_seat_pointer_position(wm->srv->seat, &px, &py);
        o = lm_output_nearest(wm->srv, (int32_t)px, (int32_t)py);
    }
    if (!o)
        o = lm_output_primary(wm->srv);
    if (!o)
        return;
    memset(ov, 0, sizeof(*ov));
    snprintf(ov->out, sizeof(ov->out), "%s", o->name);
    ov->open = true;
    ov->sel = -1;
    overview_build(wm);
    ov->dur = anim_duration(wm);
    ov->start = lm_now_ms();
    ov->from = 0;
    ov->prog = ov->dur > 0 ? 0.0f : 1.0f;
    overview_damage(wm);
    damage_panels(wm);
    if (ov->dur > 0 && wm->anim_timer && !lm_timer_is_armed(wm->anim_timer))
        lm_timer_arm_ms(wm->anim_timer, ANIM_TICK_MS, ANIM_TICK_MS);
    refresh_pointer(wm);
}

/* The set of windows changed (one mapped, went away, moved to another workspace, the workspace was switched): take the pictures again. */
void overview_changed(struct lm_wm *wm)
{
    if (!wm->ov.open)
        return;
    if (wm->ov.closing) {
        overview_finish(wm);
        return;
    }
    overview_build(wm);
    overview_damage(wm);
}

/* True while it still moves (the timer keeps running). */
bool overview_tick(struct lm_wm *wm, uint64_t now)
{
    struct wm_overview *ov = &wm->ov;
    if (!ov->open)
        return false;
    float target = ov->closing ? 0.0f : 1.0f;
    if (ov->dur <= 0 || ov->prog == target)
        return false;
    float p = fminf((float)(now - ov->start) / (float)ov->dur, 1.0f);
    ov->prog = ov->from + (target - ov->from) * p;
    overview_damage(wm);
    if (p >= 1.0f) {
        ov->prog = target;
        if (ov->closing)
            overview_finish(wm);
        return false;
    }
    return true;
}

bool overview_pick(const struct lm_wm *wm, int ix, int iy, struct lm_pick *out)
{
    struct lm_output *o = overview_output(wm);
    if (!o || lm_lock_active(wm->srv->lock) || wm->ov.closing)
        return false;
    struct lm_rect orc = lm_output_rect(o);
    if (!lm_rect_contains(&orc, ix, iy) || iy < o->y + LM_PANEL_HEIGHT)
        return false;                                           /* (the panel above stays in use) */
    struct lm_overview_info oi;
    overview_info(wm, o, &oi);
    out->kind = LM_PICK_OVERVIEW;
    out->rect = orc;
    for (int i = 0; i < LM_NUM_WORKSPACES; i++)
        if (lm_rect_contains(&oi.ws[i].rect, ix, iy)) {
            out->kind = LM_PICK_OVERVIEW_WS;
            out->index = i;
            return true;
        }
    for (int i = wm->ov.n - 1; i >= 0; i--)
        if (lm_rect_contains(&oi.item[i].frame, ix, iy)) {
            out->kind = LM_PICK_OVERVIEW_ITEM;
            out->index = i;
            out->window = wm->ov.item[i].win;
            return true;
        }
    return true;
}

static void overview_switch_workspace(struct lm_wm *wm, int ws)
{
    if (ws < 0 || ws >= LM_NUM_WORKSPACES || ws == wm->workspace)
        return;
    lm_wm_switch_workspace(wm, ws);                              /* (its hook takes the pictures of the new workspace) */
}

void overview_key(struct lm_wm *wm, uint32_t code, bool repeat, uint32_t mods)
{
    struct wm_overview *ov = &wm->ov;
    int n = ov->n, sel = ov->sel;
    if (ov->closing)
        return;
    if (!repeat && (code == KEY_ESC || (code == KEY_W && (mods & LM_MOD_SUPER)))) {
        overview_close(wm, -1);
        return;
    }
    if (code >= KEY_1 && code < KEY_1 + LM_NUM_WORKSPACES && !repeat) {
        overview_switch_workspace(wm, (int)(code - KEY_1));
        return;
    }
    if (n == 0)
        return;
    int step = 0;
    if (code == KEY_RIGHT || (code == KEY_TAB && !(mods & LM_MOD_SHIFT))) step = 1;
    else if (code == KEY_LEFT || (code == KEY_TAB && (mods & LM_MOD_SHIFT))) step = -1;
    else if (code == KEY_DOWN) step = ov->cols;
    else if (code == KEY_UP) step = -ov->cols;
    if (step) {
        int to = sel < 0 ? 0 : sel + step;
        if (abs(step) > 1 && (to < 0 || to >= n))
            to = sel;                                           /* (Up / Down stop at the first / last row) */
        else
            to = (to % n + n) % n;
        if (to != ov->sel) {
            ov->sel = to;
            overview_damage(wm);
        }
        return;
    }
    if (!repeat && (code == KEY_ENTER || code == KEY_KPENTER || code == KEY_SPACE) && sel >= 0)
        overview_close(wm, sel);
    else if (!repeat && (code == KEY_DELETE) && sel >= 0)
        lm_wm_window_close(ov->item[sel].win);
}

/* A press on a picture that moves 8 px turns into a drag: the picture follows the pointer and can be dropped on a workspace cell. */
void overview_drag_motion(struct lm_wm *wm)
{
    struct wm_overview *ov = &wm->ov;
    if (!ov->open || ov->closing || wm->press_panel_kind != LM_PICK_OVERVIEW_ITEM || !wm->srv->seat)
        return;
    double x, y;
    lm_seat_pointer_position(wm->srv->seat, &x, &y);
    if (!ov->drag) {
        if (!drag_far_enough(wm, x, y) || wm->press_panel_ws < 0 || wm->press_panel_ws >= ov->n)
            return;
        ov->drag = true;
        ov->drag_i = wm->press_panel_ws;
        ov->hover = -1;
    }
    wm->drag_x = x;
    wm->drag_y = y;
    overview_damage(wm);
}

/* The button was let go after a drag: on a workspace cell the window moves there. */
void overview_drag_finish(struct lm_wm *wm, const struct lm_pick *pick)
{
    struct wm_overview *ov = &wm->ov;
    int i = ov->drag_i;
    ov->drag = false;
    overview_damage(wm);
    if (pick->kind == LM_PICK_OVERVIEW_WS && i >= 0 && i < ov->n && pick->index != ov->item[i].win->workspace)
        move_to_workspace(wm, ov->item[i].win, pick->index);              /* (its hook takes the pictures again) */
}

/* A click (`button`, release) on a pick of the overview. */
void overview_click(struct lm_wm *wm, const struct lm_pick *pick, uint32_t button)
{
    if (pick->kind == LM_PICK_OVERVIEW_ITEM && pick->index >= 0 && pick->index < wm->ov.n) {
        if (button == BTN_LEFT)
            overview_close(wm, pick->index);
        else if (button == BTN_MIDDLE)
            lm_wm_window_close(wm->ov.item[pick->index].win);
    } else if (pick->kind == LM_PICK_OVERVIEW_WS && button == BTN_LEFT) {
        overview_switch_workspace(wm, pick->index);
    } else if (pick->kind == LM_PICK_OVERVIEW && button == BTN_LEFT) {
        overview_close(wm, -1);
    }
}

void lm_wm_overview_set(struct lm_wm *wm, bool open) { overview_set(wm, open); }
bool lm_wm_overview_is_open(const struct lm_wm *wm) { return wm->ov.open; }

size_t lm_wm_overview_describe(struct lm_wm *wm, char *buf, size_t size)
{
    const struct wm_overview *ov = &wm->ov;
    size_t n = (size_t)snprintf(buf, size, "overview open=%d closing=%d n=%d sel=%d cols=%d prog=%.2f ws=%d\n", ov->open, ov->closing, ov->n, ov->sel,
                                ov->cols, (double)ov->prog, wm->workspace);
    for (int i = 0; i < ov->n && n < size; i++) {
        struct lm_rect r = overview_item_rect(ov, i);
        n += (size_t)snprintf(buf + n, size - n, "item %d x=%d y=%d w=%d h=%d slot=%d,%d,%d,%d title=\"%s\"\n", i, r.x, r.y, r.w, r.h, ov->item[i].slot.x,
                              ov->item[i].slot.y, ov->item[i].slot.w, ov->item[i].slot.h, ov->item[i].win->title);
    }
    return n < size ? n : size - 1;
}

/* A window has just been mapped: its picture grows out of nothing while the real one waits. */
void anim_open(struct lm_wm *wm, struct lm_window *w)
{
    struct lm_ghost g;
    struct lm_rect home;
    if (!anim_wanted(wm, w) || !anim_prepare(wm, w, &g, &home))
        return;
    anim_start(wm, ANIM_OPEN, w, &g, &home);
}

/* Minimize (kind MINIMIZE, before the window is hidden) or restore (RESTORE, before it is shown): the picture flies between the window and its button. */
void anim_fly(struct lm_wm *wm, enum anim_kind kind, struct lm_window *w)
{
    struct lm_ghost g;
    struct lm_rect home;
    if (!anim_wanted(wm, w) || !w->mapped || w->workspace != wm->workspace || !anim_prepare(wm, w, &g, &home))
        return;
    anim_start(wm, kind, w, &g, &home);
}

/* The window was asked to close: keep its picture, a close that follows (within 3 s) shows it fading away. */
void anim_remember_close(struct lm_wm *wm, struct lm_window *w)
{
    if (!anim_wanted(wm, w) || !w->mapped || w->minimized || w->workspace != wm->workspace)
        return;
    struct lm_ghost *g = calloc(1, sizeof(*g));
    if (!g)
        return;
    if (!anim_prepare(wm, w, g, &w->close_rect)) {
        free(g);
        return;
    }
    if (w->close_ghost) {
        free(w->close_ghost->px);
        free(w->close_ghost);
    }
    w->close_ghost = g;
    w->close_ms = lm_now_ms();
}

/* The window goes away (unmapped): shows the picture taken when it was asked to close, if there is a fresh one. */
void anim_play_close(struct lm_wm *wm, struct lm_window *w)
{
    if (wm->wb.active && wm->wb.win == w)
        wobble_stop(wm);
    for (int i = 0; i < ANIM_MAX; i++)                              /* an opening or flying picture of it ends now */
        if (wm->anim[i].used && wm->anim[i].win == w)
            anim_end(wm, &wm->anim[i]);
    struct lm_ghost *g = w->close_ghost;
    if (!g)
        return;
    w->close_ghost = NULL;
    if (anim_wanted(wm, w) && !w->minimized && w->workspace == wm->workspace && lm_now_ms() - w->close_ms < 3000)
        anim_start(wm, ANIM_CLOSE, NULL, g, &w->close_rect);
    free(g->px);
    free(g);
}

size_t lm_wm_anim_describe(struct lm_wm *wm, char *buf, size_t size)
{
    static const char *const names[] = { "open", "close", "minimize", "restore" };
    size_t n = 0;
    int count = 0;
    for (int i = 0; i < ANIM_MAX; i++)
        count += wm->anim[i].used;
    n += (size_t)snprintf(buf + n, size - n, "anim n=%d dur=%d wobble=%d\n", count, anim_duration(wm), wm->wb.active);
    for (int i = 0; i < ANIM_MAX && n < size; i++) {
        const struct wm_anim *a = &wm->anim[i];
        if (!a->used)
            continue;
        n += (size_t)snprintf(buf + n, size - n, "item kind=%s x=%d y=%d w=%d h=%d alpha=%d\n", names[a->kind], a->shown.x, a->shown.y,
                              a->shown.w, a->shown.h, a->alpha);
    }
    if (wm->bubble.active && n < size)                           /* the launcher's bubble: "bubble opening|closing prog=0..1 x= y= w= h=" (the box that covers it) */
        n += (size_t)snprintf(buf + n, size - n, "bubble %s prog=%.3f x=%d y=%d w=%d h=%d\n", wm->bubble.closing ? "closing" : "opening", (double)wm->bubble.prog,
                              wm->bubble.shown.x, wm->bubble.shown.y, wm->bubble.shown.w, wm->bubble.shown.h);
    return n < size ? n : size - 1;
}

void lm_wm_scene_for_each(struct lm_wm *wm, const struct lm_rect *clip, lm_scene_fn fn, void *data)
{
    struct lm_scene_item it;
    struct lm_output *o;

    if (lm_lock_active(wm->srv->lock)) {                         /* locked: nothing of the session may show */
        struct lm_window *shield = NULL;                         /* ... except the screensaver that locked the screen and runs on over the lock card */
        if (wm->srv->saver && lm_saver_shields(wm->srv->saver)) {
            struct lm_window *sw;
            lm_list_for_each(sw, &wm->windows, link)
                if (win_visible(wm, sw) && (sw->states & LM_WINDOW_FULLSCREEN))
                    shield = sw;                                 /* (the topmost one) */
        }
        struct lm_rect shield_rect = { 0 };
        if (shield)
            shield_rect = frame_outer(shield);
        if (wm->srv->backend) {
            lm_list_for_each(o, &wm->srv->backend->outputs, link) {
                struct lm_rect orect = lm_output_rect(o);
                if (clip && !lm_rect_overlaps(&orect, clip))
                    continue;
                if (shield && lm_rect_overlaps(&orect, &shield_rect))
                    continue;                                    /* the saver fills this output */
                memset(&it, 0, sizeof(it));
                it.kind = LM_SCENE_LOCK;
                it.output = o;
                lm_lock_info(wm->srv->lock, o, &it.u.lock);
                if (!fn(&it, data))
                    return;
            }
        }
        if (shield) {
            int32_t ox, oy;
            win_origin(shield, &ox, &oy);
            memset(&it, 0, sizeof(it));
            it.kind = LM_SCENE_SURFACE;
            it.u.surface.root = shield->surface;
            it.u.surface.x = ox;
            it.u.surface.y = oy;
            if (!fn(&it, data))
                return;
        }
        return;
    }

    if (wm->srv->backend) {
        lm_list_for_each(o, &wm->srv->backend->outputs, link) {
            struct lm_rect orect = lm_output_rect(o);
            if (clip && !lm_rect_overlaps(&orect, clip))
                continue;
            memset(&it, 0, sizeof(it));
            it.kind = LM_SCENE_WALLPAPER;
            it.output = o;
            if (!fn(&it, data))
                return;
        }
    }

    for (int gid = 0; gid < LM_GADGET_COUNT; gid++) {            /* bottom to top: the mixer is above the clipboard */
        struct lm_rect gr;
        if (!lm_wm_gadget_rect_id(wm, gid, &gr))
            continue;
        struct lm_rect gb = gadget_bounds(&gr);
        if (clip && !lm_rect_overlaps(&gb, clip))
            continue;
        memset(&it, 0, sizeof(it));
        it.kind = LM_SCENE_GADGET;
        it.u.gadget.id = gid;
        it.u.gadget.rect = gr;
        it.u.gadget.title_h = GADGET_TITLE_H;
        it.u.gadget.dragging = (wm->grab_kind == GRAB_GADGET_MOVE || wm->grab_kind == GRAB_GADGET_RESIZE) && wm->grab_gadget == gid;
        if (gid == LM_GADGET_MIXER) {
            if (wm->srv->mixer)
                it.u.gadget.mixer = lm_mixer_state(wm->srv->mixer);
        } else if (gid == LM_GADGET_SYSTEM) {
            if (wm->srv->sysinfo)
                it.u.gadget.sys = lm_sysinfo_state(wm->srv->sysinfo);
        } else {
            gadget_slots(gr.h, &it.u.gadget.slots, &it.u.gadget.entry_h);
            int total = lm_seat_clip_count(wm->srv->seat);
            int top = clip_top(wm);
            it.u.gadget.total = total;
            it.u.gadget.top = top;
            int n = LM_MAX(LM_MIN(total - top, it.u.gadget.slots), 0);
            it.u.gadget.n = n;
            it.u.gadget.cursor = wm->clip_nav && wm->clip_cur >= top && wm->clip_cur < top + n ? wm->clip_cur - top : -1;
            it.u.gadget.hover = wm->hover_clip;
            clip_buttons(&gr, it.u.gadget.btn);
            for (int i = 0; i < n; i++)
                it.u.gadget.text[i] = lm_seat_clip_get(wm->srv->seat, top + i);
        }
        if (!fn(&it, data))
            return;
    }

    struct lm_window *w;
    lm_list_for_each(w, &wm->windows, link) {
        if (!win_visible(wm, w) || w->anim_hidden || ov_hides(wm, w))
            continue;
        struct lm_rect outer = frame_outer(w);
        int32_t ox, oy;
        win_origin(w, &ox, &oy);
        struct lm_rect sb = outer, ext;
        if (lm_surface_tree_extents(w->surface, &ext)) {
            struct lm_rect eb = lm_rect_make(ox + ext.x, oy + ext.y, ext.w, ext.h);
            sb = lm_rect_union(&outer, &eb);
        }
        if (clip && !lm_rect_overlaps(&sb, clip))
            continue;
        if (!(w->states & LM_WINDOW_FULLSCREEN) && (!clip || lm_rect_overlaps(&outer, clip))) {
            memset(&it, 0, sizeof(it));
            it.kind = LM_SCENE_FRAME;
            frame_info(wm, w, &it.u.frame);
            if (!fn(&it, data))
                return;
        }
        memset(&it, 0, sizeof(it));
        it.kind = LM_SCENE_SURFACE;
        it.u.surface.root = w->surface;
        it.u.surface.x = ox;
        it.u.surface.y = oy;
        if (!fn(&it, data))
            return;
    }

    for (int i = 0; i < ANIM_MAX; i++) {                         /* pictures of windows that open / close / fly to their button */
        struct wm_anim *a = &wm->anim[i];
        if (!a->used || (clip && !lm_rect_overlaps(&a->shown, clip)))
            continue;
        memset(&it, 0, sizeof(it));
        it.kind = LM_SCENE_GHOST;
        it.u.ghost.ghost = &a->ghost;
        it.u.ghost.dst = a->shown;
        it.u.ghost.alpha = a->alpha;
        if (!fn(&it, data))
            return;
    }

    if (wm->wb.active) {                                         /* the wobbling window: its picture cut into cells that follow the spring nodes */
        const struct wm_wobble *wb = &wm->wb;
        const int stride = WB_COLS + 1;
        for (int j = 0; j < WB_ROWS; j++)
            for (int i = 0; i < WB_COLS; i++) {
                int n[4] = { j * stride + i, j * stride + i + 1, (j + 1) * stride + i, (j + 1) * stride + i + 1 };
                float x0 = wb->x[n[0]], x1 = x0, y0 = wb->y[n[0]], y1 = y0;
                for (int k = 1; k < 4; k++) {
                    x0 = fminf(x0, wb->x[n[k]]);
                    x1 = fmaxf(x1, wb->x[n[k]]);
                    y0 = fminf(y0, wb->y[n[k]]);
                    y1 = fmaxf(y1, wb->y[n[k]]);
                }
                struct lm_rect dst = lm_rect_make((int)floorf(x0), (int)floorf(y0), (int)ceilf(x1) - (int)floorf(x0), (int)ceilf(y1) - (int)floorf(y0));
                if (clip && !lm_rect_overlaps(&dst, clip))
                    continue;
                int sx0 = wb->ghost.w * i / WB_COLS, sx1 = wb->ghost.w * (i + 1) / WB_COLS;
                int sy0 = wb->ghost.h * j / WB_ROWS, sy1 = wb->ghost.h * (j + 1) / WB_ROWS;
                memset(&it, 0, sizeof(it));
                it.kind = LM_SCENE_GHOST;
                it.u.ghost.ghost = (struct lm_ghost *)&wb->ghost;
                it.u.ghost.dst = dst;
                it.u.ghost.src = lm_rect_make(sx0, sy0, sx1 - sx0, sy1 - sy0);
                it.u.ghost.alpha = 255;
                if (!fn(&it, data))
                    return;
            }
    }

    if (wm->ov.open) {                                           /* the desktop overview: the dimmed output, then the windows' pictures */
        struct lm_output *oo = overview_output(wm);
        if (oo) {
            struct lm_rect orc = lm_output_rect(oo);
            if (!clip || lm_rect_overlaps(&orc, clip)) {
                memset(&it, 0, sizeof(it));
                it.kind = LM_SCENE_OVERVIEW;
                it.output = oo;
                overview_info(wm, oo, &it.u.overview);
                if (!fn(&it, data))
                    return;
                for (int pass = 0; pass < 2; pass++)                  /* the carried picture last, above the others */
                    for (int i = 0; i < wm->ov.n; i++) {
                        bool carried = wm->ov.drag && i == wm->ov.drag_i;
                        if (carried != (pass == 1))
                            continue;
                        memset(&it, 0, sizeof(it));
                        it.kind = LM_SCENE_GHOST;
                        it.u.ghost.ghost = (struct lm_ghost *)&wm->ov.item[i].ghost;
                        it.u.ghost.dst = overview_item_rect(&wm->ov, i);
                        it.u.ghost.alpha = wm->ov.item[i].win->minimized ? 170 : 255;
                        if (carried) {                                /* its place in the grid stays as a faint copy, the picture itself is small and follows the pointer */
                            struct lm_rect home = it.u.ghost.dst;
                            it.u.ghost.alpha = 70;
                            if (!clip || lm_rect_overlaps(&home, clip))
                                if (!fn(&it, data))
                                    return;
                            int cw = LM_MAX(home.w * 55 / 100, 24), chh = LM_MAX(home.h * 55 / 100, 16);
                            it.u.ghost.dst = lm_rect_make((int)wm->drag_x - cw / 2, (int)wm->drag_y - chh / 2, cw, chh);
                            it.u.ghost.alpha = 235;
                        }
                        if (clip && !lm_rect_overlaps(&it.u.ghost.dst, clip))
                            continue;
                        if (!fn(&it, data))
                            return;
                    }
            }
        }
    }

    if (wm->srv->backend) {
        lm_list_for_each(o, &wm->srv->backend->outputs, link) {
            struct lm_rect pr = output_panel_rect(o);
            if (fullscreen_covers(wm, o))
                continue;
            struct lm_rect sr = lm_rect_make(o->x, o->y + LM_PANEL_HEIGHT, o->width, LM_PANEL_SHADOW);
            if (!clip || lm_rect_overlaps(&sr, clip)) {
                memset(&it, 0, sizeof(it));
                it.kind = LM_SCENE_PANEL_SHADOW;
                it.output = o;
                it.u.shadow = sr;
                if (!fn(&it, data))
                    return;
            }
            if (clip && !lm_rect_overlaps(&pr, clip))
                continue;
            memset(&it, 0, sizeof(it));
            it.kind = LM_SCENE_PANEL;
            it.output = o;
            panel_info(wm, o, &it.u.panel);
            if (!fn(&it, data))
                return;
        }
    }

    struct lm_output *dock_out = dock_output(wm);
    if (dock_out && dock_shown(wm)) {
        struct dock_geo g;
        dock_geometry(dock_out, &g);
        if (!lm_rect_is_empty(&g.bounds)) {
            memset(&it, 0, sizeof(it));
            it.kind = LM_SCENE_DOCK;
            it.output = dock_out;
            dock_info(wm, dock_out, &it.u.dock);
            if ((!clip || lm_rect_overlaps(&it.u.dock.rect, clip)) && !fn(&it, data))
                return;
        }
    }
    struct lm_popup *p;
    lm_list_for_each(p, &wm->popups, link) {
        if (!popup_visible(wm, p, 0))
            continue;
        struct lm_rect pb = popup_bounds(p);
        if (clip && !lm_rect_overlaps(&pb, clip))
            continue;
        memset(&it, 0, sizeof(it));
        it.kind = LM_SCENE_SURFACE;
        it.u.surface.root = p->surface;
        popup_origin(p, &it.u.surface.x, &it.u.surface.y);
        it.u.surface.is_popup = true;
        if (!fn(&it, data))
            return;
    }
    if (wm->nnotes > 0) {                                        /* notification cards, above the windows and the panel */
        struct lm_rect rs[LM_NOTIFY_MAX];
        if (note_rects(wm, rs)) {
            for (int i = wm->nnotes - 1; i >= 0; i--) {              /* oldest first: the newest is on top */
                if (clip && !lm_rect_overlaps(&rs[i], clip))
                    continue;
                memset(&it, 0, sizeof(it));
                it.kind = LM_SCENE_NOTIFY;
                it.u.notify.rect = rs[i];
                it.u.notify.id = wm->notes[i].id;
                snprintf(it.u.notify.title, sizeof(it.u.notify.title), "%s", wm->notes[i].title);
                it.u.notify.nlines = note_wrap(wm->notes[i].body, it.u.notify.line);
                if (!fn(&it, data))
                    return;
            }
        }
    }

    if (wm->power_open) {                                        /* the session dialog */
        struct lm_output *po = power_output(wm);
        if (po) {
            struct lm_rect r = lm_output_rect(po);
            if (!clip || lm_rect_overlaps(&r, clip)) {
                memset(&it, 0, sizeof(it));
                it.kind = LM_SCENE_POWER;
                it.output = po;
                power_info(wm, po, &it.u.power);
                if (!fn(&it, data))
                    return;
            }
        }
    }

    if (wm->about_open) {                                        /* the About dialog */
        struct lm_output *ao = about_output(wm);
        if (ao) {
            struct lm_rect r = lm_output_rect(ao);
            if (!clip || lm_rect_overlaps(&r, clip)) {
                memset(&it, 0, sizeof(it));
                it.kind = LM_SCENE_ABOUT;
                it.output = ao;
                about_info(wm, ao, &it.u.about);
                if (!fn(&it, data))
                    return;
            }
        }
    }

    if (wm->bubble.active) {                                     /* the launcher is a bubble: the dimming, then the card's picture in cells, then the sheen */
        const struct wm_bubble *b = &wm->bubble;
        struct lm_output *bo = bubble_output(wm);
        struct lm_rect orc = bo ? lm_output_rect(bo) : (struct lm_rect){ 0 };
        if (bo && (!clip || lm_rect_overlaps(&orc, clip))) {
            memset(&it, 0, sizeof(it));
            it.kind = LM_SCENE_LAUNCHER;
            it.output = bo;
            launcher_info(wm, bo, &it.u.launcher);
            it.u.launcher.veil_only = true;
            it.u.launcher.veil = smooth01(b->prog);
            if (!fn(&it, data))
                return;
        }
        float s = b->s;
        if (b->frame.px && (!clip || lm_rect_overlaps(&b->shown, clip))) {
            memset(&it, 0, sizeof(it));
            it.kind = LM_SCENE_GHOST;
            it.u.ghost.ghost = (struct lm_ghost *)&b->frame;
            it.u.ghost.dst = b->shown;
            it.u.ghost.alpha = b->alpha;
            if (!fn(&it, data))
                return;
        }
        float sheen_a = powf(1.0f - LM_CLAMP(s, 0.0f, 1.0f), 1.3f);
        if (b->sheen.px && sheen_a > 0.02f) {
            float ccx = (float)b->card.x + (float)b->card.w / 2.0f, ccy = (float)b->card.y + (float)b->card.h / 2.0f, pe = smooth01(b->prog);
            float R = BUBBLE_R0 + (0.32f * (float)LM_MIN(b->card.w, b->card.h) - BUBBLE_R0) * pe;
            float cx = b->cx0 + (ccx - b->cx0) * pe, cy = b->cy0 + (ccy - b->cy0) * pe;
            struct lm_rect dst = lm_rect_make((int)lroundf(cx - R), (int)lroundf(cy - R), (int)lroundf(2.0f * R), (int)lroundf(2.0f * R));
            if (!clip || lm_rect_overlaps(&dst, clip)) {
                memset(&it, 0, sizeof(it));
                it.kind = LM_SCENE_GHOST;
                it.u.ghost.ghost = (struct lm_ghost *)&b->sheen;
                it.u.ghost.dst = dst;
                it.u.ghost.alpha = (int)lroundf(255.0f * sheen_a * fminf(b->prog / 0.05f, 1.0f));
                if (!fn(&it, data))
                    return;
            }
        }
    }

    if (wm->launcher_open && !(wm->bubble.active && !wm->bubble.closing)) {     /* the launcher dialog (not while it is still a bubble) */
        struct lm_output *lo = launcher_output(wm);
        if (lo) {
            struct lm_rect r = lm_output_rect(lo);
            if (!clip || lm_rect_overlaps(&r, clip)) {
                memset(&it, 0, sizeof(it));
                it.kind = LM_SCENE_LAUNCHER;
                it.output = lo;
                launcher_info(wm, lo, &it.u.launcher);
                if (!fn(&it, data))
                    return;
            }
        }
    }

    if (wm->menu_open && !lm_lock_active(wm->srv->lock) && (!clip || lm_rect_overlaps(&wm->menu_rect, clip))) {    /* a menu: above the dialogs */
        memset(&it, 0, sizeof(it));
        it.kind = LM_SCENE_MENU;
        it.output = lm_output_nearest(wm->srv, wm->menu_rect.x + wm->menu_rect.w / 2, wm->menu_rect.y + wm->menu_rect.h / 2);
        if (it.output) {
            menu_info(wm, &it.u.menu);
            if (!fn(&it, data))
                return;
        }
    }


    if (wm->osk_open) {                                          /* the on-screen keyboard: above the dialogs, below the Alt+Tab list */
        struct lm_osk_geo og;
        if (osk_geo(wm, &og) && (!clip || lm_rect_overlaps(&og.card, clip))) {
            memset(&it, 0, sizeof(it));
            it.kind = LM_SCENE_OSK;
            it.output = lm_output_primary(wm->srv);
            osk_info(wm, &og, &it.u.osk);
            if (!fn(&it, data))
                return;
        }
    }

    if (wm->sw_open) {                                           /* the Alt+Tab list, above everything */
        struct lm_rect r = sw_rect(wm);
        if (r.w > 0 && (!clip || lm_rect_overlaps(&r, clip))) {
            memset(&it, 0, sizeof(it));
            it.kind = LM_SCENE_SWITCHER;
            it.u.switcher.rect = r;
            it.u.switcher.n = LM_MIN(wm->sw_n, LM_SW_MAX);
            it.u.switcher.total = wm->sw_n;
            it.u.switcher.item_h = SW_ITEM_H;
            it.u.switcher.current_ws = wm->workspace;
            int first = sw_first(wm);
            it.u.switcher.sel = wm->sw_idx - first;
            for (int i = 0; i < it.u.switcher.n; i++) {
                const struct lm_window *w = wm->sw_win[first + i];
                it.u.switcher.label[i] = sw_label(w);
                it.u.switcher.ws[i] = w->workspace;
                it.u.switcher.minimized[i] = w->minimized;
                it.u.switcher.focused[i] = w == wm->focused;
            }
            if (!fn(&it, data))
                return;
        }
    }
}

static void append(char *buf, size_t size, size_t *n, const char *fmt, ...) LM_PRINTF(4, 5);
static void append(char *buf, size_t size, size_t *n, const char *fmt, ...)
{
    if (*n + 1 >= size)
        return;
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(buf + *n, size - *n, fmt, ap);
    va_end(ap);
    if (k > 0)
        *n += (size_t)k < size - *n ? (size_t)k : size - *n - 1;
}

size_t lm_wm_describe(struct lm_wm *wm, char *buf, size_t size)
{
    if (size == 0)
        return 0;
    size_t n = 0;
    double px = 0, py = 0;
    if (wm->srv->seat)
        lm_seat_pointer_position(wm->srv->seat, &px, &py);
#define APPEND(...) append(buf, size, &n, __VA_ARGS__)
    APPEND("workspace %d\n", wm->workspace);
    APPEND("pointer %d %d\n", (int)px, (int)py);
    struct lm_window *w;
    lm_list_for_each(w, &wm->windows, link) {
        char title[96], app[64];
        snprintf(title, sizeof(title), "%s", w->title);
        snprintf(app, sizeof(app), "%s", w->app_id);
        for (char *c = title; *c; c++) if (*c == '"' || *c == '\n') *c = '\'';
        for (char *c = app; *c; c++) if (*c == '"' || *c == '\n') *c = '\'';
        char flags[96] = "";
        if (w->mapped) strcat(flags, "mapped,");
        if (wm->focused == w) strcat(flags, "focused,");
        if (w->states & LM_WINDOW_MAXIMIZED) strcat(flags, "maximized,");
        if (w->states & LM_WINDOW_FULLSCREEN) strcat(flags, "fullscreen,");
        if (WIN(w)->tiled) strcat(flags, WIN(w)->tiled == LM_EDGE_LEFT ? "tiled-left," : "tiled-right,");
        if (w->minimized) strcat(flags, "minimized,");
        size_t fl = strlen(flags);
        if (fl) flags[fl - 1] = 0; else strcpy(flags, "-");
        APPEND("window %d \"%s\" \"%s\" x=%d y=%d w=%d h=%d ws=%d flags=%s\n", WIN(w)->id,
               title, app, w->x, w->y, w->width, w->height, w->workspace, flags);
    }
#undef APPEND
    if (n >= size)
        n = size - 1;
    buf[n] = 0;
    return n;
}
