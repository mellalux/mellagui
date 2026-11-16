#include "pointerconstraints.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "list.h"
#include "log.h"
#include "protocol.h"
#include "seat.h"
#include "server.h"
#include "surface.h"
#include "util.h"
#include "wm.h"

struct lm_pconstraints {
    struct lm_server *srv;
    struct lm_global *global_c;
    struct lm_global *global_r;
    struct lm_list constraints;         /* struct constraint.link */
    struct lm_list relatives;           /* struct relative.link */
    bool updating;                      /* keeps lm_pconstraints_changed from running inside itself (a warp moves the pointer) */
    bool again;
};

struct constraint {
    struct lm_list link;                /* in lm_pconstraints.constraints */
    struct lm_pconstraints *pc;         /* NULL: the compositor is going away */
    struct lm_resource *res;
    struct lm_surface *surface;         /* NULL once it was destroyed (the constraint is then spent) */
    struct lm_listener gone;            /* surface destroy_signal */
    struct lm_listener commit;          /* surface commit_signal: the pending region becomes the region */
    bool lock;
    bool persistent;
    bool active;
    bool spent;                         /* a oneshot constraint that ended, or its surface went */
    struct lm_region region;            /* surface-local; empty + !has_region = the whole surface */
    bool has_region;
    struct lm_region pending;
    bool has_pending, pending_set;      /* set_region was called since the last commit */
    bool hint_set;
    double hx, hy;
};

struct relative {
    struct lm_list link;
    struct lm_resource *res;
    struct lm_pconstraints *pc;
};

/* ---- the region ------------------------------------------------------------------------------------------ */

static bool in_region(const struct constraint *c, double sx, double sy)
{
    if (!c->surface)
        return false;
    int32_t ix = (int32_t)floor(sx), iy = (int32_t)floor(sy);
    if (!c->has_region)
        return ix >= 0 && iy >= 0 && ix < c->surface->width && iy < c->surface->height;
    return lm_region_contains_point(&c->region, ix, iy);
}

/* ---- activation ---------------------------------------------------------------------------------------------- */

static struct constraint *active_one(const struct lm_pconstraints *pc)
{
    struct constraint *c;
    lm_list_for_each(c, &pc->constraints, link)
        if (c->active)
            return c;
    return NULL;
}

static bool should_be_active(struct lm_pconstraints *pc, struct constraint *c)
{
    struct lm_server *srv = pc->srv;
    if (c->spent || !c->surface || !srv->seat)
        return false;
    if (lm_seat_pointer_focus(srv->seat) != c->surface || lm_seat_pointer_has_grab(srv->seat))
        return false;
    struct lm_surface *kf = lm_seat_keyboard_focus(srv->seat);
    if (!kf || !kf->resource || !c->surface->resource || kf->resource->client != c->surface->resource->client)
        return false;
    double sx, sy;
    if (!lm_seat_pointer_surface_pos(srv->seat, &sx, &sy))
        return false;
    /* the keyboard focus is only the program's window: a lock on a surface of a program that is not the focused one's does nothing */
    return in_region(c, sx, sy);
}

static void deactivate(struct lm_pconstraints *pc, struct constraint *c)
{
    c->active = false;
    if (c->res)
        lm_resource_post(c->res, c->lock ? LM_ZWP_LOCKED_POINTER_V1_EVT_UNLOCKED : LM_ZWP_CONFINED_POINTER_V1_EVT_UNCONFINED);
    if (!c->persistent)
        c->spent = true;
    if (c->lock && c->hint_set && c->surface && pc->srv->seat && lm_seat_pointer_focus(pc->srv->seat) == c->surface) {
        double sx, sy, px, py;
        if (lm_seat_pointer_surface_pos(pc->srv->seat, &sx, &sy)) {
            lm_seat_pointer_position(pc->srv->seat, &px, &py);
            lm_seat_pointer_warp(pc->srv->seat, px - sx + c->hx, py - sy + c->hy);         /* where the program says its own cursor is */
        }
    }
}

void lm_pconstraints_changed(struct lm_pconstraints *pc)
{
    if (!pc || lm_list_empty(&pc->constraints))
        return;
    if (pc->updating) {
        pc->again = true;
        return;
    }
    pc->updating = true;
    do {
        pc->again = false;
        struct constraint *c, *tmp;
        lm_list_for_each_safe(c, tmp, &pc->constraints, link) {
            bool want = should_be_active(pc, c);
            if (c->active && !want)
                deactivate(pc, c);
            else if (!c->active && want && !active_one(pc)) {
                c->active = true;
                if (c->res)
                    lm_resource_post(c->res, c->lock ? LM_ZWP_LOCKED_POINTER_V1_EVT_LOCKED : LM_ZWP_CONFINED_POINTER_V1_EVT_CONFINED);
            }
        }
    } while (pc->again);
    pc->updating = false;
}

bool lm_pconstraints_locked(const struct lm_pconstraints *pc)
{
    const struct constraint *c = pc ? active_one(pc) : NULL;
    return c && c->lock;
}

void lm_pconstraints_confine(const struct lm_pconstraints *pc, double x, double y, double *tx, double *ty)
{
    const struct constraint *c = pc ? active_one(pc) : NULL;
    double sx, sy;
    if (!c || c->lock || !c->surface || !pc->srv->seat || !lm_seat_pointer_surface_pos(pc->srv->seat, &sx, &sy))
        return;
    double dx = *tx - x, dy = *ty - y;                          /* the move, in surface coordinates the same */
    double nx = sx + dx, ny = sy + dy;
    if (in_region(c, nx, ny))
        return;
    bool single = !c->has_region || c->region.n == 1;
    if (single) {                                               /* one rectangle: stop at its edge, and slide along it */
        struct lm_rect box = c->has_region ? c->region.rects[0] : lm_rect_make(0, 0, c->surface->width, c->surface->height);
        double e = 1.0 / 256.0;
        nx = LM_CLAMP(nx, (double)box.x, (double)box.x + box.w - e);
        ny = LM_CLAMP(ny, (double)box.y, (double)box.y + box.h - e);
    } else if (in_region(c, nx, sy)) {                          /* several: slide along one axis if that stays inside, else stand still */
        ny = sy;
    } else if (in_region(c, sx, ny)) {
        nx = sx;
    } else {
        nx = sx;
        ny = sy;
    }
    *tx = x + (nx - sx);
    *ty = y + (ny - sy);
}

/* ---- constraints (locked / confined pointer) ----------------------------------------------------------------- */

static void constraint_unlink(struct constraint *c)
{
    lm_listener_remove(&c->gone);
    lm_list_init(&c->gone.link);
    lm_listener_remove(&c->commit);
    lm_list_init(&c->commit.link);
}

static void surface_gone(struct lm_listener *l, void *data)
{
    (void)data;
    struct constraint *c = lm_container_of(l, struct constraint, gone);
    struct lm_pconstraints *pc = c->pc;
    c->surface = NULL;
    constraint_unlink(c);
    if (c->active) {
        c->active = false;
        c->spent = true;
        if (c->res)
            lm_resource_post(c->res, c->lock ? LM_ZWP_LOCKED_POINTER_V1_EVT_UNLOCKED : LM_ZWP_CONFINED_POINTER_V1_EVT_UNCONFINED);
    }
    c->spent = true;
    (void)pc;
}

static void surface_committed(struct lm_listener *l, void *data)
{
    (void)data;
    struct constraint *c = lm_container_of(l, struct constraint, commit);
    if (!c->pending_set)
        return;
    c->pending_set = false;
    c->has_region = c->has_pending;
    lm_region_clear(&c->region);
    if (c->has_pending)
        lm_region_copy(&c->region, &c->pending);
    if (c->pc)
        lm_pconstraints_changed(c->pc);                         /* the pointer may be outside the new region now (or inside it) */
}

static void constraint_destroy(struct lm_resource *res)
{
    struct constraint *c = res->data;
    if (!c)
        return;
    res->data = NULL;
    c->res = NULL;
    struct lm_pconstraints *pc = c->pc;
    if (c->active) {
        c->active = false;
        if (c->lock && c->hint_set && c->surface && pc && pc->srv->seat && lm_seat_pointer_focus(pc->srv->seat) == c->surface) {
            double sx, sy, px, py;
            if (lm_seat_pointer_surface_pos(pc->srv->seat, &sx, &sy)) {
                lm_seat_pointer_position(pc->srv->seat, &px, &py);
                lm_seat_pointer_warp(pc->srv->seat, px - sx + c->hx, py - sy + c->hy);
            }
        }
    }
    constraint_unlink(c);
    if (pc)
        lm_list_remove(&c->link);
    lm_region_fini(&c->region);
    lm_region_fini(&c->pending);
    free(c);
    if (pc)
        lm_pconstraints_changed(pc);
}

static void con_req_destroy(struct lm_resource *res, const union lm_arg *args)
{
    (void)args;
    lm_resource_destroy(res);
}

static void con_set_region(struct constraint *c, struct lm_resource *region)
{
    lm_region_clear(&c->pending);
    c->has_pending = false;
    c->pending_set = true;
    if (!region)
        return;                                                 /* null = the whole surface */
    const struct lm_region *r = lm_region_from_resource(region);
    if (r && lm_region_copy(&c->pending, r) == 0)
        c->has_pending = true;
}

static void locked_set_region(struct lm_resource *res, const union lm_arg *args)
{
    struct constraint *c = res->data;
    if (c)
        con_set_region(c, args[0].r);
}

static void locked_set_hint(struct lm_resource *res, const union lm_arg *args)
{
    struct constraint *c = res->data;
    if (!c)
        return;
    c->hint_set = true;
    c->hx = lm_fixed_to_double(args[0].f);
    c->hy = lm_fixed_to_double(args[1].f);
}

static void confined_set_region(struct lm_resource *res, const union lm_arg *args)
{
    struct constraint *c = res->data;
    if (c)
        con_set_region(c, args[0].r);
}

static const lm_request_fn locked_handlers[] = {
    [LM_ZWP_LOCKED_POINTER_V1_REQ_DESTROY] = con_req_destroy,
    [LM_ZWP_LOCKED_POINTER_V1_REQ_SET_CURSOR_POSITION_HINT] = locked_set_hint,
    [LM_ZWP_LOCKED_POINTER_V1_REQ_SET_REGION] = locked_set_region,
};

static const lm_request_fn confined_handlers[] = {
    [LM_ZWP_CONFINED_POINTER_V1_REQ_DESTROY] = con_req_destroy,
    [LM_ZWP_CONFINED_POINTER_V1_REQ_SET_REGION] = confined_set_region,
};

static void make_constraint(struct lm_resource *res, const union lm_arg *args, bool lock)
{
    struct lm_pconstraints *pc = res->data;
    struct lm_surface *s = lm_surface_from_resource(args[1].r);
    uint32_t lifetime = args[4].u;
    if (lifetime != LM_ZWP_POINTER_CONSTRAINTS_LIFETIME_ONESHOT && lifetime != LM_ZWP_POINTER_CONSTRAINTS_LIFETIME_PERSISTENT) {
        lm_resource_post_error(res, 0, "lifetime %u is not oneshot (1) or persistent (2)", lifetime);   /* (the protocol's enum has no error for it) */
        return;
    }
    if (pc && s) {
        struct constraint *o;
        lm_list_for_each(o, &pc->constraints, link) {
            if (o->surface == s && !o->spent) {
                lm_resource_post_error(res, LM_ZWP_POINTER_CONSTRAINTS_ERR_ALREADY_CONSTRAINED,
                                       "the surface has a pointer constraint already");
                return;
            }
        }
    }
    struct constraint *c = calloc(1, sizeof(*c));
    if (!c) {
        lm_resource_post_no_memory(res);
        return;
    }
    lm_list_init(&c->link);
    lm_list_init(&c->gone.link);
    lm_list_init(&c->commit.link);
    lm_region_init(&c->region);
    lm_region_init(&c->pending);
    c->lock = lock;
    c->persistent = lifetime == LM_ZWP_POINTER_CONSTRAINTS_LIFETIME_PERSISTENT;
    c->res = lm_resource_create(res->client, args[0].n, lock ? &lm_zwp_locked_pointer_v1_interface : &lm_zwp_confined_pointer_v1_interface,
                                res->version, lock ? locked_handlers : confined_handlers, c, constraint_destroy);
    if (!c->res) {
        lm_region_fini(&c->region);
        lm_region_fini(&c->pending);
        free(c);
        return;
    }
    if (args[3].r) {                                            /* the region given at the start works at once */
        const struct lm_region *r = lm_region_from_resource(args[3].r);
        if (r && lm_region_copy(&c->region, r) == 0)
            c->has_region = true;
    }
    if (!pc || !s) {
        c->spent = true;                                        /* an inert object: the compositor is going away */
        return;
    }
    c->pc = pc;
    c->surface = s;
    c->gone.notify = surface_gone;
    lm_signal_add(&s->destroy_signal, &c->gone);
    c->commit.notify = surface_committed;
    lm_signal_add(&s->commit_signal, &c->commit);
    lm_list_append(&pc->constraints, &c->link);
    lm_pconstraints_changed(pc);
}

static void pcs_req_destroy(struct lm_resource *res, const union lm_arg *args)
{
    (void)args;
    lm_resource_destroy(res);
}

static void pcs_req_lock(struct lm_resource *res, const union lm_arg *args)
{
    make_constraint(res, args, true);
}

static void pcs_req_confine(struct lm_resource *res, const union lm_arg *args)
{
    make_constraint(res, args, false);
}

static const lm_request_fn pcs_handlers[] = {
    [LM_ZWP_POINTER_CONSTRAINTS_V1_REQ_DESTROY] = pcs_req_destroy,
    [LM_ZWP_POINTER_CONSTRAINTS_V1_REQ_LOCK_POINTER] = pcs_req_lock,
    [LM_ZWP_POINTER_CONSTRAINTS_V1_REQ_CONFINE_POINTER] = pcs_req_confine,
};

static void pcs_bind(struct lm_client *client, void *data, uint32_t version, uint32_t id)
{
    lm_resource_create(client, id, &lm_zwp_pointer_constraints_v1_interface, version, pcs_handlers, data, NULL);
}

/* ---- relative pointers -------------------------------------------------------------------------------------- */

static void relative_destroy(struct lm_resource *res)
{
    struct relative *r = res->data;
    if (!r)
        return;
    res->data = NULL;
    if (r->pc)
        lm_list_remove(&r->link);
    free(r);
}

static void rel_req_destroy(struct lm_resource *res, const union lm_arg *args)
{
    (void)args;
    lm_resource_destroy(res);
}

static const lm_request_fn relative_handlers[] = {
    [LM_ZWP_RELATIVE_POINTER_V1_REQ_DESTROY] = rel_req_destroy,
};

static void rmgr_req_destroy(struct lm_resource *res, const union lm_arg *args)
{
    (void)args;
    lm_resource_destroy(res);
}

static void rmgr_req_get_relative_pointer(struct lm_resource *res, const union lm_arg *args)
{
    struct lm_pconstraints *pc = res->data;
    struct relative *r = calloc(1, sizeof(*r));
    if (!r) {
        lm_resource_post_no_memory(res);
        return;
    }
    lm_list_init(&r->link);
    r->res = lm_resource_create(res->client, args[0].n, &lm_zwp_relative_pointer_v1_interface, res->version, relative_handlers, r, relative_destroy);
    if (!r->res) {
        free(r);
        return;
    }
    if (pc) {
        r->pc = pc;
        lm_list_append(&pc->relatives, &r->link);
    }
}

static const lm_request_fn rmgr_handlers[] = {
    [LM_ZWP_RELATIVE_POINTER_MANAGER_V1_REQ_DESTROY] = rmgr_req_destroy,
    [LM_ZWP_RELATIVE_POINTER_MANAGER_V1_REQ_GET_RELATIVE_POINTER] = rmgr_req_get_relative_pointer,
};

static void rmgr_bind(struct lm_client *client, void *data, uint32_t version, uint32_t id)
{
    lm_resource_create(client, id, &lm_zwp_relative_pointer_manager_v1_interface, version, rmgr_handlers, data, NULL);
}

void lm_pconstraints_relative(struct lm_pconstraints *pc, uint32_t time_ms, double dx, double dy, double ux, double uy)
{
    if (!pc || lm_list_empty(&pc->relatives) || !pc->srv->seat)
        return;
    struct lm_surface *f = lm_seat_pointer_focus(pc->srv->seat);
    if (!f || !f->resource)
        return;
    uint64_t us = (uint64_t)time_ms * 1000u;
    struct relative *r;
    lm_list_for_each(r, &pc->relatives, link)
        if (r->res->client == f->resource->client)
            lm_resource_post(r->res, LM_ZWP_RELATIVE_POINTER_V1_EVT_RELATIVE_MOTION, (uint32_t)(us >> 32), (uint32_t)us,
                             lm_fixed_from_double(dx), lm_fixed_from_double(dy), lm_fixed_from_double(ux), lm_fixed_from_double(uy));
}

/* ---- the module ------------------------------------------------------------------------------------------------ */

struct lm_pconstraints *lm_pconstraints_create(struct lm_server *srv)
{
    struct lm_pconstraints *pc = calloc(1, sizeof(*pc));
    if (!pc)
        return NULL;
    pc->srv = srv;
    lm_list_init(&pc->constraints);
    lm_list_init(&pc->relatives);
    pc->global_c = lm_global_create(srv, &lm_zwp_pointer_constraints_v1_interface, LM_ZWP_POINTER_CONSTRAINTS_V1_VERSION, pcs_bind, pc);
    pc->global_r = lm_global_create(srv, &lm_zwp_relative_pointer_manager_v1_interface, LM_ZWP_RELATIVE_POINTER_MANAGER_V1_VERSION, rmgr_bind, pc);
    if (!pc->global_c || !pc->global_r) {
        lm_pconstraints_destroy(pc);
        return NULL;
    }
    return pc;
}

void lm_pconstraints_destroy(struct lm_pconstraints *pc)
{
    if (!pc)
        return;
    struct constraint *c, *tmp;
    lm_list_for_each_safe(c, tmp, &pc->constraints, link) {
        lm_list_remove(&c->link);
        c->pc = NULL;                                           /* the resources live on until their clients go */
        c->active = false;
        c->spent = true;
        c->surface = NULL;
        constraint_unlink(c);
    }
    struct relative *r, *rtmp;
    lm_list_for_each_safe(r, rtmp, &pc->relatives, link) {
        lm_list_remove(&r->link);
        r->pc = NULL;
    }
    if (pc->global_c)
        lm_global_destroy(pc->global_c);
    if (pc->global_r)
        lm_global_destroy(pc->global_r);
    free(pc);
}

int lm_pconstraints_count(const struct lm_pconstraints *pc)
{
    int n = 0;
    const struct constraint *c;
    if (pc)
        lm_list_for_each(c, &pc->constraints, link)
            n++;
    return n;
}

const char *lm_pconstraints_active(const struct lm_pconstraints *pc)
{
    const struct constraint *c = pc ? active_one(pc) : NULL;
    return !c ? "none" : c->lock ? "locked" : "confined";
}

int lm_pconstraints_relatives(const struct lm_pconstraints *pc)
{
    int n = 0;
    const struct relative *r;
    if (pc)
        lm_list_for_each(r, &pc->relatives, link)
            n++;
    return n;
}
