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
#include "ffaudio.h"
#include "ffdec.h"
#include "ffpipe.h"
#include "h264dec.h"
#include "img.h"
#include "keymap.h"
#include "loop.h"
#include "metrics.h"
#include "mp4read.h"
#include "png.h"
#include "protocol.h"
#include "util.h"
#include "wlwin.h"

#define BAR_H 24
#define BG (lm_theme.card)
#define BAR_BG 0x0e1118u
#define BAR_FG 0xd8dde8u
#define VBTN_W 26                       /* the video buttons in the bar: back 5 s, play / pause, stop, on 5 s */
#define VBTN_H 18
#define VBTN_GAP 2
#define N_VBTN 4
#define AUDIO_LEAD_MS 190               /* the sound needs this long from the start of its process to the speaker (ffmpeg ~90 ms, the player ~100 ms): it starts this far ahead */
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
    int vhover, vpress;                 /* the video button under the pointer / pressed: 0 none, else index + 1 */
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
    double z = LM_MIN((double)view_w(a) / a->tw, (double)view_h(a) / a->th);
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

static void audio_lead_save(const struct app *a)
{
    char path[PATH_MAX], dir[PATH_MAX];
    if (!lead_conf_path(path, sizeof(path)))
        return;
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
    fprintf(f, "# mellagui-view: how many ms ahead of the picture the sound starts ([ = earlier, ] = later)\naudio-lead-ms = %d\n", a->aud_lead);
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
    int top = view_h(a) + 5;
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
        uint32_t bg = a->vpress == i + 1 && a->vhover == i + 1 ? 0x3d4a6a : a->vhover == i + 1 ? 0x2d3a56 : 0x1d2433;
        uint32_t fg = i == 1 && !a->playing ? lm_theme.accent : BAR_FG;
        wlwin_fill(w, x, y, VBTN_W, VBTN_H, bg);
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
    audio_reset(a);
    a->has_audio = false;
    a->aud_dead = false;
    a->vmode = 0;
    a->playing = false;
    a->vframe = 0;
    a->pos_ms = 0;
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
    int tx = 8;
    if (a->vmode) {
        paint_vbuttons(a, w, vh);
        tx = vbtn_x(N_VBTN) + 4;
    }
    wlwin_text(w, tx, vh + (BAR_H - LM_FONT_H) / 2 + 2, status, 1, BAR_FG, w->bw - tx - 8);
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
        a->vhover = a->vpress = 0;
        w->redraw = true;
        a->dragging = false;
        break;
    case LM_WL_POINTER_EVT_MOTION: {
        double x = lm_fixed_to_double(args[1].f), y = lm_fixed_to_double(args[2].f);
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
        if (args[2].u != 272)                           /* BTN_LEFT */
            break;
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
        if (args[3].u == 1 && a->vmode && a->py >= view_h(a)) {  /* a click on the bar of a video seeks */
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
    } else if (a->vmode && a->has_audio && (c == '[' || c == ']')) {      /* the sound comes earlier / later: 50 ms a step */
        a->aud_lead = LM_CLAMP(audio_lead(a) + (c == '[' ? 50 : -50), -2000, 2000);
        audio_lead_save(a);
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

/* ---- idle inhibitor: the screen must not go to sleep or lock while a video plays ----------------------------------------- */

static void global_event(struct wlwin *w, uint32_t name, const char *iface, uint32_t ver)
{
    (void)ver;
    struct app *a = w->data;
    if (!strcmp(iface, "zwp_idle_inhibit_manager_v1"))
        a->idle_mgr = wlc_bind(w->c, w->registry, name, &lm_zwp_idle_inhibit_manager_v1_interface, 1, NULL, a);
}

static void sync_inhibitor(struct app *a)
{
    if (!a->idle_mgr)
        return;
    if (a->playing && !a->inhibitor) {
        a->inhibitor = wlc_alloc(a->win.c, &lm_zwp_idle_inhibitor_v1_interface, NULL, a);
        WLC_SEND(a->win.c, a->idle_mgr, LM_ZWP_IDLE_INHIBIT_MANAGER_V1_REQ_CREATE_INHIBITOR, { .n = a->inhibitor }, { .o = a->win.surface });
    } else if (!a->playing && a->inhibitor) {
        WLC_SEND0(a->win.c, a->inhibitor, LM_ZWP_IDLE_INHIBITOR_V1_REQ_DESTROY);
        wlc_forget(a->win.c, a->inhibitor);
        a->inhibitor = 0;
    }
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

    static const struct wlwin_ops ops = { .paint = paint, .global = global_event, .pointer = pointer_event, .keyboard = keyboard_event };
    if (wlwin_connect(&a.win, "mellagui-view", &ops, &a) < 0)
        return 1;
    if (wlwin_open(&a.win, "Images", "mellagui.view", 860, 520, 240, 160) < 0)
        return 1;
    load_current(&a);
    while (wlwin_step(&a.win, video_timeout(&a))) {
        video_tick(&a);
        sync_inhibitor(&a);
    }
    wlwin_destroy(&a.win);
    lm_mp4r_close(a.vid);
    lm_h264dec_free(a.hd);
    audio_kill(&a);
    ffdec_close(a.fd);
    free(a.fbuf);
    lm_img_free(&a.img);
    free(a.scaled);
    free_names(&a);
    return 0;
}
