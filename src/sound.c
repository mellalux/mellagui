/* Feedback beeps: synthesis + a detached `paplay`/`aplay` child. See sound.h. */
#include "sound.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "audio.h"
#include "log.h"

#define RATE 44100
#define MAX_SAMPLES (RATE / 2)              /* 0.5 s at most: fits the 64 KiB pipe buffer, so write() never blocks */

struct note {
    int hz;                                 /* 0 = silence */
    int ms;
};

static const struct note k_on[] = { { 660, 70 }, { 880, 70 }, { 1175, 110 }, { 0, 0 } };
static const struct note k_off[] = { { 1175, 70 }, { 880, 70 }, { 660, 110 }, { 0, 0 } };
static const struct note k_latch[] = { { 988, 60 }, { 0, 0 } };
static const struct note k_lock[] = { { 1319, 55 }, { 0, 35 }, { 1760, 70 }, { 0, 0 } };
static const struct note k_unlock[] = { { 523, 90 }, { 0, 0 } };
static const struct note k_shot[] = { { 1568, 35 }, { 0, 20 }, { 1047, 60 }, { 0, 0 } };

static const struct {
    const char *name;
    const struct note *notes;
} k_sounds[LM_SOUND_COUNT] = {
    [LM_SOUND_STICKY_ON] = { "sticky-on", k_on },
    [LM_SOUND_STICKY_OFF] = { "sticky-off", k_off },
    [LM_SOUND_LATCH] = { "latch", k_latch },
    [LM_SOUND_LOCK] = { "lock", k_lock },
    [LM_SOUND_UNLOCK] = { "unlock", k_unlock },
    [LM_SOUND_SHOT] = { "shot", k_shot },
};

const char *lm_sound_name(enum lm_sound s)
{
    return (unsigned)s < LM_SOUND_COUNT ? k_sounds[s].name : "?";
}

/* Sine with a 5 ms linear fade in/out (no clicks), moderate volume. Returns the number of samples. */
static size_t synth(const struct note *n, int16_t *buf, size_t cap)
{
    size_t pos = 0;
    for (; n->ms; n++) {
        size_t len = (size_t)RATE * (size_t)n->ms / 1000;
        size_t fade = RATE / 200;
        if (pos + len > cap)
            len = cap - pos;
        for (size_t i = 0; i < len; i++) {
            double env = 1.0;
            if (i < fade)
                env = (double)i / (double)fade;
            else if (len - i < fade)
                env = (double)(len - i) / (double)fade;
            double v = n->hz ? sin(2.0 * M_PI * n->hz * (double)i / RATE) : 0.0;
            buf[pos + i] = (int16_t)(v * env * 7000.0);
        }
        pos += len;
    }
    return pos;
}

static char g_device[64] = "default";

void lm_sound_set_device(const char *name)
{
    if (lm_audio_name_ok(name))
        snprintf(g_device, sizeof(g_device), "%s", name);
}

void lm_sound_play(enum lm_sound s)
{
    if ((unsigned)s >= LM_SOUND_COUNT)
        return;
    static int16_t buf[MAX_SAMPLES];
    size_t n = synth(k_sounds[s].notes, buf, MAX_SAMPLES);
    if (!n)
        return;

    int fds[2];
    if (pipe2(fds, O_CLOEXEC) < 0)
        return;
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return;
    }
    if (pid == 0) {
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);      /* the loop blocks signals for signalfd */
        signal(SIGPIPE, SIG_DFL);
        setsid();
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) {
            dup2(null, 1);
            dup2(null, 2);
        }
        dup2(fds[0], 0);                            /* dup2 clears O_CLOEXEC on the new fd */
        if (strcmp(g_device, "default")) {
            execlp("aplay", "aplay", "-q", "-D", g_device, "-t", "raw", "-f", "S16_LE", "-r", "44100", "-c", "1", "-", (char *)NULL);
        } else {
            /* The sound server wakes a sleeping HDMI/DP sink itself, so a short beep is not lost; a bare ALSA
             * device swallows the first ~0.3 s. Without paplay (or a server) fall back to aplay. */
            execlp("paplay", "paplay", "--raw", "--rate=44100", "--format=s16le", "--channels=1", (char *)NULL);
            execlp("aplay", "aplay", "-q", "-t", "raw", "-f", "S16_LE", "-r", "44100", "-c", "1", "-", (char *)NULL);
        }
        _exit(127);
    }
    close(fds[0]);
    fcntl(fds[1], F_SETFL, O_NONBLOCK);
    const uint8_t *p = (const uint8_t *)buf;
    size_t left = n * sizeof(int16_t);
    while (left) {
        ssize_t k = write(fds[1], p, left);
        if (k < 0) {
            if (errno == EINTR)
                continue;
            break;                                  /* EAGAIN / EPIPE (aplay died): give up quietly */
        }
        p += k;
        left -= (size_t)k;
    }
    close(fds[1]);
    lm_debug("sound %s (aplay pid %d)", k_sounds[s].name, (int)pid);
}
