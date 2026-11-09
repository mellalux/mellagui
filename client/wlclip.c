#include "wlclip.h"

#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "protocol.h"
#include "util.h"

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
    if (op == LM_WL_DATA_OFFER_EVT_OFFER && id == a->offer_pending && a->n_offer_mimes < 16)
        snprintf(a->offer_mimes[a->n_offer_mimes++], sizeof(a->offer_mimes[0]), "%s", args[0].s);
}

static void ddev_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)id;
    struct wlclip *a = data;
    if (op == LM_WL_DATA_DEVICE_EVT_DATA_OFFER) {
        if (a->offer_pending) {
            WLC_SEND0(c, a->offer_pending, LM_WL_DATA_OFFER_REQ_DESTROY);
            wlc_forget(c, a->offer_pending);
        }
        a->offer_pending = args[0].n;
        a->n_offer_mimes = 0;
        wlc_set_handler(c, a->offer_pending, doffer_event, a);
    } else if (op == LM_WL_DATA_DEVICE_EVT_SELECTION) {
        a->offer = args[0].o && args[0].o == a->offer_pending ? args[0].o : 0;      /* the selection offer, or none */
        if (!a->offer && a->offer_pending) {
            WLC_SEND0(c, a->offer_pending, LM_WL_DATA_OFFER_REQ_DESTROY);
            wlc_forget(c, a->offer_pending);
            a->offer_pending = 0;
        }
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
