#include "wlclip.h"

#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "protocol.h"
#include "util.h"

static char *g_proc_text;               /* the text this process copied last (any of its windows) ... */
static char g_proc_mime[48];            /* ... and the type that marks an offer of it as ours: a pipe to our own source would never be answered */

static void dsrc_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    struct wlclip *a = data;
    if (op == LM_WL_DATA_SOURCE_EVT_SEND) {
        if (a->out) {
            const char *p = a->out;
            size_t left = strlen(p);
            while (left) {
                ssize_t k = write(args[1].h, p, left);
                if (k <= 0)
                    break;
                p += k;
                left -= (size_t)k;
            }
        }
        close(args[1].h);
    } else if (op == LM_WL_DATA_SOURCE_EVT_CANCELLED) {
        wlc_forget(c, id);
        if (a->dsrc == id)
            a->dsrc = 0;
    }
}

static void doffer_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)c;
    struct wlclip *a = data;
    if (op == LM_WL_DATA_OFFER_EVT_OFFER && id == a->offer_pending && a->n_pend_mimes < 16)
        snprintf(a->pend_mimes[a->n_pend_mimes++], sizeof(a->pend_mimes[0]), "%s", args[0].s);
    else if (op == LM_WL_DATA_OFFER_EVT_ACTION && id == a->drop_offer)
        a->drop_action = args[0].u;
}

static void offer_drop(struct wlc *c, uint32_t *id)
{
    if (*id) {
        WLC_SEND0(c, *id, LM_WL_DATA_OFFER_REQ_DESTROY);
        wlc_forget(c, *id);
        *id = 0;
    }
}

/* "file:///a%20b" -> "/a b" (a host name is skipped; anything that is no local file is left out). */
bool wlclip_uri_to_path(const char *uri, char *out, size_t n)
{
    if (strncmp(uri, "file://", 7))
        return false;
    uri += 7;
    if (*uri != '/') {
        const char *slash = strchr(uri, '/');
        if (!slash)
            return false;
        uri = slash;
    }
    size_t k = 0;
    for (; *uri && k + 1 < n; uri++) {
        if (*uri == '%' && uri[1] && uri[2]) {
            char hex[3] = { uri[1], uri[2], 0 };
            char *end;
            long v = strtol(hex, &end, 16);
            if (*end)
                return false;
            out[k++] = (char)v;
            uri += 2;
        } else {
            out[k++] = *uri;
        }
    }
    out[k] = 0;
    return k > 0;
}

/* The drop: the uri-list is read (at most 256 KB, two seconds of silence ends it), the paths go to the program, the source is told it is finished. */
static void drop_receive(struct wlclip *a)
{
    struct wlc *c = a->win->c;
    uint32_t offer = a->drop_offer;
    size_t cap = 1u << 18, len = 0;
    char *buf = NULL;
    if (a->drag_src && a->drag_uris) {                  /* our own drag dropped on our own window: the list is here (the source cannot answer while we wait) */
        buf = strdup(a->drag_uris);
        len = buf ? strlen(buf) : 0;
    } else {
        int fds[2];
        if (pipe2(fds, O_CLOEXEC) < 0) {
            offer_drop(c, &a->drop_offer);
            return;
        }
        WLC_SEND(c, offer, LM_WL_DATA_OFFER_REQ_RECEIVE, { .s = "text/uri-list" }, { .h = fds[1] });
        close(fds[1]);
        wlc_flush(c);
        buf = malloc(cap + 1);
        struct pollfd pf = { .fd = fds[0], .events = POLLIN };
        while (buf && len < cap && poll(&pf, 1, 2000) > 0) {
            ssize_t k = read(fds[0], buf + len, cap - len);
            if (k <= 0)
                break;
            len += (size_t)k;
        }
        close(fds[0]);
        if (buf)
            buf[len] = 0;
    }
    WLC_SEND0(c, offer, LM_WL_DATA_OFFER_REQ_FINISH);
    offer_drop(c, &a->drop_offer);
    if (!buf || !len || !a->drop) {
        free(buf);
        return;
    }
    char **paths = calloc(512, sizeof(*paths));
    int np = 0;
    for (char *line = strtok(buf, "\r\n"); line && paths && np < 512; line = strtok(NULL, "\r\n")) {
        if (line[0] == '#')
            continue;
        char path[4096];
        if (wlclip_uri_to_path(line, path, sizeof(path)) && (paths[np] = strdup(path)))
            np++;
    }
    if (np)
        a->drop(a, paths, np, a->drop_x, a->drop_y, a->drop_data);
    for (int i = 0; i < np; i++)
        free(paths[i]);
    free(paths);
    free(buf);
}

void wlclip_drag_over(struct wlclip *c, void (*over)(struct wlclip *c, bool inside, double x, double y, void *data))
{
    c->drag_over = over;
}

void wlclip_accept_drops(struct wlclip *c, void (*drop)(struct wlclip *c, char **paths, int n, double x, double y, void *data), void *data)
{
    c->drop = drop;
    c->drop_data = data;
}

static void ddev_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)id;
    struct wlclip *a = data;
    if (op == LM_WL_DATA_DEVICE_EVT_DATA_OFFER) {
        offer_drop(c, &a->offer_pending);               /* (the selection offer in use stays: it is only replaced by the next selection) */
        a->offer_pending = args[0].n;
        a->n_pend_mimes = 0;
        wlc_set_handler(c, a->offer_pending, doffer_event, a);
    } else if (op == LM_WL_DATA_DEVICE_EVT_SELECTION) {
        if (args[0].o && args[0].o == a->offer_pending) {                    /* a new selection: its types are known now */
            offer_drop(c, &a->offer);
            a->offer = a->offer_pending;
            a->offer_pending = 0;
            memcpy(a->offer_mimes, a->pend_mimes, sizeof(a->offer_mimes));
            a->n_offer_mimes = a->n_pend_mimes;
        } else if (!args[0].o) {                                            /* no selection */
            offer_drop(c, &a->offer);
            offer_drop(c, &a->offer_pending);
            a->n_offer_mimes = 0;
        }
    } else if (op == LM_WL_DATA_DEVICE_EVT_ENTER) {
        /* something is dragged over the window: a program that takes drops of files (wlclip_accept_drops) accepts a text/uri-list, the others let the offer go */
        offer_drop(c, &a->drop_offer);
        a->own_target = false;
        bool own = false;
        for (int i = 0; a->own_event && args[4].o && args[4].o == a->offer_pending && i < a->n_pend_mimes; i++)
            own = own || !strcmp(a->pend_mimes[i], a->own_mime);
        if (own) {
            a->drop_offer = a->offer_pending;
            a->offer_pending = 0;
            a->own_target = true;
            a->own_on_main = args[1].o == a->win->surface;
            a->drop_x = lm_fixed_to_double(args[2].f);
            a->drop_y = lm_fixed_to_double(args[3].f);
            WLC_SEND(c, a->drop_offer, LM_WL_DATA_OFFER_REQ_SET_ACTIONS, { .u = LM_DND_ACTION_COPY | LM_DND_ACTION_MOVE }, { .u = LM_DND_ACTION_MOVE });
            WLC_SEND(c, a->drop_offer, LM_WL_DATA_OFFER_REQ_ACCEPT, { .u = args[0].u }, { .s = a->own_mime });
            a->own_event(a, WLCLIP_OWN_OVER, a->drop_x, a->drop_y, a->own_on_main, a->own_data);
        } else if (args[4].o && args[4].o == a->offer_pending) {
            bool uris = false;
            for (int i = 0; i < a->n_pend_mimes; i++)
                uris = uris || !strcmp(a->pend_mimes[i], "text/uri-list");
            if (a->drop && uris) {
                a->drop_offer = a->offer_pending;
                a->offer_pending = 0;
                a->drop_x = lm_fixed_to_double(args[2].f);
                a->drop_y = lm_fixed_to_double(args[3].f);
                a->drop_action = 0;
                WLC_SEND(c, a->drop_offer, LM_WL_DATA_OFFER_REQ_SET_ACTIONS, { .u = LM_DND_ACTION_COPY | LM_DND_ACTION_MOVE }, { .u = LM_DND_ACTION_MOVE });
                WLC_SEND(c, a->drop_offer, LM_WL_DATA_OFFER_REQ_ACCEPT, { .u = args[0].u }, { .s = "text/uri-list" });
                if (a->drag_over)
                    a->drag_over(a, true, a->drop_x, a->drop_y, a->drop_data);
            } else {
                offer_drop(c, &a->offer_pending);
            }
        }
    } else if (op == LM_WL_DATA_DEVICE_EVT_MOTION) {
        a->drop_x = lm_fixed_to_double(args[1].f);
        a->drop_y = lm_fixed_to_double(args[2].f);
        if (a->drop_offer && a->own_target)
            a->own_event(a, WLCLIP_OWN_OVER, a->drop_x, a->drop_y, a->own_on_main, a->own_data);
        else if (a->drop_offer && a->drag_over)
            a->drag_over(a, true, a->drop_x, a->drop_y, a->drop_data);
    } else if (op == LM_WL_DATA_DEVICE_EVT_LEAVE) {
        if (a->drop_offer && a->own_target)
            a->own_event(a, WLCLIP_OWN_LEAVE, a->drop_x, a->drop_y, a->own_on_main, a->own_data);
        else if (a->drop_offer && a->drag_over)
            a->drag_over(a, false, a->drop_x, a->drop_y, a->drop_data);
        offer_drop(c, &a->drop_offer);
    } else if (op == LM_WL_DATA_DEVICE_EVT_DROP && a->drop_offer && a->own_target) {
        a->own_event(a, WLCLIP_OWN_DROP, a->drop_x, a->drop_y, a->own_on_main, a->own_data);
        WLC_SEND0(c, a->drop_offer, LM_WL_DATA_OFFER_REQ_FINISH);
        offer_drop(c, &a->drop_offer);
        wlc_roundtrip(c);                               /* (the finish is with the compositor before a window of ours that this emptied is destroyed) */
    } else if (op == LM_WL_DATA_DEVICE_EVT_DROP && a->drop_offer) {
        drop_receive(a);
    }
}

void wlclip_global(struct wlclip *c, struct wlwin *w, uint32_t name, const char *iface, uint32_t ver)
{
    if (!strcmp(iface, "wl_data_device_manager"))
        c->ddm = wlc_bind(w->c, w->registry, name, &lm_wl_data_device_manager_interface, LM_MIN(ver, 3u), NULL, NULL);
}

void wlclip_attach(struct wlclip *c, struct wlwin *w)
{
    c->win = w;
    if (c->ddm && w->seat) {
        c->ddev = wlc_alloc(w->c, &lm_wl_data_device_interface, ddev_event, c);
        WLC_SEND(w->c, c->ddm, LM_WL_DATA_DEVICE_MANAGER_REQ_GET_DATA_DEVICE, { .n = c->ddev }, { .o = w->seat });
    }
}

void wlclip_copy(struct wlclip *c, const char *text)
{
    free(c->out);
    c->out = strdup(text);
    if (!c->ddm || !c->ddev)
        return;
    struct wlwin *w = c->win;
    if (c->dsrc) {
        WLC_SEND0(w->c, c->dsrc, LM_WL_DATA_SOURCE_REQ_DESTROY);
        wlc_forget(w->c, c->dsrc);
    }
    c->dsrc = wlc_alloc(w->c, &lm_wl_data_source_interface, dsrc_event, c);
    WLC_SEND(w->c, c->ddm, LM_WL_DATA_DEVICE_MANAGER_REQ_CREATE_DATA_SOURCE, { .n = c->dsrc });
    WLC_SEND(w->c, c->dsrc, LM_WL_DATA_SOURCE_REQ_OFFER, { .s = "text/plain;charset=utf-8" });
    WLC_SEND(w->c, c->dsrc, LM_WL_DATA_SOURCE_REQ_OFFER, { .s = "text/plain" });
    free(g_proc_text);
    g_proc_text = strdup(text);
    snprintf(g_proc_mime, sizeof(g_proc_mime), "application/x-mellagui-own-%ld", (long)getpid());
    WLC_SEND(w->c, c->dsrc, LM_WL_DATA_SOURCE_REQ_OFFER, { .s = g_proc_mime });
    WLC_SEND(w->c, c->ddev, LM_WL_DATA_DEVICE_REQ_SET_SELECTION, { .o = c->dsrc }, { .u = c->serial });
}

static const char *const paste_mimes[] = { "text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "STRING", "TEXT" };

char *wlclip_paste(struct wlclip *c, size_t *len_out)
{
    char *text = NULL;
    size_t len = 0;
    if (c->dsrc && c->out) {                                            /* our own copy: no round trip through the compositor */
        text = strdup(c->out);
        len = text ? strlen(text) : 0;
    } else if (c->offer && g_proc_text && wlclip_has_mime(c, g_proc_mime)) {      /* copied in another window of ours */
        text = strdup(g_proc_text);
        len = text ? strlen(text) : 0;
    } else if (c->offer) {
        const char *mime = NULL;
        for (size_t m = 0; m < sizeof(paste_mimes) / sizeof(paste_mimes[0]) && !mime; m++)
            for (int i = 0; i < c->n_offer_mimes; i++)
                if (!strcmp(c->offer_mimes[i], paste_mimes[m])) {
                    mime = c->offer_mimes[i];
                    break;
                }
        int fds[2];
        if (!mime || pipe2(fds, O_CLOEXEC) < 0)
            return NULL;
        WLC_SEND(c->win->c, c->offer, LM_WL_DATA_OFFER_REQ_RECEIVE, { .s = mime }, { .h = fds[1] });
        close(fds[1]);
        wlc_flush(c->win->c);
        text = malloc((1u << 20) + 1);
        if (!text) {
            close(fds[0]);
            return NULL;
        }
        struct pollfd pf = { .fd = fds[0], .events = POLLIN };
        while (len < (1u << 20)) {
            if (poll(&pf, 1, 1000) <= 0)
                break;
            ssize_t k = read(fds[0], text + len, (1u << 20) - len);
            if (k <= 0)
                break;
            len += (size_t)k;
        }
        close(fds[0]);
        text[len] = 0;
    }
    if (text && len_out)
        *len_out = len;
    return text;
}

bool wlclip_has_mime(const struct wlclip *c, const char *mime)
{
    if (!c->offer)
        return false;
    for (int i = 0; i < c->n_offer_mimes; i++)
        if (!strcmp(c->offer_mimes[i], mime))
            return true;
    return false;
}

uint8_t *wlclip_paste_mime(struct wlclip *c, const char *mime, size_t maxlen, size_t *len_out)
{
    if (!wlclip_has_mime(c, mime) || !c->win)
        return NULL;
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) < 0)
        return NULL;
    WLC_SEND(c->win->c, c->offer, LM_WL_DATA_OFFER_REQ_RECEIVE, { .s = mime }, { .h = fds[1] });
    close(fds[1]);
    wlc_flush(c->win->c);
    size_t cap = 1 << 16, len = 0;
    uint8_t *buf = malloc(cap);
    struct pollfd pf = { .fd = fds[0], .events = POLLIN };
    bool ok = buf != NULL, done = false;
    while (ok && !done) {
        if (len == cap) {
            if (cap >= maxlen) {                                        /* too big: it is not read */
                ok = false;
                break;
            }
            size_t ncap = cap * 2 > maxlen ? maxlen : cap * 2;
            uint8_t *nb = realloc(buf, ncap);
            if (!nb) {
                ok = false;
                break;
            }
            buf = nb;
            cap = ncap;
        }
        if (poll(&pf, 1, 2000) <= 0) {
            ok = false;                                                 /* a source that stops answering: nothing */
            break;
        }
        ssize_t k = read(fds[0], buf + len, cap - len);
        if (k < 0)
            ok = false;
        else if (k == 0)
            done = true;
        else
            len += (size_t)k;
    }
    close(fds[0]);
    if (!ok || !done || len == 0) {
        free(buf);
        return NULL;
    }
    if (len_out)
        *len_out = len;
    return buf;
}

/* ---- dragging files out ------------------------------------------------------------------------------------------------------------- */

#include <sys/mman.h>
#include <stdio.h>

#include "font.h"
#include "uikit.h"

bool wlclip_dragging(const struct wlclip *c)
{
    return c->drag_src != 0;
}

static void drag_cleanup(struct wlclip *c)
{
    struct wlc *wc = c->win->c;
    if (c->drag_src) {
        WLC_SEND0(wc, c->drag_src, LM_WL_DATA_SOURCE_REQ_DESTROY);
        wlc_forget(wc, c->drag_src);
        c->drag_src = 0;
    }
    if (c->drag_surf) {
        WLC_SEND0(wc, c->drag_surf, LM_WL_SURFACE_REQ_DESTROY);
        wlc_forget(wc, c->drag_surf);
        c->drag_surf = 0;
    }
    if (c->drag_buf) {
        WLC_SEND0(wc, c->drag_buf, LM_WL_BUFFER_REQ_DESTROY);
        wlc_forget(wc, c->drag_buf);
        c->drag_buf = 0;
    }
    if (c->drag_pool) {
        WLC_SEND0(wc, c->drag_pool, LM_WL_SHM_POOL_REQ_DESTROY);
        wlc_forget(wc, c->drag_pool);
        c->drag_pool = 0;
    }
    free(c->drag_clean);
    c->drag_clean = NULL;
    c->drag_action = 0;
    if (c->drag_map) {
        munmap(c->drag_map, c->drag_map_size);
        c->drag_map = NULL;
    }
    free(c->drag_uris);
    free(c->drag_text);
    c->drag_uris = c->drag_text = NULL;
}

static void write_all(int fd, const char *p, size_t left)
{
    int fl = fcntl(fd, F_GETFL);
    if (fl >= 0)
        fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
    while (left) {
        ssize_t k = write(fd, p, left);
        if (k <= 0)
            break;
        p += k;
        left -= (size_t)k;
    }
}

/* The drag icon says what a drop would do: a green disc with a white "+" at its top right corner while it is a COPY (Ctrl held, or a target that only copies);
 * a move (the usual) has nothing, and neither has a place where nothing can be dropped. The picture is redrawn in its shared memory and committed again. */
static void drag_badge(struct wlclip *a, uint32_t action)
{
    if (!a->drag_map || !a->drag_clean || !a->drag_surf || action == a->drag_action)
        return;
    a->drag_action = action;
    uint32_t *px = a->drag_map;
    int iw = a->drag_iw, ih = a->drag_ih, k = a->drag_k;
    memcpy(px, a->drag_clean, a->drag_map_size);
    if (action == LM_DND_ACTION_COPY) {
        int r = 8 * k, cx = iw - r - k, cy = r + k;
        for (int y = LM_MAX(cy - r - 1, 0); y <= LM_MIN(cy + r + 1, ih - 1); y++)
            for (int x = LM_MAX(cx - r - 1, 0); x <= LM_MIN(cx + r + 1, iw - 1); x++) {
                double d = sqrt((double)((x - cx) * (x - cx) + (y - cy) * (y - cy)));
                double cov = d <= r - 0.5 ? 1.0 : d >= r + 0.5 ? 0.0 : r + 0.5 - d;            /* (a soft edge) */
                if (cov <= 0)
                    continue;
                int ax = abs(x - cx), ay = abs(y - cy);
                bool plus = (ay <= k * 3 / 4 + 0 && ax <= r * 11 / 20) || (ax <= k * 3 / 4 + 0 && ay <= r * 11 / 20);
                uint32_t col = plus ? 0xffffff : 0x2e9e4f;
                uint32_t a8 = (uint32_t)(255 * cov);
                uint32_t rr = ((col >> 16) & 255) * a8 / 255, gg = ((col >> 8) & 255) * a8 / 255, bb = (col & 255) * a8 / 255;
                px[(size_t)y * (size_t)iw + (size_t)x] = (a8 << 24) | (rr << 16) | (gg << 8) | bb;
            }
    }
    struct wlc *wc = a->win->c;
    WLC_SEND(wc, a->drag_surf, LM_WL_SURFACE_REQ_ATTACH, { .o = a->drag_buf }, { .i = 0 }, { .i = 0 });      /* (no new offset: the icon stays where it hangs) */
    WLC_SEND(wc, a->drag_surf, LM_WL_SURFACE_REQ_DAMAGE, { .i = 0 }, { .i = 0 }, { .i = iw / k }, { .i = ih / k });
    WLC_SEND0(wc, a->drag_surf, LM_WL_SURFACE_REQ_COMMIT);
    wlc_flush(wc);
}

static void drag_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)c;
    (void)id;
    struct wlclip *a = data;
    if (op == LM_WL_DATA_SOURCE_EVT_SEND) {
        const char *mime = args[0].s;
        const char *body = mime && !strcmp(mime, "text/uri-list") ? a->drag_uris : a->drag_text;
        if (body)
            write_all(args[1].h, body, strlen(body));
        close(args[1].h);                               /* (a handler owns the fds it is given: the reader sees the end of the data) */
    } else if (op == LM_WL_DATA_SOURCE_EVT_ACTION) {
        drag_badge(a, args[0].u);
    } else if (op == LM_WL_DATA_SOURCE_EVT_DND_DROP_PERFORMED) {
        a->own_dropped = true;
    } else if (op == LM_WL_DATA_SOURCE_EVT_CANCELLED || op == LM_WL_DATA_SOURCE_EVT_DND_FINISHED) {
        bool own = a->own_source;
        int how = op == LM_WL_DATA_SOURCE_EVT_DND_FINISHED ? WLCLIP_OWN_TAKEN : a->own_dropped ? WLCLIP_OWN_NOWHERE : WLCLIP_OWN_OFF;
        a->own_source = false;
        drag_cleanup(a);
        if (own && a->own_done)
            a->own_done(a, how, a->own_data);
    }
}

/* A little icon for the drag: the label on a rounded blue chip (premultiplied ARGB; the corners are see-through). */
static bool drag_icon(struct wlclip *c, const char *label, const uint32_t *pic, int pw, int ph, int k)
{
    struct wlwin *w = c->win;
    char text[64];
    snprintf(text, sizeof(text), "%s", label ? label : "");
    int cells = 0;
    for (const char *p = text; *p; p++)
        if (((unsigned char)*p & 0xc0) != 0x80)
            cells++;
    if (cells > 26) {                                   /* a long name: cut it and say so */
        int keep = 0;
        char *q = text;
        while (*q && keep < 25) {
            q++;
            while (((unsigned char)*q & 0xc0) == 0x80)
                q++;
            keep++;
        }
        snprintf(q, sizeof(text) - (size_t)(q - text), "...");
        cells = 28;
    }
    if (k < 1)
        k = 1;
    int cw = LM_FONT_W * k, ch = LM_FONT_H * k;                 /* (in buffer pixels: the surface gets buffer scale k) */
    int chip_w = cells * cw + 20 * k, chip_h = ch + 14 * k;
    int gap = pic ? 4 * k : 0;
    int iw = LM_MAX(chip_w, pic ? pw : 0), ih = chip_h + (pic ? ph + gap : 0);
    int chip_x = (iw - chip_w) / 2, chip_y = pic ? ph + gap : 0;
    size_t size = (size_t)iw * (size_t)ih * 4;
    int fd = memfd_create("mellagui-drag", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)size) < 0) {
        if (fd >= 0)
            close(fd);
        return false;
    }
    uint32_t *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED) {
        close(fd);
        return false;
    }
    memset(px, 0, size);
    const uint32_t bg = 0x2f6fb5;
    if (pic) {                                                  /* the file's own icon (straight ARGB) above the name, made premultiplied for the surface */
        int ox = (iw - pw) / 2;
        for (int y = 0; y < ph; y++)
            for (int x = 0; x < pw; x++) {
                uint32_t v = pic[(size_t)y * (size_t)pw + (size_t)x];
                uint32_t a8 = v >> 24;
                uint32_t r = ((v >> 16) & 255) * a8 / 255, g = ((v >> 8) & 255) * a8 / 255, b = (v & 255) * a8 / 255;
                px[(size_t)y * (size_t)iw + (size_t)(ox + x)] = (a8 << 24) | (r << 16) | (g << 8) | b;
            }
    }
    lm_round_fill_argb(px + (size_t)chip_y * (size_t)iw + (size_t)chip_x, iw, chip_w, chip_h, 0, 0, chip_w, chip_h, 8 * k, bg);
    int x = chip_x + 10 * k, len = (int)strlen(text);
    for (int i = 0; i < len;) {
        int used;
        uint32_t cp = lm_utf8_decode(text + i, len - i, &used);
        i += used > 0 ? used : 1;
        const uint8_t *g = lm_font_cell(cp, cw, ch);
        if (g)
            for (int gy = 0; gy < ch; gy++)
                for (int gx = 0; gx < cw; gx++) {
                    uint32_t cov = g[gy * cw + gx];
                    if (cov)
                        px[(size_t)(chip_y + 7 * k + gy) * (size_t)iw + (size_t)(x + gx)] = lm_mix_rgb(bg, 0xffffff, cov / 255.0);
                }
        x += cw;
    }
    struct wlc *wc = w->c;
    c->drag_map = px;
    c->drag_map_size = size;
    c->drag_iw = iw;
    c->drag_ih = ih;
    c->drag_k = k;
    c->drag_action = 0;
    free(c->drag_clean);
    c->drag_clean = malloc(size);                       /* (the badge is drawn over a copy: the action changes while the pointer moves over windows, Ctrl goes up and down) */
    if (c->drag_clean)
        memcpy(c->drag_clean, px, size);
    c->drag_pool = wlc_alloc(wc, &lm_wl_shm_pool_interface, NULL, NULL);
    WLC_SEND(wc, w->shm, LM_WL_SHM_REQ_CREATE_POOL, { .n = c->drag_pool }, { .h = fd }, { .i = (int32_t)size });
    close(fd);
    c->drag_buf = wlc_alloc(wc, &lm_wl_buffer_interface, NULL, NULL);
    WLC_SEND(wc, c->drag_pool, LM_WL_SHM_POOL_REQ_CREATE_BUFFER, { .n = c->drag_buf }, { .i = 0 }, { .i = iw }, { .i = ih },
             { .i = iw * 4 }, { .u = LM_WL_SHM_FORMAT_ARGB8888 });
    c->drag_surf = wlc_alloc(wc, &lm_wl_surface_interface, NULL, NULL);
    WLC_SEND(wc, w->compositor, LM_WL_COMPOSITOR_REQ_CREATE_SURFACE, { .n = c->drag_surf });
    if (k > 1)
        WLC_SEND(wc, c->drag_surf, LM_WL_SURFACE_REQ_SET_BUFFER_SCALE, { .i = k });
    /* with the file's picture the icon is held by its middle (the offsets of the attach: the surface lies up and left of the pointer by half the icon),
     * so what is under the pointer is what the icon is over; the name chip hangs below it */
    WLC_SEND(wc, c->drag_surf, LM_WL_SURFACE_REQ_ATTACH, { .o = c->drag_buf }, { .i = pic ? -(iw / k) / 2 : 0 }, { .i = pic ? -(ph / k) / 2 : 0 });
    WLC_SEND(wc, c->drag_surf, LM_WL_SURFACE_REQ_DAMAGE, { .i = 0 }, { .i = 0 }, { .i = iw }, { .i = ih });
    WLC_SEND0(wc, c->drag_surf, LM_WL_SURFACE_REQ_COMMIT);
    return true;
}

bool wlclip_drag_files_pic(struct wlclip *c, const char *uris, const char *text, const char *label, const uint32_t *pic, int pw, int ph, int scale)
{
    if (!c->win || !c->ddm || !c->ddev || !uris || !*uris)
        return false;
    struct wlwin *w = c->win;
    if (c->drag_src)                                    /* the last drop was never finished by its target: that drag is over */
        drag_cleanup(c);
    c->own_source = false;
    c->drag_uris = strdup(uris);
    c->drag_text = strdup(text ? text : uris);
    if (!c->drag_uris || !c->drag_text) {
        drag_cleanup(c);
        return false;
    }
    bool icon = drag_icon(c, label, pic && pw > 0 && ph > 0 ? pic : NULL, pw, ph, scale);
    c->drag_src = wlc_alloc(w->c, &lm_wl_data_source_interface, drag_event, c);
    WLC_SEND(w->c, c->ddm, LM_WL_DATA_DEVICE_MANAGER_REQ_CREATE_DATA_SOURCE, { .n = c->drag_src });
    WLC_SEND(w->c, c->drag_src, LM_WL_DATA_SOURCE_REQ_OFFER, { .s = "text/uri-list" });
    WLC_SEND(w->c, c->drag_src, LM_WL_DATA_SOURCE_REQ_OFFER, { .s = "text/plain;charset=utf-8" });
    WLC_SEND(w->c, c->drag_src, LM_WL_DATA_SOURCE_REQ_OFFER, { .s = "text/plain" });
    WLC_SEND(w->c, c->drag_src, LM_WL_DATA_SOURCE_REQ_SET_ACTIONS, { .u = LM_DND_ACTION_COPY | LM_DND_ACTION_MOVE });    /* (the target does the work: a move or a copy; Ctrl = copy) */
    WLC_SEND(w->c, c->ddev, LM_WL_DATA_DEVICE_REQ_START_DRAG, { .o = c->drag_src }, { .o = w->surface },
             { .o = icon ? c->drag_surf : 0 }, { .u = c->ptr_serial ? c->ptr_serial : c->serial });
    wlc_flush(w->c);
    return true;
}

bool wlclip_drag_files(struct wlclip *c, const char *uris, const char *text, const char *label)
{
    return wlclip_drag_files_pic(c, uris, text, label, NULL, 0, 0, 1);
}

void wlclip_accept_own(struct wlclip *c, const char *mime, void (*event)(struct wlclip *c, int ev, double x, double y, bool main_surface, void *data),
                       void (*done)(struct wlclip *c, int how, void *data), void *data)
{
    c->own_mime = mime;
    c->own_event = event;
    c->own_done = done;
    c->own_data = data;
}

bool wlclip_drag_own(struct wlclip *c, const char *label)
{
    if (!c->win || !c->ddm || !c->ddev || !c->own_mime)
        return false;
    struct wlwin *w = c->win;
    if (c->drag_src)
        drag_cleanup(c);
    bool icon = drag_icon(c, label, NULL, 0, 0, w->scale > 1 ? w->scale : 1);
    c->own_source = true;
    c->own_dropped = false;
    c->drag_src = wlc_alloc(w->c, &lm_wl_data_source_interface, drag_event, c);
    WLC_SEND(w->c, c->ddm, LM_WL_DATA_DEVICE_MANAGER_REQ_CREATE_DATA_SOURCE, { .n = c->drag_src });
    WLC_SEND(w->c, c->drag_src, LM_WL_DATA_SOURCE_REQ_OFFER, { .s = c->own_mime });
    WLC_SEND(w->c, c->drag_src, LM_WL_DATA_SOURCE_REQ_SET_ACTIONS, { .u = LM_DND_ACTION_COPY | LM_DND_ACTION_MOVE });
    WLC_SEND(w->c, c->ddev, LM_WL_DATA_DEVICE_REQ_START_DRAG, { .o = c->drag_src }, { .o = w->surface },
             { .o = icon ? c->drag_surf : 0 }, { .u = c->ptr_serial ? c->ptr_serial : c->serial });
    wlc_flush(w->c);
    return true;
}

void wlclip_release(struct wlclip *c)
{
    if (c->win && c->win->c)
        drag_cleanup(c);
    free(c->out);
    c->out = NULL;
}
