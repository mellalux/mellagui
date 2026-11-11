/*
 * mellagui-timer: a pomodoro timer, a countdown timer and a stopwatch in one small window.
 *
 *   mellagui-timer              the window (keys: Space start / pause, R reset, 1 2 3 or Tab the three parts, N skip to the next pomodoro phase,
 *                               L a lap, + / - a minute more / less on the timer, Esc / Q / the close button hide it - it keeps running while a timer runs)
 *   mellagui-timer --selftest   checks the arithmetic without a display
 *
 * Pomodoro: 25 minutes of work, 5 of rest, a long rest of 15 after the fourth work; the lengths are the lines `work`, `short`, `long` (minutes) and `cycles`
 * of ~/.config/mellagui/timer.conf (`auto = 1` starts the next phase by itself). When a countdown ends the program puts up a notification card and the
 * compositor plays a short tune (the write-only settings `notify` and `timer-beep` of mellagui_settings_v1). While something runs the top bar has an item
 * with the time left; a click on it pauses / continues.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "font.h"
#include "keymap.h"
#include "protocol.h"
#include "theme.h"
#include "uikit.h"
#include "util.h"
#include "wlwin.h"

#define WIN_W 380
#define WIN_H 300
#define PAD 14

enum mode { M_POMODORO, M_TIMER, M_STOPWATCH, M_COUNT };
enum phase { P_WORK, P_SHORT, P_LONG };

struct clockwork {
    bool running;
    int64_t left_ms;                    /* countdowns: what is left when stopped; stopwatch: what has been counted */
    int64_t t0;                         /* running: the monotonic time of the last start */
};

struct app {
    struct wlwin win;
    uint32_t settings, tray, tray_item;
    enum mode mode;
    struct clockwork cw[M_COUNT];
    enum phase phase;
    int done;                           /* pomodoros finished (works) */
    int work_min, short_min, long_min, cycles;
    bool auto_next;
    int timer_set_min;                  /* the countdown timer's length */
    int64_t laps[8];
    int nlaps;
    int hover;
    double px, py;
    char tray_text[32];
    bool had_tray_click;
    int tray_hold;                      /* the clock the bar's item paused (-1 = none): the item stays so that a click can continue it */
};

static int64_t g_minute = 60000;          /* a minute in ms ($MELLAGUI_TIMER_UNIT_MS makes it short: tests) */

/* ---- time ------------------------------------------------------------------------------------------------------------------ */

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* What is left (countdowns) or counted (stopwatch) right now. */
static int64_t cw_value(const struct clockwork *c, bool countdown, int64_t now)
{
    if (!c->running)
        return c->left_ms;
    int64_t d = now - c->t0;
    return countdown ? (c->left_ms - d > 0 ? c->left_ms - d : 0) : c->left_ms + d;
}

static void cw_start(struct clockwork *c, int64_t now)
{
    if (!c->running) {
        c->running = true;
        c->t0 = now;
    }
}

static void cw_stop(struct clockwork *c, bool countdown, int64_t now)
{
    if (c->running) {
        c->left_ms = cw_value(c, countdown, now);
        c->running = false;
    }
}

/* "24:59", or "1:05:09" from an hour on. */
static void fmt_time(int64_t ms, bool round_up, char *out, size_t n)
{
    int64_t s = ms / 1000;
    if (round_up && ms % 1000)
        s++;
    if (s >= 3600)
        snprintf(out, n, "%d:%02d:%02d", (int)(s / 3600), (int)(s / 60 % 60), (int)(s % 60));
    else
        snprintf(out, n, "%02d:%02d", (int)(s / 60), (int)(s % 60));
}

static int64_t phase_ms(const struct app *a, enum phase p)
{
    return (int64_t)(p == P_WORK ? a->work_min : p == P_SHORT ? a->short_min : a->long_min) * g_minute;
}

/* The phase that follows the one that ended (a work that was the `cycles`-th is followed by the long rest). */
static enum phase next_phase(const struct app *a, enum phase p, int done_after)
{
    if (p != P_WORK)
        return P_WORK;
    return done_after % a->cycles == 0 ? P_LONG : P_SHORT;
}

/* ---- the configuration ----------------------------------------------------------------------------------------------------- */

static void load_conf(struct app *a)
{
    a->work_min = 25; a->short_min = 5; a->long_min = 15; a->cycles = 4; a->timer_set_min = 5;
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    char path[1024];
    if (xdg && *xdg)
        snprintf(path, sizeof(path), "%s/mellagui/timer.conf", xdg);
    else if (home && *home)
        snprintf(path, sizeof(path), "%s/.config/mellagui/timer.conf", home);
    else
        return;
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#')
            continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        while (*k == ' ')
            k++;
        for (char *e = k + strlen(k); e > k && (e[-1] == ' ' || e[-1] == '\t'); e--)
            e[-1] = 0;
        int n = atoi(v);
        if (!strcmp(k, "work") && n >= 1 && n <= 180) a->work_min = n;
        else if (!strcmp(k, "short") && n >= 1 && n <= 60) a->short_min = n;
        else if (!strcmp(k, "long") && n >= 1 && n <= 120) a->long_min = n;
        else if (!strcmp(k, "cycles") && n >= 1 && n <= 12) a->cycles = n;
        else if (!strcmp(k, "timer") && n >= 1 && n <= 600) a->timer_set_min = n;
        else if (!strcmp(k, "auto")) a->auto_next = n != 0;
    }
    fclose(f);
}

/* ---- the compositor ------------------------------------------------------------------------------------------------------- */

static void send_set(struct app *a, const char *key, const char *value)
{
    if (a->settings && a->win.c) {
        WLC_SEND(a->win.c, a->settings, LM_MELLAGUI_SETTINGS_V1_REQ_SET, { .s = key }, { .s = value });
        wlc_flush(a->win.c);
    }
}

static void alarm_notice(struct app *a, const char *title, const char *body)
{
    char msg[400];
    snprintf(msg, sizeof(msg), "%s\t%s\t%d", title, body, 12000);
    send_set(a, "notify", msg);
    send_set(a, "timer-beep", "1");
}

static void tray_item_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data);

static void tray_update(struct app *a, int64_t now);

static void tray_item_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)c; (void)id; (void)args;
    struct app *a = data;
    if (op == LM_MELLAGUI_TRAY_ITEM_V1_EVT_CLICKED)
        a->had_tray_click = true;
}

/* ---- actions --------------------------------------------------------------------------------------------------------------- */

static bool countdown_mode(enum mode m) { return m != M_STOPWATCH; }

static void reset_pomodoro(struct app *a)
{
    a->cw[M_POMODORO] = (struct clockwork){ .left_ms = phase_ms(a, a->phase) };
}

static void set_phase(struct app *a, enum phase p, bool start, int64_t now)
{
    a->phase = p;
    a->cw[M_POMODORO] = (struct clockwork){ .left_ms = phase_ms(a, p) };
    if (start)
        cw_start(&a->cw[M_POMODORO], now);
}

static void toggle(struct app *a)
{
    int64_t now = now_ms();
    struct clockwork *c = &a->cw[a->mode];
    if (c->running) {
        cw_stop(c, countdown_mode(a->mode), now);
    } else {
        if (countdown_mode(a->mode) && c->left_ms <= 0)
            c->left_ms = a->mode == M_TIMER ? (int64_t)a->timer_set_min * g_minute : phase_ms(a, a->phase);
        cw_start(c, now);
    }
    a->win.redraw = true;
}

static void reset(struct app *a)
{
    if (a->mode == M_POMODORO) {
        a->phase = P_WORK;
        a->done = 0;
        reset_pomodoro(a);
    } else if (a->mode == M_TIMER) {
        a->cw[M_TIMER] = (struct clockwork){ .left_ms = (int64_t)a->timer_set_min * g_minute };
    } else {
        a->cw[M_STOPWATCH] = (struct clockwork){ 0 };
        a->nlaps = 0;
    }
    a->win.redraw = true;
}

static void skip(struct app *a)
{
    if (a->mode != M_POMODORO)
        return;
    int64_t now = now_ms();
    bool was_running = a->cw[M_POMODORO].running;
    int done = a->done + (a->phase == P_WORK ? 1 : 0);
    if (a->phase == P_WORK)
        a->done = done;
    set_phase(a, next_phase(a, a->phase, done), was_running, now);
    a->win.redraw = true;
}

static void set_timer_minutes(struct app *a, int minutes)
{
    a->timer_set_min = LM_CLAMP(minutes, 1, 600);
    struct clockwork *c = &a->cw[M_TIMER];
    if (!c->running)
        c->left_ms = (int64_t)a->timer_set_min * g_minute;
    a->win.redraw = true;
}

static void lap(struct app *a)
{
    if (a->mode != M_STOPWATCH || !a->cw[M_STOPWATCH].running)
        return;
    int64_t v = cw_value(&a->cw[M_STOPWATCH], false, now_ms());
    if (a->nlaps < 8)
        a->laps[a->nlaps++] = v;
    else {
        memmove(a->laps, a->laps + 1, 7 * sizeof(a->laps[0]));
        a->laps[7] = v;
    }
    a->win.redraw = true;
}

/* A countdown that reached zero: tells the user and goes on. */
static void check_done(struct app *a, int64_t now)
{
    struct clockwork *c = &a->cw[M_POMODORO];
    if (c->running && cw_value(c, true, now) <= 0) {
        enum phase p = a->phase;
        int done = a->done + (p == P_WORK ? 1 : 0);
        a->done = done;
        enum phase np = next_phase(a, p, done);
        char body[96];
        if (p == P_WORK)
            snprintf(body, sizeof(body), "%d minutes of work done. Take a %s: %d minutes.", a->work_min, np == P_LONG ? "long rest" : "short rest",
                     (int)(phase_ms(a, np) / g_minute));
        else
            snprintf(body, sizeof(body), "The rest is over. Next: %d minutes of work.", a->work_min);
        alarm_notice(a, "Pomodoro", body);
        set_phase(a, np, a->auto_next, now);
        a->win.redraw = true;
    }
    c = &a->cw[M_TIMER];
    if (c->running && cw_value(c, true, now) <= 0) {
        alarm_notice(a, "Timer", "Time is up.");
        *c = (struct clockwork){ .left_ms = (int64_t)a->timer_set_min * g_minute };
        a->win.redraw = true;
    }
}

static bool anything_runs(const struct app *a)
{
    for (int i = 0; i < M_COUNT; i++)
        if (a->cw[i].running)
            return true;
    return false;
}

/* ---- the top bar's item ------------------------------------------------------------------------------------------------------ */

static void tray_update(struct app *a, int64_t now)
{
    if (!a->tray)
        return;
    int run = -1;                                       /* the one the item shows: the pomodoro, else the timer, else the stopwatch */
    for (int i = 0; i < M_COUNT; i++)
        if (a->cw[i].running) {
            run = i;
            break;
        }
    bool paused = false;
    if (run < 0 && a->tray_hold >= 0) {                 /* paused from the bar: still shown (a reset or a finished countdown clears t0) */
        if (a->cw[a->tray_hold].t0 != 0) {
            run = a->tray_hold;
            paused = true;
        } else {
            a->tray_hold = -1;
        }
    }
    if (run < 0) {
        if (a->tray_item) {
            WLC_SEND0(a->win.c, a->tray_item, LM_MELLAGUI_TRAY_ITEM_V1_REQ_DESTROY);
            wlc_forget(a->win.c, a->tray_item);
            a->tray_item = 0;
            a->tray_text[0] = 0;
        }
        return;
    }
    if (!a->tray_item) {
        a->tray_item = wlc_alloc(a->win.c, &lm_mellagui_tray_item_v1_interface, tray_item_event, a);
        WLC_SEND(a->win.c, a->tray, LM_MELLAGUI_TRAY_V1_REQ_CREATE_ITEM, { .n = a->tray_item });
        a->tray_text[0] = 0;
    }
    char t[24], text[32];
    fmt_time(cw_value(&a->cw[run], countdown_mode((enum mode)run), now), countdown_mode((enum mode)run), t, sizeof(t));
    snprintf(text, sizeof(text), "%s%s%s", paused ? "|| " : "", run == M_POMODORO ? (a->phase == P_WORK ? "W " : "R ") : run == M_TIMER ? "T " : "S ", t);
    if (strcmp(text, a->tray_text)) {
        snprintf(a->tray_text, sizeof(a->tray_text), "%s", text);
        WLC_SEND(a->win.c, a->tray_item, LM_MELLAGUI_TRAY_ITEM_V1_REQ_SET_TEXT, { .s = text });
        wlc_flush(a->win.c);
    }
}

/* ---- layout and drawing --------------------------------------------------------------------------------------------------- */

struct rc { int x, y, w, h; };

enum hit { H_TAB0, H_TAB1, H_TAB2, H_START, H_RESET, H_AUX, H_PRE0, H_PRE1, H_PRE2, H_PRE3, H_PRE4, H_PRE5, H_MINUS, H_PLUS, H_COUNT };
static const int presets[6] = { 1, 5, 10, 15, 25, 60 };

static struct rc rect_of(int h)
{
    int tabw = (WIN_W - 2 * PAD - 16) / 3;
    int by = 190, bw = (WIN_W - 2 * PAD - 16) / 3;
    if (h <= H_TAB2)
        return (struct rc){ PAD + h * (tabw + 8), 12, tabw, 28 };
    if (h == H_START)
        return (struct rc){ PAD, by, bw, 34 };
    if (h == H_RESET)
        return (struct rc){ PAD + bw + 8, by, bw, 34 };
    if (h == H_AUX)
        return (struct rc){ PAD + 2 * (bw + 8), by, bw, 34 };
    if (h >= H_PRE0 && h <= H_PRE5) {
        int pw = (WIN_W - 2 * PAD - 5 * 6) / 6;
        return (struct rc){ PAD + (h - H_PRE0) * (pw + 6), 238, pw, 26 };
    }
    if (h == H_MINUS)
        return (struct rc){ PAD, 100, 34, 34 };
    return (struct rc){ WIN_W - PAD - 34, 100, 34, 34 };                              /* H_PLUS */
}

static bool shown(const struct app *a, int h)
{
    if (h >= H_PRE0 && h <= H_PRE5)
        return a->mode == M_TIMER;
    if (h == H_MINUS || h == H_PLUS)
        return a->mode == M_TIMER;
    return true;
}

static int hit_test(const struct app *a, double x, double y)
{
    for (int h = 0; h < H_COUNT; h++) {
        struct rc r = rect_of(h);
        if (shown(a, h) && x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h)
            return h;
    }
    return -1;
}

static void button(struct app *a, int h, const char *text, bool on, uint32_t on_rgb)
{
    struct wlwin *w = &a->win;
    struct rc r = rect_of(h);
    uint32_t bg = on ? on_rgb : a->hover == h ? lm_theme.button_hover : lm_theme.button;
    lm_round_fill(w->pixels, w->bw, w->bw, w->bh, r.x, r.y, r.w, r.h, lm_button_radius(r.h / 2), bg);
    wlwin_text(w, r.x + (r.w - lm_font_text_width(text)) / 2, r.y + (r.h - LM_FONT_H) / 2, text, 1, on ? lm_theme.ink : lm_theme.text, r.w - 6);
}

static void paint(struct wlwin *w)
{
    struct app *a = w->data;
    int64_t now = now_ms();
    wlwin_fill(w, 0, 0, w->bw, w->bh, lm_theme.background);
    static const char *const tabs[M_COUNT] = { "Pomodoro", "Timer", "Stopwatch" };
    for (int i = 0; i < M_COUNT; i++)
        button(a, H_TAB0 + i, tabs[i], a->mode == (enum mode)i, lm_theme.accent);
    struct clockwork *c = &a->cw[a->mode];
    bool down = countdown_mode(a->mode);
    int64_t v = cw_value(c, down, now);
    char t[24];
    fmt_time(v, down, t, sizeof(t));
    int scale = strlen(t) > 5 ? 4 : 6;
    int tw = lm_font_text_width(t) * scale;
    uint32_t col = a->mode == M_POMODORO && a->phase != P_WORK ? lm_theme.ok : c->running ? lm_theme.text : lm_theme.text_dim;
    wlwin_text(w, (WIN_W - tw) / 2, 78, t, scale, col, 0);
    char status[96];
    if (a->mode == M_POMODORO)
        snprintf(status, sizeof(status), "%s  -  pomodoro %d of %d", a->phase == P_WORK ? "Work" : a->phase == P_SHORT ? "Short rest" : "Long rest",
                 a->phase == P_LONG ? a->cycles : a->done % a->cycles + (a->phase == P_WORK ? 1 : 0), a->cycles);
    else if (a->mode == M_TIMER)
        snprintf(status, sizeof(status), "Countdown of %d minute%s", a->timer_set_min, a->timer_set_min == 1 ? "" : "s");
    else
        snprintf(status, sizeof(status), "%s", c->running ? "Counting" : v ? "Stopped" : "Ready");
    wlwin_text(w, (WIN_W - lm_font_text_width(status)) / 2, 160, status, 1, lm_theme.text_dim, WIN_W - 2 * PAD);
    if (down) {                                          /* a bar of what is gone */
        int64_t total = a->mode == M_POMODORO ? phase_ms(a, a->phase) : (int64_t)a->timer_set_min * g_minute;
        int bw = WIN_W - 2 * PAD, fill = total > 0 ? (int)(bw * (total - v) / total) : 0;
        wlwin_fill(w, PAD, 176, bw, 4, lm_theme.button);
        wlwin_fill(w, PAD, 176, LM_CLAMP(fill, 0, bw), 4, a->mode == M_POMODORO && a->phase != P_WORK ? lm_theme.ok : lm_theme.accent);
    }
    button(a, H_START, c->running ? "Pause" : "Start", c->running, lm_theme.accent);
    button(a, H_RESET, "Reset", false, 0);
    if (a->mode == M_POMODORO)
        button(a, H_AUX, "Skip", false, 0);
    else if (a->mode == M_STOPWATCH)
        button(a, H_AUX, "Lap", false, 0);
    else
        wlwin_text(w, rect_of(H_AUX).x + 8, rect_of(H_AUX).y + 9, "+ and - keys", 1, lm_theme.text_dim, rect_of(H_AUX).w - 8);
    if (a->mode == M_TIMER) {
        button(a, H_MINUS, "-", false, 0);
        button(a, H_PLUS, "+", false, 0);
        for (int i = 0; i < 6; i++) {
            char p[8];
            snprintf(p, sizeof(p), "%d", presets[i]);
            button(a, H_PRE0 + i, p, a->timer_set_min == presets[i], lm_theme.accent);
        }
        wlwin_text(w, PAD, 270, "minutes", 1, lm_theme.text_dim, 0);
    }
    if (a->mode == M_STOPWATCH) {                        /* the laps, newest first, two columns */
        for (int i = 0; i < a->nlaps; i++) {
            int k = a->nlaps - 1 - i;
            char lt[24], row[48];
            fmt_time(a->laps[k], false, lt, sizeof(lt));
            snprintf(row, sizeof(row), "%d  %s", k + 1, lt);
            wlwin_text(w, PAD + (i % 2) * 170, 236 + (i / 2) * 16, row, 1, lm_theme.text_dim, 0);
        }
    }
}

/* ---- events ---------------------------------------------------------------------------------------------------------------- */

static void hide(struct app *a)
{
    if (a->win.toplevel)
        WLC_SEND0(a->win.c, a->win.toplevel, LM_XDG_TOPLEVEL_REQ_SET_MINIMIZED);
}

static bool win_close(struct wlwin *w)
{
    struct app *a = w->data;
    if (anything_runs(a)) {                              /* a timer runs: the window goes to the taskbar and the program goes on */
        hide(a);
        return false;
    }
    return true;
}

static void click(struct app *a, int h)
{
    if (h >= H_TAB0 && h <= H_TAB2) {
        a->mode = (enum mode)(h - H_TAB0);
        a->win.redraw = true;
    } else if (h == H_START) {
        toggle(a);
    } else if (h == H_RESET) {
        reset(a);
    } else if (h == H_AUX) {
        if (a->mode == M_POMODORO)
            skip(a);
        else if (a->mode == M_STOPWATCH)
            lap(a);
    } else if (h >= H_PRE0 && h <= H_PRE5) {
        set_timer_minutes(a, presets[h - H_PRE0]);
    } else if (h == H_MINUS) {
        set_timer_minutes(a, a->timer_set_min - (a->timer_set_min > 10 ? 5 : 1));
    } else if (h == H_PLUS) {
        set_timer_minutes(a, a->timer_set_min + (a->timer_set_min >= 10 ? 5 : 1));
    }
}

static void pointer_event(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
    struct app *a = w->data;
    int h;
    switch (op) {
    case LM_WL_POINTER_EVT_ENTER:
        a->px = lm_fixed_to_double(args[2].f);
        a->py = lm_fixed_to_double(args[3].f);
        h = hit_test(a, a->px, a->py);
        if (h != a->hover) { a->hover = h; w->redraw = true; }
        break;
    case LM_WL_POINTER_EVT_LEAVE:
        if (a->hover >= 0) { a->hover = -1; w->redraw = true; }
        break;
    case LM_WL_POINTER_EVT_MOTION:
        a->px = lm_fixed_to_double(args[1].f);
        a->py = lm_fixed_to_double(args[2].f);
        h = hit_test(a, a->px, a->py);
        if (h != a->hover) { a->hover = h; w->redraw = true; }
        break;
    case LM_WL_POINTER_EVT_BUTTON:
        if (args[2].u == 272 && args[3].u == 1) {
            h = hit_test(a, a->px, a->py);
            if (h >= 0)
                click(a, h);
        }
        break;
    }
}

static void keyboard_event(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
    struct app *a = w->data;
    if (op != LM_WL_KEYBOARD_EVT_KEY || args[3].u == 0)
        return;
    uint32_t ks = lm_keymap_keysym(args[2].u, w->mods);
    char txt[8];
    int tl = lm_keysym_to_utf8(ks, txt);
    char c = (tl == 1 && !(w->mods & (LM_MOD_CTRL | LM_MOD_ALT | LM_MOD_SUPER))) ? txt[0] : 0;
    if (ks == LM_KS_Escape || c == 'q' || c == 'Q') {
        if (anything_runs(a))
            hide(a);
        else
            w->running = false;
    } else if (c == ' ' || ks == LM_KS_Return) {
        toggle(a);
    } else if (c == 'r' || c == 'R') {
        reset(a);
    } else if (c == '1' || c == '2' || c == '3') {
        a->mode = (enum mode)(c - '1');
        w->redraw = true;
    } else if (ks == LM_KS_Tab) {
        a->mode = (enum mode)((a->mode + 1) % M_COUNT);
        w->redraw = true;
    } else if (c == 'n' || c == 'N') {
        skip(a);
    } else if (c == 'l' || c == 'L') {
        lap(a);
    } else if (c == '+' || c == '=') {
        if (a->mode == M_TIMER)
            click(a, H_PLUS);
    } else if (c == '-') {
        if (a->mode == M_TIMER)
            click(a, H_MINUS);
    }
}

static void global_event(struct wlwin *w, uint32_t name, const char *iface, uint32_t ver)
{
    (void)ver;
    struct app *a = w->data;
    if (!strcmp(iface, "mellagui_settings_v1"))
        a->settings = wlc_bind(w->c, w->registry, name, &lm_mellagui_settings_v1_interface, 1, NULL, a);
    else if (!strcmp(iface, "mellagui_tray_v1"))
        a->tray = wlc_bind(w->c, w->registry, name, &lm_mellagui_tray_v1_interface, 1, NULL, a);
}

/* ---- self test ------------------------------------------------------------------------------------------------------------- */

static int selftest_fail;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "selftest: %s:%d: %s\n", __FILE__, __LINE__, #cond); selftest_fail++; } } while (0)

static int selftest(void)
{
    char t[24];
    fmt_time(0, true, t, sizeof(t));
    CHECK(!strcmp(t, "00:00"));
    fmt_time(1500000, true, t, sizeof(t));
    CHECK(!strcmp(t, "25:00"));
    fmt_time(1499001, true, t, sizeof(t));                             /* a countdown shows the second that is still running */
    CHECK(!strcmp(t, "25:00"));
    fmt_time(1499001, false, t, sizeof(t));
    CHECK(!strcmp(t, "24:59"));
    fmt_time(3723000, false, t, sizeof(t));
    CHECK(!strcmp(t, "1:02:03"));
    struct clockwork c = { .left_ms = 10000 };
    CHECK(cw_value(&c, true, 99999) == 10000);                         /* stopped: what it is */
    cw_start(&c, 1000);
    CHECK(cw_value(&c, true, 4000) == 7000 && cw_value(&c, true, 20000) == 0);
    cw_stop(&c, true, 4000);
    CHECK(!c.running && c.left_ms == 7000 && cw_value(&c, true, 50000) == 7000);
    struct clockwork s = { 0 };
    cw_start(&s, 500);
    CHECK(cw_value(&s, false, 2500) == 2000);
    cw_stop(&s, false, 2500);
    cw_start(&s, 9000);                                                /* it goes on from where it stopped */
    CHECK(cw_value(&s, false, 10000) == 3000);
    struct app a = { .work_min = 25, .short_min = 5, .long_min = 15, .cycles = 4 };
    CHECK(next_phase(&a, P_WORK, 1) == P_SHORT && next_phase(&a, P_WORK, 3) == P_SHORT);
    CHECK(next_phase(&a, P_WORK, 4) == P_LONG && next_phase(&a, P_WORK, 8) == P_LONG);
    CHECK(next_phase(&a, P_SHORT, 1) == P_WORK && next_phase(&a, P_LONG, 4) == P_WORK);
    CHECK(phase_ms(&a, P_WORK) == 1500000 && phase_ms(&a, P_LONG) == 900000);
    for (int h = 0; h < H_COUNT; h++) {                                  /* everything fits */
        struct rc r = rect_of(h);
        CHECK(r.x >= 0 && r.y >= 0 && r.x + r.w <= WIN_W && r.y + r.h <= WIN_H);
    }
    if (selftest_fail)
        return 1;
    puts("mellagui-timer selftest ok");
    return 0;
}

int main(int argc, char **argv)
{
    lm_theme_load_env();
    if (argc > 1 && !strcmp(argv[1], "--selftest"))
        return selftest();
    static const struct wlwin_ops ops = { .paint = paint, .global = global_event, .pointer = pointer_event, .keyboard = keyboard_event, .close = win_close };
    struct app a = { .hover = -1, .tray_hold = -1 };
    const char *unit = getenv("MELLAGUI_TIMER_UNIT_MS");
    if (unit && atoi(unit) > 0)
        g_minute = atoi(unit);
    load_conf(&a);
    reset_pomodoro(&a);
    a.cw[M_TIMER].left_ms = (int64_t)a.timer_set_min * g_minute;
    int rc = 1;
    if (wlwin_connect(&a.win, "mellagui-timer", &ops, &a) < 0)
        return 1;
    if (wlc_roundtrip(a.win.c) < 0)
        goto out;
    if (wlwin_open(&a.win, "Timer", "mellagui.timer", WIN_W, WIN_H, WIN_W, WIN_H) < 0)
        goto out;
    a.win.fixed_size = false;
    for (;;) {
        bool run = anything_runs(&a);
        if (!wlwin_step(&a.win, run ? 200 : 1000))
            break;
        int64_t now = now_ms();
        check_done(&a, now);
        if (a.had_tray_click) {                          /* the bar's item: pause / continue what it shows */
            a.had_tray_click = false;
            bool did = false;
            for (int i = 0; i < M_COUNT && !did; i++)
                if (a.cw[i].running) {
                    cw_stop(&a.cw[i], countdown_mode((enum mode)i), now);
                    a.tray_hold = i;
                    did = true;
                }
            if (!did && a.tray_hold >= 0) {                  /* the click on a paused one continues it */
                cw_start(&a.cw[a.tray_hold], now);
                a.tray_hold = -1;
            }
            a.win.redraw = true;
        }
        tray_update(&a, now);
        if (run || anything_runs(&a))
            a.win.redraw = true;                         /* the digits */
    }
    rc = 0;
out:
    if (a.win.c)
        wlwin_destroy(&a.win);
    return rc;
}
