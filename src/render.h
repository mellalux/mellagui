#ifndef MELLAGUI_RENDER_H
#define MELLAGUI_RENDER_H

/*
 * Software compositor: walks the wm scene, composites into the backend
 * framebuffer with per-output damage tracking, draws cursor. Owner: render.c
 * (and font.c).
 *
 * ---- Drawing --------------------------------------------------------------------
 * Premultiplied ARGB source over XRGB destination, fast paths: opaque
 * (tex.opaque or opaque-region) rows are memcpy'd, fully transparent skipped,
 * scale > 1 surfaces sampled nearest-neighbour. Clip every blit to the
 * output rect and the damage rect. Decorations/panel/wallpaper are painted
 * from struct lm_scene_item data (wm.h) using the font from font.h; colour
 * scheme is the renderer's own business. The software cursor (shape
 * from lm_seat_get_cursor: built-in sprites for every lm_cursor_shape, or the
 * client's cursor surface tree) is drawn last, after the scene.
 *
 * ---- Damage and scheduling ------------------------------------------------------
 * Damage arrives in global coordinates (lm_render_damage), is split per
 * output and accumulated. A frame is drawn only when an output is "ready"
 * (backend output_ready) AND has damage. With buffer age (struct
 * lm_framebuffer.age) the renderer keeps the damage of the last 2-3 frames and
 * redraws the union needed to bring the back buffer up to date.
 * Drawing is triggered from an idle callback (coalescing bursts) or
 * immediately from lm_render_output_ready().
 *
 * Frame callbacks: after a present became visible (next output_ready) the
 * renderer calls lm_wm_send_frame_done() and lm_seat_send_frame_done(). If a
 * client asked for a frame callback but nothing is damaged
 * (lm_render_request_frame) a ~16 ms timer fires the callbacks anyway so
 * clients are not starved.
 */

#include <stdint.h>
#include "backend.h"
#include "server.h"
#include "util.h"

struct lm_renderer *lm_render_create(struct lm_server *srv);
void lm_render_destroy(struct lm_renderer *r);

void lm_render_output_added(struct lm_renderer *r, struct lm_output *out);
void lm_render_output_changed(struct lm_renderer *r, struct lm_output *out);
void lm_render_output_removed(struct lm_renderer *r, struct lm_output *out);

/* Damage a rectangle in global coordinates (all outputs it touches). */
void lm_render_damage(struct lm_renderer *r, const struct lm_rect *rect);
void lm_render_damage_all(struct lm_renderer *r);
/* The cursor moved / changed image / was hidden: damages old and new cursor rects. */
void lm_render_cursor_changed(struct lm_renderer *r);
/* A client waits for a frame callback: make sure one frame tick happens. */
void lm_render_request_frame(struct lm_renderer *r);

/* Backend says `out` can be drawn (see backend.h frame protocol); `time_ms` is msec32. */
void lm_render_output_ready(struct lm_renderer *r, struct lm_output *out, uint32_t time_ms);

/*
 * Draw and present every ready output that has damage, right now (tests,
 * headless dump/sync). Does not wait for vblank.
 */
void lm_render_flush(struct lm_renderer *r);

/*
 * Paints the scene (no cursor) into a private buffer and returns the part of it inside `rect` (global
 * coordinates, clipped to the output it mostly lies on) as a malloc'd XRGB8888 array of *w x *h pixels
 * (stride w * 4), or NULL. For screenshots.
 */
uint32_t *lm_render_capture(struct lm_renderer *r, const struct lm_rect *rect, int *w, int *h);

/*
 * Feedback for a screenshot of `where` (global): a short white flash over it and, for MELLAGUI_TOAST_MS (default 3 s), a
 * card with a thumbnail of the pixels `px` (w x h XRGB8888, stride w * 4) and `label` in the bottom right corner of
 * the output. A new one replaces the old. Never part of a screenshot.
 */
void lm_render_shot_feedback(struct lm_renderer *r, const struct lm_rect *where, const uint32_t *px, int w, int h,
                             const char *label);

#endif
