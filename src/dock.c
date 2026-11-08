/*
 * The dock's configuration and entry list. See dock.h for the contract.
 */
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "conf.h"
#include "dock.h"
#include "dockicon.h"
#include "launchers.h"
#include "log.h"

struct lm_dock_settings lm_dock = { .enabled = true, .icon_size = 48, .zoom = 160, .gap = 10, .margin = 8 };

#define DEFAULT_APPS \
    "[dock-apps]\n" \
    "Terminal   = [terminal] @terminal\n" \
    "Files      = [files] mellagui-files\n" \
    "Editor     = [editor] mellagui-edit\n" \
    "Calculator = [calculator] mellagui-calc\n" \
    "Images     = [images] @view\n" \
    "Code       = [code] ~/VSCode/code " LM_CODE_FLAGS " || /snap/code/current/usr/share/code/code " LM_CODE_FLAGS "\n" \
    "Firefox    = [web] " LM_FF_ENV " /snap/bin/firefox || " LM_FF_ENV " /usr/bin/firefox\n" \
    "Settings   = [settings] @settings\n"

#define DEFAULT_SETTINGS \
    "[dock]\n" \
    "# The row of round icons along the bottom edge. autohide = yes shows it only while the pointer touches the bottom edge.\n" \
    "enabled   = yes\n" \
    "autohide  = no\n" \
    "icon-size = 48\n" \
    "zoom      = 160\n" \
    "gap       = 10\n" \
    "margin    = 8\n"

static const char default_text[] = DEFAULT_SETTINGS "\n" DEFAULT_APPS;
const char *lm_dock_default_conf(void) { return default_text; }

/* ---- what the file said ------------------------------------------------------------------------------------------- */

struct raw_item { char name[LM_DOCK_NAME_MAX], icon[LM_DOCK_ICON_MAX], cmd[LM_DOCK_CMD_MAX]; };
static struct raw_item raw[LM_DOCK_MAX];
static int raw_n;

bool lm_dock_section(const char *section)
{
    return !strcmp(section, "dock") || !strcmp(section, "dock-apps");
}

bool lm_dock_split(const char *value, char *icon, size_t icon_n, char *cmd, size_t cmd_n)
{
    icon[0] = 0;
    while (isspace((unsigned char)*value))
        value++;
    if (*value == '[') {
        const char *close = strchr(value, ']');
        if (!close)
            return false;
        const char *a = value + 1, *b = close;
        while (a < b && isspace((unsigned char)*a))
            a++;
        while (b > a && isspace((unsigned char)b[-1]))
            b--;
        if ((size_t)(b - a) >= icon_n)
            return false;
        memcpy(icon, a, (size_t)(b - a));
        icon[b - a] = 0;
        value = close + 1;
        while (isspace((unsigned char)*value))
            value++;
    }
    if (!*value || strlen(value) >= cmd_n)
        return false;
    strcpy(cmd, value);
    return true;
}

static bool parse_bool(const char *v, bool *out)
{
    static const char *const yes[] = { "yes", "true", "on", "1" }, *const no[] = { "no", "false", "off", "0" };
    for (int i = 0; i < 4; i++) {
        if (!strcasecmp(v, yes[i])) { *out = true; return true; }
        if (!strcasecmp(v, no[i])) { *out = false; return true; }
    }
    return false;
}

static bool parse_int(const char *v, int lo, int hi, int *out)
{
    char *end;
    long n = strtol(v, &end, 10);
    if (end == v || *end || n < lo || n > hi)
        return false;
    *out = (int)n;
    return true;
}

bool lm_dock_conf(void *ud, const char *section, const char *key, const char *value)
{
    (void)ud;
    if (!strcmp(section, "dock")) {
        if (!strcmp(key, "enabled")) return parse_bool(value, &lm_dock.enabled);
        if (!strcmp(key, "autohide")) return parse_bool(value, &lm_dock.autohide);
        if (!strcmp(key, "icon-size")) return parse_int(value, 24, 128, &lm_dock.icon_size);
        if (!strcmp(key, "zoom")) return parse_int(value, 100, 250, &lm_dock.zoom);
        if (!strcmp(key, "gap")) return parse_int(value, 0, 40, &lm_dock.gap);
        if (!strcmp(key, "margin")) return parse_int(value, 0, 100, &lm_dock.margin);
        return false;
    }
    if (!strcmp(section, "dock-apps")) {
        if (raw_n >= LM_DOCK_MAX) {
            lm_warn("dock: at most %d entries, '%s' ignored", LM_DOCK_MAX, key);
            return true;
        }
        struct raw_item *r = &raw[raw_n];
        if (!*key || strlen(key) >= sizeof(r->name) || !lm_dock_split(value, r->icon, sizeof(r->icon), r->cmd, sizeof(r->cmd)))
            return false;
        strcpy(r->name, key);
        raw_n++;
        return true;
    }
    return false;
}

#define BUILTIN_SETTINGS ((struct lm_dock_settings){ .enabled = true, .autohide = false, .icon_size = 48, .zoom = 160, .gap = 10, .margin = 8 })

void lm_dock_reset(void)
{
    lm_dock = BUILTIN_SETTINGS;
    raw_n = 0;
}

bool lm_dock_check(const char *key, const char *value, bool *builtin)
{
    struct lm_dock_settings keep = lm_dock, def = BUILTIN_SETTINGS;
    lm_dock = def;
    bool ok = lm_dock_conf(NULL, "dock", key, value);
    *builtin = ok && !memcmp(&lm_dock, &def, sizeof(def));
    lm_dock = keep;
    return ok;
}

int lm_dock_raw_count(void) { return raw_n; }

bool lm_dock_raw_entry(int i, const char **name, const char **icon, const char **cmd)
{
    if (i < 0 || i >= raw_n)
        return false;
    *name = raw[i].name;
    *icon = raw[i].icon;
    *cmd = raw[i].cmd;
    return true;
}

/* ---- the resolved list -------------------------------------------------------------------------------------------- */

static struct {
    struct lm_dock_item pub;
    char name[LM_DOCK_NAME_MAX], cmd[LM_DOCK_CMD_MAX], match[64];
    struct lm_image img;
    bool has_img;
} items[LM_DOCK_MAX];
static int item_n;

static void free_items(void)
{
    for (int i = 0; i < item_n; i++)
        if (items[i].has_img)
            lm_img_free(&items[i].img);
    item_n = 0;
}

int lm_dock_count(void) { return item_n; }

const struct lm_dock_item *lm_dock_item(int i)
{
    return i >= 0 && i < item_n ? &items[i].pub : NULL;
}

/* The program of a command line, lower case without its directory and a trailing ".sh"/".exe": what a window's app id is compared with.
 * `env` and VAR=value words in front are skipped. */
static void program_key(const char *cmd, char *out, size_t n)
{
    out[0] = 0;
    for (;;) {
        while (isspace((unsigned char)*cmd))
            cmd++;
        char quote = (*cmd == '\'' || *cmd == '"') ? *cmd++ : 0;
        const char *e = cmd;
        while (*e && (quote ? *e != quote : !isspace((unsigned char)*e)))
            e++;
        char word[PATH_MAX];
        size_t len = (size_t)(e - cmd);
        if (len == 0 || len >= sizeof(word))
            return;
        memcpy(word, cmd, len);
        word[len] = 0;
        const char *eq = strchr(word, '=');
        bool assign = !quote && eq && eq != word;
        if (!quote && (!strcmp(word, "env") || assign)) {
            cmd = *e ? e + 1 : e;
            continue;
        }
        const char *base = strrchr(word, '/');
        base = base ? base + 1 : word;
        size_t o = 0;
        for (; *base && o + 1 < n; base++)
            out[o++] = (char)tolower((unsigned char)*base == '.' ? '-' : (unsigned char)*base);
        out[o] = 0;
        return;
    }
}

/* ARGB (not premultiplied) picture -> premultiplied, at most `max` pixels on a side (area average), cut to a circle when it has no
 * transparency in its corners. */
static bool prepare_image(struct lm_image *img, int max)
{
    int w = img->w, h = img->h;
    int s = w > h ? w : h;
    int tw = w, th = h;
    if (s > max) {
        tw = (int)((int64_t)w * max / s);
        th = (int)((int64_t)h * max / s);
        if (tw < 1) tw = 1;
        if (th < 1) th = 1;
    }
    uint32_t *out = malloc((size_t)tw * (size_t)th * 4);
    if (!out)
        return false;
    bool round = (img->px[0] >> 24) == 255 && (img->px[w - 1] >> 24) == 255 &&
                 (img->px[(size_t)(h - 1) * (size_t)w] >> 24) == 255 && (img->px[(size_t)h * (size_t)w - 1] >> 24) == 255;
    for (int y = 0; y < th; y++) {
        int y0 = (int)((int64_t)y * h / th), y1 = (int)((int64_t)(y + 1) * h / th);
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < tw; x++) {
            int x0 = (int)((int64_t)x * w / tw), x1 = (int)((int64_t)(x + 1) * w / tw);
            if (x1 <= x0) x1 = x0 + 1;
            uint64_t a = 0, r = 0, g = 0, b = 0, cnt = 0;
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++) {
                    uint32_t p = img->px[(size_t)yy * (size_t)w + (size_t)xx];
                    uint32_t pa = p >> 24;
                    a += pa;
                    r += ((p >> 16) & 255) * pa;
                    g += ((p >> 8) & 255) * pa;
                    b += (p & 255) * pa;
                    cnt++;
                }
            float cov = 1.0f;
            if (round) {                                /* the circle through the middle of the sides, one pixel soft */
                float dx = ((float)x + 0.5f) / (float)tw * 2.0f - 1.0f, dy = ((float)y + 0.5f) / (float)th * 2.0f - 1.0f;
                float d = (sqrtf(dx * dx + dy * dy) - 1.0f) * (float)(tw < th ? tw : th) * 0.5f;
                cov = d < -0.5f ? 1.0f : d > 0.5f ? 0.0f : 0.5f - d;
            }
            uint64_t aa = cnt ? a / cnt : 0;                    /* mean alpha; colours are alpha weighted, so they premultiply as-is */
            uint32_t pr = cnt ? (uint32_t)(r / cnt / 255) : 0, pg = cnt ? (uint32_t)(g / cnt / 255) : 0, pb = cnt ? (uint32_t)(b / cnt / 255) : 0;
            out[(size_t)y * (size_t)tw + (size_t)x] = ((uint32_t)((float)aa * cov + 0.5f) << 24) | ((uint32_t)((float)pr * cov + 0.5f) << 16) |
                                                      ((uint32_t)((float)pg * cov + 0.5f) << 8) | (uint32_t)((float)pb * cov + 0.5f);
        }
    }
    free(img->px);
    img->px = out;
    img->w = tw;
    img->h = th;
    return true;
}

static bool looks_like_path(const char *s)
{
    if (strchr(s, '/') || s[0] == '~' || s[0] == '.')
        return true;
    const char *dot = strrchr(s, '.');
    return dot && (!strcasecmp(dot, ".png") || !strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg") || !strcasecmp(dot, ".ppm"));
}

/* Fills the icon of items[i] from the icon text of the entry. */
static void load_icon(int i, const char *name, const char *spec)
{
    items[i].pub.kind = LM_DOCKICON_GENERIC;
    items[i].pub.img = NULL;
    if (!*spec) {
        items[i].pub.kind = lm_dockicon_guess(name);
        return;
    }
    int k = lm_dockicon_by_name(spec);
    if (k >= 0) {
        items[i].pub.kind = k;
        return;
    }
    if (!looks_like_path(spec)) {
        lm_warn("dock: '%s': unknown icon '%s'", name, spec);
        items[i].pub.kind = lm_dockicon_guess(name);
        return;
    }
    char path[PATH_MAX], err[160];
    if (!strncmp(spec, "~/", 2)) {
        const char *home = getenv("HOME");
        snprintf(path, sizeof(path), "%s%s", home && *home ? home : "", spec + 1);
    } else {
        snprintf(path, sizeof(path), "%s", spec);
    }
    if (lm_img_load_alpha(path, &items[i].img, err, sizeof(err)) < 0) {
        lm_warn("dock: '%s': icon %s: %s", name, path, err);
        items[i].pub.kind = lm_dockicon_guess(name);
        return;
    }
    int max = (lm_dock.icon_size * lm_dock.zoom + 99) / 100 + 2;
    if (!prepare_image(&items[i].img, max)) {
        lm_img_free(&items[i].img);
        items[i].pub.kind = lm_dockicon_guess(name);
        return;
    }
    items[i].has_img = true;
    items[i].pub.img = &items[i].img;
}

void lm_dock_resolve(void)
{
    free_items();
    if (raw_n == 0)
        lm_conf_parse(DEFAULT_APPS, "<built-in dock>", lm_dock_conf, NULL);
    for (int i = 0; i < raw_n; i++) {
        int k = item_n;
        if (!lm_launch_exists(raw[i].cmd, items[k].cmd, sizeof(items[k].cmd))) {
            lm_info("dock: '%s' left out, its program was not found", raw[i].name);
            continue;
        }
        memset(&items[k].img, 0, sizeof(items[k].img));
        items[k].has_img = false;
        snprintf(items[k].name, sizeof(items[k].name), "%.*s", LM_DOCK_NAME_MAX - 1, raw[i].name);
        program_key(items[k].cmd, items[k].match, sizeof(items[k].match));
        items[k].pub.name = items[k].name;
        items[k].pub.cmd = items[k].cmd;
        items[k].pub.match = items[k].match;
        load_icon(k, raw[i].name, raw[i].icon);
        item_n++;
    }
}
