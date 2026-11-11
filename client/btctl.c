#include "btctl.h"

#include <ctype.h>
#include <errno.h>
#include <glob.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "util.h"

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void copy(char *dst, size_t n, const char *src)
{
    size_t len = src ? strnlen(src, n - 1) : 0;
    if (len)
        memcpy(dst, src, len);
    dst[len] = 0;
}

/* Runs a program (argv[0] looked up in PATH), its output (stdout and stderr) into out; time limit in ms; returns the exit status, -1 when it
 * could not run, -2 on a timeout. LC_ALL=C so that the messages are the English ones. */
static int run_cmd(char *const argv[], char *out, size_t n, int timeout_ms)
{
    int pfd[2];
    if (out && n)
        out[0] = 0;
    if (pipe(pfd) < 0)
        return -1;
    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return -1;
    }
    if (pid == 0) {
        setsid();
        int nul = open("/dev/null", O_RDONLY);
        if (nul >= 0)
            dup2(nul, 0);
        dup2(pfd[1], 1);
        dup2(pfd[1], 2);
        close(pfd[0]);
        close(pfd[1]);
        setenv("LC_ALL", "C", 1);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(pfd[1]);
    size_t len = 0;
    uint64_t end = now_ms() + (uint64_t)timeout_ms;
    bool timed_out = false;
    for (;;) {
        int left = (int)(end - now_ms());
        if (now_ms() >= end) {
            timed_out = true;
            break;
        }
        struct pollfd p = { .fd = pfd[0], .events = POLLIN };
        int r = poll(&p, 1, left);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            continue;
        char buf[1024];
        ssize_t k = read(pfd[0], buf, sizeof(buf));
        if (k <= 0)
            break;
        if (out && len + (size_t)k < n) {
            memcpy(out + len, buf, (size_t)k);
            len += (size_t)k;
        }
    }
    close(pfd[0]);
    if (out && n)
        out[len < n ? len : n - 1] = 0;
    if (timed_out) {
        kill(-pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return -2;
    }
    int st = 0;
    waitpid(pid, &st, 0);
    if (WIFEXITED(st))
        return WEXITSTATUS(st) == 127 && len == 0 ? -1 : WEXITSTATUS(st);
    return -1;
}

int bt_battery(const char *text)
{
    const char *p = strstr(text, "Battery Percentage:");
    if (!p)
        return -1;
    const char *paren = strchr(p, '(');
    const char *nl = strchr(p, '\n');
    int v = paren && (!nl || paren < nl) ? atoi(paren + 1) : (int)strtol(p + 19, NULL, 0);
    return v >= 0 && v <= 100 ? v : -1;
}

bool bt_audio_plugin_missing(void)
{
    const char *dir = getenv("MELLAGUI_SPA_DIR");
    char path[400];
    if (dir && *dir) {
        snprintf(path, sizeof(path), "%s/bluez5/libspa-bluez5.so", dir);
        return access(path, R_OK) != 0;
    }
    static const char *const pat[] = { "/usr/lib/*/spa-0.2/bluez5/libspa-bluez5.so", "/usr/lib/spa-0.2/bluez5/libspa-bluez5.so",
                                       "/usr/lib64/spa-0.2/bluez5/libspa-bluez5.so", "/usr/local/lib/*/spa-0.2/bluez5/libspa-bluez5.so" };
    for (size_t i = 0; i < sizeof(pat) / sizeof(pat[0]); i++) {
        glob_t g;
        int rc = glob(pat[i], 0, NULL, &g);
        globfree(&g);
        if (rc == 0)
            return false;
    }
    return true;
}

const char *bt_explain(const char *text)
{
    if (!text)
        return NULL;
    if (strstr(text, "profile-unavailable") || strstr(text, "Protocol not available"))
        return bt_audio_plugin_missing()
                   ? "No Bluetooth audio support in the sound system: sudo apt install libspa-0.2-bluetooth, then restart PipeWire"
                   : "The device offers nothing this computer can use (no audio profile): restart PipeWire / WirePlumber and try again";
    if (strstr(text, "AuthenticationFailed") || strstr(text, "AuthenticationRejected") || strstr(text, "AuthenticationCanceled"))
        return "Pairing was refused: wrong code, or the device left pairing mode";
    if (strstr(text, "Page Timeout") || strstr(text, "host-is-down") || strstr(text, "ConnectionAttemptFailed") || strstr(text, "page-timeout"))
        return "The device did not answer: switch it on, bring it closer or put it in pairing mode";
    if (strstr(text, "AlreadyExists"))
        return "Already paired: use Connect";
    if (strstr(text, "InProgress"))
        return "Busy with another step: wait a moment";
    if (strstr(text, "Blocked") || strstr(text, "blocked"))
        return "The radio is blocked: rfkill unblock bluetooth";
    return NULL;
}

void bt_strip(char *out)
{
    char *w = out;
    for (const char *r = out; *r; r++) {
        if (*r == 0x1b && r[1] == '[') {
            r += 2;
            while (*r && !isalpha((unsigned char)*r) && *r != '~')
                r++;
            if (!*r)
                break;
            continue;
        }
        if (*r != '\r')
            *w++ = *r;
    }
    *w = 0;
}


const char *bluetoothctl_program(void)
{
    const char *p = getenv("MELLAGUI_BLUETOOTHCTL");
    return p && *p ? p : "bluetoothctl";
}

int btctl(char *out, size_t n, int timeout_ms, ...)
{
    char *argv[16];
    int k = 0;
    argv[k++] = (char *)bluetoothctl_program();
    va_list ap;
    va_start(ap, timeout_ms);
    const char *a;
    while (k < 14 && (a = va_arg(ap, const char *)) != NULL)
        argv[k++] = (char *)a;
    va_end(ap);
    argv[k] = NULL;
    int rc = run_cmd(argv, out, n, timeout_ms);
    if (out && n)
        bt_strip(out);
    return rc;
}

bool mac_ok(const char *s)
{
    if (strlen(s) != 17)
        return false;
    for (int i = 0; i < 17; i++)
        if (i % 3 == 2 ? s[i] != ':' : !isxdigit((unsigned char)s[i]))
            return false;
    return true;
}

/* "Device AA:BB:CC:DD:EE:FF Some name" lines -> devices (name = the rest of the line). */
int parse_bt_devices(const char *text, struct btdev *out, int max)
{
    int n = 0;
    for (const char *p = text; *p && n < max;) {
        size_t l = strcspn(p, "\n");
        char line[160];
        snprintf(line, sizeof(line), "%.*s", (int)LM_MIN(l, sizeof(line) - 1), p);
        p += l + (p[l] ? 1 : 0);
        char *s = line;
        while (*s == ' ' || *s == '\t')
            s++;
        if (strncmp(s, "Device ", 7) != 0)
            continue;
        s += 7;
        char mac[18];
        snprintf(mac, sizeof(mac), "%.17s", s);
        if (!mac_ok(mac))
            continue;
        struct btdev *d = &out[n++];
        memset(d, 0, sizeof(*d));
        d->battery = -1;
        copy(d->mac, sizeof(d->mac), mac);
        s += 17;
        while (*s == ' ')
            s++;
        copy(d->name, sizeof(d->name), *s ? s : mac);
    }
    return n;
}

/* The value of "\tKey: value" in `bluetoothctl show` / `info` output. */
bool bt_field(const char *text, const char *key, char *out, size_t n)
{
    size_t kl = strlen(key);
    for (const char *p = text; *p;) {
        const char *q = p;
        while (*q == ' ' || *q == '\t')
            q++;
        size_t l = strcspn(q, "\n");
        if (!strncmp(q, key, kl) && q[kl] == ':') {
            const char *v = q + kl + 1;
            while (*v == ' ')
                v++;
            snprintf(out, n, "%.*s", (int)LM_MIN(strcspn(v, "\n"), n - 1), v);
            return true;
        }
        p = q + l + (q[l] ? 1 : 0);
    }
    return false;
}

bool bt_yes(const char *text, const char *key)
{
    char v[16];
    return bt_field(text, key, v, sizeof(v)) && !strncmp(v, "yes", 3);
}

void read_bt(struct btinfo *bi)
{
    static char text[8192];
    memset(bi, 0, sizeof(*bi));
    if (btctl(text, sizeof(text), 3000, "show", NULL) != 0 || !strstr(text, "Controller"))
        return;
    bi->have = true;
    bi->powered = bt_yes(text, "Powered");
    if (!bi->powered)
        return;
    struct btdev all[MAX_BT];
    int n = btctl(text, sizeof(text), 3000, "devices", NULL) == 0 ? parse_bt_devices(text, all, MAX_BT) : 0;
    struct btdev paired[MAX_BT];
    int np = 0;
    if (btctl(text, sizeof(text), 3000, "devices", "Paired", NULL) == 0)                  /* bluez 5.65 and newer (5.72 checked) */
        np = parse_bt_devices(text, paired, MAX_BT);
    if (np == 0 && btctl(text, sizeof(text), 3000, "paired-devices", NULL) == 0)          /* older: removed in newer versions, where it fails */
        np = parse_bt_devices(text, paired, MAX_BT);
    for (int i = 0; i < np; i++) {                              /* a paired device that `devices` did not list is added */
        bool seen = false;
        for (int j = 0; j < n; j++)
            seen |= !strcmp(all[j].mac, paired[i].mac);
        if (!seen && n < MAX_BT)
            all[n++] = paired[i];
    }
    for (int i = 0; i < n; i++)
        for (int j = 0; j < np; j++)
            if (!strcmp(all[i].mac, paired[j].mac))
                all[i].paired = true;
    int infos = 0;
    for (int i = 0; i < n; i++) {                               /* who is connected: `info` of the paired ones */
        if (!all[i].paired || infos++ >= 12)
            continue;
        if (btctl(text, sizeof(text), 3000, "info", all[i].mac, NULL) == 0) {
            all[i].connected = bt_yes(text, "Connected");
            all[i].battery = all[i].connected ? bt_battery(text) : -1;
        }
    }
    for (int pass = 0; pass < 3; pass++)                        /* connected, then paired, then the rest */
        for (int i = 0; i < n && bi->n < MAX_BT; i++) {
            if (!all[i].paired && !all[i].connected) {          /* a scan fills the list with beacons that have no name (it is their address): leave them out */
                char dashed[18];
                memcpy(dashed, all[i].mac, 18);
                for (char *c = dashed; *c; c++)
                    if (*c == ':')
                        *c = '-';
                if (!strcmp(all[i].name, dashed) || !strcmp(all[i].name, all[i].mac))
                    continue;
            }
            int rank = all[i].connected ? 0 : all[i].paired ? 1 : 2;
            if (rank == pass)
                bi->dev[bi->n++] = all[i];
        }
    for (int i = 0; i < bi->n; i++)
        if (bi->dev[i].connected && bi->nconn++ == 0) {
            copy(bi->first, sizeof(bi->first), bi->dev[i].name);
            bi->first_battery = bi->dev[i].battery;
        }
}

