/*
 * lumen-demo: minimal Wayland client showing an animated gradient, the
 * pointer/keyboard state and a text line. Exercises wlc, shm, xdg-shell,
 * frame callbacks, wl_seat and the bitmap font.
 */
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "font.h"
#include "util.h"
#include "keymap.h"
#include "wlc.h"

struct app {
    struct wlc *c;
    uint32_t registry, compositor, shm, wm_base, seat, pointer, keyboard;
    uint32_t surface, xsurface, toplevel, frame_cb;
    uint32_t pool, buffer;
    uint32_t seat_name, compositor_name;
    int w, h;
    int bw, bh;
    uint32_t *pixels;
    size_t map_size;
    bool configured, running, buffer_busy, redraw;
    double px, py;
    bool has_pointer;
    uint32_t mods;
    char last_key[64];
    int frames;
    int fd_pool;
};

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void draw_text(struct app *a, int x, int y, const char *s, uint32_t color)
{
    size_t len = strlen(s);
    for (size_t i = 0; i < len;) {
        int used;
        uint32_t cp = lm_utf8_decode(s + i, (int)(len - i), &used);
        i += used > 0 ? (size_t)used : 1;
        const uint8_t *g = lm_font_glyph(cp);
        for (int r = 0; r < LM_FONT_H; r++)
            for (int cx = 0; cx < LM_FONT_W; cx++)
                if ((g[r] & (0x80 >> cx)) && x + cx >= 0 && x + cx < a->bw && y + r >= 0 && y + r < a->bh)
                    a->pixels[(size_t)(y + r) * (size_t)a->bw + (size_t)(x + cx)] = color;
        x += LM_FONT_W;
    }
}

static void setup_buffer(struct app *a)
{
    if (a->buffer) {
        WLC_SEND0(a->c, a->buffer, LM_WL_BUFFER_REQ_DESTROY);
        wlc_forget(a->c, a->buffer);
        a->buffer = 0;
    }
    if (a->pool) {
        WLC_SEND0(a->c, a->pool, LM_WL_SHM_POOL_REQ_DESTROY);
        wlc_forget(a->c, a->pool);
        a->pool = 0;
    }
    if (a->pixels)
        munmap(a->pixels, a->map_size);
    a->bw = a->w;
    a->bh = a->h;
    a->map_size = (size_t)a->bw * (size_t)a->bh * 4;
    int fd = memfd_create("lumen-demo", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)a->map_size) < 0) {
        perror("memfd");
        exit(1);
    }
    a->pixels = mmap(NULL, a->map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    a->pool = wlc_alloc(a->c, &lm_wl_shm_pool_interface, NULL, NULL);
    WLC_SEND(a->c, a->shm, LM_WL_SHM_REQ_CREATE_POOL, { .n = a->pool }, { .h = fd }, { .i = (int32_t)a->map_size });
    close(fd);
}

static void buffer_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)c; (void)id; (void)args;
    struct app *a = data;
    if (op == LM_WL_BUFFER_EVT_RELEASE)
        a->buffer_busy = false;
}

static void frame_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data);

static void draw(struct app *a)
{
    if (!a->configured || a->buffer_busy || a->w <= 0)
        return;
    if (a->bw != a->w || a->bh != a->h || !a->pixels)
        setup_buffer(a);
    double t = (double)now_ms() / 1000.0;
    for (int y = 0; y < a->bh; y++)
        for (int x = 0; x < a->bw; x++) {
            double u = (double)x / a->bw, v = (double)y / a->bh;
            uint8_t r = (uint8_t)(127 + 127 * sin(6.0 * u + t));
            uint8_t g = (uint8_t)(127 + 127 * sin(6.0 * v + t * 1.3));
            uint8_t b = (uint8_t)(127 + 127 * sin(6.0 * (u + v) - t * 0.7));
            a->pixels[(size_t)y * (size_t)a->bw + (size_t)x] = 0xff000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | b;
        }
    char line[128];
    draw_text(a, 12, 12, "Lumen demo client", 0xffffffff);
    snprintf(line, sizeof(line), "size %dx%d  frames %d", a->bw, a->bh, a->frames);
    draw_text(a, 12, 32, line, 0xffffffff);
    if (a->has_pointer) {
        snprintf(line, sizeof(line), "pointer %.0f,%.0f", a->px, a->py);
        draw_text(a, 12, 52, line, 0xffffffff);
    }
    if (a->last_key[0])
        draw_text(a, 12, 72, a->last_key, 0xffffffff);

    a->buffer = wlc_alloc(a->c, &lm_wl_buffer_interface, buffer_event, a);
    WLC_SEND(a->c, a->pool, LM_WL_SHM_POOL_REQ_CREATE_BUFFER, { .n = a->buffer }, { .i = 0 }, { .i = a->bw },
             { .i = a->bh }, { .i = a->bw * 4 }, { .u = LM_WL_SHM_FORMAT_XRGB8888 });
    a->frame_cb = wlc_alloc(a->c, &lm_wl_callback_interface, frame_event, a);
    WLC_SEND(a->c, a->surface, LM_WL_SURFACE_REQ_FRAME, { .n = a->frame_cb });
    WLC_SEND(a->c, a->surface, LM_WL_SURFACE_REQ_ATTACH, { .o = a->buffer }, { .i = 0 }, { .i = 0 });
    WLC_SEND(a->c, a->surface, LM_WL_SURFACE_REQ_DAMAGE, { .i = 0 }, { .i = 0 }, { .i = a->bw }, { .i = a->bh });
    WLC_SEND0(a->c, a->surface, LM_WL_SURFACE_REQ_COMMIT);
    a->buffer_busy = true;
    a->redraw = false;
    a->frames++;
}

static void frame_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)op; (void)args;
    struct app *a = data;
    wlc_forget(c, id);
    a->frame_cb = 0;
    a->redraw = true;
}

static void wm_base_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)data;
    if (op == LM_XDG_WM_BASE_EVT_PING)
        WLC_SEND(c, id, LM_XDG_WM_BASE_REQ_PONG, { .u = args[0].u });
}

static void xsurface_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    struct app *a = data;
    if (op == LM_XDG_SURFACE_EVT_CONFIGURE) {
        WLC_SEND(c, id, LM_XDG_SURFACE_REQ_ACK_CONFIGURE, { .u = args[0].u });
        a->configured = true;
        a->redraw = true;
    }
}

static void toplevel_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)c; (void)id;
    struct app *a = data;
    if (op == LM_XDG_TOPLEVEL_EVT_CONFIGURE) {
        if (args[0].i > 0 && args[1].i > 0) {
            a->w = args[0].i;
            a->h = args[1].i;
        }
    } else if (op == LM_XDG_TOPLEVEL_EVT_CLOSE) {
        a->running = false;
    }
}

static void pointer_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)c; (void)id;
    struct app *a = data;
    switch (op) {
    case LM_WL_POINTER_EVT_ENTER:
        a->has_pointer = true;
        a->px = lm_fixed_to_double(args[2].f);
        a->py = lm_fixed_to_double(args[3].f);
        break;
    case LM_WL_POINTER_EVT_LEAVE:
        a->has_pointer = false;
        break;
    case LM_WL_POINTER_EVT_MOTION:
        a->px = lm_fixed_to_double(args[1].f);
        a->py = lm_fixed_to_double(args[2].f);
        break;
    }
}

static void keyboard_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)c; (void)id;
    struct app *a = data;
    switch (op) {
    case LM_WL_KEYBOARD_EVT_KEYMAP:
        close(args[1].h);
        break;
    case LM_WL_KEYBOARD_EVT_MODIFIERS:
        a->mods = args[1].u | args[3].u;
        break;
    case LM_WL_KEYBOARD_EVT_KEY:
        if (args[3].u == 1) {
            uint32_t ks = lm_keymap_us_keysym(args[2].u, a->mods);
            char txt[8];
            if (lm_keysym_to_utf8(ks, txt))
                snprintf(a->last_key, sizeof(a->last_key), "last key: '%s' (code %u)", txt, args[2].u);
            else
                snprintf(a->last_key, sizeof(a->last_key), "last key: keysym 0x%x (code %u)", ks, args[2].u);
            a->redraw = true;
            if (args[2].u == 1)
                a->running = false; /* Esc quits */
        }
        break;
    }
}

static void seat_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    struct app *a = data;
    if (op == LM_WL_SEAT_EVT_CAPABILITIES) {
        if ((args[0].u & 1) && !a->pointer) {
            a->pointer = wlc_alloc(c, &lm_wl_pointer_interface, pointer_event, a);
            WLC_SEND(c, id, LM_WL_SEAT_REQ_GET_POINTER, { .n = a->pointer });
        }
        if ((args[0].u & 2) && !a->keyboard) {
            a->keyboard = wlc_alloc(c, &lm_wl_keyboard_interface, keyboard_event, a);
            WLC_SEND(c, id, LM_WL_SEAT_REQ_GET_KEYBOARD, { .n = a->keyboard });
        }
    }
}

static void registry_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)id;
    struct app *a = data;
    if (op != LM_WL_REGISTRY_EVT_GLOBAL)
        return;
    uint32_t name = args[0].u, ver = args[2].u;
    const char *iface = args[1].s;
    if (!strcmp(iface, "wl_compositor"))
        a->compositor = wlc_bind(c, a->registry, name, &lm_wl_compositor_interface, LM_MIN(ver, 4u), NULL, a);
    else if (!strcmp(iface, "wl_shm"))
        a->shm = wlc_bind(c, a->registry, name, &lm_wl_shm_interface, 1, NULL, a);
    else if (!strcmp(iface, "xdg_wm_base"))
        a->wm_base = wlc_bind(c, a->registry, name, &lm_xdg_wm_base_interface, LM_MIN(ver, 3u), wm_base_event, a);
    else if (!strcmp(iface, "wl_seat"))
        a->seat = wlc_bind(c, a->registry, name, &lm_wl_seat_interface, LM_MIN(ver, 5u), seat_event, a);
}

int main(void)
{
    struct app a = { .w = 480, .h = 320, .running = true };
    a.c = wlc_connect(NULL);
    if (!a.c)
        return 1;
    a.registry = wlc_get_registry(a.c, registry_event, &a);
    if (wlc_roundtrip(a.c) < 0 || !a.compositor || !a.shm || !a.wm_base) {
        fprintf(stderr, "lumen-demo: compositor lacks wl_compositor/wl_shm/xdg_wm_base\n");
        return 1;
    }
    a.surface = wlc_alloc(a.c, &lm_wl_surface_interface, NULL, NULL);
    WLC_SEND(a.c, a.compositor, LM_WL_COMPOSITOR_REQ_CREATE_SURFACE, { .n = a.surface });
    a.xsurface = wlc_alloc(a.c, &lm_xdg_surface_interface, xsurface_event, &a);
    WLC_SEND(a.c, a.wm_base, LM_XDG_WM_BASE_REQ_GET_XDG_SURFACE, { .n = a.xsurface }, { .o = a.surface });
    a.toplevel = wlc_alloc(a.c, &lm_xdg_toplevel_interface, toplevel_event, &a);
    WLC_SEND(a.c, a.xsurface, LM_XDG_SURFACE_REQ_GET_TOPLEVEL, { .n = a.toplevel });
    WLC_SEND(a.c, a.toplevel, LM_XDG_TOPLEVEL_REQ_SET_TITLE, { .s = "Lumen demo" });
    WLC_SEND(a.c, a.toplevel, LM_XDG_TOPLEVEL_REQ_SET_APP_ID, { .s = "lumen.demo" });
    WLC_SEND0(a.c, a.surface, LM_WL_SURFACE_REQ_COMMIT);

    while (a.running && !wlc_dead(a.c)) {
        if (a.redraw || (a.configured && !a.frame_cb && !a.buffer_busy))
            draw(&a);
        if (wlc_dispatch(a.c, a.redraw ? 0 : 100) < 0)
            break;
    }
    wlc_destroy(a.c);
    return 0;
}
