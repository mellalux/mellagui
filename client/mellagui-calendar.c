/*
 * mellagui-calendar - the calendar that a click on the panel's clock shows.
 *
 *   mellagui-calendar              runs for the whole session: the compositor starts it with itself, its window is hidden (minimized, not
 *                                  in the taskbar) and a click on the clock shows it under the clock; it hides when it loses the focus
 *   mellagui-calendar --selftest   the calendar arithmetic (days in a month, weekday, ISO week) without a display
 *
 * The window shows the time and the date as the clock settings say (clock-hours, clock-seconds, date-format ...; read from the
 * compositor's mellagui_settings_v1 and followed when they change), a month with today marked, optional ISO week numbers (cal-weeks)
 * and the first weekday of the week (cal-week-start). Mouse: the arrows (or the wheel) change the month, a click on a day selects it
 * (a day of the next / previous month goes there), "Today" comes back. Keys: Left / Right / PgUp / PgDn month, Up / Down year, Home or T today,
 * Esc or Q hide. Opening it always starts at the current month.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "clockfmt.h"
#include "font.h"
#include "keymap.h"
#include "loop.h"
#include "protocol.h"
#include "theme.h"
#include "util.h"
#include "wlc.h"
#include "uikit.h"
#include "wlwin.h"

#define PAD 12
#define CELL_W 38
#define CELL_H 28
#define WK_W 34
#define WIN_W (2 * PAD + WK_W + 7 * CELL_W)
#define TIME_Y 10
#define DATE_Y 50
#define NAV_Y 76
#define HEAD_Y 112
#define GRID_Y 132
#define TODAY_Y (GRID_Y + 6 * CELL_H + 8)
#define WIN_H (TODAY_Y + 26 + PAD)
#define ARROW_W 30
#define BTN_H 26

struct app {
    struct wlwin win;
    uint32_t settings;
    struct lm_clockfmt fmt;
    int week_start;                     /* 1 Monday, 0 Sunday */
    bool weeknum;
    int vy, vm;                         /* the month shown (full year, 0..11) */
    int sy, sm, sd;                     /* the selected day, sd == 0: none */
    int ty, tm, td;                     /* today */
    char stamp[96];                     /* what was drawn last (time + date), to repaint when it changes */
    double px, py;
    bool has_pointer;
    int hover;                          /* 0 none, 1 prev, 2 next, 3 today button, 100 + cell */
};

/* ---- time ------------------------------------------------------------------------------------------------------------------ */

static void now_tm(struct tm *tm)
{
    time_t t = time(NULL);
    localtime_r(&t, tm);
}

static void refresh_today(struct app *a)
{
    struct tm tm;
    now_tm(&tm);
    a->ty = tm.tm_year + 1900;
    a->tm = tm.tm_mon;
    a->td = tm.tm_mday;
}

static void go_today(struct app *a)
{
    refresh_today(a);
    a->vy = a->ty;
    a->vm = a->tm;
    a->sd = 0;
    a->win.redraw = true;
}

static void shift_month(struct app *a, int d)
{
    int m = a->vm + d, y = a->vy;
    while (m < 0) { m += 12; y--; }
    while (m > 11) { m -= 12; y++; }
    if (y < 1 || y > 9999)
        return;
    a->vy = y;
    a->vm = m;
    a->win.redraw = true;
}

static void stamp_now(const struct app *a, char *out, size_t n)
{
    struct tm tm;
    char t[32], d[48];
    now_tm(&tm);
    lm_clock_time_text(&a->fmt, &tm, t, sizeof(t));
    lm_clock_date_text(&a->fmt, &tm, d, sizeof(d));
    snprintf(out, n, "%s|%s|%d", t, d, a->td);
}

/* ms until the clock text changes (the next second when seconds are shown, else the next minute). */
static int tick_timeout(const struct app *a)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    long ms = ts.tv_nsec / 1000000;
    if (a->fmt.seconds)
        return (int)(1000 - ms) + 5;
    return (int)((59 - ts.tv_sec % 60) * 1000 + (1000 - ms)) + 5;
}

/* ---- the month grid ----------------------------------------------------------------------------------------------------- */

struct cell {
    int y, m, d;                        /* the day this cell shows (full year, month 0..11) */
    bool other;                         /* belongs to the month before / after the one shown */
};

static void cell_for(const struct app *a, int idx, struct cell *c)
{
    int offset = (lm_first_weekday(a->vy, a->vm) - a->week_start + 7) % 7;
    int d = idx - offset + 1, dim = lm_days_in_month(a->vy, a->vm);
    c->y = a->vy;
    c->m = a->vm;
    c->other = false;
    if (d < 1) {
        c->m--;
        if (c->m < 0) { c->m = 11; c->y--; }
        d += lm_days_in_month(c->y, c->m);
        c->other = true;
    } else if (d > dim) {
        d -= dim;
        c->m++;
        if (c->m > 11) { c->m = 0; c->y++; }
        c->other = true;
    }
    c->d = d;
}

static int weekday_col(const struct app *a, int idx)
{
    return (a->week_start + idx % 7) % 7;               /* 0 = Sunday */
}

static int cell_x(int col) { return PAD + WK_W + col * CELL_W; }
static int cell_y(int row) { return GRID_Y + row * CELL_H; }

static bool in_rect(double x, double y, int rx, int ry, int rw, int rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static int prev_x(void) { return PAD; }
static int next_x(void) { return WIN_W - PAD - ARROW_W; }
static int today_x(void) { return (WIN_W - 100) / 2; }

static int hit(const struct app *a, double x, double y)
{
    if (in_rect(x, y, prev_x(), NAV_Y, ARROW_W, BTN_H)) return 1;
    if (in_rect(x, y, next_x(), NAV_Y, ARROW_W, BTN_H)) return 2;
    if (in_rect(x, y, today_x(), TODAY_Y, 100, BTN_H)) return 3;
    (void)a;
    for (int i = 0; i < 42; i++)
        if (in_rect(x, y, cell_x(i % 7), cell_y(i / 7), CELL_W, CELL_H))
            return 100 + i;
    return 0;
}

/* ---- painting -------------------------------------------------------------------------------------------------------------- */

static void text_center(struct wlwin *w, int cx, int y, const char *s, int scale, uint32_t rgb)
{
    wlwin_text(w, cx - lm_font_text_width(s) * scale / 2, y, s, scale, rgb, 0);
}

static void button(struct app *a, int x, int y, int w, const char *label, bool hover)
{
    wlwin_drop(&a->win, x, y, w, BTN_H, lm_button_radius(BTN_H / 2), hover ? lm_theme.button_hover : lm_theme.button);
    text_center(&a->win, x + w / 2, y + (BTN_H - LM_FONT_H) / 2, label, 1, lm_theme.text);
}

static void paint(struct wlwin *w)
{
    struct app *a = w->data;
    refresh_today(a);
    wlwin_fill(w, 0, 0, w->bw, w->bh, lm_theme.background);

    struct tm tm;
    char t[32], d[64], title[48];
    now_tm(&tm);
    lm_clock_time_text(&a->fmt, &tm, t, sizeof(t));
    text_center(w, w->bw / 2, TIME_Y, t, 2, lm_theme.text);

    /* the date line: the selected day, else today, with the full weekday */
    struct lm_clockfmt df = a->fmt;
    df.weekday = false;
    int y = a->sd ? a->sy : a->ty, m = a->sd ? a->sm : a->tm, day = a->sd ? a->sd : a->td;
    struct tm dt = { .tm_year = y - 1900, .tm_mon = m, .tm_mday = day };
    char date[40];
    lm_clock_date_text(&df, &dt, date, sizeof(date));
    int wd = (lm_first_weekday(y, m) + day - 1) % 7;
    snprintf(d, sizeof(d), "%s %s", lm_weekday_name(wd, true), date);
    text_center(w, w->bw / 2, DATE_Y, d, 1, a->sd ? lm_theme.accent : lm_theme.muted);

    /* month navigation */
    button(a, prev_x(), NAV_Y, ARROW_W, "<", a->hover == 1);
    button(a, next_x(), NAV_Y, ARROW_W, ">", a->hover == 2);
    snprintf(title, sizeof(title), "%s %04d", lm_month_name(a->vm, true), a->vy);
    text_center(w, w->bw / 2, NAV_Y + (BTN_H - LM_FONT_H) / 2, title, 1, lm_theme.text);

    /* weekday names */
    for (int c = 0; c < 7; c++) {
        char nm[4];
        snprintf(nm, sizeof(nm), "%.2s", lm_weekday_name(weekday_col(a, c), false));
        text_center(w, cell_x(c) + CELL_W / 2, HEAD_Y, nm, 1, lm_theme.text_dim);
    }
    if (a->weeknum)
        text_center(w, PAD + WK_W / 2, HEAD_Y, "Wk", 1, lm_theme.text_dim);
    wlwin_fill(w, PAD, HEAD_Y + LM_FONT_H + 2, WIN_W - 2 * PAD, 1, lm_theme.border);

    /* the days */
    for (int i = 0; i < 42; i++) {
        struct cell c;
        cell_for(a, i, &c);
        int x = cell_x(i % 7), yy = cell_y(i / 7);
        bool today = c.y == a->ty && c.m == a->tm && c.d == a->td;
        bool sel = a->sd && c.y == a->sy && c.m == a->sm && c.d == a->sd;
        int col = weekday_col(a, i % 7);
        uint32_t fg = c.other ? lm_theme.text_dim : (col == 0 || col == 6) ? lm_theme.muted : lm_theme.text;
        if (a->hover == 100 + i)
            wlwin_fill(w, x + 1, yy + 1, CELL_W - 2, CELL_H - 2, lm_theme.button_hover);
        if (today) {
            wlwin_fill(w, x + 1, yy + 1, CELL_W - 2, CELL_H - 2, lm_theme.accent);
            fg = lm_theme.background;
        }
        if (sel) {
            wlwin_fill(w, x + 1, yy + 1, CELL_W - 2, 2, lm_theme.accent);
            wlwin_fill(w, x + 1, yy + CELL_H - 3, CELL_W - 2, 2, lm_theme.accent);
            wlwin_fill(w, x + 1, yy + 1, 2, CELL_H - 2, lm_theme.accent);
            wlwin_fill(w, x + CELL_W - 3, yy + 1, 2, CELL_H - 2, lm_theme.accent);
        }
        char num[8];
        snprintf(num, sizeof(num), "%d", c.d);
        text_center(w, x + CELL_W / 2, yy + (CELL_H - LM_FONT_H) / 2, num, 1, fg);
        if (a->weeknum && i % 7 == 0) {
            /* the week of the row's Thursday (ISO: the week belongs to the year that holds its Thursday) */
            struct cell th;
            int thu = (4 - a->week_start + 7) % 7;
            cell_for(a, i + thu, &th);
            char wk[8];
            snprintf(wk, sizeof(wk), "%d", lm_iso_week(th.y, th.m, th.d));
            text_center(w, PAD + WK_W / 2, yy + (CELL_H - LM_FONT_H) / 2, wk, 1, lm_theme.text_dim);
        }
    }
    button(a, today_x(), TODAY_Y, 100, "Today", a->hover == 3);
    stamp_now(a, a->stamp, sizeof(a->stamp));
}

/* ---- events ---------------------------------------------------------------------------------------------------------------- */

static void hide(struct app *a)
{
    if (a->win.toplevel)
        WLC_SEND0(a->win.c, a->win.toplevel, LM_XDG_TOPLEVEL_REQ_SET_MINIMIZED);
}

static bool win_close(struct wlwin *w)
{
    hide(w->data);
    return false;                                       /* the calendar lives on, hidden */
}

static void click(struct app *a, int h)
{
    if (h == 1) shift_month(a, -1);
    else if (h == 2) shift_month(a, 1);
    else if (h == 3) go_today(a);
    else if (h >= 100) {
        struct cell c;
        cell_for(a, h - 100, &c);
        a->sy = c.y;
        a->sm = c.m;
        a->sd = c.d;
        if (c.other) {
            a->vy = c.y;
            a->vm = c.m;
        }
        a->win.redraw = true;
    }
}

static void pointer_event(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
    struct app *a = w->data;
    int h;
    switch (op) {
    case LM_WL_POINTER_EVT_ENTER:
        a->has_pointer = true;
        a->px = lm_fixed_to_double(args[2].f);
        a->py = lm_fixed_to_double(args[3].f);
        h = hit(a, a->px, a->py);
        if (h != a->hover) { a->hover = h; w->redraw = true; }
        break;
    case LM_WL_POINTER_EVT_LEAVE:
        a->has_pointer = false;
        if (a->hover) { a->hover = 0; w->redraw = true; }
        break;
    case LM_WL_POINTER_EVT_MOTION:
        a->px = lm_fixed_to_double(args[1].f);
        a->py = lm_fixed_to_double(args[2].f);
        h = hit(a, a->px, a->py);
        if (h != a->hover) { a->hover = h; w->redraw = true; }
        break;
    case LM_WL_POINTER_EVT_BUTTON:
        if (args[2].u == 272 && args[3].u == 1)         /* BTN_LEFT pressed */
            click(a, hit(a, a->px, a->py));
        break;
    case LM_WL_POINTER_EVT_AXIS:
        if (args[1].u == 0) {
            double v = lm_fixed_to_double(args[2].f);
            if (v != 0)
                shift_month(a, v < 0 ? -1 : 1);
        }
        break;
    }
}

static void keyboard_event(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
    struct app *a = w->data;
    if (op == LM_WL_KEYBOARD_EVT_ENTER) {               /* it was just shown: start at today again */
        go_today(a);
        return;
    }
    if (op != LM_WL_KEYBOARD_EVT_KEY || args[3].u == 0)
        return;
    uint32_t ks = lm_keymap_keysym(args[2].u, a->win.mods);
    char txt[8];
    int tl = lm_keysym_to_utf8(ks, txt);
    char c = (tl == 1 && !(a->win.mods & (LM_MOD_CTRL | LM_MOD_ALT | LM_MOD_SUPER))) ? txt[0] : 0;
    if (ks == LM_KS_Escape || c == 'q' || c == 'Q')
        hide(a);
    else if (ks == LM_KS_Left || ks == LM_KS_Page_Up)
        shift_month(a, -1);
    else if (ks == LM_KS_Right || ks == LM_KS_Page_Down)
        shift_month(a, 1);
    else if (ks == LM_KS_Up)
        shift_month(a, -12);
    else if (ks == LM_KS_Down)
        shift_month(a, 12);
    else if (ks == LM_KS_Home || c == 't' || c == 'T')
        go_today(a);
}

static void settings_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)c; (void)id;
    struct app *a = data;
    if (op != LM_MELLAGUI_SETTINGS_V1_EVT_VALUE)
        return;
    const char *k = args[0].s, *v = args[1].s;
    bool on = !strcmp(v, "1");
    if (!strcmp(k, "clock-hours")) a->fmt.h12 = !strcmp(v, "12");
    else if (!strcmp(k, "clock-seconds")) a->fmt.seconds = on;
    else if (!strcmp(k, "clock-date")) a->fmt.show_date = on;
    else if (!strcmp(k, "clock-weekday")) a->fmt.weekday = on;
    else if (!strcmp(k, "date-format")) {
        int f = lm_date_format_parse(v);
        if (f >= 0)
            a->fmt.date_fmt = f;
    } else if (!strcmp(k, "cal-week-start")) a->week_start = strcmp(v, "sun") ? 1 : 0;
    else if (!strcmp(k, "cal-weeks")) a->weeknum = on;
    else
        return;
    a->win.redraw = true;
}

static void global_event(struct wlwin *w, uint32_t name, const char *iface, uint32_t ver)
{
    (void)ver;
    struct app *a = w->data;
    if (!strcmp(iface, "mellagui_settings_v1"))
        a->settings = wlc_bind(w->c, w->registry, name, &lm_mellagui_settings_v1_interface, 1, settings_event, a);
}

/* ---- selftest ---------------------------------------------------------------------------------------------------------------- */

static int check(bool ok, const char *what)
{
    if (!ok)
        fprintf(stderr, "mellagui-calendar: selftest failed: %s\n", what);
    return ok ? 0 : 1;
}

static int selftest(void)
{
    int bad = 0;
    bad += check(lm_days_in_month(2024, 1) == 29 && lm_days_in_month(2026, 1) == 28 && lm_days_in_month(1900, 1) == 28 &&
                 lm_days_in_month(2000, 1) == 29 && lm_days_in_month(2026, 10) == 30, "days in month");
    bad += check(lm_first_weekday(2026, 10) == 0, "1 Nov 2026 is a Sunday");
    bad += check(lm_first_weekday(2000, 0) == 6, "1 Jan 2000 is a Saturday");
    bad += check(lm_first_weekday(2024, 1) == 4, "1 Feb 2024 is a Thursday");
    bad += check(lm_iso_week(2026, 10, 10) == 46, "10 Nov 2026 is in week 46");
    bad += check(lm_iso_week(2026, 0, 1) == 1, "1 Jan 2026 is in week 1");
    bad += check(lm_iso_week(2021, 0, 1) == 53, "1 Jan 2021 is in week 53 of 2020");
    bad += check(lm_iso_week(2020, 11, 31) == 53, "31 Dec 2020 is in week 53");
    bad += check(lm_iso_week(2024, 11, 30) == 1, "30 Dec 2024 is in week 1 of 2025");
    bad += check(lm_iso_week(2015, 11, 31) == 53, "31 Dec 2015 is in week 53");
    struct lm_clockfmt f = { .show_date = true };
    struct tm tm = { .tm_year = 126, .tm_mon = 10, .tm_mday = 10, .tm_hour = 14, .tm_min = 5, .tm_sec = 9, .tm_wday = 2 };
    char buf[48];
    lm_clock_bar_text(&f, &tm, buf, sizeof(buf));
    bad += check(!strcmp(buf, "10.11.2026 14:05"), "default bar text");
    f.h12 = true; f.seconds = true; f.weekday = true; f.date_fmt = LM_DATE_LONG;
    lm_clock_bar_text(&f, &tm, buf, sizeof(buf));
    bad += check(!strcmp(buf, "Tue 10 Nov 2026 2:05:09 PM"), "12 hour bar text");
    tm.tm_hour = 0;
    lm_clock_time_text(&f, &tm, buf, sizeof(buf));
    bad += check(!strcmp(buf, "12:05:09 AM"), "midnight");
    if (!bad)
        printf("mellagui-calendar selftest ok\n");
    return bad ? 1 : 0;
}

int main(int argc, char **argv)
{
    wlwin_hidpi();                      /* draws through the wlwin_* calls only: sharp on a scaled screen */
    lm_theme_load_env();
    if (argc > 1 && !strcmp(argv[1], "--selftest"))
        return selftest();

    static const struct wlwin_ops ops = { .paint = paint, .global = global_event, .pointer = pointer_event, .keyboard = keyboard_event,
                                          .close = win_close };
    struct app a = { .week_start = 1, .fmt = { .show_date = true } };
    int rc = 1;
    if (wlwin_connect(&a.win, "mellagui-calendar", &ops, &a) < 0)
        return 1;
    if (a.settings && wlc_roundtrip(a.win.c) < 0)               /* the values of the settings, before the first picture */
        goto out;
    go_today(&a);
    if (wlwin_open(&a.win, "Calendar", "mellagui.calendar", WIN_W, WIN_H, WIN_W, WIN_H) < 0)
        goto out;
    a.win.fixed_size = false;
    for (;;) {
        if (!wlwin_step(&a.win, tick_timeout(&a)))
            break;
        char now[96];
        stamp_now(&a, now, sizeof(now));
        if (strcmp(now, a.stamp)) {                             /* the minute / second turned, maybe midnight */
            refresh_today(&a);
            a.win.redraw = true;
        }
    }
    rc = 0;
out:
    if (a.win.c)
        wlwin_destroy(&a.win);
    return rc;
}
