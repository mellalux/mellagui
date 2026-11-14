/* mellagui-greeter - the login screen (docs/GREETER.md). A fullscreen window in a `mellagui --kiosk` compositor that talks to mellagui-greetd
 * over the unix socket $GREETD_SOCK (the line protocol is described at the top of mellagui-greetd.c): the users, a password field, the session to
 * start, suspend / restart / shut down. It never sees more than the typed password, which goes to the daemon (PAM) and is forgotten.
 *
 *   mellagui-greeter [--windowed] [--demo] [--selftest]
 *     --windowed   an ordinary window instead of full screen (trying it out)
 *     --demo       no daemon: made-up users, the password is "demo" (for a look at the screen)
 *
 * Keys: Enter logs in, Tab / Shift+Tab move between the users, the name, the password and the session, Left / Right choose a user or a session,
 * Esc clears, typing goes to the password. The mouse does the same. A second click on a power button makes it so ("Sure?").
 * $MELLAGUI_GREETER_DUMP=FILE writes the state and the places of everything after every paint (tests).
 */
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "font.h"
#include "keymap.h"
#include "metrics.h"
#include "protocol.h"
#include "theme.h"
#include "uikit.h"
#include "util.h"
#include "wlc.h"
#include "wlwin.h"

#define MAX_USERS 64
#define MAX_SESS 32
#define MAX_TILES 5
#define AVATAR 68
#define TILE_W 112
#define FIELD_W 300
#define FIELD_H 38
#define PW_MAX 250
#define NAME_MAX_LEN 60

struct guser { char name[64], display[64]; };
struct gsess { char id[64], name[64]; };
enum phase { PH_LOADING, PH_IDLE, PH_WAIT, PH_ANSWER, PH_STARTING, PH_DONE };
enum focus { F_USERS, F_NAME, F_PASS, F_SESS };

struct app {
    struct wlwin win;
    int sock;
    bool demo;
    char in[4096];
    size_t inn;
    int stage;                              /* the load: 0 users, 1 sessions, 2 last */
    struct guser users[MAX_USERS];
    int nusers;
    struct gsess sess[MAX_SESS];
    int nsess;
    int sel;                                /* a user, or nusers = "Other..." */
    int ofs;                                /* the first tile shown */
    int ssel;
    char last_user[64], last_sess[64];
    char name[NAME_MAX_LEN + 1];
    int namen;
    char pw[PW_MAX + 1];
    int pwn;
    char pending[PW_MAX + 1];               /* the password typed before the daemon asked for it */
    bool have_pending;
    char prompt[100];
    enum focus focus;
    enum phase phase;
    char msg[160];
    bool msg_err;
    int armed;                              /* a power button that asked "Sure?": 1 sleep, 2 restart, 3 shut down; 0 none */
    int64_t armed_ms;
    double px, py;
    uint32_t rep_code;
    int64_t rep_next;
    time_t shown_min;
    char host[64];
    int64_t splash_t0;                      /* the login went through: when the "Starting ..." card appeared (0 = no card) */
    bool demo_over;                         /* the demo has nothing more to show: the card offers the way back */
};

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- the daemon ------------------------------------------------------------------------------------------------------------------ */

static void send_line(struct app *a, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void send_line(struct app *a, const char *fmt, ...)
{
    if (a->sock < 0)
        return;
    char line[600];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n > (int)sizeof(line) - 2)
        n = (int)sizeof(line) - 2;
    line[n++] = '\n';
    for (int off = 0; off < n;) {
        ssize_t w = send(a->sock, line + off, (size_t)(n - off), MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            close(a->sock);
            a->sock = -1;
            return;
        }
        off += (int)w;
    }
    memset(line, 0, sizeof(line));
}

static void set_msg(struct app *a, bool err, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void set_msg(struct app *a, bool err, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(a->msg, sizeof(a->msg), fmt, ap);
    va_end(ap);
    a->msg_err = err;
    a->win.redraw = true;
}

static int64_t now_ms(void);

static void splash_begin(struct app *a)
{
    if (!a->splash_t0)
        a->splash_t0 = now_ms();
    a->demo_over = false;
}

static const char *current_user(const struct app *a)
{
    if (a->sel < a->nusers)
        return a->users[a->sel].name;
    return a->name;
}

static void pick_user_by_name(struct app *a, const char *name)
{
    for (int i = 0; i < a->nusers; i++)
        if (!strcmp(a->users[i].name, name)) {
            a->sel = i;
            return;
        }
}

static void fit_offset(struct app *a)
{
    int total = a->nusers + 1;
    if (a->sel < a->ofs)
        a->ofs = a->sel;
    if (a->sel >= a->ofs + MAX_TILES)
        a->ofs = a->sel - MAX_TILES + 1;
    if (a->ofs > total - MAX_TILES)
        a->ofs = total - MAX_TILES;
    if (a->ofs < 0)
        a->ofs = 0;
}

static void clear_secret(struct app *a)
{
    memset(a->pw, 0, sizeof(a->pw));
    memset(a->pending, 0, sizeof(a->pending));
    a->pwn = 0;
    a->have_pending = false;
}

static void begin_login(struct app *a)
{
    const char *u = current_user(a);
    if (!u[0]) {
        set_msg(a, true, "Who are you? Type your user name");
        a->focus = F_NAME;
        return;
    }
    if (a->phase != PH_IDLE)
        return;
    if (a->demo) {
        if (!strcmp(a->pw, "demo")) {
            a->phase = PH_DONE;
            splash_begin(a);
            set_msg(a, false, "(demo) Welcome, %s", u);
        } else {
            set_msg(a, true, "Login incorrect (the demo password is \"demo\")");
        }
        clear_secret(a);
        return;
    }
    snprintf(a->pending, sizeof(a->pending), "%s", a->pw);
    a->have_pending = true;
    memset(a->pw, 0, sizeof(a->pw));
    a->pwn = 0;
    a->phase = PH_WAIT;
    a->msg[0] = 0;
    send_line(a, "login %s", u);
}

static void answer_prompt(struct app *a)
{
    send_line(a, "answer %s", a->pw);
    clear_secret(a);
    a->phase = PH_WAIT;
    a->prompt[0] = 0;
}

static void handle_line(struct app *a, char *l)
{
    if (!strncmp(l, "user ", 5)) {
        char *tab = strchr(l + 5, '\t');
        if (a->nusers < MAX_USERS) {
            struct guser *u = &a->users[a->nusers++];
            if (tab) *tab = 0;
            snprintf(u->name, sizeof(u->name), "%s", l + 5);
            snprintf(u->display, sizeof(u->display), "%s", u->name);
            if (tab) {
                char *t2 = strchr(tab + 1, '\t');
                if (t2) *t2 = 0;
                if (tab[1])
                    snprintf(u->display, sizeof(u->display), "%s", tab + 1);
            }
        }
    } else if (!strncmp(l, "session ", 8)) {
        char *tab = strchr(l + 8, '\t');
        if (a->nsess < MAX_SESS) {
            struct gsess *s = &a->sess[a->nsess++];
            if (tab) *tab = 0;
            snprintf(s->id, sizeof(s->id), "%s", l + 8);
            snprintf(s->name, sizeof(s->name), "%s", s->id);
            if (tab) {
                char *t2 = strchr(tab + 1, '\t');
                if (t2) *t2 = 0;
                if (tab[1])
                    snprintf(s->name, sizeof(s->name), "%s", tab + 1);
            }
        }
    } else if (!strcmp(l, "end")) {
        a->stage++;
    } else if (!strncmp(l, "last ", 5)) {
        char *tab = strchr(l + 5, '\t');
        if (tab) {
            *tab = 0;
            snprintf(a->last_user, sizeof(a->last_user), "%s", l + 5);
            snprintf(a->last_sess, sizeof(a->last_sess), "%s", tab + 1);
        }
        a->stage = 3;
    } else if (!strcmp(l, "last -")) {
        a->stage = 3;
    } else if (!strncmp(l, "prompt ", 7)) {
        if (a->have_pending && !strncmp(l + 7, "secret", 6)) {                    /* the first question is the password we have */
            snprintf(a->pw, sizeof(a->pw), "%s", a->pending);
            memset(a->pending, 0, sizeof(a->pending));
            a->have_pending = false;
            answer_prompt(a);
        } else {
            const char *t = strchr(l + 7, ' ');
            snprintf(a->prompt, sizeof(a->prompt), "%s", t ? t + 1 : "");
            a->phase = PH_ANSWER;
            a->focus = F_PASS;
            clear_secret(a);
        }
    } else if (!strncmp(l, "info ", 5)) {
        set_msg(a, false, "%s", l + 5);
    } else if (!strncmp(l, "error ", 6)) {
        set_msg(a, true, "%s", l + 6);
    } else if (!strcmp(l, "ok")) {
        if (a->phase == PH_WAIT && a->nsess > 0) {
            send_line(a, "start %s", a->sess[a->ssel].id);
            a->phase = PH_STARTING;
            splash_begin(a);
            set_msg(a, false, "Starting %s ...", a->sess[a->ssel].name);
        }
    } else if (!strncmp(l, "fail", 4)) {
        a->phase = PH_IDLE;
        a->prompt[0] = 0;
        clear_secret(a);
        set_msg(a, true, "%s", l[4] == ' ' ? l + 5 : "Login incorrect");
        a->focus = F_PASS;
    } else if (!strcmp(l, "started")) {
        a->phase = PH_DONE;
        splash_begin(a);
        set_msg(a, false, "Starting ...");
    } else if (!strncmp(l, "err ", 4)) {
        set_msg(a, true, "%s", l + 4);
    }
    a->win.redraw = true;
    if (a->stage == 3 && a->phase == PH_LOADING) {                              /* everything is here */
        a->phase = PH_IDLE;
        if (a->last_user[0])
            pick_user_by_name(a, a->last_user);
        for (int i = 0; i < a->nsess; i++)
            if (!strcmp(a->sess[i].id, a->last_sess))
                a->ssel = i;
        if (a->nusers == 0)
            a->sel = 0, a->focus = F_NAME;
        else
            a->focus = F_PASS;
        fit_offset(a);
    }
}

static void net_poll(struct app *a)
{
    if (a->sock < 0)
        return;
    for (;;) {
        struct pollfd p = { .fd = a->sock, .events = POLLIN };
        if (poll(&p, 1, 0) <= 0)
            break;
        ssize_t k = recv(a->sock, a->in + a->inn, sizeof(a->in) - a->inn - 1, 0);
        if (k <= 0) {
            close(a->sock);
            a->sock = -1;
            if (a->phase != PH_DONE && a->phase != PH_STARTING) {
                a->phase = PH_IDLE;
                set_msg(a, true, "The login service went away");
            }
            return;
        }
        a->inn += (size_t)k;
        char *nl;
        while ((nl = memchr(a->in, '\n', a->inn))) {
            *nl = 0;
            handle_line(a, a->in);
            size_t used = (size_t)(nl - a->in) + 1;
            memmove(a->in, a->in + used, a->inn - used);
            a->inn -= used;
        }
        if (a->inn >= sizeof(a->in) - 1)
            a->inn = 0;
    }
}

static int connect_daemon(void)
{
    const char *path = getenv("GREETD_SOCK");
    if (!path || !*path)
        return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    if (fd < 0 || strlen(path) >= sizeof(sa.sun_path))
        return -1;
    strcpy(sa.sun_path, path);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* ---- layout ---------------------------------------------------------------------------------------------------------------------- */

struct layout {
    struct lm_rect tile[MAX_TILES + 1];     /* avatar circles, by position (tile 0 is a.ofs) */
    int ntiles;
    struct lm_rect prev, next;              /* paging the tiles */
    struct lm_rect name, pass, login;
    struct lm_rect sprev, snext, sname;
    struct lm_rect power[3];
    int cardx, cardy, cardw;
    int label_y, msg_y;
};

static bool in_rect(struct lm_rect r, double x, double y)
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static bool is_other(const struct app *a) { return a->sel >= a->nusers; }

static void do_layout(const struct app *a, struct layout *L)
{
    int bw = a->win.bw, bh = a->win.bh;
    memset(L, 0, sizeof(*L));
    int total = a->nusers + 1;
    int shown = LM_MIN(total, MAX_TILES);
    L->ntiles = shown;
    int rowx = (bw - shown * TILE_W) / 2;
    int cy = LM_MAX(bh * 36 / 100, 150);
    for (int i = 0; i < shown; i++)
        L->tile[i] = lm_rect_make(rowx + i * TILE_W + (TILE_W - AVATAR) / 2, cy, AVATAR, AVATAR);
    L->prev = lm_rect_make(rowx - 34, cy + AVATAR / 2 - 14, 28, 28);
    L->next = lm_rect_make(rowx + shown * TILE_W + 6, cy + AVATAR / 2 - 14, 28, 28);
    L->label_y = cy + AVATAR + 8;
    int y = L->label_y + LM_FONT_H + 22;
    int fx = (bw - FIELD_W) / 2;
    if (is_other(a)) {
        L->name = lm_rect_make(fx, y, FIELD_W, FIELD_H);
        y += FIELD_H + 10;
    }
    L->pass = lm_rect_make(fx, y, FIELD_W, FIELD_H);
    y += FIELD_H + 12;
    L->login = lm_rect_make((bw - 130) / 2, y, 130, 34);
    y += 34 + 12;
    L->msg_y = y;
    y += LM_FONT_H + 16;
    int sw = LM_MIN(lm_font_text_width(a->nsess ? a->sess[a->ssel].name : "-") + 16, 220);
    L->sname = lm_rect_make((bw - sw) / 2, y, sw, 26);
    L->sprev = lm_rect_make(L->sname.x - 30, y, 26, 26);
    L->snext = lm_rect_make(L->sname.x + sw + 4, y, 26, 26);
    int px = bw - 12;
    for (int i = 2; i >= 0; i--) {
        static const int widths[3] = { 76, 96, 110 };
        px -= widths[i];
        L->power[i] = lm_rect_make(px, bh - 44, widths[i], 30);
        px -= 8;
    }
}

/* ---- painting -------------------------------------------------------------------------------------------------------------------- */

static void text_c(struct wlwin *w, struct lm_rect r, const char *s, int scale, uint32_t col, bool bold)
{
    int tw = (bold ? lm_font_text_width_bold(s) : lm_font_text_width(s)) * scale;
    int x = r.x + (r.w - tw) / 2, y = r.y + (r.h - LM_FONT_H * scale) / 2;
    if (bold)
        wlwin_text_bold(w, x, y, s, scale, col, 0);
    else
        wlwin_text(w, x, y, s, scale, col, 0);
}

static uint32_t avatar_color(const char *name)
{
    uint32_t h = 2166136261u;
    for (const char *p = name; *p; p++)
        h = (h ^ (uint8_t)*p) * 16777619u;
    const uint32_t pal[] = { lm_theme.hue_1, lm_theme.hue_2, lm_theme.hue_3, lm_theme.hue_4, lm_theme.hue_5, lm_theme.hue_6, lm_theme.hue_7, lm_theme.hue_8 };
    return pal[h % 8];
}

static void initial_of(const char *s, char out[8])
{
    out[0] = 0;
    for (; *s == ' '; s++)
        ;
    if (!*s)
        return;
    int n = 1;
    if ((uint8_t)*s >= 0xc0)
        while (((uint8_t)s[n] & 0xc0) == 0x80 && n < 4)
            n++;
    memcpy(out, s, (size_t)n);
    out[n] = 0;
    if (n == 1 && out[0] >= 'a' && out[0] <= 'z')
        out[0] = (char)(out[0] - 32);
}

static void fit_text(const char *s, int maxw, char *out, size_t cap)
{
    snprintf(out, cap, "%s", s);
    if (lm_font_text_width(out) <= maxw)
        return;
    int k = lm_font_fit(out, maxw - lm_font_text_width("..."));
    if (k < 0)
        k = 0;
    if ((size_t)k + 4 > cap)
        k = (int)cap - 4;
    memcpy(out + k, "...", 4);
}

static void paint_field(struct app *a, struct lm_rect r, bool focused, const char *placeholder, const char *text, bool secret, int count)
{
    struct wlwin *w = &a->win;
    wlwin_round_fill(w, r.x - 1, r.y - 1, r.w + 2, r.h + 2, 11, focused ? lm_theme.accent : lm_mix_rgb(lm_theme.card, 0xffffff, 0.10));
    wlwin_round_fill(w, r.x, r.y, r.w, r.h, 10, lm_theme.card);
    if (count == 0 && !(text && text[0])) {
        wlwin_text(w, r.x + 14, r.y + (r.h - LM_FONT_H) / 2, placeholder, 1, lm_theme.text_dim, r.w - 28);
        if (focused)
            wlwin_fill(w, r.x + 12, r.y + 9, 2, r.h - 18, lm_theme.accent);
        return;
    }
    int x = r.x + 14;
    if (secret) {
        int shown = LM_MIN(count, (r.w - 40) / 14);
        for (int i = 0; i < shown; i++)
            wlwin_round_fill(w, x + i * 14, r.y + r.h / 2 - 4, 9, 9, 4, lm_theme.text);
        x += shown * 14;
    } else {
        char fit[128];
        fit_text(text, r.w - 40, fit, sizeof(fit));
        wlwin_text(w, x, r.y + (r.h - LM_FONT_H) / 2, fit, 1, lm_theme.text, 0);
        x += lm_font_text_width(fit);
    }
    if (focused)
        wlwin_fill(w, x + 2, r.y + 9, 2, r.h - 18, lm_theme.accent);
}

static void paint_arrow(struct wlwin *w, struct lm_rect r, bool left, bool hot, uint32_t col)
{
    if (hot)
        wlwin_round_fill(w, r.x, r.y, r.w, r.h, r.h / 2, lm_theme.button_hover);
    int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    for (int j = 0; j < 5; j++)                                                 /* a small triangle: the tip on the side it points to */
        wlwin_fill(w, left ? cx - 3 + j : cx + 3 - j, cy - j, 1, 2 * j + 1, col);
}

/* The card that says the login went through (and, in the demo, that nothing more will happen). */
static void splash_layout(const struct app *a, struct lm_rect *card, struct lm_rect *back)
{
    *card = lm_rect_make((a->win.bw - 400) / 2, (a->win.bh - 250) / 2, 400, 250);
    *back = lm_rect_make(card->x + (card->w - 230) / 2, card->y + card->h - 62, 230, 36);
}

static void paint_splash(struct app *a)
{
    struct wlwin *w = &a->win;
    wlwin_dim(w, 0.55);
    struct lm_rect card, back;
    splash_layout(a, &card, &back);
    wlwin_round_fill(w, card.x - 1, card.y - 1, card.w + 2, card.h + 2, 17, lm_mix_rgb(lm_theme.card, 0xffffff, 0.12));
    wlwin_round_fill(w, card.x, card.y, card.w, card.h, 16, lm_theme.card);
    const char *name = current_user(a), *disp = name;
    if (a->sel < a->nusers)
        disp = a->users[a->sel].display;
    int d = 76, ax = card.x + (card.w - d) / 2, ay = card.y + 22;
    wlwin_round_fill(w, ax, ay, d, d, d / 2, avatar_color(name));
    char ini[8];
    initial_of(disp, ini);
    text_c(w, lm_rect_make(ax, ay, d, d), ini, 2, lm_theme.on_colour, true);
    char line[200];
    snprintf(line, sizeof(line), "Welcome, %s", disp);
    char fit[200];
    fit_text(line, card.w - 40, fit, sizeof(fit));
    text_c(w, lm_rect_make(card.x, ay + d + 12, card.w, LM_FONT_H + 6), fit, 1, lm_theme.text_bright, true);
    if (a->demo && a->demo_over) {
        text_c(w, lm_rect_make(card.x + 16, ay + d + 40, card.w - 32, LM_FONT_H), "This was only the demo: no session was started.", 1, lm_theme.text_dim, false);
        bool hot = in_rect(back, a->px, a->py);
        wlwin_drop(w, back.x, back.y, back.w, back.h, lm_button_radius(back.h / 2), hot ? lm_theme.btn_blue_lit : lm_theme.btn_blue);
        text_c(w, back, "Back to the login screen", 1, lm_theme.on_colour, true);
        return;
    }
    snprintf(line, sizeof(line), "Starting %s ...", a->nsess ? a->sess[a->ssel].name : "the session");
    fit_text(line, card.w - 32, fit, sizeof(fit));
    text_c(w, lm_rect_make(card.x + 16, ay + d + 40, card.w - 32, LM_FONT_H), fit, 1, lm_theme.text_dim, false);
    int cx = card.x + card.w / 2, cy = card.y + card.h - 40, ph = (int)((now_ms() - a->splash_t0) / 90) % 12;      /* a ring of dots, one bright and a tail */
    for (int i = 0; i < 12; i++) {
        int age = (ph - i + 12) % 12;
        double ang = i * 3.14159265 / 6.0 - 3.14159265 / 2;
        int px = cx + (int)(16.0 * cos(ang)), py = cy + (int)(16.0 * sin(ang));
        uint32_t col = lm_mix_rgb(lm_theme.control, lm_theme.accent, age < 5 ? 1.0 - age * 0.2 : 0.0);
        wlwin_round_fill(w, px - 2, py - 2, 5, 5, 2, col);
    }
}

static void dump_state(struct app *a, const struct layout *L)
{
    const char *path = getenv("MELLAGUI_GREETER_DUMP");
    if (!path || !*path)
        return;
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return;
    static const char *const ph[] = { "loading", "idle", "wait", "answer", "starting", "done" };
    static const char *const fo[] = { "users", "name", "pass", "session" };
    fprintf(f, "phase=%s focus=%s user=%s sel=%d nusers=%d session=%s pw=%d name=%s err=%d armed=%d\n", ph[a->phase], fo[a->focus],
            current_user(a), a->sel, a->nusers, a->nsess ? a->sess[a->ssel].id : "-", a->pwn, a->name, a->msg_err ? 1 : 0, a->armed);
    fprintf(f, "msg %s\nprompt %s\n", a->msg, a->prompt);
    if (a->splash_t0) {
        struct lm_rect card, back;
        splash_layout(a, &card, &back);
        fprintf(f, "splash demo_over=%d\n", a->demo_over ? 1 : 0);
        fprintf(f, "rect back %d %d %d %d\n", back.x, back.y, back.w, back.h);
    }
    for (int i = 0; i < L->ntiles; i++) {
        int idx = a->ofs + i;
        fprintf(f, "tile %d %s %d %d %d %d\n", idx, idx < a->nusers ? a->users[idx].name : "(other)", L->tile[i].x, L->tile[i].y, L->tile[i].w, L->tile[i].h);
    }
#define R(name, r) fprintf(f, "rect %s %d %d %d %d\n", name, (r).x, (r).y, (r).w, (r).h)
    R("pass", L->pass); R("login", L->login); R("sprev", L->sprev); R("snext", L->snext);
    if (is_other(a))
        R("name", L->name);
    R("sleep", L->power[0]); R("reboot", L->power[1]); R("poweroff", L->power[2]);
    R("prev", L->prev); R("next", L->next);
#undef R
    fprintf(f, "ok\n");
    fclose(f);
    rename(tmp, path);
}

static void paint(struct wlwin *w)
{
    struct app *a = w->data;
    struct layout L;
    do_layout(a, &L);
    uint32_t top = lm_mix_rgb(lm_theme.background, lm_theme.accent, 0.10), bot = lm_theme.background;
    for (int y = 0; y < w->bh; y += 4)
        wlwin_fill(w, 0, y, w->bw, 4, lm_mix_rgb(top, bot, (double)y / LM_MAX(w->bh, 1)));
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char clock[16], date[64];
    snprintf(clock, sizeof(clock), "%02d:%02d", tm.tm_hour, tm.tm_min);
    strftime(date, sizeof(date), "%A, %d %B %Y", &tm);
    a->shown_min = now / 60;
    int ty = LM_MAX(w->bh * 8 / 100, 16);
    text_c(w, lm_rect_make(0, ty, w->bw, LM_FONT_H * 4), clock, 4, lm_theme.text_bright, true);
    text_c(w, lm_rect_make(0, ty + LM_FONT_H * 4 + 6, w->bw, LM_FONT_H + 4), date, 1, lm_theme.text_dim, false);

    /* the users */
    if (a->nusers + 1 > MAX_TILES) {
        paint_arrow(w, L.prev, true, in_rect(L.prev, a->px, a->py), a->ofs > 0 ? lm_theme.text : lm_theme.text_dim);
        paint_arrow(w, L.next, false, in_rect(L.next, a->px, a->py), a->ofs + MAX_TILES < a->nusers + 1 ? lm_theme.text : lm_theme.text_dim);
    }
    for (int i = 0; i < L.ntiles; i++) {
        int idx = a->ofs + i;
        bool other = idx >= a->nusers, chosen = idx == a->sel;
        struct lm_rect r = L.tile[i];
        const char *label = other ? "Other..." : a->users[idx].display;
        uint32_t col = other ? lm_theme.control : avatar_color(a->users[idx].name);
        if (chosen)
            wlwin_round_fill(w, r.x - 4, r.y - 4, r.w + 8, r.h + 8, (r.w + 8) / 2, lm_theme.accent);
        wlwin_round_fill(w, r.x, r.y, r.w, r.h, r.w / 2, col);
        char ini[8];
        if (other)
            snprintf(ini, sizeof(ini), "+");
        else
            initial_of(label, ini);
        text_c(w, r, ini, 2, lm_theme.on_colour, true);
        char fit[96];
        fit_text(label, TILE_W - 8, fit, sizeof(fit));
        text_c(w, lm_rect_make(r.x - (TILE_W - AVATAR) / 2, L.label_y, TILE_W, LM_FONT_H), fit, 1, chosen ? lm_theme.text_bright : lm_theme.text_dim, chosen);
    }
    if (a->focus == F_USERS && L.ntiles > 0) {                                  /* (the focus ring is the chosen tile's accent ring: a thin underline too) */
        int idx = a->sel - a->ofs;
        if (idx >= 0 && idx < L.ntiles)
            wlwin_fill(w, L.tile[idx].x + 8, L.label_y + LM_FONT_H + 4, AVATAR - 16, 2, lm_theme.accent);
    }
    bool busy = a->phase == PH_WAIT || a->phase == PH_STARTING || a->phase == PH_DONE || a->phase == PH_LOADING;
    if (is_other(a))
        paint_field(a, L.name, a->focus == F_NAME && !busy, "User name", a->name, false, a->namen);
    paint_field(a, L.pass, a->focus == F_PASS && !busy, a->prompt[0] ? a->prompt : "Password", NULL, true, a->pwn);
    /* the button */
    bool hot = in_rect(L.login, a->px, a->py) && !busy;
    wlwin_drop(w, L.login.x, L.login.y, L.login.w, L.login.h, lm_button_radius(L.login.h / 2), busy ? lm_theme.control_idle : hot ? lm_theme.btn_blue_lit : lm_theme.btn_blue);
    text_c(w, L.login, a->phase == PH_ANSWER ? "Answer" : busy ? "..." : "Log in", 1, busy ? lm_theme.title_text_idle : lm_theme.on_colour, true);
    if (a->msg[0]) {
        char fit[200];
        fit_text(a->msg, w->bw - 40, fit, sizeof(fit));
        text_c(w, lm_rect_make(0, L.msg_y, w->bw, LM_FONT_H + 4), fit, 1, a->msg_err ? lm_theme.error : lm_theme.text_dim, false);
    }
    /* the session */
    if (a->nsess > 0) {
        bool many = a->nsess > 1;
        wlwin_round_fill(w, L.sname.x, L.sname.y, L.sname.w, L.sname.h, 13, a->focus == F_SESS ? lm_mix_rgb(lm_theme.card, lm_theme.accent, 0.35) : lm_theme.card);
        char fit[96];
        fit_text(a->sess[a->ssel].name, L.sname.w - 12, fit, sizeof(fit));
        text_c(w, L.sname, fit, 1, lm_theme.text, false);
        if (many) {
            paint_arrow(w, L.sprev, true, in_rect(L.sprev, a->px, a->py), lm_theme.text);
            paint_arrow(w, L.snext, false, in_rect(L.snext, a->px, a->py), lm_theme.text);
        }
    }
    /* power and the host */
    static const char *const names[3] = { "Sleep", "Restart", "Shut down" };
    for (int i = 0; i < 3; i++) {
        bool armed = a->armed == i + 1;
        bool h = in_rect(L.power[i], a->px, a->py);
        uint32_t col = armed ? lm_theme.btn_red : h ? lm_theme.button_hover : lm_theme.button;
        wlwin_drop(w, L.power[i].x, L.power[i].y, L.power[i].w, L.power[i].h, lm_button_radius(L.power[i].h / 2), col);
        text_c(w, L.power[i], armed ? "Sure?" : names[i], 1, armed ? lm_theme.on_colour : lm_theme.text, false);
    }
    wlwin_text(w, 14, w->bh - 36, a->host, 1, lm_theme.text_dim, w->bw / 3);
    if (a->splash_t0)
        paint_splash(a);
    dump_state(a, &L);
}

/* ---- input ----------------------------------------------------------------------------------------------------------------------- */

static void select_user(struct app *a, int idx)
{
    idx = LM_CLAMP(idx, 0, a->nusers);
    if (idx != a->sel) {
        a->sel = idx;
        fit_offset(a);
        clear_secret(a);
        a->msg[0] = 0;
        if (is_other(a))
            a->focus = F_NAME;
        else if (a->focus == F_NAME)
            a->focus = F_PASS;
    }
    a->win.redraw = true;
}

static void cycle_focus(struct app *a, int dir)
{
    enum focus order[4];
    int n = 0;
    order[n++] = F_USERS;
    if (is_other(a))
        order[n++] = F_NAME;
    order[n++] = F_PASS;
    if (a->nsess > 1)
        order[n++] = F_SESS;
    int cur = 0;
    for (int i = 0; i < n; i++)
        if (order[i] == a->focus)
            cur = i;
    a->focus = order[(cur + dir + n) % n];
}

static void power_click(struct app *a, int which)               /* which: 1 sleep, 2 restart, 3 shut down */
{
    if (a->armed != which || now_ms() - a->armed_ms > 4000) {
        a->armed = which;
        a->armed_ms = now_ms();
        a->win.redraw = true;
        return;
    }
    a->armed = 0;
    static const char *const act[3] = { "suspend", "reboot", "poweroff" };
    if (a->demo)
        set_msg(a, false, "(demo) %s", act[which - 1]);
    else
        send_line(a, "power %s", act[which - 1]);
    a->win.redraw = true;
}

static void type_utf8(char *buf, int *n, int cap, const char *t, int tl)
{
    if (*n + tl > cap)
        return;
    memcpy(buf + *n, t, (size_t)tl);
    *n += tl;
    buf[*n] = 0;
}

static void backspace(char *buf, int *n)
{
    if (*n <= 0)
        return;
    int k = *n - 1;
    while (k > 0 && ((uint8_t)buf[k] & 0xc0) == 0x80)
        k--;
    memset(buf + k, 0, (size_t)(*n - k));
    *n = k;
}

static void splash_leave(struct app *a)                                    /* the demo's way back to the login screen */
{
    a->phase = PH_IDLE;
    a->splash_t0 = 0;
    a->demo_over = false;
    a->msg[0] = 0;
    a->focus = F_PASS;
    clear_secret(a);
    a->win.redraw = true;
}

static void handle_key(struct app *a, uint32_t ks)
{
    if (a->demo && a->demo_over) {
        if (ks == 0xff0d || ks == 0xff8d || ks == 0xff1b || ks == ' ')
            splash_leave(a);
        return;
    }
    bool shift = (a->win.mods & LM_MOD_SHIFT) != 0, ctrl = (a->win.mods & (LM_MOD_CTRL | LM_MOD_ALT | LM_MOD_SUPER)) != 0;
    bool busy = a->phase == PH_WAIT || a->phase == PH_STARTING || a->phase == PH_DONE || a->phase == PH_LOADING;
    a->armed = 0;
    if (busy)
        return;
    switch (ks) {
    case 0xff0d: case 0xff8d:                                               /* Return, KP_Enter */
        if (a->phase == PH_ANSWER)
            answer_prompt(a);
        else
            begin_login(a);
        return;
    case 0xff1b:                                                            /* Esc */
        if (a->phase == PH_ANSWER) {
            send_line(a, "cancel");
            a->phase = PH_IDLE;
            a->prompt[0] = 0;
        }
        clear_secret(a);
        a->msg[0] = 0;
        return;
    case 0xff09: case 0xfe20:                                               /* Tab, ISO_Left_Tab */
        cycle_focus(a, shift || ks == 0xfe20 ? -1 : 1);
        return;
    case 0xff51: case 0xff53: {                                             /* Left, Right */
        int d = ks == 0xff51 ? -1 : 1;
        if (a->focus == F_USERS)
            select_user(a, a->sel + d);
        else if (a->focus == F_SESS && a->nsess > 0)
            a->ssel = (a->ssel + d + a->nsess) % a->nsess;
        return;
    }
    case 0xff08:                                                            /* BackSpace */
        if (a->focus == F_NAME)
            backspace(a->name, &a->namen);
        else if (a->focus == F_PASS || a->focus == F_USERS)
            backspace(a->pw, &a->pwn);
        return;
    default:
        break;
    }
    char t[8];
    int tl = lm_keysym_to_utf8(ks, t);
    if (tl <= 0 || ctrl || (uint8_t)t[0] < 0x20 || t[0] == 0x7f)
        return;
    if (a->focus == F_SESS)
        return;
    if (a->focus == F_USERS)
        a->focus = is_other(a) && a->namen == 0 ? F_NAME : F_PASS;
    if (a->focus == F_NAME)
        type_utf8(a->name, &a->namen, NAME_MAX_LEN, t, tl);
    else
        type_utf8(a->pw, &a->pwn, PW_MAX, t, tl);
}

static void keyboard_event(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
    struct app *a = w->data;
    if (op != LM_WL_KEYBOARD_EVT_KEY)
        return;
    uint32_t code = args[2].u;
    if (args[3].u == 0) {
        if (a->rep_code == code)
            a->rep_code = 0;
        return;
    }
    uint32_t ks = lm_keymap_keysym(code, w->mods);
    handle_key(a, ks);
    w->redraw = true;
    if (ks == 0xff08) {                                                     /* only BackSpace repeats */
        a->rep_code = code;
        a->rep_next = now_ms() + 400;
    } else {
        a->rep_code = 0;
    }
}

static void pointer_event(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
    struct app *a = w->data;
    struct layout L;
    do_layout(a, &L);
    switch (op) {
    case LM_WL_POINTER_EVT_ENTER:
        a->px = lm_fixed_to_double(args[2].f);
        a->py = lm_fixed_to_double(args[3].f);
        w->redraw = true;
        break;
    case LM_WL_POINTER_EVT_MOTION:
        a->px = lm_fixed_to_double(args[1].f);
        a->py = lm_fixed_to_double(args[2].f);
        w->redraw = true;
        break;
    case LM_WL_POINTER_EVT_BUTTON: {
        if (args[2].u != 272 || args[3].u != 1)
            break;
        if (a->demo && a->demo_over) {
            struct lm_rect card, back;
            splash_layout(a, &card, &back);
            if (in_rect(back, a->px, a->py))
                splash_leave(a);
            return;
        }
        bool busy = a->phase == PH_WAIT || a->phase == PH_STARTING || a->phase == PH_DONE || a->phase == PH_LOADING;
        for (int i = 0; i < 3; i++)
            if (in_rect(L.power[i], a->px, a->py)) {
                power_click(a, i + 1);
                return;
            }
        a->armed = 0;
        w->redraw = true;
        if (busy)
            return;
        for (int i = 0; i < L.ntiles; i++)
            if (in_rect(L.tile[i], a->px, a->py) || (a->py >= L.label_y - 4 && a->py < L.label_y + LM_FONT_H + 6 && a->px >= L.tile[i].x - 20 && a->px < L.tile[i].x + AVATAR + 20)) {
                select_user(a, a->ofs + i);
                if (a->focus == F_USERS)
                    a->focus = is_other(a) ? F_NAME : F_PASS;
                return;
            }
        if (a->nusers + 1 > MAX_TILES) {
            if (in_rect(L.prev, a->px, a->py)) { a->ofs = LM_MAX(a->ofs - 1, 0); return; }
            if (in_rect(L.next, a->px, a->py)) { a->ofs = LM_MIN(a->ofs + 1, a->nusers + 1 - MAX_TILES); return; }
        }
        if (is_other(a) && in_rect(L.name, a->px, a->py)) { a->focus = F_NAME; return; }
        if (in_rect(L.pass, a->px, a->py)) { a->focus = F_PASS; return; }
        if (in_rect(L.login, a->px, a->py)) {
            if (a->phase == PH_ANSWER)
                answer_prompt(a);
            else
                begin_login(a);
            return;
        }
        if (a->nsess > 1) {
            if (in_rect(L.sprev, a->px, a->py)) { a->ssel = (a->ssel + a->nsess - 1) % a->nsess; a->focus = F_SESS; return; }
            if (in_rect(L.snext, a->px, a->py) || in_rect(L.sname, a->px, a->py)) { a->ssel = (a->ssel + 1) % a->nsess; a->focus = F_SESS; return; }
        }
        break;
    }
    }
}

static bool close_event(struct wlwin *w)
{
    (void)w;
    return false;                                                           /* the login screen cannot be closed */
}

/* ---- main ------------------------------------------------------------------------------------------------------------------------ */

static int g_bad;
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "selftest: line %d: ", __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); g_bad++; } } while (0)

static int selftest(void)
{
    static struct app a;
    a.sock = -1;
    a.win.bw = 800;
    a.win.bh = 600;
    char buf[32] = "";
    int n = 0;
    type_utf8(buf, &n, 8, "ab", 2);
    type_utf8(buf, &n, 8, "\xc3\xb5", 2);                                     /* o with tilde */
    CHECK(n == 4 && !strcmp(buf, "ab\xc3\xb5"), "typing '%s' %d", buf, n);
    backspace(buf, &n);
    CHECK(n == 2 && !strcmp(buf, "ab"), "a letter of two bytes goes whole: '%s' %d", buf, n);
    for (int i = 0; i < 7; i++) {
        struct guser *u = &a.users[a.nusers++];
        snprintf(u->name, sizeof(u->name), "user%d", i);
        snprintf(u->display, sizeof(u->display), "User %d", i);
    }
    a.sel = 6;
    fit_offset(&a);
    CHECK(a.ofs == 2, "the tiles follow the choice: ofs %d", a.ofs);
    struct layout L;
    do_layout(&a, &L);
    CHECK(L.ntiles == MAX_TILES, "tiles %d", L.ntiles);
    CHECK(L.pass.y > L.tile[0].y + AVATAR && L.login.y > L.pass.y && L.power[2].x + L.power[2].w <= a.win.bw, "layout order");
    char line[64];
    snprintf(line, sizeof(line), "prompt secret Password: ");
    a.phase = PH_WAIT;
    a.have_pending = true;
    snprintf(a.pending, sizeof(a.pending), "pw");
    handle_line(&a, line);
    CHECK(a.phase == PH_WAIT && !a.have_pending, "the typed password answers the first question");
    snprintf(line, sizeof(line), "fail Login incorrect");
    handle_line(&a, line);
    CHECK(a.phase == PH_IDLE && a.msg_err && !strcmp(a.msg, "Login incorrect"), "a failure comes back to the start");
    if (!g_bad)
        puts("selftest ok");
    return g_bad ? 1 : 0;
}

int main(int argc, char **argv)
{
    wlwin_hidpi();
    lm_theme_load_env();
    static struct app a;
    bool windowed = false;
    a.sock = -1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--selftest"))
            return selftest();
        else if (!strcmp(argv[i], "--windowed"))
            windowed = true;
        else if (!strcmp(argv[i], "--demo"))
            a.demo = true;
        else {
            fprintf(stderr, "usage: mellagui-greeter [--windowed] [--demo] [--selftest]\n");
            return 2;
        }
    }
    gethostname(a.host, sizeof(a.host) - 1);
    if (a.demo) {
        static const char *const names[][2] = { { "alice", "Alice Example" }, { "bob", "Bob" }, { "mella", "Mella" } };
        for (int i = 0; i < 3; i++) {
            snprintf(a.users[a.nusers].name, sizeof(a.users[0].name), "%s", names[i][0]);
            snprintf(a.users[a.nusers++].display, sizeof(a.users[0].display), "%s", names[i][1]);
        }
        snprintf(a.sess[0].id, sizeof(a.sess[0].id), "mellagui");
        snprintf(a.sess[0].name, sizeof(a.sess[0].name), "MellaGUI");
        snprintf(a.sess[1].id, sizeof(a.sess[1].id), "other");
        snprintf(a.sess[1].name, sizeof(a.sess[1].name), "Another session");
        a.nsess = 2;
        a.phase = PH_IDLE;
        a.focus = F_PASS;
    } else {
        a.sock = connect_daemon();
        if (a.sock < 0) {
            fprintf(stderr, "mellagui-greeter: no login daemon at $GREETD_SOCK (use --demo to look at the screen)\n");
            return 1;
        }
        a.phase = PH_LOADING;
        send_line(&a, "users");
        send_line(&a, "sessions");
        send_line(&a, "last");
    }
    static const struct wlwin_ops ops = { .paint = paint, .pointer = pointer_event, .keyboard = keyboard_event, .close = close_event };
    if (wlwin_connect(&a.win, "mellagui-greeter", &ops, &a) < 0)
        return 1;
    a.win.want_fullscreen = !windowed;
    if (wlwin_open(&a.win, "Login", "mellagui.greeter", 900, 640, 640, 480) < 0)
        return 1;
    while (a.win.running) {
        if (!wlwin_step(&a.win, 50))
            break;
        net_poll(&a);
        if (a.rep_code && now_ms() >= a.rep_next) {
            handle_key(&a, 0xff08);
            a.rep_next = now_ms() + 40;
            a.win.redraw = true;
        }
        if (a.armed && now_ms() - a.armed_ms > 4000) {
            a.armed = 0;
            a.win.redraw = true;
        }
        if (time(NULL) / 60 != a.shown_min)
            a.win.redraw = true;
        if (a.splash_t0) {                                              /* the card's ring turns; the demo has nothing more to do after a moment */
            a.win.redraw = true;
            if (a.demo && !a.demo_over && now_ms() - a.splash_t0 >= 2500)
                a.demo_over = true;
        }
        if (a.sock < 0 && !a.demo && a.phase != PH_DONE && a.phase != PH_STARTING && a.phase != PH_IDLE)
            break;
    }
    wlwin_destroy(&a.win);
    return 0;
}
