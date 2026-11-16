#include "wlc.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

struct entry {
    uint32_t id;
    const struct lm_interface *iface;
    wlc_event_fn fn;
    void *data;
};

struct wlc {
    struct lm_wire wire;
    int fd;
    struct entry *tab;
    size_t n, cap;
    uint32_t next_id;
    bool dead;
    bool sync_done;
    uint32_t sync_id;
};

static struct entry *find(struct wlc *c, uint32_t id)
{
    for (size_t i = 0; i < c->n; i++)
        if (c->tab[i].id == id)
            return &c->tab[i];
    return NULL;
}

static struct entry *add(struct wlc *c, uint32_t id, const struct lm_interface *iface,
                         wlc_event_fn fn, void *data)
{
    if (c->n == c->cap) {
        size_t nc = c->cap ? c->cap * 2 : 32;
        struct entry *t = realloc(c->tab, nc * sizeof(*t));
        if (!t)
            return NULL;
        c->tab = t;
        c->cap = nc;
    }
    struct entry *e = &c->tab[c->n++];
    e->id = id;
    e->iface = iface;
    e->fn = fn;
    e->data = data;
    return e;
}

struct wlc *wlc_connect(const char *name)
{
    if (!name)
        name = getenv("WAYLAND_DISPLAY");
    if (!name || !*name)
        name = "wayland-0";
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    if (name[0] == '/') {
        if (strlen(name) >= sizeof(sa.sun_path))
            return NULL;
        strcpy(sa.sun_path, name);
    } else {
        const char *dir = getenv("XDG_RUNTIME_DIR");
        if (!dir || snprintf(sa.sun_path, sizeof(sa.sun_path), "%s/%s", dir, name) >= (int)sizeof(sa.sun_path)) {
            fprintf(stderr, "wlc: XDG_RUNTIME_DIR is not set or too long\n");
            return NULL;
        }
    }
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        fprintf(stderr, "wlc: cannot connect to %s: %s\n", sa.sun_path, strerror(errno));
        if (fd >= 0)
            close(fd);
        return NULL;
    }
    struct wlc *c = calloc(1, sizeof(*c));
    if (!c) {
        close(fd);
        return NULL;
    }
    c->fd = fd;
    c->next_id = 2;
    lm_wire_init(&c->wire, fd, 0);
    add(c, 1, &lm_wl_display_interface, NULL, NULL);
    return c;
}

void wlc_destroy(struct wlc *c)
{
    if (!c)
        return;
    lm_wire_finish(&c->wire);
    close(c->fd);
    free(c->tab);
    free(c);
}

int wlc_fd(const struct wlc *c) { return c->fd; }
bool wlc_dead(const struct wlc *c) { return c->dead; }

uint32_t wlc_alloc(struct wlc *c, const struct lm_interface *iface, wlc_event_fn fn, void *data)
{
    uint32_t id = c->next_id++;
    if (!add(c, id, iface, fn, data))
        return 0;
    return id;
}

void wlc_set_handler(struct wlc *c, uint32_t id, wlc_event_fn fn, void *data)
{
    struct entry *e = find(c, id);
    if (e) {
        e->fn = fn;
        e->data = data;
    }
}

void wlc_forget(struct wlc *c, uint32_t id)
{
    struct entry *e = find(c, id);
    if (e) {
        *e = c->tab[--c->n];
    }
}

int wlc_send(struct wlc *c, uint32_t id, uint32_t opcode, const union lm_arg *args)
{
    struct entry *e = find(c, id);
    if (!e || (int)opcode >= e->iface->n_requests) {
        fprintf(stderr, "wlc: bad request %u on object %u\n", opcode, id);
        return -1;
    }
    int r = lm_wire_send(&c->wire, id, opcode, e->iface->requests[opcode].signature, args);
    if (r < 0) {
        c->dead = true;
        return -1;
    }
    return 0;
}

int wlc_flush(struct wlc *c)
{
    int r = lm_wire_flush(&c->wire);
    if (r < 0)
        c->dead = true;
    return r;
}

uint32_t wlc_get_registry(struct wlc *c, wlc_event_fn fn, void *data)
{
    uint32_t id = wlc_alloc(c, &lm_wl_registry_interface, fn, data);
    WLC_SEND(c, 1, LM_WL_DISPLAY_REQ_GET_REGISTRY, { .n = id });
    return id;
}

uint32_t wlc_bind(struct wlc *c, uint32_t registry, uint32_t name, const struct lm_interface *iface,
                  uint32_t version, wlc_event_fn fn, void *data)
{
    uint32_t id = wlc_alloc(c, iface, fn, data);
    if (!id)
        return 0;
    WLC_SEND(c, registry, LM_WL_REGISTRY_REQ_BIND,
             { .u = name }, { .s = iface->name }, { .u = version }, { .n = id });
    return id;
}

static void dispatch_one(struct wlc *c, const struct lm_msg_hdr *hdr)
{
    struct entry *e = find(c, hdr->id);
    if (!e || (int)hdr->opcode >= e->iface->n_events) {
        lm_wire_discard(&c->wire, hdr);
        /* fds that came with an event we do not understand stay queued: drop them */
        return;
    }
    const struct lm_message *m = &e->iface->events[hdr->opcode];
    union lm_arg args[LM_WIRE_MAX_ARGS];
    int n = lm_wire_decode(&c->wire, hdr, m->signature, args, LM_WIRE_MAX_ARGS);
    if (n < 0) {
        fprintf(stderr, "wlc: cannot decode %s.%s (%d)\n", e->iface->name, m->name, n);
        c->dead = true;
        return;
    }
    /* Register server-created objects announced by 'n' arguments. */
    int ti = 0;
    for (const char *s = m->signature; *s; s++) {
        if (*s == '?' || (*s >= '0' && *s <= '9'))
            continue;
        if (*s == 'n' && m->types && m->types[ti])
            add(c, args[ti].n, m->types[ti], NULL, NULL);
        ti++;
    }
    uint32_t id = hdr->id;
    wlc_event_fn fn = e->fn;
    void *data = e->data;
    if (id == 1) {
        if (hdr->opcode == LM_WL_DISPLAY_EVT_ERROR) {
            fprintf(stderr, "wlc: protocol error on object %u, code %u: %s\n", args[0].o, args[1].u, args[2].s);
            c->dead = true;
        } else if (hdr->opcode == LM_WL_DISPLAY_EVT_DELETE_ID) {
            wlc_forget(c, args[0].u);
        }
        return;
    }
    if (id == c->sync_id && hdr->opcode == LM_WL_CALLBACK_EVT_DONE) {
        c->sync_done = true;
        wlc_forget(c, id);
        return;
    }
    if (fn)
        fn(c, id, hdr->opcode, args, data);
    /* Handlers own the fds they receive; without a handler close them here. */
    if (!fn) {
        int k = 0;
        for (const char *s = m->signature; *s; s++) {
            if (*s == '?' || (*s >= '0' && *s <= '9'))
                continue;
            if (*s == 'h')
                close(args[k].h);
            k++;
        }
    }
}

int wlc_dispatch_pending(struct wlc *c)
{
    int count = 0;
    struct lm_msg_hdr hdr;
    for (;;) {
        ssize_t r = lm_wire_recv(&c->wire);
        if (r == 0) {
            c->dead = true;
            return count ? count : -1;
        }
        if (r < 0 && errno != EAGAIN && errno != EINTR) {
            c->dead = true;
            return -1;
        }
        int nx;
        while ((nx = lm_wire_next(&c->wire, &hdr)) > 0) {
            dispatch_one(c, &hdr);
            count++;
            if (c->dead)
                return -1;
        }
        if (nx < 0) {
            c->dead = true;
            return -1;
        }
        if (r < 0)
            break;
    }
    return count;
}

int wlc_dispatch(struct wlc *c, int timeout_ms)
{
    if (c->dead)
        return -1;
    wlc_flush(c);
    struct pollfd p = { .fd = c->fd, .events = POLLIN };
    if (lm_wire_out_pending(&c->wire))
        p.events |= POLLOUT;
    int r = poll(&p, 1, timeout_ms);
    if (r < 0)
        return errno == EINTR ? 0 : -1;
    if (r == 0)
        return 0;
    if (p.revents & (POLLERR | POLLHUP) && !(p.revents & POLLIN)) {
        c->dead = true;
        return -1;
    }
    if (p.revents & POLLOUT)
        wlc_flush(c);
    if (p.revents & POLLIN)
        return wlc_dispatch_pending(c);
    return 0;
}

int wlc_roundtrip(struct wlc *c)
{
    uint32_t cb = wlc_alloc(c, &lm_wl_callback_interface, NULL, NULL);
    if (!cb)
        return -1;
    c->sync_id = cb;
    c->sync_done = false;
    WLC_SEND(c, 1, LM_WL_DISPLAY_REQ_SYNC, { .n = cb });
    while (!c->sync_done) {
        if (wlc_dispatch(c, 5000) < 0)
            return -1;
    }
    return 0;
}
