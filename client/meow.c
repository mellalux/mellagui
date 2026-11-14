/* meow: a cat's meow made in code. See meow.h. */
#include "meow.h"

#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static uint32_t hash(uint32_t a)
{
    a ^= a >> 16;
    a *= 0x7feb352du;
    a ^= a >> 15;
    a *= 0x846ca68bu;
    a ^= a >> 16;
    return a;
}

static float rnd(uint32_t a) { return (float)(hash(a) & 0xffffu) / 65535.0f; }
static float lerp(float a, float b, float t) { return a + (b - a) * t; }
static float sstep(float a, float b, float v) { float t = (v - a) / (b - a); t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t; return t * t * (3.0f - 2.0f * t); }

/* A piecewise linear curve through (u, value) points, u in 0..1 */
static float curve(const float (*pt)[2], int n, float u)
{
    if (u <= pt[0][0])
        return pt[0][1];
    for (int i = 1; i < n; i++)
        if (u <= pt[i][0])
            return lerp(pt[i - 1][1], pt[i][1], (u - pt[i - 1][0]) / (pt[i][0] - pt[i - 1][0]));
    return pt[n - 1][1];
}

struct reson { float a, b, c, y1, y2; };

static void reson_set(struct reson *r, float f, float bw)
{
    float cc = -expf(-2.0f * (float)M_PI * bw / LM_MEOW_RATE), bb = 2.0f * expf(-(float)M_PI * bw / LM_MEOW_RATE) * cosf(2.0f * (float)M_PI * f / LM_MEOW_RATE);
    r->c = cc;
    r->b = bb;
    r->a = 1.0f - bb - cc;
}

static float reson_run(struct reson *r, float x)
{
    float y = r->a * x + r->b * r->y1 + r->c * r->y2;
    r->y2 = r->y1;
    r->y1 = y;
    return y;
}

size_t lm_meow_render(int16_t *buf, size_t cap, uint32_t seed)
{
    float dur = 0.62f + 0.22f * rnd(seed + 1u), pitch = 0.88f + 0.28f * rnd(seed + 2u), peak = 1.0f + 0.25f * rnd(seed + 3u);
    size_t n = (size_t)(dur * LM_MEOW_RATE);
    if (n > cap)
        n = cap;
    if (n > LM_MEOW_MAX)
        n = LM_MEOW_MAX;
    float *y = malloc((n ? n : 1) * sizeof(float));
    if (!y)
        return 0;
    /* the pitch: low at the start, up to a peak about a third in ("mee"), falling away through "aow" */
    static const float f0c[][2] = { { 0.0f, 360.0f }, { 0.12f, 400.0f }, { 0.34f, 640.0f }, { 0.62f, 520.0f }, { 1.0f, 330.0f } };
    /* the formants: m (nasal) - ee - a - oo */
    static const float f1c[][2] = { { 0.0f, 280.0f }, { 0.12f, 300.0f }, { 0.30f, 430.0f }, { 0.55f, 880.0f }, { 0.80f, 700.0f }, { 1.0f, 450.0f } };
    static const float f2c[][2] = { { 0.0f, 1100.0f }, { 0.12f, 1300.0f }, { 0.30f, 2350.0f }, { 0.55f, 1500.0f }, { 0.80f, 1100.0f }, { 1.0f, 850.0f } };
    static const float amp[][2] = { { 0.0f, 0.0f }, { 0.04f, 0.25f }, { 0.14f, 0.45f }, { 0.30f, 0.95f }, { 0.60f, 1.0f }, { 0.85f, 0.55f }, { 1.0f, 0.0f } };
    struct reson r1 = { 0 }, r2 = { 0 }, r3 = { 0 }, lp = { 0 };
    reson_set(&lp, 2600.0f, 1800.0f);
    reson_set(&r3, 3100.0f, 300.0f);
    float phase = 0.0f, noise_lp = 0.0f, sum2 = 1e-6f;
    uint32_t ns = seed * 2654435761u + 17u;
    for (size_t i = 0; i < n; i++) {
        float u = (float)i / (float)n, t = (float)i / LM_MEOW_RATE;
        if ((i & 31u) == 0u) {                                           /* the resonators follow the curves every 32 samples */
            reson_set(&r1, curve(f1c, 6, u), 90.0f);
            reson_set(&r2, curve(f2c, 6, u), 140.0f);
        }
        float f0 = curve(f0c, 5, u) * pitch * (1.0f + 0.012f * sinf(2.0f * (float)M_PI * 5.5f * t) + 0.004f * (rnd((uint32_t)i / 220u + seed) - 0.5f));
        f0 *= lerp(1.0f, peak, sstep(0.1f, 0.34f, u) * (1.0f - sstep(0.34f, 0.7f, u)));      /* (this cat's peak: how high the "mee" goes) */
        phase += f0 / LM_MEOW_RATE;
        if (phase >= 1.0f)
            phase -= 1.0f;
        float g = phase < 0.08f ? phase / 0.08f : 1.0f - (phase - 0.08f) / 0.92f;          /* the glottal pulse: a sharp rise, a soft fall */
        g = 2.0f * g - 1.0f;
        ns = ns * 1664525u + 1013904223u;
        noise_lp += 0.3f * ((float)(ns >> 16) / 32768.0f - 1.0f - noise_lp);
        float src = g + (0.10f + 0.18f * sstep(0.4f, 0.8f, u)) * noise_lp * 2.0f;           /* breath: more in the "aow" */
        float v = reson_run(&r1, src);
        v = reson_run(&r2, v * 0.6f + src * 0.05f);
        v = reson_run(&r3, v);
        v = reson_run(&lp, v);
        float a = curve(amp, 7, u);
        y[i] = v * a * a * (3.0f - 2.0f * a);
        sum2 += y[i] * y[i];
    }
    float gain = 2300.0f / sqrtf(sum2 / (float)(n ? n : 1));              /* by the loudness (rms), so one strong resonance does not set the level; it plays under the system's volume */
    for (size_t i = 0; i < n; i++) {
        float f = y[i] * gain, e = i < 220 ? (float)i / 220.0f : 1.0f;
        if (n - i < 440)
            e *= (float)(n - i) / 440.0f;
        buf[i] = (int16_t)(12000.0f * tanhf(f * e / 12000.0f));          /* (a soft limit for the peaks) */
    }
    free(y);
    return n;
}

int lm_meow_write_wav(const char *path, uint32_t seed)
{
    static int16_t pcm[LM_MEOW_MAX];
    size_t n = lm_meow_render(pcm, LM_MEOW_MAX, seed);
    FILE *f = n ? fopen(path, "wb") : NULL;
    if (!f)
        return 0;
    uint32_t bytes = (uint32_t)(n * 2), rate = LM_MEOW_RATE, brate = LM_MEOW_RATE * 2, riff = 36 + bytes, fmt = 16;
    uint16_t pcmfmt = 1, ch = 1, align = 2, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmt, 4, 1, f);
    fwrite(&pcmfmt, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f); fwrite(&brate, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f);
    int ok = fwrite(pcm, 2, n, f) == n;
    return fclose(f) == 0 && ok;
}

void lm_meow_play(uint32_t seed)
{
    const char *wav = getenv("MELLAGUI_MEOW_WAV"), *player = getenv("MELLAGUI_MEOW_PLAYER");
    if (wav && *wav) {
        lm_meow_write_wav(wav, seed);
        return;
    }
    if (player && !strcmp(player, "none"))
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
    static int16_t pcm[LM_MEOW_MAX];
    size_t n = lm_meow_render(pcm, LM_MEOW_MAX, seed);
    int p[2];
    if (!n || pipe(p) < 0)
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
