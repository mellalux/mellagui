/*
 * mellagui-du - what eats the disk: the sizes of a folder tree as a list and as a tree map (the Files button *Disk usage*, Ctrl+U in the file
 * manager, the dock / launcher, or `mellagui-du [DIR]`, default your home folder).
 *
 *   mellagui-du [DIR]                the window
 *   mellagui-du --list [DIR] [N]     print the N biggest things of DIR (default 15) without a window; --apparent counts file sizes, not disk blocks
 *   mellagui-du --selftest
 *
 * The tree is read once in a thread (symbolic links are not followed, a file with several names counts once, by default only the disk DIR is on is
 * read: X switches that), then it is moved around in memory: a click on a tile or a row selects it, a double click / Enter goes into a folder,
 * Backspace / Left / the Up button goes out (from the top of the scan: the parent folder is scanned). A folder keeps its 400 biggest entries and
 * sums the rest in "(N smaller items)". Keys: A apparent size / disk usage, X stay on this disk / cross to others, R scan again, F show in the file
 * manager, C copy the path, Delete deletes the selected thing for good after asking, Esc stops a scan, Q closes.
 */
#define _FILE_OFFSET_BITS 64
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "font.h"
#include "keymap.h"
#include "metrics.h"
#include "protocol.h"
#include "theme.h"
#include "uikit.h"
#include "util.h"
#include "wlc.h"
#include "wlclip.h"
#include "wlwin.h"

#define MAX_KIDS 400
#define MAX_DEPTH 64
#define TOOL_H 32
#define HEAD_H 40
#define STATUS_H 24
#define ROW_H 20
#define PAD 10

#define COL_BG (lm_theme.background)
#define COL_FIELD (lm_theme.field)
#define COL_BAR (lm_theme.bar)
#define COL_TEXT (lm_theme.text_soft)
#define COL_DIM (lm_theme.text_dim)
#define COL_SEL (lm_theme.button_hover)

/* ---- the tree ------------------------------------------------------------------------------------------------------------- */

struct node {
    char *name;
    struct node *parent, **kid;
    int nkid, capkid;
    long long disk, app, items;         /* disk blocks, apparent bytes, things below (a file with several names once) */
    bool dir, other;                    /* other: "(N smaller items)" */
    int errs;                           /* things below that could not be read */
};

static void node_free(struct node *n)
{
    if (!n)
        return;
    for (int i = 0; i < n->nkid; i++)
        node_free(n->kid[i]);
    free(n->kid);
    free(n->name);
    free(n);
}

static struct node *node_new(const char *name, struct node *parent, bool dir)
{
    struct node *n = calloc(1, sizeof(*n));
    if (!n)
        return NULL;
    n->name = strdup(name);
    n->parent = parent;
    n->dir = dir;
    return n;
}

static bool node_add(struct node *p, struct node *k)
{
    if (p->nkid == p->capkid) {
        int nc = p->capkid ? p->capkid * 2 : 8;
        struct node **nk = realloc(p->kid, (size_t)nc * sizeof(*nk));
        if (!nk)
            return false;
        p->kid = nk;
        p->capkid = nc;
    }
    p->kid[p->nkid++] = k;
    return true;
}

static bool g_apparent;                  /* sort and show apparent sizes instead of disk usage */

static long long nsize(const struct node *n)
{
    return g_apparent ? n->app : n->disk;
}

static int cmp_node(const void *x, const void *y)
{
    const struct node *a = *(struct node *const *)x, *b = *(struct node *const *)y;
    long long sa = nsize(a), sb = nsize(b);
    if (a->other != b->other)
        return a->other ? 1 : -1;
    return sa < sb ? 1 : sa > sb ? -1 : strcmp(a->name, b->name);
}

static int cmp_node_disk(const void *x, const void *y)                  /* (the scan thread: always by disk usage) */
{
    const struct node *a = *(struct node *const *)x, *b = *(struct node *const *)y;
    if (a->other != b->other)
        return a->other ? 1 : -1;
    return a->disk < b->disk ? 1 : a->disk > b->disk ? -1 : strcmp(a->name, b->name);
}

static void sort_kids(struct node *n)
{
    if (n->nkid > 1)
        qsort(n->kid, (size_t)n->nkid, sizeof(n->kid[0]), cmp_node);
}

static void node_path(const struct node *n, const char *root, char *out, size_t sz)
{
    if (!n->parent) {
        snprintf(out, sz, "%s", root);
        return;
    }
    node_path(n->parent, root, out, sz);
    size_t l = strlen(out);
    snprintf(out + l, sz - l, "%s%s", l && out[l - 1] == '/' ? "" : "/", n->name);
}

/* ---- scanning -------------------------------------------------------------------------------------------------------------- */

struct inoset {
    struct { dev_t dev; ino_t ino; } *e;
    size_t cap, n;
};

static bool inoset_seen(struct inoset *s, dev_t dev, ino_t ino)                    /* inserts; true when it was there already */
{
    if (s->n * 2 >= s->cap) {
        size_t nc = s->cap ? s->cap * 2 : 1024;
        typeof(s->e) ne = calloc(nc, sizeof(*ne));
        if (!ne)
            return false;
        for (size_t i = 0; i < s->cap; i++)
            if (s->e[i].ino) {
                size_t h = (size_t)(s->e[i].ino * 2654435761u + s->e[i].dev) % nc;
                while (ne[h].ino)
                    h = (h + 1) % nc;
                ne[h] = s->e[i];
            }
        free(s->e);
        s->e = ne;
        s->cap = nc;
    }
    size_t h = (size_t)(ino * 2654435761u + dev) % s->cap;
    while (s->e[h].ino) {
        if (s->e[h].ino == ino && s->e[h].dev == dev)
            return true;
        h = (h + 1) % s->cap;
    }
    s->e[h].dev = dev;
    s->e[h].ino = ino;
    s->n++;
    return false;
}

struct scan {
    char path[PATH_MAX];
    bool one_fs;
    dev_t dev;
    struct inoset seen;
    _Atomic bool cancel, done;
    _Atomic long long disk, items, errs;
    pthread_mutex_t mu;
    char cur[PATH_MAX];
    struct node *root;
    pthread_t th;
    bool threaded;
};

static bool skip_pseudo(const char *path)
{
    struct statfs sf;
    if (statfs(path, &sf) != 0)
        return false;
    return sf.f_type == 0x9fa0 /* proc */ || sf.f_type == 0x62656572 /* sysfs */ || sf.f_type == 0x27e0eb /* cgroup */ || sf.f_type == 0x63677270 /* cgroup2 */;
}

static struct node *scan_dir(struct scan *sc, const char *path, struct node *parent, const char *name, const struct stat *dst, int depth)
{
    struct node *n = node_new(name, parent, true);
    if (!n)
        return NULL;
    n->disk = (long long)dst->st_blocks * 512;
    n->app = dst->st_size;
    if (depth > MAX_DEPTH || atomic_load(&sc->cancel))
        return n;
    DIR *d = opendir(path);
    if (!d) {
        n->errs++;
        atomic_fetch_add(&sc->errs, 1);
        return n;
    }
    pthread_mutex_lock(&sc->mu);
    snprintf(sc->cur, sizeof(sc->cur), "%s", path);
    pthread_mutex_unlock(&sc->mu);
    struct dirent *de;
    while ((de = readdir(d)) && !atomic_load(&sc->cancel)) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        char sub[PATH_MAX];
        if (snprintf(sub, sizeof(sub), "%s%s%s", path, !strcmp(path, "/") ? "" : "/", de->d_name) >= (int)sizeof(sub))
            continue;
        struct stat st;
        if (lstat(sub, &st) < 0) {
            n->errs++;
            atomic_fetch_add(&sc->errs, 1);
            continue;
        }
        struct node *k = NULL;
        if (S_ISDIR(st.st_mode)) {
            if ((sc->one_fs && st.st_dev != sc->dev) || skip_pseudo(sub))
                continue;                                               /* another disk (or /proc): not part of this one */
            k = scan_dir(sc, sub, n, de->d_name, &st, depth + 1);
        } else {
            k = node_new(de->d_name, n, false);
            if (!k)
                continue;
            bool dup = st.st_nlink > 1 && S_ISREG(st.st_mode) && inoset_seen(&sc->seen, st.st_dev, st.st_ino);
            if (!dup) {
                k->disk = (long long)st.st_blocks * 512;
                k->app = st.st_size;
                k->items = 1;
                atomic_fetch_add(&sc->items, 1);
                atomic_fetch_add(&sc->disk, k->disk);
            }
        }
        if (!k)
            continue;
        if (!node_add(n, k)) {
            node_free(k);
            continue;
        }
    }
    closedir(d);
    for (int i = 0; i < n->nkid; i++) {                                 /* totals */
        n->disk += n->kid[i]->disk;
        n->app += n->kid[i]->app;
        n->items += n->kid[i]->items;
        n->errs += n->kid[i]->errs;
    }
    atomic_fetch_add(&sc->disk, (long long)dst->st_blocks * 512);
    if (n->nkid > 1)                                                    /* the pruning keeps the biggest by disk usage */
        qsort(n->kid, (size_t)n->nkid, sizeof(n->kid[0]), cmp_node_disk);
    if (n->nkid > MAX_KIDS) {                                           /* the small ones are summed up */
        struct node *o = node_new("", n, false);
        int cut = MAX_KIDS - 1, gone = n->nkid - cut;
        if (o) {
            o->other = true;
            for (int i = cut; i < n->nkid; i++) {
                o->disk += n->kid[i]->disk;
                o->app += n->kid[i]->app;
                o->items += n->kid[i]->items;
                node_free(n->kid[i]);
            }
            char label[48];
            snprintf(label, sizeof(label), "(%d smaller items)", gone);
            free(o->name);
            o->name = strdup(label);
            n->nkid = cut;
            node_add(n, o);
        }
    }
    return n;
}

static void scan_run(struct scan *sc)
{
    struct stat st;
    if (lstat(sc->path, &st) < 0) {
        atomic_fetch_add(&sc->errs, 1);
        sc->root = node_new(sc->path, NULL, true);
        return;
    }
    sc->dev = st.st_dev;
    sc->root = scan_dir(sc, sc->path, NULL, sc->path, &st, 0);
}

static void *scan_thread(void *arg)
{
    struct scan *sc = arg;
    scan_run(sc);
    atomic_store(&sc->done, true);
    return NULL;
}

static void scan_free(struct scan *sc)
{
    if (!sc)
        return;
    if (sc->threaded) {
        atomic_store(&sc->cancel, true);
        pthread_join(sc->th, NULL);
    }
    node_free(sc->root);
    free(sc->seen.e);
    pthread_mutex_destroy(&sc->mu);
    free(sc);
}

static struct scan *scan_start(const char *path, bool one_fs, bool threaded)
{
    struct scan *sc = calloc(1, sizeof(*sc));
    if (!sc)
        return NULL;
    snprintf(sc->path, sizeof(sc->path), "%s", path);
    size_t l = strlen(sc->path);
    while (l > 1 && sc->path[l - 1] == '/')
        sc->path[--l] = 0;
    sc->one_fs = one_fs;
    pthread_mutex_init(&sc->mu, NULL);
    if (threaded) {
        sc->threaded = true;
        if (pthread_create(&sc->th, NULL, scan_thread, sc) != 0) {
            sc->threaded = false;
            scan_run(sc);
            atomic_store(&sc->done, true);
        }
    } else {
        scan_run(sc);
        atomic_store(&sc->done, true);
    }
    return sc;
}

/* ---- the tree map ---------------------------------------------------------------------------------------------------------- */

struct trect {
    int x, y, w, h;
};

static double worst_ratio(const double *v, int from, int to, double side)
{
    double sum = 0, mx = 0, mn = 1e300;
    for (int i = from; i < to; i++) {
        sum += v[i];
        mx = v[i] > mx ? v[i] : mx;
        mn = v[i] < mn ? v[i] : mn;
    }
    if (sum <= 0 || mn <= 0)
        return 1e300;
    double s2 = side * side, sum2 = sum * sum;
    return fmax(s2 * mx / sum2, sum2 / (s2 * mn));
}

/* Squarified tree map (Bruls, Huizing, van Wijk): `v` is sorted from the biggest, every tile's area is proportional to its value, the tiles fill
 * `area` exactly (integer edges that meet). Entries with value 0 get an empty tile. */
static void treemap(const double *v, int n, struct trect area, struct trect *out)
{
    double total = 0;
    for (int i = 0; i < n; i++)
        total += v[i] > 0 ? v[i] : 0;
    for (int i = 0; i < n; i++)
        out[i] = (struct trect){ area.x, area.y, 0, 0 };
    if (total <= 0 || area.w <= 0 || area.h <= 0)
        return;
    double scale = (double)area.w * (double)area.h / total;
    double *a = malloc((size_t)n * sizeof(double));
    if (!a)
        return;
    for (int i = 0; i < n; i++)
        a[i] = v[i] > 0 ? v[i] * scale : 0;
    double rx = area.x, ry = area.y, rw = area.w, rh = area.h;
    int i = 0;
    while (i < n && a[i] > 0) {
        double side = fmin(rw, rh);
        int j = i + 1;
        while (j < n && a[j] > 0 && worst_ratio(a, i, j + 1, side) <= worst_ratio(a, i, j, side))
            j++;
        double sum = 0;
        for (int k = i; k < j; k++)
            sum += a[k];
        if (rw >= rh) {                                                 /* a vertical strip at the left */
            double sw = rh > 0 ? sum / rh : 0, y = ry;
            for (int k = i; k < j; k++) {
                double h = sw > 0 ? a[k] / sw : 0;
                out[k] = (struct trect){ (int)lround(rx), (int)lround(y), 0, 0 };
                out[k].w = (int)lround(rx + sw) - out[k].x;
                out[k].h = (int)lround(y + h) - out[k].y;
                y += h;
            }
            rx += sw;
            rw -= sw;
        } else {                                                        /* a horizontal strip at the top */
            double sh = rw > 0 ? sum / rw : 0, x = rx;
            for (int k = i; k < j; k++) {
                double w = sh > 0 ? a[k] / sh : 0;
                out[k] = (struct trect){ (int)lround(x), (int)lround(ry), 0, 0 };
                out[k].w = (int)lround(x + w) - out[k].x;
                out[k].h = (int)lround(ry + sh) - out[k].y;
                x += w;
            }
            ry += sh;
            rh -= sh;
        }
        i = j;
    }
    free(a);
    /* the last tile of each strip may reach to the edge of the area because of rounding: nothing to fix, edges are rounded consistently */
}

/* ---- the program ----------------------------------------------------------------------------------------------------------- */

enum confirm { C_NONE, C_DELETE };

struct app {
    struct wlwin win;
    struct wlclip clip;
    struct scan *sc;                    /* the scan that runs (NULL when none) */
    struct node *root;                  /* the finished tree (owned by `held`) */
    struct scan *held;
    char root_path[PATH_MAX];
    struct node *cur;                   /* the folder shown */
    int sel;
    bool one_fs;
    struct trect *tiles;                /* of cur's children, parallel to cur->kid */
    int ntiles;
    struct trect map;                   /* the tree map's area */
    int top;                            /* first row of the list */
    double px, py;
    int hover;                          /* the child under the pointer (tile or row), -1 none */
    int hovertool;
    enum confirm confirm;
    char msg[240];
    int64_t msg_until;
    uint32_t last_click_ms;
    int last_click_row;
    long long fs_used, fs_total;        /* the disk of the scan root */
    int menu_for;
};

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void msg(struct app *a, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void msg(struct app *a, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(a->msg, sizeof(a->msg), fmt, ap);
    va_end(ap);
    a->msg_until = now_ms() + 6000;
    a->win.redraw = true;
}

static void human(long long b, char *out, size_t n)
{
    static const char *const u[] = { "B", "K", "M", "G", "T", "P" };
    double v = (double)b;
    int i = 0;
    while (v >= 1024.0 && i < 5) {
        v /= 1024.0;
        i++;
    }
    if (i == 0)
        snprintf(out, n, "%lld B", b);
    else
        snprintf(out, n, v >= 100 ? "%.0f %s" : v >= 10 ? "%.1f %s" : "%.2f %s", v, u[i]);
}

static const struct node *nth(const struct app *a, int i)
{
    return a->cur && i >= 0 && i < a->cur->nkid ? a->cur->kid[i] : NULL;
}

static int list_rows(const struct app *a)
{
    return LM_MAX((a->win.bh - TOOL_H - HEAD_H - STATUS_H) / ROW_H, 1);
}

static int list_width(const struct app *a)
{
    return LM_CLAMP(a->win.bw * 2 / 5, 260, 420);
}

static void set_title(struct app *a)
{
    if (!a->win.toplevel)
        return;
    char t[PATH_MAX + 32];
    const char *nm = a->cur ? a->cur->name : "";
    const char *base = strrchr(nm, '/') && strlen(strrchr(nm, '/')) > 1 ? strrchr(nm, '/') + 1 : nm;
    snprintf(t, sizeof(t), "%s - Disk usage", base);
    WLC_SEND(a->win.c, a->win.toplevel, LM_XDG_TOPLEVEL_REQ_SET_TITLE, { .s = t });
}

static void relayout(struct app *a)
{
    free(a->tiles);
    a->tiles = NULL;
    a->ntiles = 0;
    if (!a->cur || a->cur->nkid == 0)
        return;
    int lw = list_width(a);
    a->map = (struct trect){ lw + 8, TOOL_H + HEAD_H, a->win.bw - lw - 8 - PAD, a->win.bh - TOOL_H - HEAD_H - STATUS_H - 4 };
    int n = a->cur->nkid;
    double *v = malloc((size_t)n * sizeof(double));
    a->tiles = malloc((size_t)n * sizeof(struct trect));
    if (!v || !a->tiles) {
        free(v);
        free(a->tiles);
        a->tiles = NULL;
        return;
    }
    for (int i = 0; i < n; i++)
        v[i] = (double)nsize(a->cur->kid[i]);
    treemap(v, n, a->map, a->tiles);
    a->ntiles = n;
    free(v);
}

static void show(struct app *a, struct node *n)
{
    a->cur = n;
    a->sel = 0;
    a->top = 0;
    a->hover = -1;
    if (n)
        sort_kids(n);
    relayout(a);
    set_title(a);
    a->win.redraw = true;
}

static void fs_info(struct app *a)
{
    struct statvfs sv;
    a->fs_used = a->fs_total = 0;
    if (statvfs(a->root_path, &sv) == 0) {
        a->fs_total = (long long)sv.f_blocks * (long long)sv.f_frsize;
        a->fs_used = (long long)(sv.f_blocks - sv.f_bfree) * (long long)sv.f_frsize;
    }
}

static void begin_scan(struct app *a, const char *path)
{
    if (a->sc) {
        scan_free(a->sc);
        a->sc = NULL;
    }
    snprintf(a->root_path, sizeof(a->root_path), "%s", path);
    a->sc = scan_start(path, a->one_fs, true);
    fs_info(a);
    a->win.redraw = true;
}

/* A finished scan becomes the tree that is shown. */
static void take_scan(struct app *a)
{
    struct scan *sc = a->sc;
    a->sc = NULL;
    if (sc->threaded) {
        pthread_join(sc->th, NULL);
        sc->threaded = false;
    }
    if (atomic_load(&sc->cancel) || !sc->root) {
        scan_free(sc);
        msg(a, "Scan stopped");
        return;
    }
    scan_free(a->held);
    a->held = sc;
    a->root = sc->root;
    show(a, a->root);
    if (sc->errs > 0)
        msg(a, "%lld things could not be read (no permission?)", (long long)atomic_load(&sc->errs));
}

static void go_into(struct app *a, int i)
{
    const struct node *k = nth(a, i);
    if (k && k->dir && k->nkid > 0)
        show(a, a->cur->kid[i]);
    else if (k && k->dir)
        msg(a, "%s is empty (or not read)", k->name);
}

static void go_up(struct app *a)
{
    if (a->cur && a->cur->parent) {
        struct node *from = a->cur;
        show(a, a->cur->parent);
        for (int i = 0; i < a->cur->nkid; i++)
            if (a->cur->kid[i] == from)
                a->sel = i;
        a->top = LM_MAX(a->sel - list_rows(a) + 1, 0);
    } else if (strcmp(a->root_path, "/") != 0) {                        /* from the top of the scan: the folder above is scanned */
        char parent[PATH_MAX];
        snprintf(parent, sizeof(parent), "%s", a->root_path);
        char *sl = strrchr(parent, '/');
        if (sl == parent)
            parent[1] = 0;
        else if (sl)
            *sl = 0;
        begin_scan(a, parent);
    }
}

static void select_row(struct app *a, int i)
{
    if (!a->cur)
        return;
    a->sel = LM_CLAMP(i, 0, LM_MAX(a->cur->nkid - 1, 0));
    if (a->sel < a->top)
        a->top = a->sel;
    if (a->sel >= a->top + list_rows(a))
        a->top = a->sel - list_rows(a) + 1;
    a->win.redraw = true;
}

static void sel_path(struct app *a, char *out, size_t n)
{
    const struct node *k = nth(a, a->sel);
    node_path(k && !k->other ? k : a->cur, a->root_path, out, n);
}

static void show_in_files(struct app *a)
{
    char path[PATH_MAX];
    const struct node *k = nth(a, a->sel);
    node_path(k && !k->other ? k : a->cur, a->root_path, path, sizeof(path));
    if (k && !k->dir) {                                                 /* a file: its folder */
        char *sl = strrchr(path, '/');
        if (sl && sl != path)
            *sl = 0;
    }
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        char exe[PATH_MAX];
        ssize_t l = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        char *prog = "mellagui-files";
        if (l > 0) {
            exe[l] = 0;
            char *sl = strrchr(exe, '/');
            if (sl) {
                snprintf(sl + 1, sizeof(exe) - (size_t)(sl + 1 - exe), "mellagui-files");
                if (access(exe, X_OK) == 0)
                    prog = exe;
            }
        }
        execlp(prog, prog, path, (char *)NULL);
        _exit(127);
    }
    msg(a, pid > 0 ? "Opened %s in the file manager" : "Could not start the file manager", path);
}

/* ---- deleting -------------------------------------------------------------------------------------------------------------- */

static int remove_tree(const char *path, int depth)
{
    struct stat st;
    if (depth > MAX_DEPTH || lstat(path, &st) < 0)
        return -1;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (!d)
            return -1;
        struct dirent *de;
        int rc = 0;
        while ((de = readdir(d)) && rc == 0) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
                continue;
            char sub[PATH_MAX];
            if (snprintf(sub, sizeof(sub), "%s/%s", path, de->d_name) >= (int)sizeof(sub))
                continue;
            rc = remove_tree(sub, depth + 1);
        }
        closedir(d);
        return rc == 0 ? rmdir(path) : rc;
    }
    return unlink(path);
}

static void do_delete(struct app *a)
{
    const struct node *k = nth(a, a->sel);
    if (!k || k->other) {
        msg(a, "Select a file or folder");
        return;
    }
    char path[PATH_MAX];
    node_path(k, a->root_path, path, sizeof(path));
    if (!strcmp(path, "/") || !strcmp(path, a->root_path)) {
        msg(a, "That one is not deleted from here");
        return;
    }
    if (remove_tree(path, 0) < 0) {
        msg(a, "Could not delete %s: %s", k->name, strerror(errno));
        return;
    }
    struct node *victim = a->cur->kid[a->sel];
    for (struct node *p = a->cur; p; p = p->parent) {                   /* the tree follows: the sizes above shrink */
        p->disk -= victim->disk;
        p->app -= victim->app;
        p->items -= victim->items;
    }
    char nm[NAME_MAX + 1];
    snprintf(nm, sizeof(nm), "%s", victim->name);
    node_free(victim);
    memmove(&a->cur->kid[a->sel], &a->cur->kid[a->sel + 1], (size_t)(a->cur->nkid - a->sel - 1) * sizeof(a->cur->kid[0]));
    a->cur->nkid--;
    relayout(a);
    select_row(a, a->sel);
    msg(a, "Deleted %s", nm);
}

/* ---- buttons --------------------------------------------------------------------------------------------------------------- */

enum btn_id { B_UP, B_HOME, B_ROOT, B_RESCAN, B_FS, B_APP, B_FILES, B_DELETE, N_BTN };

struct btn {
    enum btn_id id;
    const char *label;
    int x, w;
    uint32_t base, hover;
    bool on, enabled;
};

static int build_buttons(const struct app *a, struct btn *b)
{
    bool have = a->cur && nth(a, a->sel) && !nth(a, a->sel)->other;
    struct { enum btn_id id; const char *l; uint32_t base, hover; bool on, en; } d[N_BTN] = {
        { B_UP, "Up", 0x3d6fb4, 0x5588d0, false, a->cur != NULL },
        { B_HOME, "Home", 0x3f9a5e, 0x56b878, false, true },
        { B_ROOT, "/", 0x8a6fb8, 0xa088d0, false, true },
        { B_RESCAN, "Rescan", 0x2f8f9a, 0x45aab5, false, true },
        { B_FS, a->one_fs ? "This disk" : "All disks", 0x6b6f7a, 0x868b98, !a->one_fs, true },
        { B_APP, g_apparent ? "Apparent" : "On disk", 0x6b6f7a, 0x868b98, g_apparent, true },
        { B_FILES, "Files", 0xb8803a, 0xd49a50, false, a->cur != NULL },
        { B_DELETE, "Delete", 0xb84545, 0xd65f5f, false, have },
    };
    int x = PAD;
    for (int i = 0; i < N_BTN; i++) {
        b[i].id = d[i].id;
        b[i].label = d[i].l;
        b[i].w = (int)strlen(d[i].l) * LM_FONT_W + 22;
        b[i].x = x;
        b[i].base = d[i].on ? d[i].hover : d[i].base;
        b[i].hover = d[i].on ? lm_mix_rgb(d[i].hover, 0xffffff, 0.25) : d[i].hover;
        b[i].on = d[i].on;
        b[i].enabled = d[i].en;
        x += b[i].w + 6;
    }
    return N_BTN;
}

static int button_at(const struct app *a, double px, double py)
{
    if (py < 4 || py >= TOOL_H - 4)
        return -1;
    struct btn b[N_BTN];
    build_buttons(a, b);
    for (int i = 0; i < N_BTN; i++)
        if (px >= b[i].x && px < b[i].x + b[i].w)
            return i;
    return -1;
}

static void press_button(struct app *a, enum btn_id id)
{
    switch (id) {
    case B_UP: go_up(a); break;
    case B_HOME: begin_scan(a, getenv("HOME") && *getenv("HOME") ? getenv("HOME") : "/"); break;
    case B_ROOT: begin_scan(a, "/"); break;
    case B_RESCAN: begin_scan(a, a->root_path); break;
    case B_FS:
        a->one_fs = !a->one_fs;
        begin_scan(a, a->root_path);
        break;
    case B_APP:
        g_apparent = !g_apparent;
        if (a->cur)
            show(a, a->cur);
        break;
    case B_FILES: show_in_files(a); break;
    case B_DELETE:
        if (a->cur && nth(a, a->sel) && !nth(a, a->sel)->other)
            a->confirm = C_DELETE;
        break;
    default: break;
    }
    a->win.redraw = true;
}

/* ---- painting -------------------------------------------------------------------------------------------------------------- */

static uint32_t tile_colour(const struct node *k, int i)
{
    static const uint32_t pal[] = { 0x3d6fb4, 0x3f9a5e, 0x8a6fb8, 0x2f8f9a, 0xb8803a, 0xb84545, 0x7a9a2f, 0x4a7fa8 };
    if (k->other)
        return 0x555b66;
    uint32_t c = pal[i % 8];
    return k->dir ? c : lm_mix_rgb(c, 0x2a2f3a, 0.55);
}

static void paint_head(struct app *a, struct wlwin *w)
{
    int y = TOOL_H;
    wlwin_fill(w, 0, y, w->bw, HEAD_H, COL_BG);
    char path[PATH_MAX], t[200], sz[24], tot[24], used[24];
    if (a->sc) {
        pthread_mutex_lock(&a->sc->mu);
        snprintf(path, sizeof(path), "%s", a->sc->cur);
        pthread_mutex_unlock(&a->sc->mu);
        human(atomic_load(&a->sc->disk), sz, sizeof(sz));
        snprintf(t, sizeof(t), "Scanning %.100s ...   %s read, %lld things   Esc stops", a->root_path, sz, (long long)atomic_load(&a->sc->items));
        wlwin_text(w, PAD, y + 4, t, 1, lm_theme.accent, w->bw - 2 * PAD);
        wlwin_text(w, PAD, y + 22, path, 1, COL_DIM, w->bw - 2 * PAD);
        return;
    }
    if (!a->cur) {
        wlwin_text(w, PAD, y + 12, "Nothing scanned.", 1, COL_DIM, 0);
        return;
    }
    node_path(a->cur, a->root_path, path, sizeof(path));
    wlwin_text(w, PAD, y + 4, path, 1, lm_theme.accent, w->bw - 2 * PAD);
    human(nsize(a->cur), sz, sizeof(sz));
    human(a->fs_total, tot, sizeof(tot));
    human(a->fs_used, used, sizeof(used));
    snprintf(t, sizeof(t), "%s in %lld things (%s)%s   the disk: %s used of %s%s", sz, a->cur->items, g_apparent ? "apparent size" : "disk usage",
             a->cur->errs ? "   some could not be read" : "", used, tot, a->one_fs ? "" : "   (all disks below)");
    wlwin_text(w, PAD, y + 22, t, 1, COL_DIM, w->bw - 2 * PAD);
}

static void draw_rbtn(struct wlwin *w, const struct btn *b, bool hot)
{
    int y = 4, h = TOOL_H - 8;
    uint32_t base = b->enabled ? (hot ? b->hover : b->base) : lm_mix_rgb(b->base, COL_BG, 0.6);
    lm_round_fill(w->pixels, w->bw, w->bw, w->bh, b->x, y, b->w, h, h / 2, base);
    int tw = (int)strlen(b->label) * LM_FONT_W;
    wlwin_text(w, b->x + (b->w - tw) / 2, y + (h - LM_FONT_H) / 2, b->label, 1, b->enabled ? 0xffffff : COL_DIM, b->w - 4);
}

static void paint_list(struct app *a, struct wlwin *w)
{
    int lw = list_width(a), y0 = TOOL_H + HEAD_H, rows = list_rows(a);
    wlwin_fill(w, 0, y0, lw, rows * ROW_H, COL_FIELD);
    if (!a->cur)
        return;
    long long total = LM_MAX(nsize(a->cur), 1);
    int namew = lw - PAD - 9 * LM_FONT_W - 4 * LM_FONT_W - 70;
    for (int r = 0; r < rows && a->top + r < a->cur->nkid; r++) {
        int i = a->top + r;
        const struct node *k = a->cur->kid[i];
        int y = y0 + r * ROW_H;
        if (i == a->sel)
            wlwin_fill(w, 0, y, lw, ROW_H, COL_SEL);
        else if (i == a->hover)
            wlwin_fill(w, 0, y, lw, ROW_H, COL_BAR);
        wlwin_fill(w, 4, y + 3, 6, ROW_H - 6, tile_colour(k, i));
        char nm[NAME_MAX + 4], sz[24], pc[16];
        snprintf(nm, sizeof(nm), "%s%s", k->name, k->dir ? "/" : "");
        wlwin_text(w, 16, y + 2, nm, 1, k->dir ? lm_theme.accent : COL_TEXT, namew);
        int bw = 56, bx = 16 + namew + 6;
        wlwin_fill(w, bx, y + 7, bw, 6, COL_BG);
        wlwin_fill(w, bx, y + 7, (int)((long long)bw * nsize(k) / total), 6, tile_colour(k, i));
        human(nsize(k), sz, sizeof(sz));
        wlwin_text(w, bx + bw + 6, y + 2, sz, 1, COL_TEXT, 0);
        snprintf(pc, sizeof(pc), "%3d%%", (int)(100 * nsize(k) / total));
        wlwin_text(w, lw - 4 * LM_FONT_W - 6, y + 2, pc, 1, COL_DIM, 0);
    }
    if (a->cur->nkid > rows) {                                          /* a thin scroll bar */
        int th = LM_MAX(rows * ROW_H * rows / a->cur->nkid, 14), ty = y0 + (rows * ROW_H - th) * a->top / LM_MAX(a->cur->nkid - rows, 1);
        wlwin_fill(w, lw - 4, y0, 4, rows * ROW_H, COL_BAR);
        wlwin_fill(w, lw - 4, ty, 4, th, lm_theme.button);
    }
}

static void paint_map(struct app *a, struct wlwin *w)
{
    wlwin_fill(w, a->map.x, a->map.y, a->map.w, a->map.h, COL_FIELD);
    if (!a->cur || !a->tiles)
        return;
    for (int i = 0; i < a->ntiles; i++) {
        const struct trect *t = &a->tiles[i];
        if (t->w < 1 || t->h < 1)
            continue;
        const struct node *k = a->cur->kid[i];
        uint32_t c = tile_colour(k, i);
        if (i == a->hover)
            c = lm_mix_rgb(c, 0xffffff, 0.2);
        wlwin_fill(w, t->x, t->y, t->w, t->h, c);
        wlwin_fill(w, t->x, t->y, t->w, 1, lm_mix_rgb(c, 0x000000, 0.45));                 /* a dark edge on two sides separates the tiles */
        wlwin_fill(w, t->x, t->y, 1, t->h, lm_mix_rgb(c, 0x000000, 0.45));
        if (i == a->sel) {
            wlwin_fill(w, t->x, t->y, t->w, 2, 0xffffff);
            wlwin_fill(w, t->x, t->y + t->h - 2, t->w, 2, 0xffffff);
            wlwin_fill(w, t->x, t->y, 2, t->h, 0xffffff);
            wlwin_fill(w, t->x + t->w - 2, t->y, 2, t->h, 0xffffff);
        }
        if (t->w >= 7 * LM_FONT_W && t->h >= 2 * LM_FONT_H + 4) {
            char sz[24];
            human(nsize(k), sz, sizeof(sz));
            wlwin_text(w, t->x + 5, t->y + 4, k->name, 1, 0xffffff, t->w - 8);
            wlwin_text(w, t->x + 5, t->y + 4 + LM_FONT_H, sz, 1, lm_mix_rgb(0xffffff, c, 0.35), t->w - 8);
        } else if (t->w >= 5 * LM_FONT_W && t->h >= LM_FONT_H + 2) {
            wlwin_text(w, t->x + 3, t->y + 2, k->name, 1, 0xffffff, t->w - 5);
        }
    }
}

static void paint(struct wlwin *w)
{
    struct app *a = w->data;
    wlwin_fill(w, 0, 0, w->bw, w->bh, COL_BG);
    struct btn b[N_BTN];
    int n = build_buttons(a, b);
    for (int i = 0; i < n; i++)
        draw_rbtn(w, &b[i], a->hovertool == i);
    paint_head(a, w);
    if (a->sc) {                                                        /* the progress against what the disk holds */
        int bw = w->bw - 2 * PAD, y = TOOL_H + HEAD_H + 10;
        wlwin_fill(w, PAD, y, bw, 12, COL_FIELD);
        long long done = atomic_load(&a->sc->disk), tot = a->fs_used > 0 ? a->fs_used : 0;
        int fillw = tot > 0 ? (int)LM_MIN((long long)bw * done / tot, bw) : 0;
        wlwin_fill(w, PAD, y, fillw, 12, lm_theme.accent);
        wlwin_text(w, PAD, y + 22, "A folder is read once; then everything can be moved around at once.", 1, COL_DIM, bw);
    } else {
        paint_list(a, w);
        paint_map(a, w);
    }
    int sy = w->bh - STATUS_H;
    wlwin_fill(w, 0, sy, w->bw, STATUS_H, COL_BAR);
    char line[400];
    const struct node *k = nth(a, a->sel);
    if (a->confirm == C_DELETE && k) {
        char sz[24];
        human(nsize(k), sz, sizeof(sz));
        snprintf(line, sizeof(line), "Delete %s%s (%s) for good? This cannot be undone: Enter deletes, Esc cancels", k->name, k->dir ? "/" : "", sz);
        wlwin_text(w, PAD, sy + 4, line, 1, lm_theme.error, w->bw - 2 * PAD);
    } else if (a->msg[0] && now_ms() < a->msg_until) {
        wlwin_text(w, PAD, sy + 4, a->msg, 1, lm_theme.caution, w->bw - 2 * PAD);
    } else {
        const struct node *hv = nth(a, a->hover >= 0 ? a->hover : a->sel);
        if (hv) {
            char p[PATH_MAX], sz[24];
            node_path(hv, a->root_path, p, sizeof(p));
            human(nsize(hv), sz, sizeof(sz));
            snprintf(line, sizeof(line), "%.300s   %s", hv->other ? hv->name : p, sz);
            wlwin_text(w, PAD, sy + 4, line, 1, COL_TEXT, w->bw - 2 * PAD);
        } else {
            wlwin_text(w, PAD, sy + 4, "Enter / double click goes in   Backspace out   A apparent size   X disks   R rescan   F files   Delete deletes", 1, COL_DIM,
                       w->bw - 2 * PAD);
        }
    }
}

/* ---- input ----------------------------------------------------------------------------------------------------------------- */

static int child_at(const struct app *a, double px, double py)
{
    if (a->sc || !a->cur)
        return -1;
    int lw = list_width(a), y0 = TOOL_H + HEAD_H;
    if (px < lw && py >= y0 && py < y0 + list_rows(a) * ROW_H) {
        int i = a->top + (int)((py - y0) / ROW_H);
        return i < a->cur->nkid ? i : -1;
    }
    for (int i = 0; i < a->ntiles; i++) {
        const struct trect *t = &a->tiles[i];
        if (px >= t->x && px < t->x + t->w && py >= t->y && py < t->y + t->h)
            return i;
    }
    return -1;
}

static void menu_event(struct wlwin *w, int index)
{
    struct app *a = w->data;
    if (index < 0 || !a->cur)
        return;
    if (index == 0) {
        go_into(a, a->sel);
    } else if (index == 1) {
        show_in_files(a);
    } else if (index == 2) {
        char p[PATH_MAX];
        sel_path(a, p, sizeof(p));
        wlclip_copy(&a->clip, p);
        msg(a, "Copied the path");
    } else if (index == 3) {
        press_button(a, B_DELETE);
    }
    w->redraw = true;
}

static void pointer_event(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
    struct app *a = w->data;
    switch (op) {
    case LM_WL_POINTER_EVT_ENTER:
        a->px = lm_fixed_to_double(args[2].f);
        a->py = lm_fixed_to_double(args[3].f);
        break;
    case LM_WL_POINTER_EVT_LEAVE:
        a->hover = a->hovertool = -1;
        w->redraw = true;
        break;
    case LM_WL_POINTER_EVT_MOTION: {
        a->px = lm_fixed_to_double(args[1].f);
        a->py = lm_fixed_to_double(args[2].f);
        int h = button_at(a, a->px, a->py), c = child_at(a, a->px, a->py);
        if (h != a->hovertool || c != a->hover) {
            a->hovertool = h;
            a->hover = c;
            w->redraw = true;
        }
        break;
    }
    case LM_WL_POINTER_EVT_BUTTON: {
        a->clip.serial = args[0].u;
        if (args[3].u != 1)
            break;
        a->confirm = C_NONE;
        bool left = args[2].u == 272, right = args[2].u == 273;
        int bi = button_at(a, a->px, a->py);
        if (left && bi >= 0) {
            struct btn b[N_BTN];
            build_buttons(a, b);
            if (b[bi].enabled)
                press_button(a, b[bi].id);
            break;
        }
        int c = child_at(a, a->px, a->py);
        if ((left || right) && c >= 0) {
            select_row(a, c);
            uint32_t t = args[1].u;
            if (left && t - a->last_click_ms < (uint32_t)lm_metrics.double_click_ms && a->last_click_row == c) {
                go_into(a, c);
                a->last_click_ms = 0;
            } else {
                a->last_click_ms = t;
                a->last_click_row = c;
            }
            if (right) {
                const struct node *k = nth(a, c);
                wlwin_menu(w, k && k->dir ? "Open\nShow in Files\nCopy path\nDelete..." : "~Open\nShow in Files\nCopy path\nDelete...");
            }
        }
        break;
    }
    case LM_WL_POINTER_EVT_AXIS:
        if (args[1].u == 0 && a->cur) {
            double v = lm_fixed_to_double(args[2].f);
            a->top = LM_CLAMP(a->top + (v > 0 ? 3 : v < 0 ? -3 : 0), 0, LM_MAX(a->cur->nkid - list_rows(a), 0));
            w->redraw = true;
        }
        break;
    }
}

static void keyboard_event(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
    struct app *a = w->data;
    if (op != LM_WL_KEYBOARD_EVT_KEY)
        return;
    a->clip.serial = args[0].u;
    if (args[3].u == 0)
        return;
    uint32_t ks = lm_keymap_keysym(args[2].u, w->mods);
    bool ctrl = (w->mods & LM_MOD_CTRL) != 0, alt = (w->mods & LM_MOD_ALT) != 0;
    char txt[8];
    int tl = lm_keysym_to_utf8(ks, txt);
    char c = tl == 1 && !ctrl && !alt ? txt[0] : 0;
    w->redraw = true;
    if (a->confirm == C_DELETE) {
        if (ks == LM_KS_Return || ks == LM_KS_KP_Enter || c == 'y' || c == 'Y') {
            a->confirm = C_NONE;
            do_delete(a);
        } else {
            a->confirm = C_NONE;
        }
        return;
    }
    if (ks == LM_KS_Escape) {
        if (a->sc)
            atomic_store(&a->sc->cancel, true);
        else
            w->running = false;
        return;
    }
    int page = LM_MAX(list_rows(a) - 1, 1);
    switch (ks) {
    case LM_KS_Up: if (alt) go_up(a); else select_row(a, a->sel - 1); return;
    case LM_KS_Down: select_row(a, a->sel + 1); return;
    case LM_KS_Page_Up: select_row(a, a->sel - page); return;
    case LM_KS_Page_Down: select_row(a, a->sel + page); return;
    case LM_KS_Home: select_row(a, 0); return;
    case LM_KS_End: select_row(a, a->cur ? a->cur->nkid - 1 : 0); return;
    case LM_KS_Return:
    case LM_KS_KP_Enter:
    case LM_KS_Right: go_into(a, a->sel); return;
    case LM_KS_BackSpace:
    case LM_KS_Left: go_up(a); return;
    case LM_KS_Delete: press_button(a, B_DELETE); return;
    default: break;
    }
    if (ctrl)
        return;
    switch (c) {
    case 'q': case 'Q': w->running = false; break;
    case 'a': case 'A': press_button(a, B_APP); break;
    case 'x': case 'X': press_button(a, B_FS); break;
    case 'r': case 'R': press_button(a, B_RESCAN); break;
    case 'f': case 'F': show_in_files(a); break;
    case 'c': case 'C': {
        char p[PATH_MAX];
        sel_path(a, p, sizeof(p));
        wlclip_copy(&a->clip, p);
        msg(a, "Copied %s", p);
        break;
    }
    case 'h': case 'H': press_button(a, B_HOME); break;
    case '/': press_button(a, B_ROOT); break;
    default: break;
    }
}

static void global_event(struct wlwin *w, uint32_t name, const char *iface, uint32_t ver)
{
    struct app *a = w->data;
    wlclip_global(&a->clip, w, name, iface, ver);
}

/* ---- command line, selftest ------------------------------------------------------------------------------------------------ */

static int list_cmd(const char *dir, int n, bool apparent)
{
    g_apparent = apparent;
    struct scan *sc = scan_start(dir, true, false);
    if (!sc || !sc->root) {
        fprintf(stderr, "mellagui-du: cannot read %s\n", dir);
        scan_free(sc);
        return 1;
    }
    struct node *r = sc->root;
    sort_kids(r);
    char tot[24];
    human(nsize(r), tot, sizeof(tot));
    printf("%s  %s in %lld things (%s)%s\n", sc->path, tot, r->items, apparent ? "apparent size" : "disk usage", r->errs ? ", some could not be read" : "");
    for (int i = 0; i < r->nkid && i < n; i++) {
        char sz[24];
        human(nsize(r->kid[i]), sz, sizeof(sz));
        printf("%10lld %9s %3d%% %s%s\n", nsize(r->kid[i]), sz, (int)(100 * nsize(r->kid[i]) / LM_MAX(nsize(r), 1)), r->kid[i]->name, r->kid[i]->dir ? "/" : "");
    }
    scan_free(sc);
    return 0;
}

static int selftest(void)
{
    int bad = 0;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "mellagui-du: selftest failed: %s (line %d)\n", #cond, __LINE__); bad++; } } while (0)
    char b[24];
    human(512, b, sizeof(b));
    CHECK(!strcmp(b, "512 B"));
    human(1536, b, sizeof(b));
    CHECK(!strcmp(b, "1.50 K"));
    human(5LL << 30, b, sizeof(b));
    CHECK(!strcmp(b, "5.00 G"));
    /* the tree map fills its area and keeps the proportions */
    static const double v[] = { 600, 300, 200, 100, 100, 50, 30, 10, 5, 0 };
    enum { N = 10 };
    struct trect t[N];
    struct trect area = { 3, 5, 400, 250 };
    treemap(v, N, area, t);
    long long cover = 0;
    for (int i = 0; i < N; i++) {
        CHECK(t[i].x >= area.x && t[i].y >= area.y && t[i].x + t[i].w <= area.x + area.w && t[i].y + t[i].h <= area.y + area.h);
        cover += (long long)t[i].w * t[i].h;
        for (int j = i + 1; j < N; j++)
            CHECK(t[i].x + t[i].w <= t[j].x || t[j].x + t[j].w <= t[i].x || t[i].y + t[i].h <= t[j].y || t[j].y + t[j].h <= t[i].y ||
                  t[i].w == 0 || t[i].h == 0 || t[j].w == 0 || t[j].h == 0);
    }
    CHECK(llabs(cover - (long long)area.w * area.h) <= 2 * (area.w + area.h));
    CHECK(t[9].w * t[9].h == 0);
    double sum = 0;
    for (int i = 0; i < N; i++)
        sum += v[i];
    for (int i = 0; i < 4; i++) {
        double want = v[i] / sum * area.w * area.h, got = (double)t[i].w * t[i].h;
        CHECK(fabs(got - want) <= 0.06 * want + 2.0 * (t[i].w + t[i].h));
    }
    /* a real tree: sizes, a file with two names counted once, a link not followed */
    char dir[] = "/tmp/mellagui-du-selftest-XXXXXX";
    if (mkdtemp(dir)) {
        char p[PATH_MAX], q[PATH_MAX];
        snprintf(p, sizeof(p), "%s/a", dir);
        mkdir(p, 0755);
        snprintf(p, sizeof(p), "%s/b", dir);
        mkdir(p, 0755);
        char zeros[10000] = { 0 };
        snprintf(p, sizeof(p), "%s/a/big", dir);
        FILE *f = fopen(p, "w");
        if (f) {
            fwrite(zeros, 1, 10000, f);
            fclose(f);
        }
        snprintf(q, sizeof(q), "%s/b/alias", dir);
        if (link(p, q) < 0) {}
        snprintf(p, sizeof(p), "%s/b/small", dir);
        f = fopen(p, "w");
        if (f) {
            fwrite(zeros, 1, 5000, f);
            fclose(f);
        }
        snprintf(q, sizeof(q), "%s/b/link", dir);
        if (symlink(dir, q) < 0) {}
        g_apparent = true;
        struct scan *sc = scan_start(dir, true, false);
        CHECK(sc && sc->root && sc->root->items == 3);                      /* big, small and the link (the second name of big is not counted) */
        struct node *r = sc ? sc->root : NULL;
        if (r) {
            long long files = 0;
            for (int i = 0; i < r->nkid; i++)
                for (int j = 0; j < r->kid[i]->nkid; j++)
                    files += r->kid[i]->kid[j]->app;
            CHECK(files == 10000 + 5000 + (long long)strlen(dir));                    /* (the link's own size is its target's length) */
            sort_kids(r);
            CHECK(r->nkid == 2 && ((!strcmp(r->kid[0]->name, "a") && !strcmp(r->kid[1]->name, "b")) || (!strcmp(r->kid[0]->name, "b") && !strcmp(r->kid[1]->name, "a"))));
            CHECK(r->kid[0]->app >= r->kid[1]->app);                                  /* biggest first */
        }
        scan_free(sc);
        snprintf(p, sizeof(p), "rm -rf '%s'", dir);
        if (system(p) != 0) {}
        g_apparent = false;
    }
    if (!bad)
        puts("mellagui-du selftest ok");
    return bad ? 1 : 0;
}

int main(int argc, char **argv)
{
    lm_theme_load_env();
    if (argc > 1 && !strcmp(argv[1], "--selftest"))
        return selftest();
    if (argc > 1 && !strcmp(argv[1], "--list")) {
        bool app = false;
        const char *dir = NULL;
        int n = 15;
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "--apparent"))
                app = true;
            else if (!dir)
                dir = argv[i];
            else
                n = atoi(argv[i]);
        }
        return list_cmd(dir ? dir : (getenv("HOME") ? getenv("HOME") : "/"), n, app);
    }
    if (argc > 2 || (argc == 2 && argv[1][0] == '-')) {
        fprintf(stderr, "usage: mellagui-du [DIR | --list [--apparent] [DIR] [N] | --selftest]\n");
        return 2;
    }
    static const struct wlwin_ops ops = { .paint = paint, .global = global_event, .pointer = pointer_event, .keyboard = keyboard_event, .menu = menu_event };
    static struct app a;
    a.hover = a.hovertool = -1;
    a.one_fs = true;
    const char *start = argc == 2 ? argv[1] : getenv("HOME");
    struct stat st;
    if (!start || !*start || stat(start, &st) != 0 || !S_ISDIR(st.st_mode))
        start = "/";
    char real[PATH_MAX];
    if (realpath(start, real))
        start = real;
    signal(SIGCHLD, SIG_IGN);
    if (wlwin_connect(&a.win, "mellagui-du", &ops, &a) < 0)
        return 1;
    if (wlwin_open(&a.win, "Disk usage", "mellagui.du", 920, 600, 560, 340) < 0)
        return 1;
    wlclip_attach(&a.clip, &a.win);
    begin_scan(&a, start);
    int lw_prev = 0, bw_prev = 0, bh_prev = 0;
    while (a.win.running) {
        int timeout = a.sc ? 100 : -1;
        if (!wlwin_step(&a.win, timeout))
            break;
        if (a.sc && atomic_load(&a.sc->done))
            take_scan(&a);
        else if (a.sc)
            a.win.redraw = true;
        if (a.win.bw != bw_prev || a.win.bh != bh_prev || list_width(&a) != lw_prev) {          /* the window changed size: the tree map again */
            bw_prev = a.win.bw;
            bh_prev = a.win.bh;
            lw_prev = list_width(&a);
            relayout(&a);
            a.win.redraw = true;
        }
    }
    if (a.sc)
        scan_free(a.sc);
    scan_free(a.held);
    free(a.tiles);
    wlwin_destroy(&a.win);
    return 0;
}
