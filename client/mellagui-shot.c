/*
 * mellagui-shot - a screenshot of an area, with marks (Super+Shift+S).
 *
 *   mellagui-shot                 freeze the screen, drag a rectangle, mark it up, Enter saves it
 *   mellagui-shot --dir DIR       where the file goes (default $MELLAGUI_SHOT_DIR, else ~/Pictures)
 *   mellagui-shot --output N      the output to take (default: the one the pointer is on)
 *   mellagui-shot --grab FILE [--rect X,Y,W,H]    no window: write the output (or the part of it) to FILE as PNG
 *   mellagui-shot --selftest
 *
 * The picture comes from the compositor's private mellagui_capture_v1 (the same as the screen recorder's: only MellaGUI's own programs get
 * it, never while the screen is locked); it is taken BEFORE this program's full screen window opens, so the window is not in it. Every
 * output is grabbed once, the one the window lands on is shown.
 *
 *   Drag            choose the area (drag again inside it to move it, on a handle to resize; Select tool)
 *   1..6            Select, Pen, Arrow, Box, Mark (highlighter), Text (V P A B M T work too)
 *   Colours         the six squares in the tool bar; "Thin / Thick" the line width
 *   Ctrl+Z          undo the last mark          Delete  the last mark, too
 *   Enter or Save   the file in the folder (mode 0600, mellagui-YYYYMMDD-HHMMSS.png) and the clipboard (image/png)
 *   Ctrl+C or Copy  the clipboard only          Esc or the x  cancel (while typing a text: only the text)
 *   Enter with no area chosen takes the whole screen.
 *
 * The clipboard is the compositor's: this program writes the PNG and asks it to take the file over (settings `clip-png`), so the
 * picture stays pasteable after the program has ended. The annotation colours are literals on purpose (they are part of the picture).
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "font.h"
#include "keymap.h"
#include "png.h"
#include "protocol.h"
#include "theme.h"
#include "util.h"
#include "wlc.h"
#include "wlwin.h"

#define MAX_OUTPUTS 8
#define MAX_ANN 256
#define MAX_PEN 4096
#define HANDLE 6                        /* half the size of a resize handle */
#define BAR_H 34
#define TEXT_SCALE 2

/* ---- the picture and the marks ------------------------------------------------------------------------------------------------- */

struct canvas {
    uint32_t *px;
    int w, h;
};

enum tool { T_SELECT, T_PEN, T_ARROW, T_BOX, T_MARK, T_TEXT, T_COUNT };
static const char *const TOOL_NAMES[T_COUNT] = { "Select", "Pen", "Arrow", "Box", "Mark", "Text" };

static const uint32_t PALETTE[6] = { 0xe53935, 0xfdd835, 0x43a047, 0x1e88e5, 0xffffff, 0x111111 };

struct ann {
    enum tool tool;
    uint32_t color;
    int thick;
    int x0, y0, x1, y1;                 /* arrow: from-to, box / mark: the corners, text: where it starts (x0, y0) */
    int *pts;                           /* pen: x, y, x, y ... */
    int npts;                           /* number of points */
    char text[96];
    bool editing;                       /* a text that is still being typed */
};

static void put_px(struct canvas *c, int x, int y, uint32_t rgb, int alpha)
{
    if (x < 0 || y < 0 || x >= c->w || y >= c->h)
        return;
    uint32_t *p = &c->px[(size_t)y * (size_t)c->w + (size_t)x];
    if (alpha >= 255) {
        *p = 0xff000000u | rgb;
        return;
    }
    uint32_t o = *p;
    uint32_t r = (((rgb >> 16) & 255) * (uint32_t)alpha + ((o >> 16) & 255) * (uint32_t)(255 - alpha)) / 255;
    uint32_t g = (((rgb >> 8) & 255) * (uint32_t)alpha + ((o >> 8) & 255) * (uint32_t)(255 - alpha)) / 255;
    uint32_t b = ((rgb & 255) * (uint32_t)alpha + (o & 255) * (uint32_t)(255 - alpha)) / 255;
    *p = 0xff000000u | (r << 16) | (g << 8) | b;
}

static void disc(struct canvas *c, int cx, int cy, int r, uint32_t rgb)
{
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++)
            if (x * x + y * y <= r * r + r / 2)
                put_px(c, cx + x, cy + y, rgb, 255);
}

/* A line with round ends, `thick` pixels wide. */
static void line(struct canvas *c, int x0, int y0, int x1, int y1, int thick, uint32_t rgb)
{
    int r = LM_MAX(thick / 2, 0);
    int dx = abs(x1 - x0), dy = abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx - dy;
    for (;;) {
        if (r == 0)
            put_px(c, x0, y0, rgb, 255);
        else
            disc(c, x0, y0, r, rgb);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

static void arrow(struct canvas *c, int x0, int y0, int x1, int y1, int thick, uint32_t rgb)
{
    line(c, x0, y0, x1, y1, thick, rgb);
    double ang = atan2((double)(y1 - y0), (double)(x1 - x0)), len = 10.0 + thick * 3.0;
    for (int s = -1; s <= 1; s += 2) {
        double a = ang + M_PI - s * 0.45;
        line(c, x1, y1, x1 + (int)lround(cos(a) * len), y1 + (int)lround(sin(a) * len), thick, rgb);
    }
}

static void rect_outline(struct canvas *c, int x0, int y0, int x1, int y1, int thick, uint32_t rgb)
{
    int a = LM_MIN(x0, x1), b = LM_MIN(y0, y1), e = LM_MAX(x0, x1), f = LM_MAX(y0, y1);
    line(c, a, b, e, b, thick, rgb);
    line(c, e, b, e, f, thick, rgb);
    line(c, e, f, a, f, thick, rgb);
    line(c, a, f, a, b, thick, rgb);
}

static void glyph(struct canvas *c, int x, int y, uint32_t cp, int scale, uint32_t rgb)
{
    int cw = LM_FONT_W * scale, ch = LM_FONT_H * scale;
    const uint8_t *g = lm_font_cell(cp, cw, ch);
    for (int r = 0; r < ch; r++)
        for (int cx = 0; cx < cw; cx++)
            if (g[r * cw + cx])
                put_px(c, x + cx, y + r, rgb, g[r * cw + cx]);
}

static int text_width(const char *s, int scale)
{
    return lm_font_text_width(s) * scale;
}

static void text(struct canvas *c, int x, int y, const char *s, int scale, uint32_t rgb)
{
    for (int pass = 0; pass < 2; pass++) {                      /* a dark shadow first keeps white text readable on light pictures */
        uint32_t col = pass == 0 ? (rgb == 0x111111 ? 0xffffff : 0x101010) : rgb;
        int px = x + (pass == 0 ? 1 : 0), py = y + (pass == 0 ? 1 : 0);
        size_t len = strlen(s);
        for (size_t i = 0; i < len;) {
            int used;
            uint32_t cp = lm_utf8_decode(s + i, (int)(len - i), &used);
            i += used > 0 ? (size_t)used : 1;
            glyph(c, px, py, cp, scale, col);
            px += LM_FONT_W * scale;
        }
    }
}

static void ann_draw(struct canvas *c, const struct ann *a)
{
    switch (a->tool) {
    case T_PEN:
        for (int i = 0; i + 1 < a->npts; i++)
            line(c, a->pts[2 * i], a->pts[2 * i + 1], a->pts[2 * i + 2], a->pts[2 * i + 3], a->thick, a->color);
        if (a->npts == 1)
            disc(c, a->pts[0], a->pts[1], LM_MAX(a->thick / 2, 1), a->color);
        break;
    case T_ARROW:
        arrow(c, a->x0, a->y0, a->x1, a->y1, a->thick, a->color);
        break;
    case T_BOX:
        rect_outline(c, a->x0, a->y0, a->x1, a->y1, a->thick, a->color);
        break;
    case T_MARK:
        for (int y = LM_MIN(a->y0, a->y1); y <= LM_MAX(a->y0, a->y1); y++)
            for (int x = LM_MIN(a->x0, a->x1); x <= LM_MAX(a->x0, a->x1); x++)
                put_px(c, x, y, a->color, 100);
        break;
    case T_TEXT: {
        text(c, a->x0, a->y0, a->text, TEXT_SCALE, a->color);
        if (a->editing) {                                       /* the caret */
            int x = a->x0 + text_width(a->text, TEXT_SCALE);
            for (int y = 0; y < LM_FONT_H * TEXT_SCALE; y++)
                put_px(c, x, a->y0 + y, a->color, 255);
        }
        break;
    }
    default:
        break;
    }
}

/* ---- the program ------------------------------------------------------------------------------------------------------------------ */

struct out {
    char name[32];
    int x, y, w, h;
    uint32_t *map;                      /* the grabbed picture (XRGB8888), NULL until it is ready */
    uint32_t buf;
    bool done;
};

enum drag { D_NONE, D_NEW, D_MOVE, D_RESIZE, D_DRAW };

struct app {
    struct wlwin win;
    uint32_t capture, settings;
    struct out outs[MAX_OUTPUTS];
    int nouts, listed, pointer_out;
    int pending;                        /* grabs not answered yet */
    char fail[32];
    /* the chosen output */
    int cur;
    struct canvas shot;                 /* the picture as it was */
    struct canvas work;                 /* the picture with the marks */
    /* the area */
    bool have_sel, phase_edit;
    int sx0, sy0, sx1, sy1;             /* normalised: sx0 < sx1 */
    enum drag drag;
    int drag_handle, drag_ax, drag_ay;  /* resize: which handle; move: the offset of the grab */
    /* the marks */
    struct ann ann[MAX_ANN];
    int nann;
    enum tool tool;
    int color, thick;                   /* PALETTE index, 2 or 5 */
    double px, py;
    bool done, copy_only, saved_ok;
    char dir[512];
    char msg[640];
};

static void buffer_nothing(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)c; (void)id; (void)op; (void)args; (void)data;
}

static void capture_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)c; (void)id;
    struct app *a = data;
    switch (op) {
    case LM_MELLAGUI_CAPTURE_V1_EVT_OUTPUT: {
        int i = args[0].i;
        if (!a->listed && i >= 0 && i < MAX_OUTPUTS) {
            struct out *o = &a->outs[i];
            snprintf(o->name, sizeof(o->name), "%s", args[1].s ? args[1].s : "?");
            o->x = args[2].i; o->y = args[3].i; o->w = args[4].i; o->h = args[5].i;
            a->nouts = LM_MAX(a->nouts, i + 1);
        }
        break;
    }
    case LM_MELLAGUI_CAPTURE_V1_EVT_DONE:
        a->listed = true;
        break;
    case LM_MELLAGUI_CAPTURE_V1_EVT_READY:
        if (args[0].i >= 0 && args[0].i < a->nouts)
            a->outs[args[0].i].done = true;
        a->pending--;
        break;
    case LM_MELLAGUI_CAPTURE_V1_EVT_FAILED:
        if (args[0].i >= 0 && args[0].i < a->nouts)
            a->outs[args[0].i].map = NULL;
        snprintf(a->fail, sizeof(a->fail), "%s", args[1].s ? args[1].s : "?");
        a->pending--;
        break;
    }
}

static void settings_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)c; (void)id;
    struct app *a = data;
    if (op == LM_MELLAGUI_SETTINGS_V1_EVT_VALUE && !strcmp(args[0].s, "pointer-output"))
        a->pointer_out = atoi(args[1].s);
}

static void global_event(struct wlwin *w, uint32_t name, const char *iface, uint32_t ver)
{
    (void)ver;
    struct app *a = w->data;
    if (!strcmp(iface, "mellagui_capture_v1"))
        a->capture = wlc_bind(w->c, w->registry, name, &lm_mellagui_capture_v1_interface, 1, capture_event, a);
    else if (!strcmp(iface, "mellagui_settings_v1"))
        a->settings = wlc_bind(w->c, w->registry, name, &lm_mellagui_settings_v1_interface, 1, settings_event, a);
}

static void send_set(struct app *a, const char *key, const char *value)
{
    if (a->settings)
        WLC_SEND(a->win.c, a->settings, LM_MELLAGUI_SETTINGS_V1_REQ_SET, { .s = key }, { .s = value });
}

/* One shared buffer for every output, grabbed all at once. */
static bool grab_all(struct app *a)
{
    size_t total = 0, off[MAX_OUTPUTS];
    for (int i = 0; i < a->nouts; i++) {
        off[i] = total;
        total += (size_t)a->outs[i].w * (size_t)a->outs[i].h * 4;
    }
    if (!total)
        return false;
    int fd = memfd_create("mellagui-shot", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)total) < 0) {
        if (fd >= 0)
            close(fd);
        return false;
    }
    uint8_t *map = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        return false;
    }
    uint32_t pool = wlc_alloc(a->win.c, &lm_wl_shm_pool_interface, NULL, NULL);
    WLC_SEND(a->win.c, a->win.shm, LM_WL_SHM_REQ_CREATE_POOL, { .n = pool }, { .h = fd }, { .i = (int32_t)total });
    close(fd);
    for (int i = 0; i < a->nouts; i++) {
        struct out *o = &a->outs[i];
        o->map = (uint32_t *)(void *)(map + off[i]);
        o->buf = wlc_alloc(a->win.c, &lm_wl_buffer_interface, buffer_nothing, NULL);
        WLC_SEND(a->win.c, pool, LM_WL_SHM_POOL_REQ_CREATE_BUFFER, { .n = o->buf }, { .i = (int32_t)off[i] }, { .i = o->w }, { .i = o->h },
                 { .i = o->w * 4 }, { .u = LM_WL_SHM_FORMAT_XRGB8888 });
    }
    WLC_SEND0(a->win.c, pool, LM_WL_SHM_POOL_REQ_DESTROY);
    wlc_forget(a->win.c, pool);
    a->pending = a->nouts;
    for (int i = 0; i < a->nouts; i++)
        WLC_SEND(a->win.c, a->capture, LM_MELLAGUI_CAPTURE_V1_REQ_GRAB, { .i = i }, { .o = a->outs[i].buf });
    for (int tries = 0; a->pending > 0 && tries < 200; tries++)         /* up to ~10 s */
        if (wlc_dispatch(a->win.c, 50) < 0)
            return false;
    return a->pending <= 0;
}

/* ---- area and marks ------------------------------------------------------------------------------------------------------------ */

static void sel_norm(struct app *a, int x0, int y0, int x1, int y1)
{
    a->sx0 = LM_CLAMP(LM_MIN(x0, x1), 0, a->shot.w);
    a->sx1 = LM_CLAMP(LM_MAX(x0, x1), 0, a->shot.w);
    a->sy0 = LM_CLAMP(LM_MIN(y0, y1), 0, a->shot.h);
    a->sy1 = LM_CLAMP(LM_MAX(y0, y1), 0, a->shot.h);
}

static void rerender(struct app *a)
{
    memcpy(a->work.px, a->shot.px, (size_t)a->shot.w * (size_t)a->shot.h * 4);
    for (int i = 0; i < a->nann; i++)
        ann_draw(&a->work, &a->ann[i]);
}

static void ann_free(struct ann *n)
{
    free(n->pts);
    n->pts = NULL;
}

static void undo(struct app *a)
{
    if (a->nann > 0) {
        ann_free(&a->ann[--a->nann]);
        rerender(a);
    }
}

static struct ann *editing_text(struct app *a)
{
    return a->nann > 0 && a->ann[a->nann - 1].editing ? &a->ann[a->nann - 1] : NULL;
}

static void text_commit(struct app *a)
{
    struct ann *t = editing_text(a);
    if (!t)
        return;
    t->editing = false;
    if (!t->text[0])
        a->nann--;                                          /* nothing typed: nothing kept */
    rerender(a);
}

static void text_cancel(struct app *a)
{
    if (editing_text(a)) {
        a->nann--;
        rerender(a);
    }
}

/* The cropped picture (marks in) as XRGB8888, malloc'd; the whole output when no area is chosen. */
static uint32_t *crop(const struct app *a, int *w, int *h)
{
    int x0 = a->have_sel ? a->sx0 : 0, y0 = a->have_sel ? a->sy0 : 0, x1 = a->have_sel ? a->sx1 : a->shot.w, y1 = a->have_sel ? a->sy1 : a->shot.h;
    *w = x1 - x0;
    *h = y1 - y0;
    if (*w < 1 || *h < 1)
        return NULL;
    uint32_t *out = malloc((size_t)*w * (size_t)*h * 4);
    if (!out)
        return NULL;
    for (int y = 0; y < *h; y++)
        memcpy(out + (size_t)y * (size_t)*w, a->work.px + (size_t)(y0 + y) * (size_t)a->work.w + (size_t)x0, (size_t)*w * 4);
    return out;
}

/* ---- saving ------------------------------------------------------------------------------------------------------------------- */

static bool write_all(int fd, const uint8_t *p, size_t len)
{
    while (len > 0) {
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

/* dir/mellagui-YYYYMMDD-HHMMSS[-N].png, created with O_EXCL and mode 0600 (a screen can show secrets). */
static bool save_png(const char *dir, const uint8_t *png, size_t len, char *path, size_t pathsz)
{
    if (mkdir(dir, 0755) < 0 && errno != EEXIST)
        return false;
    time_t now = time(NULL);
    struct tm tm;
    char stamp[32];
    localtime_r(&now, &tm);
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
    int fd = -1;
    for (int n = 0; n < 100 && fd < 0; n++) {
        if (n == 0)
            snprintf(path, pathsz, "%s/mellagui-%s.png", dir, stamp);
        else
            snprintf(path, pathsz, "%s/mellagui-%s-%d.png", dir, stamp, n);
        fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0 && errno != EEXIST)
            return false;
    }
    if (fd < 0)
        return false;
    bool ok = write_all(fd, png, len);
    if (close(fd) != 0)
        ok = false;
    if (!ok)
        unlink(path);
    return ok;
}

static void notify(struct app *a, const char *title, const char *body)
{
    char msg[400];
    snprintf(msg, sizeof(msg), "%s\t%s\t6000", title, body);
    send_set(a, "notify", msg);
}

/* Enter: file + clipboard; Ctrl+C: the clipboard only (through a file that is only there until the compositor has read it). */
static bool finish(struct app *a, bool copy_only)
{
    int w, h;
    uint32_t *px = crop(a, &w, &h);
    if (!px) {
        snprintf(a->msg, sizeof(a->msg), "nothing to save");
        return false;
    }
    size_t len = 0;
    uint8_t *png = lm_png_encode_xrgb(px, w, h, (size_t)w * 4, &len);
    free(px);
    if (!png) {
        snprintf(a->msg, sizeof(a->msg), "PNG encoding failed");
        return false;
    }
    char path[640] = "", tmp[640] = "";
    bool ok = true;
    if (!copy_only) {
        ok = save_png(a->dir, png, len, path, sizeof(path));
        if (!ok)
            snprintf(a->msg, sizeof(a->msg), "cannot write into %s: %s", a->dir, strerror(errno));
    }
    const char *clip_path = path;
    if (copy_only) {
        const char *rt = getenv("XDG_RUNTIME_DIR");
        snprintf(tmp, sizeof(tmp), "%s/mellagui-shot-%d.png", rt && *rt ? rt : "/tmp", (int)getpid());
        int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        ok = fd >= 0 && write_all(fd, png, len);
        if (fd >= 0)
            close(fd);
        clip_path = tmp;
        if (!ok)
            snprintf(a->msg, sizeof(a->msg), "cannot write the clipboard file");
    }
    free(png);
    if (!ok)
        return false;
    send_set(a, "clip-png", clip_path);
    if (copy_only) {
        wlc_roundtrip(a->win.c);                                /* the compositor has read the file when this returns */
        unlink(tmp);
        char body[64];
        snprintf(body, sizeof(body), "%d x %d pixels", w, h);
        notify(a, "Screenshot copied", body);
    } else {
        const char *slash = strrchr(path, '/');
        notify(a, "Screenshot saved", slash ? slash + 1 : path);
        printf("%s\n", path);
    }
    wlc_flush(a->win.c);
    a->saved_ok = true;
    return true;
}

/* ---- the tool bar ----------------------------------------------------------------------------------------------------------------- */

enum { B_TOOL, B_COLOR, B_THICK, B_UNDO, B_COPY, B_SAVE, B_CLOSE };

struct button {
    int kind, arg;
    int x, y, w, h;
    char label[16];
    bool on;
};

#define MAX_BUTTONS 24

static int bar_layout(const struct app *a, struct button *b)
{
    int n = 0, x = 0;
    for (int t = 0; t < T_COUNT; t++, n++) {
        b[n] = (struct button){ .kind = B_TOOL, .arg = t, .x = x, .on = a->tool == (enum tool)t };
        snprintf(b[n].label, sizeof(b[n].label), "%s", TOOL_NAMES[t]);
        b[n].w = text_width(b[n].label, 1) + 14;
        x += b[n].w + 3;
    }
    x += 6;
    for (int i = 0; i < 6; i++, n++) {
        b[n] = (struct button){ .kind = B_COLOR, .arg = i, .x = x, .w = 22, .on = a->color == i };
        x += 25;
    }
    x += 6;
    b[n] = (struct button){ .kind = B_THICK, .x = x, .on = false };
    snprintf(b[n].label, sizeof(b[n].label), "%s", a->thick > 3 ? "Thick" : "Thin");
    b[n].w = text_width("Thick", 1) + 14;
    x += b[n++].w + 9;
    b[n] = (struct button){ .kind = B_UNDO, .x = x };
    snprintf(b[n].label, sizeof(b[n].label), "Undo");
    b[n].w = text_width(b[n].label, 1) + 14;
    x += b[n++].w + 9;
    b[n] = (struct button){ .kind = B_COPY, .x = x };
    snprintf(b[n].label, sizeof(b[n].label), "Copy");
    b[n].w = text_width(b[n].label, 1) + 14;
    x += b[n++].w + 3;
    b[n] = (struct button){ .kind = B_SAVE, .x = x, .on = true };
    snprintf(b[n].label, sizeof(b[n].label), "Save");
    b[n].w = text_width(b[n].label, 1) + 14;
    x += b[n++].w + 3;
    b[n] = (struct button){ .kind = B_CLOSE, .x = x };
    snprintf(b[n].label, sizeof(b[n].label), "x");
    b[n].w = 26;
    x += b[n++].w;
    int total = x, bx, by;
    int sw = a->shot.w, sh = a->shot.h;
    bx = LM_CLAMP(a->sx1 - total, 4, LM_MAX(sw - total - 4, 4));                    /* right-aligned with the area, inside the screen */
    if (a->sy1 + BAR_H + 8 <= sh)
        by = a->sy1 + 6;                                                            /* below it ... */
    else if (a->sy0 - BAR_H - 8 >= 0)
        by = a->sy0 - BAR_H - 6;                                                    /* ... else above it ... */
    else
        by = LM_MAX(a->sy1 - BAR_H - 6, 4);                                         /* ... else inside */
    for (int i = 0; i < n; i++) {
        b[i].x += bx + 4;
        b[i].y = by + 4;
        b[i].h = BAR_H - 8;
    }
    return n;
}

static void bar_rect(const struct app *a, int *x, int *y, int *w, int *h)
{
    struct button b[MAX_BUTTONS];
    int n = bar_layout(a, b);
    *x = b[0].x - 4;
    *y = b[0].y - 4;
    *w = b[n - 1].x + b[n - 1].w - b[0].x + 8;
    *h = BAR_H;
}

static bool in_box(double px, double py, int x, int y, int w, int h)
{
    return px >= x && px < x + w && py >= y && py < y + h;
}

/* ---- painting --------------------------------------------------------------------------------------------------------------------- */

static void handle_pos(const struct app *a, int hnd, int *x, int *y)
{
    int mx = (a->sx0 + a->sx1) / 2, my = (a->sy0 + a->sy1) / 2;
    int xs[3] = { a->sx0, mx, a->sx1 }, ys[3] = { a->sy0, my, a->sy1 };
    static const int cx[8] = { 0, 1, 2, 2, 2, 1, 0, 0 }, cy[8] = { 0, 0, 0, 1, 2, 2, 2, 1 };
    *x = xs[cx[hnd]];
    *y = ys[cy[hnd]];
}

static int handle_at(const struct app *a, double px, double py)
{
    for (int i = 0; i < 8; i++) {
        int x, y;
        handle_pos(a, i, &x, &y);
        if (fabs(px - x) <= HANDLE + 2 && fabs(py - y) <= HANDLE + 2)
            return i;
    }
    return -1;
}

static void paint(struct wlwin *w)
{
    struct app *a = w->data;
    int sw = a->shot.w, sh = a->shot.h;
    for (int y = 0; y < w->bh; y++) {
        uint32_t *dst = w->pixels + (size_t)y * (size_t)w->bw;
        if (y >= sh) {
            for (int x = 0; x < w->bw; x++)
                dst[x] = 0xff000000u;
            continue;
        }
        const uint32_t *src = a->work.px + (size_t)y * (size_t)sw;
        bool row_in = a->have_sel && y >= a->sy0 && y < a->sy1;
        for (int x = 0; x < w->bw; x++) {
            if (x >= sw) {
                dst[x] = 0xff000000u;
                continue;
            }
            uint32_t p = src[x];
            if (!(row_in && x >= a->sx0 && x < a->sx1))
                p = 0xff000000u | ((((p >> 16) & 255) * 110 / 255) << 16) | ((((p >> 8) & 255) * 110 / 255) << 8) | ((p & 255) * 110 / 255);
            dst[x] = p;
        }
    }
    if (!a->have_sel) {
        const char *hint = "Drag to choose an area   Enter: the whole screen   Esc: cancel";
        int tw = text_width(hint, 1) + 24, tx = (w->bw - tw) / 2;
        wlwin_fill(w, tx, 40, tw, 30, lm_theme.background);
        wlwin_text(w, tx + 12, 47, hint, 1, lm_theme.text, 0);
        return;
    }
    /* the border and, with the Select tool, the handles */
    wlwin_fill(w, a->sx0 - 1, a->sy0 - 1, a->sx1 - a->sx0 + 2, 1, lm_theme.accent);
    wlwin_fill(w, a->sx0 - 1, a->sy1, a->sx1 - a->sx0 + 2, 1, lm_theme.accent);
    wlwin_fill(w, a->sx0 - 1, a->sy0, 1, a->sy1 - a->sy0, lm_theme.accent);
    wlwin_fill(w, a->sx1, a->sy0, 1, a->sy1 - a->sy0, lm_theme.accent);
    if (a->phase_edit && a->tool == T_SELECT)
        for (int i = 0; i < 8; i++) {
            int x, y;
            handle_pos(a, i, &x, &y);
            wlwin_fill(w, x - HANDLE, y - HANDLE, 2 * HANDLE, 2 * HANDLE, lm_theme.accent);
            wlwin_fill(w, x - HANDLE + 2, y - HANDLE + 2, 2 * HANDLE - 4, 2 * HANDLE - 4, lm_theme.text);
        }
    char size[32];
    snprintf(size, sizeof(size), "%d x %d", a->sx1 - a->sx0, a->sy1 - a->sy0);
    int ly = a->sy0 >= 24 ? a->sy0 - 22 : a->sy0 + 4;
    wlwin_fill(w, a->sx0, ly, text_width(size, 1) + 12, 20, lm_theme.background);
    wlwin_text(w, a->sx0 + 6, ly + 2, size, 1, lm_theme.text, 0);
    if (!a->phase_edit)
        return;
    struct button b[MAX_BUTTONS];
    int n = bar_layout(a, b), bx, by, bw, bh;
    bar_rect(a, &bx, &by, &bw, &bh);
    wlwin_fill(w, bx, by, bw, bh, lm_theme.background);
    wlwin_fill(w, bx, by, bw, 1, lm_theme.border);
    wlwin_fill(w, bx, by + bh - 1, bw, 1, lm_theme.border);
    for (int i = 0; i < n; i++) {
        bool hot = in_box(a->px, a->py, b[i].x, b[i].y, b[i].w, b[i].h);
        if (b[i].kind == B_COLOR) {
            wlwin_fill(w, b[i].x, b[i].y, b[i].w, b[i].h, b[i].on ? lm_theme.text : lm_theme.border);
            wlwin_fill(w, b[i].x + 2, b[i].y + 2, b[i].w - 4, b[i].h - 4, PALETTE[b[i].arg]);
            continue;
        }
        uint32_t bg = b[i].kind == B_SAVE ? lm_theme.ok : b[i].on ? lm_theme.button_down : hot ? lm_theme.button_hover : lm_theme.button;
        wlwin_fill(w, b[i].x, b[i].y, b[i].w, b[i].h, bg);
        wlwin_text(w, b[i].x + (b[i].w - text_width(b[i].label, 1)) / 2, b[i].y + (b[i].h - LM_FONT_H) / 2, b[i].label, 1,
                   b[i].kind == B_SAVE ? lm_theme.ink : lm_theme.text, 0);
    }
}

/* ---- input ------------------------------------------------------------------------------------------------------------------------ */

static void begin_ann(struct app *a, int x, int y)
{
    if (a->nann >= MAX_ANN)
        return;
    struct ann *n = &a->ann[a->nann++];
    memset(n, 0, sizeof(*n));
    n->tool = a->tool;
    n->color = PALETTE[a->color];
    n->thick = a->thick;
    n->x0 = n->x1 = x;
    n->y0 = n->y1 = y;
    if (a->tool == T_PEN) {
        n->pts = malloc(sizeof(int) * 2 * MAX_PEN);
        if (!n->pts) {
            a->nann--;
            return;
        }
        n->pts[0] = x;
        n->pts[1] = y;
        n->npts = 1;
    }
    if (a->tool == T_TEXT)
        n->editing = true;
}

static void pointer_event(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
    struct app *a = w->data;
    if (op == LM_WL_POINTER_EVT_ENTER || op == LM_WL_POINTER_EVT_MOTION) {
        int base = op == LM_WL_POINTER_EVT_ENTER ? 2 : 1;
        a->px = lm_fixed_to_double(args[base].f);
        a->py = lm_fixed_to_double(args[base + 1].f);
        int x = (int)a->px, y = (int)a->py;
        if (a->drag == D_NEW) {
            sel_norm(a, a->drag_ax, a->drag_ay, x, y);
        } else if (a->drag == D_MOVE) {
            int wd = a->sx1 - a->sx0, ht = a->sy1 - a->sy0;
            int nx = LM_CLAMP(x - a->drag_ax, 0, a->shot.w - wd), ny = LM_CLAMP(y - a->drag_ay, 0, a->shot.h - ht);
            a->sx0 = nx; a->sx1 = nx + wd; a->sy0 = ny; a->sy1 = ny + ht;
        } else if (a->drag == D_RESIZE) {
            int x0 = a->sx0, y0 = a->sy0, x1 = a->sx1, y1 = a->sy1;
            static const int ex[8][2] = { { 1, 0 }, { 0, 0 }, { 0, 1 }, { 0, 1 }, { 0, 1 }, { 0, 0 }, { 1, 0 }, { 1, 0 } };      /* which edges the handle moves: x0 / x1 */
            static const int ey[8][2] = { { 1, 0 }, { 1, 0 }, { 1, 0 }, { 0, 0 }, { 0, 1 }, { 0, 1 }, { 0, 1 }, { 0, 0 } };
            if (ex[a->drag_handle][0]) x0 = x;
            if (ex[a->drag_handle][1]) x1 = x;
            if (ey[a->drag_handle][0]) y0 = y;
            if (ey[a->drag_handle][1]) y1 = y;
            sel_norm(a, x0, y0, x1, y1);
        } else if (a->drag == D_DRAW && a->nann > 0) {
            struct ann *n = &a->ann[a->nann - 1];
            int cx = LM_CLAMP(x, a->sx0, a->sx1 - 1), cy = LM_CLAMP(y, a->sy0, a->sy1 - 1);
            if (n->tool == T_PEN) {
                if (n->npts < MAX_PEN && (n->pts[2 * (n->npts - 1)] != cx || n->pts[2 * (n->npts - 1) + 1] != cy)) {
                    n->pts[2 * n->npts] = cx;
                    n->pts[2 * n->npts + 1] = cy;
                    n->npts++;
                }
            } else {
                n->x1 = cx;
                n->y1 = cy;
            }
            rerender(a);
        }
        w->redraw = true;
        return;
    }
    if (op != LM_WL_POINTER_EVT_BUTTON || args[2].u != 272)
        return;
    int x = (int)a->px, y = (int)a->py;
    if (args[3].u == 0) {                                       /* released */
        if (a->drag == D_NEW) {
            if (a->sx1 - a->sx0 >= 4 && a->sy1 - a->sy0 >= 4) {
                a->have_sel = a->phase_edit = true;
            } else {
                a->have_sel = false;                            /* a click without a drag */
            }
        } else if (a->drag == D_DRAW && a->nann > 0) {
            struct ann *n = &a->ann[a->nann - 1];
            if (n->tool != T_TEXT && n->tool != T_PEN && abs(n->x1 - n->x0) < 2 && abs(n->y1 - n->y0) < 2)
                a->nann--;                                      /* a click, not a mark */
            rerender(a);
        }
        if (a->drag != D_DRAW || !editing_text(a))
            a->drag = D_NONE;
        else
            a->drag = D_NONE;
        w->redraw = true;
        return;
    }
    /* pressed */
    if (a->phase_edit) {
        struct button b[MAX_BUTTONS];
        int n = bar_layout(a, b);
        for (int i = 0; i < n; i++) {
            if (!in_box(x, y, b[i].x, b[i].y, b[i].w, b[i].h))
                continue;
            text_commit(a);
            switch (b[i].kind) {
            case B_TOOL: a->tool = (enum tool)b[i].arg; break;
            case B_COLOR: a->color = b[i].arg; break;
            case B_THICK: a->thick = a->thick > 3 ? 2 : 5; break;
            case B_UNDO: undo(a); break;
            case B_COPY: a->copy_only = true; a->done = true; break;
            case B_SAVE: a->done = true; break;
            case B_CLOSE: a->saved_ok = false; w->running = false; break;
            }
            w->redraw = true;
            return;
        }
        int bx, by, bw, bh;
        bar_rect(a, &bx, &by, &bw, &bh);
        if (in_box(x, y, bx, by, bw, bh))
            return;
        text_commit(a);
        bool inside = x >= a->sx0 && x < a->sx1 && y >= a->sy0 && y < a->sy1;
        if (a->tool == T_SELECT) {
            int hnd = handle_at(a, x, y);
            if (hnd >= 0) {
                a->drag = D_RESIZE;
                a->drag_handle = hnd;
            } else if (inside) {
                a->drag = D_MOVE;
                a->drag_ax = x - a->sx0;
                a->drag_ay = y - a->sy0;
            } else {                                            /* outside: a new area */
                a->drag = D_NEW;
                a->drag_ax = x;
                a->drag_ay = y;
                a->have_sel = a->phase_edit = false;
                sel_norm(a, x, y, x, y);
            }
        } else if (inside) {
            begin_ann(a, x, y);
            a->drag = D_DRAW;
            if (a->tool == T_TEXT)
                a->drag = D_NONE;                               /* the text is typed, not dragged */
            rerender(a);
        }
        w->redraw = true;
        return;
    }
    a->drag = D_NEW;
    a->drag_ax = x;
    a->drag_ay = y;
    a->have_sel = false;
    sel_norm(a, x, y, x, y);
    w->redraw = true;
}

static void text_input(struct app *a, uint32_t ks)
{
    struct ann *t = editing_text(a);
    if (!t)
        return;
    size_t len = strlen(t->text);
    if (ks == LM_KS_BackSpace) {
        while (len > 0 && ((unsigned char)t->text[len - 1] & 0xc0) == 0x80)
            len--;
        if (len > 0)
            t->text[len - 1] = 0;
    } else if (ks >= 0x20 && ks != 0x7f && len + 4 < sizeof(t->text)) {
        char buf[5];
        if (ks < 0x80) {
            buf[0] = (char)ks; buf[1] = 0;
        } else if (ks < 0x800) {
            buf[0] = (char)(0xc0 | (ks >> 6)); buf[1] = (char)(0x80 | (ks & 63)); buf[2] = 0;
        } else if (ks < 0x10000) {
            buf[0] = (char)(0xe0 | (ks >> 12)); buf[1] = (char)(0x80 | ((ks >> 6) & 63)); buf[2] = (char)(0x80 | (ks & 63)); buf[3] = 0;
        } else {
            return;
        }
        if (len + strlen(buf) < sizeof(t->text))
            strcat(t->text, buf);
    }
    rerender(a);
}

static void keyboard_event(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
    struct app *a = w->data;
    if (op != LM_WL_KEYBOARD_EVT_KEY || args[3].u == 0)
        return;
    uint32_t ks = lm_keymap_keysym(args[2].u, w->mods);
    bool ctrl = (w->mods & LM_MOD_CTRL) != 0;
    if (editing_text(a)) {                                      /* typing a text: letters are letters */
        if (ks == LM_KS_Escape)
            text_cancel(a);
        else if (ks == LM_KS_Return || ks == LM_KS_KP_Enter)
            text_commit(a);
        else if (!ctrl)
            text_input(a, ks);
        w->redraw = true;
        return;
    }
    if (ks == LM_KS_Escape) {
        a->saved_ok = false;
        w->running = false;
        return;
    }
    if (ks == LM_KS_Return || ks == LM_KS_KP_Enter) {
        a->done = true;
    } else if (ctrl && (ks == 'c' || ks == 'C')) {
        a->copy_only = true;
        a->done = true;
    } else if (ctrl && (ks == 'z' || ks == 'Z')) {
        undo(a);
    } else if (ks == LM_KS_Delete || ks == LM_KS_BackSpace) {
        undo(a);
    } else if (a->phase_edit && ks >= '1' && ks <= '6') {
        a->tool = (enum tool)(ks - '1');
    } else if (a->phase_edit && !ctrl) {
        static const struct { uint32_t k; enum tool t; } keys[] = { { 'v', T_SELECT }, { 'p', T_PEN }, { 'a', T_ARROW }, { 'b', T_BOX }, { 'm', T_MARK }, { 't', T_TEXT } };
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
            if (ks == keys[i].k || ks == keys[i].k - 32)
                a->tool = keys[i].t;
    }
    w->redraw = true;
}

/* ---- start and end ----------------------------------------------------------------------------------------------------------------- */

static void default_dir(char *out, size_t n)
{
    const char *d = getenv("MELLAGUI_SHOT_DIR"), *home = getenv("HOME");
    if (d && *d)
        snprintf(out, n, "%s", d);
    else if (home && *home)
        snprintf(out, n, "%s/Pictures", home);
    else
        snprintf(out, n, ".");
}

static bool parse_rect(const char *s, int *r)
{
    char junk;
    return sscanf(s, "%d,%d,%d,%d%c", &r[0], &r[1], &r[2], &r[3], &junk) == 4 && r[2] > 0 && r[3] > 0;
}

static int selftest(void)
{
    int bad = 0;
#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "mellagui-shot selftest: " __VA_ARGS__); fputc('\n', stderr); bad++; } } while (0)
    struct canvas c = { calloc(40 * 30, 4), 40, 30 };
    line(&c, 2, 2, 30, 2, 1, 0xff0000);
    CHECK(c.px[2 * 40 + 2] == 0xffff0000u && c.px[2 * 40 + 30] == 0xffff0000u && c.px[3 * 40 + 10] == 0, "a thin line");
    struct ann m = { .tool = T_MARK, .color = 0xffff00, .x0 = 5, .y0 = 5, .x1 = 9, .y1 = 9 };
    c.px[7 * 40 + 7] = 0xff000000u;
    ann_draw(&c, &m);
    CHECK(((c.px[7 * 40 + 7] >> 16) & 255) > 60 && ((c.px[7 * 40 + 7] >> 16) & 255) < 130 && (c.px[7 * 40 + 7] & 255) == 0, "the mark blends");
    struct ann t = { .tool = T_TEXT, .color = 0xffffff, .x0 = 1, .y0 = 12 };
    snprintf(t.text, sizeof(t.text), "A");
    ann_draw(&c, &t);
    int white = 0;
    for (int y = 12; y < 28; y++)
        for (int x = 1; x < 20; x++)
            white += c.px[y * 40 + x] == 0xffffffffu;
    CHECK(white > 20, "text draws pixels (%d)", white);
    int r[4];
    CHECK(parse_rect("10,20,300,200", r) && r[0] == 10 && r[3] == 200 && !parse_rect("10,20,0,5", r) && !parse_rect("a,b", r), "rect parsing");
    free(c.px);
    struct app *a = calloc(1, sizeof(*a));
    a->shot.w = 100;
    a->shot.h = 80;
    sel_norm(a, 60, 50, 10, 70);
    CHECK(a->sx0 == 10 && a->sx1 == 60 && a->sy0 == 50 && a->sy1 == 70, "sel_norm");
    sel_norm(a, -5, -5, 500, 500);
    CHECK(a->sx0 == 0 && a->sx1 == 100 && a->sy1 == 80, "sel_norm clamps");
    free(a);
    if (!bad)
        printf("mellagui-shot selftest ok\n");
    return bad;
}

int main(int argc, char **argv)
{
    lm_theme_load_env();
    static struct app a;
    a.pointer_out = -1;
    a.color = 0;
    a.thick = 3;
    default_dir(a.dir, sizeof(a.dir));
    const char *grab_file = NULL;
    int rect[4] = { 0, 0, 0, 0 };
    bool have_rect = false;
    int want_out = -1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--selftest"))
            return selftest();
        if (!strcmp(argv[i], "--dir") && i + 1 < argc)
            snprintf(a.dir, sizeof(a.dir), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--output") && i + 1 < argc)
            want_out = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--grab") && i + 1 < argc)
            grab_file = argv[++i];
        else if (!strcmp(argv[i], "--rect") && i + 1 < argc) {
            if (!parse_rect(argv[++i], rect)) {
                fprintf(stderr, "mellagui-shot: --rect wants X,Y,W,H\n");
                return 2;
            }
            have_rect = true;
        } else {
            fprintf(stderr, "usage: mellagui-shot [--dir DIR] [--output N] | --grab FILE [--rect X,Y,W,H] | --selftest\n");
            return 2;
        }
    }
    static const struct wlwin_ops ops = { .paint = paint, .global = global_event, .pointer = pointer_event, .keyboard = keyboard_event };
    if (wlwin_connect(&a.win, "mellagui-shot", &ops, &a) < 0)
        return 1;
    if (!a.capture) {
        fprintf(stderr, "mellagui-shot: the compositor offers no mellagui_capture_v1 (or not to this program)\n");
        wlwin_destroy(&a.win);
        return 1;
    }
    WLC_SEND0(a.win.c, a.capture, LM_MELLAGUI_CAPTURE_V1_REQ_LIST);
    if (wlc_roundtrip(a.win.c) < 0 || wlc_roundtrip(a.win.c) < 0 || !a.nouts) {
        fprintf(stderr, "mellagui-shot: no output to take\n");
        wlwin_destroy(&a.win);
        return 1;
    }
    if (!grab_all(&a)) {
        fprintf(stderr, "mellagui-shot: the picture could not be taken%s%s\n", a.fail[0] ? ": " : "", a.fail);
        wlwin_destroy(&a.win);
        return 1;
    }
    a.cur = want_out >= 0 && want_out < a.nouts ? want_out : a.pointer_out >= 0 && a.pointer_out < a.nouts ? a.pointer_out : 0;
    struct out *o = &a.outs[a.cur];
    if (!o->map) {
        fprintf(stderr, "mellagui-shot: the picture could not be taken\n");
        wlwin_destroy(&a.win);
        return 1;
    }
    a.shot = (struct canvas){ o->map, o->w, o->h };
    a.work = (struct canvas){ malloc((size_t)o->w * (size_t)o->h * 4), o->w, o->h };
    if (!a.work.px) {
        wlwin_destroy(&a.win);
        return 1;
    }
    memcpy(a.work.px, a.shot.px, (size_t)o->w * (size_t)o->h * 4);
    int rc = 0;
    if (grab_file) {
        if (have_rect) {
            a.have_sel = true;
            sel_norm(&a, rect[0], rect[1], rect[0] + rect[2], rect[1] + rect[3]);
        }
        int w, h;
        uint32_t *px = crop(&a, &w, &h);
        size_t len = 0;
        uint8_t *png = px ? lm_png_encode_xrgb(px, w, h, (size_t)w * 4, &len) : NULL;
        FILE *f = png ? fopen(grab_file, "wb") : NULL;
        if (!f || fwrite(png, 1, len, f) != len) {
            fprintf(stderr, "mellagui-shot: cannot write %s\n", grab_file);
            rc = 1;
        }
        if (f)
            fclose(f);
        free(png);
        free(px);
        free(a.work.px);
        wlwin_destroy(&a.win);
        return rc;
    }
    a.win.want_fullscreen = true;
    if (wlwin_open(&a.win, "Screenshot", "mellagui.shot", o->w, o->h, 200, 120) < 0) {
        wlwin_destroy(&a.win);
        return 1;
    }
    while (wlwin_step(&a.win, -1)) {
        if (a.done) {
            a.done = false;
            text_commit(&a);
            if (finish(&a, a.copy_only))
                break;
            a.copy_only = false;
            fprintf(stderr, "mellagui-shot: %s\n", a.msg);
            a.win.redraw = true;
        }
    }
    for (int i = 0; i < a.nann; i++)
        ann_free(&a.ann[i]);
    free(a.work.px);
    wlwin_destroy(&a.win);
    return rc;
}
