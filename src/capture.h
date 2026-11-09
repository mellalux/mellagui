#ifndef MELLAGUI_CAPTURE_H
#define MELLAGUI_CAPTURE_H

/*
 * Screen capture for MellaGUI's own screen recorder (client/mellagui-record.c): the private global `mellagui_capture_v1`.
 *
 *   list            -> one `output(index, name, x, y, width, height)` per output (index = position in the backend's list), then `done`
 *   grab(index, wl_buffer)
 *                   -> the output is painted (the scene without the cursor, like a screenshot) into the client's wl_shm buffer, which
 *                      must be exactly as big as the output (the client learns that from `list`), then `ready(index, w, h)`.
 *                      `failed(index, reason)` instead: "no-output", "buffer" (not an shm buffer / wrong format), "size", "locked"
 *                      (the screen lock is on: nothing is captured then) or "capture".
 *
 * Access is the same as for the settings (lm_settings_client_allowed): on the drm backend only the executables called mellagui-*
 * next to the compositor; any other client binds an inert object. A screen shows secrets, so no other client gets it.
 * Every grab repaints the output in software (or reads the GPU's picture back), so a recorder should not ask for more than
 * about 30 a second. Owner: capture.c.
 */

#include "server.h"

struct lm_capture;

struct lm_capture *lm_capture_create(struct lm_server *srv);
void lm_capture_destroy(struct lm_capture *c);

#endif
