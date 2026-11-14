/* Part of the window manager (see wm_priv.h): the on-screen keyboard */
#include "wm_priv.h"

/* ---- the on-screen keyboard ------------------------------------------------------------------------------------------------- */

static bool osk_phys_down(const struct lm_wm *wm, uint32_t code)
{
    return code < 24 * 32 && ((wm->osk_phys[code / 32] >> (code % 32)) & 1u);
}

/* Puts floating window `w` at a geometry (the client is asked for the size when it differs) and repaints both places. */
static void osk_place(struct lm_wm *wm, struct lm_window *w, int32_t x, int32_t y, int32_t cw, int32_t ch)
{
    struct lm_rect before = frame_outer(w);
    w->x = x;
    w->y = y;
    if (cw != w->width || ch != w->height) {
        clamp_size(w, &cw, &ch);
        send_configure(wm, w, cw, ch);
    }
    damage_rect(wm, &before);
    damage_frame(wm, w);
    update_outputs(wm, w);
}

void lm_wm_osk_layout_changed(struct lm_wm *wm)
{
    bool avoid = wm->srv->config.osk_avoid && wm->osk_open;
    struct lm_window *w;
    lm_list_for_each(w, &wm->windows, link) {
        struct wm_win *ww = WIN(w);
        if (w->states & (LM_WINDOW_MAXIMIZED | LM_WINDOW_FULLSCREEN)) {
            ww->osk_moved = false;
            if (w->states & LM_WINDOW_MAXIMIZED)
                apply_state(wm, w);                              /* the usable area is another one now */
            continue;
        }
        if (!w->mapped || w->parent || win_is_gadget(w))        /* (the desktop cards stay where they are: the keyboard lies over them) */
            continue;
        int32_t bx = w->x, by = w->y, bw = w->width, bh = w->height;        /* the geometry the user gave it */
        if (ww->osk_moved) {
            bool same_pos = w->x == ww->osk_ax && w->y == ww->osk_ay;
            bool same_size = (w->width == ww->osk_aw && w->height == ww->osk_ah) || (w->width == ww->osk_ow && w->height == ww->osk_oh);
            if (same_pos && same_size) {
                bx = ww->osk_ox;
                by = ww->osk_oy;
                bw = ww->osk_ow;
                bh = ww->osk_oh;
            } else {
                ww->osk_moved = false;                           /* it was moved or resized since: that is the new original */
            }
        }
        int32_t nx = bx, ny = by, nw = bw, nh = bh;
        if (avoid) {
            struct lm_rect ua;
            lm_wm_usable_area(wm, bx + bw / 2, by + bh / 2, &ua);
            int32_t bottom = ua.y + ua.h - LM_BORDER_WIDTH, top = ua.y + LM_TITLEBAR_HEIGHT;
            if (by + bh > bottom) {                              /* its lower edge is under the keyboard (or the screen's edge): up, and smaller if it must */
                ny = LM_MAX(bottom - bh, top);
                if (ny + bh > bottom)
                    nh = bottom - ny;
            } else if (by < top) {                               /* the keyboard is at the top: down */
                ny = top;
                if (ny + bh > bottom)
                    nh = bottom - ny;
            }
            if (nh < bh)
                clamp_size(w, &nw, &nh);
        }
        bool differs = nx != bx || ny != by || nw != bw || nh != bh;
        if (differs && !ww->osk_moved) {
            ww->osk_moved = true;
            ww->osk_ox = bx;
            ww->osk_oy = by;
            ww->osk_ow = bw;
            ww->osk_oh = bh;
        }
        if (differs) {
            ww->osk_ax = nx;
            ww->osk_ay = ny;
            ww->osk_aw = nw;
            ww->osk_ah = nh;
            osk_place(wm, w, nx, ny, nw, nh);
        } else if (ww->osk_moved) {                              /* nothing is in its way any more: back where it was */
            ww->osk_moved = false;
            osk_place(wm, w, bx, by, bw, bh);
        }
    }
    damage_all(wm);
}

/* Is the on-screen key `k` lit by a key held on the real keyboard? A modifier key lights for either of its two keys. */
static bool osk_key_held(const struct lm_wm *wm, const struct lm_osk_key *k)
{
    switch (k->kind) {
    case LM_OSK_SHIFT: return osk_phys_down(wm, KEY_LEFTSHIFT) || osk_phys_down(wm, KEY_RIGHTSHIFT);
    case LM_OSK_CTRL: return osk_phys_down(wm, KEY_LEFTCTRL) || osk_phys_down(wm, KEY_RIGHTCTRL);
    case LM_OSK_SUPER: return osk_phys_down(wm, KEY_LEFTMETA) || osk_phys_down(wm, KEY_RIGHTMETA);
    case LM_OSK_HIDE: return false;
    default: return k->code && osk_phys_down(wm, k->code);
    }
}

/* Gives the keymap lookups (lm_keymap_keysym) the configured layouts and the active group, for labels and the launcher form. */
void keymap_sync(const struct lm_wm *wm)
{
    int layouts[LM_KBD_MAX_LAYOUTS], n = wm->srv->config.kbd_nlayouts;
    if (n < 1 || n > LM_KBD_MAX_LAYOUTS)
        n = 0;
    for (int i = 0; i < n; i++)
        layouts[i] = wm->srv->config.kbd_layouts[i];
    lm_keymap_set_groups(layouts, n);
    if (wm->srv->seat)
        lm_keymap_client_group(lm_seat_group(wm->srv->seat));
}

bool osk_geo(const struct lm_wm *wm, struct lm_osk_geo *g)
{
    if (!wm->osk_open || !wm->srv->backend)
        return false;
    struct lm_output *o = lm_output_primary(wm->srv);
    if (!o)
        return false;
    struct lm_rect r = lm_output_rect(o);
    lm_osk_geometry(&r, &wm->osk_pl, g);
    return true;
}

void osk_damage(struct lm_wm *wm)
{
    struct lm_osk_geo g;
    if (osk_geo(wm, &g))
        damage_rect(wm, &g.card);
    damage_panels(wm);
}

void osk_info(const struct lm_wm *wm, const struct lm_osk_geo *g, struct lm_osk_info *oi)
{
    memset(oi, 0, sizeof(*oi));
    oi->rect = g->card;
    oi->n = g->n;
    keymap_sync(wm);
    uint32_t locks = wm->srv->input ? lm_input_locks(wm->srv->input) : 0;
    uint32_t label_mods = wm->osk_latched;                      /* the labels show what the next key would give: also with a real Shift / AltGr held */
    if (osk_phys_down(wm, KEY_LEFTSHIFT) || osk_phys_down(wm, KEY_RIGHTSHIFT))
        label_mods |= LM_MOD_SHIFT;
    if (osk_phys_down(wm, KEY_RIGHTALT) && wm->srv->config.kbd_nlayouts > 1)
        label_mods |= LM_MOD_ALTGR;
    for (int i = 0; i < g->n; i++) {
        const struct lm_osk_key *k = &g->key[i];
        oi->key[i].rect = k->rect;
        oi->key[i].kind = (int)k->kind;
        oi->key[i].row = k->row;
        oi->key[i].code = k->code;
        lm_osk_label(k, label_mods, locks, oi->key[i].label);
        uint32_t st = 0;
        if (i == wm->osk_hover)
            st |= LM_OSK_KEY_HOVER;
        if (i == wm->osk_down || osk_key_held(wm, k))
            st |= LM_OSK_KEY_DOWN;
        if ((lm_osk_kind_mod(k->kind) & wm->osk_latched) || (k->kind == LM_OSK_CAPS && (locks & LM_MOD_CAPS)))
            st |= LM_OSK_KEY_ON;
        oi->key[i].state = st;
    }
}

static void osk_inject(struct lm_wm *wm, uint32_t code, bool pressed)
{
    if (!wm->srv->input || !code)
        return;
    struct lm_input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = LM_INPUT_KEY;
    ev.u.key.code = code;
    ev.u.key.pressed = pressed;
    lm_input_inject(wm->srv->input, &ev);
}

static const enum lm_osk_kind osk_mod_kinds[5] = { LM_OSK_SHIFT, LM_OSK_CTRL, LM_OSK_ALT, LM_OSK_ALTGR, LM_OSK_SUPER };

/* The held key comes up (and the modifiers that were pressed with it): the pointer let go, or the keyboard closes. */
void osk_release(struct lm_wm *wm)
{
    if (wm->osk_down < 0)
        return;
    wm->osk_down = -1;
    osk_inject(wm, wm->osk_down_code, false);
    for (int i = 4; i >= 0; i--)
        if (wm->osk_down_mods & lm_osk_kind_mod(osk_mod_kinds[i]))
            osk_inject(wm, lm_osk_kind_code(osk_mod_kinds[i]), false);
    wm->osk_down_code = wm->osk_down_mods = 0;
    osk_damage(wm);
}

void osk_set(struct lm_wm *wm, bool open)
{
    if (open == wm->osk_open)
        return;
    if (open) {
        wm->osk_open = true;
        wm->osk_latched = 0;
        wm->osk_hover = wm->osk_down = -1;
    } else {
        osk_release(wm);
        osk_damage(wm);                                         /* (while the geometry is still known) */
        wm->osk_open = false;
        wm->osk_latched = 0;
        wm->hover_osk = false;
        wm->osk_hover = -1;
        wm->osk_drag = 0;
    }
    osk_damage(wm);
    lm_wm_osk_layout_changed(wm);                               /* the windows make room for it / come back */
}

/* A press on key `i`: modifiers latch for the next key, Hide closes, anything else is typed (pressed now, released with the pointer). */
void osk_press(struct lm_wm *wm, int i)
{
    struct lm_osk_geo g;
    if (!osk_geo(wm, &g) || i < 0 || i >= g.n)
        return;
    const struct lm_osk_key *k = &g.key[i];
    uint32_t mod = lm_osk_kind_mod(k->kind);
    if (k->kind == LM_OSK_HIDE) {
        osk_set(wm, false);
    } else if (mod) {
        wm->osk_latched ^= mod;
        osk_damage(wm);
    } else {
        osk_release(wm);
        wm->osk_down = i;
        wm->osk_down_code = k->code;
        wm->osk_down_mods = wm->osk_latched;
        wm->osk_latched = 0;
        for (int m = 0; m < 5; m++)
            if (wm->osk_down_mods & lm_osk_kind_mod(osk_mod_kinds[m]))
                osk_inject(wm, lm_osk_kind_code(osk_mod_kinds[m]), true);
        osk_inject(wm, k->code, true);
        osk_damage(wm);
    }
}

bool osk_pick(const struct lm_wm *wm, int ix, int iy, struct lm_pick *out)
{
    struct lm_osk_geo g;
    if (lm_lock_active(wm->srv->lock) || !osk_geo(wm, &g) || !lm_rect_contains(&g.card, ix, iy))
        return false;
    out->kind = LM_PICK_OSK;
    out->rect = g.card;
    int m = lm_osk_margin();
    uint32_t edges = 0;                                         /* the right and the bottom margin resize (both: the corner) */
    if (ix >= g.card.x + g.card.w - m)
        edges |= LM_EDGE_RIGHT;
    if (iy >= g.card.y + g.card.h - m)
        edges |= LM_EDGE_BOTTOM;
    if (edges) {
        out->kind = LM_PICK_OSK_GRIP;
        out->edges = edges;
        return true;
    }
    for (int i = 0; i < g.n; i++)
        if (lm_rect_contains(&g.key[i].rect, ix, iy)) {
            out->kind = LM_PICK_OSK_KEY;
            out->index = i;
        }
    return true;
}

/* Dragging the keyboard: its margin moves it, its right / bottom margin resizes it (the top left corner stays where it is). The place is
 * pinned at the first press, so from then on it stays where the user put it. */
void osk_drag_begin(struct lm_wm *wm, int kind, uint32_t edges)
{
    struct lm_osk_geo g;
    if (!osk_geo(wm, &g) || !wm->srv->seat)
        return;
    osk_release(wm);
    wm->osk_start.x = g.card.x;
    wm->osk_start.y = g.card.y;
    wm->osk_start.w = g.card.w;
    wm->osk_start.kh = g.kh;
    wm->osk_pl = wm->osk_start;
    wm->osk_drag = kind;
    wm->osk_drag_edges = edges;
    double px, py;
    lm_seat_pointer_position(wm->srv->seat, &px, &py);
    wm->osk_press_x = (int)px;
    wm->osk_press_y = (int)py;
}

void osk_drag_motion(struct lm_wm *wm)
{
    if (!wm->osk_drag || !wm->srv->seat || !wm->osk_open)
        return;
    double px, py;
    lm_seat_pointer_position(wm->srv->seat, &px, &py);
    int dx = (int)px - wm->osk_press_x, dy = (int)py - wm->osk_press_y;
    struct lm_osk_place p = wm->osk_start;
    if (wm->osk_drag == 1) {
        p.x += dx;
        p.y += dy;
    } else {
        if (wm->osk_drag_edges & LM_EDGE_RIGHT)
            p.w += dx;
        if (wm->osk_drag_edges & LM_EDGE_BOTTOM)
            p.kh = lm_osk_kh_for_h(lm_osk_card_h(wm->osk_start.kh) + dy);
    }
    osk_damage(wm);                                             /* the old place ... */
    wm->osk_pl = p;
    struct lm_osk_geo g;
    if (osk_geo(wm, &g)) {                                      /* ... the new one, kept inside the output and within the size limits */
        wm->osk_pl.x = g.card.x;
        wm->osk_pl.y = g.card.y;
        wm->osk_pl.w = g.card.w;
        wm->osk_pl.kh = g.kh;
    }
    osk_damage(wm);
}

void lm_wm_osk_reset(struct lm_wm *wm)
{
    osk_damage(wm);
    memset(&wm->osk_pl, 0, sizeof(wm->osk_pl));
    wm->osk_drag = 0;
    osk_damage(wm);
    gadgets_save(wm);
    lm_wm_osk_layout_changed(wm);
}

void lm_wm_osk_set(struct lm_wm *wm, bool open) { osk_set(wm, open); }
bool lm_wm_osk_is_open(const struct lm_wm *wm) { return wm->osk_open; }

void lm_wm_tray_changed(struct lm_wm *wm)
{
    damage_panels(wm);
}

size_t lm_wm_tray_describe(struct lm_wm *wm, char *buf, size_t size)
{
    struct lm_output *o = lm_output_primary(wm->srv);
    struct lm_panel_info pi;
    size_t n = 0;
    if (!o || size == 0) {
        if (size)
            buf[0] = 0;
        return 0;
    }
    panel_info(wm, o, &pi);
    n += (size_t)snprintf(buf + n, size - n, "tray n=%d\n", pi.ntray);
    for (int i = 0; i < pi.ntray && n < size; i++) {
        char ic[24] = "-";
        if (pi.tray[i].icon)
            snprintf(ic, sizeof(ic), "%dx%d", pi.tray[i].icon->width, pi.tray[i].icon->height);
        n += (size_t)snprintf(buf + n, size - n, "item %d x=%d y=%d w=%d h=%d icon=%s text=%s\n", pi.tray[i].id, pi.tray[i].rect.x,
                              pi.tray[i].rect.y, pi.tray[i].rect.w, pi.tray[i].rect.h, ic, pi.tray[i].text);
    }
    return n < size ? n : size - 1;
}

size_t lm_wm_clock_describe(struct lm_wm *wm, char *buf, size_t size)
{
    struct lm_output *o = lm_output_primary(wm->srv);
    if (!o || size == 0) {
        if (size)
            buf[0] = 0;
        return 0;
    }
    struct lm_panel_info pi;
    panel_info(wm, o, &pi);
    struct lm_window *cw = calendar_window(wm);
    size_t n = (size_t)snprintf(buf, size, "clock x=%d y=%d w=%d h=%d tasks=%d calendar=%d text=%s\n", pi.clock.x, pi.clock.y, pi.clock.w, pi.clock.h,
                                pi.ntasks, !cw ? 0 : win_visible(wm, cw) ? 2 : 1, pi.clock_text);
    return n < size ? n : size - 1;
}

size_t lm_wm_osk_describe(struct lm_wm *wm, char *buf, size_t size)
{
    struct lm_osk_geo g;
    bool open = osk_geo(wm, &g);
    size_t n = (size_t)snprintf(buf, size, "osk open=%d latched=%u n=%d down=%d\n", open, wm->osk_latched, open ? g.n : 0, wm->osk_down);
    if (!open)
        return n;
    static struct lm_osk_info oi;
    osk_info(wm, &g, &oi);
    n += (size_t)snprintf(buf + n, size - n, "card x=%d y=%d w=%d h=%d\n", g.card.x, g.card.y, g.card.w, g.card.h);
    for (int i = 0; i < g.n && n < size; i++)
        n += (size_t)snprintf(buf + n, size - n, "key %d code=%u kind=%d x=%d y=%d w=%d h=%d on=%d held=%d label=%s\n", i, g.key[i].code,
                              (int)g.key[i].kind, g.key[i].rect.x, g.key[i].rect.y, g.key[i].rect.w, g.key[i].rect.h,
                              (oi.key[i].state & LM_OSK_KEY_ON) != 0, (oi.key[i].state & LM_OSK_KEY_DOWN) != 0, oi.key[i].label);
    return n;
}
