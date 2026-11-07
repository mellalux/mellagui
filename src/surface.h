#ifndef MELLAGUI_SURFACE_H
#define MELLAGUI_SURFACE_H

/*
 * wl_compositor, wl_surface, wl_region, wl_subcompositor, wl_subsurface.
 * Owner: surface.c.
 *
 * ---- State model ------------------------------------------------------------
 * wl_surface requests accumulate in a private *pending* state (buffer,
 * damage, opaque/input region, scale, transform, frame callbacks). On
 * wl_surface.commit the pending state is applied to the *current* state,
 * which is the part visible here in struct lm_surface:
 *   - if a buffer is attached: lm_buffer_copy() into surface->tex, the buffer
 *     is released at once, surface->width/height = tex size / scale
 *     (transform already applied), has_content = true;
 *   - attached NULL buffer: content dropped, has_content = false (unmapped);
 *   - no attach since last commit: content unchanged.
 * Synchronized subsurfaces cache their pending state and apply it when the
 * parent commits (surface.c implements the full sync/desync rules).
 *
 * After applying, in this order, surface.c:
 *   1. calls role->commit(surface) (if a role is active)
 *   2. computes damage (union of requested damage, plus the old extents when
 *      size/position/content presence changed) and reports it through the
 *      ROOT surface's role->damage(root, rect-in-root-coordinates)
 *   3. emits surface->commit_signal (data = surface)
 *   4. if frame callbacks were queued, calls lm_render_request_frame().
 *
 * ---- Coordinates ------------------------------------------------------------
 * "Surface-local" coordinates have their origin at the surface's top-left
 * (after buffer_scale/transform). Absolute (global compositor space)
 * positions are supplied by callers of the tree functions as the origin of
 * the root surface; a subsurface origin = parent origin + subsurface position.
 *
 * ---- Roles ------------------------------------------------------------------
 * A wl_surface gets exactly one role for life (xdg toplevel, xdg popup,
 * subsurface, cursor, ...). A role is *active* iff surface->role_data != NULL;
 * hooks are only called while active. The role object being destroyed calls
 * lm_surface_clear_role(), which deactivates the role (the surface remembers
 * which role it had so a different one is rejected) and unmaps the content.
 */

#include <stdbool.h>
#include <stdint.h>
#include "list.h"
#include "server.h"
#include "shm.h"
#include "util.h"

struct lm_surface;
struct lm_output;
struct lm_subsurface; /* private to surface.c */

struct lm_surface_role {
    const char *name;               /* "xdg_toplevel", "xdg_popup", "wl_subsurface", "cursor" ... */
    /* The pending state was just applied. May map/unmap windows, etc. */
    void (*commit)(struct lm_surface *s);
    /*
     * Content of `s` (which is a ROOT of its surface tree, i.e. not a
     * subsurface) changed in `rect` (root-local coordinates, may extend
     * outside the root surface when subsurfaces/old extents are involved).
     * The role forwards it to the scene owner (xdg roles -> lm_wm_surface_damage;
     * cursor role -> lm_render_cursor_changed). NULL = ignore.
     */
    void (*damage)(struct lm_surface *s, const struct lm_rect *rect);
};

struct lm_surface {
    struct lm_server *server;
    struct lm_resource *resource;   /* the wl_surface; client = resource->client */
    struct lm_list link;            /* in compositor's surface list (private use) */

    /* current (committed) state: READ-ONLY outside surface.c */
    struct lm_texture tex;          /* valid iff has_content */
    bool has_content;               /* last commit attached a non-NULL buffer */
    /* the GL renderer's mirror of `tex` (render_gl.c owns these): the GL texture and what changed in `tex` since the last upload */
    unsigned gl_tex;
    int32_t gl_w, gl_h;
    bool gl_full;                   /* the whole texture must be uploaded (new content, new size) */
    struct lm_region gl_damage;     /* texture pixels that changed since the last upload (when !gl_full) */
    int32_t width, height;          /* surface-local size (tex size / scale); 0 if !has_content */
    int32_t scale;                  /* buffer_scale, >= 1 */
    uint32_t transform;             /* buffer_transform (already applied inside tex) */
    struct lm_region opaque;        /* opaque region, surface-local */
    struct lm_region input;         /* input region, surface-local; ignored if input_infinite */
    bool input_infinite;            /* no input region set: whole surface takes input */

    /* role (written through lm_surface_set_role / lm_surface_clear_role only) */
    const struct lm_surface_role *role;
    void *role_data;                /* non-NULL = role active */

    struct lm_subsurface *sub;      /* non-NULL iff this surface is a (live) subsurface */

    struct lm_signal destroy_signal; /* emitted with data = surface just before it is freed */
    struct lm_signal commit_signal;  /* emitted after a commit has been applied */

    void *priv;                     /* surface.c: pending state, callbacks, children */
};

/* ---- compositor ----------------------------------------------------------------- */

/* Advertises wl_compositor (v4) and wl_subcompositor (v1). */
struct lm_compositor *lm_compositor_create(struct lm_server *srv);
void lm_compositor_destroy(struct lm_compositor *comp);

/* wl_surface resource -> surface (resource->data), NULL if not a wl_surface. */
struct lm_surface *lm_surface_from_resource(struct lm_resource *res);

/* ---- roles ------------------------------------------------------------------------ */

/*
 * Activate `role` with `role_data` (must be non-NULL). Returns 0, or -1 if
 * the surface already has a different role, or the same role is still active.
 * It does NOT post errors: the caller posts the error of its own interface
 * (e.g. xdg_wm_base.role). Does not check for attached buffers.
 */
int lm_surface_set_role(struct lm_surface *s, const struct lm_surface_role *role,
                        void *role_data);
/* Deactivate the role (role_data = NULL) and call lm_surface_reset_content(). */
void lm_surface_clear_role(struct lm_surface *s);
/*
 * Drop committed AND pending buffer content (surface becomes unmapped,
 * width/height 0), reporting the old extents as damage. Keeps role, regions
 * reset to defaults, frame callbacks kept.
 */
void lm_surface_reset_content(struct lm_surface *s);
/* A buffer is attached in the pending state (used by xdg for the "unconfigured buffer" check). */
bool lm_surface_has_pending_buffer(const struct lm_surface *s);

/* ---- surface tree (subsurfaces) ---------------------------------------------------- */

/* Topmost ancestor: follow subsurface parents until a non-subsurface. */
struct lm_surface *lm_surface_root(struct lm_surface *s);
/*
 * Origin of `s` relative to the origin of its root (0,0 for a root).
 * Includes subsurface positions as currently applied.
 */
void lm_surface_offset_in_root(const struct lm_surface *s, int32_t *dx, int32_t *dy);

/*
 * Visit `root` and its subsurface descendants in painting order (bottom to
 * top), skipping surfaces without content. (x, y) is the absolute origin of
 * `root`; the callback receives the absolute origin of each visited surface.
 * Return false from the callback to stop. Returns false if stopped.
 */
typedef bool (*lm_surface_visit_fn)(struct lm_surface *s, int32_t x, int32_t y,
                                    void *data);
bool lm_surface_tree_visit(struct lm_surface *root, int32_t x, int32_t y,
                           lm_surface_visit_fn fn, void *data);

/*
 * Topmost surface of the tree under the absolute point (px, py), honouring
 * input regions. (x, y) = absolute origin of root. On hit returns the surface
 * and sets *sx, *sy to surface-local coordinates (may be NULL); else NULL.
 */
struct lm_surface *lm_surface_tree_pick(struct lm_surface *root, int32_t x, int32_t y,
                                        double px, double py, double *sx, double *sy);

/* Bounding box of all content in the tree, root-local coordinates. false if none. */
bool lm_surface_tree_extents(struct lm_surface *root, struct lm_rect *out);

/* True if `s` accepts input at surface-local (sx, sy). */
bool lm_surface_accepts_input(const struct lm_surface *s, double sx, double sy);

/* ---- frame callbacks / outputs ---------------------------------------------------------- */

/*
 * Fire (wl_callback.done(time_ms)) and destroy all frame callbacks that were
 * latched by commits, for the whole tree rooted at `root`. Called by the wm
 * (and seat, for the cursor surface) after a frame was presented.
 */
void lm_surface_send_frame_done(struct lm_surface *root, uint32_t time_ms);
/* Any latched frame callback pending in the tree? */
bool lm_surface_tree_wants_frame(struct lm_surface *root);

/*
 * Tell the surface tree which outputs it is currently visible on; surface.c
 * diffs against the previous set and sends wl_surface.enter/leave (for the
 * root and all descendants) using lm_output_resource_for_client().
 * Pass n = 0 to leave all outputs (e.g. minimized / other workspace).
 */
void lm_surface_set_outputs(struct lm_surface *root, struct lm_output *const *outs,
                            size_t n);

#endif
