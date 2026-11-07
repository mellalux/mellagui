/*
 * lumen-settings: the settings panel (wl_shm + xdg-shell + the private lumen_settings_v1, bitmap font).
 *
 *   Keyboard   layouts (enable, order, active), sticky keys, key repeat delay / rate, a field to try the keyboard
 *   Pointer    pointer speed, natural scrolling
 *   Desktop    clipboard gadget, sound feedback
 *
 * Mouse: click checkboxes / radio buttons / buttons, drag the sliders. Keyboard: Tab / Shift+Tab move the focus,
 * Enter or Space activates, Left/Right change a slider, Esc closes. Every change is sent to the compositor at once,
 * which applies it, saves it (see src/settings.h) and answers with the effective values - the window only ever
 * shows what the compositor reports.
 *
 *   lumen-settings                  the panel
 *   lumen-settings --get [KEY]      print KEY=VALUE for every setting (or just one)
 *   lumen-settings --set K=V ...    change settings; prints the resulting values, exit status 1 if one was refused
 *   lumen-settings --selftest       checks the list / slider helpers without a display
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "font.h"
#include "keymap.h"
#include "util.h"
#include "wlc.h"

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
    if (selftest_fail)
        return 1;
    puts("lumen-settings selftest ok");
    return 0;
}

/* ---- state -------------------------------------------------------------------------------------------------- */

#define COL_BG      0x14171fu
#define COL_SIDE    0x0d0f14u
#define COL_FIELD   0x0d0f14u
#define COL_KEY     0x2d3a5au
#define COL_HOVER   0x41527eu
#define COL_DOWN    0x56699cu
#define COL_ACCENT  0x6fa8ffu
#define COL_TEXT    0xe6e9efu
#define COL_DIM     0x7b8294u

#define DEFAULT_W 620
#define DEFAULT_H 440
#define SIDE_W 140
#define MAX_WIDGETS 80
#define MAX_KV 32
#define VALUE_MAX 1900          /* longest value (the lists of resolutions / sound outputs) */

enum page { PAGE_KEYBOARD, PAGE_POINTER, PAGE_DESKTOP, PAGE_DISPLAY, PAGE_SOUND, N_PAGES };
static const char *const PAGE_NAMES[N_PAGES] = { "Keyboard", "Pointer", "Desktop", "Display", "Sound" };

enum wkind { W_LABEL, W_HEADING, W_TAB, W_CHECK, W_RADIO, W_BUTTON, W_SLIDER, W_TEXT };

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
    struct wlc *c;
    uint32_t registry, compositor, shm, wm_base, seat, pointer, keyboard, settings;
    uint32_t surface, xsurface, toplevel, pool, buffer;
    int w, h, bw, bh;
    uint32_t *pixels;
    size_t map_size;
    bool configured, running, buffer_busy, redraw;
    uint32_t mods;
    double px, py;
    bool has_pointer;
    struct kv kvs[MAX_KV];
    int nkv;
    enum page page;
    struct widget wd[MAX_WIDGETS];
    int nw;
    int hover, press, drag, focus;      /* widget indexes, -1 none */
    char test[64];                      /* text typed into the "try it" field */
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
        WLC_SEND(a->c, a->settings, LM_LUMEN_SETTINGS_V1_REQ_SET, { .s = key }, { .s = value });
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
}

static struct widget *add_radio(struct app *a, int x, int y, int w, const char *text, const char *key, const char *arg, bool on)
{
    struct widget *r = add(a, W_RADIO, (struct rect){ x, y, w, ROW_H }, text);
    snprintf(r->key, sizeof(r->key), "%s", key);
    snprintf(r->arg, sizeof(r->arg), "%s", arg);
    r->on = on;
    return r;
}

static void build_display(struct app *a, int x, int y, int w)
{
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
    add(a, W_HEADING, (struct rect){ x, y, w, ROW_H }, "Resolution");
    y += ROW_H;
    const char *cur = kv_get(a, "resolution");
    add_radio(a, x, y, w, "Automatic (the panel's own shape)", "resolution", "auto", !strcmp(cur, "auto"));
    y += ROW_H;
    add_radio(a, x, y, w, "The monitor's preferred mode", "resolution", "preferred", !strcmp(cur, "preferred"));
    y += ROW_H;
    const char *list = kv_get(a, "resolutions");
    char name[64], note[96], label[96];
    int shown = 0;
    while (shown < 8 && next_entry(&list, name, sizeof(name), note, sizeof(note))) {
        mode_label(name, note, label, sizeof(label));
        add_radio(a, x, y, w, label, "resolution", name, !strcmp(cur, name));
        y += ROW_H;
        shown++;
    }
    char now[96], cur_label[80];
    mode_label(kv_get(a, "resolution-now"), "", cur_label, sizeof(cur_label));
    snprintf(now, sizeof(now), "Now: %s", cur_label);
    add(a, W_LABEL, (struct rect){ x, y + 4, w, ROW_H }, now);
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
    /* 3. Lumen's own beeps (ALSA device) */
    add(a, W_HEADING, (struct rect){ x, y, w, ROW_H }, "Lumen's own beeps (ALSA)");
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
    int x = SIDE_W + 20, y = 14, w = a->bw - x - 20;
    if (w < 200)
        w = 200;
    switch (a->page) {
    case PAGE_KEYBOARD: build_keyboard(a, x, y, w); break;
    case PAGE_POINTER: build_pointer(a, x, y, w); break;
    case PAGE_DISPLAY: build_display(a, x, y, w); break;
    case PAGE_SOUND: build_sound(a, x, y, w); break;
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

static void slider_drag_to(struct app *a, int idx, double x)
{
    const struct widget *w = &a->wd[idx];
    struct rect t = slider_track(w);
    int v = slider_value(w->lo, w->hi, w->step, (x - t.x) / (double)t.w);
    if (v != w->val)
        send_set_int(a, w->key, v);
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
        a->redraw = true;
        if (a->page == PAGE_SOUND)
            send_set(a, "audio-rescan", "1");           /* ask `pactl` again: devices come and go */
        break;
    case W_CHECK:
        if (!strcmp(w->key, "layout-enable")) {
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
        send_set(a, w->key, w->arg);
        break;
    case W_BUTTON:
        if (!strcmp(w->key, "resolution-confirm")) {
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
            a->redraw = true;
            return;
        }
    }
}

/* ---- drawing -------------------------------------------------------------------------------------------------- */

static void fill(struct app *a, struct rect r, uint32_t color)
{
    for (int y = LM_MAX(r.y, 0); y < LM_MIN(r.y + r.h, a->bh); y++)
        for (int x = LM_MAX(r.x, 0); x < LM_MIN(r.x + r.w, a->bw); x++)
            a->pixels[(size_t)y * (size_t)a->bw + (size_t)x] = 0xff000000u | color;
}

static void outline(struct app *a, struct rect r, uint32_t color)
{
    fill(a, (struct rect){ r.x, r.y, r.w, 1 }, color);
    fill(a, (struct rect){ r.x, r.y + r.h - 1, r.w, 1 }, color);
    fill(a, (struct rect){ r.x, r.y, 1, r.h }, color);
    fill(a, (struct rect){ r.x + r.w - 1, r.y, 1, r.h }, color);
}

/* Draws up to max_w pixels of text (clipped on a character boundary). */
static void draw_text(struct app *a, int x, int y, const char *s, uint32_t color, int max_w)
{
    size_t len = strlen(s);
    int x0 = x;
    for (size_t i = 0; i < len;) {
        if (max_w > 0 && x + LM_FONT_W > x0 + max_w)
            break;
        int used;
        uint32_t cp = lm_utf8_decode(s + i, (int)(len - i), &used);
        i += used > 0 ? (size_t)used : 1;
        const uint8_t *g = lm_font_glyph(cp);
        for (int r = 0; r < LM_FONT_H; r++)
            for (int cx = 0; cx < LM_FONT_W; cx++) {
                int px = x + cx, py = y + r;
                if ((g[r] & (0x80 >> cx)) && px >= 0 && px < a->bw && py >= 0 && py < a->bh)
                    a->pixels[(size_t)py * (size_t)a->bw + (size_t)px] = 0xff000000u | color;
            }
        x += LM_FONT_W;
    }
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

static void paint(struct app *a)
{
    fill(a, (struct rect){ 0, 0, a->bw, a->bh }, COL_BG);
    fill(a, (struct rect){ 0, 0, SIDE_W, a->bh }, COL_SIDE);
    build(a);
    for (int i = 0; i < a->nw; i++)
        paint_widget(a, i);
    const char *file = kv_get(a, "file");
    const char *l1 = !a->settings ? "The compositor has no settings interface."
                   : *file ? "Changes apply at once and are saved to:"
                           : "Changes apply at once (this session is not saving them).";
    draw_text(a, SIDE_W + 20, a->bh - (*file ? 44 : 24), l1, COL_DIM, a->bw - SIDE_W - 30);
    if (a->settings && *file) {
        int room = (a->bw - SIDE_W - 30) / LM_FONT_W, len = (int)strlen(file);
        draw_text(a, SIDE_W + 20, a->bh - 24, len > room ? file + (len - room) : file, COL_DIM, 0);
    }
}

static void setup_buffer(struct app *a)
{
    if (a->pool) {
        WLC_SEND0(a->c, a->pool, LM_WL_SHM_POOL_REQ_DESTROY);
        wlc_forget(a->c, a->pool);
        a->pool = 0;
    }
    if (a->pixels)
        munmap(a->pixels, a->map_size);
    a->bw = a->w;
    a->bh = a->h;
    a->map_size = (size_t)a->bw * (size_t)a->bh * 4;
    int fd = memfd_create("lumen-settings", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)a->map_size) < 0) {
        perror("memfd");
        exit(1);
    }
    a->pixels = mmap(NULL, a->map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (a->pixels == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }
    a->pool = wlc_alloc(a->c, &lm_wl_shm_pool_interface, NULL, NULL);
    WLC_SEND(a->c, a->shm, LM_WL_SHM_REQ_CREATE_POOL, { .n = a->pool }, { .h = fd }, { .i = (int32_t)a->map_size });
    close(fd);
}

static void buffer_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)args;
    struct app *a = data;
    if (op != LM_WL_BUFFER_EVT_RELEASE)
        return;
    WLC_SEND0(c, id, LM_WL_BUFFER_REQ_DESTROY);
    wlc_forget(c, id);
    a->buffer = 0;
    a->buffer_busy = false;
}

static void draw(struct app *a)
{
    if (a->bw != a->w || a->bh != a->h || !a->pixels)
        setup_buffer(a);
    paint(a);
    a->buffer = wlc_alloc(a->c, &lm_wl_buffer_interface, buffer_event, a);
    WLC_SEND(a->c, a->pool, LM_WL_SHM_POOL_REQ_CREATE_BUFFER, { .n = a->buffer }, { .i = 0 }, { .i = a->bw },
             { .i = a->bh }, { .i = a->bw * 4 }, { .u = LM_WL_SHM_FORMAT_XRGB8888 });
    WLC_SEND(a->c, a->surface, LM_WL_SURFACE_REQ_ATTACH, { .o = a->buffer }, { .i = 0 }, { .i = 0 });
    WLC_SEND(a->c, a->surface, LM_WL_SURFACE_REQ_DAMAGE, { .i = 0 }, { .i = 0 }, { .i = a->bw }, { .i = a->bh });
    WLC_SEND0(a->c, a->surface, LM_WL_SURFACE_REQ_COMMIT);
    a->buffer_busy = true;
    a->redraw = false;
}

/* ---- protocol events -------------------------------------------------------------------------------------- */

static void settings_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)c; (void)id;
    struct app *a = data;
    if (op == LM_LUMEN_SETTINGS_V1_EVT_VALUE) {
        kv_set(a, args[0].s, args[1].s);
    } else if (op == LM_LUMEN_SETTINGS_V1_EVT_DONE) {
        a->redraw = true;
    }
}

static void wm_base_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)data;
    if (op == LM_XDG_WM_BASE_EVT_PING)
        WLC_SEND(c, id, LM_XDG_WM_BASE_REQ_PONG, { .u = args[0].u });
}

static void xsurface_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    struct app *a = data;
    if (op == LM_XDG_SURFACE_EVT_CONFIGURE) {
        WLC_SEND(c, id, LM_XDG_SURFACE_REQ_ACK_CONFIGURE, { .u = args[0].u });
        a->configured = true;
        a->redraw = true;
    }
}

static void toplevel_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)c; (void)id;
    struct app *a = data;
    if (op == LM_XDG_TOPLEVEL_EVT_CONFIGURE) {
        a->w = args[0].i > 0 ? args[0].i : DEFAULT_W;
        a->h = args[1].i > 0 ? args[1].i : DEFAULT_H;
        a->w = LM_MAX(a->w, 520);
        a->h = LM_MAX(a->h, 380);
    } else if (op == LM_XDG_TOPLEVEL_EVT_CLOSE) {
        a->running = false;
    }
}

static void pointer_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)c; (void)id;
    struct app *a = data;
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
        a->redraw = true;
        break;
    }
    int hover = a->has_pointer ? widget_at(a, a->px, a->py) : -1;
    if (hover != a->hover) {
        a->hover = hover;
        a->redraw = true;
    }
}

static void keyboard_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)c; (void)id;
    struct app *a = data;
    switch (op) {
    case LM_WL_KEYBOARD_EVT_KEYMAP:
        lm_keymap_client_load(args[1].h, args[2].u);
        break;
    case LM_WL_KEYBOARD_EVT_MODIFIERS:
        a->mods = args[1].u | args[2].u | args[3].u;
        lm_keymap_client_group(args[4].u);
        break;
    case LM_WL_KEYBOARD_EVT_KEY: {
        if (args[3].u == 0)
            break;
        build(a);
        uint32_t ks = lm_keymap_keysym(args[2].u, a->mods);
        struct widget *f = a->focus >= 0 ? &a->wd[a->focus] : NULL;
        bool plain = !(a->mods & (LM_MOD_CTRL | LM_MOD_ALT | LM_MOD_SUPER));
        if (ks == LM_KS_Escape) {
            a->running = false;
        } else if (ks == LM_KS_Tab) {
            move_focus(a, (a->mods & LM_MOD_SHIFT) ? -1 : 1);
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
            a->redraw = true;
        } else if (f && f->kind == W_SLIDER && (ks == LM_KS_Left || ks == LM_KS_Right)) {
            int v = LM_MAX(f->lo, LM_MIN(f->hi, f->val + (ks == LM_KS_Right ? f->step : -f->step)));
            if (v != f->val)
                send_set_int(a, f->key, v);
        } else if (f && (ks == LM_KS_Return || ks == LM_KS_KP_Enter || ks == ' ')) {
            activate(a, a->focus);
        } else if (!f && (ks == LM_KS_Down || ks == LM_KS_Right)) {
            move_focus(a, 1);
        }
        break;
    }
    }
}

static void seat_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    struct app *a = data;
    if (op == LM_WL_SEAT_EVT_CAPABILITIES) {
        if ((args[0].u & 1) && !a->pointer) {
            a->pointer = wlc_alloc(c, &lm_wl_pointer_interface, pointer_event, a);
            WLC_SEND(c, id, LM_WL_SEAT_REQ_GET_POINTER, { .n = a->pointer });
        }
        if ((args[0].u & 2) && !a->keyboard) {
            a->keyboard = wlc_alloc(c, &lm_wl_keyboard_interface, keyboard_event, a);
            WLC_SEND(c, id, LM_WL_SEAT_REQ_GET_KEYBOARD, { .n = a->keyboard });
        }
    }
}

static void registry_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)id;
    struct app *a = data;
    if (op != LM_WL_REGISTRY_EVT_GLOBAL)
        return;
    uint32_t name = args[0].u, ver = args[2].u;
    const char *iface = args[1].s;
    if (!strcmp(iface, "wl_compositor"))
        a->compositor = wlc_bind(c, a->registry, name, &lm_wl_compositor_interface, LM_MIN(ver, 4u), NULL, a);
    else if (!strcmp(iface, "wl_shm"))
        a->shm = wlc_bind(c, a->registry, name, &lm_wl_shm_interface, 1, NULL, a);
    else if (!strcmp(iface, "xdg_wm_base"))
        a->wm_base = wlc_bind(c, a->registry, name, &lm_xdg_wm_base_interface, LM_MIN(ver, 3u), wm_base_event, a);
    else if (!strcmp(iface, "wl_seat"))
        a->seat = wlc_bind(c, a->registry, name, &lm_wl_seat_interface, LM_MIN(ver, 5u), seat_event, a);
    else if (!strcmp(iface, "lumen_settings_v1"))
        a->settings = wlc_bind(c, a->registry, name, &lm_lumen_settings_v1_interface, 1, settings_event, a);
}

/* ---- command line ----------------------------------------------------------------------------------------- */

static int cli(struct app *a, int argc, char **argv)
{
    if (!a->settings) {
        fprintf(stderr, "lumen-settings: the compositor has no lumen_settings_v1\n");
        return 1;
    }
    int bad = 0;
    if (!strcmp(argv[1], "--set")) {
        for (int i = 2; i < argc; i++) {
            char *eq = strchr(argv[i], '=');
            if (!eq) {
                fprintf(stderr, "lumen-settings: expected KEY=VALUE, got '%s'\n", argv[i]);
                return 2;
            }
            *eq = 0;
            send_set(a, argv[i], eq + 1);
            if (wlc_roundtrip(a->c) < 0)
                return 1;
            printf("%s=%s\n", argv[i], kv_get(a, argv[i]));
            if (strcmp(kv_get(a, argv[i]), eq + 1)) {
                fprintf(stderr, "lumen-settings: '%s=%s' was not accepted as given\n", argv[i], eq + 1);
                bad = 1;
            }
        }
        return bad;
    }
    /* --get [KEY] */
    if (argc > 2) {
        if (!*kv_get(a, argv[2]) && strcmp(argv[2], "file")) {
            fprintf(stderr, "lumen-settings: unknown setting '%s'\n", argv[2]);
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
    if (argc > 1 && !strcmp(argv[1], "--selftest"))
        return selftest();
    bool cli_mode = argc > 1 && (!strcmp(argv[1], "--get") || !strcmp(argv[1], "--set"));
    if (argc > 1 && !cli_mode) {
        fprintf(stderr, "usage: lumen-settings [--get [KEY] | --set KEY=VALUE... | --selftest]\n");
        return 2;
    }

    struct app a = { .w = DEFAULT_W, .h = DEFAULT_H, .running = true, .press = -1, .drag = -1, .hover = -1,
                     .focus = -1 };
    a.c = wlc_connect(NULL);
    if (!a.c)
        return 1;
    a.registry = wlc_get_registry(a.c, registry_event, &a);
    if (wlc_roundtrip(a.c) < 0)
        return 1;
    if (a.settings && wlc_roundtrip(a.c) < 0)       /* the value burst that follows the bind */
        return 1;
    if (cli_mode) {
        int rc = cli(&a, argc, argv);
        wlc_destroy(a.c);
        return rc;
    }
    if (!a.compositor || !a.shm || !a.wm_base) {
        fprintf(stderr, "lumen-settings: compositor lacks wl_compositor/wl_shm/xdg_wm_base\n");
        return 1;
    }
    a.surface = wlc_alloc(a.c, &lm_wl_surface_interface, NULL, NULL);
    WLC_SEND(a.c, a.compositor, LM_WL_COMPOSITOR_REQ_CREATE_SURFACE, { .n = a.surface });
    a.xsurface = wlc_alloc(a.c, &lm_xdg_surface_interface, xsurface_event, &a);
    WLC_SEND(a.c, a.wm_base, LM_XDG_WM_BASE_REQ_GET_XDG_SURFACE, { .n = a.xsurface }, { .o = a.surface });
    a.toplevel = wlc_alloc(a.c, &lm_xdg_toplevel_interface, toplevel_event, &a);
    WLC_SEND(a.c, a.xsurface, LM_XDG_SURFACE_REQ_GET_TOPLEVEL, { .n = a.toplevel });
    WLC_SEND(a.c, a.toplevel, LM_XDG_TOPLEVEL_REQ_SET_TITLE, { .s = "Settings" });
    WLC_SEND(a.c, a.toplevel, LM_XDG_TOPLEVEL_REQ_SET_APP_ID, { .s = "lumen.settings" });
    WLC_SEND0(a.c, a.surface, LM_WL_SURFACE_REQ_COMMIT);

    while (a.running && !wlc_dead(a.c)) {
        if (a.redraw && a.configured && !a.buffer_busy)
            draw(&a);
        if (wlc_dispatch(a.c, -1) < 0)
            break;
    }
    wlc_destroy(a.c);
    return 0;
}
