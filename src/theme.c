/*
 * Colour theme. See theme.h.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "conf.h"
#include "log.h"
#include "metrics.h"
#include "theme.h"

struct lm_theme lm_theme = {
#define X(name, field, def) .field = def,
    LM_THEME_COLORS(X)
#undef X
};

void lm_theme_reset(void)
{
    static const struct lm_theme defaults = {
#define X(name, field, def) .field = def,
        LM_THEME_COLORS(X)
#undef X
    };
    lm_theme = defaults;
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

bool lm_theme_entry(int i, const char **name, uint32_t *value, uint32_t *builtin)
{
    int k = 0;
#define X(n, field, def) if (k++ == i) { *name = n; *value = lm_theme.field; *builtin = def; return true; }
    LM_THEME_COLORS(X)
#undef X
    return false;
}

bool lm_theme_set(const char *name, const char *value)
{
    uint32_t c;
    if (!parse_color(value, &c))
        return false;
#define X(n, field, def) if (!strcmp(name, n)) { lm_theme.field = c; return true; }
    LM_THEME_COLORS(X)
#undef X
    return false;
}

bool lm_theme_conf(void *ud, const char *section, const char *key, const char *value)
{
    (void)ud;
    return !strcmp(section, "theme") && lm_theme_set(key, value);
}

/* Programs only care about [theme] and [windows] (scrollbar-width); the other sections are the compositor's business and must not be reported. */
static bool client_line(void *ud, const char *section, const char *key, const char *value)
{
    if (!strcmp(section, "windows")) {
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
    if (!*path || !strcmp(path, "none"))
        return;
    lm_conf_load(path, client_line, NULL);
}

const char *lm_theme_default_conf(void)
{
    static char text[4096];
    if (!text[0]) {
        size_t n = (size_t)snprintf(text, sizeof(text), "[theme]\n");
#define X(name, field, def) n += (size_t)snprintf(text + n, sizeof(text) - n, "%-20s = #%06x\n", name, (unsigned)def);
        LM_THEME_COLORS(X)
#undef X
    }
    return text;
}
