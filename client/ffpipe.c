#include "ffpipe.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

struct ffpipe {
    pid_t pid;
    int fd;
    uint8_t *pend;                      /* ffpipe_offer: the rest of a picture the pipe did not take yet */
    size_t pend_len, pend_cap;
    bool nonblock;
};

static char g_ffmpeg[PATH_MAX];
static int g_state;                 /* 0 not looked up, 1 usable, -1 not */

static bool find_in_path(const char *name, char *out, size_t n)
{
    const char *path = getenv("PATH");
    if (!path || !*path)
        path = "/usr/local/bin:/usr/bin:/bin";
    char copy[4096];
    snprintf(copy, sizeof(copy), "%s", path);
    for (char *save = NULL, *dir = strtok_r(copy, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
        snprintf(out, n, "%s/%s", *dir ? dir : ".", name);
        if (access(out, X_OK) == 0)
            return true;
    }
    return false;
}

const char *ffpipe_ffmpeg_path(void)
{
    const char *force = getenv("MELLAGUI_FFMPEG");              /* tests / odd installs: a path, or "none" */
    if (force && !strcmp(force, "none"))
        return NULL;
    if (force && *force)
        snprintf(g_ffmpeg, sizeof(g_ffmpeg), "%s", force);
    else if (!g_ffmpeg[0] && !find_in_path("ffmpeg", g_ffmpeg, sizeof(g_ffmpeg)))
        return NULL;
    return g_ffmpeg;
}

bool ffpipe_available(void)
{
    if (g_state)
        return g_state > 0;
    g_state = -1;
    if (!ffpipe_ffmpeg_path())
        return false;
    int out[2];
    if (pipe2(out, O_CLOEXEC) < 0)
        return false;
    pid_t pid = fork();
    if (pid < 0) {
        close(out[0]);
        close(out[1]);
        return false;
    }
    if (pid == 0) {
        int nul = open("/dev/null", O_RDWR);
        dup2(nul, 0);
        dup2(out[1], 1);
        dup2(nul, 2);
        execl(g_ffmpeg, g_ffmpeg, "-hide_banner", "-encoders", (char *)NULL);
        _exit(127);
    }
    close(out[1]);
    char buf[65536];
    size_t len = 0;
    ssize_t k;
    while (len < sizeof(buf) - 1 && (k = read(out[0], buf + len, sizeof(buf) - 1 - len)) > 0)
        len += (size_t)k;
    close(out[0]);
    waitpid(pid, NULL, 0);
    buf[len] = 0;
    if (strstr(buf, "libx264"))
        g_state = 1;
    return g_state > 0;
}

const char *ffpipe_audio_source(bool mic)
{
    static char buf[2][256];
    const char *e = getenv(mic ? "MELLAGUI_RECORD_MIC_SRC" : "MELLAGUI_RECORD_SYSTEM_SRC");
    if (e && !strcmp(e, "none"))
        return NULL;
    char *out = buf[mic ? 1 : 0];
    snprintf(out, sizeof(buf[0]), "%s", e && *e ? e : mic ? "pulse:@DEFAULT_SOURCE@" : "pulse:@DEFAULT_MONITOR@");
    return out;
}

#define MAX_ARGS 128

struct argvb {
    char *v[MAX_ARGS];
    int n;
};

static void arg(struct argvb *a, const char *s)
{
    if (a->n < MAX_ARGS - 1)
        a->v[a->n++] = strdup(s);
}

/* "format:device" -> `-f format -i device`. */
static bool arg_source(struct argvb *a, const char *spec)
{
    const char *colon = strchr(spec, ':');
    if (!colon || colon == spec || !colon[1])
        return false;
    char fmt[32];
    snprintf(fmt, sizeof(fmt), "%.*s", (int)(colon - spec) < 31 ? (int)(colon - spec) : 31, spec);
    bool synthetic = !strcmp(fmt, "lavfi");             /* a test source: it makes its own times, and `-re` keeps it to the speed of the clock like a microphone */
    if (synthetic)
        arg(a, "-re");
    arg(a, "-thread_queue_size");
    arg(a, "1024");
    if (!synthetic) {
        arg(a, "-use_wallclock_as_timestamps");
        arg(a, "1");
    }
    arg(a, "-f");
    arg(a, fmt);
    arg(a, "-i");
    arg(a, colon + 1);
    return true;
}

struct ffpipe *ffpipe_start(const char *path, int width, int height, int crf, const struct ffpipe_audio *audio, char *err, size_t errsz)
{
    if (!ffpipe_available()) {
        snprintf(err, errsz, "ffmpeg with libx264 is not installed");
        return NULL;
    }
    int in[2];
    if (pipe2(in, O_CLOEXEC) < 0) {
        snprintf(err, errsz, "pipe: %s", strerror(errno));
        return NULL;
    }
    int status[2];
    if (pipe2(status, O_CLOEXEC) < 0) {                 /* exec failures come back through this one */
        close(in[0]);
        close(in[1]);
        snprintf(err, errsz, "pipe: %s", strerror(errno));
        return NULL;
    }
    char size[32], crfs[16];
    snprintf(size, sizeof(size), "%dx%d", width, height);
    snprintf(crfs, sizeof(crfs), "%d", crf < 0 ? 0 : crf > 51 ? 51 : crf);

    /* the command line, made before the fork (no malloc in the child) */
    struct argvb a = { .n = 0 };
    arg(&a, g_ffmpeg);
    const char *fixed1[] = { "-hide_banner", "-loglevel", "error", "-nostdin", "-y", "-use_wallclock_as_timestamps", "1", "-f", "rawvideo", "-pix_fmt", "bgr0",
                             "-video_size", size, "-i", "pipe:0" };
    for (size_t i = 0; i < sizeof(fixed1) / sizeof(fixed1[0]); i++)
        arg(&a, fixed1[i]);
    int naudio = 0;
    if (audio && audio->system && arg_source(&a, audio->system))
        naudio++;
    if (audio && audio->mic && arg_source(&a, audio->mic))
        naudio++;
    /* even sides for yuv420p, full range RGB to BT.709 limited range */
    const char *vf = "[0:v]scale=trunc(iw/2)*2:trunc(ih/2)*2:in_range=full:out_range=tv:out_color_matrix=bt709[v]";
    char fc[320];
    if (naudio == 2)
        snprintf(fc, sizeof(fc), "%s;[1:a][2:a]amix=inputs=2:duration=longest:normalize=0,alimiter=limit=0.97[a]", vf);
    else
        snprintf(fc, sizeof(fc), "%s", vf);
    arg(&a, "-filter_complex");
    arg(&a, fc);
    arg(&a, "-map");
    arg(&a, "[v]");
    if (naudio == 2) {
        arg(&a, "-map");
        arg(&a, "[a]");
    } else if (naudio == 1) {
        arg(&a, "-map");
        arg(&a, "1:a");
    }
    const char *fixed2[] = { "-c:v", "libx264", "-preset", "veryfast", "-crf", crfs, "-pix_fmt", "yuv420p", "-colorspace", "bt709", "-color_primaries", "bt709",
                             "-color_trc", "bt709", "-color_range", "tv", "-fps_mode", "vfr", "-g", "120" };
    for (size_t i = 0; i < sizeof(fixed2) / sizeof(fixed2[0]); i++)
        arg(&a, fixed2[i]);
    if (naudio > 0) {                                   /* the file ends with the pictures (the recorder sends a last one when it stops) */
        const char *snd[] = { "-c:a", "aac", "-b:a", "160k", "-ar", "48000", "-ac", "2", "-shortest" };
        for (size_t i = 0; i < sizeof(snd) / sizeof(snd[0]); i++)
            arg(&a, snd[i]);
    }
    const char *tail[] = { "-movflags", "frag_keyframe+empty_moov+default_base_moof", "-f", "mp4", path };
    for (size_t i = 0; i < sizeof(tail) / sizeof(tail[0]); i++)
        arg(&a, tail[i]);
    a.v[a.n] = NULL;

    pid_t pid = fork();
    if (pid < 0) {
        snprintf(err, errsz, "fork: %s", strerror(errno));
        close(in[0]); close(in[1]); close(status[0]); close(status[1]);
        for (int i = 0; i < a.n; i++)
            free(a.v[i]);
        return NULL;
    }
    if (pid == 0) {
        setpgid(0, 0);                                  /* Ctrl+C in a terminal is for the recorder, which ends the file itself */
        dup2(in[0], 0);
        int nul = open("/dev/null", O_WRONLY);
        if (nul >= 0)
            dup2(nul, 1);
        execv(g_ffmpeg, a.v);
        int e = errno;
        if (write(status[1], &e, sizeof(e)) < 0) {}
        _exit(127);
    }
    for (int i = 0; i < a.n; i++)
        free(a.v[i]);
    close(in[0]);
    close(status[1]);
    int e = 0;
    ssize_t k = read(status[0], &e, sizeof(e));
    close(status[0]);
    if (k == (ssize_t)sizeof(e)) {
        waitpid(pid, NULL, 0);
        close(in[1]);
        snprintf(err, errsz, "cannot start ffmpeg: %s", strerror(e));
        return NULL;
    }
    struct ffpipe *p = calloc(1, sizeof(*p));
    if (!p) {
        close(in[1]);
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        snprintf(err, errsz, "out of memory");
        return NULL;
    }
    p->pid = pid;
    p->fd = in[1];
    signal(SIGPIPE, SIG_IGN);                            /* a dead ffmpeg is an error code, not a signal */
    return p;
}

static void set_nonblock(struct ffpipe *p, bool on);
static int drain_pending(struct ffpipe *p);

int ffpipe_write(struct ffpipe *p, const void *px, size_t bytes)
{
    if (p->pend_len > 0) {                              /* the rest of an earlier picture (ffpipe_offer) goes first */
        set_nonblock(p, false);
        if (drain_pending(p) < 0)
            return -1;
    }
    const uint8_t *q = px;
    while (bytes > 0) {
        ssize_t k = write(p->fd, q, bytes);
        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0)
            return -1;
        q += k;
        bytes -= (size_t)k;
    }
    return 0;
}

static void set_nonblock(struct ffpipe *p, bool on)
{
    int fl = fcntl(p->fd, F_GETFL);
    if (fl >= 0)
        fcntl(p->fd, F_SETFL, on ? fl | O_NONBLOCK : fl & ~O_NONBLOCK);
    p->nonblock = on;
}

/* Writes what is left of the picture the pipe was too full for. 0 = all gone, 1 = still some, -1 = ffmpeg is gone. */
static int drain_pending(struct ffpipe *p)
{
    while (p->pend_len > 0) {
        ssize_t k = write(p->fd, p->pend, p->pend_len);
        if (k < 0 && errno == EINTR)
            continue;
        if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return 1;
        if (k <= 0)
            return -1;
        memmove(p->pend, p->pend + k, p->pend_len - (size_t)k);
        p->pend_len -= (size_t)k;
    }
    return 0;
}

int ffpipe_offer(struct ffpipe *p, const void *px, size_t bytes)
{
    if (!p->nonblock) {
        set_nonblock(p, true);
        fcntl(p->fd, F_SETPIPE_SZ, 4 << 20);            /* room for a few pictures: a short stall of x264 costs nothing */
    }
    int r = drain_pending(p);
    if (r != 0)
        return r;                                       /* busy (1) or gone (-1) */
    const uint8_t *q = px;
    size_t left = bytes;
    while (left > 0) {
        ssize_t k = write(p->fd, q, left);
        if (k < 0 && errno == EINTR)
            continue;
        if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        if (k <= 0)
            return -1;
        q += k;
        left -= (size_t)k;
    }
    if (left > 0) {                                     /* a picture is never cut: the rest waits here until the next offer */
        if (p->pend_cap < left) {
            uint8_t *n = realloc(p->pend, left);
            if (!n)
                return -1;
            p->pend = n;
            p->pend_cap = left;
        }
        memcpy(p->pend, q, left);
        p->pend_len = left;
    }
    return 0;
}

int ffpipe_finish(struct ffpipe *p)
{
    if (!p)
        return -1;
    if (p->pend_len > 0) {                              /* the rest of the last picture, then the end */
        set_nonblock(p, false);
        drain_pending(p);
    }
    free(p->pend);
    close(p->fd);
    int st = 0, rc = -1;
    /* With sound ffmpeg also has live inputs; the end of the pictures ends the file (-shortest). A few seconds' grace, then it is asked to stop (SIGINT:
     * it finishes the file, and says "255" for it). */
    pid_t r = 0;
    for (int i = 0; i < 100 && (r = waitpid(p->pid, &st, WNOHANG)) == 0; i++)
        usleep(50000);
    if (r == 0) {
        kill(p->pid, SIGINT);
        for (int i = 0; i < 100 && (r = waitpid(p->pid, &st, WNOHANG)) == 0; i++)
            usleep(50000);
        if (r == 0) {
            kill(p->pid, SIGKILL);
            r = waitpid(p->pid, &st, 0);
        }
        if (r == p->pid)
            rc = WIFEXITED(st) && (WEXITSTATUS(st) == 0 || WEXITSTATUS(st) == 255) ? 0 : -1;
    } else if (r == p->pid) {
        rc = WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
    }
    free(p);
    return rc;
}
