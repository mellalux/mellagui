#ifndef MELLAGUI_PATHCOMP_H
#define MELLAGUI_PATHCOMP_H

/*
 * File name handling for the prompts of the editor and the file manager (header only):
 *   lm_resolve_path   "~/x" is in the home folder, "/x" is absolute, anything else is relative to a base folder;
 *   lm_complete_path  Tab completion of the last name in a text (see the comment there).
 */

#include <dirent.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* "~/x" is in the home folder, "/x" is absolute, anything else is relative to `base`. */
static inline void lm_resolve_path(const char *base, const char *text, char *out, size_t sz)
{
    const char *home = getenv("HOME");
    if (text[0] == '~' && (text[1] == '/' || !text[1]) && home && home[0])
        snprintf(out, sz, "%s%s", home, text + 1);
    else if (text[0] == '/')
        snprintf(out, sz, "%s", text);
    else
        snprintf(out, sz, "%.*s%s%.*s", (int)(sz / 2), base, base[1] ? "/" : "", (int)(sz / 2) - 2, text);
}

struct lm_cand {
    char *name;
    bool dir;
};

static inline int lm_cand_cmp(const void *x, const void *y)
{
    return strcmp(((const struct lm_cand *)x)->name, ((const struct lm_cand *)y)->name);
}

/*
 * Tab in a file prompt. The last name in `buf` is completed as far as all candidates agree (`extend`; without it only
 * the list is made). `list` gets the candidates (folders with a trailing "/"). Returns how many there are.
 * A single candidate is completed fully, a folder with its "/".
 */
static inline int lm_complete_path(const char *base, char *buf, size_t bufsz, bool extend, char *list, size_t listsz)
{
    list[0] = 0;
    if (!strcmp(buf, "~") && getenv("HOME")) {
        snprintf(buf, bufsz, "~/");
        return 1;
    }
    char full[PATH_MAX], dir[PATH_MAX];
    lm_resolve_path(base, buf, full, sizeof(full));
    char *slash = strrchr(full, '/');
    if (!slash)
        return 0;
    snprintf(dir, sizeof(dir), "%.*s", (int)(slash == full ? 1 : slash - full), full);
    const char *pre = slash + 1;
    size_t pl = strlen(pre);
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    struct lm_cand c[512];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < 512) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        if (e->d_name[0] == '.' && !(pl > 0 && pre[0] == '.'))          /* hidden names only when a dot was typed */
            continue;
        if (strncmp(e->d_name, pre, pl) != 0)
            continue;
        char p[PATH_MAX + 300];
        struct stat st;
        snprintf(p, sizeof(p), "%s%s%s", dir, dir[1] ? "/" : "", e->d_name);
        c[n].dir = stat(p, &st) == 0 && S_ISDIR(st.st_mode);
        c[n++].name = strdup(e->d_name);
    }
    closedir(d);
    qsort(c, (size_t)n, sizeof(c[0]), lm_cand_cmp);
    size_t used = 0;
    for (int i = 0; i < n && used + 4 < listsz; i++)
        used += (size_t)snprintf(list + used, listsz - used, "%s%s%s", i ? "  " : "", c[i].name, c[i].dir ? "/" : "");
    if (n > 0 && extend) {
        size_t common = strlen(c[0].name);
        for (int i = 1; i < n; i++) {
            size_t k = 0;
            while (k < common && c[i].name[k] == c[0].name[k])
                k++;
            common = k;
        }
        while (common > pl && (c[0].name[common] & 0xc0) == 0x80)       /* do not cut a UTF-8 character */
            common--;
        const char *last = strrchr(buf, '/');
        size_t keep = last ? (size_t)(last - buf) + 1 : 0;
        char out[512];
        snprintf(out, sizeof(out), "%.*s%.*s%s", (int)keep, buf, (int)common, c[0].name, n == 1 && c[0].dir ? "/" : "");
        if (strlen(out) < bufsz)
            snprintf(buf, bufsz, "%s", out);
    }
    for (int i = 0; i < n; i++)
        free(c[i].name);
    return n;
}

#endif
