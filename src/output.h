#ifndef MELLAGUI_OUTPUT_H
#define MELLAGUI_OUTPUT_H

/*
 * wl_output globals (one per struct lm_output) and the output layout.
 * Owner: output.c.
 *
 * Layout: outputs are placed left to right in the order they are added
 * (first at 0,0); output.c assigns out->x/out->y. The global compositor space
 * is the union of the output rectangles.
 */

#include <stdbool.h>
#include "backend.h"
#include "server.h"
#include "util.h"

struct lm_output_mgr *lm_output_mgr_create(struct lm_server *srv);
void lm_output_mgr_destroy(struct lm_output_mgr *mgr);

/*
 * Called by main.c when the backend reports a new output: assigns x/y,
 * advertises a wl_output (v3) global with out as bind data.
 */
void lm_output_mgr_add(struct lm_output_mgr *mgr, struct lm_output *out);
/* The outputs' places changed (settings output-order): packs them again, announcing the ones that moved. */
void lm_output_mgr_relayout(struct lm_output_mgr *mgr);
/* The leftmost output (the one the gadgets and notifications live on); NULL without outputs. */
struct lm_output *lm_output_primary(struct lm_server *srv);
/* Mode/geometry changed: re-send geometry/mode/scale/done to bound resources. */
void lm_output_mgr_changed(struct lm_output_mgr *mgr, struct lm_output *out);
/* Output going away: destroys the global, sets bound resources' data to NULL, re-packs the layout. */
void lm_output_mgr_remove(struct lm_output_mgr *mgr, struct lm_output *out);

/*
 * First wl_output resource bound by `client` for `out` (for wl_surface.enter
 * / leave), or NULL if the client never bound that output.
 */
struct lm_resource *lm_output_resource_for_client(struct lm_output *out,
                                                  struct lm_client *client);

/* ---- layout queries (usable by every module; srv->backend->outputs is the source) ---- */

/* Bounding box of all outputs. Returns false if there are none. */
bool lm_output_layout_bounds(struct lm_server *srv, struct lm_rect *out);
/* Output whose rect is closest to the point (never NULL while any output exists). */
struct lm_output *lm_output_nearest(struct lm_server *srv, double x, double y);
/* ---- scale ---------------------------------------------------------------------------------------------------
 * An output has a physical mode (mode_w x mode_h pixels, the framebuffer) and a scale s (1.0, 1.25, 1.5 ... 4.0):
 * its logical size is mode / s, and the wm, the layout, the clients' sizes and every coordinate outside the renderer
 * and the backends are logical. The renderer paints the shell at s times the size (sharp) and shrinks or enlarges
 * client buffers to fit; the backends only know the mode.
 */
#define LM_SCALE_MIN 1000
#define LM_SCALE_MAX 4000
/* The mode a backend has set: stores it and recomputes width / height from the scale. */
void lm_output_set_mode(struct lm_output *out, int32_t w, int32_t h);
/* Changes the scale (milli: 1500 = 150 %); out of range or too big for the mode is clamped (the logical size stays at
 * least 640 x 360). Returns true when the logical size changed (the caller then announces it: output_changed). */
bool lm_output_set_scale(struct lm_output *out, int32_t milli);
/* The scale new outputs get (setting `scale`, [display] scale); lm_output_set_scale() is applied to the existing ones
 * by the caller. */
void lm_output_default_scale_set(int32_t milli);
int32_t lm_output_default_scale(void);
static inline double lm_output_scalef(const struct lm_output *o)
{
    return o->scale_milli >= LM_SCALE_MIN ? o->scale_milli / 1000.0 : 1.0;
}
/* The physical size = the framebuffer's. */
static inline int32_t lm_output_pw(const struct lm_output *o) { return o->mode_w > 0 ? o->mode_w : o->width; }
static inline int32_t lm_output_ph(const struct lm_output *o) { return o->mode_h > 0 ? o->mode_h : o->height; }

/* Output rectangle in global coordinates. */
static inline struct lm_rect lm_output_rect(const struct lm_output *o)
{
    return lm_rect_make(o->x, o->y, o->width, o->height);
}
/* Clamp a global point into the union of outputs (to the nearest output rect). */
void lm_output_clamp_point(struct lm_server *srv, double *x, double *y);

#endif
