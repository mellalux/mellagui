/*
 * Colour theme. See theme.h.
 */
#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "conf.h"
#include "log.h"
#include "metrics.h"
#include "theme.h"

#define NCOLORS ((int)(sizeof(struct lm_theme) / sizeof(uint32_t)))

static const struct lm_theme builtin = {
#define X(name, field, def) .field = def,
    LM_THEME_COLORS(X)
#undef X
};

struct lm_theme lm_theme = {
#define X(name, field, def) .field = def,
    LM_THEME_COLORS(X)
#undef X
};

/*
 * A theme is a name and a few colours on top of the built-in ("colorful") palette: `base` is the palette of the active theme,
 * `lm_theme` is base with the colours written one by one in [theme] on top (`over` marks them), so the order of the lines in the
 * file does not matter. Built-in presets are below; any other name is a file themes/NAME.conf next to mellagui.conf.
 */
static struct lm_theme base = {                 /* the active theme's palette with the button colours tinted */
#define X(name, field, def) .field = def,
    LM_THEME_COLORS(X)
#undef X
};
static struct lm_theme raw = {                  /* ... before the tint */
#define X(name, field, def) .field = def,
    LM_THEME_COLORS(X)
#undef X
};
static int button_mode;                         /* 0 colourful, 1 accent, 2 grey */
static const char *const button_modes[3] = { "colourful", "accent", "grey" };
static bool over[160];
static uint32_t generation = 1;                /* bumped by every change of the palette (the GL renderer caches what it painted) */

uint32_t lm_theme_generation(void) { return generation; }
static char theme_name[LM_THEME_NAME_MAX] = "colorful";
static char conf_dir[PATH_MAX];                 /* the directory of mellagui.conf ("" = unknown); themes live in conf_dir/themes */

static const struct { const char *name, *desc, *colors; } presets[] = {
    { "colorful", "The built-in look: deep blue with bright accents.", "" },
    { "graphite", "Neutral dark grey, a calmer accent.",
      "wallpaper-top=#202124 wallpaper-bottom=#3c4043 frame=#4a4d52 frame-idle=#2b2d30 content=#141517 title-text-idle=#9aa0a6 "
      "title-button-hover=#6b6f75 title-button-pressed=#3a3c40 background=#1b1c1e field=#121314 card=#212224 border=#44474b "
      "button=#3a3d41 button-hover=#4d5156 button-down=#62666c button-alt=#45484d bar=#2e3033 accent=#9bb7d4 text=#e8eaed "
      "text-soft=#dadce0 text-dim=#80868b muted=#a0a4a8 "
      "lock-box=#232528 lock-box-blocked=#2b2121 panel-line=#2e3033 help-card=#1f2023 bar-deep=#17181a viewer-button=#2a2c2f "
      "viewer-button-hover=#3a3d41 viewer-button-down=#4d5156 divider=#33353a control=#3a3d41 control-hot=#4d5156 control-idle=#4a4d52 "
      "gutter=#161718 current-line=#1f2022 terminal-button-on=#62666c menu-border=#5a5d62 menu-current=#9bb7d4 "
      "plate-dark=#2a2c2f icon-terminal=#3a3d41 icon-settings=#80868b icon-generic=#80868b tile-other=#50545a" },
    { "light", "Light windows on a pale blue desktop.",
      "wallpaper-top=#dfe7f3 wallpaper-bottom=#b9cde3 frame=#aebdd6 frame-idle=#d6dce8 content=#ffffff title-text=#1c2433 "
      "title-text-idle=#5a6478 title-button-hover=#93a6c9 title-button-pressed=#7b90b8 title-button-close=#d9534f "
      "background=#eef1f6 field=#ffffff card=#f7f9fc border=#b8c2d4 button=#d3dcec button-hover=#bfcce4 button-down=#a8b9d8 "
      "button-alt=#c4d0e6 bar=#dfe5f0 accent=#2f6fe0 text=#1c2433 text-soft=#2a3446 text-dim=#6b7587 muted=#586274 "
      "danger=#d9534f alert=#c0392b warn=#b8730f ok=#2b9a5f caution=#b8730f error=#c0392b "
      "text-bright=#0c1220 ink=#1c2433 task-text=#2a3446 text-shadow=#ffffff veil=#000000 glass=#ffffff glass-text=#1c2433 glass-hint=#2a3446 "
      "glass-error=#b83b2b glass-shadow=#ffffff key-modifier=#7d8aa5 lock-box=#e2e7f0 lock-box-blocked=#f2d8d4 panel-line=#c6cfdf "
      "sticky-hot=#2f6fe0 help-card=#eef1f6 bar-deep=#dfe5f0 bar-text=#2a3446 bar-text-bright=#1c2433 viewer-button=#d3dcec "
      "viewer-button-hover=#bfcce4 viewer-button-down=#a8b9d8 divider=#b8c2d4 control=#c9d3e6 control-hot=#b3c1db "
      "control-idle=#d8deea gutter=#e4e9f2 current-line=#e8edf6 terminal-button-on=#a8b9d8 menu-border=#9fb0cc menu-current=#2f6fe0 "
      "plate-light=#ffffff plate-dark=#d5dbe8 progress=#8ccaa0 picture=#8a52c8 tile-other=#aab4c6 calc-key=#b7c3da calc-key-lit=#9fb0cc" },
    { "contrast", "Black and white with a yellow accent, for the best readability.",
      "wallpaper-top=#000000 wallpaper-bottom=#101010 frame=#0a2a6b frame-idle=#202020 content=#000000 title-text-idle=#c8c8c8 "
      "title-button-hover=#2b57c4 title-button-pressed=#173a8f title-button-close=#d11111 background=#000000 field=#000000 "
      "card=#0a0a0a border=#ffffff button=#1a1a1a button-hover=#333333 button-down=#4d4d4d button-alt=#262626 bar=#141414 "
      "accent=#ffd400 text=#ffffff text-soft=#ffffff text-dim=#b0b0b0 muted=#d0d0d0 danger=#ff3b30 alert=#ff6b60 warn=#ffd400 "
      "ok=#3be08a caution=#ffb000 error=#ff6b60 "
      "ink=#000000 task-text=#ffffff lock-box=#101010 lock-box-blocked=#300000 panel-line=#ffffff sticky-hot=#ffd400 help-card=#000000 "
      "bar-deep=#000000 bar-text=#ffffff bar-text-bright=#ffffff viewer-button=#1a1a1a viewer-button-hover=#333333 "
      "viewer-button-down=#4d4d4d divider=#ffffff control=#333333 control-hot=#4d4d4d control-idle=#262626 gutter=#000000 "
      "current-line=#1a1a1a terminal-button-on=#4d4d4d menu-border=#ffffff menu-current=#ffd400 plate-dark=#000000 "
      "progress=#007a3d picture=#e0a0ff calc-key=#333333 calc-key-lit=#4d4d4d" },
};
#define NPRESETS ((int)(sizeof(presets) / sizeof(presets[0])))

/* Index of a colour name in the palette (the order of LM_THEME_COLORS), -1 = unknown. */
static int color_index(const char *name)
{
    int k = 0;
#define X(n, field, def) if (!strcmp(name, n)) return k; k++;
    LM_THEME_COLORS(X)
#undef X
    return -1;
}

static uint32_t *color_slot(struct lm_theme *t, int idx) { return (uint32_t *)t + idx; }

/* ---- the tint of the button colours ---- */

static void rgb_to_hsl(uint32_t c, float *h, float *s, float *l)
{
    float r = (float)((c >> 16) & 255) / 255.0f, g = (float)((c >> 8) & 255) / 255.0f, b = (float)(c & 255) / 255.0f;
    float mx = r > g ? (r > b ? r : b) : (g > b ? g : b), mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    *l = (mx + mn) / 2.0f;
    float d = mx - mn;
    if (d < 1e-6f) {
        *h = *s = 0.0f;
        return;
    }
    *s = *l > 0.5f ? d / (2.0f - mx - mn) : d / (mx + mn);
    if (mx == r)
        *h = (g - b) / d + (g < b ? 6.0f : 0.0f);
    else if (mx == g)
        *h = (b - r) / d + 2.0f;
    else
        *h = (r - g) / d + 4.0f;
    *h /= 6.0f;
}

static float hue_channel(float p, float q, float t)
{
    if (t < 0.0f) t += 1.0f;
    if (t > 1.0f) t -= 1.0f;
    if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
    if (t < 0.5f) return q;
    if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
    return p;
}

static uint32_t hsl_to_rgb(float h, float s, float l)
{
    float r, g, b;
    if (s <= 0.0f) {
        r = g = b = l;
    } else {
        float q = l < 0.5f ? l * (1.0f + s) : l + s - l * s, p = 2.0f * l - q;
        r = hue_channel(p, q, h + 1.0f / 3.0f);
        g = hue_channel(p, q, h);
        b = hue_channel(p, q, h - 1.0f / 3.0f);
    }
    uint32_t out = 0;
    float ch[3] = { r, g, b };
    for (int i = 0; i < 3; i++) {
        float v = ch[i] < 0.0f ? 0.0f : ch[i] > 1.0f ? 1.0f : ch[i];
        out = (out << 8) | (uint32_t)(v * 255.0f + 0.5f);
    }
    return out;
}

/* Is palette entry `idx` a button colour (hue-*, icon-*, btn-*)? */
static bool is_button_colour(int idx)
{
    int k = 0;
#define X(n, field, def) if (k++ == idx) return !strncmp(n, "hue-", 4) || !strncmp(n, "icon-", 5) || !strncmp(n, "btn-", 4);
    LM_THEME_COLORS(X)
#undef X
    return false;
}

/* base = raw with the button colours tinted; what the single colours of [theme] do not cover follows into lm_theme. */
static void rebase(void)
{
    base = raw;
    if (button_mode != 0) {
        int ai = color_index("button-accent");
        uint32_t acc = over[ai] ? *color_slot(&lm_theme, ai) : *color_slot(&raw, ai);
        float ah, as, al;
        rgb_to_hsl(acc, &ah, &as, &al);
        for (int i = 0; i < NCOLORS; i++) {
            if (!is_button_colour(i))
                continue;
            float h, s, l;
            uint32_t *c = color_slot(&base, i);
            rgb_to_hsl(*c, &h, &s, &l);
            *c = hsl_to_rgb(ah, button_mode == 1 ? as : 0.0f, l);
        }
    }
    for (int i = 0; i < NCOLORS; i++)
        if (!over[i])
            *color_slot(&lm_theme, i) = *color_slot(&base, i);
    generation++;
}

const char *lm_theme_buttons(void) { return button_modes[button_mode]; }

bool lm_theme_buttons_set(const char *mode)
{
    for (int i = 0; i < 3; i++)
        if (!strcasecmp(mode, button_modes[i])) {
            button_mode = i;
            rebase();
            return true;
        }
    return false;
}

void lm_theme_reset(void)
{
    generation++;
    lm_theme = builtin;
    raw = builtin;
    base = builtin;
    button_mode = 0;
    memset(over, 0, sizeof(over));
    snprintf(theme_name, sizeof(theme_name), "colorful");
}

static bool parse_color(const char *v, uint32_t *out)
{
    if (*v == '#')
        v++;
    else if (v[0] == '0' && (v[1] == 'x' || v[1] == 'X'))
        v += 2;
    size_t n = strlen(v);
    if (n != 6 && n != 3)
        return false;
    uint32_t c = 0;
    for (size_t i = 0; i < n; i++) {
        if (!isxdigit((unsigned char)v[i]))
            return false;
        uint32_t d = (uint32_t)(isdigit((unsigned char)v[i]) ? v[i] - '0' : tolower((unsigned char)v[i]) - 'a' + 10);
        c = n == 6 ? (c << 4) | d : (c << 8) | (d << 4) | d;       /* #rgb = #rrggbb */
    }
    *out = c;
    return true;
}

bool lm_theme_parse_color(const char *text, uint32_t *out) { return parse_color(text, out); }

int lm_theme_count(void)
{
    int n = 0;
#define X(name, field, def) n++;
    LM_THEME_COLORS(X)
#undef X
    return n;
}

bool lm_theme_entry(int i, const char **name, uint32_t *value, uint32_t *builtin_value)
{
    int k = 0;
#define X(n, field, def) if (k++ == i) { *name = n; *value = lm_theme.field; *builtin_value = base.field; return true; }
    LM_THEME_COLORS(X)
#undef X
    return false;
}

bool lm_theme_set(const char *name, const char *value)
{
    uint32_t c;
    int i = color_index(name);
    if (i < 0 || !parse_color(value, &c))
        return false;
    *color_slot(&lm_theme, i) = c;
    over[i] = true;
    generation++;
    if (i == color_index("button-accent"))
        rebase();                                               /* the tint follows the accent */
    return true;
}

const char *lm_theme_name(void) { return theme_name; }

void lm_theme_set_conf_path(const char *path)
{
    conf_dir[0] = 0;
    const char *e = getenv("MELLAGUI_THEMES");              /* tests: a folder of theme files */
    if (e && *e) {
        snprintf(conf_dir, sizeof(conf_dir), "%s", e);
        return;
    }
    if (!path || !*path || !strcmp(path, "none"))
        return;
    const char *slash = strrchr(path, '/');
    if (slash)
        snprintf(conf_dir, sizeof(conf_dir), "%.*s/themes", (int)(slash - path), path);
    else
        snprintf(conf_dir, sizeof(conf_dir), "themes");
}

static bool name_ok(const char *n)
{
    size_t len = strlen(n);
    if (len == 0 || len >= LM_THEME_NAME_MAX)
        return false;
    for (size_t i = 0; i < len; i++)
        if (!isalnum((unsigned char)n[i]) && n[i] != '-' && n[i] != '_')
            return false;
    return true;
}

static int preset_index(const char *name)
{
    for (int i = 0; i < NPRESETS; i++)
        if (!strcasecmp(presets[i].name, name))
            return i;
    return -1;
}

/* "name=#rrggbb name=#rrggbb ..." onto `t`. */
static void apply_colors(struct lm_theme *t, const char *text)
{
    char buf[2048];
    snprintf(buf, sizeof(buf), "%s", text);
    for (char *tok = strtok(buf, " "); tok; tok = strtok(NULL, " ")) {
        char *eq = strchr(tok, '=');
        uint32_t c;
        if (!eq)
            continue;
        *eq = 0;
        int i = color_index(tok);
        if (i >= 0 && parse_color(eq + 1, &c))
            *color_slot(t, i) = c;
    }
}

struct file_theme { struct lm_theme *t; };

static bool theme_file_line(void *ud, const char *section, const char *key, const char *value)
{
    struct file_theme *f = ud;
    if (*section && strcmp(section, "theme"))
        return false;
    if (!strcmp(key, "base")) {                                 /* start from a built-in preset (default: colorful) */
        int p = preset_index(value);
        if (p < 0)
            return false;
        *f->t = builtin;
        apply_colors(f->t, presets[p].colors);
        return true;
    }
    int i = color_index(key);
    uint32_t c;
    if (i < 0 || !parse_color(value, &c))
        return false;
    *color_slot(f->t, i) = c;
    return true;
}

/* The palette of theme `name` (a preset, else the file themes/NAME.conf). False when there is none. */
static bool theme_palette(const char *name, struct lm_theme *out)
{
    *out = builtin;
    int p = preset_index(name);
    if (p >= 0) {
        apply_colors(out, presets[p].colors);
        return true;
    }
    if (!name_ok(name) || !conf_dir[0])
        return false;
    char path[PATH_MAX + 64];
    snprintf(path, sizeof(path), "%s/%s.conf", conf_dir, name);
    struct file_theme f = { out };
    return lm_conf_load(path, theme_file_line, &f) >= 0;
}

bool lm_theme_use(const char *name)
{
    struct lm_theme nb;
    if (!name || !theme_palette(name, &nb))
        return false;
    int p = preset_index(name);
    snprintf(theme_name, sizeof(theme_name), "%s", p >= 0 ? presets[p].name : name);
    raw = nb;
    rebase();                                                   /* (single colours of [theme] stay on top of the theme) */
    return true;
}

bool lm_theme_conf(void *ud, const char *section, const char *key, const char *value)
{
    (void)ud;
    if (strcmp(section, "theme"))
        return false;
    if (!strcmp(key, "name"))
        return lm_theme_use(value);
    if (!strcmp(key, "buttons"))
        return lm_theme_buttons_set(value);
    return lm_theme_set(key, value);
}

int lm_theme_list(char names[][LM_THEME_NAME_MAX], char descs[][80], int max)
{
    int n = 0;
    for (int i = 0; i < NPRESETS && n < max; i++, n++) {
        snprintf(names[n], LM_THEME_NAME_MAX, "%s", presets[i].name);
        if (descs)
            snprintf(descs[n], 80, "%s", presets[i].desc);
    }
    DIR *d = conf_dir[0] ? opendir(conf_dir) : NULL;
    if (!d)
        return n;
    char found[32][LM_THEME_NAME_MAX];
    int nf = 0;
    struct dirent *e;
    while ((e = readdir(d)) && nf < 32) {
        size_t len = strlen(e->d_name);
        if (len < 6 || strcmp(e->d_name + len - 5, ".conf"))
            continue;
        char nm[LM_THEME_NAME_MAX];
        if (len - 5 >= sizeof(nm))
            continue;
        memcpy(nm, e->d_name, len - 5);
        nm[len - 5] = 0;
        if (!name_ok(nm) || preset_index(nm) >= 0)
            continue;
        snprintf(found[nf++], LM_THEME_NAME_MAX, "%s", nm);
    }
    closedir(d);
    for (int i = 1; i < nf; i++) {                              /* sorted by name */
        char tmp[LM_THEME_NAME_MAX];
        memcpy(tmp, found[i], sizeof(tmp));
        int j = i - 1;
        for (; j >= 0 && strcmp(found[j], tmp) > 0; j--)
            memcpy(found[j + 1], found[j], sizeof(tmp));
        memcpy(found[j + 1], tmp, sizeof(tmp));
    }
    for (int i = 0; i < nf && n < max; i++, n++) {
        snprintf(names[n], LM_THEME_NAME_MAX, "%s", found[i]);
        if (descs)
            snprintf(descs[n], 80, "A theme file.");
    }
    return n;
}

/* Programs only care about [theme] and [windows] (scrollbar-width); the other sections are the compositor's business and must not be reported. */
static bool client_line(void *ud, const char *section, const char *key, const char *value)
{
    if (!strcmp(section, "windows") || !strcmp(section, "gadgets")) {
        lm_metrics_conf(ud, section, key, value);                   /* (a bad value is the compositor's warning, not ours) */
        return true;
    }
    return strcmp(section, "theme") ? true : lm_theme_conf(ud, section, key, value);
}

void lm_theme_load_env(void)
{
    const char *path = getenv("MELLAGUI_CONFIG");
    char buf[4096];
    if (!path) {
        const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
        if (xdg && *xdg)
            snprintf(buf, sizeof(buf), "%s/mellagui/mellagui.conf", xdg);
        else if (home && *home)
            snprintf(buf, sizeof(buf), "%s/.config/mellagui/mellagui.conf", home);
        else
            return;
        path = buf;
    }
    lm_theme_set_conf_path(path);
    if (!*path || !strcmp(path, "none"))
        return;
    lm_conf_load(path, client_line, NULL);
}

const char *lm_theme_default_conf(void)
{
    static char text[8192];
    if (!text[0]) {
        size_t n = (size_t)snprintf(text, sizeof(text), "[theme]\nname                 = colorful\nbuttons              = colourful\n");
#define X(name, field, def) n += (size_t)snprintf(text + n, sizeof(text) - n, "%-20s = #%06x\n", name, (unsigned)def);
        LM_THEME_COLORS(X)
#undef X
    }
    return text;
}
