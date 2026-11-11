#ifndef MELLAGUI_WM_H
#define MELLAGUI_WM_H

/*
 * Window manager + shell: floating windows with server-side decorations,
 * stacking, focus (click-to-focus), interactive move/resize, maximize,
 * fullscreen, minimize, 4 workspaces, keyboard shortcuts, top panel
 * (launcher, workspace indicators, taskbar, clock) and wallpaper.
 * Owner: wm.c.
 *
 * ---- Responsibilities split ---------------------------------------------------
 *  wm.c    all layout/geometry decisions, hit testing, focus, stacking order,
 *          hover/pressed state of decorations and panel, the clock timer.
 *          It computes every rectangle (frame, buttons, panel cells) itself.
 *  render  only paints what lm_wm_scene_for_each() reports (it never
 *          recomputes layout) and draws the cursor.
 *  xdg.c   protocol adapter; talks to the wm through struct lm_window /
 *          struct lm_popup and struct lm_window_ops.
 *  seat.c  pointer/keyboard routing; asks the wm: lm_wm_key, lm_wm_pick,
 *          lm_wm_pointer_motion, lm_wm_pointer_button. The wm calls back into
 *          seat for focus (lm_seat_set_keyboard_focus), pointer grabs
 *          (interactive move/resize) and cursor shape overrides.
 *
 * ---- Window geometry convention -----------------------------------------------
 * For a window, (x, y, width, height) is the *window geometry* in global
 * compositor coordinates: the client content area, as set by
 * xdg_surface.set_window_geometry (or the surface bounds if never set). The
 * decorated frame surrounds it:
 *     outer = { x - LM_BORDER_WIDTH, y - LM_TITLEBAR_HEIGHT,
 *               width + 2*LM_BORDER_WIDTH, height + LM_TITLEBAR_HEIGHT + LM_BORDER_WIDTH }
 *     titlebar = { outer.x, outer.y, outer.w, LM_TITLEBAR_HEIGHT }
 * Fullscreen windows have no frame and cover their output (panel hidden
 * while a fullscreen window is focused on that output). The wl_surface origin
 * is (x - geometry.x, y - geometry.y).
 * Maximized windows fill the output's usable area (output minus panel) with
 * the frame inside it.
 *
 * ---- Configure flow (wm <-> xdg) ----------------------------------------------
 * The wm decides sizes/states and calls window->ops->configure(); xdg.c turns
 * it into xdg_toplevel.configure + xdg_surface.configure(serial), tracks
 * ack_configure and, once the client commits the acked state, calls
 * lm_wm_window_set_geometry(). Width/height passed to configure are
 * *window geometry* sizes; 0 means "client decides".
 *
 * ---- The dock ----------------------------------------------------------------------
 * Round icon buttons along the bottom of the primary output (dock.h): LM_SCENE_DOCK above the panel, LM_PICK_DOCK_ITEM / LM_PICK_DOCK
 * above the windows. Hovered icons grow, a dot marks the ones with a window, a click starts or focuses the program.
 *
 * ---- Shortcuts (lm_wm_key) -------------------------------------------------------
 * The table is src/keys.h (built-in defaults, changed by [keys] in mellagui.conf); these are the defaults:
 *   Super+Enter        spawn config.terminal_cmd
 *   Escape             closes the open About dialog (the round logo button top left opens it)
 *   Super+R            spawn config.run_cmd (run dialog; also the panel's "Run" button)
 *   Super+I            spawn config.settings_cmd (settings panel, also in the MellaGUI menu)
 *   Super+G            show / hide the clipboard gadget (history; drag its title bar to move, its edges to resize)
 *   Super+V            show / hide the volume mixer gadget (same dragging; sliders and buttons work inside)
 *   Super+S            show / hide the system monitor gadget (CPU, memory, temperature)
 *   Super+Q            close focused window (ops->close)
 *   Super+1..4         switch workspace;  Super+Shift+1..4 move focused window there
 *   Super+Tab          focus next window (cycle, raises it); Super+Shift+Tab previous
 *   Super+F / Super+M  toggle fullscreen / maximize on focused window
 *   Super+Left/Right   tile the focused window to the left/right half of the usable area
 *   Super+Up / Down    maximize / restore (un-maximize, or un-tile back to the pre-tile geometry)
 *   Super+L            lock the screen (lock.h; needs a password / PIN set in the settings, else nothing happens)
 *   Super+Shift+E      lm_server_quit
 *   Ctrl+Alt+F1..F12   lm_backend_switch_vt (works even with a grab)
 * Key codes are evdev (KEY_*), modifiers are LM_MOD_* bits.
 */

#include "about.h"
#include "dock.h"
#include "dockicon.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "list.h"
#include "osk.h"
#include "metrics.h"
#include "lock.h"
#include "server.h"
#include "util.h"

struct lm_surface;
struct lm_output;
struct lm_window;
struct lm_popup;

#define LM_NUM_WORKSPACES   4
#define LM_BORDER_WIDTH     (lm_metrics.border_width)
#define LM_TITLEBAR_HEIGHT  (lm_metrics.titlebar_height)
#define LM_PANEL_HEIGHT     (lm_metrics.panel_height)
#define LM_FRAME_BUTTON_SIZE (lm_metrics.button_size)   /* close/max/min square buttons inside the titlebar */
#define LM_FRAME_BUTTON_GAP  (lm_metrics.button_gap)
#define LM_PANEL_MAX_TASKS   16
#define LM_PANEL_MAX_TRAY    16     /* = LM_TRAY_MAX_ITEMS (tray.h) */
#define LM_MIN_WINDOW_WIDTH  (lm_metrics.min_width)
#define LM_MIN_WINDOW_HEIGHT (lm_metrics.min_height)

/* Window state bits (wm -> xdg configure). */
enum {
    LM_WINDOW_MAXIMIZED  = 1u << 0,
    LM_WINDOW_FULLSCREEN = 1u << 1,
    LM_WINDOW_RESIZING   = 1u << 2,
    LM_WINDOW_ACTIVATED  = 1u << 3,
};

/* Resize edges; same numeric values as xdg_toplevel.resize_edge (bitmask). */
enum {
    LM_EDGE_NONE = 0,
    LM_EDGE_TOP = 1,
    LM_EDGE_BOTTOM = 2,
    LM_EDGE_LEFT = 4,
    LM_EDGE_RIGHT = 8,
};

/* wm -> shell adapter (implemented by xdg.c). */
struct lm_window_ops {
    /*
     * Ask the client for a new geometry size (0 = client's choice) and
     * state set (LM_WINDOW_* bits). xdg.c sends configure events with a fresh
     * serial. May be called many times (e.g. continuously while resizing).
     */
    void (*configure)(struct lm_window *w, int32_t width, int32_t height,
                      uint32_t states);
    /* Politely ask the client to close (xdg_toplevel.close). */
    void (*close)(struct lm_window *w);
};

struct lm_window {
    struct lm_wm *wm;
    struct lm_list link;            /* in the wm stacking list (private use) */
    struct lm_surface *surface;     /* root wl_surface; valid until lm_wm_window_destroy */
    const struct lm_window_ops *ops;
    void *shell_data;               /* xdg.c's toplevel state */

    /* READ-ONLY outside wm.c */
    char *title;                    /* never NULL, UTF-8, may be empty */
    char *app_id;                   /* never NULL */
    struct lm_window *parent;       /* transient parent or NULL */
    int32_t min_w, min_h, max_w, max_h; /* 0 = unconstrained */
    struct lm_rect geometry;        /* committed window geometry inside the surface */
    int32_t x, y, width, height;    /* window geometry in global coordinates */
    uint32_t states;                /* LM_WINDOW_* currently applied */
    bool mapped;
    bool minimized;
    int workspace;                  /* 0..LM_NUM_WORKSPACES-1 */

    /* private to wm.c (animations) */
    bool anim_hidden;               /* a picture of it is animated in its place: the scene leaves the real window out meanwhile */
    struct lm_ghost *close_ghost;   /* its picture from the moment it was asked to close: shown shrinking when it then goes away */
    struct lm_rect close_rect;
    uint64_t close_ms;

    void *priv;
};

struct lm_popup {
    struct lm_wm *wm;
    struct lm_list link;
    struct lm_surface *surface;     /* popup root wl_surface */
    struct lm_surface *parent;      /* parent xdg_surface's wl_surface */
    int32_t x, y;                   /* global position of the popup's window geometry origin */
    struct lm_rect geometry;        /* window geometry inside the surface */
    bool mapped;
    void *priv;
};

/* ---- lifecycle -------------------------------------------------------------------- */

struct lm_wm *lm_wm_create(struct lm_server *srv);
void lm_wm_destroy(struct lm_wm *wm);
void lm_wm_output_added(struct lm_wm *wm, struct lm_output *out);
void lm_wm_output_changed(struct lm_wm *wm, struct lm_output *out);
/* Windows on a removed output are moved to the first remaining one. */
void lm_wm_output_removed(struct lm_wm *wm, struct lm_output *out);

/* ---- windows: calls from xdg.c ------------------------------------------------------ */

/* Creates an unmapped window for an xdg_toplevel. Never fails except OOM (NULL). */
struct lm_window *lm_wm_window_create(struct lm_wm *wm, struct lm_surface *surface,
                                      const struct lm_window_ops *ops, void *shell_data);
/* Unmaps if needed, drops focus, frees. May be called while mapped. */
void lm_wm_window_destroy(struct lm_window *w);
/*
 * The client's first (buffer-less) commit arrived: the wm answers with
 * ops->configure(w, width, height, states) for the window's initial state.
 */
void lm_wm_window_initial_configure(struct lm_window *w);
void lm_wm_window_set_title(struct lm_window *w, const char *title);
void lm_wm_window_set_app_id(struct lm_window *w, const char *app_id);
void lm_wm_window_set_parent(struct lm_window *w, struct lm_window *parent);
void lm_wm_window_set_min_size(struct lm_window *w, int32_t width, int32_t height);
void lm_wm_window_set_max_size(struct lm_window *w, int32_t width, int32_t height);
/*
 * Geometry committed by the client (after the ack): `g` is the window geometry
 * inside the surface. Called on every commit where it may have changed. The wm
 * re-anchors when resizing from the top/left edges and damages as needed.
 */
void lm_wm_window_set_geometry(struct lm_window *w, const struct lm_rect *g);
/* First buffer committed after configure ack: show the window (placed/cascaded and focused). */
void lm_wm_window_map(struct lm_window *w);
void lm_wm_window_unmap(struct lm_window *w);

/* Client requests (xdg_toplevel set_maximized & co). The wm may refuse silently. */
void lm_wm_window_request_maximize(struct lm_window *w, bool maximize);
void lm_wm_window_request_fullscreen(struct lm_window *w, bool fullscreen);
void lm_wm_window_request_minimize(struct lm_window *w);
/*
 * wm-initiated: tile to LM_EDGE_LEFT / LM_EDGE_RIGHT half of the usable area, or
 * LM_EDGE_NONE to undo a tile. Tiling leaves the maximized state, remembers the
 * pre-tile geometry, and is dropped by maximize/fullscreen, resize and move grabs.
 * Reported as "tiled-left"/"tiled-right" in lm_wm_describe flags.
 */
void lm_wm_window_request_tile(struct lm_window *w, uint32_t edge);
/* Start an interactive move/resize with the seat pointer (xdg already validated the serial). */
void lm_wm_window_request_move(struct lm_window *w);
void lm_wm_window_request_resize(struct lm_window *w, uint32_t edges);
/* wm-initiated close (title bar button, Super+Q): calls ops->close. */
void lm_wm_window_close(struct lm_window *w);

/* ---- popups: calls from xdg.c ----------------------------------------------------------- */

struct lm_popup *lm_wm_popup_create(struct lm_wm *wm, struct lm_surface *surface,
                                    struct lm_surface *parent);
void lm_wm_popup_destroy(struct lm_popup *p);
/* Global position of the popup's window-geometry origin. */
void lm_wm_popup_set_position(struct lm_popup *p, int32_t gx, int32_t gy);
void lm_wm_popup_set_geometry(struct lm_popup *p, const struct lm_rect *g);
void lm_wm_popup_map(struct lm_popup *p);
void lm_wm_popup_unmap(struct lm_popup *p);

/* ---- queries -------------------------------------------------------------------------------- */

/*
 * Global position of the origin of the (root) surface `s` as currently
 * displayed (window or popup). Returns false if it is not displayed.
 */
bool lm_wm_surface_origin(struct lm_wm *wm, struct lm_surface *s, int32_t *x, int32_t *y);
/* Output area minus the panel for the output containing/nearest (gx, gy). */
void lm_wm_usable_area(struct lm_wm *wm, int32_t gx, int32_t gy, struct lm_rect *out);
struct lm_window *lm_wm_focused_window(const struct lm_wm *wm);
void lm_wm_switch_workspace(struct lm_wm *wm, int ws);
void lm_wm_focus_window(struct lm_wm *wm, struct lm_window *w); /* raises, switches workspace if needed */

/*
 * Called through the xdg roles' damage hook: `rect` (root-local) of root
 * surface `root` changed; the wm maps it to global coordinates, clips to
 * visibility and calls lm_render_damage().
 */
void lm_wm_surface_damage(struct lm_wm *wm, struct lm_surface *root,
                          const struct lm_rect *rect);
/*
 * A frame was presented at `time_ms`: send frame done to every visible
 * (mapped, non-minimized, current workspace) window/popup surface tree and
 * update wl_surface.enter/leave output membership.
 */
void lm_wm_send_frame_done(struct lm_wm *wm, uint32_t time_ms);

/* ---- input hooks (called by seat.c) ------------------------------------------------------------ */

enum lm_pick_kind {
    LM_PICK_NONE = 0,       /* wallpaper / nothing */
    LM_PICK_SURFACE,        /* a client surface (window content, subsurface, popup) */
    LM_PICK_TITLEBAR,
    LM_PICK_CLOSE,
    LM_PICK_MAXIMIZE,
    LM_PICK_MINIMIZE,
    LM_PICK_BORDER,         /* resize border/corner: `edges` says which */
    LM_PICK_PANEL,          /* panel background / clock / title */
    LM_PICK_PANEL_LAUNCHER, /* the round logo button top left (`rect` = its square): opens the About dialog */
    LM_PICK_ABOUT_BUTTON,   /* the About dialog's Close button */
    LM_PICK_ABOUT_CARD,     /* the About dialog's card (everything but its button) */
    LM_PICK_ABOUT_BACKDROP, /* anything else while the About dialog is open: a click closes it, nothing below gets it */
    LM_PICK_LAUNCHER_ITEM,  /* a big icon of the launcher dialog (`index` = dock item) */
    LM_PICK_LAUNCHER_REMOVE,/* the red x of an icon while the launcher is in edit mode (`index` = dock item) */
    LM_PICK_LAUNCHER_ADD,   /* the "+" tile: opens the add form */
    LM_PICK_LAUNCHER_BUTTON,/* a button of the launcher dialog (`index` 0 = Edit / Add, 1 = Close / Cancel) */
    LM_PICK_LAUNCHER_FIELD, /* a text field of the add form (`index` 0 = name, 1 = command) */
    LM_PICK_LAUNCHER_KIND,  /* an icon choice of the add form (`index` = enum lm_dockicon, or -1 = "guess from the name") */
    LM_PICK_LAUNCHER_HELP,  /* the small "?" at the end of a form field (`index` 0 = name, 1 = command, 2 = icon): opens its help card */
    LM_PICK_LAUNCHER_HELPCLOSE, /* while a help card is open: its Close button, or anything outside the card */
    LM_PICK_LAUNCHER_CARD,  /* the launcher's card (between its items) */
    LM_PICK_LAUNCHER_BACKDROP, /* anything else while the launcher is open: a click closes it, nothing below gets it */
    LM_PICK_PANEL_RUN,      /* "Run" button (starts config.run_cmd) */
    LM_PICK_PANEL_CLOCK,    /* the clock: shows / hides the calendar (`rect` = the clock) */
    LM_PICK_PANEL_POWER,    /* the round power button in the top right corner: opens / closes the session dialog */
    LM_PICK_POWER_BUTTON,   /* a round button of the session dialog (`index` 0 = Lock, 1 = Exit) */
    LM_PICK_POWER_CARD,     /* the session dialog's card (between its buttons) */
    LM_PICK_POWER_BACKDROP, /* anything else while the session dialog is open: a click closes it, nothing below gets it */
    LM_PICK_MENU_ITEM,  /* an entry of the dock's right-click menu (`index` = entry) */
    LM_PICK_MENU,       /* the menu's card (between its entries) */
    LM_PICK_MENU_BACKDROP, /* anything else while the menu is open: a click closes it, nothing below gets it */
    LM_PICK_PANEL_OSK,      /* the panel's on-screen keyboard button: shows / hides the keyboard */
    LM_PICK_OSK_KEY,        /* a key of the on-screen keyboard (`index` = key) */
    LM_PICK_OSK,            /* the on-screen keyboard between its keys (its top strip, the left margin, the gaps): drag to move it */
    LM_PICK_OSK_GRIP,       /* its right / bottom margin (`edges` = LM_EDGE_RIGHT / LM_EDGE_BOTTOM, both in the corner): drag to resize it */
    LM_PICK_PANEL_LAYOUT,   /* keyboard layout indicator: left click = next layout, right click = previous */
    LM_PICK_PANEL_STICKY,   /* sticky keys toggle */
    LM_PICK_PANEL_WORKSPACE,/* `index` = workspace */
    LM_PICK_PANEL_TASK,     /* taskbar button of `window` */
    LM_PICK_PANEL_TRAY,     /* an icon / button a program put into the bar (tray.h): `index` = the item's id */
    LM_PICK_GADGET_TITLE,   /* desktop gadget title bar: drag to move, double click resets (`index` = gadget id) */
    LM_PICK_GADGET_BORDER,  /* desktop gadget edge/corner: drag to resize, `edges` says which (`index` = gadget id) */
    LM_PICK_GADGET_BODY,    /* inside the mixer gadget: sliders and buttons (`index` = gadget id, `rect` = the card) */
    LM_PICK_CLIP_ENTRY,     /* an entry of the clipboard gadget: click = use it as the current clipboard (`index` = entry) */
    LM_PICK_CLIP_DELETE,    /* the x at the right end of an entry: forget that entry (`index` = entry) */
    LM_PICK_CLIP_UP,        /* the clipboard card's up arrow: a page back towards the newest */
    LM_PICK_CLIP_DOWN,      /* ... its down arrow: a page towards the oldest */
    LM_PICK_CLIP_CLEAR,     /* ... its trash can: forget the whole history */
    LM_PICK_DOCK_ITEM,      /* a button of the dock (`index` = lm_dock_item index) */
    LM_PICK_DOCK,           /* the dock between its buttons, or (autohide) the bottom edge of the screen that shows it: swallows the click */
    LM_PICK_SHOT_CARD,      /* the screenshot card (above everything): click = open the file in the viewer */
    LM_PICK_NOTIFY,         /* a notification card: click = dismiss it (`index` = its id) */
    LM_PICK_OVERVIEW_ITEM,  /* a window's picture in the desktop overview (`index` = item): click = go to it, middle = close it, right = its menu */
    LM_PICK_OVERVIEW_WS,    /* a workspace cell at the top of the overview (`index` = workspace): click = show that workspace's windows */
    LM_PICK_OVERVIEW        /* anything else in the overview: a click closes it, nothing below gets it */
};

struct lm_pick {
    enum lm_pick_kind kind;
    struct lm_window *window;   /* owning window for SURFACE (NULL for popups of unknown owner) and frame parts */
    struct lm_surface *surface; /* LM_PICK_SURFACE: the exact (sub)surface hit */
    double sx, sy;              /* LM_PICK_SURFACE: surface-local hit position */
    uint32_t edges;             /* LM_PICK_BORDER: LM_EDGE_* */
    int index;                  /* LM_PICK_PANEL_WORKSPACE, LM_PICK_MENU_ITEM, LM_PICK_GADGET_* */
    struct lm_rect rect;        /* LM_PICK_PANEL_LAUNCHER, LM_PICK_GADGET_BODY */
};

/* Topmost thing at the global point: popups, then windows (stacking), then panel. */
void lm_wm_pick(struct lm_wm *wm, double x, double y, struct lm_pick *out);
/* Pointer moved (also called for every motion): hover highlight, resize cursors via lm_seat_set_cursor_override. */
void lm_wm_pointer_motion(struct lm_wm *wm, const struct lm_pick *pick);
/*
 * Button event. For LM_PICK_SURFACE presses the wm focuses/raises the window
 * (and dismisses popups on outside clicks) and returns false so the client
 * still receives the click. For decoration/panel hits it acts (close,
 * maximize, minimize, start move/resize, launch, workspace switch) and
 * returns true (consumed).
 */
bool lm_wm_pointer_button(struct lm_wm *wm, const struct lm_pick *pick,
                          uint32_t time_ms, uint32_t button, bool pressed);
/* The keyboard layout / sticky key state changed: repaint the indicators. */
void lm_wm_keyboard_state_changed(struct lm_wm *wm);
/* The clipboard history changed (seat.c): repaint the gadget. */
void lm_wm_clipboard_changed(struct lm_wm *wm);
/* The volume mixer's / the system monitor's state changed: repaint that gadget. */
void lm_wm_mixer_changed(struct lm_wm *wm);
void lm_wm_sysinfo_changed(struct lm_wm *wm);

/* Desktop gadgets: the clipboard viewer and the volume mixer. */
enum lm_gadget_id { LM_GADGET_CLIPBOARD, LM_GADGET_MIXER, LM_GADGET_SYSTEM, LM_GADGET_COUNT };
/* Visibility of gadget `id` (Super+G = clipboard, Super+V = mixer, Super+S = system monitor; control socket `gadget`, `mixer`, `sysinfo`). */
bool lm_wm_gadget_visible_id(const struct lm_wm *wm, int id);
void lm_wm_set_gadget_id(struct lm_wm *wm, int id, bool visible);
/* The gadget card (without shadow) in global coordinates; false when hidden or it does not fit. */
bool lm_wm_gadget_rect_id(const struct lm_wm *wm, int id, struct lm_rect *out);
/* The same for the clipboard gadget (the original single-gadget API). */
bool lm_wm_gadget_visible(const struct lm_wm *wm);
void lm_wm_set_gadget(struct lm_wm *wm, bool visible);
/*
 * The screen lock turned on / off (lock.c). Locking closes the app menu, the clipboard navigation and the popups of the
 * focused window; unlocking gives the keyboard focus back to the focused window. While locked the scene is only the lock
 * screen (see lm_wm_scene_for_each).
 */
void lm_wm_lock_changed(struct lm_wm *wm, bool locked);
/*
 * Notifications: a card with `title` and `body` (UTF-8, long text is wrapped and cut) at the top right of the first output, newest
 * on top, at most LM_NOTIFY_MAX. It goes away after `timeout_ms` (0 = it stays until it is clicked). Not shown while locked.
 */
/* The theme, sizes, shortcuts or menu were reloaded (mellagui.conf): close the menu, refit maximized windows to the new panel / frame
 * sizes and repaint everything. */
void lm_wm_config_changed(struct lm_wm *wm);
/*
 * Key capture (the settings panel's "Record" button): while on, the next key press with its modifiers is not handled by anything
 * (neither a shortcut nor a client) but remembered as text like "Super+Shift+Q" (lm_keys_combo_text); Escape alone cancels (empty text).
 * Capture then ends and the settings are announced. A key without a name in keys.h is ignored and capture goes on.
 */
void lm_wm_key_capture(struct lm_wm *wm, bool on);
bool lm_wm_key_capturing(const struct lm_wm *wm);
const char *lm_wm_key_captured(const struct lm_wm *wm);
void lm_wm_notify(struct lm_wm *wm, const char *title, const char *body, int timeout_ms);
/* A card now, even in "do not disturb", and not in the history (the answer to the user's own switch). */
void lm_wm_notify_now(struct lm_wm *wm, const char *title, const char *body, int timeout_ms);
/*
 * The outputs are about to be placed again (settings output-order): packs them (output.c) and moves every window along with the output
 * it is on, so that it keeps its place on that monitor; maximized / fullscreen windows follow their output's new geometry.
 */
void lm_wm_relayout_outputs(struct lm_wm *wm);
/* Keyboard shortcut filter; true = consumed (see "Shortcuts"). */
bool lm_wm_key(struct lm_wm *wm, uint32_t code, bool pressed, bool repeat, uint32_t mods);

/* ---- scene description for the renderer ----------------------------------------------------------- */

#define LM_FRAME_HOVER_CLOSE    (1u << 0)
#define LM_FRAME_HOVER_MAXIMIZE (1u << 1)
#define LM_FRAME_HOVER_MINIMIZE (1u << 2)

struct lm_frame_info {
    struct lm_rect outer;           /* whole decorated frame (global coords) */
    struct lm_rect titlebar;
    struct lm_rect btn_close, btn_max, btn_min;
    struct lm_rect title_text;      /* area where the title may be drawn (between the left edge and the buttons) */
    const char *title;              /* valid only during the callback */
    bool active;                    /* focused window */
    bool maximized;
    uint32_t hover;                 /* LM_FRAME_HOVER_* */
    uint32_t pressed;               /* LM_FRAME_HOVER_* bit currently held down, else 0 */
};

#define LM_PANEL_SHADOW 14          /* how far the panel's shadow reaches below it */

struct lm_texture;

struct lm_panel_info {
    struct lm_output *output;
    struct lm_rect bar;             /* whole panel */
    struct lm_rect launcher;        /* launcher button */
    struct lm_rect workspace[LM_NUM_WORKSPACES];
    int current_workspace;
    uint32_t workspace_occupied;    /* bit n set = workspace n has windows */
    int hover_workspace;            /* -1 = none */
    bool hover_launcher;
    bool about_open;                /* the About dialog is open: the logo button shows it */
    struct lm_rect caps;            /* Caps / Num Lock indicator; zero width while no lock is on */
    char caps_text[8];              /* "CAPS", "NUM" or "CAPS+N" */
    struct lm_rect osk;             /* the on-screen keyboard button, left of the layout indicator */
    bool hover_osk;
    bool osk_open;                  /* the keyboard is showing: the button is lit */
    struct lm_rect layout;          /* keyboard layout indicator */
    bool hover_layout;
    char layout_text[8];            /* "US", "EE" ... */
    struct lm_rect sticky;          /* sticky keys toggle */
    bool hover_sticky;
    bool sticky_on;
    char sticky_text[24];           /* "Sticky", or the latched (Sh) / locked (SH) modifiers */
    struct lm_rect run;             /* run-dialog button, left of the quit button */
    bool hover_run;
    struct lm_rect power;           /* the round power button, the rightmost one (right of the clock); a circle in this square */
    bool hover_power;
    bool power_open;                /* the session dialog is open: the button shows it */
    struct lm_rect title_area;      /* where the taskbar (or the focused title if it is empty) goes */
    const char *active_title;       /* "" if none; valid only during the callback */
    int ntasks;                     /* taskbar: mapped windows of the current workspace, in creation order */
    struct {
        struct lm_rect rect;
        const char *title;          /* valid only during the callback */
        bool active, minimized, hover;
    } task[LM_PANEL_MAX_TASKS];
    struct lm_window *task_win[LM_PANEL_MAX_TASKS]; /* the window behind task[i] (for picking) */
    struct lm_rect clock;
    char clock_text[40];            /* local date and time as the clock settings say (default "DD.MM.YYYY HH:MM") */
    int ntray;                      /* programs' icons (tray.h), the first one created rightmost, left of the Caps / keyboard indicators */
    struct {
        struct lm_rect rect;        /* the button */
        int id;                     /* LM_PICK_PANEL_TRAY index */
        const struct lm_texture *icon;  /* premultiplied ARGB, NULL = none; valid only during the callback */
        const char *text;           /* "" = none; valid only during the callback */
        bool hover;
    } tray[LM_PANEL_MAX_TRAY];
};

/* A small menu card: the dock icon's right-click menu (the button's name and New window, Close all windows, Change..., Remove from dock) or a
 * menu a program asked for through mellagui_menu_v1 (menu.h: text field actions; no title). */
#define LM_MENU_MAX 12
#define LM_MENU_HEAD 28             /* the title row of the card */
struct lm_menu_info {
    struct lm_rect rect;            /* the card */
    const char *title;              /* the button's name; valid only during the callback */
    int n;                          /* entries */
    struct lm_rect entry[LM_MENU_MAX];
    const char *label[LM_MENU_MAX];
    bool enabled[LM_MENU_MAX];
    int sel;                        /* highlighted entry (pointer or keyboard), -1 none */
};

/* The session dialog: the output is dimmed, a card with round buttons is in the middle: Lock and Exit in the upper row, Suspend, Restart and
 * Shutdown (smaller) in the lower one. */
#define LM_POWER_N 5
struct lm_power_info {
    struct lm_rect rect;            /* the whole output (dimmed) */
    struct lm_rect card;
    struct lm_rect button[LM_POWER_N]; /* the squares the round buttons are drawn in: [0] Lock, [1] Exit, [2] Suspend, [3] Restart, [4] Shutdown */
    struct lm_rect hit[LM_POWER_N]; /* what a click counts for: the button and its label below */
    int sel;                        /* highlighted button (pointer or keyboard), -1 none */
    bool off[LM_POWER_N];           /* the button does nothing here (Suspend on a server: `[commands] suspend = none`): drawn dim, cannot be chosen */
    int others;                     /* while a restart / shutdown is confirmed: how many other login sessions are open */
    int confirm;                    /* 0, else the action (LM_LOCK_ACT_SUSPEND / REBOOT / POWEROFF, lock.h) the dialog asks to confirm: only [0] Yes and [1] Cancel are shown */
};

/*
 * The on-screen keyboard (osk.h): not modal, it floats along the bottom of the primary output above windows and the dock and never takes
 * the focus; keys are injected like a real keyboard's.
 */
#define LM_OSK_KEY_HOVER 1u
#define LM_OSK_KEY_DOWN 2u          /* held by the pointer */
#define LM_OSK_KEY_ON 4u            /* a latched Shift / Ctrl / Alt / AltGr, or Caps Lock is on */

struct lm_osk_info {
    struct lm_rect rect;            /* the card */
    int n;
    struct {
        struct lm_rect rect;
        char label[LM_OSK_LABEL * 4];
        uint32_t state;             /* LM_OSK_KEY_* */
        int kind;                   /* enum lm_osk_kind */
        int row;                    /* 0 = the Esc / F row ... 5 = bottom row (the colour) */
        uint32_t code;              /* evdev KEY_* (the arrows are drawn as shapes) */
    } key[LM_OSK_MAX_KEYS];
};

/*
 * The About dialog: the output dimmed, a card with the logo, the name and the system information (about.h), and a Close button.
 * Modal like the session dialog (lm_wm_pick gives it every point, lm_wm_key every key).
 */
struct lm_about_info {
    struct lm_rect rect;            /* the whole output (dimmed) */
    struct lm_rect card;
    struct lm_rect logo;            /* the square the round logo is drawn in */
    struct lm_rect close;           /* the Close button */
    bool hover_close;               /* the pointer is on it (or the keyboard focus: it is the only button) */
    int n;                          /* system information rows */
    const struct lm_about_row *row; /* valid only during the callback */
};

/*
 * The launcher dialog (Launchpad): a big translucent card over the dimmed output with every dock button as a big round icon with its name,
 * a "+" tile that opens the add form (name, command, icon) and an Edit button that puts a red x on every icon to remove it; in edit mode a click on an icon
 * opens the same form to change that button. Modal like the
 * About dialog. Buttons are taken from the dock (dock.h), added and removed in [dock-apps] of mellagui.conf.
 */
#define LM_LAUNCHER_CELL_W 124          /* one icon with its name */
#define LM_LAUNCHER_ICON 64             /* the round icon */
#define LM_LAUNCHER_HELP_D 18                          /* the "?" circle of a form field */
#define LM_LAUNCHER_KIND_N (LM_DOCKICON_COUNT + 1)      /* the icon choices of the form: "auto" and the built-in ones */
struct lm_launcher_info {
    struct lm_rect rect;            /* the whole output (dimmed) */
    struct lm_rect card;
    struct lm_rect title;
    bool form;                      /* the add / change form is showing instead of the icons */
    bool editing;                   /* ... and it changes an existing button (title "Change an application", button "Save") */
    bool edit;                      /* edit mode: a red x on every icon */
    const char *search;             /* the text typed to filter the icons ("" none); valid only during the callback */
    bool searching;                 /* search mode: letters go into the text */
    int n;                          /* icons */
    int cols;                       /* icons per row (the "+" tile counts as one) */
    int sel;                        /* highlighted: an icon index, n = the "+" tile, -1 none */
    struct {
        struct lm_rect box;         /* the round icon */
        struct lm_rect hit;         /* icon and name below it */
        struct lm_rect remove;      /* the x (edit mode) */
        const struct lm_dock_item *item;
        bool running;
    } icon[LM_DOCK_MAX];
    struct lm_rect add;             /* the "+" tile (box == hit), empty when the list is full */
    int drag;                       /* an icon is being dragged (icon index), -1 none */
    struct lm_rect drag_box;        /* the icon under the pointer */
    struct lm_rect drop;            /* while dragging: where it can be dropped (the dock's place), empty when the dock is off */
    int drop_pos;                   /* ... the pointer is on it: the place the icon would take in the dock, -1 not on it */
    struct lm_rect marker;          /* ... a bar between the dock icons that shows that place */
    struct lm_rect button[2];
    const char *button_text[2];
    int hover_button;               /* -1 none */
    const char *hint;
    const char *message;            /* an error of the form, "" none */
    /* the form */
    struct lm_rect field[3];        /* name, command, icon text (a built-in name, theme:NAME or a picture file) */
    const char *field_label[3];
    const char *field_text[3];
    int caret[3];                   /* the text cursor of each field: a byte offset on a character boundary */
    struct lm_rect help_btn[3];     /* the "?" circle at the end of each field */
    int help;                       /* 0 none, else the field (+ 1) whose help card is open: the card, its text and its Close button follow */
    const char *help_title, *help_text;
    struct lm_rect help_card, help_close;
    int focus;                      /* 0 name, 1 command, 2 icon text, 3 the row of built-in icons */
    struct lm_rect kind_box[LM_LAUNCHER_KIND_N];
    int kind;                       /* the built-in icon the icon text names (-1 = empty = guess, -2 = something else), what kind_box[i + 1] stands for */
    const char *kind_name;          /* its name for the label */
};

/* The part of a form field's text that is shown: [*start, *end) of `text`, at most `avail` pixels wide and holding the caret (a byte offset). The
 * painter and the pointer both use it, so a click lands where the caret is drawn. */
void lm_launcher_field_view(const char *text, int caret, int avail, int *start, int *end);

/*
 * The dock (dock.h): round icons along the bottom of the primary output. `box` is where each icon is painted now (the icon under the
 * pointer and its neighbours are bigger), `rect` everything the dock may ever paint on this output (full zoom, shadow, label), which is
 * also its scene bound.
 */
struct lm_dock_info {
    struct lm_output *output;
    struct lm_rect rect;
    int n;
    int hover;                      /* the icon under the pointer, -1 none */
    int dot_y;                      /* where the "running" dots are centred */
    struct {
        struct lm_rect box;
        bool running;
        bool launching;             /* its program was just started and has no window yet: the icon zooms in and out */
        const struct lm_dock_item *item;
        uint8_t alpha;              /* 255 = solid; the autohide animation melts the icon away (and brings it back) */
        float blur;                 /* 0 = sharp; 0..1 while an icon rises into its place it is a vague ghost that sharpens */
    } icon[LM_DOCK_MAX];
    const struct lm_dock_item *drag_item;   /* an icon is being dragged along the dock / out of it: it is painted at drag_box, the row has a gap */
    struct lm_rect drag_box;
    bool drag_remove;               /* ... and it is out of the dock: letting go removes it (a red x on the icon) */
    struct lm_rect label;           /* the name of the hovered icon (empty = none) */
    const char *label_text;         /* valid only during the callback */
};

#define LM_GADGET_ENTRIES 50        /* == LM_CLIP_HISTORY: the history can hold this many, the card shows as many as fit and scrolls */
#define LM_CLIP_DEFAULT_ENTRIES 5   /* entries a card of the default height shows */
#define LM_CLIP_BTN 20              /* the small icon buttons in the clipboard card's title bar: up, down, clear all (right to left: clear, down, up) */
#define LM_CLIP_BTN_GAP 4
#define LM_CLIP_DEL_W 26            /* the x of an entry sits in the last this many pixels of its slot */
#define LM_GADGET_SHADOW 14         /* the drop shadow reaches this far beyond `rect` (scene bounds include it) */
/*
 * The desktop gadget: a rounded, shadowed card on the first output (docked top right below the panel until the user
 * drags it; above the wallpaper and below every window) that lists the clipboard history, newest first. It can be
 * moved by its title bar and resized by its edges; the renderer wraps/truncates the entries to the width.
 */
struct lm_mixer_state;
struct lm_sysinfo_state;
struct lm_gadget_info {
    int id;                         /* enum lm_gadget_id */
    const struct lm_sysinfo_state *sys; /* LM_GADGET_SYSTEM: what to draw (valid only during the callback) */
    const struct lm_mixer_state *mixer; /* LM_GADGET_MIXER: what to draw (valid only during the callback) */
    struct lm_rect rect;            /* the card; the shadow lies outside */
    int n;                          /* entries drawn (<= slots) */
    int top;                        /* index (0 = newest) in the history of the first entry drawn */
    int total;                      /* entries in the history (the title badge) */
    int cursor;                     /* highlighted entry among the drawn ones (keyboard mode, Super+C), -1 = none */
    int hover;                      /* 0 nothing, 1 up, 2 down, 3 clear, 10 + k = the x of the k-th drawn entry (pointer on it) */
    struct lm_rect btn[3];          /* the title bar buttons: up, down, clear all */
    int slots;                      /* entry slots that fit (1..LM_GADGET_ENTRIES); the card scrolls when the history is longer */
    int entry_h;                    /* height of one entry slot; the title bar is `title_h` high */
    int title_h;
    bool dragging;                  /* being moved / resized */
    const char *text[LM_GADGET_ENTRIES]; /* newest first; valid only during the callback */
};

#define LM_SW_MAX 10                /* entries shown at once in the Alt+Tab list */
struct lm_switcher_info {
    struct lm_rect rect;
    int n;                          /* entries shown */
    int sel;                        /* selected entry, 0..n-1 */
    int item_h;
    int total;                      /* windows in all (the list scrolls when there are more than LM_SW_MAX) */
    int current_ws;
    const char *label[LM_SW_MAX];   /* valid only during the callback */
    int ws[LM_SW_MAX];              /* workspace of the window, 0-based */
    bool minimized[LM_SW_MAX];
    bool focused[LM_SW_MAX];        /* the window that has the focus now */
};

#define LM_NOTIFY_MAX 5             /* cards shown at once; a newer one pushes the oldest out */
#define LM_NOTIFY_LINES 4
#define LM_NOTIFY_COLS 36           /* characters per line of a card */
struct lm_notify_info {
    struct lm_rect rect;
    int id;
    char title[LM_NOTIFY_COLS * 4 + 1];
    char line[LM_NOTIFY_LINES][LM_NOTIFY_COLS * 4 + 1];
    int nlines;
};

/*
 * A ghost is a picture of something that is about to appear, disappear or move (a window with its frame): premultiplied ARGB,
 * painted by the scene as LM_SCENE_GHOST at any rectangle and opacity (scaled with bilinear filtering). The wm makes one when a
 * window opens, closes or is minimized, because then the real surface is hidden or already gone (render.h: lm_render_snapshot).
 */
struct lm_ghost {
    uint32_t *px;                       /* w * h premultiplied ARGB, NULL = empty */
    int w, h;
    unsigned gl_tex;                    /* the GL renderer's copy, made on first use */
};

enum lm_scene_kind {
    LM_SCENE_WALLPAPER,             /* one per output */
    LM_SCENE_GADGET,                /* the desktop gadget (right after the wallpapers, below all windows) */
    LM_SCENE_FRAME,                 /* decoration of one window */
    LM_SCENE_SURFACE,               /* a root surface tree (window content or popup) */
    LM_SCENE_PANEL_SHADOW,          /* the soft shadow the top panel throws on what is below it (one per output, painted before the panel) */
    LM_SCENE_PANEL,                 /* one per output */
    LM_SCENE_DOCK,                  /* the dock (above the panel, below the menu) */
    LM_SCENE_MENU,              /* the dock's right-click menu (above the dock) */
    LM_SCENE_NOTIFY,                /* one notification card (they stack top right, below the panel) */
    LM_SCENE_POWER,                 /* the session dialog (above windows, popups and notifications) */
    LM_SCENE_ABOUT,                 /* the About dialog (same place as the session dialog; the two are never open together) */
    LM_SCENE_LAUNCHER,              /* the launcher dialog (same place, never open together with the others) */
    LM_SCENE_OSK,                   /* the on-screen keyboard (above the dialogs, so it can type into the launcher's form) */
    LM_SCENE_GHOST,                 /* a picture of a window that opens, closes or is minimized (above the windows, below the panel) */
    LM_SCENE_OVERVIEW,              /* the desktop overview: dimmed output, workspace strip, frames and labels (its window pictures are LM_SCENE_GHOST items right after it) */
    LM_SCENE_SWITCHER,              /* the Alt+Tab window list (above everything but the lock screen) */
    LM_SCENE_LOCK,                  /* the lock screen: while locked it is the ONLY item (one per output) */
};

#define LM_OVERVIEW_MAX 16
struct lm_overview_info {
    struct lm_rect rect;            /* the whole output */
    int dim;                        /* 0..255: how dark the output is behind the pictures (grows while it opens) */
    bool ui;                        /* the strip, frames and labels are shown (the pictures have arrived) */
    struct {
        struct lm_rect rect;
        bool active, hover;
        int count;                  /* windows on that workspace */
    } ws[LM_NUM_WORKSPACES];
    int n;
    struct {
        struct lm_rect frame;       /* around the picture at its place in the grid */
        struct lm_rect label;
        char title[64];
        bool sel, hover, minimized;
    } item[LM_OVERVIEW_MAX];
};

struct lm_scene_item {
    enum lm_scene_kind kind;
    struct lm_output *output;       /* WALLPAPER and PANEL */
    union {
        struct lm_frame_info frame; /* LM_SCENE_FRAME */
        struct {
            struct lm_surface *root;
            int32_t x, y;           /* global origin of the root surface (for lm_surface_tree_visit) */
            bool is_popup;
        } surface;                  /* LM_SCENE_SURFACE */
        struct lm_panel_info panel; /* LM_SCENE_PANEL */
        struct lm_rect shadow;      /* LM_SCENE_PANEL_SHADOW: the strip under the panel */
        struct {
            struct lm_ghost *ghost; /* the picture (w x h) */
            struct lm_rect dst;     /* where it is painted now, global: scaled to this */
            struct lm_rect src;     /* the part of the picture painted (empty = all of it): the cells of a wobbling window */
            int alpha;              /* 0..255 */
        } ghost;                    /* LM_SCENE_GHOST */
        struct lm_about_info about; /* LM_SCENE_ABOUT */
        struct lm_launcher_info launcher; /* LM_SCENE_LAUNCHER */
        struct lm_overview_info overview; /* LM_SCENE_OVERVIEW */
        struct lm_dock_info dock;   /* LM_SCENE_DOCK */
        struct lm_osk_info osk;     /* LM_SCENE_OSK */
        struct lm_power_info power; /* LM_SCENE_POWER */
        struct lm_menu_info menu; /* LM_SCENE_MENU */
        struct lm_gadget_info gadget; /* LM_SCENE_GADGET */
        struct lm_lock_info lock;   /* LM_SCENE_LOCK */
        struct lm_switcher_info switcher; /* LM_SCENE_SWITCHER */
        struct lm_notify_info notify;   /* LM_SCENE_NOTIFY */
    } u;
};

/* Animation state for the control socket (`anim`): "anim n=N" and one "item kind=open|close|minimize|restore x= y= w= h= alpha=" per
 * running animation; returns the length. */
size_t lm_wm_anim_describe(struct lm_wm *wm, char *buf, size_t size);

/* Return false to stop the iteration. */
typedef bool (*lm_scene_fn)(const struct lm_scene_item *item, void *data);

/*
 * Report scene items intersecting `clip` (global coordinates; NULL = all) in
 * painting order, bottom to top: wallpaper(s), then for each visible window
 * in stacking order its FRAME then its SURFACE, then the panel(s), then all
 * popups (SURFACE with is_popup). Minimized windows and windows on other
 * workspaces are skipped. A fullscreen focused window hides the panel of its
 * output and covers it. The cursor is NOT part of the scene. While the screen
 * is locked the scene is just one LM_SCENE_LOCK item per output.
 */
void lm_wm_scene_for_each(struct lm_wm *wm, const struct lm_rect *clip,
                          lm_scene_fn fn, void *data);

/*
 * Text snapshot for tests (control socket `windows`): writes at most `size`
 * bytes (NUL-terminated) into buf and returns the length. Format, one line each:
 *   workspace <n>
 *   pointer <x> <y>
 *   window <id> "<title>" "<app_id>" x=<x> y=<y> w=<w> h=<h> ws=<n> flags=<comma list of
 *       mapped,focused,maximized,fullscreen,minimized or "-">
 * (<id> is a stable small integer per window, in stacking order bottom to top.)
 */
size_t lm_wm_describe(struct lm_wm *wm, char *buf, size_t size);

/* The About dialog (the panel's logo button; control socket `about`): open it on the primary output, close it, ask whether it is open. */
void lm_wm_about_set(struct lm_wm *wm, bool open);
/* The desktop overview (key action `overview`, control socket `overview`): the windows of this workspace as pictures in a grid with the
 * workspaces above them; click / Enter goes to a window, Esc closes. */
void lm_wm_overview_set(struct lm_wm *wm, bool open);
bool lm_wm_overview_is_open(const struct lm_wm *wm);
size_t lm_wm_overview_describe(struct lm_wm *wm, char *buf, size_t size);   /* "overview open=0|1 closing= n= sel=" and an `item` line each */
bool lm_wm_about_is_open(const struct lm_wm *wm);
size_t lm_wm_about_describe(struct lm_wm *wm, char *buf, size_t size);      /* the lines of the control socket `about` while it is open */

/* The launcher dialog (Super+A, the dock's Launcher button, control socket `launcher`). */
void lm_wm_launcher_set(struct lm_wm *wm, bool open);
bool lm_wm_launcher_is_open(const struct lm_wm *wm);
/* Control socket `launcher`: "launcher open=<0|1> edit=<0|1> form=<0|1> n=<icons> sel=<i>" and, while open, "card x= y= w= h=", one
 * "icon <i> "<name>" x= y= w= h= remove=x,y add=x,y,w,h"-style line each (see the function), then the buttons. */
size_t lm_wm_launcher_describe(struct lm_wm *wm, char *buf, size_t size);
/* Control socket `dockmenu`: "dockmenu open=<0|1> item=<dock index> sel=<i>" and, while open, "card x= y= w= h=" and one "entry i \"label\" x= y= w= h= enabled=" line each. */
size_t lm_wm_menu_describe(struct lm_wm *wm, char *buf, size_t size);
/* Run the configured command of a session action (LM_LOCK_ACT_SUSPEND / REBOOT / POWEROFF, lock.h; `[commands] suspend` ...). A command that fails is
 * reported in a notification with what it wrote to stderr ("Suspend failed: Interactive authentication required"). */
void lm_wm_power_run(struct lm_wm *wm, int action);
/* A menu for the program behind mellagui_menu_v1 at the pointer (menu.c): `labels` (a leading '~' = shown dim, cannot be chosen), n <= LM_MENU_MAX.
 * The wm answers through lm_menu_chosen(index) / lm_menu_closed() (menu.h). False when it cannot be shown (locked, no output, too many entries). */
bool lm_wm_menu_popup(struct lm_wm *wm, const char *const *labels, int n);
/* Close a menu that was asked for by a program (it went away). */
void lm_wm_menu_cancel(struct lm_wm *wm);

/* A program's tray item changed, appeared or went (tray.c): the bar is laid out and painted again. */
void lm_wm_tray_changed(struct lm_wm *wm);
/* Control socket `tray`: "tray n=<items>" and one "item <id> x= y= w= h= icon=<w>x<h>|- text=<text>" line each (first output's bar). */
size_t lm_wm_tray_describe(struct lm_wm *wm, char *buf, size_t size);
/* "clock x= y= w= h= tasks=N text=..." (the primary output's panel; `tasks` = windows in the taskbar) and the calendar window ("calendar ..." line). */
size_t lm_wm_clock_describe(struct lm_wm *wm, char *buf, size_t size);

/* The on-screen keyboard (panel button, key action `osk`, control socket `osk`). */
void lm_wm_osk_set(struct lm_wm *wm, bool open);
bool lm_wm_osk_is_open(const struct lm_wm *wm);
/* Back to the default place and size (a double click on its margin, control socket `osk reset`); the saved place is forgotten. The place
 * the user gave it is kept in the gadgets file (`keyboard x y w kh`, --gadgets-file). */
void lm_wm_osk_reset(struct lm_wm *wm);
/* The keyboard opened, closed, was moved / resized, or the setting `osk-avoid` changed: maximized windows are refitted to the usable area (which leaves out the
 * keyboard while it sits in the bottom or top half of the screen) and floating ones that it would cover move up (and shrink when they must) and
 * go back when it is gone. */
void lm_wm_osk_layout_changed(struct lm_wm *wm);
/* Control socket `osk`: "osk open=<0|1> latched=<LM_MOD_* mask> n=<keys>" and, while open, "card x= y= w= h=" and one
 * "key <i> code=<evdev> kind=<k> x= y= w= h= on=<latched> held=<lit by a key that is down> label=<text>" line per key. */
size_t lm_wm_osk_describe(struct lm_wm *wm, char *buf, size_t size);

/* The clipboard card's view: the first history entry it shows and how many entries fit (control socket `clip`). */
void lm_wm_clip_view(struct lm_wm *wm, int *top, int *slots);

/* Some visible window is fullscreen (the screensaver does not start over a film or a game). */
bool lm_wm_has_fullscreen(const struct lm_wm *wm);
/* Next window placement starts at the first cascade position again (tests: the place of a window then does not depend on how many
 * windows the tests before it opened). */
void lm_wm_reset_cascade(struct lm_wm *wm);
/* The clock / date settings changed: write the panel's clock again. */
void lm_wm_clock_changed(struct lm_wm *wm);
/* Calendar (client/mellagui-calendar.c, app id "mellagui.calendar"): a program that runs hidden (a minimized window that the taskbar, Alt+Tab and
 * the dock do not list) and is shown under the panel's clock by a click on it; it hides again when it loses the focus. lm_wm_calendar_start
 * starts it (main.c, at startup); lm_wm_calendar_toggle shows / hides it, starting it first when it is not there (`clock` = where the clock is, NULL =
 * the primary output's); false when calendars are off (setting cal-enabled) or there is no command. */
void lm_wm_calendar_start(struct lm_wm *wm);
bool lm_wm_calendar_toggle(struct lm_wm *wm, const struct lm_rect *clock);
/* The window of this surface (or of the root it belongs to) is mapped, not minimized and on the current workspace. */
bool lm_wm_surface_visible(const struct lm_wm *wm, struct lm_surface *s);

/* Text snapshot of the dock for tests (control socket `dock`): "dock shown=<0|1> hover=<i> n=<buttons>" then one line per button
 * "icon <i> "<name>" x= y= w= h= running=<0|1> launching=<0|1> cmd=<command>". Returns the length. */
size_t lm_wm_dock_describe(struct lm_wm *wm, char *buf, size_t size);

#endif
