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
