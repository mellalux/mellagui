/*
 * mellagui-calc: small desktop calculator (wl_shm + xdg-shell, bitmap font).
 * Mouse buttons or keyboard: digits . + - * / % Enter/= Backspace, Esc/c clear.
 * Operators are evaluated left to right, like a pocket calculator.
 * `mellagui-calc --selftest` runs the arithmetic engine checks without a display.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "theme.h"
#include "font.h"
#include "keymap.h"
#include "util.h"
#include "wlwin.h"

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
    puts("mellagui-calc selftest ok");
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

#define COL_BG      (lm_theme.background)
#define COL_DISPLAY (lm_theme.field)
#define COL_KEY     (lm_theme.button)
#define COL_OP      (lm_theme.button_alt)
#define COL_HOVER   (lm_theme.button_hover)
#define COL_DOWN    (lm_theme.button_down)
#define COL_ACCENT  (lm_theme.accent)
#define COL_TEXT    (lm_theme.text)
#define COL_DIM     (lm_theme.text_dim)
struct app {
    struct wlwin win;
    double px, py;
    bool has_pointer;
    int press;                  /* button index pressed with the pointer, -1 none */
    struct calc calc;
};

struct rect { int x, y, w, h; };

static struct rect display_rect(const struct app *a)
{
    int dh = a->win.bh / 6;
    return (struct rect){ PAD, PAD, a->win.bw - 2 * PAD, dh };
}

static struct rect key_rect(const struct app *a, int row, int col)
{
    struct rect d = display_rect(a);
    int top = d.y + d.h + PAD;
    int cw = (a->win.bw - 2 * PAD - (COLS - 1) * PAD) / COLS;
    int ch = (a->win.bh - top - PAD - (ROWS - 1) * PAD) / ROWS;
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
    wlwin_fill(&a->win, r.x, r.y, r.w, r.h, color);
}

static void draw_text_in(struct app *a, struct rect r, const char *s, int scale, uint32_t color, bool right)
{
    int tw = lm_font_text_width(s) * scale;
    int x = right ? r.x + r.w - tw - 8 : r.x + (r.w - tw) / 2;
    if (x < r.x)
        x = r.x;
    wlwin_text(&a->win, x, r.y + (r.h - LM_FONT_H * scale) / 2, s, scale, color, 0);
}

static void paint(struct wlwin *w)
{
    struct app *a = w->data;
    fill(a, (struct rect){ 0, 0, a->win.bw, a->win.bh }, COL_BG);
    struct rect d = display_rect(a);
    fill(a, d, COL_DISPLAY);

    char text[64];
    calc_display(&a->calc, text, sizeof(text));
    int scale = 2;
    while (scale > 1 && lm_font_text_width(text) * scale > d.w - 16)
        scale--;
    if (a->calc.op && !a->calc.error) {
        char op[2] = { a->calc.op == '*' ? 'x' : a->calc.op, 0 };
        wlwin_text(&a->win, d.x + 8, d.y + 4, op, 1, COL_DIM, 0);
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

static void pointer_event(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
    struct app *a = w->data;
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
            a->win.redraw = true;
        }
        break;
    }
    int after = a->has_pointer ? key_at(a, a->px, a->py) : -1;
    if (op == LM_WL_POINTER_EVT_BUTTON || before != after)
        a->win.redraw = true;
}

static void keyboard_event(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
    struct app *a = w->data;
    switch (op) {
    case LM_WL_KEYBOARD_EVT_KEY: {
        if (args[3].u == 0)
            break;
        uint32_t ks = lm_keymap_keysym(args[2].u, a->win.mods);
        char txt[8];
        char key = 0;
        if (ks == LM_KS_Return || ks == LM_KS_KP_Enter)
            key = '=';
        else if (ks == LM_KS_BackSpace)
            key = '<';
        else if (ks == LM_KS_Escape || ks == LM_KS_Delete)
            key = 'C';
        else if (lm_keysym_to_utf8(ks, txt) == 1 && !(a->win.mods & (LM_MOD_CTRL | LM_MOD_ALT | LM_MOD_SUPER))) {
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
            a->win.redraw = true;
        }
        break;
    }
    }
}

int main(int argc, char **argv)
{
    lm_theme_load_env();
    if (argc > 1 && !strcmp(argv[1], "--selftest"))
        return selftest();

    static const struct wlwin_ops ops = { .paint = paint, .pointer = pointer_event, .keyboard = keyboard_event };
    struct app a = { .press = -1 };
    if (wlwin_connect(&a.win, "mellagui-calc", &ops, &a) < 0)
        return 1;
    if (wlwin_open(&a.win, "Calculator", "mellagui.calc", DEFAULT_W, DEFAULT_H, 160, 200) < 0)
        return 1;
    while (wlwin_step(&a.win, -1))
        ;
    wlwin_destroy(&a.win);
    return 0;
}
