/*
 * mellagui-calc: small desktop calculator (wl_shm + xdg-shell, bitmap font).
 * Mouse buttons or keyboard: digits . + - * / % Enter/= Backspace, Esc/c clear.
 * Operators are evaluated left to right, like a pocket calculator (5 + = gives 10); the line above the result shows the sum so far.
 * Round colourful keys like the other MellaGUI programs. Besides the four operations and %:
 *   r  square root      q  square      i  1/x      n  +/-
 *   memory: p = M+, o = M-, m = MR (recall), z = MC (clear); the display shows "M" while something is stored
 *   Ctrl+C copies the number on the display, Ctrl+V pastes a number (the first one found in the clipboard text)
 * `mellagui-calc --selftest` runs the arithmetic engine checks without a display.
 */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "theme.h"
#include "font.h"
#include "keymap.h"
#include "uikit.h"
#include "util.h"
#include "wlclip.h"
#include "wlwin.h"

/* ---- engine ---------------------------------------------------------------------------- */

#define ENTRY_MAX 15
#define EXPR_MAX 96

struct calc {
    double acc;
    char op;                    /* pending operator or 0 */
    char entry[ENTRY_MAX + 2];  /* number being typed, valid if have_entry */
    bool have_entry;
    bool error;
    double mem;                 /* the memory (M+ / M- / MR / MC) ... */
    bool has_mem;               /* ... and whether anything is stored */
    char expr[EXPR_MAX];        /* the sum so far, "2 + 3 x " or "2 + 3 =" */
    bool done;                  /* the sum ended with = (or a one-number function): the next digit starts a new one */
};

/* Clears the sum, not the memory. */
static void calc_clear(struct calc *c)
{
    double mem = c->mem;
    bool has = c->has_mem;
    memset(c, 0, sizeof(*c));
    c->mem = mem;
    c->has_mem = has;
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

static void expr_add(struct calc *c, const char *s)
{
    size_t len = strlen(c->expr), add = strlen(s);
    if (len + add >= sizeof(c->expr)) {                                 /* too long: the oldest part goes */
        size_t drop = len + add - (sizeof(c->expr) - 1);
        if (drop > len)
            drop = len;
        memmove(c->expr, c->expr + drop, len - drop + 1);
        len -= drop;
        if (add >= sizeof(c->expr))
            add = sizeof(c->expr) - 1;
    }
    memcpy(c->expr + len, s, add);
    c->expr[len + add] = 0;
}

static void calc_digit(struct calc *c, char ch)
{
    if (c->error)
        calc_clear(c);
    if (c->done) {                                                      /* a new sum */
        c->expr[0] = 0;
        c->done = false;
    }
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

static char op_char(char op)
{
    return op == '*' ? 'x' : op;
}

static void calc_operator(struct calc *c, char op)
{
    if (c->error)
        return;
    if (c->done) {                                                      /* carry on from the result */
        c->expr[0] = 0;
        c->done = false;
    }
    char sym[8], num[ENTRY_MAX + 4];
    if (c->have_entry) {
        snprintf(num, sizeof(num), "%s", c->entry[0] ? c->entry : "0");
        calc_apply(c, atof(c->entry));
        c->have_entry = false;
        expr_add(c, num);
    } else if (c->op && c->expr[0]) {                                   /* a second operator replaces the first */
        size_t len = strlen(c->expr);
        if (len >= 2)
            c->expr[len - 2] = op_char(op);
        c->op = op;
        return;
    } else {
        calc_fmt(c->acc, num, sizeof(num));
        expr_add(c, num);
    }
    snprintf(sym, sizeof(sym), " %c ", op_char(op));
    expr_add(c, sym);
    c->op = op;
}

static void calc_equals(struct calc *c)
{
    if (c->error)
        return;
    if (c->have_entry) {
        char num[ENTRY_MAX + 4];
        snprintf(num, sizeof(num), "%s", c->entry[0] ? c->entry : "0");
        calc_apply(c, atof(c->entry));
        c->have_entry = false;
        if (!c->error) {
            expr_add(c, num);
            expr_add(c, " =");
            c->done = true;
        }
    } else if (c->op) {                                                 /* "5 + =": the number on the display is used twice, like a pocket calculator: 10 */
        char num[ENTRY_MAX + 4];
        calc_fmt(c->acc, num, sizeof(num));
        calc_apply(c, c->acc);
        if (!c->error) {
            expr_add(c, num);
            expr_add(c, " =");
            c->done = true;
        }
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

/* A function of the number on the display: r = square root, s = square, i = 1/x. */
static void calc_unary(struct calc *c, char fn)
{
    if (c->error)
        return;
    double v = calc_value(c), r = 0;
    const char *name = "";
    switch (fn) {
    case 'r':
        name = "sqrt";
        if (v < 0) {
            c->error = true;
            return;
        }
        r = sqrt(v);
        break;
    case 's':
        name = "sqr";
        r = v * v;
        break;
    default:
        name = "1/";
        if (v == 0) {
            c->error = true;
            return;
        }
        r = 1.0 / v;
        break;
    }
    if (!isfinite(r)) {
        c->error = true;
        return;
    }
    if (!c->op) {                                                       /* alone: the line above shows what was done */
        char num[ENTRY_MAX + 4], line[EXPR_MAX];
        calc_fmt(v, num, sizeof(num));
        if (fn == 'i')
            snprintf(line, sizeof(line), "1/(%s) =", num);
        else
            snprintf(line, sizeof(line), "%s(%s) =", name, num);
        c->expr[0] = 0;
        expr_add(c, line);
        c->done = true;
    }
    if (c->have_entry)
        calc_fmt(r, c->entry, sizeof(c->entry));
    else
        c->acc = r;
}

/* The memory keys: Q = MC, R = MR, M = M+, N = M-. */
static void calc_memory(struct calc *c, char key)
{
    if (c->error)
        return;
    switch (key) {
    case 'Q':
        c->mem = 0;
        c->has_mem = false;
        break;
    case 'R':
        if (c->done) {
            c->expr[0] = 0;
            c->done = false;
        }
        calc_fmt(c->mem, c->entry, sizeof(c->entry));
        c->have_entry = true;
        break;
    default: {
        double v = calc_value(c);
        c->mem += key == 'M' ? v : -v;
        c->has_mem = true;
        if (!c->op && c->have_entry) {                                  /* the next digit starts a new number */
            c->acc = v;
            c->have_entry = false;
        }
        break;
    }
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

/* Puts the first number found in `text` on the display as if it had been typed. */
static bool calc_paste(struct calc *c, const char *text)
{
    char num[ENTRY_MAX + 2];
    size_t n = 0;
    bool dot = false;
    for (const char *p = text; *p && n == 0; p++) {
        if (!(isdigit((unsigned char)*p) || ((*p == '-' || *p == '.' || *p == ',') && (isdigit((unsigned char)p[1]) || p[1] == '.' || p[1] == ','))))
            continue;
        for (; *p && n < ENTRY_MAX; p++) {
            if (isdigit((unsigned char)*p) || (n == 0 && *p == '-')) {
                num[n++] = *p;
            } else if ((*p == '.' || *p == ',') && !dot) {
                dot = true;
                if (n == 0 || (n == 1 && num[0] == '-'))
                    num[n++] = '0';
                num[n++] = '.';
            } else {
                break;
            }
        }
    }
    if (n == 0)
        return false;
    num[n] = 0;
    if (c->error)
        calc_clear(c);
    if (c->done) {
        c->expr[0] = 0;
        c->done = false;
    }
    snprintf(c->entry, sizeof(c->entry), "%s", num);
    c->have_entry = true;
    return true;
}

/* Feed one key label of the button grid / keyboard ("0".."9", ".", "+", "-", "*", "/", "=", "%", "n"=negate, "<"=backspace, "C",
 * "r" "s" "i" = sqrt, square, 1/x; "Q" "R" "M" "N" = MC, MR, M+, M-). */
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
    case 'r': case 's': case 'i': calc_unary(c, key); break;
    case 'Q': case 'R': case 'M': case 'N': calc_memory(c, key); break;
    }
}

static int selftest_fail;

static void expect(const char *keys, const char *want)
{
    struct calc c;
    char got[64];
    memset(&c, 0, sizeof(c));
    for (const char *k = keys; *k; k++)
        calc_press(&c, *k);
    calc_display(&c, got, sizeof(got));
    if (strcmp(got, want)) {
        fprintf(stderr, "selftest: %s -> '%s', expected '%s'\n", keys, got, want);
        selftest_fail++;
    }
}

static void expect_expr(const char *keys, const char *want)
{
    struct calc c;
    memset(&c, 0, sizeof(c));
    for (const char *k = keys; *k; k++)
        calc_press(&c, *k);
    if (strcmp(c.expr, want)) {
        fprintf(stderr, "selftest: %s -> sum '%s', expected '%s'\n", keys, c.expr, want);
        selftest_fail++;
    }
}

static void expect_paste(const char *text, const char *want)
{
    struct calc c;
    char got[64];
    memset(&c, 0, sizeof(c));
    bool ok = calc_paste(&c, text);
    if (!ok)
        snprintf(got, sizeof(got), "(none)");
    else
        calc_display(&c, got, sizeof(got));
    if (strcmp(got, want)) {
        fprintf(stderr, "selftest: paste '%s' -> '%s', expected '%s'\n", text, got, want);
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
    expect("5+=", "10");                /* "=" right after an operator uses the displayed number twice */
    expect("6*=", "36");
    expect("5-=", "0");
    expect("5/=", "1");
    expect("0/=", "Error");
    expect("12C", "0");
    expect("1234567890123456789", "123456789012345");

    expect("9r", "3");                  /* square root, square, 1/x */
    expect("2r", "1.414213562");
    expect("9nr", "Error");
    expect("5s", "25");
    expect("0.5s", "0.25");
    expect("4i", "0.25");
    expect("0i", "Error");
    expect("0i7", "7");
    expect("16r+4=", "8");              /* a function result goes on in a sum */
    expect("2+9r=", "5");               /* ... or is the second number of one */
    expect("3s*2=", "18");
    expect("5M", "5");                  /* memory: M+ keeps the number on the display */
    expect("5MCR", "5");
    expect("5M3MCR", "8");
    expect("9M5NCR", "4");
    expect("5MQCR", "0");               /* MC forgets it */
    expect("5M7R", "5");                /* MR types the memory */
    expect("5M7+R=", "12");
    expect("5MC7R", "5");               /* C does not clear the memory */
    expect("5M2=R", "5");

    expect_expr("", "");
    expect_expr("2+3=", "2 + 3 =");
    expect_expr("2+3*", "2 + 3 x ");
    expect_expr("2+*3=", "2 x 3 =");
    expect_expr("5=", "5 =");
    expect_expr("5+=", "5 + 5 =");
    expect_expr("2*3=+4=", "6 + 4 =");
    expect_expr("2*3=7", "");           /* a new number: a new sum */
    expect_expr("9r", "sqrt(9) =");
    expect_expr("4i", "1/(4) =");
    expect_expr("2+9r", "2 + ");
    expect_expr("1/0=", "1 / ");
    expect_expr("12C", "");

    expect_paste("42", "42");
    expect_paste("  total: -3.5 EUR", "-3.5");
    expect_paste("1,25", "1.25");
    expect_paste(".5 and 7", "0.5");
    expect_paste("no digits", "(none)");
    expect_paste("1234567890123456789", "123456789012345");
    if (selftest_fail)
        return 1;
    puts("mellagui-calc selftest ok");
    return 0;
}

/* ---- UI --------------------------------------------------------------------------------- */

enum kind { K_DIGIT, K_OP, K_EQUALS, K_CLEAR, K_BACK, K_FUNC, K_MEM, K_UNARY };

struct keydef {
    char key;
    const char *label;
    int row, col, rspan;
    enum kind kind;
};

#define ROWS 7
#define COLS 4
#define PAD 8
#define DEFAULT_W 280
#define DEFAULT_H 440

static const struct keydef KEYS[] = {
    { 'Q', "MC", 0, 0, 1, K_MEM },   { 'R', "MR", 0, 1, 1, K_MEM },   { 'M', "M+", 0, 2, 1, K_MEM },  { 'N', "M-", 0, 3, 1, K_MEM },
    { 'r', "sqrt", 1, 0, 1, K_UNARY }, { 's', "x\xc2\xb2", 1, 1, 1, K_UNARY }, { 'i', "1/x", 1, 2, 1, K_UNARY }, { '/', "\xc3\xb7", 1, 3, 1, K_OP },
    { 'C', "C", 2, 0, 1, K_CLEAR },  { '<', "<-", 2, 1, 1, K_BACK },  { '%', "%", 2, 2, 1, K_FUNC },    { '*', "\xc3\x97", 2, 3, 1, K_OP },
    { '7', "7", 3, 0, 1, K_DIGIT },  { '8', "8", 3, 1, 1, K_DIGIT },  { '9', "9", 3, 2, 1, K_DIGIT },   { '-', "-", 3, 3, 1, K_OP },
    { '4', "4", 4, 0, 1, K_DIGIT },  { '5', "5", 4, 1, 1, K_DIGIT },  { '6', "6", 4, 2, 1, K_DIGIT },   { '+', "+", 4, 3, 1, K_OP },
    { '1', "1", 5, 0, 1, K_DIGIT },  { '2', "2", 5, 1, 1, K_DIGIT },  { '3', "3", 5, 2, 1, K_DIGIT },   { '=', "=", 5, 3, 2, K_EQUALS },
    { 'n', "+/-", 6, 0, 1, K_FUNC }, { '0', "0", 6, 1, 1, K_DIGIT },  { '.', ".", 6, 2, 1, K_DIGIT },
};
#define N_KEYS ((int)(sizeof(KEYS) / sizeof(KEYS[0])))

/* One colour for each sort of key, a lighter one under the pointer (the palette of the other round buttons). */
static void key_colors(enum kind k, uint32_t *base, uint32_t *hover)
{
    const uint32_t b[] = { lm_theme.calc_key, lm_theme.btn_blue, lm_theme.btn_green, lm_theme.btn_red, lm_theme.btn_amber, lm_theme.btn_slate, lm_theme.btn_teal, lm_theme.btn_purple };
    const uint32_t h[] = { lm_theme.calc_key_lit, lm_theme.btn_blue_lit, lm_theme.btn_green_lit, lm_theme.btn_red_lit, lm_theme.btn_amber_lit, lm_theme.btn_slate_lit, lm_theme.btn_teal_lit, lm_theme.btn_purple_lit };
    *base = b[k];
    *hover = h[k];
}

#define COL_BG      (lm_theme.background)
#define COL_DISPLAY (lm_theme.field)
#define COL_ACCENT  (lm_theme.accent)
#define COL_TEXT    (lm_theme.text)
#define COL_DIM     (lm_theme.text_dim)

struct app {
    struct wlwin win;
    struct wlclip clip;
    double px, py;
    bool has_pointer;
    int press;                  /* key index pressed with the pointer, -1 none */
    struct calc calc;
};

struct rect { int x, y, w, h; };

static struct rect display_rect(const struct app *a)
{
    int dh = LM_CLAMP(a->win.bh / 6, 56, 140);
    return (struct rect){ PAD, PAD, a->win.bw - 2 * PAD, dh };
}

static struct rect key_rect(const struct app *a, const struct keydef *k)
{
    struct rect d = display_rect(a);
    int top = d.y + d.h + PAD;
    int cw = (a->win.bw - 2 * PAD - (COLS - 1) * PAD) / COLS;
    int ch = (a->win.bh - top - PAD - (ROWS - 1) * PAD) / ROWS;
    return (struct rect){ PAD + k->col * (cw + PAD), top + k->row * (ch + PAD), cw, ch * k->rspan + (k->rspan - 1) * PAD };
}

static int key_at(const struct app *a, double x, double y)
{
    for (int i = 0; i < N_KEYS; i++) {
        struct rect r = key_rect(a, &KEYS[i]);
        if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h)
            return i;
    }
    return -1;
}

static void round_rect(struct app *a, struct rect r, int radius, uint32_t color)
{
    wlwin_round_fill(&a->win, r.x, r.y, r.w, r.h, radius, color);
}

static void draw_text_in(struct app *a, struct rect r, const char *s, int scale, uint32_t color, bool right)
{
    int tw = lm_font_text_width(s) * scale;
    int x = right ? r.x + r.w - tw - 12 : r.x + (r.w - tw) / 2;
    if (x < r.x)
        x = r.x;
    wlwin_text(&a->win, x, r.y + (r.h - LM_FONT_H * scale) / 2, s, scale, color, 0);
}

static void paint(struct wlwin *w)
{
    struct app *a = w->data;
    wlwin_fill(w, 0, 0, w->bw, w->bh, COL_BG);
    struct rect d = display_rect(a);
    round_rect(a, d, 12, COL_DISPLAY);

    char text[64];
    calc_display(&a->calc, text, sizeof(text));
    int line_h = LM_FONT_H + 6;                                          /* the sum so far runs along the top of the display */
    struct rect res = { d.x, d.y + line_h, d.w, d.h - line_h - 2 };
    int scale = res.h >= LM_FONT_H * 3 + 4 ? 3 : res.h >= LM_FONT_H * 2 ? 2 : 1;
    while (scale > 1 && lm_font_text_width(text) * scale > d.w - 24)
        scale--;
    if (a->calc.has_mem)
        wlwin_text(w, d.x + 12, d.y + 5, "M", 1, COL_ACCENT, 0);
    if (a->calc.expr[0] && !a->calc.error) {
        const char *e = a->calc.expr;
        int room = d.w - 24 - (a->calc.has_mem ? 20 : 0);
        while (*e && lm_font_text_width(e) > room)                       /* too long: show the end of it */
            e++;
        struct rect er = { d.x, d.y + 3, d.w, line_h };
        draw_text_in(a, er, e, 1, COL_DIM, true);
    }
    draw_text_in(a, res, text, scale, a->calc.error ? lm_theme.error : COL_TEXT, true);

    int hover = a->has_pointer ? key_at(a, a->px, a->py) : -1;
    for (int i = 0; i < N_KEYS; i++) {
        uint32_t base, lite;
        key_colors(KEYS[i].kind, &base, &lite);
        uint32_t bg = base;
        if (i == hover)
            bg = a->press == i ? lm_mix_rgb(base, 0x000000, 0.25) : lite;
        struct rect kr = key_rect(a, &KEYS[i]);
        round_rect(a, kr, lm_button_radius(LM_MIN(kr.w, kr.h) / 3), bg);
        int ls = kr.h >= 36 && kr.w >= 44 ? 2 : 1;                       /* as big as fits */
        if (lm_font_text_width(KEYS[i].label) * ls > kr.w - 6)
            ls = 1;
        draw_text_in(a, kr, KEYS[i].label, ls, lm_theme.on_colour, false);
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
        a->clip.serial = args[0].u;
        if (args[2].u != 272) /* BTN_LEFT */
            break;
        if (args[3].u == 1) {
            a->press = key_at(a, a->px, a->py);
        } else {
            int idx = key_at(a, a->px, a->py);
            if (idx >= 0 && idx == a->press)
                calc_press(&a->calc, KEYS[idx].key);
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
        a->clip.serial = args[0].u;
        if (args[3].u == 0)
            break;
        uint32_t ks = lm_keymap_keysym(args[2].u, a->win.mods);
        char txt[8];
        char key = 0;
        bool ctrl = (a->win.mods & LM_MOD_CTRL) != 0;
        if (ctrl && lm_keysym_to_utf8(ks, txt) == 1 && !(a->win.mods & (LM_MOD_ALT | LM_MOD_SUPER))) {
            if (txt[0] == 'c' || txt[0] == 'C') {                       /* Ctrl+C: the number on the display */
                char shown[64];
                calc_display(&a->calc, shown, sizeof(shown));
                if (!a->calc.error)
                    wlclip_copy(&a->clip, shown);
            } else if (txt[0] == 'v' || txt[0] == 'V') {                /* Ctrl+V: a number from the clipboard */
                size_t len = 0;
                char *t = wlclip_paste(&a->clip, &len);
                if (t) {
                    calc_paste(&a->calc, t);
                    free(t);
                }
            }
            a->win.redraw = true;
            break;
        }
        if (ks == LM_KS_Return || ks == LM_KS_KP_Enter)
            key = '=';
        else if (ks == LM_KS_BackSpace)
            key = '<';
        else if (ks == LM_KS_Escape || ks == LM_KS_Delete)
            key = 'C';
        else if (lm_keysym_to_utf8(ks, txt) == 1 && !(a->win.mods & (LM_MOD_ALT | LM_MOD_SUPER))) {
            if (strchr("0123456789.+-*/%=", txt[0]))
                key = txt[0];
            else if (txt[0] == 'c' || txt[0] == 'C')
                key = 'C';
            else if (txt[0] == ',')
                key = '.';
            else if (txt[0] == 'x' || txt[0] == 'X')
                key = '*';
            else if (txt[0] == 'r')
                key = 'r';
            else if (txt[0] == 'q')
                key = 's';
            else if (txt[0] == 'i')
                key = 'i';
            else if (txt[0] == 'n')
                key = 'n';
            else if (txt[0] == 'm')
                key = 'R';
            else if (txt[0] == 'p')
                key = 'M';
            else if (txt[0] == 'o')
                key = 'N';
            else if (txt[0] == 'z')
                key = 'Q';
        }
        if (key) {
            calc_press(&a->calc, key);
            a->win.redraw = true;
        }
        break;
    }
    }
}

static void global_event(struct wlwin *w, uint32_t name, const char *iface, uint32_t ver)
{
    struct app *a = w->data;
    wlclip_global(&a->clip, w, name, iface, ver);
}

int main(int argc, char **argv)
{
    wlwin_hidpi();                      /* draws through the wlwin_* calls only: sharp on a scaled screen */
    lm_theme_load_env();
    if (argc > 1 && !strcmp(argv[1], "--selftest"))
        return selftest();

    static const struct wlwin_ops ops = { .paint = paint, .global = global_event, .pointer = pointer_event, .keyboard = keyboard_event };
    struct app a = { .press = -1 };
    if (wlwin_connect(&a.win, "mellagui-calc", &ops, &a) < 0)
        return 1;
    if (wlwin_open(&a.win, "Calculator", "mellagui.calc", DEFAULT_W, DEFAULT_H, 220, 340) < 0)
        return 1;
    wlclip_attach(&a.clip, &a.win);
    while (wlwin_step(&a.win, -1))
        ;
    wlwin_destroy(&a.win);
    return 0;
}
