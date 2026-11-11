/*
 * mellagui-record: screen recorder. Records one output into an MP4 file.
 *
 *   mellagui-record [--start] [--duration SECONDS] [--output N] [--fps N] [--quality 1..100] [--dir DIR] [--name FILE]
 *                   [--codec auto|h264|ffmpeg|mjpeg]
 *   mellagui-record --list           the outputs (index, name, size) and exit
 *   mellagui-record --selftest       encoders and MP4 writer without a display
 *
 * Codecs: `ffmpeg` = H.264 by x264 through a pipe to ffmpeg (src: client/ffpipe.c; best compression, needs ffmpeg with libx264), `h264` =
 * H.264 by mellagui's own encoder (src/h264enc.c, constrained baseline, no ffmpeg needed, ~35 times smaller than Motion JPEG), `mjpeg` =
 * Motion JPEG by src/jpegenc.c (big files, simplest). `auto` (default) takes ffmpeg when it is installed, else our own H.264. $MELLAGUI_FFMPEG
 * names another ffmpeg ("none" = pretend there is none).
 *
 * The pictures come from the compositor's private mellagui_capture_v1 global (src/capture.h; only MellaGUI's own programs get
 * it): the main thread asks for one at a time into a small ring of shared buffers and a picture that is identical to the one before is
 * dropped (a still screen costs nothing, the earlier picture just lasts longer). The others go to the encoder: Motion JPEG on up to 4 threads
 * (src/jpegenc.c, written to src/mp4.c in order), own H.264 on one thread (src/h264enc.c: a picture is predicted from the one before),
 * ffmpeg through a pipe, stamped with the time it is read. When the encoder is slower than the frame rate pictures are skipped, never queued
 * up. The cursor is not in the picture (as in screenshots), and neither is sound.
 *
 * Window: Record / Stop, the output and the frame rate to use (not while recording). Keys: Space or Enter record / stop,
 * O next output, F next frame rate, Esc or Q close (a recording is finished first). SIGINT / SIGTERM also finish the file.
 * Files: DIR/mellagui-YYYYMMDD-HHMMSS.mp4 (mode 0600), DIR = --dir, $MELLAGUI_RECORD_DIR or ~/Videos. When it is done a
 * notification with the path is shown.
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "ffpipe.h"
#include "h264dec.h"
#include "h264enc.h"
#include "jpegenc.h"
#include "keymap.h"
#include "loop.h"
#include "mp4.h"
#include "theme.h"
#include "uikit.h"
#include "util.h"
#include "wlwin.h"

#define MAX_OUTPUTS 8
#define MAX_WORKERS 4                   /* encoder threads: one fewer than the processors, at most this many */
#define MAX_FRAMES (MAX_WORKERS + 3)    /* shared pictures: one per worker, one being captured, two waiting */
#define DEFAULT_FPS 15
#define DEFAULT_QUALITY 80

static const int FPS_CHOICES[] = { 5, 10, 15, 24, 30 };
#define N_FPS ((int)(sizeof(FPS_CHOICES) / sizeof(FPS_CHOICES[0])))

enum frame_state { F_FREE, F_GRAB, F_QUEUED, F_ENCODING };

struct frame {
    uint32_t buf;                   /* wl_buffer */
    uint32_t *px;                   /* its pixels, shared with the compositor */
    enum frame_state state;         /* under app.lock */
    uint64_t pts;                   /* ms since the start of the recording */
    uint64_t seq;                   /* the order the pictures must be written in */
};

struct output {
    char name[32];
    int x, y, w, h;
};

struct button {
    int x, y, w, h;
};

struct app {
    struct wlwin win;
    uint32_t capture, settings, tray;
    struct output outs[MAX_OUTPUTS];
    int nouts;
    bool listed;

    /* settings of the recording */
    int out_sel, fps_sel, quality;
    int codec_opt;                  /* --codec: 0 auto, 1 mjpeg (our own encoder), 2 ffmpeg (H.264 through libx264), 3 h264 (our own encoder) */
    const char *dir, *name;         /* --dir, --name */
    double duration;                /* --duration: stop (and quit) after that many seconds, 0 = never */

    /* the recording */
    bool rec;
    int rw, rh;                     /* size of the recorded output */
    int rout;                       /* output index being recorded */
    struct lm_mp4 *mp4;             /* Motion JPEG: our own file writer */
    struct ffpipe *ff;              /* H.264: ffmpeg writes the file (NULL when mp4 is used) */
    struct lm_h264enc *h264;        /* H.264 by our own encoder, into mp4 (NULL otherwise) */
    uint8_t *ff_last;               /* the last picture sent, sent once more when the recording ends (that defines its end) */
    char path[600];
    uint64_t t0, next_grab, interval;
    struct frame fr[MAX_FRAMES];
    int nframes;
    void *map;
    size_t map_size;
    uint32_t pool;
    int grabbing;                   /* index of the frame the compositor is painting, -1 none */
    uint64_t last_hash;
    bool have_hash;
    uint32_t n_grab, n_dup, n_fail;
    char fail_reason[24];

    /* the encoder thread */
    pthread_t threads[MAX_WORKERS];
    int nthreads;
    uint64_t seq_next, seq_write;   /* pictures queued so far / written so far (under lock) */
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int queue[MAX_FRAMES], qn;
    bool stop_thread;
    bool enc_error;
    uint32_t enc_frames;            /* what the encoder has written so far (under lock), for the window */
    uint64_t enc_bytes;

    /* the item in the top bar (mellagui_tray_v1): a blinking dot and the elapsed time while recording */
    uint32_t tray_item, tray_pool, tray_buf[2];
    int tray_size, tray_phase;
    uint64_t tray_sec;
    void *tray_map;
    size_t tray_map_size;
    bool drawn;                     /* the window has had its first picture */
    bool want_min;                  /* minimize the window as soon as it is on the screen */
    bool tray_clicked;              /* the bar's item was clicked: stop */
    bool hidden;                    /* we minimized it: a click on the bar's item then ends the program too */

    /* what the window shows */
    char saved[600];                /* the last finished file */
    char msg[160];                  /* the last problem */
    uint64_t shown_sec;
    struct button b_rec, b_out, b_fps;
    int hover, press;               /* 0 none, 1 record, 2 output, 3 fps */
    double px, py;
    bool has_pointer;
    bool quit;
};

static volatile sig_atomic_t g_signalled;

static void on_signal(int sig)
{
    (void)sig;
    g_signalled = 1;
}

/* ---- helpers ------------------------------------------------------------------------------------------------------- */

static uint64_t hash_frame(const uint32_t *px, size_t n)
{
    uint64_t h = 0x9e3779b97f4a7c15ull;
    const uint64_t *q = (const uint64_t *)(const void *)px;
    size_t words = n / 2;
    for (size_t i = 0; i < words; i++) {
        h ^= q[i];
        h *= 0xff51afd7ed558ccdull;
        h ^= h >> 32;
    }
    if (n & 1) {
        h ^= px[n - 1];
        h *= 0xff51afd7ed558ccdull;
    }
    return h;
}

static void set_msg(struct app *a, const char *fmt, ...) LM_PRINTF(2, 3);
static void set_msg(struct app *a, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(a->msg, sizeof(a->msg), fmt, ap);
    va_end(ap);
    fprintf(stderr, "mellagui-record: %s\n", a->msg);
    a->win.redraw = true;
}

static void notify(struct app *a, const char *title, const char *body)
{
    if (!a->settings || !a->win.c)
        return;
    char msg[800];
    snprintf(msg, sizeof(msg), "%s\t%s\t8000", title, body);
    WLC_SEND(a->win.c, a->settings, LM_MELLAGUI_SETTINGS_V1_REQ_SET, { .s = "notify" }, { .s = msg });
}

static const char *record_dir(const struct app *a, char *buf, size_t n)
{
    if (a->dir && *a->dir)
        return a->dir;
    const char *e = getenv("MELLAGUI_RECORD_DIR"), *home = getenv("HOME");
    if (e && *e)
        return e;
    if (home && *home) {
        snprintf(buf, n, "%s/Videos", home);
        return buf;
    }
    return ".";
}

/* Picks and creates the next free file (mode 0600, never an existing one): a Motion JPEG writer in *mp4 when `native`, else just an empty
 * file for ffmpeg to fill. false with a message in a->msg. */
static bool create_file(struct app *a, int w, int h, bool native, struct lm_mp4 **mp4)
{
    char dirbuf[512];
    const char *dir = record_dir(a, dirbuf, sizeof(dirbuf));
    if (mkdir(dir, 0755) < 0 && errno != EEXIST) {
        set_msg(a, "cannot create %s: %s", dir, strerror(errno));
        return false;
    }
    char stamp[32];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
    for (int n = 0; n < 100; n++) {
        if (a->name && *a->name)
            snprintf(a->path, sizeof(a->path), "%s%s%s", a->name[0] == '/' ? "" : dir, a->name[0] == '/' ? "" : "/", a->name);
        else if (n == 0)
            snprintf(a->path, sizeof(a->path), "%s/mellagui-%s.mp4", dir, stamp);
        else
            snprintf(a->path, sizeof(a->path), "%s/mellagui-%s-%d.mp4", dir, stamp, n);
        bool ok;
        if (native) {
            *mp4 = lm_mp4_create(a->path, w, h);
            ok = *mp4 != NULL;
        } else {
            int fd = open(a->path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
            ok = fd >= 0;
            if (ok)
                close(fd);
        }
        if (ok)
            return true;
        if (errno != EEXIST || (a->name && *a->name)) {
            set_msg(a, "cannot write %s: %s", a->path, strerror(errno));
            return false;
        }
    }
    set_msg(a, "no free file name in %s", dir);
    return false;
}

/* ---- encoder thread -------------------------------------------------------------------------------------------------- */

static void *encoder_main(void *arg)
{
    struct app *a = arg;
    pthread_mutex_lock(&a->lock);
    for (;;) {
        while (a->qn == 0 && !a->stop_thread)
            pthread_cond_wait(&a->cond, &a->lock);
        if (a->qn == 0)
            break;                                  /* asked to stop and nothing left */
        int i = a->queue[0];
        memmove(a->queue, a->queue + 1, (size_t)(a->qn - 1) * sizeof(int));
        a->qn--;
        struct frame *f = &a->fr[i];
        f->state = F_ENCODING;
        pthread_mutex_unlock(&a->lock);

        size_t len = 0;
        uint8_t *jpeg = lm_jpeg_encode_xrgb(f->px, a->rw, a->rh, (size_t)a->rw * 4, a->quality, &len);

        pthread_mutex_lock(&a->lock);                   /* the pictures go into the file in the order they were taken */
        while (a->seq_write != f->seq)
            pthread_cond_wait(&a->cond, &a->lock);
        pthread_mutex_unlock(&a->lock);
        bool bad = !jpeg || lm_mp4_add(a->mp4, jpeg, len, f->pts) < 0;      /* (only the thread whose turn it is gets here) */
        free(jpeg);

        pthread_mutex_lock(&a->lock);
        if (bad) {
            a->enc_error = true;
        } else {
            a->enc_frames++;
            a->enc_bytes += len;
        }
        a->seq_write++;
        pthread_cond_broadcast(&a->cond);
        f->state = F_FREE;
    }
    pthread_mutex_unlock(&a->lock);
    return NULL;
}

/* The H.264 way: one thread hands the changed pictures to ffmpeg in the order they were taken. ffmpeg stamps each with the time it reads it,
 * so this must not wait for anything; when the recording ends the last picture goes once more, which sets the end of the video. */
static void *ff_writer_main(void *arg)
{
    struct app *a = arg;
    size_t bytes = (size_t)a->rw * (size_t)a->rh * 4;
    bool have_last = false;
    pthread_mutex_lock(&a->lock);
    for (;;) {
        while (a->qn == 0 && !a->stop_thread)
            pthread_cond_wait(&a->cond, &a->lock);
        if (a->qn == 0)
            break;
        int i = a->queue[0];
        memmove(a->queue, a->queue + 1, (size_t)(a->qn - 1) * sizeof(int));
        a->qn--;
        struct frame *f = &a->fr[i];
        f->state = F_ENCODING;
        pthread_mutex_unlock(&a->lock);

        bool bad = ffpipe_write(a->ff, f->px, bytes) < 0;
        if (!bad && a->ff_last) {
            memcpy(a->ff_last, f->px, bytes);
            have_last = true;
        }

        pthread_mutex_lock(&a->lock);
        if (bad) {
            a->enc_error = true;
        } else {
            a->enc_frames++;
        }
        f->state = F_FREE;
    }
    pthread_mutex_unlock(&a->lock);
    if (have_last && !a->enc_error && ffpipe_write(a->ff, a->ff_last, bytes) < 0)
        a->enc_error = true;
    return NULL;
}

/* H.264 by our own encoder: one thread (a picture is predicted from the one before), the changed pictures in the order they were taken; an
 * IDR picture every five seconds is a place to start playing at. */
static void *h264_writer_main(void *arg)
{
    struct app *a = arg;
    uint64_t last_idr = 0;
    bool first = true;
    pthread_mutex_lock(&a->lock);
    for (;;) {
        while (a->qn == 0 && !a->stop_thread)
            pthread_cond_wait(&a->cond, &a->lock);
        if (a->qn == 0)
            break;
        int i = a->queue[0];
        memmove(a->queue, a->queue + 1, (size_t)(a->qn - 1) * sizeof(int));
        a->qn--;
        struct frame *f = &a->fr[i];
        f->state = F_ENCODING;
        pthread_mutex_unlock(&a->lock);

        size_t len = 0;
        bool key = false, want_idr = first || f->pts - last_idr >= 5000;
        uint8_t *au = lm_h264enc_encode(a->h264, f->px, (size_t)a->rw * 4, want_idr, &len, &key);
        if (au && key) {
            last_idr = f->pts;
            first = false;
        }
        bool bad = !au || lm_mp4_add_sample(a->mp4, au, len, f->pts, key) < 0;
        free(au);

        pthread_mutex_lock(&a->lock);
        if (bad) {
            a->enc_error = true;
        } else {
            a->enc_frames++;
            a->enc_bytes += len;
        }
        f->state = F_FREE;
    }
    pthread_mutex_unlock(&a->lock);
    return NULL;
}

/* ---- recording ------------------------------------------------------------------------------------------------------- */

static void buffer_nothing(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)c; (void)id; (void)op; (void)args; (void)data;
}

static void free_frames(struct app *a)
{
    for (int i = 0; i < a->nframes; i++)
        if (a->fr[i].buf && a->win.c) {
            WLC_SEND0(a->win.c, a->fr[i].buf, LM_WL_BUFFER_REQ_DESTROY);
            wlc_forget(a->win.c, a->fr[i].buf);
        }
    memset(a->fr, 0, sizeof(a->fr));
    if (a->map)
        munmap(a->map, a->map_size);
    a->map = NULL;
}

/* ---- the bar's item ---------------------------------------------------------------------------------------------------- */

static void render_dot(uint32_t *px, int s, bool on)
{
    float c = (float)s / 2.0f, R = (float)s * 0.38f;
    for (int y = 0; y < s; y++)
        for (int x = 0; x < s; x++) {
            float d = hypotf((float)x + 0.5f - c, (float)y + 0.5f - c) - R;
            float cov = 0.5f - d;
            cov = cov < 0.0f ? 0.0f : cov > 1.0f ? 1.0f : cov;
            float a = cov * (on ? 1.0f : 0.28f);
            px[(size_t)y * (size_t)s + (size_t)x] = ((uint32_t)(a * 255.0f + 0.5f) << 24) | ((uint32_t)(a * 240.0f + 0.5f) << 16) |
                                                    ((uint32_t)(a * 50.0f + 0.5f) << 8) | (uint32_t)(a * 50.0f + 0.5f);
        }
}

static void tray_item_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)c; (void)id;
    struct app *a = data;
    if (op == LM_MELLAGUI_TRAY_ITEM_V1_EVT_SIZE && !a->tray_buf[0] && args[0].i >= 8 && args[0].i <= 64) {
        int s = a->tray_size = args[0].i;
        size_t one = (size_t)s * (size_t)s * 4;
        a->tray_map_size = one * 2;
        int fd = memfd_create("mellagui-record-icon", MFD_CLOEXEC);
        if (fd < 0 || ftruncate(fd, (off_t)a->tray_map_size) < 0) {
            if (fd >= 0)
                close(fd);
            return;
        }
        a->tray_map = mmap(NULL, a->tray_map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (a->tray_map == MAP_FAILED) {
            a->tray_map = NULL;
            close(fd);
            return;
        }
        render_dot((uint32_t *)a->tray_map, s, true);
        render_dot((uint32_t *)(void *)((uint8_t *)a->tray_map + one), s, false);
        a->tray_pool = wlc_alloc(a->win.c, &lm_wl_shm_pool_interface, NULL, NULL);
        WLC_SEND(a->win.c, a->win.shm, LM_WL_SHM_REQ_CREATE_POOL, { .n = a->tray_pool }, { .h = fd }, { .i = (int32_t)a->tray_map_size });
        close(fd);
        for (int i = 0; i < 2; i++) {
            a->tray_buf[i] = wlc_alloc(a->win.c, &lm_wl_buffer_interface, buffer_nothing, NULL);
            WLC_SEND(a->win.c, a->tray_pool, LM_WL_SHM_POOL_REQ_CREATE_BUFFER, { .n = a->tray_buf[i] }, { .i = (int32_t)(one * (size_t)i) },
                     { .i = s }, { .i = s }, { .i = s * 4 }, { .u = LM_WL_SHM_FORMAT_ARGB8888 });
        }
        WLC_SEND0(a->win.c, a->tray_pool, LM_WL_SHM_POOL_REQ_DESTROY);
        wlc_forget(a->win.c, a->tray_pool);
        a->tray_pool = 0;
        a->tray_phase = -1;                         /* the next tick sets the icon */
    } else if (op == LM_MELLAGUI_TRAY_ITEM_V1_EVT_CLICKED) {
        a->tray_clicked = true;
    }
}

static void tray_start(struct app *a)
{
    if (!a->tray || a->tray_item)
        return;
    a->tray_item = wlc_alloc(a->win.c, &lm_mellagui_tray_item_v1_interface, tray_item_event, a);
    WLC_SEND(a->win.c, a->tray, LM_MELLAGUI_TRAY_V1_REQ_CREATE_ITEM, { .n = a->tray_item });
    a->tray_phase = -1;
    a->tray_sec = UINT64_MAX;
}

static void tray_stop(struct app *a)
{
    if (!a->tray_item)
        return;
    WLC_SEND0(a->win.c, a->tray_item, LM_MELLAGUI_TRAY_ITEM_V1_REQ_DESTROY);
    wlc_forget(a->win.c, a->tray_item);
    a->tray_item = 0;
    for (int i = 0; i < 2; i++)
        if (a->tray_buf[i]) {
            WLC_SEND0(a->win.c, a->tray_buf[i], LM_WL_BUFFER_REQ_DESTROY);
            wlc_forget(a->win.c, a->tray_buf[i]);
            a->tray_buf[i] = 0;
        }
    if (a->tray_map)
        munmap(a->tray_map, a->tray_map_size);
    a->tray_map = NULL;
}

/* The dot blinks twice a second, the label counts the seconds. */
static void tray_update(struct app *a, uint64_t elapsed)
{
    if (!a->tray_item)
        return;
    if (a->tray_buf[0]) {
        int ph = (int)((elapsed / 500) % 2);
        if (ph != a->tray_phase) {
            a->tray_phase = ph;
            WLC_SEND(a->win.c, a->tray_item, LM_MELLAGUI_TRAY_ITEM_V1_REQ_SET_ICON, { .o = a->tray_buf[ph] });
        }
    }
    if (elapsed / 1000 != a->tray_sec) {
        a->tray_sec = elapsed / 1000;
        char t[16];
        snprintf(t, sizeof(t), "%02u:%02u", (unsigned)(a->tray_sec / 60), (unsigned)(a->tray_sec % 60));
        WLC_SEND(a->win.c, a->tray_item, LM_MELLAGUI_TRAY_ITEM_V1_REQ_SET_TEXT, { .s = t });
    }
}

static bool start_recording(struct app *a)
{
    if (a->rec)
        return true;
    if (!a->capture) {
        set_msg(a, "the compositor does not offer screen capture to this program");
        return false;
    }
    if (a->out_sel < 0 || a->out_sel >= a->nouts) {
        set_msg(a, "no such output");
        return false;
    }
    const struct output *o = &a->outs[a->out_sel];
    a->rw = o->w;
    a->rh = o->h;
    a->rout = a->out_sel;
    a->msg[0] = 0;
    a->saved[0] = 0;
    a->fail_reason[0] = 0;

    size_t frame_bytes = (size_t)a->rw * (size_t)a->rh * 4;
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    a->nthreads = 0;
    bool use_ff = a->codec_opt == 2 || (a->codec_opt == 0 && ffpipe_available());
    bool use_h264 = a->codec_opt == 3 || (a->codec_opt == 0 && !use_ff);        /* auto: x264 through ffmpeg when it is there, else our own H.264 */
    if (use_ff && !ffpipe_available()) {
        set_msg(a, "ffmpeg with libx264 is not installed (--codec h264 and --codec mjpeg work without it)");
        return false;
    }
    int workers = use_ff || use_h264 ? 1 : (int)(cpus < 2 ? 1 : cpus - 1 > MAX_WORKERS ? MAX_WORKERS : cpus - 1);
    a->nframes = workers + 3;
    a->map_size = frame_bytes * (size_t)a->nframes;
    int fd = memfd_create("mellagui-record", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)a->map_size) < 0) {
        set_msg(a, "memfd: %s", strerror(errno));
        if (fd >= 0)
            close(fd);
        return false;
    }
    a->map = mmap(NULL, a->map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (a->map == MAP_FAILED) {
        a->map = NULL;
        close(fd);
        set_msg(a, "mmap: %s", strerror(errno));
        return false;
    }
    int cw = a->rw, ch = a->rh;                     /* the size of the picture in the file */
    if (use_h264) {
        a->h264 = lm_h264enc_create(a->rw, a->rh, LM_CLAMP((int)(46 - a->quality * 0.25 + 0.5), 10, 40));
        if (!a->h264) {
            set_msg(a, "cannot start the H.264 encoder (a picture of %dx%d?)", a->rw, a->rh);
            munmap(a->map, a->map_size);
            a->map = NULL;
            close(fd);
            return false;
        }
        cw = lm_h264enc_width(a->h264);
        ch = lm_h264enc_height(a->h264);
    }
    if (!create_file(a, cw, ch, !use_ff, &a->mp4)) {
        lm_h264enc_free(a->h264);
        a->h264 = NULL;
        munmap(a->map, a->map_size);
        a->map = NULL;
        close(fd);
        return false;
    }
    a->pool = wlc_alloc(a->win.c, &lm_wl_shm_pool_interface, NULL, NULL);
    WLC_SEND(a->win.c, a->win.shm, LM_WL_SHM_REQ_CREATE_POOL, { .n = a->pool }, { .h = fd }, { .i = (int32_t)a->map_size });
    close(fd);
    for (int i = 0; i < a->nframes; i++) {
        struct frame *f = &a->fr[i];
        f->px = (uint32_t *)(void *)((uint8_t *)a->map + (size_t)i * frame_bytes);
        f->buf = wlc_alloc(a->win.c, &lm_wl_buffer_interface, buffer_nothing, NULL);
        WLC_SEND(a->win.c, a->pool, LM_WL_SHM_POOL_REQ_CREATE_BUFFER, { .n = f->buf }, { .i = (int32_t)((size_t)i * frame_bytes) },
                 { .i = a->rw }, { .i = a->rh }, { .i = a->rw * 4 }, { .u = LM_WL_SHM_FORMAT_XRGB8888 });
    }
    WLC_SEND0(a->win.c, a->pool, LM_WL_SHM_POOL_REQ_DESTROY);
    wlc_forget(a->win.c, a->pool);
    a->pool = 0;

    a->qn = 0;
    a->stop_thread = false;
    a->enc_error = false;
    a->grabbing = -1;
    a->have_hash = false;
    a->n_grab = a->n_dup = a->n_fail = 0;
    a->enc_frames = 0;
    a->enc_bytes = 0;
    pthread_mutex_init(&a->lock, NULL);
    pthread_cond_init(&a->cond, NULL);
    a->seq_next = a->seq_write = 0;
    if (use_h264) {
        uint8_t *sps = NULL, *pps = NULL;
        size_t sn = 0, pn = 0;
        if (!lm_h264enc_headers(a->h264, &sps, &sn, &pps, &pn) || lm_mp4_set_h264(a->mp4, sps, sn, pps, pn) < 0) {
            free(sps);
            free(pps);
            set_msg(a, "cannot start the H.264 encoder");
            lm_mp4_close(a->mp4, 0);
            a->mp4 = NULL;
            lm_h264enc_free(a->h264);
            a->h264 = NULL;
            free_frames(a);
            return false;
        }
        free(sps);
        free(pps);
    } else if (use_ff) {
        char why[160];
        a->ff = ffpipe_start(a->path, a->rw, a->rh, 21, why, sizeof(why));
        a->ff_last = a->ff ? malloc(frame_bytes) : NULL;
        if (!a->ff_last && a->ff) {
            ffpipe_finish(a->ff);
            a->ff = NULL;
            snprintf(why, sizeof(why), "out of memory");
        }
        if (!a->ff) {
            set_msg(a, "%s", why);
            unlink(a->path);
            free_frames(a);
            return false;
        }
    } else {
        lm_jpeg_encode_init();                      /* the tables are built before several threads use them */
    }
    for (int i = 0; i < workers; i++)
        if (pthread_create(&a->threads[a->nthreads], NULL, use_ff ? ff_writer_main : use_h264 ? h264_writer_main : encoder_main, a) == 0)
            a->nthreads++;
    if (a->nthreads == 0) {
        set_msg(a, "cannot start the encoder thread");
        if (a->ff) {
            ffpipe_finish(a->ff);
            a->ff = NULL;
            free(a->ff_last);
            a->ff_last = NULL;
            unlink(a->path);
        } else {
            lm_mp4_close(a->mp4, 0);
            a->mp4 = NULL;
            lm_h264enc_free(a->h264);
            a->h264 = NULL;
        }
        free_frames(a);
        return false;
    }
    a->interval = 1000u / (uint64_t)FPS_CHOICES[a->fps_sel];
    a->t0 = lm_now_ms();
    a->next_grab = a->t0;
    a->rec = true;
    a->shown_sec = 0;
    a->win.redraw = true;
    tray_start(a);
    a->want_min = a->tray != 0;                     /* the bar's item shows that it runs: the window can go out of the way */
    fprintf(stderr, "mellagui-record: recording %s %dx%d at %d fps (%s) -> %s\n", o->name, a->rw, a->rh, FPS_CHOICES[a->fps_sel],
            use_ff ? "H.264 through ffmpeg" : use_h264 ? "H.264, built in" : "Motion JPEG", a->path);
    return true;
}

static void stop_recording(struct app *a)
{
    if (!a->rec)
        return;
    uint64_t end = lm_now_ms() - a->t0;
    a->rec = false;
    a->want_min = false;
    tray_stop(a);
    pthread_mutex_lock(&a->lock);
    a->stop_thread = true;
    pthread_cond_broadcast(&a->cond);
    pthread_mutex_unlock(&a->lock);
    for (int i = 0; i < a->nthreads; i++)
        pthread_join(a->threads[i], NULL);          /* they encode what is queued */
    a->nthreads = 0;
    pthread_mutex_destroy(&a->lock);
    pthread_cond_destroy(&a->cond);
    bool bad = a->enc_error;
    uint32_t frames;
    uint64_t bytes;
    int rc;
    if (a->ff) {
        frames = a->enc_frames;
        rc = ffpipe_finish(a->ff);                  /* waits until ffmpeg has finished the file */
        a->ff = NULL;
        free(a->ff_last);
        a->ff_last = NULL;
        struct stat st;
        bytes = stat(a->path, &st) == 0 ? (uint64_t)st.st_size : 0;
        if (frames == 0 || bytes == 0)
            rc = -1;
        if (rc < 0)
            unlink(a->path);
        else if (bad)
            rc = 0;
    } else {
        frames = lm_mp4_frames(a->mp4);
        bytes = lm_mp4_bytes(a->mp4);
        rc = lm_mp4_close(a->mp4, end);
        a->mp4 = NULL;
        lm_h264enc_free(a->h264);
        a->h264 = NULL;
    }
    free_frames(a);
    a->grabbing = -1;
    if (rc < 0) {
        if (frames == 0)
            set_msg(a, "nothing was captured%s%s", a->fail_reason[0] ? ": " : "", a->fail_reason);
        else
            set_msg(a, "writing %s failed%s", a->path, bad ? " (disk full?)" : "");
        notify(a, "Recording failed", a->msg);
    } else {
        snprintf(a->saved, sizeof(a->saved), "%s", a->path);
        fprintf(stderr, "mellagui-record: saved %s (%u pictures, %u unchanged skipped, %.1f MB, %.1f s)\n", a->path, frames, a->n_dup,
                (double)bytes / 1048576.0, (double)end / 1000.0);
        printf("%s\n", a->path);
        fflush(stdout);
        notify(a, "Recording saved", a->path);
        if (bad)
            set_msg(a, "some pictures could not be written");
    }
    a->win.redraw = true;
}

/* Called every loop turn: ask for the next picture when it is time. */
static void tick(struct app *a)
{
    if (g_signalled) {
        stop_recording(a);
        a->quit = true;
        return;
    }
    if (a->want_min && a->drawn) {
        a->want_min = false;
        a->hidden = true;
        WLC_SEND0(a->win.c, a->win.toplevel, LM_XDG_TOPLEVEL_REQ_SET_MINIMIZED);
    }
    if (a->tray_clicked) {
        a->tray_clicked = false;
        if (a->rec) {
            stop_recording(a);
            if (a->hidden)
                a->quit = true;                     /* nobody is looking at the window: the program ends with the recording */
        }
        return;
    }
    if (!a->rec)
        return;
    uint64_t now = lm_now_ms();
    tray_update(a, now - a->t0);
    pthread_mutex_lock(&a->lock);
    bool err = a->enc_error;
    pthread_mutex_unlock(&a->lock);
    if (err || (a->duration > 0 && (double)(now - a->t0) >= a->duration * 1000.0)) {
        if (err)
            set_msg(a, "writing the file failed");
        stop_recording(a);
        if (a->duration > 0)
            a->quit = true;
        return;
    }
    if (a->win.running && (now - a->t0) / 1000 != a->shown_sec) {
        a->shown_sec = (now - a->t0) / 1000;
        if (a->ff) {                                /* ffmpeg writes the file: its size is the best news we have */
            struct stat st;
            if (stat(a->path, &st) == 0) {
                pthread_mutex_lock(&a->lock);
                a->enc_bytes = (uint64_t)st.st_size;
                pthread_mutex_unlock(&a->lock);
            }
        }
        a->win.redraw = true;
    }
    if (a->grabbing >= 0 || now < a->next_grab)
        return;
    int free_i = -1;
    pthread_mutex_lock(&a->lock);
    for (int i = 0; i < a->nframes; i++)
        if (a->fr[i].state == F_FREE) {
            free_i = i;
            a->fr[i].state = F_GRAB;
            break;
        }
    pthread_mutex_unlock(&a->lock);
    if (free_i < 0)
        return;                                     /* the encoder is behind: skip this one */
    a->fr[free_i].pts = now - a->t0;
    a->grabbing = free_i;
    a->next_grab += a->interval;
    if (a->next_grab < now)
        a->next_grab = now + a->interval;
    WLC_SEND(a->win.c, a->capture, LM_MELLAGUI_CAPTURE_V1_REQ_GRAB, { .i = a->rout }, { .o = a->fr[free_i].buf });
}

static void capture_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)c; (void)id;
    struct app *a = data;
    switch (op) {
    case LM_MELLAGUI_CAPTURE_V1_EVT_OUTPUT: {
        int i = args[0].i;
        if (!a->listed && i >= 0 && i < MAX_OUTPUTS) {
            struct output *o = &a->outs[i];
            snprintf(o->name, sizeof(o->name), "%s", args[1].s ? args[1].s : "?");
            o->x = args[2].i; o->y = args[3].i; o->w = args[4].i; o->h = args[5].i;
            a->nouts = LM_MAX(a->nouts, i + 1);
        }
        break;
    }
    case LM_MELLAGUI_CAPTURE_V1_EVT_DONE:
        a->listed = true;
        a->win.redraw = true;
        break;
    case LM_MELLAGUI_CAPTURE_V1_EVT_READY: {
        int i = a->grabbing;
        a->grabbing = -1;
        if (i < 0 || !a->rec)
            break;
        struct frame *f = &a->fr[i];
        a->n_grab++;
        uint64_t h = hash_frame(f->px, (size_t)a->rw * (size_t)a->rh);
        if (a->have_hash && h == a->last_hash) {
            a->n_dup++;
            pthread_mutex_lock(&a->lock);
            f->state = F_FREE;
            pthread_mutex_unlock(&a->lock);
            break;
        }
        a->last_hash = h;
        a->have_hash = true;
        pthread_mutex_lock(&a->lock);
        f->state = F_QUEUED;
        f->seq = a->seq_next++;
        a->queue[a->qn++] = i;
        pthread_cond_broadcast(&a->cond);
        pthread_mutex_unlock(&a->lock);
        break;
    }
    case LM_MELLAGUI_CAPTURE_V1_EVT_FAILED: {
        int i = a->grabbing;
        a->grabbing = -1;
        const char *why = args[1].s ? args[1].s : "?";
        if (i >= 0) {
            pthread_mutex_lock(&a->lock);
            a->fr[i].state = F_FREE;
            pthread_mutex_unlock(&a->lock);
        }
        if (!a->rec)
            break;
        a->n_fail++;
        snprintf(a->fail_reason, sizeof(a->fail_reason), "%s", why);
        if (!strcmp(why, "size") || !strcmp(why, "no-output")) {
            set_msg(a, "the output changed, the recording ended");
            stop_recording(a);
            if (a->duration > 0)
                a->quit = true;
        }
        break;
    }
    }
}

static void settings_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)c; (void)id; (void)op; (void)args; (void)data;
}

static void global_event(struct wlwin *w, uint32_t name, const char *iface, uint32_t ver)
{
    (void)ver;
    struct app *a = w->data;
    if (!strcmp(iface, "mellagui_capture_v1"))
        a->capture = wlc_bind(w->c, w->registry, name, &lm_mellagui_capture_v1_interface, 1, capture_event, a);
    else if (!strcmp(iface, "mellagui_tray_v1"))
        a->tray = wlc_bind(w->c, w->registry, name, &lm_mellagui_tray_v1_interface, 1, settings_event, a);
    else if (!strcmp(iface, "mellagui_settings_v1"))
        a->settings = wlc_bind(w->c, w->registry, name, &lm_mellagui_settings_v1_interface, 1, settings_event, a);
}

/* ---- window ---------------------------------------------------------------------------------------------------------- */

#define DEF_W 400
#define DEF_H 168

static void layout(struct app *a)
{
    int m = 16, g = 8, y = 78, h = 40;
    int avail = a->win.bw - 2 * m;
    int wf = 72, wr = 112;
    int wo = LM_MAX(avail - wf - wr - 2 * g, 60);
    a->b_rec = (struct button){ m, y, wr, h };
    a->b_out = (struct button){ m + wr + g, y, wo, h };
    a->b_fps = (struct button){ m + wr + g + wo + g, y, wf, h };
}

static int button_at(const struct app *a, double x, double y)
{
    const struct button *bs[3] = { &a->b_rec, &a->b_out, &a->b_fps };
    for (int i = 0; i < 3; i++)
        if (x >= bs[i]->x && x < bs[i]->x + bs[i]->w && y >= bs[i]->y && y < bs[i]->y + bs[i]->h)
            return i + 1;
    return 0;
}

static void draw_button(struct app *a, const struct button *b, int id, uint32_t base, const char *label, bool dim)
{
    uint32_t col = base;
    if (dim)
        col = lm_mix_rgb(base, lm_theme.background, 0.55);
    else if (a->press == id)
        col = lm_mix_rgb(base, 0x000000, 0.25);
    else if (a->hover == id)
        col = lm_mix_rgb(base, 0xffffff, 0.18);
    lm_round_fill(a->win.pixels, a->win.bw, a->win.bw, a->win.bh, b->x, b->y, b->w, b->h, lm_button_radius(b->h / 2), col);
    int tw = lm_font_text_width(label);
    wlwin_text(&a->win, b->x + (b->w - tw) / 2, b->y + (b->h - LM_FONT_H) / 2, label, 1, dim ? lm_theme.text_dim : lm_theme.on_colour, b->w - 8);
}

static void paint(struct wlwin *w)
{
    struct app *a = w->data;
    a->drawn = true;
    layout(a);
    wlwin_fill(w, 0, 0, w->bw, w->bh, lm_theme.background);
    char line[200];
    if (a->rec) {
        uint64_t s = (lm_now_ms() - a->t0) / 1000;
        if (s % 2 == 0)
            lm_round_fill(w->pixels, w->bw, w->bw, w->bh, 16, 18, 14, 14, 7, lm_theme.alert);
        snprintf(line, sizeof(line), "Recording  %02u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
        wlwin_text(w, 38, 14, line, 2, lm_theme.text, w->bw - 54);
        pthread_mutex_lock(&a->lock);
        uint32_t frames = a->enc_frames;
        uint64_t bytes = a->enc_bytes;
        pthread_mutex_unlock(&a->lock);
        snprintf(line, sizeof(line), "%dx%d  %u pictures  %.1f MB", a->rw, a->rh, frames, (double)bytes / 1048576.0);
        wlwin_text(w, 16, 50, line, 1, lm_theme.muted, w->bw - 32);
    } else {
        wlwin_text(w, 16, 14, a->msg[0] ? "Not recorded" : a->saved[0] ? "Saved" : "Ready", 2, lm_theme.text, w->bw - 32);
        const char *info = a->msg[0] ? a->msg : a->saved[0] ? a->saved : a->capture ? "Record one screen into an MP4 file" :
                           "Screen capture is not available";
        wlwin_text(w, 16, 50, info, 1, a->msg[0] ? lm_theme.error : lm_theme.muted, w->bw - 32);
    }
    draw_button(a, &a->b_rec, 1, a->rec ? lm_theme.danger : lm_mix_rgb(lm_theme.ok, 0x000000, 0.35), a->rec ? "Stop" : "Record", !a->capture);
    char ol[64] = "no output";
    if (a->nouts > 0)
        snprintf(ol, sizeof(ol), "%s %dx%d", a->outs[a->out_sel].name, a->outs[a->out_sel].w, a->outs[a->out_sel].h);
    draw_button(a, &a->b_out, 2, lm_theme.button, ol, a->rec || a->nouts == 0);
    snprintf(line, sizeof(line), "%d fps", FPS_CHOICES[a->fps_sel]);
    draw_button(a, &a->b_fps, 3, lm_theme.button, line, a->rec);
    char dirbuf[512];
    snprintf(line, sizeof(line), "Into %s", record_dir(a, dirbuf, sizeof(dirbuf)));
    wlwin_text(w, 16, w->bh - 28, line, 1, lm_theme.text_dim, w->bw - 32);
}

static void toggle(struct app *a)
{
    if (a->rec)
        stop_recording(a);
    else
        start_recording(a);
}

static void press_button(struct app *a, int id)
{
    switch (id) {
    case 1:
        if (a->capture)
            toggle(a);
        break;
    case 2:
        if (!a->rec && a->nouts > 0)
            a->out_sel = (a->out_sel + 1) % a->nouts;
        break;
    case 3:
        if (!a->rec)
            a->fps_sel = (a->fps_sel + 1) % N_FPS;
        break;
    }
    a->win.redraw = true;
}

static void pointer_event(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
    struct app *a = w->data;
    int before = a->hover;
    switch (op) {
    case LM_WL_POINTER_EVT_ENTER:
        a->has_pointer = true;
        a->px = lm_fixed_to_double(args[2].f);
        a->py = lm_fixed_to_double(args[3].f);
        break;
    case LM_WL_POINTER_EVT_LEAVE:
        a->has_pointer = false;
        a->press = 0;
        break;
    case LM_WL_POINTER_EVT_MOTION:
        a->px = lm_fixed_to_double(args[1].f);
        a->py = lm_fixed_to_double(args[2].f);
        break;
    case LM_WL_POINTER_EVT_BUTTON:
        if (args[2].u != 272)                       /* BTN_LEFT */
            break;
        if (args[3].u == 1) {
            a->press = button_at(a, a->px, a->py);
        } else {
            int id = button_at(a, a->px, a->py);
            if (id && id == a->press)
                press_button(a, id);
            a->press = 0;
        }
        a->win.redraw = true;
        break;
    }
    a->hover = a->has_pointer ? button_at(a, a->px, a->py) : 0;
    if (a->hover != before)
        a->win.redraw = true;
}

/* Esc / Q / the window's close button: a recording goes on (the window is minimized, the bar's item stops it), else the program ends. */
static void close_or_hide(struct app *a)
{
    if (a->rec && a->tray) {
        a->hidden = true;
        WLC_SEND0(a->win.c, a->win.toplevel, LM_XDG_TOPLEVEL_REQ_SET_MINIMIZED);
    } else {
        a->quit = true;
    }
}

static bool win_close(struct wlwin *w)
{
    struct app *a = w->data;
    if (a->rec && a->tray) {
        close_or_hide(a);
        return false;
    }
    return true;
}

static void keyboard_event(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
    struct app *a = w->data;
    if (op != LM_WL_KEYBOARD_EVT_KEY || args[3].u == 0)
        return;
    uint32_t ks = lm_keymap_keysym(args[2].u, a->win.mods);
    char txt[8] = "";
    if (ks == LM_KS_Return || ks == LM_KS_KP_Enter) {
        press_button(a, 1);
    } else if (ks == LM_KS_Escape) {
        close_or_hide(a);
    } else if (lm_keysym_to_utf8(ks, txt) == 1 && !(a->win.mods & (LM_MOD_ALT | LM_MOD_SUPER | LM_MOD_CTRL))) {
        if (txt[0] == ' ')
            press_button(a, 1);
        else if (txt[0] == 'o' || txt[0] == 'O')
            press_button(a, 2);
        else if (txt[0] == 'f' || txt[0] == 'F')
            press_button(a, 3);
        else if (txt[0] == 'q' || txt[0] == 'Q')
            close_or_hide(a);
    }
}

/* ---- selftest -------------------------------------------------------------------------------------------------------- */

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static int selftest(void)
{
    int bad = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "selftest: FAILED %s (line %d)\n", #c, __LINE__); bad++; } } while (0)
    const int w = 203, h = 117, n = 12;
    char dir[] = "/tmp/mellagui-record-test-XXXXXX";
    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 1;
    }
    char path[300];
    snprintf(path, sizeof(path), "%s/t.mp4", dir);
    uint32_t *px = malloc((size_t)w * h * 4);
    struct lm_mp4 *m = lm_mp4_create(path, w, h);
    CHECK(m != NULL);
    for (int f = 0; f < n && m; f++) {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                px[(size_t)y * w + x] = (uint32_t)(((x * 3 + f * 9) & 255) << 16 | (y * 2 & 255) << 8 | (f * 20 & 255));
        size_t len = 0;
        uint8_t *j = lm_jpeg_encode_xrgb(px, w, h, (size_t)w * 4, 80, &len);
        CHECK(j && len > 100 && j[0] == 0xff && j[1] == 0xd8 && j[len - 2] == 0xff && j[len - 1] == 0xd9);
        CHECK(lm_mp4_add(m, j, len, 500 + (uint64_t)f * 100) == 0);
        free(j);
    }
    CHECK(m && lm_mp4_frames(m) == (uint32_t)n);
    CHECK(m && lm_mp4_close(m, 500 + n * 100 + 100) == 0);
    FILE *fp = fopen(path, "rb");
    CHECK(fp != NULL);
    if (fp) {
        fseek(fp, 0, SEEK_END);
        long size = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        uint8_t *d = malloc((size_t)size);
        CHECK(d && fread(d, 1, (size_t)size, fp) == (size_t)size);
        /* top level boxes: ftyp, mdat (64 bit size), moov, nothing after */
        size_t at = 0;
        int saw_ftyp = 0, saw_mdat = 0, saw_moov = 0;
        while (d && at + 8 <= (size_t)size) {
            uint64_t sz = be32(d + at);
            size_t hdr = 8;
            if (sz == 1) {
                sz = (uint64_t)be32(d + at + 8) << 32 | be32(d + at + 12);
                hdr = 16;
            }
            CHECK(sz >= hdr && at + sz <= (size_t)size);
            if (!memcmp(d + at + 4, "ftyp", 4)) saw_ftyp = 1;
            if (!memcmp(d + at + 4, "mdat", 4)) { saw_mdat = 1; CHECK(d[at + hdr] == 0xff && d[at + hdr + 1] == 0xd8); }
            if (!memcmp(d + at + 4, "moov", 4)) {
                saw_moov = 1;
                const uint8_t *z = memmem(d + at, (size_t)sz, "stsz", 4);
                CHECK(z && be32(z + 12) == (uint32_t)n);
                const uint8_t *t = memmem(d + at, (size_t)sz, "mvhd", 4);
                CHECK(t && be32(t + 16) == 1000 && be32(t + 20) == (uint32_t)(n * 100 + 100));       /* time scale, duration */
            }
            if (sz == 0)
                break;
            at += sz;
        }
        CHECK(saw_ftyp && saw_mdat && saw_moov && at == (size_t)size);
        free(d);
        fclose(fp);
    }
    /* a file with no picture is removed, an existing name is not overwritten */
    struct lm_mp4 *m2 = lm_mp4_create(path, w, h);
    CHECK(m2 == NULL && errno == EEXIST);
    unlink(path);
    char path2[300];
    snprintf(path2, sizeof(path2), "%s/e.mp4", dir);
    m2 = lm_mp4_create(path2, w, h);
    CHECK(m2 && lm_mp4_close(m2, 100) < 0 && access(path2, F_OK) != 0);
    /* our H.264 encoder and decoder agree, picture for picture: an I picture, P pictures with changes, an IDR in the middle */
    {
        const int hw = 70, hh = 46;                                 /* not a multiple of 16 */
        struct lm_h264enc *he = lm_h264enc_create(hw, hh, 24);
        uint8_t *sps = NULL, *pps = NULL;
        size_t sn = 0, pn = 0;
        char herr[120] = "";
        CHECK(he != NULL && lm_h264enc_headers(he, &sps, &sn, &pps, &pn));
        struct lm_h264dec *hd = he ? lm_h264dec_create(sps, sn, pps, pn, herr, sizeof(herr)) : NULL;
        CHECK(hd != NULL);
        for (int f = 0; he && hd && f < 8; f++) {
            for (int y = 0; y < hh; y++)
                for (int x = 0; x < hw; x++)
                    px[(size_t)y * (size_t)w + (size_t)x] = (uint32_t)((((x * 4) & 255) << 16) | ((y * 5) & 255) << 8 | 0x40);
            for (int y = 8; y < 24; y++)                          /* something that moves and changes colour */
                for (int x = f * 6; x < f * 6 + 20 && x < hw; x++)
                    px[(size_t)y * (size_t)w + (size_t)x] = f & 1 ? 0xffffff : 0x101010;
            size_t len = 0;
            bool key = false;
            uint8_t *au = lm_h264enc_encode(he, px, (size_t)w * 4, f == 0 || f == 4, &len, &key);
            CHECK(au != NULL && key == (f == 0 || f == 4));
            bool got = au != NULL;
            CHECK(got && lm_h264dec_decode(hd, au, len, herr, sizeof(herr)) == 0);
            free(au);
            for (int pl = 0; pl < 3 && got; pl++) {
                int s1, s2;
                const uint8_t *pa = lm_h264enc_recon(he, pl, &s1), *pb = lm_h264dec_plane(hd, pl, &s2);
                int pw2 = pl ? hw / 2 : hw, ph2 = pl ? hh / 2 : hh;
                for (int y = 0; y < ph2; y++)
                    CHECK(!memcmp(pa + (size_t)y * (size_t)s1, pb + (size_t)y * (size_t)s2, (size_t)pw2));
            }
        }
        lm_h264dec_free(hd);
        lm_h264enc_free(he);
        free(sps);
        free(pps);
    }
    /* the flat-block shortcut of the DCT: a plain colour must come out as that colour */
    for (size_t i = 0; i < (size_t)w * h; i++)
        px[i] = 0x336699;
    size_t len = 0;
    uint8_t *j = lm_jpeg_encode_xrgb(px, w, h, (size_t)w * 4, 90, &len);
    CHECK(j && len > 100 && len < 4000);
    free(j);
    free(px);
    rmdir(dir);
    if (!bad)
        printf("mellagui-record selftest: ok\n");
    return bad ? 1 : 0;
#undef CHECK
}

/* ---- main ------------------------------------------------------------------------------------------------------------ */

static void usage(void)
{
    fprintf(stderr,
            "usage: mellagui-record [--start] [--duration SECONDS] [--output N] [--fps N] [--quality 1..100] [--dir DIR] [--name FILE]\n"
            "                        [--codec auto|h264|ffmpeg|mjpeg]\n"
            "       mellagui-record --list | --selftest\n");
}

int main(int argc, char **argv)
{
    lm_theme_load_env();
    struct app a = { .fps_sel = 2, .quality = DEFAULT_QUALITY, .grabbing = -1 };
    bool want_start = false, list_only = false;
    for (int i = 1; i < argc; i++) {
        const char *s = argv[i];
        const char *val = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(s, "--selftest")) return selftest();
        else if (!strcmp(s, "--list")) list_only = true;
        else if (!strcmp(s, "--start")) want_start = true;
        else if (!strcmp(s, "--duration") && val) { a.duration = atof(val); i++; }
        else if (!strcmp(s, "--output") && val) { a.out_sel = atoi(val); i++; }
        else if (!strcmp(s, "--quality") && val) { a.quality = LM_CLAMP(atoi(val), 1, 100); i++; }
        else if (!strcmp(s, "--dir") && val) { a.dir = val; i++; }
        else if (!strcmp(s, "--name") && val) { a.name = val; i++; }
        else if (!strcmp(s, "--codec") && val) {
            a.codec_opt = !strcmp(val, "auto") ? 0 : !strcmp(val, "mjpeg") ? 1 : !strcmp(val, "ffmpeg") ? 2 : !strcmp(val, "h264") ? 3 : -1;
            if (a.codec_opt < 0) {
                usage();
                return 2;
            }
            i++;
        }
        else if (!strcmp(s, "--fps") && val) {
            int f = atoi(val), best = 0;
            for (int k = 0; k < N_FPS; k++)
                if (abs(FPS_CHOICES[k] - f) < abs(FPS_CHOICES[best] - f))
                    best = k;
            a.fps_sel = best;
            i++;
        } else {
            usage();
            return 2;
        }
    }
    (void)DEFAULT_FPS;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    static const struct wlwin_ops ops = { .paint = paint, .global = global_event, .pointer = pointer_event, .keyboard = keyboard_event, .close = win_close };
    if (wlwin_connect(&a.win, "mellagui-record", &ops, &a) < 0)
        return 1;
    if (!a.capture) {
        fprintf(stderr, "mellagui-record: the compositor offers no mellagui_capture_v1 (or not to this program)\n");
        if (!want_start && !list_only) {
            /* the window still opens and says so */
        } else {
            wlwin_destroy(&a.win);
            return 1;
        }
    } else {
        WLC_SEND0(a.win.c, a.capture, LM_MELLAGUI_CAPTURE_V1_REQ_LIST);
        if (wlc_roundtrip(a.win.c) < 0) {
            wlwin_destroy(&a.win);
            return 1;
        }
    }
    if (list_only) {
        for (int i = 0; i < a.nouts; i++)
            printf("%d %s %dx%d+%d+%d\n", i, a.outs[i].name, a.outs[i].w, a.outs[i].h, a.outs[i].x, a.outs[i].y);
        wlwin_destroy(&a.win);
        return 0;
    }
    if (a.capture && (a.out_sel < 0 || a.out_sel >= a.nouts)) {
        fprintf(stderr, "mellagui-record: no such output (%d), --list shows them\n", a.out_sel);
        wlwin_destroy(&a.win);
        return 1;
    }
    if (wlwin_open(&a.win, "Screen recorder", "mellagui.record", DEF_W, DEF_H, DEF_W, DEF_H) < 0) {
        wlwin_destroy(&a.win);
        return 1;
    }
    int rc = 0;
    if (want_start && !start_recording(&a))
        rc = 1;
    while (rc == 0 && !a.quit) {
        int timeout = 250;
        if (a.rec) {                                /* wake up when the next picture is due: a fixed 20 ms made the intervals uneven */
            uint64_t now = lm_now_ms();
            timeout = a.next_grab > now ? (int)LM_MIN(a.next_grab - now, 20) : 1;
            if (a.grabbing >= 0)
                timeout = LM_MIN(timeout, 5);       /* the answer is on its way */
        }
        if (!wlwin_step(&a.win, timeout))
            break;
        tick(&a);
    }
    stop_recording(&a);
    if (want_start && a.msg[0])
        rc = 1;
    wlwin_destroy(&a.win);
    return rc;
}
