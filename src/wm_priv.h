#ifndef MELLAGUI_WM_PRIV_H
#define MELLAGUI_WM_PRIV_H

/*
 * The private part of the window manager: the state of the compositor shell (struct lm_wm) and what the files of the wm share.
 * wm.c is the core (windows, focus, picking, pointer, keys); the other wm_*.c files are the parts that were cut out of it, one per feature:
 *   wm_notify.c (notification cards), wm_switcher.c (Alt+Tab), wm_gadgets.c (the gadgets: places and sizes), wm_osk.c (on-screen keyboard),
 *   wm_dialogs.c (session / power dialog, About), wm_launcher.c (the Launchpad), wm_dock.c (dock, drags), wm_menu.c (menus), wm_calendar.c
 *   (calendar and quick settings popups), wm_anim.c (window animations, wobble), wm_bubble.c (the launcher bubble), wm_overview.c (Exposé).
 * The functions below were static in the one big file; they are shared by name now. wm.h is the contract with the rest of the compositor.
 */

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
#include "search.h"
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
#define PANEL_QUICK_W 36                            /* the quick settings button (not a configurable size) */
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
#define SMART_W 290
#define SMART_H 180
#define SMART_MIN_W 220
#define SMART_MIN_H 80
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
    bool gad_placed;            /* a gadget window the user moved (or that has a remembered place): it is not docked automatically */
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

enum grab_kind { GRAB_NONE, GRAB_MOVE, GRAB_RESIZE };

/*
 * Window animations (metrics `animation-ms`, 0 = off; headless only with MELLAGUI_ANIMATIONS): a window that opens, closes,
 * is minimized or comes back is shown as a picture (struct lm_ghost, made by lm_render_snapshot) that grows / shrinks and
 * fades or flies to / from its taskbar button or dock icon, painted as LM_SCENE_GHOST above the windows. The real window
 * is left out of the scene while its opening / restoring picture runs (`anim_hidden`); a closed one is gone already.
 */
enum anim_kind { ANIM_OPEN, ANIM_CLOSE, ANIM_MINIMIZE, ANIM_RESTORE, ANIM_SLIDE_OUT, ANIM_SLIDE_IN };
#define ANIM_MAX 16
#define ANIM_TICK_MS 16
struct wm_anim {
    bool used;
    enum anim_kind kind;
    struct lm_window *win;              /* OPEN / RESTORE: the window to show again at the end */
    struct lm_ghost ghost;
    struct lm_rect home;                /* where the picture is at full size (global) */
    struct lm_rect away;                /* MINIMIZE / RESTORE: the taskbar button or dock icon it flies to / from */
    int shift;                          /* SLIDE_OUT: how far it has moved at the end; SLIDE_IN: how far it is when it starts (px, signed) */
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
    pid_t quick_pid;                    /* the quick settings program we started (0 = none) */
    bool quick_pending;                 /* a click started it: show it when its window appears */
    uint64_t quick_pending_ms;
    struct lm_rect quick_anchor;        /* the button that was clicked (the card opens under it) */
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
    bool hover_power, hover_layout, hover_sticky, hover_osk, hover_quick;
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
    struct {                    /* the desktop gadgets, indexed by enum lm_gadget_id */
        bool visible;
        bool placed;            /* false: docked top right (default); true: x/y chosen by dragging */
        int32_t x, y, w, h;
        uint32_t click_time;
    } gad[LM_GADGET_TOTAL];
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
    struct {                    /* the cards glued below a card that is being moved: they come along (wm_gadgets.c) */
        int n;
        struct lm_window *win[LM_GADGET_TOTAL];
        int32_t x[LM_GADGET_TOTAL], y[LM_GADGET_TOTAL];       /* where they were when the grab began */
        int32_t minx, miny, maxx, maxy;                       /* the box of the card and everything glued to it */
    } glue;
    struct {                    /* the glued cards that were zoomed together: their stack is put right again as each one gets its new size (wm_gadgets.c) */
        int n;
        uint64_t until;                                       /* lm_now_ms() after which it is forgotten */
        struct lm_window *win[LM_GADGET_TOTAL];               /* ordered by their top edge, so a card comes after the one it hangs on */
        int32_t x[LM_GADGET_TOTAL], y[LM_GADGET_TOTAL], w[LM_GADGET_TOTAL], h[LM_GADGET_TOTAL];       /* as they were before */
        int parent[LM_GADGET_TOTAL];                          /* the member it hangs on, -1 = the top of a stack: it stays where it is */
        int seq;                                              /* the number of the last order, shown in the setting `gadget-zoom` */
        char text[512];                                       /* "SEQ:name=pct,name=pct" */
    } zoomglue;
};

#define BUBBLE_MS 2000              /* the whole rise / return: two seconds (with window animations on; off = at once) */
#define BUBBLE_R0 24.0f             /* radius of the bubble at the button */
#define SW_ITEM_H 24


struct dock_geo {
    int n, size, zsize, gap, x0, icon_bottom;
    struct lm_rect strip, tall, bounds;
};

static inline struct wm_win *WIN(struct lm_window *w) { return (struct wm_win *)w; }



/* ---- shared by the files of the wm ---------------------------------------------------------------------------- */

void apply_state(struct lm_wm *wm, struct lm_window *w);
uint32_t bit_for_kind(enum lm_pick_kind k);
void clamp_size(const struct lm_window *w, int32_t *cw, int32_t *ch);
void damage_all(struct lm_wm *wm);
void damage_frame(struct lm_wm *wm, const struct lm_window *w);
void damage_rect(struct lm_wm *wm, const struct lm_rect *r);
void frame_info(const struct lm_wm *wm, const struct lm_window *w, struct lm_frame_info *f);
struct lm_rect frame_outer(const struct lm_window *w);
bool fullscreen_covers(const struct lm_wm *wm, const struct lm_output *o);
void grab_begin(struct lm_wm *wm, struct lm_window *w, enum grab_kind kind, uint32_t edges);
void launch_settings(struct lm_wm *wm);
void move_to_workspace(struct lm_wm *wm, struct lm_window *w, int ws);
void open_shot(struct lm_wm *wm);
struct lm_rect output_panel_rect(const struct lm_output *o);
void panel_info(const struct lm_wm *wm, struct lm_output *o, struct lm_panel_info *p);
int layout_badge_count(const struct lm_wm *wm);       /* badges in the panel (setting layout-badges), 0 = one badge that cycles */
struct lm_rect popup_bounds(const struct lm_popup *p);
void popup_origin(const struct lm_popup *p, int32_t *ox, int32_t *oy);
bool popup_visible(const struct lm_wm *wm, const struct lm_popup *p, int depth);
void raise_window(struct lm_wm *wm, struct lm_window *w);
void refresh_pointer(struct lm_wm *wm);
void request_quit(struct lm_wm *wm);
void send_configure(struct lm_wm *wm, struct lm_window *w, int32_t cw, int32_t ch);
void set_focus(struct lm_wm *wm, struct lm_window *w);
void update_outputs(struct lm_wm *wm, struct lm_window *w);
bool win_is_calendar(const struct lm_window *w);
/* A desktop gadget is a window of the program mellagui-gadget (app id "mellagui.gadget.NAME"): no frame, always below the other windows, on every
 * workspace, never focused by a click and listed nowhere (wm_gadgets.c). `win_is_aux` = the windows the lists (taskbar, Alt+Tab, overview, focus
 * cycling, animations) leave out: the calendar, quick settings and the gadgets. */
bool win_is_gadget(const struct lm_window *w);
bool win_is_aux(const struct lm_window *w);
void gadget_map(struct lm_wm *wm, struct lm_window *w);
void gadgets_redock(struct lm_wm *wm);
void gadget_grab_motion(struct lm_wm *wm, struct lm_window *win, double dx, double dy);
void gadget_glue_begin(struct lm_wm *wm, struct lm_window *win);     /* a move of a card begins: finds what is glued to it */
void gadget_zoomglue_fit(struct lm_wm *wm, struct lm_window *w);     /* a card of a zoomed stack got its new size: the stack is put together again */
void gadget_glue_end(struct lm_wm *wm);                               /* ... and the move ended: they stay where they are (remembered) */
struct lm_window *gadget_window(struct lm_wm *wm, int id);        /* the gadget's window, or NULL */
const char *gadget_name(int id);
void gadget_procs_stop(struct lm_wm *wm);
bool win_is_quick(const struct lm_window *w);
void win_origin(const struct lm_window *w, int32_t *ox, int32_t *oy);
bool win_visible(const struct lm_wm *wm, const struct lm_window *w);
struct lm_output *window_output(struct lm_wm *wm, const struct lm_window *w);
int anim_duration(const struct lm_wm *wm);
void anim_end(struct lm_wm *wm, struct wm_anim *a);
void anim_finish_all(struct lm_wm *wm);
void anim_forget(struct lm_wm *wm, struct lm_window *w);
struct lm_rect anim_lerp_rect(const struct lm_rect *a, const struct lm_rect *b, float e);
bool anim_prepare(struct lm_wm *wm, struct lm_window *w, struct lm_ghost *g, struct lm_rect *home);
struct wm_anim *anim_start(struct lm_wm *wm, enum anim_kind kind, struct lm_window *w, struct lm_ghost *g, const struct lm_rect *home);
void anim_state(const struct wm_anim *a, float t, struct lm_rect *r, int *alpha);
bool anim_wanted(struct lm_wm *wm, const struct lm_window *w);
float ease_smooth(float t);
void wobble_start(struct lm_wm *wm, struct lm_window *w, double px, double py);
void wobble_stop(struct lm_wm *wm);
void wobble_tick(struct lm_wm *wm, uint64_t now);
bool wobble_wanted(const struct lm_wm *wm, const struct lm_window *w);
bool anim_glide_to(struct lm_wm *wm, struct lm_window *w, int32_t x, int32_t y);
void anim_tick(void *data);
void bubble_begin(struct lm_wm *wm, bool opening);
void bubble_end(struct lm_wm *wm);
void bubble_finish(struct lm_wm *wm);
struct lm_output *bubble_output(const struct lm_wm *wm);
bool bubble_tick(struct lm_wm *wm, uint64_t now);
float smooth01(float x);
struct lm_window *calendar_window(struct lm_wm *wm);
struct lm_window *quick_window(struct lm_wm *wm);
void about_damage(struct lm_wm *wm);
void about_info(const struct lm_wm *wm, struct lm_output *o, struct lm_about_info *ai);
struct lm_output *about_output(const struct lm_wm *wm);
void about_set(struct lm_wm *wm, bool open, struct lm_output *o);
void power_activate(struct lm_wm *wm, int which);
void power_answer(struct lm_wm *wm, bool yes);
void power_damage(struct lm_wm *wm);
void power_info(const struct lm_wm *wm, struct lm_output *o, struct lm_power_info *pi);
struct lm_output *power_output(const struct lm_wm *wm);
void power_set(struct lm_wm *wm, bool open, struct lm_output *o);
bool power_suspend_off(const struct lm_wm *wm);
void dock_activate(struct lm_wm *wm, int i, bool reuse);
void dock_anim_tick(void *data);
bool dock_app_matches(const char *match, const char *app_id);
void dock_damage(struct lm_wm *wm);
void dock_drag_finish(struct lm_wm *wm);
void dock_drag_motion(struct lm_wm *wm);
void dock_geometry(const struct lm_output *o, struct dock_geo *g);
void dock_hide_tick(void *data);
void dock_info(const struct lm_wm *wm, struct lm_output *o, struct lm_dock_info *d);
void dock_launch(struct lm_wm *wm, const struct lm_dock_item *it, int i, bool reuse);
void dock_motion(struct lm_wm *wm, bool in, int item);
struct lm_output *dock_output(const struct lm_wm *wm);
bool dock_pick(const struct lm_wm *wm, int ix, int iy, struct lm_pick *out);
bool dock_shown(const struct lm_wm *wm);
int dock_window_count(const struct lm_wm *wm, const struct lm_dock_item *it);
bool drag_far_enough(const struct lm_wm *wm, double x, double y);
void launcher_drag_finish(struct lm_wm *wm);
void launcher_drag_motion(struct lm_wm *wm);
bool launcher_drop_zone(const struct lm_wm *wm, struct lm_output *o, struct lm_rect *zone, struct lm_rect *marker, int *pos);
bool settings_present(struct lm_wm *wm);
void damage_panels(struct lm_wm *wm);
/* wm_places.c: where the own programs' windows were the last time (--windows-file). */
void places_load(struct lm_wm *wm);
bool places_find(struct lm_wm *wm, const struct lm_window *w, struct lm_rect *r, bool *maximized);
void places_remember(struct lm_wm *wm, struct lm_window *w);
void places_forget(struct lm_wm *wm, const struct lm_window *w);
void gadgets_load(struct lm_wm *wm);
void gadgets_save(struct lm_wm *wm);
bool dock_special(const struct lm_dock_item *it);
void launcher_activate(struct lm_wm *wm, int kind, int index);
int launcher_count(void);
void launcher_damage(struct lm_wm *wm);
void launcher_field_clip(struct lm_wm *wm, int what);
char *launcher_field_text(struct lm_wm *wm, size_t *max);
void launcher_info(const struct lm_wm *wm, struct lm_output *o, struct lm_launcher_info *li);
bool launcher_is_self(const struct lm_dock_item *it);
const struct lm_dock_item *launcher_item(int g, int *dock_index);
void launcher_key(struct lm_wm *wm, uint32_t code, bool repeat, uint32_t mods);
void launcher_open_change(struct lm_wm *wm, int g);
struct lm_output *launcher_output(const struct lm_wm *wm);
bool launcher_pick(const struct lm_wm *wm, int ix, int iy, struct lm_pick *out);
void launcher_set(struct lm_wm *wm, bool open, struct lm_output *o);
void dockitem_menu_open(struct lm_wm *wm, int i);
void launcher_field_menu(struct lm_wm *wm, int field);
void menu_activate(struct lm_wm *wm, int which);
void menu_close(struct lm_wm *wm, bool chosen);
void menu_damage(struct lm_wm *wm);
void menu_info(const struct lm_wm *wm, struct lm_menu_info *mi);
void menu_set(struct lm_wm *wm, bool open);
void window_menu_activate(struct lm_wm *wm, int which);
void window_menu_open(struct lm_wm *wm, struct lm_window *w);
void note_dismiss(struct lm_wm *wm, int id);
bool note_rects(const struct lm_wm *wm, struct lm_rect *r);
void note_tick(void *data);
int note_wrap(const char *body, char line[LM_NOTIFY_LINES][LM_NOTIFY_COLS * 4 + 1]);
void keymap_sync(const struct lm_wm *wm);
void osk_damage(struct lm_wm *wm);
void osk_drag_begin(struct lm_wm *wm, int kind, uint32_t edges);
void osk_drag_motion(struct lm_wm *wm);
bool osk_geo(const struct lm_wm *wm, struct lm_osk_geo *g);
void osk_info(const struct lm_wm *wm, const struct lm_osk_geo *g, struct lm_osk_info *oi);
bool osk_pick(const struct lm_wm *wm, int ix, int iy, struct lm_pick *out);
void osk_press(struct lm_wm *wm, int i);
void osk_release(struct lm_wm *wm);
void osk_set(struct lm_wm *wm, bool open);
void anim_fly(struct lm_wm *wm, enum anim_kind kind, struct lm_window *w);
/* A workspace switch: pictures of the windows that leave (call before the switch) and of the ones that arrive (after it) slide sideways. dir = +1: to a higher workspace. */
void anim_workspace(struct lm_wm *wm, int dir, bool leaving);
void anim_open(struct lm_wm *wm, struct lm_window *w);
void anim_play_close(struct lm_wm *wm, struct lm_window *w);
void anim_remember_close(struct lm_wm *wm, struct lm_window *w);
bool ov_hides(const struct lm_wm *wm, const struct lm_window *w);
void overview_changed(struct lm_wm *wm);
void overview_click(struct lm_wm *wm, const struct lm_pick *pick, uint32_t button);
void overview_damage(struct lm_wm *wm);
void overview_drag_finish(struct lm_wm *wm, const struct lm_pick *pick);
void overview_drag_motion(struct lm_wm *wm);
void overview_finish(struct lm_wm *wm);
void overview_key(struct lm_wm *wm, uint32_t code, bool repeat, uint32_t mods);
struct lm_output *overview_output(const struct lm_wm *wm);
bool overview_pick(const struct lm_wm *wm, int ix, int iy, struct lm_pick *out);
void overview_set(struct lm_wm *wm, bool open);
bool overview_tick(struct lm_wm *wm, uint64_t now);
struct lm_output *gadget_output(const struct lm_wm *wm);
void sw_close(struct lm_wm *wm);
void sw_commit(struct lm_wm *wm);
int sw_first(const struct lm_wm *wm);
const char *sw_label(const struct lm_window *w);
struct lm_rect sw_rect(const struct lm_wm *wm);
void sw_step(struct lm_wm *wm, bool backwards);

#endif
