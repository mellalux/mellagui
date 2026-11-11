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
#include "theme.h"
#include "dockicon.h"
#include "iconfind.h"
#include "launchers.h"
#include "log.h"

struct lm_dock_settings lm_dock = { .enabled = true, .icon_size = 48, .zoom = 160, .gap = 10, .margin = 8, .icon_bg = LM_DOCK_BG_LIGHT };

#define DEFAULT_APPS \
    "[dock-apps]\n" \
    "Launcher   = [launcher] @launcher\n" \
    "Overview   = [overview] @overview\n" \
    "Terminal   = [terminal] @terminal\n" \
    "Files      = [files] mellagui-files\n" \
    "Editor     = [editor] mellagui-edit\n" \
    "Calculator = [calculator] mellagui-calc\n" \
    "Images     = [images] @view\n" \
    "Camera     = [camera] mellagui-cam\n" \
    "Record     = [record] mellagui-record\n" \
    "Processes  = [monitor] mellagui-top\n" \
    "Code       = [theme:vscode|code] ~/VSCode/code " LM_CODE_FLAGS " || /snap/code/current/usr/share/code/code " LM_CODE_FLAGS "\n" \
    "Firefox    = [theme:firefox] " LM_FF_ENV " /snap/bin/firefox || " LM_FF_ENV " /usr/bin/firefox\n" \
    "Settings   = [settings] @settings\n"

/* The launcher's own buttons: every MellaGUI program that the dock does not have. Used while mellagui.conf has no [launcher-apps] at all; a file that has its
 * own list gets the missing ones through the settings (launcher-defaults). The programs of the dock (Terminal, Files, Editor, Calculator, Images, Camera,
 * Record, Processes, Settings and the Overview button) show in the launcher through the dock. */
#define DEFAULT_LAUNCHER \
    "[launcher-apps]\n" \
    "Run           = [run] @run\n" \
    "Disk usage    = [disk] mellagui-du\n" \
    "Display       = [monitor] mellagui-monitor\n" \
    "Shortcuts     = [keyboard] mellagui-keys\n" \
    "Notifications = [bell] mellagui-notes\n" \
    "Bluetooth     = [bluetooth] mellagui-bluetooth\n" \
    "Screenshot    = [shot] mellagui-shot\n" \
    "Overview      = [overview] @overview\n" \
    "Notes         = [note] mellagui-edit --notes\n" \
    "Timer         = [timer] mellagui-timer\n"

#define DEFAULT_SETTINGS \
    "[dock]\n" \
    "# The row of round icons along the bottom edge. autohide = yes shows it only while the pointer touches the bottom edge.\n" \
    "enabled   = yes\n" \
    "autohide  = no\n" \
    "icon-size = 48\n" \
    "zoom      = 160\n" \
    "gap       = 10\n" \
    "margin    = 8\n" \
    "# The plate under the logos of installed programs: light, dark, none or a colour like #ffffff.\n" \
    "icon-background = light\n"

static const char default_text[] = DEFAULT_SETTINGS "\n" DEFAULT_APPS;
const char *lm_dock_default_conf(void) { return default_text; }

/* ---- what the file said ------------------------------------------------------------------------------------------- */

struct raw_item { char name[LM_DOCK_NAME_MAX], icon[LM_DOCK_ICON_MAX], cmd[LM_DOCK_CMD_MAX]; };
struct app_item {
    struct lm_dock_item pub;
    char name[LM_DOCK_NAME_MAX], cmd[LM_DOCK_CMD_MAX], match[64];
    struct lm_image img;
    bool has_img;
};
/* Two lists of buttons: [dock-apps] (the dock) and [launcher-apps] (the launcher dialog's own ones). */
struct applist {
    struct raw_item raw[LM_DOCK_MAX];
    int raw_n;
    struct app_item items[LM_DOCK_MAX];
    int item_n;
};
static struct applist lists[2];
#define DOCK_LIST (&lists[0])
#define LAUNCH_LIST (&lists[1])

bool lm_dock_section(const char *section)
{
    return !strcmp(section, "dock") || !strcmp(section, "dock-apps") || !strcmp(section, "launcher-apps");
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
        if (!strcmp(key, "icon-background")) {
            unsigned rgb;
            char tail;
            if (!strcasecmp(value, "light")) lm_dock.icon_bg = LM_DOCK_BG_LIGHT;
            else if (!strcasecmp(value, "dark")) lm_dock.icon_bg = LM_DOCK_BG_DARK;
            else if (!strcasecmp(value, "none")) lm_dock.icon_bg = LM_DOCK_BG_NONE;
            else if (value[0] == '#' && strlen(value) == 7 && sscanf(value + 1, "%6x%c", &rgb, &tail) == 1) {
                lm_dock.icon_bg = LM_DOCK_BG_COLOUR;
                lm_dock.icon_bg_rgb = rgb & 0xffffffu;
            } else {
                return false;
            }
            return true;
        }
        return false;
    }
    if (!strcmp(section, "dock-apps") || !strcmp(section, "launcher-apps")) {
        struct applist *L = strcmp(section, "dock-apps") ? LAUNCH_LIST : DOCK_LIST;
        if (L->raw_n >= LM_DOCK_MAX) {
            lm_warn("%s: at most %d entries, '%s' ignored", section, LM_DOCK_MAX, key);
            return true;
        }
        struct raw_item *r = &L->raw[L->raw_n];
        if (!*key || strlen(key) >= sizeof(r->name) || !lm_dock_split(value, r->icon, sizeof(r->icon), r->cmd, sizeof(r->cmd)))
            return false;
        strcpy(r->name, key);
        L->raw_n++;
        return true;
    }
    return false;
}

#define BUILTIN_SETTINGS ((struct lm_dock_settings){ .enabled = true, .autohide = false, .icon_size = 48, .zoom = 160, .gap = 10, .margin = 8, .icon_bg = LM_DOCK_BG_LIGHT })

int lm_dock_plate_rgb(void)
{
    switch (lm_dock.icon_bg) {
    case LM_DOCK_BG_LIGHT: return (int)lm_theme.plate_light;
    case LM_DOCK_BG_DARK: return (int)lm_theme.plate_dark;
    case LM_DOCK_BG_COLOUR: return (int)lm_dock.icon_bg_rgb;
    default: return -1;
    }
}

void lm_dock_bg_text(char *out, size_t n)
{
    switch (lm_dock.icon_bg) {
    case LM_DOCK_BG_DARK: snprintf(out, n, "dark"); break;
    case LM_DOCK_BG_NONE: snprintf(out, n, "none"); break;
    case LM_DOCK_BG_COLOUR: snprintf(out, n, "#%06x", (unsigned)lm_dock.icon_bg_rgb); break;
    default: snprintf(out, n, "light"); break;
    }
}

void lm_dock_reset(void)
{
    lm_dock = BUILTIN_SETTINGS;
    lists[0].raw_n = lists[1].raw_n = 0;
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

int lm_dock_raw_count(void) { return DOCK_LIST->raw_n; }
int lm_launchapps_raw_count(void) { return LAUNCH_LIST->raw_n; }

static bool raw_entry(const struct applist *L, int i, const char **name, const char **icon, const char **cmd)
{
    if (i < 0 || i >= L->raw_n)
        return false;
    *name = L->raw[i].name;
    *icon = L->raw[i].icon;
    *cmd = L->raw[i].cmd;
    return true;
}

bool lm_dock_raw_entry(int i, const char **name, const char **icon, const char **cmd) { return raw_entry(DOCK_LIST, i, name, icon, cmd); }
bool lm_launchapps_raw_entry(int i, const char **name, const char **icon, const char **cmd) { return raw_entry(LAUNCH_LIST, i, name, icon, cmd); }

/* ---- the resolved lists ------------------------------------------------------------------------------------------- */

static void free_items(struct applist *L)
{
    for (int i = 0; i < L->item_n; i++)
        if (L->items[i].has_img)
            lm_img_free(&L->items[i].img);
    L->item_n = 0;
}

int lm_dock_count(void) { return DOCK_LIST->item_n; }

const struct lm_dock_item *lm_dock_item(int i)
{
    return i >= 0 && i < DOCK_LIST->item_n ? &DOCK_LIST->items[i].pub : NULL;
}

int lm_launchapps_count(void) { return LAUNCH_LIST->item_n; }

const struct lm_dock_item *lm_launchapps_item(int i)
{
    return i >= 0 && i < LAUNCH_LIST->item_n ? &LAUNCH_LIST->items[i].pub : NULL;
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
/* Scales a picture down to `max` and premultiplies it. *fills = it fills its square (opaque corners): the renderer cuts it to the icon shape when it paints. */
static bool prepare_image(struct lm_image *img, int max, bool *fills)
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
            const float cov = 1.0f;
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
    *fills = round;
    return true;
}

static bool looks_like_path(const char *s)
{
    if (strchr(s, '/') || s[0] == '~' || s[0] == '.')
        return true;
    const char *dot = strrchr(s, '.');
    return dot && (!strcasecmp(dot, ".png") || !strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg") || !strcasecmp(dot, ".ppm"));
}

/* Fills the icon of `pub` (a built-in kind, or a picture loaded into `img`, which `pub->img` then points at) from an icon text: a built-in name, theme:NAME, a bare
 * name of an installed program's icon or the path of a picture. Nothing usable: the built-in icon guessed from `name`. */
void lm_dock_icon_load(struct lm_dock_item *pub, struct lm_image *img, const char *name, const char *spec)
{
    pub->kind = LM_DOCKICON_GENERIC;
    pub->img = NULL;
    pub->plate = false;
    if (!*spec) {
        pub->kind = lm_dockicon_guess(name);
        return;
    }
    int k = lm_dockicon_by_name(spec);
    if (k >= 0) {
        pub->kind = k;
        return;
    }
    char path[PATH_MAX], err[160];
    bool plate = false;
    if (!strncmp(spec, "theme:", 6)) {                                /* the icon the installed program brings along; none: the built-in one */
        if (!lm_icon_find(spec + 6, path, sizeof(path))) {
            pub->kind = lm_dockicon_guess(name);
            return;
        }
        spec = path;
    } else if (!looks_like_path(spec)) {
        if (!lm_icon_find(spec, path, sizeof(path))) {                /* a bare name that is no built-in icon: maybe an installed one */
            lm_warn("dock: '%s': unknown icon '%s'", name, spec);
            pub->kind = lm_dockicon_guess(name);
            return;
        }
        spec = path;
    }
    if (spec != path) {
        if (!strncmp(spec, "~/", 2)) {
            const char *home = getenv("HOME");
            snprintf(path, sizeof(path), "%s%s", home && *home ? home : "", spec + 1);
        } else {
            snprintf(path, sizeof(path), "%s", spec);
        }
    }
    if (lm_img_load_alpha(path, img, err, sizeof(err)) < 0) {
        lm_warn("dock: '%s': icon %s: %s", name, path, err);
        pub->kind = lm_dockicon_guess(name);
        return;
    }
    {                                                                 /* a logo (transparent corners) gets a plate, a picture that fills its square does not */
        const struct lm_image *im = img;                         /* (looked at before prepare_image cuts it to a circle) */
        int a0 = (int)(im->px[0] >> 24), a1 = (int)(im->px[im->w - 1] >> 24), a2 = (int)(im->px[(size_t)(im->h - 1) * (size_t)im->w] >> 24),
            a3 = (int)(im->px[(size_t)im->h * (size_t)im->w - 1] >> 24);
        plate = a0 < 0x80 && a1 < 0x80 && a2 < 0x80 && a3 < 0x80;
    }
    int max = (lm_dock.icon_size * lm_dock.zoom + 99) / 100 + 2;
    bool fills = false;
    if (!prepare_image(img, max, &fills)) {
        lm_img_free(img);
        pub->kind = lm_dockicon_guess(name);
        return;
    }
    pub->img = img;
    pub->plate = plate;
    pub->round = fills && !plate;
}

static void load_icon(struct app_item *it, const char *name, const char *spec)
{
    lm_dock_icon_load(&it->pub, &it->img, name, spec);
    it->has_img = it->pub.img != NULL;
}

static void resolve_list(struct applist *L, bool launcher_only)
{
    free_items(L);
    for (int i = 0; i < L->raw_n; i++) {
        struct app_item *it = &L->items[L->item_n];
        if (launcher_only)                                      /* a name the dock has is the dock's: it shows in the launcher through it */
            for (int d = 0; d < DOCK_LIST->item_n; d++)
                if (!strcasecmp(DOCK_LIST->items[d].name, L->raw[i].name))
                    goto skip;
        if (!lm_launch_exists(L->raw[i].cmd, it->cmd, sizeof(it->cmd))) {
            lm_info("%s: '%s' left out, its program was not found", launcher_only ? "launcher" : "dock", L->raw[i].name);
            continue;
        }
        memset(&it->img, 0, sizeof(it->img));
        it->has_img = false;
        snprintf(it->name, sizeof(it->name), "%.*s", LM_DOCK_NAME_MAX - 1, L->raw[i].name);
        program_key(it->cmd, it->match, sizeof(it->match));
        it->pub.name = it->name;
        it->pub.cmd = it->cmd;
        it->pub.match = it->match;
        it->pub.raw = i;
        it->pub.launcher_only = launcher_only;
        load_icon(it, L->raw[i].name, L->raw[i].icon);
        L->item_n++;
    skip:;
    }
}

bool lm_dock_cmd_is_launcher(const char *cmd)
{
    while (*cmd == ' ' || *cmd == '\t')
        cmd++;
    return !strncmp(cmd, "@launcher", 9) && (cmd[9] == 0 || isspace((unsigned char)cmd[9]));
}

/* The Launcher button cannot be missing from the dock: a file that lacks it gets it in front (in memory; the next change of the buttons writes it). */
static void ensure_launcher(void)
{
    const char *off = getenv("MELLAGUI_DOCK_LAUNCHER");           /* "0": the test harness, whose small docks should hold what the test wrote */
    if (off && !strcmp(off, "0"))
        return;
    struct applist *L = DOCK_LIST;
    for (int i = 0; i < L->raw_n; i++)
        if (lm_dock_cmd_is_launcher(L->raw[i].cmd))
            return;
    if (L->raw_n >= LM_DOCK_MAX)
        L->raw_n = LM_DOCK_MAX - 1;                             /* the last one makes room */
    memmove(&L->raw[1], &L->raw[0], sizeof(L->raw[0]) * (size_t)L->raw_n);
    L->raw_n++;
    struct raw_item *r = &L->raw[0];
    memset(r, 0, sizeof(*r));
    const char *name = "Launcher";
    for (int i = 1; i < L->raw_n; i++)
        if (!strcasecmp(L->raw[i].name, name)) {
            name = "Launchpad";
            break;
        }
    snprintf(r->name, sizeof(r->name), "%s", name);
    snprintf(r->icon, sizeof(r->icon), "launcher");
    snprintf(r->cmd, sizeof(r->cmd), "@launcher");
    lm_info("dock: the Launcher button was missing, it is back in front");
}

/* The built-in launcher buttons as (name, icon, command): parsed once from DEFAULT_LAUNCHER. */
static struct raw_item default_launcher[LM_DOCK_MAX];
static int default_launcher_n = -1;

static bool default_launcher_conf(void *ud, const char *section, const char *key, const char *value)
{
    (void)ud;
    struct raw_item *r = &default_launcher[default_launcher_n];
    if (strcmp(section, "launcher-apps") || default_launcher_n >= LM_DOCK_MAX || !lm_dock_split(value, r->icon, sizeof(r->icon), r->cmd, sizeof(r->cmd)))
        return false;
    snprintf(r->name, sizeof(r->name), "%s", key);
    default_launcher_n++;
    return true;
}

static void load_default_launcher(void)
{
    if (default_launcher_n >= 0)
        return;
    default_launcher_n = 0;
    lm_conf_parse(DEFAULT_LAUNCHER, "<built-in launcher>", default_launcher_conf, NULL);
}

int lm_launchapps_default_count(void)
{
    load_default_launcher();
    return default_launcher_n;
}

bool lm_launchapps_default(int i, const char **name, const char **icon, const char **cmd)
{
    load_default_launcher();
    if (i < 0 || i >= default_launcher_n)
        return false;
    *name = default_launcher[i].name;
    *icon = default_launcher[i].icon;
    *cmd = default_launcher[i].cmd;
    return true;
}

void lm_dock_resolve(void)
{
    if (DOCK_LIST->raw_n == 0)
        lm_conf_parse(DEFAULT_APPS, "<built-in dock>", lm_dock_conf, NULL);
    const char *nodef = getenv("MELLAGUI_LAUNCHER_DEFAULTS");        /* "0": the test harness, whose launcher holds what the test wrote */
    if (LAUNCH_LIST->raw_n == 0 && !(nodef && !strcmp(nodef, "0")))
        lm_conf_parse(DEFAULT_LAUNCHER, "<built-in launcher>", lm_dock_conf, NULL);
    ensure_launcher();
    resolve_list(DOCK_LIST, false);
    resolve_list(LAUNCH_LIST, true);
}
