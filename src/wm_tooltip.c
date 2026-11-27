/*
 * wm_tooltip.c - the tooltips of the top panel: when the pointer has rested on a button for TIP_DELAY_MS a small card under the bar says what
 * it is (and its shortcut, and what is going on: the layout, the date, a window's whole title). Once one is up, the neighbour's shows at once.
 * A press, a carried button, a menu, a dialog or the lock takes it away. The words of a program's button come from the program
 * (mellagui_tray_item_v1.set_tooltip), else from the item's name.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "wm_priv.h"

#define TIP_DELAY_MS 600
#define TIP_AGAIN_MS 400            /* from one button to the next within this: no second wait */
#define TIP_MAX_W 480
#define TIP_GAP 4                   /* between the bar and the card */

static void add_line(struct lm_tooltip_info *ti, int max_px, const char *fmt, ...)
{
    if (ti->n >= LM_TOOLTIP_LINES)
        return;
    char *line = ti->line[ti->n];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(line, LM_TOOLTIP_LINE, fmt, ap);
    va_end(ap);
    if (len <= 0)
        return;
    bool cut = len >= LM_TOOLTIP_LINE;
    if (cut || lm_font_text_width(line) > max_px) {             /* too long (a window's title): cut at a whole character */
        char whole[LM_TOOLTIP_LINE];
        snprintf(whole, sizeof(whole), "%s", line);
        if (strlen(whole) > LM_TOOLTIP_LINE - 4)
            whole[LM_TOOLTIP_LINE - 4] = 0;
        int fit = lm_font_fit(whole, max_px - lm_font_text_width("..."));
        snprintf(line, LM_TOOLTIP_LINE, "%.*s...", fit, whole);
    }
    ti->n++;
}

/* " (Super+K)" for the first combination that does `action` (with that number), "" when none does. */
static const char *shortcut(const struct lm_wm *wm, enum lm_action action, int num, char *out, size_t n)
{
    out[0] = 0;
    const struct lm_keybinds *k = &wm->srv->config.keys;
    for (int i = 0; i < k->n; i++) {
        char combo[64];
        if (k->b[i].action == action && k->b[i].num == num && lm_keys_combo_text(k->b[i].mods, k->b[i].code, combo, sizeof(combo))) {
            snprintf(out, n, "  (%s)", combo);
            break;
        }
    }
    return out;
}

static void tray_lines(struct lm_wm *wm, struct lm_tooltip_info *ti, int max_px, int id)
{
    static const struct { const char *name, *text; } known[] = {
        { "battery", "Battery" }, { "network", "Network" }, { "bell", "Notifications" }, { "bluetooth", "Bluetooth" },
        { "record", "Screen recording" }, { "timer", "Timer" },
    };
    struct lm_tray_view v;
    for (int i = 0; wm->srv->tray && lm_tray_get(wm->srv->tray, i, &v); i++) {
        if (v.id != id)
            continue;
        if (v.tooltip[0]) {
            for (const char *s = v.tooltip; *s;) {
                size_t len = strcspn(s, "\n");
                if (len)
                    add_line(ti, max_px, "%.*s", (int)len, s);
                s += len + (s[len] == '\n');
            }
            return;
        }
        for (size_t k = 0; k < sizeof(known) / sizeof(known[0]); k++)
            if (!strcmp(v.name, known[k].name)) {
                add_line(ti, max_px, "%s", known[k].text);
                return;
            }
        add_line(ti, max_px, "%s", v.text);                     /* a program that says nothing about itself: its whole label */
        return;
    }
}

/* The words for the button under the pointer and its place in the bar; false when the button is not there any more. */
static bool tip_text(struct lm_wm *wm, const struct lm_panel_info *p, struct lm_tooltip_info *ti, int max_px, struct lm_rect *at)
{
    char key[80];
    const struct lm_config *cfg = &wm->srv->config;
    switch (wm->tip.kind) {
    case LM_PICK_PANEL_LAUNCHER:
        *at = p->launcher;
        add_line(ti, max_px, "About MellaGUI");
        break;
    case LM_PICK_PANEL_WORKSPACE: {
        int ws = wm->tip.index, n = 0;
        if (ws < 0 || ws >= LM_NUM_WORKSPACES)
            return false;
        *at = p->workspace[ws];
        const struct lm_window *w;
        lm_list_for_each(w, &wm->windows, link)
            n += w->mapped && w->workspace == ws && !win_is_aux(w);
        add_line(ti, max_px, "Workspace %d%s", ws + 1, shortcut(wm, LM_ACT_WORKSPACE, ws, key, sizeof(key)));
        if (n)
            add_line(ti, max_px, "%d window%s", n, n == 1 ? "" : "s");
        else
            add_line(ti, max_px, "No windows");
        break;
    }
    case LM_PICK_PANEL_TASK:
        for (int i = 0; i < p->ntasks; i++)
            if (p->task_win[i] == wm->tip.win) {
                *at = p->task[i].rect;
                add_line(ti, max_px, "%s", p->task[i].title[0] ? p->task[i].title : "(no title)");
                if (p->task[i].minimized)
                    add_line(ti, max_px, "Minimized");
            }
        break;
    case LM_PICK_PANEL:                                         /* the Caps / Num lamp: the only other part of the bar with words */
        *at = p->caps;
        if (p->caps.w <= 0)
            return false;
        add_line(ti, max_px, "%s", !strcmp(p->caps_text, "CAPS") ? "Caps Lock is on" :
                                   !strcmp(p->caps_text, "NUM") ? "Num Lock is on" : "Caps Lock and Num Lock are on");
        break;
    case LM_PICK_PANEL_TRAY:
        for (int i = 0; i < p->ntray; i++)
            if (p->tray[i].id == wm->tip.index) {
                *at = p->tray[i].rect;
                tray_lines(wm, ti, max_px, wm->tip.index);
            }
        break;
    case LM_PICK_PANEL_QUICK:
        *at = p->quick;
        add_line(ti, max_px, "Quick settings%s", shortcut(wm, LM_ACT_QUICK, 0, key, sizeof(key)));
        break;
    case LM_PICK_PANEL_LAYOUT: {
        *at = p->layout;
        if (!wm->srv->input)
            return false;
        const char *id = lm_input_layout_id(wm->srv->input, lm_input_layout(wm->srv->input));
        int li = lm_layout_find(id);
        add_line(ti, max_px, "Keyboard layout: %s", li >= 0 ? lm_layout_name(li) : id);
        if (lm_input_layout_count(wm->srv->input) > 1) {
            add_line(ti, max_px, "Click: next layout%s", shortcut(wm, LM_ACT_LAYOUT_NEXT, 0, key, sizeof(key)));
            add_line(ti, max_px, "Right click: previous");
        }
        break;
    }
    case LM_PICK_PANEL_STICKY:
        *at = p->sticky;
        add_line(ti, max_px, "Sticky keys: %s", p->sticky_on ? "on" : "off");
        break;
    case LM_PICK_PANEL_OSK:
        *at = p->osk;
        add_line(ti, max_px, "On-screen keyboard%s", shortcut(wm, LM_ACT_OSK, 0, key, sizeof(key)));
        break;
    case LM_PICK_PANEL_MAGNIFIER:
        *at = p->magnifier;
        if (cfg->magnifier)
            add_line(ti, max_px, "Screen magnifier: %d %%", cfg->magnifier_zoom);
        else
            add_line(ti, max_px, "Screen magnifier: off");
        add_line(ti, max_px, "Right click: zoom levels");
        break;
    case LM_PICK_PANEL_CLOCK: {
        *at = p->clock;
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        add_line(ti, max_px, "%s, %d %s %d", lm_weekday_name(tm.tm_wday, true), tm.tm_mday, lm_month_name(tm.tm_mon, true), tm.tm_year + 1900);
        if (cfg->cal_enabled)
            add_line(ti, max_px, "Click: calendar");
        break;
    }
    case LM_PICK_PANEL_POWER:
        *at = p->power;
        add_line(ti, max_px, "Lock, exit, suspend, restart, shut down");
        break;
    default:
        return false;
    }
    return ti->n > 0 && at->w > 0;
}

bool tooltip_info(struct lm_wm *wm, struct lm_tooltip_info *ti, struct lm_output **out)
{
    memset(ti, 0, sizeof(*ti));
    if (!wm->tip.shown || !wm->tip.kind || !wm->srv->seat || !wm->srv->backend || lm_lock_active(wm->srv->lock) || wm->menu_open ||
        wm->launcher_open || wm->power_open || wm->about_open || wm->sw_open || wm->pdrag.on || wm->grab_kind != GRAB_NONE)
        return false;
    double px = 0, py = 0;
    lm_seat_pointer_position(wm->srv->seat, &px, &py);
    struct lm_output *o = lm_output_nearest(wm->srv, px, py);
    if (!o || fullscreen_covers(wm, o))
        return false;
    struct lm_panel_info p;
    struct lm_rect at = { 0 };
    panel_info(wm, o, &p);
    if (!tip_text(wm, &p, ti, LM_MIN(TIP_MAX_W, o->width - 8 - 2 * LM_TOOLTIP_PAD), &at))
        return false;
    int w = 0;
    for (int i = 0; i < ti->n; i++)
        w = LM_MAX(w, lm_font_text_width(ti->line[i]));
    w += 2 * LM_TOOLTIP_PAD;
    int x = LM_CLAMP(at.x + at.w / 2 - w / 2, o->x + 4, LM_MAX(o->x + o->width - 4 - w, o->x + 4));
    ti->rect = lm_rect_make(x, p.bar.y + p.bar.h + TIP_GAP, w, ti->n * LM_FONT_H + 10);
    if (out)
        *out = o;
    return true;
}

void tooltip_damage(struct lm_wm *wm)
{
    if (wm->tip.rect.w > 0)
        damage_rect(wm, &wm->tip.rect);
    struct lm_tooltip_info ti;
    wm->tip.rect = (struct lm_rect){ 0 };
    if (tooltip_info(wm, &ti, NULL)) {
        wm->tip.rect = ti.rect;
        damage_rect(wm, &ti.rect);
    }
}

static void tip_show(void *data)
{
    struct lm_wm *wm = data;
    if (!wm->tip.kind)
        return;
    wm->tip.shown = true;
    tooltip_damage(wm);
}

static void tip_hide(struct lm_wm *wm)
{
    if (wm->tip.timer)
        lm_timer_arm_ms(wm->tip.timer, 0, 0);
    wm->tip.shown = false;
    tooltip_damage(wm);
}

void tooltip_dismiss(struct lm_wm *wm)
{
    tip_hide(wm);
    wm->tip.hidden_ms = 0;
}

void tooltip_motion(struct lm_wm *wm, const struct lm_pick *pick)
{
    int kind = 0, index = 0;
    struct lm_window *win = NULL;
    switch (pick->kind) {
    case LM_PICK_PANEL_LAUNCHER: case LM_PICK_PANEL_CLOCK: case LM_PICK_PANEL_POWER: case LM_PICK_PANEL_LAYOUT:
    case LM_PICK_PANEL_STICKY: case LM_PICK_PANEL_MAGNIFIER: case LM_PICK_PANEL_OSK: case LM_PICK_PANEL_QUICK:
        kind = pick->kind;
        break;
    case LM_PICK_PANEL_WORKSPACE: case LM_PICK_PANEL_TRAY:
        kind = pick->kind;
        index = pick->index;
        break;
    case LM_PICK_PANEL_TASK:
        kind = pick->kind;
        win = pick->window;
        break;
    case LM_PICK_PANEL: {                                       /* the Caps / Num lamp is no button, it has a tooltip all the same */
        if (!wm->srv->seat)
            break;
        double px = 0, py = 0;
        lm_seat_pointer_position(wm->srv->seat, &px, &py);
        struct lm_output *o = lm_output_nearest(wm->srv, px, py);
        struct lm_panel_info p;
        if (!o)
            break;
        panel_info(wm, o, &p);
        if (lm_rect_contains(&p.caps, (int)px, (int)py))
            kind = LM_PICK_PANEL;
        break;
    }
    default:
        break;
    }
    if (kind == wm->tip.kind && index == wm->tip.index && win == wm->tip.win)
        return;
    uint64_t now = lm_now_ms();
    if (wm->tip.shown) {
        tip_hide(wm);
        wm->tip.hidden_ms = now;
    } else if (wm->tip.timer) {
        lm_timer_arm_ms(wm->tip.timer, 0, 0);
    }
    wm->tip.kind = kind;
    wm->tip.index = index;
    wm->tip.win = win;
    if (!kind)
        return;
    if (wm->tip.hidden_ms && now - wm->tip.hidden_ms < TIP_AGAIN_MS)
        tip_show(wm);
    else if (wm->tip.timer)
        lm_timer_arm_ms(wm->tip.timer, TIP_DELAY_MS, 0);
}

void tooltip_init(struct lm_wm *wm)
{
    wm->tip.timer = lm_loop_add_timer(wm->srv->loop, tip_show, wm);
}

size_t lm_wm_tooltip_describe(struct lm_wm *wm, char *buf, size_t size)
{
    struct lm_tooltip_info ti;
    bool on = tooltip_info(wm, &ti, NULL);
    if (!size)
        return 0;
    size_t len = (size_t)snprintf(buf, size, "tooltip shown=%d x=%d y=%d w=%d h=%d n=%d\n", on, ti.rect.x, ti.rect.y, ti.rect.w, ti.rect.h, ti.n);
    for (int i = 0; i < ti.n && len < size; i++)
        len += (size_t)snprintf(buf + len, size - len, "line %s\n", ti.line[i]);
    return len < size ? len : size - 1;
}
