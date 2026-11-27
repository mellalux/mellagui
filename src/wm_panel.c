/*
 * wm_panel.c - the order of the top panel's right-hand buttons, and carrying one to another place: a button that is pressed and held
 * (LONG_PRESS_MS) comes loose and follows the pointer, the others make room, and the order it is let go in is kept (setting panel-order:
 * the buttons' names from left to right). One that is let go without having been carried anywhere is clicked as usual. The logo, the workspaces and the taskbar stay where they are.
 *
 * Names: caps quick layout sticky osk magnifier clock power, and for a program's item (tray.h) the name it gave (battery network bluetooth bell record
 * timer), "tray" without one. A button the saved order does not name takes its built-in place, an unnamed program item the left end.
 */
#include <stdio.h>
#include <string.h>

#include "wm_priv.h"

#define LONG_PRESS_MS 1000          /* long enough that an unhurried click stays a click */
#define PRESS_SLOP 10               /* the pointer may wander this far while the button is being held */

static const char *const fixed_keys[] = { "caps", "quick", "layout", "sticky", "osk", "magnifier", "clock", "power" };
#define NFIXED ((int)(sizeof(fixed_keys) / sizeof(fixed_keys[0])))

static int keys_find(const struct panel_keys *l, const char *key)
{
    for (int i = 0; i < l->n; i++)
        if (!strcmp(l->k[i], key))
            return i;
    return -1;
}

static void keys_insert(struct panel_keys *l, int at, const char *key)
{
    if (l->n >= PANEL_KEYS_MAX || !*key || keys_find(l, key) >= 0)
        return;
    at = LM_CLAMP(at, 0, l->n);
    memmove(l->k[at + 1], l->k[at], (size_t)(l->n - at) * sizeof(l->k[0]));
    snprintf(l->k[at], sizeof(l->k[0]), "%s", key);
    l->n++;
}

static void keys_remove(struct panel_keys *l, int at)
{
    memmove(l->k[at], l->k[at + 1], (size_t)(l->n - at - 1) * sizeof(l->k[0]));
    l->n--;
}

/* "a,b,c" */
static void keys_parse(struct panel_keys *l, const char *text)
{
    l->n = 0;
    while (*text) {
        size_t len = strcspn(text, ",");
        char key[PANEL_KEY_LEN];
        snprintf(key, sizeof(key), "%.*s", (int)LM_MIN(len, sizeof(key) - 1), text);
        keys_insert(l, l->n, key);
        text += len + (text[len] == ',');
    }
}

/* The saved order with every built-in button in it: one it does not name goes after the built-in one before it. */
static void keys_effective(struct panel_keys *l, const char *saved)
{
    keys_parse(l, saved);
    for (int i = 0; i < NFIXED; i++) {
        if (keys_find(l, fixed_keys[i]) >= 0)
            continue;
        int at = 0;
        for (int j = i - 1; j >= 0 && !at; j--)
            at = keys_find(l, fixed_keys[j]) + 1;
        keys_insert(l, at, fixed_keys[i]);
    }
}

struct pitem { const char *key; struct lm_rect *r; int w, kind, tray; };

int panel_arrange(const struct lm_wm *wm, struct lm_output *o, struct lm_panel_info *p, const struct panel_widths *pw, int taskbar_x)
{
    int y = p->bar.y + 2, h = p->bar.h - 4, right = p->bar.x + p->bar.w - 4;
    struct pitem def[NFIXED + LM_PANEL_MAX_TRAY], sorted[NFIXED + LM_PANEL_MAX_TRAY];
    struct { struct lm_rect *r; int w, kind; } fixed[NFIXED] = {
        { &p->caps, pw->caps, LM_PANEL_ITEM_CAPS }, { &p->quick, pw->quick, LM_PANEL_ITEM_QUICK }, { &p->layout, pw->layout, LM_PANEL_ITEM_LAYOUT },
        { &p->sticky, pw->sticky, LM_PANEL_ITEM_STICKY }, { &p->osk, pw->osk, LM_PANEL_ITEM_OSK }, { &p->magnifier, pw->magnifier, LM_PANEL_ITEM_MAGNIFIER },
        { &p->clock, pw->clock, LM_PANEL_ITEM_CLOCK }, { &p->power, pw->power, LM_PANEL_ITEM_POWER },
    };
    int room = right - taskbar_x - 120;                             /* what the programs' buttons may take: the taskbar keeps some */
    for (int i = 0; i < NFIXED; i++)
        if (fixed[i].w > 0)
            room -= fixed[i].w + (fixed[i].kind == LM_PANEL_ITEM_CLOCK ? 8 : 4);
    if (wm->srv->tray) {
        int n = lm_tray_count(wm->srv->tray);
        for (int i = 0; i < n && p->ntray < LM_PANEL_MAX_TRAY; i++) {   /* the first one created is the rightmost */
            struct lm_tray_view v;
            if (!lm_tray_get(wm->srv->tray, i, &v))
                break;
            int iw = v.icon ? v.icon->width : 0, tw = v.text[0] ? lm_font_text_width(v.text) : 0;
            int bw = 12 + iw + (iw && tw ? 4 : 0) + tw;
            if (bw < h)
                bw = h;
            if (bw > room)
                break;
            room -= bw + 4;
            int k = p->ntray++;
            p->tray[k].rect = lm_rect_make(0, y, bw, h);
            p->tray[k].id = v.id;
            p->tray[k].icon = v.icon;
            p->tray[k].text = v.text;
            p->tray[k].name = v.name;
            p->tray[k].color = v.color;
            p->tray[k].hover = wm->hover_tray == v.id;
        }
    }
    int n = 0;
    for (int k = p->ntray - 1; k >= 0; k--)
        def[n++] = (struct pitem){ p->tray[k].name[0] ? p->tray[k].name : "tray", &p->tray[k].rect, p->tray[k].rect.w, LM_PANEL_ITEM_TRAY, k };
    for (int i = 0; i < NFIXED; i++)
        def[n++] = (struct pitem){ fixed_keys[i], fixed[i].r, fixed[i].w, fixed[i].kind, -1 };
    bool dragged = wm->pdrag.on;
    struct panel_keys order;
    if (dragged)
        order = wm->pdrag.order;
    else
        keys_effective(&order, wm->srv->config.panel_order);
    int m = 0;
    for (int i = 0; i < n; i++)                                     /* programs' buttons nobody has placed yet: at the left end */
        if (keys_find(&order, def[i].key) < 0)
            sorted[m++] = def[i];
    for (int k = 0; k < order.n; k++)
        for (int i = 0; i < n; i++)
            if (!strcmp(def[i].key, order.k[k]))
                sorted[m++] = def[i];
    int x = right, left = right;
    bool left_tray = false;
    for (int i = m - 1; i >= 0; i--) {                              /* from the right edge leftwards; the clock keeps a wider gap before it */
        *sorted[i].r = lm_rect_make(x - sorted[i].w, y, sorted[i].w, h);
        if (sorted[i].w <= 0) {
            sorted[i].r->x = x + 4;
            continue;
        }
        x -= sorted[i].w;
        left = x;
        left_tray = sorted[i].kind == LM_PANEL_ITEM_TRAY;
        x -= sorted[i].kind == LM_PANEL_ITEM_CLOCK ? 8 : 4;
    }
    if (dragged && o == wm->pdrag.output && wm->srv->seat) {        /* the one in the hand follows the pointer, its place stays free */
        for (int i = 0; i < m; i++) {
            if (strcmp(sorted[i].key, wm->pdrag.key) || (sorted[i].kind == LM_PANEL_ITEM_TRAY && p->tray[sorted[i].tray].id != wm->pdrag.tray_id) ||
                sorted[i].w <= 0)
                continue;
            double px = 0, py = 0;
            lm_seat_pointer_position(wm->srv->seat, &px, &py);
            p->lift = sorted[i].kind;
            p->lift_tray = sorted[i].tray;
            p->lift_gap = *sorted[i].r;
            sorted[i].r->x = LM_CLAMP((int)px - wm->pdrag.grab_dx, p->bar.x + 4, LM_MAX(right - sorted[i].w, p->bar.x + 4));
            break;
        }
    }
    return left_tray ? left - 4 : left;
}

/* ---- the long press and the drag ------------------------------------------------------------------------------------------- */

struct slot { char key[PANEL_KEY_LEN]; int x, w, id; };

static int slot_cmp(const void *a, const void *b)
{
    return ((const struct slot *)a)->x - ((const struct slot *)b)->x;
}

/* The buttons of a laid out panel from left to right, a carried one at the place that is kept for it. */
static int panel_slots(const struct lm_panel_info *p, struct slot *s)
{
    const struct { const struct lm_rect *r; int kind; } fixed[NFIXED] = {
        { &p->caps, LM_PANEL_ITEM_CAPS }, { &p->quick, LM_PANEL_ITEM_QUICK }, { &p->layout, LM_PANEL_ITEM_LAYOUT }, { &p->sticky, LM_PANEL_ITEM_STICKY },
        { &p->osk, LM_PANEL_ITEM_OSK }, { &p->magnifier, LM_PANEL_ITEM_MAGNIFIER }, { &p->clock, LM_PANEL_ITEM_CLOCK }, { &p->power, LM_PANEL_ITEM_POWER },
    };
    int n = 0;
    for (int i = 0; i < NFIXED; i++) {
        const struct lm_rect *r = p->lift == fixed[i].kind ? &p->lift_gap : fixed[i].r;
        if (r->w <= 0)
            continue;
        s[n] = (struct slot){ "", r->x, r->w, 0 };
        snprintf(s[n++].key, PANEL_KEY_LEN, "%s", fixed_keys[i]);
    }
    for (int k = 0; k < p->ntray; k++) {
        const struct lm_rect *r = p->lift == LM_PANEL_ITEM_TRAY && p->lift_tray == k ? &p->lift_gap : &p->tray[k].rect;
        s[n] = (struct slot){ "", r->x, r->w, p->tray[k].id };
        snprintf(s[n++].key, PANEL_KEY_LEN, "%.15s", p->tray[k].name[0] ? p->tray[k].name : "tray");
    }
    qsort(s, (size_t)n, sizeof(*s), slot_cmp);
    return n;
}

static void drag_cursor(struct lm_wm *wm)
{
    if (wm->srv->seat)
        lm_seat_set_cursor_override(wm->srv->seat, LM_CURSOR_MOVE);
}

/* The button was held long enough: it comes loose. */
static void long_press(void *data)
{
    struct lm_wm *wm = data;
    if (!wm->pdrag.armed || !wm->pdrag.output || lm_lock_active(wm->srv->lock))
        return;
    wm->pdrag.armed = false;
    struct lm_panel_info p;
    struct slot s[NFIXED + LM_PANEL_MAX_TRAY];
    panel_info(wm, wm->pdrag.output, &p);
    int n = panel_slots(&p, s);
    struct panel_keys now = { 0 }, saved;
    for (int i = 0; i < n; i++)
        keys_insert(&now, now.n, s[i].key);
    keys_effective(&saved, wm->srv->config.panel_order);
    for (int i = 0; i < saved.n; i++) {                             /* the places of buttons that are not there now (a battery, a recording) are kept */
        if (keys_find(&now, saved.k[i]) >= 0)
            continue;
        int at = 0;
        for (int j = i - 1; j >= 0 && !at; j--)
            at = keys_find(&now, saved.k[j]) + 1;
        keys_insert(&now, at, saved.k[i]);
    }
    wm->pdrag.order = now;
    wm->pdrag.on = true;
    wm->pdrag.moved = false;
    damage_panels(wm);
    drag_cursor(wm);
}

void panel_press(struct lm_wm *wm)
{
    if (!wm->srv->seat || !wm->pdrag.timer)
        return;
    double px = 0, py = 0;
    lm_seat_pointer_position(wm->srv->seat, &px, &py);
    struct lm_output *o = lm_output_nearest(wm->srv, px, py);
    if (!o)
        return;
    struct lm_panel_info p;
    struct slot s[NFIXED + LM_PANEL_MAX_TRAY];
    panel_info(wm, o, &p);
    int n = panel_slots(&p, s);
    for (int i = 0; i < n; i++) {
        if ((int)px < s[i].x || (int)px >= s[i].x + s[i].w || !strcmp(s[i].key, "caps"))
            continue;
        wm->pdrag.armed = true;
        wm->pdrag.output = o;
        snprintf(wm->pdrag.key, sizeof(wm->pdrag.key), "%.15s", s[i].key);
        wm->pdrag.tray_id = s[i].id;
        wm->pdrag.press_x = (int)px;
        wm->pdrag.press_y = (int)py;
        wm->pdrag.grab_dx = (int)px - s[i].x;
        lm_timer_arm_ms(wm->pdrag.timer, LONG_PRESS_MS, 0);
        return;
    }
}

bool panel_drag_motion(struct lm_wm *wm)
{
    if (!wm->srv->seat)
        return false;
    double px = 0, py = 0;
    lm_seat_pointer_position(wm->srv->seat, &px, &py);
    if (wm->pdrag.armed && (abs((int)px - wm->pdrag.press_x) > PRESS_SLOP || abs((int)py - wm->pdrag.press_y) > PRESS_SLOP)) {
        wm->pdrag.armed = false;                                    /* moved away: not a long press */
        lm_timer_arm_ms(wm->pdrag.timer, 0, 0);
    }
    if (!wm->pdrag.on)
        return false;
    if (abs((int)px - wm->pdrag.press_x) > PRESS_SLOP || abs((int)py - wm->pdrag.press_y) > PRESS_SLOP)
        wm->pdrag.moved = true;
    for (int round = 0; round < PANEL_KEYS_MAX; round++) {          /* past the middle of a neighbour: they change places */
        struct lm_panel_info p;
        struct slot s[NFIXED + LM_PANEL_MAX_TRAY];
        panel_info(wm, wm->pdrag.output, &p);
        if (!p.lift)
            break;
        int n = panel_slots(&p, s), me = -1;
        for (int i = 0; i < n && me < 0; i++)
            if (!strcmp(s[i].key, wm->pdrag.key) && s[i].x == p.lift_gap.x)
                me = i;
        if (me < 0)
            break;
        int mid = LM_CLAMP((int)px - wm->pdrag.grab_dx, p.bar.x + 4, p.bar.x + p.bar.w - 4 - p.lift_gap.w) + p.lift_gap.w / 2;
        int l = me - 1, r = me + 1, other = -1;
        bool after = false;
        while (l >= 0 && !strcmp(s[l].key, wm->pdrag.key))
            l--;
        while (r < n && !strcmp(s[r].key, wm->pdrag.key))
            r++;
        if (l >= 0 && mid < s[l].x + s[l].w / 2)
            other = l;
        else if (r < n && mid > s[r].x + s[r].w / 2) {
            other = r;
            after = true;
        }
        if (other < 0)
            break;
        struct panel_keys *k = &wm->pdrag.order;
        int from = keys_find(k, wm->pdrag.key);
        if (from < 0 || keys_find(k, s[other].key) < 0)
            break;
        keys_remove(k, from);
        keys_insert(k, keys_find(k, s[other].key) + (after ? 1 : 0), wm->pdrag.key);
    }
    damage_panels(wm);
    drag_cursor(wm);
    return true;
}

void panel_drag_cancel(struct lm_wm *wm)
{
    bool was = wm->pdrag.on;
    if (wm->pdrag.timer)
        lm_timer_arm_ms(wm->pdrag.timer, 0, 0);
    wm->pdrag.armed = false;
    wm->pdrag.on = false;
    if (was) {
        damage_panels(wm);
        refresh_pointer(wm);
    }
}

bool panel_release(struct lm_wm *wm)
{
    if (!wm->pdrag.on || !wm->pdrag.moved) {                       /* held, perhaps long, but not carried anywhere: a click */
        panel_drag_cancel(wm);
        return false;
    }
    char text[200];
    size_t len = 0;
    text[0] = 0;
    for (int i = 0; i < wm->pdrag.order.n; i++) {
        size_t need = strlen(wm->pdrag.order.k[i]) + (len ? 1 : 0);
        if (len + need >= sizeof(text))
            break;
        len += (size_t)snprintf(text + len, sizeof(text) - len, "%s%s", len ? "," : "", wm->pdrag.order.k[i]);
    }
    wm->pdrag.on = false;
    if (!wm->srv->settings || !lm_settings_set(wm->srv->settings, "panel-order", text))
        snprintf(wm->srv->config.panel_order, sizeof(wm->srv->config.panel_order), "%s", text);
    damage_panels(wm);
    refresh_pointer(wm);
    return true;
}

void panel_drag_init(struct lm_wm *wm)
{
    wm->pdrag.timer = lm_loop_add_timer(wm->srv->loop, long_press, wm);
}

size_t lm_wm_panel_describe(struct lm_wm *wm, char *buf, size_t size)
{
    struct lm_output *o = lm_output_primary(wm->srv);
    if (!o || size == 0) {
        if (size)
            buf[0] = 0;
        return 0;
    }
    struct lm_panel_info p;
    struct slot s[NFIXED + LM_PANEL_MAX_TRAY];
    panel_info(wm, o, &p);
    int n = panel_slots(&p, s);
    size_t len = (size_t)snprintf(buf, size, "panel n=%d dragging=%s order=%s\n", n, wm->pdrag.on ? wm->pdrag.key : "-",
                                  wm->srv->config.panel_order[0] ? wm->srv->config.panel_order : "-");
    for (int i = 0; i < n && len < size; i++)
        len += (size_t)snprintf(buf + len, size - len, "button %s x=%d w=%d\n", s[i].key, s[i].x, s[i].w);
    return len < size ? len : size - 1;
}
