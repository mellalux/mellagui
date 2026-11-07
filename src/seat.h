#ifndef MELLAGUI_SEAT_H
#define MELLAGUI_SEAT_H

/*
 * wl_seat (v5: pointer, keyboard, touch), wl_data_device_manager (text
 * clipboard between clients; drag-and-drop is accepted but not performed),
 * pointer cursor handling, and the pointer-grab mechanism.
 * Owner: seat.c (and keymap.c).
 *
 * ---- Input flow ---------------------------------------------------------------
 *  input.c -> lm_seat_handle_input(ev)
 *   KEY:   lm_wm_key() first. If it returns true the press is swallowed and
 *          so is the matching release (seat remembers swallowed codes).
 *          Otherwise (not a repeat) wl_keyboard.key + .modifiers go to the
 *          keyboard-focus client. Repeat events (ev.key.repeat) are never
 *          forwarded to clients.
 *   MOTION: update pointer position (clamped with lm_output_clamp_point).
 *          If a pointer grab is active -> grab->ops->motion. Otherwise
 *          lm_wm_pick() -> lm_wm_pointer_motion(); if the pick is a client
 *          surface set pointer focus + send motion, else pointer focus = NULL.
 *   BUTTON: grab -> grab->ops->button. Otherwise lm_wm_pointer_button();
 *          if it returns true (decoration/panel click) the event is consumed;
 *          else it is sent to the focused surface. (The wm raises/focuses the
 *          clicked window inside lm_wm_pointer_button, before the client
 *          gets the button.)
 *   AXIS:  forwarded to the pointer-focus client (wl_pointer v5: axis_source,
 *          axis, axis_discrete, axis_stop on smooth end, then frame).
 *   TOUCH: lm_wm_pick() for touch-down targets, events go to the client that
 *          owns the touched surface; touch also moves the logical pointer only
 *          for LM_INPUT_POINTER_MOTION_ABS.
 * Pointer events to a client end with wl_pointer.frame (v5).
 *
 * ---- Serials ----------------------------------------------------------------------
 * Every enter/leave/button/key/touch-down event uses lm_server_next_serial().
 * lm_seat_serial_valid() lets xdg validate the serial of move/resize/grab
 * requests: valid if it equals the serial of the latest button press, key
 * press, or touch-down that was sent to a client of the requesting resource.
 */

#include <stdbool.h>
#include <stdint.h>
#include "input.h"
#include "server.h"
#include "util.h"

struct lm_surface;

/* ---- lifecycle ---------------------------------------------------------------------- */

/* Advertises wl_seat (v5; capabilities depend on discovered devices, at least pointer+keyboard) and wl_data_device_manager (v3). */
struct lm_seat *lm_seat_create(struct lm_server *srv);
void lm_seat_destroy(struct lm_seat *seat);

/* Input handler for lm_input_set_handler (data = struct lm_seat *). */
void lm_seat_handle_input_cb(const struct lm_input_event *ev, void *data);
void lm_seat_handle_input(struct lm_seat *seat, const struct lm_input_event *ev);

/* ---- pointer ----------------------------------------------------------------------------- */

void lm_seat_pointer_position(const struct lm_seat *seat, double *x, double *y);
/* Move the pointer (global coords, clamped) as if the user did; triggers focus update. */
void lm_seat_pointer_warp(struct lm_seat *seat, double x, double y);
struct lm_surface *lm_seat_pointer_focus(const struct lm_seat *seat);

/*
 * Primitive operations, used by seat.c's default behaviour and by grab
 * implementations (xdg popup grabs) that want to re-route events:
 *  set_focus    : leave the old focus surface, enter `s` (may be NULL) with
 *                 the pointer at surface-local (sx, sy); the surface origin
 *                 is remembered as pointer_pos - (sx, sy).
 *  send_motion  : wl_pointer.motion to the current focus for the stored
 *                 pointer position (+frame).
 *  send_button  : wl_pointer.button (+frame) to the focus; records the serial.
 *  send_axis    : full axis sequence to the focus.
 */
void lm_seat_pointer_set_focus(struct lm_seat *seat, struct lm_surface *s,
                               double sx, double sy);
void lm_seat_pointer_send_motion(struct lm_seat *seat, uint32_t time_ms);
void lm_seat_pointer_send_button(struct lm_seat *seat, uint32_t time_ms,
                                 uint32_t button, bool pressed);
void lm_seat_pointer_send_axis(struct lm_seat *seat, uint32_t time_ms,
                               int orientation, double value, int32_t discrete);

/* ---- pointer grabs -------------------------------------------------------------------------- */

struct lm_pointer_grab;
struct lm_pointer_grab_ops {
    /* The pointer is already at (x, y) when called (global coordinates). */
    void (*motion)(struct lm_pointer_grab *g, uint32_t time_ms, double x, double y);
    void (*button)(struct lm_pointer_grab *g, uint32_t time_ms, uint32_t button,
                   bool pressed);
    /* Optional (NULL = scrolling is dropped during the grab). */
    void (*axis)(struct lm_pointer_grab *g, uint32_t time_ms, int orientation,
                 double value, int32_t discrete);
    /* The grab was taken away (replaced, seat destroyed, session lost). */
    void (*cancel)(struct lm_pointer_grab *g);
};

/* Caller-owned storage; must stay valid until lm_seat_pointer_ungrab or cancel. */
struct lm_pointer_grab {
    const struct lm_pointer_grab_ops *ops;
    void *data;
};

/*
 * Install a grab. Replaces (and cancels) an existing grab. Starting a grab
 * sets the pointer focus to NULL (leave is sent); a grab that wants clients
 * to keep getting events (popup grabs) re-establishes focus itself using the
 * primitives above. No-op return -1 if seat/grab is NULL.
 */
int lm_seat_pointer_grab(struct lm_seat *seat, struct lm_pointer_grab *grab);
/* Remove the grab if it is the active one (does not call cancel) and recompute focus. */
void lm_seat_pointer_ungrab(struct lm_seat *seat, struct lm_pointer_grab *grab);
bool lm_seat_pointer_has_grab(const struct lm_seat *seat);

/* ---- keyboard ------------------------------------------------------------------------------- */

/*
 * Set keyboard focus (NULL = none): sends wl_keyboard.leave / enter (with the
 * currently pressed keys and modifiers) and updates the clipboard selection
 * (wl_data_device.selection) for the newly focused client. Called by the wm.
 * The seat clears focus itself when the focused surface is destroyed.
 */
void lm_seat_set_keyboard_focus(struct lm_seat *seat, struct lm_surface *s);
struct lm_surface *lm_seat_keyboard_focus(const struct lm_seat *seat);

/* ---- clipboard history -----------------------------------------------------------------------------------
 * Whenever a client sets a text selection the seat reads that text itself (asynchronously, through a pipe handed to
 * the data source, at most 2 KiB, 1 s) and remembers the last LM_CLIP_HISTORY distinct texts for the wm's clipboard
 * gadget. Clearing the selection keeps the history. Entry 0 is the newest; the pointer is valid until the next
 * clipboard change. A change is announced with lm_wm_clipboard_changed().
 */
#define LM_CLIP_HISTORY 5
/* Settings panel: new set of keyboard layouts (XKB groups) / new key repeat values (from server->config). */
void lm_seat_set_layouts(struct lm_seat *seat, const int *layouts, int n);
void lm_seat_send_repeat_info(struct lm_seat *seat);

int lm_seat_clip_count(const struct lm_seat *seat);
const char *lm_seat_clip_get(const struct lm_seat *seat, int i);
/* Make history entry `i` the current selection (the compositor serves its text itself) and move it to the top. */
void lm_seat_clip_use(struct lm_seat *seat, int i);
/* The compositor becomes the selection owner and serves `data` (malloc'd, taken over even on failure) as `mime`
 * (screenshots: "image/png"). The history is not touched. */
bool lm_seat_set_selection_data(struct lm_seat *seat, const char *mime, uint8_t *data, size_t len);
void lm_seat_clip_clear(struct lm_seat *seat);      /* forget the history (control socket `clip clear`) */

/*
 * Screen lock (lock.c): while locked the seat sends nothing to clients. Keyboard and pointer focus are dropped (and stay
 * NULL whatever the wm asks for), a pointer grab is cancelled, the cursor is hidden, and input goes to lm_lock_key() only.
 * Unlocking lets the pointer find its surface again; the wm gives the keyboard focus back (lm_wm_lock_changed).
 */
void lm_seat_set_locked(struct lm_seat *seat, bool locked);

/* ---- serials -------------------------------------------------------------------------------- */

bool lm_seat_serial_valid(const struct lm_seat *seat, struct lm_client *client,
                          uint32_t serial);

/* ---- cursor --------------------------------------------------------------------------------- */

enum lm_cursor_shape {
    LM_CURSOR_NONE = 0,       /* only valid as "no override" in lm_seat_set_cursor_override */
    LM_CURSOR_DEFAULT,        /* built-in arrow */
    LM_CURSOR_HIDDEN,
    LM_CURSOR_SURFACE,        /* draw lm_cursor.surface with hotspot */
    LM_CURSOR_MOVE,
    LM_CURSOR_RESIZE_N, LM_CURSOR_RESIZE_S, LM_CURSOR_RESIZE_W, LM_CURSOR_RESIZE_E,
    LM_CURSOR_RESIZE_NW, LM_CURSOR_RESIZE_NE, LM_CURSOR_RESIZE_SW, LM_CURSOR_RESIZE_SE,
    LM_CURSOR_POINTER,        /* hand, for panel buttons */
};

struct lm_cursor {
    enum lm_cursor_shape shape;
    struct lm_surface *surface;   /* only for LM_CURSOR_SURFACE; has role "cursor" */
    int32_t hot_x, hot_y;         /* hotspot within the surface / built-in sprite */
};

/*
 * Effective cursor: the wm override if set, else the cursor surface set by
 * the pointer-focus client via wl_pointer.set_cursor (honoured only if the
 * request's serial matches the latest pointer enter), else the default arrow.
 */
void lm_seat_get_cursor(const struct lm_seat *seat, struct lm_cursor *out);
/* wm forces a shape (resize edges, panel hover); LM_CURSOR_NONE removes the override. */
void lm_seat_set_cursor_override(struct lm_seat *seat, enum lm_cursor_shape shape);
/* Fire frame callbacks of the cursor surface (called after a frame was presented). */
void lm_seat_send_frame_done(struct lm_seat *seat, uint32_t time_ms);

#endif
