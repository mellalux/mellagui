/*
 * wl_output globals (one per backend output) and the output layout.
 *
 * Layout policy: outputs are packed left to right at y = 0 in the order they
 * appear in backend->outputs. The wl_output resource's data pointer is the
 * struct lm_output (NULL once the output is gone), as protocol.h promises.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "backend.h"
#include "log.h"
#include "output.h"
#include "protocol.h"
#include "server.h"

struct lm_output_mgr {
    struct lm_server *srv;
    struct lm_list globals;             /* struct out_global.link */
};

/* Per-output state, hung off lm_output.output_data. */
struct out_global {
    struct lm_list link;                /* in mgr->globals */
    struct lm_output *out;
    struct lm_global *global;
    struct lm_list bound;               /* struct out_bound.link */
};

struct out_bound {
    struct lm_list link;                /* in out_global.bound */
    struct lm_resource *res;
};

#define OUT_EPS (1.0 / 256.0)

static struct out_global *og_of(const struct lm_output *out)
{
    return out ? out->output_data : NULL;
}

/* ---- events -------------------------------------------------------------------- */

static void send_description(struct lm_resource *res, const struct lm_output *out)
{
    const char *make = out->make[0] ? out->make : "unknown";
    const char *model = out->model[0] ? out->model : "unknown";
    int32_t scale = out->scale >= 1 ? out->scale : 1;

    lm_resource_post(res, LM_WL_OUTPUT_EVT_GEOMETRY, out->x, out->y,
                     out->phys_width_mm, out->phys_height_mm,
                     (int32_t)LM_OUTPUT_SUBPIXEL_UNKNOWN, make, model,
                     (int32_t)(out->transform & 7));
    lm_resource_post(res, LM_WL_OUTPUT_EVT_MODE,
                     (uint32_t)(LM_OUTPUT_MODE_CURRENT | LM_OUTPUT_MODE_PREFERRED),
                     out->width, out->height, out->refresh_mhz);
    /* scale and done only exist from version 2 on; lm_resource_post filters them. */
    lm_resource_post(res, LM_WL_OUTPUT_EVT_SCALE, scale);
    lm_resource_post(res, LM_WL_OUTPUT_EVT_DONE);
}

static void announce(struct lm_output *out)
{
    struct out_global *og = og_of(out);
    if (!og)
        return;
    struct out_bound *b;
    lm_list_for_each(b, &og->bound, link)
        send_description(b->res, out);
}

/* ---- layout ----------------------------------------------------------------------- */

/*
 * Re-pack all outputs (except `skip`) left to right. Outputs whose position
 * changed, other than `except_announce`, get their new geometry sent.
 */
/* Position of `name` in the comma separated `order` (0 = leftmost); outputs it does not name rank after all that it does. */
static int order_rank(const char *order, const char *name)
{
    size_t nl = strlen(name);
    int idx = 0;
    for (const char *p = order; *p;) {
        const char *e = strchr(p, ',');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n == nl && !strncmp(p, name, n))
            return idx;
        idx++;
        if (!e)
            break;
        p = e + 1;
    }
    return 1000;
}

static void layout_pack(struct lm_output_mgr *mgr, const struct lm_output *skip,
                        struct lm_output *except_announce)
{
    struct lm_backend *be = mgr->srv->backend;
    if (!be)
        return;
    struct lm_output *arr[16];
    int n = 0;
    struct lm_output *o;
    lm_list_for_each(o, &be->outputs, link)
        if (o != skip && n < 16)
            arr[n++] = o;
    const char *order = mgr->srv->config.output_order;
    for (int i = 1; i < n; i++) {                                      /* stable insertion sort by rank: the order setting, then connection order */
        struct lm_output *t = arr[i];
        int j = i;
        while (j > 0 && order_rank(order, arr[j - 1]->name) > order_rank(order, t->name)) {
            arr[j] = arr[j - 1];
            j--;
        }
        arr[j] = t;
    }
    int64_t cursor = 0;
    for (int i = 0; i < n; i++) {
        o = arr[i];
        int32_t nx = (int32_t)LM_CLAMP(cursor, 0, (int64_t)LM_COORD_LIMIT);
        bool moved = o->x != nx || o->y != 0;
        o->x = nx;
        o->y = 0;
        if (o->width > 0)
            cursor += o->width;
        if (moved && o != except_announce)
            announce(o);
    }
}

static bool output_in_list(struct lm_backend *be, const struct lm_output *out)
{
    struct lm_output *o;
    lm_list_for_each(o, &be->outputs, link)
        if (o == out)
            return true;
    return false;
}

/* ---- wl_output resource -------------------------------------------------------------- */

static void output_release(struct lm_resource *res, const union lm_arg *args)
{
    (void)args;
    lm_resource_destroy(res);
}

static const lm_request_fn output_handlers[] = {
    [LM_WL_OUTPUT_REQ_RELEASE] = output_release,
};

static void output_resource_destroy(struct lm_resource *res)
{
    struct lm_output *out = res->data;
    struct out_global *og = og_of(out);
    if (!og)
        return; /* output already gone; its bound list was released */
    struct out_bound *b, *tmp;
    lm_list_for_each_safe(b, tmp, &og->bound, link) {
        if (b->res == res) {
            lm_list_remove(&b->link);
            free(b);
            return;
        }
    }
}

static void output_bind(struct lm_client *client, void *data, uint32_t version, uint32_t id)
{
    struct lm_output *out = data;
    struct out_global *og = og_of(out);
    if (!og) {
        lm_client_post_no_memory(client);
        return;
    }
    struct lm_resource *res = lm_resource_create(client, id, &lm_wl_output_interface, version,
                                                 output_handlers, out, output_resource_destroy);
    if (!res)
        return;
    struct out_bound *b = calloc(1, sizeof(*b));
    if (!b) {
        lm_resource_post_no_memory(res);
        return;
    }
    b->res = res;
    lm_list_append(&og->bound, &b->link);
    send_description(res, out);
}

/* ---- manager ---------------------------------------------------------------------------- */

struct lm_output_mgr *lm_output_mgr_create(struct lm_server *srv)
{
    struct lm_output_mgr *mgr = calloc(1, sizeof(*mgr));
    if (!mgr)
        return NULL;
    mgr->srv = srv;
    lm_list_init(&mgr->globals);
    return mgr;
}

static void og_release(struct out_global *og)
{
    struct out_bound *b, *tmp;
    lm_list_for_each_safe(b, tmp, &og->bound, link) {
        b->res->data = NULL;
        lm_list_remove(&b->link);
        free(b);
    }
    if (og->global)
        lm_global_destroy(og->global);
    if (og->out)
        og->out->output_data = NULL;
    lm_list_remove(&og->link);
    free(og);
}

void lm_output_mgr_destroy(struct lm_output_mgr *mgr)
{
    if (!mgr)
        return;
    struct out_global *og, *tmp;
    lm_list_for_each_safe(og, tmp, &mgr->globals, link)
        og_release(og);
    free(mgr);
}

void lm_output_mgr_add(struct lm_output_mgr *mgr, struct lm_output *out)
{
    if (!mgr || !out)
        return;
    struct out_global *og = calloc(1, sizeof(*og));
    if (!og) {
        lm_error("out of memory advertising output %s", out->name);
        return;
    }
    og->out = out;
    lm_list_init(&og->bound);
    lm_list_append(&mgr->globals, &og->link);
    out->output_data = og;

    if (out->scale < 1)
        out->scale = 1;

    /* Position: pack the list; an output the backend has not linked yet goes to the end. */
    struct lm_backend *be = mgr->srv->backend;
    if (be && !output_in_list(be, out)) {
        int64_t right = 0;
        struct lm_output *o;
        lm_list_for_each(o, &be->outputs, link)
            if (o->width > 0)
                right = LM_MAX(right, (int64_t)o->x + o->width);
        out->x = (int32_t)LM_CLAMP(right, 0, (int64_t)LM_COORD_LIMIT);
        out->y = 0;
    } else {
        layout_pack(mgr, NULL, out);
    }

    og->global = lm_global_create(mgr->srv, &lm_wl_output_interface, LM_WL_OUTPUT_VERSION,
                                  output_bind, out);
    if (!og->global)
        lm_error("could not advertise wl_output for %s", out->name);
    lm_info("output %s: %dx%d@%.3f at %d,%d", out->name, out->width, out->height,
            out->refresh_mhz / 1000.0, out->x, out->y);
}

void lm_output_mgr_relayout(struct lm_output_mgr *mgr)
{
    if (mgr)
        layout_pack(mgr, NULL, NULL);
}

struct lm_output *lm_output_primary(struct lm_server *srv)
{
    struct lm_output *best = NULL, *o;
    if (!srv->backend)
        return NULL;
    lm_list_for_each(o, &srv->backend->outputs, link)
        if (!best || o->x < best->x || (o->x == best->x && o->y < best->y))
            best = o;
    return best;
}

void lm_output_mgr_changed(struct lm_output_mgr *mgr, struct lm_output *out)
{
    if (!mgr || !out || !og_of(out))
        return;
    if (out->scale < 1)
        out->scale = 1;
    layout_pack(mgr, NULL, out);
    announce(out);
}

void lm_output_mgr_remove(struct lm_output_mgr *mgr, struct lm_output *out)
{
    struct out_global *og = og_of(out);
    if (!mgr || !og)
        return;
    og_release(og);
    layout_pack(mgr, out, NULL);
}

struct lm_resource *lm_output_resource_for_client(struct lm_output *out, struct lm_client *client)
{
    struct out_global *og = og_of(out);
    if (!og || !client)
        return NULL;
    struct out_bound *b;
    lm_list_for_each(b, &og->bound, link)
        if (b->res->client == client)
            return b->res;
    return NULL;
}

/* ---- layout queries ------------------------------------------------------------------------ */

static struct lm_backend *backend_of(struct lm_server *srv)
{
    return srv ? srv->backend : NULL;
}

bool lm_output_layout_bounds(struct lm_server *srv, struct lm_rect *out)
{
    struct lm_backend *be = backend_of(srv);
    if (!be)
        return false;
    bool any = false;
    struct lm_rect acc = { 0, 0, 0, 0 };
    struct lm_output *o;
    lm_list_for_each(o, &be->outputs, link) {
        struct lm_rect r = lm_output_rect(o);
        if (lm_rect_is_empty(&r))
            continue;
        acc = any ? lm_rect_union(&acc, &r) : r;
        any = true;
    }
    if (any && out)
        *out = acc;
    return any;
}

static double axis_distance(double v, double lo, double hi)
{
    if (v < lo)
        return lo - v;
    if (v > hi)
        return v - hi;
    return 0.0;
}

struct lm_output *lm_output_nearest(struct lm_server *srv, double x, double y)
{
    struct lm_backend *be = backend_of(srv);
    if (!be)
        return NULL;
    struct lm_output *best = NULL;
    double best_d = 0.0;
    struct lm_output *o;
    lm_list_for_each(o, &be->outputs, link) {
        if (o->width <= 0 || o->height <= 0)
            continue;
        double dx = axis_distance(x, o->x, (double)o->x + o->width);
        double dy = axis_distance(y, o->y, (double)o->y + o->height);
        double d = dx * dx + dy * dy;
        if (!best || d < best_d) {
            best = o;
            best_d = d;
        }
    }
    return best;
}

void lm_output_clamp_point(struct lm_server *srv, double *x, double *y)
{
    if (!x || !y)
        return;
    if (!isfinite(*x))
        *x = 0.0;
    if (!isfinite(*y))
        *y = 0.0;
    struct lm_backend *be = backend_of(srv);
    if (!be)
        return;
    /* Already inside an output (as a real number, not a pixel index): nothing to do. */
    struct lm_output *o;
    lm_list_for_each(o, &be->outputs, link) {
        if (o->width <= 0 || o->height <= 0)
            continue;
        if (*x >= o->x && *y >= o->y && *x < (double)o->x + o->width &&
            *y < (double)o->y + o->height)
            return;
    }
    struct lm_output *n = lm_output_nearest(srv, *x, *y);
    if (!n)
        return;
    *x = LM_CLAMP(*x, (double)n->x, (double)n->x + n->width - OUT_EPS);
    *y = LM_CLAMP(*y, (double)n->y, (double)n->y + n->height - OUT_EPS);
}
