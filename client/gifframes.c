#include "gifframes.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fail(char *err, size_t n, const char *msg)
{
    if (err && n)
        snprintf(err, n, "%s", msg);
}

static unsigned le16(const uint8_t *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

void lm_gif_anim_free(struct gif_anim *g)
{
    for (int i = 0; i < g->n; i++)
        free(g->px[i]);
    free(g->px);
    free(g->delay_ms);
    memset(g, 0, sizeof(*g));
}

/* The LZW codes of one picture into palette indices (`total` of them); false when the data is damaged before the picture is complete. */
static bool lzw_decode(const uint8_t *data, size_t dn, int minbits, uint8_t *out, size_t total)
{
    if (minbits < 1 || minbits > 11)
        return false;
    static uint16_t prefix[4096];
    static uint8_t suffix[4096], stack[4100];
    int clear = 1 << minbits, eoi = clear + 1, next = clear + 2, size = minbits + 1, old = -1;
    uint8_t first = 0;
    uint32_t acc = 0;
    int nb = 0;
    size_t ip = 0, op = 0;
    for (int i = 0; i < clear; i++) {
        prefix[i] = 0;
        suffix[i] = (uint8_t)i;
    }
    while (op < total) {
        while (nb < size && ip < dn) {
            acc |= (uint32_t)data[ip++] << nb;
            nb += 8;
        }
        if (nb < size)
            break;
        int code = (int)(acc & ((1u << size) - 1));
        acc >>= size;
        nb -= size;
        if (code == clear) {
            next = clear + 2;
            size = minbits + 1;
            old = -1;
            continue;
        }
        if (code == eoi)
            break;
        if (old < 0) {
            if (code >= clear)
                return false;
            out[op++] = (uint8_t)code;
            first = (uint8_t)code;
            old = code;
            continue;
        }
        int sp = 0, cur = code;
        if (code >= next) {
            if (code > next)
                return false;
            stack[sp++] = first;
            cur = old;
        }
        while (cur >= clear) {
            if (sp >= 4096)
                return false;
            stack[sp++] = suffix[cur];
            cur = prefix[cur];
        }
        stack[sp++] = (uint8_t)cur;
        first = (uint8_t)cur;
        if (next < 4096) {
            prefix[next] = (uint16_t)old;
            suffix[next] = first;
            next++;
            if (next == (1 << size) && size < 12)
                size++;
        }
        old = code;
        while (sp > 0 && op < total)
            out[op++] = stack[--sp];
    }
    return op == total;
}

int lm_gif_frames(const uint8_t *d, size_t len, int max_frames, struct gif_anim *out, char *err, size_t errsz)
{
    memset(out, 0, sizeof(*out));
    if (len < 13 || (memcmp(d, "GIF87a", 6) && memcmp(d, "GIF89a", 6))) {
        fail(err, errsz, "not a GIF file");
        return -1;
    }
    int sw = (int)le16(d + 6), sh = (int)le16(d + 8);
    unsigned flags = d[10];
    size_t p = 13;
    uint8_t gct[768];
    int gn = 0;
    if (flags & 0x80) {
        gn = 2 << (flags & 7);
        if (p + (size_t)gn * 3 > len) {
            fail(err, errsz, "the colour table is cut off");
            return -1;
        }
        memcpy(gct, d + p, (size_t)gn * 3);
        p += (size_t)gn * 3;
    }
    if (sw < 1 || sh < 1 || (uint64_t)sw * (uint64_t)sh > 64u * 1024u * 1024u) {
        fail(err, errsz, "that size is not possible");
        return -1;
    }
    size_t np = (size_t)sw * (size_t)sh;
    uint32_t *canvas = calloc(np, 4), *saved = NULL;
    if (!canvas) {
        fail(err, errsz, "out of memory");
        return -1;
    }
    out->w = sw;
    out->h = sh;
    int disposal = 0, delay = 100, tindex = -1, pdisp = 0;
    int px0 = 0, py0 = 0, px1 = 0, py1 = 0;                              /* where the picture before lay */
    bool ok = true;
    while (p < len && ok) {
        uint8_t b = d[p++];
        if (b == 0x3b)
            break;
        if (b == 0x21) {                                                /* an extension */
            if (p >= len)
                break;
            uint8_t label = d[p++];
            if (label == 0xf9 && p + 5 < len && d[p] >= 4) {
                disposal = (d[p + 1] >> 2) & 7;
                delay = (int)le16(d + p + 2) * 10;
                tindex = (d[p + 1] & 1) ? d[p + 4] : -1;
            } else if (label == 0xff && p + 15 < len && d[p] == 11 && !memcmp(d + p + 1, "NETSCAPE2.0", 11) && d[p + 12] >= 3 && d[p + 13] == 1)
                out->loops = (int)le16(d + p + 14);
            while (p < len && d[p])
                p += (size_t)d[p] + 1;
            p++;
        } else if (b == 0x2c) {                                         /* a picture */
            if (p + 9 > len) {
                ok = false;
                break;
            }
            int left = (int)le16(d + p), top = (int)le16(d + p + 2), iw = (int)le16(d + p + 4), ih = (int)le16(d + p + 6);
            unsigned f = d[p + 8];
            p += 9;
            uint8_t lct[768];
            const uint8_t *pal = gct;
            int pn = gn;
            if (f & 0x80) {
                pn = 2 << (f & 7);
                if (p + (size_t)pn * 3 > len) {
                    ok = false;
                    break;
                }
                memcpy(lct, d + p, (size_t)pn * 3);
                pal = lct;
                p += (size_t)pn * 3;
            }
            if (p >= len || pn == 0) {
                ok = false;
                break;
            }
            int minbits = d[p++];
            size_t cap = 0, dn = 0;
            uint8_t *data = NULL;
            while (p < len && d[p]) {                                   /* the sub-blocks make one stream */
                size_t bl = d[p++];
                if (p + bl > len)
                    bl = len - p;
                if (dn + bl > cap) {
                    cap = (dn + bl) * 2 + 256;
                    uint8_t *nd = realloc(data, cap);
                    if (!nd) {
                        free(data);
                        data = NULL;
                        ok = false;
                        break;
                    }
                    data = nd;
                }
                if (bl) {
                    memcpy(data + dn, d + p, bl);
                    dn += bl;
                    p += bl;
                }
            }
            p++;
            size_t total = (size_t)iw * (size_t)ih;
            uint8_t *pic = ok ? malloc(total ? total : 1) : NULL;
            if (!pic || !lzw_decode(data, dn, minbits, pic, total)) {
                free(pic);
                free(data);
                if (out->n == 0)
                    ok = false;                                         /* nothing usable at all */
                break;                                                  /* a damaged later picture ends the animation with what there is */
            }
            free(data);
            /* what the picture before asked for, done before this one is laid on */
            if (out->n > 0) {
                if (pdisp == 2) {
                    for (int y = py0; y < py1 && y < sh; y++)
                        for (int x = px0; x < px1 && x < sw; x++)
                            canvas[(size_t)y * (size_t)sw + (size_t)x] = 0;
                } else if (pdisp == 3 && saved) {
                    memcpy(canvas, saved, np * 4);
                }
            }
            free(saved);
            saved = NULL;
            if (disposal == 3) {
                saved = malloc(np * 4);
                if (saved)
                    memcpy(saved, canvas, np * 4);
            }
            bool inter = (f & 0x40) != 0;
            int pass = 0, row = 0, step = inter ? 8 : 1;
            for (int y = 0; y < ih; y++) {
                int dy = inter ? row : y;
                if (inter) {
                    row += step;
                    while (row >= ih && pass < 3) {
                        pass++;
                        row = pass == 1 ? 4 : pass == 2 ? 2 : 1;
                        step = pass == 1 ? 8 : pass == 2 ? 4 : 2;
                    }
                }
                int cy = top + dy;
                if (cy < 0 || cy >= sh)
                    continue;
                for (int x = 0; x < iw; x++) {
                    int cx = left + x;
                    if (cx < 0 || cx >= sw)
                        continue;
                    int ci = pic[(size_t)y * (size_t)iw + (size_t)x];
                    if (ci == tindex || ci >= pn)
                        continue;
                    canvas[(size_t)cy * (size_t)sw + (size_t)cx] = 0xff000000u | ((uint32_t)pal[ci * 3] << 16) | ((uint32_t)pal[ci * 3 + 1] << 8) | pal[ci * 3 + 2];
                }
            }
            free(pic);
            if (out->n < max_frames) {
                uint32_t **npx = realloc(out->px, (size_t)(out->n + 1) * sizeof(*npx));
                int *ndl = realloc(out->delay_ms, (size_t)(out->n + 1) * sizeof(int));
                if (npx)
                    out->px = npx;
                if (ndl)
                    out->delay_ms = ndl;
                uint32_t *copy = npx && ndl ? malloc(np * 4) : NULL;
                if (!copy) {
                    ok = false;
                    break;
                }
                memcpy(copy, canvas, np * 4);
                out->px[out->n] = copy;
                out->delay_ms[out->n] = delay > 0 ? delay : 100;
                out->n++;
            } else
                out->cut++;
            pdisp = disposal;
            px0 = left;
            py0 = top;
            px1 = left + iw;
            py1 = top + ih;
            disposal = 0;
            tindex = -1;
            delay = 100;
        } else {
            ok = false;                                                 /* not a block we know */
        }
    }
    free(canvas);
    free(saved);
    if (!ok && out->n == 0) {
        fail(err, errsz, "the GIF is damaged");
        lm_gif_anim_free(out);
        return -1;
    }
    if (out->n == 0) {
        fail(err, errsz, "the GIF has no pictures");
        lm_gif_anim_free(out);
        return -1;
    }
    return 0;
}
