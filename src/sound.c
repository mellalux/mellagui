/* Feedback beeps: synthesis + a detached `paplay`/`aplay` child. See sound.h. */
#include "sound.h"

#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "audio.h"
#include "chime.h"
#include "gamesound.h"
#include "log.h"
#include "worker.h"
#include <pthread.h>

#define RATE 44100
#define MAX_SAMPLES (RATE * 2)              /* the longest beep: 2 s (a lead-in of silence comes on top of it) */
#define WAKE_MS 800                         /* silence in front of a beep on a bare ALSA device that may be asleep (HDMI sinks need long) */
#define IDLE_MS 800                         /* HDA codecs power down after ~1 s (snd_hda_intel power_save=1) */
#define CHIME_PAD_ALSA_MS 1500              /* silence in front of the start-up chime: a bare ALSA device (HDMI above all) / the sound server */
#define CHIME_PAD_SERVER_MS 1000
#define WAKE_STREAM_MS 3000                 /* lm_sound_wake(): this much silence keeps the sink running (the server suspends an idle one after ~5 s) */

static int env_ms(const char *name, int def)
{
    const char *e = getenv(name);
    int v = e && *e ? atoi(e) : -1;
    return v >= 0 && v <= 5000 ? v : def;           /* MELLAGUI_SOUND_WAKE_MS / MELLAGUI_CHIME_PAD_MS tune them for a device that wakes slowly */
}

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
static const struct note k_dock[] = { { 1047, 40 }, { 1568, 110 }, { 0, 0 } };
static const struct note k_volume[] = { { 784, 140 }, { 0, 0 } };
static const struct note k_timer[] = { { 880, 130 }, { 1109, 130 }, { 1319, 300 }, { 0, 220 }, { 880, 130 }, { 1109, 130 }, { 1319, 420 }, { 0, 0 } };

static const struct note k_card_win[] = { { 523, 90 }, { 659, 90 }, { 784, 90 }, { 1047, 90 }, { 784, 70 }, { 1047, 70 }, { 1319, 420 }, { 0, 0 } };
static const struct note k_card_no[] = { { 233, 80 }, { 175, 170 }, { 0, 0 } };

/* the card sounds, one function with a number for each variant */
static size_t gen_draw(int variant, int16_t *buf, size_t cap);
static size_t gen_place(int variant, int16_t *buf, size_t cap);
static size_t gen_shuffle(int variant, int16_t *buf, size_t cap);
#define GEN(fn, v) static size_t fn##v(int16_t *buf, size_t cap) { return gen_##fn(v, buf, cap); }
GEN(draw, 0) GEN(draw, 1) GEN(draw, 2) GEN(draw, 3) GEN(place, 0) GEN(place, 1) GEN(place, 2) GEN(shuffle, 0) GEN(shuffle, 1)

static const struct {
    const char *name;
    const struct note *notes;
    bool pluck;                             /* plucked: a sharp start and an exponential decay instead of a flat tone */
    size_t (*gen)(int16_t *buf, size_t cap);        /* made from noise instead of notes (the card sounds) */
} k_sounds[LM_SOUND_COUNT] = {
    [LM_SOUND_STICKY_ON] = { "sticky-on", k_on },
    [LM_SOUND_STICKY_OFF] = { "sticky-off", k_off },
    [LM_SOUND_LATCH] = { "latch", k_latch },
    [LM_SOUND_LOCK] = { "lock", k_lock },
    [LM_SOUND_UNLOCK] = { "unlock", k_unlock },
    [LM_SOUND_SHOT] = { "shot", k_shot },
    [LM_SOUND_DOCK] = { "dock", k_dock, true },
    [LM_SOUND_VOLUME] = { "volume", k_volume, true },
    [LM_SOUND_TIMER] = { "timer", k_timer, true },
    [LM_SOUND_CARD_DRAW] = { "card-draw", NULL, false, draw0 },
    [LM_SOUND_CARD_DRAW2] = { "card-draw2", NULL, false, draw1 },
    [LM_SOUND_CARD_DRAW3] = { "card-draw3", NULL, false, draw2 },
    [LM_SOUND_CARD_DRAW4] = { "card-draw4", NULL, false, draw3 },
    [LM_SOUND_CARD_PLACE] = { "card-place", NULL, false, place0 },
    [LM_SOUND_CARD_PLACE2] = { "card-place2", NULL, false, place1 },
    [LM_SOUND_CARD_PLACE3] = { "card-place3", NULL, false, place2 },
    [LM_SOUND_CARD_SHUFFLE] = { "card-shuffle", NULL, false, shuffle0 },
    [LM_SOUND_CARD_SHUFFLE2] = { "card-shuffle2", NULL, false, shuffle1 },
    [LM_SOUND_CARD_WIN] = { "card-win", k_card_win, true },
    [LM_SOUND_CARD_NO] = { "card-no", k_card_no, true },
};

int lm_sound_total(void) { return LM_SOUND_COUNT + lm_gamesound_count(); }

const char *lm_sound_name(enum lm_sound s)
{
    if ((unsigned)s < LM_SOUND_COUNT)
        return k_sounds[s].name;
    const char *n = lm_gamesound_name((int)s - LM_SOUND_COUNT);
    return n ? n : "?";
}

/* Sine with a 5 ms linear fade in/out (no clicks), moderate volume. Returns the number of samples. */
static size_t synth(const struct note *n, int16_t *buf, size_t cap, bool pluck)
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
            if (pluck && n->hz) {                                   /* a marimba-like pluck: a bright second partial that dies fast, a round body */
                double t = (double)i / RATE, tau = (double)n->ms / 1000.0 / 2.6;
                v = (v + 0.35 * sin(4.0 * M_PI * n->hz * t) * exp(-t / (tau * 0.4))) * exp(-t / tau);
                env = i < RATE / 1000 ? (double)i / (RATE / 1000) : 1.0;
                if (len - i < fade)
                    env *= (double)(len - i) / (double)fade;
            }
            buf[pos + i] = (int16_t)(v * env * 8000.0);
        }
        pos += len;
    }
    return pos;
}

/* ---- the card sounds: shaped noise, no files ---- */

static uint32_t noise_next(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

static double noise_f(uint32_t *s) { return (double)noise_next(s) / 2147483648.0 - 1.0; }

/* One rustle: noise through a band-pass filter whose middle slides from f0 to f1 (a card sliding over cards), under a bump that peaks `peak` of the way
 * through, with `flutter` of crackle (the card's edge catching the others) and a small tap at the end where it lands. Added into acc[] (doubles, at `at`). */
struct rustle {
    double ms, f0, f1, q, flutter, peak, tap, gain;
    uint32_t seed;
};

static double g_acc[RATE * 2];

static void add_rustle(const struct rustle *r, size_t at, size_t cap)
{
    size_t len = (size_t)(r->ms * RATE / 1000.0);
    if (at + len > cap)
        len = cap - at;
    uint32_t seed = r->seed;
    double low = 0, band = 0, crackle = 0, flick = 0;
    double k = log(0.5) / log(r->peak);                              /* the bump's peak at `peak` of the length */
    double q = 1.0 / r->q;
    for (size_t i = 0; i < len; i++) {
        double u = (double)i / (double)len, t = (double)i / RATE;
        double fc = r->f0 * pow(r->f1 / r->f0, u), f = 2.0 * sin(M_PI * fc / RATE);
        double x = noise_f(&seed);
        low += f * band;
        double high = x - low - q * band;
        band += f * high;
        crackle += 0.18 * (noise_f(&seed) - crackle);                 /* slow noise: the loose, irregular part */
        if ((noise_next(&seed) & 1023) < 9)                           /* now and then a flick: a card edge letting go */
            flick = 1.0;
        flick *= 0.9985;
        double mod = 1.0 - r->flutter * (0.55 + 0.45 * crackle * 3.0) * 0.5 + r->flutter * 0.9 * flick;
        double bump = pow(sin(M_PI * pow(u, k)), 2.0);
        g_acc[at + i] += band * mod * bump * r->gain;
        (void)t;
    }
    if (r->tap > 0.0) {                                               /* the landing: a tiny filtered click */
        double lp = 0;
        size_t tl = (size_t)RATE * 14 / 1000;
        for (size_t i = 0; i < tl && at + len + i < cap; i++) {
            double t = (double)i / RATE, x = noise_f(&seed);
            lp += 0.45 * (x - lp);
            g_acc[at + len + i] += (x - lp) * exp(-t / 0.0035) * r->tap * r->gain * 0.8;
        }
    }
}

/* one thump on the table: a low note that falls a little and a filtered click, at `at` */
static void add_tap(size_t at, size_t cap, double hz, double amp, double click, uint32_t seed)
{
    double lp = 0;
    size_t tl = (size_t)RATE * 70 / 1000;
    for (size_t i = 0; i < tl && at + i < cap; i++) {
        double t = (double)i / RATE, x = noise_f(&seed);
        lp += 0.35 * (x - lp);
        double thump = sin(2.0 * M_PI * (hz + 70.0 * exp(-t / 0.008)) * t) * exp(-t / 0.022);
        g_acc[at + i] += amp * thump * 9000.0 + click * (x - lp) * exp(-t / 0.0028) * 9000.0;
    }
}

/* the doubles to 16 bit samples, the loudest at `peak` (and a short fade at both ends); returns len */
static size_t finish_acc(int16_t *buf, size_t len, double peak)
{
    double top = 1e-9;
    for (size_t i = 0; i < len; i++)
        if (fabs(g_acc[i]) > top)
            top = fabs(g_acc[i]);
    double g = peak / top;
    size_t fade = (size_t)RATE * 3 / 1000;
    for (size_t i = 0; i < len; i++) {
        double e = i < fade ? (double)i / (double)fade : len - i < fade ? (double)(len - i) / (double)fade : 1.0;
        buf[i] = (int16_t)(g_acc[i] * g * e);
    }
    return len;
}

/* A card pulled from the pile and turned: a rustle of its own kind each time. */
static size_t gen_draw(int variant, int16_t *buf, size_t cap)
{
    static const struct rustle v[4] = {
        { 150, 1800, 4300, 1.3, 0.55, 0.42, 0.30, 1.0, 0x1234abcdu },    /* a soft "fwsh" that brightens */
        { 200, 2600, 1300, 0.9, 0.70, 0.35, 0.18, 1.0, 0x9e3779b1u },    /* a longer slide that darkens */
        { 105, 3600, 6200, 1.6, 0.35, 0.50, 0.40, 1.0, 0x2545f491u },    /* a quick bright flick */
        { 240, 1200, 2700, 0.7, 0.85, 0.50, 0.22, 1.0, 0x6c8e9cf5u },    /* a breathy, loose one */
    };
    size_t len = (size_t)(v[variant].ms * RATE / 1000.0) + (size_t)RATE * 14 / 1000;
    if (len > cap)
        len = cap;
    memset(g_acc, 0, len * sizeof(double));
    add_rustle(&v[variant], 0, len);
    return finish_acc(buf, len, 11500.0 - 800.0 * variant);
}

/* A card laid down: it slides the last bit, then taps the table. */
static size_t gen_place(int variant, int16_t *buf, size_t cap)
{
    static const struct rustle slide[3] = {
        { 62, 2400, 1700, 1.2, 0.5, 0.6, 0.0, 0.55, 0x51ed270bu },
        { 78, 1900, 1300, 1.0, 0.6, 0.5, 0.0, 0.45, 0x7f4a7c15u },
        { 45, 3200, 2200, 1.5, 0.4, 0.6, 0.0, 0.5, 0x3c6ef372u },
    };
    static const struct { double hz, amp, click; } tap[3] = { { 160, 0.9, 0.7 }, { 125, 1.0, 0.45 }, { 190, 0.7, 0.9 } };
    size_t slide_len = (size_t)(slide[variant].ms * RATE / 1000.0), len = slide_len + (size_t)RATE * 80 / 1000;
    if (len > cap)
        len = cap;
    memset(g_acc, 0, len * sizeof(double));
    add_rustle(&slide[variant], 0, len);
    add_tap(slide_len, len, tap[variant].hz, tap[variant].amp, tap[variant].click, 0xabcd1234u + (uint32_t)variant);
    return finish_acc(buf, len, 13000.0);
}

/* The deck riffled: a rustle with many small flicks that come faster, and the two halves meeting. */
static size_t gen_shuffle(int variant, int16_t *buf, size_t cap)
{
    size_t len = (size_t)RATE * (variant ? 1250 : 900) / 1000;
    if (len > cap)
        len = cap;
    memset(g_acc, 0, len * sizeof(double));
    uint32_t seed = 0x2545f491u + (uint32_t)variant * 77u;
    int parts = variant ? 2 : 1;
    for (int p = 0; p < parts; p++) {
        double ms = variant ? 480.0 : 760.0;
        size_t at = (size_t)(p * (variant ? 600.0 : 0.0) * RATE / 1000.0), plen = (size_t)(ms * RATE / 1000.0);
        struct rustle bed = { ms, 1500.0 + 300.0 * p, 3600.0 + 500.0 * p, 0.8, 0.9, 0.55, 0.0, 0.8, seed + (uint32_t)p };
        add_rustle(&bed, at, len);
        double t = 0.0, gap = 0.032;
        while (t < ms / 1000.0 - 0.02) {                              /* the flicks: a card each, closer and closer together */
            size_t pos = at + (size_t)(t * RATE);
            double amp = 0.35 + 0.55 * (noise_f(&seed) * 0.5 + 0.5), lp = 0;
            for (size_t i = 0; i < (size_t)RATE * 9 / 1000 && pos + i < len; i++) {
                double tt = (double)i / RATE, x = noise_f(&seed);
                lp += 0.55 * (x - lp);
                g_acc[pos + i] += (x - lp) * exp(-tt / 0.0022) * amp * 7000.0;
            }
            gap = gap > 0.007 ? gap * 0.955 : gap;
            t += gap * (0.8 + 0.4 * (noise_f(&seed) * 0.5 + 0.5));
        }
        if (at + plen + 100 < len)                                    /* the halves meet: a soft tap */
            add_tap(at + plen, len, 140.0, 0.45, 0.5, seed + 99u);
        (void)plen;
    }
    return finish_acc(buf, len, 12500.0);
}

size_t lm_sound_render(enum lm_sound s, int16_t *buf, size_t cap)
{
    if ((unsigned)s >= LM_SOUND_COUNT)
        return lm_gamesound_render((int)s - LM_SOUND_COUNT, buf, cap);
    if (k_sounds[s].gen)
        return k_sounds[s].gen(buf, cap);
    return synth(k_sounds[s].notes, buf, cap, k_sounds[s].pluck);
}

static char g_device[64] = "default";
static pthread_mutex_t g_dev_lock = PTHREAD_MUTEX_INITIALIZER;     /* the loop writes the device, the worker thread plays with it */
static long long g_last_ms;                 /* CLOCK_MONOTONIC ms of the last beep on a bare ALSA device (worker thread only) */

void lm_sound_set_device(const char *name)
{
    if (!lm_audio_name_ok(name))
        return;
    pthread_mutex_lock(&g_dev_lock);
    snprintf(g_device, sizeof(g_device), "%s", name);
    pthread_mutex_unlock(&g_dev_lock);
}

static void device_copy(char *out, size_t n)
{
    pthread_mutex_lock(&g_dev_lock);
    snprintf(out, n, "%s", g_device);
    pthread_mutex_unlock(&g_dev_lock);
}

/* ---- which way the sounds go --------------------------------------------------------------------------------------------------- */

/* The sound card behind an ALSA device name as the sound server names it in a sink: "plughw:CARD=PCH,DEV=3" -> "pci-0000_00_1b.0" (the card's bus
 * address, ':' written as '_'). "" when it cannot be told (a USB card, a name nobody knows): then the choice of the user is left alone.
 * $MELLAGUI_SOUND_CARD_KEY replaces the lookup (tests). */
static void card_key(const char *dev, char *out, size_t n)
{
    out[0] = 0;
    const char *fake = getenv("MELLAGUI_SOUND_CARD_KEY");
    if (fake && *fake) {
        snprintf(out, n, "%s", fake);
        return;
    }
    int card = -1;
    const char *c = strstr(dev, "CARD=");
    if (c) {
        char name[32];
        size_t k = 0;
        for (c += 5; *c && *c != ',' && k + 1 < sizeof(name); c++)
            name[k++] = *c;
        name[k] = 0;
        FILE *f = fopen("/proc/asound/cards", "r");
        char line[200];
        while (f && fgets(line, sizeof(line), f)) {
            int idx;
            char id[32];
            if (sscanf(line, " %d [%31[^] ]", &idx, id) == 2 && !strcmp(id, name))
                card = idx;
        }
        if (f)
            fclose(f);
    } else {
        const char *colon = strchr(dev, ':');
        if (colon && isdigit((unsigned char)colon[1]))
            card = atoi(colon + 1);
    }
    if (card < 0)
        return;
    char link[128], target[256];
    snprintf(link, sizeof(link), "/sys/class/sound/card%d/device", card);
    ssize_t k = readlink(link, target, sizeof(target) - 1);
    if (k <= 0)
        return;
    target[k] = 0;
    const char *base = strrchr(target, '/');
    base = base ? base + 1 : target;
    if (!isdigit((unsigned char)base[0]) || strchr(base, '-'))      /* only a PCI address (0000:00:1b.0) is something we can find in a sink's name */
        return;
    snprintf(out, n, "pci-%.40s", base);
    for (char *p = out; *p; p++)
        if (*p == ':')
            *p = '_';
}

/* The name of the sound server's default sink ("" when no server answers or pactl is missing). Takes a few milliseconds, never more than 300. */
static void default_sink(char *out, size_t n)
{
    out[0] = 0;
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
        sigprocmask(SIG_SETMASK, &none, NULL);
        setsid();
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) {
            dup2(null, 0);
            dup2(null, 2);
        }
        dup2(fds[1], 1);
        execlp("pactl", "pactl", "get-default-sink", (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    size_t len = 0;
    for (int tries = 0; tries < 6 && len + 1 < n; tries++) {
        struct pollfd p = { .fd = fds[0], .events = POLLIN };
        if (poll(&p, 1, 50) <= 0)
            continue;
        ssize_t k = read(fds[0], out + len, n - 1 - len);
        if (k <= 0)
            break;
        len += (size_t)k;
    }
    out[len] = 0;
    close(fds[0]);
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    out[strcspn(out, "\r\n")] = 0;
}

/*
 * Do the sounds go through the sound server (paplay)? Yes when the device is "default" - and also when an ALSA device was chosen but the server's
 * default output is on ANOTHER card: a speaker or headphones that came after the choice was made (a Bluetooth speaker, a USB headset) would never
 * be heard, the sounds would play into the monitor's HDMI. So with a server that answers, the chosen device is only used while it is the card of the
 * server's own default sink. The answer is kept for 30 s (lm_sound_wake, at the dock, warms it up before the first click).
 */
static bool use_server(const char *device)
{
    if (!strcmp(device, "default"))
        return true;
    static int cached = -1;
    static long long at_ms;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long long now = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    if (cached < 0 || now - at_ms > 30000) {
        char sink[200], key[64];
        default_sink(sink, sizeof(sink));
        card_key(device, key, sizeof(key));
        bool elsewhere = sink[0] && key[0] && !strstr(sink, key);
        if (elsewhere != (cached == 1))
            lm_info("sounds: %s", elsewhere ? "the sound server's default output is on another card than the chosen ALSA device: playing through the server"
                                             : "playing through the chosen ALSA device");
        cached = elsewhere ? 1 : 0;
        at_ms = now;
    }
    return cached == 1;
}

static bool write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    while (len) {
        ssize_t k = write(fd, p, len);
        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0)
            return false;
        p += k;
        len -= (size_t)k;
    }
    return true;
}

static void play_now(enum lm_sound s, const char *g_device)
{
    /* room for the longest lead-in (env_ms() allows 5 s) and the longest beep */
    static int16_t buf[RATE * 5 + MAX_SAMPLES];
    /* A bare ALSA device (HDMI above all) is powered down when idle and swallows the first ~0.3 s while it wakes
     * up, which eats a 60 ms beep whole. So when it has been quiet for a while, play silence first. The sound
     * server (paplay) keeps the device awake itself. */
    size_t pad = 0;
    bool server = use_server(g_device);               /* (`pactl get-default-sink`, once in 30 s: this is the worker thread) */
    if (!server) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        long long now = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
        if (!g_last_ms || now - g_last_ms >= IDLE_MS)
            pad = (size_t)RATE * (size_t)env_ms("MELLAGUI_SOUND_WAKE_MS", WAKE_MS) / 1000;
        g_last_ms = now;
    }
    if (pad > (size_t)RATE * 5)
        pad = (size_t)RATE * 5;
    memset(buf, 0, pad * sizeof(int16_t));
    size_t n = lm_sound_render(s, buf + pad, MAX_SAMPLES);
    if (!n)
        return;
    n += pad;
    const char *rawdir = getenv("MELLAGUI_SOUND_RAW_DIR");
    if (rawdir && *rawdir) {                            /* tests: the samples (S16_LE, 44.1 kHz, mono, without the lead-in) go to <dir>/<name>.s16 instead of a player */
        char path[512];
        snprintf(path, sizeof(path), "%s/%s.s16", rawdir, lm_sound_name(s));
        FILE *f = fopen(path, "wb");
        if (f) {
            fwrite(buf + pad, sizeof(int16_t), n - pad, f);
            fclose(f);
        }
        return;
    }

    /* The picture goes through a memfd, not a pipe: a pipe holds 64 KiB, which is less than a lead-in of silence and a beep, and a write that does not
     * fit would be lost; nothing here can block or raise SIGPIPE. */
    int fd = memfd_create("mellagui-sound", 0);
    if (fd < 0)
        return;
    if (!write_all(fd, buf, n * sizeof(int16_t)) || lseek(fd, 0, SEEK_SET) < 0) {
        close(fd);
        return;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(fd);
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
        dup2(fd, 0);
        if (!server) {
            execlp("aplay", "aplay", "-q", "-D", g_device, "-t", "raw", "-f", "S16_LE", "-r", "44100", "-c", "1", "-", (char *)NULL);
        } else {
            /* Without paplay (or a server) fall back to aplay. */
            execlp("paplay", "paplay", "--raw", "--rate=44100", "--format=s16le", "--channels=1", (char *)NULL);
            execlp("aplay", "aplay", "-q", "-t", "raw", "-f", "S16_LE", "-r", "44100", "-c", "1", "-", (char *)NULL);
        }
        _exit(127);
    }
    close(fd);
    lm_debug("sound %s (player pid %d)", lm_sound_name(s), (int)pid);
}

struct play_job { enum lm_sound s; char device[64]; };

static void play_run(void *data)
{
    struct play_job *j = data;
    play_now(j->s, j->device);
}

/* Never waits: the beep is made and handed to a player by the worker thread (worker.h), after whatever was queued before it (a volume change). */
void lm_sound_play(enum lm_sound s)
{
    if ((int)s < 0 || (int)s >= lm_sound_total())
        return;
    struct play_job *j = calloc(1, sizeof(*j));
    if (!j)
        return;
    j->s = s;
    device_copy(j->device, sizeof(j->device));
    lm_work_submit(play_run, free, j, NULL);
}

/* The pointer is heading for something that is about to click (the dock): wake the sink now, so that the click's sound is not lost to its wake-up.
 * Only through the sound server (paplay shares the sink with whatever plays next); a bare ALSA device is opened by one program at a time, so
 * there the first beep after a pause carries its own lead-in silence instead (WAKE_MS). At most once in 4 s. */
static void wake_now(const char *device)
{
    static long long last_ms;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long long now = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    if (!use_server(device) || (last_ms && now - last_ms < 4000))
        return;
    last_ms = now;
    pid_t pid = fork();
    if (pid != 0)
        return;
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
    signal(SIGPIPE, SIG_DFL);
    setsid();
    int null = open("/dev/null", O_RDWR);
    if (null >= 0) {
        dup2(null, 0);
        dup2(null, 1);
        dup2(null, 2);
    }
    syscall(SYS_close_range, 3u, ~0u, 0u);
    char cmd[160];
    snprintf(cmd, sizeof(cmd), "head -c %d /dev/zero | exec paplay --raw --rate=44100 --format=s16le --channels=1", RATE * 2 * WAKE_STREAM_MS / 1000);
    execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
    _exit(127);
}

static void wake_run(void *data)
{
    wake_now(data);
}

void lm_sound_wake(void)
{
    char *dev = calloc(1, 64);
    if (!dev)
        return;
    device_copy(dev, 64);
    lm_work_submit(wake_run, free, dev, NULL);
}

static const char *g_noted_name = "none";
static unsigned g_noted;

void lm_sound_note(enum lm_sound s)
{
    g_noted_name = lm_sound_name(s);
    g_noted++;
}

const char *lm_sound_last(void) { return g_noted_name; }
unsigned lm_sound_noted(void) { return g_noted; }

static void le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

void lm_sound_startup(void)
{
    char g_device[64];                              /* (this copy, not the global: the child must not wait for a lock another thread holds) */
    device_copy(g_device, sizeof(g_device));
    pid_t pid = fork();
    if (pid < 0)
        return;
    if (pid > 0) {
        lm_debug("start-up chime (pid %d)", (int)pid);
        return;
    }
    /* ---- the child ---- */
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
    signal(SIGPIPE, SIG_DFL);
    signal(SIGCHLD, SIG_DFL);                       /* the server's SIGCHLD handling is not ours: we wait for our own player */
    setsid();
    int null = open("/dev/null", O_RDWR);
    if (null >= 0) {
        dup2(null, 0);
        dup2(null, 1);
        dup2(null, 2);
    }
    if (syscall(SYS_close_range, 3u, ~0u, 0u) != 0)         /* the DRM and input fds are not for us (and must not stay open for 6 s) */
        for (int fd = 3; fd < 256; fd++)
            close(fd);
    static int16_t pcm[LM_CHIME_RATE * 5 + LM_CHIME_MAX_SAMPLES];             /* (room for up to 5 s of padding) */
    /* silence in front: a sleeping HDMI sink (or a codec in power save) swallows the first second or so, and the tune must not lose its start */
    bool server = use_server(g_device);             /* (here in the child: it may take its time) */
    size_t pad = (size_t)LM_CHIME_RATE * (size_t)env_ms("MELLAGUI_CHIME_PAD_MS", server ? CHIME_PAD_SERVER_MS : CHIME_PAD_ALSA_MS) / 1000;
    size_t n = lm_chime_render(pcm + pad, LM_CHIME_MAX_SAMPLES);
    if (!n)
        _exit(1);
    size_t total = (pad + n) * sizeof(int16_t);
    const char *wav = getenv("MELLAGUI_CHIME_WAV");
    int fd;
    if (wav && *wav) {
        fd = open(wav, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0)
            _exit(1);
        uint8_t h[44] = "RIFF....WAVEfmt \x10\0\0\0\x01\0\x01\0........\x02\0\x10\0data....";
        le32(h + 4, (uint32_t)(36 + total));
        le32(h + 24, LM_CHIME_RATE);
        le32(h + 28, LM_CHIME_RATE * 2);
        le32(h + 40, (uint32_t)total);
        _exit(write_all(fd, h, 44) && write_all(fd, pcm, total) && close(fd) == 0 ? 0 : 1);
    }
    fd = memfd_create("mellagui-chime", 0);
    if (fd < 0 || !write_all(fd, pcm, total))
        _exit(1);
    for (int tries = 0; tries < 12; tries++) {
        pid_t p = fork();
        if (p == 0) {
            lseek(fd, 0, SEEK_SET);
            dup2(fd, 0);
            if (!server) {
                execlp("aplay", "aplay", "-q", "-D", g_device, "-t", "raw", "-f", "S16_LE", "-r", "44100", "-c", "1", "-", (char *)NULL);
            } else {
                execlp("paplay", "paplay", "--raw", "--rate=44100", "--format=s16le", "--channels=1", (char *)NULL);
                execlp("aplay", "aplay", "-q", "-t", "raw", "-f", "S16_LE", "-r", "44100", "-c", "1", "-", (char *)NULL);
            }
            _exit(127);
        }
        int st = 0;
        if (p > 0 && waitpid(p, &st, 0) == p && WIFEXITED(st) && WEXITSTATUS(st) == 0)
            _exit(0);
        if (p > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 127)
            _exit(1);                               /* neither paplay nor aplay is installed */
        usleep(500000);
    }
    _exit(1);
}
