/*
 * Window manager + shell policy: the core. See wm.h for the contract and wm_priv.h for how the wm is split into files.
 */
#include "wm_priv.h"
#include "smarthome.h"

/* ---- geometry helpers ------------------------------------------------------------ */

struct lm_rect frame_outer(const struct lm_window *w)
{
    if ((w->states & LM_WINDOW_FULLSCREEN) || win_is_gadget(w))        /* (a gadget has no frame) */
        return lm_rect_make(w->x, w->y, w->width, w->height);
    return lm_rect_make(w->x - LM_BORDER_WIDTH, w->y - LM_TITLEBAR_HEIGHT,
                        w->width + 2 * LM_BORDER_WIDTH,
                        w->height + LM_TITLEBAR_HEIGHT + LM_BORDER_WIDTH);
}

bool win_visible(const struct lm_wm *wm, const struct lm_window *w)
{
    return w->mapped && !w->minimized && w->workspace == wm->workspace;
}

/* The calendar's and the quick settings' windows: they live hidden (minimized) and are not listed in the taskbar, Alt+Tab or the dock. */
bool win_is_quick(const struct lm_window *w)
{
    return w->app_id && !strcmp(w->app_id, "mellagui.quick");
}

bool win_is_gadget(const struct lm_window *w)
{
    return w->app_id && !strncmp(w->app_id, "mellagui.gadget.", 16);
}

bool win_is_aux(const struct lm_window *w)
{
    return win_is_calendar(w) || win_is_gadget(w);
}

bool win_is_calendar(const struct lm_window *w)
{
    return w->app_id && (!strcmp(w->app_id, "mellagui.calendar") || !strcmp(w->app_id, "mellagui.quick"));
}

void win_origin(const struct lm_window *w, int32_t *ox, int32_t *oy)
{
    *ox = w->x - w->geometry.x;
    *oy = w->y - w->geometry.y;
}

void popup_origin(const struct lm_popup *p, int32_t *ox, int32_t *oy)
{
    *ox = p->x - p->geometry.x;
    *oy = p->y - p->geometry.y;
}

void frame_info(const struct lm_wm *wm, const struct lm_window *w, struct lm_frame_info *f)
{
    memset(f, 0, sizeof(*f));
    f->outer = frame_outer(w);
    f->titlebar = lm_rect_make(f->outer.x, f->outer.y, f->outer.w, LM_TITLEBAR_HEIGHT);
    int by = f->outer.y + (LM_TITLEBAR_HEIGHT - LM_FRAME_BUTTON_SIZE) / 2;
    int right = f->outer.x + f->outer.w - LM_FRAME_BUTTON_GAP;
    f->btn_close = lm_rect_make(right - LM_FRAME_BUTTON_SIZE, by, LM_FRAME_BUTTON_SIZE, LM_FRAME_BUTTON_SIZE);
    f->btn_max = lm_rect_make(f->btn_close.x - LM_FRAME_BUTTON_GAP - LM_FRAME_BUTTON_SIZE, by,
                              LM_FRAME_BUTTON_SIZE, LM_FRAME_BUTTON_SIZE);
    f->btn_min = lm_rect_make(f->btn_max.x - LM_FRAME_BUTTON_GAP - LM_FRAME_BUTTON_SIZE, by,
                              LM_FRAME_BUTTON_SIZE, LM_FRAME_BUTTON_SIZE);
    int tx = f->outer.x + LM_BORDER_WIDTH;
    f->title_text = lm_rect_make(tx, f->outer.y, f->btn_min.x - LM_FRAME_BUTTON_GAP - tx, LM_TITLEBAR_HEIGHT);
    f->title = w->title;
    f->active = wm->focused == w;
    f->maximized = (w->states & LM_WINDOW_MAXIMIZED) != 0;
    if (wm->hover_win == w)
        f->hover = wm->hover_bits;
    if (wm->press_win == w)
        f->pressed = wm->press_bits;
}

void move_to_workspace(struct lm_wm *wm, struct lm_window *w, int ws);
struct lm_rect output_panel_rect(const struct lm_output *o)
{
    return lm_rect_make(o->x, o->y, o->width, LM_PANEL_HEIGHT);
}

void panel_info(const struct lm_wm *wm, struct lm_output *o, struct lm_panel_info *p)
{
    memset(p, 0, sizeof(*p));
    p->output = o;
    p->bar = output_panel_rect(o);
    int y = p->bar.y + 2, h = p->bar.h - 4;
    p->launcher = lm_rect_make(p->bar.x + 4, y, h, h);              /* the round logo button */
    int x = p->launcher.x + p->launcher.w + 10;
    for (int i = 0; i < LM_NUM_WORKSPACES; i++) {
        p->workspace[i] = lm_rect_make(x, y, PANEL_WS_W, h);
        x += PANEL_WS_W + 2;
    }
    p->power = lm_rect_make(p->bar.x + p->bar.w - PANEL_POWER_D - 4, y, PANEL_POWER_D, PANEL_POWER_D);
    int clock_w = LM_MAX(PANEL_CLOCK_MIN_W, lm_font_text_width(wm->clock_text) + 8);
    p->clock = lm_rect_make(p->power.x - 4 - clock_w, y, clock_w, h);
    p->sticky = lm_rect_make(p->clock.x - 8 - PANEL_STICKY_W, y, PANEL_STICKY_W, h);         /* (the Run dialog is Super+R and the launcher's Run button) */
    p->layout = lm_rect_make(p->sticky.x - 4 - PANEL_LAYOUT_W, y, PANEL_LAYOUT_W, h);
    p->osk = lm_rect_make(p->layout.x - 4 - PANEL_OSK_W, y, PANEL_OSK_W, h);
    p->hover_osk = wm->hover_osk;
    p->osk_open = wm->osk_open;
    p->quick = lm_rect_make(p->osk.x - 4 - PANEL_QUICK_W, y, PANEL_QUICK_W, h);
    p->hover_quick = wm->hover_quick;
    {
        struct lm_window *qw = quick_window((struct lm_wm *)wm);
        p->quick_open = qw && win_visible(wm, qw);
    }
    uint32_t locks = wm->srv->input ? lm_input_locks(wm->srv->input) : 0;
    p->caps_text[0] = 0;
    if ((locks & LM_MOD_CAPS) && (locks & LM_MOD_NUM))
        snprintf(p->caps_text, sizeof(p->caps_text), "CAPS+N");
    else if (locks & LM_MOD_CAPS)
        snprintf(p->caps_text, sizeof(p->caps_text), "CAPS");
    else if (locks & LM_MOD_NUM)
        snprintf(p->caps_text, sizeof(p->caps_text), "NUM");
    int caps_w = !p->caps_text[0] ? 0 : (locks & LM_MOD_NUM) && (locks & LM_MOD_CAPS) ? PANEL_CAPS_W + 24 : PANEL_CAPS_W;
    p->caps = lm_rect_make(p->quick.x - (caps_w ? 4 + caps_w : 0), y, caps_w, h);
    int tx = x + 12;
    int title_end = p->caps.x;
    if (wm->srv->tray) {                                            /* programs' icons: the first one created is the rightmost */
        int right = (caps_w ? p->caps.x : p->quick.x) - 4, n = lm_tray_count(wm->srv->tray);
        for (int i = 0; i < n && p->ntray < LM_PANEL_MAX_TRAY; i++) {
            struct lm_tray_view v;
            if (!lm_tray_get(wm->srv->tray, i, &v))
                break;
            int iw = v.icon ? v.icon->width : 0, tw = v.text[0] ? lm_font_text_width(v.text) : 0;
            int bw = 12 + iw + (iw && tw ? 4 : 0) + tw;
            if (bw < h)
                bw = h;
            if (right - bw < tx + 120)                              /* the taskbar keeps some room */
                break;
            int k = p->ntray++;
            p->tray[k].rect = lm_rect_make(right - bw, y, bw, h);
            p->tray[k].id = v.id;
            p->tray[k].icon = v.icon;
            p->tray[k].text = v.text;
            p->tray[k].hover = wm->hover_tray == v.id;
            right -= bw + 4;
            title_end = p->tray[k].rect.x - 4;
        }
    }
    p->title_area = lm_rect_make(tx, y, title_end - 12 - tx, h);
    p->hover_layout = wm->hover_layout;
    p->hover_sticky = wm->hover_sticky;
    snprintf(p->layout_text, sizeof(p->layout_text), "us");
    snprintf(p->sticky_text, sizeof(p->sticky_text), "Sticky");
    if (wm->srv->input) {
        const char *id = lm_input_layout_id(wm->srv->input, lm_input_layout(wm->srv->input));
        for (size_t i = 0; i < sizeof(p->layout_text) - 1 && id[i]; i++)
            p->layout_text[i] = (char)(id[i] >= 'a' && id[i] <= 'z' ? id[i] - 32 : id[i]);
        p->layout_text[strnlen(id, sizeof(p->layout_text) - 1)] = 0;
        p->sticky_on = lm_input_sticky(wm->srv->input);
        uint32_t lat = lm_input_latched(wm->srv->input), lck = lm_input_sticky_locked(wm->srv->input);
        static const struct { uint32_t bit; const char *lo, *up; } names[] = {
            { LM_MOD_SHIFT, "Sh", "SH" }, { LM_MOD_CTRL, "Ct", "CT" }, { LM_MOD_ALT, "Al", "AL" },
            { LM_MOD_ALTGR, "Gr", "GR" }, { LM_MOD_SUPER, "Su", "SU" },
        };
        char txt[24] = "";
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            if (!((lat | lck) & names[i].bit))
                continue;
            if (txt[0])
                strncat(txt, "+", sizeof(txt) - strlen(txt) - 1);
            strncat(txt, (lck & names[i].bit) ? names[i].up : names[i].lo, sizeof(txt) - strlen(txt) - 1);
        }
        if (p->sticky_on && txt[0])
            snprintf(p->sticky_text, sizeof(p->sticky_text), "%s", txt);
    }
    p->current_workspace = wm->workspace;
    struct lm_window *w;
    lm_list_for_each(w, &wm->windows, link)
        if (w->mapped)
            p->workspace_occupied |= 1u << w->workspace;
    p->hover_workspace = wm->hover_panel_ws;
    p->hover_launcher = wm->hover_launcher;
    p->about_open = wm->about_open;
    p->hover_power = wm->hover_power;
    p->power_open = wm->power_open;
    p->active_title = wm->focused ? wm->focused->title : "";

    /* taskbar: windows of this workspace in creation (id) order, so raising does not reshuffle it */
    struct lm_window *tw[LM_PANEL_MAX_TASKS];
    int n = 0;
    lm_list_for_each(w, &wm->windows, link) {
        if (!w->mapped || w->workspace != wm->workspace || n >= LM_PANEL_MAX_TASKS || win_is_aux(w))
            continue;
        int i = n++;
        for (; i > 0 && WIN(tw[i - 1])->id > WIN(w)->id; i--)
            tw[i] = tw[i - 1];
        tw[i] = w;
    }
    p->ntasks = n;
    if (n > 0) {
        int bw = (p->title_area.w - (n - 1) * PANEL_TASK_GAP) / n;
        bw = LM_MIN(bw, PANEL_TASK_MAX_W);
        if (bw <= 0)
            p->ntasks = 0;
        for (int i = 0; i < p->ntasks; i++) {
            p->task[i].rect = lm_rect_make(p->title_area.x + i * (bw + PANEL_TASK_GAP), y, bw, h);
            p->task[i].title = tw[i]->title;
            p->task[i].active = wm->focused == tw[i];
            p->task[i].minimized = tw[i]->minimized;
            p->task[i].hover = wm->hover_task == tw[i];
            p->task_win[i] = tw[i];
        }
    }
    snprintf(p->clock_text, sizeof(p->clock_text), "%s", wm->clock_text);
}

bool fullscreen_covers(const struct lm_wm *wm, const struct lm_output *o)
{
    struct lm_window *w;
    lm_list_for_each(w, &wm->windows, link) {
        if (!win_visible(wm, w) || !(w->states & LM_WINDOW_FULLSCREEN))
            continue;
        struct lm_rect r = lm_rect_make(w->x, w->y, w->width, w->height);
        struct lm_rect orect = lm_output_rect(o);
        if (lm_rect_overlaps(&r, &orect))
            return true;
    }
    return false;
}

/* ---- damage ---------------------------------------------------------------------------- */

void damage_rect(struct lm_wm *wm, const struct lm_rect *r)
{
    if (wm->srv->render && !lm_rect_is_empty(r))
        lm_render_damage(wm->srv->render, r);
}

void damage_frame(struct lm_wm *wm, const struct lm_window *w)
{
    struct lm_rect r = frame_outer(w);
    damage_rect(wm, &r);
}

void damage_all(struct lm_wm *wm)
{
    if (wm->srv->render)
        lm_render_damage_all(wm->srv->render);
}


/* ---- surface output membership --------------------------------------------------------- */

void update_outputs(struct lm_wm *wm, struct lm_window *w)
{
    struct lm_output *outs[16];
    size_t n = 0;
    if (win_visible(wm, w) && wm->srv->backend) {
        struct lm_rect fr = frame_outer(w);
        struct lm_output *o;
        lm_list_for_each(o, &wm->srv->backend->outputs, link) {
            struct lm_rect orect = lm_output_rect(o);
            if (n < LM_ARRAY_LEN(outs) && lm_rect_overlaps(&fr, &orect))
                outs[n++] = o;
        }
    }
    lm_surface_set_outputs(w->surface, outs, n);
}

/* ---- usable area ------------------------------------------------------------------------- */


void lm_wm_usable_area(struct lm_wm *wm, int32_t gx, int32_t gy, struct lm_rect *out)
{
    struct lm_output *o = lm_output_nearest(wm->srv, gx, gy);
    if (!o) {
        *out = lm_rect_make(0, 0, 1280, 720);
        return;
    }
    struct lm_rect r = lm_output_rect(o);
    r.y += LM_PANEL_HEIGHT;
    r.h -= LM_PANEL_HEIGHT;
    struct lm_osk_geo kb;
    if (wm->srv->config.osk_avoid && wm->osk_open && o == lm_output_primary(wm->srv) && osk_geo(wm, &kb)) {
        /* the on-screen keyboard takes the strip it sits in: from the bottom when it is in the lower half, from the top otherwise
         * (not when what is left would be too small to work in) */
        if (kb.card.y + kb.card.h / 2 >= o->y + o->height / 2) {
            int h = kb.card.y - r.y;
            if (h >= 160)
                r.h = h;
        } else {
            int y = kb.card.y + kb.card.h, h = r.y + r.h - y;
            if (h >= 160 && y > r.y) {
                r.y = y;
                r.h = h;
            }
        }
    }
    *out = r;
}

/* ---- configure ---------------------------------------------------------------------------- */

void send_configure(struct lm_wm *wm, struct lm_window *w, int32_t cw, int32_t ch)
{
    uint32_t st = w->states & (LM_WINDOW_MAXIMIZED | LM_WINDOW_FULLSCREEN | LM_WINDOW_RESIZING);
    if (wm->focused == w)
        st |= LM_WINDOW_ACTIVATED;
    w->states = st;
    if (w->ops && w->ops->configure)
        w->ops->configure(w, cw, ch, st);
}

void clamp_size(const struct lm_window *w, int32_t *cw, int32_t *ch)
{
    int32_t minw = LM_MAX(w->min_w, LM_MIN_WINDOW_WIDTH), minh = LM_MAX(w->min_h, LM_MIN_WINDOW_HEIGHT);
    if (win_is_gadget(w)) {                                     /* a card has its own limits */
        minw = LM_MAX(w->min_w, 1);
        minh = LM_MAX(w->min_h, 1);
    }
    if (*cw < minw) *cw = minw;
    if (*ch < minh) *ch = minh;
    if (w->max_w > 0 && *cw > w->max_w) *cw = w->max_w;
    if (w->max_h > 0 && *ch > w->max_h) *ch = w->max_h;
}

struct lm_output *window_output(struct lm_wm *wm, const struct lm_window *w)
{
    return lm_output_nearest(wm->srv, w->x + w->width / 2.0, w->y + w->height / 2.0);
}

/* Compute x/y/size for the current maximized/fullscreen/normal state and configure. */
void apply_state(struct lm_wm *wm, struct lm_window *w)
{
    struct wm_win *ww = WIN(w);
    struct lm_rect before = frame_outer(w);
    int32_t tw = 0, th = 0;

    ww->tiled = 0;
    if ((w->states & (LM_WINDOW_FULLSCREEN | LM_WINDOW_MAXIMIZED)) && !lm_output_primary(wm->srv)) {
        /* no output at all (a KVM switch looked at another computer, a cable was pulled): keep the size, it is fitted when one comes back */
        send_configure(wm, w, w->width, w->height);
        return;
    }
    if (w->states & LM_WINDOW_FULLSCREEN) {
        struct lm_output *o = window_output(wm, w);
        if (o) {
            w->x = o->x;
            w->y = o->y;
            tw = o->width;
            th = o->height;
        }
    } else if (w->states & LM_WINDOW_MAXIMIZED) {
        struct lm_rect ua;
        lm_wm_usable_area(wm, w->x + w->width / 2, w->y + w->height / 2, &ua);
        w->x = ua.x + LM_BORDER_WIDTH;
        w->y = ua.y + LM_TITLEBAR_HEIGHT;
        tw = ua.w - 2 * LM_BORDER_WIDTH;
        th = ua.h - LM_TITLEBAR_HEIGHT - LM_BORDER_WIDTH;
    } else if (ww->have_restore) {
        w->x = ww->rx;
        w->y = ww->ry;
        tw = ww->rw;
        th = ww->rh;
        ww->have_restore = false;
    }
    /* Keep the old extents until the client commits the new size. */
    send_configure(wm, w, tw, th);
    damage_rect(wm, &before);
    damage_frame(wm, w);
    update_outputs(wm, w);
}

static void save_restore(struct wm_win *ww)
{
    struct lm_window *w = &ww->pub;
    if (ww->have_restore || (w->states & (LM_WINDOW_MAXIMIZED | LM_WINDOW_FULLSCREEN)))
        return;
    ww->rx = w->x;
    ww->ry = w->y;
    ww->rw = w->width;
    ww->rh = w->height;
    ww->have_restore = true;
}

/* ---- focus -------------------------------------------------------------------------------- */

void refresh_pointer(struct lm_wm *wm)
{
    if (!wm->srv->seat || lm_seat_pointer_has_grab(wm->srv->seat))
        return;
    double x, y;
    lm_seat_pointer_position(wm->srv->seat, &x, &y);
    lm_seat_pointer_warp(wm->srv->seat, x, y);
}

void set_focus(struct lm_wm *wm, struct lm_window *w)
{
    struct lm_window *old = wm->focused;
    if (old == w)
        return;
    wm->focused = w;
    if (w)
        WIN(w)->focus_seq = ++wm->focus_counter;
    if (old) {
        lm_xdg_dismiss_popups(old->surface);
        send_configure(wm, old, old->width, old->height);
        damage_frame(wm, old);
    }
    if (w) {
        send_configure(wm, w, w->width, w->height);
        damage_frame(wm, w);
    }
    if (wm->srv->seat)
        lm_seat_set_keyboard_focus(wm->srv->seat, w ? w->surface : NULL);
    damage_panels(wm);
    if (old && win_is_calendar(old) && old->mapped && !old->minimized)
        lm_wm_window_request_minimize(old);                 /* the calendar goes away when something else is clicked */
}

/* Topmost visible window on the current workspace (excluding `skip`). */
static struct lm_window *top_visible(struct lm_wm *wm, const struct lm_window *skip)
{
    struct lm_window *w;
    lm_list_for_each_reverse(w, &wm->windows, link)
        if (w != skip && win_visible(wm, w) && !win_is_gadget(w))
            return w;
    return NULL;
}

void raise_window(struct lm_wm *wm, struct lm_window *w)
{
    if (win_is_gadget(w) || wm->windows.prev == &w->link)             /* (a gadget stays at the bottom) */
        return;
    lm_list_remove(&w->link);
    lm_list_append(&wm->windows, &w->link);
    damage_frame(wm, w);
}

void lm_wm_switch_workspace(struct lm_wm *wm, int ws)
{
    if (ws < 0 || ws >= LM_NUM_WORKSPACES || ws == wm->workspace)
        return;
    if (wm->focused)
        lm_xdg_dismiss_popups(wm->focused->surface);
    anim_finish_all(wm);
    int dir = ws > wm->workspace ? 1 : -1;
    anim_workspace(wm, dir, true);                              /* the windows that leave slide out ... */
    wm->workspace = ws;
    struct lm_window *w;
    lm_list_for_each(w, &wm->windows, link) {
        if (win_is_gadget(w))
            w->workspace = ws;                                  /* the gadgets are on every workspace */
        update_outputs(wm, w);
    }
    anim_workspace(wm, dir, false);                             /* ... and the ones that arrive slide in */
    set_focus(wm, NULL);
    set_focus(wm, top_visible(wm, NULL));
    overview_changed(wm);
    damage_all(wm);
    refresh_pointer(wm);
}

void lm_wm_focus_back(struct lm_wm *wm)
{
    set_focus(wm, top_visible(wm, NULL));
}

void lm_wm_focus_window(struct lm_wm *wm, struct lm_window *w)
{
    if (!w) {
        set_focus(wm, NULL);
        return;
    }
    if (w->workspace != wm->workspace)
        lm_wm_switch_workspace(wm, w->workspace);
    if (w->minimized) {
        anim_fly(wm, ANIM_RESTORE, w);                           /* (a picture flies from the button; the window itself is hidden meanwhile) */
        w->minimized = false;
        update_outputs(wm, w);
        damage_frame(wm, w);
    }
    raise_window(wm, w);
    set_focus(wm, w);
}

struct lm_window *lm_wm_focused_window(const struct lm_wm *wm) { return wm->focused; }

/* ---- lifecycle ------------------------------------------------------------------------------ */

static void update_clock(struct lm_wm *wm, bool force)
{
    char buf[40];
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    lm_clock_bar_text(&wm->srv->config.clock, &tm, buf, sizeof(buf));
    if (!force && !strcmp(buf, wm->clock_text))
        return;
    snprintf(wm->clock_text, sizeof(wm->clock_text), "%s", buf);
    if (!force)
        damage_panels(wm);
}

static void clock_tick(void *data)
{
    update_clock(data, false);
}

void lm_wm_reset_cascade(struct lm_wm *wm)
{
    if (wm)
        wm->cascade = 0;
}

void lm_wm_clock_changed(struct lm_wm *wm)
{
    if (!wm)
        return;
    update_clock(wm, false);
    damage_panels(wm);
}

static void refit_idle(void *data)
{
    lm_wm_output_changed(data, NULL);
}

struct lm_wm *lm_wm_create(struct lm_server *srv)
{
    struct lm_wm *wm = calloc(1, sizeof(*wm));
    if (!wm)
        return NULL;
    wm->srv = srv;
    lm_list_init(&wm->windows);
    lm_list_init(&wm->popups);
    wm->hover_panel_ws = -1;
    wm->osk_hover = wm->osk_down = -1;
    wm->hover_dock = -1;
    wm->gad[LM_GADGET_CLIPBOARD].visible = srv->config.gadget;
    wm->gad[LM_GADGET_MIXER].visible = srv->config.mixer;
    wm->gad[LM_GADGET_SYSTEM].visible = srv->config.sysinfo;
    wm->gad[LM_GADGET_SMART].visible = srv->config.smart;
    for (int i = 0; i < LM_GADGET_COUNT; i++) {
            }
    gadgets_load(wm);
    places_load(wm);
    update_clock(wm, true);
    wm->note_timer = lm_loop_add_timer(srv->loop, note_tick, wm);
    wm->dock_timer = lm_loop_add_timer(srv->loop, dock_hide_tick, wm);
    wm->dock_anim = lm_loop_add_timer(srv->loop, dock_anim_tick, wm);
    wm->clock_timer = lm_loop_add_timer(srv->loop, clock_tick, wm);
    wm->anim_timer = lm_loop_add_timer(srv->loop, anim_tick, wm);
    wm->refit = lm_loop_add_idle(srv->loop, refit_idle, wm);
    if (wm->clock_timer)
        lm_timer_arm_ms(wm->clock_timer, 1000, 1000);
    return wm;
}

void lm_wm_destroy(struct lm_wm *wm)
{
    if (!wm)
        return;
    if (wm->grab_kind != GRAB_NONE && wm->srv->seat)
        lm_seat_pointer_ungrab(wm->srv->seat, &wm->grab);
    gadget_procs_stop(wm);                                      /* the gadget programs end with the compositor */
    if (wm->note_timer)
        lm_timer_destroy(wm->note_timer);
    if (wm->dock_timer)
        lm_timer_destroy(wm->dock_timer);
    if (wm->dock_anim)
        lm_timer_destroy(wm->dock_anim);
    if (wm->clock_timer)
        lm_timer_destroy(wm->clock_timer);
    for (int i = 0; i < ANIM_MAX; i++)
        if (wm->anim[i].used)
            free(wm->anim[i].ghost.px);
    free(wm->wb.ghost.px);
    for (int i = 0; i < wm->ov.n; i++)
        free(wm->ov.item[i].ghost.px);
    if (wm->anim_timer)
        lm_timer_destroy(wm->anim_timer);
    if (wm->refit)
        lm_idle_destroy(wm->refit);
    free(wm);
}

void lm_wm_output_added(struct lm_wm *wm, struct lm_output *out)
{
    lm_wm_output_changed(wm, out);                      /* a monitor that comes back (KVM, cable) gets its fullscreen / maximized windows at its size */
}

void lm_wm_relayout_outputs(struct lm_wm *wm)
{
    struct { struct lm_window *w; struct lm_output *o; int32_t dx, dy; } rec[64];
    int n = 0;
    struct lm_window *w;
    lm_list_for_each(w, &wm->windows, link) {
        struct lm_output *o = n < 64 ? window_output(wm, w) : NULL;
        if (o)
            rec[n++] = (typeof(rec[0])){ w, o, w->x - o->x, w->y - o->y };
    }
    if (wm->srv->outputs)
        lm_output_mgr_relayout(wm->srv->outputs);
    for (int i = 0; i < n; i++) {
        int32_t nx = rec[i].o->x + rec[i].dx, ny = rec[i].o->y + rec[i].dy;
        WIN(rec[i].w)->rx += nx - rec[i].w->x;                  /* the position to restore to moves along, too */
        WIN(rec[i].w)->ry += ny - rec[i].w->y;
        rec[i].w->x = nx;
        rec[i].w->y = ny;
    }
    lm_list_for_each(w, &wm->windows, link)
        if (w->states & (LM_WINDOW_MAXIMIZED | LM_WINDOW_FULLSCREEN))
            apply_state(wm, w);
    gadgets_redock(wm);
    damage_all(wm);
    refresh_pointer(wm);
}

void lm_wm_config_changed(struct lm_wm *wm)
{
    wm->dock_active = wm->dock_shown = false;
    wm->dock_vis = lm_dock.autohide ? 0.0f : 1.0f;
    wm->dock_drag = false;
    wm->hover_dock = -1;
    memset(wm->dock_launch_ms, 0, sizeof(wm->dock_launch_ms));         /* (the buttons may be others now) */
    menu_set(wm, false);
    lm_wm_output_changed(wm, NULL);
}

void lm_wm_output_changed(struct lm_wm *wm, struct lm_output *out)
{
    (void)out;
    struct lm_window *w;
    lm_list_for_each(w, &wm->windows, link)
        if (w->states & (LM_WINDOW_MAXIMIZED | LM_WINDOW_FULLSCREEN))
            apply_state(wm, w);
    if (wm->ov.open && !overview_output(wm))
        overview_finish(wm);                                    /* its output went away */
    else
        overview_changed(wm);
    gadgets_redock(wm);                                         /* the cards that nobody moved follow the output's edge */
    damage_all(wm);
}

void lm_wm_output_removed(struct lm_wm *wm, struct lm_output *out)
{
    (void)out;
    /* The output is still in the list while we are called: re-clamp after it is gone. */
    struct lm_window *w;
    lm_list_for_each(w, &wm->windows, link)
        update_outputs(wm, w);
    damage_all(wm);
    if (wm->refit)
        lm_idle_schedule(wm->refit);                    /* (the output is unlinked after this call: refit then) */
}

/* ---- windows --------------------------------------------------------------------------------- */

struct lm_window *lm_wm_window_create(struct lm_wm *wm, struct lm_surface *surface,
                                      const struct lm_window_ops *ops, void *shell_data)
{
    struct wm_win *ww = calloc(1, sizeof(*ww));
    if (!ww)
        return NULL;
    struct lm_window *w = &ww->pub;
    w->wm = wm;
    w->surface = surface;
    w->ops = ops;
    w->shell_data = shell_data;
    w->title = strdup("");
    w->app_id = strdup("");
    if (!w->title || !w->app_id) {
        free(w->title);
        free(w->app_id);
        free(ww);
        return NULL;
    }
    w->workspace = wm->workspace;
    ww->id = ++wm->next_id;
    lm_list_append(&wm->windows, &w->link);
    return w;
}

void lm_wm_window_destroy(struct lm_window *w)
{
    struct lm_wm *wm = w->wm;
    for (int i = 0; i < wm->zoomglue.n; i++)
        if (wm->zoomglue.win[i] == w)
            wm->zoomglue.n = 0;                                 /* (a card of the zoomed stack went away: forget the stack) */
    if (wm->grab_win == w && wm->grab_kind != GRAB_NONE) {
        wm->grab_kind = GRAB_NONE;
        wm->grab_win = NULL;
        if (wm->srv->seat)
            lm_seat_pointer_ungrab(wm->srv->seat, &wm->grab);
    }
    if (wm->hover_win == w) { wm->hover_win = NULL; wm->hover_bits = 0; }
    if (wm->hover_task == w) wm->hover_task = NULL;
    if (wm->press_win == w) { wm->press_win = NULL; wm->press_bits = 0; }
    if (wm->last_click_win == w) wm->last_click_win = NULL;
    if (wm->menu_open && wm->menu_owner == 3 && wm->menu_window == w)
        menu_close(wm, true);                                  /* the menu is of this window */
    sw_close(wm);                                               /* the list holds window pointers */
    struct lm_window *c;
    lm_list_for_each(c, &wm->windows, link)
        if (c->parent == w)
            c->parent = NULL;
    if (w->mapped)
        lm_wm_window_unmap(w);
    anim_forget(wm, w);
    if (wm->focused == w)
        set_focus(wm, top_visible(wm, w));
    lm_list_remove(&w->link);
    free(w->title);
    free(w->app_id);
    free(w);
    refresh_pointer(wm);
}

void lm_wm_window_initial_configure(struct lm_window *w)
{
    struct lm_wm *wm = w->wm;
    int32_t cw = 0, ch = 0;
    if (w->states & LM_WINDOW_FULLSCREEN) {
        struct lm_output *o = window_output(wm, w);
        if (o) { cw = o->width; ch = o->height; }
    } else if (w->states & LM_WINDOW_MAXIMIZED) {
        struct lm_rect ua;
        lm_wm_usable_area(wm, 0, 0, &ua);
        cw = ua.w - 2 * LM_BORDER_WIDTH;
        ch = ua.h - LM_TITLEBAR_HEIGHT - LM_BORDER_WIDTH;
    }
    send_configure(wm, w, cw, ch);
}

static void replace_str(char **dst, const char *src)
{
    char *n = strdup(src ? src : "");
    if (n) {
        free(*dst);
        *dst = n;
    }
}

void lm_wm_window_set_title(struct lm_window *w, const char *title)
{
    replace_str(&w->title, title);
    if (w->mapped) {
        damage_frame(w->wm, w);
        if (w->wm->focused == w)
            damage_panels(w->wm);
    }
}

void lm_wm_window_set_app_id(struct lm_window *w, const char *app_id)
{
    replace_str(&w->app_id, app_id);
}

void lm_wm_window_set_parent(struct lm_window *w, struct lm_window *parent)
{
    for (struct lm_window *p = parent; p; p = p->parent)
        if (p == w)
            return; /* cycle */
    w->parent = parent;
}

void lm_wm_window_set_min_size(struct lm_window *w, int32_t width, int32_t height)
{
    w->min_w = LM_MAX(width, 0);
    w->min_h = LM_MAX(height, 0);
}

void lm_wm_window_set_max_size(struct lm_window *w, int32_t width, int32_t height)
{
    w->max_w = LM_MAX(width, 0);
    w->max_h = LM_MAX(height, 0);
}

void lm_wm_window_set_geometry(struct lm_window *w, const struct lm_rect *g)
{
    struct lm_wm *wm = w->wm;
    struct lm_rect before = frame_outer(w);
    struct lm_rect ng = *g;
    if (ng.w < 1 || ng.h < 1)
        return;
    bool resized = w->mapped && (w->width != ng.w || w->height != ng.h);
    w->geometry = ng;
    w->width = ng.w;
    w->height = ng.h;
    if (wm->grab_kind == GRAB_RESIZE && wm->grab_win == w) {
        if (wm->grab_edges & LM_EDGE_LEFT)
            w->x = wm->grab_x + wm->grab_w - w->width;
        if (wm->grab_edges & LM_EDGE_TOP)
            w->y = wm->grab_y + wm->grab_h - w->height;
    } else if (resized && win_is_gadget(w)) {
        if (!WIN(w)->gad_placed)
            gadgets_redock(wm);                                 /* the cards after it move to make room */
        gadget_zoomglue_fit(wm, w);                             /* a glued stack that is being zoomed keeps together */
    } else if (resized && !(w->states & (LM_WINDOW_MAXIMIZED | LM_WINDOW_FULLSCREEN)) && !w->parent) {
        /* A floating window that made itself bigger (a terminal zooming its font) must not stick out of the screen:
         * it moves back inside, and when it is bigger than the usable area it is asked to shrink to fit. */
        struct lm_rect ua;
        lm_wm_usable_area(wm, w->x + w->width / 2, w->y + w->height / 2, &ua);
        int32_t fw = LM_MIN(w->width, ua.w - 2 * LM_BORDER_WIDTH);
        int32_t fh = LM_MIN(w->height, ua.h - LM_TITLEBAR_HEIGHT - LM_BORDER_WIDTH);
        if (w->x + w->width + LM_BORDER_WIDTH > ua.x + ua.w)
            w->x = ua.x + ua.w - LM_MIN(w->width, fw) - LM_BORDER_WIDTH;
        if (w->y + w->height + LM_BORDER_WIDTH > ua.y + ua.h)
            w->y = ua.y + ua.h - LM_MIN(w->height, fh) - LM_BORDER_WIDTH;
        if (w->x < ua.x + LM_BORDER_WIDTH)
            w->x = ua.x + LM_BORDER_WIDTH;
        if (w->y < ua.y + LM_TITLEBAR_HEIGHT)
            w->y = ua.y + LM_TITLEBAR_HEIGHT;
        if (fw != w->width || fh != w->height) {
            clamp_size(w, &fw, &fh);
            send_configure(wm, w, fw, fh);
        }
    }
    if (w->mapped) {
        damage_rect(wm, &before);
        damage_frame(wm, w);
        update_outputs(wm, w);
    }
}

void lm_wm_window_map(struct lm_window *w)
{
    struct lm_wm *wm = w->wm;
    struct wm_win *ww = WIN(w);
    int32_t fit_w = 0, fit_h = 0;
    struct lm_rect was = { 0, 0, 0, 0 };                        /* where a window of this program was the last time (wm_places.c) */
    bool was_known = false, was_max = false;
    if (w->mapped)
        return;
    if (win_is_gadget(w)) {
        gadget_map(wm, w);
        return;
    }
    if (!ww->placed) {
        ww->placed = true;
        struct lm_rect ua;
        double px = 0, py = 0;
        if (wm->srv->seat)
            lm_seat_pointer_position(wm->srv->seat, &px, &py);
        lm_wm_usable_area(wm, (int32_t)px, (int32_t)py, &ua);
        if (w->states & LM_WINDOW_FULLSCREEN) {
            struct lm_output *o = lm_output_nearest(wm->srv, px, py);
            if (o) { w->x = o->x; w->y = o->y; }
        } else if (w->states & LM_WINDOW_MAXIMIZED) {
            w->x = ua.x + LM_BORDER_WIDTH;
            w->y = ua.y + LM_TITLEBAR_HEIGHT;
        } else if (w->parent && w->parent->mapped) {
            w->x = w->parent->x + (w->parent->width - w->width) / 2;
            w->y = w->parent->y + (w->parent->height - w->height) / 2;
        } else if (places_find(wm, w, &was, &was_max)) {
            was_known = true;
            w->x = was.x;
            w->y = was.y;
            lm_wm_usable_area(wm, was.x + was.w / 2, was.y + was.h / 2, &ua);    /* (kept on the output it was on) */
        } else {
            int step = (wm->cascade++ % 10) * 28;
            w->x = ua.x + LM_BORDER_WIDTH + 40 + step;
            w->y = ua.y + LM_TITLEBAR_HEIGHT + 24 + step;
        }
        /* keep the whole frame on the output where possible */
        if (!(w->states & (LM_WINDOW_FULLSCREEN | LM_WINDOW_MAXIMIZED))) {
            /* a client that opened bigger than the usable area is asked to shrink to fit */
            int32_t fw = LM_MIN(was_known ? was.w : w->width, ua.w - 2 * LM_BORDER_WIDTH);
            int32_t fh = LM_MIN(was_known ? was.h : w->height, ua.h - LM_TITLEBAR_HEIGHT - LM_BORDER_WIDTH);
            if (fw != w->width || fh != w->height) {
                clamp_size(w, &fw, &fh);
                fit_w = fw; /* sent after set_focus(), whose configure would otherwise override it */
                fit_h = fh;
            }
            if (w->x + fw + LM_BORDER_WIDTH > ua.x + ua.w)
                w->x = ua.x + ua.w - fw - LM_BORDER_WIDTH;
            if (w->y + fh + LM_BORDER_WIDTH > ua.y + ua.h)
                w->y = ua.y + ua.h - fh - LM_BORDER_WIDTH;
            if (w->x < ua.x + LM_BORDER_WIDTH) w->x = ua.x + LM_BORDER_WIDTH;
            if (w->y < ua.y + LM_TITLEBAR_HEIGHT) w->y = ua.y + LM_TITLEBAR_HEIGHT;
        }
    }
    if (win_is_calendar(w)) {                                /* it starts hidden; a click on the clock (maybe the one that started it) shows it */
        w->mapped = true;
        w->workspace = wm->workspace;
        w->minimized = true;
        damage_panels(wm);
        if (win_is_quick(w)) {
            if (wm->quick_pending && lm_now_ms() - wm->quick_pending_ms < 5000)
                lm_wm_quick_toggle(wm, &wm->quick_anchor);
            wm->quick_pending = false;
            return;
        }
        if (wm->cal_pending && lm_now_ms() - wm->cal_pending_ms < 5000)
            lm_wm_calendar_toggle(wm, &wm->cal_clock);
        wm->cal_pending = false;
        return;
    }
    w->mapped = true;
    w->workspace = wm->workspace;
    w->minimized = false;
    raise_window(wm, w);
    update_outputs(wm, w);
    damage_frame(wm, w);
    set_focus(wm, w);
    if (fit_w > 0)
        send_configure(wm, w, fit_w, fit_h);
    if (was_known && was_max)
        lm_wm_window_request_maximize(w, true);
    damage_panels(wm);
    if (wm->ov.open)
        overview_changed(wm);                                   /* (its picture joins the overview) */
    else
        anim_open(wm, w);
    refresh_pointer(wm);
}

void lm_wm_window_unmap(struct lm_window *w)
{
    struct lm_wm *wm = w->wm;
    if (!w->mapped)
        return;
    places_remember(wm, w);
    sw_close(wm);
    anim_play_close(wm, w);
    struct lm_rect r = frame_outer(w);
    lm_xdg_dismiss_popups(w->surface);
    w->mapped = false;
    lm_surface_set_outputs(w->surface, NULL, 0);
    if (wm->focused == w)
        set_focus(wm, top_visible(wm, w));
    damage_rect(wm, &r);
    damage_panels(wm);
    overview_changed(wm);
    if (win_is_gadget(w))
        gadgets_redock(wm);                                     /* the cards below it move up */
    refresh_pointer(wm);
}

void lm_wm_window_request_maximize(struct lm_window *w, bool maximize)
{
    if (win_is_gadget(w))
        return;
    struct lm_wm *wm = w->wm;
    struct wm_win *ww = WIN(w);
    bool is = (w->states & LM_WINDOW_MAXIMIZED) != 0;
    if (is == maximize)
        return;
    if (maximize) {
        save_restore(ww);
        w->states |= LM_WINDOW_MAXIMIZED;
    } else {
        w->states &= ~LM_WINDOW_MAXIMIZED;
    }
    if (!w->mapped) {
        return; /* applied on map / initial configure */
    }
    if (w->states & LM_WINDOW_FULLSCREEN)
        return;
    apply_state(wm, w);
}

void lm_wm_window_request_tile(struct lm_window *w, uint32_t edge)
{
    struct lm_wm *wm = w->wm;
    struct wm_win *ww = WIN(w);
    if (edge != LM_EDGE_LEFT && edge != LM_EDGE_RIGHT && edge != LM_EDGE_NONE)
        return;
    if (!w->mapped || (w->states & LM_WINDOW_FULLSCREEN) || ww->tiled == edge)
        return;
    if (edge == LM_EDGE_NONE) {
        if (!ww->tiled)
            return;
        apply_state(wm, w); /* clears ww->tiled and restores the saved geometry */
        return;
    }

    struct lm_rect before = frame_outer(w);
    save_restore(ww);
    w->states &= ~LM_WINDOW_MAXIMIZED;

    struct lm_rect ua;
    lm_wm_usable_area(wm, w->x + w->width / 2, w->y + w->height / 2, &ua);
    int32_t half = ua.w / 2;
    struct lm_rect outer = edge == LM_EDGE_LEFT ? lm_rect_make(ua.x, ua.y, half, ua.h)
                                                : lm_rect_make(ua.x + half, ua.y, ua.w - half, ua.h);
    int32_t tw = outer.w - 2 * LM_BORDER_WIDTH;
    int32_t th = outer.h - LM_TITLEBAR_HEIGHT - LM_BORDER_WIDTH;
    clamp_size(w, &tw, &th);
    w->x = outer.x + LM_BORDER_WIDTH;
    w->y = outer.y + LM_TITLEBAR_HEIGHT;
    ww->tiled = edge;
    send_configure(wm, w, tw, th);
    damage_rect(wm, &before);
    damage_frame(wm, w);
    update_outputs(wm, w);
}

void lm_wm_window_request_fullscreen(struct lm_window *w, bool fullscreen)
{
    if (win_is_gadget(w))
        return;
    struct lm_wm *wm = w->wm;
    struct wm_win *ww = WIN(w);
    bool is = (w->states & LM_WINDOW_FULLSCREEN) != 0;
    if (is == fullscreen)
        return;
    if (fullscreen) {
        save_restore(ww);
        w->states |= LM_WINDOW_FULLSCREEN;
    } else {
        w->states &= ~LM_WINDOW_FULLSCREEN;
    }
    if (!w->mapped)
        return;
    apply_state(wm, w);
    damage_panels(wm);
}

void lm_wm_window_request_minimize(struct lm_window *w)
{
    if (win_is_gadget(w))
        return;
    struct lm_wm *wm = w->wm;
    if (!w->mapped || w->minimized)
        return;
    struct lm_rect r = frame_outer(w);
    lm_xdg_dismiss_popups(w->surface);
    anim_fly(wm, ANIM_MINIMIZE, w);
    w->minimized = true;
    lm_surface_set_outputs(w->surface, NULL, 0);
    if (wm->focused == w)
        set_focus(wm, top_visible(wm, w));
    damage_rect(wm, &r);
    damage_panels(wm);
    refresh_pointer(wm);
}

void lm_wm_window_close(struct lm_window *w)
{
    anim_remember_close(w->wm, w);
    if (w->ops && w->ops->close)
        w->ops->close(w);
}

/* ---- popups ------------------------------------------------------------------------------------ */

struct lm_popup *lm_wm_popup_create(struct lm_wm *wm, struct lm_surface *surface, struct lm_surface *parent)
{
    struct wm_popup *wp = calloc(1, sizeof(*wp));
    if (!wp)
        return NULL;
    wp->pub.wm = wm;
    wp->pub.surface = surface;
    wp->pub.parent = parent;
    lm_list_append(&wm->popups, &wp->pub.link);
    return &wp->pub;
}

struct lm_rect popup_bounds(const struct lm_popup *p)
{
    int32_t ox, oy;
    struct lm_rect ext;
    popup_origin(p, &ox, &oy);
    if (lm_surface_tree_extents(p->surface, &ext))
        return lm_rect_make(ox + ext.x, oy + ext.y, ext.w, ext.h);
    return lm_rect_make(p->x, p->y, p->geometry.w, p->geometry.h);
}

void lm_wm_popup_destroy(struct lm_popup *p)
{
    struct lm_wm *wm = p->wm;
    if (p->mapped) {
        struct lm_rect r = popup_bounds(p);
        damage_rect(wm, &r);
    }
    lm_list_remove(&p->link);
    free(p);
    refresh_pointer(wm);
}

void lm_wm_popup_set_position(struct lm_popup *p, int32_t gx, int32_t gy)
{
    struct lm_rect before = popup_bounds(p);
    p->x = gx;
    p->y = gy;
    if (p->mapped) {
        damage_rect(p->wm, &before);
        struct lm_rect after = popup_bounds(p);
        damage_rect(p->wm, &after);
    }
}

void lm_wm_popup_set_geometry(struct lm_popup *p, const struct lm_rect *g)
{
    struct lm_rect before = popup_bounds(p);
    p->geometry = *g;
    if (p->mapped) {
        damage_rect(p->wm, &before);
        struct lm_rect after = popup_bounds(p);
        damage_rect(p->wm, &after);
    }
}

void lm_wm_popup_map(struct lm_popup *p)
{
    p->mapped = true;
    struct lm_rect r = popup_bounds(p);
    damage_rect(p->wm, &r);
    refresh_pointer(p->wm);
}

void lm_wm_popup_unmap(struct lm_popup *p)
{
    if (!p->mapped)
        return;
    struct lm_rect r = popup_bounds(p);
    p->mapped = false;
    damage_rect(p->wm, &r);
    refresh_pointer(p->wm);
}

bool popup_visible(const struct lm_wm *wm, const struct lm_popup *p, int depth)
{
    if (!p->mapped)
        return false;
    if (depth > 8)
        return false;
    struct lm_surface *root = p->parent ? lm_surface_root(p->parent) : NULL;
    if (!root)
        return true;
    struct lm_window *w;
    lm_list_for_each(w, &wm->windows, link)
        if (w->surface == root)
            return win_visible(wm, w);
    struct lm_popup *q;
    lm_list_for_each(q, &wm->popups, link)
        if (q != p && q->surface == root)
            return popup_visible(wm, q, depth + 1);
    return true;
}

/* ---- queries ------------------------------------------------------------------------------------ */

bool lm_wm_surface_origin(struct lm_wm *wm, struct lm_surface *s, int32_t *x, int32_t *y)
{
    struct lm_surface *root = lm_surface_root(s);
    int32_t dx = 0, dy = 0;
    if (root != s)
        lm_surface_offset_in_root(s, &dx, &dy);
    struct lm_window *w;
    lm_list_for_each(w, &wm->windows, link)
        if (w->surface == root && w->mapped) {
            win_origin(w, x, y);
            *x += dx;
            *y += dy;
            return true;
        }
    struct lm_popup *p;
    lm_list_for_each(p, &wm->popups, link)
        if (p->surface == root && p->mapped) {
            popup_origin(p, x, y);
            *x += dx;
            *y += dy;
            return true;
        }
    return false;
}

void lm_wm_surface_damage(struct lm_wm *wm, struct lm_surface *root, const struct lm_rect *rect)
{
    struct lm_window *w;
    lm_list_for_each(w, &wm->windows, link)
        if (w->surface == root) {
            if (!win_visible(wm, w))
                return;
            int32_t ox, oy;
            win_origin(w, &ox, &oy);
            struct lm_rect r = lm_rect_make(ox + rect->x, oy + rect->y, rect->w, rect->h);
            damage_rect(wm, &r);
            return;
        }
    struct lm_popup *p;
    lm_list_for_each(p, &wm->popups, link)
        if (p->surface == root) {
            if (!p->mapped)
                return;
            int32_t ox, oy;
            popup_origin(p, &ox, &oy);
            struct lm_rect r = lm_rect_make(ox + rect->x, oy + rect->y, rect->w, rect->h);
            damage_rect(wm, &r);
            return;
        }
}

void lm_wm_send_frame_done(struct lm_wm *wm, uint32_t time_ms)
{
    struct lm_window *w;
    lm_list_for_each(w, &wm->windows, link) {
        if (!win_visible(wm, w))
            continue;
        update_outputs(wm, w);
        lm_surface_send_frame_done(w->surface, time_ms);
    }
    struct lm_popup *p;
    lm_list_for_each(p, &wm->popups, link)
        if (popup_visible(wm, p, 0))
            lm_surface_send_frame_done(p->surface, time_ms);
}

/* ---- scene ---------------------------------------------------------------------------------------- */

/* ---- picking ----------------------------------------------------------------------------------------- */

static uint32_t edges_at(const struct lm_window *w, const struct lm_rect *o, double x, double y)
{
    if (w->states & LM_WINDOW_MAXIMIZED)
        return LM_EDGE_NONE;
    uint32_t e = 0;
    double lx = x - o->x, ly = y - o->y;
    if (lx < LM_BORDER_WIDTH) e |= LM_EDGE_LEFT;
    if (lx >= o->w - LM_BORDER_WIDTH) e |= LM_EDGE_RIGHT;
    if (ly >= o->h - LM_BORDER_WIDTH) e |= LM_EDGE_BOTTOM;
    if (ly < LM_BORDER_WIDTH) e |= LM_EDGE_TOP;
    if (e) {
        /* widen into corners */
        if ((e & (LM_EDGE_LEFT | LM_EDGE_RIGHT)) && !(e & (LM_EDGE_TOP | LM_EDGE_BOTTOM))) {
            if (ly < RESIZE_CORNER) e |= LM_EDGE_TOP;
            else if (ly >= o->h - RESIZE_CORNER) e |= LM_EDGE_BOTTOM;
        } else if ((e & (LM_EDGE_TOP | LM_EDGE_BOTTOM)) && !(e & (LM_EDGE_LEFT | LM_EDGE_RIGHT))) {
            if (lx < RESIZE_CORNER) e |= LM_EDGE_LEFT;
            else if (lx >= o->w - RESIZE_CORNER) e |= LM_EDGE_RIGHT;
        }
    }
    return e;
}

void lm_wm_pick(struct lm_wm *wm, double x, double y, struct lm_pick *out)
{
    memset(out, 0, sizeof(*out));
    out->kind = LM_PICK_NONE;
    int32_t ix = (int32_t)x, iy = (int32_t)y;

    if (osk_pick(wm, ix, iy, out))                               /* the on-screen keyboard is above everything, dialogs included */
        return;
    if (wm->menu_open && !lm_lock_active(wm->srv->lock)) {   /* a menu (above every dialog) takes every pick while it is open */
        struct lm_menu_info mi;
        menu_info(wm, &mi);
        out->kind = lm_rect_contains(&mi.rect, ix, iy) ? LM_PICK_MENU : LM_PICK_MENU_BACKDROP;
        for (int i = 0; i < mi.n; i++)
            if (mi.enabled[i] && lm_rect_contains(&mi.entry[i], ix, iy)) {
                out->kind = LM_PICK_MENU_ITEM;
                out->index = i;
            }
        out->rect = mi.rect;
        return;
    }
    if (launcher_pick(wm, ix, iy, out))                          /* the launcher dialog is modal: it gets every pick */
        return;
    struct lm_output *ao = about_output(wm);
    if (ao && !lm_lock_active(wm->srv->lock)) {                  /* the About dialog is modal, too */
        struct lm_about_info ai;
        about_info(wm, ao, &ai);
        out->kind = lm_rect_contains(&ai.card, ix, iy) ? LM_PICK_ABOUT_CARD : LM_PICK_ABOUT_BACKDROP;
        if (lm_rect_contains(&ai.close, ix, iy))
            out->kind = LM_PICK_ABOUT_BUTTON;
        out->rect = ai.card;
        return;
    }
    struct lm_output *po = power_output(wm);
    if (po && !lm_lock_active(wm->srv->lock)) {                  /* the session dialog is modal: it gets every pick */
        struct lm_power_info pi;
        power_info(wm, po, &pi);
        out->kind = lm_rect_contains(&pi.card, ix, iy) ? LM_PICK_POWER_CARD : LM_PICK_POWER_BACKDROP;
        for (int i = 0; i < LM_POWER_N; i++)
            if (lm_rect_contains(&pi.hit[i], ix, iy)) {
                out->kind = LM_PICK_POWER_BUTTON;
                out->index = i;
            }
        out->rect = pi.card;
        return;
    }

    if (wm->nnotes > 0 && !lm_lock_active(wm->srv->lock)) {
        struct lm_rect rs[LM_NOTIFY_MAX];
        if (note_rects(wm, rs))
            for (int i = 0; i < wm->nnotes; i++)
                if (lm_rect_contains(&rs[i], ix, iy)) {
                    out->kind = LM_PICK_NOTIFY;
                    out->index = wm->notes[i].id;
                    return;
                }
    }
    if (wm->srv->render && lm_render_shot_card_at(wm->srv->render, x, y)) {
        out->kind = LM_PICK_SHOT_CARD;
        return;
    }
    struct lm_popup *p;
    lm_list_for_each_reverse(p, &wm->popups, link) {
        if (!popup_visible(wm, p, 0))
            continue;
        int32_t ox, oy;
        double sx, sy;
        popup_origin(p, &ox, &oy);
        struct lm_surface *hit = lm_surface_tree_pick(p->surface, ox, oy, x, y, &sx, &sy);
        if (hit) {
            out->kind = LM_PICK_SURFACE;
            out->surface = hit;
            out->sx = sx;
            out->sy = sy;
            return;
        }
    }

    if (dock_pick(wm, ix, iy, out))                              /* the dock floats above the windows */
        return;
    if (overview_pick(wm, ix, iy, out))                          /* the desktop overview takes everything below the panel (and the dock) */
        return;

    struct lm_window *w;
    lm_list_for_each_reverse(w, &wm->windows, link) {
        if (!win_visible(wm, w) || ov_hides(wm, w))
            continue;
        int32_t ox, oy;
        double sx, sy;
        win_origin(w, &ox, &oy);
        struct lm_surface *hit = lm_surface_tree_pick(w->surface, ox, oy, x, y, &sx, &sy);
        if (hit) {
            out->kind = LM_PICK_SURFACE;
            out->window = w;
            out->surface = hit;
            out->sx = sx;
            out->sy = sy;
            return;
        }
        if ((w->states & LM_WINDOW_FULLSCREEN) || win_is_gadget(w))
            continue;
        struct lm_frame_info f;
        frame_info(wm, w, &f);
        if (!lm_rect_contains(&f.outer, ix, iy))
            continue;
        out->window = w;
        if (lm_rect_contains(&f.btn_close, ix, iy)) { out->kind = LM_PICK_CLOSE; return; }
        if (lm_rect_contains(&f.btn_max, ix, iy)) { out->kind = LM_PICK_MAXIMIZE; return; }
        if (lm_rect_contains(&f.btn_min, ix, iy)) { out->kind = LM_PICK_MINIMIZE; return; }
        uint32_t e = edges_at(w, &f.outer, x, y);
        if (e) {
            out->kind = LM_PICK_BORDER;
            out->edges = e;
            return;
        }
        if (lm_rect_contains(&f.titlebar, ix, iy)) {
            out->kind = LM_PICK_TITLEBAR;
            return;
        }
        /* inside the content area but the surface does not take input here: fall through */
        out->window = NULL;
    }

    if (wm->srv->backend) {
        struct lm_output *o;
        lm_list_for_each(o, &wm->srv->backend->outputs, link) {
            struct lm_rect pr = output_panel_rect(o);
            if (!lm_rect_contains(&pr, ix, iy) || fullscreen_covers(wm, o))
                continue;
            struct lm_panel_info pi;
            panel_info(wm, o, &pi);
            out->kind = LM_PICK_PANEL;
            int lcx = pi.launcher.x + pi.launcher.w / 2, lcy = pi.launcher.y + pi.launcher.h / 2, lr = pi.launcher.w / 2 + 1;
            if ((ix - lcx) * (ix - lcx) + (iy - lcy) * (iy - lcy) <= lr * lr) {           /* the circle, not its square */
                out->kind = LM_PICK_PANEL_LAUNCHER;
                out->rect = pi.launcher;
            }
            if (lm_rect_contains(&pi.clock, ix, iy) && wm->srv->config.cal_enabled) {
                out->kind = LM_PICK_PANEL_CLOCK;
                out->rect = pi.clock;
            }
            int pcx = pi.power.x + pi.power.w / 2, pcy = pi.power.y + pi.power.h / 2, pr2 = pi.power.w / 2 + 1;
            if ((ix - pcx) * (ix - pcx) + (iy - pcy) * (iy - pcy) <= pr2 * pr2)   /* the circle, not its square */
                out->kind = LM_PICK_PANEL_POWER;
            if (lm_rect_contains(&pi.layout, ix, iy))
                out->kind = LM_PICK_PANEL_LAYOUT;
            if (lm_rect_contains(&pi.osk, ix, iy))
                out->kind = LM_PICK_PANEL_OSK;
            if (lm_rect_contains(&pi.quick, ix, iy)) {
                out->kind = LM_PICK_PANEL_QUICK;
                out->rect = pi.quick;
            }
            if (lm_rect_contains(&pi.sticky, ix, iy))
                out->kind = LM_PICK_PANEL_STICKY;
            for (int i = 0; i < pi.ntray; i++)
                if (lm_rect_contains(&pi.tray[i].rect, ix, iy)) {
                    out->kind = LM_PICK_PANEL_TRAY;
                    out->index = pi.tray[i].id;
                    out->rect = pi.tray[i].rect;
                }
            for (int i = 0; i < LM_NUM_WORKSPACES; i++)
                if (lm_rect_contains(&pi.workspace[i], ix, iy)) {
                    out->kind = LM_PICK_PANEL_WORKSPACE;
                    out->index = i;
                }
            for (int i = 0; i < pi.ntasks; i++)
                if (lm_rect_contains(&pi.task[i].rect, ix, iy)) {
                    out->kind = LM_PICK_PANEL_TASK;
                    out->window = pi.task_win[i];
                }
            return;
        }
    }
}

/* ---- pointer motion / hover ---------------------------------------------------------------------------- */

static enum lm_cursor_shape cursor_for_edges(uint32_t e)
{
    switch (e) {
    case LM_EDGE_TOP: return LM_CURSOR_RESIZE_N;
    case LM_EDGE_BOTTOM: return LM_CURSOR_RESIZE_S;
    case LM_EDGE_LEFT: return LM_CURSOR_RESIZE_W;
    case LM_EDGE_RIGHT: return LM_CURSOR_RESIZE_E;
    case LM_EDGE_TOP | LM_EDGE_LEFT: return LM_CURSOR_RESIZE_NW;
    case LM_EDGE_TOP | LM_EDGE_RIGHT: return LM_CURSOR_RESIZE_NE;
    case LM_EDGE_BOTTOM | LM_EDGE_LEFT: return LM_CURSOR_RESIZE_SW;
    case LM_EDGE_BOTTOM | LM_EDGE_RIGHT: return LM_CURSOR_RESIZE_SE;
    }
    return LM_CURSOR_NONE;
}

void lm_wm_pointer_motion(struct lm_wm *wm, const struct lm_pick *pick)
{
    osk_drag_motion(wm);
    dock_drag_motion(wm);
    launcher_drag_motion(wm);
    overview_drag_motion(wm);
    struct lm_window *hw = NULL;
    uint32_t bits = 0;
    int pws = -1;
    bool about_btn = false, launcher = false, power_btn = false, layout_btn = false, sticky_btn = false, osk_btn = false, quick_btn = false;
    int power_item = -1, dm_item = -1;
    struct lm_window *task = NULL;
    int tray_id = 0;
    enum lm_cursor_shape shape = LM_CURSOR_NONE;

    switch (pick->kind) {
    case LM_PICK_CLOSE: hw = pick->window; bits = LM_FRAME_HOVER_CLOSE; break;
    case LM_PICK_MAXIMIZE: hw = pick->window; bits = LM_FRAME_HOVER_MAXIMIZE; break;
    case LM_PICK_MINIMIZE: hw = pick->window; bits = LM_FRAME_HOVER_MINIMIZE; break;
    case LM_PICK_BORDER: shape = cursor_for_edges(pick->edges); break;
    case LM_PICK_SHOT_CARD: shape = LM_CURSOR_POINTER; break;
    case LM_PICK_OVERVIEW_ITEM: case LM_PICK_OVERVIEW_WS: shape = LM_CURSOR_POINTER; break;
    case LM_PICK_NOTIFY: shape = LM_CURSOR_POINTER; break;
    case LM_PICK_PANEL_LAUNCHER: launcher = true; shape = LM_CURSOR_POINTER; break;
    case LM_PICK_ABOUT_BUTTON: about_btn = true; shape = LM_CURSOR_POINTER; break;
    case LM_PICK_DOCK_ITEM: shape = LM_CURSOR_POINTER; break;
    case LM_PICK_LAUNCHER_ITEM: case LM_PICK_LAUNCHER_REMOVE: case LM_PICK_LAUNCHER_ADD: case LM_PICK_LAUNCHER_BUTTON:
    case LM_PICK_LAUNCHER_KIND: case LM_PICK_LAUNCHER_HELP: case LM_PICK_LAUNCHER_HELPCLOSE: shape = LM_CURSOR_POINTER; break;
    case LM_PICK_PANEL_CLOCK: shape = LM_CURSOR_POINTER; break;
    case LM_PICK_PANEL_POWER: power_btn = true; shape = LM_CURSOR_POINTER; break;
    case LM_PICK_POWER_BUTTON: power_item = pick->index; shape = LM_CURSOR_POINTER; break;
    case LM_PICK_MENU_ITEM: dm_item = pick->index; shape = LM_CURSOR_POINTER; break;
    case LM_PICK_PANEL_LAYOUT: layout_btn = true; shape = LM_CURSOR_POINTER; break;
    case LM_PICK_PANEL_STICKY: sticky_btn = true; shape = LM_CURSOR_POINTER; break;
    case LM_PICK_PANEL_OSK: osk_btn = true; shape = LM_CURSOR_POINTER; break;
    case LM_PICK_PANEL_QUICK: quick_btn = true; shape = LM_CURSOR_POINTER; break;
    case LM_PICK_OSK_KEY: shape = LM_CURSOR_POINTER; break;
    case LM_PICK_OSK: shape = LM_CURSOR_MOVE; break;
    case LM_PICK_OSK_GRIP: shape = cursor_for_edges(pick->edges); break;
    case LM_PICK_PANEL_WORKSPACE: pws = pick->index; shape = LM_CURSOR_POINTER; break;
    case LM_PICK_PANEL_TASK: task = pick->window; shape = LM_CURSOR_POINTER; break;
    case LM_PICK_PANEL_TRAY: tray_id = pick->index; shape = LM_CURSOR_POINTER; break;
    case LM_PICK_SURFACE:
        if (pick->window && win_is_gadget(pick->window)) {      /* a card: the program asks for a resize in the same zones (6 px, 3 px at the top) */
            const struct lm_window *gw = pick->window;
            uint32_t e = 0;
            if (pick->sx < 6) e |= LM_EDGE_LEFT;
            if (pick->sx >= gw->width - 6) e |= LM_EDGE_RIGHT;
            if (pick->sy >= gw->height - 6) e |= LM_EDGE_BOTTOM;
            if (pick->sy < 3) e |= LM_EDGE_TOP;
            shape = cursor_for_edges(e);
        }
        break;
    default: break;
    }
    if (bits)
        shape = LM_CURSOR_POINTER;
    dock_motion(wm, pick->kind == LM_PICK_DOCK_ITEM || pick->kind == LM_PICK_DOCK, pick->kind == LM_PICK_DOCK_ITEM ? pick->index : -1);

    if (hw != wm->hover_win || bits != wm->hover_bits) {
        if (wm->hover_win)
            damage_frame(wm, wm->hover_win);
        wm->hover_win = hw;
        wm->hover_bits = bits;
        if (hw)
            damage_frame(wm, hw);
    }
    if (wm->ov.open) {                                          /* the pointer moves the overview's highlight */
        int hi = pick->kind == LM_PICK_OVERVIEW_ITEM && !wm->ov.drag ? pick->index : -1, hws = pick->kind == LM_PICK_OVERVIEW_WS ? pick->index : -1;
        if (hi != wm->ov.hover || hws != wm->ov.hover_ws) {
            wm->ov.hover = hi;
            wm->ov.hover_ws = hws;
            if (hi >= 0)
                wm->ov.sel = hi;
            overview_damage(wm);
        }
    }
    if (wm->launcher_open && !wm->bubble.active) {              /* the pointer moves the launcher's highlight */
        struct lm_output *lo = launcher_output(wm);
        int hb = pick->kind == LM_PICK_LAUNCHER_BUTTON ? pick->index : -1, sel = wm->launcher_sel;
        if (pick->kind == LM_PICK_LAUNCHER_ITEM || pick->kind == LM_PICK_LAUNCHER_REMOVE)
            sel = pick->index;
        else if (pick->kind == LM_PICK_LAUNCHER_ADD && lo)
            sel = launcher_count();
        else if (pick->kind == LM_PICK_LAUNCHER_BACKDROP || pick->kind == LM_PICK_LAUNCHER_CARD)
            sel = wm->launcher_form ? sel : -1;
        if (hb != wm->launcher_hover_btn || sel != wm->launcher_sel) {
            wm->launcher_hover_btn = hb;
            wm->launcher_sel = sel;
            launcher_damage(wm);
        }
    }
    if (wm->osk_open) {                                         /* the key under the pointer lights up */
        int hk = pick->kind == LM_PICK_OSK_KEY ? pick->index : -1;
        if (hk != wm->osk_hover) {
            wm->osk_hover = hk;
            osk_damage(wm);
        }
    }
    if (wm->about_open && about_btn != wm->about_hover_close) {
        wm->about_hover_close = about_btn;
        about_damage(wm);
    }
    if (wm->menu_open && dm_item != wm->menu_sel && (dm_item >= 0 || pick->kind == LM_PICK_MENU || pick->kind == LM_PICK_MENU_BACKDROP)) {
        wm->menu_sel = dm_item;                              /* the pointer moves the menu's highlight */
        menu_damage(wm);
    }
    if (wm->power_open && power_item >= 0 && power_item != wm->power_sel) {     /* the pointer moves the dialog's highlight */
        wm->power_sel = power_item;
        power_damage(wm);
    }
    if (pws != wm->hover_panel_ws || launcher != wm->hover_launcher || power_btn != wm->hover_power ||
        task != wm->hover_task || layout_btn != wm->hover_layout ||
        sticky_btn != wm->hover_sticky || osk_btn != wm->hover_osk || quick_btn != wm->hover_quick || tray_id != wm->hover_tray) {
        wm->hover_tray = tray_id;
        wm->hover_quick = quick_btn;
        wm->hover_osk = osk_btn;
        wm->hover_layout = layout_btn;
        wm->hover_sticky = sticky_btn;
        wm->hover_power = power_btn;
        wm->hover_task = task;
        wm->hover_panel_ws = pws;
        wm->hover_launcher = launcher;
        damage_panels(wm);
    }
    if (wm->srv->seat && wm->grab_kind == GRAB_NONE)
        lm_seat_set_cursor_override(wm->srv->seat, shape);
}

/* ---- interactive move / resize ------------------------------------------------------------------------------ */

static void grab_end(struct lm_wm *wm)
{
    struct lm_window *w = wm->grab_win;
    enum grab_kind kind = wm->grab_kind;
    wm->grab_kind = GRAB_NONE;
    wm->grab_win = NULL;
    if (wm->wb.active)
        wm->wb.released = true;                                  /* the mesh settles and goes */
    if (wm->srv->seat) {
        lm_seat_pointer_ungrab(wm->srv->seat, &wm->grab);
        lm_seat_set_cursor_override(wm->srv->seat, LM_CURSOR_NONE);
    }
    if (w && kind == GRAB_RESIZE) {
        w->states &= ~LM_WINDOW_RESIZING;
        send_configure(wm, w, w->width, w->height);
    }
    if (w && (kind == GRAB_MOVE || kind == GRAB_RESIZE)) {
        if (win_is_gadget(w))
            WIN(w)->gad_placed = true;                          /* from now on it stays where the user put it */
        gadget_glue_end(wm);
        places_remember(wm, w);                                 /* the next window of the program opens here */
    }
}




static void grab_motion(struct lm_pointer_grab *g, uint32_t time_ms, double x, double y)
{
    (void)time_ms;
    struct lm_wm *wm = g->data;
    struct lm_window *w = wm->grab_win;
    if (!w)
        return;
    double dx = x - wm->grab_px, dy = y - wm->grab_py;
    if (win_is_gadget(w)) {                                     /* a card: snaps to the others and the screen's edges (wm_gadgets.c) */
        gadget_grab_motion(wm, w, dx, dy);
        return;
    }
    if (wm->grab_kind == GRAB_MOVE) {
        if (!wm->wb.active && wobble_wanted(wm, w))
            wobble_start(wm, w, wm->grab_px, wm->grab_py);               /* (the picture is taken before the window moves) */
        struct lm_rect before = frame_outer(w);
        int32_t tx = wm->grab_x + (int32_t)dx, ty = wm->grab_y + (int32_t)dy;
        struct lm_rect ua;
        lm_wm_usable_area(wm, (int32_t)x, (int32_t)y, &ua);
        int top = win_is_gadget(w) ? ua.y : ua.y + LM_TITLEBAR_HEIGHT;       /* (a gadget has no title bar above it) */
        if (ty < top && !(w->states & LM_WINDOW_FULLSCREEN))
            ty = top;
        if (!wm->wb.active && anim_glide_to(wm, w, tx, ty))
            return;
        w->x = tx;
        w->y = ty;
        damage_rect(wm, &before);
        damage_frame(wm, w);
        update_outputs(wm, w);
    } else if (wm->grab_kind == GRAB_RESIZE) {
        int32_t nw = wm->grab_w, nh = wm->grab_h;
        if (wm->grab_edges & LM_EDGE_RIGHT) nw = wm->grab_w + (int32_t)dx;
        if (wm->grab_edges & LM_EDGE_LEFT) nw = wm->grab_w - (int32_t)dx;
        if (wm->grab_edges & LM_EDGE_BOTTOM) nh = wm->grab_h + (int32_t)dy;
        if (wm->grab_edges & LM_EDGE_TOP) nh = wm->grab_h - (int32_t)dy;
        clamp_size(w, &nw, &nh);
        w->states |= LM_WINDOW_RESIZING;
        send_configure(wm, w, nw, nh);
    }
}

static void grab_button(struct lm_pointer_grab *g, uint32_t time_ms, uint32_t button, bool pressed)
{
    (void)time_ms;
    struct lm_wm *wm = g->data;
    if (!pressed && button == BTN_LEFT)
        grab_end(wm);
}

static void grab_cancel(struct lm_pointer_grab *g)
{
    struct lm_wm *wm = g->data;
    struct lm_window *w = wm->grab_win;
    enum grab_kind kind = wm->grab_kind;
    wm->grab_kind = GRAB_NONE;
    wm->grab_win = NULL;
    gadget_glue_end(wm);
    if (wm->wb.active)
        wm->wb.released = true;
    if (w && kind == GRAB_RESIZE) {
        w->states &= ~LM_WINDOW_RESIZING;
        send_configure(wm, w, w->width, w->height);
    }
}

static const struct lm_pointer_grab_ops grab_ops = {
    .motion = grab_motion,
    .button = grab_button,
    .cancel = grab_cancel,
};

void grab_begin(struct lm_wm *wm, struct lm_window *w, enum grab_kind kind, uint32_t edges)
{
    if (!wm->srv->seat || wm->grab_kind != GRAB_NONE || (w->states & LM_WINDOW_FULLSCREEN))
        return;
    double px, py;
    lm_seat_pointer_position(wm->srv->seat, &px, &py);

    if (kind == GRAB_RESIZE)
        WIN(w)->tiled = 0;
    if (kind == GRAB_MOVE && ((w->states & LM_WINDOW_MAXIMIZED) || WIN(w)->tiled)) {
        /* dragging a maximized or tiled window restores it under the pointer */
        struct wm_win *ww = WIN(w);
        double ratio = w->width > 0 ? (px - w->x) / w->width : 0.5;
        w->states &= ~LM_WINDOW_MAXIMIZED;
        ww->tiled = 0;
        int32_t rw = ww->have_restore ? ww->rw : w->width * 3 / 4;
        int32_t rh = ww->have_restore ? ww->rh : w->height * 3 / 4;
        ww->have_restore = false;
        struct lm_rect before = frame_outer(w);
        w->x = (int32_t)(px - rw * ratio);
        w->y = (int32_t)py + LM_TITLEBAR_HEIGHT / 2;
        send_configure(wm, w, rw, rh);
        damage_rect(wm, &before);
    }

    wm->grab_kind = kind;
    wm->grab_win = w;
    wm->grab_px = px;
    wm->grab_py = py;
    wobble_stop(wm);
    wm->glide_win = NULL;                                       /* (a window still gliding from the last drag stays where it is) */
    wm->grab_x = w->x;
    wm->grab_y = w->y;
    wm->grab_w = w->width;
    wm->grab_h = w->height;
    wm->grab_edges = edges;
    wm->glue.n = 0;
    if (kind == GRAB_MOVE && win_is_gadget(w))
        gadget_glue_begin(wm, w);                               /* the cards glued under this one come along */
    wm->grab.ops = &grab_ops;
    wm->grab.data = wm;
    if (lm_seat_pointer_grab(wm->srv->seat, &wm->grab) < 0) {
        wm->grab_kind = GRAB_NONE;
        wm->grab_win = NULL;
        return;
    }
    lm_seat_set_cursor_override(wm->srv->seat, kind == GRAB_MOVE ? LM_CURSOR_MOVE : cursor_for_edges(edges));
}






void lm_wm_window_request_move(struct lm_window *w)
{
    if (!lm_seat_pointer_button_held(w->wm->srv->seat))
        return;                                                 /* the button was let go before the request came: a move would never end */
    grab_begin(w->wm, w, GRAB_MOVE, 0);
}

void lm_wm_window_request_resize(struct lm_window *w, uint32_t edges)
{
    if (!edges || (w->states & LM_WINDOW_MAXIMIZED) || !lm_seat_pointer_button_held(w->wm->srv->seat))
        return;
    grab_begin(w->wm, w, GRAB_RESIZE, edges);
}

/* ---- buttons ---------------------------------------------------------------------------------------------------- */

uint32_t bit_for_kind(enum lm_pick_kind k)
{
    switch (k) {
    case LM_PICK_CLOSE: return LM_FRAME_HOVER_CLOSE;
    case LM_PICK_MAXIMIZE: return LM_FRAME_HOVER_MAXIMIZE;
    case LM_PICK_MINIMIZE: return LM_FRAME_HOVER_MINIMIZE;
    default: return 0;
    }
}

static void launch_run(struct lm_wm *wm)
{
    const char *cmd = wm->srv->config.run_cmd ? wm->srv->config.run_cmd : "mellagui-run";
    lm_server_spawn(wm->srv, cmd);
}

/* Quit, but with a screen lock set only after the secret was typed (lock.h). */
void request_quit(struct lm_wm *wm)
{
    if (!lm_lock_request_quit(wm->srv->lock))
        lm_server_quit(wm->srv);
}

void launch_settings(struct lm_wm *wm)
{
    if (settings_present(wm))
        return;
    wm->settings_spawn_ms = (int64_t)lm_now_ms();
    const char *cmd = wm->srv->config.settings_cmd ? wm->srv->config.settings_cmd : "mellagui-settings";
    lm_server_spawn(wm->srv, cmd);
}

static void launch_terminal(struct lm_wm *wm)
{
    const char *cmd = wm->srv->config.terminal_cmd ? wm->srv->config.terminal_cmd : "mellagui-term";
    lm_server_spawn(wm->srv, cmd);
}

/* Click on the screenshot card: the card goes away and, when the shot was saved, the viewer opens it. */
void open_shot(struct lm_wm *wm)
{
    char path[512];
    if (!lm_render_shot_dismiss(wm->srv->render, path, sizeof(path)))
        return;
    const char *view = wm->srv->config.view_cmd ? wm->srv->config.view_cmd : "mellagui-view";
    char cmd[1200], quoted[1100];
    size_t o = 0;
    for (const char *p = path; *p && o + 5 < sizeof(quoted); p++) {          /* single-quote the path for sh */
        if (*p == '\'') {
            memcpy(quoted + o, "'\\''", 4);
            o += 4;
        } else {
            quoted[o++] = *p;
        }
    }
    quoted[o] = 0;
    snprintf(cmd, sizeof(cmd), "%s '%s'", view, quoted);
    lm_server_spawn(wm->srv, cmd);
}

/* ---- keyboard shortcuts ---------------------------------------------------------------------------------------------- */

void move_to_workspace(struct lm_wm *wm, struct lm_window *w, int ws)
{
    if (ws < 0 || ws >= LM_NUM_WORKSPACES || w->workspace == ws)
        return;
    struct lm_rect r = frame_outer(w);
    lm_xdg_dismiss_popups(w->surface);
    w->workspace = ws;
    update_outputs(wm, w);
    damage_rect(wm, &r);
    if (wm->focused == w)
        set_focus(wm, top_visible(wm, w));
    damage_panels(wm);
    overview_changed(wm);
    refresh_pointer(wm);
}

static void cycle_focus(struct lm_wm *wm, bool backwards)
{
    struct lm_window *w, *pick = NULL;
    if (backwards) {
        /* second from the top: swap back to the previous window */
        int seen = 0;
        lm_list_for_each_reverse(w, &wm->windows, link)
            if (w->mapped && w->workspace == wm->workspace && !w->minimized && !win_is_aux(w) && ++seen == 2) {
                pick = w;
                break;
            }
    } else {
        /* the lowest window of the workspace (minimized ones included) comes to the front */
        lm_list_for_each(w, &wm->windows, link)
            if (w->mapped && w->workspace == wm->workspace && w != wm->focused && !win_is_aux(w)) {
                pick = w;
                break;
            }
    }
    if (pick)
        lm_wm_focus_window(wm, pick);
}

void lm_wm_keyboard_state_changed(struct lm_wm *wm)
{
    damage_panels(wm);
}

void lm_wm_lock_shield_changed(struct lm_wm *wm)
{
    if (wm)
        damage_all(wm);
}

void lm_wm_lock_changed(struct lm_wm *wm, bool locked)
{
    if (!wm)
        return;
    if (locked) {
        overview_finish(wm);
        menu_set(wm, false);
        sw_close(wm);
        power_set(wm, false, NULL);
        about_set(wm, false, NULL);
        if (wm->srv->saver)
            lm_saver_locked(wm->srv->saver, true);
        if (wm->focused)
            lm_xdg_dismiss_popups(wm->focused->surface);
    } else if (wm->srv->seat) {
        if (wm->srv->saver)
            lm_saver_locked(wm->srv->saver, false);
        struct lm_window *w = wm->focused ? wm->focused : top_visible(wm, NULL);
        if (w && w != wm->focused)
            set_focus(wm, w);
        else if (w)
            lm_seat_set_keyboard_focus(wm->srv->seat, w->surface);
    }
    damage_all(wm);
}

/* Print: the output under the pointer, Alt+Print: the focused window with its frame (the whole output when there is
 * none), Ctrl+Print: like Print but only to the clipboard. */
static void take_screenshot(struct lm_wm *wm, bool window, bool to_file)
{
    struct lm_rect r;
    if (window && wm->focused && win_visible(wm, wm->focused)) {
        r = frame_outer(wm->focused);
    } else {
        double x = 0, y = 0;
        if (wm->srv->seat)
            lm_seat_pointer_position(wm->srv->seat, &x, &y);
        struct lm_output *o = lm_output_nearest(wm->srv, x, y);
        if (!o)
            return;
        r = lm_output_rect(o);
    }
    lm_screenshot_take(wm->srv, &r, to_file);
}

/* One shortcut of the [keys] table (keys.h). Returns true when the key is swallowed (every bound key is, press and release). */
static bool run_keybind(struct lm_wm *wm, const struct lm_keybind *kb, bool pressed, bool repeat)
{
    struct lm_window *f = wm->focused;
    if (!pressed)
        return true;
    if (repeat && kb->action != LM_ACT_FOCUS_NEXT && kb->action != LM_ACT_FOCUS_PREV)
        return true;
    switch (kb->action) {
    case LM_ACT_TERMINAL: launch_terminal(wm); break;
    case LM_ACT_RUN: launch_run(wm); break;
    case LM_ACT_LAUNCHER: launcher_set(wm, !wm->launcher_open, NULL); break;
    case LM_ACT_OSK: osk_set(wm, !wm->osk_open); break;
    case LM_ACT_QUICK: lm_wm_quick_toggle(wm, NULL); break;
    case LM_ACT_MAGNIFY_IN:
    case LM_ACT_MAGNIFY_OUT: {                                          /* the magnifier: Super+= zooms in (switching it on), Super+- out (off at the end) */
        static const int steps[] = { 125, 150, 200, 300, 400, 600, 800 };
        int cur = wm->srv->config.magnifier ? wm->srv->config.magnifier_zoom : 100, next = cur, count = (int)(sizeof(steps) / sizeof(steps[0]));
        if (kb->action == LM_ACT_MAGNIFY_IN) {
            next = steps[count - 1];
            for (int i = count - 1; i >= 0; i--)
                if (steps[i] > cur)
                    next = steps[i];
            if (cur >= steps[count - 1])
                next = cur;
        } else {
            next = 100;
            for (int i = 0; i < count; i++)
                if (steps[i] < cur)
                    next = steps[i];
        }
        if (wm->srv->settings && next != cur) {
            char v[16];
            if (next > 100) {
                snprintf(v, sizeof(v), "%d", next);
                lm_settings_set(wm->srv->settings, "magnifier-zoom", v);
            }
            lm_settings_set(wm->srv->settings, "magnifier", next > 100 ? "1" : "0");
        }
        break;
    }
    case LM_ACT_OVERVIEW: overview_set(wm, !wm->ov.open || wm->ov.closing); break;
    case LM_ACT_NIGHT_LIGHT: lm_night_toggle(wm->srv->night); break;
    case LM_ACT_DND:
        if (wm->srv->settings)
            lm_settings_set(wm->srv->settings, "dnd", wm->srv->config.dnd ? "0" : "1");
        break;
    case LM_ACT_SETTINGS: launch_settings(wm); break;
    case LM_ACT_EXEC: {
        char cmd[2048];
        lm_launch_expand(kb->arg, cmd, sizeof(cmd));
        lm_server_spawn(wm->srv, cmd);
        break;
    }
    case LM_ACT_GADGET_TOGGLE: {                                        /* gadget-toggle NAME: a built-in card or one from the gadgets folder */
        int g = lm_wm_gadget_by_name(wm, kb->arg);
        if (g >= 0)
            lm_wm_set_gadget_id(wm, g, !wm->gad[g].visible);
        break;
    }
    case LM_ACT_LAYOUT_NEXT: case LM_ACT_LAYOUT_PREV:
        if (wm->srv->input)
            lm_input_cycle_layout(wm->srv->input, kb->action == LM_ACT_LAYOUT_PREV ? -1 : 1);
        break;
    case LM_ACT_GADGET_CLIPBOARD:
        lm_wm_set_gadget(wm, !wm->gad[LM_GADGET_CLIPBOARD].visible);
        break;
    case LM_ACT_CLIPBOARD_PICK: {               /* pick from the clipboard history with the arrow keys: the card gets the keyboard */
        if (!wm->gad[LM_GADGET_CLIPBOARD].visible)
            lm_wm_set_gadget(wm, true);
        struct lm_window *cw = gadget_window(wm, LM_GADGET_CLIPBOARD);
        if (cw)
            set_focus(wm, cw);
        break;
    }
    case LM_ACT_GADGET_MIXER: lm_wm_set_gadget_id(wm, LM_GADGET_MIXER, !wm->gad[LM_GADGET_MIXER].visible); break;
    case LM_ACT_GADGET_SYSTEM: lm_wm_set_gadget_id(wm, LM_GADGET_SYSTEM, !wm->gad[LM_GADGET_SYSTEM].visible); break;
    case LM_ACT_GADGET_SMART: lm_wm_set_gadget_id(wm, LM_GADGET_SMART, !wm->gad[LM_GADGET_SMART].visible); break;
    case LM_ACT_CLOSE:
        if (f) lm_wm_window_close(f);
        break;
    case LM_ACT_LOCK:                           /* only when a password / PIN is set */
        if (!lm_lock_now(wm->srv->lock))
            lm_info("lock: the lock shortcut is ignored, no password or PIN is set (settings: Security)");
        break;
    case LM_ACT_WORKSPACE: lm_wm_switch_workspace(wm, kb->num); break;
    case LM_ACT_MOVE_TO_WORKSPACE:
        if (f) move_to_workspace(wm, f, kb->num);
        break;
    case LM_ACT_FOCUS_NEXT: cycle_focus(wm, false); break;
    case LM_ACT_FOCUS_PREV: cycle_focus(wm, true); break;
    case LM_ACT_FULLSCREEN:
        if (f) lm_wm_window_request_fullscreen(f, !(f->states & LM_WINDOW_FULLSCREEN));
        break;
    case LM_ACT_MAXIMIZE_TOGGLE:
        if (f) lm_wm_window_request_maximize(f, !(f->states & LM_WINDOW_MAXIMIZED));
        break;
    case LM_ACT_MAXIMIZE:
        if (f) lm_wm_window_request_maximize(f, true);
        break;
    case LM_ACT_RESTORE:                        /* un-maximize, or un-tile back to the pre-tile geometry */
        if (f) {
            if (f->states & LM_WINDOW_MAXIMIZED)
                lm_wm_window_request_maximize(f, false);
            else
                lm_wm_window_request_tile(f, LM_EDGE_NONE);
        }
        break;
    case LM_ACT_TILE_LEFT: case LM_ACT_TILE_RIGHT:
        if (f) lm_wm_window_request_tile(f, kb->action == LM_ACT_TILE_LEFT ? LM_EDGE_LEFT : LM_EDGE_RIGHT);
        break;
    case LM_ACT_QUIT: request_quit(wm); break;
    case LM_ACT_RELOAD_CONFIG:
        if (wm->srv->reload_config)
            wm->srv->reload_config(wm->srv, false);
        break;
    case LM_ACT_NONE: break;
    }
    return true;
}

void lm_wm_key_capture(struct lm_wm *wm, bool on)
{
    wm->key_capture = on;
    if (on)
        wm->key_captured[0] = 0;
}

bool lm_wm_key_capturing(const struct lm_wm *wm) { return wm->key_capture; }
const char *lm_wm_key_captured(const struct lm_wm *wm) { return wm->key_captured; }

static bool is_modifier_key(uint32_t code)
{
    return code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT || code == KEY_LEFTCTRL || code == KEY_RIGHTCTRL ||
           code == KEY_LEFTALT || code == KEY_RIGHTALT || code == KEY_LEFTMETA || code == KEY_RIGHTMETA ||
           code == KEY_CAPSLOCK || code == KEY_NUMLOCK || code == KEY_SCROLLLOCK;
}

bool lm_wm_key(struct lm_wm *wm, uint32_t code, bool pressed, bool repeat, uint32_t mods)
{
    if (!repeat && code < 24 * 32 && (((wm->osk_phys[code / 32] >> (code % 32)) & 1u) != 0) != pressed) {
        wm->osk_phys[code / 32] ^= 1u << (code % 32);           /* the on-screen keyboard shows what is held on the real one */
        if (wm->osk_open)
            osk_damage(wm);
    }
    mods &= ~(uint32_t)(LM_MOD_CAPS | LM_MOD_NUM);
    if (wm->capture_release && code == wm->capture_release && !pressed) {
        wm->capture_release = 0;                                /* the release of the key that was just recorded */
        return true;
    }
    if (wm->key_capture && !is_modifier_key(code)) {
        if (pressed && !repeat) {
            char text[64];
            bool cancel = code == KEY_ESC && !(mods & LM_KEY_MODS_MASK);
            if (cancel || lm_keys_combo_text(mods & LM_KEY_MODS_MASK, code, text, sizeof(text))) {
                snprintf(wm->key_captured, sizeof(wm->key_captured), "%s", cancel ? "" : text);
                wm->key_capture = false;
                wm->capture_release = code;
                lm_settings_changed(wm->srv->settings);
            }
        }
        return true;
    }
    bool super = (mods & LM_MOD_SUPER) != 0;
    bool shift = (mods & LM_MOD_SHIFT) != 0;
    bool ctrl = (mods & LM_MOD_CTRL) != 0;
    bool alt = (mods & LM_MOD_ALT) != 0;

    if (wm->srv->config.kiosk && !(ctrl && alt && !super && code >= KEY_F1 && code <= KEY_F12))
        return false;                                           /* the login screen: every key goes to its program (Ctrl+Alt+Fn still switches VT) */

    if (wm->menu_open && !(ctrl && alt)) {                  /* a menu takes every key: Esc closes, Up / Down choose, Enter does it */
        if (pressed && !repeat) {
            struct lm_menu_info mi;
            menu_info(wm, &mi);
            if (code == KEY_ESC) {
                menu_set(wm, false);
            } else if (code == KEY_UP || code == KEY_DOWN) {
                int dir = code == KEY_UP ? -1 : 1, k = wm->menu_sel;
                for (int step = 0; step < mi.n; step++) {
                    k = k < 0 ? (dir > 0 ? 0 : mi.n - 1) : (k + dir + mi.n) % mi.n;
                    if (mi.enabled[k]) {
                        wm->menu_sel = k;
                        menu_damage(wm);
                        break;
                    }
                }
            } else if ((code == KEY_ENTER || code == KEY_KPENTER) && wm->menu_sel >= 0 && mi.enabled[wm->menu_sel]) {
                menu_activate(wm, wm->menu_sel);
            }
        }
        return true;
    }
    if (wm->ov.open && !(ctrl && alt)) {                        /* the desktop overview is modal: Esc, the arrows, Enter, Delete, 1..4 */
        if (pressed && !is_modifier_key(code))
            overview_key(wm, code, repeat, mods);
        return true;
    }
    if (wm->launcher_open && !(ctrl && alt)) {                  /* the launcher dialog is modal and takes every key */
        if (pressed && !is_modifier_key(code))
            launcher_key(wm, code, repeat, mods);
        return true;
    }
    if (wm->about_open && !(ctrl && alt)) {                     /* the About dialog is modal, too: Esc, Enter and Space close it */
        if (pressed && !repeat && (code == KEY_ESC || code == KEY_ENTER || code == KEY_KPENTER || code == KEY_SPACE))
            about_set(wm, false, NULL);
        return true;
    }
    if (wm->power_open && !(ctrl && alt)) {                     /* the session dialog is modal: it takes every key */
        if (pressed && !repeat) {
            if (wm->power_confirm) {                            /* "Are you sure?": Left / Right / Tab choose, Enter answers, Y / N, Esc closes */
                if (code == KEY_ESC || code == KEY_N) {
                    power_set(wm, false, NULL);
                } else if (code == KEY_Y) {
                    power_answer(wm, true);
                } else if (code == KEY_LEFT || code == KEY_RIGHT || code == KEY_TAB) {
                    wm->power_sel = wm->power_sel == 0 ? 1 : 0;
                    power_damage(wm);
                } else if ((code == KEY_ENTER || code == KEY_KPENTER) && wm->power_sel >= 0) {
                    power_activate(wm, wm->power_sel);
                }
                return true;
            }
            if (code == KEY_ESC)
                power_set(wm, false, NULL);
            else if (code == KEY_LEFT || code == KEY_RIGHT || code == KEY_TAB) {
                int dir = code == KEY_LEFT || (code == KEY_TAB && (mods & LM_MOD_SHIFT)) ? LM_POWER_N - 1 : 1;
                int k = wm->power_sel;
                for (int step = 0; step < LM_POWER_N; step++) {         /* (a button that is off is skipped) */
                    k = k < 0 ? (dir == 1 ? 0 : LM_POWER_N - 1) : (k + dir) % LM_POWER_N;
                    if (!(k == 2 && power_suspend_off(wm)))
                        break;
                }
                wm->power_sel = k;
                power_damage(wm);
            } else if ((code == KEY_ENTER || code == KEY_KPENTER) && wm->power_sel >= 0)
                power_activate(wm, wm->power_sel);
            else if (code == KEY_L)
                power_activate(wm, 0);
            else if (code == KEY_E)
                power_activate(wm, 1);
            else if (code == KEY_S)
                power_activate(wm, 2);
            else if (code == KEY_R)
                power_activate(wm, 3);
            else if (code == KEY_P)
                power_activate(wm, 4);
        }
        return true;
    }
    if (wm->sw_open) {                                          /* Alt+Tab: Alt released = go there, Esc = stay */
        if (!pressed && (code == KEY_LEFTALT || code == KEY_RIGHTALT)) {
            sw_commit(wm);
            return false;                                       /* the application still gets the release it never saw pressed */
        }
        if (pressed && code == KEY_ESC) {
            sw_close(wm);
            return true;
        }
    }
    if (code == KEY_TAB && alt && !super && !ctrl) {            /* Alt+Tab / Alt+Shift+Tab: the next / previous window in use order */
        if (pressed)
            sw_step(wm, shift);
        return true;
    }

    /* Ctrl+Alt+F1..F12 */
    if (ctrl && alt && !super) {
        int vt = 0;
        if (code >= KEY_F1 && code <= KEY_F10)
            vt = (int)(code - KEY_F1) + 1;
        else if (code == KEY_F11)
            vt = 11;
        else if (code == KEY_F12)
            vt = 12;
        if (vt) {
            if (pressed && !repeat && wm->srv->backend)
                lm_backend_switch_vt(wm->srv->backend, vt);
            return true;
        }
    }
    if ((code == KEY_SYSRQ || code == KEY_PRINT) && !super) {
        if (pressed && !repeat)
            take_screenshot(wm, alt, !ctrl);
        return true;
    }
    const struct lm_keybind *kb = lm_keys_find(&wm->srv->config.keys, code, mods);
    return kb && run_keybind(wm, kb, pressed, repeat);
}
