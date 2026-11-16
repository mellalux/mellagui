/* Part of the window manager (see wm_priv.h): mouse drags: dock icons (reorder, take out) and launcher icons (onto the dock), the dock */
#include "wm_priv.h"

/* ---- the dock ------------------------------------------------------------------------------------------------------------ */

#define DOCK_DOT 8                  /* room under the icons for the "running" dots */
#define DOCK_PAD 12                 /* the strip reaches this far beyond the first / last icon */
#define DOCK_LABEL_H 22
#define DOCK_HIDE_MS 600            /* autohide: the dock goes away this long after the pointer left it */
#define DOCK_BOUNCE_PERIOD_MS 900   /* one swell (zoom in, zoom out) of an icon whose program is starting */
#define DOCK_PULSE 0.35f            /* ... up to this much bigger */
#define DOCK_ANIM_MS 33             /* ... redrawn this often while any of them swells */
#define DOCK_LAUNCH_MIN_MS 2000     /* the swelling lasts at least this long, even when the window comes sooner */
#define DOCK_MELT_MS 3200           /* autohide: the buttons melt into the edge of the screen this slowly ... */
#define DOCK_RISE_MS 1100           /* ... and rise out of it, vague at first, this fast (both with window animations on; off = at once) */

/* The dock lives on the primary output; NULL when there is none or it is off / empty / hidden by a fullscreen window. */
struct lm_output *dock_output(const struct lm_wm *wm)
{
    if (!wm->srv->backend || !lm_dock.enabled || lm_dock_count() == 0)
        return NULL;
    struct lm_output *o = lm_output_primary(wm->srv);
    return o && !fullscreen_covers(wm, o) ? o : NULL;
}

bool dock_shown(const struct lm_wm *wm)
{
    return !lm_dock.autohide || wm->dock_shown || wm->dock_vis > 0.001f;       /* (while it melts away it is still there) */
}

void dock_geometry(const struct lm_output *o, struct dock_geo *g)
{
    g->n = lm_dock_count();
    g->size = lm_dock.icon_size;
    g->zsize = g->size * lm_dock.zoom / 100;
    g->gap = lm_dock.gap;
    int total = g->n * g->size + (g->n - 1) * g->gap;
    g->x0 = o->x + (o->width - total) / 2;
    int out_bottom = o->y + o->height;
    g->icon_bottom = out_bottom - lm_dock.margin - DOCK_DOT;
    int top = g->icon_bottom - g->size - 8, ttop = g->icon_bottom - g->zsize - 8;
    g->strip = lm_rect_make(g->x0 - DOCK_PAD, top, total + 2 * DOCK_PAD, out_bottom - top);
    g->tall = lm_rect_make(g->x0 - DOCK_PAD, ttop, total + 2 * DOCK_PAD, out_bottom - ttop);
    int pad = (g->zsize - g->size) * 3 + DOCK_PAD + 4;          /* the neighbours of the magnified icon move out of the way */
    int btop = ttop - DOCK_LABEL_H - 10 - g->size / 2;          /* (and an icon that swells grows up to a third of its size higher) */
    struct lm_rect b = lm_rect_make(g->x0 - pad, btop, total + 2 * pad, out_bottom - btop), orect = lm_output_rect(o);
    if (!lm_rect_intersect(&b, &orect, &g->bounds))
        g->bounds = (struct lm_rect){ 0 };
}

/* Everything the icon in the hand of a dock drag may touch: a square around the pointer (icon, shadow, the x). */
static struct lm_rect dock_ghost_rect(const struct lm_wm *wm)
{
    int half = lm_dock.icon_size * lm_dock.zoom / 100 * 4 / 5 + 12;
    return lm_rect_make((int)wm->drag_x - half, (int)wm->drag_y - half, 2 * half, 2 * half);
}

bool dock_app_matches(const char *match, const char *app_id)
{
    if (!*match)
        return false;
    if (strchr(match, '*')) {
        /* a pattern ("*-synthai-ee__-*", see app_mode_key in dock.c): "*" stands for any run of characters; the app id is
         * compared in the same normal form (lower case, dots -> dashes) */
        char norm[256];
        size_t n = 0;
        for (; app_id[n]; n++) {
            if (n + 1 >= sizeof(norm))
                return false;
            norm[n] = app_id[n] == '.' ? '-' : (char)tolower((unsigned char)app_id[n]);
        }
        norm[n] = 0;
        const char *p = match, *s = norm, *star = NULL, *back = NULL;
        while (*s) {
            if (*p == '*') {
                star = p++;
                back = s;
            } else if (*p == *s) {
                p++;
                s++;
            } else if (star) {
                p = star + 1;
                s = ++back;
            } else {
                return false;
            }
        }
        while (*p == '*')
            p++;
        return !*p;
    }
    size_t i = 0;
    for (; match[i] && app_id[i]; i++) {
        char c = app_id[i] == '.' ? '-' : (char)tolower((unsigned char)app_id[i]);
        if (c != match[i])
            return false;
    }
    return !match[i] && !app_id[i];
}

/* How many mapped windows belong to the button. */
int dock_window_count(const struct lm_wm *wm, const struct lm_dock_item *it)
{
    int n = 0;
    struct lm_window *w;
    lm_list_for_each(w, &((struct lm_wm *)wm)->windows, link)
        if (w->mapped && dock_app_matches(it->match, w->app_id))
            n++;
    return n;
}

/* The pose of an icon while the autohide dock melts away or rises out of the screen edge. `p` = how far gone it is (0 there .. 1 gone), `rising`
 * = it is coming back. Each icon follows its neighbour a little later (a wave along the row); it flattens, spreads, sinks into the edge of the screen
 * and fades. Rising is the same movement backwards, and the icon is a vague ghost that sharpens as it reaches its place. */
static void dock_pose(struct lm_dock_info *d, int slot, float p, bool rising, int pos, int n, int out_bottom)
{
    const float stagger = 0.35f;
    float frac = n > 1 ? (float)pos / (float)(n - 1) : 0.0f;
    float delay = stagger * (rising ? 1.0f - frac : frac);
    float q = LM_CLAMP((p - delay) / (1.0f - stagger), 0.0f, 1.0f);
    float e = q * q * (3.0f - 2.0f * q);
    struct lm_rect *b = &d->icon[slot].box;
    float cx = (float)b->x + (float)b->w / 2.0f, bottom = (float)(b->y + b->h);
    float nw = (float)b->w * (1.0f + 0.35f * e), nh = fmaxf((float)b->h * (1.0f - e), 1.0f), nb = bottom + e * ((float)out_bottom - bottom);
    *b = lm_rect_make((int)lroundf(cx - nw / 2.0f), (int)lroundf(nb - nh), (int)lroundf(nw), (int)lroundf(nh));
    d->icon[slot].alpha = e >= 0.995f ? 0 : (uint8_t)lroundf(255.0f * powf(1.0f - e, 0.8f));
    d->icon[slot].blur = rising ? e : 0.0f;
}

void dock_info(const struct lm_wm *wm, struct lm_output *o, struct lm_dock_info *d)
{
    struct dock_geo g;
    dock_geometry(o, &g);
    memset(d, 0, sizeof(*d));
    d->output = o;
    d->rect = g.bounds;
    d->n = g.n;
    bool drag = wm->dock_drag && wm->dock_drag_idx >= 0 && wm->dock_drag_idx < g.n;
    d->hover = wm->dock_active && !drag ? wm->hover_dock : -1;
    d->dot_y = g.icon_bottom + DOCK_DOT / 2 + 1;
    int order[LM_DOCK_MAX];                                       /* what each place of the row holds; -1 = the gap of the icon in the hand */
    for (int i = 0; i < g.n; i++)
        order[i] = i;
    if (drag) {
        int idx = wm->dock_drag_idx, to = wm->dock_drag_to, k = 0;
        if (to < 0) {
            order[idx] = -1;                                      /* it is out of the row: the others stay where they are */
        } else {
            int rest[LM_DOCK_MAX];
            for (int i = 0; i < g.n; i++)
                if (i != idx)
                    rest[k++] = i;
            to = LM_CLAMP(to, 0, g.n - 1);
            for (int i = 0, r = 0; i < g.n; i++)
                order[i] = i == to ? -1 : rest[r++];
        }
    }

    /* The icons near the pointer grow with a bell curve; the ones beyond them are pushed aside so that nothing overlaps. Hit testing uses
     * the unmagnified places (dock_pick), so what the pointer measures against does not move under it. */
    double px = 0, py = 0;
    bool zoom = wm->dock_active && !drag && lm_dock.zoom > 100 && wm->srv->seat;
    if (zoom)
        lm_seat_pointer_position(wm->srv->seat, &px, &py);
    float scale[LM_DOCK_MAX], extra[LM_DOCK_MAX], total_extra = 0.0f;
    float sigma = 2.3f * (float)(g.size + g.gap);
    for (int i = 0; i < g.n; i++) {
        float cx = (float)(g.x0 + i * (g.size + g.gap)) + (float)g.size / 2.0f, f = 0.0f;
        if (zoom) {
            float dd = fabsf((float)px - cx);
            f = dd < sigma ? 0.5f * (1.0f + cosf(3.14159265f * dd / sigma)) : 0.0f;
        }
        scale[i] = 1.0f + ((float)lm_dock.zoom / 100.0f - 1.0f) * f;
        extra[i] = (float)g.size * (scale[i] - 1.0f);
        total_extra += extra[i];
    }
    float left = 0.0f;
    int vis = 0;
    for (int i = 0; i < g.n; i++) {
        float cx = (float)(g.x0 + i * (g.size + g.gap)) + (float)g.size / 2.0f;
        float cxp = cx + left + extra[i] / 2.0f - total_extra / 2.0f, w = (float)g.size * scale[i];
        left += extra[i];
        if (order[i] < 0)
            continue;
        int it = order[i];
        d->icon[vis].box = lm_rect_make((int)lroundf(cxp - w / 2.0f), (int)lroundf((float)g.icon_bottom - w), (int)lroundf(w), (int)lroundf(w));
        d->icon[vis].item = lm_dock_item(it);
        d->icon[vis].running = dock_window_count(wm, d->icon[vis].item) > 0;
        d->icon[vis].alpha = 255;
        if (wm->dock_launch_ms[it]) {                                 /* starting: swells and shrinks again until its window is there */
            float ph = (float)((lm_now_ms() - (uint64_t)wm->dock_launch_ms[it]) % DOCK_BOUNCE_PERIOD_MS) / (float)DOCK_BOUNCE_PERIOD_MS;
            float k = 1.0f + DOCK_PULSE * (0.5f - 0.5f * cosf(6.2831853f * ph));      /* 1 -> 1 + DOCK_PULSE -> 1, smooth at both ends */
            float nw = w * k;                                         /* grows from the middle of its foot: the bottom stays where it is */
            d->icon[vis].box = lm_rect_make((int)lroundf(cxp - nw / 2.0f), (int)lroundf((float)g.icon_bottom - nw), (int)lroundf(nw), (int)lroundf(nw));
            d->icon[vis].launching = true;
        }
        if (lm_dock.autohide && wm->dock_vis < 0.999f)
            dock_pose(d, vis, 1.0f - wm->dock_vis, wm->dock_shown, i, g.n, o->y + o->height);
        vis++;
    }
    d->n = vis;
    if (drag) {                                                       /* the icon in the hand follows the pointer */
        d->drag_item = lm_dock_item(wm->dock_drag_idx);
        d->drag_box = lm_rect_make((int)wm->drag_x - g.zsize / 2, (int)wm->drag_y - g.zsize / 2, g.zsize, g.zsize);
        d->drag_remove = wm->dock_drag_to < 0 && !dock_special(d->drag_item);
        struct lm_rect gr = dock_ghost_rect(wm);
        d->rect = lm_rect_union(&d->rect, &gr);
    }
    if (d->hover >= 0 && d->hover < g.n) {
        d->label_text = lm_dock_item(d->hover)->name;
        int w = lm_font_text_width(d->label_text) + 20;
        const struct lm_rect *b = &d->icon[d->hover].box;
        int x = b->x + b->w / 2 - w / 2, y = b->y - 8 - DOCK_LABEL_H;
        struct lm_rect orect = lm_output_rect(o);
        x = LM_CLAMP(x, orect.x + 4, orect.x + orect.w - w - 4);
        d->label = lm_rect_make(x, y, w, DOCK_LABEL_H);
    }
}

void dock_damage(struct lm_wm *wm)
{
    struct lm_output *o = dock_output(wm);
    if (!o)
        return;
    struct dock_geo g;
    dock_geometry(o, &g);
    damage_rect(wm, &g.bounds);
    if (wm->dock_drag) {
        struct lm_rect gr = dock_ghost_rect(wm);
        damage_rect(wm, &gr);
    }
}

/* What the pointer at (ix, iy) touches of the dock. The magnified shape is only "solid" while the pointer is inside the dock already. */
bool dock_pick(const struct lm_wm *wm, int ix, int iy, struct lm_pick *out)
{
    struct lm_output *o = dock_output(wm);
    if (!o || lm_lock_active(wm->srv->lock))
        return false;
    struct dock_geo g;
    dock_geometry(o, &g);
    if (dock_shown(wm)) {
        const struct lm_rect *r = wm->dock_active ? &g.tall : &g.strip;
        if (lm_rect_contains(r, ix, iy)) {
            out->kind = LM_PICK_DOCK;
            out->rect = *r;
            int rel = ix - g.x0 + g.gap / 2, pitch = g.size + g.gap;
            if (rel >= 0 && rel < g.n * pitch) {
                out->kind = LM_PICK_DOCK_ITEM;
                out->index = rel / pitch;
            }
            return true;
        }
    }
    if (lm_dock.autohide && !dock_shown(wm) && iy >= o->y + o->height - 2 && ix >= o->x && ix < o->x + o->width) {
        out->kind = LM_PICK_DOCK;                               /* the bottom edge of the screen reveals it */
        return true;
    }
    return false;
}

static void dock_set_shown(struct lm_wm *wm, bool shown);

void dock_hide_tick(void *data)
{
    struct lm_wm *wm = data;
    if (wm->dock_shown && !wm->dock_active && !wm->launcher_drag) {      /* (an icon carried to it keeps it there) */
        dock_damage(wm);
        dock_set_shown(wm, false);
    }
}

/* The pointer moved: `in` = it is on the dock, `item` = the icon it is on (-1 none). */
void dock_motion(struct lm_wm *wm, bool in, int item)
{
    double px = 0, py = 0;
    if (wm->srv->seat)
        lm_seat_pointer_position(wm->srv->seat, &px, &py);
    bool changed = in != wm->dock_active || item != wm->hover_dock;
    if (in && (int)px != wm->dock_px)
        changed = true;
    wm->dock_px = (int)px;
    if (in && lm_dock.autohide) {
        if (wm->dock_timer)
            lm_timer_arm_ms(wm->dock_timer, 0, 0);
        if (!wm->dock_shown) {
            dock_set_shown(wm, true);
            changed = true;
        }
    } else if (!in && wm->dock_shown && lm_dock.autohide && wm->dock_timer && !lm_timer_is_armed(wm->dock_timer)) {
        lm_timer_arm_ms(wm->dock_timer, DOCK_HIDE_MS, 0);
    }
    if (in && !wm->dock_active && wm->srv->config.sound && strcmp(wm->srv->config.backend, "headless") != 0)
        lm_sound_wake();                                        /* the pointer came to the dock: wake the sink before the click's sound needs it */
    wm->dock_active = in;
    wm->hover_dock = item;
    if (changed)
        dock_damage(wm);
}

/* A click on icon `i`: bring the program's window forward when it has one that is not in front already (`reuse`), else start it. */
void dock_activate(struct lm_wm *wm, int i, bool reuse)
{
    const struct lm_dock_item *it = lm_dock_item(i);
    if (it)
        dock_launch(wm, it, i, reuse);
}

/* Starts the program of `it` or brings its window forward. `i` is its place in the dock (the icon swells there while it starts), -1 for a
 * launcher button that is not in the dock. */
/* The settings panel is there once: when it is open (or was started a moment ago and is still coming up) it is brought forward and no second one
 * starts. True when that happened. */
bool settings_present(struct lm_wm *wm)
{
    struct lm_window *w;
    lm_list_for_each(w, &wm->windows, link)
        if (w->mapped && w->app_id && !strcmp(w->app_id, "mellagui.settings")) {
            lm_wm_focus_window(wm, w);
            return true;
        }
    return wm->settings_spawn_ms && (int64_t)lm_now_ms() - wm->settings_spawn_ms < 4000;
}

void dock_launch(struct lm_wm *wm, const struct lm_dock_item *it, int i, bool reuse)
{
    bool is_settings = !dock_special(it) && dock_app_matches(it->match, "mellagui.settings");
    if (is_settings && settings_present(wm))
        return;
    if (is_settings)
        wm->settings_spawn_ms = (int64_t)lm_now_ms();
    if (launcher_is_self(it)) {                                       /* the Launcher button: not a program */
        launcher_set(wm, true, dock_output(wm));
        return;
    }
    if (dock_special(it)) {                                           /* the Overview button: opens it, a second click closes it (like Super+W) */
        overview_set(wm, !wm->ov.open || wm->ov.closing);
        return;
    }
    struct lm_window *w, *best = NULL;
    if (reuse)
        lm_list_for_each(w, &wm->windows, link)
            if (w->mapped && dock_app_matches(it->match, w->app_id) && (!best || WIN(w)->focus_seq > WIN(best)->focus_seq))
                best = w;
    /* A window that is not in front yet comes forward; when the most recent one already has the focus the click is for another
     * instance (two terminals): the program is started again. */
    if (best && !(best == wm->focused && !best->minimized && best->workspace == wm->workspace)) {
        lm_wm_focus_window(wm, best);
    } else if (lm_server_spawn(wm->srv, it->cmd) > 0 && i >= 0 && i < LM_DOCK_MAX) {
        wm->dock_launch_ms[i] = (int64_t)lm_now_ms();                 /* the icon swells until a window of the program shows up */
        wm->dock_launch_base[i] = dock_window_count(wm, it);
        if (wm->dock_anim)
            lm_timer_arm_ms(wm->dock_anim, DOCK_ANIM_MS, DOCK_ANIM_MS);
        dock_damage(wm);
    }
}

/* ---- mouse drags: dock icons (reorder, take out) and launcher icons (onto the dock) ---------------------------------------------- */

#define DRAG_START_PX 8             /* the pointer must move this far with the button down before a press becomes a drag */
#define DOCK_OUT_PX 60              /* a dock icon dragged this far above the row leaves the dock */

bool drag_far_enough(const struct lm_wm *wm, double x, double y)
{
    double dx = x - wm->drag_press_x, dy = y - wm->drag_press_y;
    return dx * dx + dy * dy >= (double)(DRAG_START_PX * DRAG_START_PX);
}

static void dock_drag_notify_failed(struct lm_wm *wm)
{
    lm_wm_notify(wm, "Dock", "Could not change the dock: there is no config file to write, or the list is full", 5000);
}

void dock_drag_finish(struct lm_wm *wm)
{
    int idx = wm->dock_drag_idx, to = wm->dock_drag_to;
    dock_damage(wm);
    wm->dock_drag = false;
    dock_damage(wm);
    const struct lm_dock_item *it = lm_dock_item(idx);
    if (!it)
        return;
    if (to < 0) {                                                   /* out of the dock: it is gone from there, the launcher keeps it */
        if (!dock_special(it) && !lm_settings_dock_to_launcher(wm->srv, it->raw))
            dock_drag_notify_failed(wm);
    } else if (to != idx) {
        const struct lm_dock_item *target = lm_dock_item(to);
        if (target && !lm_settings_dock_reorder(wm->srv, it->raw, target->raw))
            dock_drag_notify_failed(wm);
    }
}

/* The pointer moved while a dock icon is pressed: start the drag once it has moved enough, then follow. */
void dock_drag_motion(struct lm_wm *wm)
{
    if (wm->press_panel_kind != LM_PICK_DOCK_ITEM || !wm->dock_press_left || !wm->srv->seat)
        return;
    double x, y;
    lm_seat_pointer_position(wm->srv->seat, &x, &y);
    struct lm_output *o = dock_output(wm);
    if (!o)
        return;
    struct dock_geo g;
    dock_geometry(o, &g);
    if (!wm->dock_drag) {
        if (!drag_far_enough(wm, x, y) || wm->press_panel_ws < 0 || wm->press_panel_ws >= g.n)
            return;
        wm->dock_drag = true;
        wm->dock_drag_idx = wm->press_panel_ws;
        wm->dock_active = false;
        wm->hover_dock = -1;
    }
    dock_damage(wm);                                                /* (where the icon in the hand was) */
    wm->drag_x = x;
    wm->drag_y = y;
    if (y < g.icon_bottom - g.size - DOCK_OUT_PX)
        wm->dock_drag_to = -1;
    else
        wm->dock_drag_to = LM_CLAMP((int)floor((x - g.x0) / (double)(g.size + g.gap)), 0, g.n - 1);
    dock_damage(wm);
}

/* Where a launcher icon can be dropped: the dock's place (the row and a margin around it). *pos = the place it would take, -1 when the
 * pointer is not on it. False when the dock is off. */
bool launcher_drop_zone(const struct lm_wm *wm, struct lm_output *o, struct lm_rect *zone, struct lm_rect *marker, int *pos)
{
    *pos = -1;
    if (!lm_dock.enabled || lm_dock_count() == 0)
        return false;
    struct dock_geo g;
    dock_geometry(o, &g);
    struct lm_rect z = lm_rect_make(g.strip.x - 60, g.strip.y - 40, g.strip.w + 120, o->y + o->height - (g.strip.y - 40)), orect = lm_output_rect(o);
    if (!lm_rect_intersect(&z, &orect, zone))
        return false;
    if (lm_rect_contains(zone, (int)wm->drag_x, (int)wm->drag_y)) {
        int pitch = g.size + g.gap;
        *pos = LM_CLAMP((int)floor((wm->drag_x - g.x0 + pitch / 2.0) / (double)pitch), 0, g.n);
        *marker = lm_rect_make(g.x0 + *pos * pitch - g.gap / 2 - 1, g.icon_bottom - g.size, 3, g.size);
    }
    return true;
}

#define FLIP_DWELL_MS 500                                   /* a carried icon waits this long at the edge before the page turns ... */
#define FLIP_AGAIN_MS 1500                                  /* ... and this long, still there, before it turns the next one (leaving the zone starts over) */

#define SWIPE_PX 70                                                     /* how far a press on the card moves sideways before the page turns */

/* A press on the card that is not on an icon, moved sideways with the button down, turns the pages: to the left for the next one, to the right for the one before;
 * going on another SWIPE_PX turns the next. */
void launcher_swipe_motion(struct lm_wm *wm)
{
    if (!wm->launcher_swipe || !wm->srv->seat || wm->launcher_form)
        return;
    double x, y;
    lm_seat_pointer_position(wm->srv->seat, &x, &y);
    int dx = (int)x - wm->launcher_swipe_x;
    if (dx > -SWIPE_PX && dx < SWIPE_PX)
        return;
    struct lm_output *o = launcher_output(wm);
    if (!o)
        return;
    struct lm_launcher_info li;
    launcher_info(wm, o, &li);
    int dir = dx < 0 ? 1 : -1;
    if (li.page + dir >= 0 && li.page + dir < li.pages) {
        wm->launcher_page = li.page + dir;
        wm->launcher_sel = -1;
        launcher_damage(wm);
    }
    wm->launcher_swipe_x = (int)x;
}

void launcher_edge_tick(void *data)
{
    struct lm_wm *wm = data;
    if (!wm->launcher_drag || !wm->launcher_edge) {                     /* nothing carried, or it left the edge: stop looking */
        if (wm->launcher_edge_timer)
            lm_timer_arm_ms(wm->launcher_edge_timer, 0, 0);
        return;
    }
    launcher_drag_motion(wm);
}

void launcher_drag_motion(struct lm_wm *wm)
{
    if (wm->press_panel_kind != LM_PICK_LAUNCHER_ITEM || !wm->srv->seat || wm->launcher_form || launcher_searching_now())
        return;
    double x, y;
    lm_seat_pointer_position(wm->srv->seat, &x, &y);
    if (!wm->launcher_drag) {
        if (!drag_far_enough(wm, x, y))
            return;
        const struct lm_dock_item *dragged = launcher_item(wm->press_panel_ws, NULL);
        if (!dragged || dragged->spacer || lm_search_owns(dragged))                    /* what the search found cannot be moved into the dock */
            return;
        wm->launcher_drag = true;
        wm->launcher_drag_g = wm->press_panel_ws;
        wm->launcher_edge = 0;
        wm->launcher_edge_flipped = false;
    }
    if (lm_dock.autohide && wm->dock_timer) {                           /* an icon on its way to the dock: a hidden dock comes up and stays while it is carried */
        lm_timer_arm_ms(wm->dock_timer, 0, 0);
        if (!wm->dock_shown)
            dock_set_shown(wm, true);
    }
    wm->drag_x = x;
    wm->drag_y = y;
    /* An icon held near the left / right edge of the card turns the page - once it has stayed there for FLIP_DWELL_MS, and then only once per visit: the pointer has
     * to leave the zone and come back (or stay FLIP_AGAIN_MS) before the next page turns, or a hand that rests at the edge runs over every page, including the empty
     * one at the end. */
    struct lm_output *o = launcher_output(wm);
    int64_t now = (int64_t)lm_now_ms();
    if (o) {
        struct lm_launcher_info li;
        launcher_info(wm, o, &li);
        int dir = x < li.card.x + 36 ? -1 : x > li.card.x + li.card.w - 36 ? 1 : 0;
        if (dir != wm->launcher_edge) {                                 /* entered (or left) a zone: the clock starts again */
            wm->launcher_edge = dir;
            wm->launcher_edge_ms = now;
            wm->launcher_edge_flipped = false;
        } else if (dir && li.pages > 1 && li.page + dir >= 0 && li.page + dir < li.pages) {
            if (now - wm->launcher_edge_ms >= (wm->launcher_edge_flipped ? FLIP_AGAIN_MS : FLIP_DWELL_MS)) {
                wm->launcher_page = li.page + dir;
                wm->launcher_flip_ms = now;
                wm->launcher_edge_ms = now;
                wm->launcher_edge_flipped = true;
            }
        }
        if (dir && wm->launcher_edge_timer && !lm_timer_is_armed(wm->launcher_edge_timer))      /* (a hand that rests still sends no motion) */
            lm_timer_arm_ms(wm->launcher_edge_timer, 100, 100);
    }
    launcher_damage(wm);
}


/* An icon of the grid was let go over cell `t` (any cell of the page shown - an icon's, an empty one, one of the empty page of edit mode): that is where it stays.
 * The grid is a grid, there is no first or last place. Over an empty cell the two just swap (its old cell is empty now, nothing else moves); over an icon the cells
 * between make room by one (as the preview showed); beyond the end of the list the cells before it are padded with empty cells (`-` in `[launcher] order`, `full_pp`
 * cells a page). Pages that hold only empty cells, and empty cells at the end, are dropped again. Returns the cell it ended in, -1 when nothing was changed. */
static const struct lm_dock_item k_gap = { .name = "", .spacer = true };      /* an empty cell of the order */

static int launcher_reorder(struct lm_wm *wm, int g, int t, int full_pp)
{
    int n = launcher_count();
    const struct lm_dock_item *it = launcher_item(g, NULL);
    if (!it || it->spacer || lm_search_owns(it) || g < 0 || g >= n || n > LM_LAUNCHER_MAX || t < 0 || t == g || t >= LM_LAUNCHER_MAX - 1)
        return -1;
    const struct lm_dock_item *list[LM_LAUNCHER_MAX] = { NULL };
    for (int i = 0; i < n; i++)
        list[i] = launcher_item(i, NULL);
    const struct lm_dock_item *moved = list[g];
    if (t >= n) {                                                         /* an empty cell beyond the list */
        if (t - n > LM_LAUNCHER_GAPS_MAX)
            return -1;
        list[g] = &k_gap;
        for (int i = n; i < t; i++)
            list[i] = &k_gap;
        list[t] = moved;
        n = t + 1;
    } else if (list[t]->spacer) {                                         /* an empty cell inside the list: swap */
        list[t] = moved;
        list[g] = &k_gap;
    } else {                                                              /* an icon: the ones between make room */
        if (t > g)
            memmove(&list[g], &list[g + 1], (size_t)(t - g) * sizeof(list[0]));
        else
            memmove(&list[t + 1], &list[t], (size_t)(g - t) * sizeof(list[0]));
        list[t] = moved;
    }
    int pp = LM_MAX(full_pp, 1);                                          /* a page of nothing but empty cells goes, and so do trailing ones */
    for (int p = 0; p * pp < n;) {
        int end = LM_MIN((p + 1) * pp, n), all = 1;
        for (int i = p * pp; i < end; i++)
            if (!list[i]->spacer)
                all = 0;
        if (all && end - p * pp > 0) {
            memmove(&list[p * pp], &list[end], (size_t)(n - end) * sizeof(list[0]));
            n -= end - p * pp;
            if (t >= end)
                t -= end - p * pp;
        } else {
            p++;
        }
    }
    while (n > 0 && list[n - 1]->spacer)
        n--;
    char order[2000];
    size_t o = 0;
    for (int i = 0; i < n; i++) {
        const char *name = list[i]->spacer ? "-" : list[i]->name;
        if (strstr(name, " | ") || o + strlen(name) + 4 >= sizeof(order)) {
            lm_wm_notify(wm, "Launcher", "Could not move it: the names are too long to keep the order", 4000);
            return -1;
        }
        o += (size_t)snprintf(order + o, sizeof(order) - o, "%s%s", i ? " | " : "", name);
    }
    if (!lm_settings_launcher_set_order(wm->srv, order)) {
        lm_wm_notify(wm, "Launcher", "Could not move it: there is no config file", 4000);
        return -1;
    }
    return t;
}

void launcher_drag_finish(struct lm_wm *wm)
{
    struct lm_output *o = launcher_output(wm);
    struct lm_launcher_info *li = o ? malloc(sizeof(*li)) : NULL;
    if (li)
        launcher_info(wm, o, li);                                   /* (where the icon is: over the dock, over a cell, over the empty page) */
    int g = wm->launcher_drag_g;
    wm->launcher_drag = false;
    if (lm_dock.autohide && wm->dock_shown && wm->dock_timer && !wm->dock_active)    /* let go: it goes away again after its usual wait */
        lm_timer_arm_ms(wm->dock_timer, DOCK_HIDE_MS, 0);
    launcher_damage(wm);
    const struct lm_dock_item *it = launcher_item(g, NULL);
    if (it && li && li->drop_pos >= 0) {
        if (it->launcher_only) {                                    /* (a button that is in the dock already stays where it is) */
            const struct lm_dock_item *before = li->drop_pos < lm_dock_count() ? lm_dock_item(li->drop_pos) : NULL;
            if (!lm_settings_launcher_to_dock(wm->srv, it->raw, before ? before->raw : -1))
                lm_wm_notify(wm, "Launcher", "Could not add it to the dock: it is full, the name is in use, or there is no config file", 5000);
        }
    } else if (it && li && li->drop_cell >= 0) {
        int at = launcher_reorder(wm, g, li->drop_cell, li->full_pp);
        if (at >= 0) {                                              /* the page it landed on shows */
            bool gaps = launcher_gap_count() > 0;                    /* (the order was just rewritten and the config reloaded) */
            int pp = gaps || wm->launcher_edit ? li->full_pp : li->per_page;
            if (pp > 0)
                wm->launcher_page = at / pp;
        }
    }
    free(li);
}

/* The autohide dock is to be shown / hidden: with animations on it rises or melts over a few seconds (dock_vis follows, dock_anim_tick steps it), else at once. */
static void dock_set_shown(struct lm_wm *wm, bool shown)
{
    wm->dock_shown = shown;
    if (anim_duration(wm) <= 0) {
        wm->dock_vis = shown ? 1.0f : 0.0f;
        dock_damage(wm);
        return;
    }
    wm->dock_vis_ms = (int64_t)lm_now_ms();
    if (wm->dock_anim)
        lm_timer_arm_ms(wm->dock_anim, DOCK_ANIM_MS, DOCK_ANIM_MS);
}

/* Every 33 ms while some icon swells or the dock melts / rises: ends the swelling of the ones whose window came (after at least DOCK_LAUNCH_MIN_MS,
 * or that never made one), moves the autohide animation on and repaints. */
void dock_anim_tick(void *data)
{
    struct lm_wm *wm = data;
    const char *e = getenv("MELLAGUI_DOCK_BOUNCE_MS");               /* how long to swell at most; tests make it short */
    int64_t limit = e && atol(e) > 0 ? atol(e) : 12000;
    int64_t now = (int64_t)lm_now_ms();
    bool any = false;
    for (int i = 0; i < LM_DOCK_MAX; i++) {
        if (!wm->dock_launch_ms[i])
            continue;
        const struct lm_dock_item *it = lm_dock_item(i);
        int64_t age = now - wm->dock_launch_ms[i];
        bool came = it && dock_window_count(wm, it) > wm->dock_launch_base[i];
        if (!it || (came && age >= DOCK_LAUNCH_MIN_MS) || age > limit)
            wm->dock_launch_ms[i] = 0;
        else
            any = true;
    }
    float target = wm->dock_shown ? 1.0f : 0.0f;
    if (lm_dock.autohide && wm->dock_vis != target) {
        float dur = (float)(wm->dock_shown ? DOCK_RISE_MS : DOCK_MELT_MS);
        float step = (float)(now - wm->dock_vis_ms) / dur;
        wm->dock_vis = target > wm->dock_vis ? fminf(wm->dock_vis + step, 1.0f) : fmaxf(wm->dock_vis - step, 0.0f);
        any = any || wm->dock_vis != target;
    }
    wm->dock_vis_ms = now;
    dock_damage(wm);
    if (!any && wm->dock_anim)
        lm_timer_arm_ms(wm->dock_anim, 0, 0);
}

