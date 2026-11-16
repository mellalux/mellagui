/* camsound - see camsound.h. */
#include "camsound.h"

#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *const k_names[CAMSOUND_COUNT] = { "tick", "tock", "shutter" };

const char *camsound_name(int kind)
{
    return kind >= 0 && kind < CAMSOUND_COUNT ? k_names[kind] : NULL;
}

static int16_t clip16(float v)
{
    v *= 32767.0f;
    return (int16_t)(v > 32767.0f ? 32767 : v < -32767.0f ? -32767 : v);
}

/* A short sine ping with a quick decay. */
static size_t ping(int16_t *buf, size_t cap, float freq, float secs, float decay, float amp)
{
    size_t n = (size_t)(secs * CAMSOUND_RATE);
    if (n > cap)
        return 0;
    for (size_t i = 0; i < n; i++) {
        float t = (float)i / CAMSOUND_RATE, attack = i < 24 ? (float)i / 24.0f : 1.0f;
        buf[i] = clip16(sinf(6.2831853f * freq * t) * expf(-t / decay) * amp * attack);
    }
    return n;
}

/* Noise burst (high-passed: the difference of neighbours) with a thump under it, starting at `at` seconds. */
static void click(int16_t *buf, size_t n, float at, float decay, float amp, float thump_hz, uint32_t *seed)
{
    float prev = 0;
    for (size_t i = (size_t)(at * CAMSOUND_RATE); i < n; i++) {
        float t = (float)i / CAMSOUND_RATE - at;
        if (t > decay * 8.0f)
            break;
        *seed = *seed * 1664525u + 1013904223u;
        float noise = (float)(int)(*seed >> 16) / 32768.0f - 1.0f, hp = noise - prev * 0.85f;
        prev = noise;
        float v = hp * expf(-t / decay) * amp * 0.5f + sinf(6.2831853f * thump_hz * t) * expf(-t / (decay * 3.0f)) * amp * 0.7f;
        buf[i] = clip16((float)buf[i] / 32767.0f + v);
    }
}

size_t camsound_render(int kind, int16_t *buf, size_t cap)
{
    switch (kind) {
    case CAMSOUND_TICK: return ping(buf, cap, 900.0f, 0.07f, 0.012f, 0.30f);
    case CAMSOUND_TOCK: return ping(buf, cap, 1500.0f, 0.12f, 0.03f, 0.34f);
    case CAMSOUND_SHUTTER: {
        size_t n = (size_t)(0.2f * CAMSOUND_RATE);
        if (n > cap)
            return 0;
        memset(buf, 0, n * sizeof(*buf));
        uint32_t seed = 12345;
        click(buf, n, 0.0f, 0.006f, 0.55f, 110.0f, &seed);                 /* the mirror */
        click(buf, n, 0.075f, 0.009f, 0.42f, 80.0f, &seed);                /* the curtain */
        return n;
    }
    }
    return 0;
}

void camsound_play(int kind)
{
    const char *name = camsound_name(kind), *log = getenv("MELLAGUI_CAM_SOUND_LOG"), *player = getenv("MELLAGUI_CAM_PLAYER");
    if (!name)
        return;
    if (log && *log) {
        FILE *f = fopen(log, "a");
        if (f) {
            fprintf(f, "%s\n", name);
            fclose(f);
        }
        return;
    }
    if (player && !strcmp(player, "none"))
        return;
    static int16_t pcm[CAMSOUND_MAX];
    size_t n = camsound_render(kind, pcm, CAMSOUND_MAX);
    if (!n)
        return;
    pid_t mid = fork();                                                  /* a child that forks the worker and goes at once: nobody has to reap anything */
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
    signal(SIGPIPE, SIG_IGN);
    int p[2];
    if (pipe(p) < 0)
        _exit(0);
    pid_t pl = fork();
    if (pl == 0) {
        dup2(p[0], 0);
        close(p[0]);
        close(p[1]);
        int nul = open("/dev/null", O_WRONLY);
        if (nul >= 0) {
            dup2(nul, 1);
            dup2(nul, 2);
        }
        if (player && *player) {
            execlp(player, player, (char *)NULL);
        } else {
            execlp("paplay", "paplay", "--raw", "--rate=44100", "--format=s16le", "--channels=1", (char *)NULL);
            execlp("aplay", "aplay", "-q", "-t", "raw", "-f", "S16_LE", "-r", "44100", "-c", "1", (char *)NULL);
        }
        _exit(127);
    }
    close(p[0]);
    if (pl > 0) {
        const char *b = (const char *)pcm;
        size_t left = n * 2;
        while (left) {
            ssize_t w = write(p[1], b, left);
            if (w <= 0)
                break;
            b += w;
            left -= (size_t)w;
        }
    }
    close(p[1]);
    if (pl > 0) {
        int st;
        waitpid(pl, &st, 0);
    }
    _exit(0);
}
