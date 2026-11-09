#include "mp4read.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define MAX_MOOV (256u * 1024u * 1024u)
#define MAX_SAMPLES 20000000u
#define MAX_SAMPLE_BYTES (128u * 1024u * 1024u)

struct span {
    const uint8_t *p;
    size_t n;
};

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }
static unsigned be16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }

/* The next box in `s` starting at *at: its type, its payload. false at the end / on a damaged size. */
static bool next_box(const struct span *s, size_t *at, char type[4], struct span *body)
{
    if (*at + 8 > s->n)
        return false;
    uint64_t size = be32(s->p + *at);
    size_t hdr = 8;
    if (size == 1) {
        if (*at + 16 > s->n)
            return false;
        size = be64(s->p + *at + 8);
        hdr = 16;
    } else if (size == 0) {
        size = s->n - *at;
    }
    if (size < hdr || size > s->n - *at)
        return false;
    memcpy(type, s->p + *at + 4, 4);
    body->p = s->p + *at + hdr;
    body->n = (size_t)size - hdr;
    *at += (size_t)size;
    return true;
}

static bool find_box(const struct span *s, const char *type, struct span *body)
{
    size_t at = 0;
    char t[4];
    struct span b;
    while (next_box(s, &at, t, &b))
        if (!memcmp(t, type, 4)) {
            *body = b;
            return true;
        }
    return false;
}

/* esds -> the object type indication of the DecoderConfigDescriptor, or -1. */
static int esds_object_type(const struct span *esds)
{
    const uint8_t *p = esds->p + 4;                 /* version / flags */
    const uint8_t *end = esds->p + esds->n;
    if (esds->n < 6 || *p++ != 0x03)
        return -1;
    for (int i = 0; i < 4 && p < end; i++)          /* the variable length size */
        if (!(*p++ & 0x80))
            break;
    if (end - p < 3)
        return -1;
    unsigned flags = p[2];
    p += 3;
    if (flags & 0x80)
        p += 2;
    if (flags & 0x40 && p < end)
        p += 1 + *p;
    if (flags & 0x20)
        p += 2;
    if (p >= end || *p++ != 0x04)
        return -1;
    for (int i = 0; i < 4 && p < end; i++)
        if (!(*p++ & 0x80))
            break;
    return p < end ? *p : -1;
}

void lm_mp4r_close(struct lm_mp4r *r)
{
    if (!r)
        return;
    if (r->f)
        fclose(r->f);
    free(r->off);
    free(r->size);
    free(r->pts_ms);
    free(r);
}

#define FAIL(...) do { snprintf(err, errsz, __VA_ARGS__); goto fail; } while (0)

/* Parses the 'trak' of a video track into r. 1 = used, 0 = not a video track, -1 = damaged (err set). */
static int parse_trak(struct lm_mp4r *r, const struct span *trak, char *err, size_t errsz)
{
    struct span mdia, hdlr, mdhd, minf, stbl, stsd, stts, stsz, stsc, stco;
    bool co64 = false;
    if (!find_box(trak, "mdia", &mdia) || !find_box(&mdia, "hdlr", &hdlr) || hdlr.n < 12 || memcmp(hdlr.p + 8, "vide", 4))
        return 0;
    if (!find_box(&mdia, "mdhd", &mdhd) || mdhd.n < 24)
        FAIL("damaged file (no media header)");
    uint64_t timescale = mdhd.p[0] == 1 ? be32(mdhd.p + 20) : be32(mdhd.p + 12);
    if (timescale == 0)
        FAIL("damaged file (time scale 0)");
    if (!find_box(&mdia, "minf", &minf) || !find_box(&minf, "stbl", &stbl) || !find_box(&stbl, "stsd", &stsd) ||
        !find_box(&stbl, "stts", &stts) || !find_box(&stbl, "stsz", &stsz) || !find_box(&stbl, "stsc", &stsc))
        FAIL("damaged file (incomplete sample table; a fragmented MP4?)");
    if (!find_box(&stbl, "stco", &stco)) {
        if (!find_box(&stbl, "co64", &stco))
            FAIL("damaged file (no chunk offsets)");
        co64 = true;
    }
    /* the sample description */
    if (stsd.n < 8 + 8 + 78 || be32(stsd.p + 4) < 1)
        FAIL("damaged file (no sample description)");
    struct span entry = { stsd.p + 8, stsd.n - 8 };
    size_t at = 0;
    char type[4];
    struct span ebody;
    if (!next_box(&entry, &at, type, &ebody) || ebody.n < 78)
        FAIL("damaged file (sample description)");
    memcpy(r->codec, type, 4);
    r->codec[4] = 0;
    r->w = (int)be16(ebody.p + 24);
    r->h = (int)be16(ebody.p + 26);
    r->mjpeg = !memcmp(type, "jpeg", 4) || !memcmp(type, "MJPG", 4) || !memcmp(type, "mjpg", 4);
    if (!memcmp(type, "mp4v", 4)) {
        struct span ext = { ebody.p + 78, ebody.n - 78 }, esds;
        r->mjpeg = find_box(&ext, "esds", &esds) && esds_object_type(&esds) == 0x6c;
    }
    /* sample sizes */
    if (stsz.n < 12)
        FAIL("damaged file (sample sizes)");
    uint32_t constant = be32(stsz.p + 4), n = be32(stsz.p + 8);
    if (n == 0 || n > MAX_SAMPLES || (constant == 0 && stsz.n < 12 + (size_t)n * 4))
        FAIL("damaged file (%u samples)", (unsigned)n);
    r->n = n;
    r->size = malloc((size_t)n * sizeof(*r->size));
    r->off = malloc((size_t)n * sizeof(*r->off));
    r->pts_ms = malloc((size_t)n * sizeof(*r->pts_ms));
    if (!r->size || !r->off || !r->pts_ms)
        FAIL("out of memory");
    for (uint32_t i = 0; i < n; i++) {
        r->size[i] = constant ? constant : be32(stsz.p + 12 + (size_t)i * 4);
        if (r->size[i] > MAX_SAMPLE_BYTES)
            FAIL("damaged file (a sample of %u bytes)", (unsigned)r->size[i]);
    }
    /* times */
    if (stts.n < 8)
        FAIL("damaged file (sample times)");
    uint32_t runs = be32(stts.p + 4);
    if (stts.n < 8 + (size_t)runs * 8)
        FAIL("damaged file (sample times)");
    uint64_t t = 0;
    uint32_t i = 0;
    for (uint32_t k = 0; k < runs && i < n; k++) {
        uint32_t count = be32(stts.p + 8 + (size_t)k * 8), delta = be32(stts.p + 12 + (size_t)k * 8);
        for (uint32_t j = 0; j < count && i < n; j++, i++) {
            r->pts_ms[i] = t * 1000 / timescale;
            t += delta;
        }
    }
    if (i < n)
        FAIL("damaged file (times for %u of %u samples)", (unsigned)i, (unsigned)n);
    r->duration_ms = t * 1000 / timescale;
    /* offsets: chunks, and how many samples each holds */
    if (stsc.n < 8 || stco.n < 8)
        FAIL("damaged file (chunk table)");
    uint32_t nsc = be32(stsc.p + 4), nchunks = be32(stco.p + 4);
    size_t osz = co64 ? 8 : 4;
    if (nsc == 0 || stsc.n < 8 + (size_t)nsc * 12 || stco.n < 8 + (size_t)nchunks * osz)
        FAIL("damaged file (chunk table)");
    uint32_t s = 0;
    for (uint32_t c = 0; c < nchunks && s < n; c++) {
        uint32_t per = 0;
        for (uint32_t k = 0; k < nsc; k++)           /* the last stsc entry that starts at or before chunk c + 1 */
            if (be32(stsc.p + 8 + (size_t)k * 12) <= c + 1)
                per = be32(stsc.p + 12 + (size_t)k * 12);
        uint64_t o = co64 ? be64(stco.p + 8 + (size_t)c * 8) : be32(stco.p + 8 + (size_t)c * 4);
        for (uint32_t j = 0; j < per && s < n; j++, s++) {
            r->off[s] = o;
            if (o > r->file_size || r->size[s] > r->file_size - o)
                FAIL("damaged file (a picture lies beyond the end of the file)");
            o += r->size[s];
        }
    }
    if (s < n)
        FAIL("damaged file (offsets for %u of %u samples)", (unsigned)s, (unsigned)n);
    return 1;
fail:
    return -1;
}

struct lm_mp4r *lm_mp4r_open(const char *path, char *err, size_t errsz)
{
    struct lm_mp4r *r = calloc(1, sizeof(*r));
    uint8_t *moov = NULL;
    if (!r)
        FAIL("out of memory");
    r->f = fopen(path, "rb");
    if (!r->f)
        FAIL("%s", strerror(errno));
    if (fseeko(r->f, 0, SEEK_END) != 0)
        FAIL("%s", strerror(errno));
    off_t sz = ftello(r->f);
    if (sz < 16)
        FAIL("not an MP4 file");
    r->file_size = (uint64_t)sz;
    /* the top level boxes, without reading the mdat */
    uint64_t at = 0;
    size_t moov_n = 0;
    bool saw_ftyp = false;
    while (at + 8 <= r->file_size) {
        uint8_t h[16];
        if (fseeko(r->f, (off_t)at, SEEK_SET) != 0 || fread(h, 1, 8, r->f) != 8)
            break;
        uint64_t size = be32(h);
        size_t hdr = 8;
        if (size == 1) {
            if (fread(h + 8, 1, 8, r->f) != 8)
                break;
            size = be64(h + 8);
            hdr = 16;
        } else if (size == 0) {
            size = r->file_size - at;
        }
        if (size < hdr || size > r->file_size - at)
            FAIL(!moov && saw_ftyp ? "damaged file (incomplete; a recording that was never finished?)" : "not an MP4 file");
        if (!memcmp(h + 4, "ftyp", 4) || !memcmp(h + 4, "styp", 4))
            saw_ftyp = true;
        if (!memcmp(h + 4, "moof", 4))
            FAIL("fragmented MP4 files are not supported");
        if (!memcmp(h + 4, "moov", 4)) {
            if (size - hdr > MAX_MOOV)
                FAIL("damaged file (huge index)");
            moov_n = (size_t)(size - hdr);
            moov = malloc(moov_n ? moov_n : 1);
            if (!moov)
                FAIL("out of memory");
            if (fseeko(r->f, (off_t)(at + hdr), SEEK_SET) != 0 || fread(moov, 1, moov_n, r->f) != moov_n)
                FAIL("damaged file (index)");
            break;
        }
        at += size;
    }
    if (!saw_ftyp)
        FAIL("not an MP4 file");
    if (!moov)
        FAIL("no index (moov): a recording that was never finished?");
    struct span ms = { moov, moov_n };
    size_t pos = 0;
    char type[4];
    struct span body;
    int used = 0;
    while (!used && next_box(&ms, &pos, type, &body))
        if (!memcmp(type, "trak", 4)) {
            used = parse_trak(r, &body, err, errsz);
            if (used < 0)
                goto fail;
        }
    if (!used)
        FAIL("no video track");
    free(moov);
    return r;
fail:
    free(moov);
    lm_mp4r_close(r);
    return NULL;
}

uint8_t *lm_mp4r_sample(struct lm_mp4r *r, uint32_t i, size_t *len)
{
    if (!r || i >= r->n || r->size[i] == 0)
        return NULL;
    uint8_t *buf = malloc(r->size[i]);
    if (!buf)
        return NULL;
    if (fseeko(r->f, (off_t)r->off[i], SEEK_SET) != 0 || fread(buf, 1, r->size[i], r->f) != r->size[i]) {
        free(buf);
        return NULL;
    }
    *len = r->size[i];
    return buf;
}

uint32_t lm_mp4r_frame_at(const struct lm_mp4r *r, uint64_t ms)
{
    uint32_t lo = 0, hi = r->n;                     /* the first sample with pts > ms, minus one */
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (r->pts_ms[mid] <= ms)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo ? lo - 1 : 0;
}
