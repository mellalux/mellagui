#include "smartfav.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool fav_path(char *out, size_t n)
{
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    int w;
    if (xdg && *xdg)
        w = snprintf(out, n, "%s/mellagui/smart-favourites.conf", xdg);
    else if (home && *home)
        w = snprintf(out, n, "%s/.config/mellagui/smart-favourites.conf", home);
    else
        return false;
    return w > 0 && (size_t)w < n;
}

int fav_load(char ids[][FAV_ID_MAX], int max)
{
    char path[1024], line[128];
    int n = 0;
    if (!fav_path(path, sizeof(path)))
        return 0;
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    while (n < max && fgets(line, sizeof(line), f)) {
        char id[FAV_ID_MAX];
        if (line[0] == '#' || sscanf(line, "%39s", id) != 1 || fav_find(ids, n, id) >= 0)
            continue;
        snprintf(ids[n++], FAV_ID_MAX, "%s", id);
    }
    fclose(f);
    return n;
}

bool fav_save(char ids[][FAV_ID_MAX], int n)
{
    char path[1024], tmp[1100];
    if (!fav_path(path, sizeof(path)))
        return false;
    for (char *p = path + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = 0;
        mkdir(path, 0700);
        *p = '/';
    }
    snprintf(tmp, sizeof(tmp), "%s.%d.tmp", path, (int)getpid());
    FILE *f = fopen(tmp, "w");
    if (!f)
        return false;
    fprintf(f, "# mellagui-smart: the devices shown on the desktop card (one id a line). Written by the program itself.\n");
    for (int i = 0; i < n; i++)
        fprintf(f, "%s\n", ids[i]);
    bool ok = !ferror(f);
    if (fclose(f) != 0)
        ok = false;
    if (!ok || rename(tmp, path) < 0) {
        unlink(tmp);
        return false;
    }
    return true;
}

int fav_find(char ids[][FAV_ID_MAX], int n, const char *id)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(ids[i], id))
            return i;
    return -1;
}

bool fav_set(const char *id, bool on)
{
    char ids[FAV_MAX][FAV_ID_MAX];
    int n = fav_load(ids, FAV_MAX), at = fav_find(ids, n, id);
    if (on && at < 0) {
        if (n >= FAV_MAX || strlen(id) >= FAV_ID_MAX)
            return false;
        snprintf(ids[n++], FAV_ID_MAX, "%s", id);
    } else if (!on && at >= 0) {
        memmove(ids[at], ids[at + 1], (size_t)(n - at - 1) * FAV_ID_MAX);
        n--;
    } else {
        return true;
    }
    return fav_save(ids, n);
}
