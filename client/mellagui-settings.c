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
#include <strings.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "theme.h"
#include "flag.h"
#include "font.h"
#include "keymap.h"
#include "lock.h"
#include "metrics.h"
#include "settingshelp.h"
#include "uikit.h"
#include "util.h"
#include "wlclip.h"
#include "wltext.h"
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
#define MAX_WIDGETS 180
#define MAX_KV 160
#define VALUE_MAX 3300          /* longest value (the lists of resolutions / sound outputs) */

enum page { PAGE_INPUT, PAGE_ACCESS, PAGE_DESKTOP, PAGE_DISPLAY, PAGE_SOUND, PAGE_SECURITY, PAGE_SAVER, PAGE_CONFIG, PAGE_KEYS, PAGE_COLOURS, PAGE_DOCK, PAGE_CLOCK, PAGE_NIGHT, N_PAGES };
static const char *const PAGE_NAMES[N_PAGES] = { "Input", "Accessibility", "Desktop", "Display", "Sound", "Security", "Screensaver", "Config", "Keys", "Themes", "Dock", "Clock", "Night" };

#define NOTE_SIZE 0.8                                     /* fine print is 80 % of the normal size ... */
#define NOTE_LINE_H 14                                  /* ... on lines 14 px apart */

enum wkind { W_LABEL, W_HEADING, W_TAB, W_CHECK, W_RADIO, W_BUTTON, W_SLIDER, W_TEXT, W_PASS, W_FIELD, W_INPUT, W_SWATCH, W_HELP, W_NOTE };

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
    const char *note;           /* W_NOTE: the whole text (a string that lives on: the widget's own `text` holds 63 bytes) */
    int label_w;                /* slider: width of the label (0 = 150) */
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
    uint32_t dbl_last_ms;               /* Pointer page, the double click test: when the box was last pressed (0: not before) */
    int dbl_state;                      /* 0 not tried yet, 1 a single click, 2 a double click */
    char wall[220];                     /* the wallpaper file field while it is being edited */
    bool wall_edited;                   /* (else the field shows the compositor's value) */
    struct wlclip clip;                 /* the clipboard of the text fields (Ctrl+V / C / X, the right-click menu) */
    int menu_widget;                    /* the field whose right-click menu is showing, -1 none */
    int sel_widget;                     /* the field in which the left button is down (a selection is being dragged), -1 none */
    /* Menu page: the two fields below the entry list ([0] name, [1] command) and the entry they edit (-1 = a new one) */
    char input[7][1024];                /* [2] = the action of the Keys page, [3] = the colour (#rrggbb) of the Colours page,
                                         * [4] [5] [6] = name, icon and command of a Dock button */
    /* Keys page: the shortcut being edited (kcombo from "Record", input[2] its action), the row it came from, and the capture state */
    char kcombo[64];
    char ksel[64];
    bool recording;
    char keys_err[80];
    /* Colours page: which colour of the palette is being edited and its value (0..255 each) */
    int cpage, csel, tpage;
    char cname[32];
    int cr, cg, cb;
    char col_err[80];
    /* Dock page: the part shown (0 buttons, 1 look), the first button listed, the button the form edits (-1 = a new one), a slider that is
     * being dragged (sent when it is let go) */
    int dsub, dpage, dock_sel;
    char dock_err[80];
    char dpend[16];
    int dpend_val;
    /* Sizes page: the page shown and the value of a slider that is being dragged (sent when it is let go) */
    int mpage, mpend, mpend_val;
    /* Security page: a form that is sent in one piece by "Apply" (the compositor needs the current secret for any change) */
    char pw[3][LM_LOCK_MAX_SECRET + 1]; /* current / new / repeat */
    bool sec_pin;                       /* the new secret is a PIN (else a password) */
    int sound_tab;                      /* the Sound page's part: 0 the outputs and beeps, 1 the microphone */
    int input_tab;                      /* the Input page's part: 0 the keyboard, 1 the mouse and touchpad */
    int theme_tab;                      /* the Themes page's part: 0 the theme and its colours, 1 the buttons, 2 the sizes, 3 the font */
    int fpage;                          /* the Font part: which page of the list of fonts */
    int display_tab;                    /* the Display page's part: 0 the resolution, 1 the scale */
    int sec_minutes;                    /* idle minutes (0 = never) */
    int sec_blank;                      /* minutes locked before the monitors go to standby (0 = never) */
    bool sec_start;                     /* lock when MellaGUI starts */
    bool sec_vt;                        /* Ctrl+Alt+F1..F12 switches the console on the lock screen */
    char sec_err[64];                   /* a mistake found before sending */
    /* the help modal: open while help_title is set (the text may be built for a setting that has no table entry) */
    char help_title[96];
    char help_text[1200];
    int help_scroll;                    /* first visible line when the text is longer than the card */
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

static struct widget *add_radio(struct app *a, int x, int y, int w, const char *text, const char *key, const char *arg, bool on);

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
            struct widget *fl = add(a, W_SWATCH, (struct rect){ x, y + (ROW_H - LM_FLAG_H) / 2, LM_FLAG_W, LM_FLAG_H }, "");
            snprintf(fl->arg, sizeof(fl->arg), "f%s", id);                       /* the country's flag, then the language and its short code */
            char nm[64];
            if (strchr(lm_layout_name(i), '('))                                   /* "English (US)" has its code already */
                snprintf(nm, sizeof(nm), "%s", lm_layout_name(i));
            else
                snprintf(nm, sizeof(nm), "%s (%c%c)", lm_layout_name(i), id[0] - 32 * (id[0] >= 'a'), id[1] - 32 * (id[1] >= 'a'));
            struct widget *c = add_check(a, x + LM_FLAG_W + 8, y, LM_MIN(w - 150, 260) - LM_FLAG_W - 8, nm, "layout-enable", enabled);
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
    y += ROW_H;
    add_check(a, x, y, w, "Show the flag of the layout in use in the panel", "layout-badges", kv_int(a, "layout-badges", 0) != 0);
    y += ROW_H;
    {
        const char *lab = kv_get(a, "layout-label");
        add(a, W_LABEL, (struct rect){ x + BOX + 10, y, 100, ROW_H }, "Next to it:");
        static const struct { const char *arg, *text; int w; } opts[] = { { "none", "Nothing", 100 }, { "code", "Short code", 130 }, { "name", "Language", 120 } };
        int rx = x + BOX + 10 + 100;
        for (int k = 0; k < 3; k++) {
            struct widget *r = add_radio(a, rx, y, opts[k].w, opts[k].text, "layout-label", opts[k].arg, !strcmp(lab[0] ? lab : "none", opts[k].arg));
            (void)r;
            rx += opts[k].w;
        }
    }
    y += ROW_H + 8;
    add(a, W_HEADING, (struct rect){ x, y, w, ROW_H }, "Typing");
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

static void build_security_lock(struct app *a, int x, int y, int w);

/* Accessibility: what makes the computer easier to use without the usual hands, keys or sight. */
static void build_access(struct app *a, int x, int y, int w)
{
    add(a, W_HEADING, (struct rect){ x, y, w, ROW_H }, "Accessibility");
    y += ROW_H;
    add_check(a, x, y, w, "Sticky keys", "sticky", kv_int(a, "sticky", 0) != 0);
    y += ROW_H - 4;
    add(a, W_LABEL, (struct rect){ x + BOX + 10, y, w - BOX - 10, ROW_H - 4 }, "Tap a modifier to latch it; Shift x5 toggles.");
    y += ROW_H + 8;
    add(a, W_HEADING, (struct rect){ x, y, w, ROW_H }, "On-screen keyboard");
    y += ROW_H;
    add_check(a, x, y, w, "Windows keep out of its way", "osk-avoid", kv_int(a, "osk-avoid", 1) != 0);
    y += ROW_H - 4;
    add(a, W_LABEL, (struct rect){ x + BOX + 10, y, w - BOX - 10, ROW_H - 4 }, "Super+K or the Kbd button in the panel shows it.");
    y += ROW_H + 8;
    add(a, W_HEADING, (struct rect){ x, y, w, ROW_H }, "Screen magnifier");
    y += ROW_H;
    add_check(a, x, y, w, "Magnify the screen round the pointer", "magnifier", kv_int(a, "magnifier", 0) != 0);
    y += ROW_H;
    add_slider(a, x, y, w, "Zoom", "magnifier-zoom", 125, 800, 25, " %");
    y += ROW_H;
    add(a, W_LABEL, (struct rect){ x + BOX + 10, y, w - BOX - 10, ROW_H - 4 }, "Super + and Super - zoom in and out.");
}

/* A page with parts (Input, Sound, Display, Themes): small buttons in the heading row. */
static void build_part_tabs(struct app *a, int x, int y, int w, const char *key, const char *const *names, int n, int cur)
{
    int total = -8;
    for (int i = 0; i < n; i++)
        total += lm_font_text_width(names[i]) + 24 + 8;
    int at = x + w - total;
    for (int i = 0; i < n; i++) {
        int bw = lm_font_text_width(names[i]) + 24;
        struct widget *b = add_button(a, at, y + 1, bw, names[i], key);
        snprintf(b->arg, sizeof(b->arg), "%d", i);
        b->on = cur == i;
        at += bw + 8;
    }
}

static void build_security(struct app *a, int x, int y, int w)
{
    build_security_lock(a, x, y, w);
}

static void build_security_lock(struct app *a, int x, int y, int w)
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
    y += ROW_H + 2;
    {                                                                   /* the rule: the screensaver, when it is on, decides these two itself */
        bool saver_on = kv_int(a, "saver", 1) != 0 && kv_int(a, "saver-minutes", 10) > 0;
        add(a, W_LABEL, (struct rect){ x, y, w, ROW_H - 6 }, saver_on ? "The screensaver is on: its page decides when the screen locks." : "The screensaver is off, so \"Lock after idle\" applies:");
        y += ROW_H - 6;
    }
    add_slider(a, x, y, w, "Lock after idle", "sec-minutes", 0, 120, 1, " min");
    a->wd[a->nw - 1].val = a->sec_minutes;
    y += ROW_H;
    add_slider(a, x, y, w, "Screen off after", "sec-blank", 0, 60, 1, " min");
    a->wd[a->nw - 1].val = a->sec_blank;
    y += ROW_H;
    add_check(a, x, y, w, "Lock when MellaGUI starts", "sec-start", a->sec_start);
    y += ROW_H;
    add_check(a, x, y, w, "Console switch (Ctrl+Alt+Fn) works while locked", "sec-vt", a->sec_vt);
    y += ROW_H;
    add_check(a, x, y, w, "Show the wallpaper behind the lock screen", "lock-wallpaper", kv_int(a, "lock-wallpaper", 1) != 0);
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

static int dblclick_metric(const struct app *a, int *value, int *lo, int *hi);

static void build_pointer(struct app *a, int x, int y, int w)
{
    add(a, W_HEADING, (struct rect){ x, y, w, ROW_H }, "Mouse and touchpad");
    y += ROW_H;
    add_slider(a, x, y, w, "Pointer speed", "pointer-speed", 25, 300, 5, "%");
    y += ROW_H;
    add_check(a, x, y, w, "Natural scrolling (content follows the fingers)", "natural-scroll",
              kv_int(a, "natural-scroll", 0) != 0);
    y += ROW_H + 10;
    add(a, W_HEADING, (struct rect){ x, y, w, ROW_H }, "Double click");
    y += ROW_H;
    int v, lo, hi;
    if (*kv_get(a, "config-file") && dblclick_metric(a, &v, &lo, &hi) >= 0) {
        struct widget *sl = add(a, W_SLIDER, (struct rect){ x, y, w, ROW_H }, "Double-click time");
        snprintf(sl->key, sizeof(sl->key), "dblclick");
        sl->lo = lo;
        sl->hi = hi;
        sl->step = 25;
        sl->unit = " ms";
        sl->val = v;
        y += ROW_H + 4;
        const char *label = a->dbl_state == 2 ? "Double click!" : a->dbl_state == 1 ? "Single click: click again, faster" : "Try a double click here";
        struct widget *t = add(a, W_BUTTON, (struct rect){ x, y, LM_MIN(w, 340), 44 }, label);
        snprintf(t->key, sizeof(t->key), "dbl-try");
        t->on = a->dbl_state == 2;
        y += 52;
        add(a, W_LABEL, (struct rect){ x, y, w, ROW_H }, "Open programs keep their time until restarted.");
    } else {
        add(a, W_LABEL, (struct rect){ x, y, w, ROW_H }, "The double-click time lives in the configuration file:");
        y += ROW_H;
        add(a, W_LABEL, (struct rect){ x, y, w, ROW_H }, "Config page, \"Create it with the defaults\".");
    }
}

static void build_desktop(struct app *a, int x, int y, int w)
{
    add(a, W_HEADING, (struct rect){ x, y, w, ROW_H }, "Desktop");
    y += ROW_H;
    add_check(a, x, y, w, "Clipboard gadget (Super+G)", "gadget", kv_int(a, "gadget", 1) != 0);
    y += ROW_H;
    add_check(a, x, y, w, "Sound feedback (keys, dock, screenshots)", "sound", kv_int(a, "sound", 1) != 0);
    y += ROW_H;
    add_check(a, x, y, w, "Volume mixer gadget (Super+V)", "mixer", kv_int(a, "mixer", 0) != 0);
    y += ROW_H;
    add_check(a, x, y, w, "System monitor gadget (Super+S)", "sysinfo", kv_int(a, "sysinfo", 0) != 0);
    y += ROW_H;
    add_check(a, x, y, w, "Smart home gadget (Super+H)", "smart", kv_int(a, "smart", 0) != 0);
    y += ROW_H + 8;
    add(a, W_HEADING, (struct rect){ x, y, w, ROW_H }, "Wallpaper");
    y += ROW_H;
    add(a, W_LABEL, (struct rect){ x, y, w, ROW_H }, "PNG, JPEG or PPM file (~/ is your home). Enter applies.");
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
    add_button(a, x + 240, y, 130, "Browse...", "wall-browse");
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

static void build_display_resolution(struct app *a, int x, int y, int w)
{
    char onames[8][40];
    int nout = output_names(a, onames);
    if (nout >= 2) {                                    /* several monitors: their order, left to right */
        { struct widget *hd = add(a, W_HEADING, (struct rect){ x, y, w, ROW_H }, "Monitors, left to right"); hd->label_w = w - 210; }
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
            { struct widget *hd = add(a, W_HEADING, (struct rect){ x, y, w, ROW_H }, line); hd->label_w = nout >= 2 ? 0 : w - 210; }
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
    { struct widget *hd = add(a, W_HEADING, (struct rect){ x, y, w, ROW_H }, head); hd->label_w = (nout < 2 && !*kv_get(a, "monitor")) ? w - 210 : 0; }
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

/* The scale: everything of the shell and of the programs that know it gets bigger (a bigger font, bigger buttons, as sharp as ever). */
static void build_display_scale(struct app *a, int x, int y, int w)
{
    add(a, W_HEADING, (struct rect){ x, y, w, ROW_H }, "Scale");
    y += ROW_H;
    if (!*kv_get(a, "scale")) {
        add(a, W_LABEL, (struct rect){ x, y, w, ROW_H }, "This MellaGUI cannot change the scale.");
        return;
    }
    int cur = atoi(kv_get(a, "scale"));
    char line[96], nm[8], lab[16];
    int nw = 0, nh = 0, nhz = 0;
    if (sscanf(kv_get(a, "resolution-now"), "%dx%d@%d", &nw, &nh, &nhz) >= 2 && nw > 0)
        snprintf(line, sizeof(line), "The screen has %d x %d pixels; at %d %% the shell works in %d x %d.", nw, nh, cur,
                 (int)((double)nw * 100.0 / cur), (int)((double)nh * 100.0 / cur));
    else
        snprintf(line, sizeof(line), "Makes everything bigger. Now %d %%.", cur);
    add(a, W_LABEL, (struct rect){ x, y, w, ROW_H }, line);
    y += ROW_H + 2;
    static const int steps[] = { 100, 125, 150, 175, 200, 250, 300 };
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++, y += ROW_H) {
        snprintf(nm, sizeof(nm), "%d", steps[i]);
        snprintf(lab, sizeof(lab), "%d %%", steps[i]);
        add_radio(a, x, y, 200, lab, "scale", nm, cur == steps[i]);
    }
    y += 4;
    add(a, W_LABEL, (struct rect){ x, y, w, ROW_H }, "It applies to every monitor. A program that does not know the scale");
    y += ROW_H - 6;
    add(a, W_LABEL, (struct rect){ x, y, w, ROW_H }, "is enlarged by the shell and looks softer until it is updated.");
}

static void build_display(struct app *a, int x, int y, int w)
{
    static const char *const names[2] = { "Resolution", "Scale" };
    if (a->display_tab == 1)
        build_display_scale(a, x, y, w);
    else
        build_display_resolution(a, x, y, w);
    build_part_tabs(a, x, y, w, "display-tab", names, 2, a->display_tab);
}

/* The hand-written mellagui.conf: dock, shortcuts, colours, sizes. The panel does not edit them, it opens the file. */
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
        snprintf(line, sizeof(line), "The dock, shortcuts, colours and sizes are set here.");
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

/* ---- shared helpers of the list pages (Dock, Keys, Colours, Sizes) ---------------------------------------------------------------- */

#define MENU_US "\x1f"
#define MENU_ROW_H 22

static struct widget *menu_button(struct app *a, int x, int y, int w, const char *text, const char *key, int idx)
{
    struct widget *b = add(a, W_BUTTON, (struct rect){ x, y, w, MENU_ROW_H - 2 }, text);
    snprintf(b->key, sizeof(b->key), "%s", key);
    snprintf(b->arg, sizeof(b->arg), "%d", idx);
    return b;
}

/* ---- Dock page: the round buttons along the bottom ([dock] and [dock-apps] of mellagui.conf), changed through the dock-* settings ---- */

#define DOCK_ROWS 8
#define DOCK_MAX 16

struct dock_opt { const char *key, *label; int lo, hi, step; const char *unit; };
static const struct dock_opt dock_opts[] = {
    { "icon-size", "Icon size", 24, 128, 2, " px" }, { "zoom", "Zoom under the pointer", 100, 250, 5, " %" },
    { "gap", "Space between icons", 0, 40, 1, " px" }, { "margin", "Distance from the edge", 0, 100, 1, " px" },
};

/* The value of an option of the "dock-opts" setting ("enabled=1 autohide=0 icon-size=48 ..."). */
static int dock_opt(const struct app *a, const char *key, int def)
{
    const char *p = kv_get(a, "dock-opts");
    size_t kl = strlen(key);
    for (; *p; p++)
        if ((p == kv_get(a, "dock-opts") || p[-1] == ' ') && !strncmp(p, key, kl) && p[kl] == '=')
            return atoi(p + kl + 1);
    return def;
}

/* The icon-background option ("light", "dark", "none" or "#rrggbb"). */
static const char *dock_bg(const struct app *a, char *buf, size_t n)
{
    const char *p = kv_get(a, "dock-opts"), *key = "icon-background=";
    size_t kl = strlen(key);
    for (const char *q = p; *q; q++)
        if ((q == p || q[-1] == ' ') && !strncmp(q, key, kl)) {
            size_t l = strcspn(q + kl, " ");
            snprintf(buf, n, "%.*s", (int)l, q + kl);
            return buf;
        }
    snprintf(buf, n, "light");
    return buf;
}

static void dock_flush(struct app *a)
{
    if (!a->dpend[0])
        return;
    char v[48];
    snprintf(v, sizeof(v), "%s\x1f%d", a->dpend, a->dpend_val);
    send_set(a, "dock-set", v);
    a->dpend[0] = 0;
}

static void build_dock(struct app *a, int x, int y, int w)
{
    add(a, W_HEADING, (struct rect){ x, y, w, ROW_H }, "Dock");
    y += ROW_H;
    if (!*kv_get(a, "config-file")) {
        add(a, W_LABEL, (struct rect){ x, y, w, ROW_H }, "MellaGUI was started without a configuration file.");
        return;
    }
    struct widget *t0 = menu_button(a, x, y, 90, "Buttons", "dock-sub", 0), *t1 = menu_button(a, x + 96, y, 90, "Look", "dock-sub", 1);
    t0->on = a->dsub == 0;
    t1->on = a->dsub == 1;
    y += MENU_ROW_H + 8;
    if (a->dsub == 1) {
        add_check(a, x, y, w, "Show the dock", "dock-enabled", dock_opt(a, "enabled", 1));
        y += ROW_H;
        add_check(a, x, y, w, "Hide it until the pointer touches the bottom edge", "dock-autohide", dock_opt(a, "autohide", 0));
        y += ROW_H + 6;
        {
            char bg[16];
            dock_bg(a, bg, sizeof(bg));
            add(a, W_LABEL, (struct rect){ x, y, 200, ROW_H }, "Plate under logos");
            static const char *const vals[3] = { "light", "dark", "none" };
            static const char *const names[3] = { "Light", "Dark", "None" };
            for (int i = 0; i < 3; i++)
                add_radio(a, x + 200 + i * ((w - 200) / 3), y, (w - 200) / 3, names[i], "dock-bg", vals[i], !strcmp(bg, vals[i]));
            y += ROW_H + 6;
        }
        for (size_t i = 0; i < sizeof(dock_opts) / sizeof(dock_opts[0]); i++) {
            const struct dock_opt *o = &dock_opts[i];
            struct widget *sl = add(a, W_SLIDER, (struct rect){ x, y, w, 26 }, o->label);
            snprintf(sl->key, sizeof(sl->key), "dk-%s", o->key);
            sl->lo = o->lo;
            sl->hi = o->hi;
            sl->step = o->step;
            sl->label_w = 200;
            sl->val = !strcmp(a->dpend, o->key) ? a->dpend_val : LM_CLAMP(dock_opt(a, o->key, o->lo), o->lo, o->hi);
            sl->unit = o->unit;
            y += 26;
        }
        y += 8;
        add_button(a, x, y, 220, "Restore the built-in dock", "dock-reset")->r.h = MENU_ROW_H - 2;
        add_button(a, x + 230, y, LM_MIN(w - 230, 330), "Add all MellaGUI programs to the launcher", "launcher-defaults")->r.h = MENU_ROW_H - 2;
        add(a, W_LABEL, (struct rect){ x, y + MENU_ROW_H + 4, w, 24 }, "Applied when you let go of a slider.");
        return;
    }
    char key[16];
    int n = 0;
    for (; n < DOCK_MAX; n++) {
        snprintf(key, sizeof(key), "dock-%d", n);
        if (!strchr(kv_get(a, key), 0x1f))
            break;
    }
    int pages = LM_MAX(1, (n + DOCK_ROWS - 1) / DOCK_ROWS);
    a->dpage = LM_CLAMP(a->dpage, 0, pages - 1);
    for (int i = a->dpage * DOCK_ROWS; i < LM_MIN(n, (a->dpage + 1) * DOCK_ROWS); i++) {
        snprintf(key, sizeof(key), "dock-%d", i);
        const char *v = kv_get(a, key), *us1 = strchr(v, 0x1f), *us2 = strchr(us1 + 1, 0x1f);
        char name[64], line[200];
        snprintf(name, sizeof(name), "%.*s", (int)LM_MIN((size_t)(us1 - v), sizeof(name) - 1), v);
        if (us2 && us2 > us1 + 1)
            snprintf(line, sizeof(line), "[%.*s] %s", (int)(us2 - us1 - 1), us1 + 1, us2 + 1);
        else
            snprintf(line, sizeof(line), "%s", us2 ? us2 + 1 : us1 + 1);
        struct widget *e = menu_button(a, x, y, 110, name, "dock-edit", i);
        e->on = a->dock_sel == i;
        int bx = x + w - 3 * 26;
        add(a, W_LABEL, (struct rect){ x + 118, y, bx - x - 122, MENU_ROW_H }, line);
        menu_button(a, bx, y, 24, "^", "dock-up", i);
        menu_button(a, bx + 26, y, 24, "v", "dock-down", i);
        menu_button(a, bx + 52, y, 24, "x", "dock-del", i);
        y += MENU_ROW_H;
    }
    y = 14 + ROW_H + MENU_ROW_H + 8 + DOCK_ROWS * MENU_ROW_H + 2;
    if (pages > 1) {
        menu_button(a, x, y, 24, "<", "dock-prev", 0);
        char lbl[32];
        snprintf(lbl, sizeof(lbl), "%d / %d", a->dpage + 1, pages);
        add(a, W_LABEL, (struct rect){ x + 32, y, 80, MENU_ROW_H }, lbl);
        menu_button(a, x + 100, y, 24, ">", "dock-next", 0);
    }
    y += MENU_ROW_H + 4;
    if (a->dock_sel >= n)
        a->dock_sel = -1;
    struct widget *fn = add(a, W_INPUT, (struct rect){ x, y, 120, 26 }, "name");
    snprintf(fn->arg, sizeof(fn->arg), "4");
    struct widget *fi = add(a, W_INPUT, (struct rect){ x + 128, y, w - 128, 26 }, "icon: terminal, files, theme:firefox ... or a picture file");
    snprintf(fi->arg, sizeof(fi->arg), "5");
    y += 30;
    struct widget *fc = add(a, W_INPUT, (struct rect){ x, y, w - 78, 26 }, "command");
    snprintf(fc->arg, sizeof(fc->arg), "6");
    add_button(a, x + w - 70, y, 70, a->dock_sel >= 0 ? "Update" : "Add", "dock-put")->r.h = 26;
    y += 32;
    const char *hint = a->dock_err[0] ? a->dock_err : n >= DOCK_MAX ? "The dock is full (16 buttons)." : "Click a name to edit it. An empty icon is guessed.";
    add(a, W_LABEL, (struct rect){ x, y - 1, w, 24 }, hint);
}

/* ---- Saver page: the screensaver (mellagui-matrix, started by the compositor when nothing has been touched for a while) ---------- */

static void build_saver(struct app *a, int x, int y, int w)
{
    add(a, W_HEADING, (struct rect){ x, y, w, ROW_H }, "Screensaver");
    y += ROW_H;
    add_check(a, x, y, w, "Start the screensaver when nobody uses the computer", "saver", kv_int(a, "saver", 1) != 0);
    y += ROW_H;
    add_slider(a, x, y, w, "Starts after", "saver-minutes", 1, 120, 1, " min");
    y += ROW_H;
    add_slider(a, x, y, w, "Lock after", "saver-lock", 0, 60, 1, " min");
    y += ROW_H;
    add_slider(a, x, y, w, "Monitor sleeps after", "saver-blank", 0, 240, 1, " min");
    y += ROW_H;
    const char *style = kv_get(a, "saver-style");
    add(a, W_LABEL, (struct rect){ x, y, 80, ROW_H }, "Style");
    add_radio(a, x + 80, y, 170, "Room (3D)", "saver-style", "room", strcmp(style, "flat") != 0);
    add_radio(a, x + 260, y, 170, "Flat", "saver-style", "flat", !strcmp(style, "flat"));
    y += ROW_H;
    add_check(a, x, y, w, "A cat lives in the room (Tom)", "saver-cat", kv_int(a, "saver-cat", 1) != 0);
    y += ROW_H + 8;
    add_button(a, x, y, 230, "Show it now (any key ends it)", "saver-now");
    y += ROW_H + 10;
    struct widget *nt = add(a, W_NOTE, (struct rect){ x, y, w, 6 * NOTE_LINE_H }, "");
    nt->note = "The rule: while the screensaver is on, these times decide, and the Security page's \"Lock after idle\" is not used (it applies when "
               "it is off). When the lock screen itself is in front, the Security page's \"Screen off after\" decides when the monitors sleep. "
               "\"Starts after\" and \"Monitor sleeps after\" count from the last touch, \"Lock after\" from when it started (0 = never; needs a password or PIN). The monitor fades out, "
               "then sleeps; any touch wakes it and is not passed on. It does not start over a full screen window.";
    (void)nt;
}

/* ---- Clock page: the panel's clock, the date and the calendar that a click on the clock shows (mellagui-calendar) ---------------- */

static void build_clock(struct app *a, int x, int y, int w)
{
    add(a, W_HEADING, (struct rect){ x, y, w, ROW_H }, "Clock and date");
    y += ROW_H;
    add_radio(a, x, y, w / 2, "24 hour clock", "clock-hours", "24", !strcmp(kv_get(a, "clock-hours"), "24"));
    add_radio(a, x + w / 2, y, w / 2, "12 hour (AM / PM)", "clock-hours", "12", !strcmp(kv_get(a, "clock-hours"), "12"));
    y += ROW_H;
    add_check(a, x, y, w, "Show seconds", "clock-seconds", kv_int(a, "clock-seconds", 0) != 0);
    y += ROW_H;
    add_check(a, x, y, w, "Show the date in the panel", "clock-date", kv_int(a, "clock-date", 1) != 0);
    y += ROW_H;
    add_check(a, x, y, w, "Weekday before the date", "clock-weekday", kv_int(a, "clock-weekday", 0) != 0);
    y += ROW_H;
    static const char *const fmts[4] = { "dmy", "ymd", "mdy", "long" };
    static const char *const names[4] = { "10.11.2026", "2026-11-10", "11/10/2026", "10 Nov 2026" };
    for (int i = 0; i < 4; i++)
        add_radio(a, x + (i % 2) * (w / 2), y + (i / 2) * ROW_H, w / 2, names[i], "date-format", fmts[i], !strcmp(kv_get(a, "date-format"), fmts[i]));
    y += 2 * ROW_H + 8;
    add(a, W_HEADING, (struct rect){ x, y, w, ROW_H }, "Calendar");
    y += ROW_H;
    add_check(a, x, y, w, "A click on the clock shows the calendar", "cal-enabled", kv_int(a, "cal-enabled", 1) != 0);
    y += ROW_H;
    add_radio(a, x, y, w / 2, "Week starts on Monday", "cal-week-start", "mon", !strcmp(kv_get(a, "cal-week-start"), "mon"));
    add_radio(a, x + w / 2, y, w / 2, "... on Sunday", "cal-week-start", "sun", !strcmp(kv_get(a, "cal-week-start"), "sun"));
    y += ROW_H;
    add_check(a, x, y, w, "Show week numbers", "cal-weeks", kv_int(a, "cal-weeks", 0) != 0);
    y += ROW_H + 6;
    add(a, W_LABEL, (struct rect){ x, y, w, ROW_H }, "The calendar is mellagui-calendar, started with the desktop;");
    add(a, W_LABEL, (struct rect){ x, y + ROW_H - 8, w, ROW_H }, "another program: [commands] calendar = ... in mellagui.conf.");
}

/* "HH:MM" -> minutes after midnight rounded down to 15, def when it is not a time */
static int time_minutes(const char *t, int def)
{
    int h, m;
    return t && sscanf(t, "%d:%d", &h, &m) == 2 && h >= 0 && h < 24 && m >= 0 && m < 60 ? h * 60 + m / 15 * 15 : def;
}

static bool time_slider(const struct widget *w)
{
    return w->kind == W_SLIDER && (!strcmp(w->key, "night-from") || !strcmp(w->key, "night-to"));
}

/* Night light: a warmer display in the evening (src/nightlight.c). */
static void build_night(struct app *a, int x, int y, int w)
{
    static const char *const modes[3] = { "off", "on", "auto" };
    static const char *const names[3] = { "Off", "On", "Scheduled" };
    const char *cur = kv_get(a, "night-light");
    add(a, W_HEADING, (struct rect){ x, y, w, ROW_H }, "Night light");
    y += ROW_H;
    for (int i = 0; i < 3; i++)
        add_radio(a, x + i * (w / 3), y, w / 3, names[i], "night-light", modes[i], !strcmp(cur, modes[i]));
    y += ROW_H + 6;
    add_slider(a, x, y, w, "Colour temperature", "night-temp", 2000, 6500, 100, " K");
    y += ROW_H;
    add(a, W_LABEL, (struct rect){ x, y, w, ROW_H }, "6500 K is no change; lower is warmer (less blue).");
    y += ROW_H + 8;
    add(a, W_HEADING, (struct rect){ x, y, w, ROW_H }, "Schedule");
    y += ROW_H;
    struct widget *f = add(a, W_SLIDER, (struct rect){ x, y, w, ROW_H }, "From");
    snprintf(f->key, sizeof(f->key), "night-from");
    f->lo = 0;
    f->hi = 23 * 60 + 45;
    f->step = 15;
    f->val = time_minutes(kv_get(a, "night-from"), 21 * 60);
    y += ROW_H;
    struct widget *t = add(a, W_SLIDER, (struct rect){ x, y, w, ROW_H }, "Until");
    snprintf(t->key, sizeof(t->key), "night-to");
    t->lo = 0;
    t->hi = 23 * 60 + 45;
    t->step = 15;
    t->val = time_minutes(kv_get(a, "night-to"), 6 * 60 + 30);
    y += ROW_H + 6;
    add(a, W_LABEL, (struct rect){ x, y, w, ROW_H }, "Used when 'Scheduled' is chosen.");
    add(a, W_LABEL, (struct rect){ x, y + ROW_H - 8, w, ROW_H }, "Super+Shift+N switches the night light on and off.");
}

static void dock_clear_form(struct app *a)
{
    a->input[4][0] = a->input[5][0] = a->input[6][0] = 0;
    a->dock_sel = -1;
}

static void dock_put(struct app *a)
{
    a->dock_err[0] = 0;
    char *icon_text = a->input[5];
    size_t il = strlen(icon_text);
    if (il >= 2 && icon_text[0] == '[' && icon_text[il - 1] == ']') {      /* "[theme:firefox]" as the file writes it: the brackets are not part of the icon */
        memmove(icon_text, icon_text + 1, il - 2);
        icon_text[il - 2] = 0;
    }
    const char *name = a->input[4], *icon = a->input[5], *cmd = a->input[6];
    size_t nl = strlen(name);
    if (!nl || !*cmd) {
        snprintf(a->dock_err, sizeof(a->dock_err), "Type a name and a command.");
        return;
    }
    if (nl >= 64 || strchr(name, '=') || name[0] == '#' || name[0] == ';' || name[0] == '[' || name[0] == ' ' || name[nl - 1] == ' ') {
        snprintf(a->dock_err, sizeof(a->dock_err), "Name: no '=', no leading # ; [ or spaces.");
        return;
    }
    if (icon[0] == ' ' || (*icon && icon[strlen(icon) - 1] == ' ') || strchr(icon, ']') || icon[0] == '[') {
        snprintf(a->dock_err, sizeof(a->dock_err), "Icon: a name, theme:NAME or a picture file.");
        return;
    }
    if (cmd[0] == ' ' || cmd[strlen(cmd) - 1] == ' ') {
        snprintf(a->dock_err, sizeof(a->dock_err), "Command: no space at the start or end.");
        return;
    }
    char key[16];
    for (int i = 0; i < DOCK_MAX; i++) {
        snprintf(key, sizeof(key), "dock-%d", i);
        const char *v = kv_get(a, key), *us = strchr(v, 0x1f);
        if (!us)
            break;
        if (i != a->dock_sel && strlen(name) == (size_t)(us - v) && !strncasecmp(name, v, nl)) {
            snprintf(a->dock_err, sizeof(a->dock_err), "There is already a button with that name.");
            return;
        }
    }
    char v[1700];
    snprintf(v, sizeof(v), "%d" MENU_US "%s" MENU_US "%s" MENU_US "%s", a->dock_sel, name, icon, cmd);
    send_set(a, "dock-put", v);
    dock_clear_form(a);
}

/* ---- Keys page: the keyboard shortcuts ([keys] of mellagui.conf), changed through the keys-* settings -------------------------- */

#define KEYS_ROWS 10

static void build_keys(struct app *a, int x, int y, int w)
{
    if (!*kv_get(a, "config-file")) {
        add(a, W_HEADING, (struct rect){ x, y, w, ROW_H }, "Keyboard shortcuts");
        add(a, W_LABEL, (struct rect){ x, y + ROW_H, w, ROW_H }, "MellaGUI was started without a configuration file.");
        return;
    }
    int count = kv_int(a, "keys-count", 0), page = kv_int(a, "keys-page", 0);
    int pages = LM_MAX(1, (count + KEYS_ROWS - 1) / KEYS_ROWS);
    add(a, W_HEADING, (struct rect){ x, y, w, ROW_H }, "Keyboard shortcuts");
    y += ROW_H;
    char key[16];
    for (int i = 0; i < KEYS_ROWS; i++) {
        snprintf(key, sizeof(key), "keys-row-%d", i);
        const char *v = kv_get(a, key), *us = strchr(v, 0x1f);
        if (!us)
            break;
        char combo[64];
        snprintf(combo, sizeof(combo), "%.*s", (int)LM_MIN((size_t)(us - v), sizeof(combo) - 1), v);
        struct widget *e = menu_button(a, x, y, 150, combo, "keys-edit", i);
        e->on = !strcmp(a->ksel, combo);
        add(a, W_LABEL, (struct rect){ x + 158, y, w - 158 - 28, MENU_ROW_H }, us + 1);
        menu_button(a, x + w - 24, y, 24, "x", "keys-del", i);
        y += MENU_ROW_H;
    }
    int py = 14 + ROW_H + KEYS_ROWS * MENU_ROW_H + 4;      /* the pager sits below all ten rows, the list may be shorter */
    menu_button(a, x, py, 24, "<", "keys-prev", 0);
    char lbl[32];
    snprintf(lbl, sizeof(lbl), "%d / %d", page + 1, pages);
    add(a, W_LABEL, (struct rect){ x + 32, py, 80, MENU_ROW_H }, lbl);
    menu_button(a, x + 100, py, 24, ">", "keys-next", 0);
    add_button(a, x + w - 150, py, 150, "Restore defaults", "keys-reset")->r.h = MENU_ROW_H - 2;
    py += MENU_ROW_H + 6;
    const char *rec = a->recording ? "Press the keys..." : a->kcombo[0] ? a->kcombo : "Record a key...";
    struct widget *r = add(a, W_BUTTON, (struct rect){ x, py, 150, 26 }, rec);
    snprintf(r->key, sizeof(r->key), "keys-record");
    r->on = a->recording;
    struct widget *f = add(a, W_INPUT, (struct rect){ x + 158, py, w - 158 - 124, 26 }, "action, e.g. close");
    snprintf(f->arg, sizeof(f->arg), "2");
    menu_button(a, x + w - 118, py, 24, "<", "keys-act-prev", 0)->r.h = 26;
    menu_button(a, x + w - 92, py, 24, ">", "keys-act-next", 0)->r.h = 26;
    add_button(a, x + w - 64, py, 64, a->ksel[0] ? "Update" : "Add", "keys-put")->r.h = 26;
    py += 32;
    add(a, W_LABEL, (struct rect){ x, py, w, 24 },
        a->keys_err[0] ? a->keys_err : "Click a shortcut to change it; < > pick an action.");
}

/* The action names the keys-actions setting lists, as a cyclic walk from the word the field starts with. */
static void keys_cycle_action(struct app *a, int dir)
{
    char names[512];
    snprintf(names, sizeof(names), "%s", kv_get(a, "keys-actions"));
    char *list[40];
    int n = 0;
    for (char *t = strtok(names, " "); t && n < 40; t = strtok(NULL, " "))
        list[n++] = t;
    if (!n)
        return;
    char cur[64];
    snprintf(cur, sizeof(cur), "%.*s", (int)strcspn(a->input[2], " "), a->input[2]);
    int at = -1;
    for (int i = 0; i < n; i++)
        if (!strcmp(list[i], cur))
            at = i;
    int to = at < 0 ? (dir > 0 ? 0 : n - 1) : (at + dir + n) % n;
    bool ws = !strcmp(list[to], "workspace") || !strcmp(list[to], "move-to-workspace");
    const char *rest = a->input[2] + strcspn(a->input[2], " ");
    while (*rest == ' ')
        rest++;
    char out[300];
    if (ws)
        snprintf(out, sizeof(out), "%s %s", list[to], *rest && atoi(rest) >= 1 && atoi(rest) <= 4 ? rest : "1");
    else if (!strcmp(list[to], "exec"))
        snprintf(out, sizeof(out), "%s%s%s", list[to], *rest ? " " : "", at >= 0 && !strcmp(cur, "exec") ? rest : "");
    else
        snprintf(out, sizeof(out), "%s", list[to]);
    snprintf(a->input[2], sizeof(a->input[2]), "%s", out);
    a->keys_err[0] = 0;
}

static void colour_apply(struct app *a)
{
    uint32_t c;
    if (a->csel < 0 || !lm_theme_parse_color(a->input[3], &c)) {
        snprintf(a->col_err, sizeof(a->col_err), "Type a colour like #6fa8ff.");
        return;
    }
    char v[64];
    snprintf(v, sizeof(v), "%s\x1f#%06x", a->cname, (unsigned)c);
    send_set(a, "theme-set", v);
    a->col_err[0] = 0;
}

static void keys_put(struct app *a)
{
    a->keys_err[0] = 0;
    if (!a->kcombo[0] || !a->input[2][0]) {
        snprintf(a->keys_err, sizeof(a->keys_err), "Record a key and choose an action first.");
        return;
    }
    char word[48];
    snprintf(word, sizeof(word), "%.*s", (int)LM_MIN(strcspn(a->input[2], " "), sizeof(word) - 1), a->input[2]);
    char names[512];
    snprintf(names, sizeof(names), " %s ", kv_get(a, "keys-actions"));
    char pat[52];
    snprintf(pat, sizeof(pat), " %s ", word);
    if (!strstr(names, pat)) {
        snprintf(a->keys_err, sizeof(a->keys_err), "Unknown action '%s'. Use < > to pick one.", word);
        return;
    }
    const char *arg = a->input[2] + strlen(word);
    while (*arg == ' ')
        arg++;
    bool ws = !strcmp(word, "workspace") || !strcmp(word, "move-to-workspace"), ex = !strcmp(word, "exec");
    if ((ws && !(atoi(arg) >= 1 && atoi(arg) <= 4)) || (ex && !*arg) || (!ws && !ex && *arg) || strlen(a->input[2]) > 250) {
        snprintf(a->keys_err, sizeof(a->keys_err), ws ? "workspace needs a number 1..4." : ex ? "exec needs a command." : "That action takes no argument.");
        return;
    }
    if (a->ksel[0] && strcmp(a->ksel, a->kcombo))                    /* moved to another key: the old one goes */
        send_set(a, "keys-del", a->ksel);
    char v[400];
    snprintf(v, sizeof(v), "%s\x1f%s", a->kcombo, a->input[2]);
    send_set(a, "keys-set", v);
    a->kcombo[0] = a->ksel[0] = a->input[2][0] = 0;
}

/* ---- Colours page: the palette ([theme] of mellagui.conf), changed through the theme-* settings ------------------------------- */

#define COLS_ROWS 8
#define MAX_COLOURS 160

struct colour { char name[32]; unsigned value; bool changed; };

static int colour_list(const struct app *a, struct colour *out, int max)
{
    char text[VALUE_MAX];
    snprintf(text, sizeof(text), "%s", kv_get(a, "theme-colors"));
    int n = 0;
    for (char *t = strtok(text, " "); t && n < max; t = strtok(NULL, " ")) {
        char *eq = strchr(t, '=');
        if (!eq || eq[1] != '#')
            continue;
        *eq = 0;
        snprintf(out[n].name, sizeof(out[n].name), "%s", t);
        out[n].value = (unsigned)strtoul(eq + 2, NULL, 16);
        out[n].changed = strchr(eq, '*') != NULL;
        n++;
    }
    return n;
}

/* The themes the compositor offers (theme-list: "colorful* graphite light ...", * = in use). */
#define MAX_THEMES 24
#define THEMES_SHOWN 5
#define THEME_ROW_H (MENU_ROW_H + 6)
struct theme_entry { char name[LM_THEME_NAME_MAX]; bool active; };

static int theme_names(const struct app *a, struct theme_entry *out, int max)
{
    char text[VALUE_MAX];
    snprintf(text, sizeof(text), "%s", kv_get(a, "theme-list"));
    int n = 0;
    for (char *t = strtok(text, " "); t && n < max; t = strtok(NULL, " ")) {
        size_t len = strlen(t);
        out[n].active = len && t[len - 1] == '*';
        if (out[n].active)
            t[len - 1] = 0;
        snprintf(out[n].name, sizeof(out[n].name), "%s", t);
        n++;
    }
    return n;
}

static void colour_set_hex(struct app *a)
{
    snprintf(a->input[3], sizeof(a->input[3]), "#%02x%02x%02x", a->cr, a->cg, a->cb);
}

static void build_colours(struct app *a, int x, int y, int w)
{
    if (!*kv_get(a, "config-file")) {
        add(a, W_HEADING, (struct rect){ x, y, w, ROW_H }, "Colours");
        add(a, W_LABEL, (struct rect){ x, y + ROW_H, w, ROW_H }, "MellaGUI was started without a configuration file.");
        return;
    }
    struct colour cols[MAX_COLOURS];
    int n = colour_list(a, cols, MAX_COLOURS), pages = LM_MAX(1, (n + COLS_ROWS - 1) / COLS_ROWS);
    a->cpage = LM_CLAMP(a->cpage, 0, pages - 1);
    add(a, W_HEADING, (struct rect){ x, y, w, ROW_H }, "Colours");
    y += ROW_H;
    struct theme_entry th[MAX_THEMES];
    int nth = theme_names(a, th, MAX_THEMES);
    if (nth > 0) {                                  /* the theme: a name picks a whole palette; the colours below are tweaks on top of it */
        a->tpage = LM_CLAMP(a->tpage, 0, (nth - 1) / THEMES_SHOWN);
        add(a, W_LABEL, (struct rect){ x, y, 56, MENU_ROW_H }, "Theme");
        int bx = x + 60, bw = LM_MIN(104, (w - 60 - (nth > THEMES_SHOWN ? 60 : 0)) / THEMES_SHOWN - 4);
        if (nth > THEMES_SHOWN)
            menu_button(a, bx, y, 24, "<", "th-prev", 0);
        if (nth > THEMES_SHOWN)
            bx += 28;
        for (int i = a->tpage * THEMES_SHOWN; i < LM_MIN(nth, (a->tpage + 1) * THEMES_SHOWN); i++, bx += bw + 4) {
            struct widget *b = menu_button(a, bx, y, bw, th[i].name, "theme-use", i);
            b->on = th[i].active;
        }
        if (nth > THEMES_SHOWN)
            menu_button(a, bx, y, 24, ">", "th-next", 0);
        y += THEME_ROW_H;
    }
    for (int i = a->cpage * COLS_ROWS; i < LM_MIN(n, (a->cpage + 1) * COLS_ROWS); i++) {
        struct widget *sw = add(a, W_SWATCH, (struct rect){ x, y + 2, 22, MENU_ROW_H - 4 }, "");
        sw->val = (int)cols[i].value;
        struct widget *e = menu_button(a, x + 30, y, 170, cols[i].name, "col-edit", i);
        e->on = a->csel == i;
        char hex[16];
        snprintf(hex, sizeof(hex), "#%06x%s", cols[i].value, cols[i].changed ? " *" : "");
        add(a, W_LABEL, (struct rect){ x + 208, y, 100, MENU_ROW_H }, hex);
        y += MENU_ROW_H;
    }
    y = 14 + ROW_H + (nth > 0 ? THEME_ROW_H : 0) + COLS_ROWS * MENU_ROW_H + 4;
    menu_button(a, x, y, 24, "<", "col-prev", 0);
    char lbl[32];
    snprintf(lbl, sizeof(lbl), "%d / %d", a->cpage + 1, pages);
    add(a, W_LABEL, (struct rect){ x + 32, y, 80, MENU_ROW_H }, lbl);
    menu_button(a, x + 100, y, 24, ">", "col-next", 0);
    add_button(a, x + w - 150, y, 150, "Restore defaults", "col-reset")->r.h = MENU_ROW_H - 2;
    y += MENU_ROW_H + 6;
    if (a->csel < 0 || a->csel >= n) {
        add(a, W_LABEL, (struct rect){ x, y, w, 24 }, "Click a colour to change it.");
        return;
    }
    struct widget *pv = add(a, W_SWATCH, (struct rect){ x, y, 26, 26 }, "");
    pv->val = (a->cr << 16) | (a->cg << 8) | a->cb;
    struct widget *hx = add(a, W_INPUT, (struct rect){ x + 34, y, 100, 26 }, "#rrggbb");
    snprintf(hx->arg, sizeof(hx->arg), "3");
    add_button(a, x + 142, y, 64, "Apply", "col-apply")->r.h = 26;
    add_button(a, x + 212, y, 120, "Built-in", "col-default")->r.h = 26;
    add(a, W_LABEL, (struct rect){ x + 340, y + 2, w - 340, 24 }, a->cname);
    y += 30;
    static const char *const chan[3] = { "Red", "Green", "Blue" }, *const ckey[3] = { "col-r", "col-g", "col-b" };
    const int *val[3] = { &a->cr, &a->cg, &a->cb };
    for (int i = 0; i < 3; i++) {
        add_slider(a, x, y, w, chan[i], ckey[i], 0, 255, 1, "");
        a->wd[a->nw - 1].val = *val[i];
        a->wd[a->nw - 1].r.h = 22;
        y += 22;
    }
    if (a->col_err[0])
        add(a, W_LABEL, (struct rect){ x, y, w, 22 }, a->col_err);
}

/* ---- Sizes page: window frames, panel, menu, gadgets ([windows] [panel] [gadgets] [notifications]), through the metrics-* settings ---- */

#define METRIC_ROWS 8
#define MAX_METRICS 40

struct metric { char name[48]; int value, lo, hi, def; };

static int metric_list(const struct app *a, struct metric *out, int max)
{
    char text[VALUE_MAX];
    snprintf(text, sizeof(text), "%s", kv_get(a, "metrics"));
    int n = 0;
    for (char *t = strtok(text, " "); t && n < max; t = strtok(NULL, " ")) {
        char *eq = strchr(t, '=');
        if (!eq)
            continue;
        *eq = 0;
        snprintf(out[n].name, sizeof(out[n].name), "%s", t);
        if (sscanf(eq + 1, "%d:%d:%d:%d", &out[n].value, &out[n].lo, &out[n].hi, &out[n].def) == 4)
            n++;
    }
    return n;
}

/* A size by its "section.name": its index in the `metrics` setting (or -1) and its value (the pending one while the slider is held). */
static int named_metric(const struct app *a, const char *name, int *value, int *lo, int *hi)
{
    struct metric m[MAX_METRICS];
    int n = metric_list(a, m, MAX_METRICS);
    for (int i = 0; i < n; i++)
        if (!strcmp(m[i].name, name)) {
            *value = a->mpend == i ? a->mpend_val : m[i].value;
            *lo = m[i].lo;
            *hi = m[i].hi;
            return i;
        }
    return -1;
}

/* The [windows] double-click-ms size. */
static int dblclick_metric(const struct app *a, int *value, int *lo, int *hi)
{
    return named_metric(a, "windows.double-click-ms", value, lo, hi);
}

/* ---- Theme page, second part: the shape and the colours of the buttons (dock, launcher, the programs' coloured buttons) ---- */

static void build_buttons(struct app *a, int x, int y, int w)
{
    add(a, W_HEADING, (struct rect){ x, y, w, ROW_H }, "Buttons");
    y += ROW_H;
    int round = 100, lo = 0, hi = 100;
    bool have = *kv_get(a, "config-file") && named_metric(a, "windows.button-roundness", &round, &lo, &hi) >= 0;
    if (!have) {
        add(a, W_LABEL, (struct rect){ x, y, w, ROW_H }, "MellaGUI was started without a configuration file.");
        return;
    }
    add(a, W_LABEL, (struct rect){ x, y, w, ROW_H }, "Dock, launcher and the programs' buttons.");
    y += ROW_H + 2;
    const uint32_t sample[7] = { lm_theme.icon_files, lm_theme.icon_editor, lm_theme.icon_images, lm_theme.icon_camera, lm_theme.icon_code, lm_theme.icon_bell, lm_theme.icon_launcher };
    for (int i = 0; i < 7; i++) {                                   /* a preview in the shape and the colours that are set */
        struct widget *p = add(a, W_SWATCH, (struct rect){ x + i * 54, y, 44, 44 }, "");
        p->val = (int)sample[i];
        snprintf(p->arg, sizeof(p->arg), "r%d", round);
    }
    y += 52;
    struct widget *sl = add(a, W_SLIDER, (struct rect){ x, y, w, ROW_H }, "Roundness");
    snprintf(sl->key, sizeof(sl->key), "roundness");
    sl->lo = lo;
    sl->hi = hi;
    sl->step = 5;
    sl->unit = " %";
    sl->val = round;
    y += ROW_H + 6;
    const char *mode = kv_get(a, "theme-buttons");
    add(a, W_LABEL, (struct rect){ x, y, 80, ROW_H }, "Colours");
    add_radio(a, x + 80, y, 120, "Colourful", "theme-buttons", "colourful", !strcmp(mode, "colourful") || !*mode);
    add_radio(a, x + 210, y, 130, "One colour", "theme-buttons", "accent", !strcmp(mode, "accent"));
    add_radio(a, x + 350, y, 90, "Grey", "theme-buttons", "grey", !strcmp(mode, "grey"));
    y += ROW_H + 4;
    struct colour cols[MAX_COLOURS];
    int n = colour_list(a, cols, MAX_COLOURS);
    for (int i = 0; i < n; i++)
        if (!strcmp(cols[i].name, "button-accent")) {
            struct widget *sw = add(a, W_SWATCH, (struct rect){ x + 80, y + 2, 22, ROW_H - 4 }, "");
            sw->val = (int)cols[i].value;
            char hex[40];
            snprintf(hex, sizeof(hex), "#%06x%s", cols[i].value, cols[i].changed ? " *" : "");
            add(a, W_LABEL, (struct rect){ x + 110, y, 90, ROW_H }, hex);
            add_button(a, x + 210, y, 230, "Choose the one colour...", "theme-accent");
            add(a, W_LABEL, (struct rect){ x, y, 80, ROW_H }, "Accent");
            break;
        }
    y += ROW_H + 8;
    add(a, W_LABEL, (struct rect){ x, y, w, ROW_H }, "One colour keeps each button's lightness.");
}

/* ---- the Font part of the Themes page ------------------------------------------------------------------------------------ */

#define FONTS_MAX 64
#define FONTS_ROWS 5

static char g_fonts[FONTS_MAX][LM_FONT_PATH_MAX];       /* the monospace fonts found, looked for once (-1 = not yet) */
static int g_nfonts = -1;

static void font_label(const char *path, char *out, size_t n)
{
    if (!strcmp(path, "bitmap")) {
        snprintf(out, n, "the built-in pixel font");
        return;
    }
    const char *b = strrchr(path, '/');
    snprintf(out, n, "%s", b ? b + 1 : path);
    size_t len = strlen(out);
    if (len > 4 && !strcasecmp(out + len - 4, ".ttf"))
        out[len - 4] = 0;
}

static void build_font(struct app *a, int x, int y, int w)
{
    add(a, W_HEADING, (struct rect){ x, y, w, ROW_H }, "Font");
    y += ROW_H;
    if (!*kv_get(a, "config-file")) {
        add(a, W_LABEL, (struct rect){ x, y, w, ROW_H }, "MellaGUI was started without a configuration file.");
        return;
    }
    if (g_nfonts < 0)
        g_nfonts = lm_font_list(g_fonts, FONTS_MAX);
    char name[48], line[64];
    font_label(kv_get(a, "font-name"), name, sizeof(name));
    snprintf(line, sizeof(line), "In use: %.50s", name);
    add(a, W_LABEL, (struct rect){ x, y, w, ROW_H }, line);
    y += ROW_H;
    add(a, W_LABEL, (struct rect){ x, y, w, ROW_H }, "AaBbCc ÕÄÖÜ õäöü Šš Žž 0123456789 {}[]");
    y += ROW_H + 4;
    if (!strcmp(kv_get(a, "font-locked"), "1")) {
        add(a, W_LABEL, (struct rect){ x, y, w, ROW_H }, "MELLAGUI_FONT is set: it decides, not this page.");
        y += ROW_H;
    }
    const char *cur = kv_get(a, "font-file");
    add_radio(a, x, y, 130, "Automatic", "font-pick", "auto", !strcmp(cur, "auto") || !*cur);
    add_radio(a, x + 140, y, 200, "Built-in pixel font", "font-pick", "bitmap", !strcmp(cur, "bitmap"));
    y += ROW_H + 4;
    const char *ui = kv_get(a, "font-ui");
    add(a, W_LABEL, (struct rect){ x, y, 70, ROW_H }, "Labels");
    add_radio(a, x + 70, y, 150, "Proportional", "font-ui", "auto", strcmp(ui, "mono") != 0);
    add_radio(a, x + 230, y, 140, "Monospace", "font-ui", "mono", !strcmp(ui, "mono"));
    y += ROW_H + 4;
    int pages = LM_MAX(1, (g_nfonts + FONTS_ROWS - 1) / FONTS_ROWS);
    a->fpage = LM_CLAMP(a->fpage, 0, pages - 1);
    if (g_nfonts == 0)
        add(a, W_LABEL, (struct rect){ x, y, w, ROW_H }, "No monospace TrueType font is installed.");
    for (int i = a->fpage * FONTS_ROWS; i < LM_MIN(g_nfonts, (a->fpage + 1) * FONTS_ROWS); i++, y += ROW_H) {
        char idx[16];
        snprintf(idx, sizeof(idx), "#%d", i);
        font_label(g_fonts[i], name, sizeof(name));
        add_radio(a, x, y, LM_MIN(w, 340), name, "font-pick", idx, !strcmp(cur, g_fonts[i]));
    }
    if (pages > 1) {
        y += 4;
        add_button(a, x, y, 40, "<", "font-prev");
        add_button(a, x + 48, y, 40, ">", "font-next");
        snprintf(line, sizeof(line), "%d-%d of %d", a->fpage * FONTS_ROWS + 1, LM_MIN(g_nfonts, (a->fpage + 1) * FONTS_ROWS), g_nfonts);
        add(a, W_LABEL, (struct rect){ x + 100, y, w - 100, ROW_H }, line);
    }
}

static void dock_flush(struct app *a);

static void metric_flush(struct app *a)
{
    dock_flush(a);
    if (a->mpend < 0)
        return;
    struct metric m[MAX_METRICS];
    int n = metric_list(a, m, MAX_METRICS);
    if (a->mpend < n) {
        char v[96];
        snprintf(v, sizeof(v), "%s\x1f%d", m[a->mpend].name, a->mpend_val);
        send_set(a, "metrics-set", v);
    }
    a->mpend = -1;
}

static void build_sizes(struct app *a, int x, int y, int w)
{
    add(a, W_HEADING, (struct rect){ x, y, w, ROW_H }, "Sizes in pixels");
    y += ROW_H;
    if (!*kv_get(a, "config-file")) {
        add(a, W_LABEL, (struct rect){ x, y, w, ROW_H }, "MellaGUI was started without a configuration file.");
        return;
    }
    struct metric m[MAX_METRICS];
    int n = metric_list(a, m, MAX_METRICS), pages = LM_MAX(1, (n + METRIC_ROWS - 1) / METRIC_ROWS);
    a->mpage = LM_CLAMP(a->mpage, 0, pages - 1);
    for (int i = a->mpage * METRIC_ROWS; i < LM_MIN(n, (a->mpage + 1) * METRIC_ROWS); i++) {
        struct widget *sl = add(a, W_SLIDER, (struct rect){ x, y, w, 26 }, m[i].name);
        snprintf(sl->key, sizeof(sl->key), "met-%d", i);
        sl->lo = m[i].lo;
        sl->hi = m[i].hi;
        sl->step = 1;
        sl->label_w = 200;
        int v = a->mpend == i ? a->mpend_val : m[i].value;
        sl->val = v;
        sl->unit = v != m[i].def ? " *" : "";
        y += 26;
    }
    y = 14 + ROW_H + METRIC_ROWS * 26 + 4;
    menu_button(a, x, y, 24, "<", "met-prev", 0);
    char lbl[32];
    snprintf(lbl, sizeof(lbl), "%d / %d", a->mpage + 1, pages);
    add(a, W_LABEL, (struct rect){ x + 32, y, 80, MENU_ROW_H }, lbl);
    menu_button(a, x + 100, y, 24, ">", "met-next", 0);
    add_button(a, x + w - 150, y, 150, "Restore defaults", "met-reset")->r.h = MENU_ROW_H - 2;
    y += MENU_ROW_H + 6;
    add(a, W_LABEL, (struct rect){ x, y, w, 24 }, "Applied when you let go. * = not the built-in size.");
}

static void build_sound_output(struct app *a, int x, int y, int w)
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
    /* 4. the start-up chime */
    y += 4;
    add_check(a, x, y, w - 110, "Play the start-up chime", "startup-sound", kv_int(a, "startup-sound", 1) != 0);
    struct widget *pl = add_button(a, x + w - 100, y + 2, 84, "Play now", "startup-sound-play");
    pl->r.h = ROW_H - 6;
}

/* The microphone: which input applications record from, its level and mute. */
static void build_sound_mic(struct app *a, int x, int y, int w)
{
    add(a, W_HEADING, (struct rect){ x, y, w, ROW_H }, "Microphone");
    y += ROW_H;
    struct entry ents[8];
    int shown = pick_entries(kv_get(a, "sources"), 2, ents);
    if (!shown) {
        const char *why = kv_get(a, "sinks-status");
        add(a, W_LABEL, (struct rect){ x, y, w, ROW_H }, !strcmp(why, "ok") ? "No microphone found." : "No sound server answers (see the Output part).");
        return;
    }
    const char *cur = kv_get(a, "source");
    for (int i = 0; i < shown; i++) {
        add_radio(a, x, y, w, *ents[i].desc ? ents[i].desc : ents[i].name, "source", ents[i].name, !strcmp(cur, ents[i].name));
        y += ROW_H;
    }
    y += 6;
    add_slider(a, x, y, w, "Input level", "source-volume", 0, 100, 5, " %");
    y += ROW_H;
    add_check(a, x, y, w, "Muted", "source-mute", kv_int(a, "source-mute", 0) != 0);
}

static void build_sound(struct app *a, int x, int y, int w)
{
    static const char *const names[2] = { "Output", "Microphone" };
    if (a->sound_tab == 1)
        build_sound_mic(a, x, y, w);
    else
        build_sound_output(a, x, y, w);
    build_part_tabs(a, x, y, w, "sound-tab", names, 2, a->sound_tab);
}

static void attach_help(struct app *a, int px, int pw);

static void build_input(struct app *a, int x, int y, int w)
{
    static const char *const names[2] = { "Keyboard", "Pointer" };
    if (a->input_tab == 1)
        build_pointer(a, x, y, w);
    else
        build_keyboard(a, x, y, w);
    build_part_tabs(a, x, y, w, "input-tab", names, 2, a->input_tab);
}

static void build_theme(struct app *a, int x, int y, int w)
{
    static const char *const names[4] = { "Font", "Colours", "Buttons", "Sizes" };  /* Font (part 3) stands first: the others keep their places */
    if (a->theme_tab == 3)
        build_font(a, x, y, w);
    else if (a->theme_tab == 2)
        build_sizes(a, x, y, w);
    else if (a->theme_tab == 1)
        build_buttons(a, x, y, w);
    else
        build_colours(a, x, y, w);
    build_part_tabs(a, x, y, w, "theme-tab", names, 4, (a->theme_tab + 1) % 4);
}

static void build(struct app *a)
{
    a->nw = 0;
    int pitch = LM_MIN(36, (a->win.bh - 16) / N_PAGES);                 /* all tabs have to fit under each other */
    for (int i = 0; i < N_PAGES; i++) {
        struct widget *t = add(a, W_TAB, (struct rect){ 8, 12 + i * pitch, SIDE_W - 16, pitch - 4 }, PAGE_NAMES[i]);
        snprintf(t->key, sizeof(t->key), "page");
        snprintf(t->arg, sizeof(t->arg), "%d", i);
        t->on = (int)a->page == i;
    }
    int x = SIDE_W + 20, y = 14, w = a->win.bw - x - 20;
    if (w < 200)
        w = 200;
    switch (a->page) {
    case PAGE_INPUT: build_input(a, x, y, w); break;
    case PAGE_ACCESS: build_access(a, x, y, w); break;
    case PAGE_DISPLAY: build_display(a, x, y, w); break;
    case PAGE_SOUND: build_sound(a, x, y, w); break;
    case PAGE_SECURITY: build_security(a, x, y, w); break;
    case PAGE_SAVER: build_saver(a, x, y, w); break;
    case PAGE_CONFIG: build_config(a, x, y, w); break;
    case PAGE_KEYS: build_keys(a, x, y, w); break;
    case PAGE_COLOURS: build_theme(a, x, y, w); break;
    case PAGE_DOCK: build_dock(a, x, y, w); break;
    case PAGE_CLOCK: build_clock(a, x, y, w); break;
    case PAGE_NIGHT: build_night(a, x, y, w); break;
    default: build_desktop(a, x, y, w); break;
    }
    attach_help(a, x, w);
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
    int label_w = w->label_w ? w->label_w : 150, value_w = 70;
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

/* ---- help: the small "?" circle at the end of a setting opens a modal card ------------------------------------------------------- */

static void fill(struct app *a, struct rect r, uint32_t color);
static void draw_text(struct app *a, int x, int y, const char *s, uint32_t color, int max_w);

#define HELP_D 18                                       /* diameter of the circle */
#define HELP_LINES 48
#define HELP_COLS 100

/* The key under which a widget's help is kept ("@Heading" for a heading), or false for a widget that has none. */
static bool help_key(const struct widget *w, char *key, size_t n, const char **arg)
{
    *arg = NULL;
    switch (w->kind) {
    case W_HEADING:
        if (!strncmp(w->text, "Current resolution", 18) || !strcmp(w->text, "Resolution"))
            snprintf(key, n, "@Resolution");
        else
            snprintf(key, n, "@%.62s", w->text);
        return true;
    case W_CHECK:
    case W_SLIDER:
    case W_RADIO:
    case W_FIELD:
        if (!strcmp(w->key, "layout-enable") || !strcmp(w->key, "layout"))          /* the layouts' rows have their own buttons at the end: the heading explains them */
            return false;
        snprintf(key, n, "%s", w->key);
        *arg = w->arg;
        return w->key[0] != 0;
    case W_PASS:
        snprintf(key, n, "%s", w->key);
        *arg = w->arg;
        return true;
    default:
        return false;
    }
}

/* Does something have a help text: a table entry, or one of the Sizes sliders ("met-N")? */
static bool help_exists(const char *key, const char *arg)
{
    if (!strcmp(key, "@Resolution"))
        key = "resolution";
    return settings_help_known(key, arg) || !strncmp(key, "met-", 4);
}

/* Fills the modal's title and text for `key` (a Sizes slider gets its text from the sizes table). */
static void help_open(struct app *a, const char *key, const char *arg)
{
    const char *title = NULL, *text = NULL;
    if (!strncmp(key, "met-", 4)) {
        struct metric m[MAX_METRICS];
        int n = metric_list(a, m, MAX_METRICS), i = atoi(key + 4);
        if (i >= 0 && i < n) {
            char sec[32], nm[48];
            const char *dot = strchr(m[i].name, '.');
            if (dot) {
                snprintf(sec, sizeof(sec), "%.*s", (int)LM_MIN((size_t)(dot - m[i].name), sizeof(sec) - 1), m[i].name);
                snprintf(nm, sizeof(nm), "%s", dot + 1);
                const char *what = lm_metrics_what(sec, nm);
                snprintf(a->help_title, sizeof(a->help_title), "%s", m[i].name);
                snprintf(a->help_text, sizeof(a->help_text),
                         "%s%s.\nBuilt-in size: %d pixels; allowed: %d to %d.\nThe size is sent when you let go of the slider and the shell follows at once. "
                         "A * after the value marks a size that is not the built-in one; 'Restore defaults' brings all of them back.",
                         what && *what ? "Size of the " : "", what && *what ? what : "A size of the shell", m[i].def, m[i].lo, m[i].hi);
                a->help_scroll = 0;
                a->win.redraw = true;
                return;
            }
        }
    }
    if (!strcmp(key, "@Resolution"))
        key = "resolution";
    if (!settings_help(key, arg, &title, &text)) {
        title = "No help";
        text = "There is no help text for this one yet.";
    }
    snprintf(a->help_title, sizeof(a->help_title), "%s", title);
    snprintf(a->help_text, sizeof(a->help_text), "%s", text);
    a->help_scroll = 0;
    a->win.redraw = true;
}

static void help_close(struct app *a)
{
    a->help_title[0] = 0;
    a->win.redraw = true;
}

/* After the page is built: a "?" at the end of every setting row (and after a heading's text), the row's own widgets made shorter to make room. */
static void attach_help(struct app *a, int px, int pw)
{
    int n0 = a->nw;
    char seen[24][80];
    int nseen = 0;
    for (int i = 0; i < n0; i++) {
        const struct widget *w = &a->wd[i];
        char key[64];
        const char *arg;
        if (!help_key(w, key, sizeof(key), &arg) || !help_exists(key, arg))
            continue;
        char id[80];                                            /* one circle for a group of radios; the lock's three fields have three */
        snprintf(id, sizeof(id), "%s|%s", key, w->kind == W_PASS || w->kind == W_SLIDER ? (arg ? arg : "") : "");
        bool dup = false;
        for (int k = 0; k < nseen; k++)
            if (!strcmp(seen[k], id))
                dup = true;
        if (dup)
            continue;
        if (nseen < 24)
            snprintf(seen[nseen++], sizeof(seen[0]), "%s", id);
        struct rect hr;
        if (w->kind == W_HEADING) {
            int tw = LM_MIN(lm_font_text_width_bold(w->text), w->label_w > 0 ? w->label_w : 1 << 20) + 10;
            hr = (struct rect){ w->r.x + LM_MIN(tw, w->r.w - HELP_D - 2), w->r.y + (w->r.h - HELP_D) / 2 - 1, HELP_D, HELP_D };
        } else {
            hr = (struct rect){ px + pw - HELP_D - 2, w->r.y + (w->r.h - HELP_D) / 2, HELP_D, HELP_D };
            for (int j = 0; j < n0; j++) {                      /* what lies in that row ends before the circle */
                struct widget *o = &a->wd[j];
                if (o->kind == W_TAB || o->kind == W_BUTTON || o->r.y + o->r.h <= w->r.y || o->r.y >= w->r.y + w->r.h)
                    continue;
                if (o->r.x + o->r.w > hr.x - 6)
                    o->r.w = LM_MAX(hr.x - 6 - o->r.x, 30);
            }
        }
        if (a->nw >= MAX_WIDGETS)
            break;
        memmove(&a->wd[i + 2], &a->wd[i + 1], (size_t)(a->nw - i - 1) * sizeof(a->wd[0]));        /* the circle comes right after its row's first widget: Tab visits it there */
        a->nw++;
        struct widget *h = &a->wd[i + 1];
        memset(h, 0, sizeof(*h));
        h->kind = W_HELP;
        h->r = hr;
        snprintf(h->text, sizeof(h->text), "%s", key);
        snprintf(h->key, sizeof(h->key), "help");
        snprintf(h->arg, sizeof(h->arg), "%s", arg ? arg : "");
        i++;                                                    /* (the widgets that came after it have moved on one place) */
        n0++;
    }
}

/* Word wraps `text` into lines of at most `cols` characters; '\n' ends a paragraph (and leaves an empty line between). */
static int help_wrap(const char *text, int cols, char lines[][HELP_COLS + 1], int max)
{
    int n = 0;
    cols = LM_MIN(cols, HELP_COLS);
    for (const char *p = text; *p && n < max;) {
        size_t pl = strcspn(p, "\n");
        char para[1400];
        snprintf(para, sizeof(para), "%.*s", (int)LM_MIN(pl, sizeof(para) - 1), p);
        p += pl + (p[pl] ? 1 : 0);
        char line[HELP_COLS + 1] = "";
        for (char *tok = strtok(para, " "); tok && n < max; tok = strtok(NULL, " ")) {
            size_t ll = strlen(line);
            if (ll && lm_font_text_width(line) + lm_font_advance(' ') + lm_font_text_width(tok) > cols * LM_FONT_W) {
                snprintf(lines[n++], HELP_COLS + 1, "%s", line);
                line[0] = 0;
                ll = 0;
            }
            if (n >= max)
                break;
            snprintf(line + ll, sizeof(line) - ll, "%s%.*s", ll ? " " : "", cols, tok);
        }
        if (n < max && line[0])
            snprintf(lines[n++], HELP_COLS + 1, "%s", line);
        if (n < max && *p)
            lines[n++][0] = 0;                                  /* a gap between paragraphs */
    }
    return n;
}

struct help_geom {
    struct rect card, close;
    int cols, visible, nlines;
    char lines[HELP_LINES][HELP_COLS + 1];
};

static void help_geom(const struct app *a, struct help_geom *g)
{
    int cw = LM_MIN(a->win.bw - 40, 560);
    g->cols = LM_MAX((cw - 48) / LM_FONT_W, 20);
    g->nlines = help_wrap(a->help_text, g->cols, g->lines, HELP_LINES);
    int line_h = LM_FONT_H + 3, chrome = 24 + 28 + 14 + 30 + 20;
    int max_lines = LM_MAX((a->win.bh - 40 - chrome) / line_h, 3);
    g->visible = LM_MIN(g->nlines, max_lines);
    int ch = chrome + g->visible * line_h;
    g->card = (struct rect){ (a->win.bw - cw) / 2, (a->win.bh - ch) / 2, cw, ch };
    g->close = (struct rect){ g->card.x + (cw - 90) / 2, g->card.y + ch - 20 - 28, 90, 28 };
}

static void paint_help(struct app *a)
{
    wlwin_dim(&a->win, 0.6);                                    /* the page behind the card is dimmed */
    struct help_geom *g = calloc(1, sizeof(*g));
    if (!g)
        return;
    help_geom(a, g);
    const struct rect *c = &g->card;
    wlwin_round_fill(&a->win, c->x - 1, c->y - 1, c->w + 2, c->h + 2, 11, COL_ACCENT);
    wlwin_round_fill(&a->win, c->x, c->y, c->w, c->h, 10, COL_BG);
    int hx = c->x + 20, hy = c->y + 20;
    wlwin_round_fill(&a->win, hx, hy - 1, HELP_D, HELP_D, HELP_D / 2, COL_ACCENT);
    draw_text(a, hx + (HELP_D - lm_font_text_width("?")) / 2, hy, "?", COL_BG, 0);
    draw_text(a, hx + HELP_D + 10, hy, a->help_title, COL_TEXT, c->w - 40 - HELP_D - 10);
    fill(a, (struct rect){ c->x + 20, hy + 22, c->w - 40, 1 }, COL_KEY);
    int line_h = LM_FONT_H + 3, ty = hy + 32;
    a->help_scroll = LM_CLAMP(a->help_scroll, 0, LM_MAX(g->nlines - g->visible, 0));
    for (int i = 0; i < g->visible; i++)
        draw_text(a, c->x + 24, ty + i * line_h, g->lines[a->help_scroll + i], COL_TEXT, c->w - 48);
    if (g->nlines > g->visible) {
        char more[32];
        snprintf(more, sizeof(more), "%d / %d", a->help_scroll + 1, g->nlines - g->visible + 1);
        draw_text(a, c->x + c->w - 24 - lm_font_text_width(more), g->close.y + 6, more, COL_DIM, 0);
    }
    bool hot = a->px >= g->close.x && a->px < g->close.x + g->close.w && a->py >= g->close.y && a->py < g->close.y + g->close.h;
    wlwin_round_fill(&a->win, g->close.x, g->close.y, g->close.w, g->close.h, 14, hot ? COL_DOWN : COL_HOVER);
    draw_text(a, g->close.x + (g->close.w - lm_font_text_width("Close")) / 2, g->close.y + (g->close.h - LM_FONT_H) / 2, "Close", COL_TEXT, 0);
    free(g);
}

/* A pointer event while the card is open: a click on Close or outside the card ends it, the wheel scrolls a long text. */
static void help_pointer(struct app *a, uint32_t op, const union lm_arg *args)
{
    if (op == LM_WL_POINTER_EVT_MOTION) {
        a->px = lm_fixed_to_double(args[1].f);
        a->py = lm_fixed_to_double(args[2].f);
        a->win.redraw = true;
    } else if (op == LM_WL_POINTER_EVT_AXIS && args[1].u == 0) {
        double v = lm_fixed_to_double(args[2].f);
        a->help_scroll += v > 0 ? 2 : v < 0 ? -2 : 0;
        a->win.redraw = true;
    } else if (op == LM_WL_POINTER_EVT_BUTTON && args[2].u == 272 && args[3].u == 1) {
        struct help_geom *g = calloc(1, sizeof(*g));
        if (!g)
            return;
        help_geom(a, g);
        bool on_close = a->px >= g->close.x && a->px < g->close.x + g->close.w && a->py >= g->close.y && a->py < g->close.y + g->close.h;
        bool inside = a->px >= g->card.x && a->px < g->card.x + g->card.w && a->py >= g->card.y && a->py < g->card.y + g->card.h;
        free(g);
        if (on_close || !inside)
            help_close(a);
    }
}

static void help_key_event(struct app *a, uint32_t ks)
{
    int page = 6;
    if (ks == LM_KS_Escape || ks == LM_KS_Return || ks == LM_KS_KP_Enter || ks == ' ')
        help_close(a);
    else if (ks == LM_KS_Down)
        a->help_scroll++;
    else if (ks == LM_KS_Up)
        a->help_scroll--;
    else if (ks == LM_KS_Page_Down)
        a->help_scroll += page;
    else if (ks == LM_KS_Page_Up)
        a->help_scroll -= page;
    a->win.redraw = true;
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
    } else if (!strncmp(w->key, "met-", 4) && w->key[4] >= '0' && w->key[4] <= '9') {      /* a size: sent when the slider is let go */
        a->mpend = atoi(w->key + 4);
        a->mpend_val = v;
        a->win.redraw = true;
    } else if (!strcmp(w->key, "roundness")) {                                /* the buttons' roundness: a size, sent when the slider is let go */
        int cur, lo, hi, i = named_metric(a, "windows.button-roundness", &cur, &lo, &hi);
        if (i >= 0) {
            a->mpend = i;
            a->mpend_val = v;
            a->win.redraw = true;
        }
    } else if (!strcmp(w->key, "dblclick")) {                                 /* the double-click time: a size, sent when the slider is let go */
        int cur, lo, hi, i = dblclick_metric(a, &cur, &lo, &hi);
        if (i >= 0) {
            a->mpend = i;
            a->mpend_val = v;
            a->win.redraw = true;
        }
    } else if (!strncmp(w->key, "dk-", 3)) {                                  /* a dock option: sent when the slider is let go */
        snprintf(a->dpend, sizeof(a->dpend), "%s", w->key + 3);
        a->dpend_val = v;
        a->win.redraw = true;
    } else if (!strncmp(w->key, "col-", 4) && w->key[4] && !w->key[5]) {      /* col-r / col-g / col-b: only the working colour */
        *(w->key[4] == 'r' ? &a->cr : w->key[4] == 'g' ? &a->cg : &a->cb) = v;
        colour_set_hex(a);
        a->col_err[0] = 0;
        a->win.redraw = true;
    } else if (time_slider(w)) {
        char t[16];
        snprintf(t, sizeof(t), "%02d:%02d", v / 60, v % 60);
        send_set(a, w->key, t);
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
static void launch_sibling(const char *name, const char *arg, const char *arg2)
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
            execl(path, name, arg, arg2, (char *)NULL);
        }
    }
    execlp(name, name, arg, arg2, (char *)NULL);                   /* installed on the PATH */
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
    case W_HELP:
        help_open(a, w->text, w->arg);
        break;
    case W_TAB:
        a->page = (enum page)atoi(w->arg);
        a->focus = idx;
        a->win.redraw = true;
        if (a->page == PAGE_SOUND)
            send_set(a, "audio-rescan", "1");           /* ask `pactl` again: devices come and go */
        if (a->page == PAGE_SECURITY) {
            sec_wipe(a);
            a->sec_minutes = kv_int(a, "lock-timeout", 0);
            a->sec_blank = kv_int(a, "lock-blank", 1);
            a->sec_start = kv_int(a, "lock-start", 0) != 0;
            a->sec_vt = kv_int(a, "lock-vt", 1) != 0;
        }
        break;
    case W_CHECK:
        if (!strcmp(w->key, "sec-start")) {
            a->sec_start = !a->sec_start;
        } else if (!strcmp(w->key, "sec-vt")) {
            a->sec_vt = !a->sec_vt;
        } else if (!strcmp(w->key, "dock-enabled") || !strcmp(w->key, "dock-autohide")) {
            char v[32];
            snprintf(v, sizeof(v), "%s\x1f%s", w->key + 5, w->on ? "no" : "yes");
            send_set(a, "dock-set", v);
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
        if (!strcmp(w->key, "sec-kind")) {
            a->sec_pin = !strcmp(w->arg, "pin");
        } else if (!strcmp(w->key, "font-pick")) {                      /* a font of the list goes by its number: a path does not fit the widget */
            int i = w->arg[0] == '#' ? atoi(w->arg + 1) : -1;
            if (w->arg[0] != '#')
                send_set(a, "font-use", w->arg);
            else if (i >= 0 && i < g_nfonts)
                send_set(a, "font-use", g_fonts[i]);
        } else if (!strcmp(w->key, "dock-bg")) {                        /* the plate under the logos: a dock option */
            char v[48];
            snprintf(v, sizeof(v), "icon-background\x1f%s", w->arg);
            send_set(a, "dock-set", v);
        } else
            send_set(a, w->key, w->arg);
        break;
    case W_BUTTON:
        if (!strcmp(w->key, "sec-apply") || !strcmp(w->key, "sec-remove")) {
            sec_send(a, !strcmp(w->key, "sec-remove"));
        } else if (!strcmp(w->key, "sec-lock")) {
            send_set(a, "lock-now", "1");
        } else if (!strcmp(w->key, "launch")) {
            launch_sibling(w->arg, NULL, NULL);
        } else if (!strcmp(w->key, "met-prev") || !strcmp(w->key, "met-next")) {
            a->mpage = LM_MAX(0, a->mpage + (!strcmp(w->key, "met-next") ? 1 : -1));
        } else if (!strcmp(w->key, "met-reset")) {
            send_set(a, "metrics-reset", "1");
            a->mpend = -1;
        } else if (!strcmp(w->key, "col-edit")) {
            struct colour cols[MAX_COLOURS];
            int n = colour_list(a, cols, MAX_COLOURS), i = atoi(w->arg);
            if (i >= 0 && i < n) {
                a->csel = i;
                snprintf(a->cname, sizeof(a->cname), "%s", cols[i].name);
                a->cr = (int)(cols[i].value >> 16) & 255;
                a->cg = (int)(cols[i].value >> 8) & 255;
                a->cb = (int)cols[i].value & 255;
                colour_set_hex(a);
                a->col_err[0] = 0;
            }
        } else if (!strcmp(w->key, "theme-use")) {
            struct theme_entry th[MAX_THEMES];
            int n = theme_names(a, th, MAX_THEMES), i = atoi(w->arg);
            if (i >= 0 && i < n) {
                send_set(a, "theme-use", th[i].name);
                a->csel = -1;
            }
        } else if (!strcmp(w->key, "th-prev") || !strcmp(w->key, "th-next")) {
            a->tpage = LM_MAX(0, a->tpage + (!strcmp(w->key, "th-next") ? 1 : -1));
        } else if (!strcmp(w->key, "col-prev") || !strcmp(w->key, "col-next")) {
            a->cpage = LM_MAX(0, a->cpage + (!strcmp(w->key, "col-next") ? 1 : -1));
        } else if (!strcmp(w->key, "col-apply")) {
            colour_apply(a);
        } else if (!strcmp(w->key, "col-default") && a->csel >= 0) {
            send_set(a, "theme-del", a->cname);
            a->csel = -1;
        } else if (!strcmp(w->key, "col-reset")) {
            send_set(a, "theme-reset", "1");
            a->csel = -1;
        } else if (!strcmp(w->key, "keys-edit")) {
            char key[16];
            snprintf(key, sizeof(key), "keys-row-%d", atoi(w->arg));
            const char *v = kv_get(a, key), *us = strchr(v, 0x1f);
            if (us) {
                snprintf(a->kcombo, sizeof(a->kcombo), "%.*s", (int)LM_MIN((size_t)(us - v), sizeof(a->kcombo) - 1), v);
                snprintf(a->ksel, sizeof(a->ksel), "%s", a->kcombo);
                snprintf(a->input[2], sizeof(a->input[2]), "%s", us + 1);
                a->keys_err[0] = 0;
            }
        } else if (!strcmp(w->key, "keys-del")) {
            char key[16];
            snprintf(key, sizeof(key), "keys-row-%d", atoi(w->arg));
            const char *v = kv_get(a, key), *us = strchr(v, 0x1f);
            if (us) {
                char combo[64];
                snprintf(combo, sizeof(combo), "%.*s", (int)LM_MIN((size_t)(us - v), sizeof(combo) - 1), v);
                send_set(a, "keys-del", combo);
            }
            a->kcombo[0] = a->ksel[0] = a->input[2][0] = 0;
        } else if (!strcmp(w->key, "keys-prev") || !strcmp(w->key, "keys-next")) {
            send_set_int(a, "keys-page", LM_MAX(0, kv_int(a, "keys-page", 0) + (!strcmp(w->key, "keys-next") ? 1 : -1)));
        } else if (!strcmp(w->key, "keys-record")) {
            a->recording = true;
            send_set(a, "keys-capture", "1");
        } else if (!strcmp(w->key, "keys-act-prev") || !strcmp(w->key, "keys-act-next")) {
            keys_cycle_action(a, !strcmp(w->key, "keys-act-next") ? 1 : -1);
        } else if (!strcmp(w->key, "keys-put")) {
            keys_put(a);
        } else if (!strcmp(w->key, "keys-reset")) {
            send_set(a, "keys-reset", "1");
            a->kcombo[0] = a->ksel[0] = a->input[2][0] = 0;
        } else if (!strcmp(w->key, "dock-sub")) {
            a->dsub = atoi(w->arg);
        } else if (!strcmp(w->key, "dock-prev") || !strcmp(w->key, "dock-next")) {
            a->dpage = LM_MAX(0, a->dpage + (!strcmp(w->key, "dock-next") ? 1 : -1));
        } else if (!strcmp(w->key, "dock-edit")) {
            char key[16];
            snprintf(key, sizeof(key), "dock-%d", atoi(w->arg));
            const char *v = kv_get(a, key), *us1 = strchr(v, 0x1f), *us2 = us1 ? strchr(us1 + 1, 0x1f) : NULL;
            if (us2) {
                snprintf(a->input[4], sizeof(a->input[4]), "%.*s", (int)LM_MIN((size_t)(us1 - v), sizeof(a->input[4]) - 1), v);
                snprintf(a->input[5], sizeof(a->input[5]), "%.*s", (int)LM_MIN((size_t)(us2 - us1 - 1), sizeof(a->input[5]) - 1), us1 + 1);
                snprintf(a->input[6], sizeof(a->input[6]), "%s", us2 + 1);
                a->dock_sel = atoi(w->arg);
                a->dock_err[0] = 0;
            }
        } else if (!strcmp(w->key, "dock-up") || !strcmp(w->key, "dock-down")) {
            char v[24];
            snprintf(v, sizeof(v), "%d" MENU_US "%d", atoi(w->arg), !strcmp(w->key, "dock-up") ? -1 : 1);
            send_set(a, "dock-move", v);
            dock_clear_form(a);
        } else if (!strcmp(w->key, "dock-del")) {
            send_set(a, "dock-del", w->arg);
            dock_clear_form(a);
        } else if (!strcmp(w->key, "dock-reset")) {
            send_set(a, "dock-reset", "1");
            dock_clear_form(a);
            a->dpend[0] = 0;
        } else if (!strcmp(w->key, "sound-tab")) {
            a->sound_tab = atoi(w->arg) == 1 ? 1 : 0;
            if (a->sound_tab == 1)
                send_set(a, "audio-rescan", "1");                         /* ask again: microphones come and go */
            a->focus = -1;
            a->win.redraw = true;
        } else if (!strcmp(w->key, "input-tab")) {
            a->input_tab = atoi(w->arg) == 1 ? 1 : 0;
            a->focus = -1;
            a->win.redraw = true;
        } else if (!strcmp(w->key, "display-tab")) {
            a->display_tab = atoi(w->arg) == 1 ? 1 : 0;
            a->focus = -1;
            a->win.redraw = true;
        } else if (!strcmp(w->key, "theme-tab")) {
            a->theme_tab = (LM_CLAMP(atoi(w->arg), 0, 3) + 3) % 4;       /* the buttons are Font, Colours, Buttons, Sizes */
            a->focus = -1;
            a->win.redraw = true;
        } else if (!strcmp(w->key, "font-prev") || !strcmp(w->key, "font-next")) {
            a->fpage += !strcmp(w->key, "font-next") ? 1 : -1;
            a->focus = -1;
            a->win.redraw = true;
        } else if (!strcmp(w->key, "theme-accent")) {                     /* the one colour: the Colours part with it selected */
            struct colour cols[MAX_COLOURS];
            int n = colour_list(a, cols, MAX_COLOURS);
            a->theme_tab = 0;
            for (int i = 0; i < n; i++)
                if (!strcmp(cols[i].name, "button-accent")) {
                    a->csel = i;
                    a->cpage = i / COLS_ROWS;
                    snprintf(a->cname, sizeof(a->cname), "%.31s", cols[i].name);
                    a->cr = (int)(cols[i].value >> 16) & 255;
                    a->cg = (int)(cols[i].value >> 8) & 255;
                    a->cb = (int)cols[i].value & 255;
                    colour_set_hex(a);
                    a->col_err[0] = 0;
                }
            a->focus = -1;
            a->win.redraw = true;
        } else if (!strcmp(w->key, "launcher-defaults")) {
            send_set(a, "launcher-defaults", "1");
        } else if (!strcmp(w->key, "dock-put")) {
            dock_put(a);
        } else if (!strcmp(w->key, "config-edit")) {
            launch_sibling("mellagui-edit", w->arg, NULL);
        } else if (!strcmp(w->key, "wall-browse")) {
            launch_sibling("mellagui-files", "--choose", "wallpaper");
        } else if (!strcmp(w->key, "config-reload") || !strcmp(w->key, "config-create") || !strcmp(w->key, "saver-now") || !strcmp(w->key, "startup-sound-play")) {
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

/* The one-character buttons of the lists (^ v x < >) are drawn as small arrows and a cross instead of letters. */
static bool draw_glyph_button(struct app *a, const struct rect *r, const char *text, uint32_t color)
{
    if (!text || !text[0] || text[1] || !strchr("^vx<>", text[0]))
        return false;
    int cx = r->x + r->w / 2, cy = r->y + r->h / 2, n = 4;
    for (int i = -n; i <= n; i++) {
        int k = i < 0 ? -i : i;
        switch (text[0]) {
        case '^': fill(a, (struct rect){ cx + i - 1, cy - n / 2 + k - 1, 2, 2 }, color); break;      /* a chevron pointing up ... */
        case 'v': fill(a, (struct rect){ cx + i - 1, cy + n / 2 - k - 1, 2, 2 }, color); break;      /* ... down */
        case '<': fill(a, (struct rect){ cx - n / 2 + k - 1, cy + i - 1, 2, 2 }, color); break;      /* ... left */
        case '>': fill(a, (struct rect){ cx + n / 2 - k - 1, cy + i - 1, 2, 2 }, color); break;      /* ... right */
        default:                                                                                     /* a cross */
            fill(a, (struct rect){ cx + i - 1, cy + i - 1, 2, 2 }, color);
            fill(a, (struct rect){ cx + i - 1, cy - i - 1, 2, 2 }, color);
            break;
        }
    }
    return true;
}

static void paint_widget(struct app *a, int idx)
{
    const struct widget *w = &a->wd[idx];
    bool hot = idx == a->hover, focus = idx == a->focus;
    const struct rect *r = &w->r;
    switch (w->kind) {
    case W_HEADING:
        wlwin_text_bold(&a->win, r->x, text_y(r), w->text, 1, COL_ACCENT, w->label_w > 0 ? w->label_w : r->w);
        fill(a, (struct rect){ r->x, r->y + r->h - 2, r->w, 1 }, COL_KEY);
        break;
    case W_LABEL:
        draw_text(a, r->x, text_y(r), w->text, COL_DIM, r->w);
        break;
    case W_NOTE: {                                              /* fine print: smaller type, wrapped to the width, as many lines as the rectangle holds */
        const char *p = w->note ? w->note : w->text;
        int ny = r->y, maxpx = (int)((double)r->w / NOTE_SIZE);
        while (*p && ny + NOTE_LINE_H <= r->y + r->h + 2) {
            int n = lm_font_fit(p, maxpx), cut = n;
            if (n <= 0)
                break;
            if (p[n]) {                                         /* does not end here: break at the last blank */
                int k = n;
                while (k > 0 && p[k] != ' ')
                    k--;
                if (k > 0)
                    cut = k;
            }
            char line[256];
            snprintf(line, sizeof(line), "%.*s", cut < 255 ? cut : 255, p);
            wlwin_text_small(&a->win, r->x, ny, line, NOTE_SIZE, COL_DIM, r->w);
            p += cut;
            while (*p == ' ')
                p++;
            ny += NOTE_LINE_H;
        }
        break;
    }
    case W_HELP:                                                /* the small "?" circle */
        wlwin_round_fill(&a->win, r->x, r->y, r->w, r->h, r->w / 2, hot ? COL_ACCENT : COL_KEY);
        draw_text(a, r->x + (r->w - lm_font_text_width("?")) / 2, r->y + (r->h - LM_FONT_H) / 2, "?", hot ? COL_BG : COL_TEXT, 0);
        if (focus)
            outline(a, (struct rect){ r->x - 2, r->y - 2, r->w + 4, r->h + 4 }, COL_ACCENT);
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
    case W_RADIO: {                                             /* a round radio button: a ring, with a dot in it when it is the chosen one */
        const int d = 16;
        int cy = r->y + (r->h - d) / 2;
        wlwin_round_fill(&a->win, r->x, cy, d, d, d / 2, w->on || focus ? COL_ACCENT : (hot ? COL_TEXT : COL_DIM));
        wlwin_round_fill(&a->win, r->x + 2, cy + 2, d - 4, d - 4, (d - 4) / 2, hot ? COL_HOVER : COL_FIELD);
        if (w->on)
            wlwin_round_fill(&a->win, r->x + 5, cy + 5, d - 10, d - 10, (d - 10) / 2, COL_ACCENT);
        draw_text(a, r->x + 26, text_y(r), w->text, w->on ? COL_TEXT : COL_DIM, r->w - 26);
        if (focus)
            outline(a, (struct rect){ r->x - 2, r->y + 2, r->w, r->h - 4 }, COL_ACCENT);
        break;
    }
    case W_BUTTON:
        fill(a, *r, w->on && !strcmp(w->key, "dbl-try") ? lm_theme.ok : a->press == idx && hot ? COL_DOWN : (hot ? COL_HOVER : COL_KEY));
        if (!draw_glyph_button(a, r, w->text, w->text[0] == 'x' ? lm_theme.alert : COL_TEXT))
            draw_text(a, r->x + (r->w - lm_font_text_width(w->text)) / 2, text_y(r), w->text, COL_TEXT, 0);
        if (focus)
            outline(a, *r, COL_ACCENT);
        break;
    case W_SLIDER: {
        struct rect t = slider_track(w);
        draw_text(a, r->x, text_y(r), w->text, COL_TEXT, (w->label_w ? w->label_w : 150) - 2);
        fill(a, t, COL_KEY);
        int kx = t.x + (int)((double)(w->val - w->lo) / (double)(w->hi - w->lo) * t.w);
        fill(a, (struct rect){ t.x, t.y, kx - t.x, t.h }, COL_ACCENT);
        struct rect knob = { kx - 5, r->y + 4, 10, r->h - 8 };
        fill(a, knob, a->drag == idx ? COL_DOWN : (hot ? COL_HOVER : COL_TEXT));
        if (focus)
            outline(a, (struct rect){ knob.x - 2, knob.y - 2, knob.w + 4, knob.h + 4 }, COL_ACCENT);
        char v[24];
        if (time_slider(w))
            snprintf(v, sizeof(v), "%02d:%02d", w->val / 60, w->val % 60);
        else
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
        if (focus && a->wall_edited) {
            wltext_paint(&a->win, r->x + 6, r->y + (r->h - LM_FONT_H) / 2, r->w - 12, a->wall, true, COL_TEXT, COL_HOVER, COL_ACCENT);
        } else {
            char shown[240];
            snprintf(shown, sizeof(shown), "%s%s", a->wall_edited ? a->wall : kv_get(a, "wallpaper"), focus ? "_" : "");
            draw_text(a, r->x + 6, r->y + (r->h - LM_FONT_H) / 2, shown, COL_TEXT, r->w - 12);
        }
        break;
    }
    case W_SWATCH:
        if (w->arg[0] == 'f') {                                           /* a keyboard layout's country flag */
            struct lm_flag_rect fr[LM_FLAG_MAX];
            int n = lm_flag_rects(w->arg + 1, fr);
            for (int k = 0; k < n; k++)
                fill(a, (struct rect){ r->x + fr[k].x, r->y + fr[k].y, fr[k].w, fr[k].h }, fr[k].rgb);
            outline(a, *r, COL_DIM);
            break;
        }
        if (w->arg[0] == 'r') {                                           /* a preview button: as round as the setting says (100 = a circle) */
            int pct = atoi(w->arg + 1);
            wlwin_round_fill(&a->win, r->x, r->y, r->w, r->h, LM_MIN(r->w, r->h) / 2 * pct / 100, (uint32_t)w->val & 0xffffff);
            break;
        }
        fill(a, *r, (uint32_t)w->val & 0xffffff);
        outline(a, *r, COL_DIM);
        break;
    case W_INPUT: {
        const char *buf = a->input[atoi(w->arg)];
        fill(a, *r, COL_FIELD);
        outline(a, *r, focus ? COL_ACCENT : COL_KEY);
        if (focus) {
            wltext_paint(&a->win, r->x + 6, r->y + (r->h - LM_FONT_H) / 2, r->w - 12, buf, true, COL_TEXT, COL_HOVER, COL_ACCENT);
        } else {
            draw_text(a, r->x + 6, r->y + (r->h - LM_FONT_H) / 2, *buf ? buf : w->text, *buf ? COL_TEXT : COL_DIM, r->w - 12);
        }
        break;
    }
    case W_TEXT: {
        draw_text(a, r->x, r->y + 2, w->text, COL_DIM, r->w);
        struct rect f = { r->x, r->y + 20, r->w, 28 };
        fill(a, f, COL_FIELD);
        outline(a, f, focus ? COL_ACCENT : COL_KEY);
        if (focus)
            wltext_paint(&a->win, f.x + 6, f.y + (f.h - LM_FONT_H) / 2, f.w - 12, a->test, true, COL_TEXT, COL_HOVER, COL_ACCENT);
        else
            draw_text(a, f.x + 6, f.y + (f.h - LM_FONT_H) / 2, a->test, COL_TEXT, f.w - 12);
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
    if (a->help_title[0])
        paint_help(a);
}

/* ---- protocol events -------------------------------------------------------------------------------------- */

static void settings_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)c; (void)id;
    struct app *a = data;
    if (op == LM_MELLAGUI_SETTINGS_V1_EVT_VALUE) {
        bool changed = strcmp(kv_get(a, args[0].s), args[1].s) != 0;
        kv_set(a, args[0].s, args[1].s);
        if (changed && !strcmp(args[0].s, "font-name") && args[1].s[0]) {    /* ... and the font it chooses */
            lm_font_use(args[1].s);
            if (*kv_get(a, "font-ui-name"))
                lm_font_use_ui(kv_get(a, "font-ui-name"));
            a->win.redraw = true;
        }
        if (changed && !strcmp(args[0].s, "font-ui-name") && args[1].s[0]) {
            lm_font_use_ui(args[1].s);
            a->win.redraw = true;
        }
        if (changed && !strcmp(args[0].s, "theme-colors")) {                 /* the panel wears the palette it edits */
            struct colour cols[MAX_COLOURS];
            char hex[16];
            int n = colour_list(a, cols, MAX_COLOURS);
            for (int i = 0; i < n; i++) {
                snprintf(hex, sizeof(hex), "#%06x", cols[i].value);
                lm_theme_set(cols[i].name, hex);
            }
        }
        if (changed && !strcmp(args[0].s, "lock-timeout"))
            a->sec_minutes = atoi(args[1].s);
        else if (changed && !strcmp(args[0].s, "lock-blank"))
            a->sec_blank = atoi(args[1].s);
        else if (changed && !strcmp(args[0].s, "lock-vt"))
            a->sec_vt = atoi(args[1].s) != 0;
        else if (changed && !strcmp(args[0].s, "lock-start"))
            a->sec_start = atoi(args[1].s) != 0;
    } else if (op == LM_MELLAGUI_SETTINGS_V1_EVT_DONE) {
        if (a->recording && !strcmp(kv_get(a, "keys-capture"), "0")) {      /* the compositor has recorded a combination (or Escape cancelled) */
            a->recording = false;
            if (*kv_get(a, "keys-captured")) {
                snprintf(a->kcombo, sizeof(a->kcombo), "%s", kv_get(a, "keys-captured"));
                a->keys_err[0] = 0;
            }
        }
        a->win.redraw = true;
    }
}

static char *field_buffer(struct app *a, const struct widget *f, size_t *max);

static void pointer_event(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
    struct app *a = w->data;
    build(a);
    if (a->help_title[0] && op != LM_WL_POINTER_EVT_ENTER && op != LM_WL_POINTER_EVT_LEAVE) {      /* the help card is modal */
        help_pointer(a, op, args);
        return;
    }
    switch (op) {
    case LM_WL_POINTER_EVT_ENTER:
        a->has_pointer = true;
        a->px = lm_fixed_to_double(args[2].f);
        a->py = lm_fixed_to_double(args[3].f);
        break;
    case LM_WL_POINTER_EVT_LEAVE:
        metric_flush(a);
        a->has_pointer = false;
        a->press = a->drag = -1;
        break;
    case LM_WL_POINTER_EVT_MOTION:
        a->px = lm_fixed_to_double(args[1].f);
        a->py = lm_fixed_to_double(args[2].f);
        if (a->drag >= 0)
            slider_drag_to(a, a->drag, a->px);
        if (a->sel_widget >= 0 && a->sel_widget < a->nw) {                          /* dragging in a text field: the selection follows */
            size_t max;
            char *buf = field_buffer(a, &a->wd[a->sel_widget], &max);
            if (buf) {
                wltext_click(buf, (int)a->px - (a->wd[a->sel_widget].r.x + 6), true);
                a->win.redraw = true;
            }
        }
        break;
    case LM_WL_POINTER_EVT_BUTTON:
        a->clip.serial = args[0].u;
        if (args[2].u == 273 && args[3].u == 1) {                     /* BTN_RIGHT on a text field: Cut / Copy / Paste / Clear */
            int idx = widget_at(a, a->px, a->py);
            size_t max;
            char *buf = idx >= 0 && (a->wd[idx].kind == W_INPUT || a->wd[idx].kind == W_FIELD || a->wd[idx].kind == W_TEXT) ?
                        field_buffer(a, &a->wd[idx], &max) : NULL;
            if (buf) {
                a->focus = idx;
                a->menu_widget = idx;
                if (!wlwin_menu(&a->win, wltext_menu(buf)))
                    a->menu_widget = -1;
                a->win.redraw = true;
            }
            break;
        }
        if (args[2].u != 272) /* BTN_LEFT */
            break;
        if (args[3].u == 1) {
            int idx = widget_at(a, a->px, a->py);
            a->press = idx;
            if (idx >= 0 && a->wd[idx].kind == W_BUTTON && !strcmp(a->wd[idx].key, "dbl-try")) {      /* the double-click test: two presses within the time */
                int cur = 400, lo, hi;
                dblclick_metric(a, &cur, &lo, &hi);
                uint32_t t = args[1].u;
                bool dbl = a->dbl_last_ms && t - a->dbl_last_ms < (uint32_t)cur;
                a->dbl_state = dbl ? 2 : 1;
                a->dbl_last_ms = dbl ? 0 : t;                      /* (a third click starts again) */
            }
            if (idx >= 0 && (a->wd[idx].kind == W_INPUT || a->wd[idx].kind == W_FIELD || a->wd[idx].kind == W_TEXT)) {
                size_t max;
                char *buf = field_buffer(a, &a->wd[idx], &max);                   /* the caret goes where it was clicked */
                if (buf) {
                    wltext_click(buf, (int)a->px - (a->wd[idx].r.x + 6), (a->win.mods & LM_MOD_SHIFT) != 0);
                    a->sel_widget = idx;
                }
            }
            if (idx >= 0) {
                a->focus = idx;
                if (a->wd[idx].kind == W_SLIDER) {
                    a->drag = idx;
                    slider_drag_to(a, idx, a->px);
                }
            }
        } else {
            a->sel_widget = -1;
            int idx = widget_at(a, a->px, a->py);
            if (idx >= 0 && idx == a->press && a->drag < 0)
                activate(a, idx);
            metric_flush(a);
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

/* The text a field edits and how much fits (the wallpaper field starts from the compositor's value); NULL for a widget that is no text field
 * (the lock secrets are left out on purpose: no clipboard for them). */
static char *field_buffer(struct app *a, const struct widget *f, size_t *max)
{
    if (f->kind == W_INPUT) {
        int which = atoi(f->arg);
        *max = which == 4 ? 64 : which == 2 || which == 5 ? 250 : which == 3 ? 12 : sizeof(a->input[1]);
        return a->input[which];
    }
    if (f->kind == W_FIELD) {
        if (!a->wall_edited) {
            snprintf(a->wall, sizeof(a->wall), "%s", kv_get(a, "wallpaper"));
            a->wall_edited = true;
        }
        *max = sizeof(a->wall);
        return a->wall;
    }
    if (f->kind == W_TEXT) {
        *max = sizeof(a->test);
        return a->test;
    }
    return NULL;
}

/* What typing into an input field also does: forget old error messages, let the colour sliders follow the hex text. */
static void field_edited(struct app *a, const struct widget *f)
{
    if (f->kind != W_INPUT)
        return;
    a->keys_err[0] = a->col_err[0] = a->dock_err[0] = 0;
    uint32_t c;
    if (atoi(f->arg) == 3 && lm_theme_parse_color(a->input[3], &c)) {
        a->cr = (int)(c >> 16) & 255;
        a->cg = (int)(c >> 8) & 255;
        a->cb = (int)c & 255;
    }
}

static void menu_event(struct wlwin *w, int index)
{
    struct app *a = w->data;
    int idx = a->menu_widget;
    a->menu_widget = -1;
    build(a);
    if (index < 0 || idx < 0 || idx >= a->nw)
        return;
    size_t max;
    char *buf = field_buffer(a, &a->wd[idx], &max);
    if (!buf)
        return;
    wltext_menu_do(&a->clip, buf, max, index);
    field_edited(a, &a->wd[idx]);
    a->win.redraw = true;
}

static void keyboard_event(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
    struct app *a = w->data;
    switch (op) {
    case LM_WL_KEYBOARD_EVT_KEY: {
        a->clip.serial = args[0].u;
        if (args[3].u == 0)
            break;
        build(a);
        uint32_t ks = lm_keymap_keysym(args[2].u, a->win.mods);
        if (a->help_title[0]) {                                 /* the help card is modal: Esc / Enter / Space close it (not the panel) */
            help_key_event(a, ks);
            break;
        }
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
        } else if (f && f->kind == W_INPUT) {
            if (ks == LM_KS_Return || ks == LM_KS_KP_Enter) {
                if (atoi(f->arg) == 3)
                    colour_apply(a);
                else if (atoi(f->arg) == 2)
                    keys_put(a);
                else
                    dock_put(a);
            } else {
                size_t max;
                char *buf = field_buffer(a, f, &max);
                if (buf && wltext_edit(&a->clip, buf, max, ks, a->win.mods))     /* a caret, a selection, the clipboard (wltext.h) */
                    field_edited(a, f);
            }
            a->win.redraw = true;
        } else if (f && f->kind == W_FIELD) {
            if (ks == LM_KS_Return || ks == LM_KS_KP_Enter) {
                send_set(a, "wallpaper", a->wall_edited ? a->wall : kv_get(a, "wallpaper"));
                a->wall_edited = false;
            } else {
                size_t max;
                char *buf = field_buffer(a, f, &max);                            /* (starts from the compositor's value) */
                if (buf)
                    wltext_edit(&a->clip, buf, max, ks, a->win.mods);
            }
            a->win.redraw = true;
        } else if (f && f->kind == W_TEXT) {
            wltext_edit(&a->clip, a->test, sizeof(a->test), ks, a->win.mods);
            a->win.redraw = true;
        } else if (f && f->kind == W_SLIDER && (ks == LM_KS_Left || ks == LM_KS_Right)) {
            int v = LM_MAX(f->lo, LM_MIN(f->hi, f->val + (ks == LM_KS_Right ? f->step : -f->step)));
            if (v != f->val) {
                slider_set(a, f, v);
                metric_flush(a);
            }
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
    struct app *a = w->data;
    if (!strcmp(iface, "mellagui_settings_v1"))
        a->settings = wlc_bind(w->c, w->registry, name, &lm_mellagui_settings_v1_interface, 1, settings_event, a);
    else
        wlclip_global(&a->clip, w, name, iface, ver);
}

/* Every setting on every page has a help text, every "?" opens something, and the texts wrap into the card. */
static int selftest_help(void)
{
    static struct app fa;
    static const struct { const char *k, *v; } kvs[] = {
        { "config-file", "/x/mellagui.conf" }, { "config-status", "exists" },
        { "metrics", "windows.titlebar-height=28:20:64:28 panel.height=32:20:64:32 windows.double-click-ms=400:100:1000:400" },
        { "dock-opts", "enabled=1 autohide=0 icon-size=48 zoom=160 gap=10 margin=8 icon-background=light" },
        { "outputs", "A|1024x600|0,0;B|800x600|1024,0" }, { "resolutions", "1024x600|;800x600|" }, { "resolution", "auto" },
        { "audio-profiles", "p1|Analog|;p2|HDMI|" }, { "sources", "mic1|Built-in microphone;mic2|USB headset" }, { "source", "mic1" }, { "source-volume", "60" }, { "audio-profile", "p1" }, { "sink", "s1" }, { "audio-outs", "default|System default;hw:0|Card 0" },
        { "audio-out", "default" }, { "layouts", "us,ee" }, { "layout", "us" }, { "wallpaper-mode", "fill" }, { "lock-state", "set" },
        { "dock-0", "Files\x1f" "files\x1f" "mellagui-files" }, { "theme-colors", "background=#14171f accent=#6fa8ff button-accent=#4c8dff" }, { "theme-buttons", "colourful" }, { "saver-style", "room" }, { "config-file", "/x/mellagui.conf" },
        { "metrics", "windows.button-roundness=100:0:100:100 windows.double-click-ms=400:100:1000:400" }, { "theme-list", "colorful* graphite light contrast" }, { "font-name", "bitmap" }, { "font-file", "auto" }, { "font-ui", "auto" }, { "scale", "100" }, { "smart", "0" }, { "resolution-now", "1024x600@60" },
    };
    int bad = 0, shown = 0;
    static const struct { enum page pg; int input, sound, theme, display; } extra[] = {      /* the second and further parts of the pages with parts */
        { PAGE_COLOURS, 0, 0, 1, 0 }, { PAGE_COLOURS, 0, 0, 2, 0 }, { PAGE_INPUT, 1, 0, 0, 0 }, { PAGE_SOUND, 0, 1, 0, 0 },
        { PAGE_COLOURS, 0, 0, 3, 0 }, { PAGE_DISPLAY, 0, 0, 0, 1 },
    };
    for (int pgi = 0; pgi < N_PAGES + (int)(sizeof(extra) / sizeof(extra[0])); pgi++) {
        int pg = pgi < N_PAGES ? pgi : (int)extra[pgi - N_PAGES].pg;
        memset(&fa, 0, sizeof(fa));
        for (size_t i = 0; i < sizeof(kvs) / sizeof(kvs[0]); i++)
            kv_set(&fa, kvs[i].k, kvs[i].v);
        fa.page = (enum page)pg;
        if (pgi >= N_PAGES) {
            fa.input_tab = extra[pgi - N_PAGES].input;
            fa.sound_tab = extra[pgi - N_PAGES].sound;
            fa.theme_tab = extra[pgi - N_PAGES].theme;
            fa.display_tab = extra[pgi - N_PAGES].display;
        }
        fa.win.bw = 860;
        fa.win.bh = 560;
        fa.focus = fa.hover = fa.press = fa.drag = fa.menu_widget = fa.sel_widget = -1;
        fa.dock_sel = -1;
        build(&fa);
        for (int i = 0; i < fa.nw; i++) {
            const struct widget *w = &fa.wd[i];
            char key[64];
            const char *arg;
            if (help_key(w, key, sizeof(key), &arg)) {
                bool owned = help_exists(key, arg);
                if (!owned && w->kind != W_HEADING) {
                    fprintf(stderr, "selftest: page %s: '%s' (key %s) has no help text\n", PAGE_NAMES[pg], w->text, key);
                    bad++;
                }
                if (owned && w->kind == W_HEADING)
                    shown++;
            }
            if (w->kind == W_HELP) {
                help_open(&fa, w->text, w->arg);
                if (!fa.help_title[0] || !strcmp(fa.help_title, "No help")) {
                    fprintf(stderr, "selftest: page %s: a '?' for %s opens nothing\n", PAGE_NAMES[pg], w->text);
                    bad++;
                }
                struct help_geom *g = calloc(1, sizeof(*g));
                help_geom(&fa, g);
                if (g->visible < 1 || g->card.h > fa.win.bh) {
                    fprintf(stderr, "selftest: the help of %s does not fit the window\n", w->text);
                    bad++;
                }
                free(g);
                fa.help_title[0] = 0;
            }
        }
    }
    if (bad)
        return 1;
    printf("mellagui-settings help selftest ok (%d headings explained)\n", shown);
    return 0;
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
    wlwin_hidpi();                      /* draws through the wlwin_* calls only: sharp on a scaled screen */
    lm_theme_load_env();
    if (argc > 1 && !strcmp(argv[1], "--selftest"))
        return selftest() ? 1 : selftest_help();
    bool cli_mode = argc > 1 && (!strcmp(argv[1], "--get") || !strcmp(argv[1], "--set"));
    if (argc > 1 && !cli_mode) {
        fprintf(stderr, "usage: mellagui-settings [--get [KEY] | --set KEY=VALUE... | --selftest]\n");
        return 2;
    }

    signal(SIGCHLD, SIG_IGN);                           /* the panels started from here are nobody's business afterwards */
    static const struct wlwin_ops ops = { .paint = paint, .global = global_event, .pointer = pointer_event,
                                          .keyboard = keyboard_event, .menu = menu_event };
    struct app a = { .press = -1, .drag = -1, .hover = -1, .focus = -1, .csel = -1, .mpend = -1, .dock_sel = -1, .menu_widget = -1, .sel_widget = -1 };
    if (wlwin_connect(&a.win, "mellagui-settings", &ops, &a) < 0)
        return 1;
    if (a.settings && wlc_roundtrip(a.win.c) < 0)       /* the value burst that follows the bind */
        return 1;
    if (cli_mode) {
        int rc = cli(&a, argc, argv);
        wlwin_destroy(&a.win);
        return rc;
    }
    if (wlwin_open(&a.win, "Settings", "mellagui.settings", DEFAULT_W, DEFAULT_H, 520, 420) < 0)
        return 1;
    wlclip_attach(&a.clip, &a.win);
    while (wlwin_step(&a.win, -1))
        ;
    wlwin_destroy(&a.win);
    return 0;
}
