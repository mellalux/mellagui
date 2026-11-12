/*
 * mellagui-notes - the history of notifications in one window (Super+Ctrl+N, or the bell in the panel).
 *
 *   mellagui-notes            list the last notifications, newest first; opening it marks them as read
 *   mellagui-notes --selftest
 *
 * The list comes from the compositor's private mellagui_settings_v1 (the read-only values notes-0 .. notes-4, "id<US>HH:MM<US>title<US>body" lines).
 * Up / Down / PgUp / PgDn / Home / End / the wheel / a click choose, Delete or the x forgets one (notes-del), C clears all (notes-clear),
 * D switches "do not disturb" (dnd), Esc or Q closes. The list follows new notifications while the window is open.
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

#define MAX_ROWS 50
#define PAD 14
#define ROW 44
#define HEAD 44
#define DETAIL 76
#define FOOT 40

struct row {
    int id;
    char time[8];
    char title[64];
    char body[168];
};

struct app {
    struct wlwin win;
    uint32_t settings;
    struct row rows[MAX_ROWS];
    int n;
    int top;                            /* first row shown */
    int sel;                            /* chosen row */
    int unread;
    bool dnd;
    char chunk[5][2800];
    double px, py;                      /* the pointer */
};

static int rows_visible(const struct app *a)
{
    return LM_MAX((a->win.bh - HEAD - DETAIL - FOOT) / ROW, 1);
}

static void clamp(struct app *a)
{
    a->n = LM_CLAMP(a->n, 0, MAX_ROWS);
    a->sel = LM_CLAMP(a->sel, 0, LM_MAX(a->n - 1, 0));
    int vis = rows_visible(a);
    if (a->sel < a->top)
        a->top = a->sel;
    if (a->sel >= a->top + vis)
        a->top = a->sel - vis + 1;
    a->top = LM_CLAMP(a->top, 0, LM_MAX(a->n - vis, 0));
}

/* "id<US>HH:MM<US>title<US>body" lines -> rows (appended). */
static void add_rows(struct app *a, const char *text)
{
    for (const char *p = text; *p && a->n < MAX_ROWS;) {
        char line[320];
        size_t l = strcspn(p, "\n");
        snprintf(line, sizeof(line), "%.*s", (int)LM_MIN(l, sizeof(line) - 1), p);
        p += l + (p[l] ? 1 : 0);
        char *f1 = strchr(line, 0x1f), *f2 = f1 ? strchr(f1 + 1, 0x1f) : NULL, *f3 = f2 ? strchr(f2 + 1, 0x1f) : NULL;
        if (!f3)
            continue;
        *f1 = *f2 = *f3 = 0;
        struct row *r = &a->rows[a->n++];
        r->id = atoi(line);
        snprintf(r->time, sizeof(r->time), "%s", f1 + 1);
        snprintf(r->title, sizeof(r->title), "%s", f2 + 1);
        snprintf(r->body, sizeof(r->body), "%s", f3 + 1);
    }
}

static void rebuild(struct app *a)
{
    int keep = a->n > 0 && a->sel < a->n ? a->rows[a->sel].id : 0;
    a->n = 0;
    for (int i = 0; i < 5; i++)
        add_rows(a, a->chunk[i]);
    for (int i = 0; keep && i < a->n; i++)                  /* the chosen one keeps its place in the list when others come and go */
        if (a->rows[i].id == keep) {
            a->sel = i;
            break;
        }
    clamp(a);
}

static void send_set(struct app *a, const char *key, const char *value)
{
    WLC_SEND(a->win.c, a->settings, LM_MELLAGUI_SETTINGS_V1_REQ_SET, { .s = key }, { .s = value });
}

static void settings_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)c; (void)id;
    struct app *a = data;
    struct wlwin *w = &a->win;
    if (op == LM_MELLAGUI_SETTINGS_V1_EVT_DONE) {
        rebuild(a);
        if (a->unread > 0 && a->settings) {                    /* it is being looked at: nothing here is unread */
            a->unread = 0;
            send_set(a, "notes-read", "1");
        }
        w->redraw = true;
        return;
    }
    if (op != LM_MELLAGUI_SETTINGS_V1_EVT_VALUE)
        return;
    const char *k = args[0].s;
    if (!strncmp(k, "notes-", 6) && k[6] >= '0' && k[6] <= '4' && !k[7])
        snprintf(a->chunk[k[6] - '0'], sizeof(a->chunk[0]), "%s", args[1].s);
    else if (!strcmp(k, "notes-unread"))
        a->unread = atoi(args[1].s);
    else if (!strcmp(k, "dnd"))
        a->dnd = atoi(args[1].s) != 0;
}

static void global_event(struct wlwin *w, uint32_t name, const char *iface, uint32_t ver)
{
    (void)ver;
    struct app *a = w->data;
    if (!strcmp(iface, "mellagui_settings_v1"))
        a->settings = wlc_bind(w->c, w->registry, name, &lm_mellagui_settings_v1_interface, 1, settings_event, a);
}

/* Geometry of the footer buttons and the x of a row. */
struct box { int x, y, w, h; };

static struct box btn_dnd(const struct app *a)
{
    return (struct box){ PAD, a->win.bh - FOOT + 6, 190, 26 };
}

static struct box btn_clear(const struct app *a)
{
    return (struct box){ PAD + 200, a->win.bh - FOOT + 6, 110, 26 };
}

static bool inside(double px, double py, int x, int y, int w, int h)
{
    return px >= x && px < x + w && py >= y && py < y + h;
}

static void wrap_text(struct wlwin *w, int x, int y, int max_w, const char *s, int lines, uint32_t col)
{
    char buf[200];
    snprintf(buf, sizeof(buf), "%s", s);
    const char *p = buf;
    for (int i = 0; i < lines && *p; i++) {
        int fit = 0, last_space = -1, wpx = 0;
        const char *q = p;
        while (*q) {
            int cl = 1;
            while ((q[cl] & 0xc0) == 0x80)
                cl++;
            wpx += LM_FONT_W;
            if (wpx > max_w)
                break;
            if (*q == ' ')
                last_space = fit;
            fit += cl;
            q += cl;
        }
        if (!*q) {
            wlwin_text(w, x, y + i * 18, p, 1, col, 0);
            return;
        }
        int cut = last_space > 0 && i < lines - 1 ? last_space : fit;
        char one[200];
        snprintf(one, sizeof(one), "%.*s", cut, p);
        wlwin_text(w, x, y + i * 18, one, 1, col, 0);
        p += cut;
        while (*p == ' ')
            p++;
    }
}

static void paint(struct wlwin *w)
{
    struct app *a = w->data;
    wlwin_fill(w, 0, 0, w->bw, w->bh, lm_theme.background);
    char head[64];
    snprintf(head, sizeof(head), "Notifications (%d)", a->n);
    wlwin_text(w, PAD, 14, head, 1, lm_theme.accent, 0);
    if (a->dnd)
        wlwin_text(w, w->bw - PAD - 17 * LM_FONT_W, 14, "Do not disturb: on", 1, lm_theme.warn, 0);
    wlwin_fill(w, PAD, HEAD - 8, w->bw - 2 * PAD, 1, lm_theme.border);
    clamp(a);
    int vis = rows_visible(a);
    if (a->n == 0)
        wlwin_text(w, PAD, HEAD + 12, "Nothing here yet.", 1, lm_theme.text_dim, 0);
    for (int r = 0; r < vis && a->top + r < a->n; r++) {
        const struct row *row = &a->rows[a->top + r];
        int y = HEAD + r * ROW;
        bool sel = a->top + r == a->sel;
        if (sel)
            wlwin_fill(w, PAD - 6, y, w->bw - 2 * PAD + 12, ROW - 2, lm_theme.button);
        wlwin_text(w, PAD, y + 5, row->time, 1, lm_theme.text_dim, 0);
        wlwin_text(w, PAD + 6 * LM_FONT_W, y + 5, row->title, 1, lm_theme.text, w->bw - 2 * PAD - 8 * LM_FONT_W);
        wlwin_text(w, PAD + 6 * LM_FONT_W, y + 23, row->body, 1, lm_theme.text_soft, w->bw - 2 * PAD - 8 * LM_FONT_W);
        wlwin_text(w, w->bw - PAD - LM_FONT_W, y + 5, "x", 1, inside(a->px, a->py, w->bw - PAD - 18, y, 24, 20) ? lm_theme.alert : lm_theme.text_dim, 0);
    }
    int dy = w->bh - FOOT - DETAIL;
    wlwin_fill(w, PAD, dy, w->bw - 2 * PAD, 1, lm_theme.border);
    if (a->n > 0 && a->sel < a->n)
        wrap_text(w, PAD, dy + 8, w->bw - 2 * PAD, a->rows[a->sel].body, 3, lm_theme.text_soft);
    struct box b = btn_dnd(a);
    wlwin_fill(w, b.x, b.y, b.w, b.h, lm_theme.button);
    wlwin_text(w, b.x + 8, b.y + 6, a->dnd ? "Do not disturb: turn off" : "Do not disturb: turn on", 1, lm_theme.text, b.w - 12);
    b = btn_clear(a);
    wlwin_fill(w, b.x, b.y, b.w, b.h, lm_theme.button);
    wlwin_text(w, b.x + 8, b.y + 6, "Clear all", 1, lm_theme.text, b.w - 12);
}

static void remove_selected(struct app *a)
{
    if (a->n > 0 && a->sel < a->n) {
        char id[16];
        snprintf(id, sizeof(id), "%d", a->rows[a->sel].id);
        send_set(a, "notes-del", id);
    }
}

static void pointer_event(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
    struct app *a = w->data;
    if (op == LM_WL_POINTER_EVT_MOTION || op == LM_WL_POINTER_EVT_ENTER) {
        a->px = lm_fixed_to_double(args[op == LM_WL_POINTER_EVT_ENTER ? 2 : 1].f);
        a->py = lm_fixed_to_double(args[op == LM_WL_POINTER_EVT_ENTER ? 3 : 2].f);
        w->redraw = true;
    } else if (op == LM_WL_POINTER_EVT_AXIS && args[1].u == 0) {
        double v = lm_fixed_to_double(args[2].f);
        a->top += v > 0 ? 2 : v < 0 ? -2 : 0;
        w->redraw = true;
    } else if (op == LM_WL_POINTER_EVT_BUTTON && args[2].u == 272 && args[3].u == 1) {
        struct box b = btn_dnd(a);
        if (inside(a->px, a->py, b.x, b.y, b.w, b.h)) {
            send_set(a, "dnd", a->dnd ? "0" : "1");
            return;
        }
        b = btn_clear(a);
        if (inside(a->px, a->py, b.x, b.y, b.w, b.h)) {
            send_set(a, "notes-clear", "1");
            return;
        }
        int r = ((int)a->py - HEAD) / ROW;
        if (a->py >= HEAD && r >= 0 && r < rows_visible(a) && a->top + r < a->n) {
            a->sel = a->top + r;
            if (a->px >= w->bw - PAD - 18)
                remove_selected(a);
            w->redraw = true;
        }
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
        a->sel++;
    else if (ks == LM_KS_Up)
        a->sel--;
    else if (ks == LM_KS_Page_Down)
        a->sel += page;
    else if (ks == LM_KS_Page_Up)
        a->sel -= page;
    else if (ks == LM_KS_Home)
        a->sel = 0;
    else if (ks == LM_KS_End)
        a->sel = MAX_ROWS;
    else if (ks == LM_KS_Delete || ks == LM_KS_BackSpace)
        remove_selected(a);
    else if (ks == 'c' || ks == 'C')
        send_set(a, "notes-clear", "1");
    else if (ks == 'd' || ks == 'D')
        send_set(a, "dnd", a->dnd ? "0" : "1");
    clamp(a);
    w->redraw = true;
}

static int selftest(void)
{
    struct app *a = calloc(1, sizeof(*a));
    snprintf(a->chunk[0], sizeof(a->chunk[0]), "7\x1f" "21:04\x1f" "Backup\x1f" "3 files copied\nbroken line\n6\x1f" "20:59\x1f" "Dock\x1f" "Saved");
    snprintf(a->chunk[1], sizeof(a->chunk[1]), "5\x1f" "09:00\x1f" "Old\x1f");
    rebuild(a);
    int bad = 0;
    if (a->n != 3 || a->rows[0].id != 7 || strcmp(a->rows[0].time, "21:04") || strcmp(a->rows[0].title, "Backup") ||
        strcmp(a->rows[0].body, "3 files copied") || a->rows[1].id != 6 || a->rows[2].id != 5 || a->rows[2].body[0]) {
        fprintf(stderr, "mellagui-notes: selftest failed (%d rows)\n", a->n);
        bad = 1;
    }
    free(a);
    if (!bad)
        printf("mellagui-notes selftest ok\n");
    return bad;
}

int main(int argc, char **argv)
{
    lm_font_ui_cells(true);             /* this program lays its text out in columns of characters */
    lm_theme_load_env();
    if (argc > 1 && !strcmp(argv[1], "--selftest"))
        return selftest();
    static const struct wlwin_ops ops = { .paint = paint, .global = global_event, .pointer = pointer_event, .keyboard = keyboard_event };
    static struct app a;
    if (wlwin_connect(&a.win, "mellagui-notes", &ops, &a) < 0)
        return 1;
    if (!a.settings) {
        fprintf(stderr, "mellagui-notes: the compositor offers no mellagui_settings_v1 (or not to this program)\n");
        wlwin_destroy(&a.win);
        return 1;
    }
    if (wlwin_open(&a.win, "Notifications", "mellagui.notes", 520, 480, 380, 300) < 0)
        return 1;
    if (wlc_roundtrip(a.win.c) < 0) {
        wlwin_destroy(&a.win);
        return 1;
    }
    while (wlwin_step(&a.win, -1))
        ;
    wlwin_destroy(&a.win);
    return 0;
}
