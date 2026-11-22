/*
 * mellagui-desktop - the icons of the desktop: the folder ~/Desktop shown as a grid of icons under every window.
 *
 *   mellagui-desktop             the program the compositor starts with itself ([commands] desktop; `none` = off)
 *   mellagui-desktop --list      print what the folder holds, one `kind<TAB>name` a line
 *   mellagui-desktop --selftest
 *
 * The window (app id mellagui.desktop) is the wm's business: it covers the output under the panel, lies under the gadgets and every window, is on every
 * workspace and has no frame (wm_desktop.c). The program draws into a transparent buffer, so the wallpaper shows between the icons.
 *
 * The folder is $MELLAGUI_DESKTOP_DIR, else ~/Desktop (made when it is not there). What is in it is shown as icons (folders, pictures as small pictures, programs
 * and .desktop files with their own icon, other files as a page with the kind's colour), a name under each. Hidden files are not shown. The folder is watched
 * (inotify): a file that comes appears, one that goes disappears.
 *
 * Mouse: a click chooses (Ctrl adds and takes away), a double click opens, a drag on empty space chooses what the frame touches, a drag on an icon carries it
 * to another place of the grid (dropped on a folder it goes into the folder), a right click gives a menu. Keys: arrows, Enter opens, F2 renames, Delete moves
 * to the trash, Ctrl+A chooses all, Esc lets go. Where the icons are is kept in $XDG_CONFIG_HOME/mellagui/desktop.conf (name, column, row).
 * $MELLAGUI_DESKTOP_DUMP=FILE writes the state after every paint (tests).
 */
#pragma GCC diagnostic ignored "-Wformat-truncation"   /* (a name that is too long is cut) */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "dockicon.h"
#include "font.h"
#include "iconfind.h"
#include "img.h"
#include "keymap.h"
#include "metrics.h"
#include "protocol.h"
#include "theme.h"
#include "uikit.h"
#include "wlc.h"
#include "wlcard.h"
#include "wlclip.h"
#include "wltext.h"
#include "wlwin.h"

#define MAX_ITEMS 400
#define CW 96                           /* a cell of the grid */
#define CH 100
#define MARGIN 14
#define BOTTOM_ROOM 84                  /* the dock lies over the bottom of the screen: no icons there */
#define ISZ 48                          /* the icon's size */
#define LABEL_W (CW - 8)
#define HOLD_PX 6

enum kind { K_DIR, K_IMAGE, K_VIDEO, K_AUDIO, K_ARCHIVE, K_PDF, K_CODE, K_TEXT, K_PROGRAM, K_LAUNCHER, K_OTHER };

struct item {
    char name[NAME_MAX + 1];
    char shown[160];                    /* the name under the icon (a .desktop file's Name) */
    char ext[8];                        /* the letters in the band of a page icon: "PNG" */
    char exec[512];                     /* K_LAUNCHER: what it starts */
    char icon[160];                     /* K_LAUNCHER: Icon= */
    bool terminal;                      /* K_LAUNCHER: Terminal=true */
    enum kind kind;
    bool broken;                        /* a link to nothing */
    bool sel;
    int cx, cy;                         /* the cell */
    uint32_t *img;                      /* the icon: P x P, straight ARGB */
    int img_p;
    bool thumb_wanted, thumb_done;      /* a picture: its small copy is made later, one at a time */
};

enum prompt { P_NONE, P_RENAME, P_NEWDIR, P_NEWFILE };
enum menu_what { M_NONE, M_ITEM, M_EMPTY };

struct app {
    struct wlwin win;
    struct wlclip clip;
    char dir[PATH_MAX];
    struct item *it;
    int n;
    int ino;                            /* inotify */
    int cols, rows;
    int hover;                          /* the item under the pointer, -1 */
    double px, py;
    /* the mouse */
    bool down;                          /* the left button is down */
    int down_item;                      /* ... on this item (-1: on the empty desktop) */
    double down_x, down_y;
    bool dragging;                      /* icons are being carried */
    bool banding;                       /* a selection frame is being drawn */
    bool band_add;
    int click_item;                     /* the double click: the item and when */
    uint32_t click_ms;
    /* the menu */
    enum menu_what menu;
    int menu_item;
    int menu_map[10];
    /* the question */
    enum prompt prompt;
    char field[NAME_MAX + 1];
    char perr[160];
    char target[NAME_MAX + 1];          /* what the rename is about */
    int hot_btn;
    char pending_select[NAME_MAX + 1];  /* chosen when the next listing has it (a new folder that gets its name asked) */
    /* a message at the bottom */
    char msg[200];
    uint32_t msg_until;
    bool dirty_scan;
    int anchor, cur;                    /* Shift picks a range from the anchor to the item last chosen (-1: none) */
    char ta[24];                        /* type-ahead: the letters typed lately jump to a name that starts with them */
    uint32_t ta_ms;
};

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + (uint32_t)(ts.tv_nsec / 1000000));
}

static void say(struct app *a, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void say(struct app *a, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(a->msg, sizeof(a->msg), fmt, ap);
    va_end(ap);
    a->msg_until = now_ms() + 4500;
    a->win.redraw = true;
}

/* ---- the folder ------------------------------------------------------------------------------------------------------------ */

static void desktop_dir(char *out, size_t n)
{
    const char *d = getenv("MELLAGUI_DESKTOP_DIR"), *h = getenv("HOME");
    if (d && *d)
        snprintf(out, n, "%s", d);
    else
        snprintf(out, n, "%s/Desktop", h && *h ? h : "");
}

static bool ext_in(const char *ext, const char *const *list)
{
    for (; *list; list++)
        if (!strcasecmp(ext, *list))
            return true;
    return false;
}

static void file_ext(const char *name, char *ext, size_t n)
{
    const char *dot = strrchr(name, '.');
    ext[0] = 0;
    if (!dot || dot == name || !dot[1] || strlen(dot + 1) >= n)
        return;
    for (size_t i = 0; dot[1 + i]; i++)
        ext[i] = dot[1 + i];
    ext[strlen(dot + 1)] = 0;
}

static enum kind classify(const char *path, const char *name, bool *exec)
{
    static const char *const img[] = { "png", "jpg", "jpeg", "gif", "bmp", "webp", "ico", "tga", "tif", "tiff", "ppm", "pgm", "pbm", "svg", NULL };
    static const char *const vid[] = { "mp4", "m4v", "mkv", "webm", "avi", "mov", "ogv", NULL };
    static const char *const aud[] = { "mp3", "flac", "ogg", "oga", "opus", "wav", "m4a", "aac", "wma", "mka", NULL };
    static const char *const arc[] = { "zip", "tar", "gz", "tgz", "xz", "txz", "bz2", "tbz2", "zst", "7z", "rar", "deb", "rpm", "jar", NULL };
    static const char *const code[] = { "c", "h", "cpp", "hpp", "cc", "py", "sh", "js", "ts", "rs", "go", "java", "json", "xml", "html", "css", "yml", "yaml", "toml",
                                        "lua", "sql", "php", "rb", "pl", NULL };
    char ext[16];
    file_ext(name, ext, sizeof(ext));
    *exec = false;
    if (!strcasecmp(ext, "desktop"))
        return K_LAUNCHER;
    if (ext_in(ext, img)) return K_IMAGE;
    if (ext_in(ext, vid)) return K_VIDEO;
    if (ext_in(ext, aud)) return K_AUDIO;
    if (ext_in(ext, arc)) return K_ARCHIVE;
    if (!strcasecmp(ext, "pdf")) return K_PDF;
    if (ext_in(ext, code)) return K_CODE;
    if (!strcasecmp(ext, "txt") || !strcasecmp(ext, "md") || !strcasecmp(ext, "log") || !strcasecmp(ext, "csv") || !strcasecmp(ext, "conf") || !strcasecmp(ext, "ini"))
        return K_TEXT;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return K_OTHER;
    unsigned char buf[2048];
    ssize_t k = read(fd, buf, sizeof(buf));
    close(fd);
    if (k >= 4 && !memcmp(buf, "\x7f" "ELF", 4) && access(path, X_OK) == 0) {
        *exec = true;
        return K_PROGRAM;
    }
    if (k > 0 && !memchr(buf, 0, (size_t)k))
        return K_TEXT;
    return K_OTHER;
}

/* A .desktop file: Name=, Exec=, Icon=, Terminal= of the [Desktop Entry] group. */
static void read_launcher(const char *path, struct item *it)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[700];
    bool in = false;
    while (fgets(line, sizeof(line), f)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r'))
            line[--l] = 0;
        if (line[0] == '[') {
            in = !strcmp(line, "[Desktop Entry]");
            continue;
        }
        if (!in)
            continue;
        if (!strncmp(line, "Name=", 5) && !it->shown[0])
            snprintf(it->shown, sizeof(it->shown), "%s", line + 5);
        else if (!strncmp(line, "Exec=", 5))
            snprintf(it->exec, sizeof(it->exec), "%s", line + 5);
        else if (!strncmp(line, "Icon=", 5))
            snprintf(it->icon, sizeof(it->icon), "%s", line + 5);
        else if (!strncmp(line, "Terminal=", 9))
            it->terminal = !strcmp(line + 9, "true");
    }
    fclose(f);
}

/* The Exec line without its field codes (%f %U ...); a doubled %% is one %. */
static void exec_clean(const char *in, char *out, size_t n)
{
    size_t k = 0;
    for (; *in && k + 1 < n; in++) {
        if (*in == '%' && in[1]) {
            in++;
            if (*in == '%')
                out[k++] = '%';
            continue;
        }
        if (*in == ' ' && k && out[k - 1] == ' ')                       /* (a dropped field code leaves no double blank) */
            continue;
        out[k++] = *in;
    }
    while (k && out[k - 1] == ' ')
        k--;
    out[k] = 0;
}

static int cmp_item(const void *pa, const void *pb)
{
    const struct item *a = pa, *b = pb;
    if ((a->kind == K_DIR) != (b->kind == K_DIR))
        return a->kind == K_DIR ? -1 : 1;
    return strcasecmp(a->shown, b->shown);
}

/* ---- where the icons are -------------------------------------------------------------------------------------------------------- */

static void conf_path(char *out, size_t n)
{
    const char *x = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
    if (x && *x)
        snprintf(out, n, "%s/mellagui/desktop.conf", x);
    else
        snprintf(out, n, "%s/.config/mellagui/desktop.conf", h && *h ? h : "");
}

struct place {
    char name[NAME_MAX + 1];
    int cx, cy;
};
static struct place *g_places;
static int g_nplaces, g_capplaces;

static void places_set(const char *name, int cx, int cy)
{
    for (int i = 0; i < g_nplaces; i++)
        if (!strcmp(g_places[i].name, name)) {
            g_places[i].cx = cx;
            g_places[i].cy = cy;
            return;
        }
    if (g_nplaces == g_capplaces) {
        int nc = g_capplaces ? g_capplaces * 2 : 64;
        struct place *np = realloc(g_places, (size_t)nc * sizeof(*np));
        if (!np)
            return;
        g_places = np;
        g_capplaces = nc;
    }
    snprintf(g_places[g_nplaces].name, sizeof(g_places[0].name), "%s", name);
    g_places[g_nplaces].cx = cx;
    g_places[g_nplaces].cy = cy;
    g_nplaces++;
}

static const struct place *places_get(const char *name)
{
    for (int i = 0; i < g_nplaces; i++)
        if (!strcmp(g_places[i].name, name))
            return &g_places[i];
    return NULL;
}

static void places_forget(const char *name)
{
    for (int i = 0; i < g_nplaces; i++)
        if (!strcmp(g_places[i].name, name)) {
            g_places[i] = g_places[--g_nplaces];
            return;
        }
}

static void places_load(void)
{
    char path[PATH_MAX];
    conf_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[NAME_MAX + 40];
    while (fgets(line, sizeof(line), f)) {
        char *t1 = strchr(line, '\t');
        if (!t1)
            continue;
        *t1 = 0;
        int cx, cy;
        if (sscanf(t1 + 1, "%d %d", &cx, &cy) == 2 && cx >= 0 && cy >= 0 && cx < 100 && cy < 100)
            places_set(line, cx, cy);
    }
    fclose(f);
}

static void places_save(const struct app *a)
{
    char path[PATH_MAX], tmp[PATH_MAX + 8];
    conf_path(path, sizeof(path));
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = 0;
        for (char *p = dir + 1; *p; p++)
            if (*p == '/') {
                *p = 0;
                mkdir(dir, 0700);
                *p = '/';
            }
        mkdir(dir, 0700);
    }
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return;
    for (int i = 0; i < a->n; i++)
        fprintf(f, "%s\t%d %d\n", a->it[i].name, a->it[i].cx, a->it[i].cy);
    if (fclose(f) == 0)
        rename(tmp, path);
    else
        unlink(tmp);
}

/* ---- the grid ---------------------------------------------------------------------------------------------------------------- */

static void grid_size(struct app *a)
{
    a->cols = LM_MAX((a->win.bw - 2 * MARGIN) / CW, 1);
    a->rows = LM_MAX((a->win.bh - MARGIN - BOTTOM_ROOM) / CH, 1);
}

static int item_at_cell(const struct app *a, int cx, int cy, int skip)
{
    for (int i = 0; i < a->n; i++)
        if (i != skip && a->it[i].cx == cx && a->it[i].cy == cy)
            return i;
    return -1;
}

static bool cell_in(const struct app *a, int cx, int cy)
{
    return cx >= 0 && cy >= 0 && cx < a->cols && cy < a->rows;
}

/* The first free cell, column after column. false when the grid is full. */
static bool free_cell(const struct app *a, int *cx, int *cy)
{
    for (int x = 0; x < a->cols; x++)
        for (int y = 0; y < a->rows; y++)
            if (item_at_cell(a, x, y, -1) < 0) {
                *cx = x;
                *cy = y;
                return true;
            }
    return false;
}

/* Every item gets a cell: its saved one when that is inside the grid and free, else the first free one. An item for which there is no room gets (-1, -1) and is not shown. */
static void place_items(struct app *a)
{
    grid_size(a);
    for (int i = 0; i < a->n; i++)
        a->it[i].cx = a->it[i].cy = -1;
    for (int i = 0; i < a->n; i++) {
        const struct place *p = places_get(a->it[i].name);
        if (p && cell_in(a, p->cx, p->cy) && item_at_cell(a, p->cx, p->cy, i) < 0) {
            a->it[i].cx = p->cx;
            a->it[i].cy = p->cy;
        }
    }
    for (int i = 0; i < a->n; i++)
        if (a->it[i].cx < 0 && !free_cell(a, &a->it[i].cx, &a->it[i].cy))
            a->it[i].cx = a->it[i].cy = -1;
}

static void cell_rect(const struct app *a, int cx, int cy, int *x, int *y)
{
    (void)a;
    *x = MARGIN + cx * CW;
    *y = MARGIN + cy * CH;
}

/* ---- scanning ------------------------------------------------------------------------------------------------------------------ */

static void items_free(struct app *a)
{
    for (int i = 0; i < a->n; i++)
        free(a->it[i].img);
    a->n = 0;
}

static void scan(struct app *a)
{
    struct item *old = a->it;
    int nold = a->n;
    a->it = NULL;
    a->n = 0;
    struct item *list = calloc(MAX_ITEMS, sizeof(*list));
    if (!list) {
        a->it = old;
        a->n = nold;
        return;
    }
    DIR *d = opendir(a->dir);
    int n = 0;
    struct dirent *e;
    while (d && (e = readdir(d)) && n < MAX_ITEMS) {
        if (e->d_name[0] == '.')
            continue;
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", a->dir, e->d_name);
        struct stat st;
        struct item *it = &list[n];
        snprintf(it->name, sizeof(it->name), "%s", e->d_name);
        bool exec = false;
        if (stat(path, &st) < 0) {
            it->broken = true;
            it->kind = K_OTHER;
        } else if (S_ISDIR(st.st_mode)) {
            it->kind = K_DIR;
        } else {
            it->kind = classify(path, e->d_name, &exec);
        }
        file_ext(e->d_name, it->ext, sizeof(it->ext));
        for (char *c = it->ext; *c; c++)
            if (*c >= 'a' && *c <= 'z')
                *c = (char)(*c - 32);
        if (it->kind == K_LAUNCHER)
            read_launcher(path, it);
        if (!it->shown[0])
            snprintf(it->shown, sizeof(it->shown), "%s", e->d_name);
        it->thumb_wanted = it->kind == K_IMAGE && !it->broken && st.st_size > 0 && st.st_size < 24 * 1024 * 1024;
        for (int k = 0; k < nold; k++)                          /* what was chosen stays chosen */
            if (!strcmp(old[k].name, it->name))
                it->sel = old[k].sel;
        n++;
    }
    if (d)
        closedir(d);
    qsort(list, (size_t)n, sizeof(*list), cmp_item);
    a->it = list;
    a->n = n;
    for (int k = 0; k < nold; k++)
        free(old[k].img);
    free(old);
    place_items(a);
    a->hover = -1;
    a->anchor = a->cur = -1;
    if (a->pending_select[0]) {
        for (int i = 0; i < a->n; i++)
            a->it[i].sel = !strcmp(a->it[i].name, a->pending_select);
        a->pending_select[0] = 0;
    }
    a->win.redraw = true;
}

/* ---- the pictures of the icons ----------------------------------------------------------------------------------------------------- */

/* A pixel of straight ARGB laid over what is in the picture. */
static void put_px(uint32_t *buf, int P, int x, int y, uint32_t rgb, double alpha)
{
    if (x < 0 || y < 0 || x >= P || y >= P || alpha <= 0)
        return;
    if (alpha > 1)
        alpha = 1;
    uint32_t d = buf[(size_t)y * (size_t)P + (size_t)x];
    double da = (double)(d >> 24) / 255.0, oa = alpha + da * (1 - alpha);
    uint32_t out = (uint32_t)(oa * 255.0 + 0.5) << 24;
    for (int sh = 0; sh <= 16; sh += 8) {
        double c = ((double)((rgb >> sh) & 255) * alpha + (double)((d >> sh) & 255) * da * (1 - alpha)) / (oa > 0 ? oa : 1);
        out |= (uint32_t)(c > 255 ? 255 : c + 0.5) << sh;
    }
    buf[(size_t)y * (size_t)P + (size_t)x] = out;
}

/* Coverage of a rounded rectangle (units of P) at a pixel. */
static double rr_cov(double fx, double fy, double x0, double y0, double x1, double y1, double r, double P)
{
    double cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, hx = (x1 - x0) / 2 - r, hy = (y1 - y0) / 2 - r;
    double qx = fabs(fx - cx) - hx, qy = fabs(fy - cy) - hy;
    double d = sqrt((qx > 0 ? qx : 0) * (qx > 0 ? qx : 0) + (qy > 0 ? qy : 0) * (qy > 0 ? qy : 0)) + (qx > qy ? (qx < 0 ? qx : 0) : (qy < 0 ? qy : 0)) - r;
    double c = 0.5 - d * P;
    return c < 0 ? 0 : c > 1 ? 1 : c;
}

/* A rounded rectangle with a vertical gradient from `top` to `bot` (colours 0xRRGGBB). */
static void rr_fill(uint32_t *buf, int P, double x0, double y0, double x1, double y1, double r, uint32_t top, uint32_t bot, double alpha)
{
    int ya = (int)floor(y0 * P) - 1, yb = (int)ceil(y1 * P) + 1, xa = (int)floor(x0 * P) - 1, xb = (int)ceil(x1 * P) + 1;
    for (int y = ya; y < yb; y++)
        for (int x = xa; x < xb; x++) {
            double c = rr_cov(((double)x + 0.5) / P, ((double)y + 0.5) / P, x0, y0, x1, y1, r, P);
            if (c <= 0)
                continue;
            double t = ((double)y + 0.5) / P;
            t = (t - y0) / (y1 - y0);
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            uint32_t rgb = lm_mix_rgb(top, bot, t);
            put_px(buf, P, x, y, rgb, c * alpha);
        }
}

static void shadow(uint32_t *buf, int P, double x0, double y0, double x1, double y1, double r)
{
    for (int i = 4; i >= 1; i--) {
        double g = 0.012 * i;
        rr_fill(buf, P, x0 - g, y0 - g + 0.025, x1 + g, y1 + g + 0.025, r + g, 0x000000, 0x000000, 0.07);
    }
}

static void icon_folder(uint32_t *buf, int P)
{
    shadow(buf, P, 0.06, 0.20, 0.94, 0.86, 0.08);
    rr_fill(buf, P, 0.06, 0.16, 0.44, 0.34, 0.05, 0xd9962a, 0xc98820, 1);
    rr_fill(buf, P, 0.06, 0.24, 0.94, 0.84, 0.08, 0xe0a030, 0xc98820, 1);
    rr_fill(buf, P, 0.06, 0.32, 0.94, 0.88, 0.08, 0xffd468, 0xf2b23c, 1);
    rr_fill(buf, P, 0.08, 0.33, 0.92, 0.345, 0.01, 0xffffff, 0xffffff, 0.35);
}

static uint32_t kind_colour(enum kind k)
{
    switch (k) {
    case K_IMAGE: return 0x2f9fb8;
    case K_VIDEO: return 0x8e5bd6;
    case K_AUDIO: return 0xe0813a;
    case K_ARCHIVE: return 0x9a6b3d;
    case K_PDF: return 0xd64545;
    case K_CODE: return 0x3f9f5a;
    case K_TEXT: return 0x5b7fb5;
    default: return 0x7a8594;
    }
}

/* A sheet of paper with a folded corner and a band of the kind's colour (the letters go in the band when the icon is shown). */
static void icon_page(uint32_t *buf, int P, enum kind k, bool broken)
{
    double x0 = 0.20, y0 = 0.05, x1 = 0.80, y1 = 0.95, f = 0.2;
    shadow(buf, P, x0, y0, x1, y1, 0.05);
    for (int y = 0; y < P; y++)
        for (int x = 0; x < P; x++) {
            double fx = ((double)x + 0.5) / P, fy = ((double)y + 0.5) / P;
            double c = rr_cov(fx, fy, x0, y0, x1, y1, 0.05, P);
            if (c <= 0)
                continue;
            double u = (fx - (x1 - f)) / f, v = (fy - y0) / f;
            uint32_t col = lm_mix_rgb(0xffffff, 0xdfe4ec, (fy - y0) / (y1 - y0));
            if (u > 0 && v > 0 && u < 1.2 && v < 1.2 && u > v + 0.02) {         /* above the fold: cut (and the shadow with it) */
                buf[(size_t)y * (size_t)P + (size_t)x] = 0;
                continue;
            }
            if (u > 0 && v > 0 && v <= 1 && u <= v)
                col = lm_mix_rgb(0xc2c9d6, 0xe6eaf1, 1 - v);                  /* the folded flap */
            put_px(buf, P, x, y, col, c);
        }
    for (int i = 0; i < 3; i++)                                                 /* lines of "text" */
        rr_fill(buf, P, 0.28, 0.27 + 0.075 * i, 0.72 - (i == 2 ? 0.16 : 0), 0.30 + 0.075 * i, 0.01, 0xaab3c4, 0xaab3c4, 0.85);
    rr_fill(buf, P, x0, 0.58, x1, 0.82, 0.03, broken ? 0xb0b0b0 : kind_colour(k), broken ? 0x909090 : lm_mix_rgb(kind_colour(k), 0x000000, 0.18), 1);
}

/* The picture `src` (straight ARGB, sw x sh) laid into the box (ox, oy, bw, bh) of the icon, by averaging when it is shrunk. */
static void blit_scaled(uint32_t *buf, int P, const uint32_t *src, int sw, int sh, int ox, int oy, int bw, int bh)
{
    for (int y = 0; y < bh; y++) {
        int y0 = (int)((int64_t)y * sh / bh), y1 = (int)((int64_t)(y + 1) * sh / bh);
        y1 = LM_MIN(LM_MAX(y1, y0 + 1), sh);
        for (int x = 0; x < bw; x++) {
            int x0 = (int)((int64_t)x * sw / bw), x1 = (int)((int64_t)(x + 1) * sw / bw);
            x1 = LM_MIN(LM_MAX(x1, x0 + 1), sw);
            double r = 0, g = 0, b = 0, al = 0;
            int cnt = 0;
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++) {
                    uint32_t p = src[(size_t)yy * (size_t)sw + (size_t)xx];
                    double pa = (double)(p >> 24) / 255.0;
                    r += (double)((p >> 16) & 255) * pa;
                    g += (double)((p >> 8) & 255) * pa;
                    b += (double)(p & 255) * pa;
                    al += pa;
                    cnt++;
                }
            if (al <= 0 || cnt == 0)
                continue;
            uint32_t rgb = ((uint32_t)(r / al + 0.5) << 16) | ((uint32_t)(g / al + 0.5) << 8) | (uint32_t)(b / al + 0.5);
            put_px(buf, P, ox + x, oy + y, rgb, al / cnt);
        }
    }
}

/* A built-in icon of the dock, drawn as the dock does (premultiplied ARGB turned into straight). */
static void icon_dock(uint32_t *buf, int P, int kind)
{
    double R = P * 0.40;
    for (int y = 0; y < P; y++)
        for (int x = 0; x < P; x++) {
            uint32_t p = lm_dockicon_pixel(kind, (float)((double)x + 0.5 - P / 2.0), (float)((double)y + 0.5 - P / 2.0), (float)R, 0.0f);
            uint32_t a = p >> 24;
            if (!a)
                continue;
            if (!(p & 0x00ffffffu))
                a = a * 45 / 100;                               /* the dock's shadow is too heavy over a wallpaper */
            if (!a)
                continue;
            uint32_t r = ((p >> 16) & 255) * 255 / a, g = ((p >> 8) & 255) * 255 / a, b = (p & 255) * 255 / a;
            buf[(size_t)y * (size_t)P + (size_t)x] = (a << 24) | (LM_MIN(r, 255u) << 16) | (LM_MIN(g, 255u) << 8) | LM_MIN(b, 255u);
        }
}

/* The icon of a .desktop file: the program's own picture when it is installed, else a dock icon guessed from its name. */
static void icon_launcher(struct item *it, uint32_t *buf, int P)
{
    char path[PATH_MAX] = "";
    if (it->icon[0]) {
        if (it->icon[0] == '/')
            snprintf(path, sizeof(path), "%s", it->icon);
        else
            lm_icon_find(it->icon, path, sizeof(path));
    }
    struct lm_image img = { 0 };
    char err[80];
    if (path[0] && lm_img_load_alpha(path, &img, err, sizeof(err)) == 0 && img.w > 0 && img.h > 0) {
        int box = (int)(P * 0.92);
        int bw = img.w >= img.h ? box : LM_MAX(box * img.w / img.h, 1), bh = img.h >= img.w ? box : LM_MAX(box * img.h / img.w, 1);
        blit_scaled(buf, P, img.px, img.w, img.h, (P - bw) / 2, (P - bh) / 2, bw, bh);
        lm_img_free(&img);
        return;
    }
    int kind = lm_dockicon_guess(it->shown);
    if (kind == LM_DOCKICON_GENERIC && it->icon[0]) {
        int k2 = lm_dockicon_by_name(it->icon);
        if (k2 >= 0)
            kind = k2;
    }
    icon_dock(buf, P, kind);
}

static void icon_thumb(struct app *a, struct item *it)
{
    int P = it->img_p;
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", a->dir, it->name);
    struct lm_image img = { 0 };
    char err[80];
    it->thumb_done = true;
    if (lm_img_load_alpha(path, &img, err, sizeof(err)) != 0 || img.w <= 0 || img.h <= 0)
        return;
    uint32_t *buf = it->img;
    memset(buf, 0, (size_t)P * (size_t)P * 4);
    double box = P * 0.88;
    int bw = img.w >= img.h ? (int)box : LM_MAX((int)(box * img.w / img.h), 1), bh = img.h >= img.w ? (int)box : LM_MAX((int)(box * img.h / img.w), 1);
    int ox = (P - bw) / 2, oy = (P - bh) / 2;
    double b = LM_MAX(P / 24, 2);
    shadow(buf, P, (double)(ox - b) / P, (double)(oy - b) / P, (double)(ox + bw + b) / P, (double)(oy + bh + b) / P, 0.03);
    rr_fill(buf, P, (double)(ox - b) / P, (double)(oy - b) / P, (double)(ox + bw + b) / P, (double)(oy + bh + b) / P, 0.03, 0xffffff, 0xf0f0f0, 1);
    blit_scaled(buf, P, img.px, img.w, img.h, ox, oy, bw, bh);
    lm_img_free(&img);
}

/* The icon of an item at the buffer's scale, made when it is first needed (a picture's own small copy later, by thumbs_lazy). */
static void icon_make(struct app *a, struct item *it, int P)
{
    (void)a;
    free(it->img);
    it->img = calloc((size_t)P * (size_t)P, 4);
    it->img_p = P;
    if (!it->img)
        return;
    switch (it->kind) {
    case K_DIR: icon_folder(it->img, P); break;
    case K_LAUNCHER: icon_launcher(it, it->img, P); break;
    case K_PROGRAM: icon_dock(it->img, P, LM_DOCKICON_GENERIC); break;
    default: icon_page(it->img, P, it->kind, it->broken); break;
    }
}

/* One picture's small copy a turn of the loop: a big JPEG takes a moment. */
static void thumbs_lazy(struct app *a)
{
    int P = ISZ * wlwin_scale(&a->win);
    for (int i = 0; i < a->n; i++) {
        struct item *it = &a->it[i];
        if (!it->thumb_wanted || it->thumb_done || it->cx < 0)
            continue;
        if (!it->img || it->img_p != P)
            icon_make(a, it, P);
        icon_thumb(a, it);
        a->win.redraw = true;
        return;
    }
}

/* ---- painting ------------------------------------------------------------------------------------------------------------------ */

/* A translucent rounded box over the transparent buffer (premultiplied), in logical pixels. */
static void fill_alpha(struct wlwin *w, int x, int y, int bw, int bh, int r, uint32_t rgb, int alpha)
{
    int s = wlwin_scale(w), px = x * s, py = y * s, pw = bw * s, ph = bh * s, pr = LM_MIN(r * s, LM_MIN(pw, ph) / 2);
    for (int yy = LM_MAX(py, 0); yy < LM_MIN(py + ph, w->ph); yy++)
        for (int xx = LM_MAX(px, 0); xx < LM_MIN(px + pw, w->pw); xx++) {
            double cov = 1;
            double dx = xx + 0.5 < px + pr ? px + pr - (xx + 0.5) : xx + 0.5 > px + pw - pr ? (xx + 0.5) - (px + pw - pr) : 0;
            double dy = yy + 0.5 < py + pr ? py + pr - (yy + 0.5) : yy + 0.5 > py + ph - pr ? (yy + 0.5) - (py + ph - pr) : 0;
            if (dx > 0 && dy > 0) {
                cov = pr + 0.5 - sqrt(dx * dx + dy * dy);
                cov = cov < 0 ? 0 : cov > 1 ? 1 : cov;
            }
            double al = cov * alpha / 255.0;
            if (al <= 0)
                continue;
            uint32_t *p = &w->pixels[(size_t)yy * (size_t)w->pw + (size_t)xx];
            uint32_t d = *p;
            uint32_t out = 0;
            double inv = 1 - al;
            uint32_t oa = (uint32_t)(al * 255.0 + (double)(d >> 24) * inv + 0.5);
            out = LM_MIN(oa, 255u) << 24;
            for (int sh = 0; sh <= 16; sh += 8) {
                double c = (double)((rgb >> sh) & 255) * al + (double)((d >> sh) & 255) * inv;
                out |= (uint32_t)(c > 255 ? 255 : c + 0.5) << sh;
            }
            *p = out;
        }
}

/* The name under an icon in at most two lines of LABEL_W pixels. */
static int label_lines(const char *s, char out[2][170])
{
    int len = (int)strlen(s);
    int fit = lm_font_fit(s, LABEL_W);
    if (fit >= len) {
        snprintf(out[0], 170, "%s", s);
        return 1;
    }
    int cut = fit;
    for (int i = fit; i > 0; i--)
        if (s[i] == ' ') {
            cut = i;
            break;
        }
    snprintf(out[0], 170, "%.*s", cut, s);
    const char *rest = s + cut + (s[cut] == ' ' ? 1 : 0);
    int f2 = lm_font_fit(rest, LABEL_W);
    if (f2 >= (int)strlen(rest)) {
        snprintf(out[1], 170, "%s", rest);
    } else {
        int f3 = lm_font_fit(rest, LABEL_W - lm_font_text_width("..."));
        snprintf(out[1], 170, "%.*s...", f3, rest);
    }
    return 2;
}

static void paint_prompt(struct app *a, struct wlwin *w)
{
    static const char *const titles[] = { "", "Rename", "New folder", "New text file" };
    struct wlcard c = { 0, 0, 420, 168 };
    wlcard_center(w, &c);
    wlcard_frame_at(w, &c, c.x, c.y, 420, 168, titles[a->prompt], lm_theme.accent, lm_theme.bar, lm_theme.accent, false);
    int fx = c.x + WLCARD_PAD, fy = c.y + 48, fw = c.w - 2 * WLCARD_PAD;
    wlwin_round_fill(w, fx, fy, fw, 28, 14, lm_theme.background);
    wltext_paint(w, fx + 12, fy + 6, fw - 24, a->field, true, lm_theme.text_soft, lm_theme.button_hover, lm_theme.text_soft);
    if (a->perr[0])
        wlwin_text(w, fx, fy + 38, a->perr, 1, lm_theme.caution, fw);
    const char *labels[2] = { "Cancel", a->prompt == P_RENAME ? "Rename" : "Make" };
    uint32_t cols[2] = { lm_theme.btn_slate, lm_theme.btn_green };
    wlcard_buttons(w, &c, labels, 2, cols, a->hot_btn, -1, lm_theme.on_colour);
}

static void paint(struct wlwin *w)
{
    struct app *a = w->data;
    int P = ISZ * wlwin_scale(w);
    int oc = a->cols, orr = a->rows;
    grid_size(a);
    if (a->cols != oc || a->rows != orr)
        place_items(a);                         /* the window got its size (or the screen changed): the icons are laid out again */
    for (int i = 0; i < a->n; i++) {
        struct item *it = &a->it[i];
        if (it->cx < 0)
            continue;
        int x0, y0;
        cell_rect(a, it->cx, it->cy, &x0, &y0);
        if (a->dragging && it->sel) {
            x0 += (int)(a->px - a->down_x);
            y0 += (int)(a->py - a->down_y);
        }
        if (it->sel)
            fill_alpha(w, x0 + 2, y0 + 2, CW - 4, CH - 4, 8, lm_theme.accent, 0x3a);
        else if (i == a->hover && !a->dragging)
            fill_alpha(w, x0 + 2, y0 + 2, CW - 4, CH - 4, 8, 0xffffff, 0x1c);
        if (!it->img || it->img_p != P) {
            icon_make(a, it, P);
            it->thumb_done = false;
        }
        if (it->img)
            wlwin_blit_px_over(w, x0 + (CW - ISZ) / 2, y0 + 6, it->img, P, P, P);
        if (it->kind != K_DIR && it->kind != K_LAUNCHER && it->kind != K_PROGRAM && it->ext[0] && strlen(it->ext) <= 4 && !(it->thumb_wanted && it->thumb_done)) {
            int tw = lm_font_text_width(it->ext);
            wlwin_text_small(w, x0 + (CW - ISZ) / 2 + (int)(ISZ * 0.5) - (int)(tw * 0.6 / 2), y0 + 6 + (int)(ISZ * 0.585), it->ext, 0.62, 0xffffff, 0);
        }
        char lines[2][170];
        int nl = label_lines(it->shown, lines);
        for (int l = 0; l < nl; l++) {
            int tw = lm_font_text_width(lines[l]), tx = x0 + (CW - tw) / 2, ty = y0 + 6 + ISZ + 4 + l * 15;
            if (it->sel)
                fill_alpha(w, tx - 3, ty - 1, tw + 6, 16, 5, lm_theme.accent, 0xd8);
            else
                wlwin_text(w, tx + 1, ty + 1, lines[l], 1, 0x000000, 0);
            wlwin_text(w, tx, ty, lines[l], 1, it->sel ? 0xffffff : 0xffffff, 0);
        }
    }
    if (a->banding) {
        int x0 = (int)LM_MIN(a->down_x, a->px), y0 = (int)LM_MIN(a->down_y, a->py), x1 = (int)LM_MAX(a->down_x, a->px), y1 = (int)LM_MAX(a->down_y, a->py);
        fill_alpha(w, x0 - 1, y0 - 1, x1 - x0 + 2, y1 - y0 + 2, 2, lm_theme.accent, 0x90);
        fill_alpha(w, x0, y0, x1 - x0, y1 - y0, 2, lm_theme.accent, 0x30);
    }
    if (a->msg[0] && now_ms() < a->msg_until) {
        int tw = lm_font_text_width(a->msg) + 24;
        int mx = (w->bw - tw) / 2, my = w->bh - BOTTOM_ROOM - 34;
        fill_alpha(w, mx, my, tw, 26, 13, 0x101520, 0xdc);
        wlwin_text(w, mx + 12, my + 5, a->msg, 1, 0xffffff, 0);
    }
    if (a->prompt != P_NONE)
        paint_prompt(a, w);
    const char *dump = getenv("MELLAGUI_DESKTOP_DUMP");
    if (dump && *dump) {
        char tmp[PATH_MAX + 8];
        snprintf(tmp, sizeof(tmp), "%s.new", dump);
        FILE *f = fopen(tmp, "w");
        if (f) {
            int nsel = 0;
            for (int i = 0; i < a->n; i++)
                nsel += a->it[i].sel;
            fprintf(f, "state n=%d sel=%d cols=%d rows=%d bw=%d bh=%d scale=%d prompt=%d field=%s menu=%d dragging=%d banding=%d dir=%s\n", a->n, nsel, a->cols, a->rows, w->bw, w->bh,
                    wlwin_scale(w), a->prompt, a->field, a->menu, a->dragging, a->banding, a->dir);
            for (int i = 0; i < a->n; i++) {
                int x, y;
                char nm[NAME_MAX + 1], sh[160];                 /* (a blank is a _ in the dump: it is split at blanks) */
                snprintf(nm, sizeof(nm), "%s", a->it[i].name);
                snprintf(sh, sizeof(sh), "%s", a->it[i].shown);
                for (char *q = nm; *q; q++) if (*q == ' ') *q = '_';
                for (char *q = sh; *q; q++) if (*q == ' ') *q = '_';
                cell_rect(a, a->it[i].cx, a->it[i].cy, &x, &y);
                fprintf(f, "item %d %s kind=%d cx=%d cy=%d x=%d y=%d w=%d h=%d sel=%d shown=%s\n", i, nm, a->it[i].kind, a->it[i].cx, a->it[i].cy, x, y, CW, CH, a->it[i].sel, sh);
            }
            if (a->prompt != P_NONE) {
                struct wlcard c = { 0, 0, 420, 168 };
                wlcard_center(w, &c);
                for (int k = 0; k < 2; k++) {
                    int bx, by, bw, bh;
                    wlcard_button_rect(&c, k, 2, &bx, &by, &bw, &bh);
                    fprintf(f, "cbtn %d %d %d %d %d\n", k, bx, by, bw, bh);
                }
            }
            fclose(f);
            rename(tmp, dump);
        }
    }
}

/* ---- starting programs ------------------------------------------------------------------------------------------------------------- */

static void sibling(const char *name, char *out, size_t outsz)
{
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = 0;
        char *slash = strrchr(exe, '/');
        if (slash && (size_t)(slash + 1 - exe) + strlen(name) < sizeof(exe)) {
            strcpy(slash + 1, name);
            if (access(exe, X_OK) == 0) {
                snprintf(out, outsz, "%s", exe);
                return;
            }
        }
    }
    snprintf(out, outsz, "%s", name);
}

static bool in_path(const char *prog)
{
    const char *p = getenv("PATH");
    char buf[PATH_MAX];
    while (p && *p) {
        size_t l = strcspn(p, ":");
        snprintf(buf, sizeof(buf), "%.*s/%s", (int)l, p, prog);
        if (access(buf, X_OK) == 0)
            return true;
        p += l + (p[l] ? 1 : 0);
    }
    return false;
}

/* Starts argv[0] (a MellaGUI program is looked for next to this one first) and lets go of it: a double fork, so nobody is left waiting for it. */
static void spawn(char *const argv[], const char *cwd)
{
    char prog[PATH_MAX];
    sibling(argv[0], prog, sizeof(prog));
    pid_t pid = fork();
    if (pid < 0)
        return;
    if (pid == 0) {
        setsid();
        if (fork() != 0)
            _exit(0);
        if (cwd && chdir(cwd) < 0) {}
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) {
            dup2(nul, 0);
            dup2(nul, 1);
            dup2(nul, 2);
            if (nul > 2)
                close(nul);
        }
        execvp(prog, argv);
        _exit(127);
    }
    int st;
    waitpid(pid, &st, 0);
}

static void open_item(struct app *a, int i)
{
    struct item *it = &a->it[i];
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", a->dir, it->name);
    if (it->broken) {
        say(a, "%s is a link to nothing", it->name);
        return;
    }
    switch (it->kind) {
    case K_DIR: spawn((char *[]){ "mellagui-files", path, NULL }, a->dir); break;
    case K_IMAGE: case K_VIDEO: case K_AUDIO: spawn((char *[]){ "mellagui-view", path, NULL }, a->dir); break;
    case K_ARCHIVE: spawn((char *[]){ "mellagui-archive", path, NULL }, a->dir); break;
    case K_PROGRAM: spawn((char *[]){ path, NULL }, a->dir); break;
    case K_LAUNCHER: {
        char cmd[512];
        exec_clean(it->exec, cmd, sizeof(cmd));
        if (!cmd[0]) {
            say(a, "%s has no program to start", it->name);
            break;
        }
        if (it->terminal)
            spawn((char *[]){ "mellagui-term", "-e", "sh", "-c", cmd, NULL }, a->dir);
        else
            spawn((char *[]){ "sh", "-c", cmd, NULL }, a->dir);
        break;
    }
    case K_TEXT: case K_CODE: spawn((char *[]){ "mellagui-edit", path, NULL }, a->dir); break;
    case K_PDF: case K_OTHER:
        if (in_path("xdg-open"))
            spawn((char *[]){ "xdg-open", path, NULL }, a->dir);
        else
            say(a, "No program is set up to open %s", it->name);
        break;
    }
}

static void open_in_files(struct app *a, const char *path)
{
    spawn((char *[]){ "mellagui-files", (char *)path, NULL }, a->dir);
}

/* ---- the trash and the file operations ------------------------------------------------------------------------------------------------ */

static void trash_dir(char *out, size_t n)
{
    const char *x = getenv("XDG_DATA_HOME"), *h = getenv("HOME");
    if (x && *x)
        snprintf(out, n, "%s/Trash", x);
    else
        snprintf(out, n, "%s/.local/share/Trash", h && *h ? h : "");
}

static void mkdirs(const char *path)
{
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0700);
            *p = '/';
        }
    mkdir(tmp, 0700);
}

static void url_encode(const char *in, char *out, size_t n)
{
    size_t k = 0;
    for (; *in && k + 4 < n; in++) {
        unsigned char c = (unsigned char)*in;
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '/' || c == '-' || c == '_' || c == '.' || c == '~')
            out[k++] = (char)c;
        else
            k += (size_t)snprintf(out + k, n - k, "%%%02X", c);
    }
    out[k] = 0;
}

/* Into the trash with a .trashinfo so the file manager can put it back. 0, or -1 with the reason in err. */
static int trash_path(const char *src, char *err, size_t errsz)
{
    char trash[PATH_MAX], files[PATH_MAX + 8], info[PATH_MAX + 8];
    trash_dir(trash, sizeof(trash));
    snprintf(files, sizeof(files), "%s/files", trash);
    snprintf(info, sizeof(info), "%s/info", trash);
    mkdirs(files);
    mkdirs(info);
    const char *base = strrchr(src, '/');
    base = base ? base + 1 : src;
    for (int k = 1; k < 1000; k++) {
        char name[NAME_MAX + 16], dst[PATH_MAX * 2], ipath[PATH_MAX * 2];
        if (k == 1)
            snprintf(name, sizeof(name), "%.*s", NAME_MAX, base);
        else
            snprintf(name, sizeof(name), "%.*s.%d", NAME_MAX - 5, base, k);
        snprintf(dst, sizeof(dst), "%s/%s", files, name);
        snprintf(ipath, sizeof(ipath), "%s/%s.trashinfo", info, name);
        struct stat st;
        if (lstat(dst, &st) == 0)
            continue;
        int fd = open(ipath, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0) {
            if (errno == EEXIST)
                continue;
            snprintf(err, errsz, "cannot write the trash info: %s", strerror(errno));
            return -1;
        }
        char enc[PATH_MAX * 3], date[32], body[PATH_MAX * 3 + 96];
        url_encode(src, enc, sizeof(enc));
        time_t now = time(NULL);
        struct tm lt;
        localtime_r(&now, &lt);
        strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", &lt);
        int bl = snprintf(body, sizeof(body), "[Trash Info]\nPath=%s\nDeletionDate=%s\n", enc, date);
        bool ok = write(fd, body, (size_t)bl) == bl;
        ok = close(fd) == 0 && ok;
        if (!ok || rename(src, dst) < 0) {
            snprintf(err, errsz, "cannot move %s to the trash: %s", base, !ok ? "the info file failed" : strerror(errno));
            unlink(ipath);
            return -1;
        }
        return 0;
    }
    snprintf(err, errsz, "no free name in the trash for %s", base);
    return -1;
}

static int count_sel(const struct app *a)
{
    int n = 0;
    for (int i = 0; i < a->n; i++)
        n += a->it[i].sel;
    return n;
}

static void trash_selected(struct app *a)
{
    int done = 0;
    char err[300] = "";
    for (int i = 0; i < a->n; i++) {
        if (!a->it[i].sel)
            continue;
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", a->dir, a->it[i].name);
        if (trash_path(path, err, sizeof(err)) == 0) {
            places_forget(a->it[i].name);
            done++;
        } else {
            break;
        }
    }
    scan(a);
    if (err[0])
        say(a, "%s", err);
    else if (done)
        say(a, "Moved %d item%s to the trash", done, done == 1 ? "" : "s");
}

static bool name_ok(const char *s)
{
    return s[0] && strcmp(s, ".") && strcmp(s, "..") && !strchr(s, '/');
}

static void unique_name(const char *dir, const char *base, const char *suffix, char *out, size_t n)
{
    for (int k = 1; k < 1000; k++) {
        if (k == 1)
            snprintf(out, n, "%s%s", base, suffix);
        else
            snprintf(out, n, "%s %d%s", base, k, suffix);
        char path[PATH_MAX];
        struct stat st;
        snprintf(path, sizeof(path), "%s/%s", dir, out);
        if (lstat(path, &st) < 0)
            return;
    }
}

static void prompt_open(struct app *a, enum prompt p, const char *text)
{
    a->prompt = p;
    snprintf(a->field, sizeof(a->field), "%s", text);
    wltext_end(a->field);
    wltext_select_all(a->field);
    a->perr[0] = 0;
    a->hot_btn = -1;
    a->win.redraw = true;
}

static void prompt_done(struct app *a)
{
    if (!name_ok(a->field)) {
        snprintf(a->perr, sizeof(a->perr), "That is not a name for a file");
        return;
    }
    char to[PATH_MAX];
    snprintf(to, sizeof(to), "%s/%s", a->dir, a->field);
    struct stat st;
    if (a->prompt == P_RENAME) {
        char from[PATH_MAX];
        snprintf(from, sizeof(from), "%s/%s", a->dir, a->target);
        if (!strcmp(a->field, a->target)) {
            a->prompt = P_NONE;
            return;
        }
        if (lstat(to, &st) == 0) {
            snprintf(a->perr, sizeof(a->perr), "%s is there already", a->field);
            return;
        }
        if (rename(from, to) < 0) {
            snprintf(a->perr, sizeof(a->perr), "%s", strerror(errno));
            return;
        }
        const struct place *p = places_get(a->target);
        if (p)
            places_set(a->field, p->cx, p->cy);
        places_forget(a->target);
        snprintf(a->pending_select, sizeof(a->pending_select), "%s", a->field);
    } else if (a->prompt == P_NEWDIR) {
        if (lstat(to, &st) == 0) {
            snprintf(a->perr, sizeof(a->perr), "%s is there already", a->field);
            return;
        }
        if (mkdir(to, 0777) < 0) {
            snprintf(a->perr, sizeof(a->perr), "%s", strerror(errno));
            return;
        }
        snprintf(a->pending_select, sizeof(a->pending_select), "%s", a->field);
    } else {
        int fd = open(to, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
        if (fd < 0) {
            snprintf(a->perr, sizeof(a->perr), "%s", errno == EEXIST ? "That name is taken" : strerror(errno));
            return;
        }
        close(fd);
        snprintf(a->pending_select, sizeof(a->pending_select), "%s", a->field);
    }
    a->prompt = P_NONE;
    scan(a);
    places_save(a);
}

/* ---- moving icons -------------------------------------------------------------------------------------------------------------------- */

/* Puts what is chosen into the folder `into` (an item of kind K_DIR). */
static void move_into(struct app *a, int into)
{
    int done = 0;
    char err[200] = "";
    for (int i = 0; i < a->n; i++) {
        if (!a->it[i].sel || i == into)
            continue;
        char from[PATH_MAX], to[PATH_MAX];
        snprintf(from, sizeof(from), "%s/%s", a->dir, a->it[i].name);
        snprintf(to, sizeof(to), "%s/%s/%s", a->dir, a->it[into].name, a->it[i].name);
        struct stat st;
        if (lstat(to, &st) == 0) {
            snprintf(err, sizeof(err), "%s is in %s already", a->it[i].name, a->it[into].name);
            continue;
        }
        if (rename(from, to) < 0) {
            snprintf(err, sizeof(err), "%s: %s", a->it[i].name, strerror(errno));
            continue;
        }
        places_forget(a->it[i].name);
        done++;
    }
    scan(a);
    if (err[0])
        say(a, "%s", err);
    else if (done)
        say(a, "Moved %d item%s into the folder", done, done == 1 ? "" : "s");
}

static void arrange_all(struct app *a)
{
    g_nplaces = 0;
    place_items(a);
    places_save(a);
    a->win.redraw = true;
}

/* The pointer let go while icons were carried: they take the cell it is over (all by the same step), or go into a folder it is over. */
static void drop_icons(struct app *a)
{
    a->dragging = false;
    int cx = (int)((a->px - MARGIN) / CW), cy = (int)((a->py - MARGIN) / CH);
    if (a->px < MARGIN || a->py < MARGIN)
        return;
    int base = a->down_item;
    if (base < 0 || !cell_in(a, cx, cy))
        return;
    int over = item_at_cell(a, cx, cy, -1);
    if (over >= 0 && !a->it[over].sel && a->it[over].kind == K_DIR && !a->it[over].broken) {
        move_into(a, over);
        return;
    }
    int dx = cx - a->it[base].cx, dy = cy - a->it[base].cy;
    if (!dx && !dy)
        return;
    for (int i = 0; i < a->n; i++) {                            /* every chosen one must land on a free cell (or one that a chosen one leaves) */
        if (!a->it[i].sel)
            continue;
        int nx = a->it[i].cx + dx, ny = a->it[i].cy + dy;
        int o = cell_in(a, nx, ny) ? item_at_cell(a, nx, ny, i) : -2;
        if (o == -2 || (o >= 0 && !a->it[o].sel)) {
            say(a, "There is something in the way");
            return;
        }
    }
    for (int i = 0; i < a->n; i++)
        if (a->it[i].sel) {
            a->it[i].cx += dx;
            a->it[i].cy += dy;
        }
    for (int i = 0; i < a->n; i++)
        places_set(a->it[i].name, a->it[i].cx, a->it[i].cy);
    places_save(a);
}

/* ---- the pointer ----------------------------------------------------------------------------------------------------------------------- */

static int item_under(const struct app *a, double x, double y)
{
    if (x < MARGIN || y < MARGIN)
        return -1;
    int cx = (int)((x - MARGIN) / CW), cy = (int)((y - MARGIN) / CH);
    if (!cell_in(a, cx, cy))
        return -1;
    return item_at_cell(a, cx, cy, -1);
}

static void select_only(struct app *a, int keep)
{
    for (int i = 0; i < a->n; i++)
        a->it[i].sel = i == keep;
}

/* The place of an item in the reading order of the grid: column after column. */
static int cell_rank(const struct app *a, int i)
{
    return a->it[i].cx < 0 ? -1 : a->it[i].cx * 1000 + a->it[i].cy;
}

/* Chooses every item between two (in the order of the grid); the others are let go unless `keep`. */
static void select_range(struct app *a, int from, int to, bool keep)
{
    int r0 = LM_MIN(cell_rank(a, from), cell_rank(a, to)), r1 = LM_MAX(cell_rank(a, from), cell_rank(a, to));
    for (int i = 0; i < a->n; i++) {
        int r = cell_rank(a, i);
        bool in = r >= 0 && r >= r0 && r <= r1;
        a->it[i].sel = keep ? (a->it[i].sel || in) : in;
    }
}

static void band_select(struct app *a)
{
    double x0 = LM_MIN(a->down_x, a->px), x1 = LM_MAX(a->down_x, a->px), y0 = LM_MIN(a->down_y, a->py), y1 = LM_MAX(a->down_y, a->py);
    for (int i = 0; i < a->n; i++) {
        if (a->it[i].cx < 0)
            continue;
        int cx, cy;
        cell_rect(a, a->it[i].cx, a->it[i].cy, &cx, &cy);
        bool hit = cx + CW - 6 > x0 && cx + 6 < x1 && cy + CH - 6 > y0 && cy + 6 < y1;
        if (a->band_add)
            a->it[i].sel = a->it[i].sel || hit;
        else
            a->it[i].sel = hit;
    }
}

static void menu_show(struct app *a, enum menu_what what, int item)
{
    a->menu = what;
    a->menu_item = item;
    if (what == M_ITEM) {
        int n = count_sel(a);
        char entries[256];
        snprintf(entries, sizeof(entries), "Open\n%s\nShow in the file manager\n%s", n > 1 ? "~Rename..." : "Rename...",
                 n > 1 ? "Move the items to the trash" : "Move to the trash");
        if (!wlwin_menu(&a->win, entries))
            a->menu = M_NONE;
    } else {
        if (!wlwin_menu(&a->win, "New folder\nNew text file\nSelect all\nArrange the icons\nOpen the Desktop folder\nChange the wallpaper..."))
            a->menu = M_NONE;
    }
}

static void menu_event(struct wlwin *w, int index)
{
    struct app *a = w->data;
    enum menu_what what = a->menu;
    a->menu = M_NONE;
    if (index < 0)
        return;
    if (what == M_ITEM) {
        char path[PATH_MAX];
        int it = a->menu_item;
        if (it < 0 || it >= a->n)
            return;
        snprintf(path, sizeof(path), "%s/%s", a->dir, a->it[it].name);
        if (index == 0) {
            for (int i = 0; i < a->n; i++)
                if (a->it[i].sel)
                    open_item(a, i);
        } else if (index == 1 && count_sel(a) == 1) {
            snprintf(a->target, sizeof(a->target), "%s", a->it[it].name);
            prompt_open(a, P_RENAME, a->it[it].name);
        } else if (index == 2) {
            open_in_files(a, path);
        } else if (index == 3) {
            trash_selected(a);
        }
    } else if (what == M_EMPTY) {
        char name[NAME_MAX + 1];
        if (index == 0) {
            unique_name(a->dir, "New folder", "", name, sizeof(name));
            prompt_open(a, P_NEWDIR, name);
        } else if (index == 1) {
            unique_name(a->dir, "New file", ".txt", name, sizeof(name));
            prompt_open(a, P_NEWFILE, name);
        } else if (index == 2) {
            for (int i = 0; i < a->n; i++)
                a->it[i].sel = true;
        } else if (index == 3) {
            arrange_all(a);
        } else if (index == 4) {
            open_in_files(a, a->dir);
        } else if (index == 5) {
            spawn((char *[]){ "mellagui-settings", NULL }, NULL);
        }
    }
    w->redraw = true;
}

static void pointer_event(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
    struct app *a = w->data;
    if (op == LM_WL_POINTER_EVT_ENTER || op == LM_WL_POINTER_EVT_MOTION) {
        a->px = lm_fixed_to_double(args[op == LM_WL_POINTER_EVT_ENTER ? 2 : 1].f);
        a->py = lm_fixed_to_double(args[op == LM_WL_POINTER_EVT_ENTER ? 3 : 2].f);
        if (a->prompt != P_NONE) {
            struct wlcard c = { 0, 0, 420, 168 };
            wlcard_center(w, &c);
            if (wlcard_drag_motion(w, &c, a->px, a->py)) {
                w->redraw = true;
                return;
            }
            int hb = wlcard_button_at(&c, 2, (int)a->px, (int)a->py);
            if (hb != a->hot_btn) {
                a->hot_btn = hb;
                w->redraw = true;
            }
            return;
        }
        if (a->down && !a->dragging && !a->banding && (fabs(a->px - a->down_x) > HOLD_PX || fabs(a->py - a->down_y) > HOLD_PX)) {
            if (a->down_item >= 0) {
                if (!a->it[a->down_item].sel)
                    select_only(a, a->down_item);
                a->dragging = true;
            } else {
                a->banding = true;
                if (!a->band_add)
                    for (int i = 0; i < a->n; i++)
                        a->it[i].sel = false;
            }
        }
        if (a->banding)
            band_select(a);
        int h = a->down ? -1 : item_under(a, a->px, a->py);
        if (h != a->hover || a->dragging || a->banding) {
            a->hover = h;
            w->redraw = true;
        }
    } else if (op == LM_WL_POINTER_EVT_LEAVE) {
        a->hover = -1;
        w->redraw = true;
    } else if (op == LM_WL_POINTER_EVT_BUTTON) {
        uint32_t button = args[2].u, state = args[3].u;
        if (a->prompt != P_NONE) {
            struct wlcard c = { 0, 0, 420, 168 };
            wlcard_center(w, &c);
            if (state == 0) {
                wlcard_drag_release(w);
                return;
            }
            if (button != 272)
                return;
            if (wlcard_drag_press(w, &c, a->px, a->py))
                return;
            int b = wlcard_button_at(&c, 2, (int)a->px, (int)a->py);
            if (b == 0 || !wlcard_has(&c, (int)a->px, (int)a->py))
                a->prompt = P_NONE;
            else if (b == 1)
                prompt_done(a);
            else if (a->px >= c.x + WLCARD_PAD && a->px < c.x + c.w - WLCARD_PAD && a->py >= c.y + 48 && a->py < c.y + 76)
                wltext_click(a->field, (int)a->px - (c.x + WLCARD_PAD + 12), (w->mods & LM_MOD_SHIFT) != 0);
            w->redraw = true;
            return;
        }
        if (button == 272 && state == 1) {
            int i = item_under(a, a->px, a->py);
            bool ctrl = (w->mods & LM_MOD_CTRL) != 0;
            a->down = true;
            a->down_item = i;
            a->down_x = a->px;
            a->down_y = a->py;
            a->band_add = ctrl;
            bool shift = (w->mods & LM_MOD_SHIFT) != 0;
            if (i >= 0 && shift && a->anchor >= 0 && a->anchor < a->n) {
                select_range(a, a->anchor, i, ctrl);
                a->cur = i;
            } else if (i >= 0) {
                if (ctrl)
                    a->it[i].sel = !a->it[i].sel;
                else if (!a->it[i].sel)
                    select_only(a, i);
                a->anchor = a->cur = i;
            } else if (!ctrl) {
                for (int k = 0; k < a->n; k++)
                    a->it[k].sel = false;
            }
            w->redraw = true;
        } else if (button == 272 && state == 0 && a->down) {
            a->down = false;
            uint32_t t = now_ms();
            if (a->dragging) {
                drop_icons(a);
            } else if (a->banding) {
                a->banding = false;
            } else if (a->down_item >= 0) {
                bool ctrl = (w->mods & LM_MOD_CTRL) != 0, shift = (w->mods & LM_MOD_SHIFT) != 0;
                if (shift) {
                    /* a range was chosen at the press */
                } else if (!ctrl && a->click_item == a->down_item && t - a->click_ms < (uint32_t)lm_metrics.double_click_ms) {
                    open_item(a, a->down_item);
                    a->click_item = -1;
                } else {
                    if (!ctrl)
                        select_only(a, a->down_item);
                    a->click_item = a->down_item;
                    a->click_ms = t;
                }
            }
            a->dragging = a->banding = false;
            a->hover = item_under(a, a->px, a->py);
            w->redraw = true;
        } else if (button == 273 && state == 1) {
            int i = item_under(a, a->px, a->py);
            if (i >= 0) {
                if (!a->it[i].sel)
                    select_only(a, i);
                menu_show(a, M_ITEM, i);
            } else {
                for (int k = 0; k < a->n; k++)
                    a->it[k].sel = false;
                menu_show(a, M_EMPTY, -1);
            }
            w->redraw = true;
        }
    }
}

/* ---- the keyboard ---------------------------------------------------------------------------------------------------------------------- */

static int first_selected(const struct app *a)
{
    for (int i = a->n - 1; i >= 0; i--)
        if (a->it[i].sel)
            return i;
    return -1;
}

/* The neighbour in a direction (dx, dy of -1, 0, 1): the nearest cell in that direction, the same row or column first. */
static int neighbour(const struct app *a, int from, int dx, int dy)
{
    int best = -1;
    long bs = 0;
    for (int i = 0; i < a->n; i++) {
        if (i == from || a->it[i].cx < 0)
            continue;
        int ex = a->it[i].cx - a->it[from].cx, ey = a->it[i].cy - a->it[from].cy;
        long along = ex * dx + ey * dy, across = labs((long)(ex * dy)) + labs((long)(ey * dx));
        if (along <= 0)
            continue;
        long s = along * 100 + across;
        if (best < 0 || s < bs) {
            best = i;
            bs = s;
        }
    }
    return best;
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
    bool ctrl = (w->mods & LM_MOD_CTRL) != 0;
    if (a->prompt != P_NONE) {
        if (ks == LM_KS_Escape)
            a->prompt = P_NONE;
        else if (ks == LM_KS_Return || ks == LM_KS_KP_Enter)
            prompt_done(a);
        else if (wltext_edit(&a->clip, a->field, sizeof(a->field), ks, w->mods))
            a->perr[0] = 0;
        w->redraw = true;
        return;
    }
    int cur = first_selected(a);
    if (ks == LM_KS_Escape) {
        for (int i = 0; i < a->n; i++)
            a->it[i].sel = false;
    } else if (ctrl && (ks == 'a' || ks == 'A')) {
        for (int i = 0; i < a->n; i++)
            a->it[i].sel = true;
    } else if (ks == LM_KS_Right || ks == LM_KS_Left || ks == LM_KS_Down || ks == LM_KS_Up) {
        int dx = ks == LM_KS_Right ? 1 : ks == LM_KS_Left ? -1 : 0, dy = ks == LM_KS_Down ? 1 : ks == LM_KS_Up ? -1 : 0;
        int from = a->cur >= 0 && a->cur < a->n ? a->cur : cur;
        int nx = from < 0 ? (a->n ? 0 : -1) : neighbour(a, from, dx, dy);
        if (nx >= 0) {
            if ((w->mods & LM_MOD_SHIFT) && from >= 0) {
                select_range(a, a->anchor >= 0 && a->anchor < a->n ? a->anchor : from, nx, false);
            } else {
                select_only(a, nx);
                a->anchor = nx;
            }
            a->cur = nx;
        }
    } else if ((ks == LM_KS_Return || ks == LM_KS_KP_Enter) && cur >= 0) {
        for (int i = 0; i < a->n; i++)
            if (a->it[i].sel)
                open_item(a, i);
    } else if (ks == LM_KS_F1 + 1 && count_sel(a) == 1) {
        snprintf(a->target, sizeof(a->target), "%s", a->it[cur].name);
        prompt_open(a, P_RENAME, a->it[cur].name);
    } else if (ks == LM_KS_Delete && cur >= 0) {
        trash_selected(a);
    } else if (ks == LM_KS_F1 + 4) {
        scan(a);
    } else if (!ctrl && !(w->mods & LM_MOD_ALT) && ((ks > 0x20 && ks < 0x7f) || (ks == 0x20 && a->ta[0]))) {
        uint32_t t = now_ms();                                  /* type-ahead: the first icon whose name starts with what was typed in the last second */
        if (t - a->ta_ms > 1000)
            a->ta[0] = 0;
        a->ta_ms = t;
        size_t l = strlen(a->ta);
        if (l + 1 < sizeof(a->ta)) {
            a->ta[l] = (char)ks;
            a->ta[l + 1] = 0;
            l++;
        }
        int hit = -1;
        for (int i = 0; i < a->n && hit < 0; i++)
            if (a->it[i].cx >= 0 && !strncasecmp(a->it[i].shown, a->ta, l))
                hit = i;
        if (hit >= 0) {
            select_only(a, hit);
            a->anchor = a->cur = hit;
        }
    }
    w->redraw = true;
}

/* Files dragged here from another program (the file manager) are copied into the desktop folder, or into the folder icon they were let go on. A file that is
 * in that folder already is left alone. cp does the work in the background: the folder is watched, the icons appear as the files do. */
static void on_drop(struct wlclip *c, char **paths, int n, double x, double y, void *data)
{
    (void)c;
    struct app *a = data;
    char target[PATH_MAX];
    int over = item_under(a, x, y);
    if (over >= 0 && a->it[over].kind == K_DIR && !a->it[over].broken)
        snprintf(target, sizeof(target), "%s/%s", a->dir, a->it[over].name);
    else
        snprintf(target, sizeof(target), "%s", a->dir);
    char **argv = calloc((size_t)n + 6, sizeof(*argv));
    if (!argv)
        return;
    int k = 0, kept = 0;
    argv[k++] = "cp";
    argv[k++] = "-a";
    argv[k++] = "-n";
    argv[k++] = "--";
    for (int i = 0; i < n; i++) {
        char dir[PATH_MAX];
        snprintf(dir, sizeof(dir), "%s", paths[i]);
        char *slash = strrchr(dir, '/');
        if (slash && slash != dir)
            *slash = 0;
        if (!strcmp(dir, target))
            continue;                                           /* (it is there already) */
        argv[k++] = paths[i];
        kept++;
    }
    argv[k++] = target;
    if (kept) {
        spawn(argv, NULL);
        say(a, "Copying %d item%s", kept, kept == 1 ? "" : "s");
    } else {
        say(a, "That is on the desktop already");
    }
    free(argv);
}

/* Alt+F4 or a close shortcut with the desktop focused must not end it: nothing closes the desktop. */
static bool keep_open(struct wlwin *w)
{
    (void)w;
    return false;
}

static void global_event(struct wlwin *w, uint32_t name, const char *iface, uint32_t ver)
{
    struct app *a = w->data;
    wlclip_global(&a->clip, w, name, iface, ver);
}

/* ---- the folder is watched ------------------------------------------------------------------------------------------------------------- */

static void watch_start(struct app *a)
{
    a->ino = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (a->ino >= 0 && inotify_add_watch(a->ino, a->dir, IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_ATTRIB | IN_CLOSE_WRITE) < 0) {
        close(a->ino);
        a->ino = -1;
    }
}

/* True when something changed (the events are read away). */
static bool watch_changed(struct app *a)
{
    if (a->ino < 0)
        return false;
    char buf[4096];
    bool any = false;
    ssize_t k;
    while ((k = read(a->ino, buf, sizeof(buf))) > 0)
        any = true;
    return any;
}

/* ---- the program ------------------------------------------------------------------------------------------------------------------------- */

static int selftest(void)
{
    int bad = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "mellagui-desktop: selftest failed: %s (line %d)\n", #c, __LINE__); bad++; } } while (0)
    char cmd[64];
    exec_clean("firefox %u --new-window %%f %F", cmd, sizeof(cmd));
    CHECK(!strcmp(cmd, "firefox --new-window %f"));
    char ext[8];
    file_ext("photo.JPG", ext, sizeof(ext));
    CHECK(!strcmp(ext, "JPG"));
    file_ext(".bashrc", ext, sizeof(ext));
    CHECK(!ext[0]);
    file_ext("a.verylongextension", ext, sizeof(ext));
    CHECK(!ext[0]);
    bool ex;
    CHECK(classify("/nonexistent", "x.png", &ex) == K_IMAGE);
    CHECK(classify("/nonexistent", "x.desktop", &ex) == K_LAUNCHER);
    CHECK(classify("/nonexistent", "x.zip", &ex) == K_ARCHIVE);
    CHECK(name_ok("a b") && !name_ok("") && !name_ok("..") && !name_ok("a/b"));
    char enc[64];
    url_encode("/home/a b/ä", enc, sizeof(enc));
    CHECK(!strcmp(enc, "/home/a%20b/%C3%A4"));
    /* the grid: items go column after column into free cells, a saved place is kept when it is free */
    struct app a = { 0 };
    a.win.bw = MARGIN * 2 + CW * 3;
    a.win.bh = MARGIN + BOTTOM_ROOM + CH * 2;
    a.n = 5;
    a.it = calloc(5, sizeof(*a.it));
    for (int i = 0; i < 5; i++)
        snprintf(a.it[i].name, sizeof(a.it[i].name), "f%d", i);
    places_set("f2", 2, 1);
    places_set("f3", 9, 9);                                     /* outside the grid: placed again */
    place_items(&a);
    CHECK(a.cols == 3 && a.rows == 2);
    CHECK(a.it[2].cx == 2 && a.it[2].cy == 1);
    CHECK(a.it[0].cx == 0 && a.it[0].cy == 0 && a.it[1].cx == 0 && a.it[1].cy == 1);
    CHECK(a.it[3].cx == 1 && a.it[3].cy == 0 && a.it[4].cx == 1 && a.it[4].cy == 1);
    for (int i = 0; i < 5; i++)
        for (int j = i + 1; j < 5; j++)
            CHECK(a.it[i].cx != a.it[j].cx || a.it[i].cy != a.it[j].cy);
    CHECK(cell_rank(&a, 3) == 1000 && cell_rank(&a, 1) == 1);
    select_range(&a, 0, 3, false);
    CHECK(a.it[0].sel && a.it[1].sel && a.it[3].sel && !a.it[2].sel && !a.it[4].sel);
    CHECK(neighbour(&a, 0, 1, 0) == 3 && neighbour(&a, 0, 0, 1) == 1 && neighbour(&a, 0, -1, 0) < 0);
    free(a.it);
    /* the icon pictures: every kind has a body and see-through corners */
    int P = 96;
    uint32_t *buf = calloc((size_t)P * P, 4);
    icon_folder(buf, P);
    CHECK((buf[(size_t)(P / 2) * P + P / 2] >> 24) == 255 && (buf[0] >> 24) == 0);
    memset(buf, 0, (size_t)P * P * 4);
    icon_page(buf, P, K_IMAGE, false);
    CHECK((buf[(size_t)(P / 2) * P + P / 2] >> 24) == 255 && (buf[0] >> 24) == 0);
    memset(buf, 0, (size_t)P * P * 4);
    icon_dock(buf, P, LM_DOCKICON_GENERIC);
    CHECK((buf[(size_t)(P / 2) * P + P / 2] >> 24) == 255 && (buf[0] >> 24) == 0);
    free(buf);
    free(g_places);
    g_places = NULL;
    g_nplaces = g_capplaces = 0;
    if (!bad)
        printf("mellagui-desktop selftest ok\n");
    return bad ? 1 : 0;
}

int main(int argc, char **argv)
{
    wlwin_hidpi();
    wlwin_argb();                       /* the wallpaper shows between the icons */
    lm_theme_load_env();
    if (argc > 1 && !strcmp(argv[1], "--selftest"))
        return selftest();
    static struct app a;
    desktop_dir(a.dir, sizeof(a.dir));
    if (argc > 1 && !strcmp(argv[1], "--list")) {
        static const char *const names[] = { "folder", "image", "video", "audio", "archive", "pdf", "code", "text", "program", "launcher", "file" };
        DIR *d = opendir(a.dir);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            if (e->d_name[0] == '.')
                continue;
            char path[PATH_MAX];
            struct stat st;
            snprintf(path, sizeof(path), "%s/%s", a.dir, e->d_name);
            bool ex;
            enum kind k = stat(path, &st) == 0 && S_ISDIR(st.st_mode) ? K_DIR : classify(path, e->d_name, &ex);
            printf("%s\t%s\n", names[k], e->d_name);
        }
        if (d)
            closedir(d);
        return 0;
    }
    if (!a.dir[0] || a.dir[0] != '/') {
        fprintf(stderr, "mellagui-desktop: no home folder\n");
        return 1;
    }
    struct stat st;
    if (stat(a.dir, &st) < 0)
        mkdirs(a.dir);                  /* the desktop folder is made when it is not there */
    a.hover = -1;
    a.click_item = -1;
    a.down_item = -1;
    static const struct wlwin_ops ops = { .paint = paint, .global = global_event, .pointer = pointer_event, .keyboard = keyboard_event, .menu = menu_event, .close = keep_open };
    if (wlwin_connect(&a.win, "mellagui-desktop", &ops, &a) < 0)
        return 1;
    if (!a.win.seat) {
        fprintf(stderr, "mellagui-desktop: the compositor lacks wl_seat\n");
        wlwin_destroy(&a.win);
        return 1;
    }
    if (wlwin_open(&a.win, "Desktop", "mellagui.desktop", 1024, 600, 200, 200) < 0)
        return 1;
    wlclip_attach(&a.clip, &a.win);
    wlclip_accept_drops(&a.clip, on_drop, &a);
    places_load();
    a.ino = -1;
    watch_start(&a);
    scan(&a);
    while (wlwin_step(&a.win, a.msg[0] && now_ms() < a.msg_until ? 200 : 300)) {
        if (a.msg[0] && now_ms() >= a.msg_until) {
            a.msg[0] = 0;
            a.win.redraw = true;
        }
        if (watch_changed(&a))
            scan(&a);
        thumbs_lazy(&a);
    }
    items_free(&a);
    free(a.it);
    wlwin_destroy(&a.win);
    return 0;
}
