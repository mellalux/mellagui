#include "chime.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define RATE LM_CHIME_RATE
#define TAIL 0.8                        /* seconds of ring-out after the last note has ended */

enum voice { V_PLUCK, V_REED, V_PIANO };

struct tone {
    enum voice voice;
    double hz, at, len, amp;
};

#define C2 65.41
#define G2 98.00
#define A2 110.00
#define F2 87.31
#define C3 130.81
#define E3 164.81
#define F3 174.61
#define G3 196.00
#define A3 220.00
#define B3 246.94
#define C4 261.63
#define D4 293.66
#define E4 329.63
#define F4 349.23
#define G4 392.00
#define A4 440.00
#define B4 493.88
#define C5 523.25
#define D5 587.33
#define E5 659.25
#define G5 783.99
#define C6 1046.50
#define G6 1567.98

/* Part one (0 - 1.2 s): the bounce. A mandolin runs up and down the C major arpeggio, the bass goes oom, the accordion goes pah. */
static const struct tone k_tune[] = {
    /* bass: oom (C) ... oom (G) */
    { V_PLUCK, C3, 0.00, 0.30, 0.55 }, { V_PLUCK, G2, 0.30, 0.30, 0.50 }, { V_PLUCK, C3, 0.60, 0.30, 0.55 }, { V_PLUCK, G2, 0.90, 0.30, 0.50 },
    /* accordion: pah on the off beats (C major, then G7 without the root) */
    { V_REED, C4, 0.15, 0.13, 0.22 }, { V_REED, E4, 0.15, 0.13, 0.22 }, { V_REED, G4, 0.15, 0.13, 0.22 },
    { V_REED, B3, 0.45, 0.13, 0.22 }, { V_REED, D4, 0.45, 0.13, 0.22 }, { V_REED, G4, 0.45, 0.13, 0.22 },
    { V_REED, C4, 0.75, 0.13, 0.22 }, { V_REED, E4, 0.75, 0.13, 0.22 }, { V_REED, G4, 0.75, 0.13, 0.22 },
    { V_REED, C4, 1.02, 0.18, 0.24 }, { V_REED, E4, 1.02, 0.18, 0.24 }, { V_REED, G4, 1.02, 0.18, 0.24 },
    /* the mandolin: up, down, up and a cheeky high C */
    { V_PLUCK, G5, 0.00, 0.14, 0.45 }, { V_PLUCK, E5, 0.12, 0.14, 0.40 }, { V_PLUCK, C5, 0.24, 0.14, 0.40 }, { V_PLUCK, E5, 0.36, 0.14, 0.40 },
    { V_PLUCK, G5, 0.48, 0.14, 0.42 }, { V_PLUCK, C6, 0.60, 0.20, 0.50 }, { V_PLUCK, G5, 0.78, 0.14, 0.38 }, { V_PLUCK, E5, 0.90, 0.14, 0.36 },
    { V_PLUCK, G5, 1.02, 0.20, 0.40 },
    /* Part two (1.2 s on): the ballad. The piano plays Am - F - G - C, spread out, and the line falls E - D - B - C. */
    { V_PIANO, A2, 1.20, 1.00, 0.55 }, { V_PIANO, E3, 1.28, 0.90, 0.35 }, { V_PIANO, A3, 1.36, 0.80, 0.32 }, { V_PIANO, C4, 1.44, 0.70, 0.30 },
    { V_PIANO, F2, 1.70, 1.00, 0.55 }, { V_PIANO, C3, 1.78, 0.90, 0.35 }, { V_PIANO, F3, 1.86, 0.80, 0.32 }, { V_PIANO, A3, 1.94, 0.70, 0.30 },
    { V_PIANO, G2, 2.20, 1.00, 0.55 }, { V_PIANO, D4, 2.28, 0.90, 0.33 }, { V_PIANO, G3, 2.36, 0.80, 0.32 }, { V_PIANO, B3, 2.44, 0.70, 0.30 },
    { V_PIANO, C3, 2.70, 1.10, 0.60 }, { V_PIANO, G3, 2.78, 1.00, 0.36 }, { V_PIANO, E4, 2.86, 0.90, 0.34 }, { V_PIANO, B4, 2.94, 0.80, 0.28 },
    { V_PIANO, E5, 1.20, 0.80, 0.62 }, { V_PIANO, D5, 1.70, 0.80, 0.58 }, { V_PIANO, B4, 2.20, 0.80, 0.58 }, { V_PIANO, C5, 2.70, 1.10, 0.66 },
    { V_PIANO, G6, 3.10, 0.80, 0.16 },                      /* a last small sparkle */
};

static double frand_state;

static double env_ad(double t, double attack, double len, double rel)
{
    if (t < 0.0)
        return 0.0;
    double a = t < attack ? t / attack : 1.0;
    double r = t > len ? exp(-(t - len) / rel) : 1.0;
    return a * r;
}

/* One tone added into `mix` (float, mono). */
static void add_tone(float *mix, size_t n, const struct tone *t)
{
    size_t start = (size_t)(t->at * RATE);
    size_t end = (size_t)((t->at + t->len + TAIL) * RATE);
    if (end > n)
        end = n;
    for (size_t i = start; i < end; i++) {
        double x = (double)(i - start) / RATE, v = 0.0;
        switch (t->voice) {
        case V_PLUCK: {                                         /* two strings, a little apart, bright at first and then round */
            for (int s = 0; s < 2; s++) {
                double f = t->hz * (s ? 1.003 : 0.997);
                for (int h = 1; h <= 5; h++) {
                    double decay = exp(-x * (7.0 + 9.0 * h) / (1.0 + t->len * 3.0));
                    v += sin(2.0 * M_PI * f * h * x) * decay / (h * 1.1);
                }
            }
            v *= 0.5 * (x < 0.002 ? x / 0.002 : 1.0);
            break;
        }
        case V_REED: {                                          /* musette: two reeds a few cents apart, a little breath of vibrato */
            double vib = 1.0 + 0.004 * sin(2.0 * M_PI * 5.5 * x);
            for (int s = 0; s < 2; s++) {
                double f = t->hz * (s ? 1.005 : 0.995) * vib;
                for (int h = 1; h <= 7; h++)
                    v += sin(2.0 * M_PI * f * h * x) / (h * 1.3);
            }
            v *= 0.5 * env_ad(x, 0.012, t->len, 0.05);
            break;
        }
        case V_PIANO: {                                         /* a struck string: its partials die at different speeds, low notes ring longer */
            double low = t->hz < 150.0 ? 1.4 : t->hz < 400.0 ? 1.0 : 0.75;
            for (int h = 1; h <= 7; h++) {
                double tau = 1.5 * low / pow((double)h, 0.75);
                v += sin(2.0 * M_PI * t->hz * h * x * (1.0 + 0.0004 * h * h)) * exp(-x / tau) * (1.0 / pow((double)h, 1.05));
            }
            double soft = t->at > 1.1 ? 1.0 : 0.8;
            v *= 0.55 * soft * (x < 0.004 ? x / 0.004 : 1.0) * (x > t->len + 0.5 ? exp(-(x - t->len - 0.5) * 3.0) : 1.0);
            break;
        }
        }
        mix[i] += (float)(v * t->amp);
    }
}

/* A small room: four combs and two all-pass filters (Schroeder), mixed in softly so that the end does not stop dead. */
static void reverb(float *x, size_t n)
{
    static const int comb_ms[4] = { 29, 37, 41, 44 };
    float *out = calloc(n, sizeof(float));
    if (!out)
        return;
    for (int c = 0; c < 4; c++) {
        size_t d = (size_t)comb_ms[c] * RATE / 1000;
        float *buf = calloc(d, sizeof(float));
        if (!buf)
            continue;
        size_t p = 0;
        for (size_t i = 0; i < n; i++) {
            float y = x[i] + 0.72f * buf[p];
            buf[p] = y;
            p = (p + 1) % d;
            out[i] += y * 0.25f;
        }
        free(buf);
    }
    for (int a = 0; a < 2; a++) {
        size_t d = (size_t)(a ? 5 : 12) * RATE / 1000;
        float *buf = calloc(d, sizeof(float));
        if (!buf)
            continue;
        size_t p = 0;
        for (size_t i = 0; i < n; i++) {
            float in = out[i], delayed = buf[p];
            float y = -0.6f * in + delayed;
            buf[p] = in + 0.6f * y;
            p = (p + 1) % d;
            out[i] = y;
        }
        free(buf);
    }
    for (size_t i = 0; i < n; i++)
        x[i] = x[i] * 0.82f + out[i] * 0.30f;
    free(out);
}

size_t lm_chime_render(int16_t *out, size_t cap)
{
    double last = 0.0;
    for (size_t i = 0; i < sizeof(k_tune) / sizeof(k_tune[0]); i++)
        last = fmax(last, k_tune[i].at + k_tune[i].len);
    size_t n = (size_t)((last + TAIL) * RATE);
    if (n > cap || n > LM_CHIME_MAX_SAMPLES)
        return 0;
    float *mix = calloc(n, sizeof(float));
    if (!mix)
        return 0;
    (void)frand_state;
    for (size_t i = 0; i < sizeof(k_tune) / sizeof(k_tune[0]); i++)
        add_tone(mix, n, &k_tune[i]);
    reverb(mix, n);
    float peak = 0.0f;
    for (size_t i = 0; i < n; i++)
        peak = fmaxf(peak, fabsf(mix[i]));
    float gain = peak > 0.0f ? 0.5f * 32767.0f / peak : 0.0f;          /* peak at half scale: a start-up sound is a greeting, not an alarm */
    size_t fade = RATE / 5;                                            /* the last 0.2 s fade out whatever is left */
    for (size_t i = 0; i < n; i++) {
        float v = mix[i] * gain;
        if (i + fade > n)
            v *= (float)(n - i) / (float)fade;
        out[i] = (int16_t)lrintf(v);
    }
    free(mix);
    return n;
}
