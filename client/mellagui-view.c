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
 * Home starts again, the buttons in the bottom bar (back 5 s, play / pause, stop = back to the first picture, on 5 s) do the same, a click or drag on the bar seeks, PgUp / PgDn / Backspace / N / P choose the previous / next file; the picture
 * zooms and pans like an image. `--video-info FILE` and `--decode FILE.mp4 OUT.ppm [N]` (picture N) are for tests; `--poster FILE OUT.ppm` writes one picture
 * of a video (and its info line to stdout) for the file manager's preview.
 *
 * Keys (images): Left/Right (PgUp/PgDn, Backspace/Space) previous / next, Home/End first / last, + - zoom, 0 or 1 = 100 %,
 * F = fit, I = the picture's EXIF data (camera, exposure, time, place) over the corner, R = rescan the directory, Esc or Q = quit. Mouse: wheel zooms around the pointer, drag pans, double click
 * toggles fit / 100 %.
 *
 * Under the picture is a STRIP of small pictures of every file of the directory the viewer can show (pictures, videos, sound files; the ones that are not
 * pictures have a tile with a mark): a click shows one, the wheel over the strip scrolls it, the current one is kept in view; T hides / shows it (kept in
 * viewer.conf). The "Open..." button at its left end (or O / Ctrl+O) opens a card with the folder list of the shared Open dialog (client/wlpicker.h): a
 * folder chosen there becomes the viewer's directory, a file is shown.
 *
 * ONE window per Wayland display: a second `mellagui-view PATH` (the camera's strip, the file manager's Enter, Print's card ...) hands PATH to the
 * first through an abstract unix socket (named after the runtime directory and the display) and ends; the first shows it and brings its window forward
 * (wlwin_raise). $MELLAGUI_VIEW_SINGLE=0 makes every start a window of its own. $MELLAGUI_VIEW_DUMP=FILE writes the state, the strip's places and the
 * card after every paint (tests).
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "theme.h"
#include "font.h"
#include "ffaudio.h"
#include "ffdec.h"
#include "ffpipe.h"
#include "h264dec.h"
#include "img.h"
#include "../src/exif.h"
#include "../src/svg.h"
#include "keymap.h"
#include "loop.h"
#include "metrics.h"
#include "mp4read.h"
#include "png.h"
#include "protocol.h"
#include "util.h"
#include "uikit.h"
#include "thumbbox.h"
#include "wlcard.h"
#include "wlclip.h"
#include "wlpicker.h"
#include "wlwin.h"
#include "mediakeys.h"

#define BAR_H 24
#define BG (lm_theme.card)
#define BAR_BG (lm_theme.bar_deep)
#define BAR_FG (lm_theme.bar_text)
#define VBTN_W 26                       /* the video buttons in the bar: back 5 s, play / pause, stop, on 5 s */
#define VBTN_H 18
#define VBTN_GAP 2
#define N_VBTN 4
#define AUDIO_LEAD_MS 190               /* the sound needs this long from the start of its process to the speaker (ffmpeg ~90 ms, the player ~100 ms): it starts this far ahead */
#define FF_FPS 30                       /* ffmpeg plays at a constant 30 pictures a second */
#define STRIP_H 80                      /* the strip of small pictures under the picture */
#define TS_W 64                         /* a small picture's room: 64 x 48 */
#define TS_H 48
#define TS_PITCH 72
#define TS_X0 88                        /* where the pictures start: the Open button is before them */
#define TH_KEEP_W 128                   /* a small picture is kept at up to 128 x 96 and squeezed to the buffer's pixels when it is painted */
#define TH_KEEP_H 96
#define ZMIN 0.05
#define ZMAX 32.0

struct vthumb {                         /* the small copy of one file of the directory */
    uint32_t *px;                       /* NULL: not made yet, or the file is no picture (a video, a sound file) or cannot be read */
    int w, h;
    bool tried;
};

struct app {
    struct wlwin win;
    char exif[8][200];                  /* the lines of the picture's EXIF data */
    int nexif;
    bool show_info;                     /* I: show them */
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
    int vmode;                          /* 0 an image, 1 Motion JPEG (vid), 2 ffmpeg (fd), 3 our own H.264 decoder (vid + hd), 4 a sound file (ainfo, fdur) */
    struct ffaudio_info ainfo;          /* 4: what the file says about itself */
    struct lm_image cover;              /* 4: the picture stored in it (px NULL = none) */
    uint32_t idle_mgr, inhibitor;       /* zwp_idle_inhibit_manager_v1 and the inhibitor that exists while a video plays */
    struct lm_h264dec *hd;              /* 3: the decoder ... */
    uint32_t hdone;                     /* ... and the picture it has decoded last (~0u: none, it has to start at an IDR picture) */
    int fw, fh;                         /* ffmpeg: the size of its pictures (smaller than the video while it is shown "fit": ffmpeg scales) */
    int tw, th;                         /* ffmpeg: the size of the video itself */
    int64_t ff_started;                 /* ffmpeg: when it was (re)started (a resize of the window restarts it, at most a few times a second) */
    bool has_audio, mute, aud_ended, aud_dead;   /* the file has sound / the user switched it off / its sound is over (until a seek) / no player works */
    struct ffaudio *aud;
    int aud_player;                     /* the player in use (an index of ffaudio_player_name) */
    int aud_lead;                       /* ms: how far ahead of the picture the sound starts ([ and ] change it, viewer.conf keeps it) */
    bool aud_lead_set;
    uint64_t fdur;                      /* ffmpeg: duration of the video, 0 = unknown */
    uint64_t fbase;                     /* ffmpeg: the position its first picture belongs to */
    uint64_t fidx;                      /* ffmpeg: pictures taken from it so far */
    uint32_t *fbuf;                     /* ffmpeg: the picture being collected */
    size_t fhave;
    bool fended;                        /* ffmpeg has sent its last picture */
    bool fready, fwant;                 /* a whole picture waits in fbuf; show the next one even when paused (after a seek) */
    uint32_t vframe;                    /* the picture shown */
    bool playing, seeking;
    bool next_track;                    /* 4: the track is over: on to the next sound file */
    int vhover, vpress;                 /* the video button under the pointer / pressed: 0 none, else index + 1 */
    uint64_t pos_ms;                    /* where the video is (paused), or where it was when playing (re)started */
    uint64_t play_t0;                   /* lm_now_ms() when it (re)started */
    pid_t pj_pid;                       /* the child that makes the poster of a video of the strip (`mellagui-view --poster`), 0 = none */
    uint64_t pj_t0;
    char pj_name[PATH_MAX + 300];       /* the file name (as in names[]) it is for */
    char pj_out[PATH_MAX];
    struct vthumb *thm;                 /* the small pictures, one per name */
    int thn;
    bool thumbs_on;                     /* T: the strip shows */
    double strip_x;                     /* how far the strip is scrolled (pixels) */
    int th_hover;                       /* the picture under the pointer (-1 none) */
    bool open_hover;                    /* the pointer is on the Open button */
    int ipc;                            /* the socket the other starts hand their paths to (-1 none) */
    bool card;                          /* the Open card (folder list) shows */
    char pbuf[PATH_MAX];                /* its field */
    char perr[160];
    struct wlpicker pk;
    struct wlclip clip;
    int c_hover, c_down;                /* the card's buttons: under the pointer / pressed (-1 none) */
    char dump[PATH_MAX];                /* $MELLAGUI_VIEW_DUMP */
};

/* ---- directory ---------------------------------------------------------------------------------------------------- */

static bool is_video_name(const char *n)
{
    size_t l = strlen(n);
    return l > 4 && (!strcasecmp(n + l - 4, ".mp4") || !strcasecmp(n + l - 4, ".m4v"));
}

static bool is_audio_name(const char *n)
{
    return ffaudio_name_is_audio(n);
}

static bool is_image_name(const char *n)
{
    size_t l = strlen(n);
    static const char *const exts[] = { ".png", ".ppm", ".pgm", ".pbm", ".pnm", ".jpg", ".jpeg", ".gif", ".bmp", ".ico", ".cur", ".svg", ".tif", ".tiff", ".tga", ".webp" };
    if (n[0] == '.' || l < 5)
        return false;
    for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
        size_t el = strlen(exts[i]);
        if (l > el && !strcasecmp(n + l - el, exts[i]))
            return true;
    }
    return is_video_name(n) || is_audio_name(n);
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

static void th_reset(struct app *a)                             /* the names changed: the small pictures are made again, as they come into view */
{
    for (int i = 0; i < a->thn; i++)
        free(a->thm[i].px);
    free(a->thm);
    a->thm = a->n ? calloc((size_t)a->n, sizeof(*a->thm)) : NULL;
    a->thn = a->thm ? a->n : 0;
    a->strip_x = 0;
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
    th_reset(a);
}

/* ---- image, zoom ---------------------------------------------------------------------------------------------------- */

/* The window, top to bottom: the picture, the strip of small pictures (when it is on and the window has room), the status bar. */
static int strip_h(const struct app *a) { return a->thumbs_on && a->win.bw >= 260 && a->win.bh >= 300 ? STRIP_H : 0; }
static int bar_y(const struct app *a) { return a->win.bh > BAR_H ? a->win.bh - BAR_H : 0; }
static int view_w(const struct app *a) { return a->win.bw; }
static int view_h(const struct app *a) { int h = a->win.bh - BAR_H - strip_h(a); return h > 0 ? h : 1; }

/* ---- the strip of small pictures -------------------------------------------------------------------------------------------------- */

static int strip_y(const struct app *a) { return view_h(a); }
static int strip_vis_w(const struct app *a) { return a->win.bw - TS_X0 - 8; }

static double strip_max(const struct app *a)
{
    double m = (double)a->n * TS_PITCH - strip_vis_w(a);
    return m > 0 ? m : 0;
}

static void strip_clamp(struct app *a)
{
    a->strip_x = LM_CLAMP(a->strip_x, 0.0, strip_max(a));
}

/* The current picture is kept in view (centred when it was out of it). */
static void strip_show(struct app *a)
{
    if (!strip_h(a) || !a->n)
        return;
    double x = (double)a->cur * TS_PITCH, vis = strip_vis_w(a);
    if (x < a->strip_x || x + TS_PITCH > a->strip_x + vis)
        a->strip_x = x - (vis - TS_PITCH) / 2;
    strip_clamp(a);
}

static bool open_btn_rect(const struct app *a, int *x, int *y, int *w, int *h)
{
    if (!strip_h(a))
        return false;
    *x = 8;
    *w = 68;
    *h = 28;
    *y = strip_y(a) + (STRIP_H - *h) / 2;
    return true;
}

static bool open_btn_at(const struct app *a, double px, double py)
{
    int x, y, w, h;
    return open_btn_rect(a, &x, &y, &w, &h) && px >= x && px < x + w && py >= y && py < y + h;
}

static int item_x(const struct app *a, int i) { return TS_X0 + i * TS_PITCH - (int)a->strip_x; }

/* The picture of the strip under a point of the window, -1 none. */
static int strip_item_at(const struct app *a, double px, double py)
{
    if (!strip_h(a) || py < strip_y(a) || py >= strip_y(a) + STRIP_H || px < TS_X0 - 4)
        return -1;
    int i = (int)((px - TS_X0 + a->strip_x) / TS_PITCH);
    return i >= 0 && i < a->n && px - item_x(a, i) < TS_W + 4 ? i : -1;
}

/* The small copy of picture i from a decoded picture. */
static void th_from_image(struct app *a, int i, const struct lm_image *im)
{
    if (i < 0 || i >= a->thn || a->thm[i].px || !im->px || im->w < 1 || im->h < 1)
        return;
    double sc = LM_MIN((double)TH_KEEP_W / im->w, (double)TH_KEEP_H / im->h);
    if (sc > 1.0)
        sc = 1.0;
    int tw = LM_MAX((int)(im->w * sc), 1), th = LM_MAX((int)(im->h * sc), 1);
    uint32_t *px = malloc((size_t)tw * (size_t)th * 4);
    if (!px)
        return;
    lm_thumb_box(im->px, im->w, im->h, px, tw, th);
    a->thm[i].px = px;
    a->thm[i].w = tw;
    a->thm[i].h = th;
}

/* Makes the small copy of one file by reading it (a picture; a video or a sound file keeps a tile with a mark). */
static void th_make(struct app *a, int i)
{
    if (is_video_name(a->names[i]))                             /* (a video's poster is made by a child: poster_step) */
        return;
    a->thm[i].tried = true;
    if (is_audio_name(a->names[i]))
        return;
    char path[PATH_MAX + 300], err[120];
    struct lm_image im = { 0 };
    snprintf(path, sizeof(path), "%s/%s", a->dir, a->names[i]);
    if (lm_img_load(path, &im, err, sizeof(err)) == 0)
        th_from_image(a, i, &im);
    lm_img_free(&im);
}

/* One small picture of the ones in view per turn of the loop (a big JPEG takes a moment): true while there may be more. */
/* The poster of a video of the strip: a child process runs `mellagui-view --poster FILE OUT.ppm` (our own readers, else ffmpeg: it takes a moment, so
 * the loop never waits for it); one at a time, the current file first, then the ones in view. $MELLAGUI_VIEW_POSTER = another program, "none" = no
 * posters (the strip keeps the tile). True while a child is working. */
static bool poster_step(struct app *a)
{
    if (a->pj_pid) {
        int st;
        pid_t r = waitpid(a->pj_pid, &st, WNOHANG);
        bool done = r == a->pj_pid, dead = r < 0;
        if (!done && !dead && lm_now_ms() - a->pj_t0 > 12000) {
            kill(a->pj_pid, SIGKILL);
            waitpid(a->pj_pid, &st, 0);
            dead = true;
        }
        if (!done && !dead)
            return true;
        for (int i = 0; i < a->thn; i++)
            if (!strcmp(a->names[i], a->pj_name)) {                     /* (the list may have changed meanwhile: then the poster is not wanted) */
                struct lm_image im = { 0 };
                char err[120];
                if (done && WIFEXITED(st) && WEXITSTATUS(st) == 0 && lm_img_load(a->pj_out, &im, err, sizeof(err)) == 0)
                    th_from_image(a, i, &im);
                lm_img_free(&im);
                a->thm[i].tried = true;
                break;
            }
        unlink(a->pj_out);
        a->pj_pid = 0;
        a->win.redraw = true;
        return false;
    }
    if (!strip_h(a) || !a->thm)
        return false;
    const char *v = getenv("MELLAGUI_VIEW_POSTER");
    int first = LM_MAX((int)(a->strip_x / TS_PITCH), 0), last = LM_MIN((int)((a->strip_x + strip_vis_w(a)) / TS_PITCH) + 1, a->thn - 1);
    for (int pass = 0; pass < 2; pass++)
        for (int i = pass == 0 ? a->cur : first; i >= 0 && i <= (pass == 0 ? a->cur : last) && i < a->thn; i++) {
            if (a->thm[i].tried || a->thm[i].px || !is_video_name(a->names[i]))
                continue;
            if (v && !strcmp(v, "none")) {
                a->thm[i].tried = true;
                continue;
            }
            char in[PATH_MAX + 300];
            snprintf(in, sizeof(in), "%s/%s", a->dir, a->names[i]);
            snprintf(a->pj_name, sizeof(a->pj_name), "%s", a->names[i]);
            const char *rt = getenv("XDG_RUNTIME_DIR");
            snprintf(a->pj_out, sizeof(a->pj_out), "%s/mellagui-view-poster-%d.ppm", rt && *rt ? rt : "/tmp", (int)getpid());
            pid_t pid = fork();
            if (pid < 0) {
                a->thm[i].tried = true;
                continue;
            }
            if (pid == 0) {
                int nul = open("/dev/null", O_RDWR);
                if (nul >= 0) {
                    dup2(nul, 0);
                    dup2(nul, 1);
                    dup2(nul, 2);
                }
                char self[PATH_MAX];
                ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
                if (n > 0)
                    self[n] = 0;
                const char *prog = v && *v ? v : n > 0 ? self : "mellagui-view";
                execl(prog, prog, "--poster", in, a->pj_out, (char *)NULL);
                _exit(127);
            }
            a->pj_pid = pid;
            a->pj_t0 = lm_now_ms();
            return true;
        }
    return false;
}

static bool th_lazy(struct app *a)
{
    if (!strip_h(a) || !a->thm)
        return false;
    int first = LM_MAX((int)(a->strip_x / TS_PITCH), 0), last = LM_MIN((int)((a->strip_x + strip_vis_w(a)) / TS_PITCH) + 1, a->thn - 1);
    if (a->cur >= 0 && a->cur < a->thn && !a->thm[a->cur].tried && !is_video_name(a->names[a->cur])) {
        th_make(a, a->cur);
        a->win.redraw = true;
        return true;
    }
    for (int i = first; i <= last; i++)
        if (!a->thm[i].tried && !is_video_name(a->names[i])) {
            th_make(a, i);
            a->win.redraw = true;
            return true;
        }
    return false;
}

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
    return a->vmode == 1 || a->vmode == 3 ? a->vid->duration_ms : a->vmode == 2 || a->vmode == 4 ? a->fdur : 0;
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
    a->fd = ffdec_open(path, a->fw, a->fh, a->tw, a->th, FF_FPS, ms, a->err, sizeof(a->err));
    a->ff_started = lm_now_ms();
    a->fbase = ms;
    a->fidx = 0;
    a->fhave = 0;
    a->fready = false;
    a->fwant = true;
    a->fended = false;
    return a->fd != NULL;
}

static char g_ff_path[PATH_MAX + 300];                 /* the file that plays (for seeks and for the sound) */

/* The size ffmpeg should deliver: the video's own, or - while it is shown "fit" and larger than the view - just what the view shows. */
static void ff_target(const struct app *a, bool fit, int *w, int *h)
{
    *w = a->tw;
    *h = a->th;
    if (!fit)
        return;
    int k = wlwin_scale(&a->win);                               /* decode for the buffer's own pixels: sharp on a scaled screen */
    double z = LM_MIN((double)(view_w(a) * k) / a->tw, (double)(view_h(a) * k) / a->th);
    if (z < 1.0) {
        *w = LM_MAX((int)(a->tw * z + 0.5), 16);
        *h = LM_MAX((int)(a->th * z + 0.5), 16);
    }
}

/* Pictures of another size from now on (the old one is stretched over the new buffer so that nothing flashes), decoding goes on from where it is. */
static void ff_resize(struct app *a, int w, int h)
{
    uint32_t *nb = malloc((size_t)w * (size_t)h * 4), *np = malloc((size_t)w * (size_t)h * 4);
    if (!nb || !np) {
        free(nb);
        free(np);
        return;
    }
    if (a->img.px) {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                np[(size_t)y * (size_t)w + (size_t)x] = a->img.px[(size_t)(y * a->img.h / h) * (size_t)a->img.w + (size_t)(x * a->img.w / w)];
    } else {
        memset(np, 0, (size_t)w * (size_t)h * 4);
    }
    free(a->img.px);
    free(a->fbuf);
    a->img.px = np;
    a->fbuf = nb;
    a->img.w = a->fw = w;
    a->img.h = a->fh = h;
    free(a->scaled);
    a->scaled = NULL;
    uint64_t pos = video_pos(a);
    a->pos_ms = pos;
    a->play_t0 = lm_now_ms();
    if (!ff_start(a, pos, g_ff_path))
        a->playing = false;
    else if (a->playing)
        a->fwant = false;                                       /* the clock goes on (it is not set again by the first picture): pictures that are late are dropped */
    a->win.redraw = true;
}

/* Before anything that works with the picture's real size (zoom, pan): the full size picture. */
static void ff_ensure_full(struct app *a)
{
    if (a->vmode == 2 && a->tw && (a->fw != a->tw || a->fh != a->th))
        ff_resize(a, a->tw, a->th);
}

/* ---- sound ---------------------------------------------------------------------------------------------------------------- */

static bool lead_conf_path(char *out, size_t n)
{
    const char *x = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
    if (x && *x)
        snprintf(out, n, "%s/mellagui/viewer.conf", x);
    else if (h && *h)
        snprintf(out, n, "%s/.config/mellagui/viewer.conf", h);
    else
        return false;
    return true;
}

/* The sound's lead: $MELLAGUI_AUDIO_LEAD_MS, else viewer.conf (`audio-lead-ms = N`), else the built-in value. */
static int audio_lead(struct app *a)
{
    if (a->aud_lead_set)
        return a->aud_lead;
    a->aud_lead_set = true;
    a->aud_lead = AUDIO_LEAD_MS;
    const char *e = getenv("MELLAGUI_AUDIO_LEAD_MS");
    char path[PATH_MAX];
    if (e && *e) {
        a->aud_lead = atoi(e);
    } else if (lead_conf_path(path, sizeof(path))) {
        FILE *f = fopen(path, "r");
        char line[120];
        int v;
        while (f && fgets(line, sizeof(line), f))
            if (sscanf(line, " audio-lead-ms = %d", &v) == 1)
                a->aud_lead = v;
        if (f)
            fclose(f);
    }
    a->aud_lead = LM_CLAMP(a->aud_lead, -2000, 2000);
    return a->aud_lead;
}

/* viewer.conf: the sound's lead and whether the strip shows. */
static void conf_save(struct app *a)
{
    char path[PATH_MAX], dir[PATH_MAX];
    if (!lead_conf_path(path, sizeof(path)))
        return;
    int lead = audio_lead(a);
    snprintf(dir, sizeof(dir), "%s", path);
    *strrchr(dir, '/') = 0;
    for (char *p = dir + 1; *p; p++)                            /* mkdir -p */
        if (*p == '/') {
            *p = 0;
            mkdir(dir, 0700);
            *p = '/';
        }
    mkdir(dir, 0700);
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "# mellagui-view: how many ms ahead of the picture the sound starts ([ = earlier, ] = later); whether the strip of small pictures shows (T)\naudio-lead-ms = %d\nthumbs = %d\n",
            lead, a->thumbs_on ? 1 : 0);
    fclose(f);
}

static void conf_load_thumbs(struct app *a)
{
    char path[PATH_MAX], line[120];
    int v;
    a->thumbs_on = true;
    if (!lead_conf_path(path, sizeof(path)))
        return;
    FILE *f = fopen(path, "r");
    while (f && fgets(line, sizeof(line), f))
        if (sscanf(line, " thumbs = %d", &v) == 1)
            a->thumbs_on = v != 0;
    if (f)
        fclose(f);
}

static void audio_kill(struct app *a)
{
    ffaudio_close(a->aud);
    a->aud = NULL;
}

/* A seek (or a new file): the sound starts again from the new place when it is wanted. */
static void audio_reset(struct app *a)
{
    audio_kill(a);
    a->aud_ended = false;
}

/* Called after every loop turn: the sound runs while the video plays, is stopped while it is paused, and starts where the picture is. */
static void audio_follow(struct app *a)
{
    if (!a->has_audio || !a->vmode)
        return;
    if (a->mute || a->aud_dead || ffaudio_players() == 0) {
        audio_kill(a);
        return;
    }
    if (a->aud) {
        bool quick;
        if (!ffaudio_alive(a->aud, &quick)) {
            audio_kill(a);
            if (quick && a->aud_player + 1 < ffaudio_players())
                a->aud_player++;                                /* that player could not reach a sound server: the next one */
            else if (quick)
                a->aud_dead = true;
            else
                a->aud_ended = true;                            /* the sound track is over */
        }
    }
    uint64_t len = video_len(a);
    bool want = a->playing && (!len || video_pos(a) < len) && !(a->vmode == 2 && a->fwant);     /* (ffmpeg's first picture starts the clock: the sound waits for it) */
    if (!want) {
        audio_kill(a);                                          /* a pause is silence at once (a stopped player would play out its buffer); on again it starts afresh, lead and all */
        return;
    }
    if (a->aud)
        return;
    if (a->aud_ended || a->aud_dead)
        return;
    char err[120];
    int64_t at = (int64_t)video_pos(a) + audio_lead(a);
    a->aud = ffaudio_start(g_ff_path, at > 0 ? (uint64_t)at : 0, a->aud_player, err, sizeof(err));
    if (!a->aud)
        a->aud_dead = true;
}

/* Takes pictures from ffmpeg: the one that is due now is shown, late ones are dropped. */
static void ff_tick(struct app *a)
{
    if (!a->fd || (!a->playing && !a->fwant))
        return;
    uint64_t pos = video_pos(a);
    for (int guard = 0; guard < 4; guard++) {
        if (!a->fready) {
            int r, waits = 0;
            for (;;) {                                      /* a picture that is (nearly) due is waited for on the pipe, not by sleeping in small steps */
                r = ffdec_read(a->fd, a->fbuf, &a->fhave);
                if (r != 0)
                    break;
                uint64_t due_now = a->fbase + a->fidx * 1000 / FF_FPS;
                if (!(a->fwant || video_pos(a) + 20 >= due_now) || waits++ >= 6)
                    break;
                ffdec_wait(a->fd, 5);
            }
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
    audio_reset(a);
    if (a->vmode == 4) {                                        /* a sound file: nothing to show but the clock */
        a->win.redraw = true;
        return;
    }
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
    audio_follow(a);
    if (a->vmode == 4) {
        if (a->playing && video_len(a) && video_pos(a) >= video_len(a)) {
            a->pos_ms = video_len(a);
            a->playing = false;
            a->win.redraw = true;
            a->next_track = true;                               /* (the loop goes on to the next sound file) */
        } else if (a->playing) {
            a->win.redraw = true;                               /* the clock */
        }
        return;
    }
    if (a->vmode == 2) {
        int tw, th;                                             /* the window was resized (or "fit" was switched): pictures of the right size */
        ff_target(a, a->fit, &tw, &th);
        if ((abs(tw - a->fw) > 8 || abs(th - a->fh) > 8 || (!a->fit && a->fw != a->tw)) && lm_now_ms() - (uint64_t)a->ff_started > 400)
            ff_resize(a, tw, th);
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
    if (a->vmode == 4)
        return a->playing ? 250 : -1;
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

/* Stop: pause and go back to the first picture. */
static void video_stop(struct app *a)
{
    if (!a->vmode)
        return;
    if (a->playing) {
        a->pos_ms = video_pos(a);
        a->playing = false;
    }
    video_seek(a, 0);
    a->win.redraw = true;
}

static int vbtn_x(int i) { return 6 + i * (VBTN_W + VBTN_GAP); }

/* The video button at the point of the window (0..3), or -1. */
static int vbtn_at(const struct app *a, double x, double y)
{
    int top = bar_y(a) + 5;
    if (!a->vmode || y < top || y >= top + VBTN_H)
        return -1;
    for (int i = 0; i < N_VBTN; i++)
        if (x >= vbtn_x(i) && x < vbtn_x(i) + VBTN_W)
            return i;
    return -1;
}

static void vbtn_do(struct app *a, int i)
{
    switch (i) {
    case 0: video_seek(a, (int64_t)video_pos(a) - 5000); break;
    case 1: video_toggle(a); break;
    case 2: video_stop(a); break;
    case 3: video_seek(a, (int64_t)video_pos(a) + 5000); break;
    }
}

/* A triangle of `h` rows pointing right (dir 1) or left (-1), tip at the far side, `len` wide, its middle at (cx, cy). */
static void vtri(struct wlwin *w, int cx, int cy, int h, int len, int dir, uint32_t col)
{
    for (int t = 0; t < h; t++) {
        double half = (h - 1) / 2.0, frac = 1.0 - (t > half ? t - half : half - t) / (half + 0.5);
        int n = LM_MAX((int)(frac * len + 0.5), 1);
        wlwin_fill(w, dir > 0 ? cx - len / 2 : cx + len / 2 - n, cy - h / 2 + t, n, 1, col);
    }
}

static void paint_vbuttons(struct app *a, struct wlwin *w, int vh)
{
    for (int i = 0; i < N_VBTN; i++) {
        int x = vbtn_x(i), y = vh + 5, cx = x + VBTN_W / 2, cy = y + VBTN_H / 2;
        uint32_t bg = a->vpress == i + 1 && a->vhover == i + 1 ? lm_theme.viewer_button_down : a->vhover == i + 1 ? lm_theme.viewer_button_hover : lm_theme.viewer_button;
        uint32_t fg = i == 1 && !a->playing ? lm_theme.accent : BAR_FG;
        wlwin_drop(w, x, y, VBTN_W, VBTN_H, lm_button_radius(VBTN_H / 2), bg);
        switch (i) {
        case 0: vtri(w, cx - 4, cy, 9, 5, -1, fg); vtri(w, cx + 3, cy, 9, 5, -1, fg); break;
        case 1:
            if (a->playing) {
                wlwin_fill(w, cx - 4, cy - 5, 3, 10, fg);
                wlwin_fill(w, cx + 1, cy - 5, 3, 10, fg);
            } else {
                vtri(w, cx, cy, 11, 8, 1, fg);
            }
            break;
        case 2: wlwin_fill(w, cx - 4, cy - 4, 9, 9, fg); break;
        case 3: vtri(w, cx - 3, cy, 9, 5, 1, fg); vtri(w, cx + 4, cy, 9, 5, 1, fg); break;
        }
    }
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
    lm_img_free(&a->cover);
    free(a->scaled);
    a->scaled = NULL;
    a->err[0] = 0;
    a->next_track = false;
    lm_mp4r_close(a->vid);
    a->vid = NULL;
    lm_h264dec_free(a->hd);
    a->hd = NULL;
    a->hdone = ~0u;
    ffdec_close(a->fd);
    a->fd = NULL;
    free(a->fbuf);
    a->fbuf = NULL;
    audio_reset(a);
    a->has_audio = false;
    a->aud_dead = false;
    a->vmode = 0;
    a->playing = false;
    a->vframe = 0;
    a->pos_ms = 0;
    a->nexif = 0;
    if (a->n) {
        char path[PATH_MAX + 300];
        snprintf(path, sizeof(path), "%s/%s", a->dir, a->names[a->cur]);
        if (is_video_name(a->names[a->cur])) {
            char why[160] = "";
            snprintf(g_ff_path, sizeof(g_ff_path), "%s", path);
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
                struct ffdec_info ai;
                char aerr[120];
                if (ffpipe_ffmpeg_path() && ffdec_probe(path, &ai, aerr, sizeof(aerr)))      /* does the file have sound? */
                    a->has_audio = ai.has_audio;
                a->vmode = a->hd ? 3 : 1;
                show_frame(a, 0);
                if (a->hd && a->err[0] && ffpipe_ffmpeg_path()) {   /* our decoder knows only a part of H.264 (it said "not supported" at the first picture): ffmpeg then */
                    snprintf(why, sizeof(why), "%s", a->err);
                    a->err[0] = 0;
                    lm_h264dec_free(a->hd);
                    a->hd = NULL;
                    lm_mp4r_close(a->vid);
                    a->vid = NULL;
                    lm_img_free(&a->img);
                    a->vmode = 0;
                    a->has_audio = false;
                }
                a->play_t0 = lm_now_ms();
                a->playing = a->img.px != NULL;
            }
            if (!a->vid) {                                          /* anything else: ffmpeg, when it is installed */
                struct ffdec_info info;
                char ferr[160];
                if (ffpipe_ffmpeg_path() && ffdec_probe(path, &info, ferr, sizeof(ferr))) {
                    a->tw = info.w;
                    a->th = info.h;
                    a->has_audio = info.has_audio;
                    ff_target(a, true, &a->fw, &a->fh);                 /* it starts "fit": no bigger pictures than the window shows */
                    a->fdur = info.duration_ms;
                    a->fbuf = malloc((size_t)a->fw * (size_t)a->fh * 4);
                    a->img.w = a->fw;
                    a->img.h = a->fh;
                    a->img.px = calloc((size_t)a->fw * (size_t)a->fh, 4);
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
        } else if (is_audio_name(a->names[a->cur])) {
            char aerr[160] = "";
            snprintf(g_ff_path, sizeof(g_ff_path), "%s", path);
            if (ffaudio_probe(path, &a->ainfo, aerr, sizeof(aerr))) {
                a->vmode = 4;
                a->has_audio = true;
                a->fdur = a->ainfo.duration_ms;
                if (a->ainfo.cover && !ffaudio_cover(path, &a->cover))
                    a->cover.px = NULL;
                a->play_t0 = lm_now_ms();
                a->playing = true;
            } else {
                snprintf(a->err, sizeof(a->err), "%s", aerr);
            }
        } else if (lm_img_load(path, &a->img, a->err, sizeof(a->err)) < 0) {
            a->img.px = NULL;
        } else {
            struct lm_exif ex;
            if (lm_exif_read_file(path, &ex))
                a->nexif = lm_exif_lines(&ex, a->exif, 8);
        }
    }
    a->fit = true;
    a->zoom = fit_zoom(a);
    a->win.redraw = true;
    set_title(a);
    if (!a->vmode && a->img.px)                                 /* the picture is decoded already: its small copy costs nothing */
        th_from_image(a, a->cur, &a->img);
    if (a->cur >= 0 && a->cur < a->thn)
        a->thm[a->cur].tried = true;
    strip_show(a);
}

static double cur_zoom(const struct app *a) { return a->fit ? fit_zoom(a) : a->zoom; }

/* The zoom as a share of the picture's real size (a video shown "fit" may be decoded smaller than it is). */
static double cur_zoom_true(const struct app *a)
{
    double z = cur_zoom(a);
    return a->vmode == 2 && a->tw ? z * (double)a->img.w / a->tw : z;
}

static void zoom_to(struct app *a, double z, double mx, double my)
{
    if (!a->img.px)
        return;
    ff_ensure_full(a);
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

static void text_c(struct wlwin *w, int vw, int y, const char *s, int scale, bool bold, uint32_t col)
{
    int tw = (bold ? lm_font_text_width_bold(s) : lm_font_text_width(s)) * scale;
    int x = (vw - tw) / 2;
    if (x < 8)
        x = 8;
    if (bold)
        wlwin_text_bold(w, x, y, s, scale, col, vw - 16);
    else
        wlwin_text(w, x, y, s, scale, col, vw - 16);
}

/* A sound file: the picture stored in it (or a note), the title, who and what, and the clock in the bar. */
static void paint_audio(struct app *a, struct wlwin *w, int vw, int vh, char *status, size_t status_n)
{
    const struct ffaudio_info *ai = &a->ainfo;
    char title[200];
    if (ai->title[0]) {
        snprintf(title, sizeof(title), "%s", ai->title);
    } else {
        snprintf(title, sizeof(title), "%s", a->names[a->cur]);
        char *dot = strrchr(title, '.');
        if (dot)
            *dot = 0;
    }
    int side = LM_CLAMP(vh - 190, 96, 340);
    if (vw - 40 < side)
        side = LM_MAX(vw - 40, 64);
    int total = side + 24 + 2 * 32 + 22 + 22 + 20;
    int y = LM_MAX((vh - total) / 2, 12), x = (vw - side) / 2;
    if (a->cover.px) {
        int cw = a->cover.w, ch = a->cover.h, dw = side, dh = side;
        if (cw > ch) dh = side * ch / cw; else dw = side * cw / ch;
        wlwin_blit(w, (vw - dw) / 2, y + (side - dh) / 2, dw, dh, a->cover.px, cw, ch, cw);
    } else {
        wlwin_round_fill(w, x, y, side, side, side / 8, lm_theme.control);
        int cx = x + side / 2, cy = y + side / 2, u = side / 8;
        uint32_t fg = lm_theme.text_dim;
        wlwin_fill(w, cx - u * 2, cy - u * 2, u / 2 + 1, u * 3 + u / 2, fg);                /* the two stems and the beam of a pair of notes */
        wlwin_fill(w, cx + u, cy - u * 2 - u / 2, u / 2 + 1, u * 3 + u / 2, fg);
        wlwin_fill(w, cx - u * 2, cy - u * 2 - u / 2, u * 3 + u / 2 + 1, u / 2 + 1, fg);
        wlwin_round_fill(w, cx - u * 3, cy + u, u * 2, u * 3 / 2, u * 3 / 4, fg);            /* the heads */
        wlwin_round_fill(w, cx, cy + u * 2 - u / 2, u * 2, u * 3 / 2, u * 3 / 4, fg);
    }
    y += side + 24;
    text_c(w, vw, y, title, 2, true, lm_theme.text_bright);
    y += LM_FONT_H * 2 + 8;
    char who[220] = "";
    if (ai->artist[0] && ai->album[0])
        snprintf(who, sizeof(who), "%s  -  %s", ai->artist, ai->album);
    else if (ai->artist[0] || ai->album[0])
        snprintf(who, sizeof(who), "%s", ai->artist[0] ? ai->artist : ai->album);
    if (who[0])
        text_c(w, vw, y, who, 1, false, lm_theme.text);
    y += LM_FONT_H + 6;
    char tech[120];
    char rate[24] = "", ch[24] = "", kb[24] = "";
    if (ai->rate)
        snprintf(rate, sizeof(rate), "  %d Hz", ai->rate);
    if (ai->channels[0])
        snprintf(ch, sizeof(ch), "  %s", ai->channels);
    if (ai->kbps)
        snprintf(kb, sizeof(kb), "  %d kb/s", ai->kbps);
    snprintf(tech, sizeof(tech), "%s%s%s%s", ai->codec, rate, ch, kb);
    text_c(w, vw, y, tech, 1, false, lm_theme.text_dim);
    char t1[16], t2[16];
    fmt_time(t1, sizeof(t1), video_pos(a));
    fmt_time(t2, sizeof(t2), video_len(a));
    char snd[40] = "";
    if (ffaudio_players() == 0 || a->aud_dead)
        snprintf(snd, sizeof(snd), "   no sound player");
    else if (a->mute)
        snprintf(snd, sizeof(snd), "   sound off (M)");
    snprintf(status, status_n, "%s   %s / %s   %s%s   %d/%d", a->names[a->cur], t1, t2, a->playing ? "playing" : "paused", snd, a->cur + 1, a->n);
}

/* ---- what is shown: a file (its folder, starting at it) or a folder (its newest picture) ---------------------------------------------------- */

static void set_target(struct app *a, const char *arg, bool load)
{
    struct stat st;
    char base[256] = "";
    const char *keep = NULL;
    if (stat(arg, &st) == 0 && !S_ISDIR(st.st_mode)) {
        const char *slash = strrchr(arg, '/');
        snprintf(a->dir, sizeof(a->dir), "%.*s", slash ? (int)(slash - arg) : 1, slash ? arg : ".");
        if (slash && slash == arg)
            snprintf(a->dir, sizeof(a->dir), "/");
        snprintf(base, sizeof(base), "%.255s", slash ? slash + 1 : arg);
        keep = base;
    } else {
        snprintf(a->dir, sizeof(a->dir), "%s", arg);
        size_t l = strlen(a->dir);
        while (l > 1 && a->dir[l - 1] == '/')                   /* ("/home/me/Pictures/" is the folder "/home/me/Pictures") */
            a->dir[--l] = 0;
    }
    scan_dir(a, keep);
    if (load)
        load_current(a);
}

/* ---- the Open card: the folder list of the shared Open dialog (wlpicker.h) --------------------------------------------------------------- */

static int card_rows(const struct app *a) { return wlpicker_fit(a->win.bh, 170); }

static void card_rect(const struct app *a, struct wlcard *c)
{
    int rows = card_rows(a);
    c->w = LM_MIN(560, a->win.bw - 16);
    c->h = LM_MIN(rows ? 170 + rows * WLPICKER_ROW_H : 150, a->win.bh - 16);
    c->x = (a->win.bw - c->w) / 2;
    c->y = (a->win.bh - c->h) / 2;
}

static int card_field_y(const struct wlcard *c) { return c->y + 42 + LM_FONT_H + 8; }
static int card_list_y(const struct wlcard *c) { return card_field_y(c) + 30; }

static void card_base(const struct app *a, char *base, size_t n)   /* what a relative path means: the folder that is shown */
{
    snprintf(base, n, "%s%s", a->dir[0] ? a->dir : ".", a->dir[0] && a->dir[strlen(a->dir) - 1] == '/' ? "" : "/");
}

static bool card_want(const char *name) { return is_image_name(name); }

static void card_open(struct app *a)
{
    char base[PATH_MAX];
    card_base(a, base, sizeof(base));
    a->card = true;
    snprintf(a->pbuf, sizeof(a->pbuf), "%s", base);
    a->perr[0] = 0;
    a->c_hover = a->c_down = -1;
    wlpicker_reset(&a->pk);
    wltext_end(a->pbuf);
    a->win.redraw = true;
}

static void card_close(struct app *a)
{
    a->card = false;
    a->win.redraw = true;
}

/* Open: the path in the field is a folder (it becomes the viewer's folder) or a file (it is shown, with its folder). */
static void card_accept(struct app *a)
{
    char base[PATH_MAX], path[PATH_MAX];
    card_base(a, base, sizeof(base));
    if (!a->pbuf[0]) {
        snprintf(a->perr, sizeof(a->perr), "Type a folder or a picture");
        return;
    }
    lm_resolve_path(base, a->pbuf, path, sizeof(path));
    struct stat st;
    if (stat(path, &st) < 0) {
        snprintf(a->perr, sizeof(a->perr), "No such folder or file");
        return;
    }
    set_target(a, path, true);
    card_close(a);
}

static void card_key(struct app *a, uint32_t ks)
{
    char base[PATH_MAX];
    card_base(a, base, sizeof(base));
    int rows = card_rows(a);
    if (ks == LM_KS_Escape) {
        card_close(a);
    } else if (rows && (ks == LM_KS_Up || ks == LM_KS_Down || ks == LM_KS_Page_Up || ks == LM_KS_Page_Down ||
                        ((ks == LM_KS_Return || ks == LM_KS_KP_Enter) && a->pk.sel >= 0))) {
        if (wlpicker_key(&a->pk, base, a->pbuf, sizeof(a->pbuf), rows, ks) == WLPICKER_OPEN)
            card_accept(a);                                     /* a folder was entered, a picture is taken */
        a->perr[0] = 0;
    } else if (ks == LM_KS_Return || ks == LM_KS_KP_Enter) {
        card_accept(a);
    } else if (ks == LM_KS_Tab) {
        char list[512];
        int n = lm_complete_path(base, a->pbuf, sizeof(a->pbuf), true, list, sizeof(list));
        if (n > 1)
            snprintf(a->perr, sizeof(a->perr), "%s", list);
        else
            a->perr[0] = 0;
        wltext_end(a->pbuf);
        wlpicker_typed(&a->pk);
    } else if (wltext_edit(&a->clip, a->pbuf, sizeof(a->pbuf), ks, a->win.mods)) {
        a->perr[0] = 0;
        wlpicker_typed(&a->pk);                                 /* (the list follows what is typed) */
    }
    a->win.redraw = true;
}

static void card_pointer(struct app *a, uint32_t op, const union lm_arg *args)
{
    struct wlcard c;
    card_rect(a, &c);
    char base[PATH_MAX];
    card_base(a, base, sizeof(base));
    int rows = card_rows(a), fx = c.x + WLCARD_PAD, fw = c.w - 2 * WLCARD_PAD, ly = card_list_y(&c);
    if (op == LM_WL_POINTER_EVT_MOTION) {
        a->c_hover = wlcard_button_at(&c, 2, (int)a->px, (int)a->py);
        if (rows)
            wlpicker_motion(&a->pk, base, a->pbuf, ly, rows, a->py);      /* (the scroll bar's thumb is dragged) */
        a->win.redraw = true;
        return;
    }
    if (op == LM_WL_POINTER_EVT_AXIS) {
        if (rows && args[1].u == 0)
            wlpicker_wheel(&a->pk, args[2].f > 0 ? 1 : -1);
        a->win.redraw = true;
        return;
    }
    if (op != LM_WL_POINTER_EVT_BUTTON)
        return;
    a->clip.serial = args[0].u;
    int k = wlcard_button_at(&c, 2, (int)a->px, (int)a->py);
    if (args[3].u != 1) {
        wlpicker_release(&a->pk);
        if (a->c_down >= 0 && a->c_down == k) {                 /* a button is done when it is let go on */
            a->c_down = -1;
            if (k == 0)
                card_close(a);
            else
                card_accept(a);
        }
        a->c_down = -1;
        a->win.redraw = true;
        return;
    }
    if (args[2].u != 272)
        return;
    if (k >= 0) {
        a->c_down = k;
    } else if (!wlcard_has(&c, (int)a->px, (int)a->py)) {
        card_close(a);                                          /* a click outside the card cancels */
        return;
    } else if (a->py >= card_field_y(&c) && a->py < card_field_y(&c) + 26) {
        wltext_click(a->pbuf, (int)a->px - (c.x + WLCARD_PAD + 8), (a->win.mods & LM_MOD_SHIFT) != 0);
    } else if (rows) {
        if (wlpicker_press(&a->pk, base, a->pbuf, sizeof(a->pbuf), fx, ly, fw, rows, a->px, a->py, args[1].u, (uint32_t)lm_metrics.double_click_ms) == WLPICKER_OPEN)
            card_accept(a);
        a->perr[0] = 0;
    }
    a->win.redraw = true;
}

static void paint_card(struct app *a)
{
    struct wlwin *w = &a->win;
    struct wlcard c;
    card_rect(a, &c);
    wlcard_frame(w, &c, c.w, c.h, "Open a folder or a picture", lm_theme.accent, BG, lm_theme.text_bright);
    wlwin_text(w, c.x + WLCARD_PAD, c.y + 42, "Pick a folder below (Open shows it) or a picture, or type a path (Tab completes)", 1, lm_theme.text_dim, c.w - 2 * WLCARD_PAD);
    int fy = card_field_y(&c), rows = card_rows(a);
    wlwin_round_fill(w, c.x + WLCARD_PAD, fy, c.w - 2 * WLCARD_PAD, 26, 8, lm_theme.field);
    wltext_paint(w, c.x + WLCARD_PAD + 8, fy + 5, c.w - 2 * WLCARD_PAD - 16, a->pbuf, true, lm_theme.text_soft, lm_theme.button_hover, lm_theme.accent);
    char base[PATH_MAX];
    card_base(a, base, sizeof(base));
    if (rows)
        wlpicker_paint(w, &a->pk, base, a->pbuf, c.x + WLCARD_PAD, card_list_y(&c), c.w - 2 * WLCARD_PAD, rows, a->px, a->py, a->has_pointer);
    if (a->perr[0])
        wlwin_text(w, c.x + WLCARD_PAD, c.y + c.h - WLCARD_PAD - WLCARD_BTN_H - 22, a->perr, 1, lm_theme.error, c.w - 2 * WLCARD_PAD);
    const char *labels[2] = { "Cancel", "Open" };
    const uint32_t cols[2] = { lm_theme.btn_grey, lm_theme.btn_blue };
    wlcard_buttons(w, &c, labels, 2, cols, a->c_hover, a->c_down, lm_theme.on_colour);
}

/* ---- the strip, painted ---------------------------------------------------------------------------------------------------------------------- */

static void paint_strip(struct app *a, struct wlwin *w)
{
    int sh = strip_h(a);
    if (!sh)
        return;
    strip_clamp(a);
    int sy = strip_y(a), k = wlwin_scale(w);
    wlwin_fill(w, 0, sy, w->bw, sh, lm_theme.bar);
    wlwin_fill(w, 0, sy, w->bw, 1, lm_theme.border);
    int bx, by, bw, bh;
    if (open_btn_rect(a, &bx, &by, &bw, &bh)) {
        wlwin_drop(w, bx, by, bw, bh, lm_button_radius(bh / 2), a->open_hover ? lm_theme.btn_teal_lit : lm_theme.btn_teal);
        wlwin_text(w, bx + (bw - lm_font_text_width("Open...")) / 2, by + (bh - LM_FONT_H) / 2, "Open...", 1, lm_theme.on_colour, 0);
    }
    int clip_x = TS_X0 - 4, clip_w = w->bw - clip_x;
    for (int i = LM_MAX((int)(a->strip_x / TS_PITCH), 0); i < a->n; i++) {
        int x = item_x(a, i), y = sy + (sh - TS_H) / 2;
        if (x >= w->bw)
            break;
        if (x + TS_W < clip_x)
            continue;
        bool whole = x >= clip_x + 3 && x + TS_W + 3 <= w->bw;                    /* a partly hidden one has no frame (a frame cannot be cut) */
        const struct vthumb *t = i < a->thn ? &a->thm[i] : NULL;
        if (whole)
            wlwin_round_fill(w, x - 3, y - 3, TS_W + 6, TS_H + 6, 6, i == a->cur ? lm_theme.accent : i == a->th_hover ? lm_theme.text_soft : lm_theme.border);
        if (t && t->px) {
            double sc = LM_MIN((double)TS_W / t->w, (double)TS_H / t->h);
            int dw = LM_MAX((int)(t->w * sc), 1), dh = LM_MAX((int)(t->h * sc), 1);
            if (sc > 1.0 && t->w < TS_W && t->h < TS_H) {                                       /* a small picture is not enlarged */
                dw = t->w;
                dh = t->h;
            }
            int pw = dw * k, ph = dh * k;
            uint32_t *buf = malloc((size_t)pw * (size_t)ph * 4);
            if (buf) {
                lm_thumb_box(t->px, t->w, t->h, buf, pw, ph);
                wlwin_blit_px(w, x + (TS_W - dw) / 2, y + (TS_H - dh) / 2, buf, pw, ph, pw, clip_x, sy, clip_w, sh);
                free(buf);
            }
            if (whole && is_video_name(a->names[i])) {                                         /* a poster of a clip: a play mark on it */
                int cx = x + TS_W / 2, cy = y + TS_H / 2;
                wlwin_round_fill(w, cx - 12, cy - 12, 24, 24, 12, lm_theme.ink);
                vtri(w, cx + 1, cy, 13, 9, 1, lm_theme.on_colour);
            }
        } else if (whole) {                                                                     /* a tile: not made yet, a video, a sound file */
            wlwin_round_fill(w, x, y, TS_W, TS_H, 4, lm_theme.control);
            int cx = x + TS_W / 2, cy = y + TS_H / 2;
            if (is_video_name(a->names[i]))
                vtri(w, cx, cy, 15, 11, 1, lm_theme.text_dim);
            else if (is_audio_name(a->names[i]))
                for (int b = 0; b < 5; b++)                                                     /* the bars of a sound */
                    wlwin_fill(w, cx - 12 + b * 5, cy + 7 - (4 + (b * 7 + 3) % 5 * 3), 3, 4 + (b * 7 + 3) % 5 * 3, lm_theme.text_dim);
        }
    }
}

/* $MELLAGUI_VIEW_DUMP: the state, the places in the strip and the card (window pixels), for the tests. */
static void write_dump(struct app *a)
{
    if (!a->dump[0])
        return;
    char tmp[PATH_MAX + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", a->dump);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return;
    int loaded = 0;
    for (int i = 0; i < a->thn; i++)
        loaded += a->thm[i].px != NULL;
    fprintf(f, "state dir=%s n=%d cur=%d name=%s strip=%d sx=%d loaded=%d vmode=%d card=%d ipc=%d hover=%d\n", a->dir, a->n, a->cur, a->n ? a->names[a->cur] : "-", strip_h(a) ? 1 : 0,
            (int)a->strip_x, loaded, a->vmode, a->card, a->ipc >= 0, a->th_hover);
    fprintf(f, "view %d %d %d %d\n", 0, 0, view_w(a), view_h(a));
    int bx, by, bw, bh;
    if (open_btn_rect(a, &bx, &by, &bw, &bh))
        fprintf(f, "open %d %d %d %d\n", bx, by, bw, bh);
    for (int i = 0; strip_h(a) && i < a->n; i++) {
        int x = item_x(a, i), y = strip_y(a) + (STRIP_H - TS_H) / 2;
        if (x + TS_W > TS_X0 && x < a->win.bw)
            fprintf(f, "item %d %s %d %d %d %d %d\n", i, a->names[i], x, y, TS_W, TS_H, i < a->thn && a->thm[i].px != NULL);
    }
    if (a->card) {
        struct wlcard c;
        card_rect(a, &c);
        char base[PATH_MAX];
        card_base(a, base, sizeof(base));
        fprintf(f, "card %d %d %d %d field=%s err=%s\n", c.x, c.y, c.w, c.h, a->pbuf, a->perr);
        for (int k = 0; k < 2; k++) {
            int x, y, w, h;
            wlcard_button_rect(&c, k, 2, &x, &y, &w, &h);
            fprintf(f, "cbtn %s %d %d %d %d\n", k ? "open" : "cancel", x, y, w, h);
        }
        if (card_rows(a))
            wlpicker_dump(f, &a->pk, base, a->pbuf, c.x + WLCARD_PAD, card_list_y(&c), c.w - 2 * WLCARD_PAD, card_rows(a), 0);
    }
    if (fclose(f) != 0 || rename(tmp, a->dump) < 0)
        unlink(tmp);
}

static void paint(struct wlwin *w)
{
    struct app *a = w->data;
    int vw = view_w(a), vh = view_h(a);
    wlwin_fill(w, 0, 0, w->bw, w->bh, BG);
    char status[400];
    if (a->vmode == 4) {
        paint_audio(a, w, vw, vh, status, sizeof(status));
    } else if (a->img.px) {
        double z = cur_zoom(a);
        int sw = LM_MAX((int)(a->img.w * z + 0.5), 1), sh = LM_MAX((int)(a->img.h * z + 0.5), 1);
        int ox = sw <= vw ? (vw - sw) / 2 : (int)LM_CLAMP(a->ox, (double)(vw - sw), 0.0);
        int oy = sh <= vh ? (vh - sh) / 2 : (int)LM_CLAMP(a->oy, (double)(vh - sh), 0.0);
        if (sw > vw)
            a->ox = ox;
        if (sh > vh)
            a->oy = oy;
        /* The picture is put on the buffer's own pixels: zoom, size and offsets in physical pixels (1 on a screen without scale). */
        int k = wlwin_scale(w);
        double zp = z * k;
        int vwp = vw * k, vhp = vh * k;
        int swp = LM_MAX((int)(a->img.w * zp + 0.5), 1), shp = LM_MAX((int)(a->img.h * zp + 0.5), 1);
        int oxp = ox * k, oyp = oy * k;
        const uint32_t *srcp = a->img.px;
        int stridep = a->img.w;
        if (zp < 0.999) {
            if (!a->scaled || a->scaled_zoom != zp)
                build_scaled(a, zp);
            if (a->scaled) {
                srcp = a->scaled;
                stridep = a->sw;
                swp = a->sw;
                shp = a->sh;
            }
        }
        for (int y = LM_MAX(0, -oyp); y < shp && oyp + y < vhp; y++) {
            uint32_t *dst = w->pixels + (size_t)(oyp + y) * (size_t)w->pw;
            int x0 = LM_MAX(0, -oxp), x1 = LM_MIN(swp, vwp - oxp);
            if (zp > 1.001) {                                   /* nearest neighbour */
                int sy = (int)((double)y / zp);
                for (int x = x0; x < x1; x++)
                    dst[oxp + x] = srcp[(size_t)sy * (size_t)stridep + (size_t)((double)x / zp)];
            } else if (x1 > x0) {
                memcpy(dst + oxp + x0, srcp + (size_t)y * (size_t)stridep + (size_t)x0, (size_t)(x1 - x0) * 4);
            }
        }
        if (a->show_info && a->nexif && !a->vmode) {                      /* the EXIF lines in a dark box over the corner */
            int bw = 0;
            for (int i = 0; i < a->nexif; i++)
                bw = LM_MAX(bw, lm_font_text_width(a->exif[i]));
            bw = LM_MIN(bw + 24, vw - 16);
            int bh = a->nexif * (LM_FONT_H + 2) + 16;
            wlwin_round_fill(w, 8, 8, bw, bh, 10, lm_theme.ink);
            for (int i = 0; i < a->nexif; i++)
                wlwin_text(w, 20, 16 + i * (LM_FONT_H + 2), a->exif[i], 1, lm_theme.text_bright, bw - 24);
        }
        if (a->vmode) {
            char t1[16], t2[16];
            fmt_time(t1, sizeof(t1), video_pos(a));
            fmt_time(t2, sizeof(t2), video_len(a));
            double zs = a->vmode == 2 && a->tw ? z * (double)a->img.w / a->tw : z;
            char sndtxt[48] = "";
            if (a->has_audio) {
                if (a->mute)
                    snprintf(sndtxt, sizeof(sndtxt), "   sound off (M)");
                else if (ffaudio_players() == 0 || a->aud_dead)
                    snprintf(sndtxt, sizeof(sndtxt), "   no sound player");
                else
                    snprintf(sndtxt, sizeof(sndtxt), "   sound %+d ms ([ ])", audio_lead(a));
            }
            snprintf(status, sizeof(status), "%s   %dx%d   %s / %s   %s%s   %d%%   %d/%d", a->names[a->cur], a->vmode == 2 ? a->tw : a->img.w,
                     a->vmode == 2 ? a->th : a->img.h, t1, t2, a->playing ? "playing" : "paused",
                     sndtxt, (int)(zs * 100 + 0.5), a->cur + 1, a->n);
        } else {
            snprintf(status, sizeof(status), "%s   %dx%d   %d%%%s   %d/%d%s", a->names[a->cur], a->img.w, a->img.h,
                     (int)(z * 100 + 0.5), a->fit ? " (fit)" : "", a->cur + 1, a->n, a->nexif ? "   I: EXIF data" : "");
        }
    } else if (a->n) {
        snprintf(status, sizeof(status), "%s: %s   %d/%d", a->names[a->cur], a->err, a->cur + 1, a->n);
        wlwin_text(w, 16, 16, a->err, 1, lm_theme.error, vw - 32);
    } else {
        snprintf(status, sizeof(status), "no images in %.300s", a->dir);
        wlwin_text(w, 16, 16, "No images or videos here. Press Print to take a screenshot, R to look again.", 1, BAR_FG, vw - 32);
    }
    paint_strip(a, w);
    int by = bar_y(a);
    wlwin_fill(w, 0, by, w->bw, w->bh - by, BAR_BG);
    if (a->vmode && video_len(a)) {                              /* the progress: a strip along the top of the bar */
        wlwin_fill(w, 0, by, w->bw, 4, lm_theme.divider);
        wlwin_fill(w, 0, by, (int)((uint64_t)w->bw * video_pos(a) / video_len(a)), 4, lm_theme.accent);
    }
    int tx = 8;
    if (a->vmode) {
        paint_vbuttons(a, w, by);
        tx = vbtn_x(N_VBTN) + 4;
    }
    wlwin_text(w, tx, by + (BAR_H - LM_FONT_H) / 2 + 2, status, 1, BAR_FG, w->bw - tx - 8);
    if (a->card)
        paint_card(a);
    write_dump(a);
}

/* ---- input ---------------------------------------------------------------------------------------------------------- */

static void pointer_event(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
    struct app *a = w->data;
    if (a->card && op != LM_WL_POINTER_EVT_ENTER && op != LM_WL_POINTER_EVT_LEAVE) {       /* the Open card is modal */
        if (op == LM_WL_POINTER_EVT_MOTION) {
            a->px = lm_fixed_to_double(args[1].f);
            a->py = lm_fixed_to_double(args[2].f);
        }
        card_pointer(a, op, args);
        return;
    }
    switch (op) {
    case LM_WL_POINTER_EVT_ENTER:
        a->has_pointer = true;
        a->px = lm_fixed_to_double(args[2].f);
        a->py = lm_fixed_to_double(args[3].f);
        break;
    case LM_WL_POINTER_EVT_LEAVE:
        a->has_pointer = false;
        a->seeking = false;
        a->vhover = a->vpress = 0;
        w->redraw = true;
        a->dragging = false;
        break;
    case LM_WL_POINTER_EVT_MOTION: {
        double x = lm_fixed_to_double(args[1].f), y = lm_fixed_to_double(args[2].f);
        int th = strip_item_at(a, x, y);
        bool oh = open_btn_at(a, x, y);
        if (th != a->th_hover || oh != a->open_hover) {
            a->th_hover = th;
            a->open_hover = oh;
            w->redraw = true;
        }
        if (a->vmode) {
            int hb = vbtn_at(a, x, y) + 1;
            if (hb != a->vhover) {
                a->vhover = hb;
                w->redraw = true;
            }
        }
        if (a->seeking && a->vmode)
            video_seek(a, (int64_t)(LM_CLAMP(x / (double)w->bw, 0.0, 1.0) * (double)video_len(a)));
        if (a->dragging && a->img.px) {
            ff_ensure_full(a);
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
        a->clip.serial = args[0].u;
        if (args[2].u != 272)                           /* BTN_LEFT */
            break;
        if (args[3].u == 1 && strip_h(a) && a->py >= strip_y(a) && a->py < strip_y(a) + STRIP_H) {     /* the strip: a picture is shown, Open asks for a folder */
            int i = strip_item_at(a, a->px, a->py);
            if (open_btn_at(a, a->px, a->py))
                card_open(a);
            else if (i >= 0)
                go(a, i);
            break;
        }
        if (args[3].u == 1 && a->vmode && vbtn_at(a, a->px, a->py) >= 0) {    /* a button of the bar: done when it is let go on it */
            a->vpress = vbtn_at(a, a->px, a->py) + 1;
            w->redraw = true;
            break;
        }
        if (args[3].u != 1 && a->vpress) {
            int i = vbtn_at(a, a->px, a->py);
            if (i >= 0 && i + 1 == a->vpress)
                vbtn_do(a, i);
            a->vpress = 0;
            w->redraw = true;
            break;
        }
        if (args[3].u == 1 && a->vmode && a->py >= bar_y(a)) {   /* a click on the bar of a video seeks */
            a->seeking = true;
            video_seek(a, (int64_t)(LM_CLAMP(a->px / (double)w->bw, 0.0, 1.0) * (double)video_len(a)));
            break;
        }
        if (args[3].u != 1)
            a->seeking = false;
        if (args[3].u == 1) {
            uint32_t t = args[1].u;
            if (t - a->last_click_ms < (uint32_t)lm_metrics.double_click_ms && abs((int)(a->px - a->last_click_x)) < 6 && abs((int)(a->py - a->last_click_y)) < 6) {
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
        if (strip_h(a) && a->py >= strip_y(a) && a->py < strip_y(a) + STRIP_H) {      /* over the strip the wheel scrolls it */
            double v = lm_fixed_to_double(args[2].f);
            if (v != 0) {
                a->strip_x += (v > 0 ? 1 : -1) * TS_PITCH * 3;
                strip_clamp(a);
                w->redraw = true;
            }
            break;
        }
        if (args[1].u == 0) {                           /* vertical: up (negative) zooms in */
            double v = lm_fixed_to_double(args[2].f);
            if (v != 0)
                zoom_to(a, cur_zoom_true(a) * (v < 0 ? 1.25 : 1.0 / 1.25), a->px, a->py);
        }
        break;
    }
}

static void keyboard_event(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
    struct app *a = w->data;
    if (op != LM_WL_KEYBOARD_EVT_KEY || args[3].u == 0)
        return;
    a->clip.serial = args[0].u;
    uint32_t ks = lm_keymap_keysym(args[2].u, a->win.mods);
    if (a->card) {                                               /* the Open card is modal: it takes every key */
        card_key(a, ks);
        return;
    }
    switch (args[2].u) {                                         /* the player keys of a multimedia keyboard */
    case MK_PLAYPAUSE: case MK_PLAYCD:
        video_toggle(a);
        return;
    case MK_STOPCD: case MK_STOP:
        video_stop(a);
        return;
    case MK_NEXTSONG: case MK_PREVSONG: {
        int dir = args[2].u == MK_NEXTSONG ? 1 : -1;
        bool snd = a->vmode == 4;                                /* from a sound file on to the next sound file, else the next picture */
        for (int i = a->cur + dir; i >= 0 && i < a->n; i += dir)
            if (!snd || is_audio_name(a->names[i])) {
                go(a, i);
                break;
            }
        return;
    }
    }
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
    } else if (c == 'o' || c == 'O' || ((a->win.mods & LM_MOD_CTRL) && (ks == 'o' || ks == 'O'))) {
        card_open(a);                                            /* another folder */
    } else if (c == 't' || c == 'T') {                           /* the strip of small pictures */
        a->thumbs_on = !a->thumbs_on;
        conf_save(a);
        strip_show(a);
        w->redraw = true;
    } else if (!a->vmode && (c == 'i' || c == 'I')) {
        a->show_info = !a->show_info;
        w->redraw = true;
    } else if (a->vmode && c == ' ') {
        video_toggle(a);
    } else if (a->vmode && a->has_audio && (c == '[' || c == ']')) {      /* the sound comes earlier / later: 50 ms a step */
        a->aud_lead = LM_CLAMP(audio_lead(a) + (c == '[' ? 50 : -50), -2000, 2000);
        conf_save(a);
        audio_reset(a);
        w->redraw = true;
    } else if (a->vmode && (c == 'm' || c == 'M')) {
        a->mute = !a->mute;
        a->aud_ended = false;
        w->redraw = true;
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
        zoom_to(a, cur_zoom_true(a) * 1.25, cx, cy);
    } else if (c == '-' || c == '_') {
        zoom_to(a, cur_zoom_true(a) / 1.25, cx, cy);
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


/* The SVG renderer's arithmetic: shapes whose coverage is known exactly (a 512 x 512 picture at scale 1: pixel centres on the grid). */
static int svg_selftest(void)
{
    int bad = 0;
    char err[160];
    struct lm_image im;
#define SVG(body) "<svg xmlns='http://www.w3.org/2000/svg' width='512' height='512' viewBox='0 0 512 512'>" body "</svg>"
#define RENDER(s) (lm_svg_render((const uint8_t *)(s), strlen(s), &im, err, sizeof(err)) == 0 && im.w == 512 && im.h == 512)
#define A(x, y) ((im.px[(size_t)(y) * 512 + (x)] >> 24) & 255)
#define R(x, y) ((im.px[(size_t)(y) * 512 + (x)] >> 16) & 255)
#define G(x, y) ((im.px[(size_t)(y) * 512 + (x)] >> 8) & 255)
#define B(x, y) (im.px[(size_t)(y) * 512 + (x)] & 255)
    const char *rect = SVG("<rect x='100' y='100' width='100' height='50' fill='#102030'/>");
    if (!RENDER(rect) || A(100, 100) != 255 || A(199, 149) != 255 || A(99, 100) != 0 || A(200, 120) != 0 || A(150, 99) != 0 || A(150, 150) != 0 || R(150, 120) != 0x10 || G(150, 120) != 0x20 || B(150, 120) != 0x30) {
        fprintf(stderr, "svg selftest: a rectangle on whole pixels is not exact\n");
        bad++;
    }
    lm_img_free(&im);
    const char *half = SVG("<rect x='100.5' y='100' width='100' height='50' fill='black'/>");
    if (!RENDER(half) || A(100, 120) < 126 || A(100, 120) > 129 || A(200, 120) < 126 || A(200, 120) > 129 || A(150, 120) != 255) {
        fprintf(stderr, "svg selftest: half covered edge pixels are not half\n");
        bad++;
    }
    lm_img_free(&im);
    const char *circle = SVG("<circle cx='256' cy='256' r='100' fill='red'/>");
    if (RENDER(circle)) {
        double area = 0;
        for (int i = 0; i < 512 * 512; i++)
            area += (im.px[i] >> 24) / 255.0;
        if (fabs(area - M_PI * 100 * 100) > M_PI * 100 * 100 * 0.003) {
            fprintf(stderr, "svg selftest: a circle's area is %.1f, not %.1f\n", area, M_PI * 100 * 100);
            bad++;
        }
    } else {
        bad++;
    }
    lm_img_free(&im);
    const char *line = SVG("<path d='M50 100 H450' stroke='black' stroke-width='10' fill='none'/>");
    if (!RENDER(line) || A(200, 94) != 0 || A(200, 95) != 255 || A(200, 104) != 255 || A(200, 105) != 0 || A(49, 100) != 0 || A(50, 100) != 255 || A(449, 100) != 255 || A(450, 100) != 0) {
        fprintf(stderr, "svg selftest: a butt-capped stroke has the wrong extent\n");
        bad++;
    }
    lm_img_free(&im);
    const char *square = SVG("<path d='M50 100 H450' stroke='black' stroke-width='10' stroke-linecap='square' fill='none'/>");
    if (!RENDER(square) || A(45, 100) != 255 || A(44, 100) != 0 || A(454, 100) != 255 || A(455, 100) != 0) {
        fprintf(stderr, "svg selftest: a square cap does not reach half a width\n");
        bad++;
    }
    lm_img_free(&im);
    const char *star = SVG("<path d='M256 40 L330 450 L40 190 L472 190 L182 450 Z' fill='blue' fill-rule='evenodd'/>");
    const char *star_nz = SVG("<path d='M256 40 L330 450 L40 190 L472 190 L182 450 Z' fill='blue'/>");
    int centre_eo = 0, centre_nz = 0;
    if (RENDER(star)) {
        centre_eo = (int)A(256, 250);
        lm_img_free(&im);
    } else {
        bad++;
    }
    if (RENDER(star_nz)) {
        centre_nz = (int)A(256, 250);
        lm_img_free(&im);
    } else {
        bad++;
    }
    if (centre_eo != 0 || centre_nz != 255) {
        fprintf(stderr, "svg selftest: the pentagram's middle: evenodd %d (0), nonzero %d (255)\n", centre_eo, centre_nz);
        bad++;
    }
    const char *op = SVG("<rect width='512' height='512' fill='white' fill-opacity='0.5'/><rect width='256' height='512' fill='#ff0000' opacity='0.5'/>");
    if (!RENDER(op) || A(400, 10) < 127 || A(400, 10) > 129 || A(100, 10) < 190 || A(100, 10) > 193 || R(100, 10) < 253) {
        fprintf(stderr, "svg selftest: opacities do not add up (%u %u)\n", A(400, 10), A(100, 10));
        bad++;
    }
    lm_img_free(&im);
    const char *grad = SVG("<defs><linearGradient id='g'><stop offset='0' stop-color='#000000'/><stop offset='1' stop-color='#ff0000'/></linearGradient></defs><rect width='512' height='100' fill='url(#g)'/>");
    if (!RENDER(grad) || R(0, 50) > 1 || R(511, 50) < 254 || R(256, 50) < 126 || R(256, 50) > 129 || G(256, 50) != 0) {
        fprintf(stderr, "svg selftest: a gradient is not linear\n");
        bad++;
    }
    lm_img_free(&im);
    const char *clip = SVG("<defs><clipPath id='c'><rect x='0' y='0' width='256' height='512'/></clipPath></defs><rect width='512' height='512' fill='green' clip-path='url(#c)'/>");
    if (!RENDER(clip) || A(255, 300) != 255 || A(256, 300) != 0) {
        fprintf(stderr, "svg selftest: a clip does not cut at its edge\n");
        bad++;
    }
    lm_img_free(&im);
    const char *tf = SVG("<g transform='translate(100 0) rotate(90)'><rect x='0' y='0' width='50' height='20' fill='black'/></g>");
    if (!RENDER(tf) || A(99, 10) != 255 || A(80, 10) != 255 || A(81, 49) != 255 || A(80, 50) != 0 || A(100, 10) != 0) {          /* x -> down, y -> left: a 20 wide, 50 tall bar */
        fprintf(stderr, "svg selftest: a rotated rectangle is in the wrong place\n");
        bad++;
    }
    lm_img_free(&im);
    const char *css = SVG("<style>.a{fill:#00ff00}#b{fill:#0000ff}rect{fill:#ff0000}</style><rect class='a' width='10' height='10'/><rect id='b' x='20' width='10' height='10'/><rect x='40' width='10' height='10'/>");
    if (!RENDER(css) || G(5, 5) != 255 || B(25, 5) != 255 || R(45, 5) != 255) {
        fprintf(stderr, "svg selftest: CSS rules do not apply by specificity\n");
        bad++;
    }
    lm_img_free(&im);
    const char *big = "<svg xmlns='http://www.w3.org/2000/svg' width='24' height='24'><rect width='24' height='24' fill='black'/></svg>";      /* a small icon is made about 512 pixels big */
    if (lm_svg_render((const uint8_t *)big, strlen(big), &im, err, sizeof(err)) != 0 || im.w < 500 || im.w > 520 || im.w != im.h) {
        fprintf(stderr, "svg selftest: a 24 pixel icon came out %dx%d\n", im.w, im.h);
        bad++;
    }
    lm_img_free(&im);
    const char *junk = "<svg><path d='M 10 10 L L L 1e999 1e999 A 0 0 0 0 0 5 5 C Z z z M'/><rect width='-5' height='3e9'/><circle r='1e30'/></svg>";
    if (lm_svg_render((const uint8_t *)junk, strlen(junk), &im, err, sizeof(err)) == 0)         /* nonsense in, a picture or an error out, no crash */
        lm_img_free(&im);
    return bad;
}

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
    bad += svg_selftest();
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

/* Picture number `frame` of a Motion JPEG or own H.264 file (our own readers: no ffmpeg). 0 = img is filled, -1 = err says why; the info line of the
 * file (`WxH codec=.. duration_ms=..`) goes to `info` when it is not NULL. */
static int own_video_frame(const char *in, int frame, struct lm_image *img, char *err, size_t errsz, char *info, size_t infosz)
{
    struct lm_mp4r *r = lm_mp4r_open(in, err, errsz);
    if (r && info)
        snprintf(info, infosz, "%dx%d codec=%s duration_ms=%llu", r->w, r->h, r->codec, (unsigned long long)r->duration_ms);
    int rc = -1;
    if (r && r->h264 && frame >= 0 && (uint32_t)frame < r->n) {          /* our own H.264: from the IDR picture before it */
        struct lm_h264dec *hd = lm_h264dec_create(r->sps, r->sps_n, r->pps, r->pps_n, err, errsz);
        uint32_t from = (uint32_t)frame;
        while (from > 0 && !r->key[from])
            from--;
        rc = hd ? 0 : -1;
        for (uint32_t k = from; hd && rc == 0 && k <= (uint32_t)frame; k++) {
            size_t len = 0;
            uint8_t *au = lm_mp4r_sample(r, k, &len);
            rc = au ? lm_h264dec_decode(hd, au, len, err, errsz) : (snprintf(err, errsz, "cannot read the picture"), -1);
            free(au);
        }
        if (rc == 0) {
            img->w = lm_h264dec_width(hd);
            img->h = lm_h264dec_height(hd);
            img->px = malloc((size_t)img->w * (size_t)img->h * 4);
            if (img->px)
                lm_h264dec_xrgb(hd, img->px, (size_t)img->w);
            else
                rc = (snprintf(err, errsz, "out of memory"), -1);
        }
        lm_h264dec_free(hd);
    } else {
        size_t len = 0;
        uint8_t *jpg = r && r->mjpeg && frame >= 0 && (uint32_t)frame < r->n ? lm_mp4r_sample(r, (uint32_t)frame, &len) : NULL;
        rc = !r ? -1 : !jpg ? (snprintf(err, errsz, "no such picture, or not Motion JPEG or H.264"), -1) : lm_jpeg_decode(jpg, len, img, err, errsz);
        free(jpg);
    }
    lm_mp4r_close(r);
    return rc;
}

static bool write_ppm(const char *out, const struct lm_image *img)
{
    FILE *f = fopen(out, "wb");
    if (!f)
        return false;
    fprintf(f, "P6\n%d %d\n255\n", img->w, img->h);
    for (size_t i = 0; i < (size_t)img->w * (size_t)img->h; i++) {
        uint8_t rgb[3] = { (uint8_t)(img->px[i] >> 16), (uint8_t)(img->px[i] >> 8), (uint8_t)img->px[i] };
        fwrite(rgb, 1, 3, f);
    }
    return fclose(f) == 0;
}

/* `--poster FILE OUT.ppm`: one picture of a video, for the file manager's preview, and its info line on stdout ("WxH codec=.. duration_ms=.. [audio=1]").
 * The picture is from an eighth of the way in (not the first, which is often black): our own readers for Motion JPEG and own H.264, else ffmpeg
 * (scaled down to at most 480 wide). Exit status 1 with a message on stderr when nothing could be got. */
static int poster_cmd(const char *in, const char *out)
{
    struct lm_image img = { 0 };
    char err[160] = "", info[200] = "";
    struct lm_mp4r *r = lm_mp4r_open(in, err, sizeof(err));
    int frame = r && r->n > 8 ? (int)(r->n / 8) : 0;
    lm_mp4r_close(r);
    if (own_video_frame(in, frame, &img, err, sizeof(err), info, sizeof(info)) == 0) {
        bool ok = write_ppm(out, &img);
        lm_img_free(&img);
        if (ok)
            puts(info);
        return ok ? 0 : 1;
    }
    struct ffdec_info fi;
    char ferr[200] = "";
    if (!ffpipe_ffmpeg_path() || !ffdec_probe(in, &fi, ferr, sizeof(ferr))) {
        fprintf(stderr, "mellagui-view: %s\n", ffpipe_ffmpeg_path() ? ferr : "no preview: the codec needs ffmpeg, which is not installed");
        return 1;
    }
    int tw = LM_MIN(fi.w, 480) & ~1, th = (int)((int64_t)fi.h * tw / LM_MAX(fi.w, 1)) & ~1;
    if (tw < 2 || th < 2) {
        fprintf(stderr, "mellagui-view: the video has no size\n");
        return 1;
    }
    uint64_t start = fi.duration_ms > 3000 ? fi.duration_ms / 8 : 0;
    struct ffdec *d = ffdec_open(in, tw, th, fi.w, fi.h, 1, start, ferr, sizeof(ferr));
    if (!d) {
        fprintf(stderr, "mellagui-view: %s\n", ferr);
        return 1;
    }
    uint32_t *buf = malloc((size_t)tw * (size_t)th * 4);
    size_t have = 0;
    int rc = 0;
    for (int tries = 0; buf && tries < 100 && rc == 0; tries++) {              /* up to ~5 s */
        rc = ffdec_read(d, buf, &have);
        if (rc == 0)
            ffdec_wait(d, 50);
    }
    ffdec_close(d);
    if (!buf || rc != 1) {
        free(buf);
        fprintf(stderr, "mellagui-view: ffmpeg gave no picture\n");
        return 1;
    }
    img.w = tw;
    img.h = th;
    img.px = buf;
    bool ok = write_ppm(out, &img);
    free(buf);
    if (ok)
        printf("%dx%d codec=%s duration_ms=%llu audio=%d\n", fi.w, fi.h, fi.codec, (unsigned long long)fi.duration_ms, fi.has_audio);
    return ok ? 0 : 1;
}

static int decode_cmd(const char *in, const char *out, int frame)
{
    struct lm_image img = { 0 };
    char err[160];
    if (is_video_name(in)) {
        if (own_video_frame(in, frame, &img, err, sizeof(err), NULL, 0) < 0) {
            fprintf(stderr, "mellagui-view: %s\n", err);
            return 1;
        }
    } else if (lm_img_load(in, &img, err, sizeof(err)) < 0) {
        fprintf(stderr, "mellagui-view: %s\n", err);
        return 1;
    }
    bool ok = write_ppm(out, &img);
    lm_img_free(&img);
    return ok ? 0 : 1;
}

/* ---- idle inhibitor: the screen must not go to sleep or lock while a video plays ----------------------------------------- */

/* ---- one window per Wayland display ----------------------------------------------------------------------------------------------------------- */

/* The name of this display's viewer socket (an ABSTRACT unix socket: no file to clean up, a killed viewer leaves nothing, and the name has no length limit
 * problem; it is made of a hash of the runtime directory and the display, so the displays of the tests and of a real session never meet) and the lock file;
 * false = no single window ($MELLAGUI_VIEW_SINGLE=0, no runtime directory). */
static bool ipc_names(struct sockaddr_un *sa, socklen_t *len, char *lock, size_t ln)
{
    const char *rt = getenv("XDG_RUNTIME_DIR"), *wd = getenv("WAYLAND_DISPLAY"), *off = getenv("MELLAGUI_VIEW_SINGLE");
    if (!rt || !*rt || (off && !strcmp(off, "0")))
        return false;
    uint64_t h = 1469598103934665603ull;                        /* FNV-1a */
    for (const char *p = rt; *p; p++)
        h = (h ^ (uint8_t)*p) * 1099511628211ull;
    h = (h ^ '|') * 1099511628211ull;
    for (const char *p = wd && *wd ? wd : "default"; *p; p++)
        h = (h ^ (uint8_t)*p) * 1099511628211ull;
    char name[48];
    int n = snprintf(name, sizeof(name), "mellagui-view-%016llx", (unsigned long long)h);
    memset(sa, 0, sizeof(*sa));
    sa->sun_family = AF_UNIX;
    memcpy(sa->sun_path + 1, name, (size_t)n);                   /* sun_path[0] = 0: the abstract namespace */
    *len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + (size_t)n);
    int w = snprintf(lock, ln, "%s/mellagui-view-%016llx.lock", rt, (unsigned long long)h);
    return w > 0 && (size_t)w < ln;
}

/* Called before anything else is made. 1 = another viewer of this display has the path now (it showed it and brought its window forward): end. 0 = this one
 * is the viewer (a->ipc is the socket the later starts will use, or -1 when there is none). */
static int single_start(struct app *a, const char *target)
{
    struct sockaddr_un sa;
    socklen_t salen;
    char lock[PATH_MAX];
    a->ipc = -1;
    if (!ipc_names(&sa, &salen, lock, sizeof(lock)))
        return 0;
    int lk = open(lock, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lk >= 0)
        flock(lk, LOCK_EX);                                     /* (two starts at the same time: one becomes the viewer, the other hands over) */
    int rc = 0;
    int c = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (c >= 0 && connect(c, (struct sockaddr *)&sa, salen) == 0) {
        char abs[PATH_MAX], msg[PATH_MAX + 16];
        snprintf(msg, sizeof(msg), "open\t%s\n", realpath(target, abs) ? abs : target);
        struct pollfd pf = { .fd = c, .events = POLLIN };
        char reply[8];
        if (write(c, msg, strlen(msg)) == (ssize_t)strlen(msg) && poll(&pf, 1, 2000) > 0 && read(c, reply, sizeof(reply)) > 0)
            rc = 1;                                             /* (a viewer that does not answer is left alone: this one runs without a socket) */
    } else {
        int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (s >= 0 && bind(s, (struct sockaddr *)&sa, salen) == 0 && listen(s, 8) == 0)
            a->ipc = s;
        else if (s >= 0)
            close(s);
    }
    if (c >= 0)
        close(c);
    if (lk >= 0) {
        flock(lk, LOCK_UN);
        close(lk);
    }
    return rc;
}

/* Another start handed over a path: show it and come forward. */
static void ipc_serve(struct app *a)
{
    int c = accept4(a->ipc, NULL, NULL, SOCK_CLOEXEC);
    if (c < 0)
        return;
    char buf[PATH_MAX + 16];
    struct pollfd pf = { .fd = c, .events = POLLIN };
    ssize_t n = poll(&pf, 1, 300) > 0 ? read(c, buf, sizeof(buf) - 1) : 0;
    if (n > 5) {
        buf[n] = 0;
        char *nl = strchr(buf, '\n');
        if (nl)
            *nl = 0;
        if (!strncmp(buf, "open\t", 5) && buf[5]) {
            a->card = false;
            set_target(a, buf + 5, true);
            wlwin_raise(&a->win);
            if (write(c, "ok\n", 3) < 0)
                n = 0;
        }
    }
    close(c);
}

static void global_event(struct wlwin *w, uint32_t name, const char *iface, uint32_t ver)
{
    struct app *a = w->data;
    wlclip_global(&a->clip, w, name, iface, ver);
    if (!strcmp(iface, "zwp_idle_inhibit_manager_v1"))
        a->idle_mgr = wlc_bind(w->c, w->registry, name, &lm_zwp_idle_inhibit_manager_v1_interface, 1, NULL, a);
}

static void sync_inhibitor(struct app *a)
{
    if (!a->idle_mgr)
        return;
    if (a->playing && a->vmode != 4 && !a->inhibitor) {                  /* (music does not keep the screen awake: a video does) */
        a->inhibitor = wlc_alloc(a->win.c, &lm_zwp_idle_inhibitor_v1_interface, NULL, a);
        WLC_SEND(a->win.c, a->idle_mgr, LM_ZWP_IDLE_INHIBIT_MANAGER_V1_REQ_CREATE_INHIBITOR, { .n = a->inhibitor }, { .o = a->win.surface });
    } else if ((!a->playing || a->vmode == 4) && a->inhibitor) {
        WLC_SEND0(a->win.c, a->inhibitor, LM_ZWP_IDLE_INHIBITOR_V1_REQ_DESTROY);
        wlc_forget(a->win.c, a->inhibitor);
        a->inhibitor = 0;
    }
}

int main(int argc, char **argv)
{
    wlwin_hidpi();                      /* draws through the wlwin_* calls only: sharp on a scaled screen */
    lm_theme_load_env();
    if (argc > 1 && !strcmp(argv[1], "--selftest"))
        return selftest();
    if (argc > 3 && !strcmp(argv[1], "--decode"))
        return decode_cmd(argv[2], argv[3], argc > 4 ? atoi(argv[4]) : 0);
    if (argc > 2 && !strcmp(argv[1], "--video-info"))
        return video_info_cmd(argv[2]);
    if (argc > 3 && !strcmp(argv[1], "--poster"))
        return poster_cmd(argv[2], argv[3]);

    struct app a = { 0 };
    a.ipc = -1;
    a.th_hover = -1;
    a.c_hover = a.c_down = -1;
    const char *arg = argc > 1 ? argv[1] : NULL;
    char tmp[PATH_MAX];
    if (!arg) {
        const char *home = getenv("HOME");
        snprintf(tmp, sizeof(tmp), "%s/Pictures", home && *home ? home : ".");
        struct stat st;
        arg = stat(tmp, &st) == 0 && S_ISDIR(st.st_mode) ? tmp : ".";
    }
    if (single_start(&a, arg) == 1)                             /* another viewer has it (and is in front now) */
        return 0;
    conf_load_thumbs(&a);
    const char *dumpf = getenv("MELLAGUI_VIEW_DUMP");
    if (dumpf && *dumpf)
        snprintf(a.dump, sizeof(a.dump), "%s", dumpf);
    wlpicker_init(&a.pk, card_want);
    set_target(&a, arg, false);

    static const struct wlwin_ops ops = { .paint = paint, .global = global_event, .pointer = pointer_event, .keyboard = keyboard_event };
    if (wlwin_connect(&a.win, "mellagui-view", &ops, &a) < 0)
        return 1;
    if (wlwin_open(&a.win, "Images", "mellagui.view", 860, 520, 240, 160) < 0)
        return 1;
    wlclip_attach(&a.clip, &a.win);
    load_current(&a);
    while (a.win.running) {
        if (!wlwin_step(&a.win, 0))
            break;
        video_tick(&a);
        if (a.next_track) {                                     /* a track is over: the next sound file of the folder (the player stops after the last) */
            a.next_track = false;
            for (int i = a.cur + 1; i < a.n; i++)
                if (is_audio_name(a.names[i])) {
                    go(&a, i);
                    break;
                }
        }
        sync_inhibitor(&a);
        int timeout = video_timeout(&a);
        if (th_lazy(&a))                                         /* the small pictures of the strip are made one by one between the events */
            timeout = timeout < 0 ? 2 : LM_MIN(timeout, 2);
        if (poster_step(&a))                                     /* a video's poster is being made by a child */
            timeout = timeout < 0 ? 50 : LM_MIN(timeout, 50);
        if (a.win.redraw && a.win.configured && !a.win.buffer_busy)
            timeout = 0;
        struct pollfd pf[2] = { { .fd = wlc_fd(a.win.c), .events = POLLIN }, { .fd = a.ipc, .events = POLLIN } };
        int pn = poll(pf, a.ipc >= 0 ? 2 : 1, timeout);
        if (pn > 0 && a.ipc >= 0 && (pf[1].revents & POLLIN))
            ipc_serve(&a);
    }
    if (a.ipc >= 0)
        close(a.ipc);
    wlwin_destroy(&a.win);
    lm_img_free(&a.cover);
    lm_mp4r_close(a.vid);
    lm_h264dec_free(a.hd);
    audio_kill(&a);
    ffdec_close(a.fd);
    free(a.fbuf);
    lm_img_free(&a.img);
    free(a.scaled);
    th_reset(&a);
    free(a.thm);
    free_names(&a);
    wlpicker_free(&a.pk);
    return 0;
}
