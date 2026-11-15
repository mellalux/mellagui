#include "decomp.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* ======================================================================== bzip2 ====================================================================== */
struct bzr {
    const uint8_t *p;
    size_t n, pos;
    uint64_t buf;
    int bits;
    bool eof;
};

static uint32_t bz_get(struct bzr *r, int n)
{
    while (r->bits < n) {
        if (r->pos >= r->n) {
            r->eof = true;
            r->buf <<= 8;
            r->bits += 8;
            continue;
        }
        r->buf = r->buf << 8 | r->p[r->pos++];
        r->bits += 8;
    }
    r->bits -= n;
    return (uint32_t)((r->buf >> r->bits) & ((1ull << n) - 1));
}

#define BZ_MAXLEN 20

struct bzhuff {
    int limit[BZ_MAXLEN + 2], base[BZ_MAXLEN + 2], perm[258], minlen, maxlen;
};

static bool bz_make_table(struct bzhuff *h, const uint8_t *len, int alpha)
{
    int minl = 32, maxl = 0, pp = 0;
    for (int i = 0; i < alpha; i++) {
        if (len[i] > maxl) maxl = len[i];
        if (len[i] < minl) minl = len[i];
    }
    h->minlen = minl;
    h->maxlen = maxl;
    for (int l = minl; l <= maxl; l++)
        for (int i = 0; i < alpha; i++)
            if (len[i] == l)
                h->perm[pp++] = i;
    int count[BZ_MAXLEN + 2] = { 0 };
    for (int i = 0; i < alpha; i++)
        count[len[i]]++;
    int code = 0, idx = 0;
    for (int l = minl; l <= maxl; l++) {
        h->base[l] = idx - code;                    /* perm index = code + base */
        code += count[l];
        idx += count[l];
        h->limit[l] = code - 1;
        code <<= 1;
    }
    return true;
}

static int bz_sym(struct bzr *r, const struct bzhuff *h)
{
    int n = h->minlen;
    int v = (int)bz_get(r, n);
    while (n <= h->maxlen) {
        if (v <= h->limit[n] && h->limit[n] >= 0 && (h->limit[n] - v) < 0x7fffffff) {
            int idx = v + h->base[n];
            return idx >= 0 && idx < 258 ? h->perm[idx] : -1;
        }
        if (++n > h->maxlen)
            break;
        v = v << 1 | (int)bz_get(r, 1);
    }
    return -1;
}

long lm_bzip2_prefix(const uint8_t *in, size_t inlen, uint8_t *out, size_t cap, int *complete)
{
    if (complete)
        *complete = 0;
    size_t w = 0;
    struct bzr r = { in, inlen, 0, 0, 0, false };
    uint32_t *tt = NULL;
    uint8_t *ll = NULL;
    bool any = false, done_ok = false;
    while (r.pos + 4 <= inlen || r.bits >= 32) {                              /* one or more concatenated streams */
        if (bz_get(&r, 8) != 'B' || bz_get(&r, 8) != 'Z' || bz_get(&r, 8) != 'h')
            break;
        int level = (int)bz_get(&r, 8) - '0';
        if (level < 1 || level > 9)
            break;
        size_t bsize = (size_t)level * 100000;
        if (!tt) {
            tt = malloc(bsize * sizeof(uint32_t) + 4);
            ll = malloc(bsize + 4);
            if (!tt || !ll)
                break;
        }
        bool stream_end = false;
        while (!r.eof) {
            uint32_t m1 = bz_get(&r, 24), m2 = bz_get(&r, 24);
            if (m1 == 0x177245 && m2 == 0x385090) {                           /* the end of the stream: the combined CRC, then up to a byte boundary */
                bz_get(&r, 32);
                r.bits -= r.bits % 8;
                stream_end = !r.eof;
                break;
            }
            if (m1 != 0x314159 || m2 != 0x265359)
                goto finish;
            bz_get(&r, 32);                                                   /* block CRC (not checked) */
            if (bz_get(&r, 1))
                goto finish;                                                  /* randomised blocks: obsolete */
            uint32_t orig = bz_get(&r, 24);
            uint8_t used[256], seq[256];
            int ninuse = 0;
            uint32_t map16 = bz_get(&r, 16);
            memset(used, 0, sizeof(used));
            for (int i = 0; i < 16; i++)
                if (map16 & (0x8000u >> i)) {
                    uint32_t m = bz_get(&r, 16);
                    for (int j = 0; j < 16; j++)
                        if (m & (0x8000u >> j))
                            used[i * 16 + j] = 1;
                }
            for (int i = 0; i < 256; i++)
                if (used[i])
                    seq[ninuse++] = (uint8_t)i;
            if (!ninuse)
                goto finish;
            int alpha = ninuse + 2;
            int ngroups = (int)bz_get(&r, 3), nsel = (int)bz_get(&r, 15);
            if (ngroups < 2 || ngroups > 6 || nsel < 1)
                goto finish;
            uint8_t *sel = malloc((size_t)nsel);
            if (!sel)
                goto finish;
            uint8_t mtfg[6] = { 0, 1, 2, 3, 4, 5 };
            bool bad = false;
            for (int i = 0; i < nsel; i++) {
                int j = 0;
                while (bz_get(&r, 1)) {
                    if (++j >= ngroups) {
                        bad = true;
                        break;
                    }
                }
                if (bad)
                    break;
                uint8_t v = mtfg[j];
                for (; j > 0; j--)
                    mtfg[j] = mtfg[j - 1];
                mtfg[0] = v;
                sel[i] = v;
            }
            struct bzhuff *hf = malloc(sizeof(struct bzhuff) * 6);
            uint8_t len[6][258];
            if (!hf || bad || r.eof) {
                free(sel);
                free(hf);
                goto finish;
            }
            for (int t = 0; t < ngroups && !bad; t++) {
                int cur = (int)bz_get(&r, 5);
                for (int i = 0; i < alpha; i++) {
                    for (;;) {
                        if (cur < 1 || cur > BZ_MAXLEN || r.eof) {
                            bad = true;
                            break;
                        }
                        if (!bz_get(&r, 1))
                            break;
                        cur += bz_get(&r, 1) ? -1 : 1;
                    }
                    if (bad)
                        break;
                    len[t][i] = (uint8_t)cur;
                }
                if (!bad)
                    bz_make_table(&hf[t], len[t], alpha);
            }
            if (bad) {
                free(sel);
                free(hf);
                goto finish;
            }
            /* the symbols: runs of the first symbol (RUNA / RUNB), move-to-front indices, the end of the block */
            uint8_t mtf[256];
            for (int i = 0; i < 256; i++)
                mtf[i] = (uint8_t)i;
            size_t nblock = 0;
            uint32_t cf[257] = { 0 };
            int gno = -1, gpos = 0, eob = ninuse + 1;
            uint32_t runlen = 0, runbit = 1;
            bool runs = false;
            for (;;) {
                if (gpos == 0) {
                    if (++gno >= nsel) { bad = true; break; }
                    gpos = 50;
                }
                gpos--;
                int s = bz_sym(&r, &hf[sel[gno]]);
                if (s < 0 || r.eof) {
                    bad = true;
                    break;
                }
                if (s <= 1) {                                                 /* RUNA = 1, RUNB = 2 of the current bit */
                    if (runbit > (1u << 21)) { bad = true; break; }
                    runlen += runbit << s;
                    runbit <<= 1;
                    runs = true;
                    continue;
                }
                if (runs) {
                    if (nblock + runlen > bsize) { bad = true; break; }
                    uint8_t c = seq[mtf[0]];
                    memset(ll + nblock, c, runlen);
                    cf[c] += runlen;
                    nblock += runlen;
                    runs = false;
                    runlen = 0;
                    runbit = 1;
                }
                if (s == eob)
                    break;
                int k = s - 1;
                uint8_t v = mtf[k];
                memmove(mtf + 1, mtf, (size_t)k);
                mtf[0] = v;
                if (nblock >= bsize) { bad = true; break; }
                ll[nblock++] = seq[v];
                cf[seq[v]]++;
            }
            free(sel);
            free(hf);
            if (bad || orig >= nblock + (nblock == 0))
                goto finish;
            /* the inverse Burrows-Wheeler transform */
            uint32_t sum = 0, start[256];
            for (int i = 0; i < 256; i++) {
                start[i] = sum;
                sum += cf[i];
            }
            for (size_t i = 0; i < nblock; i++)
                tt[i] = ll[i];
            for (size_t i = 0; i < nblock; i++)
                tt[start[tt[i] & 0xff]++] |= (uint32_t)i << 8;
            /* tt[j] now holds, for the j-th sorted row, the row it came from (high bits) -- walk it, undoing the first run-length step as we go */
            uint32_t pos = tt[orig] >> 8;
            int runc = 0, prev = -1;
            for (size_t k = 0; k < nblock; k++) {
                uint32_t e = tt[pos];
                int ch = (int)(e & 0xff);
                pos = e >> 8;
                if (runc == 4) {                                              /* after four equal bytes comes a count of more */
                    for (int c = 0; c < ch; c++) {
                        if (w >= cap)
                            goto full;
                        out[w++] = (uint8_t)prev;
                    }
                    runc = 0;
                    prev = -1;
                    continue;
                }
                if (ch == prev)
                    runc++;
                else {
                    runc = 1;
                    prev = ch;
                }
                if (w >= cap)
                    goto full;
                out[w++] = (uint8_t)ch;
            }
            any = true;
        }
        if (!stream_end)
            break;
        done_ok = true;
        if (r.pos >= r.n && r.bits < 8)
            break;
        done_ok = false;
        continue;
    full:
        free(tt);
        free(ll);
        return (long)w;
    }
finish:
    free(tt);
    free(ll);
    if (complete)
        *complete = done_ok;
    return any || w ? (long)w : -1;
}

/* ======================================================================== LZMA ======================================================================= */
struct lz {
    const uint8_t *in;
    size_t n, pos;
    uint32_t range, code;
    bool bad;
    uint16_t *lit;                      /* literal coder probabilities: 0x300 << (lc + lp) */
    uint16_t isMatch[12 << 4], isRep[12], isRepG0[12], isRepG1[12], isRepG2[12], isRep0Long[12 << 4];
    uint16_t posSlot[4][64], posDec[115], align[16];
    uint16_t lenChoice, lenChoice2, lenLow[16][8], lenMid[16][8], lenHigh[256];
    uint16_t rlenChoice, rlenChoice2, rlenLow[16][8], rlenMid[16][8], rlenHigh[256];
    int lc, lp, pb;
    int state;
    uint32_t rep[4];
    size_t lit_n;
};

static void lz_norm(struct lz *z)
{
    if (z->range < (1u << 24)) {
        z->range <<= 8;
        if (z->pos < z->n)
            z->code = z->code << 8 | z->in[z->pos++];
        else {
            z->bad = true;
            z->code <<= 8;
        }
    }
}

static bool lz_rc_init(struct lz *z)
{
    if (z->pos + 5 > z->n || z->in[z->pos] != 0)
        return false;
    z->code = (uint32_t)z->in[z->pos + 1] << 24 | (uint32_t)z->in[z->pos + 2] << 16 | (uint32_t)z->in[z->pos + 3] << 8 | z->in[z->pos + 4];
    z->range = 0xffffffffu;
    z->pos += 5;
    z->bad = false;
    return true;
}

static int lz_bit(struct lz *z, uint16_t *p)
{
    lz_norm(z);
    uint32_t bound = (z->range >> 11) * *p;
    if (z->code < bound) {
        z->range = bound;
        *p = (uint16_t)(*p + ((2048 - *p) >> 5));
        return 0;
    }
    z->range -= bound;
    z->code -= bound;
    *p = (uint16_t)(*p - (*p >> 5));
    return 1;
}

static uint32_t lz_direct(struct lz *z, int n)
{
    uint32_t v = 0;
    while (n--) {
        lz_norm(z);
        z->range >>= 1;
        uint32_t t = (z->code - z->range) >> 31;                              /* 1 when code < range */
        z->code -= z->range & (t - 1);
        v = v << 1 | (1 - t);
    }
    return v;
}

static uint32_t lz_tree(struct lz *z, uint16_t *probs, int bits)
{
    uint32_t m = 1;
    for (int i = 0; i < bits; i++)
        m = m << 1 | (uint32_t)lz_bit(z, &probs[m]);
    return m - (1u << bits);
}

static uint32_t lz_tree_rev(struct lz *z, uint16_t *probs, int bits)
{
    uint32_t m = 1, sym = 0;
    for (int i = 0; i < bits; i++) {
        int b = lz_bit(z, &probs[m]);
        m = m << 1 | (uint32_t)b;
        sym |= (uint32_t)b << i;
    }
    return sym;
}

static uint32_t lz_len(struct lz *z, uint16_t *choice, uint16_t *choice2, uint16_t (*low)[8], uint16_t (*mid)[8], uint16_t *high, int ps)
{
    if (!lz_bit(z, choice))
        return lz_tree(z, low[ps], 3);
    if (!lz_bit(z, choice2))
        return 8 + lz_tree(z, mid[ps], 3);
    return 16 + lz_tree(z, high, 8);
}

static void lz_reset_state(struct lz *z)
{
    for (size_t i = 0; i < z->lit_n; i++)
        z->lit[i] = 1024;
    uint16_t *all[] = { z->isMatch, z->isRep, z->isRepG0, z->isRepG1, z->isRepG2, z->isRep0Long, &z->posSlot[0][0], z->posDec, z->align, &z->lenChoice, &z->lenChoice2,
                        &z->lenLow[0][0], &z->lenMid[0][0], z->lenHigh, &z->rlenChoice, &z->rlenChoice2, &z->rlenLow[0][0], &z->rlenMid[0][0], z->rlenHigh };
    size_t cnt[] = { 12 << 4, 12, 12, 12, 12, 12 << 4, 4 * 64, 115, 16, 1, 1, 16 * 8, 16 * 8, 256, 1, 1, 16 * 8, 16 * 8, 256 };
    for (size_t k = 0; k < sizeof(all) / sizeof(all[0]); k++)
        for (size_t i = 0; i < cnt[k]; i++)
            all[k][i] = 1024;
    z->state = 0;
    z->rep[0] = z->rep[1] = z->rep[2] = z->rep[3] = 0;
}

static bool lz_set_props(struct lz *z, unsigned b)
{
    if (b >= 9 * 5 * 5)
        return false;
    int lc = (int)(b % 9), r = (int)(b / 9), lp = r % 5, pb = r / 5;
    size_t need = (size_t)0x300 << (lc + lp);
    if (need != z->lit_n) {
        free(z->lit);
        z->lit = malloc(need * sizeof(uint16_t));
        if (!z->lit) {
            z->lit_n = 0;
            return false;
        }
        z->lit_n = need;
    }
    z->lc = lc;
    z->lp = lp;
    z->pb = pb;
    return true;
}

/* Decode until `limit` bytes are in out[start..] (out[0..start) is the dictionary already there, from `dict0` on). Returns the end position or (size_t)-1. */
static size_t lz_decode(struct lz *z, uint8_t *out, size_t dict0, size_t start, size_t limit)
{
    size_t w = start;
    uint32_t pbmask = (1u << z->pb) - 1, lpmask = (1u << z->lp) - 1;
    while (w < limit) {
        if (z->bad)
            return (size_t)-1;
        uint32_t ps = (uint32_t)(w - dict0) & pbmask;
        int st = z->state;
        if (!lz_bit(z, &z->isMatch[(st << 4) + ps])) {
            uint8_t prev = w > dict0 ? out[w - 1] : 0;
            uint32_t ls = (((uint32_t)(w - dict0) & lpmask) << z->lc) + (uint32_t)(prev >> (8 - z->lc));
            uint16_t *probs = z->lit + (size_t)0x300 * ls;
            uint32_t sym = 1;
            if (st >= 7) {
                uint32_t mb = w - dict0 > z->rep[0] ? out[w - z->rep[0] - 1] : 0, off = 0x100;
                (void)off;
                uint32_t matchbyte = mb;
                uint32_t o = 0x100;
                while (sym < 0x100) {
                    matchbyte <<= 1;
                    uint32_t mbit = matchbyte & o;
                    int bit = lz_bit(z, &probs[o + mbit + sym]);
                    sym = sym << 1 | (uint32_t)bit;
                    if (bit) o &= mbit; else o &= ~mbit;
                }
            } else {
                while (sym < 0x100)
                    sym = sym << 1 | (uint32_t)lz_bit(z, &probs[sym]);
            }
            out[w++] = (uint8_t)sym;
            z->state = st < 4 ? 0 : st < 10 ? st - 3 : st - 6;
            continue;
        }
        uint32_t len;
        if (lz_bit(z, &z->isRep[st])) {
            if (w == dict0)
                return (size_t)-1;
            if (!lz_bit(z, &z->isRepG0[st])) {
                if (!lz_bit(z, &z->isRep0Long[(st << 4) + ps])) {            /* a short rep: one byte */
                    if (z->rep[0] >= w - dict0)
                        return (size_t)-1;
                    out[w] = out[w - z->rep[0] - 1];
                    w++;
                    z->state = st < 7 ? 9 : 11;
                    continue;
                }
            } else {
                uint32_t d;
                if (!lz_bit(z, &z->isRepG1[st]))
                    d = z->rep[1];
                else {
                    if (!lz_bit(z, &z->isRepG2[st]))
                        d = z->rep[2];
                    else {
                        d = z->rep[3];
                        z->rep[3] = z->rep[2];
                    }
                    z->rep[2] = z->rep[1];
                }
                z->rep[1] = z->rep[0];
                z->rep[0] = d;
            }
            len = lz_len(z, &z->rlenChoice, &z->rlenChoice2, z->rlenLow, z->rlenMid, z->rlenHigh, (int)ps);
            z->state = st < 7 ? 8 : 11;
        } else {
            z->rep[3] = z->rep[2];
            z->rep[2] = z->rep[1];
            z->rep[1] = z->rep[0];
            len = lz_len(z, &z->lenChoice, &z->lenChoice2, z->lenLow, z->lenMid, z->lenHigh, (int)ps);
            z->state = st < 7 ? 7 : 10;
            uint32_t lenstate = len < 4 ? len : 3;
            uint32_t slot = lz_tree(z, z->posSlot[lenstate], 6);
            if (slot < 4)
                z->rep[0] = slot;
            else {
                int nd = (int)(slot >> 1) - 1;
                uint32_t dist = (2 | (slot & 1)) << nd;
                if (slot < 14)
                    dist += lz_tree_rev(z, z->posDec + dist - slot, nd);
                else {
                    dist += lz_direct(z, nd - 4) << 4;
                    dist += lz_tree_rev(z, z->align, 4);
                }
                z->rep[0] = dist;
                if (dist == 0xffffffffu)                                     /* the end marker */
                    return w;
            }
        }
        len += 2;
        if (z->rep[0] >= w - dict0)
            return (size_t)-1;
        size_t take = limit - w < len ? limit - w : len;
        for (size_t i = 0; i < take; i++, w++)
            out[w] = out[w - z->rep[0] - 1];
        if (take < len)
            return w;
    }
    return w;
}

static struct lz *lz_new(const uint8_t *in, size_t n)
{
    struct lz *z = calloc(1, sizeof(*z));
    if (z) {
        z->in = in;
        z->n = n;
    }
    return z;
}

static void lz_free(struct lz *z)
{
    if (z) {
        free(z->lit);
        free(z);
    }
}

long lm_lzma_raw(const uint8_t *in, size_t inlen, const uint8_t props[5], uint8_t *out, size_t usize)
{
    struct lz *z = lz_new(in, inlen);
    if (!z)
        return -1;
    long ret = -1;
    if (lz_set_props(z, props[0]) && (lz_reset_state(z), lz_rc_init(z))) {
        size_t e = lz_decode(z, out, 0, 0, usize);
        ret = e == (size_t)-1 ? -1 : (long)e;
    }
    lz_free(z);
    return ret;
}

/* LZMA2: chunks with their own control byte; returns the bytes written and (through *used) the input consumed. */
static long lzma2_run(const uint8_t *in, size_t n, uint8_t *out, size_t cap, int *complete, size_t *used)
{
    struct lz *z = lz_new(in, n);
    if (!z)
        return -1;
    size_t p = 0, w = 0, dict0 = 0;
    bool need_props = true, need_dict = true, ok_end = false;
    while (p < n) {
        unsigned c = in[p++];
        if (c == 0) {
            ok_end = true;
            break;
        }
        if (c < 0x80) {
            if (c > 2 || p + 2 > n)
                break;
            size_t sz = ((size_t)in[p] << 8 | in[p + 1]) + 1;
            p += 2;
            if (c == 1) {
                dict0 = w;
                need_dict = false;
            } else if (need_dict)
                break;
            if (p + sz > n)
                break;
            size_t take = cap - w < sz ? cap - w : sz;
            memcpy(out + w, in + p, take);
            w += take;
            p += sz;
            if (take < sz)
                goto out_full;
            continue;
        }
        if (p + 4 > n)
            break;
        size_t us = (((size_t)(c & 0x1f) << 16) | (size_t)in[p] << 8 | in[p + 1]) + 1, ps = ((size_t)in[p + 2] << 8 | in[p + 3]) + 1;
        p += 4;
        unsigned mode = (c >> 5) & 3;
        if (mode == 3) {
            dict0 = w;
            need_dict = false;
        } else if (need_dict)
            break;
        if (mode >= 2) {
            if (p >= n || !lz_set_props(z, in[p]) || z->lc + z->lp > 4)
                break;
            p++;
            need_props = false;
        } else if (need_props)
            break;
        if (mode >= 1)
            lz_reset_state(z);
        if (p + ps > n)
            break;
        z->in = in + p;
        z->n = ps;
        z->pos = 0;
        if (!lz_rc_init(z))
            break;
        size_t limit = cap - w < us ? cap - w : us;
        size_t e = lz_decode(z, out, dict0, w, w + limit);
        if (e == (size_t)-1)
            break;
        w = e;
        p += ps;
        if (limit < us)
            goto out_full;
    }
    if (complete)
        *complete = ok_end;
    if (used)
        *used = p;
    lz_free(z);
    return (long)w;
out_full:
    if (complete)
        *complete = 0;
    if (used)
        *used = p;
    lz_free(z);
    return (long)w;
}

long lm_lzma2_raw(const uint8_t *in, size_t inlen, uint8_t *out, size_t cap, int *complete)
{
    if (complete)
        *complete = 0;
    long r = lzma2_run(in, inlen, out, cap, complete, NULL);
    return r;
}

/* ======================================================================== xz ========================================================================= */
static bool xz_varint(const uint8_t *p, size_t n, size_t *pos, uint64_t *v)
{
    *v = 0;
    for (int i = 0; i < 9 && *pos < n; i++) {
        uint8_t b = p[(*pos)++];
        *v |= (uint64_t)(b & 0x7f) << (7 * i);
        if (!(b & 0x80))
            return true;
    }
    return false;
}

long lm_xz_prefix(const uint8_t *in, size_t inlen, uint8_t *out, size_t cap, int *complete, const char **why)
{
    static const uint8_t magic[6] = { 0xfd, '7', 'z', 'X', 'Z', 0 };
    if (complete)
        *complete = 0;
    size_t p = 0, w = 0;
    bool any = false, finished = false;
    while (p + 12 <= inlen && !memcmp(in + p, magic, 6)) {
        unsigned check = in[p + 7] & 15;
        size_t csz = check == 0 ? 0 : check == 1 ? 4 : check == 4 ? 8 : check == 10 ? 32 : check <= 3 ? 4 * (size_t)(check == 1) : check <= 6 ? 8 : check <= 9 ? 16 : check <= 12 ? 32 : check <= 15 ? 64 : 0;
        if (check == 2 || check == 3)
            csz = 4;
        if (check >= 5 && check <= 6)
            csz = 8;
        if (check >= 7 && check <= 9)
            csz = 16;
        if (check >= 11 && check <= 12)
            csz = 32;
        if (check >= 13 && check <= 15)
            csz = 64;
        p += 12;
        for (;;) {
            if (p >= inlen)
                goto end;
            if (in[p] == 0)                                                   /* the index: this stream's blocks are over */
                break;
            size_t hs = ((size_t)in[p] + 1) * 4, hp = p + 1;
            if (p + hs > inlen)
                goto end;
            unsigned flags = in[hp++];
            uint64_t v;
            size_t hend = p + hs - 4;                                         /* the CRC32 is the last four bytes */
            if ((flags & 0x40) && !xz_varint(in, hend, &hp, &v))
                goto end;
            if ((flags & 0x80) && !xz_varint(in, hend, &hp, &v))
                goto end;
            unsigned nf = (flags & 3) + 1;
            bool lzma2 = false;
            for (unsigned f = 0; f < nf; f++) {
                uint64_t id, ps;
                if (!xz_varint(in, hend, &hp, &id) || !xz_varint(in, hend, &hp, &ps) || hp + ps > hend)
                    goto end;
                if (id == 0x21 && f == nf - 1)
                    lzma2 = true;
                else {
                    if (why)
                        *why = "An xz file with a filter (BCJ, delta ...) before LZMA2 is not read.";
                    free(NULL);
                    return any || w ? (long)w : -1;
                }
                hp += ps;
            }
            if (!lzma2)
                goto end;
            p += hs;
            int comp = 0;
            size_t used = 0;
            long got = lzma2_run(in + p, inlen - p, out + w, cap - w, &comp, &used);
            if (got < 0)
                goto end;
            w += (size_t)got;
            any = true;
            if (!comp)
                goto end;                                                      /* the buffer filled up, or the data is damaged */
            p += used;
            size_t blockdata = used;
            p += (4 - blockdata % 4) % 4 + csz;                                /* the block's padding and check (hs is a multiple of 4 already) */
        }
        /* the index: indicator 0, the number of records, the records, padding, CRC32; then the 12 byte footer */
        size_t ip = p + 1;
        uint64_t nrec, a, b;
        if (!xz_varint(in, inlen, &ip, &nrec))
            goto end;
        for (uint64_t i = 0; i < nrec; i++)
            if (!xz_varint(in, inlen, &ip, &a) || !xz_varint(in, inlen, &ip, &b))
                goto end;
        ip += (4 - (ip - p) % 4) % 4 + 4;
        p = ip + 12;
        finished = p <= inlen;
        while (p + 4 <= inlen && !in[p] && !in[p + 1] && !in[p + 2] && !in[p + 3] && !(p + 12 <= inlen && !memcmp(in + p, magic, 6)))
            p += 4;                                                            /* stream padding */
        if (p >= inlen)
            break;
    }
end:
    if (complete)
        *complete = finished && p >= inlen;
    return any ? (long)w : -1;
}
