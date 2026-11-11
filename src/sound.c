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
#include "log.h"

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

static const struct {
    const char *name;
    const struct note *notes;
    bool pluck;                             /* plucked: a sharp start and an exponential decay instead of a flat tone */
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
};

const char *lm_sound_name(enum lm_sound s)
{
    return (unsigned)s < LM_SOUND_COUNT ? k_sounds[s].name : "?";
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

static char g_device[64] = "default";
static long long g_last_ms;                 /* CLOCK_MONOTONIC ms of the last beep on a bare ALSA device */

void lm_sound_set_device(const char *name)
{
    if (lm_audio_name_ok(name))
        snprintf(g_device, sizeof(g_device), "%s", name);
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
static bool use_server(void)
{
    if (!strcmp(g_device, "default"))
        return true;
    static int cached = -1;
    static long long at_ms;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long long now = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    if (cached < 0 || now - at_ms > 30000) {
        char sink[200], key[64];
        default_sink(sink, sizeof(sink));
        card_key(g_device, key, sizeof(key));
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

void lm_sound_play(enum lm_sound s)
{
    if ((unsigned)s >= LM_SOUND_COUNT)
        return;
    /* room for the longest lead-in (env_ms() allows 5 s) and the longest beep */
    static int16_t buf[RATE * 5 + MAX_SAMPLES];
    /* A bare ALSA device (HDMI above all) is powered down when idle and swallows the first ~0.3 s while it wakes
     * up, which eats a 60 ms beep whole. So when it has been quiet for a while, play silence first. The sound
     * server (paplay) keeps the device awake itself. */
    size_t pad = 0;
    bool server = use_server();                     /* (a few ms at most, once in 30 s) */
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
    size_t n = synth(k_sounds[s].notes, buf + pad, MAX_SAMPLES, k_sounds[s].pluck);
    if (!n)
        return;
    n += pad;

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
    lm_debug("sound %s (player pid %d)", k_sounds[s].name, (int)pid);
}

/* The pointer is heading for something that is about to click (the dock): wake the sink now, so that the click's sound is not lost to its wake-up.
 * Only through the sound server (paplay shares the sink with whatever plays next); a bare ALSA device is opened by one program at a time, so
 * there the first beep after a pause carries its own lead-in silence instead (WAKE_MS). At most once in 4 s. */
void lm_sound_wake(void)
{
    static long long last_ms;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long long now = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    if (!use_server() || (last_ms && now - last_ms < 4000))
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
    bool server = use_server();                     /* (here in the child: it may take its time) */
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
