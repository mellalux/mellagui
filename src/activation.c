#include "activation.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>

#include "list.h"
#include "log.h"
#include "protocol.h"
#include "seat.h"
#include "server.h"
#include "surface.h"
#include "wm.h"

#define TOKEN_LIFE_MS 30000
#define TOKEN_MAX 64                    /* a program that asks for more of them than this loses its oldest ones */

struct token {
    struct lm_list link;                /* in lm_activation.tokens while it waits for an activate */
    char text[48];
    uint64_t born_ms;
    bool trusted;                       /* the serial was a real input event / the asking window has the focus */
};

struct lm_activation {
    struct lm_server *srv;
    struct lm_global *global;
    struct lm_list tokens;
    int n_tokens;
    int done, refused;
};

/* what a token resource collects before commit */
struct maker {
    struct lm_activation *act;          /* NULL: the compositor is going away */
    bool committed;
    bool has_serial;
    uint32_t serial;
    struct lm_surface *surface;
    struct lm_listener gone;            /* the surface's destroy_signal */
};

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void token_drop(struct lm_activation *a, struct token *t)
{
    lm_list_remove(&t->link);
    a->n_tokens--;
    free(t);
}

static void tokens_expire(struct lm_activation *a)
{
    uint64_t now = now_ms();
    struct token *t, *tmp;
    lm_list_for_each_safe(t, tmp, &a->tokens, link)
        if (now - t->born_ms > TOKEN_LIFE_MS)
            token_drop(a, t);
}

/* ---- xdg_activation_token_v1 ---------------------------------------------------------------------- */

static void maker_surface_gone(struct lm_listener *l, void *data)
{
    (void)data;
    struct maker *m = lm_container_of(l, struct maker, gone);
    lm_listener_remove(&m->gone);
    lm_list_init(&m->gone.link);
    m->surface = NULL;
}

static void maker_destroy(struct lm_resource *res)
{
    struct maker *m = res->data;
    if (!m)
        return;
    res->data = NULL;
    lm_listener_remove(&m->gone);
    free(m);
}

static void tok_set_serial(struct lm_resource *res, const union lm_arg *args)
{
    struct maker *m = res->data;
    if (!m)
        return;
    if (m->committed) {
        lm_resource_post_error(res, LM_XDG_ACTIVATION_TOKEN_ERR_ALREADY_USED, "the token was committed already");
        return;
    }
    m->has_serial = true;
    m->serial = args[0].u;
}

static void tok_set_app_id(struct lm_resource *res, const union lm_arg *args)
{
    struct maker *m = res->data;
    (void)args;
    if (m && m->committed)
        lm_resource_post_error(res, LM_XDG_ACTIVATION_TOKEN_ERR_ALREADY_USED, "the token was committed already");
}

static void tok_set_surface(struct lm_resource *res, const union lm_arg *args)
{
    struct maker *m = res->data;
    if (!m)
        return;
    if (m->committed) {
        lm_resource_post_error(res, LM_XDG_ACTIVATION_TOKEN_ERR_ALREADY_USED, "the token was committed already");
        return;
    }
    lm_listener_remove(&m->gone);
    lm_list_init(&m->gone.link);
    m->surface = lm_surface_from_resource(args[0].r);
    if (m->surface) {
        m->gone.notify = maker_surface_gone;
        lm_signal_add(&m->surface->destroy_signal, &m->gone);
    }
}

static void tok_commit(struct lm_resource *res, const union lm_arg *args)
{
    (void)args;
    struct maker *m = res->data;
    if (!m)
        return;
    if (m->committed) {
        lm_resource_post_error(res, LM_XDG_ACTIVATION_TOKEN_ERR_ALREADY_USED, "the token was committed already");
        return;
    }
    m->committed = true;
    struct lm_activation *a = m->act;
    struct token *t = a ? calloc(1, sizeof(*t)) : NULL;
    unsigned char rnd[12];
    if (!t || getrandom(rnd, sizeof(rnd), 0) != (ssize_t)sizeof(rnd)) {
        free(t);
        lm_resource_post(res, LM_XDG_ACTIVATION_TOKEN_V1_EVT_DONE, "");     /* an empty token never activates anything */
        return;
    }
    size_t o = (size_t)snprintf(t->text, sizeof(t->text), "mellagui-");
    for (size_t i = 0; i < sizeof(rnd); i++)
        o += (size_t)snprintf(t->text + o, sizeof(t->text) - o, "%02x", rnd[i]);
    t->born_ms = now_ms();
    t->trusted = (m->has_serial && lm_seat_serial_valid(a->srv->seat, res->client, m->serial)) ||
                 (m->surface && a->srv->wm && lm_wm_surface_focused(a->srv->wm, m->surface));
    tokens_expire(a);
    while (a->n_tokens >= TOKEN_MAX) {
        struct token *old = lm_container_of(a->tokens.next, struct token, link);
        token_drop(a, old);
    }
    lm_list_append(&a->tokens, &t->link);
    a->n_tokens++;
    lm_resource_post(res, LM_XDG_ACTIVATION_TOKEN_V1_EVT_DONE, t->text);
}

static void tok_destroy(struct lm_resource *res, const union lm_arg *args)
{
    (void)args;
    lm_resource_destroy(res);
}

static const lm_request_fn token_handlers[] = {
    [LM_XDG_ACTIVATION_TOKEN_V1_REQ_SET_SERIAL] = tok_set_serial,
    [LM_XDG_ACTIVATION_TOKEN_V1_REQ_SET_APP_ID] = tok_set_app_id,
    [LM_XDG_ACTIVATION_TOKEN_V1_REQ_SET_SURFACE] = tok_set_surface,
    [LM_XDG_ACTIVATION_TOKEN_V1_REQ_COMMIT] = tok_commit,
    [LM_XDG_ACTIVATION_TOKEN_V1_REQ_DESTROY] = tok_destroy,
};

/* ---- xdg_activation_v1 ------------------------------------------------------------------------------- */

static void act_destroy(struct lm_resource *res, const union lm_arg *args)
{
    (void)args;
    lm_resource_destroy(res);
}

static void act_get_token(struct lm_resource *res, const union lm_arg *args)
{
    struct maker *m = calloc(1, sizeof(*m));
    if (!m) {
        lm_resource_post_no_memory(res);
        return;
    }
    m->act = res->data;
    lm_list_init(&m->gone.link);
    if (!lm_resource_create(res->client, args[0].n, &lm_xdg_activation_token_v1_interface, res->version, token_handlers, m, maker_destroy))
        free(m);
}

static void act_activate(struct lm_resource *res, const union lm_arg *args)
{
    struct lm_activation *a = res->data;
    if (!a)
        return;
    const char *text = args[0].s ? args[0].s : "";
    struct lm_surface *s = lm_surface_from_resource(args[1].r);
    tokens_expire(a);
    struct token *t;
    lm_list_for_each(t, &a->tokens, link)
        if (!strcmp(t->text, text))
            break;
    if (&t->link == &a->tokens) {                               /* (list head reached: no such token, or it ended) */
        a->refused++;
        lm_debug("activation: an unknown or used token");
        return;
    }
    bool trusted = t->trusted;
    token_drop(a, t);                                           /* a token works once, whatever came of it */
    if (!trusted || !s || !a->srv->wm || !lm_wm_activate_surface(a->srv->wm, s)) {
        a->refused++;
        lm_debug("activation: not honoured (%s)", trusted ? "no window to bring forward" : "the token had no input behind it");
        return;
    }
    a->done++;
}

static const lm_request_fn act_handlers[] = {
    [LM_XDG_ACTIVATION_V1_REQ_DESTROY] = act_destroy,
    [LM_XDG_ACTIVATION_V1_REQ_GET_ACTIVATION_TOKEN] = act_get_token,
    [LM_XDG_ACTIVATION_V1_REQ_ACTIVATE] = act_activate,
};

static void act_bind(struct lm_client *client, void *data, uint32_t version, uint32_t id)
{
    lm_resource_create(client, id, &lm_xdg_activation_v1_interface, version, act_handlers, data, NULL);
}

struct lm_activation *lm_activation_create(struct lm_server *srv)
{
    struct lm_activation *a = calloc(1, sizeof(*a));
    if (!a)
        return NULL;
    a->srv = srv;
    lm_list_init(&a->tokens);
    a->global = lm_global_create(srv, &lm_xdg_activation_v1_interface, LM_XDG_ACTIVATION_V1_VERSION, act_bind, a);
    if (!a->global) {
        free(a);
        return NULL;
    }
    return a;
}

void lm_activation_destroy(struct lm_activation *a)
{
    if (!a)
        return;
    struct token *t, *tmp;
    lm_list_for_each_safe(t, tmp, &a->tokens, link)
        token_drop(a, t);
    if (a->global)
        lm_global_destroy(a->global);
    free(a);
}

int lm_activation_pending(const struct lm_activation *a) { return a ? a->n_tokens : 0; }
int lm_activation_done(const struct lm_activation *a) { return a ? a->done : 0; }
int lm_activation_refused(const struct lm_activation *a) { return a ? a->refused : 0; }
