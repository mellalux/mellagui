/*
 * lumen-run: "Run command" dialog (wl_shm + xdg-shell, bitmap font).
 * Type a shell command and press Enter: it is started detached (own session, stdio on /dev/null)
 * and the dialog closes. Esc closes without running.
 *
 *   Left/Right/Home/End, Backspace/Delete  edit        Ctrl+U  clear the line   Ctrl+A / Ctrl+E  home / end
 *   Up/Down                                 history (kept in $XDG_CACHE_HOME/lumen-run-history or ~/.cache/...)
 *
 * `lumen-run --selftest` checks the line editor without a display.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "font.h"
#include "keymap.h"
#include "util.h"
#include "wlc.h"

/* ---- line editor ----------------------------------------------------------------------- */

#define LINE_MAX_BYTES 1000
#define HIST_MAX 50

struct line {
    char text[LINE_MAX_BYTES + 1];
    size_t len, cur;            /* bytes; cur is on a UTF-8 boundary */
};

static bool is_cont(unsigned char c)
{
    return (c & 0xc0) == 0x80;
}

static size_t prev_boundary(const struct line *l, size_t i)
{
    if (i == 0)
        return 0;
    i--;
    while (i > 0 && is_cont((unsigned char)l->text[i]))
        i--;
    return i;
}

static size_t next_boundary(const struct line *l, size_t i)
{
    if (i >= l->len)
        return l->len;
    i++;
    while (i < l->len && is_cont((unsigned char)l->text[i]))
        i++;
    return i;
}

static void line_set(struct line *l, const char *s)
{
    snprintf(l->text, sizeof(l->text), "%s", s);
    l->len = strlen(l->text);
    l->cur = l->len;
}

static void line_insert(struct line *l, const char *s, size_t n)
{
    if (l->len + n > LINE_MAX_BYTES)
        return;
    memmove(l->text + l->cur + n, l->text + l->cur, l->len - l->cur + 1);
    memcpy(l->text + l->cur, s, n);
    l->len += n;
    l->cur += n;
}

static void line_backspace(struct line *l)
{
    size_t p = prev_boundary(l, l->cur);
    memmove(l->text + p, l->text + l->cur, l->len - l->cur + 1);
    l->len -= l->cur - p;
    l->cur = p;
}

static void line_delete(struct line *l)
{
    size_t n = next_boundary(l, l->cur);
    memmove(l->text + l->cur, l->text + n, l->len - n + 1);
    l->len -= n - l->cur;
}

/* ---- history ---------------------------------------------------------------------------- */

struct hist {
    char *items[HIST_MAX];      /* oldest first */
    int n;
    int pos;                    /* browsing index, n = the line being edited */
    char draft[LINE_MAX_BYTES + 1];
};

static void hist_add(struct hist *h, const char *s)
{
    if (!*s || (h->n && !strcmp(h->items[h->n - 1], s)))
        return;
    if (h->n == HIST_MAX) {
        free(h->items[0]);
        memmove(h->items, h->items + 1, sizeof(h->items[0]) * (HIST_MAX - 1));
        h->n--;
    }
    h->items[h->n++] = strdup(s);
}

static char *hist_path(void)
{
    const char *x = getenv("XDG_CACHE_HOME"), *home = getenv("HOME");
    static char path[1024];
    if (x && *x)
        snprintf(path, sizeof(path), "%s/lumen-run-history", x);
    else if (home && *home)
        snprintf(path, sizeof(path), "%s/.cache/lumen-run-history", home);
    else
        return NULL;
    return path;
}

static void hist_load(struct hist *h)
{
    char *path = hist_path();
    FILE *f = path ? fopen(path, "r") : NULL;
    char buf[LINE_MAX_BYTES + 2];
    while (f && fgets(buf, sizeof(buf), f)) {
        buf[strcspn(buf, "\n")] = 0;
        hist_add(h, buf);
    }
    if (f)
        fclose(f);
    h->pos = h->n;
}

static void hist_save(const struct hist *h)
{
    char *path = hist_path();
    if (!path)
        return;
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = 0;
        mkdir(dir, 0700);       /* may already exist */
    }
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    for (int i = 0; i < h->n; i++)
        fprintf(f, "%s\n", h->items[i]);
    fclose(f);
}

static void hist_move(struct hist *h, struct line *l, int dir)
{
    int np = h->pos + dir;
    if (np < 0 || np > h->n)
        return;
    if (h->pos == h->n)
        snprintf(h->draft, sizeof(h->draft), "%s", l->text);
    h->pos = np;
    line_set(l, np == h->n ? h->draft : h->items[np]);
}

/* ---- starting the command ------------------------------------------------------------------ */

/* Detached start: fork, setsid, fork again so nothing is left to reap, stdio on /dev/null. */
static int spawn_detached(const char *cmd, int close_fd)
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        setsid();
        pid_t p2 = fork();
        if (p2 != 0)
            _exit(p2 < 0 ? 1 : 0);
        if (close_fd >= 0)
            close(close_fd);
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) {
            dup2(nul, 0);
            dup2(nul, 1);
            dup2(nul, 2);
            if (nul > 2)
                close(nul);
        }
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    int st;
    if (waitpid(pid, &st, 0) < 0)
        return -1;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

/* ---- selftest -------------------------------------------------------------------------------- */

static int selftest(void)
{
    int bad = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "selftest failed: %s (line %d)\n", #c, __LINE__); bad++; } } while (0)
    struct line l = { 0 };
    line_insert(&l, "ls", 2);
    line_insert(&l, " -l", 3);
    CHECK(!strcmp(l.text, "ls -l") && l.cur == 5);
    l.cur = 2;
    line_insert(&l, "x", 1);
    CHECK(!strcmp(l.text, "lsx -l"));
    line_backspace(&l);
    CHECK(!strcmp(l.text, "ls -l") && l.cur == 2);
    line_delete(&l);
    CHECK(!strcmp(l.text, "ls-l"));
    l.cur = 0;
    line_backspace(&l);                         /* nothing before the cursor */
    CHECK(!strcmp(l.text, "ls-l") && l.cur == 0);
    line_set(&l, "t\xc3\xb5u");                 /* t õ u */
    l.cur = 3;
    CHECK(prev_boundary(&l, l.cur) == 1);
    line_backspace(&l);                         /* removes the whole 2-byte õ */
    CHECK(!strcmp(l.text, "tu") && l.cur == 1);
    l.cur = l.len;
    line_delete(&l);                            /* at the end: no-op */
    CHECK(!strcmp(l.text, "tu"));
    for (int i = 0; i < 2000; i++)
        line_insert(&l, "a", 1);                /* bounded */
    CHECK(l.len == LINE_MAX_BYTES);

    struct hist h = { 0 };
    hist_add(&h, "one");
    hist_add(&h, "one");                        /* duplicate of the last entry ignored */
    hist_add(&h, "two");
    hist_add(&h, "");
    CHECK(h.n == 2);
    h.pos = h.n;
    line_set(&l, "draft");
    hist_move(&h, &l, -1);
    CHECK(!strcmp(l.text, "two"));
    hist_move(&h, &l, -1);
    CHECK(!strcmp(l.text, "one"));
    hist_move(&h, &l, -1);                      /* already at the oldest */
    CHECK(!strcmp(l.text, "one"));
    hist_move(&h, &l, 1);
    hist_move(&h, &l, 1);
    CHECK(!strcmp(l.text, "draft"));
    for (int i = 0; i < 3 * HIST_MAX; i++) {
        char b[16];
        snprintf(b, sizeof(b), "c%d", i);
        hist_add(&h, b);
    }
    CHECK(h.n == HIST_MAX);
    for (int i = 0; i < h.n; i++)
        free(h.items[i]);
    return bad ? 1 : 0;
}

/* ---- window ------------------------------------------------------------------------------------ */

#define DEFAULT_W 520
#define DEFAULT_H 64
#define PAD 10
#define COL_BG      0x14171fu
#define COL_FIELD   0x0d0f14u
#define COL_ACCENT  0x6fa8ffu
#define COL_TEXT    0xe6e9efu
#define COL_DIM     0x7b8294u

struct app {
    struct wlc *c;
    uint32_t registry, compositor, shm, wm_base, seat, keyboard;
    uint32_t surface, xsurface, toplevel, pool, buffer;
    int w, h, bw, bh;
    uint32_t *pixels;
    size_t map_size;
    bool configured, running, buffer_busy, redraw;
    uint32_t mods;
    struct line line;
    struct hist hist;
    int repeat_rate, repeat_delay;
    uint32_t repeat_code;
    struct timespec repeat_next;
};

static void fill(struct app *a, int x0, int y0, int w, int h, uint32_t color)
{
    for (int y = LM_MAX(y0, 0); y < LM_MIN(y0 + h, a->bh); y++)
        for (int x = LM_MAX(x0, 0); x < LM_MIN(x0 + w, a->bw); x++)
            a->pixels[(size_t)y * (size_t)a->bw + (size_t)x] = 0xff000000u | color;
}

static void draw_glyph(struct app *a, int x, int y, uint32_t cp, uint32_t color)
{
    const uint8_t *g = lm_font_glyph(cp);
    for (int r = 0; r < LM_FONT_H; r++)
        for (int cx = 0; cx < LM_FONT_W; cx++) {
            int px = x + cx, py = y + r;
            if ((g[r] & (0x80 >> cx)) && px >= 0 && px < a->bw && py >= 0 && py < a->bh)
                a->pixels[(size_t)py * (size_t)a->bw + (size_t)px] = 0xff000000u | color;
        }
}

static void draw_text(struct app *a, int x, int y, const char *s, uint32_t color)
{
    size_t len = strlen(s);
    for (size_t i = 0; i < len;) {
        int used;
        uint32_t cp = lm_utf8_decode(s + i, (int)(len - i), &used);
        i += used > 0 ? (size_t)used : 1;
        draw_glyph(a, x, y, cp, color);
        x += LM_FONT_W;
    }
}

static void paint(struct app *a)
{
    fill(a, 0, 0, a->bw, a->bh, COL_BG);
    int label_w = lm_font_text_width("Run:") + 8;
    int fy = (a->bh - (LM_FONT_H + 12)) / 2;
    draw_text(a, PAD, fy + 6, "Run:", COL_DIM);
    int fx = PAD + label_w, fw = a->bw - fx - PAD;
    fill(a, fx, fy, fw, LM_FONT_H + 12, COL_FIELD);
    fill(a, fx, fy + LM_FONT_H + 11, fw, 1, COL_ACCENT);

    /* scroll horizontally so that the cursor stays visible */
    int cols = (fw - 12) / LM_FONT_W;
    int ccol = 0;                                     /* cursor column = code points before it */
    for (size_t i = 0; i < a->line.cur; i++)
        if (!is_cont((unsigned char)a->line.text[i]))
            ccol++;
    int first = ccol >= cols ? ccol - cols + 1 : 0;   /* first visible column */
    int col = 0;
    const char *s = a->line.text;
    size_t len = a->line.len;
    for (size_t i = 0; i < len;) {
        int used;
        uint32_t cp = lm_utf8_decode(s + i, (int)(len - i), &used);
        i += used > 0 ? (size_t)used : 1;
        if (col >= first && col - first < cols)
            draw_glyph(a, fx + 6 + (col - first) * LM_FONT_W, fy + 6, cp, COL_TEXT);
        col++;
    }
    fill(a, fx + 6 + (ccol - first) * LM_FONT_W, fy + 5, 2, LM_FONT_H + 2, COL_ACCENT);
}

static void setup_buffer(struct app *a)
{
    if (a->pool) {
        WLC_SEND0(a->c, a->pool, LM_WL_SHM_POOL_REQ_DESTROY);
        wlc_forget(a->c, a->pool);
        a->pool = 0;
    }
    if (a->pixels)
        munmap(a->pixels, a->map_size);
    a->bw = a->w;
    a->bh = a->h;
    a->map_size = (size_t)a->bw * (size_t)a->bh * 4;
    int fd = memfd_create("lumen-run", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)a->map_size) < 0) {
        perror("memfd");
        exit(1);
    }
    a->pixels = mmap(NULL, a->map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (a->pixels == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }
    a->pool = wlc_alloc(a->c, &lm_wl_shm_pool_interface, NULL, NULL);
    WLC_SEND(a->c, a->shm, LM_WL_SHM_REQ_CREATE_POOL, { .n = a->pool }, { .h = fd }, { .i = (int32_t)a->map_size });
    close(fd);
}

static void buffer_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)args;
    struct app *a = data;
    if (op != LM_WL_BUFFER_EVT_RELEASE)
        return;
    WLC_SEND0(c, id, LM_WL_BUFFER_REQ_DESTROY);
    wlc_forget(c, id);
    a->buffer = 0;
    a->buffer_busy = false;
}

static void draw(struct app *a)
{
    if (a->bw != a->w || a->bh != a->h || !a->pixels)
        setup_buffer(a);
    paint(a);
    a->buffer = wlc_alloc(a->c, &lm_wl_buffer_interface, buffer_event, a);
    WLC_SEND(a->c, a->pool, LM_WL_SHM_POOL_REQ_CREATE_BUFFER, { .n = a->buffer }, { .i = 0 }, { .i = a->bw },
             { .i = a->bh }, { .i = a->bw * 4 }, { .u = LM_WL_SHM_FORMAT_XRGB8888 });
    WLC_SEND(a->c, a->surface, LM_WL_SURFACE_REQ_ATTACH, { .o = a->buffer }, { .i = 0 }, { .i = 0 });
    WLC_SEND(a->c, a->surface, LM_WL_SURFACE_REQ_DAMAGE, { .i = 0 }, { .i = 0 }, { .i = a->bw }, { .i = a->bh });
    WLC_SEND0(a->c, a->surface, LM_WL_SURFACE_REQ_COMMIT);
    a->buffer_busy = true;
    a->redraw = false;
}

/* ---- keyboard ------------------------------------------------------------------------------------ */

static void run_line(struct app *a)
{
    if (!a->line.len) {
        a->running = false;
        return;
    }
    hist_add(&a->hist, a->line.text);
    hist_save(&a->hist);
    if (spawn_detached(a->line.text, wlc_fd(a->c)) < 0)
        fprintf(stderr, "lumen-run: cannot start '%s': %s\n", a->line.text, strerror(errno));
    a->running = false;
}

static void handle_key(struct app *a, uint32_t code)
{
    uint32_t ks = lm_keymap_keysym(code, a->mods);
    char txt[8];
    bool ctrl = (a->mods & LM_MOD_CTRL) != 0;
    if (ks == LM_KS_Return || ks == LM_KS_KP_Enter) {
        run_line(a);
    } else if (ks == LM_KS_Escape) {
        a->running = false;
    } else if (ks == LM_KS_BackSpace) {
        line_backspace(&a->line);
    } else if (ks == LM_KS_Delete) {
        line_delete(&a->line);
    } else if (ks == LM_KS_Left) {
        a->line.cur = prev_boundary(&a->line, a->line.cur);
    } else if (ks == LM_KS_Right) {
        a->line.cur = next_boundary(&a->line, a->line.cur);
    } else if (ks == LM_KS_Home || (ctrl && (ks == 'a' || ks == 'A'))) {
        a->line.cur = 0;
    } else if (ks == LM_KS_End || (ctrl && (ks == 'e' || ks == 'E'))) {
        a->line.cur = a->line.len;
    } else if (ctrl && (ks == 'u' || ks == 'U')) {
        line_set(&a->line, "");
    } else if (ks == LM_KS_Up) {
        hist_move(&a->hist, &a->line, -1);
    } else if (ks == LM_KS_Down) {
        hist_move(&a->hist, &a->line, 1);
    } else if (!(a->mods & (LM_MOD_CTRL | LM_MOD_ALT | LM_MOD_SUPER))) {
        int n = lm_keysym_to_utf8(ks, txt);
        if (n > 0)
            line_insert(&a->line, txt, (size_t)n);
    }
    a->redraw = true;
}

static void repeat_stop(struct app *a)
{
    a->repeat_code = 0;
}

static void repeat_arm(struct app *a, uint32_t code)
{
    if (a->repeat_rate <= 0)
        return;
    a->repeat_code = code;
    clock_gettime(CLOCK_MONOTONIC, &a->repeat_next);
    long ns = a->repeat_next.tv_nsec + (a->repeat_delay % 1000) * 1000000L;
    a->repeat_next.tv_sec += a->repeat_delay / 1000 + ns / 1000000000L;
    a->repeat_next.tv_nsec = ns % 1000000000L;
}

/* ms until the next repeat is due, -1 if no key repeats */
static int repeat_timeout(const struct app *a)
{
    if (!a->repeat_code)
        return -1;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long ms = (a->repeat_next.tv_sec - now.tv_sec) * 1000L + (a->repeat_next.tv_nsec - now.tv_nsec) / 1000000L;
    return ms < 0 ? 0 : (int)ms;
}

static void repeat_fire(struct app *a)
{
    if (!a->repeat_code || repeat_timeout(a) > 0)
        return;
    handle_key(a, a->repeat_code);
    long iv = 1000000000L / a->repeat_rate;
    long ns = a->repeat_next.tv_nsec + iv;
    a->repeat_next.tv_sec += ns / 1000000000L;
    a->repeat_next.tv_nsec = ns % 1000000000L;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);       /* do not burst after a stall */
    if (now.tv_sec > a->repeat_next.tv_sec ||
        (now.tv_sec == a->repeat_next.tv_sec && now.tv_nsec > a->repeat_next.tv_nsec))
        a->repeat_next = now;
}

/* ---- protocol events ------------------------------------------------------------------------------- */

static void wm_base_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)data;
    if (op == LM_XDG_WM_BASE_EVT_PING)
        WLC_SEND(c, id, LM_XDG_WM_BASE_REQ_PONG, { .u = args[0].u });
}

static void xsurface_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    struct app *a = data;
    if (op == LM_XDG_SURFACE_EVT_CONFIGURE) {
        WLC_SEND(c, id, LM_XDG_SURFACE_REQ_ACK_CONFIGURE, { .u = args[0].u });
        a->configured = true;
        a->redraw = true;
    }
}

static void toplevel_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)c; (void)id;
    struct app *a = data;
    if (op == LM_XDG_TOPLEVEL_EVT_CONFIGURE) {
        a->w = LM_MAX(args[0].i > 0 ? args[0].i : DEFAULT_W, 160);
        a->h = LM_MAX(args[1].i > 0 ? args[1].i : DEFAULT_H, 40);
    } else if (op == LM_XDG_TOPLEVEL_EVT_CLOSE) {
        a->running = false;
    }
}

static void keyboard_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)c; (void)id;
    struct app *a = data;
    switch (op) {
    case LM_WL_KEYBOARD_EVT_KEYMAP:
        lm_keymap_client_load(args[1].h, args[2].u);
        break;
    case LM_WL_KEYBOARD_EVT_LEAVE:
        repeat_stop(a);
        break;
    case LM_WL_KEYBOARD_EVT_MODIFIERS:
        a->mods = args[1].u | args[2].u | args[3].u;
        lm_keymap_client_group(args[4].u);
        break;
    case LM_WL_KEYBOARD_EVT_REPEAT_INFO:
        a->repeat_rate = args[0].i;
        a->repeat_delay = args[1].i;
        break;
    case LM_WL_KEYBOARD_EVT_KEY:
        if (args[3].u == 0) {
            if (a->repeat_code == args[2].u)
                repeat_stop(a);
            break;
        }
        handle_key(a, args[2].u);
        if (a->running) {
            uint32_t ks = lm_keymap_keysym(args[2].u, a->mods);
            if (ks != LM_KS_Return && ks != LM_KS_Escape)
                repeat_arm(a, args[2].u);
        }
        break;
    }
}

static void seat_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    struct app *a = data;
    if (op == LM_WL_SEAT_EVT_CAPABILITIES && (args[0].u & 2) && !a->keyboard) {
        a->keyboard = wlc_alloc(c, &lm_wl_keyboard_interface, keyboard_event, a);
        WLC_SEND(c, id, LM_WL_SEAT_REQ_GET_KEYBOARD, { .n = a->keyboard });
    }
}

static void registry_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)id;
    struct app *a = data;
    if (op != LM_WL_REGISTRY_EVT_GLOBAL)
        return;
    uint32_t name = args[0].u, ver = args[2].u;
    const char *iface = args[1].s;
    if (!strcmp(iface, "wl_compositor"))
        a->compositor = wlc_bind(c, a->registry, name, &lm_wl_compositor_interface, LM_MIN(ver, 4u), NULL, a);
    else if (!strcmp(iface, "wl_shm"))
        a->shm = wlc_bind(c, a->registry, name, &lm_wl_shm_interface, 1, NULL, a);
    else if (!strcmp(iface, "xdg_wm_base"))
        a->wm_base = wlc_bind(c, a->registry, name, &lm_xdg_wm_base_interface, LM_MIN(ver, 3u), wm_base_event, a);
    else if (!strcmp(iface, "wl_seat"))
        a->seat = wlc_bind(c, a->registry, name, &lm_wl_seat_interface, LM_MIN(ver, 5u), seat_event, a);
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--selftest"))
        return selftest();

    struct app a = { .w = DEFAULT_W, .h = DEFAULT_H, .running = true, .repeat_rate = 25, .repeat_delay = 400 };
    hist_load(&a.hist);
    a.c = wlc_connect(NULL);
    if (!a.c)
        return 1;
    a.registry = wlc_get_registry(a.c, registry_event, &a);
    if (wlc_roundtrip(a.c) < 0 || !a.compositor || !a.shm || !a.wm_base || !a.seat) {
        fprintf(stderr, "lumen-run: compositor lacks wl_compositor/wl_shm/xdg_wm_base/wl_seat\n");
        return 1;
    }
    a.surface = wlc_alloc(a.c, &lm_wl_surface_interface, NULL, NULL);
    WLC_SEND(a.c, a.compositor, LM_WL_COMPOSITOR_REQ_CREATE_SURFACE, { .n = a.surface });
    a.xsurface = wlc_alloc(a.c, &lm_xdg_surface_interface, xsurface_event, &a);
    WLC_SEND(a.c, a.wm_base, LM_XDG_WM_BASE_REQ_GET_XDG_SURFACE, { .n = a.xsurface }, { .o = a.surface });
    a.toplevel = wlc_alloc(a.c, &lm_xdg_toplevel_interface, toplevel_event, &a);
    WLC_SEND(a.c, a.xsurface, LM_XDG_SURFACE_REQ_GET_TOPLEVEL, { .n = a.toplevel });
    WLC_SEND(a.c, a.toplevel, LM_XDG_TOPLEVEL_REQ_SET_TITLE, { .s = "Run" });
    WLC_SEND(a.c, a.toplevel, LM_XDG_TOPLEVEL_REQ_SET_APP_ID, { .s = "lumen.run" });
    WLC_SEND0(a.c, a.surface, LM_WL_SURFACE_REQ_COMMIT);

    while (a.running && !wlc_dead(a.c)) {
        if (a.redraw && a.configured && !a.buffer_busy)
            draw(&a);
        if (wlc_dispatch(a.c, repeat_timeout(&a)) < 0)
            break;
        repeat_fire(&a);
    }
    wlc_destroy(a.c);
    return 0;
}
