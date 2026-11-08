/*
 * Section-wise editing of mellagui.conf. See confedit.h.
 */
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "confedit.h"
#include "log.h"

#define EDIT_FILE_MAX (256 * 1024)

/* The section name of a "[name]" line, or NULL. */
static bool is_header(const char *line, size_t len, char *name, size_t n)
{
    size_t a = 0;
    while (a < len && isspace((unsigned char)line[a]))
        a++;
    if (a >= len || line[a] != '[')
        return false;
    size_t b = a + 1;
    while (b < len && line[b] != ']')
        b++;
    if (b >= len)
        return false;
    size_t s = a + 1, e = b;
    while (s < e && isspace((unsigned char)line[s]))
        s++;
    while (e > s && isspace((unsigned char)line[e - 1]))
        e--;
    if (e - s >= n)
        return false;
    memcpy(name, line + s, e - s);
    name[e - s] = 0;
    return true;
}

static bool is_comment(const char *line, size_t len)
{
    size_t a = 0;
    while (a < len && isspace((unsigned char)line[a]))
        a++;
    return a < len && (line[a] == '#' || line[a] == ';');
}

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

static char *read_all(const char *path, size_t *len)
{
    *len = 0;
    FILE *f = fopen(path, "r");
    char *buf = malloc(EDIT_FILE_MAX + 1);
    if (!buf) {
        if (f)
            fclose(f);
        return NULL;
    }
    if (f) {
        *len = fread(buf, 1, EDIT_FILE_MAX, f);
        fclose(f);
    }
    buf[*len] = 0;
    return buf;
}

static bool write_atomic(const char *path, const char *out, size_t o)
{
    char dir[512], tmp[560];
    bool ok = false;
    if (snprintf(dir, sizeof(dir), "%s", path) < (int)sizeof(dir) &&
        snprintf(tmp, sizeof(tmp), "%s.%d.tmp", path, (int)getpid()) < (int)sizeof(tmp)) {
        mkdir_parents(dir);
        FILE *f = fopen(tmp, "w");
        if (f) {
            ok = fwrite(out, 1, o, f) == o;
            ok = fclose(f) == 0 && ok;
            if (ok && rename(tmp, path) < 0)
                ok = false;
            if (!ok)
                unlink(tmp);
        }
        if (!ok)
            lm_warn("confedit: cannot write %s: %s", path, strerror(errno));
    }
    return ok;
}

bool lm_confedit_set_section(const char *path, const char *section, const char *body)
{
    size_t len;
    char *text = read_all(path, &len);
    if (!text)
        return false;
    size_t body_len = body ? strlen(body) : 0;
    size_t cap = len + body_len + strlen(section) + 16;
    char *out = malloc(cap);
    if (!out) {
        free(text);
        return false;
    }
    size_t o = 0;
    bool seen = false;

#define EMIT_LINE(p, ll) do { memcpy(out + o, (p), (ll)); o += (ll); out[o++] = '\n'; } while (0)
    const char *p = text;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t ll = eol ? (size_t)(eol - p) : strlen(p);
        const char *next = eol ? eol + 1 : p + ll;
        char name[64];
        if (!is_header(p, ll, name, sizeof(name)) || strcasecmp(name, section)) {
            EMIT_LINE(p, ll);
            p = next;
            continue;
        }
        /* a [section] header: keep the first one (and the comments right under it), drop its old body, put the new one in */
        bool keep = !seen && body;
        seen = true;
        if (keep)
            EMIT_LINE(p, ll);
        p = next;
        bool head = true;
        while (*p) {
            eol = strchr(p, '\n');
            ll = eol ? (size_t)(eol - p) : strlen(p);
            next = eol ? eol + 1 : p + ll;
            char other[64];
            if (is_header(p, ll, other, sizeof(other)))
                break;
            if (keep && head && is_comment(p, ll))
                EMIT_LINE(p, ll);
            else
                head = false;
            p = next;
        }
        if (keep) {
            memcpy(out + o, body, body_len);
            o += body_len;
            if (*p)
                out[o++] = '\n';                     /* a blank line before the next section */
        }
        if (!body) {                                /* a removed section must not leave two blank lines (or one at the very end) behind */
            const char *q = p;
            while (*q == ' ' || *q == '\t')
                q++;
            bool next_blank = !*q || *q == '\n';
            while (o >= 2 && out[o - 1] == '\n' && out[o - 2] == '\n' && next_blank)
                o--;
        }
    }
#undef EMIT_LINE
    if (!seen && body) {
        if (o && out[o - 1] != '\n')
            out[o++] = '\n';
        if (o)
            out[o++] = '\n';
        o += (size_t)snprintf(out + o, cap - o, "[%s]\n", section);
        memcpy(out + o, body, body_len);
        o += body_len;
    }
    out[o] = 0;

    bool ok = write_atomic(path, out, o);
    free(text);
    free(out);
    return ok;
}

/* ---- one key of a section ------------------------------------------------------------------------------------------ */

bool lm_confedit_set_key(const char *path, const char *section, bool (*same)(const char *, void *), void *ud, const char *key,
                         const char *value)
{
    size_t len;
    char *text = read_all(path, &len);
    if (!text)
        return false;
    size_t cap = len + strlen(key) + (value ? strlen(value) : 0) + strlen(section) + 32;
    char *out = malloc(cap);
    if (!out) {
        free(text);
        return false;
    }
    size_t o = 0;
    bool in_section = false, done = false, any_section = false;
    size_t last_body_end = 0;               /* output offset just after the last non-blank line of the section */
    size_t insert_at = (size_t)-1;

#define EMIT_LINE(p, ll) do { memcpy(out + o, (p), (ll)); o += (ll); out[o++] = '\n'; } while (0)
    for (const char *p = text; *p;) {
        const char *eol = strchr(p, '\n');
        size_t ll = eol ? (size_t)(eol - p) : strlen(p);
        const char *next = eol ? eol + 1 : p + ll;
        char name[64];
        if (is_header(p, ll, name, sizeof(name))) {
            if (in_section && !done && insert_at == (size_t)-1)
                insert_at = last_body_end;
            in_section = !strcasecmp(name, section);
            any_section |= in_section;
            if (in_section)
                last_body_end = o + ll + 1;
            EMIT_LINE(p, ll);
            p = next;
            continue;
        }
        if (in_section && !is_comment(p, ll)) {
            const char *eq = memchr(p, '=', ll);
            if (eq) {
                char k[128];
                size_t a = 0, b = (size_t)(eq - p);
                while (a < b && isspace((unsigned char)p[a]))
                    a++;
                while (b > a && isspace((unsigned char)p[b - 1]))
                    b--;
                if (b - a < sizeof(k)) {
                    memcpy(k, p + a, b - a);
                    k[b - a] = 0;
                    if (same(k, ud)) {
                        if (!done && value)
                            o += (size_t)snprintf(out + o, cap - o, "%s = %s\n", key, value);
                        done = true;                 /* later matches are dropped */
                        p = next;
                        if (value)
                            last_body_end = o;
                        continue;
                    }
                }
            }
        }
        EMIT_LINE(p, ll);
        if (in_section) {
            bool blank = true;
            for (size_t i = 0; i < ll; i++)
                if (!isspace((unsigned char)p[i]))
                    blank = false;
            if (!blank)
                last_body_end = o;
        }
        p = next;
    }
#undef EMIT_LINE
    if (!done && value) {
        char line[1400];
        int ln = snprintf(line, sizeof(line), "%s = %s\n", key, value);
        if (any_section) {
            size_t at = insert_at != (size_t)-1 ? insert_at : last_body_end;       /* after the last line of the (last) section */
            memmove(out + at + ln, out + at, o - at);
            memcpy(out + at, line, (size_t)ln);
            o += (size_t)ln;
        } else {
            if (o && out[o - 1] != '\n')
                out[o++] = '\n';
            if (o)
                out[o++] = '\n';
            o += (size_t)snprintf(out + o, cap - o, "[%s]\n%s", section, line);
        }
    }
    out[o] = 0;
    bool ok = write_atomic(path, out, o);
    free(text);
    free(out);
    return ok;
}
