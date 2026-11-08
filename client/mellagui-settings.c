/*
 * mellagui-settings: the settings panel (wl_shm + xdg-shell + the private mellagui_settings_v1, bitmap font).
 *
 *   Keyboard   layouts (enable, order, active), sticky keys, key repeat delay / rate, a field to try the keyboard
 *   Pointer    pointer speed, natural scrolling
 *   Desktop    clipboard gadget, sound feedback
 *   Security   screen lock: password or PIN, lock after idle minutes, monitor standby while locked, lock at start,
 *              "Lock now" (src/lock.h)
 *
 * Mouse: click checkboxes / radio buttons / buttons, drag the sliders. Keyboard: Tab / Shift+Tab move the focus,
 * Enter or Space activates, Left/Right change a slider, Esc closes. Every change is sent to the compositor at once,
 * which applies it, saves it (see src/settings.h) and answers with the effective values - the window only ever
 * shows what the compositor reports.
 *
 *   mellagui-settings                  the panel
 *   mellagui-settings --get [KEY]      print KEY=VALUE for every setting (or just one)
 *   mellagui-settings --set K=V ...    change settings; prints the resulting values, exit status 1 if one was refused
 *   mellagui-settings --selftest       checks the list / slider helpers without a display
 */
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "theme.h"
#include "font.h"
#include "keymap.h"
#include "lock.h"
#include "util.h"
#include "wlwin.h"

/* ---- layout list helpers (pure) ---------------------------------------------------------------------------- */

struct idlist {
    char id[LM_KBD_MAX_LAYOUTS][8];
    int n;
};

static void idlist_parse(struct idlist *l, const char *s)
{
    l->n = 0;
    while (*s && l->n < LM_KBD_MAX_LAYOUTS) {
        size_t len = strcspn(s, ",");
        if (len > 0 && len < sizeof(l->id[0])) {
            memcpy(l->id[l->n], s, len);
            l->id[l->n][len] = 0;
            l->n++;
        }
        s += len + (s[len] ? 1 : 0);
    }
}

static void idlist_format(const struct idlist *l, char *out, size_t n)
{
    out[0] = 0;
    for (int i = 0; i < l->n; i++) {
        size_t len = strlen(out);
        snprintf(out + len, n - len, "%s%s", i ? "," : "", l->id[i]);
    }
}

static int idlist_find(const struct idlist *l, const char *id)
{
    for (int i = 0; i < l->n; i++)
        if (!strcmp(l->id[i], id))
            return i;
    return -1;
}

/* Enabling appends; disabling refuses to remove the last one. Returns false when nothing changed. */
static bool idlist_toggle(struct idlist *l, const char *id)
{
    int i = idlist_find(l, id);
    if (i < 0) {
        if (l->n >= LM_KBD_MAX_LAYOUTS)
            return false;
        snprintf(l->id[l->n++], sizeof(l->id[0]), "%s", id);
        return true;
    }
    if (l->n <= 1)
        return false;
    memmove(l->id[i], l->id[i + 1], sizeof(l->id[0]) * (size_t)(l->n - i - 1));
    l->n--;
    return true;
}

static bool idlist_move_up(struct idlist *l, const char *id)
{
    int i = idlist_find(l, id);
    if (i <= 0)
        return false;
    char tmp[sizeof(l->id[0])];
    memcpy(tmp, l->id[i - 1], sizeof(tmp));
    memcpy(l->id[i - 1], l->id[i], sizeof(tmp));
    memcpy(l->id[i], tmp, sizeof(tmp));
    return true;
}

/* Slider position -> value (snapped to `step`, clamped). */
static int slider_value(int lo, int hi, int step, double frac)
{
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    double raw = lo + frac * (hi - lo);
    int v = lo + (int)((raw - lo) / step + 0.5) * step;
    return LM_MAX(lo, LM_MIN(hi, v));
}

/* Entry of a "name|description;name|description" list; false at the end. */
static bool next_entry(const char **p, char *name, size_t nn, char *desc, size_t dn)
{
    if (!**p)
        return false;
    const char *end = strchr(*p, ';');
    size_t len = end ? (size_t)(end - *p) : strlen(*p);
    const char *bar = memchr(*p, '|', len);
    size_t nlen = bar ? (size_t)(bar - *p) : len;
    snprintf(name, nn, "%.*s", (int)nlen, *p);
    snprintf(desc, dn, "%.*s", bar ? (int)(len - nlen - 1) : 0, bar ? bar + 1 : "");
    *p = end ? end + 1 : *p + len;
    return true;
}

struct entry { char name[64], desc[96]; };

/*
 * The entries of a device list that fit `cap` rows, in list order. The first entry (the default) and the HDMI/DisplayPort
 * outputs are shown before the others, so that a long list of cards never hides the one the user is looking for.
 */
static int pick_entries(const char *list, int cap, struct entry *out)
{
    struct entry all[24];
    int n = 0;
    const char *p = list;
    while (n < 24 && next_entry(&p, all[n].name, sizeof(all[n].name), all[n].desc, sizeof(all[n].desc)))
        n++;
    bool show[24] = { false };
    int used = 0;
    for (int pass = 0; pass < 3; pass++)
        for (int i = 0; i < n && used < cap; i++) {
            bool hdmi = strstr(all[i].desc, "HDMI") || strstr(all[i].desc, "DisplayPort");
            if (!show[i] && (pass == 0 ? i == 0 : pass == 1 ? hdmi : true)) {
                show[i] = true;
                used++;
            }
        }
    int m = 0;
    for (int i = 0; i < n; i++)
        if (show[i])
            out[m++] = all[i];
    return m;
}

/* "1920x1080@60" + "preferred" -> "1920 x 1080   60 Hz   (preferred)" */
static void mode_label(const char *mode, const char *note, char *out, size_t n)
{
    int w = 0, h = 0, hz = 0;
    sscanf(mode, "%dx%d@%d", &w, &h, &hz);
    snprintf(out, n, "%d x %d   %d Hz%s%s%s", w, h, hz, *note ? "   (" : "", note, *note ? ")" : "");
}

/* What the compositor's `lock-status` means to the user. */
static const char *lock_status_text(const char *st, char *buf, size_t n)
{
    if (!strcmp(st, "ok"))
        return "Saved.";
    if (!strcmp(st, "wrong"))
        return "The current password / PIN is wrong.";
    if (!strncmp(st, "wait:", 5)) {
        snprintf(buf, n, "Too many wrong tries. Wait %s s.", st + 5);
        return buf;
    }
    if (!strcmp(st, "weak"))
        return "Password: 8 or more characters. PIN: 4 to 12 digits.";
    if (!strcmp(st, "invalid"))
        return "That combination is not possible.";
    if (!strcmp(st, "error"))
        return "Could not save the lock (see the log).";
    return "";
}

/* The `lock-set` value: "current<TAB>kind<TAB>new<TAB>minutes<TAB>start<TAB>blank<TAB>vt". kind is keep when no new secret is typed. */
static void lock_set_value(char *out, size_t n, const char *cur, const char *kind, const char *fresh, int minutes, bool start,
                           int blank, bool vt)
{
    snprintf(out, n, "%s\t%s\t%s\t%d\t%d\t%d\t%d", cur, kind, fresh, minutes, start ? 1 : 0, blank, vt ? 1 : 0);
}

/* Appends one typed character / removes the last one in a UTF-8 buffer of `cap` bytes. */
static void edit_text(char *buf, size_t cap, uint32_t ks, bool plain)
{
    size_t len = strlen(buf);
    char txt[8];
    if (ks == LM_KS_BackSpace) {
        while (len > 0 && (buf[len - 1] & 0xc0) == 0x80)
            len--;
        if (len > 0)
            len--;
        buf[len] = 0;
    } else if (plain && lm_keysym_to_utf8(ks, txt) > 0 && len + strlen(txt) < cap) {
        memcpy(buf + len, txt, strlen(txt) + 1);
    }
}

static int utf8_count(const char *s)
{
    int n = 0;
    for (; *s; s++)
        if (((unsigned char)*s & 0xc0) != 0x80)
            n++;
    return n;
}

static int selftest_fail;

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "selftest: %s:%d: %s\n", __FILE__, __LINE__, #cond); selftest_fail++; } } while (0)

static int selftest(void)
{
    struct idlist l;
    char buf[64];
    idlist_parse(&l, "us,ee");
    CHECK(l.n == 2 && !strcmp(l.id[1], "ee"));
    CHECK(idlist_toggle(&l, "de"));
    idlist_format(&l, buf, sizeof(buf));
    CHECK(!strcmp(buf, "us,ee,de"));
    CHECK(idlist_move_up(&l, "de"));
    idlist_format(&l, buf, sizeof(buf));
    CHECK(!strcmp(buf, "us,de,ee"));
    CHECK(!idlist_move_up(&l, "us"));               /* already first */
    CHECK(!idlist_move_up(&l, "fi"));               /* not enabled */
    CHECK(idlist_toggle(&l, "us"));                 /* disable */
    idlist_format(&l, buf, sizeof(buf));
    CHECK(!strcmp(buf, "de,ee"));
    CHECK(idlist_toggle(&l, "ee"));
    CHECK(!idlist_toggle(&l, "de"));                /* the last one stays */
    idlist_parse(&l, "us,ee,de,fi");
    CHECK(l.n == 4 && !idlist_toggle(&l, "xx"));    /* full */
    idlist_parse(&l, "");
    CHECK(l.n == 0);
    CHECK(slider_value(100, 1000, 50, 0.0) == 100);
    CHECK(slider_value(100, 1000, 50, 1.0) == 1000);
    CHECK(slider_value(100, 1000, 50, 0.5) == 550);
    CHECK(slider_value(100, 1000, 50, -3) == 100);
    CHECK(slider_value(100, 1000, 50, 7) == 1000);
    CHECK(slider_value(1, 60, 1, 0.5) == 31);
    {
        const char *p = "default|System default;plughw:CARD=PCH,DEV=3|HDA Intel PCH - HDMI 0 (HDMI);x|";
        char name[64], desc[96];
        CHECK(next_entry(&p, name, sizeof(name), desc, sizeof(desc)) && !strcmp(name, "default") && !strcmp(desc, "System default"));
        CHECK(next_entry(&p, name, sizeof(name), desc, sizeof(desc)) && !strcmp(name, "plughw:CARD=PCH,DEV=3") && strstr(desc, "HDMI"));
        CHECK(next_entry(&p, name, sizeof(name), desc, sizeof(desc)) && !strcmp(name, "x") && !*desc);
        CHECK(!next_entry(&p, name, sizeof(name), desc, sizeof(desc)));
        const char *e = "";
        CHECK(!next_entry(&e, name, sizeof(name), desc, sizeof(desc)));
    }
    {
        struct entry e[8];
        const char *many = "default|System default;a|USB Audio;b|PCH - HDMI 0 (HDMI);c|PCH - Analog;d|PCH - HDMI 1 (HDMI);e|Other";
        int n = pick_entries(many, 4, e);
        CHECK(n == 4 && !strcmp(e[0].name, "default") && !strcmp(e[1].name, "a") && !strcmp(e[2].name, "b") && !strcmp(e[3].name, "d"));      /* list order, HDMI kept */
        n = pick_entries(many, 3, e);
        CHECK(n == 3 && !strcmp(e[0].name, "default") && !strcmp(e[1].name, "b") && !strcmp(e[2].name, "d"));       /* HDMI before the rest */
        CHECK(pick_entries("", 4, e) == 0);
    }
    {
        char label[96];
        mode_label("1920x1080@60", "preferred", label, sizeof(label));
        CHECK(!strcmp(label, "1920 x 1080   60 Hz   (preferred)"));
        mode_label("2048x858@60", "", label, sizeof(label));
        CHECK(!strcmp(label, "2048 x 858   60 Hz"));
    }
    {
        char b[64], v[256], t[16] = "";
        CHECK(!strcmp(lock_status_text("ok", b, sizeof(b)), "Saved."));
        CHECK(strstr(lock_status_text("wait:25", b, sizeof(b)), "25 s"));
        CHECK(*lock_status_text("", b, sizeof(b)) == 0 && *lock_status_text("weird", b, sizeof(b)) == 0);
        lock_set_value(v, sizeof(v), "old", "password", "new pass", 10, true, 3, true);
        CHECK(!strcmp(v, "old\tpassword\tnew pass\t10\t1\t3\t1"));
        lock_set_value(v, sizeof(v), "", "keep", "", 0, false, 0, false);
        CHECK(!strcmp(v, "\tkeep\t\t0\t0\t0\t0"));
        edit_text(t, sizeof(t), 'a', true);
        edit_text(t, sizeof(t), 0xf5, true);                 /* o with tilde: two bytes */
        CHECK(utf8_count(t) == 2 && strlen(t) == 3);
        edit_text(t, sizeof(t), LM_KS_BackSpace, true);
        CHECK(!strcmp(t, "a"));
        edit_text(t, sizeof(t), 'b', false);                 /* not plain (Ctrl held): ignored */
        CHECK(!strcmp(t, "a"));
        for (int i = 0; i < 40; i++)
            edit_text(t, sizeof(t), 'x', true);
        CHECK(strlen(t) == sizeof(t) - 1);
    }
    if (selftest_fail)
        return 1;
    puts("mellagui-settings selftest ok");
    return 0;
}

/* ---- state -------------------------------------------------------------------------------------------------- */

#define COL_BG      (lm_theme.background)
#define COL_SIDE    (lm_theme.field)
#define COL_FIELD   (lm_theme.field)
#define COL_KEY     (lm_theme.button)
#define COL_HOVER   (lm_theme.button_hover)
#define COL_DOWN    (lm_theme.button_down)
#define COL_ACCENT  (lm_theme.accent)
#define COL_TEXT    (lm_theme.text)
#define COL_DIM     (lm_theme.text_dim)
#define DEFAULT_W 620
#define DEFAULT_H 440
#define SIDE_W 140
#define MAX_WIDGETS 80
#define MAX_KV 48
#define VALUE_MAX 1900          /* longest value (the lists of resolutions / sound outputs) */

enum page { PAGE_KEYBOARD, PAGE_POINTER, PAGE_DESKTOP, PAGE_DISPLAY, PAGE_SOUND, PAGE_SECURITY, PAGE_CONFIG, N_PAGES };
static const char *const PAGE_NAMES[N_PAGES] = { "Keyboard", "Pointer", "Desktop", "Display", "Sound", "Security", "Config" };

enum wkind { W_LABEL, W_HEADING, W_TAB, W_CHECK, W_RADIO, W_BUTTON, W_SLIDER, W_TEXT, W_PASS, W_FIELD };

struct rect { int x, y, w, h; };

struct widget {
    enum wkind kind;
    struct rect r;
    char text[64];
    char key[24];               /* setting key, or "layout-enable" / "layout-up" / "page" */
    char arg[64];               /* layout id / page number / mode / device name */
    bool on;                    /* check / radio state, selected tab */
    int lo, hi, step, val;      /* slider */
    const char *unit;           /* slider value suffix */
};

struct kv { char k[24], v[VALUE_MAX]; };

struct app {
    struct wlwin win;
    uint32_t settings;
    double px, py;
    bool has_pointer;
    struct kv kvs[MAX_KV];
    int nkv;
    enum page page;
    struct widget wd[MAX_WIDGETS];
    int nw;
    int hover, press, drag, focus;      /* widget indexes, -1 none */
    char test[64];                      /* text typed into the "try it" field */
    char wall[220];                     /* the wallpaper file field while it is being edited */
    bool wall_edited;                   /* (else the field shows the compositor's value) */
    /* Security page: a form that is sent in one piece by "Apply" (the compositor needs the current secret for any change) */
    char pw[3][LM_LOCK_MAX_SECRET + 1]; /* current / new / repeat */
    bool sec_pin;                       /* the new secret is a PIN (else a password) */
    int sec_minutes;                    /* idle minutes (0 = never) */
    int sec_blank;                      /* minutes locked before the monitors go to standby (0 = never) */
    bool sec_start;                     /* lock when MellaGUI starts */
    bool sec_vt;                        /* Ctrl+Alt+F1..F12 switches the console on the lock screen */
    char sec_err[64];                   /* a mistake found before sending */
};

static const char *kv_get(const struct app *a, const char *k)
{
    for (int i = 0; i < a->nkv; i++)
        if (!strcmp(a->kvs[i].k, k))
            return a->kvs[i].v;
    return "";
}

static int kv_int(const struct app *a, const char *k, int def)
{
    const char *v = kv_get(a, k);
    return *v ? atoi(v) : def;
}

static void kv_set(struct app *a, const char *k, const char *v)
{
    for (int i = 0; i < a->nkv; i++)
        if (!strcmp(a->kvs[i].k, k)) {
            snprintf(a->kvs[i].v, sizeof(a->kvs[i].v), "%s", v);
            return;
        }
    if (a->nkv < MAX_KV) {
        snprintf(a->kvs[a->nkv].k, sizeof(a->kvs[0].k), "%s", k);
        snprintf(a->kvs[a->nkv].v, sizeof(a->kvs[0].v), "%s", v);
        a->nkv++;
    }
}

static void send_set(struct app *a, const char *key, const char *value)
{
    if (a->settings)
        WLC_SEND(a->win.c, a->settings, LM_MELLAGUI_SETTINGS_V1_REQ_SET, { .s = key }, { .s = value });
}

static void send_set_int(struct app *a, const char *key, int v)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", v);
    send_set(a, key, buf);
}

/* ---- page layout ---------------------------------------------------------------------------------------------- */

#define ROW_H 28
#define BOX 16

static struct widget *add(struct app *a, enum wkind kind, struct rect r, const char *text)
{
    if (a->nw >= MAX_WIDGETS)
        return &a->wd[MAX_WIDGETS - 1];
    struct widget *w = &a->wd[a->nw++];
    memset(w, 0, sizeof(*w));
    w->kind = kind;
    w->r = r;
    snprintf(w->text, sizeof(w->text), "%s", text);
    return w;
}

static struct widget *add_check(struct app *a, int x, int y, int w, const char *text, const char *key, bool on)
{
    struct widget *wd = add(a, W_CHECK, (struct rect){ x, y, w, ROW_H }, text);
    snprintf(wd->key, sizeof(wd->key), "%s", key);
    wd->on = on;
    return wd;
}

static void add_slider(struct app *a, int x, int y, int w, const char *text, const char *key, int lo, int hi,
                       int step, const char *unit)
{
    struct widget *wd = add(a, W_SLIDER, (struct rect){ x, y, w, ROW_H }, text);
    snprintf(wd->key, sizeof(wd->key), "%s", key);
    wd->lo = lo;
    wd->hi = hi;
    wd->step = step;
    wd->unit = unit;
    wd->val = LM_MAX(lo, LM_MIN(hi, kv_int(a, key, lo)));
}

static void build_keyboard(struct app *a, int x, int y, int w)
{
    add(a, W_HEADING, (struct rect){ x, y, w, ROW_H }, "Keyboard layouts");
    y += ROW_H;
    struct idlist on;
    idlist_parse(&on, kv_get(a, "layouts"));
    const char *active = kv_get(a, "layout");
    /* enabled ones in switching order first, then the others */
    for (int pass = 0; pass < 2; pass++) {
        for (int j = 0; j < (pass == 0 ? on.n : lm_layout_count()); j++) {
            int i = pass == 0 ? lm_layout_find(on.id[j]) : j;
            if (i < 0)
                continue;
            const char *id = lm_layout_id(i);
            bool enabled = idlist_find(&on, id) >= 0;
            if (enabled != (pass == 0))
                continue;
            struct widget *c = add_check(a, x, y, LM_MIN(w - 150, 260), lm_layout_name(i), "layout-enable", enabled);
            snprintf(c->arg, sizeof(c->arg), "%s", id);
            if (enabled) {
                struct widget *r = add(a, W_RADIO, (struct rect){ x + w - 140, y, 90, ROW_H }, "active");
                snprintf(r->key, sizeof(r->key), "layout");
                snprintf(r->arg, sizeof(r->arg), "%s", id);
                r->on = !strcmp(active, id);
                if (idlist_find(&on, id) > 0) {
                    struct widget *u = add(a, W_BUTTON, (struct rect){ x + w - 40, y + 2, 32, ROW_H - 4 }, "^");
                    snprintf(u->key, sizeof(u->key), "layout-up");
                    snprintf(u->arg, sizeof(u->arg), "%s", id);
                }
            }
            y += ROW_H;
        }
    }
    add(a, W_LABEL, (struct rect){ x, y, w, ROW_H }, "Super+Space or the panel indicator switches layouts.");
    y += ROW_H + 8;
    add(a, W_HEADING, (struct rect){ x, y, w, ROW_H }, "Typing");
    y += ROW_H;
    add_check(a, x, y, w, "Sticky keys", "sticky", kv_int(a, "sticky", 0) != 0);
    y += ROW_H - 4;
    add(a, W_LABEL, (struct rect){ x + BOX + 10, y, w - BOX - 10, ROW_H - 4 },
        "Tap a modifier to latch it; Shift x5 toggles.");
    y += ROW_H;
    add_slider(a, x, y, w, "Repeat delay", "repeat-delay", 150, 1000, 50, " ms");
    y += ROW_H;
    add_slider(a, x, y, w, "Repeat rate", "repeat-rate", 1, 60, 1, "/s");
    y += ROW_H + 8;
    add(a, W_TEXT, (struct rect){ x, y, w, 48 }, "Try it:");
}

static struct widget *add_radio(struct app *a, int x, int y, int w, const char *text, const char *key, const char *arg, bool on);

/* A password field: label on the left, the masked field on the right; `field` indexes a->pw. */
static void add_pass(struct app *a, int x, int y, int w, const char *label, int field)
{
    struct widget *p = add(a, W_PASS, (struct rect){ x, y, w, ROW_H }, label);
    snprintf(p->key, sizeof(p->key), "pw");
    snprintf(p->arg, sizeof(p->arg), "%d", field);
}

static struct widget *add_button(struct app *a, int x, int y, int w, const char *text, const char *key)
{
    struct widget *b = add(a, W_BUTTON, (struct rect){ x, y, w, ROW_H - 4 }, text);
    snprintf(b->key, sizeof(b->key), "%s", key);
    return b;
}

static void build_security(struct app *a, int x, int y, int w)
{
    const char *kind = kv_get(a, "lock-kind");
    bool have = *kind && strcmp(kind, "none");
    add(a, W_HEADING, (struct rect){ x, y, w, ROW_H }, "Screen lock");
    y += ROW_H;
    char line[96];
    if (kv_int(a, "locked", 0))
        snprintf(line, sizeof(line), "The screen is locked.");
    else if (have)
        snprintf(line, sizeof(line), "Lock is on (%s). Super+L locks the screen.", kind);
    else
        snprintf(line, sizeof(line), "No lock yet. Choose a password or PIN below.");
    add(a, W_LABEL, (struct rect){ x, y, w, ROW_H }, line);
    y += ROW_H;
    if (have) {
        add_pass(a, x, y, w, !strcmp(kind, "pin") ? "Current PIN" : "Current password", 0);
        y += ROW_H + 2;
    }
    add_radio(a, x, y, 150, "Password", "sec-kind", "password", !a->sec_pin);
    add_radio(a, x + 160, y, 150, "PIN", "sec-kind", "pin", a->sec_pin);
    y += ROW_H;
    add_pass(a, x, y, w, have ? "New (empty = keep)" : "New", 1);
    y += ROW_H + 2;
    add_pass(a, x, y, w, "Repeat", 2);
    y += ROW_H + 4;
    add_slider(a, x, y, w, "Lock after idle", "sec-minutes", 0, 120, 1, " min");
    a->wd[a->nw - 1].val = a->sec_minutes;
    y += ROW_H;
    add_slider(a, x, y, w, "Screen off after", "sec-blank", 0, 60, 1, " min");
    a->wd[a->nw - 1].val = a->sec_blank;
    y += ROW_H;
    add_check(a, x, y, w, "Lock when MellaGUI starts", "sec-start", a->sec_start);
    y += ROW_H;
    add_check(a, x, y, w, "Console switch (Ctrl+Alt+Fn) works while locked", "sec-vt", a->sec_vt);
    y += ROW_H + 4;
    add_button(a, x, y, 90, "Apply", "sec-apply");
    if (have) {
        add_button(a, x + 100, y, 110, "Lock now", "sec-lock");
        add_button(a, x + 220, y, 130, "Remove lock", "sec-remove");
    }
    y += ROW_H + 2;
    char buf[80];
    const char *msg = a->sec_err[0] ? a->sec_err : lock_status_text(kv_get(a, "lock-status"), buf, sizeof(buf));
    if (*msg)
        add(a, W_LABEL, (struct rect){ x, y, w, ROW_H }, msg);
}

static void build_pointer(struct app *a, int x, int y, int w)
{
    add(a, W_HEADING, (struct rect){ x, y, w, ROW_H }, "Mouse and touchpad");
    y += ROW_H;
    add_slider(a, x, y, w, "Pointer speed", "pointer-speed", 25, 300, 5, "%");
    y += ROW_H;
    add_check(a, x, y, w, "Natural scrolling (content follows the fingers)", "natural-scroll",
              kv_int(a, "natural-scroll", 0) != 0);
}

static void build_desktop(struct app *a, int x, int y, int w)
{
    add(a, W_HEADING, (struct rect){ x, y, w, ROW_H }, "Desktop");
    y += ROW_H;
    add_check(a, x, y, w, "Clipboard gadget (Super+G)", "gadget", kv_int(a, "gadget", 1) != 0);
    y += ROW_H;
    add_check(a, x, y, w, "Sound feedback for sticky keys", "sound", kv_int(a, "sound", 1) != 0);
    y += ROW_H;
    add_check(a, x, y, w, "Volume mixer gadget (Super+V)", "mixer", kv_int(a, "mixer", 0) != 0);
    y += ROW_H;
    add_check(a, x, y, w, "System monitor gadget (Super+S)", "sysinfo", kv_int(a, "sysinfo", 0) != 0);
    y += ROW_H + 8;
    add(a, W_HEADING, (struct rect){ x, y, w, ROW_H }, "Wallpaper");
    y += ROW_H;
    add(a, W_LABEL, (struct rect){ x, y, w, ROW_H }, "PNG or PPM file (~/ is your home). Enter applies.");
    y += ROW_H;
    struct widget *f = add(a, W_FIELD, (struct rect){ x, y, w, 28 }, "wallpaper");
    snprintf(f->key, sizeof(f->key), "wallpaper");
    y += 34;
    static const char *const modes[4] = { "fill", "fit", "stretch", "center" };
    static const char *const names[4] = { "Fill", "Fit", "Stretch", "Center" };
    for (int i = 0; i < 4; i++)
        add_radio(a, x + i * (w / 4), y, w / 4, names[i], "wallpaper-mode", modes[i], !strcmp(kv_get(a, "wallpaper-mode"), modes[i]));
    y += ROW_H;
    add_button(a, x, y, 90, "Apply", "wall-apply");
    add_button(a, x + 100, y, 130, "No picture", "wall-clear");
    y += ROW_H;
    const char *st = kv_get(a, "wallpaper-status");
    if (*st)
        add(a, W_LABEL, (struct rect){ x, y, w, ROW_H }, !strcmp(st, "ok") ? "Wallpaper loaded." : st);
}

static struct widget *add_radio(struct app *a, int x, int y, int w, const char *text, const char *key, const char *arg, bool on)
{
    struct widget *r = add(a, W_RADIO, (struct rect){ x, y, w, ROW_H }, text);
    snprintf(r->key, sizeof(r->key), "%s", key);
    snprintf(r->arg, sizeof(r->arg), "%s", arg);
    r->on = on;
    return r;
}

/* "name|WxH|x,y;..." (the `outputs` setting) -> names left to right; returns how many (at most 8). */
static int output_names(const struct app *a, char names[8][40])
{
    const char *list = kv_get(a, "outputs");
    char name[64], desc[96];
    int n = 0;
    while (n < 8 && next_entry(&list, name, sizeof(name), desc, sizeof(desc)))
        snprintf(names[n++], 40, "%.39s", name);
    return n;
}

static void build_display(struct app *a, int x, int y, int w)
{
    char onames[8][40];
    int nout = output_names(a, onames);
    if (nout >= 2) {                                    /* several monitors: their order, left to right */
        add(a, W_HEADING, (struct rect){ x, y, w, ROW_H }, "Monitors, left to right");
        y += ROW_H;
        const char *ol = kv_get(a, "outputs");
        char nm[64], ds[96];
        for (int i = 0; next_entry(&ol, nm, sizeof(nm), ds, sizeof(ds)) && i < nout; i++) {
            char size[64] = "";
            sscanf(ds, "%63[^|]", size);
            char line[112];
            snprintf(line, sizeof(line), "%s   %s", nm, size);
            add(a, W_LABEL, (struct rect){ x, y, w - 76, ROW_H }, line);
            struct widget *l = add(a, W_BUTTON, (struct rect){ x + w - 68, y + 2, 32, ROW_H - 4 }, "<");
            snprintf(l->key, sizeof(l->key), "out-left");
            snprintf(l->arg, sizeof(l->arg), "%s", nm);
            struct widget *r = add(a, W_BUTTON, (struct rect){ x + w - 32, y + 2, 32, ROW_H - 4 }, ">");
            snprintf(r->key, sizeof(r->key), "out-right");
            snprintf(r->arg, sizeof(r->arg), "%s", nm);
            y += ROW_H;
        }
        y += 6;
    }
    const char *pending = kv_get(a, "resolution-confirm");
    if (*pending) {                                     /* a change waits to be kept; it is undone otherwise */
        char msg[96];
        snprintf(msg, sizeof(msg), "Keep this resolution? It goes back in %s s.", pending);
        add(a, W_LABEL, (struct rect){ x, y, w, ROW_H }, msg);
        y += ROW_H;
        struct widget *k = add(a, W_BUTTON, (struct rect){ x, y, 90, ROW_H - 4 }, "Keep");
        snprintf(k->key, sizeof(k->key), "resolution-confirm");
        snprintf(k->arg, sizeof(k->arg), "1");
        struct widget *u = add(a, W_BUTTON, (struct rect){ x + 100, y, 90, ROW_H - 4 }, "Go back");
        snprintf(u->key, sizeof(u->key), "resolution-confirm");
        snprintf(u->arg, sizeof(u->arg), "0");
        y += ROW_H + 8;
    }
    /* The monitor: who made it, what it is called, where it is connected and how big it is. */
    {
        char mk[40] = "", mo[40] = "", con[40] = "";
        int mmw = 0, mmh = 0;
        const char *v = kv_get(a, "monitor");
        if (*v && sscanf(v, "%39[^|]|%39[^|]|%39[^|]|%d|%d", mk, mo, con, &mmw, &mmh) >= 3) {
            char line[112];
            if (!strcmp(mk, "DRM") || !strcmp(mk, "MellaGUI"))
                snprintf(line, sizeof(line), "Monitor: %s", mo);                 /* no manufacturer known */
            else
                snprintf(line, sizeof(line), "Monitor: %s %s", mk, mo);
            add(a, W_HEADING, (struct rect){ x, y, w, ROW_H }, line);
            y += ROW_H;
            char info[112];
            if (mmw > 0 && mmh > 0)
                snprintf(info, sizeof(info), "%s   %d x %d mm  (%.1f\")", con, mmw, mmh,
                         sqrt((double)mmw * mmw + (double)mmh * mmh) / 25.4);
            else
                snprintf(info, sizeof(info), "%s", con);
            add(a, W_LABEL, (struct rect){ x, y, w, ROW_H }, info);
            y += ROW_H + 6;
        }
    }
    /* The heading carries what the screen runs at right now, in numbers, so it is always in view. */
    char head[112] = "Resolution";
    int nw = 0, nh = 0, nhz = 0;
    if (sscanf(kv_get(a, "resolution-now"), "%dx%d@%d", &nw, &nh, &nhz) >= 2 && nw > 0 && nh > 0) {
        if (nhz > 0)
            snprintf(head, sizeof(head), "Current resolution: %d x %d @ %d Hz", nw, nh, nhz);
        else
            snprintf(head, sizeof(head), "Current resolution: %d x %d", nw, nh);
    }
    add(a, W_HEADING, (struct rect){ x, y, w, ROW_H }, head);
    y += ROW_H;
    const char *cur = kv_get(a, "resolution");
    add_radio(a, x, y, w, "Automatic (the panel's own shape)", "resolution", "auto", !strcmp(cur, "auto"));
    y += ROW_H;
    add_radio(a, x, y, w, "The monitor's preferred mode", "resolution", "preferred", !strcmp(cur, "preferred"));
    y += ROW_H;
    const char *list = kv_get(a, "resolutions");
    char name[64], note[96], label[96];
    int shown = 0;
    while (shown < (nout >= 2 ? 3 : 7) && next_entry(&list, name, sizeof(name), note, sizeof(note))) {
        mode_label(name, note, label, sizeof(label));
        add_radio(a, x, y, w, label, "resolution", name, !strcmp(cur, name));
        y += ROW_H;
        shown++;
    }
    /* below the resolutions: the monitor's own settings (brightness, volume, input ...) are a program of their own */
    struct widget *mb = add(a, W_BUTTON, (struct rect){ x, y + 10, 260, ROW_H - 4 }, "Monitor settings...");
    snprintf(mb->key, sizeof(mb->key), "launch");
    snprintf(mb->arg, sizeof(mb->arg), "mellagui-monitor");
}

/* The hand-written mellagui.conf: menu, shortcuts, colours, sizes. The panel does not edit them, it opens the file. */
static void build_config(struct app *a, int x, int y, int w)
{
    const char *file = kv_get(a, "config-file"), *status = kv_get(a, "config-status");
    add(a, W_HEADING, (struct rect){ x, y, w, ROW_H }, "Configuration file");
    y += ROW_H;
    if (!*file) {
        add(a, W_LABEL, (struct rect){ x, y, w, ROW_H }, "MellaGUI was started without a configuration file.");
        return;
    }
    add(a, W_LABEL, (struct rect){ x, y, w, ROW_H }, file);
    y += ROW_H;
    char line[96];
    if (!strcmp(status, "missing"))
        snprintf(line, sizeof(line), "The file does not exist yet.");
    else if (!strncmp(status, "ignored:", 8))
        snprintf(line, sizeof(line), "Read, but %s line(s) were ignored (see the log).", status + 8);
    else
        snprintf(line, sizeof(line), "Menu, shortcuts, colours and sizes are set here.");
    add(a, W_LABEL, (struct rect){ x, y, w, ROW_H }, line);
    y += ROW_H + 6;
    if (!strcmp(status, "missing")) {
        add_button(a, x, y, 260, "Create it with the defaults", "config-create");
    } else {
        struct widget *e = add_button(a, x, y, 260, "Edit the file...", "config-edit");
        snprintf(e->arg, sizeof(e->arg), "%s", file);
        y += ROW_H + 4;
        add_button(a, x, y, 260, "Reload (Super+Shift+R)", "config-reload");
    }
    y += ROW_H + 10;
    add(a, W_LABEL, (struct rect){ x, y, w, ROW_H }, "Running programs keep their colours until restarted.");
}

static void build_sound(struct app *a, int x, int y, int w)
{
    struct entry ents[8];
    int shown;
    const char *cur;
    /* 1. the sound card's output (PipeWire card profile): analog, HDMI, S/PDIF - this is how HDMI audio is chosen */
    shown = pick_entries(kv_get(a, "audio-profiles"), 3, ents);
    if (shown) {
        add(a, W_HEADING, (struct rect){ x, y, w, ROW_H }, "Output device (HDMI, analog ...)");
        y += ROW_H;
        cur = kv_get(a, "audio-profile");
        for (int i = 0; i < shown; i++) {
            add_radio(a, x, y, w, ents[i].desc, "audio-profile", ents[i].name, !strcmp(cur, ents[i].name));
            y += ROW_H;
        }
        y += 6;
    }
    /* 2. the sound server's default output for applications */
    add(a, W_HEADING, (struct rect){ x, y, w, ROW_H }, "Applications (PulseAudio / PipeWire)");
    y += ROW_H;
    cur = kv_get(a, "sink");
    shown = pick_entries(kv_get(a, "sinks"), 2, ents);
    for (int i = 0; i < shown; i++) {
        add_radio(a, x, y, w, *ents[i].desc ? ents[i].desc : ents[i].name, "sink", ents[i].name, !strcmp(cur, ents[i].name));
        y += ROW_H;
    }
    if (!shown) {
        const char *why = kv_get(a, "sinks-status");
        if (!strcmp(why, "missing-tools")) {
            add(a, W_LABEL, (struct rect){ x, y, w, ROW_H }, "Neither pactl nor wpctl is installed.");
            add(a, W_LABEL, (struct rect){ x, y + ROW_H, w, ROW_H }, "sudo apt install pulseaudio-utils");
            y += 2 * ROW_H;
        } else if (!strcmp(why, "no-server")) {
            add(a, W_LABEL, (struct rect){ x, y, w, ROW_H }, "No sound server answers. Start it with:");
            add(a, W_LABEL, (struct rect){ x, y + ROW_H, w, ROW_H }, "systemctl --user enable --now pipewire");
            add(a, W_LABEL, (struct rect){ x, y + 2 * ROW_H, w, ROW_H }, "pipewire-pulse wireplumber");
            y += 3 * ROW_H;
        } else {
            add(a, W_LABEL, (struct rect){ x, y, w, ROW_H }, "Looking for a sound server...");
            y += ROW_H;
        }
    }
    y += 6;
    /* 3. MellaGUI's own beeps (ALSA device) */
    add(a, W_HEADING, (struct rect){ x, y, w, ROW_H }, "MellaGUI's own beeps (ALSA)");
    y += ROW_H;
    cur = kv_get(a, "audio-out");
    shown = pick_entries(kv_get(a, "audio-outs"), 4, ents);
    for (int i = 0; i < shown; i++) {
        add_radio(a, x, y, w, ents[i].desc, "audio-out", ents[i].name, !strcmp(cur, ents[i].name));
        y += ROW_H;
    }
}

static void build(struct app *a)
{
    a->nw = 0;
    for (int i = 0; i < N_PAGES; i++) {
        struct widget *t = add(a, W_TAB, (struct rect){ 8, 12 + i * 36, SIDE_W - 16, 32 }, PAGE_NAMES[i]);
        snprintf(t->key, sizeof(t->key), "page");
        snprintf(t->arg, sizeof(t->arg), "%d", i);
        t->on = (int)a->page == i;
    }
    int x = SIDE_W + 20, y = 14, w = a->win.bw - x - 20;
    if (w < 200)
        w = 200;
    switch (a->page) {
    case PAGE_KEYBOARD: build_keyboard(a, x, y, w); break;
    case PAGE_POINTER: build_pointer(a, x, y, w); break;
    case PAGE_DISPLAY: build_display(a, x, y, w); break;
    case PAGE_SOUND: build_sound(a, x, y, w); break;
    case PAGE_SECURITY: build_security(a, x, y, w); break;
    case PAGE_CONFIG: build_config(a, x, y, w); break;
    default: build_desktop(a, x, y, w); break;
    }
    if (a->focus >= a->nw)
        a->focus = -1;
}

static bool focusable(const struct widget *w)
{
    return w->kind != W_LABEL && w->kind != W_HEADING;
}

/* The track of a slider: label on the left, value text on the right. */
static struct rect slider_track(const struct widget *w)
{
    int label_w = 150, value_w = 70;
    return (struct rect){ w->r.x + label_w, w->r.y + w->r.h / 2 - 3, LM_MAX(w->r.w - label_w - value_w, 40), 6 };
}

static int widget_at(const struct app *a, double x, double y)
{
    for (int i = a->nw - 1; i >= 0; i--) {
        const struct widget *w = &a->wd[i];
        if (focusable(w) && x >= w->r.x && x < w->r.x + w->r.w && y >= w->r.y && y < w->r.y + w->r.h)
            return i;
    }
    return -1;
}

/* ---- actions ------------------------------------------------------------------------------------------------ */

/* A slider changed: the Security form keeps its value until "Apply", the others go to the compositor at once. */
static void slider_set(struct app *a, const struct widget *w, int v)
{
    if (!strcmp(w->key, "sec-minutes")) {
        a->sec_minutes = v;
        a->win.redraw = true;
    } else if (!strcmp(w->key, "sec-blank")) {
        a->sec_blank = v;
        a->win.redraw = true;
    } else {
        send_set_int(a, w->key, v);
    }
}

static void slider_drag_to(struct app *a, int idx, double x)
{
    const struct widget *w = &a->wd[idx];
    struct rect t = slider_track(w);
    int v = slider_value(w->lo, w->hi, w->step, (x - t.x) / (double)t.w);
    if (v != w->val)
        slider_set(a, w, v);
}

/* Starts another MellaGUI program next to this one (the monitor panel); it is not waited for. */
static void launch_sibling(const char *name, const char *arg)
{
    pid_t pid = fork();
    if (pid != 0)
        return;                                         /* the parent goes on (children are reaped: SIGCHLD is ignored) */
    setsid();
    char self[512], path[560];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n > 0) {
        self[n] = 0;
        char *slash = strrchr(self, '/');
        if (slash) {
            *slash = 0;
            snprintf(path, sizeof(path), "%s/%s", self, name);
            execl(path, name, arg, (char *)NULL);
        }
    }
    execlp(name, name, arg, (char *)NULL);                   /* installed on the PATH */
    _exit(127);
}

static void sec_wipe(struct app *a)
{
    memset(a->pw, 0, sizeof(a->pw));
    a->sec_err[0] = 0;
}

/* "Apply" / "Remove lock": send the form in one `lock-set`, then forget the typed secrets. */
static void sec_send(struct app *a, bool remove)
{
    bool have = strcmp(kv_get(a, "lock-kind"), "none") != 0 && *kv_get(a, "lock-kind");
    char v[3 * LM_LOCK_MAX_SECRET + 64];
    a->sec_err[0] = 0;
    if (!remove && strcmp(a->pw[1], a->pw[2])) {
        snprintf(a->sec_err, sizeof(a->sec_err), "The two new entries differ.");
    } else if (!remove && !have && !a->pw[1][0]) {
        snprintf(a->sec_err, sizeof(a->sec_err), "Type the new password / PIN first.");
    } else {
        const char *kind = remove ? "none" : a->pw[1][0] ? (a->sec_pin ? "pin" : "password") : "keep";
        lock_set_value(v, sizeof(v), have ? a->pw[0] : "", kind, remove ? "" : a->pw[1], a->sec_minutes, a->sec_start,
                       a->sec_blank, a->sec_vt);
        send_set(a, "lock-set", v);
        memset(v, 0, sizeof(v));
        memset(a->pw, 0, sizeof(a->pw));
    }
    a->win.redraw = true;
}

static void activate(struct app *a, int idx)
{
    const struct widget *w = &a->wd[idx];
    char buf[64];
    struct idlist l;
    switch (w->kind) {
    case W_TAB:
        a->page = (enum page)atoi(w->arg);
        a->focus = idx;
        a->win.redraw = true;
        if (a->page == PAGE_SOUND)
            send_set(a, "audio-rescan", "1");           /* ask `pactl` again: devices come and go */
        if (a->page == PAGE_SECURITY) {
            sec_wipe(a);
            a->sec_minutes = kv_int(a, "lock-timeout", 0);
            a->sec_blank = kv_int(a, "lock-blank", 3);
            a->sec_start = kv_int(a, "lock-start", 0) != 0;
            a->sec_vt = kv_int(a, "lock-vt", 1) != 0;
        }
        break;
    case W_CHECK:
        if (!strcmp(w->key, "sec-start")) {
            a->sec_start = !a->sec_start;
        } else if (!strcmp(w->key, "sec-vt")) {
            a->sec_vt = !a->sec_vt;
        } else if (!strcmp(w->key, "layout-enable")) {
            idlist_parse(&l, kv_get(a, "layouts"));
            if (idlist_toggle(&l, w->arg)) {
                idlist_format(&l, buf, sizeof(buf));
                send_set(a, "layouts", buf);
            }
        } else {
            send_set_int(a, w->key, w->on ? 0 : 1);
        }
        break;
    case W_RADIO:
        if (!strcmp(w->key, "sec-kind"))
            a->sec_pin = !strcmp(w->arg, "pin");
        else
            send_set(a, w->key, w->arg);
        break;
    case W_BUTTON:
        if (!strcmp(w->key, "sec-apply") || !strcmp(w->key, "sec-remove")) {
            sec_send(a, !strcmp(w->key, "sec-remove"));
        } else if (!strcmp(w->key, "sec-lock")) {
            send_set(a, "lock-now", "1");
        } else if (!strcmp(w->key, "launch")) {
            launch_sibling(w->arg, NULL);
        } else if (!strcmp(w->key, "config-edit")) {
            launch_sibling("mellagui-edit", w->arg);
        } else if (!strcmp(w->key, "config-reload") || !strcmp(w->key, "config-create")) {
            send_set(a, w->key, "1");
        } else if (!strcmp(w->key, "out-left") || !strcmp(w->key, "out-right")) {
            char names[8][40];
            int n = output_names(a, names), at = -1;
            for (int i = 0; i < n; i++)
                if (!strcmp(names[i], w->arg))
                    at = i;
            int to = at + (!strcmp(w->key, "out-left") ? -1 : 1);
            if (at >= 0 && to >= 0 && to < n) {
                char t[40];
                memcpy(t, names[at], sizeof(t));
                memcpy(names[at], names[to], sizeof(t));
                memcpy(names[to], t, sizeof(t));
                char joined[200] = "";
                for (int i = 0; i < n; i++)
                    snprintf(joined + strlen(joined), sizeof(joined) - strlen(joined), "%s%.39s", i ? "," : "", names[i]);
                send_set(a, "output-order", joined);
            }
        } else if (!strcmp(w->key, "wall-apply")) {
            send_set(a, "wallpaper", a->wall_edited ? a->wall : kv_get(a, "wallpaper"));
            a->wall_edited = false;
        } else if (!strcmp(w->key, "wall-clear")) {
            send_set(a, "wallpaper", "");
            a->wall_edited = false;
        } else if (!strcmp(w->key, "resolution-confirm")) {
            send_set(a, w->key, w->arg);
        } else if (!strcmp(w->key, "layout-up")) {
            idlist_parse(&l, kv_get(a, "layouts"));
            if (idlist_move_up(&l, w->arg)) {
                idlist_format(&l, buf, sizeof(buf));
                send_set(a, "layouts", buf);
            }
        }
        break;
    default:
        break;
    }
}

static void move_focus(struct app *a, int dir)
{
    if (a->nw == 0)
        return;
    int i = a->focus;
    for (int n = 0; n < a->nw; n++) {
        i = (i + dir + a->nw) % a->nw;
        if (i < 0)
            i = a->nw - 1;
        if (focusable(&a->wd[i])) {
            a->focus = i;
            a->win.redraw = true;
            return;
        }
    }
}

/* ---- drawing -------------------------------------------------------------------------------------------------- */

static void fill(struct app *a, struct rect r, uint32_t color)
{
    wlwin_fill(&a->win, r.x, r.y, r.w, r.h, color);
}

static void outline(struct app *a, struct rect r, uint32_t color)
{
    fill(a, (struct rect){ r.x, r.y, r.w, 1 }, color);
    fill(a, (struct rect){ r.x, r.y + r.h - 1, r.w, 1 }, color);
    fill(a, (struct rect){ r.x, r.y, 1, r.h }, color);
    fill(a, (struct rect){ r.x + r.w - 1, r.y, 1, r.h }, color);
}

/* Draws up to max_w pixels of text (clipped on a character boundary), 0 = no limit. */
static void draw_text(struct app *a, int x, int y, const char *s, uint32_t color, int max_w)
{
    wlwin_text(&a->win, x, y, s, 1, color, max_w);
}

static int text_y(const struct rect *r)
{
    return r->y + (r->h - LM_FONT_H) / 2;
}

static void paint_widget(struct app *a, int idx)
{
    const struct widget *w = &a->wd[idx];
    bool hot = idx == a->hover, focus = idx == a->focus;
    const struct rect *r = &w->r;
    switch (w->kind) {
    case W_HEADING:
        draw_text(a, r->x, text_y(r), w->text, COL_ACCENT, r->w);
        fill(a, (struct rect){ r->x, r->y + r->h - 2, r->w, 1 }, COL_KEY);
        break;
    case W_LABEL:
        draw_text(a, r->x, text_y(r), w->text, COL_DIM, r->w);
        break;
    case W_TAB: {
        fill(a, *r, w->on ? COL_KEY : (hot ? COL_HOVER : COL_SIDE));
        if (w->on)
            fill(a, (struct rect){ r->x, r->y, 3, r->h }, COL_ACCENT);
        draw_text(a, r->x + 14, text_y(r), w->text, COL_TEXT, r->w - 16);
        if (focus)
            outline(a, *r, COL_ACCENT);
        break;
    }
    case W_CHECK: {
        struct rect b = { r->x, r->y + (r->h - BOX) / 2, BOX, BOX };
        fill(a, b, hot ? COL_HOVER : COL_FIELD);
        outline(a, b, focus ? COL_ACCENT : COL_DIM);
        if (w->on)
            fill(a, (struct rect){ b.x + 4, b.y + 4, BOX - 8, BOX - 8 }, COL_ACCENT);
        draw_text(a, r->x + BOX + 10, text_y(r), w->text, COL_TEXT, r->w - BOX - 10);
        break;
    }
    case W_RADIO: {
        draw_text(a, r->x, text_y(r), w->on ? "(*)" : "( )", w->on ? COL_ACCENT : (hot ? COL_TEXT : COL_DIM), 0);
        draw_text(a, r->x + 32, text_y(r), w->text, w->on ? COL_TEXT : COL_DIM, r->w - 32);
        if (focus)
            outline(a, (struct rect){ r->x - 2, r->y + 2, r->w, r->h - 4 }, COL_ACCENT);
        break;
    }
    case W_BUTTON:
        fill(a, *r, a->press == idx && hot ? COL_DOWN : (hot ? COL_HOVER : COL_KEY));
        draw_text(a, r->x + (r->w - lm_font_text_width(w->text)) / 2, text_y(r), w->text, COL_TEXT, 0);
        if (focus)
            outline(a, *r, COL_ACCENT);
        break;
    case W_SLIDER: {
        struct rect t = slider_track(w);
        draw_text(a, r->x, text_y(r), w->text, COL_TEXT, 148);
        fill(a, t, COL_KEY);
        int kx = t.x + (int)((double)(w->val - w->lo) / (double)(w->hi - w->lo) * t.w);
        fill(a, (struct rect){ t.x, t.y, kx - t.x, t.h }, COL_ACCENT);
        struct rect knob = { kx - 5, r->y + 4, 10, r->h - 8 };
        fill(a, knob, a->drag == idx ? COL_DOWN : (hot ? COL_HOVER : COL_TEXT));
        if (focus)
            outline(a, (struct rect){ knob.x - 2, knob.y - 2, knob.w + 4, knob.h + 4 }, COL_ACCENT);
        char v[24];
        snprintf(v, sizeof(v), "%d%s", w->val, w->unit ? w->unit : "");
        draw_text(a, t.x + t.w + 12, text_y(r), v, COL_DIM, 0);
        break;
    }
    case W_PASS: {
        draw_text(a, r->x, text_y(r), w->text, COL_TEXT, 170);
        struct rect f = { r->x + 180, r->y + 2, LM_MAX(r->w - 180, 60), r->h - 4 };
        fill(a, f, COL_FIELD);
        outline(a, f, focus ? COL_ACCENT : COL_KEY);
        char dots[LM_LOCK_MAX_SECRET + 3];
        int n = LM_MIN(utf8_count(a->pw[atoi(w->arg)]), (int)sizeof(dots) - 3);
        memset(dots, '*', (size_t)n);
        dots[n] = 0;
        if (focus) {
            dots[n] = '_';
            dots[n + 1] = 0;
        }
        draw_text(a, f.x + 6, f.y + (f.h - LM_FONT_H) / 2, dots, COL_TEXT, f.w - 12);
        break;
    }
    case W_FIELD: {
        fill(a, *r, COL_FIELD);
        outline(a, *r, focus ? COL_ACCENT : COL_KEY);
        char shown[240];
        snprintf(shown, sizeof(shown), "%s%s", a->wall_edited ? a->wall : kv_get(a, "wallpaper"), focus ? "_" : "");
        draw_text(a, r->x + 6, r->y + (r->h - LM_FONT_H) / 2, shown, COL_TEXT, r->w - 12);
        break;
    }
    case W_TEXT: {
        draw_text(a, r->x, r->y + 2, w->text, COL_DIM, r->w);
        struct rect f = { r->x, r->y + 20, r->w, 28 };
        fill(a, f, COL_FIELD);
        outline(a, f, focus ? COL_ACCENT : COL_KEY);
        char shown[80];
        snprintf(shown, sizeof(shown), "%s%s", a->test, focus ? "_" : "");
        draw_text(a, f.x + 6, f.y + (f.h - LM_FONT_H) / 2, shown, COL_TEXT, f.w - 12);
        break;
    }
    }
}

static void paint(struct wlwin *w)
{
    struct app *a = w->data;
    fill(a, (struct rect){ 0, 0, a->win.bw, a->win.bh }, COL_BG);
    fill(a, (struct rect){ 0, 0, SIDE_W, a->win.bh }, COL_SIDE);
    build(a);
    for (int i = 0; i < a->nw; i++)
        paint_widget(a, i);
    const char *file = kv_get(a, "file");
    const char *l1 = !a->settings ? "The compositor has no settings interface."
                   : *file ? "Changes apply at once and are saved to:"
                           : "Changes apply at once (this session is not saving them).";
    draw_text(a, SIDE_W + 20, a->win.bh - (*file ? 44 : 24), l1, COL_DIM, a->win.bw - SIDE_W - 30);
    if (a->settings && *file) {
        int room = (a->win.bw - SIDE_W - 30) / LM_FONT_W, len = (int)strlen(file);
        draw_text(a, SIDE_W + 20, a->win.bh - 24, len > room ? file + (len - room) : file, COL_DIM, 0);
    }
}

/* ---- protocol events -------------------------------------------------------------------------------------- */

static void settings_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)c; (void)id;
    struct app *a = data;
    if (op == LM_MELLAGUI_SETTINGS_V1_EVT_VALUE) {
        bool changed = strcmp(kv_get(a, args[0].s), args[1].s) != 0;
        kv_set(a, args[0].s, args[1].s);
        if (changed && !strcmp(args[0].s, "lock-timeout"))
            a->sec_minutes = atoi(args[1].s);
        else if (changed && !strcmp(args[0].s, "lock-blank"))
            a->sec_blank = atoi(args[1].s);
        else if (changed && !strcmp(args[0].s, "lock-vt"))
            a->sec_vt = atoi(args[1].s) != 0;
        else if (changed && !strcmp(args[0].s, "lock-start"))
            a->sec_start = atoi(args[1].s) != 0;
    } else if (op == LM_MELLAGUI_SETTINGS_V1_EVT_DONE) {
        a->win.redraw = true;
    }
}

static void pointer_event(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
    struct app *a = w->data;
    build(a);
    switch (op) {
    case LM_WL_POINTER_EVT_ENTER:
        a->has_pointer = true;
        a->px = lm_fixed_to_double(args[2].f);
        a->py = lm_fixed_to_double(args[3].f);
        break;
    case LM_WL_POINTER_EVT_LEAVE:
        a->has_pointer = false;
        a->press = a->drag = -1;
        break;
    case LM_WL_POINTER_EVT_MOTION:
        a->px = lm_fixed_to_double(args[1].f);
        a->py = lm_fixed_to_double(args[2].f);
        if (a->drag >= 0)
            slider_drag_to(a, a->drag, a->px);
        break;
    case LM_WL_POINTER_EVT_BUTTON:
        if (args[2].u != 272) /* BTN_LEFT */
            break;
        if (args[3].u == 1) {
            int idx = widget_at(a, a->px, a->py);
            a->press = idx;
            if (idx >= 0) {
                a->focus = idx;
                if (a->wd[idx].kind == W_SLIDER) {
                    a->drag = idx;
                    slider_drag_to(a, idx, a->px);
                }
            }
        } else {
            int idx = widget_at(a, a->px, a->py);
            if (idx >= 0 && idx == a->press && a->drag < 0)
                activate(a, idx);
            a->press = a->drag = -1;
        }
        a->win.redraw = true;
        break;
    }
    int hover = a->has_pointer ? widget_at(a, a->px, a->py) : -1;
    if (hover != a->hover) {
        a->hover = hover;
        a->win.redraw = true;
    }
}

static void keyboard_event(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
    struct app *a = w->data;
    switch (op) {
    case LM_WL_KEYBOARD_EVT_KEY: {
        if (args[3].u == 0)
            break;
        build(a);
        uint32_t ks = lm_keymap_keysym(args[2].u, a->win.mods);
        struct widget *f = a->focus >= 0 ? &a->wd[a->focus] : NULL;
        bool plain = !(a->win.mods & (LM_MOD_CTRL | LM_MOD_ALT | LM_MOD_SUPER));
        if (ks == LM_KS_Escape) {
            a->win.running = false;
        } else if (ks == LM_KS_Tab) {
            move_focus(a, (a->win.mods & LM_MOD_SHIFT) ? -1 : 1);
        } else if (f && f->kind == W_PASS) {
            if (ks == LM_KS_Return || ks == LM_KS_KP_Enter) {
                sec_send(a, false);
            } else {
                edit_text(a->pw[atoi(f->arg)], sizeof(a->pw[0]), ks, plain);
                a->sec_err[0] = 0;
            }
            a->win.redraw = true;
        } else if (f && f->kind == W_FIELD) {
            if (ks == LM_KS_Return || ks == LM_KS_KP_Enter) {
                send_set(a, "wallpaper", a->wall_edited ? a->wall : kv_get(a, "wallpaper"));
                a->wall_edited = false;
            } else {
                if (!a->wall_edited) {
                    snprintf(a->wall, sizeof(a->wall), "%s", kv_get(a, "wallpaper"));
                    a->wall_edited = true;
                }
                edit_text(a->wall, sizeof(a->wall), ks, plain);
            }
            a->win.redraw = true;
        } else if (f && f->kind == W_TEXT) {
            size_t len = strlen(a->test);
            char txt[8];
            if (ks == LM_KS_BackSpace) {
                while (len > 0 && (a->test[len - 1] & 0xc0) == 0x80)
                    len--;
                if (len > 0)
                    len--;
                a->test[len] = 0;
            } else if (plain && lm_keysym_to_utf8(ks, txt) > 0 && len + strlen(txt) < sizeof(a->test)) {
                memcpy(a->test + len, txt, strlen(txt) + 1);
            }
            a->win.redraw = true;
        } else if (f && f->kind == W_SLIDER && (ks == LM_KS_Left || ks == LM_KS_Right)) {
            int v = LM_MAX(f->lo, LM_MIN(f->hi, f->val + (ks == LM_KS_Right ? f->step : -f->step)));
            if (v != f->val)
                slider_set(a, f, v);
        } else if (f && (ks == LM_KS_Return || ks == LM_KS_KP_Enter || ks == ' ')) {
            activate(a, a->focus);
        } else if (!f && (ks == LM_KS_Down || ks == LM_KS_Right)) {
            move_focus(a, 1);
        }
        break;
    }
    }
}

static void global_event(struct wlwin *w, uint32_t name, const char *iface, uint32_t ver)
{
    (void)ver;
    struct app *a = w->data;
    if (!strcmp(iface, "mellagui_settings_v1"))
        a->settings = wlc_bind(w->c, w->registry, name, &lm_mellagui_settings_v1_interface, 1, settings_event, a);
}

/* ---- command line ----------------------------------------------------------------------------------------- */

static int cli(struct app *a, int argc, char **argv)
{
    if (!a->settings) {
        fprintf(stderr, "mellagui-settings: the compositor has no mellagui_settings_v1\n");
        return 1;
    }
    int bad = 0;
    if (!strcmp(argv[1], "--set")) {
        for (int i = 2; i < argc; i++) {
            char *eq = strchr(argv[i], '=');
            if (!eq) {
                fprintf(stderr, "mellagui-settings: expected KEY=VALUE, got '%s'\n", argv[i]);
                return 2;
            }
            *eq = 0;
            if (!strcmp(argv[i], "lock-set")) {
                fprintf(stderr, "mellagui-settings: set the lock in the panel (a secret on a command line shows up in ps)\n");
                return 2;
            }
            send_set(a, argv[i], eq + 1);
            if (wlc_roundtrip(a->win.c) < 0)
                return 1;
            printf("%s=%s\n", argv[i], kv_get(a, argv[i]));
            if (strcmp(kv_get(a, argv[i]), eq + 1)) {
                fprintf(stderr, "mellagui-settings: '%s=%s' was not accepted as given\n", argv[i], eq + 1);
                bad = 1;
            }
        }
        return bad;
    }
    /* --get [KEY] */
    if (argc > 2) {
        if (!*kv_get(a, argv[2]) && strcmp(argv[2], "file")) {
            fprintf(stderr, "mellagui-settings: unknown setting '%s'\n", argv[2]);
            return 1;
        }
        printf("%s=%s\n", argv[2], kv_get(a, argv[2]));
        return 0;
    }
    for (int i = 0; i < a->nkv; i++)
        printf("%s=%s\n", a->kvs[i].k, a->kvs[i].v);
    return 0;
}

int main(int argc, char **argv)
{
    lm_theme_load_env();
    if (argc > 1 && !strcmp(argv[1], "--selftest"))
        return selftest();
    bool cli_mode = argc > 1 && (!strcmp(argv[1], "--get") || !strcmp(argv[1], "--set"));
    if (argc > 1 && !cli_mode) {
        fprintf(stderr, "usage: mellagui-settings [--get [KEY] | --set KEY=VALUE... | --selftest]\n");
        return 2;
    }

    signal(SIGCHLD, SIG_IGN);                           /* the panels started from here are nobody's business afterwards */
    static const struct wlwin_ops ops = { .paint = paint, .global = global_event, .pointer = pointer_event,
                                          .keyboard = keyboard_event };
    struct app a = { .press = -1, .drag = -1, .hover = -1, .focus = -1 };
    if (wlwin_connect(&a.win, "mellagui-settings", &ops, &a) < 0)
        return 1;
    if (a.settings && wlc_roundtrip(a.win.c) < 0)       /* the value burst that follows the bind */
        return 1;
    if (cli_mode) {
        int rc = cli(&a, argc, argv);
        wlwin_destroy(&a.win);
        return rc;
    }
    if (wlwin_open(&a.win, "Settings", "mellagui.settings", DEFAULT_W, DEFAULT_H, 520, 380) < 0)
        return 1;
    while (wlwin_step(&a.win, -1))
        ;
    wlwin_destroy(&a.win);
    return 0;
}
