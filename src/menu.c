#include "menu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "list.h"
#include "log.h"
#include "protocol.h"
#include "seat.h"
#include "settings.h"
#include "wm.h"

struct mbind {
    struct lm_list link;                /* in lm_menu.binds */
    struct lm_menu *menu;               /* NULL once the compositor is going away */
    struct lm_resource *res;
    bool allowed;
};

struct lm_menu {
    struct lm_server *srv;
    struct lm_global *global;
    struct lm_list binds;
    struct mbind *active;               /* the object whose menu is showing */
};

static void bind_destroy(struct lm_resource *res)
{
    struct mbind *b = res->data;
    if (!b)
        return;
    if (b->menu && b->menu->active == b) {                      /* the program went away: its menu goes, too */
        b->menu->active = NULL;
        if (b->menu->srv->wm)
            lm_wm_menu_cancel(b->menu->srv->wm);
    }
    lm_list_remove(&b->link);
    free(b);
    res->data = NULL;
}

static void req_destroy(struct lm_resource *res, const union lm_arg *args)
{
    (void)args;
    lm_resource_destroy(res);
}

static void req_popup(struct lm_resource *res, const union lm_arg *args)
{
    struct mbind *b = res->data;
    if (!b)
        return;
    struct lm_menu *m = b->menu;
    if (m && m->active) {                                       /* the older menu is replaced */
        struct mbind *old = m->active;
        m->active = NULL;
        if (m->srv->wm)
            lm_wm_menu_cancel(m->srv->wm);
        lm_resource_post(old->res, LM_MELLAGUI_MENU_V1_EVT_CLOSED);
    }
    char text[LM_MENU_MAX * 48 + 8];
    char labels[LM_MENU_MAX][48];
    const char *ptrs[LM_MENU_MAX];
    int n = 0;
    snprintf(text, sizeof(text), "%s", args[0].s ? args[0].s : "");
    for (char *save = NULL, *line = strtok_r(text, "\n", &save); line && n < LM_MENU_MAX; line = strtok_r(NULL, "\n", &save)) {
        snprintf(labels[n], sizeof(labels[n]), "%s", line);
        ptrs[n] = labels[n];
        n++;
    }
    if (!m || !b->allowed || !m->srv->wm || n == 0 || !lm_wm_menu_popup(m->srv->wm, ptrs, n)) {
        lm_resource_post(res, LM_MELLAGUI_MENU_V1_EVT_CLOSED);
        return;
    }
    m->active = b;
}

static void req_warp(struct lm_resource *res, const union lm_arg *args)
{
    struct mbind *b = res->data;
    if (!b || !b->allowed || !b->menu || !b->menu->srv->seat)
        return;
    lm_seat_pointer_warp_local(b->menu->srv->seat, res->client, args[0].i, args[1].i);
}

static const lm_request_fn handlers[] = {
    [LM_MELLAGUI_MENU_V1_REQ_DESTROY] = req_destroy,
    [LM_MELLAGUI_MENU_V1_REQ_POPUP] = req_popup,
    [LM_MELLAGUI_MENU_V1_REQ_WARP] = req_warp,
};

static void menu_bind(struct lm_client *client, void *data, uint32_t version, uint32_t id)
{
    struct lm_menu *m = data;
    struct mbind *b = calloc(1, sizeof(*b));
    if (!b) {
        lm_client_post_no_memory(client);
        return;
    }
    b->menu = m;
    b->allowed = lm_settings_client_allowed(m->srv, client);
    if (!b->allowed)
        lm_warn("menu: client pid %d is not one of MellaGUI's programs, its menus stay closed", (int)client->pid);
    lm_list_append(&m->binds, &b->link);
    b->res = lm_resource_create(client, id, &lm_mellagui_menu_v1_interface, version, handlers, b, bind_destroy);
    if (!b->res) {
        lm_list_remove(&b->link);
        free(b);
    }
}

struct lm_menu *lm_menu_create(struct lm_server *srv)
{
    struct lm_menu *m = calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    m->srv = srv;
    lm_list_init(&m->binds);
    m->global = lm_global_create(srv, &lm_mellagui_menu_v1_interface, LM_MELLAGUI_MENU_V1_VERSION, menu_bind, m);
    if (!m->global) {
        free(m);
        return NULL;
    }
    return m;
}

void lm_menu_destroy(struct lm_menu *m)
{
    if (!m)
        return;
    struct mbind *b;
    lm_list_for_each(b, &m->binds, link)
        b->menu = NULL;
    if (m->global)
        lm_global_destroy(m->global);
    free(m);
}

void lm_menu_chosen(struct lm_server *srv, int index)
{
    struct lm_menu *m = srv->menu;
    if (!m || !m->active)
        return;
    struct mbind *b = m->active;
    m->active = NULL;
    lm_resource_post(b->res, LM_MELLAGUI_MENU_V1_EVT_CHOSEN, (int32_t)index);
}

void lm_menu_closed(struct lm_server *srv)
{
    struct lm_menu *m = srv->menu;
    if (!m || !m->active)
        return;
    struct mbind *b = m->active;
    m->active = NULL;
    lm_resource_post(b->res, LM_MELLAGUI_MENU_V1_EVT_CLOSED);
}
