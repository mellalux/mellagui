#include "idleinhibit.h"

#include <stdlib.h>

#include "list.h"
#include "protocol.h"
#include "server.h"
#include "surface.h"
#include "wm.h"

struct inhibitor {
    struct lm_list link;                /* in lm_idle_inhibit.list while the compositor lives */
    struct lm_idle_inhibit *owner;
    struct lm_surface *surface;         /* NULL once it was destroyed */
    struct lm_listener gone;            /* the surface's destroy_signal */
};

struct lm_idle_inhibit {
    struct lm_server *srv;
    struct lm_global *global;
    struct lm_list list;
};

static void surface_gone(struct lm_listener *l, void *data)
{
    (void)data;
    struct inhibitor *in = lm_container_of(l, struct inhibitor, gone);
    lm_listener_remove(&in->gone);
    lm_list_init(&in->gone.link);
    in->surface = NULL;
}

static void inhibitor_destroy(struct lm_resource *res)
{
    struct inhibitor *in = res->data;
    if (!in)
        return;
    res->data = NULL;
    lm_listener_remove(&in->gone);
    if (in->owner)
        lm_list_remove(&in->link);
    free(in);
}

static void inhibitor_req_destroy(struct lm_resource *res, const union lm_arg *args)
{
    (void)args;
    lm_resource_destroy(res);
}

static const lm_request_fn inhibitor_handlers[] = {
    [LM_ZWP_IDLE_INHIBITOR_V1_REQ_DESTROY] = inhibitor_req_destroy,
};

static void mgr_req_destroy(struct lm_resource *res, const union lm_arg *args)
{
    (void)args;
    lm_resource_destroy(res);
}

static void mgr_req_create_inhibitor(struct lm_resource *res, const union lm_arg *args)
{
    struct lm_idle_inhibit *ii = res->data;
    struct lm_surface *s = lm_surface_from_resource(args[1].r);
    struct inhibitor *in = calloc(1, sizeof(*in));
    if (!in) {
        lm_resource_post_no_memory(res);
        return;
    }
    lm_list_init(&in->link);
    lm_list_init(&in->gone.link);
    if (!lm_resource_create(res->client, args[0].n, &lm_zwp_idle_inhibitor_v1_interface, res->version, inhibitor_handlers, in,
                            inhibitor_destroy)) {
        free(in);
        return;
    }
    if (!ii)
        return;                                                 /* the compositor is going away: inert */
    in->owner = ii;
    in->surface = s;
    if (s) {
        in->gone.notify = surface_gone;
        lm_signal_add(&s->destroy_signal, &in->gone);
    }
    lm_list_append(&ii->list, &in->link);
}

static const lm_request_fn mgr_handlers[] = {
    [LM_ZWP_IDLE_INHIBIT_MANAGER_V1_REQ_DESTROY] = mgr_req_destroy,
    [LM_ZWP_IDLE_INHIBIT_MANAGER_V1_REQ_CREATE_INHIBITOR] = mgr_req_create_inhibitor,
};

static void mgr_bind(struct lm_client *client, void *data, uint32_t version, uint32_t id)
{
    lm_resource_create(client, id, &lm_zwp_idle_inhibit_manager_v1_interface, version, mgr_handlers, data, NULL);
}

struct lm_idle_inhibit *lm_idle_inhibit_create(struct lm_server *srv)
{
    struct lm_idle_inhibit *ii = calloc(1, sizeof(*ii));
    if (!ii)
        return NULL;
    ii->srv = srv;
    lm_list_init(&ii->list);
    ii->global = lm_global_create(srv, &lm_zwp_idle_inhibit_manager_v1_interface, LM_ZWP_IDLE_INHIBIT_MANAGER_V1_VERSION, mgr_bind, ii);
    if (!ii->global) {
        free(ii);
        return NULL;
    }
    return ii;
}

void lm_idle_inhibit_destroy(struct lm_idle_inhibit *ii)
{
    if (!ii)
        return;
    struct inhibitor *in, *tmp;
    lm_list_for_each_safe(in, tmp, &ii->list, link) {
        lm_list_remove(&in->link);
        lm_list_init(&in->link);
        in->owner = NULL;                                       /* the resources live on until their clients go */
        in->surface = NULL;
        lm_listener_remove(&in->gone);
        lm_list_init(&in->gone.link);
    }
    if (ii->global)
        lm_global_destroy(ii->global);
    free(ii);
}

bool lm_idle_inhibited(const struct lm_idle_inhibit *ii)
{
    if (!ii || !ii->srv->wm)
        return false;
    const struct inhibitor *in;
    lm_list_for_each(in, &ii->list, link)
        if (in->surface && lm_wm_surface_visible(ii->srv->wm, in->surface))
            return true;
    return false;
}

int lm_idle_inhibit_count(const struct lm_idle_inhibit *ii)
{
    int n = 0;
    const struct inhibitor *in;
    if (ii)
        lm_list_for_each(in, &ii->list, link)
            n++;
    return n;
}
