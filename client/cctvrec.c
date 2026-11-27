#include "cctvrec.h"

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

#define PENDING_MAX (24u << 20)
#define SILENT_MS 30000
#define SOUND_WAIT_MS 1000              /* how long the first picture waits to see whether the camera sends sound */
#define FINISH_MS 20000

struct buf {
    uint8_t *p;
    size_t len, cap;
};

struct cctv_job {
    pthread_t thread;
    pthread_mutex_t lock;
    int wake[2];
    int kind;
    char name[48], host[128], user[64], pass[96], dir[400], ffmpeg[400];
    int port;
    bool stop;                          /* under the lock */
    struct cctv_status st;              /* under the lock */

    /* the thread's own */
    struct dv_session s;
    struct dv_demux demux;
    struct dv_packets packets;
    struct buf video, sound;            /* waiting for ffmpeg */
    int codec;                          /* of the first key frame, 0 until it has come */
    bool has_sound;
    int64_t key_ms, last_ms;
    pid_t pid;
    int vin, ain, out;                  /* ffmpeg's pipes (out: the snapshot's picture) */
    int file;                           /* the snapshot's file */
    char why[200];
    bool broken, fatal;
};

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void tell(struct cctv_job *j, int state, const char *msg)
{
    pthread_mutex_lock(&j->lock);
    if (state == CCTV_RECORDING && j->st.state != CCTV_RECORDING)
        j->st.since_ms = now_ms();
    j->st.state = state;
    snprintf(j->st.msg, sizeof(j->st.msg), "%s", msg);
    j->st.serial++;
    pthread_mutex_unlock(&j->lock);
}

static bool stopping(struct cctv_job *j)
{
    pthread_mutex_lock(&j->lock);
    bool s = j->stop;
    pthread_mutex_unlock(&j->lock);
    return s;
}

/* A new file of this camera, made empty so that nobody else takes the name. Returns the descriptor, the path in `path`. */
static int new_file(struct cctv_job *j, const char *ext, char *path, size_t cap)
{
    if (mkdir(j->dir, 0700) < 0 && errno != EEXIST)
        return -1;
    char name[48], stamp[32];
    snprintf(name, sizeof(name), "%s", j->name);
    for (char *p = name; *p; p++)
        if (*p == '/' || (unsigned char)*p < 0x20 || (p == name && (*p == '.' || *p == '-')))
            *p = '_';
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
    for (int n = 1; n < 100; n++) {
        char more[8] = "";
        if (n > 1)
            snprintf(more, sizeof(more), "-%d", n);
        if (snprintf(path, cap, "%s/%s-%s%s.%s", j->dir, name, stamp, more, ext) >= (int)cap) {
            errno = ENAMETOOLONG;
            return -1;
        }
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd >= 0 || errno != EEXIST)
            return fd;
    }
    return -1;
}

static bool add(struct buf *b, const void *p, size_t n)
{
    if (b->len + n > PENDING_MAX)
        return false;
    if (b->len + n > b->cap) {
        size_t want = b->cap ? b->cap : 1u << 18;
        while (want < b->len + n)
            want *= 2;
        uint8_t *q = realloc(b->p, want);
        if (!q)
            return false;
        b->p = q;
        b->cap = want;
    }
    memcpy(b->p + b->len, p, n);
    b->len += n;
    return true;
}

/* Writes what the pipe takes now. */
static void drain(struct cctv_job *j, int fd, struct buf *b)
{
    if (fd < 0 || !b->len)
        return;
    ssize_t k = write(fd, b->p, b->len);
    if (k > 0) {
        memmove(b->p, b->p + k, b->len - (size_t)k);
        b->len -= (size_t)k;
    } else if (k < 0 && errno != EAGAIN && errno != EINTR) {
        snprintf(j->why, sizeof(j->why), "ffmpeg stopped (is the disk full?)");
        j->broken = j->fatal = true;
        b->len = 0;
    }
}

static void on_frame(void *ud, const struct dv_frame *f)
{
    struct cctv_job *j = ud;
    bool key = f->kind == DV_VIDEO_KEY && (f->codec == DV_CODEC_H264 || f->codec == DV_CODEC_H265);
    if (key && !j->codec) {
        j->codec = f->codec;
        j->key_ms = now_ms();
    }
    if (!j->codec || j->broken)
        return;                             /* a file begins with a key frame */
    if (j->kind == CCTV_SNAPSHOT && !key)
        return;
    bool ok = true;
    if (f->kind == DV_VIDEO || f->kind == DV_VIDEO_KEY) {
        if (j->kind == CCTV_SNAPSHOT && j->video.len)
            return;                         /* one is enough */
        j->last_ms = now_ms();
        /* the delimiter behind the picture tells ffmpeg that it is complete (it takes the delimiters out again) */
        static const uint8_t aud264[] = { 0, 0, 0, 1, 0x09, 0xf0 }, aud265[] = { 0, 0, 0, 1, 0x46, 0x01, 0x50 };
        bool hevc = j->codec == DV_CODEC_H265;
        ok = add(&j->video, f->data, f->len) && add(&j->video, hevc ? aud265 : aud264, hevc ? sizeof(aud265) : sizeof(aud264));
    } else if (f->kind == DV_AUDIO && f->codec == DV_SOUND_ALAW && j->kind == CCTV_RECORD) {
        if (!j->pid)
            j->has_sound = true;
        if (j->has_sound)
            ok = add(&j->sound, f->data, f->len);
    }
    if (!ok) {
        snprintf(j->why, sizeof(j->why), "ffmpeg or the disk cannot keep up");
        j->broken = j->fatal = true;
    }
}

static void on_packet(void *ud, int msg, const uint8_t *body, size_t len)
{
    struct cctv_job *j = ud;
    if (msg == DV_MSG_STREAM && !dv_demux_feed(&j->demux, body, len, on_frame, j)) {
        snprintf(j->why, sizeof(j->why), "out of memory");
        j->broken = j->fatal = true;
    }
}

static bool pipes(int p[2])
{
    return pipe2(p, O_CLOEXEC) == 0;
}

/* Starts ffmpeg for what has come: a recording into a new file, or the JPEG of one picture. */
static bool encoder_start(struct cctv_job *j)
{
    bool rec = j->kind == CCTV_RECORD;
    int v[2] = { -1, -1 }, a[2] = { -1, -1 }, o[2] = { -1, -1 };
    char path[sizeof(j->st.file)];
    int fd = new_file(j, rec ? "mp4" : "jpg", path, sizeof(path));
    if (fd < 0) {
        snprintf(j->why, sizeof(j->why), "cannot make a file in %.120s: %s", j->dir, strerror(errno));
        return false;
    }
    if (!pipes(v) || (rec && j->has_sound && !pipes(a)) || (!rec && !pipes(o)))
        goto fail;
    const char *format = j->codec == DV_CODEC_H265 ? "hevc" : "h264";
    const char *argv[64];
    int n = 0;
#define ARG(x) argv[n++] = (x)
    ARG(j->ffmpeg); ARG("-hide_banner"); ARG("-loglevel"); ARG("quiet"); ARG("-flags"); ARG("low_delay"); ARG("-probesize"); ARG("32"); ARG("-analyzeduration"); ARG("0");
    if (rec) {
        ARG("-use_wallclock_as_timestamps"); ARG("1");
    }
    ARG("-f"); ARG(format); ARG("-i"); ARG("pipe:0");
    if (rec && j->has_sound) {
        ARG("-probesize"); ARG("32"); ARG("-analyzeduration"); ARG("0"); ARG("-f"); ARG("alaw"); ARG("-ar"); ARG("8000"); ARG("-ac"); ARG("1"); ARG("-i"); ARG("pipe:3");
        ARG("-map"); ARG("0:v"); ARG("-map"); ARG("1:a"); ARG("-c:a"); ARG("aac"); ARG("-b:a"); ARG("48k");
    }
    if (rec) {
        ARG("-c:v"); ARG("copy"); ARG("-bsf:v"); ARG(j->codec == DV_CODEC_H265 ? "filter_units=remove_types=35" : "filter_units=remove_types=9");
        if (j->codec == DV_CODEC_H265) {
            ARG("-tag:v"); ARG("hvc1");
        }
        /* fragments: what has been written can be played even when the program is killed */
        ARG("-movflags"); ARG("+frag_keyframe+empty_moov+default_base_moof"); ARG("-frag_duration"); ARG("2000000"); ARG("-f"); ARG("mp4"); ARG("-y"); ARG(path);
    } else {
        ARG("-frames:v"); ARG("1"); ARG("-vf"); ARG("scale=out_range=pc,format=yuvj420p"); ARG("-c:v"); ARG("mjpeg"); ARG("-q:v"); ARG("2"); ARG("-f"); ARG("image2pipe"); ARG("pipe:1");
    }
    argv[n] = NULL;
#undef ARG
    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);                      /* a Ctrl+C meant for the program must not end the file half way */
        umask(077);
        int nul = open("/dev/null", O_RDWR);
        dup2(v[0], 0);
        dup2(rec ? nul : o[1], 1);
        dup2(nul, 2);
        if (a[0] >= 0) {
            if (a[0] == 3)
                fcntl(3, F_SETFD, 0);
            else
                dup2(a[0], 3);
        }
        execv(j->ffmpeg, (char *const *)argv);
        _exit(127);
    }
    if (pid < 0)
        goto fail;
    j->pid = pid;
    j->vin = v[1];
    j->ain = a[1];
    j->out = o[0];
    close(v[0]);
    if (a[0] >= 0)
        close(a[0]);
    if (o[1] >= 0)
        close(o[1]);
    fcntl(j->vin, F_SETFL, O_NONBLOCK);
    if (j->ain >= 0)
        fcntl(j->ain, F_SETFL, O_NONBLOCK);
    if (rec) {
        close(fd);
        j->file = -1;
    } else {
        j->file = fd;
    }
    pthread_mutex_lock(&j->lock);
    snprintf(j->st.file, sizeof(j->st.file), "%s", path);
    pthread_mutex_unlock(&j->lock);
    return true;
fail:
    snprintf(j->why, sizeof(j->why), "ffmpeg cannot be started");
    for (int i = 0; i < 2; i++) {
        if (v[i] >= 0) close(v[i]);
        if (a[i] >= 0) close(a[i]);
        if (o[i] >= 0) close(o[i]);
    }
    close(fd);
    unlink(path);
    return false;
}

/* Gives ffmpeg the rest, closes its inputs, takes the snapshot's picture and waits for it to end. True when the file is good. */
static bool encoder_finish(struct cctv_job *j)
{
    if (!j->pid)
        return false;
    int64_t deadline = now_ms() + FINISH_MS;
    size_t written = 0;
    bool wrote_ok = true;
    for (;;) {
        if (j->vin >= 0 && !j->video.len) {
            close(j->vin);
            j->vin = -1;
        }
        if (j->ain >= 0 && !j->sound.len) {
            close(j->ain);
            j->ain = -1;
        }
        if (j->vin < 0 && j->ain < 0 && j->out < 0)
            break;
        struct pollfd p[3] = { { .fd = j->vin, .events = POLLOUT }, { .fd = j->ain, .events = POLLOUT }, { .fd = j->out, .events = POLLIN } };
        int64_t left = deadline - now_ms();
        if (left <= 0 || (poll(p, 3, (int)left) < 0 && errno != EINTR))
            break;
        if (p[0].revents)
            drain(j, j->vin, &j->video);
        if (p[1].revents)
            drain(j, j->ain, &j->sound);
        if (p[2].revents) {
            uint8_t b[65536];
            ssize_t k = read(j->out, b, sizeof(b));
            if (k > 0) {
                if (write(j->file, b, (size_t)k) != k)
                    wrote_ok = false;
                written += (size_t)k;
            } else if (!(k < 0 && (errno == EAGAIN || errno == EINTR))) {
                close(j->out);
                j->out = -1;
            }
        }
    }
    if (j->vin >= 0) close(j->vin);
    if (j->ain >= 0) close(j->ain);
    if (j->out >= 0) close(j->out);
    int status = 0;
    pid_t got = 0;
    while ((got = waitpid(j->pid, &status, WNOHANG)) == 0 && now_ms() < deadline)
        poll(NULL, 0, 20);
    if (got == 0) {
        kill(j->pid, SIGKILL);
        waitpid(j->pid, &status, 0);
        status = -1;
    }
    j->pid = 0;
    j->vin = j->ain = j->out = -1;
    if (j->file >= 0) {
        if (close(j->file) < 0)
            wrote_ok = false;
        j->file = -1;
    }
    pthread_mutex_lock(&j->lock);
    char path[sizeof(j->st.file)];
    snprintf(path, sizeof(path), "%s", j->st.file);
    pthread_mutex_unlock(&j->lock);
    struct stat st;
    bool exists = stat(path, &st) == 0 && st.st_size > 0;
    bool good = exists && WIFEXITED(status) && WEXITSTATUS(status) == 0 && wrote_ok && (j->kind == CCTV_RECORD || written > 0);
    pthread_mutex_lock(&j->lock);
    if (good || (j->kind == CCTV_RECORD && exists))
        j->st.files++;                  /* a recording that ended badly still holds what was written before: it stays */
    if (!exists || (!good && j->kind == CCTV_SNAPSHOT)) {
        unlink(path);
        j->st.file[0] = 0;
    }
    j->st.serial++;
    pthread_mutex_unlock(&j->lock);
    return good;
}

/* One connection: from the login to a lost connection, the wish to stop, or (a snapshot) the picture. */
static void take(struct cctv_job *j)
{
    int64_t now = now_ms(), next_alive = now + j->s.alive_s * 500;
    j->last_ms = now;
    fcntl(j->s.data, F_SETFL, O_NONBLOCK);
    fcntl(j->s.ctl, F_SETFL, O_NONBLOCK);
    uint8_t *b = malloc(65536);
    if (!b)
        return;
    while (!j->broken) {
        struct pollfd p[5] = { { .fd = j->wake[0], .events = POLLIN }, { .fd = j->s.data, .events = POLLIN }, { .fd = j->s.ctl, .events = POLLIN },
                               { .fd = j->video.len ? j->vin : -1, .events = POLLOUT }, { .fd = j->sound.len ? j->ain : -1, .events = POLLOUT } };
        if (!j->pid)
            p[3].fd = p[4].fd = -1;
        if (poll(p, 5, 200) < 0 && errno != EINTR)
            break;
        if (p[0].revents && stopping(j))
            break;
        if (p[1].revents) {
            ssize_t k = recv(j->s.data, b, 65536, 0);
            if (k > 0) {
                if (!dv_packets_feed(&j->packets, b, (size_t)k, on_packet, j)) {
                    snprintf(j->why, sizeof(j->why), "the camera sent something that is not understood");
                    break;
                }
            } else if (!(k < 0 && (errno == EAGAIN || errno == EINTR))) {
                snprintf(j->why, sizeof(j->why), "the connection was lost");
                break;
            }
        }
        if (p[2].revents) {
            ssize_t k = recv(j->s.ctl, b, 65536, 0);
            if (k == 0 || (k < 0 && errno != EAGAIN && errno != EINTR)) {
                snprintf(j->why, sizeof(j->why), "the connection was lost");
                break;
            }
        }
        now = now_ms();
        if (j->kind == CCTV_SNAPSHOT && j->video.len)
            break;
        if (j->kind == CCTV_RECORD && !j->pid && j->codec && (j->has_sound || now - j->key_ms >= SOUND_WAIT_MS)) {
            if (!encoder_start(j)) {
                j->broken = j->fatal = true;
                break;
            }
            tell(j, CCTV_RECORDING, "Recording");
        }
        if (j->pid) {
            drain(j, j->vin, &j->video);
            drain(j, j->ain, &j->sound);
        }
        if (now >= next_alive) {
            next_alive = now + j->s.alive_s * 500;
            if (dv_keepalive(&j->s) < 0) {
                snprintf(j->why, sizeof(j->why), "the connection was lost");
                break;
            }
        }
        if (now - j->last_ms > (j->kind == CCTV_SNAPSHOT ? 10000 : SILENT_MS)) {
            snprintf(j->why, sizeof(j->why), "no picture arrives");
            break;
        }
    }
    free(b);
}

static void *job_thread(void *ud)
{
    struct cctv_job *j = ud;
    int backoff = 0;
    while (!stopping(j)) {
        j->why[0] = 0;
        j->codec = 0;
        j->has_sound = j->broken = false;
        j->video.len = j->sound.len = j->demux.len = j->packets.len = 0;
        int r = dv_login(&j->s, j->host, j->port, j->user, j->pass, j->wake[0], j->why, sizeof(j->why));
        if (r == 0)
            r = dv_start(&j->s, false, j->wake[0], j->why, sizeof(j->why));
        if (r > 0)
            j->fatal = true;            /* refused: cameras lock accounts, so this is not tried again */
        if (r == 0)
            take(j);
        dv_close(&j->s);
        bool made = false;
        if (j->kind == CCTV_SNAPSHOT) {
            if (j->video.len && encoder_start(j))        /* (a picture that has come is saved also when the program is closing) */
                made = encoder_finish(j);
            if (made)
                tell(j, CCTV_DONE, "Saved");
            else
                tell(j, CCTV_FAILED, j->why[0] ? j->why : "no picture could be made");
            goto out;
        }
        if (!j->pid && j->codec && !j->fatal && stopping(j) && !encoder_start(j))
            j->fatal = true;            /* stopped within the first second: what has come is still saved */
        if (j->pid) {
            bool good = encoder_finish(j);
            if (!good && !j->fatal) {
                snprintf(j->why, sizeof(j->why), "ffmpeg could not finish the file");
                j->fatal = true;
            }
            backoff = 0;
        }
        if (j->fatal) {
            tell(j, CCTV_FAILED, j->why[0] ? j->why : "the recording failed");
            goto out;
        }
        if (stopping(j))
            break;
        backoff = backoff ? (backoff * 2 > 30000 ? 30000 : backoff * 2) : 2000;
        char m[240];
        snprintf(m, sizeof(m), "%s - trying again", j->why[0] ? j->why : "the connection failed");
        tell(j, CCTV_STARTING, m);
        struct pollfd p = { .fd = j->wake[0], .events = POLLIN };
        poll(&p, 1, backoff);
    }
    tell(j, CCTV_DONE, "Saved");
out:
    dv_demux_free(&j->demux);
    dv_packets_free(&j->packets);
    free(j->video.p);
    free(j->sound.p);
    j->video.p = j->sound.p = NULL;
    return NULL;
}

struct cctv_job *cctv_job_start(const struct cctv_target *t, int kind, const char *dir, const char *ffmpeg)
{
    struct cctv_job *j = calloc(1, sizeof(*j));
    if (!j)
        return NULL;
    j->kind = kind;
    snprintf(j->name, sizeof(j->name), "%s", t->name);
    snprintf(j->host, sizeof(j->host), "%s", t->host);
    snprintf(j->user, sizeof(j->user), "%s", t->user);
    snprintf(j->pass, sizeof(j->pass), "%s", t->password);
    j->port = t->port;
    snprintf(j->dir, sizeof(j->dir), "%s", dir);
    snprintf(j->ffmpeg, sizeof(j->ffmpeg), "%s", ffmpeg);
    j->vin = j->ain = j->out = j->file = -1;
    dv_init(&j->s);
    if (pipe2(j->wake, O_CLOEXEC | O_NONBLOCK) < 0) {
        free(j);
        return NULL;
    }
    pthread_mutex_init(&j->lock, NULL);
    snprintf(j->st.msg, sizeof(j->st.msg), "Connecting...");
    if (pthread_create(&j->thread, NULL, job_thread, j) != 0) {
        close(j->wake[0]);
        close(j->wake[1]);
        free(j);
        return NULL;
    }
    return j;
}

void cctv_job_stop(struct cctv_job *j)
{
    pthread_mutex_lock(&j->lock);
    j->stop = true;
    pthread_mutex_unlock(&j->lock);
    if (write(j->wake[1], "x", 1) < 0) {}
}

void cctv_job_status(struct cctv_job *j, struct cctv_status *out)
{
    pthread_mutex_lock(&j->lock);
    *out = j->st;
    pthread_mutex_unlock(&j->lock);
}

void cctv_job_free(struct cctv_job *j)
{
    if (!j)
        return;
    cctv_job_stop(j);
    pthread_join(j->thread, NULL);
    close(j->wake[0]);
    close(j->wake[1]);
    pthread_mutex_destroy(&j->lock);
    free(j);
}
