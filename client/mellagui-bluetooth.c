/*
 * mellagui-bluetooth - the Bluetooth devices around, in a window (the panel button's menu: "Devices...").
 *
 *   mellagui-bluetooth             list the paired devices and, while it scans, the ones in range; connect, disconnect, pair, forget
 *   mellagui-bluetooth --selftest
 *
 * One interactive `bluetoothctl` runs on a pty for as long as the window is open ($MELLAGUI_BLUETOOTHCTL names another program). It scans (`scan on`),
 * its [NEW] / [CHG] / [DEL] lines keep the list alive (names, signal strength, connected, paired) and it is also the pairing agent: when a device
 * wants the two sides to compare a code ("Confirm passkey 123456 (yes/no):"), a PIN or a passkey typed here, or shows a code that has to be typed on the
 * device, a small question appears in the window. Scanning stops and the session ends with the window. The first state (what is paired, what is
 * connected) comes from one-shot calls (btctl.h) before the window opens.
 *
 *   Up / Down / click   choose            Enter or the button     connect / disconnect / pair
 *   Delete or the x     forget a paired device                    S  scanning on / off      P  the radio on / off
 *   U                   show / hide the devices that have no name (most of what a scan finds are such beacons)
 *   Esc                 answer "no" to a question, else close
 *
 * A second copy does not start (a lock file per Wayland display in $XDG_RUNTIME_DIR). MELLAGUI_BT_DUMP=FILE writes the window's state there after every change (tests).
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "btctl.h"
#include "font.h"
#include "keymap.h"
#include "protocol.h"
#include "theme.h"
#include "util.h"
#include "wlc.h"
#include "wlwin.h"

#define MAX_DEV 96
#define ROW_H 46
#define HEAD_H 56
#define FOOT_H 52

struct dev {
    char mac[18];
    char name[80];
    bool named, paired, connected;
    int rssi;                           /* dBm, 0 = not known */
    int battery;                        /* the device's own battery, 0..100, -1 = not known */
    char icon[32];
};

enum modal { M_NONE, M_CONFIRM, M_INPUT, M_SHOW };

struct app {
    struct wlwin win;
    uint32_t settings;
    /* the session */
    int fd;
    pid_t pid;
    char buf[16384];
    size_t len;
    /* the controller */
    bool have, powered, discovering;
    /* the devices */
    struct dev dev[MAX_DEV];
    int n;
    char sel[18];                       /* MAC of the chosen one */
    int top;
    bool show_unnamed;
    char msg[200];
    bool msg_error;
    char busy_mac[18];                  /* a command for this device is running */
    /* the question */
    enum modal modal;
    char modal_text[200];
    char modal_mac[18];
    char input[32];
    double px, py;
    const char *dump;
};

static void copy(char *dst, size_t n, const char *src)
{
    snprintf(dst, n, "%s", src);
}

static struct dev *find_dev(struct app *a, const char *mac)
{
    for (int i = 0; i < a->n; i++)
        if (!strcmp(a->dev[i].mac, mac))
            return &a->dev[i];
    return NULL;
}

/* A scan names the devices that have no name by their address, with dashes. */
static bool is_address_name(const char *mac, const char *name)
{
    if (strlen(name) != 17)
        return false;
    for (int i = 0; i < 17; i++)
        if (i % 3 == 2 ? name[i] != '-' && name[i] != ':' : toupper((unsigned char)name[i]) != toupper((unsigned char)mac[i]))
            return false;
    return true;
}

static struct dev *add_dev(struct app *a, const char *mac, const char *name)
{
    struct dev *d = find_dev(a, mac);
    if (!d) {
        if (a->n >= MAX_DEV)
            return NULL;
        d = &a->dev[a->n++];
        memset(d, 0, sizeof(*d));
        d->battery = -1;
        copy(d->mac, sizeof(d->mac), mac);
    }
    if (name && *name) {
        copy(d->name, sizeof(d->name), name);
        d->named = !is_address_name(mac, name);
    }
    if (!d->name[0]) {
        copy(d->name, sizeof(d->name), mac);
        d->named = false;
    }
    return d;
}

static void remove_dev(struct app *a, const char *mac)
{
    for (int i = 0; i < a->n; i++)
        if (!strcmp(a->dev[i].mac, mac)) {
            memmove(&a->dev[i], &a->dev[i + 1], sizeof(a->dev[0]) * (size_t)(a->n - i - 1));
            a->n--;
            return;
        }
}

static void set_msg(struct app *a, bool error, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(a->msg, sizeof(a->msg), fmt, ap);
    va_end(ap);
    a->msg_error = error;
}

/* ---- the session ---------------------------------------------------------------------------------------------------------------- */

static bool sess_start(struct app *a)
{
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || grantpt(master) < 0 || unlockpt(master) < 0)
        return false;
    char *slave_name = ptsname(master);
    if (!slave_name) {
        close(master);
        return false;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(master);
        return false;
    }
    if (pid == 0) {
        setsid();
        int s = open(slave_name, O_RDWR);
        if (s < 0)
            _exit(126);
        ioctl(s, TIOCSCTTY, 0);
        struct termios t;
        if (tcgetattr(s, &t) == 0) {                            /* what we type is not echoed back */
            t.c_lflag &= ~(tcflag_t)(ECHO | ECHONL);
            tcsetattr(s, TCSANOW, &t);
        }
        dup2(s, 0);
        dup2(s, 1);
        dup2(s, 2);
        if (s > 2)
            close(s);
        close(master);
        setenv("LC_ALL", "C", 1);
        setenv("TERM", "dumb", 1);
        execlp(bluetoothctl_program(), bluetoothctl_program(), (char *)NULL);
        _exit(127);
    }
    fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);
    a->fd = master;
    a->pid = pid;
    return true;
}

static void sess_send(struct app *a, const char *fmt, ...)
{
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    if (n <= 0 || a->fd < 0)
        return;
    line[n++] = '\n';
    if (write(a->fd, line, (size_t)n) < 0) {}
}

static void sess_stop(struct app *a)
{
    if (a->fd < 0)
        return;
    sess_send(a, "scan off");
    sess_send(a, "quit");
    uint64_t end = (uint64_t)time(NULL) + 2;
    int st;
    while ((uint64_t)time(NULL) < end && waitpid(a->pid, &st, WNOHANG) == 0)
        usleep(50000);
    if (waitpid(a->pid, &st, WNOHANG) == 0) {
        kill(a->pid, SIGKILL);
        waitpid(a->pid, &st, 0);
    }
    close(a->fd);
    a->fd = -1;
}

/* ---- what bluetoothctl says --------------------------------------------------------------------------------------------------------- */

/* A line starts with the prompt "[bluetooth]# " when it arrived while the prompt was showing: skip it (but not "[NEW]" and its kind). */
static const char *skip_prompt(const char *l)
{
    for (;;) {
        while (*l == ' ')
            l++;
        if (*l != '[')
            return l;
        const char *e = strstr(l, "]# ");
        if (!e || e - l > 48 || !strncmp(l, "[NEW]", 5) || !strncmp(l, "[CHG]", 5) || !strncmp(l, "[DEL]", 5) || !strncmp(l, "[agent]", 7))
            return l;
        l = e + 3;
    }
}

static bool yesno(const char *v)
{
    return !strncmp(v, "yes", 3);
}

static void open_modal(struct app *a, enum modal m, const char *mac, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(a->modal_text, sizeof(a->modal_text), fmt, ap);
    va_end(ap);
    a->modal = m;
    copy(a->modal_mac, sizeof(a->modal_mac), mac ? mac : "");
    a->input[0] = 0;
}

static const char *dev_name(struct app *a, const char *mac)
{
    struct dev *d = find_dev(a, mac);
    return d ? d->name : mac;
}

static void handle_line(struct app *a, const char *raw)
{
    const char *l = skip_prompt(raw);
    char mac[18], v[160];
    if (!strncmp(l, "[NEW] Device ", 13) || !strncmp(l, "Device ", 7)) {
        const char *p = l + (l[0] == '[' ? 13 : 7);
        snprintf(mac, sizeof(mac), "%.17s", p);
        if (mac_ok(mac)) {
            const char *nm = p + 17;
            while (*nm == ' ')
                nm++;
            add_dev(a, mac, nm);
        }
    } else if (!strncmp(l, "[DEL] Device ", 13)) {
        snprintf(mac, sizeof(mac), "%.17s", l + 13);
        struct dev *d = find_dev(a, mac);
        if (d && !d->paired)
            remove_dev(a, mac);
    } else if (!strncmp(l, "[CHG] Device ", 13)) {
        const char *p = l + 13;
        snprintf(mac, sizeof(mac), "%.17s", p);
        if (!mac_ok(mac))
            return;
        struct dev *d = add_dev(a, mac, NULL);
        const char *k = p + 17;
        while (*k == ' ')
            k++;
        if (!d)
            return;
        if (!strncmp(k, "Name: ", 6) || !strncmp(k, "Alias: ", 7)) {
            const char *val = strchr(k, ' ') + 1;
            if (!is_address_name(mac, val) || !d->named)
                add_dev(a, mac, val);
        } else if (!strncmp(k, "RSSI", 4)) {
            const char *paren = strchr(k, '(');
            d->rssi = paren ? atoi(paren + 1) : 0;                     /* "RSSI: 0xffffffb8 (-72)", "RSSI is nil" */
        } else if (!strncmp(k, "Connected: ", 11)) {
            d->connected = yesno(k + 11);
        } else if (!strncmp(k, "Paired: ", 8)) {
            d->paired = yesno(k + 8);
        } else if (!strncmp(k, "Icon: ", 6)) {
            copy(d->icon, sizeof(d->icon), k + 6);
        } else if (!strncmp(k, "Battery Percentage", 18)) {
            d->battery = bt_battery(k);                                  /* "Battery Percentage: 0x56 (86)"; "... is nil" when it went away */
        }
    } else if (!strncmp(l, "[CHG] Controller ", 17)) {
        const char *k = l + 17 + 17;
        while (*k == ' ')
            k++;
        if (!strncmp(k, "Powered: ", 9))
            a->powered = yesno(k + 9);
        else if (!strncmp(k, "Discovering: ", 13))
            a->discovering = yesno(k + 13);
    } else if (strstr(l, "Pairing successful")) {
        if (a->modal != M_NONE)
            a->modal = M_NONE;
        const char *mac_p = a->busy_mac[0] ? a->busy_mac : NULL;
        if (mac_p) {
            struct dev *d = find_dev(a, mac_p);
            if (d)
                d->paired = true;
            sess_send(a, "trust %s", mac_p);                            /* so that it connects by itself next time */
            sess_send(a, "connect %s", mac_p);
            set_msg(a, false, "Paired with %s. Connecting...", dev_name(a, mac_p));
        }
    } else if (strstr(l, "Failed to pair")) {
        a->modal = M_NONE;
        const char *why = strstr(l, "org.bluez.Error."), *help = bt_explain(l);
        if (help)
            set_msg(a, true, "%s", help);
        else
            set_msg(a, true, "Pairing failed%s%s", why ? ": " : "", why ? why + 16 : "");
        a->busy_mac[0] = 0;
    } else if (strstr(l, "Connection successful")) {
        if (a->busy_mac[0])
            set_msg(a, false, "Connected to %s", dev_name(a, a->busy_mac));
        a->busy_mac[0] = 0;
    } else if (strstr(l, "Failed to connect")) {
        const char *why = strstr(l, "org.bluez.Error."), *help = bt_explain(l);
        if (help)
            set_msg(a, true, "%s", help);
        else
            set_msg(a, true, "Could not connect%s%s", why ? ": " : "", why ? why + 16 : "");
        a->busy_mac[0] = 0;
    } else if (strstr(l, "Successful disconnected")) {
        set_msg(a, false, "Disconnected");
        a->busy_mac[0] = 0;
    } else if (strstr(l, "has been removed")) {
        set_msg(a, false, "Forgotten");
    } else if (strstr(l, "Failed to set power")) {
        set_msg(a, true, "Could not switch the radio (is it blocked? rfkill unblock bluetooth)");
    } else if (sscanf(l, "[agent] Passkey: %31s", v) == 1 || sscanf(l, "[agent] PIN code: %31s", v) == 1) {
        open_modal(a, M_SHOW, a->busy_mac, "Type this code on %s:   %s", a->busy_mac[0] ? dev_name(a, a->busy_mac) : "the device", v);
    }
}

/* A prompt waits for an answer, with no line end: look at the tail. */
static void handle_tail(struct app *a, const char *tail)
{
    const char *l = skip_prompt(tail);
    if (a->modal == M_CONFIRM || a->modal == M_INPUT)
        return;
    if (strstr(l, "(yes/no):")) {
        char code[16] = "";
        const char *p = strstr(l, "passkey ");
        if (p && sscanf(p + 8, "%15s", code) == 1)
            open_modal(a, M_CONFIRM, a->busy_mac, "Does %s show the code %s too?", a->busy_mac[0] ? dev_name(a, a->busy_mac) : "the device", code);
        else if (strstr(l, "uthoriz"))
            open_modal(a, M_CONFIRM, a->busy_mac, "A device asks to connect. Allow it?");
        else
            open_modal(a, M_CONFIRM, a->busy_mac, "%.150s", l);
    } else if (strstr(l, "Enter PIN code:") || strstr(l, "Enter passkey")) {
        open_modal(a, M_INPUT, a->busy_mac, "Type the %s of %s", strstr(l, "PIN") ? "PIN code" : "passkey", a->busy_mac[0] ? dev_name(a, a->busy_mac) : "the device");
    }
}

static void sess_read(struct app *a)
{
    if (a->fd < 0)
        return;
    for (;;) {
        char tmp[2048];
        ssize_t k = read(a->fd, tmp, sizeof(tmp));
        if (k <= 0)
            break;
        if (a->len + (size_t)k >= sizeof(a->buf))
            a->len = 0;                                         /* never so much: start over rather than overflow */
        memcpy(a->buf + a->len, tmp, (size_t)k);
        a->len += (size_t)k;
        a->buf[a->len] = 0;
    }
    bt_strip(a->buf);
    a->len = strlen(a->buf);
    char *start = a->buf, *nl;
    while ((nl = strchr(start, '\n'))) {
        *nl = 0;
        handle_line(a, start);
        start = nl + 1;
    }
    size_t rest = strlen(start);
    memmove(a->buf, start, rest + 1);
    a->len = rest;
    if (rest)
        handle_tail(a, a->buf);
    a->win.redraw = true;
}

/* ---- the list --------------------------------------------------------------------------------------------------------------------- */

static int rank(const struct dev *d)
{
    return d->connected ? 0 : d->paired ? 1 : d->named ? 2 : 3;
}

/* The shown devices in order: connected, paired, nearby with a name (strongest first), and the nameless ones when asked for. */
static int shown(struct app *a, int *idx)
{
    int n = 0;
    for (int r = 0; r < 4; r++) {
        int first = n;
        for (int i = 0; i < a->n; i++)
            if (rank(&a->dev[i]) == r && (r < 3 || a->show_unnamed))
                idx[n++] = i;
        for (int i = first + 1; i < n; i++)                     /* a stable insertion sort by signal within a group (nearby ones only) */
            for (int j = i; r >= 2 && j > first && a->dev[idx[j]].rssi > a->dev[idx[j - 1]].rssi; j--) {
                int t = idx[j];
                idx[j] = idx[j - 1];
                idx[j - 1] = t;
            }
    }
    return n;
}

static int hidden_count(struct app *a)
{
    int h = 0;
    for (int i = 0; i < a->n; i++)
        h += rank(&a->dev[i]) == 3;
    return h;
}

static int rows_visible(const struct app *a)
{
    return LM_MAX((a->win.bh - HEAD_H - FOOT_H) / ROW_H, 1);
}

static void clamp_view(struct app *a)
{
    int idx[MAX_DEV], n = shown(a, idx), vis = rows_visible(a), at = -1;
    for (int i = 0; i < n; i++)
        if (!strcmp(a->dev[idx[i]].mac, a->sel))
            at = i;
    if (at < 0 && n > 0) {
        copy(a->sel, sizeof(a->sel), a->dev[idx[0]].mac);
        at = 0;
    }
    if (at >= 0) {
        if (at < a->top)
            a->top = at;
        if (at >= a->top + vis)
            a->top = at - vis + 1;
    }
    a->top = LM_CLAMP(a->top, 0, LM_MAX(n - vis, 0));
}

static const char *action_label(const struct dev *d)
{
    return d->connected ? "Disconnect" : d->paired ? "Connect" : "Pair";
}

static void act(struct app *a, struct dev *d)
{
    if (!d || a->fd < 0)
        return;
    copy(a->busy_mac, sizeof(a->busy_mac), d->mac);
    if (d->connected) {
        sess_send(a, "disconnect %s", d->mac);
        set_msg(a, false, "Disconnecting from %s...", d->name);
    } else if (d->paired) {
        sess_send(a, "connect %s", d->mac);
        set_msg(a, false, "Connecting to %s...", d->name);
    } else {
        sess_send(a, "pair %s", d->mac);
        set_msg(a, false, "Pairing with %s...", d->name);
    }
}

static void forget(struct app *a, struct dev *d)
{
    if (d && d->paired && a->fd >= 0) {
        sess_send(a, "remove %s", d->mac);
        d->paired = d->connected = false;
        if (!d->named)
            remove_dev(a, d->mac);
        set_msg(a, false, "Forgetting %s...", d->name);
    }
}

static void answer(struct app *a, const char *text)
{
    sess_send(a, "%s", text);
    a->modal = M_NONE;
    a->input[0] = 0;
}

/* ---- painting and input -------------------------------------------------------------------------------------------------------- */

struct box { int x, y, w, h; };

static bool in(double px, double py, struct box b)
{
    return px >= b.x && px < b.x + b.w && py >= b.y && py < b.y + b.h;
}

static struct box btn_power(const struct app *a) { return (struct box){ a->win.bw - 100, 14, 86, 28 }; }
static struct box btn_scan(const struct app *a) { return (struct box){ a->win.bw - 200, 14, 90, 28 }; }
static struct box row_box(const struct app *a, int r) { return (struct box){ 10, HEAD_H + r * ROW_H, a->win.bw - 20, ROW_H - 4 }; }
static struct box row_act(const struct app *a, int r) { struct box b = row_box(a, r); return (struct box){ b.x + b.w - 130, b.y + 8, 96, 28 }; }
static struct box row_x(const struct app *a, int r) { struct box b = row_box(a, r); return (struct box){ b.x + b.w - 28, b.y + 8, 22, 28 }; }
static struct box modal_box(const struct app *a) { return (struct box){ 30, a->win.bh / 2 - 80, a->win.bw - 60, 160 }; }
static struct box modal_b1(const struct app *a) { struct box m = modal_box(a); return (struct box){ m.x + m.w - 200, m.y + m.h - 44, 90, 30 }; }
static struct box modal_b2(const struct app *a) { struct box m = modal_box(a); return (struct box){ m.x + m.w - 100, m.y + m.h - 44, 90, 30 }; }

static void button(struct wlwin *w, struct box b, const char *label, bool hot, uint32_t bg)
{
    wlwin_fill(w, b.x, b.y, b.w, b.h, hot ? lm_theme.button_hover : bg);
    int tw = lm_font_text_width(label);
    wlwin_text(w, b.x + (b.w - tw) / 2, b.y + (b.h - LM_FONT_H) / 2, label, 1, lm_theme.text, 0);
}

static void dump_state(struct app *a)
{
    if (!a->dump)
        return;
    char tmp[400];
    snprintf(tmp, sizeof(tmp), "%s.tmp", a->dump);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return;
    fprintf(f, "powered=%d discovering=%d have=%d sel=%s modal=%d busy=%s unnamed_hidden=%d show_unnamed=%d\nmsg=%s\nmodal_text=%s\n", a->powered, a->discovering,
            a->have, a->sel, a->modal, a->busy_mac, a->show_unnamed ? 0 : hidden_count(a), a->show_unnamed, a->msg, a->modal_text);
    int idx[MAX_DEV], n = shown(a, idx);
    for (int i = 0; i < n; i++) {
        const struct dev *d = &a->dev[idx[i]];
        fprintf(f, "dev %s paired=%d connected=%d rssi=%d battery=%d name=%s\n", d->mac, d->paired, d->connected, d->rssi, d->battery, d->name);
    }
    if (fclose(f) == 0)
        rename(tmp, a->dump);                                   /* all at once: a reader never sees half of it */
}

static void paint(struct wlwin *w)
{
    struct app *a = w->data;
    clamp_view(a);
    wlwin_fill(w, 0, 0, w->bw, w->bh, lm_theme.background);
    wlwin_text(w, 14, 12, "Bluetooth", 1, lm_theme.accent, 0);
    const char *state = !a->have ? "no controller" : !a->powered ? "off" : a->discovering ? "on, scanning..." : "on";
    wlwin_text(w, 14, 32, state, 1, lm_theme.text_dim, 0);
    if (a->have) {
        button(w, btn_scan(a), a->discovering ? "Stop scan" : "Scan", in(a->px, a->py, btn_scan(a)), lm_theme.button);
        button(w, btn_power(a), a->powered ? "Turn off" : "Turn on", in(a->px, a->py, btn_power(a)), lm_theme.button);
    }
    wlwin_fill(w, 10, HEAD_H - 8, w->bw - 20, 1, lm_theme.border);
    int idx[MAX_DEV], n = shown(a, idx), vis = rows_visible(a);
    if (n == 0)
        wlwin_text(w, 14, HEAD_H + 10, a->powered ? "No devices yet. Put the one you want in pairing mode." : "Bluetooth is off.", 1, lm_theme.text_dim, w->bw - 28);
    for (int r = 0; r < vis && a->top + r < n; r++) {
        const struct dev *d = &a->dev[idx[a->top + r]];
        struct box b = row_box(a, r);
        bool sel = !strcmp(d->mac, a->sel);
        if (sel)
            wlwin_fill(w, b.x, b.y, b.w, b.h, lm_theme.button);
        int room = b.w - 160 - (d->connected && d->battery >= 0 ? 80 : 0);          /* (the battery takes the room left of the button) */
        wlwin_text(w, b.x + 10, b.y + 5, d->name, 1, d->named ? lm_theme.text : lm_theme.text_dim, room);
        char sub[96];
        const char *st = d->connected ? "Connected" : d->paired ? "Paired" : "Nearby";
        if (d->rssi < 0)
            snprintf(sub, sizeof(sub), "%s   %s   %d dBm%s%s", st, d->mac, d->rssi, d->icon[0] ? "   " : "", d->icon);
        else
            snprintf(sub, sizeof(sub), "%s   %s%s%s", st, d->mac, d->icon[0] ? "   " : "", d->icon);
        wlwin_text(w, b.x + 10, b.y + 23, sub, 1, d->connected ? lm_theme.ok : lm_theme.text_dim, room);
        if (d->connected && d->battery >= 0) {                    /* the device's own battery: a small cell, filled to its level, and the number */
            int bx = b.x + b.w - 130 - 76, by = b.y + 13;
            uint32_t col = d->battery > 40 ? lm_theme.ok : d->battery > 15 ? lm_theme.warn : lm_theme.alert;
            wlwin_fill(w, bx, by, 24, 12, lm_theme.text_dim);
            wlwin_fill(w, bx + 24, by + 3, 2, 6, lm_theme.text_dim);
            wlwin_fill(w, bx + 1, by + 1, 22, 10, lm_theme.background);
            wlwin_fill(w, bx + 2, by + 2, LM_MAX(d->battery * 20 / 100, d->battery > 0 ? 1 : 0), 8, col);
            char pct[8];
            snprintf(pct, sizeof(pct), "%d%%", d->battery);
            wlwin_text(w, bx + 32, b.y + 15, pct, 1, col, 0);
        }
        bool busy = !strcmp(d->mac, a->busy_mac);
        button(w, row_act(a, r), busy ? "..." : action_label(d), in(a->px, a->py, row_act(a, r)), d->connected ? lm_theme.button_alt : lm_theme.button);
        if (d->paired)
            wlwin_text(w, row_x(a, r).x + 7, row_x(a, r).y + 6, "x", 1, in(a->px, a->py, row_x(a, r)) ? lm_theme.alert : lm_theme.text_dim, 0);
    }
    int fy = w->bh - FOOT_H + 6;
    wlwin_fill(w, 10, w->bh - FOOT_H, w->bw - 20, 1, lm_theme.border);
    if (a->msg[0])
        wlwin_text(w, 14, fy, a->msg, 1, a->msg_error ? lm_theme.error : lm_theme.text, w->bw - 28);
    char hint[160];
    int h = a->show_unnamed ? 0 : hidden_count(a);
    if (h)
        snprintf(hint, sizeof(hint), "%d unnamed device%s hidden (U shows them)   Esc closes", h, h == 1 ? "" : "s");
    else
        snprintf(hint, sizeof(hint), "Enter: connect / pair   Delete: forget   S: scan   Esc closes");
    wlwin_text(w, 14, fy + 22, hint, 1, lm_theme.text_dim, w->bw - 28);
    if (a->modal != M_NONE) {
        wlwin_dim(w, 0.5);                                      /* the list dims */
        struct box m = modal_box(a);
        wlwin_fill(w, m.x - 1, m.y - 1, m.w + 2, m.h + 2, lm_theme.accent);
        wlwin_fill(w, m.x, m.y, m.w, m.h, lm_theme.card);
        wlwin_text(w, m.x + 14, m.y + 14, a->modal_text, 1, lm_theme.text, m.w - 28);
        if (a->modal == M_INPUT) {
            wlwin_fill(w, m.x + 14, m.y + 50, m.w - 28, 28, lm_theme.field);
            wlwin_text(w, m.x + 20, m.y + 56, a->input, 1, lm_theme.text, m.w - 40);
            wlwin_fill(w, m.x + 20 + lm_font_text_width(a->input), m.y + 56, 2, LM_FONT_H, lm_theme.accent);
            button(w, modal_b1(a), "OK", in(a->px, a->py, modal_b1(a)), lm_theme.ok);
            button(w, modal_b2(a), "Cancel", in(a->px, a->py, modal_b2(a)), lm_theme.button);
        } else if (a->modal == M_CONFIRM) {
            button(w, modal_b1(a), "Yes", in(a->px, a->py, modal_b1(a)), lm_theme.ok);
            button(w, modal_b2(a), "No", in(a->px, a->py, modal_b2(a)), lm_theme.button);
        } else {
            button(w, modal_b2(a), "Cancel", in(a->px, a->py, modal_b2(a)), lm_theme.button);
        }
    }
    dump_state(a);
}

static void modal_yes(struct app *a)
{
    if (a->modal == M_CONFIRM)
        answer(a, "yes");
    else if (a->modal == M_INPUT && a->input[0])
        answer(a, a->input);
}

static void modal_no(struct app *a)
{
    if (a->modal == M_CONFIRM) {
        answer(a, "no");
    } else if (a->modal == M_INPUT) {
        answer(a, "");                                          /* an empty line cancels the pairing */
        sess_send(a, "cancel-pairing %s", a->busy_mac);
    } else {
        sess_send(a, "cancel-pairing %s", a->busy_mac);
        a->modal = M_NONE;
    }
    set_msg(a, false, "Pairing cancelled");
}

static void pointer_event(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
    struct app *a = w->data;
    if (op == LM_WL_POINTER_EVT_ENTER || op == LM_WL_POINTER_EVT_MOTION) {
        int base = op == LM_WL_POINTER_EVT_ENTER ? 2 : 1;
        a->px = lm_fixed_to_double(args[base].f);
        a->py = lm_fixed_to_double(args[base + 1].f);
        w->redraw = true;
        return;
    }
    if (op == LM_WL_POINTER_EVT_AXIS && args[1].u == 0) {
        double v = lm_fixed_to_double(args[2].f);
        a->top += v > 0 ? 2 : v < 0 ? -2 : 0;
        w->redraw = true;
        return;
    }
    if (op != LM_WL_POINTER_EVT_BUTTON || args[2].u != 272 || args[3].u != 1)
        return;
    w->redraw = true;
    if (a->modal != M_NONE) {
        if (in(a->px, a->py, modal_b2(a)))
            modal_no(a);
        else if (a->modal != M_SHOW && in(a->px, a->py, modal_b1(a)))
            modal_yes(a);
        return;
    }
    if (a->have && in(a->px, a->py, btn_power(a))) {
        sess_send(a, "power %s", a->powered ? "off" : "on");
        return;
    }
    if (a->have && in(a->px, a->py, btn_scan(a))) {
        sess_send(a, "scan %s", a->discovering ? "off" : "on");
        return;
    }
    int idx[MAX_DEV], n = shown(a, idx), vis = rows_visible(a);
    for (int r = 0; r < vis && a->top + r < n; r++) {
        struct dev *d = &a->dev[idx[a->top + r]];
        if (!in(a->px, a->py, row_box(a, r)))
            continue;
        copy(a->sel, sizeof(a->sel), d->mac);
        if (in(a->px, a->py, row_act(a, r)))
            act(a, d);
        else if (d->paired && in(a->px, a->py, row_x(a, r)))
            forget(a, d);
        return;
    }
}

static void keyboard_event(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
    struct app *a = w->data;
    if (op != LM_WL_KEYBOARD_EVT_KEY || args[3].u == 0)
        return;
    uint32_t ks = lm_keymap_keysym(args[2].u, w->mods);
    w->redraw = true;
    if (a->modal == M_INPUT) {
        size_t n = strlen(a->input);
        if (ks == LM_KS_Escape)
            modal_no(a);
        else if (ks == LM_KS_Return || ks == LM_KS_KP_Enter)
            modal_yes(a);
        else if (ks == LM_KS_BackSpace && n > 0)
            a->input[n - 1] = 0;
        else if (ks >= 0x20 && ks < 0x7f && n + 1 < sizeof(a->input)) {
            a->input[n] = (char)ks;
            a->input[n + 1] = 0;
        }
        return;
    }
    if (a->modal == M_CONFIRM) {
        if (ks == 'y' || ks == 'Y' || ks == LM_KS_Return || ks == LM_KS_KP_Enter)
            modal_yes(a);
        else if (ks == 'n' || ks == 'N' || ks == LM_KS_Escape)
            modal_no(a);
        return;
    }
    if (a->modal == M_SHOW) {
        if (ks == LM_KS_Escape)
            modal_no(a);
        return;
    }
    int idx[MAX_DEV], n = shown(a, idx), at = 0;
    for (int i = 0; i < n; i++)
        if (!strcmp(a->dev[idx[i]].mac, a->sel))
            at = i;
    struct dev *cur = n ? &a->dev[idx[at]] : NULL;
    if (ks == LM_KS_Escape)
        w->running = false;
    else if (ks == LM_KS_Down && n)
        copy(a->sel, sizeof(a->sel), a->dev[idx[LM_MIN(at + 1, n - 1)]].mac);
    else if (ks == LM_KS_Up && n)
        copy(a->sel, sizeof(a->sel), a->dev[idx[LM_MAX(at - 1, 0)]].mac);
    else if (ks == LM_KS_Home && n)
        copy(a->sel, sizeof(a->sel), a->dev[idx[0]].mac);
    else if (ks == LM_KS_End && n)
        copy(a->sel, sizeof(a->sel), a->dev[idx[n - 1]].mac);
    else if (ks == LM_KS_Return || ks == LM_KS_KP_Enter)
        act(a, cur);
    else if (ks == LM_KS_Delete)
        forget(a, cur);
    else if (ks == 's' || ks == 'S')
        sess_send(a, "scan %s", a->discovering ? "off" : "on");
    else if (ks == 'p' || ks == 'P')
        sess_send(a, "power %s", a->powered ? "off" : "on");
    else if (ks == 'u' || ks == 'U')
        a->show_unnamed = !a->show_unnamed;
}

static void settings_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)c; (void)id; (void)op; (void)args; (void)data;
}

static void global_event(struct wlwin *w, uint32_t name, const char *iface, uint32_t ver)
{
    (void)ver;
    struct app *a = w->data;
    if (!strcmp(iface, "mellagui_settings_v1"))
        a->settings = wlc_bind(w->c, w->registry, name, &lm_mellagui_settings_v1_interface, 1, settings_event, a);
}

/* ---- start ------------------------------------------------------------------------------------------------------------------------ */

static void load_state(struct app *a)
{
    struct btinfo bi;
    read_bt(&bi);                                               /* (the one-shot calls: what is paired and connected) */
    a->have = bi.have;
    a->powered = bi.powered;
    for (int i = 0; i < bi.n; i++) {
        struct dev *d = add_dev(a, bi.dev[i].mac, bi.dev[i].name);
        if (d) {
            d->paired = bi.dev[i].paired;
            d->connected = bi.dev[i].connected;
            d->battery = bi.dev[i].battery;
        }
    }
}

/* The battery of a connected device is also read again now and then: the session only hears of a change that bluez announces, and a speaker may not announce
 * it (or the announcement went by while the device was not connected yet). A few milliseconds per device, every REFRESH_MS. */
#define REFRESH_MS 10000

static void refresh_batteries(struct app *a)
{
    char text[2048];
    for (int i = 0; i < a->n; i++) {
        struct dev *d = &a->dev[i];
        if (!d->connected || btctl(text, sizeof(text), 1500, "info", d->mac, NULL) != 0)
            continue;
        int b = bt_battery(text);
        if (b != d->battery) {
            d->battery = b;
            a->win.redraw = true;
        }
    }
}

static int selftest(void)
{
    int bad = 0;
#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "mellagui-bluetooth selftest: " __VA_ARGS__); fputc('\n', stderr); bad++; } } while (0)
    static struct app a;
    handle_line(&a, "[bluetooth]# [NEW] Device 11:22:33:44:55:66 My Headset");
    handle_line(&a, "[NEW] Device AA-BB-CC:DD:EE:FF AA-BB-CC-DD-EE-FF");
    handle_line(&a, "[bluetooth]# [NEW] Device AA:BB:CC:DD:EE:FF AA-BB-CC-DD-EE-FF");
    CHECK(a.n == 2 && !strcmp(a.dev[0].name, "My Headset") && a.dev[0].named && !a.dev[1].named, "NEW (%d)", a.n);
    handle_line(&a, "[CHG] Device 11:22:33:44:55:66 RSSI: 0xffffffb8 (-72)");
    CHECK(a.dev[0].rssi == -72, "RSSI %d", a.dev[0].rssi);
    handle_line(&a, "[CHG] Device 11:22:33:44:55:66 RSSI is nil");
    CHECK(a.dev[0].rssi == 0, "RSSI nil");
    handle_line(&a, "[CHG] Device 11:22:33:44:55:66 Connected: yes");
    handle_line(&a, "[CHG] Device 11:22:33:44:55:66 Paired: yes");
    CHECK(a.dev[0].connected && a.dev[0].paired, "connected / paired");
    handle_line(&a, "[CHG] Device AA:BB:CC:DD:EE:FF Name: Speaker");
    CHECK(a.dev[1].named && !strcmp(a.dev[1].name, "Speaker"), "Name %s", a.dev[1].name);
    handle_line(&a, "[CHG] Device 11:22:33:44:55:66 Battery Percentage: 0x56 (86)");
    CHECK(a.dev[0].battery == 86, "battery %d", a.dev[0].battery);
    handle_line(&a, "[CHG] Device 11:22:33:44:55:66 Battery Percentage is nil");
    CHECK(a.dev[0].battery == -1, "battery gone");
    handle_line(&a, "[CHG] Controller 88:53:95:2B:EE:5A Discovering: yes");
    CHECK(a.discovering, "Discovering");
    handle_line(&a, "[DEL] Device AA:BB:CC:DD:EE:FF");
    CHECK(a.n == 1, "DEL");
    handle_tail(&a, "[agent] Confirm passkey 123456 (yes/no): ");
    CHECK(a.modal == M_CONFIRM && strstr(a.modal_text, "123456"), "confirm modal '%s'", a.modal_text);
    a.modal = M_NONE;
    handle_tail(&a, "[agent] Enter PIN code: ");
    CHECK(a.modal == M_INPUT, "pin modal");
    a.modal = M_NONE;
    handle_line(&a, "[agent] Passkey: 482913");
    CHECK(a.modal == M_SHOW && strstr(a.modal_text, "482913"), "show modal");
    CHECK(!strcmp(skip_prompt("[bluetooth]# Agent registered"), "Agent registered") && !strncmp(skip_prompt("[CHG] Device x"), "[CHG]", 5), "skip_prompt");
    CHECK(is_address_name("AA:BB:CC:DD:EE:FF", "AA-BB-CC-DD-EE-FF") && !is_address_name("AA:BB:CC:DD:EE:FF", "Speaker"), "address names");
#undef CHECK
    if (!bad)
        printf("mellagui-bluetooth selftest ok\n");
    return bad;
}

int main(int argc, char **argv)
{
    wlwin_hidpi();                      /* draws through the wlwin_* calls only: sharp on a scaled screen */
    lm_theme_load_env();
    if (argc > 1 && !strcmp(argv[1], "--selftest"))
        return selftest();
    if (argc > 1) {
        fprintf(stderr, "usage: mellagui-bluetooth | --selftest\n");
        return 2;
    }
    const char *rt = getenv("XDG_RUNTIME_DIR");
    char lock[300];
    const char *disp = getenv("WAYLAND_DISPLAY");
    snprintf(lock, sizeof(lock), "%s/mellagui-bluetooth-%s.lock", rt && *rt ? rt : "/tmp", disp && *disp && !strchr(disp, '/') ? disp : "wayland-0");
    int lfd = open(lock, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lfd >= 0 && flock(lfd, LOCK_EX | LOCK_NB) != 0)
        return 0;                                               /* already open */
    static const struct wlwin_ops ops = { .paint = paint, .global = global_event, .pointer = pointer_event, .keyboard = keyboard_event };
    static struct app a;
    a.fd = -1;
    a.dump = getenv("MELLAGUI_BT_DUMP");
    load_state(&a);
    if (wlwin_connect(&a.win, "mellagui-bluetooth", &ops, &a) < 0)
        return 1;
    if (a.have && sess_start(&a)) {
        sess_send(&a, "scan on");
        if (bt_audio_plugin_missing())                          /* speakers and headphones would pair and then never connect: say so before it happens */
            set_msg(&a, true, "Speakers and headphones need: sudo apt install libspa-0.2-bluetooth (then restart PipeWire)");
        else
            set_msg(&a, false, "Looking for devices...");
    } else if (!a.have) {
        set_msg(&a, true, "No Bluetooth controller answers (is bluez installed and the adapter on?)");
    }
    if (wlwin_open(&a.win, "Bluetooth", "mellagui.bluetooth", 480, 520, 400, 320) < 0) {
        sess_stop(&a);
        wlwin_destroy(&a.win);
        return 1;
    }
    const char *rm = getenv("MELLAGUI_BT_REFRESH_MS");            /* (tests make it short) */
    uint64_t refresh_ms = rm && atoi(rm) > 0 ? (uint64_t)atoi(rm) : REFRESH_MS;
    uint64_t last_refresh;
    { struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0); last_refresh = (uint64_t)t0.tv_sec * 1000 + (uint64_t)t0.tv_nsec / 1000000; }
    while (a.win.running) {
        struct pollfd p = { .fd = a.fd, .events = POLLIN };
        if (a.fd >= 0 && poll(&p, 1, 0) > 0)
            sess_read(&a);
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t now_ms = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
        if (now_ms - last_refresh >= refresh_ms && a.modal == M_NONE) {
            last_refresh = now_ms;
            refresh_batteries(&a);
        }
        if (!wlwin_step(&a.win, 100))
            break;
        if (a.fd >= 0) {
            int st;
            if (waitpid(a.pid, &st, WNOHANG) == a.pid) {                 /* bluetoothctl ended by itself */
                close(a.fd);
                a.fd = -1;
                set_msg(&a, true, "bluetoothctl ended");
                a.win.redraw = true;
            }
        }
    }
    sess_stop(&a);
    wlwin_destroy(&a.win);
    return 0;
}
