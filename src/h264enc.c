#include "h264enc.h"

#include <stdlib.h>
#include <string.h>

#include "h264.h"

struct lm_h264enc {
    int w, h;                       /* coded size (even) */
    int mbw, mbh;
    int qp, qpc;
    int ys, cs;                     /* strides of the reconstruction planes (padded to whole macroblocks) */
    uint8_t *Y, *U, *V;             /* reconstruction: what a decoder shows; updated in place, macroblock by macroblock */
    uint8_t *prev;                  /* the previous source picture (XRGB, w * h * 4), NULL before the first one */
    uint8_t *nzy;                   /* per macroblock: total_coeff of its 16 luma blocks (by block index) */
    uint8_t *nzc;                   /* ... and of its 2 x 4 chroma blocks */
    uint32_t frame_num;
    int idr_id;
    struct lm_bw bw;
};

static const uint8_t blk_idx[4][4] = { { 0, 1, 4, 5 }, { 2, 3, 6, 7 }, { 8, 9, 12, 13 }, { 10, 11, 14, 15 } };

/* what is coded for one macroblock */
struct mbc {
    int type;                       /* 0 P_Skip, 1 P_L0_16x16, 2 Intra16x16 (DC) */
    int cbp_l, cbp_c;               /* luma: 4 bits (one per 8x8) for inter, 0 or 15 for intra; chroma 0..2 */
    int ydc[16];                    /* Intra16x16 DC levels, scan order */
    int yl[16][16];                 /* luma levels per block, scan order (intra: the 15 AC levels at [0..14]) */
    int cdc[2][4];
    int cac[2][4][16];              /* chroma AC levels at [0..14] */
    int tcy[16], tcc[2][4];         /* total_coeff per block as coded (for the nC of the neighbours) */
};

/* ---- creation ---------------------------------------------------------------------------------------------------------------- */

struct lm_h264enc *lm_h264enc_create(int width, int height, int qp)
{
    width &= ~1;
    height &= ~1;
    if (width < 16 || height < 16 || width > 8192 || height > 8192)
        return NULL;
    struct lm_h264enc *e = calloc(1, sizeof(*e));
    if (!e)
        return NULL;
    e->w = width;
    e->h = height;
    e->mbw = (width + 15) / 16;
    e->mbh = (height + 15) / 16;
    e->qp = qp < 0 ? 0 : qp > 51 ? 51 : qp;
    e->qpc = lm_h264_chroma_qp(e->qp);
    e->ys = e->mbw * 16;
    e->cs = e->mbw * 8;
    e->Y = calloc((size_t)e->ys * (size_t)e->mbh * 16, 1);
    e->U = calloc((size_t)e->cs * (size_t)e->mbh * 8, 1);
    e->V = calloc((size_t)e->cs * (size_t)e->mbh * 8, 1);
    e->nzy = calloc((size_t)e->mbw * (size_t)e->mbh, 16);
    e->nzc = calloc((size_t)e->mbw * (size_t)e->mbh, 8);
    if (!e->Y || !e->U || !e->V || !e->nzy || !e->nzc) {
        lm_h264enc_free(e);
        return NULL;
    }
    return e;
}

void lm_h264enc_free(struct lm_h264enc *e)
{
    if (!e)
        return;
    free(e->Y);
    free(e->U);
    free(e->V);
    free(e->prev);
    free(e->nzy);
    free(e->nzc);
    lm_bw_free(&e->bw);
    free(e);
}

int lm_h264enc_width(const struct lm_h264enc *e) { return e->w; }
int lm_h264enc_height(const struct lm_h264enc *e) { return e->h; }

const uint8_t *lm_h264enc_recon(const struct lm_h264enc *e, int plane, int *stride)
{
    *stride = plane ? e->cs : e->ys;
    return plane == 0 ? e->Y : plane == 1 ? e->U : e->V;
}

/* ---- parameter sets ------------------------------------------------------------------------------------------------------------ */

bool lm_h264enc_headers(const struct lm_h264enc *e, uint8_t **sps, size_t *sps_len, uint8_t **pps, size_t *pps_len)
{
    struct lm_bw b = { 0 };
    lm_bw_put(&b, 66, 8);                           /* profile_idc: baseline ... */
    lm_bw_put(&b, 0xc0, 8);                         /* ... constraint_set0 and 1 flags: constrained baseline */
    lm_bw_put(&b, 51, 8);                           /* level 5.1 */
    lm_bw_ue(&b, 0);                                /* seq_parameter_set_id */
    lm_bw_ue(&b, 4);                                /* log2_max_frame_num_minus4: frame_num has 8 bits */
    lm_bw_ue(&b, 2);                                /* pic_order_cnt_type 2: output order = decoding order */
    lm_bw_ue(&b, 1);                                /* max_num_ref_frames */
    lm_bw_put(&b, 0, 1);                            /* gaps_in_frame_num_value_allowed_flag */
    lm_bw_ue(&b, (uint32_t)e->mbw - 1);
    lm_bw_ue(&b, (uint32_t)e->mbh - 1);
    lm_bw_put(&b, 1, 1);                            /* frame_mbs_only_flag */
    lm_bw_put(&b, 1, 1);                            /* direct_8x8_inference_flag */
    int cr = (e->mbw * 16 - e->w) / 2, cb = (e->mbh * 16 - e->h) / 2;
    if (cr || cb) {                                 /* frame cropping, in units of two pixels */
        lm_bw_put(&b, 1, 1);
        lm_bw_ue(&b, 0);
        lm_bw_ue(&b, (uint32_t)cr);
        lm_bw_ue(&b, 0);
        lm_bw_ue(&b, (uint32_t)cb);
    } else {
        lm_bw_put(&b, 0, 1);
    }
    lm_bw_put(&b, 1, 1);                            /* vui_parameters_present_flag */
    lm_bw_put(&b, 0, 1);                            /* aspect_ratio_info_present_flag */
    lm_bw_put(&b, 0, 1);                            /* overscan_info_present_flag */
    lm_bw_put(&b, 1, 1);                            /* video_signal_type_present_flag */
    lm_bw_put(&b, 5, 3);                            /* video_format: unspecified */
    lm_bw_put(&b, 0, 1);                            /* video_full_range_flag: limited range */
    lm_bw_put(&b, 1, 1);                            /* colour_description_present_flag */
    lm_bw_put(&b, 1, 8);                            /* colour_primaries: BT.709 */
    lm_bw_put(&b, 1, 8);                            /* transfer_characteristics: BT.709 */
    lm_bw_put(&b, 1, 8);                            /* matrix_coefficients: BT.709 */
    lm_bw_put(&b, 0, 1);                            /* chroma_loc_info_present_flag */
    lm_bw_put(&b, 0, 1);                            /* timing_info_present_flag */
    lm_bw_put(&b, 0, 1);                            /* nal_hrd_parameters_present_flag */
    lm_bw_put(&b, 0, 1);                            /* vcl_hrd_parameters_present_flag */
    lm_bw_put(&b, 0, 1);                            /* pic_struct_present_flag */
    lm_bw_put(&b, 0, 1);                            /* bitstream_restriction_flag */
    lm_bw_trailing(&b);
    *sps = lm_h264_nal(3, 7, b.p, b.n, sps_len);
    lm_bw_free(&b);

    struct lm_bw p = { 0 };
    lm_bw_ue(&p, 0);                                /* pic_parameter_set_id */
    lm_bw_ue(&p, 0);                                /* seq_parameter_set_id */
    lm_bw_put(&p, 0, 1);                            /* entropy_coding_mode_flag: CAVLC */
    lm_bw_put(&p, 0, 1);                            /* bottom_field_pic_order_in_frame_present_flag */
    lm_bw_ue(&p, 0);                                /* num_slice_groups_minus1 */
    lm_bw_ue(&p, 0);                                /* num_ref_idx_l0_default_active_minus1 */
    lm_bw_ue(&p, 0);                                /* num_ref_idx_l1_default_active_minus1 */
    lm_bw_put(&p, 0, 1);                            /* weighted_pred_flag */
    lm_bw_put(&p, 0, 2);                            /* weighted_bipred_idc */
    lm_bw_se(&p, e->qp - 26);                       /* pic_init_qp_minus26 */
    lm_bw_se(&p, 0);                                /* pic_init_qs_minus26 */
    lm_bw_se(&p, 0);                                /* chroma_qp_index_offset */
    lm_bw_put(&p, 1, 1);                            /* deblocking_filter_control_present_flag */
    lm_bw_put(&p, 0, 1);                            /* constrained_intra_pred_flag */
    lm_bw_put(&p, 0, 1);                            /* redundant_pic_cnt_present_flag */
    lm_bw_trailing(&p);
    *pps = lm_h264_nal(3, 8, p.p, p.n, pps_len);
    lm_bw_free(&p);
    return *sps && *pps;
}

/* ---- colour conversion (BT.709, limited range) --------------------------------------------------------------------------------- */

static void fetch_mb(const struct lm_h264enc *e, const uint32_t *rgb, size_t stride_px, int mbx, int mby, int *ys, int *us, int *vs)
{
    for (int y = 0; y < 16; y++) {
        int sy = mby * 16 + y;
        if (sy >= e->h)
            sy = e->h - 1;
        const uint32_t *row = rgb + (size_t)sy * stride_px;
        for (int x = 0; x < 16; x++) {
            int sx = mbx * 16 + x;
            if (sx >= e->w)
                sx = e->w - 1;
            uint32_t p = row[sx];
            int r = (int)((p >> 16) & 255), g = (int)((p >> 8) & 255), b = (int)(p & 255);
            ys[y * 16 + x] = ((47 * r + 157 * g + 16 * b + 128) >> 8) + 16;
        }
    }
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            int r = 0, g = 0, b = 0;
            for (int k = 0; k < 4; k++) {
                int sy = mby * 16 + y * 2 + (k >> 1), sx = mbx * 16 + x * 2 + (k & 1);
                if (sy >= e->h)
                    sy = e->h - 1;
                if (sx >= e->w)
                    sx = e->w - 1;
                uint32_t p = rgb[(size_t)sy * stride_px + (size_t)sx];
                r += (int)((p >> 16) & 255);
                g += (int)((p >> 8) & 255);
                b += (int)(p & 255);
            }
            r = (r + 2) >> 2;
            g = (g + 2) >> 2;
            b = (b + 2) >> 2;
            us[y * 8 + x] = lm_h264_clip(((-26 * r - 87 * g + 112 * b + 128) >> 8) + 128);
            vs[y * 8 + x] = lm_h264_clip(((112 * r - 102 * g - 10 * b + 128) >> 8) + 128);
        }
}

static bool mb_unchanged(const struct lm_h264enc *e, const uint32_t *rgb, size_t stride_bytes, int mbx, int mby)
{
    int x0 = mbx * 16, y0 = mby * 16, nx = e->w - x0 < 16 ? e->w - x0 : 16, ny = e->h - y0 < 16 ? e->h - y0 : 16;
    for (int y = 0; y < ny; y++) {
        const uint8_t *a = (const uint8_t *)rgb + (size_t)(y0 + y) * stride_bytes + (size_t)x0 * 4;
        const uint8_t *b = e->prev + ((size_t)(y0 + y) * (size_t)e->w + (size_t)x0) * 4;
        if (memcmp(a, b, (size_t)nx * 4) != 0)
            return false;
    }
    return true;
}

static void remember_mb(struct lm_h264enc *e, const uint32_t *rgb, size_t stride_bytes, int mbx, int mby)
{
    int x0 = mbx * 16, y0 = mby * 16, nx = e->w - x0 < 16 ? e->w - x0 : 16, ny = e->h - y0 < 16 ? e->h - y0 : 16;
    for (int y = 0; y < ny; y++)
        memcpy(e->prev + ((size_t)(y0 + y) * (size_t)e->w + (size_t)x0) * 4, (const uint8_t *)rgb + (size_t)(y0 + y) * stride_bytes + (size_t)x0 * 4,
               (size_t)nx * 4);
}

/* ---- prediction and reconstruction ------------------------------------------------------------------------------------------ */

static int luma_dc_pred(const struct lm_h264enc *e, int mbx, int mby)
{
    return lm_h264_luma_dc_pred(e->Y, e->ys, mbx, mby);
}

static void fill_dc_pred(struct lm_h264enc *e, int mbx, int mby)
{
    int dc = luma_dc_pred(e, mbx, mby);
    uint8_t *p = e->Y + (size_t)mby * 16 * (size_t)e->ys + (size_t)mbx * 16;
    for (int y = 0; y < 16; y++)
        memset(p + (size_t)y * (size_t)e->ys, dc, 16);
    for (int c = 0; c < 2; c++) {
        uint8_t *pl = c ? e->V : e->U;
        uint8_t *q = pl + (size_t)mby * 8 * (size_t)e->cs + (size_t)mbx * 8;
        int v[4];
        for (int b = 0; b < 4; b++)
            v[b] = lm_h264_chroma_dc_pred(pl, e->cs, mbx, mby, b & 1, b >> 1);
        for (int b = 0; b < 4; b++)
            for (int y = 0; y < 4; y++)
                memset(q + (size_t)((b >> 1) * 4 + y) * (size_t)e->cs + (b & 1) * 4, v[b], 4);
    }
}

static void add_block(uint8_t *dst, int stride, const int *r)
{
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            dst[y * stride + x] = (uint8_t)lm_h264_clip(dst[y * stride + x] + r[y * 4 + x]);
}

/* Adds the decoded residual of the macroblock to the prediction that sits in the reconstruction planes. */
static void reconstruct(struct lm_h264enc *e, int mbx, int mby, const struct mbc *m)
{
    int lv[16], r[16], dc16[16], dc4[4];
    uint8_t *yp = e->Y + (size_t)mby * 16 * (size_t)e->ys + (size_t)mbx * 16;
    if (m->type == 2) {
        for (int k = 0; k < 16; k++)
            lv[lm_h264_zigzag4[k]] = m->ydc[k];
        lm_h264_dequant_luma_dc(lv, e->qp, dc16);
    }
    for (int b = 0; b < 16; b++) {
        int bx = lm_h264_blk_x[b], by = lm_h264_blk_y[b];
        memset(lv, 0, sizeof(lv));
        int any = 0;
        if (m->type == 2) {
            for (int k = 1; k < 16; k++) {
                lv[lm_h264_zigzag4[k]] = m->yl[b][k - 1];
                any |= m->yl[b][k - 1];
            }
            int dc = dc16[by * 4 + bx];
            if (!any && !dc)
                continue;
            lm_h264_dequant_idct4(lv, e->qp, &dc, r);
        } else {
            for (int k = 0; k < 16; k++) {
                lv[lm_h264_zigzag4[k]] = m->yl[b][k];
                any |= m->yl[b][k];
            }
            if (!any)
                continue;
            lm_h264_dequant_idct4(lv, e->qp, NULL, r);
        }
        add_block(yp + (size_t)by * 4 * (size_t)e->ys + (size_t)bx * 4, e->ys, r);
    }
    for (int c = 0; c < 2; c++) {
        if (!m->cbp_c)
            continue;
        uint8_t *cp = (c ? e->V : e->U) + (size_t)mby * 8 * (size_t)e->cs + (size_t)mbx * 8;
        lm_h264_dequant_chroma_dc(m->cdc[c], e->qpc, dc4);
        for (int b = 0; b < 4; b++) {
            memset(lv, 0, sizeof(lv));
            for (int k = 1; k < 16; k++)
                lv[lm_h264_zigzag4[k]] = m->cac[c][b][k - 1];
            lm_h264_dequant_idct4(lv, e->qpc, &dc4[b], r);
            add_block(cp + (size_t)(b >> 1) * 4 * (size_t)e->cs + (size_t)(b & 1) * 4, e->cs, r);
        }
    }
}

/* ---- analysis: from the source macroblock to levels ----------------------------------------------------------------------------- */

static int count_nz(const int *a, int n)
{
    int c = 0;
    for (int i = 0; i < n; i++)
        c += a[i] != 0;
    return c;
}

/* Levels for the macroblock against the prediction in the planes. `intra`: Intra16x16 (DC and AC apart), else inter (16 coefficients a block). */
static void analyse(const struct lm_h264enc *e, int mbx, int mby, const int *ys, const int *us, const int *vs, int intra, struct mbc *m)
{
    int res[16], w[16], lv[16], dc16[16], dclv[16];
    const uint8_t *yp = e->Y + (size_t)mby * 16 * (size_t)e->ys + (size_t)mbx * 16;
    memset(m, 0, sizeof(*m));
    m->type = intra ? 2 : 1;
    for (int b = 0; b < 16; b++) {
        int bx = lm_h264_blk_x[b] * 4, by = lm_h264_blk_y[b] * 4;
        for (int i = 0; i < 16; i++)
            res[i] = ys[(by + (i >> 2)) * 16 + bx + (i & 3)] - yp[(size_t)(by + (i >> 2)) * (size_t)e->ys + (size_t)(bx + (i & 3))];
        lm_h264_fdct4(res, w);
        if (intra) {
            dc16[(by >> 2) * 4 + (bx >> 2)] = w[0];
            lm_h264_quant4(w, lv, e->qp, 1, 1);
            for (int k = 1; k < 16; k++)
                m->yl[b][k - 1] = lv[lm_h264_zigzag4[k]];
            m->tcy[b] = count_nz(m->yl[b], 15);
            if (m->tcy[b])
                m->cbp_l = 15;
        } else {
            lm_h264_quant4(w, lv, e->qp, 0, 0);
            for (int k = 0; k < 16; k++)
                m->yl[b][k] = lv[lm_h264_zigzag4[k]];
            m->tcy[b] = count_nz(m->yl[b], 16);
            if (m->tcy[b])
                m->cbp_l |= 1 << (b >> 2);
        }
    }
    if (intra) {
        lm_h264_quant_luma_dc(dc16, dclv, e->qp);
        for (int k = 0; k < 16; k++)
            m->ydc[k] = dclv[lm_h264_zigzag4[k]];
        if (m->cbp_l)                                   /* the AC blocks are all sent when one has a coefficient */
            m->cbp_l = 15;
        else
            for (int b = 0; b < 16; b++)
                m->tcy[b] = 0;
    } else {
        for (int b = 0; b < 16; b++)                    /* blocks of an 8x8 without any coefficient are not sent */
            if (!(m->cbp_l & (1 << (b >> 2))))
                m->tcy[b] = 0;
    }
    int any_ac = 0, any_dc = 0;
    for (int c = 0; c < 2; c++) {
        const int *src = c ? vs : us;
        const uint8_t *cp = (c ? e->V : e->U) + (size_t)mby * 8 * (size_t)e->cs + (size_t)mbx * 8;
        int dc4[4], dcl[4];
        for (int b = 0; b < 4; b++) {
            int bx = (b & 1) * 4, by = (b >> 1) * 4;
            for (int i = 0; i < 16; i++)
                res[i] = src[(by + (i >> 2)) * 8 + bx + (i & 3)] - cp[(size_t)(by + (i >> 2)) * (size_t)e->cs + (size_t)(bx + (i & 3))];
            lm_h264_fdct4(res, w);
            dc4[b] = w[0];
            lm_h264_quant4(w, lv, e->qpc, intra, 1);
            for (int k = 1; k < 16; k++)
                m->cac[c][b][k - 1] = lv[lm_h264_zigzag4[k]];
            m->tcc[c][b] = count_nz(m->cac[c][b], 15);
            any_ac |= m->tcc[c][b] != 0;
        }
        lm_h264_quant_chroma_dc(dc4, dcl, e->qpc);
        for (int k = 0; k < 4; k++) {
            m->cdc[c][k] = dcl[k];
            any_dc |= dcl[k] != 0;
        }
    }
    m->cbp_c = any_ac ? 2 : any_dc ? 1 : 0;
    if (m->cbp_c < 2)
        memset(m->tcc, 0, sizeof(m->tcc));
}

/* ---- CAVLC ------------------------------------------------------------------------------------------------------------------------ */

static int nc_luma(const struct lm_h264enc *e, int mbx, int mby, int blk)
{
    int bx = lm_h264_blk_x[blk], by = lm_h264_blk_y[blk];
    size_t mb = (size_t)mby * (size_t)e->mbw + (size_t)mbx;
    int na = -1, nb = -1;
    if (bx > 0)
        na = e->nzy[mb * 16 + blk_idx[by][bx - 1]];
    else if (mbx > 0)
        na = e->nzy[(mb - 1) * 16 + blk_idx[by][3]];
    if (by > 0)
        nb = e->nzy[mb * 16 + blk_idx[by - 1][bx]];
    else if (mby > 0)
        nb = e->nzy[(mb - (size_t)e->mbw) * 16 + blk_idx[3][bx]];
    return na >= 0 && nb >= 0 ? (na + nb + 1) >> 1 : na >= 0 ? na : nb >= 0 ? nb : 0;
}

static int nc_chroma(const struct lm_h264enc *e, int mbx, int mby, int c, int blk)
{
    int bx = blk & 1, by = blk >> 1;
    size_t mb = (size_t)mby * (size_t)e->mbw + (size_t)mbx;
    int na = -1, nb = -1;
    if (bx > 0)
        na = e->nzc[mb * 8 + (size_t)c * 4 + (size_t)(blk - 1)];
    else if (mbx > 0)
        na = e->nzc[(mb - 1) * 8 + (size_t)c * 4 + (size_t)(by * 2 + 1)];
    if (by > 0)
        nb = e->nzc[mb * 8 + (size_t)c * 4 + (size_t)(blk - 2)];
    else if (mby > 0)
        nb = e->nzc[(mb - (size_t)e->mbw) * 8 + (size_t)c * 4 + (size_t)(2 + bx)];
    return na >= 0 && nb >= 0 ? (na + nb + 1) >> 1 : na >= 0 ? na : nb >= 0 ? nb : 0;
}

/* One residual block: coef[0..maxn-1] in scan order; nC < 0: chroma DC. Returns total_coeff. */
static int put_block(struct lm_bw *bw, const int *coef, int maxn, int nc)
{
    int pos[16], lev[16], total = 0;
    for (int i = maxn - 1; i >= 0; i--)                 /* highest frequency first */
        if (coef[i]) {
            pos[total] = i;
            lev[total] = coef[i];
            total++;
        }
    int t1 = 0;
    while (t1 < 3 && t1 < total && (lev[t1] == 1 || lev[t1] == -1))
        t1++;
    if (nc < 0) {
        lm_bw_put(bw, lm_h264_cdc_token_bits[4 * total + t1], lm_h264_cdc_token_len[4 * total + t1]);
    } else {
        int tab = nc < 2 ? 0 : nc < 4 ? 1 : nc < 8 ? 2 : 3;
        lm_bw_put(bw, lm_h264_coeff_token_bits[tab][4 * total + t1], lm_h264_coeff_token_len[tab][4 * total + t1]);
    }
    if (!total)
        return 0;
    for (int i = 0; i < t1; i++)
        lm_bw_put(bw, lev[i] < 0, 1);
    int sl = total > 10 && t1 < 3 ? 1 : 0;
    for (int i = t1; i < total; i++) {
        int level = lev[i];
        int code = level > 0 ? 2 * level - 2 : -2 * level - 1;
        if (i == t1 && t1 < 3)
            code -= 2;
        if (sl == 0) {
            if (code < 14) {
                lm_bw_put(bw, 0, code);
                lm_bw_put(bw, 1, 1);
            } else if (code < 30) {
                lm_bw_put(bw, 0, 14);
                lm_bw_put(bw, 1, 1);
                lm_bw_put(bw, (uint32_t)(code - 14), 4);
            } else {
                lm_bw_put(bw, 0, 15);
                lm_bw_put(bw, 1, 1);
                lm_bw_put(bw, (uint32_t)(code - 30), 12);
            }
        } else if (code < (15 << sl)) {
            lm_bw_put(bw, 0, code >> sl);
            lm_bw_put(bw, 1, 1);
            lm_bw_put(bw, (uint32_t)(code & ((1 << sl) - 1)), sl);
        } else {
            lm_bw_put(bw, 0, 15);
            lm_bw_put(bw, 1, 1);
            lm_bw_put(bw, (uint32_t)(code - (15 << sl)), 12);
        }
        if (sl == 0)
            sl = 1;
        if ((level < 0 ? -level : level) > (3 << (sl - 1)) && sl < 6)
            sl++;
    }
    int tz = (pos[0] + 1) - total;                      /* zeros before the last coefficient */
    if (total < maxn) {
        if (maxn == 4)
            lm_bw_put(bw, lm_h264_cdc_zeros_bits[total - 1][tz], lm_h264_cdc_zeros_len[total - 1][tz]);
        else
            lm_bw_put(bw, lm_h264_total_zeros_bits[total - 1][tz], lm_h264_total_zeros_len[total - 1][tz]);
    }
    int zeros_left = tz;
    for (int i = 0; i < total - 1 && zeros_left > 0; i++) {
        int run = pos[i] - pos[i + 1] - 1;
        int t = zeros_left > 7 ? 6 : zeros_left - 1;
        lm_bw_put(bw, lm_h264_run_bits[t][run], lm_h264_run_len[t][run]);
        zeros_left -= run;
    }
    return total;
}

static void write_chroma(struct lm_h264enc *e, int mbx, int mby, const struct mbc *m)
{
    if (!m->cbp_c)
        return;
    for (int c = 0; c < 2; c++)
        put_block(&e->bw, m->cdc[c], 4, -1);
    if (m->cbp_c == 2) {
        size_t mb = (size_t)mby * (size_t)e->mbw + (size_t)mbx;
        for (int c = 0; c < 2; c++)
            for (int b = 0; b < 4; b++) {
                int tc = put_block(&e->bw, m->cac[c][b], 15, nc_chroma(e, mbx, mby, c, b));
                e->nzc[mb * 8 + (size_t)c * 4 + (size_t)b] = (uint8_t)tc;
            }
    }
}

static void write_mb(struct lm_h264enc *e, int mbx, int mby, const struct mbc *m, int p_slice)
{
    size_t mb = (size_t)mby * (size_t)e->mbw + (size_t)mbx;
    struct lm_bw *bw = &e->bw;
    memset(e->nzy + mb * 16, 0, 16);
    memset(e->nzc + mb * 8, 0, 8);
    if (m->type == 2) {
        int type = 1 + 2 + 4 * m->cbp_c + (m->cbp_l ? 12 : 0);
        lm_bw_ue(bw, (uint32_t)(type + (p_slice ? 5 : 0)));
        lm_bw_ue(bw, 0);                                /* intra_chroma_pred_mode: DC */
        lm_bw_se(bw, 0);                                /* mb_qp_delta */
        put_block(bw, m->ydc, 16, nc_luma(e, mbx, mby, 0));
        if (m->cbp_l)
            for (int b = 0; b < 16; b++) {
                int tc = put_block(bw, m->yl[b], 15, nc_luma(e, mbx, mby, b));
                e->nzy[mb * 16 + (size_t)b] = (uint8_t)tc;
            }
        write_chroma(e, mbx, mby, m);
    } else {
        int cbp = m->cbp_l | (m->cbp_c << 4), code = 0;
        while (code < 48 && lm_h264_inter_cbp[code] != cbp)
            code++;
        lm_bw_ue(bw, 0);                                /* mb_type: P_L0_16x16 */
        lm_bw_se(bw, 0);                                /* mvd_l0 x */
        lm_bw_se(bw, 0);                                /* mvd_l0 y */
        lm_bw_ue(bw, (uint32_t)code);                   /* coded_block_pattern */
        lm_bw_se(bw, 0);                                /* mb_qp_delta (the cbp is not 0) */
        for (int b = 0; b < 16; b++) {
            if (!(m->cbp_l & (1 << (b >> 2))))
                continue;
            int tc = put_block(bw, m->yl[b], 16, nc_luma(e, mbx, mby, b));
            e->nzy[mb * 16 + (size_t)b] = (uint8_t)tc;
        }
        write_chroma(e, mbx, mby, m);
    }
}

/* ---- pictures ------------------------------------------------------------------------------------------------------------------------ */

static long sad_to_plane(const int *src, const uint8_t *plane, int stride, int n, long limit)
{
    long s = 0;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            int d = src[y * n + x] - plane[(size_t)y * (size_t)stride + (size_t)x];
            s += d < 0 ? -d : d;
        }
    (void)limit;
    return s;
}

uint8_t *lm_h264enc_encode(struct lm_h264enc *e, const uint32_t *xrgb, size_t stride, bool idr, size_t *len, bool *key)
{
    bool p_slice = !idr && e->prev != NULL;
    if (!p_slice)
        idr = true;
    if (!e->prev) {
        e->prev = malloc((size_t)e->w * (size_t)e->h * 4);
        if (!e->prev)
            return NULL;
    }
    size_t stride_px = stride / 4;
    struct lm_bw *bw = &e->bw;
    bw->n = 0;
    bw->acc = 0;
    bw->nacc = 0;
    bw->oom = 0;
    /* slice header */
    lm_bw_ue(bw, 0);                                    /* first_mb_in_slice */
    lm_bw_ue(bw, p_slice ? 0 : 2);                      /* slice_type: P / I */
    lm_bw_ue(bw, 0);                                    /* pic_parameter_set_id */
    if (idr)
        e->frame_num = 0;
    lm_bw_put(bw, e->frame_num, 8);                     /* frame_num */
    if (idr) {
        lm_bw_ue(bw, (uint32_t)(e->idr_id & 1));        /* idr_pic_id */
        e->idr_id++;
    }
    if (p_slice) {
        lm_bw_put(bw, 0, 1);                            /* num_ref_idx_active_override_flag */
        lm_bw_put(bw, 0, 1);                            /* ref_pic_list_modification_flag_l0 */
    }
    if (idr) {
        lm_bw_put(bw, 0, 1);                            /* no_output_of_prior_pics_flag */
        lm_bw_put(bw, 0, 1);                            /* long_term_reference_flag */
    } else {
        lm_bw_put(bw, 0, 1);                            /* adaptive_ref_pic_marking_mode_flag */
    }
    lm_bw_se(bw, 0);                                    /* slice_qp_delta */
    lm_bw_ue(bw, 1);                                    /* disable_deblocking_filter_idc: off */

    int ys[256], us[64], vs[64];
    int skip_run = 0;
    struct mbc m;
    for (int mby = 0; mby < e->mbh; mby++)
        for (int mbx = 0; mbx < e->mbw; mbx++) {
            size_t mb = (size_t)mby * (size_t)e->mbw + (size_t)mbx;
            if (p_slice && mb_unchanged(e, xrgb, stride, mbx, mby)) {
                skip_run++;
                memset(e->nzy + mb * 16, 0, 16);
                memset(e->nzc + mb * 8, 0, 8);
                continue;
            }
            fetch_mb(e, xrgb, stride_px, mbx, mby, ys, us, vs);
            int intra = !p_slice;
            if (p_slice) {                              /* inter against the previous picture, unless intra is clearly closer */
                const uint8_t *yp = e->Y + (size_t)mby * 16 * (size_t)e->ys + (size_t)mbx * 16;
                long inter = sad_to_plane(ys, yp, e->ys, 16, 0);
                int dc = luma_dc_pred(e, mbx, mby);
                long dcs = 0;
                for (int i = 0; i < 256; i++)
                    dcs += ys[i] > dc ? ys[i] - dc : dc - ys[i];
                intra = dcs * 5 < inter * 3;            /* clearly less to code: new flat content, e.g. a window that appeared */
            }
            if (intra)
                fill_dc_pred(e, mbx, mby);              /* (the planes now hold the prediction) */
            analyse(e, mbx, mby, ys, us, vs, intra, &m);
            if (!intra && m.cbp_l == 0 && m.cbp_c == 0) {   /* nothing to send: P_Skip */
                skip_run++;
                memset(e->nzy + mb * 16, 0, 16);
                memset(e->nzc + mb * 8, 0, 8);
                remember_mb(e, xrgb, stride, mbx, mby);
                continue;
            }
            if (p_slice) {
                lm_bw_ue(bw, (uint32_t)skip_run);
                skip_run = 0;
            }
            write_mb(e, mbx, mby, &m, p_slice);
            reconstruct(e, mbx, mby, &m);
            remember_mb(e, xrgb, stride, mbx, mby);
        }
    if (p_slice && skip_run)
        lm_bw_ue(bw, (uint32_t)skip_run);
    lm_bw_trailing(bw);
    if (bw->oom)
        return NULL;
    size_t nal_len = 0;
    uint8_t *nal = lm_h264_nal(3, idr ? 5 : 1, bw->p, bw->n, &nal_len);
    if (!nal)
        return NULL;
    uint8_t *out = malloc(4 + nal_len);
    if (!out) {
        free(nal);
        return NULL;
    }
    out[0] = (uint8_t)(nal_len >> 24);
    out[1] = (uint8_t)(nal_len >> 16);
    out[2] = (uint8_t)(nal_len >> 8);
    out[3] = (uint8_t)nal_len;
    memcpy(out + 4, nal, nal_len);
    free(nal);
    *len = 4 + nal_len;
    *key = idr;
    e->frame_num = (e->frame_num + 1) & 255;
    return out;
}
