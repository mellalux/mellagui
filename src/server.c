#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "server.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "log.h"

/* ---- private state ------------------------------------------------------------------ */

#define OBJMAP_INIT_BITS      6
#define OBJMAP_MAX_BITS       24
#define MAX_RESOURCES_PER_CLIENT (1u << 18)
#define DEAD_SERVER_ID_TOMBS  64   /* remembered destroyed server-allocated ids */
#define DEAD_GLOBAL_TOMBS     32   /* remembered removed globals */
#define ACCEPT_BURST          16
#define SOCKET_BACKLOG        128

struct res_impl {
    struct lm_resource pub;       /* must stay first: resource->priv points back here */
    struct res_impl *hnext;       /* object map chain */
    struct lm_list zlink;         /* in server_priv.zombies once destroyed */
    bool destroyed;               /* destruction started (hooks may still run) */
    bool dead;                    /* unlinked from the object map: no more events */
    bool inert;                   /* bound to an already removed global: requests ignored */
};

struct id_tomb {
    uint32_t id;
    const struct lm_interface *iface;
};

struct client_priv {
    struct lm_client *client;
    struct res_impl **buckets;
    uint32_t bits;
    uint32_t count;
    uint32_t next_server_id;
    struct lm_list flush_link;
    struct lm_list reap_link;
    bool want_out;                /* EPOLLOUT currently requested */
    struct id_tomb tombs[DEAD_SERVER_ID_TOMBS];
    uint32_t tomb_next;
    uint32_t tomb_count;
};

struct global_tomb {
    uint32_t name;
    const struct lm_interface *iface;
};

struct registry_priv {
    struct lm_list link;          /* in server_priv.registries */
    struct lm_resource *res;
};

struct server_priv {
    struct lm_list registries;
    struct lm_list flush;         /* clients with unsent output */
    struct lm_list reap;          /* clients scheduled for disconnect */
    struct lm_list zombies;       /* destroyed resources awaiting free() */
    struct lm_idle *maint_idle;
    struct lm_source *sigchld;
    struct global_tomb gtombs[DEAD_GLOBAL_TOMBS];
    uint32_t gtomb_next;
    uint32_t gtomb_count;
    bool listen_path_owned;       /* socket file created by us */
    char lock_path[sizeof(((struct lm_server *)0)->socket_path) + 8];
};

static inline struct server_priv *spriv(struct lm_server *srv) { return srv->priv; }
static inline struct client_priv *cpriv(struct lm_client *c) { return c->priv; }
static inline struct res_impl *impl_of(const struct lm_resource *r) { return r->priv; }

static void client_destroy(struct lm_client *c);
static void client_event(int fd, uint32_t events, void *data);

/* ---- maintenance idle: flush, reap, free zombies -------------------------------------- */

static void schedule_maint(struct lm_server *srv)
{
    struct server_priv *sp = spriv(srv);
    if (sp && sp->maint_idle)
        lm_idle_schedule(sp->maint_idle);
}

static void mark_flush(struct lm_client *c)
{
    struct client_priv *cp = cpriv(c);
    if (!lm_list_linked(&cp->flush_link))
        lm_list_append(&spriv(c->server)->flush, &cp->flush_link);
    schedule_maint(c->server);
}

static void purge_zombies(struct lm_server *srv)
{
    struct server_priv *sp = spriv(srv);
    while (!lm_list_empty(&sp->zombies)) {
        struct res_impl *impl = lm_container_of(sp->zombies.next, struct res_impl, zlink);
        lm_list_remove(&impl->zlink);
        free(impl);
    }
}

/* Returns false if the client had to be disconnected. */
static bool client_flush(struct lm_client *c)
{
    struct client_priv *cp = cpriv(c);
    int r = lm_wire_flush(&c->wire);
    if (r < 0) {
        lm_debug("client %d: write failed: %s", (int)c->pid, strerror(errno));
        lm_client_disconnect(c);
        return false;
    }
    bool want_out = r > 0;
    if (want_out != cp->want_out && c->source) {
        if (lm_source_set_events(c->source, want_out ? (EPOLLIN | EPOLLOUT) : EPOLLIN) == 0)
            cp->want_out = want_out;
    }
    return true;
}

static void maint_idle_cb(void *data)
{
    struct lm_server *srv = data;
    struct server_priv *sp = spriv(srv);

    while (!lm_list_empty(&sp->flush)) {
        struct client_priv *cp = lm_container_of(sp->flush.next, struct client_priv, flush_link);
        lm_list_remove(&cp->flush_link);
        if (!cp->client->dying)
            client_flush(cp->client);
    }

    while (!lm_list_empty(&sp->reap)) {
        struct client_priv *cp = lm_container_of(sp->reap.next, struct client_priv, reap_link);
        struct lm_client *c = cp->client;
        lm_list_remove(&cp->reap_link);
        lm_wire_flush(&c->wire); /* best effort: lets a pending wl_display.error out */
        client_destroy(c);
    }

    purge_zombies(srv);
}

/* ---- object map ---------------------------------------------------------------------- */

static inline uint32_t objmap_hash(uint32_t id, uint32_t bits)
{
    return (uint32_t)(id * 2654435769u) >> (32 - bits);
}

static void objmap_grow(struct client_priv *cp)
{
    if (cp->bits >= OBJMAP_MAX_BITS)
        return;
    uint32_t nbits = cp->bits + 1;
    struct res_impl **nb = calloc((size_t)1 << nbits, sizeof(*nb));
    if (!nb)
        return; /* just keep the longer chains */
    for (uint32_t i = 0; i < ((uint32_t)1 << cp->bits); i++) {
        struct res_impl *e = cp->buckets[i];
        while (e) {
            struct res_impl *next = e->hnext;
            uint32_t h = objmap_hash(e->pub.id, nbits);
            e->hnext = nb[h];
            nb[h] = e;
            e = next;
        }
    }
    free(cp->buckets);
    cp->buckets = nb;
    cp->bits = nbits;
}

static void objmap_insert(struct client_priv *cp, struct res_impl *impl)
{
    if (cp->count >= ((uint32_t)2 << cp->bits))
        objmap_grow(cp);
    uint32_t h = objmap_hash(impl->pub.id, cp->bits);
    impl->hnext = cp->buckets[h];
    cp->buckets[h] = impl;
    cp->count++;
}

static struct res_impl *objmap_find(struct client_priv *cp, uint32_t id)
{
    for (struct res_impl *e = cp->buckets[objmap_hash(id, cp->bits)]; e; e = e->hnext)
        if (e->pub.id == id)
            return e;
    return NULL;
}

static void objmap_remove(struct client_priv *cp, struct res_impl *impl)
{
    struct res_impl **pp = &cp->buckets[objmap_hash(impl->pub.id, cp->bits)];
    while (*pp) {
        if (*pp == impl) {
            *pp = impl->hnext;
            impl->hnext = NULL;
            cp->count--;
            return;
        }
        pp = &(*pp)->hnext;
    }
}

struct lm_resource *lm_client_get_object(struct lm_client *client, uint32_t id)
{
    if (!client || !client->priv)
        return NULL;
    struct res_impl *impl = objmap_find(cpriv(client), id);
    return impl ? &impl->pub : NULL;
}

/* ---- errors and disconnect --------------------------------------------------------------- */

void lm_client_disconnect(struct lm_client *client)
{
    if (!client)
        return;
    struct client_priv *cp = cpriv(client);
    client->dying = true;
    if (!lm_list_linked(&cp->reap_link))
        lm_list_append(&spriv(client->server)->reap, &cp->reap_link);
    schedule_maint(client->server);
}

static void client_post_error_va(struct lm_client *client, uint32_t object_id,
                                 uint32_t code, const char *fmt, va_list ap)
{
    if (!client || client->dying)
        return;
    char msg[512];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    lm_warn("client %d: protocol error on object %u, code %u: %s", (int)client->pid,
            object_id, code, msg);

    union lm_arg args[3];
    args[0].o = object_id;
    args[1].u = code;
    args[2].s = msg;
    lm_wire_send(&client->wire, 1, LM_WL_DISPLAY_EVT_ERROR, "ous", args);
    lm_wire_flush(&client->wire); /* best effort */
    lm_client_disconnect(client);
}

void lm_client_post_error(struct lm_client *client, uint32_t object_id, uint32_t code,
                          const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    client_post_error_va(client, object_id, code, fmt, ap);
    va_end(ap);
}

void lm_client_post_no_memory(struct lm_client *client)
{
    lm_client_post_error(client, 1, LM_WL_DISPLAY_ERR_NO_MEMORY, "server out of memory");
}

void lm_resource_post_error(struct lm_resource *res, uint32_t code, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    client_post_error_va(res->client, res->id, code, fmt, ap);
    va_end(ap);
}

void lm_resource_post_no_memory(struct lm_resource *res)
{
    lm_client_post_error(res->client, 1, LM_WL_DISPLAY_ERR_NO_MEMORY,
                         "server out of memory (%s@%u)", res->iface->name, res->id);
}

/* ---- resources ---------------------------------------------------------------------------- */

static struct res_impl *res_alloc(struct lm_client *client, uint32_t id,
                                  const struct lm_interface *iface, uint32_t version,
                                  const lm_request_fn *handlers, void *data,
                                  lm_resource_destroy_fn destroy)
{
    struct client_priv *cp = cpriv(client);
    if (cp->count >= MAX_RESOURCES_PER_CLIENT) {
        lm_client_post_no_memory(client);
        return NULL;
    }
    struct res_impl *impl = calloc(1, sizeof(*impl));
    if (!impl) {
        lm_client_post_no_memory(client);
        return NULL;
    }
    struct lm_resource *r = &impl->pub;
    r->client = client;
    r->id = id;
    r->iface = iface;
    r->version = version > iface->version ? iface->version : version;
    r->handlers = handlers;
    r->data = data;
    r->destroy = destroy;
    r->priv = impl;
    lm_signal_init(&r->destroy_signal);
    lm_list_init(&impl->zlink);
    objmap_insert(cp, impl);
    lm_list_append(&client->resources, &r->link);
    return impl;
}

struct lm_resource *lm_resource_create(struct lm_client *client, uint32_t id,
                                       const struct lm_interface *iface, uint32_t version,
                                       const lm_request_fn *handlers, void *data,
                                       lm_resource_destroy_fn destroy)
{
    if (!client || !iface || client->dying)
        return NULL;
    if (id == 0 || id > LM_ID_MAX_CLIENT) {
        lm_client_post_error(client, 1, LM_WL_DISPLAY_ERR_INVALID_OBJECT,
                             "invalid new id %u for %s", id, iface->name);
        return NULL;
    }
    if (objmap_find(cpriv(client), id)) {
        lm_client_post_error(client, 1, LM_WL_DISPLAY_ERR_INVALID_OBJECT,
                             "new id %u (%s) is already in use", id, iface->name);
        return NULL;
    }
    struct res_impl *impl = res_alloc(client, id, iface, version, handlers, data, destroy);
    return impl ? &impl->pub : NULL;
}

struct lm_resource *lm_resource_create_server(struct lm_client *client,
                                              const struct lm_interface *iface,
                                              uint32_t version,
                                              const lm_request_fn *handlers, void *data,
                                              lm_resource_destroy_fn destroy)
{
    if (!client || !iface || client->dying)
        return NULL;
    struct client_priv *cp = cpriv(client);
    if (cp->next_server_id < LM_ID_SERVER_BASE) { /* wrapped around */
        lm_client_post_no_memory(client);
        return NULL;
    }
    struct res_impl *impl = res_alloc(client, cp->next_server_id, iface, version, handlers,
                                      data, destroy);
    if (!impl)
        return NULL;
    cp->next_server_id++; /* ids are never reused; wraps to 0 and is then refused above */
    return &impl->pub;
}

static void tomb_add(struct client_priv *cp, uint32_t id, const struct lm_interface *iface)
{
    cp->tombs[cp->tomb_next].id = id;
    cp->tombs[cp->tomb_next].iface = iface;
    cp->tomb_next = (cp->tomb_next + 1) % DEAD_SERVER_ID_TOMBS;
    if (cp->tomb_count < DEAD_SERVER_ID_TOMBS)
        cp->tomb_count++;
}

static const struct lm_interface *tomb_find(const struct client_priv *cp, uint32_t id)
{
    for (uint32_t i = 0; i < cp->tomb_count; i++)
        if (cp->tombs[i].id == id)
            return cp->tombs[i].iface;
    return NULL;
}

void lm_resource_destroy(struct lm_resource *res)
{
    if (!res || !res->priv)
        return;
    struct res_impl *impl = impl_of(res);
    if (impl->destroyed)
        return;
    impl->destroyed = true;

    struct lm_client *client = res->client;
    struct client_priv *cp = cpriv(client);

    lm_signal_emit(&res->destroy_signal, res);
    if (res->destroy)
        res->destroy(res);

    impl->dead = true;
    objmap_remove(cp, impl);
    lm_list_remove(&res->link);

    if (res->id < LM_ID_SERVER_BASE) {
        if (!client->dying) {
            union lm_arg a = { .u = res->id };
            if (lm_wire_send(&client->wire, 1, LM_WL_DISPLAY_EVT_DELETE_ID, "u", &a) < 0)
                lm_client_disconnect(client);
            else
                mark_flush(client);
        }
    } else {
        tomb_add(cp, res->id, res->iface);
    }

    /* Keep the memory valid until the loop is idle: stale pointers in the same round are safe. */
    struct server_priv *sp = spriv(client->server);
    lm_list_append(&sp->zombies, &impl->zlink);
    schedule_maint(client->server);
}

/* ---- events ---------------------------------------------------------------------------------- */

int lm_resource_post_va(struct lm_resource *res, uint32_t opcode, va_list ap)
{
    struct lm_client *client = res->client;
    const struct lm_interface *iface = res->iface;

    if (client->dying || impl_of(res)->dead)
        return -1;
    if (opcode >= (uint32_t)iface->n_events) {
        lm_error("%s@%u: event opcode %u out of range", iface->name, res->id, opcode);
        return -1;
    }
    const struct lm_message *m = &iface->events[opcode];
    if (m->since > res->version)
        return 0; /* the client's object is too old to know this event */

    union lm_arg args[LM_WIRE_MAX_ARGS];
    int idx = 0;
    memset(args, 0, sizeof(args));
    for (const char *p = m->signature; *p; p++) {
        char c = *p;
        if (c == '?' || (c >= '0' && c <= '9'))
            continue;
        if (idx >= LM_WIRE_MAX_ARGS) {
            lm_error("%s@%u.%s: too many arguments", iface->name, res->id, m->name);
            return -1;
        }
        union lm_arg *a = &args[idx++];
        switch (c) {
        case 'i': a->i = va_arg(ap, int32_t); break;
        case 'u': a->u = va_arg(ap, uint32_t); break;
        case 'f': a->f = va_arg(ap, lm_fixed_t); break;
        case 's': a->s = va_arg(ap, const char *); break;
        case 'o':
        case 'n': {
            struct lm_resource *r = va_arg(ap, struct lm_resource *);
            if (r && (r->client != client || !r->priv || impl_of(r)->dead)) {
                lm_error("%s@%u.%s: object argument from another client or destroyed",
                         iface->name, res->id, m->name);
                r = NULL;
            }
            a->u = r ? r->id : 0;
            break;
        }
        case 'a': {
            const struct lm_array *arr = va_arg(ap, const struct lm_array *);
            if (arr)
                a->a = *arr;
            break;
        }
        case 'h': a->h = va_arg(ap, int); break;
        default:
            lm_error("%s@%u.%s: bad signature", iface->name, res->id, m->name);
            return -1;
        }
    }

    int r = lm_wire_send(&client->wire, res->id, opcode, m->signature, args);
    if (r < 0) {
        if (r == LM_WIRE_E_OVERFLOW)
            lm_warn("client %d: output queue overflow, disconnecting", (int)client->pid);
        else
            lm_error("%s@%u.%s: cannot encode event (%d)", iface->name, res->id, m->name, r);
        lm_client_disconnect(client);
        return -1;
    }
    lm_trace("client %d: <- %s@%u.%s", (int)client->pid, iface->name, res->id, m->name);
    mark_flush(client);
    return 0;
}

int lm_resource_post(struct lm_resource *res, uint32_t opcode, ...)
{
    va_list ap;
    va_start(ap, opcode);
    int r = lm_resource_post_va(res, opcode, ap);
    va_end(ap);
    return r;
}

/* ---- globals / registry --------------------------------------------------------------------- */

static void registries_broadcast_global(struct lm_server *srv, const struct lm_global *g)
{
    struct registry_priv *rp;
    lm_list_for_each(rp, &spriv(srv)->registries, link)
        lm_resource_post(rp->res, LM_WL_REGISTRY_EVT_GLOBAL, g->name, g->iface->name,
                         g->version);
}

struct lm_global *lm_global_create(struct lm_server *srv, const struct lm_interface *iface,
                                   uint32_t version, lm_bind_fn bind, void *data)
{
    if (!srv || !iface || !bind || version == 0)
        return NULL;
    struct lm_global *g = calloc(1, sizeof(*g));
    if (!g)
        return NULL;
    if (version > iface->version) {
        lm_warn("global %s: version %u exceeds implemented %u, clamping", iface->name,
                version, iface->version);
        version = iface->version;
    }
    g->server = srv;
    g->name = ++srv->next_global_name;
    g->iface = iface;
    g->version = version;
    g->bind = bind;
    g->data = data;
    lm_list_append(&srv->globals, &g->link);
    registries_broadcast_global(srv, g);
    lm_debug("global %u: %s v%u", g->name, iface->name, version);
    return g;
}

void lm_global_destroy(struct lm_global *g)
{
    if (!g)
        return;
    struct lm_server *srv = g->server;
    struct server_priv *sp = spriv(srv);
    lm_list_remove(&g->link);

    sp->gtombs[sp->gtomb_next].name = g->name;
    sp->gtombs[sp->gtomb_next].iface = g->iface;
    sp->gtomb_next = (sp->gtomb_next + 1) % DEAD_GLOBAL_TOMBS;
    if (sp->gtomb_count < DEAD_GLOBAL_TOMBS)
        sp->gtomb_count++;

    struct registry_priv *rp;
    lm_list_for_each(rp, &sp->registries, link)
        lm_resource_post(rp->res, LM_WL_REGISTRY_EVT_GLOBAL_REMOVE, g->name);
    free(g);
}

static struct lm_global *global_find(struct lm_server *srv, uint32_t name)
{
    struct lm_global *g;
    lm_list_for_each(g, &srv->globals, link)
        if (g->name == name)
            return g;
    return NULL;
}

static void registry_bind(struct lm_resource *res, const union lm_arg *args)
{
    struct lm_client *client = res->client;
    struct lm_server *srv = client->server;
    uint32_t name = args[0].u;
    const char *iface_name = args[1].s;
    uint32_t version = args[2].u;
    uint32_t id = args[3].n;

    struct lm_global *g = global_find(srv, name);
    if (!g) {
        /* A global removed a moment ago (hotplug race): hand out an inert object. */
        struct server_priv *sp = spriv(srv);
        const struct lm_interface *dead = NULL;
        for (uint32_t i = 0; i < sp->gtomb_count; i++)
            if (sp->gtombs[i].name == name)
                dead = sp->gtombs[i].iface;
        if (dead && strcmp(dead->name, iface_name) == 0 && version >= 1 &&
            version <= dead->version) {
            struct lm_resource *r = lm_resource_create(client, id, dead, version, NULL,
                                                       NULL, NULL);
            if (r)
                impl_of(r)->inert = true;
            return;
        }
        lm_resource_post_error(res, LM_WL_DISPLAY_ERR_INVALID_OBJECT, "invalid global %u", name);
        return;
    }
    if (strcmp(g->iface->name, iface_name) != 0) {
        lm_resource_post_error(res, LM_WL_DISPLAY_ERR_INVALID_OBJECT,
                               "invalid interface for global %u: have %s, wanted %s", name,
                               iface_name, g->iface->name);
        return;
    }
    if (version == 0) {
        lm_resource_post_error(res, LM_WL_DISPLAY_ERR_INVALID_OBJECT,
                               "invalid version for global %s (%u): start at 1", iface_name,
                               name);
        return;
    }
    if (version > g->version) {
        lm_resource_post_error(res, LM_WL_DISPLAY_ERR_INVALID_OBJECT,
                               "invalid version for global %s (%u): have %u, wanted %u",
                               iface_name, name, g->version, version);
        return;
    }
    g->bind(client, g->data, version, id);
}

static const lm_request_fn registry_handlers[] = { registry_bind };

static void registry_destroy(struct lm_resource *res)
{
    struct registry_priv *rp = res->data;
    if (!rp)
        return;
    lm_list_remove(&rp->link);
    free(rp);
    res->data = NULL;
}

/* ---- wl_display ------------------------------------------------------------------------------ */

static void display_sync(struct lm_resource *res, const union lm_arg *args)
{
    struct lm_client *client = res->client;
    struct lm_resource *cb = lm_resource_create(client, args[0].n, &lm_wl_callback_interface,
                                                1, NULL, NULL, NULL);
    if (!cb)
        return;
    lm_resource_post(cb, LM_WL_CALLBACK_EVT_DONE, lm_server_next_serial(client->server));
    lm_resource_destroy(cb);
}

static void display_get_registry(struct lm_resource *res, const union lm_arg *args)
{
    struct lm_client *client = res->client;
    struct lm_server *srv = client->server;
    struct registry_priv *rp = calloc(1, sizeof(*rp));
    if (!rp) {
        lm_resource_post_no_memory(res);
        return;
    }
    struct lm_resource *reg = lm_resource_create(client, args[0].n, &lm_wl_registry_interface,
                                                 1, registry_handlers, rp, registry_destroy);
    if (!reg) {
        free(rp);
        return;
    }
    rp->res = reg;
    lm_list_append(&spriv(srv)->registries, &rp->link);

    struct lm_global *g;
    lm_list_for_each(g, &srv->globals, link)
        lm_resource_post(reg, LM_WL_REGISTRY_EVT_GLOBAL, g->name, g->iface->name, g->version);
}

static const lm_request_fn display_handlers[] = { display_sync, display_get_registry };

/* ---- request dispatch ----------------------------------------------------------------------------- */

static void close_fd_args(const char *sig, const union lm_arg *args, int nargs)
{
    int idx = 0;
    for (const char *p = sig; *p && idx < nargs; p++) {
        if (*p == '?' || (*p >= '0' && *p <= '9'))
            continue;
        if (*p == 'h' && args[idx].h >= 0)
            close(args[idx].h);
        idx++;
    }
}

static void dispatch_one(struct lm_client *c, const struct lm_msg_hdr *hdr)
{
    struct client_priv *cp = cpriv(c);
    struct lm_resource *res = lm_client_get_object(c, hdr->id);
    union lm_arg args[LM_WIRE_MAX_ARGS];
    int n;

    if (!res) {
        /* Racing with a server-side destroy of a server-allocated object (e.g. a
         * wl_data_offer that was replaced): parse with the old signature and drop. */
        const struct lm_interface *dead = hdr->id >= LM_ID_SERVER_BASE ? tomb_find(cp, hdr->id) : NULL;
        if (dead && hdr->opcode < (uint32_t)dead->n_requests) {
            n = lm_wire_decode(&c->wire, hdr, dead->requests[hdr->opcode].signature, args,
                               LM_WIRE_MAX_ARGS);
            if (n >= 0) {
                close_fd_args(dead->requests[hdr->opcode].signature, args, n);
                lm_debug("client %d: dropped request for destroyed %s@%u", (int)c->pid,
                         dead->name, hdr->id);
                return;
            }
            lm_client_post_error(c, 1, LM_WL_DISPLAY_ERR_INVALID_METHOD,
                                 "invalid arguments for destroyed object %u", hdr->id);
            return;
        }
        lm_wire_discard(&c->wire, hdr);
        lm_client_post_error(c, 1, LM_WL_DISPLAY_ERR_INVALID_OBJECT, "invalid object %u",
                             hdr->id);
        return;
    }

    const struct lm_interface *iface = res->iface;
    if (hdr->opcode >= (uint32_t)iface->n_requests) {
        lm_wire_discard(&c->wire, hdr);
        lm_resource_post_error(res, LM_WL_DISPLAY_ERR_INVALID_METHOD,
                               "invalid method %u, object %s@%u", hdr->opcode, iface->name,
                               res->id);
        return;
    }
    const struct lm_message *m = &iface->requests[hdr->opcode];
    if (m->since > res->version) {
        lm_wire_discard(&c->wire, hdr);
        lm_resource_post_error(res, LM_WL_DISPLAY_ERR_INVALID_METHOD,
                               "%s@%u.%s requires version %u, object has version %u",
                               iface->name, res->id, m->name, m->since, res->version);
        return;
    }

    n = lm_wire_decode(&c->wire, hdr, m->signature, args, LM_WIRE_MAX_ARGS);
    if (n < 0) {
        lm_resource_post_error(res, LM_WL_DISPLAY_ERR_INVALID_METHOD,
                               "invalid arguments for %s@%u.%s (error %d)", iface->name,
                               res->id, m->name, n);
        return;
    }

    /* Resolve object arguments to resources. */
    {
        int idx = 0;
        bool nullable = false;
        for (const char *p = m->signature; *p; p++) {
            char ch = *p;
            if (ch >= '0' && ch <= '9')
                continue;
            if (ch == '?') {
                nullable = true;
                continue;
            }
            if (ch == 'o') {
                uint32_t oid = args[idx].o;
                struct lm_resource *r = NULL;
                if (oid == 0) {
                    if (!nullable) {
                        lm_resource_post_error(res, LM_WL_DISPLAY_ERR_INVALID_OBJECT,
                                               "%s@%u.%s: null object for argument %d",
                                               iface->name, res->id, m->name, idx);
                        goto fail;
                    }
                } else {
                    r = lm_client_get_object(c, oid);
                    const struct lm_interface *want = m->types ? m->types[idx] : NULL;
                    if (!r) {
                        lm_resource_post_error(res, LM_WL_DISPLAY_ERR_INVALID_OBJECT,
                                               "%s@%u.%s: unknown object %u for argument %d",
                                               iface->name, res->id, m->name, oid, idx);
                        goto fail;
                    }
                    if (want && r->iface != want) {
                        lm_resource_post_error(res, LM_WL_DISPLAY_ERR_INVALID_OBJECT,
                                               "%s@%u.%s: object %u is a %s, expected %s",
                                               iface->name, res->id, m->name, oid,
                                               r->iface->name, want->name);
                        goto fail;
                    }
                }
                args[idx].r = r;
            }
            nullable = false;
            idx++;
        }
    }

    lm_trace("client %d: -> %s@%u.%s", (int)c->pid, iface->name, res->id, m->name);

    if (impl_of(res)->inert) {
        if (strcmp(m->name, "destroy") == 0 || strcmp(m->name, "release") == 0)
            lm_resource_destroy(res);
    } else if (res->handlers && res->handlers[hdr->opcode]) {
        res->handlers[hdr->opcode](res, args);
    } else {
        lm_debug("client %d: unhandled request %s@%u.%s", (int)c->pid, iface->name, res->id,
                 m->name);
    }
    /* `res` may be gone now; only the signature (static) is used below. */
fail:
    close_fd_args(m->signature, args, n);
}

static void dispatch_client(struct lm_client *c)
{
    struct lm_msg_hdr hdr;
    while (!c->dying) {
        int r = lm_wire_next(&c->wire, &hdr);
        if (r == 0)
            break;
        if (r < 0) {
            lm_client_post_error(c, 1, LM_WL_DISPLAY_ERR_INVALID_METHOD,
                                 "invalid message framing");
            break;
        }
        dispatch_one(c, &hdr);
    }
}

static void client_event(int fd, uint32_t events, void *data)
{
    struct lm_client *c = data;
    (void)fd;
    if (c->dying)
        return;

    if (events & EPOLLOUT) {
        if (!client_flush(c))
            return;
    }
    if (events & (EPOLLIN | EPOLLHUP | EPOLLERR)) {
        for (int attempt = 0; attempt < 2 && !c->dying; attempt++) {
            ssize_t n = lm_wire_recv(&c->wire);
            if (n > 0) {
                dispatch_client(c);
                break;
            }
            if (n == 0) {
                lm_debug("client %d: disconnected", (int)c->pid);
                lm_client_disconnect(c);
                break;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                break;
            if (errno == ENOBUFS && attempt == 0) {
                /* Ring full: dispatch what we have, then retry once. */
                dispatch_client(c);
                continue;
            }
            lm_debug("client %d: read failed: %s", (int)c->pid, strerror(errno));
            lm_client_disconnect(c);
            break;
        }
    }
}

/* ---- clients ------------------------------------------------------------------------------------------ */

static struct lm_client *client_create(struct lm_server *srv, int fd)
{
    struct lm_client *c = calloc(1, sizeof(*c));
    struct client_priv *cp = calloc(1, sizeof(*cp));
    if (!c || !cp)
        goto fail;
    cp->client = c;
    cp->bits = OBJMAP_INIT_BITS;
    cp->buckets = calloc((size_t)1 << cp->bits, sizeof(*cp->buckets));
    if (!cp->buckets)
        goto fail;
    cp->next_server_id = LM_ID_SERVER_BASE;
    lm_list_init(&cp->flush_link);
    lm_list_init(&cp->reap_link);

    c->server = srv;
    c->fd = fd;
    c->pid = 0;
    c->uid = (uid_t)-1;
    c->gid = (gid_t)-1;
    c->priv = cp;
    lm_list_init(&c->link);
    lm_list_init(&c->resources);
    lm_signal_init(&c->destroy_signal);
    lm_wire_init(&c->wire, fd, LM_WIRE_DEFAULT_MAX_OUT);

    struct ucred cred;
    socklen_t len = sizeof(cred);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0) {
        c->pid = cred.pid;
        c->uid = cred.uid;
        c->gid = cred.gid;
    }
    /* Only the user the compositor runs as (and root) may be a client: the socket is 0600 and normally lives in a 0700 directory,
     * this is the second lock. A client whose credentials cannot be read is refused too. */
    if (c->uid != geteuid() && c->uid != 0) {
        lm_warn("client refused: pid %d uid %d is not the compositor's user", (int)c->pid, (int)c->uid);
        goto fail_wire;
    }

    c->source = lm_loop_add_fd(srv->loop, fd, EPOLLIN, client_event, c);
    if (!c->source)
        goto fail_wire;
    lm_list_append(&srv->clients, &c->link);

    if (!lm_resource_create(c, 1, &lm_wl_display_interface, 1, display_handlers, NULL, NULL)) {
        client_destroy(c);
        return NULL;
    }
    lm_info("client connected: pid %d uid %u", (int)c->pid, (unsigned)c->uid);
    return c;

fail_wire:
    lm_wire_finish(&c->wire);
fail:
    if (cp)
        free(cp->buckets);
    free(cp);
    free(c);
    return NULL;
}

static void client_destroy(struct lm_client *c)
{
    struct lm_server *srv = c->server;
    struct client_priv *cp = cpriv(c);

    c->dying = true;
    lm_list_remove(&cp->flush_link);
    lm_list_remove(&cp->reap_link);
    if (c->source) {
        lm_source_remove(c->source);
        c->source = NULL;
    }
    /* Newest first, re-reading the tail each time: destroy hooks may destroy others. */
    while (!lm_list_empty(&c->resources)) {
        struct lm_resource *r = lm_container_of(c->resources.prev, struct lm_resource, link);
        lm_resource_destroy(r);
    }
    lm_signal_emit(&c->destroy_signal, c);
    /* Hooks may have scheduled work for this client again. */
    lm_list_remove(&cp->flush_link);
    lm_list_remove(&cp->reap_link);
    lm_info("client disconnected: pid %d", (int)c->pid);

    lm_list_remove(&c->link);
    lm_wire_finish(&c->wire);
    close(c->fd);
    purge_zombies(srv);
    free(cp->buckets);
    free(cp);
    free(c);
}

static void listen_event(int fd, uint32_t events, void *data)
{
    struct lm_server *srv = data;
    (void)events;
    for (int i = 0; i < ACCEPT_BURST; i++) {
        int cfd = accept4(fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            if (errno == EINTR || errno == ECONNABORTED)
                continue;
            lm_warn("accept: %s", strerror(errno));
            return;
        }
        if (!client_create(srv, cfd)) {
            lm_warn("cannot create client");
            close(cfd);
        }
    }
}

/* ---- server lifecycle ------------------------------------------------------------------------------------ */

struct lm_server *lm_server_create(struct lm_loop *loop, const struct lm_config *cfg)
{
    if (!loop || !cfg)
        return NULL;
    struct lm_server *srv = calloc(1, sizeof(*srv));
    struct server_priv *sp = calloc(1, sizeof(*sp));
    if (!srv || !sp) {
        free(srv);
        free(sp);
        return NULL;
    }
    srv->config = *cfg;
    srv->loop = loop;
    srv->listen_fd = -1;
    srv->lock_fd = -1;
    lm_list_init(&srv->clients);
    lm_list_init(&srv->globals);
    srv->priv = sp;
    lm_list_init(&sp->registries);
    lm_list_init(&sp->flush);
    lm_list_init(&sp->reap);
    lm_list_init(&sp->zombies);
    sp->maint_idle = lm_loop_add_idle(loop, maint_idle_cb, srv);
    if (!sp->maint_idle) {
        free(sp);
        free(srv);
        return NULL;
    }
    signal(SIGPIPE, SIG_IGN);
    return srv;
}

#define MAX_WATCH 4
static struct watch {
    pid_t pid;
    int fd;                             /* the read end of the child's stderr, -1 when the slot is free */
    lm_exit_fn fn;
    void *data;
} watches[MAX_WATCH] = { { 0, -1, NULL, NULL }, { 0, -1, NULL, NULL }, { 0, -1, NULL, NULL }, { 0, -1, NULL, NULL } };

static void sigchld_cb(int signo, void *data)
{
    (void)signo;
    (void)data;
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (int i = 0; i < MAX_WATCH; i++)
            if (watches[i].fd >= 0 && watches[i].pid == pid) {              /* somebody wants to know how this one ended */
                char err[208];
                ssize_t n = read(watches[i].fd, err, sizeof(err) - 1);
                err[n > 0 ? n : 0] = 0;
                for (char *p = err; *p; p++)
                    if (*p == '\n' || *p == '\r')
                        *p = ' ';
                close(watches[i].fd);
                lm_exit_fn fn = watches[i].fn;
                void *wd = watches[i].data;
                watches[i].fd = -1;
                if (fn)
                    fn(wd, status, err);
            }
        if (WIFEXITED(status))
            lm_debug("child %d exited with status %d", (int)pid, WEXITSTATUS(status));
        else if (WIFSIGNALED(status))
            lm_debug("child %d killed by signal %d", (int)pid, WTERMSIG(status));
    }
}

/* Try to claim `path` (+ ".lock"). Returns 0 and fills lock_fd/listen_fd, 1 if busy, -1 on error. */
static int try_socket(struct lm_server *srv, const char *path, int *out_lock, int *out_listen)
{
    struct server_priv *sp = spriv(srv);
    struct sockaddr_un addr;
    char lock_path[sizeof(sp->lock_path)];

    if (strlen(path) >= sizeof(addr.sun_path)) {
        lm_error("socket path too long (max %zu): %s", sizeof(addr.sun_path) - 1, path);
        return -1;
    }
    if (snprintf(lock_path, sizeof(lock_path), "%s.lock", path) >= (int)sizeof(lock_path)) {
        lm_error("lock path too long: %s", path);
        return -1;
    }

    int lock = open(lock_path, O_CREAT | O_RDWR | O_CLOEXEC, 0660);
    if (lock < 0) {
        lm_error("cannot open %s: %s", lock_path, strerror(errno));
        return -1;
    }
    if (flock(lock, LOCK_EX | LOCK_NB) < 0) {
        close(lock);
        return 1;
    }

    struct stat st;
    if (lstat(path, &st) == 0 && S_ISSOCK(st.st_mode))
        unlink(path); /* stale socket of a dead compositor: we hold the lock */

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        lm_error("socket: %s", strerror(errno));
        goto fail;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, path);
    mode_t old_umask = umask(0177);                   /* the socket is created 0600: only this user may connect */
    int bound = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
    umask(old_umask);
    if (bound < 0) {
        lm_error("bind %s: %s", path, strerror(errno));
        goto fail_fd;
    }
    if (listen(fd, SOCKET_BACKLOG) < 0) {
        lm_error("listen %s: %s", path, strerror(errno));
        unlink(path);
        goto fail_fd;
    }
    strcpy(sp->lock_path, lock_path);
    *out_lock = lock;
    *out_listen = fd;
    return 0;

fail_fd:
    close(fd);
fail:
    unlink(lock_path);
    close(lock);
    return -1;
}

int lm_server_listen(struct lm_server *srv)
{
    struct server_priv *sp = spriv(srv);
    char path[sizeof(srv->socket_path)];
    const char *name = srv->config.socket_name;
    int lock = -1, fd = -1;

    if (srv->listen_fd >= 0)
        return 0;

    if (name && name[0] == '/') {
        if (strlen(name) >= sizeof(path)) {
            lm_error("socket path too long: %s", name);
            return -1;
        }
        strcpy(path, name);
        int r = try_socket(srv, path, &lock, &fd);
        if (r > 0)
            lm_error("socket %s is already in use by another compositor", path);
        if (r != 0)
            return -1;
        const char *base = strrchr(path, '/') + 1;
        snprintf(srv->socket_name, sizeof(srv->socket_name), "%s", base);
    } else {
        const char *dir = getenv("XDG_RUNTIME_DIR");
        if (!dir || !*dir) {
            lm_error("XDG_RUNTIME_DIR is not set: cannot create the Wayland socket "
                     "(log in through a normal session, or export XDG_RUNTIME_DIR=/run/user/$UID)");
            return -1;
        }
        if (name && *name) {
            if (strlen(name) >= sizeof(srv->socket_name) || strchr(name, '/')) {
                lm_error("invalid socket name '%s'", name);
                return -1;
            }
            if (snprintf(path, sizeof(path), "%s/%s", dir, name) >= (int)sizeof(path)) {
                lm_error("socket path too long");
                return -1;
            }
            int r = try_socket(srv, path, &lock, &fd);
            if (r > 0)
                lm_error("socket %s is already in use by another compositor", path);
            if (r != 0)
                return -1;
            snprintf(srv->socket_name, sizeof(srv->socket_name), "%s", name);
        } else {
            int found = 0;
            for (int i = 0; i <= 32 && !found; i++) {
                if (snprintf(path, sizeof(path), "%s/wayland-%d", dir, i) >= (int)sizeof(path)) {
                    lm_error("socket path too long");
                    return -1;
                }
                int r = try_socket(srv, path, &lock, &fd);
                if (r < 0)
                    return -1;
                if (r == 0) {
                    snprintf(srv->socket_name, sizeof(srv->socket_name), "wayland-%d", i);
                    found = 1;
                }
            }
            if (!found) {
                lm_error("no free wayland-N socket in %s (wayland-0 .. wayland-32 are all in use)",
                         dir);
                return -1;
            }
        }
    }

    srv->listen_source = lm_loop_add_fd(srv->loop, fd, EPOLLIN, listen_event, srv);
    if (!srv->listen_source) {
        unlink(path);
        unlink(sp->lock_path);
        close(fd);
        close(lock);
        return -1;
    }
    snprintf(srv->socket_path, sizeof(srv->socket_path), "%s", path);
    srv->listen_fd = fd;
    srv->lock_fd = lock;
    sp->listen_path_owned = true;

    if (!sp->sigchld)
        sp->sigchld = lm_loop_add_signal(srv->loop, SIGCHLD, sigchld_cb, srv);
    if (!sp->sigchld)
        lm_warn("cannot watch SIGCHLD: spawned children will not be reaped");

    lm_info("listening on %s", srv->socket_path);
    return 0;
}

static void server_unlisten(struct lm_server *srv)
{
    struct server_priv *sp = spriv(srv);
    if (srv->listen_source) {
        lm_source_remove(srv->listen_source);
        srv->listen_source = NULL;
    }
    if (sp->listen_path_owned) {
        unlink(srv->socket_path);
        unlink(sp->lock_path);
        sp->listen_path_owned = false;
    }
    if (srv->listen_fd >= 0)
        close(srv->listen_fd);
    if (srv->lock_fd >= 0)
        close(srv->lock_fd);
    srv->listen_fd = -1;
    srv->lock_fd = -1;
}

void lm_server_destroy(struct lm_server *srv)
{
    if (!srv)
        return;
    struct server_priv *sp = spriv(srv);

    server_unlisten(srv);
    while (!lm_list_empty(&srv->clients)) {
        struct lm_client *c = lm_container_of(srv->clients.next, struct lm_client, link);
        client_destroy(c);
    }
    while (!lm_list_empty(&srv->globals)) {
        struct lm_global *g = lm_container_of(srv->globals.next, struct lm_global, link);
        lm_list_remove(&g->link);
        free(g);
    }
    purge_zombies(srv);
    if (sp->sigchld)
        lm_source_remove(sp->sigchld);
    if (sp->maint_idle)
        lm_idle_destroy(sp->maint_idle);
    free(sp);
    free(srv);
}

const char *lm_server_socket_name(const struct lm_server *srv)
{
    return srv->socket_name;
}

uint32_t lm_server_next_serial(struct lm_server *srv)
{
    if (++srv->serial == 0)
        srv->serial = 1;
    return srv->serial;
}

void lm_server_quit(struct lm_server *srv)
{
    lm_loop_quit(srv->loop, 0);
}

static pid_t spawn_impl(struct lm_server *srv, const char *cmd, int err_fd)
{
    if (!cmd || !*cmd)
        return -1;
    if (srv->listen_fd < 0) {
        lm_error("lm_server_spawn: not listening yet");
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        lm_error("fork: %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        if (err_fd >= 0) {                              /* a watched child: stderr is the pipe, stdout goes nowhere */
            int nul = open("/dev/null", O_WRONLY);
            if (nul >= 0) {
                dup2(nul, 1);
                close(nul);
            }
            dup2(err_fd, 2);
            close(err_fd);
        }
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL); /* the loop blocks signals for signalfd */
        signal(SIGPIPE, SIG_DFL);
        setsid();
        setenv("WAYLAND_DISPLAY",
               (srv->config.socket_name && srv->config.socket_name[0] == '/')
                   ? srv->socket_path : srv->socket_name, 1);
        unsetenv("DISPLAY");
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    lm_info("spawned pid %d: %s", (int)pid, cmd);
    return pid;
}

pid_t lm_server_spawn(struct lm_server *srv, const char *cmd)
{
    return spawn_impl(srv, cmd, -1);
}

pid_t lm_server_spawn_watch(struct lm_server *srv, const char *cmd, lm_exit_fn fn, void *data)
{
    int slot = -1;
    for (int i = 0; i < MAX_WATCH && slot < 0; i++)
        if (watches[i].fd < 0)
            slot = i;
    int fds[2] = { -1, -1 };
    if (slot < 0 || pipe2(fds, O_CLOEXEC | O_NONBLOCK) < 0)
        return spawn_impl(srv, cmd, -1);                /* no watching, but it still runs */
    pid_t pid = spawn_impl(srv, cmd, fds[1]);
    close(fds[1]);
    if (pid <= 0) {
        close(fds[0]);
        return pid;
    }
    watches[slot].pid = pid;
    watches[slot].fd = fds[0];
    watches[slot].fn = fn;
    watches[slot].data = data;
    return pid;
}
