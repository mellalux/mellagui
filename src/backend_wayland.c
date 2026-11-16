/*
 * Nested backend: MellaGUI as a window of another Wayland compositor (WSLg on Windows, GNOME, KDE, Sway, ... or another MellaGUI).
 *
 * The compositor draws its one output into an ordinary window of the host: an xdg_toplevel with two wl_shm buffers; the host's mouse and keyboard
 * events are fed to the seat as if they came from devices (lm_input_inject, so the modifier tracking, key repeat and sticky keys are the same as
 * on a real keyboard). It needs no /dev/dri and no /dev/input, so it runs where those do not exist (WSL) and is a pleasant way to try MellaGUI on
 * any desktop. `--backend wayland`, or `auto` (the default): the nested one when $WAYLAND_DISPLAY names a host compositor, else DRM.
 *
 * Limits: the pointer is absolute (no pointer lock or relative motion for games), no power / tint / mode setting, the host's clipboard is not shared,
 * and the window's size is the output's: resize it and the output follows.
 */
#include <errno.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <unistd.h>

#include "backend.h"
#include "input.h"
#include "log.h"
#include "loop.h"
#include "output.h"
#include "protocol.h"
#include "wlc.h"

#define NBUF 2

struct nbuf {
    uint32_t id;                        /* wl_buffer object, 0 = none */
    uint32_t *pixels;
    bool busy;                          /* the host has it and has not released it */
};

struct nested {
    struct lm_backend base;
    struct lm_output out;
    struct lm_framebuffer fb;           /* the frame the renderer draws into: ordinary memory, copied into a shm buffer by present() */
    char host[128];                     /* the host's socket name, taken before our own server changes WAYLAND_DISPLAY */
    struct wlc *c;
    struct lm_source *src;
    uint32_t registry, compositor, shm, wm_base, seat, pointer, keyboard;
    uint32_t surface, xsurface, toplevel, pool;
    int w, h;                           /* the output's size = the window's */
    int want_w, want_h;                 /* what the host last asked for in xdg_toplevel.configure */
    bool configured, live, waiting, ready_owed;
    int frames;                         /* presented since the buffers were made (the renderer's buffer age) */
    struct nbuf buf[NBUF];
    void *map;
    size_t map_size;
    int32_t discrete[2];                /* the notches announced by axis_discrete for the axis event that follows */
    bool down[KEY_MAX + 1];             /* keys the host says are held, to let go of them when the window loses the keyboard */
};

#define NB(b) ((struct nested *)(b))

static void send_ready(struct nested *n)
{
    n->waiting = false;
    n->ready_owed = false;
    if (n->live && n->base.listener.output_ready)
        n->base.listener.output_ready(n->base.listener.data, &n->out, lm_now_msec32());
}

/* ---- buffers ---------------------------------------------------------------------------------------------------- */

static void buffers_free(struct nested *n)
{
    for (int i = 0; i < NBUF; i++) {
        if (n->buf[i].id) {
            WLC_SEND0(n->c, n->buf[i].id, LM_WL_BUFFER_REQ_DESTROY);
            wlc_forget(n->c, n->buf[i].id);
        }
        n->buf[i] = (struct nbuf){ 0 };
    }
    if (n->pool) {
        WLC_SEND0(n->c, n->pool, LM_WL_SHM_POOL_REQ_DESTROY);
        wlc_forget(n->c, n->pool);
        n->pool = 0;
    }
    if (n->map) {
        munmap(n->map, n->map_size);
        n->map = NULL;
    }
}

static void buffer_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)c;
    (void)args;
    struct nested *n = data;
    if (op != LM_WL_BUFFER_EVT_RELEASE)
        return;
    for (int i = 0; i < NBUF; i++)
        if (n->buf[i].id == id)
            n->buf[i].busy = false;
    if (n->ready_owed && n->live)                                   /* a frame was refused because both buffers were out: now there is one */
        send_ready(n);
}

/* The two buffers and the frame in memory, for n->w x n->h. */
static int buffers_make(struct nested *n)
{
    buffers_free(n);
    size_t stride = (size_t)n->w * 4, one = stride * (size_t)n->h;
    n->map_size = one * NBUF;
    int fd = memfd_create("mellagui-nested", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)n->map_size) < 0) {
        if (fd >= 0)
            close(fd);
        return -1;
    }
    n->map = mmap(NULL, n->map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (n->map == MAP_FAILED) {
        n->map = NULL;
        close(fd);
        return -1;
    }
    n->pool = wlc_alloc(n->c, &lm_wl_shm_pool_interface, NULL, NULL);
    WLC_SEND(n->c, n->shm, LM_WL_SHM_REQ_CREATE_POOL, { .n = n->pool }, { .h = fd }, { .i = (int32_t)n->map_size });
    close(fd);
    for (int i = 0; i < NBUF; i++) {
        n->buf[i].id = wlc_alloc(n->c, &lm_wl_buffer_interface, buffer_event, n);
        n->buf[i].pixels = (uint32_t *)((char *)n->map + (size_t)i * one);
        WLC_SEND(n->c, n->pool, LM_WL_SHM_POOL_REQ_CREATE_BUFFER, { .n = n->buf[i].id }, { .i = (int32_t)(one * (size_t)i) }, { .i = n->w },
                 { .i = n->h }, { .i = (int32_t)stride }, { .u = LM_WL_SHM_FORMAT_XRGB8888 });
    }
    free(n->fb.pixels);
    n->fb = (struct lm_framebuffer){ .width = n->w, .height = n->h, .stride = n->w * 4, .pixels = calloc((size_t)n->w * (size_t)n->h, 4) };
    n->frames = 0;
    return n->fb.pixels ? 0 : -1;
}

/* ---- the host's events ------------------------------------------------------------------------------------------ */

static void apply_size(struct nested *n)
{
    int w = n->want_w > 0 ? n->want_w : n->w, h = n->want_h > 0 ? n->want_h : n->h;
    if (w < 64 || h < 64 || (w == n->w && h == n->h && n->fb.pixels))
        return;
    n->w = w;
    n->h = h;
    if (buffers_make(n) < 0) {
        lm_error("nested: out of memory for a %dx%d window", w, h);
        lm_server_quit(n->base.server);
        return;
    }
    lm_output_set_mode(&n->out, w, h);
    n->out.phys_width_mm = w * 254 / 960;
    n->out.phys_height_mm = h * 254 / 960;
    if (n->live && n->base.listener.output_changed)
        n->base.listener.output_changed(n->base.listener.data, &n->out);
    if (n->live)
        send_ready(n);                                              /* (a frame of the old size may still be waited for: it will not come) */
}

static void wm_base_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)data;
    if (op == LM_XDG_WM_BASE_EVT_PING)
        WLC_SEND(c, id, LM_XDG_WM_BASE_REQ_PONG, { .u = args[0].u });
}

static void xsurface_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    struct nested *n = data;
    if (op != LM_XDG_SURFACE_EVT_CONFIGURE)
        return;
    WLC_SEND(c, id, LM_XDG_SURFACE_REQ_ACK_CONFIGURE, { .u = args[0].u });
    n->configured = true;
    apply_size(n);
}

static void toplevel_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)c;
    (void)id;
    struct nested *n = data;
    if (op == LM_XDG_TOPLEVEL_EVT_CONFIGURE) {
        n->want_w = args[0].i;                                      /* (0 = "your choice": the size we have) */
        n->want_h = args[1].i;
    } else if (op == LM_XDG_TOPLEVEL_EVT_CLOSE) {
        lm_info("nested: the window was closed");
        lm_server_quit(n->base.server);
    }
}

static void release_keys(struct nested *n)
{
    for (int k = 0; k <= KEY_MAX; k++) {
        if (!n->down[k])
            continue;
        n->down[k] = false;
        struct lm_input_event ev = { .type = LM_INPUT_KEY, .u.key = { .code = (uint32_t)k, .pressed = false } };
        if (n->base.server->input)
            lm_input_inject(n->base.server->input, &ev);
    }
}

static void pointer_move(struct nested *n, double x, double y)
{
    if (!n->base.server->input || n->w <= 0 || n->h <= 0)
        return;
    struct lm_input_event ev = { .type = LM_INPUT_POINTER_MOTION_ABS, .time_ms = 0 };
    ev.u.motion_abs.x = x / (double)n->w;
    ev.u.motion_abs.y = y / (double)n->h;
    lm_input_inject(n->base.server->input, &ev);
}

static void pointer_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)c;
    (void)id;
    struct nested *n = data;
    struct lm_input *in = n->base.server->input;
    if (!in)
        return;
    switch (op) {
    case LM_WL_POINTER_EVT_ENTER:
        pointer_move(n, lm_fixed_to_double(args[2].f), lm_fixed_to_double(args[3].f));
        break;
    case LM_WL_POINTER_EVT_MOTION:
        pointer_move(n, lm_fixed_to_double(args[1].f), lm_fixed_to_double(args[2].f));
        break;
    case LM_WL_POINTER_EVT_BUTTON: {
        struct lm_input_event ev = { .type = LM_INPUT_POINTER_BUTTON };
        ev.u.button.code = args[2].u;
        ev.u.button.pressed = args[3].u == LM_POINTER_BUTTON_PRESSED;
        lm_input_inject(in, &ev);
        break;
    }
    case LM_WL_POINTER_EVT_AXIS_DISCRETE:
        if (args[0].u < 2)
            n->discrete[args[0].u] = args[1].i;
        break;
    case LM_WL_POINTER_EVT_AXIS: {
        uint32_t axis = args[1].u < 2 ? args[1].u : 0;
        struct lm_input_event ev = { .type = LM_INPUT_POINTER_AXIS };
        ev.u.axis.orientation = (int)axis;
        ev.u.axis.value = lm_fixed_to_double(args[2].f);
        ev.u.axis.discrete = n->discrete[axis];
        n->discrete[axis] = 0;
        lm_input_inject(in, &ev);
        break;
    }
    default:
        break;
    }
}

static void keyboard_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)c;
    (void)id;
    struct nested *n = data;
    switch (op) {
    case LM_WL_KEYBOARD_EVT_KEYMAP:
        if (args[1].h >= 0)
            close(args[1].h);                                       /* (the host's keymap is of no use: the codes are evdev's) */
        break;
    case LM_WL_KEYBOARD_EVT_LEAVE:
        release_keys(n);                                            /* nothing stays held that the host will never say let go of */
        break;
    case LM_WL_KEYBOARD_EVT_KEY: {
        uint32_t code = args[2].u;
        bool pressed = args[3].u == LM_KEYBOARD_KEY_PRESSED;
        if (code > KEY_MAX || !n->base.server->input)
            break;
        n->down[code] = pressed;
        struct lm_input_event ev = { .type = LM_INPUT_KEY, .u.key = { .code = code, .pressed = pressed } };
        lm_input_inject(n->base.server->input, &ev);
        break;
    }
    default:
        break;
    }
}

static void seat_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    struct nested *n = data;
    if (op != LM_WL_SEAT_EVT_CAPABILITIES)
        return;
    if ((args[0].u & 1) && !n->pointer) {
        n->pointer = wlc_alloc(c, &lm_wl_pointer_interface, pointer_event, n);
        WLC_SEND(c, id, LM_WL_SEAT_REQ_GET_POINTER, { .n = n->pointer });
    }
    if ((args[0].u & 2) && !n->keyboard) {
        n->keyboard = wlc_alloc(c, &lm_wl_keyboard_interface, keyboard_event, n);
        WLC_SEND(c, id, LM_WL_SEAT_REQ_GET_KEYBOARD, { .n = n->keyboard });
    }
}

static void registry_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)id;
    struct nested *n = data;
    if (op != LM_WL_REGISTRY_EVT_GLOBAL)
        return;
    uint32_t name = args[0].u, ver = args[2].u;
    const char *iface = args[1].s;
    if (!strcmp(iface, "wl_compositor"))
        n->compositor = wlc_bind(c, n->registry, name, &lm_wl_compositor_interface, LM_MIN(ver, 4u), NULL, n);
    else if (!strcmp(iface, "wl_shm"))
        n->shm = wlc_bind(c, n->registry, name, &lm_wl_shm_interface, 1, NULL, n);
    else if (!strcmp(iface, "xdg_wm_base"))
        n->wm_base = wlc_bind(c, n->registry, name, &lm_xdg_wm_base_interface, LM_MIN(ver, 3u), wm_base_event, n);
    else if (!strcmp(iface, "wl_seat") && !n->seat)
        n->seat = wlc_bind(c, n->registry, name, &lm_wl_seat_interface, LM_MIN(ver, 5u), seat_event, n);
}

static void host_readable(int fd, uint32_t events, void *data)
{
    (void)fd;
    struct nested *n = data;
    if ((events & (EPOLLHUP | EPOLLERR)) || wlc_dispatch_pending(n->c) < 0 || wlc_dead(n->c)) {
        lm_error("nested: the connection to the host compositor was lost");
        lm_server_quit(n->base.server);
        return;
    }
    wlc_flush(n->c);
}

/* ---- the backend --------------------------------------------------------------------------------------------------- */

static int nb_init(struct lm_backend *b)
{
    struct nested *n = NB(b);
    n->c = wlc_connect(n->host[0] ? n->host : NULL);
    if (!n->c) {
        lm_error("nested backend: cannot connect to the host compositor '%s' (is WAYLAND_DISPLAY right? WSL needs WSLg: Windows 11 or a recent Windows 10)",
                 n->host[0] ? n->host : "wayland-0");
        return -1;
    }
    n->registry = wlc_get_registry(n->c, registry_event, n);
    if (wlc_roundtrip(n->c) < 0 || !n->compositor || !n->shm || !n->wm_base) {
        lm_error("nested backend: the host compositor lacks wl_compositor, wl_shm or xdg_wm_base");
        return -1;
    }
    wlc_roundtrip(n->c);                                            /* (the seat's capabilities, then its devices) */
    n->surface = wlc_alloc(n->c, &lm_wl_surface_interface, NULL, NULL);
    WLC_SEND(n->c, n->compositor, LM_WL_COMPOSITOR_REQ_CREATE_SURFACE, { .n = n->surface });
    n->xsurface = wlc_alloc(n->c, &lm_xdg_surface_interface, xsurface_event, n);
    WLC_SEND(n->c, n->wm_base, LM_XDG_WM_BASE_REQ_GET_XDG_SURFACE, { .n = n->xsurface }, { .o = n->surface });
    n->toplevel = wlc_alloc(n->c, &lm_xdg_toplevel_interface, toplevel_event, n);
    WLC_SEND(n->c, n->xsurface, LM_XDG_SURFACE_REQ_GET_TOPLEVEL, { .n = n->toplevel });
    WLC_SEND(n->c, n->toplevel, LM_XDG_TOPLEVEL_REQ_SET_TITLE, { .s = "MellaGUI" });
    WLC_SEND(n->c, n->toplevel, LM_XDG_TOPLEVEL_REQ_SET_APP_ID, { .s = "mellagui.nested" });
    WLC_SEND0(n->c, n->surface, LM_WL_SURFACE_REQ_COMMIT);
    n->src = lm_loop_add_fd(b->server->loop, wlc_fd(n->c), EPOLLIN, host_readable, n);
    for (int i = 0; i < 100 && !n->configured; i++)                 /* the first configure says how big the window is (or leaves it to us) */
        if (wlc_dispatch(n->c, 50) < 0)
            return -1;
    if (!n->configured) {
        lm_error("nested backend: the host compositor never configured the window");
        return -1;
    }
    if (buffers_make(n) < 0)
        return -1;

    memset(&n->out, 0, sizeof(n->out));
    n->out.backend = b;
    snprintf(n->out.name, sizeof(n->out.name), "wayland-0");
    snprintf(n->out.make, sizeof(n->out.make), "MellaGUI");
    snprintf(n->out.model, sizeof(n->out.model), "nested");
    lm_output_set_mode(&n->out, n->w, n->h);
    n->out.refresh_mhz = 60000;
    n->out.phys_width_mm = n->w * 254 / 960;
    n->out.phys_height_mm = n->h * 254 / 960;
    lm_list_append(&b->outputs, &n->out.link);
    n->live = true;
    b->session_active = true;
    if (b->listener.output_added)
        b->listener.output_added(b->listener.data, &n->out);
    send_ready(n);
    lm_info("nested backend: %dx%d window on the host compositor '%s'", n->w, n->h, n->host[0] ? n->host : "wayland-0");
    return 0;
}

static void nb_destroy(struct lm_backend *b)
{
    struct nested *n = NB(b);
    if (n->live) {
        n->live = false;
        if (b->listener.output_removed)
            b->listener.output_removed(b->listener.data, &n->out);
        lm_list_remove(&n->out.link);
    }
    if (n->src)
        lm_source_remove(n->src);
    if (n->c) {
        buffers_free(n);
        WLC_SEND0(n->c, n->toplevel, LM_XDG_TOPLEVEL_REQ_DESTROY);
        WLC_SEND0(n->c, n->xsurface, LM_XDG_SURFACE_REQ_DESTROY);
        WLC_SEND0(n->c, n->surface, LM_WL_SURFACE_REQ_DESTROY);
        wlc_flush(n->c);
        wlc_destroy(n->c);
    }
    free(n->fb.pixels);
    free(n);
}

static struct lm_framebuffer *nb_get_framebuffer(struct lm_output *out)
{
    struct nested *n = NB(out->backend);
    if (!n->live || n->waiting || !n->fb.pixels)
        return NULL;
    n->fb.age = n->frames > 0 ? 1 : 0;
    return &n->fb;
}

static void frame_done(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)args;
    struct nested *n = data;
    if (op != LM_WL_CALLBACK_EVT_DONE)
        return;
    wlc_forget(c, id);
    if (n->waiting)
        send_ready(n);
}

static int nb_present(struct lm_output *out, const struct lm_rect *damage, int n_damage)
{
    (void)damage;
    (void)n_damage;
    struct nested *n = NB(out->backend);
    struct nbuf *free_buf = NULL;
    for (int i = 0; i < NBUF && !free_buf; i++)
        if (!n->buf[i].busy)
            free_buf = &n->buf[i];
    if (!free_buf || !n->map) {
        n->ready_owed = true;                                       /* both are with the host: ready follows when one comes back */
        n->waiting = true;
        return -1;
    }
    memcpy(free_buf->pixels, n->fb.pixels, (size_t)n->w * (size_t)n->h * 4);
    uint32_t cb = wlc_alloc(n->c, &lm_wl_callback_interface, frame_done, n);
    WLC_SEND(n->c, n->surface, LM_WL_SURFACE_REQ_FRAME, { .n = cb });
    WLC_SEND(n->c, n->surface, LM_WL_SURFACE_REQ_ATTACH, { .o = free_buf->id }, { .i = 0 }, { .i = 0 });
    WLC_SEND(n->c, n->surface, LM_WL_SURFACE_REQ_DAMAGE, { .i = 0 }, { .i = 0 }, { .i = n->w }, { .i = n->h });
    WLC_SEND0(n->c, n->surface, LM_WL_SURFACE_REQ_COMMIT);
    wlc_flush(n->c);
    free_buf->busy = true;
    n->waiting = true;
    n->frames++;
    return 0;
}

static const struct lm_backend_ops nested_ops = {
    .name = "wayland",
    .init = nb_init,
    .destroy = nb_destroy,
    .get_framebuffer = nb_get_framebuffer,
    .present = nb_present,
};

struct lm_backend *lm_backend_wayland_create(struct lm_server *srv, int w, int h)
{
    struct nested *n = calloc(1, sizeof(*n));
    if (!n)
        return NULL;
    n->base.ops = &nested_ops;
    n->base.server = srv;
    n->base.tint[0] = n->base.tint[1] = n->base.tint[2] = 1.0f;
    n->base.dim = 1.0f;
    lm_list_init(&n->base.outputs);
    n->w = w > 0 ? w : 1280;
    n->h = h > 0 ? h : 720;
    n->want_w = n->w;
    n->want_h = n->h;
    const char *host = getenv("WAYLAND_DISPLAY");                   /* (our own server will change it for the programs it starts) */
    snprintf(n->host, sizeof(n->host), "%s", host && *host ? host : "");
    return &n->base;
}
