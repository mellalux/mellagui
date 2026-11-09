#include "ffdec.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ffpipe.h"

struct ffdec {
    pid_t pid;
    int fd;
    size_t frame_bytes;
};

/* "00:01:05.92" -> ms */
static uint64_t parse_hms(const char *s)
{
    unsigned h = 0, m = 0;
    double sec = 0;
    if (sscanf(s, "%u:%u:%lf", &h, &m, &sec) != 3)
        return 0;
    return ((uint64_t)h * 3600 + (uint64_t)m * 60) * 1000 + (uint64_t)(sec * 1000.0 + 0.5);
}

bool ffdec_probe(const char *path, struct ffdec_info *info, char *err, size_t errsz)
{
    const char *ff = ffpipe_ffmpeg_path();
    if (!ff) {
        snprintf(err, errsz, "ffmpeg is not installed");
        return false;
    }
    int out[2];
    if (pipe2(out, O_CLOEXEC) < 0) {
        snprintf(err, errsz, "pipe: %s", strerror(errno));
        return false;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(out[0]);
        close(out[1]);
        snprintf(err, errsz, "fork: %s", strerror(errno));
        return false;
    }
    if (pid == 0) {
        int nul = open("/dev/null", O_RDWR);
        dup2(nul, 0);
        dup2(nul, 1);
        dup2(out[1], 2);                                        /* `ffmpeg -i FILE` tells about the file on stderr */
        execl(ff, ff, "-hide_banner", "-nostdin", "-i", path, (char *)NULL);
        _exit(127);
    }
    close(out[1]);
    char buf[16384];
    size_t len = 0;
    ssize_t k;
    while (len < sizeof(buf) - 1 && (k = read(out[0], buf + len, sizeof(buf) - 1 - len)) > 0)
        len += (size_t)k;
    close(out[0]);
    waitpid(pid, NULL, 0);
    buf[len] = 0;
    memset(info, 0, sizeof(*info));
    const char *d = strstr(buf, "Duration: ");
    if (d)
        info->duration_ms = parse_hms(d + 10);
    const char *v = strstr(buf, "Video: ");
    if (!v) {
        snprintf(err, errsz, "no video stream in this file");
        return false;
    }
    v += 7;
    size_t n = 0;
    while (v[n] && v[n] != ' ' && v[n] != ',' && n < sizeof(info->codec) - 1) {
        info->codec[n] = v[n];
        n++;
    }
    info->codec[n] = 0;
    for (const char *p = v; *p && *p != '\n'; p++) {            /* the first " WIDTHxHEIGHT" that is no hex number */
        if (isdigit((unsigned char)*p) && (p == v || p[-1] == ' ' || p[-1] == ',')) {
            int w, h, used = 0;
            if (sscanf(p, "%dx%d%n", &w, &h, &used) == 2 && w >= 16 && h >= 16 && w <= 16384 && h <= 16384 &&
                (p[used] == ' ' || p[used] == ',' || p[used] == '[' || p[used] == 0 || p[used] == '\n')) {
                info->w = w;
                info->h = h;
                break;
            }
        }
    }
    if (!info->w) {
        snprintf(err, errsz, "cannot tell the size of the picture");
        return false;
    }
    return true;
}

struct ffdec *ffdec_open(const char *path, int w, int h, int fps, uint64_t start_ms, char *err, size_t errsz)
{
    const char *ff = ffpipe_ffmpeg_path();
    if (!ff) {
        snprintf(err, errsz, "ffmpeg is not installed");
        return NULL;
    }
    int out[2], status[2];
    if (pipe2(out, O_CLOEXEC) < 0 || pipe2(status, O_CLOEXEC) < 0) {
        snprintf(err, errsz, "pipe: %s", strerror(errno));
        return NULL;
    }
    char ss[32], vf[64];
    snprintf(ss, sizeof(ss), "%llu.%03llu", (unsigned long long)(start_ms / 1000), (unsigned long long)(start_ms % 1000));
    snprintf(vf, sizeof(vf), "fps=%d", fps);
    pid_t pid = fork();
    if (pid < 0) {
        snprintf(err, errsz, "fork: %s", strerror(errno));
        return NULL;
    }
    if (pid == 0) {
        setpgid(0, 0);
        int nul = open("/dev/null", O_RDWR);
        dup2(nul, 0);
        dup2(out[1], 1);
        execl(ff, ff, "-hide_banner", "-loglevel", "error", "-nostdin", "-ss", ss, "-i", path, "-an", "-sn", "-vf", vf, "-f", "rawvideo", "-pix_fmt", "bgr0", "pipe:1",
              (char *)NULL);
        int e = errno;
        if (write(status[1], &e, sizeof(e)) < 0) {}
        _exit(127);
    }
    close(out[1]);
    close(status[1]);
    int e = 0;
    ssize_t k = read(status[0], &e, sizeof(e));
    close(status[0]);
    if (k == (ssize_t)sizeof(e)) {
        waitpid(pid, NULL, 0);
        close(out[0]);
        snprintf(err, errsz, "cannot start ffmpeg: %s", strerror(e));
        return NULL;
    }
    struct ffdec *d = calloc(1, sizeof(*d));
    if (!d) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        close(out[0]);
        return NULL;
    }
    d->pid = pid;
    d->fd = out[0];
    d->frame_bytes = (size_t)w * (size_t)h * 4;
    fcntl(d->fd, F_SETFL, fcntl(d->fd, F_GETFL) | O_NONBLOCK);
    return d;
}

int ffdec_read(struct ffdec *d, void *buf, size_t *have)
{
    while (*have < d->frame_bytes) {
        ssize_t k = read(d->fd, (uint8_t *)buf + *have, d->frame_bytes - *have);
        if (k > 0) {
            *have += (size_t)k;
            continue;
        }
        if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return 0;
        if (k < 0 && errno == EINTR)
            continue;
        return -1;                                              /* end of the stream (or an error) */
    }
    *have = 0;
    return 1;
}

void ffdec_close(struct ffdec *d)
{
    if (!d)
        return;
    close(d->fd);
    kill(d->pid, SIGKILL);                                      /* it may be waiting on a full pipe */
    waitpid(d->pid, NULL, 0);
    free(d);
}
