/*
 * User settings: mellagui_settings_v1 global + the settings file. See settings.h for the contract.
 */
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "audio.h"
#include "confdefault.h"
#include "backend.h"
#include "input.h"
#include "sound.h"
#include "output.h"
#include "keymap.h"
#include "lock.h"
#include "log.h"
#include "render.h"
#include "sha256.h"
#include "seat.h"
#include "server.h"
#include "settings.h"
#include "wm.h"

#define SAVE_DELAY_MS 300

/* Keys before K_FILE are saved to the settings file, the ones after it are read-only or live state only. */
enum key_id {
    K_LAYOUTS, K_LAYOUT, K_STICKY, K_REPEAT_DELAY, K_REPEAT_RATE, K_POINTER_SPEED, K_NATURAL_SCROLL, K_SOUND,
    K_GADGET, K_MIXER, K_SYSINFO, K_RESOLUTION, K_AUDIO_OUT, K_WALLPAPER, K_WALLPAPER_MODE, K_OUTPUT_ORDER, K_FILE,
    K_RESOLUTIONS, K_RESOLUTION_NOW, K_MONITOR, K_RESOLUTION_CONFIRM, K_AUDIO_OUTS, K_SINKS, K_SINK, K_SINKS_STATUS, K_AUDIO_PROFILES, K_AUDIO_PROFILE, K_AUDIO_RESCAN, K_WALLPAPER_STATUS, K_OUTPUTS,
    K_LOCK_KIND, K_LOCK_TIMEOUT, K_LOCK_START, K_LOCK_BLANK, K_LOCK_VT, K_LOCK_STATUS, K_LOCK_SET, K_LOCK_NOW, K_LOCKED, K_NOTIFY, K_CONFIG_FILE, K_CONFIG_STATUS, K_CONFIG_RELOAD, K_CONFIG_CREATE, K_COUNT
};
#define VALUE_MAX 1900          /* longest value text (the lists); the wire carries it as one string */

static const char *const key_names[K_COUNT] = {
    [K_LAYOUTS] = "layouts", [K_LAYOUT] = "layout", [K_STICKY] = "sticky", [K_REPEAT_DELAY] = "repeat-delay",
    [K_REPEAT_RATE] = "repeat-rate", [K_POINTER_SPEED] = "pointer-speed", [K_NATURAL_SCROLL] = "natural-scroll",
    [K_SOUND] = "sound", [K_GADGET] = "gadget", [K_MIXER] = "mixer", [K_SYSINFO] = "sysinfo", [K_RESOLUTION] = "resolution", [K_AUDIO_OUT] = "audio-out", [K_WALLPAPER] = "wallpaper", [K_WALLPAPER_MODE] = "wallpaper-mode", [K_OUTPUT_ORDER] = "output-order", [K_FILE] = "file",
    [K_RESOLUTIONS] = "resolutions", [K_RESOLUTION_NOW] = "resolution-now", [K_MONITOR] = "monitor", [K_RESOLUTION_CONFIRM] = "resolution-confirm",
    [K_AUDIO_OUTS] = "audio-outs", [K_SINKS] = "sinks", [K_SINK] = "sink", [K_SINKS_STATUS] = "sinks-status", [K_AUDIO_PROFILES] = "audio-profiles",
    [K_AUDIO_PROFILE] = "audio-profile", [K_AUDIO_RESCAN] = "audio-rescan", [K_WALLPAPER_STATUS] = "wallpaper-status", [K_OUTPUTS] = "outputs",
    [K_LOCK_KIND] = "lock-kind", [K_LOCK_TIMEOUT] = "lock-timeout", [K_LOCK_START] = "lock-start", [K_LOCK_BLANK] = "lock-blank", [K_LOCK_VT] = "lock-vt", [K_LOCK_STATUS] = "lock-status",
    [K_LOCK_SET] = "lock-set", [K_LOCK_NOW] = "lock-now", [K_LOCKED] = "locked", [K_NOTIFY] = "notify",
    [K_CONFIG_FILE] = "config-file", [K_CONFIG_STATUS] = "config-status", [K_CONFIG_RELOAD] = "config-reload", [K_CONFIG_CREATE] = "config-create",
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
    /* resolution change waiting for "keep"; otherwise it is undone after revert_ms (a black screen cannot be confirmed) */
    bool res_pending;
    int prev_w, prev_h, prev_hz;
    struct lm_timer *revert_timer;
    bool no_save;           /* the current `set` must not schedule a save (resolution: only once it is kept) */
    char sinks[VALUE_MAX], sink[128];       /* cached `pactl` answers, refreshed by audio-rescan */
    char profiles[VALUE_MAX], profile[24];  /* cached PipeWire card profiles (pw-cli), the same */
    struct lm_timer *rescan_timer;          /* after a profile change PipeWire needs a moment to make the new sink */
    int rescan_left;
};

#define DEFAULT_REVERT_MS 15000

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

/* resolution text: "auto" (0), "preferred" (-1), "WxH" or "WxH@Hz" */
static bool parse_resolution(const char *v, int *w, int *h, int *hz)
{
    if (!strcmp(v, "auto")) {
        *w = *h = *hz = 0;
        return true;
    }
    if (!strcmp(v, "preferred")) {
        *w = -1;
        *h = *hz = 0;
        return true;
    }
    int rate = 60;
    char junk;
    int n = sscanf(v, "%dx%d@%d%c", w, h, &rate, &junk);
    if (n < 2 || n > 3 || *w < 320 || *h < 200 || *w > 8192 || *h > 4320 || rate < 24 || rate > 240)
        return false;
    *hz = rate;
    return true;
}

static void resolution_text(int w, int h, int hz, char *out, size_t n)
{
    if (w == 0)
        snprintf(out, n, "auto");
    else if (w < 0)
        snprintf(out, n, "preferred");
    else
        snprintf(out, n, "%dx%d@%d", w, h, hz);
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
/* A wallpaper file name: at most 200 bytes, one line (the settings file is line based), may be empty. */
static bool wallpaper_path_ok(const char *v)
{
    if (strlen(v) > 200)
        return false;
    for (; *v; v++)
        if ((unsigned char)*v < 0x20 || *v == 0x7f)
            return false;
    return true;
}

/* Connector names, comma separated: letters, digits, - and _ (at most 200 bytes), may be empty. */
static bool output_order_ok(const char *v)
{
    if (strlen(v) > 199)
        return false;
    for (; *v; v++)
        if (!((*v >= '0' && *v <= '9') || (*v >= 'a' && *v <= 'z') || (*v >= 'A' && *v <= 'Z') || *v == '-' || *v == '_' || *v == ','))
            return false;
    return true;
}

static const char *const wall_modes[] = { "fill", "fit", "stretch", "center" };
static int wallpaper_mode_parse(const char *v)
{
    for (int i = 0; i < 4; i++)
        if (!strcmp(v, wall_modes[i]))
            return i;
    return -1;
}
static const char *wallpaper_mode_name(int m) { return wall_modes[LM_CLAMP(m, 0, 3)]; }

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
    case K_MIXER:
        if (!parse_bool(val, &b)) return -1;
        cfg->mixer = b;
        break;
    case K_SYSINFO:
        if (!parse_bool(val, &b)) return -1;
        cfg->sysinfo = b;
        break;
    case K_RESOLUTION: {
        int w, h, hz;
        if (!parse_resolution(val, &w, &h, &hz)) return -1;
        cfg->drm_mode_w = w;
        cfg->drm_mode_h = h;
        cfg->drm_mode_hz = hz;
        break;
    }
    case K_AUDIO_OUT:
        if (!lm_audio_name_ok(val)) return -1;
        snprintf(cfg->audio_device, sizeof(cfg->audio_device), "%s", val);
        break;
    case K_WALLPAPER:
        if (!wallpaper_path_ok(val)) return -1;
        snprintf(cfg->wallpaper, sizeof(cfg->wallpaper), "%s", val);
        break;
    case K_WALLPAPER_MODE:
        if ((n = wallpaper_mode_parse(val)) < 0) return -1;
        cfg->wallpaper_mode = n;
        break;
    case K_OUTPUT_ORDER:
        if (!output_order_ok(val)) return -1;
        snprintf(cfg->output_order, sizeof(cfg->output_order), "%s", val);
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

static int revert_ms(void)
{
    const char *e = getenv("MELLAGUI_REVERT_MS");
    int v = e ? atoi(e) : 0;
    return v > 0 ? v : DEFAULT_REVERT_MS;
}

static struct lm_output *first_output(struct lm_server *srv)
{
    struct lm_output *lo;
    if (srv->backend)
        lm_list_for_each(lo, &srv->backend->outputs, link)
            return lo;
    return NULL;
}

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
    case K_MIXER: snprintf(out, n, "%d", srv->wm && lm_wm_gadget_visible_id(srv->wm, LM_GADGET_MIXER) ? 1 : 0); break;
    case K_SYSINFO: snprintf(out, n, "%d", srv->wm && lm_wm_gadget_visible_id(srv->wm, LM_GADGET_SYSTEM) ? 1 : 0); break;
    case K_RESOLUTION: resolution_text(c->drm_mode_w, c->drm_mode_h, c->drm_mode_hz, out, n); break;
    case K_AUDIO_OUT: snprintf(out, n, "%s", c->audio_device); break;
    case K_WALLPAPER: snprintf(out, n, "%s", c->wallpaper); break;
    case K_WALLPAPER_MODE: snprintf(out, n, "%s", wallpaper_mode_name(c->wallpaper_mode)); break;
    case K_WALLPAPER_STATUS: snprintf(out, n, "%s", srv->render ? lm_render_wallpaper_status(srv->render) : ""); break;
    case K_OUTPUT_ORDER: snprintf(out, n, "%s", c->output_order); break;
    case K_OUTPUTS: {                                   /* "name|WxH|x,y;..." in the order they lie, left to right */
        size_t o = 0;
        out[0] = 0;
        struct lm_output *prev = NULL;
        for (int round = 0; srv->backend && round < 16; round++) {
            struct lm_output *best = NULL, *p;
            lm_list_for_each(p, &srv->backend->outputs, link)
                if ((!prev || p->x > prev->x || (p->x == prev->x && p->y > prev->y)) && (!best || p->x < best->x))
                    best = p;
            if (!best)
                break;
            o += (size_t)snprintf(out + o, n - o, "%s%s|%dx%d|%d,%d", o ? ";" : "", best->name, best->width, best->height, best->x, best->y);
            prev = best;
            if (o + 64 >= n)
                break;
        }
        break;
    }
    case K_FILE: snprintf(out, n, "%s", c->settings_path ? c->settings_path : ""); break;
    case K_RESOLUTIONS: {
        struct lm_output *lo = first_output(srv);
        struct lm_mode_info modes[LM_MAX_MODES];
        int count = lo ? lm_output_list_modes(lo, modes, LM_MAX_MODES) : 0;
        for (int i = 0; i < count; i++) {
            size_t len = strlen(out);
            snprintf(out + len, n - len, "%s%dx%d@%d|%s", i ? ";" : "", modes[i].w, modes[i].h, modes[i].hz,
                     modes[i].generated ? "panel shape" : modes[i].preferred ? "preferred" : "");
        }
        break;
    }
    case K_RESOLUTION_NOW: {
        struct lm_output *lo = first_output(srv);
        if (lo)
            snprintf(out, n, "%dx%d@%d", lo->width, lo->height, (lo->refresh_mhz + 500) / 1000);
        break;
    }
    case K_MONITOR: {
        struct lm_output *lo = first_output(srv);
        if (lo)
            snprintf(out, n, "%s|%s|%s|%d|%d", lo->make, lo->model, lo->name, lo->phys_width_mm, lo->phys_height_mm);
        break;
    }
    case K_RESOLUTION_CONFIRM:
        if (s->res_pending)
            snprintf(out, n, "%d", revert_ms() / 1000 > 0 ? revert_ms() / 1000 : 1);
        break;
    case K_AUDIO_OUTS: lm_audio_list_alsa(out, n); break;
    case K_SINKS: snprintf(out, n, "%s", s->sinks); break;
    case K_SINK: snprintf(out, n, "%s", s->sink); break;
    case K_SINKS_STATUS: snprintf(out, n, "%s", lm_audio_sink_status()); break;
    case K_AUDIO_PROFILES: snprintf(out, n, "%s", s->profiles); break;
    case K_AUDIO_PROFILE: snprintf(out, n, "%s", s->profile); break;
    case K_AUDIO_RESCAN: snprintf(out, n, "0"); break;
    case K_LOCK_KIND: snprintf(out, n, "%s", lm_lock_kind(srv->lock)); break;
    case K_LOCK_TIMEOUT: snprintf(out, n, "%d", lm_lock_timeout(srv->lock)); break;
    case K_LOCK_START: snprintf(out, n, "%d", lm_lock_start(srv->lock) ? 1 : 0); break;
    case K_LOCK_BLANK: snprintf(out, n, "%d", lm_lock_blank(srv->lock)); break;
    case K_LOCK_VT: snprintf(out, n, "%d", lm_lock_vt(srv->lock) ? 1 : 0); break;
    case K_LOCK_STATUS: snprintf(out, n, "%s", lm_lock_status(srv->lock)); break;
    case K_LOCK_SET: case K_LOCK_NOW: case K_NOTIFY: break;            /* write only: the secrets never come back */
    case K_LOCKED: snprintf(out, n, "%d", lm_lock_active(srv->lock) ? 1 : 0); break;
    case K_CONFIG_FILE: snprintf(out, n, "%s", c->conf_path && strcmp(c->conf_path, "none") ? c->conf_path : ""); break;
    case K_CONFIG_STATUS:
        if (!c->conf_path || !strcmp(c->conf_path, "none"))
            snprintf(out, n, "none");
        else if (access(c->conf_path, R_OK) != 0)
            snprintf(out, n, "missing");
        else if (!srv->reloaded)
            snprintf(out, n, "ok");
        else if (srv->reload_result < 0)
            snprintf(out, n, "missing");
        else if (srv->reload_result > 0)
            snprintf(out, n, "ignored:%d", srv->reload_result);
        else
            snprintf(out, n, "ok");
        break;
    case K_CONFIG_RELOAD: case K_CONFIG_CREATE: break;                   /* write only */
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
        if (k == K_RESOLUTIONS || k == K_AUDIO_OUTS || k == K_SINKS)
            continue;                                   /* lists contain spaces and are long: ask for them one by one */
        char v[VALUE_MAX];
        get_value(s, k, v, sizeof(v));
        size_t len = strlen(out);
        if (len + 1 < n)
            snprintf(out + len, n - len, "%s%s=%s", len ? " " : "", key_names[k], v[0] ? v : "-");
    }
}

/* ---- announcing --------------------------------------------------------------------------------------- */

static void send_burst(struct lm_settings *s, struct lm_resource *res)
{
    for (int k = 0; k < K_COUNT; k++) {
        char v[VALUE_MAX];
        get_value(s, k, v, sizeof(v));
        lm_resource_post(res, LM_MELLAGUI_SETTINGS_V1_EVT_VALUE, key_names[k], v);
    }
    lm_resource_post(res, LM_MELLAGUI_SETTINGS_V1_EVT_DONE);
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
    fputs("# MellaGUI settings (written by the settings panel; edit while mellagui is not running)\n", f);
    for (int k = 0; k < K_FILE; k++) {
        char v[VALUE_MAX];
        if (k == K_RESOLUTION && s->res_pending)
            resolution_text(s->prev_w, s->prev_h, s->prev_hz, v, sizeof(v));   /* never save what was not kept */
        else
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

/* Asks the sound server again (pactl / wpctl / pw-cli): sinks, their default and the card profiles. */
static void audio_rescan(struct lm_settings *s)
{
    char list[VALUE_MAX];
    lm_audio_list_sinks(list, sizeof(list), s->sink, sizeof(s->sink));
    snprintf(s->sinks, sizeof(s->sinks), "%s", list);
    lm_audio_list_profiles(list, sizeof(list), s->profile, sizeof(s->profile));
    snprintf(s->profiles, sizeof(s->profiles), "%s", list);
}

static void rescan_cb(void *data)
{
    struct lm_settings *s = data;
    audio_rescan(s);
    broadcast(s);
    if (--s->rescan_left > 0 && s->rescan_timer)
        lm_timer_arm_ms(s->rescan_timer, 1500, 0);
}

/* ---- resolution: change, keep or revert --------------------------------------------------------------- */

static bool apply_modes(struct lm_server *srv)
{
    bool ok = true;
    struct lm_output *lo;
    if (!srv->backend)
        return false;
    lm_list_for_each(lo, &srv->backend->outputs, link)
        if (lm_output_apply_mode(lo) < 0)
            ok = false;
    return ok;
}

static void revert_resolution(struct lm_settings *s)
{
    struct lm_config *cfg = &s->srv->config;
    if (!s->res_pending)
        return;
    s->res_pending = false;
    if (s->revert_timer)
        lm_timer_arm_ms(s->revert_timer, 0, 0);
    cfg->drm_mode_w = s->prev_w;
    cfg->drm_mode_h = s->prev_h;
    cfg->drm_mode_hz = s->prev_hz;
    if (!apply_modes(s->srv))
        lm_warn("settings: could not go back to the previous resolution");
    lm_info("settings: resolution change was not kept, back to the previous one");
}

static void revert_cb(void *data)
{
    struct lm_settings *s = data;
    revert_resolution(s);
    broadcast(s);
}

static bool set_resolution(struct lm_settings *s, const char *val)
{
    struct lm_server *srv = s->srv;
    struct lm_config *cfg = &srv->config;
    int w, h, hz;
    if (!parse_resolution(val, &w, &h, &hz))
        return false;
    int ow = cfg->drm_mode_w, oh = cfg->drm_mode_h, ohz = cfg->drm_mode_hz;
    struct lm_output *lo = first_output(srv);
    int before_w = lo ? lo->width : 0, before_h = lo ? lo->height : 0, before_r = lo ? lo->refresh_mhz : 0;
    cfg->drm_mode_w = w;
    cfg->drm_mode_h = h;
    cfg->drm_mode_hz = hz;
    if (!apply_modes(srv)) {                                    /* refused: nothing changed on the screen */
        cfg->drm_mode_w = ow;
        cfg->drm_mode_h = oh;
        cfg->drm_mode_hz = ohz;
        apply_modes(srv);
        return false;
    }
    bool changed = lo && (lo->width != before_w || lo->height != before_h || lo->refresh_mhz != before_r);
    if (!changed) {                                             /* the screen is the same: just remember the choice */
        if (s->res_pending)
            s->no_save = true;                                  /* ... unless an earlier change still waits to be kept */
        return true;
    }
    if (!s->res_pending) {                                      /* the baseline is the one before the first change */
        s->res_pending = true;
        s->prev_w = ow;
        s->prev_h = oh;
        s->prev_hz = ohz;
    }
    s->no_save = true;
    if (s->revert_timer)
        lm_timer_arm_ms(s->revert_timer, revert_ms(), 0);
    return true;
}

static bool confirm_resolution(struct lm_settings *s, const char *val)
{
    bool b;
    if (!parse_bool(val, &b))
        return false;
    if (!s->res_pending)
        return true;
    if (b) {
        s->res_pending = false;
        if (s->revert_timer)
            lm_timer_arm_ms(s->revert_timer, 0, 0);
        return true;                                            /* kept: lm_settings_set saves it */
    }
    revert_resolution(s);
    s->no_save = true;
    return true;
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
    case K_MIXER:
        if (!srv->wm || !parse_bool(val, &b)) return false;
        lm_wm_set_gadget_id(srv->wm, LM_GADGET_MIXER, b);
        cfg->mixer = b;
        return true;
    case K_SYSINFO:
        if (!srv->wm || !parse_bool(val, &b)) return false;
        lm_wm_set_gadget_id(srv->wm, LM_GADGET_SYSTEM, b);
        cfg->sysinfo = b;
        return true;
    case K_RESOLUTION:
        return set_resolution(s, val);
    case K_RESOLUTION_CONFIRM:
        return confirm_resolution(s, val);
    case K_AUDIO_OUT:
        if (!lm_audio_name_ok(val)) return false;
        snprintf(cfg->audio_device, sizeof(cfg->audio_device), "%s", val);
        lm_sound_set_device(val);
        return true;
    case K_WALLPAPER:
    case K_WALLPAPER_MODE:
        if (cfg_set(cfg, key, val, NULL, 0) < 0)
            return false;
        if (srv->render)
            lm_render_set_wallpaper(srv->render, cfg->wallpaper, cfg->wallpaper_mode);
        return true;
    case K_OUTPUT_ORDER:
        if (cfg_set(cfg, key, val, NULL, 0) < 0)
            return false;
        if (srv->wm)
            lm_wm_relayout_outputs(srv->wm);                    /* the monitors move, the windows with them */
        return true;
    case K_SINK:
        if (!lm_audio_set_sink(val)) return false;
        snprintf(s->sink, sizeof(s->sink), "%s", val);
        return true;
    case K_AUDIO_PROFILE:
        if (!lm_audio_set_profile(val))
            return false;
        audio_rescan(s);
        s->rescan_left = 2;                                         /* the new sink shows up within a second or two */
        if (s->rescan_timer)
            lm_timer_arm_ms(s->rescan_timer, 800, 0);
        return true;
    case K_AUDIO_RESCAN:
        audio_rescan(s);
        return true;
    case K_LOCK_SET: {
        /* "current<TAB>kind<TAB>new<TAB>minutes<TAB>start<TAB>blank<TAB>vt" (kind: password | pin | keep | none; see lock.h) */
        char buf[2 * LM_LOCK_MAX_SECRET + 64];
        char *f[7] = { "", "", "", "", "", "", "" };
        if (strlen(val) >= sizeof(buf)) {
            lm_secure_zero(buf, sizeof(buf));
            return false;
        }
        strcpy(buf, val);
        char *p = buf;
        for (int i = 0; i < 7; i++) {
            f[i] = p;
            char *tab = i < 6 ? strchr(p, '\t') : NULL;
            if (!tab)
                break;
            *tab = 0;
            p = tab + 1;
        }
        bool ok = lm_lock_apply(srv->lock, f[0], f[1], f[2], f[3], f[4], f[5], f[6]);
        lm_secure_zero(buf, sizeof(buf));
        return ok;
    }
    case K_NOTIFY: {                                    /* "title<TAB>body<TAB>milliseconds" */
        char tmp[400];
        snprintf(tmp, sizeof(tmp), "%s", val);
        char *tab1 = strchr(tmp, '\t'), *tab2 = tab1 ? strchr(tab1 + 1, '\t') : NULL;
        if (!srv->wm || !tab1)
            return false;
        *tab1 = 0;
        if (tab2)
            *tab2 = 0;
        int ms = tab2 ? atoi(tab2 + 1) : 6000;
        if (ms < 0 || ms > 600000)
            return false;
        lm_wm_notify(srv->wm, tmp, tab1 + 1, ms);
        return true;
    }
    case K_LOCK_NOW:
        return parse_bool(val, &b) && (!b || lm_lock_now(srv->lock));
    case K_CONFIG_RELOAD:
        return parse_bool(val, &b) && (!b || (srv->reload_config && srv->reload_config(srv) >= 0));
    case K_CONFIG_CREATE: {                             /* write the built-in configuration as the user's file, never over an existing one */
        const char *path = srv->config.conf_path;
        if (!parse_bool(val, &b) || !b || !path || !*path || !strcmp(path, "none") || access(path, F_OK) == 0)
            return false;
        char dir[512];
        if (snprintf(dir, sizeof(dir), "%s", path) >= (int)sizeof(dir))
            return false;
        mkdir_parents(dir);
        FILE *f = fopen(path, "wx");
        if (!f) {
            lm_warn("settings: cannot create %s: %s", path, strerror(errno));
            return false;
        }
        fputs("# MellaGUI configuration. Everything here is the built-in default: delete what you do not change.\n"
              "# Super+Shift+R (or Settings -> Config -> Reload) reads this file again.\n\n", f);
        fputs(lm_config_default_text(), f);
        bool ok = fclose(f) == 0;
        lm_info("settings: wrote the default configuration to %s", path);
        return ok;
    }
    default:
        return false;       /* unknown or read-only ("file") */
    }
}

bool lm_settings_set(struct lm_settings *s, const char *key, const char *value)
{
    s->applying = true;
    s->no_save = false;
    bool ok = apply(s, key, value);
    s->applying = false;
    int kid = key_find(key);
    bool persistent = kid >= 0 && (kid < K_FILE || kid == K_RESOLUTION_CONFIRM);     /* the others are live state only */
    if (ok && persistent && !s->no_save)
        save_later(s);
    else if (kid != K_LOCK_SET)                                 /* never log a password */
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
    [LM_MELLAGUI_SETTINGS_V1_REQ_DESTROY] = req_destroy,
    [LM_MELLAGUI_SETTINGS_V1_REQ_SET] = req_set,
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

/*
 * Is this client one of MellaGUI's own programs? Its executable is called mellagui-* and sits in the directory of the
 * compositor's own executable (readlink on /proc). This keeps a browser or an editor in the session away from the settings;
 * it is not a boundary against code that already runs as this user (that code can copy a binary, or do worse).
 */
static bool client_is_own(const struct lm_client *c)
{
    char path[64], exe[PATH_MAX], self[PATH_MAX];
    if (c->pid <= 0)
        return false;
    snprintf(path, sizeof(path), "/proc/%d/exe", (int)c->pid);
    ssize_t n = readlink(path, exe, sizeof(exe) - 1);
    ssize_t m = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n <= 0 || m <= 0)
        return false;
    exe[n] = 0;
    self[m] = 0;
    char *eslash = strrchr(exe, '/'), *sslash = strrchr(self, '/');
    if (!eslash || !sslash)
        return false;
    return eslash - exe == sslash - self && !strncmp(exe, self, (size_t)(eslash - exe)) &&
           !strncmp(eslash + 1, "mellagui-", 9);
}

static bool access_allowed(const struct lm_settings *s, const struct lm_client *c)
{
    int mode = s->srv->config.settings_access;
    if (mode < 0)
        mode = !strcmp(s->srv->config.backend, "headless") ? 1 : 0;
    return mode == 1 || client_is_own(c);
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
    b->res = lm_resource_create(client, id, &lm_mellagui_settings_v1_interface, version, handlers, b, binding_destroy);
    if (!b->res) {
        free(b);
        return;
    }
    if (!access_allowed(s, client)) {                               /* bound, but inert: no values, requests are ignored */
        lm_warn("settings: client pid %d is not one of MellaGUI's programs, the settings stay closed to it", (int)client->pid);
        b->s = NULL;
        lm_list_init(&b->link);                                     /* binding_destroy() unlinks it */
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
    s->revert_timer = lm_loop_add_timer(srv->loop, revert_cb, s);
    s->rescan_timer = lm_loop_add_timer(srv->loop, rescan_cb, s);
    lm_sound_set_device(srv->config.audio_device);
    s->global = lm_global_create(srv, &lm_mellagui_settings_v1_interface, LM_MELLAGUI_SETTINGS_V1_VERSION,
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
    if (s->revert_timer)
        lm_timer_destroy(s->revert_timer);
    if (s->rescan_timer)
        lm_timer_destroy(s->rescan_timer);
    free(s);
}
