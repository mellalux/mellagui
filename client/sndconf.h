#ifndef MELLAGUI_SNDCONF_H
#define MELLAGUI_SNDCONF_H

/*
 * Which sound the recorder / the camera records, kept in $XDG_CONFIG_HOME/mellagui/NAME.conf (NAME = "record" or "camera"):
 *   system-sound = 1     the computer's own sound (the monitor of the default output: system beeps, music, a video ...)
 *   microphone   = 0     the default microphone
 * Read at the start, written when a switch is clicked.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static inline bool lm_snd_conf_path(const char *name, char *out, size_t n)
{
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    int w;
    if (xdg && *xdg)
        w = snprintf(out, n, "%s/mellagui/%s.conf", xdg, name);
    else if (home && *home)
        w = snprintf(out, n, "%s/.config/mellagui/%s.conf", home, name);
    else
        return false;
    return w > 0 && (size_t)w < n;
}

/* The values stay as they were (the caller's defaults) for what the file does not say. */
static inline void lm_snd_conf_load(const char *name, bool *system, bool *mic)
{
    char path[1024], line[128];
    if (!lm_snd_conf_path(name, path, sizeof(path)))
        return;
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        int v;
        if (sscanf(line, "system-sound = %d", &v) == 1 || sscanf(line, "system-sound=%d", &v) == 1)
            *system = v != 0;
        else if (sscanf(line, "microphone = %d", &v) == 1 || sscanf(line, "microphone=%d", &v) == 1)
            *mic = v != 0;
    }
    fclose(f);
}

static inline void lm_snd_conf_save(const char *name, bool system, bool mic)
{
    char path[1024], tmp[1100];
    if (!lm_snd_conf_path(name, path, sizeof(path)))
        return;
    snprintf(tmp, sizeof(tmp), "%s.%d.tmp", path, (int)getpid());
    for (char *p = path + 1; *p; p++) {                         /* parent directories */
        if (*p != '/')
            continue;
        *p = 0;
        mkdir(path, 0755);
        *p = '/';
    }
    FILE *f = fopen(tmp, "w");
    if (!f)
        return;
    fprintf(f, "# mellagui-%s: written by the program itself\nsystem-sound = %d\nmicrophone = %d\n", name, system ? 1 : 0, mic ? 1 : 0);
    if (fclose(f) != 0 || rename(tmp, path) < 0)
        unlink(tmp);
}

#endif
