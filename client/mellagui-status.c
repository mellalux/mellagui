/*
 * mellagui-status - the battery and the network in the panel.
 *
 *   mellagui-status                    runs in the background (the compositor starts it): two buttons in the top bar through mellagui_tray_v1,
 *                                      the battery (icon and percentage, only when there is one) and the network (Wi-Fi strength or Ethernet and
 *                                      the name); a click opens a menu (mellagui_menu_v1): the battery's details, or the Wi-Fi networks to connect to
 *   mellagui-status --password SSID    a small window that asks the password of a new Wi-Fi network and connects
 *   mellagui-status --list             the battery and the network as text
 *   mellagui-status --no-battery / --no-network    leave one of the buttons out
 *   mellagui-status --selftest
 *
 * Battery: /sys/class/power_supply ($MELLAGUI_POWER_ROOT replaces it; the main battery is the one that is not a "Device" (mouse, keyboard), the
 * others are listed in the menu). Network: NetworkManager's `nmcli` ($MELLAGUI_NMCLI names another program), always with LC_ALL=C and
 * the terse output; nothing is changed except by a click on a menu entry. A password goes to nmcli through a pty (`nmcli --ask`), never on
 * a command line. A low battery (15 %, 5 %) shows a notification.
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "font.h"
#include "keymap.h"
#include "protocol.h"
#include "theme.h"
#include "util.h"
#include "wlc.h"
#include "wlwin.h"

#define MAX_BAT 8
#define MAX_NETS 32
#define MENU_ENTRIES 12                 /* LM_MENU_MAX */
#define BAT_POLL_MS 20000
#define NET_POLL_MS 5000

/* ---- small helpers ------------------------------------------------------------------------------------------------------ */

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

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = 0;
}

/* The text of a small file, trimmed; false when it cannot be read. */
static bool read_file(const char *path, char *out, size_t n)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    size_t got = fread(out, 1, n - 1, f);
    fclose(f);
    out[got] = 0;
    trim(out);
    return true;
}

static long read_long(const char *dir, const char *name, long def)
{
    char path[512], buf[64];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (!read_file(path, buf, sizeof(buf)) || !buf[0])
        return def;
    char *end;
    long v = strtol(buf, &end, 10);
    return end == buf ? def : v;
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

/* Splits one line of nmcli's terse output at the unescaped colons ("\:" is a colon, "\\" a backslash); returns the number of fields. */
static int terse_split(const char *line, char fields[][96], int max)
{
    int n = 0;
    size_t o = 0;
    if (max <= 0)
        return 0;
    fields[0][0] = 0;
    for (const char *p = line; *p; p++) {
        if (*p == '\\' && p[1]) {
            p++;
            if (o + 1 < 96)
                fields[n][o++] = *p;
        } else if (*p == ':') {
            fields[n][o] = 0;
            if (++n >= max)
                return n;
            o = 0;
            fields[n][0] = 0;
        } else if (o + 1 < 96) {
            fields[n][o++] = *p;
        }
    }
    fields[n][o] = 0;
    return n + 1;
}

/* ---- the battery ---------------------------------------------------------------------------------------------------------- */

struct battery {
    char name[40];
    char model[48];
    char status[24];                    /* Charging, Discharging, Full, Not charging, Unknown */
    bool system;                        /* the computer's own battery (not a mouse or a keyboard) */
    int percent;                        /* -1 = unknown */
    int minutes;                        /* until empty / full, -1 = unknown */
};

static const char *power_root(void)
{
    const char *r = getenv("MELLAGUI_POWER_ROOT");
    return r && *r ? r : "/sys/class/power_supply";
}

static bool bat_charging(const struct battery *b) { return !strcasecmp(b->status, "Charging"); }

/* The batteries of `root`, the computer's own first. */
static int read_batteries(const char *root, struct battery *out, int max)
{
    DIR *d = opendir(root);
    if (!d)
        return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        if (e->d_name[0] == '.')
            continue;
        char dir[512], buf[640];
        snprintf(dir, sizeof(dir), "%s/%s", root, e->d_name);
        snprintf(buf, sizeof(buf), "%s/type", dir);
        char type[32];
        if (!read_file(buf, type, sizeof(type)) || strcasecmp(type, "Battery"))
            continue;
        if (read_long(dir, "present", 1) == 0)
            continue;
        struct battery *b = &out[n];
        memset(b, 0, sizeof(*b));
        copy(b->name, sizeof(b->name), e->d_name);
        snprintf(buf, sizeof(buf), "%s/model_name", dir);
        if (!read_file(buf, b->model, sizeof(b->model)))
            b->model[0] = 0;
        snprintf(buf, sizeof(buf), "%s/status", dir);
        if (!read_file(buf, b->status, sizeof(b->status)))
            copy(b->status, sizeof(b->status), "Unknown");
        char scope[24] = "";
        snprintf(buf, sizeof(buf), "%s/scope", dir);
        bool has_scope = read_file(buf, scope, sizeof(scope));
        b->system = has_scope ? strcasecmp(scope, "Device") != 0 : (!strncasecmp(e->d_name, "BAT", 3) || !strncasecmp(e->d_name, "CMB", 3));

        long cap = read_long(dir, "capacity", -1);
        long e_now = read_long(dir, "energy_now", -1), e_full = read_long(dir, "energy_full", -1), power = read_long(dir, "power_now", -1);
        long c_now = read_long(dir, "charge_now", -1), c_full = read_long(dir, "charge_full", -1), cur = read_long(dir, "current_now", -1);
        long now = e_now >= 0 ? e_now : c_now, full = e_full >= 0 ? e_full : c_full, rate = e_now >= 0 ? power : cur;
        if (cap < 0 && now >= 0 && full > 0)
            cap = now * 100 / full;
        if (cap < 0) {                                          /* a mouse: only a level */
            char lvl[24];
            snprintf(buf, sizeof(buf), "%s/capacity_level", dir);
            if (read_file(buf, lvl, sizeof(lvl))) {
                cap = !strcasecmp(lvl, "Full") ? 100 : !strcasecmp(lvl, "High") ? 80 : !strcasecmp(lvl, "Normal") ? 50 :
                      !strcasecmp(lvl, "Low") ? 20 : !strcasecmp(lvl, "Critical") ? 5 : -1;
            }
        }
        b->percent = cap > 100 ? 100 : (int)cap;
        b->minutes = -1;
        if (now >= 0 && rate > 0) {
            if (!strcasecmp(b->status, "Discharging"))
                b->minutes = (int)(now * 60 / rate);
            else if (bat_charging(b) && full > now)
                b->minutes = (int)((full - now) * 60 / rate);
        }
        n++;
    }
    closedir(d);
    for (int i = 1; i < n; i++)                                 /* the computer's own battery first, then by name */
        for (int j = i; j > 0 && ((out[j].system && !out[j - 1].system) ||
                                  (out[j].system == out[j - 1].system && strcmp(out[j].name, out[j - 1].name) < 0)); j--) {
            struct battery t = out[j];
            out[j] = out[j - 1];
            out[j - 1] = t;
        }
    return n;
}

static void fmt_minutes(char *out, size_t n, int m)
{
    if (m >= 60)
        snprintf(out, n, "%d h %02d min", m / 60, m % 60);
    else
        snprintf(out, n, "%d min", m);
}

/* ---- the network (nmcli) ------------------------------------------------------------------------------------------------------ */

enum net_kind { NET_NONE, NET_WIFI, NET_ETHERNET };

struct wifi {
    char ssid[64];
    int signal;
    bool secured, active, known;
};

struct netinfo {
    bool have_nm;                       /* nmcli runs */
    bool wifi_present;                  /* a Wi-Fi adapter exists */
    bool wifi_enabled;                  /* ... and the radio is on */
    enum net_kind kind;                 /* what is connected (Ethernet before Wi-Fi) */
    char dev[32];
    char conn[64];                      /* the connection's name */
    char ssid[64];
    int signal;                         /* Wi-Fi: 0..100 */
    char ip[48];
};

static const char *nmcli_program(void)
{
    const char *p = getenv("MELLAGUI_NMCLI");
    return p && *p ? p : "nmcli";
}

static int nmcli(char *out, size_t n, int timeout_ms, ...)
{
    char *argv[24];
    int k = 0;
    argv[k++] = (char *)nmcli_program();
    va_list ap;
    va_start(ap, timeout_ms);
    const char *a;
    while (k < 22 && (a = va_arg(ap, const char *)) != NULL)
        argv[k++] = (char *)a;
    va_end(ap);
    argv[k] = NULL;
    return run_cmd(argv, out, n, timeout_ms);
}

/* The Wi-Fi networks around (strongest per name), the active one first, then by strength. `dev` NULL = any adapter. */
static int wifi_list(const char *dev, struct wifi *out, int max)
{
    static char text[16384];
    int rc = dev && *dev ? nmcli(text, sizeof(text), 8000, "-t", "-f", "IN-USE,SSID,SIGNAL,SECURITY", "dev", "wifi", "list", "ifname", dev, "--rescan", "no", NULL)
                         : nmcli(text, sizeof(text), 8000, "-t", "-f", "IN-USE,SSID,SIGNAL,SECURITY", "dev", "wifi", "list", "--rescan", "no", NULL);
    if (rc != 0)
        return 0;
    static char known[8192];
    known[0] = 0;
    if (nmcli(known, sizeof(known), 5000, "-t", "-f", "NAME,TYPE", "connection", "show", NULL) != 0)
        known[0] = 0;
    int n = 0;
    for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
        char f[4][96];
        if (terse_split(line, f, 4) < 3 || !f[1][0])
            continue;
        struct wifi w = { .signal = atoi(f[2]), .active = f[0][0] == '*', .secured = f[3][0] && strcmp(f[3], "--") != 0 };
        copy(w.ssid, sizeof(w.ssid), f[1]);
        int at = -1;
        for (int i = 0; i < n; i++)
            if (!strcmp(out[i].ssid, w.ssid))
                at = i;
        if (at >= 0) {
            if (w.active || w.signal > out[at].signal) {
                bool act = out[at].active || w.active;
                out[at] = w;
                out[at].active = act;
            }
            continue;
        }
        if (n < max)
            out[n++] = w;
    }
    char kcopy[8192];
    snprintf(kcopy, sizeof(kcopy), "%s", known);
    for (char *line = strtok(kcopy, "\n"); line; line = strtok(NULL, "\n")) {
        char f[2][96];
        if (terse_split(line, f, 2) < 2 || (strcmp(f[1], "802-11-wireless") && strcmp(f[1], "wifi")))
            continue;
        for (int i = 0; i < n; i++)
            if (!strcmp(out[i].ssid, f[0]))
                out[i].known = true;
    }
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && ((out[j].active && !out[j - 1].active) ||
                                  (out[j].active == out[j - 1].active && out[j].signal > out[j - 1].signal)); j--) {
            struct wifi t = out[j];
            out[j] = out[j - 1];
            out[j - 1] = t;
        }
    return n;
}

/* What is connected now. */
static void read_net(struct netinfo *ni)
{
    static char text[8192];
    memset(ni, 0, sizeof(*ni));
    int rc = nmcli(text, sizeof(text), 5000, "-t", "-f", "DEVICE,TYPE,STATE,CONNECTION", "dev", NULL);
    if (rc != 0)
        return;
    ni->have_nm = true;
    for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
        char f[4][96];
        if (terse_split(line, f, 4) < 3)
            continue;
        bool wifi = !strcmp(f[1], "wifi"), eth = !strcmp(f[1], "ethernet");
        if (wifi)
            ni->wifi_present = true;
        if ((!wifi && !eth) || strncmp(f[2], "connected", 9) != 0 || !strcmp(f[2], "connected (externally)"))
            continue;
        if (ni->kind == NET_ETHERNET || (ni->kind == NET_WIFI && !eth))
            continue;
        ni->kind = eth ? NET_ETHERNET : NET_WIFI;
        copy(ni->dev, sizeof(ni->dev), f[0]);
        copy(ni->conn, sizeof(ni->conn), f[3]);
    }
    char radio[32];
    ni->wifi_enabled = ni->wifi_present && nmcli(radio, sizeof(radio), 3000, "radio", "wifi", NULL) == 0 && !strncmp(radio, "enabled", 7);
    if (ni->kind == NET_WIFI) {
        struct wifi nets[MAX_NETS];
        int n = wifi_list(ni->dev, nets, MAX_NETS);
        for (int i = 0; i < n; i++)
            if (nets[i].active) {
                copy(ni->ssid, sizeof(ni->ssid), nets[i].ssid);
                ni->signal = nets[i].signal;
            }
        if (!ni->ssid[0])
            copy(ni->ssid, sizeof(ni->ssid), ni->conn);
    }
    if (ni->kind != NET_NONE) {
        char ip[256];
        if (nmcli(ip, sizeof(ip), 3000, "-t", "-f", "IP4.ADDRESS", "dev", "show", ni->dev, NULL) == 0) {
            char f[2][96];
            char *line = strtok(ip, "\n");
            if (line && terse_split(line, f, 2) >= 2) {
                char *slash = strchr(f[1], '/');
                if (slash)
                    *slash = 0;
                copy(ni->ip, sizeof(ni->ip), f[1]);
            }
        }
    }
}

/* ---- connecting with a password (nmcli --ask on a pty) ------------------------------------------------------------------------- */

/* Connects to a new secured network: nmcli asks for the password on its terminal, which is answered here, so the password is never on a command
 * line. Returns true on success; `msg` gets nmcli's last line. */
static bool connect_with_password(const char *ssid, const char *password, char *msg, size_t n)
{
    msg[0] = 0;
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || grantpt(master) < 0 || unlockpt(master) < 0) {
        copy(msg, n, "no terminal available");
        if (master >= 0)
            close(master);
        return false;
    }
    char *slave_name = ptsname(master);
    if (!slave_name) {
        close(master);
        copy(msg, n, "no terminal available");
        return false;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(master);
        return false;
    }
    if (pid == 0) {
        setsid();
        int s = open(slave_name, O_RDWR);
        if (s < 0)
            _exit(126);
#ifdef TIOCSCTTY
        ioctl(s, TIOCSCTTY, 0);
#endif
        dup2(s, 0);
        dup2(s, 1);
        dup2(s, 2);
        if (s > 2)
            close(s);
        close(master);
        setenv("LC_ALL", "C", 1);
        char *argv[] = { (char *)nmcli_program(), "--ask", "--wait", "30", "dev", "wifi", "connect", (char *)ssid, NULL };
        execvp(argv[0], argv);
        _exit(127);
    }
    char buf[2048];
    size_t len = 0;
    bool sent = false;
    uint64_t end = now_ms() + 40000;
    int status = 0;
    bool exited = false;
    while (!exited && now_ms() < end) {
        struct pollfd p = { .fd = master, .events = POLLIN };
        int r = poll(&p, 1, 200);
        if (r > 0 && (p.revents & POLLIN)) {
            char tmp[512];
            ssize_t k = read(master, tmp, sizeof(tmp));
            if (k > 0 && len + (size_t)k < sizeof(buf)) {
                memcpy(buf + len, tmp, (size_t)k);
                len += (size_t)k;
                buf[len] = 0;
            }
            if (!sent && strcasestr(buf, "assword")) {            /* "Password: " */
                sent = true;
                char line[256];
                int m = snprintf(line, sizeof(line), "%s\n", password);
                if (m > 0 && write(master, line, (size_t)m) < 0) {}
            }
        } else if (r > 0 && (p.revents & (POLLHUP | POLLERR))) {
            /* the child closed its side: fall through to waitpid */
        }
        if (waitpid(pid, &status, WNOHANG) == pid)
            exited = true;
    }
    if (!exited) {
        kill(-pid, SIGKILL);
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        copy(msg, n, "timed out");
    }
    for (;;) {                                                  /* what is left in the pipe */
        struct pollfd p = { .fd = master, .events = POLLIN };
        if (poll(&p, 1, 0) <= 0)
            break;
        char tmp[512];
        ssize_t k = read(master, tmp, sizeof(tmp));
        if (k <= 0)
            break;
        if (len + (size_t)k < sizeof(buf)) {
            memcpy(buf + len, tmp, (size_t)k);
            len += (size_t)k;
            buf[len] = 0;
        }
    }
    close(master);
    buf[len] = 0;
    char *last = NULL;                                          /* the last non-empty line, without the password prompt and terminal noise */
    for (char *line = strtok(buf, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        while (*line && (unsigned char)*line < 32)
            line++;
        if (*line && !strcasestr(line, "assword"))
            last = line;
    }
    if (last && !msg[0])
        copy(msg, n, last);
    return exited && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/* ---- icons (premultiplied ARGB, drawn with signed distances) ---------------------------------------------------------------------- */

static float clamp01(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }

/* Coverage of a rounded rectangle at pixel centre (x, y): the box is [x0, x1] x [y0, y1], corner radius r; 1 inside, 0 outside, soft over one pixel. */
static float cov_box(float x, float y, float x0, float y0, float x1, float y1, float r)
{
    float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, hx = (x1 - x0) / 2 - r, hy = (y1 - y0) / 2 - r;
    float qx = fabsf(x - cx) - hx, qy = fabsf(y - cy) - hy;
    float d = hypotf(fmaxf(qx, 0.0f), fmaxf(qy, 0.0f)) + fminf(fmaxf(qx, qy), 0.0f) - r;
    return clamp01(0.5f - d);
}

static void blend(uint32_t *px, uint32_t rgb, float cov, float alpha)
{
    float a = cov * alpha;
    if (a <= 0.0f)
        return;
    uint32_t old = *px, oa = old >> 24;
    uint32_t na = (uint32_t)(a * 255.0f + 0.5f);
    uint32_t r = (uint32_t)((float)((rgb >> 16) & 255) * a + 0.5f), g = (uint32_t)((float)((rgb >> 8) & 255) * a + 0.5f), b = (uint32_t)((float)(rgb & 255) * a + 0.5f);
    uint32_t inv = 255 - na;                                    /* over what is there */
    uint32_t or_ = ((old >> 16) & 255) * inv / 255, og = ((old >> 8) & 255) * inv / 255, ob = (old & 255) * inv / 255;
    uint32_t oaa = oa * inv / 255;
    uint32_t A = LM_MIN(255u, na + oaa), R = LM_MIN(255u, r + or_), G = LM_MIN(255u, g + og), B = LM_MIN(255u, b + ob);
    *px = (A << 24) | (R << 16) | (G << 8) | B;
}

static uint32_t level_colour(int pct)
{
    return pct >= 50 ? lm_theme.ok : pct >= 20 ? lm_theme.warn : lm_theme.alert;
}

/* A battery lying on its side: outline, a fill of `pct`, and a bolt when it charges. */
static void draw_battery(uint32_t *px, int s, int pct, bool charging)
{
    memset(px, 0, (size_t)s * (size_t)s * 4);
    float fs = (float)s;
    float bx0 = fs * 0.06f, bx1 = fs * 0.86f, by0 = fs * 0.28f, by1 = fs * 0.72f, r = fs * 0.08f, line = fmaxf(1.3f, fs * 0.075f);
    uint32_t outline = lm_theme.text, fill = level_colour(pct < 0 ? 50 : pct);
    for (int y = 0; y < s; y++)
        for (int x = 0; x < s; x++) {
            float fx = (float)x + 0.5f, fy = (float)y + 0.5f;
            float outer = cov_box(fx, fy, bx0, by0, bx1, by1, r), inner = cov_box(fx, fy, bx0 + line, by0 + line, bx1 - line, by1 - line, fmaxf(r - line, 0.5f));
            blend(&px[y * s + x], outline, clamp01(outer - inner), 1.0f);
            float fw = (bx1 - line) - (bx0 + line + 1.0f);      /* the fill, from the left, a little in from the outline */
            float fx1 = bx0 + line + 1.0f + fw * (float)(pct < 0 ? 0 : pct) / 100.0f;
            float f = cov_box(fx, fy, bx0 + line + 1.0f, by0 + line + 1.0f, fx1, by1 - line - 1.0f, 0.6f);
            if (pct != 0)
                blend(&px[y * s + x], fill, f * inner, 1.0f);
            blend(&px[y * s + x], outline, cov_box(fx, fy, bx1 + 0.5f, fs * 0.40f, bx1 + fs * 0.09f + 0.5f, fs * 0.60f, 0.8f), 1.0f);     /* the nub */
        }
    if (charging) {                                             /* a bolt: two thin triangles, drawn as a polyline of boxes */
        float cx = (bx0 + bx1) / 2, cy = (by0 + by1) / 2;
        float pts[5][2] = { { cx + fs * 0.06f, cy - fs * 0.30f }, { cx - fs * 0.10f, cy + fs * 0.02f }, { cx + fs * 0.02f, cy + fs * 0.02f },
                            { cx - fs * 0.06f, cy + fs * 0.30f }, { cx + fs * 0.12f, cy - fs * 0.04f } };
        for (int y = 0; y < s; y++)
            for (int x = 0; x < s; x++) {
                float fx = (float)x + 0.5f, fy = (float)y + 0.5f, best = 1e9f;
                for (int i = 0; i < 4; i++) {                   /* the distance to the segments */
                    float ax = pts[i][0], ay = pts[i][1], bxx = pts[i + 1][0], byy = pts[i + 1][1];
                    float dx = bxx - ax, dy = byy - ay, t = clamp01(((fx - ax) * dx + (fy - ay) * dy) / (dx * dx + dy * dy));
                    best = fminf(best, hypotf(fx - (ax + t * dx), fy - (ay + t * dy)));
                }
                blend(&px[y * s + x], 0xffffff, clamp01(1.4f - best), 1.0f);
            }
    }
}

/* Wi-Fi: a dot and three arcs, as many lit as the signal says; `slash` = off. */
static void draw_wifi(uint32_t *px, int s, int signal, bool slash)
{
    memset(px, 0, (size_t)s * (size_t)s * 4);
    float fs = (float)s, cx = fs * 0.5f, cy = fs * 0.84f;
    int lit = signal < 0 ? 0 : signal >= 70 ? 3 : signal >= 45 ? 2 : signal >= 20 ? 1 : 0;
    if (!slash && signal >= 0 && signal < 20)
        lit = 0;
    float radius[3] = { fs * 0.28f, fs * 0.52f, fs * 0.76f }, width = fmaxf(1.4f, fs * 0.09f);
    for (int y = 0; y < s; y++)
        for (int x = 0; x < s; x++) {
            float fx = (float)x + 0.5f, fy = (float)y + 0.5f, dx = fx - cx, dy = fy - cy, d = hypotf(dx, dy);
            float ang = atan2f(-dy, dx);                        /* 90 degrees = straight up */
            bool in_fan = ang > (float)M_PI / 4.0f && ang < 3.0f * (float)M_PI / 4.0f;
            blend(&px[y * s + x], lm_theme.text, clamp01(fs * 0.085f + 0.5f - d), 1.0f);       /* the dot */
            for (int a = 0; a < 3; a++) {
                if (!in_fan)
                    continue;
                float cov = clamp01(width / 2 + 0.5f - fabsf(d - radius[a]));
                blend(&px[y * s + x], a < lit ? lm_theme.text : lm_theme.text_dim, cov, a < lit ? 1.0f : 0.35f);
            }
        }
    if (slash) {                                                /* a red slash across */
        for (int y = 0; y < s; y++)
            for (int x = 0; x < s; x++) {
                float fx = (float)x + 0.5f, fy = (float)y + 0.5f;
                float x0 = fs * 0.14f, y0 = fs * 0.14f, x1 = fs * 0.86f, y1 = fs * 0.86f;
                float dx = x1 - x0, dy = y1 - y0, t = clamp01(((fx - x0) * dx + (fy - y0) * dy) / (dx * dx + dy * dy));
                float d = hypotf(fx - (x0 + t * dx), fy - (y0 + t * dy));
                blend(&px[y * s + x], lm_theme.alert, clamp01(width * 0.55f + 0.5f - d), 1.0f);
            }
    }
}

/* The notification bell: a dome that flares out, a lip and a clapper; a red dot when something is unread, a slash while "do not disturb" is on. */
static void draw_bell(uint32_t *px, int s, bool unread, bool dnd)
{
    memset(px, 0, (size_t)s * (size_t)s * 4);
    float fs = (float)s, cx = fs * 0.5f;
    for (int y = 0; y < s; y++)
        for (int x = 0; x < s; x++) {
            float fx = (float)x + 0.5f, fy = (float)y + 0.5f, u = fy / fs, hw = -1.0f;      /* hw = half width of the bell at this height, in s units */
            if (u >= 0.17f && u < 0.40f) {                      /* the dome */
                float dy = (0.40f - u) / 0.23f;
                hw = 0.24f * sqrtf(fmaxf(1.0f - dy * dy, 0.0f));
            } else if (u >= 0.40f && u < 0.64f) {               /* the flare */
                hw = 0.24f + 0.12f * (u - 0.40f) / 0.24f;
            } else if (u >= 0.64f && u < 0.71f) {               /* the lip */
                hw = 0.40f;
            }
            float cov = hw > 0.0f ? clamp01(hw * fs - fabsf(fx - cx) + 0.5f) : 0.0f;
            blend(&px[y * s + x], lm_theme.text, cov, 1.0f);
            blend(&px[y * s + x], lm_theme.text, clamp01(fs * 0.075f + 0.5f - hypotf(fx - cx, fy - fs * 0.15f)), 1.0f);             /* the knob */
            blend(&px[y * s + x], lm_theme.text, clamp01(fs * 0.09f + 0.5f - hypotf(fx - cx, fy - fs * 0.82f)), 1.0f);              /* the clapper */
            if (unread)
                blend(&px[y * s + x], lm_theme.alert, clamp01(fs * 0.14f + 0.5f - hypotf(fx - fs * 0.80f, fy - fs * 0.22f)), 1.0f);
        }
    if (dnd) {
        float width = fmaxf(1.4f, fs * 0.09f);
        for (int y = 0; y < s; y++)
            for (int x = 0; x < s; x++) {
                float fx = (float)x + 0.5f, fy = (float)y + 0.5f;
                float x0 = fs * 0.12f, y0 = fs * 0.12f, x1 = fs * 0.88f, y1 = fs * 0.88f;
                float dx = x1 - x0, dy = y1 - y0, t = clamp01(((fx - x0) * dx + (fy - y0) * dy) / (dx * dx + dy * dy));
                blend(&px[y * s + x], lm_theme.warn, clamp01(width * 0.6f + 0.5f - hypotf(fx - (x0 + t * dx), fy - (y0 + t * dy))), 1.0f);
            }
    }
}

/* Ethernet: a plug (a body with a notch and a cable). */
static void draw_ethernet(uint32_t *px, int s)
{
    memset(px, 0, (size_t)s * (size_t)s * 4);
    float fs = (float)s, line = fmaxf(1.3f, fs * 0.08f);
    for (int y = 0; y < s; y++)
        for (int x = 0; x < s; x++) {
            float fx = (float)x + 0.5f, fy = (float)y + 0.5f;
            float outer = cov_box(fx, fy, fs * 0.16f, fs * 0.18f, fs * 0.84f, fs * 0.66f, fs * 0.08f);
            float inner = cov_box(fx, fy, fs * 0.16f + line, fs * 0.18f + line, fs * 0.84f - line, fs * 0.66f - line, 0.5f);
            blend(&px[y * s + x], lm_theme.text, clamp01(outer - inner), 1.0f);
            for (int k = 0; k < 4; k++)                         /* the contacts */
                blend(&px[y * s + x], lm_theme.text, cov_box(fx, fy, fs * (0.30f + 0.12f * (float)k), fs * 0.30f, fs * (0.30f + 0.12f * (float)k) + line, fs * 0.54f, 0.2f), 1.0f);
            blend(&px[y * s + x], lm_theme.text, cov_box(fx, fy, fs * 0.46f, fs * 0.66f, fs * 0.54f, fs * 0.88f, 0.2f), 1.0f);        /* the cable */
        }
}

/* ---- the tray buttons ------------------------------------------------------------------------------------------------------------- */

enum { ITEM_BATTERY, ITEM_NETWORK, ITEM_BELL, ITEM_COUNT };

struct tray_item {
    uint32_t id, pool, buf;
    int size;
    uint32_t *map;
    size_t map_size;
    bool clicked;
    char text[32];
    uint64_t icon_key;                  /* what the icon shows now (to redraw only on a change) */
    int which;
};

struct app {
    struct wlwin win;
    uint32_t tray, settings, menu_global;
    struct tray_item item[ITEM_COUNT];
    bool want_battery, want_network;
    struct battery bats[MAX_BAT];
    int nbats;
    struct netinfo net;
    struct wifi nets[MAX_NETS];
    int nnets;
    uint64_t bat_at, net_at;
    bool warned15, warned5;
    int menu_kind;                      /* 0 none, 1 battery, 2 network, 3 notifications */
    int unread;                         /* notifications not looked at (the settings value notes-unread) */
    bool dnd;                           /* "do not disturb" is on */
    bool bell_dirty;                    /* unread / dnd changed: look at the bell */
    struct { int action; char ssid[64]; bool secured, known; } menu_act[MENU_ENTRIES];
    int menu_n;
    char self[512];
};

enum { ACT_NONE, ACT_NOTES_SHOW, ACT_NOTES_DND, ACT_NOTES_CLEAR, ACT_RADIO, ACT_RESCAN, ACT_DISCONNECT, ACT_CONNECT_KNOWN, ACT_CONNECT_OPEN, ACT_CONNECT_PASSWORD };

static void buffer_nothing(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)c; (void)id; (void)op; (void)args; (void)data;                    /* wl_buffer.release: the compositor copied the picture at once */
}

static void notify(struct app *a, const char *title, const char *body, int ms)
{
    if (!a->settings || !a->win.c)
        return;
    char msg[800];
    snprintf(msg, sizeof(msg), "%s\t%s\t%d", title, body, ms);
    WLC_SEND(a->win.c, a->settings, LM_MELLAGUI_SETTINGS_V1_REQ_SET, { .s = "notify" }, { .s = msg });
    wlc_flush(a->win.c);
}

static bool g_icon_wanted;                                      /* an item learnt its size: draw its icon now, not at the next poll */

static void tray_item_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)c; (void)id;
    struct tray_item *it = data;
    if (op == LM_MELLAGUI_TRAY_ITEM_V1_EVT_SIZE && !it->buf && args[0].i >= 8 && args[0].i <= 64) {
        struct app *a = (struct app *)((char *)it - offsetof(struct app, item) - (size_t)it->which * sizeof(struct tray_item));
        int s = it->size = args[0].i;
        it->map_size = (size_t)s * (size_t)s * 4;
        int fd = memfd_create("mellagui-status-icon", MFD_CLOEXEC);
        if (fd < 0 || ftruncate(fd, (off_t)it->map_size) < 0) {
            if (fd >= 0)
                close(fd);
            return;
        }
        it->map = mmap(NULL, it->map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (it->map == MAP_FAILED) {
            it->map = NULL;
            close(fd);
            return;
        }
        it->pool = wlc_alloc(a->win.c, &lm_wl_shm_pool_interface, NULL, NULL);
        WLC_SEND(a->win.c, a->win.shm, LM_WL_SHM_REQ_CREATE_POOL, { .n = it->pool }, { .h = fd }, { .i = (int32_t)it->map_size });
        close(fd);
        it->buf = wlc_alloc(a->win.c, &lm_wl_buffer_interface, buffer_nothing, NULL);
        WLC_SEND(a->win.c, it->pool, LM_WL_SHM_POOL_REQ_CREATE_BUFFER, { .n = it->buf }, { .i = 0 }, { .i = s }, { .i = s }, { .i = s * 4 },
                 { .u = LM_WL_SHM_FORMAT_ARGB8888 });
        WLC_SEND0(a->win.c, it->pool, LM_WL_SHM_POOL_REQ_DESTROY);
        wlc_forget(a->win.c, it->pool);
        it->pool = 0;
        it->icon_key = UINT64_MAX;                                      /* the next update draws it */
        g_icon_wanted = true;
    } else if (op == LM_MELLAGUI_TRAY_ITEM_V1_EVT_CLICKED) {
        it->clicked = true;
    }
}

static void item_make(struct app *a, int which)
{
    struct tray_item *it = &a->item[which];
    if (it->id)
        return;
    it->which = which;
    it->id = wlc_alloc(a->win.c, &lm_mellagui_tray_item_v1_interface, tray_item_event, it);
    WLC_SEND(a->win.c, a->tray, LM_MELLAGUI_TRAY_V1_REQ_CREATE_ITEM, { .n = it->id });
    it->icon_key = UINT64_MAX;
}

static void item_drop(struct app *a, int which)
{
    struct tray_item *it = &a->item[which];
    if (!it->id)
        return;
    WLC_SEND0(a->win.c, it->id, LM_MELLAGUI_TRAY_ITEM_V1_REQ_DESTROY);
    wlc_forget(a->win.c, it->id);
    if (it->buf) {
        WLC_SEND0(a->win.c, it->buf, LM_WL_BUFFER_REQ_DESTROY);
        wlc_forget(a->win.c, it->buf);
    }
    if (it->map)
        munmap(it->map, it->map_size);
    memset(it, 0, sizeof(*it));
}

static void item_set(struct app *a, int which, uint64_t icon_key, const char *text, void (*draw)(struct app *, struct tray_item *))
{
    struct tray_item *it = &a->item[which];
    if (!it->id)
        return;
    if (it->buf && it->icon_key != icon_key) {
        it->icon_key = icon_key;
        draw(a, it);
        WLC_SEND(a->win.c, it->id, LM_MELLAGUI_TRAY_ITEM_V1_REQ_SET_ICON, { .o = it->buf });
    }
    if (strcmp(it->text, text)) {
        copy(it->text, sizeof(it->text), text);
        WLC_SEND(a->win.c, it->id, LM_MELLAGUI_TRAY_ITEM_V1_REQ_SET_TEXT, { .s = it->text });
    }
}

static void draw_battery_item(struct app *a, struct tray_item *it)
{
    const struct battery *b = a->nbats > 0 ? &a->bats[0] : NULL;
    draw_battery(it->map, it->size, b ? b->percent : -1, b && bat_charging(b));
}

static void draw_bell_item(struct app *a, struct tray_item *it)
{
    draw_bell(it->map, it->size, a->unread > 0, a->dnd);
}

static void draw_network_item(struct app *a, struct tray_item *it)
{
    const struct netinfo *n = &a->net;
    if (n->kind == NET_ETHERNET)
        draw_ethernet(it->map, it->size);
    else if (n->kind == NET_WIFI)
        draw_wifi(it->map, it->size, n->signal, false);
    else
        draw_wifi(it->map, it->size, -1, n->wifi_present && n->wifi_enabled ? false : true);
}

/* The two buttons follow what there is: a battery button only while there is a battery, a network button while nmcli runs. */
static void items_update(struct app *a)
{
    bool have_bat = a->want_battery && a->nbats > 0 && a->bats[0].system;
    if (have_bat)
        item_make(a, ITEM_BATTERY);
    else
        item_drop(a, ITEM_BATTERY);
    if (have_bat) {
        const struct battery *b = &a->bats[0];
        char t[32] = "";
        if (b->percent >= 0)
            snprintf(t, sizeof(t), "%d%%", b->percent);
        item_set(a, ITEM_BATTERY, (uint64_t)(b->percent + 2) * 4 + (bat_charging(b) ? 1 : 0) + ((uint64_t)lm_theme.text << 32), t, draw_battery_item);
    }
    bool have_net = a->want_network && a->net.have_nm;
    if (have_net)
        item_make(a, ITEM_NETWORK);
    else
        item_drop(a, ITEM_NETWORK);
    if (have_net) {
        const struct netinfo *n = &a->net;
        char t[32] = "";
        if (n->kind == NET_WIFI)
            snprintf(t, sizeof(t), "%.14s", n->ssid);
        else if (n->kind == NET_ETHERNET)
            snprintf(t, sizeof(t), "Ethernet");
        uint64_t key = (uint64_t)n->kind * 1000 + (uint64_t)(n->signal / 10) * 10 + (n->wifi_enabled ? 1 : 0) + (n->wifi_present ? 2 : 0) + ((uint64_t)lm_theme.text << 32);
        item_set(a, ITEM_NETWORK, key, t, draw_network_item);
    }
    bool have_bell = a->settings && (a->unread > 0 || a->dnd);          /* the bell is there while there is something to read or DND is on */
    if (have_bell)
        item_make(a, ITEM_BELL);
    else
        item_drop(a, ITEM_BELL);
    if (have_bell) {
        char t[32] = "";
        if (a->unread > 0)
            snprintf(t, sizeof(t), "%d", a->unread);
        else
            snprintf(t, sizeof(t), "DND");
        item_set(a, ITEM_BELL, (uint64_t)LM_MIN(a->unread, 99) * 4 + (a->dnd ? 2 : 0) + (a->unread > 0 ? 1 : 0) + ((uint64_t)lm_theme.text << 32), t, draw_bell_item);
    }
}

static void refresh_battery(struct app *a)
{
    a->nbats = read_batteries(power_root(), a->bats, MAX_BAT);
    a->bat_at = now_ms();
    if (a->nbats > 0 && a->bats[0].system && a->bats[0].percent >= 0) {         /* a warning when it runs low */
        const struct battery *b = &a->bats[0];
        bool dis = !strcasecmp(b->status, "Discharging");
        if (!dis || b->percent > 20) {
            a->warned15 = a->warned5 = false;
        } else if (dis && b->percent <= 5 && !a->warned5) {
            a->warned5 = a->warned15 = true;
            notify(a, "Battery almost empty", "Plug the computer in now", 0);
        } else if (dis && b->percent <= 15 && !a->warned15) {
            a->warned15 = true;
            char body[96];
            snprintf(body, sizeof(body), "%d%% left", b->percent);
            notify(a, "Battery low", body, 10000);
        }
    }
}

static void refresh_net(struct app *a)
{
    read_net(&a->net);
    a->net_at = now_ms();
}

/* ---- menus ------------------------------------------------------------------------------------------------------------------------ */

static void menu_line(char *buf, size_t n, size_t *o, const char *line)
{
    *o += (size_t)snprintf(buf + *o, n - *o, "%s%.46s", *o ? "\n" : "", line);
}

static void open_battery_menu(struct app *a)
{
    char buf[1024];
    size_t o = 0;
    a->menu_n = 0;
    for (int i = 0; i < a->nbats && a->menu_n < MENU_ENTRIES; i++) {
        const struct battery *b = &a->bats[i];
        char line[96], t[32];
        const char *name = b->model[0] ? b->model : b->system ? "Battery" : b->name;
        if (b->percent >= 0)
            snprintf(line, sizeof(line), "~%.24s  %d%%  %s", name, b->percent, b->status);
        else
            snprintf(line, sizeof(line), "~%.24s  %s", name, b->status);
        menu_line(buf, sizeof(buf), &o, line);
        a->menu_act[a->menu_n++].action = ACT_NONE;
        if (b->minutes >= 0 && a->menu_n < MENU_ENTRIES) {
            fmt_minutes(t, sizeof(t), b->minutes);
            snprintf(line, sizeof(line), "~   %s %s", t, bat_charging(b) ? "until full" : "left");
            menu_line(buf, sizeof(buf), &o, line);
            a->menu_act[a->menu_n++].action = ACT_NONE;
        }
    }
    if (!a->menu_n)
        return;
    a->menu_kind = 1;
    wlwin_menu(&a->win, buf);
}

static void open_network_menu(struct app *a)
{
    refresh_net(a);
    a->nnets = a->net.wifi_enabled ? wifi_list(NULL, a->nets, MAX_NETS) : 0;
    char buf[2048], line[128];
    size_t o = 0;
    a->menu_n = 0;
#define ADD(act, text, ssid_, secured_, known_) do { \
        if (a->menu_n < MENU_ENTRIES) { \
            menu_line(buf, sizeof(buf), &o, text); \
            a->menu_act[a->menu_n].action = (act); \
            copy(a->menu_act[a->menu_n].ssid, sizeof(a->menu_act[0].ssid), ssid_); \
            a->menu_act[a->menu_n].secured = (secured_); \
            a->menu_act[a->menu_n].known = (known_); \
            a->menu_n++; \
        } } while (0)
    if (a->net.kind == NET_WIFI)
        snprintf(line, sizeof(line), "~Connected to %.20s  %s", a->net.ssid, a->net.ip);
    else if (a->net.kind == NET_ETHERNET)
        snprintf(line, sizeof(line), "~Ethernet connected  %s", a->net.ip);
    else
        snprintf(line, sizeof(line), "~Not connected");
    ADD(ACT_NONE, line, "", false, false);
    if (a->net.wifi_present) {
        ADD(ACT_RADIO, a->net.wifi_enabled ? "Turn Wi-Fi off" : "Turn Wi-Fi on", "", false, false);
        if (a->net.wifi_enabled)
            ADD(ACT_RESCAN, "Scan for networks", "", false, false);
    }
    for (int i = 0; i < a->nnets; i++) {
        const struct wifi *w = &a->nets[i];
        snprintf(line, sizeof(line), "%c %.22s %3d%%%s", w->active ? '*' : ' ', w->ssid, w->signal, w->secured ? " lock" : "");
        ADD(w->active ? ACT_DISCONNECT : w->known ? ACT_CONNECT_KNOWN : w->secured ? ACT_CONNECT_PASSWORD : ACT_CONNECT_OPEN, line, w->ssid, w->secured, w->known);
    }
#undef ADD
    a->menu_kind = 2;
    wlwin_menu(&a->win, buf);
}

static void settings_send(struct app *a, const char *key, const char *value)
{
    if (a->settings)
        WLC_SEND(a->win.c, a->settings, LM_MELLAGUI_SETTINGS_V1_REQ_SET, { .s = key }, { .s = value });
}

/* The history window is another program next to this one (started twice, so that nobody has to wait for it). */
static void start_notes(struct app *a)
{
    char path[sizeof(a->self) + 16];
    snprintf(path, sizeof(path), "%s", a->self);
    char *slash = strrchr(path, '/');
    pid_t pid = fork();
    if (pid == 0) {
        if (fork() == 0) {
            setsid();
            if (slash) {
                snprintf(slash + 1, sizeof(path) - (size_t)(slash + 1 - path), "mellagui-notes");
                execl(path, path, (char *)NULL);
            }
            execlp("mellagui-notes", "mellagui-notes", (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }
    if (pid > 0)
        waitpid(pid, NULL, 0);
}

static void open_bell_menu(struct app *a)
{
    char buf[512], line[96];
    size_t o = 0;
    a->menu_n = 0;
    if (a->unread > 0)
        snprintf(line, sizeof(line), "~%d unread notification%s", a->unread, a->unread == 1 ? "" : "s");
    else
        snprintf(line, sizeof(line), "~No unread notifications");
    menu_line(buf, sizeof(buf), &o, line);
    a->menu_act[a->menu_n++].action = ACT_NONE;
    menu_line(buf, sizeof(buf), &o, "Show notifications...");
    a->menu_act[a->menu_n++].action = ACT_NOTES_SHOW;
    menu_line(buf, sizeof(buf), &o, a->dnd ? "Turn Do not disturb off" : "Turn Do not disturb on");
    a->menu_act[a->menu_n++].action = ACT_NOTES_DND;
    menu_line(buf, sizeof(buf), &o, "Clear the history");
    a->menu_act[a->menu_n++].action = ACT_NOTES_CLEAR;
    a->menu_kind = 3;
    wlwin_menu(&a->win, buf);
}

static void do_action(struct app *a, int idx)
{
    if (idx < 0 || idx >= a->menu_n)
        return;
    const char *ssid = a->menu_act[idx].ssid;
    char out[512];
    int rc;
    switch (a->menu_act[idx].action) {
    case ACT_NOTES_SHOW:
        start_notes(a);
        return;
    case ACT_NOTES_DND:
        settings_send(a, "dnd", a->dnd ? "0" : "1");
        return;
    case ACT_NOTES_CLEAR:
        settings_send(a, "notes-clear", "1");
        return;
    case ACT_RADIO:
        rc = nmcli(out, sizeof(out), 8000, "radio", "wifi", a->net.wifi_enabled ? "off" : "on", NULL);
        if (rc != 0)
            notify(a, "Wi-Fi", "Could not change the Wi-Fi switch", 6000);
        break;
    case ACT_RESCAN:
        nmcli(out, sizeof(out), 10000, "dev", "wifi", "rescan", NULL);
        break;
    case ACT_DISCONNECT:
        rc = nmcli(out, sizeof(out), 10000, "dev", "disconnect", a->net.dev, NULL);
        notify(a, "Wi-Fi", rc == 0 ? "Disconnected" : "Could not disconnect", 4000);
        break;
    case ACT_CONNECT_KNOWN:
        rc = nmcli(out, sizeof(out), 30000, "connection", "up", "id", ssid, NULL);
        trim(out);
        notify(a, "Wi-Fi", rc == 0 ? ssid : out[0] ? out : "Could not connect", rc == 0 ? 3000 : 8000);
        break;
    case ACT_CONNECT_OPEN:
        rc = nmcli(out, sizeof(out), 30000, "dev", "wifi", "connect", ssid, NULL);
        trim(out);
        notify(a, "Wi-Fi", rc == 0 ? ssid : out[0] ? out : "Could not connect", rc == 0 ? 3000 : 8000);
        break;
    case ACT_CONNECT_PASSWORD: {                                /* a small window of ours asks for the password */
        pid_t pid = fork();                                     /* (twice, so that nobody has to wait for the window) */
        if (pid == 0) {
            if (fork() == 0) {
                setsid();
                execl(a->self, a->self, "--password", ssid, (char *)NULL);
                _exit(127);
            }
            _exit(0);
        }
        if (pid > 0)
            waitpid(pid, NULL, 0);
        break;
    }
    default:
        break;
    }
    refresh_net(a);
    items_update(a);
}

static void menu_event(struct wlwin *w, int index)
{
    struct app *a = w->data;
    int kind = a->menu_kind;
    a->menu_kind = 0;
    if ((kind == 2 || kind == 3) && index >= 0)
        do_action(a, index);
}

/* ---- the connection to the compositor ----------------------------------------------------------------------------------------------- */

static void settings_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)c; (void)id;
    struct app *a = data;
    if (op != LM_MELLAGUI_SETTINGS_V1_EVT_VALUE)
        return;
    if (!strcmp(args[0].s, "notes-unread") && atoi(args[1].s) != a->unread) {
        a->unread = atoi(args[1].s);
        a->bell_dirty = true;
    } else if (!strcmp(args[0].s, "dnd") && (atoi(args[1].s) != 0) != a->dnd) {
        a->dnd = atoi(args[1].s) != 0;
        a->bell_dirty = true;
    }
}

static void global_event(struct wlwin *w, uint32_t name, const char *iface, uint32_t ver)
{
    (void)ver;
    struct app *a = w->data;
    if (!strcmp(iface, "mellagui_tray_v1"))
        a->tray = wlc_bind(w->c, w->registry, name, &lm_mellagui_tray_v1_interface, 1, settings_event, a);
    else if (!strcmp(iface, "mellagui_settings_v1"))
        a->settings = wlc_bind(w->c, w->registry, name, &lm_mellagui_settings_v1_interface, 1, settings_event, a);
}

static void paint_nothing(struct wlwin *w) { (void)w; }

static int run_background(bool battery, bool network)
{
    static const struct wlwin_ops ops = { .paint = paint_nothing, .global = global_event, .menu = menu_event };
    struct app a = { .want_battery = battery, .want_network = network };
    ssize_t k = readlink("/proc/self/exe", a.self, sizeof(a.self) - 1);
    if (k > 0)
        a.self[k] = 0;
    else
        copy(a.self, sizeof(a.self), "mellagui-status");
    int rc = 1;
    if (wlwin_connect(&a.win, "mellagui-status", &ops, &a) < 0)
        return 1;
    if (!a.tray) {
        fprintf(stderr, "mellagui-status: the compositor has no mellagui_tray_v1 (or it is not served to this program)\n");
        goto out;
    }
    refresh_battery(&a);
    refresh_net(&a);
    items_update(&a);
    if (!a.item[ITEM_BATTERY].id && !a.item[ITEM_NETWORK].id && !a.settings) {
        rc = 0;                                                  /* nothing to show: no battery and no NetworkManager */
        goto out;
    }
    for (;;) {
        uint64_t now = now_ms();
        if (now - a.bat_at >= BAT_POLL_MS) {
            refresh_battery(&a);
            items_update(&a);
        }
        if (now - a.net_at >= NET_POLL_MS) {
            refresh_net(&a);
            items_update(&a);
        }
        if (wlc_dispatch(a.win.c, 500) < 0)
            break;
        if (g_icon_wanted) {
            g_icon_wanted = false;
            items_update(&a);
        }
        if (a.bell_dirty) {
            a.bell_dirty = false;
            items_update(&a);
        }
        if (a.item[ITEM_BELL].clicked) {
            a.item[ITEM_BELL].clicked = false;
            open_bell_menu(&a);
        }
        if (a.item[ITEM_BATTERY].clicked) {
            a.item[ITEM_BATTERY].clicked = false;
            refresh_battery(&a);
            items_update(&a);
            open_battery_menu(&a);
        }
        if (a.item[ITEM_NETWORK].clicked) {
            a.item[ITEM_NETWORK].clicked = false;
            open_network_menu(&a);
            items_update(&a);
        }
        wlc_flush(a.win.c);
    }
    rc = 0;
out:
    wlwin_destroy(&a.win);
    return rc;
}

/* ---- the password window ------------------------------------------------------------------------------------------------------------ */

struct pw {
    struct wlwin win;
    uint32_t settings;
    char ssid[64];
    char text[128];
    int len;
    bool busy;
    bool done;
    char msg[160];
};

static void pw_paint(struct wlwin *w)
{
    struct pw *p = w->data;
    wlwin_fill(w, 0, 0, w->bw, w->bh, lm_theme.background);
    char title[128];
    snprintf(title, sizeof(title), "Password for %s", p->ssid);
    wlwin_text(w, 14, 14, title, 1, lm_theme.text, w->bw - 28);
    wlwin_fill(w, 14, 40, w->bw - 28, LM_FONT_H + 12, lm_theme.field);
    wlwin_fill(w, 14, 40 + LM_FONT_H + 11, w->bw - 28, 1, lm_theme.accent);
    char stars[130];
    int n = 0;
    for (const char *s = p->text; *s && n < (int)sizeof(stars) - 1; s++)        /* one star per character */
        if (((unsigned char)*s & 0xc0) != 0x80)
            stars[n++] = '*';
    stars[n] = 0;
    wlwin_text(w, 20, 46, stars, 1, lm_theme.text, w->bw - 40);
    if (!p->busy)
        wlwin_fill(w, 20 + n * LM_FONT_W, 46, 2, LM_FONT_H, lm_theme.accent);
    wlwin_text(w, 14, 84, p->busy ? "Connecting..." : p->msg[0] ? p->msg : "Enter connects, Esc cancels", 1, p->busy ? lm_theme.accent : p->msg[0] ? lm_theme.error : lm_theme.text_dim, w->bw - 28);
}

static void utf8_append(char *buf, size_t max, uint32_t cp)
{
    size_t n = strlen(buf);
    char tmp[5];
    int k = 0;
    if (cp < 0x80) tmp[k++] = (char)cp;
    else if (cp < 0x800) { tmp[k++] = (char)(0xc0 | (cp >> 6)); tmp[k++] = (char)(0x80 | (cp & 0x3f)); }
    else if (cp < 0x10000) { tmp[k++] = (char)(0xe0 | (cp >> 12)); tmp[k++] = (char)(0x80 | ((cp >> 6) & 0x3f)); tmp[k++] = (char)(0x80 | (cp & 0x3f)); }
    else { tmp[k++] = (char)(0xf0 | (cp >> 18)); tmp[k++] = (char)(0x80 | ((cp >> 12) & 0x3f)); tmp[k++] = (char)(0x80 | ((cp >> 6) & 0x3f)); tmp[k++] = (char)(0x80 | (cp & 0x3f)); }
    if (n + (size_t)k < max) {
        memcpy(buf + n, tmp, (size_t)k);
        buf[n + (size_t)k] = 0;
    }
}

static void pw_connect(struct pw *p)
{
    p->busy = true;
    p->win.redraw = true;
    while (wlwin_step(&p->win, 0)) {                            /* show "Connecting..." before the call blocks */
        if (!p->win.redraw)
            break;
    }
    char msg[256];
    bool ok = connect_with_password(p->ssid, p->text, msg, sizeof(msg));
    memset(p->text, 0, sizeof(p->text));
    p->len = 0;
    if (p->settings) {
        char m[800];
        snprintf(m, sizeof(m), "Wi-Fi\t%s\t%d", ok ? p->ssid : (msg[0] ? msg : "Could not connect"), ok ? 3000 : 8000);
        WLC_SEND(p->win.c, p->settings, LM_MELLAGUI_SETTINGS_V1_REQ_SET, { .s = "notify" }, { .s = m });
        wlc_roundtrip(p->win.c);
    }
    if (ok) {
        p->done = true;
        p->win.running = false;
        return;
    }
    p->busy = false;
    copy(p->msg, sizeof(p->msg), msg[0] ? msg : "Could not connect");
    p->win.redraw = true;
}

static void pw_keyboard(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
    struct pw *p = w->data;
    if (op != LM_WL_KEYBOARD_EVT_KEY || args[3].u == 0 || p->busy)
        return;
    uint32_t ks = lm_keymap_keysym(args[2].u, w->mods);
    p->msg[0] = 0;
    if (ks == LM_KS_Escape) {
        w->running = false;
    } else if (ks == LM_KS_Return || ks == LM_KS_KP_Enter) {
        if (p->text[0])
            pw_connect(p);
    } else if (ks == LM_KS_BackSpace) {
        size_t n = strlen(p->text);
        while (n > 0 && ((unsigned char)p->text[n - 1] & 0xc0) == 0x80)
            n--;
        if (n > 0)
            p->text[n - 1] = 0;
    } else if ((ks >= 0x20 && ks <= 0x7e) || (ks >= 0xa0 && ks <= 0xff)) {
        utf8_append(p->text, sizeof(p->text), ks);
    } else if ((ks & 0xff000000u) == 0x01000000u) {
        utf8_append(p->text, sizeof(p->text), ks & 0x00ffffffu);
    }
    w->redraw = true;
}

static void pw_global(struct wlwin *w, uint32_t name, const char *iface, uint32_t ver)
{
    (void)ver;
    struct pw *p = w->data;
    if (!strcmp(iface, "mellagui_settings_v1"))
        p->settings = wlc_bind(w->c, w->registry, name, &lm_mellagui_settings_v1_interface, 1, settings_event, p);
}

static int run_password(const char *ssid)
{
    static const struct wlwin_ops ops = { .paint = pw_paint, .global = pw_global, .keyboard = pw_keyboard };
    static struct pw p;
    copy(p.ssid, sizeof(p.ssid), ssid);
    int rc = 1;
    if (wlwin_connect(&p.win, "mellagui-status", &ops, &p) < 0)
        return 1;
    if (wlwin_open(&p.win, "Wi-Fi password", "mellagui.wifi", 380, 120, 380, 120) < 0)
        goto out;
    while (wlwin_step(&p.win, -1))
        ;
    rc = 0;
out:
    wlwin_destroy(&p.win);
    return rc;
}

/* ---- text output and the self test -------------------------------------------------------------------------------------------------- */

static int run_list(void)
{
    struct battery b[MAX_BAT];
    int n = read_batteries(power_root(), b, MAX_BAT);
    for (int i = 0; i < n; i++)
        printf("battery %s %s system=%d percent=%d minutes=%d status=%s\n", b[i].name, b[i].model[0] ? b[i].model : "-", b[i].system, b[i].percent, b[i].minutes, b[i].status);
    struct netinfo ni;
    read_net(&ni);
    printf("network nm=%d wifi=%d radio=%d kind=%s dev=%s ssid=%s signal=%d ip=%s\n", ni.have_nm, ni.wifi_present, ni.wifi_enabled,
           ni.kind == NET_WIFI ? "wifi" : ni.kind == NET_ETHERNET ? "ethernet" : "none", ni.dev[0] ? ni.dev : "-", ni.ssid[0] ? ni.ssid : "-", ni.signal, ni.ip[0] ? ni.ip : "-");
    if (ni.wifi_enabled) {
        struct wifi w[MAX_NETS];
        int m = wifi_list(NULL, w, MAX_NETS);
        for (int i = 0; i < m; i++)
            printf("wifi %s signal=%d secured=%d active=%d known=%d\n", w[i].ssid, w[i].signal, w[i].secured, w[i].active, w[i].known);
    }
    return 0;
}

static int selftest(void)
{
    int bad = 0;
#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "mellagui-status selftest: " __VA_ARGS__); fputc('\n', stderr); bad++; } } while (0)
    char f[4][96];
    int n = terse_split("*:Home\\:Net:82:WPA2", f, 4);
    CHECK(n == 4 && !strcmp(f[0], "*") && !strcmp(f[1], "Home:Net") && !strcmp(f[2], "82") && !strcmp(f[3], "WPA2"), "terse_split %d '%s'", n, f[1]);
    n = terse_split("a\\\\b:", f, 4);
    CHECK(n == 2 && !strcmp(f[0], "a\\b") && !f[1][0], "terse_split backslash");
    char root[] = "/tmp/mellagui-status-test-XXXXXX";
    if (!mkdtemp(root)) {
        fprintf(stderr, "mellagui-status selftest: no temp dir\n");
        return 1;
    }
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s/BAT0 %s/hidpp_battery_1 %s/AC && cd %s && printf Battery > BAT0/type && printf Discharging > BAT0/status && "
             "printf 40000000 > BAT0/energy_now && printf 50000000 > BAT0/energy_full && printf 10000000 > BAT0/power_now && "
             "printf Battery > hidpp_battery_1/type && printf Device > hidpp_battery_1/scope && printf Normal > hidpp_battery_1/capacity_level && "
             "printf Discharging > hidpp_battery_1/status && printf Mains > AC/type", root, root, root, root);
    if (system(cmd) != 0) {
        fprintf(stderr, "mellagui-status selftest: cannot make the test tree\n");
        return 1;
    }
    struct battery b[MAX_BAT];
    n = read_batteries(root, b, MAX_BAT);
    CHECK(n == 2, "found %d batteries", n);
    CHECK(n == 2 && b[0].system && !strcmp(b[0].name, "BAT0") && b[0].percent == 80 && b[0].minutes == 240, "BAT0 %d%% %d min", b[0].percent, b[0].minutes);
    CHECK(n == 2 && !b[1].system && b[1].percent == 50, "mouse %d%%", b[1].percent);
    snprintf(cmd, sizeof(cmd), "rm -rf %s", root);
    if (system(cmd) != 0) {}
    uint32_t px[22 * 22];
    draw_battery(px, 22, 80, true);
    int lit = 0;
    for (int i = 0; i < 22 * 22; i++)
        lit += (px[i] >> 24) > 128;
    CHECK(lit > 60, "battery icon is empty (%d)", lit);
    draw_wifi(px, 22, 60, false);
    lit = 0;
    for (int i = 0; i < 22 * 22; i++)
        lit += (px[i] >> 24) > 128;
    CHECK(lit > 30, "wifi icon is empty (%d)", lit);
    char t[32];
    fmt_minutes(t, sizeof(t), 135);
    CHECK(!strcmp(t, "2 h 15 min"), "fmt_minutes '%s'", t);
#undef CHECK
    if (!bad)
        printf("mellagui-status selftest ok\n");
    return bad ? 1 : 0;
}

int main(int argc, char **argv)
{
    lm_theme_load_env();
    bool battery = true, network = true;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--selftest"))
            return selftest();
        if (!strcmp(argv[i], "--list"))
            return run_list();
        if (!strcmp(argv[i], "--password") && i + 1 < argc)
            return run_password(argv[i + 1]);
        if (!strcmp(argv[i], "--no-battery"))
            battery = false;
        else if (!strcmp(argv[i], "--no-network"))
            network = false;
        else {
            fprintf(stderr, "usage: mellagui-status [--no-battery] [--no-network] | --password SSID | --list | --selftest\n");
            return 2;
        }
    }
    return run_background(battery, network);
}
