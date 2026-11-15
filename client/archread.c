#include "archread.h"

#include "../src/img.h"

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
