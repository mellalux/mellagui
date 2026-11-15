#include "arcio.h"

#include "archread.h"
#include "decomp.h"
#include "deflate.h"
#include "../src/img.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef S_IFLNK
#define S_IFLNK 0120000
#endif

static uint32_t u16(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8; }
static uint32_t u32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t u64(const uint8_t *p) { return u32(p) | (uint64_t)u32(p + 4) << 32; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static void seterr(char *err, size_t n, const char *msg)
{
    if (err && n)
        snprintf(err, n, "%s", msg);
}

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

/* ---- progress ---- */
void aio_progress_init(struct aio_progress *p)
{
    memset(p, 0, sizeof(*p));
    pthread_mutex_t *m = malloc(sizeof(*m));
    if (m)
        pthread_mutex_init(m, NULL);
    p->lock = m;
}

void aio_progress_free(struct aio_progress *p)
{
    if (p->lock) {
        pthread_mutex_destroy(p->lock);
        free(p->lock);
        p->lock = NULL;
    }
}

static void progress_name(struct aio_progress *p, const char *s)
{
    if (!p || !p->lock)
        return;
    pthread_mutex_lock(p->lock);
    snprintf(p->current, sizeof(p->current), "%s", s);
    pthread_mutex_unlock(p->lock);
}

void aio_progress_current(struct aio_progress *p, char *out, size_t n)
{
    if (!p->lock) {
        out[0] = 0;
        return;
    }
    pthread_mutex_lock(p->lock);
    snprintf(out, n, "%s", p->current);
    pthread_mutex_unlock(p->lock);
}

static void report_err(struct aio_report *r, const char *fmt, const char *name, const char *why)
{
    if (!r)
        return;
    r->failed++;
    if (r->nerr < 6) {
        char t[260];
        snprintf(t, sizeof(t), fmt, name, why);
        snprintf(r->err[r->nerr++], sizeof(r->err[0]), "%s", t);
    }
}

struct sz7;
static void sz7_free(struct sz7 *z);
struct src {
    const uint8_t *mem;
    uint64_t len;
    int fd;
    uint64_t foff;
    uint8_t *own;
};
static const char *sz7_slice(struct aio *a, const struct aio_entry *e, struct src *s);

/* ---- entries ---- */
static struct aio_entry *add_entry(struct aio *a, int *cap)
{
    if (a->n >= 5000000)
        return NULL;
    if (a->n == *cap) {
        *cap = *cap ? *cap * 2 : 64;
        struct aio_entry *t = realloc(a->e, (size_t)*cap * sizeof(*t));
        if (!t)
            return NULL;
        a->e = t;
    }
    struct aio_entry *e = &a->e[a->n++];
    memset(e, 0, sizeof(*e));
    e->size = -1;
    e->packed = -1;
    return e;
}

/* Sets the name from stored bytes: `raw` is kept to extract with, `path` is the readable form. */
static void set_name(struct aio_entry *e, const uint8_t *raw, size_t n, bool dos)
{
    e->raw_path = malloc(n + 1);
    if (e->raw_path) {
        memcpy(e->raw_path, raw, n);
        e->raw_path[n] = 0;
        e->raw_len = n;
    }
    e->path = arc_clean_name(raw, n, dos);
}

void aio_close(struct aio *a)
{
    if (!a)
        return;
    for (int i = 0; i < a->n; i++) {
        free(a->e[i].path);
        free(a->e[i].link);
        free(a->e[i].raw_path);
    }
    free(a->e);
    free(a->mem);
    sz7_free(a->sz);
    if (a->fd >= 0)
        close(a->fd);
    free(a);
}

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

/* ---- ZIP ---- */
static int zip_open_aio(struct aio *a, char *err, size_t errn)
{
    uint64_t fsize = a->fsize;
    size_t tail = fsize < 65557 ? (size_t)fsize : 65557;
    uint8_t *t = malloc(tail);
    if (!t || !pread_all(a->fd, t, tail, fsize - tail)) {
        free(t);
        seterr(err, errn, "A ZIP file that cannot be read.");
        return -2;
    }
    long eo = -1;
    for (long i = (long)tail - 22; i >= 0; i--)
        if (!memcmp(t + i, "PK\5\6", 4) && (size_t)i + 22 + u16(t + i + 20) <= tail) {
            eo = i;
            break;
        }
    if (eo < 0) {
        free(t);
        seterr(err, errn, "No ZIP end record: the file is cut off or damaged.");
        return -2;
    }
    uint64_t total = u16(t + eo + 10), cdsize = u32(t + eo + 12), cdoff = u32(t + eo + 16);
    if (total == 0xffff || cdsize == 0xffffffffu || cdoff == 0xffffffffu) {
        if (eo >= 20 && !memcmp(t + eo - 20, "PK\6\7", 4)) {
            uint64_t z64 = u64(t + eo - 20 + 8);
            uint8_t z[56];
            if (pread_all(a->fd, z, 56, z64) && !memcmp(z, "PK\6\6", 4)) {
                total = u64(z + 32);
                cdsize = u64(z + 40);
                cdoff = u64(z + 48);
            }
        }
    }
    free(t);
    if (cdsize > (256u << 20) || cdoff > fsize || cdsize > fsize - cdoff) {
        seterr(err, errn, "A ZIP directory that is damaged.");
        return -2;
    }
    uint8_t *cd = malloc((size_t)cdsize + 1);
    if (!cd || !pread_all(a->fd, cd, (size_t)cdsize, cdoff)) {
        free(cd);
        seterr(err, errn, "A ZIP directory that cannot be read.");
        return -2;
    }
    snprintf(a->kind, sizeof(a->kind), "ZIP");
    a->mode = 0;
    int cap = 0;
    size_t p = 0;
    while (p + 46 <= cdsize && !memcmp(cd + p, "PK\1\2", 4)) {
        unsigned made = u16(cd + p + 4), flags = u16(cd + p + 8), method = u16(cd + p + 10);
        uint32_t dosd = u16(cd + p + 14), dost = u16(cd + p + 12);
        uint64_t crc = u32(cd + p + 16), cs = u32(cd + p + 20), us = u32(cd + p + 24), lo = u32(cd + p + 42);
        size_t nl = u16(cd + p + 28), xl = u16(cd + p + 30), cl = u16(cd + p + 32);
        uint32_t ext = u32(cd + p + 38);
        if (p + 46 + nl + xl + cl > cdsize)
            break;
        const uint8_t *name = cd + p + 46, *x = name + nl;
        int64_t mt = 0;
        const uint8_t *uname = NULL;
        size_t unl = 0;
        for (size_t k = 0; k + 4 <= xl;) {
            unsigned id = u16(x + k), sz = u16(x + k + 2);
            if (k + 4 + sz > xl)
                break;
            const uint8_t *d = x + k + 4;
            if (id == 1) {                                                  /* ZIP64: the fields that were 0xFFFFFFFF, in this order */
                size_t o = 0;
                if (us == 0xffffffffu && o + 8 <= sz) { us = u64(d + o); o += 8; }
                if (cs == 0xffffffffu && o + 8 <= sz) { cs = u64(d + o); o += 8; }
                if (lo == 0xffffffffu && o + 8 <= sz) { lo = u64(d + o); o += 8; }
            } else if (id == 0x5455 && sz >= 5 && (d[0] & 1)) {
                mt = (int64_t)(int32_t)u32(d + 1);
            } else if (id == 0x7075 && sz >= 5 && d[0] == 1 && valid_utf8(d + 5, sz - 5)) {
                if (lm_crc32(0, name, nl) == u32(d + 1)) {
                    uname = d + 5;
                    unl = sz - 5;
                }
            }
            k += 4 + sz;
        }
        struct aio_entry *e = add_entry(a, &cap);
        if (!e)
            break;
        if (uname)
            set_name(e, uname, unl, false);
        else
            set_name(e, name, nl, !(flags & 0x800) && !valid_utf8(name, nl));
        if (e->raw_path && !uname && !(flags & 0x800) && !valid_utf8(name, nl)) {   /* extract with the UTF-8 form of a DOS name */
            free(e->raw_path);
            e->raw_path = (uint8_t *)strdup(e->path ? e->path : "");
            e->raw_len = strlen((char *)e->raw_path);
        }
        if (!mt) {
            struct tm tm = { .tm_year = (int)((dosd >> 9) & 127) + 80, .tm_mon = (int)((dosd >> 5) & 15) - 1, .tm_mday = (int)(dosd & 31), .tm_hour = (int)(dost >> 11),
                             .tm_min = (int)((dost >> 5) & 63), .tm_sec = (int)(dost & 31) * 2, .tm_isdst = -1 };
            if (tm.tm_mon >= 0 && tm.tm_mday > 0)
                mt = (int64_t)mktime(&tm);
        }
        e->mtime = mt;
        e->size = (int64_t)us;
        e->packed = (int64_t)cs;
        e->csize = cs;
        e->off = lo;
        e->method = (int)method;
        e->crc = (uint32_t)crc;
        e->has_crc = 1;
        uint32_t mode = (made >> 8) == 3 ? ext >> 16 : 0;
        e->mode = mode & 07777;
        bool dir = (nl && name[nl - 1] == '/') || (ext & 0x10) || (mode & 0170000) == 0040000;
        e->type = dir ? AIO_DIR : (mode & 0170000) == S_IFLNK ? AIO_SYMLINK : AIO_FILE;
        if (dir)
            e->size = -1;
        if (flags & 1) {
            e->method = -1;                                                  /* encrypted */
        }
        p += 46 + nl + xl + cl;
    }
    free(cd);
    a->extractable = 1;
    return 0;
}

/* ---- the unpacked streams: TAR, cpio, ar ---- */
static bool mem_or_file_read(const struct aio *a, uint64_t off, void *buf, size_t n)
{
    if (a->mem) {
        if (off + n > a->memn)
            return false;
        memcpy(buf, a->mem + off, n);
        return true;
    }
    return pread_all(a->fd, buf, n, off);
}

static bool tar_header_ok(const uint8_t *h)
{
    unsigned long sum = 0, want = 0;
    for (int i = 0; i < 512; i++)
        sum += (i >= 148 && i < 156) ? ' ' : h[i];
    int i = 148;
    while (i < 156 && h[i] == ' ')
        i++;
    for (; i < 156 && h[i] >= '0' && h[i] <= '7'; i++)
        want = want * 8 + (unsigned long)(h[i] - '0');
    return want == sum;
}

static uint64_t tar_num(const uint8_t *p, size_t n)
{
    if (p[0] & 0x80) {                                                      /* GNU base-256 */
        uint64_t v = p[0] & 0x7f;
        for (size_t i = 1; i < n; i++)
            v = v << 8 | p[i];
        return v;
    }
    uint64_t v = 0;
    size_t i = 0;
    while (i < n && p[i] == ' ')
        i++;
    for (; i < n && p[i] >= '0' && p[i] <= '7'; i++)
        v = v * 8 + (uint64_t)(p[i] - '0');
    return v;
}

static size_t cstr_len(const uint8_t *p, size_t n)
{
    size_t l = 0;
    while (l < n && p[l])
        l++;
    return l;
}

static int tar_parse(struct aio *a, char *err, size_t errn)
{
    uint64_t size = a->mem ? a->memn : a->fsize, off = 0;
    int cap = 0;
    uint8_t *longname = NULL, *longlink = NULL;
    size_t lnn = 0, lln = 0;
    uint8_t *paxpath = NULL, *paxlink = NULL;
    size_t ppn = 0, pln = 0;
    int64_t paxsize = -1, paxmtime = 0;
    int zeros = 0, rc = 0;
    for (long guard = 0; guard < 6000000 && off + 512 <= size; guard++) {
        uint8_t h[512];
        if (!mem_or_file_read(a, off, h, 512))
            break;
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
            if (a->n == 0) {
                rc = -1;
                seterr(err, errn, "Not a TAR archive.");
            } else
                snprintf(a->note, sizeof(a->note), "The archive is damaged after %d entries.", a->n);
            break;
        }
        uint64_t sz = tar_num(h + 124, 12);
        int type = h[156];
        uint64_t body = off + 512;
        if (paxsize >= 0)
            sz = (uint64_t)paxsize;
        if (type == 'L' || type == 'K' || type == 'x') {
            if (sz > (1u << 20) || body + sz > size) {
                snprintf(a->note, sizeof(a->note), "The archive is damaged after %d entries.", a->n);
                break;
            }
            uint8_t *d = malloc((size_t)sz + 1);
            if (!d || !mem_or_file_read(a, body, d, (size_t)sz)) {
                free(d);
                break;
            }
            d[sz] = 0;
            if (type == 'L') {
                free(longname);
                longname = d;
                lnn = cstr_len(d, (size_t)sz);
            } else if (type == 'K') {
                free(longlink);
                longlink = d;
                lln = cstr_len(d, (size_t)sz);
            } else {
                size_t q = 0;
                while (q < sz) {
                    size_t len = 0, k = q;
                    while (k < sz && d[k] >= '0' && d[k] <= '9')
                        len = len * 10 + (size_t)(d[k++] - '0');
                    if (k >= sz || d[k] != ' ' || len == 0 || q + len > sz)
                        break;
                    const uint8_t *rec = d + k + 1;
                    size_t rl = len - (k - q) - 1;
                    if (rl > 0 && rec[rl - 1] == '\n')
                        rl--;
                    const uint8_t *eq = memchr(rec, '=', rl);
                    if (eq) {
                        size_t kl = (size_t)(eq - rec), vl = rl - kl - 1;
                        if (kl == 4 && !memcmp(rec, "path", 4)) {
                            free(paxpath);
                            paxpath = malloc(vl + 1);
                            if (paxpath) { memcpy(paxpath, eq + 1, vl); paxpath[vl] = 0; ppn = vl; }
                        } else if (kl == 8 && !memcmp(rec, "linkpath", 8)) {
                            free(paxlink);
                            paxlink = malloc(vl + 1);
                            if (paxlink) { memcpy(paxlink, eq + 1, vl); paxlink[vl] = 0; pln = vl; }
                        } else if (kl == 4 && !memcmp(rec, "size", 4)) {
                            paxsize = (int64_t)strtoull((const char *)eq + 1, NULL, 10);
                        } else if (kl == 5 && !memcmp(rec, "mtime", 5)) {
                            paxmtime = (int64_t)strtod((const char *)eq + 1, NULL);
                        }
                    }
                    q += len;
                }
                free(d);
            }
            off = body + ((sz + 511) & ~(uint64_t)511);
            continue;
        }
        if (type == 'g') {
            off = body + ((sz + 511) & ~(uint64_t)511);
            continue;
        }
        uint8_t name[512];
        size_t nl;
        if (paxpath) {
            nl = ppn < sizeof(name) - 1 ? ppn : sizeof(name) - 1;
            memcpy(name, paxpath, nl);
        } else if (longname) {
            nl = lnn < sizeof(name) - 1 ? lnn : sizeof(name) - 1;
            memcpy(name, longname, nl);
        } else {
            nl = cstr_len(h, 100);
            memcpy(name, h, nl);
            if (!memcmp(h + 257, "ustar", 5) && h[345]) {
                size_t pl = cstr_len(h + 345, 155);
                memmove(name + pl + 1, name, nl);
                memcpy(name, h + 345, pl);
                name[pl] = '/';
                nl += pl + 1;
            }
        }
        struct aio_entry *e = add_entry(a, &cap);
        if (!e)
            break;
        set_name(e, name, nl, false);
        e->mode = (uint32_t)tar_num(h + 100, 8) & 07777;
        e->mtime = paxmtime ? paxmtime : (int64_t)tar_num(h + 136, 12);
        e->off = body;
        e->size = (int64_t)sz;
        switch (type) {
        case '0': case 0: case '7': e->type = AIO_FILE; break;
        case '5': e->type = AIO_DIR; e->size = -1; break;
        case '2': e->type = AIO_SYMLINK; break;
        case '1': e->type = AIO_HARDLINK; break;
        default: e->type = AIO_OTHER; break;
        }
        if (e->type == AIO_SYMLINK || e->type == AIO_HARDLINK) {
            const uint8_t *lk = paxlink ? paxlink : longlink ? longlink : h + 157;
            size_t ll = paxlink ? pln : longlink ? lln : cstr_len(h + 157, 100);
            e->link = arc_clean_name(lk, ll, 0);
            e->size = -1;
            sz = 0;
        }
        free(longname); free(longlink); free(paxpath); free(paxlink);
        longname = longlink = paxpath = paxlink = NULL;
        lnn = lln = ppn = pln = 0;
        paxsize = -1;
        paxmtime = 0;
        if (e->type == AIO_DIR || e->type == AIO_OTHER)
            sz = e->type == AIO_DIR ? 0 : sz;
        if (body + sz > size && e->type == AIO_FILE) {
            snprintf(a->note, sizeof(a->note), "The archive is cut off in the middle of %.60s.", e->path ? e->path : "");
            e->size = (int64_t)(size > body ? size - body : 0);
            break;
        }
        off = body + ((sz + 511) & ~(uint64_t)511);
    }
    free(longname); free(longlink); free(paxpath); free(paxlink);
    a->extractable = rc == 0;
    return rc;
}

static int cpio_parse(struct aio *a)
{
    const uint8_t *d = a->mem;
    size_t n = a->memn, p = 0;
    int cap = 0;
    for (int guard = 0; guard < 5000000 && p + 110 <= n; guard++) {
        if (memcmp(d + p, "070701", 6) && memcmp(d + p, "070702", 6)) {
            snprintf(a->note, sizeof(a->note), "The cpio archive is in a form that is not read.");
            break;
        }
        uint32_t f[13];
        bool ok = true;
        for (int i = 0; i < 13 && ok; i++) {
            char t[9];
            memcpy(t, d + p + 6 + 8 * i, 8);
            t[8] = 0;
            char *e;
            f[i] = (uint32_t)strtoul(t, &e, 16);
            ok = !*e;
        }
        if (!ok)
            break;
        uint32_t mode = f[1], mtime = f[5], fsz = f[6], nsz = f[11];
        if (nsz == 0 || nsz > 4096 || p + 110 + nsz > n)
            break;
        const uint8_t *nm = d + p + 110;
        size_t body = (p + 110 + nsz + 3) & ~(size_t)3;
        if (nsz == 11 && !memcmp(nm, "TRAILER!!!", 10))
            break;
        if (body + fsz > n) {
            snprintf(a->note, sizeof(a->note), "The cpio archive is cut off.");
            break;
        }
        struct aio_entry *e = add_entry(a, &cap);
        if (!e)
            break;
        set_name(e, nm, nsz - 1, false);
        e->mode = mode & 07777;
        e->mtime = mtime;
        e->off = body;
        e->size = fsz;
        unsigned type = mode & 0170000;
        e->type = type == 0040000 ? AIO_DIR : type == 0120000 ? AIO_SYMLINK : type == 0100000 ? AIO_FILE : AIO_OTHER;
        if (e->type == AIO_SYMLINK) {
            e->link = arc_clean_name(d + body, fsz, 0);
            e->size = -1;
        } else if (e->type == AIO_DIR)
            e->size = -1;
        p = (body + fsz + 3) & ~(size_t)3;
    }
    a->extractable = 1;
    return 0;
}

static int64_t dec_field(const uint8_t *p, size_t n)
{
    int64_t v = 0;
    size_t i = 0;
    while (i < n && p[i] == ' ')
        i++;
    for (; i < n && p[i] >= '0' && p[i] <= '9'; i++)
        v = v * 10 + (p[i] - '0');
    return v;
}

/* ar members from memory; `deb` = look for the data archive instead (returns its bytes through *data / *dn). */
static int ar_parse(struct aio *a, const uint8_t *d, size_t n, bool want_deb, const uint8_t **data, size_t *dn)
{
    size_t off = 8;
    char *longnames = NULL;
    size_t longn = 0;
    int cap = 0, members = 0;
    bool deb = false;
    for (int guard = 0; guard < 2000000 && off + 60 <= n; guard++) {
        const uint8_t *h = d + off;
        if (h[58] != '`' || h[59] != '\n')
            break;
        int64_t sz = dec_field(h + 48, 10);
        if (sz < 0 || (uint64_t)sz > n - off - 60)
            sz = (int64_t)(n - off - 60);
        char name[260];
        size_t nl = 16;
        memcpy(name, h, 16);
        while (nl > 0 && name[nl - 1] == ' ')
            nl--;
        name[nl] = 0;
        const uint8_t *body = h + 60;
        int64_t fsz = sz;
        size_t next = off + 60 + (size_t)sz + ((size_t)sz & 1);
        if (!strcmp(name, "/") || !strcmp(name, "/SYM64/") || !strcmp(name, "__.SYMDEF") || !strcmp(name, "__.SYMDEF SORTED")) {
            off = next;
            continue;
        }
        if (!strcmp(name, "//")) {
            free(longnames);
            longnames = malloc((size_t)sz + 1);
            if (longnames) {
                memcpy(longnames, body, (size_t)sz);
                longnames[sz] = 0;
                longn = (size_t)sz;
            }
            off = next;
            continue;
        }
        char shown[300];
        if (name[0] == '/' && name[1] >= '0' && name[1] <= '9' && longnames) {
            size_t o = (size_t)atol(name + 1), e = o;
            while (e < longn && longnames[e] != '/' && longnames[e] != '\n')
                e++;
            snprintf(shown, sizeof(shown), "%.*s", (int)(e > o ? e - o : 0), longnames + (o < longn ? o : longn));
        } else if (!strncmp(name, "#1/", 3)) {
            size_t k = (size_t)atol(name + 3);
            if (k > 255 || (int64_t)k > sz)
                k = 0;
            snprintf(shown, sizeof(shown), "%.*s", (int)k, (const char *)body);
            body += k;
            fsz -= (int64_t)k;
        } else {
            if (nl > 0 && name[nl - 1] == '/')
                name[nl - 1] = 0;
            snprintf(shown, sizeof(shown), "%s", name);
        }
        members++;
        if (members == 1 && !strcmp(shown, "debian-binary"))
            deb = true;
        if (want_deb && deb && !strncmp(shown, "data.tar", 8)) {
            *data = body;
            *dn = (size_t)fsz;
            free(longnames);
            return 1;
        }
        if (!want_deb) {
            struct aio_entry *e = add_entry(a, &cap);
            if (e) {
                set_name(e, (const uint8_t *)shown, strlen(shown), false);
                e->type = AIO_FILE;
                e->size = fsz;
                e->mtime = dec_field(h + 16, 12);
                e->mode = (uint32_t)strtoul((const char *)(h + 40), NULL, 8) & 07777;
                e->off = (uint64_t)(body - d);
            }
        }
        off = next;
    }
    free(longnames);
    a->extractable = 1;
    return 0;
}

/* The decompressed bytes of a whole file (grows the buffer until the stream ends, up to AIO_MAX_UNPACKED). */
static uint8_t *unpack_whole(const uint8_t *in, size_t n, size_t *outn, char *err, size_t errn)
{
    size_t cap = 64u << 20;
    for (;;) {
        long got = 0;
        int complete = 0;
        const char *why = NULL;
        uint8_t *out = arc_unpack(in, n, cap, &got, &complete, &why);
        if (!out) {
            seterr(err, errn, why ? why : "The compressed data cannot be unpacked.");
            return NULL;
        }
        if (complete || (size_t)got < cap) {
            *outn = (size_t)got;
            return out;
        }
        free(out);
        if (cap >= AIO_MAX_UNPACKED) {
            seterr(err, errn, "The archive unpacks to more than 1 GB: too big to open here.");
            return NULL;
        }
        cap *= 4;
        if (cap > AIO_MAX_UNPACKED)
            cap = AIO_MAX_UNPACKED;
    }
}

static bool comp_magic(const uint8_t *h, size_t n)
{
    return (n >= 2 && h[0] == 0x1f && h[1] == 0x8b) || (n >= 6 && !memcmp(h, "\xfd" "7zXZ\0", 6)) || (n >= 4 && !memcmp(h, "\x28\xb5\x2f\xfd", 4)) ||
           (n >= 10 && !memcmp(h, "BZh", 3) && h[3] >= '1' && h[3] <= '9');
}


/* ======================================================================== 7z ================================================================================ */
struct sx_rd {
    const uint8_t *p;
    size_t n, pos;
    bool bad;
};

static uint64_t sx_num(struct sx_rd *r)
{
    if (r->pos >= r->n) {
        r->bad = true;
        return 0;
    }
    uint8_t first = r->p[r->pos++], mask = 0x80;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        if (!(first & mask))
            return v | (uint64_t)(first & (mask - 1u)) << (8 * i);
        if (r->pos >= r->n) {
            r->bad = true;
            return 0;
        }
        v |= (uint64_t)r->p[r->pos++] << (8 * i);
        mask >>= 1;
    }
    return v;
}

static uint8_t sx_byte(struct sx_rd *r)
{
    if (r->pos >= r->n) {
        r->bad = true;
        return 0;
    }
    return r->p[r->pos++];
}

static void sx_skip(struct sx_rd *r, uint64_t n)
{
    if (n > r->n - r->pos) {
        r->bad = true;
        r->pos = r->n;
    } else
        r->pos += (size_t)n;
}

/* A bit vector of n bits (0 / 1 bytes); with the "all defined" byte first when asked. */
static uint8_t *sx_bits(struct sx_rd *r, uint64_t n, bool all_flag)
{
    uint8_t *v = calloc((size_t)n + 1, 1);
    if (!v)
        return NULL;
    if (all_flag && sx_byte(r)) {
        memset(v, 1, (size_t)n);
        return v;
    }
    uint8_t cur = 0, mask = 0;
    for (uint64_t i = 0; i < n && !r->bad; i++) {
        if (!mask) {
            cur = sx_byte(r);
            mask = 0x80;
        }
        v[i] = (cur & mask) != 0;
        mask >>= 1;
    }
    return v;
}

struct szcoder {
    uint8_t id[8];
    int idlen, nin, nout, nprops;
    uint8_t props[24];
};

struct szfolder {
    struct szcoder c[8];
    int nc, nbind, npacked, totalin, totalout;
    uint32_t bind_in[8], bind_out[8], packed_idx[8];
    uint64_t unpack[8];
    uint64_t final_size;
    bool crc_def;
    uint32_t crc;
    int nsub;
    int first_pack;
};

struct sz7 {
    struct szfolder *f;
    int nf;
    uint64_t pack_pos;
    uint64_t *pack_size, *pack_off;
    int npack;
    uint64_t *sub_size;
    uint32_t *sub_crc;
    uint8_t *sub_crc_def;
    uint64_t nsub_total;
    /* the folder that is unpacked now */
    int cache_idx;
    uint8_t *cache;
    size_t cache_n;
    int fail_idx;                       /* a folder that did not unpack: not tried again for every file in it */
    char fail_why[120];
};

static void sz7_free(struct sz7 *z)
{
    if (!z)
        return;
    free(z->f);
    free(z->pack_size);
    free(z->pack_off);
    free(z->sub_size);
    free(z->sub_crc);
    free(z->sub_crc_def);
    free(z->cache);
    free(z);
}

/* StreamsInfo (pack info, folders, substreams) into z. */
static bool sx_streams(struct sx_rd *r, struct sz7 *z)
{
    for (;;) {
        uint8_t id = sx_byte(r);
        if (r->bad)
            return false;
        if (id == 0)
            return true;
        if (id == 6) {
            z->pack_pos = sx_num(r);
            uint64_t np = sx_num(r);
            if (np > 100000 || r->bad)
                return false;
            z->npack = (int)np;
            free(z->pack_size);
            z->pack_size = calloc((size_t)np + 1, sizeof(uint64_t));
            for (;;) {
                uint8_t t = sx_byte(r);
                if (r->bad)
                    return false;
                if (t == 0)
                    break;
                if (t == 9) {
                    for (uint64_t i = 0; i < np; i++)
                        z->pack_size[i] = sx_num(r);
                } else if (t == 10) {
                    uint8_t *d = sx_bits(r, np, true);
                    uint64_t nd = 0;
                    for (uint64_t i = 0; d && i < np; i++)
                        nd += d[i];
                    free(d);
                    sx_skip(r, nd * 4);
                } else
                    return false;
            }
        } else if (id == 7) {
            if (sx_byte(r) != 0x0b)
                return false;
            uint64_t nf = sx_num(r);
            if (sx_byte(r) != 0 || nf > 100000 || r->bad)
                return false;
            z->nf = (int)nf;
            free(z->f);
            z->f = calloc((size_t)nf + 1, sizeof(struct szfolder));
            if (!z->f)
                return false;
            int packs = 0;
            for (uint64_t k = 0; k < nf; k++) {
                struct szfolder *f = &z->f[k];
                uint64_t nc = sx_num(r);
                if (nc < 1 || nc > 8 || r->bad)
                    return false;
                f->nc = (int)nc;
                for (int c = 0; c < f->nc; c++) {
                    uint8_t fl = sx_byte(r);
                    struct szcoder *cd = &f->c[c];
                    cd->idlen = fl & 15;
                    if (cd->idlen > 8 || (fl & 0x80))
                        return false;
                    for (int i = 0; i < cd->idlen; i++)
                        cd->id[i] = sx_byte(r);
                    cd->nin = cd->nout = 1;
                    if (fl & 0x10) {
                        cd->nin = (int)sx_num(r);
                        cd->nout = (int)sx_num(r);
                        if (cd->nin < 1 || cd->nin > 8 || cd->nout != 1)
                            return false;
                    }
                    if (fl & 0x20) {
                        uint64_t pl = sx_num(r);
                        if (pl > 1000 || r->bad)
                            return false;
                        for (uint64_t i = 0; i < pl; i++) {
                            uint8_t b = sx_byte(r);
                            if (i < sizeof(cd->props))
                                cd->props[i] = b;
                        }
                        cd->nprops = pl < sizeof(cd->props) ? (int)pl : (int)sizeof(cd->props);
                    }
                    f->totalin += cd->nin;
                    f->totalout += cd->nout;
                }
                f->nbind = f->totalout - 1;
                for (int b = 0; b < f->nbind; b++) {
                    f->bind_in[b] = (uint32_t)sx_num(r);
                    f->bind_out[b] = (uint32_t)sx_num(r);
                }
                f->npacked = f->totalin - f->nbind;
                if (f->npacked < 1 || f->npacked > 8)
                    return false;
                if (f->npacked == 1) {
                    for (int i = 0; i < f->totalin; i++) {                  /* the one input that no bind pair feeds */
                        bool bound = false;
                        for (int b = 0; b < f->nbind; b++)
                            bound |= f->bind_in[b] == (uint32_t)i;
                        if (!bound)
                            f->packed_idx[0] = (uint32_t)i;
                    }
                } else
                    for (int i = 0; i < f->npacked; i++)
                        f->packed_idx[i] = (uint32_t)sx_num(r);
                f->first_pack = packs;
                packs += f->npacked;
                if (r->bad)
                    return false;
            }
            if (sx_byte(r) != 0x0c)
                return false;
            for (uint64_t k = 0; k < nf; k++) {
                struct szfolder *f = &z->f[k];
                for (int o = 0; o < f->totalout; o++)
                    f->unpack[o] = sx_num(r);
                int fin = 0;
                for (int o = 0; o < f->totalout; o++) {
                    bool used = false;
                    for (int b = 0; b < f->nbind; b++)
                        used |= f->bind_out[b] == (uint32_t)o;
                    if (!used)
                        fin = o;
                }
                f->final_size = f->unpack[fin];
            }
            for (;;) {
                uint8_t t = sx_byte(r);
                if (r->bad)
                    return false;
                if (t == 0)
                    break;
                if (t == 10) {
                    uint8_t *d = sx_bits(r, nf, true);
                    for (uint64_t i = 0; d && i < nf; i++)
                        if (d[i]) {
                            if (r->pos + 4 > r->n) {
                                r->bad = true;
                                break;
                            }
                            z->f[i].crc_def = true;
                            z->f[i].crc = u32(r->p + r->pos);
                            r->pos += 4;
                        }
                    free(d);
                } else
                    return false;
            }
        } else if (id == 8) {
            for (int k = 0; k < z->nf; k++)
                z->f[k].nsub = 1;
            uint8_t t = sx_byte(r);
            if (t == 0x0d) {
                for (int k = 0; k < z->nf; k++) {
                    uint64_t ns = sx_num(r);
                    if (ns > 10000000)
                        return false;
                    z->f[k].nsub = (int)ns;
                }
                t = sx_byte(r);
            }
            uint64_t total = 0;
            for (int k = 0; k < z->nf; k++)
                total += (uint64_t)z->f[k].nsub;
            if (total > 20000000)
                return false;
            z->nsub_total = total;
            z->sub_size = calloc((size_t)total + 1, sizeof(uint64_t));
            z->sub_crc = calloc((size_t)total + 1, sizeof(uint32_t));
            z->sub_crc_def = calloc((size_t)total + 1, 1);
            if (!z->sub_size || !z->sub_crc || !z->sub_crc_def)
                return false;
            uint64_t at = 0;
            for (int k = 0; k < z->nf; k++) {
                struct szfolder *f = &z->f[k];
                uint64_t sum = 0;
                for (int i = 0; i < f->nsub; i++) {
                    if (t == 9 && i < f->nsub - 1) {
                        uint64_t v = sx_num(r);
                        z->sub_size[at + (uint64_t)i] = v;
                        sum += v;
                    } else if (i == f->nsub - 1)
                        z->sub_size[at + (uint64_t)i] = f->final_size >= sum ? f->final_size - sum : 0;
                }
                if (f->nsub == 1 && f->crc_def) {
                    z->sub_crc[at] = f->crc;
                    z->sub_crc_def[at] = 1;
                }
                at += (uint64_t)f->nsub;
            }
            if (t == 9)
                t = sx_byte(r);
            while (t != 0 && !r->bad) {
                if (t != 10)
                    return false;
                uint64_t need = 0;
                for (int k = 0; k < z->nf; k++)
                    if (!(z->f[k].nsub == 1 && z->f[k].crc_def))
                        need += (uint64_t)z->f[k].nsub;
                uint8_t *d = sx_bits(r, need, true);
                uint64_t di = 0;
                at = 0;
                for (int k = 0; k < z->nf && !r->bad; k++) {
                    struct szfolder *f = &z->f[k];
                    if (f->nsub == 1 && f->crc_def) {
                        at += 1;
                        continue;
                    }
                    for (int i = 0; i < f->nsub; i++, at++, di++)
                        if (d && d[di]) {
                            if (r->pos + 4 > r->n) {
                                r->bad = true;
                                break;
                            }
                            z->sub_crc[at] = u32(r->p + r->pos);
                            z->sub_crc_def[at] = 1;
                            r->pos += 4;
                        }
                }
                free(d);
                t = sx_byte(r);
            }
            if (r->bad)
                return false;
        } else
            return false;
    }
}

/* ---- the filters and the folder decoder ---- */
static uint32_t sx_be32(const uint8_t *p) { return be32(p); }

static void bcj_x86(uint8_t *data, size_t size)
{
    static const uint8_t allowed[8] = { 1, 1, 1, 0, 1, 0, 0, 0 }, bitnum[8] = { 0, 1, 2, 2, 3, 3, 3, 3 };
#define T86(b) ((b) == 0 || (b) == 0xff)
    if (size < 5)
        return;
    uint32_t ip = 5, prev_mask = 0;
    size_t pos = 0, prev_pos = (size_t)0 - 1;
    for (;;) {
        uint8_t *p = data + pos, *limit = data + size - 4;
        for (; p < limit; p++)
            if ((*p & 0xfe) == 0xe8)
                break;
        pos = (size_t)(p - data);
        if (p >= limit)
            break;
        prev_pos = pos - prev_pos;
        if (prev_pos > 3)
            prev_mask = 0;
        else {
            prev_mask = (prev_mask << ((int)prev_pos - 1)) & 7;
            if (prev_mask != 0) {
                uint8_t b = p[4 - bitnum[prev_mask]];
                if (!allowed[prev_mask] || T86(b)) {
                    prev_pos = pos;
                    prev_mask = ((prev_mask << 1) & 7) | 1;
                    pos++;
                    continue;
                }
            }
        }
        prev_pos = pos;
        if (T86(p[4])) {
            uint32_t src = (uint32_t)p[4] << 24 | (uint32_t)p[3] << 16 | (uint32_t)p[2] << 8 | p[1], dest;
            for (;;) {
                dest = src - (ip + (uint32_t)pos);
                if (prev_mask == 0)
                    break;
                int index = bitnum[prev_mask] * 8;
                uint8_t b = (uint8_t)(dest >> (24 - index));
                if (!T86(b))
                    break;
                src = dest ^ ((1u << (32 - index)) - 1);
            }
            p[4] = (uint8_t)(~(((dest >> 24) & 1) - 1));
            p[3] = (uint8_t)(dest >> 16);
            p[2] = (uint8_t)(dest >> 8);
            p[1] = (uint8_t)dest;
            pos += 5;
        } else {
            prev_mask = ((prev_mask << 1) & 7) | 1;
            pos++;
        }
    }
#undef T86
}

static void bcj_arm(uint8_t *d, size_t n)
{
    for (size_t i = 0; i + 4 <= n; i += 4)
        if (d[i + 3] == 0xeb) {
            uint32_t src = ((uint32_t)d[i + 2] << 16 | (uint32_t)d[i + 1] << 8 | d[i]) << 2;
            uint32_t dest = (src - ((uint32_t)i + 8)) >> 2;
            d[i + 2] = (uint8_t)(dest >> 16);
            d[i + 1] = (uint8_t)(dest >> 8);
            d[i] = (uint8_t)dest;
        }
}

static void bcj_armt(uint8_t *d, size_t n)
{
    for (size_t i = 0; i + 4 <= n; i += 2)
        if ((d[i + 1] & 0xf8) == 0xf0 && (d[i + 3] & 0xf8) == 0xf8) {
            uint32_t src = (((uint32_t)d[i + 1] & 7) << 19 | (uint32_t)d[i] << 11 | ((uint32_t)d[i + 3] & 7) << 8 | d[i + 2]) << 1;
            uint32_t dest = (src - ((uint32_t)i + 4)) >> 1;
            d[i + 1] = (uint8_t)(0xf0 | ((dest >> 19) & 7));
            d[i] = (uint8_t)(dest >> 11);
            d[i + 3] = (uint8_t)(0xf8 | ((dest >> 8) & 7));
            d[i + 2] = (uint8_t)dest;
            i += 2;
        }
}

static void bcj_ppc(uint8_t *d, size_t n)
{
    for (size_t i = 0; i + 4 <= n; i += 4)
        if ((d[i] >> 2) == 0x12 && (d[i + 3] & 3) == 1) {
            uint32_t src = ((uint32_t)(d[i] & 3) << 24) | (uint32_t)d[i + 1] << 16 | (uint32_t)d[i + 2] << 8 | (uint32_t)(d[i + 3] & ~3u);
            uint32_t dest = src - (uint32_t)i;
            d[i] = (uint8_t)(0x48 | ((dest >> 24) & 3));
            d[i + 1] = (uint8_t)(dest >> 16);
            d[i + 2] = (uint8_t)(dest >> 8);
            d[i + 3] = (uint8_t)((d[i + 3] & 3) | (dest & ~3u));
        }
}

static void bcj_sparc(uint8_t *d, size_t n)
{
    for (size_t i = 0; i + 4 <= n; i += 4)
        if ((d[i] == 0x40 && (d[i + 1] & 0xc0) == 0x00) || (d[i] == 0x7f && (d[i + 1] & 0xc0) == 0xc0)) {
            uint32_t src = sx_be32(d + i) << 2, dest = (src - (uint32_t)i) >> 2;
            dest = (((0 - ((dest >> 22) & 1)) << 22) & 0x3fffffffu) | (dest & 0x3fffff) | 0x40000000u;
            d[i] = (uint8_t)(dest >> 24);
            d[i + 1] = (uint8_t)(dest >> 16);
            d[i + 2] = (uint8_t)(dest >> 8);
            d[i + 3] = (uint8_t)dest;
        }
}

/* The unpacked bytes of coder `ci` of folder `f` (malloc'd, `usize` long) or NULL with a reason. */
static uint8_t *sx_coder_out(const struct aio *a, const struct sz7 *z, const struct szfolder *f, int ci, int depth, const char **why)
{
    if (depth > 8) {
        *why = "a chain of coders that is too long";
        return NULL;
    }
    const struct szcoder *c = &f->c[ci];
    if (c->nin != 1) {                                                      /* BCJ2 and the like */
        *why = "a filter (BCJ2) that is not supported";
        return NULL;
    }
    int in_base = 0;
    for (int k = 0; k < ci; k++)
        in_base += f->c[k].nin;
    uint8_t *in = NULL;
    size_t inn = 0;
    int bi = -1;
    for (int b = 0; b < f->nbind; b++)
        if (f->bind_in[b] == (uint32_t)in_base)
            bi = b;
    if (bi >= 0) {
        int src = (int)f->bind_out[bi];                                      /* each coder has one output: its index */
        if (src < 0 || src >= f->nc) {
            *why = "a damaged coder graph";
            return NULL;
        }
        in = sx_coder_out(a, z, f, src, depth + 1, why);
        inn = (size_t)f->unpack[src];
    } else {
        int j = -1;
        for (int k = 0; k < f->npacked; k++)
            if (f->packed_idx[k] == (uint32_t)in_base)
                j = k;
        if (j < 0 || f->first_pack + j >= z->npack) {
            *why = "a damaged coder graph";
            return NULL;
        }
        uint64_t sz = z->pack_size[f->first_pack + j], off = 32 + z->pack_off[f->first_pack + j];
        if (sz > AIO_MAX_UNPACKED || off + sz > a->fsize) {
            *why = "the packed data is cut off";
            return NULL;
        }
        in = malloc((size_t)sz + 1);
        inn = (size_t)sz;
        if (in && !pread_all(a->fd, in, inn, off)) {
            free(in);
            *why = "the packed data cannot be read";
            return NULL;
        }
    }
    if (!in) {
        if (!*why)
            *why = "out of memory";
        return NULL;
    }
    uint64_t usize = f->unpack[ci];
    if (usize > AIO_MAX_UNPACKED) {
        free(in);
        *why = "the block unpacks to more than 1 GB: too big to open here";
        return NULL;
    }
    uint8_t *out = NULL;
    uint64_t id = 0;
    for (int k = 0; k < c->idlen; k++)
        id = id << 8 | c->id[k];
    int complete = 0;
    long r = -1;
    switch (id) {
    case 0x00:                                                               /* copy */
        out = in;
        in = NULL;
        r = (long)(inn < usize ? inn : usize);
        if (inn < usize)
            r = -1;
        break;
    case 0x030101:
    case 0x21:
    case 0x040108:
    case 0x040202:
        out = malloc((size_t)usize + 1);
        if (!out)
            break;
        if (id == 0x030101 && c->nprops >= 5)
            r = lm_lzma_raw(in, inn, c->props, out, (size_t)usize);
        else if (id == 0x21)
            r = lm_lzma2_raw(in, inn, out, (size_t)usize, &complete);
        else if (id == 0x040108)
            r = lm_inflate_raw(in, inn, out, (size_t)usize) == 0 ? (long)usize : -1;
        else if (id == 0x040202)
            r = lm_bzip2_prefix(in, inn, out, (size_t)usize, &complete);
        break;
    case 0x03:                                                               /* delta */
    case 0x04:
    case 0x03030103:
    case 0x03030501:
    case 0x03030701:
    case 0x03030205:
    case 0x03030805:
        out = in;
        in = NULL;
        if (inn < usize) {
            r = -1;
            break;
        }
        r = (long)usize;
        if (id == 0x03) {
            size_t dist = (c->nprops ? c->props[0] : 0) + 1u;
            for (size_t i = dist; i < usize; i++)
                out[i] = (uint8_t)(out[i] + out[i - dist]);
        } else if (id == 0x04 || id == 0x03030103)
            bcj_x86(out, (size_t)usize);
        else if (id == 0x03030501)
            bcj_arm(out, (size_t)usize);
        else if (id == 0x03030701)
            bcj_armt(out, (size_t)usize);
        else if (id == 0x03030205)
            bcj_ppc(out, (size_t)usize);
        else
            bcj_sparc(out, (size_t)usize);
        break;
    case 0x06f10701:
        *why = "the archive is encrypted";
        break;
    case 0x030401:
        *why = "PPMd compression is not supported";
        break;
    default:
        *why = "a compression method that is not supported";
        break;
    }
    free(in);
    if (r < 0 || (uint64_t)r != usize || !out) {
        if (!*why)
            *why = "the packed data is damaged";
        free(out);
        return NULL;
    }
    return out;
}

/* The unpacked bytes of folder fi (the main output). */
static uint8_t *sx_folder_data(const struct aio *a, const struct sz7 *z, int fi, size_t *n, const char **why)
{
    const struct szfolder *f = &z->f[fi];
    int fin = 0;
    for (int o = 0; o < f->totalout; o++) {
        bool used = false;
        for (int b = 0; b < f->nbind; b++)
            used |= f->bind_out[b] == (uint32_t)o;
        if (!used)
            fin = o;
    }
    *why = NULL;
    uint8_t *out = sx_coder_out(a, z, f, fin, 0, why);
    if (out)
        *n = (size_t)f->unpack[fin];
    return out;
}

static void sx_pack_offsets(struct sz7 *z)
{
    free(z->pack_off);
    z->pack_off = calloc((size_t)z->npack + 1, sizeof(uint64_t));
    uint64_t off = z->pack_pos;
    for (int i = 0; z->pack_off && i < z->npack; i++) {
        z->pack_off[i] = off;
        off += z->pack_size[i];
    }
}

/* The bytes of a 7z entry: its folder is unpacked (once, the last one stays in memory) and the entry is a slice of it. */
static const char *sz7_slice(struct aio *a, const struct aio_entry *e, struct src *s)
{
    struct sz7 *z = a->sz;
    if (!z || e->method < 0 || e->method >= z->nf)
        return "the data of the entry is missing";
    if (z->fail_idx == e->method)
        return z->fail_why;
    if (z->cache_idx != e->method) {
        free(z->cache);
        z->cache = NULL;
        z->cache_idx = -1;
        const char *why = NULL;
        size_t n = 0;
        uint8_t *b = sx_folder_data(a, z, e->method, &n, &why);
        if (!b) {
            z->fail_idx = e->method;
            snprintf(z->fail_why, sizeof(z->fail_why), "%s", why ? why : "the data does not unpack");
            return z->fail_why;
        }
        z->cache = b;
        z->cache_n = n;
        z->cache_idx = e->method;
    }
    if (e->size < 0 || e->off + (uint64_t)e->size > z->cache_n)
        return "the data is cut off";
    s->mem = z->cache + e->off;
    s->len = (uint64_t)e->size;
    return NULL;
}

static int sz_open_aio(struct aio *a, char *err, size_t errn)
{
    uint8_t sh[32];
    if (!pread_all(a->fd, sh, 32, 0)) {
        seterr(err, errn, "A damaged 7z file.");
        return -2;
    }
    uint64_t noff = u64(sh + 12), nsz = u64(sh + 20);
    snprintf(a->kind, sizeof(a->kind), "7Z");
    a->mode = 2;
    a->extractable = 1;
    if (nsz == 0)
        return 0;                                                            /* an empty archive */
    if (noff > a->fsize || nsz > a->fsize || 32 + noff + nsz > a->fsize || nsz > (64u << 20)) {
        seterr(err, errn, "A 7z file that is cut off or damaged.");
        return -2;
    }
    uint8_t *hdr = malloc((size_t)nsz + 1);
    if (!hdr || !pread_all(a->fd, hdr, (size_t)nsz, 32 + noff)) {
        free(hdr);
        seterr(err, errn, "A 7z header that cannot be read.");
        return -2;
    }
    size_t hn = (size_t)nsz;
    for (int depth = 0; depth < 4 && hn > 0 && hdr[0] == 0x17; depth++) {      /* an encoded header: unpack it with the folder decoder */
        struct sz7 *t = calloc(1, sizeof(*t));
        struct sx_rd r = { hdr, hn, 1, false };
        bool ok = t && sx_streams(&r, t) && t->nf >= 1;
        uint8_t *nh = NULL;
        size_t un = 0;
        const char *why = NULL;
        if (ok) {
            sx_pack_offsets(t);
            nh = sx_folder_data(a, t, 0, &un, &why);
            if (nh && t->f[0].crc_def && lm_crc32(0, nh, un) != t->f[0].crc) {
                free(nh);
                nh = NULL;
                why = "the header's checksum does not match";
            }
        }
        sz7_free(t);
        free(hdr);
        if (!nh) {
            seterr(err, errn, why ? why : "A 7z header that cannot be unpacked.");
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
    struct sx_rd r = { hdr, hn, 1, false };
    struct sz7 *z = calloc(1, sizeof(*z));
    z->cache_idx = -1;
    z->fail_idx = -1;
    uint64_t nfiles = 0;
    uint8_t *empty_stream = NULL, *empty_file = NULL, *mt_def = NULL, *attr_def = NULL;
    uint64_t *mtimes = NULL;
    uint32_t *attr = NULL;
    char **names = NULL;
    bool ok = z != NULL;
    while (ok) {
        uint8_t id = sx_byte(&r);
        if (r.bad) {
            ok = false;
            break;
        }
        if (id == 0)
            break;
        if (id == 2) {
            for (;;) {
                uint8_t t = sx_byte(&r);
                if (r.bad || t == 0)
                    break;
                sx_skip(&r, sx_num(&r));
            }
        } else if (id == 3) {
            struct sz7 tmp;
            memset(&tmp, 0, sizeof(tmp));
            ok = sx_streams(&r, &tmp);
            free(tmp.f); free(tmp.pack_size); free(tmp.sub_size); free(tmp.sub_crc); free(tmp.sub_crc_def);
        } else if (id == 4) {
            ok = sx_streams(&r, z);
            sx_pack_offsets(z);
        } else if (id == 5) {
            nfiles = sx_num(&r);
            if (nfiles > 5000000 || r.bad) {
                ok = false;
                break;
            }
            names = calloc((size_t)nfiles + 1, sizeof(char *));
            mtimes = calloc((size_t)nfiles + 1, sizeof(uint64_t));
            attr = calloc((size_t)nfiles + 1, sizeof(uint32_t));
            for (;;) {
                uint8_t t = sx_byte(&r);
                if (r.bad) {
                    ok = false;
                    break;
                }
                if (t == 0)
                    break;
                uint64_t psz = sx_num(&r);
                if (psz > r.n - r.pos) {
                    ok = false;
                    break;
                }
                size_t pend = r.pos + (size_t)psz;
                struct sx_rd pr = { hdr, pend, r.pos, false };
                if (t == 0x0e) {
                    free(empty_stream);
                    empty_stream = sx_bits(&pr, nfiles, false);
                } else if (t == 0x0f) {
                    uint64_t ne = 0;
                    for (uint64_t i = 0; empty_stream && i < nfiles; i++)
                        ne += empty_stream[i];
                    free(empty_file);
                    empty_file = sx_bits(&pr, ne, false);
                } else if (t == 0x11) {
                    if (sx_byte(&pr) == 0)
                        for (uint64_t i = 0; i < nfiles && !pr.bad; i++) {
                            size_t st = pr.pos;
                            while (pr.pos + 1 < pend && (pr.p[pr.pos] || pr.p[pr.pos + 1]))
                                pr.pos += 2;
                            if (pr.pos + 1 >= pend) {
                                pr.bad = true;
                                break;
                            }
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
                                if (cp < 0x80)
                                    buf[bn++] = (char)cp;
                                else if (cp < 0x800) {
                                    buf[bn++] = (char)(0xc0 | cp >> 6);
                                    buf[bn++] = (char)(0x80 | (cp & 0x3f));
                                } else if (cp < 0x10000) {
                                    buf[bn++] = (char)(0xe0 | cp >> 12);
                                    buf[bn++] = (char)(0x80 | ((cp >> 6) & 0x3f));
                                    buf[bn++] = (char)(0x80 | (cp & 0x3f));
                                } else {
                                    buf[bn++] = (char)(0xf0 | cp >> 18);
                                    buf[bn++] = (char)(0x80 | ((cp >> 12) & 0x3f));
                                    buf[bn++] = (char)(0x80 | ((cp >> 6) & 0x3f));
                                    buf[bn++] = (char)(0x80 | (cp & 0x3f));
                                }
                            }
                            if (buf)
                                buf[bn] = 0;
                            names[i] = buf;
                            pr.pos += 2;
                        }
                } else if (t == 0x14) {
                    free(mt_def);
                    mt_def = sx_bits(&pr, nfiles, true);
                    if (sx_byte(&pr) == 0)
                        for (uint64_t i = 0; mt_def && i < nfiles && !pr.bad; i++)
                            if (mt_def[i]) {
                                if (pr.pos + 8 > pend) {
                                    pr.bad = true;
                                    break;
                                }
                                mtimes[i] = u64(pr.p + pr.pos);
                                pr.pos += 8;
                            }
                } else if (t == 0x15) {
                    free(attr_def);
                    attr_def = sx_bits(&pr, nfiles, true);
                    if (sx_byte(&pr) == 0)
                        for (uint64_t i = 0; attr_def && i < nfiles && !pr.bad; i++)
                            if (attr_def[i]) {
                                if (pr.pos + 4 > pend) {
                                    pr.bad = true;
                                    break;
                                }
                                attr[i] = u32(pr.p + pr.pos);
                                pr.pos += 4;
                            }
                }
                r.pos = pend;
            }
        } else {
            ok = false;
        }
    }
    int rc = 0;
    if (!ok || r.bad) {
        seterr(err, errn, "A damaged 7z header.");
        rc = -2;
    } else {
        int cap = 0;
        uint64_t sub = 0, ei = 0;
        int fi = 0, in_folder = 0;
        uint64_t foff = 0;
        for (uint64_t i = 0; i < nfiles; i++) {
            struct aio_entry *e = add_entry(a, &cap);
            if (!e)
                break;
            const char *nm = names[i] ? names[i] : "";
            set_name(e, (const uint8_t *)nm, strlen(nm), false);
            if (mt_def && mt_def[i] && mtimes[i] >= 116444736000000000ull)
                e->mtime = (int64_t)(mtimes[i] / 10000000ull) - 11644473600ll;
            uint32_t at = attr && attr_def && attr_def[i] ? attr[i] : 0;
            uint32_t um = (at & 0x8000) ? at >> 16 : 0;                      /* the Unix mode 7-Zip keeps in the top half */
            e->mode = um & 07777;
            bool empty = empty_stream && empty_stream[i];
            bool dir = (at & 0x10) || (um & 0170000) == 0040000;
            if (empty) {
                bool ef = empty_file && empty_file[ei];
                ei++;
                dir = dir || !ef;
                e->type = dir ? AIO_DIR : AIO_FILE;
                e->size = dir ? -1 : 0;
                continue;
            }
            e->type = (um & 0170000) == 0120000 ? AIO_SYMLINK : AIO_FILE;
            while (fi < z->nf && in_folder >= z->f[fi].nsub) {
                fi++;
                in_folder = 0;
                foff = 0;
            }
            if (fi >= z->nf || sub >= z->nsub_total) {
                e->size = -1;
                e->method = -1;
                continue;
            }
            e->method = fi;
            e->off = foff;
            e->size = (int64_t)z->sub_size[sub];
            if (z->sub_crc_def && z->sub_crc_def[sub]) {
                e->crc = z->sub_crc[sub];
                e->has_crc = 1;
            }
            foff += z->sub_size[sub];
            sub++;
            in_folder++;
        }
    }
    for (uint64_t i = 0; names && i < nfiles; i++)
        free(names[i]);
    free(names); free(mtimes); free(attr); free(attr_def); free(empty_stream); free(empty_file); free(mt_def);
    free(hdr);
    if (rc < 0)
        sz7_free(z);
    else
        a->sz = z;
    return rc;
}

int aio_open(const char *path, struct aio **out, char *err, size_t errn)
{
    *out = NULL;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        snprintf(err, errn, "Cannot open %s: %s", path, strerror(errno));
        return -2;
    }
    struct stat st;
    uint8_t h[512];
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 4) {
        close(fd);
        seterr(err, errn, "Not an archive.");
        return -1;
    }
    size_t hn = st.st_size < 512 ? (size_t)st.st_size : 512;
    if (!pread_all(fd, h, hn, 0)) {
        close(fd);
        seterr(err, errn, "The file cannot be read.");
        return -2;
    }
    struct aio *a = calloc(1, sizeof(*a));
    if (!a) {
        close(fd);
        return -2;
    }
    a->fd = fd;
    a->fsize = (uint64_t)st.st_size;
    int rc = -1;
    uint8_t *whole = NULL;
    size_t wn = 0;
    if (!memcmp(h, "PK\3\4", 4) || !memcmp(h, "PK\5\6", 4)) {
        rc = zip_open_aio(a, err, errn);
    } else if (hn >= 32 && !memcmp(h, "7z\xbc\xaf\x27\x1c", 6)) {
        rc = sz_open_aio(a, err, errn);
    } else if (hn == 512 && tar_header_ok(h)) {
        snprintf(a->kind, sizeof(a->kind), "TAR");
        a->mode = 1;
        rc = tar_parse(a, err, errn);
    } else if (comp_magic(h, hn) || (hn >= 8 && !memcmp(h, "!<arch>\n", 8)) || (hn >= 4 && !memcmp(h, "\xed\xab\xee\xdb", 4))) {
        if (a->fsize > AIO_MAX_UNPACKED) {
            seterr(err, errn, "The file is too big to open here.");
            rc = -2;
        } else {
            whole = malloc((size_t)a->fsize + 1);
            if (!whole || !pread_all(fd, whole, (size_t)a->fsize, 0)) {
                seterr(err, errn, "The file cannot be read.");
                rc = -2;
            } else if (!memcmp(h, "!<arch>\n", 8)) {
                const uint8_t *data = NULL;
                size_t dn = 0;
                int r = ar_parse(a, whole, (size_t)a->fsize, true, &data, &dn);
                if (r == 1) {                                                    /* a Debian package: its data archive */
                    uint8_t *tarmem = NULL;
                    if (comp_magic(data, dn)) {
                        tarmem = unpack_whole(data, dn, &wn, err, errn);
                    } else {
                        tarmem = malloc(dn + 1);
                        if (tarmem) {
                            memcpy(tarmem, data, dn);
                            wn = dn;
                        }
                    }
                    if (!tarmem) {
                        rc = -2;
                    } else {
                        a->mem = tarmem;
                        a->memn = wn;
                        snprintf(a->kind, sizeof(a->kind), "DEB");
                        a->mode = 1;
                        close(a->fd);
                        a->fd = -1;
                        rc = tar_parse(a, err, errn);
                        if (rc == 0 && !a->note[0])
                            snprintf(a->note, sizeof(a->note), "The files of the package (data archive).");
                    }
                } else {                                                         /* a plain ar archive */
                    a->mem = whole;
                    a->memn = (size_t)a->fsize;
                    whole = NULL;
                    snprintf(a->kind, sizeof(a->kind), "AR");
                    a->mode = 1;
                    ar_parse(a, a->mem, a->memn, false, NULL, NULL);
                    rc = 0;
                }
            } else if (!memcmp(h, "\xed\xab\xee\xdb", 4)) {                       /* RPM: two headers, then the payload */
                uint64_t off = 96;
                bool bad = false;
                for (int hdr = 0; hdr < 2 && !bad; hdr++) {
                    if (off + 16 > a->fsize || memcmp(whole + off, "\x8e\xad\xe8\x01", 4)) {
                        bad = true;
                        break;
                    }
                    uint32_t ni = be32(whole + off + 8), hs = be32(whole + off + 12);
                    off += 16 + (uint64_t)ni * 16 + hs;
                    if (hdr == 0)
                        off = (off + 7) & ~(uint64_t)7;
                }
                if (bad || off >= a->fsize) {
                    seterr(err, errn, "A damaged RPM header.");
                    rc = -2;
                } else {
                    uint8_t *pay;
                    size_t pn = (size_t)(a->fsize - off);
                    if (comp_magic(whole + off, pn))
                        pay = unpack_whole(whole + off, pn, &wn, err, errn);
                    else {
                        pay = malloc(pn + 1);
                        if (pay) {
                            memcpy(pay, whole + off, pn);
                            wn = pn;
                        }
                    }
                    if (!pay) {
                        rc = -2;
                    } else {
                        a->mem = pay;
                        a->memn = wn;
                        snprintf(a->kind, sizeof(a->kind), "RPM");
                        a->mode = 1;
                        rc = cpio_parse(a);
                    }
                }
            } else {                                                             /* gzip / xz / bzip2 / zstd */
                size_t un = 0;
                uint8_t *u = unpack_whole(whole, (size_t)a->fsize, &un, err, errn);
                if (!u) {
                    rc = -2;
                } else {
                    a->mem = u;
                    a->memn = un;
                    a->mode = 1;
                    if (un >= 512 && tar_header_ok(u)) {
                        int k = h[0] == 0x1f ? 1 : h[0] == 0xfd ? 2 : h[0] == 'B' ? 3 : 4;
                        static const char *const names[] = { "", "TAR.GZ", "TAR.XZ", "TAR.BZ2", "TAR.ZST" };
                        snprintf(a->kind, sizeof(a->kind), "%s", names[k]);
                        rc = tar_parse(a, err, errn);
                    } else {                                                     /* one compressed file */
                        static const char *const names[] = { "", "GZIP", "XZ", "BZIP2", "ZSTD" };
                        int k = h[0] == 0x1f ? 1 : h[0] == 0xfd ? 2 : h[0] == 'B' ? 3 : 4;
                        snprintf(a->kind, sizeof(a->kind), "%s", names[k]);
                        const char *sl = strrchr(path, '/');
                        char base[256];
                        snprintf(base, sizeof(base), "%s", sl ? sl + 1 : path);
                        char *dot = strrchr(base, '.');
                        if (dot && dot != base && (!strcmp(dot, ".gz") || !strcmp(dot, ".xz") || !strcmp(dot, ".bz2") || !strcmp(dot, ".zst") || !strcmp(dot, ".zstd")))
                            *dot = 0;
                        int cap = 0;
                        struct aio_entry *e = add_entry(a, &cap);
                        if (e) {
                            set_name(e, (const uint8_t *)base, strlen(base), false);
                            e->type = AIO_FILE;
                            e->size = (int64_t)un;
                            e->packed = (int64_t)a->fsize;
                            e->off = 0;
                            e->mtime = (int64_t)st.st_mtime;
                        }
                        a->extractable = 1;
                        rc = 0;
                    }
                }
            }
        }
    }
    free(whole);
    if (rc < 0) {
        aio_close(a);
        return rc;
    }
    for (int i = 0; i < a->n; i++)
        if (a->e[i].type == AIO_FILE && a->e[i].size > 0)
            a->total_size += a->e[i].size;
    *out = a;
    return 0;
}

/* ======================================================================== extraction ======================================================================== */
/* The stored name as a safe relative path: no "..", no leading "/", no "." or empty parts. "" = the root itself (nothing to do); NULL = refused. */
static char *safe_rel(const uint8_t *raw, size_t n)
{
    char *out = malloc(n + 2);
    if (!out)
        return NULL;
    size_t o = 0, i = 0;
    while (i < n) {
        while (i < n && raw[i] == '/')
            i++;
        size_t s = i;
        while (i < n && raw[i] != '/')
            i++;
        size_t l = i - s;
        if (l == 0 || (l == 1 && raw[s] == '.'))
            continue;
        if ((l == 2 && raw[s] == '.' && raw[s + 1] == '.') || l > 255 || memchr(raw + s, 0, l)) {
            free(out);
            return NULL;
        }
        if (o)
            out[o++] = '/';
        memcpy(out + o, raw + s, l);
        o += l;
    }
    out[o] = 0;
    return out;
}

struct dirfix {
    char *rel;
    uint32_t mode;
    int64_t mtime;
};

struct xctx {
    int rootfd;
    const struct aio_opts *o;
    struct aio_progress *p;
    struct aio_report *rep;
    struct dirfix *dirs;
    int ndirs, capdirs;
    uint8_t *chunk;
};

#define CHUNK_SZ (1u << 20)

/* Opens the folder that holds `rel` (making the folders on the way, never through a link); *leaf points into the copy returned in *copy. */
static int open_parent(int rootfd, const char *rel, char **copy, const char **leaf)
{
    char *c = strdup(rel);
    if (!c)
        return -1;
    char *last = strrchr(c, '/');
    int cur = dup(rootfd);
    if (cur < 0) {
        free(c);
        return -1;
    }
    if (last) {
        *last = 0;
        *leaf = last + 1;
        for (char *tok = c; tok && *tok;) {
            char *sl = strchr(tok, '/');
            if (sl)
                *sl = 0;
            if (mkdirat(cur, tok, 0777) < 0 && errno != EEXIST) {
                close(cur);
                free(c);
                return -1;
            }
            int nfd = openat(cur, tok, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            close(cur);
            if (nfd < 0) {
                free(c);
                return -1;
            }
            cur = nfd;
            tok = sl ? sl + 1 : NULL;
        }
    } else {
        *leaf = c;
    }
    *copy = c;
    return cur;
}

static void src_free(struct src *s)
{
    free(s->own);
    s->own = NULL;
}

/* The content of an entry; for a deflated ZIP entry it is unpacked into memory first. 0 = ok, else an error text. */
static const char *src_open(const struct aio *a, const struct aio_entry *e, struct src *s)
{
    memset(s, 0, sizeof(*s));
    s->fd = -1;
    if (a->mode == 1) {
        if (e->size < 0 || e->off > a->memn || (uint64_t)e->size > (a->mem ? a->memn - e->off : (a->fsize > e->off ? a->fsize - e->off : 0))) {
            if (a->mem)
                return "the data is cut off";
        }
        if (a->mem) {
            s->mem = a->mem + e->off;
            s->len = (uint64_t)(e->size < 0 ? 0 : e->size);
        } else {
            s->fd = a->fd;
            s->foff = e->off;
            s->len = (uint64_t)(e->size < 0 ? 0 : e->size);
        }
        return NULL;
    }
    if (a->mode == 2)
        return sz7_slice((struct aio *)a, e, s);
    /* ZIP */
    if (e->method < 0)
        return "the entry is encrypted";
    uint8_t lh[30];
    if (!pread_all(a->fd, lh, 30, e->off) || memcmp(lh, "PK\3\4", 4))
        return "the local header is damaged";
    uint64_t data = e->off + 30 + u16(lh + 26) + u16(lh + 28);
    if (data + e->csize > a->fsize)
        return "the data is cut off";
    if (e->method == 0) {
        s->fd = a->fd;
        s->foff = data;
        s->len = e->csize;
        return NULL;
    }
    if (e->method != 8)
        return "an unknown compression method";
    if ((uint64_t)e->size > AIO_MAX_ENTRY)
        return "the file is too big to unpack here (over 1 GB)";
    uint8_t *in = malloc((size_t)e->csize + 1), *out = malloc((size_t)e->size + 1);
    if (!in || !out || !pread_all(a->fd, in, (size_t)e->csize, data)) {
        free(in);
        free(out);
        return "out of memory or the data cannot be read";
    }
    int r = lm_inflate_raw(in, (size_t)e->csize, out, (size_t)e->size);
    free(in);
    if (r != 0) {
        free(out);
        return "the data does not unpack";
    }
    s->own = out;
    s->mem = out;
    s->len = (uint64_t)e->size;
    return NULL;
}

/* Copies the content to outfd (-1: only read it), counting progress and the CRC. 0 = ok, -1 = error (errno / text in *why), -2 = cancelled. */
static int src_copy(struct xctx *x, struct src *s, int outfd, uint32_t *crc, const char **why)
{
    uint64_t done = 0;
    *crc = 0;
    while (done < s->len) {
        if (x->p && x->p->cancel)
            return -2;
        size_t n = s->len - done < CHUNK_SZ ? (size_t)(s->len - done) : CHUNK_SZ;
        const uint8_t *buf;
        if (s->mem)
            buf = s->mem + done;
        else {
            if (!pread_all(s->fd, x->chunk, n, s->foff + done)) {
                *why = "the data cannot be read";
                return -1;
            }
            buf = x->chunk;
        }
        *crc = lm_crc32(*crc, buf, n);
        if (outfd >= 0) {
            size_t w = 0;
            while (w < n) {
                ssize_t r = write(outfd, buf + w, n - w);
                if (r < 0) {
                    if (errno == EINTR)
                        continue;
                    *why = strerror(errno);
                    return -1;
                }
                w += (size_t)r;
            }
        }
        done += n;
        if (x->p)
            x->p->done += n;
    }
    return 0;
}

static void note_dir(struct xctx *x, const char *rel, uint32_t mode, int64_t mtime)
{
    if (!mode && !mtime)
        return;
    if (x->ndirs == x->capdirs) {
        x->capdirs = x->capdirs ? x->capdirs * 2 : 64;
        struct dirfix *t = realloc(x->dirs, (size_t)x->capdirs * sizeof(*t));
        if (!t)
            return;
        x->dirs = t;
    }
    x->dirs[x->ndirs].rel = strdup(rel);
    x->dirs[x->ndirs].mode = mode;
    x->dirs[x->ndirs].mtime = mtime;
    x->ndirs++;
}

static int extract_one(struct aio *a, const struct aio_entry *e, struct xctx *x)
{
    struct aio_report *rep = x->rep;
    char *rel = safe_rel(e->raw_path ? e->raw_path : (const uint8_t *)"", e->raw_len);
    const char *shown = e->path ? e->path : "";
    if (!rel) {
        report_err(rep, "%s: %s", shown, "refused: the name leads out of the destination folder");
        return 0;
    }
    if (!rel[0]) {
        free(rel);
        return 0;
    }
    progress_name(x->p, shown);
    int result = 0;
    if (x->o->test_only) {
        if (e->type == AIO_FILE) {
            struct src s;
            const char *why = src_open(a, e, &s);
            uint32_t crc = 0;
            int r = why ? -1 : src_copy(x, &s, -1, &crc, &why);
            src_free(&s);
            if (r == -2)
                result = -2;
            else if (r < 0)
                report_err(rep, "%s: %s", shown, why);
            else if (e->has_crc && crc != e->crc)
                report_err(rep, "%s: %s", shown, "the checksum does not match: the data is damaged");
            else {
                rep->extracted++;
                rep->bytes += s.len;
            }
        }
        free(rel);
        return result;
    }
    char *copy = NULL;
    const char *leaf = NULL;
    int pfd = open_parent(x->rootfd, rel, &copy, &leaf);
    if (pfd < 0) {
        report_err(rep, "%s: %s", shown, errno == ELOOP || errno == ENOTDIR ? "refused: a folder on the way is a link or a file" : strerror(errno));
        free(rel);
        return 0;
    }
    struct stat st;
    bool exists = fstatat(pfd, leaf, &st, AT_SYMLINK_NOFOLLOW) == 0;
    if (e->type == AIO_DIR) {
        if (exists && !S_ISDIR(st.st_mode)) {
            if (x->o->overwrite && unlinkat(pfd, leaf, 0) == 0)
                exists = false;
            else {
                rep->skipped++;
                goto done;
            }
        }
        if (!exists && mkdirat(pfd, leaf, 0777) < 0 && errno != EEXIST)
            report_err(rep, "%s: %s", shown, strerror(errno));
        else {
            rep->dirs++;
            note_dir(x, rel, e->mode, e->mtime);
        }
        goto done;
    }
    if (exists) {
        if (!x->o->overwrite || S_ISDIR(st.st_mode)) {
            rep->skipped++;
            if (e->type == AIO_FILE && e->size > 0 && x->p)
                x->p->done += (uint64_t)e->size;
            goto done;
        }
        unlinkat(pfd, leaf, 0);
    }
    if (e->type == AIO_SYMLINK) {
        const char *target = e->link;
        char *zt = NULL;
        if (!target) {                                                       /* a ZIP symlink: the content is the target */
            struct src s;
            const char *why = src_open(a, e, &s);
            if (!why && s.len > 0 && s.len < 4096) {
                zt = calloc(1, (size_t)s.len + 1);
                if (zt) {
                    if (s.mem)
                        memcpy(zt, s.mem, (size_t)s.len);
                    else if (!pread_all(s.fd, zt, (size_t)s.len, s.foff)) {
                        free(zt);
                        zt = NULL;
                    }
                }
            }
            src_free(&s);
            target = zt;
        }
        if (!target || !target[0] || symlinkat(target, pfd, leaf) < 0)
            report_err(rep, "%s: %s", shown, target ? strerror(errno) : "a link without a target");
        else {
            rep->links++;
            if (e->mtime) {
                struct timespec ts[2] = { { .tv_nsec = UTIME_OMIT }, { .tv_sec = (time_t)e->mtime } };
                utimensat(pfd, leaf, ts, AT_SYMLINK_NOFOLLOW);
            }
        }
        free(zt);
        goto done;
    }
    if (e->type == AIO_HARDLINK) {
        char *trel = e->link ? safe_rel((const uint8_t *)e->link, strlen(e->link)) : NULL;
        if (!trel || !trel[0] || linkat(x->rootfd, trel, pfd, leaf, 0) < 0)
            report_err(rep, "%s: %s", shown, trel && trel[0] ? strerror(errno) : "a link to a file outside the destination");
        else
            rep->links++;
        free(trel);
        goto done;
    }
    if (e->type != AIO_FILE) {
        rep->skipped++;
        goto done;
    }
    {
        struct src s;
        const char *why = src_open(a, e, &s);
        if (why) {
            report_err(rep, "%s: %s", shown, why);
            goto done;
        }
        uint32_t perm = e->mode ? e->mode & 0777 : 0644;
        int fd = openat(pfd, leaf, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, perm);
        if (fd < 0) {
            report_err(rep, "%s: %s", shown, strerror(errno));
            src_free(&s);
            goto done;
        }
        uint32_t crc = 0;
        int r = src_copy(x, &s, fd, &crc, &why);
        if (r == 0 && e->has_crc && crc != e->crc) {
            r = -1;
            why = "the checksum does not match: the data is damaged";
        }
        if (r == 0 && e->mtime) {
            struct timespec ts[2] = { { .tv_nsec = UTIME_OMIT }, { .tv_sec = (time_t)e->mtime } };
            futimens(fd, ts);
        }
        close(fd);
        if (r < 0) {
            unlinkat(pfd, leaf, 0);
            if (r == -2)
                result = -2;
            else
                report_err(rep, "%s: %s", shown, why);
        } else {
            rep->extracted++;
            rep->bytes += s.len;
        }
        src_free(&s);
    }
done:
    close(pfd);
    free(copy);
    free(rel);
    return result;
}

int aio_extract(struct aio *a, const int *sel, int nsel, const char *dest, const struct aio_opts *o, struct aio_progress *p, struct aio_report *rep, char *err, size_t errn)
{
    struct aio_report local;
    if (!rep)
        rep = &local;
    memset(rep, 0, sizeof(*rep));
    if (!a->extractable) {
        seterr(err, errn, a->why_not[0] ? a->why_not : "This archive cannot be extracted.");
        return -1;
    }
    struct xctx x = { .o = o, .p = p, .rep = rep, .rootfd = -1 };
    if (!o->test_only) {
        char *dd = strdup(dest);
        int mk = 0;
        for (char *q = dd; dd && *q; q++)                                    /* the folders on the way, too */
            if (*q == '/' && q != dd) {
                *q = 0;
                mkdir(dd, 0777);
                *q = '/';
            }
        free(dd);
        mk = mkdir(dest, 0777);
        if (mk < 0 && errno != EEXIST) {
            char t[300];
            snprintf(t, sizeof(t), "Cannot make %s: %s", dest, strerror(errno));
            seterr(err, errn, t);
            return -1;
        }
        x.rootfd = open(dest, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (x.rootfd < 0) {
            char t[300];
            snprintf(t, sizeof(t), "Cannot open %s: %s", dest, strerror(errno));
            seterr(err, errn, t);
            return -1;
        }
    }
    x.chunk = malloc(CHUNK_SZ);
    if (!x.chunk) {
        if (x.rootfd >= 0)
            close(x.rootfd);
        seterr(err, errn, "out of memory");
        return -1;
    }
    int count = sel ? nsel : a->n;
    if (p) {
        uint64_t total = 0;
        for (int i = 0; i < count; i++) {
            int k = sel ? sel[i] : i;
            if (k >= 0 && k < a->n && a->e[k].type == AIO_FILE && a->e[k].size > 0)
                total += (uint64_t)a->e[k].size;
        }
        p->total = total;
        p->done = 0;
        p->files_done = 0;
    }
    int rc = 0;
    for (int i = 0; i < count && rc == 0; i++) {
        int k = sel ? sel[i] : i;
        if (k < 0 || k >= a->n)
            continue;
        if (p && p->cancel) {
            rc = -2;
            break;
        }
        rc = extract_one(a, &a->e[k], &x);
        if (p)
            p->files_done++;
    }
    for (int i = x.ndirs - 1; i >= 0; i--) {                                 /* folder modes and times, deepest first */
        struct dirfix *d = &x.dirs[i];
        if (d->mode && x.rootfd >= 0)
            (void)!fchmodat(x.rootfd, d->rel, (mode_t)((d->mode & 0777) | 0700), 0);
        if (d->mtime && x.rootfd >= 0) {
            struct timespec ts[2] = { { .tv_nsec = UTIME_OMIT }, { .tv_sec = (time_t)d->mtime } };
            utimensat(x.rootfd, d->rel, ts, 0);
        }
        free(d->rel);
    }
    free(x.dirs);
    free(x.chunk);
    if (x.rootfd >= 0)
        close(x.rootfd);
    return rc;
}

/* ======================================================================== creation ========================================================================== */
struct cent {
    char *rel, *abs, *link;
    struct stat st;
};

struct clist {
    struct cent *v;
    int n, cap;
    dev_t out_dev;
    ino_t out_ino;
    bool have_out;
    uint64_t total;
};

static int cmp_names(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static int collect(struct clist *l, const char *abs, const char *rel, int depth, struct aio_report *rep)
{
    struct stat st;
    if (lstat(abs, &st) < 0) {
        report_err(rep, "%s: %s", abs, strerror(errno));
        return 0;
    }
    if (l->have_out && st.st_dev == l->out_dev && st.st_ino == l->out_ino)
        return 0;                                                            /* the archive being made */
    if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
        report_err(rep, "%s: %s", abs, "skipped: not a file, folder or link");
        return 0;
    }
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 256;
        struct cent *t = realloc(l->v, (size_t)l->cap * sizeof(*t));
        if (!t)
            return -1;
        l->v = t;
    }
    struct cent *c = &l->v[l->n++];
    memset(c, 0, sizeof(*c));
    c->abs = strdup(abs);
    c->st = st;
    if (S_ISDIR(st.st_mode)) {
        size_t rl = strlen(rel);
        c->rel = malloc(rl + 2);
        memcpy(c->rel, rel, rl);
        c->rel[rl] = '/';
        c->rel[rl + 1] = 0;
        if (depth > 200)
            return 0;
        DIR *d = opendir(abs);
        if (!d) {
            report_err(rep, "%s: %s", abs, strerror(errno));
            return 0;
        }
        char **names = NULL;
        int nn = 0, nc = 0;
        struct dirent *de;
        while ((de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
                continue;
            if (nn == nc) {
                nc = nc ? nc * 2 : 32;
                names = realloc(names, (size_t)nc * sizeof(char *));
            }
            names[nn++] = strdup(de->d_name);
        }
        closedir(d);
        qsort(names, (size_t)nn, sizeof(char *), cmp_names);
        for (int i = 0; i < nn; i++) {
            char *ca = malloc(strlen(abs) + strlen(names[i]) + 2), *cr = malloc(strlen(rel) + strlen(names[i]) + 2);
            sprintf(ca, "%s/%s", abs, names[i]);
            sprintf(cr, "%s/%s", rel, names[i]);
            collect(l, ca, cr, depth + 1, rep);
            free(ca);
            free(cr);
            free(names[i]);
        }
        free(names);
        return 0;
    }
    c->rel = strdup(rel);
    if (S_ISLNK(st.st_mode)) {
        char t[4096];
        ssize_t n = readlink(abs, t, sizeof(t) - 1);
        if (n < 0) {
            report_err(rep, "%s: %s", abs, strerror(errno));
            free(c->rel);
            free(c->abs);
            l->n--;
            return 0;
        }
        t[n] = 0;
        c->link = strdup(t);
    } else
        l->total += (uint64_t)st.st_size;
    return 0;
}

static void clist_free(struct clist *l)
{
    for (int i = 0; i < l->n; i++) {
        free(l->v[i].rel);
        free(l->v[i].abs);
        free(l->v[i].link);
    }
    free(l->v);
}

struct wout {
    int fd;
    uint64_t pos;
    bool err;
    int saved_errno;
};

static void w_raw(struct wout *w, const void *buf, size_t n)
{
    const uint8_t *p = buf;
    while (n && !w->err) {
        ssize_t r = write(w->fd, p, n);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            w->err = true;
            w->saved_errno = errno;
            return;
        }
        p += r;
        n -= (size_t)r;
        w->pos += (uint64_t)r;
    }
}

static int def_sink(void *ctx, const uint8_t *p, size_t n)
{
    struct wout *w = ctx;
    w_raw(w, p, n);
    return w->err ? -1 : 0;
}

static void put16(uint8_t *b, uint32_t v) { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *b, uint32_t v) { for (int i = 0; i < 4; i++) b[i] = (uint8_t)(v >> (8 * i)); }
static void put64(uint8_t *b, uint64_t v) { for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (8 * i)); }

struct vec {
    uint8_t *p;
    size_t n, cap;
};

static void vec_add(struct vec *v, const void *d, size_t n)
{
    if (v->n + n > v->cap) {
        v->cap = (v->n + n) * 2 + 256;
        v->p = realloc(v->p, v->cap);
    }
    memcpy(v->p + v->n, d, n);
    v->n += n;
}

static void dos_time(int64_t t, uint32_t *dt, uint32_t *dd)
{
    time_t tt = (time_t)t;
    struct tm tm;
    if (!localtime_r(&tt, &tm) || tm.tm_year < 80) {
        *dd = (1 << 5) | 1;
        *dt = 0;
        return;
    }
    *dd = (uint32_t)((tm.tm_year - 80) << 9 | (tm.tm_mon + 1) << 5 | tm.tm_mday);
    *dt = (uint32_t)(tm.tm_hour << 11 | tm.tm_min << 5 | tm.tm_sec / 2);
}

static int create_zip(struct wout *w, struct clist *l, int level, struct aio_progress *p, struct aio_report *rep, char *err, size_t errn)
{
    struct vec cd = { 0 };
    uint8_t *chunk = malloc(CHUNK_SZ);
    if (!chunk)
        return -1;
    int rc = 0;
    uint64_t count = 0;
    for (int i = 0; i < l->n && rc == 0; i++) {
        struct cent *c = &l->v[i];
        if (p && p->cancel) {
            rc = -2;
            break;
        }
        progress_name(p, c->rel);
        bool dir = S_ISDIR(c->st.st_mode), link = c->link != NULL;
        size_t nl = strlen(c->rel);
        uint64_t usize = dir ? 0 : link ? strlen(c->link) : (uint64_t)c->st.st_size;
        bool z64 = usize >= 0xffffffffu;
        uint32_t dt, dd;
        dos_time(c->st.st_mtime, &dt, &dd);
        uint64_t local_off = w->pos;
        uint8_t xt[9];
        put16(xt, 0x5455); put16(xt + 2, 5); xt[4] = 1; put32(xt + 5, (uint32_t)c->st.st_mtime);
        uint8_t lh[30 + 20 + 9];
        size_t lxl = (z64 ? 20 : 0) + 9;
        memset(lh, 0, sizeof(lh));
        memcpy(lh, "PK\3\4", 4);
        put16(lh + 4, z64 ? 45 : 20);
        put16(lh + 6, 0x0800);
        put16(lh + 8, 8);
        put16(lh + 10, dt);
        put16(lh + 12, dd);
        put16(lh + 26, (uint32_t)nl);
        put16(lh + 28, (uint32_t)lxl);
        size_t lp = 30;
        if (z64) {
            put16(lh + lp, 1); put16(lh + lp + 2, 16);
            lp += 20;
        }
        memcpy(lh + lp, xt, 9);
        w_raw(w, lh, 30);
        w_raw(w, c->rel, nl);
        w_raw(w, lh + 30, lxl);
        uint64_t data_start = w->pos, csize = 0;
        uint32_t crc = 0;
        int method = 0;
        if (dir || usize == 0) {
            method = 0;
        } else if (link) {
            w_raw(w, c->link, usize);
            crc = lm_crc32(0, (const uint8_t *)c->link, usize);
            csize = usize;
        } else {
            int fd = open(c->abs, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
            if (fd < 0) {
                report_err(rep, "%s: %s", c->abs, strerror(errno));
                usize = 0;
            } else {
                struct lm_deflater *df = lm_deflate_new(level, def_sink, w);
                if (!df) {
                    close(fd);
                    rc = -1;
                    break;
                }
                uint64_t got = 0;
                for (;;) {
                    if (p && p->cancel) {
                        rc = -2;
                        break;
                    }
                    ssize_t r = read(fd, chunk, CHUNK_SZ);
                    if (r < 0 && errno == EINTR)
                        continue;
                    if (r <= 0)
                        break;
                    crc = lm_crc32(crc, chunk, (size_t)r);
                    got += (uint64_t)r;
                    lm_deflate_write(df, chunk, (size_t)r);
                    if (p)
                        p->done += (uint64_t)r;
                    if (w->err)
                        break;
                }
                lm_deflate_finish(df);
                usize = got;
                csize = w->pos - data_start;
                method = 8;
                if (rc == 0 && !w->err && csize >= usize) {                  /* it did not get smaller: store it */
                    if (lseek(fd, 0, SEEK_SET) == 0 && lseek(w->fd, (off_t)data_start, SEEK_SET) == (off_t)data_start) {
                        w->pos = data_start;
                        uint32_t c2 = 0;
                        for (;;) {
                            ssize_t r = read(fd, chunk, CHUNK_SZ);
                            if (r <= 0)
                                break;
                            c2 = lm_crc32(c2, chunk, (size_t)r);
                            w_raw(w, chunk, (size_t)r);
                        }
                        if (ftruncate(w->fd, (off_t)w->pos) == 0) {
                            usize = w->pos - data_start;
                            csize = usize;
                            crc = c2;
                            method = 0;
                        }
                        lseek(w->fd, (off_t)w->pos, SEEK_SET);
                    }
                }
                close(fd);
            }
        }
        if (rc == -2)
            break;
        if (w->err) {
            snprintf(err, errn, "Writing the archive failed: %s", strerror(w->saved_errno));
            rc = -1;
            break;
        }
        if (usize >= 0xffffffffu && !z64) {
            seterr(err, errn, "A file grew beyond 4 GB while it was being added.");
            rc = -1;
            break;
        }
        /* the sizes go into the local header now that they are known */
        uint8_t patch[16];
        put16(lh + 8, (uint32_t)method);
        put32(patch, crc);
        if (pwrite(w->fd, lh + 8, 2, (off_t)(local_off + 8)) != 2 || pwrite(w->fd, patch, 4, (off_t)(local_off + 14)) != 4) {
            seterr(err, errn, "Writing the archive failed.");
            rc = -1;
            break;
        }
        if (z64) {
            put32(patch, 0xffffffffu);
            (void)!pwrite(w->fd, patch, 4, (off_t)(local_off + 18));
            (void)!pwrite(w->fd, patch, 4, (off_t)(local_off + 22));
            put64(patch, usize);
            put64(patch + 8, csize);
            (void)!pwrite(w->fd, patch, 16, (off_t)(local_off + 30 + nl + 4));
        } else {
            put32(patch, (uint32_t)csize);
            put32(patch + 4, (uint32_t)usize);
            (void)!pwrite(w->fd, patch, 8, (off_t)(local_off + 18));
        }
        /* the central directory record */
        bool big_off = local_off >= 0xffffffffu;
        size_t cxl = 9 + ((z64 || big_off) ? 4 + (z64 ? 16 : 0) + (big_off ? 8 : 0) : 0);
        uint8_t ch[46];
        memset(ch, 0, sizeof(ch));
        memcpy(ch, "PK\1\2", 4);
        put16(ch + 4, (3 << 8) | ((z64 || big_off) ? 45 : 20));
        put16(ch + 6, (z64 || big_off) ? 45 : 20);
        put16(ch + 8, 0x0800);
        put16(ch + 10, (uint32_t)method);
        put16(ch + 12, dt);
        put16(ch + 14, dd);
        put32(ch + 16, crc);
        put32(ch + 20, z64 ? 0xffffffffu : (uint32_t)csize);
        put32(ch + 24, z64 ? 0xffffffffu : (uint32_t)usize);
        put16(ch + 28, (uint32_t)nl);
        put16(ch + 30, (uint32_t)cxl);
        put32(ch + 38, ((uint32_t)c->st.st_mode << 16) | (dir ? 0x10 : 0));
        put32(ch + 42, big_off ? 0xffffffffu : (uint32_t)local_off);
        vec_add(&cd, ch, 46);
        vec_add(&cd, c->rel, nl);
        if (z64 || big_off) {
            uint8_t z[28];
            size_t zl = (z64 ? 16 : 0) + (big_off ? 8 : 0);
            put16(z, 1);
            put16(z + 2, (uint32_t)zl);
            size_t zo = 4;
            if (z64) { put64(z + zo, usize); zo += 8; put64(z + zo, csize); zo += 8; }
            if (big_off) { put64(z + zo, local_off); zo += 8; }
            vec_add(&cd, z, zo);
        }
        vec_add(&cd, xt, 9);
        count++;
        if (rep && (dir ? 0 : 1)) {
            if (link)
                rep->links++;
            else
                rep->extracted++;
        } else if (rep) {
            rep->dirs++;
        }
    }
    free(chunk);
    if (rc == 0) {
        uint64_t cd_off = w->pos;
        w_raw(w, cd.p, cd.n);
        bool z64e = count >= 0xffff || cd_off >= 0xffffffffu || cd.n >= 0xffffffffu;
        if (z64e) {
            uint8_t z[56 + 20];
            memset(z, 0, sizeof(z));
            memcpy(z, "PK\6\6", 4);
            put64(z + 4, 44);
            put16(z + 12, (3 << 8) | 45);
            put16(z + 14, 45);
            put64(z + 24, count);
            put64(z + 32, count);
            put64(z + 40, cd.n);
            put64(z + 48, cd_off);
            memcpy(z + 56, "PK\6\7", 4);
            put64(z + 64, w->pos);
            put32(z + 72, 1);
            w_raw(w, z, 76);
        }
        uint8_t e[22];
        memset(e, 0, sizeof(e));
        memcpy(e, "PK\5\6", 4);
        put16(e + 8, count >= 0xffff ? 0xffff : (uint32_t)count);
        put16(e + 10, count >= 0xffff ? 0xffff : (uint32_t)count);
        put32(e + 12, cd.n >= 0xffffffffu ? 0xffffffffu : (uint32_t)cd.n);
        put32(e + 16, cd_off >= 0xffffffffu ? 0xffffffffu : (uint32_t)cd_off);
        w_raw(w, e, 22);
        if (w->err) {
            snprintf(err, errn, "Writing the archive failed: %s", strerror(w->saved_errno));
            rc = -1;
        }
    }
    free(cd.p);
    return rc;
}

/* ---- TAR (optionally through gzip) ---- */
struct tout {
    struct wout *w;
    struct lm_deflater *df;
    uint32_t crc;
    uint64_t isize;
};

static void t_write(struct tout *t, const void *buf, size_t n)
{
    if (t->df) {
        t->crc = lm_crc32(t->crc, buf, n);
        t->isize += n;
        lm_deflate_write(t->df, buf, n);
    } else
        w_raw(t->w, buf, n);
}

static void oct(uint8_t *f, size_t n, uint64_t v)
{
    if (v >= (1ull << (3 * (n - 1)))) {                                     /* too big for octal: GNU base-256 */
        memset(f, 0, n);
        f[0] = 0x80;
        for (size_t i = n - 1; i > 0 && v; i--, v >>= 8)
            f[i] = (uint8_t)v;
        return;
    }
    snprintf((char *)f, n, "%0*llo", (int)(n - 1), (unsigned long long)v);
}

static void tar_header(struct tout *t, const char *name, size_t namelen, uint32_t mode, uint64_t size, int64_t mtime, char type, const char *link)
{
    uint8_t h[512];
    memset(h, 0, sizeof(h));
    memcpy(h, name, namelen < 100 ? namelen : 100);
    oct(h + 100, 8, mode & 07777);
    oct(h + 108, 8, 0);
    oct(h + 116, 8, 0);
    oct(h + 124, 12, size);
    oct(h + 136, 12, mtime < 0 ? 0 : (uint64_t)mtime);
    memset(h + 148, ' ', 8);
    h[156] = (uint8_t)type;
    if (link)
        memcpy(h + 157, link, strlen(link) < 100 ? strlen(link) : 100);
    memcpy(h + 257, "ustar\0" "00", 8);
    unsigned long sum = 0;
    for (int i = 0; i < 512; i++)
        sum += h[i];
    snprintf((char *)h + 148, 7, "%06lo", sum);
    h[155] = ' ';
    t_write(t, h, 512);
}

static void tar_pax(struct tout *t, const char *key, const char *value)
{
    size_t kl = strlen(key), vl = strlen(value), len = kl + vl + 3;
    char digits[24];
    size_t total = len;
    for (int tries = 0; tries < 3; tries++) {
        int dn = snprintf(digits, sizeof(digits), "%zu", total);
        total = len + (size_t)dn;
    }
    char *rec = malloc(total + 32);
    int n = snprintf(rec, total + 32, "%zu %s=%s\n", total, key, value);
    char pname[] = "PaxHeader";
    tar_header(t, pname, strlen(pname), 0644, (uint64_t)n, 0, 'x', NULL);
    t_write(t, rec, (size_t)n);
    uint8_t pad[512] = { 0 };
    if (n % 512)
        t_write(t, pad, 512 - (size_t)n % 512);
    free(rec);
}

static int create_tar(struct tout *t, struct clist *l, struct aio_progress *p, struct aio_report *rep, char *err, size_t errn)
{
    uint8_t *chunk = malloc(CHUNK_SZ);
    if (!chunk)
        return -1;
    int rc = 0;
    for (int i = 0; i < l->n && rc == 0; i++) {
        struct cent *c = &l->v[i];
        if (p && p->cancel) {
            rc = -2;
            break;
        }
        progress_name(p, c->rel);
        bool dir = S_ISDIR(c->st.st_mode), link = c->link != NULL;
        size_t nl = strlen(c->rel);
        char shortname[101];
        if (nl > 100 || (link && strlen(c->link) > 100)) {
            if (nl > 100)
                tar_pax(t, "path", c->rel);
            if (link && strlen(c->link) > 100)
                tar_pax(t, "linkpath", c->link);
        }
        snprintf(shortname, sizeof(shortname), "%s", c->rel);
        uint64_t size = dir || link ? 0 : (uint64_t)c->st.st_size;
        tar_header(t, shortname, strlen(shortname), c->st.st_mode, size, c->st.st_mtime, dir ? '5' : link ? '2' : '0', link ? c->link : NULL);
        if (size) {
            int fd = open(c->abs, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
            uint64_t sent = 0;
            if (fd < 0)
                report_err(rep, "%s: %s", c->abs, strerror(errno));
            while (fd >= 0 && sent < size) {
                if (p && p->cancel) {
                    rc = -2;
                    break;
                }
                size_t want = size - sent < CHUNK_SZ ? (size_t)(size - sent) : CHUNK_SZ;
                ssize_t r = read(fd, chunk, want);
                if (r < 0 && errno == EINTR)
                    continue;
                if (r <= 0)
                    break;
                t_write(t, chunk, (size_t)r);
                sent += (uint64_t)r;
                if (p)
                    p->done += (uint64_t)r;
            }
            if (fd >= 0)
                close(fd);
            if (rc == -2)
                break;
            memset(chunk, 0, 512);
            while (sent < size) {                                            /* the file got shorter: pad with zeros so the headers stay in step */
                size_t k = size - sent < 512 ? (size_t)(size - sent) : 512;
                t_write(t, chunk, k);
                sent += k;
            }
            if (size % 512)
                t_write(t, chunk, 512 - (size_t)(size % 512));
        }
        if (rep) {
            if (dir) rep->dirs++; else if (link) rep->links++; else rep->extracted++;
        }
        if (t->w->err) {
            snprintf(err, errn, "Writing the archive failed: %s", strerror(t->w->saved_errno));
            rc = -1;
        }
    }
    free(chunk);
    if (rc == 0) {
        uint8_t zero[1024] = { 0 };
        t_write(t, zero, 1024);
    }
    return rc;
}

int aio_format_of(const char *name)
{
    size_t n = strlen(name);
    if (n > 4 && !strcasecmp(name + n - 4, ".zip"))
        return AIO_ZIP;
    if (n > 7 && !strcasecmp(name + n - 7, ".tar.gz"))
        return AIO_TARGZ;
    if (n > 4 && !strcasecmp(name + n - 4, ".tgz"))
        return AIO_TARGZ;
    if (n > 4 && !strcasecmp(name + n - 4, ".tar"))
        return AIO_TAR;
    return -1;
}

int aio_create(const char *out, int format, const char *const *inputs, int n, int level, bool overwrite, struct aio_progress *p, struct aio_report *rep, char *err, size_t errn)
{
    struct aio_report local;
    if (!rep)
        rep = &local;
    memset(rep, 0, sizeof(*rep));
    if (n < 1) {
        seterr(err, errn, "Nothing to add.");
        return -1;
    }
    struct stat ost;
    if (stat(out, &ost) == 0 && !overwrite) {
        snprintf(err, errn, "%s exists already.", out);
        return -1;
    }
    char tmp[4200];
    snprintf(tmp, sizeof(tmp), "%s.tmp-%d", out, (int)getpid());
    int fd = open(tmp, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) {
        snprintf(err, errn, "Cannot make %s: %s", out, strerror(errno));
        return -1;
    }
    struct stat tst;
    struct clist l = { 0 };
    if (fstat(fd, &tst) == 0) {
        l.have_out = true;
        l.out_dev = tst.st_dev;
        l.out_ino = tst.st_ino;
    }
    for (int i = 0; i < n; i++) {
        char *dup = strdup(inputs[i]);
        size_t dl = strlen(dup);
        while (dl > 1 && dup[dl - 1] == '/')
            dup[--dl] = 0;
        char base[4096];
        char *rp = NULL;
        const char *b = strrchr(dup, '/');
        b = b ? b + 1 : dup;
        if (!strcmp(b, ".") || !strcmp(b, "..") || !b[0]) {
            rp = realpath(dup, NULL);
            const char *rb = rp ? strrchr(rp, '/') : NULL;
            b = rb && rb[1] ? rb + 1 : "root";
        }
        snprintf(base, sizeof(base), "%s", b);
        collect(&l, dup, base, 0, rep);
        free(rp);
        free(dup);
    }
    if (l.n == 0) {
        clist_free(&l);
        close(fd);
        unlink(tmp);
        seterr(err, errn, "Nothing could be added.");
        return -1;
    }
    if (p) {
        p->total = l.total;
        p->done = 0;
    }
    struct wout w = { .fd = fd };
    int rc;
    if (format == AIO_ZIP) {
        rc = create_zip(&w, &l, level, p, rep, err, errn);
    } else {
        struct tout t = { .w = &w };
        if (format == AIO_TARGZ) {
            uint8_t gh[10] = { 0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 3 };
            put32(gh + 4, (uint32_t)time(NULL));
            w_raw(&w, gh, 10);
            t.df = lm_deflate_new(level, def_sink, &w);
            if (!t.df) {
                rc = -1;
                seterr(err, errn, "out of memory");
                goto finish;
            }
        }
        rc = create_tar(&t, &l, p, rep, err, errn);
        if (t.df) {
            if (lm_deflate_finish(t.df) && rc == 0) {
                rc = -1;
                snprintf(err, errn, "Writing the archive failed: %s", strerror(w.saved_errno));
            }
            uint8_t tr[8];
            put32(tr, t.crc);
            put32(tr + 4, (uint32_t)t.isize);
            w_raw(&w, tr, 8);
        }
    }
finish:
    if (rc == 0 && w.err) {
        snprintf(err, errn, "Writing the archive failed: %s", strerror(w.saved_errno));
        rc = -1;
    }
    if (rc == 0 && fsync(fd) < 0 && errno != EINVAL) {
        snprintf(err, errn, "Writing the archive failed: %s", strerror(errno));
        rc = -1;
    }
    close(fd);
    if (rc == 0 && rename(tmp, out) < 0) {
        snprintf(err, errn, "Cannot make %s: %s", out, strerror(errno));
        rc = -1;
    }
    if (rc != 0)
        unlink(tmp);
    clist_free(&l);
    return rc;
}
