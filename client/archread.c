#include "archread.h"

#include "../src/img.h"
#include "decomp.h"
#include "zstddec.h"

#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define GZ_CAP (64u << 20)                      /* a gzip stream is unpacked this far (and read this far) */
#define MAX_TAR_HEADERS 3000000
#define CD_MAX (96u << 20)                      /* the ZIP central directory at most this big */

static uint32_t u16(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8; }
static uint32_t u32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t u64(const uint8_t *p) { return u32(p) | (uint64_t)u32(p + 4) << 32; }

static bool pread_all(int fd, void *buf, size_t n, uint64_t off)
{
    uint8_t *p = buf;
    while (n) {
        ssize_t r = pread(fd, p, n, (off_t)off);
        if (r <= 0)
            return false;
        p += r;
        n -= (size_t)r;
        off += (uint64_t)r;
    }
    return true;
}

static void seterr(char *err, size_t n, const char *s)
{
    if (err && n)
        snprintf(err, n, "%s", s);
}

void arc_free(struct arc_list *a)
{
    for (int i = 0; i < a->n; i++) {
        free(a->name[i]);
        free(a->mtime[i]);
    }
    free(a->name);
    free(a->mtime);
    free(a->size);
    free(a->packed);
    memset(a, 0, sizeof(*a));
}

/* ---- adding an entry ---- */
static void put_utf8(char *o, size_t *n, size_t cap, unsigned cp)
{
    if (cp < 0x80 && *n + 1 < cap) {
        o[(*n)++] = (char)cp;
    } else if (cp < 0x800 && *n + 2 < cap) {
        o[(*n)++] = (char)(0xc0 | cp >> 6);
        o[(*n)++] = (char)(0x80 | (cp & 0x3f));
    } else if (cp >= 0x800 && *n + 3 < cap) {
        o[(*n)++] = (char)(0xe0 | cp >> 12);
        o[(*n)++] = (char)(0x80 | ((cp >> 6) & 0x3f));
        o[(*n)++] = (char)(0x80 | (cp & 0x3f));
    }
}

static const uint16_t cp437_high[128] = {
    0x00c7, 0x00fc, 0x00e9, 0x00e2, 0x00e4, 0x00e0, 0x00e5, 0x00e7, 0x00ea, 0x00eb, 0x00e8, 0x00ef, 0x00ee, 0x00ec, 0x00c4, 0x00c5,
    0x00c9, 0x00e6, 0x00c6, 0x00f4, 0x00f6, 0x00f2, 0x00fb, 0x00f9, 0x00ff, 0x00d6, 0x00dc, 0x00a2, 0x00a3, 0x00a5, 0x20a7, 0x0192,
    0x00e1, 0x00ed, 0x00f3, 0x00fa, 0x00f1, 0x00d1, 0x00aa, 0x00ba, 0x00bf, 0x2310, 0x00ac, 0x00bd, 0x00bc, 0x00a1, 0x00ab, 0x00bb,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557, 0x255d, 0x255c, 0x255b, 0x2510,
    0x2514, 0x2534, 0x252c, 0x251c, 0x2500, 0x253c, 0x255e, 0x255f, 0x255a, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256c, 0x2567,
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256b, 0x256a, 0x2518, 0x250c, 0x2588, 0x2584, 0x258c, 0x2590, 0x2580,
    0x03b1, 0x00df, 0x0393, 0x03c0, 0x03a3, 0x03c3, 0x00b5, 0x03c4, 0x03a6, 0x0398, 0x03a9, 0x03b4, 0x221e, 0x03c6, 0x03b5, 0x2229,
    0x2261, 0x00b1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00f7, 0x2248, 0x00b0, 0x2219, 0x00b7, 0x221a, 0x207f, 0x00b2, 0x25a0, 0x00a0,
};

static bool valid_utf8(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n;) {
        uint8_t c = p[i];
        size_t k = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!k || i + k > n)
            return false;
        for (size_t j = 1; j < k; j++)
            if ((p[i + j] & 0xc0) != 0x80)
                return false;
        i += k;
    }
    return true;
}

/* A name for display: UTF-8 as it is (or the DOS code page 437 when `dos` and it is not UTF-8), control characters as ?. */
static char *clean_name(const uint8_t *p, size_t n, bool dos)
{
    size_t cap = n * 3 + 4, o = 0;
    char *s = malloc(cap);
    if (!s)
        return NULL;
    bool as_dos = dos && !valid_utf8(p, n);
    for (size_t i = 0; i < n && o + 4 < cap; i++) {
        unsigned char c = p[i];
        if (as_dos && c >= 0x80)
            put_utf8(s, &o, cap, cp437_high[c - 0x80]);
        else if (c < 0x20 || c == 0x7f)
            s[o++] = '?';
        else
            s[o++] = (char)c;
    }
    s[o] = 0;
    return s;
}

/* The name as text a person can read (a ZIP name in the old DOS code page 437 becomes UTF-8; control characters become ?). Malloc'd. */
char *arc_clean_name(const unsigned char *p, size_t n, int dos)
{
    return clean_name(p, n, dos);
}

static void civil_from_days(long z, int *y, int *m, int *d)
{
    z += 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long yy = (long)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yy + (*m <= 2));
}

static char *unix_time_text(int64_t t)
{
    char o[48];
    if (t <= 0 || t > 253402300799LL)
        return strdup("");
    int y, m, d;
    civil_from_days((long)(t / 86400), &y, &m, &d);
    long sod = (long)(t % 86400);
    snprintf(o, sizeof(o), "%04d-%02d-%02d %02d:%02d", y, m, d, (int)(sod / 3600), (int)(sod / 60 % 60));
    return strdup(o);
}

/* Takes over `name` and `mtime`. */
static void arc_add(struct arc_list *a, int max, char *name, char *mtime, long long size, long long packed)
{
    a->total++;
    if (size > 0)
        a->total_size += size;
    if (packed > 0)
        a->total_packed += packed;
    if (a->n >= max) {
        free(name);
        free(mtime);
        return;
    }
    a->name = realloc(a->name, (size_t)(a->n + 1) * sizeof(char *));
    a->mtime = realloc(a->mtime, (size_t)(a->n + 1) * sizeof(char *));
    a->size = realloc(a->size, (size_t)(a->n + 1) * sizeof(long long));
    a->packed = realloc(a->packed, (size_t)(a->n + 1) * sizeof(long long));
    a->name[a->n] = name ? name : strdup("");
    a->mtime[a->n] = mtime ? mtime : strdup("");
    a->size[a->n] = size;
    a->packed[a->n] = packed;
    a->n++;
}

/* ---- ZIP ---- */
static int zip_list(int fd, uint64_t fsize, int max, struct arc_list *a, char *err, size_t errn)
{
    snprintf(a->kind, sizeof(a->kind), "ZIP");
    a->has_packed = 1;
    size_t tail = fsize < 22 + 65535 + 20 ? (size_t)fsize : 22 + 65535 + 20;
    uint8_t *t = malloc(tail);
    if (!t || !pread_all(fd, t, tail, fsize - tail)) {
        free(t);
        seterr(err, errn, "A ZIP file that cannot be read.");
        return -2;
    }
    long at = -1;
    for (long i = (long)tail - 22; i >= 0; i--)
        if (!memcmp(t + i, "PK\5\6", 4)) {
            at = i;
            break;
        }
    if (at < 0) {
        free(t);
        seterr(err, errn, "A damaged ZIP file (no end record).");
        return -2;
    }
    uint64_t count = u16(t + at + 10), cdsize = u32(t + at + 12), cdoff = u32(t + at + 16);
    if ((count == 0xffff || cdsize == 0xffffffffu || cdoff == 0xffffffffu) && at >= 20 && !memcmp(t + at - 20, "PK\6\7", 4)) {       /* ZIP64 */
        uint64_t z64 = u64(t + at - 20 + 8);
        uint8_t h[56];
        if (z64 + 56 <= fsize && pread_all(fd, h, 56, z64) && !memcmp(h, "PK\6\6", 4)) {
            count = u64(h + 32);
            cdsize = u64(h + 40);
            cdoff = u64(h + 48);
        }
    }
    free(t);
    if (cdsize > CD_MAX || cdoff > fsize || cdsize > fsize - cdoff) {
        seterr(err, errn, "A damaged ZIP file (its directory).");
        return -2;
    }
    uint8_t *cd = malloc(cdsize ? cdsize : 1);
    if (!cd || !pread_all(fd, cd, cdsize, cdoff)) {
        free(cd);
        seterr(err, errn, "A ZIP file whose directory cannot be read.");
        return -2;
    }
    size_t p = 0;
    for (uint64_t i = 0; i < count; i++) {
        if (p + 46 > cdsize || memcmp(cd + p, "PK\1\2", 4)) {
            snprintf(a->note, sizeof(a->note), "The directory ends early: %lld of %llu entries were found.", a->total, (unsigned long long)count);
            break;
        }
        uint32_t flags = u16(cd + p + 8), tm = u16(cd + p + 12), dt = u16(cd + p + 14);
        uint64_t csize = u32(cd + p + 20), usize = u32(cd + p + 24);
        size_t nl = u16(cd + p + 28), xl = u16(cd + p + 30), cl = u16(cd + p + 32);
        if (p + 46 + nl + xl + cl > cdsize) {
            snprintf(a->note, sizeof(a->note), "The directory ends early: %lld of %llu entries were found.", a->total, (unsigned long long)count);
            break;
        }
        if (usize == 0xffffffffu || csize == 0xffffffffu) {                 /* the ZIP64 extra field holds the real sizes, in this order */
            const uint8_t *x = cd + p + 46 + nl, *xe = x + xl;
            while (x + 4 <= xe) {
                size_t id = u16(x), len = u16(x + 2);
                if (x + 4 + len > xe)
                    break;
                if (id == 1) {
                    const uint8_t *v = x + 4, *ve = x + 4 + len;
                    if (usize == 0xffffffffu && v + 8 <= ve) { usize = u64(v); v += 8; }
                    if (csize == 0xffffffffu && v + 8 <= ve) csize = u64(v);
                }
                x += 4 + len;
            }
        }
        char *name = clean_name(cd + p + 46, nl, !(flags & 0x800));
        char tmx[32] = "";
        if (dt) {
            int y = 1980 + (int)(dt >> 9), mo = (int)((dt >> 5) & 15), d = (int)(dt & 31);
            if (mo >= 1 && mo <= 12 && d >= 1)
                snprintf(tmx, sizeof(tmx), "%04d-%02d-%02d %02d:%02d", y, mo, d, (int)(tm >> 11), (int)((tm >> 5) & 63));
        }
        bool dir = nl && cd[p + 46 + nl - 1] == '/';
        arc_add(a, max, name, strdup(tmx), dir ? -1 : (long long)usize, dir ? -1 : (long long)csize);
        p += 46 + nl + xl + cl;
    }
    free(cd);
    return 0;
}

/* ---- TAR ---- */
struct tarsrc {
    int fd;                             /* a file ... */
    const uint8_t *mem;                 /* ... or memory */
    uint64_t size;
};

static bool tar_read(const struct tarsrc *s, uint64_t off, void *buf, size_t n)
{
    if (off + n > s->size)
        return false;
    if (s->mem) {
        memcpy(buf, s->mem + off, n);
        return true;
    }
    return pread_all(s->fd, buf, n, off);
}

static bool tar_header_ok(const uint8_t *h)
{
    unsigned sum = 0;
    for (int i = 0; i < 512; i++)
        sum += (i >= 148 && i < 156) ? ' ' : h[i];
    const uint8_t *c = h + 148;
    unsigned want = 0;
    int k = 0;
    while (k < 8 && c[k] == ' ')
        k++;
    int digits = 0;
    for (; k < 8 && c[k] >= '0' && c[k] <= '7'; k++, digits++)
        want = want * 8 + (unsigned)(c[k] - '0');
    return digits > 0 && want == sum;
}

static uint64_t tar_num(const uint8_t *p, size_t n)
{
    if (p[0] & 0x80) {                                                  /* base 256 (GNU) for sizes over 8 GB */
        uint64_t v = p[0] & 0x7f;
        for (size_t i = 1; i < n; i++)
            v = v << 8 | p[i];
        return v;
    }
    uint64_t v = 0;
    size_t i = 0;
    while (i < n && (p[i] == ' ' || p[i] == 0))
        i++;
    for (; i < n && p[i] >= '0' && p[i] <= '7'; i++)
        v = v * 8 + (uint64_t)(p[i] - '0');
    return v;
}

static size_t cstrlen(const uint8_t *p, size_t n)
{
    size_t l = 0;
    while (l < n && p[l])
        l++;
    return l;
}

/* The `key=value` records of a pax header ("LEN key=value\n"): the value of `want`, or NULL. */
static char *pax_value(const uint8_t *d, size_t n, const char *want)
{
    size_t i = 0, wl = strlen(want);
    while (i < n) {
        size_t len = 0, j = i;
        while (j < n && d[j] >= '0' && d[j] <= '9' && len < 100000)
            len = len * 10 + (size_t)(d[j++] - '0');
        if (j >= n || d[j] != ' ' || len == 0 || i + len > n)
            return NULL;
        const uint8_t *k = d + j + 1, *e = d + i + len;
        if (e > k && e[-1] == '\n')
            e--;
        if ((size_t)(e - k) > wl && !memcmp(k, want, wl) && k[wl] == '=')
            return strndup((const char *)k + wl + 1, (size_t)(e - (k + wl + 1)));
        i += len;
    }
    return NULL;
}

static int tar_list(const struct tarsrc *s, int max, struct arc_list *a, bool truncated_src)
{
    uint64_t off = 0;
    char *longname = NULL, *paxpath = NULL, *longlink = NULL;
    int64_t paxsize = -1;
    int zeros = 0;
    for (long hdrs = 0; hdrs < MAX_TAR_HEADERS; hdrs++) {
        uint8_t h[512];
        if (!tar_read(s, off, h, 512)) {
            if (truncated_src)
                snprintf(a->note, sizeof(a->note), "The listing stops at the first 64 MB of the unpacked archive.");
            else if (off < s->size && !a->note[0])
                snprintf(a->note, sizeof(a->note), "The archive ends in the middle of a header.");
            break;
        }
        bool zero = true;
        for (int i = 0; i < 512 && zero; i++)
            zero = h[i] == 0;
        if (zero) {
            if (++zeros >= 2)
                break;
            off += 512;
            continue;
        }
        zeros = 0;
        if (!tar_header_ok(h)) {
            if (a->total || longname)
                snprintf(a->note, sizeof(a->note), "A header with a wrong checksum: the listing stops there.");
            break;
        }
        uint64_t size = tar_num(h + 124, 12);
        char type = (char)h[156];
        uint64_t body = off + 512, next = body + ((size + 511) / 512) * 512;
        if (type == 'L' || type == 'K' || type == 'x' || type == 'g') {          /* GNU long name / link, pax headers: they describe the entry after them */
            if (size > 65536) {                                                  /* far bigger than any real name or pax header: skipped */
                off = next;
                continue;
            }
            uint8_t *d = malloc(size + 1);
            if (d && tar_read(s, body, d, size)) {
                if (type == 'L') {
                    free(longname);
                    longname = clean_name(d, cstrlen(d, size), false);
                } else if (type == 'K') {
                    free(longlink);
                    longlink = clean_name(d, cstrlen(d, size), false);
                } else if (type == 'x') {
                    char *pv = pax_value(d, size, "path");
                    if (pv) {
                        free(paxpath);
                        paxpath = clean_name((const uint8_t *)pv, strlen(pv), false);
                        free(pv);
                    }
                    pv = pax_value(d, size, "linkpath");
                    if (pv) {
                        free(longlink);
                        longlink = clean_name((const uint8_t *)pv, strlen(pv), false);
                        free(pv);
                    }
                    pv = pax_value(d, size, "size");
                    if (pv) {
                        paxsize = (int64_t)strtoull(pv, NULL, 10);
                        free(pv);
                    }
                }
            }
            free(d);
            off = next;
            continue;
        }
        char *name;
        if (longname) {
            name = longname;
            longname = NULL;
        } else if (paxpath) {
            name = paxpath;
            paxpath = NULL;
        } else {
            size_t nl = cstrlen(h, 100);
            bool ustar = !memcmp(h + 257, "ustar", 5);
            size_t pl = ustar ? cstrlen(h + 345, 155) : 0;
            uint8_t full[300];
            size_t o = 0;
            if (pl) {
                memcpy(full, h + 345, pl);
                full[pl] = '/';
                o = pl + 1;
            }
            memcpy(full + o, h, nl);
            name = clean_name(full, o + nl, false);
        }
        if (paxsize >= 0) {
            size = (uint64_t)paxsize;
            next = body + ((size + 511) / 512) * 512;
            paxsize = -1;
        }
        bool dir = type == '5' || (name && name[0] && name[strlen(name) - 1] == '/');
        if (dir && name && name[0] && name[strlen(name) - 1] != '/') {
            size_t l = strlen(name);
            name = realloc(name, l + 2);
            name[l] = '/';
            name[l + 1] = 0;
        }
        if (type == '2' || type == '1') {                                       /* a symbolic or hard link: shown with its target */
            char *target = longlink ? longlink : clean_name(h + 157, cstrlen(h + 157, 100), false);
            longlink = NULL;
            size_t l = strlen(name) + strlen(target) + 8;
            name = realloc(name, l);
            strcat(name, type == '2' ? " -> " : " => ");
            strcat(name, target);
            free(target);
            size = 0;
        }
        free(longlink);
        longlink = NULL;
        arc_add(a, max, name, unix_time_text((int64_t)tar_num(h + 136, 12)), (dir || type == '2' || type == '1') ? -1 : (long long)size, -1);
        off = next;
    }
    free(longname);
    free(paxpath);
    free(longlink);
    return 0;
}

/* ---- gzip ---- */
static int gz_list(int fd, uint64_t fsize, const char *path, int max, struct arc_list *a, char *err, size_t errn)
{
    size_t rn = fsize < GZ_CAP ? (size_t)fsize : GZ_CAP;
    uint8_t *in = malloc(rn + 1);
    if (!in || !pread_all(fd, in, rn, 0)) {
        free(in);
        seterr(err, errn, "A gzip file that cannot be read.");
        return -2;
    }
    if (rn < 18 || in[2] != 8) {
        free(in);
        seterr(err, errn, "A damaged gzip file.");
        return -2;
    }
    unsigned flg = in[3];
    size_t p = 10;
    char *fname = NULL;
    if ((flg & 4) && p + 2 <= rn)
        p += 2 + u16(in + p);
    if ((flg & 8)) {
        size_t e = p;
        while (e < rn && in[e])
            e++;
        if (e < rn)
            fname = clean_name(in + p, e - p, false);
        p = e + 1;
    }
    if ((flg & 16)) {
        while (p < rn && in[p])
            p++;
        p++;
    }
    if ((flg & 2))
        p += 2;
    if (p >= rn) {
        free(in);
        free(fname);
        seterr(err, errn, "A damaged gzip file.");
        return -2;
    }
    uint32_t isize = fsize >= 4 && rn == fsize ? u32(in + rn - 4) : 0;       /* the unpacked size modulo 2^32, when the whole file was read */
    size_t cap = rn == fsize && isize && isize < GZ_CAP ? (size_t)isize + 1024 : GZ_CAP;
    uint8_t *out = malloc(cap + 1);
    long got = out ? lm_inflate_prefix(in + p, rn - p, out, cap) : -1;
    free(in);
    if (got < 0) {
        free(out);
        free(fname);
        seterr(err, errn, "A gzip file that does not unpack.");
        return -2;
    }
    bool is_tar = got >= 512 && tar_header_ok(out);
    if (is_tar) {
        snprintf(a->kind, sizeof(a->kind), "TAR.GZ");
        struct tarsrc s = { .fd = -1, .mem = out, .size = (uint64_t)got };
        tar_list(&s, max, a, (size_t)got >= cap);
    } else {
        snprintf(a->kind, sizeof(a->kind), "GZIP");
        a->has_packed = 1;
        char base[256];
        const char *slash = strrchr(path, '/');
        snprintf(base, sizeof(base), "%s", slash ? slash + 1 : path);
        size_t bl = strlen(base);
        if (bl > 3 && !strcmp(base + bl - 3, ".gz"))
            base[bl - 3] = 0;
        long long usize = rn == fsize ? (long long)isize : -1;
        if (rn == fsize && isize < (uint32_t)got)
            usize = got;
        arc_add(a, max, fname ? fname : strdup(base), NULL, usize, (long long)fsize);
        fname = NULL;
    }
    free(fname);
    free(out);
    return 0;
}


/* ---- xz, bzip2, zstd (a single file, or a TAR inside), and the formats that carry such streams: ar / .deb, .rpm (cpio), 7z ---- */
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

enum { CK_NONE, CK_GZ, CK_XZ, CK_BZ2, CK_ZST };

static int comp_kind(const uint8_t *p, size_t n)
{
    if (n >= 2 && p[0] == 0x1f && p[1] == 0x8b)
        return CK_GZ;
    if (n >= 6 && !memcmp(p, "\xfd" "7zXZ\0", 6))
        return CK_XZ;
    if (n >= 10 && !memcmp(p, "BZh", 3) && p[3] >= '1' && p[3] <= '9' && ((!memcmp(p + 4, "\x31\x41\x59\x26\x53\x59", 6)) || !memcmp(p + 4, "\x17\x72\x45\x38\x50\x90", 6)))
        return CK_BZ2;
    if (n >= 4 && !memcmp(p, "\x28\xb5\x2f\xfd", 4))
        return CK_ZST;
    return CK_NONE;
}

static const char *const comp_names[] = { "", "GZIP", "XZ", "BZIP2", "ZSTD" };
static const char *const tar_names[] = { "TAR", "TAR.GZ", "TAR.XZ", "TAR.BZ2", "TAR.ZST" };

/* The first `cap` bytes of a compressed stream in memory (malloc'd, *got = the count; NULL on failure); *complete: the whole stream is in it. */
static uint8_t *unpack_mem(const uint8_t *in, size_t n, int kind, size_t cap, long *got, int *complete, const char **why)
{
    uint8_t *out = malloc(cap + 1);
    if (!out)
        return NULL;
    long r = -1;
    *complete = 0;
    if (kind == CK_XZ) {
        r = lm_xz_prefix(in, n, out, cap, complete, why);
    } else if (kind == CK_BZ2) {
        r = lm_bzip2_prefix(in, n, out, cap, complete);
    } else if (kind == CK_ZST) {
        r = lm_zstd_prefix(in, n, out, cap, complete);
    } else if (kind == CK_GZ && n >= 18 && in[2] == 8) {
        unsigned flg = in[3];
        size_t p = 10;
        if ((flg & 4) && p + 2 <= n)
            p += 2 + u16(in + p);
        if (flg & 8) {
            while (p < n && in[p])
                p++;
            p++;
        }
        if (flg & 16) {
            while (p < n && in[p])
                p++;
            p++;
        }
        if (flg & 2)
            p += 2;
        if (p < n) {
            r = lm_inflate_prefix(in + p, n - p, out, cap);
            *complete = r >= 0 && (size_t)r < cap;
        }
    }
    if (r < 0) {
        free(out);
        return NULL;
    }
    *got = r;
    return out;
}

/* The first `cap` bytes of a compressed stream (gzip, xz, bzip2, zstd by its first bytes) in memory: malloc'd, *got = the count, *complete = whole stream;
 * NULL when it is none of those or cannot be decoded. */
uint8_t *arc_unpack(const uint8_t *in, size_t n, size_t cap, long *got, int *complete, const char **why)
{
    int kind = comp_kind(in, n);
    if (kind == CK_NONE)
        return NULL;
    return unpack_mem(in, n, kind, cap, got, complete, why);
}

/* A compressed file: what is inside is a TAR (listed) or one file (its name is the file's without the compressor's extension). */
static int stream_list(int fd, uint64_t fsize, const char *path, int kind, int max, struct arc_list *a, char *err, size_t errn)
{
    size_t rn = fsize < GZ_CAP ? (size_t)fsize : GZ_CAP;
    uint8_t *in = malloc(rn + 1);
    if (!in || !pread_all(fd, in, rn, 0)) {
        free(in);
        seterr(err, errn, "A file that cannot be read.");
        return -2;
    }
    long got = 0;
    int complete = 0;
    const char *why = NULL;
    uint8_t *out = unpack_mem(in, rn, kind, GZ_CAP, &got, &complete, &why);
    free(in);
    if (!out) {
        seterr(err, errn, why ? why : "A compressed file that does not unpack.");
        return -2;
    }
    if (got >= 512 && tar_header_ok(out)) {
        snprintf(a->kind, sizeof(a->kind), "%s", tar_names[kind]);
        struct tarsrc s = { .fd = -1, .mem = out, .size = (uint64_t)got };
        tar_list(&s, max, a, !complete && (size_t)got >= GZ_CAP);
    } else {
        snprintf(a->kind, sizeof(a->kind), "%s", comp_names[kind]);
        a->has_packed = 1;
        char base[256];
        const char *slash = strrchr(path, '/');
        snprintf(base, sizeof(base), "%s", slash ? slash + 1 : path);
        size_t bl = strlen(base);
        static const char *const exts[] = { "", ".gz", ".xz", ".bz2", ".zst" };
        size_t el = strlen(exts[kind]);
        if (bl > el && !strcmp(base + bl - el, exts[kind]))
            base[bl - el] = 0;
        arc_add(a, max, strdup(base), NULL, complete ? got : -1, (long long)fsize);
        if (!complete)
            snprintf(a->note, sizeof(a->note), "Unpacked only as far as the first 64 MB: the real size is bigger.");
    }
    free(out);
    return 0;
}

/* ---- ar (and .deb) ---- */
static long long dec_field(const uint8_t *p, size_t n)
{
    long long v = 0;
    size_t i = 0;
    while (i < n && p[i] == ' ')
        i++;
    for (; i < n && p[i] >= '0' && p[i] <= '9'; i++)
        v = v * 10 + (p[i] - '0');
    return v;
}

static int ar_list(int fd, uint64_t fsize, int max, struct arc_list *a, char *err, size_t errn)
{
    uint64_t off = 8;
    char *longnames = NULL;
    size_t longn = 0;
    bool deb = false;
    uint64_t data_off = 0, data_size = 0;
    char data_name[96] = "";
    int members = 0;
    for (int guard = 0; guard < 2000000 && off + 60 <= fsize; guard++) {
        uint8_t h[60];
        if (!pread_all(fd, h, 60, off) || h[58] != '`' || h[59] != '\n')
            break;
        long long sz = dec_field(h + 48, 10);
        if (sz < 0 || (uint64_t)sz > fsize - off - 60)
            sz = (long long)(fsize - off - 60);
        char name[260];
        size_t nl = 16;
        memcpy(name, h, 16);
        while (nl > 0 && name[nl - 1] == ' ')
            nl--;
        name[nl] = 0;
        uint64_t body = off + 60;
        long long fsz = sz;
        if (!strcmp(name, "/") || !strcmp(name, "/SYM64/") || !strcmp(name, "__.SYMDEF") || !strcmp(name, "__.SYMDEF SORTED")) {
            off = body + (uint64_t)sz + ((uint64_t)sz & 1);
            continue;
        }
        if (!strcmp(name, "//")) {                                           /* the table of long names */
            if (sz > 0 && sz < (4 << 20)) {
                longnames = malloc((size_t)sz + 1);
                if (longnames && pread_all(fd, longnames, (size_t)sz, body)) {
                    longn = (size_t)sz;
                    longnames[longn] = 0;
                } else {
                    free(longnames);
                    longnames = NULL;
                }
            }
            off = body + (uint64_t)sz + ((uint64_t)sz & 1);
            continue;
        }
        char shown[300];
        if (name[0] == '/' && name[1] >= '0' && name[1] <= '9' && longnames) {   /* GNU: "/NN" is an offset into the table */
            size_t o = (size_t)atol(name + 1);
            size_t e = o;
            while (e < longn && longnames[e] != '/' && longnames[e] != '\n')
                e++;
            snprintf(shown, sizeof(shown), "%.*s", (int)(e > o ? e - o : 0), longnames + (o < longn ? o : longn));
        } else if (!strncmp(name, "#1/", 3)) {                               /* BSD: the name comes first in the data */
            size_t k = (size_t)atol(name + 3);
            if (k > 255 || (long long)k > sz)
                k = 0;
            char t[256] = "";
            if (k && pread_all(fd, t, k, body))
                t[k] = 0;
            snprintf(shown, sizeof(shown), "%s", t);
            body += k;
            fsz -= (long long)k;
        } else {
            if (nl > 0 && name[nl - 1] == '/')
                name[nl - 1] = 0;
            snprintf(shown, sizeof(shown), "%s", name);
        }
        members++;
        if (members == 1 && !strcmp(shown, "debian-binary"))
            deb = true;
        if (deb && !strncmp(shown, "data.tar", 8)) {
            data_off = body;
            data_size = (uint64_t)fsz;
            snprintf(data_name, sizeof(data_name), "%s", shown);
        }
        if (!deb) {
            int64_t mt = (int64_t)dec_field(h + 16, 12);
            arc_add(a, max, clean_name((const uint8_t *)shown, strlen(shown), false), mt > 0 ? unix_time_text(mt) : NULL, fsz, -1);
        }
        off = off + 60 + (uint64_t)sz + ((uint64_t)sz & 1);
    }
    free(longnames);
    if (!deb) {
        snprintf(a->kind, sizeof(a->kind), "AR");
        return 0;
    }
    /* a Debian package: list what the data archive holds */
    snprintf(a->kind, sizeof(a->kind), "DEB");
    if (!data_off) {
        seterr(err, errn, "A Debian package without a data archive.");
        return -2;
    }
    size_t rn = data_size < GZ_CAP ? (size_t)data_size : GZ_CAP;
    uint8_t *in = malloc(rn + 1);
    if (!in || !pread_all(fd, in, rn, data_off)) {
        free(in);
        seterr(err, errn, "A Debian package whose data cannot be read.");
        return -2;
    }
    int kind = comp_kind(in, rn);
    long got = 0;
    int complete = 0;
    const char *why = NULL;
    uint8_t *out;
    if (kind == CK_NONE) {
        out = in;
        got = (long)rn;
        complete = rn == data_size;
        in = NULL;
    } else {
        out = unpack_mem(in, rn, kind, GZ_CAP, &got, &complete, &why);
        free(in);
    }
    if (!out || got < 512 || !tar_header_ok(out)) {
        free(out);
        seterr(err, errn, why ? why : "A Debian package whose data archive cannot be read.");
        return -2;
    }
    struct tarsrc s = { .fd = -1, .mem = out, .size = (uint64_t)got };
    tar_list(&s, max, a, !complete);
    snprintf(a->kind, sizeof(a->kind), "DEB");
    if (!a->note[0])
        snprintf(a->note, sizeof(a->note), "The files of %s inside the package.", data_name);
    free(out);
    return 0;
}

/* ---- RPM: the headers (name, version), then a cpio archive (compressed) ---- */
static int cpio_list(const uint8_t *d, size_t n, int max, struct arc_list *a)
{
    size_t p = 0;
    for (int guard = 0; guard < 3000000 && p + 110 <= n; guard++) {
        if (memcmp(d + p, "070701", 6) && memcmp(d + p, "070702", 6)) {
            snprintf(a->note, sizeof(a->note), "The cpio archive is in a form that is not read (large file format).");
            return 0;
        }
        uint32_t f[13];
        for (int i = 0; i < 13; i++) {
            char t[9];
            memcpy(t, d + p + 6 + 8 * i, 8);
            t[8] = 0;
            char *e;
            f[i] = (uint32_t)strtoul(t, &e, 16);
            if (*e)
                return 0;
        }
        uint32_t mode = f[1], mtime = f[5], fsz = f[6], nsz = f[11];
        if (nsz == 0 || nsz > 4096 || p + 110 + nsz > n)
            return 0;
        const uint8_t *nm = d + p + 110;
        size_t body = (p + 110 + nsz + 3) & ~(size_t)3;
        if (nsz == 11 && !memcmp(nm, "TRAILER!!!", 10))
            return 0;
        size_t nl = nsz - 1;
        const uint8_t *np = nm;
        if (nl >= 2 && np[0] == '.' && np[1] == '/') {
            np += 2;
            nl -= 2;
        } else if (nl >= 1 && np[0] == '/') {
            np++;
            nl--;
        }
        char *name = clean_name(np, nl, false);
        unsigned type = mode & 0170000;
        if (type == 0040000) {
            size_t l = strlen(name);
            name = realloc(name, l + 2);
            name[l] = '/';
            name[l + 1] = 0;
            arc_add(a, max, name, mtime ? unix_time_text(mtime) : NULL, -1, -1);
        } else if (type == 0120000 && body + fsz <= n) {
            char *t = clean_name(d + body, fsz, false);
            char *both = malloc(strlen(name) + strlen(t) + 5);
            sprintf(both, "%s -> %s", name, t);
            free(name);
            free(t);
            arc_add(a, max, both, mtime ? unix_time_text(mtime) : NULL, fsz, -1);
        } else {
            arc_add(a, max, name, mtime ? unix_time_text(mtime) : NULL, fsz, -1);
        }
        p = (body + fsz + 3) & ~(size_t)3;
    }
    return 0;
}

static int rpm_list(int fd, uint64_t fsize, int max, struct arc_list *a, char *err, size_t errn)
{
    uint8_t h[16];
    uint64_t off = 96;
    char pkg[160] = "";
    for (int hdr = 0; hdr < 2; hdr++) {
        if (!pread_all(fd, h, 16, off) || memcmp(h, "\x8e\xad\xe8\x01", 4)) {
            seterr(err, errn, "A damaged RPM header.");
            return -2;
        }
        uint32_t ni = be32(h + 8), hs = be32(h + 12);
        if (ni > 100000 || hs > (64u << 20) || off + 16 + (uint64_t)ni * 16 + hs > fsize) {
            seterr(err, errn, "A damaged RPM header.");
            return -2;
        }
        if (hdr == 1) {
            uint8_t *ix = malloc((size_t)ni * 16 + hs + 1);
            if (ix && pread_all(fd, ix, (size_t)ni * 16 + hs, off + 16)) {
                const uint8_t *store = ix + (size_t)ni * 16;
                char nm[64] = "", ver[48] = "", rel[48] = "", arch[24] = "";
                for (uint32_t i = 0; i < ni; i++) {
                    const uint8_t *e = ix + (size_t)i * 16;
                    uint32_t tag = be32(e), type = be32(e + 4), o = be32(e + 8);
                    if (type != 6 || o >= hs)
                        continue;
                    size_t l = strnlen((const char *)store + o, hs - o);
                    char *dst = tag == 1000 ? nm : tag == 1001 ? ver : tag == 1002 ? rel : tag == 1022 ? arch : NULL;
                    size_t cap = tag == 1000 ? sizeof(nm) : tag == 1001 || tag == 1002 ? sizeof(ver) : sizeof(arch);
                    if (dst)
                        snprintf(dst, cap, "%.*s", (int)l, (const char *)store + o);
                }
                snprintf(pkg, sizeof(pkg), "%s %s-%s %s", nm, ver, rel, arch);
            }
            free(ix);
        }
        off += 16 + (uint64_t)ni * 16 + hs;
        if (hdr == 0)
            off = (off + 7) & ~(uint64_t)7;                                   /* the signature header is padded to 8 bytes */
    }
    if (off >= fsize) {
        seterr(err, errn, "An RPM without a payload.");
        return -2;
    }
    size_t rn = fsize - off < GZ_CAP ? (size_t)(fsize - off) : GZ_CAP;
    uint8_t *in = malloc(rn + 1);
    if (!in || !pread_all(fd, in, rn, off)) {
        free(in);
        seterr(err, errn, "An RPM whose payload cannot be read.");
        return -2;
    }
    int kind = comp_kind(in, rn);
    long got = 0;
    int complete = 0;
    const char *why = NULL;
    uint8_t *out;
    if (kind == CK_NONE) {
        out = in;
        got = (long)rn;
        in = NULL;
    } else {
        out = unpack_mem(in, rn, kind, GZ_CAP, &got, &complete, &why);
        free(in);
    }
    if (!out || got < 110) {
        free(out);
        seterr(err, errn, why ? why : "An RPM whose payload does not unpack.");
        return -2;
    }
    snprintf(a->kind, sizeof(a->kind), "RPM");
    cpio_list(out, (size_t)got, max, a);
    if (!a->note[0])
        snprintf(a->note, sizeof(a->note), "Package %s%s", pkg, kind == CK_NONE || complete ? "" : " (the listing stops at the first 64 MB of the payload)");
    free(out);
    return 0;
}

/* ---- 7z: the (maybe compressed) header lists the files; the contents are never unpacked ---- */
struct sz_rd {
    const uint8_t *p;
    size_t n, pos;
    bool bad;
};

static uint64_t sz_num(struct sz_rd *r)
{
    if (r->pos >= r->n) {
        r->bad = true;
        return 0;
    }
    uint8_t first = r->p[r->pos++], mask = 0x80;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        if (!(first & mask)) {
            uint64_t high = first & (mask - 1u);
            return v | high << (8 * i);
        }
        if (r->pos >= r->n) {
            r->bad = true;
            return 0;
        }
        v |= (uint64_t)r->p[r->pos++] << (8 * i);
        mask >>= 1;
    }
    return v;
}

static uint8_t sz_byte(struct sz_rd *r)
{
    if (r->pos >= r->n) {
        r->bad = true;
        return 0;
    }
    return r->p[r->pos++];
}

static void sz_skip(struct sz_rd *r, uint64_t n)
{
    if (n > r->n - r->pos) {
        r->bad = true;
        r->pos = r->n;
    } else
        r->pos += (size_t)n;
}

/* A bit vector of n bits; *all = 1 when "all defined" was said (then none is read). Returns a malloc'd array of n bytes (0 / 1). */
static uint8_t *sz_bits(struct sz_rd *r, uint64_t n, bool with_all_flag)
{
    uint8_t *v = calloc((size_t)n + 1, 1);
    if (!v)
        return NULL;
    if (with_all_flag && sz_byte(r)) {
        memset(v, 1, (size_t)n);
        return v;
    }
    uint8_t cur = 0, mask = 0;
    for (uint64_t i = 0; i < n && !r->bad; i++) {
        if (!mask) {
            cur = sz_byte(r);
            mask = 0x80;
        }
        v[i] = (cur & mask) != 0;
        mask >>= 1;
    }
    return v;
}

struct sz_coder {
    uint8_t id[8];
    int idlen;
    int nin, nout;
    uint8_t props[16];
    int nprops;
};

struct sz_folder {
    struct sz_coder coder[8];
    int ncoders;
    int nbind;
    uint32_t bind_in[8], bind_out[8];
    int totalout;
    uint64_t unpack[8];                 /* per out stream */
    uint64_t final_size;                /* the folder's result */
    int nsub;
    bool crc_def;                       /* the folder's own CRC was given (then a single substream needs none) */
    uint64_t pack_size_index;
};

struct sz_info {
    uint64_t pack_pos;
    int npack;
    uint64_t *pack_size;
    int nfolders;
    struct sz_folder *folder;
    uint64_t *sub_size;                 /* sizes of all substreams, in order */
    uint64_t nsub_total;
};

static void sz_info_free(struct sz_info *si)
{
    free(si->pack_size);
    free(si->folder);
    free(si->sub_size);
    memset(si, 0, sizeof(*si));
}

static bool sz_streams(struct sz_rd *r, struct sz_info *si)
{
    memset(si, 0, sizeof(*si));
    for (;;) {
        uint8_t id = sz_byte(r);
        if (r->bad)
            return false;
        if (id == 0)
            return true;
        if (id == 6) {                                                       /* PackInfo */
            si->pack_pos = sz_num(r);
            uint64_t np = sz_num(r);
            if (np > 100000) return false;
            si->npack = (int)np;
            free(si->pack_size);                                             /* (a second PackInfo replaces the first) */
            si->pack_size = calloc((size_t)np + 1, sizeof(uint64_t));
            for (;;) {
                uint8_t t = sz_byte(r);
                if (r->bad) return false;
                if (t == 0) break;
                if (t == 9) {
                    for (uint64_t i = 0; i < np; i++)
                        si->pack_size[i] = sz_num(r);
                } else if (t == 10) {
                    uint8_t *d = sz_bits(r, np, true);
                    uint64_t nd = 0;
                    for (uint64_t i = 0; d && i < np; i++) nd += d[i];
                    free(d);
                    sz_skip(r, nd * 4);
                } else
                    return false;
            }
        } else if (id == 7) {                                                /* UnpackInfo: the folders */
            if (sz_byte(r) != 0x0b)
                return false;
            uint64_t nf = sz_num(r);
            if (sz_byte(r) != 0 || nf > 100000)
                return false;
            si->nfolders = (int)nf;
            free(si->folder);
            si->folder = calloc((size_t)nf + 1, sizeof(struct sz_folder));
            for (uint64_t k = 0; k < nf; k++) {
                struct sz_folder *f = &si->folder[k];
                uint64_t nc = sz_num(r);
                if (nc < 1 || nc > 8 || r->bad)
                    return false;
                f->ncoders = (int)nc;
                int totin = 0;
                for (int c = 0; c < f->ncoders; c++) {
                    uint8_t fl = sz_byte(r);
                    struct sz_coder *cd = &f->coder[c];
                    cd->idlen = fl & 15;
                    if (cd->idlen > 8 || (fl & 0x80))
                        return false;
                    for (int i = 0; i < cd->idlen; i++)
                        cd->id[i] = sz_byte(r);
                    cd->nin = cd->nout = 1;
                    if (fl & 0x10) {
                        cd->nin = (int)sz_num(r);
                        cd->nout = (int)sz_num(r);
                        if (cd->nin < 1 || cd->nin > 8 || cd->nout != 1)
                            return false;
                    }
                    if (fl & 0x20) {
                        uint64_t pl = sz_num(r);
                        if (pl > 1000 || r->bad)
                            return false;
                        for (uint64_t i = 0; i < pl; i++) {
                            uint8_t b = sz_byte(r);
                            if (i < sizeof(cd->props))
                                cd->props[i] = b;
                        }
                        cd->nprops = pl < sizeof(cd->props) ? (int)pl : (int)sizeof(cd->props);
                    }
                    totin += cd->nin;
                    f->totalout += cd->nout;
                }
                f->nbind = f->totalout - 1;
                for (int b = 0; b < f->nbind; b++) {
                    f->bind_in[b] = (uint32_t)sz_num(r);
                    f->bind_out[b] = (uint32_t)sz_num(r);
                }
                int npk = totin - f->nbind;
                if (npk > 1)
                    for (int i = 0; i < npk; i++)
                        sz_num(r);
                if (r->bad)
                    return false;
            }
            if (sz_byte(r) != 0x0c)
                return false;
            for (int k = 0; k < si->nfolders; k++) {
                struct sz_folder *f = &si->folder[k];
                for (int o = 0; o < f->totalout; o++)
                    f->unpack[o] = sz_num(r);
                int fin = 0;
                for (int o = 0; o < f->totalout; o++) {                      /* the out stream that no bind pair consumes is the result */
                    bool used = false;
                    for (int b = 0; b < f->nbind; b++)
                        used |= f->bind_out[b] == (uint32_t)o;
                    if (!used)
                        fin = o;
                }
                f->final_size = f->unpack[fin];
            }
            for (;;) {
                uint8_t t = sz_byte(r);
                if (r->bad) return false;
                if (t == 0) break;
                if (t == 10) {
                    uint8_t *d = sz_bits(r, (uint64_t)si->nfolders, true);
                    uint64_t nd = 0;
                    for (int i = 0; d && i < si->nfolders; i++) {
                        nd += d[i];
                        si->folder[i].crc_def = d[i] != 0;
                    }
                    free(d);
                    sz_skip(r, nd * 4);
                } else
                    return false;
            }
        } else if (id == 8) {                                                /* SubStreamsInfo */
            for (int k = 0; k < si->nfolders; k++)
                si->folder[k].nsub = 1;
            uint8_t t = sz_byte(r);
            if (t == 0x0d) {
                for (int k = 0; k < si->nfolders; k++) {
                    uint64_t ns = sz_num(r);
                    if (ns > 10000000) return false;
                    si->folder[k].nsub = (int)ns;
                }
                t = sz_byte(r);
            }
            uint64_t total = 0;
            for (int k = 0; k < si->nfolders; k++)
                total += (uint64_t)si->folder[k].nsub;
            if (total > 20000000) return false;
            si->nsub_total = total;
            free(si->sub_size);
            si->sub_size = calloc((size_t)total + 1, sizeof(uint64_t));
            if (!si->sub_size) return false;
            uint64_t at = 0;
            if (t == 9) {
                for (int k = 0; k < si->nfolders; k++) {
                    uint64_t sum = 0;
                    int ns = si->folder[k].nsub;
                    for (int i = 0; i < ns; i++) {
                        if (i == ns - 1) {
                            si->sub_size[at++] = si->folder[k].final_size >= sum ? si->folder[k].final_size - sum : 0;
                        } else {
                            uint64_t v = sz_num(r);
                            si->sub_size[at++] = v;
                            sum += v;
                        }
                    }
                }
                t = sz_byte(r);
            } else {
                for (int k = 0; k < si->nfolders; k++) {
                    int ns = si->folder[k].nsub;
                    for (int i = 0; i < ns; i++)
                        si->sub_size[at++] = i == 0 ? si->folder[k].final_size : 0;
                }
            }
            while (t != 0 && !r->bad) {                                      /* digests */
                if (t != 10)
                    return false;
                uint64_t need = 0;
                for (int k = 0; k < si->nfolders; k++)
                    if (!(si->folder[k].nsub == 1 && si->folder[k].crc_def))
                        need += (uint64_t)si->folder[k].nsub;
                uint8_t *d = sz_bits(r, need, true);
                uint64_t nd = 0;
                for (uint64_t i = 0; d && i < need; i++) nd += d[i];
                free(d);
                sz_skip(r, nd * 4);
                t = sz_byte(r);
            }
            if (r->bad) return false;
        } else {
            return false;
        }
    }
}

/* The folder's unpacked bytes for a header (a single LZMA / LZMA2 / copy coder only). */
static uint8_t *sz_unpack_header(int fd, uint64_t fsize, const struct sz_info *si, size_t *outn)
{
    if (si->nfolders < 1 || si->npack < 1)
        return NULL;
    const struct sz_folder *f = &si->folder[0];
    if (f->ncoders != 1 || f->final_size > (64u << 20) || si->pack_size[0] > (64u << 20))
        return NULL;
    uint64_t at = 32 + si->pack_pos;
    if (at + si->pack_size[0] > fsize)
        return NULL;
    uint8_t *in = malloc((size_t)si->pack_size[0] + 1);
    uint8_t *out = malloc((size_t)f->final_size + 1);
    long r = -1;
    if (in && out && pread_all(fd, in, (size_t)si->pack_size[0], at)) {
        const struct sz_coder *c = &f->coder[0];
        int complete;
        if (c->idlen == 3 && !memcmp(c->id, "\x03\x01\x01", 3) && c->nprops >= 5)
            r = lm_lzma_raw(in, (size_t)si->pack_size[0], c->props, out, (size_t)f->final_size);
        else if (c->idlen == 1 && c->id[0] == 0x21)
            r = lm_lzma2_raw(in, (size_t)si->pack_size[0], out, (size_t)f->final_size, &complete);
        else if (c->idlen == 1 && c->id[0] == 0 && si->pack_size[0] >= f->final_size) {
            memcpy(out, in, (size_t)f->final_size);
            r = (long)f->final_size;
        }
    }
    free(in);
    if (r < 0 || (uint64_t)r != f->final_size) {
        free(out);
        return NULL;
    }
    *outn = (size_t)r;
    return out;
}

static int sz_list(int fd, uint64_t fsize, int max, struct arc_list *a, char *err, size_t errn)
{
    uint8_t sh[32];
    if (!pread_all(fd, sh, 32, 0)) {
        seterr(err, errn, "A damaged 7z file.");
        return -2;
    }
    uint64_t noff = u64(sh + 12), nsz = u64(sh + 20);
    if (nsz == 0) {
        snprintf(a->kind, sizeof(a->kind), "7Z");
        return 0;                                                            /* an empty archive */
    }
    if (noff > fsize || nsz > fsize || 32 + noff + nsz > fsize || nsz > (64u << 20)) {
        seterr(err, errn, "A 7z file that is cut off or damaged.");
        return -2;
    }
    uint8_t *hdr = malloc((size_t)nsz + 1);
    if (!hdr || !pread_all(fd, hdr, (size_t)nsz, 32 + noff)) {
        free(hdr);
        seterr(err, errn, "A 7z header that cannot be read.");
        return -2;
    }
    size_t hn = (size_t)nsz;
    for (int depth = 0; depth < 4 && hn > 0 && hdr[0] == 0x17; depth++) {      /* an encoded header: unpack it */
        struct sz_rd r = { hdr, hn, 1, false };
        struct sz_info si;
        bool ok = sz_streams(&r, &si);
        size_t un = 0;
        uint8_t *nh = ok ? sz_unpack_header(fd, fsize, &si, &un) : NULL;
        sz_info_free(&si);
        free(hdr);
        if (!nh) {
            seterr(err, errn, "A 7z archive whose header is encrypted or packed in a way that is not read.");
            return -2;
        }
        hdr = nh;
        hn = un;
    }
    if (hn == 0 || hdr[0] != 0x01) {
        free(hdr);
        seterr(err, errn, "A damaged 7z header.");
        return -2;
    }
    struct sz_rd r = { hdr, hn, 1, false };
    struct sz_info si = { 0 };
    uint64_t nfiles = 0;
    uint8_t *empty_stream = NULL, *empty_file = NULL, *mt_def = NULL;
    uint64_t *mtimes = NULL;
    char **names = NULL;
    uint32_t *attr = NULL;
    uint8_t *attr_def = NULL;
    bool ok = true;
    for (;;) {
        uint8_t id = sz_byte(&r);
        if (r.bad) { ok = false; break; }
        if (id == 0)
            break;
        if (id == 2) {                                                       /* archive properties: skip */
            for (;;) {
                uint8_t t = sz_byte(&r);
                if (r.bad || t == 0) break;
                sz_skip(&r, sz_num(&r));
            }
        } else if (id == 3) {                                                /* additional streams: skip */
            struct sz_info tmp;
            ok = sz_streams(&r, &tmp);
            sz_info_free(&tmp);
            if (!ok) break;
        } else if (id == 4) {
            ok = sz_streams(&r, &si);
            if (!ok) break;
        } else if (id == 5) {
            nfiles = sz_num(&r);
            if (nfiles > 10000000 || r.bad) { ok = false; break; }
            names = calloc((size_t)nfiles + 1, sizeof(char *));
            mtimes = calloc((size_t)nfiles + 1, sizeof(uint64_t));
            attr = calloc((size_t)nfiles + 1, sizeof(uint32_t));
            for (;;) {
                uint8_t t = sz_byte(&r);
                if (r.bad) { ok = false; break; }
                if (t == 0)
                    break;
                uint64_t psz = sz_num(&r);
                if (psz > r.n - r.pos) { ok = false; break; }
                size_t pend = r.pos + (size_t)psz;
                struct sz_rd pr = { hdr, pend, r.pos, false };
                if (t == 0x0e) {
                    free(empty_stream);
                    empty_stream = sz_bits(&pr, nfiles, false);
                } else if (t == 0x0f) {
                    uint64_t ne = 0;
                    for (uint64_t i = 0; empty_stream && i < nfiles; i++) ne += empty_stream[i];
                    free(empty_file);
                    empty_file = sz_bits(&pr, ne, false);
                } else if (t == 0x11) {
                    if (sz_byte(&pr) == 0) {
                        for (uint64_t i = 0; i < nfiles && !pr.bad; i++) {
                            size_t st = pr.pos;
                            while (pr.pos + 1 < pend && (pr.p[pr.pos] || pr.p[pr.pos + 1]))
                                pr.pos += 2;
                            if (pr.pos + 1 >= pend) { pr.bad = true; break; }
                            size_t units = (pr.pos - st) / 2;
                            char *buf = malloc(units * 4 + 1);
                            size_t bn = 0;
                            for (size_t u = 0; buf && u < units; u++) {
                                unsigned cp = pr.p[st + 2 * u] | (unsigned)pr.p[st + 2 * u + 1] << 8;
                                if (cp >= 0xd800 && cp < 0xdc00 && u + 1 < units) {
                                    unsigned lo = pr.p[st + 2 * u + 2] | (unsigned)pr.p[st + 2 * u + 3] << 8;
                                    if (lo >= 0xdc00 && lo < 0xe000) {
                                        cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                                        u++;
                                    }
                                }
                                put_utf8(buf, &bn, units * 4, cp);
                            }
                            if (buf) buf[bn] = 0;
                            names[i] = buf ? clean_name((const uint8_t *)buf, bn, false) : strdup("");
                            free(buf);
                            pr.pos += 2;
                        }
                    }
                } else if (t == 0x14) {
                    free(mt_def);
                    mt_def = sz_bits(&pr, nfiles, true);
                    if (sz_byte(&pr) == 0)
                        for (uint64_t i = 0; mt_def && i < nfiles && !pr.bad; i++)
                            if (mt_def[i]) {
                                if (pr.pos + 8 > pend) { pr.bad = true; break; }
                                mtimes[i] = u64(pr.p + pr.pos);
                                pr.pos += 8;
                            }
                } else if (t == 0x15) {
                    free(attr_def);
                    attr_def = sz_bits(&pr, nfiles, true);
                    if (sz_byte(&pr) == 0)
                        for (uint64_t i = 0; attr_def && i < nfiles && !pr.bad; i++)
                            if (attr_def[i]) {
                                if (pr.pos + 4 > pend) { pr.bad = true; break; }
                                attr[i] = u32(pr.p + pr.pos);
                                pr.pos += 4;
                            }
                }
                r.pos = pend;
            }
            if (!ok)
                break;
        } else {
            ok = false;
            break;
        }
    }
    int rc = 0;
    if (!ok || r.bad) {
        seterr(err, errn, "A damaged 7z header.");
        rc = -2;
    } else {
        snprintf(a->kind, sizeof(a->kind), "7Z");
        uint64_t sub = 0, ei = 0;
        for (uint64_t i = 0; i < nfiles; i++) {
            bool is_empty = empty_stream && empty_stream[i];
            bool dir = false;
            long long size = 0;
            if (is_empty) {
                bool ef = empty_file && empty_file[ei];
                ei++;
                dir = !ef;
            } else {
                size = sub < si.nsub_total ? (long long)si.sub_size[sub] : -1;
                sub++;
            }
            if (attr && attr_def && attr_def[i] && (attr[i] & 0x10))
                dir = true;
            char *nm = names[i] ? names[i] : strdup("");
            names[i] = NULL;
            if (dir) {
                size_t l = strlen(nm);
                nm = realloc(nm, l + 2);
                nm[l] = '/';
                nm[l + 1] = 0;
            }
            char *mt = NULL;
            if (mt_def && mt_def[i] && mtimes[i] >= 116444736000000000ull)
                mt = unix_time_text((int64_t)(mtimes[i] / 10000000ull) - 11644473600ll);
            arc_add(a, max, nm, mt, dir ? -1 : size, -1);
        }
        uint64_t packed = 0;
        for (int i = 0; i < si.npack; i++)
            packed += si.pack_size[i];
        if (nfiles && packed)
            snprintf(a->note, sizeof(a->note), "Packed size in all: %llu bytes (the format packs files together, so there is none per file).", (unsigned long long)packed);
    }
    for (uint64_t i = 0; names && i < nfiles; i++)
        free(names[i]);
    free(names);
    free(mtimes);
    free(attr);
    free(attr_def);
    free(empty_stream);
    free(empty_file);
    free(mt_def);
    sz_info_free(&si);
    free(hdr);
    return rc;
}

int arc_read(const char *path, int max_entries, struct arc_list *a, char *err, size_t errn)
{
    memset(a, 0, sizeof(*a));
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    struct stat st;
    uint8_t h[512];
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 20) {
        close(fd);
        return -1;
    }
    uint64_t fsize = (uint64_t)st.st_size;
    size_t hn = fsize < 512 ? (size_t)fsize : 512;
    if (!pread_all(fd, h, hn, 0)) {
        close(fd);
        return -1;
    }
    int r = -1;
    if (hn >= 4 && (!memcmp(h, "PK\3\4", 4) || !memcmp(h, "PK\5\6", 4))) {
        r = zip_list(fd, fsize, max_entries, a, err, errn);
    } else if (hn >= 2 && h[0] == 0x1f && h[1] == 0x8b) {
        r = gz_list(fd, fsize, path, max_entries, a, err, errn);
    } else if (hn >= 10 && comp_kind(h, hn) >= CK_XZ) {
        r = stream_list(fd, fsize, path, comp_kind(h, hn), max_entries, a, err, errn);
    } else if (hn >= 8 && !memcmp(h, "!<arch>\n", 8)) {
        r = ar_list(fd, fsize, max_entries, a, err, errn);
    } else if (hn >= 4 && !memcmp(h, "\xed\xab\xee\xdb", 4) && fsize > 200) {
        r = rpm_list(fd, fsize, max_entries, a, err, errn);
    } else if (hn >= 32 && !memcmp(h, "7z\xbc\xaf\x27\x1c", 6)) {
        r = sz_list(fd, fsize, max_entries, a, err, errn);
    } else if (hn == 512 && tar_header_ok(h)) {
        snprintf(a->kind, sizeof(a->kind), "TAR");
        struct tarsrc s = { .fd = fd, .mem = NULL, .size = fsize };
        r = tar_list(&s, max_entries, a, false);
    }
    close(fd);
    if (r != 0)
        arc_free(a);
    return r;
}
