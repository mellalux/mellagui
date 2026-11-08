/*
 * mellagui.conf reader. See conf.h for the contract.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "conf.h"
#include "log.h"

#define CONF_LINE_MAX 2048
#define CONF_FILE_MAX (256 * 1024)

static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s))
        s++;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1]))
        s[--n] = 0;
    return s;
}

int lm_conf_parse(const char *text, const char *origin, lm_conf_fn fn, void *ud)
{
    char section[64] = "";
    int bad = 0, lineno = 0;
    const char *p = text;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        const char *next = eol ? eol + 1 : p + len;
        lineno++;
        if (len >= CONF_LINE_MAX) {
            lm_warn("%s:%d: line too long, ignored", origin, lineno);
            bad++;
            p = next;
            continue;
        }
        char line[CONF_LINE_MAX];
        memcpy(line, p, len);
        line[len] = 0;
        p = next;

        char *s = trim(line);
        if (!*s || *s == '#' || *s == ';')
            continue;
        if (*s == '[') {
            char *close = strchr(s, ']');
            if (!close || *trim(close + 1) || close - s - 1 >= (long)sizeof(section)) {
                lm_warn("%s:%d: bad section header '%s'", origin, lineno, s);
                bad++;
                section[0] = 0;
                continue;
            }
            *close = 0;
            snprintf(section, sizeof(section), "%s", trim(s + 1));
            for (char *c = section; *c; c++)
                *c = (char)tolower((unsigned char)*c);
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq) {
            lm_warn("%s:%d: expected 'key = value', got '%s'", origin, lineno, s);
            bad++;
            continue;
        }
        *eq = 0;
        char *key = trim(s), *val = trim(eq + 1);
        if (!*key) {
            lm_warn("%s:%d: empty key", origin, lineno);
            bad++;
            continue;
        }
        if (!fn(ud, section, key, val)) {
            lm_warn("%s:%d: unknown or invalid '%s' in [%s]", origin, lineno, key, section);
            bad++;
        }
    }
    return bad;
}

int lm_conf_load(const char *path, lm_conf_fn fn, void *ud)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    char *buf = malloc(CONF_FILE_MAX + 1);
    if (!buf) {
        fclose(f);
        return -1;
    }
    size_t n = fread(buf, 1, CONF_FILE_MAX, f);
    bool too_big = n == CONF_FILE_MAX && fgetc(f) != EOF;
    fclose(f);
    buf[n] = 0;
    if (too_big)
        lm_warn("%s: larger than %d bytes, the rest is ignored", path, CONF_FILE_MAX);
    int bad = lm_conf_parse(buf, path, fn, ud);
    free(buf);
    return bad;
}
