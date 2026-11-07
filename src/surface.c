/*
 * wl_compositor, wl_surface, wl_region, wl_subcompositor, wl_subsurface.
 *
 * Every wl_surface has a *pending* state (what requests write to) and a *cache*
 * state. wl_surface.commit always merges pending into the cache; unless the
 * surface is an effectively synchronized subsurface the cache is then applied
 * at once. A synchronized subsurface keeps its cache until its parent's state
 * is applied (apply_tree()). The "current" state lives in struct lm_surface.
 *
 * Subsurface stacking: each surface owns two ordered lists of struct
 * lm_subsurface, `sub_list` (applied) and `sub_pending` (changed by
 * place_above/below). The surface itself is a member of both lists through
 * its embedded `self` node so children can be above or below it. Position and
 * order changes become current when the parent's state is applied.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "output.h"
#include "log.h"
#include "protocol.h"
#include "render.h"
#include "server.h"
#include "shm.h"
#include "surface.h"

#define LM_MAX_SUB_DEPTH     64            /* nesting depth of the subsurface tree */
#define LM_SUB_POS_LIMIT     (1 << 22)     /* 64 * 2^22 stays within LM_COORD_LIMIT */
#define LM_MAX_CALLBACKS     2048          /* frame callbacks alive per surface */
#define LM_CLIENT_TEX_LIMIT  ((size_t)512 << 20) /* bytes of surface textures per client */
#define LM_MAX_DMG_RECTS     12

/* ---- private types ---------------------------------------------------------------- */

struct lm_compositor {
    struct lm_server *srv;
    struct lm_global *comp_global;
    struct lm_global *sub_global;
    struct lm_list surfaces;    /* struct lm_surface.link */
    struct lm_list binds;       /* struct comp_bind.link */
};

/* Data of a wl_compositor resource: survives the compositor being destroyed first. */
struct comp_bind {
    struct lm_list link;
    struct lm_compositor *comp;     /* NULL after lm_compositor_destroy() */
};

struct frame_cb {
    struct lm_list link;            /* in a surface's pending/cache/latched list */
    struct lm_resource *res;
    struct surf *owner;
};

/* Double-buffered surface state (used for both `pending` and `cache`). */
struct sstate {
    struct lm_buffer *buffer;       /* referenced */
    bool buffer_set;                /* attach seen (buffer may still be NULL) */
    struct lm_region damage;        /* surface coordinates */
    struct lm_region damage_buf;    /* buffer coordinates */
    struct lm_region opaque;
    bool opaque_set;
    struct lm_region input;
    bool input_set;
    bool input_infinite;
    int32_t scale;                  /* latest requested value, persists */
    uint32_t transform;             /* latest requested value, persists */
    struct lm_list frames;          /* struct frame_cb.link */
};

struct lm_subsurface {
    struct lm_list link;            /* in parent's sub_list */
    struct lm_list pending_link;    /* in parent's sub_pending */
    struct lm_surface *child;       /* NULL: node is the parent itself, or inert */
    struct lm_surface *parent;      /* NULL once the parent surface is gone */
    struct lm_resource *resource;
    int32_t x, y;                   /* applied position */
    int32_t pending_x, pending_y;
    bool pos_dirty;
    bool sync;
    bool is_self;
};

struct out_entry {
    struct lm_list link;
    struct lm_output *out;          /* identity only, never dereferenced */
    struct lm_resource *res;        /* the wl_output resource enter() was sent with */
    struct lm_listener destroy_l;
    struct surf *owner;
};

struct surf {
    struct lm_surface pub;          /* first: pub.priv points back at the surf */
    struct lm_compositor *comp;     /* NULL once the compositor is gone ("quiet") */

    struct sstate pending;
    struct sstate cache;
    bool cache_valid;
    bool commit_has_buffer;         /* the state being applied carries a buffer */
    struct lm_region opaque_raw;    /* as requested, unclipped */

    struct lm_list frames;          /* latched frame callbacks */
    int n_callbacks;

    struct lm_list sub_list;
    struct lm_list sub_pending;
    struct lm_subsurface self;

    struct lm_list outputs;         /* struct out_entry.link */
};

struct region {
    struct lm_region r;
};

struct dmg_list {
    struct lm_rect r[LM_MAX_DMG_RECTS];
    int n;
};

static inline struct surf *SURF(const struct lm_surface *s)
{
    return lm_container_of(s, struct surf, pub);
}

static void surface_destroy_hook(struct lm_resource *res);
static void commit_tree(struct lm_surface *s, bool s_eff_sync);

static const struct lm_surface_role sub_role = {
    .name = "wl_subsurface",
};

/* ---- small helpers --------------------------------------------------------------- */

static void region_swap(struct lm_region *a, struct lm_region *b)
{
    struct lm_region t = *a;
    *a = *b;
    *b = t;
}

/* Move src into dst (union); src is emptied. Returns -1 on OOM. */
static int region_merge(struct lm_region *dst, struct lm_region *src)
{
    int rc = 0;
    if (dst->n == 0)
        region_swap(dst, src);
    else
        rc = lm_region_add_region(dst, src);
    lm_region_clear(src);
    return rc;
}

static int64_t floor_div64(int64_t v, int64_t d)
{
    int64_t q = v / d;
    return (v % d != 0 && ((v < 0) != (d < 0))) ? q - 1 : q;
}

static int64_t ceil_div64(int64_t v, int64_t d)
{
    return -floor_div64(-v, d);
}

static int64_t clamp_coord(int64_t v)
{
    return LM_CLAMP(v, -(int64_t)LM_COORD_LIMIT, (int64_t)LM_COORD_LIMIT);
}

static void dmg_add(struct dmg_list *d, int64_t x, int64_t y, int64_t w, int64_t h)
{
    if (w <= 0 || h <= 0)
        return;
    struct lm_rect r = lm_rect_make((int32_t)clamp_coord(x), (int32_t)clamp_coord(y),
                                    (int32_t)LM_MIN(w, (int64_t)LM_COORD_LIMIT),
                                    (int32_t)LM_MIN(h, (int64_t)LM_COORD_LIMIT));
    if (d->n == LM_MAX_DMG_RECTS)
        d->r[d->n - 1] = lm_rect_union(&d->r[d->n - 1], &r);
    else
        d->r[d->n++] = r;
}

static void dmg_add_rect(struct dmg_list *d, const struct lm_rect *r)
{
    dmg_add(d, r->x, r->y, r->w, r->h);
}

/* ---- pending/cache state --------------------------------------------------------- */

static void state_init(struct sstate *st)
{
    memset(st, 0, sizeof(*st));
    lm_region_init(&st->damage);
    lm_region_init(&st->damage_buf);
    lm_region_init(&st->opaque);
    lm_region_init(&st->input);
    lm_list_init(&st->frames);
    st->scale = 1;
    st->input_infinite = true;
}

static void state_drop_buffer(struct sstate *st)
{
    lm_buffer_unref(st->buffer);
    st->buffer = NULL;
    st->buffer_set = false;
}

/* Forget all requested changes. scale/transform persist; frame callbacks are not touched. */
static void state_clear(struct sstate *st)
{
    state_drop_buffer(st);
    lm_region_clear(&st->damage);
    lm_region_clear(&st->damage_buf);
    lm_region_clear(&st->opaque);
    lm_region_clear(&st->input);
    st->opaque_set = false;
    st->input_set = false;
    st->input_infinite = true;
}

static void state_fini(struct sstate *st)
{
    state_drop_buffer(st);
    lm_region_fini(&st->damage);
    lm_region_fini(&st->damage_buf);
    lm_region_fini(&st->opaque);
    lm_region_fini(&st->input);
}

/* dst := dst + src (later wins); src is left cleared. Returns -1 on OOM. */
static int state_merge(struct sstate *dst, struct sstate *src)
{
    int rc = 0;
    if (src->buffer_set) {
        lm_buffer_unref(dst->buffer);
        dst->buffer = src->buffer;
        dst->buffer_set = true;
        src->buffer = NULL;
        src->buffer_set = false;
    }
    if (region_merge(&dst->damage, &src->damage) < 0)
        rc = -1;
    if (region_merge(&dst->damage_buf, &src->damage_buf) < 0)
        rc = -1;
    if (src->opaque_set) {
        region_swap(&dst->opaque, &src->opaque);
        dst->opaque_set = true;
        lm_region_clear(&src->opaque);
        src->opaque_set = false;
    }
    if (src->input_set) {
        region_swap(&dst->input, &src->input);
        dst->input_set = true;
        dst->input_infinite = src->input_infinite;
        lm_region_clear(&src->input);
        src->input_set = false;
    }
    dst->scale = src->scale;
    dst->transform = src->transform;
    lm_list_splice(&dst->frames, &src->frames);
    return rc;
}

/* ---- roles -------------------------------------------------------------------------- */

static bool role_compatible(const struct lm_surface *s, const struct lm_surface_role *role)
{
    return !s->role || s->role == role || strcmp(s->role->name, role->name) == 0;
}

int lm_surface_set_role(struct lm_surface *s, const struct lm_surface_role *role, void *role_data)
{
    if (!s || !role || !role_data)
        return -1;
    if (!role_compatible(s, role) || s->role_data)
        return -1;
    s->role = role;
    s->role_data = role_data;
    return 0;
}

void lm_surface_clear_role(struct lm_surface *s)
{
    if (!s)
        return;
    s->role_data = NULL;
    lm_surface_reset_content(s);
}

bool lm_surface_has_pending_buffer(const struct lm_surface *s)
{
    const struct surf *p = SURF(s);
    return p->pending.buffer != NULL || p->commit_has_buffer;
}

/* ---- surface tree ---------------------------------------------------------------------- */

struct lm_surface *lm_surface_root(struct lm_surface *s)
{
    for (int guard = 0; s->sub && s->sub->parent && guard < 4 * LM_MAX_SUB_DEPTH; guard++)
        s = s->sub->parent;
    return s;
}

void lm_surface_offset_in_root(const struct lm_surface *s, int32_t *dx, int32_t *dy)
{
    int64_t x = 0, y = 0;
    for (int guard = 0; s->sub && s->sub->parent && guard < 4 * LM_MAX_SUB_DEPTH; guard++) {
        x += s->sub->x;
        y += s->sub->y;
        s = s->sub->parent;
    }
    if (dx)
        *dx = (int32_t)x;
    if (dy)
        *dy = (int32_t)y;
}

static bool visit(struct lm_surface *s, int32_t x, int32_t y, lm_surface_visit_fn fn, void *data)
{
    struct surf *p = SURF(s);
    struct lm_subsurface *sub;
    lm_list_for_each(sub, &p->sub_list, link) {
        if (sub->is_self) {
            if (s->has_content && !fn(s, x, y, data))
                return false;
        } else if (sub->child) {
            if (!visit(sub->child, x + sub->x, y + sub->y, fn, data))
                return false;
        }
    }
    return true;
}

bool lm_surface_tree_visit(struct lm_surface *root, int32_t x, int32_t y, lm_surface_visit_fn fn,
                           void *data)
{
    if (!root || !fn)
        return true;
    return visit(root, x, y, fn, data);
}

bool lm_surface_accepts_input(const struct lm_surface *s, double sx, double sy)
{
    if (!s->has_content)
        return false;
    if (!(sx >= 0.0 && sy >= 0.0 && sx < (double)s->width && sy < (double)s->height))
        return false; /* also rejects NaN */
    if (s->input_infinite)
        return true;
    return lm_region_contains_point(&s->input, (int32_t)floor(sx), (int32_t)floor(sy));
}

static struct lm_surface *pick(struct lm_surface *s, int32_t x, int32_t y, double px, double py,
                               double *sx, double *sy)
{
    struct surf *p = SURF(s);
    struct lm_subsurface *sub;
    lm_list_for_each_reverse(sub, &p->sub_list, link) {
        if (sub->is_self) {
            double lx = px - x, ly = py - y;
            if (lm_surface_accepts_input(s, lx, ly)) {
                if (sx)
                    *sx = lx;
                if (sy)
                    *sy = ly;
                return s;
            }
        } else if (sub->child) {
            struct lm_surface *hit =
                pick(sub->child, x + sub->x, y + sub->y, px, py, sx, sy);
            if (hit)
                return hit;
        }
    }
    return NULL;
}

struct lm_surface *lm_surface_tree_pick(struct lm_surface *root, int32_t x, int32_t y, double px,
                                        double py, double *sx, double *sy)
{
    if (!root)
        return NULL;
    return pick(root, x, y, px, py, sx, sy);
}

static void extents(struct lm_surface *s, int32_t ox, int32_t oy, struct lm_rect *acc, bool *have)
{
    struct surf *p = SURF(s);
    struct lm_subsurface *sub;
    lm_list_for_each(sub, &p->sub_list, link) {
        if (sub->is_self) {
            if (!s->has_content)
                continue;
            struct lm_rect r = lm_rect_make(ox, oy, s->width, s->height);
            *acc = *have ? lm_rect_union(acc, &r) : r;
            *have = true;
        } else if (sub->child) {
            extents(sub->child, ox + sub->x, oy + sub->y, acc, have);
        }
    }
}

static bool tree_ext(struct lm_surface *s, struct lm_rect *out)
{
    bool have = false;
    struct lm_rect acc = { 0, 0, 0, 0 };
    extents(s, 0, 0, &acc, &have);
    if (have)
        *out = acc;
    return have;
}

bool lm_surface_tree_extents(struct lm_surface *root, struct lm_rect *out)
{
    struct lm_rect r;
    if (!root || !tree_ext(root, &r))
        return false;
    if (out)
        *out = r;
    return true;
}

static int sub_depth_up(struct lm_surface *s)
{
    int d = 0;
    while (s->sub && s->sub->parent && d < 4 * LM_MAX_SUB_DEPTH) {
        d++;
        s = s->sub->parent;
    }
    return d;
}

static int sub_height(struct lm_surface *s)
{
    struct surf *p = SURF(s);
    struct lm_subsurface *sub;
    int h = 0;
    lm_list_for_each(sub, &p->sub_list, link) {
        if (sub->is_self || !sub->child)
            continue;
        int c = 1 + sub_height(sub->child);
        if (c > h)
            h = c;
    }
    return h;
}

/* ---- damage reporting -------------------------------------------------------------------- */

/* Report `r` (in the coordinates of a surface whose origin is (ox, oy) in `root`). */
static void report_in_root(struct lm_surface *root, int32_t ox, int32_t oy, const struct lm_rect *r)
{
    if (!root->role || !root->role_data || !root->role->damage || !SURF(root)->comp)
        return;
    if (lm_rect_is_empty(r))
        return;
    struct lm_rect t = lm_rect_make(r->x + ox, r->y + oy, r->w, r->h);
    root->role->damage(root, &t);
}

static void report_damage(struct lm_surface *s, const struct lm_rect *r)
{
    int32_t ox, oy;
    lm_surface_offset_in_root(s, &ox, &oy);
    report_in_root(lm_surface_root(s), ox, oy, r);
}

/* ---- outputs (enter/leave) --------------------------------------------------------------- */

static void out_entry_free(struct out_entry *e)
{
    lm_listener_remove(&e->destroy_l);
    lm_list_remove(&e->link);
    free(e);
}

static void out_entry_res_destroyed(struct lm_listener *l, void *data)
{
    (void)data;
    struct out_entry *e = lm_container_of(l, struct out_entry, destroy_l);
    out_entry_free(e);
}

static void surf_set_outputs(struct lm_surface *s, struct lm_output *const *outs, size_t n)
{
    struct surf *p = SURF(s);
    struct out_entry *e, *tmp;

    lm_list_for_each_safe(e, tmp, &p->outputs, link) {
        bool keep = false;
        if (e->res->data == (void *)e->out) /* else: the output was removed */
            for (size_t i = 0; i < n; i++)
                if (outs[i] == e->out)
                    keep = true;
        if (keep)
            continue;
        if (s->resource)
            lm_resource_post(s->resource, LM_WL_SURFACE_EVT_LEAVE, e->res);
        out_entry_free(e);
    }

    if (!s->resource)
        return;
    for (size_t i = 0; i < n; i++) {
        bool present = false;
        lm_list_for_each(e, &p->outputs, link) {
            if (e->out == outs[i]) {
                present = true;
                break;
            }
        }
        if (present)
            continue;
        struct lm_resource *ores = lm_output_resource_for_client(outs[i], s->resource->client);
        if (!ores)
            continue; /* the client has not bound this output (yet) */
        e = calloc(1, sizeof(*e));
        if (!e)
            continue;
        e->out = outs[i];
        e->res = ores;
        e->owner = p;
        e->destroy_l.notify = out_entry_res_destroyed;
        lm_signal_add(&ores->destroy_signal, &e->destroy_l);
        lm_list_append(&p->outputs, &e->link);
        lm_resource_post(s->resource, LM_WL_SURFACE_EVT_ENTER, ores);
    }
}

static void set_outputs_rec(struct lm_surface *s, struct lm_output *const *outs, size_t n)
{
    surf_set_outputs(s, outs, n);
    struct surf *p = SURF(s);
    struct lm_subsurface *sub;
    lm_list_for_each(sub, &p->sub_list, link)
        if (!sub->is_self && sub->child)
            set_outputs_rec(sub->child, outs, n);
}

void lm_surface_set_outputs(struct lm_surface *root, struct lm_output *const *outs, size_t n)
{
    if (!root)
        return;
    set_outputs_rec(root, outs, n);
}

/* ---- frame callbacks -------------------------------------------------------------------- */

static void frame_cb_destroy(struct lm_resource *res)
{
    struct frame_cb *cb = res->data;
    res->data = NULL;
    if (!cb)
        return;
    lm_list_remove(&cb->link);
    cb->owner->n_callbacks--;
    free(cb);
}

static void destroy_callbacks(struct lm_list *head)
{
    while (!lm_list_empty(head)) {
        struct frame_cb *cb = lm_container_of(head->next, struct frame_cb, link);
        lm_resource_destroy(cb->res); /* unlinks and frees cb */
    }
}

static void fire_frames(struct lm_surface *s, uint32_t time_ms)
{
    struct surf *p = SURF(s);
    while (!lm_list_empty(&p->frames)) {
        struct frame_cb *cb = lm_container_of(p->frames.next, struct frame_cb, link);
        struct lm_resource *r = cb->res;
        lm_list_remove(&cb->link);
        lm_resource_post(r, LM_WL_CALLBACK_EVT_DONE, time_ms);
        lm_resource_destroy(r);
    }
    struct lm_subsurface *sub;
    lm_list_for_each(sub, &p->sub_list, link)
        if (!sub->is_self && sub->child)
            fire_frames(sub->child, time_ms);
}

void lm_surface_send_frame_done(struct lm_surface *root, uint32_t time_ms)
{
    if (root)
        fire_frames(root, time_ms);
}

bool lm_surface_tree_wants_frame(struct lm_surface *root)
{
    if (!root)
        return false;
    struct surf *p = SURF(root);
    if (!lm_list_empty(&p->frames))
        return true;
    struct lm_subsurface *sub;
    lm_list_for_each(sub, &p->sub_list, link)
        if (!sub->is_self && sub->child && lm_surface_tree_wants_frame(sub->child))
            return true;
    return false;
}

/* ---- applying state --------------------------------------------------------------------- */

/* Leaves the zero-copy state: the held wl_buffer goes back to the client, the image and texture are dropped. */
static void drop_zero_copy(struct lm_surface *s)
{
    if (s->gl_held) {
        lm_buffer_release(s->gl_held);
        lm_buffer_unref(s->gl_held);
        s->gl_held = NULL;
    }
    if (s->gl_dmabuf && s->server && s->server->render)
        lm_render_dmabuf_drop(s->server->render, s);
    s->gl_dmabuf = false;
    s->gl_full = true;
}

static void drop_content(struct lm_surface *s)
{
    drop_zero_copy(s);
    s->gl_full = true;
    lm_region_clear(&s->gl_damage);
    lm_texture_free(&s->tex);
    s->has_content = false;
    s->width = 0;
    s->height = 0;
    s->transform = LM_TRANSFORM_NORMAL;
    lm_region_clear(&s->opaque);
}

static size_t client_tex_bytes(struct lm_compositor *comp, struct lm_client *client,
                               const struct lm_surface *except)
{
    size_t total = 0;
    struct lm_surface *o;
    lm_list_for_each(o, &comp->surfaces, link) {
        if (o == except || !o->has_content || !o->resource || o->resource->client != client)
            continue;
        total += (size_t)o->tex.width * (size_t)o->tex.height * 4;
    }
    return total;
}

/* Surface damage and damage_buffer -> region in buffer coordinates (scale only). */
static void build_buffer_damage(const struct sstate *st, int32_t scale, struct lm_region *out)
{
    for (int32_t i = 0; i < st->damage.n; i++) {
        const struct lm_rect *r = &st->damage.rects[i];
        int64_t x1 = (int64_t)r->x * scale, y1 = (int64_t)r->y * scale;
        int64_t x2 = ((int64_t)r->x + r->w) * scale, y2 = ((int64_t)r->y + r->h) * scale;
        x1 = clamp_coord(x1);
        y1 = clamp_coord(y1);
        x2 = clamp_coord(x2);
        y2 = clamp_coord(y2);
        struct lm_rect b = lm_rect_make((int32_t)x1, (int32_t)y1, (int32_t)(x2 - x1),
                                        (int32_t)(y2 - y1));
        if (lm_region_add_rect(out, &b) < 0)
            return;
    }
    for (int32_t i = 0; i < st->damage_buf.n; i++)
        if (lm_region_add_rect(out, &st->damage_buf.rects[i]) < 0)
            return;
}

/* Requested damage (both kinds) -> surface-local rects clipped to w x h. */
static void collect_damage(const struct sstate *st, int32_t scale, int32_t w, int32_t h,
                           struct dmg_list *d)
{
    const struct lm_rect bounds = { 0, 0, w, h };
    for (int32_t i = 0; i < st->damage.n; i++) {
        struct lm_rect r;
        if (lm_rect_intersect(&st->damage.rects[i], &bounds, &r))
            dmg_add_rect(d, &r);
    }
    for (int32_t i = 0; i < st->damage_buf.n; i++) {
        const struct lm_rect *b = &st->damage_buf.rects[i];
        int64_t x1 = floor_div64(b->x, scale), y1 = floor_div64(b->y, scale);
        int64_t x2 = ceil_div64((int64_t)b->x + b->w, scale);
        int64_t y2 = ceil_div64((int64_t)b->y + b->h, scale);
        struct lm_rect r = lm_rect_make((int32_t)x1, (int32_t)y1, (int32_t)(x2 - x1),
                                        (int32_t)(y2 - y1)), c;
        if (lm_rect_intersect(&r, &bounds, &c))
            dmg_add_rect(d, &c);
    }
}

/*
 * Copy the attached buffer into the texture. Returns 0, -1 (OOM / over the
 * per-client budget) or -2 (pool memory gone). On failure the surface content
 * is dropped. *full is set when the whole surface content was replaced.
 */
static int attach_buffer(struct lm_surface *s, const struct sstate *st, struct lm_buffer *b,
                         bool *full)
{
    struct surf *p = SURF(s);
    uint32_t tr = st->transform;
    int32_t bw = b->width, bh = b->height;
    int32_t tw = (tr & 1) ? bh : bw, th = (tr & 1) ? bw : bh;

    size_t new_bytes = (size_t)tw * (size_t)th * 4;
    size_t cur_bytes = s->has_content ? (size_t)s->tex.width * (size_t)s->tex.height * 4 : 0;
    if (new_bytes > cur_bytes && p->comp && s->resource &&
        client_tex_bytes(p->comp, s->resource->client, s) + new_bytes > LM_CLIENT_TEX_LIMIT) {
        drop_content(s);
        return -1;
    }

    /* zero copy: a GPU client's dmabuf is sampled in place by the GL renderer */
    if (b->dmabuf && tr == LM_TRANSFORM_NORMAL && s->server && s->server->render &&
        lm_render_dmabuf_import(s->server->render, s, b)) {
        if (s->gl_held != b) {                                  /* the buffer before this one may go back to its client now */
            if (s->gl_held) {
                lm_buffer_release(s->gl_held);
                lm_buffer_unref(s->gl_held);
            }
            lm_buffer_ref(b);
            s->gl_held = b;
        }
        lm_texture_free(&s->tex);
        s->tex.width = bw;
        s->tex.height = bh;
        s->tex.stride = bw * 4;
        s->tex.opaque = s->tex.solid = b->format == LM_WL_SHM_FORMAT_XRGB8888;
        s->has_content = true;
        s->transform = tr;
        *full = true;
        return 0;
    }
    if (b->dmabuf && b->dmabuf->modifier != LM_DRM_FORMAT_MOD_LINEAR) {
        /* a tiled buffer cannot be read by the CPU: without the GPU path it cannot be shown (the content stays as it was) */
        static bool warned;
        if (!warned)
            lm_warn("dmabuf: cannot show a tiled buffer (modifier %#llx): the GL import failed or the renderer is software",
                    (unsigned long long)b->dmabuf->modifier);
        warned = true;
        return -3;
    }
    if (s->gl_dmabuf) {                                         /* back to copying: no CPU pixels exist yet */
        drop_zero_copy(s);
        lm_texture_free(&s->tex);
        s->has_content = false;
    }

    struct lm_region bd;
    lm_region_init(&bd);
    bool partial = false;
    if (tr == LM_TRANSFORM_NORMAL && s->transform == LM_TRANSFORM_NORMAL && s->has_content &&
        s->tex.width == bw && s->tex.height == bh &&
        s->tex.opaque == (b->format == LM_WL_SHM_FORMAT_XRGB8888)) {
        build_buffer_damage(st, st->scale, &bd);
        partial = bd.n > 0; /* no damage at all: be forgiving and take the whole buffer */
    }
    int rc = lm_buffer_copy(b, &s->tex, tr, partial ? &bd : NULL);
    if (rc == 0) {                                  /* the GL renderer re-uploads what changed */
        if (partial && !s->gl_full)
            lm_region_add_region(&s->gl_damage, &bd);
        else
            s->gl_full = true;
    }
    lm_region_fini(&bd);
    if (rc < 0) {
        drop_content(s);
        return rc;
    }
    s->has_content = true;
    s->transform = tr;
    *full = !partial;
    return 0;
}

/* Apply pending child positions and stacking order. Returns true if anything changed. */
static bool apply_layout(struct lm_surface *s)
{
    struct surf *p = SURF(s);
    struct lm_subsurface *sub, *tmp;
    bool changed = false;

    lm_list_for_each(sub, &p->sub_list, link) {
        if (sub->is_self || !sub->pos_dirty)
            continue;
        if (sub->x != sub->pending_x || sub->y != sub->pending_y)
            changed = true;
        sub->x = sub->pending_x;
        sub->y = sub->pending_y;
        sub->pos_dirty = false;
    }

    /* order */
    struct lm_list *a = p->sub_list.next, *b = p->sub_pending.next;
    bool same = true;
    while (a != &p->sub_list && b != &p->sub_pending) {
        struct lm_subsurface *sa = lm_container_of(a, struct lm_subsurface, link);
        struct lm_subsurface *sb = lm_container_of(b, struct lm_subsurface, pending_link);
        if (sa != sb) {
            same = false;
            break;
        }
        a = a->next;
        b = b->next;
    }
    if (!same) {
        lm_list_for_each_safe(sub, tmp, &p->sub_pending, pending_link) {
            lm_list_remove(&sub->link);
            lm_list_append(&p->sub_list, &sub->link);
        }
        changed = true;
    }
    return changed;
}

static void apply_state(struct lm_surface *s)
{
    struct surf *p = SURF(s);
    struct sstate *st = &p->cache;
    struct lm_server *srv = s->server;
    struct dmg_list dl;
    struct lm_rect old_ext, new_ext;
    bool had_ext = tree_ext(s, &old_ext);
    int32_t old_w = s->width, old_h = s->height;
    bool has_frames = !lm_list_empty(&st->frames);
    bool full = false;

    dl.n = 0;
    p->cache_valid = false;
    s->scale = st->scale;

    struct lm_buffer *b = NULL;
    bool buf_set = st->buffer_set;
    if (buf_set) {
        b = st->buffer;
        st->buffer = NULL;
        st->buffer_set = false;
    }
    p->commit_has_buffer = b != NULL;

    if (buf_set && !b) {
        drop_content(s);
    } else if (b) {
        int rc = attach_buffer(s, st, b, &full);
        if (rc == -1) {
            if (s->resource)
                lm_resource_post_no_memory(s->resource);
        } else if (rc == -2) {
            if (b->resource)
                lm_resource_post_error(b->resource, LM_WL_SHM_ERR_INVALID_FD,
                                       "failed to read the buffer (pool file truncated?)");
            else if (s->resource)
                lm_client_post_error(s->resource->client, 0, LM_WL_SHM_ERR_INVALID_FD,
                                     "failed to read the buffer (pool file truncated?)");
        } else if (s->gl_held != b) {                               /* (a zero-copy buffer stays with us until replaced) */
            lm_buffer_release(b);
        }
        lm_buffer_unref(b);
    }

    if (s->has_content) {
        s->width = LM_MAX(1, s->tex.width / s->scale);
        s->height = LM_MAX(1, s->tex.height / s->scale);
    } else {
        s->width = 0;
        s->height = 0;
    }

    /* regions */
    if (st->opaque_set)
        region_swap(&p->opaque_raw, &st->opaque);
    if (st->input_set) {
        s->input_infinite = st->input_infinite;
        region_swap(&s->input, &st->input);
    }
    if (st->opaque_set || s->width != old_w || s->height != old_h) {
        lm_region_clear(&s->opaque);
        if (s->has_content && lm_region_copy(&s->opaque, &p->opaque_raw) == 0) {
            struct lm_rect bounds = lm_rect_make(0, 0, s->width, s->height);
            lm_region_intersect_rect(&s->opaque, &bounds);
        } else {
            lm_region_clear(&s->opaque);
        }
    }

    bool layout_changed = apply_layout(s);
    lm_list_splice(&p->frames, &st->frames);

    /* damage in surface-local coordinates */
    if (full || (b && s->transform != LM_TRANSFORM_NORMAL))
        dmg_add(&dl, 0, 0, s->width, s->height);
    else
        collect_damage(st, s->scale, s->width, s->height, &dl);
    if (s->width != old_w || s->height != old_h) {
        dmg_add(&dl, 0, 0, old_w, old_h);
        dmg_add(&dl, 0, 0, s->width, s->height);
    }
    if (layout_changed) {
        if (had_ext)
            dmg_add_rect(&dl, &old_ext);
        if (tree_ext(s, &new_ext))
            dmg_add_rect(&dl, &new_ext);
    }
    state_clear(st);

    if (s->role && s->role_data && s->role->commit)
        s->role->commit(s);
    p->commit_has_buffer = false;

    if (p->comp) {
        int32_t ox, oy;
        lm_surface_offset_in_root(s, &ox, &oy);
        struct lm_surface *root = lm_surface_root(s);
        for (int i = 0; i < dl.n; i++)
            report_in_root(root, ox, oy, &dl.r[i]);
    }

    lm_signal_emit(&s->commit_signal, s);

    if (has_frames && p->comp && srv && srv->render)
        lm_render_request_frame(srv->render);
}

static void commit_tree(struct lm_surface *s, bool s_eff_sync)
{
    struct surf *p = SURF(s);
    if (p->cache_valid)
        apply_state(s);
    struct lm_subsurface *sub;
    lm_list_for_each(sub, &p->sub_list, link) {
        if (sub->is_self || !sub->child)
            continue;
        if (s_eff_sync || sub->sync)
            commit_tree(sub->child, true);
    }
}

static bool effectively_sync(const struct lm_surface *s)
{
    for (int guard = 0; s->sub && s->sub->parent && guard < 4 * LM_MAX_SUB_DEPTH; guard++) {
        if (s->sub->sync)
            return true;
        s = s->sub->parent;
    }
    return false;
}

void lm_surface_reset_content(struct lm_surface *s)
{
    if (!s)
        return;
    struct surf *p = SURF(s);
    struct lm_rect ext;
    bool have = tree_ext(s, &ext);

    state_clear(&p->pending);
    state_clear(&p->cache);
    p->commit_has_buffer = false;
    drop_content(s);
    lm_region_clear(&p->opaque_raw);
    lm_region_clear(&s->input);
    s->input_infinite = true;
    if (have)
        report_damage(s, &ext);
}

/* ---- wl_region -------------------------------------------------------------------------- */

static void region_resource_destroy(struct lm_resource *res)
{
    struct region *r = res->data;
    res->data = NULL;
    if (!r)
        return;
    lm_region_fini(&r->r);
    free(r);
}

static void region_req_destroy(struct lm_resource *res, const union lm_arg *args)
{
    (void)args;
    lm_resource_destroy(res);
}

static void region_req_add(struct lm_resource *res, const union lm_arg *args)
{
    struct region *r = res->data;
    struct lm_rect rect = lm_rect_make(args[0].i, args[1].i, args[2].i, args[3].i);
    if (r && lm_region_add_rect(&r->r, &rect) < 0)
        lm_resource_post_no_memory(res);
}

static void region_req_subtract(struct lm_resource *res, const union lm_arg *args)
{
    struct region *r = res->data;
    struct lm_rect rect = lm_rect_make(args[0].i, args[1].i, args[2].i, args[3].i);
    if (!r)
        return;
    if (lm_region_subtract_rect(&r->r, &rect) < 0)
        lm_region_clear(&r->r); /* unrepresentable: fail safe (no opaque / no input) */
}

static const lm_request_fn region_handlers[] = {
    [LM_WL_REGION_REQ_DESTROY] = region_req_destroy,
    [LM_WL_REGION_REQ_ADD] = region_req_add,
    [LM_WL_REGION_REQ_SUBTRACT] = region_req_subtract,
};

/* ---- wl_surface requests ------------------------------------------------------------------ */

static void surface_req_destroy(struct lm_resource *res, const union lm_arg *args)
{
    (void)args;
    lm_resource_destroy(res);
}

static void surface_req_attach(struct lm_resource *res, const union lm_arg *args)
{
    struct lm_surface *s = res->data;
    struct surf *p = SURF(s);
    struct lm_buffer *b = args[0].r ? lm_buffer_from_resource(args[0].r) : NULL;
    /* x/y offsets (deprecated since wl_surface v5) are accepted and ignored */

    if (b)
        lm_buffer_ref(b);
    lm_buffer_unref(p->pending.buffer);
    p->pending.buffer = b;
    p->pending.buffer_set = true;
}

static void surface_req_damage(struct lm_resource *res, const union lm_arg *args)
{
    struct lm_surface *s = res->data;
    struct lm_rect r = lm_rect_make(args[0].i, args[1].i, args[2].i, args[3].i);
    if (lm_region_add_rect(&SURF(s)->pending.damage, &r) < 0)
        lm_resource_post_no_memory(res);
}

static void surface_req_damage_buffer(struct lm_resource *res, const union lm_arg *args)
{
    struct lm_surface *s = res->data;
    struct lm_rect r = lm_rect_make(args[0].i, args[1].i, args[2].i, args[3].i);
    if (lm_region_add_rect(&SURF(s)->pending.damage_buf, &r) < 0)
        lm_resource_post_no_memory(res);
}

static void surface_req_frame(struct lm_resource *res, const union lm_arg *args)
{
    struct lm_surface *s = res->data;
    struct surf *p = SURF(s);

    if (p->n_callbacks >= LM_MAX_CALLBACKS) {
        lm_resource_post_no_memory(res);
        return;
    }
    struct frame_cb *cb = calloc(1, sizeof(*cb));
    if (!cb) {
        lm_resource_post_no_memory(res);
        return;
    }
    lm_list_init(&cb->link);
    cb->owner = p;
    cb->res = lm_resource_create(res->client, args[0].n, &lm_wl_callback_interface, 1, NULL, cb,
                                 frame_cb_destroy);
    if (!cb->res) {
        free(cb);
        return;
    }
    lm_list_append(&p->pending.frames, &cb->link);
    p->n_callbacks++;
}

static void surface_req_set_opaque_region(struct lm_resource *res, const union lm_arg *args)
{
    struct surf *p = SURF(res->data);
    struct region *reg = args[0].r ? args[0].r->data : NULL;

    lm_region_clear(&p->pending.opaque);
    p->pending.opaque_set = true;
    if (reg && lm_region_copy(&p->pending.opaque, &reg->r) < 0)
        lm_resource_post_no_memory(res);
}

static void surface_req_set_input_region(struct lm_resource *res, const union lm_arg *args)
{
    struct surf *p = SURF(res->data);
    struct region *reg = args[0].r ? args[0].r->data : NULL;

    lm_region_clear(&p->pending.input);
    p->pending.input_set = true;
    p->pending.input_infinite = reg == NULL;
    if (reg && lm_region_copy(&p->pending.input, &reg->r) < 0)
        lm_resource_post_no_memory(res);
}

static void surface_req_commit(struct lm_resource *res, const union lm_arg *args)
{
    (void)args;
    struct lm_surface *s = res->data;
    struct surf *p = SURF(s);

    if (state_merge(&p->cache, &p->pending) < 0) {
        lm_resource_post_no_memory(res);
        return;
    }
    p->cache_valid = true;
    if (effectively_sync(s))
        return; /* applied with the parent's next commit */
    commit_tree(s, false);
}

static void surface_req_set_buffer_transform(struct lm_resource *res, const union lm_arg *args)
{
    int32_t t = args[0].i;
    if (t < 0 || t > (int32_t)LM_TRANSFORM_FLIPPED_270) {
        lm_resource_post_error(res, LM_WL_SURFACE_ERR_INVALID_TRANSFORM,
                               "invalid buffer transform %d", t);
        return;
    }
    SURF(res->data)->pending.transform = (uint32_t)t;
}

static void surface_req_set_buffer_scale(struct lm_resource *res, const union lm_arg *args)
{
    int32_t sc = args[0].i;
    if (sc <= 0) {
        lm_resource_post_error(res, LM_WL_SURFACE_ERR_INVALID_SCALE, "invalid buffer scale %d", sc);
        return;
    }
    SURF(res->data)->pending.scale = sc;
}

static const lm_request_fn surface_handlers[] = {
    [LM_WL_SURFACE_REQ_DESTROY] = surface_req_destroy,
    [LM_WL_SURFACE_REQ_ATTACH] = surface_req_attach,
    [LM_WL_SURFACE_REQ_DAMAGE] = surface_req_damage,
    [LM_WL_SURFACE_REQ_FRAME] = surface_req_frame,
    [LM_WL_SURFACE_REQ_SET_OPAQUE_REGION] = surface_req_set_opaque_region,
    [LM_WL_SURFACE_REQ_SET_INPUT_REGION] = surface_req_set_input_region,
    [LM_WL_SURFACE_REQ_COMMIT] = surface_req_commit,
    [LM_WL_SURFACE_REQ_SET_BUFFER_TRANSFORM] = surface_req_set_buffer_transform,
    [LM_WL_SURFACE_REQ_SET_BUFFER_SCALE] = surface_req_set_buffer_scale,
    [LM_WL_SURFACE_REQ_DAMAGE_BUFFER] = surface_req_damage_buffer,
};

/* ---- subsurface plumbing ------------------------------------------------------------------ */

/* Unlink a live subsurface from its parent; both sides become inert. */
static void sub_detach(struct lm_subsurface *sub)
{
    struct lm_surface *c = sub->child;
    lm_list_remove(&sub->link);
    lm_list_remove(&sub->pending_link);
    sub->child = NULL;
    sub->parent = NULL;
    if (c)
        c->sub = NULL;
}

/* wl_subsurface resource going away (destroy request or client teardown). */
static void subsurface_resource_destroy(struct lm_resource *res)
{
    struct lm_subsurface *sub = res->data;
    res->data = NULL;
    if (!sub)
        return;
    struct lm_surface *c = sub->child;
    if (c) {
        struct lm_surface *root = lm_surface_root(c);
        int32_t ox, oy;
        struct lm_rect ext;
        bool have = tree_ext(c, &ext);
        lm_surface_offset_in_root(c, &ox, &oy);
        sub_detach(sub);
        lm_surface_clear_role(c);
        if (have)
            report_in_root(root, ox, oy, &ext);
    }
    free(sub);
}

static void subsurface_req_destroy(struct lm_resource *res, const union lm_arg *args)
{
    (void)args;
    lm_resource_destroy(res);
}

static void subsurface_req_set_position(struct lm_resource *res, const union lm_arg *args)
{
    struct lm_subsurface *sub = res->data;
    if (!sub || !sub->child || !sub->parent)
        return;
    sub->pending_x = LM_CLAMP(args[0].i, -LM_SUB_POS_LIMIT, LM_SUB_POS_LIMIT);
    sub->pending_y = LM_CLAMP(args[1].i, -LM_SUB_POS_LIMIT, LM_SUB_POS_LIMIT);
    sub->pos_dirty = true;
}

static void sub_place(struct lm_resource *res, struct lm_surface *sibling, bool above)
{
    struct lm_subsurface *sub = res->data;
    if (!sub || !sub->child || !sub->parent)
        return;
    struct surf *pp = SURF(sub->parent);
    struct lm_subsurface *node = NULL;

    if (sibling == sub->parent)
        node = &pp->self;
    else if (sibling != sub->child && sibling->sub && sibling->sub->parent == sub->parent)
        node = sibling->sub;
    if (!node) {
        lm_resource_post_error(res, LM_WL_SUBSURFACE_ERR_BAD_SURFACE,
                               "sibling is neither the parent nor a sibling subsurface");
        return;
    }
    lm_list_remove(&sub->pending_link);
    if (above)
        lm_list_insert(&node->pending_link, &sub->pending_link);
    else
        lm_list_insert(node->pending_link.prev, &sub->pending_link);
}

static void subsurface_req_place_above(struct lm_resource *res, const union lm_arg *args)
{
    struct lm_surface *sib = lm_surface_from_resource(args[0].r);
    if (sib)
        sub_place(res, sib, true);
}

static void subsurface_req_place_below(struct lm_resource *res, const union lm_arg *args)
{
    struct lm_surface *sib = lm_surface_from_resource(args[0].r);
    if (sib)
        sub_place(res, sib, false);
}

static void subsurface_req_set_sync(struct lm_resource *res, const union lm_arg *args)
{
    (void)args;
    struct lm_subsurface *sub = res->data;
    if (sub && sub->child && sub->parent)
        sub->sync = true;
}

static void subsurface_req_set_desync(struct lm_resource *res, const union lm_arg *args)
{
    (void)args;
    struct lm_subsurface *sub = res->data;
    if (!sub || !sub->child || !sub->parent || !sub->sync)
        return;
    sub->sync = false;
    /* became effectively desynchronized: whatever was cached goes live now */
    if (!effectively_sync(sub->child) && SURF(sub->child)->cache_valid)
        commit_tree(sub->child, false);
}

static const lm_request_fn subsurface_handlers[] = {
    [LM_WL_SUBSURFACE_REQ_DESTROY] = subsurface_req_destroy,
    [LM_WL_SUBSURFACE_REQ_SET_POSITION] = subsurface_req_set_position,
    [LM_WL_SUBSURFACE_REQ_PLACE_ABOVE] = subsurface_req_place_above,
    [LM_WL_SUBSURFACE_REQ_PLACE_BELOW] = subsurface_req_place_below,
    [LM_WL_SUBSURFACE_REQ_SET_SYNC] = subsurface_req_set_sync,
    [LM_WL_SUBSURFACE_REQ_SET_DESYNC] = subsurface_req_set_desync,
};

/* ---- wl_surface lifetime ------------------------------------------------------------------- */

static void surface_destroy_hook(struct lm_resource *res)
{
    struct lm_surface *s = res->data;
    if (!s)
        return;
    struct surf *p = SURF(s);
    res->data = NULL;

    lm_signal_emit(&s->destroy_signal, s);
    if (s->gl_held) {
        lm_buffer_release(s->gl_held);
        lm_buffer_unref(s->gl_held);
        s->gl_held = NULL;
    }
    if ((s->gl_tex || s->gl_img) && s->server && s->server->render)
        lm_render_surface_gone(s->server->render, s);

    /* If this is a mapped subsurface its parent tree must be repainted. */
    struct lm_surface *root = lm_surface_root(s);
    struct lm_rect ext;
    int32_t ox, oy;
    bool have = tree_ext(s, &ext);
    lm_surface_offset_in_root(s, &ox, &oy);
    bool was_sub = s->sub != NULL;

    struct lm_subsurface *sub, *tmp;
    lm_list_for_each_safe(sub, tmp, &p->sub_list, link) {
        if (sub->is_self)
            continue;
        struct lm_surface *c = sub->child;
        sub_detach(sub);
        if (c) {
            c->role_data = NULL;
            lm_surface_clear_role(c);
        }
    }
    if (s->sub) {
        sub_detach(s->sub);
        if (have && was_sub)
            report_in_root(root, ox, oy, &ext);
    }
    lm_list_remove(&p->self.link);
    lm_list_remove(&p->self.pending_link);

    destroy_callbacks(&p->pending.frames);
    destroy_callbacks(&p->cache.frames);
    destroy_callbacks(&p->frames);

    struct out_entry *e, *etmp;
    lm_list_for_each_safe(e, etmp, &p->outputs, link)
        out_entry_free(e);

    state_fini(&p->pending);
    state_fini(&p->cache);
    lm_region_fini(&p->opaque_raw);
    lm_region_fini(&s->opaque);
    lm_region_fini(&s->input);
    lm_region_fini(&s->gl_damage);
    lm_texture_free(&s->tex);
    lm_list_remove(&s->link);
    free(p);
}

static struct surf *surface_new(struct lm_compositor *comp, struct lm_server *srv)
{
    struct surf *p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    struct lm_surface *s = &p->pub;

    s->server = srv;
    s->scale = 1;
    s->input_infinite = true;
    s->priv = p;
    lm_list_init(&s->link);
    lm_region_init(&s->opaque);
    lm_region_init(&s->input);
    lm_region_init(&s->gl_damage);
    s->gl_full = true;
    lm_signal_init(&s->destroy_signal);
    lm_signal_init(&s->commit_signal);

    p->comp = comp;
    state_init(&p->pending);
    state_init(&p->cache);
    lm_region_init(&p->opaque_raw);
    lm_list_init(&p->frames);
    lm_list_init(&p->outputs);
    lm_list_init(&p->sub_list);
    lm_list_init(&p->sub_pending);
    p->self.is_self = true;
    lm_list_init(&p->self.link);
    lm_list_init(&p->self.pending_link);
    lm_list_append(&p->sub_list, &p->self.link);
    lm_list_append(&p->sub_pending, &p->self.pending_link);
    return p;
}

struct lm_surface *lm_surface_from_resource(struct lm_resource *res)
{
    if (!res || res->iface != &lm_wl_surface_interface)
        return NULL;
    return res->data;
}

/* ---- wl_compositor ------------------------------------------------------------------------ */

static void comp_req_create_surface(struct lm_resource *res, const union lm_arg *args)
{
    struct comp_bind *bind = res->data;
    if (!bind || !bind->comp)
        return;
    struct lm_compositor *comp = bind->comp;
    struct surf *p = surface_new(comp, comp->srv);
    if (!p) {
        lm_resource_post_no_memory(res);
        return;
    }
    struct lm_resource *sr = lm_resource_create(res->client, args[0].n, &lm_wl_surface_interface,
                                                res->version, surface_handlers, &p->pub,
                                                surface_destroy_hook);
    if (!sr) {
        state_fini(&p->pending);
        state_fini(&p->cache);
        free(p);
        return;
    }
    p->pub.resource = sr;
    lm_list_append(&comp->surfaces, &p->pub.link);
}

static void comp_req_create_region(struct lm_resource *res, const union lm_arg *args)
{
    struct region *r = calloc(1, sizeof(*r));
    if (!r) {
        lm_resource_post_no_memory(res);
        return;
    }
    lm_region_init(&r->r);
    struct lm_resource *rr = lm_resource_create(res->client, args[0].n, &lm_wl_region_interface,
                                                res->version, region_handlers, r,
                                                region_resource_destroy);
    if (!rr)
        free(r);
}

static const lm_request_fn compositor_handlers[] = {
    [LM_WL_COMPOSITOR_REQ_CREATE_SURFACE] = comp_req_create_surface,
    [LM_WL_COMPOSITOR_REQ_CREATE_REGION] = comp_req_create_region,
};

static void comp_bind_destroy(struct lm_resource *res)
{
    struct comp_bind *bind = res->data;
    res->data = NULL;
    if (!bind)
        return;
    lm_list_remove(&bind->link);
    free(bind);
}

static void compositor_bind(struct lm_client *client, void *data, uint32_t version, uint32_t id)
{
    struct lm_compositor *comp = data;
    struct comp_bind *bind = calloc(1, sizeof(*bind));
    if (!bind) {
        lm_client_post_no_memory(client);
        return;
    }
    bind->comp = comp;
    lm_list_init(&bind->link);
    struct lm_resource *res = lm_resource_create(client, id, &lm_wl_compositor_interface, version,
                                                 compositor_handlers, bind, comp_bind_destroy);
    if (!res) {
        free(bind);
        return;
    }
    lm_list_append(&comp->binds, &bind->link);
}

/* ---- wl_subcompositor ------------------------------------------------------------------- */

static void subcomp_req_destroy(struct lm_resource *res, const union lm_arg *args)
{
    (void)args;
    lm_resource_destroy(res);
}

static void subcomp_req_get_subsurface(struct lm_resource *res, const union lm_arg *args)
{
    uint32_t id = args[0].n;
    struct lm_surface *child = lm_surface_from_resource(args[1].r);
    struct lm_surface *parent = lm_surface_from_resource(args[2].r);

    if (!child || !parent) {
        lm_resource_post_error(res, LM_WL_SUBCOMPOSITOR_ERR_BAD_SURFACE, "invalid surface");
        return;
    }
    if (child == parent) {
        lm_resource_post_error(res, LM_WL_SUBCOMPOSITOR_ERR_BAD_SURFACE,
                               "a surface cannot be its own parent");
        return;
    }
    if (!role_compatible(child, &sub_role) || child->role_data) {
        lm_resource_post_error(res, LM_WL_SUBCOMPOSITOR_ERR_BAD_SURFACE,
                               "surface already has a role (%s)",
                               child->role ? child->role->name : "?");
        return;
    }
    /* no cycles: the child must not be an ancestor of the parent */
    struct lm_surface *a = parent;
    for (int guard = 0; a && guard < 4 * LM_MAX_SUB_DEPTH; guard++) {
        if (a == child) {
            lm_resource_post_error(res, LM_WL_SUBCOMPOSITOR_ERR_BAD_SURFACE,
                                   "parent is a descendant of the surface");
            return;
        }
        a = (a->sub && a->sub->parent) ? a->sub->parent : NULL;
    }
    if (sub_depth_up(parent) + 1 + sub_height(child) > LM_MAX_SUB_DEPTH) {
        lm_resource_post_error(res, LM_WL_SUBCOMPOSITOR_ERR_BAD_SURFACE,
                               "subsurface tree too deep");
        return;
    }

    struct lm_subsurface *sub = calloc(1, sizeof(*sub));
    if (!sub) {
        lm_resource_post_no_memory(res);
        return;
    }
    lm_list_init(&sub->link);
    lm_list_init(&sub->pending_link);
    sub->sync = true;

    struct lm_resource *sr = lm_resource_create(res->client, id, &lm_wl_subsurface_interface,
                                                res->version, subsurface_handlers, sub,
                                                subsurface_resource_destroy);
    if (!sr) {
        free(sub);
        return;
    }
    sub->resource = sr;
    sub->child = child;
    sub->parent = parent;
    lm_surface_set_role(child, &sub_role, sub); /* cannot fail: checked above */
    child->sub = sub;
    lm_list_append(&SURF(parent)->sub_list, &sub->link);
    lm_list_append(&SURF(parent)->sub_pending, &sub->pending_link);
}

static const lm_request_fn subcompositor_handlers[] = {
    [LM_WL_SUBCOMPOSITOR_REQ_DESTROY] = subcomp_req_destroy,
    [LM_WL_SUBCOMPOSITOR_REQ_GET_SUBSURFACE] = subcomp_req_get_subsurface,
};

static void subcompositor_bind(struct lm_client *client, void *data, uint32_t version, uint32_t id)
{
    (void)data;
    lm_resource_create(client, id, &lm_wl_subcompositor_interface, version,
                       subcompositor_handlers, NULL, NULL);
}

/* ---- module lifecycle --------------------------------------------------------------------- */

struct lm_compositor *lm_compositor_create(struct lm_server *srv)
{
    struct lm_compositor *comp = calloc(1, sizeof(*comp));
    if (!comp)
        return NULL;
    comp->srv = srv;
    lm_list_init(&comp->surfaces);
    lm_list_init(&comp->binds);
    comp->comp_global = lm_global_create(srv, &lm_wl_compositor_interface,
                                         LM_WL_COMPOSITOR_VERSION, compositor_bind, comp);
    comp->sub_global = lm_global_create(srv, &lm_wl_subcompositor_interface,
                                        LM_WL_SUBCOMPOSITOR_VERSION, subcompositor_bind, comp);
    if (!comp->comp_global || !comp->sub_global) {
        lm_compositor_destroy(comp);
        return NULL;
    }
    return comp;
}

void lm_compositor_destroy(struct lm_compositor *comp)
{
    if (!comp)
        return;
    if (comp->comp_global)
        lm_global_destroy(comp->comp_global);
    if (comp->sub_global)
        lm_global_destroy(comp->sub_global);

    /*
     * Clients (and therefore surfaces) outlive this module during shutdown.
     * Detach them so their destroy hooks never touch the freed compositor and
     * never call into other modules that are already gone ("quiet" surfaces).
     */
    struct lm_surface *s, *stmp;
    lm_list_for_each_safe(s, stmp, &comp->surfaces, link) {
        SURF(s)->comp = NULL;
        lm_list_remove(&s->link);
    }
    struct comp_bind *b, *btmp;
    lm_list_for_each_safe(b, btmp, &comp->binds, link) {
        b->comp = NULL;
        lm_list_remove(&b->link);
    }
    free(comp);
}
