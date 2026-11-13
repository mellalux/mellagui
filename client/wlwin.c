#include "wlwin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "font.h"
#include "keymap.h"
#include "uikit.h"
#include "util.h"

/* ---- shm buffer ---------------------------------------------------------------------------------------------- */

static double zoom_of(const struct wlwin *w)
{
    return w->zoom > 0.01 ? w->zoom : 1.0;
}

/* The size the program lays out in: the window's, divided by the zoom. */
static int content_len(const struct wlwin *w, int len)
{
    double z = zoom_of(w);
    return z == 1.0 ? len : LM_MAX((int)lround(len / z), 1);
}

/* Logical (content) pixels to buffer pixels: the buffer scale times the zoom. */
static double fscale(const struct wlwin *w)
{
    return (w->scale > 1 ? w->scale : 1) * zoom_of(w);
}

static int P(double f, int v)
{
    return (int)lround(v * f);
}

void wlwin_set_zoom(struct wlwin *w, int percent)
{
    w->zoom = LM_CLAMP(percent, 25, 800) / 100.0;
    w->redraw = true;
}

static void setup_buffer(struct wlwin *w)
{
    if (w->pool) {
        WLC_SEND0(w->c, w->pool, LM_WL_SHM_POOL_REQ_DESTROY);
        wlc_forget(w->c, w->pool);
        w->pool = 0;
    }
    if (w->pixels)
        munmap(w->pixels, w->map_size);
    w->bw = content_len(w, w->w);
    w->bh = content_len(w, w->h);
    w->scale = 1;
    if (w->hidpi)
        for (int i = 0; i < w->nout; i++)                       /* the biggest scale of any screen: a window may be dragged to it */
            w->scale = LM_MAX(w->scale, w->out_scale[i]);
    w->scale = LM_CLAMP(w->scale, 1, 4);
    w->pw = w->w * w->scale;                                    /* (the buffer is the window's size: the zoom is in the drawing) */
    w->ph = w->h * w->scale;
    w->map_size = (size_t)w->pw * (size_t)w->ph * 4;
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

/* The scale the buffer should have now (1 for a program that did not ask for hidpi). */
static int wanted_scale(const struct wlwin *w)
{
    int s = 1;
    if (w->hidpi)
        for (int i = 0; i < w->nout; i++)
            s = LM_MAX(s, w->out_scale[i]);
    return LM_CLAMP(s, 1, 4);
}

static void draw(struct wlwin *w)
{
    if (w->bw != content_len(w, w->w) || w->bh != content_len(w, w->h) || w->pw != w->w * w->scale || !w->pixels || w->scale != wanted_scale(w)) {
        setup_buffer(w);
        WLC_SEND(w->c, w->surface, LM_WL_SURFACE_REQ_SET_BUFFER_SCALE, { .i = w->scale });
    }
    if (w->argb)
        memset(w->pixels, 0, w->map_size);
    w->ops->paint(w);
    w->buffer = wlc_alloc(w->c, &lm_wl_buffer_interface, buffer_event, w);
    WLC_SEND(w->c, w->pool, LM_WL_SHM_POOL_REQ_CREATE_BUFFER, { .n = w->buffer }, { .i = 0 }, { .i = w->pw },
             { .i = w->ph }, { .i = w->pw * 4 }, { .u = w->argb ? LM_WL_SHM_FORMAT_ARGB8888 : LM_WL_SHM_FORMAT_XRGB8888 });
    WLC_SEND(w->c, w->surface, LM_WL_SURFACE_REQ_ATTACH, { .o = w->buffer }, { .i = 0 }, { .i = 0 });
    WLC_SEND(w->c, w->surface, LM_WL_SURFACE_REQ_DAMAGE, { .i = 0 }, { .i = 0 }, { .i = w->w }, { .i = w->h });
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
    double z = zoom_of(w);
    if (z != 1.0 && (op == LM_WL_POINTER_EVT_ENTER || op == LM_WL_POINTER_EVT_MOTION)) {   /* the pointer is in window pixels: the program thinks in content pixels */
        union lm_arg *m = (union lm_arg *)args;
        int a = op == LM_WL_POINTER_EVT_ENTER ? 2 : 1;                  /* (enter: serial, surface, x, y; motion: time, x, y) */
        m[a].f = lm_fixed_from_double(lm_fixed_to_double(m[a].f) / z);
        m[a + 1].f = lm_fixed_from_double(lm_fixed_to_double(m[a + 1].f) / z);
    }
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

/* A screen told its scale: a hidpi window follows the biggest one. */
static void output_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)c;
    struct wlwin *w = data;
    if (op != LM_WL_OUTPUT_EVT_SCALE)
        return;
    for (int i = 0; i < w->nout; i++)
        if (w->out_id[i] == id) {
            w->out_scale[i] = LM_MAX(args[0].i, 1);
            if (w->hidpi && w->scale != wanted_scale(w))
                w->redraw = true;                               /* (draw() makes the new buffer) */
        }
}

static bool g_hidpi, g_argb;

void wlwin_argb(void)
{
    g_argb = true;
}

void wlwin_move(struct wlwin *w, uint32_t serial)
{
    if (w->toplevel && w->seat)
        WLC_SEND(w->c, w->toplevel, LM_XDG_TOPLEVEL_REQ_MOVE, { .o = w->seat }, { .u = serial });
}

void wlwin_resize(struct wlwin *w, uint32_t serial, uint32_t edges)
{
    if (w->toplevel && w->seat)
        WLC_SEND(w->c, w->toplevel, LM_XDG_TOPLEVEL_REQ_RESIZE, { .o = w->seat }, { .u = serial }, { .u = edges });
}

void wlwin_hidpi(void)
{
    g_hidpi = !getenv("MELLAGUI_NO_HIDPI");                     /* (an escape hatch, and how the tests tell sharp from enlarged) */
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
    else if (!strcmp(iface, "wl_output") && w->nout < 8) {
        uint32_t oid = wlc_bind(c, w->registry, name, &lm_wl_output_interface, LM_MIN(ver, 3u), output_event, w);
        w->out_id[w->nout] = oid;
        w->out_scale[w->nout++] = 1;
    } else if (!strcmp(iface, "mellagui_menu_v1"))
        w->menu = wlc_bind(c, w->registry, name, &lm_mellagui_menu_v1_interface, 1, menu_event, w);
    else if (w->ops->global)
        w->ops->global(w, name, iface, ver);
}

/* ---- window lifecycle ---------------------------------------------------------------------------------------- */

int wlwin_connect(struct wlwin *w, const char *name, const struct wlwin_ops *ops, void *data)
{
    memset(w, 0, sizeof(*w));
    w->hidpi = g_hidpi;
    w->argb = g_argb;
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
    double f = fscale(w);
    int ya = LM_MAX(P(f, y0), 0), yb = LM_MIN(P(f, y0 + height), w->ph), xa = LM_MAX(P(f, x0), 0), xb = LM_MIN(P(f, x0 + width), w->pw);
    for (int y = ya; y < yb; y++)
        for (int x = xa; x < xb; x++)
            w->pixels[(size_t)y * (size_t)w->pw + (size_t)x] = 0xff000000u | rgb;
}

void wlwin_round_fill(struct wlwin *w, int x, int y, int width, int height, int r, uint32_t rgb)
{
    double f = fscale(w);
    int px = P(f, x), py = P(f, y), pwid = P(f, x + width) - px, phei = P(f, y + height) - py, pr = P(f, r);
    if (w->argb)
        lm_round_fill_argb(w->pixels, w->pw, w->pw, w->ph, px, py, pwid, phei, pr, rgb);
    else
        lm_round_fill(w->pixels, w->pw, w->pw, w->ph, px, py, pwid, phei, pr, rgb);
}

void wlwin_dim(struct wlwin *w, double t)
{
    for (size_t i = 0, n = (size_t)w->pw * (size_t)w->ph; i < n; i++)
        w->pixels[i] = lm_mix_rgb(w->pixels[i], 0x000000, t);
}

void wlwin_blit(struct wlwin *w, int x, int y, int width, int height, const uint32_t *src, int sw, int sh, int stride)
{
    double f = fscale(w);
    int dx0 = P(f, x), dy0 = P(f, y), dw = P(f, x + width) - dx0, dh = P(f, y + height) - dy0;
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0)
        return;
    for (int py = LM_MAX(dy0, 0); py < LM_MIN(dy0 + dh, w->ph); py++) {
        const uint32_t *srow = src + (size_t)LM_MIN((py - dy0) * sh / dh, sh - 1) * (size_t)stride;
        uint32_t *drow = w->pixels + (size_t)py * (size_t)w->pw;
        for (int px = LM_MAX(dx0, 0); px < LM_MIN(dx0 + dw, w->pw); px++)
            drow[px] = 0xff000000u | srow[LM_MIN((px - dx0) * sw / dw, sw - 1)];
    }
}

void wlwin_cell(struct wlwin *w, int x, int y, uint32_t cp, int cw, int ch, uint32_t rgb, bool bold)
{
    double f = fscale(w);
    int pcw = P(f, cw), pch = P(f, ch);
    if (pcw < 1 || pch < 1)
        return;
    const uint8_t *g = lm_font_cell_styled(cp, pcw, pch, bold);
    x = P(f, x);
    y = P(f, y);
    for (int r = 0; r < pch; r++) {
        int py = y + r;
        if (py < 0 || py >= w->ph)
            continue;
        uint32_t *row = w->pixels + (size_t)py * (size_t)w->pw;
        for (int c = 0; c < pcw; c++) {
            int px = x + c;
            unsigned a = g[r * pcw + c];
            if (a && px >= 0 && px < w->pw)
                row[px] = lm_font_mix(row[px], 0xff000000u | rgb, a);
        }
    }
}

void wlwin_blit_px(struct wlwin *w, int x, int y, const uint32_t *src, int sw, int sh, int stride, int cx, int cy, int cw, int ch)
{
    int s = w->scale > 1 ? w->scale : 1;
    int x0 = LM_MAX(LM_MAX(cx * s, x * s), 0), x1 = LM_MIN(LM_MIN((cx + cw) * s, x * s + sw), w->pw);
    int y0 = LM_MAX(LM_MAX(cy * s, y * s), 0), y1 = LM_MIN(LM_MIN((cy + ch) * s, y * s + sh), w->ph);
    for (int py = y0; py < y1; py++) {
        const uint32_t *srow = src + (size_t)(py - y * s) * (size_t)stride - (size_t)(x * s);
        uint32_t *drow = w->pixels + (size_t)py * (size_t)w->pw;
        for (int px = x0; px < x1; px++)
            drow[px] = 0xff000000u | srow[px];
    }
}

void wlwin_glyph(struct wlwin *w, int x, int y, uint32_t cp, int scale, uint32_t rgb)
{
    if (scale < 1)
        return;
    double f = fscale(w);
    int cw = P(f, LM_FONT_W * scale), ch = P(f, LM_FONT_H * scale);
    if (cw < 1 || ch < 1)
        return;
    const uint8_t *g = lm_font_cell(cp, cw, ch);
    x = P(f, x);
    y = P(f, y);
    for (int r = 0; r < ch; r++) {
        int py = y + r;
        if (py < 0 || py >= w->ph)
            continue;
        uint32_t *row = w->pixels + (size_t)py * (size_t)w->pw;
        for (int cx = 0; cx < cw; cx++) {
            int px = x + cx;
            unsigned a = g[r * cw + cx];
            if (a && px >= 0 && px < w->pw)
                row[px] = lm_font_mix(row[px], 0xff000000u | rgb, a);
        }
    }
}

void wlwin_text_cells(struct wlwin *w, int x, int y, const char *s, int scale, uint32_t rgb, int max_w)
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

static void text_styled(struct wlwin *w, int x, int y, const char *s, int scale, uint32_t rgb, int max_w, bool bold);

void wlwin_text_small(struct wlwin *w, int x, int y, const char *s, double size, uint32_t rgb, int max_w)
{
    if (!lm_font_proportional() || size >= 1.0 || size < 0.5) {
        wlwin_text(w, x, y, s, 1, rgb, max_w);
        return;
    }
    double f = fscale(w);
    size_t len = strlen(s);
    int cum = 0;
    int py = P(f, y) + (int)((LM_FONT_H * (1.0 - size)) * f / 2.0 + 0.5);           /* the smaller line sits in the middle of the normal one's height */
    for (size_t i = 0; i < len;) {
        int used;
        uint32_t cp = lm_utf8_decode(s + i, (int)(len - i), &used);
        i += used > 0 ? (size_t)used : 1;
        struct lm_font_glyph g;
        lm_font_label_scaled(cp, size * f, false, &g);
        int adv = (int)((double)g.advance / f + 0.5);                               /* logical pixels */
        if (max_w > 0 && cum + adv > max_w)
            break;
        int gx = P(f, x) + cum * (int)(f + 0.5) - g.left;
        for (int r = 0; r < g.h; r++) {
            int row_y = py + r;
            if (row_y < 0 || row_y >= w->ph)
                continue;
            uint32_t *row = w->pixels + (size_t)row_y * (size_t)w->pw;
            const uint8_t *src = g.px + (size_t)r * (size_t)g.w;
            for (int cx = LM_MAX(0, -gx); cx < g.w && gx + cx < w->pw; cx++)
                if (src[cx])
                    row[gx + cx] = lm_font_mix(row[gx + cx], 0xff000000u | rgb, src[cx]);
        }
        cum += adv;
    }
}

void wlwin_text(struct wlwin *w, int x, int y, const char *s, int scale, uint32_t rgb, int max_w)
{
    if (scale < 1)
        return;
    if (!lm_font_proportional()) {
        wlwin_text_cells(w, x, y, s, scale, rgb, max_w);
        return;
    }
    text_styled(w, x, y, s, scale, rgb, max_w, false);
}

void wlwin_text_bold(struct wlwin *w, int x, int y, const char *s, int scale, uint32_t rgb, int max_w)
{
    if (scale >= 1)
        text_styled(w, x, y, s, scale, rgb, max_w, true);
}

/* The text of a zoomed window: every letter is made at scale * zoom, but placed where the layout (which counts in whole pixels at scale 1) puts it. */
static void text_zoomed(struct wlwin *w, int x, int y, const char *s, int scale, uint32_t rgb, int max_w, bool bold)
{
    double f = fscale(w);
    size_t len = strlen(s);
    int cum = 0, py = P(f, y);
    for (size_t i = 0; i < len;) {
        int used;
        uint32_t cp = lm_utf8_decode(s + i, (int)(len - i), &used);
        i += used > 0 ? (size_t)used : 1;
        struct lm_font_glyph g1;
        lm_font_label_styled(cp, scale, bold, &g1);
        int adv = g1.advance;
        if (max_w > 0 && cum + adv > max_w)
            break;
        struct lm_font_glyph g;
        lm_font_label_scaled(cp, scale * f, bold, &g);
        int gx = P(f, x + cum) - g.left;
        for (int r = 0; r < g.h; r++) {
            int row_y = py + r;
            if (row_y < 0 || row_y >= w->ph)
                continue;
            uint32_t *row = w->pixels + (size_t)row_y * (size_t)w->pw;
            const uint8_t *src = g.px + (size_t)r * (size_t)g.w;
            for (int cx = LM_MAX(0, -gx); cx < g.w && gx + cx < w->pw; cx++)
                if (src[cx])
                    row[gx + cx] = lm_font_mix(row[gx + cx], 0xff000000u | rgb, src[cx]);
        }
        cum += adv;
    }
}

static void text_styled(struct wlwin *w, int x, int y, const char *s, int scale, uint32_t rgb, int max_w, bool bold)
{
    if (zoom_of(w) != 1.0) {
        text_zoomed(w, x, y, s, scale, rgb, max_w, bold);
        return;
    }
    int k = w->scale > 1 ? w->scale : 1;
    x *= k;                                                     /* from here on physical pixels */
    y *= k;
    max_w *= k;
    size_t len = strlen(s);
    int x0 = x;
    for (size_t i = 0; i < len;) {
        int used;
        uint32_t cp = lm_utf8_decode(s + i, (int)(len - i), &used);
        i += used > 0 ? (size_t)used : 1;
        struct lm_font_glyph g;
        lm_font_label_styled(cp, scale * k, bold, &g);
        if (max_w > 0 && x + g.advance > x0 + max_w)
            break;
        int gx = x - g.left;
        for (int r = 0; r < g.h; r++) {
            int py = y + r;
            if (py < 0 || py >= w->ph)
                continue;
            uint32_t *row = w->pixels + (size_t)py * (size_t)w->pw;
            const uint8_t *src = g.px + (size_t)r * (size_t)g.w;
            for (int cx = LM_MAX(0, -gx); cx < g.w && gx + cx < w->pw; cx++)
                if (src[cx])
                    row[gx + cx] = lm_font_mix(row[gx + cx], 0xff000000u | rgb, src[cx]);
        }
        x += g.advance;
    }
}
