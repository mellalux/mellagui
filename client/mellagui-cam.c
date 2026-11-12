/*
 * mellagui-cam - webcam viewer for MellaGUI: live picture from a V4L2 camera (UVC webcams), snapshots as PNG.
 *
 *   mellagui-cam [--device /dev/videoN] [--size WxH]      the live window
 *   mellagui-cam --list                                   one line per camera: DEVICE|name|formats|sizes
 *   mellagui-cam --snapshot FILE.png [--device D] [--size WxH]   take one picture without a window (after a short warm-up)
 *   mellagui-cam --clip FILE.mp4 [--seconds N] [--mic|--no-mic] [--device D] [--size WxH]   record a clip without a window
 *   mellagui-cam --selftest
 *
 * Keys: Space / Enter = snapshot, R = record a clip (again = stop), A = the microphone in the clip on / off, M = mirror the preview, P = pause,
 * S = next picture size, C = next camera, Esc / Q = quit.
 * Snapshots go to $MELLAGUI_CAM_DIR (default ~/Pictures) as camera-YYYYMMDD-HHMMSS.png, the picture as the camera sees it
 * (not mirrored); mellagui-view finds them there.
 *
 * Clips: H.264 + AAC in an MP4 (`camera-YYYYMMDD-HHMMSS.mp4` in $MELLAGUI_CAM_VIDEO_DIR, default ~/Videos, mode 0600), made by ffmpeg from the pictures
 * (client/ffpipe.c: sent as they come, never waiting - a picture that x264 is too slow for is dropped) and, when the microphone switch is on, the default
 * microphone (ffmpeg reads it itself, so sound and picture share one clock). Without ffmpeg the clip is Motion JPEG with the microphone as a PCM sound
 * track (parec; src/jpegenc.c, src/mp4.c, client/audiocap.c): bigger files, played by ffmpeg / mpv / VLC but not by browsers. The picture is not
 * mirrored. The switch is kept in camera.conf (microphone = 1; on by default).
 *
 * Formats: MJPEG (preferred: a webcam on a shared USB hub often cannot carry raw video) decoded by src/jpeg.c, whose
 * standard Huffman tables serve the frames a camera sends without a DHT segment, and YUYV. The camera is looked for again
 * once a second when there is none or it was unplugged. Env MELLAGUI_CAM_FAKE=1 replaces the hardware by a moving test
 * pattern (tests).
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "font.h"
#include "audiocap.h"
#include "ffpipe.h"
#include "img.h"
#include "jpegenc.h"
#include "keymap.h"
#include "mp4.h"
#include "png.h"
#include "protocol.h"
#include "sndconf.h"
#include "theme.h"
#include "uikit.h"
#include "util.h"
#include "wlwin.h"

#define NBUF 4
#define MAX_SIZES 24
#define MAX_DEVS 16
#define BAR_H 44
#define BTN_H 28
#define WARMUP_FRAMES 8

struct size { int w, h; };

struct source {
    bool fake;
    int fd;
    char dev[64], card[40];
    void *map[NBUF];
    size_t maplen[NBUF];
    int nbuf;
    uint32_t fourcc;                    /* V4L2_PIX_FMT_MJPEG or _YUYV */
    int w, h;                           /* what the camera really delivers */
    size_t bpl;
    struct size sizes[MAX_SIZES];
    int nsizes;
    bool on;                            /* streaming */
    uint64_t fake_n;
};

enum { B_SNAP, B_REC, B_MIC, B_MIRROR, B_PAUSE, B_SIZE, B_CAM, N_B };

struct app {
    struct wlwin win;
    struct source src;
    struct lm_image img;                /* the newest picture, XRGB8888 */
    bool have;
    bool mirror, paused, fixed_dev;
    char want_dev[64];
    struct size want;                   /* wanted picture size (0 = the default) */
    char msg[200];                      /* shown instead of a picture */
    char note[200];                     /* "Saved ..." for a few seconds */
    uint64_t note_until, next_scan, next_fake, fps_t;
    int frames, fps, ndev;
    int hover;
    double px, py;
    char outdir[PATH_MAX];
    char videodir[PATH_MAX];            /* where clips go */
    bool mic;                           /* the next clip has the microphone in it */
    struct clip *clip;                  /* the clip being recorded, NULL = none */
};

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do
        r = ioctl(fd, req, arg);
    while (r < 0 && errno == EINTR);
    return r;
}

static const char *fourcc_name(uint32_t f)
{
    return f == V4L2_PIX_FMT_MJPEG ? "MJPG" : f == V4L2_PIX_FMT_YUYV ? "YUYV" : "?";
}

/* ---- pictures -------------------------------------------------------------------------------------------------------- */

static uint8_t clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }

/* BT.601, limited range: what UVC cameras send as YUYV. */
static uint32_t yuv_px(int y, int u, int v)
{
    int c = y - 16, d = u - 128, e = v - 128;
    int r = (298 * c + 409 * e + 128) >> 8, g = (298 * c - 100 * d - 208 * e + 128) >> 8, b = (298 * c + 516 * d + 128) >> 8;
    return 0xff000000u | (uint32_t)clamp8(r) << 16 | (uint32_t)clamp8(g) << 8 | clamp8(b);
}

static void yuyv_to_xrgb(const uint8_t *src, size_t bpl, int w, int h, uint32_t *dst)
{
    for (int y = 0; y < h; y++) {
        const uint8_t *s = src + (size_t)y * bpl;
        uint32_t *d = dst + (size_t)y * (size_t)w;
        for (int x = 0; x + 1 < w; x += 2, s += 4) {
            d[x] = yuv_px(s[0], s[1], s[3]);
            d[x + 1] = yuv_px(s[2], s[1], s[3]);
        }
        if (w & 1)
            d[w - 1] = yuv_px(s[0], s[1], s[3]);
    }
}

static bool image_size(struct lm_image *img, int w, int h)
{
    if (img->px && img->w == w && img->h == h)
        return true;
    uint32_t *p = realloc(img->px, (size_t)w * (size_t)h * 4);
    if (!p)
        return false;
    img->px = p;
    img->w = w;
    img->h = h;
    return true;
}

/* The test pattern: colour bars above a gradient, a white square moving along the bars. */
static void fake_frame(struct lm_image *img, int w, int h, uint64_t n)
{
    static const uint32_t bars[7] = { 0xc0c0c0, 0xc0c000, 0x00c0c0, 0x00c000, 0xc000c0, 0xc00000, 0x0000c0 };
    int bs = LM_MAX(h / 10, 4), bx = (int)((n * 7) % (uint64_t)LM_MAX(w - bs, 1));
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t c = y < h * 2 / 3 ? bars[x * 7 / w] : (uint32_t)(x * 255 / w) * 0x010101u;
            if (y >= h / 4 && y < h / 4 + bs && x >= bx && x < bx + bs)
                c = 0xffffff;
            img->px[(size_t)y * (size_t)w + (size_t)x] = 0xff000000u | c;
        }
}

/* ---- the camera ------------------------------------------------------------------------------------------------------- */

static void src_stop(struct source *s)
{
    if (s->fd >= 0 && s->on) {
        int t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(s->fd, VIDIOC_STREAMOFF, &t);
    }
    for (int i = 0; i < s->nbuf; i++)
        if (s->map[i] && s->map[i] != MAP_FAILED)
            munmap(s->map[i], s->maplen[i]);
    if (s->fd >= 0 && s->nbuf) {
        struct v4l2_requestbuffers rb = { .count = 0, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
        xioctl(s->fd, VIDIOC_REQBUFS, &rb);
    }
    memset(s->map, 0, sizeof(s->map));
    s->nbuf = 0;
    s->on = false;
}

static void src_close(struct source *s)
{
    src_stop(s);
    if (s->fd >= 0)
        close(s->fd);
    s->fd = -1;
}

static void add_size(struct source *s, int w, int h)
{
    for (int i = 0; i < s->nsizes; i++)
        if (s->sizes[i].w == w && s->sizes[i].h == h)
            return;
    if (s->nsizes < MAX_SIZES)
        s->sizes[s->nsizes++] = (struct size){ w, h };
}

static int cmp_size(const void *a, const void *b)
{
    const struct size *x = a, *y = b;
    return x->w * x->h - y->w * y->h;
}

static bool has_format(int fd, uint32_t fourcc)
{
    for (unsigned i = 0;; i++) {
        struct v4l2_fmtdesc fd_ = { .index = i, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
        if (xioctl(fd, VIDIOC_ENUM_FMT, &fd_) < 0)
            return false;
        if (fd_.pixelformat == fourcc)
            return true;
    }
}

/* Open DEV (a capture device with streaming), pick MJPEG or YUYV and list the picture sizes. Returns 0 or -1 (errno-ish message in err). */
static int src_open(struct source *s, const char *dev, char *err, size_t en)
{
    memset(s, 0, sizeof(*s));
    s->fd = -1;
    snprintf(s->dev, sizeof(s->dev), "%s", dev);
    if (getenv("MELLAGUI_CAM_FAKE")) {
        s->fake = true;
        snprintf(s->dev, sizeof(s->dev), "fake");
        snprintf(s->card, sizeof(s->card), "Test pattern");
        s->fourcc = V4L2_PIX_FMT_MJPEG;
        add_size(s, 320, 240);
        add_size(s, 640, 480);
        return 0;
    }
    int fd = open(dev, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        snprintf(err, en, "cannot open %s: %s", dev, strerror(errno));
        return -1;
    }
    struct v4l2_capability cap;
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
        snprintf(err, en, "%s is not a V4L2 device", dev);
        close(fd);
        return -1;
    }
    uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
    if (!(caps & V4L2_CAP_VIDEO_CAPTURE) || !(caps & V4L2_CAP_STREAMING)) {
        snprintf(err, en, "%s cannot capture video", dev);
        close(fd);
        return -1;
    }
    snprintf(s->card, sizeof(s->card), "%s", (const char *)cap.card);
    s->fd = fd;
    s->fourcc = has_format(fd, V4L2_PIX_FMT_MJPEG) ? V4L2_PIX_FMT_MJPEG : has_format(fd, V4L2_PIX_FMT_YUYV) ? V4L2_PIX_FMT_YUYV : 0;
    if (!s->fourcc) {
        snprintf(err, en, "%s offers neither MJPEG nor YUYV", dev);
        src_close(s);
        return -1;
    }
    for (unsigned i = 0;; i++) {
        struct v4l2_frmsizeenum fs = { .index = i, .pixel_format = s->fourcc };
        if (xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fs) < 0)
            break;
        if (fs.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
            add_size(s, (int)fs.discrete.width, (int)fs.discrete.height);
        } else {                                                /* a range: the usual sizes inside it */
            static const struct size usual[] = { { 160, 120 }, { 320, 240 }, { 640, 480 }, { 800, 600 }, { 1280, 720 }, { 1920, 1080 } };
            for (size_t k = 0; k < sizeof(usual) / sizeof(usual[0]); k++)
                if ((uint32_t)usual[k].w >= fs.stepwise.min_width && (uint32_t)usual[k].w <= fs.stepwise.max_width &&
                    (uint32_t)usual[k].h >= fs.stepwise.min_height && (uint32_t)usual[k].h <= fs.stepwise.max_height)
                    add_size(s, usual[k].w, usual[k].h);
            break;
        }
    }
    if (!s->nsizes) {
        snprintf(err, en, "%s lists no picture sizes", dev);
        src_close(s);
        return -1;
    }
    qsort(s->sizes, (size_t)s->nsizes, sizeof(s->sizes[0]), cmp_size);
    return 0;
}

/* The size to start with: the wanted one, else 640x480 for MJPEG (320x240 for raw video, which a shared USB hub carries badly),
 * else the nearest in area. */
static struct size pick_size(const struct source *s, struct size want)
{
    if (want.w <= 0) {
        want = s->fourcc == V4L2_PIX_FMT_MJPEG ? (struct size){ 640, 480 } : (struct size){ 320, 240 };
    }
    struct size best = s->sizes[0];
    long bd = LONG_MAX;
    for (int i = 0; i < s->nsizes; i++) {
        long d = labs((long)s->sizes[i].w * s->sizes[i].h - (long)want.w * want.h);
        if (d < bd) {
            bd = d;
            best = s->sizes[i];
        }
    }
    return best;
}

static int src_start(struct source *s, struct size want, char *err, size_t en)
{
    src_stop(s);
    struct size sz = pick_size(s, want);
    if (s->fake) {
        s->w = sz.w;
        s->h = sz.h;
        s->on = true;
        return 0;
    }
    struct v4l2_format f = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
    f.fmt.pix.width = (uint32_t)sz.w;
    f.fmt.pix.height = (uint32_t)sz.h;
    f.fmt.pix.pixelformat = s->fourcc;
    f.fmt.pix.field = V4L2_FIELD_ANY;
    if (xioctl(s->fd, VIDIOC_S_FMT, &f) < 0 || f.fmt.pix.pixelformat != s->fourcc) {
        snprintf(err, en, "%s: cannot set %dx%d %s", s->dev, sz.w, sz.h, fourcc_name(s->fourcc));
        return -1;
    }
    s->w = (int)f.fmt.pix.width;
    s->h = (int)f.fmt.pix.height;
    s->bpl = f.fmt.pix.bytesperline ? f.fmt.pix.bytesperline : (size_t)s->w * 2;
    struct v4l2_streamparm sp = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
    sp.parm.capture.timeperframe.numerator = 1;
    sp.parm.capture.timeperframe.denominator = 30;
    xioctl(s->fd, VIDIOC_S_PARM, &sp);                          /* best effort */
    struct v4l2_requestbuffers rb = { .count = NBUF, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
    if (xioctl(s->fd, VIDIOC_REQBUFS, &rb) < 0 || rb.count < 2) {
        snprintf(err, en, "%s: no buffers (%s)", s->dev, strerror(errno));
        return -1;
    }
    for (unsigned i = 0; i < rb.count && i < NBUF; i++) {
        struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP, .index = i };
        if (xioctl(s->fd, VIDIOC_QUERYBUF, &b) < 0)
            break;
        void *m = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, s->fd, (off_t)b.m.offset);
        if (m == MAP_FAILED)
            break;
        s->map[i] = m;
        s->maplen[i] = b.length;
        s->nbuf = (int)i + 1;
        if (xioctl(s->fd, VIDIOC_QBUF, &b) < 0)
            break;
    }
    if (s->nbuf < 2) {
        snprintf(err, en, "%s: cannot map the buffers", s->dev);
        src_stop(s);
        return -1;
    }
    int t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(s->fd, VIDIOC_STREAMON, &t) < 0) {
        snprintf(err, en, "%s: cannot start the stream: %s", s->dev, strerror(errno));
        src_stop(s);
        return -1;
    }
    s->on = true;
    return 0;
}

/* One new picture into img: 1 = new picture, 0 = nothing (yet) or a damaged frame, -1 = the camera is gone. decode = false drops the frames. */
static int src_frame(struct source *s, struct lm_image *img, bool decode)
{
    if (s->fake) {
        if (decode && image_size(img, s->w, s->h))
            fake_frame(img, s->w, s->h, s->fake_n++);
        return decode ? 1 : 0;
    }
    struct v4l2_buffer held = { 0 };
    bool have_held = false;
    for (;;) {                                                  /* the newest frame counts, older ones go straight back */
        struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
        if (xioctl(s->fd, VIDIOC_DQBUF, &b) < 0) {
            if (errno == EAGAIN)
                break;
            if (have_held)
                xioctl(s->fd, VIDIOC_QBUF, &held);
            return -1;
        }
        if (have_held)
            xioctl(s->fd, VIDIOC_QBUF, &held);
        held = b;
        have_held = true;
    }
    if (!have_held)
        return 0;
    int rc = 0;
    size_t used = LM_MIN((size_t)held.bytesused, s->maplen[held.index]);
    if (decode && !(held.flags & V4L2_BUF_FLAG_ERROR) && used > 0) {
        if (s->fourcc == V4L2_PIX_FMT_MJPEG) {
            struct lm_image tmp = { 0 };
            char err[120];
            if (lm_jpeg_decode(s->map[held.index], used, &tmp, err, sizeof(err)) == 0) {
                free(img->px);
                *img = tmp;
                rc = 1;
            }
        } else if (used >= s->bpl * (size_t)s->h && image_size(img, s->w, s->h)) {
            yuyv_to_xrgb(s->map[held.index], s->bpl, s->w, s->h, img->px);
            rc = 1;
        }
    }
    xioctl(s->fd, VIDIOC_QBUF, &held);
    return rc;
}

/* The capture devices, in order: names like /dev/video0. */
static int scan_devices(char devs[][64], int max)
{
    int n = 0;
    if (getenv("MELLAGUI_CAM_FAKE")) {
        snprintf(devs[0], 64, "fake");
        return 1;
    }
    for (int i = 0; i < 64 && n < max; i++) {
        char p[64];
        snprintf(p, sizeof(p), "/dev/video%d", i);
        int fd = open(p, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;
        struct v4l2_capability cap;
        if (xioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
            uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
            if ((caps & V4L2_CAP_VIDEO_CAPTURE) && (caps & V4L2_CAP_STREAMING) &&
                (has_format(fd, V4L2_PIX_FMT_MJPEG) || has_format(fd, V4L2_PIX_FMT_YUYV)))
                snprintf(devs[n++], 64, "%s", p);
        }
        close(fd);
    }
    return n;
}

/* ---- snapshots --------------------------------------------------------------------------------------------------------- */

static bool write_png(const char *path, const struct lm_image *img, bool excl)
{
    size_t len = 0;
    uint8_t *png = lm_png_encode_xrgb(img->px, img->w, img->h, (size_t)img->w * 4, &len);
    if (!png)
        return false;
    int fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC | (excl ? O_EXCL : O_TRUNC), 0644);
    bool ok = fd >= 0;
    for (size_t off = 0; ok && off < len;) {
        ssize_t w = write(fd, png + off, len - off);
        if (w <= 0)
            ok = false;
        else
            off += (size_t)w;
    }
    if (fd >= 0)
        close(fd);
    free(png);
    return ok;
}

/* camera-YYYYMMDD-HHMMSS[-n].png in outdir; the name (without the directory) goes to name. */
static bool save_snapshot(struct app *a, char *name, size_t nsz)
{
    if (!a->have || !a->img.px)
        return false;
    if (mkdir(a->outdir, 0755) < 0 && errno != EEXIST)
        return false;
    time_t t = time(NULL);
    struct tm tm;
    char stamp[32], path[PATH_MAX + 80];
    localtime_r(&t, &tm);
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
    for (int n = 0; n < 100; n++) {
        if (n)
            snprintf(name, nsz, "camera-%s-%d.png", stamp, n);
        else
            snprintf(name, nsz, "camera-%s.png", stamp);
        snprintf(path, sizeof(path), "%s/%s", a->outdir, name);
        if (access(path, F_OK) == 0)
            continue;
        return write_png(path, &a->img, true);
    }
    return false;
}

/* ---- clips -------------------------------------------------------------------------------------------------------------- */

/* Two ways to make the file: with ffmpeg (H.264 + AAC, ffmpeg reads the microphone itself) or, when there is none, with our own writers: Motion JPEG
 * (src/jpegenc.c) and the microphone as a PCM sound track (parec, client/audiocap.c; src/mp4.c). */
struct clip {
    struct ffpipe *ff;                  /* ffmpeg ... */
    struct lm_mp4 *mp4;                 /* ... or our own file writer */
    struct audiocap *acap;
    char path[PATH_MAX + 80];
    int w, h;
    uint64_t t0;
    uint32_t frames, dropped;
    bool sound;
};

/* Opens the file: `path` as given (an old file is replaced) or the next free camera-<time>.mp4 in dir. The file descriptor goes to *fd (ffmpeg fills the
 * file itself) or the writer to *mp4. */
static bool clip_open_file(struct clip *c, const char *dir, const char *path, bool native, char *err, size_t en)
{
    time_t t = time(NULL);
    struct tm tm;
    char stamp[32];
    localtime_r(&t, &tm);
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
    if (!path && mkdir(dir, 0755) < 0 && errno != EEXIST) {
        snprintf(err, en, "cannot create %s: %s", dir, strerror(errno));
        return false;
    }
    for (int n = 0; n < 100; n++) {
        if (path)
            snprintf(c->path, sizeof(c->path), "%s", path);
        else if (n)
            snprintf(c->path, sizeof(c->path), "%s/camera-%s-%d.mp4", dir, stamp, n);
        else
            snprintf(c->path, sizeof(c->path), "%s/camera-%s.mp4", dir, stamp);
        if (path)
            unlink(c->path);                                    /* an explicit name: the old file is replaced */
        bool ok;
        if (native) {
            c->mp4 = lm_mp4_create(c->path, c->w, c->h);
            ok = c->mp4 != NULL;
        } else {
            int fd = open(c->path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
            ok = fd >= 0;
            if (ok)
                close(fd);
        }
        if (ok)
            return true;
        if (errno != EEXIST || path) {
            snprintf(err, en, "cannot write %.120s: %s", c->path, strerror(errno));
            return false;
        }
    }
    snprintf(err, en, "no free file name in %s", dir);
    return false;
}

/* Starts a clip of the pictures that are coming, of the size of the newest one. `path` = the file (NULL: camera-<time>.mp4 in dir). NULL with err. */
static struct clip *clip_start(const struct lm_image *img, const char *dir, const char *path, bool mic, char *err, size_t en)
{
    if (!img || !img->px || img->w < 16 || img->h < 16) {
        snprintf(err, en, "no picture from the camera yet");
        return NULL;
    }
    struct clip *c = calloc(1, sizeof(*c));
    if (!c) {
        snprintf(err, en, "out of memory");
        return NULL;
    }
    c->w = img->w;
    c->h = img->h;
    bool native = !ffpipe_available();
    if (!clip_open_file(c, dir, path, native, err, en)) {
        free(c);
        return NULL;
    }
    const char *mic_src = mic ? ffpipe_audio_source(true) : NULL;
    if (!native) {
        struct ffpipe_audio au = { NULL, mic_src };
        c->sound = au.mic != NULL;
        c->ff = ffpipe_start(c->path, c->w, c->h, 23, &au, err, en);
        if (!c->ff) {
            unlink(c->path);
            free(c);
            return NULL;
        }
    } else {
        lm_jpeg_encode_init();
        if (mic_src) {
            char why[160];
            c->acap = audiocap_start(NULL, mic_src, why, sizeof(why));
            if (c->acap && lm_mp4_set_audio(c->mp4, AUDIOCAP_RATE, AUDIOCAP_CHANNELS) == 0) {
                c->sound = true;
            } else {
                fprintf(stderr, "mellagui-cam: no sound in this clip: %s\n", c->acap ? "cannot add a sound track" : why);
                audiocap_stop(c->acap);
                c->acap = NULL;
            }
        }
    }
    c->t0 = now_ms();
    return c;
}

/* Our own writers: the sound that parec has delivered so far goes into the file. */
static void clip_pump(struct clip *c)
{
    if (!c->acap || !c->mp4)
        return;
    static int16_t buf[4096 * AUDIOCAP_CHANNELS];
    for (;;) {
        size_t n = audiocap_read(c->acap, buf, 4096);
        if (!n)
            break;
        uint64_t first = audiocap_first_ms(c->acap);
        if (lm_mp4_add_audio(c->mp4, buf, n, first > c->t0 ? first - c->t0 : 0) < 0)
            break;
    }
}

/* A new picture: sent when ffmpeg has room, else dropped (ffmpeg); coded as a JPEG and added (our own writers). False when ffmpeg is gone / the disk is full. */
static bool clip_frame(struct clip *c, const struct lm_image *img)
{
    if (!img->px || img->w != c->w || img->h != c->h)
        return true;
    if (c->mp4) {
        size_t len = 0;
        uint8_t *jpeg = lm_jpeg_encode_xrgb(img->px, c->w, c->h, (size_t)c->w * 4, 80, &len);
        int rc = jpeg ? lm_mp4_add(c->mp4, jpeg, len, now_ms() - c->t0) : -1;
        free(jpeg);
        clip_pump(c);
        if (rc == 0)
            c->frames++;
        return rc == 0;
    }
    int r = ffpipe_offer(c->ff, img->px, (size_t)c->w * (size_t)c->h * 4);
    if (r == 0)
        c->frames++;
    else if (r > 0)
        c->dropped++;
    return r >= 0;
}

/* Ends the clip: ffmpeg gets the newest picture once more (it defines where the file ends) and finishes the file; our writer writes its index. True when
 * there is a file. */
static bool clip_stop(struct clip *c, const struct lm_image *last, uint32_t *frames)
{
    *frames = c->frames;
    bool ok;
    if (c->mp4) {
        clip_pump(c);
        audiocap_stop(c->acap);
        c->acap = NULL;
        ok = lm_mp4_close(c->mp4, now_ms() - c->t0) == 0;
        c->mp4 = NULL;
        return ok && c->frames > 0;
    }
    if (last && last->px && last->w == c->w && last->h == c->h)
        ffpipe_write(c->ff, last->px, (size_t)c->w * (size_t)c->h * 4);
    int rc = ffpipe_finish(c->ff);
    c->ff = NULL;
    struct stat st;
    ok = rc == 0 && c->frames > 0 && stat(c->path, &st) == 0 && st.st_size > 0;
    if (!ok)
        unlink(c->path);
    return ok;
}

/* ---- the window ----------------------------------------------------------------------------------------------------------- */

#define BG (lm_theme.background)

static const char *btn_label(const struct app *a, int i, char *buf, size_t n)
{
    switch (i) {
    case B_SNAP: return "Snapshot";
    case B_REC: return a->clip ? "Stop" : "Record";
    case B_MIC: return a->mic ? "Mic on" : "Mic off";
    case B_MIRROR: return "Mirror";
    case B_PAUSE: return a->paused ? "Resume" : "Pause";
    case B_SIZE: snprintf(buf, n, "%dx%d", a->src.w ? a->src.w : 0, a->src.h ? a->src.h : 0); return buf;
    default: return "Camera";
    }
}

static bool btn_visible(const struct app *a, int i)
{
    if (i == B_CAM)
        return a->ndev > 1;
    if (i == B_SIZE)
        return a->src.nsizes > 0;
    return true;
}

static bool btn_rect(const struct app *a, int i, int *x, int *y, int *w, int *h)
{
    int bx = 10;
    char tmp[40];
    for (int k = 0; k < N_B; k++) {
        if (!btn_visible(a, k))
            continue;
        int bw = (int)strlen(btn_label(a, k, tmp, sizeof(tmp))) * LM_FONT_W + 28;
        if (k == i) {
            *x = bx;
            *y = a->win.bh - BAR_H + (BAR_H - BTN_H) / 2;
            *w = bw;
            *h = BTN_H;
            return true;
        }
        bx += bw + 6;
    }
    return false;
}

static int btn_at(const struct app *a, double px, double py)
{
    for (int i = 0; i < N_B; i++) {
        int x, y, w, h;
        if (btn_rect(a, i, &x, &y, &w, &h) && px >= x && px < x + w && py >= y && py < y + h)
            return i;
    }
    return -1;
}

static void paint_picture(struct app *a, int vw, int vh)
{
    struct wlwin *w = &a->win;
    const struct lm_image *im = &a->img;
    int k = wlwin_scale(w);                                          /* the picture is made for the buffer's own pixels: sharp at any scale */
    int pvw = vw * k, pvh = vh * k;
    double s = LM_MIN((double)pvw / im->w, (double)pvh / im->h);
    int dw = LM_MAX((int)(im->w * s), 1), dh = LM_MAX((int)(im->h * s), 1);
    int ox = (pvw - dw) / 2, oy = (pvh - dh) / 2;
    int *xi = malloc((size_t)dw * 2 * sizeof(int));                  /* per column: left source pixel and weight (0..256) */
    if (!xi)
        return;
    for (int x = 0; x < dw; x++) {
        double sx = ((a->mirror ? dw - 1 - x : x) + 0.5) / s - 0.5;
        sx = LM_CLAMP(sx, 0.0, (double)(im->w - 1));
        int x0 = (int)sx;
        xi[2 * x] = x0;
        xi[2 * x + 1] = (int)((sx - x0) * 256.0);
    }
    for (int y = 0; y < dh; y++) {
        if (oy + y < 0 || oy + y >= w->ph)
            continue;
        double sy = LM_CLAMP((y + 0.5) / s - 0.5, 0.0, (double)(im->h - 1));
        int y0 = (int)sy, y1 = LM_MIN(y0 + 1, im->h - 1), fy = (int)((sy - y0) * 256.0);
        const uint32_t *r0 = im->px + (size_t)y0 * (size_t)im->w, *r1 = im->px + (size_t)y1 * (size_t)im->w;
        uint32_t *dst = w->pixels + (size_t)(oy + y) * (size_t)w->pw + (size_t)ox;
        for (int x = 0; x < dw && ox + x < w->pw; x++) {
            int x0 = xi[2 * x], x1 = LM_MIN(x0 + 1, im->w - 1), fx = xi[2 * x + 1];
            uint32_t out = 0xff000000u;
            for (int sh = 0; sh <= 16; sh += 8) {
                int a0 = (int)((r0[x0] >> sh) & 255), a1 = (int)((r0[x1] >> sh) & 255);
                int b0 = (int)((r1[x0] >> sh) & 255), b1 = (int)((r1[x1] >> sh) & 255);
                int top = a0 * (256 - fx) + a1 * fx, bot = b0 * (256 - fx) + b1 * fx;
                out |= (uint32_t)((top * (256 - fy) + bot * fy) >> 16) << sh;
            }
            dst[x] = out;
        }
    }
    free(xi);
}

static void paint(struct wlwin *w)
{
    struct app *a = w->data;
    int vw = w->bw, vh = w->bh > BAR_H ? w->bh - BAR_H : 1;
    wlwin_fill(w, 0, 0, w->bw, w->bh, BG);
    if (a->have && a->img.px)
        paint_picture(a, vw, vh);
    else
        wlwin_text(w, 16, 16, a->msg[0] ? a->msg : "Waiting for the camera...", 1, lm_theme.text_soft, vw - 32);
    wlwin_fill(w, 0, vh, w->bw, w->bh - vh, lm_theme.bar);
    for (int i = 0; i < N_B; i++) {
        int x, y, bw, bh;
        char tmp[40];
        if (!btn_rect(a, i, &x, &y, &bw, &bh))
            continue;
        bool on = (i == B_MIRROR && a->mirror) || (i == B_PAUSE && a->paused) || (i == B_REC && a->clip) || (i == B_MIC && a->mic);
        /* round colourful buttons like the terminal's and the editor's: lighter under the pointer, a switch that is on gets a white rim */
        const uint32_t base[N_B] = { lm_theme.btn_green, lm_theme.btn_red, lm_theme.btn_slate, lm_theme.btn_blue, lm_theme.btn_amber, lm_theme.btn_teal,
                                     lm_theme.btn_purple };
        const uint32_t lite[N_B] = { lm_theme.btn_green_lit, lm_theme.btn_red_lit, lm_theme.btn_slate_lit, lm_theme.btn_blue_lit, lm_theme.btn_amber_lit,
                                     lm_theme.btn_teal_lit, lm_theme.btn_purple_lit };
        if (on)
            wlwin_round_fill(w, x - 1, y - 1, bw + 2, bh + 2, lm_button_radius((bh + 2) / 2), lm_theme.on_colour);
        wlwin_round_fill(w, x, y, bw, bh, lm_button_radius(bh / 2), on || a->hover == i ? lite[i] : base[i]);
        const char *t = btn_label(a, i, tmp, sizeof(tmp));
        wlwin_text(w, x + (bw - (int)strlen(t) * LM_FONT_W) / 2, y + (bh - LM_FONT_H) / 2, t, 1, lm_theme.on_colour, 0);
    }
    char st[240];
    if (a->clip) {
        uint64_t ms = now_ms() - a->clip->t0, s = ms / 1000;
        char tag[24];                                           /* in the corner of the picture, where it is seen however narrow the window is */
        snprintf(tag, sizeof(tag), "REC %02u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
        int tw = (int)strlen(tag) * LM_FONT_W;
        wlwin_round_fill(w, 8, 8, tw + 40, 24, 12, 0x000000);
        if ((ms / 500) % 2 == 0)
            wlwin_round_fill(w, 16, 14, 12, 12, 6, lm_theme.alert);
        wlwin_text(w, 34, 14, tag, 1, 0xffffff, 0);
        snprintf(st, sizeof(st), "REC %02u:%02u  %u pictures%s", (unsigned)(s / 60), (unsigned)(s % 60), a->clip->frames, a->clip->sound ? "  with microphone" : "");
    } else if (a->note[0] && now_ms() < a->note_until)
        snprintf(st, sizeof(st), "%s", a->note);
    else if (a->src.on)
        snprintf(st, sizeof(st), "%s  %dx%d %s  %d fps%s", a->src.card, a->src.w, a->src.h, a->src.fake ? "test" : fourcc_name(a->src.fourcc),
                 a->paused ? 0 : a->fps, a->paused ? "  paused" : "");
    else
        st[0] = 0;
    int last = 0;
    for (int i = 0; i < N_B; i++) {
        int x, y, bw, bh;
        if (btn_rect(a, i, &x, &y, &bw, &bh))
            last = x + bw;
    }
    int tw = (int)strlen(st) * LM_FONT_W;
    if (last + 16 + tw <= w->bw)
        wlwin_text(w, w->bw - tw - 10, vh + (BAR_H - LM_FONT_H) / 2, st, 1, lm_theme.muted, 0);
}

static void set_note(struct app *a, const char *fmt, const char *arg)
{
    snprintf(a->note, sizeof(a->note), fmt, arg);
    a->note_until = now_ms() + 4000;
    a->win.redraw = true;
}

static void do_snapshot(struct app *a)
{
    char name[96];
    if (save_snapshot(a, name, sizeof(name)))
        set_note(a, "Saved %s", name);
    else
        set_note(a, "Cannot save a picture (%s)", a->have ? "see the folder" : "no picture yet");
}

static void do_record(struct app *a)
{
    if (a->clip) {
        uint32_t frames = 0;
        char path[sizeof(a->clip->path)];
        snprintf(path, sizeof(path), "%s", a->clip->path);
        bool ok = clip_stop(a->clip, a->have ? &a->img : NULL, &frames);
        free(a->clip);
        a->clip = NULL;
        if (ok) {
            const char *base = strrchr(path, '/');
            set_note(a, "Saved %s", base ? base + 1 : path);
            fprintf(stderr, "mellagui-cam: saved %s (%u pictures)\n", path, frames);
        } else {
            set_note(a, "The clip could not be saved (%s)", frames ? "ffmpeg failed" : "no pictures");
        }
        return;
    }
    char err[200] = "";
    if (!a->src.on || !a->have) {
        set_note(a, "%s", "No picture to record yet");
        return;
    }
    a->clip = clip_start(&a->img, a->videodir, NULL, a->mic, err, sizeof(err));
    if (!a->clip)
        set_note(a, "%s", err);
    else
        fprintf(stderr, "mellagui-cam: recording %s%s\n", a->clip->path, a->clip->sound ? " (with microphone)" : "");
}

static void start_stream(struct app *a)
{
    char err[200] = "";
    if (src_start(&a->src, a->want, err, sizeof(err)) < 0) {
        snprintf(a->msg, sizeof(a->msg), "%s", err);
        a->next_scan = now_ms() + 2000;
        src_close(&a->src);
    } else {
        a->msg[0] = 0;
        a->have = false;
        a->frames = 0;
        a->fps_t = now_ms();
        a->next_fake = 0;
    }
    a->win.redraw = true;
}

static void open_camera(struct app *a, const char *dev)
{
    char err[200] = "";
    src_close(&a->src);
    if (src_open(&a->src, dev, err, sizeof(err)) < 0) {
        snprintf(a->msg, sizeof(a->msg), "%s", err);
        a->next_scan = now_ms() + 2000;
        a->src.fd = -1;
        a->win.redraw = true;
        return;
    }
    start_stream(a);
}

static void next_camera(struct app *a)
{
    char devs[MAX_DEVS][64];
    int n = scan_devices(devs, MAX_DEVS);
    a->ndev = n;
    if (n < 2)
        return;
    int cur = 0;
    for (int i = 0; i < n; i++)
        if (!strcmp(devs[i], a->src.dev))
            cur = i;
    snprintf(a->want_dev, sizeof(a->want_dev), "%s", devs[(cur + 1) % n]);
    a->want = (struct size){ 0, 0 };
    open_camera(a, a->want_dev);
}

static void next_size(struct app *a)
{
    if (a->src.nsizes < 2 || !a->src.on)
        return;
    int cur = 0;
    for (int i = 0; i < a->src.nsizes; i++)
        if (a->src.sizes[i].w == a->src.w && a->src.sizes[i].h == a->src.h)
            cur = i;
    a->want = a->src.sizes[(cur + 1) % a->src.nsizes];
    start_stream(a);
}

static void press(struct app *a, int b)
{
    switch (b) {
    case B_SNAP: do_snapshot(a); break;
    case B_REC: do_record(a); break;
    case B_MIC:
        if (!a->clip) {                                         /* (a clip that runs keeps what it started with) */
            a->mic = !a->mic;
            bool sys = false;
            lm_snd_conf_save("camera", sys, a->mic);
        }
        break;
    case B_MIRROR: a->mirror = !a->mirror; break;
    case B_PAUSE:
        if (!a->clip)                                           /* a clip is made of the live pictures */
            a->paused = !a->paused;
        break;
    case B_SIZE: if (!a->clip) next_size(a); break;
    case B_CAM: if (!a->clip) next_camera(a); break;
    }
    a->win.redraw = true;
}

static void pointer_event(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
    struct app *a = w->data;
    int old = a->hover;
    switch (op) {
    case LM_WL_POINTER_EVT_ENTER:
        a->px = lm_fixed_to_double(args[2].f);
        a->py = lm_fixed_to_double(args[3].f);
        a->hover = btn_at(a, a->px, a->py);
        break;
    case LM_WL_POINTER_EVT_LEAVE:
        a->hover = -1;
        break;
    case LM_WL_POINTER_EVT_MOTION:
        a->px = lm_fixed_to_double(args[1].f);
        a->py = lm_fixed_to_double(args[2].f);
        a->hover = btn_at(a, a->px, a->py);
        break;
    case LM_WL_POINTER_EVT_BUTTON:
        if (args[2].u == 272 && args[3].u == 1) {               /* BTN_LEFT pressed */
            int b = btn_at(a, a->px, a->py);
            if (b >= 0)
                press(a, b);
        }
        break;
    }
    if (a->hover != old)
        w->redraw = true;
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
    if (ks == LM_KS_Escape || c == 'q' || c == 'Q')
        w->running = false;
    else if (ks == LM_KS_Return || ks == LM_KS_KP_Enter || c == ' ')
        press(a, B_SNAP);
    else if (c == 'r' || c == 'R')
        press(a, B_REC);
    else if (c == 'a' || c == 'A')
        press(a, B_MIC);
    else if (c == 'm' || c == 'M')
        press(a, B_MIRROR);
    else if (c == 'p' || c == 'P')
        press(a, B_PAUSE);
    else if (c == 's' || c == 'S')
        press(a, B_SIZE);
    else if (c == 'c' || c == 'C')
        press(a, B_CAM);
}

/* Look for the camera (none yet, or it was unplugged). */
static void find_camera(struct app *a)
{
    char devs[MAX_DEVS][64];
    a->ndev = scan_devices(devs, MAX_DEVS);
    if (a->fixed_dev) {
        open_camera(a, a->want_dev);
        return;
    }
    if (a->ndev == 0) {
        snprintf(a->msg, sizeof(a->msg), "No camera found (looked at /dev/video*). Plug one in; this window looks again by itself.");
        a->next_scan = now_ms() + 1000;
        a->win.redraw = true;
        return;
    }
    open_camera(a, devs[0]);
}

static void pull_frames(struct app *a)
{
    int r = src_frame(&a->src, &a->img, !a->paused);
    if (r < 0) {
        if (a->clip)                                            /* the camera went away: what was recorded is kept */
            do_record(a);
        src_close(&a->src);
        a->have = false;
        snprintf(a->msg, sizeof(a->msg), "The camera went away. Looking for it again...");
        a->next_scan = now_ms() + 1000;
        a->win.redraw = true;
    } else if (r > 0) {
        a->have = true;
        a->frames++;
        a->win.redraw = true;
        if (a->clip && !clip_frame(a->clip, &a->img)) {         /* ffmpeg died: say so and end the clip */
            do_record(a);
            set_note(a, "%s", "ffmpeg stopped: the clip is lost (is the sound server running? try Mic off)");
        }
    }
}

static int run_window(struct app *a)
{
    static const struct wlwin_ops ops = { .paint = paint, .pointer = pointer_event, .keyboard = keyboard_event };
    if (wlwin_connect(&a->win, "mellagui-cam", &ops, a) < 0)
        return 1;
    if (wlwin_open(&a->win, "Camera", "mellagui.cam", 680, 560, 360, 240) < 0)
        return 1;
    a->mirror = true;
    a->hover = -1;
    find_camera(a);
    while (a->win.running) {
        if (!wlwin_step(&a->win, 0))
            break;
        uint64_t t = now_ms();
        if (t - a->fps_t >= 1000) {
            a->fps = (int)((uint64_t)a->frames * 1000 / (t - a->fps_t));
            a->frames = 0;
            a->fps_t = t;
            a->win.redraw = true;
        }
        if (a->note[0] && t >= a->note_until) {
            a->note[0] = 0;
            a->win.redraw = true;
        }
        int timeout = 250;
        if (!a->src.on) {
            if (t >= a->next_scan) {
                find_camera(a);
                continue;
            }
            timeout = (int)LM_MIN(a->next_scan - t, 250);
        } else if (a->src.fake) {
            if (t >= a->next_fake) {
                a->next_fake = t + 66;
                pull_frames(a);
                continue;
            }
            timeout = (int)LM_MIN(a->next_fake - t, 250);
        }
        struct pollfd pf[2] = { { .fd = wlc_fd(a->win.c), .events = POLLIN }, { .fd = a->src.fd, .events = POLLIN } };
        int n = poll(pf, a->src.on && !a->src.fake ? 2 : 1, LM_MIN(timeout, a->clip && a->clip->acap ? 50 : timeout));
        if (n > 0 && a->src.on && !a->src.fake && (pf[1].revents & (POLLIN | POLLERR | POLLHUP)))
            pull_frames(a);
        if (a->clip)
            clip_pump(a->clip);                                  /* (our own writers: the sound is written as it comes) */
    }
    if (a->clip)
        do_record(a);                                           /* the window was closed while recording: the file is finished */
    src_close(&a->src);
    wlwin_destroy(&a->win);
    return 0;
}

/* ---- command line ----------------------------------------------------------------------------------------------------- */

static bool parse_size(const char *s, struct size *out)
{
    int w, h;
    char junk;
    if (sscanf(s, "%dx%d%c", &w, &h, &junk) != 2 || w < 16 || h < 16 || w > 8192 || h > 8192)
        return false;
    *out = (struct size){ w, h };
    return true;
}

static int cmd_list(void)
{
    char devs[MAX_DEVS][64];
    int n = scan_devices(devs, MAX_DEVS);
    for (int i = 0; i < n; i++) {
        struct source s;
        char err[120];
        if (src_open(&s, devs[i], err, sizeof(err)) < 0)
            continue;
        printf("%s|%s|%s|", s.dev, s.card, fourcc_name(s.fourcc));
        for (int k = 0; k < s.nsizes; k++)
            printf("%s%dx%d", k ? "," : "", s.sizes[k].w, s.sizes[k].h);
        putchar('\n');
        src_close(&s);
    }
    return 0;
}

static int cmd_snapshot(const char *file, const char *dev, struct size want)
{
    struct app a = { 0 };
    char err[200] = "";
    char devs[MAX_DEVS][64];
    if (!dev) {
        if (scan_devices(devs, MAX_DEVS) == 0) {
            fprintf(stderr, "mellagui-cam: no camera found\n");
            return 1;
        }
        dev = devs[0];
    }
    if (src_open(&a.src, dev, err, sizeof(err)) < 0 || src_start(&a.src, want, err, sizeof(err)) < 0) {
        fprintf(stderr, "mellagui-cam: %s\n", err);
        return 1;
    }
    int got = 0;
    uint64_t t0 = now_ms();
    while (got < (a.src.fake ? 1 : WARMUP_FRAMES) && now_ms() - t0 < 15000) {        /* the first frames are dark: exposure settles */
        struct pollfd pf = { .fd = a.src.fd, .events = POLLIN };
        if (!a.src.fake && poll(&pf, 1, 500) <= 0)
            continue;
        int r = src_frame(&a.src, &a.img, true);
        if (r < 0)
            break;
        got += r > 0;
    }
    src_close(&a.src);
    if (!got) {
        fprintf(stderr, "mellagui-cam: no picture from %s\n", dev);
        return 1;
    }
    if (!write_png(file, &a.img, false)) {
        fprintf(stderr, "mellagui-cam: cannot write %s: %s\n", file, strerror(errno));
        return 1;
    }
    free(a.img.px);
    return 0;
}

/* A clip without a window: `seconds` of the camera into `file`. */
static int cmd_clip(const char *file, const char *dev, struct size want, double seconds, bool mic)
{
    struct app a = { 0 };
    char err[200] = "";
    char devs[MAX_DEVS][64];
    if (!dev) {
        if (scan_devices(devs, MAX_DEVS) == 0) {
            fprintf(stderr, "mellagui-cam: no camera found\n");
            return 1;
        }
        dev = devs[0];
    }
    if (src_open(&a.src, dev, err, sizeof(err)) < 0 || src_start(&a.src, want, err, sizeof(err)) < 0) {
        fprintf(stderr, "mellagui-cam: %s\n", err);
        return 1;
    }
    int got = 0;
    uint64_t t0 = now_ms();
    while (got < (a.src.fake ? 1 : WARMUP_FRAMES) && now_ms() - t0 < 15000) {
        struct pollfd pf = { .fd = a.src.fd, .events = POLLIN };
        if (!a.src.fake && poll(&pf, 1, 500) <= 0)
            continue;
        int r = src_frame(&a.src, &a.img, true);
        if (r < 0)
            break;
        got += r > 0;
    }
    if (!got) {
        fprintf(stderr, "mellagui-cam: no picture from %s\n", dev);
        src_close(&a.src);
        return 1;
    }
    struct clip *c = clip_start(&a.img, NULL, file, mic, err, sizeof(err));
    if (!c) {
        fprintf(stderr, "mellagui-cam: %s\n", err);
        src_close(&a.src);
        return 1;
    }
    uint64_t end = now_ms() + (uint64_t)(seconds * 1000.0);
    bool alive = true;
    while (alive && now_ms() < end) {
        struct pollfd pf = { .fd = a.src.fd, .events = POLLIN };
        if (a.src.fake) {
            usleep(66000);
        } else if (poll(&pf, 1, 200) <= 0) {
            continue;
        }
        int r = src_frame(&a.src, &a.img, true);
        if (r < 0)
            break;
        if (r > 0)
            alive = clip_frame(c, &a.img);
        clip_pump(c);
    }
    uint32_t frames = 0;
    bool ok = clip_stop(c, &a.img, &frames);
    free(c);
    src_close(&a.src);
    free(a.img.px);
    if (!ok) {
        fprintf(stderr, "mellagui-cam: the clip could not be saved\n");
        return 1;
    }
    printf("%s\n", file);
    return 0;
}

static int selftest(void)
{
    int bad = 0;
    uint8_t yuyv[8] = { 16, 128, 16, 128, 235, 128, 235, 128 };                      /* black black white white */
    uint32_t px[4];
    yuyv_to_xrgb(yuyv, 8, 4, 1, px);
    if (px[0] != 0xff000000u || px[1] != 0xff000000u || px[2] != 0xffffffffu || px[3] != 0xffffffffu) {
        fprintf(stderr, "selftest: YUYV black / white: %08x %08x %08x %08x\n", px[0], px[1], px[2], px[3]);
        bad++;
    }
    uint8_t red[4] = { 81, 90, 81, 240 };                                           /* BT.601 red */
    yuyv_to_xrgb(red, 4, 2, 1, px);
    if (((px[0] >> 16) & 255) < 250 || ((px[0] >> 8) & 255) > 6 || (px[0] & 255) > 6) {
        fprintf(stderr, "selftest: YUYV red: %08x\n", px[0]);
        bad++;
    }
    struct lm_image a = { 0 }, b = { 0 };
    image_size(&a, 64, 48);
    image_size(&b, 64, 48);
    fake_frame(&a, 64, 48, 0);
    fake_frame(&b, 64, 48, 5);
    if (!memcmp(a.px, b.px, 64 * 48 * 4)) {
        fprintf(stderr, "selftest: the test pattern does not move\n");
        bad++;
    }
    struct size sz;
    if (!parse_size("640x480", &sz) || sz.w != 640 || sz.h != 480 || parse_size("640", &sz) || parse_size("1x1", &sz) || parse_size("10x10x", &sz)) {
        fprintf(stderr, "selftest: size parsing\n");
        bad++;
    }
    free(a.px);
    free(b.px);
    puts(bad ? "selftest FAILED" : "selftest ok");
    return bad ? 1 : 0;
}

int main(int argc, char **argv)
{
    wlwin_hidpi();                      /* draws through the wlwin_* calls only: sharp on a scaled screen */
    lm_font_ui_cells(true);             /* this program lays its text out in columns of characters */
    lm_theme_load_env();
    const char *dev = NULL, *snap = NULL, *clip = NULL;
    struct size want = { 0, 0 };
    bool list = false, mic = true, sys_unused = false;
    double seconds = 5;
    lm_snd_conf_load("camera", &sys_unused, &mic);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--selftest"))
            return selftest();
        else if (!strcmp(argv[i], "--list"))
            list = true;
        else if (!strcmp(argv[i], "--device") && i + 1 < argc)
            dev = argv[++i];
        else if (!strcmp(argv[i], "--snapshot") && i + 1 < argc)
            snap = argv[++i];
        else if (!strcmp(argv[i], "--clip") && i + 1 < argc)
            clip = argv[++i];
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc)
            seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--mic"))
            mic = true;
        else if (!strcmp(argv[i], "--no-mic"))
            mic = false;
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) {
            if (!parse_size(argv[++i], &want)) {
                fprintf(stderr, "mellagui-cam: --size wants WIDTHxHEIGHT\n");
                return 2;
            }
        } else {
            fprintf(stderr, "usage: mellagui-cam [--device /dev/videoN] [--size WxH] [--mic|--no-mic] | --list | --snapshot FILE.png | --clip FILE.mp4 [--seconds N] | --selftest\n");
            return 2;
        }
    }
    if (list)
        return cmd_list();
    if (snap)
        return cmd_snapshot(snap, dev, want);
    if (clip)
        return cmd_clip(clip, dev, want, seconds, mic);

    static struct app a;                                         /* zero initialised */
    a.want = want;
    a.mic = mic;
    if (dev) {
        snprintf(a.want_dev, sizeof(a.want_dev), "%s", dev);
        a.fixed_dev = true;
    }
    const char *dir = getenv("MELLAGUI_CAM_DIR"), *home = getenv("HOME");
    if (dir && *dir)
        snprintf(a.outdir, sizeof(a.outdir), "%s", dir);
    else
        snprintf(a.outdir, sizeof(a.outdir), "%s/Pictures", home && *home ? home : ".");
    const char *vdir = getenv("MELLAGUI_CAM_VIDEO_DIR");
    if (vdir && *vdir)
        snprintf(a.videodir, sizeof(a.videodir), "%s", vdir);
    else
        snprintf(a.videodir, sizeof(a.videodir), "%s/Videos", home && *home ? home : ".");
    a.src.fd = -1;
    int rc = run_window(&a);
    free(a.img.px);
    return rc;
}
