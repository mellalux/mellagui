/*
 * Window manager + shell policy. See wm.h for the contract.
 */
#include "metrics.h"
#include "keys.h"
#include "launchers.h"
#include "wm.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

#include "about.h"
#include "clockfmt.h"
#include "saver.h"
#include "backend.h"
#include "input.h"
#include "font.h"
#include "keymap.h"
#include "log.h"
#include "mixer.h"
#include "sysinfo.h"
#include "osk.h"
#include "output.h"
#include "render.h"
#include "screenshot.h"
#include "tray.h"
#include "seat.h"
#include "settings.h"
#include "surface.h"
#include "xdg.h"

#define RESIZE_CORNER (lm_metrics.resize_corner)
#define DOUBLE_CLICK_MS (lm_metrics.double_click_ms)
#define PANEL_WS_W (lm_metrics.panel_ws_width)
#define PANEL_CLOCK_MIN_W 136                   /* "DD.MM.YYYY HH:MM" = 16 glyphs; longer texts (seconds, a weekday ...) make it wider */
#define PANEL_POWER_D (LM_PANEL_HEIGHT - 4)     /* the round power button's diameter: as tall as the other buttons */
#define POWER_CARD_W 264                        /* the session dialog */
#define POWER_CARD_H 172
#define POWER_BTN_D 64                          /* its round buttons */
#define ABOUT_CARD_W 520                        /* the About dialog */
#define ABOUT_ROW_H 18
#define PANEL_RUN_W (lm_metrics.panel_run_width)
#define PANEL_LAYOUT_W (lm_metrics.panel_layout_width)
#define PANEL_STICKY_W (lm_metrics.panel_sticky_width)
#define PANEL_OSK_W 36                              /* the on-screen keyboard button (not a configurable size) */
#define PANEL_CAPS_W (lm_metrics.panel_caps_width)
#define SW_MAX_WINDOWS 32
#define GADGET_W (lm_metrics.gadget_width)                /* default size (5 entries of 1 line; taller cards give the entries more lines) */
#define GADGET_H (GADGET_TITLE_H + LM_CLIP_DEFAULT_ENTRIES * GADGET_ENTRY_H + GADGET_PAD)
#define GADGET_MIN_W 180
#define GADGET_MAX_W 640
#define GADGET_MIN_H (GADGET_TITLE_H + GADGET_ENTRY_H + GADGET_PAD)
#define GADGET_MAX_H 1600           /* taller than 5 slots: more entries are shown at once */
#define GADGET_TITLE_H 26
#define GADGET_ENTRY_H 36
#define GADGET_PAD 8
#define GADGET_MARGIN (lm_metrics.gadget_margin)
#define SYS_W (lm_metrics.sys_width)
#define SYS_H (lm_metrics.sys_height)                   /* title, CPU, graph, cores, memory, swap, temperature, the device rows that fit */
#define SYS_MIN_W 210
#define SYS_MIN_H 130
#define MIXER_W (lm_metrics.mixer_width)
#define MIXER_H (lm_metrics.mixer_height)                 /* title, master, output, two application rows, hint */
#define MIXER_MIN_W 210
#define MIXER_MIN_H 120
#define GADGET_SNAP (lm_metrics.gadget_snap)              /* a dragged gadget sticks to another gadget's / the screen's edge within this many pixels */
#define GADGET_EDGE 6               /* resize zone along the left/right/bottom edge (top: 3 px) */
#define PANEL_TASK_MAX_W (lm_metrics.panel_task_max_width)
#define PANEL_TASK_GAP (lm_metrics.panel_task_gap)

struct wm_win {
    struct lm_window pub;       /* must be first */
    int id;
    bool placed;
    /* geometry to return to when leaving maximized/fullscreen */
    int32_t rx, ry, rw, rh;
    bool have_restore;
    uint32_t tiled;             /* LM_EDGE_LEFT / LM_EDGE_RIGHT half-screen tile, 0 = none */
    uint64_t focus_seq;         /* when it last had the focus (Alt+Tab lists the most recent first) */
    /* the on-screen keyboard made it move up / shrink (setting osk-avoid): where it was, and what it was given (when it is not that any more
     * the user changed it and it stays as it is) */
    bool osk_moved;
    int32_t osk_ox, osk_oy, osk_ow, osk_oh;
    int32_t osk_ax, osk_ay, osk_aw, osk_ah;
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
    pid_t cal_pid;                      /* the calendar program we started (0 = none) */
    bool cal_pending;                   /* a click started it: show it when its window appears */
    uint64_t cal_pending_ms;
    struct lm_rect cal_clock;           /* where the clock was clicked (the calendar opens under it) */
    struct lm_timer *clock_timer;
    struct note { int id; char title[LM_NOTIFY_COLS * 4 + 1]; char body[200]; int64_t expire; } notes[LM_NOTIFY_MAX];   /* newest first */
    int nnotes, next_note_id;
    struct lm_timer *note_timer;
    char clock_text[40];

    /* hover / pressed state */
    struct lm_window *hover_win;
    uint32_t hover_bits;
    int hover_panel_ws;         /* -1 none */
    bool hover_launcher;
    bool hover_power, hover_run, hover_layout, hover_sticky, hover_osk;
    int hover_tray;             /* id of the tray item under the pointer, 0 = none */
    bool osk_open;              /* the on-screen keyboard is showing on the primary output */
    uint32_t osk_latched;       /* LM_MOD_* its own Shift / Ctrl / Alt / AltGr keys hold for the next key */
    int osk_hover, osk_down;    /* the key under the pointer / held by it (index into the geometry), -1 = none */
    struct lm_osk_place osk_pl;  /* where the user put it and how big (w == 0: the default) */
    uint32_t osk_phys[24];      /* bit set = this evdev key is held down on a keyboard (the on-screen keys light up with them) */
    uint32_t osk_click_ms;      /* when the margin was last pressed (a double click resets the place) */
    int osk_drag;               /* 0 = none, 1 = moving, 2 = resizing the keyboard with the pointer */
    uint32_t osk_drag_edges;    /* LM_EDGE_RIGHT / LM_EDGE_BOTTOM while resizing */
    int osk_press_x, osk_press_y;
    struct lm_osk_place osk_start;   /* the place when the drag began */
    uint32_t osk_down_code, osk_down_mods;   /* what was pressed for the held key: its code and the latched modifiers pressed with it */
    bool power_open;            /* the session dialog (Lock / Exit) is showing on the output called power_out */
    char power_out[32];
    int power_sel;              /* its highlighted button: 0 Lock, 1 Exit, -1 none yet */
    bool key_capture;           /* the next key combination is recorded, not handled (lm_wm_key_capture) */
    char key_captured[64];      /* ... and the result ("" = cancelled / none yet) */
    uint32_t capture_release;   /* its key: the release is swallowed, too (0 = none) */
    bool about_open;            /* the About dialog is showing on the output called about_out */
    char about_out[32];
    bool about_hover_close;
    struct lm_about_row about_rows[LM_ABOUT_ROWS];
    int about_n;
    bool launcher_open;         /* the launcher dialog is showing on the output called launcher_out */
    char launcher_out[32];
    bool launcher_edit, launcher_form;
    bool launcher_editing;      /* the form changes the button (launcher_edit_raw, in the launcher's own list when launcher_edit_only) */
    bool launcher_edit_only;
    int launcher_edit_raw;
    char launcher_icon[LM_DOCK_ICON_MAX];           /* the form's icon text: empty (guess), a built-in name, theme:NAME or a picture file */
    int launcher_sel, launcher_hover_btn, launcher_focus;
    char launcher_name[LM_DOCK_NAME_MAX], launcher_cmd[LM_DOCK_CMD_MAX], launcher_msg[112];
    /* mouse drags (a press on an icon that moved far enough): a launcher icon onto the dock, a dock icon along the dock (reorder) or out of it
     * (back to the launcher). drag_x/y = the pointer. */
    int drag_press_x, drag_press_y;
    bool dock_press_left;
    bool launcher_drag, dock_drag;
    int launcher_drag_g;                // grid index being dragged
    int launcher_drag_pos;              // where it would join the dock (resolved index, count = at the end), -1 not over the dock
    int dock_drag_idx, dock_drag_to;    // dock index being dragged, where it would go (-1 = out of the dock: it leaves)
    double drag_x, drag_y;
    /* the dock: dock_active = the pointer is in it (so it is magnifying), hover_dock = the icon under the pointer, dock_shown = autohide has
     * revealed it, dock_timer hides it again a moment after the pointer left */
    bool dock_active, dock_shown;
    int hover_dock, dock_px;
    struct lm_timer *dock_timer;
    /* programs the dock has started and that have not shown a window yet: their icon bounces (start time, windows it had then) */
    int64_t dock_launch_ms[LM_DOCK_MAX];
    int dock_launch_base[LM_DOCK_MAX];
    struct lm_timer *dock_anim;
    struct lm_idle *refit;      /* after an output went away: fit the maximized / fullscreen windows to what is left, once the output is out of the list */
    /* Alt+Tab: the list is open while Alt is held; windows most recently used first */
    bool sw_open;
    int sw_n, sw_idx;
    struct lm_window *sw_win[SW_MAX_WINDOWS];
    bool clip_nav;              /* keyboard mode of the clipboard gadget (Super+C): arrows move, Enter uses the entry */
    int clip_cur;               /* the highlighted entry while clip_nav (index in the history) */
    int clip_top;               /* the first history entry the card shows (it scrolls) */
    int clip_total;             /* history length when the card last changed (a longer one = something new was copied: back to the top) */
    int hover_clip;             /* what the pointer is on in the card: see lm_gadget_info.hover */
    struct {                    /* the desktop gadgets, indexed by enum lm_gadget_id */
        bool visible;
        bool placed;            /* false: docked top right (default); true: x/y chosen by dragging */
        int32_t x, y, w, h;
        uint32_t click_time;
    } gad[LM_GADGET_COUNT];
    int grab_gadget;            /* the gadget being moved / resized / operated */
    enum lm_mixer_part mix_part;/* GRAB_MIXER: the slider being dragged ... */
    int mix_index;              /* ... and its stream row */
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

/* The calendar program's window: it lives hidden (minimized) and is not listed in the taskbar, Alt+Tab or the dock. */
static bool win_is_calendar(const struct lm_window *w)
{
    return w->app_id && !strcmp(w->app_id, "mellagui.calendar");
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

static struct lm_window *calendar_window(struct lm_wm *wm);
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
    p->launcher = lm_rect_make(p->bar.x + 4, y, h, h);              /* the round logo button */
    int x = p->launcher.x + p->launcher.w + 10;
    for (int i = 0; i < LM_NUM_WORKSPACES; i++) {
        p->workspace[i] = lm_rect_make(x, y, PANEL_WS_W, h);
        x += PANEL_WS_W + 2;
    }
    p->power = lm_rect_make(p->bar.x + p->bar.w - PANEL_POWER_D - 4, y, PANEL_POWER_D, PANEL_POWER_D);
    int clock_w = LM_MAX(PANEL_CLOCK_MIN_W, lm_font_text_width(wm->clock_text) + 8);
    p->clock = lm_rect_make(p->power.x - 4 - clock_w, y, clock_w, h);
    p->run = lm_rect_make(p->clock.x - 8 - PANEL_RUN_W, y, PANEL_RUN_W, h);
    p->sticky = lm_rect_make(p->run.x - 8 - PANEL_STICKY_W, y, PANEL_STICKY_W, h);
    p->layout = lm_rect_make(p->sticky.x - 4 - PANEL_LAYOUT_W, y, PANEL_LAYOUT_W, h);
    p->osk = lm_rect_make(p->layout.x - 4 - PANEL_OSK_W, y, PANEL_OSK_W, h);
    p->hover_osk = wm->hover_osk;
    p->osk_open = wm->osk_open;
    uint32_t locks = wm->srv->input ? lm_input_locks(wm->srv->input) : 0;
    p->caps_text[0] = 0;
    if ((locks & LM_MOD_CAPS) && (locks & LM_MOD_NUM))
        snprintf(p->caps_text, sizeof(p->caps_text), "CAPS+N");
    else if (locks & LM_MOD_CAPS)
        snprintf(p->caps_text, sizeof(p->caps_text), "CAPS");
    else if (locks & LM_MOD_NUM)
        snprintf(p->caps_text, sizeof(p->caps_text), "NUM");
    int caps_w = !p->caps_text[0] ? 0 : (locks & LM_MOD_NUM) && (locks & LM_MOD_CAPS) ? PANEL_CAPS_W + 24 : PANEL_CAPS_W;
    p->caps = lm_rect_make(p->osk.x - (caps_w ? 4 + caps_w : 0), y, caps_w, h);
    int tx = x + 12;
    int title_end = p->caps.x;
    if (wm->srv->tray) {                                            /* programs' icons: the first one created is the rightmost */
        int right = (caps_w ? p->caps.x : p->osk.x) - 4, n = lm_tray_count(wm->srv->tray);
        for (int i = 0; i < n && p->ntray < LM_PANEL_MAX_TRAY; i++) {
            struct lm_tray_view v;
            if (!lm_tray_get(wm->srv->tray, i, &v))
                break;
            int iw = v.icon ? v.icon->width : 0, tw = v.text[0] ? lm_font_text_width(v.text) : 0;
            int bw = 12 + iw + (iw && tw ? 4 : 0) + tw;
            if (bw < h)
                bw = h;
            if (right - bw < tx + 120)                              /* the taskbar keeps some room */
                break;
            int k = p->ntray++;
            p->tray[k].rect = lm_rect_make(right - bw, y, bw, h);
            p->tray[k].id = v.id;
            p->tray[k].icon = v.icon;
            p->tray[k].text = v.text;
            p->tray[k].hover = wm->hover_tray == v.id;
            right -= bw + 4;
            title_end = p->tray[k].rect.x - 4;
        }
    }
    p->title_area = lm_rect_make(tx, y, title_end - 12 - tx, h);
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
    p->about_open = wm->about_open;
    p->hover_power = wm->hover_power;
    p->power_open = wm->power_open;
    p->hover_run = wm->hover_run;
    p->active_title = wm->focused ? wm->focused->title : "";

    /* taskbar: windows of this workspace in creation (id) order, so raising does not reshuffle it */
    struct lm_window *tw[LM_PANEL_MAX_TASKS];
    int n = 0;
    lm_list_for_each(w, &wm->windows, link) {
        if (!w->mapped || w->workspace != wm->workspace || n >= LM_PANEL_MAX_TASKS || win_is_calendar(w))
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

#define NOTE_W (lm_metrics.note_width)
#define NOTE_GAP (lm_metrics.note_gap)

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
            if (w->mapped && wm->sw_n < SW_MAX_WINDOWS && !win_is_calendar(w))
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

static struct lm_output *gadget_output(const struct lm_wm *wm)
{
    return lm_output_primary(wm->srv);
}

/* Default / minimum / maximum card size per gadget. */
static struct { int w, h, min_w, min_h, max_w, max_h; } gadget_def[LM_GADGET_COUNT] = {
    [LM_GADGET_CLIPBOARD] = { 0, 0, GADGET_MIN_W, GADGET_MIN_H, GADGET_MAX_W, GADGET_MAX_H },
    [LM_GADGET_MIXER] = { 0, 0, MIXER_MIN_W, MIXER_MIN_H, GADGET_MAX_W, GADGET_MAX_H },
    [LM_GADGET_SYSTEM] = { 0, 0, SYS_MIN_W, SYS_MIN_H, GADGET_MAX_W, GADGET_MAX_H },
};

/* The starting sizes come from [gadgets] in mellagui.conf (metrics.h), so they are filled in when the wm is created. */
static void gadget_def_init(void)
{
    gadget_def[LM_GADGET_CLIPBOARD].w = GADGET_W;
    gadget_def[LM_GADGET_CLIPBOARD].h = GADGET_H;
    gadget_def[LM_GADGET_MIXER].w = LM_MAX(MIXER_W, MIXER_MIN_W);
    gadget_def[LM_GADGET_MIXER].h = MIXER_H;
    gadget_def[LM_GADGET_SYSTEM].w = SYS_W;
    gadget_def[LM_GADGET_SYSTEM].h = SYS_H;
}

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
    if (wm->osk_pl.w > 0)                                           /* the on-screen keyboard: top left corner, width and key height */
        fprintf(f, "keyboard %d %d %d %d\n", wm->osk_pl.x, wm->osk_pl.y, wm->osk_pl.w, wm->osk_pl.kh);
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
        if (!strcmp(name, "keyboard")) {                            /* (clamped to the output and to the size limits when it is laid out) */
            if (abs(x) <= 20000 && abs(y) <= 20000 && w > 0 && w <= 20000 && h > 0 && h <= 1000) {
                wm->osk_pl.x = x;
                wm->osk_pl.y = y;
                wm->osk_pl.w = w;
                wm->osk_pl.kh = h;
            }
            continue;
        }
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

/* Entry slots of a card of height `h`: as many as fit; the entries have one fixed height, the card scrolls through a longer history. */
static void gadget_slots(int h, int *slots, int *entry_h)
{
    int body = h - GADGET_TITLE_H - GADGET_PAD;
    *slots = LM_CLAMP(body / GADGET_ENTRY_H, 1, LM_GADGET_ENTRIES);
    *entry_h = GADGET_ENTRY_H;
}

/* The title bar buttons of a clipboard card: [0] up, [1] down, [2] clear all. */
static void clip_buttons(const struct lm_rect *card, struct lm_rect b[3])
{
    int y = card->y + (GADGET_TITLE_H - LM_CLIP_BTN) / 2;
    int x = card->x + card->w - 14 - LM_CLIP_BTN;
    for (int i = 2; i >= 0; i--, x -= LM_CLIP_BTN + LM_CLIP_BTN_GAP)
        b[i] = lm_rect_make(x, y, LM_CLIP_BTN, LM_CLIP_BTN);
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

/* How many entries fit in the card now (0 when it is hidden) and the history's length. */
static int clip_slots(struct lm_wm *wm)
{
    struct lm_rect gr;
    if (!lm_wm_gadget_rect_id(wm, LM_GADGET_CLIPBOARD, &gr))
        return 0;
    int slots, entry_h;
    gadget_slots(gr.h, &slots, &entry_h);
    return slots;
}

/* The first entry the card shows: clip_top kept inside the history. */
static int clip_top(struct lm_wm *wm)
{
    int slots = clip_slots(wm), total = lm_seat_clip_count(wm->srv->seat);
    return LM_CLAMP(wm->clip_top, 0, LM_MAX(total - LM_MAX(slots, 1), 0));
}

/* Entries the keyboard can reach: the whole history (the card follows the cursor). */
static int clip_nav_count(struct lm_wm *wm)
{
    return clip_slots(wm) > 0 ? lm_seat_clip_count(wm->srv->seat) : 0;
}

/* Scroll so that entry `i` is drawn. */
static void clip_show(struct lm_wm *wm, int i)
{
    int slots = LM_MAX(clip_slots(wm), 1), top = clip_top(wm);
    if (i < top)
        top = i;
    else if (i >= top + slots)
        top = i - slots + 1;
    wm->clip_top = top;
}

static void clip_nav_set(struct lm_wm *wm, bool on)
{
    if (wm->clip_nav == on)
        return;
    wm->clip_nav = on;
    wm->clip_cur = on ? clip_top(wm) : 0;
    damage_gadget(wm, LM_GADGET_CLIPBOARD);
}

void lm_wm_clipboard_changed(struct lm_wm *wm)
{
    int total = lm_seat_clip_count(wm->srv->seat);
    if (total > wm->clip_total)                                 /* something new was copied: the card shows the newest again */
        wm->clip_top = 0;
    wm->clip_total = total;
    wm->clip_top = clip_top(wm);
    if (wm->clip_nav) {                                         /* the list moved under the cursor: keep it inside */
        if (total == 0 || clip_slots(wm) == 0) {
            wm->clip_nav = false;
        } else {
            wm->clip_cur = LM_CLAMP(wm->clip_cur, 0, total - 1);
            clip_show(wm, wm->clip_cur);
        }
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

static void dock_damage(struct lm_wm *wm);
static void about_set(struct lm_wm *wm, bool open, struct lm_output *o);
static void about_damage(struct lm_wm *wm);
static void launcher_set(struct lm_wm *wm, bool open, struct lm_output *o);
static void dock_activate(struct lm_wm *wm, int i, bool reuse);
static void dock_launch(struct lm_wm *wm, const struct lm_dock_item *it, int i, bool reuse);
static void dock_hide_tick(void *data);
static void dock_anim_tick(void *data);

static void damage_panels(struct lm_wm *wm)
{
    if (!wm->srv->backend)
        return;
    struct lm_output *o;
    lm_list_for_each(o, &wm->srv->backend->outputs, link) {
        struct lm_rect r = output_panel_rect(o);
        r.h += LM_PANEL_SHADOW;                         /* (the shadow below it) */
        damage_rect(wm, &r);
    }
    dock_damage(wm);                                    /* (windows come and go here: the dock's "running" dots) */
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

static bool osk_geo(const struct lm_wm *wm, struct lm_osk_geo *g);

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
    struct lm_osk_geo kb;
    if (wm->srv->config.osk_avoid && wm->osk_open && o == lm_output_primary(wm->srv) && osk_geo(wm, &kb)) {
        /* the on-screen keyboard takes the strip it sits in: from the bottom when it is in the lower half, from the top otherwise
         * (not when what is left would be too small to work in) */
        if (kb.card.y + kb.card.h / 2 >= o->y + o->height / 2) {
            int h = kb.card.y - r.y;
            if (h >= 160)
                r.h = h;
        } else {
            int y = kb.card.y + kb.card.h, h = r.y + r.h - y;
            if (h >= 160 && y > r.y) {
                r.y = y;
                r.h = h;
            }
        }
    }
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
    if ((w->states & (LM_WINDOW_FULLSCREEN | LM_WINDOW_MAXIMIZED)) && !lm_output_primary(wm->srv)) {
        /* no output at all (a KVM switch looked at another computer, a cable was pulled): keep the size, it is fitted when one comes back */
        send_configure(wm, w, w->width, w->height);
        return;
    }
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
    if (old && win_is_calendar(old) && old->mapped && !old->minimized)
        lm_wm_window_request_minimize(old);                 /* the calendar goes away when something else is clicked */
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
    char buf[40];
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    lm_clock_bar_text(&wm->srv->config.clock, &tm, buf, sizeof(buf));
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

void lm_wm_reset_cascade(struct lm_wm *wm)
{
    if (wm)
        wm->cascade = 0;
}

void lm_wm_clock_changed(struct lm_wm *wm)
{
    if (!wm)
        return;
    update_clock(wm, false);
    damage_panels(wm);
}

static void refit_idle(void *data)
{
    lm_wm_output_changed(data, NULL);
}

struct lm_wm *lm_wm_create(struct lm_server *srv)
{
    gadget_def_init();
    struct lm_wm *wm = calloc(1, sizeof(*wm));
    if (!wm)
        return NULL;
    wm->srv = srv;
    lm_list_init(&wm->windows);
    lm_list_init(&wm->popups);
    wm->hover_panel_ws = -1;
    wm->osk_hover = wm->osk_down = -1;
    wm->hover_dock = -1;
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
    wm->dock_timer = lm_loop_add_timer(srv->loop, dock_hide_tick, wm);
    wm->dock_anim = lm_loop_add_timer(srv->loop, dock_anim_tick, wm);
    wm->clock_timer = lm_loop_add_timer(srv->loop, clock_tick, wm);
    wm->refit = lm_loop_add_idle(srv->loop, refit_idle, wm);
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
    if (wm->dock_timer)
        lm_timer_destroy(wm->dock_timer);
    if (wm->dock_anim)
        lm_timer_destroy(wm->dock_anim);
    if (wm->clock_timer)
        lm_timer_destroy(wm->clock_timer);
    if (wm->refit)
        lm_idle_destroy(wm->refit);
    free(wm);
}

void lm_wm_output_added(struct lm_wm *wm, struct lm_output *out)
{
    lm_wm_output_changed(wm, out);                      /* a monitor that comes back (KVM, cable) gets its fullscreen / maximized windows at its size */
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

void lm_wm_config_changed(struct lm_wm *wm)
{
    gadget_def_init();
    wm->dock_active = wm->dock_shown = false;
    wm->dock_drag = false;
    wm->hover_dock = -1;
    memset(wm->dock_launch_ms, 0, sizeof(wm->dock_launch_ms));         /* (the buttons may be others now) */
    lm_wm_output_changed(wm, NULL);
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
    if (wm->refit)
        lm_idle_schedule(wm->refit);                    /* (the output is unlinked after this call: refit then) */
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
    bool resized = w->mapped && (w->width != ng.w || w->height != ng.h);
    w->geometry = ng;
    w->width = ng.w;
    w->height = ng.h;
    if (wm->grab_kind == GRAB_RESIZE && wm->grab_win == w) {
        if (wm->grab_edges & LM_EDGE_LEFT)
            w->x = wm->grab_x + wm->grab_w - w->width;
        if (wm->grab_edges & LM_EDGE_TOP)
            w->y = wm->grab_y + wm->grab_h - w->height;
    } else if (resized && !(w->states & (LM_WINDOW_MAXIMIZED | LM_WINDOW_FULLSCREEN)) && !w->parent) {
        /* A floating window that made itself bigger (a terminal zooming its font) must not stick out of the screen:
         * it moves back inside, and when it is bigger than the usable area it is asked to shrink to fit. */
        struct lm_rect ua;
        lm_wm_usable_area(wm, w->x + w->width / 2, w->y + w->height / 2, &ua);
        int32_t fw = LM_MIN(w->width, ua.w - 2 * LM_BORDER_WIDTH);
        int32_t fh = LM_MIN(w->height, ua.h - LM_TITLEBAR_HEIGHT - LM_BORDER_WIDTH);
        if (w->x + w->width + LM_BORDER_WIDTH > ua.x + ua.w)
            w->x = ua.x + ua.w - LM_MIN(w->width, fw) - LM_BORDER_WIDTH;
        if (w->y + w->height + LM_BORDER_WIDTH > ua.y + ua.h)
            w->y = ua.y + ua.h - LM_MIN(w->height, fh) - LM_BORDER_WIDTH;
        if (w->x < ua.x + LM_BORDER_WIDTH)
            w->x = ua.x + LM_BORDER_WIDTH;
        if (w->y < ua.y + LM_TITLEBAR_HEIGHT)
            w->y = ua.y + LM_TITLEBAR_HEIGHT;
        if (fw != w->width || fh != w->height) {
            clamp_size(w, &fw, &fh);
            send_configure(wm, w, fw, fh);
        }
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
    if (win_is_calendar(w)) {                                /* it starts hidden; a click on the clock (maybe the one that started it) shows it */
        w->mapped = true;
        w->workspace = wm->workspace;
        w->minimized = true;
        damage_panels(wm);
        if (wm->cal_pending && lm_now_ms() - wm->cal_pending_ms < 5000)
            lm_wm_calendar_toggle(wm, &wm->cal_clock);
        wm->cal_pending = false;
        return;
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
        about_set(wm, false, NULL);
        launcher_set(wm, false, NULL);
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


/* ---- the on-screen keyboard ------------------------------------------------------------------------------------------------- */

static bool osk_phys_down(const struct lm_wm *wm, uint32_t code)
{
    return code < 24 * 32 && ((wm->osk_phys[code / 32] >> (code % 32)) & 1u);
}

/* Puts floating window `w` at a geometry (the client is asked for the size when it differs) and repaints both places. */
static void osk_place(struct lm_wm *wm, struct lm_window *w, int32_t x, int32_t y, int32_t cw, int32_t ch)
{
    struct lm_rect before = frame_outer(w);
    w->x = x;
    w->y = y;
    if (cw != w->width || ch != w->height) {
        clamp_size(w, &cw, &ch);
        send_configure(wm, w, cw, ch);
    }
    damage_rect(wm, &before);
    damage_frame(wm, w);
    update_outputs(wm, w);
}

void lm_wm_osk_layout_changed(struct lm_wm *wm)
{
    bool avoid = wm->srv->config.osk_avoid && wm->osk_open;
    struct lm_window *w;
    lm_list_for_each(w, &wm->windows, link) {
        struct wm_win *ww = WIN(w);
        if (w->states & (LM_WINDOW_MAXIMIZED | LM_WINDOW_FULLSCREEN)) {
            ww->osk_moved = false;
            if (w->states & LM_WINDOW_MAXIMIZED)
                apply_state(wm, w);                              /* the usable area is another one now */
            continue;
        }
        if (!w->mapped || w->parent)
            continue;
        int32_t bx = w->x, by = w->y, bw = w->width, bh = w->height;        /* the geometry the user gave it */
        if (ww->osk_moved) {
            bool same_pos = w->x == ww->osk_ax && w->y == ww->osk_ay;
            bool same_size = (w->width == ww->osk_aw && w->height == ww->osk_ah) || (w->width == ww->osk_ow && w->height == ww->osk_oh);
            if (same_pos && same_size) {
                bx = ww->osk_ox;
                by = ww->osk_oy;
                bw = ww->osk_ow;
                bh = ww->osk_oh;
            } else {
                ww->osk_moved = false;                           /* it was moved or resized since: that is the new original */
            }
        }
        int32_t nx = bx, ny = by, nw = bw, nh = bh;
        if (avoid) {
            struct lm_rect ua;
            lm_wm_usable_area(wm, bx + bw / 2, by + bh / 2, &ua);
            int32_t bottom = ua.y + ua.h - LM_BORDER_WIDTH, top = ua.y + LM_TITLEBAR_HEIGHT;
            if (by + bh > bottom) {                              /* its lower edge is under the keyboard (or the screen's edge): up, and smaller if it must */
                ny = LM_MAX(bottom - bh, top);
                if (ny + bh > bottom)
                    nh = bottom - ny;
            } else if (by < top) {                               /* the keyboard is at the top: down */
                ny = top;
                if (ny + bh > bottom)
                    nh = bottom - ny;
            }
            if (nh < bh)
                clamp_size(w, &nw, &nh);
        }
        bool differs = nx != bx || ny != by || nw != bw || nh != bh;
        if (differs && !ww->osk_moved) {
            ww->osk_moved = true;
            ww->osk_ox = bx;
            ww->osk_oy = by;
            ww->osk_ow = bw;
            ww->osk_oh = bh;
        }
        if (differs) {
            ww->osk_ax = nx;
            ww->osk_ay = ny;
            ww->osk_aw = nw;
            ww->osk_ah = nh;
            osk_place(wm, w, nx, ny, nw, nh);
        } else if (ww->osk_moved) {                              /* nothing is in its way any more: back where it was */
            ww->osk_moved = false;
            osk_place(wm, w, bx, by, bw, bh);
        }
    }
    damage_all(wm);
}

/* Is the on-screen key `k` lit by a key held on the real keyboard? A modifier key lights for either of its two keys. */
static bool osk_key_held(const struct lm_wm *wm, const struct lm_osk_key *k)
{
    switch (k->kind) {
    case LM_OSK_SHIFT: return osk_phys_down(wm, KEY_LEFTSHIFT) || osk_phys_down(wm, KEY_RIGHTSHIFT);
    case LM_OSK_CTRL: return osk_phys_down(wm, KEY_LEFTCTRL) || osk_phys_down(wm, KEY_RIGHTCTRL);
    case LM_OSK_HIDE: return false;
    default: return k->code && osk_phys_down(wm, k->code);
    }
}

/* Gives the keymap lookups (lm_keymap_keysym) the configured layouts and the active group, for labels and the launcher form. */
static void keymap_sync(const struct lm_wm *wm)
{
    int layouts[LM_KBD_MAX_LAYOUTS], n = wm->srv->config.kbd_nlayouts;
    if (n < 1 || n > LM_KBD_MAX_LAYOUTS)
        n = 0;
    for (int i = 0; i < n; i++)
        layouts[i] = wm->srv->config.kbd_layouts[i];
    lm_keymap_set_groups(layouts, n);
    if (wm->srv->seat)
        lm_keymap_client_group(lm_seat_group(wm->srv->seat));
}

static bool osk_geo(const struct lm_wm *wm, struct lm_osk_geo *g)
{
    if (!wm->osk_open || !wm->srv->backend)
        return false;
    struct lm_output *o = lm_output_primary(wm->srv);
    if (!o)
        return false;
    struct lm_rect r = lm_output_rect(o);
    lm_osk_geometry(&r, &wm->osk_pl, g);
    return true;
}

static void osk_damage(struct lm_wm *wm)
{
    struct lm_osk_geo g;
    if (osk_geo(wm, &g))
        damage_rect(wm, &g.card);
    damage_panels(wm);
}

static void osk_info(const struct lm_wm *wm, const struct lm_osk_geo *g, struct lm_osk_info *oi)
{
    memset(oi, 0, sizeof(*oi));
    oi->rect = g->card;
    oi->n = g->n;
    keymap_sync(wm);
    uint32_t locks = wm->srv->input ? lm_input_locks(wm->srv->input) : 0;
    uint32_t label_mods = wm->osk_latched;                      /* the labels show what the next key would give: also with a real Shift / AltGr held */
    if (osk_phys_down(wm, KEY_LEFTSHIFT) || osk_phys_down(wm, KEY_RIGHTSHIFT))
        label_mods |= LM_MOD_SHIFT;
    if (osk_phys_down(wm, KEY_RIGHTALT) && wm->srv->config.kbd_nlayouts > 1)
        label_mods |= LM_MOD_ALTGR;
    for (int i = 0; i < g->n; i++) {
        const struct lm_osk_key *k = &g->key[i];
        oi->key[i].rect = k->rect;
        oi->key[i].kind = (int)k->kind;
        oi->key[i].row = k->row;
        oi->key[i].code = k->code;
        lm_osk_label(k, label_mods, locks, oi->key[i].label);
        uint32_t st = 0;
        if (i == wm->osk_hover)
            st |= LM_OSK_KEY_HOVER;
        if (i == wm->osk_down || osk_key_held(wm, k))
            st |= LM_OSK_KEY_DOWN;
        if ((lm_osk_kind_mod(k->kind) & wm->osk_latched) || (k->kind == LM_OSK_CAPS && (locks & LM_MOD_CAPS)))
            st |= LM_OSK_KEY_ON;
        oi->key[i].state = st;
    }
}

static void osk_inject(struct lm_wm *wm, uint32_t code, bool pressed)
{
    if (!wm->srv->input || !code)
        return;
    struct lm_input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = LM_INPUT_KEY;
    ev.u.key.code = code;
    ev.u.key.pressed = pressed;
    lm_input_inject(wm->srv->input, &ev);
}

static const enum lm_osk_kind osk_mod_kinds[4] = { LM_OSK_SHIFT, LM_OSK_CTRL, LM_OSK_ALT, LM_OSK_ALTGR };

/* The held key comes up (and the modifiers that were pressed with it): the pointer let go, or the keyboard closes. */
static void osk_release(struct lm_wm *wm)
{
    if (wm->osk_down < 0)
        return;
    wm->osk_down = -1;
    osk_inject(wm, wm->osk_down_code, false);
    for (int i = 3; i >= 0; i--)
        if (wm->osk_down_mods & lm_osk_kind_mod(osk_mod_kinds[i]))
            osk_inject(wm, lm_osk_kind_code(osk_mod_kinds[i]), false);
    wm->osk_down_code = wm->osk_down_mods = 0;
    osk_damage(wm);
}

static void osk_set(struct lm_wm *wm, bool open)
{
    if (open == wm->osk_open)
        return;
    if (open) {
        wm->osk_open = true;
        wm->osk_latched = 0;
        wm->osk_hover = wm->osk_down = -1;
    } else {
        osk_release(wm);
        osk_damage(wm);                                         /* (while the geometry is still known) */
        wm->osk_open = false;
        wm->osk_latched = 0;
        wm->hover_osk = false;
        wm->osk_hover = -1;
        wm->osk_drag = 0;
    }
    osk_damage(wm);
    lm_wm_osk_layout_changed(wm);                               /* the windows make room for it / come back */
}

/* A press on key `i`: modifiers latch for the next key, Hide closes, anything else is typed (pressed now, released with the pointer). */
static void osk_press(struct lm_wm *wm, int i)
{
    struct lm_osk_geo g;
    if (!osk_geo(wm, &g) || i < 0 || i >= g.n)
        return;
    const struct lm_osk_key *k = &g.key[i];
    uint32_t mod = lm_osk_kind_mod(k->kind);
    if (k->kind == LM_OSK_HIDE) {
        osk_set(wm, false);
    } else if (mod) {
        wm->osk_latched ^= mod;
        osk_damage(wm);
    } else {
        osk_release(wm);
        wm->osk_down = i;
        wm->osk_down_code = k->code;
        wm->osk_down_mods = wm->osk_latched;
        wm->osk_latched = 0;
        for (int m = 0; m < 4; m++)
            if (wm->osk_down_mods & lm_osk_kind_mod(osk_mod_kinds[m]))
                osk_inject(wm, lm_osk_kind_code(osk_mod_kinds[m]), true);
        osk_inject(wm, k->code, true);
        osk_damage(wm);
    }
}

static bool osk_pick(const struct lm_wm *wm, int ix, int iy, struct lm_pick *out)
{
    struct lm_osk_geo g;
    if (lm_lock_active(wm->srv->lock) || !osk_geo(wm, &g) || !lm_rect_contains(&g.card, ix, iy))
        return false;
    out->kind = LM_PICK_OSK;
    out->rect = g.card;
    int m = lm_osk_margin();
    uint32_t edges = 0;                                         /* the right and the bottom margin resize (both: the corner) */
    if (ix >= g.card.x + g.card.w - m)
        edges |= LM_EDGE_RIGHT;
    if (iy >= g.card.y + g.card.h - m)
        edges |= LM_EDGE_BOTTOM;
    if (edges) {
        out->kind = LM_PICK_OSK_GRIP;
        out->edges = edges;
        return true;
    }
    for (int i = 0; i < g.n; i++)
        if (lm_rect_contains(&g.key[i].rect, ix, iy)) {
            out->kind = LM_PICK_OSK_KEY;
            out->index = i;
        }
    return true;
}

/* Dragging the keyboard: its margin moves it, its right / bottom margin resizes it (the top left corner stays where it is). The place is
 * pinned at the first press, so from then on it stays where the user put it. */
static void osk_drag_begin(struct lm_wm *wm, int kind, uint32_t edges)
{
    struct lm_osk_geo g;
    if (!osk_geo(wm, &g) || !wm->srv->seat)
        return;
    osk_release(wm);
    wm->osk_start.x = g.card.x;
    wm->osk_start.y = g.card.y;
    wm->osk_start.w = g.card.w;
    wm->osk_start.kh = g.kh;
    wm->osk_pl = wm->osk_start;
    wm->osk_drag = kind;
    wm->osk_drag_edges = edges;
    double px, py;
    lm_seat_pointer_position(wm->srv->seat, &px, &py);
    wm->osk_press_x = (int)px;
    wm->osk_press_y = (int)py;
}

static void osk_drag_motion(struct lm_wm *wm)
{
    if (!wm->osk_drag || !wm->srv->seat || !wm->osk_open)
        return;
    double px, py;
    lm_seat_pointer_position(wm->srv->seat, &px, &py);
    int dx = (int)px - wm->osk_press_x, dy = (int)py - wm->osk_press_y;
    struct lm_osk_place p = wm->osk_start;
    if (wm->osk_drag == 1) {
        p.x += dx;
        p.y += dy;
    } else {
        if (wm->osk_drag_edges & LM_EDGE_RIGHT)
            p.w += dx;
        if (wm->osk_drag_edges & LM_EDGE_BOTTOM)
            p.kh = lm_osk_kh_for_h(lm_osk_card_h(wm->osk_start.kh) + dy);
    }
    osk_damage(wm);                                             /* the old place ... */
    wm->osk_pl = p;
    struct lm_osk_geo g;
    if (osk_geo(wm, &g)) {                                      /* ... the new one, kept inside the output and within the size limits */
        wm->osk_pl.x = g.card.x;
        wm->osk_pl.y = g.card.y;
        wm->osk_pl.w = g.card.w;
        wm->osk_pl.kh = g.kh;
    }
    osk_damage(wm);
}

void lm_wm_osk_reset(struct lm_wm *wm)
{
    osk_damage(wm);
    memset(&wm->osk_pl, 0, sizeof(wm->osk_pl));
    wm->osk_drag = 0;
    osk_damage(wm);
    gadgets_save(wm);
    lm_wm_osk_layout_changed(wm);
}

void lm_wm_osk_set(struct lm_wm *wm, bool open) { osk_set(wm, open); }
bool lm_wm_osk_is_open(const struct lm_wm *wm) { return wm->osk_open; }

void lm_wm_tray_changed(struct lm_wm *wm)
{
    damage_panels(wm);
}

size_t lm_wm_tray_describe(struct lm_wm *wm, char *buf, size_t size)
{
    struct lm_output *o = lm_output_primary(wm->srv);
    struct lm_panel_info pi;
    size_t n = 0;
    if (!o || size == 0) {
        if (size)
            buf[0] = 0;
        return 0;
    }
    panel_info(wm, o, &pi);
    n += (size_t)snprintf(buf + n, size - n, "tray n=%d\n", pi.ntray);
    for (int i = 0; i < pi.ntray && n < size; i++) {
        char ic[24] = "-";
        if (pi.tray[i].icon)
            snprintf(ic, sizeof(ic), "%dx%d", pi.tray[i].icon->width, pi.tray[i].icon->height);
        n += (size_t)snprintf(buf + n, size - n, "item %d x=%d y=%d w=%d h=%d icon=%s text=%s\n", pi.tray[i].id, pi.tray[i].rect.x,
                              pi.tray[i].rect.y, pi.tray[i].rect.w, pi.tray[i].rect.h, ic, pi.tray[i].text);
    }
    return n < size ? n : size - 1;
}

size_t lm_wm_clock_describe(struct lm_wm *wm, char *buf, size_t size)
{
    struct lm_output *o = lm_output_primary(wm->srv);
    if (!o || size == 0) {
        if (size)
            buf[0] = 0;
        return 0;
    }
    struct lm_panel_info pi;
    panel_info(wm, o, &pi);
    struct lm_window *cw = calendar_window(wm);
    size_t n = (size_t)snprintf(buf, size, "clock x=%d y=%d w=%d h=%d tasks=%d calendar=%d text=%s\n", pi.clock.x, pi.clock.y, pi.clock.w, pi.clock.h,
                                pi.ntasks, !cw ? 0 : win_visible(wm, cw) ? 2 : 1, pi.clock_text);
    return n < size ? n : size - 1;
}

size_t lm_wm_osk_describe(struct lm_wm *wm, char *buf, size_t size)
{
    struct lm_osk_geo g;
    bool open = osk_geo(wm, &g);
    size_t n = (size_t)snprintf(buf, size, "osk open=%d latched=%u n=%d down=%d\n", open, wm->osk_latched, open ? g.n : 0, wm->osk_down);
    if (!open)
        return n;
    static struct lm_osk_info oi;
    osk_info(wm, &g, &oi);
    n += (size_t)snprintf(buf + n, size - n, "card x=%d y=%d w=%d h=%d\n", g.card.x, g.card.y, g.card.w, g.card.h);
    for (int i = 0; i < g.n && n < size; i++)
        n += (size_t)snprintf(buf + n, size - n, "key %d code=%u kind=%d x=%d y=%d w=%d h=%d on=%d held=%d label=%s\n", i, g.key[i].code,
                              (int)g.key[i].kind, g.key[i].rect.x, g.key[i].rect.y, g.key[i].rect.w, g.key[i].rect.h,
                              (oi.key[i].state & LM_OSK_KEY_ON) != 0, (oi.key[i].state & LM_OSK_KEY_DOWN) != 0, oi.key[i].label);
    return n;
}

/* ---- the About dialog ----------------------------------------------------------------------------------------------------- */

static struct lm_output *about_output(const struct lm_wm *wm)
{
    if (!wm->about_open || !wm->srv->backend)
        return NULL;
    struct lm_output *o;
    lm_list_for_each(o, &wm->srv->backend->outputs, link)
        if (!strcmp(o->name, wm->about_out))
            return o;
    return NULL;
}

static void about_info(const struct lm_wm *wm, struct lm_output *o, struct lm_about_info *ai)
{
    memset(ai, 0, sizeof(*ai));
    ai->rect = lm_output_rect(o);
    int h = 20 + 96 + 12 + LM_FONT_H + 6 + LM_FONT_H + 16 + wm->about_n * ABOUT_ROW_H + 18 + 26 + 18;
    h = LM_MIN(h, ai->rect.h - 8);
    ai->card = lm_rect_make(ai->rect.x + (ai->rect.w - ABOUT_CARD_W) / 2, ai->rect.y + (ai->rect.h - h) / 2, ABOUT_CARD_W, h);
    ai->logo = lm_rect_make(ai->card.x + (ABOUT_CARD_W - 96) / 2, ai->card.y + 20, 96, 96);
    ai->close = lm_rect_make(ai->card.x + (ABOUT_CARD_W - 90) / 2, ai->card.y + ai->card.h - 18 - 26, 90, 26);
    ai->hover_close = wm->about_hover_close;
    ai->n = wm->about_n;
    ai->row = wm->about_rows;
}

static void about_damage(struct lm_wm *wm)
{
    struct lm_output *o = about_output(wm);
    if (o) {
        struct lm_rect r = lm_output_rect(o);
        damage_rect(wm, &r);
    }
    damage_panels(wm);
}

/* Opens the dialog on `o` (the output whose logo button was used; NULL = the primary one) or closes it. */
static void about_set(struct lm_wm *wm, bool open, struct lm_output *o)
{
    if (open == wm->about_open)
        return;
    if (open) {
        if (!o)
            o = lm_output_primary(wm->srv);
        if (!o)
            return;
        power_set(wm, false, NULL);
        launcher_set(wm, false, NULL);
        sw_close(wm);
        clip_nav_set(wm, false);
        wm->about_n = lm_about_collect(wm->srv, wm->about_rows, LM_ABOUT_ROWS);
        snprintf(wm->about_out, sizeof(wm->about_out), "%s", o->name);
        wm->about_open = true;
        wm->about_hover_close = false;
        about_damage(wm);
    } else {
        about_damage(wm);                                       /* (while the output is still found) */
        wm->about_open = false;
        wm->about_hover_close = false;
        damage_panels(wm);
    }
}

void lm_wm_about_set(struct lm_wm *wm, bool open)
{
    about_set(wm, open, NULL);
}

bool lm_wm_about_is_open(const struct lm_wm *wm)
{
    return wm->about_open;
}

size_t lm_wm_about_describe(struct lm_wm *wm, char *buf, size_t size)
{
    struct lm_output *o = about_output(wm);
    size_t n = 0;
    buf[0] = 0;
    if (!o)
        return 0;
    struct lm_about_info ai;
    about_info(wm, o, &ai);
    n += (size_t)snprintf(buf + n, size - n, "card x=%d y=%d w=%d h=%d\nclose x=%d y=%d w=%d h=%d\n", ai.card.x, ai.card.y, ai.card.w, ai.card.h,
                          ai.close.x, ai.close.y, ai.close.w, ai.close.h);
    for (int i = 0; i < ai.n && n < size; i++)
        n += (size_t)snprintf(buf + n, size - n, "row %s|%s\n", ai.row[i].label, ai.row[i].value);
    return n < size ? n : size - 1;
}


static int dock_window_count(const struct lm_wm *wm, const struct lm_dock_item *it);

/* ---- the launcher dialog -------------------------------------------------------------------------------------------------- */

#define LC_CELL_W LM_LAUNCHER_CELL_W
#define LC_CELL_H 116
#define LC_ICON LM_LAUNCHER_ICON
#define LC_PAD 28
#define LC_HEAD 56
#define LC_FOOT 84
#define LC_BTN_W 110
#define LC_BTN_H 28
#define LC_KIND 36                  /* the icon choices of the form */
#define LC_FORM_W 580
#define LC_FORM_H 440

/* The buttons the launcher shows: the dock's (all but the one that opens the launcher itself) and then the launcher's own ([launcher-apps]). */
static bool launcher_is_self(const struct lm_dock_item *it)
{
    return !strcmp(it->cmd, "@launcher");
}

static int launcher_count(void)
{
    int n = 0;
    for (int i = 0; i < lm_dock_count(); i++)
        if (!launcher_is_self(lm_dock_item(i)))
            n++;
    for (int i = 0; i < lm_launchapps_count(); i++)
        if (!launcher_is_self(lm_launchapps_item(i)))
            n++;
    return n;
}

/* The g-th button of the launcher; *dock_index is its index in the dock, -1 for one that is only in the launcher. NULL when there is none. */
static const struct lm_dock_item *launcher_item(int g, int *dock_index)
{
    if (dock_index)
        *dock_index = -1;
    for (int i = 0; i < lm_dock_count(); i++)
        if (!launcher_is_self(lm_dock_item(i)) && g-- == 0) {
            if (dock_index)
                *dock_index = i;
            return lm_dock_item(i);
        }
    for (int i = 0; i < lm_launchapps_count(); i++)
        if (!launcher_is_self(lm_launchapps_item(i)) && g-- == 0)
            return lm_launchapps_item(i);
    return NULL;
}

static bool launcher_name_taken(const char *name)
{
    const char *nm, *ic, *cm;
    for (int i = 0; lm_dock_raw_entry(i, &nm, &ic, &cm); i++)
        if (!strcasecmp(nm, name))
            return true;
    for (int i = 0; lm_launchapps_raw_entry(i, &nm, &ic, &cm); i++)
        if (!strcasecmp(nm, name))
            return true;
    return false;
}

static bool launcher_can_add(void)
{
    return lm_launchapps_raw_count() < LM_DOCK_MAX;
}

static bool launcher_drop_zone(const struct lm_wm *wm, struct lm_output *o, struct lm_rect *zone, struct lm_rect *marker, int *pos);

static struct lm_output *launcher_output(const struct lm_wm *wm)
{
    if (!wm->launcher_open || !wm->srv->backend)
        return NULL;
    struct lm_output *o;
    lm_list_for_each(o, &wm->srv->backend->outputs, link)
        if (!strcmp(o->name, wm->launcher_out))
            return o;
    return NULL;
}

/* The built-in icon the form's icon text names: -1 when it is empty, -2 when it is something else (a theme name, a file). */
static int launcher_kind_of(const struct lm_wm *wm)
{
    if (!wm->launcher_icon[0])
        return -1;
    int k = lm_dockicon_by_name(wm->launcher_icon);
    return k >= 0 ? k : -2;
}

static void launcher_info(const struct lm_wm *wm, struct lm_output *o, struct lm_launcher_info *li)
{
    memset(li, 0, sizeof(*li));
    li->rect = lm_output_rect(o);
    li->form = wm->launcher_form;
    li->editing = wm->launcher_form && wm->launcher_editing;
    li->edit = wm->launcher_edit && !li->form;
    li->hover_button = wm->launcher_hover_btn;
    li->message = wm->launcher_msg;
    li->focus = wm->launcher_focus;
    li->kind = launcher_kind_of(wm);
    li->kind_name = li->kind >= 0 ? lm_dockicon_name(li->kind) : li->kind == -2 ? "yours" : "auto";
    int n = launcher_count(), cells = n + (launcher_can_add() ? 1 : 0);
    li->n = n;
    li->sel = LM_CLAMP(wm->launcher_sel, -1, cells - 1);
    int cw, ch;
    if (li->form) {
        cw = LM_MIN(LC_FORM_W, li->rect.w - 20);
        ch = LC_FORM_H;
    } else {
        int maxcols = LM_CLAMP((li->rect.w - 60) / LC_CELL_W, 1, 6);
        li->cols = LM_MIN(maxcols, LM_MAX(cells, 1));
        int rows = LM_MAX((cells + li->cols - 1) / li->cols, 1);
        int cell_h = LC_CELL_H;
        if (LC_HEAD + rows * cell_h + LC_FOOT > li->rect.h - 20)       /* a low screen: the rows get closer */
            cell_h = LM_MAX((li->rect.h - 20 - LC_HEAD - LC_FOOT) / rows, LC_ICON + 8 + LM_FONT_H + 6);
        cw = LM_MAX(li->cols * LC_CELL_W + 2 * LC_PAD, 2 * LC_BTN_W + 56);
        ch = LC_HEAD + rows * cell_h + LC_FOOT;
        int x0 = li->rect.x + (li->rect.w - cw) / 2 + (cw - li->cols * LC_CELL_W) / 2;
        int y0 = li->rect.y + (li->rect.h - ch) / 2 + LC_HEAD;
        for (int g = 0; g < cells; g++) {
            struct lm_rect cell = lm_rect_make(x0 + (g % li->cols) * LC_CELL_W, y0 + (g / li->cols) * cell_h, LC_CELL_W, cell_h);
            struct lm_rect box = lm_rect_make(cell.x + (LC_CELL_W - LC_ICON) / 2, cell.y + 6, LC_ICON, LC_ICON);
            if (g == n) {
                li->add = cell;
                continue;
            }
            li->icon[g].box = box;
            li->icon[g].hit = cell;
            li->icon[g].remove = lm_rect_make(box.x - 8, box.y - 8, 24, 24);
            li->icon[g].item = launcher_item(g, NULL);
            li->icon[g].running = dock_window_count(wm, li->icon[g].item) > 0;
        }
    }
    li->card = lm_rect_make(li->rect.x + (li->rect.w - cw) / 2, li->rect.y + (li->rect.h - ch) / 2, cw, ch);
    li->drag = -1;
    li->drop_pos = -1;
    if (wm->launcher_drag && !li->form && wm->launcher_drag_g < n) {
        li->drag = wm->launcher_drag_g;
        li->drag_box = lm_rect_make((int)wm->drag_x - 28, (int)wm->drag_y - 28, 56, 56);
        launcher_drop_zone(wm, o, &li->drop, &li->marker, &li->drop_pos);
    }
    li->title = lm_rect_make(li->card.x, li->card.y + 18, li->card.w, LM_FONT_H);
    int by = li->card.y + li->card.h - 16 - LC_BTN_H, bx = li->card.x + li->card.w / 2;
    li->button[0] = lm_rect_make(bx - LC_BTN_W - 8, by, LC_BTN_W, LC_BTN_H);
    li->button[1] = lm_rect_make(bx + 8, by, LC_BTN_W, LC_BTN_H);
    li->button_text[0] = li->form ? (li->editing ? "Save" : "Add") : li->edit ? "Done" : "Edit";
    li->button_text[1] = li->form ? "Cancel" : "Close";
    static const char hint_grid[] = "Enter starts   E edit   A add   Esc closes", hint_edit[] = "Click an icon to change it   red x removes   E or Done ends   Esc closes",
                      hint_form[] = "Tab next field   Left / Right icon   Enter adds   Esc back",
                      hint_change[] = "Tab next field   Left / Right icon   Enter saves   Esc back";
    li->hint = li->form ? (li->editing ? hint_change : hint_form) : li->edit ? hint_edit : hint_grid;
    if (li->form) {
        li->field_label[0] = "Name";
        li->field_label[1] = "Command   (@terminal, mellagui-*, ~/prog, a || b)";
        li->field_label[2] = "Icon   (empty = guessed, theme:firefox, a file, or pick below)";
        li->field_text[0] = wm->launcher_name;
        li->field_text[1] = wm->launcher_cmd;
        li->field_text[2] = wm->launcher_icon;
        int fx = li->card.x + LC_PAD, fw = li->card.w - 2 * LC_PAD;
        li->field[0] = lm_rect_make(fx, li->card.y + 60 + LM_FONT_H + 4, fw, 30);
        li->field[1] = lm_rect_make(fx, li->card.y + 126 + LM_FONT_H + 4, fw, 30);
        li->field[2] = lm_rect_make(fx, li->card.y + 192 + LM_FONT_H + 4, fw, 30);
        int total = LM_LAUNCHER_KIND_N * LC_KIND + (LM_LAUNCHER_KIND_N - 1) * 6;
        int kx = li->card.x + (li->card.w - total) / 2, ky = li->card.y + 286;
        for (int i = 0; i < LM_LAUNCHER_KIND_N; i++)
            li->kind_box[i] = lm_rect_make(kx + i * (LC_KIND + 6), ky, LC_KIND, LC_KIND);
    }
}

static void launcher_damage(struct lm_wm *wm)
{
    struct lm_output *o = launcher_output(wm);
    if (o) {
        struct lm_rect r = lm_output_rect(o);
        damage_rect(wm, &r);
    }
}

static void launcher_set(struct lm_wm *wm, bool open, struct lm_output *o)
{
    if (open == wm->launcher_open)
        return;
    if (open) {
        if (!o)
            o = lm_output_primary(wm->srv);
        if (!o)
            return;
        about_set(wm, false, NULL);
        power_set(wm, false, NULL);
        sw_close(wm);
        clip_nav_set(wm, false);
        snprintf(wm->launcher_out, sizeof(wm->launcher_out), "%s", o->name);
        wm->launcher_open = true;
        wm->launcher_edit = wm->launcher_form = false;
        wm->launcher_sel = -1;
        wm->launcher_hover_btn = -1;
        wm->launcher_msg[0] = 0;
        launcher_damage(wm);
    } else {
        launcher_damage(wm);
        wm->launcher_open = false;
        wm->launcher_edit = wm->launcher_form = false;
    }
}

void lm_wm_launcher_set(struct lm_wm *wm, bool open) { launcher_set(wm, open, NULL); }
bool lm_wm_launcher_is_open(const struct lm_wm *wm) { return wm->launcher_open; }

size_t lm_wm_launcher_describe(struct lm_wm *wm, char *buf, size_t size)
{
    struct lm_output *o = launcher_output(wm);
    size_t n = 0;
    struct lm_launcher_info li;
    if (o)
        launcher_info(wm, o, &li);
    n += (size_t)snprintf(buf, size, "launcher open=%d edit=%d form=%d n=%d sel=%d drag=%d droppos=%d editing=%d\n", o != NULL, o ? li.edit : 0,
                          o ? li.form : 0, o ? li.n : 0, o ? li.sel : -1, o ? li.drag : -1, o ? li.drop_pos : -1, o ? li.editing : 0);
    if (!o)
        return n;
#define LCR(r) (r).x, (r).y, (r).w, (r).h
    n += (size_t)snprintf(buf + n, size - n, "card x=%d y=%d w=%d h=%d\n", LCR(li.card));
    if (li.drag >= 0 && li.drop.w > 0)
        n += (size_t)snprintf(buf + n, size - n, "drop x=%d y=%d w=%d h=%d\n", LCR(li.drop));
    if (!li.form) {
        for (int g = 0; g < li.n && n < size; g++) {
            n += (size_t)snprintf(buf + n, size - n, "icon %d \"%s\" x=%d y=%d w=%d h=%d", g, li.icon[g].item->name, LCR(li.icon[g].box));
            if (li.edit)
                n += (size_t)snprintf(buf + n, size - n, " remove=%d,%d,%d,%d", LCR(li.icon[g].remove));
            n += (size_t)snprintf(buf + n, size - n, "\n");
        }
        if (li.add.w > 0)
            n += (size_t)snprintf(buf + n, size - n, "add x=%d y=%d w=%d h=%d\n", LCR(li.add));
    } else {
        for (int i = 0; i < 3; i++)
            n += (size_t)snprintf(buf + n, size - n, "field %d x=%d y=%d w=%d h=%d text=\"%s\"\n", i, LCR(li.field[i]), li.field_text[i]);
        for (int i = 0; i < LM_LAUNCHER_KIND_N; i++)
            n += (size_t)snprintf(buf + n, size - n, "kind %d x=%d y=%d w=%d h=%d\n", i - 1, LCR(li.kind_box[i]));
        n += (size_t)snprintf(buf + n, size - n, "focus %d kind %d message \"%s\"\n", li.focus, li.kind, li.message);
    }
    for (int i = 0; i < 2 && n < size; i++)
        n += (size_t)snprintf(buf + n, size - n, "button %d x=%d y=%d w=%d h=%d \"%s\"\n", i, LCR(li.button[i]), li.button_text[i]);
#undef LCR
    return n < size ? n : size - 1;
}

/* Where the pointer is in the dialog. */
static bool launcher_pick(const struct lm_wm *wm, int ix, int iy, struct lm_pick *out)
{
    struct lm_output *lo = launcher_output(wm);
    if (!lo || lm_lock_active(wm->srv->lock))
        return false;
    struct lm_launcher_info li;
    launcher_info(wm, lo, &li);
    out->rect = li.card;
    out->kind = lm_rect_contains(&li.card, ix, iy) ? LM_PICK_LAUNCHER_CARD : LM_PICK_LAUNCHER_BACKDROP;
    if (out->kind == LM_PICK_LAUNCHER_BACKDROP)
        return true;
    for (int i = 0; i < 2; i++)
        if (lm_rect_contains(&li.button[i], ix, iy)) {
            out->kind = LM_PICK_LAUNCHER_BUTTON;
            out->index = i;
            return true;
        }
    if (li.form) {
        for (int i = 0; i < 3; i++)
            if (lm_rect_contains(&li.field[i], ix, iy)) {
                out->kind = LM_PICK_LAUNCHER_FIELD;
                out->index = i;
                return true;
            }
        for (int i = 0; i < LM_LAUNCHER_KIND_N; i++)
            if (lm_rect_contains(&li.kind_box[i], ix, iy)) {
                out->kind = LM_PICK_LAUNCHER_KIND;
                out->index = i - 1;
                return true;
            }
        return true;
    }
    for (int g = 0; g < li.n; g++) {
        if (li.edit && lm_rect_contains(&li.icon[g].remove, ix, iy)) {
            out->kind = LM_PICK_LAUNCHER_REMOVE;
            out->index = g;
            return true;
        }
    }
    for (int g = 0; g < li.n; g++)
        if (lm_rect_contains(&li.icon[g].hit, ix, iy)) {
            out->kind = LM_PICK_LAUNCHER_ITEM;
            out->index = g;
            return true;
        }
    if (li.add.w > 0 && lm_rect_contains(&li.add, ix, iy))
        out->kind = LM_PICK_LAUNCHER_ADD;
    return true;
}

static void launcher_open_form(struct lm_wm *wm)
{
    if (!launcher_can_add())
        return;
    wm->launcher_form = true;
    wm->launcher_editing = false;
    wm->launcher_name[0] = wm->launcher_cmd[0] = wm->launcher_msg[0] = 0;
    wm->launcher_icon[0] = 0;
    wm->launcher_focus = 0;
    wm->launcher_hover_btn = -1;
    launcher_damage(wm);
}

/* The form again, filled in with the button g (edit mode: a click on its icon): name, command and icon as the file has them. */
static void launcher_open_change(struct lm_wm *wm, int g)
{
    const struct lm_dock_item *it = launcher_item(g, NULL);
    const char *nm, *ic, *cm;
    if (!it || !(it->launcher_only ? lm_launchapps_raw_entry(it->raw, &nm, &ic, &cm) : lm_dock_raw_entry(it->raw, &nm, &ic, &cm)))
        return;
    wm->launcher_form = true;
    wm->launcher_editing = true;
    wm->launcher_edit_raw = it->raw;
    wm->launcher_edit_only = it->launcher_only;
    snprintf(wm->launcher_name, sizeof(wm->launcher_name), "%s", nm);
    snprintf(wm->launcher_cmd, sizeof(wm->launcher_cmd), "%s", cm);
    wm->launcher_msg[0] = 0;
    snprintf(wm->launcher_icon, sizeof(wm->launcher_icon), "%s", ic);
    wm->launcher_focus = 0;
    wm->launcher_hover_btn = -1;
    launcher_damage(wm);
}

static void launcher_remove(struct lm_wm *wm, int g)
{
    const struct lm_dock_item *it = launcher_item(g, NULL);
    if (!it)
        return;
    if (!(it->launcher_only ? lm_settings_launcher_del(wm->srv, it->raw) : lm_settings_dock_del(wm->srv, it->raw)))
        lm_wm_notify(wm, "Launcher", "Could not remove it: there is no config file to write", 5000);
    wm->launcher_sel = LM_MIN(wm->launcher_sel, launcher_count() - 1);
    if (launcher_count() == 0)
        wm->launcher_edit = false;
    launcher_damage(wm);
}

static bool launcher_replace(struct lm_wm *wm, const char *name, const char *icon, const char *cmd)
{
    return wm->launcher_edit_only ? lm_settings_launcher_replace(wm->srv, wm->launcher_edit_raw, name, icon, cmd)
                                  : lm_settings_dock_replace(wm->srv, wm->launcher_edit_raw, name, icon, cmd);
}

static void launcher_submit(struct lm_wm *wm)
{
    char name[LM_DOCK_NAME_MAX], cmd[LM_DOCK_CMD_MAX];
    snprintf(name, sizeof(name), "%s", wm->launcher_name);
    snprintf(cmd, sizeof(cmd), "%s", wm->launcher_cmd);
    for (char *t = name; *t && (*t == ' ');)
        memmove(t, t + 1, strlen(t));
    for (size_t l = strlen(name); l > 0 && name[l - 1] == ' '; l--)
        name[l - 1] = 0;
    for (char *t = cmd; *t && (*t == ' ');)
        memmove(t, t + 1, strlen(t));
    for (size_t l = strlen(cmd); l > 0 && cmd[l - 1] == ' '; l--)
        cmd[l - 1] = 0;
    if (!name[0] || !cmd[0]) {
        snprintf(wm->launcher_msg, sizeof(wm->launcher_msg), "Give it a name and a command.");
        launcher_damage(wm);
        return;
    }
    char icon[LM_DOCK_ICON_MAX];                                     /* "[theme:firefox]" as the file shows it: the brackets are not part of it */
    snprintf(icon, sizeof(icon), "%s", wm->launcher_icon);
    for (char *t = icon; *t == ' ';)
        memmove(t, t + 1, strlen(t));
    for (size_t l = strlen(icon); l > 0 && icon[l - 1] == ' '; l--)
        icon[l - 1] = 0;
    size_t il = strlen(icon);
    if (il >= 2 && icon[0] == '[' && icon[il - 1] == ']') {
        memmove(icon, icon + 1, il - 2);
        icon[il - 2] = 0;
    }
    int before = launcher_count();
    if (wm->launcher_editing) {                                      /* change the button in place; its own name does not count as taken */
        bool ok = launcher_replace(wm, name, icon, cmd);
        if (!ok) {
            snprintf(wm->launcher_msg, sizeof(wm->launcher_msg), "Not saved: the name is in use or not allowed, or there is no config file.");
            launcher_damage(wm);
            return;
        }
        wm->launcher_form = false;
        wm->launcher_editing = false;
        wm->launcher_msg[0] = 0;
        wm->launcher_sel = LM_CLAMP(wm->launcher_sel, 0, LM_MAX(launcher_count() - 1, 0));
        if (launcher_count() < before)
            lm_wm_notify(wm, "Launcher", "Saved, but that program was not found, so it is left out for now", 6000);
        launcher_damage(wm);
        return;
    }
    if (launcher_name_taken(name) || !lm_settings_launcher_put(wm->srv, name, icon, cmd)) {
        snprintf(wm->launcher_msg, sizeof(wm->launcher_msg), "Not added: the name is in use or not allowed, the list is full, or there is no config file.");
        launcher_damage(wm);
        return;
    }
    wm->launcher_form = false;
    wm->launcher_msg[0] = 0;
    wm->launcher_sel = LM_MAX(launcher_count() - 1, 0);
    if (launcher_count() <= before)
        lm_wm_notify(wm, "Launcher", "Saved, but that program was not found, so it is left out for now", 6000);
    launcher_damage(wm);
}

/* A click (press and release on the same thing) or Enter on one of the dialog's targets. */
static void launcher_activate(struct lm_wm *wm, int kind, int index)
{
    switch (kind) {
    case LM_PICK_LAUNCHER_ITEM: {
        int di;
        const struct lm_dock_item *it = launcher_item(index, &di);
        if (!it)
            break;
        if (wm->launcher_edit) {                                      /* edit mode: change it instead of starting it */
            launcher_open_change(wm, index);
            break;
        }
        launcher_set(wm, false, NULL);
        dock_launch(wm, it, di, true);
        break;
    }
    case LM_PICK_LAUNCHER_REMOVE: launcher_remove(wm, index); break;
    case LM_PICK_LAUNCHER_ADD: launcher_open_form(wm); break;
    case LM_PICK_LAUNCHER_FIELD:
        wm->launcher_focus = index;
        launcher_damage(wm);
        break;
    case LM_PICK_LAUNCHER_KIND:                                       /* a built-in icon: its name goes into the icon text, "auto" empties it */
        snprintf(wm->launcher_icon, sizeof(wm->launcher_icon), "%s", index >= 0 ? lm_dockicon_name(index) : "");
        wm->launcher_focus = 3;
        launcher_damage(wm);
        break;
    case LM_PICK_LAUNCHER_BUTTON:
        if (wm->launcher_form) {
            if (index == 0)
                launcher_submit(wm);
            else {
                wm->launcher_form = false;
                launcher_damage(wm);
            }
        } else if (index == 0) {
            wm->launcher_edit = !wm->launcher_edit;
            launcher_damage(wm);
        } else {
            launcher_set(wm, false, NULL);
        }
        break;
    }
}

static void launcher_pop(char *text)
{
    size_t l = strlen(text);
    while (l > 0 && ((unsigned char)text[l - 1] & 0xc0) == 0x80)
        l--;
    if (l > 0)
        l--;
    text[l] = 0;
}

static void launcher_key(struct lm_wm *wm, uint32_t code, bool repeat, uint32_t mods)
{
    struct lm_output *lo = launcher_output(wm);
    if (!lo)
        return;
    struct lm_launcher_info li;
    launcher_info(wm, lo, &li);
    bool shift = (mods & LM_MOD_SHIFT) != 0;
    if (code == KEY_A && (mods & LM_MOD_SUPER) && !(mods & ~(uint32_t)LM_MOD_SUPER & LM_KEY_MODS_MASK)) {   /* Super+A again closes it */
        if (!repeat)
            launcher_set(wm, false, NULL);
        return;
    }
    bool arrow = code == KEY_LEFT || code == KEY_RIGHT || code == KEY_UP || code == KEY_DOWN;
    if (!wm->launcher_form) {
        int cells = li.n + (li.add.w > 0 ? 1 : 0);
        if (repeat && !arrow)
            return;
        if (code == KEY_ESC) {
            launcher_set(wm, false, NULL);
        } else if (arrow || code == KEY_HOME || code == KEY_END) {
            if (cells == 0)
                return;
            int cur = li.sel < 0 ? (code == KEY_LEFT || code == KEY_UP || code == KEY_END ? cells - 1 : 0) : li.sel;
            if (li.sel >= 0) {
                int cols = LM_MAX(li.cols, 1);
                cur = code == KEY_LEFT ? cur - 1 : code == KEY_RIGHT ? cur + 1 : code == KEY_UP ? cur - cols :
                      code == KEY_DOWN ? cur + cols : code == KEY_HOME ? 0 : cells - 1;
                if (code == KEY_DOWN && cur >= cells && li.sel < cells - 1)
                    cur = cells - 1;                                    /* the last, shorter row */
            }
            cur = LM_CLAMP(cur, 0, cells - 1);
            if (cur != wm->launcher_sel) {
                wm->launcher_sel = cur;
                launcher_damage(wm);
            }
        } else if (code == KEY_ENTER || code == KEY_KPENTER || code == KEY_SPACE) {
            if (li.sel >= 0 && li.sel < li.n)
                launcher_activate(wm, LM_PICK_LAUNCHER_ITEM, li.sel);
            else if (li.sel == li.n && li.add.w > 0)
                launcher_open_form(wm);
        } else if ((code == KEY_DELETE || code == KEY_BACKSPACE) && li.edit && li.sel >= 0 && li.sel < li.n) {
            launcher_remove(wm, li.sel);
        } else if (code == KEY_E && li.n > 0) {
            wm->launcher_edit = !wm->launcher_edit;
            launcher_damage(wm);
        } else if (code == KEY_A || code == KEY_N) {
            launcher_open_form(wm);
        }
        return;
    }
    /* the add form */
    if (code == KEY_ESC) {
        if (!repeat) {
            wm->launcher_form = false;
            launcher_damage(wm);
        }
        return;
    }
    if (code == KEY_TAB || ((code == KEY_UP || code == KEY_DOWN) && !repeat)) {
        if (repeat)
            return;
        bool back = code == KEY_UP || (code == KEY_TAB && shift);
        wm->launcher_focus = (wm->launcher_focus + (back ? 3 : 1)) % 4;
        launcher_damage(wm);
        return;
    }
    if (code == KEY_ENTER || code == KEY_KPENTER) {
        if (repeat)
            return;
        if (wm->launcher_focus == 0) {
            wm->launcher_focus = 1;
            launcher_damage(wm);
        } else {
            launcher_submit(wm);
        }
        return;
    }
    if (wm->launcher_focus == 3) {
        if (code == KEY_LEFT || code == KEY_RIGHT) {
            int k = LM_MAX(launcher_kind_of(wm), -1) + (code == KEY_LEFT ? -1 : 1);
            k = LM_CLAMP(k, -1, LM_DOCKICON_COUNT - 1);
            snprintf(wm->launcher_icon, sizeof(wm->launcher_icon), "%s", k >= 0 ? lm_dockicon_name(k) : "");
            launcher_damage(wm);
        }
        return;
    }
    char *text = wm->launcher_focus == 0 ? wm->launcher_name : wm->launcher_focus == 1 ? wm->launcher_cmd : wm->launcher_icon;
    size_t max = wm->launcher_focus == 0 ? sizeof(wm->launcher_name) : wm->launcher_focus == 1 ? sizeof(wm->launcher_cmd) : sizeof(wm->launcher_icon);
    if (code == KEY_BACKSPACE) {
        launcher_pop(text);
    } else if (!(mods & (LM_MOD_CTRL | LM_MOD_ALT | LM_MOD_SUPER))) {
        keymap_sync(wm);
        if (code >= KEY_KP7 && code <= KEY_KPDOT)
            mods |= LM_MOD_NUM;
        uint32_t ks = lm_keymap_keysym(code, mods);
        char txt[8];
        int k = ks ? lm_keysym_to_utf8(ks, txt) : 0;
        size_t l = strlen(text);
        if (k <= 0 || (unsigned char)txt[0] < 0x20 || l + (size_t)k >= max)
            return;
        memcpy(text + l, txt, (size_t)k);
        text[l + (size_t)k] = 0;
    } else {
        return;
    }
    wm->launcher_msg[0] = 0;
    launcher_damage(wm);
}

/* ---- the dock ------------------------------------------------------------------------------------------------------------ */

#define DOCK_DOT 8                  /* room under the icons for the "running" dots */
#define DOCK_PAD 12                 /* the strip reaches this far beyond the first / last icon */
#define DOCK_LABEL_H 22
#define DOCK_HIDE_MS 600            /* autohide: the dock goes away this long after the pointer left it */
#define DOCK_BOUNCE_PERIOD_MS 900   /* one swell (zoom in, zoom out) of an icon whose program is starting */
#define DOCK_PULSE 0.35f            /* ... up to this much bigger */
#define DOCK_ANIM_MS 33             /* ... redrawn this often while any of them swells */

struct dock_geo {
    int n, size, zsize, gap, x0, icon_bottom;
    struct lm_rect strip, tall, bounds;
};

/* The dock lives on the primary output; NULL when there is none or it is off / empty / hidden by a fullscreen window. */
static struct lm_output *dock_output(const struct lm_wm *wm)
{
    if (!wm->srv->backend || !lm_dock.enabled || lm_dock_count() == 0)
        return NULL;
    struct lm_output *o = lm_output_primary(wm->srv);
    return o && !fullscreen_covers(wm, o) ? o : NULL;
}

static bool dock_shown(const struct lm_wm *wm)
{
    return !lm_dock.autohide || wm->dock_shown;
}

static void dock_geometry(const struct lm_output *o, struct dock_geo *g)
{
    g->n = lm_dock_count();
    g->size = lm_dock.icon_size;
    g->zsize = g->size * lm_dock.zoom / 100;
    g->gap = lm_dock.gap;
    int total = g->n * g->size + (g->n - 1) * g->gap;
    g->x0 = o->x + (o->width - total) / 2;
    int out_bottom = o->y + o->height;
    g->icon_bottom = out_bottom - lm_dock.margin - DOCK_DOT;
    int top = g->icon_bottom - g->size - 8, ttop = g->icon_bottom - g->zsize - 8;
    g->strip = lm_rect_make(g->x0 - DOCK_PAD, top, total + 2 * DOCK_PAD, out_bottom - top);
    g->tall = lm_rect_make(g->x0 - DOCK_PAD, ttop, total + 2 * DOCK_PAD, out_bottom - ttop);
    int pad = (g->zsize - g->size) * 3 + DOCK_PAD + 4;          /* the neighbours of the magnified icon move out of the way */
    int btop = ttop - DOCK_LABEL_H - 10 - g->size / 2;          /* (and an icon that swells grows up to a third of its size higher) */
    struct lm_rect b = lm_rect_make(g->x0 - pad, btop, total + 2 * pad, out_bottom - btop), orect = lm_output_rect(o);
    if (!lm_rect_intersect(&b, &orect, &g->bounds))
        g->bounds = (struct lm_rect){ 0 };
}

/* Everything the icon in the hand of a dock drag may touch: a square around the pointer (icon, shadow, the x). */
static struct lm_rect dock_ghost_rect(const struct lm_wm *wm)
{
    int half = lm_dock.icon_size * lm_dock.zoom / 100 * 4 / 5 + 12;
    return lm_rect_make((int)wm->drag_x - half, (int)wm->drag_y - half, 2 * half, 2 * half);
}

static bool dock_app_matches(const char *match, const char *app_id)
{
    if (!*match)
        return false;
    size_t i = 0;
    for (; match[i] && app_id[i]; i++) {
        char c = app_id[i] == '.' ? '-' : (char)tolower((unsigned char)app_id[i]);
        if (c != match[i])
            return false;
    }
    return !match[i] && !app_id[i];
}

/* How many mapped windows belong to the button. */
static int dock_window_count(const struct lm_wm *wm, const struct lm_dock_item *it)
{
    int n = 0;
    struct lm_window *w;
    lm_list_for_each(w, &((struct lm_wm *)wm)->windows, link)
        if (w->mapped && dock_app_matches(it->match, w->app_id))
            n++;
    return n;
}

static void dock_info(const struct lm_wm *wm, struct lm_output *o, struct lm_dock_info *d)
{
    struct dock_geo g;
    dock_geometry(o, &g);
    memset(d, 0, sizeof(*d));
    d->output = o;
    d->rect = g.bounds;
    d->n = g.n;
    bool drag = wm->dock_drag && wm->dock_drag_idx >= 0 && wm->dock_drag_idx < g.n;
    d->hover = wm->dock_active && !drag ? wm->hover_dock : -1;
    d->dot_y = g.icon_bottom + DOCK_DOT / 2 + 1;
    int order[LM_DOCK_MAX];                                       /* what each place of the row holds; -1 = the gap of the icon in the hand */
    for (int i = 0; i < g.n; i++)
        order[i] = i;
    if (drag) {
        int idx = wm->dock_drag_idx, to = wm->dock_drag_to, k = 0;
        if (to < 0) {
            order[idx] = -1;                                      /* it is out of the row: the others stay where they are */
        } else {
            int rest[LM_DOCK_MAX];
            for (int i = 0; i < g.n; i++)
                if (i != idx)
                    rest[k++] = i;
            to = LM_CLAMP(to, 0, g.n - 1);
            for (int i = 0, r = 0; i < g.n; i++)
                order[i] = i == to ? -1 : rest[r++];
        }
    }

    /* The icons near the pointer grow with a bell curve; the ones beyond them are pushed aside so that nothing overlaps. Hit testing uses
     * the unmagnified places (dock_pick), so what the pointer measures against does not move under it. */
    double px = 0, py = 0;
    bool zoom = wm->dock_active && !drag && lm_dock.zoom > 100 && wm->srv->seat;
    if (zoom)
        lm_seat_pointer_position(wm->srv->seat, &px, &py);
    float scale[LM_DOCK_MAX], extra[LM_DOCK_MAX], total_extra = 0.0f;
    float sigma = 2.3f * (float)(g.size + g.gap);
    for (int i = 0; i < g.n; i++) {
        float cx = (float)(g.x0 + i * (g.size + g.gap)) + (float)g.size / 2.0f, f = 0.0f;
        if (zoom) {
            float dd = fabsf((float)px - cx);
            f = dd < sigma ? 0.5f * (1.0f + cosf(3.14159265f * dd / sigma)) : 0.0f;
        }
        scale[i] = 1.0f + ((float)lm_dock.zoom / 100.0f - 1.0f) * f;
        extra[i] = (float)g.size * (scale[i] - 1.0f);
        total_extra += extra[i];
    }
    float left = 0.0f;
    int vis = 0;
    for (int i = 0; i < g.n; i++) {
        float cx = (float)(g.x0 + i * (g.size + g.gap)) + (float)g.size / 2.0f;
        float cxp = cx + left + extra[i] / 2.0f - total_extra / 2.0f, w = (float)g.size * scale[i];
        left += extra[i];
        if (order[i] < 0)
            continue;
        int it = order[i];
        d->icon[vis].box = lm_rect_make((int)lroundf(cxp - w / 2.0f), (int)lroundf((float)g.icon_bottom - w), (int)lroundf(w), (int)lroundf(w));
        d->icon[vis].item = lm_dock_item(it);
        d->icon[vis].running = dock_window_count(wm, d->icon[vis].item) > 0;
        if (wm->dock_launch_ms[it]) {                                 /* starting: swells and shrinks again until its window is there */
            float ph = (float)((lm_now_ms() - (uint64_t)wm->dock_launch_ms[it]) % DOCK_BOUNCE_PERIOD_MS) / (float)DOCK_BOUNCE_PERIOD_MS;
            float k = 1.0f + DOCK_PULSE * (0.5f - 0.5f * cosf(6.2831853f * ph));      /* 1 -> 1 + DOCK_PULSE -> 1, smooth at both ends */
            float nw = w * k;                                         /* grows from the middle of its foot: the bottom stays where it is */
            d->icon[vis].box = lm_rect_make((int)lroundf(cxp - nw / 2.0f), (int)lroundf((float)g.icon_bottom - nw), (int)lroundf(nw), (int)lroundf(nw));
            d->icon[vis].launching = true;
        }
        vis++;
    }
    d->n = vis;
    if (drag) {                                                       /* the icon in the hand follows the pointer */
        d->drag_item = lm_dock_item(wm->dock_drag_idx);
        d->drag_box = lm_rect_make((int)wm->drag_x - g.zsize / 2, (int)wm->drag_y - g.zsize / 2, g.zsize, g.zsize);
        d->drag_remove = wm->dock_drag_to < 0 && !launcher_is_self(d->drag_item);
        struct lm_rect gr = dock_ghost_rect(wm);
        d->rect = lm_rect_union(&d->rect, &gr);
    }
    if (d->hover >= 0 && d->hover < g.n) {
        d->label_text = lm_dock_item(d->hover)->name;
        int w = lm_font_text_width(d->label_text) + 20;
        const struct lm_rect *b = &d->icon[d->hover].box;
        int x = b->x + b->w / 2 - w / 2, y = b->y - 8 - DOCK_LABEL_H;
        struct lm_rect orect = lm_output_rect(o);
        x = LM_CLAMP(x, orect.x + 4, orect.x + orect.w - w - 4);
        d->label = lm_rect_make(x, y, w, DOCK_LABEL_H);
    }
}

static void dock_damage(struct lm_wm *wm)
{
    struct lm_output *o = dock_output(wm);
    if (!o)
        return;
    struct dock_geo g;
    dock_geometry(o, &g);
    damage_rect(wm, &g.bounds);
    if (wm->dock_drag) {
        struct lm_rect gr = dock_ghost_rect(wm);
        damage_rect(wm, &gr);
    }
}

/* What the pointer at (ix, iy) touches of the dock. The magnified shape is only "solid" while the pointer is inside the dock already. */
static bool dock_pick(const struct lm_wm *wm, int ix, int iy, struct lm_pick *out)
{
    struct lm_output *o = dock_output(wm);
    if (!o || lm_lock_active(wm->srv->lock))
        return false;
    struct dock_geo g;
    dock_geometry(o, &g);
    if (dock_shown(wm)) {
        const struct lm_rect *r = wm->dock_active ? &g.tall : &g.strip;
        if (lm_rect_contains(r, ix, iy)) {
            out->kind = LM_PICK_DOCK;
            out->rect = *r;
            int rel = ix - g.x0 + g.gap / 2, pitch = g.size + g.gap;
            if (rel >= 0 && rel < g.n * pitch) {
                out->kind = LM_PICK_DOCK_ITEM;
                out->index = rel / pitch;
            }
            return true;
        }
    }
    if (lm_dock.autohide && !dock_shown(wm) && iy >= o->y + o->height - 2 && ix >= o->x && ix < o->x + o->width) {
        out->kind = LM_PICK_DOCK;                               /* the bottom edge of the screen reveals it */
        return true;
    }
    return false;
}

static void dock_hide_tick(void *data)
{
    struct lm_wm *wm = data;
    if (wm->dock_shown && !wm->dock_active) {
        dock_damage(wm);
        wm->dock_shown = false;
    }
}

/* The pointer moved: `in` = it is on the dock, `item` = the icon it is on (-1 none). */
static void dock_motion(struct lm_wm *wm, bool in, int item)
{
    double px = 0, py = 0;
    if (wm->srv->seat)
        lm_seat_pointer_position(wm->srv->seat, &px, &py);
    bool changed = in != wm->dock_active || item != wm->hover_dock;
    if (in && (int)px != wm->dock_px)
        changed = true;
    wm->dock_px = (int)px;
    if (in && lm_dock.autohide) {
        if (wm->dock_timer)
            lm_timer_arm_ms(wm->dock_timer, 0, 0);
        if (!wm->dock_shown) {
            wm->dock_shown = true;
            changed = true;
        }
    } else if (!in && wm->dock_shown && lm_dock.autohide && wm->dock_timer && !lm_timer_is_armed(wm->dock_timer)) {
        lm_timer_arm_ms(wm->dock_timer, DOCK_HIDE_MS, 0);
    }
    wm->dock_active = in;
    wm->hover_dock = item;
    if (changed)
        dock_damage(wm);
}

/* A click on icon `i`: bring the program's window forward when it has one that is not in front already (`reuse`), else start it. */
static void dock_activate(struct lm_wm *wm, int i, bool reuse)
{
    const struct lm_dock_item *it = lm_dock_item(i);
    if (it)
        dock_launch(wm, it, i, reuse);
}

/* Starts the program of `it` or brings its window forward. `i` is its place in the dock (the icon swells there while it starts), -1 for a
 * launcher button that is not in the dock. */
static void dock_launch(struct lm_wm *wm, const struct lm_dock_item *it, int i, bool reuse)
{
    if (launcher_is_self(it)) {                                       /* the Launcher button: not a program */
        launcher_set(wm, true, dock_output(wm));
        return;
    }
    struct lm_window *w, *best = NULL;
    if (reuse)
        lm_list_for_each(w, &wm->windows, link)
            if (w->mapped && dock_app_matches(it->match, w->app_id) && (!best || WIN(w)->focus_seq > WIN(best)->focus_seq))
                best = w;
    /* A window that is not in front yet comes forward; when the most recent one already has the focus the click is for another
     * instance (two terminals): the program is started again. */
    if (best && !(best == wm->focused && !best->minimized && best->workspace == wm->workspace)) {
        lm_wm_focus_window(wm, best);
    } else if (lm_server_spawn(wm->srv, it->cmd) > 0 && i >= 0 && i < LM_DOCK_MAX) {
        wm->dock_launch_ms[i] = (int64_t)lm_now_ms();                 /* the icon swells until a window of the program shows up */
        wm->dock_launch_base[i] = dock_window_count(wm, it);
        if (wm->dock_anim)
            lm_timer_arm_ms(wm->dock_anim, DOCK_ANIM_MS, DOCK_ANIM_MS);
        dock_damage(wm);
    }
}

/* ---- mouse drags: dock icons (reorder, take out) and launcher icons (onto the dock) ---------------------------------------------- */

#define DRAG_START_PX 8             /* the pointer must move this far with the button down before a press becomes a drag */
#define DOCK_OUT_PX 60              /* a dock icon dragged this far above the row leaves the dock */

static bool drag_far_enough(const struct lm_wm *wm, double x, double y)
{
    double dx = x - wm->drag_press_x, dy = y - wm->drag_press_y;
    return dx * dx + dy * dy >= (double)(DRAG_START_PX * DRAG_START_PX);
}

static void dock_drag_notify_failed(struct lm_wm *wm)
{
    lm_wm_notify(wm, "Dock", "Could not change the dock: there is no config file to write, or the list is full", 5000);
}

static void dock_drag_finish(struct lm_wm *wm)
{
    int idx = wm->dock_drag_idx, to = wm->dock_drag_to;
    dock_damage(wm);
    wm->dock_drag = false;
    dock_damage(wm);
    const struct lm_dock_item *it = lm_dock_item(idx);
    if (!it)
        return;
    if (to < 0) {                                                   /* out of the dock: it is gone from there, the launcher keeps it */
        if (!launcher_is_self(it) && !lm_settings_dock_to_launcher(wm->srv, it->raw))
            dock_drag_notify_failed(wm);
    } else if (to != idx) {
        const struct lm_dock_item *target = lm_dock_item(to);
        if (target && !lm_settings_dock_reorder(wm->srv, it->raw, target->raw))
            dock_drag_notify_failed(wm);
    }
}

/* The pointer moved while a dock icon is pressed: start the drag once it has moved enough, then follow. */
static void dock_drag_motion(struct lm_wm *wm)
{
    if (wm->press_panel_kind != LM_PICK_DOCK_ITEM || !wm->dock_press_left || !wm->srv->seat)
        return;
    double x, y;
    lm_seat_pointer_position(wm->srv->seat, &x, &y);
    struct lm_output *o = dock_output(wm);
    if (!o)
        return;
    struct dock_geo g;
    dock_geometry(o, &g);
    if (!wm->dock_drag) {
        if (!drag_far_enough(wm, x, y) || wm->press_panel_ws < 0 || wm->press_panel_ws >= g.n)
            return;
        wm->dock_drag = true;
        wm->dock_drag_idx = wm->press_panel_ws;
        wm->dock_active = false;
        wm->hover_dock = -1;
    }
    dock_damage(wm);                                                /* (where the icon in the hand was) */
    wm->drag_x = x;
    wm->drag_y = y;
    if (y < g.icon_bottom - g.size - DOCK_OUT_PX)
        wm->dock_drag_to = -1;
    else
        wm->dock_drag_to = LM_CLAMP((int)floor((x - g.x0) / (double)(g.size + g.gap)), 0, g.n - 1);
    dock_damage(wm);
}

/* Where a launcher icon can be dropped: the dock's place (the row and a margin around it). *pos = the place it would take, -1 when the
 * pointer is not on it. False when the dock is off. */
static bool launcher_drop_zone(const struct lm_wm *wm, struct lm_output *o, struct lm_rect *zone, struct lm_rect *marker, int *pos)
{
    *pos = -1;
    if (!lm_dock.enabled || lm_dock_count() == 0)
        return false;
    struct dock_geo g;
    dock_geometry(o, &g);
    struct lm_rect z = lm_rect_make(g.strip.x - 60, g.strip.y - 40, g.strip.w + 120, o->y + o->height - (g.strip.y - 40)), orect = lm_output_rect(o);
    if (!lm_rect_intersect(&z, &orect, zone))
        return false;
    if (lm_rect_contains(zone, (int)wm->drag_x, (int)wm->drag_y)) {
        int pitch = g.size + g.gap;
        *pos = LM_CLAMP((int)floor((wm->drag_x - g.x0 + pitch / 2.0) / (double)pitch), 0, g.n);
        *marker = lm_rect_make(g.x0 + *pos * pitch - g.gap / 2 - 1, g.icon_bottom - g.size, 3, g.size);
    }
    return true;
}

static void launcher_drag_motion(struct lm_wm *wm)
{
    if (wm->press_panel_kind != LM_PICK_LAUNCHER_ITEM || !wm->srv->seat || wm->launcher_form || wm->launcher_edit)
        return;
    double x, y;
    lm_seat_pointer_position(wm->srv->seat, &x, &y);
    if (!wm->launcher_drag) {
        if (!drag_far_enough(wm, x, y))
            return;
        wm->launcher_drag = true;
        wm->launcher_drag_g = wm->press_panel_ws;
    }
    wm->drag_x = x;
    wm->drag_y = y;
    launcher_damage(wm);
}

static void launcher_drag_finish(struct lm_wm *wm)
{
    struct lm_output *o = launcher_output(wm);
    struct lm_rect zone = { 0 }, marker = { 0 };
    int pos = -1;
    if (o)
        launcher_drop_zone(wm, o, &zone, &marker, &pos);
    int g = wm->launcher_drag_g;
    wm->launcher_drag = false;
    launcher_damage(wm);
    const struct lm_dock_item *it = launcher_item(g, NULL);
    if (!it || pos < 0 || !it->launcher_only)                       /* (a button that is in the dock already stays where it is) */
        return;
    const struct lm_dock_item *before = pos < lm_dock_count() ? lm_dock_item(pos) : NULL;
    if (!lm_settings_launcher_to_dock(wm->srv, it->raw, before ? before->raw : -1))
        lm_wm_notify(wm, "Launcher", "Could not add it to the dock: it is full, the name is in use, or there is no config file", 5000);
}

/* Every 33 ms while some icon swells: ends the swelling of the ones whose window came (or that never made one) and repaints. */
static void dock_anim_tick(void *data)
{
    struct lm_wm *wm = data;
    const char *e = getenv("MELLAGUI_DOCK_BOUNCE_MS");               /* how long to swell at most; tests make it short */
    int64_t limit = e && atol(e) > 0 ? atol(e) : 12000;
    bool any = false;
    for (int i = 0; i < LM_DOCK_MAX; i++) {
        if (!wm->dock_launch_ms[i])
            continue;
        const struct lm_dock_item *it = lm_dock_item(i);
        if (!it || dock_window_count(wm, it) > wm->dock_launch_base[i] || (int64_t)lm_now_ms() - wm->dock_launch_ms[i] > limit)
            wm->dock_launch_ms[i] = 0;
        else
            any = true;
    }
    dock_damage(wm);
    if (!any && wm->dock_anim)
        lm_timer_arm_ms(wm->dock_anim, 0, 0);
}

void lm_wm_clip_view(struct lm_wm *wm, int *top, int *slots)
{
    *top = clip_top(wm);
    *slots = clip_slots(wm);
}

/* ---- the calendar ------------------------------------------------------------------------------------------------------- */

static struct lm_window *calendar_window(struct lm_wm *wm)
{
    struct lm_window *w;
    lm_list_for_each(w, &wm->windows, link)
        if (w->mapped && win_is_calendar(w))
            return w;
    return NULL;
}

static bool calendar_alive(struct lm_wm *wm)
{
    if (wm->cal_pid <= 0)
        return false;
    int st;
    pid_t r = waitpid(wm->cal_pid, &st, WNOHANG);
    if (r == wm->cal_pid || (r < 0 && kill(wm->cal_pid, 0) != 0)) {
        wm->cal_pid = 0;
        return false;
    }
    return true;
}

static void calendar_spawn(struct lm_wm *wm)
{
    const char *cmd = wm->srv->config.calendar_cmd;
    if (!wm->srv->config.cal_enabled || !cmd || !*cmd || calendar_alive(wm))
        return;
    wm->cal_pid = lm_server_spawn(wm->srv, cmd);
    if (wm->cal_pid < 0)
        wm->cal_pid = 0;
}

void lm_wm_calendar_start(struct lm_wm *wm)
{
    if (wm)
        calendar_spawn(wm);
}

bool lm_wm_calendar_toggle(struct lm_wm *wm, const struct lm_rect *clock)
{
    if (!wm || !wm->srv->config.cal_enabled || !wm->srv->config.calendar_cmd || !*wm->srv->config.calendar_cmd)
        return false;
    struct lm_output *o = NULL;
    if (clock) {
        wm->cal_clock = *clock;
    } else {
        o = lm_output_primary(wm->srv);
        if (!o)
            return false;
        struct lm_panel_info pi;
        panel_info(wm, o, &pi);
        wm->cal_clock = pi.clock;
    }
    struct lm_window *w = calendar_window(wm);
    if (!w) {                                                /* not there (yet, or it ended): start it and show it when its window comes */
        wm->cal_pending = true;
        wm->cal_pending_ms = lm_now_ms();
        calendar_spawn(wm);
        return true;
    }
    if (win_visible(wm, w)) {
        lm_wm_window_request_minimize(w);
        return true;
    }
    o = lm_output_nearest(wm->srv, wm->cal_clock.x, wm->cal_clock.y);
    int32_t right = wm->cal_clock.x + wm->cal_clock.w;
    int32_t ox = o ? o->x : 0, ow = o ? o->width : 1280;
    w->x = right - LM_BORDER_WIDTH - w->width;
    if (w->x < ox + LM_BORDER_WIDTH)
        w->x = ox + LM_BORDER_WIDTH;
    if (w->x + w->width + LM_BORDER_WIDTH > ox + ow)
        w->x = ox + ow - w->width - LM_BORDER_WIDTH;
    w->y = wm->cal_clock.y + wm->cal_clock.h + 4 + LM_TITLEBAR_HEIGHT;
    w->workspace = wm->workspace;
    w->minimized = false;
    update_outputs(wm, w);
    raise_window(wm, w);
    damage_frame(wm, w);
    set_focus(wm, w);
    damage_panels(wm);
    refresh_pointer(wm);
    return true;
}

bool lm_wm_has_fullscreen(const struct lm_wm *wm)
{
    struct lm_window *w;
    lm_list_for_each(w, &((struct lm_wm *)wm)->windows, link)
        if (win_visible(wm, w) && (w->states & LM_WINDOW_FULLSCREEN))
            return true;
    return false;
}

bool lm_wm_surface_visible(const struct lm_wm *wm, struct lm_surface *s)
{
    if (!wm || !s)
        return false;
    struct lm_surface *root = lm_surface_root(s);
    struct lm_window *w;
    lm_list_for_each(w, &((struct lm_wm *)wm)->windows, link)
        if (w->surface == root)
            return win_visible(wm, w);
    return false;
}

size_t lm_wm_dock_describe(struct lm_wm *wm, char *buf, size_t size)
{
    size_t o = 0;
    struct lm_output *out = dock_output(wm);
    struct lm_dock_info d;
    bool shown = out && dock_shown(wm);
    if (out)
        dock_info(wm, out, &d);
    o += (size_t)snprintf(buf + o, size - o, "dock shown=%d hover=%d n=%d drag=%d to=%d\n", shown, shown ? d.hover : -1, out ? d.n : 0,
                          wm->dock_drag ? wm->dock_drag_idx : -1, wm->dock_drag ? wm->dock_drag_to : -1);
    for (int i = 0; out && i < d.n && o < size; i++)
        o += (size_t)snprintf(buf + o, size - o, "icon %d \"%s\" x=%d y=%d w=%d h=%d running=%d launching=%d cmd=%s\n", i, d.icon[i].item->name,
                              d.icon[i].box.x, d.icon[i].box.y, d.icon[i].box.w, d.icon[i].box.h, d.icon[i].running, d.icon[i].launching, d.icon[i].item->cmd);
    return o < size ? o : size - 1;
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
            int top = clip_top(wm);
            it.u.gadget.total = total;
            it.u.gadget.top = top;
            int n = LM_MAX(LM_MIN(total - top, it.u.gadget.slots), 0);
            it.u.gadget.n = n;
            it.u.gadget.cursor = wm->clip_nav && wm->clip_cur >= top && wm->clip_cur < top + n ? wm->clip_cur - top : -1;
            it.u.gadget.hover = wm->hover_clip;
            clip_buttons(&gr, it.u.gadget.btn);
            for (int i = 0; i < n; i++)
                it.u.gadget.text[i] = lm_seat_clip_get(wm->srv->seat, top + i);
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
            if (fullscreen_covers(wm, o))
                continue;
            struct lm_rect sr = lm_rect_make(o->x, o->y + LM_PANEL_HEIGHT, o->width, LM_PANEL_SHADOW);
            if (!clip || lm_rect_overlaps(&sr, clip)) {
                memset(&it, 0, sizeof(it));
                it.kind = LM_SCENE_PANEL_SHADOW;
                it.output = o;
                it.u.shadow = sr;
                if (!fn(&it, data))
                    return;
            }
            if (clip && !lm_rect_overlaps(&pr, clip))
                continue;
            memset(&it, 0, sizeof(it));
            it.kind = LM_SCENE_PANEL;
            it.output = o;
            panel_info(wm, o, &it.u.panel);
            if (!fn(&it, data))
                return;
        }
    }

    struct lm_output *dock_out = dock_output(wm);
    if (dock_out && dock_shown(wm)) {
        struct dock_geo g;
        dock_geometry(dock_out, &g);
        if (!lm_rect_is_empty(&g.bounds)) {
            memset(&it, 0, sizeof(it));
            it.kind = LM_SCENE_DOCK;
            it.output = dock_out;
            dock_info(wm, dock_out, &it.u.dock);
            if ((!clip || lm_rect_overlaps(&it.u.dock.rect, clip)) && !fn(&it, data))
                return;
        }
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

    if (wm->about_open) {                                        /* the About dialog */
        struct lm_output *ao = about_output(wm);
        if (ao) {
            struct lm_rect r = lm_output_rect(ao);
            if (!clip || lm_rect_overlaps(&r, clip)) {
                memset(&it, 0, sizeof(it));
                it.kind = LM_SCENE_ABOUT;
                it.output = ao;
                about_info(wm, ao, &it.u.about);
                if (!fn(&it, data))
                    return;
            }
        }
    }

    if (wm->launcher_open) {                                     /* the launcher dialog */
        struct lm_output *lo = launcher_output(wm);
        if (lo) {
            struct lm_rect r = lm_output_rect(lo);
            if (!clip || lm_rect_overlaps(&r, clip)) {
                memset(&it, 0, sizeof(it));
                it.kind = LM_SCENE_LAUNCHER;
                it.output = lo;
                launcher_info(wm, lo, &it.u.launcher);
                if (!fn(&it, data))
                    return;
            }
        }
    }

    if (wm->osk_open) {                                          /* the on-screen keyboard: above the dialogs, below the Alt+Tab list */
        struct lm_osk_geo og;
        if (osk_geo(wm, &og) && (!clip || lm_rect_overlaps(&og.card, clip))) {
            memset(&it, 0, sizeof(it));
            it.kind = LM_SCENE_OSK;
            it.output = lm_output_primary(wm->srv);
            osk_info(wm, &og, &it.u.osk);
            if (!fn(&it, data))
                return;
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

    if (osk_pick(wm, ix, iy, out))                               /* the on-screen keyboard is above everything, dialogs included */
        return;
    if (launcher_pick(wm, ix, iy, out))                          /* the launcher dialog is modal: it gets every pick */
        return;
    struct lm_output *ao = about_output(wm);
    if (ao && !lm_lock_active(wm->srv->lock)) {                  /* the About dialog is modal, too */
        struct lm_about_info ai;
        about_info(wm, ao, &ai);
        out->kind = lm_rect_contains(&ai.card, ix, iy) ? LM_PICK_ABOUT_CARD : LM_PICK_ABOUT_BACKDROP;
        if (lm_rect_contains(&ai.close, ix, iy))
            out->kind = LM_PICK_ABOUT_BUTTON;
        out->rect = ai.card;
        return;
    }
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

    if (dock_pick(wm, ix, iy, out))                              /* the dock floats above the windows */
        return;

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
            if (gid == LM_GADGET_CLIPBOARD) {                         /* the small buttons in its title bar */
                struct lm_rect b[3];
                clip_buttons(&gr, b);
                static const enum lm_pick_kind kinds[3] = { LM_PICK_CLIP_UP, LM_PICK_CLIP_DOWN, LM_PICK_CLIP_CLEAR };
                for (int k = 0; k < 3; k++)
                    if (lm_rect_contains(&b[k], ix, iy)) {
                        out->kind = kinds[k];
                        out->rect = gr;
                        return;
                    }
            }
        } else if (gid == LM_GADGET_MIXER) {
            out->kind = LM_PICK_GADGET_BODY;                      /* sliders and buttons */
        } else if (gid == LM_GADGET_CLIPBOARD) {                  /* an entry slot with text in it */
            int slots, entry_h;
            gadget_slots(gr.h, &slots, &entry_h);
            int top = clip_top(wm);
            int n = LM_MAX(LM_MIN(lm_seat_clip_count(wm->srv->seat) - top, slots), 0);
            int k = (iy - gr.y - GADGET_TITLE_H) / entry_h;
            if (k >= 0 && k < n && ix >= gr.x + 12 && ix < gr.x + gr.w - 12) {
                out->kind = ix >= gr.x + gr.w - 12 - LM_CLIP_DEL_W ? LM_PICK_CLIP_DELETE : LM_PICK_CLIP_ENTRY;
                out->index = top + k;
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
            int lcx = pi.launcher.x + pi.launcher.w / 2, lcy = pi.launcher.y + pi.launcher.h / 2, lr = pi.launcher.w / 2 + 1;
            if ((ix - lcx) * (ix - lcx) + (iy - lcy) * (iy - lcy) <= lr * lr) {           /* the circle, not its square */
                out->kind = LM_PICK_PANEL_LAUNCHER;
                out->rect = pi.launcher;
            }
            if (lm_rect_contains(&pi.run, ix, iy))
                out->kind = LM_PICK_PANEL_RUN;
            if (lm_rect_contains(&pi.clock, ix, iy) && wm->srv->config.cal_enabled) {
                out->kind = LM_PICK_PANEL_CLOCK;
                out->rect = pi.clock;
            }
            int pcx = pi.power.x + pi.power.w / 2, pcy = pi.power.y + pi.power.h / 2, pr2 = pi.power.w / 2 + 1;
            if ((ix - pcx) * (ix - pcx) + (iy - pcy) * (iy - pcy) <= pr2 * pr2)   /* the circle, not its square */
                out->kind = LM_PICK_PANEL_POWER;
            if (lm_rect_contains(&pi.layout, ix, iy))
                out->kind = LM_PICK_PANEL_LAYOUT;
            if (lm_rect_contains(&pi.osk, ix, iy))
                out->kind = LM_PICK_PANEL_OSK;
            if (lm_rect_contains(&pi.sticky, ix, iy))
                out->kind = LM_PICK_PANEL_STICKY;
            for (int i = 0; i < pi.ntray; i++)
                if (lm_rect_contains(&pi.tray[i].rect, ix, iy)) {
                    out->kind = LM_PICK_PANEL_TRAY;
                    out->index = pi.tray[i].id;
                    out->rect = pi.tray[i].rect;
                }
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
    osk_drag_motion(wm);
    dock_drag_motion(wm);
    launcher_drag_motion(wm);
    struct lm_window *hw = NULL;
    uint32_t bits = 0;
    int pws = -1;
    bool about_btn = false, launcher = false, power_btn = false, run_btn = false, layout_btn = false, sticky_btn = false, osk_btn = false;
    int power_item = -1, clip_code = 0;
    struct lm_window *task = NULL;
    int tray_id = 0;
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
    case LM_PICK_ABOUT_BUTTON: about_btn = true; shape = LM_CURSOR_POINTER; break;
    case LM_PICK_DOCK_ITEM: shape = LM_CURSOR_POINTER; break;
    case LM_PICK_LAUNCHER_ITEM: case LM_PICK_LAUNCHER_REMOVE: case LM_PICK_LAUNCHER_ADD: case LM_PICK_LAUNCHER_BUTTON:
    case LM_PICK_LAUNCHER_KIND: shape = LM_CURSOR_POINTER; break;
    case LM_PICK_CLIP_UP: clip_code = 1; shape = LM_CURSOR_POINTER; break;
    case LM_PICK_CLIP_DOWN: clip_code = 2; shape = LM_CURSOR_POINTER; break;
    case LM_PICK_CLIP_CLEAR: clip_code = 3; shape = LM_CURSOR_POINTER; break;
    case LM_PICK_CLIP_DELETE: clip_code = 10 + pick->index - clip_top(wm); shape = LM_CURSOR_POINTER; break;
    case LM_PICK_CLIP_ENTRY: shape = LM_CURSOR_POINTER; break;
    case LM_PICK_PANEL_RUN: run_btn = true; shape = LM_CURSOR_POINTER; break;
    case LM_PICK_PANEL_CLOCK: shape = LM_CURSOR_POINTER; break;
    case LM_PICK_PANEL_POWER: power_btn = true; shape = LM_CURSOR_POINTER; break;
    case LM_PICK_POWER_BUTTON: power_item = pick->index; shape = LM_CURSOR_POINTER; break;
    case LM_PICK_PANEL_LAYOUT: layout_btn = true; shape = LM_CURSOR_POINTER; break;
    case LM_PICK_PANEL_STICKY: sticky_btn = true; shape = LM_CURSOR_POINTER; break;
    case LM_PICK_PANEL_OSK: osk_btn = true; shape = LM_CURSOR_POINTER; break;
    case LM_PICK_OSK_KEY: shape = LM_CURSOR_POINTER; break;
    case LM_PICK_OSK: shape = LM_CURSOR_MOVE; break;
    case LM_PICK_OSK_GRIP: shape = cursor_for_edges(pick->edges); break;
    case LM_PICK_PANEL_WORKSPACE: pws = pick->index; shape = LM_CURSOR_POINTER; break;
    case LM_PICK_PANEL_TASK: task = pick->window; shape = LM_CURSOR_POINTER; break;
    case LM_PICK_PANEL_TRAY: tray_id = pick->index; shape = LM_CURSOR_POINTER; break;
    default: break;
    }
    if (bits)
        shape = LM_CURSOR_POINTER;
    dock_motion(wm, pick->kind == LM_PICK_DOCK_ITEM || pick->kind == LM_PICK_DOCK, pick->kind == LM_PICK_DOCK_ITEM ? pick->index : -1);

    if (hw != wm->hover_win || bits != wm->hover_bits) {
        if (wm->hover_win)
            damage_frame(wm, wm->hover_win);
        wm->hover_win = hw;
        wm->hover_bits = bits;
        if (hw)
            damage_frame(wm, hw);
    }
    if (clip_code != wm->hover_clip) {
        wm->hover_clip = clip_code;
        damage_gadget(wm, LM_GADGET_CLIPBOARD);
    }
    if (wm->launcher_open) {                                    /* the pointer moves the launcher's highlight */
        struct lm_output *lo = launcher_output(wm);
        int hb = pick->kind == LM_PICK_LAUNCHER_BUTTON ? pick->index : -1, sel = wm->launcher_sel;
        if (pick->kind == LM_PICK_LAUNCHER_ITEM || pick->kind == LM_PICK_LAUNCHER_REMOVE)
            sel = pick->index;
        else if (pick->kind == LM_PICK_LAUNCHER_ADD && lo)
            sel = launcher_count();
        else if (pick->kind == LM_PICK_LAUNCHER_BACKDROP || pick->kind == LM_PICK_LAUNCHER_CARD)
            sel = wm->launcher_form ? sel : -1;
        if (hb != wm->launcher_hover_btn || sel != wm->launcher_sel) {
            wm->launcher_hover_btn = hb;
            wm->launcher_sel = sel;
            launcher_damage(wm);
        }
    }
    if (wm->osk_open) {                                         /* the key under the pointer lights up */
        int hk = pick->kind == LM_PICK_OSK_KEY ? pick->index : -1;
        if (hk != wm->osk_hover) {
            wm->osk_hover = hk;
            osk_damage(wm);
        }
    }
    if (wm->about_open && about_btn != wm->about_hover_close) {
        wm->about_hover_close = about_btn;
        about_damage(wm);
    }
    if (wm->power_open && power_item >= 0 && power_item != wm->power_sel) {     /* the pointer moves the dialog's highlight */
        wm->power_sel = power_item;
        power_damage(wm);
    }
    if (pws != wm->hover_panel_ws || launcher != wm->hover_launcher || power_btn != wm->hover_power ||
        run_btn != wm->hover_run || task != wm->hover_task || layout_btn != wm->hover_layout ||
        sticky_btn != wm->hover_sticky || osk_btn != wm->hover_osk || tray_id != wm->hover_tray) {
        wm->hover_tray = tray_id;
        wm->hover_osk = osk_btn;
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
    if (!pressed && wm->osk_down >= 0 && button == BTN_LEFT) {   /* the held on-screen key comes up wherever the pointer is */
        osk_release(wm);
        return true;
    }
    if (!pressed && wm->osk_drag && button == BTN_LEFT) {        /* the keyboard was moved / resized: remember where it is */
        wm->osk_drag = 0;
        gadgets_save(wm);
        lm_wm_osk_layout_changed(wm);                           /* (not while it is dragged: the windows would run about) */
        return true;
    }
    if (pick->kind == LM_PICK_OSK_KEY || pick->kind == LM_PICK_OSK || pick->kind == LM_PICK_OSK_GRIP) {
        if (pressed && button == BTN_LEFT) {
            if (pick->kind == LM_PICK_OSK_KEY) {
                osk_press(wm, pick->index);
            } else if (pick->kind == LM_PICK_OSK && time_ms - wm->osk_click_ms < (uint32_t)DOUBLE_CLICK_MS && wm->osk_pl.w > 0) {
                wm->osk_click_ms = 0;                            /* a double click on the margin puts it back to its default place and size */
                lm_wm_osk_reset(wm);
            } else {
                wm->osk_click_ms = pick->kind == LM_PICK_OSK ? time_ms : 0;
                osk_drag_begin(wm, pick->kind == LM_PICK_OSK ? 1 : 2, pick->edges);
            }
        }
        return true;                                            /* the keyboard never moves the focus and nothing under it gets the click */
    }
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
    if (pick->kind >= LM_PICK_LAUNCHER_ITEM && pick->kind <= LM_PICK_LAUNCHER_BACKDROP) {
        int pk = wm->press_panel_kind, pi = wm->press_panel_ws;
        if (pressed) {
            if (pick->kind == LM_PICK_LAUNCHER_BACKDROP) {      /* a click outside the card closes it, nothing below gets it */
                launcher_set(wm, false, NULL);
            } else if (pick->kind != LM_PICK_LAUNCHER_CARD && button == BTN_LEFT) {
                wm->press_panel_kind = pick->kind;
                wm->press_panel_ws = pick->index;
                if (wm->srv->seat) {
                    double px, py;
                    lm_seat_pointer_position(wm->srv->seat, &px, &py);
                    wm->drag_press_x = (int)px;
                    wm->drag_press_y = (int)py;
                }
                wm->press_win = NULL;
                wm->press_bits = 0;
            }
        } else {
            wm->press_panel_kind = 0;
            if (wm->launcher_drag)
                launcher_drag_finish(wm);                       /* a drag ends with the button: dropped on the dock or not */
            else if (pk == (int)pick->kind && pi == pick->index && button == BTN_LEFT)
                launcher_activate(wm, pk, pi);                  /* released on what it was pressed on */
        }
        return true;
    }
    if (pick->kind == LM_PICK_ABOUT_BUTTON || pick->kind == LM_PICK_ABOUT_CARD || pick->kind == LM_PICK_ABOUT_BACKDROP) {
        int pk = wm->press_panel_kind;
        if (pressed) {
            if (pick->kind == LM_PICK_ABOUT_BACKDROP)           /* a click outside the card closes it, nothing below gets it */
                about_set(wm, false, NULL);
            else if (pick->kind == LM_PICK_ABOUT_BUTTON && button == BTN_LEFT) {
                wm->press_panel_kind = LM_PICK_ABOUT_BUTTON;
                wm->press_win = NULL;
                wm->press_bits = 0;
            }
        } else {
            wm->press_panel_kind = 0;
            if (pk == LM_PICK_ABOUT_BUTTON && pick->kind == LM_PICK_ABOUT_BUTTON && button == BTN_LEFT)
                about_set(wm, false, NULL);
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
        if (pk == LM_PICK_PANEL_LAUNCHER && pick->kind == LM_PICK_PANEL_LAUNCHER)
            about_set(wm, true, lm_output_nearest(wm->srv, pick->rect.x, pick->rect.y));
        if (pk == LM_PICK_DOCK_ITEM && wm->dock_drag)
            dock_drag_finish(wm);                               /* it was dragged: moved or taken out, not clicked */
        else if (pk == LM_PICK_DOCK_ITEM && pick->kind == LM_PICK_DOCK_ITEM && pick->index == pi)
            dock_activate(wm, pi, button != BTN_MIDDLE);        /* the middle button always starts another instance */
        if (pk == LM_PICK_PANEL_RUN && pick->kind == LM_PICK_PANEL_RUN)
            launch_run(wm);
        if (pk == LM_PICK_PANEL_CLOCK && pick->kind == LM_PICK_PANEL_CLOCK)
            lm_wm_calendar_toggle(wm, &pick->rect);
        if (pk == LM_PICK_PANEL_LAYOUT && pick->kind == LM_PICK_PANEL_LAYOUT && wm->srv->input)
            lm_input_cycle_layout(wm->srv->input, pi);
        if (pk == LM_PICK_PANEL_OSK && pick->kind == LM_PICK_PANEL_OSK)
            osk_set(wm, !wm->osk_open);
        if (pk == LM_PICK_PANEL_STICKY && pick->kind == LM_PICK_PANEL_STICKY && wm->srv->input)
            lm_input_set_sticky(wm->srv->input, !lm_input_sticky(wm->srv->input));
        if (pk == LM_PICK_PANEL_TRAY && pick->kind == LM_PICK_PANEL_TRAY && pick->index == pi)
            lm_tray_click(wm->srv->tray, pi, button);               /* pressed and let go on the same item */
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
        if (wm->last_click_win == pick->window && time_ms - wm->last_click_time < (uint32_t)DOUBLE_CLICK_MS) {
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
        if (time_ms - wm->gad[id].click_time < (uint32_t)DOUBLE_CLICK_MS) {
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
    case LM_PICK_CLIP_DELETE:
        if (button == BTN_LEFT)
            lm_seat_clip_remove(wm->srv->seat, pick->index);
        return true;
    case LM_PICK_CLIP_UP:
    case LM_PICK_CLIP_DOWN:
        if (button == BTN_LEFT) {                                       /* a page at a time */
            int slots = LM_MAX(clip_slots(wm), 1);
            wm->clip_top = clip_top(wm) + (pick->kind == LM_PICK_CLIP_UP ? -slots : slots);
            wm->clip_top = clip_top(wm);
            if (wm->clip_nav)
                wm->clip_cur = LM_CLAMP(wm->clip_cur, wm->clip_top, wm->clip_top + slots - 1);
            damage_gadget(wm, LM_GADGET_CLIPBOARD);
        }
        return true;
    case LM_PICK_CLIP_CLEAR:
        if (button == BTN_LEFT)
            lm_seat_clip_clear(wm->srv->seat);
        return true;
    case LM_PICK_DOCK_ITEM:
        if (button == BTN_LEFT || button == BTN_MIDDLE) {
            wm->press_panel_kind = LM_PICK_DOCK_ITEM;
            wm->press_panel_ws = pick->index;
            wm->dock_press_left = button == BTN_LEFT;
            if (wm->srv->seat) {
                double px, py;
                lm_seat_pointer_position(wm->srv->seat, &px, &py);
                wm->drag_press_x = (int)px;
                wm->drag_press_y = (int)py;
            }
            wm->press_win = NULL;
            wm->press_bits = 0;
        }
        return true;
    case LM_PICK_DOCK:
        return true;
    case LM_PICK_PANEL_LAUNCHER:
        if (button == BTN_LEFT) {
            wm->press_panel_kind = LM_PICK_PANEL_LAUNCHER;
            wm->press_win = NULL;
            wm->press_bits = 0;
        }
        return true;
    case LM_PICK_PANEL_CLOCK:
        if (button == BTN_LEFT) {
            wm->press_panel_kind = pick->kind;
            wm->cal_clock = pick->rect;
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
    case LM_PICK_PANEL_OSK:
        if (button == BTN_LEFT || (button == BTN_RIGHT && pick->kind == LM_PICK_PANEL_LAYOUT)) {
            wm->press_panel_kind = pick->kind;
            wm->press_panel_ws = button == BTN_RIGHT ? -1 : 1;     /* layout direction, see the release code */
            wm->press_win = NULL;
            wm->press_bits = 0;
        }
        return true;
    case LM_PICK_PANEL_TRAY:
        if (button == BTN_LEFT || button == BTN_RIGHT || button == BTN_MIDDLE) {
            wm->press_panel_kind = LM_PICK_PANEL_TRAY;
            wm->press_panel_ws = pick->index;
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
            if (w->mapped && w->workspace == wm->workspace && !w->minimized && !win_is_calendar(w) && ++seen == 2) {
                pick = w;
                break;
            }
    } else {
        /* the lowest window of the workspace (minimized ones included) comes to the front */
        lm_list_for_each(w, &wm->windows, link)
            if (w->mapped && w->workspace == wm->workspace && w != wm->focused && !win_is_calendar(w)) {
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
        about_set(wm, false, NULL);
        clip_nav_set(wm, false);
        if (wm->srv->saver)
            lm_saver_locked(wm->srv->saver, true);
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

/* One shortcut of the [keys] table (keys.h). Returns true when the key is swallowed (every bound key is, press and release). */
static bool run_keybind(struct lm_wm *wm, const struct lm_keybind *kb, bool pressed, bool repeat)
{
    struct lm_window *f = wm->focused;
    if (!pressed)
        return true;
    if (repeat && kb->action != LM_ACT_FOCUS_NEXT && kb->action != LM_ACT_FOCUS_PREV)
        return true;
    switch (kb->action) {
    case LM_ACT_TERMINAL: launch_terminal(wm); break;
    case LM_ACT_RUN: launch_run(wm); break;
    case LM_ACT_LAUNCHER: launcher_set(wm, !wm->launcher_open, NULL); break;
    case LM_ACT_OSK: osk_set(wm, !wm->osk_open); break;
    case LM_ACT_SETTINGS: launch_settings(wm); break;
    case LM_ACT_EXEC: {
        char cmd[2048];
        lm_launch_expand(kb->arg, cmd, sizeof(cmd));
        lm_server_spawn(wm->srv, cmd);
        break;
    }
    case LM_ACT_LAYOUT_NEXT: case LM_ACT_LAYOUT_PREV:
        if (wm->srv->input)
            lm_input_cycle_layout(wm->srv->input, kb->action == LM_ACT_LAYOUT_PREV ? -1 : 1);
        break;
    case LM_ACT_GADGET_CLIPBOARD:
        lm_wm_set_gadget(wm, !wm->gad[LM_GADGET_CLIPBOARD].visible);
        break;
    case LM_ACT_CLIPBOARD_PICK:                 /* pick from the clipboard history with the arrow keys */
        if (!wm->gad[LM_GADGET_CLIPBOARD].visible)
            lm_wm_set_gadget(wm, true);
        if (clip_nav_count(wm) > 0)
            clip_nav_set(wm, !wm->clip_nav);
        break;
    case LM_ACT_GADGET_MIXER: lm_wm_set_gadget_id(wm, LM_GADGET_MIXER, !wm->gad[LM_GADGET_MIXER].visible); break;
    case LM_ACT_GADGET_SYSTEM: lm_wm_set_gadget_id(wm, LM_GADGET_SYSTEM, !wm->gad[LM_GADGET_SYSTEM].visible); break;
    case LM_ACT_CLOSE:
        if (f) lm_wm_window_close(f);
        break;
    case LM_ACT_LOCK:                           /* only when a password / PIN is set */
        if (!lm_lock_now(wm->srv->lock))
            lm_info("lock: the lock shortcut is ignored, no password or PIN is set (settings: Security)");
        break;
    case LM_ACT_WORKSPACE: lm_wm_switch_workspace(wm, kb->num); break;
    case LM_ACT_MOVE_TO_WORKSPACE:
        if (f) move_to_workspace(wm, f, kb->num);
        break;
    case LM_ACT_FOCUS_NEXT: cycle_focus(wm, false); break;
    case LM_ACT_FOCUS_PREV: cycle_focus(wm, true); break;
    case LM_ACT_FULLSCREEN:
        if (f) lm_wm_window_request_fullscreen(f, !(f->states & LM_WINDOW_FULLSCREEN));
        break;
    case LM_ACT_MAXIMIZE_TOGGLE:
        if (f) lm_wm_window_request_maximize(f, !(f->states & LM_WINDOW_MAXIMIZED));
        break;
    case LM_ACT_MAXIMIZE:
        if (f) lm_wm_window_request_maximize(f, true);
        break;
    case LM_ACT_RESTORE:                        /* un-maximize, or un-tile back to the pre-tile geometry */
        if (f) {
            if (f->states & LM_WINDOW_MAXIMIZED)
                lm_wm_window_request_maximize(f, false);
            else
                lm_wm_window_request_tile(f, LM_EDGE_NONE);
        }
        break;
    case LM_ACT_TILE_LEFT: case LM_ACT_TILE_RIGHT:
        if (f) lm_wm_window_request_tile(f, kb->action == LM_ACT_TILE_LEFT ? LM_EDGE_LEFT : LM_EDGE_RIGHT);
        break;
    case LM_ACT_QUIT: request_quit(wm); break;
    case LM_ACT_RELOAD_CONFIG:
        if (wm->srv->reload_config)
            wm->srv->reload_config(wm->srv, false);
        break;
    case LM_ACT_NONE: break;
    }
    return true;
}

void lm_wm_key_capture(struct lm_wm *wm, bool on)
{
    wm->key_capture = on;
    if (on)
        wm->key_captured[0] = 0;
}

bool lm_wm_key_capturing(const struct lm_wm *wm) { return wm->key_capture; }
const char *lm_wm_key_captured(const struct lm_wm *wm) { return wm->key_captured; }

static bool is_modifier_key(uint32_t code)
{
    return code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT || code == KEY_LEFTCTRL || code == KEY_RIGHTCTRL ||
           code == KEY_LEFTALT || code == KEY_RIGHTALT || code == KEY_LEFTMETA || code == KEY_RIGHTMETA ||
           code == KEY_CAPSLOCK || code == KEY_NUMLOCK || code == KEY_SCROLLLOCK;
}

bool lm_wm_key(struct lm_wm *wm, uint32_t code, bool pressed, bool repeat, uint32_t mods)
{
    if (!repeat && code < 24 * 32 && (((wm->osk_phys[code / 32] >> (code % 32)) & 1u) != 0) != pressed) {
        wm->osk_phys[code / 32] ^= 1u << (code % 32);           /* the on-screen keyboard shows what is held on the real one */
        if (wm->osk_open)
            osk_damage(wm);
    }
    mods &= ~(uint32_t)(LM_MOD_CAPS | LM_MOD_NUM);
    if (wm->capture_release && code == wm->capture_release && !pressed) {
        wm->capture_release = 0;                                /* the release of the key that was just recorded */
        return true;
    }
    if (wm->key_capture && !is_modifier_key(code)) {
        if (pressed && !repeat) {
            char text[64];
            bool cancel = code == KEY_ESC && !(mods & LM_KEY_MODS_MASK);
            if (cancel || lm_keys_combo_text(mods & LM_KEY_MODS_MASK, code, text, sizeof(text))) {
                snprintf(wm->key_captured, sizeof(wm->key_captured), "%s", cancel ? "" : text);
                wm->key_capture = false;
                wm->capture_release = code;
                lm_settings_changed(wm->srv->settings);
            }
        }
        return true;
    }
    bool super = (mods & LM_MOD_SUPER) != 0;
    bool shift = (mods & LM_MOD_SHIFT) != 0;
    bool ctrl = (mods & LM_MOD_CTRL) != 0;
    bool alt = (mods & LM_MOD_ALT) != 0;

    if (wm->launcher_open && !(ctrl && alt)) {                  /* the launcher dialog is modal and takes every key */
        if (pressed && !is_modifier_key(code))
            launcher_key(wm, code, repeat, mods);
        return true;
    }
    if (wm->about_open && !(ctrl && alt)) {                     /* the About dialog is modal, too: Esc, Enter and Space close it */
        if (pressed && !repeat && (code == KEY_ESC || code == KEY_ENTER || code == KEY_KPENTER || code == KEY_SPACE))
            about_set(wm, false, NULL);
        return true;
    }
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
        bool arrow = code == KEY_UP || code == KEY_DOWN || code == KEY_HOME || code == KEY_END || code == KEY_PAGEUP || code == KEY_PAGEDOWN;
        bool del = code == KEY_DELETE;                          /* Delete forgets the highlighted entry, Shift+Delete the whole history */
        bool pick = code == KEY_ENTER || code == KEY_KPENTER;
        if (code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT || code == KEY_CAPSLOCK || code == KEY_NUMLOCK ||
            code == KEY_LEFTMETA || code == KEY_RIGHTMETA || code == KEY_LEFTCTRL || code == KEY_RIGHTCTRL ||
            code == KEY_LEFTALT || code == KEY_RIGHTALT)
            return false;                                       /* modifiers do not end the mode (the Super of Super+C is released) */
        if (!arrow && !pick && !del && code != KEY_ESC) {
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
        } else if (del) {
            if (shift)
                lm_seat_clip_clear(wm->srv->seat);
            else
                lm_seat_clip_remove(wm->srv->seat, LM_CLAMP(wm->clip_cur, 0, n - 1));   /* (the mode ends by itself when nothing is left) */
        } else {
            int page = LM_MAX(clip_slots(wm) - 1, 1);
            int cur = LM_CLAMP(wm->clip_cur, 0, n - 1);
            cur = code == KEY_UP ? LM_MAX(cur - 1, 0) : code == KEY_DOWN ? LM_MIN(cur + 1, n - 1) :
                  code == KEY_PAGEUP ? LM_MAX(cur - page, 0) : code == KEY_PAGEDOWN ? LM_MIN(cur + page, n - 1) :
                  code == KEY_HOME ? 0 : n - 1;
            if (cur != wm->clip_cur) {
                wm->clip_cur = cur;
                clip_show(wm, cur);
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
    const struct lm_keybind *kb = lm_keys_find(&wm->srv->config.keys, code, mods);
    return kb && run_keybind(wm, kb, pressed, repeat);
}
