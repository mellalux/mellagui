/*
 * mellagui-smart - the devices of a Smart Life (Tuya) account: plugs, blinds, a robot vacuum ... in a window.
 *
 *   mellagui-smart                     the window: log in once with a QR code, then the devices with their switches, buttons and sliders
 *   mellagui-smart --list              the devices and everything they report, as text (needs a login)
 *   mellagui-smart --set ID CODE VALUE tell a device something (VALUE: true / false, a number, or a word such as open)
 *   mellagui-smart --login CODE        log in from a terminal: the QR code is printed there
 *   mellagui-smart --logout            forget the login
 *   mellagui-smart --qr TEXT           print a QR code of TEXT (to try the encoder with a phone)
 *   mellagui-smart --selftest
 *
 * The login: in the Smart Life app, Me > Settings (the gear) > Account and Security > User Code. That code is typed here, the program asks the
 * cloud for a login code, shows it as a QR code, and the app's scanner confirms it. The password never comes near this program; what it keeps
 * are the tokens (smart.conf, mode 0600). The cloud calls are in tuya.c and run in a worker thread, so the window never waits for the network.
 *
 * A device shows what the cloud says it can be told: a truth value is a switch, a number with a range a slider (sent when it is let go), a choice
 * a button that opens a menu (the blind's open / stop / close are three buttons). At first only the main ones show; "More" shows the rest and
 * everything the device reports. The values are asked again every 10 s ($MELLAGUI_SMART_POLL_MS) and shortly after a command.
 *
 *   wheel, Up / Down, PgUp / PgDn   scroll        F5 or R   ask again        Esc   close (in the QR view: back)
 *
 * A second copy does not start (a lock file per Wayland display). MELLAGUI_SMART_DUMP=FILE writes the window's state there after every paint (tests).
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

#include "font.h"
#include "json.h"
#include "keymap.h"
#include "protocol.h"
#include "qr.h"
#include "theme.h"
#include "tuya.h"
#include "uikit.h"
#include "util.h"
#include "wlc.h"
#include "wlwin.h"

#define HEAD_H 58
#define FOOT_H 30
#define PAD 12
#define ROW 30
#define MAX_CTL 600
#define MAX_HOLD 16
#define MAX_JOBS 16
#define HOLD_MS 5000
#define QR_POLL_MS 2000
#define QR_LIFE_MS 150000

enum view { V_LOGIN, V_QR, V_DEVICES };
enum ctl_kind { C_TOGGLE, C_BUTTON, C_SLIDER, C_MENU, C_MORE };
static const char *const KIND_NAME[] = { "toggle", "button", "slider", "menu", "more" };

struct box { int x, y, w, h; };

struct ctl {
    enum ctl_kind kind;
    int dev, dp;                        /* indices into app.dev / its dp (dp -1 for C_MORE) */
    struct box b;
    char label[48];
    char value[24];                     /* C_BUTTON: what it sends */
};

/* A value the user just set: the cloud's answers are not believed about it for a few seconds (they lag behind). */
struct hold {
    char dev[40], code[40];
    bool b;
    double num;
    char str[64];
    uint64_t until;
};

enum job_type { J_QR_START, J_QR_POLL, J_LOAD, J_POLL, J_CMD, J_LOGOUT, J_QUIT };

struct job {
    enum job_type type;
    char a[64], b[64], c[200];
};

/* What the worker hands to the window (under app.lock). */
struct results {
    bool have_devs;
    struct tuya_dev *devs;
    int ndevs;
    bool qr_done, qrpoll_done, poll_done, login_ok, logged_out, relogin;
    char qr[200], username[80];
    bool have_msg, msg_err;
    char msg[200];
};

struct app {
    struct wlwin win;
    enum view view;
    double px, py;
    /* the login */
    char code[48];
    struct qr qr;
    char qr_token[200];
    bool have_qr, qr_busy, qrpoll_busy;
    uint64_t qr_at, qr_poll_at;
    /* the devices */
    struct tuya_dev *dev;
    int ndev;
    bool loaded;
    char expanded[TUYA_MAX_DEV][40];
    int nexpanded;
    int scroll, content_h;
    struct box card[TUYA_MAX_DEV];
    int extra_y[TUYA_MAX_DEV];          /* where the "everything it reports" lines of an expanded card start */
    struct ctl ctl[MAX_CTL];
    int nctl;
    bool dragging;
    char drag_dev[40], drag_code[40];
    struct hold hold[MAX_HOLD];
    char menu_dev[40], menu_code[40];
    uint64_t poll_at;
    bool poll_busy;
    int poll_ms;
    char msg[200];
    bool msg_err;
    char username[80];
    const char *dump;
    /* the worker */
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    struct job jobs[MAX_JOBS];
    int njobs;
    struct results res;
    struct tuya_session sess;           /* the worker's, once it runs */
    struct tuya_dev *master;            /* the worker's list: what it knows, copies go to the window */
    int nmaster;
};

static void copy(char *dst, size_t n, const char *src)
{
    snprintf(dst, n, "%.*s", (int)n - 1, src);
}

static uint64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static bool in(double px, double py, struct box b)
{
    return px >= b.x && px < b.x + b.w && py >= b.y && py < b.y + b.h;
}

static void set_msg(struct app *a, bool err, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(a->msg, sizeof(a->msg), fmt, ap);
    va_end(ap);
    a->msg_err = err;
    a->win.redraw = true;
}

/* ---- the worker ----------------------------------------------------------------------------------------------------------------------- */

static bool push_job(struct app *a, enum job_type type, const char *x, const char *y, const char *z)
{
    bool ok = false;
    pthread_mutex_lock(&a->lock);
    if (a->njobs < MAX_JOBS) {
        struct job *j = &a->jobs[a->njobs++];
        j->type = type;
        copy(j->a, sizeof(j->a), x ? x : "");
        copy(j->b, sizeof(j->b), y ? y : "");
        copy(j->c, sizeof(j->c), z ? z : "");
        ok = true;
        pthread_cond_signal(&a->cond);
    }
    pthread_mutex_unlock(&a->lock);
    return ok;
}

static void result_msg(struct app *a, bool err, const char *text)
{
    pthread_mutex_lock(&a->lock);
    a->res.have_msg = true;
    a->res.msg_err = err;
    copy(a->res.msg, sizeof(a->res.msg), text);
    pthread_mutex_unlock(&a->lock);
}

static void publish_devs(struct app *a)
{
    struct tuya_dev *c = malloc(sizeof(*c) * (size_t)LM_MAX(a->nmaster, 1));
    if (!c)
        return;
    memcpy(c, a->master, sizeof(*c) * (size_t)a->nmaster);
    pthread_mutex_lock(&a->lock);
    free(a->res.devs);
    a->res.devs = c;
    a->res.ndevs = a->nmaster;
    a->res.have_devs = true;
    pthread_mutex_unlock(&a->lock);
}

/* The devices and their values; a device seen for the first time is also asked what it can be told. */
static void load_all(struct app *a)
{
    char err[200] = "";
    bool ok = tuya_load_devices(&a->sess, a->master, &a->nmaster, err, sizeof(err));
    if (ok) {
        publish_devs(a);
        int asked = 0;
        for (int i = 0; i < a->nmaster; i++) {
            if (a->master[i].specs)
                continue;
            char e2[200];
            tuya_load_specs(&a->sess, &a->master[i], e2, sizeof(e2));
            if (++asked % 3 == 0)
                publish_devs(a);
        }
        if (asked)
            publish_devs(a);
        result_msg(a, false, "");
    } else {
        result_msg(a, true, err);
    }
    pthread_mutex_lock(&a->lock);
    a->res.poll_done = true;
    if (a->sess.relogin)
        a->res.relogin = true;
    pthread_mutex_unlock(&a->lock);
}

static void *worker(void *arg)
{
    struct app *a = arg;
    for (;;) {
        struct job j;
        pthread_mutex_lock(&a->lock);
        while (a->njobs == 0)
            pthread_cond_wait(&a->cond, &a->lock);
        j = a->jobs[0];
        memmove(&a->jobs[0], &a->jobs[1], sizeof(a->jobs[0]) * (size_t)--a->njobs);
        pthread_mutex_unlock(&a->lock);
        char err[200] = "";
        switch (j.type) {
        case J_QR_START: {
            char token[200] = "";
            bool ok = tuya_qr_start(j.a, token, sizeof(token), err, sizeof(err));
            if (ok) {
                copy(a->sess.user_code, sizeof(a->sess.user_code), j.a);
                if (!tuya_logged_in(&a->sess))
                    tuya_session_save(&a->sess);                /* (the code is remembered for the next time) */
            } else {
                result_msg(a, true, err);
            }
            pthread_mutex_lock(&a->lock);
            a->res.qr_done = true;
            copy(a->res.qr, sizeof(a->res.qr), ok ? token : "");
            pthread_mutex_unlock(&a->lock);
            break;
        }
        case J_QR_POLL: {
            int r = tuya_qr_poll(j.a, j.c, &a->sess, err, sizeof(err));
            if (r < 0 && err[0])
                result_msg(a, true, err);
            pthread_mutex_lock(&a->lock);
            a->res.qrpoll_done = true;
            if (r == 1) {
                a->res.login_ok = true;
                copy(a->res.username, sizeof(a->res.username), a->sess.username);
            }
            pthread_mutex_unlock(&a->lock);
            if (r == 1) {
                a->nmaster = 0;
                load_all(a);
            }
            break;
        }
        case J_LOAD:
        case J_POLL:
            load_all(a);
            break;
        case J_CMD:
            if (!tuya_command(&a->sess, j.a, j.b, j.c, err, sizeof(err)))
                result_msg(a, true, err);
            break;
        case J_LOGOUT:
            tuya_session_forget(&a->sess);
            a->nmaster = 0;
            pthread_mutex_lock(&a->lock);
            a->res.logged_out = true;
            pthread_mutex_unlock(&a->lock);
            break;
        case J_QUIT:
            return NULL;
        }
    }
}

/* ---- values --------------------------------------------------------------------------------------------------------------------------- */

static struct tuya_dev *find_dev(struct app *a, const char *id)
{
    for (int i = 0; i < a->ndev; i++)
        if (!strcmp(a->dev[i].id, id))
            return &a->dev[i];
    return NULL;
}

static void apply_holds(struct app *a)
{
    uint64_t now = mono_ms();
    for (int i = 0; i < MAX_HOLD; i++) {
        struct hold *h = &a->hold[i];
        if (!h->dev[0] || h->until <= now) {
            h->dev[0] = 0;
            continue;
        }
        struct tuya_dev *d = find_dev(a, h->dev);
        struct tuya_dp *dp = d ? tuya_find_dp(d, h->code) : NULL;
        if (dp) {
            dp->has = true;
            dp->b = h->b;
            dp->num = h->num;
            copy(dp->str, sizeof(dp->str), h->str);
        }
    }
}

/* Tells the device the data point's value as it is now in the window's list. */
static void send_value(struct app *a, struct tuya_dev *d, struct tuya_dp *dp)
{
    char json[100];
    if (dp->type == TUYA_BOOL)
        snprintf(json, sizeof(json), "%s", dp->b ? "true" : "false");
    else if (dp->type == TUYA_INT)
        snprintf(json, sizeof(json), "%lld", (long long)(dp->num < 0 ? dp->num - 0.5 : dp->num + 0.5));
    else
        json_quote(dp->str, json, sizeof(json));
    dp->has = true;
    if (!push_job(a, J_CMD, d->id, dp->code, json)) {
        set_msg(a, true, "Too many commands are waiting");
        return;
    }
    struct hold *h = &a->hold[0];
    for (int i = 0; i < MAX_HOLD; i++) {
        if (!strcmp(a->hold[i].dev, d->id) && !strcmp(a->hold[i].code, dp->code)) {
            h = &a->hold[i];
            break;
        }
        if (!a->hold[i].dev[0] || a->hold[i].until < h->until)
            h = &a->hold[i];
    }
    copy(h->dev, sizeof(h->dev), d->id);
    copy(h->code, sizeof(h->code), dp->code);
    h->b = dp->b;
    h->num = dp->num;
    copy(h->str, sizeof(h->str), dp->str);
    h->until = mono_ms() + HOLD_MS;
    a->poll_at = mono_ms() + 1500;
    a->win.redraw = true;
}

/* ---- what a device shows ------------------------------------------------------------------------------------------------------------------ */

/* "switch_1" -> "Switch 1"; a few codes have a better name. */
static void pretty(const char *code, char *out, size_t n)
{
    static const char *const names[][2] = {
        { "switch", "Power" }, { "power", "Power" }, { "power_go", "Clean" }, { "switch_go", "Clean" }, { "switch_charge", "Return to dock" },
        { "pause", "Pause" }, { "percent_control", "Position" }, { "percent_state", "Position" }, { "position", "Position" },
        { "electricity_left", "Battery" }, { "battery_percentage", "Battery" }, { "residual_electricity", "Battery" }, { "status", "State" },
        { "bright_value", "Brightness" }, { "temp_value", "Colour temperature" }, { "suction", "Suction" }, { "mode", "Mode" },
        { "temp_current", "Temperature" }, { "humidity_value", "Humidity" }, { "child_lock", "Child lock" }, { "seek", "Find the robot" },
        { "cur_power", "Power" }, { "cur_voltage", "Voltage" }, { "cur_current", "Current" }, { "add_ele", "Energy" },
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (!strcmp(code, names[i][0])) {
            copy(out, n, names[i][1]);
            return;
        }
    copy(out, n, code);
    for (char *p = out; *p; p++)
        if (*p == '_')
            *p = ' ';
    if (out[0] >= 'a' && out[0] <= 'z')
        out[0] = (char)(out[0] - 32);
}

/* Something the window has a control for. */
static bool controllable(const struct tuya_dp *dp)
{
    if (!dp->func)
        return false;
    return dp->type == TUYA_BOOL || (dp->type == TUYA_ENUM && dp->nrange > 0) || (dp->type == TUYA_INT && dp->max > dp->min && dp->max - dp->min <= 1000);
}

/* The controls a card shows before "More". */
static bool primary(const struct tuya_dev *d, const struct tuya_dp *dp)
{
    static const char *const codes[] = { "power", "power_go", "switch_go", "pause", "switch_charge", "control", "percent_control", "position",
                                         "bright_value", "bright_value_v2", "temp_set", NULL };
    const char *c = dp->code;
    if (!strncmp(c, "switch", 6) && strcmp(c, "switch_inching") && strcmp(c, "switch_backlight"))
        return true;
    for (int i = 0; codes[i]; i++)
        if (!strcmp(c, codes[i]))
            return true;
    return !strcmp(d->category, "sd") && (!strcmp(c, "mode") || !strcmp(c, "suction"));
}

static bool is_expanded(const struct app *a, const char *id)
{
    for (int i = 0; i < a->nexpanded; i++)
        if (!strcmp(a->expanded[i], id))
            return true;
    return false;
}

static void toggle_expanded(struct app *a, const char *id)
{
    for (int i = 0; i < a->nexpanded; i++)
        if (!strcmp(a->expanded[i], id)) {
            memmove(a->expanded[i], a->expanded[i + 1], sizeof(a->expanded[0]) * (size_t)(a->nexpanded - i - 1));
            a->nexpanded--;
            return;
        }
    if (a->nexpanded < TUYA_MAX_DEV)
        copy(a->expanded[a->nexpanded++], sizeof(a->expanded[0]), id);
}

/* The line under the name: what matters of what the device reports. */
static void info_text(struct tuya_dev *d, char *out, size_t n)
{
    static const char *const codes[] = { "status", "work_state", "electricity_left", "battery_percentage", "residual_electricity", "percent_state",
                                         "cur_power", "cur_voltage", "cur_current", "temp_current", "humidity_value", "clean_time", "clean_area", NULL };
    size_t o = 0;
    out[0] = 0;
    if (!d->online)
        o += (size_t)snprintf(out, n, "Offline");
    for (int i = 0; codes[i] && o + 40 < n; i++) {
        struct tuya_dp *dp = tuya_find_dp(d, codes[i]);
        char v[80], name[48];
        if (!dp || !dp->has)
            continue;
        tuya_dp_text(dp, v, sizeof(v));
        for (char *p = v; *p; p++)
            if (*p == '_')
                *p = ' ';
        if (!strncmp(codes[i], "cur_", 4))
            name[0] = 0;
        else
            pretty(codes[i], name, sizeof(name));
        o += (size_t)snprintf(out + o, n - o, "%s%s%s%s", o ? "   " : "", name, name[0] ? " " : "", v);
        if (o >= n)
            break;
    }
}

static struct ctl *add_ctl(struct app *a, enum ctl_kind kind, int dev, int dp, struct box b, const char *label, const char *value)
{
    if (a->nctl >= MAX_CTL)
        return NULL;
    struct ctl *c = &a->ctl[a->nctl++];
    c->kind = kind;
    c->dev = dev;
    c->dp = dp;
    c->b = b;
    copy(c->label, sizeof(c->label), label);
    copy(c->value, sizeof(c->value), value ? value : "");
    return c;
}

/* Places every card and its controls for the window's width and the scroll position (paint and the pointer both go by it). */
static void build_layout(struct app *a)
{
    int bw = a->win.bw, y = HEAD_H + PAD - a->scroll;
    a->nctl = 0;
    for (int i = 0; i < a->ndev; i++) {
        struct tuya_dev *d = &a->dev[i];
        bool exp = is_expanded(a, d->id), any_primary = false;
        int x0 = PAD + 12, x1 = bw - PAD - 12, cy = y + 54, cx = x0, hidden = 0, reported = 0;
        bool row_used = false;
        for (int k = 0; k < d->ndp; k++)
            if (controllable(&d->dp[k]) && primary(d, &d->dp[k]))
                any_primary = true;
        for (int k = 0; k < d->ndp; k++) {
            struct tuya_dp *dp = &d->dp[k];
            char label[48];
            if (!controllable(dp)) {
                reported++;
                continue;
            }
            if (!exp && any_primary && !primary(d, dp)) {
                hidden++;
                continue;
            }
            pretty(dp->code, label, sizeof(label));
            if (dp->type == TUYA_INT) {                         /* a slider has a row of its own */
                if (row_used)
                    cy += ROW + 6;
                add_ctl(a, C_SLIDER, i, k, (struct box){ x0, cy, x1 - x0, ROW }, label, NULL);
                cy += ROW + 6;
                cx = x0;
                row_used = false;
                continue;
            }
            if (dp->type == TUYA_ENUM && !strcmp(dp->code, "control") && dp->nrange <= 4) {     /* open / stop / close */
                for (int r = 0; r < dp->nrange; r++) {
                    pretty(dp->range[r], label, sizeof(label));
                    int w = lm_font_text_width(label) + 28;
                    if (cx + w > x1 && row_used) {
                        cy += ROW + 6;
                        cx = x0;
                    }
                    add_ctl(a, C_BUTTON, i, k, (struct box){ cx, cy, w, ROW }, label, dp->range[r]);
                    cx += w + 8;
                    row_used = true;
                }
                continue;
            }
            char text[120], v[64];
            enum ctl_kind kind = C_TOGGLE;
            int w = 44 + 8 + lm_font_text_width(label) + 14;
            if (dp->type == TUYA_ENUM) {
                kind = C_MENU;
                copy(v, sizeof(v), dp->has ? dp->str : "-");
                for (char *p = v; *p; p++)
                    if (*p == '_')
                        *p = ' ';
                snprintf(text, sizeof(text), "%.40s: %.30s", label, v);
                copy(label, sizeof(label), text);
                w = lm_font_text_width(label) + 28;
            }
            if (cx + w > x1 && row_used) {
                cy += ROW + 6;
                cx = x0;
            }
            add_ctl(a, kind, i, k, (struct box){ cx, cy, w, ROW }, label, NULL);
            cx += w + 10;
            row_used = true;
        }
        if (row_used)
            cy += ROW + 6;
        a->extra_y[i] = cy;
        if (exp) {
            int lines = 0;
            for (int k = 0; k < d->ndp; k++)
                if (!controllable(&d->dp[k]))
                    lines++;
            cy += lines * 18 + (lines ? 6 : 0);
        }
        if (hidden || exp || (reported && !any_primary) || reported)
            add_ctl(a, C_MORE, i, -1, (struct box){ bw - PAD - 12 - 56, y + 8, 56, 22 }, exp ? "Less" : "More", NULL);
        a->card[i] = (struct box){ PAD, y, bw - 2 * PAD, cy + 6 - y };
        y = cy + 6 + 10;
    }
    a->content_h = y + a->scroll - HEAD_H;
}

static void clamp_scroll(struct app *a)
{
    int room = a->win.bh - HEAD_H - FOOT_H;
    int max = a->content_h - room;
    a->scroll = LM_CLAMP(a->scroll, 0, LM_MAX(max, 0));
}

/* ---- painting ----------------------------------------------------------------------------------------------------------------------------- */

static struct box btn_refresh(const struct app *a) { return (struct box){ a->win.bw - 200, 15, 88, 28 }; }
static struct box btn_logout(const struct app *a) { return (struct box){ a->win.bw - 104, 15, 90, 28 }; }
static struct box login_field(const struct app *a) { return (struct box){ 24, 250, LM_MIN(a->win.bw - 48, 260), 30 }; }
static struct box login_go(const struct app *a) { struct box f = login_field(a); return (struct box){ f.x, f.y + 44, 150, 30 }; }
static struct box qr_back(const struct app *a) { return (struct box){ 24, a->win.bh - FOOT_H - 40, 90, 28 }; }

static void rbox(struct wlwin *w, struct box b, int r, uint32_t rgb)
{
    lm_round_fill(w->pixels, w->bw, w->bw, w->bh, b.x, b.y, b.w, b.h, r, rgb);
}

static void button(struct wlwin *w, struct box b, const char *label, bool hot, uint32_t bg, uint32_t fg)
{
    rbox(w, b, LM_MAX(lm_button_radius(b.h / 2), 3), hot ? lm_theme.button_hover : bg);
    wlwin_text(w, b.x + (b.w - lm_font_text_width(label)) / 2, b.y + (b.h - LM_FONT_H) / 2, label, 1, fg, 0);
}

/* Text broken at spaces to fit max_w; returns the y after the last line. */
static int wrap_text(struct wlwin *w, int x, int y, int max_w, const char *text, uint32_t rgb)
{
    int per = LM_MAX(max_w / LM_FONT_W, 8);
    while (*text) {
        int n = (int)strlen(text);
        if (n > per) {
            n = per;
            while (n > 0 && text[n] != ' ')
                n--;
            if (n == 0)
                n = per;
        }
        char line[200];
        snprintf(line, sizeof(line), "%.*s", LM_MIN(n, (int)sizeof(line) - 1), text);
        wlwin_text(w, x, y, line, 1, rgb, 0);
        y += LM_FONT_H + 2;
        text += n;
        while (*text == ' ')
            text++;
    }
    return y;
}

static void slider_track(struct box b, int *x0, int *x1)
{
    *x0 = b.x + 150;
    *x1 = b.x + b.w - 80;
    if (*x1 < *x0 + 40)
        *x1 = *x0 + 40;
}

static void dump_state(struct app *a)
{
    if (!a->dump)
        return;
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s.tmp", a->dump);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return;
    fprintf(f, "view=%s ndev=%d loaded=%d err=%d\nmsg %s\nqr %s\n", a->view == V_LOGIN ? "login" : a->view == V_QR ? "qr" : "devices", a->ndev,
            a->loaded, a->msg_err, a->msg, a->view == V_QR && a->have_qr ? a->qr_token : "");
    if (a->view == V_DEVICES) {
        for (int i = 0; i < a->ndev; i++)
            fprintf(f, "dev %s online=%d name=%s\n", a->dev[i].id, a->dev[i].online, a->dev[i].name);
        for (int i = 0; i < a->nctl; i++) {
            const struct ctl *c = &a->ctl[i];
            struct tuya_dev *d = &a->dev[c->dev];
            char v[80] = "-";
            if (c->dp >= 0 && c->kind == C_BUTTON)
                copy(v, sizeof(v), c->value);
            else if (c->dp >= 0 && d->dp[c->dp].type == TUYA_INT)
                snprintf(v, sizeof(v), "%g", d->dp[c->dp].num);
            else if (c->dp >= 0 && d->dp[c->dp].type == TUYA_BOOL)
                copy(v, sizeof(v), d->dp[c->dp].b ? "on" : "off");
            else if (c->dp >= 0)
                copy(v, sizeof(v), d->dp[c->dp].str);
            fprintf(f, "ctl %s %s %s %d %d %d %d %s\n", d->id, c->dp >= 0 ? d->dp[c->dp].code : "-", KIND_NAME[c->kind], c->b.x, c->b.y, c->b.w, c->b.h, v);
        }
    }
    if (fclose(f) != 0 || rename(tmp, a->dump) < 0)
        unlink(tmp);
}

static void paint_login(struct app *a)
{
    struct wlwin *w = &a->win;
    int tw = w->bw - 48;
    if (a->view == V_LOGIN) {
        int y = wrap_text(w, 24, HEAD_H + 10, tw, "Log in once with the Smart Life app on your phone. Your password is not asked for here.", lm_theme.text);
        y = wrap_text(w, 24, y + 8, tw, "1. In the app: Me > Settings (the gear) > Account and Security > User Code.", lm_theme.text_dim);
        y = wrap_text(w, 24, y + 2, tw, "2. Type that code here and press Enter.", lm_theme.text_dim);
        wrap_text(w, 24, y + 2, tw, "3. Scan the QR code that appears with the app.", lm_theme.text_dim);
        struct box f = login_field(a);
        wlwin_text(w, f.x, f.y - 20, "User code", 1, lm_theme.text, 0);
        wlwin_fill(w, f.x - 1, f.y - 1, f.w + 2, f.h + 2, lm_theme.accent);
        wlwin_fill(w, f.x, f.y, f.w, f.h, lm_theme.field);
        wlwin_text(w, f.x + 8, f.y + 7, a->code, 1, lm_theme.text, f.w - 20);
        wlwin_fill(w, f.x + 8 + lm_font_text_width(a->code), f.y + 7, 2, LM_FONT_H, lm_theme.accent);
        button(w, login_go(a), a->qr_busy ? "Asking..." : "Show QR code", in(a->px, a->py, login_go(a)), lm_theme.button, lm_theme.text);
        return;
    }
    int top = HEAD_H + 8, room_h = w->bh - FOOT_H - 48 - top - 44, room_w = w->bw - 48;
    int cells = a->qr.size + 8;                                 /* a quiet zone of four modules on every side */
    int scale = LM_MAX(LM_MIN(room_w, room_h) / cells, 2);
    int side = cells * scale, qx = (w->bw - side) / 2, qy = top;
    wlwin_fill(w, qx, qy, side, side, 0xffffff);                /* (a QR code is black on white whatever the theme) */
    for (int y = 0; y < a->qr.size; y++)
        for (int x = 0; x < a->qr.size; x++)
            if (qr_dark(&a->qr, x, y))
                wlwin_fill(w, qx + (x + 4) * scale, qy + (y + 4) * scale, scale, scale, 0x000000);
    int y = wrap_text(w, 24, qy + side + 10, tw, "Scan this with the Smart Life app (the scan button at the top right of its home screen) and confirm there.", lm_theme.text);
    wlwin_text(w, 24, y + 2, "Waiting for the scan...", 1, lm_theme.text_dim, tw);
    button(w, qr_back(a), "Back", in(a->px, a->py, qr_back(a)), lm_theme.button, lm_theme.text);
}

static void paint_devices(struct app *a)
{
    struct wlwin *w = &a->win;
    build_layout(a);
    clamp_scroll(a);
    build_layout(a);
    bool list_hot = a->py >= HEAD_H && a->py < w->bh - FOOT_H;
    if (!a->ndev)
        wlwin_text(w, 24, HEAD_H + 16, a->loaded ? "This account has no devices." : "Asking for the devices...", 1, lm_theme.text_dim, w->bw - 48);
    for (int i = 0; i < a->ndev; i++) {
        struct tuya_dev *d = &a->dev[i];
        struct box c = a->card[i];
        if (c.y + c.h < HEAD_H || c.y > w->bh - FOOT_H)
            continue;
        rbox(w, (struct box){ c.x - 1, c.y - 1, c.w + 2, c.h + 2 }, 9, lm_theme.border);
        rbox(w, c, 8, lm_theme.card);
        wlwin_text(w, c.x + 12, c.y + 10, d->name, 1, d->online ? lm_theme.text : lm_theme.text_dim, c.w - 110);
        char info[300];
        info_text(d, info, sizeof(info));
        wlwin_text(w, c.x + 12, c.y + 30, info[0] ? info : d->product, 1, d->online ? lm_theme.text_dim : lm_theme.alert, c.w - 24);
        if (is_expanded(a, d->id)) {
            int ly = a->extra_y[i];
            for (int k = 0; k < d->ndp; k++) {
                char v[80], line[160];
                if (controllable(&d->dp[k]))
                    continue;
                tuya_dp_text(&d->dp[k], v, sizeof(v));
                snprintf(line, sizeof(line), "%s: %s", d->dp[k].code, v);
                wlwin_text(w, c.x + 12, ly, line, 1, lm_theme.text_dim, c.w - 24);
                ly += 18;
            }
        }
    }
    for (int i = 0; i < a->nctl; i++) {
        const struct ctl *c = &a->ctl[i];
        struct tuya_dev *d = &a->dev[c->dev];
        struct tuya_dp *dp = c->dp >= 0 ? &d->dp[c->dp] : NULL;
        struct box b = c->b;
        bool hot = list_hot && in(a->px, a->py, b) && (d->online || c->kind == C_MORE);
        uint32_t fg = d->online ? lm_theme.text : lm_theme.text_dim;
        if (b.y + b.h < HEAD_H || b.y > w->bh - FOOT_H)
            continue;
        switch (c->kind) {
        case C_TOGGLE: {
            bool on = dp->has && dp->b;
            struct box pill = { b.x, b.y + 4, 44, 22 };
            rbox(w, pill, 11, !d->online ? lm_theme.border : on ? lm_theme.ok : hot ? lm_theme.button_hover : lm_theme.button);
            rbox(w, (struct box){ on ? pill.x + 24 : pill.x + 2, pill.y + 2, 18, 18 }, 9, d->online ? lm_theme.on_colour : lm_theme.text_dim);
            wlwin_text(w, b.x + 52, b.y + 7, c->label, 1, fg, 0);
            break;
        }
        case C_BUTTON: {
            bool cur = dp->has && !strcmp(dp->str, c->value);
            button(w, b, c->label, hot, cur ? lm_theme.button_down : lm_theme.button, fg);
            break;
        }
        case C_MENU:
            button(w, b, c->label, hot, lm_theme.button_alt, fg);
            break;
        case C_MORE:
            button(w, b, c->label, hot, lm_theme.background, lm_theme.text_dim);
            break;
        case C_SLIDER: {
            int x0, x1;
            char v[48];
            slider_track(b, &x0, &x1);
            double f = dp->has && dp->max > dp->min ? LM_CLAMP((dp->num - dp->min) / (dp->max - dp->min), 0.0, 1.0) : 0.0;
            int kx = x0 + (int)(f * (x1 - x0) + 0.5);
            wlwin_text(w, b.x, b.y + 7, c->label, 1, fg, 142);
            rbox(w, (struct box){ x0, b.y + 12, x1 - x0, 6 }, 3, lm_theme.button);
            rbox(w, (struct box){ x0, b.y + 12, kx - x0, 6 }, 3, d->online ? lm_theme.accent : lm_theme.border);
            rbox(w, (struct box){ kx - 8, b.y + 7, 16, 16 }, 8, d->online ? lm_theme.on_colour : lm_theme.text_dim);
            tuya_dp_text(dp, v, sizeof(v));
            wlwin_text(w, x1 + 14, b.y + 7, v, 1, fg, 66);
            break;
        }
        }
    }
}

static void paint(struct wlwin *w)
{
    struct app *a = w->data;
    wlwin_fill(w, 0, 0, w->bw, w->bh, lm_theme.background);
    if (a->view == V_DEVICES)
        paint_devices(a);
    else
        paint_login(a);
    /* the head and the foot lie over the list */
    wlwin_fill(w, 0, 0, w->bw, HEAD_H - 8, lm_theme.background);
    wlwin_fill(w, 0, w->bh - FOOT_H, w->bw, FOOT_H, lm_theme.background);
    wlwin_text(w, 14, 12, "Smart home", 1, lm_theme.accent, 0);
    char sub[160];
    if (a->view == V_DEVICES)
        snprintf(sub, sizeof(sub), "%s%s%d device%s", a->username, a->username[0] ? "   " : "", a->ndev, a->ndev == 1 ? "" : "s");
    else
        snprintf(sub, sizeof(sub), "Smart Life account: not logged in");
    wlwin_text(w, 14, 32, sub, 1, lm_theme.text_dim, w->bw - 230);
    if (a->view == V_DEVICES) {
        button(w, btn_refresh(a), a->poll_busy ? "..." : "Refresh", in(a->px, a->py, btn_refresh(a)), lm_theme.button, lm_theme.text);
        button(w, btn_logout(a), "Log out", in(a->px, a->py, btn_logout(a)), lm_theme.button, lm_theme.text);
    }
    wlwin_fill(w, 10, HEAD_H - 8, w->bw - 20, 1, lm_theme.border);
    wlwin_fill(w, 10, w->bh - FOOT_H, w->bw - 20, 1, lm_theme.border);
    if (a->msg[0])
        wlwin_text(w, 14, w->bh - FOOT_H + 8, a->msg, 1, a->msg_err ? lm_theme.error : lm_theme.text, w->bw - 28);
    else
        wlwin_text(w, 14, w->bh - FOOT_H + 8, a->view == V_DEVICES ? "F5 asks again   Esc closes" : "Esc closes", 1, lm_theme.text_dim, w->bw - 28);
    dump_state(a);
}

/* ---- input ------------------------------------------------------------------------------------------------------------------------------ */

static void start_qr(struct app *a)
{
    if (!a->code[0]) {
        set_msg(a, true, "Type the user code first");
        return;
    }
    if (a->qr_busy)
        return;
    if (push_job(a, J_QR_START, a->code, NULL, NULL)) {
        a->qr_busy = true;
        set_msg(a, false, "Asking for a login code...");
    }
}

static void refresh(struct app *a)
{
    if (!a->poll_busy && push_job(a, J_POLL, NULL, NULL, NULL)) {
        a->poll_busy = true;
        a->poll_at = mono_ms() + (uint64_t)a->poll_ms;
    }
    a->win.redraw = true;
}

static void slider_set(struct app *a, const struct ctl *c)
{
    struct tuya_dp *dp = &a->dev[c->dev].dp[c->dp];
    int x0, x1;
    slider_track(c->b, &x0, &x1);
    double f = LM_CLAMP((a->px - x0) / (double)(x1 - x0), 0.0, 1.0), step = dp->step > 0 ? dp->step : 1;
    double v = dp->min + f * (dp->max - dp->min);
    v = dp->min + (double)(long long)((v - dp->min) / step + 0.5) * step;
    dp->num = LM_CLAMP(v, dp->min, dp->max);
    dp->has = true;
}

static const struct ctl *drag_ctl(struct app *a)
{
    for (int i = 0; i < a->nctl; i++) {
        const struct ctl *c = &a->ctl[i];
        if (c->kind == C_SLIDER && !strcmp(a->dev[c->dev].id, a->drag_dev) && !strcmp(a->dev[c->dev].dp[c->dp].code, a->drag_code))
            return c;
    }
    return NULL;
}

static void press_devices(struct app *a)
{
    struct wlwin *w = &a->win;
    if (in(a->px, a->py, btn_refresh(a))) {
        refresh(a);
        return;
    }
    if (in(a->px, a->py, btn_logout(a))) {
        push_job(a, J_LOGOUT, NULL, NULL, NULL);
        return;
    }
    if (a->py < HEAD_H - 8 || a->py >= w->bh - FOOT_H)
        return;
    build_layout(a);
    for (int i = 0; i < a->nctl; i++) {
        const struct ctl *c = &a->ctl[i];
        struct tuya_dev *d = &a->dev[c->dev];
        if (!in(a->px, a->py, c->b))
            continue;
        if (c->kind == C_MORE) {
            toggle_expanded(a, d->id);
            return;
        }
        struct tuya_dp *dp = &d->dp[c->dp];
        if (!d->online) {
            set_msg(a, true, "%s is offline", d->name);
            return;
        }
        switch (c->kind) {
        case C_TOGGLE:
            dp->b = !(dp->has && dp->b);
            send_value(a, d, dp);
            break;
        case C_BUTTON:
            copy(dp->str, sizeof(dp->str), c->value);
            send_value(a, d, dp);
            break;
        case C_SLIDER:
            a->dragging = true;
            copy(a->drag_dev, sizeof(a->drag_dev), d->id);
            copy(a->drag_code, sizeof(a->drag_code), dp->code);
            slider_set(a, c);
            break;
        case C_MENU: {
            char entries[TUYA_MAX_RANGE * 26] = "";
            size_t o = 0;
            for (int r = 0; r < dp->nrange; r++)
                o += (size_t)snprintf(entries + o, sizeof(entries) - o, "%s%s", r ? "\n" : "", dp->range[r]);
            copy(a->menu_dev, sizeof(a->menu_dev), d->id);
            copy(a->menu_code, sizeof(a->menu_code), dp->code);
            if (!wlwin_menu(w, entries)) {                      /* no menus here: the next value in turn */
                int at = 0;
                for (int r = 0; r < dp->nrange; r++)
                    if (!strcmp(dp->range[r], dp->str))
                        at = r + 1;
                char next[sizeof(dp->range[0])];
                copy(next, sizeof(next), dp->range[at % dp->nrange]);
                copy(dp->str, sizeof(dp->str), next);
                send_value(a, d, dp);
            }
            break;
        }
        default:
            break;
        }
        return;
    }
}

static void menu_event(struct wlwin *w, int index)
{
    struct app *a = w->data;
    struct tuya_dev *d = find_dev(a, a->menu_dev);
    struct tuya_dp *dp = d ? tuya_find_dp(d, a->menu_code) : NULL;
    if (!dp || index < 0 || index >= dp->nrange || !d->online)
        return;
    copy(dp->str, sizeof(dp->str), dp->range[index]);
    send_value(a, d, dp);
}

static void pointer_event(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
    struct app *a = w->data;
    if (op == LM_WL_POINTER_EVT_ENTER || op == LM_WL_POINTER_EVT_MOTION) {
        int base = op == LM_WL_POINTER_EVT_ENTER ? 2 : 1;
        a->px = lm_fixed_to_double(args[base].f);
        a->py = lm_fixed_to_double(args[base + 1].f);
        if (a->dragging) {
            const struct ctl *c = drag_ctl(a);
            if (c)
                slider_set(a, c);
        }
        w->redraw = true;
        return;
    }
    if (op == LM_WL_POINTER_EVT_AXIS && args[1].u == 0) {
        double v = lm_fixed_to_double(args[2].f);
        a->scroll += v > 0 ? 48 : v < 0 ? -48 : 0;
        w->redraw = true;
        return;
    }
    if (op != LM_WL_POINTER_EVT_BUTTON || args[2].u != 272)
        return;
    w->redraw = true;
    if (args[3].u == 0) {                                       /* let go: a dragged slider is sent now */
        if (a->dragging) {
            struct tuya_dev *d = find_dev(a, a->drag_dev);
            struct tuya_dp *dp = d ? tuya_find_dp(d, a->drag_code) : NULL;
            a->dragging = false;
            if (dp)
                send_value(a, d, dp);
        }
        return;
    }
    if (a->view == V_DEVICES)
        press_devices(a);
    else if (a->view == V_LOGIN && in(a->px, a->py, login_go(a)))
        start_qr(a);
    else if (a->view == V_QR && in(a->px, a->py, qr_back(a))) {
        a->view = V_LOGIN;
        a->have_qr = false;
        set_msg(a, false, "");
    }
}

static void keyboard_event(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
    struct app *a = w->data;
    if (op != LM_WL_KEYBOARD_EVT_KEY || args[3].u == 0)
        return;
    uint32_t ks = lm_keymap_keysym(args[2].u, w->mods);
    int page = LM_MAX(w->bh - HEAD_H - FOOT_H - 40, 40);
    w->redraw = true;
    if (a->view == V_LOGIN) {
        size_t n = strlen(a->code);
        if (ks == LM_KS_Escape)
            w->running = false;
        else if (ks == LM_KS_Return || ks == LM_KS_KP_Enter)
            start_qr(a);
        else if (ks == LM_KS_BackSpace && n > 0)
            a->code[n - 1] = 0;
        else if (ks > 0x20 && ks < 0x7f && n + 1 < sizeof(a->code) && n < 40) {
            a->code[n] = (char)ks;
            a->code[n + 1] = 0;
        }
        return;
    }
    if (a->view == V_QR) {
        if (ks == LM_KS_Escape) {
            a->view = V_LOGIN;
            a->have_qr = false;
            set_msg(a, false, "");
        }
        return;
    }
    if (ks == LM_KS_Escape)
        w->running = false;
    else if (ks == LM_KS_F1 + 4 || ks == 'r' || ks == 'R')
        refresh(a);
    else if (ks == LM_KS_Down)
        a->scroll += 48;
    else if (ks == LM_KS_Up)
        a->scroll -= 48;
    else if (ks == LM_KS_Page_Down || ks == ' ')
        a->scroll += page;
    else if (ks == LM_KS_Page_Up)
        a->scroll -= page;
    else if (ks == LM_KS_Home)
        a->scroll = 0;
    else if (ks == LM_KS_End)
        a->scroll = a->content_h;
}

/* ---- the worker's answers, the clocks ------------------------------------------------------------------------------------------------------ */

static void apply_results(struct app *a)
{
    struct results r;
    pthread_mutex_lock(&a->lock);
    r = a->res;
    memset(&a->res, 0, sizeof(a->res));
    pthread_mutex_unlock(&a->lock);
    uint64_t now = mono_ms();
    if (r.have_msg && (r.msg[0] || !a->msg_err || r.have_devs))
        set_msg(a, r.msg_err, "%s", r.msg);
    if (r.qr_done) {
        a->qr_busy = false;
        if (r.qr[0] && a->view != V_DEVICES) {
            char text[260];
            snprintf(text, sizeof(text), "%s%s", TUYA_QR_PREFIX, r.qr);
            if (qr_encode(text, &a->qr, -1)) {
                copy(a->qr_token, sizeof(a->qr_token), r.qr);
                a->have_qr = true;
                a->qr_at = a->qr_poll_at = now;
                a->view = V_QR;
                set_msg(a, false, "");
            } else {
                set_msg(a, true, "The login code is too long for a QR code");
            }
        }
        a->win.redraw = true;
    }
    if (r.qrpoll_done)
        a->qrpoll_busy = false;
    if (r.login_ok) {
        a->view = V_DEVICES;
        a->have_qr = false;
        a->loaded = false;
        a->ndev = 0;
        a->poll_busy = true;                                    /* (the worker goes on to ask for the devices) */
        a->poll_at = now + (uint64_t)a->poll_ms;
        copy(a->username, sizeof(a->username), r.username);
        set_msg(a, false, "Logged in");
    }
    if (r.have_devs) {
        if (!a->dragging && a->view == V_DEVICES) {
            memcpy(a->dev, r.devs, sizeof(a->dev[0]) * (size_t)r.ndevs);
            a->ndev = r.ndevs;
            a->loaded = true;
            apply_holds(a);
            a->win.redraw = true;
        }
        free(r.devs);
    }
    if (r.poll_done) {
        a->poll_busy = false;
        a->win.redraw = true;
    }
    if (r.relogin && a->view == V_DEVICES) {
        set_msg(a, true, "The login is not accepted any more: log out and log in again");
        a->poll_at = now + 60000;
    }
    if (r.logged_out) {
        a->view = V_LOGIN;
        a->ndev = 0;
        a->loaded = false;
        a->username[0] = 0;
        a->scroll = 0;
        set_msg(a, false, "Logged out");
    }
}

static void tick(struct app *a)
{
    uint64_t now = mono_ms();
    apply_results(a);
    if (a->view == V_QR && a->have_qr) {
        if (!a->qrpoll_busy && now - a->qr_poll_at >= QR_POLL_MS && push_job(a, J_QR_POLL, a->code, NULL, a->qr_token)) {
            a->qrpoll_busy = true;
            a->qr_poll_at = now;
        }
        if (!a->qr_busy && now - a->qr_at >= QR_LIFE_MS && push_job(a, J_QR_START, a->code, NULL, NULL)) {   /* the code lasts a few minutes */
            a->qr_busy = true;
            a->qr_at = now;
        }
    }
    if (a->view == V_DEVICES && !a->poll_busy && !a->dragging && now >= a->poll_at)
        refresh(a);
}

/* ---- without a window ----------------------------------------------------------------------------------------------------------------------- */

static void print_qr(const struct qr *q)
{
    for (int y = -4; y < q->size + 4; y += 2) {                 /* two rows of modules per line; light = a lit block */
        for (int x = -4; x < q->size + 4; x++) {
            bool top = !qr_dark(q, x, y), bottom = !qr_dark(q, x, y + 1);
            fputs(top && bottom ? "\xe2\x96\x88" : top ? "\xe2\x96\x80" : bottom ? "\xe2\x96\x84" : " ", stdout);
        }
        putchar('\n');
    }
}

static bool need_login(struct tuya_session *s)
{
    if (tuya_session_load(s))
        return false;
    fprintf(stderr, "mellagui-smart: not logged in (open the window, or: mellagui-smart --login USERCODE)\n");
    return true;
}

static int cmd_list(void)
{
    static struct tuya_session s;
    static struct tuya_dev dev[TUYA_MAX_DEV];
    static const char *const types[] = { "truth value", "number", "choice", "text", "other" };
    int n = 0;
    char err[200] = "";
    if (need_login(&s))
        return 1;
    if (!tuya_load_devices(&s, dev, &n, err, sizeof(err))) {
        fprintf(stderr, "mellagui-smart: %s\n", err);
        return 1;
    }
    for (int i = 0; i < n; i++) {
        struct tuya_dev *d = &dev[i];
        if (!tuya_load_specs(&s, d, err, sizeof(err)))
            fprintf(stderr, "mellagui-smart: %s: %s\n", d->name, err);
        printf("%s  %s  [%s]  %s  %s%s%s\n", d->id, d->name, d->category, d->online ? "online" : "offline", d->product, d->ip[0] ? "  " : "", d->ip);
        for (int k = 0; k < d->ndp; k++) {
            struct tuya_dp *dp = &d->dp[k];
            char v[80];
            tuya_dp_text(dp, v, sizeof(v));
            printf("    %-24s = %-16s  %s%s", dp->code, v, types[dp->type], dp->func ? ", can be set" : "");
            if (dp->type == TUYA_INT && dp->max > dp->min)
                printf(" (%g..%g)", dp->min, dp->max);
            for (int r = 0; r < dp->nrange; r++)
                printf("%s%s", r ? " | " : " (", dp->range[r]);
            printf("%s\n", dp->nrange ? ")" : "");
        }
    }
    printf("%d device%s\n", n, n == 1 ? "" : "s");
    return 0;
}

static int cmd_set(const char *id, const char *code, const char *value)
{
    static struct tuya_session s;
    char err[200] = "", json[120], *end;
    if (need_login(&s))
        return 1;
    strtod(value, &end);
    if (!strcmp(value, "true") || !strcmp(value, "false") || (*value && !*end))
        copy(json, sizeof(json), value);
    else
        json_quote(value, json, sizeof(json));
    if (!tuya_command(&s, id, code, json, err, sizeof(err))) {
        fprintf(stderr, "mellagui-smart: %s\n", err);
        return 1;
    }
    printf("sent %s = %s\n", code, json);
    return 0;
}

static int cmd_login(const char *code)
{
    static struct tuya_session s;
    char err[200] = "", token[200], text[260];
    struct qr q;
    if (!tuya_qr_start(code, token, sizeof(token), err, sizeof(err))) {
        fprintf(stderr, "mellagui-smart: %s\n", err);
        return 1;
    }
    snprintf(text, sizeof(text), "%s%s", TUYA_QR_PREFIX, token);
    if (!qr_encode(text, &q, -1)) {
        fprintf(stderr, "mellagui-smart: the login code is too long for a QR code\n");
        return 1;
    }
    print_qr(&q);
    printf("Scan this with the Smart Life app and confirm there (waiting two minutes)...\n");
    fflush(stdout);
    for (int i = 0; i < 60; i++) {
        struct timespec ts = { .tv_sec = 2 };
        nanosleep(&ts, NULL);
        int r = tuya_qr_poll(code, token, &s, err, sizeof(err));
        if (r == 1) {
            printf("Logged in%s%s\n", s.username[0] ? " as " : "", s.username);
            return 0;
        }
        if (r < 0)
            fprintf(stderr, "mellagui-smart: %s\n", err);
    }
    fprintf(stderr, "mellagui-smart: nobody scanned the code\n");
    return 1;
}

static int selftest(void)
{
    int bad = tuya_selftest() + qr_selftest();
#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "mellagui-smart selftest: " __VA_ARGS__); fputc('\n', stderr); bad++; } } while (0)
    char s[64];
    pretty("switch_1", s, sizeof(s));
    CHECK(!strcmp(s, "Switch 1"), "pretty: %s", s);
    pretty("switch_charge", s, sizeof(s));
    CHECK(!strcmp(s, "Return to dock"), "pretty: %s", s);
    /* a blind: three buttons and a slider; a plug: its switch shows, the countdown hides behind More */
    static struct app a;
    static struct tuya_dev devs[2];
    a.dev = devs;
    a.ndev = 2;
    a.win.bw = 520;
    a.win.bh = 600;
    copy(devs[0].id, sizeof(devs[0].id), "blind");
    devs[0].online = true;
    devs[0].ndp = 2;
    devs[0].dp[0] = (struct tuya_dp){ .code = "control", .type = TUYA_ENUM, .func = true, .has = true, .str = "stop", .range = { "open", "stop", "close" }, .nrange = 3 };
    devs[0].dp[1] = (struct tuya_dp){ .code = "percent_control", .type = TUYA_INT, .func = true, .has = true, .num = 40, .min = 0, .max = 100, .step = 1 };
    copy(devs[1].id, sizeof(devs[1].id), "plug");
    devs[1].online = true;
    devs[1].ndp = 4;
    devs[1].dp[0] = (struct tuya_dp){ .code = "switch_1", .type = TUYA_BOOL, .func = true, .has = true, .b = true };
    devs[1].dp[1] = (struct tuya_dp){ .code = "relay_status", .type = TUYA_ENUM, .func = true, .has = true, .str = "last", .range = { "off", "on", "last" }, .nrange = 3 };
    devs[1].dp[2] = (struct tuya_dp){ .code = "countdown_1", .type = TUYA_INT, .func = true, .has = true, .min = 0, .max = 86400, .step = 1 };
    devs[1].dp[3] = (struct tuya_dp){ .code = "cur_power", .type = TUYA_INT, .has = true, .num = 123, .scale = 1, .unit = "W" };
    build_layout(&a);
    int kinds[5] = { 0 };
    for (int i = 0; i < a.nctl; i++)
        kinds[a.ctl[i].kind]++;
    CHECK(kinds[C_BUTTON] == 3 && kinds[C_SLIDER] == 1 && kinds[C_TOGGLE] == 1 && kinds[C_MENU] == 0 && kinds[C_MORE] == 1, "layout: %d buttons %d sliders %d toggles %d menus %d more",
          kinds[C_BUTTON], kinds[C_SLIDER], kinds[C_TOGGLE], kinds[C_MENU], kinds[C_MORE]);
    toggle_expanded(&a, "plug");
    build_layout(&a);
    memset(kinds, 0, sizeof(kinds));
    for (int i = 0; i < a.nctl; i++)
        kinds[a.ctl[i].kind]++;
    CHECK(kinds[C_MENU] == 1 && kinds[C_SLIDER] == 1, "expanded: %d menus %d sliders (a range of a day is no slider)", kinds[C_MENU], kinds[C_SLIDER]);
    for (int i = 0; i < a.nctl; i++)
        CHECK(a.ctl[i].b.x >= PAD && a.ctl[i].b.x + a.ctl[i].b.w <= a.win.bw - PAD, "control %d leaves its card", i);
    info_text(&devs[1], s, sizeof(s));
    CHECK(!strcmp(s, "12.3 W"), "info: %s", s);
    devs[1].online = false;
    info_text(&devs[1], s, sizeof(s));
    CHECK(!strncmp(s, "Offline", 7), "offline info: %s", s);
#undef CHECK
    if (!bad)
        printf("mellagui-smart selftest ok\n");
    return bad ? 1 : 0;
}

/* ---- start ------------------------------------------------------------------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    lm_theme_load_env();
    signal(SIGPIPE, SIG_IGN);
    if (argc == 2 && !strcmp(argv[1], "--selftest"))
        return selftest();
    if (argc == 2 && !strcmp(argv[1], "--list"))
        return cmd_list();
    if (argc == 5 && !strcmp(argv[1], "--set"))
        return cmd_set(argv[2], argv[3], argv[4]);
    if (argc == 3 && !strcmp(argv[1], "--login"))
        return cmd_login(argv[2]);
    if (argc == 2 && !strcmp(argv[1], "--logout")) {
        static struct tuya_session s;
        tuya_session_load(&s);
        tuya_session_forget(&s);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--qr")) {
        struct qr q;
        if (!qr_encode(argv[2], &q, -1)) {
            fprintf(stderr, "mellagui-smart: the text is too long (213 bytes at most)\n");
            return 1;
        }
        print_qr(&q);
        return 0;
    }
    if (argc > 1) {
        fprintf(stderr, "usage: mellagui-smart | --list | --set ID CODE VALUE | --login USERCODE | --logout | --qr TEXT | --selftest\n");
        return 2;
    }
    const char *rt = getenv("XDG_RUNTIME_DIR"), *disp = getenv("WAYLAND_DISPLAY");
    char lock[300];
    snprintf(lock, sizeof(lock), "%s/mellagui-smart-%s.lock", rt && *rt ? rt : "/tmp", disp && *disp && !strchr(disp, '/') ? disp : "wayland-0");
    int lfd = open(lock, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lfd >= 0 && flock(lfd, LOCK_EX | LOCK_NB) != 0)
        return 0;                                               /* already open */
    static const struct wlwin_ops ops = { .paint = paint, .pointer = pointer_event, .keyboard = keyboard_event, .menu = menu_event };
    static struct app a;
    a.dump = getenv("MELLAGUI_SMART_DUMP");
    const char *pm = getenv("MELLAGUI_SMART_POLL_MS");
    a.poll_ms = pm && atoi(pm) >= 100 ? atoi(pm) : 10000;
    a.dev = calloc(TUYA_MAX_DEV, sizeof(*a.dev));
    a.master = calloc(TUYA_MAX_DEV, sizeof(*a.master));
    if (!a.dev || !a.master)
        return 1;
    pthread_mutex_init(&a.lock, NULL);
    pthread_cond_init(&a.cond, NULL);
    bool logged_in = tuya_session_load(&a.sess);
    copy(a.code, sizeof(a.code), a.sess.user_code);
    copy(a.username, sizeof(a.username), a.sess.username);
    if (wlwin_connect(&a.win, "mellagui-smart", &ops, &a) < 0)
        return 1;
    if (wlwin_open(&a.win, "Smart home", "mellagui.smart", 520, 620, 420, 420) < 0) {
        wlwin_destroy(&a.win);
        return 1;
    }
    if (pthread_create(&a.thread, NULL, worker, &a) != 0) {
        fprintf(stderr, "mellagui-smart: no thread: %s\n", strerror(errno));
        wlwin_destroy(&a.win);
        return 1;
    }
    if (logged_in) {
        a.view = V_DEVICES;
        a.poll_busy = push_job(&a, J_LOAD, NULL, NULL, NULL);
        a.poll_at = mono_ms() + (uint64_t)a.poll_ms;
    }
    while (a.win.running) {
        tick(&a);
        if (!wlwin_step(&a.win, 100))
            break;
    }
    wlwin_destroy(&a.win);
    return 0;                                                   /* (the worker may be in the middle of a request: the process just ends) */
}
