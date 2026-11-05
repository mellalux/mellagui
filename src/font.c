#include "font.h"

#include <stddef.h>
#include <string.h>

#include "font_data.h"

#define N_GLYPHS ((int)(sizeof(font_cps) / sizeof(font_cps[0])))

const uint8_t *lm_font_glyph(uint32_t cp)
{
    int lo = 0, hi = N_GLYPHS - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (font_cps[mid] == cp)
            return font_bits[mid];
        if (font_cps[mid] < cp)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    if (cp != 0xfffd)
        return lm_font_glyph(0xfffd);
    /* Font without U+FFFD: a visible hollow box. */
    static const uint8_t box[16] = { 0, 0, 0x7e, 0x42, 0x42, 0x42, 0x42, 0x42,
                                     0x42, 0x42, 0x42, 0x42, 0x7e, 0, 0, 0 };
    return box;
}

uint32_t lm_utf8_decode(const char *s, int len, int *used)
{
    const unsigned char *p = (const unsigned char *)s;
    if (len <= 0) {
        *used = 0;
        return 0xfffd;
    }
    *used = 1;
    if (p[0] < 0x80)
        return p[0];
    int need;
    uint32_t cp, min;
    if ((p[0] & 0xe0) == 0xc0) { need = 1; cp = p[0] & 0x1f; min = 0x80; }
    else if ((p[0] & 0xf0) == 0xe0) { need = 2; cp = p[0] & 0x0f; min = 0x800; }
    else if ((p[0] & 0xf8) == 0xf0) { need = 3; cp = p[0] & 0x07; min = 0x10000; }
    else return 0xfffd;
    if (len < need + 1)
        return 0xfffd;
    for (int i = 1; i <= need; i++) {
        if ((p[i] & 0xc0) != 0x80)
            return 0xfffd;
        cp = (cp << 6) | (p[i] & 0x3f);
    }
    *used = need + 1;
    if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
        return 0xfffd;
    return cp;
}

int lm_font_text_width(const char *utf8)
{
    int n = 0;
    size_t len = strlen(utf8);
    for (size_t i = 0; i < len;) {
        int used;
        lm_utf8_decode(utf8 + i, (int)(len - i), &used);
        i += used > 0 ? (size_t)used : 1;
        n++;
    }
    return n * LM_FONT_W;
}
