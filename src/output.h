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
/* Output rectangle in global coordinates. */
static inline struct lm_rect lm_output_rect(const struct lm_output *o)
{
    return lm_rect_make(o->x, o->y, o->width, o->height);
}
/* Clamp a global point into the union of outputs (to the nearest output rect). */
void lm_output_clamp_point(struct lm_server *srv, double *x, double *y);

#endif
