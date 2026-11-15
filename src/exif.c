#include "exif.h"

#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct tr {
    const uint8_t *d;
    size_t n;
    bool big;
};

static uint32_t g16(const struct tr *t, size_t o)
{
    if (o + 2 > t->n)
        return 0;
    return t->big ? (uint32_t)(t->d[o] << 8 | t->d[o + 1]) : (uint32_t)(t->d[o] | t->d[o + 1] << 8);
}

static uint32_t g32(const struct tr *t, size_t o)
{
    if (o + 4 > t->n)
        return 0;
    return t->big ? (uint32_t)t->d[o] << 24 | (uint32_t)t->d[o + 1] << 16 | (uint32_t)t->d[o + 2] << 8 | t->d[o + 3]
                  : (uint32_t)t->d[o] | (uint32_t)t->d[o + 1] << 8 | (uint32_t)t->d[o + 2] << 16 | (uint32_t)t->d[o + 3] << 24;
}

struct ent {
    unsigned tag, type;
    uint32_t count;
    size_t off;                                     /* where the value lies (inside the block) */
    bool ok;
};

static size_t type_size(unsigned t)
{
    switch (t) {
    case 1: case 2: case 6: case 7: return 1;
    case 3: case 8: return 2;
    case 4: case 9: case 11: return 4;
    case 5: case 10: case 12: return 8;
    default: return 0;
    }
}

/* The i-th entry of the IFD at `ifd`. */
static struct ent entry(const struct tr *t, size_t ifd, unsigned i)
{
    struct ent e = { 0 };
    size_t p = ifd + 2 + (size_t)i * 12;
    if (p + 12 > t->n)
        return e;
    e.tag = g16(t, p);
    e.type = g16(t, p + 2);
    e.count = g32(t, p + 4);
    size_t sz = type_size(e.type);
    if (!sz || e.count > (1u << 24))
        return e;
    uint64_t total = (uint64_t)sz * e.count;
    e.off = total <= 4 ? p + 8 : g32(t, p + 8);
    e.ok = e.off + total <= t->n;
    return e;
}

static double rational(const struct tr *t, const struct ent *e, uint32_t i)
{
    if (!e->ok || (e->type != 5 && e->type != 10) || i >= e->count)
        return 0;
    size_t o = e->off + (size_t)i * 8;
    if (e->type == 5) {
        double d = g32(t, o + 4);
        return d ? (double)g32(t, o) / d : 0;
    }
    double d = (int32_t)g32(t, o + 4);
    return d ? (double)(int32_t)g32(t, o) / d : 0;
}

static long integer(const struct tr *t, const struct ent *e)
{
    if (!e->ok || e->count < 1)
        return 0;
    switch (e->type) {
    case 1: case 7: return t->d[e->off];
    case 3: return (long)g16(t, e->off);
    case 4: return (long)g32(t, e->off);
    case 6: return (int8_t)t->d[e->off];
    case 8: return (int16_t)g16(t, e->off);
    case 9: return (int32_t)g32(t, e->off);
    default: return 0;
    }
}

static void text(const struct tr *t, const struct ent *e, char *out, size_t cap)
{
    out[0] = 0;
    if (!e->ok || e->type != 2 || !e->count)
        return;
    size_t n = e->count < cap - 1 ? e->count : cap - 1, k = 0;
    for (size_t i = 0; i < n && t->d[e->off + i]; i++) {
        unsigned char c = t->d[e->off + i];
        out[k++] = c < 0x20 ? ' ' : (char)c;
    }
    while (k > 0 && out[k - 1] == ' ')
        k--;
    out[k] = 0;
}

static void gps_part(const struct tr *t, const struct ent *e, double *v, bool *ok)
{
    if (!e->ok || e->type != 5 || e->count < 3)
        return;
    *v = rational(t, e, 0) + rational(t, e, 1) / 60.0 + rational(t, e, 2) / 3600.0;
    *ok = true;
}

static void walk(const struct tr *t, size_t ifd, int kind, struct lm_exif *x, int depth)
{
    if (ifd + 2 > t->n || depth > 3)
        return;
    unsigned n = g16(t, ifd);
    if (n > 512)
        n = 512;
    char latref = 'N', lonref = 'E';
    double lat = 0, lon = 0;
    bool haslat = false, haslon = false;
    int altref = 0;
    double alt = 0;
    bool hasalt = false;
    char dt_orig[24] = "", dt_dig[24] = "", dt_mod[24] = "";
    for (unsigned i = 0; i < n; i++) {
        struct ent e = entry(t, ifd, i);
        if (!e.ok)
            continue;
        if (kind == 0) {                                                    /* IFD0 */
            switch (e.tag) {
            case 0x010f: text(t, &e, x->make, sizeof(x->make)); break;
            case 0x0110: text(t, &e, x->model, sizeof(x->model)); break;
            case 0x0131: text(t, &e, x->software, sizeof(x->software)); break;
            case 0x013b: text(t, &e, x->artist, sizeof(x->artist)); break;
            case 0x8298: text(t, &e, x->copyright, sizeof(x->copyright)); break;
            case 0x010e: text(t, &e, x->description, sizeof(x->description)); break;
            case 0x0132: text(t, &e, dt_mod, sizeof(dt_mod)); break;
            case 0x0112: x->orientation = (int)integer(t, &e); break;
            case 0x8769: walk(t, (size_t)integer(t, &e), 1, x, depth + 1); break;
            case 0x8825: walk(t, (size_t)integer(t, &e), 2, x, depth + 1); break;
            }
        } else if (kind == 1) {                                             /* the Exif sub-IFD */
            switch (e.tag) {
            case 0x829a: x->exposure = rational(t, &e, 0); break;
            case 0x829d: x->fnumber = rational(t, &e, 0); break;
            case 0x8827: x->iso = (int)integer(t, &e); break;
            case 0x9003: text(t, &e, dt_orig, sizeof(dt_orig)); break;
            case 0x9004: text(t, &e, dt_dig, sizeof(dt_dig)); break;
            case 0x9204: x->bias = rational(t, &e, 0); x->has_bias = true; break;
            case 0x9209: x->flash = (int)integer(t, &e); break;
            case 0x920a: x->focal = rational(t, &e, 0); break;
            case 0xa405: x->focal35 = (double)integer(t, &e); break;
            case 0xa002: x->width = (int)integer(t, &e); break;
            case 0xa003: x->height = (int)integer(t, &e); break;
            case 0xa434: text(t, &e, x->lens, sizeof(x->lens)); break;
            }
        } else {                                                            /* the GPS sub-IFD */
            switch (e.tag) {
            case 1: latref = e.ok && e.count >= 1 ? (char)t->d[e.off] : 'N'; break;
            case 2: gps_part(t, &e, &lat, &haslat); break;
            case 3: lonref = e.ok && e.count >= 1 ? (char)t->d[e.off] : 'E'; break;
            case 4: gps_part(t, &e, &lon, &haslon); break;
            case 5: altref = (int)integer(t, &e); break;
            case 6: alt = rational(t, &e, 0); hasalt = e.ok && e.type == 5; break;
            }
        }
    }
    if (kind == 1) {
        const char *src = dt_orig[0] ? dt_orig : dt_dig;
        if (src[0])
            snprintf(x->taken, sizeof(x->taken), "%s", src);
    } else if (kind == 0 && dt_mod[0] && !x->taken[0]) {
        snprintf(x->taken, sizeof(x->taken), "%s", dt_mod);
    }
    if (kind == 2 && haslat && haslon && fabs(lat) <= 90 && fabs(lon) <= 180 && (lat != 0 || lon != 0)) {
        x->has_gps = true;
        x->lat = (latref == 'S' || latref == 's') ? -lat : lat;
        x->lon = (lonref == 'W' || lonref == 'w') ? -lon : lon;
        if (hasalt) {
            x->has_alt = true;
            x->alt = altref == 1 ? -alt : alt;
        }
    }
}

bool lm_exif_parse_tiff(const uint8_t *tiff, size_t n, struct lm_exif *x)
{
    if (n < 8 || !((tiff[0] == 'I' && tiff[1] == 'I' && tiff[2] == 42 && !tiff[3]) || (tiff[0] == 'M' && tiff[1] == 'M' && !tiff[2] && tiff[3] == 42)))
        return false;
    struct tr t = { tiff, n, tiff[0] == 'M' };
    x->flash = -1;
    walk(&t, g32(&t, 4), 0, x, 0);
    /* a Date in IFD0 only counts when the Exif block has none: walk() handles the order; the "any" flag is set by what is worth showing */
    x->any = x->make[0] || x->model[0] || x->lens[0] || x->taken[0] || x->exposure > 0 || x->fnumber > 0 || x->iso > 0 || x->has_gps || x->focal > 0 || x->artist[0] ||
             x->copyright[0] || x->description[0] || x->software[0];
    return true;
}

bool lm_exif_read_mem(const uint8_t *d, size_t n, struct lm_exif *x)
{
    memset(x, 0, sizeof(*x));
    x->flash = -1;
    if (n >= 4 && d[0] == 0xff && d[1] == 0xd8) {                           /* JPEG: the first APP1 "Exif" segment */
        size_t p = 2;
        while (p + 4 <= n) {
            if (d[p] != 0xff) {
                p++;
                continue;
            }
            unsigned m = d[p + 1];
            if (m == 0xff) {
                p++;
                continue;
            }
            if (m == 0xd8 || (m >= 0xd0 && m <= 0xd7) || m == 0x01) {
                p += 2;
                continue;
            }
            if (m == 0xda || m == 0xd9)
                break;
            size_t len = (size_t)(d[p + 2] << 8 | d[p + 3]);
            if (len < 2 || p + 2 + len > n)
                break;
            if (m == 0xe1 && len >= 8 && !memcmp(d + p + 4, "Exif\0\0", 6))
                return lm_exif_parse_tiff(d + p + 10, len - 8, x) && x->any;
            p += 2 + len;
        }
        return false;
    }
    if (n >= 8 && !memcmp(d, "\x89PNG\r\n\x1a\n", 8)) {                     /* PNG: the eXIf chunk */
        size_t p = 8;
        while (p + 12 <= n) {
            uint32_t len = (uint32_t)d[p] << 24 | (uint32_t)d[p + 1] << 16 | (uint32_t)d[p + 2] << 8 | d[p + 3];
            if (len > n - p - 12)
                break;
            if (!memcmp(d + p + 4, "eXIf", 4))
                return lm_exif_parse_tiff(d + p + 8, len, x) && x->any;
            if (!memcmp(d + p + 4, "IDAT", 4))
                break;
            p += 12 + len;
        }
        return false;
    }
    if (n >= 16 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WEBP", 4)) {     /* WebP: the EXIF chunk */
        size_t p = 12;
        while (p + 8 <= n) {
            uint32_t len = (uint32_t)d[p + 4] | (uint32_t)d[p + 5] << 8 | (uint32_t)d[p + 6] << 16 | (uint32_t)d[p + 7] << 24;
            if (len > n - p - 8)
                break;
            if (!memcmp(d + p, "EXIF", 4)) {
                const uint8_t *b = d + p + 8;
                size_t bl = len;
                if (bl >= 6 && !memcmp(b, "Exif\0\0", 6)) {
                    b += 6;
                    bl -= 6;
                }
                return lm_exif_parse_tiff(b, bl, x) && x->any;
            }
            p += 8 + len + (len & 1);
        }
        return false;
    }
    return lm_exif_parse_tiff(d, n, x) && x->any;                           /* a TIFF file is an EXIF block itself */
}

bool lm_exif_read_file(const char *path, struct lm_exif *x)
{
    memset(x, 0, sizeof(*x));
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    enum { MAXB = 8 << 20 };                                                /* the metadata is at the start: the first 8 MB are enough */
    uint8_t *b = malloc(MAXB + 1);
    ssize_t n = b ? read(fd, b, MAXB) : -1;
    close(fd);
    bool ok = n > 0 && lm_exif_read_mem(b, (size_t)n, x);
    free(b);
    return ok;
}

static void fix_date(const char *in, char *out, size_t cap)
{
    snprintf(out, cap, "%s", in);
    if (strlen(out) >= 10 && out[4] == ':' && out[7] == ':') {
        out[4] = '-';
        out[7] = '-';
    }
}

int lm_exif_lines(const struct lm_exif *x, char lines[][200], int max)
{
    int n = 0;
    char t[200];
    if ((x->make[0] || x->model[0]) && n < max) {
        if (x->make[0] && x->model[0] && !strncasecmp(x->model, x->make, strlen(x->make)))
            snprintf(t, sizeof(t), "Camera: %s", x->model);
        else
            snprintf(t, sizeof(t), "Camera: %s%s%s", x->make, x->make[0] && x->model[0] ? " " : "", x->model);
        snprintf(lines[n++], 200, "%s", t);
    }
    if (x->lens[0] && n < max)
        snprintf(lines[n++], 200, "Lens: %s", x->lens);
    if ((x->exposure > 0 || x->fnumber > 0 || x->iso > 0 || x->focal > 0) && n < max) {
        char e[120] = "";
        size_t l = 0;
        if (x->exposure > 0) {
            if (x->exposure < 0.95)
                l += (size_t)snprintf(e + l, sizeof(e) - l, "1/%.0f s", 1.0 / x->exposure);
            else
                l += (size_t)snprintf(e + l, sizeof(e) - l, "%.1f s", x->exposure);
        }
        if (x->fnumber > 0)
            l += (size_t)snprintf(e + l, sizeof(e) - l, "%sf/%.1f", l ? "  " : "", x->fnumber);
        if (x->iso > 0)
            l += (size_t)snprintf(e + l, sizeof(e) - l, "%sISO %d", l ? "  " : "", x->iso);
        if (x->focal > 0) {
            l += (size_t)snprintf(e + l, sizeof(e) - l, "%s%.0f mm", l ? "  " : "", x->focal);
            if (x->focal35 > 0 && fabs(x->focal35 - x->focal) > 0.5)
                l += (size_t)snprintf(e + l, sizeof(e) - l, " (%.0f mm in 35 mm)", x->focal35);
        }
        if (x->has_bias && fabs(x->bias) > 0.05)
            l += (size_t)snprintf(e + l, sizeof(e) - l, "%s%+.1f EV", l ? "  " : "", x->bias);
        if (x->flash >= 0 && (x->flash & 1))
            snprintf(e + l, sizeof(e) - l, "%sflash", l ? "  " : "");
        snprintf(lines[n++], 200, "Exposure: %s", e);
    }
    if (x->taken[0] && n < max) {
        char d[32];
        fix_date(x->taken, d, sizeof(d));
        snprintf(lines[n++], 200, "Taken: %s", d);
    }
    if (x->has_gps && n < max) {
        char a[40] = "";
        if (x->has_alt)
            snprintf(a, sizeof(a), "  %.0f m", x->alt);
        snprintf(lines[n++], 200, "Place: %.5f\xc2\xb0 %c, %.5f\xc2\xb0 %c%s", fabs(x->lat), x->lat < 0 ? 'S' : 'N', fabs(x->lon), x->lon < 0 ? 'W' : 'E', a);
    }
    if (x->description[0] && n < max)
        snprintf(lines[n++], 200, "About: %s", x->description);
    if ((x->artist[0] || x->copyright[0]) && n < max)
        snprintf(lines[n++], 200, "By: %s%s%s", x->artist, x->artist[0] && x->copyright[0] ? "  " : "", x->copyright);
    if (x->software[0] && n < max)
        snprintf(lines[n++], 200, "Software: %s", x->software);
    return n;
}
