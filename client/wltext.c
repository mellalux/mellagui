#include "wltext.h"

#include <stdlib.h>
#include <string.h>

#include "font.h"
#include "util.h"
#include "keymap.h"
#include "wlclip.h"
#include "wlwin.h"

/* ---- UTF-8 ------------------------------------------------------------------------------------------------------------ */

static size_t prev_char(const char *s, size_t i)
{
    if (i == 0)
        return 0;
    i--;
    while (i > 0 && ((unsigned char)s[i] & 0xc0) == 0x80)
        i--;
    return i;
}

static size_t next_char(const char *s, size_t i, size_t len)
{
    if (i >= len)
        return len;
    i++;
    while (i < len && ((unsigned char)s[i] & 0xc0) == 0x80)
        i++;
    return i;
}

static bool is_word(unsigned char c)
{
    return c >= 0x80 || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static size_t word_left(const char *s, size_t i)
{
    while (i > 0 && !is_word((unsigned char)s[prev_char(s, i)]))
        i = prev_char(s, i);
    while (i > 0 && is_word((unsigned char)s[prev_char(s, i)]))
        i = prev_char(s, i);
    return i;
}

static size_t word_right(const char *s, size_t i, size_t len)
{
    while (i < len && !is_word((unsigned char)s[i]))
        i = next_char(s, i, len);
    while (i < len && is_word((unsigned char)s[i]))
        i = next_char(s, i, len);
    return i;
}

static size_t cp_count(const char *s, size_t bytes)
{
    size_t n = 0;
    for (size_t i = 0; i < bytes; i++)
        if (((unsigned char)s[i] & 0xc0) != 0x80)
            n++;
    return n;
}

/* The byte offset of code point number `cp` (the end of the text when it has fewer). */
static size_t byte_of_cp(const char *s, size_t cp)
{
    size_t i = 0, len = strlen(s);
    while (cp > 0 && i < len) {
        i = next_char(s, i, len);
        cp--;
    }
    return i;
}

/* ---- the state of the fields (kept by the address of their buffer) ------------------------------------------------------ */

struct state {
    const char *buf;
    size_t cur, anchor;                 /* bytes: the caret and the other end of the selection (== cur: none) */
    size_t len;                         /* strlen when it was last looked at: another length = the program set the text, the caret goes to the end */
    size_t off;                         /* the first code point shown when the text is wider than the field */
};

#define STATES 24
static struct state states[STATES];
static int next_slot;

static struct state *get(const char *buf)
{
    struct state *s = NULL;
    for (int i = 0; i < STATES; i++)
        if (states[i].buf == buf)
            s = &states[i];
    size_t len = strlen(buf);
    if (!s) {
        s = &states[next_slot];
        next_slot = (next_slot + 1) % STATES;
        memset(s, 0, sizeof(*s));
        s->buf = buf;
        s->cur = s->anchor = s->len = len;
    }
    if (s->len != len) {
        s->cur = s->anchor = s->len = len;
    }
    if (s->cur > len)
        s->cur = len;
    if (s->anchor > len)
        s->anchor = len;
    return s;
}

static bool sel_range(const struct state *s, size_t *a, size_t *b)
{
    if (s->cur == s->anchor)
        return false;
    *a = s->cur < s->anchor ? s->cur : s->anchor;
    *b = s->cur < s->anchor ? s->anchor : s->cur;
    return true;
}

static void delete_range(struct state *s, char *buf, size_t a, size_t b)
{
    memmove(buf + a, buf + b, strlen(buf) - b + 1);
    s->cur = s->anchor = a;
    s->len = strlen(buf);
}

static void insert_text(struct state *s, char *buf, size_t max, const char *text, size_t n)
{
    size_t a, b;
    if (sel_range(s, &a, &b))
        delete_range(s, buf, a, b);
    size_t len = strlen(buf);
    if (len + n + 1 > max) {                                            /* what does not fit is left out, never in the middle of a character */
        if (len + 1 >= max)
            return;
        n = max - len - 1;
        while (n > 0 && ((unsigned char)text[n] & 0xc0) == 0x80)
            n--;
    }
    if (!n)
        return;
    memmove(buf + s->cur + n, buf + s->cur, len - s->cur + 1);
    memcpy(buf + s->cur, text, n);
    s->cur += n;
    s->anchor = s->cur;
    s->len = len + n;
}

void wltext_append(char *buf, size_t max, const char *text, size_t n)
{
    size_t len = strlen(buf);
    if (len + 1 >= max)
        return;
    if (len + n + 1 > max) {
        n = max - len - 1;
        while (n > 0 && ((unsigned char)text[n] & 0xc0) == 0x80)      /* not in the middle of a character */
            n--;
    }
    memcpy(buf + len, text, n);
    buf[len + n] = 0;
}

/* ---- the clipboard ------------------------------------------------------------------------------------------------------ */

char *wltext_paste_text(struct wlclip *clip, size_t *len)
{
    size_t n = 0;
    char *t = wlclip_paste(clip, &n);
    if (!t)
        return NULL;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)t[i];
        if (c == '\r' || c == '\n' || c == '\t')
            t[o++] = ' ';
        else if (c >= 0x20 && c != 0x7f)
            t[o++] = (char)c;
    }
    t[o] = 0;
    while (o > 0 && t[o - 1] == ' ')                                    /* a trailing line end is not wanted in a field */
        t[--o] = 0;
    if (!o) {
        free(t);
        return NULL;
    }
    *len = o;
    return t;
}

/* The selection, or the whole text when nothing is selected (a field has little in it). */
static void copy_out(struct state *s, struct wlclip *clip, const char *buf)
{
    size_t a, b;
    if (!buf[0] || !clip)
        return;
    if (!sel_range(s, &a, &b)) {
        a = 0;
        b = strlen(buf);
    }
    char *t = malloc(b - a + 1);
    if (!t)
        return;
    memcpy(t, buf + a, b - a);
    t[b - a] = 0;
    wlclip_copy(clip, t);
    free(t);
}

static void do_paste(struct state *s, struct wlclip *clip, char *buf, size_t max)
{
    size_t n = 0;
    char *t = clip ? wltext_paste_text(clip, &n) : NULL;
    if (t) {
        insert_text(s, buf, max, t, n);
        free(t);
    }
}

/* 0 not a clipboard key, 1 paste, 2 copy, 3 cut. */
static int clip_kind(uint32_t ks, uint32_t mods)
{
    bool ctrl = (mods & LM_MOD_CTRL) != 0, shift = (mods & LM_MOD_SHIFT) != 0, alt = (mods & (LM_MOD_ALT | LM_MOD_SUPER)) != 0;
    if (alt)
        return 0;
    if ((ctrl && (ks == 'v' || ks == 'V')) || (shift && !ctrl && ks == LM_KS_Insert))
        return 1;
    if ((ctrl && (ks == 'c' || ks == 'C')) || (ctrl && !shift && ks == LM_KS_Insert))
        return 2;
    if ((ctrl && !shift && (ks == 'x' || ks == 'X')) || (shift && !ctrl && ks == LM_KS_Delete))
        return 3;
    return 0;
}

bool wltext_is_clipkey(uint32_t ks, uint32_t mods)
{
    return clip_kind(ks, mods) != 0;
}

/* ---- editing ------------------------------------------------------------------------------------------------------------ */

bool wltext_edit(struct wlclip *clip, char *buf, size_t max, uint32_t ks, uint32_t mods)
{
    bool ctrl = (mods & LM_MOD_CTRL) != 0, shift = (mods & LM_MOD_SHIFT) != 0, alt = (mods & (LM_MOD_ALT | LM_MOD_SUPER)) != 0;
    if (alt)
        return false;
    struct state *s = get(buf);
    size_t len = strlen(buf), a, b;
    int kind = clip_kind(ks, mods);
    if (kind == 1) {
        do_paste(s, clip, buf, max);
        return true;
    }
    if (kind == 2 || kind == 3) {
        copy_out(s, clip, buf);
        if (kind == 3 && buf[0]) {
            if (sel_range(s, &a, &b))
                delete_range(s, buf, a, b);
            else {
                buf[0] = 0;
                s->cur = s->anchor = s->len = 0;
            }
        }
        return true;
    }
    if (ctrl && (ks == 'a' || ks == 'A')) {                              /* select all */
        s->anchor = 0;
        s->cur = len;
        return true;
    }
    size_t to = s->cur;
    if (ks == LM_KS_Left) {
        if (!shift && sel_range(s, &a, &b))
            to = a;
        else
            to = ctrl ? word_left(buf, s->cur) : prev_char(buf, s->cur);
    } else if (ks == LM_KS_Right) {
        if (!shift && sel_range(s, &a, &b))
            to = b;
        else
            to = ctrl ? word_right(buf, s->cur, len) : next_char(buf, s->cur, len);
    } else if (ks == LM_KS_Home) {
        to = 0;
    } else if (ks == LM_KS_End) {
        to = len;
    } else if (ks == LM_KS_BackSpace || ks == LM_KS_Delete) {
        if (sel_range(s, &a, &b)) {
            delete_range(s, buf, a, b);
        } else if (ks == LM_KS_BackSpace && s->cur > 0) {
            delete_range(s, buf, ctrl ? word_left(buf, s->cur) : prev_char(buf, s->cur), s->cur);
        } else if (ks == LM_KS_Delete && s->cur < len) {
            delete_range(s, buf, s->cur, ctrl ? word_right(buf, s->cur, len) : next_char(buf, s->cur, len));
        }
        return true;
    } else {
        char txt[8];
        int tl = ctrl ? 0 : lm_keysym_to_utf8(ks, txt);
        if (tl > 0 && (unsigned char)txt[0] >= 0x20 && txt[0] != 0x7f) {
            insert_text(s, buf, max, txt, (size_t)tl);
            return true;
        }
        return false;
    }
    s->cur = to;
    if (!shift)
        s->anchor = to;
    return true;
}

void wltext_end(char *buf)
{
    struct state *s = get(buf);
    s->cur = s->anchor = strlen(buf);
}

bool wltext_selection(const char *buf, size_t *a, size_t *b)
{
    return sel_range(get(buf), a, b);
}

size_t wltext_cursor(const char *buf)
{
    return get(buf)->cur;
}

void wltext_select_all(char *buf)
{
    struct state *s = get(buf);
    s->anchor = 0;
    s->cur = strlen(buf);
}

/* Pixels of the label buf[from..to) (bytes). */
static int span_px(const char *buf, size_t from, size_t to)
{
    int px = 0;
    for (size_t i = from; i < to;) {
        int used;
        uint32_t cp = lm_utf8_decode(buf + i, (int)(to - i), &used);
        px += lm_font_advance(cp);
        i += used > 0 ? (size_t)used : 1;
    }
    return px;
}

void wltext_click(char *buf, int rel_px, bool extend)
{
    struct state *s = get(buf);
    size_t len = strlen(buf), pos = byte_of_cp(buf, s->off);
    int x = 0;
    while (pos < len) {                                                 /* the gap between two letters that is nearest */
        int used;
        uint32_t cp = lm_utf8_decode(buf + pos, (int)(len - pos), &used);
        int adv = lm_font_advance(cp);
        if (rel_px < x + (adv + 1) / 2)
            break;
        x += adv;
        pos += used > 0 ? (size_t)used : 1;
    }
    s->cur = pos;
    if (!extend)
        s->anchor = pos;
}

/* ---- painting ----------------------------------------------------------------------------------------------------------- */

/* The field is a label: every letter as wide as it is (LM_FONT_W each while labels are cells, which gives the old
 * columns). `off` (the first letter shown, a code point number) moves so that the caret stays inside. */
void wltext_paint(struct wlwin *w, int x, int y, int maxw, const char *buf, bool focus, uint32_t fg, uint32_t selbg, uint32_t caret)
{
    struct state *s = get(buf);
    int room = maxw - maxw % LM_FONT_W;
    if (room < 2 * LM_FONT_W)
        return;
    size_t len = strlen(buf), cur = LM_MIN(s->cur, len);
    size_t cur_cp = cp_count(buf, cur);
    if (cur_cp < s->off)                                                /* the caret stays in view */
        s->off = cur_cp;
    size_t first = byte_of_cp(buf, s->off);
    while (first < cur && span_px(buf, first, cur) > room - 2 * LM_FONT_W) {     /* ... two cells before the right end */
        first = byte_of_cp(buf, ++s->off);
    }
    if (span_px(buf, 0, len) <= room - LM_FONT_W) {
        s->off = 0;
        first = 0;
    }
    size_t last = first + (size_t)lm_font_fit(buf + first, room);
    size_t a, b;
    if (focus && sel_range(s, &a, &b)) {                                /* the selection behind the text */
        a = LM_MAX(a, first);
        b = LM_MIN(b, last);
        if (b > a)
            wlwin_fill(w, x + span_px(buf, first, a), y, span_px(buf, a, b), LM_FONT_H, selbg);
    }
    char *shown = malloc(last - first + 1);
    if (!shown)
        return;
    memcpy(shown, buf + first, last - first);
    shown[last - first] = 0;
    wlwin_text(w, x, y, shown, 1, fg, 0);
    free(shown);
    if (focus)
        wlwin_fill(w, x + span_px(buf, first, cur), y, 2, LM_FONT_H, caret);
}

/* ---- the right-click menu ------------------------------------------------------------------------------------------------ */

const char *wltext_menu(const char *buf)
{
    return buf[0] ? "Cut\nCopy\nPaste\nSelect all\nClear" : "~Cut\n~Copy\nPaste\n~Select all\n~Clear";
}

void wltext_menu_do(struct wlclip *clip, char *buf, size_t max, int index)
{
    struct state *s = get(buf);
    switch (index) {
    case 0:
    case 1:
        copy_out(s, clip, buf);
        if (index == 0 && buf[0]) {
            size_t a, b;
            if (sel_range(s, &a, &b))
                delete_range(s, buf, a, b);
            else {
                buf[0] = 0;
                s->cur = s->anchor = s->len = 0;
            }
        }
        break;
    case 2:
        do_paste(s, clip, buf, max);
        break;
    case 3:
        wltext_select_all(buf);
        break;
    case 4:
        buf[0] = 0;
        s->cur = s->anchor = s->len = 0;
        break;
    }
}

bool wltext_key(struct wlclip *clip, char *buf, size_t max, uint32_t ks, uint32_t mods)
{
    if (!clip_kind(ks, mods))
        return false;
    return wltext_edit(clip, buf, max, ks, mods);
}
