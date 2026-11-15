#include "deflate.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* ---- CRC-32 ---- */
uint32_t lm_crc32(uint32_t crc, const uint8_t *p, size_t n)
{
    static uint32_t t[8][256];
    static bool init;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            t[0][i] = c;
        }
        for (uint32_t i = 0; i < 256; i++)
            for (int s = 1; s < 8; s++)
                t[s][i] = t[0][t[s - 1][i] & 0xff] ^ (t[s - 1][i] >> 8);
        init = true;
    }
    crc = ~crc;
    while (n >= 8) {
        uint32_t a = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
        a ^= crc;
        crc = t[7][a & 0xff] ^ t[6][(a >> 8) & 0xff] ^ t[5][(a >> 16) & 0xff] ^ t[4][a >> 24] ^ t[3][p[4]] ^ t[2][p[5]] ^ t[1][p[6]] ^ t[0][p[7]];
        p += 8;
        n -= 8;
    }
    while (n--)
        crc = t[0][(crc ^ *p++) & 0xff] ^ (crc >> 8);
    return ~crc;
}

/* ---- the compressor ---- */
#define WIN 32768
#define CHUNK (1u << 20)
#define HASH_BITS 15
#define HASH_SIZE (1u << HASH_BITS)
#define MAX_TOKENS 32768
#define MIN_MATCH 3
#define MAX_MATCH 258

struct tok {
    uint16_t dist;                      /* 0: a literal (`lit`) */
    uint16_t lit;                       /* the literal, or the match length */
};

struct lm_deflater {
    int level;
    lm_deflate_sink sink;
    void *ctx;
    bool failed;
    uint8_t *buf;                       /* history + new data */
    size_t have;                        /* bytes in buf */
    size_t hist;                        /* bytes at the start of buf that are history (already coded) */
    int32_t *head, *prev;
    struct tok *toks;
    int ntok;
    uint64_t acc;
    int nacc;
    uint8_t out[1 << 16];
    size_t outn;
    uint32_t lfreq[286], dfreq[30];
    int max_chain, nice, lazy_min;
    bool lazy;
};

static void flush_out(struct lm_deflater *d)
{
    if (d->outn && !d->failed && d->sink(d->ctx, d->out, d->outn))
        d->failed = true;
    d->outn = 0;
}

static void put_bits(struct lm_deflater *d, uint32_t v, int n)
{
    d->acc |= (uint64_t)v << d->nacc;
    d->nacc += n;
    while (d->nacc >= 8) {
        d->out[d->outn++] = (uint8_t)d->acc;
        d->acc >>= 8;
        d->nacc -= 8;
        if (d->outn == sizeof(d->out))
            flush_out(d);
    }
}

static void align_byte(struct lm_deflater *d)
{
    if (d->nacc > 0)
        put_bits(d, 0, 8 - d->nacc);
}

static const uint16_t k_len_base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t k_len_extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t k_dist_base[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const uint8_t k_dist_extra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
static const uint8_t k_cl_order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };

static int len_code(int len)
{
    int c = 28;
    while (k_len_base[c] > len)
        c--;
    return c;
}

static int dist_code(int dist)
{
    int c = 29;
    while (k_dist_base[c] > dist)
        c--;
    return c;
}

/* The depths of the leaves of the Huffman tree of m frequencies (sorted ascending); returns the deepest. */
static int huff_depths(const uint32_t *f, int m, int *leaf_depth)
{
    uint64_t w[600];
    int parent[600], left = 0, ileft = m, total = m;
    for (int i = 0; i < m; i++)
        w[i] = f[i];
    for (int k = 0; k < m - 1; k++) {
        int a, b;
        if (left < m && (ileft >= total || w[left] <= w[ileft]))
            a = left++;
        else
            a = ileft++;
        if (left < m && (ileft >= total || w[left] <= w[ileft]))
            b = left++;
        else
            b = ileft++;
        w[total] = w[a] + w[b];
        parent[a] = parent[b] = total;
        total++;
    }
    int depth[600], deepest = 0;
    depth[total - 1] = 0;
    for (int i = total - 2; i >= 0; i--)
        depth[i] = depth[parent[i]] + 1;
    for (int i = 0; i < m; i++) {
        leaf_depth[i] = depth[i] < 1 ? 1 : depth[i];
        if (leaf_depth[i] > deepest)
            deepest = leaf_depth[i];
    }
    return deepest;
}

/* Code lengths for the frequencies, at most `maxlen` bits: a Huffman tree; when it is too deep the frequencies are halved (flattened) and it is built
 * again, so the code is always complete. */
static void build_lengths(const uint32_t *freq, int n, int maxlen, uint8_t *lens)
{
    int idx[300], m = 0;
    uint32_t f[300];
    memset(lens, 0, (size_t)n);
    for (int i = 0; i < n; i++)
        if (freq[i])
            idx[m++] = i;
    for (int i = 0; i < n && m < 2; i++)                                    /* at least two codes, so the code is complete */
        if (!freq[i])
            idx[m++] = i;
    for (int i = 0; i < m; i++)
        f[i] = freq[idx[i]] ? freq[idx[i]] : 1;
    for (int i = 1; i < m; i++) {                                           /* sorted by frequency (insertion sort: n <= 288) */
        int s = idx[i];
        uint32_t fv = f[i];
        int j = i - 1;
        while (j >= 0 && f[j] > fv) {
            f[j + 1] = f[j];
            idx[j + 1] = idx[j];
            j--;
        }
        f[j + 1] = fv;
        idx[j + 1] = s;
    }
    int depth[300];
    while (huff_depths(f, m, depth) > maxlen)
        for (int i = 0; i < m; i++)
            f[i] = (f[i] >> 1) | 1;                                         /* stays sorted */
    for (int i = 0; i < m; i++)
        lens[idx[i]] = (uint8_t)depth[i];
}

static void make_codes(const uint8_t *lens, int n, uint16_t *codes)
{
    int bl_count[16] = { 0 }, next[16];
    for (int i = 0; i < n; i++)
        bl_count[lens[i]]++;
    bl_count[0] = 0;
    int code = 0;
    for (int b = 1; b < 16; b++) {
        code = (code + bl_count[b - 1]) << 1;
        next[b] = code;
    }
    for (int i = 0; i < n; i++)
        if (lens[i]) {
            int c = next[lens[i]]++, r = 0;
            for (int k = 0; k < lens[i]; k++)
                if (c & (1 << k))
                    r |= 1 << (lens[i] - 1 - k);
            codes[i] = (uint16_t)r;                                         /* bit reversed: deflate sends the code's first bit first */
        }
}

static void fixed_lengths(uint8_t *ll, uint8_t *dl)
{
    for (int i = 0; i < 144; i++) ll[i] = 8;
    for (int i = 144; i < 256; i++) ll[i] = 9;
    for (int i = 256; i < 280; i++) ll[i] = 7;
    for (int i = 280; i < 288; i++) ll[i] = 8;
    for (int i = 0; i < 30; i++) dl[i] = 5;
}

struct clsym {
    uint8_t sym, extra, nbits;
};

/* The code lengths of both trees as a run-length coded list of code length symbols. */
static int rle_lengths(const uint8_t *all, int n, struct clsym *out)
{
    int m = 0;
    for (int i = 0; i < n;) {
        int v = all[i], run = 1;
        while (i + run < n && all[i + run] == v)
            run++;
        if (v == 0 && run >= 3) {
            int r = run > 138 ? 138 : run;
            if (r >= 11)
                out[m++] = (struct clsym){ 18, (uint8_t)(r - 11), 7 };
            else
                out[m++] = (struct clsym){ 17, (uint8_t)(r - 3), 3 };
            i += r;
        } else if (v != 0 && run >= 4) {
            out[m++] = (struct clsym){ (uint8_t)v, 0, 0 };
            int r = run - 1;
            i++;
            while (r >= 3) {
                int rr = r > 6 ? 6 : r;
                out[m++] = (struct clsym){ 16, (uint8_t)(rr - 3), 2 };
                r -= rr;
                i += rr;
            }
            while (r-- > 0) {
                out[m++] = (struct clsym){ (uint8_t)v, 0, 0 };
                i++;
            }
        } else {
            out[m++] = (struct clsym){ (uint8_t)v, 0, 0 };
            i++;
        }
    }
    return m;
}

/* Emits the tokens as one block (`last`: BFINAL); the raw input of the block is buf[from, to). */
static void emit_block(struct lm_deflater *d, size_t from, size_t to, bool last)
{
    uint32_t *lf = d->lfreq, *df = d->dfreq;
    memset(lf, 0, sizeof(d->lfreq));
    memset(df, 0, sizeof(d->dfreq));
    for (int i = 0; i < d->ntok; i++) {
        if (!d->toks[i].dist)
            lf[d->toks[i].lit]++;
        else {
            lf[257 + len_code(d->toks[i].lit)]++;
            df[dist_code(d->toks[i].dist)]++;
        }
    }
    lf[256] = 1;
    uint8_t ll[288], dl[30];
    memset(ll, 0, sizeof(ll));
    build_lengths(lf, 286, 15, ll);
    build_lengths(df, 30, 15, dl);
    int hlit = 286, hdist = 30;
    while (hlit > 257 && !ll[hlit - 1])
        hlit--;
    while (hdist > 1 && !dl[hdist - 1])
        hdist--;
    uint8_t all[320];
    memcpy(all, ll, (size_t)hlit);
    memcpy(all + hlit, dl, (size_t)hdist);
    struct clsym cs[320];
    int ncs = rle_lengths(all, hlit + hdist, cs);
    uint32_t clf[19] = { 0 };
    for (int i = 0; i < ncs; i++)
        clf[cs[i].sym]++;
    uint8_t cll[19];
    build_lengths(clf, 19, 7, cll);
    int hclen = 19;
    while (hclen > 4 && !cll[k_cl_order[hclen - 1]])
        hclen--;
    /* the sizes in bits */
    uint64_t dyn = 14 + 3 * (uint64_t)hclen;
    for (int i = 0; i < ncs; i++)
        dyn += cll[cs[i].sym] + cs[i].nbits;
    uint64_t fix = 0, extra = 0;
    uint8_t fl[288], fd[30];
    fixed_lengths(fl, fd);
    for (int i = 0; i < 286; i++) {
        dyn += (uint64_t)lf[i] * ll[i];
        fix += (uint64_t)lf[i] * fl[i];
        if (i >= 257)
            extra += (uint64_t)lf[i] * k_len_extra[i - 257];
    }
    for (int i = 0; i < 30; i++) {
        dyn += (uint64_t)df[i] * dl[i];
        fix += (uint64_t)df[i] * 5;
        extra += (uint64_t)df[i] * k_dist_extra[i];
    }
    dyn += extra;
    fix += extra;
    size_t span = to - from;
    uint64_t stored = (uint64_t)(span + 5 * ((span + 65534) / 65535 + (span == 0))) * 8 + 8;
    int mode = dyn <= fix ? 2 : 1;
    uint64_t best = dyn <= fix ? dyn + 3 : fix + 3;
    if (stored < best)
        mode = 0;
    if (mode == 0) {
        size_t pos = from;
        do {
            size_t n = to - pos > 65535 ? 65535 : to - pos;
            bool fin = last && pos + n == to;
            put_bits(d, fin ? 1 : 0, 1);
            put_bits(d, 0, 2);
            align_byte(d);
            put_bits(d, (uint32_t)n, 16);
            put_bits(d, (uint32_t)(~n & 0xffff), 16);
            for (size_t k = 0; k < n; k++)
                put_bits(d, d->buf[pos + k], 8);
            pos += n;
        } while (pos < to);
        return;
    }
    uint16_t lcode[288], dcode[30];
    const uint8_t *L = mode == 2 ? ll : fl, *D = mode == 2 ? dl : fd;
    put_bits(d, last ? 1 : 0, 1);
    put_bits(d, mode == 2 ? 2 : 1, 2);
    if (mode == 2) {
        put_bits(d, (uint32_t)(hlit - 257), 5);
        put_bits(d, (uint32_t)(hdist - 1), 5);
        put_bits(d, (uint32_t)(hclen - 4), 4);
        for (int i = 0; i < hclen; i++)
            put_bits(d, cll[k_cl_order[i]], 3);
        uint16_t clc[19];
        make_codes(cll, 19, clc);
        for (int i = 0; i < ncs; i++) {
            put_bits(d, clc[cs[i].sym], cll[cs[i].sym]);
            if (cs[i].nbits)
                put_bits(d, cs[i].extra, cs[i].nbits);
        }
    }
    make_codes(L, mode == 2 ? 286 : 288, lcode);
    make_codes(D, 30, dcode);
    for (int i = 0; i < d->ntok; i++) {
        if (!d->toks[i].dist)
            put_bits(d, lcode[d->toks[i].lit], L[d->toks[i].lit]);
        else {
            int lc = len_code(d->toks[i].lit), dc = dist_code(d->toks[i].dist);
            put_bits(d, lcode[257 + lc], L[257 + lc]);
            if (k_len_extra[lc])
                put_bits(d, (uint32_t)(d->toks[i].lit - k_len_base[lc]), k_len_extra[lc]);
            put_bits(d, dcode[dc], D[dc]);
            if (k_dist_extra[dc])
                put_bits(d, (uint32_t)(d->toks[i].dist - k_dist_base[dc]), k_dist_extra[dc]);
        }
    }
    put_bits(d, lcode[256], L[256]);
}

static uint32_t hash3(const uint8_t *p)
{
    return (((uint32_t)p[0] << 10) ^ ((uint32_t)p[1] << 5) ^ p[2]) * 2654435761u >> (32 - HASH_BITS);
}

struct lm_deflater *lm_deflate_new(int level, lm_deflate_sink sink, void *ctx)
{
    struct lm_deflater *d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    if (level < 1)
        level = 1;
    if (level > 9)
        level = 9;
    d->level = level;
    d->sink = sink;
    d->ctx = ctx;
    static const int chain[9] = { 4, 8, 16, 32, 64, 128, 256, 512, 1024 }, nice[9] = { 8, 16, 32, 64, 96, 128, 160, 200, 258 };
    d->max_chain = chain[level - 1];
    d->nice = nice[level - 1];
    d->lazy = level >= 4;
    d->buf = malloc(WIN + CHUNK + MAX_MATCH + 8);
    d->head = malloc(HASH_SIZE * sizeof(int32_t));
    d->prev = malloc((size_t)(WIN + CHUNK + MAX_MATCH + 8) * sizeof(int32_t));
    d->toks = malloc(MAX_TOKENS * sizeof(struct tok));
    if (!d->buf || !d->head || !d->prev || !d->toks) {
        lm_deflate_free(d);
        return NULL;
    }
    return d;
}

void lm_deflate_free(struct lm_deflater *d)
{
    if (!d)
        return;
    free(d->buf);
    free(d->head);
    free(d->prev);
    free(d->toks);
    free(d);
}

static int match_len(const uint8_t *a, const uint8_t *b, int max)
{
    int n = 0;
    while (n < max && a[n] == b[n])
        n++;
    return n;
}

/* Codes buf[hist, have) with buf[0, hist) as the window. `last`: this is the end of the stream. */
static void compress_region(struct lm_deflater *d, bool last)
{
    const uint8_t *b = d->buf;
    size_t start = d->hist, end = d->have;
    memset(d->head, 0xff, HASH_SIZE * sizeof(int32_t));
    for (size_t i = 0; i + MIN_MATCH <= start; i++) {                      /* the window's own positions */
        uint32_t h = hash3(b + i);
        d->prev[i] = d->head[h];
        d->head[h] = (int32_t)i;
    }
    size_t block_from = start, pos = start;
    d->ntok = 0;
    int prev_len = 0, prev_dist = 0;
    bool have_prev = false;
    while (pos < end) {
        int best_len = 0, best_dist = 0;
        bool can_hash = pos + MIN_MATCH <= end;
        int32_t cand = -1;
        if (can_hash) {
            uint32_t h = hash3(b + pos);
            cand = d->head[h];
            d->prev[pos] = cand;
            d->head[h] = (int32_t)pos;
        }
        if (can_hash && !(have_prev && prev_len >= d->nice)) {
            int chain = d->max_chain, maxl = (int)(end - pos < MAX_MATCH ? end - pos : MAX_MATCH);
            if (have_prev && prev_len >= 32)
                chain >>= 2;
            while (cand >= 0 && chain-- > 0 && pos - (size_t)cand <= WIN) {
                if (b[cand + best_len] == b[pos + best_len] || best_len == 0) {
                    int l = match_len(b + cand, b + pos, maxl);
                    if (l > best_len) {
                        best_len = l;
                        best_dist = (int)(pos - (size_t)cand);
                        if (l >= d->nice || l >= maxl)
                            break;
                    }
                }
                cand = d->prev[cand];
            }
            if (best_len == MIN_MATCH && best_dist > 4096)
                best_len = 0;
        }
        bool emit_prev = false;
        if (d->lazy) {
            if (have_prev && prev_len >= MIN_MATCH && prev_len >= best_len)
                emit_prev = true;
        } else if (best_len >= MIN_MATCH) {
            prev_len = best_len;
            prev_dist = best_dist;
            have_prev = true;
            emit_prev = true;
        }
        if (d->lazy && !emit_prev) {
            if (have_prev) {                                                /* the previous position's match lost to this one: it becomes a literal */
                d->toks[d->ntok++] = (struct tok){ 0, b[pos - 1] };
            }
            prev_len = best_len;
            prev_dist = best_dist;
            have_prev = true;
            pos++;
        } else if (d->lazy && emit_prev) {
            d->toks[d->ntok++] = (struct tok){ (uint16_t)prev_dist, (uint16_t)prev_len };
            size_t mend = pos - 1 + (size_t)prev_len;                       /* the match started at pos - 1 */
            for (size_t i = pos + 1; i < mend && i + MIN_MATCH <= end; i++) {
                uint32_t h = hash3(b + i);
                d->prev[i] = d->head[h];
                d->head[h] = (int32_t)i;
            }
            pos = mend;
            have_prev = false;
            prev_len = 0;
        } else if (!d->lazy && emit_prev) {
            d->toks[d->ntok++] = (struct tok){ (uint16_t)best_dist, (uint16_t)best_len };
            for (size_t i = pos + 1; i < pos + (size_t)best_len && i + MIN_MATCH <= end; i++) {
                uint32_t h = hash3(b + i);
                d->prev[i] = d->head[h];
                d->head[h] = (int32_t)i;
            }
            pos += (size_t)best_len;
            have_prev = false;
        } else {
            d->toks[d->ntok++] = (struct tok){ 0, b[pos] };
            pos++;
        }
        if (d->ntok >= MAX_TOKENS - 2) {
            size_t cut = d->lazy && have_prev ? pos - 1 : pos;              /* a pending position is not in the tokens yet: it starts the next block */
            emit_block(d, block_from, cut, false);
            block_from = cut;
            d->ntok = 0;
        }
    }
    if (d->lazy && have_prev)                                               /* the last position was only a candidate */
        d->toks[d->ntok++] = (struct tok){ 0, b[end - 1] };
    if (d->ntok || last || block_from < end)
        emit_block(d, block_from, end, last);
    d->ntok = 0;
    (void)have_prev;
}

int lm_deflate_write(struct lm_deflater *d, const uint8_t *p, size_t n)
{
    while (n && !d->failed) {
        size_t room = WIN + CHUNK - d->have;
        size_t take = n < room ? n : room;
        memcpy(d->buf + d->have, p, take);
        d->have += take;
        p += take;
        n -= take;
        if (d->have == WIN + CHUNK) {
            compress_region(d, false);
            size_t keep = d->have < WIN ? d->have : WIN;
            memmove(d->buf, d->buf + d->have - keep, keep);
            d->have = d->hist = keep;
        }
    }
    return d->failed ? -1 : 0;
}

int lm_deflate_finish(struct lm_deflater *d)
{
    compress_region(d, true);
    align_byte(d);
    flush_out(d);
    int r = d->failed ? -1 : 0;
    lm_deflate_free(d);
    return r;
}

struct bufsink {
    uint8_t *p;
    size_t n, cap;
};

static int buf_sink(void *ctx, const uint8_t *p, size_t n)
{
    struct bufsink *b = ctx;
    if (b->n + n > b->cap) {
        size_t nc = (b->n + n) * 2 + 64;
        uint8_t *t = realloc(b->p, nc);
        if (!t)
            return -1;
        b->p = t;
        b->cap = nc;
    }
    memcpy(b->p + b->n, p, n);
    b->n += n;
    return 0;
}

uint8_t *lm_deflate_buf(const uint8_t *in, size_t n, int level, size_t *outn)
{
    struct bufsink b = { 0 };
    struct lm_deflater *d = lm_deflate_new(level, buf_sink, &b);
    if (!d)
        return NULL;
    if (lm_deflate_write(d, in, n) || lm_deflate_finish(d)) {
        free(b.p);
        return NULL;
    }
    *outn = b.n;
    return b.p;
}
