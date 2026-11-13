/*
 * wm_places.c - where MellaGUI's own programs had their windows the last time (contract in wm_priv.h: places_*).
 *
 * A window of an own program (app id "mellagui.*") that is moved, resized or closed leaves its place - the window geometry's x y width height and
 * whether it was maximized - under its app id in the windows file (--windows-file; default $XDG_CONFIG_HOME/mellagui/windows.conf, headless: none):
 *
 *     mellagui.smart 44 84 520 620 0
 *
 * The next window of that app id opens there (lm_wm_window_map asks places_find): the wm puts it at that position and asks the client for that size,
 * then keeps it on the output as it does with every new window; a second window of the same program is set off a little so that it does not hide
 * the first. Not remembered: the windows the wm places itself (the calendar, quick settings), the fullscreen ones (the screensaver, the screenshot
 * tool), the sticky notes (one window per note under one app id), dialogs with a parent, and other programs' windows - they have their own ideas.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "log.h"
#include "wm_priv.h"

#define PLACES_MAX 64

struct wm_place {
    char app[64];
    int32_t x, y, w, h;
    bool max;
};

static struct wm_place places[PLACES_MAX];
static int nplaces;

static bool place_wanted(const struct lm_window *w)
{
    static const char *const not_these[] = { "mellagui.shot", "mellagui.note", "mellagui.matrix", NULL };
    if (!w->app_id || strncmp(w->app_id, "mellagui.", 9) || strlen(w->app_id) >= sizeof(places[0].app) || w->parent || win_is_calendar(w))
        return false;
    for (int i = 0; not_these[i]; i++)
        if (!strcmp(w->app_id, not_these[i]))
            return false;
    return true;
}

static struct wm_place *place_of(const char *app)
{
    for (int i = 0; i < nplaces; i++)
        if (!strcmp(places[i].app, app))
            return &places[i];
    return NULL;
}

void places_load(struct lm_wm *wm)
{
    const char *path = wm->srv->config.windows_path;
    nplaces = 0;
    if (!path || !*path)
        return;
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[200];
    while (nplaces < PLACES_MAX && fgets(line, sizeof(line), f)) {
        struct wm_place p;
        int max = 0;
        if (line[0] == '#' || sscanf(line, "%63s %d %d %d %d %d", p.app, &p.x, &p.y, &p.w, &p.h, &max) < 5)
            continue;
        if (p.w < 40 || p.h < 40 || p.w > 16384 || p.h > 16384 || p.x < -16384 || p.x > 32768 || p.y < -16384 || p.y > 32768 || place_of(p.app))
            continue;
        p.max = max != 0;
        places[nplaces++] = p;
    }
    fclose(f);
}

static void places_save(struct lm_wm *wm)
{
    const char *path = wm->srv->config.windows_path;
    char dir[512], tmp[560];
    if (!path || !*path)
        return;
    if (snprintf(dir, sizeof(dir), "%s", path) >= (int)sizeof(dir) || snprintf(tmp, sizeof(tmp), "%s.%d.tmp", path, (int)getpid()) >= (int)sizeof(tmp))
        return;
    for (char *p = dir + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = 0;
        if (mkdir(dir, 0755) < 0 && errno != EEXIST)
            return;
        *p = '/';
    }
    FILE *f = fopen(tmp, "w");
    if (!f) {
        lm_warn("windows: cannot write %s: %s", tmp, strerror(errno));
        return;
    }
    fprintf(f, "# MellaGUI: where its own programs had their windows (app id, x, y, width, height, maximized). Written by the compositor.\n");
    for (int i = 0; i < nplaces; i++)
        fprintf(f, "%s %d %d %d %d %d\n", places[i].app, places[i].x, places[i].y, places[i].w, places[i].h, places[i].max);
    bool ok = !ferror(f);
    if (fclose(f) != 0)
        ok = false;
    if (!ok || rename(tmp, path) < 0) {
        lm_warn("windows: cannot save %s: %s", path, strerror(errno));
        unlink(tmp);
    }
}

/* A card that went back to its docked place (double click on its title bar) forgets where it had been. */
void places_forget(struct lm_wm *wm, const struct lm_window *w)
{
    struct wm_place *p = w->app_id ? place_of(w->app_id) : NULL;
    if (!p)
        return;
    memmove(p, p + 1, sizeof(*p) * (size_t)(nplaces - 1 - (int)(p - places)));
    nplaces--;
    places_save(wm);
}

bool places_find(struct lm_wm *wm, const struct lm_window *w, struct lm_rect *r, bool *maximized)
{
    const char *path = wm->srv->config.windows_path;
    const struct wm_place *p = path && *path && place_wanted(w) ? place_of(w->app_id) : NULL;
    if (!p)
        return false;
    *r = (struct lm_rect){ p->x, p->y, p->w, p->h };
    *maximized = p->max;
    struct lm_window *o;                                        /* another window of the program sits there: beside it, not on it */
    for (int tries = 0; tries < 8; tries++) {
        bool taken = false;
        lm_list_for_each(o, &wm->windows, link)
            if (o != w && o->mapped && o->app_id && !strcmp(o->app_id, w->app_id) && o->x == r->x && o->y == r->y)
                taken = true;
        if (!taken)
            break;
        r->x += 28;
        r->y += 28;
    }
    return true;
}

void places_remember(struct lm_wm *wm, struct lm_window *w)
{
    const char *path = wm->srv->config.windows_path;
    if (!path || !*path || !w->mapped || w->minimized || !place_wanted(w) || (w->states & LM_WINDOW_FULLSCREEN) || w->width < 40 || w->height < 40)
        return;
    if (win_is_gadget(w) && !WIN(w)->gad_placed)                /* a card docked by the wm has no place of its own to keep */
        return;
    struct wm_win *ww = WIN(w);
    struct wm_place now = { .x = w->x, .y = w->y, .w = w->width, .h = w->height };
    struct wm_place *p = place_of(w->app_id);
    if ((w->states & LM_WINDOW_MAXIMIZED) || ww->tiled) {       /* the place it goes back to is the one worth keeping */
        now.max = (w->states & LM_WINDOW_MAXIMIZED) != 0;
        if (ww->have_restore && ww->rw >= 40 && ww->rh >= 40) {
            now.x = ww->rx; now.y = ww->ry; now.w = ww->rw; now.h = ww->rh;
        } else if (p) {
            now.x = p->x; now.y = p->y; now.w = p->w; now.h = p->h;
        } else {
            return;
        }
    }
    if (p && p->x == now.x && p->y == now.y && p->w == now.w && p->h == now.h && p->max == now.max)
        return;
    if (!p) {
        if (nplaces >= PLACES_MAX)                              /* the oldest entry makes room */
            memmove(&places[0], &places[1], sizeof(places[0]) * (size_t)--nplaces);
        p = &places[nplaces++];
    }
    snprintf(now.app, sizeof(now.app), "%s", w->app_id);
    *p = now;
    places_save(wm);
}
