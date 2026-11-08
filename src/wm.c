/*
 * Window manager + shell policy. See wm.h for the contract.
 */
#include "wm.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "backend.h"
#include "input.h"
#include "font.h"
#include "keymap.h"
#include "log.h"
#include "mixer.h"
#include "sysinfo.h"
#include "output.h"
#include "render.h"
#include "screenshot.h"
#include "seat.h"
#include "settings.h"
#include "surface.h"
#include "xdg.h"

#define RESIZE_CORNER 14
#define DOUBLE_CLICK_MS 400
#define PANEL_BTN_W 72
#define PANEL_WS_W 24
#define PANEL_CLOCK_W 136                       /* "DD.MM.YYYY HH:MM" = 16 glyphs */
#define PANEL_POWER_D (LM_PANEL_HEIGHT - 4)     /* the round power button's diameter: as tall as the other buttons */
#define POWER_CARD_W 264                        /* the session dialog */
#define POWER_CARD_H 172
#define POWER_BTN_D 64                          /* its round buttons */
#define PANEL_RUN_W 44
#define PANEL_LAYOUT_W 36
#define PANEL_STICKY_W 84
#define PANEL_CAPS_W 48
#define SW_MAX_WINDOWS 32
#define MENU_W 200
#define GADGET_W 250                /* default size (5 entries of 1 line; taller cards give the entries more lines) */
#define GADGET_H (GADGET_TITLE_H + LM_GADGET_ENTRIES * GADGET_ENTRY_H + GADGET_PAD)
#define GADGET_MIN_W 180
#define GADGET_MAX_W 640
#define GADGET_MIN_H (GADGET_TITLE_H + GADGET_ENTRY_H + GADGET_PAD)
#define GADGET_MAX_H 560            /* taller than 5 slots: each entry gets more lines */
#define GADGET_TITLE_H 26
#define GADGET_ENTRY_H 36
#define GADGET_PAD 8
#define GADGET_MARGIN 12
#define SYS_W 250
#define SYS_H 300                   /* title, CPU, graph, cores, memory, swap, temperature, the device rows that fit */
#define SYS_MIN_W 210
#define SYS_MIN_H 130
#define MIXER_W 250
#define MIXER_H 196                 /* title, master, output, two application rows, hint */
#define MIXER_MIN_W 210
#define MIXER_MIN_H 120
#define GADGET_SNAP 14              /* a dragged gadget sticks to another gadget's / the screen's edge within this many pixels */
#define GADGET_EDGE 6               /* resize zone along the left/right/bottom edge (top: 3 px) */
#define MENU_ITEM_H 26
#define PANEL_TASK_MAX_W 160
#define PANEL_TASK_GAP 2

struct wm_win {
    struct lm_window pub;       /* must be first */
    int id;
    bool placed;
    /* geometry to return to when leaving maximized/fullscreen */
    int32_t rx, ry, rw, rh;
    bool have_restore;
    uint32_t tiled;             /* LM_EDGE_LEFT / LM_EDGE_RIGHT half-screen tile, 0 = none */
    uint64_t focus_seq;         /* when it last had the focus (Alt+Tab lists the most recent first) */
};

struct wm_popup {
    struct lm_popup pub;        /* must be first */
};

enum grab_kind { GRAB_NONE, GRAB_MOVE, GRAB_RESIZE, GRAB_GADGET_MOVE, GRAB_GADGET_RESIZE, GRAB_MIXER };

struct lm_wm {
    struct lm_server *srv;
    struct lm_list windows;     /* stacking order, bottom first; lm_window.link */
    struct lm_list popups;
    struct lm_window *focused;
    int workspace;
    int next_id;
    uint64_t focus_counter;
    int cascade;
    struct lm_timer *clock_timer;
    struct note { int id; char title[LM_NOTIFY_COLS * 4 + 1]; char body[200]; int64_t expire; } notes[LM_NOTIFY_MAX];   /* newest first */
    int nnotes, next_note_id;
    struct lm_timer *note_timer;
    char clock_text[24];

    /* hover / pressed state */
    struct lm_window *hover_win;
    uint32_t hover_bits;
    int hover_panel_ws;         /* -1 none */
    bool hover_launcher;
    bool hover_power, hover_run, hover_layout, hover_sticky;
    bool power_open;            /* the session dialog (Lock / Exit) is showing on the output called power_out */
    char power_out[32];
    int power_sel;              /* its highlighted button: 0 Lock, 1 Exit, -1 none yet */
    bool menu_open;
    /* Alt+Tab: the list is open while Alt is held; windows most recently used first */
    bool sw_open;
    int sw_n, sw_idx;
    struct lm_window *sw_win[SW_MAX_WINDOWS];
    bool clip_nav;              /* keyboard mode of the clipboard gadget (Super+C): arrows move, Enter uses the entry */
    int clip_cur;               /* the highlighted entry while clip_nav */
    struct {                    /* the desktop gadgets, indexed by enum lm_gadget_id */
        bool visible;
        bool placed;            /* false: docked top right (default); true: x/y chosen by dragging */
        int32_t x, y, w, h;
        uint32_t click_time;
    } gad[LM_GADGET_COUNT];
    int grab_gadget;            /* the gadget being moved / resized / operated */
    enum lm_mixer_part mix_part;/* GRAB_MIXER: the slider being dragged ... */
    int mix_index;              /* ... and its stream row */
    struct lm_rect menu_rect;   /* valid while menu_open */
    int hover_menu;             /* -1 none */
    struct lm_window *hover_task;
    struct lm_window *press_win;
    uint32_t press_bits;
    int press_panel_kind;       /* enum lm_pick_kind of a pressed panel button, 0 none */
    int press_panel_ws;
    uint32_t last_click_time;
    struct lm_window *last_click_win;

    /* interactive grab */
    enum grab_kind grab_kind;
    struct lm_pointer_grab grab;
    struct lm_window *grab_win;
    double grab_px, grab_py;
    int32_t grab_x, grab_y, grab_w, grab_h;
    uint32_t grab_edges;
};

static inline struct wm_win *WIN(struct lm_window *w) { return (struct wm_win *)w; }

static void mixer_slide(struct lm_wm *wm, enum lm_mixer_part part, int idx, double x);

/* ---- geometry helpers ------------------------------------------------------------ */

static struct lm_rect frame_outer(const struct lm_window *w)
{
    if (w->states & LM_WINDOW_FULLSCREEN)
        return lm_rect_make(w->x, w->y, w->width, w->height);
    return lm_rect_make(w->x - LM_BORDER_WIDTH, w->y - LM_TITLEBAR_HEIGHT,
                        w->width + 2 * LM_BORDER_WIDTH,
                        w->height + LM_TITLEBAR_HEIGHT + LM_BORDER_WIDTH);
}

static bool win_visible(const struct lm_wm *wm, const struct lm_window *w)
{
    return w->mapped && !w->minimized && w->workspace == wm->workspace;
}

static void win_origin(const struct lm_window *w, int32_t *ox, int32_t *oy)
{
    *ox = w->x - w->geometry.x;
    *oy = w->y - w->geometry.y;
}

static void popup_origin(const struct lm_popup *p, int32_t *ox, int32_t *oy)
{
    *ox = p->x - p->geometry.x;
    *oy = p->y - p->geometry.y;
}

static void frame_info(const struct lm_wm *wm, const struct lm_window *w, struct lm_frame_info *f)
{
    memset(f, 0, sizeof(*f));
    f->outer = frame_outer(w);
    f->titlebar = lm_rect_make(f->outer.x, f->outer.y, f->outer.w, LM_TITLEBAR_HEIGHT);
    int by = f->outer.y + (LM_TITLEBAR_HEIGHT - LM_FRAME_BUTTON_SIZE) / 2;
    int right = f->outer.x + f->outer.w - LM_FRAME_BUTTON_GAP;
    f->btn_close = lm_rect_make(right - LM_FRAME_BUTTON_SIZE, by, LM_FRAME_BUTTON_SIZE, LM_FRAME_BUTTON_SIZE);
    f->btn_max = lm_rect_make(f->btn_close.x - LM_FRAME_BUTTON_GAP - LM_FRAME_BUTTON_SIZE, by,
                              LM_FRAME_BUTTON_SIZE, LM_FRAME_BUTTON_SIZE);
    f->btn_min = lm_rect_make(f->btn_max.x - LM_FRAME_BUTTON_GAP - LM_FRAME_BUTTON_SIZE, by,
                              LM_FRAME_BUTTON_SIZE, LM_FRAME_BUTTON_SIZE);
    int tx = f->outer.x + LM_BORDER_WIDTH;
    f->title_text = lm_rect_make(tx, f->outer.y, f->btn_min.x - LM_FRAME_BUTTON_GAP - tx, LM_TITLEBAR_HEIGHT);
    f->title = w->title;
    f->active = wm->focused == w;
    f->maximized = (w->states & LM_WINDOW_MAXIMIZED) != 0;
    if (wm->hover_win == w)
        f->hover = wm->hover_bits;
    if (wm->press_win == w)
        f->pressed = wm->press_bits;
}

static struct lm_rect output_panel_rect(const struct lm_output *o)
{
    return lm_rect_make(o->x, o->y, o->width, LM_PANEL_HEIGHT);
}

static void panel_info(const struct lm_wm *wm, struct lm_output *o, struct lm_panel_info *p)
{
    memset(p, 0, sizeof(*p));
    p->output = o;
    p->bar = output_panel_rect(o);
    int y = p->bar.y + 2, h = p->bar.h - 4;
    p->launcher = lm_rect_make(p->bar.x + 4, y, PANEL_BTN_W, h);
    int x = p->launcher.x + p->launcher.w + 10;
    for (int i = 0; i < LM_NUM_WORKSPACES; i++) {
        p->workspace[i] = lm_rect_make(x, y, PANEL_WS_W, h);
        x += PANEL_WS_W + 2;
    }
    p->power = lm_rect_make(p->bar.x + p->bar.w - PANEL_POWER_D - 4, y, PANEL_POWER_D, PANEL_POWER_D);
    p->clock = lm_rect_make(p->power.x - 4 - PANEL_CLOCK_W, y, PANEL_CLOCK_W, h);
    p->run = lm_rect_make(p->clock.x - 8 - PANEL_RUN_W, y, PANEL_RUN_W, h);
    p->sticky = lm_rect_make(p->run.x - 8 - PANEL_STICKY_W, y, PANEL_STICKY_W, h);
    p->layout = lm_rect_make(p->sticky.x - 4 - PANEL_LAYOUT_W, y, PANEL_LAYOUT_W, h);
    uint32_t locks = wm->srv->input ? lm_input_locks(wm->srv->input) : 0;
    p->caps_text[0] = 0;
    if ((locks & LM_MOD_CAPS) && (locks & LM_MOD_NUM))
        snprintf(p->caps_text, sizeof(p->caps_text), "CAPS+N");
    else if (locks & LM_MOD_CAPS)
        snprintf(p->caps_text, sizeof(p->caps_text), "CAPS");
    else if (locks & LM_MOD_NUM)
        snprintf(p->caps_text, sizeof(p->caps_text), "NUM");
    int caps_w = !p->caps_text[0] ? 0 : (locks & LM_MOD_NUM) && (locks & LM_MOD_CAPS) ? PANEL_CAPS_W + 24 : PANEL_CAPS_W;
    p->caps = lm_rect_make(p->layout.x - (caps_w ? 4 + caps_w : 0), y, caps_w, h);
    int tx = x + 12;
    p->title_area = lm_rect_make(tx, y, p->caps.x - 12 - tx, h);
    p->hover_layout = wm->hover_layout;
    p->hover_sticky = wm->hover_sticky;
    snprintf(p->layout_text, sizeof(p->layout_text), "us");
    snprintf(p->sticky_text, sizeof(p->sticky_text), "Sticky");
    if (wm->srv->input) {
        const char *id = lm_input_layout_id(wm->srv->input, lm_input_layout(wm->srv->input));
        for (size_t i = 0; i < sizeof(p->layout_text) - 1 && id[i]; i++)
            p->layout_text[i] = (char)(id[i] >= 'a' && id[i] <= 'z' ? id[i] - 32 : id[i]);
        p->layout_text[strnlen(id, sizeof(p->layout_text) - 1)] = 0;
        p->sticky_on = lm_input_sticky(wm->srv->input);
        uint32_t lat = lm_input_latched(wm->srv->input), lck = lm_input_sticky_locked(wm->srv->input);
        static const struct { uint32_t bit; const char *lo, *up; } names[] = {
            { LM_MOD_SHIFT, "Sh", "SH" }, { LM_MOD_CTRL, "Ct", "CT" }, { LM_MOD_ALT, "Al", "AL" },
            { LM_MOD_ALTGR, "Gr", "GR" }, { LM_MOD_SUPER, "Su", "SU" },
        };
        char txt[24] = "";
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            if (!((lat | lck) & names[i].bit))
                continue;
            if (txt[0])
                strncat(txt, "+", sizeof(txt) - strlen(txt) - 1);
            strncat(txt, (lck & names[i].bit) ? names[i].up : names[i].lo, sizeof(txt) - strlen(txt) - 1);
        }
        if (p->sticky_on && txt[0])
            snprintf(p->sticky_text, sizeof(p->sticky_text), "%s", txt);
    }
    p->current_workspace = wm->workspace;
    struct lm_window *w;
    lm_list_for_each(w, &wm->windows, link)
        if (w->mapped)
            p->workspace_occupied |= 1u << w->workspace;
    p->hover_workspace = wm->hover_panel_ws;
    p->hover_launcher = wm->hover_launcher;
    p->hover_power = wm->hover_power;
    p->power_open = wm->power_open;
    p->hover_run = wm->hover_run;
    p->active_title = wm->focused ? wm->focused->title : "";

    /* taskbar: windows of this workspace in creation (id) order, so raising does not reshuffle it */
    struct lm_window *tw[LM_PANEL_MAX_TASKS];
    int n = 0;
    lm_list_for_each(w, &wm->windows, link) {
        if (!w->mapped || w->workspace != wm->workspace || n >= LM_PANEL_MAX_TASKS)
            continue;
        int i = n++;
        for (; i > 0 && WIN(tw[i - 1])->id > WIN(w)->id; i--)
            tw[i] = tw[i - 1];
        tw[i] = w;
    }
    p->ntasks = n;
    if (n > 0) {
        int bw = (p->title_area.w - (n - 1) * PANEL_TASK_GAP) / n;
        bw = LM_MIN(bw, PANEL_TASK_MAX_W);
        if (bw <= 0)
            p->ntasks = 0;
        for (int i = 0; i < p->ntasks; i++) {
            p->task[i].rect = lm_rect_make(p->title_area.x + i * (bw + PANEL_TASK_GAP), y, bw, h);
            p->task[i].title = tw[i]->title;
            p->task[i].active = wm->focused == tw[i];
            p->task[i].minimized = tw[i]->minimized;
            p->task[i].hover = wm->hover_task == tw[i];
            p->task_win[i] = tw[i];
        }
    }
    snprintf(p->clock_text, sizeof(p->clock_text), "%s", wm->clock_text);
}

static bool fullscreen_covers(const struct lm_wm *wm, const struct lm_output *o)
{
    struct lm_window *w;
    lm_list_for_each(w, &wm->windows, link) {
        if (!win_visible(wm, w) || !(w->states & LM_WINDOW_FULLSCREEN))
            continue;
        struct lm_rect r = lm_rect_make(w->x, w->y, w->width, w->height);
        struct lm_rect orect = lm_output_rect(o);
        if (lm_rect_overlaps(&r, &orect))
            return true;
    }
    return false;
}

/* ---- damage ---------------------------------------------------------------------------- */

static void damage_rect(struct lm_wm *wm, const struct lm_rect *r)
{
    if (wm->srv->render && !lm_rect_is_empty(r))
        lm_render_damage(wm->srv->render, r);
}

static void damage_frame(struct lm_wm *wm, const struct lm_window *w)
{
    struct lm_rect r = frame_outer(w);
    damage_rect(wm, &r);
}

static void damage_all(struct lm_wm *wm)
{
    if (wm->srv->render)
        lm_render_damage_all(wm->srv->render);
}

static struct lm_output *gadget_output(const struct lm_wm *wm);

/* ---- notifications ---------------------------------------------------------------------------------------------------- */

#define NOTE_W 320
#define NOTE_GAP 6

static int64_t now_ms64(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Wraps `body` into at most LM_NOTIFY_LINES lines of LM_NOTIFY_COLS characters (breaking at blanks, "..." when it is cut). */
static int note_wrap(const char *body, char line[LM_NOTIFY_LINES][LM_NOTIFY_COLS * 4 + 1])
{
    int n = 0;
    const char *p = body;
    while (*p == ' ')
        p++;
    while (*p && n < LM_NOTIFY_LINES) {
        const char *q = p;
        int cols = 0, last_blank = -1;
        const char *start = p;
        while (*q && *q != '\n' && cols < LM_NOTIFY_COLS) {
            if (*q == ' ')
                last_blank = (int)(q - start);
            do
                q++;
            while ((*q & 0xc0) == 0x80);
            cols++;
        }
        size_t len = (size_t)(q - start);
        if (*q && *q != '\n' && *q != ' ' && last_blank > 0)
            len = (size_t)last_blank;                                   /* break at the last blank */
        if (len >= sizeof(line[0]))
            len = sizeof(line[0]) - 1;
        memcpy(line[n], start, len);
        line[n][len] = 0;
        n++;
        p = start + len;
        while (*p == ' ' || *p == '\n')
            p++;
    }
    if (*p && n > 0) {                                                 /* text left over: mark the cut */
        size_t l = strlen(line[n - 1]);
        if (l + 3 < sizeof(line[0]) && l >= 3) {
            line[n - 1][l - 3] = 0;
            strcat(line[n - 1], "...");
        }
    }
    return n;
}

static int note_height(const struct note *nt)
{
    char line[LM_NOTIFY_LINES][LM_NOTIFY_COLS * 4 + 1];
    int n = note_wrap(nt->body, line);
    return 8 + 20 + n * LM_FONT_H + (n ? 6 : 0) + 6;
}

/* The card rectangles, newest first, stacked down from the top right of the first output; false without an output. */
static bool note_rects(const struct lm_wm *wm, struct lm_rect *r)
{
    struct lm_output *o = gadget_output(wm);
    if (!o)
        return false;
    int y = o->y + LM_PANEL_HEIGHT + 8;
    for (int i = 0; i < wm->nnotes; i++) {
        int h = note_height(&wm->notes[i]);
        r[i] = lm_rect_make(o->x + o->width - NOTE_W - 12, y, NOTE_W, h);
        y += h + NOTE_GAP;
    }
    return true;
}

static void note_damage(struct lm_wm *wm)
{
    struct lm_rect r[LM_NOTIFY_MAX];
    if (!note_rects(wm, r))
        return;
    for (int i = 0; i < wm->nnotes; i++)
        damage_rect(wm, &r[i]);
}

static void note_timer_arm(struct lm_wm *wm)
{
    if (!wm->note_timer)
        return;
    int64_t soon = 0, now = now_ms64();
    for (int i = 0; i < wm->nnotes; i++)
        if (wm->notes[i].expire && (!soon || wm->notes[i].expire < soon))
            soon = wm->notes[i].expire;
    if (soon)
        lm_timer_arm_ms(wm->note_timer, (uint32_t)LM_MAX(soon - now, 1), 0);
}

static void note_remove(struct lm_wm *wm, int idx)
{
    note_damage(wm);                                                   /* the stack closes up: everything below moves */
    memmove(&wm->notes[idx], &wm->notes[idx + 1], sizeof(wm->notes[0]) * (size_t)(wm->nnotes - idx - 1));
    wm->nnotes--;
    note_damage(wm);
}

static void note_dismiss(struct lm_wm *wm, int id)
{
    for (int i = 0; i < wm->nnotes; i++)
        if (wm->notes[i].id == id) {
            note_remove(wm, i);
            note_timer_arm(wm);
            return;
        }
}

static void note_tick(void *data)
{
    struct lm_wm *wm = data;
    int64_t now = now_ms64();
    for (int i = wm->nnotes - 1; i >= 0; i--)
        if (wm->notes[i].expire && wm->notes[i].expire <= now)
            note_remove(wm, i);
    note_timer_arm(wm);
}

void lm_wm_notify(struct lm_wm *wm, const char *title, const char *body, int timeout_ms)
{
    if (!wm || !gadget_output(wm))
        return;
    note_damage(wm);
    if (wm->nnotes == LM_NOTIFY_MAX)                                   /* the oldest (last) one makes room */
        wm->nnotes--;
    memmove(&wm->notes[1], &wm->notes[0], sizeof(wm->notes[0]) * (size_t)wm->nnotes);
    struct note *nt = &wm->notes[0];
    memset(nt, 0, sizeof(*nt));
    nt->id = ++wm->next_note_id;
    snprintf(nt->title, sizeof(nt->title), "%.*s", LM_NOTIFY_COLS * 4, title ? title : "");
    snprintf(nt->body, sizeof(nt->body), "%s", body ? body : "");
    for (char *c = nt->title; *c; c++)
        if ((unsigned char)*c < 0x20)
            *c = ' ';
    nt->expire = timeout_ms > 0 ? now_ms64() + timeout_ms : 0;
    wm->nnotes++;
    note_damage(wm);
    note_timer_arm(wm);
}

/* ---- Alt+Tab window switcher ------------------------------------------------------------------------------------------ */

#define SW_ITEM_H 24

static int sw_first(const struct lm_wm *wm)            /* index of the first entry shown (the list scrolls to keep the selection) */
{
    if (wm->sw_n <= LM_SW_MAX)
        return 0;
    return LM_CLAMP(wm->sw_idx - LM_SW_MAX / 2, 0, wm->sw_n - LM_SW_MAX);
}

static const char *sw_label(const struct lm_window *w)
{
    return w->title[0] ? w->title : w->app_id[0] ? w->app_id : "(untitled)";
}

static struct lm_rect sw_rect(const struct lm_wm *wm)
{
    struct lm_output *o = NULL;
    if (wm->srv->backend && wm->srv->seat) {
        double x, y;
        lm_seat_pointer_position(wm->srv->seat, &x, &y);
        o = lm_output_nearest(wm->srv, x, y);
    }
    if (!o)
        return lm_rect_make(0, 0, 0, 0);
    int n = LM_MIN(wm->sw_n, LM_SW_MAX), first = sw_first(wm), chars = 12;
    for (int i = 0; i < n; i++)
        chars = LM_MAX(chars, LM_MIN((int)strlen(sw_label(wm->sw_win[first + i])), 56));
    int w = LM_MIN(chars * LM_FONT_W + 24 + 6 * LM_FONT_W, o->width - 16), h = n * SW_ITEM_H + 8;
    return lm_rect_make(o->x + (o->width - w) / 2, o->y + (o->height - h) / 2, w, h);
}

static void sw_close(struct lm_wm *wm)
{
    if (!wm->sw_open)
        return;
    struct lm_rect r = sw_rect(wm);
    wm->sw_open = false;
    wm->sw_n = 0;
    damage_rect(wm, &r);
}

static void sw_step(struct lm_wm *wm, bool backwards)
{
    struct lm_rect before = wm->sw_open ? sw_rect(wm) : lm_rect_make(0, 0, 0, 0);
    if (!wm->sw_open) {
        wm->sw_n = 0;
        struct lm_window *w;
        lm_list_for_each_reverse(w, &wm->windows, link)           /* top of the stack first, then by when it last had the focus */
            if (w->mapped && wm->sw_n < SW_MAX_WINDOWS)
                wm->sw_win[wm->sw_n++] = w;
        for (int i = 1; i < wm->sw_n; i++) {                      /* stable insertion sort, most recently focused first */
            struct lm_window *t = wm->sw_win[i];
            int j = i;
            while (j > 0 && WIN(wm->sw_win[j - 1])->focus_seq < WIN(t)->focus_seq) {
                wm->sw_win[j] = wm->sw_win[j - 1];
                j--;
            }
            wm->sw_win[j] = t;
        }
        if (wm->sw_n < 2)
            return;
        wm->sw_open = true;
        wm->sw_idx = backwards ? wm->sw_n - 1 : 1;
    } else {
        wm->sw_idx = (wm->sw_idx + (backwards ? wm->sw_n - 1 : 1)) % wm->sw_n;
    }
    struct lm_rect now = sw_rect(wm);
    damage_rect(wm, &now);
    if (before.w > 0)
        damage_rect(wm, &before);
}

static void sw_commit(struct lm_wm *wm)
{
    struct lm_window *w = wm->sw_open ? wm->sw_win[wm->sw_idx] : NULL;
    sw_close(wm);
    if (w)
        lm_wm_focus_window(wm, w);
}

static void menu_set(struct lm_wm *wm, bool open, const struct lm_rect *launcher)
{
    if (open == wm->menu_open)
        return;
    if (open) {
        int n = wm->srv->config.napps;
        if (n <= 0 || !launcher)
            return;
        wm->menu_rect = lm_rect_make(launcher->x, launcher->y + launcher->h + 2, MENU_W, n * MENU_ITEM_H + 4);
        wm->menu_open = true;
        damage_rect(wm, &wm->menu_rect);
    } else {
        wm->menu_open = false;
        wm->hover_menu = -1;
        damage_rect(wm, &wm->menu_rect);
    }
}

static struct lm_output *gadget_output(const struct lm_wm *wm)
{
    return lm_output_primary(wm->srv);
}

/* Default / minimum / maximum card size per gadget. */
static const struct { int w, h, min_w, min_h, max_w, max_h; } gadget_def[LM_GADGET_COUNT] = {
    [LM_GADGET_CLIPBOARD] = { GADGET_W, GADGET_H, GADGET_MIN_W, GADGET_MIN_H, GADGET_MAX_W, GADGET_MAX_H },
    [LM_GADGET_MIXER] = { MIXER_W, MIXER_H, MIXER_MIN_W, MIXER_MIN_H, GADGET_MAX_W, GADGET_MAX_H },
    [LM_GADGET_SYSTEM] = { SYS_W, SYS_H, SYS_MIN_W, SYS_MIN_H, GADGET_MAX_W, GADGET_MAX_H },
};

/* Largest size gadget `id` may have on `o` (it stays inside the output, below the panel). */
/* ---- the gadgets' places and sizes, kept between runs ------------------------------------------------------------------ */

static const char *const gadget_names[LM_GADGET_COUNT] = { "clipboard", "mixer", "system" };

/* One line per gadget that was moved or resized: "name x y w h". A gadget at its default place is not listed. */
static void gadgets_save(struct lm_wm *wm)
{
    const char *path = wm->srv->config.gadgets_path;
    if (!path || !*path)
        return;
    char dir[512], tmp[560];
    if (snprintf(dir, sizeof(dir), "%s", path) >= (int)sizeof(dir) ||
        snprintf(tmp, sizeof(tmp), "%s.%d.tmp", path, (int)getpid()) >= (int)sizeof(tmp))
        return;
    for (char *p = dir + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = 0;
        if (mkdir(dir, 0755) < 0 && errno != EEXIST)
            return;
        *p = '/';
    }
    FILE *f = fopen(tmp, "w");
    if (!f) {
        lm_warn("gadgets: cannot write %s: %s", tmp, strerror(errno));
        return;
    }
    for (int i = 0; i < LM_GADGET_COUNT; i++)
        if (wm->gad[i].placed)
            fprintf(f, "%s %d %d %d %d\n", gadget_names[i], wm->gad[i].x, wm->gad[i].y, wm->gad[i].w, wm->gad[i].h);
    bool ok = !ferror(f);
    if (fclose(f) != 0)
        ok = false;
    if (!ok || rename(tmp, path) < 0) {
        lm_warn("gadgets: cannot save %s: %s", path, strerror(errno));
        unlink(tmp);
    }
}

static void gadgets_load(struct lm_wm *wm)
{
    const char *path = wm->srv->config.gadgets_path;
    FILE *f = path && *path ? fopen(path, "r") : NULL;
    if (!f)
        return;
    char line[160], name[24];
    while (fgets(line, sizeof(line), f)) {
        int x, y, w, h;
        if (sscanf(line, "%23s %d %d %d %d", name, &x, &y, &w, &h) != 5)
            continue;
        for (int i = 0; i < LM_GADGET_COUNT; i++) {
            if (strcmp(name, gadget_names[i]))
                continue;
            if (abs(x) > 20000 || abs(y) > 20000)
                break;
            wm->gad[i].x = x;                                       /* (the places are clamped to the output when drawn) */
            wm->gad[i].y = y;
            wm->gad[i].w = LM_CLAMP(w, gadget_def[i].min_w, gadget_def[i].max_w);
            wm->gad[i].h = LM_CLAMP(h, gadget_def[i].min_h, gadget_def[i].max_h);
            wm->gad[i].placed = true;
            break;
        }
    }
    fclose(f);
}

static void gadget_max_size(int id, const struct lm_output *o, int *mw, int *mh)
{
    *mw = LM_MIN(gadget_def[id].max_w, o->width - 2 * GADGET_MARGIN);
    *mh = LM_MIN(gadget_def[id].max_h, o->height - LM_PANEL_HEIGHT - 2 * GADGET_MARGIN);
}

/* Where gadget `id` sits: the first output, docked top right below the panel until it is dragged (the mixer docks below
 * the clipboard card while that one is docked too). False when hidden or it does not fit. */
bool lm_wm_gadget_rect_id(const struct lm_wm *wm, int id, struct lm_rect *out)
{
    struct lm_output *o = gadget_output(wm);
    if (id < 0 || id >= LM_GADGET_COUNT || !wm->gad[id].visible || !o)
        return false;
    int mw, mh;
    gadget_max_size(id, o, &mw, &mh);
    if (mw < gadget_def[id].min_w || mh < gadget_def[id].min_h || fullscreen_covers(wm, o))
        return false;
    int w = LM_CLAMP(wm->gad[id].w, gadget_def[id].min_w, mw), h = LM_CLAMP(wm->gad[id].h, gadget_def[id].min_h, mh);
    int x, y;
    if (wm->gad[id].placed) {
        x = LM_CLAMP(wm->gad[id].x, o->x, o->x + o->width - w);
        y = LM_CLAMP(wm->gad[id].y, o->y + LM_PANEL_HEIGHT, o->y + o->height - h);
    } else {
        x = o->x + o->width - w - GADGET_MARGIN;
        y = o->y + LM_PANEL_HEIGHT + GADGET_MARGIN;
        /* docked gadgets stack downwards from the top right in id order; when the next one does not fit below the
         * previous one, it starts a new column to its left */
        for (int j = id - 1; j >= 0; j--) {
            struct lm_rect pr;
            if (wm->gad[j].placed || !lm_wm_gadget_rect_id(wm, j, &pr))
                continue;
            if (pr.y + pr.h + GADGET_MARGIN + h <= o->y + o->height - GADGET_MARGIN) {
                y = pr.y + pr.h + GADGET_MARGIN;                        /* below the previous card */
            } else if (pr.x - GADGET_MARGIN - w >= o->x) {
                x = pr.x - GADGET_MARGIN - w;                           /* a small screen: beside it, at the top */
                y = o->y + LM_PANEL_HEIGHT + GADGET_MARGIN;
            } else {                                                    /* no room at all: cascade, titles stay visible */
                x = LM_MAX(o->x, pr.x - 28);
                y = LM_MIN(pr.y + 28, o->y + o->height - h);
            }
            break;
        }
    }
    *out = lm_rect_make(x, y, w, h);
    return true;
}

/* The card plus its drop shadow (what has to be repainted). */
static struct lm_rect gadget_bounds(const struct lm_rect *r)
{
    return lm_rect_make(r->x - LM_GADGET_SHADOW, r->y - LM_GADGET_SHADOW, r->w + 2 * LM_GADGET_SHADOW,
                        r->h + 2 * LM_GADGET_SHADOW);
}

/* Entry slots of a card of height `h`: as many as fit (1..5); with room for all five the slots grow (more lines). */
static void gadget_slots(int h, int *slots, int *entry_h)
{
    int body = h - GADGET_TITLE_H - GADGET_PAD;
    int n = LM_CLAMP(body / GADGET_ENTRY_H, 1, LM_GADGET_ENTRIES);
    *slots = n;
    *entry_h = n == LM_GADGET_ENTRIES ? body / LM_GADGET_ENTRIES : GADGET_ENTRY_H;
}

static void damage_gadget(struct lm_wm *wm, int id)
{
    struct lm_rect r;
    if (lm_wm_gadget_rect_id(wm, id, &r)) {
        struct lm_rect b = gadget_bounds(&r);
        damage_rect(wm, &b);
    }
}

/* A docked mixer sits below the clipboard card: when that one appears, disappears or changes size, repaint both. */
static void damage_gadgets(struct lm_wm *wm)
{
    for (int i = 0; i < LM_GADGET_COUNT; i++)
        damage_gadget(wm, i);
}

/* Entries the keyboard can reach: those drawn in the card. */
static int clip_nav_count(struct lm_wm *wm)
{
    struct lm_rect gr;
    if (!lm_wm_gadget_rect_id(wm, LM_GADGET_CLIPBOARD, &gr))
        return 0;
    int slots, entry_h;
    gadget_slots(gr.h, &slots, &entry_h);
    return LM_MIN(lm_seat_clip_count(wm->srv->seat), slots);
}

static void clip_nav_set(struct lm_wm *wm, bool on)
{
    if (wm->clip_nav == on)
        return;
    wm->clip_nav = on;
    wm->clip_cur = 0;
    damage_gadget(wm, LM_GADGET_CLIPBOARD);
}

void lm_wm_clipboard_changed(struct lm_wm *wm)
{
    if (wm->clip_nav) {                                         /* the list moved under the cursor: keep it inside */
        int n = clip_nav_count(wm);
        if (n == 0)
            wm->clip_nav = false;
        else
            wm->clip_cur = LM_CLAMP(wm->clip_cur, 0, n - 1);
    }
    damage_gadget(wm, LM_GADGET_CLIPBOARD);
}

void lm_wm_mixer_changed(struct lm_wm *wm)
{
    damage_gadget(wm, LM_GADGET_MIXER);
}

void lm_wm_sysinfo_changed(struct lm_wm *wm)
{
    damage_gadget(wm, LM_GADGET_SYSTEM);
}

bool lm_wm_gadget_visible_id(const struct lm_wm *wm, int id)
{
    return id >= 0 && id < LM_GADGET_COUNT && wm->gad[id].visible;
}

bool lm_wm_gadget_visible(const struct lm_wm *wm)
{
    return lm_wm_gadget_visible_id(wm, LM_GADGET_CLIPBOARD);
}

void lm_wm_set_gadget_id(struct lm_wm *wm, int id, bool visible)
{
    if (id < 0 || id >= LM_GADGET_COUNT || wm->gad[id].visible == visible)
        return;
    damage_gadgets(wm);                 /* (still shown, if it was) */
    wm->gad[id].visible = visible;
    damage_gadgets(wm);
    if (id == LM_GADGET_MIXER && wm->srv->mixer)
        lm_mixer_set_active(wm->srv->mixer, visible);
    if (id == LM_GADGET_SYSTEM && wm->srv->sysinfo)
        lm_sysinfo_set_active(wm->srv->sysinfo, visible);
    lm_settings_changed(wm->srv->settings);     /* the panel's checkbox follows Super+G / Super+V */
}

void lm_wm_set_gadget(struct lm_wm *wm, bool visible)
{
    lm_wm_set_gadget_id(wm, LM_GADGET_CLIPBOARD, visible);
}

static void damage_panels(struct lm_wm *wm)
{
    if (!wm->srv->backend)
        return;
    struct lm_output *o;
    lm_list_for_each(o, &wm->srv->backend->outputs, link) {
        struct lm_rect r = output_panel_rect(o);
        damage_rect(wm, &r);
    }
}

/* ---- surface output membership --------------------------------------------------------- */

static void update_outputs(struct lm_wm *wm, struct lm_window *w)
{
    struct lm_output *outs[16];
    size_t n = 0;
    if (win_visible(wm, w) && wm->srv->backend) {
        struct lm_rect fr = frame_outer(w);
        struct lm_output *o;
        lm_list_for_each(o, &wm->srv->backend->outputs, link) {
            struct lm_rect orect = lm_output_rect(o);
            if (n < LM_ARRAY_LEN(outs) && lm_rect_overlaps(&fr, &orect))
                outs[n++] = o;
        }
    }
    lm_surface_set_outputs(w->surface, outs, n);
}

/* ---- usable area ------------------------------------------------------------------------- */

void lm_wm_usable_area(struct lm_wm *wm, int32_t gx, int32_t gy, struct lm_rect *out)
{
    struct lm_output *o = lm_output_nearest(wm->srv, gx, gy);
    if (!o) {
        *out = lm_rect_make(0, 0, 1280, 720);
        return;
    }
    struct lm_rect r = lm_output_rect(o);
    r.y += LM_PANEL_HEIGHT;
    r.h -= LM_PANEL_HEIGHT;
    *out = r;
}

/* ---- configure ---------------------------------------------------------------------------- */

static void send_configure(struct lm_wm *wm, struct lm_window *w, int32_t cw, int32_t ch)
{
    uint32_t st = w->states & (LM_WINDOW_MAXIMIZED | LM_WINDOW_FULLSCREEN | LM_WINDOW_RESIZING);
    if (wm->focused == w)
        st |= LM_WINDOW_ACTIVATED;
    w->states = st;
    if (w->ops && w->ops->configure)
        w->ops->configure(w, cw, ch, st);
}

static void clamp_size(const struct lm_window *w, int32_t *cw, int32_t *ch)
{
    int32_t minw = LM_MAX(w->min_w, LM_MIN_WINDOW_WIDTH), minh = LM_MAX(w->min_h, LM_MIN_WINDOW_HEIGHT);
    if (*cw < minw) *cw = minw;
    if (*ch < minh) *ch = minh;
    if (w->max_w > 0 && *cw > w->max_w) *cw = w->max_w;
    if (w->max_h > 0 && *ch > w->max_h) *ch = w->max_h;
}

static struct lm_output *window_output(struct lm_wm *wm, const struct lm_window *w)
{
    return lm_output_nearest(wm->srv, w->x + w->width / 2.0, w->y + w->height / 2.0);
}

/* Compute x/y/size for the current maximized/fullscreen/normal state and configure. */
static void apply_state(struct lm_wm *wm, struct lm_window *w)
{
    struct wm_win *ww = WIN(w);
    struct lm_rect before = frame_outer(w);
    int32_t tw = 0, th = 0;

    ww->tiled = 0;
    if (w->states & LM_WINDOW_FULLSCREEN) {
        struct lm_output *o = window_output(wm, w);
        if (o) {
            w->x = o->x;
            w->y = o->y;
            tw = o->width;
            th = o->height;
        }
    } else if (w->states & LM_WINDOW_MAXIMIZED) {
        struct lm_rect ua;
        lm_wm_usable_area(wm, w->x + w->width / 2, w->y + w->height / 2, &ua);
        w->x = ua.x + LM_BORDER_WIDTH;
        w->y = ua.y + LM_TITLEBAR_HEIGHT;
        tw = ua.w - 2 * LM_BORDER_WIDTH;
        th = ua.h - LM_TITLEBAR_HEIGHT - LM_BORDER_WIDTH;
    } else if (ww->have_restore) {
        w->x = ww->rx;
        w->y = ww->ry;
        tw = ww->rw;
        th = ww->rh;
        ww->have_restore = false;
    }
    /* Keep the old extents until the client commits the new size. */
    send_configure(wm, w, tw, th);
    damage_rect(wm, &before);
    damage_frame(wm, w);
    update_outputs(wm, w);
}

static void save_restore(struct wm_win *ww)
{
    struct lm_window *w = &ww->pub;
    if (ww->have_restore || (w->states & (LM_WINDOW_MAXIMIZED | LM_WINDOW_FULLSCREEN)))
        return;
    ww->rx = w->x;
    ww->ry = w->y;
    ww->rw = w->width;
    ww->rh = w->height;
    ww->have_restore = true;
}

/* ---- focus -------------------------------------------------------------------------------- */

static void refresh_pointer(struct lm_wm *wm)
{
    if (!wm->srv->seat || lm_seat_pointer_has_grab(wm->srv->seat))
        return;
    double x, y;
    lm_seat_pointer_position(wm->srv->seat, &x, &y);
    lm_seat_pointer_warp(wm->srv->seat, x, y);
}

static void set_focus(struct lm_wm *wm, struct lm_window *w)
{
    struct lm_window *old = wm->focused;
    if (old == w)
        return;
    wm->focused = w;
    if (w)
        WIN(w)->focus_seq = ++wm->focus_counter;
    if (old) {
        lm_xdg_dismiss_popups(old->surface);
        send_configure(wm, old, old->width, old->height);
        damage_frame(wm, old);
    }
    if (w) {
        send_configure(wm, w, w->width, w->height);
        damage_frame(wm, w);
    }
    if (wm->srv->seat)
        lm_seat_set_keyboard_focus(wm->srv->seat, w ? w->surface : NULL);
    damage_panels(wm);
}

/* Topmost visible window on the current workspace (excluding `skip`). */
static struct lm_window *top_visible(struct lm_wm *wm, const struct lm_window *skip)
{
    struct lm_window *w;
    lm_list_for_each_reverse(w, &wm->windows, link)
        if (w != skip && win_visible(wm, w))
            return w;
    return NULL;
}

static void raise_window(struct lm_wm *wm, struct lm_window *w)
{
    if (wm->windows.prev == &w->link)
        return;
    lm_list_remove(&w->link);
    lm_list_append(&wm->windows, &w->link);
    damage_frame(wm, w);
}

void lm_wm_switch_workspace(struct lm_wm *wm, int ws)
{
    if (ws < 0 || ws >= LM_NUM_WORKSPACES || ws == wm->workspace)
        return;
    if (wm->focused)
        lm_xdg_dismiss_popups(wm->focused->surface);
    wm->workspace = ws;
    struct lm_window *w;
    lm_list_for_each(w, &wm->windows, link)
        update_outputs(wm, w);
    set_focus(wm, NULL);
    set_focus(wm, top_visible(wm, NULL));
    damage_all(wm);
    refresh_pointer(wm);
}

void lm_wm_focus_window(struct lm_wm *wm, struct lm_window *w)
{
    if (!w) {
        set_focus(wm, NULL);
        return;
    }
    if (w->workspace != wm->workspace)
        lm_wm_switch_workspace(wm, w->workspace);
    if (w->minimized) {
        w->minimized = false;
        update_outputs(wm, w);
        damage_frame(wm, w);
    }
    raise_window(wm, w);
    set_focus(wm, w);
}

struct lm_window *lm_wm_focused_window(const struct lm_wm *wm) { return wm->focused; }

/* ---- lifecycle ------------------------------------------------------------------------------ */

static void update_clock(struct lm_wm *wm, bool force)
{
    char buf[24];
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, sizeof(buf), "%d.%m.%Y %H:%M", &tm);
    if (!force && !strcmp(buf, wm->clock_text))
        return;
    snprintf(wm->clock_text, sizeof(wm->clock_text), "%s", buf);
    if (!force)
        damage_panels(wm);
}

static void clock_tick(void *data)
{
    update_clock(data, false);
}

struct lm_wm *lm_wm_create(struct lm_server *srv)
{
    struct lm_wm *wm = calloc(1, sizeof(*wm));
    if (!wm)
        return NULL;
    wm->srv = srv;
    lm_list_init(&wm->windows);
    lm_list_init(&wm->popups);
    wm->hover_panel_ws = -1;
    wm->hover_menu = -1;
    wm->gad[LM_GADGET_CLIPBOARD].visible = srv->config.gadget;
    wm->gad[LM_GADGET_MIXER].visible = srv->config.mixer;
    wm->gad[LM_GADGET_SYSTEM].visible = srv->config.sysinfo;
    for (int i = 0; i < LM_GADGET_COUNT; i++) {
        wm->gad[i].w = gadget_def[i].w;
        wm->gad[i].h = gadget_def[i].h;
    }
    gadgets_load(wm);
    update_clock(wm, true);
    wm->note_timer = lm_loop_add_timer(srv->loop, note_tick, wm);
    wm->clock_timer = lm_loop_add_timer(srv->loop, clock_tick, wm);
    if (wm->clock_timer)
        lm_timer_arm_ms(wm->clock_timer, 1000, 1000);
    return wm;
}

void lm_wm_destroy(struct lm_wm *wm)
{
    if (!wm)
        return;
    if (wm->grab_kind != GRAB_NONE && wm->srv->seat)
        lm_seat_pointer_ungrab(wm->srv->seat, &wm->grab);
    if (wm->note_timer)
        lm_timer_destroy(wm->note_timer);
    if (wm->clock_timer)
        lm_timer_destroy(wm->clock_timer);
    free(wm);
}

void lm_wm_output_added(struct lm_wm *wm, struct lm_output *out)
{
    (void)out;
    damage_all(wm);
}

void lm_wm_relayout_outputs(struct lm_wm *wm)
{
    struct { struct lm_window *w; struct lm_output *o; int32_t dx, dy; } rec[64];
    int n = 0;
    struct lm_window *w;
    lm_list_for_each(w, &wm->windows, link) {
        struct lm_output *o = n < 64 ? window_output(wm, w) : NULL;
        if (o)
            rec[n++] = (typeof(rec[0])){ w, o, w->x - o->x, w->y - o->y };
    }
    if (wm->srv->outputs)
        lm_output_mgr_relayout(wm->srv->outputs);
    for (int i = 0; i < n; i++) {
        int32_t nx = rec[i].o->x + rec[i].dx, ny = rec[i].o->y + rec[i].dy;
        WIN(rec[i].w)->rx += nx - rec[i].w->x;                  /* the position to restore to moves along, too */
        WIN(rec[i].w)->ry += ny - rec[i].w->y;
        rec[i].w->x = nx;
        rec[i].w->y = ny;
    }
    lm_list_for_each(w, &wm->windows, link)
        if (w->states & (LM_WINDOW_MAXIMIZED | LM_WINDOW_FULLSCREEN))
            apply_state(wm, w);
    damage_all(wm);
    refresh_pointer(wm);
}

void lm_wm_output_changed(struct lm_wm *wm, struct lm_output *out)
{
    (void)out;
    struct lm_window *w;
    lm_list_for_each(w, &wm->windows, link)
        if (w->states & (LM_WINDOW_MAXIMIZED | LM_WINDOW_FULLSCREEN))
            apply_state(wm, w);
    damage_all(wm);
}

void lm_wm_output_removed(struct lm_wm *wm, struct lm_output *out)
{
    (void)out;
    /* The output is still in the list while we are called: re-clamp after it is gone. */
    struct lm_window *w;
    lm_list_for_each(w, &wm->windows, link)
        update_outputs(wm, w);
    damage_all(wm);
}

/* ---- windows --------------------------------------------------------------------------------- */

struct lm_window *lm_wm_window_create(struct lm_wm *wm, struct lm_surface *surface,
                                      const struct lm_window_ops *ops, void *shell_data)
{
    struct wm_win *ww = calloc(1, sizeof(*ww));
    if (!ww)
        return NULL;
    struct lm_window *w = &ww->pub;
    w->wm = wm;
    w->surface = surface;
    w->ops = ops;
    w->shell_data = shell_data;
    w->title = strdup("");
    w->app_id = strdup("");
    if (!w->title || !w->app_id) {
        free(w->title);
        free(w->app_id);
        free(ww);
        return NULL;
    }
    w->workspace = wm->workspace;
    ww->id = ++wm->next_id;
    lm_list_append(&wm->windows, &w->link);
    return w;
}

void lm_wm_window_destroy(struct lm_window *w)
{
    struct lm_wm *wm = w->wm;
    if (wm->grab_win == w && wm->grab_kind != GRAB_NONE) {
        wm->grab_kind = GRAB_NONE;
        wm->grab_win = NULL;
        if (wm->srv->seat)
            lm_seat_pointer_ungrab(wm->srv->seat, &wm->grab);
    }
    if (wm->hover_win == w) { wm->hover_win = NULL; wm->hover_bits = 0; }
    if (wm->hover_task == w) wm->hover_task = NULL;
    if (wm->press_win == w) { wm->press_win = NULL; wm->press_bits = 0; }
    if (wm->last_click_win == w) wm->last_click_win = NULL;
    sw_close(wm);                                               /* the list holds window pointers */
    struct lm_window *c;
    lm_list_for_each(c, &wm->windows, link)
        if (c->parent == w)
            c->parent = NULL;
    if (w->mapped)
        lm_wm_window_unmap(w);
    if (wm->focused == w)
        set_focus(wm, top_visible(wm, w));
    lm_list_remove(&w->link);
    free(w->title);
    free(w->app_id);
    free(w);
    refresh_pointer(wm);
}

void lm_wm_window_initial_configure(struct lm_window *w)
{
    struct lm_wm *wm = w->wm;
    int32_t cw = 0, ch = 0;
    if (w->states & LM_WINDOW_FULLSCREEN) {
        struct lm_output *o = window_output(wm, w);
        if (o) { cw = o->width; ch = o->height; }
    } else if (w->states & LM_WINDOW_MAXIMIZED) {
        struct lm_rect ua;
        lm_wm_usable_area(wm, 0, 0, &ua);
        cw = ua.w - 2 * LM_BORDER_WIDTH;
        ch = ua.h - LM_TITLEBAR_HEIGHT - LM_BORDER_WIDTH;
    }
    send_configure(wm, w, cw, ch);
}

static void replace_str(char **dst, const char *src)
{
    char *n = strdup(src ? src : "");
    if (n) {
        free(*dst);
        *dst = n;
    }
}

void lm_wm_window_set_title(struct lm_window *w, const char *title)
{
    replace_str(&w->title, title);
    if (w->mapped) {
        damage_frame(w->wm, w);
        if (w->wm->focused == w)
            damage_panels(w->wm);
    }
}

void lm_wm_window_set_app_id(struct lm_window *w, const char *app_id)
{
    replace_str(&w->app_id, app_id);
}

void lm_wm_window_set_parent(struct lm_window *w, struct lm_window *parent)
{
    for (struct lm_window *p = parent; p; p = p->parent)
        if (p == w)
            return; /* cycle */
    w->parent = parent;
}

void lm_wm_window_set_min_size(struct lm_window *w, int32_t width, int32_t height)
{
    w->min_w = LM_MAX(width, 0);
    w->min_h = LM_MAX(height, 0);
}

void lm_wm_window_set_max_size(struct lm_window *w, int32_t width, int32_t height)
{
    w->max_w = LM_MAX(width, 0);
    w->max_h = LM_MAX(height, 0);
}

void lm_wm_window_set_geometry(struct lm_window *w, const struct lm_rect *g)
{
    struct lm_wm *wm = w->wm;
    struct lm_rect before = frame_outer(w);
    struct lm_rect ng = *g;
    if (ng.w < 1 || ng.h < 1)
        return;
    w->geometry = ng;
    w->width = ng.w;
    w->height = ng.h;
    if (wm->grab_kind == GRAB_RESIZE && wm->grab_win == w) {
        if (wm->grab_edges & LM_EDGE_LEFT)
            w->x = wm->grab_x + wm->grab_w - w->width;
        if (wm->grab_edges & LM_EDGE_TOP)
            w->y = wm->grab_y + wm->grab_h - w->height;
    }
    if (w->mapped) {
        damage_rect(wm, &before);
        damage_frame(wm, w);
        update_outputs(wm, w);
    }
}

void lm_wm_window_map(struct lm_window *w)
{
    struct lm_wm *wm = w->wm;
    struct wm_win *ww = WIN(w);
    int32_t fit_w = 0, fit_h = 0;
    if (w->mapped)
        return;
    if (!ww->placed) {
        ww->placed = true;
        struct lm_rect ua;
        double px = 0, py = 0;
        if (wm->srv->seat)
            lm_seat_pointer_position(wm->srv->seat, &px, &py);
        lm_wm_usable_area(wm, (int32_t)px, (int32_t)py, &ua);
        if (w->states & LM_WINDOW_FULLSCREEN) {
            struct lm_output *o = lm_output_nearest(wm->srv, px, py);
            if (o) { w->x = o->x; w->y = o->y; }
        } else if (w->states & LM_WINDOW_MAXIMIZED) {
            w->x = ua.x + LM_BORDER_WIDTH;
            w->y = ua.y + LM_TITLEBAR_HEIGHT;
        } else if (w->parent && w->parent->mapped) {
            w->x = w->parent->x + (w->parent->width - w->width) / 2;
            w->y = w->parent->y + (w->parent->height - w->height) / 2;
        } else {
            int step = (wm->cascade++ % 10) * 28;
            w->x = ua.x + LM_BORDER_WIDTH + 40 + step;
            w->y = ua.y + LM_TITLEBAR_HEIGHT + 24 + step;
        }
        /* keep the whole frame on the output where possible */
        if (!(w->states & (LM_WINDOW_FULLSCREEN | LM_WINDOW_MAXIMIZED))) {
            /* a client that opened bigger than the usable area is asked to shrink to fit */
            int32_t fw = LM_MIN(w->width, ua.w - 2 * LM_BORDER_WIDTH);
            int32_t fh = LM_MIN(w->height, ua.h - LM_TITLEBAR_HEIGHT - LM_BORDER_WIDTH);
            if (fw != w->width || fh != w->height) {
                clamp_size(w, &fw, &fh);
                fit_w = fw; /* sent after set_focus(), whose configure would otherwise override it */
                fit_h = fh;
            }
            if (w->x + fw + LM_BORDER_WIDTH > ua.x + ua.w)
                w->x = ua.x + ua.w - fw - LM_BORDER_WIDTH;
            if (w->y + fh + LM_BORDER_WIDTH > ua.y + ua.h)
                w->y = ua.y + ua.h - fh - LM_BORDER_WIDTH;
            if (w->x < ua.x + LM_BORDER_WIDTH) w->x = ua.x + LM_BORDER_WIDTH;
            if (w->y < ua.y + LM_TITLEBAR_HEIGHT) w->y = ua.y + LM_TITLEBAR_HEIGHT;
        }
    }
    w->mapped = true;
    w->workspace = wm->workspace;
    w->minimized = false;
    raise_window(wm, w);
    update_outputs(wm, w);
    damage_frame(wm, w);
    set_focus(wm, w);
    if (fit_w > 0)
        send_configure(wm, w, fit_w, fit_h);
    damage_panels(wm);
    refresh_pointer(wm);
}

void lm_wm_window_unmap(struct lm_window *w)
{
    struct lm_wm *wm = w->wm;
    if (!w->mapped)
        return;
    sw_close(wm);
    struct lm_rect r = frame_outer(w);
    lm_xdg_dismiss_popups(w->surface);
    w->mapped = false;
    lm_surface_set_outputs(w->surface, NULL, 0);
    if (wm->focused == w)
        set_focus(wm, top_visible(wm, w));
    damage_rect(wm, &r);
    damage_panels(wm);
    refresh_pointer(wm);
}

void lm_wm_window_request_maximize(struct lm_window *w, bool maximize)
{
    struct lm_wm *wm = w->wm;
    struct wm_win *ww = WIN(w);
    bool is = (w->states & LM_WINDOW_MAXIMIZED) != 0;
    if (is == maximize)
        return;
    if (maximize) {
        save_restore(ww);
        w->states |= LM_WINDOW_MAXIMIZED;
    } else {
        w->states &= ~LM_WINDOW_MAXIMIZED;
    }
    if (!w->mapped) {
        return; /* applied on map / initial configure */
    }
    if (w->states & LM_WINDOW_FULLSCREEN)
        return;
    apply_state(wm, w);
}

void lm_wm_window_request_tile(struct lm_window *w, uint32_t edge)
{
    struct lm_wm *wm = w->wm;
    struct wm_win *ww = WIN(w);
    if (edge != LM_EDGE_LEFT && edge != LM_EDGE_RIGHT && edge != LM_EDGE_NONE)
        return;
    if (!w->mapped || (w->states & LM_WINDOW_FULLSCREEN) || ww->tiled == edge)
        return;
    if (edge == LM_EDGE_NONE) {
        if (!ww->tiled)
            return;
        apply_state(wm, w); /* clears ww->tiled and restores the saved geometry */
        return;
    }

    struct lm_rect before = frame_outer(w);
    save_restore(ww);
    w->states &= ~LM_WINDOW_MAXIMIZED;

    struct lm_rect ua;
    lm_wm_usable_area(wm, w->x + w->width / 2, w->y + w->height / 2, &ua);
    int32_t half = ua.w / 2;
    struct lm_rect outer = edge == LM_EDGE_LEFT ? lm_rect_make(ua.x, ua.y, half, ua.h)
                                                : lm_rect_make(ua.x + half, ua.y, ua.w - half, ua.h);
    int32_t tw = outer.w - 2 * LM_BORDER_WIDTH;
    int32_t th = outer.h - LM_TITLEBAR_HEIGHT - LM_BORDER_WIDTH;
    clamp_size(w, &tw, &th);
    w->x = outer.x + LM_BORDER_WIDTH;
    w->y = outer.y + LM_TITLEBAR_HEIGHT;
    ww->tiled = edge;
    send_configure(wm, w, tw, th);
    damage_rect(wm, &before);
    damage_frame(wm, w);
    update_outputs(wm, w);
}

void lm_wm_window_request_fullscreen(struct lm_window *w, bool fullscreen)
{
    struct lm_wm *wm = w->wm;
    struct wm_win *ww = WIN(w);
    bool is = (w->states & LM_WINDOW_FULLSCREEN) != 0;
    if (is == fullscreen)
        return;
    if (fullscreen) {
        save_restore(ww);
        w->states |= LM_WINDOW_FULLSCREEN;
    } else {
        w->states &= ~LM_WINDOW_FULLSCREEN;
    }
    if (!w->mapped)
        return;
    apply_state(wm, w);
    damage_panels(wm);
}

void lm_wm_window_request_minimize(struct lm_window *w)
{
    struct lm_wm *wm = w->wm;
    if (!w->mapped || w->minimized)
        return;
    struct lm_rect r = frame_outer(w);
    lm_xdg_dismiss_popups(w->surface);
    w->minimized = true;
    lm_surface_set_outputs(w->surface, NULL, 0);
    if (wm->focused == w)
        set_focus(wm, top_visible(wm, w));
    damage_rect(wm, &r);
    damage_panels(wm);
    refresh_pointer(wm);
}

void lm_wm_window_close(struct lm_window *w)
{
    if (w->ops && w->ops->close)
        w->ops->close(w);
}

/* ---- popups ------------------------------------------------------------------------------------ */

struct lm_popup *lm_wm_popup_create(struct lm_wm *wm, struct lm_surface *surface, struct lm_surface *parent)
{
    struct wm_popup *wp = calloc(1, sizeof(*wp));
    if (!wp)
        return NULL;
    wp->pub.wm = wm;
    wp->pub.surface = surface;
    wp->pub.parent = parent;
    lm_list_append(&wm->popups, &wp->pub.link);
    return &wp->pub;
}

static struct lm_rect popup_bounds(const struct lm_popup *p)
{
    int32_t ox, oy;
    struct lm_rect ext;
    popup_origin(p, &ox, &oy);
    if (lm_surface_tree_extents(p->surface, &ext))
        return lm_rect_make(ox + ext.x, oy + ext.y, ext.w, ext.h);
    return lm_rect_make(p->x, p->y, p->geometry.w, p->geometry.h);
}

void lm_wm_popup_destroy(struct lm_popup *p)
{
    struct lm_wm *wm = p->wm;
    if (p->mapped) {
        struct lm_rect r = popup_bounds(p);
        damage_rect(wm, &r);
    }
    lm_list_remove(&p->link);
    free(p);
    refresh_pointer(wm);
}

void lm_wm_popup_set_position(struct lm_popup *p, int32_t gx, int32_t gy)
{
    struct lm_rect before = popup_bounds(p);
    p->x = gx;
    p->y = gy;
    if (p->mapped) {
        damage_rect(p->wm, &before);
        struct lm_rect after = popup_bounds(p);
        damage_rect(p->wm, &after);
    }
}

void lm_wm_popup_set_geometry(struct lm_popup *p, const struct lm_rect *g)
{
    struct lm_rect before = popup_bounds(p);
    p->geometry = *g;
    if (p->mapped) {
        damage_rect(p->wm, &before);
        struct lm_rect after = popup_bounds(p);
        damage_rect(p->wm, &after);
    }
}

void lm_wm_popup_map(struct lm_popup *p)
{
    p->mapped = true;
    struct lm_rect r = popup_bounds(p);
    damage_rect(p->wm, &r);
    refresh_pointer(p->wm);
}

void lm_wm_popup_unmap(struct lm_popup *p)
{
    if (!p->mapped)
        return;
    struct lm_rect r = popup_bounds(p);
    p->mapped = false;
    damage_rect(p->wm, &r);
    refresh_pointer(p->wm);
}

static bool popup_visible(const struct lm_wm *wm, const struct lm_popup *p, int depth)
{
    if (!p->mapped)
        return false;
    if (depth > 8)
        return false;
    struct lm_surface *root = p->parent ? lm_surface_root(p->parent) : NULL;
    if (!root)
        return true;
    struct lm_window *w;
    lm_list_for_each(w, &wm->windows, link)
        if (w->surface == root)
            return win_visible(wm, w);
    struct lm_popup *q;
    lm_list_for_each(q, &wm->popups, link)
        if (q != p && q->surface == root)
            return popup_visible(wm, q, depth + 1);
    return true;
}

/* ---- queries ------------------------------------------------------------------------------------ */

bool lm_wm_surface_origin(struct lm_wm *wm, struct lm_surface *s, int32_t *x, int32_t *y)
{
    struct lm_surface *root = lm_surface_root(s);
    int32_t dx = 0, dy = 0;
    if (root != s)
        lm_surface_offset_in_root(s, &dx, &dy);
    struct lm_window *w;
    lm_list_for_each(w, &wm->windows, link)
        if (w->surface == root && w->mapped) {
            win_origin(w, x, y);
            *x += dx;
            *y += dy;
            return true;
        }
    struct lm_popup *p;
    lm_list_for_each(p, &wm->popups, link)
        if (p->surface == root && p->mapped) {
            popup_origin(p, x, y);
            *x += dx;
            *y += dy;
            return true;
        }
    return false;
}

void lm_wm_surface_damage(struct lm_wm *wm, struct lm_surface *root, const struct lm_rect *rect)
{
    struct lm_window *w;
    lm_list_for_each(w, &wm->windows, link)
        if (w->surface == root) {
            if (!win_visible(wm, w))
                return;
            int32_t ox, oy;
            win_origin(w, &ox, &oy);
            struct lm_rect r = lm_rect_make(ox + rect->x, oy + rect->y, rect->w, rect->h);
            damage_rect(wm, &r);
            return;
        }
    struct lm_popup *p;
    lm_list_for_each(p, &wm->popups, link)
        if (p->surface == root) {
            if (!p->mapped)
                return;
            int32_t ox, oy;
            popup_origin(p, &ox, &oy);
            struct lm_rect r = lm_rect_make(ox + rect->x, oy + rect->y, rect->w, rect->h);
            damage_rect(wm, &r);
            return;
        }
}

void lm_wm_send_frame_done(struct lm_wm *wm, uint32_t time_ms)
{
    struct lm_window *w;
    lm_list_for_each(w, &wm->windows, link) {
        if (!win_visible(wm, w))
            continue;
        update_outputs(wm, w);
        lm_surface_send_frame_done(w->surface, time_ms);
    }
    struct lm_popup *p;
    lm_list_for_each(p, &wm->popups, link)
        if (popup_visible(wm, p, 0))
            lm_surface_send_frame_done(p->surface, time_ms);
}

/* ---- scene ---------------------------------------------------------------------------------------- */

/* ---- the session dialog: a round power button in the panel opens a card with round Lock and Exit buttons ---------------- */

/* The output the dialog is on (looked up by name, so an unplugged one just ends the dialog). */
static struct lm_output *power_output(const struct lm_wm *wm)
{
    if (!wm->power_open || !wm->srv->backend)
        return NULL;
    struct lm_output *o;
    lm_list_for_each(o, &wm->srv->backend->outputs, link)
        if (!strcmp(o->name, wm->power_out))
            return o;
    return NULL;
}

static void power_info(const struct lm_wm *wm, struct lm_output *o, struct lm_power_info *pi)
{
    memset(pi, 0, sizeof(*pi));
    pi->rect = lm_output_rect(o);
    pi->card = lm_rect_make(pi->rect.x + (pi->rect.w - POWER_CARD_W) / 2, pi->rect.y + (pi->rect.h - POWER_CARD_H) / 2,
                            POWER_CARD_W, POWER_CARD_H);
    int gap = 40, total = 2 * POWER_BTN_D + gap;
    int x = pi->card.x + (POWER_CARD_W - total) / 2, y = pi->card.y + 46;
    for (int i = 0; i < 2; i++) {
        pi->button[i] = lm_rect_make(x + i * (POWER_BTN_D + gap), y, POWER_BTN_D, POWER_BTN_D);
        pi->hit[i] = lm_rect_make(pi->button[i].x - 8, y, POWER_BTN_D + 16, POWER_BTN_D + 8 + LM_FONT_H + 4);
    }
    pi->sel = wm->power_sel;
}

static void power_damage(struct lm_wm *wm)
{
    struct lm_output *o = power_output(wm);
    if (o) {
        struct lm_rect r = lm_output_rect(o);
        damage_rect(wm, &r);
    }
    damage_panels(wm);
}

/* Opens the dialog on `o` (the output whose panel button was used; NULL = the primary one) or closes it. */
static void power_set(struct lm_wm *wm, bool open, struct lm_output *o)
{
    if (open == wm->power_open)
        return;
    if (open) {
        if (!o)
            o = lm_output_primary(wm->srv);
        if (!o)
            return;
        menu_set(wm, false, NULL);
        sw_close(wm);
        clip_nav_set(wm, false);
        snprintf(wm->power_out, sizeof(wm->power_out), "%s", o->name);
        wm->power_open = true;
        wm->power_sel = -1;
        power_damage(wm);
    } else {
        power_damage(wm);                                       /* (while the output is still found) */
        wm->power_open = false;
        wm->hover_power = false;
        damage_panels(wm);
    }
}

void lm_wm_scene_for_each(struct lm_wm *wm, const struct lm_rect *clip, lm_scene_fn fn, void *data)
{
    struct lm_scene_item it;
    struct lm_output *o;

    if (lm_lock_active(wm->srv->lock)) {                         /* locked: nothing of the session may show */
        if (wm->srv->backend) {
            lm_list_for_each(o, &wm->srv->backend->outputs, link) {
                struct lm_rect orect = lm_output_rect(o);
                if (clip && !lm_rect_overlaps(&orect, clip))
                    continue;
                memset(&it, 0, sizeof(it));
                it.kind = LM_SCENE_LOCK;
                it.output = o;
                lm_lock_info(wm->srv->lock, o, &it.u.lock);
                if (!fn(&it, data))
                    return;
            }
        }
        return;
    }

    if (wm->srv->backend) {
        lm_list_for_each(o, &wm->srv->backend->outputs, link) {
            struct lm_rect orect = lm_output_rect(o);
            if (clip && !lm_rect_overlaps(&orect, clip))
                continue;
            memset(&it, 0, sizeof(it));
            it.kind = LM_SCENE_WALLPAPER;
            it.output = o;
            if (!fn(&it, data))
                return;
        }
    }

    for (int gid = 0; gid < LM_GADGET_COUNT; gid++) {            /* bottom to top: the mixer is above the clipboard */
        struct lm_rect gr;
        if (!lm_wm_gadget_rect_id(wm, gid, &gr))
            continue;
        struct lm_rect gb = gadget_bounds(&gr);
        if (clip && !lm_rect_overlaps(&gb, clip))
            continue;
        memset(&it, 0, sizeof(it));
        it.kind = LM_SCENE_GADGET;
        it.u.gadget.id = gid;
        it.u.gadget.rect = gr;
        it.u.gadget.title_h = GADGET_TITLE_H;
        it.u.gadget.dragging = (wm->grab_kind == GRAB_GADGET_MOVE || wm->grab_kind == GRAB_GADGET_RESIZE) && wm->grab_gadget == gid;
        if (gid == LM_GADGET_MIXER) {
            if (wm->srv->mixer)
                it.u.gadget.mixer = lm_mixer_state(wm->srv->mixer);
        } else if (gid == LM_GADGET_SYSTEM) {
            if (wm->srv->sysinfo)
                it.u.gadget.sys = lm_sysinfo_state(wm->srv->sysinfo);
        } else {
            gadget_slots(gr.h, &it.u.gadget.slots, &it.u.gadget.entry_h);
            int total = lm_seat_clip_count(wm->srv->seat);
            if (total > LM_GADGET_ENTRIES)
                total = LM_GADGET_ENTRIES;
            it.u.gadget.total = total;
            int n = LM_MIN(total, it.u.gadget.slots);
            it.u.gadget.n = n;
            it.u.gadget.cursor = wm->clip_nav && n > 0 ? LM_CLAMP(wm->clip_cur, 0, n - 1) : -1;
            for (int i = 0; i < n; i++)
                it.u.gadget.text[i] = lm_seat_clip_get(wm->srv->seat, i);
        }
        if (!fn(&it, data))
            return;
    }

    struct lm_window *w;
    lm_list_for_each(w, &wm->windows, link) {
        if (!win_visible(wm, w))
            continue;
        struct lm_rect outer = frame_outer(w);
        int32_t ox, oy;
        win_origin(w, &ox, &oy);
        struct lm_rect sb = outer, ext;
        if (lm_surface_tree_extents(w->surface, &ext)) {
            struct lm_rect eb = lm_rect_make(ox + ext.x, oy + ext.y, ext.w, ext.h);
            sb = lm_rect_union(&outer, &eb);
        }
        if (clip && !lm_rect_overlaps(&sb, clip))
            continue;
        if (!(w->states & LM_WINDOW_FULLSCREEN) && (!clip || lm_rect_overlaps(&outer, clip))) {
            memset(&it, 0, sizeof(it));
            it.kind = LM_SCENE_FRAME;
            frame_info(wm, w, &it.u.frame);
            if (!fn(&it, data))
                return;
        }
        memset(&it, 0, sizeof(it));
        it.kind = LM_SCENE_SURFACE;
        it.u.surface.root = w->surface;
        it.u.surface.x = ox;
        it.u.surface.y = oy;
        if (!fn(&it, data))
            return;
    }

    if (wm->srv->backend) {
        lm_list_for_each(o, &wm->srv->backend->outputs, link) {
            struct lm_rect pr = output_panel_rect(o);
            if ((clip && !lm_rect_overlaps(&pr, clip)) || fullscreen_covers(wm, o))
                continue;
            memset(&it, 0, sizeof(it));
            it.kind = LM_SCENE_PANEL;
            it.output = o;
            panel_info(wm, o, &it.u.panel);
            if (!fn(&it, data))
                return;
        }
    }

    if (wm->menu_open && (!clip || lm_rect_overlaps(&wm->menu_rect, clip))) {
        memset(&it, 0, sizeof(it));
        it.kind = LM_SCENE_MENU;
        it.u.menu.rect = wm->menu_rect;
        it.u.menu.n = wm->srv->config.napps;
        it.u.menu.item_h = MENU_ITEM_H;
        it.u.menu.hover = wm->hover_menu;
        for (int i = 0; i < it.u.menu.n; i++)
            it.u.menu.label[i] = wm->srv->config.apps[i].name;
        if (!fn(&it, data))
            return;
    }

    struct lm_popup *p;
    lm_list_for_each(p, &wm->popups, link) {
        if (!popup_visible(wm, p, 0))
            continue;
        struct lm_rect pb = popup_bounds(p);
        if (clip && !lm_rect_overlaps(&pb, clip))
            continue;
        memset(&it, 0, sizeof(it));
        it.kind = LM_SCENE_SURFACE;
        it.u.surface.root = p->surface;
        popup_origin(p, &it.u.surface.x, &it.u.surface.y);
        it.u.surface.is_popup = true;
        if (!fn(&it, data))
            return;
    }
    if (wm->nnotes > 0) {                                        /* notification cards, above the windows and the panel */
        struct lm_rect rs[LM_NOTIFY_MAX];
        if (note_rects(wm, rs)) {
            for (int i = wm->nnotes - 1; i >= 0; i--) {              /* oldest first: the newest is on top */
                if (clip && !lm_rect_overlaps(&rs[i], clip))
                    continue;
                memset(&it, 0, sizeof(it));
                it.kind = LM_SCENE_NOTIFY;
                it.u.notify.rect = rs[i];
                it.u.notify.id = wm->notes[i].id;
                snprintf(it.u.notify.title, sizeof(it.u.notify.title), "%s", wm->notes[i].title);
                it.u.notify.nlines = note_wrap(wm->notes[i].body, it.u.notify.line);
                if (!fn(&it, data))
                    return;
            }
        }
    }

    if (wm->power_open) {                                        /* the session dialog */
        struct lm_output *po = power_output(wm);
        if (po) {
            struct lm_rect r = lm_output_rect(po);
            if (!clip || lm_rect_overlaps(&r, clip)) {
                memset(&it, 0, sizeof(it));
                it.kind = LM_SCENE_POWER;
                it.output = po;
                power_info(wm, po, &it.u.power);
                if (!fn(&it, data))
                    return;
            }
        }
    }

    if (wm->sw_open) {                                           /* the Alt+Tab list, above everything */
        struct lm_rect r = sw_rect(wm);
        if (r.w > 0 && (!clip || lm_rect_overlaps(&r, clip))) {
            memset(&it, 0, sizeof(it));
            it.kind = LM_SCENE_SWITCHER;
            it.u.switcher.rect = r;
            it.u.switcher.n = LM_MIN(wm->sw_n, LM_SW_MAX);
            it.u.switcher.total = wm->sw_n;
            it.u.switcher.item_h = SW_ITEM_H;
            it.u.switcher.current_ws = wm->workspace;
            int first = sw_first(wm);
            it.u.switcher.sel = wm->sw_idx - first;
            for (int i = 0; i < it.u.switcher.n; i++) {
                const struct lm_window *w = wm->sw_win[first + i];
                it.u.switcher.label[i] = sw_label(w);
                it.u.switcher.ws[i] = w->workspace;
                it.u.switcher.minimized[i] = w->minimized;
                it.u.switcher.focused[i] = w == wm->focused;
            }
            if (!fn(&it, data))
                return;
        }
    }
}

static void append(char *buf, size_t size, size_t *n, const char *fmt, ...) LM_PRINTF(4, 5);
static void append(char *buf, size_t size, size_t *n, const char *fmt, ...)
{
    if (*n + 1 >= size)
        return;
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(buf + *n, size - *n, fmt, ap);
    va_end(ap);
    if (k > 0)
        *n += (size_t)k < size - *n ? (size_t)k : size - *n - 1;
}

size_t lm_wm_describe(struct lm_wm *wm, char *buf, size_t size)
{
    if (size == 0)
        return 0;
    size_t n = 0;
    double px = 0, py = 0;
    if (wm->srv->seat)
        lm_seat_pointer_position(wm->srv->seat, &px, &py);
#define APPEND(...) append(buf, size, &n, __VA_ARGS__)
    APPEND("workspace %d\n", wm->workspace);
    APPEND("pointer %d %d\n", (int)px, (int)py);
    struct lm_window *w;
    lm_list_for_each(w, &wm->windows, link) {
        char title[96], app[64];
        snprintf(title, sizeof(title), "%s", w->title);
        snprintf(app, sizeof(app), "%s", w->app_id);
        for (char *c = title; *c; c++) if (*c == '"' || *c == '\n') *c = '\'';
        for (char *c = app; *c; c++) if (*c == '"' || *c == '\n') *c = '\'';
        char flags[96] = "";
        if (w->mapped) strcat(flags, "mapped,");
        if (wm->focused == w) strcat(flags, "focused,");
        if (w->states & LM_WINDOW_MAXIMIZED) strcat(flags, "maximized,");
        if (w->states & LM_WINDOW_FULLSCREEN) strcat(flags, "fullscreen,");
        if (WIN(w)->tiled) strcat(flags, WIN(w)->tiled == LM_EDGE_LEFT ? "tiled-left," : "tiled-right,");
        if (w->minimized) strcat(flags, "minimized,");
        size_t fl = strlen(flags);
        if (fl) flags[fl - 1] = 0; else strcpy(flags, "-");
        APPEND("window %d \"%s\" \"%s\" x=%d y=%d w=%d h=%d ws=%d flags=%s\n", WIN(w)->id,
               title, app, w->x, w->y, w->width, w->height, w->workspace, flags);
    }
#undef APPEND
    if (n >= size)
        n = size - 1;
    buf[n] = 0;
    return n;
}

/* ---- picking ----------------------------------------------------------------------------------------- */

static uint32_t edges_at(const struct lm_window *w, const struct lm_rect *o, double x, double y)
{
    if (w->states & LM_WINDOW_MAXIMIZED)
        return LM_EDGE_NONE;
    uint32_t e = 0;
    double lx = x - o->x, ly = y - o->y;
    if (lx < LM_BORDER_WIDTH) e |= LM_EDGE_LEFT;
    if (lx >= o->w - LM_BORDER_WIDTH) e |= LM_EDGE_RIGHT;
    if (ly >= o->h - LM_BORDER_WIDTH) e |= LM_EDGE_BOTTOM;
    if (ly < LM_BORDER_WIDTH) e |= LM_EDGE_TOP;
    if (e) {
        /* widen into corners */
        if ((e & (LM_EDGE_LEFT | LM_EDGE_RIGHT)) && !(e & (LM_EDGE_TOP | LM_EDGE_BOTTOM))) {
            if (ly < RESIZE_CORNER) e |= LM_EDGE_TOP;
            else if (ly >= o->h - RESIZE_CORNER) e |= LM_EDGE_BOTTOM;
        } else if ((e & (LM_EDGE_TOP | LM_EDGE_BOTTOM)) && !(e & (LM_EDGE_LEFT | LM_EDGE_RIGHT))) {
            if (lx < RESIZE_CORNER) e |= LM_EDGE_LEFT;
            else if (lx >= o->w - RESIZE_CORNER) e |= LM_EDGE_RIGHT;
        }
    }
    return e;
}

void lm_wm_pick(struct lm_wm *wm, double x, double y, struct lm_pick *out)
{
    memset(out, 0, sizeof(*out));
    out->kind = LM_PICK_NONE;
    int32_t ix = (int32_t)x, iy = (int32_t)y;

    struct lm_output *po = power_output(wm);
    if (po && !lm_lock_active(wm->srv->lock)) {                  /* the session dialog is modal: it gets every pick */
        struct lm_power_info pi;
        power_info(wm, po, &pi);
        out->kind = lm_rect_contains(&pi.card, ix, iy) ? LM_PICK_POWER_CARD : LM_PICK_POWER_BACKDROP;
        for (int i = 0; i < 2; i++)
            if (lm_rect_contains(&pi.hit[i], ix, iy)) {
                out->kind = LM_PICK_POWER_BUTTON;
                out->index = i;
            }
        out->rect = pi.card;
        return;
    }

    if (wm->nnotes > 0 && !lm_lock_active(wm->srv->lock)) {
        struct lm_rect rs[LM_NOTIFY_MAX];
        if (note_rects(wm, rs))
            for (int i = 0; i < wm->nnotes; i++)
                if (lm_rect_contains(&rs[i], ix, iy)) {
                    out->kind = LM_PICK_NOTIFY;
                    out->index = wm->notes[i].id;
                    return;
                }
    }
    if (wm->srv->render && lm_render_shot_card_at(wm->srv->render, x, y)) {
        out->kind = LM_PICK_SHOT_CARD;
        return;
    }
    if (wm->menu_open && lm_rect_contains(&wm->menu_rect, ix, iy)) {
        int i = (iy - wm->menu_rect.y - 2) / MENU_ITEM_H;
        out->kind = LM_PICK_MENU_ITEM;
        out->index = LM_CLAMP(i, 0, wm->srv->config.napps - 1);
        return;
    }

    struct lm_popup *p;
    lm_list_for_each_reverse(p, &wm->popups, link) {
        if (!popup_visible(wm, p, 0))
            continue;
        int32_t ox, oy;
        double sx, sy;
        popup_origin(p, &ox, &oy);
        struct lm_surface *hit = lm_surface_tree_pick(p->surface, ox, oy, x, y, &sx, &sy);
        if (hit) {
            out->kind = LM_PICK_SURFACE;
            out->surface = hit;
            out->sx = sx;
            out->sy = sy;
            return;
        }
    }

    struct lm_window *w;
    lm_list_for_each_reverse(w, &wm->windows, link) {
        if (!win_visible(wm, w))
            continue;
        int32_t ox, oy;
        double sx, sy;
        win_origin(w, &ox, &oy);
        struct lm_surface *hit = lm_surface_tree_pick(w->surface, ox, oy, x, y, &sx, &sy);
        if (hit) {
            out->kind = LM_PICK_SURFACE;
            out->window = w;
            out->surface = hit;
            out->sx = sx;
            out->sy = sy;
            return;
        }
        if (w->states & LM_WINDOW_FULLSCREEN)
            continue;
        struct lm_frame_info f;
        frame_info(wm, w, &f);
        if (!lm_rect_contains(&f.outer, ix, iy))
            continue;
        out->window = w;
        if (lm_rect_contains(&f.btn_close, ix, iy)) { out->kind = LM_PICK_CLOSE; return; }
        if (lm_rect_contains(&f.btn_max, ix, iy)) { out->kind = LM_PICK_MAXIMIZE; return; }
        if (lm_rect_contains(&f.btn_min, ix, iy)) { out->kind = LM_PICK_MINIMIZE; return; }
        uint32_t e = edges_at(w, &f.outer, x, y);
        if (e) {
            out->kind = LM_PICK_BORDER;
            out->edges = e;
            return;
        }
        if (lm_rect_contains(&f.titlebar, ix, iy)) {
            out->kind = LM_PICK_TITLEBAR;
            return;
        }
        /* inside the content area but the surface does not take input here: fall through */
        out->window = NULL;
    }

    for (int gid = LM_GADGET_COUNT - 1; gid >= 0; gid--) {       /* topmost gadget first */
        struct lm_rect gr;
        if (!lm_wm_gadget_rect_id(wm, gid, &gr) || !lm_rect_contains(&gr, ix, iy))
            continue;
        uint32_t e = 0;
        if (iy < gr.y + 3) e |= LM_EDGE_TOP;
        if (iy >= gr.y + gr.h - GADGET_EDGE) e |= LM_EDGE_BOTTOM;
        if (ix < gr.x + GADGET_EDGE) e |= LM_EDGE_LEFT;
        else if (ix >= gr.x + gr.w - GADGET_EDGE) e |= LM_EDGE_RIGHT;
        if (e) {
            out->kind = LM_PICK_GADGET_BORDER;
            out->edges = e;
        } else if (iy < gr.y + GADGET_TITLE_H) {
            out->kind = LM_PICK_GADGET_TITLE;
        } else if (gid == LM_GADGET_MIXER) {
            out->kind = LM_PICK_GADGET_BODY;                      /* sliders and buttons */
        } else if (gid == LM_GADGET_CLIPBOARD) {                  /* an entry slot with text in it */
            int slots, entry_h;
            gadget_slots(gr.h, &slots, &entry_h);
            int n = LM_MIN(lm_seat_clip_count(wm->srv->seat), slots);
            int k = (iy - gr.y - GADGET_TITLE_H) / entry_h;
            if (k >= 0 && k < n && ix >= gr.x + 12 && ix < gr.x + gr.w - 12) {
                out->kind = LM_PICK_CLIP_ENTRY;
                out->index = k;
                out->rect = gr;
                return;
            }
        }
        out->index = gid;
        out->rect = gr;
        if (out->kind != LM_PICK_NONE)
            return;
        out->index = 0;
        out->rect = (struct lm_rect){ 0 };
        break;                                                    /* (the clipboard body lets the click through) */
    }

    if (wm->srv->backend) {
        struct lm_output *o;
        lm_list_for_each(o, &wm->srv->backend->outputs, link) {
            struct lm_rect pr = output_panel_rect(o);
            if (!lm_rect_contains(&pr, ix, iy) || fullscreen_covers(wm, o))
                continue;
            struct lm_panel_info pi;
            panel_info(wm, o, &pi);
            out->kind = LM_PICK_PANEL;
            if (lm_rect_contains(&pi.launcher, ix, iy)) {
                out->kind = LM_PICK_PANEL_LAUNCHER;
                out->rect = pi.launcher;
            }
            if (lm_rect_contains(&pi.run, ix, iy))
                out->kind = LM_PICK_PANEL_RUN;
            int pcx = pi.power.x + pi.power.w / 2, pcy = pi.power.y + pi.power.h / 2, pr2 = pi.power.w / 2 + 1;
            if ((ix - pcx) * (ix - pcx) + (iy - pcy) * (iy - pcy) <= pr2 * pr2)   /* the circle, not its square */
                out->kind = LM_PICK_PANEL_POWER;
            if (lm_rect_contains(&pi.layout, ix, iy))
                out->kind = LM_PICK_PANEL_LAYOUT;
            if (lm_rect_contains(&pi.sticky, ix, iy))
                out->kind = LM_PICK_PANEL_STICKY;
            for (int i = 0; i < LM_NUM_WORKSPACES; i++)
                if (lm_rect_contains(&pi.workspace[i], ix, iy)) {
                    out->kind = LM_PICK_PANEL_WORKSPACE;
                    out->index = i;
                }
            for (int i = 0; i < pi.ntasks; i++)
                if (lm_rect_contains(&pi.task[i].rect, ix, iy)) {
                    out->kind = LM_PICK_PANEL_TASK;
                    out->window = pi.task_win[i];
                }
            return;
        }
    }
}

/* ---- pointer motion / hover ---------------------------------------------------------------------------- */

static enum lm_cursor_shape cursor_for_edges(uint32_t e)
{
    switch (e) {
    case LM_EDGE_TOP: return LM_CURSOR_RESIZE_N;
    case LM_EDGE_BOTTOM: return LM_CURSOR_RESIZE_S;
    case LM_EDGE_LEFT: return LM_CURSOR_RESIZE_W;
    case LM_EDGE_RIGHT: return LM_CURSOR_RESIZE_E;
    case LM_EDGE_TOP | LM_EDGE_LEFT: return LM_CURSOR_RESIZE_NW;
    case LM_EDGE_TOP | LM_EDGE_RIGHT: return LM_CURSOR_RESIZE_NE;
    case LM_EDGE_BOTTOM | LM_EDGE_LEFT: return LM_CURSOR_RESIZE_SW;
    case LM_EDGE_BOTTOM | LM_EDGE_RIGHT: return LM_CURSOR_RESIZE_SE;
    }
    return LM_CURSOR_NONE;
}

void lm_wm_pointer_motion(struct lm_wm *wm, const struct lm_pick *pick)
{
    struct lm_window *hw = NULL;
    uint32_t bits = 0;
    int pws = -1;
    bool launcher = false, power_btn = false, run_btn = false, layout_btn = false, sticky_btn = false;
    int menu_item = -1, power_item = -1;
    struct lm_window *task = NULL;
    enum lm_cursor_shape shape = LM_CURSOR_NONE;

    switch (pick->kind) {
    case LM_PICK_CLOSE: hw = pick->window; bits = LM_FRAME_HOVER_CLOSE; break;
    case LM_PICK_MAXIMIZE: hw = pick->window; bits = LM_FRAME_HOVER_MAXIMIZE; break;
    case LM_PICK_MINIMIZE: hw = pick->window; bits = LM_FRAME_HOVER_MINIMIZE; break;
    case LM_PICK_BORDER:
    case LM_PICK_GADGET_BORDER: shape = cursor_for_edges(pick->edges); break;
    case LM_PICK_SHOT_CARD: shape = LM_CURSOR_POINTER; break;
    case LM_PICK_NOTIFY: shape = LM_CURSOR_POINTER; break;
    case LM_PICK_PANEL_LAUNCHER: launcher = true; shape = LM_CURSOR_POINTER; break;
    case LM_PICK_MENU_ITEM: menu_item = pick->index; shape = LM_CURSOR_POINTER; break;
    case LM_PICK_PANEL_RUN: run_btn = true; shape = LM_CURSOR_POINTER; break;
    case LM_PICK_PANEL_POWER: power_btn = true; shape = LM_CURSOR_POINTER; break;
    case LM_PICK_POWER_BUTTON: power_item = pick->index; shape = LM_CURSOR_POINTER; break;
    case LM_PICK_PANEL_LAYOUT: layout_btn = true; shape = LM_CURSOR_POINTER; break;
    case LM_PICK_PANEL_STICKY: sticky_btn = true; shape = LM_CURSOR_POINTER; break;
    case LM_PICK_PANEL_WORKSPACE: pws = pick->index; shape = LM_CURSOR_POINTER; break;
    case LM_PICK_PANEL_TASK: task = pick->window; shape = LM_CURSOR_POINTER; break;
    default: break;
    }
    if (bits)
        shape = LM_CURSOR_POINTER;

    if (hw != wm->hover_win || bits != wm->hover_bits) {
        if (wm->hover_win)
            damage_frame(wm, wm->hover_win);
        wm->hover_win = hw;
        wm->hover_bits = bits;
        if (hw)
            damage_frame(wm, hw);
    }
    if (menu_item != wm->hover_menu) {
        wm->hover_menu = menu_item;
        if (wm->menu_open)
            damage_rect(wm, &wm->menu_rect);
    }
    if (wm->power_open && power_item >= 0 && power_item != wm->power_sel) {     /* the pointer moves the dialog's highlight */
        wm->power_sel = power_item;
        power_damage(wm);
    }
    if (pws != wm->hover_panel_ws || launcher != wm->hover_launcher || power_btn != wm->hover_power ||
        run_btn != wm->hover_run || task != wm->hover_task || layout_btn != wm->hover_layout ||
        sticky_btn != wm->hover_sticky) {
        wm->hover_layout = layout_btn;
        wm->hover_sticky = sticky_btn;
        wm->hover_run = run_btn;
        wm->hover_power = power_btn;
        wm->hover_task = task;
        wm->hover_panel_ws = pws;
        wm->hover_launcher = launcher;
        damage_panels(wm);
    }
    if (wm->srv->seat && wm->grab_kind == GRAB_NONE)
        lm_seat_set_cursor_override(wm->srv->seat, shape);
}

/* ---- interactive move / resize ------------------------------------------------------------------------------ */

static void grab_end(struct lm_wm *wm)
{
    struct lm_window *w = wm->grab_win;
    enum grab_kind kind = wm->grab_kind;
    wm->grab_kind = GRAB_NONE;
    wm->grab_win = NULL;
    if (kind == GRAB_GADGET_MOVE || kind == GRAB_GADGET_RESIZE) {
        damage_gadget(wm, wm->grab_gadget);
        int gi = wm->grab_gadget;
        if (wm->gad[gi].x != wm->grab_x || wm->gad[gi].y != wm->grab_y || wm->gad[gi].w != wm->grab_w || wm->gad[gi].h != wm->grab_h)
            gadgets_save(wm);                                       /* it moved or changed size (a plain click does not count) */
    }
    if (wm->srv->seat) {
        lm_seat_pointer_ungrab(wm->srv->seat, &wm->grab);
        lm_seat_set_cursor_override(wm->srv->seat, LM_CURSOR_NONE);
    }
    if (w && kind == GRAB_RESIZE) {
        w->states &= ~LM_WINDOW_RESIZING;
        send_configure(wm, w, w->width, w->height);
    }
}

/*
 * Gadgets are sticky: a card that is dragged within GADGET_SNAP of another card's edge (or of the screen's) jumps flush
 * against it. Beside another card it needs to overlap it vertically (within the same reach), above / below it horizontally;
 * a card that touches another one with a side also lines up with it (same left / right / top / bottom edge).
 */
static void gadget_snap(struct lm_wm *wm, int id, const struct lm_output *o, int32_t *px, int32_t *py, int32_t w, int32_t h)
{
    int32_t x = *px, y = *py, nx = x, ny = y;
    int bestx = GADGET_SNAP + 1, besty = GADGET_SNAP + 1;
#define TRY_X(c) do { int32_t cand_ = (c); int d_ = abs(cand_ - x); if (d_ < bestx) { bestx = d_; nx = cand_; } } while (0)
#define TRY_Y(c) do { int32_t cand_ = (c); int d_ = abs(cand_ - y); if (d_ < besty) { besty = d_; ny = cand_; } } while (0)
    TRY_X(o->x);
    TRY_X(o->x + o->width - w);
    TRY_Y(o->y + LM_PANEL_HEIGHT);
    TRY_Y(o->y + o->height - h);
    for (int j = 0; j < LM_GADGET_COUNT; j++) {
        struct lm_rect pr;
        if (j == id || !lm_wm_gadget_rect_id(wm, j, &pr))
            continue;
        bool vover = y < pr.y + pr.h + GADGET_SNAP && y + h > pr.y - GADGET_SNAP;
        bool hover = x < pr.x + pr.w + GADGET_SNAP && x + w > pr.x - GADGET_SNAP;
        bool side = abs(x - (pr.x + pr.w)) <= GADGET_SNAP || abs(x + w - pr.x) <= GADGET_SNAP;       /* beside it */
        bool stack = abs(y - (pr.y + pr.h)) <= GADGET_SNAP || abs(y + h - pr.y) <= GADGET_SNAP;      /* above / below it */
        if (vover) {
            TRY_X(pr.x + pr.w);
            TRY_X(pr.x - w);
        }
        if (hover) {
            TRY_Y(pr.y + pr.h);
            TRY_Y(pr.y - h);
        }
        if (stack && hover) {                                       /* stacked: the same left or right edge */
            TRY_X(pr.x);
            TRY_X(pr.x + pr.w - w);
        }
        if (side && vover) {                                        /* side by side: the same top or bottom edge */
            TRY_Y(pr.y);
            TRY_Y(pr.y + pr.h - h);
        }
    }
#undef TRY_X
#undef TRY_Y
    *px = nx;
    *py = ny;
}

/*
 * A line (an edge of the card being resized) sticks to the edges that line up with it: the screen's and those of the other cards
 * that it faces. `vertical` = the line is a left/right edge at x = v (the card spans [lo, hi] in y), else a top/bottom edge at y = v
 * (the card spans [lo, hi] in x). Returns v, or the position of the nearest such edge within GADGET_SNAP.
 */
static int32_t gadget_snap_line(struct lm_wm *wm, int id, const struct lm_output *o, bool vertical, int32_t v, int32_t lo, int32_t hi)
{
    int32_t best = v;
    int bestd = GADGET_SNAP + 1;
#define TRY_LINE(c) do { int32_t cand_ = (c); int d_ = abs(cand_ - v); if (d_ < bestd) { bestd = d_; best = cand_; } } while (0)
    if (vertical) {
        TRY_LINE(o->x);
        TRY_LINE(o->x + o->width);
    } else {
        TRY_LINE(o->y + LM_PANEL_HEIGHT);
        TRY_LINE(o->y + o->height);
    }
    for (int j = 0; j < LM_GADGET_COUNT; j++) {
        struct lm_rect pr;
        if (j == id || !lm_wm_gadget_rect_id(wm, j, &pr))
            continue;
        if (vertical && lo < pr.y + pr.h + GADGET_SNAP && hi > pr.y - GADGET_SNAP) {
            TRY_LINE(pr.x);
            TRY_LINE(pr.x + pr.w);
        } else if (!vertical && lo < pr.x + pr.w + GADGET_SNAP && hi > pr.x - GADGET_SNAP) {
            TRY_LINE(pr.y);
            TRY_LINE(pr.y + pr.h);
        }
    }
#undef TRY_LINE
    return best;
}

/* Gadget move / resize: the pointer moved by (dx, dy) since the grab began (grab_x/y/w/h = the card back then). */
static void gadget_grab_motion(struct lm_wm *wm, double dx, double dy)
{
    struct lm_output *o = gadget_output(wm);
    struct lm_rect old;
    int id = wm->grab_gadget;
    if (!o || !lm_wm_gadget_rect_id(wm, id, &old))
        return;
    int mw, mh;
    gadget_max_size(id, o, &mw, &mh);
    int32_t x = wm->grab_x, y = wm->grab_y, w = wm->grab_w, h = wm->grab_h;
    int32_t right = x + w, bottom = y + h;
    if (wm->grab_kind == GRAB_GADGET_MOVE) {
        x += (int32_t)dx;
        y += (int32_t)dy;
        gadget_snap(wm, id, o, &x, &y, w, h);
    } else {
        uint32_t e = wm->grab_edges;
        /* the edge that moves sticks to the edges it lines up with; the vertical range of the card is the one it had */
        if (e & LM_EDGE_RIGHT) {
            int32_t edge = gadget_snap_line(wm, id, o, true, x + w + (int32_t)dx, y, y + h);
            w = LM_CLAMP(edge - x, gadget_def[id].min_w, LM_MIN(mw, o->x + o->width - x));
        }
        if (e & LM_EDGE_LEFT) {
            int32_t edge = gadget_snap_line(wm, id, o, true, x + (int32_t)dx, y, y + h);
            w = LM_CLAMP(right - edge, gadget_def[id].min_w, LM_MIN(mw, right - o->x));
            x = right - w;
        }
        if (e & LM_EDGE_BOTTOM) {
            int32_t edge = gadget_snap_line(wm, id, o, false, y + h + (int32_t)dy, x, x + w);
            h = LM_CLAMP(edge - y, gadget_def[id].min_h, LM_MIN(mh, o->y + o->height - y));
        }
        if (e & LM_EDGE_TOP) {
            int32_t edge = gadget_snap_line(wm, id, o, false, y + (int32_t)dy, x, x + w);
            h = LM_CLAMP(bottom - edge, gadget_def[id].min_h, LM_MIN(mh, bottom - o->y - LM_PANEL_HEIGHT));
            y = bottom - h;
        }
    }
    wm->gad[id].x = x;
    wm->gad[id].y = y;
    wm->gad[id].w = w;
    wm->gad[id].h = h;
    wm->gad[id].click_time = 0;         /* it was dragged: the next press is not the second click of a double click */
    struct lm_rect now;
    if (!lm_wm_gadget_rect_id(wm, id, &now) || lm_rect_equal(&now, &old))
        return;
    struct lm_rect ob = gadget_bounds(&old), nb = gadget_bounds(&now);
    damage_rect(wm, &ob);
    damage_rect(wm, &nb);
    if (id == LM_GADGET_CLIPBOARD)                                  /* a docked mixer follows the clipboard card */
        damage_gadget(wm, LM_GADGET_MIXER);
}

static void grab_motion(struct lm_pointer_grab *g, uint32_t time_ms, double x, double y)
{
    (void)time_ms;
    struct lm_wm *wm = g->data;
    struct lm_window *w = wm->grab_win;
    if (wm->grab_kind == GRAB_GADGET_MOVE || wm->grab_kind == GRAB_GADGET_RESIZE) {
        gadget_grab_motion(wm, x - wm->grab_px, y - wm->grab_py);
        return;
    }
    if (wm->grab_kind == GRAB_MIXER) {
        mixer_slide(wm, wm->mix_part, wm->mix_index, x);
        return;
    }
    if (!w)
        return;
    double dx = x - wm->grab_px, dy = y - wm->grab_py;
    if (wm->grab_kind == GRAB_MOVE) {
        struct lm_rect before = frame_outer(w);
        w->x = wm->grab_x + (int32_t)dx;
        w->y = wm->grab_y + (int32_t)dy;
        struct lm_rect ua;
        lm_wm_usable_area(wm, (int32_t)x, (int32_t)y, &ua);
        if (w->y < ua.y + LM_TITLEBAR_HEIGHT && !(w->states & LM_WINDOW_FULLSCREEN))
            w->y = ua.y + LM_TITLEBAR_HEIGHT;
        damage_rect(wm, &before);
        damage_frame(wm, w);
        update_outputs(wm, w);
    } else if (wm->grab_kind == GRAB_RESIZE) {
        int32_t nw = wm->grab_w, nh = wm->grab_h;
        if (wm->grab_edges & LM_EDGE_RIGHT) nw = wm->grab_w + (int32_t)dx;
        if (wm->grab_edges & LM_EDGE_LEFT) nw = wm->grab_w - (int32_t)dx;
        if (wm->grab_edges & LM_EDGE_BOTTOM) nh = wm->grab_h + (int32_t)dy;
        if (wm->grab_edges & LM_EDGE_TOP) nh = wm->grab_h - (int32_t)dy;
        clamp_size(w, &nw, &nh);
        w->states |= LM_WINDOW_RESIZING;
        send_configure(wm, w, nw, nh);
    }
}

static void grab_button(struct lm_pointer_grab *g, uint32_t time_ms, uint32_t button, bool pressed)
{
    (void)time_ms;
    struct lm_wm *wm = g->data;
    if (!pressed && button == BTN_LEFT)
        grab_end(wm);
}

static void grab_cancel(struct lm_pointer_grab *g)
{
    struct lm_wm *wm = g->data;
    struct lm_window *w = wm->grab_win;
    enum grab_kind kind = wm->grab_kind;
    wm->grab_kind = GRAB_NONE;
    wm->grab_win = NULL;
    if (kind == GRAB_GADGET_MOVE || kind == GRAB_GADGET_RESIZE)
        damage_gadget(wm, wm->grab_gadget);
    if (w && kind == GRAB_RESIZE) {
        w->states &= ~LM_WINDOW_RESIZING;
        send_configure(wm, w, w->width, w->height);
    }
}

static const struct lm_pointer_grab_ops grab_ops = {
    .motion = grab_motion,
    .button = grab_button,
    .cancel = grab_cancel,
};

static void grab_begin(struct lm_wm *wm, struct lm_window *w, enum grab_kind kind, uint32_t edges)
{
    if (!wm->srv->seat || wm->grab_kind != GRAB_NONE || (w->states & LM_WINDOW_FULLSCREEN))
        return;
    double px, py;
    lm_seat_pointer_position(wm->srv->seat, &px, &py);

    if (kind == GRAB_RESIZE)
        WIN(w)->tiled = 0;
    if (kind == GRAB_MOVE && ((w->states & LM_WINDOW_MAXIMIZED) || WIN(w)->tiled)) {
        /* dragging a maximized or tiled window restores it under the pointer */
        struct wm_win *ww = WIN(w);
        double ratio = w->width > 0 ? (px - w->x) / w->width : 0.5;
        w->states &= ~LM_WINDOW_MAXIMIZED;
        ww->tiled = 0;
        int32_t rw = ww->have_restore ? ww->rw : w->width * 3 / 4;
        int32_t rh = ww->have_restore ? ww->rh : w->height * 3 / 4;
        ww->have_restore = false;
        struct lm_rect before = frame_outer(w);
        w->x = (int32_t)(px - rw * ratio);
        w->y = (int32_t)py + LM_TITLEBAR_HEIGHT / 2;
        send_configure(wm, w, rw, rh);
        damage_rect(wm, &before);
    }

    wm->grab_kind = kind;
    wm->grab_win = w;
    wm->grab_px = px;
    wm->grab_py = py;
    wm->grab_x = w->x;
    wm->grab_y = w->y;
    wm->grab_w = w->width;
    wm->grab_h = w->height;
    wm->grab_edges = edges;
    wm->grab.ops = &grab_ops;
    wm->grab.data = wm;
    if (lm_seat_pointer_grab(wm->srv->seat, &wm->grab) < 0) {
        wm->grab_kind = GRAB_NONE;
        wm->grab_win = NULL;
        return;
    }
    lm_seat_set_cursor_override(wm->srv->seat, kind == GRAB_MOVE ? LM_CURSOR_MOVE : cursor_for_edges(edges));
}

static void gadget_grab_begin(struct lm_wm *wm, int id, enum grab_kind kind, uint32_t edges)
{
    struct lm_rect r;
    if (!wm->srv->seat || wm->grab_kind != GRAB_NONE || !lm_wm_gadget_rect_id(wm, id, &r))
        return;
    double px, py;
    lm_seat_pointer_position(wm->srv->seat, &px, &py);
    wm->grab_gadget = id;
    wm->gad[id].placed = true;          /* from now on it stays where the user puts it */
    wm->gad[id].x = wm->grab_x = r.x;
    wm->gad[id].y = wm->grab_y = r.y;
    wm->gad[id].w = wm->grab_w = r.w;
    wm->gad[id].h = wm->grab_h = r.h;
    wm->grab_px = px;
    wm->grab_py = py;
    wm->grab_edges = edges;
    wm->grab_win = NULL;
    wm->grab_kind = kind;
    wm->grab.ops = &grab_ops;
    wm->grab.data = wm;
    if (lm_seat_pointer_grab(wm->srv->seat, &wm->grab) < 0) {
        wm->grab_kind = GRAB_NONE;
        return;
    }
    lm_seat_set_cursor_override(wm->srv->seat, kind == GRAB_GADGET_MOVE ? LM_CURSOR_MOVE : cursor_for_edges(edges));
    damage_gadget(wm, id);              /* repaint with the "being dragged" look */
}

/* Slider value from the pointer for the mixer part being dragged. */
static void mixer_slide(struct lm_wm *wm, enum lm_mixer_part part, int idx, double x)
{
    struct lm_rect card;
    if (!wm->srv->mixer || !lm_wm_gadget_rect_id(wm, LM_GADGET_MIXER, &card))
        return;
    const struct lm_mixer_state *st = lm_mixer_state(wm->srv->mixer);
    struct lm_mixer_layout l;
    lm_mixer_layout(&card, GADGET_TITLE_H, st->nstreams, st->backend == LM_MIXER_PULSE, &l);
    if (part == LM_MIXER_MASTER_SLIDER)
        lm_mixer_set_master(wm->srv->mixer, lm_mixer_slider_value(l.master_slider.x, l.master_slider.w, x));
    else if (part == LM_MIXER_STREAM_SLIDER && idx >= 0 && idx < l.nstreams)
        lm_mixer_set_stream(wm->srv->mixer, idx, lm_mixer_slider_value(l.stream[idx].slider.x, l.stream[idx].slider.w, x));
}

/* Left press inside the mixer card: buttons act at once, a slider takes the value and follows the pointer until release. */
static void mixer_press(struct lm_wm *wm)
{
    struct lm_rect card;
    if (!wm->srv->mixer || !wm->srv->seat || !lm_wm_gadget_rect_id(wm, LM_GADGET_MIXER, &card))
        return;
    double px, py;
    lm_seat_pointer_position(wm->srv->seat, &px, &py);
    const struct lm_mixer_state *st = lm_mixer_state(wm->srv->mixer);
    struct lm_mixer_layout l;
    lm_mixer_layout(&card, GADGET_TITLE_H, st->nstreams, st->backend == LM_MIXER_PULSE, &l);
    int idx;
    enum lm_mixer_part part = lm_mixer_hit(&l, (int)px, (int)py, &idx);
    switch (part) {
    case LM_MIXER_MASTER_MUTE: lm_mixer_toggle_master_mute(wm->srv->mixer); break;
    case LM_MIXER_OUTPUT: lm_mixer_next_output(wm->srv->mixer); break;
    case LM_MIXER_STREAM_MUTE: lm_mixer_toggle_stream_mute(wm->srv->mixer, idx); break;
    case LM_MIXER_MASTER_SLIDER:
    case LM_MIXER_STREAM_SLIDER:
        mixer_slide(wm, part, idx, px);
        if (wm->grab_kind != GRAB_NONE)
            break;
        wm->grab_kind = GRAB_MIXER;
        wm->grab_gadget = LM_GADGET_MIXER;
        wm->grab_win = NULL;
        wm->mix_part = part;
        wm->mix_index = idx;
        wm->grab.ops = &grab_ops;
        wm->grab.data = wm;
        if (lm_seat_pointer_grab(wm->srv->seat, &wm->grab) < 0)
            wm->grab_kind = GRAB_NONE;
        break;
    default:
        break;
    }
}

/* Double click on the title bar: back to the default size, docked top right. */
static void gadget_reset(struct lm_wm *wm, int id)
{
    damage_gadgets(wm);
    wm->gad[id].placed = false;
    wm->gad[id].w = gadget_def[id].w;
    wm->gad[id].h = gadget_def[id].h;
    damage_gadgets(wm);
    gadgets_save(wm);
}

void lm_wm_window_request_move(struct lm_window *w)
{
    grab_begin(w->wm, w, GRAB_MOVE, 0);
}

void lm_wm_window_request_resize(struct lm_window *w, uint32_t edges)
{
    if (!edges || (w->states & LM_WINDOW_MAXIMIZED))
        return;
    grab_begin(w->wm, w, GRAB_RESIZE, edges);
}

/* ---- buttons ---------------------------------------------------------------------------------------------------- */

static uint32_t bit_for_kind(enum lm_pick_kind k)
{
    switch (k) {
    case LM_PICK_CLOSE: return LM_FRAME_HOVER_CLOSE;
    case LM_PICK_MAXIMIZE: return LM_FRAME_HOVER_MAXIMIZE;
    case LM_PICK_MINIMIZE: return LM_FRAME_HOVER_MINIMIZE;
    default: return 0;
    }
}

static void launch_run(struct lm_wm *wm)
{
    const char *cmd = wm->srv->config.run_cmd ? wm->srv->config.run_cmd : "mellagui-run";
    lm_server_spawn(wm->srv, cmd);
}

/* Quit, but with a screen lock set only after the secret was typed (lock.h). */
static void request_quit(struct lm_wm *wm)
{
    if (!lm_lock_request_quit(wm->srv->lock))
        lm_server_quit(wm->srv);
}

static void launch_settings(struct lm_wm *wm)
{
    const char *cmd = wm->srv->config.settings_cmd ? wm->srv->config.settings_cmd : "mellagui-settings";
    lm_server_spawn(wm->srv, cmd);
}

static void launch_terminal(struct lm_wm *wm)
{
    const char *cmd = wm->srv->config.terminal_cmd ? wm->srv->config.terminal_cmd : "mellagui-term";
    lm_server_spawn(wm->srv, cmd);
}

/* Click on the screenshot card: the card goes away and, when the shot was saved, the viewer opens it. */
static void open_shot(struct lm_wm *wm)
{
    char path[512];
    if (!lm_render_shot_dismiss(wm->srv->render, path, sizeof(path)))
        return;
    const char *view = wm->srv->config.view_cmd ? wm->srv->config.view_cmd : "mellagui-view";
    char cmd[1200], quoted[1100];
    size_t o = 0;
    for (const char *p = path; *p && o + 5 < sizeof(quoted); p++) {          /* single-quote the path for sh */
        if (*p == '\'') {
            memcpy(quoted + o, "'\\''", 4);
            o += 4;
        } else {
            quoted[o++] = *p;
        }
    }
    quoted[o] = 0;
    snprintf(cmd, sizeof(cmd), "%s '%s'", view, quoted);
    lm_server_spawn(wm->srv, cmd);
}

/* Lock (0) or Exit (1) was chosen in the session dialog. */
static void power_activate(struct lm_wm *wm, int which)
{
    power_set(wm, false, NULL);
    if (which == 0) {
        if (!lm_lock_now(wm->srv->lock))                        /* no password / PIN yet: open the settings to set one */
            launch_settings(wm);
    } else {
        lm_info("session dialog: exit chosen, quitting");
        request_quit(wm);
    }
}

bool lm_wm_pointer_button(struct lm_wm *wm, const struct lm_pick *pick, uint32_t time_ms,
                          uint32_t button, bool pressed)
{
    if (pick->kind == LM_PICK_POWER_BUTTON || pick->kind == LM_PICK_POWER_CARD || pick->kind == LM_PICK_POWER_BACKDROP) {
        int pk = wm->press_panel_kind, pi = wm->press_panel_ws;
        if (pressed) {
            if (pick->kind == LM_PICK_POWER_BACKDROP)           /* a click outside the card closes the dialog, nothing below gets it */
                power_set(wm, false, NULL);
            else if (pick->kind == LM_PICK_POWER_BUTTON && button == BTN_LEFT) {
                wm->press_panel_kind = LM_PICK_POWER_BUTTON;
                wm->press_panel_ws = pick->index;
                wm->press_win = NULL;
                wm->press_bits = 0;
            }
        } else {
            wm->press_panel_kind = 0;
            if (pk == LM_PICK_POWER_BUTTON && pick->kind == LM_PICK_POWER_BUTTON && pick->index == pi && button == BTN_LEFT)
                power_activate(wm, pi);                         /* released on the button it was pressed on */
        }
        return true;
    }
    if (pick->kind == LM_PICK_NOTIFY) {                    /* a click dismisses a notification and goes no further */
        if (pressed)
            note_dismiss(wm, pick->index);
        return true;
    }
    if (pick->kind == LM_PICK_SHOT_CARD) {                 /* nothing under the card gets the click */
        if (pressed && button == BTN_LEFT)
            open_shot(wm);
        else if (pressed)
            lm_render_shot_dismiss(wm->srv->render, NULL, 0);
        return true;
    }
    if (pressed && wm->clip_nav && pick->kind != LM_PICK_CLIP_ENTRY)
        clip_nav_set(wm, false);

    /* Any press outside the menu (and not on the launcher, which toggles it) closes it; the click goes on. */
    if (pressed && wm->menu_open && pick->kind != LM_PICK_MENU_ITEM && pick->kind != LM_PICK_PANEL_LAUNCHER)
        menu_set(wm, false, NULL);

    /* Releases of decoration/panel presses we consumed. */
    if (!pressed && (wm->press_bits || wm->press_panel_kind)) {
        struct lm_window *pw = wm->press_win;
        uint32_t pb = wm->press_bits;
        int pk = wm->press_panel_kind, pi = wm->press_panel_ws;
        wm->press_win = NULL;
        wm->press_bits = 0;
        wm->press_panel_kind = 0;
        if (pw)
            damage_frame(wm, pw);
        if (pb && pick->window == pw && bit_for_kind(pick->kind) == pb && pw) {
            switch (pb) {
            case LM_FRAME_HOVER_CLOSE: lm_wm_window_close(pw); break;
            case LM_FRAME_HOVER_MAXIMIZE:
                lm_wm_window_request_maximize(pw, !(pw->states & LM_WINDOW_MAXIMIZED));
                break;
            case LM_FRAME_HOVER_MINIMIZE: lm_wm_window_request_minimize(pw); break;
            }
        }
        if (pk == LM_PICK_PANEL_LAUNCHER && pick->kind == LM_PICK_PANEL_LAUNCHER) {
            if (wm->srv->config.napps > 0)
                menu_set(wm, !wm->menu_open, &pick->rect);
            else
                launch_terminal(wm);
        }
        if (pk == LM_PICK_MENU_ITEM && pick->kind == LM_PICK_MENU_ITEM && pick->index == pi) {
            const struct lm_app_entry *app = &wm->srv->config.apps[pi];
            menu_set(wm, false, NULL);
            lm_server_spawn(wm->srv, app->cmd);
        }
        if (pk == LM_PICK_PANEL_RUN && pick->kind == LM_PICK_PANEL_RUN)
            launch_run(wm);
        if (pk == LM_PICK_PANEL_LAYOUT && pick->kind == LM_PICK_PANEL_LAYOUT && wm->srv->input)
            lm_input_cycle_layout(wm->srv->input, pi);
        if (pk == LM_PICK_PANEL_STICKY && pick->kind == LM_PICK_PANEL_STICKY && wm->srv->input)
            lm_input_set_sticky(wm->srv->input, !lm_input_sticky(wm->srv->input));
        if (pk == LM_PICK_PANEL_POWER && pick->kind == LM_PICK_PANEL_POWER) {
            double px = 0, py = 0;
            if (wm->srv->seat)
                lm_seat_pointer_position(wm->srv->seat, &px, &py);
            power_set(wm, true, lm_output_nearest(wm->srv, px, py));        /* on the output whose panel was clicked */
        }
        return true;
    }
    if (!pressed)
        return false;

    switch (pick->kind) {
    case LM_PICK_SURFACE:
        if (pick->window && wm->focused != pick->window)
            lm_wm_focus_window(wm, pick->window);
        else if (pick->window)
            raise_window(wm, pick->window);
        return false;
    case LM_PICK_CLOSE:
    case LM_PICK_MAXIMIZE:
    case LM_PICK_MINIMIZE:
        if (button != BTN_LEFT)
            return true;
        lm_wm_focus_window(wm, pick->window);
        wm->press_win = pick->window;
        wm->press_bits = bit_for_kind(pick->kind);
        damage_frame(wm, pick->window);
        return true;
    case LM_PICK_TITLEBAR: {
        if (!pick->window)
            return true;
        lm_wm_focus_window(wm, pick->window);
        if (button != BTN_LEFT)
            return true;
        if (wm->last_click_win == pick->window && time_ms - wm->last_click_time < DOUBLE_CLICK_MS) {
            wm->last_click_win = NULL;
            lm_wm_window_request_maximize(pick->window, !(pick->window->states & LM_WINDOW_MAXIMIZED));
            return true;
        }
        wm->last_click_win = pick->window;
        wm->last_click_time = time_ms;
        grab_begin(wm, pick->window, GRAB_MOVE, 0);
        return true;
    }
    case LM_PICK_BORDER:
        if (!pick->window)
            return true;
        lm_wm_focus_window(wm, pick->window);
        if (button == BTN_LEFT)
            grab_begin(wm, pick->window, GRAB_RESIZE, pick->edges);
        return true;
    case LM_PICK_GADGET_TITLE: {
        int id = pick->index;
        if (button != BTN_LEFT || id < 0 || id >= LM_GADGET_COUNT)
            return true;
        if (time_ms - wm->gad[id].click_time < DOUBLE_CLICK_MS) {
            wm->gad[id].click_time = 0;
            gadget_reset(wm, id);
            return true;
        }
        wm->gad[id].click_time = time_ms;
        gadget_grab_begin(wm, id, GRAB_GADGET_MOVE, 0);
        return true;
    }
    case LM_PICK_GADGET_BORDER:
        if (button == BTN_LEFT && pick->index >= 0 && pick->index < LM_GADGET_COUNT)
            gadget_grab_begin(wm, pick->index, GRAB_GADGET_RESIZE, pick->edges);
        return true;
    case LM_PICK_GADGET_BODY:
        if (button == BTN_LEFT)
            mixer_press(wm);
        return true;
    case LM_PICK_CLIP_ENTRY:
        if (button == BTN_LEFT)
            lm_seat_clip_use(wm->srv->seat, pick->index);
        return true;
    case LM_PICK_MENU_ITEM:
        if (button == BTN_LEFT) {
            wm->press_panel_kind = LM_PICK_MENU_ITEM;
            wm->press_panel_ws = pick->index;
            wm->press_win = NULL;
            wm->press_bits = 0;
        }
        return true;
    case LM_PICK_PANEL_LAUNCHER:
        if (button == BTN_LEFT) {
            wm->press_panel_kind = LM_PICK_PANEL_LAUNCHER;
            wm->press_win = NULL;
            wm->press_bits = 0;
        }
        return true;
    case LM_PICK_PANEL_RUN:
    case LM_PICK_PANEL_POWER:
        if (button == BTN_LEFT) {
            wm->press_panel_kind = pick->kind;
            wm->press_win = NULL;
            wm->press_bits = 0;
        }
        return true;
    case LM_PICK_PANEL_LAYOUT:
    case LM_PICK_PANEL_STICKY:
        if (button == BTN_LEFT || (button == BTN_RIGHT && pick->kind == LM_PICK_PANEL_LAYOUT)) {
            wm->press_panel_kind = pick->kind;
            wm->press_panel_ws = button == BTN_RIGHT ? -1 : 1;     /* layout direction, see the release code */
            wm->press_win = NULL;
            wm->press_bits = 0;
        }
        return true;
    case LM_PICK_PANEL_WORKSPACE:
        if (button == BTN_LEFT)
            lm_wm_switch_workspace(wm, pick->index);
        return true;
    case LM_PICK_PANEL_TASK:
        if (button == BTN_LEFT && pick->window) {
            if (pick->window == wm->focused && !pick->window->minimized)
                lm_wm_window_request_minimize(pick->window);
            else
                lm_wm_focus_window(wm, pick->window);
        } else if (button == BTN_MIDDLE && pick->window) {
            lm_wm_window_close(pick->window);
        }
        return true;
    case LM_PICK_PANEL:
        return true;
    case LM_PICK_NONE:
    default:
        return false;
    }
}

/* ---- keyboard shortcuts ---------------------------------------------------------------------------------------------- */

static void move_to_workspace(struct lm_wm *wm, struct lm_window *w, int ws)
{
    if (ws < 0 || ws >= LM_NUM_WORKSPACES || w->workspace == ws)
        return;
    struct lm_rect r = frame_outer(w);
    lm_xdg_dismiss_popups(w->surface);
    w->workspace = ws;
    update_outputs(wm, w);
    damage_rect(wm, &r);
    if (wm->focused == w)
        set_focus(wm, top_visible(wm, w));
    damage_panels(wm);
    refresh_pointer(wm);
}

static void cycle_focus(struct lm_wm *wm, bool backwards)
{
    struct lm_window *w, *pick = NULL;
    if (backwards) {
        /* second from the top: swap back to the previous window */
        int seen = 0;
        lm_list_for_each_reverse(w, &wm->windows, link)
            if (w->mapped && w->workspace == wm->workspace && !w->minimized && ++seen == 2) {
                pick = w;
                break;
            }
    } else {
        /* the lowest window of the workspace (minimized ones included) comes to the front */
        lm_list_for_each(w, &wm->windows, link)
            if (w->mapped && w->workspace == wm->workspace && w != wm->focused) {
                pick = w;
                break;
            }
    }
    if (pick)
        lm_wm_focus_window(wm, pick);
}

void lm_wm_keyboard_state_changed(struct lm_wm *wm)
{
    damage_panels(wm);
}

void lm_wm_lock_changed(struct lm_wm *wm, bool locked)
{
    if (!wm)
        return;
    if (locked) {
        sw_close(wm);
        power_set(wm, false, NULL);
        menu_set(wm, false, NULL);
        clip_nav_set(wm, false);
        if (wm->focused)
            lm_xdg_dismiss_popups(wm->focused->surface);
    } else if (wm->srv->seat) {
        struct lm_window *w = wm->focused ? wm->focused : top_visible(wm, NULL);
        if (w && w != wm->focused)
            set_focus(wm, w);
        else if (w)
            lm_seat_set_keyboard_focus(wm->srv->seat, w->surface);
    }
    damage_all(wm);
}

/* Print: the output under the pointer, Alt+Print: the focused window with its frame (the whole output when there is
 * none), Ctrl+Print: like Print but only to the clipboard. */
static void take_screenshot(struct lm_wm *wm, bool window, bool to_file)
{
    struct lm_rect r;
    if (window && wm->focused && win_visible(wm, wm->focused)) {
        r = frame_outer(wm->focused);
    } else {
        double x = 0, y = 0;
        if (wm->srv->seat)
            lm_seat_pointer_position(wm->srv->seat, &x, &y);
        struct lm_output *o = lm_output_nearest(wm->srv, x, y);
        if (!o)
            return;
        r = lm_output_rect(o);
    }
    lm_screenshot_take(wm->srv, &r, to_file);
}

bool lm_wm_key(struct lm_wm *wm, uint32_t code, bool pressed, bool repeat, uint32_t mods)
{
    mods &= ~(uint32_t)(LM_MOD_CAPS | LM_MOD_NUM);
    bool super = (mods & LM_MOD_SUPER) != 0;
    bool shift = (mods & LM_MOD_SHIFT) != 0;
    bool ctrl = (mods & LM_MOD_CTRL) != 0;
    bool alt = (mods & LM_MOD_ALT) != 0;

    if (wm->power_open && !(ctrl && alt)) {                     /* the session dialog is modal: it takes every key */
        if (pressed && !repeat) {
            if (code == KEY_ESC)
                power_set(wm, false, NULL);
            else if (code == KEY_LEFT || code == KEY_RIGHT || code == KEY_TAB) {
                wm->power_sel = wm->power_sel < 0 ? (code == KEY_LEFT ? 1 : 0) : 1 - wm->power_sel;
                power_damage(wm);
            } else if ((code == KEY_ENTER || code == KEY_KPENTER) && wm->power_sel >= 0)
                power_activate(wm, wm->power_sel);
            else if (code == KEY_L)
                power_activate(wm, 0);
            else if (code == KEY_E)
                power_activate(wm, 1);
        }
        return true;
    }
    if (wm->sw_open) {                                          /* Alt+Tab: Alt released = go there, Esc = stay */
        if (!pressed && (code == KEY_LEFTALT || code == KEY_RIGHTALT)) {
            sw_commit(wm);
            return false;                                       /* the application still gets the release it never saw pressed */
        }
        if (pressed && code == KEY_ESC) {
            sw_close(wm);
            return true;
        }
    }
    if (code == KEY_TAB && alt && !super && !ctrl) {            /* Alt+Tab / Alt+Shift+Tab: the next / previous window in use order */
        if (pressed)
            sw_step(wm, shift);
        return true;
    }
    if (code == KEY_ESC && wm->menu_open) {
        if (pressed && !repeat)
            menu_set(wm, false, NULL);
        return true;
    }

    /* Ctrl+Alt+F1..F12 */
    if (ctrl && alt && !super) {
        int vt = 0;
        if (code >= KEY_F1 && code <= KEY_F10)
            vt = (int)(code - KEY_F1) + 1;
        else if (code == KEY_F11)
            vt = 11;
        else if (code == KEY_F12)
            vt = 12;
        if (vt) {
            if (pressed && !repeat && wm->srv->backend)
                lm_backend_switch_vt(wm->srv->backend, vt);
            return true;
        }
    }
    if (wm->clip_nav && !super && !ctrl && !alt) {
        bool arrow = code == KEY_UP || code == KEY_DOWN || code == KEY_HOME || code == KEY_END;
        bool pick = code == KEY_ENTER || code == KEY_KPENTER;
        if (code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT || code == KEY_CAPSLOCK || code == KEY_NUMLOCK ||
            code == KEY_LEFTMETA || code == KEY_RIGHTMETA || code == KEY_LEFTCTRL || code == KEY_RIGHTCTRL ||
            code == KEY_LEFTALT || code == KEY_RIGHTALT)
            return false;                                       /* modifiers do not end the mode (the Super of Super+C is released) */
        if (!arrow && !pick && code != KEY_ESC) {
            clip_nav_set(wm, false);                            /* any other key: the application gets it */
            return false;
        }
        if (!pressed)
            return true;
        int n = clip_nav_count(wm);
        if (n == 0 || code == KEY_ESC) {
            clip_nav_set(wm, false);
        } else if (pick) {
            if (!repeat)
                lm_seat_clip_use(wm->srv->seat, LM_CLAMP(wm->clip_cur, 0, n - 1));
            clip_nav_set(wm, false);
        } else {
            int cur = LM_CLAMP(wm->clip_cur, 0, n - 1);
            cur = code == KEY_UP ? LM_MAX(cur - 1, 0) : code == KEY_DOWN ? LM_MIN(cur + 1, n - 1) :
                  code == KEY_HOME ? 0 : n - 1;
            if (cur != wm->clip_cur) {
                wm->clip_cur = cur;
                damage_gadget(wm, LM_GADGET_CLIPBOARD);
            }
        }
        return true;
    }
    if ((code == KEY_SYSRQ || code == KEY_PRINT) && !super) {
        if (pressed && !repeat)
            take_screenshot(wm, alt, !ctrl);
        return true;
    }
    if (!super || ctrl || alt)
        return false;

    bool handled = true;
    switch (code) {
    case KEY_ENTER:
        if (pressed && !repeat) launch_terminal(wm);
        break;
    case KEY_R:
        if (pressed && !repeat) launch_run(wm);
        break;
    case KEY_I:
        if (pressed && !repeat) launch_settings(wm);
        break;
    case KEY_SPACE:                         /* Super+Space / Super+Shift+Space: next / previous keyboard layout */
        if (pressed && !repeat && wm->srv->input)
            lm_input_cycle_layout(wm->srv->input, shift ? -1 : 1);
        break;
    case KEY_G:
        if (pressed && !repeat) lm_wm_set_gadget(wm, !wm->gad[LM_GADGET_CLIPBOARD].visible);
        break;
    case KEY_C:                             /* Super+C: pick from the clipboard history with the arrow keys */
        if (pressed && !repeat) {
            if (!wm->gad[LM_GADGET_CLIPBOARD].visible)
                lm_wm_set_gadget(wm, true);
            if (clip_nav_count(wm) > 0)
                clip_nav_set(wm, !wm->clip_nav);
        }
        break;
    case KEY_V:
        if (pressed && !repeat) lm_wm_set_gadget_id(wm, LM_GADGET_MIXER, !wm->gad[LM_GADGET_MIXER].visible);
        break;
    case KEY_S:
        if (pressed && !repeat) lm_wm_set_gadget_id(wm, LM_GADGET_SYSTEM, !wm->gad[LM_GADGET_SYSTEM].visible);
        break;
    case KEY_Q:
        if (pressed && !repeat && wm->focused) lm_wm_window_close(wm->focused);
        break;
    case KEY_L:                             /* Super+L: lock the screen (only when a password / PIN is set) */
        if (pressed && !repeat && !lm_lock_now(wm->srv->lock))
            lm_info("lock: Super+L ignored, no password or PIN is set (settings: Security)");
        break;
    case KEY_1: case KEY_2: case KEY_3: case KEY_4:
        if (pressed && !repeat) {
            int ws = (int)(code - KEY_1);
            if (shift) {
                if (wm->focused) move_to_workspace(wm, wm->focused, ws);
            } else {
                lm_wm_switch_workspace(wm, ws);
            }
        }
        break;
    case KEY_TAB:
        if (pressed) cycle_focus(wm, shift);
        break;
    case KEY_F:
        if (pressed && !repeat && wm->focused)
            lm_wm_window_request_fullscreen(wm->focused, !(wm->focused->states & LM_WINDOW_FULLSCREEN));
        break;
    case KEY_M:
        if (pressed && !repeat && wm->focused)
            lm_wm_window_request_maximize(wm->focused, !(wm->focused->states & LM_WINDOW_MAXIMIZED));
        break;
    case KEY_LEFT: case KEY_RIGHT:
        if (pressed && !repeat && wm->focused)
            lm_wm_window_request_tile(wm->focused, code == KEY_LEFT ? LM_EDGE_LEFT : LM_EDGE_RIGHT);
        break;
    case KEY_UP:
        if (pressed && !repeat && wm->focused)
            lm_wm_window_request_maximize(wm->focused, true);
        break;
    case KEY_DOWN:
        if (pressed && !repeat && wm->focused) {
            if (wm->focused->states & LM_WINDOW_MAXIMIZED)
                lm_wm_window_request_maximize(wm->focused, false);
            else
                lm_wm_window_request_tile(wm->focused, LM_EDGE_NONE);
        }
        break;
    case KEY_E:
        if (shift) {
            if (pressed && !repeat) request_quit(wm);
        } else {
            handled = false;
        }
        break;
    default:
        handled = false;
    }
    return handled;
}
