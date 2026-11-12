#include "wlwin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "font.h"
#include "keymap.h"
#include "util.h"

/* ---- shm buffer ---------------------------------------------------------------------------------------------- */

static void setup_buffer(struct wlwin *w)
{
    if (w->pool) {
        WLC_SEND0(w->c, w->pool, LM_WL_SHM_POOL_REQ_DESTROY);
        wlc_forget(w->c, w->pool);
        w->pool = 0;
    }
    if (w->pixels)
        munmap(w->pixels, w->map_size);
    w->bw = w->w;
    w->bh = w->h;
    w->map_size = (size_t)w->bw * (size_t)w->bh * 4;
    int fd = memfd_create(w->name, MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)w->map_size) < 0) {
        perror("memfd");
        exit(1);
    }
    w->pixels = mmap(NULL, w->map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (w->pixels == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }
    w->pool = wlc_alloc(w->c, &lm_wl_shm_pool_interface, NULL, NULL);
    WLC_SEND(w->c, w->shm, LM_WL_SHM_REQ_CREATE_POOL, { .n = w->pool }, { .h = fd }, { .i = (int32_t)w->map_size });
    close(fd);
}

static void buffer_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)args;
    struct wlwin *w = data;
    if (op != LM_WL_BUFFER_EVT_RELEASE)
        return;
    WLC_SEND0(c, id, LM_WL_BUFFER_REQ_DESTROY);
    wlc_forget(c, id);
    w->buffer = 0;
    w->buffer_busy = false;
}

static void draw(struct wlwin *w)
{
    if (w->bw != w->w || w->bh != w->h || !w->pixels)
        setup_buffer(w);
    w->ops->paint(w);
    w->buffer = wlc_alloc(w->c, &lm_wl_buffer_interface, buffer_event, w);
    WLC_SEND(w->c, w->pool, LM_WL_SHM_POOL_REQ_CREATE_BUFFER, { .n = w->buffer }, { .i = 0 }, { .i = w->bw },
             { .i = w->bh }, { .i = w->bw * 4 }, { .u = LM_WL_SHM_FORMAT_XRGB8888 });
    WLC_SEND(w->c, w->surface, LM_WL_SURFACE_REQ_ATTACH, { .o = w->buffer }, { .i = 0 }, { .i = 0 });
    WLC_SEND(w->c, w->surface, LM_WL_SURFACE_REQ_DAMAGE, { .i = 0 }, { .i = 0 }, { .i = w->bw }, { .i = w->bh });
    WLC_SEND0(w->c, w->surface, LM_WL_SURFACE_REQ_COMMIT);
    w->buffer_busy = true;
    w->redraw = false;
}

/* ---- protocol events ----------------------------------------------------------------------------------------- */

static void wm_base_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)data;
    if (op == LM_XDG_WM_BASE_EVT_PING)
        WLC_SEND(c, id, LM_XDG_WM_BASE_REQ_PONG, { .u = args[0].u });
}

static void xsurface_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    struct wlwin *w = data;
    if (op == LM_XDG_SURFACE_EVT_CONFIGURE) {
        WLC_SEND(c, id, LM_XDG_SURFACE_REQ_ACK_CONFIGURE, { .u = args[0].u });
        w->configured = true;
        w->redraw = true;
    }
}

static void toplevel_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)c; (void)id;
    struct wlwin *w = data;
    if (op == LM_XDG_TOPLEVEL_EVT_CONFIGURE) {
        w->w = LM_MAX(args[0].i > 0 ? args[0].i : w->def_w, w->min_w);
        w->h = LM_MAX(args[1].i > 0 ? args[1].i : w->def_h, w->min_h);
        w->fixed_size = false;
        const uint32_t *st = args[2].a.data;
        for (uint32_t i = 0; st && i < args[2].a.size / 4; i++)
            if (st[i] == LM_XDG_TOPLEVEL_STATE_MAXIMIZED || st[i] == LM_XDG_TOPLEVEL_STATE_FULLSCREEN)
                w->fixed_size = true;
    } else if (op == LM_XDG_TOPLEVEL_EVT_CLOSE) {
        if (!w->ops->close || w->ops->close(w))
            w->running = false;
    }
}

static void pointer_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)c; (void)id;
    struct wlwin *w = data;
    w->ops->pointer(w, op, args);
}

static void keyboard_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)c; (void)id;
    struct wlwin *w = data;
    if (op == LM_WL_KEYBOARD_EVT_KEYMAP) {
        lm_keymap_client_load(args[1].h, args[2].u);
    } else if (op == LM_WL_KEYBOARD_EVT_MODIFIERS) {
        w->mods = args[1].u | args[2].u | args[3].u;
        lm_keymap_client_group(args[4].u);
    }
    w->ops->keyboard(w, op, args);
}

static void seat_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    struct wlwin *w = data;
    if (op != LM_WL_SEAT_EVT_CAPABILITIES)
        return;
    if ((args[0].u & 1) && w->ops->pointer && !w->pointer) {
        w->pointer = wlc_alloc(c, &lm_wl_pointer_interface, pointer_event, w);
        WLC_SEND(c, id, LM_WL_SEAT_REQ_GET_POINTER, { .n = w->pointer });
    }
    if ((args[0].u & 2) && w->ops->keyboard && !w->keyboard) {
        w->keyboard = wlc_alloc(c, &lm_wl_keyboard_interface, keyboard_event, w);
        WLC_SEND(c, id, LM_WL_SEAT_REQ_GET_KEYBOARD, { .n = w->keyboard });
    }
}

static void menu_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)c; (void)id;
    struct wlwin *w = data;
    if (!w->ops->menu)
        return;
    if (op == LM_MELLAGUI_MENU_V1_EVT_CHOSEN)
        w->ops->menu(w, args[0].i);
    else if (op == LM_MELLAGUI_MENU_V1_EVT_CLOSED)
        w->ops->menu(w, -1);
}

bool wlwin_menu(struct wlwin *w, const char *entries)
{
    if (!w->menu)
        return false;
    WLC_SEND(w->c, w->menu, LM_MELLAGUI_MENU_V1_REQ_POPUP, { .s = entries });
    return true;
}

static void registry_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)id;
    struct wlwin *w = data;
    if (op != LM_WL_REGISTRY_EVT_GLOBAL)
        return;
    uint32_t name = args[0].u, ver = args[2].u;
    const char *iface = args[1].s;
    if (!strcmp(iface, "wl_compositor"))
        w->compositor = wlc_bind(c, w->registry, name, &lm_wl_compositor_interface, LM_MIN(ver, 4u), NULL, w);
    else if (!strcmp(iface, "wl_shm"))
        w->shm = wlc_bind(c, w->registry, name, &lm_wl_shm_interface, 1, NULL, w);
    else if (!strcmp(iface, "xdg_wm_base"))
        w->wm_base = wlc_bind(c, w->registry, name, &lm_xdg_wm_base_interface, LM_MIN(ver, 3u), wm_base_event, w);
    else if (!strcmp(iface, "wl_seat"))
        w->seat = wlc_bind(c, w->registry, name, &lm_wl_seat_interface, LM_MIN(ver, 5u), seat_event, w);
    else if (!strcmp(iface, "mellagui_menu_v1"))
        w->menu = wlc_bind(c, w->registry, name, &lm_mellagui_menu_v1_interface, 1, menu_event, w);
    else if (w->ops->global)
        w->ops->global(w, name, iface, ver);
}

/* ---- window lifecycle ---------------------------------------------------------------------------------------- */

int wlwin_connect(struct wlwin *w, const char *name, const struct wlwin_ops *ops, void *data)
{
    memset(w, 0, sizeof(*w));
    w->name = name;
    w->ops = ops;
    w->data = data;
    w->running = true;
    w->c = wlc_connect(NULL);
    if (!w->c)
        return -1;
    w->registry = wlc_get_registry(w->c, registry_event, w);
    return wlc_roundtrip(w->c);
}

int wlwin_open(struct wlwin *w, const char *title, const char *app_id, int def_w, int def_h, int min_w, int min_h)
{
    if (!w->compositor || !w->shm || !w->wm_base) {
        fprintf(stderr, "%s: compositor lacks wl_compositor/wl_shm/xdg_wm_base\n", w->name);
        return -1;
    }
    w->def_w = w->w = def_w;
    w->def_h = w->h = def_h;
    w->min_w = min_w;
    w->min_h = min_h;
    w->surface = wlc_alloc(w->c, &lm_wl_surface_interface, NULL, NULL);
    WLC_SEND(w->c, w->compositor, LM_WL_COMPOSITOR_REQ_CREATE_SURFACE, { .n = w->surface });
    w->xsurface = wlc_alloc(w->c, &lm_xdg_surface_interface, xsurface_event, w);
    WLC_SEND(w->c, w->wm_base, LM_XDG_WM_BASE_REQ_GET_XDG_SURFACE, { .n = w->xsurface }, { .o = w->surface });
    w->toplevel = wlc_alloc(w->c, &lm_xdg_toplevel_interface, toplevel_event, w);
    WLC_SEND(w->c, w->xsurface, LM_XDG_SURFACE_REQ_GET_TOPLEVEL, { .n = w->toplevel });
    WLC_SEND(w->c, w->toplevel, LM_XDG_TOPLEVEL_REQ_SET_TITLE, { .s = title });
    WLC_SEND(w->c, w->toplevel, LM_XDG_TOPLEVEL_REQ_SET_APP_ID, { .s = app_id });
    if (w->want_fullscreen)
        WLC_SEND(w->c, w->toplevel, LM_XDG_TOPLEVEL_REQ_SET_FULLSCREEN, { .o = 0 });
    WLC_SEND0(w->c, w->surface, LM_WL_SURFACE_REQ_COMMIT);
    return 0;
}

bool wlwin_step(struct wlwin *w, int timeout_ms)
{
    if (!w->running || wlc_dead(w->c))
        return false;
    if (w->redraw && w->configured && !w->buffer_busy)
        draw(w);
    return wlc_dispatch(w->c, timeout_ms) >= 0;
}

bool wlwin_request_size(struct wlwin *w, int width, int height)
{
    width = LM_MAX(width, w->min_w);
    height = LM_MAX(height, w->min_h);
    if (w->fixed_size || (width == w->w && height == w->h))
        return false;
    w->w = width;
    w->h = height;
    w->redraw = true;
    return true;
}

void wlwin_destroy(struct wlwin *w)
{
    wlc_destroy(w->c);
    w->c = NULL;
}

/* ---- drawing ------------------------------------------------------------------------------------------------- */

void wlwin_fill(struct wlwin *w, int x0, int y0, int width, int height, uint32_t rgb)
{
    for (int y = LM_MAX(y0, 0); y < LM_MIN(y0 + height, w->bh); y++)
        for (int x = LM_MAX(x0, 0); x < LM_MIN(x0 + width, w->bw); x++)
            w->pixels[(size_t)y * (size_t)w->bw + (size_t)x] = 0xff000000u | rgb;
}

void wlwin_glyph(struct wlwin *w, int x, int y, uint32_t cp, int scale, uint32_t rgb)
{
    if (scale < 1)
        return;
    int cw = LM_FONT_W * scale, ch = LM_FONT_H * scale;
    const uint8_t *g = lm_font_cell(cp, cw, ch);
    for (int r = 0; r < ch; r++) {
        int py = y + r;
        if (py < 0 || py >= w->bh)
            continue;
        uint32_t *row = w->pixels + (size_t)py * (size_t)w->bw;
        for (int cx = 0; cx < cw; cx++) {
            int px = x + cx;
            unsigned a = g[r * cw + cx];
            if (a && px >= 0 && px < w->bw)
                row[px] = lm_font_mix(row[px], 0xff000000u | rgb, a);
        }
    }
}

void wlwin_text(struct wlwin *w, int x, int y, const char *s, int scale, uint32_t rgb, int max_w)
{
    size_t len = strlen(s);
    int x0 = x;
    for (size_t i = 0; i < len;) {
        if (max_w > 0 && x + LM_FONT_W * scale > x0 + max_w)
            break;
        int used;
        uint32_t cp = lm_utf8_decode(s + i, (int)(len - i), &used);
        i += used > 0 ? (size_t)used : 1;
        wlwin_glyph(w, x, y, cp, scale, rgb);
        x += LM_FONT_W * scale;
    }
}
