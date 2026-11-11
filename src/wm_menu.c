/* Part of the window manager (see wm_priv.h): menus: the dock icon's right-click menu and the ones programs ask for (text field actions) */
#include "wm_priv.h"

/* ---- menus: the dock icon's right-click menu and the ones programs ask for (text field actions) ----------------------------------------- */

#define DM_PAD 6
#define DM_HEAD LM_MENU_HEAD
#define DM_ITEM_H 28
#define DM_MIN_W 150

void menu_damage(struct lm_wm *wm)
{
    if (wm->menu_rect.w > 0)
        damage_rect(wm, &wm->menu_rect);
}

void menu_info(const struct lm_wm *wm, struct lm_menu_info *mi)
{
    memset(mi, 0, sizeof(*mi));
    mi->rect = wm->menu_rect;
    mi->title = wm->menu_title;
    mi->sel = wm->menu_sel;
    mi->n = wm->menu_n;
    int head = wm->menu_title[0] ? DM_HEAD : 0;
    for (int i = 0; i < wm->menu_n; i++) {
        mi->entry[i] = lm_rect_make(mi->rect.x + DM_PAD, mi->rect.y + head + DM_PAD + i * DM_ITEM_H, mi->rect.w - 2 * DM_PAD, DM_ITEM_H);
        mi->label[i] = wm->menu_label[i];
        mi->enabled[i] = wm->menu_enab[i];
    }
}

/* Closes the menu. A menu a program asked for is told when nothing was chosen. */
void menu_close(struct lm_wm *wm, bool chosen)
{
    if (!wm->menu_open)
        return;
    menu_damage(wm);
    bool client = wm->menu_owner == 1;
    wm->menu_open = false;
    wm->menu_sel = -1;
    wm->menu_rect = (struct lm_rect){ 0 };
    if (client && !chosen)
        lm_menu_closed(wm->srv);
}

void menu_set(struct lm_wm *wm, bool open)
{
    if (!open)
        menu_close(wm, false);
}

/* Fills the entries and works out the size of the card: the caller places it (menu_place). */
static bool menu_fill(struct lm_wm *wm, int owner, const char *title, const char *const *labels, const bool *enabled, int n, int *w, int *h)
{
    if (n < 1 || n > LM_MENU_MAX)
        return false;
    menu_close(wm, false);
    wm->menu_owner = owner;
    wm->menu_n = n;
    snprintf(wm->menu_title, sizeof(wm->menu_title), "%s", title ? title : "");
    int widest = lm_font_text_width(wm->menu_title);
    for (int i = 0; i < n; i++) {
        snprintf(wm->menu_label[i], sizeof(wm->menu_label[i]), "%s", labels[i]);
        wm->menu_enab[i] = enabled ? enabled[i] : true;
        widest = LM_MAX(widest, lm_font_text_width(wm->menu_label[i]));
    }
    *w = LM_MAX(DM_MIN_W, widest + 40);
    *h = (wm->menu_title[0] ? DM_HEAD : 0) + 2 * DM_PAD + n * DM_ITEM_H;
    return true;
}

static void menu_place(struct lm_wm *wm, int x, int y, int w, int h, const struct lm_output *o)
{
    struct lm_rect orect = lm_output_rect(o);
    x = LM_CLAMP(x, orect.x + 4, LM_MAX(orect.x + orect.w - w - 4, orect.x + 4));
    y = LM_CLAMP(y, orect.y + LM_PANEL_HEIGHT + 4, LM_MAX(orect.y + orect.h - h - 4, orect.y + LM_PANEL_HEIGHT + 4));
    wm->menu_open = true;
    wm->menu_sel = -1;
    wm->menu_rect = lm_rect_make(x, y, w, h);
    menu_damage(wm);
}

bool lm_wm_menu_popup(struct lm_wm *wm, const char *const *labels, int n)
{
    if (!wm || lm_lock_active(wm->srv->lock) || !wm->srv->seat)
        return false;
    double px = 0, py = 0;
    lm_seat_pointer_position(wm->srv->seat, &px, &py);
    struct lm_output *o = lm_output_nearest(wm->srv, px, py);
    int w, h;
    if (!o || !menu_fill(wm, 1, NULL, labels, NULL, n, &w, &h))
        return false;
    for (int i = 0; i < n; i++)                                      /* a leading '~' marks an entry that cannot be chosen */
        if (labels[i][0] == '~') {
            wm->menu_enab[i] = false;
            memmove(wm->menu_label[i], wm->menu_label[i] + 1, strlen(wm->menu_label[i]));
        }
    menu_place(wm, (int)px - 4, (int)py - 4, w, h, o);
    return true;
}

void lm_wm_menu_cancel(struct lm_wm *wm)
{
    if (wm && wm->menu_open && wm->menu_owner == 1) {
        menu_damage(wm);
        wm->menu_open = false;
        wm->menu_sel = -1;
        wm->menu_rect = (struct lm_rect){ 0 };
    }
}

/* Right click on a window's title bar or its taskbar button: Maximize / Restore, Minimize, Move to workspace 1..4, Close. */
void window_menu_open(struct lm_wm *wm, struct lm_window *w)
{
    if (!w || !w->mapped || win_is_calendar(w) || !wm->srv->seat)
        return;
    static char names[LM_NUM_WORKSPACES][24];
    const char *labels[3 + LM_NUM_WORKSPACES + 1];
    bool enabled[3 + LM_NUM_WORKSPACES + 1];
    int n = 0;
    bool max = (w->states & LM_WINDOW_MAXIMIZED) != 0;
    labels[n] = max ? "Restore" : "Maximize";
    enabled[n++] = !(w->states & LM_WINDOW_FULLSCREEN);
    labels[n] = "Minimize";
    enabled[n++] = !w->minimized;
    for (int i = 0; i < LM_NUM_WORKSPACES; i++) {
        snprintf(names[i], sizeof(names[i]), "Move to workspace %d", i + 1);
        labels[n] = names[i];
        enabled[n++] = i != w->workspace;
    }
    labels[n] = "Close";
    enabled[n++] = true;
    double px = 0, py = 0;
    lm_seat_pointer_position(wm->srv->seat, &px, &py);
    struct lm_output *o = lm_output_nearest(wm->srv, px, py);
    int mw, mh;
    if (!o || !menu_fill(wm, 3, w->title && w->title[0] ? w->title : NULL, labels, enabled, n, &mw, &mh))
        return;
    wm->menu_window = w;
    menu_place(wm, (int)px - 4, (int)py - 4, mw, mh, o);
}

void window_menu_activate(struct lm_wm *wm, int which)
{
    struct lm_window *w = wm->menu_window;
    wm->menu_window = NULL;
    if (!w)
        return;
    if (which == 0)
        lm_wm_window_request_maximize(w, !(w->states & LM_WINDOW_MAXIMIZED));
    else if (which == 1)
        lm_wm_window_request_minimize(w);
    else if (which >= 2 && which < 2 + LM_NUM_WORKSPACES)
        move_to_workspace(wm, w, which - 2);
    else
        lm_wm_window_close(w);
}

/* Right click on a text field of the launcher's form: Cut / Copy / Paste / Clear at the pointer. */
void launcher_field_menu(struct lm_wm *wm, int field)
{
    static const char *const labels[4] = { "Cut", "Copy", "Paste", "Clear" };
    wm->launcher_focus = field;
    size_t max;
    const char *text = launcher_field_text(wm, &max);
    if (!text || !wm->srv->seat)
        return;
    bool has = text[0] != 0, clip = lm_seat_clip_count(wm->srv->seat) > 0;
    bool enabled[4] = { has, has, clip, has };
    double px = 0, py = 0;
    lm_seat_pointer_position(wm->srv->seat, &px, &py);
    struct lm_output *o = lm_output_nearest(wm->srv, px, py);
    int w, h;
    if (!o || !menu_fill(wm, 2, NULL, labels, enabled, 4, &w, &h))
        return;
    menu_place(wm, (int)px - 4, (int)py - 4, w, h, o);
    launcher_damage(wm);
}

/* Right click on dock icon `i`: the menu opens above the icon. The Launcher button has none (it is not a program). */
void dockitem_menu_open(struct lm_wm *wm, int i)
{
    static const char *const labels[4] = { "New window", "Close all windows", "Change...", "Remove from dock" };
    const struct lm_dock_item *it = lm_dock_item(i);
    struct lm_output *o = dock_output(wm);
    if (!it || !o || dock_special(it))
        return;
    struct lm_dock_info d;
    dock_info(wm, o, &d);
    struct lm_rect box = { 0 };
    for (int k = 0; k < d.n; k++)
        if (d.icon[k].item == it)
            box = d.icon[k].box;
    if (box.w <= 0)
        return;
    bool enabled[4] = { true, dock_window_count(wm, it) > 0, true, true };      /* nothing to close when there is no window */
    int w, h;
    if (!menu_fill(wm, 0, it->name, labels, enabled, 4, &w, &h))
        return;
    w = LM_MAX(w, 190);
    wm->menu_dock_item = i;
    menu_place(wm, box.x + box.w / 2 - w / 2, box.y - 14 - h, w, h, o);
}

/* An entry was chosen: 0 a new window (another instance), 1 close every window of the program, 2 change the button in the launcher's form,
 * 3 take it out of the dock (it stays in the launcher). */
static void dockitem_menu_activate(struct lm_wm *wm, int which)
{
    int di = wm->menu_dock_item;
    const struct lm_dock_item *it = lm_dock_item(di);
    if (!it || dock_special(it))
        return;
    switch (which) {
    case 0:
        dock_launch(wm, it, di, false);
        break;
    case 1: {
        struct lm_window *w;
        lm_list_for_each(w, &wm->windows, link)
            if (w->mapped && dock_app_matches(it->match, w->app_id))
                lm_wm_window_close(w);
        break;
    }
    case 2: {
        int g = 0;                                                    /* its place in the launcher's list: the dock's buttons come first */
        for (int k = 0; k < di; k++)
            if (!launcher_is_self(lm_dock_item(k)))
                g++;
        launcher_set(wm, true, dock_output(wm));
        wm->launcher_edit = true;
        wm->launcher_sel = g;
        launcher_open_change(wm, g);
        break;
    }
    case 3:
        if (!lm_settings_dock_to_launcher(wm->srv, it->raw))
            lm_wm_notify(wm, "Dock", "Could not remove it: there is no config file to write", 5000);
        break;
    }
}

/* An entry was chosen: whoever asked for the menu gets it. */
void menu_activate(struct lm_wm *wm, int which)
{
    int owner = wm->menu_owner;
    if (which < 0 || which >= wm->menu_n || !wm->menu_enab[which])
        return;
    menu_close(wm, true);
    if (owner == 0)
        dockitem_menu_activate(wm, which);
    else if (owner == 2)
        launcher_field_clip(wm, which);
    else if (owner == 3)
        window_menu_activate(wm, which);
    else
        lm_menu_chosen(wm->srv, which);
}

size_t lm_wm_menu_describe(struct lm_wm *wm, char *buf, size_t size)
{
    size_t n = (size_t)snprintf(buf, size, "dockmenu open=%d item=%d sel=%d owner=%d\n", wm->menu_open,
                                wm->menu_open && wm->menu_owner == 0 ? wm->menu_dock_item : -1, wm->menu_open ? wm->menu_sel : -1,
                                wm->menu_open ? wm->menu_owner : -1);
    if (!wm->menu_open)
        return n < size ? n : size - 1;
    struct lm_menu_info mi;
    menu_info(wm, &mi);
    n += (size_t)snprintf(buf + n, size - n, "card x=%d y=%d w=%d h=%d title=\"%s\"\n", mi.rect.x, mi.rect.y, mi.rect.w, mi.rect.h, mi.title);
    for (int i = 0; i < mi.n && n < size; i++)
        n += (size_t)snprintf(buf + n, size - n, "entry %d \"%s\" x=%d y=%d w=%d h=%d enabled=%d\n", i, mi.label[i], mi.entry[i].x, mi.entry[i].y,
                              mi.entry[i].w, mi.entry[i].h, mi.enabled[i]);
    return n < size ? n : size - 1;
}
