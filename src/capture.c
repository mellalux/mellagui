#include "capture.h"

#include <stdlib.h>
#include <string.h>

#include "backend.h"
#include "lock.h"
#include "log.h"
#include "output.h"
#include "protocol.h"
#include "render.h"
#include "settings.h"
#include "shm.h"

struct binding {
    struct lm_list link;                /* in lm_capture.bindings, always linked */
    struct lm_capture *cap;             /* NULL: inert (not one of our programs) or the compositor is going away */
};

struct lm_capture {
    struct lm_server *srv;
    struct lm_global *global;
    struct lm_list bindings;
};

static struct lm_output *output_at(struct lm_server *srv, int index)
{
    if (!srv->backend || index < 0)
        return NULL;
    int i = 0;
    struct lm_output *o;
    lm_list_for_each(o, &srv->backend->outputs, link) {
        if (i++ == index)
            return o;
    }
    return NULL;
}

static void req_destroy(struct lm_resource *res, const union lm_arg *args)
{
    (void)args;
    lm_resource_destroy(res);
}

static void req_list(struct lm_resource *res, const union lm_arg *args)
{
    (void)args;
    struct binding *b = res->data;
    if (b && b->cap && b->cap->srv->backend) {
        int i = 0;
        struct lm_output *o;
        lm_list_for_each(o, &b->cap->srv->backend->outputs, link) {
            lm_resource_post(res, LM_MELLAGUI_CAPTURE_V1_EVT_OUTPUT, i, o->name, o->x, o->y, o->width, o->height);
            i++;
        }
    }
    lm_resource_post(res, LM_MELLAGUI_CAPTURE_V1_EVT_DONE);
}

static void req_grab(struct lm_resource *res, const union lm_arg *args)
{
    struct binding *b = res->data;
    int32_t index = args[0].i;
    if (!b || !b->cap) {
        lm_resource_post(res, LM_MELLAGUI_CAPTURE_V1_EVT_FAILED, index, "no-output");
        return;
    }
    struct lm_server *srv = b->cap->srv;
    struct lm_output *out = output_at(srv, index);
    struct lm_buffer *buf = args[1].r ? lm_buffer_from_resource(args[1].r) : NULL;
    const char *why = NULL;
    if (!out)
        why = "no-output";
    else if (!buf || !buf->pool || (buf->format != LM_WL_SHM_FORMAT_XRGB8888 && buf->format != LM_WL_SHM_FORMAT_ARGB8888))
        why = "buffer";
    else if (buf->width != out->width || buf->height != out->height)
        why = "size";
    else if (srv->lock && lm_lock_active(srv->lock))
        why = "locked";
    if (why) {
        lm_resource_post(res, LM_MELLAGUI_CAPTURE_V1_EVT_FAILED, index, why);
        return;
    }
    struct lm_rect r = lm_output_rect(out);
    int w = 0, h = 0;
    uint32_t *px = srv->render ? lm_render_capture_cursor(srv->render, &r, &w, &h, true) : NULL;     /* the pointer is in the picture */
    if (!px || lm_buffer_write(buf, px, w, h) < 0) {
        free(px);
        lm_resource_post(res, LM_MELLAGUI_CAPTURE_V1_EVT_FAILED, index, "capture");
        return;
    }
    free(px);
    lm_resource_post(res, LM_MELLAGUI_CAPTURE_V1_EVT_READY, index, w, h);
}

static const lm_request_fn handlers[] = {
    [LM_MELLAGUI_CAPTURE_V1_REQ_DESTROY] = req_destroy,
    [LM_MELLAGUI_CAPTURE_V1_REQ_LIST] = req_list,
    [LM_MELLAGUI_CAPTURE_V1_REQ_GRAB] = req_grab,
};

static void binding_destroy(struct lm_resource *res)
{
    struct binding *b = res->data;
    if (b)
        lm_list_remove(&b->link);
    free(b);
    res->data = NULL;
}

static void capture_bind(struct lm_client *client, void *data, uint32_t version, uint32_t id)
{
    struct lm_capture *c = data;
    struct binding *b = calloc(1, sizeof(*b));
    if (!b) {
        lm_client_post_no_memory(client);
        return;
    }
    if (lm_settings_client_allowed(c->srv, client))
        b->cap = c;
    else
        lm_warn("capture: client pid %d is not one of MellaGUI's programs, the screen stays closed to it", (int)client->pid);
    lm_list_append(&c->bindings, &b->link);
    if (!lm_resource_create(client, id, &lm_mellagui_capture_v1_interface, version, handlers, b, binding_destroy)) {
        lm_list_remove(&b->link);
        free(b);
    }
}

struct lm_capture *lm_capture_create(struct lm_server *srv)
{
    struct lm_capture *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->srv = srv;
    lm_list_init(&c->bindings);
    c->global = lm_global_create(srv, &lm_mellagui_capture_v1_interface, LM_MELLAGUI_CAPTURE_V1_VERSION, capture_bind, c);
    if (!c->global) {
        free(c);
        return NULL;
    }
    return c;
}

void lm_capture_destroy(struct lm_capture *c)
{
    if (!c)
        return;
    struct binding *b;
    lm_list_for_each(b, &c->bindings, link)
        b->cap = NULL;          /* the resources live on until their clients go */
    if (c->global)
        lm_global_destroy(c->global);
    free(c);
}
