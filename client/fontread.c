#include "fontread.h"

#include "../src/img.h"
#include "../src/ttf.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define FONT_MAX (48u << 20)
#define SAMPLE_W 880                            /* the sample is made twice as big as it is shown, and scaled down (crisper small text) */

static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static void seterr(char *err, size_t n, const char *s)
{
    if (err && n)
        snprintf(err, n, "%s", s);
}

void font_free(struct font_info *fi)
{
    free(fi->alpha);
    memset(fi, 0, sizeof(*fi));
}

/* ---- WOFF 1 -> an ordinary font file in memory ---- */
static uint8_t *woff_to_sfnt(const uint8_t *in, size_t n, size_t *outn)
{
    if (n < 44)
        return NULL;
    uint32_t flavor = be32(in + 4), nt = be16(in + 12);
    if (nt == 0 || nt > 64 || 44 + (size_t)nt * 20 > n)
        return NULL;
    size_t total = 12 + (size_t)nt * 16;
    for (uint32_t i = 0; i < nt; i++) {
        uint32_t olen = be32(in + 44 + i * 20 + 12);
        if (olen > FONT_MAX)
            return NULL;
        total += (olen + 3u) & ~3u;
        if (total > FONT_MAX)
            return NULL;
    }
    uint8_t *out = calloc(1, total);
    if (!out)
        return NULL;
    uint32_t pow2 = 1, lg = 0;
    while (pow2 * 2 <= nt) {
        pow2 *= 2;
        lg++;
    }
    out[0] = (uint8_t)(flavor >> 24); out[1] = (uint8_t)(flavor >> 16); out[2] = (uint8_t)(flavor >> 8); out[3] = (uint8_t)flavor;
    out[4] = (uint8_t)(nt >> 8); out[5] = (uint8_t)nt;
    uint32_t sr = pow2 * 16, rs = nt * 16 - sr;
    out[6] = (uint8_t)(sr >> 8); out[7] = (uint8_t)sr; out[8] = (uint8_t)(lg >> 8); out[9] = (uint8_t)lg; out[10] = (uint8_t)(rs >> 8); out[11] = (uint8_t)rs;
    size_t pos = 12 + (size_t)nt * 16;
    for (uint32_t i = 0; i < nt; i++) {
        const uint8_t *e = in + 44 + i * 20;
        uint32_t off = be32(e + 4), clen = be32(e + 8), olen = be32(e + 12);
        if (off > n || clen > n - off) {
            free(out);
            return NULL;
        }
        if (clen == olen) {
            memcpy(out + pos, in + off, olen);
        } else if (clen < 3 || clen > olen || lm_inflate_raw(in + off + 2, clen - 2, out + pos, olen) != 0) {          /* a zlib stream: its 2 byte header, then deflate */
            free(out);
            return NULL;
        }
        uint8_t *d = out + 12 + (size_t)i * 16;
        memcpy(d, e, 4);                                                /* tag */
        memcpy(d + 4, e + 16, 4);                                       /* checksum */
        d[8] = (uint8_t)(pos >> 24); d[9] = (uint8_t)(pos >> 16); d[10] = (uint8_t)(pos >> 8); d[11] = (uint8_t)pos;
        memcpy(d + 12, e + 12, 4);                                      /* length */
        pos += (olen + 3u) & ~3u;
    }
    *outn = total;
    return out;
}

/* ---- WOFF 2: Brotli (the system's libbrotlidec) and the transformed glyf / loca / hmtx tables, rebuilt into an ordinary font file ---- */
struct w2buf {
    uint8_t *p;
    size_t n, cap;
    bool bad;
};

static void w2_put(struct w2buf *b, const void *d, size_t n)
{
    if (b->n + n > FONT_MAX) {
        b->bad = true;
        return;
    }
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2;
        uint8_t *t = realloc(b->p, b->cap);
        if (!t) {
            b->bad = true;
            return;
        }
        b->p = t;
    }
    memcpy(b->p + b->n, d, n);
    b->n += n;
}

static void w2_u16(struct w2buf *b, unsigned v)
{
    uint8_t t[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    w2_put(b, t, 2);
}

static void w2_u32(struct w2buf *b, uint32_t v)
{
    uint8_t t[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    w2_put(b, t, 4);
}

struct cur {
    const uint8_t *p, *end;
    bool bad;
};

static unsigned cur_u8(struct cur *c)
{
    if (c->p >= c->end) {
        c->bad = true;
        return 0;
    }
    return *c->p++;
}

static unsigned cur_u16(struct cur *c)
{
    unsigned a = cur_u8(c);
    return a << 8 | cur_u8(c);
}

static unsigned cur_255(struct cur *c)                                  /* 255UInt16 */
{
    unsigned b = cur_u8(c);
    if (b == 253)
        return cur_u16(c);
    if (b == 255)
        return 253 + cur_u8(c);
    if (b == 254)
        return 506 + cur_u8(c);
    return b;
}

static uint32_t cur_base128(struct cur *c)                              /* UIntBase128 */
{
    uint32_t v = 0;
    for (int i = 0; i < 5; i++) {
        unsigned b = cur_u8(c);
        if (c->bad || (i == 0 && b == 0x80) || (v & 0xfe000000u))
            return c->bad = true, 0;
        v = v << 7 | (b & 0x7f);
        if (!(b & 0x80))
            return v;
    }
    c->bad = true;
    return 0;
}

static const char *const k_w2tags[63] = {
    "cmap", "head", "hhea", "hmtx", "maxp", "name", "OS/2", "post", "cvt ", "fpgm", "glyf", "loca", "prep", "CFF ", "VORG", "EBDT", "EBLC", "gasp", "hdmx", "kern",
    "LTSH", "PCLT", "VDMX", "vhea", "vmtx", "BASE", "GDEF", "GPOS", "GSUB", "EBSC", "JSTF", "MATH", "CBDT", "CBLC", "COLR", "CPAL", "SVG ", "sbix", "acnt", "avar",
    "bdat", "bloc", "bsln", "cvar", "fdsc", "feat", "fmtx", "fvar", "gvar", "hsty", "just", "lcar", "mort", "morx", "opbd", "prop", "trak", "Zapf", "Silf", "Glat",
    "Gloc", "Feat", "Sill",
};

struct w2table {
    char tag[4];
    uint32_t orig, len;                 /* the length it has after the transform is undone / in the compressed stream */
    bool transformed;
    const uint8_t *data;                /* in the unpacked stream */
    uint8_t *out;                       /* rebuilt tables */
    size_t outn;
};

static int32_t with_sign(unsigned flag, int base)
{
    return (flag & 1) ? base : -base;
}

/* The transformed glyf table -> a glyf table and a loca table (long offsets). Returns false for damaged data. `xmin` gets each glyph's xMin for the hmtx rebuild. */
static bool w2_glyf(const uint8_t *d, size_t n, struct w2buf *glyf, struct w2buf *loca, int16_t **xmin_out, unsigned *nglyphs_out, unsigned *index_fmt)
{
    if (n < 36)
        return false;
    struct cur h = { d, d + n, false };
    cur_u16(&h);
    cur_u16(&h);                                                        /* version, option flags */
    unsigned ng = cur_u16(&h), idxfmt = cur_u16(&h);
    uint32_t sz[7];
    for (int i = 0; i < 7; i++) {
        unsigned a = cur_u16(&h);
        sz[i] = a << 16 | cur_u16(&h);
    }
    if (h.bad || ng == 0)
        return false;
    size_t total = 36;
    for (int i = 0; i < 7; i++) {
        if (sz[i] > n)
            return false;
        total += sz[i];
    }
    if (total > n)
        return false;
    const uint8_t *s0 = d + 36;
    struct cur ncont = { s0, s0 + sz[0], false }, npts = { s0 + sz[0], s0 + sz[0] + sz[1], false };
    const uint8_t *fl = s0 + sz[0] + sz[1], *flend = fl + sz[2];
    struct cur glyph = { flend, flend + sz[3], false };
    const uint8_t *cmp = flend + sz[3];
    struct cur comp = { cmp, cmp + sz[4], false };
    const uint8_t *bb = cmp + sz[4];
    struct cur bbox = { bb, bb + sz[5], false };
    const uint8_t *ins = bb + sz[5];
    struct cur instr = { ins, ins + sz[6], false };
    size_t bitmap = ((ng + 31) / 32) * 4;
    if (bitmap > sz[5])
        return false;
    const uint8_t *bbmap = bbox.p;
    bbox.p += bitmap;
    int16_t *xmin = calloc(ng, sizeof(int16_t));
    if (!xmin)
        return false;
    for (unsigned g = 0; g < ng; g++) {
        w2_u32(loca, (uint32_t)glyf->n);
        int nc = (int16_t)cur_u16(&ncont);
        if (ncont.bad || glyf->bad)
            goto fail;
        if (nc == 0)
            continue;
        size_t start = glyf->n;
        bool explicit_bbox = (bbmap[g >> 3] & (0x80 >> (g & 7))) != 0;
        if (nc > 0) {
            unsigned ends[1024], total_pts = 0;
            if (nc > 1024)
                goto fail;
            for (int c = 0; c < nc; c++) {
                total_pts += cur_255(&npts);
                ends[c] = total_pts - 1;
            }
            if (npts.bad || total_pts > 65535 || (size_t)(flend - fl) < total_pts)
                goto fail;
            int32_t *xs = malloc(sizeof(int32_t) * (total_pts + 1)), *ys = malloc(sizeof(int32_t) * (total_pts + 1));
            uint8_t *onc = malloc(total_pts + 1);
            if (!xs || !ys || !onc) {
                free(xs); free(ys); free(onc);
                goto fail;
            }
            int32_t x = 0, y = 0, minx = 32767, maxx = -32768, miny = 32767, maxy = -32768;
            for (unsigned i = 0; i < total_pts; i++) {
                unsigned flag = *fl++;
                onc[i] = !(flag & 0x80);
                flag &= 0x7f;
                int32_t dx, dy;
                size_t nb = flag < 84 ? 1 : flag < 120 ? 2 : flag < 124 ? 3 : 4;
                if ((size_t)(glyph.end - glyph.p) < nb) {
                    free(xs); free(ys); free(onc);
                    goto fail;
                }
                const uint8_t *in = glyph.p;
                if (flag < 10) {
                    dx = 0;
                    dy = with_sign(flag, ((flag & 14) << 7) + in[0]);
                } else if (flag < 20) {
                    dx = with_sign(flag, (((flag - 10) & 14) << 7) + in[0]);
                    dy = 0;
                } else if (flag < 84) {
                    int b0 = (int)flag - 20, b1 = in[0];
                    dx = with_sign(flag, 1 + (b0 & 0x30) + (b1 >> 4));
                    dy = with_sign(flag >> 1, 1 + ((b0 & 0x0c) << 2) + (b1 & 0x0f));
                } else if (flag < 120) {
                    int b0 = (int)flag - 84;
                    dx = with_sign(flag, 1 + ((b0 / 12) << 8) + in[0]);
                    dy = with_sign(flag >> 1, 1 + (((b0 % 12) >> 2) << 8) + in[1]);
                } else if (flag < 124) {
                    int b2 = in[1];
                    dx = with_sign(flag, (in[0] << 4) + (b2 >> 4));
                    dy = with_sign(flag >> 1, ((b2 & 0x0f) << 8) + in[2]);
                } else {
                    dx = with_sign(flag, (in[0] << 8) + in[1]);
                    dy = with_sign(flag >> 1, (in[2] << 8) + in[3]);
                }
                glyph.p += nb;
                x += dx;
                y += dy;
                xs[i] = x;
                ys[i] = y;
                minx = x < minx ? x : minx; maxx = x > maxx ? x : maxx; miny = y < miny ? y : miny; maxy = y > maxy ? y : maxy;
            }
            unsigned ilen = cur_255(&glyph);
            if (glyph.bad || ilen > (size_t)(instr.end - instr.p)) {
                free(xs); free(ys); free(onc);
                goto fail;
            }
            w2_u16(glyf, (unsigned)nc);
            if (explicit_bbox) {
                for (int i = 0; i < 4; i++)
                    w2_u16(glyf, cur_u16(&bbox));
                xmin[g] = (int16_t)((glyf->p[glyf->n - 8] << 8) | glyf->p[glyf->n - 7]);
            } else {
                w2_u16(glyf, (unsigned)minx & 0xffff);
                w2_u16(glyf, (unsigned)miny & 0xffff);
                w2_u16(glyf, (unsigned)maxx & 0xffff);
                w2_u16(glyf, (unsigned)maxy & 0xffff);
                xmin[g] = (int16_t)minx;
            }
            for (int c = 0; c < nc; c++)
                w2_u16(glyf, ends[c]);
            w2_u16(glyf, ilen);
            w2_put(glyf, instr.p, ilen);
            instr.p += ilen;
            int32_t px = 0, py = 0;
            for (unsigned i = 0; i < total_pts; i++) {                  /* flags: on curve only, no short vectors: the coordinates follow as 16 bit deltas */
                uint8_t f = onc[i] ? 1 : 0;
                w2_put(glyf, &f, 1);
            }
            for (unsigned i = 0; i < total_pts; i++) {
                w2_u16(glyf, (unsigned)(xs[i] - px) & 0xffff);
                px = xs[i];
            }
            for (unsigned i = 0; i < total_pts; i++) {
                w2_u16(glyf, (unsigned)(ys[i] - py) & 0xffff);
                py = ys[i];
            }
            free(xs); free(ys); free(onc);
        } else {                                                        /* composite: the components are copied as they are */
            w2_u16(glyf, 0xffff);
            for (int i = 0; i < 4; i++)
                w2_u16(glyf, cur_u16(&bbox));
            xmin[g] = (int16_t)((glyf->p[glyf->n - 8] << 8) | glyf->p[glyf->n - 7]);
            unsigned flags;
            bool have_instr = false;
            do {
                flags = cur_u16(&comp);
                w2_u16(glyf, flags);
                size_t arg = (flags & 1) ? 4 : 2, extra = (flags & 8) ? 2 : (flags & 0x40) ? 4 : (flags & 0x80) ? 8 : 0;
                w2_u16(glyf, cur_u16(&comp));                           /* the component's glyph */
                if (comp.bad || (size_t)(comp.end - comp.p) < arg + extra)
                    goto fail;
                w2_put(glyf, comp.p, arg + extra);
                comp.p += arg + extra;
                have_instr |= (flags & 0x100) != 0;
            } while ((flags & 0x20) && !comp.bad);
            if (have_instr) {
                unsigned ilen = cur_255(&glyph);
                if (glyph.bad || ilen > (size_t)(instr.end - instr.p))
                    goto fail;
                w2_u16(glyf, ilen);
                w2_put(glyf, instr.p, ilen);
                instr.p += ilen;
            }
        }
        while ((glyf->n - start) & 3) {                                 /* each glyph on a multiple of four bytes */
            uint8_t z = 0;
            w2_put(glyf, &z, 1);
        }
        if (glyf->bad || comp.bad || bbox.bad || glyph.bad)
            goto fail;
    }
    w2_u32(loca, (uint32_t)glyf->n);
    *xmin_out = xmin;
    *nglyphs_out = ng;
    *index_fmt = idxfmt;
    return !glyf->bad && !loca->bad;
fail:
    free(xmin);
    return false;
}

static int w2_cmp(const void *a, const void *b)
{
    return memcmp(((const struct w2table *)a)->tag, ((const struct w2table *)b)->tag, 4);
}

typedef int (*brotli_fn)(size_t, const uint8_t *, size_t *, uint8_t *);

static uint8_t *woff2_to_sfnt(const uint8_t *in, size_t n, size_t *outn, char *err, size_t errn)
{
    if (n < 48) {
        seterr(err, errn, "A damaged WOFF 2 font.");
        return NULL;
    }
    uint32_t flavor = be32(in + 4), nt = be16(in + 12), comp_size = be32(in + 20);
    if (flavor == 0x74746366 || nt == 0 || nt > 128) {
        seterr(err, errn, "A WOFF 2 font collection: not read.");
        return NULL;
    }
    struct cur c = { in + 48, in + n, false };
    struct w2table *tb = calloc(nt, sizeof(*tb));
    if (!tb)
        return NULL;
    size_t unpacked = 0;
    for (uint32_t i = 0; i < nt && !c.bad; i++) {
        unsigned flags = cur_u8(&c), idx = flags & 63, ver = flags >> 6;
        if (idx == 63) {
            for (int k = 0; k < 4; k++)
                tb[i].tag[k] = (char)cur_u8(&c);
        } else {
            memcpy(tb[i].tag, k_w2tags[idx], 4);
        }
        tb[i].orig = cur_base128(&c);
        bool glyfloca = !memcmp(tb[i].tag, "glyf", 4) || !memcmp(tb[i].tag, "loca", 4);
        tb[i].transformed = glyfloca ? ver == 0 : ver != 0;
        tb[i].len = tb[i].transformed ? cur_base128(&c) : tb[i].orig;
        unpacked += tb[i].len;
        if (unpacked > FONT_MAX) {
            c.bad = true;
        }
    }
    if (c.bad || (size_t)(in + n - c.p) < comp_size) {
        free(tb);
        seterr(err, errn, "A damaged WOFF 2 font (its table directory).");
        return NULL;
    }
    static void *lib;
    static brotli_fn brotli;
    if (!lib) {
        lib = dlopen("libbrotlidec.so.1", RTLD_NOW);
        if (lib)
            brotli = (brotli_fn)dlsym(lib, "BrotliDecoderDecompress");
    }
    if (!brotli) {
        free(tb);
        seterr(err, errn, "A WOFF 2 font needs the system's libbrotlidec, which is not installed.");
        return NULL;
    }
    uint8_t *raw = malloc(unpacked ? unpacked : 1);
    size_t rawn = unpacked;
    if (!raw || brotli(comp_size, c.p, &rawn, raw) != 1 || rawn != unpacked) {
        free(raw);
        free(tb);
        seterr(err, errn, "A WOFF 2 font whose data does not unpack.");
        return NULL;
    }
    size_t pos = 0;
    for (uint32_t i = 0; i < nt; i++) {
        tb[i].data = raw + pos;
        pos += tb[i].len;
    }
    struct w2buf glyf = { 0 }, loca = { 0 }, hm = { 0 };
    int16_t *xmin = NULL;
    uint8_t *head_copy = NULL;
    unsigned ng = 0, idxfmt = 0;
    int gi = -1, li = -1, hi = -1, hhi = -1;
    for (uint32_t i = 0; i < nt; i++) {
        if (!memcmp(tb[i].tag, "glyf", 4)) gi = (int)i;
        else if (!memcmp(tb[i].tag, "loca", 4)) li = (int)i;
        else if (!memcmp(tb[i].tag, "hmtx", 4)) hi = (int)i;
        else if (!memcmp(tb[i].tag, "hhea", 4)) hhi = (int)i;
    }
    bool ok = true;
    if (gi >= 0 && tb[gi].transformed) {
        ok = li >= 0 && w2_glyf(tb[gi].data, tb[gi].len, &glyf, &loca, &xmin, &ng, &idxfmt);
        if (ok) {                                                       /* loca stays long (the rebuilt glyphs are bigger than the original's: short offsets might not reach) */
            tb[gi].out = glyf.p;
            tb[gi].outn = glyf.n;
            tb[li].out = loca.p;
            tb[li].outn = loca.n;
            for (uint32_t i = 0; i < nt; i++)
                if (!memcmp(tb[i].tag, "head", 4) && !tb[i].transformed && tb[i].len >= 54) {      /* ... so head must say so: indexToLocFormat = 1 */
                    tb[i].out = malloc(tb[i].len);
                    if (tb[i].out) {
                        memcpy(tb[i].out, tb[i].data, tb[i].len);
                        tb[i].out[50] = 0;
                        tb[i].out[51] = 1;
                        tb[i].outn = tb[i].len;
                        head_copy = tb[i].out;
                    }
                }
        }
    }
    if (ok && hi >= 0 && tb[hi].transformed && hhi >= 0) {              /* hmtx without the left side bearings: they are the glyphs' xMin */
        const uint8_t *d = tb[hi].data;
        size_t dn = tb[hi].len;
        unsigned nm = tb[hhi].len >= 36 && !tb[hhi].transformed ? be16(tb[hhi].data + 34) : 0;
        if (dn < 1 || !xmin || nm == 0 || nm > ng) {
            ok = false;
        } else {
            unsigned f = d[0];
            size_t p = 1;
            if (p + (size_t)nm * 2 > dn) {
                ok = false;
            } else {
                const uint8_t *adv = d + p;
                p += (size_t)nm * 2;
                const uint8_t *lsb = (f & 1) ? NULL : d + p;
                if (lsb)
                    p += (size_t)nm * 2;
                const uint8_t *lsb2 = (f & 2) ? NULL : d + p;
                if (lsb2)
                    p += (size_t)(ng - nm) * 2;
                if (p > dn || (lsb && !lsb) )
                    ok = false;
                for (unsigned g = 0; ok && g < ng; g++) {
                    if (g < nm) {
                        w2_put(&hm, adv + 2 * g, 2);
                        w2_u16(&hm, lsb ? be16(lsb + 2 * g) : (unsigned)(uint16_t)xmin[g]);
                    } else {
                        w2_u16(&hm, lsb2 ? be16(lsb2 + 2 * (g - nm)) : (unsigned)(uint16_t)xmin[g]);
                    }
                }
                if (ok) {
                    tb[hi].out = hm.p;
                    tb[hi].outn = hm.n;
                }
            }
        }
    }
    uint8_t *out = NULL;
    if (ok) {
        qsort(tb, nt, sizeof(*tb), w2_cmp);
        size_t total = 12 + (size_t)nt * 16;
        for (uint32_t i = 0; i < nt; i++)
            total += ((tb[i].out ? tb[i].outn : tb[i].orig) + 3u) & ~3u;
        if (total > FONT_MAX) {
            ok = false;
        } else {
            out = calloc(1, total);
        }
        if (out) {
            uint32_t pow2 = 1, lg = 0;
            while (pow2 * 2 <= nt) {
                pow2 *= 2;
                lg++;
            }
            out[0] = (uint8_t)(flavor >> 24); out[1] = (uint8_t)(flavor >> 16); out[2] = (uint8_t)(flavor >> 8); out[3] = (uint8_t)flavor;
            out[4] = (uint8_t)(nt >> 8); out[5] = (uint8_t)nt;
            uint32_t sr = pow2 * 16, rs = nt * 16 - sr;
            out[6] = (uint8_t)(sr >> 8); out[7] = (uint8_t)sr; out[8] = (uint8_t)(lg >> 8); out[9] = (uint8_t)lg; out[10] = (uint8_t)(rs >> 8); out[11] = (uint8_t)rs;
            size_t at = 12 + (size_t)nt * 16;
            for (uint32_t i = 0; i < nt; i++) {
                const uint8_t *src = tb[i].out ? tb[i].out : tb[i].data;
                size_t len = tb[i].out ? tb[i].outn : tb[i].orig;
                if (!tb[i].out && tb[i].len < len)
                    len = tb[i].len;
                memcpy(out + at, src, len);
                uint8_t *e = out + 12 + (size_t)i * 16;
                memcpy(e, tb[i].tag, 4);
                e[8] = (uint8_t)(at >> 24); e[9] = (uint8_t)(at >> 16); e[10] = (uint8_t)(at >> 8); e[11] = (uint8_t)at;
                e[12] = (uint8_t)(len >> 24); e[13] = (uint8_t)(len >> 16); e[14] = (uint8_t)(len >> 8); e[15] = (uint8_t)len;
                at += (len + 3u) & ~3u;
            }
            *outn = total;
        }
    }
    if (!out)
        seterr(err, errn, "A damaged WOFF 2 font (its glyph data).");
    free(raw);
    free(glyf.p);
    free(loca.p);
    free(head_copy);
    free(hm.p);
    free(xmin);
    free(tb);
    return out;
}

/* ---- tables ---- */
static bool sfnt_table(const uint8_t *d, size_t n, uint32_t base, const char *tag, size_t *off, size_t *len)
{
    if (base + 12 > n)
        return false;
    uint32_t nt = be16(d + base + 4);
    for (uint32_t i = 0; i < nt; i++) {
        size_t e = base + 12 + (size_t)i * 16;
        if (e + 16 > n)
            return false;
        if (!memcmp(d + e, tag, 4)) {
            uint32_t o = be32(d + e + 8), l = be32(d + e + 12);
            if (o > n || l > n - o)
                return false;
            *off = o;
            *len = l;
            return true;
        }
    }
    return false;
}

static void put_utf8(char *o, size_t *w, size_t cap, unsigned cp)
{
    if (cp < 0x80 && *w + 1 < cap) {
        o[(*w)++] = (char)(cp < 0x20 ? ' ' : cp);
    } else if (cp < 0x800 && *w + 2 < cap) {
        o[(*w)++] = (char)(0xc0 | cp >> 6);
        o[(*w)++] = (char)(0x80 | (cp & 0x3f));
    } else if (cp >= 0x800 && *w + 3 < cap && cp < 0x10000) {
        o[(*w)++] = (char)(0xe0 | cp >> 12);
        o[(*w)++] = (char)(0x80 | ((cp >> 6) & 0x3f));
        o[(*w)++] = (char)(0x80 | (cp & 0x3f));
    } else if (cp >= 0x10000 && *w + 4 < cap) {
        o[(*w)++] = (char)(0xf0 | cp >> 18);
        o[(*w)++] = (char)(0x80 | ((cp >> 12) & 0x3f));
        o[(*w)++] = (char)(0x80 | ((cp >> 6) & 0x3f));
        o[(*w)++] = (char)(0x80 | (cp & 0x3f));
    }
}

/* One string of the `name` table: Windows English first, then any Windows, Unicode, Mac. */
static void name_string(const uint8_t *nt, size_t nlen, int id, char *o, size_t cap)
{
    o[0] = 0;
    if (nlen < 6)
        return;
    uint32_t count = be16(nt + 2), soff = be16(nt + 4);
    int best = -1, bestscore = -1;
    for (uint32_t i = 0; i < count && 6 + (size_t)(i + 1) * 12 <= nlen; i++) {
        const uint8_t *r = nt + 6 + (size_t)i * 12;
        if ((int)be16(r + 6) != id)
            continue;
        uint32_t plat = be16(r), lang = be16(r + 4);
        int score = plat == 3 && lang == 0x409 ? 4 : plat == 3 ? 3 : plat == 0 ? 2 : plat == 1 ? 1 : 0;
        if (score > bestscore && (size_t)soff + be16(r + 10) + be16(r + 8) <= nlen) {
            best = (int)i;
            bestscore = score;
        }
    }
    if (best < 0)
        return;
    const uint8_t *r = nt + 6 + (size_t)best * 12;
    uint32_t plat = be16(r), len = be16(r + 8), off = soff + be16(r + 10);
    size_t w = 0;
    if (plat == 3 || plat == 0) {                                       /* UTF-16BE */
        for (uint32_t i = 0; i + 1 < len; i += 2) {
            unsigned cp = be16(nt + off + i);
            if (cp >= 0xd800 && cp < 0xdc00 && i + 3 < len) {
                unsigned lo = be16(nt + off + i + 2);
                if (lo >= 0xdc00 && lo < 0xe000) {
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                    i += 2;
                }
            }
            put_utf8(o, &w, cap, cp);
        }
    } else {                                                            /* Mac Roman: the plain letters are right, the rest is shown as it is */
        for (uint32_t i = 0; i < len; i++)
            put_utf8(o, &w, cap, nt[off + i] < 0x80 ? nt[off + i] : '?');
    }
    o[w] = 0;
    while (w > 0 && o[w - 1] == ' ')
        o[--w] = 0;
}

/* ---- the sample ---- */
struct probe {
    const char *name;
    uint32_t cps[4];
};

static const struct probe k_scripts[] = {
    { "Latin", { 'A', 'z', 0xe9, 0 } }, { "Latin Extended", { 0x100, 0x141, 0x15f, 0 } }, { "Estonian", { 0xd5, 0xf5, 0x160, 0x17e } },
    { "Cyrillic", { 0x416, 0x44f, 0, 0 } }, { "Greek", { 0x3a9, 0x3b1, 0, 0 } }, { "Hebrew", { 0x5d0, 0, 0, 0 } }, { "Arabic", { 0x639, 0, 0, 0 } },
    { "Devanagari", { 0x905, 0, 0, 0 } }, { "Thai", { 0xe01, 0, 0, 0 } }, { "Hiragana", { 0x3042, 0, 0, 0 } }, { "Hangul", { 0xd55c, 0, 0, 0 } },
    { "CJK", { 0x4e2d, 0, 0, 0 } }, { "Symbols", { 0x20ac, 0x2192, 0x2260, 0 } }, { "Box drawing", { 0x2500, 0x2588, 0, 0 } }, { "Emoji", { 0x1f600, 0, 0, 0 } },
};

static void scripts_text(const struct lm_ttf *f, char *o, size_t cap)
{
    size_t w = 0;
    o[0] = 0;
    for (size_t i = 0; i < sizeof(k_scripts) / sizeof(k_scripts[0]); i++) {
        bool all = true;
        for (int k = 0; k < 4 && k_scripts[i].cps[k]; k++)
            if (!lm_ttf_glyph(f, k_scripts[i].cps[k]))
                all = false;
        if (all && w + strlen(k_scripts[i].name) + 3 < cap)
            w += (size_t)snprintf(o + w, cap - w, "%s%s", w ? ", " : "", k_scripts[i].name);
    }
}

/* Draws `text` (UTF-8) at pixel size `px` into the coverage picture, the baseline at `base`; shrinks to fit the width. Returns the height used. */
static void draw_line(struct lm_ttf *f, uint8_t *pic, int w, int h, uint8_t *scratch, const char *text, double px, int *y)
{
    int units = lm_ttf_units(f);
    double sc = px / units;
    uint32_t cps[200];
    int n = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p && n < 199;) {
        uint32_t cp = *p < 0x80 ? *p : (*p >> 5) == 6 ? (uint32_t)(*p & 0x1f) << 6 | (p[1] & 0x3f) : (*p >> 4) == 14 ? (uint32_t)(*p & 0xf) << 12 | (uint32_t)(p[1] & 0x3f) << 6 | (p[2] & 0x3f)
                       : (uint32_t)(*p & 7) << 18 | (uint32_t)(p[1] & 0x3f) << 12 | (uint32_t)(p[2] & 0x3f) << 6 | (p[3] & 0x3f);
        p += *p < 0x80 ? 1 : (*p >> 5) == 6 ? 2 : (*p >> 4) == 14 ? 3 : 4;
        cps[n++] = cp;
    }
    double width = 0;
    for (int i = 0; i < n; i++) {
        int g = lm_ttf_glyph(f, cps[i]);
        width += g ? lm_ttf_advance(f, g) * sc : units * sc * 0.5;
    }
    if (width > w - 24) {
        sc *= (w - 24) / width;
        px *= (w - 24) / width;
    }
    int asc = (int)(lm_ttf_ascent(f) * sc + 1), desc = (int)(-lm_ttf_descent(f) * sc + 1);
    int base = *y + asc + 4;
    if (base + desc + 4 > h)
        return;
    double pen = 12;
    for (int i = 0; i < n; i++) {
        int g = lm_ttf_glyph(f, cps[i]);
        if (!g) {
            pen += units * sc * 0.5;
            continue;
        }
        memset(scratch, 0, (size_t)w * (size_t)h);
        if (lm_ttf_render(f, g, sc, sc, pen, base, scratch, w, h))
            for (size_t k = (size_t)(base - asc - 4 > 0 ? base - asc - 4 : 0) * (size_t)w; k < (size_t)(base + desc + 4 < h ? base + desc + 4 : h) * (size_t)w; k++)
                if (scratch[k] > pic[k])
                    pic[k] = scratch[k];
        pen += lm_ttf_advance(f, g) * sc;
    }
    *y = base + desc + (int)(px * 0.25) + 2;
}

static uint8_t *make_sample(struct lm_ttf *f, int *wout, int *hout)
{
    static const struct { const char *text; double px; } lines[] = {
        { "Aa Bb Cc Dd 0123", 120 }, { "ABCDEFGHIJKLMNOPQRSTUVWXYZ", 64 }, { "abcdefghijklmnopqrstuvwxyz", 64 }, { "0123456789 !?&@#%()[]{}<>", 56 },
        { "\xc3\x95\xc3\xb5 \xc3\x84\xc3\xa4 \xc3\x96\xc3\xb6 \xc3\x9c\xc3\xbc \xc5\xa0\xc5\xa1 \xc5\xbd\xc5\xbe \xe2\x82\xac \xe2\x86\x92 \xe2\x89\xa0", 56 },
        { "The quick brown fox jumps over the lazy dog.", 40 }, { "P\xc3\xb5" "dur Zagrebi t\xc5\xa1" "ellom\xc3\xa4ngija-f\xc3\xb6ljetonist Ciqo k\xc3\xbclmetas.", 36 },
        { "Sphinx of black quartz, judge my vow. 1234567890", 28 },
    };
    int w = SAMPLE_W, h = 0;
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++)
        h += (int)(lines[i].px * 1.45) + 8;
    uint8_t *pic = calloc((size_t)w, (size_t)h), *scratch = malloc((size_t)w * (size_t)h);
    if (!pic || !scratch) {
        free(pic);
        free(scratch);
        return NULL;
    }
    int y = 8;
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++)
        draw_line(f, pic, w, h, scratch, lines[i].text, lines[i].px, &y);
    free(scratch);
    *wout = w;
    *hout = y + 8 < h ? y + 8 : h;
    return pic;
}

int font_read(const char *path, struct font_info *fi, char *err, size_t errn)
{
    memset(fi, 0, sizeof(*fi));
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 44 || (uint64_t)st.st_size > FONT_MAX) {
        close(fd);
        return -1;
    }
    uint8_t *d = malloc((size_t)st.st_size);
    size_t n = (size_t)st.st_size;
    ssize_t got = d ? read(fd, d, n) : -1;
    close(fd);
    if (got != (ssize_t)n) {
        free(d);
        return -1;
    }
    uint32_t magic = be32(d);
    if (magic == 0x774f4632) {                                          /* "wOF2" */
        size_t sn = 0;
        uint8_t *sf = woff2_to_sfnt(d, n, &sn, err, errn);
        free(d);
        if (!sf)
            return -2;
        d = sf;
        n = sn;
        fi->woff = 2;
        magic = be32(d);
    }
    if (magic == 0x774f4646) {                                          /* "wOFF" */
        size_t sn = 0;
        uint8_t *s = woff_to_sfnt(d, n, &sn);
        free(d);
        if (!s) {
            seterr(err, errn, "A damaged WOFF font.");
            return -2;
        }
        d = s;
        n = sn;
        fi->woff = 1;
        magic = be32(d);
    }
    if (magic != 0x00010000 && magic != 0x74727565 && magic != 0x4f54544f && magic != 0x74746366) {
        free(d);
        return -1;
    }
    uint32_t base = magic == 0x74746366 && n >= 16 ? be32(d + 12) : 0;
    fi->cff = be32(d + base) == 0x4f54544f;
    size_t no, nl, ho, hl, mo, ml, oo, ol, po, pl;
    if (!sfnt_table(d, n, base, "head", &ho, &hl) || !sfnt_table(d, n, base, "maxp", &mo, &ml) || hl < 54 || ml < 6) {
        free(d);
        seterr(err, errn, "A font file without its basic tables.");
        return -2;
    }
    fi->units = (int)be16(d + ho + 18);
    fi->glyphs = (int)be16(d + mo + 4);
    fi->italic = (be16(d + ho + 44) & 2) != 0;
    if (sfnt_table(d, n, base, "OS/2", &oo, &ol) && ol >= 64) {
        fi->weight = (int)be16(d + oo + 4);
        if (be16(d + oo + 62) & 1)
            fi->italic = 1;
    }
    if (sfnt_table(d, n, base, "post", &po, &pl) && pl >= 16)
        fi->monospace = be32(d + po + 12) != 0;
    if (sfnt_table(d, n, base, "name", &no, &nl)) {
        const uint8_t *nt = d + no;
        name_string(nt, nl, 1, fi->family, sizeof(fi->family));
        name_string(nt, nl, 2, fi->style, sizeof(fi->style));
        name_string(nt, nl, 4, fi->full, sizeof(fi->full));
        name_string(nt, nl, 5, fi->version, sizeof(fi->version));
        name_string(nt, nl, 6, fi->psname, sizeof(fi->psname));
        name_string(nt, nl, 0, fi->copyright, sizeof(fi->copyright));
        name_string(nt, nl, 8, fi->manufacturer, sizeof(fi->manufacturer));
        name_string(nt, nl, 9, fi->designer, sizeof(fi->designer));
        name_string(nt, nl, 10, fi->description, sizeof(fi->description));
        name_string(nt, nl, 13, fi->license, sizeof(fi->license));
        name_string(nt, nl, 14, fi->license_url, sizeof(fi->license_url));
        char typo[128];
        name_string(nt, nl, 16, typo, sizeof(typo));                    /* the typographic family, when there is one, is the better name */
        if (typo[0])
            snprintf(fi->family, sizeof(fi->family), "%s", typo);
        name_string(nt, nl, 17, typo, sizeof(typo));
        if (typo[0])
            snprintf(fi->style, sizeof(fi->style), "%s", typo);
    }
    const char *why;
    struct lm_ttf *f = lm_ttf_open_mem(d, n, &why, true);
    if (f) {
        scripts_text(f, fi->scripts, sizeof(fi->scripts));
        if (!fi->cff) {
            int w = 0, h = 0;
            fi->alpha = make_sample(f, &w, &h);
            fi->w = w;
            fi->h = h;
        }
        lm_ttf_close(f);
    }
    free(d);
    return 0;
}
