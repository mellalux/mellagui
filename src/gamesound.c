/* The block game's sounds. See gamesound.h. Every sound is added up in g_acc[] from a few kinds of parts, then scaled so that its loudest sample is the level
 * of its table entry. */
#include "gamesound.h"

#include <math.h>
#include <string.h>

#define RATE 44100
#define MAX_LEN (RATE * 2)

static double g_acc[MAX_LEN];
static size_t g_end;                            /* one past the last sample any part wrote */
static uint32_t g_seed;

static uint32_t next(void)
{
    uint32_t x = g_seed;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_seed = x;
    return x;
}

static double noise(void) { return (double)next() / 2147483648.0 - 1.0; }
static double between(double a, double b) { return a + (b - a) * ((double)next() / 4294967296.0); }

static size_t samples(double ms) { return (size_t)(ms * RATE / 1000.0); }

/* where a part of `ms` that starts at `at_ms` lies in the picture: its first sample and how many fit */
static size_t part(double at_ms, double ms, size_t *at)
{
    *at = samples(at_ms);
    size_t len = samples(ms);
    if (*at >= MAX_LEN)
        return 0;
    if (*at + len > MAX_LEN)
        len = MAX_LEN - *at;
    if (*at + len > g_end)
        g_end = *at + len;
    return len;
}

/* the usual envelope: up in `attack` ms, then dying away with the time constant `tau` ms, and to nothing in the last 4 ms */
static double envelope(size_t i, size_t len, double attack, double tau)
{
    double t = (double)i * 1000.0 / RATE, tail = (double)(len - i) * 1000.0 / RATE;
    double e = t < attack ? t / attack : exp(-(t - attack) / tau);
    return tail < 4.0 ? e * tail / 4.0 : e;
}

/* noise through a band-pass filter whose middle slides from f0 to f1 Hz (f at most 7000, q at least 0.7) */
static void burst(double at_ms, double ms, double f0, double f1, double q, double attack, double tau, double gain)
{
    size_t at, len = part(at_ms, ms, &at);
    double low = 0, band = 0, damp = 1.0 / q;
    for (size_t i = 0; i < len; i++) {
        double f = 2.0 * sin(M_PI * f0 * pow(f1 / f0, (double)i / (double)len) / RATE);
        low += f * band;
        double high = noise() - low - damp * band;
        band += f * high;
        g_acc[at + i] += band * damp * gain * envelope(i, len, attack, tau);
    }
}

/* a tone that glides from f0 to f1 Hz, with a second and a third partial of p2 and p3 */
static void tone(double at_ms, double ms, double f0, double f1, double attack, double tau, double gain, double p2, double p3)
{
    size_t at, len = part(at_ms, ms, &at);
    double ph = 0;
    for (size_t i = 0; i < len; i++) {
        ph += 2.0 * M_PI * f0 * pow(f1 / f0, (double)i / (double)len) / RATE;
        g_acc[at + i] += (sin(ph) + p2 * sin(2.0 * ph) + p3 * sin(3.0 * ph)) * gain * envelope(i, len, attack, tau);
    }
}

/* a struck thing that rings: one partial */
static void ring(double at_ms, double hz, double tau, double gain)
{
    tone(at_ms, tau * 4.0, hz, hz * 0.997, 0.6, tau, gain, 0.0, 0.0);
}

struct resonator { double a1, a2, b0, y1, y2; };

static void res_set(struct resonator *r, double hz, double width)
{
    double rad = exp(-M_PI * width / RATE);
    r->a1 = 2.0 * rad * cos(2.0 * M_PI * hz / RATE);
    r->a2 = -rad * rad;
    r->b0 = 1.0 - rad;
}

static double res_run(struct resonator *r, double x)
{
    double y = r->b0 * x + r->a1 * r->y1 + r->a2 * r->y2;
    r->y2 = r->y1;
    r->y1 = y;
    return y;
}

/* A creature's voice: a buzzing source (and some breath) through two formants. The pitch goes p0 -> p1 (a third of the way) -> p2, the formants slide from the
 * first value to the second; `rough` 0..1 breaks it up `rough_hz` times a second (a growl). */
struct voice {
    double ms, p0, p1, p2;
    double a0, a1, b0, b1;                      /* the first and the second formant, start and end */
    double rough, rough_hz, breath, gain;
};

static void voice(double at_ms, const struct voice *v)
{
    size_t at, len = part(at_ms, v->ms, &at);
    struct resonator f1 = { 0 }, f2 = { 0 };
    double ph = 0, jitter = 0;
    for (size_t i = 0; i < len; i++) {
        double u = (double)i / (double)len, t = (double)i / RATE;
        if (!(i & 31)) {
            res_set(&f1, v->a0 + (v->a1 - v->a0) * u, 120.0);
            res_set(&f2, v->b0 + (v->b1 - v->b0) * u, 190.0);
        }
        double pitch = u < 0.33 ? v->p0 + (v->p1 - v->p0) * (u / 0.33) : v->p1 + (v->p2 - v->p1) * ((u - 0.33) / 0.67);
        jitter += 0.02 * (noise() - jitter);
        ph += pitch * (1.0 + 0.25 * jitter) / RATE;
        ph -= floor(ph);
        double src = (ph < 0.6 ? ph / 0.6 : (1.0 - ph) / 0.4) * 2.0 - 1.0 + (ph < 0.08 ? 0.8 : 0.0) + v->breath * noise();
        double am = 1.0 - v->rough * (0.5 + 0.5 * sin(2.0 * M_PI * v->rough_hz * t + 6.0 * jitter));
        double y = res_run(&f1, src * am) * 1.0 + res_run(&f2, src * am) * 0.7;
        double up = u < 0.07 ? u / 0.07 : 1.0, down = u > 0.65 ? (1.0 - u) / 0.35 : 1.0;
        g_acc[at + i] += y * up * down * v->gain * 4.0;
    }
}

/* a horn: a stack of harmonics that grows brighter as it is blown */
static void horn(double at_ms, double ms, double hz, double gain)
{
    size_t at, len = part(at_ms, ms, &at);
    double ph = 0;
    for (size_t i = 0; i < len; i++) {
        double u = (double)i / (double)len, t = (double)i / RATE;
        double up = t < 0.09 ? t / 0.09 : 1.0, down = u > 0.78 ? (1.0 - u) / 0.22 : 1.0;
        double bright = 0.45 + 0.4 * up * down;
        double vib = 1.0 + 0.006 * sin(2.0 * M_PI * 5.2 * t) * (u > 0.3 ? 1.0 : u / 0.3);
        ph += 2.0 * M_PI * hz * (t < 0.09 ? 0.94 + 0.06 * up : 1.0) * vib / RATE;
        double s = 0, a = 1.0;
        for (int k = 1; k <= 9; k++, a *= bright)
            s += a * sin(k * ph);
        g_acc[at + i] += s * up * down * gain * 0.5;
    }
}

/* ---- the sounds; v = the variant ---------------------------------------------------------------------------------------------- */

static void s_break_stone(int v)
{
    burst(0, 50, 3200 - 450 * v, 1700, 0.9, 0.5, 10, 1.6);
    tone(0, 90, 190 - 25 * v, 85, 0.8, 22, 0.55, 0.2, 0);
    for (int i = 0; i < 5 + v; i++) {                       /* the pieces fall */
        double at = between(28, 190);
        burst(at, 28, between(1100, 3600), between(900, 2000), 1.6, 0.5, 6, 0.9 * (1.0 - at / 260.0));
    }
}

static void s_break_wood(int v)
{
    tone(0, 120, 270 + 45 * v, 185 + 20 * v, 0.6, 24, 0.8, 0.45, 0.15);
    ring(2, 540 + 70 * v, 20, 0.35);
    burst(0, 30, 2100, 1500, 1.0, 0.4, 6, 0.9);
    for (int i = 0; i < 4; i++)                             /* splinters */
        burst(between(18, 130), 18, between(1400, 2900), 1200, 2.2, 0.4, 4, between(0.4, 0.9));
}

static void s_break_dirt(int v)
{
    burst(0, 170, 950 - 130 * v, 330, 0.8, 5, 40, 1.4);
    burst(8, 110, 2300 + 200 * v, 1200, 0.7, 3, 22, 0.45);
    tone(0, 80, 115 - 10 * v, 58, 1.0, 22, 0.5, 0, 0);
    for (int i = 0; i < 4; i++)
        burst(between(25, 120), 20, between(600, 1800), 500, 1.8, 0.5, 5, 0.4);
}

static void s_break_glass(int v)
{
    burst(0, 80, 6600 - 600 * v, 4300, 0.8, 0.3, 15, 1.1);
    for (int i = 0; i < 9; i++)                             /* the splinters ring */
        ring(i < 3 ? between(0, 8) : between(15, 110), between(2300, 6800), between(18, 70), between(0.12, 0.32));
}

static void s_break_leaves(int v)
{
    for (int i = 0; i < 9; i++) {
        double at = i * 19.0 + between(0, 14);
        burst(at, 50, between(2400, 5200) + 300 * v, between(1800, 4000), 0.8, 8, 14, between(0.4, 1.0) * (1.0 - at / 280.0));
    }
}

static void s_dig_stone(int v)
{
    burst(0, 40, 2500 + 350 * v, 1600, 1.4, 0.4, 7, 1.4);
    tone(0, 45, 175 + 15 * v, 120, 0.5, 11, 0.45, 0, 0);
}

static void s_dig_wood(int v)
{
    tone(0, 65, 310 + 35 * v, 250, 0.5, 15, 0.8, 0.35, 0);
    burst(0, 20, 1700, 1300, 1.2, 0.3, 4, 0.6);
}

static void s_dig_dirt(int v)
{
    burst(0, 80, 850 + 120 * v, 380, 0.8, 3, 18, 1.3);
    tone(0, 40, 105, 70, 0.8, 12, 0.35, 0, 0);
}

static void s_place_stone(int v)
{
    tone(0, 110, 150 + 18 * v, 80, 0.6, 26, 1.0, 0.25, 0);
    burst(0, 40, 1600 + 250 * v, 900, 1.0, 0.4, 8, 0.8);
    burst(34, 30, 2400, 1600, 1.4, 0.4, 5, 0.25);
}

static void s_place_wood(int v)
{
    tone(0, 130, 230 + 30 * v, 175 + 15 * v, 0.6, 30, 1.0, 0.35, 0.1);
    burst(0, 35, 950, 700, 1.0, 0.4, 8, 0.6);
}

static void s_place_dirt(int v)
{
    burst(0, 130, 620 + 90 * v, 280, 0.8, 4, 30, 1.4);
    tone(0, 90, 100 - 8 * v, 62, 1.0, 24, 0.65, 0, 0);
}

static void s_place_glass(int v)
{
    ring(0, 1900 + 240 * v, 45, 0.5);
    ring(1, 3150 + 300 * v, 30, 0.3);
    ring(2, 4620, 18, 0.15);
    tone(0, 50, 210, 150, 0.5, 12, 0.35, 0, 0);
}

static void s_splash(int v)
{
    burst(0, 330, 650 + 150 * v, 2600, 0.8, 18, 85, 1.0);
    burst(12, 180, 3800, 1800, 0.7, 8, 45, 0.35);
    for (int i = 0; i < 6; i++) {                           /* drops */
        double hz = between(480, 1100);
        tone(between(60, 300), 45, hz, hz * 1.9, 3, 13, between(0.12, 0.28), 0, 0);
    }
}

static void s_hit(int v)
{
    burst(0, 100, 650, 3400 + 300 * v, 1.3, 55, 30, 0.9);   /* the blade through the air ... */
    tone(66, 110, 165 - 14 * v, 68, 0.5, 26, 0.95, 0.2, 0);     /* ... and into the goblin */
    burst(66, 45, 2300, 1100, 0.9, 0.4, 9, 0.85);
    ring(68, 2350 + 180 * v, 28, 0.16);
    ring(68, 3720 + 120 * v, 20, 0.1);
    struct voice yelp = { 150, 380 + 40 * v, 600 + 50 * v, 340, 900, 700, 2200, 1700, 0.25, 70, 0.25, 0.16 };
    voice(88, &yelp);
}

static void s_goblin_die(int v)
{
    struct voice cry = { 560, 470 + 60 * v, 610 + 60 * v, 120, 1050, 480, 2300, 1100, 0.55, 28 + 9 * v, 0.35, 0.4 };
    voice(0, &cry);
    tone(470, 140, 105, 48, 1.0, 38, 0.55, 0, 0);           /* it falls */
    burst(470, 110, 700, 250, 0.8, 2, 28, 0.5);
}

static void s_goblin_near(int v)
{
    if (v == 0) {                                           /* "heh heh heh heh" */
        for (int i = 0; i < 4; i++) {
            struct voice heh = { 95, 300 - 16 * i, 340 - 18 * i, 250 - 14 * i, 760, 640, 1900, 1750, 0.2, 60, 0.6, 1.0 - 0.12 * i };
            voice(i * 128.0, &heh);
        }
    } else if (v == 1) {                                    /* two grunts */
        for (int i = 0; i < 2; i++) {
            struct voice grunt = { 170, 150, 185 - 20 * i, 115, 560, 480, 1250, 1000, 0.55, 42, 0.45, 1.0 - 0.2 * i };
            voice(i * 230.0, &grunt);
        }
    } else {                                                /* a snarl */
        struct voice snarl = { 520, 190, 260, 170, 700, 950, 1700, 2100, 0.8, 33, 0.7, 1.0 };
        voice(0, &snarl);
    }
}

static void s_brute(int v)
{
    struct voice roar = { 920, 78 + 8 * v, 112 + 10 * v, 62, 430, 330, 960, 760, 0.7, 31 + 5 * v, 0.55, 1.0 };
    voice(0, &roar);
    burst(0, 800, 170, 110, 1.2, 120, 380, 0.7);
}

static void s_hurt(int v)
{
    struct voice uh = { 190, 185 + 22 * v, 200 + 22 * v, 122 + 10 * v, 660, 540, 1150, 950, 0.15, 40, 0.3, 0.4 };
    voice(8, &uh);
    tone(0, 90, 120, 55, 0.6, 22, 0.8, 0, 0);
    burst(0, 40, 1300, 600, 0.8, 0.4, 9, 0.45);
}

static void s_die(int v)
{
    (void)v;
    struct voice groan = { 800, 205, 175, 78, 640, 380, 1120, 800, 0.35, 19, 0.35, 0.3 };
    voice(0, &groan);
    tone(180, 900, 220, 55, 40, 340, 0.45, 0.4, 0.15);
    tone(760, 200, 90, 42, 1.0, 55, 0.7, 0, 0);
}

static void s_arrow(int v)
{
    tone(0, 150, 175 + 25 * v, 165 + 25 * v, 0.5, 34, 0.7, 0.7, 0.45);      /* the string */
    burst(12, 230, 5200, 2100 - 200 * v, 4.0, 40, 70, 1.3);                 /* the flight */
    tone(230, 70, 220 + 20 * v, 130, 0.5, 16, 0.5, 0.2, 0);                 /* the arrow lands */
    burst(230, 25, 1500, 900, 1.0, 0.3, 5, 0.5);
}

static void s_clash(int v)
{
    burst(0, 30, 4800, 3200, 1.0, 0.3, 6, 0.9);
    ring(0, 1750 + 230 * v, 55, 0.35);
    ring(0, 2870 + 310 * v, 40, 0.25);
    ring(1, 4390 + 190 * v, 25, 0.18);
}

static void s_wall_hit(int v)
{
    tone(0, 160, 95 + 9 * v, 52, 0.8, 42, 1.0, 0.3, 0);
    burst(0, 90, 720 + 100 * v, 300, 0.9, 1, 22, 0.9);
    for (int i = 0; i < 3; i++)                             /* what is loose rattles */
        burst(between(40, 150), 22, between(1200, 2600), 900, 2.0, 0.4, 5, 0.25);
}

static void s_wall_break(int v)
{
    burst(0, 650, 1500 + 200 * v, 220, 0.7, 3, 150, 1.3);
    tone(0, 300, 110, 42, 1.0, 80, 0.9, 0.3, 0);
    for (int i = 0; i < 16; i++) {
        double at = between(20, 560);
        double hz = between(500, 3000);
        burst(at, 40, hz, hz * 0.6, 1.6, 0.4, 9, between(0.5, 1.1) * (1.0 - at / 750.0));
        if (!(i & 3))
            tone(at, 80, between(150, 300), 90, 0.5, 18, 0.35 * (1.0 - at / 750.0), 0.3, 0);
    }
}

static void s_horn(int v)                                   /* the war horn of a wave: low, then up a fourth */
{
    (void)v;
    horn(0, 620, 146.8, 0.7);
    horn(0, 620, 110.0, 0.35);
    horn(590, 1150, 196.0, 0.75);
    horn(590, 1150, 130.8, 0.35);
}

static void s_alarm(int v)                                  /* the keep's bell, three times */
{
    (void)v;
    for (int i = 0; i < 3; i++) {
        double at = i * 270.0;
        ring(at, 660, 170, 0.5);
        ring(at, 1327, 110, 0.3);
        ring(at, 1822, 80, 0.22);
        ring(at, 3540, 35, 0.12);
        burst(at, 14, 3000, 2200, 1.0, 0.3, 3, 0.3);
    }
}

static void s_keep_fall(int v)
{
    (void)v;
    burst(0, 1900, 190, 70, 0.9, 60, 750, 1.6);             /* the rumble */
    for (int i = 0; i < 22; i++) {
        double at = between(0, 1300), hz = between(350, 2200);
        burst(at, 70, hz, hz * 0.5, 1.3, 0.5, 16, between(0.4, 1.0) * (1.0 - at / 1800.0));
    }
    tone(0, 400, 120, 45, 2, 120, 0.8, 0.3, 0);
    tone(620, 500, 90, 35, 2, 150, 0.8, 0.3, 0);
    horn(500, 1350, 87.3, 0.22);
}

static void s_dawn(int v)                                   /* the night is over: C E G, and the C above, held */
{
    (void)v;
    static const double hz[4] = { 523.25, 659.25, 783.99, 1046.5 };
    for (int i = 0; i < 4; i++) {
        double at = i * 130.0, ms = i == 3 ? 900 : 380;
        tone(at, ms, hz[i], hz[i], 4, i == 3 ? 300 : 120, 0.5, 0.3, 0.08);
        tone(at, ms, hz[i] / 2, hz[i] / 2, 20, i == 3 ? 320 : 130, 0.2, 0, 0);
    }
}

static void s_built(int v)                                  /* two blows of the hammer and a ding */
{
    (void)v;
    for (int i = 0; i < 2; i++) {
        double at = i * 150.0;
        tone(at, 90, 330, 270, 0.4, 20, 0.7, 0.35, 0);
        burst(at, 22, 2300, 1700, 1.3, 0.3, 4, 0.8);
    }
    tone(320, 420, 783.99, 783.99, 3, 130, 0.4, 0.25, 0);
    tone(430, 480, 1174.7, 1174.7, 3, 150, 0.4, 0.2, 0);
}

static void s_join(int v)                                   /* a unit reports for duty: two short horn notes */
{
    (void)v;
    horn(0, 150, 392.0, 0.5);
    horn(140, 330, 523.25, 0.55);
}

static void s_lost(int v)                                   /* one of yours is gone: two low notes down */
{
    (void)v;
    tone(0, 280, 246.9, 240, 8, 130, 0.6, 0.4, 0.2);
    tone(220, 520, 174.6, 160, 8, 200, 0.7, 0.4, 0.2);
}

static void s_order(int v)                                  /* "yes": a small double tick */
{
    tone(0, 55, 640 + 90 * v, 660 + 90 * v, 1.5, 16, 0.6, 0.2, 0);
    tone(62, 80, 900 + 90 * v, 920 + 90 * v, 1.5, 22, 0.6, 0.2, 0);
}

static void s_no(int v)                                     /* "no": a low buzz, twice */
{
    (void)v;
    tone(0, 85, 175, 170, 3, 90, 0.6, 0, 0.5);
    tone(105, 150, 138, 128, 3, 90, 0.7, 0, 0.5);
}

static void s_hammer(int v)                                 /* a builder puts a block */
{
    tone(0, 70, 320 + 60 * v, 260 + 45 * v, 0.4, 15, 0.8, 0.3, 0);
    burst(0, 20, 2200 + 300 * v, 1600, 1.3, 0.3, 4, 0.8);
    ring(1, 1250 + 160 * v, 12, 0.15);
}

/* ---- the table --------------------------------------------------------------------------------------------------------------- */

struct entry {
    const char *name;
    void (*make)(int v);
    int v;
    int peak;                                   /* the loudest sample: how loud it is beside the others */
};

#define V1(n, f, p) { "game-" n, f, 0, p }
#define V2(n, f, p) V1(n, f, p), { "game-" n "2", f, 1, p }
#define V3(n, f, p) V2(n, f, p), { "game-" n "3", f, 2, p }

static const struct entry k_entry[] = {
    V3("break-stone", s_break_stone, 11500), V3("break-wood", s_break_wood, 11500), V3("break-dirt", s_break_dirt, 10500),
    V2("break-glass", s_break_glass, 9500), V2("break-leaves", s_break_leaves, 6500),
    V3("dig-stone", s_dig_stone, 6000), V3("dig-wood", s_dig_wood, 6000), V3("dig-dirt", s_dig_dirt, 5500),
    V2("place-stone", s_place_stone, 10000), V2("place-wood", s_place_wood, 9500), V2("place-dirt", s_place_dirt, 9000),
    V2("place-glass", s_place_glass, 7500), V2("splash", s_splash, 8500),
    V3("hit", s_hit, 13000), V2("goblin-die", s_goblin_die, 11000), V3("goblin-near", s_goblin_near, 7500), V2("brute", s_brute, 13500),
    V3("hurt", s_hurt, 12000), V1("die", s_die, 12000), V2("arrow", s_arrow, 8000), V2("clash", s_clash, 6500),
    V3("wall-hit", s_wall_hit, 10000), V2("wall-break", s_wall_break, 14000),
    V1("horn", s_horn, 12500), V1("alarm", s_alarm, 10000), V1("keep-fall", s_keep_fall, 15000), V1("dawn", s_dawn, 9500),
    V1("built", s_built, 9000), V1("join", s_join, 8500), V1("lost", s_lost, 8500), V2("order", s_order, 4500), V1("no", s_no, 6500),
    V3("hammer", s_hammer, 6000),
};

#define N_ENTRY ((int)(sizeof(k_entry) / sizeof(k_entry[0])))

int lm_gamesound_count(void) { return N_ENTRY; }

const char *lm_gamesound_name(int i)
{
    return i >= 0 && i < N_ENTRY ? k_entry[i].name : NULL;
}

int lm_gamesound_pick(const char *what, uint32_t seed)
{
    static int last[N_ENTRY];                   /* per family (the index of its first entry): the variant given the time before */
    static uint32_t rnd = 0x9e3779b9u;
    size_t wl = strlen(what);
    for (int i = 0; i < N_ENTRY;) {
        int n = 1;
        while (i + n < N_ENTRY && k_entry[i + n].v)
            n++;
        const char *fam = k_entry[i].name + 5;          /* (past "game-") */
        size_t fl = strlen(fam);
        if (!strcmp(what, fam)) {
            rnd ^= rnd << 13;
            rnd ^= rnd >> 17;
            rnd ^= rnd << 5;
            rnd += seed;
            int v = n > 1 ? (int)(rnd % (uint32_t)n) : 0;
            if (n > 1 && v == last[i])
                v = (v + 1 + (int)((rnd >> 8) % (uint32_t)(n - 1))) % n;
            last[i] = v;
            return i + v;
        }
        if (wl == fl + 1 && !strncmp(what, fam, fl) && what[fl] >= '1' && what[fl] - '1' < n) {
            last[i] = what[fl] - '1';
            return i + last[i];
        }
        i += n;
    }
    return -1;
}

size_t lm_gamesound_render(int i, int16_t *buf, size_t cap)
{
    if (i < 0 || i >= N_ENTRY)
        return 0;
    memset(g_acc, 0, sizeof(g_acc));
    g_end = 0;
    g_seed = 0x2545f491u + (uint32_t)i * 0x9e3779b1u;       /* the same variant is the same samples every time */
    k_entry[i].make(k_entry[i].v);
    size_t len = g_end < cap ? g_end : cap;
    double top = 1e-9;
    for (size_t k = 0; k < len; k++)
        if (fabs(g_acc[k]) > top)
            top = fabs(g_acc[k]);
    double g = k_entry[i].peak / top;
    size_t fade = (size_t)RATE * 3 / 1000;
    for (size_t k = 0; k < len; k++) {
        double e = len - k < fade ? (double)(len - k) / (double)fade : 1.0;
        buf[k] = (int16_t)(g_acc[k] * g * e);
    }
    return len;
}
