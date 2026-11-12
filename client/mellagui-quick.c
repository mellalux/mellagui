/*
 * mellagui-quick: the quick settings card of the panel.
 *
 *   mellagui-quick              runs for the whole session once it was started (the first click on the panel's quick settings button, or Super+Z, starts it): its
 *                               window is hidden (minimized, not in the taskbar) and the button shows it under itself; it hides when it loses the focus, on Esc and Q.
 *   mellagui-quick --selftest   checks the arithmetic of the sliders without a display
 *
 * What it shows: the volume of the default output and its mute, the microphone's level and mute, switches for Wi-Fi (nmcli), Bluetooth (bluetoothctl), Do not
 * disturb and the night light, and buttons for the lock, the monitor's settings and the settings panel. The values come from (and go back to) the compositor's
 * mellagui_settings_v1: sink-volume, sink-mute, source-volume, source-mute, dnd, night-light, lock-now. Wi-Fi and Bluetooth are asked from their own programs
 * (nmcli, bluetoothctl; $MELLAGUI_NMCLI, $MELLAGUI_BLUETOOTHCTL) each time the card is shown; a switch whose program is missing is dimmed.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

#define WIN_W 340
#define WIN_H 262
#define PAD 14
#define ROW_H 34
#define BTN 30                          /* the buttons' height */
#define MUTE_W 54                       /* the mute buttons' width */

enum item { I_VOL_MUTE, I_VOL, I_MIC_MUTE, I_MIC, I_WIFI, I_BT, I_DND, I_NIGHT, I_LOCK, I_DISPLAY, I_SETTINGS, I_COUNT };

struct app {
    struct wlwin win;
    uint32_t settings;
    int vol, mic;                       /* percent */
    bool vol_mute, mic_mute, have_mic, dnd, night;
    int wifi, bt;                       /* 1 on, 0 off, -1 not available */
    int hover, drag;                    /* an item or -1 */
    int64_t last_send_ms;
    double px, py;
};

/* ---- layout ---------------------------------------------------------------------------------------------------------------- */

struct rc { int x, y, w, h; };

static struct rc rect_of(int item)
{
    int y1 = 44, y2 = y1 + ROW_H + 4, y3 = y2 + ROW_H + 14, y4 = y3 + ROW_H + 6, y5 = y4 + ROW_H + 14;
    int half = (WIN_W - 2 * PAD - 8) / 2, third = (WIN_W - 2 * PAD - 16) / 3;
    switch (item) {
    case I_VOL_MUTE: return (struct rc){ PAD, y1, MUTE_W, BTN };
    case I_VOL: return (struct rc){ PAD + MUTE_W + 10, y1, WIN_W - 2 * PAD - MUTE_W - 10, BTN };
    case I_MIC_MUTE: return (struct rc){ PAD, y2, MUTE_W, BTN };
    case I_MIC: return (struct rc){ PAD + MUTE_W + 10, y2, WIN_W - 2 * PAD - MUTE_W - 10, BTN };
    case I_WIFI: return (struct rc){ PAD, y3, half, BTN };
    case I_BT: return (struct rc){ PAD + half + 8, y3, half, BTN };
    case I_DND: return (struct rc){ PAD, y4, half, BTN };
    case I_NIGHT: return (struct rc){ PAD + half + 8, y4, half, BTN };
    case I_LOCK: return (struct rc){ PAD, y5, third, BTN };
    case I_DISPLAY: return (struct rc){ PAD + third + 8, y5, third, BTN };
    default: return (struct rc){ PAD + 2 * (third + 8), y5, third, BTN };
    }
}

static bool in_rect(int item, double x, double y)
{
    struct rc r = rect_of(item);
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static int hit(struct app *a, double x, double y)
{
    for (int i = 0; i < I_COUNT; i++)
        if (in_rect(i, x, y) && !((i == I_MIC || i == I_MIC_MUTE) && !a->have_mic))
            return i;
    return -1;
}

/* The slider's value for a pointer at x: the track is the slider's rectangle without the knob's half width at each end. */
static int slider_value(int item, double x)
{
    struct rc r = rect_of(item);
    int lo = r.x + 8, hi = r.x + r.w - 8;
    double t = (x - lo) / (double)(hi - lo);
    return (int)(LM_CLAMP(t, 0.0, 1.0) * 100.0 + 0.5);
}

/* ---- the programs ---------------------------------------------------------------------------------------------------------- */

/* Runs a program, collects stdout (at most `n` bytes) for at most `ms`. False when it could not start or did not answer in time. */
static bool run_capture(const char *const argv[], char *out, size_t n, int ms)
{
    if (n)
        out[0] = 0;
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) < 0)
        return false;
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return false;
    }
    if (pid == 0) {
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) {
            dup2(nul, 0);
            dup2(nul, 2);
        }
        dup2(fds[1], 1);
        setenv("LC_ALL", "C", 1);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    close(fds[1]);
    size_t len = 0;
    bool done = false;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        struct timespec t1;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long left = ms - ((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000);
        if (left <= 0)
            break;
        struct pollfd p = { .fd = fds[0], .events = POLLIN };
        int r = poll(&p, 1, (int)left);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        char buf[512];
        ssize_t k = read(fds[0], buf, sizeof(buf));
        if (k <= 0) {
            done = true;
            break;
        }
        if (len + (size_t)k < n) {
            memcpy(out + len, buf, (size_t)k);
            len += (size_t)k;
            out[len] = 0;
        }
    }
    close(fds[0]);
    if (!done)
        kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return done && len > 0;
}

static const char *nmcli(void)
{
    const char *e = getenv("MELLAGUI_NMCLI");
    return e && *e ? e : "nmcli";
}

static const char *btctl(void)
{
    const char *e = getenv("MELLAGUI_BLUETOOTHCTL");
    return e && *e ? e : "bluetoothctl";
}

static bool tool_off(const char *prog)
{
    return !strcmp(prog, "none");
}

static void refresh_radios(struct app *a)
{
    char out[1024];
    a->wifi = -1;
    a->bt = -1;
    const char *nm = nmcli();
    if (!tool_off(nm)) {
        const char *const argv[] = { nm, "radio", "wifi", NULL };
        if (run_capture(argv, out, sizeof(out), 1500))
            a->wifi = strstr(out, "enabled") != NULL;
    }
    const char *bt = btctl();
    if (!tool_off(bt)) {
        const char *const argv[] = { bt, "show", NULL };
        if (run_capture(argv, out, sizeof(out), 1500) && strstr(out, "Powered:"))
            a->bt = strstr(out, "Powered: yes") != NULL;
    }
}

static void toggle_wifi(struct app *a)
{
    char out[256];
    const char *const argv[] = { nmcli(), "radio", "wifi", a->wifi ? "off" : "on", NULL };
    run_capture(argv, out, sizeof(out), 2500);
    refresh_radios(a);
}

static void toggle_bt(struct app *a)
{
    char out[256];
    const char *const argv[] = { btctl(), "power", a->bt ? "off" : "on", NULL };
    run_capture(argv, out, sizeof(out), 3000);
    refresh_radios(a);
}

/* Starts a program of the suite (next to this one, else from PATH), detached. */
static void launch_sibling(const char *name)
{
    char exe[1024], path[1100];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    const char *prog = name;
    if (n > 0) {
        exe[n] = 0;
        char *slash = strrchr(exe, '/');
        if (slash) {
            snprintf(path, sizeof(path), "%.*s/%s", (int)(slash - exe), exe, name);
            if (access(path, X_OK) == 0)
                prog = path;
        }
    }
    pid_t pid = fork();
    if (pid == 0) {                                     /* twice, so that nobody has to wait for the program */
        if (fork() == 0) {
            setsid();
            execlp(prog, prog, (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }
    if (pid > 0)
        waitpid(pid, NULL, 0);
}

/* ---- settings -------------------------------------------------------------------------------------------------------------- */

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void send_set(struct app *a, const char *key, const char *value)
{
    if (a->settings)
        WLC_SEND(a->win.c, a->settings, LM_MELLAGUI_SETTINGS_V1_REQ_SET, { .s = key }, { .s = value });
}

static void send_int(struct app *a, const char *key, int v)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", v);
    send_set(a, key, buf);
}

static void settings_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)c; (void)id;
    struct app *a = data;
    if (op != LM_MELLAGUI_SETTINGS_V1_EVT_VALUE)
        return;
    const char *k = args[0].s, *v = args[1].s;
    bool on = !strcmp(v, "1");
    if (!strcmp(k, "sink-volume") && a->drag != I_VOL) a->vol = LM_CLAMP(atoi(v), 0, 100);
    else if (!strcmp(k, "sink-mute")) a->vol_mute = on;
    else if (!strcmp(k, "source-volume") && a->drag != I_MIC) a->mic = LM_CLAMP(atoi(v), 0, 100);
    else if (!strcmp(k, "source-mute")) a->mic_mute = on;
    else if (!strcmp(k, "sources")) a->have_mic = v[0] != 0 && strcmp(v, "-") != 0;
    else if (!strcmp(k, "dnd")) a->dnd = on;
    else if (!strcmp(k, "night-light")) a->night = strcmp(v, "off") != 0;
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

/* ---- drawing --------------------------------------------------------------------------------------------------------------- */

static void paint_button(struct app *a, int item, const char *text, bool on, bool avail, uint32_t on_rgb)
{
    struct wlwin *w = &a->win;
    struct rc r = rect_of(item);
    uint32_t bg = !avail ? lm_mix_rgb(lm_theme.button, lm_theme.background, 0.5) : on ? on_rgb : a->hover == item ? lm_theme.button_hover : lm_theme.button;
    wlwin_round_fill(w, r.x, r.y, r.w, r.h, lm_button_radius(r.h / 2), bg);
    uint32_t fg = !avail ? lm_theme.text_dim : on ? lm_theme.ink : lm_theme.text;
    int tw = lm_font_text_width(text);
    wlwin_text(w, r.x + (r.w - tw) / 2, r.y + (r.h - LM_FONT_H) / 2, text, 1, fg, r.w - 8);
}

static void paint_slider(struct app *a, int item, int value, bool muted, bool avail)
{
    struct wlwin *w = &a->win;
    struct rc r = rect_of(item);
    int lo = r.x + 8, hi = r.x + r.w - 8, cy = r.y + r.h / 2;
    wlwin_fill(w, lo, cy - 2, hi - lo, 4, avail ? lm_theme.button : lm_mix_rgb(lm_theme.button, lm_theme.background, 0.5));
    if (!avail)
        return;
    int kx = lo + (hi - lo) * value / 100;
    wlwin_fill(w, lo, cy - 2, kx - lo, 4, muted ? lm_theme.text_dim : lm_theme.accent);
    wlwin_round_fill(w, kx - 7, cy - 8, 14, 16, lm_button_radius(7), a->drag == item ? lm_theme.button_down : lm_theme.text);
    char v[8];
    snprintf(v, sizeof(v), "%d", value);
    wlwin_text(w, r.x + r.w - lm_font_text_width(v), r.y - 2, v, 1, lm_theme.text_dim, 0);
}

static void paint(struct wlwin *w)
{
    struct app *a = w->data;
    wlwin_fill(w, 0, 0, w->bw, w->bh, lm_theme.background);
    wlwin_text(w, PAD, 14, "Quick settings", 1, lm_theme.accent, 0);
    wlwin_fill(w, PAD, 14 + LM_FONT_H + 6, WIN_W - 2 * PAD, 1, lm_theme.border);
    paint_button(a, I_VOL_MUTE, a->vol_mute ? "Muted" : "Vol", a->vol_mute, true, lm_theme.alert);
    paint_slider(a, I_VOL, a->vol, a->vol_mute, true);
    paint_button(a, I_MIC_MUTE, a->mic_mute ? "Muted" : "Mic", a->mic_mute, a->have_mic, lm_theme.alert);
    paint_slider(a, I_MIC, a->mic, a->mic_mute, a->have_mic);
    paint_button(a, I_WIFI, "Wi-Fi", a->wifi == 1, a->wifi >= 0, lm_theme.accent);
    paint_button(a, I_BT, "Bluetooth", a->bt == 1, a->bt >= 0, lm_theme.accent);
    paint_button(a, I_DND, "Do not disturb", a->dnd, true, lm_theme.accent);
    paint_button(a, I_NIGHT, "Night light", a->night, true, lm_theme.accent);
    paint_button(a, I_LOCK, "Lock", false, true, 0);
    paint_button(a, I_DISPLAY, "Display", false, true, 0);
    paint_button(a, I_SETTINGS, "Settings", false, true, 0);
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
    return false;                                       /* it lives on, hidden */
}

static void drag_to(struct app *a, double x, bool force)
{
    int item = a->drag;
    int v = slider_value(item, x);
    if (item == I_VOL)
        a->vol = v;
    else
        a->mic = v;
    a->win.redraw = true;
    int64_t now = now_ms();
    if (force || now - a->last_send_ms >= 90) {                 /* (every change runs pactl in the compositor: not a hundred a second) */
        send_int(a, item == I_VOL ? "sink-volume" : "source-volume", v);
        a->last_send_ms = now;
    }
}

static void press(struct app *a, int item)
{
    switch (item) {
    case I_VOL: case I_MIC:
        a->drag = item;
        drag_to(a, a->px, true);
        if (item == I_VOL)
            send_set(a, "sink-beep", "1");                      /* a click on the volume can be heard at once */
        break;
    case I_VOL_MUTE: send_set(a, "sink-mute", a->vol_mute ? "0" : "1"); break;
    case I_MIC_MUTE: send_set(a, "source-mute", a->mic_mute ? "0" : "1"); break;
    case I_WIFI: if (a->wifi >= 0) toggle_wifi(a); break;
    case I_BT: if (a->bt >= 0) toggle_bt(a); break;
    case I_DND: send_set(a, "dnd", a->dnd ? "0" : "1"); break;
    case I_NIGHT: send_set(a, "night-light", a->night ? "off" : "on"); break;
    case I_LOCK: hide(a); send_set(a, "lock-now", "1"); break;
    case I_DISPLAY: hide(a); launch_sibling("mellagui-monitor"); break;
    case I_SETTINGS: hide(a); launch_sibling("mellagui-settings"); break;
    }
    a->win.redraw = true;
}

static void pointer_event(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
    struct app *a = w->data;
    int h;
    switch (op) {
    case LM_WL_POINTER_EVT_ENTER:
        a->drag = -1;                                           /* a drag that lost its button release (the card was hidden meanwhile) ends here */
        a->px = lm_fixed_to_double(args[2].f);
        a->py = lm_fixed_to_double(args[3].f);
        h = hit(a, a->px, a->py);
        if (h != a->hover) { a->hover = h; w->redraw = true; }
        break;
    case LM_WL_POINTER_EVT_LEAVE:
        if (a->hover >= 0) { a->hover = -1; w->redraw = true; }
        break;
    case LM_WL_POINTER_EVT_MOTION:
        a->px = lm_fixed_to_double(args[1].f);
        a->py = lm_fixed_to_double(args[2].f);
        if (a->drag >= 0) {
            drag_to(a, a->px, false);
            break;
        }
        h = hit(a, a->px, a->py);
        if (h != a->hover) { a->hover = h; w->redraw = true; }
        break;
    case LM_WL_POINTER_EVT_BUTTON:
        if (args[2].u != 272)
            break;
        if (args[3].u == 1) {
            h = hit(a, a->px, a->py);
            if (h >= 0)
                press(a, h);
        } else if (a->drag >= 0) {
            int item = a->drag;
            drag_to(a, a->px, true);
            a->drag = -1;
            if (item == I_VOL)
                send_set(a, "sink-beep", "1");                  /* ... and the level where the drag ended */
            w->redraw = true;
        }
        break;
    case LM_WL_POINTER_EVT_AXIS:                         /* the wheel over a slider: 5 % a step */
        if (args[1].u == 0) {
            h = hit(a, a->px, a->py);
            double v = lm_fixed_to_double(args[2].f);
            if ((h == I_VOL || h == I_MIC) && v != 0) {
                int *val = h == I_VOL ? &a->vol : &a->mic;
                *val = LM_CLAMP(*val + (v < 0 ? 5 : -5), 0, 100);
                send_int(a, h == I_VOL ? "sink-volume" : "source-volume", *val);
                if (h == I_VOL)
                    send_set(a, "sink-beep", "1");
                w->redraw = true;
            }
        }
        break;
    }
}

static void keyboard_event(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
    struct app *a = w->data;
    if (op == LM_WL_KEYBOARD_EVT_ENTER) {                /* it was just shown: ask the sound server and the radios again */
        send_set(a, "audio-rescan", "levels");
        refresh_radios(a);
        w->redraw = true;
        return;
    }
    if (op != LM_WL_KEYBOARD_EVT_KEY || args[3].u == 0)
        return;
    uint32_t ks = lm_keymap_keysym(args[2].u, a->win.mods);
    if (ks == LM_KS_Escape || ks == 'q' || ks == 'Q')
        hide(a);
}

/* ---- self test ------------------------------------------------------------------------------------------------------------- */

static int selftest_fail;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "selftest: %s:%d: %s\n", __FILE__, __LINE__, #cond); selftest_fail++; } } while (0)

static int selftest(void)
{
    struct rc r = rect_of(I_VOL);
    CHECK(slider_value(I_VOL, r.x - 50) == 0 && slider_value(I_VOL, r.x + r.w + 50) == 100);       /* the ends */
    int mid = slider_value(I_VOL, r.x + r.w / 2.0);
    CHECK(mid > 45 && mid < 55);
    CHECK(slider_value(I_VOL, r.x + r.w * 0.75) > slider_value(I_VOL, r.x + r.w * 0.25));
    for (int i = 0; i < I_COUNT; i++) {                                                           /* everything fits, nothing overlaps */
        struct rc a = rect_of(i);
        CHECK(a.x >= 0 && a.y >= 0 && a.x + a.w <= WIN_W && a.y + a.h <= WIN_H);
        for (int j = i + 1; j < I_COUNT; j++) {
            struct rc b = rect_of(j);
            CHECK(a.x + a.w <= b.x || b.x + b.w <= a.x || a.y + a.h <= b.y || b.y + b.h <= a.y);
        }
    }
    struct app app = { .have_mic = false, .hover = -1, .drag = -1 };
    CHECK(hit(&app, rect_of(I_MIC).x + 5, rect_of(I_MIC).y + 5) == -1);                            /* no microphone: its row does not react */
    app.have_mic = true;
    CHECK(hit(&app, rect_of(I_MIC).x + 5, rect_of(I_MIC).y + 5) == I_MIC);
    CHECK(hit(&app, rect_of(I_LOCK).x + 3, rect_of(I_LOCK).y + 3) == I_LOCK && hit(&app, 1, 1) == -1);
    if (selftest_fail)
        return 1;
    puts("mellagui-quick selftest ok");
    return 0;
}

int main(int argc, char **argv)
{
    wlwin_hidpi();                      /* draws through the wlwin_* calls only: sharp on a scaled screen */
    lm_theme_load_env();
    if (argc > 1 && !strcmp(argv[1], "--selftest"))
        return selftest();

    static const struct wlwin_ops ops = { .paint = paint, .global = global_event, .pointer = pointer_event, .keyboard = keyboard_event, .close = win_close };
    struct app a = { .hover = -1, .drag = -1, .wifi = -1, .bt = -1, .vol = 50, .mic = 50 };
    int rc = 1;
    if (wlwin_connect(&a.win, "mellagui-quick", &ops, &a) < 0)
        return 1;
    if (a.settings && wlc_roundtrip(a.win.c) < 0)
        goto out;
    refresh_radios(&a);
    if (wlwin_open(&a.win, "Quick settings", "mellagui.quick", WIN_W, WIN_H, WIN_W, WIN_H) < 0)
        goto out;
    a.win.fixed_size = false;
    while (wlwin_step(&a.win, 1000))
        ;
    rc = 0;
out:
    if (a.win.c)
        wlwin_destroy(&a.win);
    return rc;
}
