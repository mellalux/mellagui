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
 * up. The pointer is drawn into the pictures (screenshots have none).
 *
 * Sound (only with the ffmpeg codec: ffmpeg opens the sound sources itself, so picture and sound share one clock; src: client/ffpipe.c): the computer's
 * own sound (the monitor of the default output: beeps, music, a video) and / or the microphone, mixed into one AAC track. Two switches in the window,
 * kept in record.conf (system-sound = 1 by default, microphone = 0), `--sound none|system|mic|both` for one run. $MELLAGUI_RECORD_SYSTEM_SRC / _MIC_SRC
 * name other sources ("format:device", ffmpeg's -f / -i; tests use lavfi:sine=frequency=440), `none` = off.
 *
 * Window: a row of buttons for the output, the frame rate, the sound and what happens at the end, a big Record / Stop button, and at the left a rail
 * (wlrail.h) with the last recording and the folder. Keys: Space or Enter record / stop, O next output, F next frame rate, S computer sound, M microphone,
 * V open the video at the end or not, P open the last recording, D the folder, Esc or Q close (a recording is finished first). SIGINT / SIGTERM also finish
 * the file.
 *
 * The choices are kept in record.conf (system-sound, microphone, output = NAME, fps, open = 0|1); the command line is for one run and is not saved.
 * When the user ends a recording (the button, a key, the bar's item) the file is opened in mellagui-view ($MELLAGUI_RECORD_VIEWER names another program,
 * "none" = never; $MELLAGUI_RECORD_FILES likewise for the folder). A --duration that runs out or a signal opens nothing.
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
#include <limits.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "audiocap.h"
#include "ffpipe.h"
#include "h264dec.h"
#include "h264enc.h"
#include "jpegenc.h"
#include "keymap.h"
#include "loop.h"
#include "mp4.h"
#include "sndconf.h"
#include "theme.h"
#include "uikit.h"
#include "util.h"
#include "wlrail.h"
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

/* the window's buttons: B_OUTPUT + i, B_FPS + i */
enum { B_RECORD = 1, B_OUTPUT = 10, B_SYSTEM = 30, B_MIC, B_OPEN, B_FPS = 40 };
#define MAX_BUTTONS 24

struct button {
    int id, x, y, w, h;
};

/* what record.conf keeps: changed only by the window's buttons and keys, never by the command line */
struct prefs {
    bool system, mic, open;
    int fps;
    char output[32];                /* "" = the first one */
};

struct app {
    struct wlwin win;
    uint32_t capture, settings, tray;
    uint32_t tray_ver;
    struct output outs[MAX_OUTPUTS];
    int nouts;
    bool listed;

    /* settings of the recording */
    int out_sel, fps_sel, quality;
    int codec_opt;                  /* --codec: 0 auto, 1 mjpeg (our own encoder), 2 ffmpeg (H.264 through libx264), 3 h264 (our own encoder) */
    const char *dir, *name;         /* --dir, --name */
    double duration;                /* --duration: stop (and quit) after that many seconds, 0 = never */
    bool snd_system, snd_mic;       /* record the computer's sound / the microphone (record.conf; --sound) */
    bool snd_rec;                   /* the recording in progress has a sound track */
    struct audiocap *acap;          /* the built-in encoders: the sound comes from parec (client/audiocap.c) and goes into the mp4 by tick() */

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
    struct button btn[MAX_BUTTONS];
    int nbtn;
    int hover, press;               /* a button's id, 0 none */
    int hovertool;                  /* the rail's button under the pointer, -1 none */
    struct wlrail_anim ran;
    struct prefs pref;
    bool open_after;                /* open the video when the user ends the recording */
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

static void prefs_load(struct prefs *p)
{
    lm_snd_conf_load("record", &p->system, &p->mic);
    char path[1024], line[256];
    if (!lm_snd_conf_path("record", path, sizeof(path)))
        return;
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        int v;
        char name[32];
        if (sscanf(line, "fps = %d", &v) == 1)
            p->fps = v;
        else if (sscanf(line, "open = %d", &v) == 1)
            p->open = v != 0;
        else if (sscanf(line, "output = %31s", name) == 1)
            snprintf(p->output, sizeof(p->output), "%s", name);
    }
    fclose(f);
}

static void prefs_save(const struct prefs *p)
{
    char extra[128];
    int n = snprintf(extra, sizeof(extra), "fps = %d\nopen = %d\n", p->fps, p->open ? 1 : 0);
    if (p->output[0])
        snprintf(extra + n, sizeof(extra) - (size_t)n, "output = %s\n", p->output);
    lm_snd_conf_save_x("record", p->system, p->mic, extra);
}

static int fps_index(int fps)
{
    int best = 0;
    for (int k = 1; k < N_FPS; k++)
        if (abs(FPS_CHOICES[k] - fps) < abs(FPS_CHOICES[best] - fps))
            best = k;
    return best;
}

/* Starts `sibling arg` in a detached child: the program next to this one (else in PATH); $envvar names another program, "none" = nothing. */
static void spawn_detached(const char *envvar, const char *sibling, const char *arg)
{
    const char *v = getenv(envvar);
    if (v && !strcmp(v, "none"))
        return;
    char prog[PATH_MAX];
    snprintf(prog, sizeof(prog), "%s", v && *v ? v : sibling);
    if (!v || !*v) {
        char exe[PATH_MAX];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 40);
        char *slash = n > 0 ? (exe[n] = 0, strrchr(exe, '/')) : NULL;
        if (slash) {
            snprintf(slash + 1, 32, "%s", sibling);
            if (access(exe, X_OK) == 0)
                snprintf(prog, sizeof(prog), "%s", exe);
        }
    }
    pid_t mid = fork();
    if (mid < 0)
        return;
    if (mid > 0) {
        int st;
        waitpid(mid, &st, 0);
        return;
    }
    if (fork() != 0)
        _exit(0);
    setsid();
    int nul = open("/dev/null", O_RDWR);
    if (nul >= 0) {
        dup2(nul, 0);
        dup2(nul, 1);
        dup2(nul, 2);
    }
    execlp(prog, prog, arg, (char *)NULL);
    _exit(127);
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
    if (a->tray_ver >= 3)                       /* (its place in the user's order of the panel) */
        WLC_SEND(a->win.c, a->tray_item, LM_MELLAGUI_TRAY_ITEM_V1_REQ_SET_NAME, { .s = "record" });
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
        struct ffpipe_audio au = { a->snd_system ? ffpipe_audio_source(false) : NULL, a->snd_mic ? ffpipe_audio_source(true) : NULL };
        a->snd_rec = au.system || au.mic;
        a->ff = ffpipe_start(a->path, a->rw, a->rh, 21, &au, why, sizeof(why));
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
    if (!use_ff) {
        a->snd_rec = false;
        if ((a->snd_system || a->snd_mic) && a->mp4) {          /* the built-in encoders: PCM from parec into the file's own sound track */
            char why[200];
            a->acap = audiocap_start(a->snd_system ? ffpipe_audio_source(false) : NULL, a->snd_mic ? ffpipe_audio_source(true) : NULL, why, sizeof(why));
            if (a->acap && lm_mp4_set_audio(a->mp4, AUDIOCAP_RATE, AUDIOCAP_CHANNELS) == 0) {
                a->snd_rec = true;
            } else {
                fprintf(stderr, "mellagui-record: no sound in this recording: %s\n", a->acap ? "cannot add a sound track" : why);
                audiocap_stop(a->acap);
                a->acap = NULL;
            }
        }
    }
    a->interval = 1000u / (uint64_t)FPS_CHOICES[a->fps_sel];
    a->t0 = lm_now_ms();
    a->next_grab = a->t0;
    a->rec = true;
    a->shown_sec = 0;
    a->win.redraw = true;
    tray_start(a);
    a->want_min = a->tray != 0;                     /* the bar's item shows that it runs: the window can go out of the way */
    fprintf(stderr, "mellagui-record: recording %s %dx%d at %d fps (%s%s) -> %s\n", o->name, a->rw, a->rh, FPS_CHOICES[a->fps_sel],
            use_ff ? "H.264 through ffmpeg" : use_h264 ? "H.264, built in" : "Motion JPEG", a->snd_rec ? ", with sound" : "", a->path);
    return true;
}

static void audio_pump(struct app *a);

/* `by_user`: the user ended it (or the screen changed under it), so the video may be shown; not for a signal, a --duration or a failure. */
static void stop_recording(struct app *a, bool by_user)
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
        audio_pump(a);                                  /* what is left of the sound, then the end */
        audiocap_stop(a->acap);
        a->acap = NULL;
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
        else if (by_user && a->open_after)
            spawn_detached("MELLAGUI_RECORD_VIEWER", "mellagui-view", a->saved);
    }
    a->win.redraw = true;
}

/* The sound that parec has delivered so far goes into the file (the built-in encoders; ffmpeg reads its sound itself). */
static void audio_pump(struct app *a)
{
    if (!a->acap || !a->mp4)
        return;
    static int16_t buf[4096 * AUDIOCAP_CHANNELS];
    for (;;) {
        size_t n = audiocap_read(a->acap, buf, 4096);
        if (!n)
            break;
        uint64_t first = audiocap_first_ms(a->acap);
        if (lm_mp4_add_audio(a->mp4, buf, n, first > a->t0 ? first - a->t0 : 0) < 0) {
            a->enc_error = true;
            break;
        }
    }
}

/* Called every loop turn: ask for the next picture when it is time. */
static void tick(struct app *a)
{
    audio_pump(a);
    if (g_signalled) {
        stop_recording(a, false);
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
            stop_recording(a, true);
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
        stop_recording(a, false);
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
            stop_recording(a, true);
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
    struct app *a = w->data;
    if (!strcmp(iface, "mellagui_capture_v1"))
        a->capture = wlc_bind(w->c, w->registry, name, &lm_mellagui_capture_v1_interface, 1, capture_event, a);
    else if (!strcmp(iface, "mellagui_tray_v1"))
        a->tray = wlc_bind(w->c, w->registry, name, &lm_mellagui_tray_v1_interface, a->tray_ver = LM_MIN(ver, 3u), settings_event, a);
    else if (!strcmp(iface, "mellagui_settings_v1"))
        a->settings = wlc_bind(w->c, w->registry, name, &lm_mellagui_settings_v1_interface, 1, settings_event, a);
}

/* ---- window ---------------------------------------------------------------------------------------------------------- */

/* The look of the other programs: a rail of round drops at the left (the last recording, the folder), a header that says what is going on, a card with
 * a row of drops for every setting, the big Record / Stop button under it and a status bar at the bottom. */

#define DEF_W 430
#define DEF_H 330
#define PAD 14
#define HEAD_H 62
#define FOOT_H 30
#define ROW_H 28
#define ROW_PITCH 38
#define LABEL_W 92

static const char *const ROW_LABELS[] = { "Screen", "Frames / s", "Sound", "When done" };
#define N_ROWS ((int)(sizeof(ROW_LABELS) / sizeof(ROW_LABELS[0])))

static void add_button(struct app *a, int id, int x, int y, int w, int h)
{
    if (a->nbtn < MAX_BUTTONS)
        a->btn[a->nbtn++] = (struct button){ id, x, y, w, h };
}

/* `n` buttons side by side in the row, `first` is the id of the first one */
static void add_row(struct app *a, int row, int first, int n)
{
    int x = PAD + 10 + LABEL_W, g = 6, avail = a->win.bw - x - PAD - 10;
    int y = HEAD_H + 10 + row * ROW_PITCH;
    for (int i = 0; i < n; i++) {
        int x0 = x + i * (avail + g) / n, x1 = x + (i + 1) * (avail + g) / n - g;
        add_button(a, first + i, x0, y, x1 - x0, ROW_H);
    }
}

static void layout(struct app *a)
{
    a->nbtn = 0;
    add_row(a, 0, B_OUTPUT, LM_MAX(a->nouts, 1));
    add_row(a, 1, B_FPS, N_FPS);
    add_row(a, 2, B_SYSTEM, 2);                     /* (B_MIC follows B_SYSTEM) */
    add_row(a, 3, B_OPEN, 1);
    int y = HEAD_H + 10 + N_ROWS * ROW_PITCH + 12, w = 190, h = 44;
    add_button(a, B_RECORD, (a->win.bw - w) / 2, y, w, h);
}

static int button_at(const struct app *a, double x, double y)
{
    for (int i = 0; i < a->nbtn; i++) {
        const struct button *b = &a->btn[i];
        if (x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h)
            return b->id;
    }
    return 0;
}

/* Can the button be used now? (The settings stay as they are while a recording runs.) */
static bool usable(const struct app *a, int id)
{
    if (id == B_RECORD)
        return a->capture != 0;
    if (id >= B_OUTPUT && id < B_OUTPUT + MAX_OUTPUTS)
        return !a->rec && id - B_OUTPUT < a->nouts;
    if (id == B_OPEN)
        return true;                                /* (what happens at the end may be changed to the last moment) */
    return !a->rec;
}

static void button_label(const struct app *a, const struct button *b, char *out, size_t n)
{
    int id = b->id;
    if (id == B_RECORD) {
        snprintf(out, n, "%s", a->rec ? "Stop" : "Record");
    } else if (id >= B_OUTPUT && id < B_OUTPUT + MAX_OUTPUTS) {
        int i = id - B_OUTPUT;
        if (i >= a->nouts) {
            snprintf(out, n, "no screen");
            return;
        }
        snprintf(out, n, "%s  %dx%d", a->outs[i].name, a->outs[i].w, a->outs[i].h);
        if (lm_font_text_width(out) > b->w - 10)
            snprintf(out, n, "%s", a->outs[i].name);
    } else if (id >= B_FPS && id < B_FPS + N_FPS) {
        snprintf(out, n, "%d", FPS_CHOICES[id - B_FPS]);
    } else if (id == B_SYSTEM) {
        snprintf(out, n, "%s", "Computer");
    } else if (id == B_MIC) {
        snprintf(out, n, "%s", "Microphone");
    } else {
        snprintf(out, n, "%s", a->open_after ? "Open the video" : "Only save it");
    }
}

static bool button_lit(const struct app *a, int id)
{
    if (id >= B_OUTPUT && id < B_OUTPUT + MAX_OUTPUTS)
        return id - B_OUTPUT == a->out_sel && a->nouts > 0;
    if (id >= B_FPS && id < B_FPS + N_FPS)
        return id - B_FPS == a->fps_sel;
    return (id == B_SYSTEM && a->snd_system) || (id == B_MIC && a->snd_mic) || (id == B_OPEN && a->open_after);
}

static void draw_button(struct app *a, const struct button *b)
{
    bool on = button_lit(a, b->id), can = usable(a, b->id);
    bool choice = b->id < B_SYSTEM || (b->id >= B_FPS && b->id < B_FPS + N_FPS);
    uint32_t base = b->id == B_RECORD ? lm_theme.btn_red : !on ? lm_theme.btn_slate : choice && b->id != B_RECORD ? lm_theme.btn_blue : lm_theme.btn_green;
    uint32_t ink = lm_theme.on_colour, col = base;
    if (!can) {
        col = on ? lm_mix_rgb(base, lm_theme.card, 0.45) : lm_theme.control_idle;
        ink = on ? lm_theme.on_colour : lm_theme.title_text_idle;
    } else if (a->press == b->id) {
        col = lm_mix_rgb(base, 0x000000, 0.25);
    } else if (a->hover == b->id) {
        col = lm_mix_rgb(base, 0xffffff, 0.18);
    }
    wlwin_drop(&a->win, b->x, b->y, b->w, b->h, lm_button_radius(b->h / 2), col);
    char label[64];
    button_label(a, b, label, sizeof(label));
    int tw = lm_font_text_width(label), mark = b->id == B_RECORD ? 22 : 0;
    int x = b->x + LM_MAX((b->w - tw - mark) / 2, 4);
    if (mark)                                       /* a dot to start, a square to stop */
        wlwin_round_fill(&a->win, x, b->y + b->h / 2 - 7, 14, 14, a->rec ? 2 : 7, ink);
    wlwin_text(&a->win, x + mark, b->y + (b->h - LM_FONT_H) / 2, label, 1, ink, b->w - 8 - mark);
}

enum { R_PLAY, R_FOLDER, R_N };

static void ic_play(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    (void)bg;
    for (int i = 0; i < 12; i++)
        wlwin_fill(w, cx - 4 + i, cy - 7 + (i + 1) / 2, 1, 14 - 2 * ((i + 1) / 2), c);
}

static void record_rail(const struct app *a, struct wlrail *L)
{
    memset(L, 0, sizeof(*L));
    L->y0 = 0;
    L->y1 = a->win.bh > 0 ? a->win.bh - FOOT_H : DEF_H - FOOT_H;
    L->n = R_N;
    L->it[R_PLAY] = (struct wlrail_item){ .tip = "Open the last recording  P", .icon = ic_play, .base = lm_theme.btn_green, .lite = lm_theme.btn_green_lit,
                                          .show = true, .idle = !a->saved[0] };
    L->it[R_FOLDER] = (struct wlrail_item){ .tip = "Show the folder  D", .icon = wlrail_ic_open, .base = lm_theme.btn_amber, .lite = lm_theme.btn_amber_lit,
                                            .show = true };
}

static int rail_under(const struct app *a)
{
    if (!a->has_pointer || a->px >= 0)
        return -1;
    struct wlrail L;
    record_rail(a, &L);
    int i = wlrail_at(&L, a->win.rpx, a->win.rpy);
    return i >= 0 && !L.it[i].idle ? i : -1;
}

/* $MELLAGUI_RECORD_DUMP=FILE: what the window shows, for the tests (window coordinates). */
static void dump_state(const struct app *a)
{
    const char *path = getenv("MELLAGUI_RECORD_DUMP");
    if (!path || !*path)
        return;
    char tmp[1100];
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return;
    fprintf(f, "state rec=%d out=%d fps=%d system=%d mic=%d open=%d saved=%s\n", a->rec, a->out_sel, FPS_CHOICES[a->fps_sel], a->snd_system, a->snd_mic,
            a->open_after, a->saved[0] ? a->saved : "-");
    for (int i = 0; i < a->nbtn; i++) {
        const struct button *b = &a->btn[i];
        char name[32];
        if (b->id == B_RECORD) snprintf(name, sizeof(name), "record");
        else if (b->id == B_SYSTEM) snprintf(name, sizeof(name), "system");
        else if (b->id == B_MIC) snprintf(name, sizeof(name), "mic");
        else if (b->id == B_OPEN) snprintf(name, sizeof(name), "open");
        else if (b->id >= B_FPS) snprintf(name, sizeof(name), "fps%d", FPS_CHOICES[b->id - B_FPS]);
        else snprintf(name, sizeof(name), "output%d", b->id - B_OUTPUT);
        fprintf(f, "btn %s %d %d %d %d\n", name, b->x + a->win.inx, b->y, b->w, b->h);
    }
    struct wlrail L;
    record_rail(a, &L);
    int y[WLRAIL_MAX];
    wlrail_layout(&L, y);
    for (int i = 0; i < L.n; i++)
        fprintf(f, "rail %s %d %d %d %d idle=%d\n", i == R_PLAY ? "play" : "folder", (WLRAIL_W - WLRAIL_D) / 2, y[i], WLRAIL_D, WLRAIL_D, L.it[i].idle);
    if (fclose(f) != 0 || rename(tmp, path) < 0)
        unlink(tmp);
}

static void paint(struct wlwin *w)
{
    struct app *a = w->data;
    a->drawn = true;
    layout(a);
    wlwin_fill(w, 0, 0, w->bw, w->bh, lm_theme.background);

    char line[700];
    if (a->rec) {
        uint64_t s = (lm_now_ms() - a->t0) / 1000;
        if (s % 2 == 0)
            wlwin_round_fill(w, PAD, 15, 14, 14, 7, lm_theme.alert);
        snprintf(line, sizeof(line), "Recording  %02u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
        wlwin_text(w, PAD + 22, 6, line, 2, lm_theme.accent, w->bw - 2 * PAD - 22);
        pthread_mutex_lock(&a->lock);
        uint32_t frames = a->enc_frames;
        uint64_t bytes = a->enc_bytes;
        pthread_mutex_unlock(&a->lock);
        snprintf(line, sizeof(line), "%dx%d  %u pictures  %.1f MB%s", a->rw, a->rh, frames, (double)bytes / 1048576.0, a->snd_rec ? "  with sound" : "");
        wlwin_text(w, PAD, 40, line, 1, lm_theme.text_dim, w->bw - 2 * PAD);
    } else {
        wlwin_text(w, PAD, 6, a->msg[0] ? "Not recorded" : a->saved[0] ? "Saved" : "Ready", 2, lm_theme.accent, w->bw - 2 * PAD);
        bool snd_ok = (ffpipe_available() && a->codec_opt != 1 && a->codec_opt != 3) || audiocap_available();     /* ffmpeg reads the sound itself, else parec does */
        const char *base = strrchr(a->saved, '/');
        const char *info = a->saved[0] ? (base ? base + 1 : a->saved) :
                           !a->capture ? "Screen capture is not available" :
                           (a->snd_system || a->snd_mic) && !snd_ok ? "No sound: it needs ffmpeg or parec (neither was found)" :
                           (a->snd_system || a->snd_mic) ? "Records one screen and its sound into an MP4 file" : "Records one screen into an MP4 file";
        wlwin_text(w, PAD, 40, info, 1, lm_theme.text_dim, w->bw - 2 * PAD);
    }

    int ch = N_ROWS * ROW_PITCH + 10;
    wlwin_round_fill(w, PAD - 1, HEAD_H - 1, w->bw - 2 * PAD + 2, ch + 2, 9, lm_theme.border);
    wlwin_round_fill(w, PAD, HEAD_H, w->bw - 2 * PAD, ch, 8, lm_theme.card);
    for (int i = 0; i < N_ROWS; i++)
        wlwin_text(w, PAD + 12, HEAD_H + 10 + i * ROW_PITCH + (ROW_H - LM_FONT_H) / 2, ROW_LABELS[i], 1, lm_theme.text, LABEL_W - 4);
    for (int i = 0; i < a->nbtn; i++)
        draw_button(a, &a->btn[i]);

    int sy = w->bh - FOOT_H;
    wlwin_fill(w, 0, sy, w->bw, FOOT_H, lm_theme.bar);
    if (a->msg[0]) {
        wlwin_text(w, PAD, sy + 7, a->msg, 1, lm_theme.error, w->bw - 2 * PAD);
    } else {
        char dirbuf[512];
        snprintf(line, sizeof(line), "Into %s", record_dir(a, dirbuf, sizeof(dirbuf)));
        wlwin_text(w, PAD, sy + 7, line, 1, lm_theme.text_dim, w->bw - 2 * PAD);
    }

    struct wlrail L;
    record_rail(a, &L);
    wlrail_paint(w, &L, &a->ran, a->hovertool, wlrail_now(), lm_theme.bar, lm_theme.bar, lm_theme.button_hover, lm_theme.text_soft);
    wlrail_foot(w, &L, lm_theme.bar);
    dump_state(a);
}

static void toggle(struct app *a)
{
    if (a->rec)
        stop_recording(a, true);
    else
        start_recording(a);
}

static void press_button(struct app *a, int id)
{
    if (!usable(a, id))
        return;
    if (id == B_RECORD) {
        toggle(a);
    } else {
        if (id >= B_OUTPUT && id < B_OUTPUT + MAX_OUTPUTS) {
            a->out_sel = id - B_OUTPUT;
            snprintf(a->pref.output, sizeof(a->pref.output), "%s", a->outs[a->out_sel].name);
        } else if (id >= B_FPS && id < B_FPS + N_FPS) {
            a->fps_sel = id - B_FPS;
            a->pref.fps = FPS_CHOICES[a->fps_sel];
        } else if (id == B_SYSTEM) {
            a->pref.system = a->snd_system = !a->snd_system;
        } else if (id == B_MIC) {
            a->pref.mic = a->snd_mic = !a->snd_mic;
        } else if (id == B_OPEN) {
            a->pref.open = a->open_after = !a->open_after;
        }
        prefs_save(&a->pref);
    }
    a->win.redraw = true;
}

static void press_rail(struct app *a, int i)
{
    if (i == R_PLAY && a->saved[0]) {
        spawn_detached("MELLAGUI_RECORD_VIEWER", "mellagui-view", a->saved);
    } else if (i == R_FOLDER) {
        char dirbuf[512];
        const char *dir = record_dir(a, dirbuf, sizeof(dirbuf));
        mkdir(dir, 0755);
        spawn_detached("MELLAGUI_RECORD_FILES", "mellagui-files", dir);
    }
}

static void pointer_event(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
    struct app *a = w->data;
    int before = a->hover, before_tool = a->hovertool;
    switch (op) {
    case LM_WL_POINTER_EVT_ENTER:
        a->has_pointer = true;
        a->px = lm_fixed_to_double(args[2].f);
        a->py = lm_fixed_to_double(args[3].f);
        break;
    case LM_WL_POINTER_EVT_LEAVE:
        a->has_pointer = false;
        a->press = 0;
        wlrail_release(&a->ran);
        break;
    case LM_WL_POINTER_EVT_MOTION:
        a->px = lm_fixed_to_double(args[1].f);
        a->py = lm_fixed_to_double(args[2].f);
        break;
    case LM_WL_POINTER_EVT_BUTTON:
        if (args[2].u != 272)                       /* BTN_LEFT */
            break;
        if (args[3].u == 1) {
            int tool = rail_under(a);
            if (tool >= 0) {
                wlrail_press(&a->ran, tool, wlrail_now());
                press_rail(a, tool);
            } else {
                a->press = button_at(a, a->px, a->py);
            }
        } else {
            int id = button_at(a, a->px, a->py);
            if (id && id == a->press)
                press_button(a, id);
            a->press = 0;
            wlrail_release(&a->ran);
        }
        a->win.redraw = true;
        break;
    }
    a->hover = a->has_pointer ? button_at(a, a->px, a->py) : 0;
    if (a->hover && !usable(a, a->hover))
        a->hover = 0;
    a->hovertool = rail_under(a);
    if (a->hover != before || a->hovertool != before_tool)
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
        press_button(a, B_RECORD);
    } else if (ks == LM_KS_Escape) {
        close_or_hide(a);
    } else if (lm_keysym_to_utf8(ks, txt) == 1 && !(a->win.mods & (LM_MOD_ALT | LM_MOD_SUPER | LM_MOD_CTRL))) {
        if (txt[0] == ' ')
            press_button(a, B_RECORD);
        else if ((txt[0] == 'o' || txt[0] == 'O') && a->nouts > 0)
            press_button(a, B_OUTPUT + (a->out_sel + 1) % a->nouts);
        else if (txt[0] == 'f' || txt[0] == 'F')
            press_button(a, B_FPS + (a->fps_sel + 1) % N_FPS);
        else if (txt[0] == 's' || txt[0] == 'S')
            press_button(a, B_SYSTEM);
        else if (txt[0] == 'm' || txt[0] == 'M')
            press_button(a, B_MIC);
        else if (txt[0] == 'v' || txt[0] == 'V')
            press_button(a, B_OPEN);
        else if (txt[0] == 'p' || txt[0] == 'P')
            press_rail(a, R_PLAY);
        else if (txt[0] == 'd' || txt[0] == 'D')
            press_rail(a, R_FOLDER);
        else if (txt[0] == 'q' || txt[0] == 'Q')
            close_or_hide(a);
    }
    a->win.redraw = true;
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
    /* motion and the deblocking filter: a texture that scrolls (the whole picture, a part of it, sideways, far) is coded with vectors; encoder and decoder agree on every
     * picture, a scrolled picture costs a fraction of an I picture, and with MELLAGUI_H264_DUMP=DIR the stream (Annex B) and the pictures it makes are
     * written for the test that lets ffmpeg decode the same stream (bit for bit the same pictures). */
    {
        const int sw = 176, sh = 112;
        uint32_t *tx = malloc((size_t)sw * 2 * (size_t)sh * 2 * sizeof(uint32_t));      /* a texture bigger than the picture */
        const char *qe = getenv("MELLAGUI_H264_QP");
        struct lm_h264enc *he = lm_h264enc_create(sw, sh, qe && atoi(qe) > 0 ? atoi(qe) : 24);
        uint8_t *sps = NULL, *pps = NULL;
        size_t sn = 0, pn = 0;
        char herr[120] = "";
        CHECK(tx != NULL && he != NULL && lm_h264enc_headers(he, &sps, &sn, &pps, &pn));
        struct lm_h264dec *hd = he ? lm_h264dec_create(sps, sn, pps, pn, herr, sizeof(herr)) : NULL;
        CHECK(hd != NULL);
        const char *dump = getenv("MELLAGUI_H264_DUMP");
        FILE *fs = NULL, *fy = NULL;
        if (dump && *dump && he) {
            char fp[400];
            snprintf(fp, sizeof(fp), "%s/scroll.h264", dump);
            fs = fopen(fp, "wb");
            snprintf(fp, sizeof(fp), "%s/scroll.yuv", dump);
            fy = fopen(fp, "wb");
            static const uint8_t sc[4] = { 0, 0, 0, 1 };
            if (fs) {
                fwrite(sc, 1, 4, fs);
                fwrite(sps, 1, sn, fs);
                fwrite(sc, 1, 4, fs);
                fwrite(pps, 1, pn, fs);
            }
        }
        if (tx) {
            uint32_t seed = 12345;
            for (int y = 0; y < sh * 2; y++)
                for (int x = 0; x < sw * 2; x++) {
                    seed = seed * 1664525u + 1013904223u;
                    int v = (int)(seed >> 29);                                  /* a little noise on smooth ramps: the deblocking filter has work to do */
                    int blk = ((x / 24 + y / 20) & 1) * 14;                     /* and steps the filter may soften */
                    tx[(size_t)y * (size_t)sw * 2 + (size_t)x] = (uint32_t)((((x * 2 + v + blk) & 255) << 16) | (((y * 3 + v) & 255) << 8) | ((x + y + v * 2 + blk) & 255));
                }
        }
        static const struct { int dx, dy, part; } mov[] = {                 /* where the picture looks into the texture, frame by frame */
            { 0, 0, 0 }, { 0, 3, 0 }, { 0, 6, 0 }, { 0, 9, 0 }, { 5, 9, 0 }, { 10, 9, 0 }, { 10, 9, 0 }, { 10, 49, 0 },
            { 10, 49, 0 }, { 10, 52, 1 }, { 10, 55, 1 }, { 10, 58, 1 }, { 11, 58, 0 }, { 11, 45, 0 }, { 0, 0, 0 }, { 0, 1, 0 },
        };
        size_t first_len = 0, scroll_len = 0;
        for (int f = 0; tx && he && hd && f < (int)(sizeof(mov) / sizeof(mov[0])); f++) {
            uint32_t *pic = malloc((size_t)sw * (size_t)sh * sizeof(uint32_t));
            if (!pic)
                break;
            for (int y = 0; y < sh; y++)
                for (int x = 0; x < sw; x++) {
                    int ty = y + (mov[f].part && x >= sw / 2 ? 0 : mov[f].dy), tx_x = x + (mov[f].part && x >= sw / 2 ? 10 : mov[f].dx);   /* a part: the right half stands still */
                    pic[(size_t)y * (size_t)sw + (size_t)x] = tx[(size_t)ty * (size_t)sw * 2 + (size_t)tx_x];
                }
            size_t len = 0;
            bool key = false;
            uint8_t *au = lm_h264enc_encode(he, pic, (size_t)sw * 4, f == 0 || f == 8, &len, &key);
            free(pic);
            CHECK(au != NULL);
            if (!au)
                break;
            if (f == 0)
                first_len = len;
            if (f == 2)
                scroll_len = len;
            CHECK(lm_h264dec_decode(hd, au, len, herr, sizeof(herr)) == 0);
            if (fs) {
                static const uint8_t sc[4] = { 0, 0, 0, 1 };
                for (size_t at = 0; at + 4 <= len;) {
                    size_t n = (size_t)au[at] << 24 | (size_t)au[at + 1] << 16 | (size_t)au[at + 2] << 8 | au[at + 3];
                    fwrite(sc, 1, 4, fs);
                    fwrite(au + at + 4, 1, n, fs);
                    at += 4 + n;
                }
            }
            free(au);
            for (int pl = 0; pl < 3; pl++) {
                int s1, s2;
                const uint8_t *pa = lm_h264enc_recon(he, pl, &s1), *pb = lm_h264dec_plane(hd, pl, &s2);
                int pw2 = pl ? sw / 2 : sw, ph2 = pl ? sh / 2 : sh;
                for (int y = 0; y < ph2; y++) {
                    CHECK(!memcmp(pa + (size_t)y * (size_t)s1, pb + (size_t)y * (size_t)s2, (size_t)pw2));
                    if (fy)
                        fwrite(pa + (size_t)y * (size_t)s1, 1, (size_t)pw2, fy);
                }
            }
        }
        CHECK(first_len > 0 && scroll_len > 0);
        if (!qe || atoi(qe) <= 40)                                                  /* (a very coarse quantizer makes every picture small) */
            CHECK(scroll_len * 4 < first_len);                                      /* scrolled by 3 pixels: far smaller than the first picture */
        if (fs)
            fclose(fs);
        if (fy)
            fclose(fy);
        lm_h264dec_free(hd);
        lm_h264enc_free(he);
        free(sps);
        free(pps);
        free(tx);
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
            "                        [--codec auto|h264|ffmpeg|mjpeg] [--sound none|system|mic|both]\n"
            "       mellagui-record --list | --selftest\n");
}

int main(int argc, char **argv)
{
    wlwin_hidpi();                      /* draws through the wlwin_* calls only: sharp on a scaled screen */
    lm_theme_load_env();
    struct app a = { .quality = DEFAULT_QUALITY, .grabbing = -1, .hovertool = -1, .pref = { .system = true, .open = true, .fps = DEFAULT_FPS } };
    prefs_load(&a.pref);
    a.snd_system = a.pref.system;
    a.snd_mic = a.pref.mic;
    a.open_after = a.pref.open;
    a.fps_sel = fps_index(a.pref.fps);
    bool want_start = false, list_only = false, output_given = false;
    for (int i = 1; i < argc; i++) {
        const char *s = argv[i];
        const char *val = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(s, "--selftest")) return selftest();
        else if (!strcmp(s, "--list")) list_only = true;
        else if (!strcmp(s, "--start")) want_start = true;
        else if (!strcmp(s, "--duration") && val) { a.duration = atof(val); i++; }
        else if (!strcmp(s, "--output") && val) { a.out_sel = atoi(val); output_given = true; i++; }
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
        else if (!strcmp(s, "--sound") && val) {        /* for this run only (the switches in the window are remembered) */
            if (!strcmp(val, "none") || !strcmp(val, "off")) a.snd_system = a.snd_mic = false;
            else if (!strcmp(val, "system")) { a.snd_system = true; a.snd_mic = false; }
            else if (!strcmp(val, "mic")) { a.snd_system = false; a.snd_mic = true; }
            else if (!strcmp(val, "both") || !strcmp(val, "on")) a.snd_system = a.snd_mic = true;
            else { usage(); return 2; }
            i++;
        }
        else if (!strcmp(s, "--fps") && val) {
            a.fps_sel = fps_index(atoi(val));
            i++;
        } else {
            usage();
            return 2;
        }
    }

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
    for (int i = 0; !output_given && i < a.nouts; i++)      /* the screen chosen last time, when it is there */
        if (!strcmp(a.outs[i].name, a.pref.output))
            a.out_sel = i;
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
    wlrail_init(&a.ran);
    wlwin_set_inset(&a.win, WLRAIL_W);                  /* (after the connect, which clears the window; before the open: it makes room) */
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
        struct wlrail L;
        record_rail(&a, &L);
        timeout = wlrail_loop(&a.win, &a.ran, &L, a.hovertool, timeout);
        if (!wlwin_step(&a.win, timeout))
            break;
        tick(&a);
    }
    stop_recording(&a, false);
    if (want_start && a.msg[0])
        rc = 1;
    wlwin_destroy(&a.win);
    return rc;
}
