/*
 * xdg-shell adapter: xdg_wm_base, xdg_positioner, xdg_surface, xdg_toplevel,
 * xdg_popup and zxdg_decoration_manager_v1. See xdg.h for the contract with
 * the window manager (wm.h).
 *
 * Object graph (all owned by the client's resource table):
 *
 *   wm_base ---(count)---> xdg_surface ---> xdg_toplevel ---> lm_window (wm)
 *                              |                 ^
 *                              |                 +--- zxdg_toplevel_decoration_v1
 *                              +-----------> xdg_popup ---> lm_popup (wm)
 *
 * Lifetime rules:
 *   - A resource destroy hook never touches another resource except through
 *     back pointers that the other side clears in its own hook.
 *   - The wl_surface may die before the xdg objects: a destroy_signal
 *     listener detaches the window/popup from the wm and NULLs xs->surface.
 *   - lm_xdg_destroy() runs before the clients are torn down (main.c order):
 *     it marks the module dead and forgets all wm/seat objects, because the
 *     wm has already freed its windows by then. The struct itself is freed
 *     once the last object that points to it is gone.
 *
 * Only xdg_wm_base v3 is advertised, so xdg_toplevel.configure_bounds (v4) and
 * xdg_toplevel.wm_capabilities (v5) do not exist for our clients.
 */

#include <stdlib.h>
#include <string.h>

#include "backend.h"
#include "log.h"
#include "protocol.h"
#include "seat.h"
#include "surface.h"
#include "util.h"
#include "wm.h"
#include "xdg.h"

#define XDG_MAX_PENDING_CONFIGURES 128
#define XDG_POPUP_MAX_DEPTH        32
#define XDG_STRING_MAX             255
#define XDG_SIZE_LIMIT             65536
#define XDG_POPUP_SIZE_LIMIT       16384

struct xdg_surface;
struct xdg_toplevel;
struct xdg_popup;

enum xdg_role {
    XDG_ROLE_NONE = 0,
    XDG_ROLE_TOPLEVEL,
    XDG_ROLE_POPUP,
};

struct wm_base {
    struct lm_xdg *xdg;
    struct lm_resource *res;
    struct lm_list link;                /* in xdg->bases */
    int n_surfaces;                     /* live xdg_surfaces created through this object */
};

struct positioner {
    int32_t w, h;                       /* 0 = not set */
    struct lm_rect anchor_rect;
    bool anchor_rect_set;
    uint32_t anchor;
    uint32_t gravity;
    uint32_t constraint;
    int32_t off_x, off_y;
};

struct xdg_surface {
    struct lm_xdg *xdg;
    struct lm_resource *res;
    struct wm_base *base;
    struct lm_surface *surface;         /* NULL once the wl_surface is gone */
    struct lm_listener destroy_l;       /* surface->destroy_signal */
    struct lm_listener commit_l;        /* surface->commit_signal */

    enum xdg_role role;
    struct xdg_toplevel *toplevel;
    struct xdg_popup *popup;

    /* configure handshake */
    bool initial_sent;                  /* an initial configure was sent (after the first commit) */
    bool configured;                    /* a configure was acked and applied by a commit */
    bool acked_valid;                   /* ack_configure received, not yet applied by a commit */
    bool mapped;
    uint32_t serials[XDG_MAX_PENDING_CONFIGURES]; /* sent, not yet acked, oldest first */
    int n_serials;
    bool has_dropped;
    uint32_t dropped_serial;            /* newest serial forgotten because the queue was full */
    uint32_t n_sent;                    /* total configure events sent (for fallback detection) */

    /* window geometry */
    bool pend_geom_dirty;
    struct lm_rect pend_geom;           /* from set_window_geometry, applied on commit */
    bool has_geom;
    struct lm_rect req_geom;
    struct lm_rect geom;                /* effective geometry last applied */
    bool geom_valid;

    struct lm_list popups;              /* child struct xdg_popup.sibling */
};

struct xdg_decoration;

struct xdg_toplevel {
    struct lm_xdg *xdg;
    struct lm_resource *res;
    struct xdg_surface *xs;             /* NULL once the xdg_surface is gone */
    struct lm_window *window;
    struct lm_list link;                /* in xdg->toplevels */

    struct xdg_toplevel *parent;
    struct lm_listener parent_destroy;  /* parent->res->destroy_signal */
    struct xdg_decoration *deco;

    /* double-buffered size limits */
    int32_t pend_min_w, pend_min_h, pend_max_w, pend_max_h;
    int32_t min_w, min_h, max_w, max_h;

    /* last configure sent (re-sent when the decoration mode is renegotiated) */
    int32_t cfg_w, cfg_h;
    uint32_t cfg_states;
};

struct xdg_popup {
    struct lm_xdg *xdg;
    struct lm_resource *res;
    struct xdg_surface *xs;             /* the popup's own xdg_surface */
    struct xdg_surface *parent;         /* NULL if the parent xdg_surface died */
    struct lm_popup *popup;
    struct lm_list link;                /* in xdg->popups */
    struct lm_list sibling;             /* in parent->popups */
    struct lm_list grab_link;           /* in xdg->grab_popups while grabbing */
    bool grabbing;
    bool dismissed;
    int depth;
    struct positioner pos;
    int32_t x, y, w, h;                 /* last configured rect, relative to the parent's window geometry */
};

struct xdg_decoration {
    struct lm_resource *res;
    struct xdg_toplevel *xt;            /* NULL once the toplevel is destroyed */
};

struct lm_xdg {
    struct lm_server *srv;
    struct lm_global *wm_base_global;
    struct lm_global *deco_global;
    struct lm_list bases;
    struct lm_list toplevels;
    struct lm_list popups;
    int refs;                           /* 1 (module) + wm_bases + toplevels + popups */
    bool dead;

    /* popup grab (one seat) */
    struct lm_pointer_grab grab;
    bool grab_installed;
    struct lm_list grab_popups;         /* bottom .. top */
};

static const struct lm_window_ops toplevel_window_ops;
static void popup_dismiss(struct xdg_popup *xp);
static void xs_dismiss_children(struct xdg_surface *xs);

/* ---- helpers ------------------------------------------------------------------------ */

static struct lm_wm *xdg_wm(const struct lm_xdg *xdg)
{
    return xdg->dead ? NULL : xdg->srv->wm;
}

static struct lm_seat *xdg_seat(const struct lm_xdg *xdg)
{
    return xdg->dead ? NULL : xdg->srv->seat;
}

static void xdg_unref(struct lm_xdg *xdg)
{
    if (--xdg->refs == 0)
        free(xdg);
}

static int32_t clamp_i32(int64_t v, int64_t lo, int64_t hi)
{
    return (int32_t)LM_CLAMP(v, lo, hi);
}

static int32_t clamp_coord(int64_t v)
{
    return clamp_i32(v, -(int64_t)LM_COORD_LIMIT, (int64_t)LM_COORD_LIMIT);
}

/* Copy `src` into dst (cap bytes incl. NUL), cut at a UTF-8 boundary, controls -> space. */
static void clean_string(char *dst, size_t cap, const char *src)
{
    size_t n = strnlen(src, cap - 1);
    if (src[n] != '\0') {
        while (n > 0 && ((unsigned char)src[n] & 0xc0) == 0x80)
            n--;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)src[i];
        dst[i] = (c < 0x20 || c == 0x7f) ? ' ' : (char)c;
    }
    dst[n] = '\0';
}

static struct xdg_surface *xs_from_surface(struct lm_surface *s)
{
    if (!s || !s->role_data)
        return NULL;
    if (s->role != &lm_xdg_toplevel_role && s->role != &lm_xdg_popup_role)
        return NULL;
    return s->role_data;
}

static bool res_dying(const struct lm_resource *res)
{
    return res->client->dying;
}

/* ---- configure serial queue ----------------------------------------------------------- */

/* Sends xdg_surface.configure with a fresh serial and remembers it for ack_configure. */
static void xs_send_configure(struct xdg_surface *xs)
{
    uint32_t serial = lm_server_next_serial(xs->xdg->srv);
    if (xs->n_serials == XDG_MAX_PENDING_CONFIGURES) {
        xs->has_dropped = true;
        xs->dropped_serial = xs->serials[0];
        memmove(&xs->serials[0], &xs->serials[1],
                sizeof(xs->serials[0]) * (XDG_MAX_PENDING_CONFIGURES - 1));
        xs->n_serials--;
    }
    xs->serials[xs->n_serials++] = serial;
    xs->n_sent++;
    lm_resource_post(xs->res, LM_XDG_SURFACE_EVT_CONFIGURE, serial);
}

/* Back to the "freshly created" configure state (after unmap / role destruction). */
static void xs_reset_configure_state(struct xdg_surface *xs)
{
    xs->initial_sent = false;
    xs->configured = false;
    xs->acked_valid = false;
    xs->mapped = false;
    xs->n_serials = 0;
    xs->has_dropped = false;
    xs->pend_geom_dirty = false;
    xs->has_geom = false;
    xs->geom_valid = false;
}

/* Window geometry actually used: set_window_geometry clipped to the surface tree, else the tree. */
static void xs_effective_geometry(struct xdg_surface *xs, struct lm_rect *out)
{
    struct lm_surface *s = xs->surface;
    struct lm_rect ext;
    if (!lm_surface_tree_extents(s, &ext))
        ext = lm_rect_make(0, 0, s->width, s->height);
    if (xs->has_geom && lm_rect_intersect(&xs->req_geom, &ext, out))
        return;
    *out = ext;
}

/* Applies the pending window geometry and recomputes xs->geom. Returns true if it changed. */
static bool xs_update_geometry(struct xdg_surface *xs)
{
    if (xs->pend_geom_dirty) {
        xs->req_geom = xs->pend_geom;
        xs->has_geom = true;
        xs->pend_geom_dirty = false;
    }
    struct lm_rect g;
    xs_effective_geometry(xs, &g);
    if (xs->geom_valid && lm_rect_equal(&g, &xs->geom))
        return false;
    xs->geom = g;
    xs->geom_valid = true;
    return true;
}

/* ---- xdg_positioner ----------------------------------------------------------------- */

static bool anc_left(uint32_t a)
{
    return a == LM_XDG_ANCHOR_LEFT || a == LM_XDG_ANCHOR_TOP_LEFT ||
           a == LM_XDG_ANCHOR_BOTTOM_LEFT;
}

static bool anc_right(uint32_t a)
{
    return a == LM_XDG_ANCHOR_RIGHT || a == LM_XDG_ANCHOR_TOP_RIGHT ||
           a == LM_XDG_ANCHOR_BOTTOM_RIGHT;
}

static bool anc_top(uint32_t a)
{
    return a == LM_XDG_ANCHOR_TOP || a == LM_XDG_ANCHOR_TOP_LEFT ||
           a == LM_XDG_ANCHOR_TOP_RIGHT;
}

static bool anc_bottom(uint32_t a)
{
    return a == LM_XDG_ANCHOR_BOTTOM || a == LM_XDG_ANCHOR_BOTTOM_LEFT ||
           a == LM_XDG_ANCHOR_BOTTOM_RIGHT;
}

static uint32_t anc_flip_h(uint32_t a)
{
    switch (a) {
    case LM_XDG_ANCHOR_LEFT: return LM_XDG_ANCHOR_RIGHT;
    case LM_XDG_ANCHOR_RIGHT: return LM_XDG_ANCHOR_LEFT;
    case LM_XDG_ANCHOR_TOP_LEFT: return LM_XDG_ANCHOR_TOP_RIGHT;
    case LM_XDG_ANCHOR_TOP_RIGHT: return LM_XDG_ANCHOR_TOP_LEFT;
    case LM_XDG_ANCHOR_BOTTOM_LEFT: return LM_XDG_ANCHOR_BOTTOM_RIGHT;
    case LM_XDG_ANCHOR_BOTTOM_RIGHT: return LM_XDG_ANCHOR_BOTTOM_LEFT;
    default: return a;
    }
}

static uint32_t anc_flip_v(uint32_t a)
{
    switch (a) {
    case LM_XDG_ANCHOR_TOP: return LM_XDG_ANCHOR_BOTTOM;
    case LM_XDG_ANCHOR_BOTTOM: return LM_XDG_ANCHOR_TOP;
    case LM_XDG_ANCHOR_TOP_LEFT: return LM_XDG_ANCHOR_BOTTOM_LEFT;
    case LM_XDG_ANCHOR_BOTTOM_LEFT: return LM_XDG_ANCHOR_TOP_LEFT;
    case LM_XDG_ANCHOR_TOP_RIGHT: return LM_XDG_ANCHOR_BOTTOM_RIGHT;
    case LM_XDG_ANCHOR_BOTTOM_RIGHT: return LM_XDG_ANCHOR_TOP_RIGHT;
    default: return a;
    }
}

/* One axis: anchor point on the anchor rect (+ offset), then gravity places the popup edge. */
static int64_t place_axis(int64_t a0, int64_t alen, bool anchor_lo, bool anchor_hi,
                          bool grav_lo, bool grav_hi, int64_t off, int64_t size)
{
    int64_t p = anchor_lo ? a0 : anchor_hi ? a0 + alen : a0 + alen / 2;
    p += off;
    return grav_lo ? p - size : grav_hi ? p : p - size / 2;
}

static bool axis_constrained(int64_t lo, int64_t size, int64_t b_lo, int64_t b_hi)
{
    return lo < b_lo || lo + size > b_hi;
}

static void axis_slide(int64_t *lo, int64_t size, int64_t b_lo, int64_t b_hi)
{
    if (*lo + size > b_hi)
        *lo = b_hi - size;
    if (*lo < b_lo)
        *lo = b_lo;
}

static void axis_resize(int64_t *lo, int64_t *size, int64_t b_lo, int64_t b_hi)
{
    int64_t n_lo = LM_MAX(*lo, b_lo);
    int64_t n_hi = LM_MIN(*lo + *size, b_hi);
    if (n_hi > n_lo) {
        *lo = n_lo;
        *size = n_hi - n_lo;
    }
}

/*
 * Popup rectangle relative to the parent's window geometry. `bounds` (same
 * coordinate space, may be NULL) is the area the popup should stay inside.
 * Adjustment order per axis: flip, slide, resize (each only while the popup is
 * still constrained on that axis).
 */
static void positioner_compute(const struct positioner *p, const struct lm_rect *bounds,
                               struct lm_rect *out)
{
    const struct lm_rect *ar = &p->anchor_rect;
    int64_t w = p->w, h = p->h;
    int64_t x = place_axis(ar->x, ar->w, anc_left(p->anchor), anc_right(p->anchor),
                           anc_left(p->gravity), anc_right(p->gravity), p->off_x, w);
    int64_t y = place_axis(ar->y, ar->h, anc_top(p->anchor), anc_bottom(p->anchor),
                           anc_top(p->gravity), anc_bottom(p->gravity), p->off_y, h);

    if (bounds && !lm_rect_is_empty(bounds) && (p->constraint & 63u)) {
        int64_t bx1 = bounds->x, bx2 = bx1 + bounds->w;
        int64_t by1 = bounds->y, by2 = by1 + bounds->h;

        if (axis_constrained(x, w, bx1, bx2)) {
            if (p->constraint & LM_XDG_CA_FLIP_X) {
                uint32_t fa = anc_flip_h(p->anchor), fg = anc_flip_h(p->gravity);
                int64_t fx = place_axis(ar->x, ar->w, anc_left(fa), anc_right(fa),
                                        anc_left(fg), anc_right(fg), p->off_x, w);
                if (!axis_constrained(fx, w, bx1, bx2))
                    x = fx;
            }
            if ((p->constraint & LM_XDG_CA_SLIDE_X) && axis_constrained(x, w, bx1, bx2))
                axis_slide(&x, w, bx1, bx2);
            if ((p->constraint & LM_XDG_CA_RESIZE_X) && axis_constrained(x, w, bx1, bx2))
                axis_resize(&x, &w, bx1, bx2);
        }
        if (axis_constrained(y, h, by1, by2)) {
            if (p->constraint & LM_XDG_CA_FLIP_Y) {
                uint32_t fa = anc_flip_v(p->anchor), fg = anc_flip_v(p->gravity);
                int64_t fy = place_axis(ar->y, ar->h, anc_top(fa), anc_bottom(fa),
                                        anc_top(fg), anc_bottom(fg), p->off_y, h);
                if (!axis_constrained(fy, h, by1, by2))
                    y = fy;
            }
            if ((p->constraint & LM_XDG_CA_SLIDE_Y) && axis_constrained(y, h, by1, by2))
                axis_slide(&y, h, by1, by2);
            if ((p->constraint & LM_XDG_CA_RESIZE_Y) && axis_constrained(y, h, by1, by2))
                axis_resize(&y, &h, by1, by2);
        }
    }

    out->x = clamp_coord(x);
    out->y = clamp_coord(y);
    out->w = clamp_i32(w, 1, XDG_POPUP_SIZE_LIMIT);
    out->h = clamp_i32(h, 1, XDG_POPUP_SIZE_LIMIT);
}

static bool positioner_complete(const struct positioner *p)
{
    return p->w > 0 && p->h > 0 && p->anchor_rect_set;
}

static void positioner_req_destroy(struct lm_resource *res, const union lm_arg *args)
{
    (void)args;
    lm_resource_destroy(res);
}

static void positioner_req_set_size(struct lm_resource *res, const union lm_arg *args)
{
    struct positioner *p = res->data;
    if (args[0].i <= 0 || args[1].i <= 0) {
        lm_resource_post_error(res, LM_XDG_POSITIONER_ERR_INVALID_INPUT,
                               "xdg_positioner size must be positive (%d x %d)",
                               args[0].i, args[1].i);
        return;
    }
    p->w = clamp_i32(args[0].i, 1, XDG_POPUP_SIZE_LIMIT);
    p->h = clamp_i32(args[1].i, 1, XDG_POPUP_SIZE_LIMIT);
}

static void positioner_req_set_anchor_rect(struct lm_resource *res, const union lm_arg *args)
{
    struct positioner *p = res->data;
    if (args[2].i < 0 || args[3].i < 0) {
        lm_resource_post_error(res, LM_XDG_POSITIONER_ERR_INVALID_INPUT,
                               "xdg_positioner anchor rect has a negative size");
        return;
    }
    p->anchor_rect = lm_rect_make(clamp_coord(args[0].i), clamp_coord(args[1].i),
                                  clamp_coord(args[2].i), clamp_coord(args[3].i));
    p->anchor_rect_set = true;
}

static void positioner_req_set_anchor(struct lm_resource *res, const union lm_arg *args)
{
    struct positioner *p = res->data;
    if (args[0].u > LM_XDG_ANCHOR_BOTTOM_RIGHT) {
        lm_resource_post_error(res, LM_XDG_POSITIONER_ERR_INVALID_INPUT,
                               "invalid xdg_positioner anchor %u", args[0].u);
        return;
    }
    p->anchor = args[0].u;
}

static void positioner_req_set_gravity(struct lm_resource *res, const union lm_arg *args)
{
    struct positioner *p = res->data;
    if (args[0].u > LM_XDG_ANCHOR_BOTTOM_RIGHT) {
        lm_resource_post_error(res, LM_XDG_POSITIONER_ERR_INVALID_INPUT,
                               "invalid xdg_positioner gravity %u", args[0].u);
        return;
    }
    p->gravity = args[0].u;
}

static void positioner_req_set_constraint_adjustment(struct lm_resource *res,
                                                     const union lm_arg *args)
{
    struct positioner *p = res->data;
    p->constraint = args[0].u & 63u;
}

static void positioner_req_set_offset(struct lm_resource *res, const union lm_arg *args)
{
    struct positioner *p = res->data;
    p->off_x = clamp_i32(args[0].i, -(LM_COORD_LIMIT / 2), LM_COORD_LIMIT / 2);
    p->off_y = clamp_i32(args[1].i, -(LM_COORD_LIMIT / 2), LM_COORD_LIMIT / 2);
}

/* reactive and parent_configure are accepted and ignored: we never reposition popups on our own. */
static void positioner_req_set_reactive(struct lm_resource *res, const union lm_arg *args)
{
    (void)res;
    (void)args;
}

static void positioner_req_set_parent_size(struct lm_resource *res, const union lm_arg *args)
{
    if (args[0].i < 0 || args[1].i < 0) {
        lm_resource_post_error(res, LM_XDG_POSITIONER_ERR_INVALID_INPUT,
                               "xdg_positioner parent size must not be negative");
    }
}

static void positioner_req_set_parent_configure(struct lm_resource *res, const union lm_arg *args)
{
    (void)res;
    (void)args;
}

static void positioner_destroy_hook(struct lm_resource *res)
{
    free(res->data);
    res->data = NULL;
}

static const lm_request_fn positioner_handlers[] = {
    [LM_XDG_POSITIONER_REQ_DESTROY] = positioner_req_destroy,
    [LM_XDG_POSITIONER_REQ_SET_SIZE] = positioner_req_set_size,
    [LM_XDG_POSITIONER_REQ_SET_ANCHOR_RECT] = positioner_req_set_anchor_rect,
    [LM_XDG_POSITIONER_REQ_SET_ANCHOR] = positioner_req_set_anchor,
    [LM_XDG_POSITIONER_REQ_SET_GRAVITY] = positioner_req_set_gravity,
    [LM_XDG_POSITIONER_REQ_SET_CONSTRAINT_ADJUSTMENT] = positioner_req_set_constraint_adjustment,
    [LM_XDG_POSITIONER_REQ_SET_OFFSET] = positioner_req_set_offset,
    [LM_XDG_POSITIONER_REQ_SET_REACTIVE] = positioner_req_set_reactive,
    [LM_XDG_POSITIONER_REQ_SET_PARENT_SIZE] = positioner_req_set_parent_size,
    [LM_XDG_POSITIONER_REQ_SET_PARENT_CONFIGURE] = positioner_req_set_parent_configure,
};

/* ---- xdg_wm_base: pong ---------------------------------------------------------------------- */

/* The compositor never sends xdg_wm_base.ping, so a pong carries no information. */
static void wm_base_req_pong(struct lm_resource *res, const union lm_arg *args)
{
    (void)res;
    (void)args;
}

/* ---- xdg_surface -------------------------------------------------------------------------- */

static void xdg_role_commit(struct lm_surface *s);
static void xdg_role_damage(struct lm_surface *s, const struct lm_rect *rect);

const struct lm_surface_role lm_xdg_toplevel_role = {
    .name = "xdg_toplevel",
    .commit = xdg_role_commit,
    .damage = xdg_role_damage,
};

const struct lm_surface_role lm_xdg_popup_role = {
    .name = "xdg_popup",
    .commit = xdg_role_commit,
    .damage = xdg_role_damage,
};

static void xdg_role_damage(struct lm_surface *s, const struct lm_rect *rect)
{
    struct xdg_surface *xs = xs_from_surface(s);
    struct lm_wm *wm = xs ? xdg_wm(xs->xdg) : NULL;
    if (wm)
        lm_wm_surface_damage(wm, s, rect);
}

static void xs_commit_notify(struct lm_listener *l, void *data)
{
    struct xdg_surface *xs = lm_container_of(l, struct xdg_surface, commit_l);
    struct lm_surface *s = data;
    if (xs->role == XDG_ROLE_NONE && s->has_content) {
        lm_resource_post_error(xs->res, LM_XDG_SURFACE_ERR_NOT_CONSTRUCTED,
                               "wl_surface committed with a buffer but the xdg_surface has no role");
    }
}

static void toplevel_wm_detach(struct xdg_toplevel *xt);
static void popup_wm_detach(struct xdg_popup *xp);

static void xs_surface_destroyed(struct lm_listener *l, void *data)
{
    struct xdg_surface *xs = lm_container_of(l, struct xdg_surface, destroy_l);
    (void)data;
    lm_listener_remove(&xs->commit_l);
    if (xs->toplevel)
        toplevel_wm_detach(xs->toplevel);
    if (xs->popup)
        popup_wm_detach(xs->popup);
    xs_dismiss_children(xs);
    xs->surface = NULL;
    xs->mapped = false;
}

static void xs_req_destroy(struct lm_resource *res, const union lm_arg *args)
{
    struct xdg_surface *xs = res->data;
    (void)args;
    if (xs->toplevel || xs->popup) {
        lm_resource_post_error(res, LM_XDG_SURFACE_ERR_DEFUNCT_ROLE_OBJECT,
                               "xdg_surface destroyed before its role object");
        return;
    }
    lm_resource_destroy(res);
}

static void toplevel_destroy_hook(struct lm_resource *res);
static void popup_destroy_hook(struct lm_resource *res);
static const lm_request_fn toplevel_handlers[LM_XDG_TOPLEVEL_REQ_SET_MINIMIZED + 1];
static const lm_request_fn popup_handlers[LM_XDG_POPUP_REQ_REPOSITION + 1];

static void xs_req_get_toplevel(struct lm_resource *res, const union lm_arg *args)
{
    struct xdg_surface *xs = res->data;
    struct lm_xdg *xdg = xs->xdg;

    if (xs->role != XDG_ROLE_NONE) {
        lm_resource_post_error(res, LM_XDG_SURFACE_ERR_ALREADY_CONSTRUCTED,
                               "xdg_surface already has a role object");
        return;
    }
    if (!xs->surface) {
        lm_resource_post_error(res, LM_XDG_SURFACE_ERR_NOT_CONSTRUCTED,
                               "the wl_surface of this xdg_surface was destroyed");
        return;
    }
    if (!xdg_wm(xdg)) {
        lm_client_post_error(res->client, res->id, LM_WL_DISPLAY_ERR_IMPLEMENTATION,
                             "window manager is not available");
        return;
    }
    struct xdg_toplevel *xt = calloc(1, sizeof(*xt));
    if (!xt) {
        lm_resource_post_no_memory(res);
        return;
    }
    xt->xdg = xdg;
    xt->xs = xs;
    lm_list_init(&xt->link);
    lm_list_init(&xt->parent_destroy.link);
    xt->res = lm_resource_create(res->client, args[0].n, &lm_xdg_toplevel_interface,
                                 res->version, toplevel_handlers, xt, toplevel_destroy_hook);
    if (!xt->res) {
        free(xt);
        return;
    }
    xdg->refs++;
    lm_list_append(&xdg->toplevels, &xt->link);

    if (lm_surface_set_role(xs->surface, &lm_xdg_toplevel_role, xs) < 0) {
        lm_resource_post_error(xs->base->res, LM_XDG_WM_BASE_ERR_ROLE,
                               "wl_surface already has a different role");
        lm_resource_destroy(xt->res);
        return;
    }
    xs->role = XDG_ROLE_TOPLEVEL;
    xs->toplevel = xt;
    xt->window = lm_wm_window_create(xdg_wm(xdg), xs->surface, &toplevel_window_ops, xt);
    if (!xt->window) {
        lm_resource_post_no_memory(xt->res);
        return;
    }
    if (!xt->window->shell_data)
        xt->window->shell_data = xt;
}

static void xs_req_get_popup(struct lm_resource *res, const union lm_arg *args)
{
    struct xdg_surface *xs = res->data;
    struct lm_xdg *xdg = xs->xdg;
    struct xdg_surface *pxs = args[1].r ? args[1].r->data : NULL;
    const struct positioner *pos = args[2].r->data;

    if (xs->role != XDG_ROLE_NONE) {
        lm_resource_post_error(res, LM_XDG_SURFACE_ERR_ALREADY_CONSTRUCTED,
                               "xdg_surface already has a role object");
        return;
    }
    if (!xs->surface) {
        lm_resource_post_error(res, LM_XDG_SURFACE_ERR_NOT_CONSTRUCTED,
                               "the wl_surface of this xdg_surface was destroyed");
        return;
    }
    if (!pxs || pxs == xs || pxs->role == XDG_ROLE_NONE || !pxs->surface) {
        lm_resource_post_error(xs->base->res, LM_XDG_WM_BASE_ERR_INVALID_POPUP_PARENT,
                               "xdg_popup needs a parent xdg_surface that has a role");
        return;
    }
    if (!positioner_complete(pos)) {
        lm_resource_post_error(xs->base->res, LM_XDG_WM_BASE_ERR_INVALID_POSITIONER,
                               "xdg_positioner is incomplete (size and anchor rect are required)");
        return;
    }
    int depth = pxs->role == XDG_ROLE_POPUP && pxs->popup ? pxs->popup->depth + 1 : 1;
    if (depth > XDG_POPUP_MAX_DEPTH) {
        lm_resource_post_error(xs->base->res, LM_XDG_WM_BASE_ERR_INVALID_POPUP_PARENT,
                               "xdg_popup nesting too deep");
        return;
    }
    if (!xdg_wm(xdg)) {
        lm_client_post_error(res->client, res->id, LM_WL_DISPLAY_ERR_IMPLEMENTATION,
                             "window manager is not available");
        return;
    }
    struct xdg_popup *xp = calloc(1, sizeof(*xp));
    if (!xp) {
        lm_resource_post_no_memory(res);
        return;
    }
    xp->xdg = xdg;
    xp->xs = xs;
    xp->parent = pxs;
    xp->depth = depth;
    xp->pos = *pos;
    lm_list_init(&xp->link);
    lm_list_init(&xp->sibling);
    lm_list_init(&xp->grab_link);
    xp->res = lm_resource_create(res->client, args[0].n, &lm_xdg_popup_interface, res->version,
                                 popup_handlers, xp, popup_destroy_hook);
    if (!xp->res) {
        free(xp);
        return;
    }
    xdg->refs++;
    lm_list_append(&xdg->popups, &xp->link);
    lm_list_append(&pxs->popups, &xp->sibling);

    if (lm_surface_set_role(xs->surface, &lm_xdg_popup_role, xs) < 0) {
        lm_resource_post_error(xs->base->res, LM_XDG_WM_BASE_ERR_ROLE,
                               "wl_surface already has a different role");
        lm_resource_destroy(xp->res);
        return;
    }
    xs->role = XDG_ROLE_POPUP;
    xs->popup = xp;
    xp->popup = lm_wm_popup_create(xdg_wm(xdg), xs->surface, pxs->surface);
    if (!xp->popup)
        lm_resource_post_no_memory(xp->res);
}

static void xs_req_set_window_geometry(struct lm_resource *res, const union lm_arg *args)
{
    struct xdg_surface *xs = res->data;
    if (xs->role == XDG_ROLE_NONE) {
        lm_resource_post_error(res, LM_XDG_SURFACE_ERR_NOT_CONSTRUCTED,
                               "set_window_geometry on an xdg_surface without a role");
        return;
    }
    if (args[2].i <= 0 || args[3].i <= 0) {
        lm_resource_post_error(res, LM_XDG_SURFACE_ERR_INVALID_SIZE,
                               "window geometry size must be positive (%d x %d)",
                               args[2].i, args[3].i);
        return;
    }
    struct lm_rect g = lm_rect_make(args[0].i, args[1].i, args[2].i, args[3].i);
    if (!lm_rect_sanitize(&g)) {
        lm_resource_post_error(res, LM_XDG_SURFACE_ERR_INVALID_SIZE, "window geometry out of range");
        return;
    }
    xs->pend_geom = g;
    xs->pend_geom_dirty = true;
}

static void xs_req_ack_configure(struct lm_resource *res, const union lm_arg *args)
{
    struct xdg_surface *xs = res->data;
    uint32_t serial = args[0].u;
    if (xs->role == XDG_ROLE_NONE) {
        lm_resource_post_error(res, LM_XDG_SURFACE_ERR_NOT_CONSTRUCTED,
                               "ack_configure on an xdg_surface without a role");
        return;
    }
    for (int i = 0; i < xs->n_serials; i++) {
        if (xs->serials[i] != serial)
            continue;
        /* acking a serial implicitly retires every older configure */
        memmove(&xs->serials[0], &xs->serials[i + 1],
                sizeof(xs->serials[0]) * (size_t)(xs->n_serials - i - 1));
        xs->n_serials -= i + 1;
        xs->acked_valid = true;
        return;
    }
    if (xs->has_dropped && (int32_t)(serial - xs->dropped_serial) <= 0)
        return; /* stale ack for a configure we stopped tracking */
    lm_resource_post_error(res, LM_XDG_SURFACE_ERR_INVALID_SERIAL,
                           "ack_configure with unknown serial %u", serial);
}

static const lm_request_fn xdg_surface_handlers[] = {
    [LM_XDG_SURFACE_REQ_DESTROY] = xs_req_destroy,
    [LM_XDG_SURFACE_REQ_GET_TOPLEVEL] = xs_req_get_toplevel,
    [LM_XDG_SURFACE_REQ_GET_POPUP] = xs_req_get_popup,
    [LM_XDG_SURFACE_REQ_SET_WINDOW_GEOMETRY] = xs_req_set_window_geometry,
    [LM_XDG_SURFACE_REQ_ACK_CONFIGURE] = xs_req_ack_configure,
};

static void xs_destroy_hook(struct lm_resource *res)
{
    struct xdg_surface *xs = res->data;
    if (!xs)
        return;
    res->data = NULL;

    /* Role objects normally die first; this handles the defensive cases. */
    if (xs->toplevel) {
        toplevel_wm_detach(xs->toplevel);
        xs->toplevel->xs = NULL;
        xs->toplevel = NULL;
    }
    if (xs->popup) {
        popup_wm_detach(xs->popup);
        xs->popup->xs = NULL;
        xs->popup = NULL;
    }
    /* Orphan popups that still reference us as their parent. */
    struct xdg_popup *xp, *tmp;
    lm_list_for_each_safe(xp, tmp, &xs->popups, sibling) {
        popup_dismiss(xp);
        xp->parent = NULL;
        lm_list_remove(&xp->sibling);
    }
    if (xs->surface) {
        lm_listener_remove(&xs->destroy_l);
        lm_listener_remove(&xs->commit_l);
        if (xs->role != XDG_ROLE_NONE && xs->surface->role_data == xs)
            lm_surface_clear_role(xs->surface);
    }
    if (xs->base) {
        xs->base->n_surfaces--;
    }
    free(xs);
}

/* ---- wm_base requests ------------------------------------------------------------------------ */

static void wm_base_req_destroy(struct lm_resource *res, const union lm_arg *args)
{
    struct wm_base *b = res->data;
    (void)args;
    if (b->n_surfaces > 0) {
        lm_resource_post_error(res, LM_XDG_WM_BASE_ERR_DEFUNCT_SURFACES,
                               "xdg_wm_base destroyed before its xdg_surfaces");
        return;
    }
    lm_resource_destroy(res);
}

static void wm_base_req_create_positioner(struct lm_resource *res, const union lm_arg *args)
{
    struct positioner *p = calloc(1, sizeof(*p));
    if (!p) {
        lm_resource_post_no_memory(res);
        return;
    }
    p->anchor = LM_XDG_ANCHOR_NONE;
    p->gravity = LM_XDG_ANCHOR_NONE;
    if (!lm_resource_create(res->client, args[0].n, &lm_xdg_positioner_interface, res->version,
                            positioner_handlers, p, positioner_destroy_hook))
        free(p);
}

static void wm_base_req_get_xdg_surface(struct lm_resource *res, const union lm_arg *args)
{
    struct wm_base *b = res->data;
    struct lm_surface *s = lm_surface_from_resource(args[1].r);
    if (!s) {
        lm_resource_post_error(res, LM_XDG_WM_BASE_ERR_INVALID_SURFACE_STATE,
                               "get_xdg_surface needs a live wl_surface");
        return;
    }
    if (s->role && s->role != &lm_xdg_toplevel_role && s->role != &lm_xdg_popup_role) {
        lm_resource_post_error(res, LM_XDG_WM_BASE_ERR_ROLE,
                               "wl_surface already has a non-xdg role (%s)", s->role->name);
        return;
    }
    struct lm_listener *l;
    lm_list_for_each(l, &s->commit_signal.listeners, link) {
        if (l->notify == xs_commit_notify) {
            lm_resource_post_error(res, LM_XDG_WM_BASE_ERR_ROLE,
                                   "an xdg_surface already exists for this wl_surface");
            return;
        }
    }

    struct xdg_surface *xs = calloc(1, sizeof(*xs));
    if (!xs) {
        lm_resource_post_no_memory(res);
        return;
    }
    xs->xdg = b->xdg;
    xs->base = b;
    xs->surface = s;
    xs->role = XDG_ROLE_NONE;
    lm_list_init(&xs->popups);
    lm_list_init(&xs->destroy_l.link);
    lm_list_init(&xs->commit_l.link);
    xs->destroy_l.notify = xs_surface_destroyed;
    xs->commit_l.notify = xs_commit_notify;
    xs->res = lm_resource_create(res->client, args[0].n, &lm_xdg_surface_interface, res->version,
                                 xdg_surface_handlers, xs, xs_destroy_hook);
    if (!xs->res) {
        free(xs);
        return;
    }
    b->n_surfaces++;
    lm_signal_add(&s->destroy_signal, &xs->destroy_l);
    lm_signal_add(&s->commit_signal, &xs->commit_l);

    if (s->has_content || lm_surface_has_pending_buffer(s)) {
        lm_resource_post_error(xs->res, LM_XDG_SURFACE_ERR_UNCONFIGURED_BUFFER,
                               "xdg_surface created for a wl_surface that already has a buffer");
    }
}

static void wm_base_destroy_hook(struct lm_resource *res)
{
    struct wm_base *b = res->data;
    if (!b)
        return;
    res->data = NULL;
    b->res = NULL;
    lm_list_remove(&b->link);
    struct lm_xdg *xdg = b->xdg;
    free(b);
    xdg_unref(xdg);
}

static const lm_request_fn wm_base_handlers[] = {
    [LM_XDG_WM_BASE_REQ_DESTROY] = wm_base_req_destroy,
    [LM_XDG_WM_BASE_REQ_CREATE_POSITIONER] = wm_base_req_create_positioner,
    [LM_XDG_WM_BASE_REQ_GET_XDG_SURFACE] = wm_base_req_get_xdg_surface,
    [LM_XDG_WM_BASE_REQ_PONG] = wm_base_req_pong,
};

static void wm_base_bind(struct lm_client *client, void *data, uint32_t version, uint32_t id)
{
    struct lm_xdg *xdg = data;
    struct wm_base *b = calloc(1, sizeof(*b));
    if (!b) {
        lm_client_post_no_memory(client);
        return;
    }
    b->xdg = xdg;
    lm_list_init(&b->link);
    b->res = lm_resource_create(client, id, &lm_xdg_wm_base_interface,
                                LM_MIN(version, (uint32_t)LM_XDG_WM_BASE_VERSION),
                                wm_base_handlers, b, wm_base_destroy_hook);
    if (!b->res) {
        free(b);
        return;
    }
    xdg->refs++;
    lm_list_append(&xdg->bases, &b->link);
}

/* ---- popup grab ------------------------------------------------------------------------------ */

static struct xdg_popup *grab_top(struct lm_xdg *xdg)
{
    if (lm_list_empty(&xdg->grab_popups))
        return NULL;
    return lm_container_of(xdg->grab_popups.prev, struct xdg_popup, grab_link);
}

static void grab_refresh_focus(struct lm_xdg *xdg, uint32_t time_ms, double x, double y,
                               bool send_motion)
{
    struct lm_seat *seat = xdg_seat(xdg);
    struct lm_wm *wm = xdg_wm(xdg);
    struct xdg_popup *top = grab_top(xdg);
    if (!seat || !wm || !top)
        return;
    struct lm_pick pick;
    lm_wm_pick(wm, x, y, &pick);
    struct lm_surface *want = NULL;
    if (pick.kind == LM_PICK_SURFACE && pick.surface &&
        pick.surface->resource->client == top->res->client)
        want = pick.surface;
    if (want != lm_seat_pointer_focus(seat))
        lm_seat_pointer_set_focus(seat, want, want ? pick.sx : 0, want ? pick.sy : 0);
    if (want && send_motion)
        lm_seat_pointer_send_motion(seat, time_ms);
}

static void dismiss_grab_stack(struct lm_xdg *xdg)
{
    struct xdg_popup *top;
    while ((top = grab_top(xdg)) != NULL) {
        /* popup_dismiss removes `top` from the stack even if it was dismissed before */
        popup_dismiss(top);
        if (top->grabbing) {
            top->grabbing = false;
            lm_list_remove(&top->grab_link);
        }
    }
}

static void grab_motion(struct lm_pointer_grab *g, uint32_t time_ms, double x, double y)
{
    grab_refresh_focus(g->data, time_ms, x, y, true);
}

static void grab_button(struct lm_pointer_grab *g, uint32_t time_ms, uint32_t button, bool pressed)
{
    struct lm_xdg *xdg = g->data;
    struct lm_seat *seat = xdg_seat(xdg);
    if (!seat)
        return;
    if (lm_seat_pointer_focus(seat)) {
        lm_seat_pointer_send_button(seat, time_ms, button, pressed);
    } else if (pressed) {
        /* a click outside every popup of the grabbing client dismisses the whole chain */
        dismiss_grab_stack(xdg);
    }
}

static void grab_axis(struct lm_pointer_grab *g, uint32_t time_ms, int orientation, double value,
                      int32_t discrete)
{
    struct lm_xdg *xdg = g->data;
    struct lm_seat *seat = xdg_seat(xdg);
    if (seat && lm_seat_pointer_focus(seat))
        lm_seat_pointer_send_axis(seat, time_ms, orientation, value, discrete);
}

static void grab_cancel(struct lm_pointer_grab *g)
{
    struct lm_xdg *xdg = g->data;
    xdg->grab_installed = false; /* the seat already dropped it: do not ungrab again */
    dismiss_grab_stack(xdg);
}

static const struct lm_pointer_grab_ops popup_grab_ops = {
    .motion = grab_motion,
    .button = grab_button,
    .axis = grab_axis,
    .cancel = grab_cancel,
};

/* Where keyboard focus goes when the grabbing popup `leaving` stops owning it. */
static void grab_restore_keyboard_focus(struct lm_xdg *xdg, struct lm_surface *leaving)
{
    struct lm_seat *seat = xdg_seat(xdg);
    struct lm_wm *wm = xdg_wm(xdg);
    if (!seat)
        return;
    struct lm_surface *kf = lm_seat_keyboard_focus(seat);
    if (kf && kf != leaving)
        return;
    struct lm_surface *target = NULL;
    struct xdg_popup *top = grab_top(xdg);
    if (top && top->xs && top->xs->surface && top->xs->mapped) {
        target = top->xs->surface;
    } else if (wm) {
        struct lm_window *fw = lm_wm_focused_window(wm);
        target = fw ? fw->surface : NULL;
    }
    if (target != kf)
        lm_seat_set_keyboard_focus(seat, target);
}

static void grab_stack_remove(struct xdg_popup *xp)
{
    struct lm_xdg *xdg = xp->xdg;
    if (!xp->grabbing)
        return;
    xp->grabbing = false;
    lm_list_remove(&xp->grab_link);
    struct lm_surface *leaving = xp->xs ? xp->xs->surface : NULL;
    if (lm_list_empty(&xdg->grab_popups) && xdg->grab_installed) {
        xdg->grab_installed = false;
        struct lm_seat *seat = xdg_seat(xdg);
        if (seat)
            lm_seat_pointer_ungrab(seat, &xdg->grab);
    }
    grab_restore_keyboard_focus(xdg, leaving);
}

/* ---- xdg_popup ---------------------------------------------------------------------------------- */

/* Global origin of the parent's window geometry; false if the parent is not on screen. */
static bool popup_parent_origin(const struct xdg_popup *xp, int32_t *gx, int32_t *gy)
{
    struct lm_wm *wm = xdg_wm(xp->xdg);
    const struct xdg_surface *pxs = xp->parent;
    int32_t ox, oy;
    if (!wm || !pxs || !pxs->surface || !pxs->mapped)
        return false;
    if (!lm_wm_surface_origin(wm, pxs->surface, &ox, &oy))
        return false;
    *gx = clamp_coord((int64_t)ox + (pxs->geom_valid ? pxs->geom.x : 0));
    *gy = clamp_coord((int64_t)oy + (pxs->geom_valid ? pxs->geom.y : 0));
    return true;
}

static void popup_compute(struct xdg_popup *xp, struct lm_rect *out)
{
    struct lm_wm *wm = xdg_wm(xp->xdg);
    struct lm_rect bounds = { 0, 0, 0, 0 };
    bool have = false;
    int32_t px, py;
    if (wm && popup_parent_origin(xp, &px, &py)) {
        struct lm_rect area = { 0, 0, 0, 0 };
        int64_t ax = (int64_t)px + xp->pos.anchor_rect.x + xp->pos.anchor_rect.w / 2;
        int64_t ay = (int64_t)py + xp->pos.anchor_rect.y + xp->pos.anchor_rect.h / 2;
        lm_wm_usable_area(wm, clamp_coord(ax), clamp_coord(ay), &area);
        if (!lm_rect_is_empty(&area)) {
            bounds = lm_rect_make(clamp_coord((int64_t)area.x - px),
                                  clamp_coord((int64_t)area.y - py), area.w, area.h);
            have = true;
        }
    }
    positioner_compute(&xp->pos, have ? &bounds : NULL, out);
}

static void popup_send_configure(struct xdg_popup *xp)
{
    struct lm_rect r;
    popup_compute(xp, &r);
    xp->x = r.x;
    xp->y = r.y;
    xp->w = r.w;
    xp->h = r.h;
    lm_resource_post(xp->res, LM_XDG_POPUP_EVT_CONFIGURE, r.x, r.y, r.w, r.h);
    xs_send_configure(xp->xs);
}

static void popup_apply_position(struct xdg_popup *xp)
{
    int32_t gx, gy;
    if (!xp->popup || !popup_parent_origin(xp, &gx, &gy))
        return;
    lm_wm_popup_set_position(xp->popup, clamp_coord((int64_t)gx + xp->x),
                             clamp_coord((int64_t)gy + xp->y));
}

static void xs_update_popup_positions(struct xdg_surface *xs, int guard)
{
    struct xdg_popup *xp;
    if (guard > XDG_POPUP_MAX_DEPTH + 1)
        return;
    lm_list_for_each(xp, &xs->popups, sibling) {
        if (!xp->xs || !xp->xs->mapped)
            continue;
        popup_apply_position(xp);
        xs_update_popup_positions(xp->xs, guard + 1);
    }
}

static void popup_unmap(struct xdg_popup *xp)
{
    if (xp->xs && xp->xs->mapped && xp->popup && xdg_wm(xp->xdg))
        lm_wm_popup_unmap(xp->popup);
    if (xp->xs)
        xp->xs->mapped = false;
}

/* popup_done to the client, unmap, release the grab. Children go first. */
static void popup_dismiss(struct xdg_popup *xp)
{
    if (xp->dismissed) {
        grab_stack_remove(xp);
        return;
    }
    if (xp->xs)
        xs_dismiss_children(xp->xs);
    xp->dismissed = true;
    if (!res_dying(xp->res))
        lm_resource_post(xp->res, LM_XDG_POPUP_EVT_POPUP_DONE);
    popup_unmap(xp);
    grab_stack_remove(xp);
}

static void xs_dismiss_children(struct xdg_surface *xs)
{
    struct xdg_popup *xp, *tmp;
    lm_list_for_each_safe(xp, tmp, &xs->popups, sibling)
        popup_dismiss(xp);
}

void lm_xdg_dismiss_popups(struct lm_surface *s)
{
    struct xdg_surface *xs = xs_from_surface(s);
    if (xs)
        xs_dismiss_children(xs);
}

/* Detach from the wm and the grab; the surface may be going away right now. */
static void popup_wm_detach(struct xdg_popup *xp)
{
    popup_unmap(xp);
    grab_stack_remove(xp);
    if (xp->popup) {
        if (xdg_wm(xp->xdg))
            lm_wm_popup_destroy(xp->popup);
        xp->popup = NULL;
    }
}

static void popup_commit(struct xdg_surface *xs)
{
    struct xdg_popup *xp = xs->popup;
    struct lm_surface *s = xs->surface;

    if (xs->acked_valid) {
        xs->configured = true;
        xs->acked_valid = false;
    }
    if (!s->has_content) {
        if (xs->mapped) {
            popup_unmap(xp);
            grab_stack_remove(xp);
            xs_dismiss_children(xs);
            xs_reset_configure_state(xs);
        }
        if (!xs->initial_sent) {
            xs->initial_sent = true;
            if (!xp->dismissed)
                popup_send_configure(xp);
        }
        return;
    }
    if (!xs->configured) {
        lm_resource_post_error(xs->res, LM_XDG_SURFACE_ERR_UNCONFIGURED_BUFFER,
                               "xdg_popup committed a buffer before acking a configure");
        return;
    }
    if (xp->dismissed || !xp->popup)
        return;
    xs_update_geometry(xs);
    lm_wm_popup_set_geometry(xp->popup, &xs->geom);
    popup_apply_position(xp);
    if (!xs->mapped) {
        xs->mapped = true;
        popup_apply_position(xp);
        lm_wm_popup_map(xp->popup);
        if (xp->grabbing) {
            struct lm_seat *seat = xdg_seat(xp->xdg);
            if (seat)
                lm_seat_set_keyboard_focus(seat, s);
        }
    }
    xs_update_popup_positions(xs, 0);
}

static void popup_req_destroy(struct lm_resource *res, const union lm_arg *args)
{
    struct xdg_popup *xp = res->data;
    (void)args;
    if (xp->xs && !lm_list_empty(&xp->xs->popups)) {
        lm_resource_post_error(xp->xs->base->res, LM_XDG_WM_BASE_ERR_NOT_THE_TOPMOST_POPUP,
                               "xdg_popup destroyed while it still has child popups");
        return;
    }
    lm_resource_destroy(res);
}

static void popup_req_grab(struct lm_resource *res, const union lm_arg *args)
{
    struct xdg_popup *xp = res->data;
    struct lm_xdg *xdg = xp->xdg;
    struct lm_seat *seat = xdg_seat(xdg);
    uint32_t serial = args[1].u;

    if (!xp->xs || xp->xs->mapped || xp->grabbing) {
        lm_resource_post_error(res, LM_XDG_POPUP_ERR_INVALID_GRAB,
                               "xdg_popup.grab must be called before the popup is mapped (and only once)");
        return;
    }
    struct xdg_surface *pxs = xp->parent;
    struct xdg_popup *pp = pxs && pxs->role == XDG_ROLE_POPUP ? pxs->popup : NULL;
    if (pp && !pp->grabbing && !pp->dismissed) {
        lm_resource_post_error(res, LM_XDG_POPUP_ERR_INVALID_GRAB,
                               "parent of a grabbing popup must be a toplevel or the topmost grabbing popup");
        return;
    }
    if (!seat || !pxs || (pp && pp->dismissed) || !lm_seat_serial_valid(seat, res->client, serial)) {
        popup_dismiss(xp);
        return;
    }
    if (pp) {
        if (!xdg->grab_installed || grab_top(xdg) != pp) {
            lm_resource_post_error(res, LM_XDG_POPUP_ERR_INVALID_GRAB,
                                   "parent popup is not the topmost grabbing popup");
            return;
        }
    } else {
        if (xdg->grab_installed || !lm_list_empty(&xdg->grab_popups))
            dismiss_grab_stack(xdg);
        xdg->grab.ops = &popup_grab_ops;
        xdg->grab.data = xdg;
        if (lm_seat_pointer_grab(seat, &xdg->grab) < 0) {
            popup_dismiss(xp);
            return;
        }
        xdg->grab_installed = true;
    }
    xp->grabbing = true;
    lm_list_append(&xdg->grab_popups, &xp->grab_link);

    double px, py;
    lm_seat_pointer_position(seat, &px, &py);
    grab_refresh_focus(xdg, lm_now_msec32(), px, py, false);
}

static void popup_req_reposition(struct lm_resource *res, const union lm_arg *args)
{
    struct xdg_popup *xp = res->data;
    const struct positioner *pos = args[0].r->data;
    uint32_t token = args[1].u;

    if (!positioner_complete(pos)) {
        lm_resource_post_error(xp->xs ? xp->xs->base->res : res,
                               LM_XDG_WM_BASE_ERR_INVALID_POSITIONER,
                               "xdg_positioner is incomplete (size and anchor rect are required)");
        return;
    }
    if (!xp->xs || xp->dismissed)
        return;
    xp->pos = *pos;
    lm_resource_post(res, LM_XDG_POPUP_EVT_REPOSITIONED, token);
    popup_send_configure(xp);
}

static const lm_request_fn popup_handlers[] = {
    [LM_XDG_POPUP_REQ_DESTROY] = popup_req_destroy,
    [LM_XDG_POPUP_REQ_GRAB] = popup_req_grab,
    [LM_XDG_POPUP_REQ_REPOSITION] = popup_req_reposition,
};

static void popup_destroy_hook(struct lm_resource *res)
{
    struct xdg_popup *xp = res->data;
    if (!xp)
        return;
    res->data = NULL;
    struct lm_xdg *xdg = xp->xdg;
    struct xdg_surface *xs = xp->xs;

    popup_wm_detach(xp);
    if (xs) {
        xs_dismiss_children(xs);
        xs->popup = NULL;
        xs->role = XDG_ROLE_NONE;
        if (xs->surface && xs->surface->role_data == xs)
            lm_surface_clear_role(xs->surface);
        xs_reset_configure_state(xs);
    }
    lm_list_remove(&xp->sibling);
    lm_list_remove(&xp->link);
    free(xp);
    xdg_unref(xdg);
}

/* ---- xdg_toplevel --------------------------------------------------------------------------------- */

static uint32_t window_state_to_xdg(uint32_t states, uint32_t *out)
{
    uint32_t n = 0;
    if (states & LM_WINDOW_MAXIMIZED)
        out[n++] = LM_XDG_TOPLEVEL_STATE_MAXIMIZED;
    if (states & LM_WINDOW_FULLSCREEN)
        out[n++] = LM_XDG_TOPLEVEL_STATE_FULLSCREEN;
    if (states & LM_WINDOW_RESIZING)
        out[n++] = LM_XDG_TOPLEVEL_STATE_RESIZING;
    if (states & LM_WINDOW_ACTIVATED)
        out[n++] = LM_XDG_TOPLEVEL_STATE_ACTIVATED;
    return n;
}

static void toplevel_send_configure(struct xdg_toplevel *xt, int32_t w, int32_t h, uint32_t states)
{
    uint32_t st[4];
    uint32_t n = window_state_to_xdg(states, st);
    struct lm_array arr = { .size = n * (uint32_t)sizeof(st[0]), .data = st };
    if (w < 0)
        w = 0;
    if (h < 0)
        h = 0;
    xt->cfg_w = w;
    xt->cfg_h = h;
    xt->cfg_states = states;
    lm_resource_post(xt->res, LM_XDG_TOPLEVEL_EVT_CONFIGURE, w, h, &arr);
    xs_send_configure(xt->xs);
}

static void window_op_configure(struct lm_window *w, int32_t width, int32_t height, uint32_t states)
{
    struct xdg_toplevel *xt = w->shell_data;
    if (!xt || !xt->xs || !xt->xs->initial_sent)
        return; /* the initial configure will carry the wm's state */
    toplevel_send_configure(xt, width, height, states);
}

static void window_op_close(struct lm_window *w)
{
    struct xdg_toplevel *xt = w->shell_data;
    if (xt)
        lm_resource_post(xt->res, LM_XDG_TOPLEVEL_EVT_CLOSE);
}

static const struct lm_window_ops toplevel_window_ops = {
    .configure = window_op_configure,
    .close = window_op_close,
};

static void toplevel_wm_detach(struct xdg_toplevel *xt)
{
    struct xdg_surface *xs = xt->xs;
    if (xs)
        xs_dismiss_children(xs);
    if (xt->window) {
        if (xdg_wm(xt->xdg))
            lm_wm_window_destroy(xt->window);
        xt->window = NULL;
    }
    if (xs)
        xs->mapped = false;
}

static bool seat_serial_ok(struct lm_resource *res, uint32_t serial)
{
    struct lm_seat *seat = xdg_seat(((struct xdg_toplevel *)res->data)->xdg);
    return seat && lm_seat_serial_valid(seat, res->client, serial);
}

static void toplevel_req_destroy(struct lm_resource *res, const union lm_arg *args)
{
    (void)args;
    lm_resource_destroy(res);
}

static bool toplevel_is_ancestor(const struct xdg_toplevel *anc, const struct xdg_toplevel *t)
{
    for (int guard = 0; t && guard < 1024; guard++, t = t->parent)
        if (t == anc)
            return true;
    return false;
}

static void toplevel_parent_destroyed(struct lm_listener *l, void *data);

static void toplevel_set_parent_internal(struct xdg_toplevel *xt, struct xdg_toplevel *parent)
{
    lm_listener_remove(&xt->parent_destroy);
    lm_list_init(&xt->parent_destroy.link);
    xt->parent = parent;
    if (parent) {
        xt->parent_destroy.notify = toplevel_parent_destroyed;
        lm_signal_add(&parent->res->destroy_signal, &xt->parent_destroy);
    }
    if (xt->window && xdg_wm(xt->xdg))
        lm_wm_window_set_parent(xt->window, parent ? parent->window : NULL);
}

/* The parent's resource is being destroyed: inherit its parent (xdg-shell requirement). */
static void toplevel_parent_destroyed(struct lm_listener *l, void *data)
{
    struct xdg_toplevel *xt = lm_container_of(l, struct xdg_toplevel, parent_destroy);
    struct lm_resource *pres = data;
    struct xdg_toplevel *pt = pres->data;
    toplevel_set_parent_internal(xt, pt ? pt->parent : NULL);
}

static void toplevel_req_set_parent(struct lm_resource *res, const union lm_arg *args)
{
    struct xdg_toplevel *xt = res->data;
    struct xdg_toplevel *parent = args[0].r ? args[0].r->data : NULL;
    if (parent && toplevel_is_ancestor(xt, parent)) {
        lm_resource_post_error(res, LM_XDG_TOPLEVEL_ERR_INVALID_PARENT,
                               "xdg_toplevel parent would create a cycle");
        return;
    }
    toplevel_set_parent_internal(xt, parent);
}

static void toplevel_req_set_title(struct lm_resource *res, const union lm_arg *args)
{
    struct xdg_toplevel *xt = res->data;
    char buf[XDG_STRING_MAX + 1];
    clean_string(buf, sizeof(buf), args[0].s);
    if (xt->window && xdg_wm(xt->xdg))
        lm_wm_window_set_title(xt->window, buf);
}

static void toplevel_req_set_app_id(struct lm_resource *res, const union lm_arg *args)
{
    struct xdg_toplevel *xt = res->data;
    char buf[XDG_STRING_MAX + 1];
    clean_string(buf, sizeof(buf), args[0].s);
    if (xt->window && xdg_wm(xt->xdg))
        lm_wm_window_set_app_id(xt->window, buf);
}

static void toplevel_req_show_window_menu(struct lm_resource *res, const union lm_arg *args)
{
    (void)res;
    (void)args; /* no window menu in MellaGUI: the title bar buttons cover it */
}

static void toplevel_req_move(struct lm_resource *res, const union lm_arg *args)
{
    struct xdg_toplevel *xt = res->data;
    if (!xt->window || !xdg_wm(xt->xdg) || !seat_serial_ok(res, args[1].u))
        return;
    lm_wm_window_request_move(xt->window);
}

static void toplevel_req_resize(struct lm_resource *res, const union lm_arg *args)
{
    struct xdg_toplevel *xt = res->data;
    uint32_t edges = args[2].u;
    switch (edges) {
    case LM_EDGE_NONE:
    case LM_EDGE_TOP:
    case LM_EDGE_BOTTOM:
    case LM_EDGE_LEFT:
    case LM_EDGE_TOP | LM_EDGE_LEFT:
    case LM_EDGE_BOTTOM | LM_EDGE_LEFT:
    case LM_EDGE_RIGHT:
    case LM_EDGE_TOP | LM_EDGE_RIGHT:
    case LM_EDGE_BOTTOM | LM_EDGE_RIGHT:
        break;
    default:
        lm_resource_post_error(res, LM_XDG_TOPLEVEL_ERR_INVALID_RESIZE_EDGE,
                               "invalid resize edge %u", edges);
        return;
    }
    if (!xt->window || !xdg_wm(xt->xdg) || edges == LM_EDGE_NONE ||
        !seat_serial_ok(res, args[1].u))
        return;
    lm_wm_window_request_resize(xt->window, edges);
}

static void toplevel_req_set_max_size(struct lm_resource *res, const union lm_arg *args)
{
    struct xdg_toplevel *xt = res->data;
    if (args[0].i < 0 || args[1].i < 0) {
        lm_resource_post_error(res, LM_XDG_TOPLEVEL_ERR_INVALID_SIZE,
                               "negative maximum size (%d x %d)", args[0].i, args[1].i);
        return;
    }
    xt->pend_max_w = clamp_i32(args[0].i, 0, XDG_SIZE_LIMIT);
    xt->pend_max_h = clamp_i32(args[1].i, 0, XDG_SIZE_LIMIT);
}

static void toplevel_req_set_min_size(struct lm_resource *res, const union lm_arg *args)
{
    struct xdg_toplevel *xt = res->data;
    if (args[0].i < 0 || args[1].i < 0) {
        lm_resource_post_error(res, LM_XDG_TOPLEVEL_ERR_INVALID_SIZE,
                               "negative minimum size (%d x %d)", args[0].i, args[1].i);
        return;
    }
    xt->pend_min_w = clamp_i32(args[0].i, 0, XDG_SIZE_LIMIT);
    xt->pend_min_h = clamp_i32(args[1].i, 0, XDG_SIZE_LIMIT);
}

static void toplevel_req_set_maximized(struct lm_resource *res, const union lm_arg *args)
{
    struct xdg_toplevel *xt = res->data;
    (void)args;
    if (xt->window && xdg_wm(xt->xdg))
        lm_wm_window_request_maximize(xt->window, true);
}

static void toplevel_req_unset_maximized(struct lm_resource *res, const union lm_arg *args)
{
    struct xdg_toplevel *xt = res->data;
    (void)args;
    if (xt->window && xdg_wm(xt->xdg))
        lm_wm_window_request_maximize(xt->window, false);
}

static void toplevel_req_set_fullscreen(struct lm_resource *res, const union lm_arg *args)
{
    struct xdg_toplevel *xt = res->data;
    (void)args; /* the requested output is ignored: the window goes fullscreen where it is */
    if (xt->window && xdg_wm(xt->xdg))
        lm_wm_window_request_fullscreen(xt->window, true);
}

static void toplevel_req_unset_fullscreen(struct lm_resource *res, const union lm_arg *args)
{
    struct xdg_toplevel *xt = res->data;
    (void)args;
    if (xt->window && xdg_wm(xt->xdg))
        lm_wm_window_request_fullscreen(xt->window, false);
}

static void toplevel_req_set_minimized(struct lm_resource *res, const union lm_arg *args)
{
    struct xdg_toplevel *xt = res->data;
    (void)args;
    if (xt->window && xdg_wm(xt->xdg))
        lm_wm_window_request_minimize(xt->window);
}

static const lm_request_fn toplevel_handlers[] = {
    [LM_XDG_TOPLEVEL_REQ_DESTROY] = toplevel_req_destroy,
    [LM_XDG_TOPLEVEL_REQ_SET_PARENT] = toplevel_req_set_parent,
    [LM_XDG_TOPLEVEL_REQ_SET_TITLE] = toplevel_req_set_title,
    [LM_XDG_TOPLEVEL_REQ_SET_APP_ID] = toplevel_req_set_app_id,
    [LM_XDG_TOPLEVEL_REQ_SHOW_WINDOW_MENU] = toplevel_req_show_window_menu,
    [LM_XDG_TOPLEVEL_REQ_MOVE] = toplevel_req_move,
    [LM_XDG_TOPLEVEL_REQ_RESIZE] = toplevel_req_resize,
    [LM_XDG_TOPLEVEL_REQ_SET_MAX_SIZE] = toplevel_req_set_max_size,
    [LM_XDG_TOPLEVEL_REQ_SET_MIN_SIZE] = toplevel_req_set_min_size,
    [LM_XDG_TOPLEVEL_REQ_SET_MAXIMIZED] = toplevel_req_set_maximized,
    [LM_XDG_TOPLEVEL_REQ_UNSET_MAXIMIZED] = toplevel_req_unset_maximized,
    [LM_XDG_TOPLEVEL_REQ_SET_FULLSCREEN] = toplevel_req_set_fullscreen,
    [LM_XDG_TOPLEVEL_REQ_UNSET_FULLSCREEN] = toplevel_req_unset_fullscreen,
    [LM_XDG_TOPLEVEL_REQ_SET_MINIMIZED] = toplevel_req_set_minimized,
};

static void toplevel_destroy_hook(struct lm_resource *res)
{
    struct xdg_toplevel *xt = res->data;
    if (!xt)
        return;
    res->data = NULL;
    struct lm_xdg *xdg = xt->xdg;
    struct xdg_surface *xs = xt->xs;

    if (xt->deco) {
        struct xdg_decoration *d = xt->deco;
        d->xt = NULL;
        xt->deco = NULL;
        if (!res_dying(res))
            lm_resource_post_error(d->res, LM_ZXDG_DECORATION_ERR_ORPHANED,
                                   "xdg_toplevel destroyed before its decoration object");
    }
    lm_listener_remove(&xt->parent_destroy);
    toplevel_wm_detach(xt);
    if (xs) {
        xs->toplevel = NULL;
        xs->role = XDG_ROLE_NONE;
        if (xs->surface && xs->surface->role_data == xs)
            lm_surface_clear_role(xs->surface);
        xs_reset_configure_state(xs);
    }
    lm_list_remove(&xt->link);
    free(xt);
    xdg_unref(xdg);
}

/* Applies the double-buffered min/max size. Returns false if a protocol error was posted. */
static bool toplevel_apply_pending(struct xdg_toplevel *xt)
{
    if (xt->pend_min_w == xt->min_w && xt->pend_min_h == xt->min_h &&
        xt->pend_max_w == xt->max_w && xt->pend_max_h == xt->max_h)
        return true;
    if ((xt->pend_max_w && xt->pend_min_w > xt->pend_max_w) ||
        (xt->pend_max_h && xt->pend_min_h > xt->pend_max_h)) {
        lm_resource_post_error(xt->res, LM_XDG_TOPLEVEL_ERR_INVALID_SIZE,
                               "minimum size %dx%d exceeds maximum size %dx%d",
                               xt->pend_min_w, xt->pend_min_h, xt->pend_max_w, xt->pend_max_h);
        return false;
    }
    bool min_changed = xt->pend_min_w != xt->min_w || xt->pend_min_h != xt->min_h;
    bool max_changed = xt->pend_max_w != xt->max_w || xt->pend_max_h != xt->max_h;
    xt->min_w = xt->pend_min_w;
    xt->min_h = xt->pend_min_h;
    xt->max_w = xt->pend_max_w;
    xt->max_h = xt->pend_max_h;
    if (xt->window && xdg_wm(xt->xdg)) {
        if (min_changed)
            lm_wm_window_set_min_size(xt->window, xt->min_w, xt->min_h);
        if (max_changed)
            lm_wm_window_set_max_size(xt->window, xt->max_w, xt->max_h);
    }
    return true;
}

static void toplevel_initial_configure(struct xdg_surface *xs)
{
    struct xdg_toplevel *xt = xs->toplevel;
    uint32_t before = xs->n_sent;
    xs->initial_sent = true;
    if (xt->window && xdg_wm(xt->xdg))
        lm_wm_window_initial_configure(xt->window);
    if (xs->n_sent == before) /* the wm stayed silent: let the client pick its size */
        toplevel_send_configure(xt, 0, 0, 0);
}

static void toplevel_commit(struct xdg_surface *xs)
{
    struct xdg_toplevel *xt = xs->toplevel;
    struct lm_surface *s = xs->surface;

    if (!toplevel_apply_pending(xt))
        return;
    if (xs->acked_valid) {
        xs->configured = true;
        xs->acked_valid = false;
    }
    if (!s->has_content) {
        if (xs->mapped) {
            xs_dismiss_children(xs);
            if (xt->window && xdg_wm(xt->xdg))
                lm_wm_window_unmap(xt->window);
            xs_reset_configure_state(xs);
        }
        if (!xs->initial_sent)
            toplevel_initial_configure(xs);
        return;
    }
    if (!xs->configured) {
        lm_resource_post_error(xs->res, LM_XDG_SURFACE_ERR_UNCONFIGURED_BUFFER,
                               "xdg_toplevel committed a buffer before acking a configure");
        return;
    }
    if (!xt->window || !xdg_wm(xt->xdg))
        return;
    if (xs_update_geometry(xs) || !xs->mapped)
        lm_wm_window_set_geometry(xt->window, &xs->geom);
    if (!xs->mapped) {
        xs->mapped = true;
        lm_wm_window_map(xt->window);
    }
    xs_update_popup_positions(xs, 0);
}

static void xdg_role_commit(struct lm_surface *s)
{
    struct xdg_surface *xs = xs_from_surface(s);
    if (!xs || xs->surface != s)
        return;
    if (xs->role == XDG_ROLE_TOPLEVEL && xs->toplevel)
        toplevel_commit(xs);
    else if (xs->role == XDG_ROLE_POPUP && xs->popup)
        popup_commit(xs);
}

/* ---- zxdg_decoration_manager_v1 ---------------------------------------------------------------------- */

/*
 * Sends decoration.configure(server_side). If the toplevel was already
 * configured the new mode must be carried by a fresh xdg_surface.configure
 * (the client acks it), so the current wm state is re-sent.
 */
static void decoration_configure(struct xdg_decoration *d)
{
    lm_resource_post(d->res, LM_ZXDG_TOPLEVEL_DECORATION_V1_EVT_CONFIGURE,
                     (uint32_t)LM_ZXDG_DECORATION_MODE_SERVER_SIDE);
    struct xdg_toplevel *xt = d->xt;
    if (xt && xt->xs && xt->xs->initial_sent) {
        toplevel_send_configure(xt, xt->cfg_w, xt->cfg_h, xt->cfg_states);
    }
}

static void deco_req_destroy(struct lm_resource *res, const union lm_arg *args)
{
    (void)args;
    lm_resource_destroy(res);
}

static void deco_req_set_mode(struct lm_resource *res, const union lm_arg *args)
{
    struct xdg_decoration *d = res->data;
    (void)args;
    if (!d->xt) {
        lm_resource_post_error(res, LM_ZXDG_DECORATION_ERR_ORPHANED,
                               "decoration object outlived its xdg_toplevel");
        return;
    }
    decoration_configure(d);
}

static void deco_req_unset_mode(struct lm_resource *res, const union lm_arg *args)
{
    deco_req_set_mode(res, args);
}

static const lm_request_fn deco_handlers[] = {
    [LM_ZXDG_TOPLEVEL_DECORATION_V1_REQ_DESTROY] = deco_req_destroy,
    [LM_ZXDG_TOPLEVEL_DECORATION_V1_REQ_SET_MODE] = deco_req_set_mode,
    [LM_ZXDG_TOPLEVEL_DECORATION_V1_REQ_UNSET_MODE] = deco_req_unset_mode,
};

static void deco_destroy_hook(struct lm_resource *res)
{
    struct xdg_decoration *d = res->data;
    if (!d)
        return;
    res->data = NULL;
    if (d->xt)
        d->xt->deco = NULL;
    free(d);
}

static void deco_mgr_req_destroy(struct lm_resource *res, const union lm_arg *args)
{
    (void)args;
    lm_resource_destroy(res);
}

static void deco_mgr_req_get_toplevel_decoration(struct lm_resource *res, const union lm_arg *args)
{
    struct xdg_toplevel *xt = args[1].r->data;
    if (!xt) {
        lm_resource_post_error(res, LM_ZXDG_DECORATION_ERR_ORPHANED, "xdg_toplevel is gone");
        return;
    }
    if (xt->deco) {
        lm_resource_post_error(res, LM_ZXDG_DECORATION_ERR_ALREADY_CONSTRUCTED,
                               "xdg_toplevel already has a decoration object");
        return;
    }
    struct xdg_surface *xs = xt->xs;
    if (xs && xs->surface &&
        (xs->mapped || xs->surface->has_content || lm_surface_has_pending_buffer(xs->surface))) {
        lm_resource_post_error(res, LM_ZXDG_DECORATION_ERR_UNCONFIGURED_BUFFER,
                               "decoration requested for an xdg_toplevel that already has a buffer");
        return;
    }
    struct xdg_decoration *d = calloc(1, sizeof(*d));
    if (!d) {
        lm_resource_post_no_memory(res);
        return;
    }
    d->res = lm_resource_create(res->client, args[0].n, &lm_zxdg_toplevel_decoration_v1_interface,
                                1, deco_handlers, d, deco_destroy_hook);
    if (!d->res) {
        free(d);
        return;
    }
    d->xt = xt;
    xt->deco = d;
    decoration_configure(d);
}

static const lm_request_fn deco_mgr_handlers[] = {
    [LM_ZXDG_DECORATION_MANAGER_V1_REQ_DESTROY] = deco_mgr_req_destroy,
    [LM_ZXDG_DECORATION_MANAGER_V1_REQ_GET_TOPLEVEL_DECORATION] = deco_mgr_req_get_toplevel_decoration,
};

static void deco_mgr_bind(struct lm_client *client, void *data, uint32_t version, uint32_t id)
{
    (void)data;
    (void)version;
    lm_resource_create(client, id, &lm_zxdg_decoration_manager_v1_interface,
                       LM_ZXDG_DECORATION_MANAGER_V1_VERSION, deco_mgr_handlers, NULL, NULL);
}

/* ---- module lifecycle ----------------------------------------------------------------------------------- */

struct lm_xdg *lm_xdg_create(struct lm_server *srv)
{
    struct lm_xdg *xdg = calloc(1, sizeof(*xdg));
    if (!xdg)
        return NULL;
    xdg->srv = srv;
    xdg->refs = 1;
    lm_list_init(&xdg->bases);
    lm_list_init(&xdg->toplevels);
    lm_list_init(&xdg->popups);
    lm_list_init(&xdg->grab_popups);
    xdg->grab.ops = &popup_grab_ops;
    xdg->grab.data = xdg;
    xdg->wm_base_global = lm_global_create(srv, &lm_xdg_wm_base_interface,
                                           LM_XDG_WM_BASE_VERSION, wm_base_bind, xdg);
    xdg->deco_global = lm_global_create(srv, &lm_zxdg_decoration_manager_v1_interface,
                                        LM_ZXDG_DECORATION_MANAGER_V1_VERSION, deco_mgr_bind, xdg);
    if (!xdg->wm_base_global || !xdg->deco_global) {
        lm_error("xdg: cannot create globals");
        lm_xdg_destroy(xdg);
        return NULL;
    }
    return xdg;
}

void lm_xdg_destroy(struct lm_xdg *xdg)
{
    if (!xdg || xdg->dead)
        return;

    struct lm_seat *seat = xdg_seat(xdg);
    if (xdg->grab_installed && seat)
        lm_seat_pointer_ungrab(seat, &xdg->grab);
    xdg->grab_installed = false;

    /*
     * The wm is destroyed before us and has freed its windows and popups:
     * forget them so that late client teardown does not touch freed memory.
     */
    struct xdg_toplevel *xt;
    lm_list_for_each(xt, &xdg->toplevels, link)
        xt->window = NULL;
    struct xdg_popup *xp;
    lm_list_for_each(xp, &xdg->popups, link)
        xp->popup = NULL;
    xdg->dead = true;

    if (xdg->wm_base_global) {
        lm_global_destroy(xdg->wm_base_global);
        xdg->wm_base_global = NULL;
    }
    if (xdg->deco_global) {
        lm_global_destroy(xdg->deco_global);
        xdg->deco_global = NULL;
    }
    xdg_unref(xdg);
}
