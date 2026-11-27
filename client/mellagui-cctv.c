/* mellagui-cctv - live pictures of the network cameras in the house (XiongMai cameras, the ones the iCSee app talks to), a window of the wlwin family.
 *
 *   mellagui-cctv [--config FILE] [--size WxH]
 *   mellagui-cctv --list | --selftest
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
 * Every camera has a thread of its own: it keeps the connection, feeds ffmpeg and takes its pictures; the window only shows the newest one.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
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
#include "wlwin.h"

#define MAX_CAMS 8
#define STAT_H 24
#define GAP 6
#define PENDING_MAX (8u << 20)              /* video waiting for a decoder that is too slow */
#define SILENT_MS 15000                     /* a stream with no pictures for this long is made again */

enum state { ST_IDLE, ST_CONNECTING, ST_WAITING, ST_LIVE, ST_RETRY, ST_REFUSED };
static const char *const STATE_NAME[] = { "idle", "connecting", "waiting", "live", "retry", "refused" };
enum { WANT_NONE, WANT_SUB, WANT_MAIN };

struct decoder {
    pid_t pid;
    int in, out;
    int codec, w, h;                        /* what it reads and the size of the pictures it makes */
    int src_w, src_h;
    uint32_t *px;                           /* the picture being read */
    size_t have;
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
    bool broken;                            /* the connection is to be dropped (cur message says why) */
    char why[200];
    /* the window's own */
    unsigned seen;
    struct lm_rect tile, pic;
};

struct app {
    struct wlwin win;
    struct cam cam[MAX_CAMS];
    int n;
    int single;                             /* the camera shown alone, -1 = all */
    int hover, rhover;                      /* the tile and the rail button under the pointer */
    double px, py;
    struct wlrail_anim ran;
    const char *conf;
    char conf_note[300];                    /* why there are no cameras */
};

static const char *g_dump;
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

static void on_frame(void *ud, const struct dv_frame *f)
{
    struct cam *c = ud;
    if (f->kind != DV_VIDEO && f->kind != DV_VIDEO_KEY)
        return;                             /* (sound and the rest come later) */
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
        if (p[3].revents && c->dec.pid) {
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
        if (p[4].revents && c->dec.pid && c->pend_len) {
            ssize_t k = write(c->dec.in, c->pend, c->pend_len);
            if (k > 0) {
                memmove(c->pend, c->pend + k, c->pend_len - (size_t)k);
                c->pend_len -= (size_t)k;
            } else if (k < 0 && errno != EAGAIN && errno != EINTR) {
                c->pend_len = 0;            /* (the end of ffmpeg shows on its output) */
            }
        }
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

static void poke(struct cam *c)
{
    if (write(c->wake[1], "x", 1) < 0) {}
}

/* What each camera is to show now. */
static void set_wants(struct app *a)
{
    for (int i = 0; i < a->n; i++) {
        struct cam *c = &a->cam[i];
        int want = a->single < 0 ? WANT_SUB : a->single == i ? WANT_MAIN : WANT_NONE;
        pthread_mutex_lock(&c->lock);
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

enum { B_ALL, B_CAM0, B_AGAIN = B_CAM0 + MAX_CAMS, B_COUNT };

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

/* Where the tiles go: all cameras in the arrangement that makes the pictures largest, or one alone. */
static void layout(struct app *a)
{
    int W = a->win.bw, H = a->win.bh - STAT_H;
    for (int i = 0; i < a->n; i++)
        a->cam[i].tile = a->cam[i].pic = (struct lm_rect){ 0, 0, 0, 0 };
    if (!a->n || W < 2 * GAP + 16 || H < 2 * GAP + 16)
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
    }
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
        centred(w, (struct lm_rect){ 0, 0, w->bw, w->bh - STAT_H }, text, lm_theme.text);
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
        if (st == ST_LIVE && (a->single == i || a->hover == i)) {
            char info[96];
            snprintf(info, sizeof(info), "%d x %d  %d.%d fps  %d kbit/s", sw, sh, fps10 / 10, fps10 % 10, kbps);
            int tw = LM_MIN(wlcard_text_w(info) + 20, c->tile.w - 16);
            chip(w, c->tile.x + c->tile.w - tw - 8, c->tile.y + c->tile.h - 32, info, c->tile.w - 16, 0);
        }
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
    if (!g_ffmpeg)
        snprintf(status, sizeof(status), "ffmpeg is not installed: the pictures cannot be shown");
    else if (a->single >= 0)
        snprintf(status, sizeof(status), "%s (%s)  -  Esc: all cameras", a->cam[a->single].name, a->cam[a->single].host);
    else
        snprintf(status, sizeof(status), "%d of %d cameras live%s", live, a->n, a->n ? "  -  click a picture to see it alone" : "");
    wlwin_text(w, 8, sy + (STAT_H - LM_FONT_H) / 2, status, 1, lm_theme.text, w->bw - 16);
    struct wlrail L;
    rail(a, &L);
    wlrail_foot(w, &L, lm_theme.bar);
    wlrail_paint(w, &L, &a->ran, a->rhover, now_ms(), lm_theme.bar, lm_theme.bar, lm_theme.accent, lm_theme.text);
    write_dump(a);
}

static void show(struct app *a, int i)
{
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
        a->hover = a->rhover = -1;
        wlrail_release(&a->ran);
        w->redraw = true;
        return;
    } else if (op != LM_WL_POINTER_EVT_BUTTON) {
        return;
    }
    struct wlrail L;
    rail(a, &L);
    if (op != LM_WL_POINTER_EVT_BUTTON) {
        int h = tile_at(a, a->px, a->py), rh = wlrail_at(&L, w->rpx, w->rpy);
        if (h != a->hover || rh != a->rhover)
            w->redraw = true;
        a->hover = h;
        a->rhover = rh;
        return;
    }
    if (args[3].u != 1) {
        wlrail_release(&a->ran);
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
    int t = tile_at(a, a->px, a->py);
    if (t >= 0)
        show(a, a->single == t ? -1 : t);
}

static void keyboard_event(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
    struct app *a = w->data;
    if (op != LM_WL_KEYBOARD_EVT_KEY || args[3].u == 0)
        return;
    uint32_t ks = lm_keymap_keysym(args[2].u, w->mods);
    if (w->mods & LM_MOD_CTRL) {
        if (ks == 'q' || ks == 'Q' || ks == 'w' || ks == 'W')
            w->running = false;
        return;
    }
    if (ks == LM_KS_Escape || ks == '0' || ks == 'g')
        show(a, -1);
    else if (ks >= '1' && ks <= '0' + (uint32_t)a->n)
        show(a, (int)(ks - '1'));
    else if (ks == 'r' || ks == 'R')
        press_button(a, B_AGAIN);
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
    int win_w = 960, win_h = 440;
    bool list = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--selftest"))
            return selftest();
        if (!strcmp(argv[i], "--list")) {
            list = true;
        } else if (!strcmp(argv[i], "--config") && i + 1 < argc) {
            cli_conf = argv[++i];
        } else if (!strcmp(argv[i], "--size") && i + 1 < argc) {
            char junk;
            if (sscanf(argv[++i], "%dx%d%c", &win_w, &win_h, &junk) != 2 || win_w < 360 || win_h < 240) {
                fprintf(stderr, "mellagui-cctv: --size wants a size like 960x440\n");
                return 2;
            }
        } else {
            fprintf(stderr, "usage: mellagui-cctv [--config FILE] [--size WxH] | --list | --selftest\n");
            return 2;
        }
    }
    static struct app a;
    static char path[PATH_MAX];
    conf_path(path, sizeof(path), cli_conf);
    a.conf = path;
    a.single = a.hover = a.rhover = -1;
    load_conf(&a);
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
    static const struct wlwin_ops ops = { .paint = paint, .pointer = pointer_event, .keyboard = keyboard_event };
    if (wlwin_connect(&a.win, "mellagui-cctv", &ops, &a) < 0)
        return 1;
    wlwin_set_inset(&a.win, WLRAIL_W);
    if (wlwin_open(&a.win, "Cameras", "mellagui.cctv", win_w, win_h, 360, 240) < 0)
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
    bool started = false;
    while (a.win.running) {
        if (!started && a.win.configured) {         /* connect when the room for the pictures is known */
            layout(&a);
            set_wants(&a);
            started = true;
        }
        struct wlrail L;
        rail(&a, &L);
        int timeout = wlrail_loop(&a.win, &a.ran, &L, a.rhover, a.n ? 30 : -1);
        if (!wlwin_step(&a.win, timeout))
            break;
        for (int i = 0; i < a.n; i++) {
            struct cam *c = &a.cam[i];
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
    for (int i = 0; i < a.n; i++) {
        pthread_join(a.cam[i].thread, NULL);
        free(a.cam[i].px);
    }
    wlwin_destroy(&a.win);
    return 0;
}
