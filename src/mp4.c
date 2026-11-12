#include "mp4.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
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
    uint8_t *sps, *pps;             /* H.264: the parameter sets (NAL units) */
    size_t sps_n, pps_n;
    uint8_t *key;                   /* H.264: per picture, 1 = IDR */
    pthread_mutex_t lock;
    /* the sound track */
    int arate, ach;                 /* 0 = none */
    uint32_t anc, anc_cap;          /* chunks of sound: one per lm_mp4_add_audio call */
    uint64_t *aoff;                 /* file offset of each chunk */
    uint32_t *aframes;              /* PCM frames in each chunk */
    uint64_t atotal;                /* PCM frames so far */
    uint64_t afirst_pts;
    int afirst_set;
    uint64_t *voff;                 /* with sound the pictures are interleaved with it: the file offset of each picture */
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

static void patch_u32(struct buf *b, size_t at, uint32_t v)
{
    if (b->oom)
        return;
    b->p[at] = (uint8_t)(v >> 24); b->p[at + 1] = (uint8_t)(v >> 16); b->p[at + 2] = (uint8_t)(v >> 8); b->p[at + 3] = (uint8_t)v;
}

/* An edit list that starts the track `offset_ms` late (an empty edit, then the whole media): how the sound and the pictures keep their places in time. */
static void put_edts(struct buf *b, uint64_t offset_ms, uint64_t media_ms)
{
    if (offset_ms == 0)
        return;
    size_t edts = begin_box(b, "edts");
    size_t elst = begin_full(b, "elst", 0, 0);
    b_u32(b, 2);
    b_u32(b, (uint32_t)offset_ms); b_u32(b, 0xffffffffu); b_u16(b, 1); b_u16(b, 0);          /* empty edit */
    b_u32(b, (uint32_t)media_ms); b_u32(b, 0); b_u16(b, 1); b_u16(b, 0);                    /* the media */
    end_box(b, elst);
    end_box(b, edts);
}

static const uint32_t unity[9] = { 0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000 };

static void build_moov(struct buf *b, const struct lm_mp4 *m, uint64_t duration, uint64_t first_offset)
{
    uint32_t dur = duration > 0xffffffffu ? 0xffffffffu : (uint32_t)duration;       /* the pictures' media */
    uint64_t origin = m->first_pts, vofs = 0, aofs = 0, adur_ms = 0;                /* sound: both tracks count from the earlier of the two starts */
    if (m->arate) {
        if (m->afirst_set && m->afirst_pts < origin)
            origin = m->afirst_pts;
        vofs = m->first_pts - origin;
        aofs = m->afirst_set ? m->afirst_pts - origin : 0;
        adur_ms = m->atotal * 1000 / (uint64_t)m->arate;
    }
    uint64_t movie = vofs + duration > aofs + adur_ms ? vofs + duration : aofs + adur_ms;
    uint32_t mdur = movie > 0xffffffffu ? 0xffffffffu : (uint32_t)movie;
    size_t moov = begin_box(b, "moov");

    size_t mvhd = begin_full(b, "mvhd", 0, 0);
    b_u32(b, 0); b_u32(b, 0);                       /* creation, modification (unknown: 0) */
    b_u32(b, 1000);
    b_u32(b, mdur);
    b_u32(b, 0x00010000);                           /* rate 1.0 */
    b_u16(b, 0x0100);                               /* volume */
    b_zero(b, 10);
    for (int i = 0; i < 9; i++) b_u32(b, unity[i]);
    b_zero(b, 24);                                  /* pre_defined */
    b_u32(b, m->arate ? 3 : 2);                     /* next track id */
    end_box(b, mvhd);

    size_t trak = begin_box(b, "trak");
    size_t tkhd = begin_full(b, "tkhd", 0, 3);      /* enabled, in the movie */
    b_u32(b, 0); b_u32(b, 0);
    b_u32(b, 1);                                    /* track id */
    b_u32(b, 0);
    b_u32(b, (uint32_t)(vofs + dur));
    b_zero(b, 8);
    b_u16(b, 0); b_u16(b, 0); b_u16(b, 0); b_u16(b, 0);   /* layer, alternate group, volume, reserved */
    for (int i = 0; i < 9; i++) b_u32(b, unity[i]);
    b_u32(b, (uint32_t)m->w << 16);
    b_u32(b, (uint32_t)m->h << 16);
    end_box(b, tkhd);
    put_edts(b, vofs, dur);

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
    size_t mp4v = begin_box(b, m->sps ? "avc1" : "mp4v");
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
    if (m->sps) {
        size_t avcc = begin_box(b, "avcC");
        b_u8(b, 1);                                 /* configurationVersion */
        b_u8(b, m->sps[1]); b_u8(b, m->sps[2]); b_u8(b, m->sps[3]);      /* profile, compatibility, level of the SPS */
        b_u8(b, 0xff);                              /* 4 byte NAL unit lengths */
        b_u8(b, 0xe1);                              /* one SPS */
        b_u16(b, (unsigned)m->sps_n);
        b_bytes(b, m->sps, m->sps_n);
        b_u8(b, 1);                                 /* one PPS */
        b_u16(b, (unsigned)m->pps_n);
        b_bytes(b, m->pps, m->pps_n);
        end_box(b, avcc);
    } else {
        size_t esds = begin_full(b, "esds", 0, 0);
        b_u8(b, 0x03); b_u8(b, 21);                 /* ES_Descriptor */
        b_u16(b, 1); b_u8(b, 0);                    /* ES id, flags */
        b_u8(b, 0x04); b_u8(b, 13);                 /* DecoderConfigDescriptor */
        b_u8(b, 0x6c);                              /* object type: JPEG */
        b_u8(b, 0x11);                              /* stream type: visual */
        b_u8(b, 0); b_u16(b, 0);                    /* buffer size */
        b_u32(b, 0); b_u32(b, 0);                   /* max / average bit rate (unknown) */
        b_u8(b, 0x06); b_u8(b, 1); b_u8(b, 0x02);   /* SLConfigDescriptor */
        end_box(b, esds);
    }
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

    if (m->sps) {                                   /* the pictures a player can start at */
        uint32_t nk = 0;
        for (uint32_t i = 0; i < m->n; i++)
            nk += m->key[i] != 0;
        size_t stss = begin_full(b, "stss", 0, 0);
        b_u32(b, nk);
        for (uint32_t i = 0; i < m->n; i++)
            if (m->key[i])
                b_u32(b, i + 1);
        end_box(b, stss);
    }

    size_t stsc = begin_full(b, "stsc", 0, 0);      /* without sound all pictures are one chunk, with it one each (the sound sits between them) */
    b_u32(b, 1);
    b_u32(b, 1); b_u32(b, m->arate ? 1 : m->n); b_u32(b, 1);
    end_box(b, stsc);

    size_t stsz = begin_full(b, "stsz", 0, 0);
    b_u32(b, 0);
    b_u32(b, m->n);
    for (uint32_t i = 0; i < m->n; i++)
        b_u32(b, m->size[i]);
    end_box(b, stsz);

    uint32_t nchunks = m->arate ? m->n : 1;
    bool big = first_offset > 0xffffffffu || (m->arate && m->n && m->voff[m->n - 1] > 0xffffffffu);
    size_t co = begin_full(b, big ? "co64" : "stco", 0, 0);
    b_u32(b, nchunks);
    for (uint32_t i = 0; i < nchunks; i++) {
        uint64_t off = m->arate ? m->voff[i] : first_offset;
        if (big)
            b_u64(b, off);
        else
            b_u32(b, (uint32_t)off);
    }
    end_box(b, co);
    end_box(b, stbl);
    end_box(b, minf);
    end_box(b, mdia);
    end_box(b, trak);

    if (m->arate) {                                 /* the sound track: 16 bit PCM ("sowt"), one chunk per write */
        size_t atrak = begin_box(b, "trak");
        size_t atkhd = begin_full(b, "tkhd", 0, 3);
        b_u32(b, 0); b_u32(b, 0);
        b_u32(b, 2);
        b_u32(b, 0);
        b_u32(b, (uint32_t)(aofs + adur_ms));
        b_zero(b, 8);
        b_u16(b, 0); b_u16(b, 1); b_u16(b, 0x0100); b_u16(b, 0);   /* layer, alternate group, volume 1.0, reserved */
        for (int i = 0; i < 9; i++) b_u32(b, unity[i]);
        b_u32(b, 0); b_u32(b, 0);
        end_box(b, atkhd);
        put_edts(b, aofs, adur_ms);
        size_t amdia = begin_box(b, "mdia");
        size_t amdhd = begin_full(b, "mdhd", 0, 0);
        b_u32(b, 0); b_u32(b, 0);
        b_u32(b, (uint32_t)m->arate);
        b_u32(b, (uint32_t)m->atotal);
        b_u16(b, 0x55c4);
        b_u16(b, 0);
        end_box(b, amdhd);
        size_t ahdlr = begin_full(b, "hdlr", 0, 0);
        b_u32(b, 0);
        b_tag(b, "soun");
        b_zero(b, 12);
        b_bytes(b, "MellaGUI sound\0", 15);
        end_box(b, ahdlr);
        size_t aminf = begin_box(b, "minf");
        size_t smhd = begin_full(b, "smhd", 0, 0);
        b_u16(b, 0); b_u16(b, 0);
        end_box(b, smhd);
        size_t adinf = begin_box(b, "dinf");
        size_t adref = begin_full(b, "dref", 0, 0);
        b_u32(b, 1);
        size_t aurl = begin_full(b, "url ", 0, 1);
        end_box(b, aurl);
        end_box(b, adref);
        end_box(b, adinf);
        size_t astbl = begin_box(b, "stbl");
        size_t astsd = begin_full(b, "stsd", 0, 0);
        b_u32(b, 1);
        size_t sowt = begin_box(b, "sowt");
        b_zero(b, 6);
        b_u16(b, 1);                                /* data reference index */
        b_u16(b, 0); b_u16(b, 0); b_u32(b, 0);      /* QuickTime sound description version 0: version, revision, vendor */
        b_u16(b, (unsigned)m->ach);
        b_u16(b, 16);                               /* bits per sample */
        b_u16(b, 0); b_u16(b, 0);                   /* compression id, packet size */
        b_u32(b, (uint32_t)m->arate << 16);         /* sample rate, 16.16 */
        end_box(b, sowt);
        end_box(b, astsd);
        size_t astts = begin_full(b, "stts", 0, 0); /* every PCM frame lasts one tick of the media's clock */
        b_u32(b, 1);
        b_u32(b, (uint32_t)m->atotal); b_u32(b, 1);
        end_box(b, astts);
        size_t astsc = begin_full(b, "stsc", 0, 0);
        size_t runs_at = b->n;
        b_u32(b, 0);
        uint32_t runs = 0;
        for (uint32_t i = 0; i < m->anc; i++) {
            if (i > 0 && m->aframes[i] == m->aframes[i - 1])
                continue;                           /* the same count as the chunk before: the entry before still holds */
            b_u32(b, i + 1); b_u32(b, m->aframes[i]); b_u32(b, 1);
            runs++;
        }
        patch_u32(b, runs_at, runs);
        end_box(b, astsc);
        size_t astsz = begin_full(b, "stsz", 0, 0);
        b_u32(b, (uint32_t)m->ach * 2);             /* every sample (one PCM frame) has the same size */
        b_u32(b, (uint32_t)m->atotal);
        end_box(b, astsz);
        bool abig = m->anc && m->aoff[m->anc - 1] > 0xffffffffu;
        size_t aco = begin_full(b, abig ? "co64" : "stco", 0, 0);
        b_u32(b, m->anc);
        for (uint32_t i = 0; i < m->anc; i++) {
            if (abig)
                b_u64(b, m->aoff[i]);
            else
                b_u32(b, (uint32_t)m->aoff[i]);
        }
        end_box(b, aco);
        end_box(b, astbl);
        end_box(b, aminf);
        end_box(b, amdia);
        end_box(b, atrak);
    }
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
    pthread_mutex_init(&m->lock, NULL);
    m->path = strdup(path);
    m->f = f;
    m->w = width;
    m->h = height;
    struct buf b = { 0 };
    size_t ftyp = begin_box(&b, "ftyp");
    b_tag(&b, "isom"); b_u32(&b, 512);
    b_tag(&b, "isom"); b_tag(&b, "iso2"); b_tag(&b, "avc1"); b_tag(&b, "mp41");
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

int lm_mp4_set_h264(struct lm_mp4 *m, const uint8_t *sps, size_t sps_len, const uint8_t *pps, size_t pps_len)
{
    if (!m || m->n || sps_len < 4 || sps_len > 255 || pps_len < 1 || pps_len > 255)
        return -1;
    m->sps = malloc(sps_len);
    m->pps = malloc(pps_len);
    if (!m->sps || !m->pps) {
        free(m->sps);
        free(m->pps);
        m->sps = m->pps = NULL;
        return -1;
    }
    memcpy(m->sps, sps, sps_len);
    memcpy(m->pps, pps, pps_len);
    m->sps_n = sps_len;
    m->pps_n = pps_len;
    return 0;
}

int lm_mp4_add(struct lm_mp4 *m, const uint8_t *jpeg, size_t len, uint64_t pts_ms)
{
    return lm_mp4_add_sample(m, jpeg, len, pts_ms, 1);
}

static int add_sample_locked(struct lm_mp4 *m, const uint8_t *jpeg, size_t len, uint64_t pts_ms, int key)
{
    if (!m || m->failed || !jpeg || len == 0 || len > 0x7fffffffu)
        return -1;
    if (m->n == m->cap) {
        uint32_t nc = m->cap ? m->cap * 2 : 1024;
        uint32_t *ns = realloc(m->size, (size_t)nc * sizeof(*ns));
        if (ns)
            m->size = ns;
        uint64_t *np = ns ? realloc(m->pts, (size_t)nc * sizeof(*np)) : NULL;
        uint8_t *nk = np ? realloc(m->key, nc) : NULL;
        uint64_t *nv = nk ? realloc(m->voff, (size_t)nc * sizeof(*nv)) : NULL;
        if (!ns || !np || !nk || !nv) {
            m->failed = 1;
            return -1;
        }
        m->pts = np;
        m->key = nk;
        m->voff = nv;
        m->cap = nc;
    }
    m->voff[m->n] = m->pos;
    if (m->n == 0)
        m->first_pts = pts_ms;
    uint64_t rel = pts_ms > m->first_pts ? pts_ms - m->first_pts : 0;
    if (m->n > 0 && rel <= m->pts[m->n - 1])
        rel = m->pts[m->n - 1] + 1;                 /* every picture lasts at least a millisecond */
    if (put(m, jpeg, len) < 0)
        return -1;
    m->size[m->n] = (uint32_t)len;
    m->key[m->n] = key != 0;
    m->pts[m->n] = rel;
    m->n++;
    return 0;
}

int lm_mp4_add_sample(struct lm_mp4 *m, const uint8_t *jpeg, size_t len, uint64_t pts_ms, int key)
{
    if (!m)
        return -1;
    pthread_mutex_lock(&m->lock);
    int rc = add_sample_locked(m, jpeg, len, pts_ms, key);
    pthread_mutex_unlock(&m->lock);
    return rc;
}

int lm_mp4_set_audio(struct lm_mp4 *m, int rate, int channels)
{
    if (!m || rate < 8000 || rate > 192000 || channels < 1 || channels > 2)
        return -1;
    pthread_mutex_lock(&m->lock);
    int rc = -1;
    if (m->n == 0 && m->anc == 0) {
        m->arate = rate;
        m->ach = channels;
        rc = 0;
    }
    pthread_mutex_unlock(&m->lock);
    return rc;
}

int lm_mp4_add_audio(struct lm_mp4 *m, const int16_t *pcm, size_t frames, uint64_t pts_ms)
{
    if (!m || !pcm || frames == 0 || frames > 0x1000000u)
        return -1;
    pthread_mutex_lock(&m->lock);
    int rc = -1;
    if (m->arate && !m->failed) {
        if (m->anc == m->anc_cap) {
            uint32_t nc = m->anc_cap ? m->anc_cap * 2 : 1024;
            uint64_t *no = realloc(m->aoff, (size_t)nc * sizeof(*no));
            if (no)
                m->aoff = no;
            uint32_t *nf = no ? realloc(m->aframes, (size_t)nc * sizeof(*nf)) : NULL;
            if (nf)
                m->aframes = nf;
            if (!no || !nf) {
                m->failed = 1;
            } else {
                m->anc_cap = nc;
            }
        }
        if (!m->failed) {
            if (!m->afirst_set) {
                m->afirst_pts = pts_ms;
                m->afirst_set = 1;
            }
            size_t bytes = frames * (size_t)m->ach * 2;
            uint64_t at = m->pos;
            uint8_t *le = NULL;
            const uint8_t *src = (const uint8_t *)pcm;
            const uint16_t probe = 1;
            if (*(const uint8_t *)&probe != 1) {                 /* a big endian host: the samples are written little endian */
                le = malloc(bytes);
                if (le) {
                    for (size_t i = 0; i < bytes; i += 2) {
                        le[i] = ((const uint8_t *)pcm)[i + 1];
                        le[i + 1] = ((const uint8_t *)pcm)[i];
                    }
                    src = le;
                }
            }
            if (put(m, src, bytes) == 0) {
                m->aoff[m->anc] = at;
                m->aframes[m->anc] = (uint32_t)frames;
                m->anc++;
                m->atotal += frames;
                rc = 0;
            }
            free(le);
        }
    }
    pthread_mutex_unlock(&m->lock);
    return rc;
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
    free(m->key);
    free(m->sps);
    free(m->pps);
    free(m->aoff);
    free(m->aframes);
    free(m->voff);
    pthread_mutex_destroy(&m->lock);
    free(m);
    return rc;
}
