/*
 * mellagui-timer: a pomodoro timer, a countdown timer, a stopwatch and an alarm clock in one small window.
 *
 *   mellagui-timer              the window (keys: Space start / pause, R reset, 1 2 3 4 or Tab the four parts, N skip to the next pomodoro phase,
 *                               L a lap, + / - a minute more / less on the timer, Esc / Q / the close button hide it - it keeps running while a timer runs)
 *   mellagui-timer --background the alarms without a window (the compositor starts it with itself; it ends when no alarm is on)
 *   mellagui-timer --selftest   checks the arithmetic without a display
 *
 * Alarm: up to four alarms of a time of day, each once / every day / on weekdays, on or off (the lines `alarm = 07:30 weekdays on` of timer.conf, which the program
 * writes itself). When one is due the program rings: a notification card, the compositor's alarm call (the write-only setting `timer-beep`) every three seconds
 * for at most five minutes, and an item "ALARM 07:30" in the top bar whose click opens a menu (Dismiss, Snooze 5 minutes); the window, when it is open,
 * shows the same two buttons. The program stays alive for the alarms: closing the window with an alarm on hands the alarms to a window-less copy
 * (`--background`, one holder at a time: `$XDG_RUNTIME_DIR/mellagui-timer.duty`; a window that starts takes the alarms over from it).
 * $MELLAGUI_ALARM_NOW=HH:MM[:SS] sets the clock the alarms go by (tests), $MELLAGUI_TIMER_UNIT_MS the length of a minute for the snooze and the ringing.
 *
 * Pomodoro: 25 minutes of work, 5 of rest, a long rest of 15 after the fourth work; the lengths are the lines `work`, `short`, `long` (minutes) and `cycles`
 * of ~/.config/mellagui/timer.conf (`auto = 1` starts the next phase by itself). When a countdown ends the program puts up a notification card and the
 * compositor plays a short tune (the write-only settings `notify` and `timer-beep` of mellagui_settings_v1). While something runs the top bar has an item
 * with the time left; a click on it pauses / continues.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
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

enum mode { M_POMODORO, M_TIMER, M_STOPWATCH, M_ALARM, M_COUNT };
enum phase { P_WORK, P_SHORT, P_LONG };

struct clockwork {
    bool running;
    int64_t left_ms;                    /* countdowns: what is left when stopped; stopwatch: what has been counted */
    int64_t t0;                         /* running: the monotonic time of the last start */
};

#define MAX_ALARMS 4
enum repeat { R_ONCE, R_DAILY, R_WEEKDAYS };

struct alarm {
    int hour, min;
    int repeat;
    bool on;
    int fired;                          /* the minute (a day number and the minute of the day) it last rang in, so that it rings once */
};

struct app {
    struct wlwin win;
    uint32_t settings, tray, tray_item;
    struct alarm alarms[MAX_ALARMS];
    int nalarms, asel;                  /* the alarms; the one the window edits */
    int ring;                           /* the alarm that rings now, -1 = none */
    int64_t ring_start, next_beep;
    int snooze;                         /* the alarm that rings again at snooze_until (monotonic ms), -1 = none */
    int64_t snooze_until;
    bool duty;                          /* this process rings the alarms */
    bool background;                    /* no window */
    int menu_kind;                      /* what the menu of wlwin_menu() is for: 1 = the ringing alarm, 2 = an armed one */
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
        if (!strcmp(k, "alarm") && a->nalarms < MAX_ALARMS) {          /* alarm = 07:30 weekdays on */
            int h, m;
            char rep[16] = "once", on[8] = "on";
            if (sscanf(v, " %d:%d %15s %7s", &h, &m, rep, on) >= 2 && h >= 0 && h < 24 && m >= 0 && m < 60) {
                struct alarm *al = &a->alarms[a->nalarms++];
                al->hour = h;
                al->min = m;
                al->repeat = !strcmp(rep, "daily") ? R_DAILY : !strcmp(rep, "weekdays") ? R_WEEKDAYS : R_ONCE;
                al->on = strcmp(on, "off") != 0;
                al->fired = -1;
            }
        }
        else if (!strcmp(k, "work") && n >= 1 && n <= 180) a->work_min = n;
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

static bool conf_path(char *path, size_t n)
{
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (xdg && *xdg)
        snprintf(path, n, "%s/mellagui/timer.conf", xdg);
    else if (home && *home)
        snprintf(path, n, "%s/.config/mellagui/timer.conf", home);
    else
        return false;
    return true;
}

static const char *const repeat_names[3] = { "once", "daily", "weekdays" };
static const char *const repeat_labels[3] = { "Once", "Every day", "Weekdays" };

/* Writes the alarms into timer.conf: every other line of the file stays as it is. */
static void save_alarms(const struct app *a)
{
    char path[1024], tmp[1100];
    if (!conf_path(path, sizeof(path)))
        return;
    char *keep = NULL;
    size_t keep_n = 0;
    FILE *f = fopen(path, "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            const char *p = line;
            while (*p == ' ' || *p == '\t')
                p++;
            if (!strncmp(p, "alarm", 5) && (p[5] == ' ' || p[5] == '=' || p[5] == '\t'))
                continue;                                       /* these are written again below */
            size_t l = strlen(line);
            char *nk = realloc(keep, keep_n + l + 1);
            if (!nk)
                break;
            keep = nk;
            memcpy(keep + keep_n, line, l + 1);
            keep_n += l;
        }
        fclose(f);
    }
    snprintf(tmp, sizeof(tmp), "%s.%d.tmp", path, (int)getpid());
    for (char *p = path + 1; *p; p++) {                         /* parent directories */
        if (*p != '/')
            continue;
        *p = 0;
        mkdir(path, 0755);
        *p = '/';
    }
    FILE *o = fopen(tmp, "w");
    if (o) {
        if (keep)
            fputs(keep, o);
        else
            fputs("# mellagui-timer: written by the program itself\n", o);
        for (int i = 0; i < a->nalarms; i++)
            fprintf(o, "alarm = %02d:%02d %s %s\n", a->alarms[i].hour, a->alarms[i].min, repeat_names[a->alarms[i].repeat], a->alarms[i].on ? "on" : "off");
        if (fclose(o) != 0 || rename(tmp, path) < 0)
            unlink(tmp);
    }
    free(keep);
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

/* ---- the alarms ----------------------------------------------------------------------------------------------------------- */

static time_t g_wall_off;                 /* seconds added to the real clock ($MELLAGUI_ALARM_NOW: tests) */

static time_t wall_now(void)
{
    return time(NULL) + g_wall_off;
}

static void set_wall_from_env(void)
{
    const char *e = getenv("MELLAGUI_ALARM_NOW");
    int h, m, s = 0;
    if (!e || sscanf(e, "%d:%d:%d", &h, &m, &s) < 2 || h < 0 || h > 23 || m < 0 || m > 59 || s < 0 || s > 59)
        return;
    time_t t = time(NULL);
    struct tm lt;
    localtime_r(&t, &lt);
    lt.tm_hour = h;
    lt.tm_min = m;
    lt.tm_sec = s;
    lt.tm_isdst = -1;
    g_wall_off = mktime(&lt) - t;
}

static int ring_period_ms(void)
{
    int64_t p = g_minute / 20;                                  /* 3 s in a real minute */
    return (int)(p < 200 ? 200 : p > 3000 ? 3000 : p);
}

static bool alarm_on_that_day(const struct alarm *al, const struct tm *lt)
{
    return al->repeat != R_WEEKDAYS || (lt->tm_wday >= 1 && lt->tm_wday <= 5);
}

/* Minutes until the alarm is next due (from the minute `lt` is in; 0 = this minute), -1 when it is off. */
static int alarm_minutes_until(const struct alarm *al, const struct tm *lt)
{
    if (!al->on)
        return -1;
    for (int d = 0; d <= 7; d++) {
        struct tm day = *lt;
        day.tm_mday += d;
        day.tm_isdst = -1;
        mktime(&day);                                           /* the weekday of that day */
        if (!alarm_on_that_day(al, &day))
            continue;
        int diff = d * 1440 + al->hour * 60 + al->min - (lt->tm_hour * 60 + lt->tm_min);
        if (diff >= 0)
            return diff;
    }
    return -1;
}

static bool alarms_armed(const struct app *a)
{
    if (a->ring >= 0 || a->snooze >= 0)
        return true;
    for (int i = 0; i < a->nalarms; i++)
        if (a->alarms[i].on)
            return true;
    return false;
}

/* The armed alarm that is due first, -1 when there is none. */
static int next_alarm(const struct app *a, int *minutes)
{
    time_t t = wall_now();
    struct tm lt;
    localtime_r(&t, &lt);
    int best = -1, bm = 0;
    for (int i = 0; i < a->nalarms; i++) {
        int m = alarm_minutes_until(&a->alarms[i], &lt);
        if (m >= 0 && (best < 0 || m < bm)) {
            best = i;
            bm = m;
        }
    }
    if (minutes)
        *minutes = bm;
    return best;
}

static void ring_start(struct app *a, int i, bool snoozed, int64_t now)
{
    a->ring = i;
    a->ring_start = now;
    a->next_beep = now;
    char body[64];
    snprintf(body, sizeof(body), "%02d:%02d%s", a->alarms[i].hour, a->alarms[i].min, snoozed ? " (snoozed)" : "");
    char msg[200];
    snprintf(msg, sizeof(msg), "Alarm\t%s\t%d", body, 15000);
    send_set(a, "notify", msg);                                  /* (the beeps come from the loop: the first one at once) */
    a->win.redraw = true;
}

static void ring_stop(struct app *a)
{
    a->ring = -1;
    a->win.redraw = true;
}

static void alarm_dismiss(struct app *a)
{
    if (a->ring >= 0)
        ring_stop(a);
}

static void alarm_snooze(struct app *a)
{
    if (a->ring < 0)
        return;
    a->snooze = a->ring;
    a->snooze_until = now_ms() + 5 * g_minute;
    ring_stop(a);
}

/* Called every turn of the loop (the duty holder only): rings what is due, repeats the call while it rings. */
static void alarm_check(struct app *a, int64_t now)
{
    if (!a->duty)
        return;
    time_t t = wall_now();
    struct tm lt;
    localtime_r(&t, &lt);
    int token = (lt.tm_year * 366 + lt.tm_yday) * 1440 + lt.tm_hour * 60 + lt.tm_min;
    for (int i = 0; i < a->nalarms; i++) {
        struct alarm *al = &a->alarms[i];
        if (!al->on || al->hour != lt.tm_hour || al->min != lt.tm_min || al->fired == token || !alarm_on_that_day(al, &lt))
            continue;
        al->fired = token;
        if (al->repeat == R_ONCE) {
            al->on = false;                                      /* an alarm of one day is spent */
            save_alarms(a);
        }
        if (a->ring < 0)
            ring_start(a, i, false, now);
    }
    if (a->snooze >= 0 && now >= a->snooze_until && a->ring < 0) {
        int i = a->snooze;
        a->snooze = -1;
        ring_start(a, i, true, now);
    }
    if (a->ring >= 0) {
        if (now - a->ring_start >= 5 * g_minute) {               /* nobody came: it stops, the card stays in the history */
            char msg[200];
            snprintf(msg, sizeof(msg), "Alarm\tMissed alarm %02d:%02d\t%d", a->alarms[a->ring].hour, a->alarms[a->ring].min, 12000);
            send_set(a, "notify", msg);
            ring_stop(a);
        } else if (now >= a->next_beep) {
            send_set(a, "timer-beep", "1");
            a->next_beep = now + ring_period_ms();
        }
    }
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
    char atext[32] = "";                                /* an alarm that rings or waits: shown when no clock runs (a ringing one first of all) */
    if (a->duty && a->ring >= 0) {
        snprintf(atext, sizeof(atext), "ALARM %02d:%02d", a->alarms[a->ring].hour, a->alarms[a->ring].min);
    } else if (a->duty && run < 0) {
        int mins, i = a->snooze >= 0 ? a->snooze : next_alarm(a, &mins);
        if (i >= 0)
            snprintf(atext, sizeof(atext), "%s %02d:%02d", a->snooze >= 0 ? "Z" : "A", a->alarms[i].hour, a->alarms[i].min);
    }
    if (run < 0 && !atext[0]) {
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
    if (atext[0] && (a->ring >= 0 || run < 0)) {
        snprintf(text, sizeof(text), "%s", atext);
    } else {
        fmt_time(cw_value(&a->cw[run], countdown_mode((enum mode)run), now), countdown_mode((enum mode)run), t, sizeof(t));
        snprintf(text, sizeof(text), "%s%s%s", paused ? "|| " : "", run == M_POMODORO ? (a->phase == P_WORK ? "W " : "R ") : run == M_TIMER ? "T " : "S ", t);
    }
    if (strcmp(text, a->tray_text)) {
        snprintf(a->tray_text, sizeof(a->tray_text), "%s", text);
        WLC_SEND(a->win.c, a->tray_item, LM_MELLAGUI_TRAY_ITEM_V1_REQ_SET_TEXT, { .s = text });
        wlc_flush(a->win.c);
    }
}

/* ---- layout and drawing --------------------------------------------------------------------------------------------------- */

struct rc { int x, y, w, h; };

enum hit { H_TAB0, H_TAB1, H_TAB2, H_TAB3, H_START, H_RESET, H_AUX, H_PRE0, H_PRE1, H_PRE2, H_PRE3, H_PRE4, H_PRE5, H_MINUS, H_PLUS,
            H_AROW0, H_AROW1, H_AROW2, H_AROW3, H_AHM, H_AHP, H_AMM, H_AMP, H_AREP, H_AON, H_ADEL, H_AADD, H_RDISMISS, H_RSNOOZE, H_COUNT };
static const int presets[6] = { 1, 5, 10, 15, 25, 60 };

static struct rc rect_of(int h)
{
    int tabw = (WIN_W - 2 * PAD - 24) / 4;
    int by = 190, bw = (WIN_W - 2 * PAD - 16) / 3;
    if (h <= H_TAB3)
        return (struct rc){ PAD + h * (tabw + 8), 12, tabw, 28 };
    if (h >= H_AROW0 && h <= H_AROW3)
        return (struct rc){ PAD, 50 + (h - H_AROW0) * 36, WIN_W - 2 * PAD, 34 };
    if (h >= H_AHM && h <= H_AMP) {
        int aw = (WIN_W - 2 * PAD - 18) / 4;
        return (struct rc){ PAD + (h - H_AHM) * (aw + 6), 200, aw, 28 };
    }
    if (h >= H_AREP && h <= H_AADD) {
        int aw = (WIN_W - 2 * PAD - 18) / 4;
        return (struct rc){ PAD + (h - H_AREP) * (aw + 6), 236, aw, 30 };
    }
    if (h == H_RDISMISS || h == H_RSNOOZE) {
        int rw = (WIN_W - 2 * PAD - 8) / 2;
        return (struct rc){ PAD + (h - H_RDISMISS) * (rw + 8), 200, rw, 44 };
    }
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
    if (a->ring >= 0)                                    /* an alarm rings: only its two buttons */
        return h == H_RDISMISS || h == H_RSNOOZE;
    if (h == H_RDISMISS || h == H_RSNOOZE)
        return false;
    if (h >= H_AROW0 && h <= H_AADD) {
        if (a->mode != M_ALARM)
            return false;
        if (h <= H_AROW3)
            return h - H_AROW0 < a->nalarms;
        if (h == H_AADD)
            return a->nalarms < MAX_ALARMS;
        return a->nalarms > 0;
    }
    if (a->mode == M_ALARM && (h == H_START || h == H_RESET || h == H_AUX))
        return false;
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
    wlwin_drop(w, r.x, r.y, r.w, r.h, lm_button_radius(r.h / 2), bg);
    wlwin_text(w, r.x + (r.w - lm_font_text_width(text)) / 2, r.y + (r.h - LM_FONT_H) / 2, text, 1, on ? lm_theme.ink : lm_theme.text, r.w - 6);
}

static void paint_ring(struct app *a)
{
    struct wlwin *w = &a->win;
    int64_t now = now_ms();
    char t[24];
    snprintf(t, sizeof(t), "%02d:%02d", a->alarms[a->ring].hour, a->alarms[a->ring].min);
    const char *title = "ALARM";
    uint32_t col = (now / 500) % 2 ? lm_theme.alert : lm_theme.text;               /* it blinks */
    wlwin_text(w, (WIN_W - lm_font_text_width(title) * 3) / 2, 40, title, 3, col, 0);
    wlwin_text(w, (WIN_W - lm_font_text_width(t) * 6) / 2, 92, t, 6, lm_theme.text, 0);
    const char *hint = "Enter: dismiss     S: snooze";
    wlwin_text(w, (WIN_W - lm_font_text_width(hint)) / 2, 170, hint, 1, lm_theme.text_dim, WIN_W - 2 * PAD);
    button(a, H_RDISMISS, "Dismiss", true, lm_theme.accent);
    button(a, H_RSNOOZE, "Snooze 5 min", false, 0);
}

/* The alarm tab: the alarms as rows (time, repeat, on / off), the one that is chosen is changed with the buttons below. */
static void paint_alarms(struct app *a)
{
    struct wlwin *w = &a->win;
    time_t t = wall_now();
    struct tm lt;
    localtime_r(&t, &lt);
    for (int i = 0; i < a->nalarms; i++) {
        struct rc r = rect_of(H_AROW0 + i);
        const struct alarm *al = &a->alarms[i];
        uint32_t bg = i == a->asel ? lm_theme.button_hover : lm_theme.button;
        wlwin_round_fill(w, r.x, r.y, r.w, r.h, lm_button_radius(8), bg);
        char tm[8];
        snprintf(tm, sizeof(tm), "%02d:%02d", al->hour, al->min);
        wlwin_text(w, r.x + 10, r.y + 1, tm, 2, al->on ? lm_theme.text : lm_theme.text_dim, 0);
        wlwin_text(w, r.x + 112, r.y + (r.h - LM_FONT_H) / 2, repeat_labels[al->repeat], 1, lm_theme.text_dim, 110);
        struct rc sw = { r.x + r.w - 64, r.y + 5, 54, r.h - 10 };                  /* the switch */
        wlwin_round_fill(w, sw.x, sw.y, sw.w, sw.h, lm_button_radius(sw.h / 2), al->on ? lm_theme.ok : lm_theme.background);
        const char *st = al->on ? "On" : "Off";
        wlwin_text(w, sw.x + (sw.w - lm_font_text_width(st)) / 2, sw.y + (sw.h - LM_FONT_H) / 2, st, 1, al->on ? lm_theme.ink : lm_theme.text_dim, 0);
    }
    if (a->nalarms == 0) {
        wlwin_text(w, PAD, 70, "No alarms yet.", 1, lm_theme.text_dim, 0);
        wlwin_text(w, PAD, 94, "Add one below, set its time and say how often it rings.", 1, lm_theme.text_dim, WIN_W - 2 * PAD);
    } else {
        button(a, H_AHM, "Hour -", false, 0);
        button(a, H_AHP, "Hour +", false, 0);
        button(a, H_AMM, "Min -", false, 0);
        button(a, H_AMP, "Min +", false, 0);
        const struct alarm *al = &a->alarms[a->asel];
        button(a, H_AREP, repeat_labels[al->repeat], false, 0);
        button(a, H_AON, al->on ? "Turn off" : "Turn on", al->on, lm_theme.accent);
        button(a, H_ADEL, "Delete", false, 0);
    }
    if (a->nalarms < MAX_ALARMS)
        button(a, H_AADD, "Add", false, 0);
    char hint[96] = "";
    int mins;
    int ni = next_alarm(a, &mins);
    if (!a->duty && alarms_armed(a))
        snprintf(hint, sizeof(hint), "Another timer window rings the alarms.");
    else if (ni >= 0)
        snprintf(hint, sizeof(hint), "Next: %02d:%02d, in %d h %02d min", a->alarms[ni].hour, a->alarms[ni].min, mins / 60, mins % 60);
    else if (a->nalarms)
        snprintf(hint, sizeof(hint), "All alarms are off.");
    wlwin_text(w, PAD, 274, hint, 1, lm_theme.text_dim, WIN_W - 2 * PAD);
}

static void paint(struct wlwin *w)
{
    struct app *a = w->data;
    int64_t now = now_ms();
    wlwin_fill(w, 0, 0, w->bw, w->bh, lm_theme.background);
    if (a->ring >= 0) {
        paint_ring(a);
        return;
    }
    static const char *const tabs[M_COUNT] = { "Pomodoro", "Timer", "Stopwatch", "Alarm" };
    for (int i = 0; i < M_COUNT; i++)
        button(a, H_TAB0 + i, tabs[i], a->mode == (enum mode)i, lm_theme.accent);
    if (a->mode == M_ALARM) {
        paint_alarms(a);
        return;
    }
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

static void alarm_edit(struct app *a, int what)
{
    if (a->asel < 0 || a->asel >= a->nalarms)
        return;
    struct alarm *al = &a->alarms[a->asel];
    switch (what) {
    case H_AHM: al->hour = (al->hour + 23) % 24; break;
    case H_AHP: al->hour = (al->hour + 1) % 24; break;
    case H_AMM: al->min = (al->min + 55) % 60; break;
    case H_AMP: al->min = (al->min + 5) % 60; break;
    case H_AREP: al->repeat = (al->repeat + 1) % 3; break;
    case H_AON: al->on = !al->on; break;
    case H_ADEL:
        for (int i = a->asel; i + 1 < a->nalarms; i++)
            a->alarms[i] = a->alarms[i + 1];
        a->nalarms--;
        if (a->snooze == a->asel)
            a->snooze = -1;
        if (a->asel >= a->nalarms)
            a->asel = a->nalarms - 1;
        save_alarms(a);
        a->win.redraw = true;
        return;
    }
    al->fired = -1;                                      /* a changed alarm may ring in this very minute */
    save_alarms(a);
    a->win.redraw = true;
}

static void alarm_add(struct app *a)
{
    if (a->nalarms >= MAX_ALARMS)
        return;
    time_t t = wall_now();
    struct tm lt;
    localtime_r(&t, &lt);
    a->alarms[a->nalarms] = (struct alarm){ .hour = (lt.tm_hour + 1) % 24, .min = 0, .repeat = R_DAILY, .on = true, .fired = -1 };   /* the next full hour */
    a->asel = a->nalarms++;
    save_alarms(a);
    a->win.redraw = true;
}

static void click(struct app *a, int h)
{
    if (h == H_RDISMISS) {
        alarm_dismiss(a);
    } else if (h == H_RSNOOZE) {
        alarm_snooze(a);
    } else if (h >= H_AROW0 && h <= H_AROW3) {
        a->asel = h - H_AROW0;
        struct rc r = rect_of(h);
        if (a->px >= r.x + r.w - 70)                     /* the switch at the end of the row */
            alarm_edit(a, H_AON);
        a->win.redraw = true;
    } else if (h >= H_AHM && h <= H_ADEL) {
        alarm_edit(a, h);
    } else if (h == H_AADD) {
        alarm_add(a);
    } else if (h >= H_TAB0 && h <= H_TAB3) {
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
    if (a->ring >= 0) {                                  /* an alarm rings: Enter / Space / Esc dismiss, S snoozes */
        if (c == 's' || c == 'S')
            alarm_snooze(a);
        else if (ks == LM_KS_Return || ks == LM_KS_KP_Enter || c == ' ' || ks == LM_KS_Escape)
            alarm_dismiss(a);
        return;
    }
    if (a->mode == M_ALARM && (c == 'h' || c == 'H' || c == 'm' || c == 'M' || c == 'a' || c == 'A' || ks == LM_KS_Up || ks == LM_KS_Down || ks == LM_KS_Delete ||
                               ((c == ' ' || ks == LM_KS_Return) && a->nalarms > 0))) {
        if (ks == LM_KS_Up && a->asel > 0)
            a->asel--;
        else if (ks == LM_KS_Down && a->asel + 1 < a->nalarms)
            a->asel++;
        else if (c == 'h')
            alarm_edit(a, H_AHP);
        else if (c == 'H')
            alarm_edit(a, H_AHM);
        else if (c == 'm')
            alarm_edit(a, H_AMP);
        else if (c == 'M')
            alarm_edit(a, H_AMM);
        else if (c == 'a' || c == 'A')
            alarm_add(a);
        else if (ks == LM_KS_Delete)
            alarm_edit(a, H_ADEL);
        else if (c == ' ' || ks == LM_KS_Return)
            alarm_edit(a, H_AON);
        a->win.redraw = true;
        return;
    }
    if (a->mode == M_ALARM && (c == 'r' || c == 'R')) {
        alarm_edit(a, H_AREP);
        return;
    }
    if (ks == LM_KS_Escape || c == 'q' || c == 'Q') {
        if (anything_runs(a))
            hide(a);
        else
            w->running = false;
    } else if (c == ' ' || ks == LM_KS_Return) {
        toggle(a);
    } else if (c == 'r' || c == 'R') {
        reset(a);
    } else if (c == '1' || c == '2' || c == '3' || c == '4') {
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

/* ---- the menu of the bar's item, the alarms' holder ------------------------------------------------------------------------ */

static volatile sig_atomic_t g_term;
static int g_duty_fd = -1;

static void on_term(int sig)
{
    (void)sig;
    g_term = 1;
}

static void open_self(const char *arg)
{
    char exe[1024];
    ssize_t k = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (k <= 0)
        return;
    exe[k] = 0;
    pid_t pid = fork();
    if (pid == 0) {                                      /* twice, so that nobody waits for it */
        if (fork() == 0) {
            setsid();
            execl(exe, exe, arg, (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }
    if (pid > 0)
        waitpid(pid, NULL, 0);
}

static void menu_event(struct wlwin *w, int index)
{
    struct app *a = w->data;
    int kind = a->menu_kind;
    a->menu_kind = 0;
    if (index < 0)
        return;
    if (kind == 1) {                                     /* the ringing alarm */
        if (index == 0)
            alarm_dismiss(a);
        else if (index == 1)
            alarm_snooze(a);
    } else if (kind == 2) {                              /* an alarm that waits */
        if (index == 1) {
            open_self("--window");
        } else if (index == 2) {
            int i = next_alarm(a, NULL);
            if (i >= 0) {
                a->alarms[i].on = false;
                save_alarms(a);
                a->win.redraw = true;
            }
        }
    }
}

/* Who rings the alarms: the one process that holds a lock on $XDG_RUNTIME_DIR/mellagui-timer.duty. A window takes the lock from a window-less holder
 * (which it asks to end), never from another window. */
static bool duty_take(bool background)
{
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (!rt || !*rt)
        return !background;
    char path[1024];
    snprintf(path, sizeof(path), "%s/mellagui-timer.duty", rt);
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0)
        return !background;
    for (int tries = 0; tries < 80; tries++) {
        if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
            if (ftruncate(fd, 0) == 0) {
                char line[32];
                int n = snprintf(line, sizeof(line), "%d %c\n", (int)getpid(), background ? 'b' : 'w');
                if (write(fd, line, (size_t)n) < 0) {}
            }
            g_duty_fd = fd;
            return true;
        }
        if (background)
            break;
        if (tries == 0) {
            char line[32] = "";
            ssize_t k = pread(fd, line, sizeof(line) - 1, 0);
            int pid = 0;
            char role = 0;
            if (k > 0 && sscanf(line, "%d %c", &pid, &role) == 2 && role == 'b' && pid > 1)
                kill(pid, SIGTERM);                      /* a window-less holder gives way */
            else
                break;                                   /* another window has it */
        }
        usleep(50000);
    }
    close(fd);
    return false;
}

static void duty_release(void)
{
    if (g_duty_fd >= 0)
        close(g_duty_fd);
    g_duty_fd = -1;
}

static void open_menu(struct app *a, int kind, const char *entries)
{
    a->menu_kind = kind;
    if (!wlwin_menu(&a->win, entries)) {
        a->menu_kind = 0;
        if (kind == 1)
            alarm_dismiss(a);                            /* no menu to ask: a click on the item ends the ringing */
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
    struct tm lt = { .tm_year = 126, .tm_mon = 10, .tm_mday = 13, .tm_hour = 6, .tm_min = 0, .tm_isdst = -1 };     /* Friday 13 November 2026, 06:00 */
    mktime(&lt);
    struct alarm once = { .hour = 7, .min = 30, .repeat = R_ONCE, .on = true }, wk = { .hour = 7, .min = 30, .repeat = R_WEEKDAYS, .on = true };
    CHECK(alarm_minutes_until(&once, &lt) == 90);
    lt.tm_hour = 8;
    mktime(&lt);
    CHECK(alarm_minutes_until(&once, &lt) == 1410);                     /* tomorrow */
    CHECK(alarm_minutes_until(&wk, &lt) == 3 * 1440 + 450 - 480);        /* a Friday morning that has gone: Monday */
    once.on = false;
    CHECK(alarm_minutes_until(&once, &lt) == -1);
    CHECK(alarm_on_that_day(&wk, &lt) && !alarm_on_that_day(&wk, &(struct tm){ .tm_wday = 6 }) && alarm_on_that_day(&(struct alarm){ .repeat = R_DAILY }, &(struct tm){ .tm_wday = 0 }));
    for (int h = 0; h < H_COUNT; h++) {                                  /* everything fits */
        struct rc r = rect_of(h);
        CHECK(r.x >= 0 && r.y >= 0 && r.x + r.w <= WIN_W && r.y + r.h <= WIN_H);
    }
    if (selftest_fail)
        return 1;
    puts("mellagui-timer selftest ok");
    return 0;
}

/* Handles the click on the bar's item (it ran in the loop before; the alarms need a menu). */
static void tray_clicked(struct app *a, int64_t now)
{
    if (a->duty && a->ring >= 0) {
        open_menu(a, 1, "Dismiss\nSnooze 5 minutes");
        return;
    }
    bool did = false;                                    /* pause / continue what it shows */
    for (int i = 0; i < M_COUNT && !did; i++)
        if (a->cw[i].running) {
            cw_stop(&a->cw[i], countdown_mode((enum mode)i), now);
            a->tray_hold = i;
            did = true;
        }
    if (!did && a->tray_hold >= 0) {                     /* the click on a paused one continues it */
        cw_start(&a->cw[a->tray_hold], now);
        a->tray_hold = -1;
        did = true;
    }
    if (!did && a->duty && alarms_armed(a)) {            /* only an alarm waits: a small menu */
        int i = a->snooze >= 0 ? a->snooze : next_alarm(a, NULL);
        if (i >= 0) {
            char entries[160];
            snprintf(entries, sizeof(entries), "~%s %02d:%02d\nOpen the timer...\nTurn this alarm off", a->snooze >= 0 ? "Snoozed alarm" : "Next alarm", a->alarms[i].hour,
                     a->alarms[i].min);
            open_menu(a, 2, entries);
        }
    }
    a->win.redraw = true;
}

int main(int argc, char **argv)
{
    wlwin_hidpi();                      /* draws through the wlwin_* calls only: sharp on a scaled screen */
    lm_theme_load_env();
    bool background = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--selftest"))
            return selftest();
        else if (!strcmp(argv[i], "--background"))
            background = true;
        else if (strcmp(argv[i], "--window") != 0) {
            fprintf(stderr, "usage: mellagui-timer [--background | --selftest]\n");
            return 2;
        }
    }
    static const struct wlwin_ops ops = { .paint = paint, .global = global_event, .pointer = pointer_event, .keyboard = keyboard_event, .close = win_close,
                                          .menu = menu_event };
    struct app a = { .hover = -1, .tray_hold = -1, .ring = -1, .snooze = -1, .background = background };
    const char *unit = getenv("MELLAGUI_TIMER_UNIT_MS");
    if (unit && atoi(unit) > 0)
        g_minute = atoi(unit);
    set_wall_from_env();
    load_conf(&a);
    reset_pomodoro(&a);
    a.cw[M_TIMER].left_ms = (int64_t)a.timer_set_min * g_minute;
    if (background && !alarms_armed(&a))
        return 0;                                        /* nothing to ring */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_term;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    a.duty = duty_take(background);
    if (background && !a.duty)
        return 0;                                        /* another one has the alarms */
    int rc = 1;
    if (wlwin_connect(&a.win, "mellagui-timer", &ops, &a) < 0)
        return 1;
    if (wlc_roundtrip(a.win.c) < 0)
        goto out;
    if (!background) {
        if (wlwin_open(&a.win, "Timer", "mellagui.timer", WIN_W, WIN_H, WIN_W, WIN_H) < 0)
            goto out;
        a.win.fixed_size = false;
    }
    for (;;) {
        bool run = anything_runs(&a);
        int timeout = a.ring >= 0 ? 200 : run ? 200 : alarms_armed(&a) ? 500 : 1000;
        if (!wlwin_step(&a.win, timeout) || g_term)
            break;
        int64_t now = now_ms();
        check_done(&a, now);
        alarm_check(&a, now);
        if (a.had_tray_click) {
            a.had_tray_click = false;
            tray_clicked(&a, now);
        }
        tray_update(&a, now);
        if (background && !alarms_armed(&a))
            break;                                       /* the last alarm is spent: the program has nothing left to do */
        if (run || anything_runs(&a) || a.ring >= 0)
            a.win.redraw = true;                         /* the digits */
    }
    rc = 0;
out:
    if (a.win.c)
        wlwin_destroy(&a.win);
    bool hand_over = a.duty && !background && alarms_armed(&a) && !g_term;
    duty_release();
    if (hand_over)
        open_self("--background");                       /* the window is gone, the alarms are not: a window-less copy rings them */
    return rc;
}
