#include "audio.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "log.h"

bool lm_audio_name_ok(const char *name)
{
    size_t n = name ? strlen(name) : 0;
    if (n < 1 || n > 63)
        return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (!isalnum(c) && !strchr("_:=,.+@-", c))
            return false;
    }
    return true;
}

/* ---- list building ------------------------------------------------------------------------------------------- */

static void add_entry(char *out, size_t n, const char *name, const char *desc)
{
    size_t len = strlen(out);
    char d[120];
    snprintf(d, sizeof(d), "%s", desc);
    for (char *p = d; *p; p++)
        if (*p == '|' || *p == ';' || *p == '\n')
            *p = ' ';
    int need = snprintf(NULL, 0, "%s%s|%s", len ? ";" : "", name, d);
    if (need < 0 || len + (size_t)need + 1 > n)
        return;                                         /* full: later entries are dropped */
    snprintf(out + len, n - len, "%s%s|%s", len ? ";" : "", name, d);
}

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1]))
        s[--n] = 0;
    char *p = s;
    while (*p && isspace((unsigned char)*p))
        p++;
    memmove(s, p, strlen(p) + 1);
}

struct card { int index; char id[32], label[64]; };

void lm_audio_list_alsa(char *out, size_t n)
{
    if (!n)
        return;
    out[0] = 0;
    add_entry(out, n, "default", "System default");
    const char *root = getenv("LUMEN_ASOUND_DIR");
    if (!root || !*root)
        root = "/proc/asound";
    char path[512];
    struct card cards[16];
    int ncards = 0;
    snprintf(path, sizeof(path), "%s/cards", root);
    FILE *f = fopen(path, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f) && ncards < 16) {
            int idx;
            char id[32];
            /* " 0 [PCH            ]: HDA-Intel - HDA Intel PCH" then "                      HDA Intel PCH at 0xf7e1 irq 46" */
            if (sscanf(line, " %d [%31[^]]", &idx, id) == 2) {
                struct card *c = &cards[ncards++];
                c->index = idx;
                trim(id);
                snprintf(c->id, sizeof(c->id), "%s", id);
                char label[64];
                const char *dash = strstr(line, " - ");
                snprintf(label, sizeof(label), "%s", dash ? dash + 3 : id);
                trim(label);
                snprintf(c->label, sizeof(c->label), "%s", label);
            }
        }
        fclose(f);
    }
    snprintf(path, sizeof(path), "%s/pcm", root);
    f = fopen(path, "r");
    if (!f)
        return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        int card, dev;
        if (sscanf(line, "%d-%d:", &card, &dev) != 2 || !strstr(line, "playback"))
            continue;
        char *colon = strchr(line, ':');
        char name[96] = "";
        if (colon) {
            snprintf(name, sizeof(name), "%s", colon + 1);
            char *end = strchr(name, ':');
            if (end)
                *end = 0;
            trim(name);
        }
        const struct card *c = NULL;
        for (int i = 0; i < ncards; i++)
            if (cards[i].index == card)
                c = &cards[i];
        char dname[64], desc[120];
        if (c)
            snprintf(dname, sizeof(dname), "plughw:CARD=%s,DEV=%d", c->id, dev);
        else
            snprintf(dname, sizeof(dname), "plughw:%d,%d", card, dev);
        if (!lm_audio_name_ok(dname))
            continue;
        bool hdmi = strcasestr(name, "HDMI") || strcasestr(name, "DisplayPort") || strstr(name, " DP");
        snprintf(desc, sizeof(desc), "%s - %s%s", c ? c->label : "card", name, hdmi ? " (HDMI)" : "");
        add_entry(out, n, dname, desc);
    }
    fclose(f);
}

/* ---- pactl ------------------------------------------------------------------------------------------------------ */

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* Runs argv[0] (searched in PATH) with LC_ALL=C, stdout captured into out (NUL-terminated), at most timeout_ms. Returns true
 * when the program started and ended by itself in time. */
static bool run_capture(char *const argv[], char *out, size_t n, int timeout_ms)
{
    if (n)
        out[0] = 0;
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) < 0)
        return false;
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return false;
    }
    if (pid == 0) {
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        signal(SIGPIPE, SIG_DFL);
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) {
            dup2(null, 0);
            dup2(null, 2);
        }
        dup2(fds[1], 1);
        setenv("LC_ALL", "C", 1);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(fds[1]);
    size_t len = 0;
    bool done = false;
    long deadline = now_ms() + timeout_ms;
    while (!done) {
        long left = deadline - now_ms();
        if (left <= 0)
            break;
        struct pollfd p = { .fd = fds[0], .events = POLLIN };
        int r = poll(&p, 1, (int)left);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        char buf[1024];
        ssize_t k = read(fds[0], buf, sizeof(buf));
        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0) {
            done = true;                                /* EOF: the program closed its output (it ended) */
            break;
        }
        if (len + (size_t)k < n) {
            memcpy(out + len, buf, (size_t)k);
            len += (size_t)k;
            out[len] = 0;
        }
    }
    close(fds[0]);
    if (!done)
        kill(pid, SIGKILL);
    int st;
    waitpid(pid, &st, 0);                               /* may already have been reaped by the server's SIGCHLD watch */
    return done;
}

int lm_audio_list_sinks(char *list, size_t n, char *current, size_t cn)
{
    if (n)
        list[0] = 0;
    if (cn)
        current[0] = 0;
    static char text[16384];
    char *const list_cmd[] = { "pactl", "list", "sinks", NULL };
    if (!run_capture(list_cmd, text, sizeof(text), 1000))
        return 0;
    int count = 0;
    char name[128] = "";
    for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (!strncmp(p, "Name: ", 6)) {
            snprintf(name, sizeof(name), "%s", p + 6);
            trim(name);
        } else if (!strncmp(p, "Description: ", 13) && name[0]) {
            char desc[120];
            snprintf(desc, sizeof(desc), "%s", p + 13);
            trim(desc);
            if (lm_audio_name_ok(name)) {
                add_entry(list, n, name, desc);
                count++;
            }
            name[0] = 0;
        }
    }
    static char cur[256];
    char *const get_cmd[] = { "pactl", "get-default-sink", NULL };
    if (cn && run_capture(get_cmd, cur, sizeof(cur), 1000)) {
        trim(cur);
        if (lm_audio_name_ok(cur))
            snprintf(current, cn, "%s", cur);
    }
    return count;
}

bool lm_audio_set_sink(const char *name)
{
    if (!lm_audio_name_ok(name))
        return false;
    static char sink_buf[256];
    char *const set_cmd[] = { "pactl", "set-default-sink", (char *)name, NULL };
    run_capture(set_cmd, sink_buf, sizeof(sink_buf), 1500);
    char *const get_cmd[] = { "pactl", "get-default-sink", NULL };
    if (!run_capture(get_cmd, sink_buf, sizeof(sink_buf), 1000))
        return false;
    trim(sink_buf);
    return !strcmp(sink_buf, name);
}
