#include "fontread.h"

#include "../src/img.h"
#include "../src/ttf.h"

#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define FONT_MAX (48u << 20)
#define SAMPLE_W 880                            /* the sample is made twice as big as it is shown, and scaled down (crisper small text) */

static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static void seterr(char *err, size_t n, const char *s)
{
    if (err && n)
        snprintf(err, n, "%s", s);
}

void font_free(struct font_info *fi)
{
    free(fi->alpha);
    memset(fi, 0, sizeof(*fi));
}

/* ---- WOFF 1 -> an ordinary font file in memory ---- */
static uint8_t *woff_to_sfnt(const uint8_t *in, size_t n, size_t *outn)
{
    if (n < 44)
        return NULL;
    uint32_t flavor = be32(in + 4), nt = be16(in + 12);
    if (nt == 0 || nt > 64 || 44 + (size_t)nt * 20 > n)
        return NULL;
    size_t total = 12 + (size_t)nt * 16;
    for (uint32_t i = 0; i < nt; i++) {
        uint32_t olen = be32(in + 44 + i * 20 + 12);
        if (olen > FONT_MAX)
            return NULL;
        total += (olen + 3u) & ~3u;
        if (total > FONT_MAX)
            return NULL;
    }
    uint8_t *out = calloc(1, total);
    if (!out)
        return NULL;
    uint32_t pow2 = 1, lg = 0;
    while (pow2 * 2 <= nt) {
        pow2 *= 2;
        lg++;
    }
    out[0] = (uint8_t)(flavor >> 24); out[1] = (uint8_t)(flavor >> 16); out[2] = (uint8_t)(flavor >> 8); out[3] = (uint8_t)flavor;
    out[4] = (uint8_t)(nt >> 8); out[5] = (uint8_t)nt;
    uint32_t sr = pow2 * 16, rs = nt * 16 - sr;
    out[6] = (uint8_t)(sr >> 8); out[7] = (uint8_t)sr; out[8] = (uint8_t)(lg >> 8); out[9] = (uint8_t)lg; out[10] = (uint8_t)(rs >> 8); out[11] = (uint8_t)rs;
    size_t pos = 12 + (size_t)nt * 16;
    for (uint32_t i = 0; i < nt; i++) {
        const uint8_t *e = in + 44 + i * 20;
        uint32_t off = be32(e + 4), clen = be32(e + 8), olen = be32(e + 12);
        if (off > n || clen > n - off) {
            free(out);
            return NULL;
        }
        if (clen == olen) {
            memcpy(out + pos, in + off, olen);
        } else if (clen < 3 || clen > olen || lm_inflate_raw(in + off + 2, clen - 2, out + pos, olen) != 0) {          /* a zlib stream: its 2 byte header, then deflate */
            free(out);
            return NULL;
        }
        uint8_t *d = out + 12 + (size_t)i * 16;
        memcpy(d, e, 4);                                                /* tag */
        memcpy(d + 4, e + 16, 4);                                       /* checksum */
        d[8] = (uint8_t)(pos >> 24); d[9] = (uint8_t)(pos >> 16); d[10] = (uint8_t)(pos >> 8); d[11] = (uint8_t)pos;
        memcpy(d + 12, e + 12, 4);                                      /* length */
        pos += (olen + 3u) & ~3u;
    }
    *outn = total;
    return out;
}

/* ---- tables ---- */
static bool sfnt_table(const uint8_t *d, size_t n, uint32_t base, const char *tag, size_t *off, size_t *len)
{
    if (base + 12 > n)
        return false;
    uint32_t nt = be16(d + base + 4);
    for (uint32_t i = 0; i < nt; i++) {
        size_t e = base + 12 + (size_t)i * 16;
        if (e + 16 > n)
            return false;
        if (!memcmp(d + e, tag, 4)) {
            uint32_t o = be32(d + e + 8), l = be32(d + e + 12);
            if (o > n || l > n - o)
                return false;
            *off = o;
            *len = l;
            return true;
        }
    }
    return false;
}

static void put_utf8(char *o, size_t *w, size_t cap, unsigned cp)
{
    if (cp < 0x80 && *w + 1 < cap) {
        o[(*w)++] = (char)(cp < 0x20 ? ' ' : cp);
    } else if (cp < 0x800 && *w + 2 < cap) {
        o[(*w)++] = (char)(0xc0 | cp >> 6);
        o[(*w)++] = (char)(0x80 | (cp & 0x3f));
    } else if (cp >= 0x800 && *w + 3 < cap && cp < 0x10000) {
        o[(*w)++] = (char)(0xe0 | cp >> 12);
        o[(*w)++] = (char)(0x80 | ((cp >> 6) & 0x3f));
        o[(*w)++] = (char)(0x80 | (cp & 0x3f));
    } else if (cp >= 0x10000 && *w + 4 < cap) {
        o[(*w)++] = (char)(0xf0 | cp >> 18);
        o[(*w)++] = (char)(0x80 | ((cp >> 12) & 0x3f));
        o[(*w)++] = (char)(0x80 | ((cp >> 6) & 0x3f));
        o[(*w)++] = (char)(0x80 | (cp & 0x3f));
    }
}

/* One string of the `name` table: Windows English first, then any Windows, Unicode, Mac. */
static void name_string(const uint8_t *nt, size_t nlen, int id, char *o, size_t cap)
{
    o[0] = 0;
    if (nlen < 6)
        return;
    uint32_t count = be16(nt + 2), soff = be16(nt + 4);
    int best = -1, bestscore = -1;
    for (uint32_t i = 0; i < count && 6 + (size_t)(i + 1) * 12 <= nlen; i++) {
        const uint8_t *r = nt + 6 + (size_t)i * 12;
        if ((int)be16(r + 6) != id)
            continue;
        uint32_t plat = be16(r), lang = be16(r + 4);
        int score = plat == 3 && lang == 0x409 ? 4 : plat == 3 ? 3 : plat == 0 ? 2 : plat == 1 ? 1 : 0;
        if (score > bestscore && (size_t)soff + be16(r + 10) + be16(r + 8) <= nlen) {
            best = (int)i;
            bestscore = score;
        }
    }
    if (best < 0)
        return;
    const uint8_t *r = nt + 6 + (size_t)best * 12;
    uint32_t plat = be16(r), len = be16(r + 8), off = soff + be16(r + 10);
    size_t w = 0;
    if (plat == 3 || plat == 0) {                                       /* UTF-16BE */
        for (uint32_t i = 0; i + 1 < len; i += 2) {
            unsigned cp = be16(nt + off + i);
            if (cp >= 0xd800 && cp < 0xdc00 && i + 3 < len) {
                unsigned lo = be16(nt + off + i + 2);
                if (lo >= 0xdc00 && lo < 0xe000) {
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                    i += 2;
                }
            }
            put_utf8(o, &w, cap, cp);
        }
    } else {                                                            /* Mac Roman: the plain letters are right, the rest is shown as it is */
        for (uint32_t i = 0; i < len; i++)
            put_utf8(o, &w, cap, nt[off + i] < 0x80 ? nt[off + i] : '?');
    }
    o[w] = 0;
    while (w > 0 && o[w - 1] == ' ')
        o[--w] = 0;
}

/* ---- the sample ---- */
struct probe {
    const char *name;
    uint32_t cps[4];
};

static const struct probe k_scripts[] = {
    { "Latin", { 'A', 'z', 0xe9, 0 } }, { "Latin Extended", { 0x100, 0x141, 0x15f, 0 } }, { "Estonian", { 0xd5, 0xf5, 0x160, 0x17e } },
    { "Cyrillic", { 0x416, 0x44f, 0, 0 } }, { "Greek", { 0x3a9, 0x3b1, 0, 0 } }, { "Hebrew", { 0x5d0, 0, 0, 0 } }, { "Arabic", { 0x639, 0, 0, 0 } },
    { "Devanagari", { 0x905, 0, 0, 0 } }, { "Thai", { 0xe01, 0, 0, 0 } }, { "Hiragana", { 0x3042, 0, 0, 0 } }, { "Hangul", { 0xd55c, 0, 0, 0 } },
    { "CJK", { 0x4e2d, 0, 0, 0 } }, { "Symbols", { 0x20ac, 0x2192, 0x2260, 0 } }, { "Box drawing", { 0x2500, 0x2588, 0, 0 } }, { "Emoji", { 0x1f600, 0, 0, 0 } },
};

static void scripts_text(const struct lm_ttf *f, char *o, size_t cap)
{
    size_t w = 0;
    o[0] = 0;
    for (size_t i = 0; i < sizeof(k_scripts) / sizeof(k_scripts[0]); i++) {
        bool all = true;
        for (int k = 0; k < 4 && k_scripts[i].cps[k]; k++)
            if (!lm_ttf_glyph(f, k_scripts[i].cps[k]))
                all = false;
        if (all && w + strlen(k_scripts[i].name) + 3 < cap)
            w += (size_t)snprintf(o + w, cap - w, "%s%s", w ? ", " : "", k_scripts[i].name);
    }
}

/* Draws `text` (UTF-8) at pixel size `px` into the coverage picture, the baseline at `base`; shrinks to fit the width. Returns the height used. */
static void draw_line(struct lm_ttf *f, uint8_t *pic, int w, int h, uint8_t *scratch, const char *text, double px, int *y)
{
    int units = lm_ttf_units(f);
    double sc = px / units;
    uint32_t cps[200];
    int n = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p && n < 199;) {
        uint32_t cp = *p < 0x80 ? *p : (*p >> 5) == 6 ? (uint32_t)(*p & 0x1f) << 6 | (p[1] & 0x3f) : (*p >> 4) == 14 ? (uint32_t)(*p & 0xf) << 12 | (uint32_t)(p[1] & 0x3f) << 6 | (p[2] & 0x3f)
                       : (uint32_t)(*p & 7) << 18 | (uint32_t)(p[1] & 0x3f) << 12 | (uint32_t)(p[2] & 0x3f) << 6 | (p[3] & 0x3f);
        p += *p < 0x80 ? 1 : (*p >> 5) == 6 ? 2 : (*p >> 4) == 14 ? 3 : 4;
        cps[n++] = cp;
    }
    double width = 0;
    for (int i = 0; i < n; i++) {
        int g = lm_ttf_glyph(f, cps[i]);
        width += g ? lm_ttf_advance(f, g) * sc : units * sc * 0.5;
    }
    if (width > w - 24) {
        sc *= (w - 24) / width;
        px *= (w - 24) / width;
    }
    int asc = (int)(lm_ttf_ascent(f) * sc + 1), desc = (int)(-lm_ttf_descent(f) * sc + 1);
    int base = *y + asc + 4;
    if (base + desc + 4 > h)
        return;
    double pen = 12;
    for (int i = 0; i < n; i++) {
        int g = lm_ttf_glyph(f, cps[i]);
        if (!g) {
            pen += units * sc * 0.5;
            continue;
        }
        memset(scratch, 0, (size_t)w * (size_t)h);
        if (lm_ttf_render(f, g, sc, sc, pen, base, scratch, w, h))
            for (size_t k = (size_t)(base - asc - 4 > 0 ? base - asc - 4 : 0) * (size_t)w; k < (size_t)(base + desc + 4 < h ? base + desc + 4 : h) * (size_t)w; k++)
                if (scratch[k] > pic[k])
                    pic[k] = scratch[k];
        pen += lm_ttf_advance(f, g) * sc;
    }
    *y = base + desc + (int)(px * 0.25) + 2;
}

static uint8_t *make_sample(struct lm_ttf *f, int *wout, int *hout)
{
    static const struct { const char *text; double px; } lines[] = {
        { "Aa Bb Cc Dd 0123", 120 }, { "ABCDEFGHIJKLMNOPQRSTUVWXYZ", 64 }, { "abcdefghijklmnopqrstuvwxyz", 64 }, { "0123456789 !?&@#%()[]{}<>", 56 },
        { "\xc3\x95\xc3\xb5 \xc3\x84\xc3\xa4 \xc3\x96\xc3\xb6 \xc3\x9c\xc3\xbc \xc5\xa0\xc5\xa1 \xc5\xbd\xc5\xbe \xe2\x82\xac \xe2\x86\x92 \xe2\x89\xa0", 56 },
        { "The quick brown fox jumps over the lazy dog.", 40 }, { "P\xc3\xb5" "dur Zagrebi t\xc5\xa1" "ellom\xc3\xa4ngija-f\xc3\xb6ljetonist Ciqo k\xc3\xbclmetas.", 36 },
        { "Sphinx of black quartz, judge my vow. 1234567890", 28 },
    };
    int w = SAMPLE_W, h = 0;
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++)
        h += (int)(lines[i].px * 1.45) + 8;
    uint8_t *pic = calloc((size_t)w, (size_t)h), *scratch = malloc((size_t)w * (size_t)h);
    if (!pic || !scratch) {
        free(pic);
        free(scratch);
        return NULL;
    }
    int y = 8;
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++)
        draw_line(f, pic, w, h, scratch, lines[i].text, lines[i].px, &y);
    free(scratch);
    *wout = w;
    *hout = y + 8 < h ? y + 8 : h;
    return pic;
}

int font_read(const char *path, struct font_info *fi, char *err, size_t errn)
{
    memset(fi, 0, sizeof(*fi));
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 44 || (uint64_t)st.st_size > FONT_MAX) {
        close(fd);
        return -1;
    }
    uint8_t *d = malloc((size_t)st.st_size);
    size_t n = (size_t)st.st_size;
    ssize_t got = d ? read(fd, d, n) : -1;
    close(fd);
    if (got != (ssize_t)n) {
        free(d);
        return -1;
    }
    uint32_t magic = be32(d);
    if (magic == 0x774f4632) {                                          /* "wOF2" */
        free(d);
        seterr(err, errn, "A WOFF 2 font (Brotli and a transformed glyph table): not read.");
        return -2;
    }
    if (magic == 0x774f4646) {                                          /* "wOFF" */
        size_t sn = 0;
        uint8_t *s = woff_to_sfnt(d, n, &sn);
        free(d);
        if (!s) {
            seterr(err, errn, "A damaged WOFF font.");
            return -2;
        }
        d = s;
        n = sn;
        fi->woff = 1;
        magic = be32(d);
    }
    if (magic != 0x00010000 && magic != 0x74727565 && magic != 0x4f54544f && magic != 0x74746366) {
        free(d);
        return -1;
    }
    uint32_t base = magic == 0x74746366 && n >= 16 ? be32(d + 12) : 0;
    fi->cff = be32(d + base) == 0x4f54544f;
    size_t no, nl, ho, hl, mo, ml, oo, ol, po, pl;
    if (!sfnt_table(d, n, base, "head", &ho, &hl) || !sfnt_table(d, n, base, "maxp", &mo, &ml) || hl < 54 || ml < 6) {
        free(d);
        seterr(err, errn, "A font file without its basic tables.");
        return -2;
    }
    fi->units = (int)be16(d + ho + 18);
    fi->glyphs = (int)be16(d + mo + 4);
    fi->italic = (be16(d + ho + 44) & 2) != 0;
    if (sfnt_table(d, n, base, "OS/2", &oo, &ol) && ol >= 64) {
        fi->weight = (int)be16(d + oo + 4);
        if (be16(d + oo + 62) & 1)
            fi->italic = 1;
    }
    if (sfnt_table(d, n, base, "post", &po, &pl) && pl >= 16)
        fi->monospace = be32(d + po + 12) != 0;
    if (sfnt_table(d, n, base, "name", &no, &nl)) {
        const uint8_t *nt = d + no;
        name_string(nt, nl, 1, fi->family, sizeof(fi->family));
        name_string(nt, nl, 2, fi->style, sizeof(fi->style));
        name_string(nt, nl, 4, fi->full, sizeof(fi->full));
        name_string(nt, nl, 5, fi->version, sizeof(fi->version));
        name_string(nt, nl, 6, fi->psname, sizeof(fi->psname));
        name_string(nt, nl, 0, fi->copyright, sizeof(fi->copyright));
        name_string(nt, nl, 8, fi->manufacturer, sizeof(fi->manufacturer));
        name_string(nt, nl, 9, fi->designer, sizeof(fi->designer));
        name_string(nt, nl, 10, fi->description, sizeof(fi->description));
        name_string(nt, nl, 13, fi->license, sizeof(fi->license));
        name_string(nt, nl, 14, fi->license_url, sizeof(fi->license_url));
        char typo[128];
        name_string(nt, nl, 16, typo, sizeof(typo));                    /* the typographic family, when there is one, is the better name */
        if (typo[0])
            snprintf(fi->family, sizeof(fi->family), "%s", typo);
        name_string(nt, nl, 17, typo, sizeof(typo));
        if (typo[0])
            snprintf(fi->style, sizeof(fi->style), "%s", typo);
    }
    const char *why;
    struct lm_ttf *f = lm_ttf_open_mem(d, n, &why, true);
    if (f) {
        scripts_text(f, fi->scripts, sizeof(fi->scripts));
        if (!fi->cff) {
            int w = 0, h = 0;
            fi->alpha = make_sample(f, &w, &h);
            fi->w = w;
            fi->h = h;
        }
        lm_ttf_close(f);
    }
    free(d);
    return 0;
}
