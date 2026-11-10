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

struct lm_surface;
struct lm_buffer;
struct lm_gl;

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

/* The GL context the renderer draws with (NULL for software): dmabuf.c asks it which buffer layouts the GPU can sample. */
struct lm_gl *lm_render_gl(struct lm_renderer *r);

/* Headless: copies what the GL renderer drew into the backend's memory framebuffer (dump, tests). No-op for software. */
void lm_render_readback(struct lm_renderer *r);

/* Zero copy: makes `s`'s GL texture show the dmabuf `b` (an EGLImage; no CPU read). false = not possible here (software renderer,
 * no GL dmabuf import, the driver refused): the caller falls back to copying the pixels. */
bool lm_render_dmabuf_import(struct lm_renderer *r, struct lm_surface *s, struct lm_buffer *b);
/* The surface leaves the zero-copy path (new shm content, unmapped): drops the image and the texture. */
void lm_render_dmabuf_drop(struct lm_renderer *r, struct lm_surface *s);

/* The desktop background picture (settings: wallpaper, wallpaper-mode 0 fill 1 fit 2 stretch 3 center): loads it (PNG / PPM through
 * img.c) and repaints. An empty path or a file that cannot be read gives the gradient. */
void lm_render_set_wallpaper(struct lm_renderer *r, const char *path, int mode);
const char *lm_render_wallpaper_status(const struct lm_renderer *r);          /* "", "ok" or "error: ..." */

/* The surface is being freed: its GL texture (when the GL renderer made one) goes. */
struct lm_surface;
void lm_render_surface_gone(struct lm_renderer *r, struct lm_surface *s);

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
/* The same with the pointer painted in when `cursor` is set and `rect` is a whole output (the screen recorder; screenshots leave it out). */
uint32_t *lm_render_capture_cursor(struct lm_renderer *r, const struct lm_rect *rect, int *w, int *h, bool cursor);

/*
 * Feedback for a screenshot of `where` (global): a short white flash over it and, for MELLAGUI_TOAST_MS (default 8 s), a
 * card with a thumbnail of the pixels `px` (w x h XRGB8888, stride w * 4) and `label` in the bottom right corner of
 * the output; `path` (or NULL) is the saved file. A new one replaces the old. Never part of a screenshot.
 */
void lm_render_shot_feedback(struct lm_renderer *r, const struct lm_rect *where, const uint32_t *px, int w, int h,
                             const char *label, const char *path);
/* The card is showing and the global point is on it (not while the screen is locked). */
bool lm_render_shot_card_at(struct lm_renderer *r, double x, double y);
/* Removes the card; copies the saved file's path to `path` ("" when only the clipboard got the shot). Returns true when
 * there is a path. `path` may be NULL. */
bool lm_render_shot_dismiss(struct lm_renderer *r, char *path, size_t n);

#endif
