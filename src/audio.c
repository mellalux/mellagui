#include "audio.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "loop.h"
#include "util.h"

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
    const char *root = getenv("MELLAGUI_ASOUND_DIR");
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

static bool in_path(const char *prog)
{
    const char *path = getenv("PATH");
    if (!path)
        path = "/usr/local/bin:/usr/bin:/bin";
    char dir[512];
    while (*path) {
        size_t len = strcspn(path, ":");
        snprintf(dir, sizeof(dir), "%.*s/%s", (int)len, path, prog);
        if (len && access(dir, X_OK) == 0)
            return true;
        path += len + (path[len] ? 1 : 0);
    }
    return false;
}

enum sink_backend { SINKS_NONE, SINKS_PULSE, SINKS_WPCTL };
static enum sink_backend g_sink_backend = SINKS_NONE;
static char g_sink_status[16] = "";

const char *lm_audio_sink_status(void)
{
    return g_sink_status;
}

/*
 * `wpctl status`:
 *  Audio
 *   ├─ Devices:
 *   │      49. Built-in Audio                      [alsa]
 *   ├─ Sinks:
 *   │  *   53. Built-in Audio Analog Stereo        [vol: 0.40]
 *   │      54. Built-in Audio Digital Stereo (HDMI) [vol: 1.00]
 *   ├─ Sources:
 */
static int parse_wpctl_section(const char *text, const char *section, char *list, size_t n, char *current, size_t cn)
{
    if (n)
        list[0] = 0;
    if (cn)
        current[0] = 0;
    int count = 0;
    bool in_audio = false, in_sinks = false;
    const char *p = text;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[256];
        snprintf(line, sizeof(line), "%.*s", (int)len, p);
        p = eol ? eol + 1 : p + len;
        const char *l = line;
        if (!strncmp(l, "Audio", 5)) {
            in_audio = true;
            in_sinks = false;
            continue;
        }
        if (!strncmp(l, "Video", 5) || !strncmp(l, "Settings", 8)) {
            in_audio = in_sinks = false;
            continue;
        }
        if (!in_audio)
            continue;
        if (strstr(l, section)) {
            in_sinks = true;
            continue;
        }
        if (strstr(l, "Sinks:") || strstr(l, "Sources:") || strstr(l, "Filters:") || strstr(l, "Streams:") || strstr(l, "Devices:")) {
            in_sinks = false;
            continue;
        }
        if (!in_sinks)
            continue;
        bool star = false;
        while (*l == ' ' || *l == '*' || *l == '|' || (unsigned char)*l >= 0x80) {      /* tree drawing, the default marker */
            if (*l == '*')
                star = true;
            l++;
        }
        if (!isdigit((unsigned char)*l))
            continue;
        char id[16];
        int k = 0;
        while (isdigit((unsigned char)*l) && k < 15)
            id[k++] = *l++;
        id[k] = 0;
        if (*l != '.')
            continue;
        l++;
        while (*l == ' ')
            l++;
        char desc[120];
        snprintf(desc, sizeof(desc), "%s", l);
        char *br = strstr(desc, "[vol:");
        if (br)
            *br = 0;
        trim(desc);
        add_entry(list, n, id, desc);
        if (star && cn)
            snprintf(current, cn, "%s", id);
        count++;
    }
    return count;
}

int lm_audio_parse_wpctl_sinks(const char *text, char *list, size_t n, char *current, size_t cn)
{
    return parse_wpctl_section(text, "Sinks:", list, n, current, cn);
}

int lm_audio_parse_wpctl_sources(const char *text, char *list, size_t n, char *current, size_t cn)
{
    return parse_wpctl_section(text, "Sources:", list, n, current, cn);
}

int lm_audio_list_sinks(char *list, size_t n, char *current, size_t cn)
{
    if (n)
        list[0] = 0;
    if (cn)
        current[0] = 0;
    g_sink_backend = SINKS_NONE;
    static char text[16384];
    char *const list_cmd[] = { "pactl", "list", "sinks", NULL };
    int count = 0;
    if (in_path("pactl") && run_capture(list_cmd, text, sizeof(text), 1000)) {
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
        if (count > 0) {
            g_sink_backend = SINKS_PULSE;
            static char cur[256];
            char *const get_cmd[] = { "pactl", "get-default-sink", NULL };
            if (cn && run_capture(get_cmd, cur, sizeof(cur), 1000)) {
                trim(cur);
                if (lm_audio_name_ok(cur))
                    snprintf(current, cn, "%s", cur);
            }
        }
    }
    if (count == 0 && in_path("wpctl")) {                       /* PipeWire without pulseaudio-utils */
        char *const status_cmd[] = { "wpctl", "status", NULL };
        if (run_capture(status_cmd, text, sizeof(text), 1500)) {
            count = lm_audio_parse_wpctl_sinks(text, list, n, current, cn);
            if (count > 0)
                g_sink_backend = SINKS_WPCTL;
        }
    }
    snprintf(g_sink_status, sizeof(g_sink_status), "%s",
             count > 0 ? "ok" : (in_path("pactl") || in_path("wpctl")) ? "no-server" : "missing-tools");
    return count;
}

bool lm_audio_set_sink(const char *name)
{
    if (!lm_audio_name_ok(name))
        return false;
    static char buf[256];
    if (g_sink_backend == SINKS_WPCTL) {
        char *const set_cmd[] = { "wpctl", "set-default", (char *)name, NULL };
        run_capture(set_cmd, buf, sizeof(buf), 1500);
        char list[2048], cur[64];
        lm_audio_list_sinks(list, sizeof(list), cur, sizeof(cur));
        return !strcmp(cur, name);
    }
    char *const set_cmd[] = { "pactl", "set-default-sink", (char *)name, NULL };
    run_capture(set_cmd, buf, sizeof(buf), 1500);
    char *const get_cmd[] = { "pactl", "get-default-sink", NULL };
    if (!run_capture(get_cmd, buf, sizeof(buf), 1000))
        return false;
    trim(buf);
    return !strcmp(buf, name);
}

/* ---- microphones ----------------------------------------------------------------------------------------------- */

static enum sink_backend g_source_backend = SINKS_NONE;

int lm_audio_list_sources(char *list, size_t n, char *current, size_t cn)
{
    if (n)
        list[0] = 0;
    if (cn)
        current[0] = 0;
    g_source_backend = SINKS_NONE;
    static char text[16384];
    char *const list_cmd[] = { "pactl", "list", "sources", NULL };
    int count = 0;
    if (in_path("pactl") && run_capture(list_cmd, text, sizeof(text), 1000)) {
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
                size_t nl = strlen(name);
                bool monitor = nl > 8 && !strcmp(name + nl - 8, ".monitor");           /* the monitor of an output is not a microphone */
                if (!monitor && lm_audio_name_ok(name)) {
                    add_entry(list, n, name, desc);
                    count++;
                }
                name[0] = 0;
            }
        }
        if (count > 0) {
            g_source_backend = SINKS_PULSE;
            static char cur[256];
            char *const get_cmd[] = { "pactl", "get-default-source", NULL };
            if (cn && run_capture(get_cmd, cur, sizeof(cur), 1000)) {
                trim(cur);
                if (lm_audio_name_ok(cur))
                    snprintf(current, cn, "%s", cur);
            }
        }
    }
    if (count == 0 && in_path("wpctl")) {
        char *const status_cmd[] = { "wpctl", "status", NULL };
        if (run_capture(status_cmd, text, sizeof(text), 1500)) {
            count = lm_audio_parse_wpctl_sources(text, list, n, current, cn);
            if (count > 0)
                g_source_backend = SINKS_WPCTL;
        }
    }
    return count;
}

bool lm_audio_set_source(const char *name)
{
    if (!lm_audio_name_ok(name))
        return false;
    static char buf[256];
    if (g_source_backend == SINKS_WPCTL) {
        char *const set_cmd[] = { "wpctl", "set-default", (char *)name, NULL };
        run_capture(set_cmd, buf, sizeof(buf), 1500);
        char list[2048], cur[64];
        lm_audio_list_sources(list, sizeof(list), cur, sizeof(cur));
        return !strcmp(cur, name);
    }
    char *const set_cmd[] = { "pactl", "set-default-source", (char *)name, NULL };
    run_capture(set_cmd, buf, sizeof(buf), 1500);
    char *const get_cmd[] = { "pactl", "get-default-source", NULL };
    if (!run_capture(get_cmd, buf, sizeof(buf), 1000))
        return false;
    trim(buf);
    return !strcmp(buf, name);
}

bool lm_audio_source_state(int *volume, bool *mute)
{
    static char buf[1024];
    *volume = 0;
    *mute = false;
    if (g_source_backend == SINKS_WPCTL) {
        char *const cmd[] = { "wpctl", "get-volume", "@DEFAULT_AUDIO_SOURCE@", NULL };
        if (!run_capture(cmd, buf, sizeof(buf), 1000))
            return false;
        double v;
        const char *p = strstr(buf, "Volume:");
        if (!p || sscanf(p + 7, "%lf", &v) != 1)
            return false;
        *volume = (int)(v * 100.0 + 0.5);
        *mute = strstr(buf, "MUTED") != NULL;
        return true;
    }
    char *const vcmd[] = { "pactl", "get-source-volume", "@DEFAULT_SOURCE@", NULL };
    if (!in_path("pactl") || !run_capture(vcmd, buf, sizeof(buf), 1000))
        return false;
    const char *pc = strchr(buf, '%');                          /* "Volume: front-left: 45875 /  70% / -9.29 dB, ..." */
    if (!pc)
        return false;
    const char *b = pc;
    while (b > buf && isdigit((unsigned char)b[-1]))
        b--;
    *volume = atoi(b);
    char *const mcmd[] = { "pactl", "get-source-mute", "@DEFAULT_SOURCE@", NULL };
    if (run_capture(mcmd, buf, sizeof(buf), 1000))
        *mute = strstr(buf, "yes") != NULL;
    return true;
}

bool lm_audio_set_source_volume(int percent)
{
    if (percent < 0 || percent > 150)
        return false;
    static char buf[256];
    char v[32];
    if (g_source_backend == SINKS_WPCTL) {
        snprintf(v, sizeof(v), "%d.%02d", percent / 100, percent % 100);
        char *const cmd[] = { "wpctl", "set-volume", "@DEFAULT_AUDIO_SOURCE@", v, NULL };
        return run_capture(cmd, buf, sizeof(buf), 1000);
    }
    snprintf(v, sizeof(v), "%d%%", percent);
    char *const cmd[] = { "pactl", "set-source-volume", "@DEFAULT_SOURCE@", v, NULL };
    return in_path("pactl") && run_capture(cmd, buf, sizeof(buf), 1000);
}

bool lm_audio_set_source_mute(bool mute)
{
    static char buf[256];
    if (g_source_backend == SINKS_WPCTL) {
        char *const cmd[] = { "wpctl", "set-mute", "@DEFAULT_AUDIO_SOURCE@", mute ? "1" : "0", NULL };
        return run_capture(cmd, buf, sizeof(buf), 1000);
    }
    char *const cmd[] = { "pactl", "set-source-mute", "@DEFAULT_SOURCE@", mute ? "1" : "0", NULL };
    return in_path("pactl") && run_capture(cmd, buf, sizeof(buf), 1000);
}

/* ---- PipeWire card profiles (pw-cli) ------------------------------------------------------------------------------- */

static const char *after_prefix(const char *l, const char *prefix)
{
    while (*l == ' ' || *l == '\t')
        l++;
    size_t n = strlen(prefix);
    return strncmp(l, prefix, n) ? NULL : l + n;
}

/* The quoted string of a line like `      String "Built-in Audio"` / `device.description = "Built-in Audio"`. */
static bool quoted(const char *l, char *out, size_t n)
{
    const char *a = strchr(l, '"');
    const char *b = a ? strrchr(a + 1, '"') : NULL;
    if (!a || !b)
        return false;
    snprintf(out, n, "%.*s", (int)(b - a - 1), a + 1);
    return true;
}

int lm_audio_parse_cards(const char *text, struct lm_audio_card *out, int max)
{
    int n = 0, id = -1;
    char desc[64] = "";
    bool audio = false;
    const char *p = text;
    while (1) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[256];
        snprintf(line, sizeof(line), "%.*s", (int)len, p);
        const char *v;
        bool end = !eol;
        p = eol ? eol + 1 : p + len;
        if ((v = after_prefix(line, "id ")) || end) {                   /* a new object (or the end): flush the previous one */
            if (id >= 0 && audio && n < max) {
                out[n].id = id;
                snprintf(out[n].desc, sizeof(out[n].desc), "%s", desc[0] ? desc : "Sound card");
                n++;
            }
            id = -1;
            audio = false;
            desc[0] = 0;
            if (v)
                id = atoi(v);
        } else if (strstr(line, "media.class") && strstr(line, "Audio/Device")) {
            audio = true;
        } else if (strstr(line, "device.description")) {
            quoted(line, desc, sizeof(desc));
        }
        if (end)
            break;
    }
    return n;
}

int lm_audio_parse_profiles(const char *text, struct lm_audio_profile *out, int max)
{
    int n = 0;
    struct lm_audio_profile cur;
    bool have = false;
    enum { NONE, INDEX, NAME, DESC, AVAIL } want = NONE;
    const char *p = text;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[256];
        snprintf(line, sizeof(line), "%.*s", (int)len, p);
        p = eol ? eol + 1 : p + len;
        const char *v;
        if ((v = after_prefix(line, "Object:"))) {
            if (have && n < max)
                out[n++] = cur;
            memset(&cur, 0, sizeof(cur));
            cur.index = -1;
            have = true;
            want = NONE;
        } else if (strstr(line, "Prop: key") && strstr(line, "Profile:index")) {
            want = INDEX;
        } else if (strstr(line, "Prop: key") && strstr(line, "Profile:name")) {
            want = NAME;
        } else if (strstr(line, "Prop: key") && strstr(line, "Profile:description")) {
            want = DESC;
        } else if (strstr(line, "Prop: key") && strstr(line, "Profile:available")) {
            want = AVAIL;
        } else if (strstr(line, "Prop: key")) {
            want = NONE;
        } else if (have && want == INDEX && (v = after_prefix(line, "Int "))) {
            cur.index = atoi(v);
            want = NONE;
        } else if (have && want == NAME && strstr(line, "String")) {
            quoted(line, cur.name, sizeof(cur.name));
            want = NONE;
        } else if (have && want == DESC && strstr(line, "String")) {
            quoted(line, cur.desc, sizeof(cur.desc));
            want = NONE;
        } else if (have && want == AVAIL && (v = after_prefix(line, "Id "))) {
            cur.available = !strstr(line, "ParamAvailability:no");        /* yes and unknown both count */
            want = NONE;
        }
    }
    if (have && n < max)
        out[n++] = cur;
    int m = 0;
    for (int i = 0; i < n; i++)                                           /* only complete ones */
        if (out[i].index >= 0 && out[i].name[0]) {
            if (m != i)
                out[m] = out[i];
            m++;
        }
    return m;
}

int lm_audio_parse_current_profile(const char *text)
{
    bool want = false;
    const char *p = text;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[256];
        snprintf(line, sizeof(line), "%.*s", (int)len, p);
        p = eol ? eol + 1 : p + len;
        const char *v;
        if (strstr(line, "Prop: key"))
            want = strstr(line, "Profile:index") != NULL;
        else if (want && (v = after_prefix(line, "Int ")))
            return atoi(v);
    }
    return -1;
}

/* "output:hdmi-stereo+input:iec958-stereo" -> "hdmi-stereo" (empty when there is no output part). */
static void output_type(const char *name, char *out, size_t n)
{
    out[0] = 0;
    const char *o = strstr(name, "output:");
    if (!o)
        return;
    o += 7;
    snprintf(out, n, "%.*s", (int)strcspn(o, "+"), o);
}

int lm_audio_pick_profiles(const struct lm_audio_profile *in, int n, int current_index, int *picked, int max, int *current_pick)
{
    int m = 0;
    char cur_type[48] = "";
    for (int i = 0; i < n; i++)
        if (in[i].index == current_index)
            output_type(in[i].name, cur_type, sizeof(cur_type));
    if (current_pick)
        *current_pick = -1;
    char seen[16][48];
    int nseen = 0;
    for (int i = 0; i < n && m < max; i++) {
        char type[48];
        output_type(in[i].name, type, sizeof(type));
        if (!type[0] || strstr(type, "surround"))
            continue;
        bool done = false;
        for (int k = 0; k < nseen; k++)
            done = done || !strcmp(seen[k], type);
        if (done || nseen >= 16)
            continue;
        /* the best profile for this output type: the one with nothing else in it, else the first available one */
        int best = -1;
        char pure[64];
        snprintf(pure, sizeof(pure), "output:%s", type);
        for (int j = 0; j < n; j++) {
            char t2[48];
            output_type(in[j].name, t2, sizeof(t2));
            if (strcmp(t2, type) || !in[j].available)
                continue;
            if (!strcmp(in[j].name, pure)) {
                best = j;
                break;
            }
            if (best < 0)
                best = j;
        }
        snprintf(seen[nseen++], 48, "%s", type);
        if (best < 0)
            continue;                                                   /* nothing of this type is available (unplugged) */
        if (!strcmp(type, cur_type) && current_pick)
            *current_pick = m;
        picked[m++] = best;
    }
    return m;
}

static char g_profile_buf[65536];                                       /* pw-cli prints ~700 bytes per profile */

int lm_audio_list_profiles(char *list, size_t n, char *current, size_t cn)
{
    if (n)
        list[0] = 0;
    if (cn)
        current[0] = 0;
    if (!in_path("pw-cli"))
        return 0;
    char *const ls_cmd[] = { "pw-cli", "ls", "Device", NULL };
    if (!run_capture(ls_cmd, g_profile_buf, sizeof(g_profile_buf), 1500))
        return 0;
    struct lm_audio_card cards[8];
    int nc = lm_audio_parse_cards(g_profile_buf, cards, 8), total = 0;
    for (int c = 0; c < nc; c++) {
        char id[16];
        snprintf(id, sizeof(id), "%d", cards[c].id);
        char *const enum_cmd[] = { "pw-cli", "enum-params", id, "EnumProfile", NULL };
        if (!run_capture(enum_cmd, g_profile_buf, sizeof(g_profile_buf), 1500))
            continue;
        static struct lm_audio_profile profs[64];
        int np = lm_audio_parse_profiles(g_profile_buf, profs, 64);
        char *const cur_cmd[] = { "pw-cli", "enum-params", id, "Profile", NULL };
        int cur_index = -1;
        if (run_capture(cur_cmd, g_profile_buf, sizeof(g_profile_buf), 1500))
            cur_index = lm_audio_parse_current_profile(g_profile_buf);
        int picked[8], cur_pick;
        int m = lm_audio_pick_profiles(profs, np, cur_index, picked, 8, &cur_pick);
        for (int i = 0; i < m; i++) {
            char name[24], desc[160];
            snprintf(name, sizeof(name), "%d:%d", cards[c].id, profs[picked[i]].index);
            snprintf(desc, sizeof(desc), "%s - %s", cards[c].desc, profs[picked[i]].desc);
            add_entry(list, n, name, desc);
            if (i == cur_pick && cn)
                snprintf(current, cn, "%s", name);
            total++;
        }
    }
    return total;
}

bool lm_audio_set_profile(const char *entry)
{
    int card, index;
    char junk;
    if (!entry || sscanf(entry, "%d:%d%c", &card, &index, &junk) != 2 || card < 0 || index < 0 || !in_path("pw-cli"))
        return false;
    char id[16], param[48];
    snprintf(id, sizeof(id), "%d", card);
    snprintf(param, sizeof(param), "{ index: %d }", index);
    char *const set_cmd[] = { "pw-cli", "set-param", id, "Profile", param, NULL };
    run_capture(set_cmd, g_profile_buf, sizeof(g_profile_buf), 2000);
    char *const cur_cmd[] = { "pw-cli", "enum-params", id, "Profile", NULL };
    if (!run_capture(cur_cmd, g_profile_buf, sizeof(g_profile_buf), 1500))
        return false;
    return lm_audio_parse_current_profile(g_profile_buf) == index;
}

/* A profile entry "43:6" is only good until PipeWire restarts (ids change); the lasting form is "card description|profile name". */
bool lm_audio_profile_key(const char *entry, char *out, size_t n)
{
    int card, index;
    char junk;
    if (n)
        out[0] = 0;
    if (!entry || !n || sscanf(entry, "%d:%d%c", &card, &index, &junk) != 2 || !in_path("pw-cli"))
        return false;
    char *const ls_cmd[] = { "pw-cli", "ls", "Device", NULL };
    if (!run_capture(ls_cmd, g_profile_buf, sizeof(g_profile_buf), 1500))
        return false;
    struct lm_audio_card cards[8];
    int nc = lm_audio_parse_cards(g_profile_buf, cards, 8);
    for (int c = 0; c < nc; c++) {
        if (cards[c].id != card)
            continue;
        char id[16];
        snprintf(id, sizeof(id), "%d", card);
        char *const enum_cmd[] = { "pw-cli", "enum-params", id, "EnumProfile", NULL };
        if (!run_capture(enum_cmd, g_profile_buf, sizeof(g_profile_buf), 1500))
            return false;
        static struct lm_audio_profile profs[64];
        int np = lm_audio_parse_profiles(g_profile_buf, profs, 64);
        for (int i = 0; i < np; i++)
            if (profs[i].index == index && !strchr(profs[i].name, '|') && !strchr(cards[c].desc, '\n')) {
                snprintf(out, n, "%s|%s", cards[c].desc, profs[i].name);
                return true;
            }
        return false;
    }
    return false;
}

int lm_audio_apply_profile_key(const char *key)
{
    const char *bar = key ? strrchr(key, '|') : NULL;
    if (!bar || bar == key || !bar[1])
        return 1;                                       /* nothing saved: nothing to do */
    if (!in_path("pw-cli"))
        return -1;
    char desc[96];
    snprintf(desc, sizeof(desc), "%.*s", (int)LM_MIN((size_t)(bar - key), sizeof(desc) - 1), key);
    char *const ls_cmd[] = { "pw-cli", "ls", "Device", NULL };
    if (!run_capture(ls_cmd, g_profile_buf, sizeof(g_profile_buf), 1500))
        return -1;                                      /* no PipeWire yet */
    struct lm_audio_card cards[8];
    int nc = lm_audio_parse_cards(g_profile_buf, cards, 8);
    for (int c = 0; c < nc; c++) {
        if (strcmp(cards[c].desc, desc))
            continue;
        char id[16];
        snprintf(id, sizeof(id), "%d", cards[c].id);
        char *const enum_cmd[] = { "pw-cli", "enum-params", id, "EnumProfile", NULL };
        if (!run_capture(enum_cmd, g_profile_buf, sizeof(g_profile_buf), 1500))
            return -1;
        static struct lm_audio_profile profs[64];
        int np = lm_audio_parse_profiles(g_profile_buf, profs, 64);
        char *const cur_cmd[] = { "pw-cli", "enum-params", id, "Profile", NULL };
        int cur = -1;
        if (run_capture(cur_cmd, g_profile_buf, sizeof(g_profile_buf), 1500))
            cur = lm_audio_parse_current_profile(g_profile_buf);
        for (int i = 0; i < np; i++) {
            if (strcmp(profs[i].name, bar + 1))
                continue;
            if (profs[i].index == cur)
                return 0;                               /* already the one */
            char entry[24];
            snprintf(entry, sizeof(entry), "%d:%d", cards[c].id, profs[i].index);
            return lm_audio_set_profile(entry) ? 0 : -1;
        }
        return 1;                                       /* this card has no such profile any more */
    }
    return -1;                                          /* the card is not there (yet) */
}

/* ---- asynchronous processes (mixer) ---------------------------------------------------------------------------- */

#define PROC_OUT_MAX 16384

struct lm_proc {
    pid_t pid;
    int fd;
    struct lm_source *source;
    struct lm_timer *timer;
    char *buf;
    size_t len;
    lm_proc_fn fn;
    void *data;
};

static void proc_finish(struct lm_proc *p, bool ok)
{
    if (p->source)
        lm_source_remove(p->source);
    if (p->timer)
        lm_timer_destroy(p->timer);
    if (p->fd >= 0)
        close(p->fd);
    if (!ok)
        kill(p->pid, SIGKILL);                          /* timed out: it is our child, the SIGCHLD watch reaps it */
    if (p->fn)
        p->fn(p->data, ok ? p->buf : "");
    free(p->buf);
    free(p);
}

static void proc_fd_cb(int fd, uint32_t events, void *data)
{
    struct lm_proc *p = data;
    (void)events;
    for (;;) {
        char tmp[2048];
        ssize_t k = read(fd, tmp, sizeof(tmp));
        if (k > 0) {
            if (p->len + (size_t)k < PROC_OUT_MAX) {
                memcpy(p->buf + p->len, tmp, (size_t)k);
                p->len += (size_t)k;
                p->buf[p->len] = 0;
            }
            continue;
        }
        if (k < 0 && (errno == EAGAIN || errno == EINTR))
            return;
        proc_finish(p, true);                           /* EOF (or an error): the program is done */
        return;
    }
}

static void proc_timer_cb(void *data)
{
    proc_finish(data, false);
}

struct lm_proc *lm_proc_run(struct lm_loop *loop, char *const argv[], int timeout_ms, lm_proc_fn fn, void *data)
{
    int fds[2];
    if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) < 0)
        return NULL;
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return NULL;
    }
    if (pid == 0) {
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);          /* the loop blocks signals for signalfd */
        signal(SIGPIPE, SIG_DFL);
        setsid();
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) {
            dup2(null, 0);
            dup2(null, 2);
        }
        dup2(fds[1], 1);                                /* (clears O_CLOEXEC / O_NONBLOCK on the copy) */
        setenv("LC_ALL", "C", 1);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(fds[1]);
    struct lm_proc *p = calloc(1, sizeof(*p));
    char *buf = malloc(PROC_OUT_MAX);
    if (!p || !buf) {
        free(p);
        free(buf);
        close(fds[0]);
        kill(pid, SIGKILL);
        return NULL;
    }
    buf[0] = 0;
    p->pid = pid;
    p->fd = fds[0];
    p->buf = buf;
    p->fn = fn;
    p->data = data;
    p->source = lm_loop_add_fd(loop, p->fd, EPOLLIN, proc_fd_cb, p);
    p->timer = lm_loop_add_timer(loop, proc_timer_cb, p);
    if (!p->source || !p->timer) {
        p->fn = NULL;
        proc_finish(p, false);
        return NULL;
    }
    lm_timer_arm_ms(p->timer, (uint32_t)(timeout_ms > 0 ? timeout_ms : 2000), 0);
    return p;
}

void lm_proc_cancel(struct lm_proc *p)
{
    if (!p)
        return;
    p->fn = NULL;
    proc_finish(p, false);
}
