#include "h264dec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "h264.h"

struct lm_h264dec {
    int w, h;                       /* shown size (cropped) */
    int mbw, mbh;
    int crop_r, crop_b;             /* in pixels */
    int qp_init, chroma_qp_offset;
    int log2_frame_num;
    int deblock_present;
    int ys, cs;
    uint8_t *Y, *U, *V;             /* the picture; a P picture is decoded in place over the one before */
    uint8_t *nzy, *nzc;
    int have_idr;
};

#define BAD(...) do { snprintf(err, errsz, "not supported: " __VA_ARGS__); return -1; } while (0)

/* ---- lookup tables made from the code tables ------------------------------------------------------------------------------- */

static uint16_t lut_ct[4][1 << 16];                 /* coeff_token: (length << 8) | (4 * total_coeff + trailing_ones) */
static uint16_t lut_cdc[1 << 8];                    /* chroma DC coeff_token, 8 bits */
static uint16_t lut_tz[15][1 << 9];                 /* total_zeros, 9 bits */
static uint16_t lut_cdctz[3][1 << 3];
static uint16_t lut_run[7][1 << 11];                /* run_before, 11 bits */
static int lut_ready;

static void fill(uint16_t *lut, int maxlen, const uint8_t *len, const uint8_t *bits, int nsym)
{
    for (int s = 0; s < nsym; s++) {
        if (!len[s])
            continue;
        int l = len[s], pad = maxlen - l;
        for (int j = 0; j < (1 << pad); j++)
            lut[(bits[s] << pad) + j] = (uint16_t)((l << 8) | s);
    }
}

static void build_luts(void)
{
    if (lut_ready)
        return;
    for (int t = 0; t < 4; t++)
        fill(lut_ct[t], 16, lm_h264_coeff_token_len[t], lm_h264_coeff_token_bits[t], 68);
    fill(lut_cdc, 8, lm_h264_cdc_token_len, lm_h264_cdc_token_bits, 20);
    for (int t = 0; t < 15; t++)
        fill(lut_tz[t], 9, lm_h264_total_zeros_len[t], lm_h264_total_zeros_bits[t], 16);
    for (int t = 0; t < 3; t++)
        fill(lut_cdctz[t], 3, lm_h264_cdc_zeros_len[t], lm_h264_cdc_zeros_bits[t], 4);
    for (int t = 0; t < 7; t++)
        fill(lut_run[t], 11, lm_h264_run_len[t], lm_h264_run_bits[t], 16);
    lut_ready = 1;
}

/* ---- parameter sets ---------------------------------------------------------------------------------------------------------- */

static int skip_vui(struct lm_br *r)
{
    (void)r;
    return 0;                                       /* nothing in it changes how pictures are decoded */
}

struct lm_h264dec *lm_h264dec_create(const uint8_t *sps, size_t sps_len, const uint8_t *pps, size_t pps_len, char *err, size_t errsz)
{
    build_luts();
    uint8_t *buf = malloc(sps_len + pps_len + 8);
    struct lm_h264dec *d = calloc(1, sizeof(*d));
    if (!buf || !d) {
        free(buf);
        free(d);
        snprintf(err, errsz, "out of memory");
        return NULL;
    }
    struct lm_br r;
    uint32_t v;
    int32_t sv;
    int32_t flags;
    size_t n;
    /* SPS */
    if (sps_len < 5 || (sps[0] & 31) != 7)
        goto bad_sps;
    n = lm_h264_unescape(sps + 1, sps_len - 1, buf);
    r = (struct lm_br){ buf, n * 8, 0 };
    if (lm_br_get(&r, 8) != 66)
        { snprintf(err, errsz, "not supported: this is not constrained baseline H.264"); goto fail; }
    lm_br_get(&r, 16);                                              /* constraint flags, level */
    if (lm_br_ue(&r, &v) < 0 || v != 0)
        goto bad_sps;
    if (lm_br_ue(&r, &v) < 0 || v > 12)
        goto bad_sps;
    d->log2_frame_num = (int)v + 4;
    if (lm_br_ue(&r, &v) < 0 || v != 2)
        { snprintf(err, errsz, "not supported: picture order count type %u", (unsigned)v); goto fail; }
    if (lm_br_ue(&r, &v) < 0 || v < 1 || v > 1)
        { snprintf(err, errsz, "not supported: more than one reference picture"); goto fail; }
    lm_br_get(&r, 1);                                               /* gaps_in_frame_num */
    if (lm_br_ue(&r, &v) < 0)
        goto bad_sps;
    d->mbw = (int)v + 1;
    if (lm_br_ue(&r, &v) < 0)
        goto bad_sps;
    d->mbh = (int)v + 1;
    if (!lm_br_get(&r, 1))
        { snprintf(err, errsz, "not supported: interlaced video"); goto fail; }
    lm_br_get(&r, 1);                                               /* direct_8x8_inference_flag */
    if (d->mbw > 512 || d->mbh > 512 || (long)d->mbw * d->mbh > 65536)       /* at most 4096 x 4096 */
        goto bad_sps;
    int cl = 0, cr = 0, ct = 0, cb = 0;
    if (lm_br_get(&r, 1)) {
        uint32_t a, b2, c, e2;
        if (lm_br_ue(&r, &a) < 0 || lm_br_ue(&r, &b2) < 0 || lm_br_ue(&r, &c) < 0 || lm_br_ue(&r, &e2) < 0)
            goto bad_sps;
        cl = (int)a * 2;
        cr = (int)b2 * 2;
        ct = (int)c * 2;
        cb = (int)e2 * 2;
    }
    (void)skip_vui(&r);
    d->w = d->mbw * 16 - cl - cr;
    d->h = d->mbh * 16 - ct - cb;
    if (cl || ct || d->w < 2 || d->h < 2)
        { snprintf(err, errsz, "not supported: cropping at the left or top"); goto fail; }
    d->crop_r = cr;
    d->crop_b = cb;
    /* PPS */
    if (pps_len < 3 || (pps[0] & 31) != 8)
        goto bad_pps;
    n = lm_h264_unescape(pps + 1, pps_len - 1, buf);
    r = (struct lm_br){ buf, n * 8, 0 };
    if (lm_br_ue(&r, &v) < 0 || v != 0 || lm_br_ue(&r, &v) < 0 || v != 0)
        goto bad_pps;
    if (lm_br_get(&r, 1))
        { snprintf(err, errsz, "not supported: CABAC"); goto fail; }
    lm_br_get(&r, 1);
    if (lm_br_ue(&r, &v) < 0 || v != 0)
        { snprintf(err, errsz, "not supported: slice groups"); goto fail; }
    if (lm_br_ue(&r, &v) < 0 || v != 0 || lm_br_ue(&r, &v) < 0)
        goto bad_pps;
    if (lm_br_get(&r, 1) || lm_br_get(&r, 2))
        { snprintf(err, errsz, "not supported: weighted prediction"); goto fail; }
    if (lm_br_se(&r, &sv) < 0)
        goto bad_pps;
    d->qp_init = 26 + sv;
    if (lm_br_se(&r, &sv) < 0 || lm_br_se(&r, &sv) < 0)
        goto bad_pps;
    d->chroma_qp_offset = sv;
    d->deblock_present = (int)lm_br_get(&r, 1);
    if (lm_br_get(&r, 1))
        { snprintf(err, errsz, "not supported: constrained intra prediction"); goto fail; }
    flags = (int32_t)lm_br_get(&r, 1);
    if (flags)
        { snprintf(err, errsz, "not supported: redundant pictures"); goto fail; }
    if (d->qp_init < 0 || d->qp_init > 51 || d->chroma_qp_offset < -12 || d->chroma_qp_offset > 12)
        goto bad_pps;

    d->ys = d->mbw * 16;
    d->cs = d->mbw * 8;
    d->Y = calloc((size_t)d->ys * (size_t)d->mbh * 16, 1);
    d->U = calloc((size_t)d->cs * (size_t)d->mbh * 8, 1);
    d->V = calloc((size_t)d->cs * (size_t)d->mbh * 8, 1);
    d->nzy = calloc((size_t)d->mbw * (size_t)d->mbh, 16);
    d->nzc = calloc((size_t)d->mbw * (size_t)d->mbh, 8);
    if (!d->Y || !d->U || !d->V || !d->nzy || !d->nzc) {
        snprintf(err, errsz, "out of memory");
        goto fail;
    }
    free(buf);
    return d;
bad_sps:
    snprintf(err, errsz, "damaged or unusual SPS");
    goto fail;
bad_pps:
    snprintf(err, errsz, "damaged or unusual PPS");
fail:
    free(buf);
    lm_h264dec_free(d);
    return NULL;
}

void lm_h264dec_free(struct lm_h264dec *d)
{
    if (!d)
        return;
    free(d->Y);
    free(d->U);
    free(d->V);
    free(d->nzy);
    free(d->nzc);
    free(d);
}

const uint8_t *lm_h264dec_plane(const struct lm_h264dec *d, int plane, int *stride)
{
    *stride = plane ? d->cs : d->ys;
    return plane == 0 ? d->Y : plane == 1 ? d->U : d->V;
}

int lm_h264dec_width(const struct lm_h264dec *d) { return d->w; }
int lm_h264dec_height(const struct lm_h264dec *d) { return d->h; }

/* ---- CAVLC decoding ------------------------------------------------------------------------------------------------------------ */

static const uint8_t blk_idx[4][4] = { { 0, 1, 4, 5 }, { 2, 3, 6, 7 }, { 8, 9, 12, 13 }, { 10, 11, 14, 15 } };

static int nc_luma(const struct lm_h264dec *d, int mbx, int mby, int blk)
{
    int bx = lm_h264_blk_x[blk], by = lm_h264_blk_y[blk];
    size_t mb = (size_t)mby * (size_t)d->mbw + (size_t)mbx;
    int na = -1, nb = -1;
    if (bx > 0)
        na = d->nzy[mb * 16 + blk_idx[by][bx - 1]];
    else if (mbx > 0)
        na = d->nzy[(mb - 1) * 16 + blk_idx[by][3]];
    if (by > 0)
        nb = d->nzy[mb * 16 + blk_idx[by - 1][bx]];
    else if (mby > 0)
        nb = d->nzy[(mb - (size_t)d->mbw) * 16 + blk_idx[3][bx]];
    return na >= 0 && nb >= 0 ? (na + nb + 1) >> 1 : na >= 0 ? na : nb >= 0 ? nb : 0;
}

static int nc_chroma(const struct lm_h264dec *d, int mbx, int mby, int c, int blk)
{
    int bx = blk & 1, by = blk >> 1;
    size_t mb = (size_t)mby * (size_t)d->mbw + (size_t)mbx;
    int na = -1, nb = -1;
    if (bx > 0)
        na = d->nzc[mb * 8 + (size_t)c * 4 + (size_t)(blk - 1)];
    else if (mbx > 0)
        na = d->nzc[(mb - 1) * 8 + (size_t)c * 4 + (size_t)(by * 2 + 1)];
    if (by > 0)
        nb = d->nzc[mb * 8 + (size_t)c * 4 + (size_t)(blk - 2)];
    else if (mby > 0)
        nb = d->nzc[(mb - (size_t)d->mbw) * 8 + (size_t)c * 4 + (size_t)(2 + bx)];
    return na >= 0 && nb >= 0 ? (na + nb + 1) >> 1 : na >= 0 ? na : nb >= 0 ? nb : 0;
}

/* One residual block into coef[0..maxn-1] (scan order); returns total_coeff or -1. */
static int get_block(struct lm_br *r, int *coef, int maxn, int nc)
{
    memset(coef, 0, (size_t)maxn * sizeof(int));
    uint16_t e;
    if (nc < 0)
        e = lut_cdc[lm_br_peek(r, 8)];
    else
        e = lut_ct[nc < 2 ? 0 : nc < 4 ? 1 : nc < 8 ? 2 : 3][lm_br_peek(r, 16)];
    if (!(e >> 8))
        return -1;
    r->pos += (size_t)(e >> 8);
    int total = (e & 255) >> 2, t1 = e & 3;
    if (total > maxn || t1 > total)
        return -1;
    if (!total)
        return 0;
    int lev[16];
    for (int i = 0; i < t1; i++)
        lev[i] = lm_br_get(r, 1) ? -1 : 1;
    int sl = total > 10 && t1 < 3 ? 1 : 0;
    for (int i = t1; i < total; i++) {
        int prefix = 0;
        while (prefix < 32 && !lm_br_get(r, 1)) {
            prefix++;
            if (r->pos >= r->nbits + 64)
                return -1;
        }
        if (prefix > 15)
            return -1;                              /* (levels with longer prefixes do not occur at our quantizers) */
        int code = (prefix < 15 ? prefix : 15) << sl;
        int size = prefix == 14 && sl == 0 ? 4 : prefix >= 15 ? prefix - 3 : sl;
        if (size)
            code += (int)lm_br_get(r, size);
        if (prefix >= 15 && sl == 0)
            code += 15;
        if (i == t1 && t1 < 3)
            code += 2;
        lev[i] = (code & 1) ? (-code - 1) >> 1 : (code + 2) >> 1;
        if (sl == 0)
            sl = 1;
        if ((lev[i] < 0 ? -lev[i] : lev[i]) > (3 << (sl - 1)) && sl < 6)
            sl++;
    }
    int tz = 0;
    if (total < maxn) {
        if (maxn == 4) {
            uint16_t z = lut_cdctz[total - 1][lm_br_peek(r, 3)];
            if (!(z >> 8))
                return -1;
            r->pos += (size_t)(z >> 8);
            tz = z & 255;
        } else {
            uint16_t z = lut_tz[total - 1][lm_br_peek(r, 9)];
            if (!(z >> 8))
                return -1;
            r->pos += (size_t)(z >> 8);
            tz = z & 255;
        }
    }
    if (total + tz > maxn)
        return -1;
    int zeros_left = tz, pos = total + tz - 1;
    for (int i = 0; i < total; i++) {
        coef[pos] = lev[i];
        int run = 0;
        if (i < total - 1 && zeros_left > 0) {
            uint16_t z = lut_run[zeros_left > 7 ? 6 : zeros_left - 1][lm_br_peek(r, 11)];
            if (!(z >> 8))
                return -1;
            r->pos += (size_t)(z >> 8);
            run = z & 255;
            if (run > zeros_left)
                return -1;
        } else if (i == total - 1) {
            run = 0;
        }
        zeros_left -= run;
        pos -= 1 + run;
    }
    return total;
}

/* ---- macroblocks ------------------------------------------------------------------------------------------------------------------ */

static void add_block(uint8_t *dst, int stride, const int *r)
{
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            dst[y * stride + x] = (uint8_t)lm_h264_clip(dst[y * stride + x] + r[y * 4 + x]);
}

static void fill_dc_pred(struct lm_h264dec *d, int mbx, int mby)
{
    int dc = lm_h264_luma_dc_pred(d->Y, d->ys, mbx, mby);
    uint8_t *p = d->Y + (size_t)mby * 16 * (size_t)d->ys + (size_t)mbx * 16;
    for (int y = 0; y < 16; y++)
        memset(p + (size_t)y * (size_t)d->ys, dc, 16);
    for (int c = 0; c < 2; c++) {
        uint8_t *pl = c ? d->V : d->U;
        uint8_t *q = pl + (size_t)mby * 8 * (size_t)d->cs + (size_t)mbx * 8;
        int v[4];
        for (int b = 0; b < 4; b++)
            v[b] = lm_h264_chroma_dc_pred(pl, d->cs, mbx, mby, b & 1, b >> 1);
        for (int b = 0; b < 4; b++)
            for (int y = 0; y < 4; y++)
                memset(q + (size_t)((b >> 1) * 4 + y) * (size_t)d->cs + (b & 1) * 4, v[b], 4);
    }
}

/* Decodes the chroma part (DC and AC) and adds it to the prediction in the planes. */
static int mb_chroma(struct lm_h264dec *d, struct lm_br *r, int mbx, int mby, int cbp_c, int qpc)
{
    if (!cbp_c)
        return 0;
    size_t mb = (size_t)mby * (size_t)d->mbw + (size_t)mbx;
    int dcl[2][4], ac[2][4][15];
    memset(ac, 0, sizeof(ac));
    for (int c = 0; c < 2; c++)
        if (get_block(r, dcl[c], 4, -1) < 0)
            return -1;
    if (cbp_c == 2)
        for (int c = 0; c < 2; c++)
            for (int b = 0; b < 4; b++) {
                int tc = get_block(r, ac[c][b], 15, nc_chroma(d, mbx, mby, c, b));
                if (tc < 0)
                    return -1;
                d->nzc[mb * 8 + (size_t)c * 4 + (size_t)b] = (uint8_t)tc;
            }
    for (int c = 0; c < 2; c++) {
        uint8_t *cp = (c ? d->V : d->U) + (size_t)mby * 8 * (size_t)d->cs + (size_t)mbx * 8;
        int dc4[4], lv[16], res[16];
        lm_h264_dequant_chroma_dc(dcl[c], qpc, dc4);
        for (int b = 0; b < 4; b++) {
            memset(lv, 0, sizeof(lv));
            for (int k = 1; k < 16; k++)
                lv[lm_h264_zigzag4[k]] = ac[c][b][k - 1];
            lm_h264_dequant_idct4(lv, qpc, &dc4[b], res);
            add_block(cp + (size_t)(b >> 1) * 4 * (size_t)d->cs + (size_t)(b & 1) * 4, d->cs, res);
        }
    }
    return 0;
}

static int decode_slice(struct lm_h264dec *d, const uint8_t *rbsp, size_t n, int idr, char *err, size_t errsz)
{
    struct lm_br r = { rbsp, n * 8, 0 };
    uint32_t v;
    int32_t sv;
    if (lm_br_ue(&r, &v) < 0 || v != 0)
        BAD("more than one slice in a picture");
    if (lm_br_ue(&r, &v) < 0)
        BAD("slice type");
    int p_slice;
    if (v == 0 || v == 5)
        p_slice = 1;
    else if (v == 2 || v == 7)
        p_slice = 0;
    else
        BAD("slice type %u", (unsigned)v);
    if (idr && p_slice)
        BAD("a P slice in an IDR picture");
    if (lm_br_ue(&r, &v) < 0 || v != 0)
        BAD("picture parameter set %u", (unsigned)v);
    lm_br_get(&r, d->log2_frame_num > 24 ? 24 : d->log2_frame_num);      /* frame_num */
    if (idr && lm_br_ue(&r, &v) < 0)
        BAD("damaged slice header");
    if (p_slice) {
        if (lm_br_get(&r, 1)) {
            if (lm_br_ue(&r, &v) < 0 || v != 0)
                BAD("more than one reference picture");
        }
        if (lm_br_get(&r, 1))
            BAD("reference picture list modification");
    }
    if (idr)
        lm_br_get(&r, 2);
    else if (lm_br_get(&r, 1))
        BAD("adaptive reference picture marking");
    if (lm_br_se(&r, &sv) < 0)
        BAD("damaged slice header");
    int qp = d->qp_init + sv;
    if (qp < 0 || qp > 51)
        BAD("quantizer %d", qp);
    int qi = qp + d->chroma_qp_offset;
    int qpc = lm_h264_chroma_qp(qi < 0 ? 0 : qi > 51 ? 51 : qi);
    if (d->deblock_present) {
        if (lm_br_ue(&r, &v) < 0)
            BAD("damaged slice header");
        if (v != 1)
            BAD("the deblocking filter");
    }
    if (!idr && !d->have_idr)
        BAD("a P picture without an IDR picture before it");

    int mbs = d->mbw * d->mbh, mbi = 0;
    while (mbi < mbs) {
        if (p_slice) {
            uint32_t run;
            if (lm_br_ue(&r, &run) < 0 || run > (uint32_t)(mbs - mbi))
                BAD("damaged slice data");
            for (uint32_t k = 0; k < run; k++, mbi++) {                 /* P_Skip: the picture before shows through */
                memset(d->nzy + (size_t)mbi * 16, 0, 16);
                memset(d->nzc + (size_t)mbi * 8, 0, 8);
            }
            if (mbi >= mbs)
                break;
        }
        int mbx = mbi % d->mbw, mby = mbi / d->mbw;
        size_t mb = (size_t)mbi;
        uint32_t type;
        if (lm_br_ue(&r, &type) < 0)
            BAD("damaged slice data");
        memset(d->nzy + mb * 16, 0, 16);
        memset(d->nzc + mb * 8, 0, 8);
        int intra = p_slice ? type >= 5 : 1;
        uint32_t it = p_slice ? type - 5 : type;
        if (!intra) {
            if (type != 0)
                BAD("macroblock type %u", (unsigned)type);
            int32_t mvx, mvy;
            if (lm_br_se(&r, &mvx) < 0 || lm_br_se(&r, &mvy) < 0)
                BAD("damaged slice data");
            if (mvx || mvy)
                BAD("motion vectors");
            uint32_t code;
            if (lm_br_ue(&r, &code) < 0 || code > 47)
                BAD("damaged slice data");
            int cbp = lm_h264_inter_cbp[code];
            if (cbp) {
                if (lm_br_se(&r, &sv) < 0 || sv != 0)
                    BAD("a quantizer change inside a picture");
            }
            uint8_t *yp = d->Y + (size_t)mby * 16 * (size_t)d->ys + (size_t)mbx * 16;
            for (int b = 0; b < 16; b++) {
                if (!(cbp & (1 << (b >> 2))))
                    continue;
                int co[16], lv[16], res[16];
                int tc = get_block(&r, co, 16, nc_luma(d, mbx, mby, b));
                if (tc < 0)
                    BAD("damaged slice data");
                d->nzy[mb * 16 + (size_t)b] = (uint8_t)tc;
                if (!tc)
                    continue;
                memset(lv, 0, sizeof(lv));
                for (int k = 0; k < 16; k++)
                    lv[lm_h264_zigzag4[k]] = co[k];
                lm_h264_dequant_idct4(lv, qp, NULL, res);
                add_block(yp + (size_t)lm_h264_blk_y[b] * 4 * (size_t)d->ys + (size_t)lm_h264_blk_x[b] * 4, d->ys, res);
            }
            if (mb_chroma(d, &r, mbx, mby, cbp >> 4, qpc) < 0)
                BAD("damaged slice data");
        } else if (it == 25) {                                          /* I_PCM: raw samples */
            r.pos = (r.pos + 7) & ~(size_t)7;
            if (r.pos + 384 * 8 > r.nbits)
                BAD("damaged slice data");
            uint8_t *yp = d->Y + (size_t)mby * 16 * (size_t)d->ys + (size_t)mbx * 16;
            for (int y = 0; y < 16; y++)
                for (int x = 0; x < 16; x++)
                    yp[(size_t)y * (size_t)d->ys + (size_t)x] = (uint8_t)lm_br_get(&r, 8);
            for (int c = 0; c < 2; c++) {
                uint8_t *cp = (c ? d->V : d->U) + (size_t)mby * 8 * (size_t)d->cs + (size_t)mbx * 8;
                for (int y = 0; y < 8; y++)
                    for (int x = 0; x < 8; x++)
                        cp[(size_t)y * (size_t)d->cs + (size_t)x] = (uint8_t)lm_br_get(&r, 8);
            }
            memset(d->nzy + mb * 16, 16, 16);
            memset(d->nzc + mb * 8, 16, 8);
        } else if (it >= 1 && it <= 24) {                               /* Intra16x16 */
            int pred = (int)(it - 1) % 4, cbp_c = ((int)(it - 1) / 4) % 3, cbp_l = (it - 1) / 12 ? 15 : 0;
            if (pred != 2)
                BAD("an Intra16x16 prediction mode other than DC");
            uint32_t cm;
            if (lm_br_ue(&r, &cm) < 0 || cm != 0)
                BAD("a chroma prediction mode other than DC");
            if (lm_br_se(&r, &sv) < 0 || sv != 0)
                BAD("a quantizer change inside a picture");
            fill_dc_pred(d, mbx, mby);
            int dcl[16], lv[16], dc16[16], res[16];
            int tc = get_block(&r, dcl, 16, nc_luma(d, mbx, mby, 0));
            if (tc < 0)
                BAD("damaged slice data");
            for (int k = 0; k < 16; k++)
                lv[lm_h264_zigzag4[k]] = dcl[k];
            lm_h264_dequant_luma_dc(lv, qp, dc16);
            uint8_t *yp = d->Y + (size_t)mby * 16 * (size_t)d->ys + (size_t)mbx * 16;
            for (int b = 0; b < 16; b++) {
                int ac[15] = { 0 };
                if (cbp_l) {
                    int t = get_block(&r, ac, 15, nc_luma(d, mbx, mby, b));
                    if (t < 0)
                        BAD("damaged slice data");
                    d->nzy[mb * 16 + (size_t)b] = (uint8_t)t;
                }
                memset(lv, 0, sizeof(lv));
                for (int k = 1; k < 16; k++)
                    lv[lm_h264_zigzag4[k]] = ac[k - 1];
                int dc = dc16[lm_h264_blk_y[b] * 4 + lm_h264_blk_x[b]];
                lm_h264_dequant_idct4(lv, qp, &dc, res);
                add_block(yp + (size_t)lm_h264_blk_y[b] * 4 * (size_t)d->ys + (size_t)lm_h264_blk_x[b] * 4, d->ys, res);
            }
            if (mb_chroma(d, &r, mbx, mby, cbp_c, qpc) < 0)
                BAD("damaged slice data");
        } else {
            BAD("macroblock type %u", (unsigned)type);
        }
        mbi++;
    }
    if (idr)
        d->have_idr = 1;
    return 0;
}

int lm_h264dec_decode(struct lm_h264dec *d, const uint8_t *au, size_t len, char *err, size_t errsz)
{
    size_t at = 0;
    while (at + 4 <= len) {
        size_t n = (size_t)au[at] << 24 | (size_t)au[at + 1] << 16 | (size_t)au[at + 2] << 8 | au[at + 3];
        at += 4;
        if (n < 2 || n > len - at) {
            snprintf(err, errsz, "damaged picture data");
            return -1;
        }
        int type = au[at] & 31;
        if (type == 1 || type == 5) {
            uint8_t *rbsp = malloc(n);
            if (!rbsp) {
                snprintf(err, errsz, "out of memory");
                return -1;
            }
            size_t m = lm_h264_unescape(au + at + 1, n - 1, rbsp);
            int rc = decode_slice(d, rbsp, m, type == 5, err, errsz);
            free(rbsp);
            return rc;
        }
        at += n;                                                        /* SEI, parameter sets, delimiters: ignored */
    }
    snprintf(err, errsz, "no picture in this sample");
    return -1;
}

/* ---- colour --------------------------------------------------------------------------------------------------------------------------- */

void lm_h264dec_xrgb(const struct lm_h264dec *d, uint32_t *out, size_t stride_px)
{
    for (int y = 0; y < d->h; y++) {
        const uint8_t *yr = d->Y + (size_t)y * (size_t)d->ys;
        int cy = y >> 1;
        const uint8_t *ur = d->U + (size_t)cy * (size_t)d->cs, *vr = d->V + (size_t)cy * (size_t)d->cs;
        /* chroma between two rows: weigh the nearer one 3:1 (the chroma sample sits between the two luma rows) */
        int oy = (y & 1) ? 1 : -1;
        int cy2 = cy + oy;
        if (cy2 < 0)
            cy2 = 0;
        if (cy2 >= d->mbh * 8)
            cy2 = d->mbh * 8 - 1;
        const uint8_t *u2 = d->U + (size_t)cy2 * (size_t)d->cs, *v2 = d->V + (size_t)cy2 * (size_t)d->cs;
        uint32_t *o = out + (size_t)y * stride_px;
        for (int x = 0; x < d->w; x++) {
            int cx = x >> 1, cx2 = cx + ((x & 1) ? 1 : -1);
            if (cx2 < 0)
                cx2 = 0;
            if (cx2 >= d->mbw * 8)
                cx2 = d->mbw * 8 - 1;
            int u = (9 * ur[cx] + 3 * ur[cx2] + 3 * u2[cx] + u2[cx2] + 8) >> 4;
            int v = (9 * vr[cx] + 3 * vr[cx2] + 3 * v2[cx] + v2[cx2] + 8) >> 4;
            int c = 298 * (yr[x] - 16), du = u - 128, dv = v - 128;
            int r = (c + 459 * dv + 128) >> 8, g = (c - 55 * du - 136 * dv + 128) >> 8, b = (c + 541 * du + 128) >> 8;
            o[x] = ((uint32_t)lm_h264_clip(r) << 16) | ((uint32_t)lm_h264_clip(g) << 8) | (uint32_t)lm_h264_clip(b);
        }
    }
}
