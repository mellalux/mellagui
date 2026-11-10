#include "wltext.h"

#include <stdlib.h>
#include <string.h>

#include "keymap.h"
#include "wlclip.h"

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

static void do_paste(struct wlclip *clip, char *buf, size_t max)
{
    size_t n = 0;
    char *t = wltext_paste_text(clip, &n);
    if (t) {
        wltext_append(buf, max, t, n);
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

bool wltext_key(struct wlclip *clip, char *buf, size_t max, uint32_t ks, uint32_t mods)
{
    int kind = clip_kind(ks, mods);
    if (!kind || !clip)
        return false;
    if (kind == 1) {
        do_paste(clip, buf, max);
    } else if (buf[0]) {
        wlclip_copy(clip, buf);
        if (kind == 3)
            buf[0] = 0;
    }
    return true;
}

const char *wltext_menu(const char *buf)
{
    return buf[0] ? "Cut\nCopy\nPaste\nClear" : "~Cut\n~Copy\nPaste\n~Clear";
}

void wltext_menu_do(struct wlclip *clip, char *buf, size_t max, int index)
{
    switch (index) {
    case 0:
    case 1:
        if (buf[0])
            wlclip_copy(clip, buf);
        if (index == 0)
            buf[0] = 0;
        break;
    case 2:
        do_paste(clip, buf, max);
        break;
    case 3:
        buf[0] = 0;
        break;
    }
}
