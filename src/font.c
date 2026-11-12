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

/* ---- the font that is drawn: the bitmap or a TrueType file ------------------------------------------------------ */

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "log.h"
#include "ttf.h"

#define CELL_MAX_W 512
#define CELL_MAX_H 1024
#define CACHE_SLOTS 4096                /* a power of two */
#define CACHE_BYTES (12u << 20)         /* everything is thrown away when the pictures grow beyond this */

struct cell {
    uint32_t cp;
    uint16_t cw, ch;
    uint8_t *px;                        /* NULL = free slot */
};

static struct {
    bool chosen;                        /* lm_font_use ran (or the lazy start did) */
    struct lm_ttf *ttf;
    char name[512];
    uint32_t generation;
    int adv;                            /* the advance all cells are measured by, font units */
    struct cell slot[CACHE_SLOTS];
    int used;
    size_t bytes;
    char conf[512];                     /* `[font] file` of the configuration, "" = not said */
    char env[512];                      /* MELLAGUI_FONT as it was before this process changed it */
    bool env_read;
    uint8_t gamma[256];
} F = { .name = "bitmap" };

static void cache_clear(void)
{
    for (int i = 0; i < CACHE_SLOTS; i++) {
        free(F.slot[i].px);
        F.slot[i].px = NULL;
    }
    F.used = 0;
    F.bytes = 0;
}

static const char *const k_auto[] = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
    "/usr/share/fonts/dejavu-sans-mono-fonts/DejaVuSansMono.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
    "/usr/share/fonts/truetype/liberation2/LiberationMono-Regular.ttf",
    "/usr/share/fonts/liberation/LiberationMono-Regular.ttf",
    "/usr/share/fonts/liberation-mono/LiberationMono-Regular.ttf",
    "/usr/share/fonts/truetype/noto/NotoSansMono-Regular.ttf",
    "/usr/share/fonts/noto/NotoSansMono-Regular.ttf",
    "/usr/share/fonts/truetype/ubuntu/UbuntuMono-R.ttf",
    "/usr/share/fonts/truetype/freefont/FreeMono.ttf",
    "/usr/share/fonts/gnu-free/FreeMono.ttf",
};

static bool open_file(const char *path, bool quiet)
{
    const char *why = NULL;
    struct lm_ttf *t = lm_ttf_open(path, &why);
    if (!t) {
        if (!quiet)
            lm_warn("font: %s %s - the built-in bitmap font is used", path, why ? why : "cannot be used");
        return false;
    }
    /* the cell's width is the advance of the digits (every glyph's, in a monospace font) */
    int g = lm_ttf_glyph(t, '0');
    int adv = g ? lm_ttf_advance(t, g) : 0;
    if (adv <= 0) {
        if (!quiet)
            lm_warn("font: %s has no digits - the built-in bitmap font is used", path);
        lm_ttf_close(t);
        return false;
    }
    F.ttf = t;
    F.adv = adv;
    snprintf(F.name, sizeof(F.name), "%s", path);
    return true;
}

bool lm_font_use(const char *spec)
{
    if (!spec || !*spec)
        spec = "auto";
    char old[sizeof(F.name)];
    snprintf(old, sizeof(old), "%s", F.name);
    bool was = F.chosen;
    F.chosen = true;
    if (was && !strcmp(spec, old))
        return true;                                    /* the same file again: keep the cache */
    if (F.ttf)
        lm_ttf_close(F.ttf);
    F.ttf = NULL;
    snprintf(F.name, sizeof(F.name), "bitmap");
    for (int i = 0; i < 256; i++)                       /* thin strokes of small text a little darker than linear */
        F.gamma[i] = (uint8_t)(pow(i / 255.0, 0.78) * 255.0 + 0.5);

    bool ok = true;
    if (!strcmp(spec, "auto")) {
        for (size_t i = 0; i < sizeof(k_auto) / sizeof(k_auto[0]) && !F.ttf; i++)
            if (access(k_auto[i], R_OK) == 0)
                open_file(k_auto[i], true);
    } else if (strcmp(spec, "bitmap") != 0) {
        ok = open_file(spec, false);
    }
    if (strcmp(old, F.name) != 0 || !was) {
        cache_clear();
        F.generation++;
    }
    return ok;
}

static void ensure(void)
{
    if (!F.chosen) {
        const char *e = getenv("MELLAGUI_FONT");
        lm_font_use(e && *e ? e : "auto");
    }
}

const char *lm_font_name(void)
{
    ensure();
    return F.name;
}

uint32_t lm_font_generation(void)
{
    return F.generation;
}

void lm_font_reset(void)
{
    F.conf[0] = '\0';
}

bool lm_font_conf(void *ud, const char *section, const char *key, const char *value)
{
    (void)ud;
    (void)section;
    if (strcmp(key, "file") != 0)
        return false;
    if (!*value || strlen(value) >= sizeof(F.conf))
        return false;
    if (strcmp(value, "auto") != 0 && strcmp(value, "bitmap") != 0 && value[0] != '/')
        return false;                                   /* a file is named by its whole path */
    snprintf(F.conf, sizeof(F.conf), "%s", value);
    return true;
}

void lm_font_apply(void)
{
    if (!F.env_read) {
        const char *e = getenv("MELLAGUI_FONT");
        snprintf(F.env, sizeof(F.env), "%s", e ? e : "");
        F.env_read = true;
    }
    lm_font_use(F.env[0] ? F.env : F.conf[0] ? F.conf : "auto");
    setenv("MELLAGUI_FONT", F.name, 1);
}

const char *lm_font_choice(void)
{
    return F.env[0] ? F.env : F.conf[0] ? F.conf : "auto";
}

bool lm_font_locked(void)
{
    return F.env[0] != 0;
}

bool lm_font_usable(const char *spec)
{
    if (!spec || !*spec)
        return false;
    if (!strcmp(spec, "auto") || !strcmp(spec, "bitmap"))
        return true;
    if (spec[0] != '/' || strlen(spec) >= sizeof(F.conf) || strpbrk(spec, "\r\n"))
        return false;
    struct lm_ttf *t = lm_ttf_open(spec, NULL);
    bool ok = t && lm_ttf_glyph(t, '0') && lm_ttf_advance(t, lm_ttf_glyph(t, '0')) > 0;
    lm_ttf_close(t);
    return ok;
}

struct scan {
    char (*out)[LM_FONT_PATH_MAX];
    int n, max, opened;
};

static bool plain_face(const char *name)
{
    static const char *const styled[] = { "bold", "italic", "oblique", "light", "thin", "condensed", "black", "medium" };
    char low[256];
    size_t i = 0;
    for (; name[i] && i < sizeof(low) - 1; i++)
        low[i] = (char)(name[i] >= 'A' && name[i] <= 'Z' ? name[i] + 32 : name[i]);
    low[i] = 0;
    for (size_t k = 0; k < sizeof(styled) / sizeof(styled[0]); k++)
        if (strstr(low, styled[k]))
            return false;
    return true;
}

static void scan_dir(struct scan *sc, const char *dir, int depth)
{
    DIR *d = depth < 5 ? opendir(dir) : NULL;
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) && sc->n < sc->max && sc->opened < 600) {
        if (e->d_name[0] == '.')
            continue;
        char path[LM_FONT_PATH_MAX];
        if (snprintf(path, sizeof(path), "%s/%s", dir, e->d_name) >= (int)sizeof(path))
            continue;
        struct stat st;
        if (stat(path, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            scan_dir(sc, path, depth + 1);
            continue;
        }
        size_t len = strlen(e->d_name);
        if (!S_ISREG(st.st_mode) || len < 5 || strcasecmp(e->d_name + len - 4, ".ttf") != 0 || st.st_size > (16 << 20) ||
            !plain_face(e->d_name))
            continue;
        sc->opened++;
        struct lm_ttf *t = lm_ttf_open(path, NULL);
        if (t && lm_ttf_monospace(t)) {
            bool dup = false;                                   /* the same file name in two folders: the first one */
            for (int i = 0; i < sc->n; i++) {
                const char *b = strrchr(sc->out[i], '/');
                dup = dup || !strcmp(b ? b + 1 : sc->out[i], e->d_name);
            }
            if (!dup)
                snprintf(sc->out[sc->n++], LM_FONT_PATH_MAX, "%s", path);
        }
        lm_ttf_close(t);
    }
    closedir(d);
}

static int by_file_name(const void *a, const void *b)
{
    const char *x = a, *y = b, *bx = strrchr(x, '/'), *by = strrchr(y, '/');
    return strcasecmp(bx ? bx + 1 : x, by ? by + 1 : y);
}

int lm_font_list(char (*out)[LM_FONT_PATH_MAX], int max)
{
    struct scan sc = { out, 0, max, 0 };
    const char *dirs = getenv("MELLAGUI_FONT_DIRS");
    if (dirs) {
        char buf[1024];
        snprintf(buf, sizeof(buf), "%s", dirs);
        for (char *save = NULL, *p = strtok_r(buf, ":", &save); p; p = strtok_r(NULL, ":", &save))
            scan_dir(&sc, p, 0);
    } else {
        const char *home = getenv("HOME");
        char p[LM_FONT_PATH_MAX];
        scan_dir(&sc, "/usr/share/fonts", 0);
        scan_dir(&sc, "/usr/local/share/fonts", 0);
        if (home && *home) {
            if (snprintf(p, sizeof(p), "%s/.local/share/fonts", home) < (int)sizeof(p))
                scan_dir(&sc, p, 0);
            if (snprintf(p, sizeof(p), "%s/.fonts", home) < (int)sizeof(p))
                scan_dir(&sc, p, 0);
        }
    }
    qsort(out, (size_t)sc.n, LM_FONT_PATH_MAX, by_file_name);
    return sc.n;
}

/* The bitmap glyph in a cell of cw x ch: each pixel is the part of it that the font's set pixels cover. */
static void bitmap_cell(uint32_t cp, int cw, int ch, uint8_t *out)
{
    const uint8_t *g = lm_font_glyph(cp);
    if (cw % LM_FONT_W == 0 && ch % LM_FONT_H == 0) {   /* whole multiples: crisp */
        int zx = cw / LM_FONT_W, zy = ch / LM_FONT_H;
        for (int y = 0; y < ch; y++)
            for (int x = 0; x < cw; x++)
                out[y * cw + x] = (g[y / zy] & (0x80 >> (x / zx))) ? 255 : 0;
        return;
    }
    for (int y = 0; y < ch; y++) {
        int ylo = y * LM_FONT_H, yhi = ylo + LM_FONT_H;         /* in units of 1/ch of a font row */
        for (int x = 0; x < cw; x++) {
            int xlo = x * LM_FONT_W, xhi = xlo + LM_FONT_W;
            long cov = 0;
            for (int r = ylo / ch; r < LM_FONT_H && r * ch < yhi; r++) {
                int oy = (yhi < (r + 1) * ch ? yhi : (r + 1) * ch) - (ylo > r * ch ? ylo : r * ch);
                if (oy <= 0)
                    continue;
                for (int c = xlo / cw; c < LM_FONT_W && c * cw < xhi; c++) {
                    int ox = (xhi < (c + 1) * cw ? xhi : (c + 1) * cw) - (xlo > c * cw ? xlo : c * cw);
                    if (ox > 0 && (g[r] & (0x80 >> c)))
                        cov += (long)ox * oy;
                }
            }
            out[y * cw + x] = (uint8_t)(cov * 255 / ((long)LM_FONT_W * LM_FONT_H));
        }
    }
}

static bool in_bitmap(uint32_t cp)
{
    int lo = 0, hi = N_GLYPHS - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (font_cps[mid] == cp)
            return true;
        if (font_cps[mid] < cp)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return false;
}

static void render_cell(uint32_t cp, int cw, int ch, uint8_t *out)
{
    bool lines = cp >= 0x2500 && cp <= 0x259f;
    int g = F.ttf && !(lines && in_bitmap(cp)) ? lm_ttf_glyph(F.ttf, cp) : 0;
    if (!g) {
        if (F.ttf && (cp == ' ' || cp == 0xa0)) {
            memset(out, 0, (size_t)cw * (size_t)ch);
            return;
        }
        bitmap_cell(cp, cw, ch, out);
        return;
    }
    struct lm_ttf *t = F.ttf;
    double asc = lm_ttf_ascent(t), desc = -lm_ttf_descent(t);
    double s = cw / (double)F.adv;                              /* pixels per font unit */
    if ((asc + desc) * s > ch)
        s = ch / (asc + desc);
    /* the line sits in the middle of the cell, its baseline on a whole pixel */
    double base = floor((ch - (asc + desc) * s) / 2 + asc * s + 0.5);
    /* the x height on whole pixels too: the tops of the small letters are then sharp (all the hinting there is) */
    double sy = s;
    int xh = lm_ttf_xheight(t);
    if (xh > 0) {
        double px = xh * s, snapped = floor(px + 0.5);
        if (snapped >= 1 && snapped / px > 0.88 && snapped / px < 1.14)
            sy = s * snapped / px;
    }
    double sx = s, ox = 0;
    int adv = lm_ttf_advance(t, g);
    if (adv > 0 && adv * s > cw + 0.01)                         /* a wide glyph (not a monospace font): squeezed */
        sx = cw / (double)adv;
    else if (adv > 0)
        ox = (cw - adv * s) / 2;
    if (!lm_ttf_render(t, g, sx, sy, ox, base, out, cw, ch))
        return;                                                 /* no outline: an empty cell */
    if (ch < 40)
        for (int i = 0; i < cw * ch; i++)
            out[i] = F.gamma[out[i]];
}

const uint8_t *lm_font_cell(uint32_t cp, int cw, int ch)
{
    ensure();
    cw = cw < 1 ? 1 : cw > CELL_MAX_W ? CELL_MAX_W : cw;
    ch = ch < 1 ? 1 : ch > CELL_MAX_H ? CELL_MAX_H : ch;
    uint32_t h = (cp * 2654435761u) ^ ((uint32_t)cw * 40503u) ^ ((uint32_t)ch * 2246822519u);
    h ^= h >> 15;
    int i = (int)(h & (CACHE_SLOTS - 1));
    for (int probe = 0; probe < CACHE_SLOTS; probe++, i = (i + 1) & (CACHE_SLOTS - 1)) {
        struct cell *c = &F.slot[i];
        if (!c->px)
            break;
        if (c->cp == cp && c->cw == cw && c->ch == ch)
            return c->px;
    }
    size_t size = (size_t)cw * (size_t)ch;
    if (F.used >= CACHE_SLOTS * 3 / 4 || F.bytes + size > CACHE_BYTES) {
        cache_clear();
        i = (int)(h & (CACHE_SLOTS - 1));
    }
    uint8_t *px = malloc(size);
    if (!px) {                                                  /* out of memory: an empty cell, never NULL */
        static uint8_t none[CELL_MAX_W * CELL_MAX_H];
        return none;
    }
    render_cell(cp, cw, ch, px);
    F.slot[i] = (struct cell){ cp, (uint16_t)cw, (uint16_t)ch, px };
    F.used++;
    F.bytes += size;
    return px;
}
