#include "osk.h"

#include <stdio.h>
#include <string.h>
#include <linux/input-event-codes.h>

#include "keymap.h"
#include "util.h"

struct spec { uint32_t code; int q; enum lm_osk_kind kind; };       /* q = width in quarters of a key unit */

#define K(c, w) { KEY_##c, w, LM_OSK_KEY }
#define M(c, w, k) { KEY_##c, w, k }

static const struct spec row0[] = {
    K(GRAVE, 4), K(1, 4), K(2, 4), K(3, 4), K(4, 4), K(5, 4), K(6, 4), K(7, 4), K(8, 4), K(9, 4), K(0, 4), K(MINUS, 4), K(EQUAL, 4),
    K(BACKSPACE, 8),
};
static const struct spec row1[] = {
    K(TAB, 6), K(Q, 4), K(W, 4), K(E, 4), K(R, 4), K(T, 4), K(Y, 4), K(U, 4), K(I, 4), K(O, 4), K(P, 4), K(LEFTBRACE, 4),
    K(RIGHTBRACE, 4), K(BACKSLASH, 6),
};
static const struct spec row2[] = {
    M(CAPSLOCK, 7, LM_OSK_CAPS), K(A, 4), K(S, 4), K(D, 4), K(F, 4), K(G, 4), K(H, 4), K(J, 4), K(K, 4), K(L, 4), K(SEMICOLON, 4),
    K(APOSTROPHE, 4), K(ENTER, 9),
};
static const struct spec row3[] = {
    M(LEFTSHIFT, 5, LM_OSK_SHIFT), K(102ND, 4), K(Z, 4), K(X, 4), K(C, 4), K(V, 4), K(B, 4), K(N, 4), K(M, 4), K(COMMA, 4), K(DOT, 4),
    K(SLASH, 4), M(RIGHTSHIFT, 11, LM_OSK_SHIFT),
};
static const struct spec row4[] = {
    M(RESERVED, 5, LM_OSK_HIDE), K(ESC, 5), M(LEFTCTRL, 5, LM_OSK_CTRL), M(LEFTALT, 5, LM_OSK_ALT), K(SPACE, 19),
    M(RIGHTALT, 5, LM_OSK_ALTGR), K(LEFT, 4), K(UP, 4), K(DOWN, 4), K(RIGHT, 4),
};

static const struct { const struct spec *s; int n; } rows[5] = {
    { row0, (int)(sizeof(row0) / sizeof(row0[0])) }, { row1, (int)(sizeof(row1) / sizeof(row1[0])) },
    { row2, (int)(sizeof(row2) / sizeof(row2[0])) }, { row3, (int)(sizeof(row3) / sizeof(row3[0])) },
    { row4, (int)(sizeof(row4) / sizeof(row4[0])) },
};

#define PAD 8
#define GAP 4
#define ROW_QUARTERS 60

void lm_osk_geometry(const struct lm_rect *out, struct lm_osk_geo *g)
{
    memset(g, 0, sizeof(*g));
    int w = LM_MIN(out->w - 8, 1180);
    if (w < 15 * 16 + 2 * PAD)
        w = LM_MAX(out->w, 15 * 16 + 2 * PAD);
    int inner = w - 2 * PAD;
    int kh = LM_CLAMP(inner / 15 * 65 / 100, 30, 56);
    int h = 5 * kh + 4 * GAP + 2 * PAD;
    g->card = lm_rect_make(out->x + (out->w - w) / 2, out->y + out->h - h - 6, w, h);
    for (int r = 0; r < 5; r++) {
        int qx = 0, y = g->card.y + PAD + r * (kh + GAP);
        for (int i = 0; i < rows[r].n && g->n < LM_OSK_MAX_KEYS; i++) {
            const struct spec *s = &rows[r].s[i];
            int x0 = g->card.x + PAD + inner * qx / ROW_QUARTERS;
            int x1 = g->card.x + PAD + inner * (qx + s->q) / ROW_QUARTERS;
            struct lm_osk_key *k = &g->key[g->n++];
            k->code = s->code;
            k->row = r;
            k->kind = s->kind;
            k->rect = lm_rect_make(x0 + GAP / 2, y, LM_MAX(x1 - x0 - GAP, 1), kh);
            qx += s->q;
        }
    }
}

uint32_t lm_osk_kind_mod(enum lm_osk_kind kind)
{
    switch (kind) {
    case LM_OSK_SHIFT: return LM_MOD_SHIFT;
    case LM_OSK_CTRL: return LM_MOD_CTRL;
    case LM_OSK_ALT: return LM_MOD_ALT;
    case LM_OSK_ALTGR: return LM_MOD_ALTGR;
    default: return 0;
    }
}

uint32_t lm_osk_kind_code(enum lm_osk_kind kind)
{
    switch (kind) {
    case LM_OSK_SHIFT: return KEY_LEFTSHIFT;
    case LM_OSK_CTRL: return KEY_LEFTCTRL;
    case LM_OSK_ALT: return KEY_LEFTALT;
    case LM_OSK_ALTGR: return KEY_RIGHTALT;
    case LM_OSK_CAPS: return KEY_CAPSLOCK;
    default: return 0;
    }
}

void lm_osk_label(const struct lm_osk_key *k, uint32_t mods, uint32_t locks, char out[LM_OSK_LABEL * 4])
{
    out[0] = 0;
    const char *name = NULL;
    switch (k->kind) {
    case LM_OSK_SHIFT: name = "Shift"; break;
    case LM_OSK_CTRL: name = "Ctrl"; break;
    case LM_OSK_ALT: name = "Alt"; break;
    case LM_OSK_ALTGR: name = "AltGr"; break;
    case LM_OSK_CAPS: name = "Caps"; break;
    case LM_OSK_HIDE: name = "Hide"; break;
    default: break;
    }
    if (!name)
        switch (k->code) {
        case KEY_TAB: name = "Tab"; break;
        case KEY_BACKSPACE: name = "Bksp"; break;
        case KEY_ENTER: name = "Enter"; break;
        case KEY_ESC: name = "Esc"; break;
        case KEY_SPACE: name = ""; break;
        case KEY_LEFT: name = "<"; break;
        case KEY_RIGHT: name = ">"; break;
        case KEY_UP: name = "^"; break;
        case KEY_DOWN: name = "v"; break;
        default: break;
        }
    if (name) {
        snprintf(out, LM_OSK_LABEL * 4, "%s", name);
        return;
    }
    uint32_t m = mods & (LM_MOD_SHIFT | LM_MOD_ALTGR);
    if (locks & LM_MOD_CAPS)
        m |= LM_MOD_CAPS;
    uint32_t ks = lm_keymap_keysym(k->code, m);
    char txt[8];
    int n = ks ? lm_keysym_to_utf8(ks, txt) : 0;
    if (n > 0 && (unsigned char)txt[0] >= 0x20) {
        memcpy(out, txt, (size_t)n);
        out[n] = 0;
    }
}
