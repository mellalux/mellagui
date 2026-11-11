/*
 * User settings: mellagui_settings_v1 global + the settings file. See settings.h for the contract.
 */
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "audio.h"
#include "confdefault.h"
#include "confedit.h"
#include "keys.h"
#include "metrics.h"
#include "theme.h"
#include "launchers.h"
#include <strings.h>
#include "backend.h"
#include "dock.h"
#include "input.h"
#include "sound.h"
#include "nightlight.h"
#include "notifylog.h"
#include "output.h"
#include "keymap.h"
#include "lock.h"
#include "log.h"
#include "render.h"
#include "sha256.h"
#include "saver.h"
#include "seat.h"
#include "server.h"
#include "settings.h"
#include "wm.h"

#define SAVE_DELAY_MS 300

/* Keys before K_FILE are saved to the settings file, the ones after it are read-only or live state only. */
#define KEYS_PAGE_ROWS 10
#define KEYS_HELP_CHUNKS 5            /* the whole shortcut table for mellagui-keys, 12 bindings in each value */
#define KEYS_HELP_PER 12

enum key_id {
    K_LAYOUTS, K_LAYOUT, K_STICKY, K_REPEAT_DELAY, K_REPEAT_RATE, K_POINTER_SPEED, K_NATURAL_SCROLL, K_SOUND,
    K_GADGET, K_MIXER, K_SYSINFO, K_RESOLUTION, K_AUDIO_OUT, K_WALLPAPER, K_WALLPAPER_MODE, K_OUTPUT_ORDER, K_SAVER, K_SAVER_MINUTES, K_SAVER_LOCK, K_OSK_AVOID,
    K_CLOCK_HOURS, K_CLOCK_SECONDS, K_CLOCK_DATE, K_CLOCK_WEEKDAY, K_DATE_FORMAT, K_CAL_ENABLED, K_CAL_WEEK_START, K_CAL_WEEKS, K_AUDIO_PROFILE_KEEP, K_NIGHT_LIGHT, K_NIGHT_TEMP, K_NIGHT_FROM, K_NIGHT_TO, K_DND, K_STARTUP_SOUND, K_FILE,
    K_RESOLUTIONS, K_RESOLUTION_NOW, K_MONITOR, K_RESOLUTION_CONFIRM, K_AUDIO_OUTS, K_SINKS, K_SINK, K_SINKS_STATUS, K_AUDIO_PROFILES, K_AUDIO_PROFILE, K_AUDIO_RESCAN, K_WALLPAPER_STATUS, K_OUTPUTS,
    K_LOCK_KIND, K_LOCK_TIMEOUT, K_LOCK_START, K_LOCK_BLANK, K_LOCK_VT, K_LOCK_STATUS, K_LOCK_SET, K_LOCK_NOW, K_LOCKED, K_NOTIFY, K_CONFIG_FILE, K_CONFIG_STATUS, K_CONFIG_RELOAD, K_CONFIG_CREATE, K_SAVER_NOW,
    K_KEYS_HELP_0, K_KEYS_HELP_LAST = K_KEYS_HELP_0 + KEYS_HELP_CHUNKS - 1,
    K_KEYS_COUNT, K_KEYS_PAGE, K_KEYS_ROW_0, K_KEYS_ROW_LAST = K_KEYS_ROW_0 + KEYS_PAGE_ROWS - 1, K_KEYS_ACTIONS, K_KEYS_CAPTURE,
    K_KEYS_CAPTURED, K_KEYS_SET, K_KEYS_DEL, K_KEYS_RESET,
    K_THEME_COLORS, K_THEME_SET, K_THEME_DEL, K_THEME_RESET, K_THEME_NAME, K_THEME_LIST, K_THEME_USE,
    K_METRICS, K_METRICS_SET, K_METRICS_RESET,
    K_DOCK_0, K_DOCK_LAST = K_DOCK_0 + LM_DOCK_MAX - 1, K_DOCK_PUT, K_DOCK_DEL, K_DOCK_MOVE, K_DOCK_RESET, K_DOCK_OPTS, K_DOCK_SET,
    K_NOTES_0, K_NOTES_LAST = K_NOTES_0 + 4, K_NOTES_COUNT, K_NOTES_UNREAD, K_NOTES_DEL, K_NOTES_CLEAR, K_NOTES_READ, K_POINTER_OUTPUT, K_CLIP_PNG, K_STARTUP_SOUND_PLAY, K_LAUNCHER_DEFAULTS, K_COUNT
};
#define VALUE_MAX 3300          /* longest value text (the lists); the wire carries it as one string */

static const char *const key_names[K_COUNT] = {
    [K_LAYOUTS] = "layouts", [K_LAYOUT] = "layout", [K_STICKY] = "sticky", [K_REPEAT_DELAY] = "repeat-delay",
    [K_REPEAT_RATE] = "repeat-rate", [K_POINTER_SPEED] = "pointer-speed", [K_NATURAL_SCROLL] = "natural-scroll",
    [K_SOUND] = "sound", [K_GADGET] = "gadget", [K_MIXER] = "mixer", [K_SYSINFO] = "sysinfo", [K_RESOLUTION] = "resolution", [K_AUDIO_OUT] = "audio-out", [K_WALLPAPER] = "wallpaper", [K_WALLPAPER_MODE] = "wallpaper-mode", [K_OUTPUT_ORDER] = "output-order", [K_SAVER] = "saver", [K_SAVER_MINUTES] = "saver-minutes", [K_SAVER_LOCK] = "saver-lock", [K_OSK_AVOID] = "osk-avoid",
    [K_CLOCK_HOURS] = "clock-hours", [K_CLOCK_SECONDS] = "clock-seconds", [K_CLOCK_DATE] = "clock-date", [K_CLOCK_WEEKDAY] = "clock-weekday",
    [K_DATE_FORMAT] = "date-format", [K_CAL_ENABLED] = "cal-enabled", [K_CAL_WEEK_START] = "cal-week-start", [K_CAL_WEEKS] = "cal-weeks", [K_AUDIO_PROFILE_KEEP] = "audio-profile-keep", [K_NIGHT_LIGHT] = "night-light", [K_NIGHT_TEMP] = "night-temp", [K_NIGHT_FROM] = "night-from", [K_NIGHT_TO] = "night-to", [K_DND] = "dnd", [K_STARTUP_SOUND] = "startup-sound", [K_FILE] = "file",
    [K_RESOLUTIONS] = "resolutions", [K_RESOLUTION_NOW] = "resolution-now", [K_MONITOR] = "monitor", [K_RESOLUTION_CONFIRM] = "resolution-confirm",
    [K_AUDIO_OUTS] = "audio-outs", [K_SINKS] = "sinks", [K_SINK] = "sink", [K_SINKS_STATUS] = "sinks-status", [K_AUDIO_PROFILES] = "audio-profiles",
    [K_AUDIO_PROFILE] = "audio-profile", [K_AUDIO_RESCAN] = "audio-rescan", [K_WALLPAPER_STATUS] = "wallpaper-status", [K_OUTPUTS] = "outputs",
    [K_LOCK_KIND] = "lock-kind", [K_LOCK_TIMEOUT] = "lock-timeout", [K_LOCK_START] = "lock-start", [K_LOCK_BLANK] = "lock-blank", [K_LOCK_VT] = "lock-vt", [K_LOCK_STATUS] = "lock-status",
    [K_LOCK_SET] = "lock-set", [K_LOCK_NOW] = "lock-now", [K_LOCKED] = "locked", [K_NOTIFY] = "notify",
    [K_CONFIG_FILE] = "config-file", [K_CONFIG_STATUS] = "config-status", [K_CONFIG_RELOAD] = "config-reload", [K_CONFIG_CREATE] = "config-create", [K_SAVER_NOW] = "saver-now",
    [K_KEYS_HELP_0] = "keys-help-0", [K_KEYS_HELP_0 + 1] = "keys-help-1", [K_KEYS_HELP_0 + 2] = "keys-help-2", [K_KEYS_HELP_0 + 3] = "keys-help-3", [K_KEYS_HELP_0 + 4] = "keys-help-4",
    [K_KEYS_COUNT] = "keys-count", [K_KEYS_PAGE] = "keys-page", [K_KEYS_ACTIONS] = "keys-actions", [K_KEYS_CAPTURE] = "keys-capture",
    [K_KEYS_CAPTURED] = "keys-captured", [K_KEYS_SET] = "keys-set", [K_KEYS_DEL] = "keys-del", [K_KEYS_RESET] = "keys-reset",
    [K_THEME_COLORS] = "theme-colors", [K_THEME_SET] = "theme-set", [K_THEME_DEL] = "theme-del", [K_THEME_RESET] = "theme-reset",
    [K_THEME_NAME] = "theme-name", [K_THEME_LIST] = "theme-list", [K_THEME_USE] = "theme-use",
    [K_METRICS] = "metrics", [K_METRICS_SET] = "metrics-set", [K_METRICS_RESET] = "metrics-reset",
    [K_DOCK_PUT] = "dock-put", [K_DOCK_DEL] = "dock-del", [K_DOCK_MOVE] = "dock-move", [K_DOCK_RESET] = "dock-reset",
    [K_DOCK_OPTS] = "dock-opts", [K_DOCK_SET] = "dock-set",
#define DK(i) [K_DOCK_0 + (i)] = "dock-" #i
    DK(0), DK(1), DK(2), DK(3), DK(4), DK(5), DK(6), DK(7), DK(8), DK(9), DK(10), DK(11), DK(12), DK(13), DK(14), DK(15),
#undef DK
#define KR(i) [K_KEYS_ROW_0 + (i)] = "keys-row-" #i
    KR(0), KR(1), KR(2), KR(3), KR(4), KR(5), KR(6), KR(7), KR(8), KR(9),
#undef KR
    [K_NOTES_0] = "notes-0", [K_NOTES_0 + 1] = "notes-1", [K_NOTES_0 + 2] = "notes-2", [K_NOTES_0 + 3] = "notes-3", [K_NOTES_0 + 4] = "notes-4",
    [K_NOTES_COUNT] = "notes-count", [K_NOTES_UNREAD] = "notes-unread", [K_NOTES_DEL] = "notes-del", [K_NOTES_CLEAR] = "notes-clear",
    [K_NOTES_READ] = "notes-read", [K_POINTER_OUTPUT] = "pointer-output", [K_CLIP_PNG] = "clip-png", [K_STARTUP_SOUND_PLAY] = "startup-sound-play", [K_LAUNCHER_DEFAULTS] = "launcher-defaults",
};
_Static_assert(LM_NOTELOG_MAX == 5 * LM_NOTELOG_CHUNK, "notes-0 .. notes-4 hold the whole history");
_Static_assert(KEYS_PAGE_ROWS == 10, "key_names lists keys-row-0 .. keys-row-9");
_Static_assert(LM_DOCK_MAX == 16, "key_names lists dock-0 .. dock-15");

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
    struct lm_timer *profile_timer;         /* puts the saved card profile back once PipeWire is up */
    int profile_tries;
    struct lm_timer *rescan_timer;          /* after a profile change PipeWire needs a moment to make the new sink */
    int rescan_left;
    struct lm_timer *capture_timer;         /* gives up a key capture nobody finished (the panel was closed meanwhile) */
    int keys_page;          /* which ten shortcuts keys-row-0..9 show (the Keys page of the panel) */
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
    case K_AUDIO_PROFILE_KEEP:                          /* "card description|profile name", see audio.h */
        if (strlen(val) >= sizeof(cfg->audio_profile) || strchr(val, '\n')) return -1;
        snprintf(cfg->audio_profile, sizeof(cfg->audio_profile), "%s", val);
        break;
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
    case K_SAVER:
        if (!parse_bool(val, &b)) return -1;
        cfg->saver = b;
        break;
    case K_SAVER_MINUTES:
        if (!parse_int(val, 0, 240, &n)) return -1;
        cfg->saver_min = n;
        break;
    case K_SAVER_LOCK:
        if (!parse_int(val, 0, 120, &n)) return -1;
        cfg->saver_lock_min = n;
        break;
    case K_OSK_AVOID:
        if (!parse_bool(val, &b)) return -1;
        cfg->osk_avoid = b;
        break;
    case K_NIGHT_LIGHT:
        if (!strcmp(val, "off")) cfg->night_mode = 0;
        else if (!strcmp(val, "on")) cfg->night_mode = 1;
        else if (!strcmp(val, "auto")) cfg->night_mode = 2;
        else return -1;
        break;
    case K_DND:
        if (!parse_bool(val, &b)) return -1;
        cfg->dnd = b;
        break;
    case K_STARTUP_SOUND:
        if (!parse_bool(val, &b)) return -1;
        cfg->startup_sound = b;
        break;
    case K_NIGHT_TEMP:
        if (!parse_int(val, 1500, 6500, &n)) return -1;
        cfg->night_kelvin = n;
        break;
    case K_NIGHT_FROM:
    case K_NIGHT_TO: {
        int m = lm_night_parse_time(val);
        if (m < 0 || strlen(val) > 5) return -1;
        char t[16];
        snprintf(t, sizeof(t), "%02d:%02d", (m / 60) % 24, m % 60);
        snprintf(k == K_NIGHT_FROM ? cfg->night_from : cfg->night_to, 8, "%.5s", t);
        break;
    }
    case K_CLOCK_HOURS:
        if (!strcmp(val, "12")) cfg->clock.h12 = true;
        else if (!strcmp(val, "24")) cfg->clock.h12 = false;
        else return -1;
        break;
    case K_CLOCK_SECONDS:
        if (!parse_bool(val, &b)) return -1;
        cfg->clock.seconds = b;
        break;
    case K_CLOCK_DATE:
        if (!parse_bool(val, &b)) return -1;
        cfg->clock.show_date = b;
        break;
    case K_CLOCK_WEEKDAY:
        if (!parse_bool(val, &b)) return -1;
        cfg->clock.weekday = b;
        break;
    case K_DATE_FORMAT:
        if ((n = lm_date_format_parse(val)) < 0) return -1;
        cfg->clock.date_fmt = n;
        break;
    case K_CAL_ENABLED:
        if (!parse_bool(val, &b)) return -1;
        cfg->cal_enabled = b;
        break;
    case K_CAL_WEEK_START:
        if (!strcmp(val, "mon")) cfg->cal_week_start = 1;
        else if (!strcmp(val, "sun")) cfg->cal_week_start = 0;
        else return -1;
        break;
    case K_CAL_WEEKS:
        if (!parse_bool(val, &b)) return -1;
        cfg->cal_weeknum = b;
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

static int keys_page_clamped(const struct lm_settings *s)
{
    int last = s->srv->config.keys.n > 0 ? (s->srv->config.keys.n - 1) / KEYS_PAGE_ROWS : 0;
    return LM_CLAMP(s->keys_page, 0, last);
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
    case K_SAVER: snprintf(out, n, "%d", c->saver ? 1 : 0); break;
    case K_SAVER_MINUTES: snprintf(out, n, "%d", c->saver_min); break;
    case K_SAVER_LOCK: snprintf(out, n, "%d", c->saver_lock_min); break;
    case K_OSK_AVOID: snprintf(out, n, "%d", c->osk_avoid ? 1 : 0); break;
    case K_NIGHT_LIGHT: snprintf(out, n, "%s", c->night_mode == 1 ? "on" : c->night_mode == 2 ? "auto" : "off"); break;
    case K_NIGHT_TEMP: snprintf(out, n, "%d", c->night_kelvin); break;
    case K_DND: snprintf(out, n, "%d", c->dnd ? 1 : 0); break;
    case K_STARTUP_SOUND: snprintf(out, n, "%d", c->startup_sound ? 1 : 0); break;
    case K_STARTUP_SOUND_PLAY: case K_LAUNCHER_DEFAULTS: break;                      /* write only */
    case K_NOTES_0 ... K_NOTES_LAST: lm_notelog_text(srv->notelog, k - K_NOTES_0, out, n); break;
    case K_NOTES_COUNT: snprintf(out, n, "%d", lm_notelog_count(srv->notelog)); break;
    case K_NOTES_UNREAD: snprintf(out, n, "%d", lm_notelog_unread(srv->notelog)); break;
    case K_NOTES_DEL: case K_NOTES_CLEAR: case K_NOTES_READ: break;                 /* write only */
    case K_CLIP_PNG: break;                                                          /* write only */
    case K_POINTER_OUTPUT: {                    /* index (as mellagui_capture_v1 numbers them) of the output the pointer is on, -1 = none */
        int idx = -1, i = 0;
        double px = 0, py = 0;
        if (srv->backend && srv->seat) {
            lm_seat_pointer_position(srv->seat, &px, &py);
            struct lm_output *near = lm_output_nearest(srv, px, py), *o;
            lm_list_for_each(o, &srv->backend->outputs, link) {
                if (o == near)
                    idx = i;
                i++;
            }
        }
        snprintf(out, n, "%d", idx);
        break;
    }
    case K_NIGHT_FROM: snprintf(out, n, "%s", c->night_from); break;
    case K_NIGHT_TO: snprintf(out, n, "%s", c->night_to); break;
    case K_CLOCK_HOURS: snprintf(out, n, "%d", c->clock.h12 ? 12 : 24); break;
    case K_CLOCK_SECONDS: snprintf(out, n, "%d", c->clock.seconds ? 1 : 0); break;
    case K_CLOCK_DATE: snprintf(out, n, "%d", c->clock.show_date ? 1 : 0); break;
    case K_CLOCK_WEEKDAY: snprintf(out, n, "%d", c->clock.weekday ? 1 : 0); break;
    case K_DATE_FORMAT: snprintf(out, n, "%s", lm_date_format_name(c->clock.date_fmt)); break;
    case K_CAL_ENABLED: snprintf(out, n, "%d", c->cal_enabled ? 1 : 0); break;
    case K_CAL_WEEK_START: snprintf(out, n, "%s", c->cal_week_start ? "mon" : "sun"); break;
    case K_CAL_WEEKS: snprintf(out, n, "%d", c->cal_weeknum ? 1 : 0); break;
    case K_SAVER_NOW: break;                                            /* write only */
    case K_GADGET: snprintf(out, n, "%d", srv->wm && lm_wm_gadget_visible(srv->wm) ? 1 : 0); break;
    case K_MIXER: snprintf(out, n, "%d", srv->wm && lm_wm_gadget_visible_id(srv->wm, LM_GADGET_MIXER) ? 1 : 0); break;
    case K_SYSINFO: snprintf(out, n, "%d", srv->wm && lm_wm_gadget_visible_id(srv->wm, LM_GADGET_SYSTEM) ? 1 : 0); break;
    case K_RESOLUTION: resolution_text(c->drm_mode_w, c->drm_mode_h, c->drm_mode_hz, out, n); break;
    case K_AUDIO_OUT: snprintf(out, n, "%s", c->audio_device); break;
    case K_AUDIO_PROFILE_KEEP: snprintf(out, n, "%s", c->audio_profile); break;
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
    case K_KEYS_COUNT: snprintf(out, n, "%d", c->keys.n); break;
    case K_KEYS_HELP_0 ... K_KEYS_HELP_LAST: {  /* "Super+Q<US>close" lines (separated by newlines) of bindings 12 * chunk ... : mellagui-keys lists them all */
        size_t o = 0;
        for (int i = (k - K_KEYS_HELP_0) * KEYS_HELP_PER; i < c->keys.n && i < (k - K_KEYS_HELP_0 + 1) * KEYS_HELP_PER && o + 80 < n; i++) {
            char combo[64], act[LM_KEYBIND_ARG + 40];
            if (!lm_keys_combo_text(c->keys.b[i].mods, c->keys.b[i].code, combo, sizeof(combo)))
                snprintf(combo, sizeof(combo), "?");
            lm_keys_action_text(&c->keys.b[i], act, sizeof(act));
            o += (size_t)snprintf(out + o, n - o, "%s%s\x1f%s", o ? "\n" : "", combo, act);
        }
        break;
    }
    case K_KEYS_PAGE: snprintf(out, n, "%d", keys_page_clamped(s)); break;
    case K_KEYS_ACTIONS: snprintf(out, n, "%s", lm_keys_action_names()); break;
    case K_KEYS_CAPTURE: snprintf(out, n, "%d", srv->wm && lm_wm_key_capturing(srv->wm) ? 1 : 0); break;
    case K_KEYS_CAPTURED: snprintf(out, n, "%s", srv->wm ? lm_wm_key_captured(srv->wm) : ""); break;
    case K_KEYS_SET: case K_KEYS_DEL: case K_KEYS_RESET: break;
    case K_THEME_SET: case K_THEME_DEL: case K_THEME_RESET: case K_THEME_USE: break;
    case K_THEME_NAME:                          /* the active theme */
        snprintf(out, n, "%s", lm_theme_name());
        break;
    case K_THEME_LIST: {                        /* "colorful* graphite light contrast ...": every theme there is, the active one marked with a * */
        char names[24][LM_THEME_NAME_MAX];
        int cnt = lm_theme_list(names, NULL, 24);
        size_t o = 0;
        for (int i = 0; i < cnt && o + LM_THEME_NAME_MAX + 3 < n; i++)
            o += (size_t)snprintf(out + o, n - o, "%s%s%s", i ? " " : "", names[i], !strcasecmp(names[i], lm_theme_name()) ? "*" : "");
        break;
    }
    case K_METRICS_SET: case K_METRICS_RESET: break;
    case K_METRICS: {                           /* "section.name=value:min:max:builtin ..." every size of metrics.h */
        const char *sec, *nm;
        int v, d, lo, hi;
        size_t o = 0;
        for (int i = 0; lm_metrics_entry(i, &sec, &nm, &v, &d, &lo, &hi) && o + 80 < n; i++)
            o += (size_t)snprintf(out + o, n - o, "%s%s.%s=%d:%d:%d:%d", i ? " " : "", sec, nm, v, lo, hi, d);
        break;
    }
    case K_THEME_COLORS: {                      /* "name=#rrggbb name=#rrggbb* ...": a * marks a colour that differs from the built-in one */
        const char *nm;
        uint32_t v, d;
        size_t o = 0;
        for (int i = 0; lm_theme_entry(i, &nm, &v, &d) && o + 48 < n; i++)
            o += (size_t)snprintf(out + o, n - o, "%s%s=#%06x%s", i ? " " : "", nm, (unsigned)v, v != d ? "*" : "");
        break;
    }
    case K_KEYS_ROW_0 ... K_KEYS_ROW_LAST: {    /* "Super+Q<US>close" of the shortcut at keys-page * 10 + row, "" past the end */
        int i = keys_page_clamped(s) * KEYS_PAGE_ROWS + (k - K_KEYS_ROW_0);
        if (i < c->keys.n) {
            char combo[64], act[LM_KEYBIND_ARG + 40];
            if (!lm_keys_combo_text(c->keys.b[i].mods, c->keys.b[i].code, combo, sizeof(combo)))
                snprintf(combo, sizeof(combo), "?");
            lm_keys_action_text(&c->keys.b[i], act, sizeof(act));
            snprintf(out, n, "%s\x1f%s", combo, act);
        }
        break;
    }
    case K_DOCK_0 ... K_DOCK_LAST: {            /* "name<US>icon<US>command" of button i of [dock-apps] (raw text), "" past the end */
        const char *nm, *ic, *cm;
        if (lm_dock_raw_entry(k - K_DOCK_0, &nm, &ic, &cm))
            snprintf(out, n, "%s\x1f%s\x1f%s", nm, ic, cm);
        break;
    }
    case K_DOCK_OPTS:                           /* "enabled=1 autohide=0 icon-size=48 zoom=160 gap=10 margin=8" */
        {
            char bg[16];
            lm_dock_bg_text(bg, sizeof(bg));
            snprintf(out, n, "enabled=%d autohide=%d icon-size=%d zoom=%d gap=%d margin=%d icon-background=%s", lm_dock.enabled, lm_dock.autohide,
                     lm_dock.icon_size, lm_dock.zoom, lm_dock.gap, lm_dock.margin, bg);
        }
        break;
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
        if (k == K_RESOLUTIONS || k == K_AUDIO_OUTS || k == K_SINKS || (k >= K_DOCK_0 && k <= K_DOCK_LAST) || k == K_DOCK_OPTS || k == K_KEYS_ACTIONS || k == K_THEME_COLORS || k == K_THEME_LIST || k == K_METRICS || (k >= K_KEYS_ROW_0 && k <= K_KEYS_ROW_LAST) || (k >= K_KEYS_HELP_0 && k <= K_KEYS_HELP_LAST) || (k >= K_NOTES_0 && k <= K_NOTES_LAST))
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

/* Reads a PNG file of at most 64 MB (regular file, PNG signature) and offers it as the clipboard selection. */
static bool clip_png_file(struct lm_server *srv, const char *path)
{
    if (!srv->seat || !path || path[0] != '/')
        return false;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return false;
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 8 || st.st_size > (off_t)64 << 20) {
        close(fd);
        return false;
    }
    uint8_t *buf = malloc((size_t)st.st_size);
    size_t got = 0;
    while (buf && got < (size_t)st.st_size) {
        ssize_t k = read(fd, buf + got, (size_t)st.st_size - got);
        if (k <= 0)
            break;
        got += (size_t)k;
    }
    close(fd);
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
    if (!buf || got != (size_t)st.st_size || memcmp(buf, sig, 8) != 0) {
        free(buf);
        return false;
    }
    return lm_seat_set_selection_data(srv->seat, "image/png", buf, got);        /* takes the buffer over */
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

static void capture_cb(void *data)
{
    struct lm_settings *s = data;
    if (s->srv->wm && lm_wm_key_capturing(s->srv->wm)) {
        lm_wm_key_capture(s->srv->wm, false);
        broadcast(s);
    }
}

/* At start (and after a PipeWire restart is not watched): the profile of the last session, tried for ~30 s while the sound server comes up. */
static void profile_cb(void *data)
{
    struct lm_settings *s = data;
    int r = lm_audio_apply_profile_key(s->srv->config.audio_profile);
    if (r < 0 && ++s->profile_tries < 10) {
        lm_timer_arm_ms(s->profile_timer, 3000, 0);
        return;
    }
    if (r == 0) {
        audio_rescan(s);
        broadcast(s);
    }
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

/* ---- names and the field separator the Dock page shares ----------------------------------------------------------------- */

#define MENU_US '\x1f'                      /* separates the fields of a dock value */

static bool dock_name_ok(const char *s)
{
    size_t n = strlen(s);
    if (!n || n >= 64 || s[0] == ' ' || s[n - 1] == ' ' || s[0] == '#' || s[0] == ';' || s[0] == '[')
        return false;
    for (const char *p = s; *p; p++)
        if (*p == '=' || (unsigned char)*p < 0x20)
            return false;
    return true;
}

/* ---- the [dock] and [dock-apps] sections, edited from the panel's Dock page ------------------------------------------------- */

struct dock_list {
    char name[LM_DOCK_MAX][LM_DOCK_NAME_MAX];
    char icon[LM_DOCK_MAX][LM_DOCK_ICON_MAX];
    char cmd[LM_DOCK_MAX][LM_DOCK_CMD_MAX];
    int n;
};

static void dock_load(struct dock_list *l)
{
    const char *nm, *ic, *cm;
    l->n = 0;
    while (l->n < LM_DOCK_MAX && lm_dock_raw_entry(l->n, &nm, &ic, &cm)) {
        snprintf(l->name[l->n], sizeof(l->name[0]), "%s", nm);
        snprintf(l->icon[l->n], sizeof(l->icon[0]), "%s", ic);
        snprintf(l->cmd[l->n], sizeof(l->cmd[0]), "%s", cm);
        l->n++;
    }
}

static bool dock_text_ok(const char *s, size_t max, bool allow_empty)
{
    size_t n = strlen(s);
    if ((!n && !allow_empty) || n >= max || (n && (s[0] == ' ' || s[n - 1] == ' ')))
        return false;
    for (const char *p = s; *p; p++)
        if ((unsigned char)*p < 0x20 && *p != '\t')
            return false;
    return true;
}

static bool dock_commit(struct lm_server *srv, const struct dock_list *l, const char *section)
{
    const char *path = srv->config.conf_path;
    if (!path || !*path || !strcmp(path, "none") || !srv->reload_config)
        return false;
    size_t cap = (size_t)LM_DOCK_MAX * (LM_DOCK_NAME_MAX + LM_DOCK_ICON_MAX + LM_DOCK_CMD_MAX + 16) + 1;
    char *body = malloc(cap);
    if (!body)
        return false;
    size_t o = 0;
    for (int i = 0; i < l->n; i++) {
        if (l->icon[i][0])
            o += (size_t)snprintf(body + o, cap - o, "%s = [%s] %s\n", l->name[i], l->icon[i], l->cmd[i]);
        else
            o += (size_t)snprintf(body + o, cap - o, "%s = %s\n", l->name[i], l->cmd[i]);
    }
    body[o] = 0;
    bool ok = lm_confedit_set_section(path, section, body);
    free(body);
    if (ok)
        srv->reload_config(srv, true);
    return ok;
}

static bool dock_same(const char *existing, void *ud) { return !strcmp(existing, ud); }

static bool dock_op(struct lm_server *srv, int kid, const char *val)
{
    static struct dock_list l;                  /* 22 KB: not on the stack */
    const char *path = srv->config.conf_path;
    if (!path || !*path || !strcmp(path, "none") || !srv->reload_config)
        return false;
    if (kid == K_DOCK_RESET) {                  /* "1": the whole dock (settings and buttons) back to the built-in one */
        if (strcmp(val, "1"))
            return false;
        bool ok = lm_confedit_set_section(path, "dock-apps", NULL);
        ok = lm_confedit_set_section(path, "dock", NULL) && ok;
        if (ok)
            srv->reload_config(srv, true);
        return ok;
    }
    if (kid == K_DOCK_SET) {                    /* "option<US>value" */
        char buf[96];
        if (strlen(val) >= sizeof(buf))
            return false;
        strcpy(buf, val);
        char *v = strchr(buf, MENU_US);
        bool builtin;
        if (!v)
            return false;
        *v++ = 0;
        if (!lm_dock_check(buf, v, &builtin))
            return false;
        bool ok = lm_confedit_set_key(path, "dock", dock_same, buf, buf, builtin ? NULL : v);   /* the built-in value needs no line */
        if (ok)
            srv->reload_config(srv, true);
        return ok;
    }
    dock_load(&l);
    static char buf[1700];
    if (strlen(val) >= sizeof(buf))
        return false;
    strcpy(buf, val);
    char *f1 = strchr(buf, MENU_US);
    if (f1)
        *f1++ = 0;
    char *idx_end;
    long idx = strtol(buf, &idx_end, 10);
    if (*idx_end)
        return false;
    if (kid == K_DOCK_DEL) {
        if (f1 || idx < 0 || idx >= l.n || lm_dock_cmd_is_launcher(l.cmd[idx]))      /* (the Launcher button cannot be removed) */
            return false;
        for (int i = (int)idx; i + 1 < l.n; i++) {
            memcpy(l.name[i], l.name[i + 1], sizeof(l.name[0]));
            memcpy(l.icon[i], l.icon[i + 1], sizeof(l.icon[0]));
            memcpy(l.cmd[i], l.cmd[i + 1], sizeof(l.cmd[0]));
        }
        l.n--;
    } else if (kid == K_DOCK_MOVE) {
        char *e;
        long d = f1 ? strtol(f1, &e, 10) : 0;
        if (!f1 || *e || (d != -1 && d != 1) || idx < 0 || idx >= l.n || idx + d < 0 || idx + d >= l.n)
            return false;
        char tn[LM_DOCK_NAME_MAX], ti[LM_DOCK_ICON_MAX], tc[LM_DOCK_CMD_MAX];
        memcpy(tn, l.name[idx], sizeof(tn));
        memcpy(ti, l.icon[idx], sizeof(ti));
        memcpy(tc, l.cmd[idx], sizeof(tc));
        memcpy(l.name[idx], l.name[idx + d], sizeof(tn));
        memcpy(l.icon[idx], l.icon[idx + d], sizeof(ti));
        memcpy(l.cmd[idx], l.cmd[idx + d], sizeof(tc));
        memcpy(l.name[idx + d], tn, sizeof(tn));
        memcpy(l.icon[idx + d], ti, sizeof(ti));
        memcpy(l.cmd[idx + d], tc, sizeof(tc));
    } else {                                    /* K_DOCK_PUT: "index<US>name<US>icon<US>command", index -1 = a new button */
        char *icon = f1 ? strchr(f1, MENU_US) : NULL, *cmd = icon ? strchr(icon + 1, MENU_US) : NULL;
        if (!cmd)
            return false;
        *icon++ = 0;
        *cmd++ = 0;
        if (!dock_name_ok(f1) || strlen(f1) >= LM_DOCK_NAME_MAX || !dock_text_ok(icon, LM_DOCK_ICON_MAX, true) || strchr(icon, ']') ||
            icon[0] == '[' || !dock_text_ok(cmd, LM_DOCK_CMD_MAX, false) || idx < -1 || idx >= l.n || (idx < 0 && l.n >= LM_DOCK_MAX))
            return false;
        for (int i = 0; i < l.n; i++)
            if (i != idx && !strcasecmp(l.name[i], f1))
                return false;
        if (idx >= 0 && lm_dock_cmd_is_launcher(l.cmd[idx]) && !lm_dock_cmd_is_launcher(cmd))
            return false;                                       /* the Launcher button keeps its command */
        if (idx < 0)
            idx = l.n++;
        snprintf(l.name[idx], sizeof(l.name[0]), "%s", f1);
        snprintf(l.icon[idx], sizeof(l.icon[0]), "%s", icon);
        snprintf(l.cmd[idx], sizeof(l.cmd[0]), "%s", cmd);
    }
    return dock_commit(srv, &l, "dock-apps");
}

/* ---- what the launcher dialog and the dock's drag and drop do: [dock-apps] and [launcher-apps], one rewrite per change ------------------ */

static void apps_load(struct dock_list *l, bool launcher)
{
    const char *nm, *ic, *cm;
    l->n = 0;
    while (l->n < LM_DOCK_MAX && (launcher ? lm_launchapps_raw_entry(l->n, &nm, &ic, &cm) : lm_dock_raw_entry(l->n, &nm, &ic, &cm))) {
        snprintf(l->name[l->n], sizeof(l->name[0]), "%s", nm);
        snprintf(l->icon[l->n], sizeof(l->icon[0]), "%s", ic);
        snprintf(l->cmd[l->n], sizeof(l->cmd[0]), "%s", cm);
        l->n++;
    }
}

static void apps_move_entry(struct dock_list *l, int from, int to)          /* remove entry `from`, insert it so that it ends up at `to` */
{
    char n[LM_DOCK_NAME_MAX], i[LM_DOCK_ICON_MAX], c[LM_DOCK_CMD_MAX];
    memcpy(n, l->name[from], sizeof(n));
    memcpy(i, l->icon[from], sizeof(i));
    memcpy(c, l->cmd[from], sizeof(c));
    for (int k = from; k < l->n - 1; k++) {
        memcpy(l->name[k], l->name[k + 1], sizeof(l->name[0]));
        memcpy(l->icon[k], l->icon[k + 1], sizeof(l->icon[0]));
        memcpy(l->cmd[k], l->cmd[k + 1], sizeof(l->cmd[0]));
    }
    for (int k = l->n - 1; k > to; k--) {
        memcpy(l->name[k], l->name[k - 1], sizeof(l->name[0]));
        memcpy(l->icon[k], l->icon[k - 1], sizeof(l->icon[0]));
        memcpy(l->cmd[k], l->cmd[k - 1], sizeof(l->cmd[0]));
    }
    memcpy(l->name[to], n, sizeof(n));
    memcpy(l->icon[to], i, sizeof(i));
    memcpy(l->cmd[to], c, sizeof(c));
}

/* Add a button at index `at` (-1 or past the end = last) of [dock-apps] or [launcher-apps]. */
static bool apps_put(struct lm_server *srv, bool launcher, const char *name, const char *icon, const char *cmd, int at)
{
    static struct dock_list l;
    apps_load(&l, launcher);
    if (!dock_name_ok(name) || strlen(name) >= LM_DOCK_NAME_MAX || !dock_text_ok(icon, LM_DOCK_ICON_MAX, true) || strchr(icon, ']') ||
        icon[0] == '[' || !dock_text_ok(cmd, LM_DOCK_CMD_MAX, false) || l.n >= LM_DOCK_MAX)
        return false;
    for (int i = 0; i < l.n; i++)
        if (!strcasecmp(l.name[i], name))
            return false;
    int k = l.n++;
    snprintf(l.name[k], sizeof(l.name[0]), "%s", name);
    snprintf(l.icon[k], sizeof(l.icon[0]), "%s", icon);
    snprintf(l.cmd[k], sizeof(l.cmd[0]), "%s", cmd);
    if (at >= 0 && at < k)
        apps_move_entry(&l, k, at);
    return dock_commit(srv, &l, launcher ? "launcher-apps" : "dock-apps");
}

/* Change entry `raw` of [dock-apps] / [launcher-apps] in place (name, icon text and command); its place in the list stays. The name may not be
 * one that another entry has (its own, in any letter case, is fine). */
static bool apps_replace(struct lm_server *srv, bool launcher, int raw, const char *name, const char *icon, const char *cmd)
{
    static struct dock_list l;
    apps_load(&l, launcher);
    if (raw < 0 || raw >= l.n)
        return false;
    if (!dock_name_ok(name) || strlen(name) >= LM_DOCK_NAME_MAX || !dock_text_ok(icon, LM_DOCK_ICON_MAX, true) || strchr(icon, ']') ||
        icon[0] == '[' || !dock_text_ok(cmd, LM_DOCK_CMD_MAX, false))
        return false;
    for (int i = 0; i < l.n; i++)
        if (i != raw && !strcasecmp(l.name[i], name))
            return false;
    if (!launcher && lm_dock_cmd_is_launcher(l.cmd[raw]) && !lm_dock_cmd_is_launcher(cmd))
        return false;                                           /* the Launcher button keeps its command */
    snprintf(l.name[raw], sizeof(l.name[0]), "%s", name);
    snprintf(l.icon[raw], sizeof(l.icon[0]), "%s", icon);
    snprintf(l.cmd[raw], sizeof(l.cmd[0]), "%s", cmd);
    return dock_commit(srv, &l, launcher ? "launcher-apps" : "dock-apps");
}

static bool apps_del(struct lm_server *srv, bool launcher, int raw)
{
    static struct dock_list l;
    apps_load(&l, launcher);
    if (raw < 0 || raw >= l.n)
        return false;
    if (!launcher && lm_dock_cmd_is_launcher(l.cmd[raw]))
        return false;                                           /* the Launcher button stays in the dock */
    for (int i = raw; i + 1 < l.n; i++) {
        memcpy(l.name[i], l.name[i + 1], sizeof(l.name[0]));
        memcpy(l.icon[i], l.icon[i + 1], sizeof(l.icon[0]));
        memcpy(l.cmd[i], l.cmd[i + 1], sizeof(l.cmd[0]));
    }
    l.n--;
    return dock_commit(srv, &l, launcher ? "launcher-apps" : "dock-apps");
}

bool lm_settings_dock_put(struct lm_server *srv, const char *name, const char *icon, const char *cmd)
{
    return apps_put(srv, false, name, icon, cmd, -1);
}

bool lm_settings_dock_del(struct lm_server *srv, int raw_index) { return apps_del(srv, false, raw_index); }
bool lm_settings_dock_replace(struct lm_server *srv, int raw_index, const char *name, const char *icon, const char *cmd)
{
    return apps_replace(srv, false, raw_index, name, icon, cmd);
}
bool lm_settings_launcher_replace(struct lm_server *srv, int raw_index, const char *name, const char *icon, const char *cmd)
{
    return apps_replace(srv, true, raw_index, name, icon, cmd);
}
bool lm_settings_launcher_put(struct lm_server *srv, const char *name, const char *icon, const char *cmd)
{
    return apps_put(srv, true, name, icon, cmd, -1);
}
bool lm_settings_launcher_del(struct lm_server *srv, int raw_index) { return apps_del(srv, true, raw_index); }

/* The built-in launcher buttons (dock.c DEFAULT_LAUNCHER) that the file's own lists do not have yet - by name or by command - are added to [launcher-apps].
 * Returns how many were added, -1 when nothing could be written (no file). */
int lm_settings_launcher_add_defaults(struct lm_server *srv)
{
    int added = 0;
    const char *nm, *ic, *cm, *n2, *i2, *c2;
    for (int i = 0; lm_launchapps_default(i, &nm, &ic, &cm); i++) {
        bool have = false;
        for (int k = 0; lm_dock_raw_entry(k, &n2, &i2, &c2) && !have; k++)
            have = !strcasecmp(n2, nm) || !strcmp(c2, cm);
        for (int k = 0; lm_launchapps_raw_entry(k, &n2, &i2, &c2) && !have; k++)
            have = !strcasecmp(n2, nm) || !strcmp(c2, cm);
        if (have)
            continue;
        if (!apps_put(srv, true, nm, ic, cm, -1))
            return added ? added : -1;
        added++;
    }
    return added;
}

bool lm_settings_dock_reorder(struct lm_server *srv, int from, int to)
{
    static struct dock_list l;
    apps_load(&l, false);
    if (from < 0 || to < 0 || from >= l.n || to >= l.n)
        return false;
    if (from != to)
        apps_move_entry(&l, from, to);
    return dock_commit(srv, &l, "dock-apps");
}

bool lm_settings_launcher_to_dock(struct lm_server *srv, int raw_index, int at)
{
    const char *nm, *ic, *cm;
    if (!lm_launchapps_raw_entry(raw_index, &nm, &ic, &cm))
        return false;
    char name[LM_DOCK_NAME_MAX], icon[LM_DOCK_ICON_MAX], cmd[LM_DOCK_CMD_MAX];
    snprintf(name, sizeof(name), "%s", nm);
    snprintf(icon, sizeof(icon), "%s", ic);
    snprintf(cmd, sizeof(cmd), "%s", cm);
    if (!apps_put(srv, false, name, icon, cmd, at))
        return false;
    return apps_del(srv, true, raw_index);
}

bool lm_settings_dock_to_launcher(struct lm_server *srv, int raw_index)
{
    const char *nm, *ic, *cm;
    if (!lm_dock_raw_entry(raw_index, &nm, &ic, &cm) || lm_dock_cmd_is_launcher(cm))      /* (the Launcher button stays in the dock) */
        return false;
    char name[LM_DOCK_NAME_MAX], icon[LM_DOCK_ICON_MAX], cmd[LM_DOCK_CMD_MAX];
    snprintf(name, sizeof(name), "%s", nm);
    snprintf(icon, sizeof(icon), "%s", ic);
    snprintf(cmd, sizeof(cmd), "%s", cm);
    bool listed = false;                                    /* the launcher's own list may have it already (the dock's copy won in the dialog) */
    for (int i = 0; lm_launchapps_raw_entry(i, &nm, &ic, &cm); i++)
        if (!strcasecmp(nm, name))
            listed = true;
    if (!listed && !apps_put(srv, true, name, icon, cmd, -1))
        return false;
    return apps_del(srv, false, raw_index);
}

/* ---- the [keys] section, edited from the panel's Keys page ----------------------------------------------------------------- */

static bool keys_same(const char *existing, void *ud) { return lm_keys_same_combo(existing, ud); }

/* Write one line of [keys] (value NULL = drop the line) and read the file again. */
static bool keys_write(struct lm_server *srv, const char *combo, const char *value)
{
    const char *path = srv->config.conf_path;
    if (!path || !*path || !strcmp(path, "none") || !srv->reload_config)
        return false;
    if (!lm_confedit_set_key(path, "keys", keys_same, (void *)combo, combo, value))
        return false;
    srv->reload_config(srv, true);
    return true;
}

static bool keys_op(struct lm_settings *s, int kid, const char *val)
{
    struct lm_server *srv = s->srv;
    const char *path = srv->config.conf_path;
    uint32_t mods, code;
    char canon[64];
    if (kid == K_KEYS_RESET) {
        if (strcmp(val, "1") || !path || !*path || !strcmp(path, "none") || !srv->reload_config)
            return false;
        bool ok = lm_confedit_set_section(path, "keys", NULL);
        if (ok)
            srv->reload_config(srv, true);
        return ok;
    }
    char buf[LM_KEYBIND_ARG + 200];
    if (strlen(val) >= sizeof(buf))
        return false;
    strcpy(buf, val);
    char *act = strchr(buf, '\x1f');
    if (act)
        *act++ = 0;
    if (!lm_keys_parse_combo(buf, &mods, &code) || !lm_keys_combo_text(mods, code, canon, sizeof(canon)))
        return false;
    const struct lm_keybind *builtin = lm_keys_builtin(code, mods);
    if (kid == K_KEYS_DEL) {
        if (act || !lm_keys_find(&srv->config.keys, code, mods))
            return false;
        return keys_write(srv, canon, builtin ? "none" : NULL);          /* a built-in one is switched off, an own one dropped */
    }
    /* K_KEYS_SET: "combo<US>action [arg]" */
    if (!act || !lm_keys_valid(canon, act))
        return false;
    if (builtin) {                                                       /* the same as the built-in one: just drop our line */
        char bt[LM_KEYBIND_ARG + 40];
        lm_keys_action_text(builtin, bt, sizeof(bt));
        if (!strcmp(bt, act))
            return keys_write(srv, canon, NULL) && true;
    }
    return keys_write(srv, canon, act);
}

/* ---- the [theme] section, edited from the panel's Colours page -------------------------------------------------------------- */

static bool theme_same(const char *existing, void *ud) { return !strcmp(existing, ud); }   /* [theme] names are exact */

static bool theme_op(struct lm_server *srv, int kid, const char *val)
{
    const char *path = srv->config.conf_path;
    if (!path || !*path || !strcmp(path, "none") || !srv->reload_config)
        return false;
    bool ok;
    if (kid == K_THEME_USE) {                                   /* switch the theme: the section keeps only its name (the single colours went with the old theme) */
        char names[24][LM_THEME_NAME_MAX];
        int cnt = lm_theme_list(names, NULL, 24);
        bool known = false;
        for (int i = 0; i < cnt; i++)
            known = known || !strcasecmp(names[i], val);
        if (!known)
            return false;
        if (!strcasecmp(val, "colorful"))
            ok = lm_confedit_set_section(path, "theme", NULL);          /* the default needs no line at all */
        else {
            char body[64];
            snprintf(body, sizeof(body), "name = %s\n", val);
            ok = lm_confedit_set_section(path, "theme", body);
        }
    } else if (kid == K_THEME_RESET) {
        if (strcmp(val, "1"))
            return false;
        ok = lm_confedit_set_section(path, "theme", NULL);
    } else {
        char buf[96];
        if (strlen(val) >= sizeof(buf))
            return false;
        strcpy(buf, val);
        char *hex = strchr(buf, '\x1f');
        if (hex)
            *hex++ = 0;
        const char *nm;
        uint32_t cur, def = 0;
        bool known = false;
        for (int i = 0; lm_theme_entry(i, &nm, &cur, &def); i++)
            if (!strcmp(nm, buf)) {
                known = true;
                break;
            }
        if (!known)
            return false;
        if (kid == K_THEME_DEL) {
            if (hex)
                return false;
            ok = lm_confedit_set_key(path, "theme", theme_same, buf, buf, NULL);
        } else {
            uint32_t c;
            if (!hex || !lm_theme_parse_color(hex, &c))
                return false;
            char text[16];
            snprintf(text, sizeof(text), "#%06x", (unsigned)c);
            ok = lm_confedit_set_key(path, "theme", theme_same, buf, buf, c == def ? NULL : text);   /* the built-in colour needs no line */
        }
    }
    if (ok)
        srv->reload_config(srv, true);
    return ok;
}

/* ---- the size sections, edited from the panel's Sizes page ------------------------------------------------------------------- */

static bool metrics_same(const char *existing, void *ud) { return !strcmp(existing, ud); }

static bool metrics_op(struct lm_server *srv, int kid, const char *val)
{
    const char *path = srv->config.conf_path;
    if (!path || !*path || !strcmp(path, "none") || !srv->reload_config)
        return false;
    bool ok = true;
    if (kid == K_METRICS_RESET) {
        if (strcmp(val, "1"))
            return false;
        static const char *const sections[4] = { "windows", "panel", "gadgets", "notifications" };
        for (int i = 0; i < 4; i++)
            ok = lm_confedit_set_section(path, sections[i], NULL) && ok;
    } else {                                    /* K_METRICS_SET: "section.name<US>value" */
        char buf[96];
        if (strlen(val) >= sizeof(buf))
            return false;
        strcpy(buf, val);
        char *num = strchr(buf, '\x1f'), *dot = strchr(buf, '.');
        if (!num || !dot || dot > num)
            return false;
        *num++ = 0;
        *dot++ = 0;
        char *end;
        long v = strtol(num, &end, 10);
        if (!*num || *end)
            return false;
        const char *sec, *nm;
        int cur, def = 0, lo, hi;
        bool known = false;
        for (int i = 0; lm_metrics_entry(i, &sec, &nm, &cur, &def, &lo, &hi); i++)
            if (!strcmp(sec, buf) && !strcmp(nm, dot)) {
                known = v >= lo && v <= hi;
                break;
            }
        if (!known)
            return false;
        char text[16];
        snprintf(text, sizeof(text), "%ld", v);
        ok = lm_confedit_set_key(path, buf, metrics_same, dot, dot, v == def ? NULL : text);    /* the built-in size needs no line */
    }
    if (ok)
        srv->reload_config(srv, true);
    return ok;
}

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
    case K_SAVER:
    case K_SAVER_MINUTES:
    case K_SAVER_LOCK:
    case K_AUDIO_PROFILE_KEEP:
        return cfg_set(cfg, key, val, NULL, 0) >= 0;
    case K_NIGHT_LIGHT:                                 /* the display fades to the new state */
    case K_NIGHT_TEMP:
    case K_NIGHT_FROM:
    case K_NIGHT_TO:
        if (cfg_set(cfg, key, val, NULL, 0) < 0)
            return false;
        lm_night_changed(srv->night);
        return true;
    case K_OSK_AVOID:                                   /* the windows move out of the keyboard's way (or back) at once */
        if (cfg_set(cfg, key, val, NULL, 0) < 0)
            return false;
        if (srv->wm)
            lm_wm_osk_layout_changed(srv->wm);
        return true;
    case K_CLOCK_HOURS:                                 /* the panel's clock is written again at once */
    case K_CLOCK_SECONDS:
    case K_CLOCK_DATE:
    case K_CLOCK_WEEKDAY:
    case K_DATE_FORMAT:
        if (cfg_set(cfg, key, val, NULL, 0) < 0)
            return false;
        if (srv->wm)
            lm_wm_clock_changed(srv->wm);
        return true;
    case K_CAL_ENABLED:
    case K_CAL_WEEK_START:
    case K_CAL_WEEKS:
        return cfg_set(cfg, key, val, NULL, 0) >= 0;
    case K_SAVER_NOW:                                   /* "1": start the screensaver now (a preview), "0": end it */
        if (!srv->saver || !parse_bool(val, &b)) return false;
        if (b)
            return lm_saver_start(srv->saver);
        lm_saver_stop(srv->saver);
        return true;
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
        {                                                           /* remember it by name: the ids change with PipeWire's next start */
            char keep[sizeof(cfg->audio_profile)];
            if (lm_audio_profile_key(val, keep, sizeof(keep))) {
                snprintf(cfg->audio_profile, sizeof(cfg->audio_profile), "%s", keep);
                save_later(s);
            }
        }
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
    case K_DND: {
        if (!parse_bool(val, &b))
            return false;
        bool was = cfg->dnd;
        cfg->dnd = b;
        if (srv->wm && b != was) {                      /* the answer is a card even while it is on: it says what happened */
            int unread = lm_notelog_unread(srv->notelog);
            if (b) {
                lm_wm_notify_now(srv->wm, "Do not disturb", "On: notifications are kept, not shown. Super+Ctrl+N lists them.", 3500);
            } else if (unread > 0) {
                char msg[96];
                snprintf(msg, sizeof(msg), "Off. %d notification%s came meanwhile (Super+Ctrl+N).", unread, unread == 1 ? "" : "s");
                lm_wm_notify_now(srv->wm, "Do not disturb", msg, 6000);
            } else {
                lm_wm_notify_now(srv->wm, "Do not disturb", "Off", 2500);
            }
        }
        return true;
    }
    case K_NOTES_DEL:
        return parse_int(val, 1, 1000000000, &n) && lm_notelog_remove(srv->notelog, n);
    case K_NOTES_CLEAR:
        lm_notelog_clear(srv->notelog);
        return true;
    case K_STARTUP_SOUND:
        return cfg_set(cfg, key, val, NULL, 0) >= 0;
    case K_LAUNCHER_DEFAULTS: {                         /* "1": add the built-in launcher buttons the file lacks */
        if (!parse_bool(val, &b))
            return false;
        if (!b)
            return true;
        int added = lm_settings_launcher_add_defaults(srv);
        if (added < 0)
            return false;
        if (srv->wm) {
            char msg[96];
            if (added)
                snprintf(msg, sizeof(msg), "%d MellaGUI program%s added to the launcher", added, added == 1 ? "" : "s");
            else
                snprintf(msg, sizeof(msg), "The launcher has all of MellaGUI's programs already");
            lm_wm_notify(srv->wm, "Launcher", msg, 5000);
        }
        return true;
    }
    case K_STARTUP_SOUND_PLAY:                          /* "1": play the start-up chime now (the Sound page's button); "wake": wake the sink (tests) */
        if (!strcmp(val, "wake")) {
            lm_sound_wake();
            return true;
        }
        if (!parse_bool(val, &b))
            return false;
        if (b)
            lm_sound_startup();
        return true;
    case K_CLIP_PNG:                                    /* a PNG file (the screenshot tool's) becomes the clipboard selection "image/png" */
        return clip_png_file(srv, val);
    case K_NOTES_READ:
        lm_notelog_mark_read(srv->notelog);
        return true;
    case K_LOCK_NOW:
        return parse_bool(val, &b) && (!b || lm_lock_now(srv->lock));
    case K_DOCK_PUT: case K_DOCK_DEL: case K_DOCK_MOVE: case K_DOCK_RESET: case K_DOCK_SET:
        return dock_op(srv, k, val);
    case K_KEYS_SET: case K_KEYS_DEL: case K_KEYS_RESET:
        return keys_op(s, k, val);
    case K_THEME_SET: case K_THEME_DEL: case K_THEME_RESET: case K_THEME_USE:
        return theme_op(srv, k, val);
    case K_METRICS_SET: case K_METRICS_RESET:
        return metrics_op(srv, k, val);
    case K_KEYS_PAGE:
        if (!parse_int(val, 0, 1000, &n))
            return false;
        s->keys_page = n;
        return true;
    case K_KEYS_CAPTURE:
        if (!srv->wm || !parse_bool(val, &b))
            return false;
        lm_wm_key_capture(srv->wm, b);
        if (s->capture_timer)
            lm_timer_arm_ms(s->capture_timer, b ? 15000 : 0, 0);
        return true;
    case K_CONFIG_RELOAD:
        return parse_bool(val, &b) && (!b || (srv->reload_config && srv->reload_config(srv, false) >= 0));
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

bool lm_settings_client_allowed(struct lm_server *srv, const struct lm_client *c)
{
    int mode = srv->config.settings_access;
    if (mode < 0)
        mode = !strcmp(srv->config.backend, "headless") ? 1 : 0;
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
    s->profile_timer = lm_loop_add_timer(srv->loop, profile_cb, s);
    if (s->profile_timer && srv->config.audio_profile[0])
        lm_timer_arm_ms(s->profile_timer, 1500, 0);
    s->capture_timer = lm_loop_add_timer(srv->loop, capture_cb, s);
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
    if (s->profile_timer)
        lm_timer_destroy(s->profile_timer);
    if (s->capture_timer)
        lm_timer_destroy(s->capture_timer);
    free(s);
}
