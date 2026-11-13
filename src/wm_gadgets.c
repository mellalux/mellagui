/* Part of the window manager (see wm_priv.h): the desktop gadgets and the places kept between runs.
 *
 * A gadget (the clipboard history, the volume mixer, the system monitor, the smart home card) is a program of its own: `mellagui-gadget NAME`,
 * one process each, started when the gadget is switched on (setting `gadget` / `mixer` / `sysinfo` / `smart`, Super+G / V / S / H) and ended when it is
 * switched off. Its window (app id "mellagui.gadget.NAME") has a role in the wm: no frame, always at the bottom of the stack, on every workspace, not
 * focused by a click, listed nowhere (win_is_gadget). The card is docked at the top right under the panel, one below the other in the order of the
 * ids, until the user moves it (the program asks for a move with xdg_toplevel.move; the place is remembered by wm_places.c like every program's).
 *
 * The file the places are kept in (gadgets.conf) only has the on-screen keyboard's place now.
 */
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "launchers.h"
#include "log.h"
#include "wm_priv.h"

static const char *const gadget_names[LM_GADGET_COUNT] = { "clipboard", "mixer", "system", "smart" };

/* Gadgets of other programs: a file NAME.conf in the gadgets folder ($MELLAGUI_GADGET_DIR, else $XDG_CONFIG_HOME/mellagui/gadgets, else
 * ~/.config/mellagui/gadgets) each, with the lines `command = ...` (started like a dock button's: @terminal, ~/, bare mellagui-* ...), `autostart = yes|no`
 * (default yes) and `size = WxH` (what a double click on the title bar goes back to; default 240x120). The program opens a window with the app id
 * "mellagui.gadget.NAME" and from then on is treated like a built-in card. Read once, when the compositor starts. */
#define EXT_NAME 25
#define EXT_CMD 400
static struct gad_ext {
    char name[EXT_NAME];
    char cmd[EXT_CMD];
    int w, h;
    bool autostart;
} ext[LM_GADGET_EXT_MAX];
static int n_ext;
/* A file named like a built-in gadget replaces that gadget's program (`command`) and size (`size`); whether it is on stays the setting's / shortcut's business. */
static struct { char cmd[EXT_CMD]; int w, h; } over[LM_GADGET_COUNT];

const char *gadget_name(int id)
{
    if (id >= 0 && id < LM_GADGET_COUNT)
        return gadget_names[id];
    if (id >= LM_GADGET_COUNT && id < LM_GADGET_COUNT + n_ext)
        return ext[id - LM_GADGET_COUNT].name;
    return "?";
}

int lm_wm_gadget_by_name(const struct lm_wm *wm, const char *name)
{
    (void)wm;
    if (!name)
        return -1;
    for (int i = 0; i < LM_GADGET_COUNT + n_ext; i++)
        if (!strcmp(name, gadget_name(i)))
            return i;
    return -1;
}

static bool ext_name_ok(const char *n)
{
    size_t len = strlen(n);
    if (len == 0 || len >= EXT_NAME)
        return false;
    for (size_t i = 0; i < len; i++)
        if (!((n[i] >= 'a' && n[i] <= 'z') || (n[i] >= '0' && n[i] <= '9') || n[i] == '-' || n[i] == '_'))
            return false;
    return true;
}

static void ext_read_file(const char *dir, const char *file)
{
    size_t fl = strlen(file);
    if (fl < 6 || strcmp(file + fl - 5, ".conf") || n_ext >= LM_GADGET_EXT_MAX)
        return;
    struct gad_ext g = { .w = 240, .h = 120, .autostart = true };
    snprintf(g.name, sizeof(g.name), "%.*s", (int)(fl - 5) < EXT_NAME ? (int)(fl - 5) : EXT_NAME - 1, file);
    if (!ext_name_ok(g.name)) {
        lm_warn("gadgets: %s: the name must be 1..24 letters a-z, digits, - or _, and not a built-in one", file);
        return;
    }
    char path[1000];
    snprintf(path, sizeof(path), "%s/%s", dir, file);
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[EXT_CMD + 40];
    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = 0;
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#' || line[0] == '[')
            continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        while (*k == ' ' || *k == '\t') k++;
        for (char *e = k + strlen(k); e > k && (e[-1] == ' ' || e[-1] == '\t'); ) *--e = 0;
        while (*v == ' ' || *v == '\t') v++;
        for (char *e = v + strlen(v); e > v && (e[-1] == ' ' || e[-1] == '\t'); ) *--e = 0;
        if (!strcmp(k, "command"))
            snprintf(g.cmd, sizeof(g.cmd), "%s", v);
        else if (!strcmp(k, "autostart"))
            g.autostart = strcmp(v, "no") && strcmp(v, "0") && strcmp(v, "false") && strcmp(v, "off");
        else if (!strcmp(k, "size")) {
            int w, h;
            if (sscanf(v, "%dx%d", &w, &h) == 2 && w >= 40 && w <= 640 && h >= 30 && h <= 1600) {
                g.w = w;
                g.h = h;
            }
        }
    }
    fclose(f);
    if (!g.cmd[0]) {
        lm_warn("gadgets: %s has no command", path);
        return;
    }

    for (int i = 0; i < LM_GADGET_COUNT; i++)
        if (!strcmp(g.name, gadget_names[i])) {
            snprintf(over[i].cmd, sizeof(over[i].cmd), "%s", g.cmd);
            over[i].w = g.w;
            over[i].h = g.h;
            return;
        }
    ext[n_ext++] = g;
}

static int ext_cmp(const void *a, const void *b)
{
    return strcmp(((const struct gad_ext *)a)->name, ((const struct gad_ext *)b)->name);
}

static void ext_load(struct lm_wm *wm, bool headless)
{
    const char *dir = getenv("MELLAGUI_GADGET_DIR");
    char buf[600];
    if (!dir || !*dir) {
        if (headless)
            return;                                             /* (the tests read a folder only when they name one) */
        const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
        if (xdg && *xdg)
            snprintf(buf, sizeof(buf), "%s/mellagui/gadgets", xdg);
        else if (home && *home)
            snprintf(buf, sizeof(buf), "%s/.config/mellagui/gadgets", home);
        else
            return;
        dir = buf;
    }
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *de;
    while ((de = readdir(d)))
        ext_read_file(dir, de->d_name);
    closedir(d);
    qsort(ext, (size_t)n_ext, sizeof(ext[0]), ext_cmp);         /* (docked in the order of the names) */
    for (int i = 0; i < n_ext; i++)
        wm->gad[LM_GADGET_COUNT + i].visible = ext[i].autostart;
}

/* ---- the on-screen keyboard's place, kept between runs ----------------------------------------------------------------------- */

void gadgets_save(struct lm_wm *wm)
{
    const char *path = wm->srv->config.gadgets_path;
    if (!path || !*path)
        return;
    char dir[512], tmp[560];
    if (snprintf(dir, sizeof(dir), "%s", path) >= (int)sizeof(dir) ||
        snprintf(tmp, sizeof(tmp), "%s.%d.tmp", path, (int)getpid()) >= (int)sizeof(tmp))
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
        lm_warn("gadgets: cannot write %s: %s", tmp, strerror(errno));
        return;
    }
    if (wm->osk_pl.w > 0)                                           /* the on-screen keyboard: top left corner, width and key height */
        fprintf(f, "keyboard %d %d %d %d\n", wm->osk_pl.x, wm->osk_pl.y, wm->osk_pl.w, wm->osk_pl.kh);
    bool ok = !ferror(f);
    if (fclose(f) != 0)
        ok = false;
    if (!ok || rename(tmp, path) < 0) {
        lm_warn("gadgets: cannot save %s: %s", path, strerror(errno));
        unlink(tmp);
    }
}

void gadgets_load(struct lm_wm *wm)
{
    const char *path = wm->srv->config.gadgets_path;
    FILE *f = path && *path ? fopen(path, "r") : NULL;
    if (!f)
        return;
    char line[160], name[24];
    while (fgets(line, sizeof(line), f)) {
        int x, y, w, h;
        if (sscanf(line, "%23s %d %d %d %d", name, &x, &y, &w, &h) != 5 || strcmp(name, "keyboard"))
            continue;
        if (abs(x) <= 20000 && abs(y) <= 20000 && w > 0 && w <= 20000 && h > 0 && h <= 1000) {   /* (clamped to the output when it is laid out) */
            wm->osk_pl.x = x;
            wm->osk_pl.y = y;
            wm->osk_pl.w = w;
            wm->osk_pl.kh = h;
        }
    }
    fclose(f);
}

/* ---- the windows ------------------------------------------------------------------------------------------------------------- */

static int gadget_rank(const struct lm_window *w)
{
    for (int i = 0; i < LM_GADGET_COUNT + n_ext; i++)
        if (w->app_id && !strcmp(w->app_id + 16, gadget_name(i)))
            return i;
    return LM_GADGET_COUNT + n_ext;                             /* (a card nobody listed: after the others) */
}

/* The docked cards (not moved by the user) stack downwards from the top right in the order of the ids; when the next one does not fit below the
 * previous one it starts a new column to its left. Run whenever a gadget window comes or goes. */
void gadgets_redock(struct lm_wm *wm)
{
    struct lm_output *o = gadget_output(wm);
    if (!o)
        return;
    int margin = GADGET_MARGIN, x = o->x + o->width - margin, y = o->y + LM_PANEL_HEIGHT + margin, colw = 0;
    for (int rank = 0; rank <= LM_GADGET_COUNT + n_ext; rank++) {
        struct lm_window *w;
        lm_list_for_each(w, &wm->windows, link) {
            if (!win_is_gadget(w) || !w->mapped || WIN(w)->gad_placed || gadget_rank(w) != rank)
                continue;
            if (y + w->height > o->y + o->height - margin && colw > 0) {
                x -= colw + margin;                             /* a small screen: a new column to the left */
                y = o->y + LM_PANEL_HEIGHT + margin;
                colw = 0;
            }
            int nx = x - w->width, ny = y;
            if (nx != w->x || ny != w->y) {
                struct lm_rect before = frame_outer(w);
                w->x = nx;
                w->y = ny;
                damage_rect(wm, &before);
                damage_frame(wm, w);
                update_outputs(wm, w);
            }
            y += w->height + margin;
            colw = LM_MAX(colw, w->width);
        }
    }
}

void gadget_map(struct lm_wm *wm, struct lm_window *w)
{
    struct wm_win *ww = WIN(w);
    struct lm_rect was = { 0, 0, 0, 0 };
    bool was_max = false;
    ww->placed = true;
    if (places_find(wm, w, &was, &was_max)) {
        struct lm_output *o = lm_output_nearest(wm->srv, was.x + was.w / 2.0, was.y + was.h / 2.0);
        w->x = was.x;
        w->y = was.y;
        if (o) {                                                /* (kept on the screen it was on) */
            w->x = LM_CLAMP(w->x, o->x, LM_MAX(o->x + o->width - w->width, o->x));
            w->y = LM_CLAMP(w->y, o->y + LM_PANEL_HEIGHT, LM_MAX(o->y + o->height - w->height, o->y + LM_PANEL_HEIGHT));
        }
        ww->gad_placed = true;
        if (was.w > 0 && was.h > 0 && (was.w != w->width || was.h != w->height))
            send_configure(wm, w, was.w, was.h);
    }
    int rank = gadget_rank(w);
    if (!ww->gad_placed && rank < LM_GADGET_COUNT + n_ext) {      /* a card from the folder (or one whose file names a size) starts at that size */
        int gw = 0, gh = 0;
        if (rank >= LM_GADGET_COUNT) {
            gw = ext[rank - LM_GADGET_COUNT].w;
            gh = ext[rank - LM_GADGET_COUNT].h;
        } else if (over[rank].w > 0) {
            gw = over[rank].w;
            gh = over[rank].h;
        }
        if (gw > 0 && (gw != w->width || gh != w->height))
            send_configure(wm, w, gw, gh);
    }
    w->mapped = true;
    w->workspace = wm->workspace;
    w->minimized = false;
    lm_list_remove(&w->link);
    lm_list_insert(&wm->windows, &w->link);                     /* the bottom of the stack: above the wallpaper, below every window */
    gadgets_redock(wm);
    update_outputs(wm, w);
    damage_frame(wm, w);
    refresh_pointer(wm);
}

/* ---- the programs -------------------------------------------------------------------------------------------------------------- */

static struct gad_proc {
    struct lm_wm *wm;
    int id;
    pid_t pid;
} procs[LM_GADGET_TOTAL];

static void proc_exit(void *data, int status, const char *err)
{
    struct gad_proc *p = data;
    if (status != 0)
        lm_warn("gadget %s ended with status %d%s%s", gadget_name(p->id), status, err && *err ? ": " : "", err ? err : "");
    p->pid = 0;
    struct lm_wm *wm = p->wm;
    if (wm && wm->gad[p->id].visible) {                         /* it ended on its own (a crash, or its window was closed): the gadget is off */
        wm->gad[p->id].visible = false;
        lm_settings_changed(wm->srv->settings);
    }
}

static void proc_start(struct lm_wm *wm, int id)
{
    struct gad_proc *p = &procs[id];
    if (p->pid > 0 && kill(p->pid, 0) == 0)
        return;
    if (getenv("MELLAGUI_NO_GADGET_PROCS"))                          /* (the fuzzer: no program may connect to it) */
        return;
    char spec[96], cmd[PATH_MAX + 64];
    const char *dump = getenv("MELLAGUI_GADGET_DUMP");               /* (the tests: the program writes its state there; an old file would be stale) */
    if (dump && *dump) {
        char path[600];
        snprintf(path, sizeof(path), "%s.%s", dump, gadget_name(id));
        unlink(path);
    }
    snprintf(spec, sizeof(spec), "mellagui-gadget %s", gadget_name(id));
    if (id >= LM_GADGET_COUNT)
        lm_launch_expand(ext[id - LM_GADGET_COUNT].cmd, cmd, sizeof(cmd));
    else
        lm_launch_expand(over[id].cmd[0] ? over[id].cmd : spec, cmd, sizeof(cmd));
    p->wm = wm;
    p->id = id;
    p->pid = lm_server_spawn_watch(wm->srv, cmd, proc_exit, p);
    if (p->pid <= 0) {
        p->pid = 0;
        lm_warn("gadget %s: cannot start %s", gadget_name(id), cmd);
    }
}

static void proc_stop(int id)
{
    struct gad_proc *p = &procs[id];
    if (p->pid > 0)
        kill(-p->pid, SIGTERM);                                 /* the whole process group: the shell and the program */
}

/* At start: the gadgets that are switched on. */
void lm_wm_gadgets_start(struct lm_wm *wm, bool headless)
{
    ext_load(wm, headless);
    for (int i = 0; i < LM_GADGET_COUNT + n_ext; i++) {
        if (!wm->gad[i].visible)
            continue;
        /* The clipboard card is on by default: the headless backend (the tests) starts it only when asked, the others are on only when asked anyway. */
        if (headless && i == LM_GADGET_CLIPBOARD && !getenv("MELLAGUI_GADGETS_PRELOAD"))
            wm->gad[i].visible = false;
        else
            proc_start(wm, i);
    }
}

/*
 * Gadgets are sticky: a card that is dragged within GADGET_SNAP of another card's edge (or of the screen's) jumps flush against it. Beside another card it
 * needs to overlap it vertically (within the same reach), above / below it horizontally; a card that touches another one with a side also lines up with
 * it (same left / right / top / bottom edge). A resized edge sticks to the edges that line up with it.
 */
static void gadget_snap(struct lm_wm *wm, const struct lm_window *self, const struct lm_output *o, int32_t *px, int32_t *py, int32_t w, int32_t h)
{
    int32_t x = *px, y = *py, nx = x, ny = y;
    int bestx = GADGET_SNAP + 1, besty = GADGET_SNAP + 1;
#define TRY_X(c) do { int32_t cand_ = (c); int d_ = abs(cand_ - x); if (d_ < bestx) { bestx = d_; nx = cand_; } } while (0)
#define TRY_Y(c) do { int32_t cand_ = (c); int d_ = abs(cand_ - y); if (d_ < besty) { besty = d_; ny = cand_; } } while (0)
    TRY_X(o->x);
    TRY_X(o->x + o->width - w);
    TRY_Y(o->y + LM_PANEL_HEIGHT);
    TRY_Y(o->y + o->height - h);
    struct lm_window *other;
    lm_list_for_each(other, &wm->windows, link) {
        if (other == self || !win_is_gadget(other) || !other->mapped)
            continue;
        struct lm_rect pr = lm_rect_make(other->x, other->y, other->width, other->height);
        bool vover = y < pr.y + pr.h + GADGET_SNAP && y + h > pr.y - GADGET_SNAP;
        bool hover = x < pr.x + pr.w + GADGET_SNAP && x + w > pr.x - GADGET_SNAP;
        bool side = abs(x - (pr.x + pr.w)) <= GADGET_SNAP || abs(x + w - pr.x) <= GADGET_SNAP;       /* beside it */
        bool stack = abs(y - (pr.y + pr.h)) <= GADGET_SNAP || abs(y + h - pr.y) <= GADGET_SNAP;      /* above / below it */
        if (vover) {
            TRY_X(pr.x + pr.w);
            TRY_X(pr.x - w);
        }
        if (hover) {
            TRY_Y(pr.y + pr.h);
            TRY_Y(pr.y - h);
        }
        if (stack && hover) {                                       /* stacked: the same left or right edge */
            TRY_X(pr.x);
            TRY_X(pr.x + pr.w - w);
        }
        if (side && vover) {                                        /* side by side: the same top or bottom edge */
            TRY_Y(pr.y);
            TRY_Y(pr.y + pr.h - h);
        }
    }
#undef TRY_X
#undef TRY_Y
    *px = nx;
    *py = ny;
}

/* A line (an edge of the card being resized) sticks to the edges that line up with it: the screen's and those of the other cards that it faces.
 * `vertical` = the line is a left/right edge at x = v (the card spans [lo, hi] in y), else a top/bottom edge at y = v (the card spans [lo, hi] in x). */
static int32_t gadget_snap_line(struct lm_wm *wm, const struct lm_window *self, const struct lm_output *o, bool vertical, int32_t v, int32_t lo, int32_t hi)
{
    int32_t best = v;
    int bestd = GADGET_SNAP + 1;
#define TRY_LINE(c) do { int32_t cand_ = (c); int d_ = abs(cand_ - v); if (d_ < bestd) { bestd = d_; best = cand_; } } while (0)
    if (vertical) {
        TRY_LINE(o->x);
        TRY_LINE(o->x + o->width);
    } else {
        TRY_LINE(o->y + LM_PANEL_HEIGHT);
        TRY_LINE(o->y + o->height);
    }
    struct lm_window *other;
    lm_list_for_each(other, &wm->windows, link) {
        if (other == self || !win_is_gadget(other) || !other->mapped)
            continue;
        struct lm_rect pr = lm_rect_make(other->x, other->y, other->width, other->height);
        if (vertical && lo < pr.y + pr.h + GADGET_SNAP && hi > pr.y - GADGET_SNAP) {
            TRY_LINE(pr.x);
            TRY_LINE(pr.x + pr.w);
        } else if (!vertical && lo < pr.x + pr.w + GADGET_SNAP && hi > pr.x - GADGET_SNAP) {
            TRY_LINE(pr.y);
            TRY_LINE(pr.y + pr.h);
        }
    }
#undef TRY_LINE
    return best;
}

/* A card is moved / resized: the pointer moved by (dx, dy) since the grab began (grab_x/y/w/h = the card back then). */
void gadget_grab_motion(struct lm_wm *wm, struct lm_window *win, double dx, double dy)
{
    struct lm_output *o = gadget_output(wm);
    if (!o)
        return;
    int32_t x = wm->grab_x, y = wm->grab_y, w = wm->grab_w, h = wm->grab_h;
    int32_t right = x + w, bottom = y + h;
    int32_t minw = LM_MAX(win->min_w, 1), minh = LM_MAX(win->min_h, 1);
    int32_t maxw = win->max_w > 0 ? win->max_w : 640, maxh = win->max_h > 0 ? win->max_h : 1600;
    if (wm->grab_kind == GRAB_MOVE) {
        x += (int32_t)dx;
        y += (int32_t)dy;
        gadget_snap(wm, win, o, &x, &y, w, h);
        x = LM_CLAMP(x, o->x, LM_MAX(o->x, o->x + o->width - w));
        y = LM_CLAMP(y, o->y + LM_PANEL_HEIGHT, LM_MAX(o->y + LM_PANEL_HEIGHT, o->y + o->height - h));
        if (x == win->x && y == win->y)
            return;
        struct lm_rect before = frame_outer(win);
        win->x = x;
        win->y = y;
        damage_rect(wm, &before);
        damage_frame(wm, win);
        update_outputs(wm, win);
        return;
    }
    uint32_t e = wm->grab_edges;
    if (e & LM_EDGE_RIGHT) {
        int32_t edge = gadget_snap_line(wm, win, o, true, x + w + (int32_t)dx, y, y + h);
        w = LM_CLAMP(edge - x, minw, LM_MIN(maxw, o->x + o->width - x));
    }
    if (e & LM_EDGE_LEFT) {
        int32_t edge = gadget_snap_line(wm, win, o, true, x + (int32_t)dx, y, y + h);
        w = LM_CLAMP(right - edge, minw, LM_MIN(maxw, right - o->x));
    }
    if (e & LM_EDGE_BOTTOM) {
        int32_t edge = gadget_snap_line(wm, win, o, false, y + h + (int32_t)dy, x, x + w);
        h = LM_CLAMP(edge - y, minh, LM_MIN(maxh, o->y + o->height - y));
    }
    if (e & LM_EDGE_TOP) {
        int32_t edge = gadget_snap_line(wm, win, o, false, y + (int32_t)dy, x, x + w);
        h = LM_CLAMP(bottom - edge, minh, LM_MIN(maxh, bottom - o->y - LM_PANEL_HEIGHT));
    }
    win->states |= LM_WINDOW_RESIZING;
    send_configure(wm, win, w, h);                              /* (a left / top edge: lm_wm_window_set_geometry keeps the far edge in place) */
}

/* The size a gadget card starts with (and gets back after a double click on its title bar). */
static void gadget_default_size(int id, int *w, int *h)
{
    if (id >= 0 && id < LM_GADGET_COUNT && over[id].w > 0) {
        *w = over[id].w;
        *h = over[id].h;
        return;
    }
    switch (id) {
    case LM_GADGET_MIXER: *w = LM_MAX(lm_metrics.mixer_width, 210); *h = lm_metrics.mixer_height; break;
    case LM_GADGET_SYSTEM: *w = lm_metrics.sys_width; *h = lm_metrics.sys_height; break;
    case LM_GADGET_SMART: *w = 290; *h = 180; break;
    default:
        if (id >= LM_GADGET_COUNT && id < LM_GADGET_COUNT + n_ext) {
            *w = ext[id - LM_GADGET_COUNT].w;
            *h = ext[id - LM_GADGET_COUNT].h;
            break;
        }
        *w = lm_metrics.gadget_width;
        *h = 26 + 5 * 36 + 8;
        break;
    }
}

/* Double click on the title bar: back to the default size, docked top right. */
void lm_wm_gadget_reset(struct lm_wm *wm, int id)
{
    struct lm_window *w = gadget_window(wm, id);
    if (!w)
        return;
    WIN(w)->gad_placed = false;
    int dw, dh;
    gadget_default_size(id, &dw, &dh);
    if (dw != w->width || dh != w->height)
        send_configure(wm, w, dw, dh);
    gadgets_redock(wm);
    places_forget(wm, w);
}

/* SIGUSR1: the smart home card asks its program again (the control socket's `smart refresh`). */
void lm_wm_gadget_refresh(struct lm_wm *wm, int id)
{
    (void)wm;
    if (id >= 0 && id < LM_GADGET_COUNT && procs[id].pid > 0)
        kill(procs[id].pid, SIGUSR1);
}

void gadget_procs_stop(struct lm_wm *wm)
{
    (void)wm;
    for (int i = 0; i < LM_GADGET_COUNT + n_ext; i++) {
        proc_stop(i);
        procs[i].wm = NULL;                                     /* (its exit must not touch the wm any more) */
    }
}

bool lm_wm_gadget_visible_id(const struct lm_wm *wm, int id)
{
    return id >= 0 && id < LM_GADGET_COUNT + n_ext && wm->gad[id].visible;
}

bool lm_wm_gadget_visible(const struct lm_wm *wm)
{
    return lm_wm_gadget_visible_id(wm, LM_GADGET_CLIPBOARD);
}

void lm_wm_set_gadget_id(struct lm_wm *wm, int id, bool visible)
{
    if (id < 0 || id >= LM_GADGET_COUNT + n_ext || wm->gad[id].visible == visible)
        return;
    wm->gad[id].visible = visible;
    if (visible)
        proc_start(wm, id);
    else
        proc_stop(id);
    lm_settings_changed(wm->srv->settings);                     /* the panel's checkbox follows Super+G / V / S / H */
}

void lm_wm_set_gadget(struct lm_wm *wm, bool visible)
{
    lm_wm_set_gadget_id(wm, LM_GADGET_CLIPBOARD, visible);
}

/* The card's window (global coordinates); false when the program has no window (yet). */
bool lm_wm_gadget_rect_id(const struct lm_wm *wm, int id, struct lm_rect *out)
{
    struct lm_window *w;
    if (id < 0 || id >= LM_GADGET_COUNT + n_ext)
        return false;
    lm_list_for_each(w, &wm->windows, link)
        if (win_is_gadget(w) && w->mapped && !strcmp(w->app_id + 16, gadget_name(id))) {
            *out = lm_rect_make(w->x, w->y, w->width, w->height);
            return true;
        }
    return false;
}

/* The window of a gadget (to give it the keyboard: Super+C). */
struct lm_window *gadget_window(struct lm_wm *wm, int id)
{
    struct lm_window *w;
    lm_list_for_each(w, &wm->windows, link)
        if (win_is_gadget(w) && w->mapped && gadget_rank(w) == id)
            return w;
    return NULL;
}

void damage_panels(struct lm_wm *wm)
{
    if (!wm->srv->backend)
        return;
    struct lm_output *o;
    lm_list_for_each(o, &wm->srv->backend->outputs, link) {
        struct lm_rect r = output_panel_rect(o);
        r.h += LM_PANEL_SHADOW;                         /* (the shadow below it) */
        damage_rect(wm, &r);
    }
    dock_damage(wm);                                    /* (windows come and go here: the dock's "running" dots) */
}
