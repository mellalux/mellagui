/*
 * mellagui-gadget - the desktop gadgets, each one a program of its own (the compositor starts them when they are switched on).
 *
 *   mellagui-gadget clipboard   the clipboard history (newest first; click = use, x = forget, arrows page, trash = forget all)
 *   mellagui-gadget mixer       the volume mixer (master, output, one slider per playing application)
 *   mellagui-gadget system      CPU, memory, temperature and the device
 *   mellagui-gadget smart       the devices pinned in mellagui-smart (a switch, or open / stop / close for a blind)
 *
 * The window is a frameless card (app id mellagui.gadget.NAME, premultiplied ARGB so that the corners are round): the compositor puts it at the
 * bottom of the window stack on every workspace, docks the gadgets in a column at the top right until one is moved, and never gives it the keyboard
 * on a click. The card moves by its title bar and resizes by its edges: this program asks the compositor (xdg_toplevel.move / resize).
 *
 * The clipboard history comes from the compositor's private mellagui_settings_v1 (clip-0 .. clip-4: ten lines each, clip-count) and the card
 * answers with clip-use / clip-del / clip-clear. Super+C gives the card the keyboard: Up / Down / PageUp / PageDown / Home / End move the
 * cursor, Enter uses the entry, Delete forgets it, Shift+Delete forgets all, Esc gives the keyboard back (focus-back). The mixer, the system
 * monitor and the smart home card use the same modules the compositor had before (mixer.c, sysinfo.c, smarthome.c: child processes and /proc reads
 * from this program's loop).
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "font.h"
#include "keymap.h"
#include "loop.h"
#include "metrics.h"
#include "mixer.h"
#include "protocol.h"
#include "smarthome.h"
#include "sysinfo.h"
#include "theme.h"
#include "uikit.h"
#include "util.h"
#include "wlc.h"
#include "wlwin.h"

#define TITLE_H 26
#define ENTRY_H 36
#define PAD 8
#define DEL_W 26
#define BTN 20
#define BTN_GAP 4
#define EDGE 6
#define CLIP_MAX 50
#define CLIP_LEN 124

enum kind { G_CLIP, G_MIXER, G_SYS, G_SMART };

struct app {
    struct wlwin win;
    enum kind kind;
    struct lm_loop *loop;
    uint32_t settings;
    double px, py;
    bool in;                            /* the pointer is on the card */
    uint32_t title_ms;                  /* time of the last press on the title bar (a double click puts the card back) */
    /* clipboard */
    char chunk[5][3400];
    char text[CLIP_MAX][CLIP_LEN];
    int count;                          /* entries in the history */
    int top;                            /* first entry shown */
    int cursor;                         /* keyboard cursor (an index into the history), -1 = none */
    bool kbd;                           /* the card has the keyboard */
    int hover;                          /* 0 nothing, 1 up, 2 down, 3 clear, 10 + k = the x of the k-th drawn entry */
    /* mixer */
    struct lm_mixer *mixer;
    enum lm_mixer_part drag_part;
    int drag_index;
    /* system */
    struct lm_sysinfo *sys;
    /* smart */
    struct lm_smart *smart;
};

static struct app g_app;
static volatile sig_atomic_t g_refresh;                         /* SIGUSR1: ask the smart home again now (the tests) */

static void on_usr1(int sig)
{
    (void)sig;
    g_refresh = 1;
}

/* For the tests: the state, in the words of the headless control socket's replies, in $MELLAGUI_GADGET_DUMP.NAME after every change. */
static int clip_slots_of(const struct app *a);
static const char *kind_name(enum kind k)
{
    return k == G_CLIP ? "clipboard" : k == G_MIXER ? "mixer" : k == G_SYS ? "system" : "smart";
}

static void dump_state(struct app *a)
{
    const char *base = getenv("MELLAGUI_GADGET_DUMP");
    if (!base || !*base)
        return;
    char path[600], tmp[620];
    snprintf(path, sizeof(path), "%s.%s", base, kind_name(a->kind));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return;
    if (a->kind == G_CLIP) {
        fprintf(f, "ok n=%d gadget=1 top=%d slots=%d cursor=%d kbd=%d w=%d h=%d\n", a->count, a->top, clip_slots_of(a), a->cursor, a->kbd ? 1 : 0,
                a->win.bw, a->win.bh);
    } else if (a->kind == G_MIXER) {
        const struct lm_mixer_state *ms = lm_mixer_state(a->mixer);
        fprintf(f, "ok visible=1 backend=%s ready=%d volume=%d mute=%d streams=%d output=%s\n",
                ms->backend == LM_MIXER_PULSE ? "pulse" : ms->backend == LM_MIXER_WPCTL ? "wpctl" : ms->backend == LM_MIXER_ALSA ? "alsa" : "none",
                ms->ready ? 1 : 0, ms->volume, ms->mute ? 1 : 0, ms->nstreams, ms->output[0] ? ms->output : "-");
    } else if (a->kind == G_SYS) {
        const struct lm_sysinfo_state *ss = lm_sysinfo_state(a->sys);
        fprintf(f, "ok visible=1 ready=%d cpu=%d cores=%d mem=%d mem_total_kb=%ld swap=%d temp=%d load=%.2f hist=%d\n", ss->ready ? 1 : 0, ss->cpu,
                ss->ncores, ss->mem_pct, ss->mem_total_kb, ss->swap_pct, ss->temp_c, ss->load1, ss->nhist);
        fprintf(f, "ok maker=%s|model=%s|cpu=%s|os=%s|kernel=%s|host=%s|uptime=%ld\n", ss->maker, ss->model, ss->cpu_model, ss->os_name, ss->kernel,
                ss->host, ss->uptime_s);
    } else {
        const struct lm_smart_state *ms = lm_smart_state(a->smart);
        fprintf(f, "smart visible=1 ready=%d n=%d note=%s\n", ms->ready ? 1 : 0, ms->n, ms->note[0] ? ms->note : "-");
        for (int i = 0; i < ms->n; i++)
            fprintf(f, "item %d %s kind=%s online=%d on=%d percent=%d\n", i, ms->item[i].name,
                    ms->item[i].kind == LM_SMART_SWITCH ? "switch" : ms->item[i].kind == LM_SMART_BLIND ? "blind" : "none", ms->item[i].online ? 1 : 0,
                    ms->item[i].on ? 1 : 0, ms->item[i].percent);
        fprintf(f, "ok\n");
    }
    fclose(f);
    if (rename(tmp, path) < 0)
        unlink(tmp);
}

/* ---- small drawing helpers (the card is the whole window) ------------------------------------------------------------------------ */

static uint32_t veil_mix(int a)         /* the desktop's light touch on a dark card (hairlines, hovered rows) as an opaque colour */
{
    return lm_mix_rgb(lm_theme.card, lm_theme.veil, (double)a / 255.0);
}

static uint32_t over(uint32_t base, uint32_t top, int a)
{
    return lm_mix_rgb(base, top, (double)a / 255.0);
}

static void rr(struct wlwin *w, struct lm_rect r, int rad, uint32_t rgb)
{
    wlwin_round_fill(w, r.x, r.y, r.w, r.h, rad, rgb);
}

static void rect(struct wlwin *w, struct lm_rect r, uint32_t rgb)
{
    wlwin_fill(w, r.x, r.y, r.w, r.h, rgb);
}

static void text(struct wlwin *w, int x, int y, const char *s, uint32_t rgb)
{
    wlwin_text(w, x, y, s, 1, rgb, 0);
}

static void text_centered(struct wlwin *w, struct lm_rect r, const char *s, uint32_t rgb)
{
    int tw = lm_font_text_width(s);
    wlwin_text(w, r.x + (r.w - tw) / 2, r.y + (r.h - LM_FONT_H) / 2, s, 1, rgb, 0);
}

/* One line cut to `maxw` pixels, ending in ".." when it does not fit. */
static void text_fit(struct wlwin *w, int x, int y, const char *s, int maxw, uint32_t rgb)
{
    if (lm_font_text_width(s) <= maxw) {
        text(w, x, y, s, rgb);
        return;
    }
    char buf[256];
    int n = lm_font_fit(s, maxw - lm_font_text_width(".."));
    if (n < 0)
        n = 0;
    if (n > (int)sizeof(buf) - 4)
        n = (int)sizeof(buf) - 4;
    memcpy(buf, s, (size_t)n);
    strcpy(buf + n, "..");
    text(w, x, y, buf, rgb);
}

/* Up to `lines` lines of `s` wrapped at blanks to `maxw` pixels. */
static void text_wrap(struct wlwin *w, int x, int y, const char *s, int maxw, int lines, uint32_t rgb)
{
    for (int l = 0; l < lines && *s; l++) {
        int n = lm_font_fit(s, maxw), len = (int)strlen(s);
        if (n < len) {
            int cut = n;
            while (cut > 0 && s[cut] != ' ')
                cut--;
            if (cut > 0)
                n = cut;
        }
        if (n <= 0)
            n = len;
        char buf[256];
        int m = n < (int)sizeof(buf) - 1 ? n : (int)sizeof(buf) - 1;
        memcpy(buf, s, (size_t)m);
        buf[m] = 0;
        text(w, x, y + l * (LM_FONT_H + 2), buf, rgb);
        s += n;
        while (*s == ' ')
            s++;
    }
}

static void card_base(struct wlwin *w)
{
    wlwin_round_fill(w, 0, 0, w->bw, w->bh, 12, veil_mix(0x2e));         /* the hairline: the 1 px ring around the body */
    wlwin_round_fill(w, 1, 1, w->bw - 2, w->bh - 2, 11, lm_theme.card);
}

static void grip(struct wlwin *w)
{
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 3 - r; k++)
            wlwin_fill(w, w->bw - 11 - k * 4, w->bh - 11 - r * 4, 2, 2, veil_mix(0x70));
}

static void header(struct wlwin *w, const char *title, int tx)
{
    wlwin_text_bold(w, tx, (TITLE_H - LM_FONT_H) / 2, title, 1, lm_theme.text, 0);
    wlwin_fill(w, 12, TITLE_H - 1, w->bw - 24, 1, veil_mix(0x30));
    grip(w);
}

static void pill_badge(struct wlwin *w, const char *t, int right_edge, uint32_t col)
{
    int bw = lm_font_text_width(t) + 14;
    struct lm_rect pill = lm_rect_make(right_edge - bw, (TITLE_H - 16) / 2, bw, 16);
    rr(w, pill, 8, lm_theme.button);
    text_centered(w, pill, t, col);
}

/* Little icons made of rectangles. */
static void tri_up(struct wlwin *w, int cx, int cy, int h, uint32_t col)
{
    for (int i = 0; i < h; i++)
        wlwin_fill(w, cx - i, cy - h / 2 + i, 2 * i + 1, 1, col);
}

static void tri_down(struct wlwin *w, int cx, int cy, int h, uint32_t col)
{
    for (int i = 0; i < h; i++)
        wlwin_fill(w, cx - (h - 1 - i), cy - h / 2 + i, 2 * (h - 1 - i) + 1, 1, col);
}

static void cross(struct wlwin *w, int cx, int cy, int r, uint32_t col)
{
    for (int k = -r; k <= r; k++) {
        wlwin_fill(w, cx + k, cy + k, 2, 2, col);
        wlwin_fill(w, cx + k, cy - k, 2, 2, col);
    }
}

static void trash(struct wlwin *w, int cx, int cy, uint32_t col)
{
    wlwin_fill(w, cx - 5, cy - 4, 11, 2, col);                      /* lid */
    wlwin_fill(w, cx - 2, cy - 6, 5, 2, col);                       /* handle */
    wlwin_fill(w, cx - 4, cy - 2, 9, 9, col);                       /* body */
    for (int i = -1; i <= 1; i += 2)
        wlwin_fill(w, cx + i * 2, cy, 1, 5, lm_theme.card);         /* slits */
}

/* Speaker glyph in `r`: cone and, unless muted, two sound waves; a red slash when muted. */
static void speaker(struct wlwin *w, struct lm_rect r, bool muted, uint32_t col)
{
    int cx = r.x + r.w / 2 - 3, cy = r.y + r.h / 2;
    wlwin_round_fill(w, cx - 5, cy - 3, 4, 6, 1, col);
    for (int i = 0; i < 6; i++)
        wlwin_fill(w, cx - 1 + i, cy - 3 - i, 1, 6 + 2 * i, col);
    if (!muted) {
        uint32_t wave = over(lm_theme.card, col, 0xb0);
        for (int k = 1; k <= 2; k++)
            for (int j = -k - 1; j <= k + 1; j++)
                wlwin_fill(w, cx + 7 + k * 3 - (j * j) / (k + 2), cy + j * 2, 1, 2, wave);
    } else {
        for (int k = -7; k <= 7; k++)
            wlwin_fill(w, r.x + r.w / 2 + k, r.y + r.h / 2 + k, 2, 2, lm_theme.alert);
    }
}

/* A horizontal slider: track, filled part, knob; `value` 0..100; the percentage is written to its right. */
static void slider(struct wlwin *w, struct lm_rect s, int value, bool muted)
{
    int ty = s.y + s.h / 2 - 3;
    rr(w, lm_rect_make(s.x, ty, s.w, 6), 3, lm_theme.button);
    int fw = s.w * LM_CLAMP(value, 0, 100) / 100;
    if (fw > 0)
        rr(w, lm_rect_make(s.x, ty, fw, 6), 3, muted ? lm_theme.text_dim : lm_theme.accent);
    rr(w, lm_rect_make(s.x + fw - 6, s.y + s.h / 2 - 7, 14, 14), 7, muted ? lm_theme.muted : lm_theme.text);
    char t[12];
    snprintf(t, sizeof(t), "%d%%", value);
    text(w, s.x + s.w + 8, s.y + (s.h - LM_FONT_H) / 2, t, muted ? lm_theme.text_dim : lm_theme.text);
}

/* ---- clipboard ------------------------------------------------------------------------------------------------------------------ */

static int clip_slots(const struct wlwin *w)
{
    return LM_CLAMP((w->bh - TITLE_H - PAD) / ENTRY_H, 1, CLIP_MAX);
}

static int clip_slots_of(const struct app *a)
{
    return clip_slots(&a->win);
}

static struct lm_rect clip_btn(const struct wlwin *w, int k)
{
    return lm_rect_make(w->bw - 10 - (3 - k) * (BTN + BTN_GAP) + BTN_GAP, (TITLE_H - BTN) / 2, BTN, BTN);
}

static struct lm_rect clip_slot(const struct wlwin *w, int i)
{
    return lm_rect_make(12, TITLE_H + 4 + i * ENTRY_H, w->bw - 24, ENTRY_H - 4);
}

static struct lm_rect clip_del(const struct wlwin *w, int i)
{
    struct lm_rect s = clip_slot(w, i);
    return lm_rect_make(s.x + s.w - DEL_W, s.y + (s.h - 22) / 2, DEL_W - 4, 22);
}

static bool in_rect(struct lm_rect r, double x, double y)
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static void clip_clamp(struct app *a)
{
    int slots = clip_slots(&a->win);
    if (a->cursor >= a->count)
        a->cursor = a->count - 1;
    if (a->kbd && a->cursor < 0 && a->count)
        a->cursor = 0;
    if (a->cursor >= 0) {                                           /* the cursor stays in view */
        if (a->cursor < a->top)
            a->top = a->cursor;
        if (a->cursor >= a->top + slots)
            a->top = a->cursor - slots + 1;
    }
    if (a->top > a->count - slots)
        a->top = a->count - slots;
    if (a->top < 0)
        a->top = 0;
}

static void clip_parse(struct app *a)
{
    char newest[CLIP_LEN];
    snprintf(newest, sizeof(newest), "%s", a->count ? a->text[0] : "");
    int had = a->count;
    a->count = 0;
    for (int c = 0; c < 5; c++) {
        const char *p = a->chunk[c];
        while (*p && a->count < CLIP_MAX) {
            const char *nl = strchr(p, '\n');
            size_t len = nl ? (size_t)(nl - p) : strlen(p);
            if (len >= CLIP_LEN)
                len = CLIP_LEN - 1;
            memcpy(a->text[a->count], p, len);
            a->text[a->count][len] = 0;
            a->count++;
            if (!nl)
                break;
            p = nl + 1;
        }
    }
    if (a->count > had || (a->count && strcmp(newest, a->text[0])))
        a->top = 0;                                                 /* something new was copied (or an old one used): the newest is in view */
    clip_clamp(a);
}

static void send_set(struct app *a, const char *key, const char *value)
{
    if (a->settings)
        WLC_SEND(a->win.c, a->settings, LM_MELLAGUI_SETTINGS_V1_REQ_SET, { .s = key }, { .s = value });
}

static void send_index(struct app *a, const char *key, int idx)
{
    char v[16];
    snprintf(v, sizeof(v), "%d", idx);
    send_set(a, key, v);
}

static void paint_clip(struct app *a)
{
    struct wlwin *w = &a->win;
    card_base(w);
    /* header: a clipboard glyph, the title, the count */
    struct lm_rect icon = lm_rect_make(14, (TITLE_H - 14) / 2, 11, 14);
    rr(w, icon, 2, lm_theme.accent);
    rr(w, lm_rect_make(icon.x + 3, icon.y - 2, 5, 4), 1, lm_theme.text);
    rr(w, lm_rect_make(icon.x + 2, icon.y + 4, 7, 8), 1, lm_theme.card);
    header(w, "Clipboard", 32);
    char badge[8];
    snprintf(badge, sizeof(badge), "%d", a->count);
    if (clip_btn(w, 0).x - 8 - (lm_font_text_width(badge) + 14) > 112)
        pill_badge(w, badge, clip_btn(w, 0).x - 8, lm_theme.accent);

    int slots = clip_slots(w);
    static const int hov[3] = { 1, 2, 3 };
    for (int k = 0; k < 3; k++) {                                   /* up and down (a page; dim when there is nothing more that way), clear all */
        struct lm_rect b = clip_btn(w, k);
        bool can = k == 0 ? a->top > 0 : k == 1 ? a->top + slots < a->count : a->count > 0;
        bool hot = a->hover == hov[k];
        if (hot && can)
            rr(w, b, 6, veil_mix(0x34));
        uint32_t col = !can ? veil_mix(0x58) : hot && k == 2 ? lm_theme.danger : lm_theme.text;
        int cx = b.x + b.w / 2, cy = b.y + b.h / 2;
        if (k == 0)
            tri_up(w, cx, cy, 5, col);
        else if (k == 1)
            tri_down(w, cx, cy, 5, col);
        else
            trash(w, cx, cy, col);
    }
    if (!a->count) {
        text_centered(w, lm_rect_make(1, TITLE_H, w->bw - 2, ENTRY_H), "Nothing copied yet", lm_theme.text_dim);
        return;
    }
    for (int i = 0; i < slots && a->top + i < a->count; i++) {
        int e = a->top + i;
        struct lm_rect slot = clip_slot(w, i);
        bool newest = e == 0;
        rr(w, slot, 7, veil_mix(newest ? 0x26 : 0x12));
        if (newest)
            rr(w, lm_rect_make(slot.x + 4, slot.y + 6, 3, slot.h - 12), 1, lm_theme.accent);
        if (a->kbd && e == a->cursor) {                             /* keyboard cursor: an accent outline */
            wlwin_fill(w, slot.x, slot.y, slot.w, 2, lm_theme.accent);
            wlwin_fill(w, slot.x, slot.y + slot.h - 2, slot.w, 2, lm_theme.accent);
            wlwin_fill(w, slot.x, slot.y, 2, slot.h, lm_theme.accent);
            wlwin_fill(w, slot.x + slot.w - 2, slot.y, 2, slot.h, lm_theme.accent);
        }
        struct lm_rect del = clip_del(w, i);
        bool del_hot = a->hover == 10 + i;
        if (del_hot)
            rr(w, del, 6, veil_mix(0x34));
        cross(w, del.x + del.w / 2, del.y + del.h / 2, 4, del_hot ? lm_theme.danger : over(veil_mix(0x12), lm_theme.text_dim, 0xa0));
        text_fit(w, slot.x + 14, slot.y + (slot.h - LM_FONT_H) / 2, a->text[e], slot.w - 14 - DEL_W - 6, newest ? lm_theme.text : lm_theme.text_dim);
    }
}

static int clip_hover(struct app *a, double x, double y)
{
    struct wlwin *w = &a->win;
    for (int k = 0; k < 3; k++)
        if (in_rect(clip_btn(w, k), x, y))
            return k + 1;
    int slots = clip_slots(w);
    for (int i = 0; i < slots && a->top + i < a->count; i++)
        if (in_rect(clip_del(w, i), x, y))
            return 10 + i;
    return 0;
}

static bool clip_press(struct app *a)
{
    struct wlwin *w = &a->win;
    int slots = clip_slots(w);
    int h = clip_hover(a, a->px, a->py);
    if (h == 1) {
        a->top -= slots;
    } else if (h == 2) {
        a->top += slots;
    } else if (h == 3) {
        send_set(a, "clip-clear", "1");
    } else if (h >= 10) {
        send_index(a, "clip-del", a->top + h - 10);
    } else {
        for (int i = 0; i < slots && a->top + i < a->count; i++)
            if (in_rect(clip_slot(w, i), a->px, a->py)) {
                send_index(a, "clip-use", a->top + i);
                return true;
            }
        return false;
    }
    clip_clamp(a);
    w->redraw = true;
    return true;
}

static void clip_key(struct app *a, uint32_t ks)
{
    int slots = clip_slots(&a->win);
    if (ks == LM_KS_Escape) {
        send_set(a, "focus-back", "1");
        return;
    }
    if (ks == LM_KS_Down)
        a->cursor++;
    else if (ks == LM_KS_Up)
        a->cursor--;
    else if (ks == LM_KS_Page_Down)
        a->cursor += slots;
    else if (ks == LM_KS_Page_Up)
        a->cursor -= slots;
    else if (ks == LM_KS_Home)
        a->cursor = 0;
    else if (ks == LM_KS_End)
        a->cursor = a->count - 1;
    else if ((ks == LM_KS_Return || ks == LM_KS_KP_Enter) && a->cursor >= 0 && a->cursor < a->count) {
        send_index(a, "clip-use", a->cursor);
        send_set(a, "focus-back", "1");
        return;
    } else if (ks == LM_KS_Delete) {
        if (a->win.mods & LM_MOD_SHIFT) {
            send_set(a, "clip-clear", "1");
            send_set(a, "focus-back", "1");                         /* nothing left to choose from */
        } else if (a->cursor >= 0 && a->cursor < a->count)
            send_index(a, "clip-del", a->cursor);
    }
    if (a->cursor < 0)
        a->cursor = 0;
    clip_clamp(a);
}

/* ---- mixer ---------------------------------------------------------------------------------------------------------------------- */

static void mixer_layout(struct app *a, struct lm_mixer_layout *l)
{
    const struct lm_mixer_state *st = lm_mixer_state(a->mixer);
    struct lm_rect card = lm_rect_make(0, 0, a->win.bw, a->win.bh);
    lm_mixer_layout(&card, TITLE_H, st->nstreams, st->backend == LM_MIXER_PULSE, l);
}

static void mixer_slide(struct app *a, enum lm_mixer_part part, int idx, double x)
{
    struct lm_mixer_layout l;
    mixer_layout(a, &l);
    if (part == LM_MIXER_MASTER_SLIDER)
        lm_mixer_set_master(a->mixer, lm_mixer_slider_value(l.master_slider.x, l.master_slider.w, x));
    else if (part == LM_MIXER_STREAM_SLIDER && idx >= 0 && idx < l.nstreams)
        lm_mixer_set_stream(a->mixer, idx, lm_mixer_slider_value(l.stream[idx].slider.x, l.stream[idx].slider.w, x));
}

static bool mixer_press(struct app *a)
{
    struct lm_mixer_layout l;
    mixer_layout(a, &l);
    int idx;
    enum lm_mixer_part part = lm_mixer_hit(&l, (int)a->px, (int)a->py, &idx);
    switch (part) {
    case LM_MIXER_MASTER_MUTE: lm_mixer_toggle_master_mute(a->mixer); return true;
    case LM_MIXER_OUTPUT: lm_mixer_next_output(a->mixer); return true;
    case LM_MIXER_STREAM_MUTE: lm_mixer_toggle_stream_mute(a->mixer, idx); return true;
    case LM_MIXER_MASTER_SLIDER:
    case LM_MIXER_STREAM_SLIDER:
        mixer_slide(a, part, idx, a->px);
        a->drag_part = part;
        a->drag_index = idx;
        return true;
    default:
        return false;
    }
}

static void paint_mixer(struct app *a)
{
    struct wlwin *w = &a->win;
    const struct lm_mixer_state *st = lm_mixer_state(a->mixer);
    card_base(w);
    speaker(w, lm_rect_make(10, (TITLE_H - 20) / 2, 20, 20), st->mute, lm_theme.accent);
    header(w, "Volume", 36);
    char badge[16];
    if (st->backend == LM_MIXER_NONE)
        snprintf(badge, sizeof(badge), "--");
    else if (st->mute)
        snprintf(badge, sizeof(badge), "muted");
    else
        snprintf(badge, sizeof(badge), "%d%%", st->volume);
    pill_badge(w, badge, w->bw - 14, st->mute ? lm_theme.alert : lm_theme.accent);
    if (st->backend == LM_MIXER_NONE) {
        text_centered(w, lm_rect_make(12, TITLE_H, w->bw - 24, w->bh - TITLE_H - 12),
                      st->ready ? "No mixer found (pactl or amixer)" : "Looking for a mixer...", lm_theme.text_dim);
        return;
    }
    bool pulse = st->backend == LM_MIXER_PULSE;
    struct lm_mixer_layout l;
    mixer_layout(a, &l);
    rr(w, l.master_mute, 6, st->mute ? over(lm_theme.card, lm_theme.alert, 0x40) : veil_mix(0x26));
    speaker(w, l.master_mute, st->mute, st->mute ? lm_theme.alert : lm_theme.text);
    slider(w, l.master_slider, st->volume, st->mute);
    if (pulse) {
        rr(w, l.output, 7, veil_mix(0x1a));
        char label[96];
        snprintf(label, sizeof(label), "%s%s", st->output, st->nsinks > 1 ? "   >" : "");
        text(w, l.output.x + 10, l.output.y + (l.output.h - LM_FONT_H) / 2, label, lm_theme.text);
    }
    for (int i = 0; i < l.nstreams; i++) {
        const struct lm_mixer_stream *s = &st->streams[i];
        text_fit(w, l.stream[i].label.x, l.stream[i].label.y, s->name, l.stream[i].label.w, s->mute ? lm_theme.text_dim : lm_theme.text);
        rr(w, l.stream[i].mute, 5, s->mute ? over(lm_theme.card, lm_theme.alert, 0x40) : veil_mix(0x26));
        speaker(w, lm_rect_make(l.stream[i].mute.x + 2, l.stream[i].mute.y, 18, 20), s->mute, s->mute ? lm_theme.alert : lm_theme.text);
        slider(w, l.stream[i].slider, s->volume, s->mute);
    }
    if (l.hint.w > 0 && l.nstreams == st->nstreams) {
        const char *hint = st->backend == LM_MIXER_ALSA ? "ALSA Master (no per-app volume)" :
                           st->backend == LM_MIXER_WPCTL ? "Install pulseaudio-utils for apps" :
                           st->nstreams ? "" : "Applications that play sound show up here.";
        if (*hint)
            text_fit(w, l.hint.x, l.hint.y + 2, hint, l.hint.w, lm_theme.text_dim);
    }
}

/* ---- system monitor ------------------------------------------------------------------------------------------------------------- */

static uint32_t level_color(int pct, int warn, int crit)
{
    return pct >= crit ? lm_theme.alert : pct >= warn ? lm_theme.warn : lm_theme.accent;
}

/* A labelled horizontal bar: `label` left, `value` text right, a track with a fill below. */
static void meter(struct wlwin *w, int x, int y, int width, const char *label, const char *value, int pct, uint32_t col)
{
    text(w, x, y, label, lm_theme.text_dim);
    text(w, x + width - lm_font_text_width(value), y, value, lm_theme.text);
    rr(w, lm_rect_make(x, y + 17, width, 6), 3, lm_theme.button);
    int fw = width * LM_CLAMP(pct, 0, 100) / 100;
    if (fw >= 2)
        rr(w, lm_rect_make(x, y + 17, fw, 6), 3, col);
}

static void paint_system(struct app *a)
{
    struct wlwin *w = &a->win;
    const struct lm_sysinfo_state *st = lm_sysinfo_state(a->sys);
    card_base(w);
    struct lm_rect chip = lm_rect_make(14, (TITLE_H - 12) / 2, 12, 12);
    rr(w, chip, 2, lm_theme.accent);
    rr(w, lm_rect_make(chip.x + 3, chip.y + 3, 6, 6), 1, lm_theme.card);
    for (int i = 0; i < 3; i++) {                                   /* pins */
        wlwin_fill(w, chip.x + 2 + i * 4, chip.y - 3, 1, 3, lm_theme.muted);
        wlwin_fill(w, chip.x + 2 + i * 4, chip.y + 12, 1, 3, lm_theme.muted);
        wlwin_fill(w, chip.x - 3, chip.y + 2 + i * 4, 3, 1, lm_theme.muted);
        wlwin_fill(w, chip.x + 12, chip.y + 2 + i * 4, 3, 1, lm_theme.muted);
    }
    header(w, "System", 38);
    char badge[16];
    snprintf(badge, sizeof(badge), "%d%%", st->cpu);
    pill_badge(w, badge, w->bw - 14, level_color(st->cpu, 70, 90));
    if (!st->ready) {
        text_centered(w, lm_rect_make(12, TITLE_H, w->bw - 24, w->bh - TITLE_H - 12), "Reading...", lm_theme.text_dim);
        return;
    }
    int x = 12, width = w->bw - 24, y = TITLE_H + 6, bottom = w->bh - 8;
    /* What fits, in order of importance: CPU, memory, temperature are always drawn (the minimum card height holds them), then the CPU graph,
     * the per-core bars and swap as room allows. */
    int room = bottom - y - (28 + 30 + 30);
    bool graph = room >= 32, cores, swap;
    if (graph)
        room -= 32;
    cores = room >= 22 && st->ncores > 1;
    if (cores)
        room -= 22;
    swap = room >= 30 && st->swap_total_kb > 0;

    char val[64];
    snprintf(val, sizeof(val), "%d%%   load %.2f", st->cpu, st->load1);
    meter(w, x, y, width, "CPU", val, st->cpu, level_color(st->cpu, 70, 90));
    y += 28;
    if (graph) {                                                    /* the last minute, one bar per second */
        struct lm_rect area = lm_rect_make(x, y, width, 26);
        rr(w, area, 5, veil_mix(0x12));
        int n = st->nhist, bwid = LM_MAX(1, (width - 4) / LM_SYS_HISTORY);
        for (int i = 0; i < n; i++) {
            int v = st->hist[i], hh = LM_MAX(1, (area.h - 6) * v / 100);
            struct lm_rect bar = lm_rect_make(area.x + area.w - 2 - (n - i) * bwid, area.y + area.h - 3 - hh, LM_MAX(1, bwid - 1), hh);
            if (bar.x >= area.x + 1)
                rect(w, bar, over(lm_theme.card, level_color(v, 70, 90), 0xd0));
        }
        y += 32;
    }
    if (cores) {                                                    /* one small bar per logical CPU */
        int n = st->ncores, gap = 3, bwid = LM_MAX(3, (width - gap * (n - 1)) / n);
        for (int i = 0; i < n; i++) {
            struct lm_rect slot = lm_rect_make(x + i * (bwid + gap), y, bwid, 16);
            if (slot.x + slot.w > x + width)
                break;
            rr(w, slot, 2, lm_theme.button);
            int hh = 16 * st->core[i] / 100;
            if (hh > 0)
                rr(w, lm_rect_make(slot.x, slot.y + 16 - hh, slot.w, hh), 2, level_color(st->core[i], 70, 90));
        }
        y += 22;
    }
    snprintf(val, sizeof(val), "%.1f / %.1f GiB   %d%%", (double)(st->mem_total_kb - st->mem_avail_kb) / 1048576.0,
             (double)st->mem_total_kb / 1048576.0, st->mem_pct);
    meter(w, x, y, width, "Memory", val, st->mem_pct, level_color(st->mem_pct, 80, 92));
    y += 30;
    if (swap) {
        snprintf(val, sizeof(val), "%.1f / %.1f GiB   %d%%", (double)(st->swap_total_kb - st->swap_free_kb) / 1048576.0,
                 (double)st->swap_total_kb / 1048576.0, st->swap_pct);
        meter(w, x, y, width, "Swap", val, st->swap_pct, level_color(st->swap_pct, 30, 70));
        y += 30;
    }
    if (st->temp_c >= 0) {
        snprintf(val, sizeof(val), "%d \xc2\xb0""C", st->temp_c);
        int pct = LM_CLAMP((st->temp_c - 20) * 100 / 80, 0, 100);   /* the bar spans 20..100 degrees */
        meter(w, x, y, width, "Temperature", val, pct, st->temp_c >= 85 ? lm_theme.alert : st->temp_c >= 70 ? lm_theme.warn : lm_theme.ok);
    } else {
        meter(w, x, y, width, "Temperature", "no sensor", 0, lm_theme.accent);
    }
    y += 30;

    char up[24];                                                    /* the device: what room is left, in order of importance */
    lm_sysinfo_format_uptime(st->uptime_s, up, sizeof(up));
    const struct { const char *label, *value; } info[] = {
        { "Model", st->model }, { "Maker", st->maker }, { "CPU", st->cpu_model }, { "OS", st->os_name },
        { "Kernel", st->kernel }, { "Host", st->host }, { "Uptime", st->uptime_s >= 0 ? up : "" },
    };
    int rows = (bottom - y - 4) / 16;
    if (rows > 0) {
        wlwin_fill(w, x, y - 6, width, 1, veil_mix(0x30));
        int vx = x + 64;
        for (size_t i = 0; i < sizeof(info) / sizeof(info[0]) && rows > 0; i++) {
            if (!info[i].value[0])
                continue;
            text(w, x, y, info[i].label, lm_theme.text_dim);
            text_fit(w, vx, y, info[i].value, x + width - vx, lm_theme.text);
            y += 16;
            rows--;
        }
    }
}

/* ---- smart home ----------------------------------------------------------------------------------------------------------------- */

static void smart_layout(struct app *a, struct lm_smart_layout *l)
{
    const struct lm_smart_state *st = lm_smart_state(a->smart);
    lm_smart_layout(0, 0, a->win.bw, a->win.bh, TITLE_H, st->n, l);
}

static bool smart_press(struct app *a)
{
    const struct lm_smart_state *st = lm_smart_state(a->smart);
    struct lm_smart_layout l;
    smart_layout(a, &l);
    int idx = 0;
    enum lm_smart_part part = lm_smart_hit(&l, st, (int)a->px, (int)a->py, &idx);
    if (part == LM_SMART_SWITCH_PART)
        lm_smart_toggle(a->smart, idx);
    else if (part != LM_SMART_NOTHING)
        lm_smart_blind(a->smart, idx, part - LM_SMART_OPEN);
    return part != LM_SMART_NOTHING;
}

static void paint_smart(struct app *a)
{
    struct wlwin *w = &a->win;
    const struct lm_smart_state *st = lm_smart_state(a->smart);
    card_base(w);
    int iy = (TITLE_H - 14) / 2;                                    /* a small house: walls and a stepped roof */
    wlwin_fill(w, 14, iy + 7, 12, 7, lm_theme.accent);
    for (int i = 0; i < 4; i++)
        wlwin_fill(w, 20 - (i + 1) * 2 + 1, iy + 3 + i, (i + 1) * 4 - 2, 1, lm_theme.accent);
    header(w, "Smart home", 36);
    if (!st->n) {
        const char *note = st->note[0] ? st->note : st->ready ? "Nothing is pinned yet." : "Asking the smart home ...";
        text_wrap(w, 14, TITLE_H + 8, note, w->bw - 28, 4, lm_theme.text_dim);
        return;
    }
    struct lm_smart_layout l;
    smart_layout(a, &l);
    for (int i = 0; i < l.rows; i++) {
        const struct lm_smart_item *it = &st->item[i];
        struct lm_rect row = lm_rect_make(8, TITLE_H + 4 + i * LM_SMART_ROW_H, w->bw - 16, LM_SMART_ROW_H - 4);
        rr(w, row, 7, veil_mix(0x12));
        int namew = it->kind == LM_SMART_BLIND ? l.btn[i][0].x - 8 - 18 : it->kind == LM_SMART_SWITCH ? l.sw[i].x - 8 - 18 : w->bw - 36;
        char label[80];
        if (it->kind == LM_SMART_BLIND && it->percent >= 0)
            snprintf(label, sizeof(label), "%s  %d%%", it->name, it->percent);
        else
            snprintf(label, sizeof(label), "%s", it->name);
        text_fit(w, 18, row.y + (row.h - LM_FONT_H) / 2, label, namew, it->online ? lm_theme.text : lm_theme.text_dim);
        if (!it->online) {
            int ow = lm_font_text_width("offline");
            text(w, row.x + row.w - 8 - ow, row.y + (row.h - LM_FONT_H) / 2, "offline", lm_theme.text_dim);
            continue;
        }
        if (it->kind == LM_SMART_SWITCH) {
            struct lm_rect tr = lm_rect_make(l.sw[i].x, l.sw[i].y, l.sw[i].w, l.sw[i].h);
            rr(w, tr, 11, it->on ? lm_theme.ok : lm_theme.button);
            int kx = it->on ? tr.x + tr.w - 11 : tr.x + 11;
            rr(w, lm_rect_make(kx - 8, tr.y + tr.h / 2 - 8, 16, 16), 8, lm_theme.text);
        } else if (it->kind == LM_SMART_BLIND) {
            for (int b = 0; b < 3; b++) {
                struct lm_rect br = lm_rect_make(l.btn[i][b].x, l.btn[i][b].y, l.btn[i][b].w, l.btn[i][b].h);
                rr(w, br, 8, lm_theme.button);
                int cx = br.x + br.w / 2, cy = br.y + br.h / 2;
                if (b == 0)
                    tri_up(w, cx, cy, 6, lm_theme.text);
                else if (b == 1)
                    rr(w, lm_rect_make(cx - 4, cy - 4, 8, 8), 1, lm_theme.text);        /* stop: a small square */
                else
                    tri_down(w, cx, cy, 6, lm_theme.text);
            }
        }
    }
}

/* ---- window --------------------------------------------------------------------------------------------------------------------- */

static void paint(struct wlwin *w)
{
    struct app *a = w->data;
    switch (a->kind) {
    case G_CLIP: paint_clip(a); break;
    case G_MIXER: paint_mixer(a); break;
    case G_SYS: paint_system(a); break;
    case G_SMART: paint_smart(a); break;
    }
    dump_state(a);
}

static void changed(void *data)
{
    struct app *a = data;
    a->win.redraw = true;
}

static void settings_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)c; (void)id;
    struct app *a = data;
    if (op == LM_MELLAGUI_SETTINGS_V1_EVT_DONE) {
        clip_parse(a);
        a->win.redraw = true;
        return;
    }
    if (op != LM_MELLAGUI_SETTINGS_V1_EVT_VALUE)
        return;
    const char *k = args[0].s;
    if (!strncmp(k, "clip-", 5) && k[5] >= '0' && k[5] <= '4' && !k[6])
        snprintf(a->chunk[k[5] - '0'], sizeof(a->chunk[0]), "%s", args[1].s);
}

static void global_event(struct wlwin *w, uint32_t name, const char *iface, uint32_t ver)
{
    (void)ver;
    struct app *a = w->data;
    if (!strcmp(iface, "mellagui_settings_v1"))
        a->settings = wlc_bind(w->c, w->registry, name, &lm_mellagui_settings_v1_interface, 1, settings_event, a);
}

static uint32_t edges_at(const struct wlwin *w, double x, double y)
{
    uint32_t e = 0;
    if (x < EDGE)
        e |= 4;                                                     /* xdg_toplevel resize_edge: top 1, bottom 2, left 4, right 8 */
    if (x >= w->bw - EDGE)
        e |= 8;
    if (y >= w->bh - EDGE)
        e |= 2;
    if (y < 3)
        e |= 1;                                                     /* (the title bar is below: the top zone is thin) */
    return e;
}

static void pointer_event(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
    struct app *a = w->data;
    if (op == LM_WL_POINTER_EVT_MOTION || op == LM_WL_POINTER_EVT_ENTER) {
        a->px = lm_fixed_to_double(args[op == LM_WL_POINTER_EVT_ENTER ? 2 : 1].f);
        a->py = lm_fixed_to_double(args[op == LM_WL_POINTER_EVT_ENTER ? 3 : 2].f);
        a->in = true;
        if (a->kind == G_CLIP) {
            int h = clip_hover(a, a->px, a->py);
            if (h != a->hover) {
                a->hover = h;
                w->redraw = true;
            }
        } else if (a->kind == G_MIXER && a->drag_part != LM_MIXER_NOTHING) {
            mixer_slide(a, a->drag_part, a->drag_index, a->px);
        }
    } else if (op == LM_WL_POINTER_EVT_LEAVE) {
        a->in = false;
        if (a->hover) {
            a->hover = 0;
            w->redraw = true;
        }
    } else if (op == LM_WL_POINTER_EVT_AXIS && args[1].u == 0 && a->kind == G_CLIP) {
        double v = lm_fixed_to_double(args[2].f);
        a->top += v > 0 ? 1 : v < 0 ? -1 : 0;
        clip_clamp(a);
        w->redraw = true;
    } else if (op == LM_WL_POINTER_EVT_BUTTON && args[2].u == 272) {
        if (args[3].u == 0) {                                       /* released */
            a->drag_part = LM_MIXER_NOTHING;
            return;
        }
        uint32_t serial = args[0].u;
        uint32_t e = edges_at(w, a->px, a->py);
        if (e) {
            wlwin_resize(w, serial, e);
            return;
        }
        bool handled = false;
        switch (a->kind) {
        case G_CLIP: handled = clip_press(a); break;
        case G_MIXER: handled = mixer_press(a); break;
        case G_SMART: handled = smart_press(a); break;
        default: break;
        }
        if (!handled && a->py < TITLE_H) {
            uint32_t now = args[1].u;
            if (a->title_ms && now - a->title_ms < (uint32_t)lm_metrics.double_click_ms) {
                a->title_ms = 0;
                send_set(a, "gadget-reset", kind_name(a->kind));
            } else {
                a->title_ms = now;
                wlwin_move(w, serial);
            }
        }
        w->redraw = true;
    }
}

static void keyboard_event(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
    struct app *a = w->data;
    if (a->kind != G_CLIP)
        return;
    if (op == LM_WL_KEYBOARD_EVT_ENTER) {
        a->kbd = true;
        a->cursor = a->count ? 0 : -1;
        a->top = 0;
        w->redraw = true;
    } else if (op == LM_WL_KEYBOARD_EVT_LEAVE) {
        a->kbd = false;
        a->cursor = -1;
        w->redraw = true;
    } else if (op == LM_WL_KEYBOARD_EVT_KEY && args[3].u != 0) {
        clip_key(a, lm_keymap_keysym(args[2].u, w->mods));
        w->redraw = true;
    }
}

static bool close_ask(struct wlwin *w)
{
    (void)w;
    return true;
}

int main(int argc, char **argv)
{
    wlwin_hidpi();
    wlwin_argb();
    lm_theme_load_env();
    static const struct { const char *name, *title; int w, h, min_w, min_h; } def[] = {
        [G_CLIP] = { "clipboard", "Clipboard", 0, 0, 180, TITLE_H + ENTRY_H + PAD },
        [G_MIXER] = { "mixer", "Volume", 0, 0, 210, 120 },
        [G_SYS] = { "system", "System", 0, 0, 210, 130 },
        [G_SMART] = { "smart", "Smart home", 290, 180, 220, 80 },
    };
    struct app *a = &g_app;
    int kind = -1;
    for (int i = 1; i < argc; i++)
        for (int k = 0; k < 4; k++)
            if (!strcmp(argv[i], def[k].name))
                kind = k;
    if (kind < 0) {
        fprintf(stderr, "usage: mellagui-gadget clipboard|mixer|system|smart\n");
        return 2;
    }
    a->kind = (enum kind)kind;
    a->cursor = -1;
    a->drag_part = LM_MIXER_NOTHING;
    int dw = def[kind].w, dh = def[kind].h;
    if (kind == G_CLIP) {
        dw = lm_metrics.gadget_width;
        dh = TITLE_H + 5 * ENTRY_H + PAD;
    } else if (kind == G_MIXER) {
        dw = LM_MAX(lm_metrics.mixer_width, def[kind].min_w);
        dh = lm_metrics.mixer_height;
    } else if (kind == G_SYS) {
        dw = lm_metrics.sys_width;
        dh = lm_metrics.sys_height;
    }
    signal(SIGCHLD, SIG_IGN);                                       /* fire-and-forget children (pactl, mellagui-smart --set) */
    signal(SIGPIPE, SIG_IGN);
    static const struct wlwin_ops ops = { .paint = paint, .global = global_event, .pointer = pointer_event, .keyboard = keyboard_event, .close = close_ask };
    if (wlwin_connect(&a->win, "mellagui-gadget", &ops, a) < 0)
        return 1;
    a->loop = lm_loop_create();
    if (kind == G_CLIP && !a->settings) {
        fprintf(stderr, "mellagui-gadget: the compositor offers no mellagui_settings_v1 (or not to this program)\n");
        wlwin_destroy(&a->win);
        return 1;
    }
    if (kind == G_MIXER) {
        a->mixer = lm_mixer_create(a->loop, changed, a);
        lm_mixer_set_active(a->mixer, true);
    } else if (kind == G_SYS) {
        a->sys = lm_sysinfo_create(a->loop, changed, a);
        lm_sysinfo_set_active(a->sys, true);
    } else if (kind == G_SMART) {
        a->smart = lm_smart_create(a->loop, changed, a);
        lm_smart_set_active(a->smart, true);
    }
    if (kind == G_CLIP && wlc_roundtrip(a->win.c) < 0) {            /* the value burst */
        wlwin_destroy(&a->win);
        return 1;
    }
    char app_id[64];
    snprintf(app_id, sizeof(app_id), "mellagui.gadget.%s", def[kind].name);
    if (wlwin_open(&a->win, def[kind].title, app_id, dw, dh, def[kind].min_w, def[kind].min_h) < 0)
        return 1;
    WLC_SEND(a->win.c, a->win.toplevel, LM_XDG_TOPLEVEL_REQ_SET_MIN_SIZE, { .i = def[kind].min_w }, { .i = def[kind].min_h });   /* the compositor holds the card to these */
    WLC_SEND(a->win.c, a->win.toplevel, LM_XDG_TOPLEVEL_REQ_SET_MAX_SIZE, { .i = 640 }, { .i = 1600 });
    signal(SIGUSR1, on_usr1);
    while (wlwin_step(&a->win, 50)) {
        lm_loop_dispatch(a->loop, 0);
        if (g_refresh) {
            g_refresh = 0;
            if (a->smart)
                lm_smart_refresh(a->smart);
        }
    }
    wlwin_destroy(&a->win);
    return 0;
}
