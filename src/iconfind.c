#include "iconfind.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const int sizes[] = { 256, 512, 192, 128, 96, 64, 48, 72, 36, 32 };

static bool exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

static bool valid_name(const char *name)
{
    if (!*name || strlen(name) > 80)
        return false;
    for (const char *p = name; *p; p++)
        if (*p == '/' || (*p == '.' && p[1] == '.') || (unsigned char)*p < 0x20)
            return false;
    return true;
}

static bool in_root(const char *root, const char *name, char *path, size_t n)
{
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        snprintf(path, n, "%s/icons/hicolor/%dx%d/apps/%s.png", root, sizes[i], sizes[i], name);
        if (exists(path))
            return true;
    }
    snprintf(path, n, "%s/pixmaps/%s.png", root, name);
    return exists(path);
}

static bool in_snaps(const char *name, char *path, size_t n)
{
    snprintf(path, n, "/snap/%s/current/default256.png", name);
    if (exists(path))
        return true;
    DIR *d = opendir("/snap");
    if (!d)
        return false;
    bool found = false;
    struct dirent *e;
    while (!found && (e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        snprintf(path, n, "/snap/%s/current/meta/gui/%s.png", e->d_name, name);
        found = exists(path);
    }
    closedir(d);
    return found;
}

static bool find_one(const char *name, char *path, size_t n)
{
    const char *env = getenv("MELLAGUI_ICON_PATH");
    char roots[1024];
    if (env) {
        snprintf(roots, sizeof(roots), "%s", env);
    } else {
        const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
        char user[PATH_MAX] = "", flat[PATH_MAX] = "";
        if (xdg && *xdg)
            snprintf(user, sizeof(user), "%s", xdg);
        else if (home && *home)
            snprintf(user, sizeof(user), "%s/.local/share", home);
        if (home && *home)
            snprintf(flat, sizeof(flat), "%s/.local/share/flatpak/exports/share", home);
        snprintf(roots, sizeof(roots), "%s:/usr/share:/usr/local/share:/var/lib/snapd/desktop:/var/lib/flatpak/exports/share:%s:snap", user, flat);
    }
    for (char *save = NULL, *r = strtok_r(roots, ":", &save); r; r = strtok_r(NULL, ":", &save)) {
        if (!strcmp(r, "snap")) {
            if (in_snaps(name, path, n))
                return true;
        } else if (in_root(r, name, path, n)) {
            return true;
        }
    }
    return false;
}

bool lm_icon_find(const char *names, char *path, size_t n)
{
    char list[256];
    snprintf(list, sizeof(list), "%s", names ? names : "");
    for (char *save = NULL, *nm = strtok_r(list, "|", &save); nm; nm = strtok_r(NULL, "|", &save))
        if (valid_name(nm) && find_one(nm, path, n))
            return true;
    return false;
}
