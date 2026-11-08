/*
 * mellagui-monitor: the settings of the monitors themselves - brightness, contrast, speaker volume and mute, input source,
 * colour preset and standby - sent to the monitor over DDC/CI (src/ddc.c, /dev/i2c-N) and, for a laptop's own panel, through
 * /sys/class/backlight. One tab per connected monitor; a control the monitor does not implement is not shown.
 *
 *   mellagui-monitor                         the panel
 *   mellagui-monitor --list                  print the monitors and their values (no display needed)
 *   mellagui-monitor --set NAME CODE VALUE   e.g. HDMI-A-1 10 40 (CODE is the hexadecimal VCP code, 10 = brightness)
 *
 * Access: /dev/i2c-N must be readable and writable (kernel module i2c-dev, group "i2c" or a logind ACL); the backlight needs a
 * udev rule or the video group. Env MELLAGUI_MONITOR_FAKE=FILE replaces the hardware by a text file (tests, see load_fake()).
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "theme.h"
#include "ddc.h"
#include "edid.h"
#include "font.h"
#include "keymap.h"
#include "util.h"
#include "wlwin.h"

#define COL_BG      (lm_theme.background)
#define COL_KEY     (lm_theme.button)
#define COL_HOVER   (lm_theme.button_hover)
#define COL_DOWN    (lm_theme.button_down)
#define COL_ACCENT  (lm_theme.accent)
#define COL_TEXT    (lm_theme.text)
#define COL_DIM     (lm_theme.text_dim)
#define COL_ERR     (lm_theme.error)
#define MAX_MON 8
#define MAX_WIDGETS 64
#define PAD 20
#define SEND_GAP_MS 150         /* while a slider is dragged, one value to the monitor at most this often */

/* ---- the controls ---------------------------------------------------------------------------------------------- */

enum { C_BRIGHT, C_CONTRAST, C_VOLUME, C_MUTE, C_INPUT, C_PRESET, C_POWER, NCTL };

static const struct { uint8_t code; const char *name; } CTL[NCTL] = {
    { LM_VCP_BRIGHTNESS, "Brightness" }, { LM_VCP_CONTRAST, "Contrast" }, { LM_VCP_VOLUME, "Volume" },
    { LM_VCP_MUTE, "Mute" }, { LM_VCP_INPUT, "Input" }, { LM_VCP_PRESET, "Colour" }, { LM_VCP_POWER, "Power" },
};

/* Names of the values (MCCS); the first entries are what is offered when the monitor does not list its own values. */
struct vname { int value; const char *name; };
static const struct vname INPUTS[] = {
    { 0x01, "VGA" }, { 0x03, "DVI" }, { 0x0f, "DisplayPort 1" }, { 0x10, "DisplayPort 2" }, { 0x11, "HDMI 1" }, { 0x12, "HDMI 2" },
    { 0x02, "VGA 2" }, { 0x04, "DVI 2" }, { 0x13, "HDMI 3" }, { 0x19, "USB-C" },
};
static const struct vname PRESETS[] = {
    { 0x01, "sRGB" }, { 0x02, "Native" }, { 0x04, "5000 K" }, { 0x05, "6500 K" }, { 0x06, "7500 K" }, { 0x08, "9300 K" }, { 0x0b, "User 1" },
    { 0x03, "4000 K" }, { 0x07, "8200 K" }, { 0x09, "10000 K" }, { 0x0a, "11500 K" }, { 0x0c, "User 2" }, { 0x0d, "User 3" },
};
#define N_INPUTS_DEFAULT 6
#define N_PRESETS_DEFAULT 7
#define MAX_VALUES 16

enum backend { B_DDC, B_BACKLIGHT, B_FAKE };

struct control {
    bool ok;
    int max, cur;
};

struct mon {
    char name[32];              /* connector, e.g. "HDMI-A-3" */
    char label[72];             /* "Dell DELL U4025QW" */
    enum backend kind;
    int bus, fd;                /* DDC: i2c bus number, open descriptor (-1 = none) */
    char path[300];             /* backlight directory */
    char err[160];              /* what went wrong (shown in red) */
    struct control c[NCTL];
    int probed;                 /* controls read so far (NCTL = done) */
    bool caps_done;             /* the capabilities string was asked for (DDC) */
    char caps[1200];            /* what the monitor says it has ("" = it did not tell: every control is tried) */
    int in_vals[MAX_VALUES], n_in;      /* inputs / colour presets the monitor lists for itself (0 = not listed) */
    int pre_vals[MAX_VALUES], n_pre;
    int saved_volume;           /* volume before a software mute (a monitor without a mute feature is silenced with volume 0) */
};

static struct mon mons[MAX_MON];
static int nmon;
static const char *fake_file;

/* ---- the fake monitors (tests) ----------------------------------------------------------------------------------- */

/* "monitor NAME LABEL..." starts a monitor, then one line per supported feature: "CODE(hex) MAX CURRENT". */
static void load_fake(void)
{
    FILE *f = fopen(fake_file, "r");
    if (!f)
        return;
    char line[256];
    struct mon *m = NULL;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = 0;
        if (!strncmp(line, "monitor ", 8) && nmon < MAX_MON) {
            m = &mons[nmon++];
            memset(m, 0, sizeof(*m));
            m->kind = B_FAKE;
            m->fd = -1;
            m->probed = NCTL;
            char name[32] = "", label[72] = "";
            sscanf(line + 8, "%31s %71[^\n]", name, label);
            snprintf(m->name, sizeof(m->name), "%s", name);
            snprintf(m->label, sizeof(m->label), "%s", label[0] ? label : name);
        } else if (m) {
            unsigned code;
            int max, cur;
            if (sscanf(line, "%x %d %d", &code, &max, &cur) == 3)
                for (int i = 0; i < NCTL; i++)
                    if (CTL[i].code == code) {
                        m->c[i] = (struct control){ true, max, cur };
                        break;
                    }
        }
    }
    fclose(f);
}

static void save_fake(void)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", fake_file);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return;
    for (int i = 0; i < nmon; i++) {
        fprintf(f, "monitor %s %s\n", mons[i].name, mons[i].label);
        for (int k = 0; k < NCTL; k++)
            if (mons[i].c[k].ok)
                fprintf(f, "%02x %d %d\n", CTL[k].code, mons[i].c[k].max, mons[i].c[k].cur);
    }
    fclose(f);
    rename(tmp, fake_file);
}

/* ---- finding the monitors ----------------------------------------------------------------------------------------- */

static bool read_text(const char *path, char *out, size_t n)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    size_t len = fread(out, 1, n - 1, f);
    fclose(f);
    out[len] = 0;
    out[strcspn(out, "\n")] = 0;
    return true;
}

static void add_drm_monitor(const char *dirname, const char *connector)
{
    char path[300], link[200], buf[64];
    snprintf(path, sizeof(path), "/sys/class/drm/%s/status", dirname);
    if (!read_text(path, buf, sizeof(buf)) || strcmp(buf, "connected"))
        return;
    if (strstr(connector, "eDP") || strstr(connector, "LVDS") || strstr(connector, "DSI"))
        return;                                                     /* a laptop panel: the backlight takes care of it */
    snprintf(path, sizeof(path), "/sys/class/drm/%s/ddc", dirname);
    ssize_t n = readlink(path, link, sizeof(link) - 1);
    if (n <= 0)
        return;
    link[n] = 0;
    const char *base = strrchr(link, '/') ? strrchr(link, '/') + 1 : link;
    if (strncmp(base, "i2c-", 4) || nmon >= MAX_MON)
        return;
    struct mon *m = &mons[nmon++];
    memset(m, 0, sizeof(*m));
    m->kind = B_DDC;
    m->bus = atoi(base + 4);
    m->fd = -1;
    snprintf(m->name, sizeof(m->name), "%s", connector);
    snprintf(m->label, sizeof(m->label), "%s", connector);
    snprintf(path, sizeof(path), "/sys/class/drm/%s/edid", dirname);
    FILE *f = fopen(path, "r");
    if (f) {
        uint8_t edid[256];
        size_t len = fread(edid, 1, sizeof(edid), f);
        fclose(f);
        struct lm_edid_info info;
        if (lm_edid_info(edid, len, &info)) {
            const char *vendor = lm_edid_vendor_name(info.vendor);
            if (!info.name[0])
                snprintf(m->label, sizeof(m->label), "%s monitor", vendor);
            else if (!strncasecmp(info.name, vendor, strlen(vendor)))
                snprintf(m->label, sizeof(m->label), "%s", info.name);      /* "DELL U4025QW" already names the maker */
            else
                snprintf(m->label, sizeof(m->label), "%s %s", vendor, info.name);
        }
    }
    m->fd = lm_ddc_open(m->bus);
    if (m->fd < 0) {
        int e = errno;
        snprintf(m->err, sizeof(m->err), "Cannot open /dev/i2c-%d: %s. %s", m->bus, strerror(e),
                 e == ENOENT ? "Load the kernel module: modprobe i2c-dev." :
                 e == EACCES ? "Your user needs access: join the i2c group (or a udev ACL)." : "");
        m->probed = NCTL;
    }
}

static void add_backlight(void)
{
    DIR *d = opendir("/sys/class/backlight");
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) && nmon < MAX_MON) {
        if (e->d_name[0] == '.')
            continue;
        char dir[300], p[340], buf[32];
        snprintf(dir, sizeof(dir), "/sys/class/backlight/%s", e->d_name);
        snprintf(p, sizeof(p), "%s/max_brightness", dir);
        if (!read_text(p, buf, sizeof(buf)) || atoi(buf) <= 0)
            continue;
        struct mon *m = &mons[nmon++];
        memset(m, 0, sizeof(*m));
        m->kind = B_BACKLIGHT;
        m->fd = -1;
        snprintf(m->path, sizeof(m->path), "%s", dir);
        snprintf(m->name, sizeof(m->name), "backlight");
        snprintf(m->label, sizeof(m->label), "Built-in display (%.40s)", e->d_name);
        m->c[C_BRIGHT].max = atoi(buf);
        m->probed = 0;
        break;                                                      /* the first one is the panel's */
    }
    closedir(d);
}

static void discover(void)
{
    if (fake_file) {
        load_fake();
        return;
    }
    DIR *d = opendir("/sys/class/drm");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (strncmp(e->d_name, "card", 4))
                continue;
            const char *dash = strchr(e->d_name, '-');
            if (dash)
                add_drm_monitor(e->d_name, dash + 1);
        }
        closedir(d);
    }
    add_backlight();
}

/* ---- reading and writing ------------------------------------------------------------------------------------------ */

/* Asks a DDC monitor what it really has (a second or two). The answers to single reads cannot be trusted: a monitor
 * without a mute feature still "supports" it with nonsense values. */
static void probe_caps(struct mon *m)
{
    if (m->caps_done)
        return;
    m->caps_done = true;
    if (m->kind != B_DDC || m->fd < 0)
        return;
    if (!lm_ddc_capabilities(m->fd, m->caps, sizeof(m->caps)))
        m->caps[0] = 0;
    m->n_in = lm_ddc_caps_values(m->caps, LM_VCP_INPUT, m->in_vals, MAX_VALUES);
    m->n_pre = lm_ddc_caps_values(m->caps, LM_VCP_PRESET, m->pre_vals, MAX_VALUES);
}

/* Reads control `i` of `m`; sets m->c[i].ok. */
static void probe_one(struct mon *m, int i)
{
    struct control *c = &m->c[i];
    if (m->kind == B_BACKLIGHT) {
        if (i != C_BRIGHT)
            return;
        char p[340], buf[32];
        snprintf(p, sizeof(p), "%s/brightness", m->path);
        if (read_text(p, buf, sizeof(buf))) {
            c->ok = true;
            c->cur = atoi(buf);
        }
        return;
    }
    if (m->kind != B_DDC || m->fd < 0)
        return;
    if (m->caps[0] && !lm_ddc_caps_has(m->caps, CTL[i].code))
        return;                                                     /* the monitor does not list it: it is not there */
    struct lm_ddc_vcp v;
    if (lm_ddc_get(m->fd, CTL[i].code, &v) && v.max > 0 && (i != C_MUTE || v.cur == 1 || v.cur == 2)) {
        c->ok = true;
        c->max = v.max;
        c->cur = v.cur;
    }
}

static bool set_control(struct mon *m, int i, int value)
{
    struct control *c = &m->c[i];
    if (value < 0)
        value = 0;
    if (c->max > 0 && i != C_POWER && i != C_INPUT && i != C_PRESET && i != C_MUTE && value > c->max)
        value = c->max;
    m->err[0] = 0;
    bool ok = false;
    if (m->kind == B_FAKE) {
        c->cur = value;
        save_fake();
        return true;
    } else if (m->kind == B_BACKLIGHT) {
        if (i == C_BRIGHT && value < 1)
            value = 1;                                              /* never switch the panel off by accident */
        char p[340];
        snprintf(p, sizeof(p), "%s/brightness", m->path);
        FILE *f = fopen(p, "w");
        if (f) {
            ok = fprintf(f, "%d\n", value) > 0;
            if (fclose(f) != 0)
                ok = false;
        }
        if (!ok)
            snprintf(m->err, sizeof(m->err), "Cannot write the backlight: %s. A udev rule or the video group gives access.", strerror(errno));
    } else if (m->kind == B_DDC) {
        ok = m->fd >= 0 && lm_ddc_set(m->fd, CTL[i].code, value);
        if (!ok)
            snprintf(m->err, sizeof(m->err), "The monitor did not take the %s setting.", CTL[i].name);
    }
    if (ok)
        c->cur = value;
    return ok;
}

/* Mute: the monitor's own feature (1 = muted, 2 = not) or, when it has none, volume 0 with the old volume remembered. */
static bool mute_available(const struct mon *m)
{
    return m->c[C_MUTE].ok || m->c[C_VOLUME].ok;
}

static bool is_muted(const struct mon *m)
{
    return m->c[C_MUTE].ok ? m->c[C_MUTE].cur == 1 : m->c[C_VOLUME].ok && m->c[C_VOLUME].cur == 0;
}

static void toggle_mute(struct mon *m)
{
    bool muted = is_muted(m);
    if (m->c[C_MUTE].ok) {
        set_control(m, C_MUTE, muted ? 2 : 1);
        return;
    }
    struct control *v = &m->c[C_VOLUME];
    if (muted) {
        int back = m->saved_volume > 0 ? m->saved_volume : v->max / 2;
        set_control(m, C_VOLUME, back > 0 ? back : 1);
    } else {
        m->saved_volume = v->cur;
        set_control(m, C_VOLUME, 0);
    }
}

/* ---- command line ------------------------------------------------------------------------------------------------- */

static void probe_all(void)
{
    for (int k = 0; k < nmon; k++) {
        probe_caps(&mons[k]);
        for (; mons[k].probed < NCTL; mons[k].probed++)
            probe_one(&mons[k], mons[k].probed);
    }
}

static int cli_list(void)
{
    probe_all();
    for (int k = 0; k < nmon; k++) {
        const struct mon *m = &mons[k];
        printf("%s|%s|%s", m->name, m->label, m->kind == B_DDC ? "ddc" : m->kind == B_BACKLIGHT ? "backlight" : "fake");
        for (int i = 0; i < NCTL; i++)
            if (m->c[i].ok)
                printf("|%02x=%d/%d", CTL[i].code, m->c[i].cur, m->c[i].max);
        if (m->err[0])
            printf("|error=%s", m->err);
        printf("\n");
    }
    return 0;
}

static int cli_set(const char *name, const char *code, const char *value)
{
    for (int k = 0; k < nmon; k++) {
        if (strcmp(mons[k].name, name))
            continue;
        unsigned c = (unsigned)strtoul(code, NULL, 16);
        for (int i = 0; i < NCTL; i++)
            if (CTL[i].code == c) {
                if (!set_control(&mons[k], i, atoi(value))) {
                    fprintf(stderr, "mellagui-monitor: %s\n", mons[k].err[0] ? mons[k].err : "write failed");
                    return 1;
                }
                return 0;
            }
        fprintf(stderr, "mellagui-monitor: unknown code %s\n", code);
        return 2;
    }
    fprintf(stderr, "mellagui-monitor: no monitor %s\n", name);
    return 2;
}

/* ---- the window --------------------------------------------------------------------------------------------------- */

enum wkind { W_TAB, W_HEADING, W_LABEL, W_SLIDER, W_CHECK, W_CHIP, W_BUTTON };

struct rect { int x, y, w, h; };

struct widget {
    enum wkind kind;
    struct rect r;
    char text[72];
    int ctl;                    /* control index, or the monitor for tabs */
    int value;                  /* chip: the value it sets */
    bool on;                    /* chip: selected; check: checked */
    uint32_t color;             /* labels */
};

struct app {
    struct wlwin win;
    int sel;                    /* selected monitor */
    double px, py;
    bool has_pointer;
    struct widget wd[MAX_WIDGETS];
    int nw;
    int drag;                   /* slider being dragged (widget index), -1 none */
    int focus;                  /* control index with the keyboard focus (slider / mute), -1 none */
    bool standby_armed;         /* the first click on "Standby" was made */
    bool dirty;                 /* a dragged slider has a value that was not sent yet */
    long last_send_ms;
};

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static struct widget *add(struct app *a, enum wkind kind, struct rect r, const char *text)
{
    if (a->nw >= MAX_WIDGETS)
        return &a->wd[MAX_WIDGETS - 1];
    struct widget *w = &a->wd[a->nw++];
    memset(w, 0, sizeof(*w));
    w->kind = kind;
    w->r = r;
    w->color = COL_DIM;
    snprintf(w->text, sizeof(w->text), "%s", text);
    return w;
}

/* Chips of one choice control (input, colour preset) flowing over as many lines as needed. Returns the new y. */
static int add_chips(struct app *a, int x, int y, int w, int ctl, const struct control *c, const char *title,
                     const int *values, char (*names)[16], int n)
{
    add(a, W_HEADING, (struct rect){ x, y, w, 20 }, title);
    y += 24;
    int cx = x;
    int cur = c->cur & 0xff;
    bool known = false;
    for (int i = 0; i <= n; i++) {
        char unknown[24];
        const char *name;
        int value;
        if (i < n) {
            value = values[i];
            name = names[i];
            known |= value == cur;
        } else {
            if (known)
                break;
            value = cur;                                            /* what the monitor reports, though it is not in our list */
            snprintf(unknown, sizeof(unknown), "0x%02x", cur);
            name = unknown;
        }
        int cw = lm_font_text_width(name) + 20;
        if (cx + cw > x + w) {
            cx = x;
            y += 32;
        }
        struct widget *wd = add(a, W_CHIP, (struct rect){ cx, y, cw, 26 }, name);
        wd->ctl = ctl;
        wd->value = value;
        wd->on = value == cur;
        cx += cw + 8;
    }
    return y + 38;
}

/* Name of a value ("HDMI 1"); an unknown one is shown as hexadecimal. */
static void value_name(const struct vname *tab, int n, int value, char out[16])
{
    for (int i = 0; i < n; i++)
        if (tab[i].value == value) {
            snprintf(out, 16, "%s", tab[i].name);
            return;
        }
    snprintf(out, 16, "0x%02x", value);
}

/* The values to offer: the ones the monitor lists for itself, else the first `dflt` of the table. */
static int fill_choices(const struct vname *tab, int ntab, int dflt, const int *listed, int nlisted, int *out)
{
    int n = 0;
    if (nlisted > 0) {
        for (int i = 0; i < nlisted && n < MAX_VALUES; i++)
            out[n++] = listed[i] & 0xff;
        return n;
    }
    for (int i = 0; i < dflt && i < ntab; i++)
        out[n++] = tab[i].value;
    return n;
}

static void build(struct app *a)
{
    a->nw = 0;
    int x = PAD, y = 16, w = a->win.bw - 2 * PAD;
    if (nmon == 0) {
        add(a, W_HEADING, (struct rect){ x, y, w, 20 }, "No monitor found");
        add(a, W_LABEL, (struct rect){ x, y + 30, w, 20 }, "A monitor must be connected and speak DDC/CI (HDMI, DisplayPort, DVI, VGA).");
        add(a, W_LABEL, (struct rect){ x, y + 54, w, 20 }, "Needs the kernel module i2c-dev and access to /dev/i2c-* (group i2c).");
        return;
    }
    if (a->sel >= nmon)
        a->sel = 0;
    if (nmon > 1) {
        int tx = x;
        for (int i = 0; i < nmon; i++) {
            int tw = lm_font_text_width(mons[i].name) + 24;
            struct widget *t = add(a, W_TAB, (struct rect){ tx, y, tw, 28 }, mons[i].name);
            t->ctl = i;
            t->on = i == a->sel;
            tx += tw + 6;
        }
        y += 40;
    }
    struct mon *m = &mons[a->sel];
    add(a, W_HEADING, (struct rect){ x, y, w, 20 }, m->label);
    y += 24;
    char sub[128];
    snprintf(sub, sizeof(sub), m->kind == B_DDC ? "%s   DDC/CI on /dev/i2c-%d" : m->kind == B_BACKLIGHT ? "%s   backlight" : "%s", m->name, m->bus);
    add(a, W_LABEL, (struct rect){ x, y, w, 20 }, sub);
    y += 30;
    if (m->probed < NCTL) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Reading the monitor's settings... (%d/%d)", m->probed, NCTL);
        add(a, W_LABEL, (struct rect){ x, y, w, 20 }, msg);
        return;
    }
    int shown = 0;
    for (int i = C_BRIGHT; i <= C_VOLUME; i++) {
        if (!m->c[i].ok)
            continue;
        struct widget *s = add(a, W_SLIDER, (struct rect){ x, y, w, 30 }, CTL[i].name);
        s->ctl = i;
        y += 34;
        shown++;
    }
    if (mute_available(m)) {
        struct widget *c = add(a, W_CHECK, (struct rect){ x, y, w, 26 }, "Mute the speakers");
        c->ctl = C_MUTE;
        c->on = is_muted(m);
        y += 32;
        shown++;
    }
    if (m->c[C_INPUT].ok) {
        int values[MAX_VALUES], n = fill_choices(INPUTS, (int)(sizeof(INPUTS) / sizeof(INPUTS[0])), N_INPUTS_DEFAULT, m->in_vals, m->n_in, values);
        char names[MAX_VALUES][16];
        for (int i = 0; i < n; i++)
            value_name(INPUTS, (int)(sizeof(INPUTS) / sizeof(INPUTS[0])), values[i], names[i]);
        y = add_chips(a, x, y + 6, w, C_INPUT, &m->c[C_INPUT], "Input", values, names, n);
        add(a, W_LABEL, (struct rect){ x, y - 8, w, 16 }, "An input without a signal blanks this screen.");
        y += 16;
        shown++;
    }
    if (m->c[C_PRESET].ok) {
        int values[MAX_VALUES], n = fill_choices(PRESETS, (int)(sizeof(PRESETS) / sizeof(PRESETS[0])), N_PRESETS_DEFAULT, m->pre_vals, m->n_pre, values);
        char names[MAX_VALUES][16];
        for (int i = 0; i < n; i++)
            value_name(PRESETS, (int)(sizeof(PRESETS) / sizeof(PRESETS[0])), values[i], names[i]);
        y = add_chips(a, x, y, w, C_PRESET, &m->c[C_PRESET], "Colour", values, names, n);
        shown++;
    }
    if (m->c[C_POWER].ok) {
        struct widget *b = add(a, W_BUTTON, (struct rect){ x, y, 300, 28 },
                               a->standby_armed ? "Click again: the monitor goes to standby" : "Standby (turn the monitor off)");
        b->ctl = C_POWER;
        b->on = a->standby_armed;
        shown++;
    }
    if (!shown && !m->err[0])
        add(a, W_LABEL, (struct rect){ x, y, w, 20 }, "This monitor does not answer to any setting.");
    struct widget *rb = add(a, W_BUTTON, (struct rect){ a->win.bw - PAD - 90, a->win.bh - 44, 90, 28 }, "Reload");
    rb->ctl = -1;
    if (m->err[0]) {
        struct widget *e = add(a, W_LABEL, (struct rect){ x, a->win.bh - 44, a->win.bw - 2 * PAD - 100, 20 }, m->err);
        e->color = COL_ERR;
    }
}

/* ---- widgets: geometry, painting -------------------------------------------------------------------------------- */

#define LABEL_W 110
#define VALUE_W 60

static struct rect slider_track(const struct widget *w)
{
    return (struct rect){ w->r.x + LABEL_W, w->r.y + 11, w->r.w - LABEL_W - VALUE_W, 8 };
}

static void fill(struct app *a, struct rect r, uint32_t color)
{
    wlwin_fill(&a->win, r.x, r.y, r.w, r.h, color);
}

static void outline(struct app *a, struct rect r, uint32_t color)
{
    fill(a, (struct rect){ r.x, r.y, r.w, 1 }, color);
    fill(a, (struct rect){ r.x, r.y + r.h - 1, r.w, 1 }, color);
    fill(a, (struct rect){ r.x, r.y, 1, r.h }, color);
    fill(a, (struct rect){ r.x + r.w - 1, r.y, 1, r.h }, color);
}

static bool inside(const struct rect *r, double x, double y)
{
    return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

static int widget_at(const struct app *a, double x, double y)
{
    for (int i = a->nw - 1; i >= 0; i--)
        if (a->wd[i].kind >= W_SLIDER || a->wd[i].kind == W_TAB)
            if (inside(&a->wd[i].r, x, y))
                return i;
    return -1;
}

static void paint(struct wlwin *win)
{
    struct app *a = win->data;
    fill(a, (struct rect){ 0, 0, win->bw, win->bh }, COL_BG);
    build(a);
    int hover = a->has_pointer ? widget_at(a, a->px, a->py) : -1;
    for (int i = 0; i < a->nw; i++) {
        const struct widget *w = &a->wd[i];
        const struct rect *r = &w->r;
        bool hot = i == hover;
        switch (w->kind) {
        case W_HEADING:
            wlwin_text(win, r->x, r->y, w->text, 1, COL_ACCENT, r->w);
            fill(a, (struct rect){ r->x, r->y + 19, r->w, 1 }, COL_KEY);
            break;
        case W_LABEL:
            wlwin_text(win, r->x, r->y, w->text, 1, w->color, r->w);
            break;
        case W_TAB:
            fill(a, *r, w->on ? COL_ACCENT : hot ? COL_HOVER : COL_KEY);
            wlwin_text(win, r->x + 12, r->y + (r->h - LM_FONT_H) / 2, w->text, 1, w->on ? COL_BG : COL_TEXT, r->w - 12);
            break;
        case W_SLIDER: {
            const struct mon *m = &mons[a->sel];
            const struct control *c = &m->c[w->ctl];
            struct rect t = slider_track(w);
            wlwin_text(win, r->x, r->y + (r->h - LM_FONT_H) / 2, w->text, 1, COL_TEXT, LABEL_W - 8);
            fill(a, t, COL_KEY);
            int kx = t.x + (c->max > 0 ? (int)((double)c->cur / c->max * t.w) : 0);
            fill(a, (struct rect){ t.x, t.y, kx - t.x, t.h }, COL_ACCENT);
            struct rect knob = { kx - 5, r->y + 4, 10, r->h - 8 };
            fill(a, knob, a->drag == i ? COL_DOWN : hot ? COL_HOVER : COL_TEXT);
            if (a->focus == w->ctl)
                outline(a, (struct rect){ knob.x - 2, knob.y - 2, knob.w + 4, knob.h + 4 }, COL_ACCENT);
            char v[24];
            if (c->max == 100)
                snprintf(v, sizeof(v), "%d%%", c->cur);
            else
                snprintf(v, sizeof(v), "%d/%d", c->cur, c->max);
            wlwin_text(win, t.x + t.w + 12, r->y + (r->h - LM_FONT_H) / 2, v, 1, COL_DIM, 0);
            break;
        }
        case W_CHECK: {
            struct rect box = { r->x, r->y + 5, 16, 16 };
            fill(a, box, hot ? COL_HOVER : COL_KEY);
            if (w->on)
                fill(a, (struct rect){ box.x + 4, box.y + 4, 8, 8 }, COL_ACCENT);
            if (a->focus == w->ctl)
                outline(a, (struct rect){ box.x - 2, box.y - 2, 20, 20 }, COL_ACCENT);
            wlwin_text(win, r->x + 26, r->y + (r->h - LM_FONT_H) / 2, w->text, 1, COL_TEXT, r->w - 26);
            break;
        }
        case W_CHIP:
            fill(a, *r, w->on ? COL_ACCENT : hot ? COL_HOVER : COL_KEY);
            wlwin_text(win, r->x + 10, r->y + (r->h - LM_FONT_H) / 2, w->text, 1, w->on ? COL_BG : COL_TEXT, r->w - 10);
            break;
        case W_BUTTON:
            fill(a, *r, w->on ? COL_ERR : hot ? COL_HOVER : COL_KEY);
            wlwin_text(win, r->x + 10, r->y + (r->h - LM_FONT_H) / 2, w->text, 1, w->on ? COL_BG : COL_TEXT, r->w - 10);
            break;
        }
    }
}

/* ---- actions -------------------------------------------------------------------------------------------------- */

static int slider_value(const struct app *a, const struct widget *w, double x)
{
    const struct control *c = &mons[a->sel].c[w->ctl];
    struct rect t = slider_track(w);
    double f = (x - t.x) / t.w;
    f = f < 0 ? 0 : f > 1 ? 1 : f;
    return (int)(f * c->max + 0.5);
}

static void flush_slider(struct app *a, int ctl)
{
    if (!a->dirty)
        return;
    a->dirty = false;
    set_control(&mons[a->sel], ctl, mons[a->sel].c[ctl].cur);
    a->last_send_ms = now_ms();
    a->win.redraw = true;
}

static void activate(struct app *a, int idx)
{
    const struct widget *w = &a->wd[idx];
    struct mon *m = &mons[a->sel];
    if (w->kind != W_BUTTON || w->ctl != C_POWER)
        a->standby_armed = false;
    switch (w->kind) {
    case W_TAB:
        a->sel = w->ctl;
        a->focus = -1;
        break;
    case W_CHECK:
        toggle_mute(m);
        break;
    case W_CHIP:
        set_control(m, w->ctl, w->value);
        break;
    case W_BUTTON:
        if (w->ctl == C_POWER) {
            if (!a->standby_armed)
                a->standby_armed = true;
            else {
                a->standby_armed = false;
                set_control(m, C_POWER, 4);
            }
        } else if (m->kind != B_FAKE) {                             /* Reload: read everything again */
            memset(m->c, 0, sizeof(m->c));
            if (m->kind == B_BACKLIGHT)
                m->c[C_BRIGHT].max = atoi("1");
            m->probed = 0;
            m->err[0] = 0;
        }
        break;
    default:
        break;
    }
    a->win.redraw = true;
}

static void pointer_event(struct wlwin *win, uint32_t op, const union lm_arg *args)
{
    struct app *a = win->data;
    switch (op) {
    case LM_WL_POINTER_EVT_ENTER:
        a->has_pointer = true;
        a->px = lm_fixed_to_double(args[2].f);
        a->py = lm_fixed_to_double(args[3].f);
        break;
    case LM_WL_POINTER_EVT_LEAVE:
        a->has_pointer = false;
        break;
    case LM_WL_POINTER_EVT_MOTION:
        a->px = lm_fixed_to_double(args[1].f);
        a->py = lm_fixed_to_double(args[2].f);
        if (a->drag >= 0 && a->drag < a->nw) {
            const struct widget *w = &a->wd[a->drag];
            mons[a->sel].c[w->ctl].cur = slider_value(a, w, a->px);
            a->dirty = true;
        }
        break;
    case LM_WL_POINTER_EVT_BUTTON:
        if (args[2].u != 272)                                       /* BTN_LEFT */
            break;
        if (args[3].u == 1) {
            build(a);
            int i = widget_at(a, a->px, a->py);
            if (i < 0)
                break;
            if (a->wd[i].kind == W_SLIDER) {
                a->drag = i;
                a->focus = a->wd[i].ctl;
                mons[a->sel].c[a->wd[i].ctl].cur = slider_value(a, &a->wd[i], a->px);
                a->dirty = true;
            } else {
                a->focus = a->wd[i].kind == W_CHECK ? a->wd[i].ctl : -1;
                activate(a, i);
            }
        } else if (a->drag >= 0) {
            int ctl = a->wd[a->drag].ctl;
            a->drag = -1;
            flush_slider(a, ctl);
        }
        break;
    }
    a->win.redraw = true;
}

static void keyboard_event(struct wlwin *win, uint32_t op, const union lm_arg *args)
{
    struct app *a = win->data;
    if (op != LM_WL_KEYBOARD_EVT_KEY || args[3].u == 0 || !nmon)
        return;
    uint32_t ks = lm_keymap_keysym(args[2].u, win->mods);
    struct mon *m = &mons[a->sel];
    if (ks == LM_KS_Escape) {
        win->running = false;
        return;
    }
    static const int order[] = { C_BRIGHT, C_CONTRAST, C_VOLUME, C_MUTE };
    if (ks == LM_KS_Up || ks == LM_KS_Down || ks == LM_KS_Tab) {
        int dir = ks == LM_KS_Up ? -1 : 1, pos = -1;
        for (int i = 0; i < 4; i++)
            if (order[i] == a->focus)
                pos = i;
        for (int step = 1; step <= 4; step++) {
            int cand = ((pos < 0 ? (dir > 0 ? -1 : 0) : pos) + dir * step + 8) % 4;
            if (order[cand] == C_MUTE ? mute_available(m) : m->c[order[cand]].ok) {
                a->focus = order[cand];
                break;
            }
        }
    } else if (a->focus >= 0 && a->focus != C_MUTE && m->c[a->focus].ok &&
               (ks == LM_KS_Left || ks == LM_KS_Right || ks == LM_KS_Home || ks == LM_KS_End ||
                ks == LM_KS_Page_Up || ks == LM_KS_Page_Down)) {
        struct control *c = &m->c[a->focus];
        int step = c->max >= 100 ? 5 : 1, v = c->cur;
        v = ks == LM_KS_Left ? v - step : ks == LM_KS_Right ? v + step : ks == LM_KS_Page_Down ? v - 10 * step :
            ks == LM_KS_Page_Up ? v + 10 * step : ks == LM_KS_Home ? 0 : c->max;
        set_control(m, a->focus, LM_CLAMP(v, 0, c->max));
    } else if (a->focus == C_MUTE && mute_available(m) && (ks == ' ' || ks == LM_KS_Return)) {
        toggle_mute(m);
    }
    win->redraw = true;
}

/* Reads the next control of the first monitor that is not completely read. Returns true while there is more to do. */
static bool probe_step(struct app *a)
{
    for (int k = 0; k < nmon; k++) {
        if (mons[k].probed >= NCTL && mons[k].caps_done)
            continue;
        if (!mons[k].caps_done) {
            probe_caps(&mons[k]);
            a->win.redraw = true;
            return true;
        }
        probe_one(&mons[k], mons[k].probed++);
        a->win.redraw = true;
        return true;
    }
    return false;
}

int main(int argc, char **argv)
{
    lm_theme_load_env();
    fake_file = getenv("MELLAGUI_MONITOR_FAKE");
    if (fake_file && !*fake_file)
        fake_file = NULL;
    discover();
    if (argc > 1 && !strcmp(argv[1], "--list"))
        return cli_list();
    if (argc > 4 && !strcmp(argv[1], "--set")) {
        probe_all();
        return cli_set(argv[2], argv[3], argv[4]);
    }
    if (argc > 1) {
        fprintf(stderr, "usage: mellagui-monitor [--list | --set NAME HEXCODE VALUE]\n");
        return 2;
    }

    static const struct wlwin_ops ops = { .paint = paint, .pointer = pointer_event, .keyboard = keyboard_event };
    struct app a = { .drag = -1, .focus = -1 };
    if (wlwin_connect(&a.win, "mellagui-monitor", &ops, &a) < 0)
        return 1;
    if (wlwin_open(&a.win, "Monitor", "mellagui.monitor", 560, 520, 420, 360) < 0)
        return 1;
    bool probing = true;
    while (wlwin_step(&a.win, probing ? 0 : (a.dirty ? SEND_GAP_MS : -1))) {
        if (probing)
            probing = probe_step(&a);
        if (a.dirty && a.drag >= 0 && now_ms() - a.last_send_ms >= SEND_GAP_MS)
            flush_slider(&a, a.wd[a.drag].ctl);
        if (!probing && !a.dirty) {
            for (int k = 0; k < nmon; k++)
                if (mons[k].probed < NCTL)
                    probing = true;                                 /* Reload was pressed */
        }
    }
    for (int k = 0; k < nmon; k++)
        lm_ddc_close(mons[k].fd);
    wlwin_destroy(&a.win);
    return 0;
}
