/*
 * mellagui-keys - the keyboard shortcuts of the compositor in one window (Super+/ starts it).
 *
 *   mellagui-keys            list every shortcut ([keys] of mellagui.conf and the built-in ones): the keys on the left, what they do on the right
 *   mellagui-keys --selftest
 *
 * The table comes from the compositor's private mellagui_settings_v1 (the read-only values keys-help-0 .. 4). Up / Down / PgUp / PgDn / the wheel
 * scroll, Esc or Q closes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "font.h"
#include "keymap.h"
#include "protocol.h"
#include "theme.h"
#include "util.h"
#include "wlc.h"
#include "wlwin.h"

#define MAX_ROWS 128
#define PAD 14
#define ROW 22
#define HEAD 44
#define FOOT 30

struct row {
    char keys[64];
    char what[96];
};

struct app {
    struct wlwin win;
    uint32_t settings;
    struct row rows[MAX_ROWS];
    int n;
    int top;                            /* first row shown */
    char chunk[5][1900];                /* the values as they came */
};

static int rows_visible(const struct app *a)
{
    return LM_MAX((a->win.bh - HEAD - FOOT) / ROW, 1);
}

static void clamp_top(struct app *a)
{
    a->top = LM_CLAMP(a->top, 0, LM_MAX(a->n - rows_visible(a), 0));
}

/* What a shortcut does, in words ("close" -> "close the window"). */
static const char *describe(const char *act, char *buf, size_t n)
{
    static const struct { const char *act, *text; } t[] = {
        { "terminal", "open a terminal" }, { "run", "the Run dialog" }, { "settings", "the settings panel" }, { "layout-next", "next keyboard layout" },
        { "layout-prev", "previous keyboard layout" }, { "gadget-clipboard", "clipboard history on / off" }, { "clipboard-pick", "choose from the clipboard history" },
        { "gadget-mixer", "volume mixer on / off" }, { "gadget-system", "system monitor on / off" }, { "close", "close the window" },
        { "lock", "lock the screen" }, { "focus-next", "next window" }, { "focus-prev", "previous window" }, { "fullscreen", "full screen on / off" },
        { "maximize-toggle", "maximize / restore" }, { "tile-left", "window to the left half" }, { "tile-right", "window to the right half" },
        { "maximize", "maximize" }, { "restore", "restore the size" }, { "quit", "quit MellaGUI" }, { "launcher", "the application launcher" },
        { "osk", "on-screen keyboard on / off" }, { "reload-config", "read mellagui.conf again" },
        { "overview", "show all windows" }, { "night-light", "night light on / off" }, { "dnd", "do not disturb on / off" },
    };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++)
        if (!strcmp(act, t[i].act)) {
            snprintf(buf, n, "%s", t[i].text);
            return buf;
        }
    if (!strncmp(act, "workspace ", 10))
        snprintf(buf, n, "go to workspace %s", act + 10);
    else if (!strncmp(act, "move-to-workspace ", 18))
        snprintf(buf, n, "send the window to workspace %s", act + 18);
    else if (!strncmp(act, "exec ", 5))
        snprintf(buf, n, "start %s", act + 5);
    else
        snprintf(buf, n, "%s", act);
    return buf;
}

/* "Super+Q<US>close" lines -> rows. */
static void add_rows(struct app *a, const char *text)
{
    char line[256];
    for (const char *p = text; *p && a->n < MAX_ROWS;) {
        size_t l = strcspn(p, "\n");
        snprintf(line, sizeof(line), "%.*s", (int)LM_MIN(l, sizeof(line) - 1), p);
        p += l + (p[l] ? 1 : 0);
        char *us = strchr(line, 0x1f);
        if (!us)
            continue;
        *us = 0;
        struct row *r = &a->rows[a->n++];
        snprintf(r->keys, sizeof(r->keys), "%.*s", (int)sizeof(r->keys) - 1, line);
        describe(us + 1, r->what, sizeof(r->what));
    }
}

static void settings_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)c; (void)id;
    struct app *a = data;
    if (op != LM_MELLAGUI_SETTINGS_V1_EVT_VALUE)
        return;
    const char *k = args[0].s;
    if (!strncmp(k, "keys-help-", 10) && k[10] >= '0' && k[10] <= '4' && !k[11])
        snprintf(a->chunk[k[10] - '0'], sizeof(a->chunk[0]), "%s", args[1].s);
}

static void global_event(struct wlwin *w, uint32_t name, const char *iface, uint32_t ver)
{
    (void)ver;
    struct app *a = w->data;
    if (!strcmp(iface, "mellagui_settings_v1"))
        a->settings = wlc_bind(w->c, w->registry, name, &lm_mellagui_settings_v1_interface, 1, settings_event, a);
}

static void paint(struct wlwin *w)
{
    struct app *a = w->data;
    wlwin_fill(w, 0, 0, w->bw, w->bh, lm_theme.background);
    wlwin_text(w, PAD, 14, "Keyboard shortcuts", 1, lm_theme.accent, 0);
    wlwin_fill(w, PAD, HEAD - 8, w->bw - 2 * PAD, 1, lm_theme.border);
    clamp_top(a);
    int visible = rows_visible(a), keyw = 0;
    for (int i = 0; i < a->n; i++)
        keyw = LM_MAX(keyw, lm_font_text_width(a->rows[i].keys));
    for (int r = 0; r < visible && a->top + r < a->n; r++) {
        const struct row *row = &a->rows[a->top + r];
        int y = HEAD + r * ROW;
        if (r % 2)
            wlwin_fill(w, PAD - 6, y - 2, w->bw - 2 * PAD + 12, ROW, lm_theme.field);
        wlwin_text(w, PAD, y + 2, row->keys, 1, lm_theme.text, keyw + 4);
        wlwin_text(w, PAD + keyw + 24, y + 2, row->what, 1, lm_theme.text_soft, w->bw - PAD - keyw - 24 - PAD);
    }
    char foot[96];
    snprintf(foot, sizeof(foot), "%d shortcuts   Up / Down / PgUp / PgDn scroll   Esc closes", a->n);
    wlwin_text(w, PAD, w->bh - FOOT + 6, foot, 1, lm_theme.text_dim, w->bw - 2 * PAD);
}

static void pointer_event(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
    struct app *a = w->data;
    if (op == LM_WL_POINTER_EVT_AXIS && args[1].u == 0) {
        double v = lm_fixed_to_double(args[2].f);
        a->top += v > 0 ? 3 : v < 0 ? -3 : 0;
        w->redraw = true;
    }
}

static void keyboard_event(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
    struct app *a = w->data;
    if (op != LM_WL_KEYBOARD_EVT_KEY || args[3].u == 0)
        return;
    uint32_t ks = lm_keymap_keysym(args[2].u, w->mods);
    int page = LM_MAX(rows_visible(a) - 1, 1);
    if (ks == LM_KS_Escape || ks == 'q' || ks == 'Q')
        w->running = false;
    else if (ks == LM_KS_Down)
        a->top++;
    else if (ks == LM_KS_Up)
        a->top--;
    else if (ks == LM_KS_Page_Down || ks == ' ')
        a->top += page;
    else if (ks == LM_KS_Page_Up)
        a->top -= page;
    else if (ks == LM_KS_Home)
        a->top = 0;
    else if (ks == LM_KS_End)
        a->top = MAX_ROWS;
    w->redraw = true;
}

static int selftest(void)
{
    struct app *a = calloc(1, sizeof(*a));
    add_rows(a, "Super+Q\x1f" "close\nSuper+1\x1f" "workspace 1\nSuper+Slash\x1f" "exec mellagui-keys\nBroken line without a separator\nCtrl+X\x1fmystery");
    int bad = 0;
    if (a->n != 4 || strcmp(a->rows[0].what, "close the window") || strcmp(a->rows[1].what, "go to workspace 1") ||
        strcmp(a->rows[2].what, "start mellagui-keys") || strcmp(a->rows[3].what, "mystery") || strcmp(a->rows[2].keys, "Super+Slash")) {
        fprintf(stderr, "mellagui-keys: selftest failed (%d rows)\n", a->n);
        bad = 1;
    }
    free(a);
    if (!bad)
        printf("mellagui-keys selftest ok\n");
    return bad;
}

int main(int argc, char **argv)
{
    lm_theme_load_env();
    if (argc > 1 && !strcmp(argv[1], "--selftest"))
        return selftest();
    static const struct wlwin_ops ops = { .paint = paint, .global = global_event, .pointer = pointer_event, .keyboard = keyboard_event };
    static struct app a;
    if (wlwin_connect(&a.win, "mellagui-keys", &ops, &a) < 0)
        return 1;
    if (!a.settings) {
        fprintf(stderr, "mellagui-keys: the compositor offers no mellagui_settings_v1 (or not to this program)\n");
        wlwin_destroy(&a.win);
        return 1;
    }
    if (wlc_roundtrip(a.win.c) < 0) {                       /* the value burst */
        wlwin_destroy(&a.win);
        return 1;
    }
    for (int i = 0; i < 5; i++)
        add_rows(&a, a.chunk[i]);
    if (wlwin_open(&a.win, "Keyboard shortcuts", "mellagui.keys", 560, 480, 360, 200) < 0)
        return 1;
    while (wlwin_step(&a.win, -1))
        ;
    wlwin_destroy(&a.win);
    return 0;
}
