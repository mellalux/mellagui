#include "tray.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "list.h"
#include "log.h"
#include "metrics.h"
#include "protocol.h"
#include "settings.h"
#include "shm.h"
#include "wm.h"

struct tbind {
    struct lm_list link;                /* in lm_tray.binds */
    struct lm_tray *tray;               /* NULL once the compositor is going away */
    bool allowed;
};

struct item {
    struct lm_list link;                /* in lm_tray.items while it is live (shown); unlinked (self-linked) when inert / gone */
    struct lm_tray *tray;
    struct lm_resource *res;
    struct lm_client *client;
    int id;
    bool shown;
    struct lm_texture icon;
    bool has_icon;
    char text[LM_TRAY_TEXT];
};

struct lm_tray {
    struct lm_server *srv;
    struct lm_global *global;
    struct lm_list binds;
    struct lm_list items;
    int next_id;
};

static void changed(struct lm_tray *t)
{
    if (t->srv->wm)
        lm_wm_tray_changed(t->srv->wm);
}

int lm_tray_icon_size(void)
{
    int s = LM_PANEL_HEIGHT - 12;
    return s < 8 ? 8 : s > LM_TRAY_MAX_ICON ? LM_TRAY_MAX_ICON : s;
}

/* ---- items -------------------------------------------------------------------------------------------------------- */

static void item_destroy(struct lm_resource *res)
{
    struct item *it = res->data;
    if (!it)
        return;
    res->data = NULL;
    struct lm_tray *t = it->tray;
    bool was_shown = it->shown;
    if (it->shown)
        lm_list_remove(&it->link);
    lm_texture_free(&it->icon);
    free(it);
    if (t && was_shown)
        changed(t);
}

static void item_req_destroy(struct lm_resource *res, const union lm_arg *args)
{
    (void)args;
    lm_resource_destroy(res);
}

static void item_req_set_icon(struct lm_resource *res, const union lm_arg *args)
{
    struct item *it = res->data;
    if (!it)
        return;
    struct lm_buffer *b = args[0].r ? lm_buffer_from_resource(args[0].r) : NULL;
    if (!args[0].r) {                                           /* no icon */
        lm_texture_free(&it->icon);
        memset(&it->icon, 0, sizeof(it->icon));
        it->has_icon = false;
    } else if (!b || !b->pool || b->width > LM_TRAY_MAX_ICON || b->height > LM_TRAY_MAX_ICON ||
               (b->format != LM_WL_SHM_FORMAT_XRGB8888 && b->format != LM_WL_SHM_FORMAT_ARGB8888)) {
        lm_warn("tray: an icon that is not a wl_shm ARGB/XRGB buffer of at most %dx%d is ignored", LM_TRAY_MAX_ICON, LM_TRAY_MAX_ICON);
        if (b)
            lm_buffer_release(b);
        return;
    } else {
        int rc = lm_buffer_copy(b, &it->icon, LM_TRANSFORM_NORMAL, NULL);
        lm_buffer_release(b);
        if (rc == -2) {
            lm_resource_post_error(args[0].r, LM_WL_SHM_ERR_INVALID_FD, "tray icon: the buffer's memory is gone");
            return;
        }
        if (rc < 0) {
            lm_resource_post_no_memory(res);
            return;
        }
        it->has_icon = true;
    }
    if (it->shown && it->tray)
        changed(it->tray);
}

static void item_req_set_text(struct lm_resource *res, const union lm_arg *args)
{
    struct item *it = res->data;
    if (!it)
        return;
    char text[LM_TRAY_TEXT];
    snprintf(text, sizeof(text), "%s", args[0].s ? args[0].s : "");
    for (char *p = text; *p; p++)
        if ((unsigned char)*p < 0x20)
            *p = ' ';
    if (!strcmp(text, it->text))
        return;
    memcpy(it->text, text, sizeof(it->text));
    if (it->shown && it->tray)
        changed(it->tray);
}

static const lm_request_fn item_handlers[] = {
    [LM_MELLAGUI_TRAY_ITEM_V1_REQ_DESTROY] = item_req_destroy,
    [LM_MELLAGUI_TRAY_ITEM_V1_REQ_SET_ICON] = item_req_set_icon,
    [LM_MELLAGUI_TRAY_ITEM_V1_REQ_SET_TEXT] = item_req_set_text,
};

/* ---- the global ------------------------------------------------------------------------------------------------------- */

static void tray_req_destroy(struct lm_resource *res, const union lm_arg *args)
{
    (void)args;
    lm_resource_destroy(res);
}

static void tray_req_create_item(struct lm_resource *res, const union lm_arg *args)
{
    struct tbind *b = res->data;
    struct item *it = calloc(1, sizeof(*it));
    if (!it) {
        lm_resource_post_no_memory(res);
        return;
    }
    lm_list_init(&it->link);
    it->client = res->client;
    it->res = lm_resource_create(res->client, args[0].u, &lm_mellagui_tray_item_v1_interface, res->version, item_handlers, it,
                                 item_destroy);
    if (!it->res) {
        free(it);
        return;
    }
    struct lm_tray *t = b ? b->tray : NULL;
    if (!t || !b->allowed)
        return;                                                 /* inert */
    int total = 0, mine = 0;
    struct item *o;
    lm_list_for_each(o, &t->items, link) {
        total++;
        mine += o->client == res->client;
    }
    if (total >= LM_TRAY_MAX_ITEMS || mine >= LM_TRAY_PER_CLIENT) {
        lm_warn("tray: too many items (client pid %d), this one stays inert", (int)res->client->pid);
        return;
    }
    it->tray = t;
    it->id = ++t->next_id;
    it->shown = true;
    lm_list_append(&t->items, &it->link);
    lm_resource_post(it->res, LM_MELLAGUI_TRAY_ITEM_V1_EVT_SIZE, (int32_t)lm_tray_icon_size());
    changed(t);
}

static const lm_request_fn tray_handlers[] = {
    [LM_MELLAGUI_TRAY_V1_REQ_DESTROY] = tray_req_destroy,
    [LM_MELLAGUI_TRAY_V1_REQ_CREATE_ITEM] = tray_req_create_item,
};

static void bind_destroy(struct lm_resource *res)
{
    struct tbind *b = res->data;
    if (b)
        lm_list_remove(&b->link);
    free(b);
    res->data = NULL;
}

static void tray_bind(struct lm_client *client, void *data, uint32_t version, uint32_t id)
{
    struct lm_tray *t = data;
    struct tbind *b = calloc(1, sizeof(*b));
    if (!b) {
        lm_client_post_no_memory(client);
        return;
    }
    b->tray = t;
    b->allowed = lm_settings_client_allowed(t->srv, client);
    if (!b->allowed)
        lm_warn("tray: client pid %d is not one of MellaGUI's programs, its items stay hidden", (int)client->pid);
    lm_list_append(&t->binds, &b->link);
    if (!lm_resource_create(client, id, &lm_mellagui_tray_v1_interface, version, tray_handlers, b, bind_destroy)) {
        lm_list_remove(&b->link);
        free(b);
    }
}

struct lm_tray *lm_tray_create(struct lm_server *srv)
{
    struct lm_tray *t = calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    t->srv = srv;
    lm_list_init(&t->binds);
    lm_list_init(&t->items);
    t->global = lm_global_create(srv, &lm_mellagui_tray_v1_interface, LM_MELLAGUI_TRAY_V1_VERSION, tray_bind, t);
    if (!t->global) {
        free(t);
        return NULL;
    }
    return t;
}

void lm_tray_destroy(struct lm_tray *t)
{
    if (!t)
        return;
    struct tbind *b;
    lm_list_for_each(b, &t->binds, link)
        b->tray = NULL;
    struct item *it, *tmp;
    lm_list_for_each_safe(it, tmp, &t->items, link) {
        lm_list_remove(&it->link);
        lm_list_init(&it->link);
        it->shown = false;
        it->tray = NULL;                                        /* the resources live on until their clients go */
    }
    if (t->global)
        lm_global_destroy(t->global);
    free(t);
}

/* ---- what the bar asks ------------------------------------------------------------------------------------------------- */

int lm_tray_count(const struct lm_tray *t)
{
    int n = 0;
    const struct item *it;
    if (t)
        lm_list_for_each(it, &t->items, link)
            n++;
    return n;
}

bool lm_tray_get(const struct lm_tray *t, int i, struct lm_tray_view *out)
{
    const struct item *it;
    if (!t || i < 0)
        return false;
    lm_list_for_each(it, &t->items, link) {
        if (i-- > 0)
            continue;
        out->id = it->id;
        out->icon = it->has_icon && it->icon.pixels ? &it->icon : NULL;
        out->text = it->text;
        return true;
    }
    return false;
}

void lm_tray_click(struct lm_tray *t, int id, uint32_t button)
{
    struct item *it;
    if (!t)
        return;
    lm_list_for_each(it, &t->items, link)
        if (it->id == id) {
            lm_resource_post(it->res, LM_MELLAGUI_TRAY_ITEM_V1_EVT_CLICKED, button);
            return;
        }
}
