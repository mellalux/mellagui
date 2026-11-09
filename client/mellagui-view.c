/*
 * mellagui-view - image viewer (PNG, JPEG, PPM) and player of Motion JPEG videos (.mp4) for MellaGUI, mostly for the screenshots (Print)
 * in ~/Pictures and the recordings of mellagui-record.
 *
 *   mellagui-view [FILE | DIR]     FILE: the images of its directory, starting at FILE; DIR (default ~/Pictures): the
 *                                  newest image (the last name: screenshots are named by date)
 *   mellagui-view --decode FILE OUT.ppm   decode FILE and write it as a PPM (tests)
 *   mellagui-view --selftest
 *
 * Videos (.mp4 / .m4v with Motion JPEG pictures: what mellagui-record writes, or `ffmpeg -c:v mjpeg`; no sound; H.264 and the rest
 * are recognised but cannot be shown) play at once: Space plays / pauses, Left / Right go 5 s back / on, `,` and `.` step a picture,
 * Home starts again, a click or drag on the bottom bar seeks, PgUp / PgDn / Backspace / N / P choose the previous / next file; the picture
 * zooms and pans like an image. `--video-info FILE` and `--decode FILE.mp4 OUT.ppm [N]` (picture N) are for tests.
 *
 * Keys (images): Left/Right (PgUp/PgDn, Backspace/Space) previous / next, Home/End first / last, + - zoom, 0 or 1 = 100 %,
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
#include "ffdec.h"
#include "ffpipe.h"
#include "h264dec.h"
#include "img.h"
#include "keymap.h"
#include "loop.h"
#include "mp4read.h"
#include "png.h"
#include "protocol.h"
#include "util.h"
#include "wlwin.h"

#define BAR_H 24
#define BG (lm_theme.card)
#define BAR_BG 0x0e1118u
#define BAR_FG 0xd8dde8u
#define FF_FPS 30                       /* ffmpeg plays at a constant 30 pictures a second */
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
    struct lm_mp4r *vid;                /* the video being played with our own reader (Motion JPEG) ... */
    struct ffdec *fd;                   /* ... or through ffmpeg (anything else, when it is installed) */
    int vmode;                          /* 0 an image, 1 Motion JPEG (vid), 2 ffmpeg (fd), 3 our own H.264 decoder (vid + hd) */
    struct lm_h264dec *hd;              /* 3: the decoder ... */
    uint32_t hdone;                     /* ... and the picture it has decoded last (~0u: none, it has to start at an IDR picture) */
    int fw, fh;                         /* ffmpeg: the size of its pictures */
    uint64_t fdur;                      /* ffmpeg: duration of the video, 0 = unknown */
    uint64_t fbase;                     /* ffmpeg: the position its first picture belongs to */
    uint64_t fidx;                      /* ffmpeg: pictures taken from it so far */
    uint32_t *fbuf;                     /* ffmpeg: the picture being collected */
    size_t fhave;
    bool fended;                        /* ffmpeg has sent its last picture */
    bool fready, fwant;                 /* a whole picture waits in fbuf; show the next one even when paused (after a seek) */
    uint32_t vframe;                    /* the picture shown */
    bool playing, seeking;
    uint64_t pos_ms;                    /* where the video is (paused), or where it was when playing (re)started */
    uint64_t play_t0;                   /* lm_now_ms() when it (re)started */
};

/* ---- directory ---------------------------------------------------------------------------------------------------- */

static bool is_video_name(const char *n)
{
    size_t l = strlen(n);
    return l > 4 && (!strcasecmp(n + l - 4, ".mp4") || !strcasecmp(n + l - 4, ".m4v"));
}

static bool is_image_name(const char *n)
{
    size_t l = strlen(n);
    return n[0] != '.' && l > 4 && (!strcasecmp(n + l - 4, ".png") || !strcasecmp(n + l - 4, ".ppm") || !strcasecmp(n + l - 4, ".jpg") ||
                                    (l > 5 && !strcasecmp(n + l - 5, ".jpeg")) || is_video_name(n));
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

/* ---- video ------------------------------------------------------------------------------------------------------------- */

static uint64_t video_len(const struct app *a)
{
    return a->vmode == 1 || a->vmode == 3 ? a->vid->duration_ms : a->vmode == 2 ? a->fdur : 0;
}

static uint64_t video_pos(const struct app *a)
{
    if (!a->vmode)
        return 0;
    uint64_t p = a->playing ? a->pos_ms + (lm_now_ms() - a->play_t0) : a->pos_ms;
    uint64_t len = video_len(a);
    return len && p > len ? len : p;
}

/* Own H.264 decoder: picture f, decoded from the IDR picture before it (or on from the one before, which is what playing does). */
static void show_h264(struct app *a, uint32_t f)
{
    char err[160] = "";
    uint32_t from = f;
    if (a->hdone != ~0u && f > a->hdone && f - a->hdone <= 120)
        from = a->hdone + 1;
    else
        while (from > 0 && !a->vid->key[from])
            from--;
    for (uint32_t k = from; k <= f; k++) {
        size_t len = 0;
        uint8_t *au = lm_mp4r_sample(a->vid, k, &len);
        int rc = au ? lm_h264dec_decode(a->hd, au, len, err, sizeof(err)) : -1;
        free(au);
        if (rc < 0) {
            a->hdone = ~0u;
            snprintf(a->err, sizeof(a->err), "picture %u: %s", (unsigned)k + 1, err[0] ? err : "cannot read it");
            a->playing = false;
            a->win.redraw = true;
            return;
        }
    }
    a->hdone = f;
    lm_h264dec_xrgb(a->hd, a->img.px, (size_t)a->img.w);
    free(a->scaled);
    a->scaled = NULL;
    a->vframe = f;
    a->err[0] = 0;
    a->win.redraw = true;
}

static void show_frame(struct app *a, uint32_t f)
{
    if (a->vmode == 3) {
        show_h264(a, f);
        return;
    }
    size_t len = 0;
    uint8_t *jpg = lm_mp4r_sample(a->vid, f, &len);
    struct lm_image img = { 0 };
    char err[160] = "cannot read the picture";
    if (jpg && lm_jpeg_decode(jpg, len, &img, err, sizeof(err)) == 0) {
        lm_img_free(&a->img);
        a->img = img;
        free(a->scaled);
        a->scaled = NULL;
        a->vframe = f;
        a->err[0] = 0;
    } else {
        snprintf(a->err, sizeof(a->err), "picture %u: %s", (unsigned)f + 1, err);
        a->playing = false;
    }
    free(jpg);
    a->win.redraw = true;
}

/* ffmpeg: (re)start decoding at `ms`; the first picture shows even while paused. */
static bool ff_start(struct app *a, uint64_t ms, const char *path)
{
    ffdec_close(a->fd);
    a->fd = ffdec_open(path, a->fw, a->fh, FF_FPS, ms, a->err, sizeof(a->err));
    a->fbase = ms;
    a->fidx = 0;
    a->fhave = 0;
    a->fready = false;
    a->fwant = true;
    a->fended = false;
    return a->fd != NULL;
}

static char g_ff_path[PATH_MAX + 300];                 /* the file ffmpeg plays (for seeks) */

/* Takes pictures from ffmpeg: the one that is due now is shown, late ones are dropped. */
static void ff_tick(struct app *a)
{
    if (!a->fd || (!a->playing && !a->fwant))
        return;
    uint64_t pos = video_pos(a);
    for (int guard = 0; guard < 4; guard++) {
        if (!a->fready) {
            int r = ffdec_read(a->fd, a->fbuf, &a->fhave);
            if (r < 0) {                                    /* the end of the video */
                a->playing = false;
                a->fwant = false;
                a->fended = true;
                if (a->fdur)
                    a->pos_ms = LM_MAX(a->pos_ms, a->fbase + a->fidx * 1000 / FF_FPS);
                else
                    a->pos_ms = a->fbase + a->fidx * 1000 / FF_FPS;
                a->win.redraw = true;
                return;
            }
            if (r == 0)
                return;
            a->fready = true;
        }
        uint64_t due = a->fbase + a->fidx * 1000 / FF_FPS;
        if (!a->fwant && pos < due)
            return;                                         /* not yet */
        memcpy(a->img.px, a->fbuf, (size_t)a->fw * (size_t)a->fh * 4);
        free(a->scaled);
        a->scaled = NULL;
        a->fidx++;
        a->fready = false;
        a->win.redraw = true;
        if (a->fwant) {                                     /* the first one after a start or a seek: the clock begins now, not when ffmpeg was asked */
            a->fwant = false;
            a->pos_ms = due;
            a->play_t0 = lm_now_ms();
            return;
        }
        if (pos < a->fbase + a->fidx * 1000 / FF_FPS)       /* the next one is not due: this one stays */
            return;
    }
}

static void video_seek(struct app *a, int64_t ms)
{
    if (!a->vmode)
        return;
    uint64_t len = video_len(a);
    if (ms < 0)
        ms = 0;
    if (len && (uint64_t)ms > len)
        ms = (int64_t)len;
    a->pos_ms = (uint64_t)ms;
    a->play_t0 = lm_now_ms();
    if (a->vmode == 2) {
        if (!ff_start(a, a->pos_ms, g_ff_path))
            a->playing = false;
        a->win.redraw = true;
        return;
    }
    uint32_t f = lm_mp4r_frame_at(a->vid, a->pos_ms);
    if (f != a->vframe || !a->img.px)
        show_frame(a, f);
    a->win.redraw = true;
}

static void video_toggle(struct app *a)
{
    if (!a->vmode)
        return;
    if (a->playing) {
        a->pos_ms = video_pos(a);
        a->playing = false;
    } else {
        uint64_t len = video_len(a);
        if (len && a->pos_ms >= len)                    /* at the end: again from the start */
            video_seek(a, 0);
        else if (a->vmode == 2 && a->fended)            /* ffmpeg had ended (a video of unknown length): again from the start */
            video_seek(a, 0);
        a->play_t0 = lm_now_ms();
        a->playing = true;
    }
    a->win.redraw = true;
}

/* Called after every loop turn: shows the picture that belongs to now; ends the playback at the end. */
static void video_tick(struct app *a)
{
    if (a->vmode == 2) {
        ff_tick(a);
        if (a->playing && video_len(a) && video_pos(a) >= video_len(a)) {
            a->pos_ms = video_len(a);
            a->playing = false;
            a->win.redraw = true;
        }
        return;
    }
    if (!a->vid || !a->playing)
        return;
    uint64_t pos = video_pos(a);
    uint32_t f = lm_mp4r_frame_at(a->vid, pos);
    if (f != a->vframe)
        show_frame(a, f);
    if (pos >= a->vid->duration_ms) {
        a->pos_ms = a->vid->duration_ms;
        a->playing = false;
        a->win.redraw = true;
    }
}

/* How long the loop may sleep: until the next picture (at most 250 ms: the clock in the bar). */
static int video_timeout(const struct app *a)
{
    if (a->vmode == 2) {
        if (!a->fd || (!a->playing && !a->fwant))
            return -1;
        if (!a->fready)
            return 4;                                       /* waiting for ffmpeg's data */
        uint64_t pos = video_pos(a), due = a->fbase + a->fidx * 1000 / FF_FPS;
        return due > pos ? (int)LM_MIN(due - pos, 50) : 1;
    }
    if (!a->vid || !a->playing)
        return -1;
    uint64_t pos = video_pos(a);
    int t = 250;
    if (a->vframe + 1 < a->vid->n) {
        uint64_t next = a->vid->pts_ms[a->vframe + 1];
        t = next > pos ? (int)LM_MIN(next - pos, 250) : 1;
    }
    return t;
}

static void fmt_time(char *out, size_t n, uint64_t ms)
{
    unsigned s = (unsigned)(ms / 1000);
    if (s >= 3600)
        snprintf(out, n, "%u:%02u:%02u", s / 3600, s / 60 % 60, s % 60);
    else
        snprintf(out, n, "%02u:%02u", s / 60, s % 60);
}

static void load_current(struct app *a)
{
    lm_img_free(&a->img);
    free(a->scaled);
    a->scaled = NULL;
    a->err[0] = 0;
    lm_mp4r_close(a->vid);
    a->vid = NULL;
    lm_h264dec_free(a->hd);
    a->hd = NULL;
    a->hdone = ~0u;
    ffdec_close(a->fd);
    a->fd = NULL;
    free(a->fbuf);
    a->fbuf = NULL;
    a->vmode = 0;
    a->playing = false;
    a->vframe = 0;
    a->pos_ms = 0;
    if (a->n) {
        char path[PATH_MAX + 300];
        snprintf(path, sizeof(path), "%s/%s", a->dir, a->names[a->cur]);
        if (is_video_name(a->names[a->cur])) {
            char why[160] = "";
            a->vid = lm_mp4r_open(path, why, sizeof(why));          /* our own reader first: Motion JPEG needs nothing else */
            if (a->vid && a->vid->h264) {                           /* H.264 of our own making plays without ffmpeg */
                char herr[160];
                a->hd = lm_h264dec_create(a->vid->sps, a->vid->sps_n, a->vid->pps, a->vid->pps_n, herr, sizeof(herr));
                if (a->hd) {
                    a->img.w = lm_h264dec_width(a->hd);
                    a->img.h = lm_h264dec_height(a->hd);
                    a->img.px = calloc((size_t)a->img.w * (size_t)a->img.h, 4);
                    if (!a->img.px) {
                        lm_h264dec_free(a->hd);
                        a->hd = NULL;
                    }
                } else {
                    snprintf(why, sizeof(why), "%s", herr);
                }
                if (!a->hd) {
                    lm_mp4r_close(a->vid);
                    a->vid = NULL;
                }
            } else if (a->vid && !a->vid->mjpeg) {
                snprintf(why, sizeof(why), "This video is %s (%dx%d)", a->vid->codec, a->vid->w, a->vid->h);
                lm_mp4r_close(a->vid);
                a->vid = NULL;
            }
            if (a->vid) {
                a->vmode = a->hd ? 3 : 1;
                show_frame(a, 0);
                a->play_t0 = lm_now_ms();
                a->playing = a->img.px != NULL;
            } else {                                                /* anything else: ffmpeg, when it is installed */
                struct ffdec_info info;
                char ferr[160];
                if (ffpipe_ffmpeg_path() && ffdec_probe(path, &info, ferr, sizeof(ferr))) {
                    a->fw = info.w;
                    a->fh = info.h;
                    a->fdur = info.duration_ms;
                    a->fbuf = malloc((size_t)a->fw * (size_t)a->fh * 4);
                    a->img.w = a->fw;
                    a->img.h = a->fh;
                    a->img.px = calloc((size_t)a->fw * (size_t)a->fh, 4);
                    snprintf(g_ff_path, sizeof(g_ff_path), "%s", path);
                    if (a->fbuf && a->img.px && ff_start(a, 0, path)) {
                        a->vmode = 2;
                        a->play_t0 = lm_now_ms();
                        a->playing = true;
                    } else {
                        lm_img_free(&a->img);
                        free(a->fbuf);
                        a->fbuf = NULL;
                        snprintf(why, sizeof(why), "%s", a->err[0] ? a->err : "cannot play it");
                    }
                } else if (!ffpipe_ffmpeg_path()) {
                    snprintf(a->err, sizeof(a->err), "%s%sSound-free Motion JPEG plays by itself; for this one install ffmpeg", why, why[0] ? ": " : "");
                    why[0] = 0;
                } else {
                    snprintf(why, sizeof(why), "%s", ferr);
                }
                if (!a->vmode && !a->err[0])
                    snprintf(a->err, sizeof(a->err), "%s", why);
            }
        } else if (lm_img_load(path, &a->img, a->err, sizeof(a->err)) < 0) {
            a->img.px = NULL;
        }
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
        if (a->vmode) {
            char t1[16], t2[16];
            fmt_time(t1, sizeof(t1), video_pos(a));
            fmt_time(t2, sizeof(t2), video_len(a));
            snprintf(status, sizeof(status), "%s   %dx%d   %s / %s   %s   %d%%   %d/%d", a->names[a->cur], a->img.w, a->img.h, t1, t2,
                     a->playing ? "playing" : "paused", (int)(z * 100 + 0.5), a->cur + 1, a->n);
        } else {
            snprintf(status, sizeof(status), "%s   %dx%d   %d%%%s   %d/%d", a->names[a->cur], a->img.w, a->img.h,
                     (int)(z * 100 + 0.5), a->fit ? " (fit)" : "", a->cur + 1, a->n);
        }
    } else if (a->n) {
        snprintf(status, sizeof(status), "%s: %s   %d/%d", a->names[a->cur], a->err, a->cur + 1, a->n);
        wlwin_text(w, 16, 16, a->err, 1, 0xff8080u, vw - 32);
    } else {
        snprintf(status, sizeof(status), "no images in %.300s", a->dir);
        wlwin_text(w, 16, 16, "No images or videos here. Press Print to take a screenshot, R to look again.", 1, BAR_FG, vw - 32);
    }
    wlwin_fill(w, 0, vh, w->bw, w->bh - vh, BAR_BG);
    if (a->vmode && video_len(a)) {                              /* the progress: a strip along the top of the bar */
        wlwin_fill(w, 0, vh, w->bw, 4, 0x2a3040u);
        wlwin_fill(w, 0, vh, (int)((uint64_t)w->bw * video_pos(a) / video_len(a)), 4, lm_theme.accent);
    }
    wlwin_text(w, 8, vh + (BAR_H - LM_FONT_H) / 2 + 2, status, 1, BAR_FG, w->bw - 16);
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
        a->seeking = false;
        a->dragging = false;
        break;
    case LM_WL_POINTER_EVT_MOTION: {
        double x = lm_fixed_to_double(args[1].f), y = lm_fixed_to_double(args[2].f);
        if (a->seeking && a->vmode)
            video_seek(a, (int64_t)(LM_CLAMP(x / (double)w->bw, 0.0, 1.0) * (double)video_len(a)));
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
        if (args[3].u == 1 && a->vmode && a->py >= view_h(a)) {  /* a click on the bar of a video seeks */
            a->seeking = true;
            video_seek(a, (int64_t)(LM_CLAMP(a->px / (double)w->bw, 0.0, 1.0) * (double)video_len(a)));
            break;
        }
        if (args[3].u != 1)
            a->seeking = false;
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
    } else if (a->vmode && c == ' ') {
        video_toggle(a);
    } else if (a->vmode && ks == LM_KS_Right) {
        video_seek(a, (int64_t)video_pos(a) + 5000);
    } else if (a->vmode && ks == LM_KS_Left) {
        video_seek(a, (int64_t)video_pos(a) - 5000);
    } else if ((a->vmode == 1 || a->vmode == 3) && (c == ',' || c == '.')) {          /* one picture back / on, paused */
        uint32_t f = lm_mp4r_frame_at(a->vid, video_pos(a));
        if (a->playing)
            video_toggle(a);
        f = c == '.' ? LM_MIN(f + 1, a->vid->n - 1) : (f ? f - 1 : 0);
        video_seek(a, (int64_t)a->vid->pts_ms[f]);
    } else if (a->vmode == 2 && (c == ',' || c == '.')) {
        int64_t at = (int64_t)video_pos(a) + (c == '.' ? 1000 / FF_FPS : -1000 / FF_FPS);
        if (a->playing)
            video_toggle(a);
        video_seek(a, at);
    } else if (a->vmode && ks == LM_KS_Home) {
        video_seek(a, 0);
    } else if (a->vmode && ks == LM_KS_End) {
        video_seek(a, (int64_t)(video_len(a) ? video_len(a) : 0));
    } else if (ks == LM_KS_Right || ks == LM_KS_Page_Down || c == ' ' || c == 'n' || c == 'N') {
        go(a, a->cur + 1);
    } else if (ks == LM_KS_Left || ks == LM_KS_Page_Up || ks == LM_KS_BackSpace || c == 'p' || c == 'P') {
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

/* `--video-info FILE`: "WxH codec=.. mjpeg=0|1 pictures=N duration_ms=N" */
static int video_info_cmd(const char *in)
{
    char err[160];
    struct lm_mp4r *r = lm_mp4r_open(in, err, sizeof(err));
    if (!r) {
        fprintf(stderr, "mellagui-view: %s\n", err);
        return 1;
    }
    printf("%dx%d codec=%s mjpeg=%d pictures=%u duration_ms=%llu\n", r->w, r->h, r->codec, r->mjpeg, (unsigned)r->n,
           (unsigned long long)r->duration_ms);
    lm_mp4r_close(r);
    return 0;
}

static int decode_cmd(const char *in, const char *out, int frame)
{
    struct lm_image img = { 0 };
    char err[160];
    if (is_video_name(in)) {
        struct lm_mp4r *r = lm_mp4r_open(in, err, sizeof(err));
        int rc = -1;
        if (r && r->h264 && frame >= 0 && (uint32_t)frame < r->n) {          /* our own H.264: from the IDR picture before it */
            struct lm_h264dec *hd = lm_h264dec_create(r->sps, r->sps_n, r->pps, r->pps_n, err, sizeof(err));
            uint32_t from = (uint32_t)frame;
            while (from > 0 && !r->key[from])
                from--;
            rc = hd ? 0 : -1;
            for (uint32_t k = from; hd && rc == 0 && k <= (uint32_t)frame; k++) {
                size_t len = 0;
                uint8_t *au = lm_mp4r_sample(r, k, &len);
                rc = au ? lm_h264dec_decode(hd, au, len, err, sizeof(err)) : (snprintf(err, sizeof(err), "cannot read the picture"), -1);
                free(au);
            }
            if (rc == 0) {
                img.w = lm_h264dec_width(hd);
                img.h = lm_h264dec_height(hd);
                img.px = malloc((size_t)img.w * (size_t)img.h * 4);
                if (img.px)
                    lm_h264dec_xrgb(hd, img.px, (size_t)img.w);
                else
                    rc = (snprintf(err, sizeof(err), "out of memory"), -1);
            }
            lm_h264dec_free(hd);
        } else {
            size_t len = 0;
            uint8_t *jpg = r && r->mjpeg && frame >= 0 && (uint32_t)frame < r->n ? lm_mp4r_sample(r, (uint32_t)frame, &len) : NULL;
            rc = !r ? -1 : !jpg ? (snprintf(err, sizeof(err), "no such picture, or not Motion JPEG or H.264"), -1) : lm_jpeg_decode(jpg, len, &img, err, sizeof(err));
            free(jpg);
        }
        lm_mp4r_close(r);
        if (rc < 0) {
            fprintf(stderr, "mellagui-view: %s\n", err);
            return 1;
        }
    } else if (lm_img_load(in, &img, err, sizeof(err)) < 0) {
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
        return decode_cmd(argv[2], argv[3], argc > 4 ? atoi(argv[4]) : 0);
    if (argc > 2 && !strcmp(argv[1], "--video-info"))
        return video_info_cmd(argv[2]);

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
    while (wlwin_step(&a.win, video_timeout(&a)))
        video_tick(&a);
    wlwin_destroy(&a.win);
    lm_mp4r_close(a.vid);
    lm_h264dec_free(a.hd);
    ffdec_close(a.fd);
    free(a.fbuf);
    lm_img_free(&a.img);
    free(a.scaled);
    free_names(&a);
    return 0;
}
