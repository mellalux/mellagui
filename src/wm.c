/*
 * Window manager + shell policy. See wm.h for the contract.
 */
#include "metrics.h"
#include "keys.h"
#include "launchers.h"
#include "nightlight.h"
#include "notifylog.h"
#include "sound.h"
#include "menu.h"
#include "wm.h"

#include <ctype.h>
#include <dirent.h>
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
#define POWER_CARD_H 268                        /* (the card grows downwards: the upper row stays where it was) */
#define POWER_ROW1_H 172
#define POWER_BTN2_D 48                         /* the lower row's buttons */
#define POWER_BTN_D 64                          /* its round buttons */
#define ABOUT_CARD_W 520                        /* the About dialog */
#define ABOUT_ROW_H 18
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

/*
 * Window animations (metrics `animation-ms`, 0 = off; headless only with MELLAGUI_ANIMATIONS): a window that opens, closes,
 * is minimized or comes back is shown as a picture (struct lm_ghost, made by lm_render_snapshot) that grows / shrinks and
 * fades or flies to / from its taskbar button or dock icon, painted as LM_SCENE_GHOST above the windows. The real window
 * is left out of the scene while its opening / restoring picture runs (`anim_hidden`); a closed one is gone already.
 */
enum anim_kind { ANIM_OPEN, ANIM_CLOSE, ANIM_MINIMIZE, ANIM_RESTORE };
#define ANIM_MAX 8
#define ANIM_TICK_MS 16
struct wm_anim {
    bool used;
    enum anim_kind kind;
    struct lm_window *win;              /* OPEN / RESTORE: the window to show again at the end */
    struct lm_ghost ghost;
    struct lm_rect home;                /* where the picture is at full size (global) */
    struct lm_rect away;                /* MINIMIZE / RESTORE: the taskbar button or dock icon it flies to / from */
    uint64_t start;
    int dur;
    struct lm_rect shown;               /* painted now */
    int alpha;
};

/*
 * A dragged window wobbles: it is painted as a mesh of (WB_COLS x WB_ROWS) cells cut from its picture, whose nodes are
 * springs. Every node is pulled to where it would be if the window were rigid; the ones far from the grab point are softer, so
 * they stay behind when the window moves and swing back when it stops. The real window moves with the pointer at once (and is left
 * out of the scene meanwhile); the picture is taken again now and then so the content stays alive.
 */
#define WB_COLS 14
#define WB_ROWS 32
#define WB_N ((WB_COLS + 1) * (WB_ROWS + 1))
struct wm_wobble {
    bool active;
    bool released;                      /* the button is up: it settles and ends */
    struct lm_window *win;
    struct lm_ghost ghost;
    int off_x, off_y;                   /* the picture's top left relative to the window's (x, y) */
    float gx, gy;                       /* the grab point, relative to the picture's top left */
    float x[WB_N], y[WB_N], vx[WB_N], vy[WB_N];     /* node positions (global) and velocities */
    uint64_t last;
    int ticks;
    struct lm_rect shown;               /* the union of the cells painted last */
};

/* The launcher dialog opens as a bubble that rises out of its dock button and takes the shape of the card, and closes the other way round. The card's
 * picture is a mesh of BUB_C x BUB_R cells (two triangles each) whose corners are moved by bubble_nodes(); every tick the triangles are rasterised into one
 * picture (`frame`, bilinear, from a chain of smaller copies of the card so that a small bubble does not glitter) that the scene shows as one ghost: no
 * seams, no gaps. A highlight (the sheen) lies on the bubble. */
#define BUB_C 36
#define BUB_R 24
#define BUB_N ((BUB_C + 1) * (BUB_R + 1))
#define BUB_LEVELS 5
struct wm_bubble {
    bool active, closing;
    float prog;                         /* 0 = a small bubble at the button, 1 = the card */
    uint64_t last;
    struct lm_ghost ghost;              /* the card as a picture */
    uint32_t *mip[BUB_LEVELS];          /* the card halved again and again (level 0 is ghost.px, not owned) */
    int mw[BUB_LEVELS], mh[BUB_LEVELS];
    struct lm_ghost frame;              /* the mesh as it is now, one picture covering `shown` */
    int alpha;                          /* how solid it is painted (0..255) */
    float s;                            /* how far it has spread out (0 bubble .. 1 card) */
    struct lm_ghost sheen;              /* the highlight on the bubble */
    struct lm_rect card;                /* where the card is (global) */
    float cx0, cy0;                     /* the button: where the bubble comes from / goes to */
    struct lm_rect shown;               /* the rectangle covered at the last tick */
};

#define OV_MAX_ITEMS LM_OVERVIEW_MAX
struct wm_ov_item {
    struct lm_window *win;
    struct lm_ghost ghost;
    struct lm_rect home;                /* where its picture is at full size (the window, global) */
    struct lm_rect slot;                /* ... and in the grid */
};
struct wm_overview {
    bool open, closing;
    char out[32];                       /* the output it is shown on */
    int n, cols;
    struct wm_ov_item item[LM_OVERVIEW_MAX];
    float prog, from;                   /* 0 = the windows where they are, 1 = in the grid */
    uint64_t start;
    int dur;
    int sel, hover, hover_ws, press;
    bool drag;                          /* a picture is being carried (towards a workspace cell) ... */
    int drag_i;                         /* ... this one; the pointer is wm->drag_x / drag_y */
};

struct lm_wm {
    struct lm_server *srv;
    struct wm_anim anim[ANIM_MAX];
    struct wm_overview ov;
    bool anim_suppress;                 /* no fly-in / fly-out animations just now (the overview brings a window back itself) */
    struct wm_wobble wb;
    struct wm_bubble bubble;
    struct lm_timer *anim_timer;
    struct lm_window *glide_win;        /* a window that glides to glide_tx / glide_ty (dragged, or let go a moment ago) */
    int32_t glide_tx, glide_ty;
    uint64_t glide_last;
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
    bool hover_power, hover_layout, hover_sticky, hover_osk;
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
    bool menu_open;             /* a menu is showing in the card menu_rect: the dock icon's (owner 0, for dock item menu_dock_item) or a program's (owner 1) */
    struct lm_window *menu_window;      /* owner 3: the window whose title bar / taskbar button was right-clicked */
    int menu_owner, menu_dock_item, menu_sel, menu_n;
    struct lm_rect menu_rect;
    char menu_title[LM_DOCK_NAME_MAX], menu_label[LM_MENU_MAX][48];
    bool menu_enab[LM_MENU_MAX];
    bool power_open;            /* the session dialog (Lock / Exit) is showing on the output called power_out */
    char power_out[32];
    int power_sel;              /* its highlighted button: 0 Lock, 1 Exit, ... -1 none yet */
    int power_others;           /* ... and this many other login sessions are open (logind) */
    int power_confirm;          /* the dialog asks "are you sure?" about this action (LM_LOCK_ACT_*), 0 none */
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
    int64_t settings_spawn_ms;  /* when the settings panel was last started (its window may not be there yet) */
    int launcher_caret[3];      /* the text cursor of the form's name / command / icon field (a byte offset; -1 = the end) */
    int launcher_help;          /* 0 none, else the form field (+ 1) whose help card is open */
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
    float dock_vis;                     /* autohide: how far the dock is out, 0 = gone .. 1 = there; it melts away / rises over a few seconds (dock_info) */
    int64_t dock_vis_ms;                /* the time of the last step of dock_vis */
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
static void move_to_workspace(struct lm_wm *wm, struct lm_window *w, int ws);
static void window_menu_activate(struct lm_wm *wm, int which);
static void menu_close(struct lm_wm *wm, bool chosen);
static void menu_set(struct lm_wm *wm, bool open);
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
    p->sticky = lm_rect_make(p->clock.x - 8 - PANEL_STICKY_W, y, PANEL_STICKY_W, h);         /* (the Run dialog is Super+R and the launcher's Run button) */
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

static void notify_card(struct lm_wm *wm, const char *title, const char *body, int timeout_ms);

/* A notification: kept in the history (notifylog.h) and, unless "do not disturb" is on, shown as a card. */
void lm_wm_notify(struct lm_wm *wm, const char *title, const char *body, int timeout_ms)
{
    if (!wm)
        return;
    if (wm->srv->notelog) {
        lm_notelog_add(wm->srv->notelog, title, body, time(NULL));
        if (wm->srv->settings)
            lm_settings_changed(wm->srv->settings);                    /* the readers of notes-N / notes-unread */
    }
    if (!wm->srv->config.dnd)
        notify_card(wm, title, body, timeout_ms);
}

/* The same without the history and whatever "do not disturb" says: feedback for the user's own switch. */
void lm_wm_notify_now(struct lm_wm *wm, const char *title, const char *body, int timeout_ms)
{
    if (wm)
        notify_card(wm, title, body, timeout_ms);
}

static void notify_card(struct lm_wm *wm, const char *title, const char *body, int timeout_ms)
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
    menu_set(wm, false);
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
static void menu_set(struct lm_wm *wm, bool open);
static void about_set(struct lm_wm *wm, bool open, struct lm_output *o);
static void about_damage(struct lm_wm *wm);
static void launcher_set(struct lm_wm *wm, bool open, struct lm_output *o);
static void dock_activate(struct lm_wm *wm, int i, bool reuse);
static void dock_launch(struct lm_wm *wm, const struct lm_dock_item *it, int i, bool reuse);
static void dock_hide_tick(void *data);
static int anim_duration(const struct lm_wm *wm);
static void dock_anim_tick(void *data);
static void anim_tick(void *data);
static bool anim_prepare(struct lm_wm *wm, struct lm_window *w, struct lm_ghost *g, struct lm_rect *home);
static void anim_start(struct lm_wm *wm, enum anim_kind kind, struct lm_window *w, struct lm_ghost *g, const struct lm_rect *home);
static void anim_finish_all(struct lm_wm *wm);
static void anim_forget(struct lm_wm *wm, struct lm_window *w);
static bool anim_glide_to(struct lm_wm *wm, struct lm_window *w, int32_t x, int32_t y);
static void wobble_start(struct lm_wm *wm, struct lm_window *w, double px, double py);
static void wobble_stop(struct lm_wm *wm);
static bool ov_hides(const struct lm_wm *wm, const struct lm_window *w);
static void overview_changed(struct lm_wm *wm);
static void overview_finish(struct lm_wm *wm);
static bool overview_tick(struct lm_wm *wm, uint64_t now);
static void overview_key(struct lm_wm *wm, uint32_t code, bool repeat, uint32_t mods);
static bool overview_pick(const struct lm_wm *wm, int ix, int iy, struct lm_pick *out);
static void overview_click(struct lm_wm *wm, const struct lm_pick *pick, uint32_t button);
static void overview_set(struct lm_wm *wm, bool open);
static struct lm_output *overview_output(const struct lm_wm *wm);
static void overview_damage(struct lm_wm *wm);
static void wobble_tick(struct lm_wm *wm, uint64_t now);
static bool wobble_wanted(const struct lm_wm *wm, const struct lm_window *w);
static void bubble_begin(struct lm_wm *wm, bool opening);
static void bubble_finish(struct lm_wm *wm);
static bool bubble_tick(struct lm_wm *wm, uint64_t now);
static void bubble_end(struct lm_wm *wm);
static void anim_open(struct lm_wm *wm, struct lm_window *w);
static void anim_fly(struct lm_wm *wm, enum anim_kind kind, struct lm_window *w);
static void anim_remember_close(struct lm_wm *wm, struct lm_window *w);
static void anim_play_close(struct lm_wm *wm, struct lm_window *w);

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
    anim_finish_all(wm);
    wm->workspace = ws;
    struct lm_window *w;
    lm_list_for_each(w, &wm->windows, link)
        update_outputs(wm, w);
    set_focus(wm, NULL);
    set_focus(wm, top_visible(wm, NULL));
    overview_changed(wm);
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
        anim_fly(wm, ANIM_RESTORE, w);                           /* (a picture flies from the button; the window itself is hidden meanwhile) */
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
    wm->anim_timer = lm_loop_add_timer(srv->loop, anim_tick, wm);
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
    for (int i = 0; i < ANIM_MAX; i++)
        if (wm->anim[i].used)
            free(wm->anim[i].ghost.px);
    free(wm->wb.ghost.px);
    for (int i = 0; i < wm->ov.n; i++)
        free(wm->ov.item[i].ghost.px);
    if (wm->anim_timer)
        lm_timer_destroy(wm->anim_timer);
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
    wm->dock_vis = lm_dock.autohide ? 0.0f : 1.0f;
    wm->dock_drag = false;
    wm->hover_dock = -1;
    memset(wm->dock_launch_ms, 0, sizeof(wm->dock_launch_ms));         /* (the buttons may be others now) */
    menu_set(wm, false);
    lm_wm_output_changed(wm, NULL);
}

void lm_wm_output_changed(struct lm_wm *wm, struct lm_output *out)
{
    (void)out;
    struct lm_window *w;
    lm_list_for_each(w, &wm->windows, link)
        if (w->states & (LM_WINDOW_MAXIMIZED | LM_WINDOW_FULLSCREEN))
            apply_state(wm, w);
    if (wm->ov.open && !overview_output(wm))
        overview_finish(wm);                                    /* its output went away */
    else
        overview_changed(wm);
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
    if (wm->menu_open && wm->menu_owner == 3 && wm->menu_window == w)
        menu_close(wm, true);                                  /* the menu is of this window */
    sw_close(wm);                                               /* the list holds window pointers */
    struct lm_window *c;
    lm_list_for_each(c, &wm->windows, link)
        if (c->parent == w)
            c->parent = NULL;
    if (w->mapped)
        lm_wm_window_unmap(w);
    anim_forget(wm, w);
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
    if (wm->ov.open)
        overview_changed(wm);                                   /* (its picture joins the overview) */
    else
        anim_open(wm, w);
    refresh_pointer(wm);
}

void lm_wm_window_unmap(struct lm_window *w)
{
    struct lm_wm *wm = w->wm;
    if (!w->mapped)
        return;
    sw_close(wm);
    anim_play_close(wm, w);
    struct lm_rect r = frame_outer(w);
    lm_xdg_dismiss_popups(w->surface);
    w->mapped = false;
    lm_surface_set_outputs(w->surface, NULL, 0);
    if (wm->focused == w)
        set_focus(wm, top_visible(wm, w));
    damage_rect(wm, &r);
    damage_panels(wm);
    overview_changed(wm);
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
    anim_fly(wm, ANIM_MINIMIZE, w);
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
    anim_remember_close(w->wm, w);
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

/* Suspend makes no sense on a server: `[commands] suspend = none` (or empty) turns the button off. */
static bool power_suspend_off(const struct lm_wm *wm)
{
    const char *c = wm->srv->config.suspend_cmd;
    return !c || !*c || !strcmp(c, "none");
}

/* How many login sessions other than ours logind knows about (a server may have people on ssh): /run/systemd/sessions has one file per session. */
static int other_sessions(void)
{
    DIR *d = opendir("/run/systemd/sessions");
    if (!d)
        return 0;
    const char *mine = getenv("XDG_SESSION_ID");
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        const char *nm = e->d_name;
        bool digits = *nm != 0;
        for (const char *p = nm; *p; p++)
            if (*p < '0' || *p > '9')
                digits = false;
        if (digits && (!mine || strcmp(mine, nm)))
            n++;
    }
    closedir(d);
    return n;
}

static void power_info(const struct lm_wm *wm, struct lm_output *o, struct lm_power_info *pi)
{
    memset(pi, 0, sizeof(*pi));
    pi->rect = lm_output_rect(o);
    pi->card = lm_rect_make(pi->rect.x + (pi->rect.w - POWER_CARD_W) / 2, pi->rect.y + (pi->rect.h - POWER_ROW1_H) / 2,
                            POWER_CARD_W, POWER_CARD_H);
    int gap = 40, total = 2 * POWER_BTN_D + gap;
    int x = pi->card.x + (POWER_CARD_W - total) / 2, y = pi->card.y + 46;
    for (int i = 0; i < 2; i++) {
        pi->button[i] = lm_rect_make(x + i * (POWER_BTN_D + gap), y, POWER_BTN_D, POWER_BTN_D);
        pi->hit[i] = lm_rect_make(pi->button[i].x - 8, y, POWER_BTN_D + 16, POWER_BTN_D + 8 + LM_FONT_H + 4);
    }
    int gap2 = 36, total2 = 3 * POWER_BTN2_D + 2 * gap2;                 /* the lower row: Suspend, Restart, Shutdown */
    int x2 = pi->card.x + (POWER_CARD_W - total2) / 2, y2 = pi->card.y + 156;
    for (int i = 0; i < 3; i++) {
        pi->button[2 + i] = lm_rect_make(x2 + i * (POWER_BTN2_D + gap2), y2, POWER_BTN2_D, POWER_BTN2_D);
        pi->hit[2 + i] = lm_rect_make(pi->button[2 + i].x - 8, y2, POWER_BTN2_D + 16, POWER_BTN2_D + 8 + LM_FONT_H + 4);
    }
    pi->sel = wm->power_sel;
    pi->confirm = wm->power_confirm;
    pi->others = wm->power_others;
    pi->off[2] = power_suspend_off(wm);
    if (pi->off[2])
        pi->hit[2] = (struct lm_rect){ 0 };
    if (wm->power_confirm)                                       /* only Yes and Cancel (the upper row's places) are there */
        for (int i = 2; i < LM_POWER_N; i++)
            pi->button[i] = pi->hit[i] = (struct lm_rect){ 0 };
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
    if (open)
        menu_set(wm, false);
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
        wm->power_confirm = 0;
        wm->power_sel = -1;
        power_damage(wm);
    } else {
        power_damage(wm);                                       /* (while the output is still found) */
        wm->power_open = false;
        wm->power_confirm = 0;
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
    case LM_OSK_SUPER: return osk_phys_down(wm, KEY_LEFTMETA) || osk_phys_down(wm, KEY_RIGHTMETA);
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

static const enum lm_osk_kind osk_mod_kinds[5] = { LM_OSK_SHIFT, LM_OSK_CTRL, LM_OSK_ALT, LM_OSK_ALTGR, LM_OSK_SUPER };

/* The held key comes up (and the modifiers that were pressed with it): the pointer let go, or the keyboard closes. */
static void osk_release(struct lm_wm *wm)
{
    if (wm->osk_down < 0)
        return;
    wm->osk_down = -1;
    osk_inject(wm, wm->osk_down_code, false);
    for (int i = 4; i >= 0; i--)
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
        for (int m = 0; m < 5; m++)
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
    if (open)
        menu_set(wm, false);
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

/* The dock's buttons that are not programs: the one that opens this dialog and the one that opens the desktop overview. */
static bool dock_special(const struct lm_dock_item *it)
{
    return launcher_is_self(it) || !strcmp(it->cmd, "@overview");
}

/* Search: what is typed in the dialog filters the icons by name (one wm, so file scope). */
static char launcher_filter[48];
static bool launcher_searching;

static bool launcher_shown(const struct lm_dock_item *it)
{
    return !launcher_is_self(it) && (!launcher_filter[0] || strcasestr(it->name, launcher_filter));          /* (the Overview button shows: it is a button like the others) */
}

static int launcher_count(void)
{
    int n = 0;
    for (int i = 0; i < lm_dock_count(); i++)
        if (launcher_shown(lm_dock_item(i)))
            n++;
    for (int i = 0; i < lm_launchapps_count(); i++)
        if (launcher_shown(lm_launchapps_item(i)))
            n++;
    return n;
}

/* The g-th button of the launcher; *dock_index is its index in the dock, -1 for one that is only in the launcher. NULL when there is none. */
static const struct lm_dock_item *launcher_item(int g, int *dock_index)
{
    if (dock_index)
        *dock_index = -1;
    for (int i = 0; i < lm_dock_count(); i++)
        if (launcher_shown(lm_dock_item(i)) && g-- == 0) {
            if (dock_index)
                *dock_index = i;
            return lm_dock_item(i);
        }
    for (int i = 0; i < lm_launchapps_count(); i++)
        if (launcher_shown(lm_launchapps_item(i)) && g-- == 0)
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

/* ---- the text cursor of the form's fields ---- */

static int text_width_to(const char *text, int from, int to)
{
    char tmp[LM_DOCK_CMD_MAX];
    int n = LM_MIN(to - from, (int)sizeof(tmp) - 1);
    memcpy(tmp, text + from, (size_t)LM_MAX(n, 0));
    tmp[LM_MAX(n, 0)] = 0;
    return lm_font_text_width(tmp);
}

static int utf8_prev(const char *t, int i)
{
    if (i > 0)
        i--;
    while (i > 0 && ((unsigned char)t[i] & 0xc0) == 0x80)
        i--;
    return i;
}

static int utf8_next(const char *t, int i, int len)
{
    if (i < len)
        i++;
    while (i < len && ((unsigned char)t[i] & 0xc0) == 0x80)
        i++;
    return i;
}

void lm_launcher_field_view(const char *text, int caret, int avail, int *start, int *end)
{
    int len = (int)strlen(text), st = 0;
    caret = LM_CLAMP(caret, 0, len);
    while (st < caret && text_width_to(text, st, caret) > avail - 2)       /* scrolled so that the caret shows */
        st = utf8_next(text, st, len);
    int en = st;
    while (en < len) {
        int nx = utf8_next(text, en, len);
        if (text_width_to(text, st, nx) > avail)
            break;
        en = nx;
    }
    *start = st;
    *end = en;
}

/* The caret of field f, always on a character boundary inside the text (-1 = the end). */
static int lf_caret(const struct lm_wm *wm, int f, const char *text)
{
    int len = (int)strlen(text), c = wm->launcher_caret[f];
    if (c < 0 || c > len)
        c = len;
    while (c > 0 && ((unsigned char)text[c] & 0xc0) == 0x80)
        c--;
    return c;
}

static bool is_word_char(unsigned char c)
{
    return isalnum(c) || c >= 0x80 || c == '_';
}

static int lf_word_prev(const char *t, int i)
{
    while (i > 0 && !is_word_char((unsigned char)t[i - 1]))
        i--;
    while (i > 0 && is_word_char((unsigned char)t[i - 1]))
        i--;
    return i;
}

static int lf_word_next(const char *t, int i, int len)
{
    while (i < len && !is_word_char((unsigned char)t[i]))
        i++;
    while (i < len && is_word_char((unsigned char)t[i]))
        i++;
    return i;
}

/* Puts `s` (n bytes) into text at the caret; false when it does not fit. The caret moves behind it. */
static bool lf_insert(char *text, size_t max, int *caret, const char *s, size_t n)
{
    size_t len = strlen(text);
    if (len + n >= max)
        return false;
    memmove(text + *caret + n, text + *caret, len - (size_t)*caret + 1);
    memcpy(text + *caret, s, n);
    *caret += (int)n;
    return true;
}

static void lf_erase(char *text, int from, int to)
{
    memmove(text + from, text + to, strlen(text + to) + 1);
}

static void launcher_info(const struct lm_wm *wm, struct lm_output *o, struct lm_launcher_info *li)
{
    memset(li, 0, sizeof(*li));
    li->rect = lm_output_rect(o);
    li->veil = 1.0f;
    li->form = wm->launcher_form;
    li->editing = wm->launcher_form && wm->launcher_editing;
    li->edit = wm->launcher_edit && !li->form;
    li->hover_button = wm->launcher_hover_btn;
    li->message = wm->launcher_msg;
    li->focus = wm->launcher_focus;
    li->kind = launcher_kind_of(wm);
    li->kind_name = li->kind >= 0 ? lm_dockicon_name(li->kind) : li->kind == -2 ? "yours" : "auto";
    int n = launcher_count(), cells = n + (launcher_can_add() && !launcher_searching ? 1 : 0);
    li->search = launcher_filter;
    li->searching = launcher_searching && !li->form;
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
    static const char hint_grid[] = "Enter starts   E edit   A add   Type to search   Esc closes", hint_edit[] = "Click an icon to change it   red x removes   E or Done ends   Esc closes",
                      hint_form[] = "Tab next field   Left / Right cursor (or icon)   Enter adds   Esc back",
                      hint_change[] = "Tab next field   Left / Right cursor (or icon)   Enter saves   Esc back";
    static const char hint_search[] = "Type to search   Enter starts   Esc clears";
    li->hint = li->form ? (li->editing ? hint_change : hint_form) : li->searching ? hint_search : li->edit ? hint_edit : hint_grid;
    if (li->form) {
        li->field_label[0] = "Name";
        li->field_label[1] = "Command   (@terminal, mellagui-*, ~/prog, a || b)";
        li->field_label[2] = "Icon   (empty = guessed, theme:firefox, a file, or pick below)";
        li->field_text[0] = wm->launcher_name;
        li->field_text[1] = wm->launcher_cmd;
        li->field_text[2] = wm->launcher_icon;
        for (int i = 0; i < 3; i++)
            li->caret[i] = lf_caret(wm, i, li->field_text[i]);
        int fx = li->card.x + LC_PAD, fw = li->card.w - 2 * LC_PAD;
        li->field[0] = lm_rect_make(fx, li->card.y + 60 + LM_FONT_H + 4, fw, 30);
        li->field[1] = lm_rect_make(fx, li->card.y + 126 + LM_FONT_H + 4, fw, 30);
        li->field[2] = lm_rect_make(fx, li->card.y + 192 + LM_FONT_H + 4, fw, 30);
        for (int i = 0; i < 3; i++)                                     /* the "?" at the end of each field */
            li->help_btn[i] = lm_rect_make(li->field[i].x + li->field[i].w - LM_LAUNCHER_HELP_D - 8, li->field[i].y + (li->field[i].h - LM_LAUNCHER_HELP_D) / 2,
                                           LM_LAUNCHER_HELP_D, LM_LAUNCHER_HELP_D);
        li->help = wm->launcher_help;
        if (li->help >= 1 && li->help <= 3) {
            static const char *const titles[3] = { "Name", "Command", "Icon" };
            static const char *const texts[3] = {
                "The name of the button. It is written under its icon and the search finds it by it. Every button needs a name of its own (capital "
                "and small letters do not count as different).",
                "The program to start. @terminal, @run, @settings ... are MellaGUI's own programs, mellagui-* are found next to MellaGUI, ~/prog is in "
                "your home folder. 'a || b' tries a and, if it is not installed, b. '@terminal -e alpine' runs a text program in a terminal. "
                "env VAR=value comes before the program.",
                "Empty: guessed from the name. A built-in icon: pick one below or type its name. theme:NAME takes an installed program's own icon "
                "(theme:firefox, or theme:vscode|code to try several names). Or the path of a PNG or JPEG picture; a logo with transparent corners "
                "gets a round plate.",
            };
            li->help_title = titles[li->help - 1];
            li->help_text = texts[li->help - 1];
            li->help_card = lm_rect_make(li->card.x + 50, li->card.y + 86, li->card.w - 100, 210);
            li->help_close = lm_rect_make(li->help_card.x + (li->help_card.w - 90) / 2, li->help_card.y + li->help_card.h - 42, 90, 28);
        }
        int kgap = 5, ksz = LM_MIN(LC_KIND, (li->card.w - 2 * LC_PAD - (LM_LAUNCHER_KIND_N - 1) * kgap) / LM_LAUNCHER_KIND_N);       /* all the icon choices fit the card */
        int total = LM_LAUNCHER_KIND_N * ksz + (LM_LAUNCHER_KIND_N - 1) * kgap;
        int kx = li->card.x + (li->card.w - total) / 2, ky = li->card.y + 286;
        for (int i = 0; i < LM_LAUNCHER_KIND_N; i++)
            li->kind_box[i] = lm_rect_make(kx + i * (ksz + kgap), ky, ksz, ksz);
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
    if (open)
        menu_set(wm, false);
    if (open == wm->launcher_open)
        return;
    if (open) {
        if (!o)
            o = lm_output_primary(wm->srv);
        if (!o)
            return;
        launcher_filter[0] = 0;
        launcher_searching = false;
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
        if (wm->bubble.active && wm->bubble.closing)
            wm->bubble.closing = false;                          /* it was on its way back to the button: turns round */
        else
            bubble_begin(wm, true);
    } else {
        if (wm->bubble.active && !wm->bubble.closing) {
            wm->bubble.closing = true;                           /* still rising: it goes back from where it is */
            wm->bubble.last = lm_now_ms();
        } else {
            bubble_begin(wm, false);                             /* (the picture of the card as it is now) */
        }
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
    n += (size_t)snprintf(buf, size, "launcher open=%d edit=%d form=%d n=%d sel=%d drag=%d droppos=%d editing=%d searching=%d search=\"%s\"\n", o != NULL, o ? li.edit : 0,
                          o ? li.form : 0, o ? li.n : 0, o ? li.sel : -1, o ? li.drag : -1, o ? li.drop_pos : -1, o ? li.editing : 0,
                          o ? li.searching : 0, o ? li.search : "");
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
        for (int i = 0; i < 3; i++)
            n += (size_t)snprintf(buf + n, size - n, "caret %d %d\nhelp %d x=%d y=%d w=%d h=%d\n", i, li.caret[i], i, LCR(li.help_btn[i]));
        n += (size_t)snprintf(buf + n, size - n, "helpcard open=%d x=%d y=%d w=%d h=%d close=%d,%d,%d,%d title=\"%s\"\n", li.help, LCR(li.help_card), LCR(li.help_close),
                              li.help_title ? li.help_title : "");
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
    if (li.form && li.help) {                                       /* a help card is open: its Close button, a click elsewhere also closes it */
        out->kind = lm_rect_contains(&li.help_card, ix, iy) && !lm_rect_contains(&li.help_close, ix, iy) ? LM_PICK_LAUNCHER_CARD : LM_PICK_LAUNCHER_HELPCLOSE;
        return true;
    }
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
            if (lm_rect_contains(&li.help_btn[i], ix, iy)) {
                out->kind = LM_PICK_LAUNCHER_HELP;
                out->index = i;
                return true;
            }
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
    launcher_filter[0] = 0;                                         /* (the "+" tile and the counts want the whole list) */
    launcher_searching = false;
    wm->launcher_form = true;
    wm->launcher_editing = false;
    wm->launcher_name[0] = wm->launcher_cmd[0] = wm->launcher_msg[0] = 0;
    wm->launcher_icon[0] = 0;
    wm->launcher_focus = 0;
    wm->launcher_caret[0] = wm->launcher_caret[1] = wm->launcher_caret[2] = -1;
    wm->launcher_help = 0;
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
    wm->launcher_caret[0] = wm->launcher_caret[1] = wm->launcher_caret[2] = -1;
    wm->launcher_help = 0;
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
    case LM_PICK_LAUNCHER_FIELD: {                                    /* the caret goes where the pointer is */
        wm->launcher_focus = index;
        struct lm_output *lo = launcher_output(wm);
        double px = 0, py = 0;
        if (lo && wm->srv->seat) {
            lm_seat_pointer_position(wm->srv->seat, &px, &py);
            struct lm_launcher_info li;
            launcher_info(wm, lo, &li);
            const char *text = li.field_text[index];
            int avail = li.field[index].w - 20 - LM_LAUNCHER_HELP_D - 8, st, en;
            lm_launcher_field_view(text, li.caret[index], avail, &st, &en);
            int tx = li.field[index].x + 10, best = st, bestd = 1 << 30;
            for (int b = st; b <= en;) {
                int d = abs(tx + text_width_to(text, st, b) - (int)px);
                if (d < bestd) {
                    bestd = d;
                    best = b;
                }
                if (b >= en)
                    break;
                b = utf8_next(text, b, (int)strlen(text));
            }
            wm->launcher_caret[index] = best;
        }
        launcher_damage(wm);
        break;
    }
    case LM_PICK_LAUNCHER_HELP:
        wm->launcher_help = index + 1;
        wm->launcher_focus = index;
        launcher_damage(wm);
        break;
    case LM_PICK_LAUNCHER_HELPCLOSE:
        wm->launcher_help = 0;
        launcher_damage(wm);
        break;
    case LM_PICK_LAUNCHER_KIND:                                       /* a built-in icon: its name goes into the icon text, "auto" empties it */
        snprintf(wm->launcher_icon, sizeof(wm->launcher_icon), "%s", index >= 0 ? lm_dockicon_name(index) : "");
        wm->launcher_caret[2] = -1;
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

/* The text of the form field that has the focus (name, command, icon) and its size; NULL while the icon row has it. */
static char *launcher_field_text(struct lm_wm *wm, size_t *max)
{
    switch (wm->launcher_focus) {
    case 0: *max = sizeof(wm->launcher_name); return wm->launcher_name;
    case 1: *max = sizeof(wm->launcher_cmd); return wm->launcher_cmd;
    case 2: *max = sizeof(wm->launcher_icon); return wm->launcher_icon;
    default: return NULL;
    }
}

/* Clipboard actions on the focused field of the form: 0 cut, 1 copy (both take the whole field), 2 paste (the newest copied text as one line, on
 * the end), 3 clear. The compositor keeps the clipboard history itself, so it can read the text without asking a program. */
static void launcher_field_clip(struct lm_wm *wm, int what)
{
    size_t max;
    char *text = launcher_field_text(wm, &max);
    if (!text || !wm->srv->seat)
        return;
    if (what == 0 || what == 1) {
        if (text[0]) {
            lm_seat_clip_add(wm->srv->seat, text);
            lm_seat_clip_use(wm->srv->seat, 0);
        }
        if (what == 0) {
            text[0] = 0;
            wm->launcher_caret[wm->launcher_focus] = 0;
        }
    } else if (what == 2) {                                                  /* paste: at the caret, as one line */
        const char *c = lm_seat_clip_count(wm->srv->seat) > 0 ? lm_seat_clip_get(wm->srv->seat, 0) : NULL;
        int caret = lf_caret(wm, wm->launcher_focus, text);
        for (; c && *c; c++) {
            unsigned char ch = (unsigned char)*c;
            if (ch == '\n' || ch == '\r' || ch == '\t')
                ch = ' ';
            if (ch < 0x20 || ch == 0x7f)
                continue;
            if ((ch & 0xc0) != 0x80 && ch >= 0xc0) {                         /* a multi-byte character goes in whole or not at all */
                size_t need = ch >= 0xf0 ? 4 : ch >= 0xe0 ? 3 : 2;
                if (strlen(text) + need >= max)
                    break;
            }
            char one = (char)ch;
            if (!lf_insert(text, max, &caret, &one, 1))
                break;
        }
        wm->launcher_caret[wm->launcher_focus] = caret;
    } else {
        text[0] = 0;
        wm->launcher_caret[wm->launcher_focus] = 0;
    }
    wm->launcher_msg[0] = 0;
    launcher_damage(wm);
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
    if (wm->bubble.active && !wm->bubble.closing && code != KEY_ESC && !(code == KEY_A && (mods & LM_MOD_SUPER))) {     /* still rising: a key only finishes it */
        bubble_finish(wm);
        return;
    }
    struct lm_launcher_info li;
    launcher_info(wm, lo, &li);
    bool shift = (mods & LM_MOD_SHIFT) != 0;
    if (code == KEY_A && (mods & LM_MOD_SUPER) && !(mods & ~(uint32_t)LM_MOD_SUPER & LM_KEY_MODS_MASK)) {   /* Super+A again closes it */
        if (!repeat)
            launcher_set(wm, false, NULL);
        return;
    }
    if (wm->launcher_form && wm->launcher_help) {                   /* a help card is open: Esc / Enter / Space close it, nothing else is typed */
        if (!repeat && (code == KEY_ESC || code == KEY_ENTER || code == KEY_KPENTER || code == KEY_SPACE)) {
            wm->launcher_help = 0;
            launcher_damage(wm);
        }
        return;
    }
    bool arrow = code == KEY_LEFT || code == KEY_RIGHT || code == KEY_UP || code == KEY_DOWN;
    if (!wm->launcher_form) {
        int cells = li.n + (li.add.w > 0 ? 1 : 0);
        keymap_sync(wm);
        uint32_t sks = lm_keymap_keysym(code, mods);
        char stxt[8];
        int stl = sks ? lm_keysym_to_utf8(sks, stxt) : 0;
        bool splain = !(mods & (LM_MOD_CTRL | LM_MOD_ALT | LM_MOD_SUPER));
        bool sprint = splain && stl == 1 && (unsigned char)stxt[0] >= 0x20 && (unsigned char)stxt[0] < 0x7f;
        bool changed = false;
        if (launcher_searching) {                                       /* search mode: the letters go into the text */
            size_t l = strlen(launcher_filter);
            if (code == KEY_BACKSPACE) {
                launcher_pop(launcher_filter);
                changed = true;
            } else if (code == KEY_ESC) {
                if (repeat)
                    return;
                if (l)
                    launcher_filter[0] = 0;
                else
                    launcher_searching = false;
                changed = true;
            } else if (sprint && l + 1 < sizeof(launcher_filter)) {
                launcher_filter[l] = stxt[0];
                launcher_filter[l + 1] = 0;
                changed = true;
            }
        } else if (!repeat && ((sprint && code != KEY_E && code != KEY_A && code != KEY_N && code != KEY_SPACE) ||
                               (code == KEY_F && (mods & LM_MOD_CTRL) && !(mods & (LM_MOD_ALT | LM_MOD_SUPER))))) {
            launcher_searching = true;                                  /* typing a letter other than the shortcuts (E A N) starts a search; "/" and Ctrl+F too */
            launcher_filter[0] = 0;
            if (sprint && code != KEY_SLASH) {
                launcher_filter[0] = stxt[0];
                launcher_filter[1] = 0;
            }
            changed = true;
        }
        if (changed) {
            wm->launcher_sel = launcher_count() > 0 ? 0 : -1;
            launcher_damage(wm);
            return;
        }
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
        if (wm->launcher_focus < 3)
            wm->launcher_caret[wm->launcher_focus] = -1;                /* (a field entered with the keyboard has the caret at its end) */
        launcher_damage(wm);
        return;
    }
    if (code == KEY_ENTER || code == KEY_KPENTER) {
        if (repeat)
            return;
        if (wm->launcher_focus == 0) {
            wm->launcher_focus = 1;
            wm->launcher_caret[1] = -1;
            launcher_damage(wm);
        } else {
            launcher_submit(wm);
        }
        return;
    }
    if (!(mods & (LM_MOD_ALT | LM_MOD_SUPER))) {                    /* the clipboard keys of a text field */
        bool ctl = (mods & LM_MOD_CTRL) != 0;
        if ((ctl && code == KEY_V) || (shift && !ctl && code == KEY_INSERT)) {
            launcher_field_clip(wm, 2);
            return;
        }
        if ((ctl && code == KEY_C) || (ctl && !shift && code == KEY_INSERT)) {
            launcher_field_clip(wm, 1);
            return;
        }
        if ((ctl && !shift && code == KEY_X) || (shift && !ctl && code == KEY_DELETE)) {
            launcher_field_clip(wm, 0);
            return;
        }
    }
    if (wm->launcher_focus == 3) {
        if (code == KEY_LEFT || code == KEY_RIGHT) {
            int k = LM_MAX(launcher_kind_of(wm), -1) + (code == KEY_LEFT ? -1 : 1);
            k = LM_CLAMP(k, -1, LM_DOCKICON_COUNT - 1);
            snprintf(wm->launcher_icon, sizeof(wm->launcher_icon), "%s", k >= 0 ? lm_dockicon_name(k) : "");
            wm->launcher_caret[2] = -1;
            launcher_damage(wm);
        }
        return;
    }
    size_t max = 0;
    char *text = launcher_field_text(wm, &max);
    if (!text)
        return;
    int f = wm->launcher_focus, len = (int)strlen(text), caret = lf_caret(wm, f, text);
    bool ctrl = (mods & LM_MOD_CTRL) != 0;
    if (code == KEY_LEFT) {                                          /* the caret: a character, with Ctrl a word */
        caret = ctrl ? lf_word_prev(text, caret) : utf8_prev(text, caret);
    } else if (code == KEY_RIGHT) {
        caret = ctrl ? lf_word_next(text, caret, len) : utf8_next(text, caret, len);
    } else if (code == KEY_HOME) {
        caret = 0;
    } else if (code == KEY_END) {
        caret = len;
    } else if (code == KEY_BACKSPACE) {                              /* before the caret */
        int from = ctrl ? lf_word_prev(text, caret) : utf8_prev(text, caret);
        lf_erase(text, from, caret);
        caret = from;
    } else if (code == KEY_DELETE) {                                 /* at the caret (Shift+Delete is Cut, handled above) */
        lf_erase(text, caret, ctrl ? lf_word_next(text, caret, len) : utf8_next(text, caret, len));
    } else if (!(mods & (LM_MOD_CTRL | LM_MOD_ALT | LM_MOD_SUPER))) {
        keymap_sync(wm);
        if (code >= KEY_KP7 && code <= KEY_KPDOT)
            mods |= LM_MOD_NUM;
        uint32_t ks = lm_keymap_keysym(code, mods);
        char txt[8];
        int k = ks ? lm_keysym_to_utf8(ks, txt) : 0;
        if (k <= 0 || (unsigned char)txt[0] < 0x20 || !lf_insert(text, max, &caret, txt, (size_t)k))
            return;
    } else {
        return;
    }
    wm->launcher_caret[f] = caret;
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
#define DOCK_LAUNCH_MIN_MS 2000     /* the swelling lasts at least this long, even when the window comes sooner */
#define DOCK_MELT_MS 3200           /* autohide: the buttons melt into the edge of the screen this slowly ... */
#define DOCK_RISE_MS 1100           /* ... and rise out of it, vague at first, this fast (both with window animations on; off = at once) */

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
    return !lm_dock.autohide || wm->dock_shown || wm->dock_vis > 0.001f;       /* (while it melts away it is still there) */
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

/* The pose of an icon while the autohide dock melts away or rises out of the screen edge. `p` = how far gone it is (0 there .. 1 gone), `rising`
 * = it is coming back. Each icon follows its neighbour a little later (a wave along the row); it flattens, spreads, sinks into the edge of the screen
 * and fades. Rising is the same movement backwards, and the icon is a vague ghost that sharpens as it reaches its place. */
static void dock_pose(struct lm_dock_info *d, int slot, float p, bool rising, int pos, int n, int out_bottom)
{
    const float stagger = 0.35f;
    float frac = n > 1 ? (float)pos / (float)(n - 1) : 0.0f;
    float delay = stagger * (rising ? 1.0f - frac : frac);
    float q = LM_CLAMP((p - delay) / (1.0f - stagger), 0.0f, 1.0f);
    float e = q * q * (3.0f - 2.0f * q);
    struct lm_rect *b = &d->icon[slot].box;
    float cx = (float)b->x + (float)b->w / 2.0f, bottom = (float)(b->y + b->h);
    float nw = (float)b->w * (1.0f + 0.35f * e), nh = fmaxf((float)b->h * (1.0f - e), 1.0f), nb = bottom + e * ((float)out_bottom - bottom);
    *b = lm_rect_make((int)lroundf(cx - nw / 2.0f), (int)lroundf(nb - nh), (int)lroundf(nw), (int)lroundf(nh));
    d->icon[slot].alpha = e >= 0.995f ? 0 : (uint8_t)lroundf(255.0f * powf(1.0f - e, 0.8f));
    d->icon[slot].blur = rising ? e : 0.0f;
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
        d->icon[vis].alpha = 255;
        if (wm->dock_launch_ms[it]) {                                 /* starting: swells and shrinks again until its window is there */
            float ph = (float)((lm_now_ms() - (uint64_t)wm->dock_launch_ms[it]) % DOCK_BOUNCE_PERIOD_MS) / (float)DOCK_BOUNCE_PERIOD_MS;
            float k = 1.0f + DOCK_PULSE * (0.5f - 0.5f * cosf(6.2831853f * ph));      /* 1 -> 1 + DOCK_PULSE -> 1, smooth at both ends */
            float nw = w * k;                                         /* grows from the middle of its foot: the bottom stays where it is */
            d->icon[vis].box = lm_rect_make((int)lroundf(cxp - nw / 2.0f), (int)lroundf((float)g.icon_bottom - nw), (int)lroundf(nw), (int)lroundf(nw));
            d->icon[vis].launching = true;
        }
        if (lm_dock.autohide && wm->dock_vis < 0.999f)
            dock_pose(d, vis, 1.0f - wm->dock_vis, wm->dock_shown, i, g.n, o->y + o->height);
        vis++;
    }
    d->n = vis;
    if (drag) {                                                       /* the icon in the hand follows the pointer */
        d->drag_item = lm_dock_item(wm->dock_drag_idx);
        d->drag_box = lm_rect_make((int)wm->drag_x - g.zsize / 2, (int)wm->drag_y - g.zsize / 2, g.zsize, g.zsize);
        d->drag_remove = wm->dock_drag_to < 0 && !dock_special(d->drag_item);
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

static void dock_set_shown(struct lm_wm *wm, bool shown);

static void dock_hide_tick(void *data)
{
    struct lm_wm *wm = data;
    if (wm->dock_shown && !wm->dock_active) {
        dock_damage(wm);
        dock_set_shown(wm, false);
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
            dock_set_shown(wm, true);
            changed = true;
        }
    } else if (!in && wm->dock_shown && lm_dock.autohide && wm->dock_timer && !lm_timer_is_armed(wm->dock_timer)) {
        lm_timer_arm_ms(wm->dock_timer, DOCK_HIDE_MS, 0);
    }
    if (in && !wm->dock_active && wm->srv->config.sound && strcmp(wm->srv->config.backend, "headless") != 0)
        lm_sound_wake();                                        /* the pointer came to the dock: wake the sink before the click's sound needs it */
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
/* The settings panel is there once: when it is open (or was started a moment ago and is still coming up) it is brought forward and no second one
 * starts. True when that happened. */
static bool settings_present(struct lm_wm *wm)
{
    struct lm_window *w;
    lm_list_for_each(w, &wm->windows, link)
        if (w->mapped && w->app_id && !strcmp(w->app_id, "mellagui.settings")) {
            lm_wm_focus_window(wm, w);
            return true;
        }
    return wm->settings_spawn_ms && (int64_t)lm_now_ms() - wm->settings_spawn_ms < 4000;
}

static void dock_launch(struct lm_wm *wm, const struct lm_dock_item *it, int i, bool reuse)
{
    bool is_settings = !dock_special(it) && dock_app_matches(it->match, "mellagui.settings");
    if (is_settings && settings_present(wm))
        return;
    if (is_settings)
        wm->settings_spawn_ms = (int64_t)lm_now_ms();
    if (launcher_is_self(it)) {                                       /* the Launcher button: not a program */
        launcher_set(wm, true, dock_output(wm));
        return;
    }
    if (dock_special(it)) {                                           /* the Overview button: opens it, a second click closes it (like Super+W) */
        overview_set(wm, !wm->ov.open || wm->ov.closing);
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

/* ---- menus: the dock icon's right-click menu and the ones programs ask for (text field actions) ----------------------------------------- */

#define DM_PAD 6
#define DM_HEAD LM_MENU_HEAD
#define DM_ITEM_H 28
#define DM_MIN_W 150

static void menu_damage(struct lm_wm *wm)
{
    if (wm->menu_rect.w > 0)
        damage_rect(wm, &wm->menu_rect);
}

static void menu_info(const struct lm_wm *wm, struct lm_menu_info *mi)
{
    memset(mi, 0, sizeof(*mi));
    mi->rect = wm->menu_rect;
    mi->title = wm->menu_title;
    mi->sel = wm->menu_sel;
    mi->n = wm->menu_n;
    int head = wm->menu_title[0] ? DM_HEAD : 0;
    for (int i = 0; i < wm->menu_n; i++) {
        mi->entry[i] = lm_rect_make(mi->rect.x + DM_PAD, mi->rect.y + head + DM_PAD + i * DM_ITEM_H, mi->rect.w - 2 * DM_PAD, DM_ITEM_H);
        mi->label[i] = wm->menu_label[i];
        mi->enabled[i] = wm->menu_enab[i];
    }
}

/* Closes the menu. A menu a program asked for is told when nothing was chosen. */
static void menu_close(struct lm_wm *wm, bool chosen)
{
    if (!wm->menu_open)
        return;
    menu_damage(wm);
    bool client = wm->menu_owner == 1;
    wm->menu_open = false;
    wm->menu_sel = -1;
    wm->menu_rect = (struct lm_rect){ 0 };
    if (client && !chosen)
        lm_menu_closed(wm->srv);
}

static void menu_set(struct lm_wm *wm, bool open)
{
    if (!open)
        menu_close(wm, false);
}

/* Fills the entries and works out the size of the card: the caller places it (menu_place). */
static bool menu_fill(struct lm_wm *wm, int owner, const char *title, const char *const *labels, const bool *enabled, int n, int *w, int *h)
{
    if (n < 1 || n > LM_MENU_MAX)
        return false;
    menu_close(wm, false);
    wm->menu_owner = owner;
    wm->menu_n = n;
    snprintf(wm->menu_title, sizeof(wm->menu_title), "%s", title ? title : "");
    int widest = lm_font_text_width(wm->menu_title);
    for (int i = 0; i < n; i++) {
        snprintf(wm->menu_label[i], sizeof(wm->menu_label[i]), "%s", labels[i]);
        wm->menu_enab[i] = enabled ? enabled[i] : true;
        widest = LM_MAX(widest, lm_font_text_width(wm->menu_label[i]));
    }
    *w = LM_MAX(DM_MIN_W, widest + 40);
    *h = (wm->menu_title[0] ? DM_HEAD : 0) + 2 * DM_PAD + n * DM_ITEM_H;
    return true;
}

static void menu_place(struct lm_wm *wm, int x, int y, int w, int h, const struct lm_output *o)
{
    struct lm_rect orect = lm_output_rect(o);
    x = LM_CLAMP(x, orect.x + 4, LM_MAX(orect.x + orect.w - w - 4, orect.x + 4));
    y = LM_CLAMP(y, orect.y + LM_PANEL_HEIGHT + 4, LM_MAX(orect.y + orect.h - h - 4, orect.y + LM_PANEL_HEIGHT + 4));
    wm->menu_open = true;
    wm->menu_sel = -1;
    wm->menu_rect = lm_rect_make(x, y, w, h);
    menu_damage(wm);
}

bool lm_wm_menu_popup(struct lm_wm *wm, const char *const *labels, int n)
{
    if (!wm || lm_lock_active(wm->srv->lock) || !wm->srv->seat)
        return false;
    double px = 0, py = 0;
    lm_seat_pointer_position(wm->srv->seat, &px, &py);
    struct lm_output *o = lm_output_nearest(wm->srv, px, py);
    int w, h;
    if (!o || !menu_fill(wm, 1, NULL, labels, NULL, n, &w, &h))
        return false;
    for (int i = 0; i < n; i++)                                      /* a leading '~' marks an entry that cannot be chosen */
        if (labels[i][0] == '~') {
            wm->menu_enab[i] = false;
            memmove(wm->menu_label[i], wm->menu_label[i] + 1, strlen(wm->menu_label[i]));
        }
    menu_place(wm, (int)px - 4, (int)py - 4, w, h, o);
    return true;
}

void lm_wm_menu_cancel(struct lm_wm *wm)
{
    if (wm && wm->menu_open && wm->menu_owner == 1) {
        menu_damage(wm);
        wm->menu_open = false;
        wm->menu_sel = -1;
        wm->menu_rect = (struct lm_rect){ 0 };
    }
}

/* Right click on a window's title bar or its taskbar button: Maximize / Restore, Minimize, Move to workspace 1..4, Close. */
static void window_menu_open(struct lm_wm *wm, struct lm_window *w)
{
    if (!w || !w->mapped || win_is_calendar(w) || !wm->srv->seat)
        return;
    static char names[LM_NUM_WORKSPACES][24];
    const char *labels[3 + LM_NUM_WORKSPACES + 1];
    bool enabled[3 + LM_NUM_WORKSPACES + 1];
    int n = 0;
    bool max = (w->states & LM_WINDOW_MAXIMIZED) != 0;
    labels[n] = max ? "Restore" : "Maximize";
    enabled[n++] = !(w->states & LM_WINDOW_FULLSCREEN);
    labels[n] = "Minimize";
    enabled[n++] = !w->minimized;
    for (int i = 0; i < LM_NUM_WORKSPACES; i++) {
        snprintf(names[i], sizeof(names[i]), "Move to workspace %d", i + 1);
        labels[n] = names[i];
        enabled[n++] = i != w->workspace;
    }
    labels[n] = "Close";
    enabled[n++] = true;
    double px = 0, py = 0;
    lm_seat_pointer_position(wm->srv->seat, &px, &py);
    struct lm_output *o = lm_output_nearest(wm->srv, px, py);
    int mw, mh;
    if (!o || !menu_fill(wm, 3, w->title && w->title[0] ? w->title : NULL, labels, enabled, n, &mw, &mh))
        return;
    wm->menu_window = w;
    menu_place(wm, (int)px - 4, (int)py - 4, mw, mh, o);
}

static void window_menu_activate(struct lm_wm *wm, int which)
{
    struct lm_window *w = wm->menu_window;
    wm->menu_window = NULL;
    if (!w)
        return;
    if (which == 0)
        lm_wm_window_request_maximize(w, !(w->states & LM_WINDOW_MAXIMIZED));
    else if (which == 1)
        lm_wm_window_request_minimize(w);
    else if (which >= 2 && which < 2 + LM_NUM_WORKSPACES)
        move_to_workspace(wm, w, which - 2);
    else
        lm_wm_window_close(w);
}

/* Right click on a text field of the launcher's form: Cut / Copy / Paste / Clear at the pointer. */
static void launcher_field_menu(struct lm_wm *wm, int field)
{
    static const char *const labels[4] = { "Cut", "Copy", "Paste", "Clear" };
    wm->launcher_focus = field;
    size_t max;
    const char *text = launcher_field_text(wm, &max);
    if (!text || !wm->srv->seat)
        return;
    bool has = text[0] != 0, clip = lm_seat_clip_count(wm->srv->seat) > 0;
    bool enabled[4] = { has, has, clip, has };
    double px = 0, py = 0;
    lm_seat_pointer_position(wm->srv->seat, &px, &py);
    struct lm_output *o = lm_output_nearest(wm->srv, px, py);
    int w, h;
    if (!o || !menu_fill(wm, 2, NULL, labels, enabled, 4, &w, &h))
        return;
    menu_place(wm, (int)px - 4, (int)py - 4, w, h, o);
    launcher_damage(wm);
}

/* Right click on dock icon `i`: the menu opens above the icon. The Launcher button has none (it is not a program). */
static void dockitem_menu_open(struct lm_wm *wm, int i)
{
    static const char *const labels[4] = { "New window", "Close all windows", "Change...", "Remove from dock" };
    const struct lm_dock_item *it = lm_dock_item(i);
    struct lm_output *o = dock_output(wm);
    if (!it || !o || dock_special(it))
        return;
    struct lm_dock_info d;
    dock_info(wm, o, &d);
    struct lm_rect box = { 0 };
    for (int k = 0; k < d.n; k++)
        if (d.icon[k].item == it)
            box = d.icon[k].box;
    if (box.w <= 0)
        return;
    bool enabled[4] = { true, dock_window_count(wm, it) > 0, true, true };      /* nothing to close when there is no window */
    int w, h;
    if (!menu_fill(wm, 0, it->name, labels, enabled, 4, &w, &h))
        return;
    w = LM_MAX(w, 190);
    wm->menu_dock_item = i;
    menu_place(wm, box.x + box.w / 2 - w / 2, box.y - 14 - h, w, h, o);
}

/* An entry was chosen: 0 a new window (another instance), 1 close every window of the program, 2 change the button in the launcher's form,
 * 3 take it out of the dock (it stays in the launcher). */
static void dockitem_menu_activate(struct lm_wm *wm, int which)
{
    int di = wm->menu_dock_item;
    const struct lm_dock_item *it = lm_dock_item(di);
    if (!it || dock_special(it))
        return;
    switch (which) {
    case 0:
        dock_launch(wm, it, di, false);
        break;
    case 1: {
        struct lm_window *w;
        lm_list_for_each(w, &wm->windows, link)
            if (w->mapped && dock_app_matches(it->match, w->app_id))
                lm_wm_window_close(w);
        break;
    }
    case 2: {
        int g = 0;                                                    /* its place in the launcher's list: the dock's buttons come first */
        for (int k = 0; k < di; k++)
            if (!launcher_is_self(lm_dock_item(k)))
                g++;
        launcher_set(wm, true, dock_output(wm));
        wm->launcher_edit = true;
        wm->launcher_sel = g;
        launcher_open_change(wm, g);
        break;
    }
    case 3:
        if (!lm_settings_dock_to_launcher(wm->srv, it->raw))
            lm_wm_notify(wm, "Dock", "Could not remove it: there is no config file to write", 5000);
        break;
    }
}

/* An entry was chosen: whoever asked for the menu gets it. */
static void menu_activate(struct lm_wm *wm, int which)
{
    int owner = wm->menu_owner;
    if (which < 0 || which >= wm->menu_n || !wm->menu_enab[which])
        return;
    menu_close(wm, true);
    if (owner == 0)
        dockitem_menu_activate(wm, which);
    else if (owner == 2)
        launcher_field_clip(wm, which);
    else if (owner == 3)
        window_menu_activate(wm, which);
    else
        lm_menu_chosen(wm->srv, which);
}

size_t lm_wm_menu_describe(struct lm_wm *wm, char *buf, size_t size)
{
    size_t n = (size_t)snprintf(buf, size, "dockmenu open=%d item=%d sel=%d owner=%d\n", wm->menu_open,
                                wm->menu_open && wm->menu_owner == 0 ? wm->menu_dock_item : -1, wm->menu_open ? wm->menu_sel : -1,
                                wm->menu_open ? wm->menu_owner : -1);
    if (!wm->menu_open)
        return n < size ? n : size - 1;
    struct lm_menu_info mi;
    menu_info(wm, &mi);
    n += (size_t)snprintf(buf + n, size - n, "card x=%d y=%d w=%d h=%d title=\"%s\"\n", mi.rect.x, mi.rect.y, mi.rect.w, mi.rect.h, mi.title);
    for (int i = 0; i < mi.n && n < size; i++)
        n += (size_t)snprintf(buf + n, size - n, "entry %d \"%s\" x=%d y=%d w=%d h=%d enabled=%d\n", i, mi.label[i], mi.entry[i].x, mi.entry[i].y,
                              mi.entry[i].w, mi.entry[i].h, mi.enabled[i]);
    return n < size ? n : size - 1;
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
        if (!dock_special(it) && !lm_settings_dock_to_launcher(wm->srv, it->raw))
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

/* The autohide dock is to be shown / hidden: with animations on it rises or melts over a few seconds (dock_vis follows, dock_anim_tick steps it), else at once. */
static void dock_set_shown(struct lm_wm *wm, bool shown)
{
    wm->dock_shown = shown;
    if (anim_duration(wm) <= 0) {
        wm->dock_vis = shown ? 1.0f : 0.0f;
        dock_damage(wm);
        return;
    }
    wm->dock_vis_ms = (int64_t)lm_now_ms();
    if (wm->dock_anim)
        lm_timer_arm_ms(wm->dock_anim, DOCK_ANIM_MS, DOCK_ANIM_MS);
}

/* Every 33 ms while some icon swells or the dock melts / rises: ends the swelling of the ones whose window came (after at least DOCK_LAUNCH_MIN_MS,
 * or that never made one), moves the autohide animation on and repaints. */
static void dock_anim_tick(void *data)
{
    struct lm_wm *wm = data;
    const char *e = getenv("MELLAGUI_DOCK_BOUNCE_MS");               /* how long to swell at most; tests make it short */
    int64_t limit = e && atol(e) > 0 ? atol(e) : 12000;
    int64_t now = (int64_t)lm_now_ms();
    bool any = false;
    for (int i = 0; i < LM_DOCK_MAX; i++) {
        if (!wm->dock_launch_ms[i])
            continue;
        const struct lm_dock_item *it = lm_dock_item(i);
        int64_t age = now - wm->dock_launch_ms[i];
        bool came = it && dock_window_count(wm, it) > wm->dock_launch_base[i];
        if (!it || (came && age >= DOCK_LAUNCH_MIN_MS) || age > limit)
            wm->dock_launch_ms[i] = 0;
        else
            any = true;
    }
    float target = wm->dock_shown ? 1.0f : 0.0f;
    if (lm_dock.autohide && wm->dock_vis != target) {
        float dur = (float)(wm->dock_shown ? DOCK_RISE_MS : DOCK_MELT_MS);
        float step = (float)(now - wm->dock_vis_ms) / dur;
        wm->dock_vis = target > wm->dock_vis ? fminf(wm->dock_vis + step, 1.0f) : fmaxf(wm->dock_vis - step, 0.0f);
        any = any || wm->dock_vis != target;
    }
    wm->dock_vis_ms = now;
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

/* ---- window animations ------------------------------------------------------------------------------------------------ */

/* How long an animation takes (ms); 0 = none. MELLAGUI_ANIMATIONS (tests; headless otherwise never animates): a number above 1 is the
 * time, 1 the metrics value, 0 off. */
static int anim_duration(const struct lm_wm *wm)
{
    const char *e = getenv("MELLAGUI_ANIMATIONS");
    if (e && *e) {
        int v = atoi(e);
        return v > 1 ? LM_MIN(v, 5000) : v == 1 ? lm_metrics.animation_ms : 0;
    }
    if (wm->srv->backend && !strcmp(wm->srv->backend->ops->name, "headless"))
        return 0;
    return lm_metrics.animation_ms;
}

static bool anim_wanted(struct lm_wm *wm, const struct lm_window *w)
{
    return anim_duration(wm) > 0 && !wm->anim_suppress && wm->srv->render && !lm_lock_active(wm->srv->lock) &&
           !(w->states & LM_WINDOW_FULLSCREEN) && !win_is_calendar(w);
}

/* The window's picture (frame and content, without popups) and the rectangle it covers. */
static bool anim_prepare(struct lm_wm *wm, struct lm_window *w, struct lm_ghost *g, struct lm_rect *home)
{
    struct lm_scene_item it[2];
    memset(it, 0, sizeof(it));
    it[0].kind = LM_SCENE_FRAME;
    frame_info(wm, w, &it[0].u.frame);
    int32_t ox, oy;
    win_origin(w, &ox, &oy);
    it[1].kind = LM_SCENE_SURFACE;
    it[1].u.surface.root = w->surface;
    it[1].u.surface.x = ox;
    it[1].u.surface.y = oy;
    struct lm_rect outer = frame_outer(w), ext;
    *home = outer;
    if (lm_surface_tree_extents(w->surface, &ext)) {
        struct lm_rect eb = lm_rect_make(ox + ext.x, oy + ext.y, ext.w, ext.h);
        *home = lm_rect_union(&outer, &eb);
    }
    return lm_render_snapshot(wm->srv->render, it, 2, home, g);
}

/* Where a minimized window goes to: the dock icon of its program when the dock shows one, else its taskbar button, else the bottom centre. */
static struct lm_rect anim_target(struct lm_wm *wm, struct lm_window *w, const struct lm_rect *home)
{
    struct lm_output *o = window_output(wm, w);
    if (!o)
        o = lm_output_nearest(wm->srv, home->x + home->w / 2, home->y + home->h / 2);
    if (o && lm_dock.enabled && dock_shown(wm)) {
        struct lm_dock_info di;
        dock_info(wm, o, &di);
        for (int i = 0; i < di.n; i++)
            if (di.icon[i].item && dock_app_matches(di.icon[i].item->match, w->app_id))
                return di.icon[i].box;
    }
    if (o) {
        struct lm_panel_info pi;
        panel_info(wm, o, &pi);
        for (int i = 0; i < pi.ntasks; i++)
            if (pi.task_win[i] == w)
                return pi.task[i].rect;
        return lm_rect_make(o->x + o->width / 2 - 24, o->y + o->height - 12, 48, 12);
    }
    return lm_rect_make(home->x + home->w / 2 - 24, home->y + home->h - 12, 48, 12);
}

static float ease_out(float t) { float u = 1.0f - t; return 1.0f - u * u * u; }
static float ease_smooth(float t) { return t * t * (3.0f - 2.0f * t); }

static struct lm_rect anim_lerp_rect(const struct lm_rect *a, const struct lm_rect *b, float e)
{
    return lm_rect_make((int)lroundf((float)a->x + (float)(b->x - a->x) * e), (int)lroundf((float)a->y + (float)(b->y - a->y) * e),
                        LM_MAX((int)lroundf((float)a->w + (float)(b->w - a->w) * e), 1),
                        LM_MAX((int)lroundf((float)a->h + (float)(b->h - a->h) * e), 1));
}

/* The picture's rectangle and opacity `t` (0..1) of the way through. */
static void anim_state(const struct wm_anim *a, float t, struct lm_rect *r, int *alpha)
{
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    switch (a->kind) {
    case ANIM_OPEN:
    case ANIM_CLOSE: {
        float e = ease_out(t), s, op;
        if (a->kind == ANIM_OPEN) {
            s = 0.9f + 0.1f * e;
            op = e;
        } else {
            s = 1.0f - 0.08f * e;
            op = 1.0f - e;
        }
        int w = LM_MAX((int)lroundf((float)a->home.w * s), 1), h = LM_MAX((int)lroundf((float)a->home.h * s), 1);
        *r = lm_rect_make(a->home.x + (a->home.w - w) / 2, a->home.y + (a->home.h - h) / 2, w, h);
        *alpha = (int)lroundf(255.0f * op);
        break;
    }
    case ANIM_MINIMIZE:
    case ANIM_RESTORE: {
        float e = ease_smooth(a->kind == ANIM_MINIMIZE ? t : 1.0f - t);          /* a restore runs the minimize backwards */
        *r = anim_lerp_rect(&a->home, &a->away, e);
        *alpha = (int)lroundf(255.0f * (1.0f - e * e));
        break;
    }
    }
    if (*alpha < 0) *alpha = 0;
    if (*alpha > 255) *alpha = 255;
}

static void anim_end(struct lm_wm *wm, struct wm_anim *a)
{
    struct lm_rect d = lm_rect_union(&a->shown, &a->home);
    if (a->win)
        a->win->anim_hidden = false;
    lm_render_ghost_free(wm->srv->render, &a->ghost);
    a->used = false;
    a->win = NULL;
    damage_rect(wm, &d);
}

static void anim_finish_all(struct lm_wm *wm)
{
    wobble_stop(wm);
    bubble_end(wm);
    for (int i = 0; i < ANIM_MAX; i++)
        if (wm->anim[i].used)
            anim_end(wm, &wm->anim[i]);
}

/* The window is going away: its animation (and the picture it holds) ends. */
static void anim_forget(struct lm_wm *wm, struct lm_window *w)
{
    if (wm->glide_win == w)
        wm->glide_win = NULL;
    if (wm->wb.active && wm->wb.win == w)
        wobble_stop(wm);
    for (int i = 0; i < ANIM_MAX; i++)
        if (wm->anim[i].used && wm->anim[i].win == w)
            anim_end(wm, &wm->anim[i]);
    if (w->close_ghost) {
        free(w->close_ghost->px);
        free(w->close_ghost);
        w->close_ghost = NULL;
    }
}

/* Starts an animation; the ghost's pixels now belong to it. */
static void anim_start(struct lm_wm *wm, enum anim_kind kind, struct lm_window *w, struct lm_ghost *g, const struct lm_rect *home)
{
    struct wm_anim *a = NULL;
    for (int i = 0; i < ANIM_MAX && !a; i++)
        if (!wm->anim[i].used)
            a = &wm->anim[i];
    if (!a) {                                                       /* all busy: the first one ends at once */
        a = &wm->anim[0];
        anim_end(wm, a);
    }
    if (w)
        for (int i = 0; i < ANIM_MAX; i++)                          /* one animation per window: the older one stops */
            if (wm->anim[i].used && wm->anim[i].win == w)
                anim_end(wm, &wm->anim[i]);
    memset(a, 0, sizeof(*a));
    a->used = true;
    a->kind = kind;
    a->ghost = *g;
    memset(g, 0, sizeof(*g));
    a->home = *home;
    a->dur = anim_duration(wm);
    a->start = lm_now_ms();
    if (kind == ANIM_MINIMIZE || kind == ANIM_RESTORE) {
        a->away = anim_target(wm, w, home);
        a->away.y += a->away.h / 2;                                 /* shrinks towards the middle of the button */
        a->away.x += a->away.w / 4;
        a->away.w = LM_MAX(a->away.w / 2, 2);
        a->away.h = 2;
    }
    if (kind == ANIM_OPEN || kind == ANIM_RESTORE) {
        a->win = w;
        w->anim_hidden = true;
    }
    anim_state(a, 0.0f, &a->shown, &a->alpha);
    struct lm_rect d = lm_rect_union(&a->home, &a->shown);
    damage_rect(wm, &d);
    if (wm->anim_timer && !lm_timer_is_armed(wm->anim_timer))
        lm_timer_arm_ms(wm->anim_timer, ANIM_TICK_MS, ANIM_TICK_MS);
}

/* ---- the launcher's bubble --------------------------------------------------------------------------------------------------------- */

#define BUBBLE_MS 2000              /* the whole rise / return: two seconds (with window animations on; off = at once) */
#define BUBBLE_R0 24.0f             /* radius of the bubble at the button */

static struct lm_output *bubble_output(const struct lm_wm *wm)
{
    if (!wm->srv->backend)
        return NULL;
    struct lm_output *o;
    lm_list_for_each(o, &wm->srv->backend->outputs, link)
        if (!strcmp(o->name, wm->launcher_out))
            return o;
    return NULL;
}

/* The middle of the Launcher button in the dock (where it would be even if autohide keeps the dock away), else the bottom middle of the output. */
static void bubble_origin(const struct lm_wm *wm, struct lm_output *o, float *cx, float *cy)
{
    struct lm_output *dock = lm_output_primary(wm->srv);
    if (dock == o && lm_dock.enabled && lm_dock_count() > 0) {
        struct dock_geo g;
        dock_geometry(o, &g);
        for (int i = 0; i < g.n; i++)
            if (launcher_is_self(lm_dock_item(i))) {
                *cx = (float)(g.x0 + i * (g.size + g.gap)) + (float)g.size / 2.0f;
                *cy = (float)g.icon_bottom - (float)g.size / 2.0f;
                return;
            }
    }
    *cx = (float)(o->x + o->width / 2);
    *cy = (float)(o->y + o->height - 40);
}

static float smooth01(float x)
{
    x = LM_CLAMP(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

/* Where the corners of the cells are at progress p. The bubble leaves the button slowly, grows as it rises, wobbles (squashed one way, then the other)
 * and, from a little way on, spreads out into the card; the card is exactly there at p = 1. */
static void bubble_nodes(const struct wm_bubble *b, float p, float *xs, float *ys, float *s_out)
{
    float pe = smooth01(p);
    float s = smooth01((p - 0.28f) / 0.64f);                                 /* a bubble for the first quarter, then it spreads out */
    s += 0.05f * sinf(p * 18.8496f) * sinf(p * 3.14159265f);               /* it jiggles as it takes its shape (zero at both ends) */
    s = LM_CLAMP(s, 0.0f, 1.06f);
    float ccx = (float)b->card.x + (float)b->card.w / 2.0f, ccy = (float)b->card.y + (float)b->card.h / 2.0f;
    float cx = b->cx0 + (ccx - b->cx0) * pe, cy = b->cy0 + (ccy - b->cy0) * pe;
    float r1 = 0.32f * (float)LM_MIN(b->card.w, b->card.h);
    float R = BUBBLE_R0 + (r1 - BUBBLE_R0) * pe;
    float k = 0.12f * sinf(p * 25.0f) * (1.0f - fminf(s, 1.0f));              /* the squash: wider and flatter, then taller and narrower */
    float sx = 1.0f + k, sy = 1.0f - k;
    for (int j = 0; j <= BUB_R; j++)
        for (int i = 0; i <= BUB_C; i++) {
            float u = (float)i / (float)BUB_C, v = (float)j / (float)BUB_R, a = 2.0f * u - 1.0f, c = 2.0f * v - 1.0f;
            float dx = a * sqrtf(1.0f - c * c / 2.0f), dy = c * sqrtf(1.0f - a * a / 2.0f);       /* the square onto a disc */
            float bx = cx + dx * R * sx, by = cy + dy * R * sy;
            float rx = (float)b->card.x + u * (float)b->card.w, ry = (float)b->card.y + v * (float)b->card.h;
            xs[j * (BUB_C + 1) + i] = bx + (rx - bx) * s;
            ys[j * (BUB_C + 1) + i] = by + (ry - by) * s;
        }
    if (s_out)
        *s_out = s;
}

/* The highlight of a wet bubble: a soft bright patch up on the left and a thin lit rim, on a transparent square. */
static void bubble_make_sheen(struct lm_ghost *g)
{
    const int n = 96;
    uint32_t *px = calloc((size_t)n * (size_t)n, 4);
    if (!px)
        return;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            float fx = ((float)x + 0.5f) / (float)n * 2.0f - 1.0f, fy = ((float)y + 0.5f) / (float)n * 2.0f - 1.0f, r = sqrtf(fx * fx + fy * fy);
            float px_ = fx + 0.36f, py_ = fy + 0.40f, rot = 0.6f;                                   /* the patch, turned a little */
            float ex = (px_ * cosf(rot) + py_ * sinf(rot)) / 0.30f, ey = (-px_ * sinf(rot) + py_ * cosf(rot)) / 0.13f;
            float patch = fmaxf(1.0f - (ex * ex + ey * ey), 0.0f);
            float rim = r > 0.86f && r < 1.0f ? (1.0f - fabsf(r - 0.93f) / 0.07f) * (0.35f + 0.25f * (fx - fy)) : 0.0f;
            float film = r < 0.93f ? 0.10f * (1.0f - 0.5f * r) : 0.0f;                                  /* the thin skin of the bubble */
            float a = fminf(patch * patch * 0.85f + fmaxf(rim, 0.0f) * 0.55f + film, 1.0f);
            uint32_t av = (uint32_t)lroundf(a * 255.0f);
            px[y * n + x] = (av << 24) | (av << 16) | (av << 8) | av;                                  /* white, premultiplied */
        }
    g->px = px;
    g->w = g->h = n;
    g->gl_tex = 0;
}

/* ---- rasterising the mesh ----------------------------------------------------------------------------------------------------- */

static void bubble_free_pictures(struct lm_wm *wm, struct wm_bubble *b)
{
    for (int l = BUB_LEVELS - 1; l >= 1; l--) {                    /* (a level that is a copy of the next bigger one, after an out-of-memory, is freed once) */
        if (b->mip[l] && b->mip[l] != b->mip[l - 1])
            free(b->mip[l]);
        b->mip[l] = NULL;
    }
    b->mip[0] = NULL;
    if (wm->srv->render) {
        lm_render_ghost_free(wm->srv->render, &b->frame);
        lm_render_ghost_free(wm->srv->render, &b->ghost);
        lm_render_ghost_free(wm->srv->render, &b->sheen);
    }
}

/* The card's picture and, halved by averaging 2 x 2 each time, its smaller copies. */
static void bubble_make_mips(struct wm_bubble *b)
{
    b->mip[0] = b->ghost.px;
    b->mw[0] = b->ghost.w;
    b->mh[0] = b->ghost.h;
    for (int l = 1; l < BUB_LEVELS; l++) {
        int w = LM_MAX(b->mw[l - 1] / 2, 1), h = LM_MAX(b->mh[l - 1] / 2, 1);
        uint32_t *px = calloc((size_t)w * (size_t)h, 4);
        if (!px) {
            b->mip[l] = b->mip[l - 1];                              /* out of memory: the bigger one will do */
            b->mw[l] = b->mw[l - 1];
            b->mh[l] = b->mh[l - 1];
            continue;
        }
        b->mip[l] = px;
        b->mw[l] = w;
        b->mh[l] = h;
        const uint32_t *src = b->mip[l - 1];
        int sw = b->mw[l - 1], sh = b->mh[l - 1];
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                uint32_t acc[4] = { 0, 0, 0, 0 };
                for (int k = 0; k < 4; k++) {
                    int sx = LM_MIN(2 * x + (k & 1), sw - 1), sy = LM_MIN(2 * y + (k >> 1), sh - 1);
                    uint32_t p = src[(size_t)sy * (size_t)sw + (size_t)sx];
                    acc[0] += p >> 24; acc[1] += (p >> 16) & 255; acc[2] += (p >> 8) & 255; acc[3] += p & 255;
                }
                px[(size_t)y * (size_t)w + (size_t)x] = ((acc[0] / 4) << 24) | ((acc[1] / 4) << 16) | ((acc[2] / 4) << 8) | (acc[3] / 4);
            }
    }
}

/* Bilinear lookup in level `l` at (u, v) in that level's texels (premultiplied ARGB, the edge repeats). */
static uint32_t bubble_sample(const struct wm_bubble *b, int l, float u, float v)
{
    int w = b->mw[l], h = b->mh[l];
    const uint32_t *px = b->mip[l];
    float fx = u - 0.5f, fy = v - 0.5f;
    int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
    float tx = fx - (float)x0, ty = fy - (float)y0;
    int x1 = LM_CLAMP(x0 + 1, 0, w - 1), y1 = LM_CLAMP(y0 + 1, 0, h - 1);
    x0 = LM_CLAMP(x0, 0, w - 1);
    y0 = LM_CLAMP(y0, 0, h - 1);
    uint32_t p00 = px[(size_t)y0 * (size_t)w + (size_t)x0], p10 = px[(size_t)y0 * (size_t)w + (size_t)x1];
    uint32_t p01 = px[(size_t)y1 * (size_t)w + (size_t)x0], p11 = px[(size_t)y1 * (size_t)w + (size_t)x1];
    uint32_t out = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        float a = (float)((p00 >> sh) & 255) * (1.0f - tx) + (float)((p10 >> sh) & 255) * tx;
        float c = (float)((p01 >> sh) & 255) * (1.0f - tx) + (float)((p11 >> sh) & 255) * tx;
        out |= (uint32_t)lroundf(a * (1.0f - ty) + c * ty) << sh;
    }
    return out;
}

/* One triangle of the mesh: x, y = where its corners are, u, v = which texel of the card each shows. Covered pixels take the card's colour; a pixel that
 * two triangles both reach keeps the more solid one, so a seam never shows a hole. */
static void bubble_triangle(const struct wm_bubble *b, uint32_t *out, int bx, int by, int bw, int bh, const float *x, const float *y,
                            const float *u, const float *v)
{
    float det = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
    float src = (u[1] - u[0]) * (v[2] - v[0]) - (u[2] - u[0]) * (v[1] - v[0]);
    if (fabsf(det) < 1e-3f || fabsf(src) < 1e-6f)
        return;
    float step = sqrtf(fabsf(src) / fabsf(det));                    /* card texels under one pixel: above 1 the picture is shrunk */
    int level = step > 1.0f ? LM_CLAMP((int)floorf(log2f(step)), 0, BUB_LEVELS - 1) : 0;
    float scale = 1.0f / (float)(1 << level), inv = 1.0f / det;
    int x0 = LM_MAX((int)floorf(fminf(x[0], fminf(x[1], x[2]))) - bx, 0), x1 = LM_MIN((int)ceilf(fmaxf(x[0], fmaxf(x[1], x[2]))) - bx, bw - 1);
    int y0 = LM_MAX((int)floorf(fminf(y[0], fminf(y[1], y[2]))) - by, 0), y1 = LM_MIN((int)ceilf(fmaxf(y[0], fmaxf(y[1], y[2]))) - by, bh - 1);
    const float e = -0.02f;
    for (int py = y0; py <= y1; py++) {
        float fy = (float)(by + py) + 0.5f;
        for (int px = x0; px <= x1; px++) {
            float fx = (float)(bx + px) + 0.5f;
            float l1 = ((fx - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (fy - y[0])) * inv;
            float l2 = ((x[1] - x[0]) * (fy - y[0]) - (fx - x[0]) * (y[1] - y[0])) * inv;
            float l0 = 1.0f - l1 - l2;
            if (l0 < e || l1 < e || l2 < e)
                continue;
            uint32_t c = bubble_sample(b, level, (l0 * u[0] + l1 * u[1] + l2 * u[2]) * scale, (l0 * v[0] + l1 * v[1] + l2 * v[2]) * scale);
            uint32_t *dst = &out[(size_t)py * (size_t)bw + (size_t)px];
            if ((c >> 24) >= (*dst >> 24))
                *dst = c;
        }
    }
}

/* The mesh as it is at b->prog, painted into b->frame (and b->shown, b->alpha, b->s set). */
static void bubble_render_frame(struct lm_wm *wm)
{
    struct wm_bubble *b = &wm->bubble;
    float xs[BUB_N], ys[BUB_N], s;
    bubble_nodes(b, b->prog, xs, ys, &s);
    float x0 = xs[0], x1 = xs[0], y0 = ys[0], y1 = ys[0];
    for (int i = 1; i < BUB_N; i++) {
        x0 = fminf(x0, xs[i]); x1 = fmaxf(x1, xs[i]);
        y0 = fminf(y0, ys[i]); y1 = fmaxf(y1, ys[i]);
    }
    struct lm_rect r = lm_rect_make((int)floorf(x0) - 2, (int)floorf(y0) - 2, (int)ceilf(x1 - x0) + 5, (int)ceilf(y1 - y0) + 5);
    b->s = s;
    b->alpha = (int)lroundf(255.0f * (0.62f + 0.38f * LM_CLAMP(s, 0.0f, 1.0f)) * fminf(b->prog / 0.05f, 1.0f));
    if (wm->srv->render)
        lm_render_ghost_free(wm->srv->render, &b->frame);
    memset(&b->frame, 0, sizeof(b->frame));
    b->shown = r;
    if (r.w <= 0 || r.h <= 0 || (int64_t)r.w * r.h > 8 * 1000 * 1000)
        return;
    uint32_t *out = calloc((size_t)r.w * (size_t)r.h, 4);
    if (!out)
        return;
    const float W = (float)b->mw[0], H = (float)b->mh[0];
    const int stride = BUB_C + 1;
    for (int j = 0; j < BUB_R; j++)
        for (int i = 0; i < BUB_C; i++) {
            int n[4] = { j * stride + i, j * stride + i + 1, (j + 1) * stride + i, (j + 1) * stride + i + 1 };
            float u[4] = { W * (float)i / BUB_C, W * (float)(i + 1) / BUB_C, W * (float)i / BUB_C, W * (float)(i + 1) / BUB_C };
            float v[4] = { H * (float)j / BUB_R, H * (float)j / BUB_R, H * (float)(j + 1) / BUB_R, H * (float)(j + 1) / BUB_R };
            float cx[4], cy[4];
            for (int k = 0; k < 4; k++) {
                cx[k] = xs[n[k]];
                cy[k] = ys[n[k]];
            }
            static const int t1[3] = { 0, 1, 2 }, t2[3] = { 1, 3, 2 };
            float tx[3], ty[3], tu[3], tv[3];
            for (int k = 0; k < 3; k++) {
                tx[k] = cx[t1[k]]; ty[k] = cy[t1[k]]; tu[k] = u[t1[k]]; tv[k] = v[t1[k]];
            }
            bubble_triangle(b, out, r.x, r.y, r.w, r.h, tx, ty, tu, tv);
            for (int k = 0; k < 3; k++) {
                tx[k] = cx[t2[k]]; ty[k] = cy[t2[k]]; tu[k] = u[t2[k]]; tv[k] = v[t2[k]];
            }
            bubble_triangle(b, out, r.x, r.y, r.w, r.h, tx, ty, tu, tv);
        }
    b->frame.px = out;
    b->frame.w = r.w;
    b->frame.h = r.h;
}


static void bubble_end(struct lm_wm *wm)
{
    struct wm_bubble *b = &wm->bubble;
    if (!b->active)
        return;
    struct lm_rect d = b->shown;
    bubble_free_pictures(wm, b);
    b->active = false;
    damage_rect(wm, &d);
    struct lm_output *o = bubble_output(wm);
    if (o) {                                                        /* the whole output was dimmed, and the real launcher shows now (or nothing) */
        struct lm_rect r = lm_output_rect(o);
        damage_rect(wm, &r);
    }
}

/* The launcher opens (the picture is made now, of the card as it will be) or closes (as it is now). */
static void bubble_begin(struct lm_wm *wm, bool opening)
{
    struct lm_output *o = opening ? bubble_output(wm) : launcher_output(wm);
    if (!o || anim_duration(wm) <= 0 || !wm->srv->render || lm_lock_active(wm->srv->lock))
        return;
    if (wm->bubble.active)
        bubble_end(wm);
    struct wm_bubble *b = &wm->bubble;
    memset(b, 0, sizeof(*b));
    struct lm_scene_item it;
    memset(&it, 0, sizeof(it));
    it.kind = LM_SCENE_LAUNCHER;
    it.output = o;
    launcher_info(wm, o, &it.u.launcher);
    it.u.launcher.veil = 0.0f;                                      /* the dimming is not part of the picture: it fades on its own */
    b->card = it.u.launcher.card;
    struct lm_rect snap = lm_rect_make(b->card.x - 2, b->card.y - 2, b->card.w + 4, b->card.h + 4);
    if (!lm_render_snapshot(wm->srv->render, &it, 1, &snap, &b->ghost))
        return;
    b->card = snap;                                                 /* (the picture is a little larger than the card: the cells map it exactly) */
    bubble_make_sheen(&b->sheen);
    bubble_origin(wm, o, &b->cx0, &b->cy0);
    b->active = true;
    b->closing = !opening;
    b->prog = opening ? 0.0f : 1.0f;
    b->last = lm_now_ms();
    bubble_make_mips(b);
    bubble_render_frame(wm);
    damage_rect(wm, &b->shown);
    struct lm_rect r = lm_output_rect(o);
    damage_rect(wm, &r);
    if (wm->anim_timer && !lm_timer_is_armed(wm->anim_timer))
        lm_timer_arm_ms(wm->anim_timer, ANIM_TICK_MS, ANIM_TICK_MS);
}

/* The opening is cut short (a key, a click): the card is there now. */
static void bubble_finish(struct lm_wm *wm)
{
    if (wm->bubble.active)
        bubble_end(wm);
}

static bool bubble_tick(struct lm_wm *wm, uint64_t now)
{
    struct wm_bubble *b = &wm->bubble;
    if (!b->active)
        return false;
    float step = (float)(now - b->last) / (float)BUBBLE_MS;
    b->last = now;
    b->prog += b->closing ? -step : step;
    if (b->prog >= 1.0f && !b->closing) {
        bubble_end(wm);
        return false;
    }
    if (b->prog <= 0.0f) {
        b->prog = 0.0f;
        if (b->closing) {
            bubble_end(wm);
            return false;
        }
    }
    struct lm_rect old = b->shown;
    bubble_render_frame(wm);
    struct lm_rect d = lm_rect_union(&old, &b->shown);
    damage_rect(wm, &d);
    struct lm_output *o = bubble_output(wm);
    if (o) {                                                        /* the dimming fades with it */
        struct lm_rect r = lm_output_rect(o);
        damage_rect(wm, &r);
    }
    return true;
}


static void anim_tick(void *data)
{
    struct lm_wm *wm = data;
    bool any = false;
    uint64_t now = lm_now_ms();
    if (bubble_tick(wm, now))
        any = true;
    for (int i = 0; i < ANIM_MAX; i++) {
        struct wm_anim *a = &wm->anim[i];
        if (!a->used)
            continue;
        float t = a->dur > 0 ? (float)(now - a->start) / (float)a->dur : 1.0f;
        if (t >= 1.0f) {
            anim_end(wm, a);
            continue;
        }
        struct lm_rect old = a->shown;
        anim_state(a, t, &a->shown, &a->alpha);
        struct lm_rect d = lm_rect_union(&old, &a->shown);
        damage_rect(wm, &d);
        any = true;
    }
    if (wm->wb.active) {
        wobble_tick(wm, now);
        any = true;
    }
    if (overview_tick(wm, now))
        any = true;
    struct lm_window *g = wm->glide_win;
    if (g) {                                                        /* a dragged window follows the pointer with a short delay: it glides */
        float tau = (float)LM_MAX(anim_duration(wm) / 5, 20);       /* ms: how fast it catches up (a fifth of the animation time) */
        float f = 1.0f - expf(-(float)(now - wm->glide_last) / tau);
        wm->glide_last = now;
        int32_t dx = wm->glide_tx - g->x, dy = wm->glide_ty - g->y;
        int32_t sx = (int32_t)lroundf((float)dx * f), sy = (int32_t)lroundf((float)dy * f);
        if (dx && !sx) sx = dx > 0 ? 1 : -1;
        if (dy && !sy) sy = dy > 0 ? 1 : -1;
        if (sx || sy) {
            struct lm_rect before = frame_outer(g);
            g->x += sx;
            g->y += sy;
            damage_rect(wm, &before);
            damage_frame(wm, g);
            update_outputs(wm, g);
            any = true;
        }
        if (g->x == wm->glide_tx && g->y == wm->glide_ty)
            wm->glide_win = NULL;                                   /* arrived (a new motion of the pointer starts it again) */
    }
    if (!any && wm->anim_timer)
        lm_timer_arm_ms(wm->anim_timer, 0, 0);
}

/* The dragged window should be at (x, y): with animations on it glides there (true), else the caller moves it at once (false). */
static bool anim_glide_to(struct lm_wm *wm, struct lm_window *w, int32_t x, int32_t y)
{
    if (anim_duration(wm) <= 0 || !wm->anim_timer || (w->states & LM_WINDOW_FULLSCREEN))
        return false;
    wm->glide_win = w;
    wm->glide_tx = x;
    wm->glide_ty = y;
    if (!lm_timer_is_armed(wm->anim_timer)) {
        wm->glide_last = lm_now_ms();
        lm_timer_arm_ms(wm->anim_timer, ANIM_TICK_MS, ANIM_TICK_MS);
    }
    return true;
}

/* ---- wobbling windows ------------------------------------------------------------------------------------------------- */

static bool wobble_wanted(const struct lm_wm *wm, const struct lm_window *w)
{
    return lm_metrics.wobble > 0 && anim_duration(wm) > 0 && wm->srv->render && !(w->states & LM_WINDOW_FULLSCREEN) && !win_is_calendar(w);
}

/* Where node (i, j) is when the window is rigid. */
static void wobble_rest(const struct wm_wobble *wb, int i, int j, float *x, float *y)
{
    *x = (float)(wb->win->x + wb->off_x) + (float)wb->ghost.w * (float)i / (float)WB_COLS;
    *y = (float)(wb->win->y + wb->off_y) + (float)wb->ghost.h * (float)j / (float)WB_ROWS;
}

/* The rectangle all the nodes lie in. */
static struct lm_rect wobble_bounds(const struct wm_wobble *wb)
{
    float x0 = wb->x[0], x1 = wb->x[0], y0 = wb->y[0], y1 = wb->y[0];
    for (int n = 1; n < WB_N; n++) {
        x0 = fminf(x0, wb->x[n]);
        x1 = fmaxf(x1, wb->x[n]);
        y0 = fminf(y0, wb->y[n]);
        y1 = fmaxf(y1, wb->y[n]);
    }
    return lm_rect_make((int)floorf(x0) - 1, (int)floorf(y0) - 1, (int)ceilf(x1 - x0) + 3, (int)ceilf(y1 - y0) + 3);
}

static void wobble_stop(struct lm_wm *wm)
{
    struct wm_wobble *wb = &wm->wb;
    if (!wb->active)
        return;
    struct lm_rect d = wb->shown;
    if (wb->win) {
        wb->win->anim_hidden = false;
        struct lm_rect f = frame_outer(wb->win);
        d = lm_rect_union(&d, &f);
    }
    lm_render_ghost_free(wm->srv->render, &wb->ghost);
    memset(wb, 0, sizeof(*wb));
    damage_rect(wm, &d);
}

/* The window is being dragged: from now on it wobbles. (px, py) is the grab point. */
static void wobble_start(struct lm_wm *wm, struct lm_window *w, double px, double py)
{
    struct wm_wobble *wb = &wm->wb;
    struct lm_ghost g;
    struct lm_rect home;
    if (wb->active)
        wobble_stop(wm);
    if (!wobble_wanted(wm, w) || !anim_prepare(wm, w, &g, &home))
        return;
    memset(wb, 0, sizeof(*wb));
    wb->active = true;
    wb->win = w;
    wb->ghost = g;
    wb->off_x = home.x - w->x;
    wb->off_y = home.y - w->y;
    wb->gx = (float)px - (float)home.x;
    wb->gy = (float)py - (float)home.y;
    for (int j = 0; j <= WB_ROWS; j++)
        for (int i = 0; i <= WB_COLS; i++)
            wobble_rest(wb, i, j, &wb->x[j * (WB_COLS + 1) + i], &wb->y[j * (WB_COLS + 1) + i]);
    wb->last = lm_now_ms();
    wb->shown = wobble_bounds(wb);
    w->anim_hidden = true;
    damage_rect(wm, &wb->shown);
    if (wm->anim_timer && !lm_timer_is_armed(wm->anim_timer))
        lm_timer_arm_ms(wm->anim_timer, ANIM_TICK_MS, ANIM_TICK_MS);
}

/* One physics step of dt seconds: every node is a damped spring to its rigid place, soft far from the grab point, and drawn
 * towards the mean of its neighbours (so the bend travels through the window like a wave). */
static void wobble_step(struct wm_wobble *wb, float dt)
{
    const int stride = WB_COLS + 1;
    float s = (float)lm_metrics.wobble / 100.0f;
    float kn = 1600.0f, kf = 1600.0f * powf(0.07f, s), reach = 0.7f * hypotf((float)wb->ghost.w, (float)wb->ghost.h);
    float ox = (float)(wb->win->x + wb->off_x), oy = (float)(wb->win->y + wb->off_y);
    static float dx[WB_N], dy[WB_N];                            /* displacement from the rigid place */
    float rx[WB_N], ry[WB_N];
    for (int j = 0; j <= WB_ROWS; j++)
        for (int i = 0; i <= WB_COLS; i++) {
            int n = j * stride + i;
            wobble_rest(wb, i, j, &rx[n], &ry[n]);
            dx[n] = wb->x[n] - rx[n];
            dy[n] = wb->y[n] - ry[n];
        }
    for (int j = 0; j <= WB_ROWS; j++)
        for (int i = 0; i <= WB_COLS; i++) {
            int n = j * stride + i, cnt = 0;
            float ax = 0, ay = 0;
            if (i > 0) { ax += dx[n - 1]; ay += dy[n - 1]; cnt++; }
            if (i < WB_COLS) { ax += dx[n + 1]; ay += dy[n + 1]; cnt++; }
            if (j > 0) { ax += dx[n - stride]; ay += dy[n - stride]; cnt++; }
            if (j < WB_ROWS) { ax += dx[n + stride]; ay += dy[n + stride]; cnt++; }
            ax = ax / (float)cnt - dx[n];
            ay = ay / (float)cnt - dy[n];
            float w = fminf(hypotf(rx[n] - ox - wb->gx, ry[n] - oy - wb->gy) / reach, 1.0f);
            float k = kn + (kf - kn) * w, c = 2.0f * 0.32f * sqrtf(k);
            wb->vx[n] += (-k * dx[n] - c * wb->vx[n] + 90.0f * ax) * dt;
            wb->vy[n] += (-k * dy[n] - c * wb->vy[n] + 90.0f * ay) * dt;
        }
    for (int n = 0; n < WB_N; n++) {
        wb->x[n] += wb->vx[n] * dt;
        wb->y[n] += wb->vy[n] * dt;
    }
}

static void wobble_tick(struct lm_wm *wm, uint64_t now)
{
    struct wm_wobble *wb = &wm->wb;
    float dt = fminf((float)(now - wb->last) / 1000.0f, 0.05f);
    wb->last = now;
    for (float t = 0; t < dt; t += 0.008f)
        wobble_step(wb, fminf(0.008f, dt - t));
    if (++wb->ticks % 6 == 0) {                                 /* the content stays alive: a fresh picture every ~100 ms */
        struct lm_ghost g;
        struct lm_rect home;
        if (anim_prepare(wm, wb->win, &g, &home)) {
            if (g.w == wb->ghost.w && g.h == wb->ghost.h) {
                lm_render_ghost_free(wm->srv->render, &wb->ghost);
                wb->ghost = g;
            } else {
                lm_render_ghost_free(wm->srv->render, &g);
            }
        }
    }
    struct lm_rect old = wb->shown;
    wb->shown = wobble_bounds(wb);
    struct lm_rect d = lm_rect_union(&old, &wb->shown);
    damage_rect(wm, &d);
    if (wb->released) {                                         /* settled when every node is back and still */
        float dev = 0, spd = 0;
        for (int j = 0; j <= WB_ROWS; j++)
            for (int i = 0; i <= WB_COLS; i++) {
                int n = j * (WB_COLS + 1) + i;
                float rx, ry;
                wobble_rest(wb, i, j, &rx, &ry);
                dev = fmaxf(dev, fmaxf(fabsf(wb->x[n] - rx), fabsf(wb->y[n] - ry)));
                spd = fmaxf(spd, fmaxf(fabsf(wb->vx[n]), fabsf(wb->vy[n])));
            }
        if (dev < 0.4f && spd < 6.0f)
            wobble_stop(wm);
    }
}

/* ---- the desktop overview ----------------------------------------------------------------------------------------------- */

#define OV_STRIP_H 56                       /* the band of workspace cells at the top */
#define OV_LABEL_H 20
#define OV_GAP 28

static struct lm_output *overview_output(const struct lm_wm *wm)
{
    if (!wm->ov.open || !wm->srv->backend)
        return NULL;
    struct lm_output *o;
    lm_list_for_each(o, &wm->srv->backend->outputs, link)
        if (!strcmp(o->name, wm->ov.out))
            return o;
    return NULL;
}

/* True while the window is shown as a picture in the overview (the scene and the picking leave the real one out). */
static bool ov_hides(const struct lm_wm *wm, const struct lm_window *w)
{
    if (!wm->ov.open)
        return false;
    for (int i = 0; i < wm->ov.n; i++)
        if (wm->ov.item[i].win == w)
            return true;
    return false;
}

static void overview_free(struct lm_wm *wm)
{
    for (int i = 0; i < wm->ov.n; i++)
        lm_render_ghost_free(wm->srv->render, &wm->ov.item[i].ghost);
    wm->ov.n = 0;
}

static void overview_damage(struct lm_wm *wm)
{
    struct lm_output *o = overview_output(wm);
    if (o) {
        struct lm_rect r = lm_output_rect(o);
        damage_rect(wm, &r);
    }
}

/* Slots for the pictures: the grid (rows of `cols`, the last row centred) that makes them largest, below the workspace cells. */
static void overview_layout(struct lm_wm *wm, struct lm_output *o)
{
    struct wm_overview *ov = &wm->ov;
    int n = ov->n;
    if (n == 0)
        return;
    int top = o->y + LM_PANEL_HEIGHT + OV_STRIP_H + 36, bottom = o->y + o->height - 24;
    struct lm_output *dock_o = dock_output(wm);
    if (dock_o == o) {
        struct dock_geo g;
        dock_geometry(o, &g);
        bottom = LM_MIN(bottom, g.strip.y - 12);
    }
    int left = o->x + 40, aw = o->width - 80, ah = LM_MAX(bottom - top, 120);
    int best_cols = 1;
    float best = -1;
    for (int cols = 1; cols <= n; cols++) {
        int rows = (n + cols - 1) / cols, cw = (aw - (cols - 1) * OV_GAP) / cols, ch = (ah - (rows - 1) * OV_GAP) / rows;
        if (cw < 48 || ch < 48 + OV_LABEL_H)
            continue;
        float sum = 0;
        for (int i = 0; i < n; i++) {
            float sx = (float)cw / (float)ov->item[i].home.w, sy = (float)(ch - OV_LABEL_H - 6) / (float)ov->item[i].home.h;
            sum += fminf(1.0f, fminf(sx, sy));
        }
        if (sum > best) {
            best = sum;
            best_cols = cols;
        }
    }
    int cols = best_cols, rows = (n + cols - 1) / cols, cw = (aw - (cols - 1) * OV_GAP) / cols, ch = (ah - (rows - 1) * OV_GAP) / rows;
    cw = LM_MAX(cw, 48);
    ov->cols = cols;
    for (int r = 0; r < rows; r++) {                             /* a row: the pictures side by side with a gap, the row centred */
        int first = r * cols, last = LM_MIN(first + cols, n), total = (last - first - 1) * OV_GAP;
        int ws[LM_OVERVIEW_MAX], hs[LM_OVERVIEW_MAX];
        for (int i = first; i < last; i++) {
            struct lm_rect hm = ov->item[i].home;
            float sc = fminf(1.0f, fminf((float)cw / (float)hm.w, (float)(ch - OV_LABEL_H - 6) / (float)hm.h));
            ws[i - first] = LM_MAX((int)((float)hm.w * sc), 8);
            hs[i - first] = LM_MAX((int)((float)hm.h * sc), 8);
            total += ws[i - first];
        }
        int x = left + (aw - total) / 2, y0 = top + r * (ch + OV_GAP);
        for (int i = first; i < last; i++) {
            ov->item[i].slot = lm_rect_make(x, y0 + (ch - OV_LABEL_H - 6 - hs[i - first]) / 2, ws[i - first], hs[i - first]);
            x += ws[i - first] + OV_GAP;
        }
    }
}

/* The pictures of this workspace's windows on the overview's output, in taskbar order. */
static void overview_build(struct lm_wm *wm)
{
    struct wm_overview *ov = &wm->ov;
    struct lm_output *o = overview_output(wm);
    struct lm_window *prev_sel = ov->sel >= 0 && ov->sel < ov->n ? ov->item[ov->sel].win : NULL;
    overview_free(wm);
    ov->hover = ov->hover_ws = ov->press = -1;
    ov->drag = false;
    if (!o)
        return;
    struct lm_window *list[LM_OVERVIEW_MAX];
    int n = 0;
    struct lm_window *w;
    lm_list_for_each(w, &wm->windows, link) {
        if (!w->mapped || w->workspace != wm->workspace || win_is_calendar(w) || (w->states & LM_WINDOW_FULLSCREEN) ||
            window_output(wm, w) != o || n >= LM_OVERVIEW_MAX)
            continue;
        int i = n++;
        for (; i > 0 && WIN(list[i - 1])->id > WIN(w)->id; i--)
            list[i] = list[i - 1];
        list[i] = w;
    }
    for (int i = 0; i < n; i++) {
        struct wm_ov_item *it = &ov->item[ov->n];
        memset(it, 0, sizeof(*it));
        if (!anim_prepare(wm, list[i], &it->ghost, &it->home))
            continue;                                           /* (no picture: the real window stays) */
        it->win = list[i];
        ov->n++;
    }
    ov->sel = ov->n > 0 ? ov->n - 1 : -1;
    for (int i = 0; i < ov->n; i++)
        if (ov->item[i].win == (prev_sel ? prev_sel : wm->focused))
            ov->sel = i;
    overview_layout(wm, o);
}

/* The state of the control socket / the scene: where picture i is now. */
static struct lm_rect overview_item_rect(const struct wm_overview *ov, int i)
{
    return anim_lerp_rect(&ov->item[i].home, &ov->item[i].slot, ease_smooth(ov->prog));
}

static void overview_info(const struct lm_wm *wm, struct lm_output *o, struct lm_overview_info *oi)
{
    const struct wm_overview *ov = &wm->ov;
    memset(oi, 0, sizeof(*oi));
    oi->rect = lm_output_rect(o);
    oi->dim = (int)(0xb4 * ease_smooth(ov->prog));
    oi->ui = !ov->closing && ov->prog >= 1.0f;
    int cw = LM_MIN(96, (o->width - 80) / LM_NUM_WORKSPACES - 12), total = LM_NUM_WORKSPACES * cw + (LM_NUM_WORKSPACES - 1) * 12;
    for (int i = 0; i < LM_NUM_WORKSPACES; i++) {
        oi->ws[i].rect = lm_rect_make(o->x + (o->width - total) / 2 + i * (cw + 12), o->y + LM_PANEL_HEIGHT + 14, cw, OV_STRIP_H - 14);
        oi->ws[i].active = i == wm->workspace;
        oi->ws[i].hover = i == ov->hover_ws;
        const struct lm_window *w;
        lm_list_for_each(w, &wm->windows, link)
            if (w->mapped && w->workspace == i && !win_is_calendar(w))
                oi->ws[i].count++;
    }
    oi->n = ov->n;
    for (int i = 0; i < ov->n; i++) {
        const struct lm_rect *s = &ov->item[i].slot;
        oi->item[i].frame = lm_rect_make(s->x - 6, s->y - 6, s->w + 12, s->h + 12 + OV_LABEL_H + 2);
        int lw = LM_MAX(s->w + 12, 96);
        oi->item[i].label = lm_rect_make(s->x + s->w / 2 - lw / 2, s->y + s->h + 6, lw, OV_LABEL_H - 2);
        snprintf(oi->item[i].title, sizeof(oi->item[i].title), "%s", ov->item[i].win->title);
        oi->item[i].sel = i == ov->sel;
        oi->item[i].hover = i == ov->hover;
        oi->item[i].minimized = ov->item[i].win->minimized;
    }
}

static void overview_finish(struct lm_wm *wm)
{
    if (!wm->ov.open)
        return;
    overview_damage(wm);
    overview_free(wm);
    wm->ov.open = wm->ov.closing = false;
    damage_panels(wm);
    refresh_pointer(wm);
}

/* Closes the overview; `activate` >= 0 first goes to that picture's window (a minimized one comes back, without its own fly-in). */
static void overview_close(struct lm_wm *wm, int activate)
{
    struct wm_overview *ov = &wm->ov;
    if (!ov->open)
        return;
    if (activate >= 0 && activate < ov->n && !ov->closing) {
        wm->anim_suppress = true;
        lm_wm_focus_window(wm, ov->item[activate].win);
        wm->anim_suppress = false;
    }
    if (ov->closing)
        return;
    int dur = anim_duration(wm);
    if (dur <= 0 || ov->n == 0) {
        overview_finish(wm);
        return;
    }
    ov->closing = true;
    ov->from = ov->prog;
    ov->start = lm_now_ms();
    ov->dur = dur;
    overview_damage(wm);
    if (wm->anim_timer && !lm_timer_is_armed(wm->anim_timer))
        lm_timer_arm_ms(wm->anim_timer, ANIM_TICK_MS, ANIM_TICK_MS);
}

static void overview_set(struct lm_wm *wm, bool open)
{
    struct wm_overview *ov = &wm->ov;
    if (!open) {
        overview_close(wm, -1);
        return;
    }
    if (lm_lock_active(wm->srv->lock) || !wm->srv->backend)
        return;
    if (ov->open) {
        if (ov->closing) {                                      /* asked again while it flies back: it turns round */
            ov->closing = false;
            ov->from = ov->prog;
            ov->start = lm_now_ms();
            ov->dur = anim_duration(wm);
            if (wm->anim_timer && !lm_timer_is_armed(wm->anim_timer))
                lm_timer_arm_ms(wm->anim_timer, ANIM_TICK_MS, ANIM_TICK_MS);
        }
        return;
    }
    menu_set(wm, false);
    about_set(wm, false, NULL);
    launcher_set(wm, false, NULL);
    power_set(wm, false, NULL);
    sw_close(wm);
    clip_nav_set(wm, false);
    if (wm->focused)
        lm_xdg_dismiss_popups(wm->focused->surface);
    struct lm_output *o = NULL;
    if (wm->srv->seat) {
        double px, py;
        lm_seat_pointer_position(wm->srv->seat, &px, &py);
        o = lm_output_nearest(wm->srv, (int32_t)px, (int32_t)py);
    }
    if (!o)
        o = lm_output_primary(wm->srv);
    if (!o)
        return;
    memset(ov, 0, sizeof(*ov));
    snprintf(ov->out, sizeof(ov->out), "%s", o->name);
    ov->open = true;
    ov->sel = -1;
    overview_build(wm);
    ov->dur = anim_duration(wm);
    ov->start = lm_now_ms();
    ov->from = 0;
    ov->prog = ov->dur > 0 ? 0.0f : 1.0f;
    overview_damage(wm);
    damage_panels(wm);
    if (ov->dur > 0 && wm->anim_timer && !lm_timer_is_armed(wm->anim_timer))
        lm_timer_arm_ms(wm->anim_timer, ANIM_TICK_MS, ANIM_TICK_MS);
    refresh_pointer(wm);
}

/* The set of windows changed (one mapped, went away, moved to another workspace, the workspace was switched): take the pictures again. */
static void overview_changed(struct lm_wm *wm)
{
    if (!wm->ov.open)
        return;
    if (wm->ov.closing) {
        overview_finish(wm);
        return;
    }
    overview_build(wm);
    overview_damage(wm);
}

/* True while it still moves (the timer keeps running). */
static bool overview_tick(struct lm_wm *wm, uint64_t now)
{
    struct wm_overview *ov = &wm->ov;
    if (!ov->open)
        return false;
    float target = ov->closing ? 0.0f : 1.0f;
    if (ov->dur <= 0 || ov->prog == target)
        return false;
    float p = fminf((float)(now - ov->start) / (float)ov->dur, 1.0f);
    ov->prog = ov->from + (target - ov->from) * p;
    overview_damage(wm);
    if (p >= 1.0f) {
        ov->prog = target;
        if (ov->closing)
            overview_finish(wm);
        return false;
    }
    return true;
}

static bool overview_pick(const struct lm_wm *wm, int ix, int iy, struct lm_pick *out)
{
    struct lm_output *o = overview_output(wm);
    if (!o || lm_lock_active(wm->srv->lock) || wm->ov.closing)
        return false;
    struct lm_rect orc = lm_output_rect(o);
    if (!lm_rect_contains(&orc, ix, iy) || iy < o->y + LM_PANEL_HEIGHT)
        return false;                                           /* (the panel above stays in use) */
    struct lm_overview_info oi;
    overview_info(wm, o, &oi);
    out->kind = LM_PICK_OVERVIEW;
    out->rect = orc;
    for (int i = 0; i < LM_NUM_WORKSPACES; i++)
        if (lm_rect_contains(&oi.ws[i].rect, ix, iy)) {
            out->kind = LM_PICK_OVERVIEW_WS;
            out->index = i;
            return true;
        }
    for (int i = wm->ov.n - 1; i >= 0; i--)
        if (lm_rect_contains(&oi.item[i].frame, ix, iy)) {
            out->kind = LM_PICK_OVERVIEW_ITEM;
            out->index = i;
            out->window = wm->ov.item[i].win;
            return true;
        }
    return true;
}

static void overview_switch_workspace(struct lm_wm *wm, int ws)
{
    if (ws < 0 || ws >= LM_NUM_WORKSPACES || ws == wm->workspace)
        return;
    lm_wm_switch_workspace(wm, ws);                              /* (its hook takes the pictures of the new workspace) */
}

static void overview_key(struct lm_wm *wm, uint32_t code, bool repeat, uint32_t mods)
{
    struct wm_overview *ov = &wm->ov;
    int n = ov->n, sel = ov->sel;
    if (ov->closing)
        return;
    if (!repeat && (code == KEY_ESC || (code == KEY_W && (mods & LM_MOD_SUPER)))) {
        overview_close(wm, -1);
        return;
    }
    if (code >= KEY_1 && code < KEY_1 + LM_NUM_WORKSPACES && !repeat) {
        overview_switch_workspace(wm, (int)(code - KEY_1));
        return;
    }
    if (n == 0)
        return;
    int step = 0;
    if (code == KEY_RIGHT || (code == KEY_TAB && !(mods & LM_MOD_SHIFT))) step = 1;
    else if (code == KEY_LEFT || (code == KEY_TAB && (mods & LM_MOD_SHIFT))) step = -1;
    else if (code == KEY_DOWN) step = ov->cols;
    else if (code == KEY_UP) step = -ov->cols;
    if (step) {
        int to = sel < 0 ? 0 : sel + step;
        if (abs(step) > 1 && (to < 0 || to >= n))
            to = sel;                                           /* (Up / Down stop at the first / last row) */
        else
            to = (to % n + n) % n;
        if (to != ov->sel) {
            ov->sel = to;
            overview_damage(wm);
        }
        return;
    }
    if (!repeat && (code == KEY_ENTER || code == KEY_KPENTER || code == KEY_SPACE) && sel >= 0)
        overview_close(wm, sel);
    else if (!repeat && (code == KEY_DELETE) && sel >= 0)
        lm_wm_window_close(ov->item[sel].win);
}

/* A press on a picture that moves 8 px turns into a drag: the picture follows the pointer and can be dropped on a workspace cell. */
static void overview_drag_motion(struct lm_wm *wm)
{
    struct wm_overview *ov = &wm->ov;
    if (!ov->open || ov->closing || wm->press_panel_kind != LM_PICK_OVERVIEW_ITEM || !wm->srv->seat)
        return;
    double x, y;
    lm_seat_pointer_position(wm->srv->seat, &x, &y);
    if (!ov->drag) {
        if (!drag_far_enough(wm, x, y) || wm->press_panel_ws < 0 || wm->press_panel_ws >= ov->n)
            return;
        ov->drag = true;
        ov->drag_i = wm->press_panel_ws;
        ov->hover = -1;
    }
    wm->drag_x = x;
    wm->drag_y = y;
    overview_damage(wm);
}

/* The button was let go after a drag: on a workspace cell the window moves there. */
static void overview_drag_finish(struct lm_wm *wm, const struct lm_pick *pick)
{
    struct wm_overview *ov = &wm->ov;
    int i = ov->drag_i;
    ov->drag = false;
    overview_damage(wm);
    if (pick->kind == LM_PICK_OVERVIEW_WS && i >= 0 && i < ov->n && pick->index != ov->item[i].win->workspace)
        move_to_workspace(wm, ov->item[i].win, pick->index);              /* (its hook takes the pictures again) */
}

/* A click (`button`, release) on a pick of the overview. */
static void overview_click(struct lm_wm *wm, const struct lm_pick *pick, uint32_t button)
{
    if (pick->kind == LM_PICK_OVERVIEW_ITEM && pick->index >= 0 && pick->index < wm->ov.n) {
        if (button == BTN_LEFT)
            overview_close(wm, pick->index);
        else if (button == BTN_MIDDLE)
            lm_wm_window_close(wm->ov.item[pick->index].win);
    } else if (pick->kind == LM_PICK_OVERVIEW_WS && button == BTN_LEFT) {
        overview_switch_workspace(wm, pick->index);
    } else if (pick->kind == LM_PICK_OVERVIEW && button == BTN_LEFT) {
        overview_close(wm, -1);
    }
}

void lm_wm_overview_set(struct lm_wm *wm, bool open) { overview_set(wm, open); }
bool lm_wm_overview_is_open(const struct lm_wm *wm) { return wm->ov.open; }

size_t lm_wm_overview_describe(struct lm_wm *wm, char *buf, size_t size)
{
    const struct wm_overview *ov = &wm->ov;
    size_t n = (size_t)snprintf(buf, size, "overview open=%d closing=%d n=%d sel=%d cols=%d prog=%.2f ws=%d\n", ov->open, ov->closing, ov->n, ov->sel,
                                ov->cols, (double)ov->prog, wm->workspace);
    for (int i = 0; i < ov->n && n < size; i++) {
        struct lm_rect r = overview_item_rect(ov, i);
        n += (size_t)snprintf(buf + n, size - n, "item %d x=%d y=%d w=%d h=%d slot=%d,%d,%d,%d title=\"%s\"\n", i, r.x, r.y, r.w, r.h, ov->item[i].slot.x,
                              ov->item[i].slot.y, ov->item[i].slot.w, ov->item[i].slot.h, ov->item[i].win->title);
    }
    return n < size ? n : size - 1;
}

/* A window has just been mapped: its picture grows out of nothing while the real one waits. */
static void anim_open(struct lm_wm *wm, struct lm_window *w)
{
    struct lm_ghost g;
    struct lm_rect home;
    if (!anim_wanted(wm, w) || !anim_prepare(wm, w, &g, &home))
        return;
    anim_start(wm, ANIM_OPEN, w, &g, &home);
}

/* Minimize (kind MINIMIZE, before the window is hidden) or restore (RESTORE, before it is shown): the picture flies between the window and its button. */
static void anim_fly(struct lm_wm *wm, enum anim_kind kind, struct lm_window *w)
{
    struct lm_ghost g;
    struct lm_rect home;
    if (!anim_wanted(wm, w) || !w->mapped || w->workspace != wm->workspace || !anim_prepare(wm, w, &g, &home))
        return;
    anim_start(wm, kind, w, &g, &home);
}

/* The window was asked to close: keep its picture, a close that follows (within 3 s) shows it fading away. */
static void anim_remember_close(struct lm_wm *wm, struct lm_window *w)
{
    if (!anim_wanted(wm, w) || !w->mapped || w->minimized || w->workspace != wm->workspace)
        return;
    struct lm_ghost *g = calloc(1, sizeof(*g));
    if (!g)
        return;
    if (!anim_prepare(wm, w, g, &w->close_rect)) {
        free(g);
        return;
    }
    if (w->close_ghost) {
        free(w->close_ghost->px);
        free(w->close_ghost);
    }
    w->close_ghost = g;
    w->close_ms = lm_now_ms();
}

/* The window goes away (unmapped): shows the picture taken when it was asked to close, if there is a fresh one. */
static void anim_play_close(struct lm_wm *wm, struct lm_window *w)
{
    if (wm->wb.active && wm->wb.win == w)
        wobble_stop(wm);
    for (int i = 0; i < ANIM_MAX; i++)                              /* an opening or flying picture of it ends now */
        if (wm->anim[i].used && wm->anim[i].win == w)
            anim_end(wm, &wm->anim[i]);
    struct lm_ghost *g = w->close_ghost;
    if (!g)
        return;
    w->close_ghost = NULL;
    if (anim_wanted(wm, w) && !w->minimized && w->workspace == wm->workspace && lm_now_ms() - w->close_ms < 3000)
        anim_start(wm, ANIM_CLOSE, NULL, g, &w->close_rect);
    free(g->px);
    free(g);
}

size_t lm_wm_anim_describe(struct lm_wm *wm, char *buf, size_t size)
{
    static const char *const names[] = { "open", "close", "minimize", "restore" };
    size_t n = 0;
    int count = 0;
    for (int i = 0; i < ANIM_MAX; i++)
        count += wm->anim[i].used;
    n += (size_t)snprintf(buf + n, size - n, "anim n=%d dur=%d wobble=%d\n", count, anim_duration(wm), wm->wb.active);
    for (int i = 0; i < ANIM_MAX && n < size; i++) {
        const struct wm_anim *a = &wm->anim[i];
        if (!a->used)
            continue;
        n += (size_t)snprintf(buf + n, size - n, "item kind=%s x=%d y=%d w=%d h=%d alpha=%d\n", names[a->kind], a->shown.x, a->shown.y,
                              a->shown.w, a->shown.h, a->alpha);
    }
    if (wm->bubble.active && n < size)                           /* the launcher's bubble: "bubble opening|closing prog=0..1 x= y= w= h=" (the box that covers it) */
        n += (size_t)snprintf(buf + n, size - n, "bubble %s prog=%.3f x=%d y=%d w=%d h=%d\n", wm->bubble.closing ? "closing" : "opening", (double)wm->bubble.prog,
                              wm->bubble.shown.x, wm->bubble.shown.y, wm->bubble.shown.w, wm->bubble.shown.h);
    return n < size ? n : size - 1;
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
        if (!win_visible(wm, w) || w->anim_hidden || ov_hides(wm, w))
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

    for (int i = 0; i < ANIM_MAX; i++) {                         /* pictures of windows that open / close / fly to their button */
        struct wm_anim *a = &wm->anim[i];
        if (!a->used || (clip && !lm_rect_overlaps(&a->shown, clip)))
            continue;
        memset(&it, 0, sizeof(it));
        it.kind = LM_SCENE_GHOST;
        it.u.ghost.ghost = &a->ghost;
        it.u.ghost.dst = a->shown;
        it.u.ghost.alpha = a->alpha;
        if (!fn(&it, data))
            return;
    }

    if (wm->wb.active) {                                         /* the wobbling window: its picture cut into cells that follow the spring nodes */
        const struct wm_wobble *wb = &wm->wb;
        const int stride = WB_COLS + 1;
        for (int j = 0; j < WB_ROWS; j++)
            for (int i = 0; i < WB_COLS; i++) {
                int n[4] = { j * stride + i, j * stride + i + 1, (j + 1) * stride + i, (j + 1) * stride + i + 1 };
                float x0 = wb->x[n[0]], x1 = x0, y0 = wb->y[n[0]], y1 = y0;
                for (int k = 1; k < 4; k++) {
                    x0 = fminf(x0, wb->x[n[k]]);
                    x1 = fmaxf(x1, wb->x[n[k]]);
                    y0 = fminf(y0, wb->y[n[k]]);
                    y1 = fmaxf(y1, wb->y[n[k]]);
                }
                struct lm_rect dst = lm_rect_make((int)floorf(x0), (int)floorf(y0), (int)ceilf(x1) - (int)floorf(x0), (int)ceilf(y1) - (int)floorf(y0));
                if (clip && !lm_rect_overlaps(&dst, clip))
                    continue;
                int sx0 = wb->ghost.w * i / WB_COLS, sx1 = wb->ghost.w * (i + 1) / WB_COLS;
                int sy0 = wb->ghost.h * j / WB_ROWS, sy1 = wb->ghost.h * (j + 1) / WB_ROWS;
                memset(&it, 0, sizeof(it));
                it.kind = LM_SCENE_GHOST;
                it.u.ghost.ghost = (struct lm_ghost *)&wb->ghost;
                it.u.ghost.dst = dst;
                it.u.ghost.src = lm_rect_make(sx0, sy0, sx1 - sx0, sy1 - sy0);
                it.u.ghost.alpha = 255;
                if (!fn(&it, data))
                    return;
            }
    }

    if (wm->ov.open) {                                           /* the desktop overview: the dimmed output, then the windows' pictures */
        struct lm_output *oo = overview_output(wm);
        if (oo) {
            struct lm_rect orc = lm_output_rect(oo);
            if (!clip || lm_rect_overlaps(&orc, clip)) {
                memset(&it, 0, sizeof(it));
                it.kind = LM_SCENE_OVERVIEW;
                it.output = oo;
                overview_info(wm, oo, &it.u.overview);
                if (!fn(&it, data))
                    return;
                for (int pass = 0; pass < 2; pass++)                  /* the carried picture last, above the others */
                    for (int i = 0; i < wm->ov.n; i++) {
                        bool carried = wm->ov.drag && i == wm->ov.drag_i;
                        if (carried != (pass == 1))
                            continue;
                        memset(&it, 0, sizeof(it));
                        it.kind = LM_SCENE_GHOST;
                        it.u.ghost.ghost = (struct lm_ghost *)&wm->ov.item[i].ghost;
                        it.u.ghost.dst = overview_item_rect(&wm->ov, i);
                        it.u.ghost.alpha = wm->ov.item[i].win->minimized ? 170 : 255;
                        if (carried) {                                /* its place in the grid stays as a faint copy, the picture itself is small and follows the pointer */
                            struct lm_rect home = it.u.ghost.dst;
                            it.u.ghost.alpha = 70;
                            if (!clip || lm_rect_overlaps(&home, clip))
                                if (!fn(&it, data))
                                    return;
                            int cw = LM_MAX(home.w * 55 / 100, 24), chh = LM_MAX(home.h * 55 / 100, 16);
                            it.u.ghost.dst = lm_rect_make((int)wm->drag_x - cw / 2, (int)wm->drag_y - chh / 2, cw, chh);
                            it.u.ghost.alpha = 235;
                        }
                        if (clip && !lm_rect_overlaps(&it.u.ghost.dst, clip))
                            continue;
                        if (!fn(&it, data))
                            return;
                    }
            }
        }
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

    if (wm->bubble.active) {                                     /* the launcher is a bubble: the dimming, then the card's picture in cells, then the sheen */
        const struct wm_bubble *b = &wm->bubble;
        struct lm_output *bo = bubble_output(wm);
        struct lm_rect orc = bo ? lm_output_rect(bo) : (struct lm_rect){ 0 };
        if (bo && (!clip || lm_rect_overlaps(&orc, clip))) {
            memset(&it, 0, sizeof(it));
            it.kind = LM_SCENE_LAUNCHER;
            it.output = bo;
            launcher_info(wm, bo, &it.u.launcher);
            it.u.launcher.veil_only = true;
            it.u.launcher.veil = smooth01(b->prog);
            if (!fn(&it, data))
                return;
        }
        float s = b->s;
        if (b->frame.px && (!clip || lm_rect_overlaps(&b->shown, clip))) {
            memset(&it, 0, sizeof(it));
            it.kind = LM_SCENE_GHOST;
            it.u.ghost.ghost = (struct lm_ghost *)&b->frame;
            it.u.ghost.dst = b->shown;
            it.u.ghost.alpha = b->alpha;
            if (!fn(&it, data))
                return;
        }
        float sheen_a = powf(1.0f - LM_CLAMP(s, 0.0f, 1.0f), 1.3f);
        if (b->sheen.px && sheen_a > 0.02f) {
            float ccx = (float)b->card.x + (float)b->card.w / 2.0f, ccy = (float)b->card.y + (float)b->card.h / 2.0f, pe = smooth01(b->prog);
            float R = BUBBLE_R0 + (0.32f * (float)LM_MIN(b->card.w, b->card.h) - BUBBLE_R0) * pe;
            float cx = b->cx0 + (ccx - b->cx0) * pe, cy = b->cy0 + (ccy - b->cy0) * pe;
            struct lm_rect dst = lm_rect_make((int)lroundf(cx - R), (int)lroundf(cy - R), (int)lroundf(2.0f * R), (int)lroundf(2.0f * R));
            if (!clip || lm_rect_overlaps(&dst, clip)) {
                memset(&it, 0, sizeof(it));
                it.kind = LM_SCENE_GHOST;
                it.u.ghost.ghost = (struct lm_ghost *)&b->sheen;
                it.u.ghost.dst = dst;
                it.u.ghost.alpha = (int)lroundf(255.0f * sheen_a * fminf(b->prog / 0.05f, 1.0f));
                if (!fn(&it, data))
                    return;
            }
        }
    }

    if (wm->launcher_open && !(wm->bubble.active && !wm->bubble.closing)) {     /* the launcher dialog (not while it is still a bubble) */
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

    if (wm->menu_open && !lm_lock_active(wm->srv->lock) && (!clip || lm_rect_overlaps(&wm->menu_rect, clip))) {    /* a menu: above the dialogs */
        memset(&it, 0, sizeof(it));
        it.kind = LM_SCENE_MENU;
        it.output = lm_output_nearest(wm->srv, wm->menu_rect.x + wm->menu_rect.w / 2, wm->menu_rect.y + wm->menu_rect.h / 2);
        if (it.output) {
            menu_info(wm, &it.u.menu);
            if (!fn(&it, data))
                return;
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
    if (wm->menu_open && !lm_lock_active(wm->srv->lock)) {   /* a menu (above every dialog) takes every pick while it is open */
        struct lm_menu_info mi;
        menu_info(wm, &mi);
        out->kind = lm_rect_contains(&mi.rect, ix, iy) ? LM_PICK_MENU : LM_PICK_MENU_BACKDROP;
        for (int i = 0; i < mi.n; i++)
            if (mi.enabled[i] && lm_rect_contains(&mi.entry[i], ix, iy)) {
                out->kind = LM_PICK_MENU_ITEM;
                out->index = i;
            }
        out->rect = mi.rect;
        return;
    }
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
        for (int i = 0; i < LM_POWER_N; i++)
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
    if (overview_pick(wm, ix, iy, out))                          /* the desktop overview takes everything below the panel (and the dock) */
        return;

    struct lm_window *w;
    lm_list_for_each_reverse(w, &wm->windows, link) {
        if (!win_visible(wm, w) || ov_hides(wm, w))
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
    overview_drag_motion(wm);
    struct lm_window *hw = NULL;
    uint32_t bits = 0;
    int pws = -1;
    bool about_btn = false, launcher = false, power_btn = false, layout_btn = false, sticky_btn = false, osk_btn = false;
    int power_item = -1, clip_code = 0, dm_item = -1;
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
    case LM_PICK_OVERVIEW_ITEM: case LM_PICK_OVERVIEW_WS: shape = LM_CURSOR_POINTER; break;
    case LM_PICK_NOTIFY: shape = LM_CURSOR_POINTER; break;
    case LM_PICK_PANEL_LAUNCHER: launcher = true; shape = LM_CURSOR_POINTER; break;
    case LM_PICK_ABOUT_BUTTON: about_btn = true; shape = LM_CURSOR_POINTER; break;
    case LM_PICK_DOCK_ITEM: shape = LM_CURSOR_POINTER; break;
    case LM_PICK_LAUNCHER_ITEM: case LM_PICK_LAUNCHER_REMOVE: case LM_PICK_LAUNCHER_ADD: case LM_PICK_LAUNCHER_BUTTON:
    case LM_PICK_LAUNCHER_KIND: case LM_PICK_LAUNCHER_HELP: case LM_PICK_LAUNCHER_HELPCLOSE: shape = LM_CURSOR_POINTER; break;
    case LM_PICK_CLIP_UP: clip_code = 1; shape = LM_CURSOR_POINTER; break;
    case LM_PICK_CLIP_DOWN: clip_code = 2; shape = LM_CURSOR_POINTER; break;
    case LM_PICK_CLIP_CLEAR: clip_code = 3; shape = LM_CURSOR_POINTER; break;
    case LM_PICK_CLIP_DELETE: clip_code = 10 + pick->index - clip_top(wm); shape = LM_CURSOR_POINTER; break;
    case LM_PICK_CLIP_ENTRY: shape = LM_CURSOR_POINTER; break;
    case LM_PICK_PANEL_CLOCK: shape = LM_CURSOR_POINTER; break;
    case LM_PICK_PANEL_POWER: power_btn = true; shape = LM_CURSOR_POINTER; break;
    case LM_PICK_POWER_BUTTON: power_item = pick->index; shape = LM_CURSOR_POINTER; break;
    case LM_PICK_MENU_ITEM: dm_item = pick->index; shape = LM_CURSOR_POINTER; break;
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
    if (wm->ov.open) {                                          /* the pointer moves the overview's highlight */
        int hi = pick->kind == LM_PICK_OVERVIEW_ITEM && !wm->ov.drag ? pick->index : -1, hws = pick->kind == LM_PICK_OVERVIEW_WS ? pick->index : -1;
        if (hi != wm->ov.hover || hws != wm->ov.hover_ws) {
            wm->ov.hover = hi;
            wm->ov.hover_ws = hws;
            if (hi >= 0)
                wm->ov.sel = hi;
            overview_damage(wm);
        }
    }
    if (wm->launcher_open && !wm->bubble.active) {              /* the pointer moves the launcher's highlight */
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
    if (wm->menu_open && dm_item != wm->menu_sel && (dm_item >= 0 || pick->kind == LM_PICK_MENU || pick->kind == LM_PICK_MENU_BACKDROP)) {
        wm->menu_sel = dm_item;                              /* the pointer moves the menu's highlight */
        menu_damage(wm);
    }
    if (wm->power_open && power_item >= 0 && power_item != wm->power_sel) {     /* the pointer moves the dialog's highlight */
        wm->power_sel = power_item;
        power_damage(wm);
    }
    if (pws != wm->hover_panel_ws || launcher != wm->hover_launcher || power_btn != wm->hover_power ||
        task != wm->hover_task || layout_btn != wm->hover_layout ||
        sticky_btn != wm->hover_sticky || osk_btn != wm->hover_osk || tray_id != wm->hover_tray) {
        wm->hover_tray = tray_id;
        wm->hover_osk = osk_btn;
        wm->hover_layout = layout_btn;
        wm->hover_sticky = sticky_btn;
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
    if (wm->wb.active)
        wm->wb.released = true;                                  /* the mesh settles and goes */
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
        if (!wm->wb.active && wobble_wanted(wm, w))
            wobble_start(wm, w, wm->grab_px, wm->grab_py);               /* (the picture is taken before the window moves) */
        struct lm_rect before = frame_outer(w);
        int32_t tx = wm->grab_x + (int32_t)dx, ty = wm->grab_y + (int32_t)dy;
        struct lm_rect ua;
        lm_wm_usable_area(wm, (int32_t)x, (int32_t)y, &ua);
        if (ty < ua.y + LM_TITLEBAR_HEIGHT && !(w->states & LM_WINDOW_FULLSCREEN))
            ty = ua.y + LM_TITLEBAR_HEIGHT;
        if (!wm->wb.active && anim_glide_to(wm, w, tx, ty))
            return;
        w->x = tx;
        w->y = ty;
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
    if (wm->wb.active)
        wm->wb.released = true;
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
    wobble_stop(wm);
    wm->glide_win = NULL;                                       /* (a window still gliding from the last drag stays where it is) */
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
    if (settings_present(wm))
        return;
    wm->settings_spawn_ms = (int64_t)lm_now_ms();
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

/* ---- Suspend / Restart / Shutdown ------------------------------------------------------------------------------------------ */

struct power_run {
    struct lm_wm *wm;
    int action;
};

static const char *power_word(int action)
{
    return action == LM_LOCK_ACT_SUSPEND ? "Suspend" : action == LM_LOCK_ACT_REBOOT ? "Restart" : "Shutdown";
}

static void power_exit(void *data, int status, const char *err)
{
    struct power_run *pr = data;
    if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0)) {                 /* it did not work (no permission, an inhibitor ...): say so */
        char msg[256];
        while (*err == ' ')
            err++;
        if (*err)
            snprintf(msg, sizeof(msg), "%s failed: %s", power_word(pr->action), err);
        else
            snprintf(msg, sizeof(msg), "%s failed (exit status %d)", power_word(pr->action), WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        lm_wm_notify(pr->wm, "Session", msg, 10000);
    }
    free(pr);
}

void lm_wm_power_run(struct lm_wm *wm, int action)
{
    if (!wm || action < LM_LOCK_ACT_SUSPEND || action > LM_LOCK_ACT_POWEROFF)
        return;
    const struct lm_config *c = &wm->srv->config;
    const char *cmd = action == LM_LOCK_ACT_SUSPEND ? c->suspend_cmd : action == LM_LOCK_ACT_REBOOT ? c->reboot_cmd : c->poweroff_cmd;
    if (!cmd || !*cmd) {
        lm_wm_notify(wm, "Session", "There is no command for that: set it in [commands] of mellagui.conf", 6000);
        return;
    }
    lm_info("session: %s", power_word(action));
    struct power_run *pr = malloc(sizeof(*pr));
    if (!pr)
        return;
    pr->wm = wm;
    pr->action = action;
    if (lm_server_spawn_watch(wm->srv, cmd, power_exit, pr) <= 0) {
        free(pr);
        lm_wm_notify(wm, "Session", "Could not start the command", 6000);
    }
}

/* The answer of the confirmation the dialog asks for a power action. */
static void power_answer(struct lm_wm *wm, bool yes)
{
    int act = wm->power_confirm;
    power_set(wm, false, NULL);
    if (yes)
        lm_wm_power_run(wm, act);
}

/* Lock (0), Exit (1), Suspend (2), Restart (3) or Shutdown (4) was chosen in the session dialog. */
static void power_activate(struct lm_wm *wm, int which)
{
    if (wm->power_confirm) {                                    /* the question is up: 0 = Yes, 1 = Cancel */
        power_answer(wm, which == 0);
        return;
    }
    if (which == 2 && power_suspend_off(wm))
        return;                                                 /* (no command: the button is off) */
    if (which >= 2 && which <= 4) {
        int act = which;                                        /* (the dialog's index is the action number) */
        if (wm->srv->lock && lm_lock_request_action(wm->srv->lock, act)) {      /* a secret is set: the lock screen asks for it */
            power_set(wm, false, NULL);
            return;
        }
        wm->power_confirm = act;                                /* no secret: the dialog asks once more; Cancel is highlighted */
        wm->power_others = other_sessions();
        wm->power_sel = 1;
        power_damage(wm);
        return;
    }
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
    if (pick->kind == LM_PICK_MENU_ITEM || pick->kind == LM_PICK_MENU || pick->kind == LM_PICK_MENU_BACKDROP) {
        int pk = wm->press_panel_kind, pi = wm->press_panel_ws;
        if (pressed) {
            if (pick->kind == LM_PICK_MENU_BACKDROP) {      /* a click outside the menu closes it, nothing below gets it */
                menu_set(wm, false);
            } else if (pick->kind == LM_PICK_MENU_ITEM && button == BTN_LEFT) {
                wm->press_panel_kind = LM_PICK_MENU_ITEM;
                wm->press_panel_ws = pick->index;
                wm->press_win = NULL;
                wm->press_bits = 0;
            }
        } else {
            wm->press_panel_kind = 0;
            if (pk == LM_PICK_MENU_ITEM && pick->kind == LM_PICK_MENU_ITEM && pick->index == pi && button == BTN_LEFT)
                menu_activate(wm, pi);                          /* released on the entry it was pressed on */
        }
        return true;
    }
    if (pick->kind >= LM_PICK_OVERVIEW_ITEM && pick->kind <= LM_PICK_OVERVIEW) {       /* the desktop overview: a click is done when the button is let go on what it was pressed on */
        if (pressed) {
            wm->press_panel_kind = pick->kind;
            wm->press_panel_ws = pick->index;
            wm->press_win = NULL;
            wm->press_bits = 0;
            if (button == BTN_RIGHT && pick->kind == LM_PICK_OVERVIEW_ITEM && pick->index < wm->ov.n)
                window_menu_open(wm, wm->ov.item[pick->index].win);          /* Maximize, Minimize, Move to workspace ..., Close */
            if (wm->srv->seat) {
                double px, py;
                lm_seat_pointer_position(wm->srv->seat, &px, &py);
                wm->drag_press_x = (int)px;
                wm->drag_press_y = (int)py;
            }
        } else {
            int pk = wm->press_panel_kind, pi = wm->press_panel_ws;
            wm->press_panel_kind = 0;
            if (wm->ov.drag)
                overview_drag_finish(wm, pick);                              /* carried to a workspace cell (or dropped nowhere) */
            else if (pk == (int)pick->kind && pi == pick->index)
                overview_click(wm, pick, button);
        }
        return true;
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
    if (pressed && button == BTN_RIGHT && pick->kind == LM_PICK_LAUNCHER_FIELD) {      /* a right click on a field of the form: its menu */
        launcher_field_menu(wm, pick->index);
        return true;
    }
    if (pick->kind >= LM_PICK_LAUNCHER_ITEM && pick->kind <= LM_PICK_LAUNCHER_BACKDROP) {
        if (pressed && wm->bubble.active && !wm->bubble.closing) {      /* still rising: the first click only finishes it */
            bubble_finish(wm);
            return true;
        }
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
        else if (pk == LM_PICK_DOCK_ITEM && pick->kind == LM_PICK_DOCK_ITEM && pick->index == pi) {
            lm_sound_note(LM_SOUND_DOCK);                       /* our own click sound (not on the headless backend, where the tests only look at the record) */
            if (wm->srv->config.sound && (strcmp(wm->srv->config.backend, "headless") != 0 || getenv("MELLAGUI_SOUND_FORCE")))
                lm_sound_play(LM_SOUND_DOCK);                       /* (MELLAGUI_SOUND_FORCE: the tests play it on the headless backend, too) */
            dock_activate(wm, pi, button != BTN_MIDDLE);        /* the middle button always starts another instance */
        }
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
        if (button == BTN_RIGHT) {
            window_menu_open(wm, pick->window);
            return true;
        }
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
        if (button == BTN_RIGHT) {
            dockitem_menu_open(wm, pick->index);
            return true;
        }
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
        } else if (button == BTN_RIGHT && pick->window) {
            window_menu_open(wm, pick->window);
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
    overview_changed(wm);
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
        overview_finish(wm);
        menu_set(wm, false);
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
    case LM_ACT_OVERVIEW: overview_set(wm, !wm->ov.open || wm->ov.closing); break;
    case LM_ACT_NIGHT_LIGHT: lm_night_toggle(wm->srv->night); break;
    case LM_ACT_DND:
        if (wm->srv->settings)
            lm_settings_set(wm->srv->settings, "dnd", wm->srv->config.dnd ? "0" : "1");
        break;
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

    if (wm->menu_open && !(ctrl && alt)) {                  /* a menu takes every key: Esc closes, Up / Down choose, Enter does it */
        if (pressed && !repeat) {
            struct lm_menu_info mi;
            menu_info(wm, &mi);
            if (code == KEY_ESC) {
                menu_set(wm, false);
            } else if (code == KEY_UP || code == KEY_DOWN) {
                int dir = code == KEY_UP ? -1 : 1, k = wm->menu_sel;
                for (int step = 0; step < mi.n; step++) {
                    k = k < 0 ? (dir > 0 ? 0 : mi.n - 1) : (k + dir + mi.n) % mi.n;
                    if (mi.enabled[k]) {
                        wm->menu_sel = k;
                        menu_damage(wm);
                        break;
                    }
                }
            } else if ((code == KEY_ENTER || code == KEY_KPENTER) && wm->menu_sel >= 0 && mi.enabled[wm->menu_sel]) {
                menu_activate(wm, wm->menu_sel);
            }
        }
        return true;
    }
    if (wm->ov.open && !(ctrl && alt)) {                        /* the desktop overview is modal: Esc, the arrows, Enter, Delete, 1..4 */
        if (pressed && !is_modifier_key(code))
            overview_key(wm, code, repeat, mods);
        return true;
    }
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
            if (wm->power_confirm) {                            /* "Are you sure?": Left / Right / Tab choose, Enter answers, Y / N, Esc closes */
                if (code == KEY_ESC || code == KEY_N) {
                    power_set(wm, false, NULL);
                } else if (code == KEY_Y) {
                    power_answer(wm, true);
                } else if (code == KEY_LEFT || code == KEY_RIGHT || code == KEY_TAB) {
                    wm->power_sel = wm->power_sel == 0 ? 1 : 0;
                    power_damage(wm);
                } else if ((code == KEY_ENTER || code == KEY_KPENTER) && wm->power_sel >= 0) {
                    power_activate(wm, wm->power_sel);
                }
                return true;
            }
            if (code == KEY_ESC)
                power_set(wm, false, NULL);
            else if (code == KEY_LEFT || code == KEY_RIGHT || code == KEY_TAB) {
                int dir = code == KEY_LEFT || (code == KEY_TAB && (mods & LM_MOD_SHIFT)) ? LM_POWER_N - 1 : 1;
                int k = wm->power_sel;
                for (int step = 0; step < LM_POWER_N; step++) {         /* (a button that is off is skipped) */
                    k = k < 0 ? (dir == 1 ? 0 : LM_POWER_N - 1) : (k + dir) % LM_POWER_N;
                    if (!(k == 2 && power_suspend_off(wm)))
                        break;
                }
                wm->power_sel = k;
                power_damage(wm);
            } else if ((code == KEY_ENTER || code == KEY_KPENTER) && wm->power_sel >= 0)
                power_activate(wm, wm->power_sel);
            else if (code == KEY_L)
                power_activate(wm, 0);
            else if (code == KEY_E)
                power_activate(wm, 1);
            else if (code == KEY_S)
                power_activate(wm, 2);
            else if (code == KEY_R)
                power_activate(wm, 3);
            else if (code == KEY_P)
                power_activate(wm, 4);
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
