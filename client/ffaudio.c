#include "ffaudio.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "ffpipe.h"
#include "img.h"
#include <poll.h>

struct ffaudio {
    pid_t ff, player;
    bool stopped;
    bool reaped_player;
    int64_t t0;
};

static const struct {
    const char *name;
    const char *const argv[12];
} g_players[] = {
    { "pw-cat", { "pw-cat", "--playback", "--format=s16", "--rate=48000", "--channels=2", "--latency=100ms", "-", NULL } },
    { "paplay", { "paplay", "--raw", "--format=s16le", "--rate=48000", "--channels=2", "--latency-msec=100", NULL } },
    { "aplay", { "aplay", "-q", "-t", "raw", "-f", "S16_LE", "-r", "48000", "-c", "2", NULL } },
};
#define N_PLAYERS ((int)(sizeof(g_players) / sizeof(g_players[0])))

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static bool in_path(const char *prog)
{
    if (strchr(prog, '/'))
        return access(prog, X_OK) == 0;
    const char *path = getenv("PATH");
    for (const char *s = path ? path : ""; *s;) {
        size_t l = strcspn(s, ":");
        char full[1024];
        snprintf(full, sizeof(full), "%.*s/%s", (int)(l < 800 ? l : 800), s, prog);
        if (access(full, X_OK) == 0)
            return true;
        s += l + (s[l] ? 1 : 0);
    }
    return false;
}

/* The players that exist, in order; $MELLAGUI_AUDIO_PLAYER puts one first (or "none": no sound). */
static int g_order[N_PLAYERS + 1], g_n = -1;
static char g_forced[256];

static void find_players(void)
{
    if (g_n >= 0)
        return;
    g_n = 0;
    const char *f = getenv("MELLAGUI_AUDIO_PLAYER");
    if (f && !strcmp(f, "none"))
        return;
    if (f && *f)
        snprintf(g_forced, sizeof(g_forced), "%s", f);
    if (g_forced[0] && in_path(g_forced))
        g_order[g_n++] = -1;
    for (int i = 0; i < N_PLAYERS; i++)
        if (in_path(g_players[i].name))
            g_order[g_n++] = i;
}

int ffaudio_players(void)
{
    find_players();
    return ffpipe_ffmpeg_path() ? g_n : 0;
}

const char *ffaudio_player_name(int i)
{
    find_players();
    if (i < 0 || i >= g_n)
        return "";
    return g_order[i] < 0 ? g_forced : g_players[g_order[i]].name;
}

struct ffaudio *ffaudio_start(const char *path, uint64_t start_ms, int player, char *err, size_t errsz)
{
    find_players();
    const char *ff = ffpipe_ffmpeg_path();
    if (!ff || player < 0 || player >= g_n) {
        snprintf(err, errsz, "no sound: %s", ff ? "no player" : "no ffmpeg");
        return NULL;
    }
    int pcm[2], st[2];
    if (pipe2(pcm, O_CLOEXEC) < 0 || pipe2(st, O_CLOEXEC) < 0) {
        snprintf(err, errsz, "pipe: %s", strerror(errno));
        return NULL;
    }
    char ss[32];
    snprintf(ss, sizeof(ss), "%llu.%03llu", (unsigned long long)(start_ms / 1000), (unsigned long long)(start_ms % 1000));
    pid_t parent = getpid();
    pid_t fpid = fork();
    if (fpid == 0) {                                            /* ffmpeg: the sound as raw 16 bit stereo */
        setpgid(0, 0);
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (getppid() != parent)
            _exit(1);
        int nul = open("/dev/null", O_RDWR);
        dup2(nul, 0);
        dup2(nul, 2);
        dup2(pcm[1], 1);
        execl(ff, ff, "-hide_banner", "-loglevel", "quiet", "-nostdin", "-ss", ss, "-i", path, "-vn", "-sn", "-f", "s16le", "-ar", "48000", "-ac", "2",
              "pipe:1", (char *)NULL);
        _exit(127);
    }
    if (fpid < 0) {
        snprintf(err, errsz, "fork: %s", strerror(errno));
        close(pcm[0]); close(pcm[1]); close(st[0]); close(st[1]);
        return NULL;
    }
    setpgid(fpid, fpid);                                        /* (both sides set it: no race with the player's setpgid) */
    pid_t ppid = fork();
    if (ppid == 0) {                                            /* the player reads it */
        setpgid(0, fpid);
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (getppid() != parent)
            _exit(1);
        int nul = open("/dev/null", O_RDWR);
        dup2(pcm[0], 0);
        dup2(nul, 1);
        dup2(nul, 2);
        if (g_order[player] < 0) {
            execlp(g_forced, g_forced, (char *)NULL);
        } else {
            const char *const *av = g_players[g_order[player]].argv;
            execvp(av[0], (char *const *)av);
        }
        int e = errno;
        if (write(st[1], &e, sizeof(e)) < 0) {}
        _exit(127);
    }
    close(pcm[0]);
    close(pcm[1]);
    close(st[1]);
    int e = 0;
    ssize_t k = ppid > 0 ? read(st[0], &e, sizeof(e)) : -1;
    close(st[0]);
    if (ppid < 0 || k == (ssize_t)sizeof(e)) {
        kill(fpid, SIGKILL);
        waitpid(fpid, NULL, 0);
        if (ppid > 0)
            waitpid(ppid, NULL, 0);
        snprintf(err, errsz, "cannot start %s", ffaudio_player_name(player));
        return NULL;
    }
    setpgid(ppid, fpid);
    struct ffaudio *a = calloc(1, sizeof(*a));
    if (!a) {
        kill(-fpid, SIGKILL);
        waitpid(fpid, NULL, 0);
        waitpid(ppid, NULL, 0);
        return NULL;
    }
    a->ff = fpid;
    a->player = ppid;
    a->t0 = now_ms();
    return a;
}

int ffaudio_alive(struct ffaudio *a, bool *quick)
{
    if (quick)
        *quick = false;
    if (!a || a->reaped_player)
        return 0;
    int status;
    pid_t r = waitpid(a->player, &status, WNOHANG);
    if (r == 0)
        return 1;
    a->reaped_player = true;
    if (quick)
        *quick = now_ms() - a->t0 < 2000 && !(r > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0);      /* gave up at once: no sound server */
    return 0;
}

void ffaudio_stop(struct ffaudio *a)
{
    if (a && !a->stopped) {
        kill(-a->ff, SIGSTOP);
        a->stopped = true;
    }
}

void ffaudio_cont(struct ffaudio *a)
{
    if (a && a->stopped) {
        kill(-a->ff, SIGCONT);
        a->stopped = false;
    }
}

bool ffaudio_is_stopped(const struct ffaudio *a)
{
    return a && a->stopped;
}

void ffaudio_close(struct ffaudio *a)
{
    if (!a)
        return;
    kill(-a->ff, SIGKILL);
    kill(-a->ff, SIGCONT);
    waitpid(a->ff, NULL, 0);
    if (!a->reaped_player)
        waitpid(a->player, NULL, 0);
    free(a);
}

/* ---- sound files ---------------------------------------------------------------------------------------------------------------- */

bool ffaudio_name_is_audio(const char *name)
{
    static const char *const ext[] = { ".mp3", ".flac", ".ogg", ".oga", ".opus", ".wav", ".m4a", ".aac", ".wma", ".aif", ".aiff", ".mka", ".weba" };
    const char *dot = strrchr(name, '.');
    if (!dot || name[0] == '.')
        return false;
    for (size_t i = 0; i < sizeof(ext) / sizeof(ext[0]); i++)
        if (!strcasecmp(dot, ext[i]))
            return true;
    return false;
}

/* Runs argv and collects its stderr (or stdout) up to cap bytes; gives up after 10 s. Returns malloc'd text (NUL terminated) or NULL. */
static char *capture(char *const argv[], bool from_stdout, size_t cap, size_t *len_out)
{
    int pp[2];
    if (pipe2(pp, O_CLOEXEC) < 0)
        return NULL;
    pid_t parent = getpid();
    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (getppid() != parent)
            _exit(1);
        int nul = open("/dev/null", O_RDWR);
        dup2(nul, 0);
        dup2(from_stdout ? pp[1] : nul, 1);
        dup2(from_stdout ? nul : pp[1], 2);
        execv(argv[0], argv);
        _exit(127);
    }
    close(pp[1]);
    if (pid < 0) {
        close(pp[0]);
        return NULL;
    }
    char *buf = malloc(cap + 1);
    size_t n = 0;
    int64_t end = now_ms() + 10000;
    while (buf) {
        int left = (int)(end - now_ms());
        if (left <= 0)
            break;
        struct pollfd p = { .fd = pp[0], .events = POLLIN };
        int r = poll(&p, 1, left);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        ssize_t k = read(pp[0], buf + n, cap - n);
        if (k <= 0)
            break;                                              /* the end of the output */
        n += (size_t)k;
        if (n >= cap)
            break;
    }
    close(pp[0]);
    kill(-pid, SIGKILL);
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
        ;
    if (buf)
        buf[n] = 0;
    if (len_out)
        *len_out = n;
    return buf;
}

static void tag_value(const char *line, const char *key, char *out, size_t cap)
{
    while (*line == ' ')
        line++;
    size_t kl = strlen(key);
    if (strncasecmp(line, key, kl) != 0 || out[0])
        return;
    const char *p = line + kl;
    while (*p == ' ')
        p++;
    if (*p != ':')
        return;
    p++;
    while (*p == ' ')
        p++;
    snprintf(out, cap, "%s", p);
}

bool ffaudio_probe(const char *path, struct ffaudio_info *info, char *err, size_t errsz)
{
    memset(info, 0, sizeof(*info));
    const char *ff = ffpipe_ffmpeg_path();
    if (!ff) {
        snprintf(err, errsz, "Sound files need ffmpeg (it is not installed)");
        return false;
    }
    char *argv[] = { (char *)ff, (char *)"-hide_banner", (char *)"-nostdin", (char *)"-i", (char *)path, NULL };
    char *text = capture(argv, false, 65536, NULL);
    if (!text) {
        snprintf(err, errsz, "cannot run ffmpeg");
        return false;
    }
    bool audio = false;
    for (char *save = NULL, *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        const char *d = strstr(line, "Duration:");
        if (d && !info->duration_ms) {
            int h, m;
            double sec;
            if (sscanf(d + 9, " %d:%d:%lf", &h, &m, &sec) == 3)
                info->duration_ms = (uint64_t)(((double)h * 3600 + m * 60 + sec) * 1000.0 + 0.5);
            const char *br = strstr(line, "bitrate:");
            if (br)
                info->kbps = atoi(br + 8);
        }
        const char *au = strstr(line, "Audio:");
        if (au && strstr(line, "Stream") && !audio) {
            audio = true;
            au += 6;
            while (*au == ' ')
                au++;
            size_t k = 0;
            while (au[k] && au[k] != ',' && au[k] != ' ' && k < sizeof(info->codec) - 1)
                k++;
            snprintf(info->codec, sizeof(info->codec), "%.*s", (int)k, au);
            const char *hz = strstr(au, " Hz");
            if (hz) {
                const char *b = hz;
                while (b > au && b[-1] >= '0' && b[-1] <= '9')
                    b--;
                info->rate = atoi(b);
                const char *c = strchr(hz, ',');
                if (c) {
                    c++;
                    while (*c == ' ')
                        c++;
                    size_t cl = strcspn(c, ",");
                    snprintf(info->channels, sizeof(info->channels), "%.*s", (int)(cl < 15 ? cl : 15), c);
                }
            }
        }
        if (strstr(line, "Stream") && strstr(line, "Video:") && strstr(line, "attached pic"))
            info->cover = true;
        tag_value(line, "title", info->title, sizeof(info->title));
        tag_value(line, "artist", info->artist, sizeof(info->artist));
        tag_value(line, "album", info->album, sizeof(info->album));
    }
    free(text);
    if (!audio) {
        snprintf(err, errsz, "No sound track in this file");
        return false;
    }
    return true;
}

bool ffaudio_cover(const char *path, struct lm_image *img)
{
    const char *ff = ffpipe_ffmpeg_path();
    if (!ff)
        return false;
    char *argv[] = { (char *)ff, (char *)"-v", (char *)"error", (char *)"-nostdin", (char *)"-i", (char *)path, (char *)"-an", (char *)"-frames:v", (char *)"1",
                     (char *)"-f", (char *)"image2pipe", (char *)"-vcodec", (char *)"png", (char *)"pipe:1", NULL };
    size_t n = 0;
    char *data = capture(argv, true, 16u << 20, &n);
    bool ok = false;
    if (data && n > 8) {
        char err[80];
        ok = lm_img_decode((const uint8_t *)data, n, img, err, sizeof(err)) == 0 && img->w > 0 && img->h > 0;
    }
    free(data);
    return ok;
}
