/* mellagui-cctv - live pictures of the network cameras in the house (XiongMai cameras, the ones the iCSee app talks to), a window of the wlwin family.
 *
 *   mellagui-cctv [--config FILE] [--size WxH]
 *   mellagui-cctv --list | --selftest | --watch
 *
 * The cameras are named in $XDG_CONFIG_HOME/mellagui/cctv.conf ($MELLAGUI_CCTV_CONF), one a line, at most eight:
 *
 *   # name    address[:port]   user    password ("-" = none)
 *   Yard      192.168.1.20     admin   secret
 *
 * All of them are shown side by side (their small streams); a click or a number key shows one alone with its big stream, Esc goes back. The program
 * talks to the cameras directly (dvrip.c), nothing leaves the local network; ffmpeg turns the H.264 / H.265 into pictures. A lost connection is made
 * again by itself, but a camera that REFUSES the login is not asked again until you say so (R): cameras lock accounts after a few wrong tries.
 *
 * S saves a picture and V starts / stops a recording (of the camera shown alone, else of every camera that is live): full size, into ~/CCTV
 * ($MELLAGUI_CCTV_DIR), cctvrec.c. M plays the sound of the camera shown alone ($MELLAGUI_CCTV_PLAYER = a program that takes 16 bit 8000 Hz mono
 * samples, or "none"). O opens the folder.
 * A camera shown alone can be steered: the arrow keys or the pad on its picture move it while they are held (cctvctl.c). P shows a strip with its four
 * positions: a click on one goes there (or 1..4), "Set here" (Shift+1..4) makes the view as it is that position - its small picture is kept beside the
 * list of cameras (cctv-positions/). N chooses the night picture and the lamp.
 * L chooses what happens when it raises an alarm (a card, a recording): that is done by `mellagui-cctv --watch`, a process without a window that goes on
 * when this one is closed (cctvalarm.c). A opens the recordings on its memory card, a day at a time (cctvarch.c): Enter plays one here as it arrives
 * from the camera - nothing is written to the disk; Space pauses, the arrows and the bar move in it, M gives its sound -, S saves it into the folder.
 *
 * Every camera has a thread of its own: it keeps the connection, feeds ffmpeg and takes its pictures; the window only shows the newest one.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "cctvalarm.h"
#include "cctvarch.h"
#include "cctvctl.h"
#include "cctvrec.h"
#include "dvrip.h"
#include "ffpipe.h"
#include "font.h"
#include "keymap.h"
#include "metrics.h"
#include "protocol.h"
#include "theme.h"
#include "uikit.h"
#include "util.h"
#include "wlc.h"
#include "wlcard.h"
#include "wlrail.h"
#include "wlsbar.h"
#include "wlwin.h"

#define MAX_CAMS 8
#define STAT_H 24
#define GAP 6
#define PENDING_MAX (8u << 20)              /* video waiting for a decoder that is too slow */
#define SILENT_MS 15000                     /* a stream with no pictures for this long is made again */

enum state { ST_IDLE, ST_CONNECTING, ST_WAITING, ST_LIVE, ST_RETRY, ST_REFUSED };
static const char *const STATE_NAME[] = { "idle", "connecting", "waiting", "live", "retry", "refused" };
enum { WANT_NONE, WANT_SUB, WANT_MAIN };
struct playback;

struct decoder {
    pid_t pid;
    int in, out;
    int codec, w, h;                        /* what it reads and the size of the pictures it makes */
    int src_w, src_h;
    uint32_t *px;                           /* the picture being read */
    size_t have;
};

/* A small copy of what a position shows (made when it was set from here). */
struct thumb {
    uint32_t *px;
    int w, h;
};

struct cam {
    /* from the file */
    char name[48], host[128], user[64], pass[96];
    int port;

    pthread_t thread;
    int wake[2];
    pthread_mutex_t lock;
    /* the window's wishes (under the lock) */
    int want;                               /* WANT_* */
    unsigned cmd;                           /* goes up when the connection has to be made again */
    int box_w, box_h;                       /* the room the picture has, in buffer pixels */
    bool quit;
    bool listen;                            /* play its sound */
    /* what the thread tells (under the lock) */
    enum state state;
    char msg[200];
    uint32_t *px;                           /* the newest picture */
    int w, h;
    int stream;                             /* which stream that picture came from */
    unsigned serial;                        /* goes up with every change worth painting */
    unsigned frames;
    int fps10, kbps;
    int src_w, src_h;
    double aspect;                          /* width / height of what the camera sees (the main stream's), 0 = not known */

    /* the thread's own */
    struct dv_session s;
    struct dv_demux demux;
    struct dv_packets packets;
    struct decoder dec;
    uint8_t *pend;
    size_t pend_len, pend_cap;
    bool need_key;
    int cur_stream;
    unsigned dec_failures;
    unsigned got_frames, got_bytes;         /* since the last count */
    int64_t last_picture;
    pid_t player;                           /* the program that plays the sound, 0 = none */
    int speaker;                            /* its input */
    bool mute_failed;                       /* it ended by itself: not started again before M */
    bool broken;                            /* the connection is to be dropped (cur message says why) */
    char why[200];
    /* the window's own */
    struct cctv_job *rec, *snap;
    struct cctv_ctl *ctl;                   /* steering, once the camera has been shown alone */
    struct cctv_view view;                  /* what it last told */
    bool asked;                             /* its settings, since it was last shown alone */
    unsigned seen, rec_seen;
    int rec_secs;
    struct lm_rect tile, pic;
    struct thumb pos[CCTV_POSITIONS], pos_new[CCTV_POSITIONS];      /* the positions' pictures; pos_new: waiting for the camera to agree */
    bool pos_loaded;
};

struct app {
    struct wlwin win;
    struct cam cam[MAX_CAMS];
    int n;
    int single;                             /* the camera shown alone, -1 = all */
    int hover, rhover, shover;              /* the tile, the button of the left and of the right rail under the pointer */
    double px, py;
    struct wlrail_anim ran, san;
    const char *conf;
    bool sound;                             /* M */
    int moving, move_dir;                   /* the camera that is being moved (-1 = none) and which way */
    uint32_t move_key;                      /* the key held for it, 0 = the pad is */
    int menu, menu_cam;                     /* the menu that is open: MENU_* */
    int menu_do[16];                        /* what its entries do */
    struct cctv_want want[MAX_CAMS];        /* what to do on an alarm */
    bool pos_open;                          /* the panel of positions (of the camera shown alone) */
    int pos_ask;                            /* the position that is asked about before it is replaced, -1 = none */
    int64_t pos_ask_until;
    char wants_path[PATH_MAX];
    /* the card's recordings of a day */
    int arch;                               /* the camera whose recordings are shown, -1 = none */
    struct tm day;
    struct cctv_arch *list, *fetch;
    struct playback *play;                  /* the recording that is being watched, NULL = the list shows */
    int play_file;                          /* which row it is */
    struct dv_file *files;
    int nfiles, *shown, nshown;             /* shown: the files in the list, newest first (movement only: those) */
    int sel, top, bar_grab;
    bool motion_only, loading, bar_held;
    unsigned list_seen, fetch_seen;
    int fetch_pct;
    char fetch_what[16], arch_msg[240];
    int64_t click_ms;
    int click_row;
    char note[600];                         /* a word for the status line */
    int64_t note_until;
    char dir[400];                          /* where pictures and recordings go */
    char conf_note[300];                    /* why there are no cameras */
};

static const char *g_dump;
static volatile sig_atomic_t g_end;          /* SIGTERM / SIGINT: the recordings are finished first */
static void on_end(int sig) { (void)sig; g_end = 1; }
static const char *g_ffmpeg;

static int64_t now_ms(void) { return wlrail_now(); }

/* ---- the file ------------------------------------------------------------------------------------------------------------ */

static void conf_path(char *out, size_t cap, const char *cli)
{
    const char *env = getenv("MELLAGUI_CCTV_CONF"), *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (cli)
        snprintf(out, cap, "%s", cli);
    else if (env && *env)
        snprintf(out, cap, "%s", env);
    else if (xdg && *xdg)
        snprintf(out, cap, "%s/mellagui/cctv.conf", xdg);
    else
        snprintf(out, cap, "%s/.config/mellagui/cctv.conf", home ? home : ".");
}

/* One line: name address[:port] user password. 1 = a camera, 0 = empty or a comment, -1 = not understood. */
static int parse_line(char *line, struct cam *c)
{
    char *save = NULL, *name = strtok_r(line, " \t\r\n", &save);
    if (!name || *name == '#')
        return 0;
    char *addr = strtok_r(NULL, " \t\r\n", &save), *user = strtok_r(NULL, " \t\r\n", &save), *pass = strtok_r(NULL, "\r\n", &save);
    if (!addr || !user)
        return -1;
    while (pass && (*pass == ' ' || *pass == '\t'))
        pass++;
    if (pass)
        for (size_t n = strlen(pass); n && (pass[n - 1] == ' ' || pass[n - 1] == '\t'); n--)
            pass[n - 1] = 0;
    if (!pass || !strcmp(pass, "-"))
        pass = "";
    if (strlen(name) >= sizeof(c->name) || strlen(user) >= sizeof(c->user) || strlen(pass) >= sizeof(c->pass) || strlen(addr) >= sizeof(c->host))
        return -1;
    int port = DV_PORT;
    char *colon = strrchr(addr, ':');
    if (colon && colon == strchr(addr, ':')) {             /* (an IPv6 address has several: it takes no port here) */
        char junk;
        if (sscanf(colon + 1, "%d%c", &port, &junk) != 1 || port < 1 || port > 65535)
            return -1;
        *colon = 0;
    }
    snprintf(c->name, sizeof(c->name), "%s", name);
    snprintf(c->host, sizeof(c->host), "%s", addr);
    snprintf(c->user, sizeof(c->user), "%s", user);
    snprintf(c->pass, sizeof(c->pass), "%s", pass);
    c->port = port;
    return 1;
}

static void load_conf(struct app *a)
{
    FILE *f = fopen(a->conf, "r");
    if (!f) {
        snprintf(a->conf_note, sizeof(a->conf_note), "%s", errno == ENOENT ? "There is no list of cameras yet." : strerror(errno));
        return;
    }
    struct stat st;
    if (fstat(fileno(f), &st) == 0 && (st.st_mode & 077))
        fprintf(stderr, "mellagui-cctv: %s holds passwords and others may read it: chmod 600 it\n", a->conf);
    char line[512];
    int ln = 0;
    while (fgets(line, sizeof(line), f)) {
        ln++;
        struct cam c;
        memset(&c, 0, sizeof(c));
        int k = parse_line(line, &c);
        if (k < 0)
            fprintf(stderr, "mellagui-cctv: %s line %d is not understood (name address user password)\n", a->conf, ln);
        else if (k > 0 && a->n < MAX_CAMS)
            a->cam[a->n++] = c;
        else if (k > 0)
            fprintf(stderr, "mellagui-cctv: %s line %d: at most %d cameras\n", a->conf, ln, MAX_CAMS);
    }
    fclose(f);
    if (!a->n)
        snprintf(a->conf_note, sizeof(a->conf_note), "The list of cameras is empty.");
}

/* ---- the decoder --------------------------------------------------------------------------------------------------------- */

static void dec_stop(struct decoder *d)
{
    if (!d->pid)
        return;
    close(d->in);
    close(d->out);
    kill(d->pid, SIGKILL);                  /* it holds nothing worth saving */
    waitpid(d->pid, NULL, 0);
    free(d->px);
    memset(d, 0, sizeof(*d));
}

static bool dec_start(struct decoder *d, int codec, int w, int h)
{
    int in[2], out[2];
    if (!g_ffmpeg || pipe2(in, O_CLOEXEC) < 0)
        return false;
    if (pipe2(out, O_CLOEXEC) < 0) {
        close(in[0]);
        close(in[1]);
        return false;
    }
    uint32_t *px = malloc((size_t)w * (size_t)h * 4);
    char vf[64];
    snprintf(vf, sizeof(vf), "scale=%d:%d:flags=bilinear", w, h);
    pid_t pid = px ? fork() : -1;
    if (pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        dup2(in[0], 0);
        dup2(out[1], 1);
        /* (not -fflags nobuffer: with it ffmpeg sometimes throws away the key frame it probed with, and then nothing comes) */
        execl(g_ffmpeg, g_ffmpeg, "-hide_banner", "-loglevel", "quiet", "-flags", "low_delay", "-probesize", "32", "-analyzeduration", "0",
              "-f", codec == DV_CODEC_H265 ? "hevc" : "h264", "-i", "pipe:0", "-an", "-sn", "-vf", vf, "-fps_mode", "passthrough", "-f", "rawvideo", "-pix_fmt", "bgr0",
              "-flush_packets", "1", "pipe:1", (char *)NULL);
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    if (pid < 0) {
        close(in[1]);
        close(out[0]);
        free(px);
        return false;
    }
    memset(d, 0, sizeof(*d));
    d->pid = pid;
    d->in = in[1];
    d->out = out[0];
    d->codec = codec;
    d->w = w;
    d->h = h;
    d->px = px;
    fcntl(d->in, F_SETFL, O_NONBLOCK);
    fcntl(d->out, F_SETFL, O_NONBLOCK);
    if (fcntl(d->out, F_SETPIPE_SZ, 1 << 20) < 0) {}
    return true;
}

/* ---- a camera's thread ----------------------------------------------------------------------------------------------------- */

static void tell(struct cam *c, enum state st, const char *msg)
{
    pthread_mutex_lock(&c->lock);
    c->state = st;
    snprintf(c->msg, sizeof(c->msg), "%s", msg);
    if (st != ST_LIVE)
        c->fps10 = c->kbps = 0;
    c->serial++;
    pthread_mutex_unlock(&c->lock);
}

/* The shape of the picture: the small streams are often stored squeezed (704 x 576 for a 4:3 or 16:9 view), so the big stream's shape counts. */
static double picture_aspect(double known, int w, int h)
{
    bool squeezed = (w == 704 || w == 960 || w == 352) && (h == 576 || h == 480 || h == 288 || h == 240);
    if (squeezed)
        return known > 0 ? known : 4.0 / 3.0;
    return w > 0 && h > 0 ? (double)w / h : 4.0 / 3.0;
}

/* The size ffmpeg is to make: the picture's shape inside the box, never bigger than the source, even numbers. */
static void output_size(double aspect, int src_w, int src_h, int box_w, int box_h, int *w, int *h)
{
    double full_w = LM_MAX((double)src_w, src_h * aspect);
    double ow = LM_MIN((double)box_w, box_h * aspect);
    ow = LM_CLAMP(ow, 64.0, LM_MAX(full_w, 64.0));
    *w = ((int)(ow + 0.5) + 1) & ~1;
    *h = ((int)(ow / aspect + 0.5) + 1) & ~1;
    *w = LM_CLAMP(*w, 64, 4096);
    *h = LM_CLAMP(*h, 48, 4096);
}

static bool pend_add(struct cam *c, const void *p, size_t n)
{
    if (c->pend_len + n > c->pend_cap) {
        size_t want = c->pend_cap ? c->pend_cap : 1u << 18;
        while (want < c->pend_len + n)
            want *= 2;
        uint8_t *b = realloc(c->pend, want);
        if (!b)
            return false;
        c->pend = b;
        c->pend_cap = want;
    }
    memcpy(c->pend + c->pend_len, p, n);
    c->pend_len += n;
    return true;
}

/* ---- sound ---- */

static void player_stop(struct cam *c)
{
    if (!c->player)
        return;
    close(c->speaker);
    kill(c->player, SIGKILL);               /* silence at once: it would first play out what it holds */
    waitpid(c->player, NULL, 0);
    c->player = 0;
}

static bool player_start(struct cam *c)
{
    const char *own = getenv("MELLAGUI_CCTV_PLAYER");
    int p[2];
    if ((own && !strcmp(own, "none")) || pipe2(p, O_CLOEXEC) < 0)
        return false;
    pid_t pid = fork();
    if (pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        dup2(p[0], 0);
        if (own && *own) {
            execl(own, own, (char *)NULL);
        } else {
            execlp("paplay", "paplay", "--raw", "--format=s16le", "--rate=8000", "--channels=1", "--latency-msec=150", (char *)NULL);
            execlp("pw-cat", "pw-cat", "--playback", "--format=s16", "--rate=8000", "--channels=1", "-", (char *)NULL);
            execlp("aplay", "aplay", "-q", "-t", "raw", "-f", "S16_LE", "-r", "8000", "-c", "1", (char *)NULL);
        }
        _exit(127);
    }
    close(p[0]);
    if (pid < 0) {
        close(p[1]);
        return false;
    }
    c->player = pid;
    c->speaker = p[1];
    fcntl(c->speaker, F_SETFL, O_NONBLOCK);
    if (fcntl(c->speaker, F_SETPIPE_SZ, 4096) < 0) {}        /* a quarter of a second at most waits in the pipe */
    return true;
}

static int16_t alaw(uint8_t v)
{
    v ^= 0x55;
    int t = (v & 15) << 4, e = (v >> 4) & 7;
    t = e == 0 ? t + 8 : (t + 0x108) << (e - 1);
    return (int16_t)(v & 0x80 ? t : -t);
}

static void play(struct cam *c, const uint8_t *data, size_t len)
{
    pthread_mutex_lock(&c->lock);
    bool wanted = c->listen;
    pthread_mutex_unlock(&c->lock);
    if (!wanted) {
        player_stop(c);
        c->mute_failed = false;
        return;
    }
    if (c->mute_failed || (!c->player && !player_start(c)))
        return;
    int16_t out[1024];
    while (len) {
        size_t n = len < LM_ARRAY_LEN(out) ? len : LM_ARRAY_LEN(out);
        for (size_t i = 0; i < n; i++)
            out[i] = alaw(data[i]);
        if (write(c->speaker, out, n * 2) < 0 && errno != EAGAIN && errno != EINTR) {     /* (a full pipe: these samples are dropped) */
            player_stop(c);
            c->mute_failed = true;
            return;
        }
        data += n;
        len -= n;
    }
}

static void on_frame(void *ud, const struct dv_frame *f)
{
    struct cam *c = ud;
    if (f->kind == DV_AUDIO && f->codec == DV_SOUND_ALAW)
        play(c, f->data, f->len);
    if (f->kind != DV_VIDEO && f->kind != DV_VIDEO_KEY)
        return;
    if (f->kind == DV_VIDEO_KEY && (f->codec == DV_CODEC_H264 || f->codec == DV_CODEC_H265)) {
        pthread_mutex_lock(&c->lock);
        if (c->cur_stream == WANT_MAIN && f->w > 0 && f->h > 0)
            c->aspect = (double)f->w / f->h;
        c->src_w = f->w;
        c->src_h = f->h;
        double asp = picture_aspect(c->aspect, f->w, f->h);
        int bw = c->box_w, bh = c->box_h;
        pthread_mutex_unlock(&c->lock);
        int ow, oh;
        output_size(asp, f->w, f->h, bw > 0 ? bw : 640, bh > 0 ? bh : 480, &ow, &oh);
        /* a decoder lives until the stream changes, or the room has changed by a good deal; a new one begins with a key frame */
        struct decoder *d = &c->dec;
        if (!d->pid || d->codec != f->codec || d->src_w != f->w || d->src_h != f->h || abs(d->w - ow) * 8 > d->w || abs(d->h - oh) * 8 > d->h) {
            dec_stop(d);
            c->pend_len = 0;
            if (!dec_start(d, f->codec, ow, oh)) {
                c->broken = true;
                snprintf(c->why, sizeof(c->why), "ffmpeg cannot be started");
                return;
            }
            d->src_w = f->w;
            d->src_h = f->h;
        }
        c->need_key = false;
    }
    if (!c->dec.pid || c->need_key)
        return;
    if (c->pend_len + f->len > PENDING_MAX) {          /* the decoder cannot keep up: begin again at the next key frame */
        dec_stop(&c->dec);
        c->pend_len = 0;
        c->need_key = true;
        return;
    }
    /* an access unit delimiter behind the frame tells ffmpeg that it is complete: without it the picture waits for the next frame */
    static const uint8_t aud264[] = { 0, 0, 0, 1, 0x09, 0xf0 }, aud265[] = { 0, 0, 0, 1, 0x46, 0x01, 0x50 };
    bool hevc = c->dec.codec == DV_CODEC_H265;
    if (!pend_add(c, f->data, f->len) || !pend_add(c, hevc ? aud265 : aud264, hevc ? sizeof(aud265) : sizeof(aud264))) {
        c->broken = true;
        snprintf(c->why, sizeof(c->why), "out of memory");
    }
}

static void on_packet(void *ud, int msg, const uint8_t *body, size_t len)
{
    struct cam *c = ud;
    if (msg != DV_MSG_STREAM)
        return;
    c->got_bytes += (unsigned)len;
    if (!dv_demux_feed(&c->demux, body, len, on_frame, c)) {
        c->broken = true;
        snprintf(c->why, sizeof(c->why), "out of memory");
    }
}

static void publish(struct cam *c)
{
    struct decoder *d = &c->dec;
    pthread_mutex_lock(&c->lock);
    uint32_t *next = c->px;                 /* the picture shown until now becomes the one to read into, when it is as big */
    if (!next || c->w != d->w || c->h != d->h) {
        next = malloc((size_t)d->w * (size_t)d->h * 4);
        if (!next) {
            pthread_mutex_unlock(&c->lock);
            return;
        }
        free(c->px);
    }
    c->px = d->px;
    c->w = d->w;
    c->h = d->h;
    c->stream = c->cur_stream;
    c->frames++;
    c->serial++;
    bool first = c->state != ST_LIVE;
    if (first) {
        c->state = ST_LIVE;
        c->msg[0] = 0;
    }
    pthread_mutex_unlock(&c->lock);
    d->px = next;
    c->got_frames++;
    c->dec_failures = 0;
    c->last_picture = now_ms();
}

static void drain_wake(struct cam *c)
{
    char b[64];
    while (read(c->wake[0], b, sizeof(b)) > 0) {}
}

/* Takes the pictures the decoder has made and gives it what waits for it. */
static void dec_pump(struct cam *c, bool readable, bool writable)
{
    if (readable && c->dec.pid) {
        size_t total = (size_t)c->dec.w * (size_t)c->dec.h * 4;
        for (;;) {
            ssize_t k = read(c->dec.out, (uint8_t *)c->dec.px + c->dec.have, total - c->dec.have);
            if (k > 0) {
                c->dec.have += (size_t)k;
                if (c->dec.have == total) {
                    c->dec.have = 0;
                    publish(c);
                }
                continue;
            }
            if (k < 0 && (errno == EAGAIN || errno == EINTR))
                break;
            dec_stop(&c->dec);          /* ffmpeg gave up */
            c->pend_len = 0;
            c->need_key = true;
            if (++c->dec_failures >= 3) {
                snprintf(c->why, sizeof(c->why), "ffmpeg cannot decode this stream");
                c->broken = true;
            }
            break;
        }
    }
    if (writable && c->dec.pid && c->pend_len) {
        ssize_t k = write(c->dec.in, c->pend, c->pend_len);
        if (k > 0) {
            memmove(c->pend, c->pend + k, c->pend_len - (size_t)k);
            c->pend_len -= (size_t)k;
        } else if (k < 0 && errno != EAGAIN && errno != EINTR) {
            c->pend_len = 0;            /* (the end of ffmpeg shows on its output) */
        }
    }
}

/* Shows the stream until something goes wrong (c->why says what) or the window wants something else. */
static void watch(struct cam *c, unsigned cmd)
{
    int64_t now = now_ms(), next_alive = now + c->s.alive_s * 500, counted = now;
    c->last_picture = now;
    c->need_key = true;
    c->broken = false;
    c->got_frames = c->got_bytes = 0;
    c->dec_failures = 0;
    fcntl(c->s.data, F_SETFL, O_NONBLOCK);
    fcntl(c->s.ctl, F_SETFL, O_NONBLOCK);
    static __thread uint8_t buf[65536];
    while (!c->broken) {
        struct pollfd p[5] = { { .fd = c->wake[0], .events = POLLIN }, { .fd = c->s.data, .events = POLLIN }, { .fd = c->s.ctl, .events = POLLIN },
                               { .fd = c->dec.pid ? c->dec.out : -1, .events = POLLIN }, { .fd = c->dec.pid && c->pend_len ? c->dec.in : -1, .events = POLLOUT } };
        if (poll(p, 5, 500) < 0 && errno != EINTR)
            break;
        if (p[0].revents) {
            drain_wake(c);
            pthread_mutex_lock(&c->lock);
            bool stop = c->quit || c->cmd != cmd;
            pthread_mutex_unlock(&c->lock);
            if (stop)
                return;
        }
        /* the decoder first: its pictures belong to what was read before */
        dec_pump(c, p[3].revents != 0, p[4].revents != 0);
        if (p[1].revents) {
            ssize_t k = recv(c->s.data, buf, sizeof(buf), 0);
            if (k > 0) {
                if (!dv_packets_feed(&c->packets, buf, (size_t)k, on_packet, c)) {
                    snprintf(c->why, sizeof(c->why), "the camera sent something that is not understood");
                    break;
                }
            } else if (!(k < 0 && (errno == EAGAIN || errno == EINTR))) {
                snprintf(c->why, sizeof(c->why), "the connection was lost");
                break;
            }
        }
        if (p[2].revents) {                 /* answers on the first connection: nothing in them is needed */
            ssize_t k = recv(c->s.ctl, buf, sizeof(buf), 0);
            if (k == 0 || (k < 0 && errno != EAGAIN && errno != EINTR)) {
                snprintf(c->why, sizeof(c->why), "the connection was lost");
                break;
            }
        }
        now = now_ms();
        if (now >= next_alive) {
            next_alive = now + c->s.alive_s * 500;
            if (dv_keepalive(&c->s) < 0) {
                snprintf(c->why, sizeof(c->why), "the connection was lost");
                break;
            }
        }
        if (now - counted >= 2000) {
            pthread_mutex_lock(&c->lock);
            c->fps10 = (int)((int64_t)c->got_frames * 10000 / (now - counted));
            c->kbps = (int)((int64_t)c->got_bytes * 8 / (now - counted));
            c->serial++;
            pthread_mutex_unlock(&c->lock);
            c->got_frames = c->got_bytes = 0;
            counted = now;
        }
        if (now - c->last_picture > SILENT_MS) {
            snprintf(c->why, sizeof(c->why), "no picture arrives");
            break;
        }
    }
}

static void *cam_thread(void *ud)
{
    struct cam *c = ud;
    int backoff = 0;
    unsigned refused_at = 0;                /* the command that was refused: the same one is never tried again */
    bool refused = false;
    unsigned last_cmd = 0;
    for (;;) {
        drain_wake(c);
        pthread_mutex_lock(&c->lock);
        bool quit = c->quit;
        int want = c->want;
        unsigned cmd = c->cmd;
        pthread_mutex_unlock(&c->lock);
        if (quit)
            break;
        if (cmd != last_cmd) {
            backoff = 0;
            last_cmd = cmd;
        }
        if (refused && refused_at != cmd)
            refused = false;
        if (want == WANT_NONE || refused) {
            if (!refused)
                tell(c, ST_IDLE, "");
            struct pollfd p = { .fd = c->wake[0], .events = POLLIN };
            poll(&p, 1, -1);
            continue;
        }
        tell(c, ST_CONNECTING, "Connecting...");
        c->why[0] = 0;
        c->cur_stream = want;
        unsigned frames_before = c->frames;  /* (only this thread writes it) */
        int r = dv_login(&c->s, c->host, c->port, c->user, c->pass, c->wake[0], c->why, sizeof(c->why));
        if (r == 0) {
            tell(c, ST_WAITING, "Waiting for the picture...");
            r = dv_start(&c->s, want == WANT_SUB, c->wake[0], c->why, sizeof(c->why));
            if (r == 0)
                watch(c, cmd);
        }
        bool pictured = c->frames != frames_before;
        dec_stop(&c->dec);
        player_stop(c);
        c->mute_failed = false;
        dv_close(&c->s);
        c->demux.len = c->packets.len = 0;
        c->pend_len = 0;
        pthread_mutex_lock(&c->lock);
        quit = c->quit;
        bool changed = c->cmd != cmd;
        pthread_mutex_unlock(&c->lock);
        if (quit)
            break;
        if (changed)
            continue;                       /* what looked like a failure was the window changing its mind */
        if (r > 0) {
            refused = true;
            refused_at = cmd;
            char m[240];
            snprintf(m, sizeof(m), "%s. Press R to try again.", c->why);
            tell(c, ST_REFUSED, m);
            continue;
        }
        if (pictured)
            backoff = 0;
        backoff = backoff ? LM_MIN(backoff * 2, 30000) : 2000;
        char m[240];
        snprintf(m, sizeof(m), "%s - trying again in %d s", c->why[0] ? c->why : "the connection failed", backoff / 1000);
        tell(c, ST_RETRY, m);
        struct pollfd p = { .fd = c->wake[0], .events = POLLIN };
        poll(&p, 1, backoff);               /* (a new command wakes it at once) */
    }
    dv_demux_free(&c->demux);
    dv_packets_free(&c->packets);
    free(c->pend);
    return NULL;
}

/* ---- a recording of the card, played as it arrives --------------------------------------------------------------------------- */

#define PLAY_AHEAD_MS 3000                  /* pictures held before reading stops: the camera sends several times faster than real time */
#define PLAY_BYTES (24u << 20)
#define PLAY_SILENT_MS 5000
#define PLAY_STEP_MS 20000                  /* the arrows: a picture can begin only at a key frame, about ten seconds apart */

struct held {
    struct dv_frame f;                      /* (data is a copy of ours) */
    struct held *next;
};

struct playback {
    struct cam cam;                         /* the session, the decoder, the sound and the picture, as for a live one; its lock guards what follows too */
    struct dv_file file;
    long long begin, length_ms;
    /* the window's wishes */
    bool paused;
    long long seek_ms;                      /* -1 = none */
    /* what the thread tells */
    long long pos_ms;
    bool ended, failed;
    /* the thread's own: what has arrived and waits for its time */
    struct held *first, *last;
    unsigned nvideo;                        /* pictures waiting */
    size_t bytes;
    unsigned long long in_frames, in_samples;
    long long key0, key1;                   /* the first and the newest key frame that arrived (the camera's clock) */
    unsigned long long key0_frames, key1_frames;
    bool eof, keyed;                        /* keyed: the first key frame has come */
    unsigned passed;                        /* frames that were left out before it */
    long long gop, a_key;                   /* seconds between the file's key frames and one of their times, 0 = not seen (kept through seeks) */
};

static void held_clear(struct playback *p)
{
    while (p->first) {
        struct held *h = p->first;
        p->first = h->next;
        free((void *)h->f.data);
        free(h);
    }
    p->last = NULL;
    p->nvideo = 0;
    p->bytes = 0;
    p->in_frames = p->in_samples = 0;
    p->key0 = p->key1 = 0;
    p->eof = p->keyed = false;
}

static void play_arrived(void *ud, const struct dv_frame *f)
{
    struct playback *p = ud;
    bool video = f->kind == DV_VIDEO || f->kind == DV_VIDEO_KEY;
    if (!video && f->kind != DV_AUDIO)
        return;
    if (!p->keyed) {
        if (f->kind != DV_VIDEO_KEY) {
            p->passed++;
            return;                 /* a camera begins in the middle with pictures that need earlier ones: nothing of them is waited through */
        }
        p->keyed = true;
    }
    if (f->kind == DV_VIDEO_KEY && dv_seconds(f->time)) {
        long long t = dv_seconds(f->time);
        if (p->key1 && t > p->key1 && t - p->key1 <= 60)
            p->gop = t - p->key1;
        p->a_key = t;
        p->key1 = t;
        p->key1_frames = p->in_frames;
        if (!p->key0) {
            p->key0 = p->key1;
            p->key0_frames = p->key1_frames;
        }
    }
    if (video)
        p->in_frames++;
    else if (f->codec == DV_SOUND_ALAW)
        p->in_samples += f->len;
    struct held *h = malloc(sizeof(*h));
    uint8_t *copy = h ? malloc(f->len ? f->len : 1) : NULL;
    if (!copy) {
        free(h);
        p->cam.broken = true;
        snprintf(p->cam.why, sizeof(p->cam.why), "out of memory");
        return;
    }
    memcpy(copy, f->data, f->len);
    h->f = *f;
    h->f.data = copy;
    h->next = NULL;
    if (p->last)
        p->last->next = h;
    else
        p->first = h;
    p->last = h;
    p->nvideo += video;
    p->bytes += f->len;
}

static void play_packet(void *ud, int msg, const uint8_t *body, size_t len)
{
    struct playback *p = ud;
    if ((msg != DV_MSG_REPLAY && msg != DV_MSG_PLAY) || p->eof)
        return;
    if (!len) {
        p->eof = true;
        return;
    }
    if (!dv_demux_feed(&p->cam.demux, body, len, play_arrived, p)) {
        p->cam.broken = true;
        snprintf(p->cam.why, sizeof(p->cam.why), "out of memory");
    }
}

/* How long one picture lasts, in milliseconds; 0 = not known yet. The pictures carry no times and the rate the stream claims need not be kept: it is
 * counted from the sound that came with them, without sound from the clocks of the key frames. */
static double frame_ms(const struct playback *p)
{
    double ms = 0;
    if (p->in_samples >= 8000 && p->in_frames)
        ms = (double)p->in_samples / 8.0 / (double)p->in_frames;
    else if (!p->in_samples && p->key1 - p->key0 >= 2 && p->key1_frames > p->key0_frames)
        ms = (double)(p->key1 - p->key0) * 1000.0 / (double)(p->key1_frames - p->key0_frames);
    else if (p->eof || p->bytes > PLAY_BYTES / 2)
        ms = 100;
    return ms > 0 ? LM_CLAMP(ms, 10.0, 1000.0) : 0;
}

static void play_told(struct playback *p, long long pos, bool ended)
{
    pthread_mutex_lock(&p->cam.lock);
    if (p->seek_ms < 0 && (p->pos_ms / 200 != pos / 200 || p->ended != ended)) {
        p->pos_ms = pos;
        p->ended = ended;
        p->cam.serial++;
    }
    pthread_mutex_unlock(&p->cam.lock);
}

/* Plays what the camera sends until the window wants something else (true) or the connection fails (false). *at = where it is, in the file. */
static bool play_stream(struct playback *p, long long *at)
{
    struct cam *c = &p->cam;
    int64_t now = now_ms(), last_data = now, next_alive = now + c->s.alive_s * 500;
    double due = (double)now, pos = (double)*at;
    unsigned before = c->frames, fed_first = 0;
    c->need_key = true;
    c->broken = false;
    c->dec_failures = 0;
    fcntl(c->s.data, F_SETFL, O_NONBLOCK);
    fcntl(c->s.ctl, F_SETFL, O_NONBLOCK);
    static __thread uint8_t buf[65536];
    while (!c->broken) {
        pthread_mutex_lock(&c->lock);
        bool go = c->quit || p->seek_ms >= 0, paused = p->paused;
        pthread_mutex_unlock(&c->lock);
        if (go)
            return true;
        double ms = frame_ms(p);
        now = now_ms();
        /* what is due goes to the decoder and to the sound; while paused only as far as the first picture */
        while (p->first && ms > 0 && c->pend_len < (4u << 20)) {
            struct held *h = p->first;
            bool video = h->f.kind != DV_AUDIO;
            bool first = paused && c->frames == before && fed_first < 30;
            if (paused && !first)
                break;
            if (video && !first && (double)now < due)
                break;
            if (video) {
                if (h->f.kind == DV_VIDEO_KEY && dv_seconds(h->f.time))
                    pos = (double)(dv_seconds(h->f.time) - p->begin) * 1000.0;
                else
                    pos += ms;
                pos = LM_CLAMP(pos, 0.0, (double)p->length_ms);
                due = LM_MAX(due + ms, (double)now - 200.0);
                fed_first += c->frames == before;
                p->nvideo--;
            }
            if (video || !paused)
                on_frame(c, &h->f);
            p->first = h->next;
            if (!p->first)
                p->last = NULL;
            p->bytes -= h->f.len;
            free((void *)h->f.data);
            free(h);
        }
        if (paused || !p->first)
            due = LM_MAX(due, (double)now);     /* (what was waited for is not run after) */
        bool ended = p->eof && !p->first;
        if (ended)
            pos = (double)p->length_ms;
        *at = (long long)pos;
        play_told(p, *at, ended);
        bool want = !p->eof && p->bytes < PLAY_BYTES && (ms <= 0 || p->nvideo * ms < PLAY_AHEAD_MS);
        struct pollfd pf[5] = { { .fd = c->wake[0], .events = POLLIN }, { .fd = want ? c->s.data : -1, .events = POLLIN }, { .fd = c->s.ctl, .events = POLLIN },
                                { .fd = c->dec.pid ? c->dec.out : -1, .events = POLLIN }, { .fd = c->dec.pid && c->pend_len ? c->dec.in : -1, .events = POLLOUT } };
        int timeout = 50;
        if (!paused && p->first && ms > 0)
            timeout = (int)LM_CLAMP(due - (double)now, 0.0, 50.0);
        if (poll(pf, 5, timeout) < 0 && errno != EINTR)
            break;
        if (pf[0].revents)
            drain_wake(c);
        dec_pump(c, pf[3].revents != 0, pf[4].revents != 0);
        now = now_ms();
        if (pf[1].revents) {
            ssize_t k = recv(c->s.data, buf, sizeof(buf), 0);
            if (k > 0) {
                last_data = now;
                if (!dv_packets_feed(&c->packets, buf, (size_t)k, play_packet, p)) {
                    snprintf(c->why, sizeof(c->why), "the camera sent something that is not understood");
                    return false;
                }
            } else if (!(k < 0 && (errno == EAGAIN || errno == EINTR))) {
                snprintf(c->why, sizeof(c->why), "the connection was lost");
                if (p->key1 && p->key1 - p->begin >= p->length_ms / 1000 - 12)
                    p->eof = true;              /* (it closed at the file's end) */
                else
                    return false;
            }
        }
        if (!want)
            last_data = now;
        if (want && now - last_data > PLAY_SILENT_MS) {
            if (p->key1 && p->key1 - p->begin >= p->length_ms / 1000 - 12) {
                p->eof = true;                  /* nothing more comes: that was the file */
            } else {
                snprintf(c->why, sizeof(c->why), "the camera stopped sending");
                return false;
            }
        }
        if (pf[2].revents) {
            ssize_t k = recv(c->s.ctl, buf, sizeof(buf), 0);
            if (k == 0 || (k < 0 && errno != EAGAIN && errno != EINTR)) {
                snprintf(c->why, sizeof(c->why), "the connection was lost");
                return false;
            }
        }
        if (now >= next_alive) {
            next_alive = now + c->s.alive_s * 500;
            if (dv_keepalive(&c->s) < 0) {
                snprintf(c->why, sizeof(c->why), "the connection was lost");
                return false;
            }
        }
    }
    return false;
}

static void *play_thread(void *ud)
{
    struct playback *p = ud;
    struct cam *c = &p->cam;
    long long at = 0;
    int failures = 0;
    for (;;) {
        drain_wake(c);
        pthread_mutex_lock(&c->lock);
        bool quit = c->quit;
        if (p->seek_ms >= 0) {
            at = p->seek_ms;
            p->seek_ms = -1;
            p->pos_ms = at;
            p->ended = p->failed = false;
            c->serial++;
            failures = 0;
        }
        bool failed = p->failed;
        pthread_mutex_unlock(&c->lock);
        if (quit)
            break;
        if (failed) {                           /* nothing more is tried until the window asks for another place */
            struct pollfd pf = { .fd = c->wake[0], .events = POLLIN };
            poll(&pf, 1, -1);
            continue;
        }
        held_clear(p);
        c->demux.len = c->packets.len = 0;
        dec_stop(&c->dec);
        c->pend_len = 0;
        c->why[0] = 0;
        bool reused = c->s.ctl >= 0;
        int r = reused ? 0 : dv_login(&c->s, c->host, c->port, c->user, c->pass, c->wake[0], c->why, sizeof(c->why));
        struct dv_file from = p->file;          /* the file, from the place that is wanted */
        if (at > 0) {
            long long want = p->begin + at / 1000;
            if (p->gop > 0) {                   /* just before the key frame at or before it: little is sent in vain, and it comes quickly */
                long long off = (want - p->a_key) % p->gop;
                long long key = want - (off < 0 ? off + p->gop : off);
                want = LM_MAX(key - 1, p->begin);
            }
            time_t when = (time_t)want;
            struct tm t;
            gmtime_r(&when, &t);
            strftime(from.begin, sizeof(from.begin), "%Y-%m-%d %H:%M:%S", &t);
        }
        if (r == 0)
            r = dv_replay_start(&c->s, &from, c->wake[0], c->why, sizeof(c->why));
        long long began = at;
        bool changed = false;
        if (r == 0) {
            changed = play_stream(p, &at);
            pthread_mutex_lock(&c->lock);
            bool leaving = c->quit;             /* (then closing the connection is enough) */
            pthread_mutex_unlock(&c->lock);
            if (changed && !leaving)
                dv_replay_stop(&c->s, &from, -1);
        } else if (reused) {
            dv_close(&c->s);                    /* the old session is of no use any more: a new one, at once */
            continue;
        }
        pthread_mutex_lock(&c->lock);
        changed |= c->quit || p->seek_ms >= 0;  /* (what looked like a failure was the window changing its mind) */
        pthread_mutex_unlock(&c->lock);
        if (changed && r == 0)
            continue;
        dv_close(&c->s);
        player_stop(c);
        if (changed)
            continue;
        failures = at > began + 5000 ? 1 : failures + 1;
        if (r > 0 || failures > 3) {            /* a refused login is never tried again by itself */
            pthread_mutex_lock(&c->lock);
            p->failed = true;
            pthread_mutex_unlock(&c->lock);
            tell(c, r > 0 ? ST_REFUSED : ST_RETRY, c->why[0] ? c->why : "the connection failed");
            continue;
        }
        struct pollfd pf = { .fd = c->wake[0], .events = POLLIN };
        poll(&pf, 1, 1000 * failures);
    }
    held_clear(p);
    dec_stop(&c->dec);
    player_stop(c);
    dv_close(&c->s);
    dv_demux_free(&c->demux);
    dv_packets_free(&c->packets);
    free(c->pend);
    return NULL;
}

static void playback_free(struct playback *p)
{
    if (!p)
        return;
    pthread_mutex_lock(&p->cam.lock);
    p->cam.quit = true;
    pthread_mutex_unlock(&p->cam.lock);
    if (write(p->cam.wake[1], "x", 1) < 0) {}
    pthread_join(p->cam.thread, NULL);
    close(p->cam.wake[0]);
    close(p->cam.wake[1]);
    pthread_mutex_destroy(&p->cam.lock);
    free(p->cam.px);
    free(p);
}

static struct playback *playback_start(const struct cam *from, const struct dv_file *f, bool sound)
{
    struct playback *p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    struct cam *c = &p->cam;
    memcpy(c->name, from->name, sizeof(c->name));
    memcpy(c->host, from->host, sizeof(c->host));
    memcpy(c->user, from->user, sizeof(c->user));
    memcpy(c->pass, from->pass, sizeof(c->pass));
    c->port = from->port;
    c->cur_stream = WANT_MAIN;
    c->listen = sound;
    p->file = *f;
    p->begin = dv_seconds_text(f->begin);
    long long len = dv_seconds_text(f->end) - p->begin;
    p->length_ms = LM_CLAMP(len, 1LL, 86400LL) * 1000;
    p->seek_ms = -1;
    dv_init(&c->s);
    pthread_mutex_init(&c->lock, NULL);
    if (pipe2(c->wake, O_CLOEXEC | O_NONBLOCK) < 0) {
        free(p);
        return NULL;
    }
    if (pthread_create(&c->thread, NULL, play_thread, p) != 0) {
        close(c->wake[0]);
        close(c->wake[1]);
        free(p);
        return NULL;
    }
    return p;
}

static void poke(struct cam *c)
{
    if (write(c->wake[1], "x", 1) < 0) {}
}

/* What each camera is to show now. */
static void set_wants(struct app *a)
{
    for (int i = 0; i < a->n; i++) {
        struct cam *c = &a->cam[i];
        int want = a->arch >= 0 ? WANT_NONE : a->single < 0 ? WANT_SUB : a->single == i ? WANT_MAIN : WANT_NONE;
        pthread_mutex_lock(&c->lock);
        c->listen = a->sound && a->single == i;
        bool changed = c->want != want;
        if (changed) {
            c->want = want;
            if (c->state != ST_REFUSED)     /* a refused login stays refused until R */
                c->cmd++;
        }
        pthread_mutex_unlock(&c->lock);
        if (changed)
            poke(c);
    }
    if (a->play) {
        pthread_mutex_lock(&a->play->cam.lock);
        a->play->cam.listen = a->sound;
        pthread_mutex_unlock(&a->play->cam.lock);
    }
}

static void reconnect(struct app *a, int i)
{
    struct cam *c = &a->cam[i];
    pthread_mutex_lock(&c->lock);
    c->cmd++;
    if (c->state == ST_REFUSED)
        c->state = ST_CONNECTING;
    pthread_mutex_unlock(&c->lock);
    poke(c);
}

/* ---- the window ----------------------------------------------------------------------------------------------------------- */

/* ---- pictures, recordings, the folder ---- */

static void centred(struct wlwin *w, struct lm_rect r, const char *text, uint32_t color);
static void position_taken(struct app *a, int cam, int n);
struct pos_layout {
    struct lm_rect all, shut, tile[CCTV_POSITIONS], set[CCTV_POSITIONS];
};

static bool pos_layout(struct app *a, struct pos_layout *L);
static void chip(struct wlwin *w, int x, int y, const char *text, int max_w, uint32_t dot);

static void note(struct app *a, const char *text)
{
    snprintf(a->note, sizeof(a->note), "%s", text);
    a->note_until = now_ms() + 6000;
    a->win.redraw = true;
}

static struct cctv_job *start_job(struct app *a, struct cam *c, int kind)
{
    struct cctv_target t = { .name = c->name, .host = c->host, .port = c->port, .user = c->user, .password = c->pass };
    return cctv_job_start(&t, kind, a->dir, g_ffmpeg);
}

static bool is_live(struct cam *c)
{
    pthread_mutex_lock(&c->lock);
    bool live = c->state == ST_LIVE;
    pthread_mutex_unlock(&c->lock);
    return live;
}

/* S: a picture of the camera shown alone, else of every one that shows a picture. (Only those: their login is known to be good.) */
static void snapshot(struct app *a)
{
    int n = 0;
    for (int i = 0; i < a->n; i++) {
        struct cam *c = &a->cam[i];
        if ((a->single >= 0 && a->single != i) || !is_live(c))
            continue;
        if (!c->snap)
            c->snap = start_job(a, c, CCTV_SNAPSHOT);
        n += c->snap != NULL;
    }
    note(a, n ? "Taking the picture..." : "No camera shows a picture");
}

/* V: stop the recordings of what is shown; when there are none, start them. */
static void toggle_record(struct app *a)
{
    int stopped = 0, started = 0;
    for (int i = 0; i < a->n; i++)
        if ((a->single < 0 || a->single == i) && a->cam[i].rec) {
            cctv_job_stop(a->cam[i].rec);
            stopped++;
        }
    if (stopped) {
        note(a, "Finishing the recording...");
        return;
    }
    for (int i = 0; i < a->n; i++) {
        struct cam *c = &a->cam[i];
        if ((a->single >= 0 && a->single != i) || !is_live(c))
            continue;
        c->rec = start_job(a, c, CCTV_RECORD);
        c->rec_secs = -1;
        started += c->rec != NULL;
    }
    note(a, started ? "Starting to record..." : "No camera shows a picture");
}

/* What the jobs have to say; the ones that are over are put away. */
static void collect_jobs(struct app *a)
{
    for (int i = 0; i < a->n; i++) {
        struct cam *c = &a->cam[i];
        for (int k = 0; k < 2; k++) {
            struct cctv_job **j = k ? &c->rec : &c->snap;
            if (!*j)
                continue;
            struct cctv_status st;
            cctv_job_status(*j, &st);
            if (k) {
                int secs = st.state == CCTV_RECORDING ? (int)((now_ms() - st.since_ms) / 1000) : -1;
                if (secs != c->rec_secs || st.serial != c->rec_seen) {
                    c->rec_secs = secs;
                    c->rec_seen = st.serial;
                    a->win.redraw = true;
                }
            }
            if (st.state != CCTV_DONE && st.state != CCTV_FAILED)
                continue;
            char text[600];
            const char *base = strrchr(st.file, '/');
            if (st.state == CCTV_FAILED)
                snprintf(text, sizeof(text), "%s: %s", c->name, st.msg);
            else if (base)
                snprintf(text, sizeof(text), "Saved %s in %s", base + 1, a->dir);
            else
                snprintf(text, sizeof(text), "%s: nothing was recorded", c->name);
            cctv_job_free(*j);
            *j = NULL;
            note(a, text);
        }
    }
}

/* Starts a program of ours (or the one $env names; "none" = nothing) with one argument; nobody has to wait for it. */
static bool spawn_beside(const char *env, const char *name, const char *arg)
{
    const char *own = env ? getenv(env) : NULL;
    if (own && !strcmp(own, "none"))
        return false;
    char prog[PATH_MAX + 32];
    if (own && *own) {
        snprintf(prog, sizeof(prog), "%s", own);
    } else {
        ssize_t n = readlink("/proc/self/exe", prog, PATH_MAX - 1);
        char *slash = n > 0 ? (prog[n] = 0, strrchr(prog, '/')) : NULL;
        strcpy(slash ? slash + 1 : prog, name);
    }
    pid_t pid = fork();
    if (pid == 0) {
        if (fork() == 0) {
            setsid();
            execlp(prog, prog, arg, (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }
    if (pid > 0)
        waitpid(pid, NULL, 0);
    return pid > 0;
}

/* O: the folder in the file manager. */
static void open_folder(struct app *a)
{
    if (mkdir(a->dir, 0700) < 0 && errno != EEXIST) {
        note(a, "The folder cannot be made");
        return;
    }
    spawn_beside("MELLAGUI_CCTV_FILES", "mellagui-files", a->dir);
}

static void toggle_sound(struct app *a)
{
    if (a->single < 0) {
        note(a, "Show one camera alone to hear it");
        return;
    }
    a->sound = !a->sound;
    set_wants(a);
    note(a, a->sound ? "Sound on" : "Sound off");
}

/* ---- steering ---- */

static const char *const DIRECTION[4] = { "DirectionUp", "DirectionDown", "DirectionLeft", "DirectionRight" };
enum { MENU_NONE, MENU_NIGHT, MENU_ALARM };
#define DO_LAMP 100                         /* menu_do: a night value, DO_LAMP + a lamp mode, DO_GO / DO_SAVE + a position */
#define DO_NOTIFY 400
#define DO_RECORD 401
static const struct { const char *mode, *text; } LAMP[] = { { "Close", "Off" }, { "KeepOpen", "Always on" }, { "Auto", "Automatic" } };

/* The steering of a camera, made when it is first wanted. Only for one that shows a picture: its login is known to be good. */
static struct cctv_ctl *steer(struct app *a, int i)
{
    struct cam *c = &a->cam[i];
    if (!c->ctl && is_live(c)) {
        struct cctv_target t = { .name = c->name, .host = c->host, .port = c->port, .user = c->user, .password = c->pass };
        c->ctl = cctv_ctl_start(&t);
    }
    return c->ctl;
}

static void move_stop(struct app *a)
{
    if (a->moving < 0)
        return;
    cctv_ctl_move(a->cam[a->moving].ctl, NULL);
    a->moving = -1;
    a->move_key = 0;
    a->win.redraw = true;
}

static void move_start(struct app *a, int dir, uint32_t key)
{
    if (a->single < 0 || (a->moving == a->single && a->move_dir == dir))
        return;
    struct cctv_ctl *ctl = steer(a, a->single);
    if (!ctl)
        return;
    cctv_ctl_move(ctl, DIRECTION[dir]);     /* (the one before is stopped by it) */
    a->moving = a->single;
    a->move_dir = dir;
    a->move_key = key;
    a->win.redraw = true;
}

static void night_menu(struct app *a)
{
    if (a->single < 0) {
        note(a, "Show one camera alone to set it up");
        return;
    }
    struct cctv_ctl *ctl = steer(a, a->single);
    if (!ctl)
        return;
    move_stop(a);
    struct cctv_view v;
    cctv_ctl_view(ctl, &v);
    if (!v.known) {
        cctv_ctl_query(ctl);
        note(a, "Asking the camera, press N again in a moment");
        return;
    }
    static const struct { int value; bool lamp; const char *text; } NIGHT[] = {
        { CCTV_NIGHT_AUTO, false, "Automatic" }, { CCTV_NIGHT_INFRARED, true, "Infrared" }, { CCTV_NIGHT_LAMP, true, "Colour, with the lamp" },
        { CCTV_NIGHT_SMART, true, "Smart: lamp on movement" }, { CCTV_NIGHT_COLOUR, false, "Always colour" }, { CCTV_NIGHT_GREY, false, "Always black and white" } };
    char menu[600] = "";
    size_t o = 0;
    int n = 0;
    if (v.night >= 0) {
        o += (size_t)snprintf(menu + o, sizeof(menu) - o, "~Night picture\n");
        a->menu_do[n++] = -1;
        for (size_t k = 0; k < LM_ARRAY_LEN(NIGHT); k++) {
            if (NIGHT[k].lamp && !v.lamp[0])
                continue;
            o += (size_t)snprintf(menu + o, sizeof(menu) - o, "%s%s\n", v.night == NIGHT[k].value ? "* " : "   ", NIGHT[k].text);
            a->menu_do[n++] = NIGHT[k].value;
        }
    }
    if (v.lamp[0]) {
        o += (size_t)snprintf(menu + o, sizeof(menu) - o, "~Lamp\n");
        a->menu_do[n++] = -1;
        for (size_t k = 0; k < LM_ARRAY_LEN(LAMP); k++) {
            o += (size_t)snprintf(menu + o, sizeof(menu) - o, "%s%s\n", !strcmp(v.lamp, LAMP[k].mode) ? "* " : "   ", LAMP[k].text);
            a->menu_do[n++] = DO_LAMP + (int)k;
        }
    }
    if (!n) {
        note(a, "This camera has no night settings");
        return;
    }
    menu[o - 1] = 0;
    a->menu = wlwin_menu(&a->win, menu) ? MENU_NIGHT : MENU_NONE;
    a->menu_cam = a->single;
}

/* ---- alarms ---- */

static void watcher_start(const struct app *a)
{
    if (getenv("MELLAGUI_CCTV_NO_WATCH"))
        return;
    bool any = false;
    for (int i = 0; i < a->n; i++)
        any |= a->want[i].notify || a->want[i].record;
    if (!any)
        return;
    pid_t pid = fork();
    if (pid == 0) {
        if (fork() == 0) {
            setsid();
            int nul = open("/dev/null", O_RDWR);
            dup2(nul, 0);
            dup2(nul, 1);
            execl("/proc/self/exe", "mellagui-cctv", "--config", a->conf, "--watch", (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }
    if (pid > 0)
        waitpid(pid, NULL, 0);
}

/* ---- the positions: a strip of four pictures over the camera shown alone -------------------------------------------------------- */

#define THUMB_W 192                         /* the kept picture is at most this big */
#define THUMB_H 144
#define POS_GAP 8
#define POS_HEAD 26
#define POS_BTN 24
#define POS_ASK_MS 4000

/* The file of a position's picture: beside the list of cameras. */
static void position_path(const struct app *a, int cam, int n, char *out, size_t cap)
{
    const char *slash = strrchr(a->conf, '/');
    char name[48];
    snprintf(name, sizeof(name), "%s", a->cam[cam].name);
    for (char *p = name; *p; p++)
        if (*p == '/' || (unsigned char)*p < 0x20 || (p == name && *p == '.'))
            *p = '_';
    snprintf(out, cap, "%.*scctv-positions/%s-%d.ppm", slash ? (int)(slash - a->conf + 1) : 0, a->conf, name, n);
}

static void position_load(struct app *a, int cam)
{
    struct cam *c = &a->cam[cam];
    if (c->pos_loaded)
        return;
    c->pos_loaded = true;
    for (int n = 1; n <= CCTV_POSITIONS; n++) {
        char path[PATH_MAX + 80];
        position_path(a, cam, n, path, sizeof(path));
        FILE *f = fopen(path, "rb");
        int w = 0, h = 0, max = 0;
        if (f && fscanf(f, "P6 %d %d %d", &w, &h, &max) == 3 && fgetc(f) != EOF && max == 255 && w > 0 && h > 0 && w <= THUMB_W && h <= THUMB_H) {
            uint32_t *px = malloc((size_t)w * (size_t)h * 4);
            int i = 0;
            for (uint8_t rgb[3]; px && i < w * h && fread(rgb, 1, 3, f) == 3; i++)
                px[i] = 0xff000000u | (uint32_t)rgb[0] << 16 | (uint32_t)rgb[1] << 8 | rgb[2];
            if (px && i == w * h)
                c->pos[n - 1] = (struct thumb){ px, w, h };
            else
                free(px);
        }
        if (f)
            fclose(f);
    }
}

/* The picture as it is now, made small (each pixel the middle of the ones it stands for). */
static struct thumb position_picture(struct cam *c)
{
    struct thumb t = { 0 };
    pthread_mutex_lock(&c->lock);
    if (c->px && c->w > 0 && c->h > 0) {
        double k = LM_MIN(1.0, LM_MIN((double)THUMB_W / c->w, (double)THUMB_H / c->h));
        int w = LM_MAX((int)(c->w * k), 1), h = LM_MAX((int)(c->h * k), 1);
        t.px = malloc((size_t)w * (size_t)h * 4);
        for (int y = 0; t.px && y < h; y++)
            for (int x = 0; x < w; x++) {
                int x0 = x * c->w / w, x1 = LM_MAX((x + 1) * c->w / w, x0 + 1), y0 = y * c->h / h, y1 = LM_MAX((y + 1) * c->h / h, y0 + 1);
                unsigned r = 0, g = 0, b = 0, n = 0;
                for (int v = y0; v < y1; v++)
                    for (int u = x0; u < x1; u++, n++) {
                        uint32_t p = c->px[v * c->w + u];
                        r += p >> 16 & 255;
                        g += p >> 8 & 255;
                        b += p & 255;
                    }
                t.px[y * w + x] = 0xff000000u | (r / n) << 16 | (g / n) << 8 | b / n;
            }
        if (t.px) {
            t.w = w;
            t.h = h;
        }
    }
    pthread_mutex_unlock(&c->lock);
    return t;
}

/* The camera has taken position n: its new picture is shown and kept (a picture of a home: for the owner's eyes only). */
static void position_taken(struct app *a, int cam, int n)
{
    struct cam *c = &a->cam[cam];
    struct thumb t = c->pos_new[n - 1];
    if (!t.px)
        return;
    c->pos_new[n - 1] = (struct thumb){ 0 };
    free(c->pos[n - 1].px);
    c->pos[n - 1] = t;
    char path[PATH_MAX + 80], tmp[PATH_MAX + 100];
    position_path(a, cam, n, path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s", path);
    char *slash = strrchr(tmp, '/');
    if (slash) {
        *slash = 0;
        mkdir(tmp, 0700);
    }
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    unlink(tmp);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    FILE *f = fd >= 0 ? fdopen(fd, "wb") : NULL;
    if (!f) {
        if (fd >= 0)
            close(fd);
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", t.w, t.h);
    for (int i = 0; i < t.w * t.h; i++) {
        uint8_t rgb[3] = { (uint8_t)(t.px[i] >> 16), (uint8_t)(t.px[i] >> 8), (uint8_t)t.px[i] };
        fwrite(rgb, 1, 3, f);
    }
    if (fclose(f) != 0 || rename(tmp, path) < 0)
        unlink(tmp);
}

static bool pos_layout(struct app *a, struct pos_layout *L)
{
    if (!a->pos_open || a->single < 0 || a->arch >= 0)
        return false;
    struct lm_rect r = a->cam[a->single].tile;
    int tw = LM_MIN((r.w - 16 - (CCTV_POSITIONS + 1) * POS_GAP) / CCTV_POSITIONS, 150);
    if (tw < 48)
        return false;
    int th = tw * 3 / 4, w = CCTV_POSITIONS * tw + (CCTV_POSITIONS + 1) * POS_GAP, h = POS_HEAD + th + 6 + POS_BTN + POS_GAP;
    L->all = (struct lm_rect){ r.x + (r.w - w) / 2, r.y + r.h - h - 8, w, h };
    L->shut = (struct lm_rect){ L->all.x + w - 26, L->all.y + 3, 20, 20 };
    for (int i = 0; i < CCTV_POSITIONS; i++) {
        int x = L->all.x + POS_GAP + i * (tw + POS_GAP);
        L->tile[i] = (struct lm_rect){ x, L->all.y + POS_HEAD, tw, th };
        L->set[i] = (struct lm_rect){ x, L->all.y + POS_HEAD + th + 6, tw, POS_BTN };
    }
    return true;
}

static void positions_close(struct app *a)
{
    a->pos_open = false;
    a->pos_ask = -1;
    a->win.redraw = true;
}

/* P: the panel of the camera shown alone. */
static void positions_toggle(struct app *a)
{
    if (a->pos_open) {
        positions_close(a);
        return;
    }
    if (a->single < 0) {
        note(a, "Show one camera alone to move it");
        return;
    }
    if (!steer(a, a->single))
        return;
    position_load(a, a->single);
    a->pos_open = true;
    a->pos_ask = -1;
    a->note[0] = 0;
    a->win.redraw = true;
}

static void position_go(struct app *a, int i)
{
    struct cctv_ctl *ctl = steer(a, a->single);
    if (!ctl)
        return;
    move_stop(a);
    a->pos_ask = -1;
    cctv_ctl_position(ctl, i + 1, false);
    a->win.redraw = true;
}

/* The view as it is becomes position i + 1; one that has a picture is asked about first. */
static void position_set(struct app *a, int i)
{
    struct cctv_ctl *ctl = steer(a, a->single);
    if (!ctl)
        return;
    struct cam *c = &a->cam[a->single];
    move_stop(a);
    if (c->pos[i].px && a->pos_ask != i) {
        a->pos_ask = i;
        a->pos_ask_until = now_ms() + POS_ASK_MS;
        a->win.redraw = true;
        return;
    }
    a->pos_ask = -1;
    free(c->pos_new[i].px);
    c->pos_new[i] = position_picture(c);
    cctv_ctl_position(ctl, i + 1, true);
    a->win.redraw = true;
}

static bool in_rect(struct lm_rect r, double x, double y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

/* 0 = not on the panel; 1 + i = picture i; 101 + i = its button; -1 = the close mark; -2 = elsewhere on the panel. */
static int positions_at(struct app *a, double x, double y)
{
    struct pos_layout L;
    if (!pos_layout(a, &L) || !in_rect(L.all, x, y))
        return 0;
    if (in_rect(L.shut, x, y))
        return -1;
    for (int i = 0; i < CCTV_POSITIONS; i++) {
        if (in_rect(L.tile[i], x, y))
            return 1 + i;
        if (in_rect(L.set[i], x, y))
            return 101 + i;
    }
    return -2;
}

static void paint_positions(struct app *a)
{
    struct wlwin *w = &a->win;
    struct pos_layout L;
    if (!pos_layout(a, &L))
        return;
    struct cam *c = &a->cam[a->single];
    int over = positions_at(a, a->px, a->py);
    wlwin_round_fill(w, L.all.x, L.all.y, L.all.w, L.all.h, 10, 0x101418);
    wlwin_text(w, L.all.x + POS_GAP + 2, L.all.y + 5, "Positions - click a picture to go there", 1, 0xf0f2f5, L.all.w - 44);
    wlwin_text(w, L.shut.x + 6, L.shut.y + 2, "x", 1, over == -1 ? 0xffffff : 0x9aa0a8, 0);
    for (int i = 0; i < CCTV_POSITIONS; i++) {
        struct lm_rect t = L.tile[i], b = L.set[i];
        bool hot = over == 1 + i;
        wlwin_fill(w, t.x - 2, t.y - 2, t.w + 4, t.h + 4, hot ? lm_theme.accent : 0x2a3038);
        wlwin_fill(w, t.x, t.y, t.w, t.h, 0x0a0c0f);
        if (c->pos[i].px) {
            int pw = t.w, ph = LM_MAX(1, c->pos[i].h * pw / c->pos[i].w);
            if (ph > t.h) {
                ph = t.h;
                pw = LM_MAX(1, c->pos[i].w * ph / c->pos[i].h);
            }
            wlwin_blit(w, t.x + (t.w - pw) / 2, t.y + (t.h - ph) / 2, pw, ph, c->pos[i].px, c->pos[i].w, c->pos[i].h, c->pos[i].w);
        } else {
            centred(w, t, "no picture", 0x6c737c);
        }
        char num[4];
        snprintf(num, sizeof(num), "%d", i + 1);
        wlwin_round_fill(w, t.x + 4, t.y + 4, 20, 20, 10, lm_theme.accent);
        wlwin_text(w, t.x + 10, t.y + 6, num, 1, 0xffffff, 0);
        bool ask = a->pos_ask == i;
        const char *label = ask ? "Replace?" : "Set here";
        wlwin_drop(w, b.x, b.y, b.w, b.h, lm_button_radius(b.h / 2),
                   ask ? lm_theme.btn_red : over == 101 + i ? lm_theme.btn_green : lm_theme.btn_slate);
        int lw = wlcard_text_w(label);
        wlwin_text(w, b.x + LM_MAX((b.w - lw) / 2, 2), b.y + (b.h - LM_FONT_H) / 2 - 1, label, 1, lm_theme.on_colour, b.w - 4);
    }
}

static void alarm_menu(struct app *a)
{
    if (a->single < 0) {
        note(a, "Show one camera alone to set its alarms");
        return;
    }
    const struct cctv_want *wt = &a->want[a->single];
    /* (a camera that shows no picture is not given to the watcher: its login is not known to be good) */
    if (!wt->notify && !wt->record && !is_live(&a->cam[a->single]))
        return;
    move_stop(a);
    char menu[300];
    snprintf(menu, sizeof(menu), "~When this camera raises an alarm\n%sShow a notification\n%sRecord the movement", wt->notify ? "* " : "   ", wt->record ? "* " : "   ");
    a->menu_do[0] = -1;
    a->menu_do[1] = DO_NOTIFY;
    a->menu_do[2] = DO_RECORD;
    a->menu = wlwin_menu(&a->win, menu) ? MENU_ALARM : MENU_NONE;
    a->menu_cam = a->single;
}

static void change_want(struct app *a, int i, int what)
{
    struct cctv_want old = a->want[i];
    if (what == DO_NOTIFY)
        a->want[i].notify = !old.notify;
    else
        a->want[i].record = !old.record;
    const char *names[MAX_CAMS];
    for (int k = 0; k < a->n; k++)
        names[k] = a->cam[k].name;
    if (!cctv_wants_save(a->wants_path, names, a->want, a->n)) {
        a->want[i] = old;
        note(a, "The choice cannot be saved");
        return;
    }
    watcher_start(a);                       /* (one that runs reads the file anew by itself) */
    note(a, a->want[i].notify || a->want[i].record ? "The camera is watched, also when this window is closed" : "The camera is not watched any more");
}

/* ---- the recordings on the card ---- */

#define ROW_H 22
static const struct { int x; const char *name; } COLUMN[] = { { 14, "Time" }, { 110, "Length" }, { 180, "Kind" }, { 290, "Size" } };
#define HEAD_H 30
#define BAR_H 40
#define COL_H 20                            /* the headings of the columns */
#define ROWS_Y (HEAD_H + COL_H)

enum { AB_PREV, AB_NEXT, AB_MOTION, AB_OPEN, AB_SAVE, AB_N };

static char file_kind(const struct dv_file *f)
{
    const char *b = strchr(f->name, '[');
    return b ? b[1] : '?';
}

static const char *kind_text(char k)
{
    return k == 'R' ? "Regular" : k == 'M' ? "Movement" : k == 'A' ? "Alarm" : k == 'H' ? "By hand" : "Other";
}

static void day_string(const struct tm *t, char *out, size_t cap) { strftime(out, cap, "%Y-%m-%d", t); }

static void filter_files(struct app *a)
{
    free(a->shown);
    a->shown = a->nfiles ? malloc((size_t)a->nfiles * sizeof(int)) : NULL;
    a->nshown = 0;
    for (int i = a->nfiles - 1; a->shown && i >= 0; i--) {
        char k = file_kind(&a->files[i]);
        if (!a->motion_only || k == 'M' || k == 'A')
            a->shown[a->nshown++] = i;
    }
    a->sel = LM_CLAMP(a->sel, 0, LM_MAX(a->nshown - 1, 0));
    a->top = 0;
    a->win.redraw = true;
}

static void load_day(struct app *a)
{
    cctv_arch_free(a->list);
    free(a->files);
    a->files = NULL;
    a->nfiles = a->sel = a->top = 0;
    filter_files(a);
    struct cam *c = &a->cam[a->arch];
    struct cctv_target t = { .name = c->name, .host = c->host, .port = c->port, .user = c->user, .password = c->pass };
    char day[16];
    day_string(&a->day, day, sizeof(day));
    a->list = cctv_list_start(&t, day);
    a->list_seen = 0;
    a->loading = a->list != NULL;
    snprintf(a->arch_msg, sizeof(a->arch_msg), "Asking the camera...");
}

static void set_wants(struct app *a);
static void positions_close(struct app *a);

static void arch_close(struct app *a)
{
    if (a->arch < 0)
        return;
    playback_free(a->play);
    a->play = NULL;
    cctv_arch_free(a->list);
    cctv_arch_free(a->fetch);
    a->list = a->fetch = NULL;
    free(a->files);
    free(a->shown);
    a->files = NULL;
    a->shown = NULL;
    a->nfiles = a->nshown = 0;
    a->arch = -1;
    a->win.redraw = true;
    set_wants(a);
}

static void arch_open(struct app *a)
{
    if (a->arch >= 0) {
        arch_close(a);
        return;
    }
    if (a->single < 0) {
        note(a, "Show one camera alone to see its recordings");
        return;
    }
    if (!is_live(&a->cam[a->single]))
        return;
    move_stop(a);
    positions_close(a);
    a->arch = a->single;
    time_t now = time(NULL);
    localtime_r(&now, &a->day);
    a->motion_only = false;
    a->note[0] = 0;
    set_wants(a);                           /* the live picture rests meanwhile */
    load_day(a);
}

static void other_day(struct app *a, int by)
{
    struct tm t = a->day, today;
    t.tm_hour = 12;
    t.tm_mday += by;
    t.tm_isdst = -1;
    time_t when = mktime(&t), now = time(NULL);
    localtime_r(&now, &today);
    char d1[16], d2[16];
    localtime_r(&when, &t);
    day_string(&t, d1, sizeof(d1));
    day_string(&today, d2, sizeof(d2));
    if (strcmp(d1, d2) > 0)
        return;                             /* there is nothing from tomorrow */
    a->day = t;
    load_day(a);
}

/* NAME-YYYYMMDD-HHMMSS of a recording, fit for a file name. */
static void file_base(const struct cam *c, const struct dv_file *f, char *out, size_t cap)
{
    char name[48], stamp[20];
    snprintf(name, sizeof(name), "%s", c->name);
    for (char *p = name; *p; p++)
        if (*p == '/' || (unsigned char)*p < 0x20 || (p == name && (*p == '.' || *p == '-')))
            *p = '_';
    size_t n = 0;
    for (const char *p = f->begin; *p && n + 1 < sizeof(stamp); p++)
        if (*p >= '0' && *p <= '9')
            stamp[n++] = *p;
        else if (*p == ' ')
            stamp[n++] = '-';
    stamp[n] = 0;
    snprintf(out, cap, "%s-%s", name, stamp);
}

/* S: the chosen recording (or the one that is watched) is saved into the folder. */
static void save_file(struct app *a)
{
    int row = a->play ? a->play_file : a->sel;
    if (row < 0 || row >= a->nshown || !g_ffmpeg)
        return;
    if (a->fetch) {
        note(a, "One file is being fetched already (Esc stops it)");
        return;
    }
    struct cam *c = &a->cam[a->arch];
    const struct dv_file *f = a->play ? &a->play->file : &a->files[a->shown[row]];
    char base[96];
    file_base(c, f, base, sizeof(base));
    struct cctv_target t = { .name = c->name, .host = c->host, .port = c->port, .user = c->user, .password = c->pass };
    a->fetch = cctv_fetch_start(&t, f, a->dir, base, g_ffmpeg);
    a->fetch_pct = 0;
    a->fetch_seen = 0;
    snprintf(a->fetch_what, sizeof(a->fetch_what), "%.8s", strlen(f->begin) > 11 ? f->begin + 11 : f->begin);
    a->note[0] = 0;
    a->win.redraw = true;
}

/* Enter: the chosen recording plays, as it comes from the card. */
static void play_open(struct app *a)
{
    if (a->sel < 0 || a->sel >= a->nshown || !g_ffmpeg)
        return;
    playback_free(a->play);
    a->play_file = a->sel;
    a->play = playback_start(&a->cam[a->arch], &a->files[a->shown[a->sel]], a->sound);
    if (!a->play)
        note(a, "It cannot be played");
    a->note[0] = 0;
    a->win.redraw = true;
}

static void play_close(struct app *a)
{
    playback_free(a->play);
    a->play = NULL;
    a->win.redraw = true;
}

static void play_seek(struct app *a, long long ms)
{
    struct playback *p = a->play;
    pthread_mutex_lock(&p->cam.lock);
    ms = LM_CLAMP(ms, 0LL, LM_MAX(p->length_ms - 12000, 0LL));          /* (after the last key frame there is nothing to begin with) */
    p->seek_ms = p->pos_ms = ms;
    p->ended = p->failed = false;
    pthread_mutex_unlock(&p->cam.lock);
    if (write(p->cam.wake[1], "x", 1) < 0) {}
    a->win.redraw = true;
}

static void play_jump(struct app *a, long long by)
{
    pthread_mutex_lock(&a->play->cam.lock);
    long long at = a->play->pos_ms;
    pthread_mutex_unlock(&a->play->cam.lock);
    play_seek(a, at + by);
}

/* Space: stop and go on; at the end (or after a failure) from the beginning (from where it failed). */
static void play_pause(struct app *a)
{
    struct playback *p = a->play;
    pthread_mutex_lock(&p->cam.lock);
    bool again = p->ended || p->failed;
    long long at = p->failed ? p->pos_ms : 0;
    p->paused = again ? false : !p->paused;
    pthread_mutex_unlock(&p->cam.lock);
    if (again)
        play_seek(a, at);
    a->win.redraw = true;
}

static void collect_arch(struct app *a)
{
    if (a->arch < 0)
        return;
    struct cctv_arch_status st;
    if (a->list && a->loading) {
        cctv_arch_status(a->list, &st);
        if (st.done) {
            a->loading = false;
            a->nfiles = st.n;
            a->files = st.n ? malloc((size_t)st.n * sizeof(*a->files)) : NULL;
            if (a->files)
                memcpy(a->files, st.files, (size_t)st.n * sizeof(*a->files));
            else
                a->nfiles = 0;
            snprintf(a->arch_msg, sizeof(a->arch_msg), "%s", st.failed ? st.msg : "Nothing was recorded on this day (or the camera has no memory card)");
            filter_files(a);
        }
    }
    if (a->fetch) {
        cctv_arch_status(a->fetch, &st);
        if (st.serial != a->fetch_seen) {
            a->fetch_seen = st.serial;
            a->fetch_pct = st.kb ? (int)LM_MIN((uint64_t)st.got_kb * 100 / st.kb, (uint64_t)100) : 0;
            a->win.redraw = true;
        }
        if (st.done) {
            char text[600];
            const char *base = strrchr(st.path, '/');
            if (st.failed) {
                snprintf(text, sizeof(text), "%s: %s", a->fetch_what, st.msg);
            } else {
                snprintf(text, sizeof(text), "Saved %.120s in %s", base ? base + 1 : st.path, a->dir);
            }
            cctv_arch_free(a->fetch);
            a->fetch = NULL;
            note(a, text);
        }
    }
}

static int arch_rows(const struct app *a) { return LM_MAX((a->win.bh - STAT_H - ROWS_Y - BAR_H) / ROW_H, 1); }
static int arch_w(const struct app *a) { return a->win.bw - WLRAIL_W; }

static struct wlsbar arch_bar(const struct app *a)
{
    return (struct wlsbar){ arch_w(a) - wlsbar_w() - 4, ROWS_Y, wlsbar_w(), arch_rows(a) * ROW_H, a->nshown, arch_rows(a), a->top };
}

static struct lm_rect arch_button(const struct app *a, int i)
{
    static const int wd[AB_N] = { 76, 76, 140, 76, 76 };
    int x = 8;
    for (int k = 0; k < i; k++)
        x += wd[k] + 6;
    return (struct lm_rect){ x, a->win.bh - STAT_H - BAR_H + 6, wd[i], 28 };
}

static void arch_scroll_to(struct app *a)
{
    int rows = arch_rows(a);
    if (a->sel < a->top)
        a->top = a->sel;
    if (a->sel >= a->top + rows)
        a->top = a->sel - rows + 1;
    a->top = LM_CLAMP(a->top, 0, LM_MAX(a->nshown - rows, 0));
}

static void arch_select(struct app *a, int to)
{
    a->sel = LM_CLAMP(to, 0, LM_MAX(a->nshown - 1, 0));
    arch_scroll_to(a);
    a->win.redraw = true;
}

static void arch_press_button(struct app *a, int b)
{
    if (b == AB_PREV)
        other_day(a, -1);
    else if (b == AB_NEXT)
        other_day(a, 1);
    else if (b == AB_MOTION) {
        a->motion_only = !a->motion_only;
        a->sel = 0;
        filter_files(a);
    } else if (b == AB_OPEN) {
        play_open(a);
    } else {
        save_file(a);
    }
}

static void paint_arch(struct app *a)
{
    struct wlwin *w = &a->win;
    int W = arch_w(a), rows = arch_rows(a);
    char text[300], day[40];
    strftime(day, sizeof(day), "%a %d.%m.%Y", &a->day);
    int moves = 0;
    for (int i = 0; i < a->nfiles; i++)
        moves += file_kind(&a->files[i]) == 'M' || file_kind(&a->files[i]) == 'A';
    if (a->loading)
        snprintf(text, sizeof(text), "%s  -  %s", a->cam[a->arch].name, day);
    else
        snprintf(text, sizeof(text), "%s  -  %s  -  %d recordings, %d with movement", a->cam[a->arch].name, day, a->nfiles, moves);
    wlwin_fill(w, 0, 0, W, HEAD_H, lm_theme.bar);
    wlwin_text_bold(w, 10, (HEAD_H - LM_FONT_H) / 2, text, 1, lm_theme.text, W - 20);
    for (size_t i = 0; i < LM_ARRAY_LEN(COLUMN); i++)
        wlwin_text_bold(w, COLUMN[i].x, HEAD_H + (COL_H - LM_FONT_H) / 2, COLUMN[i].name, 1, lm_theme.accent, W - COLUMN[i].x - 4);
    wlwin_fill(w, 8, ROWS_Y - 1, W - 16, 1, lm_theme.control);
    if (!a->nshown) {
        const char *why = a->loading || !a->nfiles ? a->arch_msg : "No movement was recorded on this day";
        centred(w, (struct lm_rect){ 0, ROWS_Y, W, rows * ROW_H }, why, lm_theme.text);
    }
    struct wlsbar bar = arch_bar(a);
    int rw = wlsbar_on(&bar) ? W - wlsbar_w() - 8 : W - 4;
    for (int r = 0; r < rows && a->top + r < a->nshown; r++) {
        int k = a->top + r, y = ROWS_Y + r * ROW_H;
        const struct dv_file *f = &a->files[a->shown[k]];
        char kind = file_kind(f);
        if (k == a->sel)
            wlwin_round_fill(w, 4, y + 1, rw - 4, ROW_H - 2, 6, lm_theme.accent);
        uint32_t ink = k == a->sel ? 0xffffff : lm_theme.text;
        snprintf(text, sizeof(text), "%.8s", strlen(f->begin) > 11 ? f->begin + 11 : f->begin);
        wlwin_text(w, COLUMN[0].x, y + 3, text, 1, ink, 0);
        struct tm b0 = { 0 }, e0 = { 0 };
        long secs = 0;
        if (sscanf(f->begin, "%*d-%*d-%d %d:%d:%d", &b0.tm_mday, &b0.tm_hour, &b0.tm_min, &b0.tm_sec) == 4 &&
            sscanf(f->end, "%*d-%*d-%d %d:%d:%d", &e0.tm_mday, &e0.tm_hour, &e0.tm_min, &e0.tm_sec) == 4)
            secs = (e0.tm_hour - b0.tm_hour) * 3600L + (e0.tm_min - b0.tm_min) * 60L + e0.tm_sec - b0.tm_sec + (e0.tm_mday != b0.tm_mday ? 86400L : 0);
        snprintf(text, sizeof(text), "%ld:%02ld", secs / 60, secs % 60);
        wlwin_text(w, COLUMN[1].x, y + 3, text, 1, ink, 0);
        wlwin_text(w, COLUMN[2].x, y + 3, kind_text(kind), 1, k == a->sel ? ink : kind == 'M' || kind == 'A' ? lm_theme.warn : lm_theme.text, 0);
        snprintf(text, sizeof(text), "%u.%u MB", f->kb / 1024, f->kb % 1024 * 10 / 1024);
        wlwin_text(w, COLUMN[3].x, y + 3, text, 1, ink, rw - COLUMN[3].x - 4);
    }
    wlsbar_paint(w, &bar, a->bar_held);
    static const char *const label[AB_N] = { "< Day", "Day >", "Movement only", "Play", "Save" };
    for (int i = 0; i < AB_N; i++) {
        struct lm_rect b = arch_button(a, i);
        uint32_t c = i == AB_MOTION ? (a->motion_only ? lm_theme.btn_amber_lit : lm_theme.btn_slate) : i >= AB_OPEN ? lm_theme.btn_green : lm_theme.btn_blue;
        wlwin_drop(w, b.x, b.y, b.w, b.h, lm_button_radius(b.h / 2), c);
        wlwin_text(w, b.x + (b.w - wlcard_text_w(label[i])) / 2, b.y + (b.h - LM_FONT_H) / 2 - 1, label[i], 1, lm_theme.on_colour, b.w - 4);
    }
}

static void arch_pointer(struct app *a, uint32_t op, const union lm_arg *args)
{
    struct wlsbar bar = arch_bar(a);
    if (op == LM_WL_POINTER_EVT_AXIS) {
        if (args[1].u == 0) {
            a->top = LM_CLAMP(a->top + (lm_fixed_to_double(args[2].f) > 0 ? 3 : -3), 0, LM_MAX(a->nshown - arch_rows(a), 0));
            a->win.redraw = true;
        }
        return;
    }
    if (op == LM_WL_POINTER_EVT_MOTION && a->bar_held) {
        a->top = wlsbar_drag(&bar, a->py, a->bar_grab);
        a->win.redraw = true;
        return;
    }
    if (op != LM_WL_POINTER_EVT_BUTTON || args[2].u != 272)
        return;
    if (args[3].u != 1) {
        a->bar_held = false;
        a->win.redraw = true;
        return;
    }
    int top = a->top, grab = 0, r = wlsbar_press(&bar, a->px, a->py, &top, &grab);
    if (r) {
        a->top = top;
        a->bar_held = r == 1;
        a->bar_grab = grab;
        a->win.redraw = true;
        return;
    }
    for (int i = 0; i < AB_N; i++) {
        struct lm_rect b = arch_button(a, i);
        if (a->px >= b.x && a->px < b.x + b.w && a->py >= b.y && a->py < b.y + b.h) {
            arch_press_button(a, i);
            return;
        }
    }
    int row = a->py >= ROWS_Y && a->px < arch_w(a) ? (int)((a->py - ROWS_Y) / ROW_H) : -1;
    if (row < 0 || row >= arch_rows(a) || a->top + row >= a->nshown)
        return;
    int k = a->top + row;
    int64_t now = now_ms();
    bool again = k == a->click_row && now - a->click_ms <= lm_metrics.double_click_ms;
    a->click_row = k;
    a->click_ms = again ? 0 : now;
    arch_select(a, k);
    if (again)
        play_open(a);
}

static void arch_key(struct app *a, uint32_t ks)
{
    if (ks == LM_KS_Up || ks == LM_KS_Down)
        arch_select(a, a->sel + (ks == LM_KS_Up ? -1 : 1));
    else if (ks == LM_KS_Page_Up || ks == LM_KS_Page_Down)
        arch_select(a, a->sel + (ks == LM_KS_Page_Up ? -1 : 1) * arch_rows(a));
    else if (ks == LM_KS_Home || ks == LM_KS_End)
        arch_select(a, ks == LM_KS_Home ? 0 : a->nshown - 1);
    else if (ks == LM_KS_Left || ks == LM_KS_Right)
        other_day(a, ks == LM_KS_Left ? -1 : 1);
    else if (ks == LM_KS_Return || ks == LM_KS_KP_Enter)
        play_open(a);
    else if (ks == 's' || ks == 'S')
        save_file(a);
    else if (ks == 'f' || ks == 'F')
        arch_press_button(a, AB_MOTION);
    else if (ks == 'r' || ks == 'R')
        load_day(a);
    else if (ks == 'o' || ks == 'O')
        open_folder(a);
    else if (ks == 'm' || ks == 'M')
        toggle_sound(a);
    else if (ks == 'a' || ks == 'A')
        arch_close(a);
    else if (ks == LM_KS_Escape) {
        if (a->fetch) {
            cctv_arch_free(a->fetch);
            a->fetch = NULL;
            note(a, "Stopped");
        } else {
            arch_close(a);
        }
    }
}

/* ---- the recording that is watched ---- */

enum { PB_LIST, PB_PAUSE, PB_SAVE, PB_N };

static struct lm_rect play_button(const struct app *a, int i)
{
    return (struct lm_rect){ 8 + i * 82, a->win.bh - STAT_H - BAR_H + 6, 76, 28 };
}

static struct lm_rect play_area(const struct app *a)
{
    return (struct lm_rect){ 0, HEAD_H, arch_w(a), LM_MAX(a->win.bh - STAT_H - HEAD_H - BAR_H, 1) };
}

/* The strip that shows the place (and takes a click). */
static struct lm_rect play_bar(const struct app *a)
{
    int x = 8 + PB_N * 82 + 8;
    return (struct lm_rect){ x, a->win.bh - STAT_H - BAR_H + 12, LM_MAX(arch_w(a) - x - 130, 20), 16 };
}

static void paint_play(struct app *a)
{
    struct wlwin *w = &a->win;
    struct playback *p = a->play;
    struct cam *c = &p->cam;
    int W = arch_w(a);
    struct lm_rect area = play_area(a), bar = play_bar(a);
    wlwin_fill(w, area.x, area.y, area.w, area.h, 0x0a0c0f);
    pthread_mutex_lock(&c->lock);
    c->box_w = area.w * wlwin_scale(w);
    c->box_h = area.h * wlwin_scale(w);
    bool have = c->px && c->w > 0, paused = p->paused, ended = p->ended, failed = p->failed;
    long long pos = p->pos_ms, len = p->length_ms;
    char msg[240];
    snprintf(msg, sizeof(msg), "%s", c->msg);
    if (have) {
        double asp = (double)c->w / c->h;
        int pw = LM_MAX(1, (int)(LM_MIN((double)area.w, area.h * asp) + 0.5)), ph = LM_CLAMP((int)(pw / asp + 0.5), 1, area.h);
        wlwin_blit(w, area.x + (area.w - pw) / 2, area.y + (area.h - ph) / 2, pw, ph, c->px, c->w, c->h, c->w);
    }
    pthread_mutex_unlock(&c->lock);
    if (failed) {
        char text[300];
        snprintf(text, sizeof(text), "%s  -  Space: try again", msg);
        if (have)
            chip(w, 8, area.y + area.h - 32, text, area.w - 16, lm_theme.error);
        else
            centred(w, area, text, 0xff9a8f);
    } else if (!have) {
        centred(w, area, "Asking the camera...", 0xc8ccd2);
    }
    char text[300], clock[16];
    time_t when = (time_t)(p->begin + pos / 1000);
    struct tm t;
    gmtime_r(&when, &t);
    strftime(clock, sizeof(clock), "%H:%M:%S", &t);
    snprintf(text, sizeof(text), "%s  -  %.10s %s  -  %s", c->name, p->file.begin, clock, kind_text(file_kind(&p->file)));
    wlwin_fill(w, 0, 0, W, HEAD_H, lm_theme.bar);
    wlwin_text_bold(w, 10, (HEAD_H - LM_FONT_H) / 2, text, 1, lm_theme.text, W - 20);
    const char *label[PB_N] = { "< List", paused || ended || failed ? "Play" : "Pause", "Save" };
    for (int i = 0; i < PB_N; i++) {
        struct lm_rect b = play_button(a, i);
        uint32_t col = i == PB_LIST ? lm_theme.btn_blue : i == PB_PAUSE ? lm_theme.btn_green : lm_theme.btn_slate;
        wlwin_drop(w, b.x, b.y, b.w, b.h, lm_button_radius(b.h / 2), col);
        wlwin_text(w, b.x + (b.w - wlcard_text_w(label[i])) / 2, b.y + (b.h - LM_FONT_H) / 2 - 1, label[i], 1, lm_theme.on_colour, b.w - 4);
    }
    wlwin_round_fill(w, bar.x, bar.y + 5, bar.w, 6, 3, lm_theme.control);
    int done = len > 0 ? (int)((long long)bar.w * pos / len) : 0;
    if (done > 0)
        wlwin_round_fill(w, bar.x, bar.y + 5, LM_CLAMP(done, 6, bar.w), 6, 3, lm_theme.accent);
    wlwin_round_fill(w, bar.x + LM_CLAMP(done - 7, 0, bar.w - 14), bar.y + 1, 14, 14, 7, lm_theme.accent);
    snprintf(text, sizeof(text), "%lld:%02lld / %lld:%02lld", pos / 60000, pos / 1000 % 60, len / 60000, len / 1000 % 60);
    wlwin_text(w, bar.x + bar.w + 12, bar.y, text, 1, lm_theme.text, W - bar.x - bar.w - 16);
}

static void play_pointer(struct app *a, uint32_t op, const union lm_arg *args)
{
    if (op != LM_WL_POINTER_EVT_BUTTON || args[2].u != 272 || args[3].u != 1)
        return;
    for (int i = 0; i < PB_N; i++) {
        struct lm_rect b = play_button(a, i);
        if (a->px >= b.x && a->px < b.x + b.w && a->py >= b.y && a->py < b.y + b.h) {
            if (i == PB_LIST)
                play_close(a);
            else if (i == PB_PAUSE)
                play_pause(a);
            else
                save_file(a);
            return;
        }
    }
    struct lm_rect bar = play_bar(a), area = play_area(a);
    if (a->px >= bar.x - 4 && a->px < bar.x + bar.w + 4 && a->py >= bar.y - 6 && a->py < bar.y + bar.h + 6)
        play_seek(a, (long long)(LM_CLAMP((a->px - bar.x) / bar.w, 0.0, 1.0) * (double)a->play->length_ms));
    else if (a->px >= area.x && a->px < area.x + area.w && a->py >= area.y && a->py < area.y + area.h)
        play_pause(a);
}

static void play_key(struct app *a, uint32_t ks)
{
    if (ks == ' ' || ks == 'k' || ks == 'K')
        play_pause(a);
    else if (ks == LM_KS_Left || ks == LM_KS_Right)
        play_jump(a, ks == LM_KS_Left ? -PLAY_STEP_MS : PLAY_STEP_MS);
    else if (ks == LM_KS_Page_Up || ks == LM_KS_Page_Down)
        play_jump(a, ks == LM_KS_Page_Up ? -60000 : 60000);
    else if (ks == LM_KS_Home)
        play_seek(a, 0);
    else if (ks == 's' || ks == 'S')
        save_file(a);
    else if (ks == 'm' || ks == 'M')
        toggle_sound(a);
    else if (ks == 'o' || ks == 'O')
        open_folder(a);
    else if (ks == 'a' || ks == 'A')
        arch_close(a);
    else if (ks == LM_KS_Escape || ks == LM_KS_BackSpace) {
        if (a->fetch) {
            cctv_arch_free(a->fetch);
            a->fetch = NULL;
            note(a, "Stopped");
        } else {
            play_close(a);
        }
    }
}

static void menu_event(struct wlwin *w, int index)
{
    struct app *a = w->data;
    int menu = a->menu;
    a->menu = MENU_NONE;
    w->redraw = true;
    if (menu == MENU_NONE || index < 0 || index >= (int)LM_ARRAY_LEN(a->menu_do) || a->menu_cam != a->single)
        return;
    int d = a->menu_do[index];
    if (menu == MENU_ALARM) {
        if (d >= DO_NOTIFY)
            change_want(a, a->menu_cam, d);
        return;
    }
    struct cctv_ctl *ctl = a->cam[a->menu_cam].ctl;
    if (!ctl)
        return;
    if (d >= DO_LAMP)
        cctv_ctl_night(ctl, -1, LAMP[d - DO_LAMP].mode);
    else if (d >= 0)
        cctv_ctl_night(ctl, d, NULL);
}

/* The camera shown alone is asked for its settings once it shows a picture; what the steering has to say goes to the status line. */
static void watch_steering(struct app *a)
{
    if (a->single >= 0 && !a->cam[a->single].asked && steer(a, a->single)) {
        cctv_ctl_query(a->cam[a->single].ctl);
        a->cam[a->single].asked = true;
    }
    for (int i = 0; i < a->n; i++) {
        struct cam *c = &a->cam[i];
        if (!c->ctl)
            continue;
        struct cctv_view v;
        cctv_ctl_view(c->ctl, &v);
        if (v.serial == c->view.serial)
            continue;
        for (int n = 1; n <= CCTV_POSITIONS; n++)
            if (v.stored[n] != c->view.stored[n])
                position_taken(a, i, n);        /* the picture that was taken with the order becomes the position's */
        bool new_word = v.said != c->view.said && v.msg[0];
        c->view = v;
        a->win.redraw = true;
        if (new_word) {
            char text[600];
            snprintf(text, sizeof(text), "%s: %s", c->name, v.msg);
            note(a, text);
        }
    }
}

/* The pad in the corner of the picture of a camera shown alone: its middle, false when there is none. */
#define PAD_R 54

static bool pad_centre(struct app *a, int *cx, int *cy)
{
    if (a->single < 0 || a->arch >= 0)
        return false;
    struct cam *c = &a->cam[a->single];
    if (c->pic.w < 200 || c->pic.h < 150 || !is_live(c))
        return false;
    *cx = c->pic.x + c->pic.w - PAD_R - 10;
    *cy = c->pic.y + c->pic.h - PAD_R - 10;
    struct pos_layout L;
    if (pos_layout(a, &L))                  /* (the pad makes room for the positions) */
        *cy = LM_MAX(LM_MIN(*cy, L.all.y - PAD_R - 8), c->tile.y + PAD_R + 40);
    return true;
}

/* 0..3 = an arrow of the pad, -2 = elsewhere on the pad, -1 = not on it. */
static int pad_at(struct app *a, double x, double y)
{
    int cx, cy;
    if (!pad_centre(a, &cx, &cy) || (x - cx) * (x - cx) + (y - cy) * (y - cy) > PAD_R * PAD_R)
        return -1;
    double dx = x - cx, dy = y - cy;
    if (dx * dx + dy * dy < 12 * 12)
        return -2;
    return fabs(dx) > fabs(dy) ? (dx < 0 ? 2 : 3) : (dy < 0 ? 0 : 1);
}

static void paint_pad(struct app *a)
{
    struct wlwin *w = &a->win;
    int cx, cy;
    if (!pad_centre(a, &cx, &cy))
        return;
    wlwin_round_fill(w, cx - PAD_R, cy - PAD_R, 2 * PAD_R, 2 * PAD_R, PAD_R, 0x101418);
    static const int at[4][2] = { { 0, -33 }, { 0, 33 }, { -33, 0 }, { 33, 0 } };
    for (int d = 0; d < 4; d++) {
        int x = cx + at[d][0], y = cy + at[d][1];
        bool on = a->moving == a->single && a->move_dir == d;
        wlwin_drop(w, x - 17, y - 17, 34, 34, lm_button_radius(17), on ? lm_theme.btn_blue_lit : lm_theme.btn_slate);
        for (int i = 0; i < 7; i++) {       /* an arrow head, a line at a time */
            if (d < 2)
                wlwin_fill(w, x - i - 1, y + (d == 0 ? i - 4 : 3 - i), 2 * i + 2, 1, 0xffffff);
            else
                wlwin_fill(w, x + (d == 2 ? i - 4 : 3 - i), y - i - 1, 1, 2 * i + 2, 0xffffff);
        }
    }
}

static void night_icon(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    wlwin_round_fill(w, cx - 8, cy - 8, 16, 16, 8, c);
    wlwin_round_fill(w, cx - 3, cy - 10, 14, 14, 7, bg);
}

static void speaker_icon(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    (void)bg;
    wlwin_fill(w, cx - 7, cy - 3, 4, 7, c);
    for (int i = 0; i < 5; i++)
        wlwin_fill(w, cx - 3 + i, cy - 3 - i, 1, 7 + 2 * i, c);
    wlwin_fill(w, cx + 4, cy - 3, 2, 7, c);
    wlwin_fill(w, cx + 7, cy - 6, 1, 13, c);
}

/* ---- the window ---- */

/* the left rail chooses what is shown, the right one does things with it */
enum { B_ALL, B_CAM0, B_AGAIN = B_CAM0 + MAX_CAMS, B_COUNT };
enum { S_SHOT, S_REC, S_SOUND, S_FOLDER, S_NIGHT, S_POS, S_ALARM, S_ARCH, S_COUNT };
_Static_assert(B_COUNT <= WLRAIL_MAX && S_COUNT <= WLRAIL_MAX, "the rails hold the buttons");

static void bell_icon(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    (void)bg;
    wlwin_round_fill(w, cx - 5, cy - 7, 10, 12, 5, c);
    wlwin_fill(w, cx - 5, cy - 1, 10, 5, c);
    wlwin_fill(w, cx - 8, cy + 3, 16, 2, c);
    wlwin_round_fill(w, cx - 2, cy + 6, 4, 3, 1, c);
}

static void card_icon(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    wlwin_round_fill(w, cx - 6, cy - 8, 12, 16, 3, c);
    for (int i = 0; i < 3; i++)
        wlwin_fill(w, cx - 3 + 3 * i, cy - 6, 2, 4, bg);
}

static void rail(struct app *a, struct wlrail *L)
{
    static char num[MAX_CAMS][4];
    memset(L, 0, sizeof(*L));
    L->y1 = a->win.bh > 0 ? a->win.bh - STAT_H : 400;
    L->n = B_COUNT;
    L->it[B_ALL] = (struct wlrail_item){ .tip = "All cameras (Esc)", .icon = wlrail_ic_panes, .base = lm_theme.btn_blue, .lite = lm_theme.btn_blue_lit,
                                         .show = true, .lit = a->single < 0, .idle = !a->n };
    for (int i = 0; i < MAX_CAMS; i++) {
        snprintf(num[i], sizeof(num[i]), "%d", i + 1);
        L->it[B_CAM0 + i] = (struct wlrail_item){ .tip = i < a->n ? a->cam[i].name : NULL, .text = num[i], .base = lm_theme.btn_teal, .lite = lm_theme.btn_teal_lit,
                                                  .show = i < a->n, .lit = a->single == i, .gap = i == 0 };
    }
    L->it[B_AGAIN] = (struct wlrail_item){ .tip = "Connect again (R)", .icon = wlrail_ic_swap, .base = lm_theme.btn_amber, .lite = lm_theme.btn_amber_lit,
                                           .show = true, .bottom = true, .idle = !a->n };
}

static void side_rail(struct app *a, struct wlrail *S)
{
    memset(S, 0, sizeof(*S));
    S->x = a->win.inx + a->win.bw - WLRAIL_W;           /* (in window pixels) */
    S->y1 = a->win.bh > 0 ? a->win.bh - STAT_H : 400;
    S->n = S_COUNT;
    bool recording = false;
    for (int i = 0; i < a->n; i++)
        recording |= a->cam[i].rec && (a->single < 0 || a->single == i);
    bool one = a->single >= 0, arch = a->arch >= 0, live_one = one && !arch, hear = one && (!arch || a->play);
    bool watched = one && (a->want[a->single].notify || a->want[a->single].record);
    S->it[S_SHOT] = (struct wlrail_item){ .tip = "Save a picture (S)", .icon = wlrail_ic_camera, .base = lm_theme.btn_green, .lite = lm_theme.btn_green_lit,
                                          .show = true, .idle = !a->n || arch };
    S->it[S_REC] = (struct wlrail_item){ .tip = recording ? "Stop recording (V)" : "Record (V)", .icon = wlrail_ic_rec, .base = lm_theme.btn_red,
                                         .lite = lm_theme.btn_red_lit, .show = true, .lit = recording, .idle = !a->n || arch };
    S->it[S_SOUND] = (struct wlrail_item){ .tip = !one ? "Sound (M): one camera alone" : a->sound ? "Sound off (M)" : "Sound on (M)", .icon = speaker_icon,
                                           .base = lm_theme.btn_purple, .lite = lm_theme.btn_purple_lit, .show = true, .lit = a->sound && hear, .idle = !hear };
    S->it[S_FOLDER] = (struct wlrail_item){ .tip = "Open the folder (O)", .icon = wlrail_ic_open, .base = lm_theme.btn_slate, .lite = lm_theme.btn_slate_lit, .show = true };
    S->it[S_NIGHT] = (struct wlrail_item){ .tip = one ? "Night picture and lamp (N)" : "Night (N): one camera alone", .icon = night_icon, .base = lm_theme.btn_blue,
                                           .lite = lm_theme.btn_blue_lit, .show = true, .gap = true, .idle = !live_one };
    S->it[S_POS] = (struct wlrail_item){ .tip = one ? "Positions (P)" : "Positions (P): one camera alone", .icon = wlrail_ic_home, .base = lm_theme.btn_teal,
                                         .lite = lm_theme.btn_teal_lit, .show = true, .lit = a->pos_open && live_one, .idle = !live_one };
    S->it[S_ALARM] = (struct wlrail_item){ .tip = one ? "Alarms (L)" : "Alarms (L): one camera alone", .icon = bell_icon, .base = lm_theme.btn_amber,
                                           .lite = lm_theme.btn_amber_lit, .show = true, .lit = watched && live_one, .idle = !live_one };
    S->it[S_ARCH] = (struct wlrail_item){ .tip = !one ? "Recordings (A): one camera alone" : arch ? "Back to the live picture (A)" : "Recordings on the card (A)",
                                          .icon = card_icon, .base = lm_theme.btn_rose, .lite = lm_theme.btn_rose_lit, .show = true, .lit = arch, .idle = !one };
}

/* Where the tiles go: all cameras in the arrangement that makes the pictures largest, or one alone. */
static void layout(struct app *a)
{
    int W = a->win.bw - WLRAIL_W, H = a->win.bh - STAT_H;
    for (int i = 0; i < a->n; i++)
        a->cam[i].tile = a->cam[i].pic = (struct lm_rect){ 0, 0, 0, 0 };
    if (!a->n || a->arch >= 0 || W < 2 * GAP + 16 || H < 2 * GAP + 16)
        return;
    int shown = a->single >= 0 ? 1 : a->n, cols = 1;
    double best = 0;
    for (int k = 1; k <= shown; k++) {
        int rows = (shown + k - 1) / k;
        double tw = (W - GAP * (k + 1.0)) / k, th = (H - GAP * (rows + 1.0)) / rows, pw = LM_MIN(tw, th * 4 / 3);
        if (pw > best) {
            best = pw;
            cols = k;
        }
    }
    int rows = (shown + cols - 1) / cols, tw = (W - GAP * (cols + 1)) / cols, th = (H - GAP * (rows + 1)) / rows;
    for (int k = 0; k < shown; k++) {
        struct cam *c = &a->cam[a->single >= 0 ? a->single : k];
        c->tile = (struct lm_rect){ GAP + (k % cols) * (tw + GAP), GAP + (k / cols) * (th + GAP), tw, th };
        pthread_mutex_lock(&c->lock);
        double asp = c->w > 0 && c->h > 0 ? (double)c->w / c->h : 4.0 / 3.0;
        c->box_w = tw * wlwin_scale(&a->win);
        c->box_h = th * wlwin_scale(&a->win);
        pthread_mutex_unlock(&c->lock);
        int pw = LM_MAX(1, (int)(LM_MIN((double)tw, th * asp) + 0.5)), ph = LM_MAX(1, (int)(pw / asp + 0.5));
        ph = LM_MIN(ph, th);
        c->pic = (struct lm_rect){ c->tile.x + (tw - pw) / 2, c->tile.y + (th - ph) / 2, pw, ph };
    }
}

static void chip(struct wlwin *w, int x, int y, const char *text, int max_w, uint32_t dot)
{
    int pad = dot ? 22 : 10, tw = LM_MIN(wlcard_text_w(text) + pad + 10, max_w);
    if (tw < 40)
        return;
    wlwin_round_fill(w, x, y, tw, 24, 12, 0x101418);
    if (dot)
        wlwin_round_fill(w, x + 8, y + 8, 8, 8, 4, dot);
    wlwin_text(w, x + pad, y + 4, text, 1, 0xf0f2f5, tw - pad - 6);
}

/* Words in lines, in the middle of a rectangle. */
static void centred(struct wlwin *w, struct lm_rect r, const char *text, uint32_t color)
{
    char line[240];
    int per = LM_CLAMP((r.w - 24) / LM_FONT_W, 8, (int)sizeof(line) - 1);
    const char *lines[8];
    int len[8], n = 0;
    for (const char *p = text; *p && n < 8;) {
        int l = (int)strlen(p);
        if (l > per) {
            l = per;
            while (l > per / 2 && p[l] != ' ')
                l--;
        }
        lines[n] = p;
        len[n++] = l;
        p += l;
        while (*p == ' ')
            p++;
    }
    int y = r.y + (r.h - n * (LM_FONT_H + 2)) / 2;
    for (int i = 0; i < n; i++) {
        snprintf(line, sizeof(line), "%.*s", len[i], lines[i]);
        wlwin_text(w, r.x + (r.w - wlcard_text_w(line)) / 2, y + i * (LM_FONT_H + 2), line, 1, color, r.w - 8);
    }
}

static void write_dump(struct app *a)
{
    if (!g_dump)
        return;
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.new", g_dump);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return;
    fprintf(f, "state view=%d n=%d\n", a->single, a->n);
    for (int i = 0; i < a->n; i++) {
        struct cam *c = &a->cam[i];
        pthread_mutex_lock(&c->lock);
        fprintf(f, "cam %d name=%s state=%s stream=%d w=%d h=%d src=%dx%d frames=%u fps10=%d kbps=%d\n", i, c->name, STATE_NAME[c->state], c->stream, c->w, c->h,
                c->src_w, c->src_h, c->frames, c->fps10, c->kbps);
        fprintf(f, "msg %d %s\n", i, c->msg);
        pthread_mutex_unlock(&c->lock);
        fprintf(f, "tile %d %d %d %d %d\n", i, c->tile.x, c->tile.y, c->tile.w, c->tile.h);
        fprintf(f, "pic %d %d %d %d %d\n", i, c->pic.x, c->pic.y, c->pic.w, c->pic.h);
        struct cctv_status st = { 0 };
        if (c->rec)
            cctv_job_status(c->rec, &st);
        fprintf(f, "rec %d on=%d state=%d snap=%d\n", i, c->rec != NULL, st.state, c->snap != NULL);
    }
    fprintf(f, "sound on=%d\n", a->sound);
    fprintf(f, "ptz cam=%d dir=%s\n", a->moving, a->moving >= 0 ? DIRECTION[a->move_dir] : "-");
    for (int i = 0; i < a->n; i++)
        if (a->cam[i].view.known)
            fprintf(f, "ctl %d night=%d lamp=%s\n", i, a->cam[i].view.night, a->cam[i].view.lamp);
    int px, py;
    if (pad_centre(a, &px, &py))
        fprintf(f, "pad %d %d %d\n", a->single, px, py);
    fprintf(f, "menu %d\n", a->menu);
    struct pos_layout L;
    if (pos_layout(a, &L)) {
        fprintf(f, "pos open=1 ask=%d\n", a->pos_ask);
        for (int i = 0; i < CCTV_POSITIONS; i++) {
            fprintf(f, "ptile %d %d %d %d %d %d\n", i + 1, L.tile[i].x, L.tile[i].y, L.tile[i].w, L.tile[i].h, a->cam[a->single].pos[i].px != NULL);
            fprintf(f, "pset %d %d %d %d %d\n", i + 1, L.set[i].x, L.set[i].y, L.set[i].w, L.set[i].h);
        }
    }
    for (int i = 0; i < a->n; i++)
        fprintf(f, "want %d notify=%d record=%d\n", i, a->want[i].notify, a->want[i].record);
    if (a->arch >= 0) {
        char day[16];
        day_string(&a->day, day, sizeof(day));
        fprintf(f, "arch cam=%d day=%s loading=%d n=%d shown=%d sel=%d top=%d motion=%d fetch=%d\n", a->arch, day, a->loading, a->nfiles, a->nshown, a->sel, a->top,
                a->motion_only, a->fetch != NULL);
        for (int r = 0; r < arch_rows(a) && a->top + r < a->nshown; r++) {
            const struct dv_file *fl = &a->files[a->shown[a->top + r]];
            fprintf(f, "row %d %.8s %c %u %d\n", a->top + r, fl->begin + 11, file_kind(fl), fl->kb, ROWS_Y + r * ROW_H + ROW_H / 2);
        }
        for (int i = 0; i < AB_N; i++) {
            struct lm_rect b = arch_button(a, i);
            fprintf(f, "abtn %d %d %d %d %d\n", i, b.x, b.y, b.w, b.h);
        }
    }
    if (a->play) {
        struct playback *p = a->play;
        pthread_mutex_lock(&p->cam.lock);
        fprintf(f, "play paused=%d pos=%lld len=%lld ended=%d failed=%d frames=%u w=%d h=%d listen=%d passed=%u\n", p->paused, p->pos_ms, p->length_ms, p->ended,
                p->failed, p->cam.frames, p->cam.w, p->cam.h, p->cam.listen, p->passed);
        fprintf(f, "playmsg %s\n", p->cam.msg);
        pthread_mutex_unlock(&p->cam.lock);
        for (int i = 0; i < PB_N; i++) {
            struct lm_rect b = play_button(a, i);
            fprintf(f, "pbtn %d %d %d %d %d\n", i, b.x, b.y, b.w, b.h);
        }
        struct lm_rect b = play_bar(a);
        fprintf(f, "pbar %d %d %d %d\n", b.x, b.y, b.w, b.h);
    }
    fprintf(f, "note %s\n", a->note);
    fclose(f);
    if (rename(tmp, g_dump) < 0) {}
}

static void paint(struct wlwin *w)
{
    struct app *a = w->data;
    layout(a);
    wlwin_fill(w, 0, 0, w->bw, w->bh, lm_theme.background);
    if (!a->n) {
        char text[600];
        snprintf(text, sizeof(text), "%s Write one camera a line into %s : name address user password (a - for no password), then start the program again.",
                 a->conf_note, a->conf);
        centred(w, (struct lm_rect){ 0, 0, w->bw - WLRAIL_W, w->bh - STAT_H }, text, lm_theme.text);
    }
    for (int i = 0; i < a->n; i++) {
        struct cam *c = &a->cam[i];
        if (c->tile.w <= 0)
            continue;
        wlwin_round_fill(w, c->tile.x, c->tile.y, c->tile.w, c->tile.h, 6, 0x0a0c0f);
        pthread_mutex_lock(&c->lock);
        enum state st = c->state;
        char msg[240];
        snprintf(msg, sizeof(msg), "%s", c->msg);
        bool have = c->px && c->w > 0;
        if (have)
            wlwin_blit(w, c->pic.x, c->pic.y, c->pic.w, c->pic.h, c->px, c->w, c->h, c->w);
        int fps10 = c->fps10, sw = c->src_w, sh = c->src_h, kbps = c->kbps;
        pthread_mutex_unlock(&c->lock);
        uint32_t dot = st == ST_LIVE ? lm_theme.ok : st == ST_REFUSED || st == ST_RETRY ? lm_theme.error : lm_theme.warn;
        if (!have)
            centred(w, c->tile, msg[0] ? msg : "...", st == ST_REFUSED || st == ST_RETRY ? 0xff9a8f : 0xc8ccd2);
        else if (st != ST_LIVE)             /* the last picture stays, with a word about what is wrong */
            chip(w, c->tile.x + 8, c->tile.y + c->tile.h - 32, msg, c->tile.w - 16, 0);
        chip(w, c->tile.x + 8, c->tile.y + 8, c->name, c->tile.w - 16, dot);
        int cy = c->tile.y + 38;
        if (a->want[i].notify || a->want[i].record) {
            const char *what = !a->want[i].record ? "Alarms: notify" : a->want[i].notify ? "Alarms: notify, record" : "Alarms: record";
            chip(w, c->tile.x + 8, cy, what, c->tile.w - 16, lm_theme.warn);
            cy += 30;
        }
        if (c->rec) {
            char r[64] = "Starting to record";
            if (c->rec_secs >= 0)
                snprintf(r, sizeof(r), "REC %d:%02d", c->rec_secs / 60, c->rec_secs % 60);
            chip(w, c->tile.x + 8, cy, r, c->tile.w - 16, 0xff3b30);
        }
        if (st == ST_LIVE && (a->single == i || a->hover == i)) {
            char info[96];
            snprintf(info, sizeof(info), "%d x %d  %d.%d fps  %d kbit/s", sw, sh, fps10 / 10, fps10 % 10, kbps);
            int tw = LM_MIN(wlcard_text_w(info) + 20, c->tile.w - 16);
            chip(w, c->tile.x + c->tile.w - tw - 8, a->single == i ? c->tile.y + 8 : c->tile.y + c->tile.h - 32, info, c->tile.w - 16, 0);
        }
    }
    if (a->play)
        paint_play(a);
    else if (a->arch >= 0)
        paint_arch(a);
    else {
        paint_positions(a);
        paint_pad(a);
    }
    int sy = w->bh - STAT_H;
    wlwin_fill(w, 0, sy, w->bw, STAT_H, lm_theme.bar);
    char status[300];
    int live = 0;
    for (int i = 0; i < a->n; i++) {
        pthread_mutex_lock(&a->cam[i].lock);
        live += a->cam[i].state == ST_LIVE;
        pthread_mutex_unlock(&a->cam[i].lock);
    }
    int recording = 0;
    for (int i = 0; i < a->n; i++)
        recording += a->cam[i].rec != NULL;
    if (!g_ffmpeg)
        snprintf(status, sizeof(status), "ffmpeg is not installed: the pictures cannot be shown");
    else if (a->fetch)
        snprintf(status, sizeof(status), "Fetching %s... %d %%  -  Esc: stop", a->fetch_what, a->fetch_pct);
    else if (a->note[0])
        snprintf(status, sizeof(status), "%.299s", a->note);
    else if (a->play)
        snprintf(status, sizeof(status), "Space: pause  arrows: 20 s  PgUp/PgDn: 1 min  M: sound %s  S: save  Esc: the list", a->sound ? "off" : "on");
    else if (a->arch >= 0)
        snprintf(status, sizeof(status), "Enter: play  S: save  arrows: choose, other day  F: movement only  Esc: back");
    else if (recording && a->single >= 0 && !a->cam[a->single].rec)
        snprintf(status, sizeof(status), "Recording %d other camera%s  -  Esc: all cameras", recording, recording > 1 ? "s" : "");
    else if (a->pos_open)
        snprintf(status, sizeof(status), "1..4: go there  Shift+1..4: set this view  arrows: move  Esc: close");
    else if (a->single >= 0)
        snprintf(status, sizeof(status), "%s (%s)  -  arrows: move  -  Esc: all cameras", a->cam[a->single].name, a->cam[a->single].host);
    else
        snprintf(status, sizeof(status), "%d of %d cameras live%s", live, a->n, a->n ? "  -  click a picture to see it alone" : "");
    wlwin_text(w, 8, sy + (STAT_H - LM_FONT_H) / 2, status, 1, lm_theme.text, w->bw - 16);
    struct wlrail L;
    rail(a, &L);
    wlrail_foot(w, &L, lm_theme.bar);
    struct wlrail S;
    side_rail(a, &S);
    wlrail_paint(w, &S, &a->san, a->shover, now_ms(), lm_theme.bar, lm_theme.bar, lm_theme.accent, lm_theme.text);
    wlrail_paint(w, &L, &a->ran, a->rhover, now_ms(), lm_theme.bar, lm_theme.bar, lm_theme.accent, lm_theme.text);
    write_dump(a);
}

static void show(struct app *a, int i)
{
    move_stop(a);
    arch_close(a);
    positions_close(a);
    for (int k = 0; k < a->n; k++)
        a->cam[k].asked = false;
    a->single = i >= 0 && i < a->n ? i : -1;
    a->hover = -1;
    layout(a);                              /* (the new room is known before the connections are made) */
    set_wants(a);
    a->win.redraw = true;
}

static void press_button(struct app *a, int b)
{
    if (b == B_ALL)
        show(a, -1);
    else if (b >= B_CAM0 && b < B_CAM0 + a->n)
        show(a, b - B_CAM0);
    else if (b == B_AGAIN)
        for (int i = 0; i < a->n; i++)
            if (a->single < 0 || a->single == i)
                reconnect(a, i);
}

static void press_side(struct app *a, int b)
{
    bool arch = a->arch >= 0;
    if (b == S_SHOT && g_ffmpeg && !arch)
        snapshot(a);
    else if (b == S_REC && g_ffmpeg && !arch)
        toggle_record(a);
    else if (b == S_SOUND && (!arch || a->play))
        toggle_sound(a);
    else if (b == S_FOLDER)
        open_folder(a);
    else if (b == S_NIGHT && !arch)
        night_menu(a);
    else if (b == S_POS && !arch)
        positions_toggle(a);
    else if (b == S_ALARM && !arch)
        alarm_menu(a);
    else if (b == S_ARCH)
        arch_open(a);
}

static int tile_at(const struct app *a, double x, double y)
{
    for (int i = 0; i < a->n; i++) {
        struct lm_rect r = a->cam[i].tile;
        if (r.w > 0 && x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h)
            return i;
    }
    return -1;
}

static void pointer_event(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
    struct app *a = w->data;
    if (op == LM_WL_POINTER_EVT_ENTER || op == LM_WL_POINTER_EVT_MOTION) {
        int k = op == LM_WL_POINTER_EVT_ENTER ? 2 : 1;
        a->px = lm_fixed_to_double(args[k].f);
        a->py = lm_fixed_to_double(args[k + 1].f);
    } else if (op == LM_WL_POINTER_EVT_LEAVE) {
        a->hover = a->rhover = a->shover = -1;
        wlrail_release(&a->ran);
        wlrail_release(&a->san);
        if (!a->move_key)
            move_stop(a);
        w->redraw = true;
        return;
    } else if (op != LM_WL_POINTER_EVT_BUTTON && op != LM_WL_POINTER_EVT_AXIS) {
        return;
    }
    struct wlrail L, S;
    rail(a, &L);
    side_rail(a, &S);
    if (op != LM_WL_POINTER_EVT_BUTTON) {
        int h = tile_at(a, a->px, a->py), rh = wlrail_at(&L, w->rpx, w->rpy), sh = wlrail_at(&S, w->rpx, w->rpy);
        if (h != a->hover || rh != a->rhover || sh != a->shover)
            w->redraw = true;
        a->hover = h;
        a->rhover = rh;
        a->shover = sh;
        if (a->pos_open)
            w->redraw = true;               /* (what the pointer is on is lit) */
        if (a->arch >= 0 && !a->play)
            arch_pointer(a, op, args);
        return;
    }
    if (args[3].u != 1) {
        wlrail_release(&a->ran);
        wlrail_release(&a->san);
        if (args[2].u == 272 && !a->move_key)
            move_stop(a);
        if (a->arch >= 0 && !a->play)
            arch_pointer(a, op, args);
        w->redraw = true;
        return;
    }
    if (args[2].u != 272)
        return;
    int b = wlrail_at(&L, w->rpx, w->rpy);
    if (b >= 0) {
        wlrail_press(&a->ran, b, now_ms());
        if (!L.it[b].idle)
            press_button(a, b);
        w->redraw = true;
        return;
    }
    int sb = wlrail_at(&S, w->rpx, w->rpy);
    if (sb >= 0) {
        wlrail_press(&a->san, sb, now_ms());
        if (!S.it[sb].idle)
            press_side(a, sb);
        w->redraw = true;
        return;
    }
    if (a->play) {
        play_pointer(a, op, args);
        return;
    }
    if (a->arch >= 0) {
        arch_pointer(a, op, args);
        return;
    }
    int pd = pad_at(a, a->px, a->py);
    if (pd != -1) {
        if (pd >= 0)
            move_start(a, pd, 0);
        return;
    }
    if (a->pos_open) {                      /* a click beside the panel puts it away */
        int at = positions_at(a, a->px, a->py);
        if (at > 100)
            position_set(a, at - 101);
        else if (at > 0)
            position_go(a, at - 1);
        else if (at == 0 || at == -1)
            positions_close(a);
        w->redraw = true;
        return;
    }
    int t = tile_at(a, a->px, a->py);
    if (t >= 0)
        show(a, a->single == t ? -1 : t);
}

static void keyboard_event(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
    struct app *a = w->data;
    if (op == LM_WL_KEYBOARD_EVT_LEAVE) {       /* the release of the key may never come */
        move_stop(a);
        return;
    }
    if (op != LM_WL_KEYBOARD_EVT_KEY)
        return;
    if (args[3].u == 0) {
        if (a->move_key && args[2].u == a->move_key)
            move_stop(a);
        return;
    }
    uint32_t ks = lm_keymap_keysym(args[2].u, w->mods);
    if (w->mods & LM_MOD_CTRL) {
        if (ks == 'q' || ks == 'Q' || ks == 'w' || ks == 'W')
            w->running = false;
        return;
    }
    if (a->arch >= 0) {
        if (ks >= '1' && ks <= '0' + (uint32_t)a->n)
            show(a, (int)(ks - '1'));
        else if (ks == '0' || ks == 'g')
            show(a, -1);
        else if (a->play)
            play_key(a, ks);
        else
            arch_key(a, ks);
        return;
    }
    if (a->pos_open) {                      /* 1..4 go to a position, with Shift set it */
        if (args[2].u >= 2 && args[2].u < 2 + CCTV_POSITIONS) {
            if (w->mods & LM_MOD_SHIFT)
                position_set(a, (int)args[2].u - 2);
            else
                position_go(a, (int)args[2].u - 2);
            return;
        }
        if (ks == LM_KS_Escape) {
            positions_close(a);
            return;
        }
    }
    if (ks == LM_KS_Up || ks == LM_KS_Down || ks == LM_KS_Left || ks == LM_KS_Right) {
        if (a->single < 0)
            note(a, "Show one camera alone to move it");
        else
            move_start(a, ks == LM_KS_Up ? 0 : ks == LM_KS_Down ? 1 : ks == LM_KS_Left ? 2 : 3, args[2].u);
    } else if (ks == 'n' || ks == 'N') {
        press_side(a, S_NIGHT);
    } else if (ks == 'p' || ks == 'P') {
        press_side(a, S_POS);
    } else if (ks == 'l' || ks == 'L') {
        press_side(a, S_ALARM);
    } else if (ks == 'a' || ks == 'A') {
        press_side(a, S_ARCH);
    } else if (ks == LM_KS_Escape || ks == '0' || ks == 'g')
        show(a, -1);
    else if (ks >= '1' && ks <= '0' + (uint32_t)a->n)
        show(a, (int)(ks - '1'));
    else if (ks == 'r' || ks == 'R')
        press_button(a, B_AGAIN);
    else if (ks == 's' || ks == 'S' || ks == ' ')
        press_side(a, S_SHOT);
    else if (ks == 'v' || ks == 'V')
        press_side(a, S_REC);
    else if (ks == 'm' || ks == 'M')
        press_side(a, S_SOUND);
    else if (ks == 'o' || ks == 'O')
        press_side(a, S_FOLDER);
}

/* ---- self test ------------------------------------------------------------------------------------------------------------- */

static int g_fail;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); g_fail++; } } while (0)

struct seen { int n[4]; size_t bytes; int w, h, codec; };
static void count_frame(void *ud, const struct dv_frame *f)
{
    struct seen *s = ud;
    s->n[f->kind]++;
    s->bytes += f->len;
    if (f->kind == DV_VIDEO_KEY) {
        s->w = f->w;
        s->h = f->h;
        s->codec = f->codec;
    }
}

static int selftest(void)
{
    char h[9];
    dv_hash("", h);
    CHECK(!strcmp(h, "tlJwpbo6"));                           /* the well known value of an empty password */
    /* a stream of frames, with rubbish between, fed in pieces of every size */
    static const uint8_t stream[] = {
        0, 0, 1, 0xfc, 0x12, 6, 68, 243, 0, 0, 0, 0, 4, 0, 0, 0, 'K', 'E', 'Y', '!',          /* 2592 x 1944, H.264 */
        0, 0, 1, 0xfa, 14, 2, 3, 0, 'a', 'u', 'd',
        'j', 'u', 'n', 'k', 0, 0,
        0, 0, 1, 0xfd, 2, 0, 0, 0, 'p', 'p',
        0, 0, 1, 0xf9, 1, 0, 1, 0, 'i',
    };
    for (size_t step = 1; step <= sizeof(stream); step++) {
        struct dv_demux d = { 0 };
        struct seen s = { 0 };
        for (size_t at = 0; at < sizeof(stream); at += step)
            CHECK(dv_demux_feed(&d, stream + at, LM_MIN(step, sizeof(stream) - at), count_frame, &s));
        CHECK(s.n[DV_VIDEO_KEY] == 1 && s.n[DV_VIDEO] == 1 && s.n[DV_AUDIO] == 1 && s.n[DV_INFO] == 1 && s.bytes == 10 && d.skipped == 6);
        CHECK(s.w == 2592 && s.h == 1944 && s.codec == DV_CODEC_H264);
        dv_demux_free(&d);
    }
    char l1[] = "Yard 10.0.0.2 admin two words \n", l2[] = "  # nothing\n", l3[] = "Door 10.0.0.3:1234 u -\n", l4[] = "Bad 10.0.0.4\n", l5[] = "X h:99999 u p\n";
    struct cam c;
    memset(&c, 0, sizeof(c));
    CHECK(parse_line(l1, &c) == 1 && !strcmp(c.pass, "two words") && c.port == DV_PORT && !strcmp(c.host, "10.0.0.2"));
    CHECK(parse_line(l2, &c) == 0);
    CHECK(parse_line(l3, &c) == 1 && !c.pass[0] && c.port == 1234 && !strcmp(c.host, "10.0.0.3"));
    CHECK(parse_line(l4, &c) == -1);
    CHECK(parse_line(l5, &c) == -1);
    CHECK(picture_aspect(0, 704, 576) > 1.33 && picture_aspect(0, 704, 576) < 1.34);
    CHECK(picture_aspect(16.0 / 9, 704, 576) > 1.77);
    CHECK(picture_aspect(0, 1920, 1080) > 1.77);
    int ow, oh;
    output_size(4.0 / 3, 2592, 1944, 1920, 1000, &ow, &oh);
    CHECK(ow == 1334 && oh == 1000);
    output_size(4.0 / 3, 704, 576, 4000, 4000, &ow, &oh);
    CHECK(ow == 768 && oh == 576);                           /* never bigger than the source */
    const char *reply = "{ \"Name\" : \"Camera.Param\", \"Camera.Param\" : [ { \"X\" : { \"Mode\" : [\"}\"] }, \"DayNightColor\" : \"0x00000003\", \"Z\" : 5 } ], \"Ret\" : 100 }";
    char *v = dv_config_change(reply, "Camera.Param", "DayNightColor", "0x00000001");
    CHECK(v && !strcmp(v, "[ { \"X\" : { \"Mode\" : [\"}\"] }, \"DayNightColor\" : \"0x00000001\", \"Z\" : 5 } ]"));
    free(v);
    CHECK(!dv_config_change(reply, "Camera.Param", "NoSuch", "1") && !dv_config_change(reply, "Camera.Other", "DayNightColor", "1"));
    CHECK(!dv_config_change(reply, "Camera.Param", "Ret", "1"));                 /* outside the setting */
    CHECK(!dv_config_change("{\"A\":[{\"k\":\"v\"", "A", "k", "w"));             /* cut off */
    printf("%s\n", g_fail ? "selftest FAILED" : "selftest ok");
    return g_fail != 0;
}

/* ---- main ----------------------------------------------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    wlwin_hidpi();
    lm_theme_load_env();
    signal(SIGPIPE, SIG_IGN);
    const char *cli_conf = NULL;
    int win_w = 1000, win_h = 500;
    bool list = false, watch = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--selftest"))
            return selftest();
        if (!strcmp(argv[i], "--list")) {
            list = true;
        } else if (!strcmp(argv[i], "--watch")) {
            watch = true;
        } else if (!strcmp(argv[i], "--config") && i + 1 < argc) {
            cli_conf = argv[++i];
        } else if (!strcmp(argv[i], "--size") && i + 1 < argc) {
            char junk;
            if (sscanf(argv[++i], "%dx%d%c", &win_w, &win_h, &junk) != 2 || win_w < 520 || win_h < 440) {
                fprintf(stderr, "mellagui-cctv: --size wants a size like 960x440\n");
                return 2;
            }
        } else {
            fprintf(stderr, "usage: mellagui-cctv [--config FILE] [--size WxH] | --list | --selftest | --watch\n");
            return 2;
        }
    }
    static struct app a;
    static char path[PATH_MAX];
    conf_path(path, sizeof(path), cli_conf);
    a.conf = path;
    a.single = a.hover = a.rhover = a.shover = a.moving = a.arch = a.click_row = a.pos_ask = -1;
    load_conf(&a);
    const char *dir = getenv("MELLAGUI_CCTV_DIR"), *home = getenv("HOME");
    if (dir && *dir)
        snprintf(a.dir, sizeof(a.dir), "%s", dir);
    else
        snprintf(a.dir, sizeof(a.dir), "%s/CCTV", home ? home : ".");
    const char *alarms = getenv("MELLAGUI_CCTV_ALARMS");
    if (alarms && *alarms) {
        snprintf(a.wants_path, sizeof(a.wants_path), "%s", alarms);
    } else {                                /* beside the list of cameras */
        const char *slash = strrchr(path, '/');
        snprintf(a.wants_path, sizeof(a.wants_path), "%.*scctv-alarms.conf", slash ? (int)(slash - path + 1) : 0, path);
    }
    const char *names[MAX_CAMS];
    struct cctv_target targets[MAX_CAMS];
    for (int i = 0; i < a.n; i++) {
        struct cam *c = &a.cam[i];
        names[i] = c->name;
        targets[i] = (struct cctv_target){ .name = c->name, .host = c->host, .port = c->port, .user = c->user, .password = c->pass };
    }
    cctv_wants_load(a.wants_path, names, a.want, a.n);
    if (watch) {
        if (!a.n)
            return 0;
        return cctv_watch(targets, a.n, a.wants_path, a.dir, ffpipe_ffmpeg_path());
    }
    if (list) {
        for (int i = 0; i < a.n; i++)
            printf("%s\t%s:%d\t%s\t%s\n", a.cam[i].name, a.cam[i].host, a.cam[i].port, a.cam[i].user, a.cam[i].pass[0] ? "password set" : "no password");
        if (!a.n)
            fprintf(stderr, "mellagui-cctv: %s (%s)\n", a.conf_note, a.conf);
        return a.n ? 0 : 1;
    }
    g_ffmpeg = ffpipe_ffmpeg_path();
    g_dump = getenv("MELLAGUI_CCTV_DUMP");
    wlrail_init(&a.ran);
    wlrail_init(&a.san);
    watcher_start(&a);
    static const struct wlwin_ops ops = { .paint = paint, .pointer = pointer_event, .keyboard = keyboard_event, .menu = menu_event };
    if (wlwin_connect(&a.win, "mellagui-cctv", &ops, &a) < 0)
        return 1;
    wlwin_set_inset(&a.win, WLRAIL_W);
    if (wlwin_open(&a.win, "Cameras", "mellagui.cctv", win_w, win_h, 520, 440) < 0)
        return 1;
    for (int i = 0; i < a.n; i++) {
        struct cam *c = &a.cam[i];
        dv_init(&c->s);
        pthread_mutex_init(&c->lock, NULL);
        if (pipe2(c->wake, O_CLOEXEC | O_NONBLOCK) < 0 || pthread_create(&c->thread, NULL, cam_thread, c) != 0) {
            fprintf(stderr, "mellagui-cctv: cannot start a thread\n");
            return 1;
        }
    }
    struct sigaction sa = { .sa_handler = on_end };
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    bool started = false;
    while (a.win.running && !g_end) {
        if (!started && a.win.configured) {         /* connect when the room for the pictures is known */
            layout(&a);
            set_wants(&a);
            started = true;
        }
        struct wlrail L;
        rail(&a, &L);
        int timeout = wlrail_loop(&a.win, &a.ran, &L, a.rhover, a.n ? 30 : -1);
        struct wlrail S;
        side_rail(&a, &S);
        timeout = wlrail_loop(&a.win, &a.san, &S, a.shover, timeout);
        if (!wlwin_step(&a.win, timeout))
            break;
        collect_jobs(&a);
        watch_steering(&a);
        collect_arch(&a);
        if (a.pos_ask >= 0 && now_ms() >= a.pos_ask_until) {
            a.pos_ask = -1;
            a.win.redraw = true;
        }
        if (a.note[0] && now_ms() >= a.note_until) {
            a.note[0] = 0;
            a.win.redraw = true;
        }
        for (int i = 0; i < a.n + (a.play != NULL); i++) {
            struct cam *c = i < a.n ? &a.cam[i] : &a.play->cam;
            pthread_mutex_lock(&c->lock);
            if (c->serial != c->seen) {
                c->seen = c->serial;
                a.win.redraw = true;
            }
            pthread_mutex_unlock(&c->lock);
        }
    }
    for (int i = 0; i < a.n; i++) {
        struct cam *c = &a.cam[i];
        pthread_mutex_lock(&c->lock);
        c->quit = true;
        pthread_mutex_unlock(&c->lock);
        poke(c);
    }
    playback_free(a.play);
    cctv_arch_free(a.list);
    cctv_arch_free(a.fetch);
    for (int i = 0; i < a.n; i++)           /* a camera that moves is stopped before anything else */
        cctv_ctl_free(a.cam[i].ctl);
    for (int i = 0; i < a.n; i++)           /* all are told first, so that they finish side by side */
        if (a.cam[i].rec)
            cctv_job_stop(a.cam[i].rec);
    for (int i = 0; i < a.n; i++) {
        cctv_job_free(a.cam[i].snap);
        cctv_job_free(a.cam[i].rec);
        pthread_join(a.cam[i].thread, NULL);
        free(a.cam[i].px);
        for (int k = 0; k < CCTV_POSITIONS; k++) {
            free(a.cam[i].pos[k].px);
            free(a.cam[i].pos_new[k].px);
        }
    }
    wlwin_destroy(&a.win);
    return 0;
}
