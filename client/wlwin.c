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

/* The drawing area's width: the window's, divided by the zoom, less the rail's. */
static int content_w(const struct wlwin *w)
{
    return LM_MAX(content_len(w, w->w) - w->inx, 1);
}

static int P(double f, int v)
{
    return (int)lround(v * f);
}

void wlwin_set_inset(struct wlwin *w, int left)
{
    w->inx = LM_MAX(left, 0);
    w->redraw = true;
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
    w->bw = content_w(w);
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

/* ---- card windows ---------------------------------------------------------------------------------------------------- */

bool wlwin_panes(const struct wlwin *w)
{
    static int off = -1;
    if (off < 0)
        off = getenv("MELLAGUI_NO_PANES") != NULL;
    return !off && !w->no_panes && !w->argb && w->menu && w->menu_ver >= 2 && w->toplevel && zoom_of(w) == 1.0;
}

bool wlwin_pane_shown(const struct wlwin *w, int pane)
{
    return pane >= 0 && pane < WLPANE_N && w->pane[pane].surface != 0;
}

void wlwin_pane_move(struct wlwin *w, int pane)
{
    if (wlwin_pane_shown(w, pane) && w->seat)
        WLC_SEND(w->c, w->pane[pane].toplevel, LM_XDG_TOPLEVEL_REQ_MOVE, { .o = w->seat }, { .u = w->button_serial });
}

static void pane_buffer_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data);

/* the picture goes to the compositor when it may: the window has been configured and the last buffer is back */
static void pane_flush(struct wlpane *p)
{
    struct wlwin *w = p->win;
    if (!p->surface || !p->configured || p->busy || !p->dirty)
        return;
    size_t size = (size_t)p->sw * (size_t)p->sh * 4;
    int fd = memfd_create("card", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)size) < 0) {
        if (fd >= 0)
            close(fd);
        return;
    }
    uint32_t *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED) {
        close(fd);
        return;
    }
    memcpy(px, p->last, size);
    munmap(px, size);
    uint32_t pool = wlc_alloc(w->c, &lm_wl_shm_pool_interface, NULL, NULL);
    WLC_SEND(w->c, w->shm, LM_WL_SHM_REQ_CREATE_POOL, { .n = pool }, { .h = fd }, { .i = (int32_t)size });
    close(fd);
    p->buffer = wlc_alloc(w->c, &lm_wl_buffer_interface, pane_buffer_event, p);
    WLC_SEND(w->c, pool, LM_WL_SHM_POOL_REQ_CREATE_BUFFER, { .n = p->buffer }, { .i = 0 }, { .i = p->sw }, { .i = p->sh }, { .i = p->sw * 4 }, { .u = LM_WL_SHM_FORMAT_ARGB8888 });
    WLC_SEND0(w->c, pool, LM_WL_SHM_POOL_REQ_DESTROY);
    wlc_forget(w->c, pool);
    WLC_SEND(w->c, p->surface, LM_WL_SURFACE_REQ_SET_BUFFER_SCALE, { .i = p->scale });
    WLC_SEND(w->c, p->surface, LM_WL_SURFACE_REQ_ATTACH, { .o = p->buffer }, { .i = 0 }, { .i = 0 });
    WLC_SEND(w->c, p->surface, LM_WL_SURFACE_REQ_DAMAGE, { .i = 0 }, { .i = 0 }, { .i = p->w }, { .i = p->h });
    WLC_SEND0(w->c, p->surface, LM_WL_SURFACE_REQ_COMMIT);
    p->busy = true;
    p->dirty = false;
    wlc_flush(w->c);                    /* (also from an event's handler: a program with a loop of its own would not send it before the next event) */
}

static void pane_buffer_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)args;
    struct wlpane *p = data;
    if (op != LM_WL_BUFFER_EVT_RELEASE)
        return;
    WLC_SEND0(c, id, LM_WL_BUFFER_REQ_DESTROY);
    wlc_forget(c, id);
    if (p->buffer == id) {
        p->buffer = 0;
        p->busy = false;
        pane_flush(p);
    }
}

static void pane_xsurface_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    struct wlpane *p = data;
    if (op != LM_XDG_SURFACE_EVT_CONFIGURE)
        return;
    WLC_SEND(c, id, LM_XDG_SURFACE_REQ_ACK_CONFIGURE, { .u = args[0].u });
    if (!p->configured) {
        p->configured = true;
        pane_flush(p);
    }
}

static void pane_toplevel_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)c; (void)id; (void)op; (void)args; (void)data;                /* the size is ours; it is closed by its program */
}

static void pane_close(struct wlwin *w, int i)
{
    struct wlpane *p = &w->pane[i];
    if (p->surface) {
        WLC_SEND0(w->c, p->toplevel, LM_XDG_TOPLEVEL_REQ_DESTROY);
        wlc_forget(w->c, p->toplevel);
        WLC_SEND0(w->c, p->xsurface, LM_XDG_SURFACE_REQ_DESTROY);
        wlc_forget(w->c, p->xsurface);
        WLC_SEND0(w->c, p->surface, LM_WL_SURFACE_REQ_DESTROY);
        wlc_forget(w->c, p->surface);
        if (p->buffer) {
            WLC_SEND0(w->c, p->buffer, LM_WL_BUFFER_REQ_DESTROY);
            wlc_forget(w->c, p->buffer);
        }
    }
    free(p->img);
    free(p->last);
    memset(p, 0, sizeof(*p));
    if (w->on_pane == i)
        w->on_pane = -1;
}

static void pane_place(struct wlwin *w, const struct wlpane *p)
{
    WLC_SEND(w->c, w->menu, LM_MELLAGUI_MENU_V1_REQ_PLACE, { .o = p->surface }, { .i = p->at_x }, { .i = p->at_y });
}

bool wlwin_pane_begin(struct wlwin *w, int i, int x, int y, int width, int height, int at_x, int at_y, int radius, uint32_t edge, bool modal)
{
    wlwin_pane_end(w);
    if (!wlwin_panes(w) || i < 0 || i >= WLPANE_N || width < 1 || height < 1 || width > 4000 || height > 4000)
        return false;
    struct wlpane *p = &w->pane[i];
    int s = w->scale > 1 ? w->scale : 1, sw = width * s, sh = height * s;
    size_t n = (size_t)sw * (size_t)sh;
    if (p->surface && i == WLPANE_CARD && p->modal != modal)
        pane_close(w, i);                                               /* (a dialog and a floating panel are two kinds of window) */
    if (n > p->cap) {
        free(p->img);
        free(p->last);
        p->img = malloc(n * 4);
        p->last = malloc(n * 4);
        p->cap = p->img && p->last ? n : 0;
        p->sw = p->sh = 0;
        if (!p->cap)
            return false;
    }
    if (sw != p->sw || sh != p->sh)
        memset(p->last, 0, n * 4);                                      /* (whatever is painted is new) */
    p->win = w;
    p->sw = sw;
    p->sh = sh;
    p->scale = s;
    p->x = x;
    p->y = y;
    p->w = width;
    p->h = height;
    p->r = radius;
    p->modal = modal;
    int ax = at_x + w->inx, ay = at_y;
    if (p->surface && (ax != p->at_x || ay != p->at_y)) {
        p->at_x = ax;
        p->at_y = ay;
        pane_place(w, p);
    }
    p->at_x = ax;
    p->at_y = ay;
    for (size_t k = 0; k < n; k++)
        p->img[k] = 0xff000000u | edge;
    w->main_px = w->pixels;
    w->main_pw = w->pw;
    w->main_ph = w->ph;
    w->main_inx = w->inx;
    w->pixels = p->img;
    w->pw = sw;
    w->ph = sh;
    w->inx = -x;
    w->iny = -y;
    w->cur_pane = i;
    return true;
}

/* the corners are rounded off: outside them the picture is see-through */
static void pane_round(struct wlpane *p)
{
    int r = LM_MIN(p->r * p->scale, LM_MIN(p->sw, p->sh) / 2);
    for (int cy = 0; cy < r; cy++)
        for (int cx = 0; cx < r; cx++) {
            double dx = r - cx - 0.5, dy = r - cy - 0.5, cov = (double)r + 0.5 - sqrt(dx * dx + dy * dy);
            if (cov >= 1.0)
                continue;
            uint32_t a = cov <= 0.0 ? 0 : (uint32_t)(cov * 255.0 + 0.5);
            const int xs[2] = { cx, p->sw - 1 - cx }, ys[2] = { cy, p->sh - 1 - cy };
            for (int i = 0; i < 2; i++)
                for (int j = 0; j < 2; j++) {
                    uint32_t *q = &p->img[(size_t)ys[j] * (size_t)p->sw + (size_t)xs[i]], c = *q;
                    *q = a << 24 | (((c >> 16) & 255) * a / 255) << 16 | (((c >> 8) & 255) * a / 255) << 8 | ((c & 255) * a / 255);
                }
        }
}

void wlwin_pane_end(struct wlwin *w)
{
    if (w->cur_pane < 0)
        return;
    struct wlpane *p = &w->pane[w->cur_pane];
    w->pixels = w->main_px;
    w->pw = w->main_pw;
    w->ph = w->main_ph;
    w->inx = w->main_inx;
    w->iny = 0;
    w->cur_pane = -1;
    for (size_t k = 0, n = (size_t)p->sw * (size_t)p->sh; k < n; k++)
        p->img[k] |= 0xff000000u;                                       /* (what was blended may have lost its alpha) */
    pane_round(p);
    p->used = true;
}

/* after the window's own picture: the panes that were painted are (made and) given their pictures, the others are closed */
static void panes_commit(struct wlwin *w)
{
    for (int i = 0; i < WLPANE_N; i++) {
        struct wlpane *p = &w->pane[i];
        if (!p->used) {
            if (p->surface || p->img)
                pane_close(w, i);
            continue;
        }
        if (!p->surface) {
            char id[64];
            snprintf(id, sizeof(id), "%s%s", w->app_id, i == WLPANE_TOOL ? ".tool" : p->modal ? ".card" : ".panel");
            p->surface = wlc_alloc(w->c, &lm_wl_surface_interface, NULL, NULL);
            WLC_SEND(w->c, w->compositor, LM_WL_COMPOSITOR_REQ_CREATE_SURFACE, { .n = p->surface });
            p->xsurface = wlc_alloc(w->c, &lm_xdg_surface_interface, pane_xsurface_event, p);
            WLC_SEND(w->c, w->wm_base, LM_XDG_WM_BASE_REQ_GET_XDG_SURFACE, { .n = p->xsurface }, { .o = p->surface });
            p->toplevel = wlc_alloc(w->c, &lm_xdg_toplevel_interface, pane_toplevel_event, p);
            WLC_SEND(w->c, p->xsurface, LM_XDG_SURFACE_REQ_GET_TOPLEVEL, { .n = p->toplevel });
            WLC_SEND(w->c, p->toplevel, LM_XDG_TOPLEVEL_REQ_SET_PARENT, { .o = w->toplevel });
            WLC_SEND(w->c, p->toplevel, LM_XDG_TOPLEVEL_REQ_SET_TITLE, { .s = w->name });
            WLC_SEND(w->c, p->toplevel, LM_XDG_TOPLEVEL_REQ_SET_APP_ID, { .s = id });
            pane_place(w, p);
            WLC_SEND0(w->c, p->surface, LM_WL_SURFACE_REQ_COMMIT);
            p->dirty = true;
            memcpy(p->last, p->img, (size_t)p->sw * (size_t)p->sh * 4);
        } else if (memcmp(p->last, p->img, (size_t)p->sw * (size_t)p->sh * 4)) {
            memcpy(p->last, p->img, (size_t)p->sw * (size_t)p->sh * 4);
            p->dirty = true;
        }
        pane_flush(p);
    }
}

static void draw(struct wlwin *w)
{
    if (w->bw != content_w(w) || w->bh != content_len(w, w->h) || w->pw != w->w * w->scale || !w->pixels || w->scale != wanted_scale(w)) {
        setup_buffer(w);
        WLC_SEND(w->c, w->surface, LM_WL_SURFACE_REQ_SET_BUFFER_SCALE, { .i = w->scale });
    }
    if (w->argb)
        memset(w->pixels, 0, w->map_size);
    if (!w->card_shown)                                                 /* no dialog card in the last picture: the next one opens in the middle again */
        w->card_dx = w->card_dy = 0, w->card_drag = false;
    w->card_shown = false;
    for (int i = 0; i < WLPANE_N; i++)
        w->pane[i].used = false;
    w->ops->paint(w);
    wlwin_pane_end(w);
    w->buffer = wlc_alloc(w->c, &lm_wl_buffer_interface, buffer_event, w);
    WLC_SEND(w->c, w->pool, LM_WL_SHM_POOL_REQ_CREATE_BUFFER, { .n = w->buffer }, { .i = 0 }, { .i = w->pw },
             { .i = w->ph }, { .i = w->pw * 4 }, { .u = w->argb ? LM_WL_SHM_FORMAT_ARGB8888 : LM_WL_SHM_FORMAT_XRGB8888 });
    WLC_SEND(w->c, w->surface, LM_WL_SURFACE_REQ_ATTACH, { .o = w->buffer }, { .i = 0 }, { .i = 0 });
    WLC_SEND(w->c, w->surface, LM_WL_SURFACE_REQ_DAMAGE, { .i = 0 }, { .i = 0 }, { .i = w->w }, { .i = w->h });
    WLC_SEND0(w->c, w->surface, LM_WL_SURFACE_REQ_COMMIT);
    w->buffer_busy = true;
    w->redraw = false;
    panes_commit(w);
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

static void cursor_buffer_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)args;
    (void)data;
    if (op != LM_WL_BUFFER_EVT_RELEASE)
        return;
    WLC_SEND0(c, id, LM_WL_BUFFER_REQ_DESTROY);
    wlc_forget(c, id);
}

/* The stored picture on the cursor surface (a new buffer, attached and committed): a surface the compositor took the role of the cursor from has no content. */
static void cursor_upload(struct wlwin *w)
{
    if (!w->cur_px || !w->cur_surface)
        return;
    size_t size = (size_t)w->cur_w * (size_t)w->cur_h * 4;
    int fd = memfd_create("cursor", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)size) < 0) {
        if (fd >= 0)
            close(fd);
        return;
    }
    uint32_t *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED) {
        close(fd);
        return;
    }
    memcpy(px, w->cur_px, size);
    munmap(px, size);
    uint32_t pool = wlc_alloc(w->c, &lm_wl_shm_pool_interface, NULL, NULL);
    WLC_SEND(w->c, w->shm, LM_WL_SHM_REQ_CREATE_POOL, { .n = pool }, { .h = fd }, { .i = (int32_t)size });
    close(fd);
    uint32_t buf = wlc_alloc(w->c, &lm_wl_buffer_interface, cursor_buffer_event, w);
    WLC_SEND(w->c, pool, LM_WL_SHM_POOL_REQ_CREATE_BUFFER, { .n = buf }, { .i = 0 }, { .i = w->cur_w }, { .i = w->cur_h }, { .i = w->cur_w * 4 }, { .u = LM_WL_SHM_FORMAT_ARGB8888 });
    WLC_SEND0(w->c, pool, LM_WL_SHM_POOL_REQ_DESTROY);
    wlc_forget(w->c, pool);
    WLC_SEND(w->c, w->cur_surface, LM_WL_SURFACE_REQ_ATTACH, { .o = buf }, { .i = 0 }, { .i = 0 });
    WLC_SEND(w->c, w->cur_surface, LM_WL_SURFACE_REQ_DAMAGE, { .i = 0 }, { .i = 0 }, { .i = w->cur_w }, { .i = w->cur_h });
    WLC_SEND0(w->c, w->cur_surface, LM_WL_SURFACE_REQ_COMMIT);
}

/* set_cursor with the serial of the last enter; the picture goes up first (the surface may be empty again). */
static void send_cursor(struct wlwin *w)
{
    if (!w->cur_set || !w->cur_serial || !w->cur_surface || !w->pointer)
        return;
    cursor_upload(w);
    WLC_SEND(w->c, w->pointer, LM_WL_POINTER_REQ_SET_CURSOR, { .u = w->cur_serial }, { .o = w->cur_surface }, { .i = w->cur_hx }, { .i = w->cur_hy });
}

void wlwin_cursor(struct wlwin *w, const uint32_t *argb, int cw, int ch, int hx, int hy)
{
    if (!w->compositor || !w->shm || !w->pointer || cw < 1 || ch < 1 || cw > 256 || ch > 256)
        return;
    if (!w->cur_surface) {
        w->cur_surface = wlc_alloc(w->c, &lm_wl_surface_interface, NULL, NULL);
        WLC_SEND(w->c, w->compositor, LM_WL_COMPOSITOR_REQ_CREATE_SURFACE, { .n = w->cur_surface });
    }
    uint32_t *copy = malloc((size_t)cw * (size_t)ch * 4);
    if (!copy)
        return;
    for (size_t i = 0; i < (size_t)cw * (size_t)ch; i++) {                  /* the protocol wants premultiplied colour */
        uint32_t c = argb[i], a = c >> 24;
        copy[i] = (a << 24) | ((((c >> 16) & 255) * a / 255) << 16) | ((((c >> 8) & 255) * a / 255) << 8) | ((c & 255) * a / 255);
    }
    free(w->cur_px);
    w->cur_px = copy;
    w->cur_w = cw;
    w->cur_h = ch;
    w->cur_hx = hx;
    w->cur_hy = hy;
    w->cur_set = true;
    if (w->cur_serial)
        send_cursor(w);
}

static void pointer_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)c; (void)id;
    struct wlwin *w = data;
    double z = zoom_of(w);
    if (op == LM_WL_POINTER_EVT_ENTER) {
        w->cur_serial = args[0].u;
        send_cursor(w);
        w->on_pane = -1;
        for (int i = 0; i < WLPANE_N; i++)
            if (w->pane[i].surface && w->pane[i].surface == args[1].o)
                w->on_pane = i;
    }
    if (op == LM_WL_POINTER_EVT_BUTTON && args[3].u == 1)
        w->button_serial = args[0].u;
    if (w->on_pane == WLPANE_TOOL) {                                    /* a panel: a press carries it, the program hears nothing of it */
        if (op == LM_WL_POINTER_EVT_BUTTON && args[3].u == 1 && args[2].u == 0x110)
            wlwin_pane_move(w, WLPANE_TOOL);
        if (op == LM_WL_POINTER_EVT_LEAVE)
            w->on_pane = -1;
        return;
    }
    if (op == LM_WL_POINTER_EVT_ENTER || op == LM_WL_POINTER_EVT_MOTION) {   /* the pointer is in window pixels: the program thinks in content pixels */
        union lm_arg *m = (union lm_arg *)args;
        int a = op == LM_WL_POINTER_EVT_ENTER ? 2 : 1;                  /* (enter: serial, surface, x, y; motion: time, x, y) */
        w->rpx = lm_fixed_to_double(m[a].f) / z;
        w->rpy = lm_fixed_to_double(m[a + 1].f) / z;
        const struct wlpane *card = &w->pane[WLPANE_CARD];
        if (w->on_pane == WLPANE_CARD) {                                /* on the card window: where the program drew it, wherever it is now */
            m[a].f = lm_fixed_from_double(card->x + w->rpx);
            m[a + 1].f = lm_fixed_from_double(card->y + w->rpy);
            w->rpx = 100000.0;                                          /* (not over a rail at the left) */
        } else if (card->surface && card->modal && w->rpx - w->inx >= card->x && w->rpx - w->inx < card->x + card->w && w->rpy >= card->y && w->rpy < card->y + card->h) {
            m[a].f = lm_fixed_from_double(-10000.0);                    /* on the window, where the card was drawn but is not: outside it */
            m[a + 1].f = lm_fixed_from_double(-10000.0);
        } else if (z != 1.0 || w->inx) {
            m[a].f = lm_fixed_from_double(w->rpx - w->inx);
            m[a + 1].f = lm_fixed_from_double(w->rpy);
        }
    }
    w->ops->pointer(w, op, args);
    if (op == LM_WL_POINTER_EVT_LEAVE)
        w->on_pane = -1;
}

/* ---- the pointer lock ---- */

static void locked_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)c; (void)id; (void)args;
    struct wlwin *w = data;
    if (op != LM_ZWP_LOCKED_POINTER_V1_EVT_LOCKED && op != LM_ZWP_LOCKED_POINTER_V1_EVT_UNLOCKED)
        return;
    w->locked = op == LM_ZWP_LOCKED_POINTER_V1_EVT_LOCKED;
    if (w->ops->lock)
        w->ops->lock(w, w->locked);
}

static void relative_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)c; (void)id;
    struct wlwin *w = data;
    if (op != LM_ZWP_RELATIVE_POINTER_V1_EVT_RELATIVE_MOTION || !w->ops->relative)
        return;
    double z = zoom_of(w);
    w->ops->relative(w, lm_fixed_to_double(args[2].f) / z, lm_fixed_to_double(args[3].f) / z);
}

bool wlwin_lock_pointer(struct wlwin *w, bool on)
{
    if (!on) {
        if (w->lockobj) {
            WLC_SEND0(w->c, w->lockobj, LM_ZWP_LOCKED_POINTER_V1_REQ_DESTROY);
            wlc_forget(w->c, w->lockobj);
            w->lockobj = 0;
        }
        if (w->relobj) {
            WLC_SEND0(w->c, w->relobj, LM_ZWP_RELATIVE_POINTER_V1_REQ_DESTROY);
            wlc_forget(w->c, w->relobj);
            w->relobj = 0;
        }
        w->locked = false;
        return true;
    }
    if (!w->pconstraints || !w->pointer || !w->surface)
        return false;
    if (w->lockobj)
        return true;
    w->lockobj = wlc_alloc(w->c, &lm_zwp_locked_pointer_v1_interface, locked_event, w);
    WLC_SEND(w->c, w->pconstraints, LM_ZWP_POINTER_CONSTRAINTS_V1_REQ_LOCK_POINTER, { .n = w->lockobj }, { .o = w->surface }, { .o = w->pointer },
             { .o = 0 }, { .u = LM_ZWP_POINTER_CONSTRAINTS_LIFETIME_PERSISTENT });
    if (w->relmgr && !w->relobj) {
        w->relobj = wlc_alloc(w->c, &lm_zwp_relative_pointer_v1_interface, relative_event, w);
        WLC_SEND(w->c, w->relmgr, LM_ZWP_RELATIVE_POINTER_MANAGER_V1_REQ_GET_RELATIVE_POINTER, { .n = w->relobj }, { .o = w->pointer });
    }
    return true;
}

void wlwin_hide_cursor(struct wlwin *w, bool hide)
{
    if (!w->pointer || !w->cur_serial)
        return;
    if (hide)
        WLC_SEND(w->c, w->pointer, LM_WL_POINTER_REQ_SET_CURSOR, { .u = w->cur_serial }, { .o = 0 }, { .i = 0 }, { .i = 0 });
    else
        send_cursor(w);
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

void wlwin_warp_pointer(struct wlwin *w, int x, int y)
{
    if (!w->menu)
        return;
    double z = zoom_of(w);
    WLC_SEND(w->c, w->menu, LM_MELLAGUI_MENU_V1_REQ_WARP, { .i = (int32_t)lround(x * z) }, { .i = (int32_t)lround(y * z) });
}

void wlwin_raise(struct wlwin *w)
{
    if (!w->menu || !w->surface)
        return;
    WLC_SEND(w->c, w->menu, LM_MELLAGUI_MENU_V1_REQ_RAISE, { .o = w->surface });
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
        w->menu = wlc_bind(c, w->registry, name, &lm_mellagui_menu_v1_interface, w->menu_ver = LM_MIN(ver, 2u), menu_event, w);
    else if (!strcmp(iface, "zwp_pointer_constraints_v1"))
        w->pconstraints = wlc_bind(c, w->registry, name, &lm_zwp_pointer_constraints_v1_interface, 1, NULL, w);
    else if (!strcmp(iface, "zwp_relative_pointer_manager_v1"))
        w->relmgr = wlc_bind(c, w->registry, name, &lm_zwp_relative_pointer_manager_v1_interface, 1, NULL, w);
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
    w->cur_pane = w->on_pane = -1;
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
    def_w += w->inx;
    min_w += w->inx;
    w->def_w = w->w = def_w;
    w->def_h = w->h = def_h;
    w->min_w = min_w;
    w->min_h = min_h;
    snprintf(w->app_id, sizeof(w->app_id), "%s", app_id);
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
    for (int i = 0; i < WLPANE_N; i++) {
        free(w->pane[i].img);
        free(w->pane[i].last);
        w->pane[i].img = w->pane[i].last = NULL;
    }
    free(w->cur_px);
    w->cur_px = NULL;
    wlc_destroy(w->c);
    w->c = NULL;
}

/* ---- drawing ------------------------------------------------------------------------------------------------- */

void wlwin_fill(struct wlwin *w, int x0, int y0, int width, int height, uint32_t rgb)
{
    x0 += w->inx;
    y0 += w->iny;
    double f = fscale(w);
    int ya = LM_MAX(P(f, y0), 0), yb = LM_MIN(P(f, y0 + height), w->ph), xa = LM_MAX(P(f, x0), 0), xb = LM_MIN(P(f, x0 + width), w->pw);
    for (int y = ya; y < yb; y++)
        for (int x = xa; x < xb; x++)
            w->pixels[(size_t)y * (size_t)w->pw + (size_t)x] = 0xff000000u | rgb;
}

void wlwin_round_fill(struct wlwin *w, int x, int y, int width, int height, int r, uint32_t rgb)
{
    x += w->inx;
    y += w->iny;
    double f = fscale(w);
    int px = P(f, x), py = P(f, y), pwid = P(f, x + width) - px, phei = P(f, y + height) - py, pr = P(f, r);
    if (w->argb)
        lm_round_fill_argb(w->pixels, w->pw, w->pw, w->ph, px, py, pwid, phei, pr, rgb);
    else
        lm_round_fill(w->pixels, w->pw, w->pw, w->ph, px, py, pwid, phei, pr, rgb);
}

/* A coloured button as a bead of water; the drawing is lm_drop_px (uikit.h), here only the scale is worked out. */
void wlwin_drop(struct wlwin *w, int x, int y, int bw, int bh, int r, uint32_t c)
{
    if (w->argb) {                                                      /* (a card over the desktop: plain) */
        wlwin_round_fill(w, x, y, bw, bh, r, c);
        return;
    }
    x += w->inx;
    y += w->iny;
    double f = fscale(w);
    int px = P(f, x), py = P(f, y), pwid = P(f, x + bw) - px, phei = P(f, y + bh) - py, pr = P(f, r);
    lm_drop_px(w->pixels, w->pw, w->pw, w->ph, px, py, pwid, phei, pr, c, LM_MAX((int)(f + 0.5), 1));
}

void wlwin_dim(struct wlwin *w, double t)
{
    for (size_t i = 0, n = (size_t)w->pw * (size_t)w->ph; i < n; i++)
        w->pixels[i] = lm_mix_rgb(w->pixels[i], 0x000000, t);
}

void wlwin_blit(struct wlwin *w, int x, int y, int width, int height, const uint32_t *src, int sw, int sh, int stride)
{
    x += w->inx;
    y += w->iny;
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
    x += w->inx;
    y += w->iny;
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
    x += w->inx;
    cx += w->inx;
    y += w->iny;
    cy += w->iny;
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

void wlwin_blit_px_over(struct wlwin *w, int x, int y, const uint32_t *src, int sw, int sh, int stride)
{
    x += w->inx;
    y += w->iny;
    int s = w->scale > 1 ? w->scale : 1;
    int x0 = LM_MAX(x * s, 0), x1 = LM_MIN(x * s + sw, w->pw);
    int y0 = LM_MAX(y * s, 0), y1 = LM_MIN(y * s + sh, w->ph);
    for (int py = y0; py < y1; py++) {
        const uint32_t *srow = src + (size_t)(py - y * s) * (size_t)stride - (size_t)(x * s);
        uint32_t *drow = w->pixels + (size_t)py * (size_t)w->pw;
        for (int px = x0; px < x1; px++) {
            uint32_t sp = srow[px], a = sp >> 24;
            if (a == 255) {
                drow[px] = sp;
            } else if (a) {
                uint32_t d = drow[px];
                uint32_t ia = 255 - a;
                uint32_t r = (((sp >> 16) & 255) * a + ((d >> 16) & 255) * ia + 127) / 255;
                uint32_t g = (((sp >> 8) & 255) * a + ((d >> 8) & 255) * ia + 127) / 255;
                uint32_t b = ((sp & 255) * a + (d & 255) * ia + 127) / 255;
                drow[px] = 0xff000000u | r << 16 | g << 8 | b;
            }
        }
    }
}

void wlwin_glyph(struct wlwin *w, int x, int y, uint32_t cp, int scale, uint32_t rgb)
{
    if (scale < 1)
        return;
    x += w->inx;
    y += w->iny;
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
    x += w->inx;
    y += w->iny;
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
    x += w->inx;
    y += w->iny;
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
