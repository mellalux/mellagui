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

struct ffpipe *ffpipe_start(const char *path, int width, int height, int crf, char *err, size_t errsz)
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
    pid_t pid = fork();
    if (pid < 0) {
        snprintf(err, errsz, "fork: %s", strerror(errno));
        close(in[0]); close(in[1]); close(status[0]); close(status[1]);
        return NULL;
    }
    if (pid == 0) {
        setpgid(0, 0);                                  /* Ctrl+C in a terminal is for the recorder, which ends the file itself */
        dup2(in[0], 0);
        int nul = open("/dev/null", O_WRONLY);
        if (nul >= 0)
            dup2(nul, 1);
        /* even sides for yuv420p, full range RGB to BT.709 limited range */
        execl(g_ffmpeg, g_ffmpeg, "-hide_banner", "-loglevel", "error", "-nostdin", "-y", "-use_wallclock_as_timestamps", "1", "-f", "rawvideo",
              "-pix_fmt", "bgr0", "-video_size", size, "-i", "pipe:0", "-vf",
              "scale=trunc(iw/2)*2:trunc(ih/2)*2:in_range=full:out_range=tv:out_color_matrix=bt709", "-c:v", "libx264", "-preset", "veryfast",
              "-crf", crfs, "-pix_fmt", "yuv420p", "-colorspace", "bt709", "-color_primaries", "bt709", "-color_trc", "bt709",
              "-color_range", "tv", "-fps_mode", "vfr", "-g", "120", "-movflags", "frag_keyframe+empty_moov+default_base_moof", "-f", "mp4", path,
              (char *)NULL);
        int e = errno;
        if (write(status[1], &e, sizeof(e)) < 0) {}
        _exit(127);
    }
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

int ffpipe_write(struct ffpipe *p, const void *px, size_t bytes)
{
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

int ffpipe_finish(struct ffpipe *p)
{
    if (!p)
        return -1;
    close(p->fd);
    int st = 0, rc = -1;
    if (waitpid(p->pid, &st, 0) == p->pid)
        rc = WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
    free(p);
    return rc;
}
