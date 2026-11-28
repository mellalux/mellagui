#include "cctvarch.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SILENT_MS 15000

struct cctv_arch {
    pthread_t thread;
    pthread_mutex_t lock;
    int wake[2];
    bool fetch;
    char host[128], user[64], pass[96], day[16], dir[400], base[96], ffmpeg[400];
    int port;
    struct dv_file file;
    bool stop;                          /* under the lock */
    struct cctv_arch_status st;         /* under the lock */
    /* the thread's own */
    struct dv_session s;
    struct dv_demux demux;
    struct dv_packets packets;
    struct dv_file *files;
    int video, sound;                   /* the parts being collected */
    int codec;
    uint64_t frames, samples, bytes;
    bool ended, broken;
};

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static bool stopping(struct cctv_arch *j)
{
    pthread_mutex_lock(&j->lock);
    bool s = j->stop;
    pthread_mutex_unlock(&j->lock);
    return s;
}

static void finish(struct cctv_arch *j, bool ok, const char *msg)
{
    pthread_mutex_lock(&j->lock);
    j->st.done = true;
    j->st.failed = !ok;
    snprintf(j->st.msg, sizeof(j->st.msg), "%s", msg);
    j->st.serial++;
    pthread_mutex_unlock(&j->lock);
}

static bool put(int fd, const void *p, size_t n)
{
    const char *c = p;
    while (n) {
        ssize_t k = write(fd, c, n);
        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0)
            return false;
        c += k;
        n -= (size_t)k;
    }
    return true;
}

static void on_frame(void *ud, const struct dv_frame *f)
{
    struct cctv_arch *j = ud;
    if (f->kind == DV_VIDEO_KEY && !j->codec && (f->codec == DV_CODEC_H264 || f->codec == DV_CODEC_H265))
        j->codec = f->codec;
    if ((f->kind == DV_VIDEO || f->kind == DV_VIDEO_KEY) && j->codec) {
        if (!put(j->video, f->data, f->len))
            j->broken = true;
        j->frames++;
    } else if (f->kind == DV_AUDIO && f->codec == DV_SOUND_ALAW && j->codec) {
        if (!put(j->sound, f->data, f->len))
            j->broken = true;
        j->samples += f->len;
    }
}

static void on_packet(void *ud, int msg, const uint8_t *body, size_t len)
{
    struct cctv_arch *j = ud;
    if (msg != DV_MSG_PLAY || j->ended)
        return;
    if (!len) {
        j->ended = true;
        return;
    }
    j->bytes += len;
    if (!dv_demux_feed(&j->demux, body, len, on_frame, j))
        j->broken = true;
}

/* The length of the file by the camera's clock, in seconds. */
static long seconds(const struct dv_file *f)
{
    struct tm a = { 0 }, b = { 0 };
    if (sscanf(f->begin, "%d-%d-%d %d:%d:%d", &a.tm_year, &a.tm_mon, &a.tm_mday, &a.tm_hour, &a.tm_min, &a.tm_sec) != 6 ||
        sscanf(f->end, "%d-%d-%d %d:%d:%d", &b.tm_year, &b.tm_mon, &b.tm_mday, &b.tm_hour, &b.tm_min, &b.tm_sec) != 6)
        return 0;
    a.tm_year -= 1900;
    b.tm_year -= 1900;
    a.tm_mon--;
    b.tm_mon--;
    return (long)(timegm(&b) - timegm(&a));
}

/* Runs ffmpeg and waits for it, giving up when asked to. */
static bool run(struct cctv_arch *j, const char *const *argv)
{
    pid_t pid = fork();
    if (pid == 0) {
        umask(077);
        int nul = open("/dev/null", O_RDWR);
        dup2(nul, 0);
        dup2(nul, 1);
        dup2(nul, 2);
        execv(argv[0], (char *const *)argv);
        _exit(127);
    }
    if (pid < 0)
        return false;
    int status = 0;
    while (waitpid(pid, &status, WNOHANG) == 0) {
        if (stopping(j)) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            return false;
        }
        poll(NULL, 0, 20);
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static const char *do_fetch(struct cctv_arch *j, char *why, size_t cap)
{
    if (mkdir(j->dir, 0700) < 0 && errno != EEXIST)
        return "the folder cannot be made";
    char vpath[540], apath[540], part[540];
    snprintf(vpath, sizeof(vpath), "%s/.video-XXXXXX", j->dir);
    snprintf(apath, sizeof(apath), "%s/.sound-XXXXXX", j->dir);
    snprintf(part, sizeof(part), "%s/.part-XXXXXX", j->dir);
    j->video = mkostemp(vpath, O_CLOEXEC);
    j->sound = mkostemp(apath, O_CLOEXEC);
    int pfd = mkostemp(part, O_CLOEXEC);
    const char *bad = NULL;
    if (j->video < 0 || j->sound < 0 || pfd < 0) {
        bad = "no file can be made in the folder";
        goto out;
    }
    int r = dv_login(&j->s, j->host, j->port, j->user, j->pass, j->wake[0], why, cap);
    if (r != 0 || dv_play_start(&j->s, &j->file, j->wake[0], why, cap) != 0) {
        bad = why;
        goto out;
    }
    fcntl(j->s.data, F_SETFL, O_NONBLOCK);
    int64_t last = now_ms(), next_alive = last + j->s.alive_s * 500, told = 0;
    static __thread uint8_t buf[65536];
    while (!j->ended && !bad) {
        struct pollfd p[3] = { { .fd = j->wake[0], .events = POLLIN }, { .fd = j->s.data, .events = POLLIN }, { .fd = j->s.ctl, .events = POLLIN } };
        if (poll(p, 3, 500) < 0 && errno != EINTR)
            break;
        if (stopping(j))
            bad = "stopped";
        if (p[1].revents) {
            ssize_t k = recv(j->s.data, buf, sizeof(buf), 0);
            if (k > 0) {
                last = now_ms();
                if (!dv_packets_feed(&j->packets, buf, (size_t)k, on_packet, j))
                    bad = "the camera sent something that is not understood";
            } else if (!(k < 0 && (errno == EAGAIN || errno == EINTR))) {
                bad = "the connection was lost";
            }
        }
        if (p[2].revents && recv(j->s.ctl, buf, sizeof(buf), MSG_DONTWAIT) == 0)
            bad = "the connection was lost";
        if (j->broken)
            bad = "the file cannot be written (is the disk full?)";
        int64_t now = now_ms();
        if (now - last > SILENT_MS && !j->ended)
            bad = "the camera stopped sending";
        if (now >= next_alive) {
            next_alive = now + j->s.alive_s * 500;
            dv_keepalive(&j->s);
        }
        if (now - told >= 200) {
            told = now;
            pthread_mutex_lock(&j->lock);
            j->st.got_kb = (uint32_t)(j->bytes / 1024);
            j->st.serial++;
            pthread_mutex_unlock(&j->lock);
        }
    }
    if (!bad)
        dv_play_stop(&j->s, &j->file);
    if (!bad && !j->frames)
        bad = "the file has no picture";
    if (!bad) {
        /* the pictures are spread over the length of the sound (they come without times); without sound, over what the camera's clock says */
        long secs = seconds(&j->file);
        double length = j->samples > 8000 ? (double)j->samples / 8000.0 : secs > 0 ? (double)secs : (double)j->frames / 10.0;
        /* The times are written here: ffmpeg would otherwise take the rate the stream itself claims, which a camera need not keep. */
        char times[128];
        double rate = (double)j->frames / length;
        snprintf(times, sizeof(times), "setts=pts=N/%.5f/TB:dts=N/%.5f/TB:duration=1/%.5f/TB", rate, rate, rate);
        const char *argv[48];
        int n = 0;
#define ARG(x) argv[n++] = (x)
        ARG(j->ffmpeg); ARG("-hide_banner"); ARG("-loglevel"); ARG("quiet");
        ARG("-f"); ARG(j->codec == DV_CODEC_H265 ? "hevc" : "h264"); ARG("-i"); ARG(vpath);
        if (j->samples) {
            ARG("-f"); ARG("alaw"); ARG("-ar"); ARG("8000"); ARG("-ac"); ARG("1"); ARG("-i"); ARG(apath);
            ARG("-map"); ARG("0:v"); ARG("-map"); ARG("1:a"); ARG("-c:a"); ARG("aac"); ARG("-b:a"); ARG("48k");
        }
        ARG("-c:v"); ARG("copy"); ARG("-bsf:v"); ARG(times);
        if (j->codec == DV_CODEC_H265) {
            ARG("-tag:v"); ARG("hvc1");
        }
        ARG("-movflags"); ARG("+faststart"); ARG("-f"); ARG("mp4"); ARG("-y"); ARG(part);
        argv[n] = NULL;
#undef ARG
        if (!run(j, argv))
            bad = stopping(j) ? "stopped" : "ffmpeg could not make the file";
    }
    if (!bad) {
        char path[sizeof(j->st.path)];
        bad = "no name is free for the file";
        for (int k = 1; k < 100; k++) {
            char more[8] = "";
            if (k > 1)
                snprintf(more, sizeof(more), "-%d", k);
            snprintf(path, sizeof(path), "%s/%s%s.mp4", j->dir, j->base, more);
            if (link(part, path) == 0) {            /* (never over a file that is there) */
                pthread_mutex_lock(&j->lock);
                snprintf(j->st.path, sizeof(j->st.path), "%s", path);
                pthread_mutex_unlock(&j->lock);
                bad = NULL;
                break;
            }
            if (errno != EEXIST) {
                bad = "the file cannot be given its name";
                break;
            }
        }
    }
out:
    if (j->video >= 0) {
        close(j->video);
        unlink(vpath);
    }
    if (j->sound >= 0) {
        close(j->sound);
        unlink(apath);
    }
    if (pfd >= 0) {
        close(pfd);
        unlink(part);
    }
    return bad;
}

static void *job_thread(void *ud)
{
    struct cctv_arch *j = ud;
    char why[200] = "";
    if (j->fetch) {
        const char *bad = do_fetch(j, why, sizeof(why));
        dv_close(&j->s);
        finish(j, !bad, bad ? bad : "");
    } else {
        int r = dv_login(&j->s, j->host, j->port, j->user, j->pass, j->wake[0], why, sizeof(why)), n = 0;
        if (r == 0) {
            char from[24], to[24];
            snprintf(from, sizeof(from), "%.10s 00:00:00", j->day);
            snprintf(to, sizeof(to), "%.10s 23:59:59", j->day);
            r = dv_files(&j->s, from, to, &j->files, &n, CCTV_DAY_MAX, j->wake[0]);
            if (r > 0) {                            /* (a camera answers a day with nothing with a code of its own) */
                r = 0;
                n = 0;
            } else if (r < 0) {
                snprintf(why, sizeof(why), "the camera did not answer");
            }
        }
        dv_close(&j->s);
        pthread_mutex_lock(&j->lock);
        j->st.files = j->files;
        j->st.n = n;
        pthread_mutex_unlock(&j->lock);
        finish(j, r == 0, r == 0 ? "" : why);
    }
    dv_demux_free(&j->demux);
    dv_packets_free(&j->packets);
    return NULL;
}

static struct cctv_arch *start(const struct cctv_target *t, struct cctv_arch *j)
{
    if (!j)
        return NULL;
    snprintf(j->host, sizeof(j->host), "%s", t->host);
    snprintf(j->user, sizeof(j->user), "%s", t->user);
    snprintf(j->pass, sizeof(j->pass), "%s", t->password);
    j->port = t->port;
    j->video = j->sound = -1;
    dv_init(&j->s);
    if (pipe2(j->wake, O_CLOEXEC | O_NONBLOCK) < 0) {
        free(j);
        return NULL;
    }
    pthread_mutex_init(&j->lock, NULL);
    if (pthread_create(&j->thread, NULL, job_thread, j) != 0) {
        close(j->wake[0]);
        close(j->wake[1]);
        free(j);
        return NULL;
    }
    return j;
}

struct cctv_arch *cctv_list_start(const struct cctv_target *t, const char *day)
{
    struct cctv_arch *j = calloc(1, sizeof(*j));
    if (j)
        snprintf(j->day, sizeof(j->day), "%s", day);
    return start(t, j);
}

struct cctv_arch *cctv_fetch_start(const struct cctv_target *t, const struct dv_file *f, const char *dir, const char *base, const char *ffmpeg)
{
    struct cctv_arch *j = calloc(1, sizeof(*j));
    if (j) {
        j->fetch = true;
        j->file = *f;
        j->st.kb = f->kb;
        snprintf(j->dir, sizeof(j->dir), "%s", dir);
        snprintf(j->base, sizeof(j->base), "%s", base);
        snprintf(j->ffmpeg, sizeof(j->ffmpeg), "%s", ffmpeg);
    }
    return start(t, j);
}

void cctv_arch_status(struct cctv_arch *j, struct cctv_arch_status *out)
{
    pthread_mutex_lock(&j->lock);
    *out = j->st;
    pthread_mutex_unlock(&j->lock);
}

void cctv_arch_stop(struct cctv_arch *j)
{
    pthread_mutex_lock(&j->lock);
    j->stop = true;
    pthread_mutex_unlock(&j->lock);
    if (write(j->wake[1], "x", 1) < 0) {}
}

void cctv_arch_free(struct cctv_arch *j)
{
    if (!j)
        return;
    cctv_arch_stop(j);
    pthread_join(j->thread, NULL);
    close(j->wake[0]);
    close(j->wake[1]);
    pthread_mutex_destroy(&j->lock);
    free(j->files);
    free(j);
}
