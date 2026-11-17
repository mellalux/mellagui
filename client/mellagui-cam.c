/*
 * mellagui-cam - webcam viewer for MellaGUI: live picture from a V4L2 camera (UVC webcams), snapshots as PNG.
 *
 *   mellagui-cam [--device /dev/videoN] [--size WxH]      the live window
 *   mellagui-cam --list                                   one line per camera: DEVICE|name|formats|sizes
 *   mellagui-cam --snapshot FILE.png [--device D] [--size WxH]   take one picture without a window (after a short warm-up)
 *   mellagui-cam --clip FILE.mp4 [--seconds N] [--mic|--no-mic] [--device D] [--size WxH]   record a clip without a window
 *   mellagui-cam --effects                                the effects, one id a line
 *   mellagui-cam --snapshot FILE.png --effect ID [--focus X,Y]   ... with an effect on the picture (also for --clip)
 *   mellagui-cam --selftest
 *
 * Like Apple's Photo Booth: under the picture is a row of controls (never over it): Effects, a Photo / Video switch, a big red shutter and, for a
 * photo, "1-up / 4-up" and the countdown, for a video the microphone. The shutter takes a picture after a countdown of 3 (or 10) seconds - the seconds are
 * shown big over the picture, the screen flashes white when it is taken - and "4-up" takes four pictures one after the other into ONE picture of four
 * (2 x 2); in Video mode the shutter starts and stops a clip. "Effects" replaces the picture by a grid of 3 x 3 live previews of the effects
 * (client/camfx.c: black & white, sepia, pencil, comic book, pop art, thermal, x-ray, glow, vintage, mirror, kaleidoscope, light tunnel, squeeze, stretch,
 * bulge, dent, twirl), pages of nine; a click chooses one. The rail at the left has the camera's own things: mirror, pause, size, which camera.
 * The effect is in the live picture, the snapshots and the clips. The last page of the grid has BACKDROPS (sunset, space, neon, ghost): choosing one
 * asks you to step out of the picture, takes the empty scene after the countdown (the key B takes it again) and then keeps only what differs from it
 * - you - on the made-up backdrop. The countdown ticks (a higher tick for the last second) and the shutter clicks, sounds made in code
 * (client/camsound.c; O = sound on / off). The pictures are a column of small ones at the right of the picture (beside it, not over it; as many as fit,
 * the wheel shows the older ones; the ones already in the folder too): a click opens one in mellagui-view, the Folder button the folder in mellagui-files.
 * The effects of the second page of the grid (mirror, kaleidoscope, tunnel, squeeze, stretch, bulge, dent, twirl, vintage) have a point you move with
 * the mouse on the live picture (press or drag; --focus X,Y on the command line).
 *
 * Keys: Space / Enter = the shutter (again = cancel the countdown; a clip in video mode), V = photo / video, E = the effects, 4 = one picture / four-up, T = the countdown (none / 3 s / 10 s),
 * B = take the background again (backdrop effects), O = sound on / off,
 * R = record a clip (again = stop), A = the microphone in the clip on / off, M = mirror the preview, P = pause,
 * S = next picture size, C = next camera, Esc / Q = quit. In the grid: arrows / Enter / 1..9 choose, PgUp / PgDn the page, Esc closes it.
 * Snapshots go to $MELLAGUI_CAM_DIR (default ~/Camera) as camera-YYYYMMDD-HHMMSS.png, the picture as the camera sees it
 * (not mirrored); mellagui-view finds them there.
 *
 * Clips: H.264 + AAC in an MP4 (`camera-YYYYMMDD-HHMMSS.mp4` in $MELLAGUI_CAM_VIDEO_DIR, default the same folder as the pictures, mode 0600), made by ffmpeg from the pictures
 * (client/ffpipe.c: sent as they come, never waiting - a picture that x264 is too slow for is dropped) and, when the microphone switch is on, the default
 * microphone (ffmpeg reads it itself, so sound and picture share one clock). Without ffmpeg the clip is Motion JPEG with the microphone as a PCM sound
 * track (parec; src/jpegenc.c, src/mp4.c, client/audiocap.c): bigger files, played by ffmpeg / mpv / VLC but not by browsers. The picture is not
 * mirrored. The switches are kept in camera.conf (microphone = 1, on by default; timer = 3 seconds; four-up = 0; sound; video = 0; size = WxH, the resolution last chosen). $MELLAGUI_CAM_TIMER (seconds)
 * replaces the timer for one run (the tests use 0). $MELLAGUI_CAM_WINDOW=WxH is the window's first size. $MELLAGUI_CAM_DUMP=FILE writes the state and the places of the controls after every paint.
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
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "font.h"
#include "audiocap.h"
#include "camfx.h"
#include "camsound.h"
#include "ffpipe.h"
#include "img.h"
#include "jpegenc.h"
#include "keymap.h"
#include "mp4.h"
#include "png.h"
#include "protocol.h"
#include "sndconf.h"
#include "thumbbox.h"
#include "theme.h"
#include "uikit.h"
#include "util.h"
#include "wlrail.h"
#include "wlwin.h"

#define NBUF 4
#define MAX_SIZES 24
#define MAX_DEVS 16
/* The window: the live picture (with the strip column at its right), under it a row of controls (never over the picture), at the bottom a status line;
 * at the left a rail of round drops (wlrail.h) for the things that are about the camera: mirror, pause, size, which camera. */
#define STATUS_H 24
#define CTRL_H 70
#define BAR_H STATUS_H                  /* (the old name: the height of the status line) */
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

#define STRIP_MAX 200                   /* the pictures the strip knows (the newest ones of the folder) */
#define STRIP_H 96                      /* the height they are kept at (they are shown at most 48 high, squeezed to the buffer's pixels) */
#define STRIP_TW 64                     /* the room of a picture in the strip: 64 x 48 */
#define STRIP_TH 48
#define STRIP_COL 84                    /* the width of the column the strip has at the right of the picture */

struct thumb {
    struct lm_image img;                /* the small copy; px NULL until it was made (the ones of the folder are read when they come into view) */
    bool tried;                         /* it was read (or failed) */
    bool video;                         /* a clip (camera-*.mp4): its picture is a poster made by `mellagui-view --poster` */
    char path[PATH_MAX + 300];
};

enum { B_MIRROR, B_PAUSE, B_SIZE, B_CAM, N_B };                /* the rail */

/* The controls over the bottom of the picture, like Photo Booth's: the effects, the shutter, one picture / four, the countdown. */
enum { O_FX, O_KIND, O_SHUTTER, O_MODE, O_TIMER, O_MIC, N_O };       /* photo: fx kind shutter mode timer; video: fx kind shutter mic */
enum { PH_IDLE, PH_WAIT };                                  /* the shutter: nothing going on / waiting for the moment of the next picture */
#define FX_PER_PAGE 9
#define FX_PAGES ((CAMFX_COUNT + FX_PER_PAGE - 1) / FX_PER_PAGE)
#define FLASH_MS 380
#define FOURUP_GAP_MS 1100

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
    struct wlrail_anim ran;             /* the springs of the rail */
    bool menu_open;                     /* the list of sizes (the Size button's drop-down) is showing */
    int menu_hover, menu_top;           /* its highlighted entry and the first one that shows (a long list scrolls) */
    double px, py;
    char outdir[PATH_MAX];
    char videodir[PATH_MAX];            /* where clips go */
    bool mic;                           /* the next clip has the microphone in it */
    struct clip *clip;                  /* the clip being recorded, NULL = none */
    /* Photo Booth */
    int fx;                             /* the effect in use (CAMFX_*) */
    struct lm_image fxi;                /* the newest picture with the effect on it (used while fx is not normal) */
    bool fx_open;                       /* the grid of effects is showing instead of the picture */
    int fx_page, fx_hover;              /* its page, and the tile the pointer / the keys are on (-1 = none) */
    int ohover;                         /* the control under the pointer (O_*), -1 none */
    bool fourup;                        /* the shutter takes four pictures into one */
    bool video;                         /* the shutter records a clip (else it takes a picture) */
    struct size saved_size;             /* the resolution the user chose last: kept in camera.conf (size = WxH) */
    int timer_s;                        /* the countdown before a picture: 0, 3 or 10 seconds */
    int phase;                          /* PH_IDLE / PH_WAIT */
    uint64_t t_shot;                    /* the moment of the next picture */
    bool digits;                        /* the wait is the countdown (the seconds are shown), not the pause between the four pictures */
    int shot_n;                         /* pictures of the four-up taken so far */
    struct lm_image quad[4];            /* ... each at half size */
    uint64_t flash_t0;                  /* the white flash of a picture that was just taken (0 = none) */
    char dump[PATH_MAX];                /* $MELLAGUI_CAM_DUMP */
    struct lm_image ref;                /* the empty scene of the backdrop effects */
    bool have_ref;
    uint64_t bg_until;                  /* "step out of the picture": the moment the empty scene is taken (0 = not waiting) */
    bool sound;                         /* the countdown ticks and the shutter clicks */
    int last_tick;                      /* the second of the countdown that was last ticked */
    struct thumb strip[STRIP_MAX];      /* the newest picture first */
    int nstrip, strip_top, shover;      /* how many, the first one in view, and the one under the pointer (-1 none) */
    bool fhover;                        /* the pointer is on the strip's folder button */
    float focus_x, focus_y;             /* the point the effect works round (0..1 of the picture; the effect's own space, before the mirror) */
    bool focus_drag;                    /* the button is down on the picture: the point follows the pointer */
    bool pin;                           /* the pointer is in the window */
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

/* ---- the effect ------------------------------------------------------------------------------------------------------------ */

/* What is shown, saved and recorded: the camera's picture, with the effect on it unless that is Normal. */
static const struct lm_image *shown(const struct app *a)
{
    if (a->bg_until)                                            /* waiting for the empty scene: the picture as it is */
        return &a->img;
    return a->fx != CAMFX_NORMAL && a->fxi.px ? &a->fxi : &a->img;
}

/* Makes the picture with the effect from the newest camera picture (once per picture, not once per paint). */
static void fx_update(struct app *a)
{
    if (a->fx == CAMFX_NORMAL || !a->have || !a->img.px || a->bg_until || !image_size(&a->fxi, a->img.w, a->img.h))
        return;
    const uint32_t *ref = a->have_ref && a->ref.px && a->ref.w == a->img.w && a->ref.h == a->img.h ? a->ref.px : NULL;
    camfx_apply_at(a->fx, a->img.px, ref, a->img.w, a->img.h, a->fxi.px, a->focus_x, a->focus_y);
}

static void thumb_box(const uint32_t *s, int sw, int sh, uint32_t *d, int dw, int dh);

static bool is_clip_name(const char *path)
{
    size_t n = strlen(path);
    return n > 4 && !strcmp(path + n - 4, ".mp4");
}

/* The picture that was just saved goes first into the strip (a small copy; the file's name is kept to open it). */
static void strip_push(struct app *a, const char *path, const struct lm_image *img)
{
    if (!img || !img->px || img->w < 2 || img->h < 2)
        return;
    int tw = LM_CLAMP(img->w * STRIP_H / img->h, 16, 256);
    uint32_t *px = malloc((size_t)tw * STRIP_H * 4);
    if (!px)
        return;
    thumb_box(img->px, img->w, img->h, px, tw, STRIP_H);
    if (a->nstrip == STRIP_MAX)
        free(a->strip[STRIP_MAX - 1].img.px);
    memmove(&a->strip[1], &a->strip[0], sizeof(a->strip[0]) * (STRIP_MAX - 1));
    memset(&a->strip[0], 0, sizeof(a->strip[0]));
    a->strip[0].img = (struct lm_image){ tw, STRIP_H, px };
    a->strip[0].tried = true;
    snprintf(a->strip[0].path, sizeof(a->strip[0].path), "%s", path);
    a->strip[0].video = is_clip_name(path);
    if (a->nstrip < STRIP_MAX)
        a->nstrip++;
    a->strip_top = 0;                                           /* the new one is in view */
}

static int cam_media_filter(const struct dirent *e)
{
    size_t n = strlen(e->d_name);
    return !strncmp(e->d_name, "camera-", 7) && n > 11 && (!strcmp(e->d_name + n - 4, ".png") || !strcmp(e->d_name + n - 4, ".mp4"));
}

struct strip_name {
    char name[256];
    char dir[PATH_MAX];
};

static int strip_name_desc(const void *x, const void *y)
{
    return strcmp(((const struct strip_name *)y)->name, ((const struct strip_name *)x)->name);
}

/* At the start: the names of the newest pictures AND clips that are in the folder(s) already, the newest first (the names are time stamps, so the
 * two kinds mix right); the pictures are read when they come into view: `strip_lazy`, the clips' posters `poster_step`. */
static void strip_load(struct app *a)
{
    const char *dirs[2] = { a->outdir, strcmp(a->videodir, a->outdir) ? a->videodir : NULL };
    struct strip_name *all = NULL;
    size_t nall = 0, cap = 0;
    for (int d = 0; d < 2; d++) {
        if (!dirs[d])
            continue;
        struct dirent **list = NULL;
        int n = scandir(dirs[d], &list, cam_media_filter, NULL);
        for (int i = 0; i < n; i++) {
            if (nall == cap) {
                struct strip_name *g = realloc(all, (cap = cap ? cap * 2 : 64) * sizeof(*all));
                if (!g)
                    break;
                all = g;
            }
            snprintf(all[nall].name, sizeof(all[nall].name), "%s", list[i]->d_name);
            snprintf(all[nall].dir, sizeof(all[nall].dir), "%s", dirs[d]);
            nall++;
        }
        for (int i = 0; i < n; i++)
            free(list[i]);
        free(list);
    }
    if (all)
        qsort(all, nall, sizeof(*all), strip_name_desc);
    for (size_t i = 0; i < nall && a->nstrip < STRIP_MAX; i++) {
        memset(&a->strip[a->nstrip], 0, sizeof(a->strip[0]));
        snprintf(a->strip[a->nstrip].path, sizeof(a->strip[0].path), "%s/%s", all[i].dir, all[i].name);
        a->strip[a->nstrip].video = is_clip_name(all[i].name);
        a->nstrip++;
    }
    free(all);
}

/* Starts `sibling arg` in a detached child: the program next to this one (else in PATH); $envvar names another program, "none" = nothing. */
static void spawn_detached(const char *envvar, const char *sibling, const char *arg)
{
    const char *v = getenv(envvar);
    if (v && !strcmp(v, "none"))
        return;
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
    char prog[PATH_MAX];
    snprintf(prog, sizeof(prog), "%s", sibling);
    if (v && *v) {
        snprintf(prog, sizeof(prog), "%s", v);
    } else {
        char exe[PATH_MAX];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 40);
        char *slash = n > 0 ? (exe[n] = 0, strrchr(exe, '/')) : NULL;
        if (slash) {
            snprintf(slash + 1, 32, "%s", sibling);
            if (access(exe, X_OK) == 0)
                snprintf(prog, sizeof(prog), "%s", exe);
        }
    }
    int nul = open("/dev/null", O_RDWR);
    if (nul >= 0) {
        dup2(nul, 0);
        dup2(nul, 1);
        dup2(nul, 2);
    }
    execlp(prog, prog, arg, (char *)NULL);
    _exit(127);
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
static bool save_snapshot(struct app *a, const struct lm_image *img, char *name, size_t nsz)
{
    if (!img || !img->px)
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
        if (!write_png(path, img, true))
            return false;
        strip_push(a, path, img);
        return true;
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

static void cam_rail(const struct app *a, struct wlrail *L)
{
    static void (*const icons[N_B])(struct wlwin *, int, int, uint32_t, uint32_t) = { wlrail_ic_mirror, wlrail_ic_pause, wlrail_ic_size, wlrail_ic_swap };
    const uint32_t base[N_B] = { lm_theme.btn_blue, lm_theme.btn_amber, lm_theme.btn_teal, lm_theme.btn_purple };
    const uint32_t lite[N_B] = { lm_theme.btn_blue_lit, lm_theme.btn_amber_lit, lm_theme.btn_teal_lit, lm_theme.btn_purple_lit };
    static char tips[N_B][40];
    memset(L, 0, sizeof(*L));
    L->y0 = 0;
    L->y1 = a->win.bh > 0 ? a->win.bh - STATUS_H : 400;
    L->n = N_B;
    for (int i = 0; i < N_B; i++) {
        char tmp[40];
        snprintf(tips[i], sizeof(tips[i]), "%s", btn_label(a, i, tmp, sizeof(tmp)));
        bool on = (i == B_MIRROR && a->mirror) || (i == B_PAUSE && a->paused);
        L->it[i] = (struct wlrail_item){ .tip = tips[i], .icon = icons[i], .base = base[i], .lite = lite[i], .show = btn_visible(a, i), .lit = on };
    }
}

static int btn_at(const struct app *a, double px, double py)
{
    (void)px;
    (void)py;
    struct wlrail L;
    cam_rail(a, &L);
    return wlrail_at(&L, a->win.rpx, a->win.rpy);
}

/* ---- the Size button's drop-down: a list of the sizes the camera can deliver ----------------------------------------------------------------------- */

#define MENU_ROW_H (LM_FONT_H + 4)

static int cur_size_index(const struct app *a)
{
    for (int i = 0; i < a->src.nsizes; i++)
        if (a->src.sizes[i].w == a->src.w && a->src.sizes[i].h == a->src.h)
            return i;
    return 0;
}

static int menu_rows_fit(const struct app *a)
{
    return LM_CLAMP((a->win.bh - 12) / MENU_ROW_H, 1, MAX_SIZES);
}

/* The list's place in the drawing area (the window beyond the rail): beside the Size drop of the rail. */
static void menu_rect(const struct app *a, int *x, int *y, int *w, int *h)
{
    char tmp[24];
    int rows = LM_MIN(a->src.nsizes, menu_rows_fit(a)), tw = 0;
    for (int i = 0; i < a->src.nsizes; i++) {
        snprintf(tmp, sizeof(tmp), "%dx%d", a->src.sizes[i].w, a->src.sizes[i].h);
        tw = LM_MAX(tw, (int)strlen(tmp));
    }
    *w = tw * LM_FONT_W + 32;
    *h = rows * MENU_ROW_H + 4;
    int bx = 4, by;
    struct wlrail L;
    int ys[WLRAIL_MAX];
    cam_rail(a, &L);
    wlrail_layout(&L, ys);
    by = ys[B_SIZE] >= 0 ? ys[B_SIZE] : 4;
    *x = bx;
    *y = LM_CLAMP(by, 0, LM_MAX(a->win.bh - *h, 0));
}

static int menu_row_at(const struct app *a, double px, double py)           /* index into the sizes, or -1 */
{
    int x, y, w, h;
    menu_rect(a, &x, &y, &w, &h);
    if (px < x || px >= x + w || py < y + 2 || py >= y + h - 2)
        return -1;
    int row = a->menu_top + ((int)py - y - 2) / MENU_ROW_H;
    return row < a->src.nsizes ? row : -1;
}

static void menu_scroll_to_hover(struct app *a)
{
    int rows = menu_rows_fit(a);
    if (a->menu_hover < a->menu_top)
        a->menu_top = a->menu_hover;
    if (a->menu_hover >= a->menu_top + rows)
        a->menu_top = a->menu_hover - rows + 1;
    a->menu_top = LM_CLAMP(a->menu_top, 0, LM_MAX(a->src.nsizes - rows, 0));
}

static void menu_set(struct app *a, bool open)
{
    a->menu_open = open && a->src.nsizes > 0;
    if (a->menu_open) {
        a->menu_hover = cur_size_index(a);
        a->menu_top = 0;
        menu_scroll_to_hover(a);
    }
    a->win.redraw = true;
}

static void start_stream(struct app *a);

static void conf_save(const struct app *a);

static void menu_pick(struct app *a, int i)
{
    if (i >= 0 && i < a->src.nsizes && a->src.on && !a->clip) {
        a->want = a->src.sizes[i];
        a->saved_size = a->want;                                /* the next start has this resolution again */
        conf_save(a);
        start_stream(a);
    }
    menu_set(a, false);
}

static void paint_menu(struct app *a)
{
    struct wlwin *w = &a->win;
    int x, y, mw, mh;
    menu_rect(a, &x, &y, &mw, &mh);
    wlwin_round_fill(w, x - 1, y - 1, mw + 2, mh + 2, 9, lm_theme.menu_border);
    wlwin_round_fill(w, x, y, mw, mh, 8, lm_theme.bar);
    int rows = LM_MIN(a->src.nsizes - a->menu_top, menu_rows_fit(a)), cur = cur_size_index(a);
    for (int r = 0; r < rows; r++) {
        int i = a->menu_top + r, ry = y + 2 + r * MENU_ROW_H;
        char t[24];
        snprintf(t, sizeof(t), "%dx%d", a->src.sizes[i].w, a->src.sizes[i].h);
        if (i == a->menu_hover)
            wlwin_round_fill(w, x + 3, ry, mw - 6, MENU_ROW_H, MENU_ROW_H / 2, lm_theme.terminal_button_on);
        wlwin_text(w, x + 10, ry + 2, t, 1, i == cur ? lm_theme.menu_current : lm_theme.bar_text_bright, 0);
        if (i == cur)
            wlwin_fill(w, x + mw - 12, ry + MENU_ROW_H / 2 - 2, 4, 4, lm_theme.menu_current);       /* a dot marks the size in use */
    }
}

static void paint_picture(struct app *a, int vw, int vh)
{
    struct wlwin *w = &a->win;
    const struct lm_image *im = shown(a);
    int k = wlwin_scale(w);                                          /* the picture is made for the buffer's own pixels: sharp at any scale */
    int pvw = vw * k, pvh = vh * k;
    double s = LM_MIN((double)pvw / im->w, (double)pvh / im->h);
    int dw = LM_MAX((int)(im->w * s), 1), dh = LM_MAX((int)(im->h * s), 1);
    int ox = w->inx * k + (pvw - dw) / 2, oy = (pvh - dh) / 2;       /* (right of the rail) */
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

/* ---- Photo Booth: the controls over the picture, the grid of effects ------------------------------------------------------------- */

#define CTL_D 54                        /* the shutter */
#define CTL_BH 28
#define CTL_GAP 8
#define CTL_PAD 8
#define GRID_M 14
#define GRID_G 10
#define GRID_NAV_H 36
#define GRID_LAB_H (LM_FONT_H + 6)
#define NAV_PREV 100
#define NAV_NEXT 101

static int picture_h(const struct app *a)                       /* the height of the live picture: the controls and the status line are under it */
{
    return a->win.bh > STATUS_H + CTRL_H ? a->win.bh - STATUS_H - CTRL_H : 1;
}

/* The strip of the last pictures has a column of its own at the right of the picture (not on a narrow or low window, not while the grid is open). */
static bool strip_on(const struct app *a)
{
    return !a->fx_open && a->win.bw >= 420 && picture_h(a) >= 160;
}

/* The width the live picture (and its controls) has. */
static int pic_w(const struct app *a)
{
    return a->win.bw - (strip_on(a) ? STRIP_COL : 0);
}

static int strip_fit(const struct app *a)                       /* how many pictures the column shows at once */
{
    return LM_MAX((picture_h(a) - 44 - 8) / (STRIP_TH + 8), 0);
}

static const char *ctl_label(const struct app *a, int i, char *buf, size_t n)
{
    switch (i) {
    case O_FX: return "Effects";
    case O_KIND: return a->video ? "Video" : "Photo";
    case O_MODE: return a->fourup ? "4-up" : "1-up";
    case O_MIC: return a->mic ? "Mic on" : "Mic off";
    case O_TIMER:
        if (!a->timer_s)
            return "No timer";
        snprintf(buf, n, "%d sec", a->timer_s);
        return buf;
    default: return "";
    }
}

static const char *ctl_name(int i)
{
    static const char *const names[N_O] = { "fx", "kind", "shutter", "mode", "timer", "mic" };
    return i >= 0 && i < N_O ? names[i] : "?";
}

static int ctl_width(const struct app *a, int i)
{
    char tmp[16];
    return i == O_SHUTTER ? CTL_D : (int)strlen(ctl_label(a, i, tmp, sizeof(tmp))) * LM_FONT_W + 20;
}

/* The controls that show, left to right: a photo has a choice of one or four pictures and a countdown, a video the microphone. */
static int ctl_list(const struct app *a, int *ids)
{
    int n = 0;
    ids[n++] = O_FX;
    ids[n++] = O_KIND;
    ids[n++] = O_SHUTTER;
    if (a->video) {
        ids[n++] = O_MIC;
    } else {
        ids[n++] = O_MODE;
        ids[n++] = O_TIMER;
    }
    return n;
}

/* The place of a control in the window's drawing area (logical pixels): a row under the picture, in the middle. */
static bool ctl_rect(const struct app *a, int i, int *x, int *y, int *w, int *h)
{
    int ids[N_O], n = ctl_list(a, ids), vw = a->win.bw, top = picture_h(a) + (CTRL_H - CTL_D) / 2;
    int total = CTL_GAP * (n - 1);
    for (int k = 0; k < n; k++)
        total += ctl_width(a, ids[k]);
    int bx = (vw - total) / 2;
    if (vw < 200)
        return false;
    for (int k = 0; k < n; k++) {
        int bw = ctl_width(a, ids[k]);
        if (ids[k] == i) {
            *x = bx;
            *w = bw;
            *y = i == O_SHUTTER ? top : top + (CTL_D - CTL_BH) / 2;
            *h = i == O_SHUTTER ? CTL_D : CTL_BH;
            return true;
        }
        bx += bw + CTL_GAP;
    }
    return false;
}

static int ctl_at(const struct app *a, double px, double py)
{
    for (int i = 0; i < N_O; i++) {
        int x, y, w, h;
        if (ctl_rect(a, i, &x, &y, &w, &h) && px >= x && px < x + w && py >= y && py < y + h)
            return i;
    }
    return -1;
}

/* The row of controls under the picture. */
static void paint_controls(struct app *a)
{
    struct wlwin *w = &a->win;
    int vh = picture_h(a);
    wlwin_fill(w, 0, vh, w->bw, CTRL_H, lm_theme.bar);
    wlwin_fill(w, 0, vh, w->bw, 1, lm_theme.border);
    const uint32_t base[N_O] = { lm_theme.btn_purple, lm_theme.btn_slate, lm_theme.btn_red, lm_theme.btn_teal, lm_theme.btn_amber, lm_theme.btn_slate };
    const uint32_t lite[N_O] = { lm_theme.btn_purple_lit, lm_theme.btn_slate_lit, lm_theme.btn_red_lit, lm_theme.btn_teal_lit, lm_theme.btn_amber_lit, lm_theme.btn_slate_lit };
    for (int i = 0; i < N_O; i++) {
        int x, y, bw, bh;
        char tmp[16];
        if (!ctl_rect(a, i, &x, &y, &bw, &bh))
            continue;
        bool on = (i == O_FX && a->fx != CAMFX_NORMAL) || (i == O_MODE && a->fourup) || (i == O_TIMER && a->timer_s) || (i == O_MIC && a->mic) || (i == O_KIND && a->video);
        if (i == O_SHUTTER) {
            wlwin_round_fill(w, x, y, bw, bh, bh / 2, lm_theme.on_colour);                          /* the white ring of a shutter button */
            wlwin_drop(w, x + 5, y + 5, bw - 10, bh - 10, (bh - 10) / 2, a->ohover == i ? lm_theme.btn_red_lit : lm_theme.btn_red);
            if (a->phase == PH_WAIT || a->clip)                                                     /* a square: press to cancel / stop */
                wlwin_round_fill(w, x + bw / 2 - 8, y + bh / 2 - 8, 16, 16, 3, lm_theme.on_colour);
            else if (a->video)                                                                      /* a video: the dot of a recorder */
                wlwin_round_fill(w, x + bw / 2 - 6, y + bh / 2 - 6, 12, 12, 6, lm_theme.on_colour);
            continue;
        }
        if (on)
            wlwin_round_fill(w, x - 1, y - 1, bw + 2, bh + 2, lm_button_radius((bh + 2) / 2), lm_theme.on_colour);
        wlwin_drop(w, x, y, bw, bh, lm_button_radius(bh / 2), on || a->ohover == i ? lite[i] : base[i]);
        const char *t = ctl_label(a, i, tmp, sizeof(tmp));
        wlwin_text(w, x + (bw - (int)strlen(t) * LM_FONT_W) / 2, y + (bh - LM_FONT_H) / 2, t, 1, lm_theme.on_colour, 0);
    }
}

/* The seconds, huge, in the middle of the picture. */
static void paint_digits(struct app *a, int secs)
{
    int vw = pic_w(a), vh = picture_h(a), sc = LM_CLAMP(vh / 40, 3, 16);
    char txt[16];
    snprintf(txt, sizeof(txt), "%d", secs);
    int tw = (int)strlen(txt) * LM_FONT_W * sc, tx = (vw - tw) / 2, ty = (vh - LM_FONT_H * sc) / 2 - 20;
    wlwin_text(&a->win, tx + 3, ty + 3, txt, sc, 0x000000, 0);
    wlwin_text(&a->win, tx, ty, txt, sc, 0xffffff, 0);
}

/* "Step out of the picture": the countdown to the empty scene of a backdrop effect. */
static void paint_bgwait(struct app *a)
{
    struct wlwin *w = &a->win;
    if (!a->bg_until)
        return;
    uint64_t t = now_ms();
    paint_digits(a, t >= a->bg_until ? 1 : (int)((a->bg_until - t + 999) / 1000));
    const char *msg = "Step out of the picture";
    int tw = (int)strlen(msg) * LM_FONT_W;
    wlwin_round_fill(w, (pic_w(a) - tw) / 2 - 14, 10, tw + 28, 28, 14, 0x000000);
    wlwin_text(w, (pic_w(a) - tw) / 2, 16, msg, 1, 0xffffff, 0);
}

/* The countdown over the picture (big seconds), and for four pictures which one comes. */
static void paint_countdown(struct app *a)
{
    struct wlwin *w = &a->win;
    if (a->phase != PH_WAIT)
        return;
    int vw = pic_w(a);
    uint64_t t = now_ms();
    if (a->digits)
        paint_digits(a, t >= a->t_shot ? 1 : (int)((a->t_shot - t + 999) / 1000));
    if (a->fourup) {
        char tag[32];
        snprintf(tag, sizeof(tag), "Photo %d of 4", a->shot_n + 1);
        int tw = (int)strlen(tag) * LM_FONT_W;
        wlwin_round_fill(w, vw - tw - 32, 8, tw + 24, 24, 12, 0x000000);
        wlwin_text(w, vw - tw - 20, 12, tag, 1, 0xffffff, 0);
    }
}

/* The white flash of a picture that was taken: the picture area goes white and fades out in FLASH_MS. */
static void paint_flash(struct app *a)
{
    struct wlwin *w = &a->win;
    uint64_t t = now_ms();
    if (!a->flash_t0 || t - a->flash_t0 >= FLASH_MS)
        return;
    double f = 1.0 - (double)(t - a->flash_t0) / FLASH_MS;
    f *= f;
    int k = wlwin_scale(w), x0 = w->inx * k, x1 = LM_MIN((w->inx + pic_w(a)) * k, w->pw), y1 = LM_MIN(picture_h(a) * k, w->ph);
    for (int y = 0; y < y1; y++) {
        uint32_t *row = w->pixels + (size_t)y * (size_t)w->pw;
        for (int x = x0; x < x1; x++)
            row[x] = lm_mix_rgb(row[x], 0xffffff, f);
    }
}

/* The grid of effects: tile i (0..8) of the page, the cell with its name under it. */
static bool grid_tile(const struct app *a, int i, int *x, int *y, int *w, int *h)
{
    int vw = a->win.bw, vh = picture_h(a);
    int cw = (vw - 2 * GRID_M - 2 * GRID_G) / 3, ch = (vh - GRID_M - GRID_NAV_H - 2 * GRID_G) / 3;
    if (cw < 48 || ch < GRID_LAB_H + 24 || i < 0 || i >= FX_PER_PAGE)
        return false;
    *x = GRID_M + (i % 3) * (cw + GRID_G);
    *y = GRID_M + (i / 3) * (ch + GRID_G);
    *w = cw;
    *h = ch;
    return true;
}

static bool grid_nav(const struct app *a, int which, int *x, int *y, int *w, int *h)
{
    int vw = a->win.bw, vh = picture_h(a);
    if (vw < 120 || vh < 120)
        return false;
    *w = 34;
    *h = 26;
    *y = vh - GRID_NAV_H + 4;
    *x = which == NAV_PREV ? vw / 2 - 70 : vw / 2 + 36;
    return true;
}

/* A tile 0..8 (only the ones that have an effect), NAV_PREV / NAV_NEXT, or -1. */
static int grid_hit(const struct app *a, double px, double py)
{
    for (int i = 0; i < FX_PER_PAGE; i++) {
        int x, y, w, h;
        if (a->fx_page * FX_PER_PAGE + i < CAMFX_COUNT && grid_tile(a, i, &x, &y, &w, &h) && px >= x && px < x + w && py >= y && py < y + h)
            return i;
    }
    for (int n = NAV_PREV; n <= NAV_NEXT; n++) {
        int x, y, w, h;
        if (grid_nav(a, n, &x, &y, &w, &h) && px >= x && px < x + w && py >= y && py < y + h)
            return n;
    }
    return -1;
}

/* d = the source squeezed into dw x dh by averaging blocks (client/thumbbox.h). */
static void thumb_box(const uint32_t *s, int sw, int sh, uint32_t *d, int dw, int dh)
{
    lm_thumb_box(s, sw, sh, d, dw, dh);
}

static void paint_grid(struct app *a)
{
    struct wlwin *w = &a->win;
    int k = wlwin_scale(w);
    for (int i = 0; i < FX_PER_PAGE; i++) {
        int fxn = a->fx_page * FX_PER_PAGE + i, x, y, cw, ch;
        if (fxn >= CAMFX_COUNT || !grid_tile(a, i, &x, &y, &cw, &ch))
            continue;
        bool sel = fxn == a->fx, hov = i == a->fx_hover;
        if (sel || hov)
            wlwin_round_fill(w, x - 3, y - 3, cw + 6, ch + 6, 11, sel ? lm_theme.accent : lm_theme.text_soft);
        wlwin_round_fill(w, x, y, cw, ch, 8, lm_theme.bar);
        int ih = ch - GRID_LAB_H - 4, iw = cw - 8;
        if (a->have && a->img.px && ih > 8 && iw > 8) {
            double sc = LM_MIN((double)iw / a->img.w, (double)ih / a->img.h);
            int dw = LM_MAX((int)(a->img.w * sc), 1), dh = LM_MAX((int)(a->img.h * sc), 1);
            int pw = dw * k, ph = dh * k;
            bool useref = camfx_is_backdrop(fxn) && a->have_ref && a->ref.px && a->ref.w == a->img.w && a->ref.h == a->img.h;
            uint32_t *src = malloc((size_t)pw * (size_t)ph * 12);
            if (src) {
                uint32_t *dst = src + (size_t)pw * (size_t)ph, *rt = dst + (size_t)pw * (size_t)ph;
                thumb_box(a->img.px, a->img.w, a->img.h, src, pw, ph);
                if (useref)
                    thumb_box(a->ref.px, a->ref.w, a->ref.h, rt, pw, ph);
                camfx_apply_at(fxn, src, useref ? rt : NULL, pw, ph, dst, a->focus_x, a->focus_y);
                if (a->mirror)                                                  /* as the big picture: the effect first, then the mirror */
                    for (int yy = 0; yy < ph; yy++) {
                        uint32_t *row = dst + (size_t)yy * (size_t)pw;
                        for (int xx = 0; xx < pw / 2; xx++) {
                            uint32_t t = row[xx];
                            row[xx] = row[pw - 1 - xx];
                            row[pw - 1 - xx] = t;
                        }
                    }
                wlwin_blit_px(w, x + 4 + (iw - dw) / 2, y + 4 + (ih - dh) / 2, dst, pw, ph, pw, 0, 0, w->bw, w->bh);
                free(src);
            }
        }
        const char *nm = camfx_name(fxn);
        int tw = (int)strlen(nm) * LM_FONT_W;
        wlwin_text(w, x + (cw - tw) / 2, y + ch - GRID_LAB_H + 1, nm, 1, sel ? lm_theme.accent : lm_theme.bar_text_bright, cw - 4);
    }
    for (int n = NAV_PREV; n <= NAV_NEXT; n++) {
        int x, y, bw, bh;
        if (!grid_nav(a, n, &x, &y, &bw, &bh))
            continue;
        bool off = n == NAV_PREV ? a->fx_page <= 0 : a->fx_page >= FX_PAGES - 1;
        wlwin_drop(w, x, y, bw, bh, lm_button_radius(bh / 2), off ? lm_theme.btn_slate : lm_theme.btn_blue);
        wlwin_text(w, x + (bw - LM_FONT_W) / 2, y + (bh - LM_FONT_H) / 2, n == NAV_PREV ? "<" : ">", 1, lm_theme.on_colour, 0);
    }
    char pg[16];
    snprintf(pg, sizeof(pg), "%d / %d", a->fx_page + 1, FX_PAGES);
    wlwin_text(w, a->win.bw / 2 - (int)strlen(pg) * LM_FONT_W / 2, picture_h(a) - GRID_NAV_H + 4 + (26 - LM_FONT_H) / 2, pg, 1, lm_theme.text_soft, 0);
}

/* ---- the strip of the last pictures: a column at the right of the picture --------------------------------------------------------- */

static void strip_clamp(struct app *a)
{
    a->strip_top = LM_CLAMP(a->strip_top, 0, LM_MAX(a->nstrip - strip_fit(a), 0));
}

static bool folder_rect(const struct app *a, int *x, int *y, int *w, int *h)
{
    if (!strip_on(a))
        return false;
    *w = STRIP_TW;
    *h = 26;
    *x = a->win.bw - STRIP_COL + (STRIP_COL - STRIP_TW) / 2;
    *y = 8;
    return true;
}

static bool folder_at(const struct app *a, double px, double py)
{
    int x, y, w, h;
    return folder_rect(a, &x, &y, &w, &h) && px >= x && px < x + w && py >= y && py < y + h;
}

/* The place of picture i (an index into the strip) if it is in view: its slot of 64 x 48 in the column, the picture fitted into it (a slot with no
 * small copy yet is the whole slot). */
static bool strip_rect(const struct app *a, int i, int *x, int *y, int *w, int *h)
{
    int slot = i - a->strip_top;
    if (!strip_on(a) || i < 0 || i >= a->nstrip || slot < 0 || slot >= strip_fit(a))
        return false;
    int sx = a->win.bw - STRIP_COL + (STRIP_COL - STRIP_TW) / 2, sy = 44 + slot * (STRIP_TH + 8);
    const struct lm_image *t = &a->strip[i].img;
    *w = STRIP_TW;
    *h = STRIP_TH;
    if (t->px && t->w > 0 && t->h > 0) {
        double sc = LM_MIN((double)STRIP_TW / t->w, (double)STRIP_TH / t->h);
        *w = LM_MAX((int)(t->w * sc), 1);
        *h = LM_MAX((int)(t->h * sc), 1);
    }
    *x = sx + (STRIP_TW - *w) / 2;
    *y = sy + (STRIP_TH - *h) / 2;
    return true;
}

static int strip_at(const struct app *a, double px, double py)
{
    for (int i = a->strip_top; i < a->nstrip; i++) {
        int x, y, w, h;
        if (!strip_rect(a, i, &x, &y, &w, &h))
            break;
        if (px >= x - 3 && px < x + w + 3 && py >= y - 3 && py < y + h + 3)
            return i;
    }
    return -1;
}

/* Makes the small copy of one picture of the folder that is in view and has none yet (one a call: a big PNG takes a moment). True when it did one,
 * so the loop comes back soon for the next. */
static bool strip_lazy(struct app *a)
{
    if (!strip_on(a))
        return false;
    for (int i = a->strip_top; i < a->nstrip && i < a->strip_top + strip_fit(a); i++) {
        struct thumb *t = &a->strip[i];
        if (t->img.px || t->tried || t->video)
            continue;
        t->tried = true;
        struct lm_image im = { 0 };
        char err[120];
        if (lm_img_load(t->path, &im, err, sizeof(err)) == 0) {
            int tw = LM_CLAMP(im.w * STRIP_H / im.h, 16, 256);
            uint32_t *px = malloc((size_t)tw * STRIP_H * 4);
            if (px) {
                thumb_box(im.px, im.w, im.h, px, tw, STRIP_H);
                t->img = (struct lm_image){ tw, STRIP_H, px };
            }
            lm_img_free(&im);
        }
        a->win.redraw = true;
        return true;
    }
    return false;
}

/* The poster of a clip of the folder: `mellagui-view --poster FILE OUT.ppm` in a child (ffmpeg may be needed and takes a moment, so the loop never
 * waits for it); one at a time, for the clips that are in view. $MELLAGUI_CAM_POSTER = another program, "none" = no posters (the strip shows a tile). */
static struct {
    pid_t pid;
    uint64_t t0;
    char clip[PATH_MAX + 300];
    char out[PATH_MAX];
} g_poster;

static void poster_finish(struct app *a, bool ok)
{
    for (int i = 0; i < a->nstrip; i++) {
        struct thumb *t = &a->strip[i];
        if (strcmp(t->path, g_poster.clip))
            continue;
        t->tried = true;
        struct lm_image im = { 0 };
        char err[120];
        if (ok && lm_img_load(g_poster.out, &im, err, sizeof(err)) == 0) {
            int tw = LM_CLAMP(im.w * STRIP_H / im.h, 16, 256);
            uint32_t *px = malloc((size_t)tw * STRIP_H * 4);
            if (px) {
                thumb_box(im.px, im.w, im.h, px, tw, STRIP_H);
                free(t->img.px);
                t->img = (struct lm_image){ tw, STRIP_H, px };
            }
            lm_img_free(&im);
        }
        break;
    }
    unlink(g_poster.out);
    g_poster.pid = 0;
    a->win.redraw = true;
}

static void poster_start(struct app *a, struct thumb *t)
{
    const char *v = getenv("MELLAGUI_CAM_POSTER");
    if (v && !strcmp(v, "none")) {
        t->tried = true;
        return;
    }
    const char *rt = getenv("XDG_RUNTIME_DIR");
    snprintf(g_poster.out, sizeof(g_poster.out), "%s/mellagui-cam-poster-%d.ppm", rt && *rt ? rt : "/tmp", (int)getpid());
    snprintf(g_poster.clip, sizeof(g_poster.clip), "%s", t->path);
    pid_t pid = fork();
    if (pid < 0) {
        t->tried = true;
        return;
    }
    if (pid == 0) {
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) {
            dup2(nul, 0);
            dup2(nul, 1);
            dup2(nul, 2);
        }
        char prog[PATH_MAX] = "mellagui-view";
        if (v && *v) {
            snprintf(prog, sizeof(prog), "%s", v);
        } else {
            char exe[PATH_MAX];
            ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 40);
            char *slash = n > 0 ? (exe[n] = 0, strrchr(exe, '/')) : NULL;
            if (slash) {
                snprintf(slash + 1, 32, "mellagui-view");
                if (access(exe, X_OK) == 0)
                    snprintf(prog, sizeof(prog), "%s", exe);
            }
        }
        execlp(prog, prog, "--poster", g_poster.clip, g_poster.out, (char *)NULL);
        _exit(127);
    }
    g_poster.pid = pid;
    g_poster.t0 = now_ms();
    (void)a;
}

/* Called every turn of the loop: collects a finished poster, starts the next one. True while a child is working (the loop comes back soon). */
static bool poster_step(struct app *a)
{
    if (g_poster.pid) {
        int st;
        pid_t r = waitpid(g_poster.pid, &st, WNOHANG);
        if (r == g_poster.pid) {
            poster_finish(a, WIFEXITED(st) && WEXITSTATUS(st) == 0);
        } else if (r < 0) {
            poster_finish(a, false);
        } else if (now_ms() - g_poster.t0 > 12000) {
            kill(g_poster.pid, SIGKILL);
            waitpid(g_poster.pid, &st, 0);
            poster_finish(a, false);
        }
        return g_poster.pid != 0;
    }
    if (!strip_on(a))
        return false;
    for (int i = a->strip_top; i < a->nstrip && i < a->strip_top + strip_fit(a); i++) {
        struct thumb *t = &a->strip[i];
        if (!t->video || t->img.px || t->tried)
            continue;
        poster_start(a, t);
        return g_poster.pid != 0;
    }
    return false;
}

/* A small play mark in the middle of a clip's picture (the box x, y, w, h in logical pixels). */
static void paint_play_mark(struct wlwin *w, int x, int y, int tw, int th)
{
    int s = LM_CLAMP(LM_MIN(tw, th) / 2, 8, 20), cx = x + tw / 2 - s / 4, cy = y + th / 2;
    wlwin_round_fill(w, x + tw / 2 - s / 2 - 3, cy - s / 2 - 3, s + 6, s + 6, (s + 6) / 2, lm_theme.ink);
    for (int r = 0; r < s; r++) {
        int dist = abs(2 * r + 1 - s);                                  /* 0 in the middle row .. s at the ends */
        wlwin_fill(w, cx - s / 4, cy - s / 2 + r, (s - dist) * 2 / 3 + 1, 1, lm_theme.on_colour);
    }
}

static void paint_strip(struct app *a)
{
    struct wlwin *w = &a->win;
    if (!strip_on(a))
        return;
    strip_clamp(a);
    int k = wlwin_scale(w), vh = picture_h(a), x0 = w->bw - STRIP_COL, fit = strip_fit(a);
    wlwin_fill(w, x0, 0, STRIP_COL, vh, lm_theme.bar);
    wlwin_fill(w, x0, 0, 1, vh, lm_theme.border);
    int fx, fy, fw, fh;
    if (folder_rect(a, &fx, &fy, &fw, &fh)) {
        wlwin_drop(w, fx, fy, fw, fh, lm_button_radius(fh / 2), a->fhover ? lm_theme.btn_teal_lit : lm_theme.btn_teal);
        wlwin_text(w, fx + (fw - 6 * LM_FONT_W) / 2, fy + (fh - LM_FONT_H) / 2, "Folder", 1, lm_theme.on_colour, 0);
    }
    for (int i = a->strip_top; i < a->nstrip; i++) {
        int x, y, tw, th;
        if (!strip_rect(a, i, &x, &y, &tw, &th))
            break;
        if (!a->strip[i].img.px) {                                                                       /* not read yet (or not a picture) */
            wlwin_round_fill(w, x, y, tw, th, 4, lm_theme.border);
            if (a->strip[i].video)
                paint_play_mark(w, x, y, tw, th);
            continue;
        }
        wlwin_round_fill(w, x - 3, y - 3, tw + 6, th + 6, 5, a->shover == i ? lm_theme.accent : lm_theme.on_colour);        /* a print with a white border */
        int pw = tw * k, ph = th * k;
        uint32_t *buf = malloc((size_t)pw * (size_t)ph * 4);
        if (!buf)
            continue;
        thumb_box(a->strip[i].img.px, a->strip[i].img.w, a->strip[i].img.h, buf, pw, ph);
        wlwin_blit_px(w, x, y, buf, pw, ph, pw, 0, 0, w->bw, w->bh);
        free(buf);
        if (a->strip[i].video)
            paint_play_mark(w, x, y, tw, th);
    }
    if (a->nstrip > fit && fit > 0) {                                                                    /* more than fit: a thumb for the wheel */
        int track = fit * (STRIP_TH + 8) - 8, th = LM_MAX(track * fit / a->nstrip, 12), room = a->nstrip - fit;
        wlwin_fill(w, x0 + 3, 44, 3, track, lm_theme.border);
        wlwin_fill(w, x0 + 3, 44 + (track - th) * a->strip_top / room, 3, th, lm_theme.text_soft);
    }
}

/* ---- the focus of the effects of the second page ------------------------------------------------------------------------------------ */

/* The picture as it lies in the picture's area (logical pixels). */
static bool pic_rect(const struct app *a, double *x, double *y, double *w, double *h)
{
    const struct lm_image *im = shown(a);
    if (!a->have || !im->px || im->w < 1 || im->h < 1)
        return false;
    int vw = pic_w(a), vh = picture_h(a);
    double s = LM_MIN((double)vw / im->w, (double)vh / im->h);
    *w = im->w * s;
    *h = im->h * s;
    *x = (vw - *w) / 2;
    *y = (vh - *h) / 2;
    return true;
}

/* The mouse works the effect when it has a point (mirror, kaleidoscope, tunnel, lens effects, twirl, vintage) and nothing else is going on. */
static bool focus_active(const struct app *a)
{
    return camfx_has_focus(a->fx) && !a->fx_open && !a->bg_until && a->have;
}

static bool pic_hit(const struct app *a, double px, double py)
{
    double x, y, w, h;
    return pic_rect(a, &x, &y, &w, &h) && px >= x && px < x + w && py >= y && py < y + h;
}

/* The point under the pointer is the effect's focus. The picture is shown mirrored, the effect works in the camera's own space: the x is turned. */
static void focus_from(struct app *a, double px, double py)
{
    double x, y, w, h;
    if (!pic_rect(a, &x, &y, &w, &h))
        return;
    double u = LM_CLAMP((px - x) / w, 0.0, 1.0), v = LM_CLAMP((py - y) / h, 0.0, 1.0);
    a->focus_x = (float)(a->mirror ? 1.0 - u : u);
    a->focus_y = (float)v;
    fx_update(a);
    a->win.redraw = true;
}

/* A small crosshair at the focus (not in the saved pictures) while the pointer is on the picture or the point is being moved. */
static void paint_focus(struct app *a)
{
    struct wlwin *w = &a->win;
    double x, y, pw, ph;
    if (!focus_active(a) || !pic_rect(a, &x, &y, &pw, &ph))
        return;
    if (!a->focus_drag && !(a->pin && pic_hit(a, a->px, a->py) && a->ohover < 0 && a->shover < 0))
        return;
    int cx = (int)(x + (a->mirror ? 1.0f - a->focus_x : a->focus_x) * pw), cy = (int)(y + a->focus_y * ph);
    for (int pass = 0; pass < 2; pass++) {                                                               /* a shadow, then white */
        int o = pass == 0 ? 1 : 0;
        uint32_t col = pass == 0 ? 0x000000 : 0xffffff;
        wlwin_fill(w, cx - 15 + o, cy - 1 + o, 10, 2, col);
        wlwin_fill(w, cx + 6 + o, cy - 1 + o, 10, 2, col);
        wlwin_fill(w, cx - 1 + o, cy - 15 + o, 2, 10, col);
        wlwin_fill(w, cx - 1 + o, cy + 6 + o, 2, 10, col);
    }
}

/* $MELLAGUI_CAM_DUMP: the state and the places of the controls (window pixels), for the tests. */
static void write_dump(const struct app *a)
{
    if (!a->dump[0])
        return;
    char tmp[PATH_MAX + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", a->dump);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return;
    const struct lm_image *im = shown(a);
    fprintf(f, "state fx=%s page=%d open=%d hover=%d fourup=%d timer=%d phase=%d shot=%d digits=%d flash=%d have=%d mirror=%d clip=%d ref=%d bgwait=%d sound=%d strip=%d top=%d video=%d focusx=%.3f focusy=%.3f\n",
            camfx_id(a->fx), a->fx_page, a->fx_open, a->fx_hover, a->fourup, a->timer_s, a->phase, a->shot_n, a->digits, a->flash_t0 != 0, a->have, a->mirror,
            a->clip != NULL, a->have_ref, a->bg_until != 0, a->sound, a->nstrip, a->strip_top, a->video, a->focus_x, a->focus_y);
    fprintf(f, "image %d %d\n", a->have ? im->w : 0, a->have ? im->h : 0);
    for (int i = 0; i < N_O; i++) {
        int x, y, w, h;
        if (ctl_rect(a, i, &x, &y, &w, &h))
            fprintf(f, "ctl %s %d %d %d %d\n", ctl_name(i), x + a->win.inx, y, w, h);
    }
    double px0, py0, pw0, ph0;
    if (!a->fx_open && pic_rect(a, &px0, &py0, &pw0, &ph0))
        fprintf(f, "pic %d %d %d %d\n", (int)px0 + a->win.inx, (int)py0, (int)pw0, (int)ph0);
    {
        int x, y, w, h;
        if (folder_rect(a, &x, &y, &w, &h))
            fprintf(f, "ctl folder %d %d %d %d\n", x + a->win.inx, y, w, h);
    }
    for (int i = 0; i < a->nstrip; i++) {
        int x, y, w, h;
        if (strip_rect(a, i, &x, &y, &w, &h)) {
            const char *base = strrchr(a->strip[i].path, '/');
            fprintf(f, "thumb %d %s %d %d %d %d\n", i, base ? base + 1 : a->strip[i].path, x + a->win.inx, y, w, h);
        }
    }
    if (a->fx_open) {
        for (int i = 0; i < FX_PER_PAGE; i++) {
            int x, y, w, h;
            if (a->fx_page * FX_PER_PAGE + i < CAMFX_COUNT && grid_tile(a, i, &x, &y, &w, &h))
                fprintf(f, "tile %d %s %d %d %d %d\n", i, camfx_id(a->fx_page * FX_PER_PAGE + i), x + a->win.inx, y, w, h);
        }
        for (int n = NAV_PREV; n <= NAV_NEXT; n++) {
            int x, y, w, h;
            if (grid_nav(a, n, &x, &y, &w, &h))
                fprintf(f, "nav %s %d %d %d %d\n", n == NAV_PREV ? "prev" : "next", x + a->win.inx, y, w, h);
        }
    }
    if (fclose(f) != 0 || rename(tmp, a->dump) < 0)
        unlink(tmp);
}

static void paint(struct wlwin *w)
{
    struct app *a = w->data;
    int vw = pic_w(a), vh = picture_h(a);
    wlwin_fill(w, 0, 0, w->bw, w->bh, BG);
    if (a->fx_open) {
        paint_grid(a);
    } else {
        if (a->have && a->img.px)
            paint_picture(a, vw, vh);
        else
            wlwin_text(w, 16, 16, a->msg[0] ? a->msg : "Waiting for the camera...", 1, lm_theme.text_soft, vw - 32);
        paint_strip(a);
        paint_focus(a);
        paint_countdown(a);
        paint_bgwait(a);
    }
    paint_flash(a);
    wlwin_fill(w, 0, vh, w->bw, w->bh - vh, lm_theme.bar);                 /* under the picture: the controls, then the status line */
    paint_controls(a);
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
    {
        /* The status line: what is happening at the left (a clip being recorded, what was saved, else the camera), how the camera delivers at the right:
         * size, format, pictures a second and the switches that are on. */
        char left[240], right[200];
        uint32_t lcol = lm_theme.text_soft;
        if (a->clip) {
            snprintf(left, sizeof(left), "%s", st);
            lcol = lm_theme.alert;
        } else if (a->note[0] && now_ms() < a->note_until) {
            snprintf(left, sizeof(left), "%s", a->note);
            lcol = lm_theme.caution;
        } else if (a->src.on) {
            snprintf(left, sizeof(left), "%s", a->src.card);
        } else {
            snprintf(left, sizeof(left), "%s", a->msg[0] ? a->msg : "No camera");
            lcol = lm_theme.muted;
        }
        right[0] = 0;
        if (a->src.on) {
            snprintf(right, sizeof(right), "%dx%d %s  %d fps%s%s%s%s%s", a->src.w, a->src.h, a->src.fake ? "test" : fourcc_name(a->src.fourcc), a->paused ? 0 : a->fps,
                     a->mirror ? "  mirrored" : "", a->paused ? "  paused" : "", a->mic && a->video ? "  mic" : "", a->fx != CAMFX_NORMAL ? "  " : "",
                     a->fx != CAMFX_NORMAL ? camfx_name(a->fx) : "");
            if (!a->sound && strlen(right) + 10 < sizeof(right))
                strcat(right, "  silent");
        }
        int rw = (int)strlen(right) * LM_FONT_W, sy = w->bh - STATUS_H, ty = sy + (STATUS_H - LM_FONT_H) / 2;
        wlwin_fill(w, 0, sy, w->bw, 1, lm_theme.border);
        wlwin_text(w, 10, ty, left, 1, lcol, LM_MAX(w->bw - rw - 36, 40));
        if (rw && rw + 20 < w->bw)
            wlwin_text(w, w->bw - rw - 10, ty, right, 1, lm_theme.muted, 0);
    }
    if (a->menu_open)
        paint_menu(a);
    {
        struct wlrail L;
        cam_rail(a, &L);
        wlrail_paint(w, &L, &a->ran, a->hover, wlrail_now(), lm_theme.bar, lm_theme.bar, lm_theme.accent, lm_theme.text_soft);
        wlrail_foot(w, &L, lm_theme.bar);
    }
    write_dump(a);
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
    if (a->have && save_snapshot(a, shown(a), name, sizeof(name)))
        set_note(a, "Saved %s", name);
    else
        set_note(a, "Cannot save a picture (%s)", a->have ? "see the folder" : "no picture yet");
}

/* ---- Photo Booth: effects, countdown, flash, four pictures in one -------------------------------------------------------------- */

static void conf_save(const struct app *a)
{
    char extra[160];
    char size[40] = "";
    if (a->saved_size.w > 0)
        snprintf(size, sizeof(size), "size = %dx%d\n", a->saved_size.w, a->saved_size.h);
    snprintf(extra, sizeof(extra), "timer = %d\nfour-up = %d\nsound = %d\nvideo = %d\n%s", a->timer_s, a->fourup ? 1 : 0, a->sound ? 1 : 0, a->video ? 1 : 0, size);
    lm_snd_conf_save_x("camera", false, a->mic, extra);
}

static void conf_load(struct app *a)
{
    char path[1024], line[128];
    a->timer_s = 3;                                             /* Photo Booth counts three seconds */
    a->sound = true;
    if (lm_snd_conf_path("camera", path, sizeof(path))) {
        FILE *f = fopen(path, "r");
        while (f && fgets(line, sizeof(line), f)) {
            int v, sw, sh;
            if (sscanf(line, "timer = %d", &v) == 1)
                a->timer_s = LM_CLAMP(v, 0, 60);
            else if (sscanf(line, "four-up = %d", &v) == 1)
                a->fourup = v != 0;
            else if (sscanf(line, "sound = %d", &v) == 1)
                a->sound = v != 0;
            else if (sscanf(line, "video = %d", &v) == 1)
                a->video = v != 0;
            else if (sscanf(line, "size = %dx%d", &sw, &sh) == 2 && sw >= 16 && sh >= 16 && sw <= 8192 && sh <= 8192)
                a->saved_size = (struct size){ sw, sh };
        }
        if (f)
            fclose(f);
    }
    if (!a->want.w && a->saved_size.w > 0)                      /* no --size: the resolution of last time (the nearest the camera has) */
        a->want = a->saved_size;
    const char *env = getenv("MELLAGUI_CAM_TIMER");
    if (env && *env)
        a->timer_s = LM_CLAMP(atoi(env), 0, 60);
}

static void snd(const struct app *a, int kind)
{
    if (a->sound)
        camsound_play(kind);
}

/* A backdrop effect needs the scene with nobody in it: wait (the countdown of the shutter, else 3 s; $MELLAGUI_CAM_BG_MS) for the person to step out. */
static void bg_start(struct app *a)
{
    if (!a->have || !a->img.px) {
        set_note(a, "%s", "No picture to take the background from yet");
        return;
    }
    const char *e = getenv("MELLAGUI_CAM_BG_MS");
    a->bg_until = now_ms() + (e && *e ? (uint64_t)atoll(e) : (uint64_t)(a->timer_s > 0 ? a->timer_s : 3) * 1000);
    a->last_tick = 0;
    a->phase = PH_IDLE;
    a->shot_n = 0;
    set_note(a, "%s", "Step out of the picture...");
}

static void set_fx(struct app *a, int fx)
{
    a->fx = LM_CLAMP(fx, 0, CAMFX_COUNT - 1);
    a->bg_until = 0;
    a->focus_x = a->focus_y = 0.5f;
    fx_update(a);
    if (camfx_is_backdrop(a->fx))
        bg_start(a);
    else if (camfx_has_focus(a->fx))
        set_note(a, "%s", "Drag on the picture to move the effect");
    a->win.redraw = true;
}

static void grid_set(struct app *a, bool open)
{
    a->fx_open = open;
    if (open) {
        a->fx_page = a->fx / FX_PER_PAGE;
        a->fx_hover = a->fx % FX_PER_PAGE;                       /* the keys start on the effect in use */
    }
    a->ohover = -1;
    a->win.redraw = true;
}

static void save_fourup(struct app *a)
{
    int w2 = a->quad[0].w, h2 = a->quad[0].h;
    struct lm_image out = { 0 };
    for (int i = 0; i < 4; i++)
        if (!a->quad[i].px || a->quad[i].w != w2 || a->quad[i].h != h2) {
            set_note(a, "%s", "The four pictures do not fit together");
            return;
        }
    out.w = w2 * 2;
    out.h = h2 * 2;
    out.px = calloc((size_t)out.w * (size_t)out.h, 4);
    if (!out.px) {
        set_note(a, "%s", "Out of memory");
        return;
    }
    for (int i = 0; i < 4; i++)
        for (int y = 0; y < h2; y++)
            memcpy(out.px + (size_t)((i / 2) * h2 + y) * (size_t)out.w + (size_t)((i % 2) * w2), a->quad[i].px + (size_t)y * (size_t)w2, (size_t)w2 * 4);
    char name[96];
    if (save_snapshot(a, &out, name, sizeof(name)))
        set_note(a, "Saved %s", name);
    else
        set_note(a, "%s", "Cannot save the picture (see the folder)");
    free(out.px);
}

/* The moment has come: one picture - or the next of four. */
static void take_shot(struct app *a)
{
    snd(a, CAMSOUND_SHUTTER);
    a->flash_t0 = now_ms();
    a->win.redraw = true;
    if (!a->fourup) {
        a->phase = PH_IDLE;
        do_snapshot(a);
        return;
    }
    const struct lm_image *im = shown(a);
    struct lm_image *q = &a->quad[a->shot_n];
    int w2 = im->w / 2, h2 = im->h / 2;
    if (w2 < 1 || h2 < 1 || !image_size(q, w2, h2)) {
        a->phase = PH_IDLE;
        a->shot_n = 0;
        set_note(a, "%s", "Cannot take the pictures");
        return;
    }
    for (int y = 0; y < h2; y++)
        for (int x = 0; x < w2; x++) {
            const uint32_t *p0 = im->px + (size_t)(2 * y) * (size_t)im->w + (size_t)(2 * x), *p1 = p0 + im->w;
            unsigned c0 = p0[0], c1 = p0[1], c2 = p1[0], c3 = p1[1], out = 0xff000000u;
            for (int sh = 0; sh <= 16; sh += 8)
                out |= ((((c0 >> sh) & 255) + ((c1 >> sh) & 255) + ((c2 >> sh) & 255) + ((c3 >> sh) & 255) + 2) / 4) << sh;
            q->px[(size_t)y * (size_t)w2 + (size_t)x] = out;
        }
    if (++a->shot_n < 4) {
        a->phase = PH_WAIT;
        a->digits = false;
        a->t_shot = now_ms() + FOURUP_GAP_MS;
        return;
    }
    a->phase = PH_IDLE;
    a->shot_n = 0;
    save_fourup(a);
}

/* The shutter: a picture now (no countdown), or the countdown; pressed during a countdown it cancels. */
static void shutter(struct app *a)
{
    if (a->phase != PH_IDLE) {
        a->phase = PH_IDLE;
        a->shot_n = 0;
        set_note(a, "%s", "Cancelled");
        return;
    }
    if (!a->have || !a->img.px) {
        set_note(a, "%s", "No picture to take yet");
        return;
    }
    if (a->bg_until) {
        set_note(a, "%s", "Step out of the picture first: the empty scene is being taken");
        return;
    }
    if (a->fx_open)
        grid_set(a, false);
    a->shot_n = 0;
    a->last_tick = 0;
    if (a->timer_s <= 0) {
        take_shot(a);
        return;
    }
    a->phase = PH_WAIT;
    a->digits = true;
    a->t_shot = now_ms() + (uint64_t)a->timer_s * 1000;
    a->win.redraw = true;
}

/* Once per turn of the loop: the countdown runs down, the flash fades. */
static void booth_tick(struct app *a, uint64_t t)
{
    if (a->bg_until) {
        a->win.redraw = true;
        if (!a->have || !a->img.px) {
            a->bg_until = 0;
        } else if (t >= a->bg_until) {                           /* the empty scene: this picture */
            if (image_size(&a->ref, a->img.w, a->img.h)) {
                memcpy(a->ref.px, a->img.px, (size_t)a->img.w * (size_t)a->img.h * 4);
                a->have_ref = true;
            }
            a->bg_until = 0;
            snd(a, CAMSOUND_SHUTTER);
            a->flash_t0 = t;
            fx_update(a);
            set_note(a, "%s", a->have_ref ? "Background taken (B takes it again)" : "Cannot take the background");
        } else {
            int secs = (int)((a->bg_until - t + 999) / 1000);
            if (secs != a->last_tick && secs <= 10) {
                a->last_tick = secs;
                snd(a, secs <= 1 ? CAMSOUND_TOCK : CAMSOUND_TICK);
            }
        }
    }
    if (a->phase == PH_WAIT) {
        a->win.redraw = true;
        if (a->digits && t < a->t_shot) {
            int secs = (int)((a->t_shot - t + 999) / 1000);
            if (secs != a->last_tick) {
                a->last_tick = secs;
                snd(a, secs <= 1 ? CAMSOUND_TOCK : CAMSOUND_TICK);
            }
        }
        if (t >= a->t_shot) {
            if (!a->have || !a->img.px) {                       /* the camera went away meanwhile */
                a->phase = PH_IDLE;
                a->shot_n = 0;
                set_note(a, "%s", "The camera went away");
            } else {
                take_shot(a);
            }
        }
    }
    if (a->flash_t0) {
        a->win.redraw = true;
        if (t - a->flash_t0 >= FLASH_MS)
            a->flash_t0 = 0;
    }
}

static void do_record(struct app *a);

static void press_ctl(struct app *a, int i)
{
    switch (i) {
    case O_FX: grid_set(a, !a->fx_open); break;
    case O_KIND:                                                /* photo <-> video (not while a clip is being made or a picture is coming) */
        if (!a->clip && a->phase == PH_IDLE && !a->bg_until) {
            a->video = !a->video;
            conf_save(a);
        }
        break;
    case O_SHUTTER:
        if (a->video)
            do_record(a);
        else
            shutter(a);
        break;
    case O_MIC:
        if (!a->clip) {                                         /* (a clip that runs keeps what it started with) */
            a->mic = !a->mic;
            conf_save(a);
        }
        break;
    case O_MODE:
        if (a->phase == PH_IDLE) {
            a->fourup = !a->fourup;
            conf_save(a);
        }
        break;
    case O_TIMER:
        if (a->phase == PH_IDLE) {
            a->timer_s = a->timer_s == 0 ? 3 : a->timer_s == 3 ? 10 : 0;
            conf_save(a);
        }
        break;
    }
    a->win.redraw = true;
}

static void do_record(struct app *a)
{
    if (a->clip) {
        uint32_t frames = 0;
        char path[sizeof(a->clip->path)];
        snprintf(path, sizeof(path), "%s", a->clip->path);
        bool ok = clip_stop(a->clip, a->have ? shown(a) : NULL, &frames);
        free(a->clip);
        a->clip = NULL;
        if (ok) {
            const char *base = strrchr(path, '/');
            if (a->have)
                strip_push(a, path, shown(a));                  /* the clip goes into the strip with the newest picture as its poster */
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
    a->clip = clip_start(shown(a), a->videodir, NULL, a->mic, err, sizeof(err));
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
        a->have_ref = false;                                    /* (the empty scene was taken with another camera / size) */
        a->bg_until = 0;
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

/* The rail's drops. */
static void press(struct app *a, int b)
{
    switch (b) {
    case B_MIRROR: a->mirror = !a->mirror; break;
    case B_PAUSE:
        if (!a->clip)                                           /* a clip is made of the live pictures */
            a->paused = !a->paused;
        break;
    case B_SIZE: if (!a->clip && a->phase == PH_IDLE) menu_set(a, !a->menu_open); break;
    case B_CAM: if (!a->clip && a->phase == PH_IDLE) next_camera(a); break;
    }
    a->win.redraw = true;
}

static void pointer_event(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
    struct app *a = w->data;
    int old = a->hover, old_o = a->ohover, old_g = a->fx_hover, old_s = a->shover;
    bool old_f = a->fhover;
    switch (op) {
    case LM_WL_POINTER_EVT_ENTER:
        a->px = lm_fixed_to_double(args[2].f);
        a->py = lm_fixed_to_double(args[3].f);
        a->hover = btn_at(a, a->px, a->py);
        a->ohover = ctl_at(a, a->px, a->py);
        a->shover = a->fx_open ? -1 : strip_at(a, a->px, a->py);
        a->fhover = folder_at(a, a->px, a->py);
        a->pin = true;
        break;
    case LM_WL_POINTER_EVT_LEAVE:
        a->hover = -1;
        a->ohover = -1;
        a->shover = -1;
        a->fhover = false;
        a->pin = false;
        a->focus_drag = false;
        wlrail_release(&a->ran);
        break;
    case LM_WL_POINTER_EVT_MOTION:
        a->px = lm_fixed_to_double(args[1].f);
        a->py = lm_fixed_to_double(args[2].f);
        a->hover = btn_at(a, a->px, a->py);
        a->ohover = ctl_at(a, a->px, a->py);
        a->shover = a->fx_open ? -1 : strip_at(a, a->px, a->py);
        a->fhover = folder_at(a, a->px, a->py);
        a->pin = true;
        if (a->focus_drag && focus_active(a))
            focus_from(a, a->px, a->py);
        else if (focus_active(a) && pic_hit(a, a->px, a->py))
            w->redraw = true;                                   /* (the crosshair follows the pointer) */
        if (a->fx_open) {
            int g = grid_hit(a, a->px, a->py);
            if (g >= 0 && g < FX_PER_PAGE)
                a->fx_hover = g;
        }
        if (a->menu_open) {
            int row = menu_row_at(a, a->px, a->py);
            if (row >= 0 && row != a->menu_hover) {
                a->menu_hover = row;
                w->redraw = true;
            }
        }
        break;
    case LM_WL_POINTER_EVT_AXIS:
        if (a->menu_open && args[1].u == 0) {                   /* the wheel scrolls a long list */
            a->menu_top = LM_CLAMP(a->menu_top + (args[2].f > 0 ? 1 : -1) * 2, 0, LM_MAX(a->src.nsizes - menu_rows_fit(a), 0));
            w->redraw = true;
        } else if (args[1].u == 0 && strip_on(a) && a->px >= a->win.bw - STRIP_COL) {         /* ... and the strip's column: more pictures */
            a->strip_top += args[2].f > 0 ? 1 : -1;
            strip_clamp(a);
            a->shover = strip_at(a, a->px, a->py);
            w->redraw = true;
        }
        break;
    case LM_WL_POINTER_EVT_BUTTON:
        if (a->menu_open && args[2].u == 272 && args[3].u == 1) {           /* any press: pick an entry, or just close the list */
            int row = menu_row_at(a, a->px, a->py);
            if (row >= 0)
                menu_pick(a, row);
            else
                menu_set(a, false);
            break;
        }
        if (args[2].u == 272 && args[3].u == 1) {               /* BTN_LEFT pressed */
            int b = btn_at(a, a->px, a->py);
            int o = b < 0 ? ctl_at(a, a->px, a->py) : -1;
            if (o >= 0) {                                       /* the row of controls under the picture (also while the grid shows) */
                press_ctl(a, o);
                break;
            }
            if (b < 0 && a->fx_open) {                          /* the grid of effects: a tile chooses, the arrows turn the page, anywhere else closes it */
                int g = grid_hit(a, a->px, a->py);
                if (g >= 0 && g < FX_PER_PAGE) {
                    set_fx(a, a->fx_page * FX_PER_PAGE + g);
                    grid_set(a, false);
                } else if (g == NAV_PREV || g == NAV_NEXT) {
                    a->fx_page = LM_CLAMP(a->fx_page + (g == NAV_PREV ? -1 : 1), 0, FX_PAGES - 1);
                    a->fx_hover = -1;
                } else {
                    grid_set(a, false);
                }
                w->redraw = true;
                break;
            }
            int sp = b < 0 ? strip_at(a, a->px, a->py) : -1;
            if (sp >= 0) {                                      /* a picture of the strip: look at it */
                spawn_detached("MELLAGUI_CAM_VIEWER", "mellagui-view", a->strip[sp].path);
                break;
            }
            if (b < 0 && folder_at(a, a->px, a->py)) {          /* the folder of the pictures in the file manager */
                mkdir(a->outdir, 0755);
                spawn_detached("MELLAGUI_CAM_FILES", "mellagui-files", a->outdir);
                break;
            }
            if (b < 0 && focus_active(a) && pic_hit(a, a->px, a->py)) {         /* the effect's point: where the button went down, then dragged */
                a->focus_drag = true;
                focus_from(a, a->px, a->py);
                break;
            }
            if (b >= 0) {
                wlrail_press(&a->ran, b, wlrail_now());
                press(a, b);
            }
        } else if (args[2].u == 272) {
            wlrail_release(&a->ran);                            /* (the drop springs back) */
            a->focus_drag = false;
            w->redraw = true;
        }
        break;
    }
    if (a->hover != old || a->ohover != old_o || a->fx_hover != old_g || a->shover != old_s || a->fhover != old_f)
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
    if (a->menu_open) {                                         /* the list of sizes: Up / Down / Enter / Esc */
        if (ks == LM_KS_Escape || c == 's' || c == 'S') {
            menu_set(a, false);
        } else if (ks == LM_KS_Up || ks == LM_KS_Down || ks == LM_KS_Page_Up || ks == LM_KS_Page_Down) {
            int step = ks == LM_KS_Up ? -1 : ks == LM_KS_Down ? 1 : ks == LM_KS_Page_Up ? -menu_rows_fit(a) : menu_rows_fit(a);
            a->menu_hover = LM_CLAMP(a->menu_hover + step, 0, LM_MAX(a->src.nsizes - 1, 0));
            menu_scroll_to_hover(a);
            w->redraw = true;
        } else if (ks == LM_KS_Return || ks == LM_KS_KP_Enter) {
            menu_pick(a, a->menu_hover);
        }
        return;
    }
    if (a->fx_open) {                                           /* the grid of effects */
        int n_on_page = LM_MIN(CAMFX_COUNT - a->fx_page * FX_PER_PAGE, FX_PER_PAGE);
        int step = ks == LM_KS_Left ? -1 : ks == LM_KS_Right ? 1 : ks == LM_KS_Up ? -3 : ks == LM_KS_Down ? 3 : 0;
        if (ks == LM_KS_Escape || c == 'e' || c == 'E') {
            grid_set(a, false);
            return;
        } else if (step) {
            a->fx_hover = LM_CLAMP((a->fx_hover < 0 ? 0 : a->fx_hover) + step, 0, n_on_page - 1);
            w->redraw = true;
            return;
        } else if (ks == LM_KS_Page_Up || ks == LM_KS_Page_Down || c == ',' || c == '.') {
            a->fx_page = LM_CLAMP(a->fx_page + (ks == LM_KS_Page_Up || c == ',' ? -1 : 1), 0, FX_PAGES - 1);
            a->fx_hover = 0;
            w->redraw = true;
            return;
        } else if ((ks == LM_KS_Return || ks == LM_KS_KP_Enter) && a->fx_hover >= 0 && a->fx_hover < n_on_page) {
            set_fx(a, a->fx_page * FX_PER_PAGE + a->fx_hover);
            grid_set(a, false);
            return;
        } else if (c >= '1' && c <= '9' && c - '1' < n_on_page) {
            set_fx(a, a->fx_page * FX_PER_PAGE + (c - '1'));
            grid_set(a, false);
            return;
        }
    }
    if (ks == LM_KS_Escape || c == 'q' || c == 'Q')
        w->running = false;
    else if (c == 'e' || c == 'E')
        press_ctl(a, O_FX);
    else if (c == '4' && !a->video)
        press_ctl(a, O_MODE);
    else if ((c == 't' || c == 'T') && !a->video)
        press_ctl(a, O_TIMER);
    else if (c == 'b' || c == 'B') {
        if (camfx_is_backdrop(a->fx) && a->phase == PH_IDLE)
            bg_start(a);
        else
            set_note(a, "%s", "B takes the background again: choose a backdrop effect first (Effects, last page)");
    } else if (c == 'o' || c == 'O') {
        a->sound = !a->sound;
        conf_save(a);
        set_note(a, "%s", a->sound ? "Sound on" : "Sound off");
    }
    else if (ks == LM_KS_Return || ks == LM_KS_KP_Enter || c == ' ')
        press_ctl(a, O_SHUTTER);                                /* a picture or, in video mode, start / stop the clip */
    else if (c == 'v' || c == 'V')
        press_ctl(a, O_KIND);
    else if (c == 'r' || c == 'R')
        do_record(a);
    else if (c == 'a' || c == 'A')
        press_ctl(a, O_MIC);
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
        fx_update(a);
        if (a->clip && !clip_frame(a->clip, shown(a))) {         /* ffmpeg died: say so and end the clip */
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
    wlrail_init(&a->ran);
    wlwin_set_inset(&a->win, WLRAIL_W);                     /* (after the connect, which clears the window; before the open: it makes room) */
    int ww = 680, wh = 560;
    const char *wenv = getenv("MELLAGUI_CAM_WINDOW");                   /* the window's first size (the tests keep it clear of the dock) */
    if (wenv && sscanf(wenv, "%dx%d", &ww, &wh) == 2) {
        ww = LM_CLAMP(ww, 360, 4000);
        wh = LM_CLAMP(wh, 240, 4000);
    } else {
        ww = 680;
        wh = 560;
    }
    if (wlwin_open(&a->win, "Camera", "mellagui.cam", ww, wh, 360, 240) < 0)
        return 1;
    a->mirror = true;
    a->hover = -1;
    a->ohover = -1;
    a->fx_hover = -1;
    a->shover = -1;
    strip_load(a);
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
        booth_tick(a, t);
        int timeout = a->phase != PH_IDLE || a->flash_t0 ? 33 : 250;                 /* (the seconds and the flash need frames of their own) */
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
        if (strip_lazy(a))                                       /* a picture of the folder came into view: its small copy, then the next */
            timeout = LM_MIN(timeout, 10);
        if (poster_step(a))                                      /* a clip's poster is being made by a child */
            timeout = LM_MIN(timeout, 60);
        {                                                        /* the drops of the rail are moving: a frame every 16 ms */
            struct wlrail L;
            cam_rail(a, &L);
            timeout = wlrail_loop(&a->win, &a->ran, &L, a->hover, timeout);
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
    free(a->fxi.px);
    free(a->ref.px);
    for (int i = 0; i < 4; i++)
        free(a->quad[i].px);
    for (int i = 0; i < a->nstrip; i++)
        free(a->strip[i].img.px);
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

static int cmd_snapshot(const char *file, const char *dev, struct size want, int fx, float fx_x, float fx_y)
{
    struct app a = { 0 };
    a.focus_x = fx_x;
    a.focus_y = fx_y;
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
    a.fx = fx;
    a.have = true;
    fx_update(&a);
    if (!write_png(file, shown(&a), false)) {
        fprintf(stderr, "mellagui-cam: cannot write %s: %s\n", file, strerror(errno));
        return 1;
    }
    free(a.img.px);
    free(a.fxi.px);
    return 0;
}

/* A clip without a window: `seconds` of the camera into `file`. */
static int cmd_clip(const char *file, const char *dev, struct size want, double seconds, bool mic, int fx, float fx_x, float fx_y)
{
    struct app a = { 0 };
    a.focus_x = fx_x;
    a.focus_y = fx_y;
    a.fx = fx;
    a.have = true;
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
    fx_update(&a);
    struct clip *c = clip_start(shown(&a), NULL, file, mic, err, sizeof(err));
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
        if (r > 0) {
            fx_update(&a);
            alive = clip_frame(c, shown(&a));
        }
        clip_pump(c);
    }
    uint32_t frames = 0;
    bool ok = clip_stop(c, shown(&a), &frames);
    free(c);
    src_close(&a.src);
    free(a.img.px);
    free(a.fxi.px);
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
    /* the effects: names, ids, no crash at any size, grey stays grey, the mirror is symmetric, same picture same result, the distortions leave the corners */
    for (int i = 0; i < CAMFX_COUNT; i++)
        if (!camfx_name(i) || !camfx_id(i) || camfx_by_id(camfx_id(i)) != i) {
            fprintf(stderr, "selftest: effect %d has no name or id\n", i);
            bad++;
        }
    if (camfx_by_id("nonsense") != -1 || camfx_by_id("SEPIA") != CAMFX_SEPIA || camfx_by_id(NULL) != -1) {
        fprintf(stderr, "selftest: effect lookup\n");
        bad++;
    }
    static const int sizes[][2] = { { 1, 1 }, { 2, 2 }, { 3, 5 }, { 17, 9 }, { 64, 48 } };
    for (int fx = 0; fx < CAMFX_COUNT; fx++)
        for (size_t k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
            int w = sizes[k][0], h = sizes[k][1];
            uint32_t *in = malloc((size_t)w * (size_t)h * 4), *o1 = malloc((size_t)w * (size_t)h * 4), *o2 = malloc((size_t)w * (size_t)h * 4);
            for (int i = 0; i < w * h; i++)
                in[i] = 0xff000000u | (uint32_t)(i * 2654435761u >> 8);
            camfx_apply(fx, in, w, h, o1);
            camfx_apply(fx, in, w, h, o2);
            bool opaque = true;
            for (int i = 0; i < w * h; i++)
                opaque = opaque && (o1[i] >> 24) == 0xff;
            if (memcmp(o1, o2, (size_t)w * (size_t)h * 4) || !opaque) {
                fprintf(stderr, "selftest: effect %s at %dx%d is %s\n", camfx_id(fx), w, h, opaque ? "not repeatable" : "not opaque");
                bad++;
            }
            free(in);
            free(o1);
            free(o2);
        }
    {
        struct lm_image src = { 0 }, dst = { 0 };
        image_size(&src, 64, 48);
        image_size(&dst, 64, 48);
        fake_frame(&src, 64, 48, 3);
        camfx_apply(CAMFX_BW, src.px, 64, 48, dst.px);
        bool grey = true;
        for (int i = 0; i < 64 * 48; i++)
            grey = grey && ((dst.px[i] >> 16) & 255) == ((dst.px[i] >> 8) & 255) && ((dst.px[i] >> 8) & 255) == (dst.px[i] & 255);
        if (!grey) {
            fprintf(stderr, "selftest: black & white has colour in it\n");
            bad++;
        }
        camfx_apply(CAMFX_SEPIA, src.px, 64, 48, dst.px);
        int warm = 0;
        for (int i = 0; i < 64 * 48; i++)
            warm += ((dst.px[i] >> 16) & 255) >= ((dst.px[i] >> 8) & 255) && ((dst.px[i] >> 8) & 255) >= (dst.px[i] & 255);
        if (warm != 64 * 48) {
            fprintf(stderr, "selftest: sepia is not warm everywhere (%d of %d)\n", warm, 64 * 48);
            bad++;
        }
        {                                                                          /* the focus: the mirror's axis moves, the lens effects follow it */
            struct lm_image f2 = { 0 };
            image_size(&f2, 64, 48);
            camfx_apply_at(CAMFX_MIRROR, src.px, NULL, 64, 48, dst.px, 16.0f / 63.0f, 0.5f);          /* the axis at column 16 */
            bool axis = true;
            for (int y = 0; y < 48; y++)
                axis = axis && dst.px[y * 64 + 10] == src.px[y * 64 + 10] && dst.px[y * 64 + 20] == src.px[y * 64 + 12];
            if (!axis) {
                fprintf(stderr, "selftest: the mirror does not reflect about the focus\n");
                bad++;
            }
            for (int fx = CAMFX_KALEIDO; fx <= CAMFX_TWIRL; fx++) {
                camfx_apply_at(fx, src.px, NULL, 64, 48, dst.px, 0.5f, 0.5f);
                camfx_apply_at(fx, src.px, NULL, 64, 48, f2.px, 0.25f, 0.7f);
                if (!memcmp(dst.px, f2.px, 64 * 48 * 4)) {
                    fprintf(stderr, "selftest: %s ignores its focus\n", camfx_id(fx));
                    bad++;
                }
            }
            camfx_apply_at(CAMFX_BW, src.px, NULL, 64, 48, f2.px, 0.1f, 0.9f);                         /* the others do not look at it */
            camfx_apply(CAMFX_BW, src.px, 64, 48, dst.px);
            if (memcmp(dst.px, f2.px, 64 * 48 * 4) || !camfx_has_focus(CAMFX_MIRROR) || !camfx_has_focus(CAMFX_VINTAGE) || camfx_has_focus(CAMFX_GLOW) ||
                camfx_has_focus(CAMFX_SUNSET)) {
                fprintf(stderr, "selftest: which effects have a focus\n");
                bad++;
            }
            free(f2.px);
        }
        camfx_apply(CAMFX_MIRROR, src.px, 64, 48, dst.px);
        bool sym = true;
        for (int y = 0; y < 48; y++)
            for (int x = 0; x < 32; x++)
                sym = sym && dst.px[y * 64 + x] == dst.px[y * 64 + 63 - x] && dst.px[y * 64 + x] == src.px[y * 64 + x];
        if (!sym) {
            fprintf(stderr, "selftest: the mirror is not symmetric (or the left half changed)\n");
            bad++;
        }
        for (int fx = CAMFX_SQUEEZE; fx <= CAMFX_DENT; fx++) {                       /* the corners are outside the ellipse: untouched */
            camfx_apply(fx, src.px, 64, 48, dst.px);
            if (dst.px[0] != src.px[0] || dst.px[63] != src.px[63] || dst.px[47 * 64] != src.px[47 * 64] || dst.px[47 * 64 + 63] != src.px[47 * 64 + 63]) {
                fprintf(stderr, "selftest: %s changed a corner\n", camfx_id(fx));
                bad++;
            }
        }
        /* the backdrops: where nothing differs from the reference the backdrop shows and the picture does not; where all differs the picture is there */
        struct lm_image src2 = { 0 }, o1 = { 0 }, o2 = { 0 };
        image_size(&src2, 64, 48);
        image_size(&o1, 64, 48);
        image_size(&o2, 64, 48);
        fake_frame(&src2, 64, 48, 40);
        for (int fx = CAMFX_SUNSET; fx <= CAMFX_NEON; fx++) {
            camfx_apply_ref(fx, src.px, src.px, 64, 48, o1.px);                    /* nobody in it: the same backdrop whatever the picture */
            camfx_apply_ref(fx, src2.px, src2.px, 64, 48, o2.px);
            if (memcmp(o1.px, o2.px, 64 * 48 * 4)) {
                fprintf(stderr, "selftest: %s shows the picture where nothing differs from the reference\n", camfx_id(fx));
                bad++;
            }
            uint32_t black[64 * 48];
            memset(black, 0, sizeof(black));
            camfx_apply_ref(fx, src.px, black, 64, 48, o2.px);                     /* everything differs: the picture stays, the backdrop is hidden */
            int kept = 0;
            for (int i = 0; i < 64 * 48; i++)
                kept += o2.px[i] == src.px[i];
            if (kept < 64 * 48 * 9 / 10 || !memcmp(o1.px, o2.px, 64 * 48 * 4)) {
                fprintf(stderr, "selftest: %s lost the picture where all differs from the reference (%d kept)\n", camfx_id(fx), kept);
                bad++;
            }
        }
        camfx_apply(CAMFX_SUNSET, src.px, 64, 48, o1.px);                          /* no reference: an oval keeps the picture in the middle, the corners are the backdrop */
        if (o1.px[24 * 64 + 32] != src.px[24 * 64 + 32] || o1.px[0] == src.px[0]) {
            fprintf(stderr, "selftest: the oval of a backdrop without a reference\n");
            bad++;
        }
        free(src2.px);
        free(o1.px);
        free(o2.px);
        free(src.px);
        free(dst.px);
    }
    for (int k = 0; k < CAMSOUND_COUNT; k++) {                                      /* the sounds: a short clip, not silent, not clipped to a square wave */
        static int16_t pcm[CAMSOUND_MAX];
        size_t n = camsound_render(k, pcm, CAMSOUND_MAX);
        int peak = 0;
        for (size_t i = 0; i < n; i++)
            peak = LM_MAX(peak, abs(pcm[i]));
        if (n < 1000 || n > CAMSOUND_MAX || peak < 3000 || peak > 32000 || !camsound_name(k)) {
            fprintf(stderr, "selftest: sound %d has %zu samples, peak %d\n", k, n, peak);
            bad++;
        }
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
    bool list = false, list_fx = false, mic = true, sys_unused = false;
    int fx = CAMFX_NORMAL;
    float fx_x = 0.5f, fx_y = 0.5f;
    double seconds = 5;
    lm_snd_conf_load("camera", &sys_unused, &mic);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--selftest"))
            return selftest();
        else if (!strcmp(argv[i], "--list"))
            list = true;
        else if (!strcmp(argv[i], "--effects"))
            list_fx = true;
        else if (!strcmp(argv[i], "--focus") && i + 1 < argc) {
            char junk;
            if (sscanf(argv[++i], "%f,%f%c", &fx_x, &fx_y, &junk) != 2 || fx_x < 0 || fx_x > 1 || fx_y < 0 || fx_y > 1) {
                fprintf(stderr, "mellagui-cam: --focus wants X,Y between 0 and 1 (for example 0.3,0.5)\n");
                return 2;
            }
        } else if (!strcmp(argv[i], "--effect") && i + 1 < argc) {
            fx = camfx_by_id(argv[++i]);
            if (fx < 0) {
                fprintf(stderr, "mellagui-cam: unknown effect \"%s\" (mellagui-cam --effects lists them)\n", argv[i]);
                return 2;
            }
        }
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
            fprintf(stderr, "usage: mellagui-cam [--device /dev/videoN] [--size WxH] [--mic|--no-mic] [--effect ID] [--focus X,Y] | --list | --effects | --snapshot FILE.png | --clip FILE.mp4 [--seconds N] | --selftest\n");
            return 2;
        }
    }
    if (list)
        return cmd_list();
    if (list_fx) {
        for (int i = 0; i < CAMFX_COUNT; i++)
            printf("%s|%s\n", camfx_id(i), camfx_name(i));
        return 0;
    }
    if (snap)
        return cmd_snapshot(snap, dev, want, fx, fx_x, fx_y);
    if (clip)
        return cmd_clip(clip, dev, want, seconds, mic, fx, fx_x, fx_y);

    static struct app a;                                         /* zero initialised */
    a.want = want;
    a.mic = mic;
    a.fx = fx;
    a.focus_x = fx_x;
    a.focus_y = fx_y;
    conf_load(&a);
    const char *dump = getenv("MELLAGUI_CAM_DUMP");
    if (dump && *dump)
        snprintf(a.dump, sizeof(a.dump), "%s", dump);
    if (dev) {
        snprintf(a.want_dev, sizeof(a.want_dev), "%s", dev);
        a.fixed_dev = true;
    }
    const char *dir = getenv("MELLAGUI_CAM_DIR"), *home = getenv("HOME");
    if (dir && *dir)
        snprintf(a.outdir, sizeof(a.outdir), "%s", dir);
    else
        snprintf(a.outdir, sizeof(a.outdir), "%s/Camera", home && *home ? home : ".");
    const char *vdir = getenv("MELLAGUI_CAM_VIDEO_DIR");
    if (vdir && *vdir)
        snprintf(a.videodir, sizeof(a.videodir), "%s", vdir);
    else
        snprintf(a.videodir, sizeof(a.videodir), "%s", a.outdir);          /* (clips next to the pictures) */
    a.src.fd = -1;
    int rc = run_window(&a);
    free(a.img.px);
    return rc;
}
