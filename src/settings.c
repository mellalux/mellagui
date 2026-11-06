/*
 * User settings: lumen_settings_v1 global + the settings file. See settings.h for the contract.
 */
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "input.h"
#include "keymap.h"
#include "log.h"
#include "seat.h"
#include "server.h"
#include "settings.h"
#include "wm.h"

#define SAVE_DELAY_MS 300

enum key_id {
    K_LAYOUTS, K_LAYOUT, K_STICKY, K_REPEAT_DELAY, K_REPEAT_RATE, K_POINTER_SPEED, K_NATURAL_SCROLL, K_SOUND,
    K_GADGET, K_FILE, K_COUNT
};

static const char *const key_names[K_COUNT] = {
    [K_LAYOUTS] = "layouts", [K_LAYOUT] = "layout", [K_STICKY] = "sticky", [K_REPEAT_DELAY] = "repeat-delay",
    [K_REPEAT_RATE] = "repeat-rate", [K_POINTER_SPEED] = "pointer-speed", [K_NATURAL_SCROLL] = "natural-scroll",
    [K_SOUND] = "sound", [K_GADGET] = "gadget", [K_FILE] = "file",
};

static int key_find(const char *key)
{
    for (int i = 0; i < K_COUNT; i++)
        if (!strcmp(key_names[i], key))
            return i;
    return -1;
}

struct binding {
    struct lm_settings *s;          /* NULL once the module is gone */
    struct lm_resource *res;
    struct lm_list link;
};

struct lm_settings {
    struct lm_server *srv;
    struct lm_global *global;
    struct lm_list bindings;
    struct lm_timer *save_timer;
    bool dirty;
    bool applying;          /* inside a `set`: suppress the nested announcements */
};

/* ---- parsing ---------------------------------------------------------------------------------------------- */

static bool parse_bool(const char *v, bool *out)
{
    if (!strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "on") || !strcmp(v, "yes")) {
        *out = true;
        return true;
    }
    if (!strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "off") || !strcmp(v, "no")) {
        *out = false;
        return true;
    }
    return false;
}

static bool parse_int(const char *v, int lo, int hi, int *out)
{
    char *end;
    errno = 0;
    long n = strtol(v, &end, 10);
    if (errno || end == v || *end || n < lo || n > hi)
        return false;
    *out = (int)n;
    return true;
}

/* "us, ee" -> indices; canonical text "us,ee" in canon. */
static int parse_layouts(const char *v, int ids[LM_KBD_MAX_LAYOUTS], char *canon, size_t n)
{
    char err[64];
    int count = lm_layout_parse(v, ids, err, sizeof(err));
    if (count <= 0 || !v || !*v)
        return 0;
    if (canon) {
        canon[0] = 0;
        for (int i = 0; i < count; i++) {
            size_t len = strlen(canon);
            snprintf(canon + len, n - len, "%s%s", i ? "," : "", lm_layout_id(ids[i]));
        }
    }
    return count;
}

/* Validate + store a value in the config (file loading; the config fields that need no live module). Returns the key id or -1. */
static int cfg_set(struct lm_config *cfg, const char *key, const char *val, char *layouts_out, size_t layouts_n)
{
    int k = key_find(key), n;
    bool b;
    int ids[LM_KBD_MAX_LAYOUTS];
    switch (k) {
    case K_LAYOUTS:
        if (!parse_layouts(val, ids, layouts_out, layouts_n))
            return -1;
        break;
    case K_LAYOUT:
        n = lm_layout_find(val);
        if (n < 0)
            return -1;
        cfg->kbd_active = n;
        break;
    case K_STICKY:
        if (!parse_bool(val, &b)) return -1;
        cfg->sticky_keys = b;
        break;
    case K_REPEAT_DELAY:
        if (!parse_int(val, 100, 2000, &n)) return -1;
        cfg->repeat_delay_ms = n;
        break;
    case K_REPEAT_RATE:
        if (!parse_int(val, 0, 100, &n)) return -1;
        cfg->repeat_rate = n;
        break;
    case K_POINTER_SPEED:
        if (!parse_int(val, 10, 500, &n)) return -1;
        cfg->pointer_speed = n / 100.0;
        break;
    case K_NATURAL_SCROLL:
        if (!parse_bool(val, &b)) return -1;
        cfg->natural_scroll = b;
        break;
    case K_SOUND:
        if (!parse_bool(val, &b)) return -1;
        cfg->sound = b;
        break;
    case K_GADGET:
        if (!parse_bool(val, &b)) return -1;
        cfg->gadget = b;
        break;
    default:
        return -1;
    }
    return k;
}

int lm_settings_load(struct lm_config *cfg, const char *path, char *layouts_out, size_t layouts_n)
{
    if (layouts_n)
        layouts_out[0] = 0;
    FILE *f = path ? fopen(path, "r") : NULL;
    if (!f)
        return 0;
    char line[256];
    int applied = 0, lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '#' || *p == '\n' || !*p)
            continue;
        char *eq = strchr(p, '=');
        if (!eq) {
            lm_warn("settings: %s:%d: expected key=value", path, lineno);
            continue;
        }
        *eq = 0;
        char *val = eq + 1;
        for (char *e = p + strlen(p); e > p && (e[-1] == ' ' || e[-1] == '\t'); )
            *--e = 0;
        while (*val == ' ' || *val == '\t')
            val++;
        val[strcspn(val, "\r\n")] = 0;
        for (char *e = val + strlen(val); e > val && (e[-1] == ' ' || e[-1] == '\t'); )
            *--e = 0;
        if (cfg_set(cfg, p, val, layouts_out, layouts_n) < 0)
            lm_warn("settings: %s:%d: ignoring '%s=%s'", path, lineno, p, val);
        else
            applied++;
    }
    fclose(f);
    return applied;
}

/* ---- current values ------------------------------------------------------------------------------------- */

static void get_value(struct lm_settings *s, int k, char *out, size_t n)
{
    struct lm_server *srv = s->srv;
    const struct lm_config *c = &srv->config;
    out[0] = 0;
    switch (k) {
    case K_LAYOUTS:
        if (srv->input) {
            for (int i = 0; i < lm_input_layout_count(srv->input); i++) {
                size_t len = strlen(out);
                snprintf(out + len, n - len, "%s%s", i ? "," : "", lm_input_layout_id(srv->input, i));
            }
        }
        break;
    case K_LAYOUT:
        if (srv->input)
            snprintf(out, n, "%s", lm_input_layout_id(srv->input, lm_input_layout(srv->input)));
        break;
    case K_STICKY: snprintf(out, n, "%d", srv->input && lm_input_sticky(srv->input) ? 1 : 0); break;
    case K_REPEAT_DELAY: snprintf(out, n, "%d", c->repeat_delay_ms); break;
    case K_REPEAT_RATE: snprintf(out, n, "%d", c->repeat_rate); break;
    case K_POINTER_SPEED: snprintf(out, n, "%ld", lround(c->pointer_speed * 100.0)); break;
    case K_NATURAL_SCROLL: snprintf(out, n, "%d", c->natural_scroll ? 1 : 0); break;
    case K_SOUND: snprintf(out, n, "%d", c->sound ? 1 : 0); break;
    case K_GADGET: snprintf(out, n, "%d", srv->wm && lm_wm_gadget_visible(srv->wm) ? 1 : 0); break;
    case K_FILE: snprintf(out, n, "%s", c->settings_path ? c->settings_path : ""); break;
    }
}

bool lm_settings_get(struct lm_settings *s, const char *key, char *out, size_t n)
{
    int k = key_find(key);
    if (k < 0 || !n)
        return false;
    get_value(s, k, out, n);
    return true;
}

void lm_settings_dump(struct lm_settings *s, char *out, size_t n)
{
    out[0] = 0;
    for (int k = 0; k < K_COUNT; k++) {
        char v[96];
        get_value(s, k, v, sizeof(v));
        size_t len = strlen(out);
        if (len + 1 < n)
            snprintf(out + len, n - len, "%s%s=%s", k ? " " : "", key_names[k], v[0] ? v : "-");
    }
}

/* ---- announcing --------------------------------------------------------------------------------------- */

static void send_burst(struct lm_settings *s, struct lm_resource *res)
{
    for (int k = 0; k < K_COUNT; k++) {
        char v[96];
        get_value(s, k, v, sizeof(v));
        lm_resource_post(res, LM_LUMEN_SETTINGS_V1_EVT_VALUE, key_names[k], v);
    }
    lm_resource_post(res, LM_LUMEN_SETTINGS_V1_EVT_DONE);
}

static void broadcast(struct lm_settings *s)
{
    struct binding *b;
    lm_list_for_each(b, &s->bindings, link)
        send_burst(s, b->res);
}

void lm_settings_changed(struct lm_settings *s)
{
    if (s && !s->applying)
        broadcast(s);
}

/* ---- saving ------------------------------------------------------------------------------------------- */

static void mkdir_parents(char *path)
{
    for (char *p = path + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = 0;
        if (mkdir(path, 0755) < 0 && errno != EEXIST) {
            *p = '/';
            return;
        }
        *p = '/';
    }
}

static void save_now(struct lm_settings *s)
{
    const char *path = s->srv->config.settings_path;
    s->dirty = false;
    if (s->save_timer)
        lm_timer_arm_ms(s->save_timer, 0, 0);
    if (!path || !*path)
        return;
    char dir[512], tmp[560];
    if (snprintf(dir, sizeof(dir), "%s", path) >= (int)sizeof(dir) ||
        snprintf(tmp, sizeof(tmp), "%s.%d.tmp", path, (int)getpid()) >= (int)sizeof(tmp)) {
        lm_warn("settings: path too long");
        return;
    }
    mkdir_parents(dir);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        lm_warn("settings: cannot write %s: %s", tmp, strerror(errno));
        return;
    }
    fputs("# Lumen settings (written by the settings panel; edit while lumen is not running)\n", f);
    for (int k = 0; k < K_FILE; k++) {
        char v[96];
        get_value(s, k, v, sizeof(v));
        fprintf(f, "%s=%s\n", key_names[k], v);
    }
    bool ok = !ferror(f);
    if (fclose(f) != 0)
        ok = false;
    if (!ok || rename(tmp, path) < 0) {
        lm_warn("settings: cannot save %s: %s", path, strerror(errno));
        unlink(tmp);
        return;
    }
    lm_debug("settings: saved %s", path);
}

static void save_timer_cb(void *data)
{
    save_now(data);
}

static void save_later(struct lm_settings *s)
{
    s->dirty = true;
    if (s->save_timer)
        lm_timer_arm_ms(s->save_timer, SAVE_DELAY_MS, 0);
}

/* ---- applying a change ---------------------------------------------------------------------------- */

static bool apply(struct lm_settings *s, const char *key, const char *val)
{
    struct lm_server *srv = s->srv;
    struct lm_config *cfg = &srv->config;
    int k = key_find(key), n;
    bool b;
    int ids[LM_KBD_MAX_LAYOUTS];
    switch (k) {
    case K_LAYOUTS: {
        if (!srv->input || !srv->seat)
            return false;
        int count = parse_layouts(val, ids, NULL, 0);
        if (!count)
            return false;
        const char *cur = lm_input_layout_id(srv->input, lm_input_layout(srv->input));
        int active = -1;
        for (int i = 0; i < count; i++)
            if (!strcmp(lm_layout_id(ids[i]), cur))
                active = ids[i];
        memcpy(cfg->kbd_layouts, ids, sizeof(int) * (size_t)count);
        cfg->kbd_nlayouts = count;
        lm_seat_set_layouts(srv->seat, ids, count);       /* the new keymap first, then the group number in it */
        lm_input_set_layouts(srv->input, ids, count, active);
        return true;
    }
    case K_LAYOUT: {
        if (!srv->input)
            return false;
        for (int i = 0; i < lm_input_layout_count(srv->input); i++)
            if (!strcmp(lm_input_layout_id(srv->input, i), val)) {
                lm_input_set_layout(srv->input, i);
                return true;
            }
        return false;
    }
    case K_STICKY:
        if (!srv->input || !parse_bool(val, &b)) return false;
        lm_input_set_sticky(srv->input, b);
        return true;
    case K_REPEAT_DELAY:
        if (!parse_int(val, 100, 2000, &n)) return false;
        cfg->repeat_delay_ms = n;
        if (srv->seat) lm_seat_send_repeat_info(srv->seat);
        return true;
    case K_REPEAT_RATE:
        if (!parse_int(val, 0, 100, &n)) return false;
        cfg->repeat_rate = n;
        if (srv->seat) lm_seat_send_repeat_info(srv->seat);
        return true;
    case K_POINTER_SPEED:
    case K_NATURAL_SCROLL:
    case K_SOUND:
        return cfg_set(cfg, key, val, NULL, 0) >= 0;
    case K_GADGET:
        if (!srv->wm || !parse_bool(val, &b)) return false;
        lm_wm_set_gadget(srv->wm, b);
        return true;
    default:
        return false;       /* unknown or read-only ("file") */
    }
}

bool lm_settings_set(struct lm_settings *s, const char *key, const char *value)
{
    s->applying = true;
    bool ok = apply(s, key, value);
    s->applying = false;
    if (ok)
        save_later(s);
    else
        lm_debug("settings: rejected %s=%s", key, value);
    broadcast(s);
    return ok;
}

/* ---- the global --------------------------------------------------------------------------------------- */

static void req_destroy(struct lm_resource *res, const union lm_arg *args)
{
    (void)args;
    lm_resource_destroy(res);
}

static void req_set(struct lm_resource *res, const union lm_arg *args)
{
    struct binding *b = res->data;
    if (b && b->s)
        lm_settings_set(b->s, args[0].s, args[1].s);
}

static const lm_request_fn handlers[] = {
    [LM_LUMEN_SETTINGS_V1_REQ_DESTROY] = req_destroy,
    [LM_LUMEN_SETTINGS_V1_REQ_SET] = req_set,
};

static void binding_destroy(struct lm_resource *res)
{
    struct binding *b = res->data;
    if (!b)
        return;
    res->data = NULL;
    lm_list_remove(&b->link);
    free(b);
}

static void settings_bind(struct lm_client *client, void *data, uint32_t version, uint32_t id)
{
    struct lm_settings *s = data;
    struct binding *b = calloc(1, sizeof(*b));
    if (!b) {
        lm_client_post_no_memory(client);
        return;
    }
    b->s = s;
    b->res = lm_resource_create(client, id, &lm_lumen_settings_v1_interface, version, handlers, b, binding_destroy);
    if (!b->res) {
        free(b);
        return;
    }
    lm_list_append(&s->bindings, &b->link);
    send_burst(s, b->res);
}

struct lm_settings *lm_settings_create(struct lm_server *srv)
{
    struct lm_settings *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->srv = srv;
    lm_list_init(&s->bindings);
    s->save_timer = lm_loop_add_timer(srv->loop, save_timer_cb, s);
    s->global = lm_global_create(srv, &lm_lumen_settings_v1_interface, LM_LUMEN_SETTINGS_V1_VERSION,
                                 settings_bind, s);
    if (!s->global) {
        lm_settings_destroy(s);
        return NULL;
    }
    return s;
}

void lm_settings_destroy(struct lm_settings *s)
{
    if (!s)
        return;
    if (s->dirty)
        save_now(s);
    struct binding *b, *tmp;
    lm_list_for_each_safe(b, tmp, &s->bindings, link) {
        lm_list_remove(&b->link);       /* the resource (and this struct) lives on until its client is torn down */
        b->s = NULL;
    }
    if (s->global)
        lm_global_destroy(s->global);
    if (s->save_timer)
        lm_timer_destroy(s->save_timer);
    free(s);
}
