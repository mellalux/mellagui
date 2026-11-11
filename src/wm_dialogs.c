/* Part of the window manager (see wm_priv.h): Suspend / Restart / Shutdown, the About dialog, the session dialog: a round power button in the panel opens a card with round Lock and Exit buttons */
#include "wm_priv.h"

/* ---- the session dialog: a round power button in the panel opens a card with round Lock and Exit buttons ---------------- */

/* The output the dialog is on (looked up by name, so an unplugged one just ends the dialog). */
struct lm_output *power_output(const struct lm_wm *wm)
{
    if (!wm->power_open || !wm->srv->backend)
        return NULL;
    struct lm_output *o;
    lm_list_for_each(o, &wm->srv->backend->outputs, link)
        if (!strcmp(o->name, wm->power_out))
            return o;
    return NULL;
}

/* Suspend makes no sense on a server: `[commands] suspend = none` (or empty) turns the button off. */
bool power_suspend_off(const struct lm_wm *wm)
{
    const char *c = wm->srv->config.suspend_cmd;
    return !c || !*c || !strcmp(c, "none");
}

/* How many login sessions other than ours logind knows about (a server may have people on ssh): /run/systemd/sessions has one file per session. */
static int other_sessions(void)
{
    DIR *d = opendir("/run/systemd/sessions");
    if (!d)
        return 0;
    const char *mine = getenv("XDG_SESSION_ID");
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        const char *nm = e->d_name;
        bool digits = *nm != 0;
        for (const char *p = nm; *p; p++)
            if (*p < '0' || *p > '9')
                digits = false;
        if (digits && (!mine || strcmp(mine, nm)))
            n++;
    }
    closedir(d);
    return n;
}

void power_info(const struct lm_wm *wm, struct lm_output *o, struct lm_power_info *pi)
{
    memset(pi, 0, sizeof(*pi));
    pi->rect = lm_output_rect(o);
    pi->card = lm_rect_make(pi->rect.x + (pi->rect.w - POWER_CARD_W) / 2, pi->rect.y + (pi->rect.h - POWER_ROW1_H) / 2,
                            POWER_CARD_W, POWER_CARD_H);
    int gap = 40, total = 2 * POWER_BTN_D + gap;
    int x = pi->card.x + (POWER_CARD_W - total) / 2, y = pi->card.y + 46;
    for (int i = 0; i < 2; i++) {
        pi->button[i] = lm_rect_make(x + i * (POWER_BTN_D + gap), y, POWER_BTN_D, POWER_BTN_D);
        pi->hit[i] = lm_rect_make(pi->button[i].x - 8, y, POWER_BTN_D + 16, POWER_BTN_D + 8 + LM_FONT_H + 4);
    }
    int gap2 = 36, total2 = 3 * POWER_BTN2_D + 2 * gap2;                 /* the lower row: Suspend, Restart, Shutdown */
    int x2 = pi->card.x + (POWER_CARD_W - total2) / 2, y2 = pi->card.y + 156;
    for (int i = 0; i < 3; i++) {
        pi->button[2 + i] = lm_rect_make(x2 + i * (POWER_BTN2_D + gap2), y2, POWER_BTN2_D, POWER_BTN2_D);
        pi->hit[2 + i] = lm_rect_make(pi->button[2 + i].x - 8, y2, POWER_BTN2_D + 16, POWER_BTN2_D + 8 + LM_FONT_H + 4);
    }
    pi->sel = wm->power_sel;
    pi->confirm = wm->power_confirm;
    pi->others = wm->power_others;
    pi->off[2] = power_suspend_off(wm);
    if (pi->off[2])
        pi->hit[2] = (struct lm_rect){ 0 };
    if (wm->power_confirm)                                       /* only Yes and Cancel (the upper row's places) are there */
        for (int i = 2; i < LM_POWER_N; i++)
            pi->button[i] = pi->hit[i] = (struct lm_rect){ 0 };
}

void power_damage(struct lm_wm *wm)
{
    struct lm_output *o = power_output(wm);
    if (o) {
        struct lm_rect r = lm_output_rect(o);
        damage_rect(wm, &r);
    }
    damage_panels(wm);
}

/* Opens the dialog on `o` (the output whose panel button was used; NULL = the primary one) or closes it. */
void power_set(struct lm_wm *wm, bool open, struct lm_output *o)
{
    if (open)
        menu_set(wm, false);
    if (open == wm->power_open)
        return;
    if (open) {
        if (!o)
            o = lm_output_primary(wm->srv);
        if (!o)
            return;
        about_set(wm, false, NULL);
        launcher_set(wm, false, NULL);
        sw_close(wm);
        clip_nav_set(wm, false);
        snprintf(wm->power_out, sizeof(wm->power_out), "%s", o->name);
        wm->power_open = true;
        wm->power_confirm = 0;
        wm->power_sel = -1;
        power_damage(wm);
    } else {
        power_damage(wm);                                       /* (while the output is still found) */
        wm->power_open = false;
        wm->power_confirm = 0;
        wm->hover_power = false;
        damage_panels(wm);
    }
}


/* ---- the About dialog ----------------------------------------------------------------------------------------------------- */

struct lm_output *about_output(const struct lm_wm *wm)
{
    if (!wm->about_open || !wm->srv->backend)
        return NULL;
    struct lm_output *o;
    lm_list_for_each(o, &wm->srv->backend->outputs, link)
        if (!strcmp(o->name, wm->about_out))
            return o;
    return NULL;
}

void about_info(const struct lm_wm *wm, struct lm_output *o, struct lm_about_info *ai)
{
    memset(ai, 0, sizeof(*ai));
    ai->rect = lm_output_rect(o);
    int h = 20 + 96 + 12 + LM_FONT_H + 6 + LM_FONT_H + 16 + wm->about_n * ABOUT_ROW_H + 18 + 26 + 18;
    h = LM_MIN(h, ai->rect.h - 8);
    ai->card = lm_rect_make(ai->rect.x + (ai->rect.w - ABOUT_CARD_W) / 2, ai->rect.y + (ai->rect.h - h) / 2, ABOUT_CARD_W, h);
    ai->logo = lm_rect_make(ai->card.x + (ABOUT_CARD_W - 96) / 2, ai->card.y + 20, 96, 96);
    ai->close = lm_rect_make(ai->card.x + (ABOUT_CARD_W - 90) / 2, ai->card.y + ai->card.h - 18 - 26, 90, 26);
    ai->hover_close = wm->about_hover_close;
    ai->n = wm->about_n;
    ai->row = wm->about_rows;
}

void about_damage(struct lm_wm *wm)
{
    struct lm_output *o = about_output(wm);
    if (o) {
        struct lm_rect r = lm_output_rect(o);
        damage_rect(wm, &r);
    }
    damage_panels(wm);
}

/* Opens the dialog on `o` (the output whose logo button was used; NULL = the primary one) or closes it. */
void about_set(struct lm_wm *wm, bool open, struct lm_output *o)
{
    if (open)
        menu_set(wm, false);
    if (open == wm->about_open)
        return;
    if (open) {
        if (!o)
            o = lm_output_primary(wm->srv);
        if (!o)
            return;
        power_set(wm, false, NULL);
        launcher_set(wm, false, NULL);
        sw_close(wm);
        clip_nav_set(wm, false);
        wm->about_n = lm_about_collect(wm->srv, wm->about_rows, LM_ABOUT_ROWS);
        snprintf(wm->about_out, sizeof(wm->about_out), "%s", o->name);
        wm->about_open = true;
        wm->about_hover_close = false;
        about_damage(wm);
    } else {
        about_damage(wm);                                       /* (while the output is still found) */
        wm->about_open = false;
        wm->about_hover_close = false;
        damage_panels(wm);
    }
}

void lm_wm_about_set(struct lm_wm *wm, bool open)
{
    about_set(wm, open, NULL);
}

bool lm_wm_about_is_open(const struct lm_wm *wm)
{
    return wm->about_open;
}

size_t lm_wm_about_describe(struct lm_wm *wm, char *buf, size_t size)
{
    struct lm_output *o = about_output(wm);
    size_t n = 0;
    buf[0] = 0;
    if (!o)
        return 0;
    struct lm_about_info ai;
    about_info(wm, o, &ai);
    n += (size_t)snprintf(buf + n, size - n, "card x=%d y=%d w=%d h=%d\nclose x=%d y=%d w=%d h=%d\n", ai.card.x, ai.card.y, ai.card.w, ai.card.h,
                          ai.close.x, ai.close.y, ai.close.w, ai.close.h);
    for (int i = 0; i < ai.n && n < size; i++)
        n += (size_t)snprintf(buf + n, size - n, "row %s|%s\n", ai.row[i].label, ai.row[i].value);
    return n < size ? n : size - 1;
}



/* ---- Suspend / Restart / Shutdown ------------------------------------------------------------------------------------------ */

struct power_run {
    struct lm_wm *wm;
    int action;
};

static const char *power_word(int action)
{
    return action == LM_LOCK_ACT_SUSPEND ? "Suspend" : action == LM_LOCK_ACT_REBOOT ? "Restart" : "Shutdown";
}

static void power_exit(void *data, int status, const char *err)
{
    struct power_run *pr = data;
    if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0)) {                 /* it did not work (no permission, an inhibitor ...): say so */
        char msg[256];
        while (*err == ' ')
            err++;
        if (*err)
            snprintf(msg, sizeof(msg), "%s failed: %s", power_word(pr->action), err);
        else
            snprintf(msg, sizeof(msg), "%s failed (exit status %d)", power_word(pr->action), WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        lm_wm_notify(pr->wm, "Session", msg, 10000);
    }
    free(pr);
}

void lm_wm_power_run(struct lm_wm *wm, int action)
{
    if (!wm || action < LM_LOCK_ACT_SUSPEND || action > LM_LOCK_ACT_POWEROFF)
        return;
    const struct lm_config *c = &wm->srv->config;
    const char *cmd = action == LM_LOCK_ACT_SUSPEND ? c->suspend_cmd : action == LM_LOCK_ACT_REBOOT ? c->reboot_cmd : c->poweroff_cmd;
    if (!cmd || !*cmd) {
        lm_wm_notify(wm, "Session", "There is no command for that: set it in [commands] of mellagui.conf", 6000);
        return;
    }
    lm_info("session: %s", power_word(action));
    struct power_run *pr = malloc(sizeof(*pr));
    if (!pr)
        return;
    pr->wm = wm;
    pr->action = action;
    if (lm_server_spawn_watch(wm->srv, cmd, power_exit, pr) <= 0) {
        free(pr);
        lm_wm_notify(wm, "Session", "Could not start the command", 6000);
    }
}

/* The answer of the confirmation the dialog asks for a power action. */
void power_answer(struct lm_wm *wm, bool yes)
{
    int act = wm->power_confirm;
    power_set(wm, false, NULL);
    if (yes)
        lm_wm_power_run(wm, act);
}

/* Lock (0), Exit (1), Suspend (2), Restart (3) or Shutdown (4) was chosen in the session dialog. */
void power_activate(struct lm_wm *wm, int which)
{
    if (wm->power_confirm) {                                    /* the question is up: 0 = Yes, 1 = Cancel */
        power_answer(wm, which == 0);
        return;
    }
    if (which == 2 && power_suspend_off(wm))
        return;                                                 /* (no command: the button is off) */
    if (which >= 2 && which <= 4) {
        int act = which;                                        /* (the dialog's index is the action number) */
        if (wm->srv->lock && lm_lock_request_action(wm->srv->lock, act)) {      /* a secret is set: the lock screen asks for it */
            power_set(wm, false, NULL);
            return;
        }
        wm->power_confirm = act;                                /* no secret: the dialog asks once more; Cancel is highlighted */
        wm->power_others = other_sessions();
        wm->power_sel = 1;
        power_damage(wm);
        return;
    }
    power_set(wm, false, NULL);
    if (which == 0) {
        if (!lm_lock_now(wm->srv->lock))                        /* no password / PIN yet: open the settings to set one */
            launch_settings(wm);
    } else {
        lm_info("session dialog: exit chosen, quitting");
        request_quit(wm);
    }
}

bool lm_wm_pointer_button(struct lm_wm *wm, const struct lm_pick *pick, uint32_t time_ms,
                          uint32_t button, bool pressed)
{
    if (!pressed && wm->osk_down >= 0 && button == BTN_LEFT) {   /* the held on-screen key comes up wherever the pointer is */
        osk_release(wm);
        return true;
    }
    if (!pressed && wm->osk_drag && button == BTN_LEFT) {        /* the keyboard was moved / resized: remember where it is */
        wm->osk_drag = 0;
        gadgets_save(wm);
        lm_wm_osk_layout_changed(wm);                           /* (not while it is dragged: the windows would run about) */
        return true;
    }
    if (pressed && button == BTN_LEFT) {                        /* a click anywhere else sends the calendar / the quick settings card away */
        struct lm_window *pw;
        lm_list_for_each(pw, &wm->windows, link)
            if (pw->mapped && !pw->minimized && win_is_calendar(pw) && pick->window != pw &&
                !(pick->kind == LM_PICK_PANEL_QUICK && win_is_quick(pw)) && !(pick->kind == LM_PICK_PANEL_CLOCK && !win_is_quick(pw)))
                lm_wm_window_request_minimize(pw);
    }
    if (pick->kind == LM_PICK_OSK_KEY || pick->kind == LM_PICK_OSK || pick->kind == LM_PICK_OSK_GRIP) {
        if (pressed && button == BTN_LEFT) {
            if (pick->kind == LM_PICK_OSK_KEY) {
                osk_press(wm, pick->index);
            } else if (pick->kind == LM_PICK_OSK && time_ms - wm->osk_click_ms < (uint32_t)DOUBLE_CLICK_MS && wm->osk_pl.w > 0) {
                wm->osk_click_ms = 0;                            /* a double click on the margin puts it back to its default place and size */
                lm_wm_osk_reset(wm);
            } else {
                wm->osk_click_ms = pick->kind == LM_PICK_OSK ? time_ms : 0;
                osk_drag_begin(wm, pick->kind == LM_PICK_OSK ? 1 : 2, pick->edges);
            }
        }
        return true;                                            /* the keyboard never moves the focus and nothing under it gets the click */
    }
    if (pick->kind == LM_PICK_MENU_ITEM || pick->kind == LM_PICK_MENU || pick->kind == LM_PICK_MENU_BACKDROP) {
        int pk = wm->press_panel_kind, pi = wm->press_panel_ws;
        if (pressed) {
            if (pick->kind == LM_PICK_MENU_BACKDROP) {      /* a click outside the menu closes it, nothing below gets it */
                menu_set(wm, false);
            } else if (pick->kind == LM_PICK_MENU_ITEM && button == BTN_LEFT) {
                wm->press_panel_kind = LM_PICK_MENU_ITEM;
                wm->press_panel_ws = pick->index;
                wm->press_win = NULL;
                wm->press_bits = 0;
            }
        } else {
            wm->press_panel_kind = 0;
            if (pk == LM_PICK_MENU_ITEM && pick->kind == LM_PICK_MENU_ITEM && pick->index == pi && button == BTN_LEFT)
                menu_activate(wm, pi);                          /* released on the entry it was pressed on */
        }
        return true;
    }
    if (pick->kind >= LM_PICK_OVERVIEW_ITEM && pick->kind <= LM_PICK_OVERVIEW) {       /* the desktop overview: a click is done when the button is let go on what it was pressed on */
        if (pressed) {
            wm->press_panel_kind = pick->kind;
            wm->press_panel_ws = pick->index;
            wm->press_win = NULL;
            wm->press_bits = 0;
            if (button == BTN_RIGHT && pick->kind == LM_PICK_OVERVIEW_ITEM && pick->index < wm->ov.n)
                window_menu_open(wm, wm->ov.item[pick->index].win);          /* Maximize, Minimize, Move to workspace ..., Close */
            if (wm->srv->seat) {
                double px, py;
                lm_seat_pointer_position(wm->srv->seat, &px, &py);
                wm->drag_press_x = (int)px;
                wm->drag_press_y = (int)py;
            }
        } else {
            int pk = wm->press_panel_kind, pi = wm->press_panel_ws;
            wm->press_panel_kind = 0;
            if (wm->ov.drag)
                overview_drag_finish(wm, pick);                              /* carried to a workspace cell (or dropped nowhere) */
            else if (pk == (int)pick->kind && pi == pick->index)
                overview_click(wm, pick, button);
        }
        return true;
    }
    if (pick->kind == LM_PICK_POWER_BUTTON || pick->kind == LM_PICK_POWER_CARD || pick->kind == LM_PICK_POWER_BACKDROP) {
        int pk = wm->press_panel_kind, pi = wm->press_panel_ws;
        if (pressed) {
            if (pick->kind == LM_PICK_POWER_BACKDROP)           /* a click outside the card closes the dialog, nothing below gets it */
                power_set(wm, false, NULL);
            else if (pick->kind == LM_PICK_POWER_BUTTON && button == BTN_LEFT) {
                wm->press_panel_kind = LM_PICK_POWER_BUTTON;
                wm->press_panel_ws = pick->index;
                wm->press_win = NULL;
                wm->press_bits = 0;
            }
        } else {
            wm->press_panel_kind = 0;
            if (pk == LM_PICK_POWER_BUTTON && pick->kind == LM_PICK_POWER_BUTTON && pick->index == pi && button == BTN_LEFT)
                power_activate(wm, pi);                         /* released on the button it was pressed on */
        }
        return true;
    }
    if (pressed && button == BTN_RIGHT && pick->kind == LM_PICK_LAUNCHER_FIELD) {      /* a right click on a field of the form: its menu */
        launcher_field_menu(wm, pick->index);
        return true;
    }
    if (pick->kind >= LM_PICK_LAUNCHER_ITEM && pick->kind <= LM_PICK_LAUNCHER_BACKDROP) {
        if (pressed && wm->bubble.active && !wm->bubble.closing) {      /* still rising: the first click only finishes it */
            bubble_finish(wm);
            return true;
        }
        int pk = wm->press_panel_kind, pi = wm->press_panel_ws;
        if (pressed) {
            if (pick->kind == LM_PICK_LAUNCHER_BACKDROP) {      /* a click outside the card closes it, nothing below gets it */
                launcher_set(wm, false, NULL);
            } else if (pick->kind != LM_PICK_LAUNCHER_CARD && button == BTN_LEFT) {
                wm->press_panel_kind = pick->kind;
                wm->press_panel_ws = pick->index;
                if (wm->srv->seat) {
                    double px, py;
                    lm_seat_pointer_position(wm->srv->seat, &px, &py);
                    wm->drag_press_x = (int)px;
                    wm->drag_press_y = (int)py;
                }
                wm->press_win = NULL;
                wm->press_bits = 0;
            }
        } else {
            wm->press_panel_kind = 0;
            if (wm->launcher_drag)
                launcher_drag_finish(wm);                       /* a drag ends with the button: dropped on the dock or not */
            else if (pk == (int)pick->kind && pi == pick->index && button == BTN_LEFT)
                launcher_activate(wm, pk, pi);                  /* released on what it was pressed on */
        }
        return true;
    }
    if (pick->kind == LM_PICK_ABOUT_BUTTON || pick->kind == LM_PICK_ABOUT_CARD || pick->kind == LM_PICK_ABOUT_BACKDROP) {
        int pk = wm->press_panel_kind;
        if (pressed) {
            if (pick->kind == LM_PICK_ABOUT_BACKDROP)           /* a click outside the card closes it, nothing below gets it */
                about_set(wm, false, NULL);
            else if (pick->kind == LM_PICK_ABOUT_BUTTON && button == BTN_LEFT) {
                wm->press_panel_kind = LM_PICK_ABOUT_BUTTON;
                wm->press_win = NULL;
                wm->press_bits = 0;
            }
        } else {
            wm->press_panel_kind = 0;
            if (pk == LM_PICK_ABOUT_BUTTON && pick->kind == LM_PICK_ABOUT_BUTTON && button == BTN_LEFT)
                about_set(wm, false, NULL);
        }
        return true;
    }
    if (pick->kind == LM_PICK_NOTIFY) {                    /* a click dismisses a notification and goes no further */
        if (pressed)
            note_dismiss(wm, pick->index);
        return true;
    }
    if (pick->kind == LM_PICK_SHOT_CARD) {                 /* nothing under the card gets the click */
        if (pressed && button == BTN_LEFT)
            open_shot(wm);
        else if (pressed)
            lm_render_shot_dismiss(wm->srv->render, NULL, 0);
        return true;
    }
    if (pressed && wm->clip_nav && pick->kind != LM_PICK_CLIP_ENTRY)
        clip_nav_set(wm, false);

    /* Releases of decoration/panel presses we consumed. */
    if (!pressed && (wm->press_bits || wm->press_panel_kind)) {
        struct lm_window *pw = wm->press_win;
        uint32_t pb = wm->press_bits;
        int pk = wm->press_panel_kind, pi = wm->press_panel_ws;
        wm->press_win = NULL;
        wm->press_bits = 0;
        wm->press_panel_kind = 0;
        if (pw)
            damage_frame(wm, pw);
        if (pb && pick->window == pw && bit_for_kind(pick->kind) == pb && pw) {
            switch (pb) {
            case LM_FRAME_HOVER_CLOSE: lm_wm_window_close(pw); break;
            case LM_FRAME_HOVER_MAXIMIZE:
                lm_wm_window_request_maximize(pw, !(pw->states & LM_WINDOW_MAXIMIZED));
                break;
            case LM_FRAME_HOVER_MINIMIZE: lm_wm_window_request_minimize(pw); break;
            }
        }
        if (pk == LM_PICK_PANEL_LAUNCHER && pick->kind == LM_PICK_PANEL_LAUNCHER)
            about_set(wm, true, lm_output_nearest(wm->srv, pick->rect.x, pick->rect.y));
        if (pk == LM_PICK_DOCK_ITEM && wm->dock_drag)
            dock_drag_finish(wm);                               /* it was dragged: moved or taken out, not clicked */
        else if (pk == LM_PICK_DOCK_ITEM && pick->kind == LM_PICK_DOCK_ITEM && pick->index == pi) {
            lm_sound_note(LM_SOUND_DOCK);                       /* our own click sound (not on the headless backend, where the tests only look at the record) */
            if (wm->srv->config.sound && (strcmp(wm->srv->config.backend, "headless") != 0 || getenv("MELLAGUI_SOUND_FORCE")))
                lm_sound_play(LM_SOUND_DOCK);                       /* (MELLAGUI_SOUND_FORCE: the tests play it on the headless backend, too) */
            dock_activate(wm, pi, button != BTN_MIDDLE);        /* the middle button always starts another instance */
        }
        if (pk == LM_PICK_PANEL_CLOCK && pick->kind == LM_PICK_PANEL_CLOCK)
            lm_wm_calendar_toggle(wm, &pick->rect);
        if (pk == LM_PICK_PANEL_LAYOUT && pick->kind == LM_PICK_PANEL_LAYOUT && wm->srv->input)
            lm_input_cycle_layout(wm->srv->input, pi);
        if (pk == LM_PICK_PANEL_OSK && pick->kind == LM_PICK_PANEL_OSK)
            osk_set(wm, !wm->osk_open);
        if (pk == LM_PICK_PANEL_QUICK && pick->kind == LM_PICK_PANEL_QUICK)
            lm_wm_quick_toggle(wm, &pick->rect);
        if (pk == LM_PICK_PANEL_STICKY && pick->kind == LM_PICK_PANEL_STICKY && wm->srv->input)
            lm_input_set_sticky(wm->srv->input, !lm_input_sticky(wm->srv->input));
        if (pk == LM_PICK_PANEL_TRAY && pick->kind == LM_PICK_PANEL_TRAY && pick->index == pi)
            lm_tray_click(wm->srv->tray, pi, button);               /* pressed and let go on the same item */
        if (pk == LM_PICK_PANEL_POWER && pick->kind == LM_PICK_PANEL_POWER) {
            double px = 0, py = 0;
            if (wm->srv->seat)
                lm_seat_pointer_position(wm->srv->seat, &px, &py);
            power_set(wm, true, lm_output_nearest(wm->srv, px, py));        /* on the output whose panel was clicked */
        }
        return true;
    }
    if (!pressed)
        return false;

    switch (pick->kind) {
    case LM_PICK_SURFACE:
        if (pick->window && wm->focused != pick->window)
            lm_wm_focus_window(wm, pick->window);
        else if (pick->window)
            raise_window(wm, pick->window);
        return false;
    case LM_PICK_CLOSE:
    case LM_PICK_MAXIMIZE:
    case LM_PICK_MINIMIZE:
        if (button != BTN_LEFT)
            return true;
        lm_wm_focus_window(wm, pick->window);
        wm->press_win = pick->window;
        wm->press_bits = bit_for_kind(pick->kind);
        damage_frame(wm, pick->window);
        return true;
    case LM_PICK_TITLEBAR: {
        if (!pick->window)
            return true;
        lm_wm_focus_window(wm, pick->window);
        if (button == BTN_RIGHT) {
            window_menu_open(wm, pick->window);
            return true;
        }
        if (button != BTN_LEFT)
            return true;
        if (wm->last_click_win == pick->window && time_ms - wm->last_click_time < (uint32_t)DOUBLE_CLICK_MS) {
            wm->last_click_win = NULL;
            lm_wm_window_request_maximize(pick->window, !(pick->window->states & LM_WINDOW_MAXIMIZED));
            return true;
        }
        wm->last_click_win = pick->window;
        wm->last_click_time = time_ms;
        grab_begin(wm, pick->window, GRAB_MOVE, 0);
        return true;
    }
    case LM_PICK_BORDER:
        if (!pick->window)
            return true;
        lm_wm_focus_window(wm, pick->window);
        if (button == BTN_LEFT)
            grab_begin(wm, pick->window, GRAB_RESIZE, pick->edges);
        return true;
    case LM_PICK_GADGET_TITLE: {
        int id = pick->index;
        if (button != BTN_LEFT || id < 0 || id >= LM_GADGET_COUNT)
            return true;
        if (time_ms - wm->gad[id].click_time < (uint32_t)DOUBLE_CLICK_MS) {
            wm->gad[id].click_time = 0;
            gadget_reset(wm, id);
            return true;
        }
        wm->gad[id].click_time = time_ms;
        gadget_grab_begin(wm, id, GRAB_GADGET_MOVE, 0);
        return true;
    }
    case LM_PICK_GADGET_BORDER:
        if (button == BTN_LEFT && pick->index >= 0 && pick->index < LM_GADGET_COUNT)
            gadget_grab_begin(wm, pick->index, GRAB_GADGET_RESIZE, pick->edges);
        return true;
    case LM_PICK_GADGET_BODY:
        if (button == BTN_LEFT)
            mixer_press(wm);
        return true;
    case LM_PICK_CLIP_ENTRY:
        if (button == BTN_LEFT)
            lm_seat_clip_use(wm->srv->seat, pick->index);
        return true;
    case LM_PICK_CLIP_DELETE:
        if (button == BTN_LEFT)
            lm_seat_clip_remove(wm->srv->seat, pick->index);
        return true;
    case LM_PICK_CLIP_UP:
    case LM_PICK_CLIP_DOWN:
        if (button == BTN_LEFT) {                                       /* a page at a time */
            int slots = LM_MAX(clip_slots(wm), 1);
            wm->clip_top = clip_top(wm) + (pick->kind == LM_PICK_CLIP_UP ? -slots : slots);
            wm->clip_top = clip_top(wm);
            if (wm->clip_nav)
                wm->clip_cur = LM_CLAMP(wm->clip_cur, wm->clip_top, wm->clip_top + slots - 1);
            damage_gadget(wm, LM_GADGET_CLIPBOARD);
        }
        return true;
    case LM_PICK_CLIP_CLEAR:
        if (button == BTN_LEFT)
            lm_seat_clip_clear(wm->srv->seat);
        return true;
    case LM_PICK_DOCK_ITEM:
        if (button == BTN_RIGHT) {
            dockitem_menu_open(wm, pick->index);
            return true;
        }
        if (button == BTN_LEFT || button == BTN_MIDDLE) {
            wm->press_panel_kind = LM_PICK_DOCK_ITEM;
            wm->press_panel_ws = pick->index;
            wm->dock_press_left = button == BTN_LEFT;
            if (wm->srv->seat) {
                double px, py;
                lm_seat_pointer_position(wm->srv->seat, &px, &py);
                wm->drag_press_x = (int)px;
                wm->drag_press_y = (int)py;
            }
            wm->press_win = NULL;
            wm->press_bits = 0;
        }
        return true;
    case LM_PICK_DOCK:
        return true;
    case LM_PICK_PANEL_LAUNCHER:
        if (button == BTN_LEFT) {
            wm->press_panel_kind = LM_PICK_PANEL_LAUNCHER;
            wm->press_win = NULL;
            wm->press_bits = 0;
        }
        return true;
    case LM_PICK_PANEL_CLOCK:
        if (button == BTN_LEFT) {
            wm->press_panel_kind = pick->kind;
            wm->cal_clock = pick->rect;
            wm->press_win = NULL;
            wm->press_bits = 0;
        }
        return true;
    case LM_PICK_PANEL_POWER:
        if (button == BTN_LEFT) {
            wm->press_panel_kind = pick->kind;
            wm->press_win = NULL;
            wm->press_bits = 0;
        }
        return true;
    case LM_PICK_PANEL_LAYOUT:
    case LM_PICK_PANEL_STICKY:
    case LM_PICK_PANEL_OSK:
    case LM_PICK_PANEL_QUICK:
        if (button == BTN_LEFT || (button == BTN_RIGHT && pick->kind == LM_PICK_PANEL_LAYOUT)) {
            wm->press_panel_kind = pick->kind;
            wm->press_panel_ws = button == BTN_RIGHT ? -1 : 1;     /* layout direction, see the release code */
            wm->press_win = NULL;
            wm->press_bits = 0;
        }
        return true;
    case LM_PICK_PANEL_TRAY:
        if (button == BTN_LEFT || button == BTN_RIGHT || button == BTN_MIDDLE) {
            wm->press_panel_kind = LM_PICK_PANEL_TRAY;
            wm->press_panel_ws = pick->index;
            wm->press_win = NULL;
            wm->press_bits = 0;
        }
        return true;
    case LM_PICK_PANEL_WORKSPACE:
        if (button == BTN_LEFT)
            lm_wm_switch_workspace(wm, pick->index);
        return true;
    case LM_PICK_PANEL_TASK:
        if (button == BTN_LEFT && pick->window) {
            if (pick->window == wm->focused && !pick->window->minimized)
                lm_wm_window_request_minimize(pick->window);
            else
                lm_wm_focus_window(wm, pick->window);
        } else if (button == BTN_MIDDLE && pick->window) {
            lm_wm_window_close(pick->window);
        } else if (button == BTN_RIGHT && pick->window) {
            window_menu_open(wm, pick->window);
        }
        return true;
    case LM_PICK_PANEL:
        return true;
    case LM_PICK_NONE:
    default:
        return false;
    }
}
