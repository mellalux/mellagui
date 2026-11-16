#ifndef MELLAGUI_SCREENCOPY_H
#define MELLAGUI_SCREENCOPY_H

/*
 * zwlr_screencopy_manager_v1 version 3 (wlr-screencopy-unstable-v1): what grim, wf-recorder, slurp's picker and OBS's wlroots source use.
 * A program asks for a frame of an output, or of a rectangle of it (logical coordinates of the output), is told the buffer it has to
 * provide (`buffer`: XRGB8888, the size in the output's own pixels - with a scale of 2 a 100 x 50 rectangle is 200 x 100 pixels -,
 * `buffer_done`), gives a wl_shm buffer with `copy` or `copy_with_damage` and gets `flags`, `damage` and `ready` (the time) or `failed`.
 * Only wl_shm buffers are offered (no `linux_dmabuf` event: the client then uses shared memory).
 *
 * `copy` is answered at once (from the loop, not inside the request). `copy_with_damage` waits until the output has changed since the
 * last frame that program got of it (the first one is answered at once), so a recorder that asks again after every frame is paced by the
 * screen's own changes. The picture is the scene without the pointer unless `overlay_cursor` is set and the whole output is wanted (a
 * rectangle never has the pointer in it). A locked screen fails every frame. Not offered in kiosk mode (the login screen).
 * The frame's size is fixed when it is asked for: a mode change in between fails it. Owner: screencopy.c.
 */

struct lm_server;
struct lm_output;
struct lm_screencopy;

struct lm_screencopy *lm_screencopy_create(struct lm_server *srv);
void lm_screencopy_destroy(struct lm_screencopy *sc);
/* The renderer says some output has new damage (may be called from inside painting: only schedules). */
void lm_screencopy_damaged(struct lm_screencopy *sc);
/* The output is going away: its waiting frames fail. */
void lm_screencopy_output_removed(struct lm_screencopy *sc, struct lm_output *out);
/* Control socket `screencopy`: frames waiting, frames delivered, frames failed. */
int lm_screencopy_pending(const struct lm_screencopy *sc);
int lm_screencopy_delivered(const struct lm_screencopy *sc);
int lm_screencopy_failed(const struct lm_screencopy *sc);

#endif
