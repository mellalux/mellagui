#include "screencopy.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "backend.h"
#include "list.h"
#include "lock.h"
#include "log.h"
#include "loop.h"
#include "output.h"
#include "protocol.h"
#include "render.h"
#include "server.h"
#include "settings.h"
#include "shm.h"

#define SEEN_MAX 16

struct frame {
    struct lm_list link;                /* in lm_screencopy.pending while it waits for a picture */
    struct lm_list alink;               /* in lm_screencopy.all as long as the frame object lives */
    bool dead;                          /* it has failed: a late copy is ignored, not an error */
    struct lm_screencopy *sc;           /* NULL: the compositor is going away */
    struct lm_resource *res;
    struct lm_client *client;
    struct lm_output *out;              /* NULL once the output went away */
    struct lm_rect rect;                /* what was asked for, global logical coordinates, inside the output */
    int32_t w, h;                       /* the buffer it needs, in pixels */
    bool cursor;
    bool queued;                        /* in `pending` */
    bool used;                          /* copy was called */
    bool with_damage;
    struct lm_buffer *buf;              /* the client's buffer (a reference), set by copy */
};

struct seen {                           /* the damage serial of the last frame a client got of an output */
    struct lm_client *client;
    struct lm_output *out;
    uint64_t serial;
};

struct lm_screencopy {
    struct lm_server *srv;
    struct lm_global *global;
    struct lm_list pending;
    struct lm_list all;
    struct lm_timer *timer;
    struct seen seen[SEEN_MAX];
    int seen_next;
    int n_pending, delivered, failed;
};

static void arm(struct lm_screencopy *sc, uint32_t ms)
{
    if (sc->timer && !lm_timer_is_armed(sc->timer))
        lm_timer_arm_ms(sc->timer, ms, 0);
}

static bool output_alive(struct lm_screencopy *sc, const struct lm_output *out)
{
    if (!out || !sc->srv->backend)
        return false;
    struct lm_output *o;
    lm_list_for_each(o, &sc->srv->backend->outputs, link)
        if (o == out)
            return true;
    return false;
}

static struct seen *seen_find(struct lm_screencopy *sc, struct lm_client *c, struct lm_output *out, bool create)
{
    for (int i = 0; i < SEEN_MAX; i++)
        if (sc->seen[i].client == c && sc->seen[i].out == out)
            return &sc->seen[i];
    if (!create)
        return NULL;
    struct seen *s = &sc->seen[sc->seen_next];
    sc->seen_next = (sc->seen_next + 1) % SEEN_MAX;
    s->client = c;
    s->out = out;
    s->serial = 0;
    return s;
}

static void dequeue(struct frame *f)
{
    if (f->queued) {
        lm_list_remove(&f->link);
        f->queued = false;
        if (f->sc)
            f->sc->n_pending--;
    }
}

static void frame_failed(struct frame *f)
{
    dequeue(f);
    if (f->buf) {
        lm_buffer_unref(f->buf);
        f->buf = NULL;
    }
    if (f->sc)
        f->sc->failed++;
    f->used = true;                                              /* nothing more can be done with it */
    f->dead = true;
    lm_resource_post(f->res, LM_ZWLR_SCREENCOPY_FRAME_V1_EVT_FAILED);
}

/* Paints the frame into its buffer and tells the client. */
static void frame_deliver(struct frame *f)
{
    struct lm_screencopy *sc = f->sc;
    struct lm_server *srv = sc->srv;
    dequeue(f);
    struct lm_rect clip;
    int ew = 0, eh = 0;
    if (!output_alive(sc, f->out) || !srv->render || !f->buf || !f->buf->resource ||
        !lm_render_capture_extent(f->out, &f->rect, &clip, &ew, &eh) || ew != f->w || eh != f->h ||
        (srv->lock && lm_lock_active(srv->lock))) {
        frame_failed(f);                                         /* the output went, changed its mode, or the screen is locked */
        return;
    }
    int w = 0, h = 0;
    bool whole = f->cursor && clip.w == f->out->width && clip.h == f->out->height;
    uint32_t *px = lm_render_capture_cursor(srv->render, &clip, &w, &h, whole);
    if (!px || w != f->w || h != f->h || lm_buffer_write(f->buf, px, w, h) < 0) {
        free(px);
        frame_failed(f);
        return;
    }
    free(px);
    lm_buffer_unref(f->buf);
    f->buf = NULL;
    struct seen *s = seen_find(sc, f->client, f->out, true);
    s->serial = lm_render_damage_serial(srv->render, f->out);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    lm_resource_post(f->res, LM_ZWLR_SCREENCOPY_FRAME_V1_EVT_FLAGS, 0u);
    if (f->with_damage)
        lm_resource_post(f->res, LM_ZWLR_SCREENCOPY_FRAME_V1_EVT_DAMAGE, 0u, 0u, (uint32_t)w, (uint32_t)h);
    lm_resource_post(f->res, LM_ZWLR_SCREENCOPY_FRAME_V1_EVT_READY, (uint32_t)((uint64_t)ts.tv_sec >> 32), (uint32_t)ts.tv_sec,
                     (uint32_t)ts.tv_nsec);
    sc->delivered++;
}

static bool frame_ready_to_go(struct lm_screencopy *sc, struct frame *f)
{
    if (!f->with_damage || !output_alive(sc, f->out) || !sc->srv->render)
        return true;                                             /* (a dead output fails at once) */
    struct seen *s = seen_find(sc, f->client, f->out, false);
    return !s || s->serial != lm_render_damage_serial(sc->srv->render, f->out);
}

static void process(struct lm_screencopy *sc)
{
    struct frame *f, *tmp;
    lm_list_for_each_safe(f, tmp, &sc->pending, link)
        if (frame_ready_to_go(sc, f))
            frame_deliver(f);
}

static void timer_cb(void *data)
{
    process(data);
}

void lm_screencopy_damaged(struct lm_screencopy *sc)
{
    if (sc && sc->n_pending > 0)
        arm(sc, 16);                                             /* one picture per ~frame however much damage there is */
}

void lm_screencopy_output_removed(struct lm_screencopy *sc, struct lm_output *out)
{
    if (!sc)
        return;
    struct frame *f, *tmp;
    lm_list_for_each_safe(f, tmp, &sc->pending, link) {
        if (f->out == out) {
            f->out = NULL;
            frame_failed(f);
        }
    }
    for (int i = 0; i < SEEN_MAX; i++)
        if (sc->seen[i].out == out)
            memset(&sc->seen[i], 0, sizeof(sc->seen[i]));
}

/* ---- zwlr_screencopy_frame_v1 ------------------------------------------------------------------------ */

static void frame_destroy(struct lm_resource *res)
{
    struct frame *f = res->data;
    if (!f)
        return;
    res->data = NULL;
    dequeue(f);
    lm_list_remove(&f->alink);
    if (f->buf)
        lm_buffer_unref(f->buf);
    free(f);
}

static void frame_req_destroy(struct lm_resource *res, const union lm_arg *args)
{
    (void)args;
    lm_resource_destroy(res);
}

static void frame_copy_common(struct lm_resource *res, struct lm_resource *bufres, bool with_damage)
{
    struct frame *f = res->data;
    if (!f || f->dead)
        return;
    if (f->used) {
        lm_resource_post_error(res, LM_ZWLR_SCREENCOPY_FRAME_ERR_ALREADY_USED, "the frame was copied already");
        return;
    }
    struct lm_buffer *b = lm_buffer_from_resource(bufres);
    if (!b || !b->pool || b->dmabuf || (b->format != LM_WL_SHM_FORMAT_XRGB8888 && b->format != LM_WL_SHM_FORMAT_ARGB8888) ||
        b->width != f->w || b->height != f->h || b->stride < f->w * 4) {
        lm_resource_post_error(res, LM_ZWLR_SCREENCOPY_FRAME_ERR_INVALID_BUFFER, "the buffer is not a %dx%d XRGB8888 wl_shm buffer", f->w, f->h);
        return;
    }
    f->used = true;
    f->with_damage = with_damage;
    if (!f->sc || !f->out) {
        frame_failed(f);
        return;
    }
    lm_buffer_ref(b);
    f->buf = b;
    lm_list_append(&f->sc->pending, &f->link);
    f->queued = true;
    f->sc->n_pending++;
    arm(f->sc, 1);                                               /* from the loop, never inside the request */
}

static void frame_req_copy(struct lm_resource *res, const union lm_arg *args)
{
    frame_copy_common(res, args[0].r, false);
}

static void frame_req_copy_with_damage(struct lm_resource *res, const union lm_arg *args)
{
    frame_copy_common(res, args[0].r, true);
}

static const lm_request_fn frame_handlers[] = {
    [LM_ZWLR_SCREENCOPY_FRAME_V1_REQ_COPY] = frame_req_copy,
    [LM_ZWLR_SCREENCOPY_FRAME_V1_REQ_DESTROY] = frame_req_destroy,
    [LM_ZWLR_SCREENCOPY_FRAME_V1_REQ_COPY_WITH_DAMAGE] = frame_req_copy_with_damage,
};

/* ---- zwlr_screencopy_manager_v1 ------------------------------------------------------------------------ */

static void mgr_req_destroy(struct lm_resource *res, const union lm_arg *args)
{
    (void)args;
    lm_resource_destroy(res);
}

static void capture(struct lm_resource *res, uint32_t id, int32_t cursor, struct lm_resource *outres, bool region, int32_t x, int32_t y,
                    int32_t w, int32_t h)
{
    struct lm_screencopy *sc = res->data;
    struct frame *f = calloc(1, sizeof(*f));
    if (!f) {
        lm_resource_post_no_memory(res);
        return;
    }
    f->sc = sc;
    f->client = res->client;
    f->cursor = cursor != 0;
    lm_list_init(&f->link);
    lm_list_init(&f->alink);
    f->res = lm_resource_create(res->client, id, &lm_zwlr_screencopy_frame_v1_interface, res->version, frame_handlers, f, frame_destroy);
    if (!f->res) {
        free(f);
        return;
    }
    if (sc)
        lm_list_append(&sc->all, &f->alink);
    struct lm_output *out = outres ? outres->data : NULL;
    if (!sc || !out || !output_alive(sc, out)) {
        frame_failed(f);
        return;
    }
    struct lm_rect want = region ? lm_rect_make(out->x + x, out->y + y, w, h) : lm_output_rect(out);
    struct lm_rect clip;
    int pw = 0, ph = 0;
    if ((region && (w <= 0 || h <= 0)) || !lm_render_capture_extent(out, &want, &clip, &pw, &ph)) {
        frame_failed(f);                                         /* an empty rectangle, or one outside the output */
        return;
    }
    f->out = out;
    f->rect = clip;
    f->w = pw;
    f->h = ph;
    lm_resource_post(f->res, LM_ZWLR_SCREENCOPY_FRAME_V1_EVT_BUFFER, (uint32_t)LM_WL_SHM_FORMAT_XRGB8888, pw, ph, (uint32_t)pw * 4u);
    lm_resource_post(f->res, LM_ZWLR_SCREENCOPY_FRAME_V1_EVT_BUFFER_DONE);       /* (version 3 only: filtered out for older clients) */
}

static void mgr_req_capture_output(struct lm_resource *res, const union lm_arg *args)
{
    capture(res, args[0].n, args[1].i, args[2].r, false, 0, 0, 0, 0);
}

static void mgr_req_capture_output_region(struct lm_resource *res, const union lm_arg *args)
{
    capture(res, args[0].n, args[1].i, args[2].r, true, args[3].i, args[4].i, args[5].i, args[6].i);
}

static const lm_request_fn mgr_handlers[] = {
    [LM_ZWLR_SCREENCOPY_MANAGER_V1_REQ_CAPTURE_OUTPUT] = mgr_req_capture_output,
    [LM_ZWLR_SCREENCOPY_MANAGER_V1_REQ_CAPTURE_OUTPUT_REGION] = mgr_req_capture_output_region,
    [LM_ZWLR_SCREENCOPY_MANAGER_V1_REQ_DESTROY] = mgr_req_destroy,
};

static void mgr_bind(struct lm_client *client, void *data, uint32_t version, uint32_t id)
{
    struct lm_screencopy *sc = data;
    bool allowed = sc->srv->config.screencopy_access != 0 || lm_settings_client_allowed(sc->srv, client);
    if (!allowed)
        lm_warn("screencopy: client pid %d is not one of MellaGUI's programs, its frames will fail", (int)client->pid);
    lm_resource_create(client, id, &lm_zwlr_screencopy_manager_v1_interface, version, mgr_handlers, allowed ? sc : NULL, NULL);
}

struct lm_screencopy *lm_screencopy_create(struct lm_server *srv)
{
    struct lm_screencopy *sc = calloc(1, sizeof(*sc));
    if (!sc)
        return NULL;
    sc->srv = srv;
    lm_list_init(&sc->pending);
    lm_list_init(&sc->all);
    sc->timer = lm_loop_add_timer(srv->loop, timer_cb, sc);
    sc->global = lm_global_create(srv, &lm_zwlr_screencopy_manager_v1_interface, LM_ZWLR_SCREENCOPY_MANAGER_V1_VERSION, mgr_bind, sc);
    if (!sc->global || !sc->timer) {
        lm_screencopy_destroy(sc);
        return NULL;
    }
    return sc;
}

void lm_screencopy_destroy(struct lm_screencopy *sc)
{
    if (!sc)
        return;
    struct frame *f, *tmp;
    lm_list_for_each_safe(f, tmp, &sc->all, alink) {
        dequeue(f);                                              /* (decrements n_pending: do it before the sc pointer is dropped) */
        f->sc = NULL;                                            /* the frame objects live on until their clients go */
        if (f->buf) {
            lm_buffer_unref(f->buf);
            f->buf = NULL;
        }
    }
    if (sc->timer)
        lm_timer_destroy(sc->timer);
    if (sc->global)
        lm_global_destroy(sc->global);
    free(sc);
}

int lm_screencopy_pending(const struct lm_screencopy *sc) { return sc ? sc->n_pending : 0; }
int lm_screencopy_delivered(const struct lm_screencopy *sc) { return sc ? sc->delivered : 0; }
int lm_screencopy_failed(const struct lm_screencopy *sc) { return sc ? sc->failed : 0; }
