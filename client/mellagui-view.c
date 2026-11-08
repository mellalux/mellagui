/*
 * mellagui-view - image viewer (PNG, PPM) for MellaGUI, mostly for the screenshots (Print) in ~/Pictures.
 *
 *   mellagui-view [FILE | DIR]     FILE: the images of its directory, starting at FILE; DIR (default ~/Pictures): the
 *                                  newest image (the last name: screenshots are named by date)
 *   mellagui-view --decode FILE OUT.ppm   decode FILE and write it as a PPM (tests)
 *   mellagui-view --selftest
 *
 * Keys: Left/Right (PgUp/PgDn, Backspace/Space) previous / next, Home/End first / last, + - zoom, 0 or 1 = 100 %,
 * F = fit, R = rescan the directory, Esc or Q = quit. Mouse: wheel zooms around the pointer, drag pans, double click
 * toggles fit / 100 %.
 */
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "theme.h"
#include "font.h"
#include "img.h"
#include "keymap.h"
#include "png.h"
#include "protocol.h"
#include "util.h"
#include "wlwin.h"

#define BAR_H 24
#define BG (lm_theme.card)
#define BAR_BG 0x0e1118u
#define BAR_FG 0xd8dde8u
#define ZMIN 0.05
#define ZMAX 32.0

struct app {
    struct wlwin win;
    char dir[PATH_MAX];
    char **names;
    int n, cur;
    struct lm_image img;
    char err[160];
    bool fit;
    double zoom;
    double ox, oy;                      /* top left of the scaled image in view coordinates (when larger than the view) */
    uint32_t *scaled;                   /* box-filtered copy for zoom < 1, valid for scaled_zoom */
    int sw, sh;
    double scaled_zoom;
    bool has_pointer, dragging;
    double px, py, drag_x, drag_y;
    uint32_t last_click_ms;
    double last_click_x, last_click_y;
};

/* ---- directory ---------------------------------------------------------------------------------------------------- */

static bool is_image_name(const char *n)
{
    size_t l = strlen(n);
    return n[0] != '.' && l > 4 && (!strcasecmp(n + l - 4, ".png") || !strcasecmp(n + l - 4, ".ppm") || !strcasecmp(n + l - 4, ".jpg") ||
                                    (l > 5 && !strcasecmp(n + l - 5, ".jpeg")));
}

static int cmp_names(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void free_names(struct app *a)
{
    for (int i = 0; i < a->n; i++)
        free(a->names[i]);
    free(a->names);
    a->names = NULL;
    a->n = 0;
}

static void scan_dir(struct app *a, const char *keep)           /* keep: name to stay on, else the last image */
{
    free_names(a);
    DIR *d = opendir(a->dir);
    int cap = 0;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!is_image_name(e->d_name) && !(keep && !strcmp(e->d_name, keep)))
                continue;
            if (a->n == cap) {
                cap = cap ? cap * 2 : 32;
                a->names = realloc(a->names, (size_t)cap * sizeof(*a->names));
            }
            a->names[a->n++] = strdup(e->d_name);
        }
        closedir(d);
    }
    if (a->n > 1)
        qsort(a->names, (size_t)a->n, sizeof(*a->names), cmp_names);
    a->cur = a->n ? a->n - 1 : 0;
    for (int i = 0; keep && i < a->n; i++)
        if (!strcmp(a->names[i], keep))
            a->cur = i;
}

/* ---- image, zoom ---------------------------------------------------------------------------------------------------- */

static int view_w(const struct app *a) { return a->win.bw; }
static int view_h(const struct app *a) { return a->win.bh > BAR_H ? a->win.bh - BAR_H : 1; }

static double fit_zoom(const struct app *a)
{
    if (!a->img.px)
        return 1.0;
    double z = LM_MIN((double)view_w(a) / a->img.w, (double)view_h(a) / a->img.h);
    return z > 1.0 ? 1.0 : z;
}

static void set_title(struct app *a)
{
    char t[300];
    snprintf(t, sizeof(t), "%s - Images", a->n ? a->names[a->cur] : "no image");
    WLC_SEND(a->win.c, a->win.toplevel, LM_XDG_TOPLEVEL_REQ_SET_TITLE, { .s = t });
}

static void load_current(struct app *a)
{
    lm_img_free(&a->img);
    free(a->scaled);
    a->scaled = NULL;
    a->err[0] = 0;
    if (a->n) {
        char path[PATH_MAX + 300];
        snprintf(path, sizeof(path), "%s/%s", a->dir, a->names[a->cur]);
        if (lm_img_load(path, &a->img, a->err, sizeof(a->err)) < 0)
            a->img.px = NULL;
    }
    a->fit = true;
    a->zoom = fit_zoom(a);
    a->win.redraw = true;
    set_title(a);
}

static double cur_zoom(const struct app *a) { return a->fit ? fit_zoom(a) : a->zoom; }

static void zoom_to(struct app *a, double z, double mx, double my)
{
    if (!a->img.px)
        return;
    double oz = cur_zoom(a);
    z = LM_CLAMP(z, ZMIN, ZMAX);
    /* the image point under (mx, my) stays there */
    double sw = a->img.w * oz, sh = a->img.h * oz;
    double ox = sw <= view_w(a) ? (view_w(a) - sw) / 2 : a->ox, oy = sh <= view_h(a) ? (view_h(a) - sh) / 2 : a->oy;
    a->ox = mx - (mx - ox) * z / oz;
    a->oy = my - (my - oy) * z / oz;
    a->zoom = z;
    a->fit = false;
    a->win.redraw = true;
}

static void go(struct app *a, int idx)
{
    if (!a->n)
        return;
    idx = LM_CLAMP(idx, 0, a->n - 1);
    if (idx == a->cur && a->img.px)
        return;
    a->cur = idx;
    load_current(a);
}

/* ---- painting ------------------------------------------------------------------------------------------------------- */

static void build_scaled(struct app *a, double z)
{
    int sw = LM_MAX((int)(a->img.w * z + 0.5), 1), sh = LM_MAX((int)(a->img.h * z + 0.5), 1);
    free(a->scaled);
    a->scaled = malloc((size_t)sw * (size_t)sh * 4);
    if (!a->scaled)
        return;
    a->sw = sw;
    a->sh = sh;
    a->scaled_zoom = z;
    for (int y = 0; y < sh; y++) {                              /* box filter */
        int y0 = (int)((int64_t)y * a->img.h / sh), y1 = LM_MAX((int)((int64_t)(y + 1) * a->img.h / sh), y0 + 1);
        for (int x = 0; x < sw; x++) {
            int x0 = (int)((int64_t)x * a->img.w / sw), x1 = LM_MAX((int)((int64_t)(x + 1) * a->img.w / sw), x0 + 1);
            uint32_t r = 0, g = 0, b = 0, n = 0;
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++) {
                    uint32_t v = a->img.px[(size_t)yy * (size_t)a->img.w + (size_t)xx];
                    r += (v >> 16) & 255;
                    g += (v >> 8) & 255;
                    b += v & 255;
                    n++;
                }
            a->scaled[(size_t)y * (size_t)sw + (size_t)x] = ((r / n) << 16) | ((g / n) << 8) | (b / n);
        }
    }
}

static void paint(struct wlwin *w)
{
    struct app *a = w->data;
    int vw = view_w(a), vh = view_h(a);
    wlwin_fill(w, 0, 0, w->bw, w->bh, BG);
    char status[400];
    if (a->img.px) {
        double z = cur_zoom(a);
        int sw = LM_MAX((int)(a->img.w * z + 0.5), 1), sh = LM_MAX((int)(a->img.h * z + 0.5), 1);
        int ox = sw <= vw ? (vw - sw) / 2 : (int)LM_CLAMP(a->ox, (double)(vw - sw), 0.0);
        int oy = sh <= vh ? (vh - sh) / 2 : (int)LM_CLAMP(a->oy, (double)(vh - sh), 0.0);
        if (sw > vw)
            a->ox = ox;
        if (sh > vh)
            a->oy = oy;
        const uint32_t *src = a->img.px;
        int stride = a->img.w;
        if (z < 0.999) {
            if (!a->scaled || a->scaled_zoom != z)
                build_scaled(a, z);
            if (a->scaled) {
                src = a->scaled;
                stride = a->sw;
                sw = a->sw;
                sh = a->sh;
            }
        }
        for (int y = LM_MAX(0, -oy); y < sh && oy + y < vh; y++) {
            uint32_t *dst = w->pixels + (size_t)(oy + y) * (size_t)w->bw;
            int x0 = LM_MAX(0, -ox), x1 = LM_MIN(sw, vw - ox);
            if (z > 1.001) {                                    /* nearest neighbour */
                int sy = (int)((double)y / z);
                for (int x = x0; x < x1; x++)
                    dst[ox + x] = src[(size_t)sy * (size_t)stride + (size_t)((double)x / z)];
            } else if (x1 > x0) {
                memcpy(dst + ox + x0, src + (size_t)y * (size_t)stride + (size_t)x0, (size_t)(x1 - x0) * 4);
            }
        }
        snprintf(status, sizeof(status), "%s   %dx%d   %d%%%s   %d/%d", a->names[a->cur], a->img.w, a->img.h,
                 (int)(z * 100 + 0.5), a->fit ? " (fit)" : "", a->cur + 1, a->n);
    } else if (a->n) {
        snprintf(status, sizeof(status), "%s: %s   %d/%d", a->names[a->cur], a->err, a->cur + 1, a->n);
        wlwin_text(w, 16, 16, a->err, 1, 0xff8080u, vw - 32);
    } else {
        snprintf(status, sizeof(status), "no images in %.300s", a->dir);
        wlwin_text(w, 16, 16, "No images (PNG / PPM) here. Press Print to take a screenshot, R to look again.", 1, BAR_FG, vw - 32);
    }
    wlwin_fill(w, 0, vh, w->bw, w->bh - vh, BAR_BG);
    wlwin_text(w, 8, vh + (BAR_H - LM_FONT_H) / 2, status, 1, BAR_FG, w->bw - 16);
}

/* ---- input ---------------------------------------------------------------------------------------------------------- */

static void pointer_event(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
    struct app *a = w->data;
    switch (op) {
    case LM_WL_POINTER_EVT_ENTER:
        a->has_pointer = true;
        a->px = lm_fixed_to_double(args[2].f);
        a->py = lm_fixed_to_double(args[3].f);
        break;
    case LM_WL_POINTER_EVT_LEAVE:
        a->has_pointer = false;
        a->dragging = false;
        break;
    case LM_WL_POINTER_EVT_MOTION: {
        double x = lm_fixed_to_double(args[1].f), y = lm_fixed_to_double(args[2].f);
        if (a->dragging && a->img.px) {
            double z = cur_zoom(a);
            if (a->fit) {                               /* dragging starts from the centred position */
                a->fit = false;
                a->zoom = z;
                double sw = a->img.w * z, sh = a->img.h * z;
                a->ox = sw <= view_w(a) ? (view_w(a) - sw) / 2 : 0;
                a->oy = sh <= view_h(a) ? (view_h(a) - sh) / 2 : 0;
            }
            a->ox += x - a->drag_x;
            a->oy += y - a->drag_y;
            a->drag_x = x;
            a->drag_y = y;
            w->redraw = true;
        }
        a->px = x;
        a->py = y;
        break;
    }
    case LM_WL_POINTER_EVT_BUTTON:
        if (args[2].u != 272)                           /* BTN_LEFT */
            break;
        if (args[3].u == 1) {
            uint32_t t = args[1].u;
            if (t - a->last_click_ms < 400 && abs((int)(a->px - a->last_click_x)) < 6 && abs((int)(a->py - a->last_click_y)) < 6) {
                if (a->fit)
                    zoom_to(a, 1.0, a->px, a->py);
                else {
                    a->fit = true;
                    a->win.redraw = true;
                }
                a->last_click_ms = 0;
            } else {
                a->last_click_ms = t;
                a->last_click_x = a->px;
                a->last_click_y = a->py;
            }
            a->dragging = true;
            a->drag_x = a->px;
            a->drag_y = a->py;
        } else {
            a->dragging = false;
        }
        break;
    case LM_WL_POINTER_EVT_AXIS:
        if (args[1].u == 0) {                           /* vertical: up (negative) zooms in */
            double v = lm_fixed_to_double(args[2].f);
            if (v != 0)
                zoom_to(a, cur_zoom(a) * (v < 0 ? 1.25 : 1.0 / 1.25), a->px, a->py);
        }
        break;
    }
}

static void keyboard_event(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
    struct app *a = w->data;
    if (op != LM_WL_KEYBOARD_EVT_KEY || args[3].u == 0)
        return;
    uint32_t ks = lm_keymap_keysym(args[2].u, a->win.mods);
    char txt[8];
    int tl = lm_keysym_to_utf8(ks, txt);
    char c = (tl == 1 && !(a->win.mods & (LM_MOD_CTRL | LM_MOD_ALT | LM_MOD_SUPER))) ? txt[0] : 0;
    double cx = view_w(a) / 2.0, cy = view_h(a) / 2.0;
    if (a->has_pointer) {
        cx = a->px;
        cy = a->py;
    }
    if (ks == LM_KS_Escape || c == 'q' || c == 'Q') {
        w->running = false;
    } else if (ks == LM_KS_Right || ks == LM_KS_Page_Down || c == ' ') {
        go(a, a->cur + 1);
    } else if (ks == LM_KS_Left || ks == LM_KS_Page_Up || ks == LM_KS_BackSpace) {
        go(a, a->cur - 1);
    } else if (ks == LM_KS_Home) {
        go(a, 0);
    } else if (ks == LM_KS_End) {
        go(a, a->n - 1);
    } else if (c == '+' || c == '=') {
        zoom_to(a, cur_zoom(a) * 1.25, cx, cy);
    } else if (c == '-' || c == '_') {
        zoom_to(a, cur_zoom(a) / 1.25, cx, cy);
    } else if (c == '0' || c == '1') {
        zoom_to(a, 1.0, cx, cy);
    } else if (c == 'f' || c == 'F') {
        a->fit = true;
        w->redraw = true;
    } else if (c == 'r' || c == 'R') {
        scan_dir(a, NULL);                              /* and on to the newest one */
        load_current(a);
    }
}

/* ---- main ----------------------------------------------------------------------------------------------------------- */

static int selftest(void)
{
    int bad = 0;
    char err[160];
    /* our own encoder -> decoder (stored deflate blocks), 70x45 so that the data spans a block boundary */
    enum { W = 70, H = 45 };
    uint32_t px[W * H];
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            px[y * W + x] = (uint32_t)(x * 3) << 16 | (uint32_t)(y * 5) << 8 | (uint32_t)((x + y) & 255);
    size_t n = 0;
    uint8_t *png = lm_png_encode_xrgb(px, W, H, W * 4, &n);
    struct lm_image img = { 0 };
    if (!png || lm_img_decode(png, n, &img, err, sizeof(err)) < 0 || img.w != W || img.h != H || memcmp(img.px, px, sizeof(px))) {
        fprintf(stderr, "selftest: round trip failed (%s)\n", err);
        bad++;
    }
    lm_img_free(&img);
    free(png);
    /* (compressed streams: tests/test_mellagui.py feeds it real zlib output of every colour type) */
    if (lm_img_decode((const uint8_t *)"junk", 4, &img, err, sizeof(err)) == 0) {
        fprintf(stderr, "selftest: junk accepted\n");
        bad++;
    }
    puts(bad ? "selftest FAILED" : "selftest ok");
    return bad ? 1 : 0;
}

static int decode_cmd(const char *in, const char *out)
{
    struct lm_image img = { 0 };
    char err[160];
    if (lm_img_load(in, &img, err, sizeof(err)) < 0) {
        fprintf(stderr, "mellagui-view: %s\n", err);
        return 1;
    }
    FILE *f = fopen(out, "wb");
    if (!f)
        return 1;
    fprintf(f, "P6\n%d %d\n255\n", img.w, img.h);
    for (size_t i = 0; i < (size_t)img.w * (size_t)img.h; i++) {
        uint8_t rgb[3] = { (uint8_t)(img.px[i] >> 16), (uint8_t)(img.px[i] >> 8), (uint8_t)img.px[i] };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    lm_img_free(&img);
    return 0;
}

int main(int argc, char **argv)
{
    lm_theme_load_env();
    if (argc > 1 && !strcmp(argv[1], "--selftest"))
        return selftest();
    if (argc > 3 && !strcmp(argv[1], "--decode"))
        return decode_cmd(argv[2], argv[3]);

    struct app a = { 0 };
    const char *arg = argc > 1 ? argv[1] : NULL, *keep = NULL;
    char tmp[PATH_MAX], base[256] = "";
    if (!arg) {
        const char *home = getenv("HOME");
        snprintf(tmp, sizeof(tmp), "%s/Pictures", home && *home ? home : ".");
        struct stat st;
        arg = stat(tmp, &st) == 0 && S_ISDIR(st.st_mode) ? tmp : ".";
    }
    struct stat st;
    if (stat(arg, &st) == 0 && !S_ISDIR(st.st_mode)) {
        const char *slash = strrchr(arg, '/');
        snprintf(a.dir, sizeof(a.dir), "%.*s", slash ? (int)(slash - arg) : 1, slash ? arg : ".");
        if (slash && slash == arg)
            snprintf(a.dir, sizeof(a.dir), "/");
        snprintf(base, sizeof(base), "%.255s", slash ? slash + 1 : arg);
        keep = base;
    } else {
        snprintf(a.dir, sizeof(a.dir), "%s", arg);
    }
    scan_dir(&a, keep);

    static const struct wlwin_ops ops = { .paint = paint, .pointer = pointer_event, .keyboard = keyboard_event };
    if (wlwin_connect(&a.win, "mellagui-view", &ops, &a) < 0)
        return 1;
    if (wlwin_open(&a.win, "Images", "mellagui.view", 860, 520, 240, 160) < 0)
        return 1;
    load_current(&a);
    while (wlwin_step(&a.win, -1))
        ;
    wlwin_destroy(&a.win);
    lm_img_free(&a.img);
    free(a.scaled);
    free_names(&a);
    return 0;
}
