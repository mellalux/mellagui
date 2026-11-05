/*
 * lumen-calc: small desktop calculator (wl_shm + xdg-shell, bitmap font).
 * Mouse buttons or keyboard: digits . + - * / % Enter/= Backspace, Esc/c clear.
 * Operators are evaluated left to right, like a pocket calculator.
 * `lumen-calc --selftest` runs the arithmetic engine checks without a display.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "font.h"
#include "keymap.h"
#include "util.h"
#include "wlc.h"

/* ---- engine ---------------------------------------------------------------------------- */

#define ENTRY_MAX 15

struct calc {
    double acc;
    char op;                    /* pending operator or 0 */
    char entry[ENTRY_MAX + 2];  /* number being typed, valid if have_entry */
    bool have_entry;
    bool error;
};

static void calc_clear(struct calc *c)
{
    memset(c, 0, sizeof(*c));
}

static void calc_fmt(double v, char *out, size_t n)
{
    snprintf(out, n, "%.10g", v == 0 ? 0.0 : v); /* no "-0" */
}

static void calc_display(const struct calc *c, char *out, size_t n)
{
    if (c->error)
        snprintf(out, n, "Error");
    else if (c->have_entry)
        snprintf(out, n, "%s", c->entry[0] ? c->entry : "0");
    else
        calc_fmt(c->acc, out, n);
}

static double calc_value(const struct calc *c)
{
    return c->have_entry ? atof(c->entry) : c->acc;
}

static void calc_digit(struct calc *c, char ch)
{
    if (c->error)
        calc_clear(c);
    if (!c->have_entry) {
        c->entry[0] = 0;
        c->have_entry = true;
    }
    size_t len = strlen(c->entry);
    if (ch == '.') {
        if (strchr(c->entry, '.'))
            return;
        if (len == 0 || (len == 1 && c->entry[0] == '-')) {
            if (len + 1 >= ENTRY_MAX)
                return;
            c->entry[len++] = '0';
        }
    }
    if (len >= ENTRY_MAX)
        return;
    if (ch != '.' && !strcmp(c->entry, "0"))
        len = 0; /* no leading zeros */
    else if (ch != '.' && !strcmp(c->entry, "-0"))
        len = 1;
    c->entry[len] = ch;
    c->entry[len + 1] = 0;
}

static void calc_apply(struct calc *c, double rhs)
{
    switch (c->op) {
    case '+': c->acc += rhs; break;
    case '-': c->acc -= rhs; break;
    case '*': c->acc *= rhs; break;
    case '/':
        if (rhs == 0)
            c->error = true;
        else
            c->acc /= rhs;
        break;
    default: c->acc = rhs;
    }
    if (!isfinite(c->acc))
        c->error = true;
}

static void calc_operator(struct calc *c, char op)
{
    if (c->error)
        return;
    if (c->have_entry) {
        calc_apply(c, atof(c->entry));
        c->have_entry = false;
    }
    c->op = op;
}

static void calc_equals(struct calc *c)
{
    if (c->error)
        return;
    if (c->have_entry) {
        calc_apply(c, atof(c->entry));
        c->have_entry = false;
    }
    c->op = 0;
}

static void calc_negate(struct calc *c)
{
    if (c->error)
        return;
    if (!c->have_entry) {
        c->acc = -c->acc;
    } else if (c->entry[0] == '-') {
        memmove(c->entry, c->entry + 1, strlen(c->entry));
    } else if (strlen(c->entry) < ENTRY_MAX) {
        memmove(c->entry + 1, c->entry, strlen(c->entry) + 1);
        c->entry[0] = '-';
    }
}

static void calc_percent(struct calc *c)
{
    if (c->error)
        return;
    double v = calc_value(c) / 100.0;
    if (c->have_entry) {
        calc_fmt(v, c->entry, sizeof(c->entry));
    } else {
        c->acc = v;
    }
}

static void calc_backspace(struct calc *c)
{
    if (c->error) {
        calc_clear(c);
        return;
    }
    if (!c->have_entry)
        return;
    size_t len = strlen(c->entry);
    if (len)
        c->entry[len - 1] = 0;
    if (!strcmp(c->entry, "-"))
        c->entry[0] = 0;
}

/* Feed one key label of the button grid / keyboard ("0".."9", ".", "+", "-", "*", "/", "=", "%", "n"=negate, "<"=backspace, "C"). */
static void calc_press(struct calc *c, char key)
{
    switch (key) {
    case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9': case '.':
        calc_digit(c, key);
        break;
    case '+': case '-': case '*': case '/': calc_operator(c, key); break;
    case '=': calc_equals(c); break;
    case '%': calc_percent(c); break;
    case 'n': calc_negate(c); break;
    case '<': calc_backspace(c); break;
    case 'C': calc_clear(c); break;
    }
}

static int selftest_fail;

static void expect(const char *keys, const char *want)
{
    struct calc c;
    char got[64];
    calc_clear(&c);
    for (const char *k = keys; *k; k++)
        calc_press(&c, *k);
    calc_display(&c, got, sizeof(got));
    if (strcmp(got, want)) {
        fprintf(stderr, "selftest: %s -> '%s', expected '%s'\n", keys, got, want);
        selftest_fail++;
    }
}

static int selftest(void)
{
    expect("", "0");
    expect("123", "123");
    expect("007", "7");
    expect("2+3=", "5");
    expect("2+3*4=", "20");            /* left to right */
    expect("9-10=", "-1");
    expect("1/0=", "Error");
    expect("1/0=5", "5");               /* typing clears the error */
    expect("0.1+0.2=", "0.3");
    expect("1.5.5", "1.55");           /* second dot ignored */
    expect(".5", "0.5");
    expect("12<", "1");
    expect("1<<", "0");
    expect("5n", "-5");
    expect("5nn", "5");
    expect("5=n", "-5");                /* negate the result */
    expect("50%", "0.5");
    expect("200+10%=", "200.1");
    expect("2*3=+4=", "10");            /* continue from the result */
    expect("2*3=7", "7");               /* a new number after = starts fresh */
    expect("2+*3=", "6");               /* a second operator replaces the first */
    expect("12C", "0");
    expect("1234567890123456789", "123456789012345");
    if (selftest_fail)
        return 1;
    puts("lumen-calc selftest ok");
    return 0;
}

/* ---- UI --------------------------------------------------------------------------------- */

#define ROWS 5
#define COLS 4
#define PAD 8
#define DEFAULT_W 264
#define DEFAULT_H 360

static const char KEYS[ROWS][COLS] = {
    { 'C', '<', '%', '/' },
    { '7', '8', '9', '*' },
    { '4', '5', '6', '-' },
    { '1', '2', '3', '+' },
    { 'n', '0', '.', '=' },
};
static const char *const LABELS[ROWS][COLS] = {
    { "C", "<-", "%", "/" },
    { "7", "8", "9", "x" },
    { "4", "5", "6", "-" },
    { "1", "2", "3", "+" },
    { "+/-", "0", ".", "=" },
};

#define COL_BG      0x14171fu
#define COL_DISPLAY 0x0d0f14u
#define COL_KEY     0x2d3a5au
#define COL_OP      0x3a4a72u
#define COL_HOVER   0x41527eu
#define COL_DOWN    0x56699cu
#define COL_ACCENT  0x6fa8ffu
#define COL_TEXT    0xe6e9efu
#define COL_DIM     0x7b8294u

struct app {
    struct wlc *c;
    uint32_t registry, compositor, shm, wm_base, seat, pointer, keyboard;
    uint32_t surface, xsurface, toplevel, pool, buffer;
    int w, h, bw, bh;
    uint32_t *pixels;
    size_t map_size;
    bool configured, running, buffer_busy, redraw;
    uint32_t mods;
    double px, py;
    bool has_pointer;
    int press;                  /* button index pressed with the pointer, -1 none */
    struct calc calc;
};

struct rect { int x, y, w, h; };

static struct rect display_rect(const struct app *a)
{
    int dh = a->bh / 6;
    return (struct rect){ PAD, PAD, a->bw - 2 * PAD, dh };
}

static struct rect key_rect(const struct app *a, int row, int col)
{
    struct rect d = display_rect(a);
    int top = d.y + d.h + PAD;
    int cw = (a->bw - 2 * PAD - (COLS - 1) * PAD) / COLS;
    int ch = (a->bh - top - PAD - (ROWS - 1) * PAD) / ROWS;
    return (struct rect){ PAD + col * (cw + PAD), top + row * (ch + PAD), cw, ch };
}

static int key_at(const struct app *a, double x, double y)
{
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
            struct rect k = key_rect(a, r, c);
            if (x >= k.x && x < k.x + k.w && y >= k.y && y < k.y + k.h)
                return r * COLS + c;
        }
    return -1;
}

static void fill(struct app *a, struct rect r, uint32_t color)
{
    for (int y = LM_MAX(r.y, 0); y < LM_MIN(r.y + r.h, a->bh); y++)
        for (int x = LM_MAX(r.x, 0); x < LM_MIN(r.x + r.w, a->bw); x++)
            a->pixels[(size_t)y * (size_t)a->bw + (size_t)x] = 0xff000000u | color;
}

static void draw_text(struct app *a, int x, int y, const char *s, int scale, uint32_t color)
{
    size_t len = strlen(s);
    for (size_t i = 0; i < len;) {
        int used;
        uint32_t cp = lm_utf8_decode(s + i, (int)(len - i), &used);
        i += used > 0 ? (size_t)used : 1;
        const uint8_t *g = lm_font_glyph(cp);
        for (int r = 0; r < LM_FONT_H * scale; r++)
            for (int cx = 0; cx < LM_FONT_W * scale; cx++) {
                int px = x + cx, py = y + r;
                if ((g[r / scale] & (0x80 >> (cx / scale))) && px >= 0 && px < a->bw && py >= 0 && py < a->bh)
                    a->pixels[(size_t)py * (size_t)a->bw + (size_t)px] = 0xff000000u | color;
            }
        x += LM_FONT_W * scale;
    }
}

static void draw_text_in(struct app *a, struct rect r, const char *s, int scale, uint32_t color, bool right)
{
    int tw = lm_font_text_width(s) * scale;
    int x = right ? r.x + r.w - tw - 8 : r.x + (r.w - tw) / 2;
    if (x < r.x)
        x = r.x;
    draw_text(a, x, r.y + (r.h - LM_FONT_H * scale) / 2, s, scale, color);
}

static void paint(struct app *a)
{
    fill(a, (struct rect){ 0, 0, a->bw, a->bh }, COL_BG);
    struct rect d = display_rect(a);
    fill(a, d, COL_DISPLAY);

    char text[64];
    calc_display(&a->calc, text, sizeof(text));
    int scale = 2;
    while (scale > 1 && lm_font_text_width(text) * scale > d.w - 16)
        scale--;
    if (a->calc.op && !a->calc.error) {
        char op[2] = { a->calc.op == '*' ? 'x' : a->calc.op, 0 };
        draw_text(a, d.x + 8, d.y + 4, op, 1, COL_DIM);
    }
    draw_text_in(a, d, text, scale, a->calc.error ? 0xff7070u : COL_TEXT, true);

    int hover = a->has_pointer ? key_at(a, a->px, a->py) : -1;
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
            int idx = r * COLS + c;
            char k = KEYS[r][c];
            bool is_op = k == '/' || k == '*' || k == '-' || k == '+';
            uint32_t bg = k == '=' ? COL_ACCENT : (is_op ? COL_OP : COL_KEY);
            if (idx == hover)
                bg = a->press == idx ? COL_DOWN : COL_HOVER;
            struct rect kr = key_rect(a, r, c);
            fill(a, kr, bg);
            draw_text_in(a, kr, LABELS[r][c], 2, k == '=' && idx != hover ? COL_BG : COL_TEXT, false);
        }
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
    int fd = memfd_create("lumen-calc", MFD_CLOEXEC);
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

/* ---- protocol events -------------------------------------------------------------------- */

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
        a->w = args[0].i > 0 ? args[0].i : DEFAULT_W;
        a->h = args[1].i > 0 ? args[1].i : DEFAULT_H;
        a->w = LM_MAX(a->w, 160);
        a->h = LM_MAX(a->h, 200);
    } else if (op == LM_XDG_TOPLEVEL_EVT_CLOSE) {
        a->running = false;
    }
}

static void pointer_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)c; (void)id;
    struct app *a = data;
    int before = a->has_pointer ? key_at(a, a->px, a->py) : -1;
    switch (op) {
    case LM_WL_POINTER_EVT_ENTER:
        a->has_pointer = true;
        a->px = lm_fixed_to_double(args[2].f);
        a->py = lm_fixed_to_double(args[3].f);
        break;
    case LM_WL_POINTER_EVT_LEAVE:
        a->has_pointer = false;
        a->press = -1;
        break;
    case LM_WL_POINTER_EVT_MOTION:
        a->px = lm_fixed_to_double(args[1].f);
        a->py = lm_fixed_to_double(args[2].f);
        break;
    case LM_WL_POINTER_EVT_BUTTON:
        if (args[2].u != 272) /* BTN_LEFT */
            break;
        if (args[3].u == 1) {
            a->press = key_at(a, a->px, a->py);
        } else {
            int idx = key_at(a, a->px, a->py);
            if (idx >= 0 && idx == a->press)
                calc_press(&a->calc, KEYS[idx / COLS][idx % COLS]);
            a->press = -1;
            a->redraw = true;
        }
        break;
    }
    int after = a->has_pointer ? key_at(a, a->px, a->py) : -1;
    if (op == LM_WL_POINTER_EVT_BUTTON || before != after)
        a->redraw = true;
}

static void keyboard_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)c; (void)id;
    struct app *a = data;
    switch (op) {
    case LM_WL_KEYBOARD_EVT_KEYMAP:
        close(args[1].h);
        break;
    case LM_WL_KEYBOARD_EVT_MODIFIERS:
        a->mods = args[1].u | args[3].u;
        break;
    case LM_WL_KEYBOARD_EVT_KEY: {
        if (args[3].u == 0)
            break;
        uint32_t ks = lm_keymap_us_keysym(args[2].u, a->mods);
        char txt[8];
        char key = 0;
        if (ks == LM_KS_Return || ks == LM_KS_KP_Enter)
            key = '=';
        else if (ks == LM_KS_BackSpace)
            key = '<';
        else if (ks == LM_KS_Escape || ks == LM_KS_Delete)
            key = 'C';
        else if (lm_keysym_to_utf8(ks, txt) == 1 && !(a->mods & (LM_MOD_CTRL | LM_MOD_ALT | LM_MOD_SUPER))) {
            if (strchr("0123456789.+-*/%=", txt[0]))
                key = txt[0];
            else if (txt[0] == 'c' || txt[0] == 'C')
                key = 'C';
            else if (txt[0] == ',')
                key = '.';
            else if (txt[0] == 'x' || txt[0] == 'X')
                key = '*';
        }
        if (key) {
            calc_press(&a->calc, key);
            a->redraw = true;
        }
        break;
    }
    }
}

static void seat_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    struct app *a = data;
    if (op == LM_WL_SEAT_EVT_CAPABILITIES) {
        if ((args[0].u & 1) && !a->pointer) {
            a->pointer = wlc_alloc(c, &lm_wl_pointer_interface, pointer_event, a);
            WLC_SEND(c, id, LM_WL_SEAT_REQ_GET_POINTER, { .n = a->pointer });
        }
        if ((args[0].u & 2) && !a->keyboard) {
            a->keyboard = wlc_alloc(c, &lm_wl_keyboard_interface, keyboard_event, a);
            WLC_SEND(c, id, LM_WL_SEAT_REQ_GET_KEYBOARD, { .n = a->keyboard });
        }
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

    struct app a = { .w = DEFAULT_W, .h = DEFAULT_H, .running = true, .press = -1 };
    a.c = wlc_connect(NULL);
    if (!a.c)
        return 1;
    a.registry = wlc_get_registry(a.c, registry_event, &a);
    if (wlc_roundtrip(a.c) < 0 || !a.compositor || !a.shm || !a.wm_base) {
        fprintf(stderr, "lumen-calc: compositor lacks wl_compositor/wl_shm/xdg_wm_base\n");
        return 1;
    }
    a.surface = wlc_alloc(a.c, &lm_wl_surface_interface, NULL, NULL);
    WLC_SEND(a.c, a.compositor, LM_WL_COMPOSITOR_REQ_CREATE_SURFACE, { .n = a.surface });
    a.xsurface = wlc_alloc(a.c, &lm_xdg_surface_interface, xsurface_event, &a);
    WLC_SEND(a.c, a.wm_base, LM_XDG_WM_BASE_REQ_GET_XDG_SURFACE, { .n = a.xsurface }, { .o = a.surface });
    a.toplevel = wlc_alloc(a.c, &lm_xdg_toplevel_interface, toplevel_event, &a);
    WLC_SEND(a.c, a.xsurface, LM_XDG_SURFACE_REQ_GET_TOPLEVEL, { .n = a.toplevel });
    WLC_SEND(a.c, a.toplevel, LM_XDG_TOPLEVEL_REQ_SET_TITLE, { .s = "Calculator" });
    WLC_SEND(a.c, a.toplevel, LM_XDG_TOPLEVEL_REQ_SET_APP_ID, { .s = "lumen.calc" });
    WLC_SEND0(a.c, a.surface, LM_WL_SURFACE_REQ_COMMIT);

    while (a.running && !wlc_dead(a.c)) {
        if (a.redraw && a.configured && !a.buffer_busy)
            draw(&a);
        if (wlc_dispatch(a.c, -1) < 0)
            break;
    }
    wlc_destroy(a.c);
    return 0;
}
