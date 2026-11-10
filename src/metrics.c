/*
 * Shell sizes. See metrics.h.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "metrics.h"

struct lm_metrics lm_metrics = {
#define X(sec, name, field, def, lo, hi, what) .field = def,
    LM_METRICS(X)
#undef X
};

void lm_metrics_reset(void)
{
    static const struct lm_metrics defaults = {
#define X(sec, name, field, def, lo, hi, what) .field = def,
        LM_METRICS(X)
#undef X
    };
    lm_metrics = defaults;
}

int lm_metrics_count(void)
{
    int n = 0;
#define X(sec, name, field, def, lo, hi, what) n++;
    LM_METRICS(X)
#undef X
    return n;
}

bool lm_metrics_entry(int i, const char **section, const char **name, int *value, int *builtin, int *lo_, int *hi_)
{
    int k = 0;
#define X(sec, nm, field, def, lo, hi, what) \
    if (k++ == i) { *section = sec; *name = nm; *value = lm_metrics.field; *builtin = def; *lo_ = lo; *hi_ = hi; return true; }
    LM_METRICS(X)
#undef X
    return false;
}

const char *lm_metrics_what(const char *section, const char *name)
{
#define X(sec, nm, field, def, lo, hi, what) \
    if (!strcmp(section, sec) && !strcmp(name, nm)) \
        return what;
    LM_METRICS(X)
#undef X
    return NULL;
}

bool lm_metrics_set(const char *section, const char *name, int value)
{
#define X(sec, nm, field, def, lo, hi, what) \
    if (!strcmp(section, sec) && !strcmp(name, nm)) { \
        if (value < (lo) || value > (hi)) \
            return false; \
        lm_metrics.field = value; \
        return true; \
    }
    LM_METRICS(X)
#undef X
    return false;
}

bool lm_metrics_section(const char *s)
{
    return !strcmp(s, "windows") || !strcmp(s, "panel") || !strcmp(s, "gadgets") || !strcmp(s, "notifications");
}

bool lm_metrics_conf(void *ud, const char *section, const char *key, const char *value)
{
    (void)ud;
    char *end;
    errno = 0;
    long n = strtol(value, &end, 10);
    if (errno || end == value || *end)
        return false;
#define X(sec, name, field, def, lo, hi, what) \
    if (!strcmp(section, sec) && !strcmp(key, name)) { \
        if (n < (lo) || n > (hi)) \
            return false; \
        lm_metrics.field = (int)n; \
        return true; \
    }
    LM_METRICS(X)
#undef X
    return false;
}

void lm_metrics_fixup(void)
{
    if (lm_metrics.button_size > lm_metrics.titlebar_height - 2)
        lm_metrics.button_size = lm_metrics.titlebar_height - 2;
}

const char *lm_metrics_default_conf(void)
{
    static char text[8192];
    if (!text[0]) {
        size_t n = 0;
        const char *last = "";
#define X(sec, name, field, def, lo, hi, what) \
        if (strcmp(last, sec)) { \
            n += (size_t)snprintf(text + n, sizeof(text) - n, "%s[%s]\n", *last ? "\n" : "", sec); \
            last = sec; \
        } \
        n += (size_t)snprintf(text + n, sizeof(text) - n, "# %s (%d..%d)\n%s = %d\n", what, lo, hi, name, def);
        LM_METRICS(X)
#undef X
    }
    return text;
}
