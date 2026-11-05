#ifndef LUMEN_XDG_H
#define LUMEN_XDG_H

/*
 * xdg_wm_base (v3), xdg_surface, xdg_toplevel, xdg_popup, xdg_positioner and
 * zxdg_decoration_manager_v1. Owner: xdg.c.
 *
 * xdg.c is the *shell protocol adapter*: it owns all xdg object state and the
 * configure/ack_configure serial bookkeeping, and talks to the window
 * manager exclusively through struct lm_window / struct lm_popup (wm.h):
 *
 *   toplevel:  xdg_toplevel creation -> lm_wm_window_create(wm, surface,
 *              &ops, xdg_toplevel_state). Requests from the client
 *              (set_title, set_min_size, set_maximized, move, resize, ...)
 *              are forwarded to lm_wm_window_* functions. The wm drives
 *              configures through struct lm_window_ops.
 *   popup:     xdg_popup creation -> positioner math (anchor rect, anchor,
 *              gravity, offset, constraint adjustment: slide/flip/resize,
 *              all against lm_wm_usable_area()) -> lm_wm_popup_create +
 *              lm_wm_popup_set_position; xdg_popup.grab installs a pointer
 *              grab through lm_seat_pointer_grab.
 *
 * Mapping logic: an xdg_surface is mapped when the client commits a buffer
 * after having acked a configure; xdg.c's surface role->commit hook then
 * calls lm_wm_window_map / lm_wm_popup_map; a null-buffer commit unmaps.
 * Initial commit without buffer triggers lm_wm_window_initial_configure().
 * Committing a buffer before the initial configure is acked is the
 * protocol error xdg_surface.unconfigured_buffer.
 *
 * zxdg_decoration_manager_v1: every toplevel decoration object immediately
 * (and after set_mode/unset_mode) gets configure(server_side).
 */

#include "server.h"
#include "surface.h"

/* Advertises xdg_wm_base (v3) and zxdg_decoration_manager_v1 (v1). */
struct lm_xdg *lm_xdg_create(struct lm_server *srv);
void lm_xdg_destroy(struct lm_xdg *xdg);

/* Roles used for xdg surfaces: lets other modules identify surfaces. */
extern const struct lm_surface_role lm_xdg_toplevel_role;
extern const struct lm_surface_role lm_xdg_popup_role;

/*
 * Send popup_done to every popup (and nested popup) whose root is the
 * surface `s` (a toplevel or popup surface) and destroy their grabs. Called by
 * the wm when a toplevel is closed/unmapped or focus moves elsewhere.
 */
void lm_xdg_dismiss_popups(struct lm_surface *s);

/* Ping the client that owns `s` (xdg_wm_base.ping); unresponsive clients are only logged. */
void lm_xdg_ping(struct lm_surface *s);

#endif
