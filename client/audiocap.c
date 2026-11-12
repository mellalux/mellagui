#include "audiocap.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "loop.h"

#define RING_FRAMES (AUDIOCAP_RATE * 6)         /* six seconds: the caller drains it every few milliseconds */

struct audiocap;

struct source {
    struct audiocap *owner;
    pid_t pid;
    int fd;
    pthread_t thread;
    bool thread_made;
    int16_t *ring;                              /* RING_FRAMES stereo frames */
    size_t head, count;                         /* oldest frame, frames held */
    bool dead;                                  /* the program ended and its pipe is empty */
    uint8_t half[4];                            /* a frame that arrived in two pieces */
    int half_n;
};

struct audiocap {
    struct source src[2];
    int n;
    pthread_mutex_t lock;
    uint64_t first_ms;
};

static void *reader_main(void *arg)
{
    struct source *s = arg;
    struct audiocap *a = s->owner;
    uint8_t buf[16384];
    for (;;) {
        ssize_t k = read(s->fd, buf, sizeof(buf));
        if (k < 0 && errno == EINTR)
            continue;
        pthread_mutex_lock(&a->lock);
        if (k <= 0) {
            s->dead = true;
            pthread_mutex_unlock(&a->lock);
            return NULL;
        }
        size_t at = 0;
        size_t frame_bytes = AUDIOCAP_CHANNELS * 2;
        if (!a->first_ms)
            a->first_ms = lm_now_ms() - (uint64_t)k * 1000 / (AUDIOCAP_RATE * frame_bytes);
        while (at < (size_t)k) {
            size_t need = frame_bytes - (size_t)s->half_n;
            size_t take = (size_t)k - at < need ? (size_t)k - at : need;
            memcpy(s->half + s->half_n, buf + at, take);
            s->half_n += (int)take;
            at += take;
            if (s->half_n == (int)frame_bytes) {
                size_t slot = (s->head + s->count) % RING_FRAMES;
                if (s->count == RING_FRAMES) {                  /* full: the oldest frame goes (nobody was listening) */
                    s->head = (s->head + 1) % RING_FRAMES;
                    s->count--;
                    slot = (s->head + s->count) % RING_FRAMES;
                }
                memcpy(&s->ring[slot * AUDIOCAP_CHANNELS], s->half, frame_bytes);
                s->count++;
                s->half_n = 0;
            }
        }
        pthread_mutex_unlock(&a->lock);
    }
}

bool audiocap_available(void)
{
    const char *prog = getenv("MELLAGUI_PAREC");
    if (!prog || !*prog)
        prog = "parec";
    if (strchr(prog, '/'))
        return access(prog, X_OK) == 0;
    const char *path = getenv("PATH");
    if (!path)
        path = "/usr/local/bin:/usr/bin:/bin";
    char copy[4096], full[4200];
    snprintf(copy, sizeof(copy), "%s", path);
    for (char *save = NULL, *dir = strtok_r(copy, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
        snprintf(full, sizeof(full), "%s/%s", *dir ? dir : ".", prog);
        if (access(full, X_OK) == 0)
            return true;
    }
    return false;
}

static bool start_source(struct audiocap *a, const char *spec, const char *label, char *err, size_t en)
{
    if (strncmp(spec, "pulse:", 6) != 0 || !spec[6]) {
        snprintf(err, en, "the %s source \"%s\" can only be read by ffmpeg", label, spec);
        return false;
    }
    const char *prog = getenv("MELLAGUI_PAREC");
    if (!prog || !*prog)
        prog = "parec";
    int out[2], status[2];
    if (pipe2(out, O_CLOEXEC) < 0 || pipe2(status, O_CLOEXEC) < 0) {
        snprintf(err, en, "pipe: %s", strerror(errno));
        return false;
    }
    char dev[256];
    snprintf(dev, sizeof(dev), "%s", spec + 6);
    pid_t pid = fork();
    if (pid < 0) {
        snprintf(err, en, "fork: %s", strerror(errno));
        close(out[0]); close(out[1]); close(status[0]); close(status[1]);
        return false;
    }
    if (pid == 0) {
        setpgid(0, 0);
        dup2(out[1], 1);
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) {
            dup2(nul, 0);
            dup2(nul, 2);
        }
        execlp(prog, prog, "--raw", "--format=s16le", "--rate=44100", "--channels=2", "--latency-msec=40", "--client-name=mellagui", "-d", dev, (char *)NULL);
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
        snprintf(err, en, "cannot start %s: %s (the sound needs parec, or ffmpeg)", prog, strerror(e));
        return false;
    }
    struct source *s = &a->src[a->n];
    s->ring = calloc((size_t)RING_FRAMES * AUDIOCAP_CHANNELS, sizeof(int16_t));
    if (!s->ring) {
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        close(out[0]);
        snprintf(err, en, "out of memory");
        return false;
    }
    s->owner = a;
    s->pid = pid;
    s->fd = out[0];
    a->n++;
    return true;
}

struct audiocap *audiocap_start(const char *system_src, const char *mic_src, char *err, size_t en)
{
    struct audiocap *a = calloc(1, sizeof(*a));
    if (!a) {
        snprintf(err, en, "out of memory");
        return NULL;
    }
    pthread_mutex_init(&a->lock, NULL);
    err[0] = 0;
    char e2[160] = "";
    if (system_src)
        start_source(a, system_src, "computer sound", e2, sizeof(e2));
    if (mic_src)
        start_source(a, mic_src, "microphone", e2, sizeof(e2));
    if (a->n == 0) {
        snprintf(err, en, "%s", e2[0] ? e2 : "no sound source");
        pthread_mutex_destroy(&a->lock);
        free(a);
        return NULL;
    }
    sigset_t all, old;                                  /* the reader threads do not take the process's signals (the recorder ends on SIGINT in its own thread) */
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &old);
    for (int i = 0; i < a->n; i++)
        a->src[i].thread_made = pthread_create(&a->src[i].thread, NULL, reader_main, &a->src[i]) == 0;
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    return a;
}

uint64_t audiocap_first_ms(const struct audiocap *a)
{
    return a ? a->first_ms : 0;
}

size_t audiocap_read(struct audiocap *a, int16_t *out, size_t max_frames)
{
    if (!a || !max_frames)
        return 0;
    pthread_mutex_lock(&a->lock);
    size_t n = max_frames;
    int live = 0;
    for (int i = 0; i < a->n; i++) {
        struct source *s = &a->src[i];
        if (s->dead && s->count == 0)
            continue;                                   /* gone: it does not hold the others back */
        live++;
        if (s->count < n)
            n = s->count;
    }
    if (!live)
        n = 0;
    for (size_t f = 0; f < n; f++) {
        int32_t l = 0, r = 0;
        int mixed = 0;
        for (int i = 0; i < a->n; i++) {
            struct source *s = &a->src[i];
            if (s->count == 0)
                continue;
            size_t slot = (s->head + f) % RING_FRAMES;
            l += s->ring[slot * 2];
            r += s->ring[slot * 2 + 1];
            mixed++;
        }
        if (mixed > 1) {                                /* two sources: a little lower each, so that the sum rarely clips */
            l = l * 3 / 4;
            r = r * 3 / 4;
        }
        out[f * 2] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
        out[f * 2 + 1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
    }
    for (int i = 0; i < a->n; i++) {
        struct source *s = &a->src[i];
        size_t drop = s->count < n ? s->count : n;
        s->head = (s->head + drop) % RING_FRAMES;
        s->count -= drop;
    }
    pthread_mutex_unlock(&a->lock);
    return n;
}

bool audiocap_alive(const struct audiocap *a)
{
    if (!a)
        return false;
    struct audiocap *w = (struct audiocap *)a;
    pthread_mutex_lock(&w->lock);
    bool alive = false;
    for (int i = 0; i < a->n; i++)
        if (!a->src[i].dead || a->src[i].count > 0)
            alive = true;
    pthread_mutex_unlock(&w->lock);
    return alive;
}

void audiocap_stop(struct audiocap *a)
{
    if (!a)
        return;
    for (int i = 0; i < a->n; i++)
        if (a->src[i].pid > 0)
            kill(a->src[i].pid, SIGTERM);
    for (int i = 0; i < a->n; i++) {
        struct source *s = &a->src[i];
        if (s->thread_made)
            pthread_join(s->thread, NULL);              /* parec is gone, so its pipe ends and the reader stops */
        close(s->fd);
        if (s->pid > 0)
            waitpid(s->pid, NULL, 0);
        free(s->ring);
    }
    pthread_mutex_destroy(&a->lock);
    free(a);
}
