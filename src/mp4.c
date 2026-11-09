#include "mp4.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct lm_mp4 {
    char *path;
    FILE *f;
    int w, h;
    uint64_t data_start;        /* offset of the first picture */
    uint64_t pos;               /* bytes written to the file */
    uint32_t n, cap;
    uint32_t *size;             /* per picture */
    uint64_t *pts;              /* per picture, relative to the first one */
    uint64_t first_pts;
    int failed;
};

/* ---- box building in memory ---------------------------------------------------------------------------------------- */

struct buf {
    uint8_t *p;
    size_t n, cap;
    int oom;
};

static void b_bytes(struct buf *b, const void *d, size_t n)
{
    if (b->n + n > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 4096;
        while (nc < b->n + n)
            nc *= 2;
        uint8_t *np = realloc(b->p, nc);
        if (!np) {
            b->oom = 1;
            return;
        }
        b->p = np;
        b->cap = nc;
    }
    if (!b->oom && n) {
        memcpy(b->p + b->n, d, n);
        b->n += n;
    }
}

static void b_u8(struct buf *b, unsigned v) { uint8_t c = (uint8_t)v; b_bytes(b, &c, 1); }
static void b_u16(struct buf *b, unsigned v) { b_u8(b, v >> 8); b_u8(b, v); }
static void b_u32(struct buf *b, uint32_t v) { b_u16(b, v >> 16); b_u16(b, v & 0xffff); }
static void b_u64(struct buf *b, uint64_t v) { b_u32(b, (uint32_t)(v >> 32)); b_u32(b, (uint32_t)v); }
static void b_zero(struct buf *b, size_t n) { while (n--) b_u8(b, 0); }
static void b_tag(struct buf *b, const char *t) { b_bytes(b, t, 4); }

/* begin_box returns the offset of the size field; end_box patches it. */
static size_t begin_box(struct buf *b, const char *type)
{
    size_t at = b->n;
    b_u32(b, 0);
    b_tag(b, type);
    return at;
}

static void end_box(struct buf *b, size_t at)
{
    if (b->oom)
        return;
    uint32_t sz = (uint32_t)(b->n - at);
    b->p[at] = (uint8_t)(sz >> 24);
    b->p[at + 1] = (uint8_t)(sz >> 16);
    b->p[at + 2] = (uint8_t)(sz >> 8);
    b->p[at + 3] = (uint8_t)sz;
}

static size_t begin_full(struct buf *b, const char *type, unsigned version, unsigned flags)
{
    size_t at = begin_box(b, type);
    b_u8(b, version);
    b_u8(b, flags >> 16);
    b_u16(b, flags & 0xffff);
    return at;
}

static const uint32_t unity[9] = { 0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000 };

static void build_moov(struct buf *b, const struct lm_mp4 *m, uint64_t duration, uint64_t first_offset)
{
    uint32_t dur = duration > 0xffffffffu ? 0xffffffffu : (uint32_t)duration;
    size_t moov = begin_box(b, "moov");

    size_t mvhd = begin_full(b, "mvhd", 0, 0);
    b_u32(b, 0); b_u32(b, 0);                       /* creation, modification (unknown: 0) */
    b_u32(b, 1000);
    b_u32(b, dur);
    b_u32(b, 0x00010000);                           /* rate 1.0 */
    b_u16(b, 0x0100);                               /* volume */
    b_zero(b, 10);
    for (int i = 0; i < 9; i++) b_u32(b, unity[i]);
    b_zero(b, 24);                                  /* pre_defined */
    b_u32(b, 2);                                    /* next track id */
    end_box(b, mvhd);

    size_t trak = begin_box(b, "trak");
    size_t tkhd = begin_full(b, "tkhd", 0, 3);      /* enabled, in the movie */
    b_u32(b, 0); b_u32(b, 0);
    b_u32(b, 1);                                    /* track id */
    b_u32(b, 0);
    b_u32(b, dur);
    b_zero(b, 8);
    b_u16(b, 0); b_u16(b, 0); b_u16(b, 0); b_u16(b, 0);   /* layer, alternate group, volume, reserved */
    for (int i = 0; i < 9; i++) b_u32(b, unity[i]);
    b_u32(b, (uint32_t)m->w << 16);
    b_u32(b, (uint32_t)m->h << 16);
    end_box(b, tkhd);

    size_t mdia = begin_box(b, "mdia");
    size_t mdhd = begin_full(b, "mdhd", 0, 0);
    b_u32(b, 0); b_u32(b, 0);
    b_u32(b, 1000);
    b_u32(b, dur);
    b_u16(b, 0x55c4);                               /* language "und" */
    b_u16(b, 0);
    end_box(b, mdhd);
    size_t hdlr = begin_full(b, "hdlr", 0, 0);
    b_u32(b, 0);
    b_tag(b, "vide");
    b_zero(b, 12);
    b_bytes(b, "MellaGUI screen\0", 16);
    end_box(b, hdlr);

    size_t minf = begin_box(b, "minf");
    size_t vmhd = begin_full(b, "vmhd", 0, 1);
    b_zero(b, 8);
    end_box(b, vmhd);
    size_t dinf = begin_box(b, "dinf");
    size_t dref = begin_full(b, "dref", 0, 0);
    b_u32(b, 1);
    size_t url = begin_full(b, "url ", 0, 1);       /* media is in this file */
    end_box(b, url);
    end_box(b, dref);
    end_box(b, dinf);

    size_t stbl = begin_box(b, "stbl");
    size_t stsd = begin_full(b, "stsd", 0, 0);
    b_u32(b, 1);
    size_t mp4v = begin_box(b, "mp4v");
    b_zero(b, 6);
    b_u16(b, 1);                                    /* data reference index */
    b_zero(b, 16);                                  /* pre_defined, reserved */
    b_u16(b, (unsigned)m->w);
    b_u16(b, (unsigned)m->h);
    b_u32(b, 0x00480000); b_u32(b, 0x00480000);     /* 72 dpi */
    b_u32(b, 0);
    b_u16(b, 1);                                    /* frame count */
    b_zero(b, 32);                                  /* compressor name */
    b_u16(b, 0x18);                                 /* depth */
    b_u16(b, 0xffff);
    size_t esds = begin_full(b, "esds", 0, 0);
    b_u8(b, 0x03); b_u8(b, 21);                     /* ES_Descriptor */
    b_u16(b, 1); b_u8(b, 0);                        /* ES id, flags */
    b_u8(b, 0x04); b_u8(b, 13);                     /* DecoderConfigDescriptor */
    b_u8(b, 0x6c);                                  /* object type: JPEG */
    b_u8(b, 0x11);                                  /* stream type: visual */
    b_u8(b, 0); b_u16(b, 0);                        /* buffer size */
    b_u32(b, 0); b_u32(b, 0);                       /* max / average bit rate (unknown) */
    b_u8(b, 0x06); b_u8(b, 1); b_u8(b, 0x02);       /* SLConfigDescriptor */
    end_box(b, esds);
    end_box(b, mp4v);
    end_box(b, stsd);

    size_t stts = begin_full(b, "stts", 0, 0);      /* run length coded durations */
    size_t count_at = b->n;
    b_u32(b, 0);
    uint32_t runs = 0;
    for (uint32_t i = 0; i < m->n;) {
        uint64_t end = i + 1 < m->n ? m->pts[i + 1] : duration;
        uint32_t d = end > m->pts[i] ? (uint32_t)(end - m->pts[i]) : 1;
        uint32_t j = i + 1;
        while (j < m->n) {
            uint64_t e2 = j + 1 < m->n ? m->pts[j + 1] : duration;
            uint32_t d2 = e2 > m->pts[j] ? (uint32_t)(e2 - m->pts[j]) : 1;
            if (d2 != d)
                break;
            j++;
        }
        b_u32(b, j - i);
        b_u32(b, d);
        runs++;
        i = j;
    }
    if (!b->oom) {
        b->p[count_at] = (uint8_t)(runs >> 24); b->p[count_at + 1] = (uint8_t)(runs >> 16);
        b->p[count_at + 2] = (uint8_t)(runs >> 8); b->p[count_at + 3] = (uint8_t)runs;
    }
    end_box(b, stts);

    size_t stsc = begin_full(b, "stsc", 0, 0);      /* all pictures are one chunk */
    b_u32(b, 1);
    b_u32(b, 1); b_u32(b, m->n); b_u32(b, 1);
    end_box(b, stsc);

    size_t stsz = begin_full(b, "stsz", 0, 0);
    b_u32(b, 0);
    b_u32(b, m->n);
    for (uint32_t i = 0; i < m->n; i++)
        b_u32(b, m->size[i]);
    end_box(b, stsz);

    if (first_offset > 0xffffffffu) {
        size_t co = begin_full(b, "co64", 0, 0);
        b_u32(b, 1);
        b_u64(b, first_offset);
        end_box(b, co);
    } else {
        size_t co = begin_full(b, "stco", 0, 0);
        b_u32(b, 1);
        b_u32(b, (uint32_t)first_offset);
        end_box(b, co);
    }
    end_box(b, stbl);
    end_box(b, minf);
    end_box(b, mdia);
    end_box(b, trak);
    end_box(b, moov);
}

/* ---- file ----------------------------------------------------------------------------------------------------------- */

static int put(struct lm_mp4 *m, const void *d, size_t n)
{
    if (m->failed)
        return -1;
    if (fwrite(d, 1, n, m->f) != n) {
        m->failed = 1;
        return -1;
    }
    m->pos += n;
    return 0;
}

struct lm_mp4 *lm_mp4_create(const char *path, int width, int height)
{
    if (!path || width < 1 || height < 1 || width > 65535 || height > 65535) {
        errno = EINVAL;
        return NULL;
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0)
        return NULL;
    struct lm_mp4 *m = calloc(1, sizeof(*m));
    FILE *f = m ? fdopen(fd, "wb") : NULL;
    if (!m || !f) {
        int e = errno;
        if (f)
            fclose(f);
        else
            close(fd);
        unlink(path);
        free(m);
        errno = e ? e : ENOMEM;
        return NULL;
    }
    m->path = strdup(path);
    m->f = f;
    m->w = width;
    m->h = height;
    struct buf b = { 0 };
    size_t ftyp = begin_box(&b, "ftyp");
    b_tag(&b, "isom"); b_u32(&b, 512);
    b_tag(&b, "isom"); b_tag(&b, "iso2"); b_tag(&b, "mp41");
    end_box(&b, ftyp);
    b_u32(&b, 1);                                   /* mdat with a 64 bit size, patched at the end */
    b_tag(&b, "mdat");
    b_u64(&b, 16);
    if (b.oom || !m->path || put(m, b.p, b.n) < 0) {
        free(b.p);
        fclose(f);
        unlink(path);
        free(m->path);
        free(m);
        errno = ENOMEM;
        return NULL;
    }
    free(b.p);
    m->data_start = m->pos;
    return m;
}

int lm_mp4_add(struct lm_mp4 *m, const uint8_t *jpeg, size_t len, uint64_t pts_ms)
{
    if (!m || m->failed || !jpeg || len == 0 || len > 0x7fffffffu)
        return -1;
    if (m->n == m->cap) {
        uint32_t nc = m->cap ? m->cap * 2 : 1024;
        uint32_t *ns = realloc(m->size, (size_t)nc * sizeof(*ns));
        if (ns)
            m->size = ns;
        uint64_t *np = ns ? realloc(m->pts, (size_t)nc * sizeof(*np)) : NULL;
        if (!ns || !np) {
            m->failed = 1;
            return -1;
        }
        m->pts = np;
        m->cap = nc;
    }
    if (m->n == 0)
        m->first_pts = pts_ms;
    uint64_t rel = pts_ms > m->first_pts ? pts_ms - m->first_pts : 0;
    if (m->n > 0 && rel <= m->pts[m->n - 1])
        rel = m->pts[m->n - 1] + 1;                 /* every picture lasts at least a millisecond */
    if (put(m, jpeg, len) < 0)
        return -1;
    m->size[m->n] = (uint32_t)len;
    m->pts[m->n] = rel;
    m->n++;
    return 0;
}

uint32_t lm_mp4_frames(const struct lm_mp4 *m) { return m ? m->n : 0; }
uint64_t lm_mp4_bytes(const struct lm_mp4 *m) { return m ? m->pos : 0; }

int lm_mp4_close(struct lm_mp4 *m, uint64_t end_ms)
{
    if (!m)
        return -1;
    int rc = -1;
    if (m->n > 0 && !m->failed) {
        uint64_t end = end_ms > m->first_pts ? end_ms - m->first_pts : 0;
        if (end <= m->pts[m->n - 1])
            end = m->pts[m->n - 1] + 1;
        uint64_t mdat_size = m->pos - (m->data_start - 16);
        struct buf b = { 0 };
        build_moov(&b, m, end, m->data_start);
        if (!b.oom && put(m, b.p, b.n) == 0) {
            uint8_t sz[8];
            for (int i = 0; i < 8; i++)
                sz[i] = (uint8_t)(mdat_size >> (56 - 8 * i));
            if (fflush(m->f) == 0 && fseeko(m->f, (off_t)(m->data_start - 8), SEEK_SET) == 0 && fwrite(sz, 1, 8, m->f) == 8)
                rc = 0;
        }
        free(b.p);
    }
    if (fclose(m->f) != 0)
        rc = -1;
    if (rc < 0)
        unlink(m->path);
    free(m->path);
    free(m->size);
    free(m->pts);
    free(m);
    return rc;
}
