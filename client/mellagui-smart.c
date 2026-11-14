/*
 * mellagui-smart - the devices of a Smart Life (Tuya) account: plugs, blinds, a robot vacuum ... in a window.
 *
 *   mellagui-smart                     the window: log in once with a QR code, then the devices with their switches, buttons and sliders
 *   mellagui-smart --list              the devices and everything they report, as text (needs a login)
 *   mellagui-smart --set ID CODE VALUE tell a device something (VALUE: true / false, a number, or a word such as open)
 *   mellagui-smart --login CODE        log in from a terminal: the QR code is printed there
 *   mellagui-smart --logout            forget the login
 *   mellagui-smart --set-local ID CODE VALUE   the same, but only over the local network, and it says how that went
 *   mellagui-smart --get-local ID              asks the device itself for its values over the local network
 *   mellagui-smart --background        carry out the timetable (smartsched.h; no window, started with the compositor)
 *   mellagui-smart --sun               today's sunrise and sunset at the place the timetable uses (smartsun.h)
 *   mellagui-smart --place LAT LON     the place for them (degrees, east positive); `--place auto` goes back to the one of the time zone
 *   mellagui-smart --scenes            the scenes ("Tap-to-Run") made in the app
 *   mellagui-smart --scene NAME        start one of them (for a key binding: Super+F1 = exec mellagui-smart --scene "Movie night")
 *   mellagui-smart --scan [SECONDS]    listen for the devices in this network (default 20 s): their local addresses and protocol versions
 *   mellagui-smart --qr TEXT           print a QR code of TEXT (to try the encoder with a phone)
 *   mellagui-smart --selftest
 *
 * The login: in the Smart Life app, Me > Settings (the gear) > Account and Security > User Code. That code is typed here, the program asks the
 * cloud for a login code, shows it as a QR code, and the app's scanner confirms it. The password never comes near this program; what it keeps
 * are the tokens (smart.conf, mode 0600). The cloud calls are in tuya.c and run in a worker thread, so the window never waits for the network.
 *
 * A card shows a device's main controls: a truth value is a switch, a number with a range a slider (sent when it is let go), the blind's open /
 * stop / close are three buttons. "Details" (or a click on the card) opens the device's own view over the window: every control - a choice is a
 * row of buttons - and everything the device reports. A robot vacuum gets a remote control there: start / stop, return to the dock, the modes,
 * the suction, and a cross of arrows to drive it by hand (hold a button or an arrow key; it stops when that is let go). The values are asked again every 10 s ($MELLAGUI_SMART_POLL_MS) and shortly after a command.
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
#include <strings.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

#include "font.h"
#include "json.h"
#include "keymap.h"
#include "protocol.h"
#include "qr.h"
#include "smartfav.h"
#include "smartsched.h"
#include "smartsun.h"
#include "theme.h"
#include "tuya.h"
#include "tuyalan.h"
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
/* C_TOGGLE a switch; C_BUTTON sets its value; C_SLIDER a number, sent when let go; C_PUSH a truth value as a big button (label = what a press does while
 * it is off, value = while it is on); C_HOLD sends its value when pressed and "stop" when let go (driving the robot); C_DETAILS opens the device's own
 * view, C_CLOSE closes it; C_LABEL / C_TEXT are a heading and a line of text in that view. */
enum ctl_kind { C_TOGGLE, C_BUTTON, C_SLIDER, C_PUSH, C_HOLD, C_DETAILS, C_CLOSE, C_LABEL, C_TEXT, C_EDIT, C_EDSLIDER };
static const char *const KIND_NAME[] = { "toggle", "button", "slider", "push", "hold", "details", "close", "label", "text", "edit", "edslider" };
/* C_EDIT is a button of the timetable (its `value` says what it does: "add", "tog:N", "del:N", "h+", "m-", "d3", "a2", "save", "cancel"), C_EDSLIDER the
 * number of the action being made (the blind's position). They tell the device nothing. */

struct box { int x, y, w, h; };

struct ctl {
    enum ctl_kind kind;
    int dev, dp;                        /* indices into app.dev / its dp (dp -1 for C_MORE) */
    struct box b;
    char label[100];
    char value[24];                     /* C_BUTTON, C_HOLD: what it sends */
    bool lit;                           /* C_EDIT: chosen (a day, an action), or a rule that is on */
    bool modal;                         /* it belongs to the device's own view, not to a card */
    bool hidden;                        /* ... and is scrolled out of it */
};

/* A value the user just set: the cloud's answers are not believed about it for a few seconds (they lag behind). */
struct hold {
    char dev[40], code[40];
    bool b;
    double num;
    char str[64];
    uint64_t until;
};

enum job_type { J_QR_START, J_QR_POLL, J_LOAD, J_POLL, J_CMD, J_SCENE, J_LOGOUT, J_QUIT };

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
    bool have_scenes;
    struct tuya_scene scenes[TUYA_MAX_SCENE];
    int nscenes;
};

/* A device heard in this network (tuyalan). */
struct lanent {
    char id[40], ip[48], version[8];
    uint64_t at;                        /* when it was heard last (UINT64_MAX: for ever, tests) */
};
#define MAX_LAN 64
#define LAN_STALE_MS 180000

/* What a device said itself when it was asked in this network (tuyalan_get), and what is needed to ask it. */
struct lanread {
    char id[40], key[40], ask[400];
    char json[2048];
    uint64_t at;                        /* when it answered last, 0 = never */
    int fails, skip;                    /* one that does not answer is asked less often */
};
#define LAN_VALUE_MS 30000              /* an answer older than this is not believed over the cloud's */

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
    int scroll, content_h;
    struct box card[TUYA_MAX_DEV];
    struct tuya_scene scene[TUYA_MAX_SCENE];    /* the app's scenes: a row of buttons above the cards */
    struct box scene_box[TUYA_MAX_SCENE];
    int nscene;
    char modal_dev[40];                 /* the device whose own view is open ("" = none) */
    char want_dev[40];                  /* a device whose view was asked for (the gadget's card was clicked, `--device ID`): opened as soon as the devices are known */
    int modal_scroll, modal_h;
    struct box modal;
    int sb_drag;                        /* a scroll bar's thumb is in the hand: 1 = the list's, 2 = the device view's */
    int sb_grab;                        /* ... and where it was taken, from its top */
    bool confirm_logout;                /* the foot asks "Log out?" */
    /* the timetable (smartsched.h) and the form that adds a rule to the open device */
    struct sched_rule rules[SCHED_MAX];
    int nrules;
    struct {
        bool open;
        int hour, min;
        int kind;                       /* 0 = at hour:min, 1 = at sunrise, 2 = at sunset */
        int offset;                     /* minutes after (+) or before (-) the sun, -180..180 */
        unsigned days;
        int action;                     /* index into acts[] */
        double num;                     /* the number of an action that has one */
    } ed;
    struct sact {
        int dp;
        bool is_int;
        char value[24], label[64];
    } acts[24];
    int nacts;
    bool ed_drag;
    bool holding;                       /* a C_HOLD button or an arrow key is down: "stop" goes out when it is let go */
    char hold_dev[40], hold_code[40];
    struct ctl ctl[MAX_CTL];
    int nctl;
    bool dragging;
    char drag_dev[40], drag_code[40];
    struct hold hold[MAX_HOLD];
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
    /* the local network: a thread of its own listens for the devices */
    pthread_t scan_thread;
    pthread_mutex_t lan_lock;
    struct lanent lan[MAX_LAN];
    int nlan;
    bool lan_changed;
    int lan_cmds;                       /* commands that went the local way (shown in the dump) */
    /* ... and another one asks them for their values */
    pthread_t read_thread;
    pthread_mutex_t lan_io;             /* one conversation with a device at a time (a command does not meet a question) */
    struct lanread *read;               /* [TUYA_MAX_DEV], under lan_lock */
    int nread, lan_reads;               /* lan_reads: the answers so far (shown in the dump) */
    bool read_changed, read_kick;
    int read_ms;
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

static bool now_stale(uint64_t at)
{
    return mono_ms() - at >= LAN_STALE_MS;
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

/* ---- the local network ------------------------------------------------------------------------------------------------------------------- */

static bool lan_lookup(struct app *a, const char *id, struct lanent *out)
{
    bool found = false;
    uint64_t now = mono_ms();
    pthread_mutex_lock(&a->lan_lock);
    for (int i = 0; i < a->nlan && !found; i++)
        if (!strcmp(a->lan[i].id, id) && (a->lan[i].at == UINT64_MAX || now - a->lan[i].at < LAN_STALE_MS)) {
            if (out)
                *out = a->lan[i];
            found = true;
        }
    pthread_mutex_unlock(&a->lan_lock);
    return found;
}

static void lan_note(struct app *a, const char *id, const char *ip, const char *version, uint64_t at)
{
    pthread_mutex_lock(&a->lan_lock);
    struct lanent *e = NULL;
    for (int i = 0; i < a->nlan; i++)
        if (!strcmp(a->lan[i].id, id))
            e = &a->lan[i];
    if (!e && a->nlan < MAX_LAN) {
        e = &a->lan[a->nlan++];
        memset(e, 0, sizeof(*e));
        a->lan_changed = true;
    }
    if (e) {
        if (e->at != UINT64_MAX && now_stale(e->at))
            a->lan_changed = true;
        copy(e->id, sizeof(e->id), id);
        copy(e->ip, sizeof(e->ip), ip);
        copy(e->version, sizeof(e->version), version);
        e->at = at;
    }
    pthread_mutex_unlock(&a->lan_lock);
}

static void *scanner(void *arg)
{
    struct app *a = arg;
    for (;;) {
        static struct tuyalan_found found[MAX_LAN];
        char err[200];
        int n = tuyalan_scan(3000, found, MAX_LAN, NULL, err, sizeof(err));
        if (n < 0) {                                            /* the ports cannot be opened: everything goes through the cloud */
            struct timespec ts = { .tv_sec = 30 };
            nanosleep(&ts, NULL);
            continue;
        }
        for (int i = 0; i < n; i++)
            lan_note(a, found[i].id, found[i].ip, found[i].version, mono_ms());
    }
    return NULL;
}

/* Tells the device the value over the local network when that data point can go that way: the return values of tuyalan_set(). */
static int lan_send(const struct lanent *e, struct tuya_dev *d, const char *code, const char *value_json, char *err, size_t errsz)
{
    struct tuya_dp *dp = tuya_find_dp(d, code);
    char dps[200];
    if (!dp || !dp->lan_ok || dp->lan_id <= 0) {
        snprintf(err, errsz, "%s only goes through the cloud", code);
        return 0;
    }
    snprintf(dps, sizeof(dps), "{\"%d\":%s}", dp->lan_id, value_json);
    return tuyalan_set(e->ip, e->version, d->id, d->local_key, dps, err, errsz);
}

/* The devices heard in this network are asked for their values every few seconds (and right after a command): the cloud's list lags behind and
 * needs the internet. The answers wait in a->read for the window's thread (lan_values_apply). */
static void *lan_reader(void *arg)
{
    struct app *a = arg;
    static char json[2048];
    for (;;) {
        uint64_t until = mono_ms() + (uint64_t)a->read_ms;
        bool kick = false;
        while (!kick && mono_ms() < until) {
            struct timespec ts = { .tv_nsec = 100 * 1000000 };
            nanosleep(&ts, NULL);
            pthread_mutex_lock(&a->lan_lock);
            kick = a->read_kick;
            a->read_kick = false;
            pthread_mutex_unlock(&a->lan_lock);
        }
        if (kick) {                                             /* (the device needs a moment to do what it was told) */
            struct timespec ts = { .tv_nsec = 300 * 1000000 };
            nanosleep(&ts, NULL);
        }
        for (int i = 0;; i++) {
            char id[40], key[40], ask[400], err[200];
            struct lanent e;
            pthread_mutex_lock(&a->lan_lock);
            bool more = i < a->nread, skip = false;
            if (more) {
                struct lanread *r = &a->read[i];
                copy(id, sizeof(id), r->id);
                copy(key, sizeof(key), r->key);
                copy(ask, sizeof(ask), r->ask);
                if (!kick && r->skip > 0) {
                    r->skip--;
                    skip = true;
                }
            }
            pthread_mutex_unlock(&a->lan_lock);
            if (!more)
                break;
            if (skip || !key[0] || !lan_lookup(a, id, &e))
                continue;
            pthread_mutex_lock(&a->lan_io);
            bool ok = tuyalan_get(e.ip, e.version, id, key, ask, json, sizeof(json), err, sizeof(err));
            pthread_mutex_unlock(&a->lan_io);
            pthread_mutex_lock(&a->lan_lock);
            if (i < a->nread && !strcmp(a->read[i].id, id)) {
                struct lanread *r = &a->read[i];
                if (ok) {
                    if (strcmp(r->json, json) || r->at == 0 || mono_ms() - r->at >= LAN_VALUE_MS)
                        a->read_changed = true;
                    copy(r->json, sizeof(r->json), json);
                    r->at = mono_ms();
                    r->fails = r->skip = 0;
                    a->lan_reads++;
                } else {
                    r->fails = LM_MIN(r->fails + 1, 6);
                    r->skip = r->fails;
                }
            }
            pthread_mutex_unlock(&a->lan_lock);
        }
    }
    return NULL;
}

/* The worker's list changed: whom the reader may ask (the key and the numbers come from the cloud). */
static void lan_read_targets(struct app *a)
{
    pthread_mutex_lock(&a->lan_lock);
    for (int i = 0; i < a->nmaster && a->read; i++) {
        const struct tuya_dev *d = &a->master[i];
        struct lanread *r = NULL;
        if (!d->local_key[0] || !d->map)
            continue;
        for (int k = 0; k < a->nread && !r; k++)
            if (!strcmp(a->read[k].id, d->id))
                r = &a->read[k];
        if (!r && a->nread < TUYA_MAX_DEV) {
            r = &a->read[a->nread++];
            memset(r, 0, sizeof(*r));
            copy(r->id, sizeof(r->id), d->id);
            a->read_kick = true;                                /* (a new one is asked at once) */
        }
        if (r) {
            copy(r->key, sizeof(r->key), d->local_key);
            tuya_local_question(d, r->ask, sizeof(r->ask));
        }
    }
    if (a->nmaster == 0)
        a->nread = 0;
    pthread_mutex_unlock(&a->lan_lock);
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
    lan_read_targets(a);
}

/* The devices and their values; a device seen for the first time is also asked what it can be told. */
static void load_all(struct app *a, bool scenes)
{
    char err[200] = "";
    if (scenes) {                                               /* (they change rarely: at the start and when the user asks again) */
        static struct tuya_scene list[TUYA_MAX_SCENE];
        int n = 0;
        if (tuya_load_scenes(&a->sess, list, &n, err, sizeof(err))) {
            pthread_mutex_lock(&a->lock);
            memcpy(a->res.scenes, list, sizeof(list));
            a->res.nscenes = n;
            a->res.have_scenes = true;
            pthread_mutex_unlock(&a->lock);
        }
        err[0] = 0;
    }
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
        for (int i = 0; i < a->nmaster; i++) {                  /* the numbers of the local protocol, once per device */
            char e2[200];
            if (!a->master[i].map && a->master[i].local_key[0]) {
                tuya_load_local_map(&a->sess, &a->master[i], e2, sizeof(e2));
                asked++;
            }
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
                load_all(a, true);
            }
            break;
        }
        case J_LOAD:
        case J_POLL:
            load_all(a, j.type == J_LOAD || j.a[0]);
            break;
        case J_SCENE: {
            char text[200];
            bool ok = tuya_trigger_scene(&a->sess, j.a, j.b, err, sizeof(err));
            snprintf(text, sizeof(text), "%s%.100s", ok ? "Started: " : "", ok ? j.c : err);
            result_msg(a, !ok, text);
            break;
        }
        case J_CMD: {                                           /* the local network when the device is heard there, else the cloud */
            struct tuya_dev *d = NULL;
            struct lanent e;
            int r = 0;
            for (int i = 0; i < a->nmaster; i++)
                if (!strcmp(a->master[i].id, j.a))
                    d = &a->master[i];
            if (d && lan_lookup(a, d->id, &e)) {
                pthread_mutex_lock(&a->lan_io);
                r = lan_send(&e, d, j.b, j.c, err, sizeof(err));
                pthread_mutex_unlock(&a->lan_io);
            }
            pthread_mutex_lock(&a->lan_lock);
            if (r == 1)
                a->lan_cmds++;
            a->read_kick = true;                                /* what the device says now is asked for right away */
            pthread_mutex_unlock(&a->lan_lock);
            if (r != 1 && (r < 0 || !tuya_command(&a->sess, j.a, j.b, j.c, err, sizeof(err)))) {
                result_msg(a, true, err);                       /* (sent but not confirmed: not sent a second time through the cloud) */
            }
            break;
        }
        case J_LOGOUT:
            tuya_session_forget(&a->sess);
            a->nmaster = 0;
            lan_read_targets(a);
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

/* What the devices said themselves in this network goes over the cloud's values (the window's list). */
static void lan_values_apply(struct app *a)
{
    uint64_t now = mono_ms();
    pthread_mutex_lock(&a->lan_lock);
    a->read_changed = false;
    for (int i = 0; i < a->nread; i++) {
        struct lanread *r = &a->read[i];
        struct tuya_dev *d = r->at && now - r->at < LAN_VALUE_MS ? find_dev(a, r->id) : NULL;
        if (d)
            tuya_apply_local(d, r->json);
    }
    pthread_mutex_unlock(&a->lan_lock);
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
        { "va_temperature", "Temperature" }, { "va_humidity", "Humidity" }, { "manual_feed", "Feed" }, { "feed_state", "Feeder" },
        { "direction_control", "Drive" }, { "chargego", "To the dock" }, { "wall_follow", "Along the walls" }, { "switch_alarm_sound", "Alarm sound" }, { "relay_status", "After a power cut" }, { "light_mode", "Indicator light" },
    };
    if (!strncmp(code, "switch_usb", 10)) {
        snprintf(out, n, "USB %s", code + 10);
        return;
    }
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

/* "Switch this over after a while": countdown_1 belongs to switch_1, countdown_usb1 to switch_usb1 ... The number is seconds (minutes on the few
 * devices whose range ends at a day of minutes). */
static bool is_countdown(const struct tuya_dp *dp)
{
    return dp->func && dp->type == TUYA_INT && !strncmp(dp->code, "countdown", 9) && dp->max >= 600;
}

static int countdown_unit(const struct tuya_dp *dp)
{
    return dp->max > 1440 ? 60 : 1;                             /* what a minute is in the device's number */
}

/* Something the window has a control for. */
static bool controllable(const struct tuya_dp *dp)
{
    if (!dp->func || !strcmp(dp->code, "factory_reset"))       /* (nobody resets a gateway with a stray click) */
        return false;
    return dp->type == TUYA_BOOL || (dp->type == TUYA_ENUM && dp->nrange > 0) || (dp->type == TUYA_INT && dp->max > dp->min && dp->max - dp->min <= 1000);
}

/* The controls a card shows before "More". */
static bool primary(const struct tuya_dev *d, const struct tuya_dp *dp)
{
    static const char *const codes[] = { "power", "power_go", "switch_go", "pause", "switch_charge", "control", "percent_control", "position",
                                         "bright_value", "bright_value_v2", "temp_set", "manual_feed", NULL };
    const char *c = dp->code;
    if (!strncmp(c, "switch", 6) && strcmp(c, "switch_inching") && strcmp(c, "switch_backlight"))
        return true;
    for (int i = 0; codes[i]; i++)
        if (!strcmp(c, codes[i]))
            return true;
    return !strcmp(d->category, "sd") && (!strcmp(c, "mode") || !strcmp(c, "suction"));
}

/* The line under the name: what matters of what the device reports. */
static void info_text(struct tuya_dev *d, char *out, size_t n)
{
    static const char *const codes[] = { "status", "feed_state", "va_temperature", "va_humidity", "temp_current", "humidity_value", "electricity_left",
                                         "battery_percentage", "residual_electricity", "percent_state", "cur_power", "cur_voltage", "cur_current",
                                         "clean_time", "clean_area", NULL };
    size_t o = 0;
    out[0] = 0;
    if (!d->online)
        o += (size_t)snprintf(out, n, "Offline");
    for (int k = 0; k < d->ndp && d->online && o + 40 < n; k++) {      /* a countdown that runs */
        struct tuya_dp *dp = &d->dp[k];
        char code[48];
        if (!is_countdown(dp) || !dp->has || dp->num <= 0)
            continue;
        snprintf(code, sizeof(code), "switch%s", dp->code + 9);
        struct tuya_dp *sw = tuya_find_dp(d, code);
        o += (size_t)snprintf(out + o, n - o, "%s%s in %d min", o ? "   " : "", sw && sw->has && sw->b ? "Off" : "On",
                              LM_MAX((int)(dp->num / countdown_unit(dp) + 0.5), 1));
    }
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
    memset(c, 0, sizeof(*c));
    c->kind = kind;
    c->dev = dev;
    c->dp = dp;
    c->b = b;
    copy(c->label, sizeof(c->label), label);
    copy(c->value, sizeof(c->value), value ? value : "");
    return c;
}

static int dp_index(const struct tuya_dev *d, const char *code)
{
    for (int k = 0; k < d->ndp; k++)
        if (!strcmp(d->dp[k].code, code))
            return k;
    return -1;
}

static bool in_range(const struct tuya_dp *dp, const char *value)
{
    for (int r = 0; r < dp->nrange; r++)
        if (!strcmp(dp->range[r], value))
            return true;
    return false;
}

/* Controls are placed left to right and wrap inside x0..x1. */
struct flow {
    struct app *a;
    int dev;
    bool modal;
    int x0, x1, cx, cy;
    bool used;                          /* the current row has something */
};

static void flow_break(struct flow *f)
{
    if (f->used)
        f->cy += ROW + 6;
    f->cx = f->x0;
    f->used = false;
}

static struct ctl *flow_add(struct flow *f, enum ctl_kind kind, int dp, int w, const char *label, const char *value)
{
    if (f->cx + w > f->x1 && f->used)
        flow_break(f);
    struct ctl *c = add_ctl(f->a, kind, f->dev, dp, (struct box){ f->cx, f->cy, LM_MIN(w, f->x1 - f->x0), ROW }, label, value);
    if (c)
        c->modal = f->modal;
    f->cx += w + 8;
    f->used = true;
    return c;
}

/* A line of its own: a heading (C_LABEL) or plain text (C_TEXT). */
static void flow_line(struct flow *f, enum ctl_kind kind, const char *text)
{
    flow_break(f);
    if (kind == C_LABEL)
        f->cy += 6;
    struct ctl *c = add_ctl(f->a, kind, f->dev, -1, (struct box){ f->x0, f->cy, f->x1 - f->x0, 18 }, text, NULL);
    if (c)
        c->modal = f->modal;
    f->cy += kind == C_LABEL ? 22 : 18;
}

static void flow_toggle(struct flow *f, int k)
{
    char label[48];
    pretty(f->a->dev[f->dev].dp[k].code, label, sizeof(label));
    flow_add(f, C_TOGGLE, k, 44 + 8 + lm_font_text_width(label) + 14, label, NULL);
}

static void flow_slider(struct flow *f, int k)
{
    char label[48];
    pretty(f->a->dev[f->dev].dp[k].code, label, sizeof(label));
    flow_break(f);
    flow_add(f, C_SLIDER, k, f->x1 - f->x0, label, NULL);
    flow_break(f);
}

/* A button per value of a choice (the one that holds is lit). */
static void flow_choices(struct flow *f, int k)
{
    struct tuya_dp *dp = &f->a->dev[f->dev].dp[k];
    for (int r = 0; r < dp->nrange; r++) {
        char label[48];
        pretty(dp->range[r], label, sizeof(label));
        flow_add(f, C_BUTTON, k, lm_font_text_width(label) + 28, label, dp->range[r]);
    }
}

/* The feeder: a button per number of portions, not a slider that feeds when it is let go. */
static void flow_feed(struct flow *f, int k)
{
    struct tuya_dp *dp = &f->a->dev[f->dev].dp[k];
    for (int r = 1; r <= 3 && r <= (int)dp->max; r++) {
        char label[16], v[8];
        snprintf(label, sizeof(label), "Feed %d", r);
        snprintf(v, sizeof(v), "%d", r);
        flow_add(f, C_BUTTON, k, lm_font_text_width(label) + 28, label, v);
    }
}

/* What a card shows: the main controls only. */
static void card_controls(struct flow *f)
{
    struct tuya_dev *d = &f->a->dev[f->dev];
    int ctrl = dp_index(d, "control");
    bool buttons = ctrl >= 0 && controllable(&d->dp[ctrl]) && d->dp[ctrl].nrange <= 4;     /* the blind has open / stop / close */
    for (int k = 0; k < d->ndp; k++) {
        struct tuya_dp *dp = &d->dp[k];
        if (!controllable(dp) || !primary(d, dp))
            continue;
        if (buttons && !strcmp(dp->code, "percent_control"))
            continue;                                           /* the position slider is in the blind's own view (Details), not on the card */
        if (!strcmp(dp->code, "manual_feed"))
            flow_feed(f, k);
        else if (dp->type == TUYA_INT)
            flow_slider(f, k);
        else if (dp->type == TUYA_BOOL)
            flow_toggle(f, k);
        else if (!strcmp(dp->code, "control") && dp->nrange <= 4)     /* the blind's open / stop / close */
            flow_choices(f, k);
        else if (!strcmp(dp->code, "mode") && in_range(dp, "chargego"))
            flow_add(f, C_BUTTON, k, lm_font_text_width("Dock") + 28, "Dock", "chargego");
    }
}

#define PAD_W 52
#define PAD_H 40

/* The robot vacuum's remote control: the big buttons, the modes, the suction, and a cross of arrows to drive it by hand. `done` marks the data
 * points that got their place here. */
static void vacuum_controls(struct flow *f, bool *done)
{
    struct tuya_dev *d = &f->a->dev[f->dev];
    static const char *const go[] = { "power_go", "switch_go", NULL };
    int k;
    for (int i = 0; go[i]; i++)
        if ((k = dp_index(d, go[i])) >= 0 && controllable(&d->dp[k]) && d->dp[k].type == TUYA_BOOL) {
            struct ctl *c = flow_add(f, C_PUSH, k, 150, "Start cleaning", "Stop cleaning");
            (void)c;
            done[k] = true;
            break;
        }
    if ((k = dp_index(d, "pause")) >= 0 && controllable(&d->dp[k]) && d->dp[k].type == TUYA_BOOL) {
        flow_add(f, C_PUSH, k, 100, "Pause", "Go on");
        done[k] = true;
    }
    if ((k = dp_index(d, "switch_charge")) >= 0 && controllable(&d->dp[k]) && d->dp[k].type == TUYA_BOOL) {
        flow_add(f, C_PUSH, k, 150, "Return to dock", "Stop returning");
        done[k] = true;
    } else if ((k = dp_index(d, "mode")) >= 0 && controllable(&d->dp[k]) && in_range(&d->dp[k], "chargego")) {
        flow_add(f, C_BUTTON, k, 150, "Return to dock", "chargego");
    }
    if ((k = dp_index(d, "power")) >= 0 && controllable(&d->dp[k]) && d->dp[k].type == TUYA_BOOL) {
        flow_toggle(f, k);
        done[k] = true;
    }
    static const char *const lists[][2] = { { "mode", "Mode" }, { "suction", "Suction" }, { "cistern", "Water" } };
    for (size_t i = 0; i < sizeof(lists) / sizeof(lists[0]); i++)
        if ((k = dp_index(d, lists[i][0])) >= 0 && controllable(&d->dp[k]) && d->dp[k].type == TUYA_ENUM) {
            flow_line(f, C_LABEL, lists[i][1]);
            flow_choices(f, k);
            done[k] = true;
        }
    if ((k = dp_index(d, "direction_control")) >= 0 && controllable(&d->dp[k]) && d->dp[k].type == TUYA_ENUM) {
        struct tuya_dp *dp = &d->dp[k];
        static const struct { const char *value; int col, row; } keys[] = {
            { "forward", 1, 0 }, { "turn_left", 0, 1 }, { "stop", 1, 1 }, { "turn_right", 2, 1 }, { "backward", 1, 2 },
        };
        flow_line(f, C_LABEL, "Drive by hand (hold a button, or an arrow key)");
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
            if (!in_range(dp, keys[i].value))
                continue;
            struct box b = { f->x0 + keys[i].col * (PAD_W + 6), f->cy + keys[i].row * (PAD_H + 6), PAD_W, PAD_H };
            struct ctl *c = add_ctl(f->a, strcmp(keys[i].value, "stop") ? C_HOLD : C_BUTTON, f->dev, k, b, "", keys[i].value);
            if (c)
                c->modal = true;
        }
        f->cy += 3 * (PAD_H + 6) + 4;
        f->cx = f->x0;
        f->used = false;
        done[k] = true;
    }
}

static void schedule_controls(struct flow *f);

/* Everything of a device, for its own view: every control (a choice is a row of buttons), then what it reports. */
static void modal_controls(struct flow *f)
{
    struct tuya_dev *d = &f->a->dev[f->dev];
    bool done[TUYA_MAX_DP] = { false };
    int left = 0;
    if (!strcmp(d->category, "sd"))
        vacuum_controls(f, done);
    for (int k = 0; k < d->ndp; k++)
        if (controllable(&d->dp[k]) && !done[k])
            left++;
    if (left && f->a->nctl > 0 && f->a->ctl[f->a->nctl - 1].modal && f->a->ctl[f->a->nctl - 1].kind != C_CLOSE)
        flow_line(f, C_LABEL, "Other settings");
    for (int k = 0; k < d->ndp; k++)                            /* the switches together, then the numbers, then the choices */
        if (controllable(&d->dp[k]) && !done[k] && d->dp[k].type == TUYA_BOOL)
            flow_toggle(f, k);
    for (int k = 0; k < d->ndp; k++) {
        if (!controllable(&d->dp[k]) || done[k] || d->dp[k].type != TUYA_INT)
            continue;
        if (!strcmp(d->dp[k].code, "manual_feed")) {
            flow_break(f);
            flow_feed(f, k);
        } else {
            flow_slider(f, k);
        }
    }
    for (int k = 0; k < d->ndp; k++)
        if (controllable(&d->dp[k]) && !done[k] && d->dp[k].type == TUYA_ENUM) {
            char label[48];
            pretty(d->dp[k].code, label, sizeof(label));
            flow_line(f, C_LABEL, label);
            flow_choices(f, k);
        }
    for (int k = 0; k < d->ndp; k++) {                          /* the countdowns: "turn it off after ..." */
        struct tuya_dp *dp = &d->dp[k], *sw;
        char code[48], name[48], label[100];
        static const int minutes[] = { 15, 30, 60, 120 };
        if (!is_countdown(dp))
            continue;
        snprintf(code, sizeof(code), "switch%s", dp->code + 9);
        sw = tuya_find_dp(d, code);
        pretty(code, name, sizeof(name));
        const char *what = sw && sw->has && sw->type == TUYA_BOOL ? (sw->b ? "off" : "on") : "over";
        int left = dp->has ? (int)(dp->num / countdown_unit(dp) + 0.5) : 0;
        if (dp->has && dp->num > 0)
            snprintf(label, sizeof(label), "%s turns %s in %d min", name, what, LM_MAX(left, 1));
        else
            snprintf(label, sizeof(label), "Turn %s %s after", name, what);
        flow_line(f, C_LABEL, label);
        for (size_t i = 0; i < sizeof(minutes) / sizeof(minutes[0]); i++) {
            char text[16], v[16];
            if (minutes[i] * countdown_unit(dp) > dp->max)
                continue;
            if (minutes[i] < 60)
                snprintf(text, sizeof(text), "%d min", minutes[i]);
            else
                snprintf(text, sizeof(text), "%d h", minutes[i] / 60);
            snprintf(v, sizeof(v), "%d", minutes[i] * countdown_unit(dp));
            flow_add(f, C_BUTTON, k, lm_font_text_width(text) + 28, text, v);
        }
        if (dp->has && dp->num > 0)
            flow_add(f, C_BUTTON, k, lm_font_text_width("Cancel") + 28, "Cancel", "0");
    }
    flow_break(f);
    flow_line(f, C_LABEL, "On the desktop");                    /* the pin: the device's main switch (or the blind's buttons) on the desktop card */
    {
        char ids[FAV_MAX][FAV_ID_MAX];
        bool pinned = fav_find(ids, fav_load(ids, FAV_MAX), d->id) >= 0;
        const char *text = pinned ? "Remove it from the desktop" : "Show it on the desktop";
        flow_add(f, C_EDIT, -1, lm_font_text_width(text) + 28, text, "fav");
        struct ctl *pin = f->a->nctl > 0 ? &f->a->ctl[f->a->nctl - 1] : NULL;
        if (pin)
            pin->lit = pinned;
        flow_break(f);
    }
    schedule_controls(f);
    bool head = false;
    for (int k = 0; k < d->ndp; k++) {
        char v[80], name[48], line[160];
        if (controllable(&d->dp[k]) || is_countdown(&d->dp[k]))
            continue;
        if (!head)
            flow_line(f, C_LABEL, "It reports");
        head = true;
        tuya_dp_text(&d->dp[k], v, sizeof(v));
        pretty(d->dp[k].code, name, sizeof(name));
        snprintf(line, sizeof(line), "%s: %s", name, v);
        flow_line(f, C_TEXT, line);
    }
    flow_break(f);
}

/* What a rule can tell this device: its main switches on and off, the blind's buttons and its position as a number, the robot's start and dock, the
 * feeder's portions. */
static void build_actions(struct app *a, struct tuya_dev *d)
{
    a->nacts = 0;
#define ACT(k_, int_, value_, ...) do { if (a->nacts < 24) { struct sact *s_ = &a->acts[a->nacts++]; s_->dp = (k_); s_->is_int = (int_); \
        copy(s_->value, sizeof(s_->value), (value_)); snprintf(s_->label, sizeof(s_->label), __VA_ARGS__); } } while (0)
    for (int k = 0; k < d->ndp; k++) {
        struct tuya_dp *dp = &d->dp[k];
        char name[48];
        if (!controllable(dp) || !primary(d, dp))
            continue;
        pretty(dp->code, name, sizeof(name));
        if (!strcmp(dp->code, "manual_feed")) {
            for (int r = 1; r <= 3 && r <= (int)dp->max; r++) {
                char v[8];
                snprintf(v, sizeof(v), "%d", r);
                ACT(k, false, v, "Feed %d", r);
            }
        } else if (dp->type == TUYA_BOOL && (!strcmp(dp->code, "power_go") || !strcmp(dp->code, "switch_go"))) {
            ACT(k, false, "true", "Start cleaning");
            ACT(k, false, "false", "Stop cleaning");
        } else if (dp->type == TUYA_BOOL) {
            ACT(k, false, "true", "%.40s on", name);
            ACT(k, false, "false", "%.40s off", name);
        } else if (dp->type == TUYA_INT) {
            ACT(k, true, "", "%.40s", name);
        } else if (!strcmp(dp->code, "control")) {
            for (int r = 0; r < dp->nrange; r++) {
                pretty(dp->range[r], name, sizeof(name));
                ACT(k, false, dp->range[r], "%.40s", name);
            }
        } else if (!strcmp(dp->code, "mode") && in_range(dp, "chargego")) {
            ACT(k, false, "chargego", "To the dock");
        }
    }
#undef ACT
}

/* "07:30  weekdays  Position 30%" */
static void rule_text(struct tuya_dev *d, const struct sched_rule *r, char *out, size_t n)
{
    char days[40], what[80], name[48];
    struct tuya_dp *dp = tuya_find_dp(d, r->code);
    sched_days_text(r->days, days, sizeof(days));
    pretty(r->code, name, sizeof(name));
    if (dp && dp->type == TUYA_BOOL && (!strcmp(r->code, "power_go") || !strcmp(r->code, "switch_go")))
        snprintf(what, sizeof(what), "%s", !strcmp(r->value, "true") ? "Start cleaning" : "Stop cleaning");
    else if (dp && dp->type == TUYA_BOOL)
        snprintf(what, sizeof(what), "%.40s %s", name, !strcmp(r->value, "true") ? "on" : "off");
    else if (dp && dp->type == TUYA_INT && !strcmp(r->code, "manual_feed"))
        snprintf(what, sizeof(what), "Feed %.10s", r->value);
    else if (dp && dp->type == TUYA_INT) {
        struct tuya_dp v = *dp;
        char num[40];
        v.has = true;
        v.num = atof(r->value);
        tuya_dp_text(&v, num, sizeof(num));
        snprintf(what, sizeof(what), "%.30s %s", name, num);
    } else {
        pretty(r->value, what, sizeof(what));
    }
    char at[48];
    sched_time_text(r, at, sizeof(at));
    int due;
    if (r->sun && sched_minute(r, time(NULL), &due)) {          /* a sun rule says what time that makes today */
        char tail[16];
        snprintf(tail, sizeof(tail), " (%02d:%02d)", due / 60, due % 60);
        if (strlen(at) + strlen(tail) < sizeof(at))
            strcat(at, tail);
    }
    snprintf(out, n, "%s  %s  %s", at, days, what);
}

static struct ctl *modal_ctl(struct flow *f, enum ctl_kind kind, int dp, struct box b, const char *label, const char *value, bool lit)
{
    struct ctl *c = add_ctl(f->a, kind, f->dev, dp, b, label, value);
    if (c) {
        c->modal = true;
        c->lit = lit;
    }
    return c;
}

/* The timetable of the open device: its rules (each can be switched off or deleted) and the form that adds one. */
static void schedule_controls(struct flow *f)
{
    struct app *a = f->a;
    struct tuya_dev *d = &a->dev[f->dev];
    build_actions(a, d);
    if (!a->nacts)
        return;                                                 /* nothing a rule could tell it */
    flow_line(f, C_LABEL, "Timetable (while this computer is on)");
    for (int i = 0; i < a->nrules; i++) {
        char text[160], v[16];
        if (strcmp(a->rules[i].dev, d->id))
            continue;
        rule_text(d, &a->rules[i], text, sizeof(text));
        modal_ctl(f, C_TEXT, -1, (struct box){ f->x0, f->cy + 4, f->x1 - f->x0 - 124, 18 }, text, NULL, false);
        snprintf(v, sizeof(v), "tog:%d", i);
        modal_ctl(f, C_EDIT, -1, (struct box){ f->x1 - 116, f->cy, 48, 24 }, a->rules[i].on ? "On" : "Off", v, a->rules[i].on);
        snprintf(v, sizeof(v), "del:%d", i);
        modal_ctl(f, C_EDIT, -1, (struct box){ f->x1 - 62, f->cy, 62, 24 }, "Delete", v, false);
        f->cy += 30;
    }
    f->cx = f->x0;
    f->used = false;
    if (!a->ed.open) {
        struct ctl *c = flow_add(f, C_EDIT, -1, lm_font_text_width("Add a time...") + 28, "Add a time...", "add");
        (void)c;
        flow_break(f);
        return;
    }
    char text[120];
    static const char *const day[7] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
    static const char *const step[4][2] = { { "h-", "- 1 h" }, { "h+", "+ 1 h" }, { "m-", "- 5 min" }, { "m+", "+ 5 min" } };
    static const char *const ostep[4][2] = { { "o:-30", "- 30 min" }, { "o:-5", "- 5 min" }, { "o:5", "+ 5 min" }, { "o:30", "+ 30 min" } };
    static const char *const kinds[3][2] = { { "k:0", "At a time" }, { "k:1", "At sunrise" }, { "k:2", "At sunset" } };
    for (int i = 0; i < 3; i++) {
        struct ctl *c = flow_add(f, C_EDIT, -1, lm_font_text_width(kinds[i][1]) + 28, kinds[i][1], kinds[i][0]);
        if (c)
            c->lit = i == a->ed.kind;
    }
    flow_break(f);
    if (a->ed.kind == 0) {
        snprintf(text, sizeof(text), "At %02d:%02d", a->ed.hour, a->ed.min);
        flow_line(f, C_TEXT, text);
        f->cy += 4;
        for (int i = 0; i < 4; i++)
            flow_add(f, C_EDIT, -1, lm_font_text_width(step[i][1]) + 24, step[i][1], step[i][0]);
    } else {
        struct sched_rule probe = { .sun = a->ed.kind, .offset = a->ed.offset };
        char at[48], hint[64] = "";
        int due;
        double lat, lon;
        char place[64];
        sched_time_text(&probe, at, sizeof(at));
        if (sched_minute(&probe, time(NULL), &due))
            snprintf(hint, sizeof(hint), ", today %02d:%02d", due / 60, due % 60);
        else if (!sun_place(&lat, &lon, place, sizeof(place)))
            snprintf(hint, sizeof(hint), " - the place is not known");
        snprintf(text, sizeof(text), "At %s%s", at, hint);
        flow_line(f, C_TEXT, text);
        f->cy += 4;
        for (int i = 0; i < 4; i++)
            flow_add(f, C_EDIT, -1, lm_font_text_width(ostep[i][1]) + 24, ostep[i][1], ostep[i][0]);
        if (sun_place(&lat, &lon, place, sizeof(place))) {
            flow_break(f);
            snprintf(text, sizeof(text), "Place: %.2f %c %.2f %c (%s)", lat < 0 ? -lat : lat, lat < 0 ? 'S' : 'N', lon < 0 ? -lon : lon, lon < 0 ? 'W' : 'E', place);
            flow_line(f, C_TEXT, text);
        } else {
            flow_break(f);
            flow_line(f, C_TEXT, "Tell the place with: mellagui-smart --place LAT LON");
        }
    }
    flow_break(f);
    for (int i = 0; i < 7; i++) {
        char v[4] = { 'd', (char)('0' + i), 0 };
        struct ctl *c = flow_add(f, C_EDIT, -1, 46, day[i], v);
        if (c)
            c->lit = a->ed.days & (1u << i);
    }
    flow_add(f, C_EDIT, -1, lm_font_text_width("Weekdays") + 24, "Weekdays", "dw");
    flow_add(f, C_EDIT, -1, lm_font_text_width("Every day") + 24, "Every day", "da");
    flow_break(f);
    a->ed.action = LM_CLAMP(a->ed.action, 0, a->nacts - 1);
    for (int i = 0; i < a->nacts; i++) {
        char v[16];
        snprintf(v, sizeof(v), "a%d", i);
        struct ctl *c = flow_add(f, C_EDIT, -1, lm_font_text_width(a->acts[i].label) + 28, a->acts[i].label, v);
        if (c)
            c->lit = i == a->ed.action;
    }
    flow_break(f);
    if (a->acts[a->ed.action].is_int) {                         /* the number: the blind's position in per cent */
        struct ctl *c = flow_add(f, C_EDSLIDER, a->acts[a->ed.action].dp, f->x1 - f->x0, a->acts[a->ed.action].label, NULL);
        (void)c;
        flow_break(f);
    }
    flow_add(f, C_EDIT, -1, 80, "Save", "save");
    flow_add(f, C_EDIT, -1, 80, "Cancel", "cancel");
    flow_break(f);
}

#define MODAL_HEAD 62

/* The room the scroll bars take at the right edge of the list and of the device view ([windows] scrollbar-width). */
static int sbar_w(void)
{
    return LM_CLAMP(lm_metrics.scrollbar_width, 6, 30);
}

/* Places every card and its controls for the window's width and the scroll position, and the open device view over them (paint and the pointer both
 * go by it). */
static void build_layout(struct app *a)
{
    int bw = a->win.bw - sbar_w() - 2, y = HEAD_H + PAD - a->scroll;
    a->nctl = 0;
    if (a->nscene > 0) {                                        /* the scenes: a heading and a row of buttons that wraps */
        int cx = PAD, cy = y + 22;
        for (int i = 0; i < a->nscene; i++) {
            int w = LM_MIN(lm_font_text_width(a->scene[i].name) + 28, bw - 2 * PAD);
            if (cx + w > bw - PAD && cx > PAD) {
                cx = PAD;
                cy += ROW + 6;
            }
            a->scene_box[i] = (struct box){ cx, cy, w, ROW };
            cx += w + 8;
        }
        y = cy + ROW + 14;
    }
    for (int i = 0; i < a->ndev; i++) {
        struct flow f = { .a = a, .dev = i, .x0 = PAD + 12, .x1 = bw - PAD - 12, .cx = PAD + 12, .cy = y + 54 };
        card_controls(&f);
        flow_break(&f);
        add_ctl(a, C_DETAILS, i, -1, (struct box){ bw - PAD - 12 - 68, y + 8, 68, 22 }, "Details", NULL);
        a->card[i] = (struct box){ PAD, y, bw - 2 * PAD, f.cy + 6 - y };
        y = f.cy + 6 + 10;
    }
    a->content_h = y + a->scroll - HEAD_H;
    struct tuya_dev *d = a->modal_dev[0] ? find_dev(a, a->modal_dev) : NULL;
    if (!d) {
        a->modal_dev[0] = 0;
        return;
    }
    bw = a->win.bw;
    int mw = LM_MIN(bw - 24, 500), room = a->win.bh - 48, first = a->nctl;
    int mx = (bw - mw) / 2, top = 24, ctop = top + MODAL_HEAD;
    struct flow f = { .a = a, .dev = (int)(d - a->dev), .modal = true, .x0 = mx + 16, .x1 = mx + mw - 10 - sbar_w(), .cx = mx + 16, .cy = ctop - a->modal_scroll };
    modal_controls(&f);
    a->modal_h = f.cy + a->modal_scroll - ctop + 12;
    int mh = LM_MIN(MODAL_HEAD + a->modal_h, room);
    a->modal = (struct box){ mx, top, mw, mh };
    for (int i = first; i < a->nctl; i++)                       /* what is scrolled out of the view is not there */
        a->ctl[i].hidden = a->ctl[i].b.y < ctop || a->ctl[i].b.y + a->ctl[i].b.h > top + mh - 6;
    struct ctl *c = add_ctl(a, C_CLOSE, f.dev, -1, (struct box){ mx + mw - 16 - 60, top + 12, 60, 24 }, "Close", NULL);
    if (c)
        c->modal = true;
}

/* A scroll bar: its strip, how much there is and how much shows, and the scroll position it moves. */
struct sbar {
    struct box track;
    int content, room, *scroll;
};

/* The list's bar (which = 1) or the device view's (2): false when everything shows. */
static bool sbar_get(struct app *a, int which, struct sbar *s)
{
    if (which == 1) {
        s->room = a->win.bh - HEAD_H - FOOT_H;
        s->content = a->content_h;
        s->scroll = &a->scroll;
        s->track = (struct box){ a->win.bw - sbar_w() - 2, HEAD_H - 4, sbar_w(), s->room + 2 };
    } else {
        if (!a->modal_dev[0])
            return false;
        s->room = a->modal.h - MODAL_HEAD;
        s->content = a->modal_h;
        s->scroll = &a->modal_scroll;
        s->track = (struct box){ a->modal.x + a->modal.w - sbar_w() - 5, a->modal.y + MODAL_HEAD - 2, sbar_w(), s->room - 6 };
    }
    return s->content > s->room && s->room > 40;
}

static struct box sbar_thumb(const struct sbar *s)
{
    int h = LM_MAX(s->track.h * s->room / s->content, 24), max = s->content - s->room;
    int y = s->track.y + (max > 0 ? (int)((long long)(s->track.h - h) * LM_CLAMP(*s->scroll, 0, max) / max) : 0);
    return (struct box){ s->track.x, y, s->track.w, h };
}

/* The thumb in the hand follows the pointer. */
static void sbar_drag(struct app *a)
{
    struct sbar s;
    if (!a->sb_drag || !sbar_get(a, a->sb_drag, &s))
        return;
    struct box t = sbar_thumb(&s);
    int span = s.track.h - t.h, max = s.content - s.room;
    if (span > 0)
        *s.scroll = LM_CLAMP((int)(((long long)((int)a->py - a->sb_grab - s.track.y)) * max / span), 0, max);
}

/* A press on a scroll bar: the thumb is taken, a click beside it turns a page. True when the press was the bar's. */
static bool sbar_press(struct app *a, int which)
{
    struct sbar s;
    if (!sbar_get(a, which, &s) || !in(a->px, a->py, s.track))
        return false;
    struct box t = sbar_thumb(&s);
    if (in(a->px, a->py, t)) {
        a->sb_drag = which;
        a->sb_grab = (int)a->py - t.y;
    } else {
        *s.scroll += a->py < t.y ? -(s.room - 40) : s.room - 40;
    }
    return true;
}

static void sbar_paint(struct app *a, int which)
{
    struct sbar s;
    if (!sbar_get(a, which, &s))
        return;
    struct box t = sbar_thumb(&s);
    bool hot = a->sb_drag == which || (!a->sb_drag && in(a->px, a->py, t) && (which == 2 || !a->modal_dev[0]));
    wlwin_round_fill(&a->win, s.track.x, s.track.y, s.track.w, s.track.h, s.track.w / 2, lm_theme.field);
    wlwin_round_fill(&a->win, t.x + 2, t.y + 2, t.w - 4, t.h - 4, (t.w - 4) / 2, hot ? lm_theme.button_hover : lm_theme.button_down);
}

static void clamp_scroll(struct app *a)
{
    int room = a->win.bh - HEAD_H - FOOT_H;
    int max = a->content_h - room;
    a->scroll = LM_CLAMP(a->scroll, 0, LM_MAX(max, 0));
    max = MODAL_HEAD + a->modal_h - (a->win.bh - 48);
    a->modal_scroll = LM_CLAMP(a->modal_scroll, 0, LM_MAX(max, 0));
}

/* ---- painting ----------------------------------------------------------------------------------------------------------------------------- */

static struct box btn_refresh(const struct app *a) { return (struct box){ a->win.bw - 104, 15, 90, 28 }; }
/* Logging out is out of the way: a dim "Account" at the foot asks first, and the button that does it appears beside the pointer, not under it. */
static struct box btn_account(const struct app *a) { return (struct box){ a->win.bw - 14 - 76, a->win.bh - FOOT_H + 4, 76, 22 }; }
static struct box btn_logout(const struct app *a) { return (struct box){ a->win.bw - 14 - 76 - 8 - 76, a->win.bh - FOOT_H + 4, 76, 22 }; }
static struct box login_field(const struct app *a) { return (struct box){ 24, 250, LM_MIN(a->win.bw - 48, 260), 30 }; }
static struct box login_go(const struct app *a) { struct box f = login_field(a); return (struct box){ f.x, f.y + 44, 150, 30 }; }
static struct box qr_back(const struct app *a) { return (struct box){ 24, a->win.bh - FOOT_H - 40, 90, 28 }; }

static void rbox(struct wlwin *w, struct box b, int r, uint32_t rgb)
{
    wlwin_round_fill(w, b.x, b.y, b.w, b.h, r, rgb);
}

static void button(struct wlwin *w, struct box b, const char *label, bool hot, uint32_t bg, uint32_t fg)
{
    wlwin_drop(w, b.x, b.y, b.w, b.h, LM_MAX(lm_button_radius(b.h / 2), 3), hot ? lm_theme.button_hover : bg);
    wlwin_text(w, b.x + (b.w - lm_font_text_width(label)) / 2, b.y + (b.h - LM_FONT_H) / 2, label, 1, fg, 0);
}

/* Text broken at spaces to fit max_w; returns the y after the last line. */
static int wrap_text(struct wlwin *w, int x, int y, int max_w, const char *text, uint32_t rgb)
{
    while (*text) {
        int n = (int)strlen(text), per = LM_MAX(lm_font_fit(text, max_w), 8);     /* what of it fits the width */
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
    pthread_mutex_lock(&a->lan_lock);
    int lan_cmds = a->lan_cmds, lan_reads = a->lan_reads;
    pthread_mutex_unlock(&a->lan_lock);
    if (a->view == V_DEVICES && !a->modal_dev[0]) {
        struct box b = a->confirm_logout ? btn_logout(a) : btn_account(a);
        fprintf(f, "foot %s %d %d %d %d\n", a->confirm_logout ? "logout" : "account", b.x, b.y, b.w, b.h);
    } else {
        fprintf(f, "foot - 0 0 0 0\n");
    }
    fprintf(f, "view=%s ndev=%d loaded=%d err=%d lan=%d reads=%d modal=%s\nmsg %s\nqr %s\n", a->view == V_LOGIN ? "login" : a->view == V_QR ? "qr" : "devices", a->ndev,
            a->loaded, a->msg_err, lan_cmds, lan_reads, a->modal_dev[0] ? a->modal_dev : "-", a->msg, a->view == V_QR && a->have_qr ? a->qr_token : "");
    if (a->view == V_DEVICES) {
        for (int i = 0; i < a->ndev; i++)
            fprintf(f, "dev %s online=%d local=%d name=%s\n", a->dev[i].id, a->dev[i].online, lan_lookup(a, a->dev[i].id, NULL), a->dev[i].name);
        for (int which = 1; which <= 2; which++) {              /* "bar list|view x y w h thumb_y thumb_h scroll" */
            struct sbar s;
            if (sbar_get(a, which, &s)) {
                struct box t = sbar_thumb(&s);
                fprintf(f, "bar %s %d %d %d %d %d %d %d\n", which == 1 ? "list" : "view", s.track.x, s.track.y, s.track.w, s.track.h, t.y, t.h, *s.scroll);
            }
        }
        for (int i = 0; i < a->nscene; i++)
            fprintf(f, "scene %s %d %d %d %d %s\n", a->scene[i].id, a->scene_box[i].x, a->scene_box[i].y, a->scene_box[i].w, a->scene_box[i].h, a->scene[i].name);
        for (int i = 0; i < a->nctl; i++) {                     /* "ctl" = on a card, "mctl" = in the open device view */
            const struct ctl *c = &a->ctl[i];
            struct tuya_dev *d = &a->dev[c->dev];
            char v[100] = "-";
            if (c->hidden || c->kind == C_LABEL)
                continue;
            if (c->kind == C_TEXT)
                copy(v, sizeof(v), c->label);
            else if (c->kind == C_EDIT)
                snprintf(v, sizeof(v), "%s %s%s", c->value, c->lit ? "*" : "", c->label);
            else if (c->kind == C_EDSLIDER)
                snprintf(v, sizeof(v), "%g", a->ed.num);
            else if (c->dp >= 0 && (c->kind == C_BUTTON || c->kind == C_HOLD))
                copy(v, sizeof(v), c->value);
            else if (c->dp >= 0 && d->dp[c->dp].type == TUYA_INT)
                snprintf(v, sizeof(v), "%g", d->dp[c->dp].num);
            else if (c->dp >= 0 && d->dp[c->dp].type == TUYA_BOOL)
                copy(v, sizeof(v), d->dp[c->dp].b ? "on" : "off");
            else if (c->dp >= 0)
                copy(v, sizeof(v), d->dp[c->dp].str);
            fprintf(f, "%s %s %s %s %d %d %d %d %s\n", c->modal ? "mctl" : "ctl", d->id, c->dp >= 0 ? d->dp[c->dp].code : "-", KIND_NAME[c->kind], c->b.x, c->b.y,
                    c->b.w, c->b.h, v);
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

/* A filled arrow (or the stop square) in the middle of a button of the driving cross. */
static void paint_arrow(struct wlwin *w, struct box b, const char *dir, uint32_t rgb)
{
    int cx = b.x + b.w / 2, cy = b.y + b.h / 2, n = 9;
    if (!strcmp(dir, "stop")) {
        wlwin_fill(w, cx - 7, cy - 7, 14, 14, rgb);
        return;
    }
    for (int i = 0; i < n; i++) {                               /* row i of the triangle is 2 i + 2 wide */
        if (!strcmp(dir, "forward"))
            wlwin_fill(w, cx - i - 1, cy - n / 2 + i, 2 * i + 2, 1, rgb);
        else if (!strcmp(dir, "backward"))
            wlwin_fill(w, cx - i - 1, cy + n / 2 - i, 2 * i + 2, 1, rgb);
        else if (!strcmp(dir, "turn_left"))
            wlwin_fill(w, cx - n / 2 + i, cy - i - 1, 1, 2 * i + 2, rgb);
        else
            wlwin_fill(w, cx + n / 2 - i, cy - i - 1, 1, 2 * i + 2, rgb);
    }
}

static void paint_ctl(struct app *a, const struct ctl *c, bool live)
{
    struct wlwin *w = &a->win;
    struct tuya_dev *d = &a->dev[c->dev];
    struct tuya_dp *dp = c->dp >= 0 ? &d->dp[c->dp] : NULL;
    struct box b = c->b;
    bool passive = c->kind == C_DETAILS || c->kind == C_CLOSE;
    bool hot = live && in(a->px, a->py, b) && (d->online || passive);
    uint32_t fg = d->online ? lm_theme.text : lm_theme.text_dim;
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
        bool cur = dp->has && dp->type == TUYA_ENUM && !strcmp(dp->str, c->value);
        if (c->label[0]) {
            button(w, b, c->label, hot, cur ? lm_theme.button_down : lm_theme.button, fg);
        } else {                                                /* the stop key of the driving cross */
            rbox(w, b, 8, hot ? lm_theme.button_hover : lm_theme.button_alt);
            paint_arrow(w, b, c->value, d->online ? lm_theme.alert : lm_theme.text_dim);
        }
        break;
    }
    case C_HOLD: {
        bool down = a->holding && !strcmp(a->hold_dev, d->id) && dp->has && !strcmp(dp->str, c->value);
        rbox(w, b, 8, down ? lm_theme.accent : hot ? lm_theme.button_hover : lm_theme.button);
        paint_arrow(w, b, c->value, down ? lm_theme.ink : fg);
        break;
    }
    case C_PUSH: {
        bool on = dp->has && dp->b;
        rbox(w, b, LM_MAX(lm_button_radius(b.h / 2), 3), !d->online ? lm_theme.button : hot ? lm_theme.button_hover : on ? lm_theme.warn : lm_theme.ok);
        const char *label = on ? c->value : c->label;
        wlwin_text(w, b.x + (b.w - lm_font_text_width(label)) / 2, b.y + (b.h - LM_FONT_H) / 2, label, 1, d->online && !hot ? lm_theme.ink : fg, 0);
        break;
    }
    case C_DETAILS:
        button(w, b, c->label, hot, lm_theme.background, lm_theme.text_dim);
        break;
    case C_CLOSE:
        button(w, b, c->label, hot, lm_theme.button, lm_theme.text);
        break;
    case C_LABEL:
        wlwin_text(w, b.x, b.y, c->label, 1, lm_theme.accent, b.w);
        break;
    case C_EDIT: {
        bool hot_e = live && in(a->px, a->py, b);
        bool save = !strcmp(c->value, "save");
        button(w, b, c->label, hot_e, save ? lm_theme.ok : c->lit ? lm_theme.button_down : lm_theme.button, save && !hot_e ? lm_theme.ink : c->lit || hot_e || save ? lm_theme.text : lm_theme.text_dim);
        break;
    }
    case C_EDSLIDER: {
        int x0, x1;
        char v[48];
        struct tuya_dp shown = *dp;
        slider_track(b, &x0, &x1);
        shown.has = true;
        shown.num = a->ed.num;
        double f = dp->max > dp->min ? LM_CLAMP((a->ed.num - dp->min) / (dp->max - dp->min), 0.0, 1.0) : 0.0;
        int kx = x0 + (int)(f * (x1 - x0) + 0.5);
        wlwin_text(w, b.x, b.y + 7, c->label, 1, lm_theme.text, 142);
        rbox(w, (struct box){ x0, b.y + 12, x1 - x0, 6 }, 3, lm_theme.button);
        rbox(w, (struct box){ x0, b.y + 12, kx - x0, 6 }, 3, lm_theme.accent);
        rbox(w, (struct box){ kx - 8, b.y + 7, 16, 16 }, 8, lm_theme.on_colour);
        tuya_dp_text(&shown, v, sizeof(v));
        wlwin_text(w, x1 + 14, b.y + 7, v, 1, lm_theme.text, 66);
        break;
    }
    case C_TEXT:
        wlwin_text(w, b.x, b.y, c->label, 1, lm_theme.text_dim, b.w);
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

static void paint_devices(struct app *a)
{
    struct wlwin *w = &a->win;
    build_layout(a);
    clamp_scroll(a);
    build_layout(a);
    bool live = !a->modal_dev[0] && a->py >= HEAD_H && a->py < w->bh - FOOT_H;
    if (a->nscene > 0) {
        wlwin_text(w, PAD + 2, a->scene_box[0].y - 22, "Scenes", 1, lm_theme.accent, 0);
        for (int i = 0; i < a->nscene; i++)
            button(w, a->scene_box[i], a->scene[i].name, live && a->scene[i].enabled && in(a->px, a->py, a->scene_box[i]), lm_theme.button_alt,
                   a->scene[i].enabled ? lm_theme.text : lm_theme.text_dim);
    }
    if (!a->ndev)
        wlwin_text(w, 24, HEAD_H + (a->nscene ? 90 : 16), a->loaded ? "This account has no devices." : "Asking for the devices...", 1, lm_theme.text_dim, w->bw - 48);
    for (int i = 0; i < a->ndev; i++) {
        struct tuya_dev *d = &a->dev[i];
        struct box c = a->card[i];
        if (c.y + c.h < HEAD_H || c.y > w->bh - FOOT_H)
            continue;
        rbox(w, (struct box){ c.x - 1, c.y - 1, c.w + 2, c.h + 2 }, 9, lm_theme.border);
        rbox(w, c, 8, lm_theme.card);
        wlwin_text(w, c.x + 12, c.y + 10, d->name, 1, d->online ? lm_theme.text : lm_theme.text_dim, c.w - 172);
        if (d->online && lan_lookup(a, d->id, NULL))             /* heard in this network: its commands do not go round by the cloud */
            wlwin_text(w, c.x + c.w - 12 - 68 - 8 - lm_font_text_width("local"), c.y + 11, "local", 1, lm_theme.ok, 0);
        char info[300];
        info_text(d, info, sizeof(info));
        wlwin_text(w, c.x + 12, c.y + 30, info[0] ? info : d->product, 1, d->online ? lm_theme.text_dim : lm_theme.alert, c.w - 24);
    }
    for (int i = 0; i < a->nctl; i++) {
        const struct ctl *c = &a->ctl[i];
        if (!c->modal && c->b.y + c->b.h >= HEAD_H && c->b.y <= w->bh - FOOT_H)
            paint_ctl(a, c, live);
    }
}

/* (after the head and the foot were painted over the list's ends) */
static void paint_list_bar(struct app *a)
{
    sbar_paint(a, 1);
}

/* The open device's own view: the window dims, a card with the name, what it reports in a line, its battery, and every control. */
static void paint_modal(struct app *a)
{
    struct wlwin *w = &a->win;
    struct tuya_dev *d = find_dev(a, a->modal_dev);
    if (!d)
        return;
    wlwin_dim(w, 0.6);
    struct box m = a->modal;
    rbox(w, (struct box){ m.x - 1, m.y - 1, m.w + 2, m.h + 2 }, 11, lm_theme.accent);
    rbox(w, m, 10, lm_theme.card);
    wlwin_text(w, m.x + 16, m.y + 14, d->name, 1, d->online ? lm_theme.text : lm_theme.text_dim, m.w - 190);
    char info[300];
    info_text(d, info, sizeof(info));
    wlwin_text(w, m.x + 16, m.y + 36, info[0] ? info : d->product, 1, d->online ? lm_theme.text_dim : lm_theme.alert, m.w - 32);
    static const char *const cells[] = { "electricity_left", "battery_percentage", "residual_electricity", NULL };
    for (int i = 0; cells[i]; i++) {                            /* the device's own battery: a small cell filled to its level */
        struct tuya_dp *dp = tuya_find_dp(d, cells[i]);
        if (!dp || !dp->has || dp->type != TUYA_INT)
            continue;
        int level = (int)LM_CLAMP(dp->num, 0.0, 100.0), bx = m.x + m.w - 16 - 60 - 12 - 30, by = m.y + 18;
        uint32_t col = level > 40 ? lm_theme.ok : level > 15 ? lm_theme.warn : lm_theme.alert;
        wlwin_fill(w, bx, by, 26, 13, lm_theme.text_dim);
        wlwin_fill(w, bx + 26, by + 4, 2, 5, lm_theme.text_dim);
        wlwin_fill(w, bx + 1, by + 1, 24, 11, lm_theme.card);
        wlwin_fill(w, bx + 2, by + 2, LM_MAX(level * 22 / 100, level > 0 ? 1 : 0), 9, col);
        break;
    }
    wlwin_fill(w, m.x + 12, m.y + MODAL_HEAD - 6, m.w - 24, 1, lm_theme.border);
    for (int i = 0; i < a->nctl; i++)
        if (a->ctl[i].modal && !a->ctl[i].hidden)
            paint_ctl(a, &a->ctl[i], !a->sb_drag);
    sbar_paint(a, 2);
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
    }
    wlwin_fill(w, 10, HEAD_H - 8, w->bw - 20, 1, lm_theme.border);
    wlwin_fill(w, 10, w->bh - FOOT_H, w->bw - 20, 1, lm_theme.border);
    int foot_w = w->bw - 28 - (a->view == V_DEVICES ? (a->confirm_logout ? 176 : 92) : 0);
    if (a->view == V_DEVICES && a->confirm_logout) {
        char q[160];
        snprintf(q, sizeof(q), "Log out%s%s? The devices stay in the account.", a->username[0] ? " of " : "", a->username);
        wlwin_text(w, 14, w->bh - FOOT_H + 8, q, 1, lm_theme.text, foot_w);
        button(w, btn_logout(a), "Log out", in(a->px, a->py, btn_logout(a)), lm_theme.danger, lm_theme.on_colour);
        button(w, btn_account(a), "Cancel", in(a->px, a->py, btn_account(a)), lm_theme.button, lm_theme.text);
    } else {
        if (a->msg[0])
            wlwin_text(w, 14, w->bh - FOOT_H + 8, a->msg, 1, a->msg_err ? lm_theme.error : lm_theme.text, foot_w);
        else
            wlwin_text(w, 14, w->bh - FOOT_H + 8, a->view == V_DEVICES ? "F5 asks again   Esc closes" : "Esc closes", 1, lm_theme.text_dim, foot_w);
        if (a->view == V_DEVICES)
            button(w, btn_account(a), "Account", in(a->px, a->py, btn_account(a)), lm_theme.background, lm_theme.text_dim);
    }
    if (a->view == V_DEVICES)
        paint_list_bar(a);
    if (a->view == V_DEVICES && a->modal_dev[0])
        paint_modal(a);
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

/* Asks for the values again; `full` = the list of scenes too (the user asked). */
static void refresh(struct app *a, bool full)
{
    if (!a->poll_busy && push_job(a, J_POLL, full ? "scenes" : NULL, NULL, NULL)) {
        a->poll_busy = true;
        a->poll_at = mono_ms() + (uint64_t)a->poll_ms;
    }
    a->win.redraw = true;
}

/* The number the pointer stands at on a slider. */
static double slider_value(struct app *a, const struct ctl *c)
{
    struct tuya_dp *dp = &a->dev[c->dev].dp[c->dp];
    int x0, x1;
    slider_track(c->b, &x0, &x1);
    double f = LM_CLAMP((a->px - x0) / (double)(x1 - x0), 0.0, 1.0), step = dp->step > 0 ? dp->step : 1;
    double v = dp->min + f * (dp->max - dp->min);
    v = dp->min + (double)(long long)((v - dp->min) / step + 0.5) * step;
    return LM_CLAMP(v, dp->min, dp->max);
}

static void slider_set(struct app *a, const struct ctl *c)
{
    struct tuya_dp *dp = &a->dev[c->dev].dp[c->dp];
    dp->num = slider_value(a, c);
    dp->has = true;
}

static void rules_changed(struct app *a)
{
    if (!sched_save(a->rules, a->nrules))
        set_msg(a, true, "The timetable could not be saved");
    sched_spawn_background();                                   /* (it ends by itself when another one runs or no rule is on) */
}

/* A button of the timetable. */
static void press_edit(struct app *a, const struct ctl *c)
{
    struct tuya_dev *d = &a->dev[c->dev];
    const char *v = c->value;
    int n = atoi(strchr(v, ':') ? strchr(v, ':') + 1 : "0");
    if (!strcmp(v, "fav")) {
        char ids[FAV_MAX][FAV_ID_MAX];
        bool pinned = fav_find(ids, fav_load(ids, FAV_MAX), d->id) >= 0;
        if (!fav_set(d->id, !pinned))
            set_msg(a, true, "Only eight devices fit on the desktop card");
        return;
    }
    if (!strcmp(v, "add")) {
        a->ed.open = true;
        a->ed.hour = 7;
        a->ed.min = 30;
        a->ed.kind = 0;
        a->ed.offset = 0;
        a->ed.days = 0x7f;
        a->ed.action = 0;
        for (int i = 0; i < a->nacts; i++)                      /* a number starts at what the device has now */
            if (a->acts[i].is_int)
                a->ed.num = d->dp[a->acts[i].dp].num;
    } else if (!strncmp(v, "tog:", 4) && n >= 0 && n < a->nrules) {
        a->rules[n].on = !a->rules[n].on;
        rules_changed(a);
    } else if (!strncmp(v, "del:", 4) && n >= 0 && n < a->nrules) {
        memmove(&a->rules[n], &a->rules[n + 1], sizeof(a->rules[0]) * (size_t)(a->nrules - n - 1));
        a->nrules--;
        rules_changed(a);
    } else if (!strcmp(v, "h+")) {
        a->ed.hour = (a->ed.hour + 1) % 24;
    } else if (!strcmp(v, "h-")) {
        a->ed.hour = (a->ed.hour + 23) % 24;
    } else if (!strcmp(v, "m+")) {
        a->ed.min = (a->ed.min / 5 * 5 + 5) % 60;
    } else if (!strcmp(v, "m-")) {
        a->ed.min = ((a->ed.min + 4) / 5 * 5 + 55) % 60;
    } else if (!strncmp(v, "k:", 2) && n >= 0 && n <= 2) {
        a->ed.kind = n;
    } else if (!strncmp(v, "o:", 2)) {
        a->ed.offset = LM_CLAMP(a->ed.offset + n, -180, 180);
    } else if (!strcmp(v, "dw")) {
        a->ed.days = 0x1f;
    } else if (!strcmp(v, "da")) {
        a->ed.days = 0x7f;
    } else if (v[0] == 'd' && v[1] >= '0' && v[1] <= '6') {
        a->ed.days ^= 1u << (v[1] - '0');
    } else if (v[0] == 'a' && atoi(v + 1) < a->nacts) {
        a->ed.action = atoi(v + 1);
        if (a->acts[a->ed.action].is_int)
            a->ed.num = d->dp[a->acts[a->ed.action].dp].num;
    } else if (!strcmp(v, "cancel")) {
        a->ed.open = false;
    } else if (!strcmp(v, "save")) {
        const struct sact *act = &a->acts[LM_CLAMP(a->ed.action, 0, LM_MAX(a->nacts - 1, 0))];
        if (!a->nacts || !(a->ed.days & 0x7f)) {
            set_msg(a, true, "Choose at least one day");
            return;
        }
        if (a->nrules >= SCHED_MAX) {
            set_msg(a, true, "The timetable is full");
            return;
        }
        struct sched_rule *r = &a->rules[a->nrules++];
        memset(r, 0, sizeof(*r));
        r->on = true;
        r->hour = a->ed.hour;
        r->min = a->ed.min;
        r->sun = a->ed.kind;
        r->offset = a->ed.kind ? a->ed.offset : 0;
        r->days = a->ed.days & 0x7f;
        copy(r->dev, sizeof(r->dev), d->id);
        copy(r->code, sizeof(r->code), d->dp[act->dp].code);
        if (act->is_int)
            snprintf(r->value, sizeof(r->value), "%lld", (long long)(a->ed.num < 0 ? a->ed.num - 0.5 : a->ed.num + 0.5));
        else
            copy(r->value, sizeof(r->value), act->value);
        a->ed.open = false;
        rules_changed(a);
        set_msg(a, false, "Saved. It runs while this computer is on.");
    }
}

static const struct ctl *drag_ctl(struct app *a)
{
    for (int i = 0; i < a->nctl; i++) {
        const struct ctl *c = &a->ctl[i];
        if (c->kind == C_SLIDER && c->modal == (a->modal_dev[0] != 0) && !strcmp(a->dev[c->dev].id, a->drag_dev) &&
            !strcmp(a->dev[c->dev].dp[c->dp].code, a->drag_code))
            return c;
    }
    return NULL;
}

static void close_modal(struct app *a);

/* Driving: the direction goes out now, "stop" when the button or the key is let go. */
static void hold_start(struct app *a, struct tuya_dev *d, struct tuya_dp *dp, const char *value)
{
    if (a->holding && !strcmp(dp->str, value))
        return;
    copy(dp->str, sizeof(dp->str), value);
    send_value(a, d, dp);
    a->holding = true;
    copy(a->hold_dev, sizeof(a->hold_dev), d->id);
    copy(a->hold_code, sizeof(a->hold_code), dp->code);
}

static void hold_end(struct app *a)
{
    if (!a->holding)
        return;
    a->holding = false;
    struct tuya_dev *d = find_dev(a, a->hold_dev);
    struct tuya_dp *dp = d ? tuya_find_dp(d, a->hold_code) : NULL;
    if (dp && in_range(dp, "stop")) {
        copy(dp->str, sizeof(dp->str), "stop");
        send_value(a, d, dp);
    }
    a->win.redraw = true;
}

static void close_modal(struct app *a)
{
    hold_end(a);
    a->ed.open = false;
    a->ed_drag = false;
    a->modal_dev[0] = 0;
    a->modal_scroll = 0;
    a->dragging = false;
    a->win.redraw = true;
}

static void open_modal(struct app *a, const struct tuya_dev *d)
{
    copy(a->modal_dev, sizeof(a->modal_dev), d->id);
    a->ed.open = false;
    a->modal_scroll = 0;
    a->win.redraw = true;
}

/* A press on a control. */
static void press_ctl(struct app *a, const struct ctl *c)
{
    struct tuya_dev *d = &a->dev[c->dev];
    if (c->kind == C_DETAILS) {
        open_modal(a, d);
        return;
    }
    if (c->kind == C_CLOSE) {
        close_modal(a);
        return;
    }
    if (c->kind == C_LABEL || c->kind == C_TEXT)
        return;
    if (c->kind == C_EDIT) {
        press_edit(a, c);
        return;
    }
    if (c->kind == C_EDSLIDER) {
        a->ed_drag = true;
        a->ed.num = slider_value(a, c);
        return;
    }
    struct tuya_dp *dp = &d->dp[c->dp];
    if (!d->online) {
        set_msg(a, true, "%s is offline", d->name);
        return;
    }
    switch (c->kind) {
    case C_TOGGLE:
    case C_PUSH:
        dp->b = !(dp->has && dp->b);
        send_value(a, d, dp);
        break;
    case C_BUTTON:
        if (dp->type == TUYA_INT)
            dp->num = atof(c->value);
        else
            copy(dp->str, sizeof(dp->str), c->value);
        send_value(a, d, dp);
        break;
    case C_HOLD:
        hold_start(a, d, dp, c->value);
        break;
    case C_SLIDER:
        a->dragging = true;
        copy(a->drag_dev, sizeof(a->drag_dev), d->id);
        copy(a->drag_code, sizeof(a->drag_code), dp->code);
        slider_set(a, c);
        break;
    default:
        break;
    }
}

static void press_devices(struct app *a)
{
    struct wlwin *w = &a->win;
    build_layout(a);
    if (a->modal_dev[0]) {                                      /* the device view takes every click; one beside it closes it */
        if (!in(a->px, a->py, a->modal)) {
            close_modal(a);
            return;
        }
        if (sbar_press(a, 2))
            return;
        for (int i = 0; i < a->nctl; i++)
            if (a->ctl[i].modal && !a->ctl[i].hidden && in(a->px, a->py, a->ctl[i].b)) {
                press_ctl(a, &a->ctl[i]);
                return;
            }
        return;
    }
    if (a->confirm_logout) {                                    /* the question takes the next click: only its Log out button logs out */
        a->confirm_logout = false;
        if (in(a->px, a->py, btn_logout(a)))
            push_job(a, J_LOGOUT, NULL, NULL, NULL);
        return;
    }
    if (in(a->px, a->py, btn_refresh(a))) {
        refresh(a, true);
        return;
    }
    if (in(a->px, a->py, btn_account(a))) {
        a->confirm_logout = true;
        return;
    }
    if (sbar_press(a, 1))
        return;
    if (a->py < HEAD_H - 8 || a->py >= w->bh - FOOT_H)
        return;
    for (int i = 0; i < a->nscene; i++)
        if (in(a->px, a->py, a->scene_box[i])) {
            if (!a->scene[i].enabled)
                set_msg(a, true, "%s is switched off in the app", a->scene[i].name);
            else if (push_job(a, J_SCENE, a->scene[i].home, a->scene[i].id, a->scene[i].name))
                set_msg(a, false, "Starting %s...", a->scene[i].name);
            return;
        }
    for (int i = 0; i < a->nctl; i++)
        if (!a->ctl[i].modal && in(a->px, a->py, a->ctl[i].b)) {
            press_ctl(a, &a->ctl[i]);
            return;
        }
    for (int i = 0; i < a->ndev; i++)                           /* the card itself: its view */
        if (in(a->px, a->py, a->card[i])) {
            open_modal(a, &a->dev[i]);
            return;
        }
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
        sbar_drag(a);
        for (int i = 0; a->ed_drag && i < a->nctl; i++)
            if (a->ctl[i].kind == C_EDSLIDER)
                a->ed.num = slider_value(a, &a->ctl[i]);
        w->redraw = true;
        return;
    }
    if (op == LM_WL_POINTER_EVT_LEAVE) {                        /* nothing stays lit when the pointer is gone */
        a->px = a->py = -1;
        w->redraw = true;
        return;
    }
    if (op == LM_WL_POINTER_EVT_AXIS && args[1].u == 0) {
        double v = lm_fixed_to_double(args[2].f);
        int *scroll = a->modal_dev[0] ? &a->modal_scroll : &a->scroll;
        *scroll += v > 0 ? 48 : v < 0 ? -48 : 0;
        w->redraw = true;
        return;
    }
    if (op != LM_WL_POINTER_EVT_BUTTON || args[2].u != 272)
        return;
    w->redraw = true;
    if (args[3].u == 0) {                                       /* let go: a dragged slider is sent now, a held direction stops */
        if (a->dragging) {
            struct tuya_dev *d = find_dev(a, a->drag_dev);
            struct tuya_dp *dp = d ? tuya_find_dp(d, a->drag_code) : NULL;
            a->dragging = false;
            if (dp)
                send_value(a, d, dp);
        }
        hold_end(a);
        a->ed_drag = false;
        a->sb_drag = 0;
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

/* An arrow key in the open view of a device that can be driven: the direction it stands for, else NULL. */
static const char *drive_key(struct app *a, uint32_t ks, struct tuya_dev **dev, struct tuya_dp **dpp)
{
    const char *v = ks == LM_KS_Up ? "forward" : ks == LM_KS_Down ? "backward" : ks == LM_KS_Left ? "turn_left" : ks == LM_KS_Right ? "turn_right" : NULL;
    struct tuya_dev *d = a->modal_dev[0] ? find_dev(a, a->modal_dev) : NULL;
    struct tuya_dp *dp = d ? tuya_find_dp(d, "direction_control") : NULL;
    if (!v || !dp || !d->online || !controllable(dp) || !in_range(dp, v))
        return NULL;
    *dev = d;
    *dpp = dp;
    return v;
}

static void keyboard_event(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
    struct app *a = w->data;
    if (op != LM_WL_KEYBOARD_EVT_KEY)
        return;
    uint32_t ks = lm_keymap_keysym(args[2].u, w->mods);
    struct tuya_dev *d;
    struct tuya_dp *dp;
    if (args[3].u == 0) {                                       /* a key let go: the robot stops */
        if (a->view == V_DEVICES && (ks == LM_KS_Up || ks == LM_KS_Down || ks == LM_KS_Left || ks == LM_KS_Right))
            hold_end(a);
        return;
    }
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
    int *scroll = a->modal_dev[0] ? &a->modal_scroll : &a->scroll;
    const char *drive = drive_key(a, ks, &d, &dp);
    if (drive)
        hold_start(a, d, dp, drive);
    else if (ks == LM_KS_Escape && a->confirm_logout)
        a->confirm_logout = false;
    else if (ks == LM_KS_Escape && a->modal_dev[0])
        close_modal(a);
    else if (ks == LM_KS_Escape)
        w->running = false;
    else if (ks == LM_KS_F1 + 4 || ks == 'r' || ks == 'R')
        refresh(a, true);
    else if (ks == LM_KS_Down)
        *scroll += 48;
    else if (ks == LM_KS_Up)
        *scroll -= 48;
    else if (ks == LM_KS_Page_Down || ks == ' ')
        *scroll += page;
    else if (ks == LM_KS_Page_Up)
        *scroll -= page;
    else if (ks == LM_KS_Home)
        *scroll = 0;
    else if (ks == LM_KS_End)
        *scroll = a->modal_dev[0] ? a->modal_h : a->content_h;
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
    if (r.have_scenes && a->view == V_DEVICES) {
        memcpy(a->scene, r.scenes, sizeof(a->scene));
        a->nscene = r.nscenes;
        a->win.redraw = true;
    }
    if (r.have_devs) {
        if (!a->dragging && a->view == V_DEVICES) {
            a->ndev = 0;
            for (int pass = 0; pass < 2; pass++)                /* the ones that answer first, the offline ones after them */
                for (int i = 0; i < r.ndevs; i++)
                    if (r.devs[i].online == (pass == 0))
                        a->dev[a->ndev++] = r.devs[i];
            a->loaded = true;
            lan_values_apply(a);
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
        close_modal(a);
        a->confirm_logout = false;
        a->nscene = 0;
        a->view = V_LOGIN;
        a->ndev = 0;
        a->loaded = false;
        a->username[0] = 0;
        a->scroll = 0;
        set_msg(a, false, "Logged out");
    }
}

static volatile sig_atomic_t g_open_req;                        /* SIGUSR1: another start asked this window for a device's view (see open_request_file) */

static void on_usr1(int sig)
{
    (void)sig;
    g_open_req = 1;
}

static void open_request_path(char *out, size_t n)
{
    const char *rt = getenv("XDG_RUNTIME_DIR"), *disp = getenv("WAYLAND_DISPLAY");
    snprintf(out, n, "%s/mellagui-smart-%s.open", rt && *rt ? rt : "/tmp", disp && *disp && !strchr(disp, '/') ? disp : "wayland-0");
}

static void tick(struct app *a)
{
    uint64_t now = mono_ms();
    if (g_open_req) {                                           /* `mellagui-smart --device ID` found this window running and left the id for it */
        g_open_req = 0;
        char path[320];
        open_request_path(path, sizeof(path));
        FILE *f = fopen(path, "r");
        if (f) {
            if (fgets(a->want_dev, sizeof(a->want_dev), f))
                a->want_dev[strcspn(a->want_dev, "\r\n")] = 0;
            fclose(f);
            unlink(path);
        }
    }
    if (a->want_dev[0] && a->view == V_DEVICES && a->loaded) {  /* the devices are known: open the one that was asked for */
        struct tuya_dev *d = find_dev(a, a->want_dev);
        a->want_dev[0] = 0;
        if (d)
            open_modal(a, d);
    }
    apply_results(a);
    pthread_mutex_lock(&a->lan_lock);
    if (a->lan_changed) {
        a->lan_changed = false;
        a->win.redraw = true;
    }
    bool said = a->read_changed;
    pthread_mutex_unlock(&a->lan_lock);
    if (said && !a->dragging && a->view == V_DEVICES && a->loaded) {
        lan_values_apply(a);
        apply_holds(a);
        a->win.redraw = true;
    }
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
        refresh(a, false);
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
        if (!tuya_load_specs(&s, d, err, sizeof(err)) || !tuya_load_local_map(&s, d, err, sizeof(err)))
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
            printf("%s", dp->nrange ? ")" : "");
            if (dp->lan_id)
                printf("  [local %d%s]", dp->lan_id, dp->lan_ok ? "" : ", cloud only");
            printf("\n");
        }
    }
    printf("%d device%s\n", n, n == 1 ? "" : "s");
    return 0;
}

/* The pinned devices with what the desktop card needs, one tab separated line each:
 *   fav ID NAME ONLINE KIND CODE VALUE     KIND switch (VALUE 0 / 1), blind (VALUE the position in per cent, -1 unknown; CODE control) or none */
static int cmd_favourites(void)
{
    static struct tuya_session s;
    static struct tuya_dev dev[TUYA_MAX_DEV];
    char ids[FAV_MAX][FAV_ID_MAX], err[200] = "";
    int nf = fav_load(ids, FAV_MAX), n = 0;
    if (!nf) {
        printf("favs\t0\n");
        return 0;
    }
    if (need_login(&s))
        return 1;
    if (!tuya_load_devices(&s, dev, &n, err, sizeof(err))) {
        fprintf(stderr, "mellagui-smart: %s\n", err);
        return 1;
    }
    printf("favs\t%d\n", nf);
    for (int f = 0; f < nf; f++)
        for (int i = 0; i < n; i++) {
            struct tuya_dev *d = &dev[i];
            if (strcmp(d->id, ids[f]))
                continue;
            const char *kind = "none", *code = "-";
            int value = 0;
            struct tuya_dp *ctl = tuya_find_dp(d, "control"), *pos = tuya_find_dp(d, "percent_control"), *state = tuya_find_dp(d, "percent_state");
            char extra[96] = "";
            int go = -1;
            if (!strcmp(d->category, "sd") || !strcmp(d->category, "cl"))   /* what can be set (and the modes, the blind's position) comes with the specifications: one more call, only for vacuums and blinds */
                tuya_load_specs(&s, d, err, sizeof(err));
            for (const char *const *g = (const char *const[]){ "power_go", "switch_go", NULL }; *g && go < 0; g++) {
                int k = dp_index(d, *g);
                if (k >= 0 && controllable(&d->dp[k]) && d->dp[k].type == TUYA_BOOL)
                    go = k;
            }
            if (!strcmp(d->category, "cl") && ctl) {
                kind = "blind";
                code = "control";
                /* percent_control is what was asked for (the cloud keeps 0 there), percent_state is where the blind is */
                value = state && state->has ? (int)(state->num + 0.5) : pos && pos->has ? (int)(pos->num + 0.5) : -1;
                snprintf(extra, sizeof(extra), "\t%d", pos && controllable(pos) ? 1 : 0);          /* a position can be set: the desktop card shows a slider */
            } else if (!strcmp(d->category, "sd") && go >= 0) {            /* a robot vacuum: its own buttons on the card */
                struct tuya_dp *bat = tuya_find_dp(d, "electricity_left");
                if (!bat || !bat->has)
                    bat = tuya_find_dp(d, "battery_percentage");
                const char *dc = "", *dv = "";
                int k;
                if ((k = dp_index(d, "switch_charge")) >= 0 && controllable(&d->dp[k]) && d->dp[k].type == TUYA_BOOL) {
                    dc = "switch_charge";
                    dv = "true";
                } else if ((k = dp_index(d, "mode")) >= 0 && controllable(&d->dp[k]) && in_range(&d->dp[k], "chargego")) {
                    dc = "mode";
                    dv = "chargego";
                }
                kind = "vacuum";
                code = d->dp[go].code;
                value = d->dp[go].has && d->dp[go].b ? 1 : 0;
                snprintf(extra, sizeof(extra), "\t%d,%s,%s", bat && bat->has ? (int)(bat->num + 0.5) : -1, dc, dv);
            } else {
                int sw[TUYA_MAX_DP], nsw = 0;                                  /* every switch of the device: a power strip is a row for each socket */
                for (int k = 0; k < d->ndp && nsw < TUYA_MAX_DP; k++) {
                    struct tuya_dp *dp = &d->dp[k];
                    if (dp->type == TUYA_BOOL && !strncmp(dp->code, "switch", 6) && strcmp(dp->code, "switch_inching") &&
                        strcmp(dp->code, "switch_backlight") && !is_countdown(dp))
                        sw[nsw++] = k;
                }
                for (int q = 0; q < nsw; q++) {
                    struct tuya_dp *dp = &d->dp[sw[q]];
                    char lab[40] = "";
                    if (nsw > 1)                                               /* the card shows the device's name once and one row for each: "Switch 2", "USB 1" */
                        pretty(dp->code, lab, sizeof(lab));
                    printf("fav\t%s\t%s\t%d\tswitch\t%s\t%d%s%s\n", d->id, d->name, d->online ? 1 : 0, dp->code, dp->has && dp->b ? 1 : 0, nsw > 1 ? "\t" : "", lab);
                }
                if (nsw)
                    continue;
            }
            printf("fav\t%s\t%s\t%d\t%s\t%s\t%d%s\n", d->id, d->name, d->online ? 1 : 0, kind, code, value, extra);
        }
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

/* The device of the account with what the cloud knows about it, and where it is in this network. Messages on stderr. */
static bool local_device(const char *id, struct tuya_dev **dout, struct lanent *e)
{
    static struct tuya_session s;
    static struct tuya_dev dev[TUYA_MAX_DEV];
    static struct tuyalan_found found[MAX_LAN];
    struct tuya_dev *d = NULL;
    int n = 0;
    char err[200] = "";
    memset(e, 0, sizeof(*e));
    if (need_login(&s))
        return false;
    if (!tuya_load_devices(&s, dev, &n, err, sizeof(err))) {
        fprintf(stderr, "mellagui-smart: %s\n", err);
        return false;
    }
    for (int i = 0; i < n; i++)
        if (!strcmp(dev[i].id, id))
            d = &dev[i];
    if (!d) {
        fprintf(stderr, "mellagui-smart: the account has no device %s\n", id);
        return false;
    }
    if (!tuya_load_specs(&s, d, err, sizeof(err)) || !tuya_load_local_map(&s, d, err, sizeof(err))) {
        fprintf(stderr, "mellagui-smart: %s\n", err);
        return false;
    }
    const char *fake = getenv("MELLAGUI_TUYA_LAN_FAKE");        /* "id=address=version" (tests) */
    char fid[40], fip[48], fver[8];
    if (fake && sscanf(fake, "%39[^=]=%47[^=]=%7[^;]", fid, fip, fver) == 3 && !strcmp(fid, id)) {
        copy(e->ip, sizeof(e->ip), fip);
        copy(e->version, sizeof(e->version), fver);
    }
    for (int round = 0; round < 6 && !e->ip[0]; round++) {      /* a device says where it is every few seconds */
        int f = tuyalan_scan(2500, found, MAX_LAN, NULL, err, sizeof(err));
        if (f < 0) {
            fprintf(stderr, "mellagui-smart: %s\n", err);
            return false;
        }
        for (int i = 0; i < f; i++)
            if (!strcmp(found[i].id, id)) {
                copy(e->ip, sizeof(e->ip), found[i].ip);
                copy(e->version, sizeof(e->version), found[i].version);
            }
    }
    if (!e->ip[0]) {
        fprintf(stderr, "mellagui-smart: %s was not heard in this network\n", d->name);
        return false;
    }
    *dout = d;
    return true;
}

/* What the device says itself when it is asked in the local network, beside what the cloud says. */
static int cmd_get_local(const char *id)
{
    struct tuya_dev *d = NULL;
    struct lanent e;
    static struct tuya_dev before;
    static char json[2048];
    char err[200] = "", ask[400];
    if (!local_device(id, &d, &e))
        return 1;
    before = *d;
    tuya_local_question(d, ask, sizeof(ask));
    if (!tuyalan_get(e.ip, e.version, d->id, d->local_key, ask, json, sizeof(json), err, sizeof(err))) {
        fprintf(stderr, "mellagui-smart: %s\n", err);
        return 1;
    }
    int took = tuya_apply_local(d, json);
    printf("%s at %s (protocol %s) says %s\n", d->name, e.ip, e.version, json);
    for (int k = 0; k < d->ndp; k++) {
        struct tuya_dp *dp = &d->dp[k];
        char v[80], c[80];
        if (dp->lan_id <= 0)
            continue;
        tuya_dp_text(dp, v, sizeof(v));
        tuya_dp_text(&before.dp[k], c, sizeof(c));
        printf("    %3d  %-24s = %-16s%s%s%s\n", dp->lan_id, dp->code, v, dp->lan_ok ? "" : "  (from the cloud: it converts this value)",
               dp->lan_ok && c[0] && strcmp(v, c) ? "  the cloud says " : "", dp->lan_ok && c[0] && strcmp(v, c) ? c : "");
    }
    printf("%d value%s read locally\n", took, took == 1 ? "" : "s");
    return 0;
}

/* A command over the local network only, saying how it went (to try a device's local protocol). */
static int cmd_set_local(const char *id, const char *code, const char *value)
{
    struct tuya_dev *d = NULL;
    struct lanent e;
    char err[200] = "", json[120], *end;
    if (!local_device(id, &d, &e))
        return 1;
    strtod(value, &end);
    if (!strcmp(value, "true") || !strcmp(value, "false") || (*value && !*end))
        copy(json, sizeof(json), value);
    else
        json_quote(value, json, sizeof(json));
    int r = lan_send(&e, d, code, json, err, sizeof(err));
    if (r != 1) {
        fprintf(stderr, "mellagui-smart: %s%s\n", err, r < 0 ? " (it was sent)" : " (nothing was sent)");
        return 1;
    }
    printf("%s at %s (protocol %s) confirmed %s = %s\n", d->name, e.ip, e.version, code, json);
    return 0;
}

/* The scenes of the app as text, or one of them started (by its name or id): for a key binding. */
static int cmd_scene(const char *which)
{
    static struct tuya_session s;
    static struct tuya_scene scene[TUYA_MAX_SCENE];
    int n = 0;
    char err[200] = "";
    if (need_login(&s))
        return 1;
    if (!tuya_load_scenes(&s, scene, &n, err, sizeof(err))) {
        fprintf(stderr, "mellagui-smart: %s\n", err);
        return 1;
    }
    for (int i = 0; i < n; i++) {
        if (!which) {
            printf("%s  %s%s\n", scene[i].id, scene[i].name, scene[i].enabled ? "" : "  (off)");
        } else if (!strcasecmp(scene[i].name, which) || !strcmp(scene[i].id, which)) {
            if (!tuya_trigger_scene(&s, scene[i].home, scene[i].id, err, sizeof(err))) {
                fprintf(stderr, "mellagui-smart: %s\n", err);
                return 1;
            }
            printf("started %s\n", scene[i].name);
            return 0;
        }
    }
    if (which) {
        fprintf(stderr, "mellagui-smart: the account has no scene '%s' (--scenes lists them)\n", which);
        return 1;
    }
    printf("%d scene%s\n", n, n == 1 ? "" : "s");
    return 0;
}

/* --sun: today's sunrise and sunset where the timetable works them out. */
static int cmd_sun(void)
{
    double lat, lon;
    char name[64];
    int rise, set;
    if (!getenv("MELLAGUI_SMART_SUN") && !sun_place(&lat, &lon, name, sizeof(name))) {
        fprintf(stderr, "mellagui-smart: no place is known (no time zone table found): --place LAT LON\n");
        return 1;
    }
    if (getenv("MELLAGUI_SMART_SUN") && *getenv("MELLAGUI_SMART_SUN")) {
        lat = lon = 0;
        snprintf(name, sizeof(name), "MELLAGUI_SMART_SUN");
    }
    if (!sun_today(time(NULL), &rise, &set)) {
        printf("place %.4f %.4f (%s)\nthe sun does not rise and set today\n", lat, lon, name);
        return 0;
    }
    printf("place %.4f %.4f (%s)\nsunrise %02d:%02d\nsunset %02d:%02d\n", lat, lon, name, rise / 60, rise % 60, set / 60, set % 60);
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

/* The devices that announce themselves in this network, with the names the account has for them. */
static int cmd_scan(int seconds)
{
    static struct tuya_session s;
    static struct tuya_dev dev[TUYA_MAX_DEV];
    static struct tuyalan_found found[64];
    int n = 0;
    char err[200] = "";
    if (tuya_session_load(&s) && !tuya_load_devices(&s, dev, &n, err, sizeof(err)))
        n = 0;                                                  /* (no names then) */
    printf("Listening for %d seconds...\n", seconds);
    fflush(stdout);
    int stats[2];
    int f = tuyalan_scan(seconds * 1000, found, 64, stats, err, sizeof(err));
    if (f < 0) {
        fprintf(stderr, "mellagui-smart: %s\n", err);
        return 1;
    }
    for (int i = 0; i < f; i++) {
        const char *name = "(not in the account)";
        for (int k = 0; k < n; k++)
            if (!strcmp(dev[k].id, found[i].id))
                name = dev[k].name;
        printf("%-24s  %-15s  version %-4s  %s\n", found[i].id, found[i].ip, found[i].version, name);
    }
    for (int k = 0; k < n; k++) {
        bool heard = false;
        for (int i = 0; i < f; i++)
            heard = heard || !strcmp(dev[k].id, found[i].id);
        if (!heard)
            printf("%-24s  %-15s  %-12s  %s%s\n", dev[k].id, "-", "not heard", dev[k].name, dev[k].online ? "" : " (offline)");
    }
    printf("%d device%s heard (%d broadcast%s arrived, %d not understood)\n", f, f == 1 ? "" : "s", stats[0], stats[0] == 1 ? "" : "s", stats[1]);
    if (!stats[0])
        printf("Nothing arrived at all: is this computer in the same network as the devices (not a guest network), and does a firewall let UDP 6666 / 6667 in?\n");
    return 0;
}

static int selftest(void)
{
    int bad = tuya_selftest() + qr_selftest() + tuyalan_selftest() + sched_selftest() + sun_selftest();
#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "mellagui-smart selftest: " __VA_ARGS__); fputc('\n', stderr); bad++; } } while (0)
    char s[64];
    pretty("switch_1", s, sizeof(s));
    CHECK(!strcmp(s, "Switch 1"), "pretty: %s", s);
    pretty("switch_charge", s, sizeof(s));
    CHECK(!strcmp(s, "Return to dock"), "pretty: %s", s);
    /* a blind: three buttons and a slider; a plug: its switch (the rest is in its own view) */
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
    int kinds[9] = { 0 };
    for (int i = 0; i < a.nctl; i++)
        kinds[a.ctl[i].kind]++;
    CHECK(kinds[C_BUTTON] == 3 && kinds[C_SLIDER] == 0 && kinds[C_TOGGLE] == 1 && kinds[C_DETAILS] == 2, "cards: %d buttons %d sliders %d toggles %d details",
          kinds[C_BUTTON], kinds[C_SLIDER], kinds[C_TOGGLE], kinds[C_DETAILS]);
    for (int i = 0; i < a.nctl; i++)
        CHECK(a.ctl[i].b.x >= PAD && a.ctl[i].b.x + a.ctl[i].b.w <= a.win.bw - PAD, "control %d leaves its card", i);
    /* the plug's own view: its switch, the choice as three buttons, the countdown as four ("15 min" ... "2 h"; a range of a day is no slider), what it reports */
    copy(a.modal_dev, sizeof(a.modal_dev), "plug");
    build_layout(&a);
    memset(kinds, 0, sizeof(kinds));
    for (int i = 0; i < a.nctl; i++)
        if (a.ctl[i].modal) {
            kinds[a.ctl[i].kind]++;
            CHECK(a.ctl[i].b.x >= a.modal.x && a.ctl[i].b.x + a.ctl[i].b.w <= a.modal.x + a.modal.w, "control %d leaves the view", i);
        }
    CHECK(kinds[C_TOGGLE] == 1 && kinds[C_BUTTON] == 3 + 4 && kinds[C_SLIDER] == 0 && kinds[C_TEXT] == 1 && kinds[C_CLOSE] == 1, "view: %d toggles %d buttons %d sliders %d lines",
          kinds[C_TOGGLE], kinds[C_BUTTON], kinds[C_SLIDER], kinds[C_TEXT]);
    /* a robot: the big buttons, a button per mode, the driving cross */
    static struct tuya_dev robot;
    copy(robot.id, sizeof(robot.id), "robot");
    copy(robot.category, sizeof(robot.category), "sd");
    robot.online = true;
    robot.ndp = 4;
    robot.dp[0] = (struct tuya_dp){ .code = "power_go", .type = TUYA_BOOL, .func = true, .has = true };
    robot.dp[1] = (struct tuya_dp){ .code = "mode", .type = TUYA_ENUM, .func = true, .has = true, .str = "standby", .range = { "standby", "smart", "chargego" }, .nrange = 3 };
    robot.dp[2] = (struct tuya_dp){ .code = "direction_control", .type = TUYA_ENUM, .func = true, .has = true, .str = "stop",
                                    .range = { "forward", "backward", "turn_left", "turn_right", "stop" }, .nrange = 5 };
    robot.dp[3] = (struct tuya_dp){ .code = "electricity_left", .type = TUYA_INT, .has = true, .num = 80, .max = 100, .unit = "%" };
    devs[0] = robot;
    copy(a.modal_dev, sizeof(a.modal_dev), "robot");
    build_layout(&a);
    memset(kinds, 0, sizeof(kinds));
    for (int i = 0; i < a.nctl; i++)
        if (a.ctl[i].modal)
            kinds[a.ctl[i].kind]++;
    CHECK(kinds[C_PUSH] == 1 && kinds[C_HOLD] == 4 && kinds[C_BUTTON] == 1 + 3 + 1, "robot: %d push %d hold %d buttons (dock, three modes, stop)", kinds[C_PUSH], kinds[C_HOLD],
          kinds[C_BUTTON]);
    a.modal_dev[0] = 0;
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
    wlwin_hidpi();                      /* draws through the wlwin_* calls only: sharp on a scaled screen */
    lm_theme_load_env();
    signal(SIGPIPE, SIG_IGN);
    if (argc == 2 && !strcmp(argv[1], "--selftest"))
        return selftest();
    if (argc == 2 && !strcmp(argv[1], "--list"))
        return cmd_list();
    if (argc == 2 && !strcmp(argv[1], "--favourites"))
        return cmd_favourites();
    if (argc == 4 && !strcmp(argv[1], "--fav") && (!strcmp(argv[3], "on") || !strcmp(argv[3], "off")))
        return fav_set(argv[2], !strcmp(argv[3], "on")) ? 0 : 1;
    if (argc == 5 && !strcmp(argv[1], "--set"))
        return cmd_set(argv[2], argv[3], argv[4]);
    if (argc == 3 && !strcmp(argv[1], "--get-local"))
        return cmd_get_local(argv[2]);
    if (argc == 5 && !strcmp(argv[1], "--set-local"))
        return cmd_set_local(argv[2], argv[3], argv[4]);
    if (argc == 2 && !strcmp(argv[1], "--background"))
        return sched_background();
    if (argc == 2 && !strcmp(argv[1], "--sun"))
        return cmd_sun();
    if (argc == 3 && !strcmp(argv[1], "--place") && !strcmp(argv[2], "auto")) {
        double lat, lon;
        char name[64];
        const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
        char path[1024];
        snprintf(path, sizeof(path), xdg && *xdg ? "%s/mellagui/smart-place.conf" : "%s/.config/mellagui/smart-place.conf", xdg && *xdg ? xdg : home ? home : "");
        unlink(path);
        printf(sun_place(&lat, &lon, name, sizeof(name)) ? "place: %.4f %.4f (%s)\n" : "no place is known: --place LAT LON\n", lat, lon, name);
        return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "--place")) {
        char *e1, *e2;
        double lat = strtod(argv[2], &e1), lon = strtod(argv[3], &e2);
        if (*e1 || *e2 || !sun_place_save(lat, lon)) {
            fprintf(stderr, "mellagui-smart: --place LAT LON (degrees; north and east are positive, e.g. 59.437 24.754)\n");
            return 2;
        }
        return cmd_sun();
    }
    if (argc == 2 && !strcmp(argv[1], "--scenes"))
        return cmd_scene(NULL);
    if (argc == 3 && !strcmp(argv[1], "--scene"))
        return cmd_scene(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "--login"))
        return cmd_login(argv[2]);
    if (argc == 2 && !strcmp(argv[1], "--logout")) {
        static struct tuya_session s;
        tuya_session_load(&s);
        tuya_session_forget(&s);
        return 0;
    }
    if ((argc == 2 || argc == 3) && !strcmp(argv[1], "--scan"))
        return cmd_scan(argc == 3 && atoi(argv[2]) > 0 ? LM_MIN(atoi(argv[2]), 300) : 20);
    if (argc == 3 && !strcmp(argv[1], "--qr")) {
        struct qr q;
        if (!qr_encode(argv[2], &q, -1)) {
            fprintf(stderr, "mellagui-smart: the text is too long (213 bytes at most)\n");
            return 1;
        }
        print_qr(&q);
        return 0;
    }
    const char *open_id = NULL;
    if (argc == 3 && !strcmp(argv[1], "--device") && argv[2][0] && strlen(argv[2]) < 40) {         /* start with a device's own view open */
        open_id = argv[2];
        argc = 1;
    }
    if (argc > 1) {
        fprintf(stderr, "usage: mellagui-smart [--device ID] | --list | --favourites | --fav ID on|off | --set ID CODE VALUE | --set-local ID CODE VALUE | --get-local ID | --scan [SECONDS] | --scenes | --scene NAME | --background | --sun | --place LAT LON|auto | --login USERCODE | --logout | --qr TEXT | --selftest\n");
        return 2;
    }
    const char *rt = getenv("XDG_RUNTIME_DIR"), *disp = getenv("WAYLAND_DISPLAY");
    char lock[300];
    snprintf(lock, sizeof(lock), "%s/mellagui-smart-%s.lock", rt && *rt ? rt : "/tmp", disp && *disp && !strchr(disp, '/') ? disp : "wayland-0");
    int lfd = open(lock, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lfd >= 0 && flock(lfd, LOCK_EX | LOCK_NB) != 0) {
        if (open_id) {                                          /* already open: leave the id for it and tell it (SIGUSR1) */
            char path[320], pidtext[24] = "";
            open_request_path(path, sizeof(path));
            FILE *f = fopen(path, "w");
            if (f) {
                fprintf(f, "%s\n", open_id);
                fclose(f);
            }
            int pfd = open(lock, O_RDONLY | O_CLOEXEC);
            if (pfd >= 0) {
                ssize_t k = read(pfd, pidtext, sizeof(pidtext) - 1);
                pidtext[k > 0 ? k : 0] = 0;
                close(pfd);
            }
            if (atoi(pidtext) > 1)
                kill((pid_t)atoi(pidtext), SIGUSR1);
        }
        return 0;                                               /* already open */
    }
    if (lfd >= 0 && ftruncate(lfd, 0) == 0) {                   /* (the pid, for the next start that wants this window to show a device) */
        char pidtext[24];
        int len = snprintf(pidtext, sizeof(pidtext), "%d\n", (int)getpid());
        if (write(lfd, pidtext, (size_t)len) < 0)
            (void)lfd;
    }
    signal(SIGUSR1, on_usr1);
    static const struct wlwin_ops ops = { .paint = paint, .pointer = pointer_event, .keyboard = keyboard_event };
    static struct app a;
    a.dump = getenv("MELLAGUI_SMART_DUMP");
    if (open_id)
        copy(a.want_dev, sizeof(a.want_dev), open_id);
    const char *pm = getenv("MELLAGUI_SMART_POLL_MS");
    a.poll_ms = pm && atoi(pm) >= 100 ? atoi(pm) : 10000;
    a.dev = calloc(TUYA_MAX_DEV, sizeof(*a.dev));
    a.master = calloc(TUYA_MAX_DEV, sizeof(*a.master));
    a.read = calloc(TUYA_MAX_DEV, sizeof(*a.read));
    if (!a.dev || !a.master || !a.read)
        return 1;
    const char *rm = getenv("MELLAGUI_SMART_READ_MS");
    a.read_ms = rm && atoi(rm) >= 100 ? atoi(rm) : 5000;
    pthread_mutex_init(&a.lock, NULL);
    pthread_mutex_init(&a.lan_lock, NULL);
    pthread_mutex_init(&a.lan_io, NULL);
    pthread_cond_init(&a.cond, NULL);
    a.nrules = sched_load(a.rules, SCHED_MAX);
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
    const char *fake = getenv("MELLAGUI_TUYA_LAN_FAKE");        /* "id=address=version;..." instead of listening (tests) */
    if (fake) {
        char fid[40], fip[48], fver[8];
        int used = 0;
        while (sscanf(fake, "%39[^=]=%47[^=]=%7[^;]%n", fid, fip, fver, &used) == 3) {
            lan_note(&a, fid, fip, fver, UINT64_MAX);
            fake += used;
            if (*fake == ';')
                fake++;
        }
    } else if (pthread_create(&a.scan_thread, NULL, scanner, &a) != 0) {
        fprintf(stderr, "mellagui-smart: no thread to listen in the local network\n");
    }
    if (pthread_create(&a.read_thread, NULL, lan_reader, &a) != 0)
        fprintf(stderr, "mellagui-smart: no thread to ask the devices in the local network\n");
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
